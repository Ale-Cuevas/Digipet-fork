// Host side of the interactive simulator: runs the firmware's real setup()
// and loop() (src/main.cpp + the vpet game module) against modelled board
// hardware. See ../README.md ("Interactive simulator") for usage.

#include "sim_hal.h"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "board_amoled_216.h"
#include "esp_system.h"
#include "frontend.h"

void setup();
void loop();

// RTC slow memory: every RTC_DATA_ATTR variable lands in this section (see
// fakes/esp_attr.h). Weak so a build with none still links.
extern "C" char __start_rtc_data[] __attribute__((weak));
extern "C" char __stop_rtc_data[] __attribute__((weak));

namespace sim {
namespace {

using Clock = std::chrono::steady_clock;

// --- Options -----------------------------------------------------------------

struct Options {
  std::string dataDir = ".sim";
  std::string scriptPath;
  int scale = 1;
  double speed = 1.0;
  double fastForward = 60.0;
  bool usbPower = false;
  bool fresh = false;
};
Options options;
std::vector<std::string> originalArgs;

// --- Emulated clock ------------------------------------------------------------

Clock::time_point clockAnchorReal = Clock::now();
uint64_t clockAnchorMicros = 0;
std::atomic<double> clockSpeed{1.0};
std::mutex clockMutex;

void setSpeed(double speed) {
  std::lock_guard<std::mutex> lock(clockMutex);
  const auto now = Clock::now();
  clockAnchorMicros += static_cast<uint64_t>(
      std::chrono::duration<double, std::micro>(now - clockAnchorReal).count() * clockSpeed);
  clockAnchorReal = now;
  clockSpeed = speed;
}

std::thread::id mainThread;
bool onMainThread() { return std::this_thread::get_id() == mainThread; }

// --- Board state -------------------------------------------------------------

uint16_t *panelFramebuffer = nullptr;
int16_t panelWidth = 0, panelHeight = 0;
uint8_t panelBrightness = 0;
bool panelOn = true;

bool bootKeyHeld = false;
bool userKeyHeld = false;
bool touchHeld = false;
int16_t touchX = 0, touchY = 0;
void (*touchIsr)() = nullptr;

uint8_t batteryPercent = 82;
bool usbPower = false;
bool resumed = false;
esp_reset_reason_t resetReason = ESP_RST_POWERON;

Clock::time_point lastPresent;
Clock::time_point lastTitle;

void setTouch(bool held, int16_t x, int16_t y) {
  touchHeld = held;
  touchX = std::max<int16_t>(0, std::min<int16_t>(x, panelWidth ? panelWidth - 1 : x));
  touchY = std::max<int16_t>(0, std::min<int16_t>(y, panelHeight ? panelHeight - 1 : y));
  if (touchIsr) touchIsr();  // the CST92xx pulls INT low on press, move and release
}

// --- I2C devices -------------------------------------------------------------

struct I2cDevice {
  uint8_t pointer = 0;
  uint8_t registers[256]{};
};
std::map<uint8_t, I2cDevice> i2cDevices;
time_t rtcOffsetSeconds = 0;  // RTC time minus host time, set by rtcWrite()

uint8_t toBcd(int value) { return static_cast<uint8_t>(((value / 10) << 4) | (value % 10)); }
int fromBcd(uint8_t value) { return (value >> 4) * 10 + (value & 0x0F); }

uint16_t batteryMillivolts() { return static_cast<uint16_t>(3300 + batteryPercent * 9); }

// PCF85063 time registers 0x04..0x0A and the AXP2101 status/fuel-gauge
// registers main.cpp reads; everything else is plain storage.
uint8_t readRegister(uint8_t address, uint8_t reg) {
  if (address == BOARD_I2C_ADDR_RTC && reg >= 0x04 && reg <= 0x0A) {
    const time_t now = time(nullptr) + rtcOffsetSeconds;
    struct tm local{};
    localtime_r(&now, &local);
    switch (reg) {
      case 0x04: return toBcd(local.tm_sec);  // bit 7 clear: oscillator ran
      case 0x05: return toBcd(local.tm_min);
      case 0x06: return toBcd(local.tm_hour);
      case 0x07: return toBcd(local.tm_mday);
      case 0x08: return toBcd(local.tm_wday);
      case 0x09: return toBcd(local.tm_mon + 1);
      default: return toBcd(local.tm_year % 100);
    }
  }
  if (address == BOARD_I2C_ADDR_PMIC) {
    switch (reg) {
      // Charger state: 2 = constant-current charge, 4 = done, 5 = not charging.
      case 0x01: return usbPower ? (batteryPercent < 100 ? 0x02 : 0x04) : 0x05;
      case 0x34: return (batteryMillivolts() >> 8) & 0x3F;
      case 0x35: return batteryMillivolts() & 0xFF;
      case 0xA4: return batteryPercent;
      default: break;
    }
  }
  return i2cDevices[address].registers[reg];
}

void writeRtc(const uint8_t *data, size_t length) {
  if (length < 8 || data[0] != 0x04) return;
  struct tm value{};
  value.tm_sec = fromBcd(data[1] & 0x7F);
  value.tm_min = fromBcd(data[2] & 0x7F);
  value.tm_hour = fromBcd(data[3] & 0x3F);
  value.tm_mday = fromBcd(data[4] & 0x3F);
  value.tm_mon = fromBcd(data[6] & 0x1F) - 1;
  value.tm_year = fromBcd(data[7]) + 100;
  value.tm_isdst = -1;
  rtcOffsetSeconds = mktime(&value) - time(nullptr);
}

// --- Audio -------------------------------------------------------------------

constexpr size_t kAudioBytesPerMs = 16000 * 4 / 1000;
constexpr size_t kAudioQueueLimit = kAudioBytesPerMs * 120;  // ~I2S DMA depth

float codecGain() {
  // ES8311 DAC volume (reg 0x32): 0.5 dB per step, 0xBF = 0 dB, 0 = mute.
  const uint8_t volume = i2cDevices[BOARD_I2C_ADDR_ES8311].registers[0x32];
  if (volume == 0) return 0.0f;
  return powf(10.0f, (static_cast<int>(volume) - 0xBF) * 0.5f / 20.0f);
}

// --- NVS ---------------------------------------------------------------------

std::map<std::string, std::string> nvs;
std::mutex nvsMutex;

std::string nvsPath() { return options.dataDir + "/nvs.bin"; }

void nvsLoad() {
  std::ifstream in(nvsPath(), std::ios::binary);
  uint32_t keyLength = 0, valueLength = 0;
  while (in.read(reinterpret_cast<char *>(&keyLength), sizeof(keyLength))) {
    std::string key(keyLength, '\0');
    in.read(key.data(), keyLength);
    in.read(reinterpret_cast<char *>(&valueLength), sizeof(valueLength));
    std::string value(valueLength, '\0');
    in.read(value.data(), valueLength);
    if (!in) break;
    nvs[key] = value;
  }
}

void nvsSave() {
  const std::string temporary = nvsPath() + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    for (const auto &[key, value] : nvs) {
      const uint32_t keyLength = key.size(), valueLength = value.size();
      out.write(reinterpret_cast<const char *>(&keyLength), sizeof(keyLength));
      out.write(key.data(), keyLength);
      out.write(reinterpret_cast<const char *>(&valueLength), sizeof(valueLength));
      out.write(value.data(), valueLength);
    }
  }
  rename(temporary.c_str(), nvsPath().c_str());
}

// --- Serial input ------------------------------------------------------------

std::deque<std::string> serialLines;
std::mutex serialMutex;

void startSerialReader() {
  std::thread([] {
    std::string line;
    while (std::getline(std::cin, line)) {
      std::lock_guard<std::mutex> lock(serialMutex);
      serialLines.push_back(line);
    }
  }).detach();
}

// --- Screenshots -------------------------------------------------------------

bool writeBmp(const std::string &path) {
  if (!panelFramebuffer) return false;
  FILE *file = fopen(path.c_str(), "wb");
  if (!file) return false;
  const uint32_t rowBytes = (panelWidth * 3 + 3) & ~3u;
  const uint32_t imageBytes = rowBytes * panelHeight;
  uint8_t header[54]{'B', 'M'};
  auto put32 = [&](int offset, uint32_t value) { memcpy(header + offset, &value, 4); };
  put32(2, 54 + imageBytes);
  put32(10, 54);
  put32(14, 40);
  put32(18, panelWidth);
  put32(22, panelHeight);
  header[26] = 1;
  header[28] = 24;
  put32(34, imageBytes);
  fwrite(header, 1, sizeof(header), file);
  std::vector<uint8_t> row(rowBytes, 0);
  for (int y = panelHeight - 1; y >= 0; --y) {
    for (int x = 0; x < panelWidth; ++x) {
      const uint16_t pixel = panelFramebuffer[y * panelWidth + x];
      row[x * 3 + 0] = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);
      row[x * 3 + 1] = static_cast<uint8_t>(((pixel >> 5) & 0x3F) * 255 / 63);
      row[x * 3 + 2] = static_cast<uint8_t>((pixel >> 11) * 255 / 31);
    }
    fwrite(row.data(), 1, rowBytes, file);
  }
  fclose(file);
  return true;
}

