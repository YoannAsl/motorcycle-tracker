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
#include <esp_system.h>
#include <freertos/semphr.h>

#include "delivery_recovery.h"
#include "delivery_scheduler.h"
#include "diagnostic_log.h"
#include "storage_cleanup.h"
#include "tracking_workflow.h"
#include "upload_contract.h"

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
std::string diagnosticBacklog;

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
  if (diagnosticFile) {
    diagnosticFile.print(buffer);
    diagnosticFile.flush();
  } else {
    diagnosticBacklog += buffer;
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

class Esp32TrackingStorage : public tracking::TrackingStorage {
 public:
  bool recoverPersistedSessions() {
    ScopedSdLock lock;
    if (!lock) return false;
    std::vector<tracking::StoredTrackingSession> storedSessions;
    storedDiagnosticLogs_.clear();
    File root = SD.open("/");
    if (!root || !root.isDirectory()) return false;

    maxStoredTrackingSessionNumber_ = 0;
    File entry = root.openNextFile();
    while (entry) {
      uint32_t trackingSessionNumber = 0;
      if (entry.isDirectory() &&
          parseTrackingSessionNumber(entry.name(), trackingSessionNumber)) {
        if (trackingSessionNumber > maxStoredTrackingSessionNumber_) {
          maxStoredTrackingSessionNumber_ = trackingSessionNumber;
        }

        tracking::StoredTrackingSession stored;
        stored.trackingSessionNumber = trackingSessionNumber;
        char path[80];
        snprintf(path, sizeof(path), "/session-%010lu/track-points.ndjson",
                 static_cast<unsigned long>(trackingSessionNumber));
        stored.highestRecordedPoint = countCompleteLines(path);
        snprintf(path, sizeof(path), "/session-%010lu/delivery-state.log",
                 static_cast<unsigned long>(trackingSessionNumber));
        stored.deliveryState = readFile(path);
        storedSessions.push_back(stored);

        tracking::StoredDiagnosticLog diagnostic;
        diagnostic.trackingSessionNumber = trackingSessionNumber;
        snprintf(path, sizeof(path), "/session-%010lu/session.log",
                 static_cast<unsigned long>(trackingSessionNumber));
        diagnostic.contents = readFile(path);
        snprintf(path, sizeof(path),
                 "/session-%010lu/diagnostic-delivery-state.log",
                 static_cast<unsigned long>(trackingSessionNumber));
        diagnostic.deliveryState = readFile(path);
        storedDiagnosticLogs_.push_back(diagnostic);
      }
      entry.close();
      entry = root.openNextFile();
    }
    root.close();

    recoveredSessions_ = tracking::recoverTrackingSessions(storedSessions);
    for (std::vector<tracking::RecoveredTrackingSession>::const_iterator it =
             recoveredSessions_.begin();
         it != recoveredSessions_.end(); ++it) {
      logDiagnostic(
          "[RECOVERY] session=%lu recorded=%lu confirmed=%lu status=%s\n",
          static_cast<unsigned long>(it->trackingSessionNumber),
          static_cast<unsigned long>(it->highestRecordedPoint),
          static_cast<unsigned long>(it->highestConfirmedPoint),
          it->recoveryRequired ? "safe-resend" : "clean");
    }
    return true;
  }

  const std::vector<tracking::RecoveredTrackingSession>& recoveredSessions()
      const {
    return recoveredSessions_;
  }

  bool confirmDeliveryThrough(uint32_t trackingSessionNumber,
                              uint32_t highestConfirmedPoint) {
    ScopedSdLock lock;
    if (!lock) return false;
    char path[80];
    snprintf(path, sizeof(path), "/session-%010lu/delivery-state.log",
             static_cast<unsigned long>(trackingSessionNumber));
    File stateFile = SD.open(path, FILE_APPEND);
    if (!stateFile) return false;
    const std::string record = tracking::serializeDeliveryProgress(
        trackingSessionNumber, highestConfirmedPoint);
    const size_t written = stateFile.print(record.c_str());
    stateFile.flush();
    stateFile.close();
    if (written != record.size()) return false;
    for (std::vector<tracking::RecoveredTrackingSession>::iterator it =
             recoveredSessions_.begin();
         it != recoveredSessions_.end(); ++it) {
      if (it->trackingSessionNumber == trackingSessionNumber &&
          highestConfirmedPoint > it->highestConfirmedPoint &&
          highestConfirmedPoint <= it->highestRecordedPoint) {
        it->highestConfirmedPoint = highestConfirmedPoint;
        break;
      }
    }
    cleanupDeliveredSessions();
    return true;
  }

  bool readPendingDiagnosticLog(tracking::DiagnosticLogUpload& upload) {
    ScopedSdLock lock;
    if (!lock) return false;
    return tracking::selectOldestPendingDiagnosticLog(
        TRACKER_ID, storedDiagnosticLogs_, upload);
  }

  bool confirmDiagnosticDelivery(uint32_t trackingSessionNumber) {
    ScopedSdLock lock;
    if (!lock) return false;
    char path[96];
    snprintf(path, sizeof(path),
             "/session-%010lu/diagnostic-delivery-state.log",
             static_cast<unsigned long>(trackingSessionNumber));
    File stateFile = SD.open(path, FILE_APPEND);
    if (!stateFile) return false;
    const std::string record = tracking::serializeDiagnosticDelivery(
        TRACKER_ID, trackingSessionNumber);
    const size_t written = stateFile.print(record.c_str());
    stateFile.flush();
    stateFile.close();
    if (written != record.size()) return false;
    for (std::vector<tracking::StoredDiagnosticLog>::iterator it =
             storedDiagnosticLogs_.begin();
         it != storedDiagnosticLogs_.end(); ++it) {
      if (it->trackingSessionNumber == trackingSessionNumber) {
        it->deliveryState += record;
        cleanupDeliveredSessions();
        return true;
      }
    }
    return false;
  }

  bool readPendingBatch(tracking::UploadBatch& batch) {
    ScopedSdLock lock;
    if (!lock) return false;
    tracking::PendingDeliveryBatch pending;
    if (!tracking::selectOldestPendingBatch(recoveredSessions_, pending)) {
      return false;
    }

    char path[80];
    snprintf(path, sizeof(path), "/session-%010lu/track-points.ndjson",
             static_cast<unsigned long>(pending.trackingSessionNumber));
    File points = SD.open(path, FILE_READ);
    if (!points) return false;

    uint32_t pointNumber = 0;
    batch = tracking::UploadBatch();
    batch.trackerId = TRACKER_ID;
    batch.trackingSessionNumber = pending.trackingSessionNumber;
    batch.firstPointNumber = pending.firstPointNumber;
    while (points.available() &&
           batch.ndjsonPoints.size() < pending.pointCount) {
      String line = points.readStringUntil('\n');
      ++pointNumber;
      if (pointNumber < batch.firstPointNumber) continue;
      if (line.endsWith("\r")) line.remove(line.length() - 1);
      if (line.length() == 0) {
        points.close();
        return false;
      }
      batch.ndjsonPoints.push_back(
          std::string(line.c_str(), static_cast<size_t>(line.length())));
    }
    points.close();
    if (batch.ndjsonPoints.size() != pending.pointCount) {
      batch = tracking::UploadBatch();
      return false;
    }
    return true;
  }

  bool startTrackingSession(uint32_t& trackingSessionNumber) override {
    ScopedSdLock lock;
    if (!lock) return false;
    if (!SD.cardSize()) return false;

    Preferences preferences;
    if (!preferences.begin("tracker", false)) return false;
    uint32_t candidate = preferences.getUInt("session", 0) + 1;
    if (candidate <= maxStoredTrackingSessionNumber_) {
      if (maxStoredTrackingSessionNumber_ == UINT32_MAX) {
        preferences.end();
        return false;
      }
      candidate = maxStoredTrackingSessionNumber_ + 1;
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
    if (!diagnosticBacklog.empty()) {
      diagnosticFile.print(diagnosticBacklog.c_str());
      diagnosticFile.flush();
      diagnosticBacklog.clear();
    }

    trackingSessionNumber = candidate;
    tracking::RecoveredTrackingSession activeSession;
    activeSession.trackingSessionNumber = candidate;
    activeSession.inactive = false;
    recoveredSessions_.push_back(activeSession);
    logDiagnostic("[SESSION] Started number=%lu directory=%s\n",
                  static_cast<unsigned long>(candidate),
                  trackingSessionDirectory);
    return true;
  }

  bool appendAndFlushRawPoint(const tracking::TrackPoint&,
                              const std::string& ndjson) override {
    ScopedSdLock lock;
    if (!lock) return false;
    if (!rawPointFile) return false;
    const size_t bodyWritten = rawPointFile.print(ndjson.c_str());
    const size_t newlineWritten = rawPointFile.print('\n');
    rawPointFile.flush();
    if (bodyWritten != ndjson.size() || newlineWritten != 1) return false;
    if (!recoveredSessions_.empty() && !recoveredSessions_.back().inactive &&
        recoveredSessions_.back().highestRecordedPoint != UINT32_MAX) {
      ++recoveredSessions_.back().highestRecordedPoint;
    }
    cleanupDeliveredSessions();
    return true;
  }

  void cleanupDeliveredSessions() {
    ScopedSdLock lock;
    if (!lock) return;
    const uint64_t capacityBytes = SD.cardSize();
    const uint64_t usedBytes = SD.usedBytes();
    if (!tracking::storageCleanupRequired(capacityBytes, usedBytes)) {
      cleanupBlockedLogged_ = false;
      return;
    }
    std::vector<tracking::CleanupSession> sessions;
    for (std::vector<tracking::RecoveredTrackingSession>::const_iterator it =
             recoveredSessions_.begin();
         it != recoveredSessions_.end(); ++it) {
      tracking::CleanupSession candidate;
      candidate.trackingSessionNumber = it->trackingSessionNumber;
      candidate.inactive = it->inactive;
      candidate.allPointsConfirmed =
          it->highestConfirmedPoint >= it->highestRecordedPoint;
      candidate.diagnosticLogConfirmed = diagnosticDelivered(
          it->trackingSessionNumber);
      char path[48];
      snprintf(path, sizeof(path), "/session-%010lu",
               static_cast<unsigned long>(it->trackingSessionNumber));
      candidate.bytes = treeSize(path);
      sessions.push_back(candidate);
    }

    const tracking::CleanupPlan plan = tracking::planStorageCleanup(
        capacityBytes, usedBytes, sessions);
    if (!plan.started) {
      cleanupBlockedLogged_ = false;
      return;
    }
    for (std::vector<uint32_t>::const_iterator number =
             plan.sessionsToDelete.begin();
         number != plan.sessionsToDelete.end(); ++number) {
      if (tracking::storageCleanupTargetReached(capacityBytes,
                                                SD.usedBytes())) {
        break;
      }
      const uint32_t deletedSessionNumber = *number;
      char path[48];
      snprintf(path, sizeof(path), "/session-%010lu",
               static_cast<unsigned long>(deletedSessionNumber));
      if (!removeTree(path)) {
        logDiagnostic("[ERROR] cleanup delete failed session=%lu\n",
                      static_cast<unsigned long>(deletedSessionNumber));
        continue;
      }
      recoveredSessions_.erase(
          std::remove_if(recoveredSessions_.begin(), recoveredSessions_.end(),
                         [deletedSessionNumber](
                             const tracking::RecoveredTrackingSession& item) {
                           return item.trackingSessionNumber ==
                                  deletedSessionNumber;
                         }),
          recoveredSessions_.end());
      storedDiagnosticLogs_.erase(
          std::remove_if(storedDiagnosticLogs_.begin(),
                         storedDiagnosticLogs_.end(),
                         [deletedSessionNumber](
                             const tracking::StoredDiagnosticLog& item) {
                           return item.trackingSessionNumber ==
                                  deletedSessionNumber;
                         }),
          storedDiagnosticLogs_.end());
      logDiagnostic("[CLEANUP] deleted delivered session=%lu\n",
                    static_cast<unsigned long>(deletedSessionNumber));
    }

    const bool targetReached =
        tracking::storageCleanupTargetReached(capacityBytes, SD.usedBytes());
    if (!targetReached && plan.protectedDataRemains &&
        !cleanupBlockedLogged_) {
      logDiagnostic(
          "[CLEANUP] unable to reach below 70 percent; pending data protected\n");
      cleanupBlockedLogged_ = true;
    } else if (targetReached) {
      cleanupBlockedLogged_ = false;
    }
  }

 private:
  static bool parseTrackingSessionNumber(const char* path,
                                         uint32_t& trackingSessionNumber) {
    const char* name = strrchr(path, '/');
    name = name == nullptr ? path : name + 1;
    unsigned long parsed = 0;
    char trailing = '\0';
    if (sscanf(name, "session-%lu%c", &parsed, &trailing) != 1 ||
        parsed == 0 || parsed > UINT32_MAX) {
      return false;
    }
    trackingSessionNumber = static_cast<uint32_t>(parsed);
    return true;
  }

  static uint32_t countCompleteLines(const char* path) {
    File file = SD.open(path, FILE_READ);
    if (!file) return 0;
    uint32_t completeLines = 0;
    while (file.available()) {
      if (file.read() == '\n' && completeLines != UINT32_MAX) ++completeLines;
    }
    file.close();
    return completeLines;
  }

  static std::string readFile(const char* path) {
    File file = SD.open(path, FILE_READ);
    if (!file) return std::string();
    std::string contents;
    contents.reserve(file.size());
    while (file.available()) contents += static_cast<char>(file.read());
    file.close();
    return contents;
  }

  bool diagnosticDelivered(uint32_t trackingSessionNumber) const {
    for (std::vector<tracking::StoredDiagnosticLog>::const_iterator it =
             storedDiagnosticLogs_.begin();
         it != storedDiagnosticLogs_.end(); ++it) {
      if (it->trackingSessionNumber == trackingSessionNumber) {
        return it->deliveryState.find(tracking::serializeDiagnosticDelivery(
                   TRACKER_ID, trackingSessionNumber)) != std::string::npos;
      }
    }
    return false;
  }

  static uint64_t treeSize(const char* path) {
    File directory = SD.open(path);
    if (!directory || !directory.isDirectory()) return 0;
    uint64_t bytes = 0;
    File entry = directory.openNextFile();
    while (entry) {
      if (entry.isDirectory()) {
        bytes += treeSize(entry.path());
      } else {
        bytes += entry.size();
      }
      entry.close();
      entry = directory.openNextFile();
    }
    directory.close();
    return bytes;
  }

  static bool removeTree(const char* path) {
    File directory = SD.open(path);
    if (!directory || !directory.isDirectory()) return false;
    File entry = directory.openNextFile();
    while (entry) {
      const std::string childPath = entry.path();
      const bool childIsDirectory = entry.isDirectory();
      entry.close();
      if (childIsDirectory ? !removeTree(childPath.c_str())
                           : !SD.remove(childPath.c_str())) {
        directory.close();
        return false;
      }
      entry = directory.openNextFile();
    }
    directory.close();
    return SD.rmdir(path);
  }

  uint32_t maxStoredTrackingSessionNumber_ = 0;
  std::vector<tracking::RecoveredTrackingSession> recoveredSessions_;
  std::vector<tracking::StoredDiagnosticLog> storedDiagnosticLogs_;
  bool cleanupBlockedLogged_ = false;
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

const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt-watchdog";
    case ESP_RST_TASK_WDT: return "task-watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}

std::string diagnosticUploadUrl() {
  const std::string pointUrl(UPLOAD_URL);
  const size_t path = pointUrl.find('/', 8);
  if (pointUrl.compare(0, 8, "https://") != 0 || path == std::string::npos) {
    return std::string();
  }
  return pointUrl.substr(0, path) + "/v1/diagnostic-logs";
}

bool uploadDiagnosticLog(const tracking::DiagnosticLogUpload& upload) {
  tracking::DiagnosticUploadRequest request;
  if (!tracking::buildDiagnosticUploadRequest(
          diagnosticUploadUrl(), TRACKER_TOKEN, upload, request)) {
    logDiagnostic("[ERROR] diagnostic upload configuration invalid\n");
    return false;
  }

  logDiagnostic("[UPLOAD] diagnostic attempt session=%lu\n",
                static_cast<unsigned long>(upload.trackingSessionNumber));
  logDiagnostic("[WIFI] connecting for diagnostic upload\n");
  WiFi.mode(WIFI_STA);
  WiFi.begin(HOTSPOT_NAME, HOTSPOT_PASSWORD);
  const uint32_t connectionStartedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - connectionStartedAt < NETWORK_TIMEOUT_MS) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (WiFi.status() != WL_CONNECTED) {
    logDiagnostic("[UPLOAD] diagnostic result=%s\n",
                  tracking::diagnosticUploadFailureMessage(
                      tracking::DiagnosticUploadFailure::Wifi)
                      .c_str());
    return false;
  }
  logDiagnostic("[WIFI] connected for diagnostic upload\n");

  WiFiClientSecure tlsClient;
  tlsClient.setCACert(UPLOAD_ROOT_CA_CERTIFICATE);
  tlsClient.setTimeout(NETWORK_TIMEOUT_MS);
  HTTPClient http;
  http.setTimeout(NETWORK_TIMEOUT_MS);
  if (!http.begin(tlsClient, request.url.c_str())) {
    logDiagnostic("[UPLOAD] diagnostic result=%s\n",
                  tracking::diagnosticUploadFailureMessage(
                      tracking::DiagnosticUploadFailure::Tls)
                      .c_str());
    return false;
  }
  for (std::vector<tracking::DiagnosticUploadRequest::Header>::const_iterator it =
           request.headers.begin();
       it != request.headers.end(); ++it) {
    http.addHeader(it->name.c_str(), it->value.c_str());
  }
  const int statusCode = http.POST(
      reinterpret_cast<uint8_t*>(const_cast<char*>(request.body.data())),
      request.body.size());
  const String response = statusCode > 0 ? http.getString() : String();
  http.end();
  logDiagnostic("[CLEANUP] diagnostic HTTPS client closed\n");

  if (!tracking::validateDiagnosticUploadResponse(
          statusCode, std::string(response.c_str(), response.length()),
          upload)) {
    tracking::DiagnosticUploadFailure failure =
        tracking::DiagnosticUploadFailure::MalformedResponse;
    if (statusCode == 401 || statusCode == 403) {
      failure = tracking::DiagnosticUploadFailure::Authentication;
    } else if (statusCode >= 400 && statusCode < 600) {
      failure = tracking::DiagnosticUploadFailure::Server;
    } else if (statusCode == HTTPC_ERROR_READ_TIMEOUT) {
      failure = tracking::DiagnosticUploadFailure::Timeout;
    } else if (statusCode < 0) {
      failure = tracking::DiagnosticUploadFailure::Tls;
    }
    logDiagnostic("[UPLOAD] diagnostic result=%s status=%d\n",
                  tracking::diagnosticUploadFailureMessage(failure).c_str(),
                  statusCode);
    return false;
  }

  if (!storage.confirmDiagnosticDelivery(upload.trackingSessionNumber)) {
    logDiagnostic(
        "[ERROR] diagnostic confirmation persistence failed session=%lu\n",
        static_cast<unsigned long>(upload.trackingSessionNumber));
    return false;
  }
  logDiagnostic("[UPLOAD] diagnostic result=confirmed session=%lu\n",
                static_cast<unsigned long>(upload.trackingSessionNumber));
  return true;
}

bool uploadBatch(const tracking::UploadBatch& batch) {
  tracking::UploadRequest request;
  if (!tracking::buildUploadRequest(UPLOAD_URL, TRACKER_TOKEN, batch, request)) {
    logDiagnostic("[ERROR] track-point upload configuration or range invalid\n");
    return false;
  }

  logDiagnostic("[UPLOAD] track-points attempt session=%lu points=%lu-%lu\n",
                static_cast<unsigned long>(batch.trackingSessionNumber),
                static_cast<unsigned long>(batch.firstPointNumber),
                static_cast<unsigned long>(
                    batch.firstPointNumber + batch.ndjsonPoints.size() - 1));

  WiFi.mode(WIFI_STA);
  logDiagnostic("[WIFI] connecting for track-point upload\n");
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
  logDiagnostic("[WIFI] connected for track-point upload\n");

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
  logDiagnostic("[CLEANUP] track-point HTTPS client closed\n");

  tracking::UploadConfirmation confirmation;
  if (!tracking::validateUploadResponse(
          statusCode, std::string(response.c_str(), response.length()), batch,
          confirmation)) {
    tracking::DiagnosticUploadFailure failure =
        tracking::DiagnosticUploadFailure::MalformedResponse;
    if (statusCode == 401 || statusCode == 403) {
      failure = tracking::DiagnosticUploadFailure::Authentication;
    } else if (statusCode >= 400 && statusCode < 600) {
      failure = tracking::DiagnosticUploadFailure::Server;
    } else if (statusCode == HTTPC_ERROR_READ_TIMEOUT) {
      failure = tracking::DiagnosticUploadFailure::Timeout;
    } else if (statusCode < 0) {
      failure = tracking::DiagnosticUploadFailure::Tls;
    }
    logDiagnostic(
        "[UPLOAD] track-points result=%s status=%d; points remain pending\n",
        tracking::diagnosticUploadFailureMessage(failure).c_str(), statusCode);
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
  tracking::DeliveryRetrySchedule diagnosticRetry;
  tracking::DeliveryRetrySchedule pointRetry;
  while (true) {
    bool foundPendingData = false;
    uint32_t retrySeconds = 0;
    tracking::DiagnosticLogUpload diagnostic;
    if (storage.readPendingDiagnosticLog(diagnostic)) {
      foundPendingData = true;
      if (uploadDiagnosticLog(diagnostic)) {
        diagnosticRetry.recordSuccess();
      } else {
        retrySeconds = diagnosticRetry.recordFailure();
        logDiagnostic("[UPLOAD] Retrying pending diagnostic in %lu seconds\n",
                      static_cast<unsigned long>(retrySeconds));
      }
    }
    tracking::UploadBatch batch;
    if (storage.readPendingBatch(batch)) {
      foundPendingData = true;
      if (uploadBatch(batch)) {
        pointRetry.recordSuccess();
      } else {
        const uint32_t pointRetrySeconds = pointRetry.recordFailure();
        if (retrySeconds == 0 || pointRetrySeconds < retrySeconds) {
          retrySeconds = pointRetrySeconds;
        }
        logDiagnostic("[UPLOAD] Retrying pending points in %lu seconds\n",
                      static_cast<unsigned long>(pointRetrySeconds));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(
        (retrySeconds == 0 ? (foundPendingData ? 0 : 1) : retrySeconds) *
        1000));
  }
}

}  // namespace

void setup() {
  Serial.begin(USB_BAUD);
  const uint32_t serialStartedAt = millis();
  while (!Serial && millis() - serialStartedAt < 3000) delay(10);
  Serial.println("\n=== ESP32 Motorcycle Tracker ===");
  logDiagnostic("[BOOT] reset=%s\n", resetReasonName(esp_reset_reason()));
  sdMutex = xSemaphoreCreateRecursiveMutex();
  const bool sdReady = initializeSd();
  if (sdReady) {
    if (!storage.recoverPersistedSessions()) {
      logDiagnostic("[RECOVERY] SD session scan FAILED\n");
    } else {
      storage.cleanupDeliveredSessions();
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
    logDiagnostic("[HEALTH] uptime_ms=%lu raw_points=%lu no_fresh_location=%lu\n",
                  static_cast<unsigned long>(now),
                  static_cast<unsigned long>(rawPointsRecorded),
                  static_cast<unsigned long>(noFreshLocationCount));
  }
}
