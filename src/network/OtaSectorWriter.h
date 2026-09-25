#pragma once

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <spi_flash_mmap.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ota_sector {
inline void serviceWatchdog() {
  esp_task_wdt_reset();
  // yield() alone may immediately schedule this task again. Block briefly so
  // the idle task can run as well as feeding the calling task's watchdog.
  delay(1);
}

inline esp_err_t begin(const esp_partition_t* partition, esp_ota_handle_t* handle) {
  serviceWatchdog();
  // A concrete image size (or OTA_SIZE_UNKNOWN) erases the whole image here.
  // Sequential mode defers erases until write(), where we bound each sector.
  const esp_err_t result = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, handle);
  serviceWatchdog();
  return result;
}

inline esp_err_t write(esp_ota_handle_t handle, const uint8_t* data, size_t length, size_t& offset) {
  while (length > 0) {
    // Network chunks, including the initial 14-byte chip header, need not be
    // aligned. Never cross a sector boundary in one SDK erase/write call.
    const size_t count = std::min(length, SPI_FLASH_SEC_SIZE - offset % SPI_FLASH_SEC_SIZE);
    serviceWatchdog();
    const esp_err_t result = esp_ota_write(handle, data, count);
    serviceWatchdog();
    if (result != ESP_OK) return result;
    offset += count;
    data += count;
    length -= count;
  }
  return ESP_OK;
}
}  // namespace ota_sector
