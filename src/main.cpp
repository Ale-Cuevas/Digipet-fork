#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <cstdarg>
#include <time.h>
#include <esp32-hal-psram.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>

#include <Adafruit_XCA9554.h>
#include <Arduino_DriveBus_Library.h>
#include <Arduino_GFX_Library.h>

#include "pin_config.h"
#include "familiar_battle_service.h"
#include "ota_updater.h"
#include "pet_genome.h"
#include "ui_pages.h"


constexpr uint32_t PET_MAGIC_V1 = 0x44504731;
constexpr uint32_t PET_MAGIC_V2 = 0x44504732;
constexpr uint32_t PET_MAGIC_V3 = 0x44504733;  // no bonusHp/Attack/Defense/Special fields yet
constexpr uint32_t PET_MAGIC_V4 = 0x44504734;  // no stepsTotal field yet
constexpr uint32_t PET_MAGIC = 0x44504735;
constexpr uint32_t ANIMATION_MS = 650;
constexpr uint32_t SETTINGS_MAGIC_V1 = 0x44505331;
constexpr uint32_t SETTINGS_MAGIC_V2 = 0x44505332;
constexpr uint32_t SETTINGS_MAGIC = 0x44505333;

uint16_t COLOR_BACKGROUND = 0x0823;
uint16_t COLOR_CARD = 0x18E8;
uint16_t COLOR_MINT = 0x6718;
uint16_t COLOR_TEXT = 0xE73C;
uint16_t COLOR_MUTED = 0x8413;
uint16_t COLOR_WARNING = 0xFE48;
uint16_t COLOR_DANGER = 0xF2CB;
uint16_t COLOR_CYAN = 0x269F;
uint16_t COLOR_PURPLE = 0xA81F;

const char *STAGE_NAMES[] = {"BIT EGG", "HATCHLING", "SCOUT", "GUARDIAN", "TITAN"};

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(
    bus, GFX_NOT_DEFINED, 0, LCD_WIDTH, LCD_HEIGHT, 16, 0, 0, 0);
Arduino_GFX *display = panel;
Arduino_Canvas pageCanvasA(LCD_WIDTH, LCD_HEIGHT, panel);
Arduino_Canvas pageCanvasB(LCD_WIDTH, LCD_HEIGHT, panel);
uint16_t *transitionFrame = nullptr;
bool transitionsReady = false;

std::shared_ptr<Arduino_IIC_DriveBus> i2cBus =
    std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);

void touchInterrupt();
void drawCentered(const char *text, int16_t y, uint8_t size, uint16_t color);
bool i2cPresent(uint8_t address);
void saveCopiedGenome(const PetGenome &genome);
std::unique_ptr<Arduino_IIC> touch(new Arduino_CST816x(
    i2cBus, CST816T_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, TP_INT, touchInterrupt));

Adafruit_XCA9554 expander;
Preferences preferences;
I2SClass audioI2S;
PetState pet;
FamiliarBattleService battle;

DeviceSettings settings;
const uint32_t SLEEP_TIMEOUTS[] = {15000, 30000, 60000, 120000};
const char *SLEEP_LABELS[] = {"15 SEC", "30 SEC", "1 MIN", "2 MIN"};
const char *VOLUME_LABELS[] = {"MUTE", "LOW", "MEDIUM", "HIGH", "MAX"};
const uint8_t CODEC_VOLUMES[] = {0x00, 0x58, 0x8B, 0xBF, 0xF2};
const char *WAKE_LABELS[] = {"TOUCH", "BOOT KEY"};
const char *THEME_LABELS[] = {"AUTO // PET", "CYBER MINT", "AMBER CORE",
                              "VIOLET LINK", "MONO SIGNAL"};
uint8_t settingsGridPage = 0;

uint8_t brightnessLevel() {
  // Keep the 0% choice barely visible so it can always be changed again.
  return settings.brightnessIndex == 0 ? 8 :
      static_cast<uint8_t>(settings.brightnessIndex * 255 / 10);
}

SettingsView settingsView = SETTINGS_HOME;

uint32_t lastMinute = 0;
uint32_t lastTouchRead = 0;
uint32_t lastInteraction = 0;
uint32_t lastAnimation = 0;
uint16_t creatureFrameInterval = 45;
bool touchWasDown = false;
bool sleeping = false;
bool screenOff = false;
bool animationFrame = false;
bool imuDetected = false;
bool rtcDetected = false;
bool pmuDetected = false;
bool codecDetected = false;
uint8_t i2cDeviceCount = 0;
bool audioReady = false;
bool sdDetected = false;
bool clockValid = false;
bool timeSynced = false;
uint32_t lastClockDraw = 0;
uint32_t sleepStarted = 0;
// Captured once in setup() via getCpuFrequencyMhz() rather than assumed, so
// turnScreenOff()/wakeUp() restore whatever this board's own default
// actually is instead of a guessed constant. Deep idle (asleep, screen
// off) runs the CPU at kIdleCpuMhz instead -- with WiFi/BLE already fully
// off in that state (both only ever init on demand, see
// FamiliarBattleService::beginRadio()'s own comment), there's no radio
// timing floor to respect, and 80 MHz is the conservative, widely-used
// "still safe for I2C/UART" floor rather than pushing all the way down.
uint32_t fullCpuMhz = 240;
constexpr uint32_t kIdleCpuMhz = 80;
uint32_t lastImuRead = 0;
uint32_t lastBootRead = 0;
uint32_t nextBlink = 0;
uint32_t blinkUntil = 0;
bool bootWasDown = false;
// Tracks AUDIO_PA independent of screenOff -- showIdleFromLift() clears
// screenOff on its own without touching the amp, so wakeUp() needs its own
// signal for "was the amp actually powered down" rather than re-deriving it
// from screenOff, which may already have been cleared by the time it runs.
bool audioPaOn = true;
bool imuReady = false;
bool imuBaselineReady = false;
float lastAccelX = 0;
float lastAccelY = 0;
float lastAccelZ = 0;
// Pocket-pedometer state (pollMotion() below), separate from the lift-wake
// baseline above even though both read the same accelerometer stream --
// lift-wake wants a raw per-axis delta between consecutive samples, this
// wants a slow-tracked baseline on acceleration *magnitude* so it keeps
// working no matter how the device is oriented in a pocket.
bool stepBaselineReady = false;
float stepAccelBaseline = 1.0f;
bool stepAboveThreshold = false;
uint32_t lastStepTime = 0;
// Steps since the last agePet() tick; folded into pet.stepsTotal (lifetime,
// persisted) immediately, but also read and reset once a minute by agePet()
// itself to decide whether that minute counts as "walked".
uint32_t stepsThisMinute = 0;
char clockText[6] = "--:--";
// `message` is the current feedback string; it's only actually shown while
// `toastVisible` is true, as a slide-down banner drawn by drawToastOverlay()
// (see its definition for the animation). Set both together through
// showToast()/showToastf() below rather than writing `message` directly, so
// nothing can set feedback text without also making it appear on screen.
char message[40] = "Your companion is awake";
bool toastVisible = false;
uint32_t toastShownAt = 0;
uint8_t playerId[32]{};
char playerIdHex[65]{};
uint64_t playerIdTimestamp = 0;
bool showingPlayerId = false;
bool showingUpdate = false;
bool showingGenomeProfile = false;
// See this flag's own comment in ui_pages.h.
bool genomeProfileShowingData = false;
bool showingEvolutionDebug = false;
bool newEggConfirmation = false;
uint32_t newEggConfirmationUntil = 0;
uint8_t pendingHatchMode = 0;
uint32_t touchStartedAt = 0;
PetGenome copiedGenome{};
bool hasCopiedGenome = false;
bool battleGenomeCopied = false;
char genomeTransferStatus[32] = "NO COPIED GENOME";
// A Find scan's results are shown as a picker instead of auto-connecting to
// the first one found -- battle.state() stays Idle throughout, so this is
// tracked separately rather than as another FamiliarBattleState.
bool showingBattleResults = false;
uint8_t battleResultsPage = 0;
// Flee needs a second confirming tap so it can't be triggered by a
// mis-tap during a real match; the arm expires on its own too.
bool fleeArmed = false;
uint32_t fleeArmedUntil = 0;

// A per-device record, not a per-pet one -- like playerId itself (derived
// from ESP.getEfuseMac(), not saved in `pet`), it persists across egg
// hatches/blends rather than resetting with the current companion.
constexpr uint32_t BATTLE_STATS_MAGIC = 0x42535401;  // "BST" + schema 1

BattleStats battleStats{};
bool showingRivals = false;

// Same reasoning as BATTLE_STATS_MAGIC/battleStats just above.
constexpr uint32_t RECON_LOG_MAGIC = 0x52434C31;  // "RCL" + schema 1

ReconLog reconLog{};
bool showingReconLog = false;

// Same reasoning as BATTLE_STATS_MAGIC/battleStats above.
constexpr uint32_t FRIENDS_LIST_MAGIC = 0x46524E31;  // "FRN" + schema 1

FriendsList friendsList{};
bool showingFriends = false;

// See this flag's own comment in ui_pages.h.
bool statusShowingActions = false;
// See this variable's own comment in ui_pages.h.
uint8_t statusActionsPage = 0;

// Not a per-device record like the ones above -- items are earned by and
// belong to the current companion the same way food/joy do, so this reads
// more like DeviceSettings' shape (single magic-guarded blob, no capped
// array of entries) than BattleStats'.
constexpr uint32_t INVENTORY_MAGIC = 0x49544D31;  // "ITM" + schema 1

Inventory inventory{};
bool showingItems = false;

// See this flag's own comment in ui_pages.h.
bool battleShowingActions = false;

// Sets `message` and arms drawToastOverlay() to show it as a slide-down
// banner on whichever of the five main pages is on screen next -- see the
// comment on `message`'s declaration above.
void showToast(const char *text) {
  strncpy(message, text, sizeof(message) - 1);
  message[sizeof(message) - 1] = '\0';
  toastShownAt = millis();
  toastVisible = true;
}

void showToastf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  toastShownAt = millis();
  toastVisible = true;
}

struct NetworkConfig {
  char ssid[65];
  char password[65];
  char timezone[48];
  char ntp[65];
  bool syncOnBoot;
  bool valid;
};

NetworkConfig networkConfig{};

Page genomeProfileReturnPage = PAGE_COMPANION;
void presentCoherentPageFrame(Page page);
void transitionSettingsView(SettingsView target, bool forward);
void transitionSettingsGrid(uint8_t target);
void transitionStatusView(bool showActions);
void transitionStatusActionsGrid(uint8_t target);
void transitionBattleView(bool showActions);
void transitionGenomeProfileView(bool showData);
void presentGenomeProfilePage();
void performAction(int action);
void presentOverlayEntrance(Page fromPage, void (*drawOverlay)());
void presentOverlayExit(void (*drawOverlay)(), Page toPage);
Page currentPage = PAGE_COMPANION;
FamiliarBattleState lastBattleState = FamiliarBattleState::Idle;
uint16_t lastBattleTurn = 0;
uint16_t lastBattleMyHp = 0;
uint16_t lastBattleOpponentHp = 0;
int32_t touchStartX = -1;
int32_t touchStartY = -1;
int32_t touchLastX = -1;
int32_t touchLastY = -1;

void touchInterrupt() {
  touch->IIC_Interrupt_Flag = true;
}

bool codecWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(0x18);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

uint8_t codecRead(uint8_t reg) {
  Wire.beginTransmission(0x18);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom(0x18, 1) != 1) return 0;
  return Wire.read();
}

bool initAudio() {
  if (!codecDetected) return false;
  pinMode(AUDIO_PA, OUTPUT);
  digitalWrite(AUDIO_PA, HIGH);
  delay(20);
  audioI2S.setPins(I2S_BCLK, I2S_LRCLK, I2S_DOUT, I2S_DIN, I2S_MCLK);
  if (!audioI2S.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT,
                      I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("Audio: I2S initialization failed");
    digitalWrite(AUDIO_PA, LOW);
    return false;
  }

  // ES8311 setup for 16 kHz / 4.096 MHz MCLK, following Waveshare's V2 example.
  const uint8_t init[][2] = {
      {0x00, 0x1F}, {0x00, 0x00}, {0x00, 0x80}, {0x01, 0x3F},
      {0x06, 0x00}, {0x02, 0x00}, {0x03, 0x10}, {0x04, 0x10},
      {0x05, 0x00}, {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xFF},
      {0x09, 0x0C}, {0x0A, 0x0C}, {0x0D, 0x01}, {0x0E, 0x02},
      {0x12, 0x00}, {0x13, 0x10}, {0x1C, 0x6A}, {0x37, 0x08},
      {0x17, 0xC8}, {0x14, 0x1A}, {0x32, CODEC_VOLUMES[settings.volumeIndex]}};
  for (const auto &entry : init) {
    if (!codecWrite(entry[0], entry[1])) return false;
  }
  codecWrite(0x31, codecRead(0x31) & ~0x60); // unmute DAC
  Serial.println("Audio: ES8311 speaker ready");
  return true;
}

// Shared 32-sample sine table for every synthesized tone in the app.
static const int8_t kSineTable[] = {
    0, 25, 49, 71, 90, 106, 117, 125, 127, 125, 117, 106, 90, 71, 49, 25,
    0, -25, -49, -71, -90, -106, -117, -125, -127, -125, -117, -106,
    -90, -71, -49, -25};

// A soft attack into a curved decay across the rest of the note, rather
// than attack/flat-sustain/release: the flat plateau is what makes a tone
// read as a hard electronic "beep" instead of a struck/plucked chime.
float chimeEnvelope(uint32_t position, uint32_t attackSamples, uint32_t total) {
  if (total == 0) return 0.0f;
  if (attackSamples >= total) attackSamples = total - 1;
  if (position < attackSamples) {
    return attackSamples ? static_cast<float>(position) / attackSamples : 1.0f;
  }
  const float t = static_cast<float>(position - attackSamples) /
                  (total - attackSamples);
  return (1.0f - t) * (1.0f - t);
}

void playTone(uint16_t frequency, uint16_t durationMs, uint8_t level = 55) {
  if (!settings.soundEnabled || !audioReady || frequency == 0) return;
  constexpr uint32_t sampleRate = 16000;
  const uint32_t total = (sampleRate * durationMs) / 1000;
  const uint32_t attackSamples = min<uint32_t>(total / 6, sampleRate * 10 / 1000);
  uint32_t phase = 0;
  const uint32_t step = (static_cast<uint64_t>(frequency) << 32) / sampleRate;
  int16_t buffer[256]; // 128 stereo frames
  uint32_t produced = 0;
  while (produced < total) {
    const uint16_t frames = min<uint32_t>(128, total - produced);
    for (uint16_t i = 0; i < frames; i++) {
      const float envelope = chimeEnvelope(produced + i, attackSamples, total);
      // Peak is about 10,000 at level 100, safely inside signed 16-bit PCM.
      const int16_t sample = static_cast<int16_t>(
          kSineTable[phase >> 27] * static_cast<float>(level) * envelope *
          (10000.0f / (127.0f * 100.0f)));
      phase += step;
      buffer[i * 2] = sample;
      buffer[i * 2 + 1] = sample;
    }
    audioI2S.write(reinterpret_cast<uint8_t *>(buffer), frames * 4);
    produced += frames;
  }
}

// Mixes up to 4 simultaneous notes with the same chime envelope, for actual
// chords instead of a fast sequential arpeggio -- the single biggest lever
// for making a simple synth sound like a modern earcon instead of a 90s
// appliance beep.
void playChord(const uint16_t *frequencies, uint8_t voiceCount, uint16_t durationMs,
               uint8_t level = 55) {
  if (!settings.soundEnabled || !audioReady || voiceCount == 0) return;
  constexpr uint32_t sampleRate = 16000;
  const uint8_t voices = min<uint8_t>(voiceCount, 4);
  const uint32_t total = (sampleRate * durationMs) / 1000;
  const uint32_t attackSamples = min<uint32_t>(total / 6, sampleRate * 12 / 1000);
  uint32_t phase[4] = {0, 0, 0, 0};
  uint32_t step[4] = {0, 0, 0, 0};
  for (uint8_t v = 0; v < voices; v++)
    step[v] = (static_cast<uint64_t>(frequencies[v]) << 32) / sampleRate;
  const float scale = (10000.0f / 127.0f / 100.0f) / voices;
  int16_t buffer[256];
  uint32_t produced = 0;
  while (produced < total) {
    const uint16_t frames = min<uint32_t>(128, total - produced);
    for (uint16_t i = 0; i < frames; i++) {
      const float envelope = chimeEnvelope(produced + i, attackSamples, total);
      int32_t mixed = 0;
      for (uint8_t v = 0; v < voices; v++) {
        mixed += kSineTable[phase[v] >> 27];
        phase[v] += step[v];
      }
      const int16_t sample = static_cast<int16_t>(
          mixed * static_cast<float>(level) * envelope * scale);
      buffer[i * 2] = sample;
      buffer[i * 2 + 1] = sample;
    }
    audioI2S.write(reinterpret_cast<uint8_t *>(buffer), frames * 4);
    produced += frames;
  }
}

// The boot clip is embedded directly in flash (see platformio.ini's
// board_build.embed_files) as raw 16kHz/16-bit/stereo PCM, converted once
// from sd-card/Audio/Boot.mp3 -- there's no MP3 decoder in this firmware,
// so decoding happens offline and playback is just a straight PCM stream
// through the same I2S path playTone() already uses.
extern const uint8_t boot_clip_pcm_start[] asm("_binary_src_assets_boot_clip_pcm_start");
extern const uint8_t boot_clip_pcm_end[] asm("_binary_src_assets_boot_clip_pcm_end");

void bootClipTask(void *) {
  const uint8_t *data = boot_clip_pcm_start;
  const size_t total = static_cast<size_t>(boot_clip_pcm_end - boot_clip_pcm_start);
  size_t offset = 0;
  while (offset < total) {
    const size_t chunk = min<size_t>(512, total - offset);
    audioI2S.write(data + offset, chunk);
    offset += chunk;
  }
  vTaskDelete(nullptr);
}

// Streamed on its own FreeRTOS task, pinned off the core running setup(),
// so the ~5.2s clip plays in real time alongside the boot animation's
// render loop instead of blocking it outright (a single blocking call here
// would freeze the animation on its first frame until playback finished).
void playBootClipAsync() {
  if (!settings.soundEnabled || !audioReady) return;
  xTaskCreatePinnedToCore(bootClipTask, "bootClip", 3072, nullptr, 1, nullptr, 0);
}

void playActionSound(uint8_t action) {
  if (action == 0) {
    playTone(440, 55); playTone(523, 80);
  } else if (action == 1) {
    playTone(659, 55); playTone(880, 55); playTone(1047, 90);
  } else {
    playTone(330, 55); playTone(440, 55); playTone(659, 110);
  }
}

uint8_t bcdToDec(uint8_t value) {
  return ((value >> 4) * 10) + (value & 0x0F);
}

uint8_t decToBcd(uint8_t value) {
  return ((value / 10) << 4) | (value % 10);
}

bool imuWrite(uint8_t reg, uint8_t value) {
  const uint8_t address = i2cPresent(0x6B) ? 0x6B : 0x6A;
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool imuRead(uint8_t reg, uint8_t *data, uint8_t length) {
  const uint8_t address = i2cPresent(0x6B) ? 0x6B : 0x6A;
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(address, length) != length) return false;
  for (uint8_t i = 0; i < length; i++) data[i] = Wire.read();
  return true;
}

bool initializeLiftSensor() {
  if (!imuDetected) return false;
  uint8_t ctrl5 = 0;
  uint8_t ctrl7 = 0;
  imuRead(0x06, &ctrl5, 1);
  imuRead(0x08, &ctrl7, 1);
  // ±4 g, 11 Hz low-power accelerometer, LPF enabled; gyroscope stays off.
  const bool configured = imuWrite(0x03, 0x1E) && imuWrite(0x06, (ctrl5 & 0xF0) | 0x01) &&
                          imuWrite(0x08, (ctrl7 & 0xFC) | 0x01);
  Serial.printf("Lift sensor: %s\n", configured ? "ready" : "initialization failed");
  return configured;
}

