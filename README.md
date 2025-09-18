# ESP32 Motorcycle GPS Data Logger (CSV + GPX)

A compact, robust ride logger for motorcycles based on an ESP32 DevKitC, a u‑blox NEO‑7M GPS module, and a microSD card. It records GPS data once per second to CSV and GPX, designed to keep files valid even if power is cut when the ignition turns off. Includes an on‑device Wi‑Fi live dashboard you can open on your phone.

---

## Quick Start

1) Wire the modules
- GPS NEO‑7M → ESP32
  - TX → `GPIO16` (ESP32 RX2)
  - RX → `GPIO17` (ESP32 TX2)
  - VCC → 3.3 V, GND → GND
- microSD (SPI) → ESP32
  - CS → `GPIO5`
  - SCK/CLK → `GPIO18`
  - MISO/DO → `GPIO19`
  - MOSI/DI → `GPIO23`
  - VCC → 3.3 V, GND → GND

2) Prepare the SD card
- Format as FAT32 (MBR). exFAT will not mount with the Arduino SD driver on ESP32.

3) Build and upload (PlatformIO)
```bash
pio run -t upload
pio device monitor  # 115200 baud
```

4) Ride and log
- Place the GPS antenna with a clear view of the sky; first fix can take 1–5 minutes (longer indoors).
- A per‑session folder is created at SD root, e.g. `/session-0001/`, containing `gpslog.csv`, `gpslog.gpx`, and `session.log`.
- After GPS time is valid, the folder is renamed to a timestamp (e.g., `/session-20250101_123045/`).

5) Optional: Live dashboard on your phone
- By default, the ESP32 tries your Wi‑Fi if credentials are set; otherwise it brings up an AP like `GPS-Tracker-XXXX` (password `gpslogger`).
- Open the printed IP from Serial, e.g. `http://192.168.4.1/`, to see live position, sats/HDOP, and a Google Maps link.

---

## Project Overview

- Purpose: Log GPS latitude, longitude, altitude, speed, course, timestamp to CSV and GPX at 1 Hz for post‑ride analysis.
- Why: GPX loads easily into Google Earth/Strava/My Maps; CSV works with spreadsheets or custom scripts. The logger is resilient to sudden power loss typical of ignition‑switched bike power.
- Extras: A minimal Wi‑Fi web UI for live monitoring on a phone, even without Internet (AP mode).

### Key Features
- Dual logging: CSV (tabular) + GPX (map‑friendly) at 1 Hz
- Always‑valid GPX: re‑appends the closing tail after every point
- Auto file rename: switches from numbered to timestamped names once GPS time is known
- Session diagnostics log with boot info and 60 s heartbeats
- Wi‑Fi live dashboard (`/` and `/gps.json`) in STA or AP mode

---

## Hardware

- MCU: ESP32 DevKitC (`board = esp32dev`)
- GPS: u‑blox NEO‑7M (UART @ 9600 baud default)
- Storage: microSD (SPI) breakout board (3.3 V logic)
- Power: Ignition‑switched 5 V motorcycle USB, ideally through an automotive‑rated buck converter

### Wiring Map

| Module | Signal | ESP32 Pin |
|---|---|---|
| GPS | TX | `GPIO16` (RX2) |
| GPS | RX | `GPIO17` (TX2) |
| GPS | VCC | 3.3 V |
| GPS | GND | GND |
| SD  | CS  | `GPIO5` |
| SD  | SCK | `GPIO18` |
| SD  | MISO | `GPIO19` |
| SD  | MOSI | `GPIO23` |
| SD  | VCC | 3.3 V |
| SD  | GND | GND |

Notes
- Many GPS boards accept 5 V on their VIN regulator but still use 3.3 V UART logic. Connect UART to 3.3 V pins.
- Keep antenna away from noise sources, with clear sky view.
- Bike power can be noisy; short wiring and input filtering help (e.g., 470–1000 µF electrolytic + 0.1 µF ceramic at the 5 V input).

---

## Software Setup

