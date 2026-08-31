#include "diagnostic_log.h"

#include "delivery_recovery.h"

#include <cctype>
#include <cstdio>
#include <limits>

#include "upload_contract.h"

namespace tracking {
namespace {

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
  if (input[position] == '0' && position + 1 < input.size() &&
      std::isdigit(static_cast<unsigned char>(input[position + 1]))) {
    return false;
  }
  uint32_t parsed = 0;
  do {
    const uint32_t digit = static_cast<uint32_t>(input[position] - '0');
    if (parsed > (std::numeric_limits<uint32_t>::max() - digit) / 10u) {
      return false;
    }
    parsed = parsed * 10 + digit;
    ++position;
  } while (position < input.size() &&
           std::isdigit(static_cast<unsigned char>(input[position])));
  value = parsed;
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

DiagnosticLog::DiagnosticLog(size_t pendingCapacity)
    : pendingCapacity_(pendingCapacity) {}

DiagnosticWriteStatus DiagnosticLog::retain(const std::string& text) {
  const size_t available = pendingCapacity_ - pending_.size();
  pending_.append(text, 0, available);
  return text.size() <= available ? DiagnosticWriteStatus::retained
                                  : DiagnosticWriteStatus::full;
}

DiagnosticWriteStatus DiagnosticLog::flush(DiagnosticLogStorage& storage) {
  if (pending_.empty()) return DiagnosticWriteStatus::persisted;
  const size_t written = storage.appendDiagnosticText(pending_);
  if (written >= pending_.size()) {
    pending_.clear();
    return DiagnosticWriteStatus::persisted;
  }
  pending_.erase(0, written);
  return DiagnosticWriteStatus::retained;
}

DiagnosticWriteStatus DiagnosticLog::append(DiagnosticLogStorage* storage,
                                            const std::string& text) {
  if (storage != nullptr && flush(*storage) == DiagnosticWriteStatus::persisted) {
    const size_t written = storage->appendDiagnosticText(text);
    if (written >= text.size()) return DiagnosticWriteStatus::persisted;
    return retain(text.substr(written));
  }
  return retain(text);
}

const std::string& DiagnosticLog::pending() const { return pending_; }

bool buildDiagnosticUploadRequest(const std::string& uploadUrl,
                                  const std::string& bearerToken,
                                  const DiagnosticLogUpload& upload,
                                  DiagnosticUploadRequest& request) {
  if (!hasDiagnosticUrl(uploadUrl) || bearerToken.empty() ||
      upload.trackerId.empty() || upload.trackingSessionNumber == 0 ||
      upload.contents.empty()) {
    return false;
  }
  const std::string session = formatUInt32(upload.trackingSessionNumber);
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
    case DiagnosticUploadFailure::Connection:
      return "DNS or connection failed";
    case DiagnosticUploadFailure::Authentication:
      return "authentication rejected";
    case DiagnosticUploadFailure::Http:
      return "HTTP request rejected";
    case DiagnosticUploadFailure::Tls:
      return "TLS setup or certificate validation failed";
    case DiagnosticUploadFailure::Transport:
      return "HTTP transport failed";
    case DiagnosticUploadFailure::MalformedResponse:
      return "malformed or mismatched confirmation";
    case DiagnosticUploadFailure::Timeout:
      return "network timeout";
  }
  return "unknown failure";
}

DiagnosticUploadFailure classifyHttpFailure(int statusCode, bool tlsFailure) {
  if (statusCode < 0 && tlsFailure) return DiagnosticUploadFailure::Tls;
  if (statusCode == 401 || statusCode == 403) {
    return DiagnosticUploadFailure::Authentication;
  }
  if (statusCode >= 400 && statusCode < 600) {
    return DiagnosticUploadFailure::Http;
  }
  if (statusCode == -11 || statusCode == -12) {
    return DiagnosticUploadFailure::Timeout;
  }
  if (statusCode == -1 || statusCode == -4 || statusCode == -5 ||
      statusCode == -7) {
    return DiagnosticUploadFailure::Connection;
  }
  if (statusCode < 0) return DiagnosticUploadFailure::Transport;
  return DiagnosticUploadFailure::MalformedResponse;
}

std::string serializeDiagnosticDelivery(const std::string& trackerId,
                                        uint32_t trackingSessionNumber) {
  return "v1 tracker=" + trackerId + " session=" +
         formatUInt32(trackingSessionNumber) + " delivered\n";
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

bool confirmDiagnosticDelivery(DiagnosticConfirmationStorage& storage,
                               const std::string& trackerId,
                               uint32_t trackingSessionNumber,
                               std::vector<StoredDiagnosticLog>& storedLogs) {
  for (std::vector<StoredDiagnosticLog>::iterator log = storedLogs.begin();
       log != storedLogs.end(); ++log) {
    if (log->trackingSessionNumber != trackingSessionNumber) continue;
    const std::string record =
        serializeDiagnosticDelivery(trackerId, trackingSessionNumber);
    if (!storage.appendText(
            sessionFilePath(trackingSessionNumber,
                            "diagnostic-delivery-state.log"),
            record)) {
      return false;
    }
    log->deliveryState += record;
    return true;
  }
  return false;
}

}  // namespace tracking
