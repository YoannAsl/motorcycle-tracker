#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace tracking {

struct CleanupSession {
  uint32_t trackingSessionNumber = 0;
  uint64_t bytes = 0;
  bool inactive = false;
  bool allPointsConfirmed = false;
  bool diagnosticLogConfirmed = false;
};

struct CleanupPlan {
  bool cleanupRequired = false;
  bool targetReached = false;
  bool protectedDataRemains = false;
  std::vector<uint32_t> sessionsToDelete;
};

enum class CleanupStorageStatus {
  ok,
  lockUnavailable,
  metricUnavailable,
  scanFailed,
  deleteFailed,
  diagnosticWriteFailed
};

enum class CleanupOutcome {
  notRequired,
  targetReached,
  waitingForDelivery,
  retryRequired
};

struct CleanupRun {
  CleanupOutcome outcome = CleanupOutcome::notRequired;
  CleanupStorageStatus failure = CleanupStorageStatus::ok;
  std::vector<uint32_t> deletedSessions;
};

class StorageCleanupStorage {
 public:
  virtual ~StorageCleanupStorage() = default;
  virtual CleanupStorageStatus readUsage(uint64_t& capacityBytes,
                                         uint64_t& usedBytes) = 0;
  virtual CleanupStorageStatus readCleanupSessions(
      std::vector<CleanupSession>& sessions) = 0;
  virtual CleanupStorageStatus deleteSessionFiles(
      uint32_t trackingSessionNumber) = 0;
  virtual void forgetDeletedSession(uint32_t trackingSessionNumber) = 0;
  virtual CleanupStorageStatus flushPendingCleanupDiagnostic() = 0;
  virtual CleanupStorageStatus persistCleanupDiagnostic(
      const std::string& message) = 0;
};

enum class CleanupTrigger { startup, deliveryConfirmation };

class StorageCleanupSchedule {
 public:
  void request(CleanupTrigger trigger);
  bool ready(uint32_t nowMilliseconds) const;
  void recordRun(const CleanupRun& run, uint32_t nowMilliseconds);
  bool pending() const;
  CleanupTrigger trigger() const;

 private:
  bool pending_ = false;
  uint32_t retryAtMilliseconds_ = 0;
  CleanupTrigger trigger_ = CleanupTrigger::startup;
};

bool storageCleanupRequired(uint64_t capacityBytes, uint64_t usedBytes);
bool storageCleanupTargetReached(uint64_t capacityBytes, uint64_t usedBytes);

CleanupPlan planStorageCleanup(
    uint64_t capacityBytes, uint64_t usedBytes,
    const std::vector<CleanupSession>& sessions);

CleanupRun runStorageCleanup(StorageCleanupStorage& storage);

const char* cleanupStorageStatusName(CleanupStorageStatus status);

}  // namespace tracking
