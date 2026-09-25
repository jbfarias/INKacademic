#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
constexpr int ESP_OK = 0;
struct esp_partition_t {
  uint32_t size;
  const char* label;
  uint32_t address;
};
esp_err_t esp_partition_read(const esp_partition_t*, size_t, void*, size_t);
esp_err_t esp_partition_write(const esp_partition_t*, size_t, const void*, size_t);
esp_err_t esp_partition_erase_range(const esp_partition_t*, size_t, size_t);
