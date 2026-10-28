// No window, no audio: the firmware still runs in full, driven by a
// --script (taps, swipes, keys, screenshots). Used for automated UI checks
// and on machines without SDL2.

#include "frontend.h"

namespace sim {
namespace frontend {

bool init(int, int, int) { return true; }
void shutdown() {}
bool pollEvent(InputEvent &) { return false; }
void present(const uint16_t *, int, int, uint8_t, bool) {}
void setTitle(const char *) {}
bool hasAudio() { return false; }
void queueAudio(const int16_t *, size_t) {}
size_t queuedAudioBytes() { return 0; }
bool windowPosition(int &, int &) { return false; }

}  // namespace frontend
}  // namespace sim
