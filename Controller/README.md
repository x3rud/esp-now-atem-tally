# ESP-NOW Tally Controller

ESP32-based bridge that reads switcher tallies (ATEM, OBS 5.x, or vMix) and broadcasts state to up to 64 wireless receivers over ESP-NOW. Runs a web UI/API for configuration, shows status on an OLED, and serves a vMix-compatible TCP tally endpoint.

## Hardware
- Primary target: WT32-ETH01 (ESP32 + Ethernet) with OLED on I2C (`SDA=33`, `SCL=32` on WT32).  
- Alternative: Olimex ESP32-POE-ISO (env `esp32-poe-iso`).  
- Optional 128x64 SSD1306 OLED on I2C (`OLED_I2C_ADDRESS` defaults to `0x3C`; override `OLED_SDA_PIN`/`OLED_SCL_PIN` if needed).

## Building & Flashing (PlatformIO)
1) Install PlatformIO.  
2) Choose env in `platformio.ini`: `wt32-eth01` (default) or `esp32-poe-iso`.  
3) First flash over serial (set `upload_protocol = esptool` and `upload_port` to your USB device), then OTA is enabled with `upload_protocol = espota` and `upload_port = tally-controller.local` (or device IP).  
4) Build: `pio run -e wt32-eth01`  
5) Upload firmware: `pio run -e wt32-eth01 -t upload`  
6) Upload SPIFFS assets (`data/index.html`, `data/ota.html`): `pio run -e wt32-eth01 -t uploadfs`  
7) Monitor: `pio device monitor -b 115200`

### Useful build flags
Add under the desired env in `platformio.ini`:
- Disable WebSocket broadcasting (already default): `-DDISABLE_WS`
- Custom I2C pins: `-DOLED_SDA_PIN=<gpio> -DOLED_SCL_PIN=<gpio>`

## What it does
- Connects to one of three protocols (configurable):  
  - **ATEM** via `ATEMstd` over Ethernet.  
  - **OBS** via obs-websocket 5.x (WebSocket).  
  - **vMix** via TCP tally subscription.  
- Broadcasts tally program/preview bits over ESP-NOW to receivers and forwards per-device commands (name, brightness, ID, identify, blink, signal).  
- Tracks receiver heartbeats (RSSI, last seen, names, brightness) and shows counts and signal health on OLED.  
- Hosts a simple HTTP UI/API on port 80; optional WebSocket mirror on port 81 (disabled when `DISABLE_WS` is set).  
- Exposes a vMix-compatible TCP tally server on port 8099 for downstream tools.  
- mDNS: `tally.local` (HTTP) and `tally-controller.local` (OTA).

## Configuration & Web API
The web UI is served from SPIFFS; if missing, `/` returns 500. Key endpoints:
- `GET /config` – current protocol, connection state, IPs/ports, and known tallies.  
- `GET /tally` – JSON with `program`/`preview` bitfields.  
- `GET /seen` – JSON of recently heard receivers (id, age, MAC, name, signal, brightness).  
- `GET /set` – control endpoint (returns `OK` unless validation fails). Parameters:
  - `program=<csv>` / `preview=<csv>`: set tally bits (e.g. `program=1,4&preview=2`).  
  - `color=<RRGGBB>&i=<csv>`: set override color for IDs.  
  - `brightness=<0-255>&i=<csv>`: set RGB brightness for IDs.  
  - `brightness=<0-255>&mac=<AA:BB:...>`: per-MAC brightness.  
  - `statusbrightness=<0-255>&mac=<...>`: per-MAC status LED brightness.  
  - `camid=<n>&i=<csv>`: reassign IDs in bulk; or with `mac=<...>` for per-device.  
  - `name=<text>&i=<csv>`: set receiver names; or `name=<text>&mac=<...>` for per-MAC.  
  - `identify[&seconds=<n>]&i=<csv>` or with `mac=<...>`: trigger identify blink.  
  - `blink=<RRGGBB>&i=<csv>` (add `&off` to disable): make LEDs blink.  
  - `signal=<n>&i=<csv>`: send custom signal to IDs (also forwarded to OBS as vendor events).  
  - Controller config: `protocol=<1|2|3>`, `connect=<0|1>`, `atemip=<x.x.x.x>`, `obsip`, `obsport`, `vmixip`, `vmixport`. Changes persist to EEPROM; protocol changes reboot to take effect.

## Protocol specifics
- **ATEM**: uses `ATEMstd`; listens for program/preview tallies and triggers ESP-NOW updates. Default IP: `192.168.88.240`, port `9910`.  
- **OBS**: connects to obs-websocket 5.x (`obsip`/`obsport`), maps scene names containing `T<number>` tags to tally bits, and listens for custom/vendor events to relay signals.  
- **vMix**: connects to vMix tally TCP (`vmixip`/`vmixport`), subscribes, parses `TALLY OK ...` payloads, and also serves a local TCP tally server on port 8099 mirroring current state.  
- Heartbeats to receivers are pushed every `TALLY_UPDATE_EACH` ms (2s) from `espNow.cpp`.

## File layout
- `platformio.ini` – two ESP32 Ethernet envs; OTA upload is default.  
- `src/` – protocol bridges, ESP-NOW broadcaster, web server/API, OLED status.  
- `data/` – SPIFFS web UI (`index.html`) and OTA upload page (`ota.html`).  
- `receiver-node/` – separate firmware for ESP8266 tally receivers.

## Troubleshooting
- **Cannot reach web UI**: ensure SPIFFS is uploaded (`pio run -t uploadfs`) and device has an IP (check OLED/serial).  
- **OTA upload fails**: verify `upload_port` matches the device IP/hostname and that the board is reachable on the network. First flash must be over serial.  
- **Receivers not updating**: confirm ESP-NOW channel proximity, check OLED for heartbeat age/RSSI, and verify `program/preview` bits via `/tally`.  
- **OBS not driving tallies**: scene names must contain tags like `T1`, `T2`, etc., to map to tally IDs; confirm obs-websocket v5 is installed and reachable.

