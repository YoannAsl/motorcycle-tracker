#include "storage_cleanup.h"

#include <algorithm>
#include <cstdio>

namespace tracking {

namespace {

const uint32_t CLEANUP_RETRY_DELAY_MS = 60000;

uint64_t ceilingFraction(uint64_t value, uint64_t numerator,
                         uint64_t denominator) {
  const uint64_t quotient = value / denominator;
  const uint64_t remainder = value % denominator;
  return quotient * numerator +
         (remainder * numerator + denominator - 1) / denominator;
}

bool oldestFirst(const CleanupSession& left, const CleanupSession& right) {
  return left.trackingSessionNumber < right.trackingSessionNumber;
}

bool eligibleForDeletion(const CleanupSession& session) {
  return session.inactive && session.allPointsConfirmed &&
         session.diagnosticLogConfirmed;
}

CleanupStorageStatus persist(StorageCleanupStorage& storage,
                             const std::string& message,
                             CleanupRun& run) {
  const CleanupStorageStatus status = storage.persistCleanupDiagnostic(message);
  if (status != CleanupStorageStatus::ok) {
    if (run.failure == CleanupStorageStatus::ok) run.failure = status;
    run.outcome = CleanupOutcome::retryRequired;
  }
  return status;
}

bool refreshUsage(StorageCleanupStorage& storage, uint64_t& capacityBytes,
                  uint64_t& usedBytes, CleanupRun& run) {
  const CleanupStorageStatus status =
      storage.readUsage(capacityBytes, usedBytes);
  if (status == CleanupStorageStatus::ok) return true;
  run.outcome = CleanupOutcome::retryRequired;
  if (run.failure == CleanupStorageStatus::ok) run.failure = status;
  return false;
}

}  // namespace

bool storageCleanupRequired(uint64_t capacityBytes, uint64_t usedBytes) {
  return capacityBytes != 0 &&
         usedBytes >= ceilingFraction(capacityBytes, 4, 5);
}

bool storageCleanupTargetReached(uint64_t capacityBytes, uint64_t usedBytes) {
  return capacityBytes != 0 &&
         usedBytes < ceilingFraction(capacityBytes, 7, 10);
}

CleanupPlan planStorageCleanup(
    uint64_t capacityBytes, uint64_t usedBytes,
    const std::vector<CleanupSession>& sessions) {
  CleanupPlan plan;
  if (!storageCleanupRequired(capacityBytes, usedBytes)) {
    return plan;
  }

  plan.cleanupRequired = true;
  const uint64_t target = ceilingFraction(capacityBytes, 7, 10);
  std::vector<CleanupSession> oldest = sessions;
  std::sort(oldest.begin(), oldest.end(), oldestFirst);
  for (std::vector<CleanupSession>::const_iterator session = oldest.begin();
       session != oldest.end() && usedBytes >= target; ++session) {
    if (!eligibleForDeletion(*session)) {
      if (session->bytes > 0) plan.protectedDataRemains = true;
      continue;
    }
    plan.sessionsToDelete.push_back(session->trackingSessionNumber);
    usedBytes = session->bytes >= usedBytes ? 0 : usedBytes - session->bytes;
  }
  plan.targetReached = storageCleanupTargetReached(capacityBytes, usedBytes);
  return plan;
}

CleanupRun runStorageCleanup(StorageCleanupStorage& storage) {
  CleanupRun run;
  const CleanupStorageStatus flushStatus =
      storage.flushPendingCleanupDiagnostic();
  if (flushStatus != CleanupStorageStatus::ok) {
    run.outcome = CleanupOutcome::retryRequired;
    run.failure = flushStatus;
    return run;
  }
  uint64_t capacityBytes = 0;
  uint64_t usedBytes = 0;
  if (!refreshUsage(storage, capacityBytes, usedBytes, run)) return run;
  if (!storageCleanupRequired(capacityBytes, usedBytes)) return run;

  std::vector<CleanupSession> sessions;
  const CleanupStorageStatus scanStatus =
      storage.readCleanupSessions(sessions);
  if (scanStatus != CleanupStorageStatus::ok) {
    run.outcome = CleanupOutcome::retryRequired;
    run.failure = scanStatus;
    persist(storage, "[ERROR] cleanup session scan failed\n", run);
    return run;
  }

  const CleanupPlan plan =
      planStorageCleanup(capacityBytes, usedBytes, sessions);
  std::sort(sessions.begin(), sessions.end(), oldestFirst);
  bool deletionFailed = false;
  for (std::vector<CleanupSession>::const_iterator session = sessions.begin();
       session != sessions.end(); ++session) {
    if (!eligibleForDeletion(*session)) continue;
    const CleanupStorageStatus deleteStatus =
        storage.deleteSessionFiles(session->trackingSessionNumber);
    if (deleteStatus != CleanupStorageStatus::ok) {
      deletionFailed = true;
      if (run.failure == CleanupStorageStatus::ok) run.failure = deleteStatus;
      char message[96];
      std::snprintf(message, sizeof(message),
                    "[ERROR] cleanup delete failed session=%lu\n",
                    static_cast<unsigned long>(
                        session->trackingSessionNumber));
      if (persist(storage, message, run) != CleanupStorageStatus::ok) return run;
    } else {
      storage.forgetDeletedSession(session->trackingSessionNumber);
      run.deletedSessions.push_back(session->trackingSessionNumber);
      char message[96];
      std::snprintf(message, sizeof(message),
                    "[CLEANUP] deleted delivered session=%lu\n",
                    static_cast<unsigned long>(
                        session->trackingSessionNumber));
      if (persist(storage, message, run) != CleanupStorageStatus::ok) return run;
    }

    if (!refreshUsage(storage, capacityBytes, usedBytes, run)) return run;
    if (storageCleanupTargetReached(capacityBytes, usedBytes) &&
        run.failure != CleanupStorageStatus::diagnosticWriteFailed) {
      run.outcome = CleanupOutcome::targetReached;
      return run;
    }
  }

  run.outcome = deletionFailed ? CleanupOutcome::retryRequired
                               : CleanupOutcome::waitingForDelivery;
  if (!plan.protectedDataRemains) {
    run.outcome = CleanupOutcome::retryRequired;
  }
  const char* message = plan.protectedDataRemains
      ? "[CLEANUP] unable to reach below 70 percent; pending data protected\n"
      : "[ERROR] cleanup unable to reach below 70 percent\n";
  if (persist(storage, message, run) != CleanupStorageStatus::ok) return run;
  if (!refreshUsage(storage, capacityBytes, usedBytes, run)) return run;
  if (storageCleanupTargetReached(capacityBytes, usedBytes) &&
      run.failure != CleanupStorageStatus::diagnosticWriteFailed) {
    run.outcome = CleanupOutcome::targetReached;
  } else if (run.failure == CleanupStorageStatus::diagnosticWriteFailed) {
    run.outcome = CleanupOutcome::retryRequired;
  }
  return run;
}

void StorageCleanupSchedule::request(CleanupTrigger trigger) {
  pending_ = true;
  retryAtMilliseconds_ = 0;
  trigger_ = trigger;
}

bool StorageCleanupSchedule::ready(uint32_t nowMilliseconds) const {
  return pending_ &&
         static_cast<int32_t>(nowMilliseconds - retryAtMilliseconds_) >= 0;
}

void StorageCleanupSchedule::recordRun(const CleanupRun& run,
                                       uint32_t nowMilliseconds) {
  if (run.outcome == CleanupOutcome::retryRequired) {
    retryAtMilliseconds_ = nowMilliseconds + CLEANUP_RETRY_DELAY_MS;
  } else {
    pending_ = false;
  }
}

bool StorageCleanupSchedule::pending() const { return pending_; }

CleanupTrigger StorageCleanupSchedule::trigger() const { return trigger_; }

const char* cleanupStorageStatusName(CleanupStorageStatus status) {
  switch (status) {
    case CleanupStorageStatus::ok: return "none";
    case CleanupStorageStatus::lockUnavailable: return "SD lock unavailable";
    case CleanupStorageStatus::metricUnavailable: return "SD metrics unavailable";
    case CleanupStorageStatus::scanFailed: return "session scan failed";
    case CleanupStorageStatus::deleteFailed: return "session deletion failed";
    case CleanupStorageStatus::diagnosticWriteFailed:
      return "diagnostic persistence failed";
  }
  return "unknown";
}

}  // namespace tracking
