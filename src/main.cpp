#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <cstdarg>
#include <time.h>
#include <esp32-hal-psram.h>
#include <esp_attr.h>
#include <driver/rtc_io.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>

#include <Adafruit_XCA9554.h>
#include <Arduino_DriveBus_Library.h>
#include <Arduino_GFX_Library.h>

#include "pin_config.h"
#include "familiar_battle_service.h"
#include "game_module.h"
#include "ota_updater.h"
#include "game/vpet/pet_genome.h"
#include "ui_pages.h"


constexpr uint32_t PET_MAGIC_V1 = 0x44504731;
constexpr uint32_t PET_MAGIC_V2 = 0x44504732;
constexpr uint32_t PET_MAGIC_V3 = 0x44504733;  // no bonusHp/Attack/Defense/Special fields yet
// Not part of the normal V1..V3 lineage: the pocket-pedometer feature added
// a stepsTotal field under this magic and shipped to the one device this
// firmware actually runs on, then got pulled back out the same day. Kept
// here solely so loadPet() can downgrade that one device's save gracefully
// instead of failing to recognize it and silently starting a new egg.
constexpr uint32_t PET_MAGIC_PEDOMETER = 0x44504735;
constexpr uint32_t PET_MAGIC = 0x44504734;
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
void turnScreenOff();
// The vpet game module (src/game/vpet/game_vpet.cpp) still owns these --
// forward-declared here because main.cpp keeps calling them directly by
// name for now (the dispatch cutover to game_module.h's gameXxx() hooks is
// a later Stage 1 step; see that header's own comment).
void agePet();
void checkEvolution();
void beginNewEgg(uint8_t mode);
void recordBattleOutcome(FamiliarBattleOutcome outcome, uint32_t opponentPlayerId);
void addFriend(uint32_t playerId);
void handleBattleTap(int16_t x, int16_t y);
void handleBattleGridTap(int16_t x, int16_t y);
void handleFriendsTap(int16_t x, int16_t y, int16_t dx, int16_t dy);
void presentFriendsPage();
void handleItemsTap(int16_t x, int16_t y);
bool exportActiveGenome();
bool importCopiedGenome();
void updateCreatureAnimation();
void loadPet();
void savePet();
void loadBattleStats();
void loadReconLog();
void loadFriendsList();
void loadInventory();
void loadCopiedGenome();
void drawEvolutionDebugPage();
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
// Battery state, read straight off the AXP2101 PMU at I2C address 0x34
// (see updateBatteryReading()). batteryPercent is only meaningful once
// pmuDetected is true and updateBatteryReading() has run at least once
// (setup() does one synchronously so the first Status page view isn't
// stuck at 0%).
bool batteryCharging = false;
bool externalPowerPresent = false;
uint8_t batteryPercent = 0;
uint16_t batteryVoltageMv = 0;
uint32_t lastBatteryPoll = 0;
// Runway estimate for the Status page -- see updateBatteryTimeEstimate()'s
// own comment for why the rate it's derived from lives in RTC memory
// rather than an ordinary global. False whenever there isn't yet a
// trustworthy rate (no synced clock, no observed transition yet) or the
// device is currently charging, in which case "time until empty" isn't a
// meaningful thing to show.
bool batteryTimeEstimateValid = false;
uint32_t batteryMinutesRemaining = 0;
// Minutes since the drain-rate anchor (rtcBatteryTransitionEpoch) was last
// set -- exposed so the Status page can show real wait-time progress
// instead of a static "ESTIMATING..." that looks identical whether it's
// been 1 minute or 3 hours. Connecting a serial monitor to check this any
// other way means plugging in USB, which charges the device and defeats
// the point of watching a discharge estimate -- this has to be readable
// from the screen alone.
uint32_t batteryMinutesSinceTransition = 0;
bool audioReady = false;
bool sdDetected = false;
bool clockValid = false;
bool timeSynced = false;
uint32_t lastClockDraw = 0;
uint32_t lastPwrRead = 0;
uint32_t nextBlink = 0;
uint32_t blinkUntil = 0;
bool pwrWasDown = false;
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
uint8_t friendsPage = 0;
bool friendDiscoverable = false;

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