- Editor/IDE: VS Code + PlatformIO extension
- Framework: Arduino
- Environment: `platformio.ini` → `[env:esp32dev]`
- Libraries
  - `TinyGPSPlus` via `lib_deps`
  - `SD`, `WiFi`, `WebServer` from the ESP32 Arduino core

Build/Upload/Monitor
```bash
pio run -t upload
pio device monitor  # 115200 baud
```

Configure Wi‑Fi via build flags (recommended, keeps secrets out of source):
```ini
; platformio.ini
[env:esp32dev]
; ...
build_flags =
  -D WIFI_SSID="YourSSID"
  -D WIFI_PASS="YourPassword"
```
- Leave `WIFI_SSID` empty (`-D WIFI_SSID=""`) to skip STA and start only AP mode.
- To disable Wi‑Fi entirely, remove or comment the `startWiFiAndWeb();` call in code.

---

## Configuration (Code Constants)

Source: `src/main.cpp`
- UART/GPS
  - `GPS_BAUD` (default `9600`): `src/main.cpp:17`
  - `GPS_RX_PIN = 16`, `GPS_TX_PIN = 17`: `src/main.cpp:18`
- SD (SPI)
  - `SD_CS_PIN = 5` (SPI pins use ESP32 defaults 18/19/23): `src/main.cpp:22`
- Logging
  - `LOG_INTERVAL_MS = 1000` for 1 Hz logging: `src/main.cpp:28`
  - Per‑session folder at SD root: initial `session-0001`, then timestamped after GPS fix.
- Wi‑Fi & web server
  - `startWiFiAndWeb()`: `src/main.cpp:132` (serves `/` and `/gps.json`)

---

## How It Works

Setup sequence (see `setup()` at `src/main.cpp:441`)
1. `waitForSerial()` — starts Serial and pauses briefly so logs show up: `src/main.cpp:197`
2. `initSD()` — initializes SPI and mounts SD: `src/main.cpp:204`
3. `nextLogNames()` — chooses first free numbered names for CSV/GPX/session: `src/main.cpp:214`
4. `openSession()` — opens a session log; `logBoth()` mirrors key messages to Serial and session: `src/main.cpp:288`
5. `openCSV()` — creates CSV and writes a header if new: `src/main.cpp:227`
6. `openGPX()` — creates a valid GPX (header + initial tail): `src/main.cpp:252`
7. `startGPS()` — starts UART2 for GPS at `GPS_BAUD`: `src/main.cpp:301`
8. `startWiFiAndWeb()` — STA or AP mode; starts HTTP server: `src/main.cpp:132`

Main loop (see `loop()` at `src/main.cpp:466`)
- Feed TinyGPS++ with incoming NMEA (`gps.encode(...)`).
- Once per `LOG_INTERVAL_MS`:
  - `maybeRenameLogsToTimestamp()` renames files to timestamped names when GPS time is valid: `src/main.cpp:332`
  - If `gps.location.isValid()` and recent, write one row to CSV (`logFixCSV()`) and one `<trkpt>` to GPX (`logFixGPX()`).
- Every 60 s, write a heartbeat (`[HB]`) with sats, HDOP, TinyGPS++ stats to the session log.
- Update a live state struct for the web UI and service HTTP requests.

---

## Files and Formats on SD

Folders and files
- At boot: creates `/session-0001/` (first free index) with:
  - `/session-0001/gpslog.csv`
  - `/session-0001/gpslog.gpx`
  - `/session-0001/session.log`
- After GPS time is valid: the folder is renamed once to `/session-YYYYMMDD_HHMMSS/`.

CSV (`src/main.cpp:235`, `src/main.cpp:396`)
- Header: `timestamp,lat,lon,alt_m,speed_kmh,course_deg,hdop,sats`
- Example row
  ```csv
  2025-01-01T12:30:45Z,48.856613,2.352222,35.2,27.53,182.4,0.98,10
  ```

