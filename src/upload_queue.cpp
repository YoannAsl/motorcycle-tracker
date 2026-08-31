#include "upload_queue.h"

namespace tracking {

BatchReadStatus readFullPendingBatch(
    const std::vector<RecoveredTrackingSession>& sessions,
    UploadPointSource& source, const std::string& trackerId,
    UploadBatch& batch) {
  const size_t batchSize = 30;
  for (std::vector<RecoveredTrackingSession>::const_iterator session =
           sessions.begin();
       session != sessions.end(); ++session) {
    if (session->highestConfirmedPoint > session->highestRecordedPoint) {
      return BatchReadStatus::malformed_input;
    }
    if (session->highestRecordedPoint - session->highestConfirmedPoint <
        batchSize) {
      continue;
    }

    UploadBatch candidate;
    candidate.trackerId = trackerId;
    candidate.trackingSessionNumber = session->trackingSessionNumber;
    candidate.firstPointNumber = session->highestConfirmedPoint + 1;
    const BatchReadStatus status = source.readPointLines(
        candidate.trackingSessionNumber, candidate.firstPointNumber, batchSize,
        candidate.ndjsonPoints);
    if (status != BatchReadStatus::ready) return status;
    if (candidate.ndjsonPoints.size() != batchSize) {
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
  return BatchReadStatus::no_full_batch;
}

}  // namespace tracking
