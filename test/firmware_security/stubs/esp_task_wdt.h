#pragma once
#include <cstdint>
struct esp_task_wdt_config_t {
  uint32_t timeout_ms;
  uint32_t idle_core_mask;
  bool trigger_panic;
};
inline int esp_task_wdt_reconfigure(const esp_task_wdt_config_t*) { return 0; }
void esp_task_wdt_reset();