bool readAcceleration(float &x, float &y, float &z) {
  uint8_t raw[6];
  if (!imuReady || !imuRead(0x35, raw, sizeof(raw))) return false;
  const int16_t rx = static_cast<int16_t>((raw[1] << 8) | raw[0]);
  const int16_t ry = static_cast<int16_t>((raw[3] << 8) | raw[2]);
  const int16_t rz = static_cast<int16_t>((raw[5] << 8) | raw[4]);
  x = rx * (4.0f / 32768.0f);
  y = ry * (4.0f / 32768.0f);
  z = rz * (4.0f / 32768.0f);
  return true;
}

bool rtcRead(struct tm &value) {
  Wire.beginTransmission(0x51);
  Wire.write(0x04);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(0x51, 7) != 7) {
    return false;
  }
  uint8_t data[7];
  for (uint8_t &byte : data) byte = Wire.read();
  if (data[0] & 0x80) return false; // oscillator-stop flag: time is untrustworthy
  value = {};
  value.tm_sec = bcdToDec(data[0] & 0x7F);
  value.tm_min = bcdToDec(data[1] & 0x7F);
  value.tm_hour = bcdToDec(data[2] & 0x3F);
  value.tm_mday = bcdToDec(data[3] & 0x3F);
  value.tm_wday = bcdToDec(data[4] & 0x07);
  value.tm_mon = bcdToDec(data[5] & 0x1F) - 1;
  value.tm_year = bcdToDec(data[6]) + 100;
  return value.tm_year >= 124 && value.tm_mon >= 0 && value.tm_mon < 12 &&
         value.tm_mday >= 1 && value.tm_mday <= 31 && value.tm_hour <= 23 &&
         value.tm_min <= 59 && value.tm_sec <= 59;
}

bool rtcWrite(const struct tm &value) {
  const uint8_t data[] = {
      0x04, decToBcd(value.tm_sec), decToBcd(value.tm_min),
      decToBcd(value.tm_hour), decToBcd(value.tm_mday),
      decToBcd(value.tm_wday), decToBcd(value.tm_mon + 1),
      decToBcd((value.tm_year + 1900) % 100)};
  Wire.beginTransmission(0x51);
  Wire.write(data, sizeof(data));
  return Wire.endTransmission() == 0;
}

const char *posixTimezone(const char *timezone) {
  if (strcmp(timezone, "Europe/London") == 0) {
    return "GMT0BST,M3.5.0/1,M10.5.0";
  }
  // Advanced users may place a POSIX TZ expression directly in the file.
  return timezone[0] ? timezone : "UTC0";
}

void copyConfigValue(char *destination, size_t size, const String &value) {
  String clean = value;
  if (clean.endsWith("\r")) clean.remove(clean.length() - 1);
  clean.toCharArray(destination, size);
}

bool readNetworkConfig() {
  if (!SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA) ||
      !SD_MMC.begin("/sdcard", true)) {
    Serial.println("SD: card mount failed");
    return false;
  }
  sdDetected = SD_MMC.cardType() != CARD_NONE;
  if (!sdDetected) {
    Serial.println("SD: no card present");
    SD_MMC.end();
    return false;
  }
  File file = SD_MMC.open("/digipet/wifi.ini", FILE_READ);
  if (!file || file.size() == 0 || file.size() > 1024) {
    Serial.println("SD: /digipet/wifi.ini missing or invalid size");
    if (file) file.close();
    SD_MMC.end();
    return false;
  }
  networkConfig = {};
  networkConfig.syncOnBoot = true;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.endsWith("\r")) line.remove(line.length() - 1);
    String trimmed = line;
    trimmed.trim();
    if (!trimmed.length() || trimmed.startsWith("#") || trimmed.startsWith(";")) continue;
    const int separator = line.indexOf('=');
    if (separator <= 0) continue;
    String key = line.substring(0, separator);
    String value = line.substring(separator + 1);
    key.trim(); key.toLowerCase();
    if (key == "ssid") copyConfigValue(networkConfig.ssid, sizeof(networkConfig.ssid), value);
    else if (key == "password") copyConfigValue(networkConfig.password, sizeof(networkConfig.password), value);
    else if (key == "timezone") copyConfigValue(networkConfig.timezone, sizeof(networkConfig.timezone), value);
    else if (key == "ntp") copyConfigValue(networkConfig.ntp, sizeof(networkConfig.ntp), value);
    else if (key == "sync_on_boot") {
      value.trim(); value.toLowerCase();
      networkConfig.syncOnBoot = value == "true" || value == "1" || value == "yes";
    }
  }
  file.close();
  SD_MMC.end();
  networkConfig.valid = networkConfig.ssid[0] && networkConfig.password[0] &&
                        networkConfig.timezone[0] && networkConfig.ntp[0];
  Serial.printf("SD: Wi-Fi configuration %s (credentials hidden)\n",
                networkConfig.valid ? "valid" : "invalid");
  return networkConfig.valid;
}

bool mountGenomeCard() {
  expander.pinMode(7, OUTPUT);
  expander.digitalWrite(7, HIGH);
  delay(100);
  if (!SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA) ||
      !SD_MMC.begin("/sdcard", true)) {
    expander.digitalWrite(7, LOW);
    return false;
  }
  return SD_MMC.cardType() != CARD_NONE;
}

void closeGenomeCard() {
  SD_MMC.end();
  expander.digitalWrite(7, LOW);
}

bool exportActiveGenome() {
  if (!mountGenomeCard()) {
    strcpy(genomeTransferStatus, "SD CARD NOT READY");
    return false;
  }
  SD_MMC.mkdir("/digipet");
  File file = SD_MMC.open("/digipet/genome.txt", FILE_WRITE);
  char code[PET_GENOME_CODE_LENGTH + 1]{};
  const bool encoded = encodePetGenome(pet.genome, code, sizeof(code));
  const bool written = file && encoded && file.println(code) > 0;
  if (file) file.close();
  closeGenomeCard();
  strcpy(genomeTransferStatus, written ? "EXPORTED TO SD" : "EXPORT FAILED");
  return written;
}

bool importCopiedGenome() {
  if (!mountGenomeCard()) {
    strcpy(genomeTransferStatus, "SD CARD NOT READY");
    return false;
  }
  File file = SD_MMC.open("/digipet/genome.txt", FILE_READ);
  String code;
  while (file && file.available() && !code.length()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() && !line.startsWith("#") && !line.startsWith(";")) code = line;
  }
  if (file) file.close();
  closeGenomeCard();
  code.trim();
  PetGenome imported{};
  if (!decodePetGenome(code.c_str(), imported)) {
    strcpy(genomeTransferStatus, "INVALID GENOME CODE");
    return false;
  }
  saveCopiedGenome(imported);
  return hasCopiedGenome;
}

// A BIOS/POST-style scrolling status log, standing in for the old spinner
// screen. Runs on every boot now (see runStartupNetworkSync), not just as a
// rare manual action.
struct BootLogEntry {
  char text[26];
  uint16_t color;
};
constexpr uint8_t BOOT_LOG_MAX = 6;
constexpr int16_t BOOT_LOG_X = 34;
constexpr int16_t BOOT_LOG_TOP = 156;
constexpr int16_t BOOT_LOG_LINE_H = 32;
constexpr int16_t BOOT_LOG_SPIN_X = 310;
BootLogEntry bootLog[BOOT_LOG_MAX];
uint8_t bootLogCount = 0;

// Renders "LABEL...... STATUS", dot-padding the label to a fixed column so
// every line lines up like a real boot log.
void formatBiosLine(char *out, size_t outSize, const char *label, const char *status) {
  char padded[16];
  size_t len = 0;
  while (label[len] && len < sizeof(padded) - 1) { padded[len] = label[len]; ++len; }
  while (len < 14 && len < sizeof(padded) - 1) padded[len++] = '.';
  padded[len] = '\0';
  snprintf(out, outSize, "%s %s", padded, status);
}

// Full redraw through the canvas: only called on real state changes (a new
// line, or a line's final status), never in the per-frame wait loop.
void drawBootLogScreen() {
  Arduino_GFX *previousDisplay = display;
  if (transitionsReady) display = &pageCanvasA;
  display->fillScreen(COLOR_BACKGROUND);
  drawCentered("SYSTEM BOOT", 60, 3, COLOR_MINT);
  drawCentered("STARTUP DIAGNOSTICS", 96, 1, COLOR_CYAN);
  display->setTextSize(2);
  for (uint8_t i = 0; i < bootLogCount; ++i) {
    display->setTextColor(bootLog[i].color);
    display->setCursor(BOOT_LOG_X, BOOT_LOG_TOP + i * BOOT_LOG_LINE_H);
    display->print(bootLog[i].text);
  }
  drawCentered("OFFLINE PLAY REMAINS AVAILABLE", 404, 1, COLOR_MUTED);
  if (transitionsReady) {
    display = previousDisplay;
    panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);
  }
}

void bootLogPush(const char *label, const char *status, uint16_t color) {
  if (bootLogCount >= BOOT_LOG_MAX) {
    for (uint8_t i = 1; i < BOOT_LOG_MAX; ++i) bootLog[i - 1] = bootLog[i];
    bootLogCount = BOOT_LOG_MAX - 1;
  }
  formatBiosLine(bootLog[bootLogCount].text, sizeof(bootLog[bootLogCount].text), label, status);
  bootLog[bootLogCount].color = color;
  ++bootLogCount;
  drawBootLogScreen();
}

void bootLogUpdateLast(const char *label, const char *status, uint16_t color) {
  if (bootLogCount == 0) return;
  formatBiosLine(bootLog[bootLogCount - 1].text, sizeof(bootLog[bootLogCount - 1].text),
                label, status);
  bootLog[bootLogCount - 1].color = color;
  drawBootLogScreen();
}

// Ticks a small "|/-\" spinner glyph right after the current line's text
// (measured, not a fixed column, so it doesn't float in a gap after short
// status words), drawn straight to the live panel rather than through a
// full-frame canvas blit — this is the same lesson as the old circular
// spinner: a ~20x20px update every ~100ms is fine, but a 329KB blit at that
// rate stacks extra SPI traffic right on top of Wi-Fi's own bus contention
// and causes hitches.
void bootLogSpin(uint8_t frame) {
  if (bootLogCount == 0) return;
  static const char glyphs[] = "|/-\\";
  const int16_t y = BOOT_LOG_TOP + (bootLogCount - 1) * BOOT_LOG_LINE_H;
  display->setTextSize(2);
  int16_t boundsX, boundsY;
  uint16_t textW, textH;
  display->getTextBounds(bootLog[bootLogCount - 1].text, BOOT_LOG_X, y,
                         &boundsX, &boundsY, &textW, &textH);
  const int16_t glyphX = BOOT_LOG_X + textW + 12;
  display->fillRect(glyphX, y - 2, 22, 20, COLOR_BACKGROUND);
  display->setTextColor(bootLog[bootLogCount - 1].color);
  display->setCursor(glyphX, y);
  display->print(glyphs[frame % 4]);
}

bool synchronizeClock() {
  bootLogCount = 0;
  if (!networkConfig.valid) return false;
  bootLogPush("LINK", "CONNECTING", COLOR_CYAN);
  WiFi.mode(WIFI_STA);
  WiFi.begin(networkConfig.ssid, networkConfig.password);
  const uint32_t started = millis();
  uint8_t linkFrame = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - started < 12000) {
    bootLogSpin(linkFrame++);
    delay(90);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi: connection timed out");
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    bootLogUpdateLast("LINK", "OFFLINE", COLOR_WARNING);
    delay(650);
    return false;
  }
  Serial.println("Wi-Fi: connected; requesting NTP time");
  bootLogUpdateLast("LINK", "OK", COLOR_MINT);
  bootLogPush("TIME SYNC", "SYNCING", COLOR_CYAN);
  configTzTime(posixTimezone(networkConfig.timezone), networkConfig.ntp);
  struct tm local{};
  bool received = false;
  const uint32_t syncStarted = millis();
  while (!received && millis() - syncStarted < 10000) {
    received = getLocalTime(&local, 120);
    bootLogSpin(linkFrame++);
  }
  if (received && rtcWrite(local)) {
    clockValid = timeSynced = true;
    preferences.putULong64("lastSync", static_cast<uint64_t>(time(nullptr)));
    snprintf(clockText, sizeof(clockText), "%02d:%02d", local.tm_hour, local.tm_min);
    Serial.println("Time: NTP synchronized and RTC updated");
    bootLogUpdateLast("TIME SYNC", "OK", COLOR_MINT);
  } else {
    Serial.println("Time: NTP or RTC update failed");
    bootLogUpdateLast("TIME SYNC", "FAILED", COLOR_DANGER);
  }
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  Serial.println("Wi-Fi: radio disabled");
  bootLogPush("SYSTEM", "READY", COLOR_MINT);
  delay(500);
  return received;
}

// Runs as its own visible phase after the boot animation finishes, instead
// of polled silently mid-frame (that used to cause visible stutter during
// the animation, since the WiFi/NTP checks weren't as free as they looked).
void runStartupNetworkSync() {
  struct tm rtcTime{};
  clockValid = rtcDetected && rtcRead(rtcTime);
  if (clockValid) {
    snprintf(clockText, sizeof(clockText), "%02d:%02d", rtcTime.tm_hour, rtcTime.tm_min);
    Serial.println("RTC: valid retained time found");
  } else {
    Serial.println("RTC: time is invalid or power was lost");
  }
  if (!readNetworkConfig() || (!networkConfig.syncOnBoot && clockValid)) return;

  expander.pinMode(7, OUTPUT);
  expander.digitalWrite(7, HIGH);
  delay(100);
  synchronizeClock();
  expander.digitalWrite(7, LOW);
}

void generatePlayerId() {
  uint8_t mac[6]{};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  playerIdTimestamp = static_cast<uint64_t>(time(nullptr));
  const uint32_t randomValue = esp_random();
  uint8_t input[15]{};
  memcpy(input, mac + 3, 3);
  for (uint8_t i = 0; i < 8; ++i) input[3 + i] = playerIdTimestamp >> (56 - i * 8);
  for (uint8_t i = 0; i < 4; ++i) input[11 + i] = randomValue >> (24 - i * 8);
  mbedtls_sha256(input, sizeof(input), playerId, 0);
  for (uint8_t i = 0; i < sizeof(playerId); ++i) {
    snprintf(playerIdHex + i * 2, 3, "%02X", playerId[i]);
  }
  Serial.printf("Player ID generated at UTC %llu from device suffix %02X%02X%02X\n",
                static_cast<unsigned long long>(playerIdTimestamp),
                mac[3], mac[4], mac[5]);
}

uint8_t addClamped(uint8_t value, uint8_t amount) {
  return value > 100 - amount ? 100 : value + amount;
}

uint8_t subtractClamped(uint8_t value, uint8_t amount) {
  return value > amount ? value - amount : 0;
}

void applyTheme() {
  if (settings.themeIndex == 0) {
    const PetPalette petPalette = paletteForGenome(pet.genome);
    COLOR_BACKGROUND = scaleRgb565(petPalette.primaryDark, 22);
    COLOR_CARD = scaleRgb565(petPalette.primaryDark, 55);
    COLOR_MINT = petPalette.primaryLight;
    COLOR_TEXT = 0xF7BE;
    // Blended toward COLOR_TEXT rather than just darkened off
    // petPalette.secondary (the old scaleRgb565(..., 82) here) -- AUTO's
    // background/card are genome-derived and can land anywhere, so unlike
    // the 4 fixed themes' own COLOR_MUTED (see kThemes[]' own comment)
    // there's no single contrast fix that's provably correct for every
    // possible genome. Leaning on the one fixed, always-bright color this
    // branch has (COLOR_TEXT) is the best available heuristic improvement
    // over a flat darken, which had no relationship to either backdrop's
    // actual brightness at all.
    COLOR_MUTED = lerpRgb565(petPalette.secondary, COLOR_TEXT, 0.35f);
    COLOR_WARNING = petPalette.accent;
    COLOR_DANGER = 0xF2CB;
    COLOR_CYAN = petPalette.glow;
    COLOR_PURPLE = petPalette.secondary;
    return;
  }

  const ThemeColors &theme = kThemes[settings.themeIndex];
  COLOR_BACKGROUND = theme.background;
  COLOR_CARD = theme.card;
  COLOR_MINT = theme.primary;
  COLOR_TEXT = theme.text;
  COLOR_MUTED = theme.muted;
  COLOR_WARNING = theme.warning;
  COLOR_DANGER = theme.danger;
  COLOR_CYAN = theme.cyan;
  COLOR_PURPLE = theme.secondary;
}

void savePet() {
  preferences.putBytes("state", &pet, sizeof(pet));
}

void saveBattleStats() {
  preferences.putBytes("battleStats", &battleStats, sizeof(battleStats));
}

void loadBattleStats() {
  if (preferences.getBytesLength("battleStats") == sizeof(battleStats)) {
    preferences.getBytes("battleStats", &battleStats, sizeof(battleStats));
  }
  if (battleStats.magic != BATTLE_STATS_MAGIC) {
    // First boot, or a stored blob from a different (and thus incompatible)
    // schema -- start clean rather than risk misreading old bytes as a
    // BattleStats of the current shape.
    battleStats = BattleStats{};
    battleStats.magic = BATTLE_STATS_MAGIC;
  }
}

void saveReconLog() {
  preferences.putBytes("reconLog", &reconLog, sizeof(reconLog));
}

void loadReconLog() {
  if (preferences.getBytesLength("reconLog") == sizeof(reconLog)) {
    preferences.getBytes("reconLog", &reconLog, sizeof(reconLog));
  }
  if (reconLog.magic != RECON_LOG_MAGIC) {
    reconLog = ReconLog{};
    reconLog.magic = RECON_LOG_MAGIC;
  }
}

// Records one scan hit, evicting the oldest entry (by firstSeenAge) once
// kMaxReconEntries is full -- the same "keep what's still relevant" shape
// findOrAddRival() uses below, just keyed by recency instead of battle
// count since a signal has no equivalent tally. Returns true the first
// time `idHash` is ever seen (a "new device" bonus for performFeedScan()),
// false on every scan after that.
bool reconLogRecord(uint32_t idHash, uint8_t kind, const char *label) {
  for (uint8_t i = 0; i < reconLog.entryCount; ++i) {
    if (reconLog.entries[i].idHash == idHash) return false;
  }
  ReconEntry entry{};
  entry.idHash = idHash;
  entry.kind = kind;
  entry.firstSeenAge = pet.ageMinutes;
  if (label) {
    strncpy(entry.label, label, sizeof(entry.label) - 1);
  }
  if (reconLog.entryCount < kMaxReconEntries) {
    reconLog.entries[reconLog.entryCount++] = entry;
  } else {
    uint8_t oldestIndex = 0;
    uint32_t oldestAge = 0xFFFFFFFF;
    for (uint8_t i = 0; i < kMaxReconEntries; ++i) {
      if (reconLog.entries[i].firstSeenAge < oldestAge) {
        oldestAge = reconLog.entries[i].firstSeenAge;
        oldestIndex = i;
      }
    }
    reconLog.entries[oldestIndex] = entry;
  }
  ++reconLog.totalUniqueSeen;
  return true;
}

void saveFriendsList() {
  preferences.putBytes("friends", &friendsList, sizeof(friendsList));
}

void loadFriendsList() {
  if (preferences.getBytesLength("friends") == sizeof(friendsList)) {
    preferences.getBytes("friends", &friendsList, sizeof(friendsList));
  }
  if (friendsList.magic != FRIENDS_LIST_MAGIC) {
    friendsList = FriendsList{};
    friendsList.magic = FRIENDS_LIST_MAGIC;
  }
}

