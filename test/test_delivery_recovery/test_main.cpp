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

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(reboot_recovers_recorded_points_and_confirmed_progress);
  return UNITY_END();
}
