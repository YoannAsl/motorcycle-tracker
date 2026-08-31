#include <unity.h>

#include <map>
#include <string>
#include <vector>

#include "delivery_recovery.h"

void setUp() {}
void tearDown() {}

namespace {

class FakeRecoveryStorage : public tracking::DeliveryRecoveryStorage {
 public:
  struct FileValue {
    tracking::RecoveryReadStatus status =
        tracking::RecoveryReadStatus::missing;
    std::string contents;
  };

  bool listRoot(std::vector<tracking::RecoveryDirectoryEntry>& result) override {
    if (!rootReadable) return false;
    result = entries;
    return true;
  }

  tracking::RecoveryReadStatus countCompleteLines(
      const std::string& path, uint32_t& count) override {
    requestedPaths.push_back(path);
    const FileValue value = files[path];
    if (value.status != tracking::RecoveryReadStatus::readable) {
      return value.status;
    }
    count = 0;
    for (std::string::const_iterator character = value.contents.begin();
         character != value.contents.end(); ++character) {
      if (*character == '\n') ++count;
    }
    return value.status;
  }

  tracking::RecoveryReadStatus readText(const std::string& path,
                                        std::string& contents) override {
    requestedPaths.push_back(path);
    const FileValue value = files[path];
    if (value.status == tracking::RecoveryReadStatus::readable) {
      contents = value.contents;
    }
    return value.status;
  }

  bool appendText(const std::string& path,
                  const std::string& contents) override {
    if (!appendSucceeds) return false;
    FileValue& value = files[path];
    value.status = tracking::RecoveryReadStatus::readable;
    value.contents += contents;
    return true;
  }

  void addSession(uint32_t number, uint32_t pointCount,
                  tracking::RecoveryReadStatus stateStatus,
                  const std::string& state = std::string()) {
    const std::string base = tracking::sessionDirectoryPath(number);
    tracking::RecoveryDirectoryEntry entry;
    entry.name = base.substr(1);
    entry.directory = true;
    entries.push_back(entry);
    FileValue points;
    points.status = tracking::RecoveryReadStatus::readable;
    for (uint32_t point = 0; point < pointCount; ++point) {
      points.contents += "{}\n";
    }
    files[base + "/track-points.ndjson"] = points;
    FileValue delivery;
    delivery.status = stateStatus;
    delivery.contents = state;
    files[base + "/delivery-state.log"] = delivery;
  }

