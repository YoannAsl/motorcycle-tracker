#include <unity.h>

#include <algorithm>
#include <string>
#include <vector>

#include "raw_point_log.h"

void setUp() {}
void tearDown() {}

namespace {

class ScenarioStorage : public tracking::RawPointLogStorage {
 public:
  size_t append(const char* bytes, size_t length) override {
    ++appendAttempts;
    if (abandoned) return 0;
    const size_t written = std::min(length, writeLimits.front());
    writeLimits.erase(writeLimits.begin());
    contents.append(bytes, written);
    return written;
  }

  void flush() override { ++flushes; }
  void abandon() override { abandoned = true; }

  std::vector<size_t> writeLimits;
  std::string contents;
  int appendAttempts = 0;
  int flushes = 0;
  bool abandoned = false;
};

void complete_body_and_newline_are_persisted() {
  ScenarioStorage storage;
  storage.writeLimits.push_back(64);
  storage.writeLimits.push_back(1);

  TEST_ASSERT_TRUE(tracking::appendCompleteRawPoint(storage, "{\"point\":1}"));
  TEST_ASSERT_EQUAL_STRING("{\"point\":1}\n", storage.contents.c_str());
  TEST_ASSERT_EQUAL(2, storage.appendAttempts);
  TEST_ASSERT_EQUAL(1, storage.flushes);
  TEST_ASSERT_FALSE(storage.abandoned);
}

void short_body_is_not_terminated_and_file_is_abandoned() {
  ScenarioStorage storage;
  storage.writeLimits.push_back(5);

  TEST_ASSERT_FALSE(
      tracking::appendCompleteRawPoint(storage, "{\"point\":1}"));
  TEST_ASSERT_EQUAL_STRING("{\"poi", storage.contents.c_str());
  TEST_ASSERT_EQUAL(1, storage.appendAttempts);
  TEST_ASSERT_EQUAL(1, storage.flushes);
  TEST_ASSERT_TRUE(storage.abandoned);
}

void failed_newline_abandons_the_file_for_recovery_on_reboot() {
  ScenarioStorage storage;
  storage.writeLimits.push_back(64);
  storage.writeLimits.push_back(0);

  TEST_ASSERT_FALSE(
      tracking::appendCompleteRawPoint(storage, "{\"point\":1}"));
  TEST_ASSERT_EQUAL_STRING("{\"point\":1}", storage.contents.c_str());
  TEST_ASSERT_EQUAL(2, storage.appendAttempts);
  TEST_ASSERT_EQUAL(1, storage.flushes);
  TEST_ASSERT_TRUE(storage.abandoned);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(complete_body_and_newline_are_persisted);
  RUN_TEST(short_body_is_not_terminated_and_file_is_abandoned);
  RUN_TEST(failed_newline_abandons_the_file_for_recovery_on_reboot);
  return UNITY_END();
}
