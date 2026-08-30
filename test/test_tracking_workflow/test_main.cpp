#include <unity.h>

#include <string>
#include <vector>

#include "tracking_workflow.h"

void setUp() {}
void tearDown() {}

namespace {

class ScenarioStorage : public tracking::TrackingStorage {
 public:
  bool startTrackingSession(uint32_t& trackingSessionNumber) override {
    ++startAttempts;
    trackingSessionNumber = 41;
    return true;
  }

  bool appendAndFlushRawPoint(const tracking::TrackPoint& point,
                              const std::string& ndjson) override {
    points.push_back(point);
    lines.push_back(ndjson);
    return true;
  }

  int startAttempts = 0;
  std::vector<tracking::TrackPoint> points;
  std::vector<std::string> lines;
};

tracking::GpsFix stationaryFix() {
  tracking::GpsFix fix;
  fix.locationValid = true;
  fix.locationFresh = true;
  fix.latitude = 48.856613;
  fix.longitude = 2.352222;
  fix.utcValid = true;
  fix.utc = "2026-08-30T12:00:00Z";
  fix.speedKmh.valid = true;
  fix.speedKmh.value = 0.0;
  fix.hdop.valid = true;
  fix.hdop.value = 0.9;
  return fix;
}

tracking::GpsFix movingFix() {
  tracking::GpsFix fix = stationaryFix();
  fix.speedKmh.valid = true;
  fix.speedKmh.value = 12.5;
  return fix;
}

void boot_and_stationary_fixes_create_no_tracking_session() {
  tracking::TrackingWorkflow workflow("tracker-01");
  ScenarioStorage storage;

  const tracking::TrackingDecision decision =
      workflow.processFix(stationaryFix(), storage);

  TEST_ASSERT_FALSE(workflow.trackingSessionActive());
  TEST_ASSERT_FALSE(decision.rawPointRecorded);
  TEST_ASSERT_EQUAL(0, storage.startAttempts);
  TEST_ASSERT_TRUE(storage.points.empty());
}

void third_qualifying_fix_starts_with_point_one() {
  tracking::TrackingWorkflow workflow("tracker-01");
  ScenarioStorage storage;

  const tracking::TrackingDecision first = workflow.processFix(movingFix(), storage);
  const tracking::TrackingDecision second = workflow.processFix(movingFix(), storage);
  const tracking::TrackingDecision third = workflow.processFix(movingFix(), storage);

  TEST_ASSERT_FALSE(first.rawPointRecorded);
  TEST_ASSERT_FALSE(second.rawPointRecorded);
  TEST_ASSERT_TRUE(third.trackingSessionStarted);
  TEST_ASSERT_TRUE(third.rawPointRecorded);
  TEST_ASSERT_EQUAL_UINT32(1, third.point.pointNumber);
  TEST_ASSERT_EQUAL_UINT32(41, third.point.trackingSessionNumber);
  TEST_ASSERT_EQUAL(1, storage.startAttempts);
  TEST_ASSERT_EQUAL_UINT32(1, storage.points.size());
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(boot_and_stationary_fixes_create_no_tracking_session);
  RUN_TEST(third_qualifying_fix_starts_with_point_one);
  return UNITY_END();
}
