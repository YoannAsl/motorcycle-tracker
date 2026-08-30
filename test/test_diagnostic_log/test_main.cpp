#include <unity.h>

#include <string>
#include <vector>

#include "diagnostic_log.h"

void setUp() {}
void tearDown() {}

namespace {

const char* header(const tracking::DiagnosticUploadRequest& request,
                   const char* name) {
  for (std::vector<tracking::DiagnosticUploadRequest::Header>::const_iterator it =
           request.headers.begin();
       it != request.headers.end(); ++it) {
    if (it->name == name) return it->value.c_str();
  }
  return nullptr;
}

void useful_events_form_a_concise_log_without_track_point_copies() {
  tracking::DiagnosticLog log;
  log.recordBoot("power-on");
  log.recordHealth(60000, 12, 3);
  log.recordWifiState("connected");
  log.recordUploadAttempt("diagnostic", 41);
  log.recordUploadResult("diagnostic", "timeout");
  log.recordCleanup("closed files");
  log.recordError("SD", "append failed");

  const std::string text = log.contents();
  TEST_ASSERT_NOT_EQUAL(std::string::npos, text.find("[BOOT] reset=power-on"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, text.find("[HEALTH] uptime_ms=60000 raw_points=12 no_fresh_location=3"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, text.find("[WIFI] connected"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, text.find("[UPLOAD] diagnostic attempt session=41"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, text.find("[UPLOAD] diagnostic result=timeout"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, text.find("[CLEANUP] closed files"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, text.find("[ERROR] SD append failed"));
  TEST_ASSERT_EQUAL(std::string::npos, text.find("latitude"));
  TEST_ASSERT_EQUAL(std::string::npos, text.find("longitude"));
}

void completed_log_request_has_stable_identity_and_is_retry_safe() {
  tracking::DiagnosticLogUpload upload;
  upload.trackerId = "tracker-01";
  upload.trackingSessionNumber = 41;
  upload.contents = "[BOOT] reset=power-on\n[HEALTH] uptime_ms=60000\n";
  tracking::DiagnosticUploadRequest first;
  tracking::DiagnosticUploadRequest retry;

  TEST_ASSERT_TRUE(tracking::buildDiagnosticUploadRequest(
      "https://uploads.example/v1/diagnostic-logs", "secret", upload, first));
  TEST_ASSERT_TRUE(tracking::buildDiagnosticUploadRequest(
      "https://uploads.example/v1/diagnostic-logs", "secret", upload, retry));
  TEST_ASSERT_EQUAL_STRING("tracker-01", header(first, "X-Tracker-ID"));
  TEST_ASSERT_EQUAL_STRING("41", header(first, "X-Tracking-Session-Number"));
  TEST_ASSERT_EQUAL_STRING("tracker-01:41", header(first, "Idempotency-Key"));
  TEST_ASSERT_EQUAL_STRING(first.body.c_str(), retry.body.c_str());
  TEST_ASSERT_EQUAL_STRING(header(first, "Idempotency-Key"),
                           header(retry, "Idempotency-Key"));
}

void only_a_valid_matching_confirmation_marks_the_log_delivered() {
  tracking::DiagnosticLogUpload upload;
  upload.trackerId = "tracker-01";
  upload.trackingSessionNumber = 41;
  upload.contents = "[BOOT] reset=power-on\n";

  TEST_ASSERT_TRUE(tracking::validateDiagnosticUploadResponse(
      200,
      "{\"tracker_id\":\"tracker-01\",\"tracking_session_number\":41,"
      "\"diagnostic_log_stored\":true}", upload));
  TEST_ASSERT_FALSE(tracking::validateDiagnosticUploadResponse(
      200,
      "{\"tracker_id\":\"other\",\"tracking_session_number\":41,"
      "\"diagnostic_log_stored\":true}", upload));
  TEST_ASSERT_FALSE(tracking::validateDiagnosticUploadResponse(
      200, "malformed", upload));
  TEST_ASSERT_FALSE(tracking::validateDiagnosticUploadResponse(
      200,
      "{\"tracker_id\":\"tracker-01\",\"tracking_session_number\":41,"
      "\"diagnostic_log_stored\":true,}", upload));
  TEST_ASSERT_FALSE(tracking::validateDiagnosticUploadResponse(
      401,
      "{\"tracker_id\":\"tracker-01\",\"tracking_session_number\":41,"
      "\"diagnostic_log_stored\":true}", upload));
}

void failure_messages_distinguish_actionable_causes() {
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        tracking::diagnosticUploadFailureMessage(
                            tracking::DiagnosticUploadFailure::Wifi)
                            .find("Wi-Fi"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        tracking::diagnosticUploadFailureMessage(
                            tracking::DiagnosticUploadFailure::Authentication)
                            .find("authentication"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        tracking::diagnosticUploadFailureMessage(
                            tracking::DiagnosticUploadFailure::Server)
                            .find("server"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        tracking::diagnosticUploadFailureMessage(
                            tracking::DiagnosticUploadFailure::Tls)
                            .find("TLS"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        tracking::diagnosticUploadFailureMessage(
                            tracking::DiagnosticUploadFailure::MalformedResponse)
                            .find("malformed"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        tracking::diagnosticUploadFailureMessage(
                            tracking::DiagnosticUploadFailure::Timeout)
                            .find("timeout"));
}

void completed_logs_stay_pending_until_matching_confirmation_is_persisted() {
  tracking::StoredDiagnosticLog older;
  older.trackingSessionNumber = 41;
  older.contents = "[BOOT] reset=power-on\n";
  tracking::StoredDiagnosticLog newer;
  newer.trackingSessionNumber = 42;
  newer.contents = "[BOOT] reset=watchdog\n";
  std::vector<tracking::StoredDiagnosticLog> stored = {newer, older};
  tracking::DiagnosticLogUpload pending;

  TEST_ASSERT_TRUE(tracking::selectOldestPendingDiagnosticLog(
      "tracker-01", stored, pending));
  TEST_ASSERT_EQUAL_UINT32(41, pending.trackingSessionNumber);

  TEST_ASSERT_FALSE(tracking::validateDiagnosticUploadResponse(
      200,
      "{\"tracker_id\":\"other\",\"tracking_session_number\":41,"
      "\"diagnostic_log_stored\":true}", pending));
  TEST_ASSERT_TRUE(stored[1].deliveryState.empty());
  TEST_ASSERT_TRUE(tracking::selectOldestPendingDiagnosticLog(
      "tracker-01", stored, pending));

  stored[1].deliveryState =
      tracking::serializeDiagnosticDelivery("tracker-01", 41);
  TEST_ASSERT_TRUE(tracking::selectOldestPendingDiagnosticLog(
      "tracker-01", stored, pending));
  TEST_ASSERT_EQUAL_UINT32(42, pending.trackingSessionNumber);

  stored[0].deliveryState =
      tracking::serializeDiagnosticDelivery("tracker-01", 42);
  TEST_ASSERT_FALSE(tracking::selectOldestPendingDiagnosticLog(
      "tracker-01", stored, pending));
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(useful_events_form_a_concise_log_without_track_point_copies);
  RUN_TEST(completed_log_request_has_stable_identity_and_is_retry_safe);
  RUN_TEST(only_a_valid_matching_confirmation_marks_the_log_delivered);
  RUN_TEST(failure_messages_distinguish_actionable_causes);
  RUN_TEST(completed_logs_stay_pending_until_matching_confirmation_is_persisted);
  return UNITY_END();
}
