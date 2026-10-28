#pragma once

#include <Arduino.h>

#define FSPI 0
#define HSPI 1

class SPIClass {
 public:
  explicit SPIClass(uint8_t = FSPI) {}
  void begin(int8_t = -1, int8_t = -1, int8_t = -1, int8_t = -1) {}
  void end() {}
};
