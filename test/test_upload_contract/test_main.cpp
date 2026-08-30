#include <unity.h>

#include <string>
#include <vector>

#include "upload_contract.h"

void setUp() {}
void tearDown() {}

namespace {

std::string point(uint32_t number, const char* trackerId = "tracker-01",
                  uint32_t session = 41) {
  char body[192];
  snprintf(body, sizeof(body),
           "{\"schema_version\":1,\"tracker_id\":\"%s\"," 
           "\"tracking_session_number\":%lu,\"point_number\":%lu}",
           trackerId, static_cast<unsigned long>(session),
           static_cast<unsigned long>(number));
  return body;
}

tracking::UploadBatch thirtyPoints() {
  tracking::UploadBatch batch;
  batch.schemaVersion = 1;
  batch.trackerId = "tracker-01";
  batch.trackingSessionNumber = 41;
  batch.firstPointNumber = 1;
  for (uint32_t number = 1; number <= 30; ++number) {
    batch.ndjsonPoints.push_back(point(number));
  }
  return batch;
}

void request_carries_authentication_content_type_identity_and_range() {
  tracking::UploadRequest request;
  const tracking::UploadBatch batch = thirtyPoints();

  const bool built = tracking::buildUploadRequest(
      "https://uploads.example/v1/track-point-batches", "secret-token", batch,
      request);

  TEST_ASSERT_TRUE(built);
  TEST_ASSERT_EQUAL_STRING(
      "https://uploads.example/v1/track-point-batches", request.url.c_str());
  TEST_ASSERT_EQUAL_STRING("Bearer secret-token", request.authorization.c_str());
  TEST_ASSERT_EQUAL_STRING("application/x-ndjson", request.contentType.c_str());
  TEST_ASSERT_EQUAL_STRING("1", request.schemaVersion.c_str());
  TEST_ASSERT_EQUAL_STRING("tracker-01", request.trackerId.c_str());
  TEST_ASSERT_EQUAL_STRING("41", request.trackingSessionNumber.c_str());
  TEST_ASSERT_EQUAL_STRING("1", request.firstPointNumber.c_str());
  TEST_ASSERT_EQUAL_STRING("30", request.lastPointNumber.c_str());
  TEST_ASSERT_EQUAL_STRING((point(1) + "\n").c_str(),
                           request.body.substr(0, point(1).size() + 1).c_str());
  TEST_ASSERT_EQUAL_CHAR('\n', request.body[request.body.size() - 1]);
}

void request_rejects_insecure_wrong_path_oversized_and_unordered_batches() {
  tracking::UploadRequest request;
  tracking::UploadBatch batch = thirtyPoints();
  TEST_ASSERT_FALSE(tracking::buildUploadRequest(
      "http://uploads.example/v1/track-point-batches", "token", batch,
      request));
  TEST_ASSERT_FALSE(tracking::buildUploadRequest(
      "https://uploads.example/other", "token", batch, request));

  batch.ndjsonPoints.push_back(point(31));
  TEST_ASSERT_FALSE(tracking::buildUploadRequest(
      "https://uploads.example/v1/track-point-batches", "token", batch,
      request));

  batch = thirtyPoints();
  batch.ndjsonPoints[12] = point(14);
  TEST_ASSERT_FALSE(tracking::buildUploadRequest(
      "https://uploads.example/v1/track-point-batches", "token", batch,
      request));
}

void only_matching_complete_success_confirms_delivery() {
  const tracking::UploadBatch batch = thirtyPoints();
  tracking::UploadConfirmation confirmation;

  TEST_ASSERT_TRUE(tracking::validateUploadResponse(
      200,
      "{\"tracker_id\":\"tracker-01\",\"tracking_session_number\":41,"
      "\"highest_stored_point_number\":30}",
      batch, confirmation));
  TEST_ASSERT_EQUAL_UINT32(30, confirmation.highestStoredPointNumber);

  TEST_ASSERT_FALSE(tracking::validateUploadResponse(
      401,
      "{\"tracker_id\":\"tracker-01\",\"tracking_session_number\":41,"
      "\"highest_stored_point_number\":30}",
      batch, confirmation));
  TEST_ASSERT_FALSE(tracking::validateUploadResponse(200, "not-json", batch,
                                                     confirmation));
  TEST_ASSERT_FALSE(tracking::validateUploadResponse(
      200,
      "{\"tracker_id\":\"other\",\"tracking_session_number\":41,"
      "\"highest_stored_point_number\":30}",
      batch, confirmation));
  TEST_ASSERT_FALSE(tracking::validateUploadResponse(
      200,
      "{\"tracker_id\":\"tracker-01\",\"tracking_session_number\":42,"
      "\"highest_stored_point_number\":30}",
      batch, confirmation));
  TEST_ASSERT_FALSE(tracking::validateUploadResponse(
      200,
      "{\"tracker_id\":\"tracker-01\",\"tracking_session_number\":41,"
      "\"highest_stored_point_number\":29}",
      batch, confirmation));
}

void repeated_confirmation_is_still_a_valid_success() {
  tracking::UploadBatch batch = thirtyPoints();
  tracking::UploadConfirmation confirmation;
  const char* response =
      "{\"highest_stored_point_number\":60,\"tracking_session_number\":41,"
      "\"tracker_id\":\"tracker-01\"}";

  TEST_ASSERT_TRUE(
      tracking::validateUploadResponse(201, response, batch, confirmation));
  TEST_ASSERT_TRUE(
      tracking::validateUploadResponse(200, response, batch, confirmation));
  TEST_ASSERT_EQUAL_UINT32(60, confirmation.highestStoredPointNumber);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(request_carries_authentication_content_type_identity_and_range);
  RUN_TEST(request_rejects_insecure_wrong_path_oversized_and_unordered_batches);
  RUN_TEST(only_matching_complete_success_confirms_delivery);
  RUN_TEST(repeated_confirmation_is_still_a_valid_success);
  return UNITY_END();
}
