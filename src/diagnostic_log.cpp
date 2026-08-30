#include "diagnostic_log.h"

#include <cctype>
#include <cstdio>
#include <limits>

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

void skipWhitespace(const std::string& input, size_t& position) {
  while (position < input.size() &&
         std::isspace(static_cast<unsigned char>(input[position]))) {
    ++position;
  }
}

bool parseString(const std::string& input, size_t& position,
                 std::string& value) {
  skipWhitespace(input, position);
  if (position >= input.size() || input[position++] != '"') return false;
  value.clear();
  while (position < input.size()) {
    const char character = input[position++];
    if (character == '"') return true;
    if (character == '\\' || static_cast<unsigned char>(character) < 0x20) {
      return false;
    }
    value += character;
  }
  return false;
}

bool parseUInt(const std::string& input, size_t& position, uint32_t& value) {
  skipWhitespace(input, position);
  if (position >= input.size() ||
      !std::isdigit(static_cast<unsigned char>(input[position]))) {
    return false;
  }
  uint64_t parsed = 0;
  do {
    parsed = parsed * 10 + static_cast<unsigned>(input[position++] - '0');
    if (parsed > std::numeric_limits<uint32_t>::max()) return false;
  } while (position < input.size() &&
           std::isdigit(static_cast<unsigned char>(input[position])));
  value = static_cast<uint32_t>(parsed);
  return true;
}

bool parseDiagnosticConfirmation(const std::string& input,
                                 std::string& trackerId,
                                 uint32_t& trackingSessionNumber) {
  size_t position = 0;
  skipWhitespace(input, position);
  if (position >= input.size() || input[position++] != '{') return false;
  bool hasTracker = false;
  bool hasSession = false;
  bool hasStored = false;
  for (;;) {
    skipWhitespace(input, position);
    if (position < input.size() && input[position] == '}') {
      ++position;
      break;
    }
    std::string key;
    if (!parseString(input, position, key)) return false;
    skipWhitespace(input, position);
    if (position >= input.size() || input[position++] != ':') return false;
    if (key == "tracker_id") {
      if (hasTracker || !parseString(input, position, trackerId)) return false;
      hasTracker = true;
    } else if (key == "tracking_session_number") {
      if (hasSession || !parseUInt(input, position, trackingSessionNumber)) {
        return false;
      }
      hasSession = true;
    } else if (key == "diagnostic_log_stored") {
      skipWhitespace(input, position);
      if (hasStored || input.compare(position, 4, "true") != 0) return false;
      position += 4;
      hasStored = true;
    } else {
      return false;
    }
    skipWhitespace(input, position);
    if (position < input.size() && input[position] == ',') {
      ++position;
      size_t next = position;
      skipWhitespace(input, next);
      if (next >= input.size() || input[next] == '}') return false;
      continue;
    }
    if (position < input.size() && input[position] == '}') {
      ++position;
      break;
    }
    return false;
  }
  skipWhitespace(input, position);
  return position == input.size() && hasTracker && hasSession && hasStored;
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
  std::string trackerId;
  uint32_t trackingSessionNumber = 0;
  return parseDiagnosticConfirmation(responseBody, trackerId,
                                     trackingSessionNumber) &&
         trackerId == upload.trackerId &&
         trackingSessionNumber == upload.trackingSessionNumber;
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

std::string serializeDiagnosticDelivery(const std::string& trackerId,
                                        uint32_t trackingSessionNumber) {
  return "v1 tracker=" + trackerId + " session=" +
         number(trackingSessionNumber) + " delivered\n";
}

bool selectOldestPendingDiagnosticLog(
    const std::string& trackerId,
    const std::vector<StoredDiagnosticLog>& storedLogs,
    DiagnosticLogUpload& pending) {
  const StoredDiagnosticLog* oldest = nullptr;
  for (std::vector<StoredDiagnosticLog>::const_iterator it = storedLogs.begin();
       it != storedLogs.end(); ++it) {
    if (it->trackingSessionNumber == 0 || it->contents.empty() ||
        it->deliveryState.find(serializeDiagnosticDelivery(
            trackerId, it->trackingSessionNumber)) != std::string::npos) {
      continue;
    }
    if (oldest == nullptr ||
        it->trackingSessionNumber < oldest->trackingSessionNumber) {
      oldest = &*it;
    }
  }
  if (oldest == nullptr) return false;
  pending.trackerId = trackerId;
  pending.trackingSessionNumber = oldest->trackingSessionNumber;
  pending.contents = oldest->contents;
  return true;
}

}  // namespace tracking
