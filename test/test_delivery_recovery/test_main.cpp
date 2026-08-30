#include <unity.h>

#include <string>
#include <vector>

#include "delivery_recovery.h"

void setUp() {}
void tearDown() {}

namespace {

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

void valid_progress_never_moves_backwards_and_sessions_recover_oldest_first() {
  tracking::StoredTrackingSession newer;
  newer.trackingSessionNumber = 44;
  newer.highestRecordedPoint = 60;
  newer.deliveryState = tracking::serializeDeliveryProgress(44, 30);
  newer.deliveryState += tracking::serializeDeliveryProgress(44, 20);
  newer.deliveryState += tracking::serializeDeliveryProgress(44, 60);

  tracking::StoredTrackingSession older;
  older.trackingSessionNumber = 41;
  older.highestRecordedPoint = 4;

  const std::vector<tracking::RecoveredTrackingSession> recovered =
      tracking::recoverTrackingSessions({newer, older});

  TEST_ASSERT_EQUAL_UINT32(41, recovered[0].trackingSessionNumber);
  TEST_ASSERT_EQUAL_UINT32(44, recovered[1].trackingSessionNumber);
  TEST_ASSERT_EQUAL_UINT32(60, recovered[1].highestConfirmedPoint);
  TEST_ASSERT_FALSE(recovered[1].recoveryRequired);
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

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(reboot_recovers_recorded_points_and_confirmed_progress);
  RUN_TEST(damaged_latest_progress_resends_from_the_last_safe_point);
  RUN_TEST(progress_ahead_of_recorded_data_is_rejected);
  RUN_TEST(valid_progress_never_moves_backwards_and_sessions_recover_oldest_first);
  RUN_TEST(recorded_points_without_delivery_state_are_pending_with_a_recovery_reason);
  return UNITY_END();
}