void screenshot() {
  static int counter = 0;
  char path[512];
  snprintf(path, sizeof(path), "%s/screenshot-%ld-%d.bmp", options.dataDir.c_str(),
           static_cast<long>(time(nullptr)), counter++);
  if (writeBmp(path)) printf("[sim] screenshot: %s\n", path);
}

// --- Script ------------------------------------------------------------------

// A --script file is a list of timed inputs, one per line, in emulated
// milliseconds (so it behaves the same at any --speed):
//   wait MS | tap X Y | hold X Y MS | swipe X0 Y0 X1 Y1 | key boot|user
//   usb on|off | battery PCT | speed N | shot FILE.bmp | quit
// A script keeps running through deep sleep: its next `key` wakes the board
// and the steps after it run in the re-executed (woken) sim.
struct ScriptStep {
  enum Type { Wait, Down, Move, Up, KeyDown, KeyUp, Usb, Battery, Speed, Shot, Quit } type;
  int16_t x = 0, y = 0;
  uint32_t value = 0;
  std::string text;
};
std::vector<ScriptStep> script;
size_t scriptIndex = 0;
uint32_t scriptWaitUntil = 0;

bool loadScript(const std::string &path) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  int lineNumber = 0;
  while (std::getline(in, line)) {
    ++lineNumber;
    std::istringstream words(line);
    std::string command;
    if (!(words >> command) || command[0] == '#') continue;
    int a = 0, b = 0, c = 0, d = 0;
    std::string text;
    if (command == "wait" && words >> a) {
      script.push_back({ScriptStep::Wait, 0, 0, static_cast<uint32_t>(a)});
    } else if (command == "tap" && words >> a >> b) {
      script.push_back({ScriptStep::Down, static_cast<int16_t>(a), static_cast<int16_t>(b)});
      script.push_back({ScriptStep::Wait, 0, 0, 80});
      script.push_back({ScriptStep::Up});
      script.push_back({ScriptStep::Wait, 0, 0, 60});
    } else if (command == "hold" && words >> a >> b >> c) {
      script.push_back({ScriptStep::Down, static_cast<int16_t>(a), static_cast<int16_t>(b)});
      script.push_back({ScriptStep::Wait, 0, 0, static_cast<uint32_t>(c)});
      script.push_back({ScriptStep::Up});
      script.push_back({ScriptStep::Wait, 0, 0, 60});
    } else if (command == "swipe" && words >> a >> b >> c >> d) {
      constexpr int kSteps = 10;
      script.push_back({ScriptStep::Down, static_cast<int16_t>(a), static_cast<int16_t>(b)});
      for (int i = 1; i <= kSteps; ++i) {
        script.push_back({ScriptStep::Wait, 0, 0, 30});
        script.push_back({ScriptStep::Move, static_cast<int16_t>(a + (c - a) * i / kSteps),
                          static_cast<int16_t>(b + (d - b) * i / kSteps)});
      }
      script.push_back({ScriptStep::Wait, 0, 0, 40});
      script.push_back({ScriptStep::Up});
      script.push_back({ScriptStep::Wait, 0, 0, 60});
    } else if (command == "key" && words >> text && (text == "boot" || text == "user")) {
      const uint32_t which = text == "boot" ? BOARD_BTN_BOOT : BOARD_BTN_USER;
      script.push_back({ScriptStep::KeyDown, 0, 0, which});
      script.push_back({ScriptStep::Wait, 0, 0, 120});
      script.push_back({ScriptStep::KeyUp, 0, 0, which});
      script.push_back({ScriptStep::Wait, 0, 0, 60});
    } else if (command == "usb" && words >> text) {
      script.push_back({ScriptStep::Usb, 0, 0, text == "on" ? 1u : 0u});
    } else if (command == "battery" && words >> a) {
      script.push_back({ScriptStep::Battery, 0, 0, static_cast<uint32_t>(a)});
    } else if (command == "speed" && words >> a) {
      script.push_back({ScriptStep::Speed, 0, 0, static_cast<uint32_t>(a)});
    } else if (command == "shot" && words >> text) {
      script.push_back({ScriptStep::Shot, 0, 0, 0, text});
    } else if (command == "quit") {
      script.push_back({ScriptStep::Quit});
    } else {
      fprintf(stderr, "%s:%d: can't parse \"%s\"\n", path.c_str(), lineNumber, line.c_str());
      return false;
    }
  }
  return true;
}