// Called on the Idle-state edge into FamiliarBattleState::Exchanged (see
// loop()'s battle-state-change handling, mirroring recordBattleOutcome()'s
// own edge-into-Result call). No-op if already a friend or the list is
// full -- unlike findOrAddRival(), a full list just stops accepting new
// friends rather than evicting an old one: a rival's slot can meaningfully
// go stale (you stopped battling them), but there's no equivalent
// staleness signal for a friend, so evicting one silently on your behalf
// would be a surprising thing for the app to decide alone.
void addFriend(uint32_t playerId) {
  for (uint8_t i = 0; i < friendsList.count; ++i) {
    if (friendsList.friends[i].playerId == playerId) return;
  }
  if (friendsList.count >= kMaxFriends) return;
  FriendEntry &entry = friendsList.friends[friendsList.count++];
  entry.playerId = playerId;
  entry.addedAge = pet.ageMinutes;
  saveFriendsList();
}

void saveInventory() {
  preferences.putBytes("inventory", &inventory, sizeof(inventory));
}

void loadInventory() {
  if (preferences.getBytesLength("inventory") == sizeof(inventory)) {
    preferences.getBytes("inventory", &inventory, sizeof(inventory));
  }
  if (inventory.magic != INVENTORY_MAGIC) {
    inventory = Inventory{};
    inventory.magic = INVENTORY_MAGIC;
  }
}

// Each point of boost this app hands out this way is deliberately small
// (matching addClamped()'s own care-stat scale being 0-100, not this) --
// the cap keeps FEED-farmed items from turning battles into a pure
// time-investment contest.
constexpr uint8_t kItemBonusStep = 2;
constexpr uint8_t kItemBonusCap = 30;

// Consumes one of `item`, applying its effect immediately -- items are used
// the instant they're tapped in drawItemsPage(), not equipped/held for
// later. No-op if none are held. See ItemType's own comment in ui_pages.h
// for why HP/ATK/DEF/SPECIAL are a shared numeric-boost shape and
// TYPE_SHIFT (a genome.element reroll) isn't.
//
// pet.bonusHp/Attack/Defense/Special only ever affect this device's own
// displays and locally-initiated battles (drawBattlePage()'s Idle-state
// stat line, battleCapabilities() for Special) -- a deliberate scope
// decision, not a limitation of how the values could flow: capabilities.
// special is transmitted directly to an opponent already (sendCapabilities())
// so a Special boost would in fact reach a live PvP battle correctly with
// no protocol change, but HP/ATK/DEF are independently *recomputed* by
// each side from transmitted level/stage (deriveMaxHp/deriveAttack/
// deriveDefense), not transmitted as raw numbers -- boosting only this
// device's own myMaxHp_/myAttack_/myDefense_ without also telling the peer
// would desync what each side thinks this pet's stats are. Keeping all
// four consistent (none of them affect a live battle yet) avoids the
// confusing "why did my Special item work in a real battle but not my ATK
// one" asymmetry that treating them differently would create.
void useItem(uint8_t item) {
  if (item >= kItemTypeCount || inventory.counts[item] == 0) return;
  --inventory.counts[item];
  saveInventory();

  if (item == ITEM_TYPE_SHIFT) {
    const uint8_t previous = pet.genome.element;
    uint8_t next = previous;
    while (next == previous) next = esp_random() % 6;
    pet.genome.element = next;
    showToastf("Element shifted to %s", elementName(pet.genome.element));
  } else {
    uint8_t *bonus = item == ITEM_HP_BOOST ? &pet.bonusHp :
                     item == ITEM_ATK_BOOST ? &pet.bonusAttack :
                     item == ITEM_DEF_BOOST ? &pet.bonusDefense : &pet.bonusSpecial;
    *bonus = min<uint8_t>(kItemBonusCap, *bonus + kItemBonusStep);
    const char *labels[] = {"HP", "ATK", "DEF", "SPECIAL"};
    showToastf("%s boosted to +%u", labels[item], *bonus);
  }
  playTone(659, 60, 40);
  playTone(880, 90, 45);
  savePet();
}

// Rolled once per FEED scan that turns up at least one never-seen-before
// device (performFeedScan(), see its own comment) -- a flat chance, not
// scaled by how many new devices were found, so spamming scans in a
// device-dense area isn't strictly better than one scan somewhere new.
constexpr uint8_t kItemDropPercent = 20;

void maybeAwardItem() {
  if (esp_random() % 100 >= kItemDropPercent) return;
  const uint8_t item = esp_random() % kItemTypeCount;
  ++inventory.counts[item];
  saveInventory();
  static const char *names[] = {"HP BOOST", "ATK BOOST", "DEF BOOST", "SPECIAL BOOST",
                                "TYPE SHIFT"};
  showToastf("Mutation trigger found: %s", names[item]);
  playTone(988, 50, 40);
  playTone(1319, 90, 50);
}

// Finds `playerId`'s rival slot, adding a new one if this is a first-time
// opponent. Once all kMaxBattleRivals slots are full, reuses whichever
// rival has the fewest recorded battles -- a simple "keep the rivalries
// you've actually built up" eviction rule rather than tracking timestamps.
BattleRival &findOrAddRival(uint32_t playerId) {
  for (uint8_t i = 0; i < battleStats.rivalCount; ++i) {
    if (battleStats.rivals[i].playerId == playerId) return battleStats.rivals[i];
  }
  if (battleStats.rivalCount < kMaxBattleRivals) {
    BattleRival &rival = battleStats.rivals[battleStats.rivalCount++];
    rival = BattleRival{};
    rival.playerId = playerId;
    return rival;
  }
  uint8_t leastIndex = 0;
  uint32_t leastTotal = 0xFFFFFFFF;
  for (uint8_t i = 0; i < kMaxBattleRivals; ++i) {
    const uint32_t total = static_cast<uint32_t>(battleStats.rivals[i].wins) +
                           battleStats.rivals[i].losses;
    if (total < leastTotal) {
      leastTotal = total;
      leastIndex = i;
    }
  }
  battleStats.rivals[leastIndex] = BattleRival{};
  battleStats.rivals[leastIndex].playerId = playerId;
  return battleStats.rivals[leastIndex];
}

// Called once per battle, on the edge into FamiliarBattleState::Result --
// see loop()'s battle-state-change handling. Only Victory/Defeat update a
// specific rival's record: fleeing or disconnecting isn't a decisive result
// against that opponent, so those only move the aggregate counters.
void recordBattleOutcome(FamiliarBattleOutcome outcome, uint32_t opponentPlayerId) {
  switch (outcome) {
    case FamiliarBattleOutcome::Victory:
      battleStats.wins++;
      if (opponentPlayerId) findOrAddRival(opponentPlayerId).wins++;
      break;
    case FamiliarBattleOutcome::Defeat:
      battleStats.losses++;
      if (opponentPlayerId) findOrAddRival(opponentPlayerId).losses++;
      break;
    case FamiliarBattleOutcome::Fled:
      battleStats.fled++;
      break;
    case FamiliarBattleOutcome::OpponentFled:
      battleStats.opponentFled++;
      break;
    case FamiliarBattleOutcome::Disconnected:
      battleStats.disconnected++;
      break;
    default:
      return;  // None -- shouldn't reach here, but don't spend a flash write on it.
  }
  saveBattleStats();
}

void loadCopiedGenome() {
  hasCopiedGenome = preferences.getBytesLength("copiedGenome") == sizeof(copiedGenome);
  if (hasCopiedGenome) {
    preferences.getBytes("copiedGenome", &copiedGenome, sizeof(copiedGenome));
    strcpy(genomeTransferStatus, "COPIED GENOME READY");
  }
}

void saveCopiedGenome(const PetGenome &genome) {
  copiedGenome = genome;
  hasCopiedGenome = preferences.putBytes("copiedGenome", &copiedGenome,
                                         sizeof(copiedGenome)) == sizeof(copiedGenome);
  strcpy(genomeTransferStatus, hasCopiedGenome ? "COPIED GENOME READY" :
                                                "COPY SAVE FAILED");
}

void saveSettings() {
  preferences.putBytes("settings", &settings, sizeof(settings));
}

void loadSettings() {
  if (preferences.getBytesLength("settings") == sizeof(settings)) {
    preferences.getBytes("settings", &settings, sizeof(settings));
  }
  if (settings.magic == SETTINGS_MAGIC_V1) {
    settings.magic = SETTINGS_MAGIC_V2;
    settings.themeIndex = 0;
  }
  if (settings.magic == SETTINGS_MAGIC_V2) {
    static constexpr uint8_t oldBrightnessToPercent[] = {3, 5, 8, 10};
    settings.brightnessIndex = oldBrightnessToPercent[
        min<uint8_t>(settings.brightnessIndex, 3)];
    settings.magic = SETTINGS_MAGIC;
    saveSettings();
  }
  if (settings.magic != SETTINGS_MAGIC || settings.brightnessIndex > 10 ||
      settings.sleepIndex > 3 || settings.volumeIndex > 4 ||
      settings.wakeMode > 1 || settings.themeIndex > 4) {
    settings = {SETTINGS_MAGIC, 8, 1, true, true, 2, 0, 0};
    saveSettings();
  }
}

void loadPet() {
  const size_t storedSize = preferences.getBytesLength("state");
  if (storedSize == sizeof(pet)) {
    preferences.getBytes("state", &pet, sizeof(pet));
  } else if (storedSize > 0) {
    // PET_MAGIC's own layout immediately before the pedometer feature added
    // stepsTotal -- everyone's stored pet was this shape immediately before
    // today.
    struct StateV4 {
      uint32_t magic;
      uint32_t ageMinutes;
      uint32_t actions;
      uint8_t food;
      uint8_t joy;
      uint8_t energy;
      uint8_t health;
      uint8_t stage;
      uint8_t training;
      uint8_t reserved[2];
      uint8_t bonusHp;
      uint8_t bonusAttack;
      uint8_t bonusDefense;
      uint8_t bonusSpecial;
      PetGenome genome;
    } v4{};
    // PET_MAGIC_V4's own layout, minus the bonusHp/Attack/Defense/Special
    // fields the Items feature added.
    struct StateV3 {
      uint32_t magic;
      uint32_t ageMinutes;
      uint32_t actions;
      uint8_t food;
      uint8_t joy;
      uint8_t energy;
      uint8_t health;
      uint8_t stage;
      uint8_t training;
      uint8_t reserved[2];
      PetGenome genome;
    } v3{};
    struct StateV2 {
      uint32_t magic;
      uint32_t ageMinutes;
      uint32_t actions;
      uint8_t food;
      uint8_t joy;
      uint8_t energy;
      uint8_t health;
      uint8_t stage;
      uint8_t training;
      uint8_t reserved[2];
    } previous{};
    struct OldState {
      uint32_t magic, ageMinutes, actions;
      uint8_t food, joy, energy, health;
    } old{};
    if (storedSize == sizeof(v4)) {
      preferences.getBytes("state", &v4, sizeof(v4));
    } else if (storedSize == sizeof(v3)) {
      preferences.getBytes("state", &v3, sizeof(v3));
    } else if (storedSize == sizeof(previous)) {
      preferences.getBytes("state", &previous, sizeof(previous));
    }
    if (v4.magic == PET_MAGIC_V4) {
      pet = {PET_MAGIC, v4.ageMinutes, v4.actions, v4.food, v4.joy, v4.energy,
             v4.health, v4.stage, v4.training, {v4.reserved[0], v4.reserved[1]},
             v4.bonusHp, v4.bonusAttack, v4.bonusDefense, v4.bonusSpecial, 0,
             v4.genome};
      Serial.println("Pet: existing companion gained a step counter");
    } else if (v3.magic == PET_MAGIC_V3) {
      pet = {PET_MAGIC, v3.ageMinutes, v3.actions, v3.food, v3.joy, v3.energy,
             v3.health, v3.stage, v3.training, {v3.reserved[0], v3.reserved[1]},
             0, 0, 0, 0, 0, v3.genome};
      Serial.println("Pet: existing companion gained battle-stat item slots");
    } else if (previous.magic == PET_MAGIC_V2) {
      pet = {PET_MAGIC, previous.ageMinutes, previous.actions, previous.food,
             previous.joy, previous.energy, previous.health, previous.stage,
             previous.training, {previous.reserved[0], previous.reserved[1]},
             0, 0, 0, 0, 0, generatePetGenome()};
      Serial.println("Pet: existing companion received a persistent genome");
    } else if (storedSize >= sizeof(old)) {
      preferences.getBytes("state", &old, sizeof(old));
    }
    if (old.magic == PET_MAGIC_V1) {
      pet = {PET_MAGIC, old.ageMinutes, old.actions, old.food, old.joy,
             old.energy, old.health, 1, 0, {0, 0}, 0, 0, 0, 0, 0, generatePetGenome()};
    }
  }
  if (pet.magic != PET_MAGIC) {
    pet = {PET_MAGIC, 0, 0, 82, 82, 82, 100, 0, 0, {0, 0}, 0, 0, 0, 0, 0,
           generatePetGenome()};
    Serial.printf("Pet: new %s selected (%s affinity)\n",
                  eggLineageName(pet.genome.lineage),
                  elementName(pet.genome.element));
  }
  // Battle-system bring-up profile. reserved[0] is the pet's level until
  // the persistent progression schema lands.
  pet.reserved[0] = 5;
  savePet();
}

void paintBootBackdrop(const PetPalette &palette) {
  Arduino_GFX *previousDisplay = display;
  display = &pageCanvasB;
  const uint16_t horizon = scaleRgb565(palette.primaryDark, 55);
  for (int16_t y = 0; y < LCD_HEIGHT; ++y) {
    display->drawFastHLine(0, y, LCD_WIDTH,
                           lerpRgb565(COLOR_BACKGROUND, horizon,
                                     static_cast<float>(y) / (LCD_HEIGHT - 1)));
  }
  auto glowBlob = [&](int16_t x, int16_t y, int16_t radius, uint16_t color,
                      uint8_t maxPercent) {
    const uint16_t backdrop = lerpRgb565(COLOR_BACKGROUND, horizon,
                                         static_cast<float>(y) / (LCD_HEIGHT - 1));
    for (int16_t r = radius; r > 4; r -= 5) {
      const float t = 1.0f - static_cast<float>(r) / radius;
      display->fillCircle(x, y, r, lerpRgb565(backdrop, color, t * (maxPercent / 100.0f)));
    }
  };
  glowBlob(90, 130, 120, palette.secondary, 30);
  glowBlob(280, 360, 140, palette.accent, 26);
  display = previousDisplay;
}

void drawBootParticles(const PetPalette &palette, uint32_t frame) {
  constexpr uint8_t COUNT = 22;
  const float rise = frame * 0.6f;
  for (uint8_t i = 0; i < COUNT; ++i) {
    const uint32_t seedWord = pet.genome.seed[i % 4];
    const uint8_t place = (seedWord >> ((i * 5) % 24)) & 0xFF;
    const int16_t x = 26 + (place % (LCD_WIDTH - 52));
    const float speed = 0.35f + (i % 5) * 0.12f;
    const float travel = fmodf(rise * speed + i * 37.0f, 300.0f);
    const int16_t y = 344 - static_cast<int16_t>(travel);
    if (y < 96) continue;
    const float twinkle = (sinf(frame * 0.09f + i * 1.7f) + 1.0f) * 0.5f;
    display->fillCircle(x, y, (i & 3) == 0 ? 2 : 1,
                        lerpRgb565(palette.secondary, palette.glow, twinkle));
  }
}

void drawBootHelix(const PetPalette &palette, uint32_t frame, float progress) {
  const float buildT = bootSmoothstep(0.0f, 0.42f, progress);
  const float convergeT = bootSmoothstep(0.55f, 0.85f, progress);
  constexpr float ANGLE_STEP = 6.2832f / BOOT_NODE_COUNT;

  for (uint8_t node = 0; node < BOOT_NODE_COUNT; ++node) {
    const float normalizedPos = static_cast<float>(node) / (BOOT_NODE_COUNT - 1);
    if (normalizedPos > buildT) continue;

    const float baseY = BOOT_TOP_Y + normalizedPos * (BOOT_BOTTOM_Y - BOOT_TOP_Y);
    const int16_t y = lroundf(baseY + (BOOT_CORE_Y - baseY) * convergeT);
    const float orbit = 60.0f * (1.0f - convergeT);
    const float phase = frame * 0.10f + node * ANGLE_STEP * 2.0f;
    const int16_t offset = lroundf(sinf(phase) * orbit);
    const float depthT = (cosf(phase) + 1.0f) * 0.5f;

    const uint16_t strandA = lerpRgb565(palette.primaryDark, palette.primaryLight, normalizedPos);
    const uint16_t strandB = lerpRgb565(palette.secondary, palette.accent, normalizedPos);
    const int16_t xA = BOOT_CX - offset, xB = BOOT_CX + offset;

    if (orbit > 6.0f) {
      display->drawLine(xA, y, xB, y, scaleRgb565(palette.primaryDark, 65));
    }
    display->fillCircle(xA, y, lroundf(2 + depthT * 3), scaleRgb565(strandA, 55 + lroundf(depthT * 45)));
    display->fillCircle(xB, y, lroundf(2 + (1.0f - depthT) * 3),
                        scaleRgb565(strandB, 55 + lroundf((1.0f - depthT) * 45)));
  }
}

void drawBootCore(const PetPalette &palette, uint32_t frame, float progress) {
  const float convergeT = bootSmoothstep(0.55f, 0.85f, progress);
  const float pulse = (sinf(frame * 0.18f) + 1.0f) * 0.5f;
  const int16_t radius = 8 + lroundf(convergeT * 22) + lroundf(pulse * (2 + convergeT * 5));

  display->fillCircle(BOOT_CX, BOOT_CORE_Y, radius + 14, scaleRgb565(palette.glow, 16));
  display->fillCircle(BOOT_CX, BOOT_CORE_Y, radius + 7, scaleRgb565(palette.glow, 30));
  display->fillCircle(BOOT_CX, BOOT_CORE_Y, radius, lerpRgb565(palette.primary, palette.glow, pulse));
  display->fillCircle(BOOT_CX, BOOT_CORE_Y, max<int16_t>(3, radius - 8), palette.primaryLight);

  if (convergeT > 0.6f) {
    for (uint8_t ring = 0; ring < 2; ++ring) {
      const float ringPhase = fmodf(frame * 0.035f + ring * 0.5f, 1.0f);
      const int16_t ringRadius = radius + 10 + lroundf(ringPhase * 76);
      display->drawCircle(BOOT_CX, BOOT_CORE_Y, ringRadius,
                          lerpRgb565(palette.glow, COLOR_BACKGROUND, ringPhase));
    }
  }
}

void drawBootTitle(float progress) {
  const float fadeT = bootSmoothstep(0.0f, 0.14f, progress);
  drawCentered("DIGIPET", 30, 3, lerpRgb565(COLOR_BACKGROUND, COLOR_TEXT, fadeT));
  drawCentered("GENETIC LIFE INTERFACE", 66, 1, lerpRgb565(COLOR_BACKGROUND, COLOR_CYAN, fadeT));
}

void drawBootStatus(float progress) {
  constexpr int16_t barX = 41, barY = 372, barW = 286, barH = 8;
  display->fillRoundRect(barX, barY, barW, barH, 4, COLOR_CARD);
  const uint16_t filled = static_cast<uint16_t>(barW * progress);
  if (filled > 2) {
    display->fillRoundRect(barX, barY, filled, barH, 4, lerpRgb565(COLOR_PURPLE, COLOR_CYAN, progress));
    display->fillCircle(barX + filled - 1, barY + barH / 2, 6, COLOR_CYAN);
    display->fillCircle(barX + filled - 1, barY + barH / 2, 3, RGB565_WHITE);
  }

  // Network/time status is no longer reported here: sync now runs as its
  // own visible phase after the animation instead of polled mid-frame,
  // which used to cause visible stutter during the sequence.
  const char *phaseText = progress < 0.30f ? "READING GENOME" :
                          progress < 0.58f ? "ASSEMBLING FORM" :
                          progress < 0.90f ? "STABILISING SIGNAL" :
                                             "LINK READY";
  drawCentered(phaseText, 410, 1, progress > 0.89f ? COLOR_MINT : COLOR_MUTED);
}

