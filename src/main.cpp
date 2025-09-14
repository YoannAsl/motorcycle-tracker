// ESP32 + NEO-7M GPS logger to SD (CSV route log)
// Wiring (adjust pins to your board):
// - GPS NEO-7M: VCC->3V3, GND->GND, TX->GPIO16 (GPS_RX_PIN), RX->GPIO17 (GPS_TX_PIN)
// - SD module (SPI): CS->GPIO5, SCK->GPIO18, MISO->GPIO19, MOSI->GPIO23, VCC->3V3, GND->GND

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <TinyGPSPlus.h>
#include <WiFi.h>
#include <WebServer.h>

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
static char csvPath[32];
static char gpxPath[32];
static char sessionPath[32];

HardwareSerial GPS_Serial(1);
TinyGPSPlus gps;
File csvFile;
File gpxFile;
static bool filesRenamedToTimestamp = false;
File sessionFile;
static uint32_t pointsLogged = 0;

// ===== Live monitoring over Wi‑Fi (phone) =====
#ifndef WIFI_SSID
#define WIFI_SSID "Numericable-a82d-5g"   // set to your Wi‑Fi SSID to use STA mode
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "l11m99y7zcxf"   // set to your Wi‑Fi password
#endif

static WebServer server(80);
static bool wifiActive = false;
static bool wifiIsAP = false;
static IPAddress wifiIP;

struct GpsLiveState {
  bool hasFix = false;
  double lat = NAN;
  double lon = NAN;
  double alt = NAN;
  double spd = NAN;
  double crs = NAN;
  double hdop = NAN;
  uint32_t sats = 0;
  uint32_t lastUpdateMs = 0;
  String ts; // ISO8601 timestamp or placeholder
};
static GpsLiveState gState;

