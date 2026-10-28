#pragma once

// Wi-Fi radio with a fixed set of fake access points for FEED's scan.
// There's no network to join: begin() never connects, so the boot-time
// NTP sync (only attempted with an SD wifi.ini) reports LINK OFFLINE.

#include <Arduino.h>

typedef enum { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 } wifi_mode_t;
typedef enum { WL_IDLE_STATUS = 0, WL_CONNECTED = 3, WL_DISCONNECTED = 6 } wl_status_t;

class WiFiClass {
 public:
  bool mode(wifi_mode_t mode) {
    mode_ = mode;
    return true;
  }
  wifi_mode_t getMode() { return mode_; }
  wl_status_t begin(const char *, const char * = nullptr) { return WL_DISCONNECTED; }
  wl_status_t status() { return WL_DISCONNECTED; }
  bool disconnect(bool = false, bool = false) { return true; }

  // A random subset of the fake APs, after roughly a real scan's duration.
  int16_t scanNetworks(bool = false, bool = false, bool = false, uint32_t = 300) {
    delay(1800);
    count_ = 0;
    for (uint8_t i = 0; i < kApCount; ++i) {
      if (esp_random() % 100 < 70) visible_[count_++] = i;
    }
    return count_;
  }
  void scanDelete() { count_ = 0; }
  String SSID(uint8_t i) { return i < count_ ? String(kSsids[visible_[i]]) : String(); }
  const uint8_t *BSSID(uint8_t i) {
    if (i >= count_) return nullptr;
    static uint8_t bssid[6];
    const uint8_t base[6] = {0x02, 0x5E, 0x1A, 0x00, 0x00, 0x00};
    memcpy(bssid, base, sizeof(bssid));
    bssid[5] = visible_[i];
    return bssid;
  }

 private:
  static constexpr uint8_t kApCount = 8;
  static constexpr const char *kSsids[kApCount] = {
      "HomeNet-5G", "CoffeeBar_Guest", "Pixel_7731", "NETGEAR42",
      "eduroam",    "SimCity Free",    "DIRECT-printer", "TrainWiFi"};
  wifi_mode_t mode_ = WIFI_OFF;
  uint8_t visible_[kApCount]{};
  uint8_t count_ = 0;
};
inline WiFiClass WiFi;
