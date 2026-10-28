#pragma once

// ESP-IDF system calls the firmware uses, answered by the sim host.

#include <cstdint>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1

typedef enum {
  ESP_RST_UNKNOWN,
  ESP_RST_POWERON,
  ESP_RST_EXT,
  ESP_RST_SW,
  ESP_RST_PANIC,
  ESP_RST_INT_WDT,
  ESP_RST_TASK_WDT,
  ESP_RST_WDT,
  ESP_RST_DEEPSLEEP,
} esp_reset_reason_t;

uint32_t esp_random();
esp_reset_reason_t esp_reset_reason();
[[noreturn]] void esp_restart();
