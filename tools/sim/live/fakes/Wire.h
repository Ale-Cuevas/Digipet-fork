#pragma once

// Byte-level I2C master in front of the sim's register models.

#include <Arduino.h>

class TwoWire {
 public:
  bool begin(int = -1, int = -1, uint32_t = 0) { return true; }
  void beginTransmission(uint8_t address) {
    address_ = address;
    txLength_ = 0;
  }
  size_t write(uint8_t value) {
    if (txLength_ < sizeof(tx_)) tx_[txLength_++] = value;
    return 1;
  }
  size_t write(const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; ++i) write(data[i]);
    return length;
  }
  // 0 = ACK, 2 = address NACK (nothing there), as in the real core.
  uint8_t endTransmission(bool = true) {
    return sim::i2cWrite(address_, tx_, txLength_) ? 0 : 2;
  }
  uint8_t requestFrom(uint8_t address, uint8_t length) {
    rxLength_ = sim::i2cRead(address, rx_, std::min<size_t>(length, sizeof(rx_)));
    rxIndex_ = 0;
    return static_cast<uint8_t>(rxLength_);
  }
  uint8_t requestFrom(int address, int length) {
    return requestFrom(static_cast<uint8_t>(address), static_cast<uint8_t>(length));
  }
  int available() { return static_cast<int>(rxLength_ - rxIndex_); }
  int read() { return rxIndex_ < rxLength_ ? rx_[rxIndex_++] : -1; }

 private:
  uint8_t address_ = 0;
  uint8_t tx_[64]{};
  size_t txLength_ = 0;
  uint8_t rx_[64]{};
  size_t rxLength_ = 0;
  size_t rxIndex_ = 0;
};
inline TwoWire Wire;
