# ESP32 motorcycle tracker firmware

This firmware turns an ESP32, a u-blox NEO-7M GPS receiver, and a microSD card into a failure-tolerant motorcycle tracker. The SD card is the local record: recording does not depend on Wi-Fi, and confirmed delivery does not immediately remove the local copy.

The firmware uses these terms consistently:

- A **tracking session** begins after the motorcycle has a usable moving GPS fix and ends when the tracker loses power.
- A **track point** is one fresh GPS observation recorded during a tracking session.
- **Pending data** is local data whose remote receipt has not been confirmed.
- **Delivered data** is local data whose remote receipt has been confirmed and may still remain on the SD card.

## Hardware and wiring

Required hardware:

- ESP32 DevKitC (`esp32dev` PlatformIO board)
- u-blox NEO-7M GPS module
- SPI microSD module and a FAT32 card
- an automotive-rated, fused 12 V-to-5 V supply for installation on a motorcycle

Connect all grounds together. The UART signal names below are the GPS module's labels; transmit and receive cross at the ESP32.

| Module | Module signal | ESP32 connection |
| --- | --- | --- |
| NEO-7M | TX | GPIO16 (ESP32 RX) |
| NEO-7M | RX | GPIO17 (ESP32 TX) |
| NEO-7M | VCC | 3.3 V |
| NEO-7M | GND | GND |
| microSD | CS | GPIO5 |
| microSD | SCK | GPIO18 |
| microSD | MISO | GPIO19 |
| microSD | MOSI | GPIO23 |
| microSD | VCC | 3.3 V |
| microSD | GND | GND |

The GPS UART runs at 9600 baud. The USB serial monitor runs at 115200 baud. Format the card with one FAT32 partition and an MBR partition table; many larger cards ship as exFAT and will not mount with this setup.

Before installing the tracker, confirm that the particular GPS and microSD breakout boards accept 3.3 V power and logic. Keep wiring short, add suitable input filtering, and secure the tracker and antenna away from the motorcycle controls.

## Local settings and secrets

Copy the versioned example to the ignored local header:

```sh
cp include/tracker_config.example.h include/tracker_config.h
```

On PowerShell, the equivalent is:

```powershell
Copy-Item include/tracker_config.example.h include/tracker_config.h
```

Replace every placeholder in `include/tracker_config.h`:

```cpp
#define HOTSPOT_NAME "my-phone-hotspot"
#define HOTSPOT_PASSWORD "replace-with-hotspot-password"
#define UPLOAD_URL "https://uploads.example/v1/track-point-batches"
#define TRACKER_ID "motorcycle-tracker-01"
#define TRACKER_TOKEN "replace-with-bearer-token"
#define UPLOAD_ROOT_CA_CERTIFICATE                                      \
  "-----BEGIN CERTIFICATE-----\n"                                    \
  "...\n"                                                           \
  "-----END CERTIFICATE-----\n"
```

`UPLOAD_URL` must use HTTPS and end with exactly `/v1/track-point-batches`. `TRACKER_ID` must remain stable so retrying a request identifies the same data. Use the PEM-encoded root CA that validates the configured service; the firmware has no insecure TLS option.

`include/tracker_config.h` is ignored by Git. Keep the hotspot password, bearer token, tracker identity, upload address, and certificate out of commits and logs. The project falls back to the safe placeholder example when the local header is absent, which permits a build but does not produce a deployable configuration.

## Recording behavior

Boot alone does not create a tracking session. Once per second, a GPS fix qualifies to start one only when all of these are true:

- the location is valid and fresh;
- the GPS date and time are valid;
- speed is valid and greater than 2 km/h;
- HDOP is valid and no higher than 5.

The third consecutive qualifying fix starts a tracking session and becomes track point 1. The first two fixes are not recorded, and any failed condition resets the consecutive-fix count.

After a tracking session starts, every fresh, valid location becomes a raw track point. Stopped points, weak fixes, and missing optional measurements remain in that raw record; stale or invalid locations do not. The CSV and GPX exports keep their movement filter and receive a point only when its speed is valid and greater than 2 km/h.

Each accepted raw track point is appended to NDJSON and flushed before the workflow treats it as recorded. If the body or its terminating newline is short, the firmware closes that file and stops recording to it. It never adds a newline after a short body. On the next boot, recovery ignores the unterminated tail and delivers the earlier complete lines. Each CSV write is also flushed. GPX writes restore the closing XML after every point so the file remains parseable if ignition power disappears without warning. A monotonic tracking session number is kept in ESP32 Preferences, while point numbers restart at 1 for each tracking session.