void runScript() {
  while (scriptIndex < script.size() && millis() >= scriptWaitUntil) {
    const ScriptStep &step = script[scriptIndex++];
    switch (step.type) {
      case ScriptStep::Wait: scriptWaitUntil = millis() + step.value; break;
      case ScriptStep::Down:
      case ScriptStep::Move: setTouch(true, step.x, step.y); break;
      case ScriptStep::Up: setTouch(false, touchX, touchY); break;
      case ScriptStep::KeyDown:
      case ScriptStep::KeyUp:
        (step.value == BOARD_BTN_BOOT ? bootKeyHeld : userKeyHeld) =
            step.type == ScriptStep::KeyDown;
        break;
      case ScriptStep::Usb: usbPower = step.value != 0; break;
      case ScriptStep::Battery: batteryPercent = std::min<uint32_t>(step.value, 100); break;
      case ScriptStep::Speed: setSpeed(std::max<uint32_t>(step.value, 1)); break;
      case ScriptStep::Shot:
        if (writeBmp(step.text)) printf("[sim] screenshot: %s\n", step.text.c_str());
        else fprintf(stderr, "[sim] couldn't write %s\n", step.text.c_str());
        break;
      case ScriptStep::Quit:
        frontend::shutdown();
        exit(0);
    }
  }
}

