#include "upload_contract.h"

#include <cctype>
#include <cstdio>
#include <limits>

namespace tracking {

namespace {

const char REQUIRED_PATH[] = "/v1/track-point-batches";

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
    if (static_cast<unsigned char>(character) < 0x20) return false;
    if (character != '\\') {
      value += character;
      continue;
    }
    if (position >= input.size()) return false;
    const char escaped = input[position++];
    switch (escaped) {
      case '"': value += '"'; break;
      case '\\': value += '\\'; break;
      case '/': value += '/'; break;
      case 'b': value += '\b'; break;
      case 'f': value += '\f'; break;
      case 'n': value += '\n'; break;
      case 'r': value += '\r'; break;
      case 't': value += '\t'; break;
      default: return false;
    }
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
  uint64_t parsed = 0;
  do {
    const unsigned digit = static_cast<unsigned>(input[position] - '0');
    if (parsed >
        (std::numeric_limits<uint32_t>::max() - digit) / 10u) {
      return false;
    }
    parsed = parsed * 10 + digit;
    ++position;
  } while (position < input.size() &&
           std::isdigit(static_cast<unsigned char>(input[position])));
  value = static_cast<uint32_t>(parsed);
  return true;
}

bool skipSimpleValue(const std::string& input, size_t& position) {
  skipWhitespace(input, position);
  std::string ignored;
  if (position < input.size() && input[position] == '"') {
    return parseString(input, position, ignored);
  }
  const char* literals[] = {"true", "false", "null"};
  for (size_t index = 0; index < 3; ++index) {
    const std::string literal(literals[index]);
    if (input.compare(position, literal.size(), literal) == 0) {
      position += literal.size();
      return true;
    }
  }

  size_t cursor = position;
  if (cursor < input.size() && input[cursor] == '-') ++cursor;
  if (cursor >= input.size() ||
      !std::isdigit(static_cast<unsigned char>(input[cursor]))) {
    return false;
  }
  if (input[cursor] == '0') {
    ++cursor;
  } else {
    while (cursor < input.size() &&
           std::isdigit(static_cast<unsigned char>(input[cursor]))) {
      ++cursor;
    }
  }
  if (cursor < input.size() && input[cursor] == '.') {
    ++cursor;
    if (cursor >= input.size() ||
        !std::isdigit(static_cast<unsigned char>(input[cursor]))) {
      return false;
    }
    while (cursor < input.size() &&
           std::isdigit(static_cast<unsigned char>(input[cursor]))) {
      ++cursor;
    }
  }
  if (cursor < input.size() &&
      (input[cursor] == 'e' || input[cursor] == 'E')) {
    ++cursor;
    if (cursor < input.size() &&
        (input[cursor] == '+' || input[cursor] == '-')) {
      ++cursor;
    }
    if (cursor >= input.size() ||
        !std::isdigit(static_cast<unsigned char>(input[cursor]))) {
      return false;
    }
    while (cursor < input.size() &&
           std::isdigit(static_cast<unsigned char>(input[cursor]))) {
      ++cursor;
    }
  }
  position = cursor;
  return true;
}

bool parseConfirmation(const std::string& input, std::string& trackerId,
                       uint32_t& trackingSessionNumber,
                       uint32_t& highestStoredPointNumber) {
  size_t position = 0;
  skipWhitespace(input, position);
  if (position >= input.size() || input[position++] != '{') return false;

  bool hasTracker = false;
  bool hasSession = false;
  bool hasHighest = false;
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
    } else if (key == "highest_stored_point_number") {
      if (hasHighest || !parseUInt(input, position, highestStoredPointNumber)) {
        return false;
      }
      hasHighest = true;
    } else if (!skipSimpleValue(input, position)) {
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
  return position == input.size() && hasTracker && hasSession && hasHighest;
}

bool findFieldValue(const std::string& json, const char* field,
                    size_t& position) {
  const std::string needle = std::string("\"") + field + "\":";
  position = json.find(needle);
  if (position == std::string::npos) return false;
  position += needle.size();
  return true;
}

bool findStringField(const std::string& json, const char* field,
                     std::string& value) {
  size_t position = 0;
  if (!findFieldValue(json, field, position)) return false;
  return parseString(json, position, value);
}

bool findUIntField(const std::string& json, const char* field,
                   uint32_t& value) {
  size_t position = 0;
  if (!findFieldValue(json, field, position)) return false;
  return parseUInt(json, position, value);
}

bool hasRequiredUrl(const std::string& url) {
  if (url.compare(0, 8, "https://") != 0) return false;
  const size_t path = url.find('/', 8);
  return path != std::string::npos && url.substr(path) == REQUIRED_PATH;
}

}  // namespace

