#include "upload_queue.h"

#include "delivery_scheduler.h"

namespace tracking {

BatchReadStatus readPendingBatch(
    const std::vector<RecoveredTrackingSession>& sessions,
    UploadPointSource& source, const std::string& trackerId,
    UploadBatch& batch) {
  for (std::vector<RecoveredTrackingSession>::const_iterator session =
           sessions.begin();
       session != sessions.end(); ++session) {
    if (session->highestConfirmedPoint > session->highestRecordedPoint) {
      return BatchReadStatus::malformed_input;
    }
  }

  PendingDeliveryBatch pending;
  if (!selectOldestPendingBatch(sessions, pending)) {
    return BatchReadStatus::no_pending_batch;
  }

  UploadBatch candidate;
  candidate.trackerId = trackerId;
  candidate.trackingSessionNumber = pending.trackingSessionNumber;
  candidate.firstPointNumber = pending.firstPointNumber;
  const BatchReadStatus status = source.readPointLines(
      candidate.trackingSessionNumber, candidate.firstPointNumber,
      pending.pointCount, candidate.ndjsonPoints);
  if (status != BatchReadStatus::ready) return status;
  if (candidate.ndjsonPoints.size() != pending.pointCount) {
    return BatchReadStatus::malformed_input;
  }
  for (std::vector<std::string>::const_iterator line =
           candidate.ndjsonPoints.begin();
       line != candidate.ndjsonPoints.end(); ++line) {
    if (line->empty()) return BatchReadStatus::malformed_input;
  }
  batch = candidate;
  return BatchReadStatus::ready;
}

}  // namespace tracking