GPX (`src/main.cpp:252`, `src/main.cpp:410`)
- Valid GPX v1.1 with `<trk><trkseg>`; tail is re‑written after every `<trkpt>` to keep file parseable on sudden power loss.
- Example track point
  ```xml
  <trkpt lat="48.856613" lon="2.352222"><ele>35.2</ele><time>2025-01-01T12:30:45Z</time></trkpt>
  ```

Session log (`src/main.cpp:288`)
- Contains `[BOOT]` lines with file paths, UART info; `[POINT]` lines per logged fix; `[HB]` every 60 s with sats, HDOP, and TinyGPS++ counters.

---

## Live Web Dashboard

- Modes
  - STA: Connects to your Wi‑Fi if credentials are set by build flags; IP shown in Serial.
  - AP: If STA fails or credentials are empty, starts an AP like `GPS-Tracker-XXXX` (password `gpslogger`); IP like `http://192.168.4.1/`.

- Endpoints
  - `/` — minimal, offline‑first page showing time/fix, lat/lon, speed, course, sats, HDOP; includes a Google Maps link.
  - `/gps.json` — JSON snapshot for scripts/integration.

- Sample `/gps.json`
  ```json
  {
    "hasFix": true,
    "ts": "2025-01-01T12:30:45Z",
    "lat": 48.856613,
    "lon": 2.352222,
    "alt": 35.2,
    "speed": 27.53,
    "course": 182.4,
    "sats": 10,
    "hdop": 0.98,
    "ageMs": 123
  }
  ```

Security note
- Don’t commit real Wi‑Fi credentials in source. Prefer `build_flags` in `platformio.ini`.
- To skip STA, set `-D WIFI_SSID=""` and rely on AP mode.

---

## Troubleshooting

SD mount fails
- Symptom: `[SD] Mount FAILED` or `f_mount failed: (13)` in Serial.
- Fix: Reformat to FAT32 (MBR). Many 64 GB cards ship as exFAT.
- Check `CS` pin (`GPIO5` default) and SPI wiring.

No points logged
- Session shows `[HB]` lines but no `[POINT]` → GPS has no valid fix.
- Move outdoors, ensure clear sky, verify GPS UART pins and `GPS_BAUD = 9600`.

Works on laptop power but flaky on bike
- Bike USB can be noisy/unstable. Use an automotive‑rated buck converter and add input capacitance. Keep wires short.

Wi‑Fi not visible
- If STA credentials are set but connect fails, it falls back to AP and prints the AP IP in Serial. If nothing appears, verify 2.4 GHz is enabled on your phone and reduce distance.

---

## Limitations & Roadmap

Current behavior
- Logs once per second when a valid fix is present.
- Renames files to a timestamp only once (when time becomes valid).
- Maintains GPX validity by rewriting the tail after each point; last second can still be lost on sudden power‑off.

Possible improvements
- Time‑based rotation (e.g., new file every 5–10 minutes) to limit worst‑case loss and keep files small.
- Distance/speed gating (e.g., log only if moved ≥ 10–15 m or speed ≥ 2 km/h) to reduce GPS drift at standstill.
- LED status (no fix vs logging), and reset‑reason logging.
- On‑device Wi‑Fi file browser or push‑to‑phone/cloud sync.
- UBX (u‑blox) binary logging for higher‑rate and compact data.
- Graceful shutdown capacitor or supercap to ensure final flush on ignition off.

---

## Code Pointers

- Pins and rates: `src/main.cpp:17`–`28`
- Wi‑Fi/web: `src/main.cpp:132`
- SD + filenames: `src/main.cpp:204`, `src/main.cpp:214`
- CSV/GPX open: `src/main.cpp:227`, `src/main.cpp:252`
- Per‑fix logging: `src/main.cpp:383`, `src/main.cpp:410`
- `setup()` and `loop()`: `src/main.cpp:441`, `src/main.cpp:466`

---

## Safety Note

Always secure wiring and the GPS antenna to avoid interference with riding or controls. Use appropriate fusing and weather‑proofing for on‑vehicle installations.

---

## License

This project’s license is not specified in the repository. Add one if you plan to publish or share binaries.
