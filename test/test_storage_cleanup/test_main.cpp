#include <unity.h>

#include <vector>

#include "storage_cleanup.h"

void setUp() {}
void tearDown() {}

namespace {

tracking::CleanupSession delivered(uint32_t number, uint64_t bytes) {
  tracking::CleanupSession session;
  session.trackingSessionNumber = number;
  session.bytes = bytes;
  session.inactive = true;
  session.allPointsConfirmed = true;
  session.diagnosticLogConfirmed = true;
  return session;
}

void cleanup_starts_at_eighty_percent_but_not_below() {
  const std::vector<tracking::CleanupSession> sessions = {delivered(41, 20)};

  const tracking::CleanupPlan below =
      tracking::planStorageCleanup(100, 79, sessions);
  TEST_ASSERT_FALSE(below.started);
  TEST_ASSERT_TRUE(below.sessionsToDelete.empty());

  const tracking::CleanupPlan atThreshold =
      tracking::planStorageCleanup(100, 80, sessions);
  TEST_ASSERT_TRUE(atThreshold.started);
  TEST_ASSERT_EQUAL_UINT32(1, atThreshold.sessionsToDelete.size());
  TEST_ASSERT_TRUE(atThreshold.targetReached);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(cleanup_starts_at_eighty_percent_but_not_below);
  return UNITY_END();
}
