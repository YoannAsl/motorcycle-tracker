#pragma once

#include <stddef.h>
#include <string>

namespace tracking {

class RawPointLogStorage {
 public:
  virtual ~RawPointLogStorage() = default;
  virtual size_t append(const char* bytes, size_t length) = 0;
  virtual void flush() = 0;
  virtual void abandon() = 0;
};

bool appendCompleteRawPoint(RawPointLogStorage& storage,
                            const std::string& ndjson);

}  // namespace tracking
