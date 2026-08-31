#include <unity.h>

#include <algorithm>
#include <string>
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

class FakeCleanupStorage : public tracking::StorageCleanupStorage {
 public:
  uint64_t capacityBytes = 100;
  uint64_t usedBytes = 90;
  std::vector<tracking::CleanupSession> sessions;
  std::vector<uint32_t> deleted;
  std::vector<uint32_t> forgotten;
  std::string diagnostics;
  uint32_t usageReads = 0;
  uint64_t diagnosticBytes = 0;
  uint32_t deletionToFail = 0;
  tracking::CleanupStorageStatus usageStatus =
      tracking::CleanupStorageStatus::ok;
  tracking::CleanupStorageStatus scanStatus =
      tracking::CleanupStorageStatus::ok;
  tracking::CleanupStorageStatus diagnosticStatus =
      tracking::CleanupStorageStatus::ok;
  std::string pendingDiagnostic;

  tracking::CleanupStorageStatus readUsage(
      uint64_t& capacity, uint64_t& used) override {
    ++usageReads;
    if (usageStatus != tracking::CleanupStorageStatus::ok) return usageStatus;
    capacity = capacityBytes;
    used = usedBytes;
    return tracking::CleanupStorageStatus::ok;
  }

  tracking::CleanupStorageStatus readCleanupSessions(
      std::vector<tracking::CleanupSession>& result) override {
    if (scanStatus != tracking::CleanupStorageStatus::ok) return scanStatus;
    result = sessions;
    return tracking::CleanupStorageStatus::ok;
  }

  tracking::CleanupStorageStatus deleteSessionFiles(uint32_t number) override {
    deleted.push_back(number);
    if (number == deletionToFail) {
      return tracking::CleanupStorageStatus::deleteFailed;
    }
    for (std::vector<tracking::CleanupSession>::const_iterator session =
             sessions.begin();
         session != sessions.end(); ++session) {
      if (session->trackingSessionNumber == number) {
        usedBytes = session->bytes >= usedBytes ? 0 : usedBytes - session->bytes;
        break;
      }
    }
    return tracking::CleanupStorageStatus::ok;
  }

  void forgetDeletedSession(uint32_t number) override {
    forgotten.push_back(number);
  }

  tracking::CleanupStorageStatus flushPendingCleanupDiagnostic() override {
    if (pendingDiagnostic.empty()) return tracking::CleanupStorageStatus::ok;
    if (diagnosticStatus != tracking::CleanupStorageStatus::ok) {
      return diagnosticStatus;
    }
    diagnostics += pendingDiagnostic;
    pendingDiagnostic.clear();
    usedBytes += diagnosticBytes;
    return tracking::CleanupStorageStatus::ok;
  }

  tracking::CleanupStorageStatus persistCleanupDiagnostic(
      const std::string& message) override {
    if (diagnosticStatus != tracking::CleanupStorageStatus::ok) {
      pendingDiagnostic = message;
      return diagnosticStatus;
    }
    diagnostics += message;
    usedBytes += diagnosticBytes;
    return tracking::CleanupStorageStatus::ok;
  }
};

void cleanup_starts_at_eighty_percent_but_not_below() {
  const std::vector<tracking::CleanupSession> sessions = {delivered(41, 20)};

  const tracking::CleanupPlan below =
      tracking::planStorageCleanup(100, 79, sessions);
  TEST_ASSERT_FALSE(below.cleanupRequired);
  TEST_ASSERT_TRUE(below.sessionsToDelete.empty());

  const tracking::CleanupPlan atThreshold =
      tracking::planStorageCleanup(100, 80, sessions);
  TEST_ASSERT_TRUE(atThreshold.cleanupRequired);
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

  TEST_ASSERT_TRUE(plan.cleanupRequired);
  TEST_ASSERT_TRUE(plan.sessionsToDelete.empty());
  TEST_ASSERT_FALSE(plan.targetReached);
  TEST_ASSERT_TRUE(plan.protectedDataRemains);
}

void cleanup_rechecks_real_usage_after_deletion_and_diagnostic_writes() {
  FakeCleanupStorage storage;
  storage.sessions = {delivered(41, 21), delivered(42, 10)};
  storage.diagnosticBytes = 2;

  const tracking::CleanupRun run = tracking::runStorageCleanup(storage);

  TEST_ASSERT_EQUAL(static_cast<int>(tracking::CleanupOutcome::targetReached),
                    static_cast<int>(run.outcome));
  TEST_ASSERT_EQUAL_UINT32(2, storage.deleted.size());
  TEST_ASSERT_EQUAL_UINT32(41, storage.deleted[0]);
  TEST_ASSERT_EQUAL_UINT32(42, storage.deleted[1]);
  TEST_ASSERT_EQUAL_UINT32(2, storage.forgotten.size());
  TEST_ASSERT_EQUAL_UINT32(3, storage.usageReads);
  TEST_ASSERT_TRUE(storage.usedBytes < 70);
}

