#pragma once
#include "esp_partition.h"
using wifi_ps_type_t = int;
constexpr int WIFI_PS_MIN_MODEM = 1, WIFI_PS_NONE = 0;
inline int esp_wifi_get_ps(wifi_ps_type_t* mode) {
  *mode = WIFI_PS_MIN_MODEM;
  return ESP_OK;
}
inline int esp_wifi_set_ps(wifi_ps_type_t) { return ESP_OK; }
