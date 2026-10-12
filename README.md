# DesktopDisplay – ESP32 Weather Station

A touch-screen weather station for the 3.5-inch **E32R35T / E32N35T** ESP32 display board. It shows live weather for up to three cities, a rain forecast, sunrise/sunset and moon phase, keeps a day-by-day weather history on a microSD card, can read an optional BME280 indoor sensor, and has a full web interface for settings, a live dashboard and history charts.

<!-- Tip: add a photo of the display here, e.g. ![DesktopDisplay](docs/photo.jpg) -->

## Features

**On the display**
- Big clock with local time for the selected city (time zone is picked up automatically from the weather data)
- Current weather: temperature, feels-like, humidity, wind speed and direction, air pressure, visibility
- Three cities, switchable from the screen or the web page
- Rain page with 1h/3h rainfall and upcoming rain in 3-hour steps, plus a rain preview on the home screen
- Astro page with sunrise, sunset and the current moon phase
- Forecast page and an on-device 7-day history
- Little animated astronaut on the home screen
- Status indicator showing whether the weather data is fresh, updating or failed
- Light and dark themes, or Auto: dark from sunset to sunrise
- Weather icons drawn on the display (sun, moon at night, clouds, rain, drizzle, thunder, snow, fog)
- RGB LED that changes color with the weather (or a fixed color you pick)

