#pragma once

// The one seam between the fake hardware headers in fakes/ and the SDL
// host in sim_hal.cpp. Fakes call into here; nothing in src/ includes it.
// See ../README.md ("Interactive simulator") for what is faked and how.

#include <cstddef>
#include <cstdint>
#include <string>

namespace sim {

// Emulated time. millis()/micros() run at `speed` x wall-clock so pet decay
// (agePet() once a minute) can be fast-forwarded; delay() waits for that
// same emulated time to pass.
uint32_t millis();
uint64_t micros();
void delayMs(uint32_t ms);

// Polls SDL input and presents the panel at ~60 Hz. Called from delay()
// and after every loop() iteration -- the firmware has blocking animations
// (boot, page slides) that only ever yield through delay().
void pump();

// Level of a GPIO the firmware reads. Only the two keys are wired up;
// everything else reads HIGH (idle).
int digitalRead(uint8_t pin);
void attachInterrupt(uint8_t pin, void (*isr)());

// Mouse-as-finger, in panel coordinates.
bool touchPoint(int16_t &x, int16_t &y);

// Panel: the fake CO5300 registers its framebuffer here.
void attachPanel(uint16_t *framebuffer, int16_t width, int16_t height);
void setBrightness(uint8_t level);
void setDisplayOn(bool on);

// I2C bus with the board's devices modelled at register level.
bool i2cPresent(uint8_t address);
bool i2cWrite(uint8_t address, const uint8_t *data, size_t length);
size_t i2cRead(uint8_t address, uint8_t *out, size_t length);

// 16 kHz / 16-bit / stereo PCM to the host speaker. Blocks while the queue
// is full, like the real I2S DMA does.
void audioWrite(const uint8_t *data, size_t length);

// NVS backing file (one flat map, keys are "namespace/key").
bool nvsGet(const std::string &key, std::string &value);
void nvsPut(const std::string &key, const std::string &value);
void nvsRemove(const std::string &key);
void nvsClearNamespace(const std::string &ns);

// Directory standing in for the SD card root.
std::string sdRoot();

// One line typed on the terminal running the sim, forwarded to the
// firmware's Serial (WHOAMI, SIMBATTLE ..., POWERLOG, ...).
bool serialLineAvailable();
std::string serialReadLine();

// Deep sleep: the real board reboots on wake, so the sim re-executes itself
// once a power key is pressed. RTC_DATA_ATTR memory is carried over.
bool resumedFromDeepSleep();
[[noreturn]] void deepSleep();
[[noreturn]] void restart();

}  // namespace sim
