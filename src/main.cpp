// ESP32 + NEO-7M GPS logger to SD (CSV route log)
// Wiring (adjust pins to your board):
// - GPS NEO-7M: VCC->3V3, GND->GND, TX->GPIO16 (GPS_RX_PIN), RX->GPIO17 (GPS_TX_PIN)
// - SD module (SPI): CS->GPIO5, SCK->GPIO18, MISO->GPIO19, MOSI->GPIO23, VCC->3V3, GND->GND

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <TinyGPSPlus.h>

// Serial monitor
static const uint32_t USB_BAUD = 115200;

// GPS UART settings (NEO-7M default is 9600 baud)
static const uint32_t GPS_BAUD = 9600;
static const int GPS_RX_PIN = 16; // ESP32 RX (connect to GPS TX)
static const int GPS_TX_PIN = 17; // ESP32 TX (connect to GPS RX)

// SD card SPI pins and CS (adjust CS if needed)
static const int SD_CS_PIN = 5;
static const int SD_SCK_PIN = 18;
static const int SD_MISO_PIN = 19;
static const int SD_MOSI_PIN = 23;

// Log rate and filenames
static const uint32_t LOG_INTERVAL_MS = 1000; // 1 Hz route logging

// Per-session directory at SD root
static char sessionDir[64];      // e.g., "/session-0001" then "/session-YYYYMMDD_HHMMSS"
static char csvPath[96];         // <sessionDir>/gpslog.csv
static char gpxPath[96];         // <sessionDir>/gpslog.gpx
static char sessionPath[96];     // <sessionDir>/session.log

HardwareSerial GPS_Serial(1);
TinyGPSPlus gps;
File csvFile;
File gpxFile;
static bool filesRenamedToTimestamp = false;
File sessionFile;
static uint32_t pointsLogged = 0;

// Forward declarations for functions used before definition
static String isoTimestampUTC();

static void waitForSerial() {
  Serial.begin(USB_BAUD);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) { delay(10); }
  delay(100);
}

static bool initSD() {
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("[SD] Mount FAILED");
    return false;
  }
  Serial.println("[SD] Mount OK");
  return true;
}

static bool nextLogNames() {
  // Create a fresh session directory at SD root using the first free index
  for (int i = 1; i <= 9999; ++i) {
    snprintf(sessionDir, sizeof(sessionDir), "/session-%04d", i);
    if (!SD.exists(sessionDir)) {
      if (!SD.mkdir(sessionDir)) {
        Serial.printf("[SD] mkdir %s FAILED\n", sessionDir);
        return false;
      }
      // Place files inside the session directory with fixed names
      snprintf(csvPath, sizeof(csvPath), "%s/gpslog.csv", sessionDir);
      snprintf(gpxPath, sizeof(gpxPath), "%s/gpslog.gpx", sessionDir);
      snprintf(sessionPath, sizeof(sessionPath), "%s/session.log", sessionDir);
      Serial.printf("[SD] Session dir -> %s\n", sessionDir);
      return true;
    }
  }
  return false;
}

static bool openCSV() {
  bool exists = SD.exists(csvPath);
  csvFile = SD.open(csvPath, FILE_WRITE); // append by default
  if (!csvFile) {
    Serial.println("[SD] Open CSV FAILED");
    return false;
  }
  if (!exists) {
    csvFile.println("timestamp,lat,lon,alt_m,speed_kmh,course_deg,hdop,sats");
    csvFile.flush();
  }
  Serial.printf("[SD] CSV -> %s\n", csvPath);
  return true;
}

