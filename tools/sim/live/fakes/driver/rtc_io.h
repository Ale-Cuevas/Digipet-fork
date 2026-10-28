#pragma once

#include "esp_system.h"

typedef int gpio_num_t;
typedef enum { RTC_GPIO_MODE_INPUT_ONLY, RTC_GPIO_MODE_OUTPUT_ONLY } rtc_gpio_mode_t;

inline esp_err_t rtc_gpio_init(gpio_num_t) { return ESP_OK; }
inline esp_err_t rtc_gpio_set_direction(gpio_num_t, rtc_gpio_mode_t) { return ESP_OK; }
inline esp_err_t rtc_gpio_pullup_en(gpio_num_t) { return ESP_OK; }
inline esp_err_t rtc_gpio_pulldown_dis(gpio_num_t) { return ESP_OK; }
