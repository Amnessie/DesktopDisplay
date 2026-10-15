#include <Arduino.h>
#include "ArduinoJson.h"
#include <TimeLib.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_system.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <TFT_eSPI.h> 
#include <SPI.h>
#include <TJpg_Decoder.h>
#include <FS.h>           
#include <WiFiManager.h>  
#include <Preferences.h>  
#include <WebServer.h>    
#include <SD.h>
#include <vector>
#include <algorithm>

// --- [OPTIONAL SD CARD - HISTORY LOGGING] ---
// Set to 0 to fully disable SD/history support if you don't have a card.
// Pins confirmed from the official E32R35T/E32N35T manual (section 4.2):
// SD card CS=IO5, SCK=IO18, MISO=IO19, MOSI=IO23. Confirmed via the user's
// actual TFT_eSPI/User_Setup.h that TFT/touch use the VSPI peripheral
// (USE_HSPI_PORT is not defined there, so VSPI is the default) - the SD
// card is put on HSPI instead (see sdSPI below), a genuinely separate
// hardware peripheral, so initializing it can't disturb the display/touch's
// SPI routing the way sharing VSPI did (that was the actual cause of an
// earlier freeze: the screen kept updating fine, but touch input stopped
// working entirely, since drawing doesn't depend on touch but every tap
// does depend on the touch controller's own SPI bus staying intact).
#define USE_SD_HISTORY 1
#define SD_CS_PIN 5
#define SD_SCK_PIN 18
#define SD_MISO_PIN 19
#define SD_MOSI_PIN 23
// CONFIRMED via the user's actual TFT_eSPI/User_Setup.h: USE_HSPI_PORT is NOT
// defined there, so TFT_eSPI (and the touch controller, which shares its SPI
// class) defaults to VSPI. The SD card MUST use HSPI instead - using VSPI
// here (as an earlier version of this code did) silently reconfigures the
// same hardware peripheral the touch controller depends on, breaking touch
// input entirely (screen still updates, since drawing doesn't depend on
// touch, but no tap/gesture is ever detected again).
SPIClass sdSPI(HSPI); // dedicated bus for the SD card only - genuinely separate hardware peripheral from the display/touch's VSPI
bool sdOK = false;
void handleHistoryPage(); // defined at the very end of the sketch (see [HISTORY WEB PAGE])
// Web UI assets (HTML/CSS/JS), defined at the very end of the sketch (see [WEB UI ASSETS])
extern const char APP_CSS[], APP_JS[], DASHBOARD_BODY[], SETTINGS_BODY[], DEBUG_BODY[];
void sendPage(const char *title, const char *current, bool cityTabs, const char *body, const char *body2 = nullptr);
String pageStart(const char *title, const char *current, bool cityTabs);
String pageEnd(const char *current);
// NOTE: history is now stored per city (/hist_c0.csv, /hist_c1.csv,
// /hist_c2.csv) - see the [PER-CITY HISTORY] section further down.

// --- [OPTIONAL LOCAL SENSOR] ---
// Set to 0 to fully disable BME280 support (no I2C pins used at all).
// If enabled, VERIFY the pins below are actually free on your wiring —
// GPIO22 is already used by LED_R on this board, so it can't be the I2C SCL pin.
#define USE_BME280 1
#if USE_BME280
  #include <Wire.h>
  #include <Adafruit_Sensor.h>
  #include <Adafruit_BME280.h>
  #define BME_SDA_PIN 21   // CHECK: change if this conflicts with your wiring
  #define BME_SCL_PIN 25   // CHECK: default SCL(22) conflicts with LED_R, using 25 instead - verify free
  Adafruit_BME280 bme;
  bool bmeOK = false;
  float localTemp = 0, localHumid = 0, localPressure = 0;
  unsigned long lastBmeRead = 0;
#endif

// --- [HARDWARE PIN MAPPING] ---
#define LCD_BL_PIN 27    
#define TP_IRQ 36        
#define TP_CS 33         
#define LED_R 22         
#define LED_G 16         
#define LED_B 17         

// --- [DIAGNOSTIC] ---
// Set to 1 to temporarily skip touch reading entirely (disables all touch
// input). This is purely a test: if the clock/status indicator resume
// updating normally with this set to 1, tft.getTouch() (i.e. the touch
// controller's wiring - TP_CS/TP_IRQ) is what was hanging loop(), not any
// app logic. Set back to 0 once you're done testing.
#define DIAG_DISABLE_TOUCH 0

// --- [API SETTINGS] ---
// UPDATE 1: Made apiKey a String
String apiKey = "x"; 
String apiKeyBackup = ""; // NEW: optional fallback key, used only on auth/rate-limit failures
String activeApiKeySource = "primary"; // NEW: which key served the last successful request

// --- [DYNAMIC MULTI-CITY SETTINGS] ---
const int NUM_CITIES = 3;
String locs[NUM_CITIES];
long tzOffsetSec[NUM_CITIES] = {0, 0, 0}; // UTC offset per city in seconds (handles half-hour zones like India)
int currentCity = 0; 
Preferences prefs;            
WebServer server(80); 

char city1_buf[40], city2_buf[40], city3_buf[40];

// NEW: per-city geocode cache, so switching cities doesn't re-geocode every time
double cityLat[NUM_CITIES] = {0, 0, 0};
double cityLon[NUM_CITIES] = {0, 0, 0};
bool cityGeoCached[NUM_CITIES] = {false, false, false};
String cityGeoLocStr[NUM_CITIES]; // the location string each cache entry corresponds to

// NEW: per-city weather data provider. Geocoding (lat/lon lookup) always
// stays on OpenWeatherMap's geocoder above regardless of this setting -
// this only controls which provider supplies the actual weather data.
enum WeatherProvider { PROVIDER_OPENWEATHER = 0, PROVIDER_OPENMETEO_BEST = 1, PROVIDER_OPENMETEO_KNMI = 2 };
int cityProvider[NUM_CITIES] = { PROVIDER_OPENWEATHER, PROVIDER_OPENWEATHER, PROVIDER_OPENWEATHER };

// NEW: fetch status tracking (for the freshness/status indicator)
bool lastFetchOK = false;
unsigned long lastFetchMillis = 0;
bool isUpdating = false;

// NEW: runtime WiFi reconnection tracking. The earlier retry-with-backoff
// logic only ran once, at boot, in setup() - if WiFi dropped later during
// normal operation there was nothing watching for it or attempting to
// recover, so the device would just sit disconnected indefinitely.
unsigned long lastWifiCheckMillis = 0;
const unsigned long WIFI_CHECK_INTERVAL_MS = 15000; // check connection health every 15s
bool wifiWasConnected = true; // setup() already ensures we reach loop() connected (or in AP config mode)

// NEW: how often to CHECK for a change in the System Info page's WiFi/sync
// status while you're sitting on that page (the actual redraw only happens
// if something's genuinely different - see drawSystemDynamicInfo). This
// just keeps WiFi.SSID()/localIP() from being called on every single loop()
// iteration, which would otherwise churn through temporary String objects
// far more often than needed.
unsigned long lastSystemInfoRefresh = 0;
const unsigned long SYSTEM_INFO_REFRESH_MS = 2000;

// --- [WIFI & TIME VARIABLES] ---
const int NTP_PACKET_SIZE = 48;
byte packetBuffer[NTP_PACKET_SIZE];
static const char ntpServerName[] = "time.google.com";
WiFiUDP Udp;                 
unsigned int localPort = 8000;

#include "img/pangzi/i0.h"
#include "img/pangzi/i1.h"
#include "img/pangzi/i2.h"
#include "img/pangzi/i3.h"
#include "img/pangzi/i4.h"
#include "img/pangzi/i5.h"
#include "img/pangzi/i6.h"
#include "img/pangzi/i7.h"
#include "img/pangzi/i8.h"
#include "img/pangzi/i9.h"


// NEW: arrays of pointers instead of repeated switch statements (code hygiene)

const uint8_t* animFrames[10] = { i0, i1, i2, i3, i4, i5, i6, i7, i8, i9 };
const size_t   animSizes[10]  = { sizeof(i0), sizeof(i1), sizeof(i2), sizeof(i3), sizeof(i4),
                                   sizeof(i5), sizeof(i6), sizeof(i7), sizeof(i8), sizeof(i9) };

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite clk = TFT_eSprite(&tft); 

// --- [SYSTEM STATE] ---
// --- [THEMES] ---
// Every color the screen uses comes from the active theme T, so the whole UI
// can switch between light and dark. THEME_AUTO uses dark between sunset and
// sunrise for the city on screen (see applyTheme()).
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
struct Theme {
  uint16_t bg, card, card2, border, text, muted, line, accent;
  uint16_t icon, rain, rainSoft, good, bad, hi, lo, sun, moon;
  uint16_t sunriseFg, sunriseBg, sunsetFg, sunsetBg;
  uint16_t moonShadow; // the moon's unlit side (T.moon is the lit side)
};
const Theme THEME_LIGHT_COLORS = {
  RGB565(0xE9, 0xEE, 0xF5), RGB565(0xFF, 0xFF, 0xFF), RGB565(0xF3, 0xF6, 0xFA), RGB565(0xD6, 0xDE, 0xE9),
  RGB565(0x16, 0x20, 0x2F), RGB565(0x5F, 0x6F, 0x86), RGB565(0xC9, 0xD3, 0xE0), RGB565(0xF0, 0x8C, 0x2E),
  RGB565(0x9F, 0xB0, 0xC8), RGB565(0x3B, 0x82, 0xF6), RGB565(0xC3, 0xDA, 0xFA), RGB565(0x22, 0xA0, 0x6B),
  RGB565(0xDC, 0x26, 0x26), RGB565(0xCC, 0x33, 0x33), RGB565(0x1E, 0x63, 0xD6), RGB565(0xF5, 0xB4, 0x00),
  RGB565(0xDC, 0xC4, 0x84), RGB565(0xE6, 0x96, 0x1E), RGB565(0xFF, 0xF4, 0xE0), RGB565(0x1A, 0x2A, 0x5E),
  RGB565(0xE0, 0xE7, 0xF5), RGB565(0xD2, 0xDA, 0xE5)
};
const Theme THEME_DARK_COLORS = {
  RGB565(0x0B, 0x12, 0x20), RGB565(0x14, 0x1E, 0x30), RGB565(0x18, 0x23, 0x3A), RGB565(0x1F, 0x2B, 0x42),
  RGB565(0xE7, 0xED, 0xF7), RGB565(0x85, 0x96, 0xAD), RGB565(0x2B, 0x3A, 0x55), RGB565(0xFF, 0x9F, 0x43),
  RGB565(0x7F, 0x93, 0xB0), RGB565(0x4C, 0x95, 0xFF), RGB565(0x1E, 0x3A, 0x66), RGB565(0x34, 0xD3, 0x99),
  RGB565(0xF8, 0x71, 0x71), RGB565(0xF8, 0x71, 0x71), RGB565(0x60, 0xA5, 0xFA), RGB565(0xFB, 0xBF, 0x24),
  RGB565(0xE8, 0xD9, 0xA8), RGB565(0xFB, 0xBF, 0x24), RGB565(0x2A, 0x24, 0x16), RGB565(0xA5, 0xB8, 0xFF),
  RGB565(0x1A, 0x24, 0x40), RGB565(0x2B, 0x38, 0x54)
};
enum ThemeMode { THEME_LIGHT = 0, THEME_DARK = 1, THEME_AUTO = 2 };
int themeMode = THEME_AUTO; // saved as "theme"; set on the System page or the web Settings page
bool themeDark = false;
Theme T = THEME_LIGHT_COLORS;
bool accentDirty = false;          // text color changed -> redraw the page (see loop())
unsigned long accentChangedAt = 0;
uint16_t bgColor = RGB565(0xE9, 0xEE, 0xF5); // = T.bg (used by the boot/setup screens)
int globalBrightness = 255, ledBrightness = 255;
bool useCustomColor = false;
int custR = 255, custG = 255, custB = 255;

// Floats for decimals
float tempnum, tempMin, tempMax, feels_like;
int huminum, pressure, visibility, clouds;
float windSpeedKmH, rain1h, rain3h;
String weatherDesc, mainWeather, windDir, sunRise, sunSet;

int Anim = 0;
bool showAnimation = true; // NEW: toggle for the home-page animation, set via the web portal
bool showRainPreview = true; // NEW: toggle for the Home page's compact upcoming-rain preview, shares the same on-screen space as the animation (see drawPageHome)

// Rain outlook for the next 2 hours and the pressure trend, for the city on
// screen (see [RAIN NOWCAST + PRESSURE TREND]).
const int NOW_STEPS_MAX = 25;
float nowMm[NOW_STEPS_MAX];            // rain intensity per step, mm/h
String nowLabel[NOW_STEPS_MAX];        // "HH:MM", local time of the city
int nowCount = 0, nowStepMin = 5, nowCity = -1, nowFirst = 0, nowStartIdx = -1;
bool nowRadar = false, nowValid = false, nowRainingNow = false;
float nowMaxMm = 0;
String nowMsg = "", nowSource = "", nowLastError = "";
unsigned long nowFetchedAt = 0, nowNextFetch = 0, nowVersion = 0;
float pTrendRate = 0;                  // hPa per 3 hours
int pTrendLevel = 0, pTrendCity = -1;  // -3..3 (falling very fast .. rising very fast)
unsigned long pTrendFetchedAt = 0, pTrendNextFetch = 0, pTrendVersion = 0;
String pTrendLastError = "";
bool rainLedAlert = true;              // web setting: pulse the LED when rain is coming
String nowShort = "";                  // shorter version of nowMsg for the home screen card
bool nowBlend = false;                 // true = radar dry, but the model expects rain 1-3 h out
// Model forecast (Open-Meteo, 15-min steps, 3 h) used next to the radar ("blending")
const int MOD_STEPS_MAX = 12;
float modMm[MOD_STEPS_MAX];            // mm/h
String modLabel[MOD_STEPS_MAX];        // "HH:MM", local time of the city
int modCount = 0, modCity = -1;
unsigned long modFetchedAt = 0, modNextFetch = 0;
// One background network request (see [BACKGROUND NETWORK WORKER]). Declared
// up here, before any function, for the same prototype reason as SunCountdown.
struct NetJob { int kind, city, step, slot; uint32_t start; String url; int code; String body; unsigned long ms; };
unsigned long loopMaxMs = 0;           // longest main-loop pass since boot (Debug page)

// Forecast verification (see [FORECAST VERIFICATION]): per city, per source
// (0 = OpenWeatherMap, 1 = Open-Meteo, 2 = Open-Meteo KNMI - same order as
// cityProvider) the counts of hits, misses, false alarms and correct "dry".
const int VERIFY_SOURCES = 3;
uint16_t vfCnt[NUM_CITIES][VERIFY_SOURCES][4];
uint32_t vfSince[NUM_CITIES] = {0, 0, 0};
String vfLoc[NUM_CITIES];
String vfStatus[NUM_CITIES];
unsigned long AprevTime = 0, weatherUpdateTimer = 0;
time_t prevDisplay = 0, sysRise = 0, sysSet = 0;
int displayMode = 0, drawnMode = -1;

struct ForecastDay { 
  String dayName; 
  float high, low; 
  String main;
  bool valid; 
};
ForecastDay forecast[5];

// NEW: return type for computeSunCountdown() (defined later, near drawAstroCountdown) -
// declared here, early, so Arduino's auto-generated function prototypes
// (inserted near the top of the file) can see this type before they're used.
struct SunCountdown { String eventName; int diffMin; };

// NEW: rainfall-per-3-hour-slice data for the precipitation graph
const int RAIN_GRAPH_POINTS = 5; // next 5 x 3-hour forecast slices (~15 hours ahead)
float upcomingRainMm[RAIN_GRAPH_POINTS];
String upcomingRainLabel[RAIN_GRAPH_POINTS];
int upcomingRainCount = 0;

// NEW: everything a fetch overwrites, so a failed refresh can put the last good
// data back instead of leaving the screen at 0.0 C until the next attempt.
struct WeatherSnapshot {
  float tempnum, tempMin, tempMax, feels_like, windSpeedKmH, rain1h, rain3h;
  int huminum, pressure, visibility, clouds;
  String weatherDesc, mainWeather, windDir, sunRise, sunSet;
  time_t sysRise, sysSet;
  ForecastDay forecast[5];
  float upcomingRainMm[RAIN_GRAPH_POINTS];
  String upcomingRainLabel[RAIN_GRAPH_POINTS];
  int upcomingRainCount;
};
// (Declared up here, before any function, for the same prototype reason as SunCountdown.)

bool isTouched = false, touchHandled = false;
unsigned long touchStartTime = 0;
uint16_t startX, startY;

// NEW: live debug tracking, exposed via the web /debug page - lets you
// watch exactly what the touch controller is reporting in real time
// without a Serial Monitor connection (which the README already warns can
// trigger a hardware reset on this board). Updated every loop() pass.
uint16_t debugRawX = 0, debugRawY = 0;
bool debugPressed = false;
unsigned long debugStuckResetCount = 0;
const int DEBUG_LOG_SIZE = 40;
String debugLog[DEBUG_LOG_SIZE];
int debugLogIndex = 0;
int debugLogCount = 0; // how many slots are actually filled (caps at DEBUG_LOG_SIZE)

void logDebug(String msg) {
  debugLog[debugLogIndex] = "[" + String(millis() / 1000) + "s] " + msg;
  debugLogIndex = (debugLogIndex + 1) % DEBUG_LOG_SIZE;
  if (debugLogCount < DEBUG_LOG_SIZE) debugLogCount++;
}

// --- [ESCAPING HELPERS] ---
// For text from the user or an API that ends up inside a JSON string
// (quotes in a city name or an HTTP error body would otherwise break the JSON).
String jsonEsc(String s) {
  s.replace("\\", "\\\\"); s.replace("\"", "\\\"");
  s.replace("\n", "\\n"); s.replace("\r", "\\r"); s.replace("\t", "\\t");
  return s;
}

// For text placed inside an HTML attribute value or element (e.g. "Val-d'Or").
String htmlEsc(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;");
  s.replace("\"", "&quot;"); s.replace("'", "&#39;");
  return s;
}

// Percent-encodes everything except unreserved characters (RFC 3986), so city
// names containing '&', '#', accents etc. survive being put in a query string.
String urlEncode(String s) {
  const char *hex = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() * 3);
  for (unsigned int i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 0x0F]; }
  }
  return out;
}

// --- [CSRF PROTECTION] ---
// Every endpoint that changes something requires this header. Browsers won't
// let another website add a custom header to a request aimed at the device
// (it would need a CORS preflight, which the device never approves), and plain
// links, <img> tags or <form> posts can't add one at all - so a random page you
// visit can no longer change settings or wipe history behind your back.
const char* APP_HEADER_NAME  = "X-Requested-With";
const char* APP_HEADER_VALUE = "DesktopDisplay";

bool hasAppHeader() { return server.header(APP_HEADER_NAME) == APP_HEADER_VALUE; }

bool requireAppHeader() {
  if (hasAppHeader()) return true;
  server.send(403, "text/plain", "Forbidden: missing X-Requested-With: DesktopDisplay header");
  return false;
}

// NEW: layout constants for newly-added UI elements.
// (Pre-existing layout numbers are left as inline literals to limit the size of this change;
//  these are just for the bits added in this update, and are the easiest to retune on real hardware.)
const int HOME_ARROW_LEFT_X   = 25;
const int HOME_ARROW_RIGHT_X  = 295;
const int HOME_ARROW_Y        = 25;
const int STATUS_DOT_X        = 296; // top-right of the home page rain card
const int STATUS_DOT_Y        = 381;
const int STATUS_DOT_R        = 4;
const int NAV_DOT_Y           = 475; // cards end at y=466, so the dots no longer touch their border
const int CITY_DOT_Y          = 44;
const int UPDATE_BADGE_X      = 270;
const int UPDATE_BADGE_Y      = 6;
const int UPDATE_BADGE_W      = 40;
const int UPDATE_BADGE_H      = 16;

// NEW: System page slider/swatch/button layout
const int SLIDER_X       = 50, SLIDER_W = 220, SLIDER_TRACK_H = 10, SLIDER_THUMB_R = 11;
const int SCREEN_LABEL_Y = 220, SCREEN_SLIDER_Y = 240;
const int LED_LABEL_Y    = 266, LED_SLIDER_Y    = 286;
const int MODE_BTN_Y     = 304, MODE_BTN_H      = 32;
// NEW: single hue-gradient picker bar replaces the three separate R/G/B
// sliders - one continuous rainbow strip you drag along, plus a live
// preview swatch showing the resulting color.
const int HUE_BAR_Y = 375, HUE_BAR_H = 22;
const int HUE_BAR_X = 30, HUE_BAR_W = 250;
const int SAVE_BTN_Y     = 415, SAVE_BTN_H      = 36;

// NEW: save-button confirmation state (turns the button blue + "SAVED!" for
// ~1 second after tapping, then reverts automatically)
bool showSaveConfirmation = false;
unsigned long saveConfirmationTime = 0;
const unsigned long SAVE_CONFIRMATION_MS = 1000;

// --- [COLOR UTILS] ---
// NOTE: previously flagged as dead code (never called) - now used by the
// hue-picker slider below to convert a drag position into an RGB color.
// NEW: the IP address and clock minutes are colored with the LED's live
// color (currentLedR/G/B), which legitimately CAN be pure white - the
// weather-sync mapping uses white for "Clouds" conditions. Drawn as opaque
// text on our white card backgrounds, white-on-white is invisible. This
// falls back to black whenever the color is too close to white to read.
uint16_t safeTextColor(int r, int g, int b) {
  int lum = (r * 299 + g * 587 + b * 114) / 1000;
  if (themeDark ? lum < 90 : lum > 215) return T.text; // too close to the card color to read
  return tft.color565(r, g, b);
}

void HSBtoRGB(uint16_t hue, uint8_t sat, uint8_t val, int &r, int &g, int &b) {
  float h = hue / 60.0, s = sat / 255.0, v = val / 255.0;
  float c = v * s, x = c * (1 - abs(fmod(h, 2) - 1)), m = v - c;
  float r1, g1, b1;
  if (h < 1) { r1=c; g1=x; b1=0; } else if (h < 2) { r1=x; g1=c; b1=0; }
  else if (h < 3) { r1=0; g1=c; b1=x; } else if (h < 4) { r1=0; g1=x; b1=c; }
  else if (h < 5) { r1=x; g1=0; b1=c; } else { r1=c; g1=0; b1=x; }
  r = (r1 + m) * 255;
  g = (g1 + m) * 255; b = (b1 + m) * 255;
}

// NEW: reverse direction - approximates the hue of an arbitrary RGB color,
// used only to position the picker's thumb correctly when a color that
// didn't come from the hue bar itself (a saved default, grayscale, etc.)
// is loaded. Grayscale colors have no true hue; they just default to 0.
int rgbToHueApprox(int r, int g, int b) {
  float rf = r / 255.0, gf = g / 255.0, bf = b / 255.0;
  float mx = max(rf, max(gf, bf)), mn = min(rf, min(gf, bf));
  float d = mx - mn;
  if (d < 0.0001) return 0;
  float h;
  if (mx == rf) h = fmod((gf - bf) / d, 6.0);
  else if (mx == gf) h = (bf - rf) / d + 2.0;
  else h = (rf - gf) / d + 4.0;
  h *= 60.0;
  if (h < 0) h += 360.0;
  return (int)h;
}

// NEW: the LED's actual current color, updated every time setWeatherLED runs -
// used to make the clock's minute digits reflect whatever color the LED is
// showing right now, whether that's custom-picked or weather-synced.
int currentLedR = 0, currentLedG = 0, currentLedB = 0;

// NEW: the exact hue last selected on the picker bar, kept as the single
// source of truth. Previously the thumb position was re-derived every
// redraw by converting custR/G/B back into a hue - that round-trip isn't
// perfectly stable at the six color-segment boundaries due to integer
// rounding, which is what caused the picker to visibly jump around during
// a drag. Storing the hue directly and only ever computing custR/G/B FROM
// it (never the reverse, except once at boot to approximate a loaded
// default) removes that instability entirely.
int customHue = 0;

// NEW: saturation and brightness/value for the custom color (0-255 each).
// Previously hardcoded to 255,255 wherever the hue picker computed a color -
// these make that configurable from the web portal's new Saturation and
// Lightness sliders (see handleLive/applyCustomColor). Defaulting to 255,255
// preserves the exact behavior of any color saved before this change.
int customSat = 255;
int customVal = 255;

// NEW: separate color for on-screen text (clock minutes + IP address),
// fully independent of the LED. The LED isn't color-calibrated, so its RGB
// values don't visually match the same values rendered on the (calibrated)
// screen - forcing text to match the LED's exact RGB was fighting that
// mismatch. Defaults to black (readable on every white card) until set.
int textR = 0, textG = 0, textB = 0;
int textHue = 0, textSat = 0, textVal = 0;

// Highlights (page dots, sliders, buttons, the web accent) use the chosen
// text color when it's an actual color that reads well on this theme, so the
// UI matches the clock minutes. Grey/black/white or unreadable picks fall
// back to the theme's own orange. Data colors (rain, temperatures, sunrise/
// sunset, status) never change. Returns true if the accent changed.
bool refreshAccent() {
  int lum = (textR * 299 + textG * 587 + textB * 114) / 1000;
  bool readable = themeDark ? lum >= 90 : lum <= 215;
  uint16_t base = themeDark ? THEME_DARK_COLORS.accent : THEME_LIGHT_COLORS.accent;
  uint16_t a = (textSat >= 60 && textVal >= 60 && readable) ? tft.color565(textR, textG, textB) : base;
  bool changed = a != T.accent;
  T.accent = a;
  return changed;
}

// White or dark label, whichever reads better on the accent color.
uint16_t onAccentText() {
  int r = (T.accent >> 11) << 3, g = ((T.accent >> 5) & 0x3F) << 2, b = (T.accent & 0x1F) << 3;
  return (r * 299 + g * 587 + b * 114) / 1000 > 170 ? RGB565(0x16, 0x20, 0x2F) : TFT_WHITE;
}

// NEW: recomputes textR/G/B from the text color's own hue/saturation/value.
// Deliberately separate from applyCustomColor()/the LED for the reason
// above - this only affects the clock's minute digits and the IP text.
void applyTextColor() {
  HSBtoRGB(textHue, textSat, textVal, textR, textG, textB);
  if (refreshAccent()) { accentDirty = true; accentChangedAt = millis(); } // loop() redraws the page shortly
}

void setWeatherLED(String weather) {
  int r, g, b, val = ledBrightness;
  if (useCustomColor) { r = custR; g = custG; b = custB; } 
  else {
    if (weather == "Clear") { r = 255; g = 165; b = 0; }      
    else if (weather == "Clouds") { r = 255; g = 255; b = 255; } 
    else if (weather == "Rain" || weather == "Drizzle") { r = 0; g = 0; b = 255; }   
    else if (weather == "Thunderstorm") { r = 255; g = 0; b = 255; } 
    else if (weather == "Snow") { r = 0; g = 255; b = 255; } 
    else { r = 0; g = 255; b = 0; } 
  }
  currentLedR = r; currentLedG = g; currentLedB = b; // NEW
  analogWrite(LED_R, 255 - ((r * val) / 255));
  analogWrite(LED_G, 255 - ((g * val) / 255));
  analogWrite(LED_B, 255 - ((b * val) / 255));
}

// NEW: recomputes custR/G/B from the current hue/saturation/value and pushes
// it to the LED right away. Shared by the touchscreen's hue-bar drag and the
// web portal's live sliders so both always agree on the exact same color.
void applyCustomColor() {
  int r, g, b;
  HSBtoRGB(customHue, customSat, customVal, r, g, b);
  custR = r; custG = g; custB = b;
  setWeatherLED(mainWeather);
}

void updateBacklight() { analogWrite(LCD_BL_PIN, globalBrightness); }

// Screen/LED brightness are saved a few seconds after the last change rather
// than on every slider step, so dragging a slider doesn't hammer the flash.
bool brightnessDirty = false;
unsigned long brightnessChangedAt = 0;
const unsigned long BRIGHTNESS_SAVE_DELAY_MS = 3000;

void markBrightnessDirty() { brightnessDirty = true; brightnessChangedAt = millis(); }

void saveBrightnessIfDue() {
  if (!brightnessDirty || millis() - brightnessChangedAt < BRIGHTNESS_SAVE_DELAY_MS) return;
  brightnessDirty = false;
  prefs.putInt("bright", globalBrightness);
  prefs.putInt("ledB", ledBrightness);
}

// Saves the custom LED color as the boot default. Hue/saturation/value are
// stored too, so the sliders come back exactly where they were (R/G/B alone
// can't reproduce a reduced saturation or lightness setting).
void saveCustomColor() {
  prefs.putInt("cR", custR); prefs.putInt("cG", custG); prefs.putInt("cB", custB);
  prefs.putInt("cHue", customHue); prefs.putInt("cSat", customSat); prefs.putInt("cVal", customVal);
}

void touch_calibrate() {
  uint16_t calData[5]; uint8_t calDataOK = 0;
  if (!SPIFFS.begin()) { SPIFFS.format(); SPIFFS.begin(); }
  if (SPIFFS.exists("/TouchCalData1")) {
    File f = SPIFFS.open("/TouchCalData1", "r");
    if (f) { if (f.readBytes((char *)calData, 10) == 10) calDataOK = 1; f.close(); }
  }
  if (!calDataOK) {
    tft.fillScreen(TFT_BLACK); tft.setCursor(20, 160); tft.setTextColor(TFT_WHITE); tft.setTextSize(2);
    tft.println("TOUCH CORNERS TO CALIBRATE");
    tft.calibrateTouch(calData, TFT_MAGENTA, TFT_BLACK, 15);
    File f = SPIFFS.open("/TouchCalData1", "w"); if (f) { f.write((const unsigned char *)calData, 10); f.close(); }
  } else tft.setTouch(calData);
}

void configModeCallback(WiFiManager *myWiFiManager) {
  // NOTE: the old version used long strings at large font sizes with CC_DATUM
  // (centered) — text wider than the screen overflows equally off BOTH edges,
  // which is why it looked cut off on your display. Shorter lines, smaller
  // fonts, and one item per line keeps everything on-screen.
  tft.fillScreen(bgColor); tft.setTextColor(T.text); tft.setTextDatum(CC_DATUM);
  tft.drawString("WIFI SETUP", 160, 90, 4);
  tft.drawString("Connect to WiFi:", 160, 150, 2);
  tft.drawString("WeatherStation-Setup", 160, 175, 2);
  tft.drawString("Then open:", 160, 220, 2);
  tft.drawString("192.168.4.1", 160, 250, 4);
}

const char *const DAY_NAMES[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char *const MONTH_NAMES[] = {"January", "February", "March", "April", "May", "June", "July",
                                   "August", "September", "October", "November", "December"};

String getDayName(int dow) {
  switch(dow) {
    case 1: return "SUN"; case 2: return "MON"; case 3: return "TUE";
    case 4: return "WED"; case 5: return "THU"; case 6: return "FRI"; case 7: return "SAT";
    default: return "";
  }
}

String getDisplayCity(String loc) {
  int commaIndex = loc.indexOf(',');
  if (commaIndex > 0) return loc.substring(0, commaIndex);
  return loc;
}

// Moon age as a fraction of the synodic month (0 = new, 0.5 = full), counted
// from a known new moon on 6 Jan 2000 18:14 UTC. Same formula as the web
// dashboard, so the display and the browser always agree.
const double MOON_SYNODIC_DAYS = 29.530588853;
float moonPhaseFraction() {
  double days = now() / 86400.0 - 10962.7597;
  double age = fmod(days, MOON_SYNODIC_DAYS);
  if (age < 0) age += MOON_SYNODIC_DAYS;
  return (float)(age / MOON_SYNODIC_DAYS);
}

// 0..7 = new, waxing crescent, first quarter, waxing gibbous, full, ... (see getMoonPhaseName)
int moonPhaseIndex(float f) { return ((int)floorf(f * 8 + 0.5f)) & 7; }

String getMoonPhaseName(int b) {
  switch (b) {
    case 0: return "NEW MOON"; case 1: return "WAXING CRESCENT";
    case 2: return "FIRST QUARTER"; case 3: return "WAXING GIBBOUS";
    case 4: return "FULL MOON"; case 5: return "WANING GIBBOUS";
    case 6: return "LAST QUARTER"; case 7: return "WANING CRESCENT";
    default: return "UNKNOWN";
  }
}

String getWindDir(int deg) {
  if (deg >= 337.5 || deg < 22.5) return "N";
  if (deg >= 22.5 && deg < 67.5) return "NE";
  if (deg >= 67.5 && deg < 112.5) return "E";
  if (deg >= 112.5 && deg < 157.5) return "SE";
  if (deg >= 157.5 && deg < 202.5) return "S";
  if (deg >= 202.5 && deg < 247.5) return "SW";
  if (deg >= 247.5 && deg < 292.5) return "W";
  return "NW";
}

// NEW: Open-Meteo reports conditions as a numeric WMO weather code rather
// than OpenWeatherMap's named main/description pair. These two functions
// translate a WMO code into the same style of values our display code
// already expects, so every drawPage*() function works unchanged regardless
// of which provider actually supplied the data.
String wmoToMain(int code) {
  if (code == 0) return "Clear";
  if (code >= 1 && code <= 3) return "Clouds";
  if (code == 45 || code == 48) return "Clouds"; // fog - no closer existing category
  if (code >= 51 && code <= 57) return "Drizzle";
  if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return "Rain";
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "Snow";
  if (code >= 95 && code <= 99) return "Thunderstorm";
  return "Clouds";
}

String wmoToDesc(int code) {
  switch (code) {
    case 0: return "CLEAR SKY";
    case 1: return "MAINLY CLEAR";
    case 2: return "PARTLY CLOUDY";
    case 3: return "OVERCAST";
    case 45: return "FOG";
    case 48: return "RIME FOG";
    case 51: return "LIGHT DRIZZLE";
    case 53: return "DRIZZLE";
    case 55: return "DENSE DRIZZLE";
    case 56: return "LIGHT FREEZING DRIZZLE";
    case 57: return "FREEZING DRIZZLE";
    case 61: return "SLIGHT RAIN";
    case 63: return "RAIN";
    case 65: return "HEAVY RAIN";
    case 66: return "LIGHT FREEZING RAIN";
    case 67: return "FREEZING RAIN";
    case 71: return "SLIGHT SNOW";
    case 73: return "SNOW";
    case 75: return "HEAVY SNOW";
    case 77: return "SNOW GRAINS";
    case 80: return "SLIGHT RAIN SHOWERS";
    case 81: return "RAIN SHOWERS";
    case 82: return "VIOLENT RAIN SHOWERS";
    case 85: return "SLIGHT SNOW SHOWERS";
    case 86: return "HEAVY SNOW SHOWERS";
    case 95: return "THUNDERSTORM";
    case 96: return "THUNDERSTORM, HAIL";
    case 99: return "SEVERE THUNDERSTORM";
    default: return "UNKNOWN";
  }
}

// NEW: Open-Meteo gives dates as "YYYY-MM-DD" strings rather than a unix
// timestamp, so day-of-week isn't available for free the way it is from
// OpenWeatherMap's dt field. Reuses TimeLib (already linked in) rather than
// hand-rolling a day-of-week algorithm.
int dayOfWeekFromDateString(String dateStr) {
  tmElements_t tmEl;
  tmEl.Year = dateStr.substring(0, 4).toInt() - 1970;
  tmEl.Month = dateStr.substring(5, 7).toInt();
  tmEl.Day = dateStr.substring(8, 10).toInt();
  tmEl.Hour = 12; tmEl.Minute = 0; tmEl.Second = 0; // noon avoids any DST edge case
  return weekday(makeTime(tmEl));
}

bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if(y >= tft.height()) return 0;
  tft.pushImage(x, y, w, h, bitmap); 
  return 1;
}

// --- [NEW: LOCAL SENSOR READ] ---
#if USE_BME280
void readLocalSensor() {
  if (!bmeOK) return;
  localTemp = bme.readTemperature();
  localHumid = bme.readHumidity();
  localPressure = bme.readPressure() / 100.0F;
}
#endif

// --- [NEW: API KEY FALLBACK] ---
// Tries the primary key first. Only falls back to the backup key on 401
// (bad/revoked key) or 429 (rate limit hit) — those are the failures a
// second key can actually fix. Any other failure (timeout, DNS, etc.)
// wouldn't be helped by a different key, so it isn't retried.
// baseUrlNoKey should be the full URL *without* the trailing "&appid=...".
int httpGetJson(String baseUrlNoKey, String &payload) {
  HTTPClient http;
  http.setTimeout(5000);

  http.begin(baseUrlNoKey + "&appid=" + apiKey);
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    payload = http.getString();
    http.end();
    activeApiKeySource = "primary";
    return code;
  }
  http.end();

  if ((code == 401 || code == 429) && apiKeyBackup.length() > 0 && apiKeyBackup != apiKey) {
    http.begin(baseUrlNoKey + "&appid=" + apiKeyBackup);
    int code2 = http.GET();
    if (code2 == HTTP_CODE_OK) {
      payload = http.getString();
      http.end();
      activeApiKeySource = "backup";
      return code2;
    }
    http.end();
    return code2;
  }
  return code;
}


// Returns true and fills lat/lon if a coordinate for this city is available
// (either from cache, or freshly fetched). City coordinates don't change,
// so once geocoded we don't need to hit that endpoint again unless the
// location string itself changes.
bool getCityCoords(int idx, double &lat, double &lon) {
  String loc = locs[idx];

  if (cityGeoCached[idx] && cityGeoLocStr[idx] == loc) {
    lat = cityLat[idx]; lon = cityLon[idx];
    return true;
  }

  String currentLoc = loc;
  currentLoc.replace(",UK", ",GB");
  currentLoc.replace(",uk", ",GB");
  String encodedLoc = urlEncode(currentLoc);
  String geoUrl = "http://api.openweathermap.org/geo/1.0/direct?q=" + encodedLoc + "&limit=1";

  String payload;
  bool ok = false;
  if (httpGetJson(geoUrl, payload) == HTTP_CODE_OK) {
    DynamicJsonDocument geoDoc(1536);
    deserializeJson(geoDoc, payload);
    if (geoDoc.size() > 0) {
      lat = geoDoc[0]["lat"]; lon = geoDoc[0]["lon"];
      cityLat[idx] = lat; cityLon[idx] = lon;
      cityGeoCached[idx] = true; cityGeoLocStr[idx] = loc;
      prefs.putFloat(("lat" + String(idx)).c_str(), (float)lat);
      prefs.putFloat(("lon" + String(idx)).c_str(), (float)lon);
      prefs.putString(("geoLoc" + String(idx)).c_str(), loc);
      ok = true;
    }
  }
  return ok;
}

// --- [CORE WEATHER DATA FETCH] ---

// NEW: fetches from OpenWeatherMap - this is the original fetch logic,
// unchanged, just moved into its own function so getOpenWeatherData() can
// dispatch to whichever provider the current city is configured to use.
bool fetchFromOpenWeatherMap() {
  bool weatherFetchSuccess = false;

  // Step 1: Geocoding (cached per-city, see getCityCoords)
  double lat = 0, lon = 0;
  bool foundByGeo = getCityCoords(currentCity, lat, lon);

  String currentLoc = locs[currentCity];
  currentLoc.replace(",UK", ",GB");
  currentLoc.replace(",uk", ",GB");
  String encodedLoc = urlEncode(currentLoc);

  // Step 2: Weather Call
  String weatherUrl;
  if (foundByGeo) weatherUrl = "http://api.openweathermap.org/data/2.5/weather?lat=" + String(lat, 6) + "&lon=" + String(lon, 6) + "&units=metric&cb=" + String(millis());
  else weatherUrl = "http://api.openweathermap.org/data/2.5/weather?q=" + encodedLoc + "&units=metric&cb=" + String(millis());

  String weatherPayload;
  if (httpGetJson(weatherUrl, weatherPayload) == HTTP_CODE_OK) {
    DynamicJsonDocument doc(3072); deserializeJson(doc, weatherPayload);
    if (doc.containsKey("main")) {
      tempnum = doc["main"]["temp"].as<float>(); 
      tempMin = doc["main"]["temp_min"].as<float>(); 
      tempMax = doc["main"]["temp_max"].as<float>();
      feels_like = doc["main"]["feels_like"].as<float>();
      huminum = (int)doc["main"]["humidity"]; 
      pressure = (int)doc["main"]["pressure"];
      clouds = (int)doc["clouds"]["all"]; 
      visibility = (int)doc["visibility"] / 1000;
      windSpeedKmH = doc["wind"]["speed"].as<float>() * 3.6;
      windDir = getWindDir(doc["wind"]["deg"]);
      mainWeather = doc["weather"][0]["main"].as<String>(); 
      weatherDesc = doc["weather"][0]["description"].as<String>(); 
      weatherDesc.toUpperCase();
      rain1h = doc["rain"]["1h"] | 0.0; 
      rain3h = doc["rain"]["3h"] | 0.0;
      if (!foundByGeo && doc.containsKey("coord")) { lat = doc["coord"]["lat"]; lon = doc["coord"]["lon"]; }
      tzOffsetSec[currentCity] = doc["timezone"].as<long>();
      sysRise = doc["sys"]["sunrise"].as<long>() + tzOffsetSec[currentCity];
      sysSet  = doc["sys"]["sunset"].as<long>() + tzOffsetSec[currentCity];
      sunRise = (hour(sysRise) < 10 ? "0" : "") + String(hour(sysRise)) + ":" + (minute(sysRise) < 10 ? "0" : "") + String(minute(sysRise));
      sunSet  = (hour(sysSet) < 10 ? "0" : "") + String(hour(sysSet)) + ":" + (minute(sysSet) < 10 ? "0" : "") + String(minute(sysSet));
      setWeatherLED(mainWeather);
      weatherFetchSuccess = true;
    }
  }

  // Step 3: Forecast (Today + 4 Days)
  String forecastUrl;
  if (lat != 0 || lon != 0) forecastUrl = "http://api.openweathermap.org/data/2.5/forecast?lat=" + String(lat, 6) + "&lon=" + String(lon, 6) + "&units=metric&cb=" + String(millis());
  else forecastUrl = "http://api.openweathermap.org/data/2.5/forecast?q=" + encodedLoc + "&units=metric&cb=" + String(millis());

  String forecastPayload;
  if (httpGetJson(forecastUrl, forecastPayload) == HTTP_CODE_OK) {
    // Only keep the fields we actually use - the full 40-slice response is far
    // bigger than needed and could run out of memory part-way through parsing
    // (silently dropping the later forecast days).
    DynamicJsonDocument filter(512);
    filter["list"][0]["dt"] = true;
    filter["list"][0]["main"]["temp"] = true;
    filter["list"][0]["weather"][0]["main"] = true;
    filter["list"][0]["rain"]["3h"] = true;
    DynamicJsonDocument fdoc(12288);
    DeserializationError ferr = deserializeJson(fdoc, forecastPayload, DeserializationOption::Filter(filter));
    if (ferr) logDebug("Forecast JSON error: " + String(ferr.c_str()));
    if (!ferr && fdoc.containsKey("list")) {
      JsonArray list = fdoc["list"];
      int dIdx = -1; String curL = "";
      for (JsonObject slice : list) {
        long dt = slice["dt"].as<long>() + tzOffsetSec[currentCity];

        // Grab the rainfall figure for each of the next few 3-hour slices,
        // independent of the day-bucketing below, for the precipitation graph
        if (upcomingRainCount < RAIN_GRAPH_POINTS) {
          float r3 = slice["rain"]["3h"] | 0.0;
          upcomingRainMm[upcomingRainCount] = r3;
          int hh = hour(dt);
          upcomingRainLabel[upcomingRainCount] = (hh < 10 ? "0" : "") + String(hh) + "h";
          upcomingRainCount++;
        }

        String dL = getDayName(weekday(dt)); 
        if (dL != curL) { 
          dIdx++;
          if (dIdx >= 5) break;
          curL = dL; forecast[dIdx].dayName = dL;
          forecast[dIdx].high = -99.0; forecast[dIdx].low = 99.0;
          forecast[dIdx].main = slice["weather"][0]["main"].as<String>();
          forecast[dIdx].main.toUpperCase();
          forecast[dIdx].valid = true;
        }
        float t = slice["main"]["temp"].as<float>();
        if (t > forecast[dIdx].high) forecast[dIdx].high = t;
        if (t < forecast[dIdx].low) forecast[dIdx].low = t;
      }
    }
  }

  return weatherFetchSuccess;
}

// NEW: fetches from Open-Meteo - no API key needed, one combined request
// gets current + hourly + daily data. useKnmiModel uses KNMI's "seamless"
// blend: their 2km Harmonie AROME Netherlands model where it has data, filled
// in with KNMI's Europe model and then ECMWF beyond its ~2.5-day range or
// outside the Netherlands - so all 5 forecast days always get real values.
// Reads a JSON number as float, or def if it's missing/null (Open-Meteo uses
// null for values a model doesn't cover).
float jsonNum(JsonVariant v, float def) { return v.isNull() ? def : v.as<float>(); }

bool fetchFromOpenMeteo(bool useKnmiModel) {
  double lat = 0, lon = 0;
  if (!getCityCoords(currentCity, lat, lon)) return false; // Open-Meteo needs coordinates, not a free-text city name

  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 6) + "&longitude=" + String(lon, 6)
    + "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,pressure_msl,wind_speed_10m,wind_direction_10m,visibility,cloud_cover"
    + "&hourly=weather_code,precipitation"
    + "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset"
    + "&timezone=auto&forecast_days=5&forecast_hours=15";
  if (useKnmiModel) url += "&models=knmi_seamless";

  HTTPClient http;
  http.setTimeout(5000);
  WiFiClientSecure tls; WiFiClient plain;
  httpBeginSafe(http, tls, plain, url);
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }
  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, payload);
  if (err || !doc.containsKey("current")) return false;

  JsonObject cur = doc["current"];
  // A model without data for this place/time returns nulls - treat that as a
  // failed fetch rather than showing 0.0 C.
  if (cur["temperature_2m"].isNull() || cur["weather_code"].isNull()) return false;
  tempnum = cur["temperature_2m"].as<float>();
  feels_like = jsonNum(cur["apparent_temperature"], tempnum);
  huminum = (int)round(jsonNum(cur["relative_humidity_2m"], 0));
  pressure = (int)round(jsonNum(cur["pressure_msl"], 0)); // sea level, same as OpenWeatherMap and the history log
  clouds = (int)round(jsonNum(cur["cloud_cover"], 0));
  visibility = (int)(jsonNum(cur["visibility"], 0) / 1000);
  windSpeedKmH = jsonNum(cur["wind_speed_10m"], 0); // Open-Meteo's default unit is already km/h
  windDir = getWindDir((int)jsonNum(cur["wind_direction_10m"], 0));
  int codeNow = cur["weather_code"].as<int>();
  mainWeather = wmoToMain(codeNow);
  weatherDesc = wmoToDesc(codeNow);
  setWeatherLED(mainWeather);

  long utcOffsetSec = doc["utc_offset_seconds"] | 0L;
  tzOffsetSec[currentCity] = utcOffsetSec;

  // Daily: today's high/low, sunrise/sunset, and the 5-day forecast page
  if (doc.containsKey("daily")) {
    JsonObject daily = doc["daily"];
    JsonArray dTime = daily["time"];
    JsonArray dMax = daily["temperature_2m_max"];
    JsonArray dMin = daily["temperature_2m_min"];
    JsonArray dCode = daily["weather_code"];
    JsonArray dSunrise = daily["sunrise"];
    JsonArray dSunset = daily["sunset"];

    if (dMax.size() > 0 && !dMax[0].isNull()) tempMax = dMax[0].as<float>();
    if (dMin.size() > 0 && !dMin[0].isNull()) tempMin = dMin[0].as<float>();
    if (dSunrise.size() > 0 && !dSunrise[0].isNull()) sunRise = dSunrise[0].as<String>().substring(11, 16);
    if (dSunset.size() > 0 && !dSunset[0].isNull()) sunSet = dSunset[0].as<String>().substring(11, 16);

    int days = min((int)dTime.size(), 5);
    for (int i = 0; i < days; i++) {
      // Days the model doesn't cover come back as null - show "NO DATA"
      // for them instead of a made-up 0/0 C "CLEAR" day.
      if (dMax[i].isNull() || dMin[i].isNull() || dCode[i].isNull()) { forecast[i].valid = false; continue; }
      forecast[i].dayName = getDayName(dayOfWeekFromDateString(dTime[i].as<String>()));
      forecast[i].high = dMax[i].as<float>();
      forecast[i].low = dMin[i].as<float>();
      forecast[i].main = wmoToMain(dCode[i].as<int>());
      forecast[i].main.toUpperCase();
      forecast[i].valid = true;
    }
  }

  // Hourly: 1h/3h rainfall totals, and the upcoming-rain graph. Each graph
  // point is the SUM of three hourly values, so it's a true 3-hour total -
  // the same thing OpenWeatherMap's 3-hour forecast slices report.
  if (doc.containsKey("hourly")) {
    JsonObject hourlyObj = doc["hourly"];
    JsonArray hTime = hourlyObj["time"];
    JsonArray hPrecip = hourlyObj["precipitation"];
    int nHours = min((int)hPrecip.size(), (int)hTime.size());

    rain1h = nHours > 0 ? jsonNum(hPrecip[0], 0) : 0.0f;
    rain3h = 0.0;
    for (int i = 0; i < 3 && i < nHours; i++) rain3h += jsonNum(hPrecip[i], 0);

    for (int i = 0; i < RAIN_GRAPH_POINTS; i++) {
      int idx = i * 3;
      if (idx >= nHours) break;
      float sum = 0;
      for (int h = idx; h < idx + 3 && h < nHours; h++) sum += jsonNum(hPrecip[h], 0);
      upcomingRainMm[upcomingRainCount] = sum;
      upcomingRainLabel[upcomingRainCount] = hTime[idx].as<String>().substring(11, 13) + "h";
      upcomingRainCount++;
    }
  }

  return true;
}

void saveForecastTo(WeatherSnapshot &s) {
  for (int i = 0; i < 5; i++) s.forecast[i] = forecast[i];
  for (int i = 0; i < RAIN_GRAPH_POINTS; i++) { s.upcomingRainMm[i] = upcomingRainMm[i]; s.upcomingRainLabel[i] = upcomingRainLabel[i]; }
  s.upcomingRainCount = upcomingRainCount;
}

void restoreForecastFrom(const WeatherSnapshot &s) {
  for (int i = 0; i < 5; i++) forecast[i] = s.forecast[i];
  for (int i = 0; i < RAIN_GRAPH_POINTS; i++) { upcomingRainMm[i] = s.upcomingRainMm[i]; upcomingRainLabel[i] = s.upcomingRainLabel[i]; }
  upcomingRainCount = s.upcomingRainCount;
}

void restoreCurrentFrom(const WeatherSnapshot &s) {
  tempnum = s.tempnum; tempMin = s.tempMin; tempMax = s.tempMax; feels_like = s.feels_like;
  windSpeedKmH = s.windSpeedKmH; rain1h = s.rain1h; rain3h = s.rain3h;
  huminum = s.huminum; pressure = s.pressure; visibility = s.visibility; clouds = s.clouds;
  weatherDesc = s.weatherDesc; mainWeather = s.mainWeather; windDir = s.windDir;
  sunRise = s.sunRise; sunSet = s.sunSet; sysRise = s.sysRise; sysSet = s.sysSet;
}

int lastDataCity = -1;  // which city the data currently in memory belongs to
bool lastDataOK = false; // whether that data came from a successful fetch

void getOpenWeatherData() {
  if (WiFi.status() != WL_CONNECTED) { lastFetchOK = false; return; }

  // Keep a copy of the current data if it's a good set for this same city -
  // a refresh that fails (or only half-succeeds) then falls back to it.
  bool canRestore = (lastDataCity == currentCity && lastDataOK);
  WeatherSnapshot snap;
  if (canRestore) {
    snap.tempnum = tempnum; snap.tempMin = tempMin; snap.tempMax = tempMax; snap.feels_like = feels_like;
    snap.windSpeedKmH = windSpeedKmH; snap.rain1h = rain1h; snap.rain3h = rain3h;
    snap.huminum = huminum; snap.pressure = pressure; snap.visibility = visibility; snap.clouds = clouds;
    snap.weatherDesc = weatherDesc; snap.mainWeather = mainWeather; snap.windDir = windDir;
    snap.sunRise = sunRise; snap.sunSet = sunSet; snap.sysRise = sysRise; snap.sysSet = sysSet;
    saveForecastTo(snap);
  }

  // RESET ALL DATA: clears ghost data (from the previous city, or from the
  // last fetch) so nothing stale gets mixed into the new results
  tempnum = 0; tempMin = 0; tempMax = 0;
  huminum = 0; feels_like = 0;
  pressure = 0; visibility = 0; clouds = 0; rain1h = 0; rain3h = 0;
  mainWeather = "Fetching..."; weatherDesc = "";
  for(int i=0; i<5; i++) forecast[i].valid = false;
  upcomingRainCount = 0;

  bool weatherFetchSuccess;
  if (cityProvider[currentCity] == PROVIDER_OPENWEATHER) {
    weatherFetchSuccess = fetchFromOpenWeatherMap();
  } else {
    weatherFetchSuccess = fetchFromOpenMeteo(cityProvider[currentCity] == PROVIDER_OPENMETEO_KNMI);
  }

  if (canRestore) {
    if (!weatherFetchSuccess) {
      // Whole refresh failed - put everything back (the status dot still
      // turns red so you can see the data is getting old).
      restoreCurrentFrom(snap);
      restoreForecastFrom(snap);
      setWeatherLED(mainWeather);
      logDebug("Weather refresh failed - keeping previous data");
    } else if (upcomingRainCount == 0 && !forecast[0].valid) {
      // Current weather OK but the separate forecast request failed
      restoreForecastFrom(snap);
    }
  }

  // NEW: record fetch outcome/time for the status indicator
  lastFetchOK = weatherFetchSuccess;
  if (weatherFetchSuccess) lastFetchMillis = millis();
  if (weatherFetchSuccess || !canRestore) { lastDataCity = currentCity; lastDataOK = weatherFetchSuccess; }
  // NOTE: history is no longer sampled here - see historyLoop(), which logs
  // all three cities in the background regardless of which one is shown.
}

// --- [NEW: NON-BLOCKING-FEELING UPDATE WRAPPER] ---
// Shows a small "..." badge instead of wiping the whole screen, so the old
// data stays visible (and the UI doesn't look frozen/blank) while the
// network calls run. Once done, forces a full redraw of the current page.
void drawUpdatingBadge(bool show) {
  if (show) {
    // Home page: over the city dots (the corners hold the city arrows).
    // Other pages: top-right corner.
    int bx = displayMode == 0 ? 140 : UPDATE_BADGE_X, by = displayMode == 0 ? 37 : UPDATE_BADGE_Y;
    tft.fillRoundRect(bx, by, UPDATE_BADGE_W, UPDATE_BADGE_H, 6, T.accent);
    tft.setTextColor(onAccentText(), T.accent); tft.setTextDatum(CC_DATUM);
    tft.drawString("...", bx + UPDATE_BADGE_W / 2, by + UPDATE_BADGE_H / 2, 1);
  }
  // no separate "hide" drawing needed - the subsequent drawPageBackground() call
  // repaints that whole area anyway.
}

void triggerWeatherUpdate() {
  unsigned long t0 = millis();
  isUpdating = true;
  drawUpdatingBadge(true);
  bool locked = netLock(3000); // let a background request finish first, but never stall the screen for long
  unsigned long waited = millis() - t0;
  getOpenWeatherData();
  if (locked) netUnlock();
  isUpdating = false;
  if (millis() - t0 > 1000) logDebug("SLOW weather update: " + String(millis() - t0) + " ms (waited " + String(waited) + " ms for a background request)");
  applyTheme(); // new sunrise/sunset times (or another city) can change the AUTO theme
  drawPageBackground(displayMode);
  drawnMode = -1;
}

// NEW: runs periodically from loop(). If WiFi has dropped, kicks off a
// reconnect attempt using the already-known configuration (non-blocking -
// WiFi.reconnect() just requests the reconnect and returns immediately).
// If it just came back online after being down, refreshes weather data
// right away instead of waiting for the next scheduled 10-minute cycle.
void checkWifiHealth() {
  if (millis() - lastWifiCheckMillis < WIFI_CHECK_INTERVAL_MS) return;
  lastWifiCheckMillis = millis();

  bool isConnected = (WiFi.status() == WL_CONNECTED);
  if (!isConnected) {
    Serial.println("WiFi disconnected - attempting reconnect...");
    WiFi.reconnect();
  } else if (!wifiWasConnected) {
    Serial.println("WiFi reconnected - refreshing weather data");
    triggerWeatherUpdate();
  }
  wifiWasConnected = isConnected;
}

// --- [RAIN NOWCAST + PRESSURE TREND] ---
// Short-term rain (next 2 hours) and the 3-hour pressure tendency for the city
// on screen. Both follow the same rules: a message is only shown when the data
// clearly supports it, and it disappears when the data gets old - showing
// nothing beats showing something wrong.
//
// Rain: Buienradar's radar-based forecast for the Netherlands/Belgium (5-min
// steps, refreshed every 5 minutes; mm/h = 10^((value-109)/32), value 77 =
// 0.1 mm/h per their documentation). Elsewhere, or if Buienradar can't be
// reached, Open-Meteo's 15-minute model forecast - labelled as "forecast".

bool inBuienradarArea(double lat, double lon) { return lat > 49.4 && lat < 54.0 && lon > 2.4 && lon < 7.5; }

// Parses Buienradar's "value|HH:MM" lines into mm[] (mm/h) and lbl[]. Returns the step count.
int parseRainText(const String &body, float *mm, String *lbl, int maxN) {
  int n = 0, from = 0;
  while (n < maxN && from < (int)body.length()) {
    int nl = body.indexOf('\n', from);
    if (nl < 0) nl = body.length();
    String line = body.substring(from, nl);
    from = nl + 1;
    line.trim();
    int bar = line.indexOf('|');
    if (bar <= 0) continue;
    String v = line.substring(0, bar); v.replace(",", ".");
    String t = line.substring(bar + 1); t.trim();
    if (t.length() < 5 || t[2] != ':') continue;
    float val = v.toFloat();
    mm[n] = val <= 0 ? 0 : pow(10.0, (val - 109.0) / 32.0);
    lbl[n] = t.substring(0, 5);
    n++;
  }
  return n;
}

// --- [BACKGROUND NETWORK WORKER] ---
// Every request for the rain outlook, rain model, pressure trend and forecast
// check runs on its own FreeRTOS task (core 0). The main loop - touch, screen
// and web pages - only hands over a job and later parses the reply, so a slow
// server or a stalled TLS handshake can never freeze the device. One job at a
// time; the blocking weather and history fetches on the main loop take the
// same lock, so two HTTPS sessions (~45 KB of RAM each) never run at once.
enum NetKind { NJ_NOW = 1, NJ_MODEL, NJ_PTREND, NJ_VF_OBS, NJ_VF_OM, NJ_VF_OWM, NJ_HIST };
NetJob netJob;
bool netBusy = false;
QueueHandle_t netJobQ = nullptr, netDoneQ = nullptr;
SemaphoreHandle_t netMutex = nullptr;

// HTTPClient::begin(url) leaves the TLS handshake timeout at the library's
// 120 s; this sets 8 s (plus 5 s to connect) for the fetches on the main loop.
bool httpBeginSafe(HTTPClient &http, WiFiClientSecure &tls, WiFiClient &plain, const String &url) {
  http.setConnectTimeout(5000);
  if (url.startsWith("https://")) { tls.setInsecure(); tls.setHandshakeTimeout(8); return http.begin(tls, url); }
  return http.begin(plain, url);
}

// Plain GET with hard limits: 5 s to connect, 8 s for the TLS handshake (the
// library default is 120 s), 6 s per read.
int netGet(const String &url, String &body) {
  bool https = url.startsWith("https://");
  WiFiClientSecure tls;
  WiFiClient plain;
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(6000);
  bool ok;
  if (https) { tls.setInsecure(); tls.setHandshakeTimeout(8); ok = http.begin(tls, url); }
  else ok = http.begin(plain, url);
  if (!ok) return -1;
  int code = http.GET();
  body = code == HTTP_CODE_OK ? http.getString() : String("");
  http.end();
  return code;
}

void netWorker(void *) {
  for (;;) {
    NetJob *j;
    if (xQueueReceive(netJobQ, &j, portMAX_DELAY) != pdTRUE) continue;
    unsigned long t0 = millis();
    xSemaphoreTake(netMutex, portMAX_DELAY);
    j->code = WiFi.status() == WL_CONNECTED ? netGet(j->url, j->body) : -1;
    xSemaphoreGive(netMutex);
    j->ms = millis() - t0;
    xQueueSend(netDoneQ, &j, portMAX_DELAY);
  }
}

void netBegin() {
  netJobQ = xQueueCreate(1, sizeof(NetJob *));
  netDoneQ = xQueueCreate(1, sizeof(NetJob *));
  netMutex = xSemaphoreCreateMutex();
  // Idle priority: it time-shares core 0 with the idle task, so even a long
  // TLS handshake can't starve the task watchdog (which restarts the chip).
  xTaskCreatePinnedToCore(netWorker, "net", 12288, nullptr, tskIDLE_PRIORITY, nullptr, 0);
}

// Main loop: hand a request to the worker. False if one is still running.
bool netPost(int kind, int city, int step, int slot, uint32_t start, const String &url) {
  if (netBusy || !netJobQ) return false;
  netJob.kind = kind; netJob.city = city; netJob.step = step; netJob.slot = slot; netJob.start = start;
  netJob.url = url; netJob.body = ""; netJob.code = 0; netJob.ms = 0;
  NetJob *jp = &netJob;
  if (xQueueSend(netJobQ, &jp, 0) != pdTRUE) return false;
  netBusy = true;
  return true;
}

// For the blocking fetches still on the main loop (weather, history). They
// wait at most a few seconds; if the background request is still running they
// go ahead anyway rather than freezing the screen.
bool netLock(unsigned long ms) { return netMutex && xSemaphoreTake(netMutex, pdMS_TO_TICKS(ms)) == pdTRUE; }
void netUnlock() { xSemaphoreGive(netMutex); }

// Coordinates only if already known - never a (blocking) geocoding request.
bool cachedCoords(int c, double &lat, double &lon) {
  if (!cityGeoCached[c] || cityGeoLocStr[c] != locs[c]) return false;
  lat = cityLat[c]; lon = cityLon[c];
  return true;
}

// Rain outlook sources, tried in this order: Buienradar radar (it has used two
// hostnames over the years), then Open-Meteo's 15-minute forecast.
String nowcastUrl(int step, double lat, double lon) {
  if (step == 0) return "https://gps.buienradar.nl/getrr.php?lat=" + String(lat, 2) + "&lon=" + String(lon, 2);
  if (step == 1) return "https://gpsgadget.buienradar.nl/data/raintext?lat=" + String(lat, 2) + "&lon=" + String(lon, 2);
  return "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4) + "&longitude=" + String(lon, 4)
    + "&minutely_15=precipitation&forecast_minutely_15=9&timezone=auto";
}

void nowcastDone(int city, bool ok) {
  bool cityChanged = nowCity != city;
  if (ok) { nowFetchedAt = millis(); nowVersion++; nowLastError = ""; }
  else if (cityChanged) { nowCount = 0; nowVersion++; } // never show the old city's rain
  nowCity = city;
  nowNextFetch = millis() + (ok ? (nowRadar ? 5UL : 15UL) * 60000UL : 2UL * 60000UL);
  analyzeNowcast(); checkRainAlert();
  if (!ok) logDebug("RAIN 2H fetch failed: " + nowLastError);
}

int nowBrPreferred = 0; // the Buienradar hostname (step 0 or 1) that answered last time

void nowcastStart(int city) {
  double lat, lon;
  nowLastError = "";
  if (!cachedCoords(city, lat, lon)) { nowLastError = "no coordinates for the city yet"; nowcastDone(city, false); return; }
  int step = inBuienradarArea(lat, lon) ? nowBrPreferred : 2;
  // 'slot' carries which Buienradar hostnames were tried (bit 0 / bit 1)
  netPost(NJ_NOW, city, step, step < 2 ? (1 << step) : 0, 0, nowcastUrl(step, lat, lon));
}

void nowcastResult(NetJob &j) {
  if (j.city != currentCity) return; // city switched meanwhile - nowcastLoop starts over
  bool ok = false;
  String err;
  if (j.code != HTTP_CODE_OK) err = String(j.step < 2 ? "Buienradar" : "Open-Meteo") + " HTTP " + String(j.code);
  else if (j.step < 2) {
    float mm[NOW_STEPS_MAX]; String lbl[NOW_STEPS_MAX];
    int n = parseRainText(j.body, mm, lbl, NOW_STEPS_MAX);
    if (n >= 6) {
      for (int i = 0; i < n; i++) { nowMm[i] = mm[i]; nowLabel[i] = lbl[i]; }
      nowCount = n; nowStepMin = 5; nowRadar = true;
      nowSource = "Buienradar radar (" + j.url.substring(8, j.url.indexOf('/', 8)) + ")";
      nowBrPreferred = j.step; // start with this hostname next time
      ok = true;
    } else err = "Buienradar: unexpected reply (" + String(n) + " steps)";
  } else {
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, j.body)) err = "Open-Meteo: bad JSON";
    else {
      JsonArray t = doc["minutely_15"]["time"], p = doc["minutely_15"]["precipitation"];
      float mm[NOW_STEPS_MAX]; String lbl[NOW_STEPS_MAX];
      int n = 0;
      for (int i = 0; i < (int)t.size() && i < (int)p.size() && n < NOW_STEPS_MAX; i++) {
        if (p[i].isNull()) continue;
        mm[n] = p[i].as<float>() * 4.0f; // mm per 15 min -> mm/h
        lbl[n] = t[i].as<String>().substring(11, 16);
        n++;
      }
      if (n < 4) err = "Open-Meteo: no 15-minute data here";
      else {
        for (int i = 0; i < n; i++) { nowMm[i] = mm[i]; nowLabel[i] = lbl[i]; }
        nowCount = n; nowStepMin = 15; nowRadar = false;
        nowSource = "Open-Meteo 15-min forecast";
        ok = true;
      }
    }
  }
  if (!ok) {
    nowLastError += String(nowLastError.length() ? "; " : "") + err;
    double lat, lon;
    if (j.step < 2 && cachedCoords(j.city, lat, lon)) { // next source: the other hostname, then Open-Meteo
      int tried = j.slot, city = j.city;
      int next = !(tried & 1) ? 0 : (!(tried & 2) ? 1 : 2);
      netPost(NJ_NOW, city, next, next < 2 ? (tried | (1 << next)) : tried, 0, nowcastUrl(next, lat, lon));
      return;
    }
  }
  nowcastDone(j.city, ok);
}

// Open-Meteo 15-minute model forecast for the next 3 hours, kept next to the
// radar so the two can be compared ("blending", see analyzeNowcast).
void modelDone(int city, bool ok) {
  if (ok) { modCity = city; modFetchedAt = millis(); nowVersion++; }
  else if (modCity != city) { modCity = city; modFetchedAt = 0; }
  modNextFetch = millis() + (ok ? 15UL : 5UL) * 60000UL;
  analyzeNowcast();
}

void modelStart(int city) {
  double lat, lon;
  if (!cachedCoords(city, lat, lon)) { modelDone(city, false); return; }
  netPost(NJ_MODEL, city, 0, 0, 0, "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4) + "&longitude=" + String(lon, 4)
    + "&minutely_15=precipitation&forecast_minutely_15=12&timezone=auto");
}

void modelResult(NetJob &j) {
  if (j.city != currentCity) return;
  bool ok = false;
  DynamicJsonDocument doc(4096);
  if (j.code == HTTP_CODE_OK && !deserializeJson(doc, j.body)) {
    JsonArray t = doc["minutely_15"]["time"], p = doc["minutely_15"]["precipitation"];
    int n = 0;
    for (int i = 0; i < (int)t.size() && i < (int)p.size() && n < MOD_STEPS_MAX; i++) {
      if (p[i].isNull()) continue;
      modMm[n] = p[i].as<float>() * 4.0f; // mm per 15 min -> mm/h
      modLabel[n] = t[i].as<String>().substring(11, 16);
      n++;
    }
    if (n >= 4) { modCount = n; ok = true; }
  }
  modelDone(j.city, ok);
}

bool modelValid() {
  return modCity == currentCity && modCount >= 4 && modFetchedAt != 0 && millis() - modFetchedAt < 30UL * 60000UL;
}

// The model's rain rate at a "HH:MM" time (the 15-min step it falls in), or -1.
float modelAt(const String &hhmm) {
  if (!modelValid()) return -1;
  int m = timeStrToMinutes(hhmm);
  for (int j = 0; j < modCount; j++) {
    int d = ((m - timeStrToMinutes(modLabel[j]) + 1440 + 720) % 1440) - 720;
    if (d >= 0 && d < 15) return modMm[j];
  }
  return -1;
}

// Minutes from now (local time of the city on screen) until a "HH:MM" label;
// negative if it's in the past. Handles midnight.
int minutesUntil(const String &hhmm) {
  int m = timeStrToMinutes(hhmm);
  if (m < 0) return 9999;
  time_t lt = now() + tzOffsetSec[currentCity];
  int cur = hour(lt) * 60 + minute(lt);
  return ((m - cur + 1440 + 720) % 1440) - 720;
}

// Turns the raw steps into the outlook shown everywhere (display, web, LED).
// Rules: steps under 0.1 mm/h count as dry (radar noise and mist); rain only
// "starts" when two steps in a row are wet (no single-speckle alarms), and
// only "stops" when two steps in a row are dry.
void analyzeNowcast() {
  unsigned long maxAge = (nowRadar ? 15UL : 30UL) * 60000UL;
  nowValid = nowCount >= 4 && nowCity == currentCity && nowFetchedAt != 0
             && millis() - nowFetchedAt < maxAge && timeStatus() != timeNotSet;
  nowMsg = ""; nowShort = ""; nowBlend = false; nowStartIdx = -1; nowRainingNow = false; nowMaxMm = 0; nowFirst = 0;
  if (!nowValid) return;

  // Skip steps that are already in the past
  while (nowFirst < nowCount - 1 && minutesUntil(nowLabel[nowFirst]) < -nowStepMin) nowFirst++;
  if (nowCount - nowFirst < 4) { nowValid = false; return; }

  const float WET = 0.1f;
  auto wet = [&](int i) { return i < nowCount && nowMm[i] >= WET; };
  for (int i = nowFirst; i < nowCount; i++) nowMaxMm = max(nowMaxMm, nowMm[i]);
  nowRainingNow = wet(nowFirst) && (wet(nowFirst + 1) || nowMm[nowFirst] >= 0.5f);

  if (nowRainingNow) {
    int stop = -1;
    for (int i = nowFirst + 1; i < nowCount - 1; i++) if (!wet(i) && !wet(i + 1)) { stop = i; break; }
    nowMsg = stop < 0 ? String("Rain for the next 2 hours") : "Rain stops around " + nowLabel[stop];
  } else {
    for (int i = nowFirst + 1; i < nowCount - 1; i++) if (wet(i) && wet(i + 1)) { nowStartIdx = i; break; }
    nowMsg = nowStartIdx < 0 ? String("Dry for the next 2 hours") : "Rain from " + nowLabel[nowStartIdx];
  }
  if (nowRainingNow || nowStartIdx >= 0) nowMsg += nowMaxMm < 1.0f ? ", light" : (nowMaxMm < 4.0f ? ", moderate" : ", heavy");
  nowShort = nowMsg;

  // Blending, as weather services do: radar decides the first hour (it sees
  // rain that really exists). Further out the model may know better - so if
  // the radar is dry for 2 hours but the model expects rain 1 to 3 hours from
  // now (two wet 15-min steps in a row), say so, clearly marked as forecast.
  // The model never overrides rain the radar sees, and never triggers the LED.
  if (nowRadar && !nowRainingNow && nowStartIdx < 0 && modelValid()) {
    for (int j = 0; j < modCount - 1; j++) {
      int m = minutesUntil(modLabel[j]);
      if (m < 60 || m > 180) continue;
      if (modMm[j] >= WET && modMm[j + 1] >= WET) {
        nowMsg = String(m < 120 ? "Dry for the next hour; rain possible from " : "Dry for the next 2 hours; rain possible from ")
                 + modLabel[j] + " (forecast)";
        nowShort = "Dry, rain possible " + modLabel[j];
        nowBlend = true;
        break;
      }
    }
  }
}

// --- LED rain alert: two slow "breaths" towards blue, once per rain spell.
unsigned long ledPulseStart = 0;
bool ledPulsing = false;
bool rainAlertArmed = true;
const unsigned long LED_PULSE_MS = 2600; // one slow breath

void startLedPulse() { ledPulsing = true; ledPulseStart = millis(); }

void ledPulseLoop() {
  if (!ledPulsing) return;
  unsigned long t = millis() - ledPulseStart;
  if (t >= 2 * LED_PULSE_MS) { ledPulsing = false; setWeatherLED(mainWeather); return; }
  float k = 0.5f - 0.5f * cosf((t % LED_PULSE_MS) * 2.0f * PI / LED_PULSE_MS); // 0 -> 1 -> 0
  int r = currentLedR + (int)((0 - currentLedR) * k);
  int g = currentLedG + (int)((90 - currentLedG) * k);
  int b = currentLedB + (int)((255 - currentLedB) * k);
  int val = max(ledBrightness, 120); // stays visible even when the LED is dimmed right down
  analogWrite(LED_R, 255 - ((r * val) / 255));
  analogWrite(LED_G, 255 - ((g * val) / 255));
  analogWrite(LED_B, 255 - ((b * val) / 255));
}

// Fires once when rain is expected within 30 minutes. It only re-arms after
// the outlook has been clear (no rain within 45 minutes, not raining) again,
// so a forecast that wobbles around the 30-minute mark can't pulse repeatedly.
void checkRainAlert() {
  if (!nowValid) return;
  int mins = (!nowRainingNow && nowStartIdx >= 0) ? minutesUntil(nowLabel[nowStartIdx]) : -1;
  bool soon = mins >= 0 && mins <= 30;
  if (soon && rainAlertArmed) {
    rainAlertArmed = false;
    logDebug("RAIN ALERT: " + nowMsg + (rainLedAlert ? " (LED pulse)" : ""));
    if (rainLedAlert) startLedPulse();
  }
  if (!nowRainingNow && (mins < 0 || mins > 45)) rainAlertArmed = true;
}

// --- Pressure trend (Open-Meteo hourly sea-level pressure, same source for every city)
void pTrendDone(int city, bool ok) {
  if (ok) { pTrendCity = city; pTrendFetchedAt = millis(); pTrendVersion++; }
  else if (pTrendCity != city) { pTrendCity = city; pTrendFetchedAt = 0; pTrendVersion++; }
  pTrendNextFetch = millis() + (ok ? 60UL : 10UL) * 60000UL;
  if (!ok) logDebug("PRESSURE TREND fetch failed: " + pTrendLastError);
}

void pTrendStart(int city) {
  double lat, lon;
  if (!cachedCoords(city, lat, lon)) { pTrendLastError = "no coordinates for the city yet"; pTrendDone(city, false); return; }
  netPost(NJ_PTREND, city, 0, 0, 0, "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4) + "&longitude=" + String(lon, 4)
    + "&hourly=pressure_msl&past_hours=3&forecast_hours=1&timezone=auto");
}

void pTrendResult(NetJob &j) {
  if (j.city != currentCity) return;
  pTrendDone(j.city, parsePressureTrend(j));
}

bool parsePressureTrend(NetJob &j) {
  int city = j.city;
  if (j.code != HTTP_CODE_OK) { pTrendLastError = "Open-Meteo HTTP " + String(j.code); return false; }
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, j.body)) { pTrendLastError = "bad JSON"; return false; }
  JsonArray p = doc["hourly"]["pressure_msl"];
  // Least-squares slope over the hourly points (past 3 hours + now) - steadier
  // than comparing just the first and last value.
  float sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
  for (int i = 0; i < (int)p.size(); i++) {
    if (p[i].isNull()) continue;
    float x = i, y = p[i].as<float>();
    sx += x; sy += y; sxx += x * x; sxy += x * y; n++;
  }
  if (n < 3) { pTrendLastError = "not enough hourly values (" + String(n) + ")"; return false; }
  float slopePerHour = (n * sxy - sx * sy) / (n * sxx - sx * sx);
  float rate = slopePerHour * 3.0f;
  pTrendLevel = pressureTrendLevel(rate, pTrendCity == city ? pTrendLevel : 0);
  pTrendRate = rate;
  pTrendLastError = "";
  return true;
}

// WMO pressure-tendency classes (hPa per 3 h): steady < 1.6, then 1.6-3.5,
// 3.6-6.0, > 6.0. Leaving a class needs the rate to drop 0.6 hPa below the
// class's entry threshold, so a value hovering at a boundary can't make the
// label flip back and forth on every update.
int pressureTrendLevel(float rate, int prev) {
  const float enter[4] = {0, 1.6f, 3.6f, 6.0f};
  float a = fabs(rate);
  int lvl = 0;
  for (int k = 3; k >= 1; k--) if (a >= enter[k]) { lvl = k; break; }
  int pa = abs(prev);
  bool sameDirection = prev != 0 && ((prev > 0) == (rate > 0));
  if (sameDirection && lvl < pa && a >= enter[pa] - 0.6f) lvl = pa; // hold the previous class
  return rate < 0 ? -lvl : lvl;
}

bool pTrendValid() {
  return pTrendCity == currentCity && pTrendFetchedAt != 0 && millis() - pTrendFetchedAt < 2UL * 3600000UL;
}

const char *pressureTrendText(int lvl) {
  static const char *up[] = {"Steady", "Rising", "Rising fast", "Rising very fast"};
  static const char *down[] = {"Steady", "Falling", "Falling fast", "Falling very fast"};
  return lvl >= 0 ? up[min(lvl, 3)] : down[min(-lvl, 3)];
}

// Only the fast classes get an interpretation; "slowly rising/falling" alone
// says too little to promise anything.
const char *pressureTrendHint(int lvl) {
  if (lvl <= -2) return "unsettled, windy weather likely";
  if (lvl >= 2) return "weather likely improving";
  return "";
}

// Call every loop(): keeps the rain outlook (5 min radar / 15 min forecast)
// and the pressure trend (hourly) fresh for the city on screen. At most one
// request per 10 seconds, so the screen and touch stay responsive.
void nowcastLoop() {
  ledPulseLoop();
  static unsigned long lastAttempt = 0, lastAnalyze = 0;
  if (millis() - lastAnalyze > 15000) { lastAnalyze = millis(); analyzeNowcast(); checkRainAlert(); }

  // A finished background request? Parse it here, on the main loop.
  NetJob *jp;
  if (netBusy && xQueueReceive(netDoneQ, &jp, 0) == pdTRUE) {
    netBusy = false;
    if (jp->ms > 8000) logDebug("NET slow: job " + String(jp->kind) + " took " + String(jp->ms) + " ms (HTTP " + String(jp->code) + ")");
    unsigned long pt0 = millis();
    int pkind = jp->kind;
    switch (jp->kind) {
      case NJ_NOW: nowcastResult(*jp); break;
      case NJ_MODEL: modelResult(*jp); break;
      case NJ_PTREND: pTrendResult(*jp); break;
      case NJ_VF_OBS: case NJ_VF_OM: case NJ_VF_OWM: verifyResult(*jp); break;
      case NJ_HIST: histResult(*jp); break;
    }
    if (millis() - pt0 > 500) logDebug("SLOW handling job " + String(pkind) + ": " + String(millis() - pt0) + " ms");
    if (!netBusy) { netJob.body = ""; netJob.url = ""; } // free the reply (unless a follow-up was posted)
  }
  if (netBusy) return;

  if (WiFi.status() != WL_CONNECTED || isUpdating || timeStatus() == timeNotSet) return;
  if (lastAttempt != 0 && millis() - lastAttempt < 3000) return; // a short breather between requests

  if (nowCity != currentCity || (long)(millis() - nowNextFetch) >= 0) { lastAttempt = millis(); nowcastStart(currentCity); return; }
  if (nowRadar && nowCity == currentCity && (modCity != currentCity || (long)(millis() - modNextFetch) >= 0)) {
    lastAttempt = millis(); modelStart(currentCity); return;
  }
  if (verifyLoop()) { lastAttempt = millis(); return; } // forecast verification
  if (pTrendCity != currentCity || (long)(millis() - pTrendNextFetch) >= 0) { lastAttempt = millis(); pTrendStart(currentCity); }
}

// --- [FORECAST VERIFICATION] ---
// Checks afterwards how well each weather source forecast rain for cities
// inside the Buienradar radar area, the way weather services score their own
// forecasts. The unit is a 3-hour block (00-03, 03-06 ... UTC, the same
// blocks OpenWeatherMap forecasts in, so every source is judged the same way):
//  - 1 to 2 hours before a block starts, each source's predicted rain for it
//    is recorded (OpenWeatherMap; Open-Meteo best match and KNMI in one request)
//  - during the block the radar at the city is sampled every 10 minutes
//  - afterwards: "rain" = at least 0.3 mm in the block, for forecast and radar
//    alike, and each source gets a hit / miss / false alarm / correct dry.
// Blocks with too few radar samples (WiFi out, reboot) are skipped, not guessed.
const uint32_t VF_BLOCK = 10800;      // 3 hours
const float VF_WET_MM = 0.3f;         // mm in a block that counts as "it rained"
const int VF_MIN_SAMPLES = 12;        // of ~18 possible radar samples per block
const char *VF_SOURCE_NAMES[VERIFY_SOURCES] = {"OpenWeatherMap", "Open-Meteo", "Open-Meteo KNMI"};

uint32_t vfStart[NUM_CITIES][2];      // two blocks per city: the one being observed + the next one
float vfFc[NUM_CITIES][2][VERIFY_SOURCES];
bool vfFcOK[NUM_CITIES][2][VERIFY_SOURCES];
float vfObsSum[NUM_CITIES][2];
int vfObsN[NUM_CITIES][2];
unsigned long vfLastObs[NUM_CITIES] = {0, 0, 0};

bool vfInRadarArea(int c) {
  return cityGeoCached[c] && cityGeoLocStr[c] == locs[c] && inBuienradarArea(cityLat[c], cityLon[c]);
}

void vfSave(int c) {
  prefs.putBytes(("vfC" + String(c)).c_str(), vfCnt[c], sizeof(vfCnt[c]));
  prefs.putUInt(("vfS" + String(c)).c_str(), vfSince[c]);
  prefs.putString(("vfL" + String(c)).c_str(), vfLoc[c]);
}

void vfReset(int c) {
  memset(vfCnt[c], 0, sizeof(vfCnt[c]));
  vfSince[c] = timeStatus() != timeNotSet ? (uint32_t)now() : 0;
  vfLoc[c] = locs[c];
  vfStart[c][0] = vfStart[c][1] = 0;
  vfSave(c);
}

void vfLoad() {
  for (int c = 0; c < NUM_CITIES; c++) {
    vfLoc[c] = prefs.getString(("vfL" + String(c)).c_str(), "");
    vfSince[c] = prefs.getUInt(("vfS" + String(c)).c_str(), 0);
    if (prefs.getBytes(("vfC" + String(c)).c_str(), vfCnt[c], sizeof(vfCnt[c])) != sizeof(vfCnt[c])) memset(vfCnt[c], 0, sizeof(vfCnt[c]));
  }
}

// Radar rain rate right now at city c (mm/h); reuses the 2-hour outlook when fresh.
// Radar rain rate right now at city c (mm/h) from the 2-hour outlook, when
// it's fresh - saves a request. Otherwise verifyLoop() asks the worker.
bool vfRadarFromNowcast(int c, float &mm) {
  if (c == currentCity && nowRadar && nowCity == c && nowFetchedAt != 0 && millis() - nowFetchedAt < 6UL * 60000UL && nowCount > nowFirst) {
    mm = nowMm[nowFirst];
    return true;
  }
  return false;
}

// Records every source's predicted rain (mm) for the block starting at 'start'.
// Starts recording every source's predicted rain (mm) for the block starting
// at 'start': Open-Meteo first (both models in one request), then OWM.
void vfCapture(int c, int slot, uint32_t start) {
  vfStart[c][slot] = start;
  vfObsSum[c][slot] = 0; vfObsN[c][slot] = 0;
  for (int s = 0; s < VERIFY_SOURCES; s++) { vfFc[c][slot][s] = 0; vfFcOK[c][slot][s] = false; }
  double lat, lon;
  if (!cachedCoords(c, lat, lon)) return;
  netPost(NJ_VF_OM, c, 0, slot, start, "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4) + "&longitude=" + String(lon, 4)
    + "&hourly=precipitation&models=best_match,knmi_seamless&forecast_hours=8&timezone=GMT&timeformat=unixtime");
}

void verifyResult(NetJob &j) {
  int c = j.city, slot = j.slot;
  uint32_t start = j.start;
  if (vfStart[c][slot] != start) return; // that block was already scored or replaced
  if (j.kind == NJ_VF_OBS) {
    float v[3]; String l[3];
    if (j.code == HTTP_CODE_OK && parseRainText(j.body, v, l, 3) >= 1) { vfObsSum[c][slot] += v[0]; vfObsN[c][slot]++; }
    else nowBrPreferred = 1 - nowBrPreferred; // that hostname failed - try the other one next time
    return;
  }
  if (j.kind == NJ_VF_OM) {
    // An hourly value covers the hour before its timestamp, so the block
    // [start, start+3h) is the values at start+1h, +2h and +3h.
    DynamicJsonDocument doc(3072);
    if (j.code == HTTP_CODE_OK && !deserializeJson(doc, j.body)) {
      JsonArray t = doc["hourly"]["time"];
      const char *keys[2] = {"precipitation_best_match", "precipitation_knmi_seamless"};
      for (int k = 0; k < 2; k++) {
        JsonArray p = doc["hourly"][keys[k]];
        float sum = 0; int got = 0;
        for (int i = 0; i < (int)t.size() && i < (int)p.size(); i++) {
          uint32_t ts = t[i].as<uint32_t>();
          if (ts > start && ts <= start + VF_BLOCK && !p[i].isNull()) { sum += p[i].as<float>(); got++; }
        }
        if (got == 3) { vfFc[c][slot][1 + k] = sum; vfFcOK[c][slot][1 + k] = true; }
      }
    }
    double lat, lon;
    if (apiKey.length() > 0 && cachedCoords(c, lat, lon)) {
      netPost(NJ_VF_OWM, c, 0, slot, start, "http://api.openweathermap.org/data/2.5/forecast?lat=" + String(lat, 4) + "&lon=" + String(lon, 4)
        + "&cnt=4&units=metric&appid=" + apiKey);
      return;
    }
  } else if (j.kind == NJ_VF_OWM && j.code == HTTP_CODE_OK) {
    // OpenWeatherMap's 3-hour slices report the rain of the 3 hours before
    // their timestamp, so this block is the slice stamped start+3h.
    DynamicJsonDocument filter(256);
    filter["list"][0]["dt"] = true;
    filter["list"][0]["rain"]["3h"] = true;
    DynamicJsonDocument doc(2048);
    if (!deserializeJson(doc, j.body, DeserializationOption::Filter(filter))) {
      for (JsonObject sl : doc["list"].as<JsonArray>()) {
        if (sl["dt"].as<uint32_t>() == start + VF_BLOCK) { vfFc[c][slot][0] = sl["rain"]["3h"] | 0.0f; vfFcOK[c][slot][0] = true; }
      }
    }
  }
  int ok = vfFcOK[c][slot][0] + vfFcOK[c][slot][1] + vfFcOK[c][slot][2];
  vfStatus[c] = "Recorded " + String(ok) + " forecasts for the next block";
}

// Scores a finished block.
void vfScore(int c, int slot) {
  uint32_t start = vfStart[c][slot];
  vfStart[c][slot] = 0;
  if (vfObsN[c][slot] < VF_MIN_SAMPLES) { vfStatus[c] = "Skipped a block: only " + String(vfObsN[c][slot]) + " radar samples"; return; }
  float obs = vfObsSum[c][slot] / vfObsN[c][slot] * 3.0f; // mean mm/h over the block x 3 h
  bool wet = obs >= VF_WET_MM;
  int scored = 0;
  for (int s = 0; s < VERIFY_SOURCES; s++) {
    if (!vfFcOK[c][slot][s]) continue;
    bool fcWet = vfFc[c][slot][s] >= VF_WET_MM;
    int k = wet ? (fcWet ? 0 : 1) : (fcWet ? 2 : 3); // hit, miss, false alarm, correct dry
    if (vfCnt[c][s][k] < 65000) vfCnt[c][s][k]++;
    scored++;
  }
  if (scored) vfSave(c);
  vfStatus[c] = "Scored block " + String(hour(start)) + "h UTC: radar " + String(obs, 2) + " mm (" + (wet ? "rain" : "dry") + ")";
  logDebug("VERIFY city " + String(c + 1) + ": " + vfStatus[c]);
}

// Called from nowcastLoop(). Starts at most one background request; returns true if it did.
bool verifyLoop() {
  if (timeStatus() == timeNotSet) return false;
  uint32_t t = now();
  uint32_t cur = t - t % VF_BLOCK, next = cur + VF_BLOCK;
  for (int c = 0; c < NUM_CITIES; c++) {
    String loc = locs[c]; loc.trim();
    if (loc.length() == 0) continue;
    if (vfLoc[c] != locs[c]) vfReset(c); // new city: start counting from scratch
    if (!vfInRadarArea(c)) continue;
    if (vfSince[c] == 0) { vfSince[c] = t; vfSave(c); }
    for (int sl = 0; sl < 2; sl++) if (vfStart[c][sl] != 0 && t >= vfStart[c][sl] + VF_BLOCK) vfScore(c, sl);
    int ns = (next / VF_BLOCK) & 1, cs = (cur / VF_BLOCK) & 1;
    if (vfStart[c][ns] != next && t >= next - 7200 && t < next - 3600) { vfCapture(c, ns, next); return netBusy; }
    if (vfStart[c][cs] == cur && millis() - vfLastObs[c] >= 10UL * 60000UL) {
      vfLastObs[c] = millis();
      float mm;
      if (vfRadarFromNowcast(c, mm)) { vfObsSum[c][cs] += mm; vfObsN[c][cs]++; continue; } // no request needed
      double lat, lon;
      if (!cachedCoords(c, lat, lon)) continue;
      return netPost(NJ_VF_OBS, c, 0, cs, cur, nowcastUrl(nowBrPreferred, lat, lon)); // the Buienradar hostname that works
    }
  }
  return false;
}

// JSON summary for one city (used by /api/status for the dashboard card)
String verifyJson(int c) {
  String j = "{\"radar\":" + String(vfInRadarArea(c) ? "true" : "false") + ",\"since\":" + String(vfSince[c]) + ",\"current\":" + String(cityProvider[c]) + ",\"sources\":[";
  for (int s = 0; s < VERIFY_SOURCES; s++) {
    j += String(s ? "," : "") + "{\"name\":\"" + VF_SOURCE_NAMES[s] + "\",\"hits\":" + String(vfCnt[c][s][0]) + ",\"misses\":" + String(vfCnt[c][s][1])
       + ",\"falseAlarms\":" + String(vfCnt[c][s][2]) + ",\"dryOK\":" + String(vfCnt[c][s][3]) + "}";
  }
  return j + "]}";
}

// NEW: builds the <select> dropdown for one city's weather-data provider
String buildProviderSelect(String fieldName, int currentProvider) {
  String html = "<select name='" + fieldName + "'>";
  html += "<option value='0'" + String(currentProvider == PROVIDER_OPENWEATHER ? " selected" : "") + ">OpenWeatherMap</option>";
  html += "<option value='1'" + String(currentProvider == PROVIDER_OPENMETEO_BEST ? " selected" : "") + ">Open-Meteo (Best Match)</option>";
  html += "<option value='2'" + String(currentProvider == PROVIDER_OPENMETEO_KNMI ? " selected" : "") + ">Open-Meteo (KNMI Netherlands)</option>";
  html += "</select>";
  return html;
}

// Settings page (/settings). The page itself is static (SETTINGS_BODY); it
// fills its fields from /api/settings, and saves through /save and /live.
void handleSettingsPage() {
  sendPage("Settings", "settings", false, SETTINGS_BODY);
}

// GET /api/settings - current settings for the Settings page
void handleApiSettings() {
  String j = "{\"cities\":[";
  for (int i = 0; i < NUM_CITIES; i++) j += String(i ? "," : "") + "\"" + jsonEsc(locs[i]) + "\"";
  j += "],\"providers\":[";
  for (int i = 0; i < NUM_CITIES; i++) j += String(i ? "," : "") + String(cityProvider[i]);
  j += "],\"api\":\"" + jsonEsc(apiKey) + "\",\"apiBackup\":\"" + jsonEsc(apiKeyBackup) + "\"";
  j += ",\"theme\":" + String(themeMode);
  j += ",\"animOn\":" + String(showAnimation ? "true" : "false") + ",\"rainPrevOn\":" + String(showRainPreview ? "true" : "false");
  j += ",\"rainLed\":" + String(rainLedAlert ? "true" : "false");
  j += ",\"screenPct\":" + String(map(globalBrightness, 10, 255, 0, 100)) + ",\"ledPct\":" + String(map(ledBrightness, 0, 255, 0, 100));
  j += ",\"useCustom\":" + String(useCustomColor ? "true" : "false");
  j += ",\"hue\":" + String(customHue) + ",\"satPct\":" + String(map(customSat, 0, 255, 0, 100)) + ",\"valPct\":" + String(map(customVal, 0, 255, 0, 100));
  j += ",\"textHue\":" + String(textHue) + ",\"textSatPct\":" + String(map(textSat, 0, 255, 0, 100)) + ",\"textValPct\":" + String(map(textVal, 0, 255, 0, 100));
  j += "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

void handleSave() {
  if (!requireAppHeader()) return;
  // NEW: invalidate the geocode cache for any city whose text actually changed
  if (server.hasArg("c1") && server.arg("c1") != locs[0]) { locs[0] = server.arg("c1"); cityGeoCached[0] = false; }
  if (server.hasArg("c2") && server.arg("c2") != locs[1]) { locs[1] = server.arg("c2"); cityGeoCached[1] = false; }
  if (server.hasArg("c3") && server.arg("c3") != locs[2]) { locs[2] = server.arg("c3"); cityGeoCached[2] = false; }
  if (server.hasArg("api")) apiKey = server.arg("api");
  if (server.hasArg("apiBackup")) apiKeyBackup = server.arg("apiBackup"); // NEW

  // NEW: per-city weather provider
  if (server.hasArg("provider1")) cityProvider[0] = constrain(server.arg("provider1").toInt(), 0, 2);
  if (server.hasArg("provider2")) cityProvider[1] = constrain(server.arg("provider2").toInt(), 0, 2);
  if (server.hasArg("provider3")) cityProvider[2] = constrain(server.arg("provider3").toInt(), 0, 2);
  prefs.putInt("provider0", cityProvider[0]);
  prefs.putInt("provider1", cityProvider[1]);
  prefs.putInt("provider2", cityProvider[2]);

  // NEW: checkboxes are only present in the POST body when checked
  showAnimation = server.hasArg("animOn");
  prefs.putBool("animOn", showAnimation);
  showRainPreview = server.hasArg("rainPrevOn");
  prefs.putBool("rainPrevOn", showRainPreview);
  rainLedAlert = server.hasArg("rainLed");
  prefs.putBool("rainLed", rainLedAlert);
  if (server.hasArg("theme")) {
    themeMode = constrain(server.arg("theme").toInt(), 0, 2);
    prefs.putInt("theme", themeMode); // applied by the triggerWeatherUpdate() below
  }

  prefs.putString("loc1", locs[0]); 
  prefs.putString("loc2", locs[1]); 
  prefs.putString("loc3", locs[2]);
  prefs.putString("api", apiKey);
  prefs.putString("apiBackup", apiKeyBackup); // NEW

  server.send(200, "text/plain", "OK");
  triggerWeatherUpdate();
}

// NEW: live-update endpoint for the web portal's sliders. Applies a change
// immediately (LED color, backlight, sync mode) the moment the browser sends
// it - the web equivalent of dragging a slider on the touchscreen. The
// sync-mode toggle is saved right away and brightness a few seconds after the
// last change (markBrightnessDirty); the custom color itself is only saved
// permanently via handleSaveColor()/"Save Color as Default" below, matching
// the touchscreen's separate "SAVE CUSTOM AS DEFAULT" button.
void handleLive() {
  if (!requireAppHeader()) return;
  String action = server.arg("action");
  int value = server.arg("value").toInt();

  if (action == "hue") {
    customHue = constrain(value, 0, 360);
    applyCustomColor();
  } else if (action == "sat") {
    customSat = map(constrain(value, 0, 100), 0, 100, 0, 255);
    applyCustomColor();
  } else if (action == "val") {
    customVal = map(constrain(value, 0, 100), 0, 100, 0, 255);
    applyCustomColor();
  } else if (action == "screenBright") {
    globalBrightness = map(constrain(value, 0, 100), 0, 100, 10, 255);
    updateBacklight();
    markBrightnessDirty();
  } else if (action == "ledBright") {
    ledBrightness = map(constrain(value, 0, 100), 0, 100, 0, 255);
    setWeatherLED(mainWeather);
    markBrightnessDirty();
  } else if (action == "mode") {
    useCustomColor = (value == 1);
    prefs.putBool("useC", useCustomColor); // matches the touchscreen: mode persists immediately
    setWeatherLED(mainWeather);
  } else if (action == "textHue") {
    textHue = constrain(value, 0, 360);
    applyTextColor();
  } else if (action == "textSat") {
    textSat = map(constrain(value, 0, 100), 0, 100, 0, 255);
    applyTextColor();
  } else if (action == "textVal") {
    textVal = map(constrain(value, 0, 100), 0, 100, 0, 255);
    applyTextColor();
  } else if (action == "provider") {
    // "Use this source" from the dashboard's forecast check card (city on screen)
    cityProvider[currentCity] = constrain(value, 0, 2);
    prefs.putInt(("provider" + String(currentCity)).c_str(), cityProvider[currentCity]);
    triggerWeatherUpdate();
  } else if (action == "testPulse") {
    startLedPulse(); // "Test" button next to the rain-alert setting
  } else if (action == "city") {
    // NEW: lets the web dashboard switch cities too, exactly like the
    // touchscreen's arrows - same triggerWeatherUpdate() call, so it
    // re-fetches and forces the physical screen to redraw as well.
    int newCity = constrain(value, 0, NUM_CITIES - 1);
    if (newCity != currentCity) {
      currentCity = newCity;
      triggerWeatherUpdate();
    }
  }

  server.send(200, "text/plain", "OK");
}

// NEW: persists the current text color (clock minutes + IP address) as the
// boot default - the text-color equivalent of handleSaveColor() below.
void handleSaveTextColor() {
  if (!requireAppHeader()) return;
  prefs.putInt("txHue", textHue);
  prefs.putInt("txSat", textSat);
  prefs.putInt("txVal", textVal);
  server.send(200, "text/plain", "OK");
}

// NEW: persists the current custom color (whatever the live sliders last set
// it to) as the boot default - the web equivalent of the touchscreen's
// "SAVE CUSTOM AS DEFAULT" button. (Screen/LED brightness don't need a save
// button - they're saved automatically a few seconds after you change them.)
void handleSaveColor() {
  if (!requireAppHeader()) return;
  saveCustomColor();
  server.send(200, "text/plain", "OK");
}

// --- shared page frame for every web page: top bar (desktop), tab bar (phone),
// theme button, and the shared stylesheet/script. The asset URLs carry a
// build hash, so browsers cache them until a new sketch is uploaded.
String assetVersion() {
  static String v;
  if (!v.length()) {
    uint32_t h = 2166136261u;
    for (const char *p = __DATE__ " " __TIME__; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    v = String(h, HEX);
  }
  return v;
}

String pageStart(const char *title, const char *current, bool cityTabs) {
  struct { const char *id, *href, *label; } links[] = {
    {"dashboard", "/", "Dashboard"},
#if USE_SD_HISTORY
    {"history", "/history", "History"},
#endif
    {"settings", "/settings", "Settings"}, {"debug", "/debug", "Debug"}};
  String h = "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>"
             "<meta name='theme-color' content='#0b1220'>";
  h += "<title>" + String(title) + " - Weather Station</title>";
  h += "<link rel='stylesheet' href='/app.css?v=" + assetVersion() + "'>";
  h += "<script>window._txt=[" + String(textR) + "," + String(textG) + "," + String(textB) + "];</script>";
  h += "<script src='/app.js?v=" + assetVersion() + "'></script></head><body>";
  h += "<header class='top'><a class='brand' href='/'><i id='brandDot'" + String(lastFetchOK ? "" : " class='bad'") + "></i>Weather Station</a>";
  if (cityTabs) h += "<div class='tabs' id='cityTabs'></div>";
  h += "<nav>";
  for (auto &l : links) h += "<a href='" + String(l.href) + "'" + (strcmp(l.id, current) == 0 ? " class='on'" : "") + ">" + l.label + "</a>";
  h += "</nav><button class='iconbtn' id='themeBtn' onclick='cycleTheme()' aria-label='Theme'></button></header>";
  return h;
}

String pageEnd(const char *current) {
  struct { const char *id, *href, *label, *icon; } tabs[] = {
    {"dashboard", "/", "Dashboard", "<path d='M3 11l9-7 9 7v9H5v-9'/>"},
#if USE_SD_HISTORY
    {"history", "/history", "History", "<path d='M4 19V9M10 19V5M16 19v-7M22 19H2'/>"},
#endif
    {"settings", "/settings", "Settings", "<circle cx='12' cy='12' r='3'/><path d='M12 2v3M12 19v3M2 12h3M19 12h3M5 5l2 2M17 17l2 2M5 19l2-2M17 7l2-2'/>"},
    {"debug", "/debug", "Debug", "<path d='M4 17l6-6-6-6M12 19h8'/>"}};
  String h = "<nav class='bottom'>";
  for (auto &t : tabs)
    h += "<a href='" + String(t.href) + "'" + (strcmp(t.id, current) == 0 ? " class='on'" : "") +
         "><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>" + t.icon + "</svg>" + t.label + "</a>";
  h += "</nav><script>applyThemePref();</script></body></html>";
  return h;
}

// Sends a page in chunks: shared header + body (from flash) + shared footer.
void sendPage(const char *title, const char *current, bool cityTabs, const char *body, const char *body2) {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  server.sendContent(pageStart(title, current, cityTabs));
  server.sendContent_P(body);
  if (body2) server.sendContent_P(body2);
  server.sendContent(pageEnd(current));
  server.sendContent("");
}

void handleAppCss() {
  server.sendHeader("Cache-Control", "public, max-age=31536000");
  server.send_P(200, "text/css", APP_CSS);
}

void handleAppJs() {
  server.sendHeader("Cache-Control", "public, max-age=31536000");
  server.send_P(200, "application/javascript", APP_JS);
}

// NEW: JSON snapshot of live touch/system diagnostics for the /debug console.
void handleApiDebug() {
  String json = "{";
  json += "\"rawX\":" + String(debugRawX) + ",\"rawY\":" + String(debugRawY) + ",";
  json += "\"pressed\":" + String(debugPressed ? "true" : "false") + ",";
  json += "\"isTouched\":" + String(isTouched ? "true" : "false") + ",";
  json += "\"touchHandled\":" + String(touchHandled ? "true" : "false") + ",";
  json += "\"msHeld\":" + String(isTouched ? (millis() - touchStartTime) : 0) + ",";
  json += "\"stuckResets\":" + String(debugStuckResetCount) + ",";
  json += "\"displayMode\":" + String(displayMode) + ",";
  json += "\"uptimeSec\":" + String(millis() / 1000) + ",";
  json += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"minHeap\":" + String(ESP.getMinFreeHeap()) + ",\"loopMaxMs\":" + String(loopMaxMs) + ",";
  json += "\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  // Data sources: what each feed last returned, so feeds can be checked on the device
  auto age = [](unsigned long t) { return t == 0 ? String("never") : String((millis() - t) / 60000) + " min ago"; };
  analyzeNowcast();
  json += "\"sources\":[";
  json += "{\"name\":\"Weather\",\"ok\":" + String(lastFetchOK ? "true" : "false") + ",\"age\":\"" + age(lastFetchMillis)
        + "\",\"info\":\"" + jsonEsc(getDisplayCity(locs[currentCity]) + ", " + String(tempnum, 1) + " C, " + weatherDesc) + "\"},";
  json += "{\"name\":\"Rain next 2h\",\"ok\":" + String(nowValid ? "true" : "false") + ",\"age\":\"" + age(nowFetchedAt)
        + "\",\"info\":\"" + jsonEsc(nowValid ? nowSource + ": " + String(nowCount - nowFirst) + " steps, max " + String(nowMaxMm, 2) + " mm/h. " + nowMsg
                                              : (nowLastError.length() ? nowLastError : String("waiting for first fetch"))) + "\"},";
  json += "{\"name\":\"Pressure trend\",\"ok\":" + String(pTrendValid() ? "true" : "false") + ",\"age\":\"" + age(pTrendFetchedAt)
        + "\",\"info\":\"" + jsonEsc(pTrendValid() ? String(pTrendRate, 2) + " hPa/3h: " + pressureTrendText(pTrendLevel)
                                                      : (pTrendLastError.length() ? pTrendLastError : String("waiting for first fetch"))) + "\"},";
  json += "{\"name\":\"Rain model (blending)\",\"ok\":" + String(nowRadar && modelValid() ? "true" : "false") + ",\"age\":\"" + age(modFetchedAt)
        + "\",\"info\":\"" + jsonEsc(nowRadar ? (modelValid() ? "Open-Meteo 15-min: " + String(modCount) + " steps" + (nowBlend ? ". Model expects rain the radar doesn't see yet" : "") : String("waiting for first fetch"))
                                               : String("only used next to radar (NL/BE)")) + "\"},";
  json += "{\"name\":\"Forecast check\",\"ok\":" + String(vfInRadarArea(currentCity) ? "true" : "false") + ",\"age\":\"-\",\"info\":\""
        + jsonEsc(vfInRadarArea(currentCity) ? (vfStatus[currentCity].length() ? vfStatus[currentCity] : String("waiting for the next 3-hour block"))
                                             : String("needs radar - only for cities in the Netherlands/Belgium")) + "\"}";
  json += "],";
  json += "\"log\":[";
  int n = debugLogCount;
  for (int i = 0; i < n; i++) {
    int idx = (debugLogIndex - 1 - i + DEBUG_LOG_SIZE) % DEBUG_LOG_SIZE; // newest first
    if (i > 0) json += ",";
    json += "\"" + jsonEsc(debugLog[idx]) + "\"";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// Live debug console: what the touch controller reports, plus the event log,
// without needing a Serial Monitor connection. Polls /api/debug every 400ms.
void handleDebugPage() {
  sendPage("Debug", "debug", false, DEBUG_BODY);
}

void handleApiStatus() {
  time_t localT = now() + tzOffsetSec[currentCity];
  unsigned long ageMin = lastFetchMillis == 0 ? 0 : (millis() - lastFetchMillis) / 60000;
  SunCountdown sc = computeSunCountdown();
  int moonIdx = moonPhaseIndex(moonPhaseFraction());

  String json = "{";
  json += "\"cityIndex\":" + String(currentCity) + ",";
  json += "\"cityNames\":[\"" + jsonEsc(getDisplayCity(locs[0])) + "\",\"" + jsonEsc(getDisplayCity(locs[1])) + "\",\"" + jsonEsc(getDisplayCity(locs[2])) + "\"],";
  json += "\"city\":\"" + jsonEsc(getDisplayCity(locs[currentCity])) + "\",";
  json += "\"weatherDesc\":\"" + jsonEsc(weatherDesc) + "\",";
  json += "\"main\":\"" + jsonEsc(mainWeather) + "\",";
  json += "\"visibility\":" + String(visibility) + ",\"clouds\":" + String(clouds) + ",";
  json += "\"night\":" + String(isNightNow() ? "true" : "false") + ",\"deviceDark\":" + String(themeDark ? "true" : "false") + ",";
  static const char *PROVIDER_NAMES[] = {"OpenWeatherMap", "Open-Meteo", "Open-Meteo KNMI"};
  json += "\"provider\":\"" + String(PROVIDER_NAMES[constrain(cityProvider[currentCity], 0, 2)]) + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  analyzeNowcast();
  if (nowValid) {
    json += "\"nowcast\":{\"valid\":true,\"radar\":" + String(nowRadar ? "true" : "false") + ",\"stepMin\":" + String(nowStepMin)
          + ",\"ageMin\":" + String((millis() - nowFetchedAt) / 60000) + ",\"msg\":\"" + jsonEsc(nowMsg) + "\",\"steps\":[";
    for (int i = nowFirst; i < nowCount; i++) {
      float m = nowRadar ? modelAt(nowLabel[i]) : -1;
      json += String(i > nowFirst ? "," : "") + "[\"" + nowLabel[i] + "\"," + String(nowMm[i], 2) + "," + (m < 0 ? String("null") : String(m, 2)) + "]";
    }
    json += "],\"blend\":" + String(nowBlend ? "true" : "false") + ",\"model\":" + String(nowRadar && modelValid() ? "true" : "false") + "},";
  } else json += "\"nowcast\":{\"valid\":false},";
  if (pTrendValid()) json += "\"pTrend\":{\"valid\":true,\"rate\":" + String(pTrendRate, 1) + ",\"level\":" + String(pTrendLevel)
                           + ",\"text\":\"" + pressureTrendText(pTrendLevel) + "\",\"hint\":\"" + pressureTrendHint(pTrendLevel) + "\"},";
  else json += "\"pTrend\":{\"valid\":false},";
  json += "\"verify\":" + verifyJson(currentCity) + ",";
  json += "\"temp\":" + String(tempnum, 1) + ",";
  json += "\"feelsLike\":" + String(feels_like, 1) + ",";
  json += "\"tempMin\":" + String(tempMin, 1) + ",";
  json += "\"tempMax\":" + String(tempMax, 1) + ",";
  json += "\"humid\":" + String(huminum) + ",";
  json += "\"pressure\":" + String(pressure) + ",";
  json += "\"windSpeed\":" + String(windSpeedKmH, 1) + ",";
  json += "\"windDir\":\"" + jsonEsc(windDir) + "\",";
  json += "\"sunRise\":\"" + jsonEsc(sunRise) + "\",";
  json += "\"sunSet\":\"" + jsonEsc(sunSet) + "\",";
  json += "\"sunEvent\":\"" + sc.eventName + "\",";
  json += "\"sunDiffMin\":" + String(sc.diffMin) + ",";
  json += "\"moonPhase\":\"" + getMoonPhaseName(moonIdx) + "\",";
  json += "\"southern\":" + String(cityGeoCached[currentCity] && cityLat[currentCity] < 0 ? "true" : "false") + ",";
  json += "\"hour\":" + String(hour(localT)) + ",";
  json += "\"minute\":" + String(minute(localT)) + ",";
  json += "\"dateStr\":\"" + String(day(localT)) + "/" + String(month(localT)) + "/" + String(year(localT)) + "\",";
  json += "\"dateLong\":\"" + String(DAY_NAMES[weekday(localT) - 1]) + " " + String(day(localT)) + " " + MONTH_NAMES[month(localT) - 1] + " " + String(year(localT)) + "\",";
  json += "\"ledR\":" + String(currentLedR) + ",\"ledG\":" + String(currentLedG) + ",\"ledB\":" + String(currentLedB) + ",";
  json += "\"textR\":" + String(textR) + ",\"textG\":" + String(textG) + ",\"textB\":" + String(textB) + ",";
  json += "\"lastFetchOK\":" + String(lastFetchOK ? "true" : "false") + ",";
  json += "\"ageMin\":" + String(ageMin) + ",";
#if USE_BME280
  json += "\"bmeOK\":" + String(bmeOK ? "true" : "false") + ",";
  json += "\"localTemp\":" + String(localTemp, 1) + ",";
  json += "\"localHumid\":" + String(localHumid, 1) + ",";
  json += "\"localPressure\":" + String(localPressure, 1) + ",";
#else
  json += "\"bmeOK\":false,";
#endif
  json += "\"forecast\":[";
  for (int i = 0; i < 5; i++) {
    if (i > 0) json += ",";
    if (forecast[i].valid) {
      json += "{\"day\":\"" + forecast[i].dayName + "\",\"low\":" + String(forecast[i].low, 1) + ",\"high\":" + String(forecast[i].high, 1) + ",\"main\":\"" + jsonEsc(forecast[i].main) + "\"}";
    } else {
      json += "{\"day\":\"\",\"low\":0,\"high\":0,\"main\":\"\"}";
    }
  }
  json += "],";
  json += "\"rain\":[";
  for (int i = 0; i < upcomingRainCount; i++) {
    if (i > 0) json += ",";
    json += "{\"mm\":" + String(upcomingRainMm[i], 1) + ",\"label\":\"" + jsonEsc(upcomingRainLabel[i]) + "\"}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// Live dashboard (the start page): polls /api/status every 7 seconds.
void handleDashboardPage() {
  sendPage("Dashboard", "dashboard", true, DASHBOARD_BODY);
}

// --- [PER-CITY HISTORY (SD CARD)] ---
// Each city has its own log: /hist_c0.csv, /hist_c1.csv, /hist_c2.csv
// Columns: date,tMin,tMax,rainMm,humidAvg,pressureAvg,gustMaxKmh,sunHours
// (rows from older versions have only the first 6 - they're upgraded with
// wind/sun automatically when a sync or backfill covers that date).
//
// - Sync (all three cities, whichever is on screen): every 6 hours the last
//   7 days are fetched, and once a day the last 92 days, from Open-Meteo.
//   Missing days get filled, the last 3 days are refreshed, older rows are
//   never overwritten.
// - Backfill (on request, from the web History page): goes back up to 10
//   years using Open-Meteo's historical archive, one 90-day chunk at a time
//   in the background. Only fills missing days. Survives a reboot.
// - /hist_cN.loc remembers which location a file belongs to - if a city is
//   renamed, the old file is kept as /hist_cN_oldX.csv and a fresh history starts.
// - The BME280 (if fitted) is logged hourly to /local.csv.
// No day limit anywhere: files are streamed, never loaded into RAM whole.
#if USE_SD_HISTORY

const unsigned long HIST_SYNC_INTERVAL_MS   = 6UL * 3600UL * 1000UL;  // light sync (last 7 days) every 6h
const unsigned long HIST_FULL_SYNC_MS       = 24UL * 3600UL * 1000UL; // full 92-day sync once a day
const unsigned long HIST_RETRY_MS           = 10UL * 60UL * 1000UL;   // retry a failed sync after 10 min
const unsigned long HIST_MIN_GAP_MS         = 15000;                  // at most one network job per 15s (keeps the UI responsive)
const unsigned long HIST_STORAGE_REFRESH_MS = 30UL * 60UL * 1000UL;
const int HIST_PAST_DAYS      = 92; // Open-Meteo forecast API's maximum look-back
const int HIST_SHORT_DAYS     = 7;
const int HIST_OVERWRITE_DAYS = 3;  // the most recent N finished days are always refreshed
const int HIST_MAX_FETCH      = HIST_PAST_DAYS + 1; // + today
const int HIST_BF_CHUNK_DAYS  = 90; // backfill request size (must be <= HIST_MAX_FETCH)
const int HIST_BF_MAX_FAILS   = 5;
const char* HIST_DAILY_VARS = "temperature_2m_min,temperature_2m_max,precipitation_sum,relative_humidity_2m_mean,pressure_msl_mean,wind_gusts_10m_max,sunshine_duration";
const char* LOCAL_LOG = "/local.csv";

String histMetaLoc[NUM_CITIES];
unsigned long histNextSync[NUM_CITIES] = {0, 0, 0}; // millis() when due (0 = as soon as possible)
unsigned long histLastFull[NUM_CITIES] = {0, 0, 0};
bool histFullDone[NUM_CITIES] = {false, false, false};
unsigned long histLastAttempt = 0;
unsigned long histLastStorageRefresh = 0;
uint32_t histCardTotalKB = 0, histCardUsedKB = 0;

// Today's values per city (a forecast for the whole day, shown on the page
// as the last, highlighted point; never written to file)
bool histTodayValid[NUM_CITIES] = {false, false, false};
String histTodayDate[NUM_CITIES];
float histTodayMin[NUM_CITIES], histTodayMax[NUM_CITIES], histTodayRain[NUM_CITIES];
float histTodayHum[NUM_CITIES], histTodayPres[NUM_CITIES], histTodayGust[NUM_CITIES], histTodaySun[NUM_CITIES];
bool histTodayGustOK[NUM_CITIES], histTodaySunOK[NUM_CITIES];

// Backfill state (persisted in Preferences so it resumes after a reboot)
bool bfActive[NUM_CITIES] = {false, false, false};
String bfStart[NUM_CITIES];  // oldest date wanted
String bfCursor[NUM_CITIES]; // end date of the next chunk to fetch
String bfEnd[NUM_CITIES];    // where this backfill started (for the progress %)
String bfMsg[NUM_CITIES];
int bfFails[NUM_CITIES] = {0, 0, 0};

// Scratch space for one fetch (filled by histParseDaily, used by the merge)
char  hfDate[HIST_MAX_FETCH][11];
float hfMin[HIST_MAX_FETCH], hfMax[HIST_MAX_FETCH], hfRain[HIST_MAX_FETCH];
float hfHum[HIST_MAX_FETCH], hfPres[HIST_MAX_FETCH], hfGust[HIST_MAX_FETCH], hfSun[HIST_MAX_FETCH];
bool  hfValid[HIST_MAX_FETCH], hfGustOK[HIST_MAX_FETCH], hfSunOK[HIST_MAX_FETCH];
int   hfCount = 0;
String hkLine[HIST_MAX_FETCH];
bool   hkPresent[HIST_MAX_FETCH];

String histPath(int i)     { return "/hist_c" + String(i) + ".csv"; }
String histMetaPath(int i) { return "/hist_c" + String(i) + ".loc"; }

String histTrimmedLoc(int i) { String l = locs[i]; l.trim(); return l; }

bool histClockOK() { return timeStatus() != timeNotSet && now() > 1600000000; }

// --- date helpers ("YYYY-MM-DD" strings compare correctly as plain text)
String histDateStr(time_t t) {
  char b[11];
  snprintf(b, sizeof(b), "%04d-%02d-%02d", year(t), month(t), day(t));
  return String(b);
}
time_t histDateToTime(String d) {
  tmElements_t e;
  e.Year = d.substring(0, 4).toInt() - 1970; e.Month = d.substring(5, 7).toInt(); e.Day = d.substring(8, 10).toInt();
  e.Hour = 12; e.Minute = 0; e.Second = 0;
  return makeTime(e);
}
String histAddDays(String d, int days) { return histDateStr(histDateToTime(d) + (long)days * 86400L); }

// Splits a CSV row into at most maxF fields; returns the number found.
int histSplit(String line, String *f, int maxF) {
  int n = 0, from = 0;
  while (n < maxF) {
    int p = line.indexOf(',', from);
    if (p < 0) { f[n++] = line.substring(from); break; }
    f[n++] = line.substring(from, p);
    from = p + 1;
  }
  return n;
}


bool histFieldOK(String s) {
  if (s.length() == 0) return false;
  for (unsigned int k = 0; k < s.length(); k++) {
    char c = s[k];
    if (!((c >= '0' && c <= '9') || c == '-' || c == '.')) return false; // rejects nan/inf, which would break the JSON
  }
  return true;
}

// Reads a text file line by line in 2 KB blocks. File::readStringUntil()
// fetches ONE byte per call from the SD card (each one a full trip through the
// filesystem layers) and grows the String a character at a time - with years
// of history that's what made the History page take ~10+ seconds to load.
class LineReader {
 public:
  explicit LineReader(File &file) : f(file), buf((uint8_t *)malloc(BUF_SIZE)) {}
  ~LineReader() { free(buf); }
  // Next line into out, without the line ending. Returns false at the end of the file.
  bool next(String &out) {
    out = "";
    bool got = false;
    while (buf) {
      if (pos >= len) {
        len = f.read(buf, BUF_SIZE);
        pos = 0;
        if (len == 0) return got;
      }
      uint8_t *start = buf + pos;
      uint8_t *nl = (uint8_t *)memchr(start, '\n', len - pos);
      size_t n = nl ? (size_t)(nl - start) : len - pos;
      if (out.length() < MAX_LINE) out.concat((const char *)start, n);
      got = true;
      pos += n + (nl ? 1 : 0);
      if (nl) { if (out.endsWith("\r")) out.remove(out.length() - 1); return true; }
    }
    return false;
  }
 private:
  static const size_t BUF_SIZE = 2048, MAX_LINE = 512;
  File &f;
  uint8_t *buf;
  size_t len = 0, pos = 0;
  LineReader(const LineReader &) = delete;
  LineReader &operator=(const LineReader &) = delete;
};

// Sends the rest of an open file to the client as-is, in 4 KB blocks.
void histStreamFileBody(File &f) {
  const size_t BLOCK = 4096;
  uint8_t *b = (uint8_t *)malloc(BLOCK);
  if (!b) return;
  WiFiClient client = server.client();
  size_t n;
  while ((n = f.read(b, BLOCK)) > 0) {
    if (client.write(b, n) != n) break; // browser went away
  }
  free(b);
}

// Each city's file gets a version number that goes up whenever the file is
// written. Together with a per-boot random id it forms the file's ETag, so a
// browser that already has the data gets a tiny "304 Not Modified" instead of
// downloading the whole file again.
uint32_t histBootId = 0;
uint32_t histGen[NUM_CITIES] = {0, 0, 0};
void histTouch(int i) { histGen[i]++; }
uint32_t histEpoch[NUM_CITIES] = {0, 0, 0}; // bumped when a city's log is renamed, wiped or imported
String histEtag(int i, size_t size) {
  return "\"" + String(histBootId, HEX) + "-" + String(histGen[i]) + "-" + String((unsigned long)size) + "\"";
}

String histReadMeta(int i) {
  String p = histMetaPath(i);
  if (!SD.exists(p)) return "";
  File f = SD.open(p, FILE_READ);
  if (!f) return "";
  String s = f.readStringUntil('\n');
  f.close();
  s.trim();
  return s;
}

void histWriteMeta(int i, String loc) {
  File f = SD.open(histMetaPath(i), FILE_WRITE);
  if (f) { f.println(loc); f.close(); }
  histMetaLoc[i] = loc;
}

// Moves a city's log aside (never deletes it) so a new location starts clean.
void histArchive(int i) {
  histMergeAbort(i);
  histEpoch[i]++;
  String src = histPath(i);
  if (!SD.exists(src)) return;
  int n = 1;
  String dst;
  do { dst = "/hist_c" + String(i) + "_old" + String(n) + ".csv"; n++; } while (SD.exists(dst));
  SD.rename(src, dst);
  histTouch(i);
  logDebug("HIST city " + String(i + 1) + " renamed - old log kept as " + dst);
}

void histBackfillSave(int i) {
  prefs.putString(("bfS" + String(i)).c_str(), bfActive[i] ? bfStart[i] : String(""));
  prefs.putString(("bfC" + String(i)).c_str(), bfActive[i] ? bfCursor[i] : String(""));
  prefs.putString(("bfE" + String(i)).c_str(), bfActive[i] ? bfEnd[i] : String(""));
}

void histBackfillStop(int i, String msg) {
  bfActive[i] = false;
  bfMsg[i] = msg;
  histBackfillSave(i);
}

void histBackfillStart(int i, int years) {
  String today = histDateStr(now());
  String start = histAddDays(today, -(years * 365 + years / 4)); // + leap days
  if (bfActive[i] && bfCursor[i].length() == 10) {
    if (start < bfStart[i]) bfStart[i] = start; // already running - just reach further back
  } else {
    bfEnd[i] = histAddDays(today, -6); // the archive lags ~5 days; newer days come from the regular sync
    bfCursor[i] = bfEnd[i];
    bfStart[i] = start;
  }
  bfActive[i] = true;
  bfFails[i] = 0;
  bfMsg[i] = "Queued";
  histBackfillSave(i);
}

// Detects a city being renamed in Settings (cheap, runs every couple of seconds).
void histCheckLocations() {
  for (int i = 0; i < NUM_CITIES; i++) {
    String l = histTrimmedLoc(i);
    if (l.length() == 0 || l == histMetaLoc[i]) continue;
    if (histMetaLoc[i].length() > 0) histArchive(i); // a different place was logged here before
    histWriteMeta(i, l);
    histTodayValid[i] = false;
    histFullDone[i] = false;
    histNextSync[i] = 0; // fetch the new city's history right away
    if (bfActive[i]) histBackfillStop(i, "Stopped - city was renamed");
  }
}

void histRefreshStorage() {
  histCardTotalKB = (uint32_t)(SD.totalBytes() / 1024ULL);
  histCardUsedKB  = (uint32_t)(SD.usedBytes() / 1024ULL);
  histLastStorageRefresh = millis();
}

// Call once in setup(), after the city names are loaded and WiFi is up.
void historySetup() {
  if (!sdOK) return;

  // One-time migration: the original single log only ever recorded City 1.
  if (SD.exists("/history.csv") && !SD.exists(histPath(0))) {
    SD.rename("/history.csv", histPath(0));
    histWriteMeta(0, histTrimmedLoc(0));
    logDebug("HIST migrated /history.csv to City 1");
  }

  for (int i = 0; i < NUM_CITIES; i++) {
    histMetaLoc[i] = histReadMeta(i);
    bfStart[i]  = prefs.getString(("bfS" + String(i)).c_str(), "");
    bfCursor[i] = prefs.getString(("bfC" + String(i)).c_str(), "");
    bfEnd[i]    = prefs.getString(("bfE" + String(i)).c_str(), "");
    bfActive[i] = bfStart[i].length() == 10 && bfCursor[i].length() == 10;
    if (bfActive[i]) bfMsg[i] = "Resuming after restart";
  }
  histCheckLocations();
  histRefreshStorage();
}

// Formats fetched day k as a CSV row (unknown wind/sun are left empty).
void histFormatRow(int k, char *buf, size_t len) {
  char g[12] = "", s[12] = "";
  if (hfGustOK[k]) snprintf(g, sizeof(g), "%.0f", hfGust[k]);
  if (hfSunOK[k]) snprintf(s, sizeof(s), "%.1f", hfSun[k]);
  snprintf(buf, len, "%s,%.1f,%.1f,%.1f,%.0f,%.0f,%s,%s", hfDate[k], hfMin[k], hfMax[k], hfRain[k], hfHum[k], hfPres[k], g, s);
}

// An existing row is kept as-is, except that missing wind/sun columns are
// added from fetched day k when available.
String histUpgradeLine(String line, int k) {
  String f[8];
  int n = histSplit(line, f, 8);
  if (n < 6) return line;
  bool hasG = n > 6 && f[6].length() > 0, hasS = n > 7 && f[7].length() > 0;
  if ((hasG || !hfGustOK[k]) && (hasS || !hfSunOK[k])) return line;
  char g[12] = "", s[12] = "";
  if (hasG) f[6].toCharArray(g, sizeof(g)); else if (hfGustOK[k]) snprintf(g, sizeof(g), "%.0f", hfGust[k]);
  if (hasS) f[7].toCharArray(s, sizeof(s)); else if (hfSunOK[k]) snprintf(s, sizeof(s), "%.1f", hfSun[k]);
  return f[0] + "," + f[1] + "," + f[2] + "," + f[3] + "," + f[4] + "," + f[5] + "," + String(g) + "," + String(s);
}

int histFindDay(String d, int n) {
  for (int k = 0; k < n; k++) if (d == hfDate[k]) return k;
  return -1;
}

// Merges fetched days hf[0..n-1] (consecutive, oldest first) into city i's file,
// keeping it sorted. Days at index >= overwriteFrom replace existing rows;
// earlier days only fill gaps. Rows dated >= dropFrom ("" = none) are dropped.
// Streams through the file, so it doesn't matter how big it gets.
// Merging fetched days into a city's file streams through the whole file
// (it's kept sorted, and a backfill adds days at the front), which takes
// seconds once there are years of history. So it runs in slices of ~25 ms
// per loop() pass instead of all at once: the screen and web pages keep
// running while it works. The new file is written next to the old one and
// only swapped in at the end, so readers always see a complete file.
bool hmActive = false;
int hmCity = -1, hmN = 0, hmOverwriteFrom = 0;
String hmDropFrom, hmWinStart, hmWinEnd;
File hmIn, hmOut;
LineReader *hmReader = nullptr;
bool hmFlushed = false;

void histMergeFlushWindow() {
  if (hmFlushed) return;
  hmFlushed = true;
  char buf[100];
  for (int k = 0; k < hmN; k++) {
    if (hkPresent[k]) hmOut.println(hkLine[k]);
    else if (hfValid[k]) { histFormatRow(k, buf, sizeof(buf)); hmOut.println(buf); }
    hkLine[k] = ""; // free memory
  }
}

void histMergeClose() {
  if (hmReader) { delete hmReader; hmReader = nullptr; }
  if (hmIn) hmIn.close();
  if (hmOut) hmOut.close();
  hmActive = false;
}

// Merges fetched days hf[0..n-1] (consecutive, oldest first) into city i's file,
// keeping it sorted. Days at index >= overwriteFrom replace existing rows;
// earlier days only fill gaps. Rows dated >= dropFrom ("" = none) are dropped.
// Starts the merge; histMergeStep() does the work and calls histMergeDone().
bool histMergeBegin(int i, int n, int overwriteFrom, String dropFrom) {
  if (hmActive || n <= 0) return false;
  hmCity = i; hmN = n; hmOverwriteFrom = overwriteFrom; hmDropFrom = dropFrom;
  hmWinStart = String(hfDate[0]); hmWinEnd = String(hfDate[n - 1]);
  for (int k = 0; k < n; k++) { hkPresent[k] = false; hkLine[k] = ""; }
  String tmp = "/hist_c" + String(i) + ".tmp";
  if (SD.exists(tmp)) SD.remove(tmp);
  hmOut = SD.open(tmp, FILE_WRITE);
  if (!hmOut) return false;
  if (SD.exists(histPath(i))) {
    hmIn = SD.open(histPath(i), FILE_READ);
    if (hmIn) hmReader = new LineReader(hmIn);
  }
  hmFlushed = false;
  hmActive = true;
  return true;
}

// Stops a running merge for city i (or any city with -1) and throws the half-written file away.
void histMergeAbort(int i) {
  if (!hmActive || (i >= 0 && hmCity != i)) return;
  int c = hmCity;
  histMergeClose();
  String tmp = "/hist_c" + String(c) + ".tmp";
  if (SD.exists(tmp)) SD.remove(tmp);
  histEpoch[c]++; // a fetch in flight for this city is now outdated too
  histMergeDone(false, true);
}

// Called from historyLoop() while a merge is running.
void histMergeStep() {
  if (!hmActive) return;
  unsigned long t0 = millis();
  String line;
  while (hmReader && millis() - t0 < 25) {
    if (!hmIn || !hmReader->next(line)) { delete hmReader; hmReader = nullptr; break; }
    line.trim();
    if (line.length() < 11) continue;
    String d = line.substring(0, 10);
    if (hmDropFrom.length() == 10 && d >= hmDropFrom) continue;
    if (d < hmWinStart) { hmOut.println(line); continue; }
    if (d > hmWinEnd) { histMergeFlushWindow(); hmOut.println(line); continue; }
    int k = histFindDay(d, hmN);
    if (k < 0) continue;
    if (k >= hmOverwriteFrom && hfValid[k]) continue; // replaced by the fresh value
    hkPresent[k] = true;
    hkLine[k] = histUpgradeLine(line, k);
  }
  if (hmReader) return; // more next time

  histMergeFlushWindow();
  int i = hmCity;
  histMergeClose();
  String path = histPath(i), tmp = "/hist_c" + String(i) + ".tmp";
  if (SD.exists(path)) SD.remove(path);
  histTouch(i);
  histMergeDone(SD.rename(tmp, path), false);
}

// Parses an Open-Meteo daily response into hf*. Returns the number of days, or -1.
int histParseDaily(const String &payload, String who) {
  DynamicJsonDocument doc(32768);
  if (deserializeJson(doc, payload)) { logDebug("HIST " + who + ": bad JSON"); return -1; }

  JsonArray dTime = doc["daily"]["time"];
  JsonArray dMin  = doc["daily"]["temperature_2m_min"];
  JsonArray dMax  = doc["daily"]["temperature_2m_max"];
  JsonArray dRain = doc["daily"]["precipitation_sum"];
  JsonArray dHum  = doc["daily"]["relative_humidity_2m_mean"];
  JsonArray dPres = doc["daily"]["pressure_msl_mean"];
  JsonArray dGust = doc["daily"]["wind_gusts_10m_max"];
  JsonArray dSun  = doc["daily"]["sunshine_duration"];

  int n = min((int)dTime.size(), HIST_MAX_FETCH);
  for (int k = 0; k < n; k++) {
    strlcpy(hfDate[k], dTime[k] | "", sizeof(hfDate[k]));
    hfValid[k] = strlen(hfDate[k]) == 10 && !dMin[k].isNull() && !dMax[k].isNull();
    hfMin[k]  = dMin[k]  | 0.0f;
    hfMax[k]  = dMax[k]  | 0.0f;
    hfRain[k] = dRain[k] | 0.0f;
    hfHum[k]  = dHum[k]  | 0.0f;
    hfPres[k] = dPres[k] | 0.0f;
    hfGustOK[k] = !dGust[k].isNull();
    hfGust[k] = dGust[k] | 0.0f;
    hfSunOK[k] = !dSun[k].isNull();
    hfSun[k] = (dSun[k] | 0.0f) / 3600.0f; // seconds -> hours
  }
  hfCount = n;
  return n;
}

// Regular sync for one city: the last pastDays finished days + today's forecast.
// What the history download/merge in progress is for (one at a time).
bool histJobPending = false;  // a download is with the background worker
int hjKind = 0;               // 0 = regular sync, 1 = backfill chunk
int hjCity = -1;
bool hjFull = false;          // sync: the full 92-day one
String hjChunkStart;          // backfill: first day of the chunk
int hjToday = -1;             // sync: index of today's entry in hf*

// Regular sync for one city: the last pastDays finished days + today's forecast.
bool histSyncStart(int i, bool full) {
  double lat = 0, lon = 0;
  if (!getCityCoords(i, lat, lon)) { logDebug("HIST city " + String(i + 1) + ": no coordinates"); return false; }
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4) + "&longitude=" + String(lon, 4)
    + "&daily=" + String(HIST_DAILY_VARS) + "&timezone=auto&past_days=" + String(full ? HIST_PAST_DAYS : HIST_SHORT_DAYS) + "&forecast_days=1";
  if (!netPost(NJ_HIST, i, 0, full ? 1 : 0, histEpoch[i], url)) return false;
  histJobPending = true; hjKind = 0; hjCity = i; hjFull = full;
  return true;
}

void histSyncDone(int i, bool ok) {
  if (ok && hjFull) { histFullDone[i] = true; histLastFull[i] = millis(); }
  histNextSync[i] = millis() + (ok ? HIST_SYNC_INTERVAL_MS : HIST_RETRY_MS);
  logDebug("HIST city " + String(i + 1) + (hjFull ? " full" : " light") + " sync " + String(ok ? "OK" : "FAILED"));
  if (ok && millis() - histLastStorageRefresh > HIST_STORAGE_REFRESH_MS) histRefreshStorage();
}

// One backfill chunk (up to 90 days) from the historical archive, fill-only.
// One backfill chunk (up to 90 days) from the historical archive, fill-only.
bool histBackfillChunk(int i) {
  if (bfCursor[i] < bfStart[i]) { histBackfillStop(i, "Done - history goes back to " + bfStart[i]); return true; }
  String chunkEnd = bfCursor[i];
  String chunkStart = histAddDays(chunkEnd, -(HIST_BF_CHUNK_DAYS - 1));
  if (chunkStart < bfStart[i]) chunkStart = bfStart[i];
  double lat = 0, lon = 0;
  if (!getCityCoords(i, lat, lon)) { histBackfillFailed(i); return true; }
  String url = "https://archive-api.open-meteo.com/v1/archive?latitude=" + String(lat, 4) + "&longitude=" + String(lon, 4)
    + "&start_date=" + chunkStart + "&end_date=" + chunkEnd + "&daily=" + String(HIST_DAILY_VARS) + "&timezone=auto";
  if (!netPost(NJ_HIST, i, 1, 0, histEpoch[i], url)) return false;
  histJobPending = true; hjKind = 1; hjCity = i; hjChunkStart = chunkStart;
  return true;
}

void histBackfillFailed(int i) {
  bfFails[i]++;
  bfMsg[i] = "Request failed - retrying (" + String(bfFails[i]) + "/" + String(HIST_BF_MAX_FAILS) + ")";
  if (bfFails[i] >= HIST_BF_MAX_FAILS) histBackfillStop(i, "Stopped after repeated errors - see the Debug page");
}

void histBackfillDone(int i) {
  bfFails[i] = 0;
  bfCursor[i] = histAddDays(hjChunkStart, -1);
  bfMsg[i] = "Filled back to " + hjChunkStart;
  if (bfCursor[i] < bfStart[i]) histBackfillStop(i, "Done - history goes back to " + bfStart[i]);
  else histBackfillSave(i);
  if (millis() - histLastStorageRefresh > HIST_STORAGE_REFRESH_MS) histRefreshStorage();
}

// A history download came back from the worker: parse it and start the merge.
void histResult(NetJob &j) {
  histJobPending = false;
  int i = j.city;
  if (j.start != histEpoch[i]) return; // city renamed, wiped or imported meanwhile - drop it
  String who = String(hjKind ? "backfill city " : "city ") + String(i + 1);
  int n = -1;
  if (j.code == HTTP_CODE_OK) n = histParseDaily(j.body, who);
  else logDebug("HIST " + who + ": HTTP " + String(j.code));
  j.body = ""; // free memory before the SD work
  if (hjKind == 0) {
    if (n < 2) { histSyncDone(i, false); return; }
    int nPast = n - 1; // last entry is today
    hjToday = n - 1;
    if (!histMergeBegin(i, nPast, max(0, nPast - HIST_OVERWRITE_DAYS), String(hfDate[n - 1]))) {
      logDebug("HIST city " + String(i + 1) + ": SD write failed");
      histSyncDone(i, false);
    }
  } else {
    if (!bfActive[i]) return; // stopped meanwhile
    if (n < 1 || !histMergeBegin(i, n, n, "")) histBackfillFailed(i);
  }
}

// The merge finished (ok = the new file is in place), or was aborted.
void histMergeDone(bool ok, bool aborted) {
  int i = hjCity;
  if (hjKind == 0) {
    if (ok) {
      int t = hjToday;
      histTodayValid[i] = hfValid[t];
      histTodayDate[i] = String(hfDate[t]);
      histTodayMin[i] = hfMin[t]; histTodayMax[i] = hfMax[t]; histTodayRain[i] = hfRain[t];
      histTodayHum[i] = hfHum[t]; histTodayPres[i] = hfPres[t];
      histTodayGust[i] = hfGust[t]; histTodayGustOK[i] = hfGustOK[t];
      histTodaySun[i] = hfSun[t]; histTodaySunOK[i] = hfSunOK[t];
    } else if (!aborted) logDebug("HIST city " + String(i + 1) + ": SD write failed");
    histSyncDone(i, ok);
  } else if (!aborted && bfActive[i]) {
    if (ok) histBackfillDone(i); else histBackfillFailed(i);
  }
}

// Call every loop(). Starts at most one background download at a time, spaced
// out; regular syncs go first, then backfill chunks. Downloads run on the
// network worker and merges in short slices, so the device never stalls.
void historyLoop() {
  if (!sdOK) return;

  static unsigned long lastLocCheck = 0;
  if (millis() - lastLocCheck > 2000) { lastLocCheck = millis(); histCheckLocations(); }

  if (hmActive) { histMergeStep(); return; } // a merge in progress, a slice at a time
  if (histJobPending) return;                 // waiting for the background download
  if (WiFi.status() != WL_CONNECTED || isUpdating || netBusy) return;
  if (histLastAttempt != 0 && millis() - histLastAttempt < HIST_MIN_GAP_MS) return;

  for (int i = 0; i < NUM_CITIES; i++) {
    if (histTrimmedLoc(i).length() == 0) continue;
    if ((long)(millis() - histNextSync[i]) < 0) continue; // not due yet
    bool full = !histFullDone[i] || millis() - histLastFull[i] > HIST_FULL_SYNC_MS;
    histLastAttempt = millis();
    if (!histSyncStart(i, full)) { hjFull = full; histSyncDone(i, false); }
    return;
  }

  if (!histClockOK()) return;
  for (int i = 0; i < NUM_CITIES; i++) {
    if (!bfActive[i] || histTrimmedLoc(i).length() == 0) continue;
    histLastAttempt = millis();
    histBackfillChunk(i);
    return;
  }
}

// Last maxN finished days of a city, oldest first (for the on-device page).
// Only reads the tail of the file, so it stays fast however long the log is.
int histReadLastDays(int city, String *dates, float *hi, float *lo, float *rn, int maxN) {
  if (!sdOK || !SD.exists(histPath(city))) return 0;
  File f = SD.open(histPath(city), FILE_READ);
  if (!f) return 0;
  size_t sz = f.size();
  if (sz > 1500) { f.seek(sz - 1500); f.readStringUntil('\n'); } // skip the partial first line
  int n = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    String fl[4];
    if (histSplit(line, fl, 4) < 4 || fl[0].length() != 10 || !histFieldOK(fl[1]) || !histFieldOK(fl[2]) || !histFieldOK(fl[3])) continue;
    if (fl[1].toFloat() > 900) continue;
    if (n == maxN) {
      for (int k = 1; k < maxN; k++) { dates[k - 1] = dates[k]; hi[k - 1] = hi[k]; lo[k - 1] = lo[k]; rn[k - 1] = rn[k]; }
    } else n++;
    dates[n - 1] = fl[0]; lo[n - 1] = fl[1].toFloat(); hi[n - 1] = fl[2].toFloat(); rn[n - 1] = fl[3].toFloat();
  }
  f.close();
  return n;
}

// --- [LOCAL SENSOR LOG] ---
// Hourly averages of the BME280's 5-second readings: epochUTC,tempC,humidity,pressureHpa
#if USE_BME280
float lsSumT = 0, lsSumH = 0, lsSumP = 0;
int lsCount = 0;
long lsHour = -1;

// Call right after each readLocalSensor().
void localLogSample() {
  if (!sdOK || !bmeOK || !histClockOK()) return;
  long h = (long)(now() / 3600);
  if (lsHour < 0) lsHour = h;
  if (h != lsHour) {
    if (lsCount > 0) {
      File f = SD.open(LOCAL_LOG, FILE_APPEND);
      if (f) { f.printf("%ld,%.2f,%.1f,%.1f\n", lsHour * 3600L, lsSumT / lsCount, lsSumH / lsCount, lsSumP / lsCount); f.close(); }
    }
    lsSumT = lsSumH = lsSumP = 0; lsCount = 0; lsHour = h;
  }
  if (isnan(localTemp) || isnan(localHumid) || isnan(localPressure) || localPressure < 300) return; // skip bad readings
  lsSumT += localTemp; lsSumH += localHumid; lsSumP += localPressure; lsCount++;
}
#endif

// Counts (and optionally deletes) the archived logs left behind when a city
// was renamed (/hist_cN_oldX.csv).
int histListArchives(bool deleteThem) {
  File root = SD.open("/");
  if (!root) return 0;
  std::vector<String> found;
  File f = root.openNextFile();
  while (f) {
    String n = f.name();
    if (n.startsWith("/")) n = n.substring(1);
    if (!f.isDirectory() && n.startsWith("hist_c") && n.indexOf("_old") > 0) found.push_back("/" + n);
    f.close();
    f = root.openNextFile();
  }
  root.close();
  if (deleteThem) for (size_t k = 0; k < found.size(); k++) SD.remove(found[k]);
  return (int)found.size();
}

void histWipeCity(int i) {
  histMergeAbort(i);
  histEpoch[i]++;
  if (SD.exists(histPath(i))) SD.remove(histPath(i));
  histTouch(i);
  histTodayValid[i] = false;
  histFullDone[i] = false;
  histNextSync[i] = 0; // re-download the last 92 days right away
}

void histSendJson(bool ok, String msg) {
  server.send(ok ? 200 : 400, "application/json", "{\"ok\":" + String(ok ? "true" : "false") + ",\"msg\":\"" + jsonEsc(msg) + "\"}");
}

// Fields shared by /api/history and /api/history/meta (no surrounding braces).
String histMetaFields() {
  uint32_t histBytes = 0;
  int archives = 0;
  if (sdOK) {
    for (int i = 0; i < NUM_CITIES; i++) {
      if (!SD.exists(histPath(i))) continue;
      File f = SD.open(histPath(i), FILE_READ);
      if (f) { histBytes += f.size(); f.close(); }
    }
    archives = histListArchives(false);
  }
  String j = "\"currentCity\":" + String(currentCity) + ",\"names\":[";
  for (int i = 0; i < NUM_CITIES; i++) {
    if (i > 0) j += ",";
    j += "\"" + jsonEsc(getDisplayCity(locs[i])) + "\"";
  }
  j += "],\"sdOK\":" + String(sdOK ? "true" : "false");
  j += ",\"cardTotalKB\":" + String(histCardTotalKB) + ",\"cardUsedKB\":" + String(histCardUsedKB);
  j += ",\"histKB\":" + String((histBytes + 1023) / 1024);
  j += ",\"archives\":" + String(archives);
  return j;
}

// Today's forecast row for a city as a JSON array (with a trailing 1 marking
// it as a forecast), or "null" if there isn't one yet.
String histTodayJson(int city) {
  if (!histTodayValid[city]) return "null";
  return "[\"" + histTodayDate[city] + "\"," + String(histTodayMin[city], 1) + "," + String(histTodayMax[city], 1)
       + "," + String(histTodayRain[city], 1) + "," + String(histTodayHum[city], 0) + "," + String(histTodayPres[city], 0)
       + "," + (histTodayGustOK[city] ? String(histTodayGust[city], 0) : String("null"))
       + "," + (histTodaySunOK[city] ? String(histTodaySun[city], 1) : String("null")) + ",1]";
}

// GET /api/history/meta - everything the History page needs except the day
// rows themselves: names, storage info and today's forecast row per city.
void handleHistoryMeta() {
  String j = "{" + histMetaFields() + ",\"today\":[";
  for (int i = 0; i < NUM_CITIES; i++) { if (i > 0) j += ","; j += histTodayJson(i); }
  j += "]}";
  server.send(200, "application/json", j);
}

// GET /api/history/raw?city=N - the city's history file exactly as stored
// (CSV: date,tMin,tMax,rainMm,humidAvg,pressureAvg,gustMaxKmh,sunHours).
// The History page parses it in the browser, which is far faster than
// building JSON on the ESP32. Supports ETag/If-None-Match, so reloading the
// page only re-downloads files that actually changed.
void handleHistoryRaw() {
  int city = constrain(server.arg("city").toInt(), 0, NUM_CITIES - 1);
  File f;
  if (sdOK && SD.exists(histPath(city))) f = SD.open(histPath(city), FILE_READ);
  size_t size = f ? f.size() : 0;
  String etag = histEtag(city, size);
  server.sendHeader("Cache-Control", "no-cache"); // always revalidate - a 304 is cheap
  server.sendHeader("ETag", etag);
  if (server.header("If-None-Match") == etag) {
    if (f) f.close();
    server.send(304);
    return;
  }
  server.setContentLength(size);
  server.send(200, "text/csv", "");
  if (f) { histStreamFileBody(f); f.close(); }
}

// GET /api/history?city=N  (no city = whichever city the device is showing)
// Streamed in chunks, so there's no size limit. (The History page itself uses
// the faster /api/history/meta + /api/history/raw; this stays for scripts.)
// days: [date,tMin,tMax,rain,humid,pressure,gust|null,sun|null(,1 = today's forecast)]
void handleApiHistory() {
  int city = server.hasArg("city") ? constrain(server.arg("city").toInt(), 0, NUM_CITIES - 1) : currentCity;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  String chunk;
  chunk.reserve(1400);
  chunk = "{\"city\":" + String(city) + "," + histMetaFields() + ",\"days\":[";

  bool first = true;
  if (sdOK && SD.exists(histPath(city))) {
    File f = SD.open(histPath(city), FILE_READ);
    if (f) {
      LineReader lr(f);
      String line;
      while (lr.next(line)) {
        line.trim();
        String fl[8];
        int n = histSplit(line, fl, 8);
        if (n < 6 || fl[0].length() != 10) continue;
        bool ok = true;
        for (int k = 1; k < 6; k++) if (!histFieldOK(fl[k])) ok = false;
        if (!ok || fl[1].toFloat() > 900 || fl[2].toFloat() < -900) continue; // empty day from the old logger (999/-999)
        String g = (n > 6 && histFieldOK(fl[6])) ? fl[6] : String("null");
        String s = (n > 7 && histFieldOK(fl[7])) ? fl[7] : String("null");
        if (!first) chunk += ",";
        first = false;
        chunk += "[\"" + fl[0] + "\"," + fl[1] + "," + fl[2] + "," + fl[3] + "," + fl[4] + "," + fl[5] + "," + g + "," + s + "]";
        if (chunk.length() > 1200) { server.sendContent(chunk); chunk = ""; }
      }
      f.close();
    }
  }
  if (histTodayValid[city]) {
    if (!first) chunk += ",";
    chunk += histTodayJson(city);
  }
  chunk += "]}";
  server.sendContent(chunk);
  server.sendContent(""); // ends the chunked response
}

// GET /api/history/status - backfill progress (polled by the page while it runs)
void handleHistoryStatus() {
  String j = "{\"backfill\":[";
  for (int i = 0; i < NUM_CITIES; i++) {
    if (i > 0) j += ",";
    j += "{\"active\":" + String(bfActive[i] ? "true" : "false") + ",\"start\":\"" + bfStart[i] + "\",\"cursor\":\"" + bfCursor[i]
       + "\",\"end\":\"" + bfEnd[i] + "\",\"msg\":\"" + jsonEsc(bfMsg[i]) + "\"}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

// POST /api/history/backfill?target=0|1|2|all&years=1..10   (add &stop=1 to stop)
void handleHistoryBackfill() {
  if (!requireAppHeader()) return;
  String t = server.arg("target");
  bool stop = server.hasArg("stop");
  int years = constrain(server.arg("years").toInt(), 1, 10);
  if (!sdOK) { histSendJson(false, "SD card not found."); return; }
  if (!stop && !histClockOK()) { histSendJson(false, "The clock isn't synced yet - try again in a minute."); return; }
  for (int i = 0; i < NUM_CITIES; i++) {
    if (t != "all" && t.toInt() != i) continue;
    if (stop) { if (bfActive[i]) histBackfillStop(i, "Stopped"); }
    else if (histTrimmedLoc(i).length() > 0) histBackfillStart(i, years);
  }
  String logMsg = "HIST backfill " + String(stop ? "stop " : "start ") + t;
  if (!stop) logMsg += " " + String(years) + "y";
  logDebug(logMsg);
  histSendJson(true, stop ? "Backfill stopped." : "Backfill started - it runs in the background, you can close this page.");
}

// GET /api/local?hours=N (0 = all) - hourly local sensor readings
void handleApiLocal() {
  long hours = server.hasArg("hours") ? server.arg("hours").toInt() : 168;
  long cutoff = (hours > 0 && histClockOK()) ? (long)now() - hours * 3600L : 0;
  bool live = false;
#if USE_BME280
  live = bmeOK;
#endif
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  String chunk = "{\"bmeOK\":" + String(live ? "true" : "false") + ",\"rows\":[";
  bool first = true;
  if (sdOK && SD.exists(LOCAL_LOG)) {
    File f = SD.open(LOCAL_LOG, FILE_READ);
    LineReader lr(f);
    String line;
    while (f && lr.next(line)) {
      line.trim();
      String fl[4];
      if (histSplit(line, fl, 4) < 4 || !histFieldOK(fl[0]) || !histFieldOK(fl[1]) || !histFieldOK(fl[2]) || !histFieldOK(fl[3])) continue;
      if (fl[0].toInt() < cutoff) continue;
      if (!first) chunk += ",";
      first = false;
      chunk += "[" + fl[0] + "," + fl[1] + "," + fl[2] + "," + fl[3] + "]";
      if (chunk.length() > 1200) { server.sendContent(chunk); chunk = ""; }
    }
    if (f) f.close();
  }
#if USE_BME280
  if (lsCount > 0 && histClockOK()) { // the current, still-running hour
    if (!first) chunk += ",";
    chunk += "[" + String((long)now()) + "," + String(lsSumT / lsCount, 2) + "," + String(lsSumH / lsCount, 1) + "," + String(lsSumP / lsCount, 1) + "]";
  }
#endif
  chunk += "]}";
  server.sendContent(chunk);
  server.sendContent("");
}

// GET /api/local/export - the local sensor log as a spreadsheet-friendly CSV
void handleLocalExport() {
  server.sendHeader("Content-Disposition", "attachment; filename=\"local_sensor.csv\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  String chunk = "timeUTC,epochUTC,tempC,humidity,pressureHpa\n";
  if (sdOK && SD.exists(LOCAL_LOG)) {
    File f = SD.open(LOCAL_LOG, FILE_READ);
    LineReader lr(f);
    String line;
    char tb[20];
    while (f && lr.next(line)) {
      line.trim();
      if (line.length() < 10) continue;
      time_t e = (time_t)line.substring(0, line.indexOf(',')).toInt();
      snprintf(tb, sizeof(tb), "%04d-%02d-%02d %02d:00", year(e), month(e), day(e), hour(e));
      chunk += String(tb) + "," + line + "\n";
      if (chunk.length() > 1200) { server.sendContent(chunk); chunk = ""; }
    }
    if (f) f.close();
  }
  server.sendContent(chunk);
  server.sendContent("");
}

// --- [HISTORY EXPORT / IMPORT / WIPE] ---

// GET /api/history/export?city=N - downloads that city's log as a CSV file.
void handleHistoryExport() {
  int city = constrain(server.arg("city").toInt(), 0, NUM_CITIES - 1);
  String name = getDisplayCity(locs[city]), safe = "";
  for (unsigned int k = 0; k < name.length(); k++) safe += isalnum((unsigned char)name[k]) ? name[k] : '_';

  server.sendHeader("Content-Disposition", "attachment; filename=\"history_" + safe + ".csv\"");

  // The file is already plain CSV, so it's sent as-is after a header row.
  File f;
  if (sdOK && SD.exists(histPath(city))) f = SD.open(histPath(city), FILE_READ);
  String header = "date,tMin,tMax,rainMm,humidAvg,pressureAvg,gustMaxKmh,sunHours\r\n";
  server.setContentLength(header.length() + (f ? f.size() : 0));
  server.send(200, "text/csv", header);
  if (f) { histStreamFileBody(f); f.close(); }
}

const char* HIST_IMPORT_TMP = "/hist_import.tmp";
const int HIST_IMPORT_MAX_ROWS = 8000; // ~22 years per import, ~160 KB of RAM while importing
const uint16_t HIST_NONE = 0xFFFF;     // "no wind/sun value" marker
File histUploadFile;
bool histUploadOK = false;

struct HistImportRow {
  uint32_t key;      // YYYYMMDD
  int16_t tMin10, tMax10;
  uint16_t rain10, pres, seq, gust, sun10;
  uint8_t hum;
};

// Receives the uploaded CSV in pieces and writes it straight to the SD card,
// so even a big file never has to fit in RAM.
void handleHistoryUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    histUploadOK = false;
    if (!sdOK || !hasAppHeader()) return; // nothing is written for a request without the header
    if (SD.exists(HIST_IMPORT_TMP)) SD.remove(HIST_IMPORT_TMP);
    histUploadFile = SD.open(HIST_IMPORT_TMP, FILE_WRITE);
    histUploadOK = (bool)histUploadFile;
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (histUploadOK && histUploadFile.write(up.buf, up.currentSize) != up.currentSize) histUploadOK = false;
  } else if (up.status == UPLOAD_FILE_END) {
    if (histUploadFile) histUploadFile.close();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (histUploadFile) histUploadFile.close();
    histUploadOK = false;
  }
}

// Parses one CSV row: date,tMin,tMax[,rainMm,humidAvg,pressureAvg,gustMaxKmh,sunHours].
// v[0..6] = tMin,tMax,rain,hum,pres,gust,sun (gust/sun NAN when missing).
// Also accepts ; or tab separated files with decimal commas (Dutch Excel).
// Header lines and anything else that doesn't start with a date are skipped.
bool histParseImportLine(String line, uint32_t &key, float *v) {
  line.replace("\"", "");
  line.trim();
  if (line.length() < 12) return false;
  char sep = ',';
  if (line.indexOf(';') >= 0) sep = ';';
  else if (line.indexOf('\t') >= 0) sep = '\t';

  String f[8];
  int n = 0, from = 0;
  while (n < 8) {
    int p = line.indexOf(sep, from);
    if (p < 0) { f[n++] = line.substring(from); break; }
    f[n++] = line.substring(from, p);
    from = p + 1;
  }
  if (n < 3) return false;
  for (int k = 0; k < n; k++) { f[k].trim(); if (sep != ',') f[k].replace(',', '.'); }

  String d = f[0];
  if (d.length() != 10 || d[4] != '-' || d[7] != '-') return false;
  int y = d.substring(0, 4).toInt(), m = d.substring(5, 7).toInt(), dd = d.substring(8, 10).toInt();
  if (y < 1900 || y > 2200 || m < 1 || m > 12 || dd < 1 || dd > 31) return false;
  if (!histFieldOK(f[1]) || !histFieldOK(f[2])) return false;

  key = (uint32_t)y * 10000UL + (uint32_t)m * 100UL + (uint32_t)dd;
  v[0] = f[1].toFloat();
  v[1] = f[2].toFloat();
  for (int k = 3; k < 6; k++) v[k - 1] = (k < n && histFieldOK(f[k])) ? f[k].toFloat() : 0;
  for (int k = 6; k < 8; k++) v[k - 1] = (k < n && histFieldOK(f[k])) ? f[k].toFloat() : NAN;
  if (v[0] > 900 || v[1] < -900) return false;
  return true;
}

// Merges (or replaces) a city's log with the uploaded file. Imported days win
// over existing days with the same date. Existing rows are streamed, so only
// the imported rows are held in RAM.
bool histImportProcess(int city, bool replaceAll, String &msg) {
  histMergeAbort(city); // it writes the same temporary file
  histEpoch[city]++;
  if (!SD.exists(HIST_IMPORT_TMP)) { msg = "Upload failed."; return false; }

  uint32_t key;
  float v[7];
  int count = 0;
  File in = SD.open(HIST_IMPORT_TMP, FILE_READ);
  if (!in) { msg = "Upload failed."; return false; }
  {
    LineReader lr(in);
    String line;
    while (lr.next(line)) if (histParseImportLine(line, key, v)) count++;
  }
  in.close();

  if (count == 0) { msg = "No valid rows found. Expected columns: date,tMin,tMax,rainMm,humidAvg,pressureAvg,gustMaxKmh,sunHours with dates as YYYY-MM-DD."; return false; }
  if (count > HIST_IMPORT_MAX_ROWS) { msg = "The file has " + String(count) + " rows; the limit per import is " + String(HIST_IMPORT_MAX_ROWS) + ". Split it into smaller files."; return false; }
  if (ESP.getMaxAllocHeap() < (uint32_t)count * sizeof(HistImportRow) + 30000) { msg = "Not enough free memory for a file this size. Split it into smaller files."; return false; }

  std::vector<HistImportRow> rows;
  rows.reserve(count);
  in = SD.open(HIST_IMPORT_TMP, FILE_READ);
  if (!in) { msg = "Upload failed."; return false; }
  uint16_t seq = 0;
  {
  LineReader lr(in);
  String line;
  while ((int)rows.size() < count && lr.next(line)) {
    if (!histParseImportLine(line, key, v)) continue;
    HistImportRow r;
    r.key = key;
    r.tMin10 = (int16_t)constrain(lroundf(v[0] * 10), -3000L, 3000L);
    r.tMax10 = (int16_t)constrain(lroundf(v[1] * 10), -3000L, 3000L);
    r.rain10 = (uint16_t)constrain(lroundf(v[2] * 10), 0L, 65000L);
    r.hum    = (uint8_t)constrain(lroundf(v[3]), 0L, 100L);
    r.pres   = (uint16_t)constrain(lroundf(v[4]), 0L, 2000L);
    r.gust   = isnan(v[5]) ? HIST_NONE : (uint16_t)constrain(lroundf(v[5]), 0L, 1000L);
    r.sun10  = isnan(v[6]) ? HIST_NONE : (uint16_t)constrain(lroundf(v[6] * 10), 0L, 300L);
    r.seq    = seq++;
    rows.push_back(r);
  }
  }
  in.close();

  // Sort by date; for duplicate dates inside the file, the last one wins.
  std::sort(rows.begin(), rows.end(), [](const HistImportRow &a, const HistImportRow &b) {
    return a.key != b.key ? a.key < b.key : a.seq < b.seq;
  });
  size_t w = 0;
  for (size_t k = 0; k < rows.size(); k++) {
    if (w > 0 && rows[w - 1].key == rows[k].key) rows[w - 1] = rows[k];
    else rows[w++] = rows[k];
  }
  rows.resize(w);

  String path = histPath(city), tmp = "/hist_c" + String(city) + ".tmp";
  if (SD.exists(tmp)) SD.remove(tmp);
  File out = SD.open(tmp, FILE_WRITE);
  if (!out) { msg = "Could not write to the SD card."; return false; }

  char buf[100];
  auto writeRow = [&](const HistImportRow &r) {
    char g[8] = "", s[8] = "";
    if (r.gust != HIST_NONE) snprintf(g, sizeof(g), "%u", (unsigned)r.gust);
    if (r.sun10 != HIST_NONE) snprintf(s, sizeof(s), "%.1f", r.sun10 / 10.0f);
    snprintf(buf, sizeof(buf), "%04lu-%02lu-%02lu,%.1f,%.1f,%.1f,%u,%u,%s,%s",
             (unsigned long)(r.key / 10000UL), (unsigned long)((r.key / 100UL) % 100UL), (unsigned long)(r.key % 100UL),
             r.tMin10 / 10.0f, r.tMax10 / 10.0f, r.rain10 / 10.0f, (unsigned)r.hum, (unsigned)r.pres, g, s);
    out.println(buf);
  };

  int added = 0, replaced = 0, kept = 0;
  size_t ri = 0;
  if (!replaceAll && SD.exists(path)) {
    File ex = SD.open(path, FILE_READ);
    LineReader lr(ex);
    String line;
    while (ex && lr.next(line)) {
      line.trim();
      if (line.length() < 10) continue;
      uint32_t k = (uint32_t)(line.substring(0, 4).toInt() * 10000L + line.substring(5, 7).toInt() * 100L + line.substring(8, 10).toInt());
      while (ri < rows.size() && rows[ri].key < k) { writeRow(rows[ri++]); added++; }
      if (ri < rows.size() && rows[ri].key == k) { writeRow(rows[ri++]); replaced++; continue; }
      out.println(line);
      kept++;
    }
    if (ex) ex.close();
  }
  while (ri < rows.size()) { writeRow(rows[ri++]); added++; }
  out.close();

  if (SD.exists(path)) SD.remove(path);
  histTouch(city);
  if (!SD.rename(tmp, path)) { msg = "Could not save the merged file."; return false; }

  String name = getDisplayCity(locs[city]);
  if (replaceAll) msg = name + ": history replaced with " + String(rows.size()) + " imported days.";
  else msg = name + ": imported " + String(rows.size()) + " days (" + String(added) + " new, " + String(replaced) + " replaced, " + String(kept) + " existing kept).";
  return true;
}

// POST /api/history/import?city=N&mode=merge|replace (multipart file upload)
void handleHistoryImportDone() {
  if (!requireAppHeader()) return;
  int city = constrain(server.arg("city").toInt(), 0, NUM_CITIES - 1);
  bool replaceAll = server.arg("mode") == "replace";
  String msg;
  bool ok = false;
  if (!sdOK) msg = "SD card not found.";
  else if (!histUploadOK) msg = "Upload failed - try again.";
  else ok = histImportProcess(city, replaceAll, msg);
  if (sdOK && SD.exists(HIST_IMPORT_TMP)) SD.remove(HIST_IMPORT_TMP);
  if (sdOK) histRefreshStorage();
  logDebug("HIST import city " + String(city + 1) + ": " + msg);
  histSendJson(ok, msg);
}

// POST /api/history/wipe?target=0|1|2|all|archives|local
void handleHistoryWipe() {
  if (!requireAppHeader()) return;
  String t = server.arg("target");
  String msg;
  if (!sdOK) { histSendJson(false, "SD card not found."); return; }
  if (t == "archives") {
    int n = histListArchives(true);
    msg = "Deleted " + String(n) + " archived log(s).";
  } else if (t == "local") {
    if (SD.exists(LOCAL_LOG)) SD.remove(LOCAL_LOG);
    msg = "Local sensor log wiped.";
  } else if (t == "all") {
    for (int i = 0; i < NUM_CITIES; i++) { histWipeCity(i); if (bfActive[i]) histBackfillStop(i, "Stopped - history was wiped"); }
    msg = "All history wiped. The last 92 days are downloaded again automatically within about a minute.";
  } else {
    int i = constrain(t.toInt(), 0, NUM_CITIES - 1);
    histWipeCity(i);
    if (bfActive[i]) histBackfillStop(i, "Stopped - history was wiped");
    msg = getDisplayCity(locs[i]) + " history wiped. The last 92 days are downloaded again automatically within about a minute.";
  }
  histRefreshStorage();
  logDebug("HIST wipe " + t);
  histSendJson(true, msg);
}

#endif // USE_SD_HISTORY

// --- [PAGE RENDERING FUNCTIONS] ---

// --- theme selection
// True between sunset and sunrise (local time of the city on screen).
bool isNightNow() {
  int riseMin = timeStrToMinutes(sunRise), setMin = timeStrToMinutes(sunSet);
  if (riseMin < 0 || setMin < 0 || timeStatus() == timeNotSet) return false;
  time_t localT = now() + tzOffsetSec[currentCity];
  int nowMin = hour(localT) * 60 + minute(localT);
  return nowMin < riseMin || nowMin >= setMin;
}

// Picks the colors for the current theme mode (and, for AUTO, the time of
// day). Returns true if they changed, i.e. the screen needs a full redraw.
bool applyTheme() {
  bool dark = themeMode == THEME_DARK || (themeMode == THEME_AUTO && isNightNow());
  bool changed = dark != themeDark;
  themeDark = dark;
  T = dark ? THEME_DARK_COLORS : THEME_LIGHT_COLORS;
  bgColor = T.bg;
  refreshAccent();
  return changed;
}

const char* themeModeName() {
  return themeMode == THEME_DARK ? "DARK" : (themeMode == THEME_LIGHT ? "LIGHT" : "AUTO");
}

// --- drawing helpers
// Draws text in one of TFT_eSPI's FreeFonts (transparent background), then
// switches back to the built-in fonts - otherwise every later drawString(...,
// 1) would keep using the FreeFont instead of the small GLCD font.
void ffText(const String &s, int x, int y, const GFXfont *f, uint8_t datum, uint16_t color) {
  tft.setFreeFont(f);
  tft.setTextDatum(datum);
  tft.setTextColor(color);
  tft.drawString(s, x, y);
  tft.setFreeFont(nullptr);
}

int ffWidth(const String &s, const GFXfont *f) {
  tft.setFreeFont(f);
  int w = tft.textWidth(s);
  tft.setFreeFont(nullptr);
  return w;
}

// Rounded card with a 1px anti-aliased border. outside = the color around it.
void drawCard(int x, int y, int w, int h, int r, uint16_t fill, uint16_t border, uint16_t outside) {
  tft.fillSmoothRoundRect(x, y, w, h, r, border, outside);
  tft.fillSmoothRoundRect(x + 1, y + 1, w - 2, h - 2, r - 1, fill, border);
}

// A degree sign drawn as a small ring (the fonts don't include one).
void drawDegree(int cx, int cy, int r, uint16_t color, uint16_t bg) {
  tft.fillSmoothCircle(cx, cy, r, color, bg);
  tft.fillSmoothCircle(cx, cy, max(1, r - 2), bg, color);
}

// Value in bold with a small unit after it, vertically centered on yMid.
// Returns the x where the unit ends.
int valueWidth(const String &value, const String &unit, bool degree) {
  return ffWidth(value, &FreeSansBold12pt7b) + (degree ? 12 : 5) + (unit.length() ? tft.textWidth(unit, 2) : 0);
}

int drawValue(int x, int yMid, const String &value, const String &unit, bool degree, uint16_t bg) {
  ffText(value, x, yMid, &FreeSansBold12pt7b, ML_DATUM, T.text);
  int ux = x + ffWidth(value, &FreeSansBold12pt7b) + 5;
  if (degree) { drawDegree(ux + 1, yMid - 8, 3, T.text, bg); ux += 7; }
  if (unit.length()) {
    tft.setTextDatum(ML_DATUM); tft.setTextColor(T.muted);
    tft.drawString(unit, ux, yMid + 2, 2);
    ux += tft.textWidth(unit, 2);
  }
  return ux;
}

// --- weather icons, drawn from shapes (no image files), 64x64 design grid
enum IconKind { ICON_CLEAR, ICON_PARTLY, ICON_CLOUD, ICON_RAIN, ICON_DRIZZLE, ICON_THUNDER, ICON_SNOW, ICON_FOG };

int iconKindFor(String main, String desc) {
  main.toUpperCase(); desc.toUpperCase();
  if (main == "CLEAR") return ICON_CLEAR;
  if (main == "THUNDERSTORM") return ICON_THUNDER;
  if (main == "RAIN") return ICON_RAIN;
  if (main == "DRIZZLE") return ICON_DRIZZLE;
  if (main == "SNOW") return ICON_SNOW;
  if (main == "MIST" || main == "FOG" || main == "HAZE" || main == "SMOKE" || main == "DUST"
      || main == "SAND" || main == "ASH" || desc.indexOf("FOG") >= 0) return ICON_FOG;
  if (desc.indexOf("FEW") >= 0 || desc.indexOf("SCATTERED") >= 0 || desc.indexOf("PARTLY") >= 0
      || desc.indexOf("MAINLY CLEAR") >= 0) return ICON_PARTLY;
  return ICON_CLOUD;
}

void drawWeatherIcon(int kind, int x, int y, int size, bool night, uint16_t bg) {
  // Shapes on a 64x64 design grid, listed back to front. Every screen pixel is
  // sampled 4x4 times and each sample takes the color of the topmost shape
  // there, so overlapping shapes (a cloud over the sun) blend into each other
  // instead of showing a light fringe where an edge was blended against the
  // background.
  struct Shape { uint8_t type; float p[6]; uint16_t color; float x0, y0, x1, y1; };
  enum { SH_CIRCLE, SH_CAPSULE, SH_RRECT, SH_TRI, SH_CRESCENT };
  Shape sh[16];
  int n = 0;
  auto add = [&](uint8_t type, uint16_t color, float a, float b, float c, float d, float e, float g) {
    if (n >= 16) return;
    Shape &q = sh[n++];
    q.type = type; q.color = color;
    q.p[0] = a; q.p[1] = b; q.p[2] = c; q.p[3] = d; q.p[4] = e; q.p[5] = g;
    switch (type) { // bounding box, for skipping empty pixels quickly
      case SH_CIRCLE: case SH_CRESCENT: q.x0 = a - c; q.x1 = a + c; q.y0 = b - c; q.y1 = b + c; break;
      case SH_CAPSULE: q.x0 = min(a, c) - e; q.x1 = max(a, c) + e; q.y0 = min(b, d) - e; q.y1 = max(b, d) + e; break;
      case SH_RRECT: q.x0 = a; q.x1 = a + c; q.y0 = b; q.y1 = b + d; break;
      default: q.x0 = min(a, min(c, e)); q.x1 = max(a, max(c, e)); q.y0 = min(b, min(d, g)); q.y1 = max(b, max(d, g));
    }
  };
  auto circle = [&](float cx, float cy, float r, uint16_t c) { add(SH_CIRCLE, c, cx, cy, r, 0, 0, 0); };
  auto line = [&](float x1, float y1, float x2, float y2, float w, uint16_t c) { add(SH_CAPSULE, c, x1, y1, x2, y2, w / 2, 0); };
  auto cloud = [&](float dx, float dy, uint16_t c) {
    circle(20 + dx, 38 + dy, 11, c);
    circle(33 + dx, 29 + dy, 15, c);
    circle(46 + dx, 38 + dy, 11, c);
    add(SH_RRECT, c, 9 + dx, 36 + dy, 48, 13, 6, 0);
  };
  auto sun = [&](float cx, float cy, float r, float r1, float r2, float w) {
    for (int k = 0; k < 8; k++) {
      float a = k * PI / 4;
      line(cx + cos(a) * r1, cy + sin(a) * r1, cx + cos(a) * r2, cy + sin(a) * r2, w, T.sun);
    }
    circle(cx, cy, r, T.sun);
  };
  auto moon = [&](float cx, float cy, float r) { add(SH_CRESCENT, T.moon, cx, cy, r, cx + r * 0.45f, cy - r * 0.35f, r * 0.85f); };

  switch (kind) {
    case ICON_CLEAR:
      if (night) moon(32, 32, 18); else sun(32, 32, 12, 18, 27, 4);
      break;
    case ICON_PARTLY:
      if (night) moon(24, 22, 13); else sun(23, 22, 9, 14, 20, 3.5f);
      cloud(6, 8, T.icon);
      break;
    case ICON_RAIN:
      for (int i = 0; i < 3; i++) line(23 + 11 * i, 47, 19 + 11 * i, 58, 3.5f, T.rain);
      cloud(0, -6, T.icon);
      break;
    case ICON_DRIZZLE:
      for (int i = 0; i < 3; i++) { circle(22 + 11 * i, 50, 2.4f, T.rain); circle(27 + 11 * i, 58, 2.4f, T.rain); }
      cloud(0, -6, T.icon);
      break;
    case ICON_THUNDER:
      cloud(0, -6, T.icon);
      add(SH_TRI, T.sun, 36, 40, 25, 54, 33, 54);
      add(SH_TRI, T.sun, 30, 52, 39, 52, 27, 64);
      break;
    case ICON_SNOW:
      cloud(0, -6, T.icon);
      circle(21, 50, 2.8f, T.lo); circle(33, 55, 2.8f, T.lo); circle(45, 50, 2.8f, T.lo);
      circle(27, 61, 2.8f, T.lo); circle(39, 61, 2.8f, T.lo);
      break;
    case ICON_FOG:
      for (int i = 0; i < 4; i++) line(i % 2 ? 14 : 8, 20 + 10 * i, i % 2 ? 56 : 50, 20 + 10 * i, 5, T.icon);
      break;
    default:
      cloud(0, 0, T.icon);
  }

  auto inside = [&](const Shape &q, float px, float py) -> bool {
    const float *p = q.p;
    switch (q.type) {
      case SH_CIRCLE: { float dx = px - p[0], dy = py - p[1]; return dx * dx + dy * dy <= p[2] * p[2]; }
      case SH_CRESCENT: {
        float dx = px - p[0], dy = py - p[1], ex = px - p[3], ey = py - p[4];
        return dx * dx + dy * dy <= p[2] * p[2] && ex * ex + ey * ey > p[5] * p[5];
      }
      case SH_CAPSULE: { // distance from the segment <= half the width (round caps)
        float vx = p[2] - p[0], vy = p[3] - p[1], wx = px - p[0], wy = py - p[1];
        float t = constrain((wx * vx + wy * vy) / (vx * vx + vy * vy), 0.0f, 1.0f);
        float dx = wx - t * vx, dy = wy - t * vy;
        return dx * dx + dy * dy <= p[4] * p[4];
      }
      case SH_RRECT: {
        float hw = p[2] / 2, hh = p[3] / 2, r = p[4];
        float qx = fabsf(px - (p[0] + hw)) - (hw - r), qy = fabsf(py - (p[1] + hh)) - (hh - r);
        if (qx <= 0 || qy <= 0) return qx <= r && qy <= r;
        return qx * qx + qy * qy <= r * r;
      }
      default: { // triangle: same side of all three edges
        float d1 = (px - p[2]) * (p[1] - p[3]) - (p[0] - p[2]) * (py - p[3]);
        float d2 = (px - p[4]) * (p[3] - p[5]) - (p[2] - p[4]) * (py - p[5]);
        float d3 = (px - p[0]) * (p[5] - p[1]) - (p[4] - p[0]) * (py - p[1]);
        bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
        return !(neg && pos);
      }
    }
  };

  float f = size / 64.0f, inv = 1.0f / f;
  int cand[16];
  for (int py = 0; py < size; py++) {
    float gy0 = py * inv, gy1 = (py + 1) * inv;
    for (int px = 0; px < size; px++) {
      float gx0 = px * inv, gx1 = (px + 1) * inv;
      int nc = 0;
      for (int i = n - 1; i >= 0; i--) // topmost first
        if (sh[i].x1 >= gx0 && sh[i].x0 <= gx1 && sh[i].y1 >= gy0 && sh[i].y0 <= gy1) cand[nc++] = i;
      if (!nc) continue;
      int r = 0, g = 0, b = 0, hit = 0;
      for (int sy = 0; sy < 4; sy++) {
        float gy = (py + (sy + 0.5f) / 4) * inv;
        for (int sx = 0; sx < 4; sx++) {
          float gx = (px + (sx + 0.5f) / 4) * inv;
          uint16_t c = bg;
          for (int k = 0; k < nc; k++) if (inside(sh[cand[k]], gx, gy)) { c = sh[cand[k]].color; hit++; break; }
          r += (c >> 11) & 31; g += (c >> 5) & 63; b += c & 31;
        }
      }
      if (!hit) continue; // nothing here - leave the background as it is
      tft.drawPixel(x + px, y + py, (((r + 8) / 16) << 11) | (((g + 8) / 16) << 5) | ((b + 8) / 16));
    }
  }
}

// The moon at phase f (0 = new, 0.5 = full), radius R, centred on cx/cy.
// Each pixel is sampled 4x4 times to find how much of it is moon and how much
// of that is lit, which gives smooth edges along both the outline and the
// day/night line. mirror = southern-hemisphere view.
void drawMoonPhase(int cx, int cy, int R, float f, bool mirror, uint16_t bg) {
  float c = cosf(2 * PI * f);
  bool waxing = f < 0.5f;
  float R2 = (float)R * R;
  for (int py = -R - 1; py <= R; py++) {
    for (int px = -R - 1; px <= R; px++) {
      int inDisc = 0, inLit = 0;
      for (int sy = 0; sy < 4; sy++) {
        float y = py + (sy + 0.5f) / 4;
        float w2 = R2 - y * y;
        if (w2 <= 0) continue;
        float w = sqrtf(w2);
        for (int sx = 0; sx < 4; sx++) {
          float x = px + (sx + 0.5f) / 4;
          if (x * x > w2) continue;
          inDisc++;
          float xm = mirror ? -x : x;
          if (waxing ? (xm >= c * w) : (xm <= -c * w)) inLit++; // lit side grows from the right while waxing
        }
      }
      if (!inDisc) continue;
      uint16_t col = tft.alphaBlend(inLit * 255 / inDisc, T.moon, T.moonShadow);
      if (inDisc < 16) col = tft.alphaBlend(inDisc * 255 / 16, col, bg);
      tft.drawPixel(cx + px, cy + py, col);
    }
  }
}

// --- page frame
void drawNavDots(int mode) {
  // Page dots along the bottom - shows which of the 7 pages you're on
  int totalPages = 7, spacing = 16;
  int startXdots = 160 - ((totalPages - 1) * spacing) / 2;
  for (int i = 0; i < totalPages; i++) {
    int cx = startXdots + i * spacing;
    if (i == mode) tft.fillSmoothCircle(cx, NAV_DOT_Y, 4, T.accent, T.bg);
    else tft.fillSmoothCircle(cx, NAV_DOT_Y, 3, T.line, T.bg);
  }
  // City dots under the title - only on the home page, where the arrows live.
  if (mode == 0) {
    int cSpacing = 12;
    int cStartX = 160 - ((NUM_CITIES - 1) * cSpacing) / 2;
    for (int i = 0; i < NUM_CITIES; i++) {
      int cx = cStartX + i * cSpacing;
      tft.fillSmoothCircle(cx, CITY_DOT_Y, 3, i == currentCity ? T.accent : T.line, T.bg);
    }
  }
}

void drawHomeTile(int x, int y, const char *label, const String &value, const String &unit, bool degree) {
  drawCard(x, y, 146, 48, 10, T.card, T.border, T.bg);
  tft.setTextDatum(TL_DATUM); tft.setTextColor(T.muted);
  tft.drawString(label, x + 11, y + 6, 2);
  drawValue(x + 11, y + 33, value, unit, degree, T.card);
}

// Pressure tile, with an arrow for the 3-hour trend (two arrows when fast).
// No arrow when steady or when there's no recent trend data.
void drawPressureTile() {
  drawHomeTile(162, 316, "PRESSURE", String(pressure), "hPa", false);
  if (!pTrendValid() || pTrendLevel == 0) return;
  bool up = pTrendLevel > 0;
  int n = abs(pTrendLevel) >= 2 ? 2 : 1;
  uint16_t c = abs(pTrendLevel) >= 2 ? T.text : T.muted;
  for (int k = 0; k < n; k++) {
    int cx = 162 + 146 - 16 - k * 13, cy = 316 + 31;
    if (up) tft.fillTriangle(cx - 5, cy + 4, cx + 5, cy + 4, cx, cy - 5, c);
    else tft.fillTriangle(cx - 5, cy - 4, cx + 5, cy - 4, cx, cy + 5, c);
  }
}

// Everything on the home page that only changes when new weather arrives
// (or the city/theme changes). The clock, date, rain bars and status dot are
// drawn by drawPageHome() since they change on their own.
void drawHomeStatic() {
  // Header: city name with switch arrows either side
  String city = getDisplayCity(locs[currentCity]);
  const GFXfont *cf = ffWidth(city, &FreeSansBold12pt7b) > 240 ? &FreeSansBold9pt7b : &FreeSansBold12pt7b;
  ffText(city, 160, 24, cf, MC_DATUM, T.text);
  tft.drawWideLine(24, 15, 16, 24, 3, T.muted, T.bg); tft.drawWideLine(16, 24, 24, 33, 3, T.muted, T.bg);
  tft.drawWideLine(296, 15, 304, 24, 3, T.muted, T.bg); tft.drawWideLine(304, 24, 296, 33, 3, T.muted, T.bg);

  // Current conditions: icon + temperature
  drawWeatherIcon(iconKindFor(mainWeather, weatherDesc), 18, 56, 72, isNightNow(), T.bg);
  String tNum = String(tempnum, 1);
  int nW = tft.textWidth(tNum, 7);
  int tx = 104, groupW = nW + 17 + ffWidth("C", &FreeSansBold12pt7b);
  if (tx + groupW > 308) tx = 308 - groupW; // very wide values like -12.5
  tft.setTextDatum(TL_DATUM); tft.setTextColor(T.text, T.bg);
  tft.drawString(tNum, tx, 62, 7);
  drawDegree(tx + nW + 9, 68, 5, T.text, T.bg);
  ffText("C", tx + nW + 17, 62, &FreeSansBold12pt7b, TL_DATUM, T.text);

  // Description on the left, today's high/low on the right
  String hl = "H " + String((int)round(tempMax)) + "   L " + String((int)round(tempMin));
  int room = 280 - ffWidth(hl, &FreeSans9pt7b) - 12;
  String desc = weatherDesc;
  const GFXfont *df = &FreeSansBold9pt7b;
  if (ffWidth(desc, df) > room) df = &FreeSans9pt7b;
  if (ffWidth(desc, df) > room) {
    while (desc.length() > 3 && ffWidth(desc + "..", df) > room) desc.remove(desc.length() - 1);
    desc.trim(); desc += "..";
  }
  ffText(desc, 20, 136, df, ML_DATUM, T.muted);
  ffText(hl, 300, 136, &FreeSans9pt7b, MR_DATUM, T.muted);

  // Clock card (digits drawn by drawPageHome) and the four detail tiles
  drawCard(12, 150, 296, 104, 14, T.card, T.border, T.bg);
  drawHomeTile(12, 262, "FEELS", String(feels_like, 1), "C", true);
  drawHomeTile(162, 262, "WIND", String((int)round(windSpeedKmH)), "km/h " + windDir, false);
  drawHomeTile(12, 316, "HUMIDITY", String(huminum), "%", false);
  drawPressureTile();

  // Bottom card: upcoming rain (or the animation, if that's chosen instead)
  drawCard(12, 370, 296, 92, 12, T.card, T.border, T.bg); // contents: drawHomeRainPreview()
}

void drawPageBackground(int mode) {
  tft.fillScreen(T.bg);
  if (mode == 0) {
    drawHomeStatic();
  } else {
    const char *titles[] = {"", "ATMOSPHERE", "PRECIPITATION", "SUN & MOON", "SYSTEM", "5-DAY FORECAST", "7-DAY HISTORY"};
    ffText(titles[mode], 160, 36, &FreeSansBold12pt7b, MC_DATUM, T.text);
    drawCard(15, 70, 290, 397, 15, T.card, T.border, T.bg);
  }
  drawNavDots(mode); // page + city position indicators
}

// Freshness dot + age in the top-right of the home page's rain card.
// Green = last fetch succeeded, red = it failed (data on screen is getting old).
void drawStatusIndicator(bool forceDraw = false) {
  static int lastShownAge = -1;
  static bool lastShownOK = true;
  unsigned long ageMin = lastFetchMillis == 0 ? 0 : (millis() - lastFetchMillis) / 60000;
  if (!forceDraw && (int)ageMin == lastShownAge && lastFetchOK == lastShownOK && drawnMode == 0) return;
  lastShownAge = (int)ageMin; lastShownOK = lastFetchOK;

  tft.fillRect(STATUS_DOT_X - 62, STATUS_DOT_Y - 8, 68, 17, T.card); // erase previous dot+text
  tft.fillSmoothCircle(STATUS_DOT_X, STATUS_DOT_Y, STATUS_DOT_R, lastFetchOK ? T.good : T.bad, T.card);
  tft.setTextDatum(MR_DATUM); tft.setTextColor(lastFetchOK ? T.muted : T.bad, T.card);
  tft.drawString(lastFetchOK ? (String(ageMin) + "m") : String("ERR"), STATUS_DOT_X - STATUS_DOT_R - 5, STATUS_DOT_Y + 1, 2);
}

// Bottom card of the home page. When there's a fresh 2-hour outlook (radar in
// NL/BE, 15-min forecast elsewhere) it shows that: a one-line message and a
// bar per step. Otherwise the 3-hour steps for the next 15 hours. Only
// redraws when something visible changed.
void drawHomeRainPreview() {
  static String lastSig = "";
  String sig;
  if (nowValid) {
    sig = "N" + nowShort + "/" + String(nowVersion) + "/" + String(nowFirst) + "/" + String(modelValid() ? modFetchedAt : 0);
  } else {
    sig = "F";
    for (int i = 0; i < upcomingRainCount; i++) sig += String(upcomingRainMm[i], 1) + ",";
  }
  if (sig == lastSig && drawnMode == 0) return;
  lastSig = sig;

  tft.fillRect(18, 373, 214, 17, T.card); // title row (the status dot sits to its right)
  tft.fillRect(18, 391, 284, 68, T.card); // bars + labels

  if (nowValid) {
    // Message, bold if it fits; drop the intensity word before shrinking
    String msg = nowShort;
    const GFXfont *f = &FreeSansBold9pt7b;
    if (ffWidth(msg, f) > 206 && msg.indexOf(',') > 0 && !nowBlend) msg = msg.substring(0, msg.indexOf(','));
    if (ffWidth(msg, f) > 206) f = &FreeSans9pt7b;
    ffText(msg, 24, 381, f, ML_DATUM, nowRainingNow || nowStartIdx >= 0 ? T.text : T.muted);

    int n = min(nowCount - nowFirst, 24);
    float slot = 272.0f / n;
    int bw = max(2, (int)slot - 2), baseline = 436, maxH = 34;
    // Faint bars behind the radar: what the weather model expects (radar cities only)
    float model[24];
    float scale = max(1.0f, nowMaxMm);
    for (int k = 0; k < n; k++) {
      model[k] = nowRadar ? modelAt(nowLabel[nowFirst + k]) : -1;
      if (model[k] > scale) scale = min(model[k], 8.0f);
    }
    for (int k = 0; k < n; k++) {
      if (model[k] < 0.1f) continue;
      int x = 24 + (int)(k * slot);
      int h = constrain((int)(model[k] / scale * maxH), 3, maxH);
      tft.fillSmoothRoundRect(x, baseline - h, bw, h, min(2, bw / 2), T.rainSoft, T.card);
    }
    for (int k = 0; k < n; k++) {
      float mm = nowMm[nowFirst + k];
      int x = 24 + (int)(k * slot);
      if (mm >= 0.1f) {
        int h = constrain((int)(mm / scale * maxH), 3, maxH);
        tft.fillSmoothRoundRect(x, baseline - h, bw, h, min(2, bw / 2), T.rain, T.card);
      } else if (model[k] < 0.1f) tft.fillRect(x, baseline - 2, bw, 2, T.line);
    }
    // Time labels every 30 minutes, plus where the data comes from
    tft.setTextDatum(MC_DATUM); tft.setTextColor(T.muted);
    int per = 30 / nowStepMin;
    for (int k = 0; k < n; k += per) {
      int x = constrain(24 + (int)(k * slot + slot / 2), 40, 280);
      tft.drawString(k == 0 ? String("now") : nowLabel[nowFirst + k], x, baseline + 13, 2);
    }
    tft.setTextDatum(TR_DATUM);
    tft.drawString(nowRadar ? "RADAR" : "FORECAST", 300, 393, 1);
    return;
  }

  // 15-hour view: one bar per 3-hour step, plus the total
  tft.setTextDatum(ML_DATUM); tft.setTextColor(T.muted);
  tft.drawString("RAIN NEXT 15H", 24, 381, 2);
  int n = upcomingRainCount;
  if (n == 0) {
    tft.setTextDatum(MC_DATUM);
    tft.drawString("NO FORECAST DATA", 160, 422, 2);
    return;
  }
  float total = 0, maxRain = 1.0f; // 1 mm floor so a dry forecast doesn't fill the card
  for (int i = 0; i < n; i++) { total += upcomingRainMm[i]; if (upcomingRainMm[i] > maxRain) maxRain = upcomingRainMm[i]; }
  tft.setTextColor(T.text);
  tft.drawString(String(total, 1) + " mm", 136, 381, 2);

  int baseline = 436, maxH = 36;
  for (int i = 0; i < n; i++) {
    int cx = 44 + i * 58;
    int h = (int)(upcomingRainMm[i] / maxRain * maxH);
    if (upcomingRainMm[i] > 0 && h < 3) h = 3;
    if (h > 0) tft.fillSmoothRoundRect(cx - 15, baseline - h, 30, h, min(4, h / 2), T.rain, T.card);
    else tft.fillRect(cx - 15, baseline - 2, 30, 2, T.line);
    tft.setTextDatum(MC_DATUM); tft.setTextColor(T.muted);
    tft.drawString(upcomingRainLabel[i], cx, baseline + 13, 2);
  }
}

void drawPageHome() {
  static String lastDate = "";
  if (now() != prevDisplay || drawnMode != 0) {
    time_t localT = now() + tzOffsetSec[currentCity];
    // Font 7 = TFT_eSPI's built-in 7-segment digits. Hours in the theme's
    // text color, minutes in the chosen text color (see safeTextColor).
    String hourStr = (hour(localT) < 10 ? "0" : "") + String(hour(localT));
    String minStr  = (minute(localT) < 10 ? "0" : "") + String(minute(localT));
    int ty = 162;
    int blockW = tft.textWidth("88", 7); // rendered width of a 2-digit Font 7 block
    int gap = 30;                        // gap between the hour and minute blocks
    int tx = 160 - (blockW * 2 + gap) / 2;

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(T.text, T.card);
    tft.drawString(hourStr, tx, ty, 7);
    tft.setTextColor(safeTextColor(textR, textG, textB), T.card);
    tft.drawString(minStr, tx + blockW + gap, ty, 7);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(T.text, T.card);
    tft.drawString(":", tx + blockW + gap / 2, ty + 24, 7);

    String d = String(DAY_NAMES[weekday(localT) - 1]) + " " + String(day(localT)) + " " + MONTH_NAMES[month(localT) - 1];
    if (d != lastDate || drawnMode != 0) {
      lastDate = d;
      tft.fillRect(20, 224, 280, 24, T.card);
      ffText(d, 160, 236, &FreeSansBold9pt7b, MC_DATUM, T.muted);
    }
    prevDisplay = now();
  }

  // Pressure trend arrived / changed / expired -> redraw just that tile
  static unsigned long lastPV = 0;
  static bool lastPValid = false;
  if (drawnMode == 0 && (pTrendVersion != lastPV || pTrendValid() != lastPValid)) drawPressureTile();
  lastPV = pTrendVersion; lastPValid = pTrendValid();

  if (showRainPreview) {
    drawHomeRainPreview();
  } else if (showAnimation) {
    if (millis() - AprevTime > 100) {
      Anim++; if (Anim == 10) Anim = 0;
      AprevTime = millis();
      TJpgDec.drawJpg(135, 384, animFrames[Anim], animSizes[Anim]);
    }
  }
  drawStatusIndicator(drawnMode != 0);
}

void drawPageStats() {
  if (drawnMode == 1) return;
  int sY = 80;
  String labels[9] = {"CITY", "TEMPERATURE", "FEELS LIKE", "HUMIDITY", "PRESSURE", "VISIBILITY", "", "", ""};
  String vals[9]   = {getDisplayCity(locs[currentCity]), String(tempnum, 1), String(feels_like, 1),
                      String(huminum), String(pressure), String(visibility), "", "", ""};
  String units[9]  = {"", "C", "C", "%", "hPa", "km", "", "", ""};
  bool deg[9]      = {false, true, true, false, false, false, false, false, false};
  int rowCount = 6;
  if (pTrendValid()) {
    labels[rowCount] = "PRESSURE TREND"; vals[rowCount] = pressureTrendText(pTrendLevel); units[rowCount] = "";
    rowCount++;
  }
#if USE_BME280
  // Local sensor readings, if a BME280 was detected
  if (bmeOK) {
    labels[rowCount] = "INDOOR TEMP";     vals[rowCount] = String(localTemp, 1); units[rowCount] = "C"; deg[rowCount] = true; rowCount++;
    labels[rowCount] = "INDOOR HUMIDITY"; vals[rowCount] = String(localHumid, 1); units[rowCount] = "%"; rowCount++;
  }
#endif
  // One row per value: label on the left, value right-aligned
  int step = 368 / rowCount;
  for (int i = 0; i < rowCount; i++) {
    int yMid = sY + i * step + step / 2;
    tft.setTextDatum(ML_DATUM); tft.setTextColor(T.muted);
    tft.drawString(labels[i], 34, yMid, 2);
    String v = vals[i];
    int maxW = 286 - 130; // keep clear of the label column
    while (v.length() > 3 && valueWidth(v, units[i], deg[i]) > maxW) v.remove(v.length() - 1); // very long city names
    drawValue(286 - valueWidth(v, units[i], deg[i]), yMid, v, units[i], deg[i], T.card);
    if (i < rowCount - 1) tft.drawFastHLine(30, sY + (i + 1) * step, 260, T.line);
  }
}

void drawPageRain() {
  if (drawnMode == 2) return;
  tft.setTextDatum(MC_DATUM); tft.setTextColor(T.muted);
  tft.drawString("LAST HOUR", 90, 94, 2);
  tft.drawString("NEXT 3 HOURS", 230, 94, 2);
  ffText(String(rain1h, 1) + " mm", 90, 122, &FreeSansBold12pt7b, MC_DATUM, T.text);
  ffText(String(rain3h, 1) + " mm", 230, 122, &FreeSansBold12pt7b, MC_DATUM, T.text);
  tft.drawFastVLine(160, 86, 52, T.line);
  analyzeNowcast();
  if (nowValid) {
    String line = nowBlend ? nowMsg : nowMsg + (nowRadar ? " (radar)" : " (forecast)");
    if (tft.textWidth(line, 2) > 270) line = nowBlend ? nowShort + " (forecast)" : nowMsg;
    tft.setTextDatum(MC_DATUM); tft.setTextColor(T.text);
    tft.drawString(line, 160, 150, 2);
  }

  // Graph of the next few 3-hour steps, so you can see *when* rain is expected
  tft.setTextColor(T.muted);
  tft.drawString("UPCOMING RAIN (3H STEPS)", 160, 184, 2);

  int graphLeft = 55, graphRight = 265, baseline = 420, maxBarHeight = 190;
  int n = upcomingRainCount;
  if (n == 0) {
    tft.drawString("NO FORECAST DATA", 160, 330, 2);
    return;
  }
  float maxRain = 0.5; // floor so an all-dry forecast doesn't divide by ~0
  for (int i = 0; i < n; i++) if (upcomingRainMm[i] > maxRain) maxRain = upcomingRainMm[i];
  maxRain *= 1.15; // headroom so the highest point doesn't touch its label

  int stepX = (n > 1) ? (graphRight - graphLeft) / (n - 1) : 0;
  int px[RAIN_GRAPH_POINTS], py[RAIN_GRAPH_POINTS];
  for (int i = 0; i < n; i++) {
    px[i] = graphLeft + i * stepX;
    py[i] = baseline - (int)((upcomingRainMm[i] / maxRain) * maxBarHeight);
  }
  for (int g = 1; g <= 3; g++) tft.drawFastHLine(graphLeft - 10, baseline - g * maxBarHeight / 3, (graphRight - graphLeft) + 20, T.line);
  for (int i = 0; i < n - 1; i++) {
    tft.fillTriangle(px[i], baseline, px[i], py[i], px[i + 1], py[i + 1], T.rainSoft);
    tft.fillTriangle(px[i], baseline, px[i + 1], py[i + 1], px[i + 1], baseline, T.rainSoft);
  }
  tft.drawFastHLine(graphLeft - 10, baseline, (graphRight - graphLeft) + 20, T.muted); // baseline axis
  for (int i = 0; i < n - 1; i++) tft.drawWideLine(px[i], py[i], px[i + 1], py[i + 1], 3, T.rain, T.rainSoft);
  for (int i = 0; i < n; i++) {
    tft.fillSmoothCircle(px[i], py[i], 4, T.rain, T.rainSoft);
    // Transparent text, so labels never punch holes in the line/fill
    tft.setTextDatum(MC_DATUM); tft.setTextColor(T.text);
    tft.drawString(String(upcomingRainMm[i], 1), px[i], py[i] - 14, 2);
    tft.setTextColor(T.muted);
    tft.drawString(upcomingRainLabel[i], px[i], baseline + 16, 2);
  }
}

// NEW: converts "HH:MM" into minutes-since-midnight.
int timeStrToMinutes(String hhmm) {
  if (hhmm.length() < 5) return -1;
  return hhmm.substring(0, 2).toInt() * 60 + hhmm.substring(3, 5).toInt();
}

// NEW: shared sunrise/sunset countdown math, used by both the on-device
// Astro page and the web dashboard's /api/status, so they always agree.
// If both today's sunrise and sunset have already passed, "tomorrow's
// sunrise" is approximated as happening at the exact same clock time as
// today's - actual sunrise shifts by only a minute or two a day, so this
// stays accurate to within that margin until the next fetch brings a real
// value after midnight. eventName is "" if there's no data yet.
SunCountdown computeSunCountdown() {
  SunCountdown c; c.eventName = ""; c.diffMin = -1;
  int riseMin = timeStrToMinutes(sunRise);
  int setMin = timeStrToMinutes(sunSet);
  if (riseMin < 0 || setMin < 0) return c;

  time_t localT = now() + tzOffsetSec[currentCity];
  int nowMin = hour(localT) * 60 + minute(localT);

  if (nowMin < riseMin) { c.eventName = "SUNRISE"; c.diffMin = riseMin - nowMin; }
  else if (nowMin < setMin) { c.eventName = "SUNSET"; c.diffMin = setMin - nowMin; }
  else { c.eventName = "SUNRISE"; c.diffMin = (1440 - nowMin) + riseMin; } // tomorrow, approximated - see note above
  return c;
}

// Sunrise/sunset countdown badge - ticks live while you're on the page.
void drawAstroCountdown() {
  static String lastLabel = "";
  SunCountdown c = computeSunCountdown();
  if (c.eventName == "") return; // no data yet

  bool rise = c.eventName == "SUNRISE";
  uint16_t fg = rise ? T.sunriseFg : T.sunsetFg, bg = rise ? T.sunriseBg : T.sunsetBg;
  String label = c.eventName + " IN " + String(c.diffMin / 60) + "H " + String(c.diffMin % 60) + "M";
  if (label == lastLabel && drawnMode == 3) return; // nothing changed since last draw
  lastLabel = label;

  int badgeW = 230, badgeH = 30, badgeCY = 440; // inside the moon card (ends at y=460)
  drawCard(160 - badgeW / 2, badgeCY - badgeH / 2, badgeW, badgeH, 8, bg, fg, T.card2);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(fg, bg);
  tft.drawString(label, 160, badgeCY, 2);
}

void drawPageAstro() {
  if (drawnMode != 3) {
    // Sunrise and sunset side by side in tinted cards, moon phase below.
    int cardY = 88, cardW = 130, cardH = 95;
    int sunriseX = 25, sunsetX = 165;

    drawCard(sunriseX, cardY, cardW, cardH, 10, T.sunriseBg, T.sunriseFg, T.card);
    int scx = sunriseX + cardW / 2;
    tft.fillTriangle(scx - 8, cardY + 32, scx + 8, cardY + 32, scx, cardY + 17, T.sunriseFg); // up arrow
    tft.setTextDatum(MC_DATUM); tft.setTextColor(T.muted);
    tft.drawString("SUNRISE", scx, cardY + 48, 2);
    ffText(sunRise, scx, cardY + 74, &FreeSansBold12pt7b, MC_DATUM, T.sunriseFg);

    drawCard(sunsetX, cardY, cardW, cardH, 10, T.sunsetBg, T.sunsetFg, T.card);
    int stx = sunsetX + cardW / 2;
    tft.fillTriangle(stx - 8, cardY + 17, stx + 8, cardY + 17, stx, cardY + 32, T.sunsetFg); // down arrow
    tft.setTextDatum(MC_DATUM); tft.setTextColor(T.muted);
    tft.drawString("SUNSET", stx, cardY + 48, 2);
    ffText(sunSet, stx, cardY + 74, &FreeSansBold12pt7b, MC_DATUM, T.sunsetFg);

    // Moon phase card
    int moonCardY = cardY + cardH + 15;
    int moonCardH = 470 - moonCardY - 10;
    drawCard(25, moonCardY, 270, moonCardH, 10, T.card2, T.border, T.card);

    // The moon is drawn, not a photo: exact phase, theme colors, no edges.
    // Seen from the southern hemisphere (e.g. Australia) it's mirrored.
    float f = moonPhaseFraction();
    int pIdx = moonPhaseIndex(f);
    bool south = cityGeoCached[currentCity] && cityLat[currentCity] < 0;
    drawMoonPhase(160, moonCardY + 80, 46, f, south, T.card2);
    ffText(getMoonPhaseName(pIdx), 160, moonCardY + 150, &FreeSansBold9pt7b, MC_DATUM, T.text);

    // "62% lit - full moon in 4 days" (whichever of full/new moon comes first)
    int lit = (int)round((1 - cos(2 * PI * f)) / 2 * 100);
    float toFull = (f < 0.5f ? 0.5f - f : 1.5f - f) * MOON_SYNODIC_DAYS, toNew = (1 - f) * MOON_SYNODIC_DAYS;
    bool full = toFull < toNew;
    int days = (int)round(full ? toFull : toNew);
    String next = days == 0 ? String(full ? "full moon today" : "new moon today")
                            : String(full ? "full moon in " : "new moon in ") + days + (days == 1 ? " day" : " days");
    tft.setTextDatum(MC_DATUM); tft.setTextColor(T.muted);
    tft.drawString(String(lit) + "% lit - " + next, 160, moonCardY + 176, 2);
  }
  drawAstroCountdown(); // ticks live even while already on this page
}

void drawPageForecast() {
  if (drawnMode == 5) return;
  // Shared scale for the temperature range bars, so the days compare at a glance
  float mn = 999, mx = -999;
  for (int i = 0; i < 5; i++) if (forecast[i].valid) { mn = min(mn, forecast[i].low); mx = max(mx, forecast[i].high); }
  if (mx - mn < 1) mx = mn + 1;

  for (int i = 0; i < 5; i++) {
    int y = 90 + i * 74;
    drawCard(28, y, 264, 64, 12, T.card2, T.border, T.card);
    if (!forecast[i].valid) {
      tft.setTextDatum(MC_DATUM); tft.setTextColor(T.muted);
      tft.drawString("NO DATA", 160, y + 32, 2);
      continue;
    }
    ffText(forecast[i].dayName, 40, y + 32, &FreeSansBold12pt7b, ML_DATUM, T.text);
    drawWeatherIcon(iconKindFor(forecast[i].main, ""), 102, y + 12, 40, false, T.card2);

    String lo = String((int)round(forecast[i].low)), hi = String((int)round(forecast[i].high));
    tft.setTextDatum(MR_DATUM); tft.setTextColor(T.muted);
    tft.drawString(lo, 168, y + 33, 4);
    drawDegree(172, y + 24, 3, T.muted, T.card2);

    int bx = 182, bw = 50, by = y + 29;
    tft.fillSmoothRoundRect(bx, by, bw, 7, 3, T.line, T.card2);
    int a = bx + (int)((forecast[i].low - mn) / (mx - mn) * bw);
    int b = bx + (int)((forecast[i].high - mn) / (mx - mn) * bw);
    if (b - a < 7) b = a + 7;
    tft.fillSmoothRoundRect(a, by, b - a, 7, 3, T.accent, T.line);

    tft.setTextDatum(ML_DATUM); tft.setTextColor(T.text);
    tft.drawString(hi, 240, y + 33, 4);
    drawDegree(244 + tft.textWidth(hi, 4), y + 24, 3, T.text, T.card2);
  }
}

// On-device 7-day history for the current city (page 7) - highs/lows as two
// lines and daily rain as bars, read from the city's SD history log. Drawn
// once when you switch to the page (the data only changes a few times a day).
void drawPageHistory() {
  if (drawnMode == 6) return;
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(T.muted);
  tft.drawString(getDisplayCity(locs[currentCity]), 160, 86, 2);

  String dates[7]; float hi[7], lo[7], rn[7];
  int n = 0;
#if USE_SD_HISTORY
  n = histReadLastDays(currentCity, dates, hi, lo, rn, 7);
#endif
  if (n == 0) {
    tft.setTextColor(T.text);
    tft.drawString("NO HISTORY YET", 160, 250, 2);
    tft.setTextColor(T.muted);
    tft.drawString("(NEEDS THE SD CARD)", 160, 275, 1);
    return;
  }

  int left = 52, right = 268;
  int px[7];
  for (int i = 0; i < n; i++) px[i] = (n > 1) ? left + i * (right - left) / (n - 1) : 160;

  // --- temperature lines
  float tMinV = lo[0], tMaxV = hi[0];
  for (int i = 0; i < n; i++) { if (lo[i] < tMinV) tMinV = lo[i]; if (hi[i] > tMaxV) tMaxV = hi[i]; }
  float span = max(tMaxV - tMinV, 4.0f);
  float lowB = tMinV - span * 0.12f, highB = tMaxV + span * 0.12f;
  int top = 130, bot = 240;
  auto ty = [&](float v) { return (int)(bot - (v - lowB) / (highB - lowB) * (bot - top)); };

  tft.setTextColor(T.muted);
  tft.drawString("HIGH / LOW (C)", 160, 106, 1);
  for (int g = 0; g < 3; g++) tft.drawFastHLine(30, top + g * (bot - top) / 2, 260, T.line);
  for (int i = 0; i < n - 1; i++) {
    tft.drawWideLine(px[i], ty(hi[i]), px[i + 1], ty(hi[i + 1]), 2.5f, T.hi, T.card);
    tft.drawWideLine(px[i], ty(lo[i]), px[i + 1], ty(lo[i + 1]), 2.5f, T.lo, T.card);
  }
  for (int i = 0; i < n; i++) {
    tft.fillSmoothCircle(px[i], ty(hi[i]), 3, T.hi, T.card);
    tft.fillSmoothCircle(px[i], ty(lo[i]), 3, T.lo, T.card);
    tft.setTextColor(T.hi); // transparent text, so labels never punch holes in the lines
    tft.drawString(String((int)round(hi[i])), px[i], ty(hi[i]) - 11, 2);
    tft.setTextColor(T.lo);
    tft.drawString(String((int)round(lo[i])), px[i], ty(lo[i]) + 12, 2);
    tft.setTextColor(T.muted);
    tft.drawString(getDayName(dayOfWeekFromDateString(dates[i])), px[i], 270, 2);
  }

  // --- rain bars
  tft.setTextColor(T.muted);
  tft.drawString("RAIN (MM)", 160, 296, 1);
  int rTop = 318, rBase = 405;
  float rMax = 2.0f, tot = 0, hiSum = 0;
  for (int i = 0; i < n; i++) { if (rn[i] > rMax) rMax = rn[i]; tot += rn[i]; hiSum += hi[i]; }
  tft.drawFastHLine(30, rBase, 260, T.line);
  for (int i = 0; i < n; i++) {
    int h = (int)(rn[i] / rMax * (rBase - rTop));
    if (h > 0) tft.fillSmoothRoundRect(px[i] - 10, rBase - h, 20, h, min(4, h / 2), T.rain, T.card);
    if (rn[i] >= 0.05f) {
      tft.setTextColor(T.text);
      tft.drawString(String(rn[i], 1), px[i], rBase - h - 8, 1);
    }
    tft.setTextColor(T.muted);
    tft.drawString(dates[i].substring(8, 10), px[i], rBase + 10, 1);
  }

  // --- totals
  tft.setTextColor(T.text);
  tft.drawString(String(n) + "-DAY RAIN " + String(tot, 1) + " MM   AVG HIGH " + String(hiSum / n, 1) + " C", 160, 440, 2);
}

// The WiFi/sync/key status block, split out so it can be refreshed on its own
// while sitting on this page. Only redraws (and wipes the region) when
// something has actually changed, so the text doesn't flash every couple of
// seconds. forceDraw=true is used on first entering the page.
void drawSystemDynamicInfo(bool forceDraw = false) {
  static String lastSSID = "";
  static String lastIP = "";
  static int lastAgeMin = -1;
  static bool lastOK = false;
  static String lastKeySource = "";
  static int lastTextR = -1, lastTextG = -1, lastTextB = -1; // so the IP-text color preview updates live
  static bool everDrawn = false;

  String ssid = WiFi.SSID();
  String ip = WiFi.localIP().toString();
  int ageMin = lastFetchMillis == 0 ? 0 : (int)((millis() - lastFetchMillis) / 60000);
  String keySource = apiKeyBackup.length() > 0 ? (activeApiKeySource == "backup" ? "BACKUP" : "PRIMARY") : "";

  bool changed = !everDrawn || ssid != lastSSID || ip != lastIP || ageMin != lastAgeMin
                 || lastFetchOK != lastOK || keySource != lastKeySource
                 || textR != lastTextR || textG != lastTextG || textB != lastTextB;
  if (!forceDraw && !changed) return;

  everDrawn = true;
  lastSSID = ssid; lastIP = ip; lastAgeMin = ageMin; lastOK = lastFetchOK; lastKeySource = keySource;
  lastTextR = textR; lastTextG = textG; lastTextB = textB;

  // Clear first, so a shorter SSID/IP doesn't leave old characters behind
  tft.fillRect(20, 83, 280, 122, T.card);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(T.muted, T.card); tft.drawString("WIFI: " + ssid, 160, 95, 2);
  // The IP address doubles as the preview of the chosen text color
  tft.setTextColor(safeTextColor(textR, textG, textB), T.card);
  tft.drawString(ip, 160, 120, 4);

  tft.setTextColor(T.muted, T.card); tft.drawString("LAST SYNC", 160, 155, 2);
  tft.setTextColor(lastFetchOK ? T.good : T.bad, T.card);
  tft.drawString(lastFetchOK ? (String(ageMin) + " MIN AGO") : "FAILED", 160, 175, 2);

  if (apiKeyBackup.length() > 0) {
    tft.setTextColor(T.muted, T.card);
    tft.drawString("API KEY: " + keySource, 160, 196, 1);
  }
}

// Generic slider track+fill+thumb, used for screen and LED brightness
void drawSlider(int cy, int percent, uint16_t fillColor, int trackX = SLIDER_X, int trackW = SLIDER_W, int thumbR = SLIDER_THUMB_R) {
  percent = constrain(percent, 0, 100);
  // Clear the full thumb height first (the thumb is taller than the track),
  // otherwise a fast drag leaves a trail of old thumb arcs behind.
  int clearMargin = thumbR + 2;
  tft.fillRect(trackX - clearMargin, cy - clearMargin, trackW + 2 * clearMargin, 2 * clearMargin, T.card);

  int trackY = cy - SLIDER_TRACK_H / 2;
  tft.fillSmoothRoundRect(trackX, trackY, trackW, SLIDER_TRACK_H, SLIDER_TRACK_H / 2, T.line, T.card);
  int filledW = map(percent, 0, 100, 0, trackW);
  if (filledW > SLIDER_TRACK_H) tft.fillSmoothRoundRect(trackX, trackY, filledW, SLIDER_TRACK_H, SLIDER_TRACK_H / 2, fillColor, T.line);
  int thumbX = trackX + filledW;
  tft.fillSmoothCircle(thumbX, cy, thumbR, fillColor, T.card);
  tft.fillSmoothCircle(thumbX, cy, thumbR - 3, T.card, fillColor);
}

// forceDraw=true is used on page entry; otherwise this only redraws when the
// visible PERCENT changes, which avoids needless flicker while dragging.
void drawScreenBrightnessSlider(bool forceDraw = false) {
  static int lastPct = -1;
  int pct = map(globalBrightness, 10, 255, 0, 100);
  if (!forceDraw && pct == lastPct) return;
  lastPct = pct;
  tft.fillRect(20, SCREEN_LABEL_Y - 12, 280, 20, T.card); // clear label row (percent text width varies)
  tft.setTextDatum(MC_DATUM); tft.setTextColor(T.muted, T.card);
  tft.drawString("SCREEN BRIGHTNESS " + String(pct) + "%", 160, SCREEN_LABEL_Y, 2);
  drawSlider(SCREEN_SLIDER_Y, pct, T.accent);
}

void drawLedBrightnessSlider(bool forceDraw = false) {
  static int lastPct = -1;
  int pct = map(ledBrightness, 0, 255, 0, 100);
  if (!forceDraw && pct == lastPct) return;
  lastPct = pct;
  tft.fillRect(20, LED_LABEL_Y - 12, 280, 20, T.card);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(T.muted, T.card);
  tft.drawString("LED BRIGHTNESS " + String(pct) + "%", 160, LED_LABEL_Y, 2);
  drawSlider(LED_SLIDER_Y, pct, T.accent);
}

void drawButton(int x, int y, int w, int h, const String &label, bool active) {
  drawCard(x, y, w, h, 8, active ? T.accent : T.card2, active ? T.accent : T.border, T.card);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(active ? onAccentText() : T.text);
  tft.drawString(label, x + w / 2, y + h / 2 + 1, 2);
}

// Two buttons side by side: LED color mode (left) and screen theme (right).
void drawModeToggleButton() {
  drawButton(20, MODE_BTN_Y, 136, MODE_BTN_H, useCustomColor ? "LED: CUSTOM" : "LED: WEATHER", useCustomColor);
  drawButton(164, MODE_BTN_Y, 136, MODE_BTN_H, String("THEME: ") + themeModeName(), false);
}

// Hue picker: one continuous rainbow strip you drag along. The IP address
// above (drawSystemDynamicInfo) shows the live result.
void drawHuePicker(bool forceDraw = false) {
  static int lastHue = -1;

  if (!useCustomColor) {
    if (lastHue != -1 || forceDraw) {
      tft.fillRect(20, HUE_BAR_Y - 16, 280, HUE_BAR_H + 32, T.card);
      lastHue = -1;
    }
    return;
  }

  if (!forceDraw && customHue == lastHue) return; // nothing changed, skip redraw
  lastHue = customHue;

  // Clear just the bar's own footprint (plus thumb overhang) before redrawing
  tft.fillRect(HUE_BAR_X - 6, HUE_BAR_Y - 6, HUE_BAR_W + 12, HUE_BAR_H + 12, T.card);

  // Gradient (250 vertical lines - cheap on SPI) plus border
  for (int i = 0; i < HUE_BAR_W; i++) {
    int r, g, b;
    HSBtoRGB((i * 360) / HUE_BAR_W, 255, 255, r, g, b);
    tft.drawFastVLine(HUE_BAR_X + i, HUE_BAR_Y, HUE_BAR_H, tft.color565(r, g, b));
  }
  tft.drawRect(HUE_BAR_X - 1, HUE_BAR_Y - 1, HUE_BAR_W + 2, HUE_BAR_H + 2, T.border);

  // Thumb marker at the current hue's position
  int thumbX = HUE_BAR_X + (customHue * HUE_BAR_W) / 360;
  tft.drawRect(thumbX - 4, HUE_BAR_Y - 4, 8, HUE_BAR_H + 8, T.card);
  tft.drawRect(thumbX - 5, HUE_BAR_Y - 5, 10, HUE_BAR_H + 10, T.text);

  drawSystemDynamicInfo(); // refreshes the IP-text color preview live while dragging
}

void drawSaveButton(bool confirmed) {
  drawCard(50, SAVE_BTN_Y, 220, SAVE_BTN_H, 8, confirmed ? T.good : T.accent, confirmed ? T.good : T.accent, T.card);
  tft.setTextColor(confirmed ? TFT_WHITE : onAccentText()); tft.setTextDatum(MC_DATUM);
  tft.drawString(confirmed ? "SAVED!" : "SAVE CUSTOM AS DEFAULT", 160, SAVE_BTN_Y + SAVE_BTN_H / 2 + 1, 2);
}

void drawSystemUI() {
  drawSystemDynamicInfo(true); // force: the page was just wiped, draw regardless of cached state

  drawScreenBrightnessSlider(true);
  drawLedBrightnessSlider(true);
  drawModeToggleButton();
  drawHuePicker(true);
  drawSaveButton(false);
}

void sendNTPpacket(IPAddress& address) {
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  packetBuffer[0] = 0b11100011; packetBuffer[1] = 0; packetBuffer[2] = 6; packetBuffer[3] = 0xEC;
  packetBuffer[12] = 49; packetBuffer[13] = 0x4E; packetBuffer[14] = 49; packetBuffer[15] = 52;
  Udp.beginPacket(address, 123); Udp.write(packetBuffer, NTP_PACKET_SIZE); Udp.endPacket();
}

time_t getNtpTime() {
  IPAddress ntpServerIP;
  while (Udp.parsePacket() > 0); 
  WiFi.hostByName(ntpServerName, ntpServerIP); 
  sendNTPpacket(ntpServerIP); 
  uint32_t bWait = millis(); 
  while (millis() - bWait < 3000) { 
    if (Udp.parsePacket() >= NTP_PACKET_SIZE) { 
      Udp.read(packetBuffer, NTP_PACKET_SIZE);
      unsigned long s = (unsigned long)packetBuffer[40] << 24; 
      s |= (unsigned long)packetBuffer[41] << 16; 
      s |= (unsigned long)packetBuffer[42] << 8;
      s |= (unsigned long)packetBuffer[43]; 
      return s - 2208988800UL; 
    } 
  } 
  return 0;
}

// --- [MAIN SETUP] ---

void setup() { 
  Serial.begin(115200); 
  // NEW: default Stream timeout is 1000ms - if Serial Monitor's connection
  // handshake ever leaves a stray byte on the line without a newline
  // following it, handleSerialCommand()'s readStringUntil() would block for
  // a full second waiting for a terminator that never comes. Shortening
  // this makes any such hiccup barely noticeable instead of looking like
  // the device has frozen.
  Serial.setTimeout(50);
  pinMode(LCD_BL_PIN, OUTPUT); pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT); 
  tft.begin(); tft.setRotation(0);
  touch_calibrate(); 
  TJpgDec.setJpgScale(1); TJpgDec.setSwapBytes(true); TJpgDec.setCallback(tft_output); 
  prefs.begin("weather_app", false); 

#if USE_SD_HISTORY
  // NEW: SD card for the daily history log, on its own explicit SPI bus/pins
  // (confirmed from the manual) - kept entirely separate from whatever bus
  // TFT_eSPI is using for the display, so initializing the card can't
  // disturb the screen's SPI routing. Purely optional - if there's no card,
  // this just fails quietly and history logging stays off.
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  sdOK = SD.begin(SD_CS_PIN, sdSPI);
  if (!sdOK) Serial.println("SD card not found - history logging disabled");
#endif

  // UPDATE 3: Load API Key from Memory
  String s1 = prefs.getString("loc1", "Amsterdam,NL"); 
  String s2 = prefs.getString("loc2", "South Pole,AQ");
  String s3 = prefs.getString("loc3", "London,GB"); 
  apiKey = prefs.getString("api", "");
  apiKeyBackup = prefs.getString("apiBackup", ""); // NEW
  cityProvider[0] = prefs.getInt("provider0", PROVIDER_OPENWEATHER); // NEW
  cityProvider[1] = prefs.getInt("provider1", PROVIDER_OPENWEATHER);
  cityProvider[2] = prefs.getInt("provider2", PROVIDER_OPENWEATHER);

  s1.toCharArray(city1_buf, 40); s2.toCharArray(city2_buf, 40); s3.toCharArray(city3_buf, 40); 
  locs[0] = s1; locs[1] = s2; locs[2] = s3;

  globalBrightness = prefs.getInt("bright", 255); ledBrightness = prefs.getInt("ledB", 255);
  useCustomColor = prefs.getBool("useC", false); 
  custR = prefs.getInt("cR", 255); custG = prefs.getInt("cG", 255); custB = prefs.getInt("cB", 255); 
  if (prefs.getInt("cHue", -1) >= 0) {
    // Saved by this version: restore the exact hue/saturation/lightness
    customHue = prefs.getInt("cHue", 0);
    customSat = prefs.getInt("cSat", 255);
    customVal = prefs.getInt("cVal", 255);
  } else {
    customHue = rgbToHueApprox(custR, custG, custB); // older saves only stored R/G/B - approximate once at boot
  }

  // NEW: independent text color (clock minutes + IP), loaded separately from the LED's color
  textHue = prefs.getInt("txHue", 0);
  textSat = prefs.getInt("txSat", 0);
  textVal = prefs.getInt("txVal", 0);
  applyTextColor();
  showAnimation = prefs.getBool("animOn", true); // NEW
  showRainPreview = prefs.getBool("rainPrevOn", true); // NEW
  rainLedAlert = prefs.getBool("rainLed", true);
  vfLoad();
  netBegin(); // background task for the rain/pressure/verification requests
  {
    esp_reset_reason_t r = esp_reset_reason();
    const char *why = r == ESP_RST_POWERON ? "power on" : r == ESP_RST_SW ? "restart" : r == ESP_RST_PANIC ? "CRASH (panic)"
      : r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ? "CRASH (watchdog)" : r == ESP_RST_BROWNOUT ? "brownout (power dip)" : "other";
    logDebug("BOOT: reset reason " + String((int)r) + " - " + why);
  }
  themeMode = constrain(prefs.getInt("theme", THEME_AUTO), 0, 2);
  applyTheme(); // AUTO can't know about sunset yet - re-checked after the first weather fetch
  updateBacklight(); 

  // NEW: try reconnecting with saved WiFi credentials first, with a few retries,
  // before dropping into the AP config portal. Avoids a brief router hiccup
  // forcing you to redo WiFi setup on a wall-mounted device.
  tft.fillScreen(bgColor); tft.setTextColor(T.text); tft.setTextDatum(CC_DATUM);
  tft.drawString("CONNECTING WIFI...", 160, 240, 2);
  WiFi.mode(WIFI_STA);
  WiFi.begin(); // ESP32 will use the last-saved credentials stored in NVS
  int wifiRetries = 0;
  while (WiFi.status() != WL_CONNECTED && wifiRetries < 6) {
    delay(1500);
    wifiRetries++;
    tft.fillRect(40, 260, 240, 20, bgColor);
    tft.drawString("RETRY " + String(wifiRetries) + "/6", 160, 265, 2);
  }

  if (WiFi.status() != WL_CONNECTED) {
    // Retries failed (or no credentials saved yet) - fall back to the config portal
    WiFiManager wm; wm.setAPCallback(configModeCallback);
    WiFiManagerParameter custom_city1("c1", "City 1", city1_buf, 40); 
    WiFiManagerParameter custom_city2("c2", "City 2", city2_buf, 40); 
    WiFiManagerParameter custom_city3("c3", "City 3", city3_buf, 40); 
    wm.addParameter(&custom_city1); wm.addParameter(&custom_city2);
    wm.addParameter(&custom_city3); 
    if (!wm.autoConnect("WeatherStation-Setup")) ESP.restart(); 

    prefs.putString("loc1", custom_city1.getValue());
    prefs.putString("loc2", custom_city2.getValue());
    prefs.putString("loc3", custom_city3.getValue());
    locs[0] = custom_city1.getValue(); locs[1] = custom_city2.getValue(); locs[2] = custom_city3.getValue();
  }

  // NEW: load any cached geocode results (only valid if the location string still matches)
  for (int i = 0; i < NUM_CITIES; i++) {
    String cachedLoc = prefs.getString(("geoLoc" + String(i)).c_str(), "");
    if (cachedLoc.length() > 0 && cachedLoc == locs[i]) {
      cityLat[i] = prefs.getFloat(("lat" + String(i)).c_str(), 0);
      cityLon[i] = prefs.getFloat(("lon" + String(i)).c_str(), 0);
      cityGeoCached[i] = true;
      cityGeoLocStr[i] = cachedLoc;
    }
  }

#if USE_BME280
  // NEW: optional local sensor - safe to leave enabled even without hardware attached,
  // bme.begin() just returns false and readings are skipped.
  Wire.begin(BME_SDA_PIN, BME_SCL_PIN);
  bmeOK = bme.begin(0x76) || bme.begin(0x77);
  if (bmeOK) readLocalSensor();
#endif

#if USE_SD_HISTORY
  histBootId = esp_random(); // makes this boot's history ETags unique
  historySetup(); // NEW: per-city history files (migrates the old /history.csv to City 1 on first boot)
#endif

  // The WebServer only keeps request headers it's told about up front
  const char* collectedHeaders[] = { APP_HEADER_NAME, "If-None-Match" };
  server.collectHeaders(collectedHeaders, 2);
  server.on("/", handleDashboardPage);         // the live dashboard is the start page
  server.on("/settings", handleSettingsPage);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/api/settings", handleApiSettings); // NEW: current settings for the Settings page
  server.on("/app.css", handleAppCss);           // NEW: shared stylesheet (cached by the browser)
  server.on("/app.js", handleAppJs);             // NEW: shared script: theme, icons, 7-segment clock
  server.on("/live", handleLive);           // NEW: live slider updates (color, brightness, mode)
  server.on("/saveColor", handleSaveColor); // NEW: persists the current custom color as the boot default
  server.on("/saveTextColor", handleSaveTextColor); // NEW: persists the current text color as the boot default
  server.on("/dashboard", handleDashboardPage); // old address, kept so existing bookmarks still work
  server.on("/api/status", handleApiStatus);    // NEW: JSON feed for the dashboard
  server.on("/debug", handleDebugPage);         // NEW: live touch/system debug console
  server.on("/api/debug", handleApiDebug);      // NEW: JSON feed for the debug console
#if USE_SD_HISTORY
  server.on("/history", handleHistoryPage);     // NEW: charted history page
  server.on("/api/history", handleApiHistory);  // NEW: JSON feed for the history page
  server.on("/api/history/meta", handleHistoryMeta);                                      // NEW: page metadata + today's forecast rows
  server.on("/api/history/raw", handleHistoryRaw);                                        // NEW: raw history CSV (fast, cacheable)
  server.on("/api/history/export", handleHistoryExport);                                  // NEW: CSV download per city
  server.on("/api/history/import", HTTP_POST, handleHistoryImportDone, handleHistoryUpload); // NEW: CSV upload (merge/replace)
  server.on("/api/history/wipe", HTTP_POST, handleHistoryWipe);                            // NEW: wipe a city / all / archived logs / local log
  server.on("/api/history/status", handleHistoryStatus);                                  // NEW: backfill progress
  server.on("/api/history/backfill", HTTP_POST, handleHistoryBackfill);                    // NEW: start/stop archive backfill
  server.on("/api/local", handleApiLocal);                                                // NEW: hourly local sensor (BME280) readings
  server.on("/api/local/export", handleLocalExport);                                      // NEW: local sensor log as CSV
#endif
  server.begin();

  tft.fillScreen(bgColor); tft.setTextColor(T.text); tft.setTextDatum(CC_DATUM);
  tft.drawString("SYNCING TIME...", 160, 240, 2);

  Udp.begin(localPort); 
  setSyncProvider(getNtpTime);

  tft.fillScreen(bgColor); tft.drawString("FETCHING WEATHER...", 160, 240, 2);
  getOpenWeatherData();
  applyTheme();

  drawPageBackground(displayMode);
  drawnMode = -1;
}

// NEW: read-only serial debug commands. Purely reports current state -
// never changes anything - so it can't interfere with normal operation.
// Type "ping" or "status" into the Serial Monitor's message box and hit
// Enter (line ending set to "New Line" or "Newline" works fine).
void handleSerialCommand() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toLowerCase();

  if (cmd == "ping") {
    Serial.println("PONG");
  } else if (cmd == "status") {
    Serial.println("--- STATUS ---");
    Serial.println("Uptime: " + String(millis() / 1000) + "s");
    Serial.println("WiFi: " + String(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED"));
    Serial.println("SSID: " + WiFi.SSID());
    Serial.println("IP: " + WiFi.localIP().toString());
    unsigned long ageMin = lastFetchMillis == 0 ? 0 : (millis() - lastFetchMillis) / 60000;
    Serial.println("Last fetch: " + String(lastFetchOK ? "OK" : "FAILED") + " (" + String(ageMin) + " min ago)");
    Serial.println("Current city: " + locs[currentCity]);
    Serial.println("Display mode: " + String(displayMode));
    Serial.println("Free heap: " + String(ESP.getFreeHeap()) + " bytes");
    Serial.println("--------------");
  } else if (cmd.length() > 0) {
    Serial.println("Unknown command. Try: ping, status");
  }
}

// Times one part of loop(): anything over a second is logged with its name, so
// a "LOOP blocked" line in the Debug log comes with the part that caused it.
// (The name is evaluated after the call, so it can describe what just ran.)
#define TIMED(name, call) do { unsigned long _t0 = millis(); call; unsigned long _dt = millis() - _t0; \
  if (_dt > 1000) logDebug(String("SLOW ") + (name) + ": " + String(_dt) + " ms"); } while (0)

void loop() {
  // Watch for anything that holds up the main loop (touch and web pages wait meanwhile)
  static unsigned long lastLoopAt = 0;
  unsigned long loopNow = millis();
  if (lastLoopAt != 0) {
    unsigned long gap = loopNow - lastLoopAt;
    if (gap > loopMaxMs) loopMaxMs = gap;
    if (gap > 1500) logDebug("LOOP blocked for " + String(gap) + " ms");
  }
  lastLoopAt = loopNow;
  TIMED(String("web page ") + server.uri(), server.handleClient());
  TIMED("WiFi check", checkWifiHealth()); // NEW: watches for a dropped connection and recovers automatically
  handleSerialCommand(); // NEW: read-only ping/status over Serial
  saveBrightnessIfDue(); // persists slider changes once you've stopped dragging
#if USE_SD_HISTORY
  TIMED(hmActive ? "history (SD merge)" : "history", historyLoop()); // NEW: background per-city history sync (one city at a time, every 6h)
#endif
  TIMED("background jobs", nowcastLoop()); // rain in the next 2 hours, pressure trend, LED rain alert

  // Text color changed (web sliders) -> redraw once it has settled
  if (accentDirty && millis() - accentChangedAt > 400) {
    accentDirty = false;
    drawPageBackground(displayMode); drawnMode = -1;
  }

  // AUTO theme: switch between light and dark at sunrise/sunset
  static unsigned long lastThemeCheck = 0;
  if (millis() - lastThemeCheck > 30000) {
    lastThemeCheck = millis();
    if (applyTheme()) { drawPageBackground(displayMode); drawnMode = -1; }
  }

  // UPDATE 4: Force display redraw on 10-minute auto update (now via the
  // non-blocking-feeling wrapper, so the screen doesn't flash blank)
  if (millis() - weatherUpdateTimer > 600000) { 
    weatherUpdateTimer = millis();
    triggerWeatherUpdate();
  }

#if USE_BME280
  // NEW: periodic local sensor read, independent of the network weather timer
  if (bmeOK && millis() - lastBmeRead > 5000) {
    lastBmeRead = millis();
    TIMED("indoor sensor", readLocalSensor());
#if USE_SD_HISTORY
    TIMED("indoor log", localLogSample()); // NEW: hourly averages to /local.csv on the SD card
#endif
  }
#endif

  // NEW: smooth the raw touch reading with an exponential moving average.
  // Resistive touchscreens commonly have a few pixels of read-to-read noise
  // even with a perfectly steady finger - since every slider reads this
  // coordinate directly, that noise was showing up as jitter/jumping on
  // ALL of them (brightness sliders included, not just the hue picker).
  // Smoothing once here, at the single point touch is read, fixes it
  // everywhere at once rather than needing a per-slider workaround.
  static float smoothTouchX = -1, smoothTouchY = -1;
  uint16_t rawX = 0, rawY = 0;
#if DIAG_DISABLE_TOUCH
  bool pressed = false; // DIAGNOSTIC: touch read skipped - see DIAG_DISABLE_TOUCH above
#else
  bool pressed = tft.getTouch(&rawX, &rawY);
#endif
  uint16_t x, y;
  if (pressed) {
    if (smoothTouchX < 0) { smoothTouchX = rawX; smoothTouchY = rawY; } // first frame of this touch - no history yet
    else {
      smoothTouchX = smoothTouchX * 0.6f + rawX * 0.4f;
      smoothTouchY = smoothTouchY * 0.6f + rawY * 0.4f;
    }
    x = (uint16_t)(smoothTouchX + 0.5f);
    y = (uint16_t)(smoothTouchY + 0.5f);
  } else {
    smoothTouchX = -1; smoothTouchY = -1; // reset so the next touch-down starts fresh, not eased in from the old position
    x = rawX; y = rawY;
  }
  
  debugRawX = rawX; debugRawY = rawY; debugPressed = pressed; // NEW: mirror for the /debug page

  if (pressed && !isTouched) {
    isTouched = true; touchHandled = false;
    touchStartTime = millis(); startX = x; startY = y;
    logDebug("TOUCH DOWN x=" + String(x) + " y=" + String(y));
  }

  // NEW: stuck/ghost-touch watchdog. Every touch action in this UI (tap to
  // switch pages, arrows, sliders, long-press) only fires on RELEASE
  // (!pressed && isTouched below). If the touch controller ever reports a
  // continuous "still pressed" reading that never lets go - a known
  // real-world quirk with resistive touch + the XPT2046 chip on this board,
  // from dust, moisture, or a marginal IRQ signal - NOTHING touch-related
  // can ever fire again, while everything else (the clock, rain preview,
  // etc.) keeps updating fine, since none of that depends on touch. This
  // looks exactly like "frozen" but isn't - it's a stuck touch reading.
  // The longest legitimate deliberate hold in this UI is the ~5s
  // long-press-to-switch-city gesture, so anything held far longer than
  // that is almost certainly a stuck reading, not a real touch - silently
  // abandon it (no action fires) so the UI recovers on its own instead of
  // needing a power cycle.
  const unsigned long STUCK_TOUCH_MS = 8000;
  if (pressed && isTouched && millis() - touchStartTime > STUCK_TOUCH_MS) {
    isTouched = false; touchHandled = true;
    smoothTouchX = -1; smoothTouchY = -1;
    debugStuckResetCount++;
    logDebug("STUCK TOUCH auto-reset (#" + String(debugStuckResetCount) + ") - held " + String((millis() - touchStartTime) / 1000) + "s");
  }

  // NEW: continuous slider dragging - checked every frame while the screen
  // is being touched (not just on initial touch-down or release), so the
  // slider follows your finger smoothly instead of only reading the tap's
  // starting position. Placed after the isTouched/touchStartTime setup
  // above (not before it) so this doesn't get its touchHandled flag
  // clobbered by that block resetting it to false on the same first frame.
  if (pressed && displayMode == 4) {
    if (y > SCREEN_SLIDER_Y - 20 && y < SCREEN_SLIDER_Y + 20) {
      int pct = constrain(map((int)x, SLIDER_X, SLIDER_X + SLIDER_W, 0, 100), 0, 100);
      int newBrightness = map(pct, 0, 100, 10, 255);
      if (newBrightness != globalBrightness) {
        globalBrightness = newBrightness;
        updateBacklight();
        markBrightnessDirty();
        drawScreenBrightnessSlider();
      }
      touchHandled = true;
    } else if (y > LED_SLIDER_Y - 20 && y < LED_SLIDER_Y + 20) {
      int pct = constrain(map((int)x, SLIDER_X, SLIDER_X + SLIDER_W, 0, 100), 0, 100);
      int newLed = map(pct, 0, 100, 0, 255);
      if (newLed != ledBrightness) {
        ledBrightness = newLed;
        setWeatherLED(mainWeather);
        markBrightnessDirty();
        drawLedBrightnessSlider();
      }
      touchHandled = true;
    } else if (useCustomColor && y > HUE_BAR_Y - 16 && y < HUE_BAR_Y + HUE_BAR_H + 16) {
      int hue = constrain(map((int)x, HUE_BAR_X, HUE_BAR_X + HUE_BAR_W, 0, 360), 0, 360);
      if (hue != customHue) {
        customHue = hue;
        applyCustomColor(); // NEW: uses whatever saturation/brightness the web portal's sliders last set
        drawHuePicker();
      }
      touchHandled = true;
    }
  }

  if (!pressed && isTouched) {
    unsigned long dur = millis() - touchStartTime;
    logDebug("RELEASE dur=" + String(dur) + "ms x=" + String(startX) + " y=" + String(startY) + " mode=" + String(displayMode));
    // !touchHandled here matters: if the last few frames were a slider drag,
    // touchHandled is already true, so a release landing over e.g. the mode
    // button doesn't also fire a second, unintended action.
    if (!touchHandled && displayMode == 4 && dur < 600) {
      int tx = startX, ty = startY;
      if (ty > MODE_BTN_Y && ty < MODE_BTN_Y + MODE_BTN_H && tx >= 160) {
        // Theme button: AUTO -> LIGHT -> DARK -> AUTO
        themeMode = (themeMode == THEME_AUTO) ? THEME_LIGHT : (themeMode == THEME_LIGHT ? THEME_DARK : THEME_AUTO);
        prefs.putInt("theme", themeMode);
        applyTheme();
        drawPageBackground(displayMode); drawnMode = -1; // full redraw in the new colors
        touchHandled = true;
      } else if (ty > MODE_BTN_Y && ty < MODE_BTN_Y + MODE_BTN_H) {
        useCustomColor = !useCustomColor; setWeatherLED(mainWeather); prefs.putBool("useC", useCustomColor);
        drawModeToggleButton();
        drawHuePicker(true); // force: show/hide as mode changes
        drawSystemDynamicInfo(); // refreshes the IP-text color preview to match the new mode's LED color
        touchHandled = true;
      } else if (ty > SAVE_BTN_Y && ty < SAVE_BTN_Y + SAVE_BTN_H) {
        saveCustomColor();
        showSaveConfirmation = true; saveConfirmationTime = millis();
        drawSaveButton(true);
        touchHandled = true;
      }
    }

    // City switch arrows beside the city name, home page only
    if (!touchHandled && displayMode == 0 && dur < 600 && startY > 5 && startY < 45) {
      if (startX < 70) {
        currentCity = (currentCity - 1 + NUM_CITIES) % NUM_CITIES;
        triggerWeatherUpdate(); touchHandled = true;
      } else if (startX > 250) {
        currentCity = (currentCity + 1) % NUM_CITIES;
        triggerWeatherUpdate(); touchHandled = true;
      }
    }

    if (!touchHandled && dur > 1500 && dur < 5000) {
        currentCity = (currentCity + 1) % NUM_CITIES;
        triggerWeatherUpdate(); touchHandled = true;
    }
    if (!touchHandled && dur < 600) {
      if (startX < 160) displayMode = (displayMode + 6) % 7; else displayMode = (displayMode + 1) % 7;
      drawPageBackground(displayMode);
    }
    logDebug(touchHandled ? "-> action fired" : "-> no action matched");
    isTouched = false;
  }
  
  unsigned long drawT0 = millis();
  int drawFrom = drawnMode;
  if (displayMode == 0) drawPageHome();
  else if (displayMode == 1) drawPageStats();
  else if (displayMode == 2) drawPageRain();
  else if (displayMode == 3) drawPageAstro();
  else if (displayMode == 4) {
    if (drawnMode != 4) {
      drawSystemUI(); // first entry to the page - draw everything, including buttons
      lastSystemInfoRefresh = millis();
    } else if (millis() - lastSystemInfoRefresh > SYSTEM_INFO_REFRESH_MS) {
      lastSystemInfoRefresh = millis();
      drawSystemDynamicInfo(); // already on the page - just refresh WiFi/sync status
    }
    // NEW: revert the "SAVED!" confirmation back to normal after ~1 second
    if (showSaveConfirmation && millis() - saveConfirmationTime > SAVE_CONFIRMATION_MS) {
      showSaveConfirmation = false;
      drawSaveButton(false);
    }
  }
  else if (displayMode == 5) drawPageForecast();
  else if (displayMode == 6) drawPageHistory(); // NEW: on-device 7-day history
  if (millis() - drawT0 > 1000) logDebug("SLOW drawing page " + String(displayMode) + (drawFrom != displayMode ? " (full redraw)" : "") + ": " + String(millis() - drawT0) + " ms");

  drawnMode = displayMode;
}

// --- [HISTORY WEB PAGE] ---
// Kept at the very end of the sketch on purpose: the page's JavaScript contains
// lots of quote characters, which confuse the Arduino IDE's automatic
// prototype generator for any C++ that comes after it.
#if USE_SD_HISTORY
const char HISTORY_PAGE_TOP[] PROGMEM = R"rawliteral(<style>
.hist{max-width:980px}
.hist .btnRow{margin:6px 0;display:flex;gap:8px;flex-wrap:wrap;justify-content:center;align-items:center}
#modeRow,#cityRow,#metricRow,#rangeRow,#groupRow,#lRangeRow{display:inline-flex;gap:3px;background:var(--card2);padding:4px;border-radius:11px;border:1px solid var(--line);flex-wrap:wrap;justify-content:center}
.btnRow button{border:0;background:none;color:var(--muted);padding:6px 12px;border-radius:8px;font:inherit;font-weight:600;cursor:pointer}
.btnRow button.active{background:var(--card);color:var(--text);box-shadow:var(--shadow)}
.split{display:inline-block;width:6px}
.ctl{text-align:center}
label.chk{font-size:.9em;color:var(--muted);cursor:pointer;display:inline-flex;gap:6px;align-items:center;margin-top:6px}
.hist h2{font-size:1rem;margin:22px 0 4px;text-align:center} .hist h2:first-child{margin-top:4px}
.hist h2.sec{margin-top:4px}
canvas{width:100%;max-width:100%;display:block;margin:0 auto;border-radius:10px}
#tooltip{position:fixed;background:#16202f;color:#fff;padding:6px 10px;border-radius:8px;font-size:.85em;pointer-events:none;display:none;text-align:left;z-index:30;box-shadow:0 6px 20px rgba(0,0,0,.3)}
#noData{color:var(--muted);text-align:center}
#storageBox{margin:12px auto 0;max-width:520px;font-size:.9em;color:var(--muted)}
#storageBar{height:8px;background:var(--line);border-radius:4px;overflow:hidden;margin-top:6px}
#storageFill{height:100%;width:0;background:var(--blue)}
.legend{display:flex;justify-content:center;gap:16px;flex-wrap:wrap;font-size:.86em;margin:2px 0 6px;color:var(--muted)}
.legend span{display:inline-flex;align-items:center;gap:6px}
.legend .sw,.tw .sw{width:16px;height:4px;border-radius:2px;display:inline-block;vertical-align:middle}
.tw{overflow-x:auto}
tr.sel td{background:var(--card2);font-weight:600}
.hist .note{text-align:center}
details#manage{background:var(--card);border:1px solid var(--line);border-radius:18px;padding:14px 20px;box-shadow:var(--shadow)}
details#manage .note{text-align:left}
summary{cursor:pointer;font-weight:700;color:var(--text)}
.mg{margin:18px 0}.mg h3{font-size:.76em;letter-spacing:.08em;text-transform:uppercase;margin:0 0 8px;color:var(--muted)}
.mg button,.mg select,.mg input[type=file]{margin:3px 6px 3px 0;padding:7px 12px;border-radius:9px;border:1px solid var(--line);background:var(--card2);color:var(--text);font:inherit;cursor:pointer}
.mg select{width:auto}
.mg button.danger{border-color:color-mix(in srgb,var(--bad) 40%,var(--line));color:var(--bad)}
.mg button:disabled{opacity:.5;cursor:default}
#bfStatus div{margin:2px 0}
.bar{display:inline-block;width:120px;height:8px;background:var(--line);border-radius:4px;overflow:hidden;vertical-align:middle;margin:0 6px}
.bar i{display:block;height:100%;background:var(--blue)}
#msg{font-size:.9em;margin-top:8px}
</style>
<main class='page hist'><h1>Weather history</h1>
)rawliteral";

const char HISTORY_PAGE_BOTTOM[] PROGMEM = R"rawliteral(
<section class='card ctl'><div class='btnRow'><span id='modeRow'><button data-m='single'>Single city</button><button data-m='compare'>Compare cities</button></span></div>
<div id='singleCtl'>
<div class='btnRow'><span id='cityRow'></span></div>
<label class='chk'><input type='checkbox' id='sameScale'> Same scale for all cities</label>
</div>
<div id='compareCtl' style='display:none;'>
<div class='btnRow'><span id='metricRow'><button data-k='tMax'>Highs</button><button data-k='tMin'>Lows</button><button data-k='mean'>Average</button></span></div>
</div>
<div class='btnRow'><span id='rangeRow'><button data-d='7'>7D</button><button data-d='30'>30D</button><button data-d='90'>90D</button><button data-d='365'>1Y</button><button data-d='0'>All</button></span><span class='split'></span><span id='groupRow'><button data-g='day'>Daily</button><button data-g='month'>Monthly</button></span></div>
<div id='storageBox'><div id='storage'>Loading...</div><div id='storageBar'><div id='storageFill'></div></div></div></section>
<div id='noData' class='card' style='display:none;'></div>
<div id='content' class='card' style='display:none;'>
<h2 id='t_temp'></h2><div class='legend' id='l_temp'></div><canvas id='c_temp' width='840' height='280'></canvas>
<h2 id='t_rain'></h2><div class='legend' id='l_rain'></div><canvas id='c_rain' width='840' height='200'></canvas>
<h2 id='t_sun'></h2><div class='legend' id='l_sun'></div><canvas id='c_sun' width='840' height='180'></canvas>
<h2 id='t_hum'></h2><div class='legend' id='l_hum'></div><canvas id='c_hum' width='840' height='170'></canvas>
<h2 id='t_pres'></h2><div class='legend' id='l_pres'></div><canvas id='c_pres' width='840' height='170'></canvas>
<h2 id='t_wind'></h2><div class='legend' id='l_wind'></div><canvas id='c_wind' width='840' height='170'></canvas>
<h2>Summary for this period</h2>
<div class='tw'><table id='sumTable'></table></div>
<h2>All-time records</h2>
<div class='tw'><table id='recTable'></table></div>
<div class='note'>Daily values from Open-Meteo for every city, whichever city the device is showing. The light point/bar at the end is today's forecast and isn't counted in summaries or records. Pressure is the sea-level value, so cities at different heights compare fairly. In the monthly view the current month is "so far".</div>
</div>

<div id='localSec' class='card' style='display:none;'>
<h2 class='sec'>Local sensor (BME280)</h2>
<div class='btnRow'><span id='lRangeRow'><button data-h='24'>24H</button><button data-h='168'>7D</button><button data-h='720'>30D</button><button data-h='8760'>1Y</button><button data-h='0'>All</button></span></div>
<div class='legend' id='lStats'></div>
<h2>Temperature (&deg;C)</h2><canvas id='c_lsT' width='840' height='170'></canvas>
<h2>Humidity (%)</h2><canvas id='c_lsH' width='840' height='150'></canvas>
<h2>Pressure (hPa at the sensor &ndash; not sea-level corrected)</h2><canvas id='c_lsP' width='840' height='150'></canvas>
<div class='note'>Hourly averages measured by the sensor on the device itself. The last point is the current hour so far.</div>
</div>

<details id='manage'><summary>Manage history data</summary>
<div class='mg'><h3>Backfill older history (Open-Meteo archive)</h3>
<select id='bfCity'></select> back <select id='bfYears'><option value='1'>1 year</option><option value='2'>2 years</option><option value='5' selected>5 years</option><option value='10'>10 years</option></select>
<button onclick='doBackfill()'>Start</button><button onclick='stopBackfill()'>Stop</button>
<div id='bfStatus' class='note' style='text-align:left;'></div>
<div class='note' style='text-align:left;'>Runs in the background on the device (one 90-day chunk about every 15 seconds) and only fills days that are missing, so it's safe to run again. 5 years for all three cities uses roughly 1,000 of Open-Meteo's 10,000 free daily API calls; 10 years roughly 2,000. It continues after a restart.</div>
</div>
<div class='mg'><h3>Export to CSV</h3><div id='expRow'></div></div>
<div class='mg'><h3>Import from CSV</h3>
Into <select id='impCity'></select><input type='file' id='impFile' accept='.csv,.txt'><br>
<label><input type='radio' name='impMode' value='merge' checked> Merge &ndash; imported days replace matching dates, everything else is kept</label><br>
<label><input type='radio' name='impMode' value='replace'> Replace &ndash; this city's history becomes exactly the file</label><br>
<button onclick='doImport()'>Import</button>
<div class='note' style='text-align:left;'>Columns: date,tMin,tMax,rainMm,humidAvg,pressureAvg,gustMaxKmh,sunHours (date as YYYY-MM-DD; everything after tMax is optional). Files exported here re-import as-is, and semicolon-separated files with decimal commas (Dutch Excel) work too.</div>
</div>
<div class='mg'><h3>Wipe</h3>
<select id='wipeCity'></select><button class='danger' onclick='doWipe()'>Wipe</button>
<button class='danger' id='archBtn' onclick='doWipeArchives()'>Delete archived logs</button>
<div class='note' style='text-align:left;'>After wiping a city the device downloads the last 92 days again within about a minute; anything older is gone unless you exported it first (or backfill it again). Archived logs are the old files kept when you rename a city.</div>
</div>
<div id='msg'></div>
</details>
<div id='tooltip'></div>

<script>
var COLORS=['#e07a1f','#2e86ab','#8e44ad'],MONTHS=['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
var S={cities:[],meta:null,sel:0,range:30,mode:'single',metric:'tMax',same:true,group:'day',lrange:168,loaded:false,local:null};
try{var sv=JSON.parse(localStorage.getItem('histView')||'{}');['mode','metric','range','same','group','lrange'].forEach(k=>{if(sv[k]!==undefined)S[k]=sv[k];});}catch(e){}
var $=function(id){return document.getElementById(id);}
var CH={};['temp','rain','sun','hum','pres','wind','lsT','lsH','lsP'].forEach(k=>{var c=$('c_'+k);CH[k]={cv:c,ctx:c.getContext('2d')};});
var tip=$('tooltip'),PAD={l:46,r:14,t:12,b:26},view=null,lview=null;
var PRES_HINT='<span style="color:var(--muted);">high = settled weather &middot; low or falling = unsettled, rain/wind likely</span>';
var SAME_NOTE='<span style="color:var(--muted);">same scale as the other cities</span>';
var NO_EXTRA='No wind/sunshine data here yet - it is added by the next sync or a backfill';
$('sameScale').checked=S.same;

var fmtKB=function(kb){if(kb>=1048576)return (kb/1048576).toFixed(1)+' GB';if(kb>=1024)return (kb/1024).toFixed(1)+' MB';return kb+' KB';}
var parseDays=function(a){return a.map(x=>({date:x[0],tMin:x[1],tMax:x[2],rain:x[3],humid:x[4],pressure:x[5],gust:x[6],sun:x[7],partial:x.length>8}));}
var leg=function(c,t){return '<span><i class="sw" style="background:'+c+'"></i>'+t+'</span>';}
var setT=function(k,t,l){$('t_'+k).innerHTML=t;$('l_'+k).innerHTML=l||'';}
var nz=function(v){return v>0?v:null;}
var clean=function(a){return a.filter(v=>v!=null);}
var rangeOf=function(a){a=clean(a);return a.length?[Math.min(...a),Math.max(...a)]:null;}
var avgOf=function(a){a=clean(a);return a.length?a.reduce((x,y)=>x+y,0)/a.length:null;}
var sumOf=function(a){a=clean(a);return a.length?a.reduce((x,y)=>x+y,0):null;}
var maxOf=function(a){a=clean(a);return a.length?Math.max(...a):null;}
var dayNum=function(s){return Math.round(Date.UTC(+s.slice(0,4),+s.slice(5,7)-1,+s.slice(8,10))/86400000);}
var monthTitle=function(k){return MONTHS[+k.slice(5,7)-1]+' '+k.slice(0,4);}
var lbl=function(d,n){if(d.length==7)return MONTHS[+d.slice(5,7)-1]+" '"+d.slice(2,4);return n>400?d.slice(0,7):d.slice(5);}

// ---------- data
// Raw CSV from /api/history/raw -> the same row arrays /api/history returns.
// Same validation as the device used to do: numeric fields only, and skip the
// old logger's 999/-999 placeholder days.
var NUMF=/^-?[0-9]*\.?[0-9]+$/;
var parseCsv=function(t){var out=[];t.split('\n').forEach(l=>{l=l.trim();if(!l)return;var f=l.split(',');
  if(f.length<6||f[0].length!=10)return;var v=[];
  for(var k=1;k<6;k++){if(!NUMF.test(f[k]))return;v.push(+f[k]);}
  if(v[0]>900||v[1]<-900)return;
  var opt=k=>(f.length>k&&NUMF.test(f[k]))?+f[k]:null;
  out.push([f[0]].concat(v,[opt(6),opt(7)]));});return out;}
var loadAll=async function(){
  try{
    var meta=await (await fetch('/api/history/meta')).json(),cities=[];
    for(var i=0;i<meta.names.length;i++){
      var rows=parseCsv(await (await fetch('/api/history/raw?city='+i)).text());
      if(meta.today[i])rows.push(meta.today[i]);
      cities.push({name:meta.names[i],days:parseDays(rows)});}
    S.cities=cities;S.meta=meta;
    if(!S.loaded){S.sel=meta.currentCity;S.loaded=true;}
    buildStatic();render();
  }catch(e){$('storage').innerText='Could not load history: '+e;}
}
var monthly=function(days){
  var m={},order=[];
  days.forEach(p=>{if(p.partial)return;var k=p.date.slice(0,7);
    if(!m[k]){m[k]={n:0,hi:0,lo:0,rain:0,hum:[],pres:[],gust:[],sun:[]};order.push(k);}
    var o=m[k];o.n++;o.hi+=p.tMax;o.lo+=p.tMin;o.rain+=p.rain;o.hum.push(nz(p.humid));o.pres.push(nz(p.pressure));o.gust.push(p.gust);o.sun.push(p.sun);});
  return order.map(k=>{var o=m[k];return{date:k,n:o.n,tMax:o.hi/o.n,tMin:o.lo/o.n,rain:o.rain,humid:avgOf(o.hum)||0,pressure:avgOf(o.pres)||0,gust:maxOf(o.gust),sun:sumOf(o.sun)};});
}
var slice=function(days){return S.range===0?days:days.slice(-S.range);}
var viewDays=function(days){var d=slice(days);return S.group=='month'?monthly(d):d;}

var buildStatic=function(){
  var m=S.meta,cr=$('cityRow');cr.innerHTML='';
  S.cities.forEach((c,i)=>{var b=document.createElement('button');b.innerText=c.name+(i==m.currentCity?' \u2022':'');b.onclick=()=>{S.sel=i;render();};cr.appendChild(b);});
  var ex=$('expRow');ex.innerHTML='';
  S.cities.forEach((c,i)=>{var b=document.createElement('button');b.innerText=c.name;b.onclick=()=>{location.href='/api/history/export?city='+i;};ex.appendChild(b);});
  var lb=document.createElement('button');lb.innerText='Local sensor log';lb.onclick=()=>{location.href='/api/local/export';};ex.appendChild(lb);
  [['impCity',[]],['wipeCity',[['all','All cities'],['local','Local sensor log']]],['bfCity',[['all','All cities']]]].forEach(a=>{
    var s=$(a[0]),keep=s.value;s.innerHTML='';
    S.cities.forEach((c,i)=>{var o=document.createElement('option');o.value=i;o.text=c.name;s.appendChild(o);});
    a[1].forEach(x=>{var o=document.createElement('option');o.value=x[0];o.text=x[1];s.appendChild(o);});
    if(keep)s.value=keep;else if(a[0]=='bfCity')s.value='all';});
  var ab=$('archBtn');ab.innerText='Delete archived logs ('+m.archives+')';ab.disabled=m.archives==0;
  var s=$('storage'),f=$('storageFill');
  if(!m.sdOK){s.innerText='SD card not found - history is not being logged.';f.style.width='0';return;}
  var pct=m.cardTotalKB>0?100*m.cardUsedKB/m.cardTotalKB:0;
  s.innerHTML='SD card <b>'+pct.toFixed(pct<1?2:1)+'%</b> full ('+fmtKB(m.cardUsedKB)+' of '+fmtKB(m.cardTotalKB)+') &middot; history files '+fmtKB(m.histKB);
  f.style.width=Math.max(pct,0.5)+'%';f.style.background=pct>90?'#cc3333':(pct>75?'#e6961e':'#4a90d9');
}

var setActive=function(sel,fn){document.querySelectorAll(sel).forEach((b,i)=>b.classList.toggle('active',!!fn(b,i)));}
var saveView=function(){try{localStorage.setItem('histView',JSON.stringify({mode:S.mode,metric:S.metric,range:S.range,same:S.same,group:S.group,lrange:S.lrange}));}catch(e){}}
var render=function(){
  saveView();
  $('singleCtl').style.display=S.mode=='single'?'':'none';
  $('compareCtl').style.display=S.mode=='compare'?'':'none';
  setActive('#modeRow button',b=>b.dataset.m==S.mode);
  setActive('#cityRow button',(b,i)=>i==S.sel);
  setActive('#metricRow button',b=>b.dataset.k==S.metric);
  setActive('#rangeRow button',b=>+b.dataset.d==S.range);
  setActive('#groupRow button',b=>b.dataset.g==S.group);
  var any=S.cities.some(c=>c.days.length>0);
  $('noData').style.display=any?'none':'block';$('content').style.display=any?'block':'none';
  if(!any){$('noData').innerText=S.meta.sdOK?'No history yet - it is fetched automatically, check back in a minute.':'';view=null;return;}
  if(S.mode=='single')drawSingle();else drawCompare();
  drawSummary();drawRecords();
}

// ---------- chart helpers: rounded axis steps (1/2/2.5/5 x 10^n)
var niceStep=function(span,n){var raw=span/n,p=Math.pow(10,Math.floor(Math.log10(raw))),m=raw/p;return (m<=1?1:m<=2?2:m<=2.5?2.5:m<=5?5:10)*p;}
var nice=function(lo,hi,n,zero){if(zero)lo=0;if(hi-lo<1)hi=lo+1;var st=niceStep(hi-lo,n);var a=zero?0:Math.floor(lo/st)*st,b=Math.ceil(hi/st)*st;if(b<=a)b=a+st;return{lo:a,hi:b,st:st};}
var humScale=function(r){if(!r)return{lo:0,hi:100,st:20};var s=nice(r[0],r[1],4,false);if(s.lo<0)s.lo=0;if(s.hi>100)s.hi=100;return s;}
var presScale=function(r){return r?nice(r[0],r[1],4,false):{lo:1000,hi:1030,st:10};}
var decs=function(st){var s=String(+st.toFixed(4)),i=s.indexOf('.');return i<0?0:s.length-i-1;}
var frame=function(C,sc){
  var ctx=C.ctx,cv=C.cv,w=cv.width,h=cv.height;ctx.clearRect(0,0,w,h);
  var Y=v=>PAD.t+(1-(v-sc.lo)/(sc.hi-sc.lo))*(h-PAD.t-PAD.b),dc=decs(sc.st),steps=Math.round((sc.hi-sc.lo)/sc.st);
  ctx.font='10px sans-serif';ctx.textAlign='right';ctx.lineWidth=1;
  for(var k=0;k<=steps;k++){var v=sc.lo+k*sc.st,y=Y(v);
    ctx.strokeStyle=(Math.abs(v)<1e-9&&sc.lo<0)?cssVar('--faint'):cssVar('--line');
    ctx.beginPath();ctx.moveTo(PAD.l,y);ctx.lineTo(w-PAD.r,y);ctx.stroke();
    var s=v.toFixed(dc);if(parseFloat(s)===0)s=(0).toFixed(dc);ctx.fillStyle=cssVar('--muted');ctx.fillText(s,PAD.l-6,y+3);}
  return Y;
}
var xs=function(cv,n){var bw=(cv.width-PAD.l-PAD.r)/n;return{bw:bw,X:i=>PAD.l+(i+0.5)*bw};}
var dateLabels=function(C,dates,X){var ctx=C.ctx,n=dates.length,every=Math.max(1,Math.ceil(n/10));ctx.fillStyle=cssVar('--muted');ctx.textAlign='center';ctx.font='10px sans-serif';
  dates.forEach((d,i)=>{if(i%every==0)ctx.fillText(lbl(d,n),X(i),C.cv.height-8);});}
var polyline=function(ctx,pts,color,w){ctx.strokeStyle=color;ctx.lineWidth=w||1.8;ctx.beginPath();var pen=false;
  pts.forEach(p=>{if(p==null){pen=false;return;}if(!pen){ctx.moveTo(p.x,p.y);pen=true;}else ctx.lineTo(p.x,p.y);});ctx.stroke();}
var dot=function(ctx,x,y,c){ctx.fillStyle=c;ctx.beginPath();ctx.arc(x,y,3,0,7);ctx.fill();}
var emptyChart=function(C,t){var ctx=C.ctx,cv=C.cv;ctx.clearRect(0,0,cv.width,cv.height);ctx.fillStyle=cssVar('--muted');ctx.font='13px sans-serif';ctx.textAlign='center';ctx.fillText(t,cv.width/2,cv.height/2);}
var emptyAll=function(t){['temp','rain','sun','hum','pres','wind'].forEach((k,i)=>emptyChart(CH[k],i?'':t));}
// lines: series=[{c,v:[value|null],w}]
var drawSeries=function(C,dates,series,sc,o){
  o=o||{};var ctx=C.ctx,Y=frame(C,sc),g=xs(C.cv,dates.length);
  if(o.ref!=null&&o.ref>sc.lo&&o.ref<sc.hi){var ry=Y(o.ref);ctx.save();ctx.setLineDash([4,4]);ctx.strokeStyle=cssVar('--faint');ctx.beginPath();ctx.moveTo(PAD.l,ry);ctx.lineTo(C.cv.width-PAD.r,ry);ctx.stroke();ctx.restore();
    ctx.fillStyle=cssVar('--muted');ctx.textAlign='left';ctx.font='10px sans-serif';ctx.fillText(o.refLabel,PAD.l+4,ry-3);}
  if(o.fill){var seg=[],base=Y(sc.lo);ctx.fillStyle=o.fill;
    var flush=()=>{if(seg.length){ctx.beginPath();ctx.moveTo(seg[0].x,base);seg.forEach(p=>ctx.lineTo(p.x,p.y));ctx.lineTo(seg[seg.length-1].x,base);ctx.closePath();ctx.fill();}seg=[];};
    series[0].v.forEach((v,i)=>{if(v==null)flush();else seg.push({x:g.X(i),y:Y(v)});});flush();}
  series.forEach(s=>polyline(ctx,s.v.map((v,i)=>v==null?null:{x:g.X(i),y:Y(v)}),s.c,s.w||1.8));
  if(o.partialLast){var v0=series[0].v,L=v0.length-1;if(v0[L]!=null)dot(ctx,g.X(L),Y(v0[L]),cssVar('--faint'));}
  dateLabels(C,dates,g.X);
}
// bars, side by side when there are several series: series=[{c,lc,v,light:[bool]}]
var drawBars=function(C,dates,series,sc){
  var ctx=C.ctx,Y=frame(C,sc),g=xs(C.cv,dates.length),k=series.length,gap=g.bw>4?1:0,w=Math.max((g.bw-2*gap)/k,1),b=Y(0);
  series.forEach((s,si)=>s.v.forEach((v,i)=>{if(v==null||v<=0)return;var y=Y(v);ctx.fillStyle=(s.light&&s.light[i])?s.lc:s.c;ctx.fillRect(g.X(i)-g.bw/2+gap+si*w,y,w,b-y);}));
  dateLabels(C,dates,g.X);
}
var cumul=function(maps,dates,f){return maps.map(m=>{var t=0;return dates.map(d=>{var p=m[d];if(!p||p.partial)return null;var v=f(p);if(v==null)return null;t+=v;return t;});});}

// ---------- single city
var singleInfo=function(p,M){
  var h='<b>'+(M?monthTitle(p.date)+' ('+p.n+' days)':p.date)+'</b>'+(p.partial?' (today, forecast)':'');
  h+='<br>'+(M?'Avg high ':'High ')+(+p.tMax).toFixed(1)+'&deg;C &nbsp;'+(M?'Avg low ':'Low ')+(+p.tMin).toFixed(1)+'&deg;C';
  h+='<br>Rain '+(+p.rain).toFixed(1)+' mm';if(p.sun!=null)h+=' &nbsp;Sun '+(+p.sun).toFixed(1)+' h';
  if(p.gust!=null)h+='<br>'+(M?'Strongest gust ':'Max gust ')+Math.round(p.gust)+' km/h';
  h+='<br>Humid '+(p.humid>0?Math.round(p.humid)+'%':'-')+' &nbsp;'+(p.pressure>0?Math.round(p.pressure)+' hPa':'-');
  return h;
}
var drawSingle=function(){
  var c=S.cities[S.sel],M=S.group=='month',d=viewDays(c.days),per=M?'month':'day',nm=' &ndash; '+c.name;
  var pool=S.same?[].concat(...S.cities.map(x=>viewDays(x.days))):d;
  setT('temp',(M?'Monthly average temperature':'Temperature')+' (&deg;C)'+nm,leg('#cc3333',M?'Avg high':'Daily high')+leg('#1e63d6',M?'Avg low':'Daily low')+(S.same?SAME_NOTE:''));
  setT('rain','Rainfall (mm/'+per+')'+nm);
  setT('sun','Sunshine (hours/'+per+')'+nm);
  setT('hum','Humidity (% '+(M?'monthly':'daily')+' average)'+nm);
  setT('pres','Air pressure (hPa '+(M?'monthly':'daily')+' average, sea level)'+nm,PRES_HINT);
  setT('wind','Strongest gust per '+per+' (km/h)'+nm);
  if(!d.length){emptyAll('No data for '+c.name+' yet');view=null;return;}
  var dates=d.map(p=>p.date),L=d.length-1,part=!!d[L].partial,light=d.map(p=>!!p.partial);

  var T=CH.temp,tx=T.ctx,ts=nice(Math.min(...pool.map(x=>x.tMin)),Math.max(...pool.map(x=>x.tMax)),5,false);
  var Y=frame(T,ts),g=xs(T.cv,d.length);
  tx.fillStyle='rgba(230,120,60,0.15)';tx.beginPath();
  d.forEach((p,i)=>i?tx.lineTo(g.X(i),Y(p.tMax)):tx.moveTo(g.X(i),Y(p.tMax)));
  for(var i=L;i>=0;i--)tx.lineTo(g.X(i),Y(d[i].tMin));
  tx.closePath();tx.fill();
  polyline(tx,d.map((p,i)=>({x:g.X(i),y:Y(p.tMax)})),'#cc3333');
  polyline(tx,d.map((p,i)=>({x:g.X(i),y:Y(p.tMin)})),'#1e63d6');
  if(part){dot(tx,g.X(L),Y(d[L].tMax),cssVar('--faint'));dot(tx,g.X(L),Y(d[L].tMin),cssVar('--faint'));}
  dateLabels(T,dates,g.X);

  drawBars(CH.rain,dates,[{c:'#4a90d9',lc:'#a9c8ec',v:d.map(p=>p.rain),light:light}],nice(0,Math.max(2,...pool.map(x=>x.rain)),4,true));
  var sv=d.map(p=>p.sun);
  if(!clean(sv).length)emptyChart(CH.sun,NO_EXTRA);
  else drawBars(CH.sun,dates,[{c:'#f4b400',lc:'#f9dc85',v:sv,light:light}],nice(0,Math.max(M?20:2,maxOf(pool.map(p=>p.sun))||0),4,true));
  drawSeries(CH.hum,dates,[{c:'#2a9d8f',v:d.map(p=>nz(p.humid))}],humScale(rangeOf(pool.map(p=>nz(p.humid)))),{fill:'rgba(42,157,143,0.13)',partialLast:part});
  drawSeries(CH.pres,dates,[{c:'#6c5ce7',v:d.map(p=>nz(p.pressure))}],presScale(rangeOf(pool.map(p=>nz(p.pressure)))),{ref:1013,refLabel:'1013 hPa standard',partialLast:part});
  var wv=d.map(p=>p.gust);
  if(!clean(wv).length)emptyChart(CH.wind,NO_EXTRA);
  else drawSeries(CH.wind,dates,[{c:'#5d6d7e',v:wv}],nice(0,Math.max(30,maxOf(pool.map(p=>p.gust))||0),4,true),{fill:'rgba(93,109,126,0.10)',ref:62,refLabel:'62 km/h = gale (Bft 8)',partialLast:part});

  view={n:d.length,info:i=>singleInfo(d[i],M)};
}

// ---------- compare cities
var drawCompare=function(){
  var M=S.group=='month',per=M?'month':'day';
  var maps=S.cities.map(c=>{var m={};viewDays(c.days).forEach(p=>m[p.date]=p);return m;});
  var set={};maps.forEach(m=>Object.keys(m).forEach(k=>set[k]=1));
  var dates=Object.keys(set).sort();if(!M&&S.range>0)dates=dates.slice(-S.range);
  var val=p=>S.metric=='mean'?(p.tMin+p.tMax)/2:p[S.metric];
  var real=(ci,d)=>{var p=maps[ci][d];return p&&!p.partial?p:null;};
  var col=f=>maps.map((m,ci)=>({c:COLORS[ci],w:2,v:dates.map(d=>m[d]?f(m[d]):null)}));
  var names=S.cities.map((c,i)=>leg(COLORS[i],c.name)).join('');
  var mN={tMax:'highs',tMin:'lows',mean:'average (high+low)/2'}[S.metric];
  setT('temp',(M?'Monthly average ':'Daily ')+mN+' (&deg;C)',names);
  setT('hum','Humidity (% '+(M?'monthly':'daily')+' average)');
  setT('pres','Air pressure (hPa '+(M?'monthly':'daily')+' average, sea level)');
  setT('wind','Strongest gust per '+per+' (km/h)');
  var vals=[];maps.forEach(m=>dates.forEach(d=>{if(m[d])vals.push(val(m[d]));}));
  if(!vals.length){setT('rain','');setT('sun','');emptyAll('No data yet');view=null;return;}
  drawSeries(CH.temp,dates,col(val),nice(Math.min(...vals),Math.max(...vals),5,false));

  // rain + sunshine: running totals in the daily view, side-by-side bars per month in the monthly view
  var totR=S.cities.map((c,ci)=>sumOf(dates.map(d=>{var p=real(ci,d);return p?p.rain:null;}))||0);
  var totS=S.cities.map((c,ci)=>sumOf(dates.map(d=>{var p=real(ci,d);return p?p.sun:null;})));
  var cumR=null,cumS=null;
  if(M){
    var rS=maps.map((m,ci)=>({c:COLORS[ci],v:dates.map(d=>m[d]?m[d].rain:null)}));
    setT('rain','Rainfall per month (mm)',S.cities.map((c,i)=>leg(COLORS[i],c.name+': <b>'+totR[i].toFixed(0)+' mm</b> total')).join(''));
    drawBars(CH.rain,dates,rS,nice(0,Math.max(2,maxOf([].concat(...rS.map(s=>s.v)))||0),4,true));
  }else{
    cumR=cumul(maps,dates,p=>p.rain);
    setT('rain','Cumulative rainfall over the period (mm)',S.cities.map((c,i)=>leg(COLORS[i],c.name+': <b>'+totR[i].toFixed(1)+' mm</b>')).join(''));
    drawSeries(CH.rain,dates,cumR.map((a,ci)=>({c:COLORS[ci],w:2,v:a})),nice(0,Math.max(2,...totR),4,true));
  }
  var sunLeg=S.cities.map((c,i)=>leg(COLORS[i],c.name+': <b>'+(totS[i]==null?'-':totS[i].toFixed(0)+' h')+'</b>')).join('');
  if(totS.every(v=>v==null)){setT('sun',M?'Sunshine per month (hours)':'Cumulative sunshine over the period (hours)');emptyChart(CH.sun,NO_EXTRA);}
  else if(M){
    var sS=maps.map((m,ci)=>({c:COLORS[ci],v:dates.map(d=>m[d]?m[d].sun:null)}));
    setT('sun','Sunshine per month (hours)',sunLeg);
    drawBars(CH.sun,dates,sS,nice(0,Math.max(20,maxOf([].concat(...sS.map(s=>s.v)))||0),4,true));
  }else{
    cumS=cumul(maps,dates,p=>p.sun);
    setT('sun','Cumulative sunshine over the period (hours)',sunLeg);
    drawSeries(CH.sun,dates,cumS.map((a,ci)=>({c:COLORS[ci],w:2,v:a})),nice(0,Math.max(2,...totS.map(v=>v||0)),4,true));
  }

  var hS=col(p=>nz(p.humid)),pS=col(p=>nz(p.pressure)),wS=col(p=>p.gust);
  drawSeries(CH.hum,dates,hS,humScale(rangeOf([].concat(...hS.map(s=>s.v)))));
  drawSeries(CH.pres,dates,pS,presScale(rangeOf([].concat(...pS.map(s=>s.v)))),{ref:1013,refLabel:'1013 hPa standard'});
  var cityAvg=(ci,f)=>avgOf(dates.map(d=>{var p=real(ci,d);return p?f(p):null;}));
  $('l_hum').innerHTML=S.cities.map((c,i)=>{var a=cityAvg(i,p=>nz(p.humid));return leg(COLORS[i],c.name+': avg <b>'+(a==null?'-':a.toFixed(0)+'%')+'</b>');}).join('');
  $('l_pres').innerHTML=S.cities.map((c,i)=>{var a=cityAvg(i,p=>nz(p.pressure));return leg(COLORS[i],c.name+': avg <b>'+(a==null?'-':a.toFixed(0)+' hPa')+'</b>');}).join('')+'<br>'+PRES_HINT;
  if(!clean([].concat(...wS.map(s=>s.v))).length)emptyChart(CH.wind,NO_EXTRA);
  else drawSeries(CH.wind,dates,wS,nice(0,Math.max(30,maxOf([].concat(...wS.map(s=>s.v)))),4,true),{ref:62,refLabel:'62 km/h = gale (Bft 8)'});
  $('l_wind').innerHTML=S.cities.map((c,i)=>{var mx=maxOf(dates.map(d=>{var p=real(i,d);return p?p.gust:null;}));return leg(COLORS[i],c.name+': max <b>'+(mx==null?'-':Math.round(mx)+' km/h')+'</b>');}).join('');

  view={n:dates.length,info:(i,k)=>{var d=dates[i],h='<b>'+(M?monthTitle(d):d)+'</b>';
    S.cities.forEach((c,ci)=>{var p=maps[ci][d],v='-';
      if(p){
        if(k=='rain')v=(+p.rain).toFixed(1)+' mm'+(cumR&&cumR[ci][i]!=null?' (total '+cumR[ci][i].toFixed(1)+')':'');
        else if(k=='sun')v=p.sun==null?'-':(+p.sun).toFixed(1)+' h'+(cumS&&cumS[ci][i]!=null?' (total '+cumS[ci][i].toFixed(0)+')':'');
        else if(k=='hum')v=p.humid>0?Math.round(p.humid)+'%':'-';
        else if(k=='pres')v=p.pressure>0?Math.round(p.pressure)+' hPa':'-';
        else if(k=='wind')v=p.gust==null?'-':Math.round(p.gust)+' km/h';
        else v=val(p).toFixed(1)+'&deg;C';
        if(p.partial)v+=' (forecast)';}
      h+='<br><span style="color:'+COLORS[ci]+';">&#9632;</span> '+c.name+': '+v;});return h;}};
}

// ---------- tables
var drawSummary=function(){
  var longR=S.range===0||S.range>=365;
  var rows=S.cities.map((c,i)=>{
    var d=slice(c.days).filter(p=>!p.partial),cls=(S.mode=='single'&&i==S.sel)?' class="sel"':'';
    var nameCell='<td style="text-align:left;"><i class="sw" style="background:'+COLORS[i]+';"></i> '+c.name+'</td>';
    if(!d.length)return '<tr'+cls+'>'+nameCell+'<td colspan="11">no data yet</td></tr>';
    var avg=k=>d.reduce((a,p)=>a+p[k],0)/d.length;
    var hi=d.reduce((a,p)=>p.tMax>a.tMax?p:a),lo=d.reduce((a,p)=>p.tMin<a.tMin?p:a);
    var tot=d.reduce((a,p)=>a+p.rain,0),rd=d.filter(p=>p.rain>=1).length;
    var ah=avgOf(d.map(p=>nz(p.humid))),ap=avgOf(d.map(p=>nz(p.pressure))),sun=sumOf(d.map(p=>p.sun)),gu=maxOf(d.map(p=>p.gust));
    var dt=x=>'<small style="color:var(--muted);">'+(longR?x.date:x.date.slice(5))+'</small>';
    return '<tr'+cls+'>'+nameCell+'<td>'+d.length+'</td><td>'+avg('tMax').toFixed(1)+'&deg;</td><td>'+avg('tMin').toFixed(1)+'&deg;</td>'
      +'<td>'+hi.tMax+'&deg; '+dt(hi)+'</td><td>'+lo.tMin+'&deg; '+dt(lo)+'</td><td>'+tot.toFixed(1)+' mm</td><td>'+rd+'</td>'
      +'<td>'+(sun==null?'-':sun.toFixed(0)+' h')+'</td><td>'+(gu==null?'-':Math.round(gu)+' km/h')+'</td>'
      +'<td>'+(ah==null?'-':ah.toFixed(0)+'%')+'</td><td>'+(ap==null?'-':ap.toFixed(0)+' hPa')+'</td></tr>';});
  $('sumTable').innerHTML='<tr><th>City</th><th>Days</th><th>Avg high</th><th>Avg low</th><th>Warmest</th><th>Coldest</th><th>Total rain</th><th>Rain days &ge;1mm</th><th>Sunshine</th><th>Max gust</th><th>Avg humidity</th><th>Avg pressure</th></tr>'+rows.join('');
}
var records=function(days){
  var d=days.filter(p=>!p.partial);if(!d.length)return null;
  var best=(f,better)=>{var b=null;d.forEach(p=>{var v=f(p);if(v==null)return;if(!b||better(v,b.v))b={v:v,p:p};});return b;};
  var gt=(a,b)=>a>b,lt=(a,b)=>a<b;
  var R={first:d[0].date,n:d.length,hot:best(p=>p.tMax,gt),coldN:best(p=>p.tMin,lt),warmN:best(p=>p.tMin,gt),coldD:best(p=>p.tMax,lt),
    wet:best(p=>p.rain,gt),gust:best(p=>p.gust,gt),sun:best(p=>p.sun,gt),pHi:best(p=>nz(p.pressure),gt),pLo:best(p=>nz(p.pressure),lt),dry:null,wetS:null};
  var cur=null,prev=null;
  var close=()=>{if(!cur)return;var k=cur.wet?'wetS':'dry';if(!R[k]||cur.n>R[k].n)R[k]={n:cur.n,from:cur.from,to:cur.to};cur=null;};
  d.forEach(p=>{var w=p.rain>=1,next=prev&&dayNum(p.date)-dayNum(prev.date)==1;
    if(!cur||!next||cur.wet!=w){close();cur={wet:w,n:0,from:p.date};}cur.n++;cur.to=p.date;prev=p;});
  close();return R;
}
var drawRecords=function(){
  var rs=S.cities.map(c=>records(c.days));
  var deg=v=>(+v).toFixed(1)+'&deg;C',hpa=v=>Math.round(v)+' hPa';
  var defs=[['Hottest day','hot',deg],['Warmest night','warmN',deg],['Coldest day','coldD',deg],['Coldest night','coldN',deg],
    ['Wettest day','wet',v=>(+v).toFixed(1)+' mm'],['Sunniest day','sun',v=>(+v).toFixed(1)+' h'],['Strongest gust','gust',v=>Math.round(v)+' km/h'],
    ['Highest pressure','pHi',hpa],['Lowest pressure','pLo',hpa]];
  var sm=t=>' <small style="color:var(--muted);">'+t+'</small>';
  var h='<tr><th></th>'+S.cities.map((c,i)=>'<th><i class="sw" style="background:'+COLORS[i]+';"></i> '+c.name+'</th>').join('')+'</tr>';
  h+='<tr><td style="text-align:left;">Logged since</td>'+rs.map(R=>'<td>'+(R?R.first+sm(R.n+' days'):'-')+'</td>').join('')+'</tr>';
  defs.forEach(r=>{h+='<tr><td style="text-align:left;">'+r[0]+'</td>'+rs.map(R=>{var b=R&&R[r[1]];return '<td>'+(b?r[2](b.v)+sm(b.p.date):'-')+'</td>';}).join('')+'</tr>';});
  [['Longest dry spell (&lt;1 mm)','dry'],['Longest wet spell (&ge;1 mm)','wetS']].forEach(r=>{
    h+='<tr><td style="text-align:left;">'+r[0]+'</td>'+rs.map(R=>{var b=R&&R[r[1]];return '<td>'+(b?b.n+' days'+sm(b.from.slice(5)+' &ndash; '+b.to.slice(5)+' '+b.to.slice(0,4)):'-')+'</td>';}).join('')+'</tr>';});
  $('recTable').innerHTML=h;
}

// ---------- tooltips
var placeTip=function(e,html){tip.innerHTML=html;tip.style.display='block';var x=e.clientX+14;if(x+260>window.innerWidth)x=e.clientX-270;tip.style.left=x+'px';tip.style.top=(e.clientY+14)+'px';}
var hover=function(C,key){
  C.cv.addEventListener('mousemove',e=>{
    if(!view){tip.style.display='none';return;}
    var r=C.cv.getBoundingClientRect(),mx=(e.clientX-r.left)*(C.cv.width/r.width);
    var bw=(C.cv.width-PAD.l-PAD.r)/view.n,i=Math.floor((mx-PAD.l)/bw);
    if(i<0||i>=view.n){tip.style.display='none';return;}
    placeTip(e,view.info(i,key));});
  C.cv.addEventListener('mouseleave',()=>tip.style.display='none');
}
;['temp','rain','sun','hum','pres','wind'].forEach(k=>hover(CH[k],k));

// ---------- local sensor
var loadLocal=async function(){try{S.local=await (await fetch('/api/local?hours='+S.lrange)).json();drawLocal();}catch(e){}}
var tX=function(cv,e,e0,e1){return PAD.l+(e-e0)/(e1-e0)*(cv.width-PAD.l-PAD.r);}
var timeLabels=function(C,e0,e1){
  var ctx=C.ctx,span=e1-e0,n=6;ctx.fillStyle=cssVar('--muted');ctx.font='10px sans-serif';
  for(var k=0;k<=n;k++){var e=e0+span*k/n,d=new Date(e*1000),s;
    if(span<=2*86400)s=String(d.getHours()).padStart(2,'0')+':'+String(d.getMinutes()).padStart(2,'0');
    else if(span<=60*86400)s=d.getDate()+' '+MONTHS[d.getMonth()]+(span<=8*86400?' '+String(d.getHours()).padStart(2,'0')+'h':'');
    else s=MONTHS[d.getMonth()]+" '"+String(d.getFullYear()).slice(2);
    ctx.textAlign=k==0?'left':(k==n?'right':'center');ctx.fillText(s,tX(C.cv,e,e0,e1),C.cv.height-8);}
}
var drawLocal=function(){
  var j=S.local;if(!j)return;
  var show=j.bmeOK||j.rows.length>0;$('localSec').style.display=show?'':'none';if(!show)return;
  setActive('#lRangeRow button',b=>+b.dataset.h==S.lrange);
  var r=j.rows,defs=[['lsT',1,'#cc3333',5,'&deg;C',1],['lsH',2,'#2a9d8f',4,'%',0],['lsP',3,'#6c5ce7',4,' hPa',0]];
  if(!r.length){defs.forEach(a=>emptyChart(CH[a[0]],'No hourly readings yet - the first one is saved at the top of the next hour'));$('lStats').innerHTML='';lview=null;return;}
  var e0=r[0][0],e1=r[r.length-1][0];if(e1<=e0)e1=e0+3600;
  defs.forEach(a=>{var C=CH[a[0]],vals=r.map(x=>x[a[1]]),sc=nice(Math.min(...vals),Math.max(...vals),a[3],false);
    if(a[0]=='lsH'){if(sc.lo<0)sc.lo=0;if(sc.hi>100)sc.hi=100;}
    var Y=frame(C,sc),pts=[],prev=null;
    r.forEach(x=>{if(prev!=null&&x[0]-prev>2*3600)pts.push(null);pts.push({x:tX(C.cv,x[0],e0,e1),y:Y(x[a[1]])});prev=x[0];});
    polyline(C.ctx,pts,a[2],1.6);timeLabels(C,e0,e1);});
  $('lStats').innerHTML=defs.map(a=>{var v=r.map(x=>x[a[1]]),f=x=>x.toFixed(a[5])+a[4];
    return '<span>'+{lsT:'Temp',lsH:'Humidity',lsP:'Pressure'}[a[0]]+': min <b>'+f(Math.min(...v))+'</b> &middot; avg <b>'+f(avgOf(v))+'</b> &middot; max <b>'+f(Math.max(...v))+'</b></span>';}).join('');
  lview={r:r,e0:e0,e1:e1};
}
;['lsT','lsH','lsP'].forEach(k=>{var C=CH[k];
  C.cv.addEventListener('mousemove',e=>{if(!lview){tip.style.display='none';return;}
    var rect=C.cv.getBoundingClientRect(),mx=(e.clientX-rect.left)*(C.cv.width/rect.width);
    var et=lview.e0+(mx-PAD.l)/(C.cv.width-PAD.l-PAD.r)*(lview.e1-lview.e0),b=null;
    lview.r.forEach(x=>{if(!b||Math.abs(x[0]-et)<Math.abs(b[0]-et))b=x;});
    var d=new Date(b[0]*1000);
    placeTip(e,'<b>'+d.toLocaleDateString()+' '+String(d.getHours()).padStart(2,'0')+':00</b><br>'+b[1].toFixed(1)+'&deg;C &middot; '+Math.round(b[2])+'% &middot; '+Math.round(b[3])+' hPa');});
  C.cv.addEventListener('mouseleave',()=>tip.style.display='none');});

// ---------- controls
document.querySelectorAll('#modeRow button').forEach(b=>b.onclick=()=>{S.mode=b.dataset.m;render();});
document.querySelectorAll('#metricRow button').forEach(b=>b.onclick=()=>{S.metric=b.dataset.k;render();});
document.querySelectorAll('#rangeRow button').forEach(b=>b.onclick=()=>{S.range=+b.dataset.d;render();});
document.querySelectorAll('#groupRow button').forEach(b=>b.onclick=()=>{S.group=b.dataset.g;render();});
document.querySelectorAll('#lRangeRow button').forEach(b=>b.onclick=()=>{S.lrange=+b.dataset.h;saveView();loadLocal();});
$('sameScale').onchange=e=>{S.same=e.target.checked;render();};

// ---------- manage data
var msg=function(t,bad){var m=$('msg');m.innerText=t;m.style.color=bad?'var(--bad)':'var(--good)';}
var post=async function(u,body){try{var r=await fetch(u,{method:'POST',body:body,headers:{'X-Requested-With':'DesktopDisplay'}});return await r.json();}catch(e){return{ok:false,msg:'Request failed: '+e};}}
var doImport=async function(){
  var f=$('impFile').files[0];if(!f){msg('Choose a CSV file first.',true);return;}
  var city=$('impCity').value,mode=document.querySelector('input[name=impMode]:checked').value,name=S.cities[city].name;
  if(mode=='replace'&&!confirm('Replace ALL history for '+name+' with this file?'))return;
  var fd=new FormData();fd.append('file',f,f.name);
  msg('Importing...');
  var j=await post('/api/history/import?city='+city+'&mode='+mode,fd);
  msg(j.msg,!j.ok);if(j.ok){$('impFile').value='';loadAll();}
}
var doWipe=async function(){
  var v=$('wipeCity').value,name=v=='all'?'ALL cities':(v=='local'?'the local sensor log':S.cities[v].name);
  if(!confirm('Wipe '+name+'?\nExport it first if you want to keep a copy.'))return;
  var j=await post('/api/history/wipe?target='+v);msg(j.msg,!j.ok);loadAll();loadLocal();
}
var doWipeArchives=async function(){
  if(!confirm('Delete all archived logs (old files from renamed cities)?'))return;
  var j=await post('/api/history/wipe?target=archives');msg(j.msg,!j.ok);loadAll();
}
var doBackfill=async function(){
  var t=$('bfCity').value,y=$('bfYears').value;
  var j=await post('/api/history/backfill?target='+t+'&years='+y);msg(j.msg,!j.ok);pollBF();
}
var stopBackfill=async function(){var j=await post('/api/history/backfill?stop=1&target='+$('bfCity').value);msg(j.msg,!j.ok);pollBF();}
var bfTimer=null,bfWas=false,bfTick=0;
var showBF=function(list){
  var names=S.cities.length?S.cities.map(c=>c.name):['City 1','City 2','City 3'];
  $('bfStatus').innerHTML=list.map((b,i)=>{
    if(!b.active&&!b.msg)return '';
    var line='<b>'+names[i]+'</b>: ';
    if(b.active&&b.start&&b.end&&b.cursor){var tot=dayNum(b.end)-dayNum(b.start),done=dayNum(b.end)-dayNum(b.cursor),pct=tot>0?Math.min(100,Math.max(0,100*done/tot)):0;
      line+='<span class="bar"><i style="width:'+pct.toFixed(0)+'%"></i></span>'+pct.toFixed(0)+'% &middot; target '+b.start+' &middot; ';}
    return '<div>'+line+b.msg+'</div>';}).join('');
}
var pollBF=async function(){
  clearTimeout(bfTimer);
  try{var j=await (await fetch('/api/history/status')).json();showBF(j.backfill);var act=j.backfill.some(b=>b.active);
    if(act){bfTimer=setTimeout(pollBF,5000);} // the charts reload once when the backfill is done
    else if(bfWas)loadAll();
    bfWas=act;
  }catch(e){bfTimer=setTimeout(pollBF,10000);}
}
document.addEventListener('themechange',()=>{if(S.loaded){render();drawLocal();}});
loadAll().then(()=>{loadLocal();pollBF();});
</script>
</main>)rawliteral";

void handleHistoryPage() {
  sendPage("History", "history", false, HISTORY_PAGE_TOP, HISTORY_PAGE_BOTTOM);
}

#endif // USE_SD_HISTORY

// --- [WEB UI ASSETS] ---
// The web pages' shared stylesheet and script, and the page bodies. Kept down
// here for the same reason as the history page: quote-heavy text confuses the
// Arduino IDE's prototype generator for any C++ that comes after it.
const char APP_CSS[] PROGMEM = R"rawliteral(
:root,[data-theme=light]{--bg:#eef2f7;--card:#fff;--card2:#f5f7fb;--text:#16202f;--muted:#64748b;--faint:#94a3b8;--line:#e2e8f0;--accent:#f08c2e;--onaccent:#fff;--blue:#2f6fdf;--rain:#3b82f6;--good:#22a06b;--bad:#dc2626;--hi:#d64545;--lo:#2f6fdf;--sun:#f5b400;--shadow:0 1px 2px rgba(16,24,40,.06),0 4px 16px rgba(16,24,40,.06);--hero:linear-gradient(135deg,#dfe8f5 0%,#f7f9fc 60%);color-scheme:light}
[data-theme=dark]{--bg:#0b1220;--card:#121b2b;--card2:#18233a;--text:#e7edf7;--muted:#8fa0b8;--faint:#5d6e88;--line:#22304a;--accent:#ff9f43;--onaccent:#16202f;--blue:#5b9bff;--rain:#5aa2ff;--good:#34d399;--bad:#f87171;--hi:#f87171;--lo:#60a5fa;--sun:#fbbf24;--shadow:0 1px 2px rgba(0,0,0,.4);--hero:linear-gradient(135deg,#17253f 0%,#121b2b 70%);color-scheme:dark}
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;-webkit-font-smoothing:antialiased}
a{color:var(--blue)}
.top{position:sticky;top:0;z-index:20;display:flex;align-items:center;gap:14px;padding:10px 22px;background:color-mix(in srgb,var(--bg) 88%,transparent);backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px);border-bottom:1px solid var(--line)}
.brand{font-weight:700;display:flex;align-items:center;gap:9px;white-space:nowrap;color:var(--text);text-decoration:none}
.brand i{width:10px;height:10px;border-radius:50%;background:var(--good);box-shadow:0 0 0 3px color-mix(in srgb,var(--good) 25%,transparent)}
.brand i.bad{background:var(--bad);box-shadow:0 0 0 3px color-mix(in srgb,var(--bad) 25%,transparent)}
.tabs{display:flex;gap:3px;background:var(--card2);padding:4px;border-radius:12px;border:1px solid var(--line);min-width:0;overflow-x:auto}
.tabs button{border:0;background:none;color:var(--muted);padding:7px 14px;border-radius:9px;font:inherit;font-weight:600;cursor:pointer;white-space:nowrap}
.tabs button.on{background:var(--card);color:var(--text);box-shadow:var(--shadow)}
.top nav{margin-left:auto;display:flex;gap:2px}
.top nav a{color:var(--muted);text-decoration:none;padding:7px 12px;border-radius:9px;font-weight:600;font-size:.95em}
.top nav a:hover{color:var(--text)}
.top nav a.on{color:var(--text);background:var(--card2)}
.iconbtn{border:1px solid var(--line);background:var(--card);color:var(--text);border-radius:10px;width:38px;height:38px;cursor:pointer;display:inline-flex;align-items:center;justify-content:center;flex:none;padding:0}
.iconbtn svg{width:18px;height:18px}
.bottom{display:none}
.page{max-width:900px;margin:0 auto;padding:20px 22px 40px;display:grid;gap:16px}
.page>h1{margin:4px 0 0;font-size:1.5em}
.card{background:var(--card);border:1px solid var(--line);border-radius:18px;padding:18px 20px;box-shadow:var(--shadow);min-width:0}
.card h3,.label{margin:0 0 12px;font-size:.76em;letter-spacing:.08em;text-transform:uppercase;color:var(--muted);font-weight:700}
.card h3{display:flex;justify-content:space-between;gap:10px;align-items:baseline}
.card h3 span{text-transform:none;letter-spacing:0;font-weight:600}
.muted{color:var(--muted)} .small{font-size:.86em} .note{font-size:.85em;color:var(--muted);margin:8px 0 0}
.tile{background:var(--card2);border-radius:12px;padding:12px 14px;display:flex;flex-direction:column;gap:3px;min-width:0}
.tile .k{font-size:.78em;color:var(--muted);font-weight:600}
.tile .v{font-size:1.35em;font-weight:650}
.tile .v small{font-size:.6em;color:var(--muted);font-weight:500}
.bar{height:6px;border-radius:3px;background:var(--line);overflow:hidden;margin-top:4px}.bar i{display:block;height:100%;background:var(--blue);border-radius:3px}
/* forms */
.field{display:grid;gap:6px;margin:0 0 14px}
.field>span{font-size:.85em;font-weight:600;color:var(--muted)}
input[type=text],input[type=password],select{width:100%;font:inherit;color:var(--text);background:var(--card2);border:1px solid var(--line);border-radius:10px;padding:9px 12px;outline:none}
input[type=text]:focus,select:focus{border-color:var(--accent);box-shadow:0 0 0 3px color-mix(in srgb,var(--accent) 22%,transparent)}
.row2{display:grid;grid-template-columns:1fr 210px;gap:10px}
.switch{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:10px 0;border-top:1px solid var(--line);cursor:pointer}
.switch:first-of-type{border-top:0}
.switch input{appearance:none;-webkit-appearance:none;width:44px;height:26px;border-radius:13px;background:var(--line);position:relative;cursor:pointer;flex:none;transition:background .15s;margin:0}
.switch input::after{content:"";position:absolute;top:3px;left:3px;width:20px;height:20px;border-radius:50%;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.3);transition:left .15s}
.switch input:checked{background:var(--accent)} .switch input:checked::after{left:21px}
.switch small{display:block;color:var(--muted);font-size:.84em}
input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:10px;border-radius:5px;background:var(--line);margin:10px 0;outline:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:22px;height:22px;border-radius:50%;background:var(--card);border:3px solid var(--accent);cursor:pointer}
input[type=range]::-moz-range-thumb{width:16px;height:16px;border-radius:50%;background:var(--card);border:3px solid var(--accent);cursor:pointer}
.slider{display:grid;grid-template-columns:110px 1fr 48px;align-items:center;gap:12px}
.slider span{font-size:.88em;color:var(--muted);font-weight:600}.slider b{text-align:right;font-size:.9em}
.btn{font:inherit;font-weight:600;border-radius:10px;padding:9px 16px;border:1px solid var(--line);background:var(--card2);color:var(--text);cursor:pointer}
.btn:hover{border-color:var(--faint)}
.btn.primary{background:var(--accent);border-color:var(--accent);color:var(--onaccent)}
.btn.danger{color:var(--bad);border-color:color-mix(in srgb,var(--bad) 40%,var(--line))}
.btn:disabled{opacity:.5;cursor:default}
.seg{display:inline-flex;gap:3px;background:var(--card2);padding:4px;border-radius:11px;border:1px solid var(--line);flex-wrap:wrap}
.seg button{border:0;background:none;color:var(--muted);padding:6px 12px;border-radius:8px;font:inherit;font-weight:600;cursor:pointer}
.seg button.on,.seg button.active{background:var(--card);color:var(--text);box-shadow:var(--shadow)}
.swatch{width:44px;height:44px;border-radius:12px;border:1px solid var(--line);flex:none}
.savebar{position:sticky;bottom:12px;display:flex;align-items:center;gap:12px;justify-content:flex-end;background:var(--card);border:1px solid var(--line);border-radius:14px;padding:10px 12px;box-shadow:var(--shadow)}
.savebar span{margin-right:auto;color:var(--muted);font-size:.9em}
table{border-collapse:collapse;width:100%;font-size:.88em}
th,td{padding:7px 9px;border-bottom:1px solid var(--line);white-space:nowrap;text-align:left}
th{color:var(--muted);font-weight:600}
@media (max-width:900px){
  .top{flex-wrap:wrap;padding:9px 14px;gap:10px}
  .top nav{display:none}
  .top .iconbtn{margin-left:auto}
  .tabs{order:3;width:100%}.tabs button{flex:1}
  .page{padding:14px 14px 96px;gap:12px}
  .card{padding:16px}
  .row2{grid-template-columns:1fr}
  .slider{grid-template-columns:86px 1fr 42px}
  .savebar{bottom:76px}
  .bottom{display:flex;position:fixed;left:0;right:0;bottom:0;z-index:20;justify-content:space-around;background:var(--card);border-top:1px solid var(--line);padding:7px 0 calc(9px + env(safe-area-inset-bottom))}
  .bottom a{display:flex;flex-direction:column;align-items:center;gap:3px;color:var(--muted);text-decoration:none;font-size:.72em;font-weight:600;min-width:64px}
  .bottom a.on{color:var(--accent)}
  .bottom svg{width:22px;height:22px}
}
)rawliteral";

const char APP_JS[] PROGMEM = R"rawliteral(
// Shared by every page: theme toggle, accent color, API helper, icons, 7-segment clock.
var H={'X-Requested-With':'DesktopDisplay'};
function api(u,o){o=o||{};o.headers=Object.assign({},o.headers||{},H);return fetch(u,o);}
var $=function(id){return document.getElementById(id);};

// ---- theme: auto (follows the phone/PC setting) -> light -> dark
var THEME_ICONS={auto:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="9"/><path d="M12 3a9 9 0 0 0 0 18z" fill="currentColor"/></svg>',
 light:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M2 12h2M20 12h2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/></svg>',
 dark:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"/></svg>'};
function themePref(){try{return localStorage.getItem('wsTheme')||'auto';}catch(e){return 'auto';}}
function applyThemePref(){var p=themePref(),dark=p=='dark'||(p=='auto'&&matchMedia('(prefers-color-scheme: dark)').matches);
 document.documentElement.dataset.theme=dark?'dark':'light';var b=$('themeBtn');if(b){b.innerHTML=THEME_ICONS[p];b.title='Theme: '+p;}
 setAccentFromText(window._txt);document.dispatchEvent(new Event('themechange'));}
function cycleTheme(){var p=themePref(),n=p=='auto'?'light':(p=='light'?'dark':'auto');try{localStorage.setItem('wsTheme',n);}catch(e){}applyThemePref();}
matchMedia('(prefers-color-scheme: dark)').addEventListener('change',applyThemePref);

// ---- accent = the device's text color when it is a real, readable color (same rule as the display)
function setAccentFromText(t){window._txt=t;var st=document.documentElement.style;
 if(!t){st.removeProperty('--accent');st.removeProperty('--onaccent');return;}
 var r=t[0],g=t[1],b=t[2],mx=Math.max(r,g,b),mn=Math.min(r,g,b),sat=mx?(mx-mn)/mx:0,lum=(r*299+g*587+b*114)/1000;
 var dark=document.documentElement.dataset.theme=='dark',ok=sat>=.24&&mx>=60&&(dark?lum>=90:lum<=215);
 if(!ok){st.removeProperty('--accent');st.removeProperty('--onaccent');return;}
 st.setProperty('--accent','rgb('+r+','+g+','+b+')');st.setProperty('--onaccent',lum>170?'#16202f':'#fff');}
function cssVar(n){return getComputedStyle(document.documentElement).getPropertyValue(n).trim();}

// ---- weather icons (inline SVG, nothing loaded from the internet)
function iconKind(main,desc){main=(main||'').toUpperCase();desc=(desc||'').toUpperCase();
 if(main=='CLEAR')return 'clear';if(main=='THUNDERSTORM')return 'thunder';if(main=='RAIN')return 'rain';if(main=='DRIZZLE')return 'drizzle';if(main=='SNOW')return 'snow';
 if(/MIST|FOG|HAZE|SMOKE|DUST|SAND|ASH/.test(main)||desc.indexOf('FOG')>=0)return 'fog';
 if(/FEW|SCATTERED|PARTLY|MAINLY CLEAR/.test(desc))return 'partly';return 'cloud';}
function iconSvg(kind,night){
 var cl=function(dy,c){return '<path d="M18 '+(48+dy)+'h28a11 11 0 0 0 0-22 15 15 0 0 0-29-2A10 10 0 0 0 18 '+(48+dy)+'z" fill="'+(c||'#9fb0c8')+'"/>';};
 var sun='<circle cx="32" cy="32" r="12" fill="#fbbf24"/><g stroke="#fbbf24" stroke-width="4" stroke-linecap="round"><path d="M32 6v6M32 52v6M6 32h6M52 32h6M13.6 13.6l4.2 4.2M46.2 46.2l4.2 4.2M13.6 50.4l4.2-4.2M46.2 17.8l4.2-4.2"/></g>';
 var moon=function(x,y,r){return '<path d="M'+x+' '+(y-r)+'a'+r+' '+r+' 0 1 0 '+(r*.9)+' '+(r*1.55)+' '+(r*.85)+' '+(r*.85)+' 0 0 1 '+(-r*.9)+' '+(-r*1.55)+'z" fill="#e8d9a8"/>';};
 var s='';
 if(kind=='clear')s=night?moon(30,32,18):sun;
 else if(kind=='partly')s=(night?moon(22,22,12):'<circle cx="24" cy="24" r="10" fill="#fbbf24"/><g stroke="#fbbf24" stroke-width="3.5" stroke-linecap="round"><path d="M24 4v5M4 24h5M9.9 9.9l3.5 3.5M38.1 9.9l-3.5 3.5"/></g>')+'<path d="M22 52h26a10 10 0 0 0 0-20 14 14 0 0 0-27-2A9 9 0 0 0 22 52z" fill="#c3cfdf"/>';
 else if(kind=='rain')s=cl(-8,'#8aa0bd')+'<g stroke="#3b82f6" stroke-width="3.5" stroke-linecap="round"><path d="M22 46l-3 8M33 46l-3 8M44 46l-3 8"/></g>';
 else if(kind=='drizzle')s=cl(-8,'#8aa0bd')+'<g fill="#3b82f6"><circle cx="22" cy="48" r="2.4"/><circle cx="33" cy="48" r="2.4"/><circle cx="44" cy="48" r="2.4"/><circle cx="27" cy="56" r="2.4"/><circle cx="38" cy="56" r="2.4"/></g>';
 else if(kind=='thunder')s=cl(-8,'#8aa0bd')+'<path d="M35 40l-9 13h7l-4 11 11-15h-7l4-9z" fill="#fbbf24"/>';
 else if(kind=='snow')s=cl(-8,'#9fb0c8')+'<g fill="#60a5fa"><circle cx="21" cy="48" r="2.8"/><circle cx="33" cy="53" r="2.8"/><circle cx="45" cy="48" r="2.8"/><circle cx="27" cy="59" r="2.8"/><circle cx="39" cy="59" r="2.8"/></g>';
 else if(kind=='fog')s='<g stroke="#9fb0c8" stroke-width="5" stroke-linecap="round"><path d="M10 20h40M16 30h40M10 40h40M16 50h40"/></g>';
 else s=cl(0);
 return '<svg viewBox="0 0 64 64" aria-hidden="true">'+s+'</svg>';}

// ---- 7-segment digits, styled after the device's TFT_eSPI Font 7
var SEG=(function(){var T=11,h=T/2,G=1.3,W=70,L=h,R=W-h,TP=h,M=50,B=100-h;
function Hs(y,a,b){a+=G;b-=G;return [[a,y],[a+h,y-h],[b-h,y-h],[b,y],[b-h,y+h],[a+h,y+h]];}
function Vs(x,a,b){a+=G;b-=G;return [[x,a],[x+h,a+h],[x+h,b-h],[x,b],[x-h,b-h],[x-h,a+h]];}
return {a:Hs(TP,L,R),g:Hs(M,L,R),d:Hs(B,L,R),f:Vs(L,TP,M),b:Vs(R,TP,M),e:Vs(L,M,B),c:Vs(R,M,B)};})();
var DIG=['abcdef','bc','abdeg','abcdg','bcfg','acdfg','acdefg','abc','abcdefg','abcdfg'];
function segDigit(d,x,col){return DIG[d].split('').map(function(s){return '<polygon fill="'+col+'" points="'+SEG[s].map(function(p){return (p[0]+x)+','+p[1];}).join(' ')+'"/>';}).join('');}
function segClock(hh,mm,hCol,minCol){var hs=String(hh).padStart(2,'0'),ms=String(mm).padStart(2,'0'),o='',x=0;
 o+=segDigit(+hs[0],x,hCol);x+=82;o+=segDigit(+hs[1],x,hCol);x+=70+24;
 o+='<rect x="'+x+'" y="25" width="12" height="12" rx="2" fill="'+hCol+'"/><rect x="'+x+'" y="63" width="12" height="12" rx="2" fill="'+hCol+'"/>';x+=12+24;
 o+=segDigit(+ms[0],x,minCol);x+=82;o+=segDigit(+ms[1],x,minCol);x+=70;
 return '<svg viewBox="-2 -2 '+(x+4)+' 104" role="img" aria-label="'+hs+':'+ms+'">'+o+'</svg>';}

applyThemePref();
)rawliteral";

const char DASHBOARD_BODY[] PROGMEM = R"rawliteral(
<style>
.dash{max-width:1280px;margin:0 auto;padding:20px 22px 40px;display:grid;grid-template-columns:repeat(12,minmax(0,1fr));gap:16px}
.hero{grid-column:span 8;background:var(--hero);display:grid;grid-template-columns:1.1fr 1fr;gap:12px;align-items:center;padding:24px 28px}
.now{display:flex;align-items:center;gap:18px}
.now .ic svg{width:96px;height:96px;display:block}
.temp{font-size:72px;font-weight:300;line-height:1;letter-spacing:-2px;white-space:nowrap}
.temp sup{font-size:28px;font-weight:400;vertical-align:top;margin-left:2px;letter-spacing:0}
.desc{font-weight:600;margin-top:6px;text-transform:capitalize}
.hl{color:var(--muted);margin-top:4px;font-size:.95em}
.clockbox{text-align:center}
.clockbox svg{width:100%;max-width:300px;height:auto}
.date{color:var(--muted);margin-top:8px;font-weight:500}
.details{grid-column:span 4;display:grid;grid-template-columns:1fr 1fr;gap:10px;align-content:start}
.details h3{grid-column:1/-1}
.compass{display:flex;align-items:center;gap:10px}
.rain{grid-column:span 8}.sun{grid-column:span 4}.fc{grid-column:span 8}.side{grid-column:span 4;display:grid;gap:16px;align-content:start}
.fcrow{display:grid;grid-template-columns:64px 40px minmax(0,1fr) 44px minmax(60px,1.4fr) 44px;align-items:center;gap:10px;padding:9px 0;border-bottom:1px solid var(--line)}
.fcrow:last-child{border:0}
.fcrow b{font-weight:650}.fcrow svg{width:34px;height:34px;display:block}
.fcrow .lo{color:var(--muted);text-align:right}.fcrow .hi{font-weight:650}
.fcrow .what{color:var(--muted);font-size:.9em;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.range{height:8px;border-radius:4px;background:var(--line);position:relative}
.range i{position:absolute;top:0;bottom:0;border-radius:4px;background:linear-gradient(90deg,#60a5fa,#fbbf24,#f97316)}
.sunarc{width:100%;height:auto;display:block}
.sunrow{display:flex;justify-content:space-between;margin-top:6px;font-size:.92em}
.sunrow b{display:block;font-size:1.2em}
.moon{display:flex;align-items:center;gap:14px;margin-top:14px;padding-top:14px;border-top:1px solid var(--line)}
.indoor{display:flex;gap:18px;align-items:baseline;flex-wrap:wrap}.indoor .big{font-size:2.2em;font-weight:300}
.sw{display:inline-flex;align-items:center;gap:8px;margin-right:16px;color:var(--muted);font-size:.9em}
.sw i{width:18px;height:18px;border-radius:6px;border:1px solid var(--line)}
.status{display:flex;align-items:center;gap:8px;color:var(--muted);font-size:.9em;margin-top:10px}
.status i{width:9px;height:9px;border-radius:50%;background:var(--good);flex:none}
.empty{color:var(--muted);text-align:center;padding:30px 0}
.nowmsg{display:flex;justify-content:space-between;align-items:baseline;gap:10px;margin-bottom:4px}
.nowmsg b{font-size:1.15em}.nowmsg b.soft{color:var(--muted)}
.legend{display:flex;gap:14px;font-size:.78em;color:var(--muted);margin-top:2px}.legend i{display:inline-block;width:10px;height:10px;border-radius:3px;margin-right:5px;vertical-align:-1px}
.vf{display:grid;gap:9px}.vfrow{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:2px 10px;align-items:baseline}
.vfrow b{font-weight:650}.vfrow b.cur::after{content:" \2022  in use";color:var(--muted);font-weight:500;font-size:.85em}
.vfrow .sc{font-weight:700}.vfrow .muted{grid-column:1/-1;font-size:.84em}.vfrow .bar{grid-column:1/-1;margin:0}
.vfrec{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-top:4px;padding-top:12px;border-top:1px solid var(--line);font-size:.9em}.vfrec .btn{flex:none;white-space:nowrap}
.tag{font-size:.72em;font-weight:700;letter-spacing:.06em;color:var(--muted);border:1px solid var(--line);border-radius:6px;padding:2px 6px;white-space:nowrap}
@media (max-width:900px){
 .dash{grid-template-columns:minmax(0,1fr);padding:14px 14px 96px;gap:12px}
 .hero,.details,.rain,.sun,.fc,.side{grid-column:1/-1}
 .hero{grid-template-columns:1fr;padding:20px}
 .now .ic svg{width:72px;height:72px}.temp{font-size:60px}
 .clockbox svg{max-width:240px}
 .fcrow{grid-template-columns:48px 32px 38px minmax(0,1fr) 38px}.fcrow .what{display:none}
}
</style>
<main class="dash">
 <section class="card hero">
  <div class="now"><div class="ic" id="heroIcon"></div>
   <div><div class="temp" id="temp">--</div><div class="desc" id="desc">Loading...</div><div class="hl" id="hl"></div></div></div>
  <div class="clockbox"><div id="clock"></div><div class="date" id="date"></div></div>
 </section>
 <section class="card details"><h3>Right now</h3>
  <div class="tile"><span class="k">Wind</span><div class="compass"><svg width="30" height="30" viewBox="0 0 30 30"><circle cx="15" cy="15" r="13" fill="none" stroke="var(--line)" stroke-width="2"/><path id="windArrow" d="M15 6 L19 18 L15 15.5 L11 18 Z" fill="var(--blue)"/></svg><span class="v" id="wind">--</span></div></div>
  <div class="tile"><span class="k">Humidity</span><span class="v" id="humid">--</span><div class="bar"><i id="humidBar" style="width:0"></i></div></div>
  <div class="tile"><span class="k">Pressure</span><span class="v" id="baro">--</span><span class="k" id="pTrend">sea level</span></div>
  <div class="tile"><span class="k">Visibility</span><span class="v" id="vis">--</span><span class="k" id="clouds"></span></div>
 </section>
 <section class="card rain"><h3>Rain <span id="rainTotal"></span></h3><div id="nowcast"></div><div class="label" id="rain15Label" style="margin-top:6px">Next 15 hours</div><div id="rainChart"></div></section>
 <section class="card sun"><h3>Sun &amp; Moon</h3>
  <svg class="sunarc" viewBox="0 0 300 120" id="sunArc"></svg>
  <div class="sunrow"><div><span class="muted">Sunrise</span><b id="sunRise">--</b></div><div style="text-align:center"><span class="muted" id="sunEvent">--</span><b id="sunIn" style="color:var(--accent)">--</b></div><div style="text-align:right"><span class="muted">Sunset</span><b id="sunSet">--</b></div></div>
  <div class="moon"><svg width="44" height="44" viewBox="0 0 44 44" id="moonIcon"></svg><div><b id="moonName">--</b><div class="muted small" id="moonNext"></div></div></div>
 </section>
 <section class="card fc"><h3>5-day forecast</h3><div id="fc"></div></section>
 <div class="side">
  <section class="card" id="indoorCard" style="display:none"><h3>Indoor <span>BME280</span></h3>
   <div class="indoor"><span class="big" id="inTemp">--</span><span id="inHum"></span><span class="muted" id="inPres"></span></div></section>
  <section class="card" id="vfCard" style="display:none"><h3>Forecast check <span id="vfSince"></span></h3><div id="vf"></div></section>
  <section class="card"><h3>Device</h3>
   <div><span class="sw"><i id="ledSw"></i>LED</span><span class="sw"><i id="txtSw"></i>Text &amp; accent</span></div>
   <div class="status"><i id="stDot"></i><span id="stText">--</span></div></section>
 </div>
</main>
<script>
var cur=null,DIRS={N:0,NE:45,E:90,SE:135,S:180,SW:225,W:270,NW:315};
var WORDS={CLEAR:'Clear',CLOUDS:'Cloudy',RAIN:'Rain',DRIZZLE:'Drizzle',THUNDERSTORM:'Thunderstorms',SNOW:'Snow',MIST:'Mist',FOG:'Fog',HAZE:'Haze'};
var DAYS={SUN:'Sun',MON:'Mon',TUE:'Tue',WED:'Wed',THU:'Thu',FRI:'Fri',SAT:'Sat'};
function esc(s){return String(s).replace(/[&<>"]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c];});}
function cityTabs(d){var t=$('cityTabs');if(!t)return;var h='';d.cityNames.forEach(function(n,i){h+='<button class="'+(i==d.cityIndex?'on':'')+'" onclick="setCity('+i+')">'+esc(n)+'</button>';});t.innerHTML=h;}
function setCity(i){if(cur&&i==cur.cityIndex)return;$('desc').innerText='Loading...';api('/live?action=city&value='+i).then(refresh);}
function rainChart(d){
 var pts=d.rain||[],tot=0;pts.forEach(function(p){tot+=p.mm;});$('rainTotal').innerText=pts.length?tot.toFixed(1)+' mm in 15 h':'';
 if(!pts.length){$('rainChart').innerHTML='<div class="empty">No forecast data</div>';return;}
 var W=Math.max(300,Math.round($('rainChart').clientWidth||760)),Hh=W<500?190:230,l=40,r=10,t=16,b=30,max=Math.max(1,Math.max.apply(null,pts.map(function(p){return p.mm;}))),bw=(W-l-r)/pts.length,o='';
 var st=max<=2?0.5:(max<=5?1:(max<=10?2:5));max=Math.ceil(max/st)*st;
 for(var v=0;v<=max+1e-9;v+=st){var y=Hh-b-(Hh-t-b)*v/max;o+='<line x1="'+l+'" x2="'+(W-r)+'" y1="'+y+'" y2="'+y+'" stroke="var(--line)"/><text x="'+(l-8)+'" y="'+(y+4)+'" font-size="12" fill="var(--muted)" text-anchor="end">'+(+v.toFixed(1))+'</text>';}
 pts.forEach(function(p,i){var x=l+i*bw+bw*.2,w=bw*.6,hh=(Hh-t-b)*p.mm/max,y=Hh-b-hh;
  if(p.mm>0)o+='<rect x="'+x+'" y="'+y+'" width="'+w+'" height="'+Math.max(hh,3)+'" rx="6" fill="var(--rain)"/><text x="'+(x+w/2)+'" y="'+(y-6)+'" font-size="12" font-weight="600" fill="var(--text)" text-anchor="middle">'+p.mm.toFixed(1)+'</text>';
  else o+='<rect x="'+x+'" y="'+(Hh-b-3)+'" width="'+w+'" height="3" rx="1.5" fill="var(--line)"/>';
  o+='<text x="'+(x+w/2)+'" y="'+(Hh-9)+'" font-size="12" fill="var(--muted)" text-anchor="middle">'+esc(p.label)+'</text>';});
 o+='<text x="'+l+'" y="11" font-size="11" fill="var(--muted)">mm per 3 h</text>';
 $('rainChart').innerHTML='<svg viewBox="0 0 '+W+' '+Hh+'" style="width:100%;height:auto;display:block">'+o+'</svg>';}
// Next 2 hours: one bar per step (5 min radar / 15 min forecast), same rules as the display
function nowcast(d){var n=d.nowcast||{},el=$('nowcast');
 if(!n.valid){el.innerHTML='';$('rain15Label').style.display='none';return;}$('rain15Label').style.display='';
 var st=n.steps,W=Math.max(300,Math.round(el.clientWidth||760)),Hh=120,l=6,r=6,t=10,b=24,sw=(W-l-r)/st.length,o='',per=Math.round(30/n.stepMin);
 var mx=Math.max(1,Math.max.apply(null,st.map(function(x){return Math.max(x[1],Math.min(8,x[2]||0));})));
 st.forEach(function(x,i){var bx=l+i*sw+1,w=Math.max(2,sw-2);
  // faint bar behind: what the weather model expects for this step
  if(x[2]!=null&&x[2]>=0.1){var mh=Math.min(Hh-t-b,Math.max(3,(Hh-t-b)*x[2]/mx));o+='<rect x="'+bx+'" y="'+(Hh-b-mh)+'" width="'+w+'" height="'+mh+'" rx="2" fill="var(--rain)" opacity=".28"><title>'+x[0]+' model '+x[2].toFixed(1)+' mm/h</title></rect>';}
  if(x[1]>=0.1){var h=Math.max(3,(Hh-t-b)*x[1]/mx);o+='<rect x="'+bx+'" y="'+(Hh-b-h)+'" width="'+w+'" height="'+h+'" rx="2" fill="var(--rain)"><title>'+x[0]+' '+x[1].toFixed(1)+' mm/h</title></rect>';}
  else if(!(x[2]>=0.1))o+='<rect x="'+bx+'" y="'+(Hh-b-2)+'" width="'+w+'" height="2" fill="var(--line)"/>';
  if(i%per==0)o+='<text x="'+(bx+w/2)+'" y="'+(Hh-7)+'" font-size="12" fill="var(--muted)" text-anchor="middle">'+(i==0?'now':x[0])+'</text>';});
 el.innerHTML='<div class="nowmsg"><b'+(n.blend?' class="soft"':'')+'>'+esc(n.msg)+'</b><span class="tag">'+(n.radar?'RADAR':'FORECAST')+' &middot; '+n.ageMin+' min ago</span></div><svg viewBox="0 0 '+W+' '+Hh+'" style="width:100%;height:auto;display:block">'+o+'</svg>'
  +(n.model?'<div class="legend"><span><i style="background:var(--rain)"></i>Radar</span><span><i style="background:var(--rain);opacity:.28"></i>Weather model</span></div>':'');}
// Forecast check: how often each source got rain right, scored against the radar afterwards
var VF_NEED=10;
function verifyCard(d){var v=d.verify,c=$('vfCard');if(!v){c.style.display='none';return;}c.style.display='';
 if(!v.radar){$('vfSince').innerText='';$('vf').innerHTML='<div class="muted small">Needs a city inside the Buienradar radar area (Netherlands, Belgium and the edges of the neighbouring countries) to compare forecasts with what really fell.</div>';return;}
 var since=v.since?new Date(v.since*1000):null;$('vfSince').innerText=since?'since '+since.toLocaleDateString(undefined,{day:'numeric',month:'short'}):'';
 var rainy=0,best=-1,bestCsi=-1,cur=-1,o='';
 v.sources.forEach(function(s){rainy=Math.max(rainy,s.hits+s.misses);});
 v.sources.forEach(function(s,i){var all=s.hits+s.misses+s.falseAlarms,n=all+s.dryOK,csi=all?s.hits/all:null;
  if(i==v.current)cur=csi;
  if(csi!=null&&s.hits+s.misses>=VF_NEED&&csi>bestCsi){bestCsi=csi;best=i;}
  o+='<div class="vfrow"><b'+(i==v.current?' class="cur"':'')+'>'+esc(s.name)+'</b><span class="sc">'+(csi==null?'&ndash;':Math.round(csi*100)+'%')+'</span>'
   +'<div class="bar"><i style="width:'+(csi==null?0:Math.round(csi*100))+'%"></i></div>'
   +'<span class="muted">'+(n?'Caught '+s.hits+' of '+(s.hits+s.misses)+' rainy periods &middot; '+s.falseAlarms+' false alarm'+(s.falseAlarms==1?'':'s')+' &middot; '+s.dryOK+' dry right':'No periods scored yet')+'</span></div>';});
 var rec;
 if(rainy<VF_NEED)rec='<span class="muted">Collecting: '+rainy+' of '+VF_NEED+' rainy periods. Each 3-hour period is checked against the radar.</span>';
 else if(best==v.current||best<0||bestCsi-(cur||0)<0.05)rec='<span class="muted">Your current source is doing as well as any.</span>';
 else rec='<span><b>'+esc(v.sources[best].name)+'</b> got rain right most often here.</span><button class="btn primary" onclick="useSource('+best+',this)">Use it</button>';
 $('vf').innerHTML='<div class="vf">'+o+'</div><div class="vfrec">'+rec+'</div>';}
function useSource(p,b){b.disabled=true;b.innerText='Switching...';api('/live?action=provider&value='+p).then(refresh);}
function toMin(s){return s&&s.length>=5?(+s.slice(0,2))*60+(+s.slice(3,5)):-1;}
function sunArc(d){var rise=toMin(d.sunRise),set=toMin(d.sunSet),now=d.hour*60+d.minute,cx=150,cy=108,R=92;
 var o='<path d="M'+(cx-R)+' '+cy+' A'+R+' '+R+' 0 0 1 '+(cx+R)+' '+cy+'" fill="none" stroke="var(--line)" stroke-width="3" stroke-dasharray="4 6"/><line x1="20" x2="280" y1="'+cy+'" y2="'+cy+'" stroke="var(--line)"/>';
 if(rise>=0&&set>rise&&now>=rise&&now<=set){var f=(now-rise)/(set-rise),a=Math.PI*(1-f),sx=cx+R*Math.cos(a),sy=cy-R*Math.sin(a);
  o+='<path d="M'+(cx-R)+' '+cy+' A'+R+' '+R+' 0 0 1 '+sx.toFixed(1)+' '+sy.toFixed(1)+'" fill="none" stroke="var(--accent)" stroke-width="3"/><circle cx="'+sx+'" cy="'+sy+'" r="16" fill="#fbbf24" opacity=".25"/><circle cx="'+sx+'" cy="'+sy+'" r="10" fill="#fbbf24"/>';}
 else o+='<circle cx="'+cx+'" cy="'+(cy-50)+'" r="12" fill="#e8d9a8"/><circle cx="'+(cx+6)+'" cy="'+(cy-54)+'" r="10" fill="var(--card)"/>';
 $('sunArc').innerHTML=o;}
// Moon age from a known new moon (6 Jan 2000 18:14 UTC); drawn as a lit fraction
function moonSvg(f){var r=18,c=22,k=Math.cos(2*Math.PI*f)*r,wax=f<.5,o='<circle cx="22" cy="22" r="20" fill="var(--card2)" stroke="var(--line)"/>';
 // outer edge: right half while waxing, left half while waning; inner edge: an ellipse (terminator) whose width follows cos(phase)
 o+='<path d="M'+c+' '+(c-r)+' A'+r+' '+r+' 0 0 '+(wax?1:0)+' '+c+' '+(c+r)+' A'+Math.abs(k).toFixed(2)+' '+r+' 0 0 '+((k>0)==wax?0:1)+' '+c+' '+(c-r)+'Z" fill="#e8d9a8"/>';
 return o;}
function moon(){var syn=29.530588853,age=((Date.now()/864e5-10962.7597)%syn+syn)%syn,f=age/syn;
 $('moonIcon').innerHTML=cur&&cur.southern?'<g transform="matrix(-1 0 0 1 44 0)">'+moonSvg(f)+'</g>':moonSvg(f);var toFull=f<.5?(.5-f)*syn:(1.5-f)*syn,toNew=(1-f)*syn;
 $('moonNext').innerText=toFull<toNew?'Full moon in '+Math.round(toFull)+' day'+(Math.round(toFull)==1?'':'s'):'New moon in '+Math.round(toNew)+' day'+(Math.round(toNew)==1?'':'s');}
function forecast(d){var v=d.forecast.filter(function(x){return x.day;});if(!v.length){$('fc').innerHTML='<div class="empty">No forecast data</div>';return;}
 var mn=Math.min.apply(null,v.map(function(x){return x.low;})),mx=Math.max.apply(null,v.map(function(x){return x.high;}));if(mx-mn<1)mx=mn+1;var o='';
 d.forecast.forEach(function(x,i){if(!x.day){o+='<div class="fcrow"><b class="muted">&ndash;</b><span></span><span class="what">No data</span></div>';return;}
  var a=(x.low-mn)/(mx-mn)*100,b=(x.high-mn)/(mx-mn)*100;
  o+='<div class="fcrow"><b>'+(i==0?'Today':(DAYS[x.day]||x.day))+'</b>'+iconSvg(iconKind(x.main,''),false)+'<span class="what">'+(WORDS[x.main]||esc(x.main))+'</span><span class="lo">'+Math.round(x.low)+'&deg;</span><div class="range"><i style="left:'+a+'%;right:'+(100-b)+'%"></i></div><span class="hi">'+Math.round(x.high)+'&deg;</span></div>';});
 $('fc').innerHTML=o;}
async function refresh(){
 try{var d=await (await fetch('/api/status')).json();cur=d;cityTabs(d);
  setAccentFromText([d.textR,d.textG,d.textB]);
  $('heroIcon').innerHTML=iconSvg(iconKind(d.main,d.weatherDesc),d.night);
  $('temp').innerHTML=d.temp.toFixed(1)+'<sup>&deg;C</sup>';
  $('desc').innerText=(d.weatherDesc||'').toLowerCase();
  $('hl').innerHTML='H '+d.tempMax.toFixed(1)+'&deg; &middot; L '+d.tempMin.toFixed(1)+'&deg; &middot; Feels '+d.feelsLike.toFixed(1)+'&deg;';
  var txt=document.documentElement.style.getPropertyValue('--accent')?'var(--accent)':'rgb('+d.textR+','+d.textG+','+d.textB+')';
  $('clock').innerHTML=segClock(d.hour,d.minute,'var(--text)',(d.textR>235&&d.textG>235&&d.textB>235)||(d.textR+d.textG+d.textB<40)?'var(--text)':txt);
  $('date').innerText=d.dateLong;
  $('wind').innerHTML=Math.round(d.windSpeed)+' <small>km/h '+esc(d.windDir)+'</small>';
  $('windArrow').setAttribute('transform','rotate('+(((DIRS[d.windDir]||0)+180)%360)+' 15 15)');
  $('humid').innerHTML=d.humid+' <small>%</small>';$('humidBar').style.width=Math.min(100,d.humid)+'%';
  $('baro').innerHTML=d.pressure+' <small>hPa</small>';
  $('vis').innerHTML=d.visibility+' <small>km</small>';$('clouds').innerText='Clouds '+d.clouds+'%';
  rainChart(d);nowcast(d);sunArc(d);moon();forecast(d);verifyCard(d);
  var pt=d.pTrend||{},ar={'-3':'\u25bc\u25bc','-2':'\u25bc\u25bc','-1':'\u25bc','0':'','1':'\u25b2','2':'\u25b2\u25b2','3':'\u25b2\u25b2'};
  $('pTrend').innerHTML=pt.valid?(ar[pt.level]+' '+esc(pt.text)+' <span class="muted">('+(pt.rate>0?'+':'')+pt.rate.toFixed(1)+' hPa/3h)</span>'+(pt.hint?'<br>'+esc(pt.hint):'')):'sea level';
  $('pTrend').style.color=pt.valid&&Math.abs(pt.level)>=2?'var(--text)':'';
  $('sunRise').innerText=d.sunRise||'--';$('sunSet').innerText=d.sunSet||'--';
  $('sunEvent').innerText=d.sunEvent=='SUNRISE'?'Sunrise in':'Sunset in';$('sunIn').innerText=d.sunDiffMin>=0?Math.floor(d.sunDiffMin/60)+'h '+(d.sunDiffMin%60)+'m':'--';
  $('moonName').innerText=(d.moonPhase||'').toLowerCase().replace(/^./,function(c){return c.toUpperCase();});
  if(d.bmeOK){$('indoorCard').style.display='';$('inTemp').innerHTML=d.localTemp.toFixed(1)+'&deg;';$('inHum').innerText=Math.round(d.localHumid)+'% humidity';$('inPres').innerText=Math.round(d.localPressure)+' hPa';}
  $('ledSw').style.background='rgb('+d.ledR+','+d.ledG+','+d.ledB+')';$('txtSw').style.background='rgb('+d.textR+','+d.textG+','+d.textB+')';
  $('stDot').style.background=d.lastFetchOK?'var(--good)':'var(--bad)';$('brandDot').className=d.lastFetchOK?'':'bad';
  $('stText').innerText=(d.lastFetchOK?'Updated '+d.ageMin+' min ago':'Last update failed')+' · '+d.provider+' · Wi-Fi '+d.rssi+' dBm';
 }catch(e){$('stText').innerText='Connection lost - retrying';$('stDot').style.background='var(--bad)';}
}
document.addEventListener('themechange',function(){if(cur){refresh();}});
var rz;window.addEventListener('resize',function(){clearTimeout(rz);rz=setTimeout(function(){if(cur){rainChart(cur);nowcast(cur);}},150);});
refresh();setInterval(refresh,7000);
</script>
)rawliteral";

const char SETTINGS_BODY[] PROGMEM = R"rawliteral(
<form class="page" id="settingsForm" onsubmit="return saveSettings(this)" autocomplete="off">
<h1>Settings</h1>
<section class="card"><h3>Cities &amp; weather source</h3>
 <div class="field"><span>City 1</span><div class="row2"><input type="text" name="c1" id="c1" placeholder="City,CountryCode"><select name="provider1" id="provider1"></select></div></div>
 <div class="field"><span>City 2</span><div class="row2"><input type="text" name="c2" id="c2" placeholder="City,CountryCode"><select name="provider2" id="provider2"></select></div></div>
 <div class="field"><span>City 3</span><div class="row2"><input type="text" name="c3" id="c3" placeholder="City,CountryCode"><select name="provider3" id="provider3"></select></div></div>
 <p class="note">Write cities as <b>City,CountryCode</b>, e.g. Amsterdam,NL. Open-Meteo needs no key for the weather itself; the API key below is still used to look up the city's coordinates. The KNMI option uses KNMI's 2&nbsp;km Netherlands model, topped up with ECMWF. Renaming a city starts a fresh history for it (the old log is kept on the SD card).</p>
</section>
<section class="card"><h3>OpenWeatherMap API keys</h3>
 <div class="field"><span>API key</span><input type="text" name="api" id="api" spellcheck="false"></div>
 <div class="field" style="margin:0"><span>Backup API key (optional, used if the main key is rejected or rate-limited)</span><input type="text" name="apiBackup" id="apiBackup" spellcheck="false"></div>
</section>
<section class="card"><h3>Display</h3>
 <div class="field"><span>Screen theme</span><select name="theme" id="theme"><option value="2">Auto &ndash; dark from sunset to sunrise</option><option value="0">Light</option><option value="1">Dark</option></select></div>
 <label class="switch"><div>Upcoming rain on the home screen<small>Bars for the next 15 hours</small></div><input type="checkbox" name="rainPrevOn" id="rainPrevOn" value="1"></label>
 <label class="switch"><div>Home screen animation<small>Shown instead of the rain bars when those are off</small></div><input type="checkbox" name="animOn" id="animOn" value="1"></label>
 <label class="switch"><div>Pulse the LED when rain is coming<small>Two slow blue pulses when rain is expected within 30 minutes. Once per rain spell; uses radar in NL/BE.</small></div><input type="checkbox" name="rainLed" id="rainLed" value="1"></label>
 <div style="margin:2px 0 6px"><button type="button" class="btn" onclick="var b=this;api('/live?action=testPulse').then(function(){flash(b,'Pulsing...');})">Test the pulse</button></div>
 <div class="label" style="margin-top:14px">Brightness <span class="muted" style="text-transform:none;letter-spacing:0;font-weight:500">&middot; applies right away, saved automatically</span></div>
 <div class="slider"><span>Screen</span><input type="range" id="screenSlider" min="0" max="100"><b id="screenVal"></b></div>
 <div class="slider"><span>LED</span><input type="range" id="ledSlider" min="0" max="100"><b id="ledVal"></b></div>
</section>
<section class="card"><h3>LED color <span>applies right away</span></h3>
 <div class="seg" id="modeSeg"><button type="button" data-m="0">Weather sync</button><button type="button" data-m="1">Custom color</button></div>
 <div id="colorSection" style="margin-top:14px">
  <div style="display:flex;gap:14px;align-items:center"><div class="swatch" id="swatch"></div><div class="note" style="margin:0">Orange = clear, white = clouds, blue = rain, purple = thunder, cyan = snow in weather sync mode.</div></div>
  <div class="slider"><span>Hue</span><input type="range" id="hueSlider" min="0" max="360"><b id="hueVal"></b></div>
  <div class="slider"><span>Saturation</span><input type="range" id="satSlider" min="0" max="100"><b id="satVal"></b></div>
  <div class="slider"><span>Lightness</span><input type="range" id="valSlider" min="0" max="100"><b id="valVal"></b></div>
  <button type="button" class="btn" onclick="saveColor(this)">Save as default</button>
 </div>
</section>
<section class="card"><h3>Text &amp; accent color <span>applies right away</span></h3>
 <div style="display:flex;gap:14px;align-items:center"><div class="swatch" id="textSwatch"></div><div class="note" style="margin:0">Colors the clock minutes and IP address on the display. When it's a clear color it's also used as the accent for highlights (page dots, sliders, buttons) on the display and here.</div></div>
 <div class="slider"><span>Hue</span><input type="range" id="textHueSlider" min="0" max="360"><b id="textHueVal"></b></div>
 <div class="slider"><span>Saturation</span><input type="range" id="textSatSlider" min="0" max="100"><b id="textSatVal"></b></div>
 <div class="slider"><span>Lightness</span><input type="range" id="textValSlider" min="0" max="100"><b id="textValVal"></b></div>
 <button type="button" class="btn" onclick="saveTextColor(this)">Save as default</button>
</section>
<div class="savebar"><span id="saveMsg">Cities, keys and display options are saved with this button.</span><button class="btn primary" type="submit">Save settings</button></div>
</form>
<script>
var PROV=['OpenWeatherMap','Open-Meteo','Open-Meteo KNMI (NL)'],lastSent=0;
function hsv(h,s,v){s/=100;v/=100;var f=function(n){var k=(n+h/60)%6;return v-v*s*Math.max(0,Math.min(k,4-k,1));};return [Math.round(f(5)*255),Math.round(f(3)*255),Math.round(f(1)*255)];}
function liveSend(a,v,force){var n=Date.now();if(!force&&n-lastSent<60)return;lastSent=n;api('/live?action='+a+'&value='+v);}
function flash(b,t){var o=b.innerText;b.innerText=t;setTimeout(function(){b.innerText=o;},1400);}
function saveColor(b){api('/saveColor').then(function(){flash(b,'Saved');});}
function saveTextColor(b){api('/saveTextColor').then(function(){flash(b,'Saved');});}
function saveSettings(f){var m=$('saveMsg');m.innerText='Saving...';
 api('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(new FormData(f))})
 .then(function(r){return r.text().then(function(t){m.innerText=r.ok?'Saved - the display is fetching fresh weather.':t;});}).catch(function(e){m.innerText='Save failed: '+e;});return false;}
function setMode(m,send){document.querySelectorAll('#modeSeg button').forEach(function(b){b.classList.toggle('on',b.dataset.m==m);});$('colorSection').style.display=m==1?'':'none';if(send)api('/live?action=mode&value='+m);}
function paint(){var c=hsv(+$('hueSlider').value,+$('satSlider').value,+$('valSlider').value);$('swatch').style.background='rgb('+c+')';
 var t=hsv(+$('textHueSlider').value,+$('textSatSlider').value,+$('textValSlider').value);$('textSwatch').style.background='rgb('+t+')';setAccentFromText(t);
 var rb='linear-gradient(to right,'+[0,60,120,180,240,300,360].map(function(h){return 'hsl('+h+',100%,50%)';}).join(',')+')';$('hueSlider').style.background=rb;$('textHueSlider').style.background=rb;}
function bind(id,action,valId,suffix){var s=$(id);s.oninput=function(){$(valId).innerText=s.value+(suffix||'');paint();liveSend(action,s.value,false);};s.onchange=function(){liveSend(action,s.value,true);};}
fetch('/api/settings').then(function(r){return r.json();}).then(function(d){
 for(var i=1;i<=3;i++){$('c'+i).value=d.cities[i-1];var sel=$('provider'+i);sel.innerHTML=PROV.map(function(p,k){return '<option value="'+k+'">'+p+'</option>';}).join('');sel.value=d.providers[i-1];}
 $('api').value=d.api;$('apiBackup').value=d.apiBackup;$('theme').value=d.theme;$('animOn').checked=d.animOn;$('rainPrevOn').checked=d.rainPrevOn;$('rainLed').checked=d.rainLed;
 var set=function(id,v,suffix){$(id+'Slider').value=v;$(id+'Val').innerText=v+(suffix||'');};
 set('screen',d.screenPct,'%');set('led',d.ledPct,'%');set('hue',d.hue);set('sat',d.satPct,'%');set('val',d.valPct,'%');
 set('textHue',d.textHue);set('textSat',d.textSatPct,'%');set('textVal',d.textValPct,'%');
 setMode(d.useCustom?1:0,false);paint();
});
bind('screenSlider','screenBright','screenVal','%');bind('ledSlider','ledBright','ledVal','%');
bind('hueSlider','hue','hueVal');bind('satSlider','sat','satVal','%');bind('valSlider','val','valVal','%');
bind('textHueSlider','textHue','textHueVal');bind('textSatSlider','textSat','textSatVal','%');bind('textValSlider','textVal','textValVal','%');
document.querySelectorAll('#modeSeg button').forEach(function(b){b.onclick=function(){setMode(b.dataset.m,true);};});
</script>
)rawliteral";

const char DEBUG_BODY[] PROGMEM = R"rawliteral(
<style>
.con{background:#0b0f14;color:#7ee787;font-family:ui-monospace,Menlo,Consolas,'Courier New',monospace;border-radius:12px;padding:12px 14px;border:1px solid var(--line)}
.drow{display:flex;justify-content:space-between;gap:12px;padding:8px 0;border-bottom:1px solid var(--line);font-size:.93em}
.drow:last-child{border:0}.drow span:last-child{font-family:ui-monospace,Menlo,Consolas,monospace;font-weight:600}
.hot{color:#d29922}.bad{color:var(--bad)}.ok{color:var(--good)}
#log{height:340px;overflow-y:auto;font-size:.84em;white-space:pre-wrap}
</style>
<main class="page"><h1>Debug</h1>
<section class="card"><h3>Touch &amp; system <span>live</span></h3>
 <div class="drow"><span>Touch pressed (raw)</span><span id="pressed">--</span></div>
 <div class="drow"><span>Raw X / Y</span><span id="xy">--</span></div>
 <div class="drow"><span>isTouched (internal)</span><span id="isTouched">--</span></div>
 <div class="drow"><span>Held for</span><span id="held">--</span></div>
 <div class="drow"><span>Stuck-touch auto-resets</span><span id="stuck">--</span></div>
 <div class="drow"><span>Display page</span><span id="mode">--</span></div>
 <div class="drow"><span>Uptime</span><span id="uptime">--</span></div>
 <div class="drow"><span>Free heap</span><span id="heap">--</span></div>
 <div class="drow"><span>Longest freeze</span><span id="loopMax">--</span></div>
 <div class="drow"><span>WiFi</span><span id="wifi">--</span></div>
</section>
<section class="card"><h3>Data sources <span>last fetch per feed</span></h3><div id="sources">Loading...</div></section>
<section class="card"><h3>Event log <span>newest first</span></h3><div class="con" id="log">Loading...</div></section>
</main>
<script>
var PAGES=['Home','Atmosphere','Precipitation','Sun & Moon','System','Forecast','History'];
async function refresh(){
 try{var d=await (await fetch('/api/debug')).json();
  var p=$('pressed');p.innerText=d.pressed;p.className=d.pressed?'hot':'ok';
  $('xy').innerText=d.rawX+', '+d.rawY;
  var it=$('isTouched');it.innerText=d.isTouched;it.className=d.isTouched?'hot':'ok';
  $('held').innerText=d.isTouched?(d.msHeld+' ms'):'-';
  var sr=$('stuck');sr.innerText=d.stuckResets;sr.className=d.stuckResets>0?'bad':'ok';
  $('mode').innerText=d.displayMode+' ('+(PAGES[d.displayMode]||'?')+')';
  var u=d.uptimeSec;$('uptime').innerText=(u>=86400?Math.floor(u/86400)+'d ':'')+Math.floor(u%86400/3600)+'h '+Math.floor(u%3600/60)+'m '+(u%60)+'s';
  $('heap').innerText=(d.freeHeap/1024).toFixed(1)+' KB'+(d.minHeap?' (lowest '+(d.minHeap/1024).toFixed(1)+' KB)':'');
  $('loopMax').innerText=d.loopMaxMs==null?'--':(d.loopMaxMs/1000).toFixed(1)+' s';
  var w=$('wifi');w.innerText=d.wifiConnected?'connected':'DISCONNECTED';w.className=d.wifiConnected?'ok':'bad';
  $('log').innerText=d.log.join('\n');
  $('sources').innerHTML=(d.sources||[]).map(function(x){return '<div class="drow" style="display:block"><div style="display:flex;justify-content:space-between;gap:10px"><b>'+x.name+'</b><span class="'+(x.ok?'ok':'bad')+'">'+(x.ok?'OK':'not available')+' &middot; '+x.age+'</span></div><div class="muted small" style="margin-top:3px;font-family:inherit;font-weight:400">'+String(x.info).replace(/[&<>]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;'}[c];})+'</div></div>';}).join('');
 }catch(e){$('log').innerText='connection lost: '+e;}
}
refresh();setInterval(refresh,400);
</script>
)rawliteral";