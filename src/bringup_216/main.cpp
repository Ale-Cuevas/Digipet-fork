// Phase 1 bring-up firmware for the Waveshare ESP32-S3-Touch-AMOLED-2.16.
// Built only by [env:waveshare_amoled_216] until the pet app is ported
// (Phase 2). Exercises each peripheral on its own and logs to USB serial,
// so a failure points at one chip. Init sequences follow Waveshare's demo
// sketches; see include/board_amoled_216.h for where each value came from.

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>

#include "board_amoled_216.h"  // before XPowersLib: it selects the PMIC driver
#include <XPowersLib.h>
#include <TouchDrvCSTXXX.hpp>
#include <SensorQMI8658.hpp>
#include <SensorPCF85063.hpp>

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    BOARD_LCD_CS, BOARD_LCD_SCLK, BOARD_LCD_SDIO0, BOARD_LCD_SDIO1, BOARD_LCD_SDIO2, BOARD_LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(
    bus, BOARD_LCD_RESET, 0, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT,
    BOARD_LCD_COL_OFFSET, BOARD_LCD_ROW_OFFSET, BOARD_LCD_COL_OFFSET, BOARD_LCD_ROW_OFFSET);

XPowersPMU pmu;
TouchDrvCST92xx touch;
SensorQMI8658 imu;
SensorPCF85063 rtc;
bool pmuOk = false;
bool displayOk = false;
bool touchOk = false;
bool imuOk = false;
bool rtcOk = false;
volatile bool touchPending = false;

struct ButtonInput {
  const char *name;
  uint8_t pin;
  uint8_t pressedLevel;
  bool pressed;
};

ButtonInput buttons[] = {
    {"BOOT", BOARD_BTN_BOOT, BOARD_BTN_PRESSED_LEVEL, false},
    {"USER", BOARD_BTN_USER, BOARD_BTN_PRESSED_LEVEL, false},
    {"PWR", BOARD_BTN_PWR_SENSE, BOARD_BTN_PWR_PRESSED_LEVEL, false},
};

const uint8_t BRINGUP_BRIGHTNESS = 160;
const uint32_t STATUS_INTERVAL_MS = 2000;
const uint16_t RTC_VALID_FROM_YEAR = 2024;  // older means never set or lost power
const int16_t STATUS_BAND_X = 32;  // clear of the 24 px corner markers
const int16_t STATUS_BAND_Y = 300;

void initPmu() {
  pmuOk = pmu.begin(Wire, BOARD_I2C_ADDR_PMIC, BOARD_I2C_SDA, BOARD_I2C_SCL);
  if (!pmuOk) {
    Serial.println("[pmu] AXP2101 not found");
    return;
  }
  pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
  pmu.enableBattDetection();
  pmu.enableVbusVoltageMeasure();
  pmu.enableBattVoltageMeasure();
  pmu.enableSystemVoltageMeasure();
  Serial.printf("[pmu] AXP2101 ok, chip id 0x%02X\n", pmu.getChipID());
}

void logPmu() {
  if (!pmuOk) return;
  Serial.printf("[pmu] vbus %s %u mV, sys %u mV, batt %s %u mV %d%%, charging %s\n",
                pmu.isVbusIn() ? "in" : "out", pmu.getVbusVoltage(), pmu.getSystemVoltage(),
                pmu.isBatteryConnect() ? "yes" : "no", pmu.getBattVoltage(),
                pmu.isBatteryConnect() ? pmu.getBatteryPercent() : -1,
                pmu.isCharging() ? "yes" : "no");
}

void initDisplay() {
  displayOk = panel->begin();
  if (!displayOk) {
    Serial.println("[lcd] panel->begin() failed");
    return;
  }
  bus->writeC8D8(BOARD_LCD_MADCTL_REG, BOARD_LCD_MADCTL_VALUE);
  panel->setBrightness(BRINGUP_BRIGHTNESS);
  Serial.printf("[lcd] CO5300 ok, %dx%d\n", panel->width(), panel->height());
}

void IRAM_ATTR onTouchInterrupt() {
  touchPending = true;
}