## SD card record

A started tracking session gets a directory such as `/session-0000000041/`. Its recording files are created when the tracking session starts, and its delivery-state files appear when confirmations are persisted:

| File | Purpose |
| --- | --- |
| `track-points.ndjson` | Append-only local record of every raw track point |
| `gpslog.csv` | Movement-filtered tabular export |
| `gpslog.gpx` | Movement-filtered GPX export kept parseable after every write |
| `session.log` | Boot/reset details, session start, one-minute health records, Wi-Fi state, delivery attempts/results, cleanup, and faults |
| `delivery-state.log` | Append-only confirmed track-point progress |
| `diagnostic-delivery-state.log` | Confirmation that the completed diagnostic log was delivered |

Every NDJSON object carries schema version, stable tracker ID, tracking session number, point number, GPS UTC time, latitude, longitude, optional altitude, optional speed, optional course, optional HDOP, optional satellite count, and ESP32 uptime in milliseconds. Missing optional values are JSON `null`; non-finite numbers are never serialized.

`session.log` does not copy individual track points. Messages are also printed to the USB serial monitor. Boot messages produced before a tracking session starts are buffered and written when the session's log opens.

## HTTPS delivery contract

A background task sends pending data through the configured phone hotspot without making the GPS loop wait for network work. SD access is protected by a shared mutex and held only while reading or updating local files. At most 30 ordered NDJSON lines are copied into memory, and the SD file is closed before DNS, Wi-Fi, TLS, or HTTP work begins.

Active tracking sessions expose complete 30-point batches. At the next boot, previous tracking sessions are inactive, so their final batch may contain fewer than 30 points. Across tracking sessions, the oldest eligible pending data is selected first.

The checked boundary for track points is an HTTPS `POST` to `/v1/track-point-batches` with bearer authentication, `Content-Type: application/x-ndjson`, and these headers:

- `X-Track-Point-Schema-Version`
- `X-Tracker-ID`
- `X-Tracking-Session-Number`
- `X-First-Point-Number`
- `X-Last-Point-Number`

The body contains ordered NDJSON objects whose tracker, tracking session, schema, and point numbers match those headers. A response marks data delivered only when it is 2xx JSON with the matching `tracker_id` and `tracking_session_number`, and `highest_stored_point_number` covers the entire sent range. Stable identities and point numbers make repeated requests safe at this contract boundary.

Completed diagnostic logs are delivered on a later boot to the same HTTPS service origin at `/v1/diagnostic-logs`. That request uses bearer authentication, stable tracker and tracking session headers, and an idempotency key. A log becomes delivered only after a matching 2xx confirmation says it was stored. This repository defines and checks only these HTTPS request and confirmation boundaries; the service's storage implementation is outside the firmware.

## Retry and restart recovery

Wi-Fi failure, a combined DNS-or-connection failure, TLS setup or certificate failure, timeout, authentication rejection, another 4xx or 5xx HTTP rejection, another transport failure, malformed JSON, mismatched identity, an incomplete confirmation, or failure to persist a confirmation leaves the affected data pending. Diagnostics report those categories, but do not distinguish DNS failure from connection failure. Point batches and diagnostic logs keep separate failure counts. Each failure advances that data type through delays of 15, 30, 60, and 120 seconds, then 300 seconds for every later failure; when both types fail in one pass, the background task waits for the shorter delay before checking pending data again. A confirmed and locally persisted success resets that data type's failure count.

On every boot, the firmware scans tracking-session directories in number order. Complete NDJSON lines determine the highest recorded point; a trailing partial line is not counted. The last valid, checksummed delivery-progress record determines the highest confirmed point. Missing, damaged, mismatched, or impossible progress is ignored, so the firmware resends from the last safe point. It never infers delivery from an attempted request.

All recovered tracking sessions are inactive. Their pending track points, including a final partial batch, and their completed pending diagnostic logs are eligible for delivery. Confirmed data remains confirmed, and safe resends are harmless because the HTTPS boundary uses stable identities.

## Storage cleanup

Cleanup begins when SD use reaches 80%. It considers tracking sessions oldest first and removes whole directories until use is strictly below 70%.

A directory may be removed only when its tracking session is inactive, all of its track points are delivered, and its completed diagnostic log is delivered. The active tracking session and every tracking session with pending data are protected. If protected data prevents reaching the target, the firmware records that condition and continues recording while physical space remains; it never deletes the only local copy of pending data.

## Build, host tests, flash, and monitor

