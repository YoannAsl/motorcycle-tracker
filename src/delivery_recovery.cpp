#include "delivery_recovery.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace tracking {

namespace {

uint32_t checksum(uint32_t trackingSessionNumber,
                  uint32_t highestConfirmedPoint) {
  uint32_t value = 2166136261u;
  const uint32_t fields[] = {1u, trackingSessionNumber, highestConfirmedPoint};
  for (size_t field = 0; field < 3; ++field) {
    for (size_t byte = 0; byte < 4; ++byte) {
      value ^= (fields[field] >> (byte * 8)) & 0xffu;
      value *= 16777619u;
    }
  }
  return value;
}

bool parseDeliveryProgressRecord(const std::string& line,
                                 uint32_t expectedSession,
                                 uint32_t& confirmedPoint) {
  unsigned long version = 0;
  unsigned long session = 0;
  unsigned long confirmed = 0;
  unsigned long storedChecksum = 0;
  char trailing = '\0';
  const int fields = std::sscanf(line.c_str(), "%lu,%lu,%lu,%lx%c", &version,
                                 &session, &confirmed, &storedChecksum,
                                 &trailing);
  if (fields != 4 || version != 1 || session != expectedSession ||
      session > UINT32_MAX || confirmed > UINT32_MAX ||
      storedChecksum > UINT32_MAX) {
    return false;
  }
  const uint32_t parsedConfirmed = static_cast<uint32_t>(confirmed);
  if (storedChecksum != checksum(static_cast<uint32_t>(session),
                                 parsedConfirmed)) {
    return false;
  }
  confirmedPoint = parsedConfirmed;
  return true;
}

bool parseSessionDirectoryName(const std::string& path,
                               uint32_t& trackingSessionNumber) {
  const size_t slash = path.find_last_of("/\\");
  const std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
  unsigned long parsed = 0;
  char trailing = '\0';
  if (std::sscanf(name.c_str(), "session-%lu%c", &parsed, &trailing) != 1 ||
      parsed == 0 || parsed > UINT32_MAX) {
    return false;
  }
  trackingSessionNumber = static_cast<uint32_t>(parsed);
  return true;
}

std::string sessionFilePath(uint32_t trackingSessionNumber,
                            const char* filename) {
  char path[80];
  std::snprintf(path, sizeof(path), "/session-%010lu/%s",
                static_cast<unsigned long>(trackingSessionNumber), filename);
  return path;
}

bool bySessionNumber(const RecoveredTrackingSession& left,
                     const RecoveredTrackingSession& right) {
  return left.trackingSessionNumber < right.trackingSessionNumber;
}

}  // namespace

std::string serializeDeliveryProgress(uint32_t trackingSessionNumber,
                                      uint32_t highestConfirmedPoint) {
  char record[64];
  std::snprintf(record, sizeof(record), "1,%lu,%lu,%08lx\n",
                static_cast<unsigned long>(trackingSessionNumber),
                static_cast<unsigned long>(highestConfirmedPoint),
                static_cast<unsigned long>(
                    checksum(trackingSessionNumber, highestConfirmedPoint)));
  return record;
}

bool nextTrackingSessionNumber(uint32_t persistedSessionNumber,
                               uint32_t highestStoredSessionNumber,
                               uint32_t& nextSessionNumber) {
  const uint32_t previous =
      std::max(persistedSessionNumber, highestStoredSessionNumber);
  if (previous == UINT32_MAX) return false;
  nextSessionNumber = previous + 1;
  return true;
}

std::vector<RecoveredTrackingSession> recoverTrackingSessions(
    const std::vector<StoredTrackingSession>& storedSessions) {
  std::vector<RecoveredTrackingSession> recovered;
  recovered.reserve(storedSessions.size());
  for (std::vector<StoredTrackingSession>::const_iterator stored =
           storedSessions.begin();
       stored != storedSessions.end(); ++stored) {
    RecoveredTrackingSession session;
    session.trackingSessionNumber = stored->trackingSessionNumber;
    session.highestRecordedPoint = stored->highestRecordedPoint;
    session.recoveryRequired =
        !stored->deliveryStateReadable ||
        (stored->highestRecordedPoint > 0 && stored->deliveryState.empty());

    std::istringstream records(stored->deliveryState);
    std::string line;
    while (std::getline(records, line)) {
      uint32_t confirmedPoint = 0;
      if (parseDeliveryProgressRecord(line, stored->trackingSessionNumber,
                                      confirmedPoint) &&
          confirmedPoint <= stored->highestRecordedPoint) {
        if (confirmedPoint < session.highestConfirmedPoint) {
          session.recoveryRequired = true;
        }
        session.highestConfirmedPoint = confirmedPoint;
      } else {
        session.recoveryRequired = true;
      }
    }
    recovered.push_back(session);
  }
  std::sort(recovered.begin(), recovered.end(), bySessionNumber);
  return recovered;
}

