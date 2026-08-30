# ESP32 motorcycle tracker

This firmware records GPS track points from a u-blox NEO-7M to a microSD card. It keeps an append-only raw NDJSON file and filtered CSV and GPX exports. The ESP32 may lose power with the ignition, so every accepted raw point and every export write is flushed immediately.

## Hardware

- ESP32 DevKitC
- u-blox NEO-7M GPS module
- SPI microSD module with a FAT32 card

| Module | Signal | ESP32 pin |
| --- | --- | --- |
| GPS | TX | GPIO16 |
| GPS | RX | GPIO17 |
| GPS | VCC | 3.3 V |
| GPS | GND | GND |
| SD | CS | GPIO5 |
| SD | SCK | GPIO18 |
| SD | MISO | GPIO19 |
| SD | MOSI | GPIO23 |
| SD | VCC | 3.3 V |
| SD | GND | GND |

Many large cards ship as exFAT. Reformat the card as FAT32 with an MBR partition table before using it with the ESP32 SD driver.

## Tracker configuration

Copy `include/tracker_config.example.h` to `include/tracker_config.h`. Replace the placeholder with one stable identifier for the physical tracker:

```cpp
#define TRACKER_ID "motorcycle-tracker-01"
```

Git ignores `include/tracker_config.h`. If it is missing, the firmware still builds with the placeholder from the example file. Do not use that placeholder for real data.

## When recording starts

Booting the tracker does not create a tracking session. A GPS fix qualifies as movement when all of these conditions hold:

- the location is valid and fresh;
- the GPS date and time are valid;
- speed is valid and greater than 2 km/h;
- HDOP is valid and no higher than 5.

The third consecutive qualifying fix starts a tracking session and becomes point 1. The first two fixes are not recorded. Any failed condition resets the count.

After the tracking session starts, every fresh, valid location becomes a raw track point. Stops, invalid speed, weak HDOP, and missing optional measurements remain in the raw record. Invalid and stale locations do not. CSV and GPX retain the movement filter and receive only points with valid speed greater than 2 km/h.

## Files on the SD card

The ESP32 stores a monotonic tracking session number in Preferences. A started session uses a directory such as `/session-0000000041/` with these files:

- `track-points.ndjson` contains every raw track point.
- `gpslog.csv` contains the filtered tabular export.
- `gpslog.gpx` contains the filtered GPX track.
- `session.log` contains session start information and one-minute health records. It does not duplicate track points.

Each NDJSON line has schema version, tracker ID, tracking session number, point number, GPS UTC time, coordinates, optional altitude, speed, course, HDOP, satellite count, and ESP32 uptime. Missing optional values are JSON `null`. Non-finite numeric values never enter the file.

The raw file is appended and flushed before the workflow reports a point as recorded. GPX writes replace and restore the closing XML tail, so the file remains parseable after each point.

## Build, test, and flash

Install PlatformIO, then run:

```sh
platformio test -e native
platformio run -e esp32dev
platformio run -e esp32dev -t upload
platformio device monitor -b 115200
```

The native test environment needs a C++ compiler on `PATH`. On Windows, PlatformIO's `platformio/toolchain-gccmingw32` package is sufficient. The host scenarios cover start qualification, reset conditions, post-start recording, export filtering, raw point numbering, JSON null handling, and failed raw writes. The ESP32 build is a separate check.

## Troubleshooting

If the SD card does not mount, check FAT32 formatting, GPIO5 chip select, and SPI wiring. If no session starts, move the GPS antenna outdoors and check the serial monitor for fresh fixes. A cold GPS fix can take several minutes.

Motorcycle USB power can be noisy. Use an automotive-rated 12 V to 5 V converter, short wiring, suitable fusing, and input filtering. Secure the tracker and antenna away from the controls before riding.
