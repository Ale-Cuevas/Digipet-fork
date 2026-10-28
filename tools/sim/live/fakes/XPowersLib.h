#pragma once

// AXP2101: begin() only probes the bus; battery readings go through
// main.cpp's own axp2101Read() register reads, which the sim's I2C model
// answers (see sim_hal.cpp, PMU registers 0x01/0x34/0x35/0xA4).

#include "Wire.h"

#define XPOWERS_AXP2101_CHG_VOL_4V2 3

class XPowersPMU {
 public:
  bool begin(TwoWire &, uint8_t address, int, int) { return sim::i2cPresent(address); }
  bool setChargeTargetVoltage(uint8_t) { return true; }
  void enableBattDetection() {}
  void enableVbusVoltageMeasure() {}
  void enableBattVoltageMeasure() {}
  void enableSystemVoltageMeasure() {}
};
