#pragma once

// The real Arduino_GFX core and canvas, plus host versions of the two
// ESP32-only classes main.cpp constructs: the QSPI bus (inert) and the
// CO5300 panel, which is an Arduino_Canvas whose framebuffer the SDL
// window shows -- the same "display RAM" the AMOLED keeps between writes.

#include <Arduino_GFX.h>
#include <Arduino_DataBus.h>
#include <canvas/Arduino_Canvas.h>

class Arduino_ESP32QSPI : public Arduino_DataBus {
 public:
  Arduino_ESP32QSPI(int8_t, int8_t, int8_t, int8_t, int8_t, int8_t) {}
  bool begin(int32_t = 0, int8_t = 0) override { return true; }
  void beginWrite() override {}
  void endWrite() override {}
  void writeCommand(uint8_t) override {}
  void writeCommand16(uint16_t) override {}
  void writeCommandBytes(uint8_t *, uint32_t) override {}
  void write(uint8_t) override {}
  void write16(uint16_t) override {}
  void writeC8D8(uint8_t, uint8_t) override {}
  void writeRepeat(uint16_t, uint32_t) override {}
  void writeBytes(uint8_t *, uint32_t) override {}
  void writePixels(uint16_t *, uint32_t) override {}
};

class Arduino_CO5300 : public Arduino_Canvas {
 public:
  Arduino_CO5300(Arduino_DataBus *, int8_t, uint8_t, int16_t w, int16_t h, uint8_t, uint8_t,
                 uint8_t, uint8_t)
      : Arduino_Canvas(w, h, nullptr) {}
  bool begin(int32_t = GFX_NOT_DEFINED) override {
    if (!Arduino_Canvas::begin(GFX_SKIP_OUTPUT_BEGIN)) return false;
    memset(getFramebuffer(), 0, static_cast<size_t>(_width) * _height * 2);
    sim::attachPanel(getFramebuffer(), _width, _height);
    return true;
  }
  void flush(bool = false) override {}
  void displayOn() override { sim::setDisplayOn(true); }
  void displayOff() override { sim::setDisplayOn(false); }
  void setBrightness(uint8_t brightness) { sim::setBrightness(brightness); }
};