// --- Reboot by re-exec -----------------------------------------------------

std::string rtcMemoryPath() { return options.dataDir + "/rtc_mem.bin"; }

// keepRtcMemory: deep-sleep wake and software reset keep RTC slow memory;
// a power cycle doesn't.
[[noreturn]] void reexec(const char *reason, bool keepRtcMemory) {
  if (keepRtcMemory && __start_rtc_data && __stop_rtc_data) {
    std::ofstream out(rtcMemoryPath(), std::ios::binary | std::ios::trunc);
    out.write(__start_rtc_data, __stop_rtc_data - __start_rtc_data);
  } else {
    remove(rtcMemoryPath().c_str());
  }
  int x, y;
  if (frontend::windowPosition(x, y)) {
    char position[32];
    snprintf(position, sizeof(position), "%d,%d", x, y);
    setenv("DIGIPET_SIM_WINPOS", position, 1);
  }
  setenv("DIGIPET_SIM_RESET", reason, 1);
  char speed[32];
  snprintf(speed, sizeof(speed), "%g", clockSpeed.load());
  setenv("DIGIPET_SIM_SPEED", speed, 1);
  setenv("DIGIPET_SIM_USB", usbPower ? "1" : "0", 1);
  setenv("DIGIPET_SIM_BATTERY", std::to_string(batteryPercent).c_str(), 1);
  // A script picks up where it left off (e.g. right after its wake key).
  setenv("DIGIPET_SIM_SCRIPT_STEP", std::to_string(scriptIndex).c_str(), 1);
  fflush(stdout);
  frontend::shutdown();
  std::vector<char *> argv;
  for (std::string &arg : originalArgs) argv.push_back(arg.data());
  argv.push_back(nullptr);
  execv("/proc/self/exe", argv.data());
  perror("execv");
  exit(1);
}

