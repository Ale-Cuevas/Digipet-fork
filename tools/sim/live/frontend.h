#pragma once

// What sim_hal.cpp needs from a window system. Two implementations:
// frontend_sdl.cpp (the interactive window) and frontend_headless.cpp
// (no window, for --script runs and machines without SDL2).

#include <cstddef>
#include <cstdint>

namespace sim {

enum class Key {
  None,
  Boot,      // Space / B -- the BOOT key (GPIO0)
  User,      // N -- the user key (GPIO18)
  UsbPower,  // P -- plug/unplug USB power
  FastForward,
  BatteryDown,
  BatteryUp,
  Screenshot,
  PowerCycle,
  Quit,
};

struct InputEvent {
  enum Type { MouseDown, MouseMove, MouseUp, KeyDown, KeyUp, Quit } type;
  int16_t x = 0, y = 0;  // panel coordinates
  Key key = Key::None;
};

namespace frontend {

bool init(int width, int height, int scale);
void shutdown();
// Returns false when no event is pending.
bool pollEvent(InputEvent &event);
// brightness 0..255 as sent to the panel; on == false shows black.
void present(const uint16_t *framebuffer, int width, int height, uint8_t brightness, bool on);
void setTitle(const char *title);
// Host audio: 16 kHz, 16-bit, stereo. hasAudio() false means samples are
// dropped and sim_hal.cpp paces writes itself.
bool hasAudio();
void queueAudio(const int16_t *samples, size_t bytes);
size_t queuedAudioBytes();
// Kept across the re-exec a deep-sleep wake does, so the window stays put.
bool windowPosition(int &x, int &y);

}  // namespace frontend
}  // namespace sim
