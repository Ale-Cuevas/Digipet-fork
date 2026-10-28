#pragma once

// CST92xx touch controller: one finger, fed from the mouse (sim::touchPoint).
// Coordinates come out already in panel space, i.e. after the swap/mirror
// the real driver applies from setSwapXY()/setMirrorXY().

#include "Wire.h"

class TouchDrvCST92xx {
 public:
  void setPins(int, int) {}
  bool begin(TwoWire &, uint8_t address, int, int) { return sim::i2cPresent(address); }
  void setMaxCoordinates(uint16_t, uint16_t) {}
  void setSwapXY(bool) {}
  void setMirrorXY(bool, bool) {}
  const char *getModelName() { return "CST9220 (sim)"; }
  uint8_t getPoint(int16_t *x, int16_t *y, uint8_t maxPoints) {
    if (maxPoints == 0 || !sim::touchPoint(x[0], y[0])) return 0;
    return 1;
  }
};
