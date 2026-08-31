#pragma once

#include <stdint.h>
#include <vector>

#include "delivery_recovery.h"

namespace tracking {

struct PendingDeliveryBatch {
  uint32_t trackingSessionNumber = 0;
  uint32_t firstPointNumber = 0;
  uint32_t pointCount = 0;
};

bool selectOldestPendingBatch(
    const std::vector<RecoveredTrackingSession>& sessions,
    PendingDeliveryBatch& batch);

class DeliveryRetrySchedule {
 public:
  uint32_t recordFailure();
  void recordSuccess();
  bool ready(uint32_t nowMilliseconds) const;
  uint32_t scheduleFailure(uint32_t nowMilliseconds);
  void scheduleSuccess(uint32_t nowMilliseconds);

 private:
  uint32_t consecutiveFailures_ = 0;
  uint32_t nextAttemptAtMilliseconds_ = 0;
};

}  // namespace tracking
