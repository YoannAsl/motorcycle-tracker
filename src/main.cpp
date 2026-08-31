// ESP32 + NEO-7M GPS logger to microSD.
// GPS: TX->GPIO16, RX->GPIO17. SD SPI: CS 5, SCK 18, MISO 19, MOSI 23.

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <TinyGPSPlus.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/semphr.h>

#include "delivery_recovery.h"
#include "delivery_scheduler.h"
#include "tracking_workflow.h"
#include "upload_contract.h"
#include "upload_queue.h"

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
const uint32_t NETWORK_TIMEOUT_MS = 15000;
const uint32_t UPLOAD_RETRY_MS = 5000;

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
SemaphoreHandle_t sdMutex = nullptr;

class ScopedSdLock {
 public:
  ScopedSdLock()
      : locked_(sdMutex != nullptr &&
                xSemaphoreTakeRecursive(sdMutex, portMAX_DELAY) == pdTRUE) {}
  ~ScopedSdLock() {
    if (locked_) xSemaphoreGiveRecursive(sdMutex);
  }
  explicit operator bool() const { return locked_; }

 private:
  bool locked_;
};

void logDiagnostic(const char* format, ...) {
  char buffer[256];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(buffer, sizeof(buffer), format, arguments);
  va_end(arguments);
  Serial.print(buffer);
  ScopedSdLock lock;
  if (!lock) return;
  if (diagnosticFile) {
    diagnosticFile.print(buffer);
    diagnosticFile.flush();
  }
}

bool initializeSd() {
  ScopedSdLock lock;
  if (!lock) return false;
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("[SD] Mount FAILED");
    return false;
  }
  Serial.println("[SD] Mount OK");
  return true;
}