// Eases the helix/core into the resting badge logo instead of hard-cutting
// to it, so the sequence resolves as one continuous motion.
void bootOutro(const PetPalette &palette) {
  constexpr uint8_t FRAMES = 22;
  const uint16_t startWash = scaleRgb565(palette.primaryDark, 55);
  for (uint8_t f = 1; f <= FRAMES; ++f) {
    const float eased = bootSmoothstep(0.0f, 1.0f, static_cast<float>(f) / FRAMES);
    Arduino_GFX *previousDisplay = display;
    display = &pageCanvasA;

    display->fillScreen(lerpRgb565(startWash, COLOR_BACKGROUND, eased));
    const int16_t ringOuter = lroundf(78 + eased * 23);
    display->fillCircle(BOOT_CX, 202, lroundf(66 + eased * 22), COLOR_CARD);
    display->drawCircle(BOOT_CX, 202, ringOuter, COLOR_CYAN);
    display->drawCircle(BOOT_CX, 202, ringOuter + 10, COLOR_PURPLE);
    display->fillCircle(BOOT_CX, 202, lroundf(40 + eased * 14), lerpRgb565(palette.primary, palette.glow, eased));
    display->fillCircle(BOOT_CX, 202, lroundf(24 + eased * 10), COLOR_BACKGROUND);
    display->fillCircle(BOOT_CX, 202, lroundf(8 + eased * 7), COLOR_TEXT);

    drawCentered("DIGIPET", 315, 5, lerpRgb565(COLOR_BACKGROUND, COLOR_TEXT, eased));
    drawCentered("LIFE SIGNAL ONLINE", 377, 2, lerpRgb565(COLOR_BACKGROUND, COLOR_CYAN, eased));

    display = previousDisplay;
    panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);
    delay(18);
  }
  // The boot clip (started in bootAnimation/bootAnimationFallback) already
  // covers this outro; nothing further to play here.
}

// Direct-to-panel fallback for the rare case PSRAM canvases aren't
// available; functionally the same beats as the smooth path, just drawn
// straight to the panel without a framebuffer behind it.
void bootAnimationFallback() {
  const uint32_t animationStarted = millis();
  constexpr uint32_t BOOT_DURATION_MS = 5000;
  panel->setBrightness(180);
  display->fillScreen(COLOR_BACKGROUND);
  drawCentered("DIGIPET", 23, 3, COLOR_TEXT);
  drawCentered("GENETIC LIFE INTERFACE", 57, 1, COLOR_CYAN);
  display->drawRoundRect(24, 88, 320, 260, 30, COLOR_CARD);
  display->drawRoundRect(31, 95, 306, 246, 25, COLOR_PURPLE);
  playBootClipAsync();

  for (uint8_t frame = 0; millis() - animationStarted < BOOT_DURATION_MS; ++frame) {
    const uint32_t elapsed = millis() - animationStarted;
    const float progress = min(1.0f, elapsed / static_cast<float>(BOOT_DURATION_MS));
    display->fillRoundRect(38, 102, 292, 232, 20, COLOR_BACKGROUND);

    const int16_t coreY = 210;
    const int16_t orbit = 58 - static_cast<int16_t>(progress * 18);
    for (uint8_t node = 0; node < 10; ++node) {
      const float phase = frame * 0.13f + node * 0.6283f;
      const int16_t y = 126 + node * 19;
      const int16_t offset = lroundf(sinf(phase) * orbit);
      const uint16_t nearColor = cosf(phase) > 0 ? COLOR_MINT : COLOR_PURPLE;
      display->drawLine(184 - offset, y, 184 + offset, y, COLOR_CARD);
      display->fillCircle(184 - offset, y, 5, nearColor);
      display->fillCircle(184 + offset, y, 5, COLOR_CYAN);
    }

    const int16_t ringRadius = 30 + static_cast<int16_t>(progress * 70);
    display->drawCircle(184, coreY, ringRadius, COLOR_CYAN);
    display->drawCircle(184, coreY, max<int16_t>(8, ringRadius - 7), COLOR_CARD);
    display->fillCircle(184, coreY, 12 + lroundf(sinf(frame * 0.22f) * 3), COLOR_MINT);

    const uint16_t progressWidth = static_cast<uint16_t>(286 * progress);
    display->fillRoundRect(41, 315, 286, 7, 3, COLOR_CARD);
    if (progressWidth) display->fillRoundRect(41, 315, progressWidth, 7, 3, COLOR_CYAN);
    const char *phaseText = progress < 0.28f ? "READING GENOME" :
                            progress < 0.62f ? "ASSEMBLING FORM" :
                            progress < 0.90f ? "STABILISING SIGNAL" :
                                               "LINK READY";
    display->fillRect(80, 361, 208, 18, COLOR_BACKGROUND);
    drawCentered(phaseText, 364, 1, progress > 0.89f ? COLOR_MINT : COLOR_MUTED);
    delay(35);
  }

  display->fillScreen(COLOR_BACKGROUND);
  display->fillCircle(184, 202, 88, COLOR_CARD);
  display->drawCircle(184, 202, 91, COLOR_CYAN);
  display->drawCircle(184, 202, 101, COLOR_PURPLE);
  display->fillCircle(184, 202, 54, COLOR_MINT);
  display->fillCircle(184, 202, 38, COLOR_BACKGROUND);
  display->fillCircle(184, 202, 15, COLOR_TEXT);
  drawCentered("DIGIPET", 315, 5, COLOR_TEXT);
  drawCentered("LIFE SIGNAL ONLINE", 377, 2, COLOR_CYAN);
  // The boot clip started above already covers this outro.
}

void bootAnimation() {
  if (!transitionsReady) {
    bootAnimationFallback();
    return;
  }

  const uint32_t animationStarted = millis();
  constexpr uint32_t BOOT_DURATION_MS = 5200;
  const PetPalette palette = paletteForGenome(pet.genome);
  panel->setBrightness(180);
  paintBootBackdrop(palette);
  pinMode(0, INPUT_PULLUP);  // lets a boot-key press fast-forward the sequence
  playBootClipAsync();

  const size_t frameBytes = static_cast<size_t>(LCD_WIDTH) * LCD_HEIGHT * sizeof(uint16_t);
  float progress = 0.0f;
  bool skipping = false;
  for (uint32_t frame = 0; ; ++frame) {
    if (!skipping) {
      const uint32_t elapsed = millis() - animationStarted;
      progress = min(1.0f, elapsed / static_cast<float>(BOOT_DURATION_MS));
      if (elapsed > 250 && digitalRead(0) == LOW) skipping = true;
    } else {
      progress = min(1.0f, progress + 0.05f);
    }

    Arduino_GFX *previousDisplay = display;
    display = &pageCanvasA;
    memcpy(pageCanvasA.getFramebuffer(), pageCanvasB.getFramebuffer(), frameBytes);
    drawBootParticles(palette, frame);
    drawBootHelix(palette, frame, progress);
    drawBootCore(palette, frame, progress);
    drawBootTitle(progress);
    drawBootStatus(progress);
    display = previousDisplay;
    panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);

    if (progress >= 1.0f) break;
    delay(18);
  }

  bootOutro(palette);
}

bool i2cPresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

// A single targeted probe for just the audio codec, so the boot jingle can
// start without waiting on the full 126-address scan below (deferred until
// after the boot animation, since nothing it finds is needed before then).
void detectAudioHardware() {
  codecDetected = i2cPresent(0x18);  // ES8311
}

void detectHardware() {
  i2cDeviceCount = 0;
  Serial.println("I2C hardware scan:");
  for (uint8_t address = 1; address < 127; address++) {
    if (i2cPresent(address)) {
      i2cDeviceCount++;
      Serial.printf("  found 0x%02X\n", address);
    }
  }
  imuDetected = i2cPresent(0x6A) || i2cPresent(0x6B);  // QMI8658
  rtcDetected = i2cPresent(0x51);                       // PCF85063
  pmuDetected = i2cPresent(0x34);                       // AXP2101
  codecDetected = i2cPresent(0x18);                     // ES8311
  Serial.printf("QMI8658:%s  RTC:%s  PMU:%s  CODEC:%s\n",
                imuDetected ? "yes" : "no", rtcDetected ? "yes" : "no",
                pmuDetected ? "yes" : "no", codecDetected ? "yes" : "no");
}

void flashPressedHighlight(int16_t x, int16_t y, int16_t w, int16_t h, int16_t radius) {
  display->drawRoundRect(x, y, w, h, radius, RGB565_WHITE);
  display->drawRoundRect(x + 1, y + 1, w - 2, h - 2, max<int16_t>(1, radius - 1), RGB565_WHITE);
}

void beginNewEgg(uint8_t mode) {
  PetGenome nextGenome = generatePetGenome();
  if (mode == 2 && hasCopiedGenome) nextGenome = copiedGenome;
  if (mode == 3 && hasCopiedGenome)
    nextGenome = blendPetGenomes(pet.genome, copiedGenome);

  // A companion that reached the final stage has a 50% chance to pass half
  // its item-earned battle stats on to the next egg -- reaching stage 4 is
  // the bar, not a guarantee of inheritance itself, so hatching still
  // feels like starting over most of the time rather than every stat grind
  // compounding forever across generations.
  uint8_t inheritedHp = 0, inheritedAttack = 0, inheritedDefense = 0, inheritedSpecial = 0;
  if (pet.stage == 4 && esp_random() % 100 < 50) {
    inheritedHp = pet.bonusHp / 2;
    inheritedAttack = pet.bonusAttack / 2;
    inheritedDefense = pet.bonusDefense / 2;
    inheritedSpecial = pet.bonusSpecial / 2;
  }
  const bool inherited =
      inheritedHp || inheritedAttack || inheritedDefense || inheritedSpecial;

  pet = {PET_MAGIC, 0, 0, 82, 82, 82, 100, 0, 0, {5, 0},
         inheritedHp, inheritedAttack, inheritedDefense, inheritedSpecial, 0,
         nextGenome};
  savePet();
  if (settings.themeIndex == 0) applyTheme();
  newEggConfirmation = false;
  pendingHatchMode = 0;
  newEggConfirmationUntil = 0;
  showToastf(inherited ? "%s signal acquired -- stats inherited!"
                       : "%s signal acquired",
             eggLineageName(pet.genome.lineage));
  playTone(392, 70, 40);
  playTone(523, 70, 45);
  playTone(784, 130, 50);
  Serial.printf("Genome Lab: started %s, element=%s\n",
                eggLineageName(pet.genome.lineage),
                elementName(pet.genome.element));
}


void presentGenomeProfilePage() {
  if (transitionsReady) {
    Arduino_GFX *previousDisplay = display;
    display = &pageCanvasA;
    drawGenomeProfilePage();
    display = previousDisplay;
    panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);
  } else {
    drawGenomeProfilePage();
  }
}

void drawPlayerIdPage() {
  paintPageBackdrop();
  drawCentered("PLAYER IDENTITY", 22, 3, COLOR_MINT);
  drawCentered("SHA-256 // BOOT SESSION", 58, 1, COLOR_CYAN);
  drawPanelGlow(20, 93, 328, 218, 24, COLOR_PURPLE);
  display->fillRoundRect(20, 93, 328, 218, 24, COLOR_CARD);
  display->drawRoundRect(27, 100, 314, 204, 19, COLOR_PURPLE);
  display->setTextSize(2);
  display->setTextColor(COLOR_TEXT);
  for (uint8_t row = 0; row < 4; ++row) {
    char segment[17]{};
    memcpy(segment, playerIdHex + row * 16, 16);
    display->setCursor(88, 127 + row * 42);
    display->print(segment);
  }
  display->setTextSize(1);
  display->setTextColor(COLOR_MUTED);
  display->setCursor(45, 330);
  display->printf("UTC SOURCE: %llu", static_cast<unsigned long long>(playerIdTimestamp));
  display->fillRoundRect(84, 375, 200, 48, 15, COLOR_CYAN);
  drawCentered("BACK TO SETTINGS", 393, 2, readableTextColor(COLOR_CYAN));
}

void drawUpdatePage(const char *status, int progress) {
  paintPageBackdrop();
  drawCentered("SIGNED UPDATE", 30, 3, COLOR_MINT);
  drawCentered("DIGIPET OTA // ECDSA P-256", 68, 1, COLOR_CYAN);
  drawPanelGlow(24, 110, 320, 208, 26, COLOR_PURPLE);
  display->fillRoundRect(24, 110, 320, 208, 26, COLOR_CARD);
  display->drawRoundRect(32, 118, 304, 192, 20, COLOR_PURPLE);
  display->drawCircle(184, 184, 43, COLOR_CYAN);
  display->drawCircle(184, 184, 29, COLOR_MINT);
  display->fillCircle(184, 184, 10, progress >= 0 ? COLOR_MINT : COLOR_WARNING);
  drawCentered(status, 249, 1, COLOR_TEXT);
  if (progress >= 0) {
    display->drawRoundRect(52, 280, 264, 17, 7, COLOR_MUTED);
    if (progress) display->fillRoundRect(55, 283, 258 * progress / 100, 11, 5, COLOR_MINT);
    char percent[8];
    snprintf(percent, sizeof(percent), "%d%%", progress);
    drawCentered(percent, 332, 2, COLOR_CYAN);
  } else {
    drawCentered("TAP TO RETURN", 343, 2, COLOR_CYAN);
  }
  display->setTextSize(1);
  display->setTextColor(COLOR_MUTED);
  display->setCursor(125, 410);
  display->printf("INSTALLED v%s", DIGIPET_VERSION);
}

void presentUpdatePage(const char *status, int progress) {
  if (transitionsReady) {
    Arduino_GFX *previousDisplay = display;
    display = &pageCanvasA;
    drawUpdatePage(status, progress);
    display = previousDisplay;
    panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);
  } else {
    drawUpdatePage(status, progress);
  }
}

void drawEvolutionDebugPage() {
  paintPageBackdrop();
  drawCentered("EVOLUTION DEBUG", 24, 3, COLOR_WARNING);
  drawCentered("DEVELOPMENT CONTROL", 61, 1, COLOR_MUTED);
  drawPanelGlow(24, 96, 320, 174, 24, COLOR_PURPLE);
  display->fillRoundRect(24, 96, 320, 174, 24, COLOR_CARD);
  display->drawRoundRect(31, 103, 306, 160, 19, COLOR_PURPLE);
  drawCentered("CURRENT FORM", 122, 1, COLOR_CYAN);
  drawCentered(STAGE_NAMES[pet.stage], 148, 3, COLOR_TEXT);
  drawCentered(pet.stage < 4 ? "NEXT EXPRESSION" : "FINAL EXPRESSION", 198, 1,
               COLOR_CYAN);
  drawCentered(pet.stage < 4 ? STAGE_NAMES[pet.stage + 1] : "MAXIMUM STAGE",
               222, 2, pet.stage < 4 ? COLOR_MINT : COLOR_WARNING);

  const uint16_t advanceFill = pet.stage < 4 ? COLOR_DANGER : COLOR_MUTED;
  display->fillRoundRect(48, 296, 272, 58, 16, advanceFill);
  drawCentered(pet.stage < 4 ? "ADVANCE STAGE" : "MAX STAGE", 316, 2,
               readableTextColor(advanceFill));
  display->fillRoundRect(92, 375, 184, 47, 14, COLOR_CARD);
  display->drawRoundRect(92, 375, 184, 47, 14, COLOR_CYAN);
  drawCentered("CANCEL", 391, 2, COLOR_TEXT);
}

void debugAdvanceEvolution() {
  if (pet.stage >= 4) return;
  pet.stage++;
  savePet();
  showToastf("Debug form: %s", STAGE_NAMES[pet.stage]);
  playTone(440, 60, 40);
  playTone(659, 70, 45);
  playTone(880, 110, 50);
  Serial.printf("Evolution Debug: advanced to %s\n", STAGE_NAMES[pet.stage]);
}

// One row of a scanned opponent: stage/level identity plus a 3-bar signal
// strength readout from RSSI. No creature portrait here -- their genome
// hasn't been exchanged yet at this point, only their HELLO/advertisement.
void drawBattlePage() {
  if (showingBattleResults) {
    drawBattleResultsPage(battle.scanResults(), battleResultsPage);
    return;
  }

  paintPageBackdrop();
  drawCentered("PVP", 16, 3, COLOR_MINT);

  const FamiliarBattleState state = battle.state();
  // Idle now draws one of its own two full-screen sub-views (see
  // battleShowingActions' own comment in ui_pages.h) instead of a compact
  // header + portrait + buttons all on one screen -- only the three
  // transient connecting-ish states below still want the old compact
  // one-liner for context while they wait.
  const bool showsCompactHeader = state == FamiliarBattleState::Scanning ||
                                  state == FamiliarBattleState::Hosting ||
                                  state == FamiliarBattleState::Connecting;
  if (showsCompactHeader) {
    display->setTextSize(1);
    display->setTextColor(COLOR_CYAN);
    display->setCursor(24, 51);
    // Item bonuses (pet.bonusHp/Attack/Defense) fold in here for display --
    // see useItem()'s own comment on why they don't (yet) travel over BLE
    // into what a real opponent's device computes for these same numbers.
    display->printf("LV 5  HP %u  ATK %u  DEF %u",
                    FamiliarBattleService::deriveMaxHp(5) + pet.bonusHp,
                    FamiliarBattleService::deriveAttack(5, pet.stage) + pet.bonusAttack,
                    FamiliarBattleService::deriveDefense(5, pet.stage) + pet.bonusDefense);
    display->setTextColor(COLOR_MUTED);
    display->setCursor(24, 64);
    display->printf("RECORD %luW-%luL", (unsigned long)battleStats.wins,
                    (unsigned long)battleStats.losses);
  }

  if (state == FamiliarBattleState::Idle) {
    if (battleShowingActions) {
      drawBattleActionsView();
    } else {
      drawBattleStatsView();
    }
  } else if (state == FamiliarBattleState::Scanning) {
    // Effectively unreachable today (beginFind() blocks for its whole scan
    // window before returning, so the UI never renders mid-scan -- see the
    // dedicated screen drawn in handleBattleTap instead) but handled here
    // too rather than silently falling through to the battling layout.
    const PetPalette palette = paletteForGenome(pet.genome);
    drawCreaturePortrait(pet.genome, max<uint8_t>(pet.stage, 1), 184, 184, 70, palette.glow);
    display->drawCircle(184, 184, 82, COLOR_CYAN);
    drawCentered("SCANNING", 288, 2, COLOR_TEXT);
    drawCentered(battle.status().c_str(), 320, 1, COLOR_MUTED);
  } else if (state == FamiliarBattleState::Hosting ||
             state == FamiliarBattleState::Connecting) {
    const PetPalette palette = paletteForGenome(pet.genome);
    display->drawCircle(184, 184, 72, palette.glow);
    display->drawCircle(184, 184, 51, palette.secondary);
    drawCreaturePortrait(pet.genome, max<uint8_t>(pet.stage, 1), 184, 184, 60, palette.glow);
    drawCentered(state == FamiliarBattleState::Hosting ? "SIGNAL OPEN" : "HANDSHAKE",
                 278, 2, COLOR_TEXT);
    drawCentered(battle.status().c_str(), 310, 1, COLOR_MUTED);
    drawCentered("TAP BELOW TO CANCEL", 367, 1, COLOR_WARNING);
  } else if (state == FamiliarBattleState::Exchanged) {
    const PetPalette palette = paletteForGenome(pet.genome);
    drawCreaturePortrait(pet.genome, max<uint8_t>(pet.stage, 1), 184, 170, 70, palette.glow);
    drawCentered("FRIEND ADDED", 260, 2, COLOR_MINT);
    drawCentered(battle.status().c_str(), 296, 1, COLOR_MUTED);
    drawCentered("TAP TO RETURN", 340, 1, COLOR_CYAN);
  } else {
    // The two most recent log lines ("you did this, they did that") rather
    // than one line under a big decorative VS panel.
    const auto &log = battle.log();
    // Truncated tighter than the old single-line 42 chars now that the log
    // renders at text size 2 -- the addLog() messages themselves were also
    // shortened to fit comfortably within this.
    const String line2 = log.empty() ? String("CHOOSE YOUR MOVE")
                                     : log.back().substring(0, 24);
    const String line1 = log.size() >= 2 ? log[log.size() - 2].substring(0, 24) : String("");
    if (fleeArmed && millis() >= fleeArmedUntil) fleeArmed = false;
    PetGenome opponentGenome{};
    const bool haveOpponentGenome = battle.opponentGenomeCodeAvailable() &&
        decodePetGenome(battle.opponentGenomeCode(), opponentGenome);
    drawBattlingLayout(battle.myHp(), battle.myMaxHp(), battle.opponentHp(),
                       battle.opponentMaxHp(), battle.opponent().level,
                       battle.negotiatedCapabilities(), line1.c_str(), line2.c_str(),
                       state == FamiliarBattleState::Result, battle.myMoveSubmitted(),
                       fleeArmed, battle.opponentGenomeCodeAvailable(), battleGenomeCopied,
                       haveOpponentGenome ? &opponentGenome : nullptr);
  }
  drawPageDots(PAGE_BATTLE);
}

