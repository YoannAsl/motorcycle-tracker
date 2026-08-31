#include "delivery_scheduler.h"

#include <algorithm>

namespace tracking {

bool selectOldestPendingBatch(
    const std::vector<RecoveredTrackingSession>& sessions,
    PendingDeliveryBatch& batch) {
  const RecoveredTrackingSession* selected = nullptr;
  for (std::vector<RecoveredTrackingSession>::const_iterator it =
           sessions.begin();
       it != sessions.end(); ++it) {
    if (it->highestConfirmedPoint >= it->highestRecordedPoint) continue;
    const uint32_t pending =
        it->highestRecordedPoint - it->highestConfirmedPoint;
    if (!it->inactive && pending < 30) continue;
    if (selected == nullptr ||
        it->trackingSessionNumber < selected->trackingSessionNumber) {
      selected = &*it;
    }
  }
  if (selected == nullptr) return false;

  const uint32_t pending =
      selected->highestRecordedPoint - selected->highestConfirmedPoint;
  batch.trackingSessionNumber = selected->trackingSessionNumber;
  batch.firstPointNumber = selected->highestConfirmedPoint + 1;
  batch.pointCount = std::min<uint32_t>(pending, 30);
  return true;
}

uint32_t DeliveryRetrySchedule::recordFailure() {
  static const uint32_t delays[] = {15, 30, 60, 120, 300};
  const uint32_t index =
      std::min<uint32_t>(consecutiveFailures_,
                         sizeof(delays) / sizeof(delays[0]) - 1);
  if (consecutiveFailures_ < UINT32_MAX) ++consecutiveFailures_;
  return delays[index];
}

void DeliveryRetrySchedule::recordSuccess() { consecutiveFailures_ = 0; }

bool DeliveryRetrySchedule::ready(uint32_t nowMilliseconds) const {
  return static_cast<int32_t>(nowMilliseconds - nextAttemptAtMilliseconds_) >= 0;
}

uint32_t DeliveryRetrySchedule::scheduleFailure(uint32_t nowMilliseconds) {
  const uint32_t delaySeconds = recordFailure();
  nextAttemptAtMilliseconds_ = nowMilliseconds + delaySeconds * 1000;
  return delaySeconds;
}

void DeliveryRetrySchedule::scheduleSuccess(uint32_t nowMilliseconds) {
  recordSuccess();
  nextAttemptAtMilliseconds_ = nowMilliseconds;
}

}  // namespace tracking
