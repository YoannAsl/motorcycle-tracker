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

void cleanup_deletes_oldest_delivered_sessions_until_strictly_below_seventy() {
  const std::vector<tracking::CleanupSession> sessions = {
      delivered(44, 11), delivered(41, 10), delivered(43, 10)};

  const tracking::CleanupPlan plan =
      tracking::planStorageCleanup(100, 90, sessions);

  TEST_ASSERT_EQUAL_UINT32(3, plan.sessionsToDelete.size());
  TEST_ASSERT_EQUAL_UINT32(41, plan.sessionsToDelete[0]);
  TEST_ASSERT_EQUAL_UINT32(43, plan.sessionsToDelete[1]);
  TEST_ASSERT_EQUAL_UINT32(44, plan.sessionsToDelete[2]);
  TEST_ASSERT_TRUE(plan.targetReached);
}

void pending_points_or_diagnostic_logs_protect_the_whole_session() {
  tracking::CleanupSession pendingPoints = delivered(41, 20);
  pendingPoints.allPointsConfirmed = false;
  tracking::CleanupSession pendingDiagnostic = delivered(42, 20);
  pendingDiagnostic.diagnosticLogConfirmed = false;
  const std::vector<tracking::CleanupSession> sessions = {
      pendingPoints, pendingDiagnostic, delivered(43, 20)};

  const tracking::CleanupPlan plan =
      tracking::planStorageCleanup(100, 90, sessions);

  TEST_ASSERT_EQUAL_UINT32(1, plan.sessionsToDelete.size());
  TEST_ASSERT_EQUAL_UINT32(43, plan.sessionsToDelete[0]);
  TEST_ASSERT_FALSE(plan.targetReached);
  TEST_ASSERT_TRUE(plan.protectedDataRemains);
}

void cleanup_reports_when_protected_data_prevents_the_target() {
  tracking::CleanupSession pending = delivered(41, 30);
  pending.allPointsConfirmed = false;

  const tracking::CleanupPlan plan =
      tracking::planStorageCleanup(1000, 800, {pending});

  TEST_ASSERT_TRUE(plan.started);
  TEST_ASSERT_TRUE(plan.sessionsToDelete.empty());
  TEST_ASSERT_FALSE(plan.targetReached);
  TEST_ASSERT_TRUE(plan.protectedDataRemains);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(cleanup_starts_at_eighty_percent_but_not_below);
  RUN_TEST(cleanup_deletes_oldest_delivered_sessions_until_strictly_below_seventy);
  RUN_TEST(pending_points_or_diagnostic_logs_protect_the_whole_session);
  RUN_TEST(cleanup_reports_when_protected_data_prevents_the_target);
  return UNITY_END();
}
