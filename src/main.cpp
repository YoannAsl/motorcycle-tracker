// ESP32 SD card module diagnostic (SPI)
// Wiring (matches your table):
//   3V3 -> 3.3V (use 3.3V, not VIN/5V)
//   GND -> GND
//   CLK -> GPIO18 (aka SCK)
//   DO  -> GPIO19 (aka MISO/CIPO)
//   DI  -> GPIO23 (aka MOSI/COPI)
//   CS  -> GPIO5 (chip-select; can use another free GPIO)

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

static const uint32_t USB_BAUD = 115200;
static const int SD_CS_PIN   = 5;
static const int SD_SCK_PIN  = 18;
static const int SD_MISO_PIN = 19;
static const int SD_MOSI_PIN = 23;

static void waitForSerial() {
  Serial.begin(USB_BAUD);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) { delay(10); }
  delay(100);
}

static const char* cardTypeName(uint8_t t) {
  switch (t) {
    case CARD_NONE: return "NONE";
    case CARD_MMC:  return "MMC";
    case CARD_SD:   return "SDSC";
    case CARD_SDHC: return "SDHC/SDXC";
    default:        return "UNKNOWN";
  }
}

static void listDir(fs::FS &fs, const char * dirname) {
  Serial.printf("[SD] Listing: %s\n", dirname);
  File root = fs.open(dirname);
  if (!root) { Serial.println("  open failed"); return; }
  if (!root.isDirectory()) { Serial.println("  not a dir"); return; }
  File file = root.openNextFile();
  if (!file) { Serial.println("  (empty)"); }
  while (file) {
    Serial.printf("  %s  %u bytes\n", file.name(), (unsigned)file.size());
    file = root.openNextFile();
  }
}

static bool writeTestFile() {
  const char* path = "/sd_test.txt";
  SD.remove(path); // start fresh
  File f = SD.open(path, FILE_WRITE);
  if (!f) { Serial.println("[SD] Open for write FAILED"); return false; }
  f.println("SD card write test");
  f.printf("millis=%lu\n", (unsigned long)millis());
  for (int i = 0; i < 5; ++i) f.printf("line %d\n", i + 1);
  f.flush();
  f.close();
  Serial.printf("[SD] Wrote %s\n", path);
  return true;
}

static void readBackTestFile() {
  const char* path = "/sd_test.txt";
  File f = SD.open(path, FILE_READ);
  if (!f) { Serial.println("[SD] Open for read FAILED"); return; }
  Serial.printf("[SD] Reading %s:\n", path);
  while (f.available()) Serial.write(f.read());
  f.close();
}

static void throughputTestKB(size_t kib) {
  const char* path = "/sd_speed.bin";
  const size_t block = 512; // bytes per write
  const size_t totalBytes = kib * 1024ULL;
  static uint8_t buf[block];
  for (size_t i = 0; i < block; ++i) buf[i] = (uint8_t)i;

  SD.remove(path);
  File f = SD.open(path, FILE_WRITE);
  if (!f) { Serial.println("[SD] Open for throughput FAILED"); return; }
  unsigned long t0 = millis();
  size_t written = 0;
  while (written < totalBytes) {
    size_t toWrite = min(block, totalBytes - written);
    size_t w = f.write(buf, toWrite);
    if (w != toWrite) { Serial.println("[SD] Short write"); break; }
    written += w;
  }
  f.flush();
  f.close();
  unsigned long dt = millis() - t0;
  float kbps = dt ? (written / 1024.0f) / (dt / 1000.0f) : 0.0f;
  Serial.printf("[SD] Wrote %u bytes in %lu ms (%.1f KiB/s)\n",
                (unsigned)written, dt, kbps);
}

void setup() {
  waitForSerial();
  Serial.println("\n=== ESP32 SD Card Diagnostic ===");

  // Initialize SPI with explicit pins, then mount SD
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("[SD] Mount FAILED. Check wiring/power and CS pin.");
    return;
  }

  uint8_t type = SD.cardType();
  Serial.printf("[SD] Card type: %s\n", cardTypeName(type));
  if (type == CARD_NONE) {
    Serial.println("[SD] No card detected");
    return;
  }

  uint64_t sizeMB = SD.cardSize() / (1024ULL * 1024ULL);
  Serial.printf("[SD] Card size: %llu MB\n", sizeMB);

  listDir(SD, "/");

  if (writeTestFile()) {
    readBackTestFile();
  }

  // Short throughput check (~100 KiB)
  throughputTestKB(100);

  // Show directory again with new files present
  listDir(SD, "/");

  Serial.println("[SD] Test complete.");
}

void loop() {
  // Nothing to do in loop.
  delay(1000);
}
