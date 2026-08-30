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

 private:
  uint32_t consecutiveFailures_ = 0;
};

}  // namespace tracking