// A slide-down banner for `message`/showToast(), drawn on top of whichever
// page just rendered. There's no alpha channel on a 16-bit panel, so unlike
// the boot sequence's color fades this only ever moves -- slide in, hold,
// slide out -- the same trick playSlideTransition() uses for whole pages,
// just applied to one small panel instead of the full frame.
constexpr uint32_t kToastSlideMs = 220;
constexpr uint32_t kToastHoldMs = 1500;
constexpr int16_t kToastWidth = 300;
constexpr int16_t kToastHeight = 44;
constexpr int16_t kToastTargetY = 10;

void drawToastOverlay() {
  if (!toastVisible) return;
  const uint32_t elapsed = millis() - toastShownAt;
  constexpr uint32_t kTotalMs = kToastSlideMs * 2 + kToastHoldMs;
  if (elapsed >= kTotalMs) {
    toastVisible = false;  // Next redraw naturally omits it -- nothing to erase.
    return;
  }

  float progress;
  if (elapsed < kToastSlideMs) {
    progress = bootSmoothstep(0.0f, kToastSlideMs, elapsed);
  } else if (elapsed < kToastSlideMs + kToastHoldMs) {
    progress = 1.0f;
  } else {
    progress = 1.0f - bootSmoothstep(0.0f, kToastSlideMs,
                                     elapsed - kToastSlideMs - kToastHoldMs);
  }

  const int16_t hiddenY = -(kToastHeight + 6);
  const int16_t x = (LCD_WIDTH - kToastWidth) / 2;
  const int16_t y = hiddenY + lroundf((kToastTargetY - hiddenY) * progress);

  drawPanelGlow(x, y, kToastWidth, kToastHeight, 20, COLOR_MINT);
  display->fillRoundRect(x, y, kToastWidth, kToastHeight, 20, COLOR_CARD);
  display->drawRoundRect(x, y, kToastWidth, kToastHeight, 20, COLOR_MINT);
  // Text only once the panel is fully on-screen: Arduino_GFX::drawChar's
  // single-size fast path clips its *bottom* edge per pixel but only
  // rejects the *top* as an all-or-nothing block check, so a glyph merely
  // straddling y=0 still calls writePixelPreclipped() for its still-negative
  // rows -- on a canvas that's a write below the framebuffer's PSRAM
  // allocation, which panics ("Cache error: Dbus write to cache rejected")
  // rather than silently clipping like every shape primitive here does.
  if (y >= 0) drawCenteredInRect(message, x, y, kToastWidth, kToastHeight, 1, COLOR_TEXT);
}

// Shared by drawHome() (draws straight to whatever `display` already is --
// the live panel outside a transition) and renderPageToCanvas() (redirects
// `display` to an offscreen canvas first) so the toast overlay always draws
// last, on top of the freshly-composed page, regardless of which path
// rendered it.
void drawActivePage(Page page) {
  if (page == PAGE_COMPANION) drawCompanionPage();
  else if (page == PAGE_STATUS) drawStatusPage();
  else if (page == PAGE_BATTLE) drawBattlePage();
  else if (page == PAGE_SETTINGS) drawSettingsPage();
  else drawGenomeLabPage();
  drawToastOverlay();
}

void drawHome() {
  drawActivePage(currentPage);
}

void renderPageToCanvas(Page page, Arduino_Canvas &canvas) {
  Arduino_GFX *previousDisplay = display;
  display = &canvas;
  drawActivePage(page);
  display = previousDisplay;
}

// Debug-only: lets a connected dev machine pull the actual rendered
// framebuffer back over serial (`DUMP [bodyType] [lineage] [stage]
// [markingGene] [faceGene] [featureGenes]`), so creature art can be tuned
// against a real screenshot instead of guesswork. Renders into the scratch
// canvas so the live display is never disturbed; every override is applied
// to a copy of the live genome and restored immediately after.
void serviceSerialDebug() {
  if (!transitionsReady || !Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line == "WHOAMI") {
    Serial.printf("WHOAMI stage=%u age=%lu actions=%lu bodyType=%u lineage=%u "
                  "design=%016llX battleW=%lu battleL=%lu battleFled=%lu "
                  "battleOppFled=%lu battleDisc=%lu rivals=%u\n",
                  pet.stage, (unsigned long)pet.ageMinutes,
                  (unsigned long)pet.actions, pet.genome.bodyType,
                  pet.genome.lineage,
                  static_cast<unsigned long long>(petGenomeDesignId(pet.genome)),
                  (unsigned long)battleStats.wins, (unsigned long)battleStats.losses,
                  (unsigned long)battleStats.fled, (unsigned long)battleStats.opponentFled,
                  (unsigned long)battleStats.disconnected, battleStats.rivalCount);
    for (uint8_t i = 0; i < battleStats.rivalCount; ++i) {
      Serial.printf("  rival[%u] id=%08lX W=%u L=%u\n", i,
                    (unsigned long)battleStats.rivals[i].playerId,
                    battleStats.rivals[i].wins, battleStats.rivals[i].losses);
    }
    Serial.flush();
    return;
  }
  if (line.startsWith("DUMPBIOS")) {
    int phase = 0;
    sscanf(line.c_str(), "DUMPBIOS %d", &phase);
    const uint8_t savedCount = bootLogCount;
    BootLogEntry savedLog[BOOT_LOG_MAX];
    memcpy(savedLog, bootLog, sizeof(bootLog));

    bootLogCount = 0;
    if (phase == 1) {
      bootLogPush("LINK", "OK", COLOR_MINT);
      bootLogPush("TIME SYNC", "OK", COLOR_MINT);
      bootLogPush("SYSTEM", "READY", COLOR_MINT);
    } else if (phase == 2) {
      bootLogPush("LINK", "OFFLINE", COLOR_WARNING);
    } else {
      bootLogPush("LINK", "OK", COLOR_MINT);
      bootLogPush("TIME SYNC", "SYNCING", COLOR_CYAN);
      // bootLogSpin() draws straight to the live panel by design; redirect
      // it into the canvas just for this preview so it's capturable too.
      Arduino_GFX *previousDisplay = display;
      display = &pageCanvasA;
      bootLogSpin(2);
      display = previousDisplay;
    }

    Serial.printf("\nFBDUMP %d %d\n", LCD_WIDTH, LCD_HEIGHT);
    Serial.write(reinterpret_cast<uint8_t *>(pageCanvasA.getFramebuffer()),
                 static_cast<size_t>(LCD_WIDTH) * LCD_HEIGHT * sizeof(uint16_t));
    Serial.flush();

    memcpy(bootLog, savedLog, sizeof(bootLog));
    bootLogCount = savedCount;
    return;
  }
  if (line.startsWith("DUMPBATTLE")) {
    // Renders the real drawBattlingLayout() (the same function the live
    // Battling/Result states call) against synthetic opponent data -- a
    // live BLE match needs two physical devices to test at all, so this is
    // the only way to see this layout without one.
    int phase = 0;
    sscanf(line.c_str(), "DUMPBATTLE %d", &phase);
    Arduino_GFX *previousDisplay = display;
    display = &pageCanvasB;
    paintPageBackdrop();
    drawCentered("LINK BATTLE", 16, 3, COLOR_MINT);
    PetGenome other = pet.genome;
    other.lineage = (other.lineage + 3) % 10;
    other.bodyType = (other.bodyType + 2) % 5;
    if (phase == 1) {
      drawBattlingLayout(40, 40, 0, 40, 5, true, "", "Opp: fled!", true, false, false,
                         true, false, &other);
    } else if (phase == 2) {
      drawBattlingLayout(0, 40, 40, 40, 5, false, "", "Defeat...", true, false, false,
                         false, false, nullptr);
    } else if (phase == 4) {
      drawBattlingLayout(24, 40, 31, 40, 5, true, "Opp: ATTACK -6 dmg",
                         "Genome received.", false, true, true, false, false, nullptr);
    } else if (phase == 3) {
      drawBattlingLayout(24, 40, 31, 40, 5, true, "Opp: ATTACK -6 dmg",
                         "Awaiting genome...", false, true, false, false, false, nullptr);
    } else {
      drawBattlingLayout(24, 40, 31, 40, 5, true, "Opp: DEFEND",
                         "You: SPECIAL -9 dmg", false, false, false, false, false, &other);
    }
    drawPageDots(PAGE_BATTLE);
    display = previousDisplay;

    Serial.printf("\nFBDUMP %d %d\n", LCD_WIDTH, LCD_HEIGHT);
    Serial.write(reinterpret_cast<uint8_t *>(pageCanvasB.getFramebuffer()),
                 static_cast<size_t>(LCD_WIDTH) * LCD_HEIGHT * sizeof(uint16_t));
    Serial.flush();
    return;
  }
  if (line.startsWith("DUMPSCAN")) {
    // Preview of the Find picker with synthetic scan results -- a real
    // scan needs a second physical device advertising to find at all.
    int page = 0, count = 5;
    sscanf(line.c_str(), "DUMPSCAN %d %d", &page, &count);
    std::vector<FamiliarBattleOpponent> results;
    const int rssiSamples[] = {-52, -68, -80, -58, -74};
    for (int i = 0; i < count && i < 8; ++i) {
      FamiliarBattleOpponent fake;
      fake.stageIndex = (i % 4) + 1;
      fake.level = 5;
      fake.rssi = rssiSamples[i % 5];
      results.push_back(fake);
    }
    Arduino_GFX *previousDisplay = display;
    display = &pageCanvasB;
    drawBattleResultsPage(results, static_cast<uint8_t>(page));
    display = previousDisplay;

    Serial.printf("\nFBDUMP %d %d\n", LCD_WIDTH, LCD_HEIGHT);
    Serial.write(reinterpret_cast<uint8_t *>(pageCanvasB.getFramebuffer()),
                 static_cast<size_t>(LCD_WIDTH) * LCD_HEIGHT * sizeof(uint16_t));
    Serial.flush();
    return;
  }
  if (line.startsWith("SIMBATTLE")) {
    // Debug-only: exercises recordBattleOutcome() and its NVS persistence
    // without a live match -- the same testing gap DUMPBATTLE/DUMPSCAN
    // cover for rendering, since a real Direct Challenge needs two
    // physical devices to ever reach FamiliarBattleState::Result at all.
    // `SIMBATTLE <outcome 1-5> [opponent playerId hex]`; outcome values
    // match FamiliarBattleOutcome's own numbering (Victory=1, Defeat=2,
    // Fled=3, OpponentFled=4, Disconnected=5).
    int outcomeValue = 0;
    unsigned long opponentId = 0;
    sscanf(line.c_str(), "SIMBATTLE %d %lx", &outcomeValue, &opponentId);
    recordBattleOutcome(static_cast<FamiliarBattleOutcome>(outcomeValue),
                        static_cast<uint32_t>(opponentId));
    Serial.println("SIMBATTLE recorded");
    Serial.flush();
    return;
  }
  if (!line.startsWith("DUMP")) return;

  int bodyType = -1, lineage = -1, stage = -1, markingGene = -1, faceGene = -1,
      featureGenes = -1, page = -1;
  sscanf(line.c_str(), "DUMP %d %d %d %d %d %d %d", &bodyType, &lineage, &stage,
         &markingGene, &faceGene, &featureGenes, &page);

  const PetGenome savedGenome = pet.genome;
  const uint8_t savedStage = pet.stage;
  if (bodyType >= 0) pet.genome.bodyType = static_cast<uint8_t>(bodyType);
  if (lineage >= 0) pet.genome.lineage = static_cast<uint8_t>(lineage);
  if (stage >= 0) pet.stage = static_cast<uint8_t>(stage);
  if (markingGene >= 0) pet.genome.markingGene = static_cast<uint8_t>(markingGene);
  if (faceGene >= 0) pet.genome.faceGene = static_cast<uint8_t>(faceGene);
  if (featureGenes >= 0) pet.genome.featureGenes = static_cast<uint16_t>(featureGenes);

  const Page dumpPage = page == 1 ? PAGE_BATTLE : page == 2 ? PAGE_STATUS :
                        page == 3 ? PAGE_SETTINGS : page == 4 ? PAGE_GENOME_LAB :
                                                                PAGE_COMPANION;
  renderPageToCanvas(dumpPage, pageCanvasB);

  pet.genome = savedGenome;
  pet.stage = savedStage;

  Serial.printf("\nFBDUMP %d %d\n", LCD_WIDTH, LCD_HEIGHT);
  Serial.write(reinterpret_cast<uint8_t *>(pageCanvasB.getFramebuffer()),
               static_cast<size_t>(LCD_WIDTH) * LCD_HEIGHT * sizeof(uint16_t));
  Serial.flush();
}

void presentCoherentPageFrame(Page page) {
  if (!transitionsReady) {
    drawHome();
    return;
  }
  renderPageToCanvas(page, pageCanvasA);
  panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(),
                            LCD_WIDTH, LCD_HEIGHT);
}

// Shared eased slide, used for every full-frame transition in this app
// (page swipes, settings sub-view pushes, settings grid paging, overlay
// open/close) so there is exactly one place that owns the smoothstep
// curve, the frame pacing, and the "new content enters, old content exits"
// compositing. `vertical` selects which axis slides; `enterFromEnd` is true
// when the new frame should slide in from the right/bottom while the old
// one exits toward the left/top (a "forward" swipe), false for the
// reverse. Always measures the actual panel blit time and subtracts it
// from the per-frame delay, so every transition in the app holds the same
// ~1000/frameTimeMs pace regardless of how long that particular blit
// happens to take -- which is also why frameCount can sit as high as 18
// (up from an earlier 14/15): the adaptive delay means a finer-grained
// slide costs total transition time, not per-frame smoothness, so it's a
// free upgrade on hardware fast enough to not need the delay at all, and
// still just a proportionally longer transition on hardware that does.
void playSlideTransition(const uint16_t *from, const uint16_t *to, bool vertical,
                         bool enterFromEnd, uint8_t frameCount = 18,
                         uint32_t frameTimeMs = 17) {
  const int16_t span = vertical ? LCD_HEIGHT : LCD_WIDTH;
  for (uint8_t frame = 1; frame <= frameCount; ++frame) {
    const float linear = static_cast<float>(frame) / frameCount;
    const float eased = linear * linear * (3.0f - 2.0f * linear);
    const int16_t shift = min<int16_t>(span, lroundf(eased * span));
    const int16_t remaining = span - shift;

    if (vertical) {
      if (enterFromEnd) {
        if (remaining) memcpy(transitionFrame, from + static_cast<size_t>(shift) * LCD_WIDTH,
                              static_cast<size_t>(remaining) * LCD_WIDTH * sizeof(uint16_t));
        if (shift) memcpy(transitionFrame + static_cast<size_t>(remaining) * LCD_WIDTH, to,
                          static_cast<size_t>(shift) * LCD_WIDTH * sizeof(uint16_t));
      } else {
        if (shift) memcpy(transitionFrame, to + static_cast<size_t>(remaining) * LCD_WIDTH,
                          static_cast<size_t>(shift) * LCD_WIDTH * sizeof(uint16_t));
        if (remaining) memcpy(transitionFrame + static_cast<size_t>(shift) * LCD_WIDTH, from,
                              static_cast<size_t>(remaining) * LCD_WIDTH * sizeof(uint16_t));
      }
    } else {
      for (int16_t y = 0; y < LCD_HEIGHT; ++y) {
        uint16_t *out = transitionFrame + static_cast<size_t>(y) * LCD_WIDTH;
        const uint16_t *oldRow = from + static_cast<size_t>(y) * LCD_WIDTH;
        const uint16_t *newRow = to + static_cast<size_t>(y) * LCD_WIDTH;
        if (enterFromEnd) {
          if (remaining) memcpy(out, oldRow + shift, remaining * sizeof(uint16_t));
          if (shift) memcpy(out + remaining, newRow, shift * sizeof(uint16_t));
        } else {
          if (shift) memcpy(out, newRow + remaining, shift * sizeof(uint16_t));
          if (remaining) memcpy(out + shift, oldRow, remaining * sizeof(uint16_t));
        }
      }
    }

    const uint32_t frameStarted = millis();
    panel->draw16bitRGBBitmap(0, 0, transitionFrame, LCD_WIDTH, LCD_HEIGHT);
    const uint32_t elapsed = millis() - frameStarted;
    if (elapsed < frameTimeMs) delay(frameTimeMs - elapsed);
  }
}

// Slides a full-screen overlay (Genome Profile, Player ID, Rivals, Evolution
// Debug -- see each one's own comment in ui_pages.h/main.cpp) up from the
// bottom over whatever page it was opened from, using the same eased slide
// as every other transition in the app (playSlideTransition) instead of the
// hard cut these overlays used to pop in with. `fromPage` is rendered as
// the outgoing frame via the usual drawActivePage() dispatch; `drawOverlay`
// draws the incoming one directly (it isn't one of the 5 swipeable pages,
// so it has no Page enum value of its own to hand renderPageToCanvas()).
void presentOverlayEntrance(Page fromPage, void (*drawOverlay)()) {
  if (!transitionsReady) {
    drawOverlay();
    return;
  }
  renderPageToCanvas(fromPage, pageCanvasA);
  Arduino_GFX *previousDisplay = display;
  display = &pageCanvasB;
  drawOverlay();
  display = previousDisplay;
  playSlideTransition(pageCanvasA.getFramebuffer(), pageCanvasB.getFramebuffer(),
                      /*vertical=*/true, /*enterFromEnd=*/true, 18);
}

// The reverse of presentOverlayEntrance(): slides the still-open overlay
// back down and out, revealing `toPage` underneath. `drawOverlay` re-draws
// the overlay's current (already-on-screen) content as the outgoing frame
// rather than reading it back from the live panel, the same "just redraw
// it" approach renderPageToCanvas() itself relies on for every other
// transition here.
void presentOverlayExit(void (*drawOverlay)(), Page toPage) {
  if (!transitionsReady) {
    presentCoherentPageFrame(toPage);
    return;
  }
  Arduino_GFX *previousDisplay = display;
  display = &pageCanvasA;
  drawOverlay();
  display = previousDisplay;
  renderPageToCanvas(toPage, pageCanvasB);
  playSlideTransition(pageCanvasA.getFramebuffer(), pageCanvasB.getFramebuffer(),
                      /*vertical=*/true, /*enterFromEnd=*/false, 18);
}

void transitionSettingsView(SettingsView target, bool forward) {
  if (target == settingsView) return;
  if (!transitionsReady) {
    settingsView = target;
    drawSettingsPage();
    return;
  }

  renderPageToCanvas(PAGE_SETTINGS, pageCanvasA);
  settingsView = target;
  renderPageToCanvas(PAGE_SETTINGS, pageCanvasB);
  playSlideTransition(pageCanvasA.getFramebuffer(), pageCanvasB.getFramebuffer(),
                      /*vertical=*/false, /*enterFromEnd=*/forward, 18);
}