// Same shape as codecRead() above, for the AXP2101 PMU at I2C address
// 0x34 (register numbers below are that chip's own, e.g. "register 0x01"
// -- unrelated to the 0x34 device address itself). Deliberately read-only:
// there's no axp2101Write(). The AXP2101 also owns the board's DCDC/LDO
// voltage rails, and this device already boots and runs fine without our
// firmware touching any of that, so updateBatteryReading() below only
// ever reads status/data registers the chip already reports on its own
// power-on defaults (fuel gauge enabled: reg0x18 bit 3; VBAT ADC channel
// enabled: reg0x30 bit 0 -- both 1 out of reset per the AXP2101 datasheet).
uint8_t axp2101Read(uint8_t reg) {
  Wire.beginTransmission(0x34);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom(0x34, 1) != 1) return 0;
  return Wire.read();
}

// reg0x34/reg0x35 are the fuel gauge's own VBAT ADC result (14-bit,
// 1 code = 1mV); reg0xA4 is the E-Gauge's coulomb-counted percentage,
// already 0-100 with no curve-fitting needed on our end; reg0x01 bits
// [2:0] are the charging state machine (0-3 = tri/pre/constant-current/
// constant-voltage charge, 4 = done, 5 = not charging -- see the
// AXP2101 datasheet's REG 01 and REG A4 descriptions).
void updateBatteryReading() {
  if (!pmuDetected) return;
  const uint16_t mv = (static_cast<uint16_t>(axp2101Read(0x34) & 0x3F) << 8) |
                       axp2101Read(0x35);
  if (mv == 0) return;  // failed/garbage read -- keep the last good value
  batteryVoltageMv = mv;
  batteryPercent = min<uint8_t>(axp2101Read(0xA4), 100);
  const uint8_t chargeState = axp2101Read(0x01) & 0x07;
  batteryCharging = chargeState <= 0x03;
  // AXP2101 state 4 means charge complete: current has stopped, but VBUS is
  // still supplying the board. Keep that distinct from "charging" for the
  // status UI while treating it as externally powered for sleep policy.
  externalPowerPresent = chargeState <= 0x04;
}

bool rtcRead(struct tm &value);

// Last-observed-transition percentage/timestamp and the smoothed drain
// rate derived from it, for updateBatteryTimeEstimate() below. RTC_DATA_ATTR
// places these in the ESP32-S3's RTC slow memory -- the one region that
// survives esp_deep_sleep_start() (see enterDeepIdle()), which every
// ordinary global here does not: this device's "sleep" is a full reboot,
// so a plain variable would forget the rate on every single wake, right
// when there'd otherwise be a fresh data point to add to it. Reset only by
// a genuine power-on (battery disconnect or full depletion) -- exactly the
// moment a stale rate should be discarded anyway.
RTC_DATA_ATTR uint8_t rtcBatteryTransitionPercent = 0xFF;  // 0xFF = no history yet
RTC_DATA_ATTR time_t rtcBatteryTransitionEpoch = 0;
RTC_DATA_ATTR float rtcBatteryDrainPercentPerHour = 0.0f;
RTC_DATA_ATTR bool rtcWasCharging = false;

