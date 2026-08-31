#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace tracking {

struct StoredTrackingSession {
  uint32_t trackingSessionNumber = 0;
  uint32_t highestRecordedPoint = 0;
  std::string deliveryState;
  bool deliveryStateReadable = true;
};

struct RecoveredTrackingSession {
  uint32_t trackingSessionNumber = 0;
  uint32_t highestRecordedPoint = 0;
  uint32_t highestConfirmedPoint = 0;
  bool inactive = true;
  bool recoveryRequired = false;
};

enum class RecoveryReadStatus { readable, missing, unreadable };

struct RecoveryDirectoryEntry {
  std::string name;
  bool directory = false;
};

class DeliveryRecoveryStorage {
 public:
  virtual ~DeliveryRecoveryStorage() = default;
  virtual bool listRoot(std::vector<RecoveryDirectoryEntry>& entries) = 0;
  virtual RecoveryReadStatus countCompleteLines(const std::string& path,
                                                uint32_t& count) = 0;
  virtual RecoveryReadStatus readText(const std::string& path,
                                      std::string& contents) = 0;
  virtual bool appendText(const std::string& path,
                          const std::string& contents) = 0;
};

class DeliveryRecovery {
 public:
  bool restore(DeliveryRecoveryStorage& storage);
  bool ready() const;
  uint32_t maxStoredTrackingSessionNumber() const;
  const std::vector<RecoveredTrackingSession>& sessions() const;
  const RecoveredTrackingSession* oldestPendingSession() const;
  bool beginSession(uint32_t trackingSessionNumber);
  bool recordPoint(uint32_t trackingSessionNumber);
  bool confirmDeliveryThrough(DeliveryRecoveryStorage& storage,
                              uint32_t trackingSessionNumber,
                              uint32_t highestConfirmedPoint);

 private:
  bool ready_ = false;
  uint32_t maxStoredTrackingSessionNumber_ = 0;
  std::vector<RecoveredTrackingSession> sessions_;
};

std::string serializeDeliveryProgress(uint32_t trackingSessionNumber,
                                      uint32_t highestConfirmedPoint);

bool nextTrackingSessionNumber(uint32_t persistedSessionNumber,
                               uint32_t highestStoredSessionNumber,
                               uint32_t& nextSessionNumber);

std::vector<RecoveredTrackingSession> recoverTrackingSessions(
    const std::vector<StoredTrackingSession>& storedSessions);

}  // namespace tracking