static const char GPX_HEADER_PREFIX[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<gpx version=\"1.1\" creator=\"ESP32 GPS Logger\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n";
static const char GPX_META_TIME_PRE[] = "  <metadata>\n    <time>";
static const char GPX_META_TIME_POST[] = "</time>\n  </metadata>\n";
static const char GPX_TRK_OPEN_PRE[] = "  <trk>\n    <name>";
static const char GPX_TRK_OPEN_POST[] = "</name>\n    <trkseg>\n";
static const char GPX_TAIL[] = "    </trkseg>\n  </trk>\n</gpx>\n";
static const size_t GPX_TAIL_LEN = sizeof(GPX_TAIL) - 1; // exclude null

static bool openGPX() {
  bool exists = SD.exists(gpxPath);
  gpxFile = SD.open(gpxPath, FILE_WRITE);
  if (!gpxFile) {
    Serial.println("[SD] Open GPX FAILED");
    return false;
  }
  if (!exists) {
    // Write header and an initial tail so file is always valid GPX.
    gpxFile.print(GPX_HEADER_PREFIX);
    // Use valid ISO time if available; otherwise a fixed placeholder to keep GPX schema-valid
    String ts = (gps.date.isValid() && gps.time.isValid()) ? isoTimestampUTC() : String("1970-01-01T00:00:00Z");
    gpxFile.print(GPX_META_TIME_PRE);
    gpxFile.print(ts);
    gpxFile.print(GPX_META_TIME_POST);
    gpxFile.print(GPX_TRK_OPEN_PRE);
    gpxFile.print(ts);
    gpxFile.print(GPX_TRK_OPEN_POST);
    gpxFile.print(GPX_TAIL);
    gpxFile.flush();
  }
  Serial.printf("[SD] GPX -> %s\n", gpxPath);
  return true;
}

// Simple helper to mirror logs to Serial and session file
#include <stdarg.h>
static void logBoth(const char* fmt, ...) {
  char buf[256];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
  if (sessionFile) { sessionFile.print(buf); sessionFile.flush(); }
}

static bool openSession() {
  sessionFile = SD.open(sessionPath, FILE_WRITE);
  if (!sessionFile) {
    Serial.println("[SD] Open session log FAILED");
    return false;
  }
  logBoth("[BOOT] Session log -> %s\n", sessionPath);
  logBoth("[BOOT] CSV path -> %s\n", csvPath);
  logBoth("[BOOT] GPX path -> %s\n", gpxPath);
  logBoth("[BOOT] GPS UART RX=%d TX=%d baud=%lu\n", GPS_RX_PIN, GPS_TX_PIN, (unsigned long)GPS_BAUD);
  return true;
}

static void startGPS() {
  GPS_Serial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.printf("[GPS] UART on RX=%d TX=%d @ %lu baud\n", GPS_RX_PIN, GPS_TX_PIN, (unsigned long)GPS_BAUD);
}

static String isoTimestampUTC() {
  if (gps.date.isValid() && gps.time.isValid()) {
    char buf[25];
    uint16_t y = gps.date.year();
    uint8_t mo = gps.date.month();
    uint8_t d = gps.date.day();
    uint8_t h = gps.time.hour();
    uint8_t mi = gps.time.minute();
    uint8_t s = gps.time.second();
    snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ", y, mo, d, h, mi, s);
    return String(buf);
  }
  // Fallback to millis since boot
  return String("BOOT+") + String(millis());
}

static bool buildTimestampBase(char* out, size_t outLen) {
  if (gps.date.isValid() && gps.time.isValid()) {
    snprintf(out, outLen, "%04u%02u%02u_%02u%02u%02u",
             gps.date.year(), gps.date.month(), gps.date.day(),
             gps.time.hour(), gps.time.minute(), gps.time.second());
    return true;
  }
  return false;
}

static void maybeRenameLogsToTimestamp() {
  if (filesRenamedToTimestamp) return;
  char base[24];
  if (!buildTimestampBase(base, sizeof(base))) return; // wait until time available

  // Build target directory name at SD root: /session-YYYYMMDD_HHMMSS (avoid collisions with -NN suffix)
  char newDir[64];
  snprintf(newDir, sizeof(newDir), "/session-%s", base);
  if (SD.exists(newDir)) {
    for (int i = 1; i <= 99; ++i) {
      snprintf(newDir, sizeof(newDir), "/session-%s-%02d", base, i);
      if (!SD.exists(newDir)) break;
      if (i == 99) return; // too many collisions
    }
  }

  // Close files before renaming the directory
  if (csvFile) { csvFile.flush(); csvFile.close(); }
  if (gpxFile) { gpxFile.flush(); gpxFile.close(); }
  if (sessionFile) { sessionFile.flush(); sessionFile.close(); }

  bool okDir = SD.rename(sessionDir, newDir);
  if (!okDir) {
    Serial.printf("[SD] Rename dir %s -> %s FAILED\n", sessionDir, newDir);
    return;
  }

  // Update paths to new directory and reopen
  strncpy(sessionDir, newDir, sizeof(sessionDir));
  snprintf(csvPath, sizeof(csvPath), "%s/gpslog.csv", sessionDir);
  snprintf(gpxPath, sizeof(gpxPath), "%s/gpslog.gpx", sessionDir);
  snprintf(sessionPath, sizeof(sessionPath), "%s/session.log", sessionDir);

  csvFile = SD.open(csvPath, FILE_APPEND);
  gpxFile = SD.open(gpxPath, FILE_WRITE);
  sessionFile = SD.open(sessionPath, FILE_WRITE);
  if (!csvFile) Serial.println("[SD] Reopen CSV after dir rename FAILED");
  if (!gpxFile) Serial.println("[SD] Reopen GPX after dir rename FAILED");
  if (!sessionFile) Serial.println("[SD] Reopen session after dir rename FAILED");

  filesRenamedToTimestamp = (bool)okDir;
  if (filesRenamedToTimestamp) {
    Serial.printf("[SD] Session folder timestamped: %s\n", sessionDir);
  }
}

static void logFixCSV() {
  if (!csvFile) return;
  double lat = gps.location.lat();
  double lon = gps.location.lng();
  double alt = gps.altitude.isValid() ? gps.altitude.meters() : NAN;
  double spd = gps.speed.isValid() ? gps.speed.kmph() : NAN;
  double crs = gps.course.isValid() ? gps.course.deg() : NAN;
  double hdop = gps.hdop.isValid() ? (gps.hdop.value() / 100.0) : NAN; // TinyGPS++ hdop.value() is in hundredths
  uint32_t sats = gps.satellites.isValid() ? gps.satellites.value() : 0;

  String ts = isoTimestampUTC();

  // CSV line with fixed decimals for map tools
  csvFile.printf("%s,%.6f,%.6f,%.1f,%.2f,%.1f,%.2f,%lu\n",
                 ts.c_str(), lat, lon, alt, spd, crs, hdop, (unsigned long)sats);
  csvFile.flush();

  Serial.printf("[LOG] %s lat=%.6f lon=%.6f spd=%.1fkm/h sats=%lu\n",
                ts.c_str(), lat, lon, spd, (unsigned long)sats);
  if (sessionFile) {
    sessionFile.printf("[POINT] %s lat=%.6f lon=%.6f spd=%.1f hdop=%.2f sats=%lu\n",
                       ts.c_str(), lat, lon, spd, hdop, (unsigned long)sats);
    sessionFile.flush();
  }
  pointsLogged++;
}

static void logFixGPX() {
  if (!gpxFile) return;

  // Construct a <trkpt> entry
  char buf[256];
  double lat = gps.location.lat();
  double lon = gps.location.lng();
  bool haveAlt = gps.altitude.isValid();
  double alt = gps.altitude.meters();
  String ts = isoTimestampUTC();

  if (haveAlt) {
    snprintf(buf, sizeof(buf),
             "      <trkpt lat=\"%.6f\" lon=\"%.6f\"><ele>%.1f</ele><time>%s</time></trkpt>\n",
             lat, lon, alt, ts.c_str());
  } else {
    snprintf(buf, sizeof(buf),
             "      <trkpt lat=\"%.6f\" lon=\"%.6f\"><time>%s</time></trkpt>\n",
             lat, lon, ts.c_str());
  }

  // Keep GPX always valid by overwriting the tail and re-appending it
  size_t sizeNow = gpxFile.size();
  if (sizeNow >= GPX_TAIL_LEN) {
    gpxFile.seek(sizeNow - GPX_TAIL_LEN);
  }
  gpxFile.print(buf);
  gpxFile.print(GPX_TAIL);
  gpxFile.flush();
}

void setup() {
  waitForSerial();
  Serial.println();
  Serial.println("=== ESP32 GPS SD Logger ===");

  if (!initSD()) {
    Serial.println("Fatal: SD not available");
    // Keep running so user can see error
  }
  if (!nextLogNames()) {
    Serial.println("Fatal: could not allocate log filenames");
  } else {
    // Open session first so we can capture subsequent events
    if (!openSession()) Serial.println("Fatal: could not open session log");
    if (!openCSV()) logBoth("Fatal: could not open CSV file\n");
    if (!openGPX()) logBoth("Fatal: could not open GPX file\n");
  }

  startGPS();
  logBoth("[BOOT] Logger started. Interval=%lu ms\n", (unsigned long)LOG_INTERVAL_MS);
}

void loop() {
  // Feed TinyGPS++ with incoming NMEA
  while (GPS_Serial.available()) {
    gps.encode(GPS_Serial.read());
  }

  static uint32_t lastLog = 0;
  static uint32_t lastHeartbeat = 0;
  static uint32_t noFixReports = 0;
  uint32_t now = millis();

  // Log once per interval if we have a valid location fix
  if (now - lastLog >= LOG_INTERVAL_MS) {
    lastLog = now;
    // Rename files to timestamped names once GPS time is valid
    maybeRenameLogsToTimestamp();
    if (gps.location.isValid() && gps.location.age() < 2000) {
      logFixCSV();
      logFixGPX();
    } else {
      Serial.println("[GPS] No valid fix yet...");
      noFixReports++;
    }
  }

  // Heartbeat to session log every 60s for post-ride diagnostics
  if (now - lastHeartbeat >= 60000) { // 60,000 ms = 60s
    lastHeartbeat = now;
    uint32_t sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
    double hdop = gps.hdop.isValid() ? (gps.hdop.value() / 100.0) : NAN;
    logBoth("[HB] t=%.1fs sats=%lu hdop=%.2f chars=%lu fixSent=%lu points=%lu nofix=%lu\n",
            now / 1000.0,
            (unsigned long)sats,
            hdop,
            (unsigned long)gps.charsProcessed(),
            (unsigned long)gps.sentencesWithFix(),
            (unsigned long)pointsLogged,
            (unsigned long)noFixReports);
  }
}
