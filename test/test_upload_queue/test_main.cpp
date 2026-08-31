#include <unity.h>

#include <string>
#include <vector>

#include "upload_queue.h"

void setUp() {}
void tearDown() {}

namespace {

class FakePointSource : public tracking::UploadPointSource {
 public:
  tracking::BatchReadStatus status = tracking::BatchReadStatus::ready;
  size_t returnedLineCount = 30;
  bool returnEmptyLine = false;
  uint32_t requestedSession = 0;
  uint32_t requestedFirstPoint = 0;
  size_t requestedCount = 0;

  tracking::BatchReadStatus readPointLines(
      uint32_t trackingSessionNumber, uint32_t firstPointNumber,
      size_t pointCount, std::vector<std::string>& lines) override {
    requestedSession = trackingSessionNumber;
    requestedFirstPoint = firstPointNumber;
    requestedCount = pointCount;
    if (status != tracking::BatchReadStatus::ready) return status;
    for (size_t index = 0; index < returnedLineCount; ++index) {
      lines.push_back(returnEmptyLine && index == 4 ? "" : "{}");
    }
    return status;
  }
};

tracking::RecoveredTrackingSession session(uint32_t number, uint32_t recorded,
                                           uint32_t confirmed) {
  tracking::RecoveredTrackingSession value;
  value.trackingSessionNumber = number;
  value.highestRecordedPoint = recorded;
  value.highestConfirmedPoint = confirmed;
  return value;
}

void no_full_batch_is_distinct_from_storage_failures() {
  FakePointSource source;
  tracking::UploadBatch batch;
  std::vector<tracking::RecoveredTrackingSession> sessions;
  sessions.push_back(session(7, 29, 0));

  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(tracking::BatchReadStatus::no_full_batch),
      static_cast<int>(tracking::readFullPendingBatch(
          sessions, source, "tracker-01", batch)));
  TEST_ASSERT_EQUAL_UINT32(0, source.requestedSession);
}

void lock_open_and_malformed_failures_are_preserved() {
  const tracking::BatchReadStatus failures[] = {
      tracking::BatchReadStatus::lock_unavailable,
      tracking::BatchReadStatus::storage_open_failed,
      tracking::BatchReadStatus::malformed_input};
  std::vector<tracking::RecoveredTrackingSession> sessions;
  sessions.push_back(session(7, 30, 0));

  for (size_t index = 0; index < 3; ++index) {
    FakePointSource source;
    source.status = failures[index];
    tracking::UploadBatch batch;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(failures[index]),
        static_cast<int>(tracking::readFullPendingBatch(
            sessions, source, "tracker-01", batch)));
  }
}

void short_or_empty_input_is_malformed() {
  std::vector<tracking::RecoveredTrackingSession> sessions;
  sessions.push_back(session(7, 30, 0));
  tracking::UploadBatch batch;

  FakePointSource shortSource;
  shortSource.returnedLineCount = 29;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(tracking::BatchReadStatus::malformed_input),
      static_cast<int>(tracking::readFullPendingBatch(
          sessions, shortSource, "tracker-01", batch)));

  FakePointSource emptySource;
  emptySource.returnEmptyLine = true;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(tracking::BatchReadStatus::malformed_input),
      static_cast<int>(tracking::readFullPendingBatch(
          sessions, emptySource, "tracker-01", batch)));
}

void progress_ahead_of_recorded_points_is_malformed() {
  std::vector<tracking::RecoveredTrackingSession> sessions;
  sessions.push_back(session(7, 29, 30));
  FakePointSource source;
  tracking::UploadBatch batch;

  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(tracking::BatchReadStatus::malformed_input),
      static_cast<int>(tracking::readFullPendingBatch(
          sessions, source, "tracker-01", batch)));
  TEST_ASSERT_EQUAL_UINT32(0, source.requestedSession);
}

void oldest_full_batch_sets_identity_and_range() {
  std::vector<tracking::RecoveredTrackingSession> sessions;
  sessions.push_back(session(4, 31, 1));
  sessions.push_back(session(5, 90, 0));
  FakePointSource source;
  tracking::UploadBatch batch;

  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(tracking::BatchReadStatus::ready),
      static_cast<int>(tracking::readFullPendingBatch(
          sessions, source, "tracker-01", batch)));
  TEST_ASSERT_EQUAL_UINT32(4, batch.trackingSessionNumber);
  TEST_ASSERT_EQUAL_UINT32(2, batch.firstPointNumber);
  TEST_ASSERT_EQUAL_UINT32(4, source.requestedSession);
  TEST_ASSERT_EQUAL_UINT32(2, source.requestedFirstPoint);
  TEST_ASSERT_EQUAL_UINT32(30, source.requestedCount);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(no_full_batch_is_distinct_from_storage_failures);
  RUN_TEST(lock_open_and_malformed_failures_are_preserved);
  RUN_TEST(short_or_empty_input_is_malformed);
  RUN_TEST(progress_ahead_of_recorded_points_is_malformed);
  RUN_TEST(oldest_full_batch_sets_identity_and_range);
  return UNITY_END();
}