void initTouch() {
  touch.setPins(BOARD_TP_RST, BOARD_TP_INT);
  touchOk = touch.begin(Wire, BOARD_I2C_ADDR_TOUCH, BOARD_I2C_SDA, BOARD_I2C_SCL);
  if (!touchOk) {
    Serial.println("[touch] CST92xx not found");
    return;
  }
  touch.setMaxCoordinates(BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT);
  touch.setSwapXY(BOARD_TP_SWAP_XY);
  touch.setMirrorXY(BOARD_TP_MIRROR_X, BOARD_TP_MIRROR_Y);
  pinMode(BOARD_TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BOARD_TP_INT), onTouchInterrupt, FALLING);
  Serial.printf("[touch] %s ok, %u points\n", touch.getModelName(), touch.getSupportTouchPoint());
}

// Logs each touch and leaves a dot on screen, so the mapping can be checked
// against the corner markers.
void pollTouch() {
  if (!touchOk) return;
  noInterrupts();
  const bool pending = touchPending;
  touchPending = false;
  interrupts();
  if (!pending) return;

  int16_t x[2], y[2];
  const uint8_t points = touch.getPoint(x, y, touch.getSupportTouchPoint());
  for (uint8_t i = 0; i < points; i++) {
    Serial.printf("[touch] #%u x=%d y=%d\n", i, x[i], y[i]);
    if (displayOk) panel->fillCircle(x[i], y[i], 4, i == 0 ? RGB565_CYAN : RGB565_MAGENTA);
  }
}

// Same accelerometer config as the demo, plus the gyro so both halves of
// the chip get exercised.
void initImu() {
  imuOk = imu.begin(Wire, BOARD_I2C_ADDR_IMU, BOARD_I2C_SDA, BOARD_I2C_SCL);
  if (!imuOk) {
    Serial.println("[imu] QMI8658 not found");
    return;
  }
  imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_1000Hz, SensorQMI8658::LPF_MODE_0);
  imu.configGyroscope(SensorQMI8658::GYR_RANGE_256DPS, SensorQMI8658::GYR_ODR_896_8Hz, SensorQMI8658::LPF_MODE_0);
  imu.enableAccelerometer();
  imu.enableGyroscope();
  Serial.printf("[imu] QMI8658 ok, chip id 0x%02X\n", imu.getChipID());
}

// Read-only: the RTC is never written unless asked to over serial, so a
// device test can check that it kept time across a reset or power cycle.
void initRtc() {
  rtcOk = rtc.begin(Wire, BOARD_I2C_SDA, BOARD_I2C_SCL);
  if (!rtcOk) {
    Serial.println("[rtc] PCF85063 not found");
    return;
  }
  Serial.printf("[rtc] PCF85063 ok, %s\n", rtc.isRunning() ? "running" : "STOPPED");
}

void initButtons() {
  for (ButtonInput &b : buttons) {
    pinMode(b.pin, INPUT);  // all three have external 10K pull-ups [sch]
    b.pressed = digitalRead(b.pin) == b.pressedLevel;
    Serial.printf("[btn] %s on GPIO%u is %s at boot\n", b.name, b.pin, b.pressed ? "PRESSED" : "up");
  }
}

void pollButtons() {
  for (ButtonInput &b : buttons) {
    const bool pressed = digitalRead(b.pin) == b.pressedLevel;
    if (pressed == b.pressed) continue;
    b.pressed = pressed;
    Serial.printf("[btn] %s %s\n", b.name, pressed ? "down" : "up");
  }
}

// "settime YYYY-MM-DD HH:MM:SS" over serial writes the RTC.
void pollSerialCommands() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  int y, mo, d, h, mi, s;
  if (sscanf(line.c_str(), "settime %d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
    if (!rtcOk) {
      Serial.println("[rtc] not available");
      return;
    }
    rtc.setDateTime(y, mo, d, h, mi, s);
    Serial.println("[rtc] time set");
  } else if (line.length() > 0) {
    Serial.println("commands: settime YYYY-MM-DD HH:MM:SS");
  }
}

