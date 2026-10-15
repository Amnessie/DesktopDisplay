# Changelog

All notable changes to this project are listed here.

## [Unreleased]

### Added
- Night mode (web Settings): between two times (City 1's local time) the screen dims to a chosen level or switches off, with a smooth fade, and the LED can switch off too (the rain alert then stays quiet). When the screen is off, a touch wakes it for 30 seconds without pressing anything. The dashboard's device status shows when night mode is on
- System page: an LED | TEXT switch above the color bar, so the bar on the device can set the text/accent color as well as the LED color; the save button stores the selected one
- README with screenshots of the display and the web dashboard (desktop and phone) that follow your GitHub light/dark theme, and a short Highlights section
- Rain in the next 2 hours: Buienradar radar for cities in the Netherlands/Belgium (5-minute steps, refreshed every 5 minutes), Open-Meteo 15-minute forecast elsewhere. Shown on the home screen, the Precipitation page and the dashboard. Rain only "starts" or "stops" on two consecutive steps, values under 0.1 mm/h count as dry, and the outlook disappears when the data is more than 15 minutes old
- LED rain alert (web Settings, with a test button): the LED dims twice, slowly, when rain is expected within 30 minutes, once per rain spell
- Pressure trend: least-squares slope over the last 3 hours of Open-Meteo sea-level pressure, in WMO tendency classes with hysteresis so the label can't flicker at a boundary; arrow in the pressure tile, row on the Atmosphere page, text on the dashboard
- Radar and model blending: where the radar sees no rain for the next hour but Open-Meteo's 15-minute model expects rain 1 to 3 hours out (two wet steps in a row), the outlook says "Dry for the next hour; rain possible from 16:45 (forecast)". The radar always decides the first hour and the LED alert stays radar-only. The model is drawn as faint bars behind the radar bars on the display and the dashboard
- Forecast check (dashboard): for cities inside the radar area, every 3-hour period is scored afterwards against the radar for OpenWeatherMap, Open-Meteo and Open-Meteo KNMI (hits, misses, false alarms, and a score that ignores easy dry days). After 10 rainy periods it recommends the best source, with a button to switch to it. Counts are kept across reboots and start over when the city is renamed
- Data sources panel on the Debug page
- Display themes: Light, Dark and Auto (dark between sunset and sunrise for the city on screen). Switch with the THEME button on the System page or on the web Settings page
- Redesigned display: weather icons, a new home screen with a clock card, detail tiles and upcoming-rain bars, bold text, and anti-aliased cards on every page
- Your text color is used as the accent for highlights (page dots, sliders, buttons) on the display and the web pages, when it is a clear, readable color
- Redesigned web interface for desktop and phone, with light/dark/auto themes: a new dashboard (weather icons, rain chart, sun arc, moon phase, forecast range bars, indoor sensor and device status), card-based Settings, and History and Debug in the same style. Shared stylesheet/script are cached by the browser

### Fixed
- Compile error on ESP32 core 3.x ("'FS' was not declared" in WebServer.h) after the image decoder was removed: the file-system headers (FS, SPIFFS, SD) are now included first and explicitly, instead of relying on another library to pull them in
- "Test the pulse" never reached the device: its inline click handler sits inside the Settings form, where the name `api` means the API key field, not the request helper ("api is not a function"). The handler now lives in the page script
- LED rain alert: the LED dips twice from its normal brightness to 5% of it and back, 1 second per dip, in its current color (custom or weather sync), through the same code the LED brightness slider uses, then returns to exactly where it was. Start and end are logged on the Debug page
- The weather update (every 10 minutes, and on every city switch) runs in the background too, so nothing waits on the network anymore: switching city responds right away, shows the "..." badge, and the new weather appears when it's in. Previously it ran on the main loop and first waited up to 3 s for any background download, which during a history backfill froze the screen for several seconds each time
- A long press now needs 2.5 s (was 1.5 s) to switch city, so a slow tap no longer switches city by accident
- Freezes of 3-5 s every ~15 s while a history backfill runs: history downloads now go through the background network task, and merging the fetched days into the (sorted, possibly years-long) history file on the SD card runs in ~25 ms slices instead of one long block. The new file is written next to the old one and swapped in at the end, so the History page always reads a complete file; renaming, wiping or importing a city cancels a merge in progress
- Weather icons on the display are drawn with 4x4 supersampling: no more light fringes where a cloud overlaps the sun, seams inside clouds, or a ring around the crescent moon
- The rain outlook remembers which Buienradar hostname answered and starts there, instead of first waiting on one that's down every time; the forecast check's radar samples use the working hostname too (they were stuck on the first one, so cities could never be scored when it was down)
- Freezes and a slow web interface: the rain outlook, rain model, pressure trend and forecast check requests now run on a background task, so a slow server can no longer hold up the touchscreen or the web pages. HTTPS requests also give up after 8 s in the TLS handshake instead of the library's 120 s, and they never run two at a time (each needs ~45 KB of RAM). The Debug page shows the longest freeze since boot and the lowest free memory, and the log records why the device last restarted
- A failed weather refresh no longer blanks the screen (0.0 °C) until the next attempt; the last good data stays up, with the status dot turning red
- Open-Meteo (KNMI) no longer shows made-up "0/0 C CLEAR" forecast days: it now uses KNMI's seamless model (2 km Netherlands model, topped up with ECMWF), and any day a model doesn't cover shows "NO DATA"
- The Debug page no longer breaks ("connection lost") after a history sync error; text in the JSON endpoints is now escaped properly
- The Open-Meteo rain graph now shows real 3-hour totals instead of one hour out of every three
- Open-Meteo air pressure is now the sea-level value, matching OpenWeatherMap and the history
- City names with an apostrophe (e.g. Val-d'Or) no longer break the settings form
- The saved custom LED color now also keeps its saturation and lightness
- The OpenWeatherMap forecast could run out of memory part-way through with ArduinoJson 6, losing the last forecast days; only the needed fields are parsed now
- Half-hour time zones (e.g. India) now show the correct time
- Screen and LED brightness are now remembered across restarts
- City names with `&`, `#` or accented letters are now looked up correctly

### Security
- Endpoints that change settings or history now require the `X-Requested-With: DesktopDisplay` header, so other websites can't change them through your browser

### Changed
- Removed the home screen animation (and with it the TJpg_Decoder library and the image files). With the rain card switched off, the card shows today's sunrise, sunset and hours of daylight
- The moon on the Sun & Moon page is now drawn instead of a photo: exact phase with smooth edges in the theme's colors, mirrored for southern-hemisphere cities, plus % lit and days to the next full/new moon. It uses the same phase calculation as the web dashboard (the old 8-picture calculation could be a phase off). The 8 moon images are removed (~45 KB less flash); the dark theme's inner cards are slightly darker
- The Settings page loads its values from the new `/api/settings` endpoint
- Requires TFT_eSPI 2.5 or newer (anti-aliased drawing functions)
- Atmosphere page shows label and value on one line; forecast rows show an icon and a temperature range bar
- Opening the device's address now shows the Live Dashboard; Settings moved to `/settings`
- The dashboard clock uses 7-segment digits like the device's own clock
- Home screen: the temperature and its °C are now centered together, and the page dots no longer touch the card border
- The History page loads much faster with years of data: the SD card is read in 2 KB blocks instead of one byte at a time, the raw CSV is sent as-is and parsed in the browser, and unchanged files are not downloaded again (ETag). Background history syncs, imports and exports use the faster reading too
- New endpoints `/api/history/meta` and `/api/history/raw?city=N` (raw CSV); `/api/history` still works for your own scripts
- Raw API responses are no longer printed to the serial port
- Settings, dashboard and debug pages now scale properly on phones

## [1.0.0] – 2026-10-12

First public release.

### Features
- Weather for up to three cities, with OpenWeatherMap, Open-Meteo or Open-Meteo (KNMI model) selectable per city
- Seven display pages: Home, Stats, Rain, Astro, System, Forecast and History
- Touch controls for pages, cities, brightness and LED color
- RGB LED that follows the weather, or a custom color
- microSD weather history per city, with backfill from Open-Meteo's archive (up to 10 years), CSV export/import and charts in the browser
- Optional BME280 indoor sensor with an hourly log
- Web interface: settings, live dashboard, history charts and debug console
- Optional backup API key, used automatically if the main key is rejected or rate-limited
- Automatic WiFi reconnection and stuck-touch recovery

### Changed
- Clock digits are now drawn with TFT_eSPI's built-in 7-segment font instead of JPEG images
- SD card moved to its own SPI bus (HSPI) so it no longer breaks touch input

### Removed
- Unused leftovers from the original SmallDesktopDisplay project: JPEG digit fonts, the `Number` and `WeatherNum` classes, the old weather icon set, the Misaka boot animation, an unused 1.2 MB font and duplicate moon images
- The hard-coded API key. Keys are now entered only through the web settings page
