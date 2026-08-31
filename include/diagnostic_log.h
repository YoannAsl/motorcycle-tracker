#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace tracking {

class DiagnosticLogStorage {
 public:
  virtual ~DiagnosticLogStorage() = default;
  virtual size_t appendDiagnosticText(const std::string& text) = 0;
};

enum class DiagnosticWriteStatus { persisted, retained, full };

class DiagnosticLog {
 public:
  explicit DiagnosticLog(size_t pendingCapacity = 2048);
  DiagnosticWriteStatus append(DiagnosticLogStorage* storage,
                               const std::string& text);
  DiagnosticWriteStatus flush(DiagnosticLogStorage& storage);
  const std::string& pending() const;

 private:
  DiagnosticWriteStatus retain(const std::string& text);
  size_t pendingCapacity_;
  std::string pending_;
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
  Connection,
  Authentication,
  Http,
  Tls,
  Transport,
  MalformedResponse,
  Timeout,
};

class DiagnosticConfirmationStorage {
 public:
  virtual ~DiagnosticConfirmationStorage() = default;
  virtual bool appendText(const std::string& path,
                          const std::string& contents) = 0;
};

bool buildDiagnosticUploadRequest(const std::string& uploadUrl,
                                  const std::string& bearerToken,
                                  const DiagnosticLogUpload& upload,
                                  DiagnosticUploadRequest& request);
bool validateDiagnosticUploadResponse(int statusCode,
                                      const std::string& responseBody,
                                      const DiagnosticLogUpload& upload);
std::string diagnosticUploadFailureMessage(DiagnosticUploadFailure failure);
DiagnosticUploadFailure classifyHttpFailure(int statusCode,
                                            bool tlsFailure = false);
std::string serializeDiagnosticDelivery(const std::string& trackerId,
                                        uint32_t trackingSessionNumber);
bool selectOldestPendingDiagnosticLog(
    const std::string& trackerId,
    const std::vector<StoredDiagnosticLog>& storedLogs,
    DiagnosticLogUpload& pending);
bool confirmDiagnosticDelivery(DiagnosticConfirmationStorage& storage,
                               const std::string& trackerId,
                               uint32_t trackingSessionNumber,
                               std::vector<StoredDiagnosticLog>& storedLogs);

}  // namespace tracking
