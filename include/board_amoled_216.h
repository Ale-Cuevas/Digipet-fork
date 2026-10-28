#pragma once

// Board config for the Waveshare ESP32-S3-Touch-AMOLED-2.16. Every pin,
// I2C address and panel constant for this board lives here; nothing else
// should hardcode them.
//
// Sources (checked 2026-10-28 against waveshareteam/ESP32-S3-Touch-AMOLED-2.16
// at commit 225a62b):
//   [demo]  examples/arduino/libraries/Mylibrary/pin_config.h and the
//           01_HelloWorld / 03_AXP2101 / 05_LVGL_Widgets / 07_ES8311 sketches
//   [sch]   schematic/ESP32-S3-Touch-AMOLED-2.16-Schematic.pdf, page 1
//           (GPIO table, KEYS, RTC, 6/9-Axis and SD-CARD blocks)
//   [lib]   default addresses in SensorLib 0.3.3 / XPowersLib 0.2.6, the
//           driver versions the demo ships
//
// Unlike the 1.8" board there is no I/O expander: display reset, touch
// reset and SD CS are plain GPIOs [sch].

#include <stdint.h>

// --- Display: CO5300 over QSPI ---------------------------------------------
#define BOARD_LCD_SDIO0 4  // [demo][sch]
#define BOARD_LCD_SDIO1 5
#define BOARD_LCD_SDIO2 6
#define BOARD_LCD_SDIO3 7
#define BOARD_LCD_SCLK 38
#define BOARD_LCD_CS 12
#define BOARD_LCD_RESET 39

#define BOARD_LCD_WIDTH 480
#define BOARD_LCD_HEIGHT 480
// Column/row offsets passed to Arduino_CO5300. The 1.8" board needs a
// column offset of 16; the 2.16" demo passes all zeros [demo].
#define BOARD_LCD_COL_OFFSET 0
#define BOARD_LCD_ROW_OFFSET 0
// The demo writes MADCTL (0x36) = 0xA0 right after gfx->begin() [demo].
// The touch swap/mirror settings below are tuned to this orientation, so
// change both together.
#define BOARD_LCD_MADCTL_REG 0x36
#define BOARD_LCD_MADCTL_VALUE 0xA0

// --- Shared I2C bus: touch, PMIC, IMU, RTC, audio codecs --------------------
#define BOARD_I2C_SDA 15  // [demo][sch]
#define BOARD_I2C_SCL 14

#define BOARD_I2C_ADDR_TOUCH 0x5A   // CST9220, CST92XX_SLAVE_ADDRESS [lib]
#define BOARD_I2C_ADDR_PMIC 0x34    // AXP2101_SLAVE_ADDRESS [lib]
#define BOARD_I2C_ADDR_IMU 0x6B     // QMI8658, QMI8658_L_SLAVE_ADDRESS [demo][lib]
#define BOARD_I2C_ADDR_RTC 0x51     // PCF85063, PCF85063_SLAVE_ADDRESS [lib]
#define BOARD_I2C_ADDR_ES8311 0x18  // ES8311_ADDRESS_0 [demo]
#define BOARD_I2C_ADDR_ES7210 0x40  // ES7210_AD1_AD0_00 [demo]

// --- Touch: CST9220 ---------------------------------------------------------
#define BOARD_TP_INT 11  // [demo][sch]
#define BOARD_TP_RST 40
// Mapping the demo applies so touch lines up with the MADCTL above [demo].
#define BOARD_TP_SWAP_XY true
#define BOARD_TP_MIRROR_X true
#define BOARD_TP_MIRROR_Y false

// --- IMU: QMI8658 -----------------------------------------------------------
#define BOARD_IMU_INT1 17  // [sch]
#define BOARD_IMU_INT2 21

// --- RTC: PCF85063ATL -------------------------------------------------------
#define BOARD_RTC_INT 13  // [sch]

// --- Buttons ----------------------------------------------------------------
// BOOT (Key2) and the user key (Key3) pull to GND with 10K pull-ups to 3V3,
// so they read LOW when pressed [sch].
#define BOARD_BTN_BOOT 0
#define BOARD_BTN_USER 18
#define BOARD_BTN_PRESSED_LEVEL 0
// PWR (Key1) goes to the AXP2101 PWRON pin. A BSS138 driven from PWRON
// pulls SYS_OUT (GPIO16, 10K pull-up) low while the key is up, so GPIO16
// should read HIGH while PWR is held [sch]. Not yet verified on the device.
#define BOARD_BTN_PWR_SENSE 16
#define BOARD_BTN_PWR_PRESSED_LEVEL 1

// --- PMIC: AXP2101 ----------------------------------------------------------
// The demo leaves every power rail at its power-on default and only sets a
// 4.2 V charge target (setChargeTargetVoltage(3) == XPOWERS_AXP2101_CHG_VOL_4V2)
// [demo]. We do the same.
// Selects the AXP2101 driver in XPowersLib; include this header before
// <XPowersLib.h> [demo].
#define XPOWERS_CHIP_AXP2101

// --- Audio: ES8311 (speaker) + ES7210 (mics) over I2S -----------------------
#define BOARD_I2S_MCLK 42  // [demo][sch]; the 1.8" board uses 16
#define BOARD_I2S_BCLK 9
#define BOARD_I2S_LRCK 45
#define BOARD_I2S_DOUT 8   // ESP -> ES8311 DSDIN
#define BOARD_I2S_DIN 10   // ES7210 ASDOUT -> ESP
#define BOARD_AUDIO_PA 46  // speaker amp enable

// --- SD card: SPI (not SD_MMC as on the 1.8") --------------------------------
#define BOARD_SD_MOSI 1  // [sch]
#define BOARD_SD_SCK 2
#define BOARD_SD_MISO 3
#define BOARD_SD_CS 41

// --- UART0 (header) ---------------------------------------------------------
#define BOARD_UART_TX 43  // [sch]
#define BOARD_UART_RX 44
