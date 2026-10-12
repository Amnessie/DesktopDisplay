# Security

DesktopDisplay is meant to run on a trusted home network. Please keep the following in mind.

## Things to know

- **The web interface has no password.** Anyone on the same WiFi network can open the device's IP address, change settings and start or wipe the history.
- **API keys are shown in plain text** on the settings page, so anyone on your network can read them.
- **Don't expose the device to the internet.** Don't port-forward it on your router or put it on a public or guest network you don't control.
- The OpenWeatherMap requests use plain HTTP, so the API key travels unencrypted inside your network and to OpenWeatherMap. Open-Meteo requests use HTTPS.

## Keeping your API key safe

- Never put your API key in the source code. Enter it on the device's settings page; it's stored in the ESP32's flash, not in the sketch.
- If you think your key has leaked (for example it was committed to a public repo), delete it in your OpenWeatherMap account and create a new one.

## Reporting a problem

If you find a security issue, please report it privately through GitHub's **Security → Report a vulnerability** option on this repository instead of opening a public issue.
