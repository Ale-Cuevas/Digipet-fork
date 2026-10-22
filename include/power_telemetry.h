#pragma once
#include <stdint.h>

// Stable CSV event codes. No identifiers, nearby-device data, or credentials.
enum class PowerEvent : uint8_t {
  Boot, Wake, Sample, Sleep, PowerConnected, PowerRemoved,
  WifiScanStart, WifiScanEnd, BleScanStart, BleScanEnd, State
};
struct PowerSnapshot {
  uint32_t epoch; // 0 when the wall clock is invalid
  uint16_t millivolts;
  uint8_t percent, brightness, page, wifi, battle, flags;
  // flags: bit 0 external power, bit 1 charging, bit 2 screen off,
  // bit 3 valid battery reading
};
void powerTelemetryBegin(bool deepResume);
void powerTelemetryRecord(PowerEvent event, const PowerSnapshot &snapshot);
void powerTelemetryFlush();
void powerTelemetryDump();
// Captures current firmware state; callable around blocking scans.
void recordPowerEvent(PowerEvent event);