void transitionSettingsGrid(uint8_t target) {
  target = min<uint8_t>(target, 1);
  if (target == settingsGridPage) return;
  if (!transitionsReady) {
    settingsGridPage = target;
    drawSettingsPage();
    return;
  }

  const bool upward = target > settingsGridPage;
  renderPageToCanvas(PAGE_SETTINGS, pageCanvasA);
  settingsGridPage = target;
  renderPageToCanvas(PAGE_SETTINGS, pageCanvasB);
  playSlideTransition(pageCanvasA.getFramebuffer(), pageCanvasB.getFramebuffer(),
                      /*vertical=*/true, /*enterFromEnd=*/upward, 18);
}

// Same "peer sub-views, swipe between them" shape as transitionSettingsGrid()
// just above -- see statusShowingActions' own comment in ui_pages.h.
void transitionStatusView(bool showActions) {
  if (showActions == statusShowingActions) return;
  // Always lands on the grid's first page -- landing wherever it was last
  // left, the way settingsGridPage persists across leaving/re-entering
  // Settings, would mean the summary's own "SWIPE UP FOR ACTIONS" hint
  // sometimes silently skips straight to ITEMS instead of FEED/PLAY/TRAIN,
  // which is a worse surprise than always starting at the top.
  statusActionsPage = 0;
  if (!transitionsReady) {
    statusShowingActions = showActions;
    drawStatusPage();
    return;
  }
  renderPageToCanvas(PAGE_STATUS, pageCanvasA);
  statusShowingActions = showActions;
  renderPageToCanvas(PAGE_STATUS, pageCanvasB);
  playSlideTransition(pageCanvasA.getFramebuffer(), pageCanvasB.getFramebuffer(),
                      /*vertical=*/true, /*enterFromEnd=*/showActions, 18);
}

// Same "peer sub-views, swipe between them" shape as transitionSettingsGrid()
// -- only meaningful while statusShowingActions is true, same relationship
// settingsGridPage has with settingsView == SETTINGS_HOME.
void transitionStatusActionsGrid(uint8_t target) {
  target = min<uint8_t>(target, 1);
  if (target == statusActionsPage) return;
  if (!transitionsReady) {
    statusActionsPage = target;
    drawStatusPage();
    return;
  }
  const bool upward = target > statusActionsPage;
  renderPageToCanvas(PAGE_STATUS, pageCanvasA);
  statusActionsPage = target;
  renderPageToCanvas(PAGE_STATUS, pageCanvasB);
  playSlideTransition(pageCanvasA.getFramebuffer(), pageCanvasB.getFramebuffer(),
                      /*vertical=*/true, /*enterFromEnd=*/upward, 18);
}

// Same "peer sub-views, swipe between them" shape as transitionStatusView()
// -- see battleShowingActions' own comment in ui_pages.h. Only meaningful
// while battle.state() == Idle; the caller (loop()'s touch handling) is
// what already guarantees that.
void transitionBattleView(bool showActions) {
  if (showActions == battleShowingActions) return;
  if (!transitionsReady) {
    battleShowingActions = showActions;
    drawBattlePage();
    return;
  }
  renderPageToCanvas(PAGE_BATTLE, pageCanvasA);
  battleShowingActions = showActions;
  renderPageToCanvas(PAGE_BATTLE, pageCanvasB);
  playSlideTransition(pageCanvasA.getFramebuffer(), pageCanvasB.getFramebuffer(),
                      /*vertical=*/true, /*enterFromEnd=*/showActions, 18);
}

// Same "peer sub-views, swipe between them" shape as transitionStatusView()/
// transitionBattleView() above, for the Genome Profile overlay -- except
// this one isn't one of the 5 swipeable pages, so there's no Page enum
// value to hand renderPageToCanvas(); drawGenomeProfilePage() is called
// directly into each canvas instead, the same way presentOverlayEntrance()
// already does for a *different* page's content underneath an overlay.
void transitionGenomeProfileView(bool showData) {
  if (showData == genomeProfileShowingData) return;
  if (!transitionsReady) {
    genomeProfileShowingData = showData;
    presentGenomeProfilePage();
    return;
  }
  Arduino_GFX *previousDisplay = display;
  display = &pageCanvasA;
  drawGenomeProfilePage();
  genomeProfileShowingData = showData;
  display = &pageCanvasB;
  drawGenomeProfilePage();
  display = previousDisplay;
  playSlideTransition(pageCanvasA.getFramebuffer(), pageCanvasB.getFramebuffer(),
                      /*vertical=*/true, /*enterFromEnd=*/showData, 18);
}

void presentBattlePage() {
  if (transitionsReady) {
    renderPageToCanvas(PAGE_BATTLE, pageCanvasA);
    panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(),
                              LCD_WIDTH, LCD_HEIGHT);
  } else {
    drawBattlePage();
  }
}

void transitionToPage(Page target, int8_t direction) {
  if (!transitionsReady || target == currentPage) {
    currentPage = target;
    drawHome();
    return;
  }

  renderPageToCanvas(currentPage, pageCanvasA);
  currentPage = target;
  renderPageToCanvas(currentPage, pageCanvasB);
  playSlideTransition(pageCanvasA.getFramebuffer(), pageCanvasB.getFramebuffer(),
                      /*vertical=*/false, /*enterFromEnd=*/direction < 0, 18);
}

void updateClockDisplay() {
  if (!clockValid) return;
  struct tm rtcTime{};
  if (!rtcRead(rtcTime)) {
    clockValid = false;
    strcpy(clockText, "--:--");
  } else {
    snprintf(clockText, sizeof(clockText), "%02d:%02d", rtcTime.tm_hour, rtcTime.tm_min);
  }
  if (!sleeping && currentPage == PAGE_COMPANION &&
      !showingEvolutionDebug && !showingGenomeProfile) {
    // A flat COLOR_BACKGROUND fill here doesn't match paintPageBackdrop()'s
    // actual gradient at this Y, which drifts lighter toward the bottom of
    // the screen -- row by row, same as that function does, so this erase
    // is invisible against the real backdrop instead of leaving a
    // faintly-wrong-shade rectangle behind it every second.
    for (int16_t row = 42; row < 64; ++row) {
      display->drawFastHLine(284, row, 68, backdropColorAt(row));
    }
    display->setTextSize(2);
    display->setTextColor(clockValid ? COLOR_TEXT : COLOR_MUTED);
    display->setCursor(288, 45);
    display->print(clockText);
  }
}

void changeSetting(int16_t x, int16_t y) {
  if (y < 78 || y >= 364) return;
  const uint8_t column = x >= LCD_WIDTH / 2;
  const uint8_t row = y >= 227;
  const uint8_t item = settingsGridPage * 4 + row * 2 + column;

  if (item <= 5) {
    transitionSettingsView(static_cast<SettingsView>(item + 1), true);
  } else if (item == 6) {
    showingPlayerId = true;
    presentOverlayEntrance(PAGE_SETTINGS, drawPlayerIdPage);
  } else if (item == 7) {
    showingUpdate = true;
    // Entrance slide only for this first frame -- see presentOverlayExit's
    // own comment on why Update's close (and, by the same reasoning, its
    // repeated in-place progress updates below) stays a hard cut instead.
    if (transitionsReady) {
      renderPageToCanvas(PAGE_SETTINGS, pageCanvasA);
      Arduino_GFX *previousDisplay = display;
      display = &pageCanvasB;
      drawUpdatePage("STARTING SECURE CHECK", 0);
      display = previousDisplay;
      playSlideTransition(pageCanvasA.getFramebuffer(), pageCanvasB.getFramebuffer(),
                          /*vertical=*/true, /*enterFromEnd=*/true, 18);
    } else {
      presentUpdatePage("STARTING SECURE CHECK", 0);
    }
    const OtaResult otaResult = performSignedOta(
        networkConfig.ssid, networkConfig.password,
        [](const char *status, int progress) { presentUpdatePage(status, progress); });
    presentUpdatePage(otaResultMessage(otaResult), -1);
    lastInteraction = millis();
  }
}

// Same tile-grid hit-test shape as changeSetting() just above, applied to
// the Status page's action grid (see statusShowingActions' own comment in
// ui_pages.h) instead of Settings' home grid -- one page's worth of tiles
// here, not two, so no settingsGridPage-style page offset is needed.
void handleStatusGridTap(int16_t x, int16_t y) {
  if (y < 78 || y >= 364) return;
  const uint8_t column = x >= LCD_WIDTH / 2;
  const uint8_t row = y >= 227;
  const uint8_t tile = statusActionsPage * 4 + row * 2 + column;
  if (tile == 3) {
    showingReconLog = true;
    presentOverlayEntrance(PAGE_STATUS, drawReconLogPage);
  } else if (tile == 4) {
    showingItems = true;
    presentOverlayEntrance(PAGE_STATUS, drawItemsPage);
  } else if (tile >= 5) {
    // Tile 5 (STEPS) is informational, not a locked placeholder like 6/7 --
    // either way, nothing to do on tap.
  } else {
    performAction(tile);
  }
}

void handleSettingsControlTap(int16_t x, int16_t y) {
  if (y >= 375) {
    transitionSettingsView(SETTINGS_HOME, false);
    return;
  }

  bool changed = false;
  if (settingsView == SETTINGS_BRIGHTNESS && y >= 83 && y < 355) {
    const int column = (x - 22) / 113;
    const int row = (y - 83) / 68;
    if (x >= 22 && column >= 0 && column < 3 && row >= 0 && row < 4 &&
        (x - 22) % 113 < 98 && (y - 83) % 68 < 53) {
      const int value = row * 3 + column;
      if (value <= 10) {
        settings.brightnessIndex = value;
        panel->setBrightness(brightnessLevel());
        changed = true;
      }
    }
  } else if (settingsView == SETTINGS_IDLE && y >= 89 && y < 337) {
    const uint8_t value = (y - 89) / 62;
    if ((y - 89) % 62 < 48 && value < 4) {
      settings.sleepIndex = value;
      changed = true;
    }
  } else if ((settingsView == SETTINGS_VOLUME || settingsView == SETTINGS_THEME) &&
             y >= 78 && y < 363) {
    const uint8_t value = (y - 78) / 57;
    if ((y - 78) % 57 < 48 && value < 5) {
      if (settingsView == SETTINGS_VOLUME) {
        settings.volumeIndex = value;
        settings.soundEnabled = value > 0;
        if (audioReady) codecWrite(0x32, CODEC_VOLUMES[value]);
        if (settings.soundEnabled) playTone(880, 100, 70);
      } else {
        settings.themeIndex = value;
        applyTheme();
      }
      changed = true;
    }
  } else if (settingsView == SETTINGS_WAKE) {
    if (y >= 121 && y < 169) { settings.wakeMode = 0; changed = true; }
    if (y >= 190 && y < 238) { settings.wakeMode = 1; changed = true; }
  } else if (settingsView == SETTINGS_BOOT) {
    if (y >= 130 && y < 178) { settings.bootAnimationEnabled = true; changed = true; }
    if (y >= 199 && y < 247) { settings.bootAnimationEnabled = false; changed = true; }
  }

  if (changed) {
    saveSettings();
    presentCoherentPageFrame(PAGE_SETTINGS);
  }
}

void drawSleep() {
  display->fillScreen(RGB565_BLACK);
  display->setTextColor(COLOR_CYAN);
  display->setTextSize(2);
  display->setCursor(animationFrame ? 170 : 174, 226);
  display->print("z");
  display->setTextSize(3);
  display->setCursor(animationFrame ? 193 : 197, 196);
  display->print("Z");
}

void updateCreatureAnimation() {
  if (sleeping) {
    // Keep idle visually quiet: only the two dream marks move.
    display->fillRect(158, 184, 72, 72, RGB565_BLACK);
    display->setTextColor(COLOR_CYAN);
    display->setTextSize(2);
    display->setCursor(animationFrame ? 170 : 174, 226);
    display->print("z");
    display->setTextSize(3);
    display->setCursor(animationFrame ? 193 : 197, 196);
    display->print("Z");
  } else if (currentPage == PAGE_COMPANION) {
    // Compose the entire frame in PSRAM. Directly streaming overlapping
    // primitives to the CO5300 causes visible scanline tearing.
    const uint32_t started = millis();
    presentCoherentPageFrame(PAGE_COMPANION);
    const uint16_t renderTime = millis() - started;
    const uint16_t nextInterval = constrain(renderTime + 6, 40, 90);
    creatureFrameInterval = (creatureFrameInterval * 3 + nextInterval) / 4;
  }
}

uint8_t calculateStage() {
  if (pet.ageMinutes < 2 || pet.actions < 2) return 0;
  if (pet.ageMinutes < 30 || pet.actions < 8) return 1;
  if (pet.ageMinutes < 360 || pet.actions < 40) return 2;
  if (pet.ageMinutes < 1440 || pet.actions < 120 || pet.training < 25) return 3;
  return 4;
}

// Reuses the boot sequence's own visual language (paintBootBackdrop/
// drawBootParticles/drawBootCore, same screen position) rather than
// inventing a second one, so evolving reads as the same kind of "genesis"
// moment hatching/booting already are. Composed entirely in pageCanvasA
// and blitted in one shot per frame -- same double-buffering the rest of
// the app uses -- instead of drawing straight to the live panel.
void playEvolutionAnimation() {
  const PetPalette palette = paletteForGenome(pet.genome);
  Arduino_GFX *previousDisplay = display;
  paintBootBackdrop(palette);
  const size_t frameBytes = static_cast<size_t>(LCD_WIDTH) * LCD_HEIGHT * sizeof(uint16_t);

  // Phase 1: a converging energy burst at the core, echoing the boot
  // sequence's helix-converge finale. Held past drawBootCore's own 0.55
  // convergence threshold throughout so the pulsing rings play the whole
  // phase instead of only kicking in near the end.
  constexpr uint32_t kBurstMs = 850;
  const uint32_t burstStarted = millis();
  for (uint32_t frame = 0; ; ++frame) {
    const uint32_t elapsed = millis() - burstStarted;
    const float progress = min(1.0f, elapsed / static_cast<float>(kBurstMs));
    display = &pageCanvasA;
    memcpy(pageCanvasA.getFramebuffer(), pageCanvasB.getFramebuffer(), frameBytes);
    drawBootParticles(palette, frame);
    drawBootCore(palette, frame, 0.6f + progress * 0.4f);
    drawCentered("DATA EVOLUTION", 60, 3,
                lerpRgb565(COLOR_BACKGROUND, COLOR_TEXT, bootSmoothstep(0.0f, 0.3f, progress)));
    display = previousDisplay;
    panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);
    if (progress >= 1.0f) break;
    delay(18);
  }

  // Phase 2: reveal the evolved creature itself (not just its stage name)
  // growing and fading in at the same spot the core just pulsed at.
  constexpr uint32_t kRevealMs = 600;
  const uint32_t revealStarted = millis();
  for (uint32_t frame = 0; ; ++frame) {
    const uint32_t elapsed = millis() - revealStarted;
    const float progress = min(1.0f, elapsed / static_cast<float>(kRevealMs));
    const float eased = bootSmoothstep(0.0f, 1.0f, progress);
    display = &pageCanvasA;
    memcpy(pageCanvasA.getFramebuffer(), pageCanvasB.getFramebuffer(), frameBytes);
    drawBootParticles(palette, kBurstMs / 18 + frame);
    drawCentered("DATA EVOLUTION", 60, 3, COLOR_TEXT);
    drawCreaturePortrait(pet.genome, pet.stage, BOOT_CX, BOOT_CORE_Y,
                        lroundf(40 + eased * 62),
                        lerpRgb565(COLOR_BACKGROUND, palette.glow, eased));
    drawCentered(STAGE_NAMES[pet.stage], 330, 3,
                lerpRgb565(COLOR_BACKGROUND, COLOR_MINT, eased));
    display = previousDisplay;
    panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);
    if (progress >= 1.0f) break;
    delay(18);
  }
  delay(750);  // Hold on the reveal before the caller returns to normal play.
}

void checkEvolution() {
  const uint8_t newStage = calculateStage();
  if (newStage > pet.stage) {
    pet.stage = newStage;
    savePet();
    playEvolutionAnimation();
    showToastf("Evolved into %s", STAGE_NAMES[pet.stage]);
  }
}

// FNV-1a, used only for hashing a WiFi BSSID into a ReconEntry idHash --
// FamiliarBattleService::scanNearbyBle() needs the exact same constants for
// its own BLE-address hashing but isn't shared code with this: keeping BLE
// fully encapsulated inside that class (see its own comment) is worth a
// second four-line copy of a hash loop this small.
uint32_t fnv1aHash(const uint8_t *data, size_t length) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < length; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

void drawScanningPage(uint8_t phase) {
  paintPageBackdrop();
  drawCentered("SIGNAL SCAN", 40, 3, COLOR_MINT);
  drawCentered("PASSIVE WIFI + BLE SWEEP", 76, 1, COLOR_CYAN);
  drawPanelGlow(24, 120, 320, 190, 26, COLOR_PURPLE);
  display->fillRoundRect(24, 120, 320, 190, 26, COLOR_CARD);
  display->drawRoundRect(32, 128, 304, 174, 20, COLOR_PURPLE);
  drawCentered(phase == 0 ? "WIFI SWEEP" : "BLE SWEEP", 175, 2, COLOR_TEXT);
  drawCentered(phase == 0 ? "READING BEACON FRAMES" : "READING ADVERTISEMENTS",
               208, 1, COLOR_MUTED);
  for (uint8_t i = 0; i < 2; ++i) {
    display->fillCircle(160 + i * 48, 260, 10, i <= phase ? COLOR_MINT : COLOR_MUTED);
  }
  drawCentered("HOLD STILL...", 340, 1, COLOR_WARNING);
}

// Blits drawScanningPage() straight to the panel (through the transition
// canvas pipeline where available, same as presentUpdatePage()) -- called
// once per phase, not on a redraw cadence, since both scans below are
// blocking calls with nothing else running meanwhile to animate against.
void presentScanningPage(uint8_t phase) {
  if (transitionsReady) {
    Arduino_GFX *previousDisplay = display;
    display = &pageCanvasA;
    drawScanningPage(phase);
    display = previousDisplay;
    panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);
  } else {
    drawScanningPage(phase);
  }
}

struct FeedScanResult {
  uint8_t wifiCount = 0;
  uint8_t bleCount = 0;
  uint8_t newCount = 0;
};

// FEED's mechanic: a passive ~4s WiFi beacon scan followed by a passive
// ~4s BLE advertisement scan -- sequential, never simultaneous, because the
// ESP32-S3 has exactly one 2.4GHz radio shared between Wi-Fi and BLE (see
// FamiliarBattleService::beginRadio()'s own comment, which this mirrors).
// Purely passive: WiFi.scanNetworks() only reads public beacon frames and
// scanNearbyBle() only reads public advertisements -- this never
// associates, pairs, connects, or transmits anything at any discovered
// device, the same "just listening" scan any phone already does to list
// nearby networks.
FeedScanResult performFeedScan() {
  FeedScanResult result;
  presentScanningPage(0);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  const int16_t found = WiFi.scanNetworks(false, false, false, 300);
  if (found > 0) {
    result.wifiCount = static_cast<uint8_t>(min<int16_t>(found, 255));
    for (int16_t i = 0; i < found; ++i) {
      const uint8_t *bssid = WiFi.BSSID(i);
      if (bssid != nullptr &&
          reconLogRecord(fnv1aHash(bssid, 6), 0, WiFi.SSID(i).c_str())) {
        ++result.newCount;
      }
    }
  }
  WiFi.scanDelete();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(150);

  presentScanningPage(1);
  const std::vector<uint32_t> bleHashes = battle.scanNearbyBle(4000);
  result.bleCount = static_cast<uint8_t>(min<size_t>(bleHashes.size(), 255));
  for (uint32_t hash : bleHashes) {
    if (reconLogRecord(hash, 1, nullptr)) ++result.newCount;
  }

  ++reconLog.totalScans;
  saveReconLog();
  return result;
}

