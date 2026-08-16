# ser2wifi

Serial-to-WiFi bridge on ESP32-C5 Zero. Streams UART data to a web page over WebSocket and lets you send commands back to the serial port.

## Hardware

| Signal | GPIO  | Notes                        |
|--------|-------|------------------------------|
| RX     | GPIO4 | External device TX -> ESP32  |
| TX     | GPIO5 | ESP32 -> external device RX  |
| LED    | GPIO27| WS2812 onboard RGB LED       |

## Features

- Real-time serial output in browser with ANSI color support
- Send ASCII text via web UI (appends `\n`)
- TX confirmation echo (`>> text`) in cyan
- Pause / Resume per client, Pause All / Resume All for everyone
- Copy / Save / Clear log buttons
- WebSocket auto-reconnect with watchdog
- WiFi auto-reconnect, LED status indicator
- Support for channels 1-13 (EU regulatory domain)

## Build & Flash

Requires [pioarduino](https://github.com/pioarduino/platform-espressif32) for ESP32-C5 support.

```bash
pio run -t upload
pio device monitor
```

## WiFi

Edit SSID and password in `app.cpp`:

```cpp
const char* SSID     = "";
const char* PASSWORD = "";
```