  bool rootReadable = true;
  bool appendSucceeds = true;
  std::vector<tracking::RecoveryDirectoryEntry> entries;
  std::map<std::string, FileValue> files;
  std::vector<std::string> requestedPaths;
};

void reboot_recovers_recorded_points_and_confirmed_progress() {
  tracking::StoredTrackingSession stored;
  stored.trackingSessionNumber = 41;
  stored.highestRecordedPoint = 4;
  stored.deliveryState = tracking::serializeDeliveryProgress(41, 2);

  const std::vector<tracking::RecoveredTrackingSession> recovered =
      tracking::recoverTrackingSessions({stored});

  TEST_ASSERT_EQUAL_UINT32(1, recovered.size());
  TEST_ASSERT_EQUAL_UINT32(41, recovered[0].trackingSessionNumber);
  TEST_ASSERT_EQUAL_UINT32(4, recovered[0].highestRecordedPoint);
  TEST_ASSERT_EQUAL_UINT32(2, recovered[0].highestConfirmedPoint);
  TEST_ASSERT_TRUE(recovered[0].inactive);
  TEST_ASSERT_FALSE(recovered[0].recoveryRequired);
}

void damaged_latest_progress_resends_from_the_last_safe_point() {
  tracking::StoredTrackingSession stored;
  stored.trackingSessionNumber = 42;
  stored.highestRecordedPoint = 60;
  stored.deliveryState = tracking::serializeDeliveryProgress(42, 30);
  stored.deliveryState += "1,42,60,damaged\n";

  const std::vector<tracking::RecoveredTrackingSession> recovered =
      tracking::recoverTrackingSessions({stored});

  TEST_ASSERT_EQUAL_UINT32(30, recovered[0].highestConfirmedPoint);
  TEST_ASSERT_TRUE(recovered[0].recoveryRequired);
}

void progress_ahead_of_recorded_data_is_rejected() {
  tracking::StoredTrackingSession stored;
  stored.trackingSessionNumber = 43;
  stored.highestRecordedPoint = 20;
  stored.deliveryState = tracking::serializeDeliveryProgress(43, 15);
  stored.deliveryState += tracking::serializeDeliveryProgress(43, 30);

  const std::vector<tracking::RecoveredTrackingSession> recovered =
      tracking::recoverTrackingSessions({stored});

  TEST_ASSERT_EQUAL_UINT32(15, recovered[0].highestConfirmedPoint);
  TEST_ASSERT_TRUE(recovered[0].recoveryRequired);
}

void older_progress_triggers_safe_resend_and_sessions_recover_oldest_first() {
  tracking::StoredTrackingSession newer;
  newer.trackingSessionNumber = 44;
  newer.highestRecordedPoint = 60;
  newer.deliveryState = tracking::serializeDeliveryProgress(44, 30);
  newer.deliveryState += tracking::serializeDeliveryProgress(44, 20);

  tracking::StoredTrackingSession older;
  older.trackingSessionNumber = 41;
  older.highestRecordedPoint = 4;

  const std::vector<tracking::RecoveredTrackingSession> recovered =
      tracking::recoverTrackingSessions({newer, older});

  TEST_ASSERT_EQUAL_UINT32(41, recovered[0].trackingSessionNumber);
  TEST_ASSERT_EQUAL_UINT32(44, recovered[1].trackingSessionNumber);
  TEST_ASSERT_EQUAL_UINT32(20, recovered[1].highestConfirmedPoint);
  TEST_ASSERT_TRUE(recovered[1].recoveryRequired);
}

void reboot_scan_restores_paths_identity_pending_data_and_progress() {
  FakeRecoveryStorage storage;
  tracking::RecoveryDirectoryEntry unrelated;
  unrelated.name = "notes";
  storage.entries.push_back(unrelated);
  tracking::RecoveryDirectoryEntry invalid;
  invalid.name = "session-invalid";
  invalid.directory = true;
  storage.entries.push_back(invalid);
  storage.addSession(41, 4, tracking::RecoveryReadStatus::readable,
                     tracking::serializeDeliveryProgress(41, 2));
  storage.addSession(105, 2, tracking::RecoveryReadStatus::missing);

  tracking::DeliveryRecovery recovery;
  TEST_ASSERT_TRUE(recovery.restore(storage));
  TEST_ASSERT_EQUAL_UINT32(105, recovery.maxStoredTrackingSessionNumber());
  TEST_ASSERT_EQUAL_UINT32(2, recovery.sessions().size());
  TEST_ASSERT_EQUAL_UINT32(41,
                           recovery.oldestPendingSession()->trackingSessionNumber);
  TEST_ASSERT_TRUE(recovery.sessions()[0].inactive);
  TEST_ASSERT_EQUAL_UINT32(2, recovery.sessions()[0].highestConfirmedPoint);
  TEST_ASSERT_TRUE(recovery.sessions()[1].recoveryRequired);
  TEST_ASSERT_EQUAL_STRING("/session-0000000041/track-points.ndjson",
                           storage.requestedPaths[0].c_str());
  TEST_ASSERT_EQUAL_STRING("/session-0000000041/delivery-state.log",
                           storage.requestedPaths[1].c_str());
}

void reboot_ignores_unterminated_point_tail_and_keeps_complete_lines_pending() {
  FakeRecoveryStorage storage;
  storage.addSession(42, 2, tracking::RecoveryReadStatus::missing);
  storage.files["/session-0000000042/track-points.ndjson"].contents +=
      "{\"point_number\":";

  tracking::DeliveryRecovery recovery;
  TEST_ASSERT_TRUE(recovery.restore(storage));
  TEST_ASSERT_EQUAL_UINT32(2, recovery.sessions()[0].highestRecordedPoint);
  TEST_ASSERT_EQUAL_UINT32(
      42, recovery.oldestPendingSession()->trackingSessionNumber);
  TEST_ASSERT_EQUAL_UINT32(0,
                           recovery.sessions()[0].highestConfirmedPoint);
}

void next_session_number_reconciles_persisted_and_scanned_identity() {
  uint32_t next = 0;
  TEST_ASSERT_TRUE(tracking::nextTrackingSessionNumber(40, 105, next));
  TEST_ASSERT_EQUAL_UINT32(106, next);
  TEST_ASSERT_TRUE(tracking::nextTrackingSessionNumber(200, 105, next));
  TEST_ASSERT_EQUAL_UINT32(201, next);
  TEST_ASSERT_FALSE(
      tracking::nextTrackingSessionNumber(UINT32_MAX, 105, next));
}

void unreadable_raw_points_fail_scan_instead_of_looking_empty() {
  FakeRecoveryStorage storage;
  storage.addSession(8, 1, tracking::RecoveryReadStatus::missing);
  storage.files["/session-0000000008/track-points.ndjson"].status =
      tracking::RecoveryReadStatus::unreadable;

  tracking::DeliveryRecovery recovery;
  TEST_ASSERT_FALSE(recovery.restore(storage));
  TEST_ASSERT_FALSE(recovery.ready());
  TEST_ASSERT_EQUAL_UINT32(0, recovery.sessions().size());
  TEST_ASSERT_EQUAL_UINT32(0, recovery.maxStoredTrackingSessionNumber());
}

void unreadable_or_blank_delivery_state_resends_safely() {
  FakeRecoveryStorage storage;
  storage.addSession(9, 3, tracking::RecoveryReadStatus::unreadable);
  storage.addSession(10, 3, tracking::RecoveryReadStatus::readable, "\n");

  tracking::DeliveryRecovery recovery;
  TEST_ASSERT_TRUE(recovery.restore(storage));
  TEST_ASSERT_EQUAL_UINT32(0, recovery.sessions()[0].highestConfirmedPoint);
  TEST_ASSERT_TRUE(recovery.sessions()[0].recoveryRequired);
  TEST_ASSERT_EQUAL_UINT32(0, recovery.sessions()[1].highestConfirmedPoint);
  TEST_ASSERT_TRUE(recovery.sessions()[1].recoveryRequired);
}

void confirmed_progress_is_persisted_and_used_after_the_next_reboot() {
  FakeRecoveryStorage storage;
  storage.addSession(12, 4, tracking::RecoveryReadStatus::missing);
  tracking::DeliveryRecovery firstBoot;
  TEST_ASSERT_TRUE(firstBoot.restore(storage));
  TEST_ASSERT_TRUE(firstBoot.confirmDeliveryThrough(storage, 12, 2));
  TEST_ASSERT_EQUAL_UINT32(2,
                           firstBoot.oldestPendingSession()->highestConfirmedPoint);

  tracking::DeliveryRecovery secondBoot;
  TEST_ASSERT_TRUE(secondBoot.restore(storage));
  TEST_ASSERT_EQUAL_UINT32(2,
                           secondBoot.oldestPendingSession()->highestConfirmedPoint);
  TEST_ASSERT_FALSE(secondBoot.sessions()[0].recoveryRequired);
}

void failed_confirmation_write_does_not_advance_recovered_progress() {
  FakeRecoveryStorage storage;
  storage.addSession(13, 4, tracking::RecoveryReadStatus::missing);
  tracking::DeliveryRecovery recovery;
  TEST_ASSERT_TRUE(recovery.restore(storage));
  storage.appendSucceeds = false;

  TEST_ASSERT_FALSE(recovery.confirmDeliveryThrough(storage, 13, 2));
  TEST_ASSERT_EQUAL_UINT32(0,
                           recovery.oldestPendingSession()->highestConfirmedPoint);
}

void recorded_points_without_delivery_state_are_pending_with_a_recovery_reason() {
  tracking::StoredTrackingSession stored;
  stored.trackingSessionNumber = 45;
  stored.highestRecordedPoint = 3;

  const std::vector<tracking::RecoveredTrackingSession> recovered =
      tracking::recoverTrackingSessions({stored});

  TEST_ASSERT_EQUAL_UINT32(0, recovered[0].highestConfirmedPoint);
  TEST_ASSERT_TRUE(recovered[0].recoveryRequired);
}

void active_session_points_join_the_recovered_delivery_state() {
  FakeRecoveryStorage storage;
  tracking::DeliveryRecovery recovery;
  TEST_ASSERT_TRUE(recovery.restore(storage));

  TEST_ASSERT_TRUE(recovery.beginSession(1));
  TEST_ASSERT_TRUE(recovery.recordPoint(1));
  TEST_ASSERT_EQUAL_UINT32(1, recovery.sessions().size());
  TEST_ASSERT_FALSE(recovery.sessions()[0].inactive);
  TEST_ASSERT_EQUAL_UINT32(1, recovery.sessions()[0].highestRecordedPoint);
  TEST_ASSERT_FALSE(recovery.beginSession(1));
  TEST_ASSERT_FALSE(recovery.recordPoint(2));
}

void deleted_session_is_removed_from_recovered_memory() {
  FakeRecoveryStorage storage;
  storage.addSession(41, 2, tracking::RecoveryReadStatus::missing);
  storage.addSession(42, 2, tracking::RecoveryReadStatus::missing);
  tracking::DeliveryRecovery recovery;
  TEST_ASSERT_TRUE(recovery.restore(storage));

  recovery.forgetSession(41);

  TEST_ASSERT_EQUAL_UINT32(1, recovery.sessions().size());
  TEST_ASSERT_EQUAL_UINT32(42, recovery.sessions()[0].trackingSessionNumber);
  TEST_ASSERT_EQUAL_UINT32(42, recovery.maxStoredTrackingSessionNumber());
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(reboot_recovers_recorded_points_and_confirmed_progress);
  RUN_TEST(damaged_latest_progress_resends_from_the_last_safe_point);
  RUN_TEST(progress_ahead_of_recorded_data_is_rejected);
  RUN_TEST(older_progress_triggers_safe_resend_and_sessions_recover_oldest_first);
  RUN_TEST(recorded_points_without_delivery_state_are_pending_with_a_recovery_reason);
  RUN_TEST(reboot_scan_restores_paths_identity_pending_data_and_progress);
  RUN_TEST(
      reboot_ignores_unterminated_point_tail_and_keeps_complete_lines_pending);
  RUN_TEST(next_session_number_reconciles_persisted_and_scanned_identity);
  RUN_TEST(unreadable_raw_points_fail_scan_instead_of_looking_empty);
  RUN_TEST(unreadable_or_blank_delivery_state_resends_safely);
  RUN_TEST(confirmed_progress_is_persisted_and_used_after_the_next_reboot);
  RUN_TEST(failed_confirmation_write_does_not_advance_recovered_progress);
  RUN_TEST(active_session_points_join_the_recovered_delivery_state);
  RUN_TEST(deleted_session_is_removed_from_recovered_memory);
  return UNITY_END();
}
