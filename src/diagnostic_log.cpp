#include "diagnostic_log.h"

#include <cstdio>

namespace tracking {
namespace {

std::string number(uint32_t value) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%lu",
                static_cast<unsigned long>(value));
  return buffer;
}

bool hasDiagnosticUrl(const std::string& url) {
  if (url.compare(0, 8, "https://") != 0) return false;
  const size_t path = url.find('/', 8);
  return path != std::string::npos &&
         url.substr(path) == "/v1/diagnostic-logs";
}

bool hasStringField(const std::string& body, const char* field,
                    const std::string& expected) {
  return body.find(std::string("\"") + field + "\":\"" + expected + "\"") !=
         std::string::npos;
}

bool hasNumberField(const std::string& body, const char* field,
                    uint32_t expected) {
  return body.find(std::string("\"") + field + "\":" + number(expected)) !=
         std::string::npos;
}

}  // namespace

void DiagnosticLog::append(const char* category, const std::string& detail) {
  contents_ += '[';
  contents_ += category;
  contents_ += "] ";
  contents_ += detail;
  contents_ += '\n';
}

void DiagnosticLog::recordBoot(const std::string& resetReason) {
  append("BOOT", "reset=" + resetReason);
}

void DiagnosticLog::recordHealth(uint32_t uptimeMilliseconds,
                                 uint32_t rawPoints,
                                 uint32_t noFreshLocation) {
  append("HEALTH", "uptime_ms=" + number(uptimeMilliseconds) +
                       " raw_points=" + number(rawPoints) +
                       " no_fresh_location=" + number(noFreshLocation));
}

void DiagnosticLog::recordWifiState(const std::string& state) {
  append("WIFI", state);
}

void DiagnosticLog::recordUploadAttempt(const std::string& kind,
                                        uint32_t trackingSessionNumber) {
  append("UPLOAD", kind + " attempt session=" + number(trackingSessionNumber));
}

void DiagnosticLog::recordUploadResult(const std::string& kind,
                                       const std::string& result) {
  append("UPLOAD", kind + " result=" + result);
}

void DiagnosticLog::recordCleanup(const std::string& detail) {
  append("CLEANUP", detail);
}

void DiagnosticLog::recordError(const std::string& component,
                                const std::string& detail) {
  append("ERROR", component + " " + detail);
}

const std::string& DiagnosticLog::contents() const { return contents_; }

bool buildDiagnosticUploadRequest(const std::string& uploadUrl,
                                  const std::string& bearerToken,
                                  const DiagnosticLogUpload& upload,
                                  DiagnosticUploadRequest& request) {
  if (!hasDiagnosticUrl(uploadUrl) || bearerToken.empty() ||
      upload.trackerId.empty() || upload.trackingSessionNumber == 0 ||
      upload.contents.empty()) {
    return false;
  }
  const std::string session = number(upload.trackingSessionNumber);
  request.url = uploadUrl;
  request.headers.clear();
  request.headers.push_back({"Authorization", "Bearer " + bearerToken});
  request.headers.push_back({"Content-Type", "text/plain; charset=utf-8"});
  request.headers.push_back({"X-Tracker-ID", upload.trackerId});
  request.headers.push_back({"X-Tracking-Session-Number", session});
  request.headers.push_back(
      {"Idempotency-Key", upload.trackerId + ":" + session});
  request.body = upload.contents;
  return true;
}

bool validateDiagnosticUploadResponse(int statusCode,
                                      const std::string& responseBody,
                                      const DiagnosticLogUpload& upload) {
  if (statusCode < 200 || statusCode >= 300) return false;
  if (responseBody.empty() || responseBody.front() != '{' ||
      responseBody.back() != '}') {
    return false;
  }
  return hasStringField(responseBody, "tracker_id", upload.trackerId) &&
         hasNumberField(responseBody, "tracking_session_number",
                        upload.trackingSessionNumber) &&
         responseBody.find("\"diagnostic_log_stored\":true") !=
             std::string::npos;
}

std::string diagnosticUploadFailureMessage(DiagnosticUploadFailure failure) {
  switch (failure) {
    case DiagnosticUploadFailure::Wifi:
      return "Wi-Fi connection failed";
    case DiagnosticUploadFailure::Authentication:
      return "authentication rejected";
    case DiagnosticUploadFailure::Server:
      return "server rejected the request";
    case DiagnosticUploadFailure::Tls:
      return "TLS setup or certificate validation failed";
    case DiagnosticUploadFailure::MalformedResponse:
      return "malformed or mismatched confirmation";
    case DiagnosticUploadFailure::Timeout:
      return "network timeout";
  }
  return "unknown failure";
}

}  // namespace tracking