bool DeliveryRecovery::restore(DeliveryRecoveryStorage& storage) {
  ready_ = false;
  std::vector<RecoveryDirectoryEntry> entries;
  if (!storage.listRoot(entries)) return false;

  uint32_t recoveredMaximum = 0;
  std::vector<StoredTrackingSession> storedSessions;
  for (std::vector<RecoveryDirectoryEntry>::const_iterator entry =
           entries.begin();
       entry != entries.end(); ++entry) {
    uint32_t sessionNumber = 0;
    if (!entry->directory ||
        !parseSessionDirectoryName(entry->name, sessionNumber)) {
      continue;
    }

    StoredTrackingSession stored;
    stored.trackingSessionNumber = sessionNumber;
    const RecoveryReadStatus pointsStatus = storage.countCompleteLines(
        sessionFilePath(sessionNumber, "track-points.ndjson"),
        stored.highestRecordedPoint);
    if (pointsStatus != RecoveryReadStatus::readable) return false;

    const RecoveryReadStatus stateStatus = storage.readText(
        sessionFilePath(sessionNumber, "delivery-state.log"),
        stored.deliveryState);
    stored.deliveryStateReadable =
        stateStatus == RecoveryReadStatus::readable;
    storedSessions.push_back(stored);
    recoveredMaximum = std::max(recoveredMaximum, sessionNumber);
  }

  std::vector<RecoveredTrackingSession> recovered =
      recoverTrackingSessions(storedSessions);
  sessions_.swap(recovered);
  maxStoredTrackingSessionNumber_ = recoveredMaximum;
  ready_ = true;
  return true;
}

bool DeliveryRecovery::ready() const { return ready_; }

uint32_t DeliveryRecovery::maxStoredTrackingSessionNumber() const {
  return maxStoredTrackingSessionNumber_;
}

const std::vector<RecoveredTrackingSession>& DeliveryRecovery::sessions()
    const {
  return sessions_;
}

const RecoveredTrackingSession* DeliveryRecovery::oldestPendingSession() const {
  for (std::vector<RecoveredTrackingSession>::const_iterator session =
           sessions_.begin();
       session != sessions_.end(); ++session) {
    if (session->inactive &&
        session->highestConfirmedPoint < session->highestRecordedPoint) {
      return &*session;
    }
  }
  return nullptr;
}

bool DeliveryRecovery::beginSession(uint32_t trackingSessionNumber) {
  if (!ready_ || trackingSessionNumber == 0 ||
      trackingSessionNumber <= maxStoredTrackingSessionNumber_) {
    return false;
  }
  RecoveredTrackingSession session;
  session.trackingSessionNumber = trackingSessionNumber;
  session.inactive = false;
  sessions_.push_back(session);
  maxStoredTrackingSessionNumber_ = trackingSessionNumber;
  return true;
}

bool DeliveryRecovery::recordPoint(uint32_t trackingSessionNumber) {
  for (std::vector<RecoveredTrackingSession>::iterator session =
           sessions_.begin();
       session != sessions_.end(); ++session) {
    if (session->trackingSessionNumber != trackingSessionNumber ||
        session->inactive || session->highestRecordedPoint == UINT32_MAX) {
      continue;
    }
    ++session->highestRecordedPoint;
    return true;
  }
  return false;
}

bool DeliveryRecovery::confirmDeliveryThrough(
    DeliveryRecoveryStorage& storage, uint32_t trackingSessionNumber,
    uint32_t highestConfirmedPoint) {
  for (std::vector<RecoveredTrackingSession>::iterator session =
           sessions_.begin();
       session != sessions_.end(); ++session) {
    if (session->trackingSessionNumber != trackingSessionNumber) continue;
    if (highestConfirmedPoint < session->highestConfirmedPoint ||
        highestConfirmedPoint > session->highestRecordedPoint) {
      return false;
    }
    const std::string record = serializeDeliveryProgress(
        trackingSessionNumber, highestConfirmedPoint);
    if (!storage.appendText(
            sessionFilePath(trackingSessionNumber, "delivery-state.log"),
            record)) {
      return false;
    }
    session->highestConfirmedPoint = highestConfirmedPoint;
    session->recoveryRequired = false;
    return true;
  }
  return false;
}

}  // namespace tracking
