#pragma once

#include <cstdint>
#include <cstring>

#include "esp_system.h"

typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT, ESP_MAC_ETH } esp_mac_type_t;

// Fixed fake MAC so the player ID's device suffix is stable between runs.
inline esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t) {
  static const uint8_t kMac[6] = {0x02, 0x51, 0x4D, 0xD1, 0x9E, 0x70};
  memcpy(mac, kMac, sizeof(kMac));
  return ESP_OK;
}
