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

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    BOARD_LCD_CS, BOARD_LCD_SCLK, BOARD_LCD_SDIO0, BOARD_LCD_SDIO1, BOARD_LCD_SDIO2, BOARD_LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(
    bus, BOARD_LCD_RESET, 0, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT,
    BOARD_LCD_COL_OFFSET, BOARD_LCD_ROW_OFFSET, BOARD_LCD_COL_OFFSET, BOARD_LCD_ROW_OFFSET);

XPowersPMU pmu;
TouchDrvCST92xx touch;
bool pmuOk = false;
bool displayOk = false;
bool touchOk = false;
volatile bool touchPending = false;

const uint8_t BRINGUP_BRIGHTNESS = 160;
const uint32_t STATUS_INTERVAL_MS = 2000;

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
  if (displayOk) drawTestScreen();
}

void loop() {
  static uint32_t lastStatus = 0;
  if (millis() - lastStatus >= STATUS_INTERVAL_MS) {
    lastStatus = millis();
    logPmu();
  }
  pollTouch();
  delay(10);
}
