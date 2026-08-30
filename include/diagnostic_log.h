#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace tracking {

class DiagnosticLog {
 public:
  void recordBoot(const std::string& resetReason);
  void recordHealth(uint32_t uptimeMilliseconds, uint32_t rawPoints,
                    uint32_t noFreshLocation);
  void recordWifiState(const std::string& state);
  void recordUploadAttempt(const std::string& kind,
                           uint32_t trackingSessionNumber);
  void recordUploadResult(const std::string& kind, const std::string& result);
  void recordCleanup(const std::string& detail);
  void recordError(const std::string& component, const std::string& detail);
  const std::string& contents() const;

 private:
  void append(const char* category, const std::string& detail);
  std::string contents_;
};

struct DiagnosticLogUpload {
  std::string trackerId;
  uint32_t trackingSessionNumber = 0;
  std::string contents;
};

struct StoredDiagnosticLog {
  uint32_t trackingSessionNumber = 0;
  std::string contents;
  std::string deliveryState;
};

struct DiagnosticUploadRequest {
  std::string url;
  struct Header {
    std::string name;
    std::string value;
  };
  std::vector<Header> headers;
  std::string body;
};

enum class DiagnosticUploadFailure {
  Wifi,
  Authentication,
  Server,
  Tls,
  MalformedResponse,
  Timeout,
};

bool buildDiagnosticUploadRequest(const std::string& uploadUrl,
                                  const std::string& bearerToken,
                                  const DiagnosticLogUpload& upload,
                                  DiagnosticUploadRequest& request);
bool validateDiagnosticUploadResponse(int statusCode,
                                      const std::string& responseBody,
                                      const DiagnosticLogUpload& upload);
std::string diagnosticUploadFailureMessage(DiagnosticUploadFailure failure);
std::string serializeDiagnosticDelivery(const std::string& trackerId,
                                        uint32_t trackingSessionNumber);
bool selectOldestPendingDiagnosticLog(
    const std::string& trackerId,
    const std::vector<StoredDiagnosticLog>& storedLogs,
    DiagnosticLogUpload& pending);

}  // namespace tracking
