#include "tracking_workflow.h"

#include <cmath>
#include <cstdio>

namespace tracking {

namespace {

bool finite(double value) { return std::isfinite(value); }

bool isFreshLocation(const GpsFix& fix) {
  return fix.locationValid && fix.locationFresh && finite(fix.latitude) &&
         finite(fix.longitude);
}

bool isStartCandidate(const GpsFix& fix) {
  return isFreshLocation(fix) && fix.utcValid && !fix.utc.empty() &&
         fix.speedKmh.valid && finite(fix.speedKmh.value) &&
         fix.speedKmh.value > 2.0 && fix.hdop.valid &&
         finite(fix.hdop.value) && fix.hdop.value <= 5.0;
}

OptionalDouble safe(OptionalDouble value) {
  if (!value.valid || !finite(value.value)) {
    value.valid = false;
    value.value = 0.0;
  }
  return value;
}

std::string escapeJson(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (std::string::const_iterator it = value.begin(); it != value.end(); ++it) {
    const unsigned char character = static_cast<unsigned char>(*it);
    switch (character) {
      case '\"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\b': escaped += "\\b"; break;
      case '\f': escaped += "\\f"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (character < 0x20) {
          char buffer[7];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", character);
          escaped += buffer;
        } else {
          escaped += static_cast<char>(character);
        }
    }
  }
  return escaped;
}

void appendOptionalDouble(std::string& json, const OptionalDouble& value) {
  if (!value.valid || !finite(value.value)) {
    json += "null";
    return;
  }
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.8g", value.value);
  json += buffer;
}

void appendOptionalUInt(std::string& json, const OptionalUInt& value) {
  if (!value.valid) {
    json += "null";
    return;
  }
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%lu",
                static_cast<unsigned long>(value.value));
  json += buffer;
}

TrackPoint makePoint(const GpsFix& fix, const std::string& trackerId,
                     uint32_t trackingSessionNumber, uint32_t pointNumber) {
  TrackPoint point;
  point.trackerId = trackerId;
  point.trackingSessionNumber = trackingSessionNumber;
  point.pointNumber = pointNumber;
  point.utcValid = fix.utcValid && !fix.utc.empty();
  point.utc = point.utcValid ? fix.utc : std::string();
  point.latitude = fix.latitude;
  point.longitude = fix.longitude;
  point.altitudeMeters = safe(fix.altitudeMeters);
  point.speedKmh = safe(fix.speedKmh);
  point.courseDegrees = safe(fix.courseDegrees);
  point.hdop = safe(fix.hdop);
  point.satellites = fix.satellites;
  point.uptimeMilliseconds = fix.uptimeMilliseconds;
  return point;
}

}  // namespace

TrackingWorkflow::TrackingWorkflow(const std::string& trackerId)
    : trackerId_(trackerId) {}

TrackingDecision TrackingWorkflow::processFix(const GpsFix& fix,
                                               TrackingStorage& storage) {
  TrackingDecision decision;

  if (!active_) {
    if (!isStartCandidate(fix)) {
      consecutiveStartCandidates_ = 0;
      return decision;
    }

    ++consecutiveStartCandidates_;
    if (consecutiveStartCandidates_ < 3) return decision;

    if (!storage.startTrackingSession(trackingSessionNumber_)) {
      consecutiveStartCandidates_ = 2;
      return decision;
    }
    active_ = true;
    decision.trackingSessionStarted = true;
  } else if (!isFreshLocation(fix)) {
    return decision;
  }

  decision.point =
      makePoint(fix, trackerId_, trackingSessionNumber_, nextPointNumber_);
  const std::string ndjson = serializeTrackPoint(decision.point);
  if (!storage.appendAndFlushRawPoint(decision.point, ndjson)) {
    decision.rawPointWriteFailed = true;
    return decision;
  }

  decision.rawPointRecorded = true;
  decision.writeFilteredCsv =
      decision.point.speedKmh.valid && decision.point.speedKmh.value > 2.0;
  decision.writeFilteredGpx = decision.writeFilteredCsv;
  ++nextPointNumber_;
  return decision;
}

bool TrackingWorkflow::trackingSessionActive() const { return active_; }

std::string serializeTrackPoint(const TrackPoint& point) {
  char numbers[96];
  std::snprintf(numbers, sizeof(numbers), "{\"schema_version\":%lu,",
                static_cast<unsigned long>(point.schemaVersion));
  std::string json(numbers);
  json += "\"tracker_id\":\"" + escapeJson(point.trackerId) + "\",";
  std::snprintf(numbers, sizeof(numbers),
                "\"tracking_session_number\":%lu,\"point_number\":%lu,"
                "\"gps_utc\":",
                static_cast<unsigned long>(point.trackingSessionNumber),
                static_cast<unsigned long>(point.pointNumber));
  json += numbers;
  if (point.utcValid && !point.utc.empty()) {
    json += "\"" + escapeJson(point.utc) + "\"";
  } else {
    json += "null";
  }

  std::snprintf(numbers, sizeof(numbers),
                ",\"latitude\":%.8f,\"longitude\":%.8f,\"altitude_m\":",
                point.latitude, point.longitude);
  json += numbers;
  appendOptionalDouble(json, point.altitudeMeters);
  json += ",\"speed_kmh\":";
  appendOptionalDouble(json, point.speedKmh);
  json += ",\"course_deg\":";
  appendOptionalDouble(json, point.courseDegrees);
  json += ",\"hdop\":";
  appendOptionalDouble(json, point.hdop);
  json += ",\"satellites\":";
  appendOptionalUInt(json, point.satellites);
  std::snprintf(numbers, sizeof(numbers), ",\"uptime_ms\":%lu}",
                static_cast<unsigned long>(point.uptimeMilliseconds));
  json += numbers;
  return json;
}

}  // namespace tracking