bool writeInitialExports() {
  ScopedSdLock lock;
  if (!lock) return false;
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

tracking::DeliveryRecovery deliveryRecovery;

class Esp32TrackingStorage : public tracking::TrackingStorage,
                              public tracking::DeliveryRecoveryStorage,
                              public tracking::UploadPointSource {
 public:
  bool listRoot(
      std::vector<tracking::RecoveryDirectoryEntry>& entries) override {
    ScopedSdLock lock;
    if (!lock) return false;
    File root = SD.open("/");
    if (!root || !root.isDirectory()) return false;

    File entry = root.openNextFile();
    while (entry) {
      tracking::RecoveryDirectoryEntry recoveredEntry;
      recoveredEntry.name = entry.name();
      recoveredEntry.directory = entry.isDirectory();
      entries.push_back(recoveredEntry);
      entry.close();
      entry = root.openNextFile();
    }
    root.close();
    return true;
  }

  tracking::RecoveryReadStatus countCompleteLines(
      const std::string& path, uint32_t& completeLines) override {
    ScopedSdLock lock;
    if (!lock) return tracking::RecoveryReadStatus::unreadable;
    if (!SD.exists(path.c_str())) return tracking::RecoveryReadStatus::missing;
    File file = SD.open(path.c_str(), FILE_READ);
    if (!file) return tracking::RecoveryReadStatus::unreadable;
    completeLines = 0;
    const size_t expectedBytes = file.size();
    size_t bytesRead = 0;
    while (file.available()) {
      const int value = file.read();
      if (value < 0) break;
      ++bytesRead;
      if (value == '\n' && completeLines != UINT32_MAX) ++completeLines;
    }
    file.close();
    return bytesRead == expectedBytes ? tracking::RecoveryReadStatus::readable
                                      : tracking::RecoveryReadStatus::unreadable;
  }

  tracking::RecoveryReadStatus readText(const std::string& path,
                                        std::string& contents) override {
    ScopedSdLock lock;
    if (!lock) return tracking::RecoveryReadStatus::unreadable;
    if (!SD.exists(path.c_str())) return tracking::RecoveryReadStatus::missing;
    File file = SD.open(path.c_str(), FILE_READ);
    if (!file) return tracking::RecoveryReadStatus::unreadable;
    const size_t expectedBytes = file.size();
    contents.clear();
    contents.reserve(expectedBytes);
    while (file.available()) {
      const int value = file.read();
      if (value < 0) break;
      contents += static_cast<char>(value);
    }
    file.close();
    return contents.size() == expectedBytes
               ? tracking::RecoveryReadStatus::readable
               : tracking::RecoveryReadStatus::unreadable;
  }

  bool appendText(const std::string& path,
                   const std::string& contents) override {
    ScopedSdLock lock;
    if (!lock) return false;
    File stateFile = SD.open(path.c_str(), FILE_APPEND);
    if (!stateFile) return false;
    const size_t written = stateFile.print(contents.c_str());
    stateFile.flush();
    stateFile.close();
    return written == contents.size();
  }

  tracking::BatchReadStatus readPointLines(
      uint32_t trackingSessionNumber, uint32_t firstPointNumber,
      size_t pointCount, std::vector<std::string>& lines) override {
    ScopedSdLock lock;
    if (!lock) return tracking::BatchReadStatus::lock_unavailable;
    char path[80];
    snprintf(path, sizeof(path), "/session-%010lu/track-points.ndjson",
             static_cast<unsigned long>(trackingSessionNumber));
    File points = SD.open(path, FILE_READ);
    if (!points) return tracking::BatchReadStatus::storage_open_failed;

    uint32_t pointNumber = 0;
    lines.clear();
    while (points.available() && lines.size() < pointCount) {
      String line = points.readStringUntil('\n');
      ++pointNumber;
      if (pointNumber < firstPointNumber) continue;
      if (line.endsWith("\r")) line.remove(line.length() - 1);
      if (line.length() == 0) {
        points.close();
        return tracking::BatchReadStatus::malformed_input;
      }
      lines.push_back(
          std::string(line.c_str(), static_cast<size_t>(line.length())));
    }
    points.close();
    return lines.size() == pointCount ? tracking::BatchReadStatus::ready
                                     : tracking::BatchReadStatus::malformed_input;
  }

  tracking::BatchReadStatus readPendingBatch(tracking::UploadBatch& batch) {
    ScopedSdLock lock;
    if (!lock) return tracking::BatchReadStatus::lock_unavailable;
    return tracking::readPendingBatch(deliveryRecovery.sessions(), *this,
                                      TRACKER_ID, batch);
  }

  bool confirmDeliveryThrough(uint32_t trackingSessionNumber,
                              uint32_t highestConfirmedPoint) {
    ScopedSdLock lock;
    if (!lock) return false;
    return deliveryRecovery.confirmDeliveryThrough(
        *this, trackingSessionNumber, highestConfirmedPoint);
  }

  bool startTrackingSession(uint32_t& trackingSessionNumber) override {
    ScopedSdLock lock;
    if (!lock) return false;
    if (!deliveryRecovery.ready() || !SD.cardSize()) return false;

    Preferences preferences;
    if (!preferences.begin("tracker", false)) return false;
    uint32_t candidate = 0;
    if (!tracking::nextTrackingSessionNumber(
            preferences.getUInt("session", 0),
            deliveryRecovery.maxStoredTrackingSessionNumber(), candidate)) {
      preferences.end();
      return false;
    }
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
    if (!deliveryRecovery.beginSession(candidate)) return false;
    logDiagnostic("[SESSION] Started number=%lu directory=%s\n",
                  static_cast<unsigned long>(candidate),
                  trackingSessionDirectory);
    return true;
  }

  bool appendAndFlushRawPoint(const tracking::TrackPoint& point,
                              const std::string& ndjson) override {
    ScopedSdLock lock;
    if (!lock) return false;
    if (!rawPointFile) return false;
    const size_t bodyWritten = rawPointFile.print(ndjson.c_str());
    const size_t newlineWritten = rawPointFile.print('\n');
    rawPointFile.flush();
    return bodyWritten == ndjson.size() && newlineWritten == 1 &&
           deliveryRecovery.recordPoint(point.trackingSessionNumber);
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
  ScopedSdLock lock;
  if (!lock) return;
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
  ScopedSdLock lock;
  if (!lock) return;
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

bool uploadBatch(const tracking::UploadBatch& batch) {
  tracking::UploadRequest request;
  if (!tracking::buildUploadRequest(UPLOAD_URL, TRACKER_TOKEN, batch, request)) {
    logDiagnostic("[UPLOAD] Invalid local configuration or point range\n");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(HOTSPOT_NAME, HOTSPOT_PASSWORD);
  const uint32_t connectionStartedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - connectionStartedAt < NETWORK_TIMEOUT_MS) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (WiFi.status() != WL_CONNECTED) {
    logDiagnostic("[UPLOAD] Wi-Fi connection failed; points remain pending\n");
    return false;
  }

  WiFiClientSecure tlsClient;
  tlsClient.setCACert(UPLOAD_ROOT_CA_CERTIFICATE);
  tlsClient.setTimeout(NETWORK_TIMEOUT_MS);
  HTTPClient http;
  http.setTimeout(NETWORK_TIMEOUT_MS);
  if (!http.begin(tlsClient, request.url.c_str())) {
    logDiagnostic("[UPLOAD] HTTPS setup failed; points remain pending\n");
    return false;
  }
  for (std::vector<tracking::UploadRequest::Header>::const_iterator it =
           request.headers.begin();
       it != request.headers.end(); ++it) {
    http.addHeader(it->name.c_str(), it->value.c_str());
  }
  const int statusCode = http.POST(
      reinterpret_cast<uint8_t*>(const_cast<char*>(request.body.data())),
      request.body.size());
  const String response = statusCode > 0 ? http.getString() : String();
  http.end();

  tracking::UploadConfirmation confirmation;
  if (!tracking::validateUploadResponse(
          statusCode, std::string(response.c_str(), response.length()), batch,
          confirmation)) {
    logDiagnostic(
        "[UPLOAD] HTTPS response rejected status=%d; points remain pending\n",
        statusCode);
    return false;
  }

  const uint32_t lastPoint = batch.firstPointNumber +
      static_cast<uint32_t>(batch.ndjsonPoints.size()) - 1;
  if (storage.confirmDeliveryThrough(batch.trackingSessionNumber, lastPoint)) {
    logDiagnostic("[UPLOAD] Confirmed session=%lu points=%lu-%lu\n",
                  static_cast<unsigned long>(batch.trackingSessionNumber),
                  static_cast<unsigned long>(batch.firstPointNumber),
                  static_cast<unsigned long>(lastPoint));
    return true;
  } else {
    logDiagnostic(
        "[UPLOAD] Confirmation persistence failed; points remain pending\n");
    return false;
  }
}

void uploadPendingData(void*) {
  tracking::DeliveryRetrySchedule retry;
  while (true) {
    tracking::UploadBatch batch;
    const tracking::BatchReadStatus batchStatus =
        storage.readPendingBatch(batch);
    if (batchStatus == tracking::BatchReadStatus::no_pending_batch) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (batchStatus != tracking::BatchReadStatus::ready) {
      const char* failure = "malformed point data";
      if (batchStatus == tracking::BatchReadStatus::lock_unavailable) {
        failure = "SD lock unavailable";
      } else if (batchStatus ==
                 tracking::BatchReadStatus::storage_open_failed) {
        failure = "point file open failed";
      }
      logDiagnostic("[UPLOAD] Batch read failed: %s; retrying\n", failure);
      vTaskDelay(pdMS_TO_TICKS(UPLOAD_RETRY_MS));
      continue;
    }
    if (uploadBatch(batch)) {
      retry.recordSuccess();
      continue;
    }
    const uint32_t retrySeconds = retry.recordFailure();
    logDiagnostic("[UPLOAD] Retrying pending data in %lu seconds\n",
                  static_cast<unsigned long>(retrySeconds));
    vTaskDelay(pdMS_TO_TICKS(retrySeconds * 1000));
  }
}

}  // namespace

void setup() {
  Serial.begin(USB_BAUD);
  const uint32_t serialStartedAt = millis();
  while (!Serial && millis() - serialStartedAt < 3000) delay(10);
  Serial.println("\n=== ESP32 Motorcycle Tracker ===");
  sdMutex = xSemaphoreCreateRecursiveMutex();
  const bool sdReady = initializeSd();
  if (sdReady) {
    if (!deliveryRecovery.restore(storage)) {
      logDiagnostic("[RECOVERY] SD session scan FAILED\n");
    } else {
      const std::vector<tracking::RecoveredTrackingSession>& sessions =
          deliveryRecovery.sessions();
      for (std::vector<tracking::RecoveredTrackingSession>::const_iterator it =
               sessions.begin();
           it != sessions.end(); ++it) {
        logDiagnostic(
            "[RECOVERY] session=%lu recorded=%lu confirmed=%lu status=%s\n",
            static_cast<unsigned long>(it->trackingSessionNumber),
            static_cast<unsigned long>(it->highestRecordedPoint),
            static_cast<unsigned long>(it->highestConfirmedPoint),
             it->recoveryRequired ? "safe-resend" : "clean");
      }
      if (xTaskCreatePinnedToCore(uploadPendingData, "track-upload", 8192,
                                  nullptr, 0, nullptr, 0) != pdPASS) {
        logDiagnostic("[UPLOAD] Background task start failed\n");
      }
    }
  }
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
