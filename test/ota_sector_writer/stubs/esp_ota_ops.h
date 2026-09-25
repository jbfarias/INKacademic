#pragma once
#include <cstddef>
using esp_err_t = int;
using esp_ota_handle_t = unsigned;
struct esp_partition_t {};
constexpr esp_err_t ESP_OK = 0;
constexpr size_t OTA_WITH_SEQUENTIAL_WRITES = 0xfffffffe;
esp_err_t esp_ota_begin(const esp_partition_t*, size_t, esp_ota_handle_t*);
esp_err_t esp_ota_write(esp_ota_handle_t, const void*, size_t);