// Writes IMU, RTC and button state to serial and to a band on screen.
void logSensors() {
  char imuLine[64] = "IMU: MISSING";
  if (imuOk) {
    float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
    if (imu.getDataReady()) {
      imu.getAccelerometer(ax, ay, az);
      imu.getGyroscope(gx, gy, gz);
    }
    Serial.printf("[imu] acc %.2f %.2f %.2f g, gyr %.1f %.1f %.1f dps\n", ax, ay, az, gx, gy, gz);
    snprintf(imuLine, sizeof(imuLine), "IMU: %+.2f %+.2f %+.2f g", ax, ay, az);
  }

  char rtcLine[64] = "RTC: MISSING";
  if (rtcOk) {
    RTC_DateTime now = rtc.getDateTime();
    const bool valid = now.getYear() >= RTC_VALID_FROM_YEAR;
    snprintf(rtcLine, sizeof(rtcLine), "RTC: %04u-%02u-%02u %02u:%02u:%02u%s",
             now.getYear(), now.getMonth(), now.getDay(), now.getHour(), now.getMinute(),
             now.getSecond(), valid ? "" : " (unset)");
    Serial.printf("[rtc] %s\n", rtcLine + 5);
  }

  char btnLine[64];
  snprintf(btnLine, sizeof(btnLine), "BTN: BOOT %s USER %s PWR %s",
           buttons[0].pressed ? "dn" : "up", buttons[1].pressed ? "dn" : "up",
           buttons[2].pressed ? "dn" : "up");

  if (!displayOk) return;
  const int16_t lineH = 24;
  panel->fillRect(STATUS_BAND_X, STATUS_BAND_Y, BOARD_LCD_WIDTH - 2 * STATUS_BAND_X, lineH * 3, RGB565_BLACK);
  panel->setTextSize(2);
  panel->setTextColor(RGB565_WHITE);
  const char *lines[] = {imuLine, rtcLine, btnLine};
  for (uint8_t i = 0; i < 3; i++) {
    panel->setCursor(STATUS_BAND_X, STATUS_BAND_Y + i * lineH);
    panel->print(lines[i]);
  }
}

// One full-screen fill per primary so a wrong color order or offset is
// obvious, then corner markers to check orientation and edge clipping.
void drawTestScreen() {
  const uint16_t fills[] = {RGB565_RED, RGB565_GREEN, RGB565_BLUE};
  for (uint16_t color : fills) {
    panel->fillScreen(color);
    delay(400);
  }
  panel->fillScreen(RGB565_BLACK);

  const int16_t w = panel->width();
  const int16_t h = panel->height();
  const int16_t m = 24;
  panel->drawRect(0, 0, w, h, RGB565_WHITE);
  panel->fillRect(0, 0, m, m, RGB565_RED);          // top-left
  panel->fillRect(w - m, 0, m, m, RGB565_GREEN);    // top-right
  panel->fillRect(0, h - m, m, m, RGB565_BLUE);     // bottom-left
  panel->fillRect(w - m, h - m, m, m, RGB565_YELLOW);  // bottom-right

  panel->setTextColor(RGB565_WHITE);
  panel->setTextSize(3);
  panel->setCursor(m + 8, m + 8);
  panel->println("Digipet");
  panel->setTextSize(2);
  panel->setCursor(m + 8, m + 44);
  panel->println("AMOLED 2.16 bring-up");
  panel->setCursor(m + 8, m + 68);
  panel->printf("PMIC: %s", pmuOk ? "ok" : "MISSING");
  panel->setCursor(m + 8, m + 92);
  panel->printf("Touch: %s", touchOk ? "ok, draw here" : "MISSING");
}

void setup() {
  Serial.begin(115200);
  delay(1500);  // give USB CDC time to enumerate so early logs are not lost
  Serial.println("\n[bringup] Waveshare ESP32-S3-Touch-AMOLED-2.16");

  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
  initPmu();
  initDisplay();
  initTouch();
  initImu();
  initRtc();
  initButtons();
  if (displayOk) drawTestScreen();
}

void loop() {
  static uint32_t lastStatus = 0;
  if (millis() - lastStatus >= STATUS_INTERVAL_MS) {
    lastStatus = millis();
    logPmu();
    logSensors();
  }
  pollTouch();
  pollButtons();
  pollSerialCommands();
  delay(10);
}