void partial_deletion_failure_is_persisted_and_does_not_reconcile_memory() {
  FakeCleanupStorage storage;
  storage.sessions = {delivered(41, 10), delivered(42, 25)};
  storage.deletionToFail = 41;

  const tracking::CleanupRun run = tracking::runStorageCleanup(storage);

  TEST_ASSERT_EQUAL(static_cast<int>(tracking::CleanupOutcome::targetReached),
                    static_cast<int>(run.outcome));
  TEST_ASSERT_EQUAL(static_cast<int>(tracking::CleanupStorageStatus::deleteFailed),
                    static_cast<int>(run.failure));
  TEST_ASSERT_EQUAL_UINT32(2, storage.deleted.size());
  TEST_ASSERT_EQUAL_UINT32(1, storage.forgotten.size());
  TEST_ASSERT_EQUAL_UINT32(42, storage.forgotten[0]);
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        storage.diagnostics.find("delete failed session=41"));
}

void pending_data_block_is_durable_and_waits_for_a_new_trigger() {
  FakeCleanupStorage storage;
  tracking::CleanupSession pending = delivered(41, 30);
  pending.allPointsConfirmed = false;
  storage.sessions = {pending};

  const tracking::CleanupRun run = tracking::runStorageCleanup(storage);
  tracking::StorageCleanupSchedule schedule;
  schedule.request(tracking::CleanupTrigger::startup);
  schedule.recordRun(run, 1000);

  TEST_ASSERT_EQUAL(static_cast<int>(tracking::CleanupOutcome::waitingForDelivery),
                    static_cast<int>(run.outcome));
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        storage.diagnostics.find("pending data protected"));
  TEST_ASSERT_FALSE(schedule.pending());
  TEST_ASSERT_FALSE(schedule.ready(61000));
  schedule.request(tracking::CleanupTrigger::deliveryConfirmation);
  TEST_ASSERT_TRUE(schedule.ready(61000));
}

void recoverable_failures_retry_after_the_named_delay() {
  FakeCleanupStorage storage;
  storage.usageStatus = tracking::CleanupStorageStatus::lockUnavailable;
  const tracking::CleanupRun run = tracking::runStorageCleanup(storage);
  tracking::StorageCleanupSchedule schedule;
  schedule.request(tracking::CleanupTrigger::startup);
  schedule.recordRun(run, 1000);

  TEST_ASSERT_EQUAL(static_cast<int>(tracking::CleanupOutcome::retryRequired),
                    static_cast<int>(run.outcome));
  TEST_ASSERT_EQUAL(static_cast<int>(tracking::CleanupStorageStatus::lockUnavailable),
                    static_cast<int>(run.failure));
  TEST_ASSERT_FALSE(schedule.ready(60999));
  TEST_ASSERT_TRUE(schedule.ready(61000));

  FakeCleanupStorage metricFailure;
  metricFailure.usageStatus =
      tracking::CleanupStorageStatus::metricUnavailable;
  const tracking::CleanupRun metricRun =
      tracking::runStorageCleanup(metricFailure);
  TEST_ASSERT_EQUAL(
      static_cast<int>(tracking::CleanupStorageStatus::metricUnavailable),
      static_cast<int>(metricRun.failure));
}

void scan_and_diagnostic_failures_remain_visible_and_retryable() {
  FakeCleanupStorage scanFailure;
  scanFailure.scanStatus = tracking::CleanupStorageStatus::scanFailed;
  tracking::CleanupRun run = tracking::runStorageCleanup(scanFailure);
  TEST_ASSERT_EQUAL(static_cast<int>(tracking::CleanupOutcome::retryRequired),
                    static_cast<int>(run.outcome));
  TEST_ASSERT_EQUAL(static_cast<int>(tracking::CleanupStorageStatus::scanFailed),
                    static_cast<int>(run.failure));

  FakeCleanupStorage diagnosticFailure;
  tracking::CleanupSession pending = delivered(41, 30);
  pending.diagnosticLogConfirmed = false;
  diagnosticFailure.sessions = {pending};
  diagnosticFailure.diagnosticStatus =
      tracking::CleanupStorageStatus::diagnosticWriteFailed;
  run = tracking::runStorageCleanup(diagnosticFailure);
  TEST_ASSERT_EQUAL(static_cast<int>(tracking::CleanupOutcome::retryRequired),
                    static_cast<int>(run.outcome));
  TEST_ASSERT_EQUAL(
      static_cast<int>(tracking::CleanupStorageStatus::diagnosticWriteFailed),
      static_cast<int>(run.failure));

  diagnosticFailure.diagnosticStatus = tracking::CleanupStorageStatus::ok;
  run = tracking::runStorageCleanup(diagnosticFailure);
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        diagnosticFailure.diagnostics.find(
                            "pending data protected"));
  TEST_ASSERT_EQUAL(static_cast<int>(tracking::CleanupOutcome::waitingForDelivery),
                    static_cast<int>(run.outcome));
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(cleanup_starts_at_eighty_percent_but_not_below);
  RUN_TEST(cleanup_deletes_oldest_delivered_sessions_until_strictly_below_seventy);
  RUN_TEST(pending_points_or_diagnostic_logs_protect_the_whole_session);
  RUN_TEST(cleanup_reports_when_protected_data_prevents_the_target);
  RUN_TEST(cleanup_rechecks_real_usage_after_deletion_and_diagnostic_writes);
  RUN_TEST(partial_deletion_failure_is_persisted_and_does_not_reconcile_memory);
  RUN_TEST(pending_data_block_is_durable_and_waits_for_a_new_trigger);
  RUN_TEST(recoverable_failures_retry_after_the_named_delay);
  RUN_TEST(scan_and_diagnostic_failures_remain_visible_and_retryable);
  return UNITY_END();
}
