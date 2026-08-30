#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace tracking {

struct StoredTrackingSession {
  uint32_t trackingSessionNumber = 0;
  uint32_t highestRecordedPoint = 0;
  std::string deliveryState;
};

struct RecoveredTrackingSession {
  uint32_t trackingSessionNumber = 0;
  uint32_t highestRecordedPoint = 0;
  uint32_t highestConfirmedPoint = 0;
  bool inactive = true;
  bool recoveryRequired = false;
};

std::string serializeDeliveryProgress(uint32_t trackingSessionNumber,
                                      uint32_t highestConfirmedPoint);

std::vector<RecoveredTrackingSession> recoverTrackingSessions(
    const std::vector<StoredTrackingSession>& storedSessions);

}  // namespace tracking
