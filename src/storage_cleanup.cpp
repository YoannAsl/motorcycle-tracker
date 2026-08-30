#include "storage_cleanup.h"

#include <algorithm>

namespace tracking {

namespace {

uint64_t ceilingFraction(uint64_t value, uint64_t numerator,
                         uint64_t denominator) {
  const uint64_t quotient = value / denominator;
  const uint64_t remainder = value % denominator;
  return quotient * numerator +
         (remainder * numerator + denominator - 1) / denominator;
}

bool oldestFirst(const CleanupSession& left, const CleanupSession& right) {
  return left.trackingSessionNumber < right.trackingSessionNumber;
}

}  // namespace

CleanupPlan planStorageCleanup(
    uint64_t capacityBytes, uint64_t usedBytes,
    const std::vector<CleanupSession>& sessions) {
  CleanupPlan plan;
  if (capacityBytes == 0 ||
      usedBytes < ceilingFraction(capacityBytes, 4, 5)) {
    return plan;
  }

  plan.started = true;
  const uint64_t target = ceilingFraction(capacityBytes, 7, 10);
  std::vector<CleanupSession> oldest = sessions;
  std::sort(oldest.begin(), oldest.end(), oldestFirst);
  for (std::vector<CleanupSession>::const_iterator session = oldest.begin();
       session != oldest.end() && usedBytes >= target; ++session) {
    if (!session->inactive || !session->allPointsConfirmed ||
        !session->diagnosticLogConfirmed) {
      continue;
    }
    plan.sessionsToDelete.push_back(session->trackingSessionNumber);
    usedBytes = session->bytes >= usedBytes ? 0 : usedBytes - session->bytes;
  }
  plan.targetReached = usedBytes < target;
  return plan;
}

}  // namespace tracking
