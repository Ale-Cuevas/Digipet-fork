// SDL2 window: shows the panel framebuffer, turns the mouse into the touch
// controller and the keyboard into the board's keys, and plays I2S audio.

#include "frontend.h"

#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sim {
namespace frontend {
namespace {

SDL_Window *window = nullptr;
SDL_Renderer *renderer = nullptr;
SDL_Texture *texture = nullptr;
SDL_AudioDeviceID audioDevice = 0;
bool mouseDown = false;

Key keyFor(SDL_Keycode code) {
  switch (code) {
    case SDLK_SPACE:
    case SDLK_b: return Key::Boot;
    case SDLK_n: return Key::User;
    case SDLK_p: return Key::UsbPower;
    case SDLK_f: return Key::FastForward;
    case SDLK_MINUS: return Key::BatteryDown;
    case SDLK_EQUALS: return Key::BatteryUp;
    case SDLK_s: return Key::Screenshot;
    case SDLK_r: return Key::PowerCycle;
    case SDLK_ESCAPE: return Key::Quit;
    default: return Key::None;
  }
}

}  // namespace

bool init(int width, int height, int scale) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return false;
  }
  int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED;
  if (const char *saved = getenv("DIGIPET_SIM_WINPOS")) sscanf(saved, "%d,%d", &x, &y);
  window = SDL_CreateWindow("Digipet sim", x, y, width * scale, height * scale, 0);
  if (!window) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    return false;
  }
  renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!renderer) {
    fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    return false;
  }
  // Mouse events arrive in panel coordinates whatever the window scale.
  SDL_RenderSetLogicalSize(renderer, width, height);
  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                              width, height);

  SDL_AudioSpec want{}, have{};
  want.freq = 16000;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 512;
  audioDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (audioDevice) SDL_PauseAudioDevice(audioDevice, 0);
  else fprintf(stderr, "No audio device (%s); running silent\n", SDL_GetError());
  return texture != nullptr;
}

void shutdown() {
  if (audioDevice) SDL_CloseAudioDevice(audioDevice);
  SDL_Quit();
}

bool pollEvent(InputEvent &event) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT:
        event.type = InputEvent::Quit;
        return true;
      case SDL_MOUSEBUTTONDOWN:
        if (e.button.button != SDL_BUTTON_LEFT) break;
        mouseDown = true;
        event = {InputEvent::MouseDown, static_cast<int16_t>(e.button.x),
                 static_cast<int16_t>(e.button.y)};
        return true;
      case SDL_MOUSEMOTION:
        if (!mouseDown) break;
        event = {InputEvent::MouseMove, static_cast<int16_t>(e.motion.x),
                 static_cast<int16_t>(e.motion.y)};
        return true;
      case SDL_MOUSEBUTTONUP:
        if (e.button.button != SDL_BUTTON_LEFT) break;
        mouseDown = false;
        event = {InputEvent::MouseUp, static_cast<int16_t>(e.button.x),
                 static_cast<int16_t>(e.button.y)};
        return true;
      case SDL_KEYDOWN:
      case SDL_KEYUP: {
        if (e.key.repeat) break;
        const Key key = keyFor(e.key.keysym.sym);
        if (key == Key::None) break;
        event = {e.type == SDL_KEYDOWN ? InputEvent::KeyDown : InputEvent::KeyUp};
        event.key = key;
        return true;
      }
      default:
        break;
    }
  }
  return false;
}

void present(const uint16_t *framebuffer, int width, int height, uint8_t brightness, bool on) {
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);
  if (on && framebuffer && brightness) {
    SDL_UpdateTexture(texture, nullptr, framebuffer, width * 2);
    // Perceptual rather than linear, so mid brightness settings stay
    // readable on a monitor; 0 is fully dark like the panel.
    const uint8_t mod = static_cast<uint8_t>(255.0f * sqrtf(brightness / 255.0f));
    SDL_SetTextureColorMod(texture, mod, mod, mod);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
  }
  SDL_RenderPresent(renderer);
}

void setTitle(const char *title) { SDL_SetWindowTitle(window, title); }

bool hasAudio() { return audioDevice != 0; }

void queueAudio(const int16_t *samples, size_t bytes) {
  SDL_QueueAudio(audioDevice, samples, static_cast<Uint32>(bytes));
}

size_t queuedAudioBytes() { return audioDevice ? SDL_GetQueuedAudioSize(audioDevice) : 0; }

bool windowPosition(int &x, int &y) {
  if (!window) return false;
  SDL_GetWindowPosition(window, &x, &y);
  return true;
}

}  // namespace frontend
}  // namespace sim
