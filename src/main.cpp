// ESP32 + NEO-7M GPS logger to microSD.
// GPS: TX->GPIO16, RX->GPIO17. SD SPI: CS 5, SCK 18, MISO 19, MOSI 23.

#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <TinyGPSPlus.h>

#include "tracking_workflow.h"

#if __has_include("tracker_config.h")
#include "tracker_config.h"
#else
#include "tracker_config.example.h"
#endif

namespace {

const uint32_t USB_BAUD = 115200;
const uint32_t GPS_BAUD = 9600;
const int GPS_RX_PIN = 16;
const int GPS_TX_PIN = 17;
const int SD_CS_PIN = 5;
const int SD_SCK_PIN = 18;
const int SD_MISO_PIN = 19;
const int SD_MOSI_PIN = 23;
const uint32_t LOG_INTERVAL_MS = 1000;

const char GPX_HEADER[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<gpx version=\"1.1\" creator=\"ESP32 GPS Logger\" "
    "xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
    "  <trk><name>Motorcycle tracking session</name><trkseg>\n";
const char GPX_TAIL[] = "  </trkseg></trk>\n</gpx>\n";
const size_t GPX_TAIL_LENGTH = sizeof(GPX_TAIL) - 1;

HardwareSerial gpsSerial(1);
TinyGPSPlus gps;
File csvFile;
File gpxFile;
File diagnosticFile;
File rawPointFile;
char trackingSessionDirectory[48];
uint32_t rawPointsRecorded = 0;
uint32_t noFreshLocationCount = 0;

void logDiagnostic(const char* format, ...) {
  char buffer[256];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(buffer, sizeof(buffer), format, arguments);
  va_end(arguments);
  Serial.print(buffer);
  if (diagnosticFile) {
    diagnosticFile.print(buffer);
    diagnosticFile.flush();
  }
}

bool initializeSd() {
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("[SD] Mount FAILED");
    return false;
  }
  Serial.println("[SD] Mount OK");
  return true;
}

bool writeInitialExports() {
  char path[80];
  snprintf(path, sizeof(path), "%s/gpslog.csv", trackingSessionDirectory);
  csvFile = SD.open(path, FILE_WRITE);
  if (!csvFile) return false;
  csvFile.println("timestamp,lat,lon,alt_m,speed_kmh,course_deg,hdop,sats");
  csvFile.flush();

  snprintf(path, sizeof(path), "%s/gpslog.gpx", trackingSessionDirectory);
  gpxFile = SD.open(path, FILE_WRITE);
  if (!gpxFile) return false;
  gpxFile.print(GPX_HEADER);
  gpxFile.print(GPX_TAIL);
  gpxFile.flush();
  return true;
}

class Esp32TrackingStorage : public tracking::TrackingStorage {
 public:
  bool startTrackingSession(uint32_t& trackingSessionNumber) override {
    if (!SD.cardSize()) return false;

    Preferences preferences;
    if (!preferences.begin("tracker", false)) return false;
    uint32_t candidate = preferences.getUInt("session", 0) + 1;
    do {
      snprintf(trackingSessionDirectory, sizeof(trackingSessionDirectory),
               "/session-%010lu", static_cast<unsigned long>(candidate));
      if (!SD.exists(trackingSessionDirectory)) break;
      ++candidate;
    } while (candidate != 0);

    if (candidate == 0 || !SD.mkdir(trackingSessionDirectory)) {
      preferences.end();
      return false;
    }
    if (preferences.putUInt("session", candidate) != sizeof(candidate)) {
      preferences.end();
      return false;
    }
    preferences.end();

    char path[80];
    snprintf(path, sizeof(path), "%s/track-points.ndjson",
             trackingSessionDirectory);
    rawPointFile = SD.open(path, FILE_WRITE);
    if (!rawPointFile || !writeInitialExports()) return false;

    snprintf(path, sizeof(path), "%s/session.log", trackingSessionDirectory);
    diagnosticFile = SD.open(path, FILE_WRITE);
    if (!diagnosticFile) return false;

    trackingSessionNumber = candidate;
    logDiagnostic("[SESSION] Started number=%lu directory=%s\n",
                  static_cast<unsigned long>(candidate),
                  trackingSessionDirectory);
    return true;
  }

