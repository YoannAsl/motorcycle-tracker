#include <unity.h>

#include <cmath>
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
    ++appendAttempts;
    if (!appendSucceeds) return false;
    points.push_back(point);
    lines.push_back(ndjson);
    return true;
  }

  bool appendSucceeds = true;
  int startAttempts = 0;
  int appendAttempts = 0;
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

void startTracking(tracking::TrackingWorkflow& workflow,
                   ScenarioStorage& storage) {
  workflow.processFix(movingFix(), storage);
  workflow.processFix(movingFix(), storage);
  workflow.processFix(movingFix(), storage);
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

void every_failed_candidate_condition_resets_the_start_count() {
  tracking::GpsFix failures[6];
  for (int i = 0; i < 6; ++i) failures[i] = movingFix();
  failures[0].locationValid = false;
  failures[1].locationFresh = false;
  failures[2].utcValid = false;
  failures[3].speedKmh.value = 2.0;
  failures[4].speedKmh.valid = false;
  failures[5].hdop.value = 5.01;

  for (int i = 0; i < 6; ++i) {
    tracking::TrackingWorkflow workflow("tracker-01");
    ScenarioStorage storage;
    workflow.processFix(movingFix(), storage);
    workflow.processFix(movingFix(), storage);
    workflow.processFix(failures[i], storage);
    workflow.processFix(movingFix(), storage);
    const tracking::TrackingDecision afterOneNewCandidate =
        workflow.processFix(movingFix(), storage);

    TEST_ASSERT_FALSE(afterOneNewCandidate.rawPointRecorded);
    TEST_ASSERT_EQUAL(0, storage.startAttempts);
  }
}

void active_session_records_stopped_and_weak_fixes_but_filters_exports() {
  tracking::TrackingWorkflow workflow("tracker-01");
  ScenarioStorage storage;
  startTracking(workflow, storage);
  tracking::GpsFix stoppedWeakFix = stationaryFix();
  stoppedWeakFix.utcValid = false;
  stoppedWeakFix.utc.clear();
  stoppedWeakFix.hdop.value = 8.2;

  const tracking::TrackingDecision decision =
      workflow.processFix(stoppedWeakFix, storage);

  TEST_ASSERT_TRUE(decision.rawPointRecorded);
  TEST_ASSERT_FALSE(decision.writeFilteredCsv);
  TEST_ASSERT_FALSE(decision.writeFilteredGpx);
  TEST_ASSERT_EQUAL_UINT32(2, decision.point.pointNumber);
}

void active_session_ignores_stale_and_invalid_locations() {
  tracking::TrackingWorkflow workflow("tracker-01");
  ScenarioStorage storage;
  startTracking(workflow, storage);
  tracking::GpsFix stale = movingFix();
  stale.locationFresh = false;
  tracking::GpsFix invalid = movingFix();
  invalid.locationValid = false;

  const tracking::TrackingDecision staleDecision = workflow.processFix(stale, storage);
  const tracking::TrackingDecision invalidDecision = workflow.processFix(invalid, storage);

  TEST_ASSERT_FALSE(staleDecision.rawPointRecorded);
  TEST_ASSERT_FALSE(invalidDecision.rawPointRecorded);
  TEST_ASSERT_EQUAL(1, storage.appendAttempts);
}

void point_contract_preserves_values_and_uses_null_for_missing_or_nonfinite() {
  tracking::TrackingWorkflow workflow("tracker-01");
  ScenarioStorage storage;
  tracking::GpsFix fix = movingFix();
  fix.altitudeMeters.valid = true;
  fix.altitudeMeters.value = NAN;
  fix.courseDegrees.valid = false;
  fix.satellites.valid = true;
  fix.satellites.value = 9;
  fix.uptimeMilliseconds = 123456;
  workflow.processFix(fix, storage);
  workflow.processFix(fix, storage);

  const tracking::TrackingDecision decision = workflow.processFix(fix, storage);
  const std::string& json = storage.lines[0];

  TEST_ASSERT_EQUAL_STRING("tracker-01", decision.point.trackerId.c_str());
  TEST_ASSERT_EQUAL_STRING("2026-08-30T12:00:00Z", decision.point.utc.c_str());
  TEST_ASSERT_EQUAL_DOUBLE(48.856613, decision.point.latitude);
  TEST_ASSERT_EQUAL_DOUBLE(2.352222, decision.point.longitude);
  TEST_ASSERT_NOT_EQUAL(std::string::npos, json.find("\"schema_version\":1"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, json.find("\"altitude_m\":null"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, json.find("\"course_deg\":null"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, json.find("\"satellites\":9"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, json.find("\"uptime_ms\":123456"));
  TEST_ASSERT_EQUAL(std::string::npos, json.find("nan"));
  TEST_ASSERT_EQUAL(std::string::npos, json.find("inf"));
}

void failed_raw_append_is_not_treated_as_recorded() {
  tracking::TrackingWorkflow workflow("tracker-01");
  ScenarioStorage storage;
  workflow.processFix(movingFix(), storage);
  workflow.processFix(movingFix(), storage);
  storage.appendSucceeds = false;

  const tracking::TrackingDecision failed = workflow.processFix(movingFix(), storage);
  storage.appendSucceeds = true;
  const tracking::TrackingDecision retried = workflow.processFix(movingFix(), storage);

  TEST_ASSERT_TRUE(failed.trackingSessionStarted);
  TEST_ASSERT_FALSE(failed.rawPointRecorded);
  TEST_ASSERT_FALSE(failed.writeFilteredCsv);
  TEST_ASSERT_TRUE(retried.rawPointRecorded);
  TEST_ASSERT_EQUAL_UINT32(1, retried.point.pointNumber);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(boot_and_stationary_fixes_create_no_tracking_session);
  RUN_TEST(third_qualifying_fix_starts_with_point_one);
  RUN_TEST(every_failed_candidate_condition_resets_the_start_count);
  RUN_TEST(active_session_records_stopped_and_weak_fixes_but_filters_exports);
  RUN_TEST(active_session_ignores_stale_and_invalid_locations);
  RUN_TEST(point_contract_preserves_values_and_uses_null_for_missing_or_nonfinite);
  RUN_TEST(failed_raw_append_is_not_treated_as_recorded);
  return UNITY_END();
}
