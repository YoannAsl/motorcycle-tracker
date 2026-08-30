#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace tracking {

struct UploadBatch {
  uint32_t schemaVersion = 1;
  std::string trackerId;
  uint32_t trackingSessionNumber = 0;
  uint32_t firstPointNumber = 0;
  std::vector<std::string> ndjsonPoints;
};

struct UploadRequest {
  std::string url;
  struct Header {
    std::string name;
    std::string value;
  };
  std::vector<Header> headers;
  std::string body;
};

struct UploadConfirmation {
  uint32_t highestStoredPointNumber = 0;
};

bool buildUploadRequest(const std::string& uploadUrl,
                        const std::string& bearerToken,
                        const UploadBatch& batch, UploadRequest& request);

bool validateUploadResponse(int statusCode, const std::string& responseBody,
                            const UploadBatch& batch,
                            UploadConfirmation& confirmation);

}  // namespace tracking
