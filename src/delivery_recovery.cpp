#include "delivery_recovery.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace tracking {

namespace {

uint32_t checksum(uint32_t trackingSessionNumber,
                  uint32_t highestConfirmedPoint) {
  uint32_t value = 2166136261u;
  const uint32_t fields[] = {1u, trackingSessionNumber, highestConfirmedPoint};
  for (size_t field = 0; field < 3; ++field) {
    for (size_t byte = 0; byte < 4; ++byte) {
      value ^= (fields[field] >> (byte * 8)) & 0xffu;
      value *= 16777619u;
    }
  }
  return value;
}

bool parseRecord(const std::string& line, uint32_t expectedSession,
                 uint32_t& confirmedPoint) {
  unsigned long version = 0;
  unsigned long session = 0;
  unsigned long confirmed = 0;
  unsigned long storedChecksum = 0;
  char trailing = '\0';
  const int fields = std::sscanf(line.c_str(), "%lu,%lu,%lu,%lx%c", &version,
                                 &session, &confirmed, &storedChecksum,
                                 &trailing);
  if (fields != 4 || version != 1 || session != expectedSession ||
      session > UINT32_MAX || confirmed > UINT32_MAX ||
      storedChecksum > UINT32_MAX) {
    return false;
  }
  const uint32_t parsedConfirmed = static_cast<uint32_t>(confirmed);
  if (storedChecksum != checksum(static_cast<uint32_t>(session),
                                 parsedConfirmed)) {
    return false;
  }
  confirmedPoint = parsedConfirmed;
  return true;
}

bool bySessionNumber(const RecoveredTrackingSession& left,
                     const RecoveredTrackingSession& right) {
  return left.trackingSessionNumber < right.trackingSessionNumber;
}

}  // namespace

std::string serializeDeliveryProgress(uint32_t trackingSessionNumber,
                                      uint32_t highestConfirmedPoint) {
  char record[64];
  std::snprintf(record, sizeof(record), "1,%lu,%lu,%08lx\n",
                static_cast<unsigned long>(trackingSessionNumber),
                static_cast<unsigned long>(highestConfirmedPoint),
                static_cast<unsigned long>(
                    checksum(trackingSessionNumber, highestConfirmedPoint)));
  return record;
}

std::vector<RecoveredTrackingSession> recoverTrackingSessions(
    const std::vector<StoredTrackingSession>& storedSessions) {
  std::vector<RecoveredTrackingSession> recovered;
  recovered.reserve(storedSessions.size());
  for (std::vector<StoredTrackingSession>::const_iterator stored =
           storedSessions.begin();
       stored != storedSessions.end(); ++stored) {
    RecoveredTrackingSession session;
    session.trackingSessionNumber = stored->trackingSessionNumber;
    session.highestRecordedPoint = stored->highestRecordedPoint;

    std::istringstream records(stored->deliveryState);
    std::string line;
    while (std::getline(records, line)) {
      uint32_t confirmedPoint = 0;
      if (parseRecord(line, stored->trackingSessionNumber, confirmedPoint) &&
          confirmedPoint <= stored->highestRecordedPoint) {
        session.highestConfirmedPoint =
            std::max(session.highestConfirmedPoint, confirmedPoint);
      } else if (!line.empty()) {
        session.recoveryRequired = true;
      }
    }
    recovered.push_back(session);
  }
  std::sort(recovered.begin(), recovered.end(), bySessionNumber);
  return recovered;
}

}  // namespace tracking
