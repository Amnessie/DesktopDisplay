#include <Arduino.h>
#include "ArduinoJson.h"
#include <TimeLib.h>
#include <HTTPClient.h>
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
#include "img/moon0.h"
#include "img/moon1.h"
#include "img/moon2.h"
#include "img/moon3.h"
#include "img/moon4.h"
#include "img/moon5.h"
#include "img/moon6.h"
#include "img/moon7.h"


// NEW: arrays of pointers instead of repeated switch statements (code hygiene)
const uint8_t* moonImages[8] = { moon0, moon1, moon2, moon3, moon4, moon5, moon6, moon7 };
const size_t   moonSizes[8]  = { sizeof(moon0), sizeof(moon1), sizeof(moon2), sizeof(moon3),
                                  sizeof(moon4), sizeof(moon5), sizeof(moon6), sizeof(moon7) };

const uint8_t* animFrames[10] = { i0, i1, i2, i3, i4, i5, i6, i7, i8, i9 };
const size_t   animSizes[10]  = { sizeof(i0), sizeof(i1), sizeof(i2), sizeof(i3), sizeof(i4),
                                   sizeof(i5), sizeof(i6), sizeof(i7), sizeof(i8), sizeof(i9) };

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite clk = TFT_eSprite(&tft); 

// --- [SYSTEM STATE] ---
uint16_t bgColor = 0xCE79;
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
const int STATUS_DOT_X        = 270;
const int STATUS_DOT_Y        = 395; // NEW: raised to sit level with the "NEXT RAIN" title, was overlapping the rain preview's rightmost point down at 450
const int STATUS_DOT_R        = 5;
const int NAV_DOT_Y           = 474;
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
  if (r > 235 && g > 235 && b > 235) return TFT_BLACK;
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

// NEW: recomputes textR/G/B from the text color's own hue/saturation/value.
// Deliberately separate from applyCustomColor()/the LED for the reason
// above - this only affects the clock's minute digits and the IP text.
void applyTextColor() {
  HSBtoRGB(textHue, textSat, textVal, textR, textG, textB);
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
  tft.fillScreen(bgColor); tft.setTextColor(TFT_BLACK); tft.setTextDatum(CC_DATUM);
  tft.drawString("WIFI SETUP", 160, 90, 4);
  tft.drawString("Connect to WiFi:", 160, 150, 2);
  tft.drawString("WeatherStation-Setup", 160, 175, 2);
  tft.drawString("Then open:", 160, 220, 2);
  tft.drawString("192.168.4.1", 160, 250, 4);
}

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

int getMoonPhaseIndex(int d, int m, int y) {
  int c, e; double jd; int b;
  if (m < 3) { y--; m += 12; } m++;
  c = 365.25 * y; e = 30.6 * m;
  jd = c + e + d - 694039.09; jd /= 29.5305882; 
  b = jd; jd -= b; b = jd * 8 + 0.5; b = b & 7;
  return b;
}

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
  http.begin(url);
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
    tft.fillRoundRect(UPDATE_BADGE_X, UPDATE_BADGE_Y, UPDATE_BADGE_W, UPDATE_BADGE_H, 4, TFT_ORANGE);
    tft.setTextColor(TFT_BLACK, TFT_ORANGE); tft.setTextDatum(CC_DATUM);
    tft.drawString("...", UPDATE_BADGE_X + UPDATE_BADGE_W/2, UPDATE_BADGE_Y + UPDATE_BADGE_H/2, 1);
  }
  // no separate "hide" drawing needed - the subsequent drawPageBackground() call
  // repaints that whole area anyway.
}

