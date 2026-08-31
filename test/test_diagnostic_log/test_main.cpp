#include <unity.h>

#include <algorithm>
#include <string>
#include <vector>

#include "diagnostic_log.h"

void setUp() {}
void tearDown() {}

namespace {

class FakeDiagnosticStorage : public tracking::DiagnosticLogStorage,
                              public tracking::DiagnosticConfirmationStorage {
 public:
  size_t appendDiagnosticText(const std::string& text) override {
    const size_t written = std::min(writeLimit, text.size());
    persisted.append(text, 0, written);
    return written;
  }

  bool appendText(const std::string& path,
                  const std::string& contents) override {
    confirmationPath = path;
    if (!confirmationSucceeds) return false;
    confirmationContents += contents;
    return true;
  }

  size_t writeLimit = static_cast<size_t>(-1);
  bool confirmationSucceeds = true;
  std::string persisted;
  std::string confirmationPath;
  std::string confirmationContents;
};

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
  FakeDiagnosticStorage storage;
  tracking::DiagnosticLog log;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(tracking::DiagnosticWriteStatus::persisted),
      static_cast<int>(log.append(&storage, "[BOOT] reset=power-on\n")));
  log.append(&storage,
             "[HEALTH] uptime_ms=60000 raw_points=12 no_fresh_location=3\n");
  log.append(&storage, "[WIFI] connected\n");
  log.append(&storage, "[UPLOAD] diagnostic attempt session=41\n");
  log.append(&storage, "[UPLOAD] diagnostic result=timeout\n");
  log.append(&storage, "[CLEANUP] closed files\n");
  log.append(&storage, "[ERROR] SD append failed\n");

  const std::string& text = storage.persisted;
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

void failed_and_short_writes_keep_unwritten_diagnostics_for_retry() {
  tracking::DiagnosticLog log(64);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(tracking::DiagnosticWriteStatus::retained),
      static_cast<int>(log.append(nullptr, "[BOOT] reset=watchdog\n")));

  FakeDiagnosticStorage storage;
  storage.writeLimit = 7;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(tracking::DiagnosticWriteStatus::retained),
      static_cast<int>(log.flush(storage)));
  TEST_ASSERT_FALSE(log.pending().empty());

  storage.writeLimit = static_cast<size_t>(-1);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(tracking::DiagnosticWriteStatus::persisted),
      static_cast<int>(log.flush(storage)));
  TEST_ASSERT_TRUE(log.pending().empty());
  TEST_ASSERT_EQUAL_STRING("[BOOT] reset=watchdog\n",
                           storage.persisted.c_str());
}

void bounded_pending_diagnostics_report_when_capacity_is_exhausted() {
  tracking::DiagnosticLog log(8);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(tracking::DiagnosticWriteStatus::full),
      static_cast<int>(log.append(nullptr, "123456789")));
  TEST_ASSERT_EQUAL_STRING("12345678", log.pending().c_str());
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
      200,
      "{\"tracker_id\":\"tracker-01\",\"tracking_session_number\":041,"
      "\"diagnostic_log_stored\":true}", upload));
  TEST_ASSERT_FALSE(tracking::validateDiagnosticUploadResponse(
      200,
      "{\"tracker_id\":\"tracker-01\","
      "\"tracking_session_number\":42949672960,"
      "\"diagnostic_log_stored\":true}", upload));
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
                            tracking::DiagnosticUploadFailure::Http)
                            .find("HTTP"));
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
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(tracking::DiagnosticUploadFailure::Connection),
      static_cast<int>(tracking::classifyHttpFailure(-1)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(tracking::DiagnosticUploadFailure::Timeout),
      static_cast<int>(tracking::classifyHttpFailure(-11)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(tracking::DiagnosticUploadFailure::Tls),
      static_cast<int>(tracking::classifyHttpFailure(-1, true)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(tracking::DiagnosticUploadFailure::Authentication),
      static_cast<int>(tracking::classifyHttpFailure(401)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(tracking::DiagnosticUploadFailure::Http),
      static_cast<int>(tracking::classifyHttpFailure(503)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(tracking::DiagnosticUploadFailure::MalformedResponse),
      static_cast<int>(tracking::classifyHttpFailure(200)));
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

  FakeDiagnosticStorage storage;
  storage.confirmationSucceeds = false;
  TEST_ASSERT_FALSE(tracking::confirmDiagnosticDelivery(
      storage, "tracker-01", 41, stored));
  TEST_ASSERT_TRUE(stored[1].deliveryState.empty());
  TEST_ASSERT_TRUE(tracking::selectOldestPendingDiagnosticLog(
      "tracker-01", stored, pending));
  TEST_ASSERT_EQUAL_UINT32(41, pending.trackingSessionNumber);

  storage.confirmationSucceeds = true;
  TEST_ASSERT_TRUE(tracking::confirmDiagnosticDelivery(
      storage, "tracker-01", 41, stored));
  TEST_ASSERT_EQUAL_STRING(
      "/session-0000000041/diagnostic-delivery-state.log",
      storage.confirmationPath.c_str());
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
  RUN_TEST(failed_and_short_writes_keep_unwritten_diagnostics_for_retry);
  RUN_TEST(bounded_pending_diagnostics_report_when_capacity_is_exhausted);
  RUN_TEST(completed_log_request_has_stable_identity_and_is_retry_safe);
  RUN_TEST(only_a_valid_matching_confirmation_marks_the_log_delivered);
  RUN_TEST(failure_messages_distinguish_actionable_causes);
  RUN_TEST(completed_logs_stay_pending_until_matching_confirmation_is_persisted);
  return UNITY_END();
}