void restoreAfterReexec() {
  const char *reason = getenv("DIGIPET_SIM_RESET");
  if (!reason) return;
  const std::string why = reason;
  resetReason = why == "deepsleep" ? ESP_RST_DEEPSLEEP :
                why == "sw" ? ESP_RST_SW : ESP_RST_POWERON;
  resumed = resetReason == ESP_RST_DEEPSLEEP;
  if (const char *speed = getenv("DIGIPET_SIM_SPEED")) clockSpeed = atof(speed);
  if (const char *usb = getenv("DIGIPET_SIM_USB")) usbPower = atoi(usb) != 0;
  if (const char *battery = getenv("DIGIPET_SIM_BATTERY")) batteryPercent = atoi(battery);
  if (const char *step = getenv("DIGIPET_SIM_SCRIPT_STEP")) scriptIndex = strtoul(step, nullptr, 10);
  std::ifstream in(rtcMemoryPath(), std::ios::binary);
  if (in && __start_rtc_data && __stop_rtc_data) {
    in.read(__start_rtc_data, __stop_rtc_data - __start_rtc_data);
  }
  remove(rtcMemoryPath().c_str());
  for (const char *name : {"DIGIPET_SIM_RESET", "DIGIPET_SIM_SPEED", "DIGIPET_SIM_USB",
                           "DIGIPET_SIM_BATTERY", "DIGIPET_SIM_WINPOS",
                           "DIGIPET_SIM_SCRIPT_STEP"}) {
    unsetenv(name);
  }
}

// --- Window ------------------------------------------------------------------

void updateTitle() {
  char title[160];
  snprintf(title, sizeof(title), "Digipet sim  |  %s %u%%  |  %gx%s", usbPower ? "USB" : "BAT",
           batteryPercent, clockSpeed.load(), panelOn ? "" : "  |  display off");
  frontend::setTitle(title);
}

void handleKey(Key key, bool down) {
  switch (key) {
    case Key::Boot: bootKeyHeld = down; break;
    case Key::User: userKeyHeld = down; break;
    default: break;
  }
  if (!down) return;
  switch (key) {
    case Key::UsbPower:
      usbPower = !usbPower;
      printf("[sim] USB power %s (the firmware polls the PMU every 30 s)\n",
             usbPower ? "connected" : "removed");
      break;
    case Key::FastForward:
      setSpeed(clockSpeed > 1.0 ? 1.0 : options.fastForward);
      printf("[sim] speed %gx\n", clockSpeed.load());
      break;
    case Key::BatteryDown: batteryPercent = batteryPercent >= 10 ? batteryPercent - 10 : 0; break;
    case Key::BatteryUp: batteryPercent = std::min(100, batteryPercent + 10); break;
    case Key::Screenshot: screenshot(); break;
    case Key::PowerCycle: reexec("poweron", false);
    case Key::Quit:
      frontend::shutdown();
      exit(0);
    default: break;
  }
  updateTitle();
}

void processInput() {
  InputEvent event;
  while (frontend::pollEvent(event)) {
    switch (event.type) {
      case InputEvent::MouseDown:
      case InputEvent::MouseMove: setTouch(true, event.x, event.y); break;
      case InputEvent::MouseUp: setTouch(false, event.x, event.y); break;
      case InputEvent::KeyDown: handleKey(event.key, true); break;
      case InputEvent::KeyUp: handleKey(event.key, false); break;
      case InputEvent::Quit:
        frontend::shutdown();
        exit(0);
    }
  }
}

void printHelp() {
  printf(
      "Digipet simulator -- the real firmware on a fake AMOLED-2.16\n"
      "  mouse        touch (click = tap, drag = swipe)\n"
      "  Space / B    BOOT key (power key: sleep / wake)\n"
      "  N            user key (same as BOOT)\n"
      "  P            plug / unplug USB power\n"
      "  F            fast-forward on/off (%gx)\n"
      "  - / =        battery -10%% / +10%%\n"
      "  S            screenshot to %s/\n"
      "  R            power-cycle (cold boot)\n"
      "  Esc          quit\n"
      "  terminal     type serial debug commands (WHOAMI, POWERLOG, SIMBATTLE 1 ...)\n\n",
      options.fastForward, options.dataDir.c_str());
}

void usage(const char *program) {
  fprintf(stderr,
          "usage: %s [--scale N] [--speed X] [--ff X] [--usb] [--fresh]\n"
          "          [--data DIR] [--script FILE]\n",
          program);
  exit(2);
}

void parseArgs(int argc, char **argv) {
  for (int i = 0; i < argc; ++i) originalArgs.emplace_back(argv[i]);
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> const char * {
      if (i + 1 >= argc) usage(argv[0]);
      return argv[++i];
    };
    if (arg == "--scale") options.scale = std::max(1, atoi(next()));
    else if (arg == "--speed") options.speed = std::max(0.1, atof(next()));
    else if (arg == "--ff") options.fastForward = std::max(1.0, atof(next()));
    else if (arg == "--usb") options.usbPower = true;
    else if (arg == "--fresh") options.fresh = true;
    else if (arg == "--data") options.dataDir = next();
    else if (arg == "--script") options.scriptPath = next();
    else usage(argv[0]);
  }
}

}  // namespace