void performAction(int action) {
  if (action == 0) {
    const FeedScanResult scan = performFeedScan();
    const uint16_t totalSeen = static_cast<uint16_t>(scan.wifiCount) + scan.bleCount;
    const uint8_t deviceGain = static_cast<uint8_t>(min<uint16_t>(totalSeen, 20));
    const uint8_t foodGain = static_cast<uint8_t>(6 + deviceGain + scan.newCount * 3);
    pet.food = addClamped(pet.food, foodGain);
    pet.joy = addClamped(pet.joy, static_cast<uint8_t>(scan.newCount * 2));
    pet.energy = addClamped(pet.energy, 2);
    showToastf("SCAN: %u seen, %u new (+%u food)", totalSeen, scan.newCount, foodGain);
    // Overwrites the toast just set above with its own if it hits -- an
    // item find is the more exciting outcome, worth being the one shown.
    if (scan.newCount > 0) maybeAwardItem();
  } else if (action == 1) {
    if (pet.energy < 8) {
      showToast("Too tired to play");
      drawHome();
      return;
    }
    pet.joy = addClamped(pet.joy, 16);
    pet.energy = subtractClamped(pet.energy, 8);
    pet.food = subtractClamped(pet.food, 3);
    showToast("Connection strengthened");
  } else {
    if (pet.energy < 12) {
      showToast("Needs idle time first");
      drawHome();
      return;
    }
    pet.training = addClamped(pet.training, 4);
    pet.energy = subtractClamped(pet.energy, 12);
    pet.food = subtractClamped(pet.food, 4);
    pet.joy = addClamped(pet.joy, 3);
    showToast("Training complete!");
  }
  playActionSound(action);
  pet.actions++;
  checkEvolution();
  savePet();
  drawHome();
}

void agePet() {
  pet.ageMinutes++;
  // A pocket pedometer minute: a bit of walking staves off this tick's
  // usual food/joy decay (kWalkStepsPerMinute is a low bar on purpose --
  // detection through fabric in a pocket undercounts), and a genuinely
  // active minute has a chance to drop bonus food on top, the way a good
  // walk would. stepsThisMinute itself comes from pollMotion() and isn't
  // persisted; only the running pet.stepsTotal it also updates is.
  constexpr uint32_t kWalkStepsPerMinute = 20;
  constexpr uint32_t kActiveStepsPerMinute = 60;
  const bool walked = stepsThisMinute >= kWalkStepsPerMinute;
  if (stepsThisMinute >= kActiveStepsPerMinute && esp_random() % 4 == 0) {
    pet.food = addClamped(pet.food, 3);
    showToastf("Good walk: +3 food (%lu steps)",
               static_cast<unsigned long>(stepsThisMinute));
  }
  stepsThisMinute = 0;
  if (!walked && pet.ageMinutes % 5 == 0) pet.food = subtractClamped(pet.food, 1);
  if (!walked && pet.ageMinutes % 8 == 0) pet.joy = subtractClamped(pet.joy, 1);
  if (sleeping) {
    pet.energy = addClamped(pet.energy, 5);
    if (pet.health < 100) pet.health = addClamped(pet.health, 1);
  } else if (pet.ageMinutes % 3 == 0) {
    pet.energy = subtractClamped(pet.energy, 1);
  }
  // Low stats lower general vitality; there is deliberately no illness state.
  if (pet.food < 15 || pet.joy < 15 || pet.energy < 5) {
    pet.health = subtractClamped(pet.health, 1);
  } else if (pet.health < 100 && pet.ageMinutes % 10 == 0) {
    pet.health = addClamped(pet.health, 1);
  }
  checkEvolution();
  savePet();
  if (!sleeping) drawHome();
  else if (!screenOff) drawSleep();
}

void enterSleep() {
  playTone(523, 80);
  playTone(392, 120);
  sleeping = true;
  showingEvolutionDebug = false;
  screenOff = false;
  sleepStarted = millis();
  animationFrame = false;
  panel->setBrightness(55);
  drawSleep();
}

void turnScreenOff() {
  if (!sleeping || screenOff) return;
  screenOff = true;
  imuBaselineReady = false;
  panel->setBrightness(0);
  panel->displayOff();
  digitalWrite(AUDIO_PA, LOW);
  audioPaOn = false;
  setCpuFrequencyMhz(kIdleCpuMhz);
  Serial.println("Display: off; lift and selected wake control remain active");
}

void showIdleFromLift() {
  if (!screenOff) return;
  panel->displayOn();
  panel->setBrightness(55);
  screenOff = false;
  sleepStarted = millis();
  animationFrame = false;
  drawSleep();
  Serial.println("Wake: lift detected; showing idle screen");
}

void wakeUp() {
  setCpuFrequencyMhz(fullCpuMhz);
  if (screenOff) panel->displayOn();
  if (!audioPaOn) {
    digitalWrite(AUDIO_PA, HIGH);
    delay(20);  // same settle time initAudio() already waits after this pin
    audioPaOn = true;
  }
  sleeping = false;
  screenOff = false;
  lastInteraction = millis();
  panel->setBrightness(brightnessLevel());
  showToast("Link restored");
  playTone(659, 60);
  playTone(880, 100);
  drawHome();
}

// Reads the accelerometer once per ~90 ms tick (the QMI8658's own 11 Hz
// low-power refresh rate, see initializeLiftSensor()) and drives two
// independent jobs off that one reading:
//
//   - Step detection runs unconditionally, screen on or off, since this is
//     meant to live in a pocket and most walking happens with the display
//     asleep. A footstep is a brief spike in acceleration *magnitude* above
//     a slow-tracked baseline; tracking magnitude rather than a raw axis
//     means it doesn't care which way up the device is riding in a pocket.
//     11 Hz comfortably clears Nyquist for a walking cadence (~1.5-2.5 Hz);
//     it'll undercount a fast run, which is an acceptable simplification
//     for a companion you carry around rather than a sports watch.
//   - Lift-wake only matters, and is only armed, while the screen is off --
//     unchanged from before this function grew a second job.
void pollMotion(uint32_t now) {
  if (!imuReady || now - lastImuRead < 90) return;
  lastImuRead = now;
  float x, y, z;
  if (!readAcceleration(x, y, z)) return;

  const float magnitude = sqrtf(x * x + y * y + z * z);
  if (!stepBaselineReady) {
    stepAccelBaseline = magnitude;
    stepBaselineReady = true;
  } else {
    stepAccelBaseline += (magnitude - stepAccelBaseline) * 0.08f;
    const float deviation = magnitude - stepAccelBaseline;
    if (!stepAboveThreshold && deviation > 0.35f && now - lastStepTime > 250) {
      stepAboveThreshold = true;
      lastStepTime = now;
      stepsThisMinute++;
      pet.stepsTotal++;
    } else if (stepAboveThreshold && deviation < 0.15f) {
      stepAboveThreshold = false;
    }
  }

  if (!screenOff) return;
  if (!imuBaselineReady) {
    lastAccelX = x; lastAccelY = y; lastAccelZ = z;
    imuBaselineReady = true;
    return;
  }
  const float movement = fabsf(x - lastAccelX) + fabsf(y - lastAccelY) + fabsf(z - lastAccelZ);
  lastAccelX = x; lastAccelY = y; lastAccelZ = z;
  if (movement > 0.42f) showIdleFromLift();
}

void pollBootWake(uint32_t now) {
  if (now - lastBootRead < 25) return;
  lastBootRead = now;
  const bool down = digitalRead(0) == LOW;
  if (down && !bootWasDown && sleeping && settings.wakeMode == 1) wakeUp();
  bootWasDown = down;
}

uint32_t battlePlayerId() {
  const uint64_t mac = ESP.getEfuseMac();
  return static_cast<uint32_t>(mac ^ (mac >> 32));
}

FamiliarBattleCapabilities battleCapabilities() {
  FamiliarBattleCapabilities capabilities;
  capabilities.flags = FamiliarCapBodyType | FamiliarCapElement |
      FamiliarCapSpeed | FamiliarCapSpecial | FamiliarCapMoveMatchups;
  capabilities.bodyType = pet.genome.bodyType % 5;
  capabilities.element = pet.genome.element % 6;
  const int bodySpeedBonus = capabilities.bodyType == 2 ? 12 :
                             (capabilities.bodyType == 3 ? -8 : 0);
  capabilities.speed = constrain(20 + pet.genome.limbGene * 55 / 255 +
                                 pet.stage * 6 + bodySpeedBonus, 1, 100);
  // pet.bonusSpecial folds in here -- unlike ATK/DEF/HP (recomputed
  // independently by each side from transmitted level/stage, see
  // useItem()'s own comment), capabilities.special is transmitted directly
  // to a peer as one of these raw fields (sendCapabilities()), so an item
  // boost genuinely does reach a real opponent correctly through this,
  // no protocol change needed.
  capabilities.special = constrain(25 + pet.genome.faceGene + pet.stage * 9 +
                                   pet.training / 4 + pet.bonusSpecial, 1, 100);
  return capabilities;
}

void handleBattleTap(int16_t x, int16_t y) {
  const FamiliarBattleState state = battle.state();
  if (showingBattleResults) {
    const auto &results = battle.scanResults();
    const uint8_t start = battleResultsPage * kBattleResultsPerPage;
    if (y >= 66 && y < 66 + kBattleResultsPerPage * 76) {
      const uint8_t row = (y - 66) / 76;
      const size_t index = start + row;
      // Rows are 68 tall inside each 76 slot -- a tap in the 8px gap still
      // resolves to the nearest row, which is fine here.
      if (index < results.size()) {
        showingBattleResults = false;
        playTone(659, 60, 40);
        // connectTo() blocks through the BLE connect handshake and service
        // discovery (can take a couple of seconds) -- show feedback first
        // so the tap doesn't look ignored while that runs.
        Arduino_GFX *previousDisplay = display;
        if (transitionsReady) display = &pageCanvasA;
        paintPageBackdrop();
        const PetPalette palette = paletteForGenome(pet.genome);
        drawCreaturePortrait(pet.genome, max<uint8_t>(pet.stage, 1), 184, 170, 70,
                             palette.glow);
        drawCentered("CONNECTING...", 260, 2, COLOR_CYAN);
        drawCentered("ESTABLISHING LINK", 296, 1, COLOR_MUTED);
        if (transitionsReady) {
          display = previousDisplay;
          panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);
        }
        battle.connectTo(index);
        presentBattlePage();
      }
    } else if (y >= 385 && y <= 428) {
      showingBattleResults = false;
      playTone(392, 60, 35);
      presentBattlePage();
    }
    return;
  }
  if (state == FamiliarBattleState::Battling && y >= 276 && y <= 382 &&
      !battle.myMoveSubmitted()) {
    // Matches the 2x2 grid in drawBattlingLayout: columns split at the
    // midpoint, rows split at the gap between the two button rows. Row
    // index doubled gives [ATK,DEF]/[SPEC,FLEE] = FamiliarBattleMove's own
    // Attack=0/Defend=1/Special=2/Flee=3 ordering directly.
    const uint8_t col = x < LCD_WIDTH / 2 ? 0 : 1;
    const uint8_t row = y < 329 ? 0 : 1;
    const uint8_t move = row * 2 + col;
    if (move == static_cast<uint8_t>(FamiliarBattleMove::Flee)) {
      if (fleeArmed && millis() < fleeArmedUntil) {
        fleeArmed = false;
        battle.submitMove(FamiliarBattleMove::Flee);
        playTone(220, 90, 40);
        presentBattlePage();
      } else {
        // First tap only arms it -- a second tap on Flee within the window
        // actually flees, so a stray tap can't end the battle by accident.
        fleeArmed = true;
        fleeArmedUntil = millis() + 4000;
        playTone(330, 60, 35);
        presentBattlePage();
      }
    } else {
      fleeArmed = false;
      battle.submitMove(static_cast<FamiliarBattleMove>(move));
      playTone(560 + move * 110, 55, 35);
      presentBattlePage();
    }
  } else if (state == FamiliarBattleState::Result) {
    if (battle.opponentGenomeCodeAvailable() && y >= 292 && x < LCD_WIDTH / 2) {
      PetGenome received{};
      if (decodePetGenome(battle.opponentGenomeCode(), received)) {
        saveCopiedGenome(received);
        battleGenomeCopied = hasCopiedGenome;
        playTone(784, 70, 45);
        playTone(1047, 110, 55);
        presentBattlePage();
      }
    } else {
      battle.end();
      presentBattlePage();
    }
  } else if ((state == FamiliarBattleState::Hosting ||
              state == FamiliarBattleState::Connecting) && y >= 340) {
    battle.end();
    presentBattlePage();
  } else if (state == FamiliarBattleState::Exchanged) {
    battle.end();
    presentBattlePage();
  }
}

// The Battle Idle state's action grid taps (see drawBattleActionsView()'s
// own comment in ui_pages.h for the tile layout) -- only reachable while
// battleShowingActions is true, same relationship handleStatusGridTap()
// has with the Status action grid.
void handleBattleGridTap(int16_t x, int16_t y) {
  if (y < 78 || y >= 364) return;
  const uint8_t column = x >= LCD_WIDTH / 2;
  const uint8_t row = y >= 227;
  const auto tile = static_cast<BattleGridTile>(row * 2 + column);
  if (tile == GRID_RIVALS) {
    showingRivals = true;
    presentOverlayEntrance(PAGE_BATTLE, drawRivalsPage);
    return;
  }
  if (tile == GRID_FRIENDS) {
    showingFriends = true;
    presentOverlayEntrance(PAGE_BATTLE, drawFriendsPage);
    return;
  }

  // tile == GRID_HOST or GRID_FIND -- verbatim what used to be the Idle
  // screen's own HOST/FIND button taps (handleBattleTap), just reached
  // through the grid now instead of two fixed buttons on the same screen
  // the stats used to share.
  const FamiliarBattleCapabilities capabilities = battleCapabilities();
  char genomeCode[PET_GENOME_CODE_LENGTH + 1]{};
  encodePetGenome(pet.genome, genomeCode, sizeof(genomeCode));
  battleGenomeCopied = false;
  fleeArmed = false;
  if (tile == GRID_HOST) {
    battle.beginHost(battlePlayerId(), pet.stage, 5, capabilities, genomeCode);
  } else {
    // beginFind() blocks for its whole scan window, so this is a single
    // static frame rather than an animated one -- still routed through
    // the canvas so it matches the rest of the app instead of the bare
    // direct-to-panel draw this used to be.
    Arduino_GFX *previousDisplay = display;
    if (transitionsReady) display = &pageCanvasA;
    paintPageBackdrop();
    const PetPalette palette = paletteForGenome(pet.genome);
    drawCreaturePortrait(pet.genome, max<uint8_t>(pet.stage, 1), 184, 170, 70, palette.glow);
    drawCentered("SCANNING BATTLE LINKS", 260, 2, COLOR_CYAN);
    drawCentered("4 SECOND SEARCH", 296, 1, COLOR_MUTED);
    if (transitionsReady) {
      display = previousDisplay;
      panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);
    }
    battle.beginFind(battlePlayerId(), pet.stage, 5, capabilities, genomeCode);
    // Shows a picker instead of auto-connecting to whatever was found
    // first -- battleResultsPage was already reset the last time results
    // were cleared (beginFind()) or a pick was made, so it only needs
    // resetting here if this is a fresh search.
    battleResultsPage = 0;
    showingBattleResults = !battle.scanResults().empty();
  }
  presentBattlePage();
}

// The Friends overlay's HOST/FIND buttons (see drawFriendsPage()'s own
// prototype comment) -- hit-test regions key off that function's exact
// 18/190,378 pair of rects. Any other tap closes the overlay, the same
// "tap anywhere to return" Rivals/Recon Log already use.
void handleFriendsTap(int16_t x, int16_t y) {
  if (y >= 378 && y < 432) {
    showingFriends = false;
    const FamiliarBattleCapabilities capabilities = battleCapabilities();
    if (x < LCD_WIDTH / 2) {
      battle.beginHost(battlePlayerId(), pet.stage, 5, capabilities, nullptr,
                       FamiliarBattleMode::FriendExchange);
    } else {
      // Same blocking-scan treatment as the Battle Idle screen's own FIND
      // button (handleBattleTap) -- a static frame while beginFind() blocks
      // for its whole scan window -- but auto-connects to the
      // first/strongest signal instead of showing the battle picker: a
      // friend exchange is a "we're both here right now" interaction, not
      // a pick-an-opponent one, and drawBattleResultsPage's "SELECT
      // OPPONENT"/level framing doesn't fit it.
      Arduino_GFX *previousDisplay = display;
      if (transitionsReady) display = &pageCanvasA;
      paintPageBackdrop();
      const PetPalette palette = paletteForGenome(pet.genome);
      drawCreaturePortrait(pet.genome, max<uint8_t>(pet.stage, 1), 184, 170, 70,
                           palette.glow);
      drawCentered("SCANNING FOR FRIENDS", 260, 2, COLOR_CYAN);
      drawCentered("4 SECOND SEARCH", 296, 1, COLOR_MUTED);
      if (transitionsReady) {
        display = previousDisplay;
        panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);
      }
      battle.beginFind(battlePlayerId(), pet.stage, 5, capabilities, nullptr,
                       FamiliarBattleMode::FriendExchange);
      if (!battle.scanResults().empty()) battle.connectTo(0);
    }
    presentBattlePage();
  } else {
    showingFriends = false;
    presentOverlayExit(drawFriendsPage, PAGE_BATTLE);
  }
}

// Refreshes the still-open Items overlay in place after useItem() changes
// its count/bonus display -- same "plain hard blit, no slide" shape as
// presentGenomeProfilePage()'s own EXPORT/IMPORT-triggered refresh, for the
// same reason: this is a redraw of an already-open overlay, not an
// open/close transition.
void presentItemsPage() {
  if (transitionsReady) {
    Arduino_GFX *previousDisplay = display;
    display = &pageCanvasA;
    drawItemsPage();
    display = previousDisplay;
    panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);
  } else {
    drawItemsPage();
  }
}

// The Items overlay's row taps (see drawItemsPage()'s own prototype
// comment for why this uses an explicit BACK button, y >= 375 here,
// instead of Rivals/Recon Log/Friends' tap-anywhere dismissal) and its
// per-item hit-test rows, keyed off that function's exact kFirstRowY/
// kRowHeight geometry.
void handleItemsTap(int16_t x, int16_t y) {
  if (y >= 375) {
    showingItems = false;
    presentOverlayExit(drawItemsPage, PAGE_STATUS);
    return;
  }
  constexpr int16_t kFirstRowY = 76;
  constexpr int16_t kRowHeight = 52;
  if (y < kFirstRowY) return;
  const uint8_t row = (y - kFirstRowY) / kRowHeight;
  if (row >= kItemTypeCount) return;
  useItem(row);
  presentItemsPage();
}

