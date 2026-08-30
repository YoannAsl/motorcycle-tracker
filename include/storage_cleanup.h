#pragma once

#include <stdint.h>
#include <vector>

namespace tracking {

struct CleanupSession {
  uint32_t trackingSessionNumber = 0;
  uint64_t bytes = 0;
  bool inactive = false;
  bool allPointsConfirmed = false;
  bool diagnosticLogConfirmed = false;
};

struct CleanupPlan {
  bool started = false;
  bool targetReached = false;
  std::vector<uint32_t> sessionsToDelete;
};

CleanupPlan planStorageCleanup(
    uint64_t capacityBytes, uint64_t usedBytes,
    const std::vector<CleanupSession>& sessions);

}  // namespace tracking