Install PlatformIO Core, then run these commands from the repository root:

```sh
platformio test -e native
platformio run -e esp32dev
platformio run -e esp32dev -t upload
platformio device monitor -b 115200
```

`platformio test -e native` runs the full host suite for recording, batch scheduling, HTTPS contracts, retry, restart recovery, diagnostic delivery, and storage cleanup. The native environment needs a C++ compiler on `PATH`. On Windows, the compiler bundled by PlatformIO can be exposed for the current PowerShell session with:

```powershell
$env:Path = "$env:USERPROFILE\.platformio\packages\toolchain-gccmingw32\bin;$env:Path"
platformio test -e native
```

`platformio run -e esp32dev` separately compiles the Arduino firmware for the configured ESP32 board. Connect the ESP32 over USB before the upload and monitor commands. If PlatformIO cannot select the right serial port, list ports with `platformio device list` and pass `--upload-port <port>` when flashing or `--port <port>` when monitoring.

## Fault checks

Use the 115200-baud serial monitor and the current tracking session's `session.log` together:

- No `[SD] Mount OK`: check FAT32/MBR formatting, 3.3 V power, common ground, CS GPIO5, and SPI pins 18/19/23.
- No fresh fixes or no tracking session: place the GPS antenna outdoors, check its TX connection to GPIO16 and 9600-baud output, then verify valid UTC, speed above 2 km/h, and HDOP no higher than 5 for three consecutive checks. A cold fix can take several minutes.
- `[SD] Raw point ... append/flush FAILED`: stop relying on that card until its wiring, write protection, filesystem, free space, and health have been checked. A short body is left without a newline, the file is closed, and the failed append is not treated as a recorded track point. Recovery ignores that unterminated tail after reboot.
- Wi-Fi connection failures: verify hotspot name/password, that the phone hotspot is enabled, and that the ESP32 is in range. Local recording should continue and the data should remain pending.
- DNS-or-connection, authentication, HTTP, TLS, transport, timeout, or malformed/mismatched confirmation results: check the hotspot's internet and DNS service, token, URL path, root CA, service availability, and response contract. Do not bypass certificate validation.
- `[RECOVERY] ... safe-resend`: delivery progress was absent or damaged; leave the local files intact and allow the stable range to be retried.
- Cleanup cannot reach below 70%: one or more tracking sessions still contain pending data. Restore delivery rather than manually deleting those directories.

## Hardware smoke test

Run this short release check with the target ESP32, NEO-7M, microSD module, FAT32 card, phone hotspot, and a test HTTPS service that implements the checked contract:

- [ ] Cold-boot with the hotspot unavailable. Confirm SD mount and GPS UART messages, and confirm stationary fixes create no tracking session.
- [ ] Move with three consecutive qualifying fixes. Confirm the third fix creates a new tracking-session directory as track point 1.
- [ ] Confirm raw NDJSON is written and flushed, and moving points appear in valid CSV and GPX files. Then stop or degrade fix quality and confirm fresh locations remain raw track points while the filtered exports skip stopped points.
- [ ] With the hotspot disabled, create two tracking sessions by recording points, cutting power, and rebooting between them. Give the older tracking session more than 30 points. Enable the hotspot and confirm its lowest pending 30-point range is sent before any range from the newer tracking session. Confirm the expected authentication and metadata, a matching success response, and persisted delivered progress without interruption to recording.
- [ ] Disable the hotspot during recording. Then induce representative DNS-or-connection, TLS, timeout, authentication, other HTTP, transport, and malformed or mismatched confirmation failures with the test service. Confirm diagnostics use the documented categories, retries follow the documented delays, local recording continues while the raw file remains writable, and the affected data remains pending.
- [ ] Cut ignition power with a final batch shorter than 30 points, restore power, and confirm no false tracking session is created while stationary. Confirm restart recovery sends the older final partial batch and the completed diagnostic log.
- [ ] Make the service handle the same stable batch twice. Confirm the repeated request succeeds without duplicate track points and local delivery progress does not move incorrectly.
- [ ] Exercise cleanup with test data at or above 80% card use. Confirm only the oldest fully delivered, inactive tracking-session directories are removed, cleanup stops below 70%, and pending data is never deleted.
- [ ] Inspect the serial monitor and delivered diagnostic log for boot/reset, health, Wi-Fi, delivery, retry, cleanup, and fault evidence, with no duplicated per-point diagnostic entries.

Do not road-test until the tracker, antenna, power converter, fuse, and wiring are mechanically secured and cannot interfere with steering, braking, or other controls.