void setup() {
  fullCpuMhz = getCpuFrequencyMhz();
  Serial.begin(115200);
  delay(100);
  Serial.printf("Digipet firmware v%s\n", DIGIPET_VERSION);
  Serial.printf("Reset reason: %d\n", static_cast<int>(esp_reset_reason()));
  Wire.begin(IIC_SDA, IIC_SCL);
  preferences.begin("digipet", false);
  loadSettings();
  loadPet();
  loadCopiedGenome();
  loadBattleStats();
  loadReconLog();
  loadFriendsList();
  loadInventory();
  applyTheme();
  char genomeSelfTest[PET_GENOME_CODE_LENGTH + 1]{};
  PetGenome decodedSelfTest{};
  const bool genomeCodecReady = encodePetGenome(pet.genome, genomeSelfTest,
                                                 sizeof(genomeSelfTest)) &&
      decodePetGenome(genomeSelfTest, decodedSelfTest) &&
      petGenomeDesignId(decodedSelfTest) == petGenomeDesignId(pet.genome);
  Serial.printf("Genome codec: %s\n", genomeCodecReady ? "ready" : "FAILED");

  if (expander.begin(0x20)) {
    for (uint8_t pin : {0, 1, 2}) {
      expander.pinMode(pin, OUTPUT);
      expander.digitalWrite(pin, LOW);
    }
    delay(20);
    for (uint8_t pin : {0, 1, 2}) expander.digitalWrite(pin, HIGH);
  }

  // Only what the boot jingle needs is checked before the screen wakes up;
  // the full hardware scan and any network use are deferred until after the
  // animation has already played (see below) so nothing before the screen
  // turns on other than the minimum required to init audio.
  detectAudioHardware();
  audioReady = initAudio();

  panel->begin();

  // Stand up the PSRAM framebuffers before the boot animation so it can use
  // the same double-buffered blit path as the rest of the UI's transitions.
  if (psramFound()) {
    const bool firstCanvasReady = pageCanvasA.begin(GFX_SKIP_OUTPUT_BEGIN);
    const bool secondCanvasReady = pageCanvasB.begin(GFX_SKIP_OUTPUT_BEGIN);
    transitionFrame = static_cast<uint16_t *>(
        ps_malloc(static_cast<size_t>(LCD_WIDTH) * LCD_HEIGHT * sizeof(uint16_t)));
    transitionsReady = firstCanvasReady && secondCanvasReady && transitionFrame;
  }
  Serial.printf("PSRAM: %u bytes; smooth transitions: %s\n",
                ESP.getPsramSize(), transitionsReady ? "ready" : "fallback");

  if (settings.bootAnimationEnabled) {
    bootAnimation();
  } else {
    panel->setBrightness(brightnessLevel());
    display->fillScreen(COLOR_BACKGROUND);
    drawCentered("DIGIPET", 196, 4, COLOR_MINT);
    delay(350);
  }
  panel->setBrightness(brightnessLevel());

  // The rest of hardware bring-up and any network/time sync now run as
  // their own visible phase, after the animation has already played
  // uninterrupted rather than polled mid-frame.
  detectHardware();
  runStartupNetworkSync();
  generatePlayerId();
  checkEvolution();

  if (transitionsReady) {
    renderPageToCanvas(currentPage, pageCanvasA);
    panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(),
                              LCD_WIDTH, LCD_HEIGHT);
  } else {
    drawHome();
  }

  if (touch->begin()) {
    touch->IIC_Write_Device_State(
        Arduino_IIC_Touch::Device::TOUCH_DEVICE_INTERRUPT_MODE,
        Arduino_IIC_Touch::Device_Mode::TOUCH_DEVICE_INTERRUPT_PERIODIC);
  }
  pinMode(0, INPUT_PULLUP);
  imuReady = initializeLiftSensor();
  nextBlink = millis() + 2200;
  lastMinute = lastInteraction = lastAnimation = millis();
}

void loop() {
  // Deep idle (asleep, screen off -- the common state once this is riding
  // in a pocket) has nothing that needs sub-25ms responsiveness: touch is
  // the wake source and already gated to its own 25ms poll below, and
  // pollMotion() gates itself to the accelerometer's own ~90ms refresh.
  // Without this, loop() would spin flat-out re-checking those same gates
  // thousands of times between each one actually firing, never giving
  // FreeRTOS's idle task a chance to run -- a real (if modest) bit of idle
  // power back on top of turnScreenOff()'s CPU-frequency drop, which does
  // most of the work.
  if (sleeping && screenOff) delay(15);
  const uint32_t now = millis();
  serviceSerialDebug();
  if (currentPage == PAGE_BATTLE && battle.state() != FamiliarBattleState::Idle) {
    battle.update();
    if (battle.state() != lastBattleState || battle.turnNumber() != lastBattleTurn ||
        battle.myHp() != lastBattleMyHp || battle.opponentHp() != lastBattleOpponentHp) {
      // Edge into Result, not the level -- record exactly once per battle,
      // using the opponent identity/outcome this same tick still has live
      // rather than trying to reconstruct it later from saved state.
      if (battle.state() == FamiliarBattleState::Result &&
          lastBattleState != FamiliarBattleState::Result) {
        recordBattleOutcome(battle.outcome(), battle.opponent().playerId);
      }
      // Same edge-detection shape, for FamiliarBattleMode::FriendExchange's
      // own terminal state instead of a battle's.
      if (battle.state() == FamiliarBattleState::Exchanged &&
          lastBattleState != FamiliarBattleState::Exchanged) {
        addFriend(battle.opponent().playerId);
      }
      lastBattleState = battle.state();
      lastBattleTurn = battle.turnNumber();
      lastBattleMyHp = battle.myHp();
      lastBattleOpponentHp = battle.opponentHp();
      presentBattlePage();
    }
  }
  if (now - lastClockDraw >= 1000) {
    lastClockDraw = now;
    updateClockDisplay();
  }
  if (now - lastMinute >= 60000) {
    lastMinute += 60000;
    agePet();
  }

  if (!sleeping && now - lastInteraction >= SLEEP_TIMEOUTS[settings.sleepIndex]) {
    enterSleep();
  }

  if (sleeping && !screenOff && now - sleepStarted >= 10000) turnScreenOff();
  pollMotion(now);
  pollBootWake(now);

  if (sleeping && !screenOff && now - lastAnimation >= 900) {
    lastAnimation = now;
    animationFrame = !animationFrame;
    updateCreatureAnimation();
  } else if (!sleeping && !showingEvolutionDebug && !showingGenomeProfile &&
             currentPage == PAGE_COMPANION &&
             now - lastAnimation >= creatureFrameInterval) {
    lastAnimation = now;
    animationFrame = !animationFrame;
    updateCreatureAnimation();
  } else if (!sleeping && !showingGenomeProfile &&
             currentPage == PAGE_GENOME_LAB &&
             now - lastAnimation >= 50) {
    lastAnimation = now;
    animationFrame = !animationFrame;
    presentCoherentPageFrame(PAGE_GENOME_LAB);
  } else if (!sleeping && toastVisible && !showingEvolutionDebug &&
             !showingGenomeProfile && !showingPlayerId && !showingUpdate &&
             !showingBattleResults && !showingRivals && !showingReconLog &&
             !showingFriends && !showingItems &&
             (currentPage == PAGE_STATUS || currentPage == PAGE_BATTLE ||
              currentPage == PAGE_SETTINGS) &&
             now - lastAnimation >= 30) {
    // Companion and Genome Lab above already redraw every tick and so pick
    // up the toast for free; the other pages don't otherwise animate, so
    // give the toast's slide its own redraw cadence here while it's up.
    lastAnimation = now;
    presentCoherentPageFrame(currentPage);
  }

  if (newEggConfirmation && now >= newEggConfirmationUntil) {
    newEggConfirmation = false;
    pendingHatchMode = 0;
    if (currentPage == PAGE_GENOME_LAB) drawHome();
  }

  if (now - lastTouchRead < 25) return;
  lastTouchRead = now;
  const int32_t fingers = touch->IIC_Read_Device_Value(
      Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);

  if (fingers > 0) {
    const int32_t x = touch->IIC_Read_Device_Value(
        Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    const int32_t y = touch->IIC_Read_Device_Value(
        Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);

    if (!touchWasDown) {
      touchWasDown = true;
      touchStartedAt = now;
      if (sleeping) {
        touchStartX = touchStartY = -1;
        if (settings.wakeMode == 0) wakeUp();
        return;  // The wake touch never activates a button.
      }
      lastInteraction = now;
      touchStartX = touchLastX = x;
      touchStartY = touchLastY = y;

      // Instant press feedback for the highest-traffic controls -- the
      // Status action grid, the Battle Idle action grid, and the battle
      // move grid. The gate on each branch matches handleStatusGridTap()'s/
      // handleBattleGridTap()'s/handleBattleTap()'s own hit-test bounds
      // exactly, so a highlight always appears whenever that tap will
      // actually fire the action on release.
      if (currentPage == PAGE_STATUS && statusShowingActions && y >= 78 && y < 364) {
        constexpr int16_t kTileX[] = {18, 190};
        constexpr int16_t kTileY[] = {78, 227};
        const uint8_t col = x < LCD_WIDTH / 2 ? 0 : 1;
        const uint8_t row = y < 227 ? 0 : 1;
        flashPressedHighlight(kTileX[col], kTileY[row], 160, 137, 20);
      } else if (currentPage == PAGE_BATTLE &&
                 battle.state() == FamiliarBattleState::Idle &&
                 battleShowingActions && y >= 78 && y < 364) {
        constexpr int16_t kTileX[] = {18, 190};
        constexpr int16_t kTileY[] = {78, 227};
        const uint8_t col = x < LCD_WIDTH / 2 ? 0 : 1;
        const uint8_t row = y < 227 ? 0 : 1;
        flashPressedHighlight(kTileX[col], kTileY[row], 160, 137, 20);
      } else if (currentPage == PAGE_BATTLE &&
                 battle.state() == FamiliarBattleState::Battling &&
                 !battle.myMoveSubmitted() && y >= 276 && y <= 382) {
        constexpr int16_t kColX[] = {24, 194};
        constexpr int16_t kRowY[] = {280, 336};
        const uint8_t col = x < LCD_WIDTH / 2 ? 0 : 1;
        const uint8_t row = y < 329 ? 0 : 1;
        flashPressedHighlight(kColX[col], kRowY[row], 150, 50, 14);
      }
    } else {
      touchLastX = x;
      touchLastY = y;
    }
  } else if (touchWasDown) {
    touchWasDown = false;
    if (touchStartX >= 0) {
      const int32_t deltaX = touchLastX - touchStartX;
      const int32_t deltaY = touchLastY - touchStartY;
      const uint32_t touchDuration = now - touchStartedAt;
      if (showingEvolutionDebug) {
        if (touchLastY >= 285 && touchLastY < 365 && pet.stage < 4) {
          debugAdvanceEvolution();
          showingEvolutionDebug = false;
          presentOverlayExit(drawEvolutionDebugPage, PAGE_COMPANION);
        } else if (touchLastY >= 365) {
          showingEvolutionDebug = false;
          presentOverlayExit(drawEvolutionDebugPage, PAGE_COMPANION);
        }
        touchStartX = touchStartY = touchLastX = touchLastY = -1;
        return;
      }
      if (currentPage == PAGE_COMPANION && touchDuration >= 900 &&
          abs(deltaX) < 30 && abs(deltaY) < 30 &&
          touchStartX >= 105 && touchStartX <= 263 &&
          touchStartY >= 125 && touchStartY <= 305) {
        showingEvolutionDebug = true;
        playTone(262, 80, 35);
        presentOverlayEntrance(PAGE_COMPANION, drawEvolutionDebugPage);
        touchStartX = touchStartY = touchLastX = touchLastY = -1;
        return;
      }
      if (showingPlayerId || showingUpdate) {
        // Update's own close stays a hard cut rather than gaining an
        // entrance/exit slide like the other overlays here -- redoing its
        // exit slide would mean redrawing the OTA result screen from
        // scratch (drawUpdatePage() takes the status/progress it last
        // showed as parameters, unlike every other overlay's parameterless
        // draw function), and that isn't worth the added state just for
        // this one screen's close animation.
        const bool wasPlayerId = showingPlayerId;
        showingPlayerId = false;
        showingUpdate = false;
        if (wasPlayerId) {
          presentOverlayExit(drawPlayerIdPage, PAGE_SETTINGS);
        } else if (transitionsReady) {
          renderPageToCanvas(PAGE_SETTINGS, pageCanvasA);
          panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(),
                                    LCD_WIDTH, LCD_HEIGHT);
        } else {
          drawSettingsPage();
        }
        touchStartX = touchStartY = touchLastX = touchLastY = -1;
        return;
      }
      if (showingRivals) {
        showingRivals = false;
        presentOverlayExit(drawRivalsPage, PAGE_BATTLE);
        touchStartX = touchStartY = touchLastX = touchLastY = -1;
        return;
      }
      if (showingReconLog) {
        showingReconLog = false;
        presentOverlayExit(drawReconLogPage, PAGE_STATUS);
        touchStartX = touchStartY = touchLastX = touchLastY = -1;
        return;
      }
      if (showingFriends) {
        handleFriendsTap(touchLastX, touchLastY);
        touchStartX = touchStartY = touchLastX = touchLastY = -1;
        return;
      }
      if (showingItems) {
        handleItemsTap(touchLastX, touchLastY);
        touchStartX = touchStartY = touchLastX = touchLastY = -1;
        return;
      }
      if (showingGenomeProfile) {
        // Same two-sub-view swipe shape as PAGE_STATUS/PAGE_BATTLE's own
        // vertical-swipe handling -- see genomeProfileShowingData's own
        // comment in ui_pages.h.
        if (abs(deltaY) > 55 && abs(deltaY) > abs(deltaX)) {
          if (deltaY < 0 && !genomeProfileShowingData) {
            playTone(988, 30, 30);
            transitionGenomeProfileView(true);
          } else if (deltaY > 0 && genomeProfileShowingData) {
            playTone(784, 30, 30);
            transitionGenomeProfileView(false);
          }
        } else if (genomeProfileShowingData && touchLastY >= 270 && touchLastY < 316) {
          if (touchLastX < LCD_WIDTH / 2) exportActiveGenome();
          else importCopiedGenome();
          presentGenomeProfilePage();
        } else if ((genomeProfileShowingData && touchLastY >= 393) ||
                   (!genomeProfileShowingData && deltaX > 55)) {
          showingGenomeProfile = false;
          presentOverlayExit(drawGenomeProfilePage, genomeProfileReturnPage);
        }
        touchStartX = touchStartY = touchLastX = touchLastY = -1;
        return;
      }
      if (currentPage == PAGE_SETTINGS && settingsView != SETTINGS_HOME) {
        if (deltaX > 55 && abs(deltaX) > abs(deltaY)) {
          transitionSettingsView(SETTINGS_HOME, false);
        } else if (abs(deltaX) < 35 && abs(deltaY) < 35) {
          handleSettingsControlTap(touchLastX, touchLastY);
        }
        touchStartX = touchStartY = touchLastX = touchLastY = -1;
        return;
      }
      if (currentPage == PAGE_SETTINGS && settingsView == SETTINGS_HOME &&
          abs(deltaY) > 55 &&
          abs(deltaY) > abs(deltaX)) {
        playTone(deltaY < 0 ? 988 : 784, 30, 30);
        transitionSettingsGrid(deltaY < 0 ? 1 : 0);
        touchStartX = touchStartY = touchLastX = touchLastY = -1;
        return;
      }
      if (currentPage == PAGE_STATUS && abs(deltaY) > 55 && abs(deltaY) > abs(deltaX)) {
        // Three linear steps -- summary, actions page 1, actions page 2 --
        // swiped through in sequence, same shape Settings' HOME grid uses
        // for its own two pages, just with the summary as an extra step
        // before the grid rather than the grid being the whole page.
        // Bounded at each end (swipe up already on page 2, or down already
        // on the summary) rather than wrapping.
        // Every branch below -- including the two bounded ends, which
        // change nothing -- redraws unconditionally. A touch-down that
        // landed on a grid tile just before this swipe was recognized
        // already drew flashPressedHighlight()'s highlight straight to the
        // live panel (see that function's own comment on why it bypasses
        // the canvas pipeline); if this gesture then resolves to a bounded
        // no-op, nothing else would ever repaint over that highlight, and
        // it would sit there -- looking exactly like a corrupted button --
        // until some unrelated full redraw happened to land on this same
        // page later.
        if (deltaY < 0) {
          if (!statusShowingActions) {
            playTone(988, 30, 30);
            transitionStatusView(true);
          } else if (statusActionsPage == 0) {
            playTone(988, 30, 30);
            transitionStatusActionsGrid(1);
          } else {
            presentCoherentPageFrame(PAGE_STATUS);
          }
        } else {
          if (statusShowingActions && statusActionsPage == 1) {
            playTone(784, 30, 30);
            transitionStatusActionsGrid(0);
          } else if (statusShowingActions) {
            playTone(784, 30, 30);
            transitionStatusView(false);
          } else {
            presentCoherentPageFrame(PAGE_STATUS);
          }
        }
        touchStartX = touchStartY = touchLastX = touchLastY = -1;
        return;
      }
      if (currentPage == PAGE_BATTLE && !showingBattleResults &&
          battle.state() == FamiliarBattleState::Idle &&
          abs(deltaY) > 55 && abs(deltaY) > abs(deltaX)) {
        // Same two-sub-view swipe shape as PAGE_STATUS above, including the
        // "always redraw, even the bounded ends" reasoning in its own
        // comment just above.
        if (deltaY < 0 && !battleShowingActions) {
          playTone(988, 30, 30);
          transitionBattleView(true);
        } else if (deltaY > 0 && battleShowingActions) {
          playTone(784, 30, 30);
          transitionBattleView(false);
        } else {
          presentCoherentPageFrame(PAGE_BATTLE);
        }
        touchStartX = touchStartY = touchLastX = touchLastY = -1;
        return;
      }
      if (currentPage == PAGE_BATTLE && showingBattleResults &&
          abs(deltaY) > 55 && abs(deltaY) > abs(deltaX)) {
        const uint8_t totalPages = max<uint8_t>(1,
            (battle.scanResults().size() + kBattleResultsPerPage - 1) /
                kBattleResultsPerPage);
        if (deltaY < 0 && battleResultsPage + 1 < totalPages) {
          ++battleResultsPage;
          playTone(988, 30, 30);
          presentBattlePage();
        } else if (deltaY > 0 && battleResultsPage > 0) {
          --battleResultsPage;
          playTone(784, 30, 30);
          presentBattlePage();
        }
        touchStartX = touchStartY = touchLastX = touchLastY = -1;
        return;
      }
      const bool battleLinkActive = currentPage == PAGE_BATTLE &&
                                    (battle.state() != FamiliarBattleState::Idle ||
                                     showingBattleResults);
      if (!battleLinkActive && abs(deltaX) > 60 && abs(deltaX) > abs(deltaY)) {
        // No toast on a page swipe -- the destination page's own title
        // already says where you landed, so a banner on top of it would
        // just be repeated-every-swipe noise rather than useful feedback.
        if (deltaX < 0 && currentPage < PAGE_GENOME_LAB) {
          playTone(988, 30, 35);
          transitionToPage(static_cast<Page>(currentPage + 1), -1);
        } else if (deltaX > 0 && currentPage > PAGE_COMPANION) {
          playTone(784, 30, 35);
          transitionToPage(static_cast<Page>(currentPage - 1), 1);
        }
      } else if (currentPage == PAGE_COMPANION && touchDuration < 900 &&
                 abs(deltaX) < 30 && abs(deltaY) < 30 &&
                 touchStartX >= 55 && touchStartX <= 313 &&
                 touchStartY >= 90 && touchStartY <= 358) {
        genomeProfileReturnPage = PAGE_COMPANION;
        showingGenomeProfile = true;
        // Always opens on the identity sub-view -- see transitionStatusView()'s
        // own comment on why landing wherever it was last left would be a
        // worse surprise than always starting at the top.
        genomeProfileShowingData = false;
        presentOverlayEntrance(PAGE_COMPANION, drawGenomeProfilePage);
      } else if (currentPage == PAGE_STATUS && statusShowingActions) {
        handleStatusGridTap(touchLastX, touchLastY);
      } else if (currentPage == PAGE_SETTINGS) {
        changeSetting(touchLastX, touchLastY);
      } else if (currentPage == PAGE_GENOME_LAB && touchLastY >= 374) {
        const uint8_t mode = min<uint8_t>(3, touchLastX / 119 + 1);
        if (mode > 1 && !hasCopiedGenome) {
          strcpy(genomeTransferStatus, "IMPORT A GENOME FIRST");
          playTone(196, 100, 35);
        } else if (newEggConfirmation && pendingHatchMode == mode &&
                   millis() < newEggConfirmationUntil) {
          beginNewEgg(mode);
          transitionToPage(PAGE_COMPANION, 1);
        } else {
          newEggConfirmation = true;
          pendingHatchMode = mode;
          newEggConfirmationUntil = millis() + 6000;
          playTone(330, 90, 38);
        }
        drawHome();
      } else if (currentPage == PAGE_BATTLE && battle.state() == FamiliarBattleState::Idle &&
                 battleShowingActions) {
        handleBattleGridTap(touchLastX, touchLastY);
      } else if (currentPage == PAGE_BATTLE) {
        handleBattleTap(touchLastX, touchLastY);
      }
    }
    touchStartX = touchStartY = touchLastX = touchLastY = -1;
  }
}
