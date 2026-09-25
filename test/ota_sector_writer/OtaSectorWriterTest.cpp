// Tests the production OTA writer, with SDK/clock calls replaced at the I/O
// boundary. Does not simulate physical flash timing or certify hardware.
#include <cassert>
#include <cstdio>
#include <vector>

#include "src/network/OtaSectorWriter.h"

static std::vector<unsigned char> written;
static unsigned feeds = 0, pauses = 0, operations = 0;
static unsigned lastFeeds = 0, lastPauses = 0;
static unsigned failOperation = 0;
static int beginResult = ESP_OK;

int esp_task_wdt_reset() {
  ++feeds;
  return ESP_OK;
}
void delay(unsigned long ms) {
  assert(ms >= 1);
  ++pauses;
}

static void checkService() {
  assert(feeds > lastFeeds && pauses > lastPauses);
  lastFeeds = feeds;
  lastPauses = pauses;
  ++operations;
}

esp_err_t esp_ota_begin(const esp_partition_t*, size_t size, esp_ota_handle_t* handle) {
  checkService();
  assert(size == OTA_WITH_SEQUENTIAL_WRITES);  // No up-front whole-image erase.
  *handle = 17;
  return beginResult;
}

esp_err_t esp_ota_write(esp_ota_handle_t handle, const void* bytes, size_t length) {
  checkService();
  assert(handle == 17);
  assert(length > 0 && length <= 4096 - written.size() % 4096);
  if (operations == failOperation) return -9;
  const auto* data = static_cast<const unsigned char*>(bytes);
  written.insert(written.end(), data, data + length);
  return ESP_OK;
}

static void reset() {
  written.clear();
  feeds = pauses = operations = lastFeeds = lastPauses = failOperation = 0;
  beginResult = ESP_OK;
}

int main() {
  std::vector<unsigned char> image(6 * 1024 * 1024 + 137);
  for (size_t i = 0; i < image.size(); ++i) image[i] = static_cast<unsigned char>(i * 13);
  for (size_t chunk : {size_t(1), size_t(14), size_t(4096), size_t(8192), size_t(65536)}) {
    reset();
    esp_ota_handle_t handle = 0;
    esp_partition_t partition;
    assert(ota_sector::begin(&partition, &handle) == ESP_OK);
    size_t offset = 0;
    assert(ota_sector::write(handle, image.data(), 14, offset) == ESP_OK);
    while (offset < image.size()) {
      const size_t count = std::min(chunk, image.size() - offset);
      assert(ota_sector::write(handle, image.data() + offset, count, offset) == ESP_OK);
    }
    assert(offset == image.size() && written == image);
    assert(feeds > lastFeeds && pauses > lastPauses);
  }
  // Failure at the first, second, or a later sector stops immediately, with
  // accurate progress for the successfully written prefix and watchdog service.
  for (unsigned failure : {1U, 2U, 10U}) {
    reset();
    failOperation = failure;
    size_t offset = 0;
    assert(ota_sector::write(17, image.data(), 65536, offset) == -9);
    assert(operations == failure && offset == written.size());
    assert(offset == (failure - 1) * 4096);
    assert(feeds > lastFeeds && pauses > lastPauses);
  }
  reset();
  beginResult = -5;
  esp_ota_handle_t handle = 0;
  assert(ota_sector::begin(nullptr, &handle) == -5);
  assert(written.empty() && feeds > lastFeeds && pauses > lastPauses);
  size_t offset = 0;
  assert(ota_sector::write(17, nullptr, 0, offset) == ESP_OK);
  assert(offset == 0 && written.empty());
  std::puts("PASS: 10 OTA sector writer scenarios (host I/O fakes; hardware validation pending)");
}
