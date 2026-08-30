#pragma once

#include <stdint.h>
#include <string>

namespace tracking {

struct OptionalDouble {
  bool valid = false;
  double value = 0.0;
};

struct OptionalUInt {
  bool valid = false;
  uint32_t value = 0;
};

struct GpsFix {
  bool locationValid = false;
  bool locationFresh = false;
  double latitude = 0.0;
  double longitude = 0.0;
  bool utcValid = false;
  std::string utc;
  OptionalDouble altitudeMeters;
  OptionalDouble speedKmh;
  OptionalDouble courseDegrees;
  OptionalDouble hdop;
  OptionalUInt satellites;
  uint32_t uptimeMilliseconds = 0;
};

struct TrackPoint {
  uint32_t schemaVersion = 1;
  std::string trackerId;
  uint32_t trackingSessionNumber = 0;
  uint32_t pointNumber = 0;
  bool utcValid = false;
  std::string utc;
  double latitude = 0.0;
  double longitude = 0.0;
  OptionalDouble altitudeMeters;
  OptionalDouble speedKmh;
  OptionalDouble courseDegrees;
  OptionalDouble hdop;
  OptionalUInt satellites;
  uint32_t uptimeMilliseconds = 0;
};

class TrackingStorage {
 public:
  virtual ~TrackingStorage() = default;
  virtual bool startTrackingSession(uint32_t& trackingSessionNumber) = 0;
  virtual bool appendAndFlushRawPoint(const TrackPoint& point,
                                      const std::string& ndjson) = 0;
};

struct TrackingDecision {
  bool trackingSessionStarted = false;
  bool rawPointRecorded = false;
  bool rawPointWriteFailed = false;
  bool writeFilteredCsv = false;
  bool writeFilteredGpx = false;
  TrackPoint point;
};

class TrackingWorkflow {
 public:
  explicit TrackingWorkflow(const std::string& trackerId);

  TrackingDecision processFix(const GpsFix& fix, TrackingStorage& storage);
  bool trackingSessionActive() const;

 private:
  std::string trackerId_;
  uint8_t consecutiveStartCandidates_ = 0;
  bool active_ = false;
  uint32_t trackingSessionNumber_ = 0;
  uint32_t nextPointNumber_ = 1;
};

std::string serializeTrackPoint(const TrackPoint& point);

}  // namespace tracking
