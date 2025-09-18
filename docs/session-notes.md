# GPS Tracker – Session Notes

These notes summarize the setup, wiring, code behavior, and troubleshooting outcomes from this session. Keep this file updated as you iterate so future sessions can reference it quickly.

## Hardware

- Board: ESP32 DevKit (`[env:esp32dev]` in PlatformIO)
- GPS: u‑blox NEO‑7M (UART)
- SD: SPI microSD module

### Wiring

- GPS
  - GPS TX → ESP32 `GPIO16` (RX2)
  - GPS RX → ESP32 `GPIO17` (TX2)
  - 3V3 → 3.3V, GND → GND

- SD (SPI)
  - CS → `GPIO5`
  - CLK/SCK → `GPIO18`
  - DO/MISO/CIPO → `GPIO19`
  - DI/MOSI/COPI → `GPIO23`
  - 3V3 → 3.3V (or 5V if your module requires it), GND → GND

## Software

- Framework: Arduino (PlatformIO)
- Libraries
  - `TinyGPSPlus` (declared in `platformio.ini`)
  - `SD` from Arduino‑ESP32 core (no external `arduino-libraries/SD`)

### Key Files

- Logger: `src/main.cpp`
  - Pins/constants: `src/main.cpp:16` (GPS RX/TX), `src/main.cpp:20` (SD CS), `src/main.cpp:26` (`LOG_INTERVAL_MS`)
  - Logging functions: `logFixCSV()` `src/main.cpp:229`, `logFixGPX()` nearby
  - Timestamp rename: `maybeRenameLogsToTimestamp()` `src/main.cpp:178`
  - Session log: `openSession()` `src/main.cpp:134`; heartbeat in `loop()`

- Platform config: `platformio.ini`
  - Includes `TinyGPSPlus` in `lib_deps`

## Current Behavior (Dual Logger)

- Creates a session folder at SD root using the first free index:
  - Folder: `/session-0001/`
  - Files inside: `gpslog.csv`, `gpslog.gpx`, `session.log`

- Renames the session folder to a timestamp once GPS time is valid:
  - Example: `/session-20250101_123045/`

- Logs once per second when a valid fix is present:
  - CSV columns: `timestamp,lat,lon,alt_m,speed_kmh,course_deg,hdop,sats`
  - GPX track points: `<trkpt lat="…" lon="…"><ele>…</ele><time>…</time></trkpt>`
  - GPX file always remains valid (tail is re‑appended after each point)

- Session log contents:
  - `[BOOT]` lines with paths and UART info
  - `[POINT]` lines for each logged GPS fix (lat/lon/speed/hdop/sats)
  - `[HB]` heartbeat every 60 s with sats, HDOP, TinyGPS++ stats, counts

## Usage

- Build/upload: `pio run -t upload`
- Serial monitor: `pio device monitor` (115200). Press EN/Reset to re‑see boot logs.
- Files saved under the per‑session folder at SD root. If GPS time never locks, the folder keeps `session-000X`.

## Known Constraints & Tips

- GPS Fix Time: First cold fix can take 1–5 minutes (longer indoors). Place the antenna with clear sky view.
- SD Format: ESP32 `SD` driver requires FAT32 (MBR). exFAT (typical on 64 GB cards) causes `f_mount failed: (13)`; reformat to FAT32.
- Bike Power: Motorcycle USB ports can be noisy/unstable; prefer a proper 12V→5V buck (automotive‑rated), add input caps (e.g., 470–1000 µF + 0.1 µF), and keep wiring short.
- Importing to Maps: Prefer GPX for Google My Maps. Large sessions import better as GPX tracks than CSV markers. Consider splitting per ride.

## Tuning

- Log rate: `LOG_INTERVAL_MS` at `src/main.cpp:26` (default 1000 ms). 2000–5000 ms reduces file size.
- Pins: `GPS_RX_PIN/GPS_TX_PIN` at `src/main.cpp:16`; `SD_CS_PIN` at `src/main.cpp:20`.
- Filters: Currently logs any valid fix (`gps.location.isValid()` and recent). Optional speed/HDOP filters can be added.
- SPI Clock: For flaky modules, lower SD SPI clock via `SD.begin(SD_CS_PIN, SPI, 8000000)`.

## Troubleshooting

- No logs created:
  - Check SD root for `/session-####/session.log` or a timestamped folder.
  - If session log is missing → SD didn’t mount or board reset too early (power).
  - If session log exists with `[HB]` but no `[POINT]` → GPS never had a valid fix (antenna/sky view/filters).

- exFAT/FAT error:
  - Error: `[E][sd_diskio.cpp:806] sdcard_mount(): f_mount failed: (13)`
  - Solution: Reformat microSD to FAT32 (MBR). For >32 GB cards, use SD Association’s formatter or a tool that supports FAT32.

- Bike vs Laptop Power:
  - Works on laptop, fails on bike → unstable 5V from bike USB. Use a buck converter and input filtering; consider startup grace delay in code.

## Future Enhancements (Optional)

- Distance threshold (e.g., log only if moved ≥ 10–15 m) and/or speed threshold (e.g., ≥ 2 km/h)
- Keepalive point every 60–120 s to ensure file isn’t empty on short rides
- LED status (blink no fix, solid when logging)
- Record reset reason (`esp_reset_reason()`) at boot in session log
- Auto‑baud or brief NMEA echo at startup for field diagnostics
- On‑device simplified GPX alongside raw log for faster map imports

## Changelog (Session Summary)

1. Initial Serial/no‑logs issue: ensured baud match and added loop prints.
2. Implemented dual CSV + GPX logger; GPX tailing so files stay valid.
3. Numbered filenames per boot; later added timestamp rename on first valid GPS time.
4. Removed external SD library; rely on ESP32’s built‑in SD.
5. Corrected HDOP handling (`value()/100.0`).
6. Added session diagnostic log + 60 s heartbeat.
7. Resolved SD mount error due to exFAT on 64 GB card (reformatted to FAT32).
8. Investigated bike USB power stability and recommended hardware fixes.

---

If you change pins, file naming, or add filters, update this document accordingly for future reference.