// Simple embedded page (offline-first, no external CDN dependencies)
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>ESP32 GPS Live</title>
  <style>
    body { font-family: system-ui, Arial, sans-serif; margin: 0; padding: 12px; }
    h1 { font-size: 18px; margin: 0 0 8px; }
    #grid { display: grid; grid-template-columns: auto 1fr; gap: 6px 12px; align-items: center; }
    .mono { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }
    .pill { display: inline-block; padding: 2px 8px; border-radius: 999px; background:#eee; }
    footer { color:#666; font-size: 12px; margin-top: 8px; }
    a.btn { background:#0b5; color:white; text-decoration:none; padding:6px 10px; border-radius:6px; display:inline-block; }
  </style>
</head>
<body>
  <h1>ESP32 GPS Live <span id="fix" class="pill">—</span></h1>
  <div id="grid">
    <div>Time (UTC)</div><div id="ts" class="mono">—</div>
    <div>Latitude</div><div id="lat" class="mono">—</div>
    <div>Longitude</div><div id="lon" class="mono">—</div>
    <div>Speed</div><div id="spd" class="mono">—</div>
    <div>Course</div><div id="crs" class="mono">—</div>
    <div>Satellites</div><div id="sats" class="mono">—</div>
    <div>HDOP</div><div id="hdop" class="mono">—</div>
    <div>Age</div><div id="age" class="mono">—</div>
  </div>
  <p><a id="gm" class="btn" href="#" target="_blank">Open in Google Maps</a></p>
  <footer>Offline‑first: page loads even in ESP32 AP without Internet. Use the Google Maps link when you have Internet.</footer>
  <script>
  async function tick(){
    try{
      const r = await fetch('/gps.json',{cache:'no-store'});
      const j = await r.json();
      document.getElementById('ts').textContent = j.ts || '—';
      document.getElementById('fix').textContent = j.hasFix? 'FIX' : 'NO FIX';
      document.getElementById('fix').style.background = j.hasFix? '#0b5' : '#c33';
      document.getElementById('sats').textContent = j.sats ?? '—';
      document.getElementById('hdop').textContent = j.hdop ?? '—';
      document.getElementById('age').textContent = (j.ageMs!=null? (j.ageMs+' ms') : '—');
      const latEl = document.getElementById('lat');
      const lonEl = document.getElementById('lon');
      const spdEl = document.getElementById('spd');
      const crsEl = document.getElementById('crs');
      if(j.hasFix && j.lat!=null && j.lon!=null){
        latEl.textContent = j.lat.toFixed(6);
        lonEl.textContent = j.lon.toFixed(6);
        spdEl.textContent = (j.speed!=null? (j.speed.toFixed(2)+' km/h') : '—');
        crsEl.textContent = (j.course!=null? (j.course.toFixed(1)+'°') : '—');
        document.getElementById('gm').href = `https://maps.google.com/?q=${j.lat},${j.lon}`;
      } else {
        latEl.textContent = lonEl.textContent = spdEl.textContent = crsEl.textContent = '—';
      }
    }catch(e){ /* ignore */ }
  }
  setInterval(tick, 1000);
  tick();
  </script>
</body>
</html>
)rawliteral";

static void startWiFiAndWeb() {
  // Try Station mode if credentials provided
  if (strlen(WIFI_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("[WiFi] Connecting to '%s'...\n", WIFI_SSID);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      wifiActive = true;
      wifiIsAP = false;
      wifiIP = WiFi.localIP();
      Serial.printf("[WiFi] Connected. IP: %s\n", wifiIP.toString().c_str());
    } else {
      Serial.println("[WiFi] STA connect timeout, falling back to AP");
    }
  }

  // Fallback to Access Point so phone can connect anywhere
  if (!wifiActive) {
    WiFi.mode(WIFI_AP);
    String ssid = String("GPS-Tracker-") + String((uint32_t)ESP.getEfuseMac(), HEX).substring(4);
    const char* pass = "gpslogger"; // simple default; change if desired
    bool ok = WiFi.softAP(ssid.c_str(), pass);
    wifiActive = ok;
    wifiIsAP = true;
    wifiIP = WiFi.softAPIP();
    Serial.printf("[WiFi] AP %s (%s). IP: %s, pass: %s\n", ok?"started":"FAILED", ssid.c_str(), wifiIP.toString().c_str(), pass);
  }

  if (wifiActive) {
    server.on("/", []() {
      server.send_P(200, "text/html", INDEX_HTML);
    });
    server.on("/ping", []() { server.send(200, "text/plain", "pong"); });
    server.on("/gps.json", []() {
      String s; s.reserve(256);
      s += F("{");
      s += F("\"hasFix\":"); s += (gState.hasFix ? F("true") : F("false"));
      s += F(",\"ts\":\""); s += gState.ts; s += F("\"");
      s += F(",\"lat\":"); s += (gState.hasFix ? String(gState.lat, 6) : String(F("null")));
      s += F(",\"lon\":"); s += (gState.hasFix ? String(gState.lon, 6) : String(F("null")));
      s += F(",\"alt\":"); s += (!isnan(gState.alt) ? String(gState.alt, 1) : String(F("null")));
      s += F(",\"speed\":"); s += (!isnan(gState.spd) ? String(gState.spd, 2) : String(F("null")));
      s += F(",\"course\":"); s += (!isnan(gState.crs) ? String(gState.crs, 1) : String(F("null")));
      s += F(",\"sats\":"); s += String(gState.sats);
      s += F(",\"hdop\":"); s += (!isnan(gState.hdop) ? String(gState.hdop, 2) : String(F("null")));
      s += F(",\"ageMs\":"); s += String((unsigned long)(millis() - gState.lastUpdateMs));
      s += F("}");
      server.send(200, "application/json", s);
    });
    server.onNotFound([](){ server.send(404, "text/plain", "Not found"); });
    server.begin();
    Serial.printf("[HTTP] Web server on http://%s/\n", wifiIP.toString().c_str());
    if (wifiIsAP) {
      Serial.println("[HTTP] Note: Map tiles may not load without Internet; data still live");
    }
  }
}

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
  // Find the first free index: gpslog-0001.csv / .gpx
  for (int i = 1; i <= 9999; ++i) {
    snprintf(csvPath, sizeof(csvPath), "/gpslog-%04d.csv", i);
    snprintf(gpxPath, sizeof(gpxPath), "/gpslog-%04d.gpx", i);
    snprintf(sessionPath, sizeof(sessionPath), "/session-%04d.log", i);
    if (!SD.exists(csvPath) && !SD.exists(gpxPath) && !SD.exists(sessionPath)) {
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

  char newCsv[40];
  char newGpx[40];
  char newSes[40];
  snprintf(newCsv, sizeof(newCsv), "/gpslog-%s.csv", base);
  snprintf(newGpx, sizeof(newGpx), "/gpslog-%s.gpx", base);
  snprintf(newSes, sizeof(newSes), "/session-%s.log", base);

  if (SD.exists(newCsv) || SD.exists(newGpx) || SD.exists(newSes)) {
    for (int i = 1; i <= 99; ++i) {
      snprintf(newCsv, sizeof(newCsv), "/gpslog-%s-%02d.csv", base, i);
      snprintf(newGpx, sizeof(newGpx), "/gpslog-%s-%02d.gpx", base, i);
      snprintf(newSes, sizeof(newSes), "/session-%s-%02d.log", base, i);
      if (!SD.exists(newCsv) && !SD.exists(newGpx) && !SD.exists(newSes)) break;
      if (i == 99) return; // give up if too many collisions
    }
  }

  if (csvFile) csvFile.flush();
  if (gpxFile) gpxFile.flush();
  if (sessionFile) sessionFile.flush();
  if (csvFile) csvFile.close();
  if (gpxFile) gpxFile.close();
  if (sessionFile) sessionFile.close();

  bool okCsv = SD.rename(csvPath, newCsv);
  bool okGpx = SD.rename(gpxPath, newGpx);
  bool okSes = SD.rename(sessionPath, newSes);

  if (okCsv) strncpy(csvPath, newCsv, sizeof(csvPath));
  if (okGpx) strncpy(gpxPath, newGpx, sizeof(gpxPath));
  if (okSes) strncpy(sessionPath, newSes, sizeof(sessionPath));

  // Reopen for continued logging
  csvFile = SD.open(csvPath, FILE_APPEND);
  gpxFile = SD.open(gpxPath, FILE_WRITE);
  sessionFile = SD.open(sessionPath, FILE_WRITE);
  if (!csvFile) Serial.println("[SD] Reopen CSV after rename FAILED");
  if (!gpxFile) Serial.println("[SD] Reopen GPX after rename FAILED");
  if (!sessionFile) Serial.println("[SD] Reopen session after rename FAILED");

  filesRenamedToTimestamp = okCsv && okGpx && okSes;
  if (filesRenamedToTimestamp) {
    Serial.printf("[SD] Renamed to timestamp: %s | %s | %s\n", csvPath, gpxPath, sessionPath);
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

  // Start Wi‑Fi + web server for phone live view
  startWiFiAndWeb();
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

  // Update live state (for web UI)
  gState.sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
  gState.hdop = gps.hdop.isValid() ? (gps.hdop.value() / 100.0) : NAN;
  if (gps.location.isValid() && gps.location.age() < 2000) {
    gState.hasFix = true;
    gState.lat = gps.location.lat();
    gState.lon = gps.location.lng();
    gState.alt = gps.altitude.isValid() ? gps.altitude.meters() : NAN;
    gState.spd = gps.speed.isValid() ? gps.speed.kmph() : NAN;
    gState.crs = gps.course.isValid() ? gps.course.deg() : NAN;
    gState.ts = isoTimestampUTC();
    gState.lastUpdateMs = now;
  } else {
    gState.hasFix = false;
    gState.ts = isoTimestampUTC();
  }

  // Service HTTP requests
  if (wifiActive) {
    server.handleClient();
  }
}