// --- sim_hal.h ---------------------------------------------------------------

uint32_t millis() { return static_cast<uint32_t>(micros() / 1000); }

uint64_t micros() {
  std::lock_guard<std::mutex> lock(clockMutex);
  return clockAnchorMicros + static_cast<uint64_t>(
      std::chrono::duration<double, std::micro>(Clock::now() - clockAnchorReal).count() *
      clockSpeed);
}

void delayMs(uint32_t ms) {
  const uint64_t until = micros() + static_cast<uint64_t>(ms) * 1000;
  for (;;) {
    const uint64_t now = micros();
    if (now >= until) return;
    if (onMainThread()) pump();
    const double realMicros = (until - now) / clockSpeed.load();
    std::this_thread::sleep_for(std::chrono::microseconds(
        static_cast<int64_t>(std::min(realMicros, 2000.0)) + 1));
  }
}

void pump() {
  if (!onMainThread()) return;
  processInput();
  runScript();
  const auto now = Clock::now();
  if (now - lastPresent >= std::chrono::milliseconds(16)) {
    lastPresent = now;
    frontend::present(panelFramebuffer, panelWidth, panelHeight, panelBrightness, panelOn);
  }
  if (now - lastTitle >= std::chrono::milliseconds(500)) {
    lastTitle = now;
    updateTitle();
  }
}

int digitalRead(uint8_t pin) {
  if (pin == BOARD_BTN_BOOT) return bootKeyHeld ? BOARD_BTN_PRESSED_LEVEL : !BOARD_BTN_PRESSED_LEVEL;
  if (pin == BOARD_BTN_USER) return userKeyHeld ? BOARD_BTN_PRESSED_LEVEL : !BOARD_BTN_PRESSED_LEVEL;
  return 1;
}

void attachInterrupt(uint8_t pin, void (*isr)()) {
  if (pin == BOARD_TP_INT) touchIsr = isr;
}

bool touchPoint(int16_t &x, int16_t &y) {
  if (!touchHeld) return false;
  x = touchX;
  y = touchY;
  return true;
}

void attachPanel(uint16_t *framebuffer, int16_t width, int16_t height) {
  panelFramebuffer = framebuffer;
  panelWidth = width;
  panelHeight = height;
}

void setBrightness(uint8_t level) { panelBrightness = level; }
void setDisplayOn(bool on) { panelOn = on; }

bool i2cPresent(uint8_t address) { return i2cDevices.count(address) != 0; }

bool i2cWrite(uint8_t address, const uint8_t *data, size_t length) {
  if (!i2cPresent(address)) return false;
  if (length == 0) return true;  // address probe
  I2cDevice &device = i2cDevices[address];
  device.pointer = data[0];
  if (address == BOARD_I2C_ADDR_RTC) writeRtc(data, length);
  for (size_t i = 1; i < length; ++i) {
    device.registers[static_cast<uint8_t>(device.pointer + i - 1)] = data[i];
  }
  return true;
}

size_t i2cRead(uint8_t address, uint8_t *out, size_t length) {
  if (!i2cPresent(address)) return 0;
  const uint8_t pointer = i2cDevices[address].pointer;
  for (size_t i = 0; i < length; ++i) {
    out[i] = readRegister(address, static_cast<uint8_t>(pointer + i));
  }
  return length;
}

