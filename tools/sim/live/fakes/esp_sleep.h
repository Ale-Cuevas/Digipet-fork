#pragma once

#include <cstdint>

#include "esp_system.h"
#include "../sim_hal.h"

typedef enum {
  ESP_SLEEP_WAKEUP_UNDEFINED,
  ESP_SLEEP_WAKEUP_ALL,
  ESP_SLEEP_WAKEUP_EXT0,
  ESP_SLEEP_WAKEUP_EXT1,
  ESP_SLEEP_WAKEUP_TIMER,
} esp_sleep_wakeup_cause_t;
typedef enum { ESP_PD_DOMAIN_RTC_PERIPH } esp_sleep_pd_domain_t;
typedef enum { ESP_PD_OPTION_OFF, ESP_PD_OPTION_ON, ESP_PD_OPTION_AUTO } esp_sleep_pd_option_t;
typedef enum { ESP_EXT1_WAKEUP_ANY_LOW, ESP_EXT1_WAKEUP_ANY_HIGH } esp_sleep_ext1_wakeup_mode_t;

inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause() {
  return sim::resumedFromDeepSleep() ? ESP_SLEEP_WAKEUP_EXT1 : ESP_SLEEP_WAKEUP_UNDEFINED;
}
inline uint64_t esp_sleep_get_ext1_wakeup_status() { return 0; }
inline esp_err_t esp_sleep_pd_config(esp_sleep_pd_domain_t, esp_sleep_pd_option_t) { return ESP_OK; }
inline esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t, esp_sleep_ext1_wakeup_mode_t) {
  return ESP_OK;
}
[[noreturn]] inline void esp_deep_sleep_start() { sim::deepSleep(); }
