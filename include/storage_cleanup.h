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
  bool protectedDataRemains = false;
  std::vector<uint32_t> sessionsToDelete;
};

bool storageCleanupRequired(uint64_t capacityBytes, uint64_t usedBytes);
bool storageCleanupTargetReached(uint64_t capacityBytes, uint64_t usedBytes);

CleanupPlan planStorageCleanup(
    uint64_t capacityBytes, uint64_t usedBytes,
    const std::vector<CleanupSession>& sessions);

}  // namespace tracking