void audioWrite(const uint8_t *data, size_t length) {
  if (!frontend::hasAudio()) {
    // Keep the real I2S pacing: a 200 ms tone still takes 200 ms.
    const uint32_t ms = static_cast<uint32_t>(length / kAudioBytesPerMs);
    if (ms) delayMs(ms);
    return;
  }
  while (frontend::queuedAudioBytes() > kAudioQueueLimit) {
    if (onMainThread()) pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const float gain = codecGain();
  std::vector<int16_t> samples(length / 2);
  memcpy(samples.data(), data, samples.size() * 2);
  for (int16_t &sample : samples) {
    sample = static_cast<int16_t>(std::max(-32768.0f, std::min(32767.0f, sample * gain)));
  }
  frontend::queueAudio(samples.data(), samples.size() * 2);
}

bool nvsGet(const std::string &key, std::string &value) {
  std::lock_guard<std::mutex> lock(nvsMutex);
  const auto found = nvs.find(key);
  if (found == nvs.end()) return false;
  value = found->second;
  return true;
}

void nvsPut(const std::string &key, const std::string &value) {
  std::lock_guard<std::mutex> lock(nvsMutex);
  nvs[key] = value;
  nvsSave();
}

void nvsRemove(const std::string &key) {
  std::lock_guard<std::mutex> lock(nvsMutex);
  nvs.erase(key);
  nvsSave();
}

void nvsClearNamespace(const std::string &ns) {
  std::lock_guard<std::mutex> lock(nvsMutex);
  for (auto it = nvs.begin(); it != nvs.end();) {
    it = it->first.rfind(ns + "/", 0) == 0 ? nvs.erase(it) : std::next(it);
  }
  nvsSave();
}

std::string sdRoot() { return options.dataDir + "/sd"; }

bool serialLineAvailable() {
  std::lock_guard<std::mutex> lock(serialMutex);
  return !serialLines.empty();
}

std::string serialReadLine() {
  std::lock_guard<std::mutex> lock(serialMutex);
  if (serialLines.empty()) return {};
  std::string line = serialLines.front();
  serialLines.pop_front();
  return line;
}

bool resumedFromDeepSleep() { return resumed; }

void deepSleep() {
  printf("[sim] deep sleep -- press Space/B or N to wake\n");
  panelOn = false;
  updateTitle();
  // EXT1 ANY_LOW: a key that is already held wakes the chip straight away.
  while (!bootKeyHeld && !userKeyHeld) {
    pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  reexec("deepsleep", true);
}

void restart() { reexec("sw", true); }

}  // namespace sim

// --- ESP-IDF -----------------------------------------------------------------

uint32_t esp_random() {
  static std::mutex mutex;
  static std::mt19937 generator{std::random_device{}()};
  std::lock_guard<std::mutex> lock(mutex);
  return generator();
}

esp_reset_reason_t esp_reset_reason() { return sim::resetReason; }

void esp_restart() { sim::restart(); }

// Plain FIPS 180-4 SHA-256 for generatePlayerId().
int mbedtls_sha256(const unsigned char *input, size_t length, unsigned char output[32], int) {
  static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
      0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
      0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
      0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
      0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  std::vector<uint8_t> message(input, input + length);
  message.push_back(0x80);
  while (message.size() % 64 != 56) message.push_back(0);
  const uint64_t bits = static_cast<uint64_t>(length) * 8;
  for (int i = 7; i >= 0; --i) message.push_back(static_cast<uint8_t>(bits >> (i * 8)));
  for (size_t chunk = 0; chunk < message.size(); chunk += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      const uint8_t *p = &message[chunk + i * 4];
      w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) +
                          k[i] + w[i];
      const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  for (int i = 0; i < 8; ++i) {
    for (int j = 0; j < 4; ++j) output[i * 4 + j] = static_cast<uint8_t>(h[i] >> (24 - j * 8));
  }
  return 0;
}

// --- Entry point -------------------------------------------------------------

int main(int argc, char **argv) {
  using namespace sim;
  mainThread = std::this_thread::get_id();
  setvbuf(stdout, nullptr, _IOLBF, 0);
  parseArgs(argc, argv);
  mkdir(options.dataDir.c_str(), 0755);
  usbPower = options.usbPower;
  clockSpeed = options.speed;
  const bool coldStart = getenv("DIGIPET_SIM_RESET") == nullptr;
  restoreAfterReexec();
  if (coldStart && options.fresh) {
    remove(nvsPath().c_str());
    printf("[sim] --fresh: cleared saved state\n");
  }
  nvsLoad();
  if (!options.scriptPath.empty() && !loadScript(options.scriptPath)) return 2;

  for (uint8_t address : {BOARD_I2C_ADDR_TOUCH, BOARD_I2C_ADDR_PMIC, BOARD_I2C_ADDR_IMU,
                          BOARD_I2C_ADDR_RTC, BOARD_I2C_ADDR_ES8311, BOARD_I2C_ADDR_ES7210}) {
    i2cDevices[address];
  }

  if (!frontend::init(BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, options.scale)) return 1;
  if (coldStart) printHelp();
  if (script.empty()) startSerialReader();

  ::setup();
  for (;;) {
    ::loop();
    pump();
    // The firmware's loop() spins flat out on the board too; on a desktop
    // that would just burn a core for no visible difference.
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
}
