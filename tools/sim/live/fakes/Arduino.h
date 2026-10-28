#pragma once

// Arduino core stand-in for the interactive simulator. Unlike
// ../../fakes/Arduino.h (static render harness), time here is real:
// millis()/delay() follow the sim clock and delay() keeps the window alive.
// String/Print/PROGMEM are shared with the render harness's fakes.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <thread>

#include "pgmspace.h"
#include "WString.h"
#include "Print.h"
#include "Printable.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp32-hal-psram.h"
#include "../sim_hal.h"

using std::abs;
using std::max;
using std::min;

#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))

#define LOW 0x0
#define HIGH 0x1
#define INPUT 0x01
#define OUTPUT 0x03
#define INPUT_PULLUP 0x05
#define FALLING 0x02
#define RISING 0x01
#define CHANGE 0x03

inline uint32_t millis() { return sim::millis(); }
inline unsigned long micros() { return static_cast<unsigned long>(sim::micros()); }
inline void delay(uint32_t ms) { sim::delayMs(ms); }
inline void yield() {}

inline void pinMode(uint8_t, uint8_t) {}
inline void digitalWrite(uint8_t, uint8_t) {}
inline int digitalRead(uint8_t pin) { return sim::digitalRead(pin); }
inline uint8_t digitalPinToInterrupt(uint8_t pin) { return pin; }
inline void attachInterrupt(uint8_t pin, void (*isr)(), int) { sim::attachInterrupt(pin, isr); }

inline long random(long howBig) { return howBig > 0 ? static_cast<long>(esp_random() % howBig) : 0; }
inline long random(long howSmall, long howBig) {
  return howSmall >= howBig ? howSmall : howSmall + random(howBig - howSmall);
}
inline void randomSeed(unsigned long) {}

// Network time: there's no network, so NTP never answers. The RTC model
// already keeps host local time.
inline void configTzTime(const char *, const char *, const char * = nullptr,
                         const char * = nullptr) {}
inline bool getLocalTime(struct tm *, uint32_t ms = 5000) {
  delay(ms);
  return false;
}

class HardwareSerial : public Print {
 public:
  void begin(unsigned long) {}
  size_t write(uint8_t c) override { return fwrite(&c, 1, 1, stdout); }
  size_t write(const uint8_t *buffer, size_t size) override {
    return fwrite(buffer, 1, size, stdout);
  }
  using Print::write;
  void flush() override { fflush(stdout); }
  int available() { return sim::serialLineAvailable() ? 1 : 0; }
  String readStringUntil(char) { return String(sim::serialReadLine().c_str()); }
};
inline HardwareSerial Serial;

class EspClass {
 public:
  uint32_t getPsramSize() { return 8u * 1024 * 1024; }
  uint64_t getEfuseMac() { return 0x0000A1B2C3D4E5F6ull; }
  [[noreturn]] void restart() { sim::restart(); }
};
inline EspClass ESP;

// FreeRTOS: the firmware only spawns one fire-and-forget task (the boot
// audio clip), which a detached host thread covers. A task ends itself with
// vTaskDelete(nullptr); that unwinds back to the thread's entry here.
typedef void *TaskHandle_t;
typedef int BaseType_t;
#define pdPASS 1
namespace sim {
struct TaskExit {};
}  // namespace sim
inline BaseType_t xTaskCreatePinnedToCore(void (*task)(void *), const char *, uint32_t,
                                          void *arg, unsigned, TaskHandle_t *handle, int) {
  std::thread([task, arg] {
    try {
      task(arg);
    } catch (const sim::TaskExit &) {
    }
  }).detach();
  if (handle) *handle = nullptr;
  return pdPASS;
}
inline void vTaskDelete(TaskHandle_t handle) {
  if (handle == nullptr) throw sim::TaskExit{};
}