**Weather sources** (selectable per city)
- OpenWeatherMap
- Open-Meteo
- Open-Meteo with the KNMI model (KNMI's 2 km Netherlands model, topped up with ECMWF beyond its ~2.5-day range)

**Web interface** (open the device's IP address in a browser; it starts on the live dashboard)
- Works on desktop and phone, in light or dark (the theme button in the top bar switches between Auto, Light and Dark; Auto follows your phone/PC setting)
- Uses your chosen text color as the accent color, just like the display
- Settings: cities, weather source per city, API keys (with an optional backup key), screen and LED brightness (remembered across restarts), colors
- Live dashboard: current weather with icon, 7-segment clock, wind/humidity/pressure/visibility, rain for the next 15 hours, sun position and moon phase, 5-day forecast with temperature ranges, indoor sensor and device status
- History page with charts per city: temperature, rain, sunshine, wind, humidity, pressure, monthly averages and a summary table
- CSV export and import of the history, plus backfilling up to 10 years from Open-Meteo's archive
- Debug console with live touch and system info

**Optional extras**
- microSD card: per-city weather history (`/hist_c0.csv` … `/hist_c2.csv`) and an hourly log of the indoor sensor (`/local.csv`)
- BME280 sensor: indoor temperature, humidity and pressure on the stats page and in the history

The device also reconnects to WiFi on its own if the connection drops.

## Hardware

| Part | Notes |
|---|---|
| E32R35T or E32N35T board | ESP32-WROOM-32E, 4 MB flash, 3.5" 320×480 ST7796 display with resistive touch (E32R35T) |
| microSD card (optional) | FAT32. Uses the board's built-in slot (CS 5, SCK 18, MISO 19, MOSI 23) |
| BME280 sensor (optional) | I2C on **SDA 21 / SCL 25** (not the usual 22, which drives the red LED on this board) |

## Installation

### 1. Arduino IDE and ESP32 support

Install the [Arduino IDE](https://www.arduino.cc/en/software) and add ESP32 support through the Boards Manager (search for **esp32** by Espressif).

### 2. Libraries

Install these through **Tools → Manage Libraries**:

- **TFT_eSPI** by Bodmer (version 2.5 or newer, for the anti-aliased drawing)
- **TJpg_Decoder** by Bodmer
- **WiFiManager** by tzapu
- **ArduinoJson** by Benoit Blanchon
- **Time** by Michael Margolis (provides `TimeLib.h`)
- **Adafruit BME280 Library** and **Adafruit Unified Sensor** (only needed if `USE_BME280` is 1)

### 3. Display settings (important)

TFT_eSPI stores its pin settings inside its own library folder, so they're not part of the sketch. This repo includes a ready-made copy for this board.

Copy `libraries/TFT_eSPI/User_Setup.h` from this repo into your Arduino libraries folder, replacing the existing file:

```
Documents/Arduino/libraries/TFT_eSPI/User_Setup.h
```

Do this again after every TFT_eSPI update, because updating the library overwrites the file. A blank or garbled screen after an update almost always means this step was missed.

### 4. Board settings

Under **Tools**:

| Setting | Value |
|---|---|
| Board | ESP32 Dev Module |
| Flash Size | 4MB (32Mb) |
| **Partition Scheme** | **Huge APP (3MB No OTA/1MB SPIFFS)** |
| PSRAM | Disabled |
| CPU Frequency | 240MHz (WiFi/BT) |
| Flash Mode / Frequency | QIO / 80MHz |
| Upload Speed | 921600 (use 460800 or 115200 if uploads fail) |
| Erase All Flash Before Sketch Upload | Disabled (enabling it wipes your saved settings) |

The partition scheme matters: the sketch is around 1.4 MB, which doesn't fit in the default 1.2 MB app partition ("Sketch too big").

### 5. Upload

Open `DesktopDisplay.ino` and click Upload. The sketch folder must be called `DesktopDisplay` (the Arduino IDE requires the folder and `.ino` names to match).

## First-time setup

1. **Touch calibration.** On the very first boot the screen says *TOUCH CORNERS TO CALIBRATE*. Tap each corner marker. The result is saved, so this only happens once.
2. **WiFi.** The display creates a WiFi network called **WeatherStation-Setup**. Connect to it with your phone, open **192.168.4.1** if the setup page doesn't appear by itself, and pick your home WiFi.
3. **API key.** Get a free API key at [openweathermap.org](https://openweathermap.org/api). Then open the device's IP address (shown on the system page of the display) in a browser, go to **Settings** (or open `http://<device-ip>/settings` directly), paste the key into **API Key** and save. A key is needed even if you only use Open-Meteo, because city names are looked up through OpenWeatherMap's geocoder.
4. **Cities.** Enter up to three cities as `City,CountryCode`, for example `Amsterdam,NL` or `London,GB`, and pick a weather source for each.

All settings are stored in the ESP32's flash and survive power cuts and new uploads.

## Using the display

| Action | What it does |
|---|---|
| Tap the right half of the screen | Next page |
| Tap the left half of the screen | Previous page |
| Tap the arrows beside the city name (home page) | Previous / next city |
| Hold anywhere for 1.5–5 seconds | Next city |

The pages are: **Home**, **Stats**, **Rain**, **Astro**, **System**, **Forecast** and **History**.

On the **System** page you can drag the sliders for screen and LED brightness (saved automatically a few seconds after you let go), switch the LED between weather colors and a custom color, pick the custom color, and save it as the default. The **THEME** button cycles between Auto (dark after sunset), Light and Dark; the same setting is also on the web Settings page.

LED weather colors: orange = clear, white = clouds, blue = rain, purple = thunderstorm, cyan = snow.

## Web pages

| Address | What's there |
|---|---|
| `/` | Live dashboard (start page; `/dashboard` also still works) |
| `/settings` | Settings |
| `/history` | History charts, export, import, backfill |
| `/debug` | Debug console |

There are also JSON endpoints (`/api/status`, `/api/settings`, `/api/history`, `/api/local` and others, plus `/api/history/raw?city=N` for a city's raw history CSV) used by these pages, which you can use for your own scripts. Reading them needs nothing special. Endpoints that change something (`/save`, `/live`, `/saveColor`, `/saveTextColor` and the history wipe/backfill/import endpoints) only accept requests carrying the header `X-Requested-With: DesktopDisplay` (see [SECURITY.md](SECURITY.md)), for example:

```
curl -H "X-Requested-With: DesktopDisplay" "http://<device-ip>/live?action=city&value=1"
```

## Configuration switches

At the top of `DesktopDisplay.ino`:

| Switch | Default | Meaning |
|---|---|---|
| `USE_SD_HISTORY` | 1 | Set to 0 if you don't use a microSD card |
| `USE_BME280` | 1 | Set to 0 if you don't have a BME280 sensor |
| `DIAG_DISABLE_TOUCH` | 0 | Set to 1 only to test whether touch is causing a freeze |

## Troubleshooting

**"Sketch too big" when compiling**
Set the Partition Scheme to *Huge APP (3MB No OTA/1MB SPIFFS)*.

**Blank, white or garbled screen**
Copy `User_Setup.h` into the TFT_eSPI library folder again (see step 3). If it still glitches, try lowering `SPI_FREQUENCY` in that file from 80 MHz to 40 MHz.

**Touch stops responding but the screen still updates**
This happens if the SD card is moved onto the same SPI bus as the display. The SD card must stay on HSPI and `USE_HSPI_PORT` must stay commented out in `User_Setup.h`.

**The touch calibration screen appears again**
Normal after changing the partition scheme, because the calibration file lives in the SPIFFS area. Tap the corners once more.

**A script gets "403 Forbidden"**
Endpoints that change something need the header `X-Requested-With: DesktopDisplay` (see [Web pages](#web-pages)). Reading endpoints like `/api/status` don't.

**Weather doesn't load**
Check the API key on the settings page (`/settings`) and that the city is written as `City,CountryCode`. A new OpenWeatherMap key can take up to a couple of hours to start working.

## Known limitations

- The web interface has no password. See [SECURITY.md](SECURITY.md).

## Credits

This project started as a heavily modified version of [SmallDesktopDisplay](https://github.com/SmallDesktopDisplay-team/SmallDesktopDisplay). The astronaut animation comes from that project.

Weather data from [OpenWeatherMap](https://openweathermap.org) and [Open-Meteo](https://open-meteo.com) (including KNMI models). Built with [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI), [TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder), [WiFiManager](https://github.com/tzapu/WiFiManager) and [ArduinoJson](https://arduinojson.org).

## License

MIT – see [LICENSE](LICENSE).