std::string formatUInt32(uint32_t value) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%lu",
                static_cast<unsigned long>(value));
  return buffer;
}

bool buildUploadRequest(const std::string& uploadUrl,
                        const std::string& bearerToken,
                        const UploadBatch& batch, UploadRequest& request) {
  if (!hasRequiredUrl(uploadUrl) || bearerToken.empty() ||
      batch.trackerId.empty() || batch.trackingSessionNumber == 0 ||
      batch.firstPointNumber == 0 || batch.ndjsonPoints.empty() ||
      batch.ndjsonPoints.size() > 30) {
    return false;
  }

  std::string body;
  for (size_t index = 0; index < batch.ndjsonPoints.size(); ++index) {
    const uint32_t expectedPoint =
        batch.firstPointNumber + static_cast<uint32_t>(index);
    if (expectedPoint < batch.firstPointNumber) return false;
    std::string trackerId;
    uint32_t schemaVersion = 0;
    uint32_t trackingSessionNumber = 0;
    uint32_t pointNumber = 0;
    const std::string& point = batch.ndjsonPoints[index];
    if (!findUIntField(point, "schema_version", schemaVersion) ||
        !findStringField(point, "tracker_id", trackerId) ||
        !findUIntField(point, "tracking_session_number",
                       trackingSessionNumber) ||
        !findUIntField(point, "point_number", pointNumber) ||
        schemaVersion != batch.schemaVersion || trackerId != batch.trackerId ||
        trackingSessionNumber != batch.trackingSessionNumber ||
        pointNumber != expectedPoint) {
      return false;
    }
    body += point;
    body += '\n';
  }

  const uint32_t lastPoint = batch.firstPointNumber +
                             static_cast<uint32_t>(batch.ndjsonPoints.size()) - 1;
  request.url = uploadUrl;
  request.headers.clear();
  request.headers.push_back({"Authorization", "Bearer " + bearerToken});
  request.headers.push_back({"Content-Type", "application/x-ndjson"});
  request.headers.push_back(
      {"X-Track-Point-Schema-Version", formatUInt32(batch.schemaVersion)});
  request.headers.push_back({"X-Tracker-ID", batch.trackerId});
  request.headers.push_back(
      {"X-Tracking-Session-Number", formatUInt32(batch.trackingSessionNumber)});
  request.headers.push_back(
      {"X-First-Point-Number", formatUInt32(batch.firstPointNumber)});
  request.headers.push_back({"X-Last-Point-Number", formatUInt32(lastPoint)});
  request.body = body;
  return true;
}

bool validateUploadResponse(int statusCode, const std::string& responseBody,
                            const UploadBatch& batch,
                            UploadConfirmation& confirmation) {
  if (statusCode < 200 || statusCode >= 300 || batch.ndjsonPoints.empty()) {
    return false;
  }
  std::string trackerId;
  uint32_t trackingSessionNumber = 0;
  uint32_t highestStoredPointNumber = 0;
  if (!parseConfirmation(responseBody, trackerId, trackingSessionNumber,
                         highestStoredPointNumber)) {
    return false;
  }
  const uint32_t requiredPoint = batch.firstPointNumber +
      static_cast<uint32_t>(batch.ndjsonPoints.size()) - 1;
  if (requiredPoint < batch.firstPointNumber || trackerId != batch.trackerId ||
      trackingSessionNumber != batch.trackingSessionNumber ||
      highestStoredPointNumber < requiredPoint) {
    return false;
  }
  confirmation.highestStoredPointNumber = highestStoredPointNumber;
  return true;
}

}  // namespace tracking