// Updates the drain-rate estimate and, from it, batteryMinutesRemaining --
// called alongside updateBatteryReading() (same two call sites). Samples
// only on an actual percentage change rather than every call: the fuel
// gauge only reports whole percentage points and this device spends most
// of its time asleep between glances, so sampling on a fixed timer would
// see "no change yet" on nearly every call and drag a smoothed average
// toward zero even while genuinely discharging. Sampling on transitions
// instead means every data point reflects a real amount of charge used
// over a real amount of time, however that time was actually split
// between awake and asleep.
void updateBatteryTimeEstimate() {
  if (!pmuDetected || !clockValid) {
    Serial.printf("Battery estimate: skipped (pmuDetected=%d clockValid=%d)\n",
                  pmuDetected, clockValid);
    batteryTimeEstimateValid = false;
    return;
  }
  struct tm nowTm{};
  if (!rtcRead(nowTm)) {
    Serial.println("Battery estimate: skipped (RTC read failed)");
    batteryTimeEstimateValid = false;
    return;
  }
  const time_t nowEpoch = mktime(&nowTm);

  // Charging pushes the percentage up -- a completely different quantity
  // from the load-current drain this estimate is about, not just a
  // negative version of it. The first version of this function blended
  // charging's percent-rising ticks into the very same smoothed average as
  // real discharge ticks; a single charge session could stack up several
  // strongly negative samples that then took many real discharge
  // transitions to wash back out, leaving the estimate stuck invalid (or
  // wrong) for a long stretch after every charge. Restarting clean the
  // moment a charging session ends avoids that entirely, at the cost of
  // needing one fresh discharge transition after every charge before an
  // estimate reappears -- a real trade, but a bounded and honest one,
  // unlike silently-contaminated data.
  if (rtcWasCharging && !batteryCharging) {
    rtcBatteryDrainPercentPerHour = 0.0f;
    rtcBatteryTransitionPercent = batteryPercent;
    rtcBatteryTransitionEpoch = nowEpoch;
    Serial.printf("Battery estimate: charging ended at %u%%, rate reset\n", batteryPercent);
  }
  rtcWasCharging = batteryCharging;

  if (batteryCharging) {
    // Nothing to estimate while charging -- just keep the anchor current
    // so whenever charging ends, the first post-charge transition measures
    // real discharge time only, not time stretching back through the
    // charge too.
    rtcBatteryTransitionPercent = batteryPercent;
    rtcBatteryTransitionEpoch = nowEpoch;
    batteryTimeEstimateValid = false;
    return;
  }

  if (rtcBatteryTransitionPercent == 0xFF) {
    // First sample since the last power-on -- nothing to compare against
    // yet, just establish the starting point.
    rtcBatteryTransitionPercent = batteryPercent;
    rtcBatteryTransitionEpoch = nowEpoch;
    Serial.printf("Battery estimate: first sample this power-on (%u%%)\n", batteryPercent);
  } else if (batteryPercent != rtcBatteryTransitionPercent) {
    const float deltaHours = (nowEpoch - rtcBatteryTransitionEpoch) / 3600.0f;
    if (deltaHours > 0.01f) {  // guards a clock rollback/glitch, not real usage
      const float instantRate =
          (static_cast<float>(rtcBatteryTransitionPercent) - batteryPercent) / deltaHours;
      // Exponential moving average rather than trusting the latest
      // transition alone -- a single transition's timing can be thrown
      // off by whatever the device happened to be doing (screen on vs.
      // deep sleep) right around it.
      rtcBatteryDrainPercentPerHour = rtcBatteryDrainPercentPerHour <= 0.0f
          ? instantRate
          : rtcBatteryDrainPercentPerHour * 0.7f + instantRate * 0.3f;
      Serial.printf("Battery estimate: transition %u%%->%u%% over %.2fh, "
                    "instant=%.2f%%/h, smoothed=%.2f%%/h\n",
                    rtcBatteryTransitionPercent, batteryPercent, deltaHours, instantRate,
                    rtcBatteryDrainPercentPerHour);
    }
    rtcBatteryTransitionPercent = batteryPercent;
    rtcBatteryTransitionEpoch = nowEpoch;
  } else {
    const float hoursSinceAnchor = (nowEpoch - rtcBatteryTransitionEpoch) / 3600.0f;
    Serial.printf("Battery estimate: still %u%%, %.2fh since last transition, "
                  "rate=%.2f%%/h\n",
                  batteryPercent, hoursSinceAnchor, rtcBatteryDrainPercentPerHour);
  }

  batteryMinutesSinceTransition =
      static_cast<uint32_t>((nowEpoch - rtcBatteryTransitionEpoch) / 60);
  batteryTimeEstimateValid = rtcBatteryDrainPercentPerHour > 0.01f;
  if (batteryTimeEstimateValid) {
    const float hoursRemaining = batteryPercent / rtcBatteryDrainPercentPerHour;
    batteryMinutesRemaining = static_cast<uint32_t>(hoursRemaining * 60.0f + 0.5f);
  }
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

uint8_t bcdToDec(uint8_t value) {
  return ((value >> 4) * 10) + (value & 0x0F);
}

uint8_t decToBcd(uint8_t value) {
  return ((value / 10) << 4) | (value % 10);
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

// A deep-sleep resume's counterpart to runStartupNetworkSync() -- unlike a
// cold boot, a resume never otherwise touches the RTC at all, and RAM
// (including clockValid itself) doesn't survive the reboot deep sleep
// actually is, so without this a resume would read clockValid as false
// forever, even when the RTC's own retained time is perfectly fine (it
// keeps time independently of the ESP32's own sleep state; deep sleep
// doesn't touch it). A plain read covers the overwhelming majority of
// resumes at near-zero cost -- no SD card, no WiFi.
//
// The one thing a plain read can't fix -- the RTC chip itself lost power
// (e.g. the whole device fully discharged) and its retained time is gone
// -- falls back to the same SD-card/WiFi/NTP recovery path a cold boot
// uses, but only then, and deliberately ignoring the syncOnBoot
// preference (that setting means "resync every cold boot regardless,"
// which isn't what a resume's one-time recovery attempt is for) --
// otherwise every single ordinary wake would pay for an SD card mount
// just to check a setting it doesn't need. This is also the only
// automatic way the clock ever recovers after a full battery drain: nothing
// else in normal day-to-day use (waking via the power key) ever performs
// a genuine cold boot.
void resyncClockAfterResume() {
  struct tm rtcTime{};
  clockValid = rtcDetected && rtcRead(rtcTime);
  if (clockValid) {
    snprintf(clockText, sizeof(clockText), "%02d:%02d", rtcTime.tm_hour, rtcTime.tm_min);
    Serial.println("RTC: valid retained time found (resume)");
    return;
  }
  Serial.println("RTC: time is invalid or power was lost (resume) -- attempting recovery sync");
  if (!readNetworkConfig()) return;
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
  // AUTO (themeIndex 0) has no fixed palette of its own -- it's derived
  // from whatever game is running (gameApplyAutoTheme(), game_vpet.cpp for
  // this build) instead of console code reaching into game-specific state
  // (PetGenome/paletteForGenome()) directly.
  ThemeColors theme = kThemes[settings.themeIndex];
  if (settings.themeIndex == 0) gameApplyAutoTheme(theme);
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
    // brightnessIndex 5 (~50%, was 8/~80%) and sleepIndex 0 (15s, was 1/30s)
    // -- battery-friendlier starting points for a device running an
    // emissive AMOLED off a 15mAh cell, picked over the previous, brighter/
    // slower-to-sleep defaults specifically for that reason. Both are still
    // just the fresh-install/settings-reset starting point -- Settings'
    // own brightness slider and sleep-timeout picker still change either
    // one at any time exactly as before.
    settings = {SETTINGS_MAGIC, 5, 0, true, true, 2, 0, 0};
    saveSettings();
  }
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

void drawBootParticles(const PetPalette &palette, uint32_t frame, const uint32_t seed[4]) {
  constexpr uint8_t COUNT = 22;
  const float rise = frame * 0.6f;
  for (uint8_t i = 0; i < COUNT; ++i) {
    const uint32_t seedWord = seed[i % 4];
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
  const PetPalette palette = gameBootAnimationPalette();
  uint32_t particleSeed[4];
  gameBootAnimationSeed(particleSeed);
  panel->setBrightness(180);
  paintBootBackdrop(palette);
  pinMode(0, INPUT_PULLUP);  // lets a power-key press fast-forward the sequence
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
    drawBootParticles(palette, frame, particleSeed);
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

// fullScan enumerates all 126 addresses (and logs every hit) -- worth the
// bus time and Serial spam on a genuine cold boot, since it's the only
// window where a board revision with different hardware would ever be
// seen for the first time. A deep-sleep resume is the exact same physical
// board it was 30 seconds ago; re-enumerating everything on every single
// wake (glancing at the pet, most of this device's actual usage) was pure
// waste, so a resume only re-probes the 4 addresses anything here actually
// reads from. i2cDeviceCount stays 0 on a resume -- it's a diagnostic-only
// count (Status page's own diagnostics line), not read by any decision
// logic, so a resume simply not refreshing it costs nothing but a stale
// number on a screen most people never open right after waking the device.
void detectHardware(bool fullScan) {
  if (fullScan) {
    i2cDeviceCount = 0;
    Serial.println("I2C hardware scan:");
    for (uint8_t address = 1; address < 127; address++) {
      if (i2cPresent(address)) {
        i2cDeviceCount++;
        Serial.printf("  found 0x%02X\n", address);
      }
    }
  }
  // 0x6A (QMI8658's alternate address) used to be checked here too, but
  // a live scan against the real board shows nothing answers there --
  // this board's IMU only responds at 0x6B, so it was always a no-op.
  imuDetected = i2cPresent(0x6B);   // QMI8658
  rtcDetected = i2cPresent(0x51);   // PCF85063
  pmuDetected = i2cPresent(0x34);   // AXP2101
  codecDetected = i2cPresent(0x18); // ES8311
  Serial.printf("QMI8658:%s  RTC:%s  PMU:%s  CODEC:%s\n", imuDetected ? "yes" : "no",
                rtcDetected ? "yes" : "no", pmuDetected ? "yes" : "no",
                codecDetected ? "yes" : "no");
}

void flashPressedHighlight(int16_t x, int16_t y, int16_t w, int16_t h, int16_t radius) {
  display->drawRoundRect(x, y, w, h, radius, RGB565_WHITE);
  display->drawRoundRect(x + 1, y + 1, w - 2, h - 2, max<int16_t>(1, radius - 1), RGB565_WHITE);
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
  // PAGE_SETTINGS is the one genuinely generic page (console-owned); every
  // other page's content is a game module's own -- see gameDrawPage()'s
  // own comment in game_module.h.
  if (page == PAGE_SETTINGS) drawSettingsPage();
  else gameDrawPage(page);
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
    gameDrawPage(PAGE_BATTLE);
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
    gameDrawPage(PAGE_BATTLE);
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
  if (!sleeping && currentPage == PAGE_COMPANION && !gameOverlayActive()) {
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

  // item == 3 (Wake Mode) is a locked placeholder now -- see
  // drawSettingsTile()'s own call site for why -- so it's deliberately
  // excluded here even though it still falls in the item <= 5 range.
  if (item <= 5 && item != 3) {
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

void enterSleep() {
  // Refresh at the decision boundary so a cable connected during the
  // ordinary 30-second polling interval cannot be missed.
  updateBatteryReading();
  playTone(523, 80);
  playTone(392, 120);
  sleeping = true;
  showingEvolutionDebug = false;
  screenOff = false;
  animationFrame = false;
  // Touch reads stop entirely while sleeping (see loop()), so a touch that
  // somehow was still down at the exact moment the idle timeout fired would
  // otherwise never get its release processed, leaving stale coordinates
  // for the next real touch to trip over on wake. Clearing it here means
  // there's nothing left to resolve.
  touchWasDown = false;
  touchStartX = touchStartY = touchLastX = touchLastY = -1;
  // On battery, idle becomes deep sleep. While externally powered, only the
  // display and amplifier turn off; the CPU/BLE stay alive and the power key
  // wakes the UI without a reboot.
  turnScreenOff();
}

// GPIO0 is the board's active-low boot button and is also an ESP32-S3 RTC
// GPIO, so it can wake the chip directly without periodic timer reboots.
void enterDeepIdle() {
  // Configure the pin through the RTC block explicitly. Keeping RTC_PERIPH
  // powered is required for the internal pull-up to remain active; otherwise
  // GPIO0 can float low in deep sleep and immediately wake the chip again.
  rtc_gpio_init(GPIO_NUM_0);
  rtc_gpio_set_direction(GPIO_NUM_0, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en(GPIO_NUM_0);
  rtc_gpio_pulldown_dis(GPIO_NUM_0);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

  // EXT1 is the ESP32-S3's robust multi-RTC-GPIO wake path. ANY_LOW matches
  // the active-low BOOT switch and consumes no periodic timer wakeups.
  const esp_err_t wakeResult = esp_sleep_enable_ext1_wakeup_io(
      1ULL << GPIO_NUM_0, ESP_EXT1_WAKEUP_ANY_LOW);
  if (wakeResult != ESP_OK) {
    Serial.printf("Deep sleep: button wake setup failed (%d)\n",
                  static_cast<int>(wakeResult));
    Serial.flush();
    delay(50);
    esp_restart();
  }
  esp_deep_sleep_start();
}

void turnScreenOff() {
  if (!sleeping || screenOff) return;
  screenOff = true;
  panel->setBrightness(0);
  panel->displayOff();
  digitalWrite(AUDIO_PA, LOW);
  if (externalPowerPresent) {
    Serial.println("Display: off; external power present, remaining awake");
    return;
  }
  Serial.println("Display: off; entering deep sleep, power key remains the only wake");
  enterDeepIdle();
}

// Reachable two ways: during the brief sleeping-but-not-screenOff window
// (the blinking-face countdown in enterSleep(), before turnScreenOff()
// hands off), where screenOff is false and there's nothing to restore; and
// from the externally-powered screenOff state turnScreenOff() can now
// leave the CPU running in, where it is true and the display/amp need
// restoring below. A real wake from actual deep sleep (on battery) goes
// through setup()'s own fast path instead, since deep sleep is a reboot
// and this function's in-RAM state wouldn't have survived it anyway.
void wakeUp() {
  if (screenOff) {
    panel->displayOn();
    digitalWrite(AUDIO_PA, HIGH);
    screenOff = false;
  }
  sleeping = false;
  lastInteraction = millis();
  panel->setBrightness(brightnessLevel());
  showToast("Link restored");
  playTone(659, 60);
  playTone(880, 100);
  drawHome();
}

// GPIO0 is a genuine power key now, not just a wake source: a fresh press
// wakes it from sleep same as always, but a fresh press while awake also
// puts it to sleep immediately instead of waiting for the idle timeout.
// On battery this is a one-way trip through a full reboot (enterSleep()
// hands off to deep sleep); while externally powered it instead leaves the
// CPU running with the display off, so enterSleep() does return here.
void pollPowerButton(uint32_t now) {
  if (now - lastPwrRead < 25) return;
  lastPwrRead = now;
  const bool down = digitalRead(0) == LOW;
  if (down && !pwrWasDown) {
    if (sleeping) {
      wakeUp();
    } else {
      enterSleep();
    }
  }
  pwrWasDown = down;
}

void setup() {
  // A deep-sleep resume is only possible through the boot-button wake source.
  // Other resets remain ordinary boots even if their reset reason happens to
  // follow an earlier idle cycle.
  const bool resumingFromDeepIdle =
      esp_reset_reason() == ESP_RST_DEEPSLEEP &&
      esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1;

  Serial.begin(115200);
  delay(100);
  Serial.printf("Digipet firmware v%s\n", DIGIPET_VERSION);
  Serial.printf("Reset reason: %d%s\n", static_cast<int>(esp_reset_reason()),
                resumingFromDeepIdle ? " (deep-sleep resume)" : "");
  Serial.printf("Wake cause: %d; EXT1 status: 0x%llX\n",
                static_cast<int>(esp_sleep_get_wakeup_cause()),
                static_cast<unsigned long long>(esp_sleep_get_ext1_wakeup_status()));
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

  // The animated intro is a first-boot thing. Replaying it on every resume
  // from deep sleep would reintroduce exactly the "reboot every time you
  // want to glance at your pet" cost deep sleep is meant to avoid -- a
  // resume just brings brightness back up instead, skipping even the
  // disabled-boot-animation fallback's own delay(350).
  if (resumingFromDeepIdle) {
    panel->setBrightness(brightnessLevel());
  } else if (settings.bootAnimationEnabled) {
    bootAnimation();
    panel->setBrightness(brightnessLevel());
  } else {
    panel->setBrightness(brightnessLevel());
    display->fillScreen(COLOR_BACKGROUND);
    drawCentered("DIGIPET", 196, 4, COLOR_MINT);
    delay(350);
  }

  // The rest of hardware bring-up runs as its own visible phase, after the
  // animation has already played uninterrupted rather than polled
  // mid-frame -- unchanged on a resume. The *full* network/time sync
  // (runStartupNetworkSync()'s up-to-12s WiFi connect attempt, gated by
  // the syncOnBoot preference) is skipped on a resume specifically: it's
  // meant as a once-per-real-boot thing, and repeating it on every wake
  // would be a far worse cost than the animation ever was. A resume still
  // needs to know whether the clock is currently valid, though --
  // resyncClockAfterResume() is the cheap RTC-only check that answers
  // that (and only falls through to the same expensive WiFi/NTP path if
  // the RTC itself has actually lost its retained time, its own comment
  // has the reasoning).
  detectHardware(!resumingFromDeepIdle);
  if (!resumingFromDeepIdle) runStartupNetworkSync();
  else resyncClockAfterResume();
  generatePlayerId();
  checkEvolution();

  if (touch->begin()) {
    touch->IIC_Write_Device_State(
        Arduino_IIC_Touch::Device::TOUCH_DEVICE_INTERRUPT_MODE,
        Arduino_IIC_Touch::Device_Mode::TOUCH_DEVICE_INTERRUPT_PERIODIC);
  }
  // pmuDetected was already set above (detectHardware()); nothing to
  // initialize on the AXP2101 itself (see axp2101Read()'s own comment) --
  // just take a first reading so the Status page isn't stuck at 0% before
  // loop()'s own poll gets to it.
  updateBatteryReading();
  updateBatteryTimeEstimate();
  Serial.printf("Battery: %s (%umV, %u%%)\n", pmuDetected ? "AXP2101 ready" : "PMU not found",
                batteryVoltageMv, batteryPercent);
  pinMode(0, INPUT_PULLUP);
  // A resume's own wake press can easily still be held down right as
  // loop() starts -- seeding this from the pin's actual current state
  // (rather than the false a fresh RAM global would default to) means
  // pollPowerButton() sees "no new edge" until that same press is
  // released, instead of misreading the tail end of the wake press itself
  // as a brand new one and immediately sleeping right back.
  pwrWasDown = digitalRead(0) == LOW;
  nextBlink = millis() + 2200;
  lastMinute = lastInteraction = lastBatteryPoll = lastAnimation = millis();

  // wakeUp() draws its own frame (with the "Link restored" toast/chime) on
  // a resume, so the plain draw below is only for a genuine boot -- doing
  // both would just be the same frame painted twice.
  if (resumingFromDeepIdle) {
    wakeUp();
  } else if (transitionsReady) {
    renderPageToCanvas(currentPage, pageCanvasA);
    panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(),
                              LCD_WIDTH, LCD_HEIGHT);
  } else {
    drawHome();
  }
}

void loop() {
  // screenOff (and sleeping) can be observed true here whenever the device
  // is externally powered: turnScreenOff() only hands off to enterDeepIdle()
  // (a reboot, never returning to this loop -- deep idle is handled
  // entirely in setup()'s own fast path instead) while running on battery.
  // With power connected it instead leaves the CPU/BLE running with the
  // display off, so this loop keeps iterating and needs to behave -- see
  // the sleeping-gated branches below.
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
      if (showingFriends && friendDiscoverable &&
          battle.state() != FamiliarBattleState::Hosting) {
        friendDiscoverable = false;
      }
      lastBattleState = battle.state();
      lastBattleTurn = battle.turnNumber();
      lastBattleMyHp = battle.myHp();
      lastBattleOpponentHp = battle.opponentHp();
      // A background friend exchange (Discoverable, screen off/asleep) can
      // still land a state change here -- battle.update() above runs
      // unconditionally, regardless of sleeping. Skip the redraw while
      // asleep: with the screen off there's nothing to show it on, and
      // while merely drifting off (sleeping but not yet screenOff) the
      // panel is mid-way through drawSleep()'s own animation, which this
      // would otherwise paint straight over.
      if (!sleeping) {
        if (showingFriends) presentFriendsPage();
        else presentBattlePage();
      }
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
  // Continue PMU polling in the externally-powered screen-off state so
  // unplugging the cable can hand off to real deep sleep.
  if ((!sleeping || screenOff) && now - lastBatteryPoll >= 30000) {
    lastBatteryPoll = now;
    updateBatteryReading();
    updateBatteryTimeEstimate();
    if (sleeping && screenOff && !externalPowerPresent) {
      Serial.println("External power removed while display was off; entering deep sleep");
      enterDeepIdle();
    }
    // The Status summary view is the only screen showing any of this, and
    // nothing else redraws it on a timer (unlike Companion/Genome Lab,
    // which already repaint every tick, Status only otherwise redraws on
    // navigation or while a toast is up) -- without this, plugging in a
    // charger while sitting on this exact page wouldn't actually appear
    // on screen until some unrelated event happened to repaint it. Skipped
    // while sleeping (this branch can still run then, screenOff-on-external-
    // power) since the display is physically off with nothing to show it.
    if (!sleeping && currentPage == PAGE_STATUS && !statusShowingActions &&
        !gameOverlayActive()) {
      presentCoherentPageFrame(PAGE_STATUS);
    }
  }

  if (!sleeping && now - lastInteraction >= SLEEP_TIMEOUTS[settings.sleepIndex]) {
    enterSleep();
  }

  pollPowerButton(now);

  if (sleeping && !screenOff && now - lastAnimation >= 900) {
    lastAnimation = now;
    animationFrame = !animationFrame;
    updateCreatureAnimation();
  } else if (!sleeping && !gameOverlayActive() &&
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
  } else if (!sleeping && toastVisible && !gameOverlayActive() &&
             !showingPlayerId && !showingUpdate && !showingBattleResults &&
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

  // Touch can't wake it anymore (see pollPowerButton()/the sleeping branch
  // below), so there's nothing for a touch read to accomplish at all while
  // asleep -- skip the I2C round trip entirely rather than reading data
  // that's guaranteed to be thrown away.
  if (sleeping) return;
  // While nothing is touching the panel, only bother reading when the
  // touch controller's own periodic-mode IRQ (touchInterrupt(), wired up
  // via TOUCH_DEVICE_INTERRUPT_PERIODIC in setup()) says something is --
  // that mode pulses the interrupt line for as long as a finger is on the
  // panel and stays silent otherwise, so gating on it instead of polling
  // blindly every 25ms skips the I2C round trip entirely for as long as
  // the screen just sits there, which is most of any awake session.
  // Once a touch actually starts, this reverts to the exact original
  // fixed-cadence poll (unconditionally, ignoring the flag) all the way
  // through release -- deliberately not trusting periodic mode's pulse
  // behavior for mid-touch tracking or the release edge itself, only for
  // "should I even check right now."
  if (!touchWasDown && !touch->IIC_Interrupt_Flag) return;
  if (now - lastTouchRead < 25) return;
  touch->IIC_Interrupt_Flag = false;
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
      // sleeping is unreachable here -- the whole touch block above is
      // skipped while asleep (touch can't wake it anymore, see
      // pollPowerButton()), so nothing below needs to handle that case.
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
      // One gate for every game-owned overlay (evolution debug/Rivals/Recon
      // Log/Friends/Items/Genome Profile) -- gameOverlayTouch() keeps the
      // exact same per-overlay if-chain these used to be here, just moved.
      // Safe to check ahead of the console's own showingPlayerId/
      // showingUpdate overlays and the evolution-debug open-trigger just
      // below despite those two originally sitting between certain of
      // these checks: at most one showingX is ever true at a time (every
      // overlay-open site sets exactly one and nothing reads another's
      // flag), so this reordering changes nothing observable.
      if (gameOverlayActive()) {
        gameOverlayTouch(touchLastX, touchLastY, deltaX, deltaY, true);
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
      // Vertical swipe on Status/Battle -- both delegate to gameHandleSwipe(),
      // which keeps the exact original PAGE_STATUS-vs-PAGE_BATTLE bodies
      // (and, within PAGE_BATTLE, the results-vs-idle distinction the two
      // original separate blocks here used to encode via their own outer
      // guards); merging those guards into one "vertical swipe on either
      // page" check changes nothing observable, since neither original
      // block did anything when its own sub-condition didn't hold either.
      if ((currentPage == PAGE_STATUS || currentPage == PAGE_BATTLE) &&
          abs(deltaY) > 55 && abs(deltaY) > abs(deltaX)) {
        gameHandleSwipe(currentPage, deltaX, deltaY);
        touchStartX = touchStartY = touchLastX = touchLastY = -1;
        return;
      }
      // Horizontal page-to-page swipe -- a console mechanism (transitionToPage
      // works on any Page), gated on one piece of live game state (a BLE
      // battle link shouldn't be interrupted by accidentally paging away).
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
      } else if (currentPage == PAGE_SETTINGS) {
        changeSetting(touchLastX, touchLastY);
      } else {
        gameHandleTap(currentPage, touchLastX, touchLastY, deltaX, deltaY, touchDuration);
      }
    }
    touchStartX = touchStartY = touchLastX = touchLastY = -1;
  }
}
