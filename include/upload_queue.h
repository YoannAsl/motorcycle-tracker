#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "delivery_recovery.h"
#include "upload_contract.h"

namespace tracking {

enum class BatchReadStatus {
  ready,
  no_pending_batch,
  lock_unavailable,
  storage_open_failed,
  malformed_input
};

class UploadPointSource {
 public:
  virtual ~UploadPointSource() = default;
  virtual BatchReadStatus readPointLines(uint32_t trackingSessionNumber,
                                         uint32_t firstPointNumber,
                                         size_t pointCount,
                                         std::vector<std::string>& lines) = 0;
};

BatchReadStatus readPendingBatch(
    const std::vector<RecoveredTrackingSession>& sessions,
    UploadPointSource& source, const std::string& trackerId,
    UploadBatch& batch);

}  // namespace tracking
