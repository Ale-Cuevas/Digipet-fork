#pragma once

// I2S output to the ES8311 speaker codec, played on the host speaker.

#include <Arduino.h>

typedef enum { I2S_MODE_STD } i2s_mode_t;
typedef enum { I2S_DATA_BIT_WIDTH_16BIT = 16 } i2s_data_bit_width_t;
typedef enum { I2S_SLOT_MODE_MONO = 1, I2S_SLOT_MODE_STEREO = 2 } i2s_slot_mode_t;
typedef enum { I2S_STD_SLOT_LEFT = 1, I2S_STD_SLOT_RIGHT = 2, I2S_STD_SLOT_BOTH = 3 } i2s_std_slot_mask_t;

class I2SClass {
 public:
  void setPins(int8_t, int8_t, int8_t, int8_t = -1, int8_t = -1) {}
  bool begin(i2s_mode_t, uint32_t, i2s_data_bit_width_t, i2s_slot_mode_t, int8_t = -1) {
    return true;
  }
  size_t write(const uint8_t *data, size_t length) {
    sim::audioWrite(data, length);
    return length;
  }
  size_t write(uint8_t *data, size_t length) {
    return write(const_cast<const uint8_t *>(data), length);
  }
};
