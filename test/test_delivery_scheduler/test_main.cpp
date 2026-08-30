#include <unity.h>

#include <vector>

#include "delivery_scheduler.h"

void setUp() {}
void tearDown() {}

namespace {

tracking::RecoveredTrackingSession session(uint32_t number,
                                           uint32_t recorded,
                                           uint32_t confirmed,
                                           bool inactive) {
  tracking::RecoveredTrackingSession value;
  value.trackingSessionNumber = number;
  value.highestRecordedPoint = recorded;
  value.highestConfirmedPoint = confirmed;
  value.inactive = inactive;
  return value;
}

void active_session_exposes_only_complete_thirty_point_batches() {
  tracking::PendingDeliveryBatch batch;

  TEST_ASSERT_FALSE(tracking::selectOldestPendingBatch(
      {session(41, 29, 0, false)}, batch));
  TEST_ASSERT_TRUE(tracking::selectOldestPendingBatch(
      {session(41, 30, 0, false)}, batch));
  TEST_ASSERT_EQUAL_UINT32(41, batch.trackingSessionNumber);
  TEST_ASSERT_EQUAL_UINT32(1, batch.firstPointNumber);
  TEST_ASSERT_EQUAL_UINT32(30, batch.pointCount);
}

void inactive_session_exposes_its_final_partial_batch() {
  tracking::PendingDeliveryBatch batch;

  TEST_ASSERT_TRUE(tracking::selectOldestPendingBatch(
      {session(41, 34, 30, true)}, batch));
  TEST_ASSERT_EQUAL_UINT32(41, batch.trackingSessionNumber);
  TEST_ASSERT_EQUAL_UINT32(31, batch.firstPointNumber);
  TEST_ASSERT_EQUAL_UINT32(4, batch.pointCount);
}

void oldest_eligible_pending_range_is_selected_first() {
  tracking::PendingDeliveryBatch batch;
  const std::vector<tracking::RecoveredTrackingSession> sessions = {
      session(44, 60, 30, true), session(41, 7, 0, true),
      session(43, 60, 0, true)};

  TEST_ASSERT_TRUE(tracking::selectOldestPendingBatch(sessions, batch));
  TEST_ASSERT_EQUAL_UINT32(41, batch.trackingSessionNumber);
  TEST_ASSERT_EQUAL_UINT32(1, batch.firstPointNumber);
  TEST_ASSERT_EQUAL_UINT32(7, batch.pointCount);
}

void retry_sequence_caps_at_five_minutes_and_success_resets_it() {
  tracking::DeliveryRetrySchedule retry;
  const uint32_t expected[] = {15, 30, 60, 120, 300, 300};
  for (size_t index = 0; index < sizeof(expected) / sizeof(expected[0]);
       ++index) {
    TEST_ASSERT_EQUAL_UINT32(expected[index], retry.recordFailure());
  }

  retry.recordSuccess();
  TEST_ASSERT_EQUAL_UINT32(15, retry.recordFailure());
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(active_session_exposes_only_complete_thirty_point_batches);
  RUN_TEST(inactive_session_exposes_its_final_partial_batch);
  RUN_TEST(oldest_eligible_pending_range_is_selected_first);
  RUN_TEST(retry_sequence_caps_at_five_minutes_and_success_resets_it);
  return UNITY_END();
}
