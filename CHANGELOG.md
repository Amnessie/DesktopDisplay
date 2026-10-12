# Changelog

All notable changes to this project are listed here.

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