  bool appendAndFlushRawPoint(const tracking::TrackPoint&,
                              const std::string& ndjson) override {
    if (!rawPointFile) return false;
    const size_t bodyWritten = rawPointFile.print(ndjson.c_str());
    const size_t newlineWritten = rawPointFile.print('\n');
    rawPointFile.flush();
    return bodyWritten == ndjson.size() && newlineWritten == 1;
  }
};

Esp32TrackingStorage storage;
tracking::TrackingWorkflow workflow(TRACKER_ID);

String pointTimestamp(const tracking::TrackPoint& point) {
  if (point.utcValid) return String(point.utc.c_str());
  return String("BOOT+") + String(point.uptimeMilliseconds);
}

void printOptional(File& file, const tracking::OptionalDouble& value,
                   unsigned int decimals) {
  if (value.valid) file.print(value.value, decimals);
}

void writeCsv(const tracking::TrackPoint& point) {
  if (!csvFile) return;
  csvFile.print(pointTimestamp(point));
  csvFile.print(',');
  csvFile.print(point.latitude, 6);
  csvFile.print(',');
  csvFile.print(point.longitude, 6);
  csvFile.print(',');
  printOptional(csvFile, point.altitudeMeters, 1);
  csvFile.print(',');
  printOptional(csvFile, point.speedKmh, 2);
  csvFile.print(',');
  printOptional(csvFile, point.courseDegrees, 1);
  csvFile.print(',');
  printOptional(csvFile, point.hdop, 2);
  csvFile.print(',');
  if (point.satellites.valid) csvFile.print(point.satellites.value);
  csvFile.println();
  csvFile.flush();
}

void writeGpx(const tracking::TrackPoint& point) {
  if (!gpxFile) return;
  const size_t currentSize = gpxFile.size();
  if (currentSize >= GPX_TAIL_LENGTH) {
    gpxFile.seek(currentSize - GPX_TAIL_LENGTH);
  }
  gpxFile.printf("    <trkpt lat=\"%.6f\" lon=\"%.6f\">",
                 point.latitude, point.longitude);
  if (point.altitudeMeters.valid) {
    gpxFile.printf("<ele>%.1f</ele>", point.altitudeMeters.value);
  }
  gpxFile.print("<time>");
  gpxFile.print(point.utcValid ? point.utc.c_str() : "1970-01-01T00:00:00Z");
  gpxFile.println("</time></trkpt>");
  gpxFile.print(GPX_TAIL);
  gpxFile.flush();
}

tracking::GpsFix currentGpsFix(uint32_t now) {
  tracking::GpsFix fix;
  fix.locationValid = gps.location.isValid();
  fix.locationFresh = gps.location.isUpdated() && gps.location.age() < 2000;
  if (fix.locationValid) {
    fix.latitude = gps.location.lat();
    fix.longitude = gps.location.lng();
  }
  fix.utcValid = gps.date.isValid() && gps.time.isValid();
  if (fix.utcValid) {
    char timestamp[25];
    snprintf(timestamp, sizeof(timestamp), "%04u-%02u-%02uT%02u:%02u:%02uZ",
             gps.date.year(), gps.date.month(), gps.date.day(), gps.time.hour(),
             gps.time.minute(), gps.time.second());
    fix.utc = timestamp;
  }
  if (gps.altitude.isValid()) {
    fix.altitudeMeters.valid = true;
    fix.altitudeMeters.value = gps.altitude.meters();
  }
  if (gps.speed.isValid()) {
    fix.speedKmh.valid = true;
    fix.speedKmh.value = gps.speed.kmph();
  }
  if (gps.course.isValid()) {
    fix.courseDegrees.valid = true;
    fix.courseDegrees.value = gps.course.deg();
  }
  if (gps.hdop.isValid()) {
    fix.hdop.valid = true;
    fix.hdop.value = gps.hdop.value() / 100.0;
  }
  if (gps.satellites.isValid()) {
    fix.satellites.valid = true;
    fix.satellites.value = gps.satellites.value();
  }
  fix.uptimeMilliseconds = now;
  return fix;
}

}  // namespace

void setup() {
  Serial.begin(USB_BAUD);
  const uint32_t serialStartedAt = millis();
  while (!Serial && millis() - serialStartedAt < 3000) delay(10);
  Serial.println("\n=== ESP32 Motorcycle Tracker ===");
  initializeSd();
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.printf("[GPS] UART RX=%d TX=%d baud=%lu\n", GPS_RX_PIN, GPS_TX_PIN,
                static_cast<unsigned long>(GPS_BAUD));
}

void loop() {
  while (gpsSerial.available()) gps.encode(gpsSerial.read());

  static uint32_t lastFixCheck = 0;
  static uint32_t lastHeartbeat = 0;
  const uint32_t now = millis();

  if (now - lastFixCheck >= LOG_INTERVAL_MS) {
    lastFixCheck = now;
    const tracking::GpsFix fix = currentGpsFix(now);
    const tracking::TrackingDecision decision = workflow.processFix(fix, storage);
    if (!fix.locationFresh) ++noFreshLocationCount;
    if (decision.rawPointRecorded) {
      ++rawPointsRecorded;
      if (decision.writeFilteredCsv) writeCsv(decision.point);
      if (decision.writeFilteredGpx) writeGpx(decision.point);
    } else if (decision.rawPointWriteFailed) {
      logDiagnostic("[SD] Raw point %lu append/flush FAILED\n",
                    static_cast<unsigned long>(decision.point.pointNumber));
    }
  }

  if (workflow.trackingSessionActive() && now - lastHeartbeat >= 60000) {
    lastHeartbeat = now;
    logDiagnostic("[HB] uptime_ms=%lu raw_points=%lu no_fresh_location=%lu\n",
                  static_cast<unsigned long>(now),
                  static_cast<unsigned long>(rawPointsRecorded),
                  static_cast<unsigned long>(noFreshLocationCount));
  }
}
