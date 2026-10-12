# Changelog

All notable changes to this project are listed here.

## [Unreleased]

### Fixed
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