void triggerWeatherUpdate() {
  isUpdating = true;
  drawUpdatingBadge(true);
  getOpenWeatherData();
  isUpdating = false;
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

// NEW: builds the <select> dropdown for one city's weather-data provider
String buildProviderSelect(String fieldName, int currentProvider) {
  String html = "<select name='" + fieldName + "'>";
  html += "<option value='0'" + String(currentProvider == PROVIDER_OPENWEATHER ? " selected" : "") + ">OpenWeatherMap</option>";
  html += "<option value='1'" + String(currentProvider == PROVIDER_OPENMETEO_BEST ? " selected" : "") + ">Open-Meteo (Best Match)</option>";
  html += "<option value='2'" + String(currentProvider == PROVIDER_OPENMETEO_KNMI ? " selected" : "") + ">Open-Meteo (KNMI Netherlands)</option>";
  html += "</select>";
  return html;
}

// --- [SMARTER WEB PORTAL FUNCTIONS] ---
// UPDATE 2: Added API key input to HTML and Saving Process
void handleRoot() {
  // NEW: current values, converted to the 0-100% scale the sliders use
  int screenPct = map(globalBrightness, 10, 255, 0, 100);
  int ledPct    = map(ledBrightness, 0, 255, 0, 100);
  int satPct    = map(customSat, 0, 255, 0, 100);
  int valPct    = map(customVal, 0, 255, 0, 100);
  int textHuePct  = textHue; // hue stays 0-360, no conversion needed
  int textSatPct  = map(textSat, 0, 255, 0, 100);
  int textValPct  = map(textVal, 0, 255, 0, 100);

  String html = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><style>";
  html += "body{font-family:sans-serif;text-align:center;max-width:480px;margin:0 auto;padding:10px;}";
  html += "h1{font-size:1.4em;} h2{font-size:1.1em;margin-top:30px;border-top:1px solid #ccc;padding-top:15px;}";
  html += "input[type=range]{width:90%;height:22px;-webkit-appearance:none;border-radius:11px;margin:8px 0;}";
  html += "input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:22px;height:22px;border-radius:50%;background:#fff;border:2px solid #333;cursor:pointer;}";
  html += "input[type=range]::-moz-range-thumb{width:22px;height:22px;border-radius:50%;background:#fff;border:2px solid #333;cursor:pointer;}";
  html += "#colorSection{display:" + String(useCustomColor ? "block" : "none") + ";}";
  html += "#swatch,#textSwatch{width:80px;height:80px;border-radius:10px;border:2px solid #333;margin:10px auto;}";
  html += ".row{margin:14px 0;}";
  html += "</style></head><body>";

  html += "<h1>Weather Station Portal</h1>";
  html += navBar("settings");

  // Submitted from JavaScript (saveSettings below) so the request can carry the
  // X-Requested-With header that /save requires - see requireAppHeader().
  html += "<form id='settingsForm' onsubmit='return saveSettings(this)'>";
  html += "City 1: <input type='text' name='c1' value='" + htmlEsc(locs[0]) + "'> " + buildProviderSelect("provider1", cityProvider[0]) + "<br><br>";
  html += "City 2: <input type='text' name='c2' value='" + htmlEsc(locs[1]) + "'> " + buildProviderSelect("provider2", cityProvider[1]) + "<br><br>";
  html += "City 3: <input type='text' name='c3' value='" + htmlEsc(locs[2]) + "'> " + buildProviderSelect("provider3", cityProvider[2]) + "<br><br>";
  html += "<p style='font-size:0.9em;color:#555;'>OpenWeatherMap uses the API key below. The Open-Meteo weather itself needs no key (the key is still used to look up the city's coordinates). The KNMI option uses KNMI's 2km Netherlands model, topped up with ECMWF beyond its ~2.5-day range.</p>";
  html += "<p style='font-size:0.85em;color:#777;'>Renaming a city starts a fresh history for it (the old log is kept on the SD card).</p>";
  html += "API Key: <input type='text' name='api' style='width:300px;' value='" + htmlEsc(apiKey) + "'><br><br>";
  html += "Backup API Key (optional): <input type='text' name='apiBackup' style='width:300px;' value='" + htmlEsc(apiKeyBackup) + "'><br><br>";
  html += "Show Home Animation: <input type='checkbox' name='animOn' value='1'" + String(showAnimation ? " checked" : "") + "><br><br>";
  html += "Show Upcoming Rain Preview: <input type='checkbox' name='rainPrevOn' value='1'" + String(showRainPreview ? " checked" : "") + "><br>";
  html += "<span style='font-size:0.85em;color:#777;'>(shares the same spot as the animation above - only one shows at a time)</span><br><br>";
  html += "<input type='submit' value='Update Settings'> <span id='saveMsg'></span>";
  html += "</form>";

  // NEW: Display controls - mirrors the touchscreen's System Info page, but
  // live: every slider here applies immediately over WiFi as you drag it,
  // the same way dragging it on the physical screen would.
  html += "<h2>Display Brightness (live)</h2>";

  html += "<div class='row'>Screen Brightness: <span id='screenVal'>" + String(screenPct) + "</span>%<br>";
  html += "<input type='range' id='screenSlider' min='0' max='100' value='" + String(screenPct) + "' style='background:linear-gradient(to right,#333,#fff);'></div>";

  html += "<div class='row'>LED Brightness: <span id='ledVal'>" + String(ledPct) + "</span>%<br>";
  html += "<input type='range' id='ledSlider' min='0' max='100' value='" + String(ledPct) + "' style='background:linear-gradient(to right,#333,#fff);'></div>";

  // NEW: LED color - nothing here is written to flash except the mode
  // toggle (matching the touchscreen's own behavior) - use "Save Color as
  // Default" to make a custom color survive a reboot.
  html += "<h2>LED Color (live)</h2>";
  html += "<div class='row'>";
  html += "<label><input type='radio' name='mode' value='0'" + String(!useCustomColor ? " checked" : "") + " onclick='setMode(0)'> Weather Sync</label> &nbsp; ";
  html += "<label><input type='radio' name='mode' value='1'" + String(useCustomColor ? " checked" : "") + " onclick='setMode(1)'> Custom Color</label>";
  html += "</div>";

  html += "<div id='colorSection'>";
  html += "<div id='swatch'></div>";
  html += "<div class='row'>Hue: <span id='hueVal'>" + String(customHue) + "</span><br>";
  html += "<input type='range' id='hueSlider' min='0' max='360' value='" + String(customHue) + "'></div>";
  html += "<div class='row'>Saturation: <span id='satVal'>" + String(satPct) + "</span>%<br>";
  html += "<input type='range' id='satSlider' min='0' max='100' value='" + String(satPct) + "'></div>";
  html += "<div class='row'>Lightness: <span id='valVal'>" + String(valPct) + "</span>%<br>";
  html += "<input type='range' id='valSlider' min='0' max='100' value='" + String(valPct) + "'></div>";
  html += "<button onclick='saveColor()' type='button'>Save Color as Default</button>";
  html += "</div>";

  // NEW: independent text color (clock minutes + IP address) - deliberately
  // has no "mode" toggle and never syncs with the LED above; it's always
  // just whatever these three sliders say.
  html += "<h2>Text Color - Time &amp; IP (live)</h2>";
  html += "<div id='textSwatch'></div>";
  html += "<div class='row'>Hue: <span id='textHueVal'>" + String(textHuePct) + "</span><br>";
  html += "<input type='range' id='textHueSlider' min='0' max='360' value='" + String(textHuePct) + "'></div>";
  html += "<div class='row'>Saturation: <span id='textSatVal'>" + String(textSatPct) + "</span>%<br>";
  html += "<input type='range' id='textSatSlider' min='0' max='100' value='" + String(textSatPct) + "'></div>";
  html += "<div class='row'>Lightness: <span id='textValVal'>" + String(textValPct) + "</span>%<br>";
  html += "<input type='range' id='textValSlider' min='0' max='100' value='" + String(textValPct) + "'></div>";
  html += "<button onclick='saveTextColor()' type='button'>Save Text Color as Default</button>";

  // NEW: throttled live updates - 'oninput' fires continuously while
  // dragging (throttled to ~1 request/60ms so the ESP32's single-threaded
  // web server isn't flooded), 'onchange' fires once on release and always
  // sends, guaranteeing the final value is never dropped by the throttle.
  // Saturation/Lightness/swatch gradients are recomputed in JS as CSS's
  // native color model (HSL) - a close visual approximation of the HSV math
  // the ESP32 actually uses, so the on-screen preview may differ very
  // slightly from the physical LED, which is always exact.
  html += "<script>";
  html += "var screenSlider=document.getElementById('screenSlider'),ledSlider=document.getElementById('ledSlider');";
  html += "var hueSlider=document.getElementById('hueSlider'),satSlider=document.getElementById('satSlider'),valSlider=document.getElementById('valSlider');";
  html += "var textHueSlider=document.getElementById('textHueSlider'),textSatSlider=document.getElementById('textSatSlider'),textValSlider=document.getElementById('textValSlider');";
  html += "var lastSent=0,H={'X-Requested-With':'DesktopDisplay'};";
  html += "function api(u,o){o=o||{};o.headers=Object.assign({},o.headers||{},H);return fetch(u,o);}";
  html += "function saveSettings(f){var m=document.getElementById('saveMsg');m.innerText='Saving...';";
  html += "api('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(new FormData(f))})";
  html += ".then(r=>r.text().then(t=>{m.innerText=r.ok?'Saved - refreshing weather...':t;})).catch(e=>{m.innerText='Save failed: '+e;});return false;}";
  html += "function liveSend(action,value,force){var now=Date.now();if(!force&&now-lastSent<60)return;lastSent=now;api('/live?action='+action+'&value='+value);}";
  html += "function setMode(m){api('/live?action=mode&value='+m);document.getElementById('colorSection').style.display=(m==1?'block':'none');}";
  html += "function saveColor(){api('/saveColor');}";
  html += "function saveTextColor(){api('/saveTextColor');}";
  html += "function updateGradients(){";
  html += "var h=hueSlider.value,s=satSlider.value,v=valSlider.value;";
  html += "satSlider.style.background='linear-gradient(to right, hsl('+h+',0%,'+Math.round(v/2)+'%), hsl('+h+',100%,50%))';";
  html += "valSlider.style.background='linear-gradient(to right, black, hsl('+h+','+s+'%,50%))';";
  html += "var lApprox=Math.round(v*(200-s)/200);";
  html += "document.getElementById('swatch').style.background='hsl('+h+','+s+'%,'+lApprox+'%)';";
  html += "}";
  html += "function updateTextGradients(){";
  html += "var h=textHueSlider.value,s=textSatSlider.value,v=textValSlider.value;";
  html += "textSatSlider.style.background='linear-gradient(to right, hsl('+h+',0%,'+Math.round(v/2)+'%), hsl('+h+',100%,50%))';";
  html += "textValSlider.style.background='linear-gradient(to right, black, hsl('+h+','+s+'%,50%))';";
  html += "var lApprox=Math.round(v*(200-s)/200);";
  html += "document.getElementById('textSwatch').style.background='hsl('+h+','+s+'%,'+lApprox+'%)';";
  html += "}";
  html += "hueSlider.style.background='linear-gradient(to right, hsl(0,100%,50%), hsl(60,100%,50%), hsl(120,100%,50%), hsl(180,100%,50%), hsl(240,100%,50%), hsl(300,100%,50%), hsl(360,100%,50%))';";
  html += "textHueSlider.style.background=hueSlider.style.background;";
  html += "hueSlider.oninput=function(){document.getElementById('hueVal').innerText=this.value;updateGradients();liveSend('hue',this.value,false);};";
  html += "hueSlider.onchange=function(){liveSend('hue',this.value,true);};";
  html += "satSlider.oninput=function(){document.getElementById('satVal').innerText=this.value;updateGradients();liveSend('sat',this.value,false);};";
  html += "satSlider.onchange=function(){liveSend('sat',this.value,true);};";
  html += "valSlider.oninput=function(){document.getElementById('valVal').innerText=this.value;updateGradients();liveSend('val',this.value,false);};";
  html += "valSlider.onchange=function(){liveSend('val',this.value,true);};";
  html += "textHueSlider.oninput=function(){document.getElementById('textHueVal').innerText=this.value;updateTextGradients();liveSend('textHue',this.value,false);};";
  html += "textHueSlider.onchange=function(){liveSend('textHue',this.value,true);};";
  html += "textSatSlider.oninput=function(){document.getElementById('textSatVal').innerText=this.value;updateTextGradients();liveSend('textSat',this.value,false);};";
  html += "textSatSlider.onchange=function(){liveSend('textSat',this.value,true);};";
  html += "textValSlider.oninput=function(){document.getElementById('textValVal').innerText=this.value;updateTextGradients();liveSend('textVal',this.value,false);};";
  html += "textValSlider.onchange=function(){liveSend('textVal',this.value,true);};";
  html += "screenSlider.oninput=function(){document.getElementById('screenVal').innerText=this.value;liveSend('screenBright',this.value,false);};";
  html += "screenSlider.onchange=function(){liveSend('screenBright',this.value,true);};";
  html += "ledSlider.oninput=function(){document.getElementById('ledVal').innerText=this.value;liveSend('ledBright',this.value,false);};";
  html += "ledSlider.onchange=function(){liveSend('ledBright',this.value,true);};";
  html += "updateGradients();updateTextGradients();";
  html += "</script>";

  html += "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
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
// it - the web equivalent of dragging a slider on the touchscreen. Nothing
// here is written to flash except the sync-mode toggle (which the
// touchscreen also persists immediately) - the custom color itself is only
// saved permanently via handleSaveColor()/"Save Color as Default" below,
// matching the touchscreen's separate "SAVE CUSTOM AS DEFAULT" button.
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

// NEW: small nav bar shared by all three web pages, so they feel like one site
String navBar(String current) {
  String html = "<div style='text-align:center;margin-bottom:15px;font-size:0.95em;'>";
  html += "<a href='/' style='margin:0 10px;" + String(current == "settings" ? "font-weight:bold;text-decoration:underline;" : "") + "'>Settings</a>";
  html += "<a href='/dashboard' style='margin:0 10px;" + String(current == "dashboard" ? "font-weight:bold;text-decoration:underline;" : "") + "'>Live Dashboard</a>";
  html += "<a href='/history' style='margin:0 10px;" + String(current == "history" ? "font-weight:bold;text-decoration:underline;" : "") + "'>History</a>";
  html += "<a href='/debug' style='margin:0 10px;" + String(current == "debug" ? "font-weight:bold;text-decoration:underline;" : "") + "'>Debug</a>";
  html += "</div>";
  return html;
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
  json += "\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
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

// NEW: live debug console - shows exactly what the touch controller is
// reporting in real time, plus a scrolling event log, without needing a
// Serial Monitor connection (which the README notes can itself trigger a
// hardware reset on this board). Polls every 400ms.
void handleDebugPage() {
  String html = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><style>";
  html += "body{font-family:'Courier New',monospace;max-width:600px;margin:0 auto;padding:10px;background:#111;color:#0f0;}";
  html += "h1{font-size:1.2em;text-align:center;color:#fff;font-family:sans-serif;}";
  html += ".card{background:#000;border:1px solid #333;border-radius:8px;padding:12px;margin-top:12px;}";
  html += ".row{display:flex;justify-content:space-between;padding:3px 0;border-bottom:1px solid #222;font-size:0.9em;}";
  html += ".hot{color:#ff0;} .bad{color:#f55;} .ok{color:#5f5;}";
  html += "#log{height:300px;overflow-y:auto;font-size:0.85em;white-space:pre-wrap;}";
  html += "</style></head><body>";
  html += "<h1>Weather Station - Debug Console</h1>";
  html += "<div style='text-align:center;font-family:sans-serif;'>" + navBar("debug") + "</div>";

  html += "<div class='card'>";
  html += "<div class='row'><span>Touch pressed (raw)</span><span id='pressed'>--</span></div>";
  html += "<div class='row'><span>Raw X / Y</span><span id='xy'>--</span></div>";
  html += "<div class='row'><span>isTouched (internal)</span><span id='isTouched'>--</span></div>";
  html += "<div class='row'><span>Held for</span><span id='held'>--</span></div>";
  html += "<div class='row'><span>Stuck-touch auto-resets</span><span id='stuck'>--</span></div>";
  html += "<div class='row'><span>Display mode (page)</span><span id='mode'>--</span></div>";
  html += "<div class='row'><span>Uptime</span><span id='uptime'>--</span></div>";
  html += "<div class='row'><span>Free heap</span><span id='heap'>--</span></div>";
  html += "<div class='row'><span>WiFi</span><span id='wifi'>--</span></div>";
  html += "</div>";

  html += "<div class='card'><div id='log'>Loading...</div></div>";

  html += "<script>";
  html += "async function refresh(){";
  html += "try{var r=await fetch('/api/debug');var d=await r.json();";
  html += "var p=document.getElementById('pressed');p.innerText=d.pressed;p.className=d.pressed?'hot':'ok';";
  html += "document.getElementById('xy').innerText=d.rawX+', '+d.rawY;";
  html += "var it=document.getElementById('isTouched');it.innerText=d.isTouched;it.className=d.isTouched?'hot':'ok';";
  html += "document.getElementById('held').innerText=d.isTouched?(d.msHeld+'ms'):'-';";
  html += "var sr=document.getElementById('stuck');sr.innerText=d.stuckResets;sr.className=d.stuckResets>0?'bad':'ok';";
  html += "document.getElementById('mode').innerText=d.displayMode;";
  html += "document.getElementById('uptime').innerText=Math.floor(d.uptimeSec/60)+'m '+(d.uptimeSec%60)+'s';";
  html += "document.getElementById('heap').innerText=(d.freeHeap/1024).toFixed(1)+' KB';";
  html += "var w=document.getElementById('wifi');w.innerText=d.wifiConnected?'connected':'DISCONNECTED';w.className=d.wifiConnected?'ok':'bad';";
  html += "document.getElementById('log').innerText=d.log.join('\\n');";
  html += "}catch(e){document.getElementById('log').innerText='connection lost: '+e;}";
  html += "}";
  html += "refresh();setInterval(refresh,400);";
  html += "</script></body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

void handleApiStatus() {
  time_t localT = now() + tzOffsetSec[currentCity];
  unsigned long ageMin = lastFetchMillis == 0 ? 0 : (millis() - lastFetchMillis) / 60000;
  SunCountdown sc = computeSunCountdown();
  int moonIdx = getMoonPhaseIndex(day(localT), month(localT), year(localT));

  String json = "{";
  json += "\"cityIndex\":" + String(currentCity) + ",";
  json += "\"cityNames\":[\"" + jsonEsc(getDisplayCity(locs[0])) + "\",\"" + jsonEsc(getDisplayCity(locs[1])) + "\",\"" + jsonEsc(getDisplayCity(locs[2])) + "\"],";
  json += "\"city\":\"" + jsonEsc(getDisplayCity(locs[currentCity])) + "\",";
  json += "\"weatherDesc\":\"" + jsonEsc(weatherDesc) + "\",";
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
  json += "\"hour\":" + String(hour(localT)) + ",";
  json += "\"minute\":" + String(minute(localT)) + ",";
  json += "\"dateStr\":\"" + String(day(localT)) + "/" + String(month(localT)) + "/" + String(year(localT)) + "\",";
  json += "\"ledR\":" + String(currentLedR) + ",\"ledG\":" + String(currentLedG) + ",\"ledB\":" + String(currentLedB) + ",";
  json += "\"textR\":" + String(textR) + ",\"textG\":" + String(textG) + ",\"textB\":" + String(textB) + ",";
  json += "\"lastFetchOK\":" + String(lastFetchOK ? "true" : "false") + ",";
  json += "\"ageMin\":" + String(ageMin) + ",";
#if USE_BME280
  json += "\"bmeOK\":" + String(bmeOK ? "true" : "false") + ",";
  json += "\"localTemp\":" + String(localTemp, 1) + ",";
  json += "\"localHumid\":" + String(localHumid, 1) + ",";
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
  int n = min(upcomingRainCount, 3);
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    json += "{\"mm\":" + String(upcomingRainMm[i], 1) + ",\"label\":\"" + jsonEsc(upcomingRainLabel[i]) + "\"}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// NEW: live-mirroring dashboard - polls /api/status every few seconds and
// re-renders, styled to echo the physical device's Home page (digital
// clock, weather card, LED/text color swatches, feels/wind/humid/baro grid,
// mini rain preview) without needing to walk over and look at the screen.
void handleDashboardPage() {
  String html = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><style>";
  html += "body{font-family:sans-serif;text-align:center;max-width:480px;margin:0 auto;padding:10px;background:#eef1f5;}";
  html += "h1{font-size:1.4em;} h3{font-size:1em;color:#4a5a7a;margin:0 0 8px 0;}";
  html += ".card{background:#fff;border-radius:15px;border:2px solid #4a5a7a;padding:20px 10px;margin-top:15px;}";
  html += ".cityRow{display:flex;align-items:center;justify-content:center;gap:14px;}";
  html += ".cityRow button{border:none;background:none;font-size:1.4em;color:#4a5a7a;cursor:pointer;padding:0 8px;}";
  html += ".clock{font-family:'Courier New',monospace;font-weight:bold;font-size:3em;letter-spacing:2px;}";
  html += ".temp{font-size:2.2em;}";
  html += ".grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;text-align:left;margin-top:15px;font-size:0.95em;}";
  html += ".swatchRow{display:flex;justify-content:center;gap:20px;margin-top:12px;}";
  html += ".swatch{width:36px;height:36px;border-radius:8px;border:2px solid #333;display:inline-block;vertical-align:middle;}";
  html += ".dot{width:10px;height:10px;border-radius:50%;display:inline-block;margin-right:5px;}";
  html += "canvas{max-width:100%;margin-top:10px;}";
  html += ".fcRow{display:flex;justify-content:space-between;padding:6px 4px;border-bottom:1px solid #eee;font-size:0.9em;}";
  html += ".fcRow:last-child{border-bottom:none;}";
  html += ".sunBadge{display:inline-block;padding:8px 16px;border-radius:8px;font-weight:bold;margin-top:6px;}";
  html += "</style></head><body>";

  html += "<h1>Weather Station</h1>";
  html += navBar("dashboard");

  html += "<div class='card'>";
  html += "<div class='cityRow'><button onclick='cityNav(-1)'>&#10094;</button>";
  html += "<div id='city' style='font-size:1.3em;font-weight:bold;'>--</div>";
  html += "<button onclick='cityNav(1)'>&#10095;</button></div>";
  html += "<div id='desc' style='color:#4a5a7a;'>--</div>";
  html += "<div class='temp' id='temp'>--</div>";
  html += "<div id='minmax' style='color:#555;'>L:-- H:--</div>";
  html += "<div class='clock' id='clock'>--:--</div>";
  html += "<div id='date' style='color:#555;'>--</div>";
  html += "<div class='grid'>";
  html += "<div>Feels: <span id='feels'>--</span> C</div><div>Wind: <span id='wind'>--</span></div>";
  html += "<div>Humid: <span id='humid'>--</span> %</div><div>Baro: <span id='baro'>--</span> hPa</div>";
  html += "</div>";
  html += "<div id='localRow' class='grid' style='display:none;'>";
  html += "<div>Local Temp: <span id='localTemp'>--</span> C</div><div>Local Humid: <span id='localHumid'>--</span> %</div>";
  html += "</div>";
  html += "<canvas id='rainChart' width='400' height='110'></canvas>";
  html += "<div class='swatchRow'>";
  html += "<div>LED<br><div class='swatch' id='ledSwatch'></div></div>";
  html += "<div>Text<br><div class='swatch' id='textSwatch2'></div></div>";
  html += "</div>";
  html += "<div style='margin-top:12px;font-size:0.85em;color:#777;'><span class='dot' id='statusDot'></span><span id='statusText'>--</span></div>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<h3>SUN &amp; MOON</h3>";
  html += "<div>Sunrise: <b id='sunRise'>--</b> &nbsp;&nbsp; Sunset: <b id='sunSet'>--</b></div>";
  html += "<div class='sunBadge' id='sunBadge'>--</div>";
  html += "<div style='margin-top:8px;color:#555;' id='moonPhase'>--</div>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<h3>5-DAY FORECAST</h3>";
  html += "<div id='forecastList'></div>";
  html += "</div>";

  html += "<script>";
  html += "var ctx=document.getElementById('rainChart').getContext('2d');";
  html += "var curData=null;";
  html += "function cityNav(delta){";
  html += "if(!curData)return;var n=curData.cityIndex+delta;if(n<0)n=curData.cityNames.length-1;if(n>=curData.cityNames.length)n=0;";
  html += "fetch('/live?action=city&value='+n,{headers:{'X-Requested-With':'DesktopDisplay'}}).then(refresh);";
  html += "}";
  html += "function drawRain(rain){";
  html += "ctx.clearRect(0,0,400,110);";
  html += "ctx.font='11px sans-serif';ctx.fillStyle='#4a5a7a';ctx.textAlign='center';ctx.fillText('NEXT RAIN',200,14);";
  html += "if(!rain||rain.length==0){ctx.fillText('No forecast data',200,60);return;}";
  html += "var maxV=0.5;rain.forEach(p=>{if(p.mm>maxV)maxV=p.mm;});maxV*=1.2;";
  html += "var left=60,right=340,base=90,maxH=55;";
  html += "var step=rain.length>1?(right-left)/(rain.length-1):0;";
  html += "var pts=rain.map((p,i)=>({x:rain.length>1?left+i*step:200,y:base-(p.mm/maxV)*maxH,mm:p.mm,label:p.label}));";
  html += "ctx.strokeStyle='#4a5a7a';ctx.beginPath();ctx.moveTo(left-10,base);ctx.lineTo(right+10,base);ctx.stroke();";
  html += "ctx.fillStyle='rgba(90,150,230,0.25)';ctx.beginPath();ctx.moveTo(pts[0].x,base);";
  html += "pts.forEach(p=>ctx.lineTo(p.x,p.y));ctx.lineTo(pts[pts.length-1].x,base);ctx.closePath();ctx.fill();";
  html += "ctx.strokeStyle='#1e63d6';ctx.lineWidth=2;ctx.beginPath();";
  html += "pts.forEach((p,i)=>i==0?ctx.moveTo(p.x,p.y):ctx.lineTo(p.x,p.y));ctx.stroke();";
  html += "pts.forEach(p=>{ctx.fillStyle='#1e63d6';ctx.beginPath();ctx.arc(p.x,p.y,3,0,7);ctx.fill();";
  html += "ctx.fillStyle='#000';ctx.font='10px sans-serif';ctx.fillText(p.mm.toFixed(1),p.x,p.y-8);";
  html += "ctx.fillStyle='#4a5a7a';ctx.fillText(p.label,p.x,base+16);});";
  html += "}";
  html += "async function refresh(){";
  html += "try{var r=await fetch('/api/status');var d=await r.json();curData=d;";
  html += "document.getElementById('city').innerText=d.city;";
  html += "document.getElementById('desc').innerText=d.weatherDesc;";
  html += "document.getElementById('temp').innerText=d.temp.toFixed(1)+'\\u00B0C';";
  html += "document.getElementById('minmax').innerText='L:'+d.tempMin.toFixed(1)+'  H:'+d.tempMax.toFixed(1);";
  html += "document.getElementById('clock').innerText=String(d.hour).padStart(2,'0')+':'+String(d.minute).padStart(2,'0');";
  html += "document.getElementById('clock').style.color='rgb('+d.textR+','+d.textG+','+d.textB+')';";
  html += "document.getElementById('date').innerText=d.dateStr;";
  html += "document.getElementById('feels').innerText=d.feelsLike.toFixed(1);";
  html += "document.getElementById('wind').innerText=d.windSpeed.toFixed(1)+' '+d.windDir;";
  html += "document.getElementById('humid').innerText=d.humid;";
  html += "document.getElementById('baro').innerText=d.pressure;";
  html += "if(d.bmeOK){document.getElementById('localRow').style.display='grid';";
  html += "document.getElementById('localTemp').innerText=d.localTemp.toFixed(1);";
  html += "document.getElementById('localHumid').innerText=d.localHumid.toFixed(1);}";
  html += "document.getElementById('ledSwatch').style.background='rgb('+d.ledR+','+d.ledG+','+d.ledB+')';";
  html += "document.getElementById('textSwatch2').style.background='rgb('+d.textR+','+d.textG+','+d.textB+')';";
  html += "document.getElementById('statusDot').style.background=d.lastFetchOK?'#2e8b57':'#cc3333';";
  html += "document.getElementById('statusText').innerText=d.lastFetchOK?(d.ageMin+'m ago'):'fetch failed';";
  html += "drawRain(d.rain);";
  html += "document.getElementById('sunRise').innerText=d.sunRise;";
  html += "document.getElementById('sunSet').innerText=d.sunSet;";
  html += "var h=Math.floor(d.sunDiffMin/60),m=d.sunDiffMin%60;";
  html += "var badge=document.getElementById('sunBadge');";
  html += "badge.innerText=d.sunEvent+' IN '+h+'H '+m+'M';";
  html += "badge.style.background=d.sunEvent=='SUNRISE'?'#fff4e0':'#e0e7f5';";
  html += "badge.style.color=d.sunEvent=='SUNRISE'?'#e6961e':'#1a2a5e';";
  html += "document.getElementById('moonPhase').innerText='Moon: '+d.moonPhase;";
  html += "var fc=document.getElementById('forecastList');fc.innerHTML='';";
  html += "d.forecast.forEach(f=>{if(!f.day)return;";
  html += "var row=document.createElement('div');row.className='fcRow';";
  html += "row.innerHTML='<b>'+f.day+'</b><span>'+f.main+'</span><span>'+f.low.toFixed(0)+'/'+f.high.toFixed(0)+'\\u00B0C</span>';";
  html += "fc.appendChild(row);});";
  html += "}catch(e){document.getElementById('statusText').innerText='connection lost';}";
  html += "}";
  html += "refresh();setInterval(refresh,7000);";
  html += "</script></body></html>";

  server.send(200, "text/html; charset=utf-8", html);
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

// Scratch space for one fetch (filled by histFetchDaily, used by histMergeDays)
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
  String src = histPath(i);
  if (!SD.exists(src)) return;
  int n = 1;
  String dst;
  do { dst = "/hist_c" + String(i) + "_old" + String(n) + ".csv"; n++; } while (SD.exists(dst));
  SD.rename(src, dst);
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
bool histMergeDays(int i, int n, int overwriteFrom, String dropFrom) {
  if (n <= 0) return true;
  String winStart = String(hfDate[0]), winEnd = String(hfDate[n - 1]);
  for (int k = 0; k < n; k++) { hkPresent[k] = false; hkLine[k] = ""; }

  String path = histPath(i), tmp = "/hist_c" + String(i) + ".tmp";
  if (SD.exists(tmp)) SD.remove(tmp);
  File out = SD.open(tmp, FILE_WRITE);
  if (!out) return false;

  bool flushed = false;
  char buf[100];
  auto flushWindow = [&]() {
    if (flushed) return;
    flushed = true;
    for (int k = 0; k < n; k++) {
      if (hkPresent[k]) out.println(hkLine[k]);
      else if (hfValid[k]) { histFormatRow(k, buf, sizeof(buf)); out.println(buf); }
      hkLine[k] = ""; // free memory
    }
  };

  if (SD.exists(path)) {
    File in = SD.open(path, FILE_READ);
    while (in && in.available()) {
      String line = in.readStringUntil('\n');
      line.trim();
      if (line.length() < 11) continue;
      String d = line.substring(0, 10);
      if (dropFrom.length() == 10 && d >= dropFrom) continue;
      if (d < winStart) { out.println(line); continue; }
      if (d > winEnd) { flushWindow(); out.println(line); continue; }
      int k = histFindDay(d, n);
      if (k < 0) continue;
      if (k >= overwriteFrom && hfValid[k]) continue; // replaced by the fresh value
      hkPresent[k] = true;
      hkLine[k] = histUpgradeLine(line, k);
    }
    if (in) in.close();
  }
  flushWindow();
  out.close();

  if (SD.exists(path)) SD.remove(path);
  return SD.rename(tmp, path);
}

// Fetches an Open-Meteo daily response into hf*. Returns the number of days, or -1.
int histFetchDaily(String url, String who) {
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(url);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    String body = code > 0 ? http.getString() : "";
    http.end();
    logDebug("HIST " + who + ": HTTP " + String(code) + " " + body.substring(0, 90));
    return -1;
  }
  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(32768);
  if (deserializeJson(doc, payload)) { logDebug("HIST " + who + ": bad JSON"); return -1; }
  payload = ""; // free memory before the SD work

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
bool histSyncCity(int i, int pastDays) {
  double lat = 0, lon = 0;
  if (!getCityCoords(i, lat, lon)) { logDebug("HIST city " + String(i + 1) + ": no coordinates"); return false; }

  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4) + "&longitude=" + String(lon, 4)
    + "&daily=" + String(HIST_DAILY_VARS) + "&timezone=auto&past_days=" + String(pastDays) + "&forecast_days=1";
  int n = histFetchDaily(url, "city " + String(i + 1));
  if (n < 2) return false;

  int nPast = n - 1; // last entry is today
  String today = String(hfDate[n - 1]);
  if (!histMergeDays(i, nPast, max(0, nPast - HIST_OVERWRITE_DAYS), today)) {
    logDebug("HIST city " + String(i + 1) + ": SD write failed");
    return false;
  }

  int t = n - 1;
  histTodayValid[i] = hfValid[t];
  histTodayDate[i] = today;
  histTodayMin[i] = hfMin[t]; histTodayMax[i] = hfMax[t]; histTodayRain[i] = hfRain[t];
  histTodayHum[i] = hfHum[t]; histTodayPres[i] = hfPres[t];
  histTodayGust[i] = hfGust[t]; histTodayGustOK[i] = hfGustOK[t];
  histTodaySun[i] = hfSun[t]; histTodaySunOK[i] = hfSunOK[t];
  return true;
}

// One backfill chunk (up to 90 days) from the historical archive, fill-only.
void histBackfillStep(int i) {
  if (bfCursor[i] < bfStart[i]) { histBackfillStop(i, "Done - history goes back to " + bfStart[i]); return; }
  String chunkEnd = bfCursor[i];
  String chunkStart = histAddDays(chunkEnd, -(HIST_BF_CHUNK_DAYS - 1));
  if (chunkStart < bfStart[i]) chunkStart = bfStart[i];

  double lat = 0, lon = 0;
  int n = -1;
  if (getCityCoords(i, lat, lon)) {
    String url = "https://archive-api.open-meteo.com/v1/archive?latitude=" + String(lat, 4) + "&longitude=" + String(lon, 4)
      + "&start_date=" + chunkStart + "&end_date=" + chunkEnd + "&daily=" + String(HIST_DAILY_VARS) + "&timezone=auto";
    n = histFetchDaily(url, "backfill city " + String(i + 1));
  }
  if (n < 1 || !histMergeDays(i, n, n, "")) {
    bfFails[i]++;
    bfMsg[i] = "Request failed - retrying (" + String(bfFails[i]) + "/" + String(HIST_BF_MAX_FAILS) + ")";
    if (bfFails[i] >= HIST_BF_MAX_FAILS) histBackfillStop(i, "Stopped after repeated errors - see the Debug page");
    return;
  }
  bfFails[i] = 0;
  bfCursor[i] = histAddDays(chunkStart, -1);
  bfMsg[i] = "Filled back to " + chunkStart;
  if (bfCursor[i] < bfStart[i]) histBackfillStop(i, "Done - history goes back to " + bfStart[i]);
  else histBackfillSave(i);
}

// Call every loop(). Does at most one network job per call, spaced out, so the
// device never stalls for long. Regular syncs go first, then backfill chunks.
void historyLoop() {
  if (!sdOK) return;

  static unsigned long lastLocCheck = 0;
  if (millis() - lastLocCheck > 2000) { lastLocCheck = millis(); histCheckLocations(); }

  if (WiFi.status() != WL_CONNECTED || isUpdating) return;
  if (histLastAttempt != 0 && millis() - histLastAttempt < HIST_MIN_GAP_MS) return;

  for (int i = 0; i < NUM_CITIES; i++) {
    if (histTrimmedLoc(i).length() == 0) continue;
    if ((long)(millis() - histNextSync[i]) < 0) continue; // not due yet

    bool full = !histFullDone[i] || millis() - histLastFull[i] > HIST_FULL_SYNC_MS;
    histLastAttempt = millis();
    bool ok = histSyncCity(i, full ? HIST_PAST_DAYS : HIST_SHORT_DAYS);
    if (ok && full) { histFullDone[i] = true; histLastFull[i] = millis(); }
    histNextSync[i] = millis() + (ok ? HIST_SYNC_INTERVAL_MS : HIST_RETRY_MS);
    logDebug("HIST city " + String(i + 1) + (full ? " full" : " light") + " sync " + String(ok ? "OK" : "FAILED"));
    if (ok && millis() - histLastStorageRefresh > HIST_STORAGE_REFRESH_MS) histRefreshStorage();
    return;
  }

  if (!histClockOK()) return;
  for (int i = 0; i < NUM_CITIES; i++) {
    if (!bfActive[i] || histTrimmedLoc(i).length() == 0) continue;
    histLastAttempt = millis();
    histBackfillStep(i);
    if (millis() - histLastStorageRefresh > HIST_STORAGE_REFRESH_MS) histRefreshStorage();
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
  if (SD.exists(histPath(i))) SD.remove(histPath(i));
  histTodayValid[i] = false;
  histFullDone[i] = false;
  histNextSync[i] = 0; // re-download the last 92 days right away
}

void histSendJson(bool ok, String msg) {
  server.send(ok ? 200 : 400, "application/json", "{\"ok\":" + String(ok ? "true" : "false") + ",\"msg\":\"" + jsonEsc(msg) + "\"}");
}

// GET /api/history?city=N  (no city = whichever city the device is showing)
// Streamed in chunks, so there's no size limit.
// days: [date,tMin,tMax,rain,humid,pressure,gust|null,sun|null(,1 = today's forecast)]
void handleApiHistory() {
  int city = server.hasArg("city") ? constrain(server.arg("city").toInt(), 0, NUM_CITIES - 1) : currentCity;

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

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  String chunk;
  chunk.reserve(1400);
  chunk = "{\"city\":" + String(city) + ",\"currentCity\":" + String(currentCity) + ",\"names\":[";
  for (int i = 0; i < NUM_CITIES; i++) {
    if (i > 0) chunk += ",";
    chunk += "\"" + jsonEsc(getDisplayCity(locs[i])) + "\"";
  }
  chunk += "],\"sdOK\":" + String(sdOK ? "true" : "false");
  chunk += ",\"cardTotalKB\":" + String(histCardTotalKB) + ",\"cardUsedKB\":" + String(histCardUsedKB);
  chunk += ",\"histKB\":" + String((histBytes + 1023) / 1024);
  chunk += ",\"archives\":" + String(archives);
  chunk += ",\"days\":[";

  bool first = true;
  if (sdOK && SD.exists(histPath(city))) {
    File f = SD.open(histPath(city), FILE_READ);
    if (f) {
      while (f.available()) {
        String line = f.readStringUntil('\n');
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
    chunk += "[\"" + histTodayDate[city] + "\"," + String(histTodayMin[city], 1) + "," + String(histTodayMax[city], 1)
           + "," + String(histTodayRain[city], 1) + "," + String(histTodayHum[city], 0) + "," + String(histTodayPres[city], 0)
           + "," + (histTodayGustOK[city] ? String(histTodayGust[city], 0) : String("null"))
           + "," + (histTodaySunOK[city] ? String(histTodaySun[city], 1) : String("null")) + ",1]";
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
    while (f && f.available()) {
      String line = f.readStringUntil('\n');
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
    char tb[20];
    while (f && f.available()) {
      String line = f.readStringUntil('\n');
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
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");

  String chunk = "date,tMin,tMax,rainMm,humidAvg,pressureAvg,gustMaxKmh,sunHours\n";
  if (sdOK && SD.exists(histPath(city))) {
    File f = SD.open(histPath(city), FILE_READ);
    if (f) {
      while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() < 10) continue;
        chunk += line + "\n";
        if (chunk.length() > 1200) { server.sendContent(chunk); chunk = ""; }
      }
      f.close();
    }
  }
  server.sendContent(chunk);
  server.sendContent("");
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
  if (!SD.exists(HIST_IMPORT_TMP)) { msg = "Upload failed."; return false; }

  uint32_t key;
  float v[7];
  int count = 0;
  File in = SD.open(HIST_IMPORT_TMP, FILE_READ);
  if (!in) { msg = "Upload failed."; return false; }
  while (in.available()) { String line = in.readStringUntil('\n'); if (histParseImportLine(line, key, v)) count++; }
  in.close();

  if (count == 0) { msg = "No valid rows found. Expected columns: date,tMin,tMax,rainMm,humidAvg,pressureAvg,gustMaxKmh,sunHours with dates as YYYY-MM-DD."; return false; }
  if (count > HIST_IMPORT_MAX_ROWS) { msg = "The file has " + String(count) + " rows; the limit per import is " + String(HIST_IMPORT_MAX_ROWS) + ". Split it into smaller files."; return false; }
  if (ESP.getMaxAllocHeap() < (uint32_t)count * sizeof(HistImportRow) + 30000) { msg = "Not enough free memory for a file this size. Split it into smaller files."; return false; }

  std::vector<HistImportRow> rows;
  rows.reserve(count);
  in = SD.open(HIST_IMPORT_TMP, FILE_READ);
  if (!in) { msg = "Upload failed."; return false; }
  uint16_t seq = 0;
  while (in.available() && (int)rows.size() < count) {
    String line = in.readStringUntil('\n');
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
    while (ex && ex.available()) {
      String line = ex.readStringUntil('\n');
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

void drawNavDots(int mode) {
  // Page dots along the bottom - shows which of the 7 pages you're on
  int totalPages = 7, spacing = 16;
  int startXdots = 160 - ((totalPages - 1) * spacing) / 2;
  for (int i = 0; i < totalPages; i++) {
    int cx = startXdots + i * spacing;
    if (i == mode) tft.fillCircle(cx, NAV_DOT_Y, 4, 0x4228);
    else tft.drawCircle(cx, NAV_DOT_Y, 4, 0x4228);
  }
  // City dots under the title - only relevant on the home page (where the
  // arrows live), so we don't clutter the other pages with them.
  if (mode == 0) {
    int cSpacing = 12;
    int cStartX = 160 - ((NUM_CITIES - 1) * cSpacing) / 2;
    for (int i = 0; i < NUM_CITIES; i++) {
      int cx = cStartX + i * cSpacing;
      if (i == currentCity) tft.fillCircle(cx, CITY_DOT_Y, 3, 0x4228);
      else tft.drawCircle(cx, CITY_DOT_Y, 3, 0x4228);
    }
  }
}

void drawPageBackground(int mode) {
  tft.fillScreen(bgColor); tft.setTextDatum(CC_DATUM); tft.setTextColor(TFT_BLACK);
  if (mode == 0) {
    tft.drawString(getDisplayCity(locs[currentCity]), 160, 25, 4);
    // City switch arrows either side of the city name
    tft.setTextColor(0x4228);
    tft.drawString("<", HOME_ARROW_LEFT_X, HOME_ARROW_Y, 4);
    tft.drawString(">", HOME_ARROW_RIGHT_X, HOME_ARROW_Y, 4);
    tft.setTextColor(0x4228); tft.drawString(weatherDesc, 160, 60, 2);
    tft.setTextColor(TFT_BLACK);
    String tNum = String(tempnum, 1); 
    tft.drawString(tNum, 160, 110, 7);
    int nW = tft.textWidth(tNum, 7); tft.setTextDatum(TL_DATUM);
    tft.drawString("o", 160 + (nW/2) + 2, 85, 2);
    tft.drawString("C", 160 + (nW/2) + 12, 95, 4);
    tft.setTextDatum(CC_DATUM);
    tft.drawString("L:" + String(tempMin, 1) + "  H:" + String(tempMax, 1), 160, 145, 2);
    tft.fillRoundRect(15, 165, 290, 305, 15, TFT_WHITE);
    tft.drawRoundRect(15, 165, 290, 305, 15, 0x4228);
  } else {
    tft.fillRoundRect(15, 70, 290, 400, 15, TFT_WHITE);
    tft.drawRoundRect(15, 70, 290, 400, 15, 0x4228);
    String titles[] = {"", "ATMOSPHERIC LOG", "PRECIPITATION", "SOLAR & LUNAR", "SYSTEM INFO", "5-DAY FORECAST", "7-DAY HISTORY"};
    tft.drawString(titles[mode], 160, 35, 4);
  }
  drawNavDots(mode); // NEW: page + city position indicators
}

// NEW: small freshness/status dot + age label, drawn inside the home card.
// Green = last fetch succeeded, Red = last fetch failed (stale data on screen).
void drawStatusIndicator(bool forceDraw = false) {
  // Now that the dot lives in the spacious bottom-right corner of the card
  // (nothing else is drawn near there), there's room for the "Xm ago" text
  // again without it fighting the clock digits like it did up top.
  static int lastShownAge = -1;
  static bool lastShownOK = true;
  unsigned long ageMin = lastFetchMillis == 0 ? 0 : (millis() - lastFetchMillis) / 60000;
  if (!forceDraw && (int)ageMin == lastShownAge && lastFetchOK == lastShownOK && drawnMode == 0) return;
  lastShownAge = (int)ageMin; lastShownOK = lastFetchOK;

  uint16_t dotColor = lastFetchOK ? TFT_DARKGREEN : TFT_RED;
  tft.fillRect(STATUS_DOT_X - 60, STATUS_DOT_Y - 8, 70, 16, TFT_WHITE); // erase previous dot+text
  tft.fillCircle(STATUS_DOT_X, STATUS_DOT_Y, STATUS_DOT_R, dotColor);
  tft.setTextColor(TFT_BLACK, TFT_WHITE); tft.setTextDatum(MR_DATUM);
  String label = lastFetchOK ? (String(ageMin) + "m") : "ERR";
  tft.drawString(label, STATUS_DOT_X - STATUS_DOT_R - 4, STATUS_DOT_Y, 1);
}

// NEW: compact "upcoming rain" preview for the bottom of the Home card,
// using the exact same forecast data the full Precipitation page uses (see
// upcomingRainMm/upcomingRainLabel). A mini line graph - same visual style
// as the Precipitation page's graph, just smaller - rather than bars, and
// shows the next 3 x 3-hour steps (~9h ahead) rather than the full ~15h
// graph, since there's only a little unused space here. Text is drawn
// transparent (single-arg setTextColor), not opaque - opaque text pre-fills
// a white background box, which was overlapping the "NEXT RAIN" title
// above when a bar/point sat close to it (the same issue fixed on the
// Precipitation page's own graph). Shares its screen area with the home
// animation (see drawPageHome) - only one of the two is ever drawn.
// Returns true if this call actually wiped and redrew its area (which also
// erases the status dot, since it lives inside that area's footprint) -
// false if it skipped drawing because nothing changed. drawPageHome() uses
// this to know when the status dot needs to be force-redrawn right after.
bool drawHomeRainPreview() {
  static float lastVals[3] = { -1, -1, -1 };
  int n = min(upcomingRainCount, 3);
  bool changed = (drawnMode != 0);
  for (int i = 0; i < 3; i++) {
    float v = (i < n) ? upcomingRainMm[i] : -1;
    if (v != lastVals[i]) changed = true;
    lastVals[i] = v;
  }
  if (!changed) return false; // nothing changed since last draw

  int areaX = 25, areaY = 386, areaW = 270, areaH = 78;
  tft.fillRect(areaX, areaY, areaW, areaH, TFT_WHITE); // clear first - label widths vary between redraws
  tft.setTextDatum(CC_DATUM);
  tft.setTextColor(0x4228, TFT_WHITE);
  tft.drawString("NEXT RAIN", 160, areaY + 9, 1);

  if (n == 0) {
    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.drawString("NO FORECAST DATA", 160, areaY + 45, 2);
    return true;
  }

  int graphLeft = 70, graphRight = 250, baseline = areaY + areaH - 14, maxBarHeight = 34;
  float maxRain = 0.5; // floor so an all-dry forecast doesn't divide by ~0
  for (int i = 0; i < n; i++) if (upcomingRainMm[i] > maxRain) maxRain = upcomingRainMm[i];
  maxRain *= 1.2; // headroom so the highest point stays clear of the title above

  int stepX = (n > 1) ? (graphRight - graphLeft) / (n - 1) : 0;
  int px[3], py[3];
  for (int i = 0; i < n; i++) {
    px[i] = (n > 1) ? (graphLeft + i * stepX) : 160;
    int h = (int)((upcomingRainMm[i] / maxRain) * maxBarHeight);
    py[i] = baseline - h;
  }

  tft.drawFastHLine(graphLeft - 10, baseline, (graphRight - graphLeft) + 20, 0x4228); // baseline axis

  uint16_t fillColor = tft.color565(195, 222, 250); // soft blue fill under the line
  for (int i = 0; i < n - 1; i++) {
    tft.fillTriangle(px[i], baseline, px[i], py[i], px[i+1], py[i+1], fillColor);
    tft.fillTriangle(px[i], baseline, px[i+1], py[i+1], px[i+1], baseline, fillColor);
  }
  for (int i = 0; i < n - 1; i++) {
    tft.drawLine(px[i], py[i], px[i+1], py[i+1], TFT_BLUE);
  }
  for (int i = 0; i < n; i++) {
    tft.fillCircle(px[i], py[i], 2, TFT_BLUE);
    // Transparent text - see comment above the function
    tft.setTextColor(TFT_BLACK); tft.setTextDatum(CC_DATUM);
    tft.drawString(String(upcomingRainMm[i], 1), px[i], py[i]-10, 1);
    tft.setTextColor(0x4228);
    tft.drawString(upcomingRainLabel[i], px[i], baseline+10, 1);
  }
  return true;
}

void drawPageHome() {
  static unsigned long lastGridUpdate = 0;
  if (millis() - lastGridUpdate > 60000 || drawnMode != 0) {
    clk.createSprite(270, 80); clk.fillSprite(TFT_WHITE); clk.setTextDatum(ML_DATUM); clk.setTextColor(TFT_BLACK);
    clk.drawFastHLine(0, 0, 270, 0xD6BA);
    clk.drawString("Feels: " + String(feels_like, 1) + " C", 10, 25, 2);
    clk.drawString("Humid: " + String(huminum) + " %", 10, 55, 2);
    clk.drawString("Wind: " + String(windSpeedKmH, 1) + " " + windDir, 140, 25, 2);
    clk.drawString("Baro: " + String(pressure) + " hPa", 140, 55, 2);
    clk.pushSprite(25, 300); clk.deleteSprite();
    lastGridUpdate = millis();
  }
  if (now() != prevDisplay || drawnMode != 0) {
    time_t localT = now() + tzOffsetSec[currentCity];
    // NEW: Font 7 (TFT_eSPI's built-in pseudo-7-segment digit font) replaces
    // the old per-digit JPEG images - it's already linked into the binary
    // since the temperature display uses it too, so this adds no extra font
    // flash cost. Hour stays black; minute now tracks the LED's actual live
    // color (custom-picked or weather-synced, whichever is active).
    String hourStr = (hour(localT) < 10 ? "0" : "") + String(hour(localT));
    String minStr  = (minute(localT) < 10 ? "0" : "") + String(minute(localT));
    int ty = 185;
    int blockW = tft.textWidth("88", 7); // actual rendered width of a 2-digit Font 7 block
    int gap = 30; // visual gap between the hour block and the minute block
    int totalW = blockW * 2 + gap;
    int tx = 160 - totalW / 2; // center on x=160, matching city/temp/date above and below it

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.drawString(hourStr, tx, ty, 7);
    tft.setTextColor(safeTextColor(textR, textG, textB), TFT_WHITE);
    tft.drawString(minStr, tx + blockW + gap, ty, 7);

    // NEW: colon separator, centered in the gap between the two blocks -
    // previously relied on color alone to tell hour/minute apart
    tft.setTextDatum(CC_DATUM);
    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.drawString(":", tx + blockW + gap/2, ty + 24, 7);
    tft.setTextDatum(TL_DATUM);

    clk.createSprite(250, 40); clk.fillSprite(TFT_WHITE); clk.setTextDatum(CC_DATUM); clk.setTextColor(TFT_BLACK);
    String d = String(day(localT)) + " / " + String(month(localT)) + " / " + String(year(localT));
    clk.drawString(d, 125, 20, 4); clk.pushSprite(35, 250); clk.deleteSprite();
    prevDisplay = now();
  }
  bool rainAreaWiped = false;
  if (showRainPreview) {
    rainAreaWiped = drawHomeRainPreview();
  } else if (showAnimation) {
    int ax = 135, ay = 385;
    if (millis() - AprevTime > 100) {
      Anim++; if(Anim == 10) Anim = 0;
      AprevTime = millis();
      // NEW: array lookup instead of a 10-case switch
      TJpgDec.drawJpg(ax, ay, animFrames[Anim], animSizes[Anim]);
    }
  }

  drawStatusIndicator(rainAreaWiped); // NEW: drawn last, and forced whenever the rain preview above just wiped this shared area
}

void drawPageStats() { 
  if (drawnMode != 1) { 
    int sY = 100;
    tft.setTextDatum(TL_DATUM); 
    String labels[8] = {"Current City", "Temperature", "Feels Like", "Humidity", "Pressure", "Visibility", "", ""};
    String vals[8]   = {getDisplayCity(locs[currentCity]), String(tempnum, 1)+" C", String(feels_like, 1)+" C",
                         String(huminum)+" %", String(pressure)+" hPa", String(visibility)+" km", "", ""};
    int rowCount = 6;
#if USE_BME280
    // NEW: append local sensor readings if a BME280 was actually detected
    if (bmeOK) {
      labels[6] = "Local Temp";     vals[6] = String(localTemp, 1) + " C";
      labels[7] = "Local Humidity"; vals[7] = String(localHumid, 1) + " %";
      rowCount = 8;
    }
#endif
    int step = 380 / rowCount;
    for(int i=0; i<rowCount; i++) { 
      tft.setTextColor(0x4228, TFT_WHITE); tft.drawString(labels[i], 40, sY + (i*step), 2); 
      tft.setTextColor(TFT_BLACK, TFT_WHITE);
      tft.drawString(vals[i], 40, sY + (i*step) + 20, 4); 
    } 
  } 
}

void drawPageRain() { 
  if (drawnMode != 2) { 
    tft.setTextDatum(CC_DATUM);
    // NOTE: shrunk from font 6 to font 4 (both are fonts this project already
    // uses elsewhere) to free up vertical room for the graph below.
    tft.setTextColor(0x4228, TFT_WHITE); tft.drawString("1H RAINFALL", 160, 95, 2); 
    tft.setTextColor(TFT_BLACK, TFT_WHITE); tft.drawString(String(rain1h, 2) + " mm", 160, 120, 4); 
    tft.setTextColor(0x4228, TFT_WHITE);
    tft.drawString("3H RAINFALL", 160, 160, 2); 
    tft.setTextColor(TFT_BLACK, TFT_WHITE); tft.drawString(String(rain3h, 2) + " mm", 160, 185, 4); 

    // NEW: bar graph of the next few 3-hour forecast slices, so you can see
    // *when* rain is expected rather than just the current single number.
    // (The API's forecast data only comes in 3-hour steps, so this is the
    // finest resolution available - each bar IS a 3-hour window.)
    tft.setTextColor(0x4228, TFT_WHITE);
    tft.drawString("UPCOMING RAIN (3H STEPS)", 160, 225, 2);

    int graphLeft = 55, graphRight = 265, baseline = 420, maxBarHeight = 130;
    int n = upcomingRainCount;
    if (n == 0) {
      tft.setTextColor(TFT_BLACK, TFT_WHITE);
      tft.drawString("NO FORECAST DATA", 160, 330, 2);
    } else {
      float maxRain = 0.5; // floor so an all-dry forecast doesn't divide by ~0
      for (int i = 0; i < n; i++) if (upcomingRainMm[i] > maxRain) maxRain = upcomingRainMm[i];
      maxRain *= 1.15; // headroom so the highest point doesn't touch its label

      int stepX = (n > 1) ? (graphRight - graphLeft) / (n - 1) : 0;
      int px[RAIN_GRAPH_POINTS], py[RAIN_GRAPH_POINTS];
      for (int i = 0; i < n; i++) {
        px[i] = graphLeft + i * stepX;
        int h = (int)((upcomingRainMm[i] / maxRain) * maxBarHeight);
        py[i] = baseline - h;
      }

      tft.drawFastHLine(graphLeft - 10, baseline, (graphRight - graphLeft) + 20, 0x4228); // baseline axis

      uint16_t fillColor = tft.color565(195, 222, 250); // soft blue fill under the line
      for (int i = 0; i < n - 1; i++) {
        tft.fillTriangle(px[i], baseline, px[i], py[i], px[i+1], py[i+1], fillColor);
        tft.fillTriangle(px[i], baseline, px[i+1], py[i+1], px[i+1], baseline, fillColor);
      }
      for (int i = 0; i < n - 1; i++) {
        tft.drawLine(px[i], py[i], px[i+1], py[i+1], TFT_BLUE);
        tft.drawLine(px[i], py[i]-1, px[i+1], py[i+1]-1, TFT_BLUE); // thicken slightly
      }
      for (int i = 0; i < n; i++) {
        tft.fillCircle(px[i], py[i], 3, TFT_BLUE);
        // Transparent text (single-arg setTextColor) instead of opaque -
        // opaque text pre-fills a background box, which was carving a
        // visible white notch out of the rising line/fill whenever a
        // neighboring point's line passed under a label. Transparent just
        // draws the digit pixels on top of whatever's already there.
        tft.setTextColor(TFT_BLACK); tft.setTextDatum(CC_DATUM);
        tft.drawString(String(upcomingRainMm[i], 1), px[i], py[i]-14, 1);
        tft.setTextColor(0x4228);
        tft.drawString(upcomingRainLabel[i], px[i], baseline+14, 1);
      }
    }
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

// NEW: sunrise/sunset countdown badge - unlike the rest of the Astro page,
// this ticks live every loop() pass while you're sitting on the page, not
// just once when you switch to it.
void drawAstroCountdown() {
  static String lastLabel = "";
  SunCountdown c = computeSunCountdown();
  if (c.eventName == "") return; // no data yet

  uint16_t badgeColor, badgeBg;
  if (c.eventName == "SUNRISE") { badgeColor = tft.color565(230, 150, 30); badgeBg = tft.color565(255, 244, 224); }
  else { badgeColor = TFT_NAVY; badgeBg = tft.color565(224, 231, 245); }

  String label = c.eventName + " IN " + String(c.diffMin / 60) + "H " + String(c.diffMin % 60) + "M";
  if (label == lastLabel && drawnMode == 3) return; // nothing changed since last draw
  lastLabel = label;

  int badgeW = 230, badgeH = 30, badgeCY = 460 - 20; // 460 = the moon card's actual bottom edge (moonCardY+moonCardH in drawPageAstro); leaves a clean margin instead of poking through the border
  int badgeX = 160 - badgeW / 2;
  tft.fillRoundRect(badgeX, badgeCY - badgeH / 2, badgeW, badgeH, 8, badgeBg);
  tft.drawRoundRect(badgeX, badgeCY - badgeH / 2, badgeW, badgeH, 8, badgeColor);
  tft.setTextDatum(CC_DATUM); tft.setTextColor(badgeColor, badgeBg);
  tft.drawString(label, 160, badgeCY, 2);
}

void drawPageAstro() {
  if (drawnMode != 3) {
    // NEW: full redesign. Sunrise/Sunset get their own tinted mini-cards
    // side by side (with up/down arrows) instead of floating text over
    // mostly-empty space, and the moon phase gets a dedicated card below
    // rather than sitting directly on the page background. Flat TFT_YELLOW
    // read as garish on white, so sunrise uses a richer warm gold instead.
    int cardY = 88, cardW = 130, cardH = 95;
    int sunriseX = 25, sunsetX = 165;
    uint16_t sunriseColor = tft.color565(230, 150, 30);  // warm gold, not flat yellow
    uint16_t sunsetColor  = TFT_NAVY;
    uint16_t sunriseBg = tft.color565(255, 244, 224);    // soft amber tint
    uint16_t sunsetBg  = tft.color565(224, 231, 245);    // soft blue tint

    // Sunrise card
    tft.fillRoundRect(sunriseX, cardY, cardW, cardH, 10, sunriseBg);
    tft.drawRoundRect(sunriseX, cardY, cardW, cardH, 10, sunriseColor);
    int scx = sunriseX + cardW/2;
    tft.fillTriangle(scx-8, cardY+35, scx+8, cardY+35, scx, cardY+18, sunriseColor); // up arrow
    tft.setTextDatum(CC_DATUM); tft.setTextColor(0x4228, sunriseBg);
    tft.drawString("SUNRISE", scx, cardY+50, 2);
    tft.setTextColor(sunriseColor, sunriseBg);
    tft.drawString(sunRise, scx, cardY+76, 4);

    // Sunset card
    tft.fillRoundRect(sunsetX, cardY, cardW, cardH, 10, sunsetBg);
    tft.drawRoundRect(sunsetX, cardY, cardW, cardH, 10, sunsetColor);
    int stx = sunsetX + cardW/2;
    tft.fillTriangle(stx-8, cardY+18, stx+8, cardY+18, stx, cardY+35, sunsetColor); // down arrow
    tft.setTextColor(0x4228, sunsetBg);
    tft.drawString("SUNSET", stx, cardY+50, 2);
    tft.setTextColor(sunsetColor, sunsetBg);
    tft.drawString(sunSet, stx, cardY+76, 4);

    // Moon phase - its own card below, rather than floating on bare background
    uint16_t moonBg = tft.color565(245, 246, 250);
    int moonCardY = cardY + cardH + 15;
    int moonCardH = 470 - moonCardY - 10;
    tft.fillRoundRect(25, moonCardY, 270, moonCardH, 10, moonBg);
    tft.drawRoundRect(25, moonCardY, 270, moonCardH, 10, 0x4228);

    time_t localT = now() + tzOffsetSec[currentCity];
    int pIdx = getMoonPhaseIndex(day(localT), month(localT), year(localT));

    uint16_t jw = 0, jh = 0;
    TJpgDec.getJpgSize(&jw, &jh, moonImages[pIdx], moonSizes[pIdx]);
    int imgW = jw > 0 ? jw : 100, imgH = jh > 0 ? jh : 100;
    int imgX = 160 - (imgW/2);
    int imgY = moonCardY + 45; // roughly centers image+rings+name within the card

    TJpgDec.drawJpg(imgX, imgY, moonImages[pIdx], moonSizes[pIdx]);
    tft.drawRoundRect(imgX - 1, imgY - 1, imgW + 2, imgH + 2, 10, 0x4228);
    for(int i=0; i<6; i++) tft.drawRoundRect(imgX - 3 - i, imgY - 3 - i, imgW + 6 + (i*2), imgH + 6 + (i*2), 12 + i, moonBg);
    tft.setTextColor(TFT_BLACK, moonBg);
    tft.drawString(getMoonPhaseName(pIdx), 160, imgY + imgH + 25, 4);
  } 
  drawAstroCountdown(); // NEW: ticks live even while already on this page
}

void drawPageForecast() { 
  if (drawnMode != 5) { 
    int sY = 95, step = 72;
    for(int i=0; i<5; i++) { 
      int yPos = sY + (i*step); tft.fillRoundRect(30, yPos, 260, 60, 10, 0xF79E);
      tft.drawRoundRect(30, yPos, 260, 60, 10, 0x4228); 
      if (forecast[i].valid) { 
        tft.setTextColor(0x4228); tft.setTextDatum(ML_DATUM); tft.drawString(forecast[i].dayName, 45, yPos + 30, 4); 
        tft.setTextColor(TFT_BLACK); tft.setTextDatum(CC_DATUM);
        tft.drawString(forecast[i].main, 145, yPos + 30, 2); tft.setTextDatum(MR_DATUM); 
        tft.drawString(String((int)round(forecast[i].low)) + "/" + String((int)round(forecast[i].high)) + " C", 275, yPos + 30, 4);
      } else { 
        tft.setTextColor(TFT_BLACK); tft.setTextDatum(CC_DATUM); tft.drawString("NO DATA", 160, yPos + 30, 2); 
      } 
    } 
  } 
}

// NEW: on-device 7-day history for the current city (page 7) - highs/lows
// as two lines and daily rain as bars, read from the city's SD history log.
// Drawn once when you switch to the page (the data only changes a few times a day).
void drawPageHistory() {
  if (drawnMode == 6) return;
  tft.setTextDatum(CC_DATUM);
  tft.setTextColor(0x4228, TFT_WHITE);
  tft.drawString(getDisplayCity(locs[currentCity]), 160, 86, 2);

  String dates[7]; float hi[7], lo[7], rn[7];
  int n = 0;
#if USE_SD_HISTORY
  n = histReadLastDays(currentCity, dates, hi, lo, rn, 7);
#endif
  if (n == 0) {
    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.drawString("NO HISTORY YET", 160, 250, 2);
    tft.drawString("(NEEDS THE SD CARD)", 160, 275, 1);
    return;
  }

  uint16_t hiCol = tft.color565(204, 51, 51), loCol = tft.color565(30, 99, 214);
  uint16_t rainCol = tft.color565(74, 144, 217), gridCol = tft.color565(225, 225, 225);
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

  tft.setTextColor(0x4228, TFT_WHITE);
  tft.drawString("HIGH / LOW (C)", 160, 106, 1);
  for (int g = 0; g < 3; g++) tft.drawFastHLine(30, top + g * (bot - top) / 2, 260, gridCol);
  for (int i = 0; i < n - 1; i++) {
    tft.drawLine(px[i], ty(hi[i]), px[i + 1], ty(hi[i + 1]), hiCol);
    tft.drawLine(px[i], ty(hi[i]) - 1, px[i + 1], ty(hi[i + 1]) - 1, hiCol);
    tft.drawLine(px[i], ty(lo[i]), px[i + 1], ty(lo[i + 1]), loCol);
    tft.drawLine(px[i], ty(lo[i]) + 1, px[i + 1], ty(lo[i + 1]) + 1, loCol);
  }
  for (int i = 0; i < n; i++) {
    tft.fillCircle(px[i], ty(hi[i]), 3, hiCol);
    tft.fillCircle(px[i], ty(lo[i]), 3, loCol);
    tft.setTextColor(hiCol); // transparent text, so labels never punch holes in the lines
    tft.drawString(String((int)round(hi[i])), px[i], ty(hi[i]) - 11, 2);
    tft.setTextColor(loCol);
    tft.drawString(String((int)round(lo[i])), px[i], ty(lo[i]) + 12, 2);
    tft.setTextColor(0x4228, TFT_WHITE);
    tft.drawString(getDayName(dayOfWeekFromDateString(dates[i])), px[i], 270, 2);
  }

  // --- rain bars
  tft.setTextColor(0x4228, TFT_WHITE);
  tft.drawString("RAIN (MM)", 160, 296, 1);
  int rTop = 318, rBase = 405;
  float rMax = 2.0f, tot = 0, hiSum = 0;
  for (int i = 0; i < n; i++) { if (rn[i] > rMax) rMax = rn[i]; tot += rn[i]; hiSum += hi[i]; }
  tft.drawFastHLine(30, rBase, 260, 0x4228);
  for (int i = 0; i < n; i++) {
    int h = (int)(rn[i] / rMax * (rBase - rTop));
    if (h > 0) tft.fillRect(px[i] - 10, rBase - h, 20, h, rainCol);
    if (rn[i] >= 0.05f) {
      tft.setTextColor(TFT_BLACK);
      tft.drawString(String(rn[i], 1), px[i], rBase - h - 8, 1);
    }
    tft.setTextColor(0x4228, TFT_WHITE);
    tft.drawString(dates[i].substring(8, 10), px[i], rBase + 10, 1);
  }

  // --- totals
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.drawString(String(n) + "-DAY RAIN " + String(tot, 1) + " MM   AVG HIGH " + String(hiSum / n, 1) + " C", 160, 440, 2);
}

// NEW: the WiFi/sync/key status block, split out so it can be refreshed on
// its own while sitting on this page. Only actually redraws (and wipes the
// region) when something has genuinely changed - SSID, IP, sync age in
// minutes, sync success/failure, or which API key is active - rather than
// on a fixed timer, so the text doesn't visibly flash every couple seconds
// when nothing new has happened. forceDraw=true is used on first entering
// the page, since the screen was just wiped and needs the info regardless
// of whether it happens to match the last-cached values from before.
void drawSystemDynamicInfo(bool forceDraw = false) {
  static String lastSSID = "";
  static String lastIP = "";
  static int lastAgeMin = -1;
  static bool lastOK = false;
  static String lastKeySource = "";
  static int lastTextR = -1, lastTextG = -1, lastTextB = -1; // NEW: so the IP-text color preview updates live
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

  // Explicit clear first: the opaque text below only fills the background
  // behind its OWN new width, so if e.g. the IP address or SSID gets
  // shorter on refresh, leftover characters from the old, longer string
  // would otherwise stay on screen. Clearing the whole region up front
  // avoids that ghosting regardless of how the text length changes.
  tft.fillRect(20, 83, 280, 122, TFT_WHITE);

  tft.setTextDatum(CC_DATUM);
  tft.setTextColor(0x4228, TFT_WHITE); tft.drawString("WIFI: " + ssid, 160, 95, 2); 
  // NEW: the IP address itself doubles as the color preview - colored with
  // the LED's actual live color (custom-picked or weather-synced) rather
  // than a separate swatch, using the full width already available here
  // instead of adding a small dot that risked visual clutter.
  tft.setTextColor(safeTextColor(textR, textG, textB), TFT_WHITE);
  tft.drawString(ip, 160, 120, 4); 

  tft.setTextColor(0x4228, TFT_WHITE); tft.drawString("LAST SYNC", 160, 155, 2);
  tft.setTextColor(lastFetchOK ? TFT_DARKGREEN : TFT_RED, TFT_WHITE);
  tft.drawString(lastFetchOK ? (String(ageMin) + " MIN AGO") : "FAILED", 160, 175, 2);

  if (apiKeyBackup.length() > 0) {
    tft.setTextColor(0x4228, TFT_WHITE);
    tft.drawString("API KEY: " + keySource, 160, 196, 1);
  }
}

// NEW: generic slider track+fill+thumb drawer, reused for screen and LED brightness
void drawSlider(int cy, int percent, uint16_t fillColor, int trackX = SLIDER_X, int trackW = SLIDER_W, int thumbR = SLIDER_THUMB_R) {
  percent = constrain(percent, 0, 100);
  // Clear the FULL thumb height first, not just the thin track. The thumb
  // is taller than the track, so previous thumb positions were leaving
  // visible arcs above/below the track uncleared - that's what showed up
  // as a jagged "trail" during a fast drag.
  int clearMargin = thumbR + 2;
  tft.fillRect(trackX - clearMargin, cy - clearMargin, trackW + 2 * clearMargin, 2 * clearMargin, TFT_WHITE);

  int trackY = cy - SLIDER_TRACK_H / 2;
  tft.fillRoundRect(trackX, trackY, trackW, SLIDER_TRACK_H, SLIDER_TRACK_H / 2, 0xC618); // track background
  int filledW = map(percent, 0, 100, 0, trackW);
  if (filledW > 0) tft.fillRoundRect(trackX, trackY, filledW, SLIDER_TRACK_H, SLIDER_TRACK_H / 2, fillColor);
  int thumbX = trackX + filledW;
  tft.fillCircle(thumbX, cy, thumbR, TFT_WHITE);
  tft.drawCircle(thumbX, cy, thumbR, fillColor);
  tft.drawCircle(thumbX, cy, thumbR - 1, fillColor);
}

// forceDraw=true is used on page entry (page was just wiped); otherwise
// this only actually redraws when the visual PERCENT changes, not on every
// tiny raw brightness change - the brightness range is finer than the
// slider's 220px width can show anyway, so redrawing for a change that
// wouldn't move the thumb by even a pixel was just adding needless flicker.
void drawScreenBrightnessSlider(bool forceDraw = false) {
  static int lastPct = -1;
  int pct = map(globalBrightness, 10, 255, 0, 100);
  if (!forceDraw && pct == lastPct) return;
  lastPct = pct;
  tft.fillRect(20, SCREEN_LABEL_Y - 12, 280, 20, TFT_WHITE); // clear label row (percent text width varies)
  tft.setTextDatum(CC_DATUM); tft.setTextColor(0x4228, TFT_WHITE);
  tft.drawString("SCREEN BRIGHTNESS " + String(pct) + "%", 160, SCREEN_LABEL_Y, 2);
  drawSlider(SCREEN_SLIDER_Y, pct, 0x4228);
}

void drawLedBrightnessSlider(bool forceDraw = false) {
  static int lastPct = -1;
  int pct = map(ledBrightness, 0, 255, 0, 100);
  if (!forceDraw && pct == lastPct) return;
  lastPct = pct;
  tft.fillRect(20, LED_LABEL_Y - 12, 280, 20, TFT_WHITE);
  tft.setTextDatum(CC_DATUM); tft.setTextColor(0x4228, TFT_WHITE);
  tft.drawString("LED BRIGHTNESS " + String(pct) + "%", 160, LED_LABEL_Y, 2);
  drawSlider(LED_SLIDER_Y, pct, 0x4228);
}

void drawModeToggleButton() {
  tft.fillRoundRect(50, MODE_BTN_Y, 220, MODE_BTN_H, 8, useCustomColor ? 0x051D : 0x4228);
  tft.setTextColor(TFT_WHITE); tft.setTextDatum(CC_DATUM);
  tft.drawString(useCustomColor ? "MODE: CUSTOM COLOR" : "MODE: WEATHER SYNC", 160, MODE_BTN_Y + MODE_BTN_H/2, 2);
}

// NEW: single hue-picker bar replaces the old 3-slider R/G/B approach - one
// continuous rainbow strip you drag along, using the full available width.
// No separate label or preview swatch - the gradient itself makes clear
// what it does, and the IP-text color preview (drawSystemDynamicInfo)
// already shows the live result.
void drawHuePicker(bool forceDraw = false) {
  static int lastHue = -1;

  if (!useCustomColor) {
    if (lastHue != -1) {
      tft.fillRect(20, HUE_BAR_Y - 16, 280, HUE_BAR_H + 32, TFT_WHITE);
      lastHue = -1;
    }
    return;
  }

  if (!forceDraw && customHue == lastHue) return; // nothing changed, skip redraw
  lastHue = customHue;

  // Clear just the bar's own footprint (plus thumb overhang) before redrawing
  tft.fillRect(HUE_BAR_X - 6, HUE_BAR_Y - 6, HUE_BAR_W + 12, HUE_BAR_H + 12, TFT_WHITE);

  // Redraw the gradient (250 vertical lines - cheap on SPI) plus border
  for (int i = 0; i < HUE_BAR_W; i++) {
    int r, g, b;
    HSBtoRGB((i * 360) / HUE_BAR_W, 255, 255, r, g, b);
    tft.drawFastVLine(HUE_BAR_X + i, HUE_BAR_Y, HUE_BAR_H, tft.color565(r, g, b));
  }
  tft.drawRect(HUE_BAR_X - 1, HUE_BAR_Y - 1, HUE_BAR_W + 2, HUE_BAR_H + 2, TFT_BLACK);

  // Thumb marker at the current hue's position
  int thumbX = HUE_BAR_X + (customHue * HUE_BAR_W) / 360;
  tft.drawRect(thumbX - 4, HUE_BAR_Y - 4, 8, HUE_BAR_H + 8, TFT_WHITE);
  tft.drawRect(thumbX - 5, HUE_BAR_Y - 5, 10, HUE_BAR_H + 10, TFT_BLACK);

  drawSystemDynamicInfo(); // NEW: refreshes the IP-text color preview live while dragging (change-detection catches the LED color diff)
}

void drawSaveButton(bool confirmed) {
  tft.fillRoundRect(50, SAVE_BTN_Y, 220, SAVE_BTN_H, 8, confirmed ? TFT_BLUE : 0x4228);
  tft.setTextColor(TFT_WHITE); tft.setTextDatum(CC_DATUM);
  tft.drawString(confirmed ? "SAVED!" : "SAVE CUSTOM AS DEFAULT", 160, SAVE_BTN_Y + SAVE_BTN_H/2, 2);
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
  updateBacklight(); 

  // NEW: try reconnecting with saved WiFi credentials first, with a few retries,
  // before dropping into the AP config portal. Avoids a brief router hiccup
  // forcing you to redo WiFi setup on a wall-mounted device.
  tft.fillScreen(bgColor); tft.setTextColor(TFT_BLACK); tft.setTextDatum(CC_DATUM);
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
  historySetup(); // NEW: per-city history files (migrates the old /history.csv to City 1 on first boot)
#endif

  // The WebServer only keeps request headers it's told about up front
  const char* collectedHeaders[] = { APP_HEADER_NAME };
  server.collectHeaders(collectedHeaders, 1);
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/live", handleLive);           // NEW: live slider updates (color, brightness, mode)
  server.on("/saveColor", handleSaveColor); // NEW: persists the current custom color as the boot default
  server.on("/saveTextColor", handleSaveTextColor); // NEW: persists the current text color as the boot default
  server.on("/dashboard", handleDashboardPage); // NEW: live-mirroring dashboard
  server.on("/api/status", handleApiStatus);    // NEW: JSON feed for the dashboard
  server.on("/debug", handleDebugPage);         // NEW: live touch/system debug console
  server.on("/api/debug", handleApiDebug);      // NEW: JSON feed for the debug console
#if USE_SD_HISTORY
  server.on("/history", handleHistoryPage);     // NEW: charted history page
  server.on("/api/history", handleApiHistory);  // NEW: JSON feed for the history page
  server.on("/api/history/export", handleHistoryExport);                                  // NEW: CSV download per city
  server.on("/api/history/import", HTTP_POST, handleHistoryImportDone, handleHistoryUpload); // NEW: CSV upload (merge/replace)
  server.on("/api/history/wipe", HTTP_POST, handleHistoryWipe);                            // NEW: wipe a city / all / archived logs / local log
  server.on("/api/history/status", handleHistoryStatus);                                  // NEW: backfill progress
  server.on("/api/history/backfill", HTTP_POST, handleHistoryBackfill);                    // NEW: start/stop archive backfill
  server.on("/api/local", handleApiLocal);                                                // NEW: hourly local sensor (BME280) readings
  server.on("/api/local/export", handleLocalExport);                                      // NEW: local sensor log as CSV
#endif
  server.begin();

  tft.fillScreen(bgColor); tft.setTextColor(TFT_BLACK); tft.setTextDatum(CC_DATUM);
  tft.drawString("SYNCING TIME...", 160, 240, 2);

  Udp.begin(localPort); 
  setSyncProvider(getNtpTime);

  tft.fillScreen(bgColor); tft.drawString("FETCHING WEATHER...", 160, 240, 2);
  getOpenWeatherData();

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

void loop() {
  server.handleClient();
  checkWifiHealth(); // NEW: watches for a dropped connection and recovers automatically
  handleSerialCommand(); // NEW: read-only ping/status over Serial
  saveBrightnessIfDue(); // persists slider changes once you've stopped dragging
#if USE_SD_HISTORY
  historyLoop(); // NEW: background per-city history sync (one city at a time, every 6h)
#endif

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
    readLocalSensor();
#if USE_SD_HISTORY
    localLogSample(); // NEW: hourly averages to /local.csv on the SD card
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
      if (ty > MODE_BTN_Y && ty < MODE_BTN_Y + MODE_BTN_H) {
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
  
  drawnMode = displayMode;
}

// --- [HISTORY WEB PAGE] ---
// Kept at the very end of the sketch on purpose: the page's JavaScript contains
// lots of quote characters, which confuse the Arduino IDE's automatic
// prototype generator for any C++ that comes after it.
#if USE_SD_HISTORY
const char HISTORY_PAGE_TOP[] PROGMEM = R"rawliteral(<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><style>
body{font-family:sans-serif;text-align:center;max-width:900px;margin:0 auto;padding:10px;color:#222;}
h1{font-size:1.4em;} h2{font-size:1.05em;margin:24px 0 4px;} h2.sec{margin-top:40px;padding-top:16px;border-top:1px solid #ddd;}
.btnRow{margin:8px 0;}
.btnRow button{padding:6px 14px;margin:3px;border-radius:6px;border:1px solid #4a5a7a;background:#fff;cursor:pointer;font-size:0.95em;}
.btnRow button.active{background:#4a5a7a;color:#fff;}
#modeRow button,#cityRow button{font-weight:bold;}
.split{display:inline-block;width:14px;}
canvas{width:100%;max-width:840px;border:1px solid #ddd;border-radius:8px;}
#tooltip{position:fixed;background:#222;color:#fff;padding:6px 10px;border-radius:6px;font-size:0.85em;pointer-events:none;display:none;text-align:left;z-index:9;}
#noData{color:#777;margin-top:40px;}
#storageBox{margin:12px auto;max-width:520px;font-size:0.9em;color:#444;}
#storageBar{height:10px;background:#e3e7ee;border-radius:5px;overflow:hidden;margin-top:6px;}
#storageFill{height:100%;width:0;background:#4a90d9;}
.legend{display:flex;justify-content:center;gap:16px;flex-wrap:wrap;font-size:0.88em;margin:2px 0 6px;color:#444;}
.legend span{display:inline-flex;align-items:center;gap:6px;}
.sw{width:16px;height:4px;border-radius:2px;display:inline-block;vertical-align:middle;}
.tw{overflow-x:auto;}
table{border-collapse:collapse;margin:8px auto;font-size:0.88em;}
th,td{padding:5px 9px;border-bottom:1px solid #e6e6e6;white-space:nowrap;}
th{color:#4a5a7a;font-weight:600;}
tr.sel td{background:#eef2f8;font-weight:600;}
.note{font-size:0.8em;color:#888;margin-top:8px;}
label.chk{font-size:0.9em;color:#444;cursor:pointer;}
details{margin:34px auto 20px;max-width:680px;text-align:left;border:1px solid #ddd;border-radius:8px;padding:10px 16px;}
summary{cursor:pointer;font-weight:bold;color:#4a5a7a;}
.mg{margin:18px 0;} .mg h3{font-size:0.95em;margin:0 0 6px;color:#4a5a7a;}
.mg button,.mg select{margin:3px 6px 3px 0;padding:5px 10px;border-radius:6px;border:1px solid #4a5a7a;background:#fff;cursor:pointer;}
.mg button.danger{border-color:#cc3333;color:#a00;background:#fff4f4;}
.mg button:disabled{opacity:0.5;cursor:default;}
#bfStatus div{margin:2px 0;}
.bar{display:inline-block;width:120px;height:8px;background:#e3e7ee;border-radius:4px;overflow:hidden;vertical-align:middle;margin:0 6px;}
.bar i{display:block;height:100%;background:#4a90d9;}
#msg{font-size:0.9em;margin-top:8px;}
</style></head><body>
<h1>Weather History</h1>
)rawliteral";

const char HISTORY_PAGE_BOTTOM[] PROGMEM = R"rawliteral(
<div class='btnRow' id='modeRow'><button data-m='single'>Single city</button><button data-m='compare'>Compare cities</button></div>
<div id='singleCtl'>
<div class='btnRow' id='cityRow'></div>
<label class='chk'><input type='checkbox' id='sameScale'> Same scale for all cities</label>
</div>
<div id='compareCtl' style='display:none;'>
<div class='btnRow' id='metricRow'><button data-k='tMax'>Highs</button><button data-k='tMin'>Lows</button><button data-k='mean'>Average</button></div>
</div>
<div class='btnRow'><span id='rangeRow'><button data-d='7'>7D</button><button data-d='30'>30D</button><button data-d='90'>90D</button><button data-d='365'>1Y</button><button data-d='0'>All</button></span><span class='split'></span><span id='groupRow'><button data-g='day'>Daily</button><button data-g='month'>Monthly</button></span></div>
<div id='storageBox'><div id='storage'>Loading...</div><div id='storageBar'><div id='storageFill'></div></div></div>
<div id='noData' style='display:none;'></div>
<div id='content' style='display:none;'>
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

<div id='localSec' style='display:none;'>
<h2 class='sec'>Local sensor (BME280)</h2>
<div class='btnRow' id='lRangeRow'><button data-h='24'>24H</button><button data-h='168'>7D</button><button data-h='720'>30D</button><button data-h='8760'>1Y</button><button data-h='0'>All</button></div>
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
var PRES_HINT='<span style="color:#888;">high = settled weather &middot; low or falling = unsettled, rain/wind likely</span>';
var SAME_NOTE='<span style="color:#888;">same scale as the other cities</span>';
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
var loadAll=async function(){
  try{
    var cities=[],meta=null,i=0,n=1;
    while(i<n){var j=await (await fetch('/api/history?city='+i)).json();meta=j;n=j.names.length;cities.push({name:j.names[i],days:parseDays(j.days)});i++;}
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
    ctx.strokeStyle=(Math.abs(v)<1e-9&&sc.lo<0)?'#9a9a9a':'#e4e4e4';
    ctx.beginPath();ctx.moveTo(PAD.l,y);ctx.lineTo(w-PAD.r,y);ctx.stroke();
    var s=v.toFixed(dc);if(parseFloat(s)===0)s=(0).toFixed(dc);ctx.fillStyle='#888';ctx.fillText(s,PAD.l-6,y+3);}
  return Y;
}
var xs=function(cv,n){var bw=(cv.width-PAD.l-PAD.r)/n;return{bw:bw,X:i=>PAD.l+(i+0.5)*bw};}
var dateLabels=function(C,dates,X){var ctx=C.ctx,n=dates.length,every=Math.max(1,Math.ceil(n/10));ctx.fillStyle='#555';ctx.textAlign='center';ctx.font='10px sans-serif';
  dates.forEach((d,i)=>{if(i%every==0)ctx.fillText(lbl(d,n),X(i),C.cv.height-8);});}
var polyline=function(ctx,pts,color,w){ctx.strokeStyle=color;ctx.lineWidth=w||1.8;ctx.beginPath();var pen=false;
  pts.forEach(p=>{if(p==null){pen=false;return;}if(!pen){ctx.moveTo(p.x,p.y);pen=true;}else ctx.lineTo(p.x,p.y);});ctx.stroke();}
var dot=function(ctx,x,y,c){ctx.fillStyle=c;ctx.beginPath();ctx.arc(x,y,3,0,7);ctx.fill();}
var emptyChart=function(C,t){var ctx=C.ctx,cv=C.cv;ctx.clearRect(0,0,cv.width,cv.height);ctx.fillStyle='#999';ctx.font='13px sans-serif';ctx.textAlign='center';ctx.fillText(t,cv.width/2,cv.height/2);}
var emptyAll=function(t){['temp','rain','sun','hum','pres','wind'].forEach((k,i)=>emptyChart(CH[k],i?'':t));}
// lines: series=[{c,v:[value|null],w}]
var drawSeries=function(C,dates,series,sc,o){
  o=o||{};var ctx=C.ctx,Y=frame(C,sc),g=xs(C.cv,dates.length);
  if(o.ref!=null&&o.ref>sc.lo&&o.ref<sc.hi){var ry=Y(o.ref);ctx.save();ctx.setLineDash([4,4]);ctx.strokeStyle='#bbb';ctx.beginPath();ctx.moveTo(PAD.l,ry);ctx.lineTo(C.cv.width-PAD.r,ry);ctx.stroke();ctx.restore();
    ctx.fillStyle='#aaa';ctx.textAlign='left';ctx.font='10px sans-serif';ctx.fillText(o.refLabel,PAD.l+4,ry-3);}
  if(o.fill){var seg=[],base=Y(sc.lo);ctx.fillStyle=o.fill;
    var flush=()=>{if(seg.length){ctx.beginPath();ctx.moveTo(seg[0].x,base);seg.forEach(p=>ctx.lineTo(p.x,p.y));ctx.lineTo(seg[seg.length-1].x,base);ctx.closePath();ctx.fill();}seg=[];};
    series[0].v.forEach((v,i)=>{if(v==null)flush();else seg.push({x:g.X(i),y:Y(v)});});flush();}
  series.forEach(s=>polyline(ctx,s.v.map((v,i)=>v==null?null:{x:g.X(i),y:Y(v)}),s.c,s.w||1.8));
  if(o.partialLast){var v0=series[0].v,L=v0.length-1;if(v0[L]!=null)dot(ctx,g.X(L),Y(v0[L]),'#999');}
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
  if(part){dot(tx,g.X(L),Y(d[L].tMax),'#999');dot(tx,g.X(L),Y(d[L].tMin),'#999');}
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
    var dt=x=>'<small style="color:#888;">'+(longR?x.date:x.date.slice(5))+'</small>';
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
  var sm=t=>' <small style="color:#888;">'+t+'</small>';
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
  var ctx=C.ctx,span=e1-e0,n=6;ctx.fillStyle='#555';ctx.font='10px sans-serif';
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
var msg=function(t,bad){var m=$('msg');m.innerText=t;m.style.color=bad?'#a00':'#2e6b2e';}
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
    if(act){bfTick++;if(bfTick%8==0)loadAll();bfTimer=setTimeout(pollBF,5000);}
    else if(bfWas)loadAll();
    bfWas=act;
  }catch(e){bfTimer=setTimeout(pollBF,10000);}
}
loadAll().then(()=>{loadLocal();pollBF();});
</script></body></html>)rawliteral";

void handleHistoryPage() {
  String html = FPSTR(HISTORY_PAGE_TOP);
  html += navBar("history");
  html += FPSTR(HISTORY_PAGE_BOTTOM);
  server.send(200, "text/html; charset=utf-8", html);
}

#endif // USE_SD_HISTORY