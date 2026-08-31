#include "raw_point_log.h"

namespace tracking {

bool appendCompleteRawPoint(RawPointLogStorage& storage,
                            const std::string& ndjson) {
  const size_t bodyWritten = storage.append(ndjson.data(), ndjson.size());
  if (bodyWritten != ndjson.size()) {
    storage.flush();
    storage.abandon();
    return false;
  }

  const char newline = '\n';
  const size_t newlineWritten = storage.append(&newline, 1);
  storage.flush();
  if (newlineWritten != 1) {
    storage.abandon();
    return false;
  }
  return true;
}

}  // namespace tracking
