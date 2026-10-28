# Digipet architecture (as forked, before the 2.16" port)

Snapshot of the code at upstream commit `0e4514a` (Zetascrub/Digipet), written
to plan the port to the Waveshare ESP32-S3-Touch-AMOLED-2.16. Line numbers are
approximate and will drift as code changes.

## Build

| Env | What it builds |
|---|---|
| `waveshare_amoled_v2` (default) | Firmware for the 1.8" V2 board. Arduino core 3.3.11 on pioarduino 55.03.311, `esp32-s3-devkitc-1`, 16 MB flash (`default_16MB.csv`), QIO flash + OPI PSRAM, USB CDC on boot. Embeds boot audio and mascot frames from `src/assets/`. |
| `native` | Host unit tests for the pure logic only (`pet_genome`, `familiar_battle_rules`, `color_utils`). Fakes in `test/native/fakes`. |

Libraries: vendored Waveshare copies of `GFX_Library_for_Arduino` 1.6.4 (CO5300
QSPI driver + canvas), `Arduino_DriveBus` 1.0.1 (I2C bus + touch drivers) and
`Adafruit_XCA9554` (I/O expander); see `lib/VENDORED.md`. From the registry:
NimBLE-Arduino 2.5.1, ArduinoJson 7.4.2, Adafruit BusIO 1.17.2.

`tools/sim/` renders the real page-drawing code to PNG on the host (no board needed).
CI (`.github/workflows/build.yml`) runs `pio run` + `pio test -e native` and checks
that the OTA public key in the source matches `release/`.

## Modules

```
src/main.cpp (~2600 lines)     "console": board bring-up, drivers, power, sleep,
                               settings, boot sequence, transitions, touch dispatch
src/pin_config.h               all pins + LCD_WIDTH/LCD_HEIGHT (1.8" board)
src/game/vpet/game_vpet.cpp    pet logic: stats, aging, evolution, actions, feed scan,
                               items, battles/friends/rivals pages, save/load
src/game/vpet/ui_pages.cpp     Companion/Status/Battle/Genome page renderers
src/game/vpet/pet_genome.cpp   procedural genome encode/decode, palettes  (pure)
src/game/vpet/familiar_battle_rules.cpp  battle math                       (pure)
src/familiar_battle_service.cpp  BLE (NimBLE) host/join/battle protocol, BLE scanning
src/ota_updater.cpp            signed OTA over Wi-Fi from GitHub Releases
src/power_telemetry.cpp        power event log in NVS ("powerlog" namespace)
src/color_utils.cpp            RGB565 helpers                               (pure)
include/game_module.h          console <-> game seam (gameInit/Tick/DrawPage/HandleTap...)
include/persist_blob.h         magic-guarded struct save/load over Preferences
include/ui_pages.h             Page enum + extern globals the renderers read
```

The seam in `game_module.h` is only partly wired. `main.cpp` still calls many
vpet functions by name (`agePet`, `checkEvolution`, `savePet`, ...), and game
code reaches back into console globals (`display`, `panel`, `pageCanvasA`,
`playTone`, `mountGenomeCard`, battery fields) via `extern`.

## Main loop

`setup()` (`main.cpp` ~2162):
1. Detect deep-sleep resume (EXT1 wake on GPIO0).
2. `Wire.begin`, open NVS namespace `digipet`, load settings and all game state.
3. Pulse I/O expander pins 0-2 low then high (resets for display/touch, presumably).
4. Probe and init ES8311 codec + I2S, then `panel->begin()`.
5. Allocate two full-screen `Arduino_Canvas` + one transition frame in PSRAM.
6. Boot animation (cold boot only), full I2C scan, Wi-Fi/NTP sync from the SD card's
   `wifi.ini` (cold boot) or RTC check (resume).
7. Touch init (CST816x, interrupt mode), first battery read, GPIO0 as the power key.

`loop()` (`main.cpp` ~2302), all cooperative, `millis()` based:
- serial debug commands, power telemetry state changes (1 s)
- `battle.update()` while a BLE battle is active, redraw on state change
- clock redraw (1 s), `agePet()` stat decay (60 s), battery poll (30 s)
- idle timeout leads to `enterSleep()`, GPIO0 edge toggles sleep/wake
- creature / Genome Lab animation ticks
- touch polling (25 ms), tap vs. swipe detection, dispatch to overlays, game, or Settings

Rendering: pages are drawn into a PSRAM canvas, then blitted full frame with
`panel->draw16bitRGBBitmap`. Page transitions slide between two canvases.
There are no partial updates.

Power: on battery, sleep turns into **deep sleep** (CPU off, wake only on GPIO0;
a wake is a reboot with a fast path). On USB power, only the display and amp turn off.

## How the concerns are separated

| Concern | Where | Separation |
|---|---|---|
| Display | `main.cpp` globals, `ui_pages.cpp`, `game_vpet.cpp` | Weak. ~530 direct `display->` calls with hardcoded 368x448 coordinates. |
| Input | `main.cpp` loop touch block, `game*Handle*()` | Medium. Raw touch is read in `main.cpp`, but per-page handlers hit-test hardcoded coordinates. |
| Save | `persist_blob.h` + Preferences/NVS | Good. No hardware dependency beyond NVS. |
| Network | `familiar_battle_service.cpp`, `ota_updater.cpp`, Wi-Fi in `main.cpp` and `game_vpet.cpp` | Fair. Wi-Fi and BLE share one radio and the code switches between them sequentially. |
| Pure logic | `pet_genome`, `familiar_battle_rules`, `color_utils` | Good. Unit-tested on the host. |

## Hardware touchpoint inventory

Each row is something the 2.16" port has to check or change.
"1.8" value" is what the code uses today. **Do not assume it is the same on the 2.16".**

### Display (CO5300 QSPI)
| Where | What | 1.8" value |
|---|---|---|
| `src/pin_config.h` | QSPI pins SDIO0-3, SCLK, CS; `LCD_WIDTH`/`LCD_HEIGHT` | 4,5,6,7 / 11 / 12; 368x448 |
| `main.cpp` ~56-63 | `Arduino_ESP32QSPI` + `Arduino_CO5300(bus, no RST, rot 0, w, h, 16, 0,0,0)`, 2 canvases | Reset not wired to a GPIO (expander?); column offset 16 |
| `main.cpp` ~2197 | XCA9554 expander at 0x20, pins 0/1/2 pulsed as resets | Unknown which line is which |
| `main.cpp` ~2213-2222 | `panel->begin()`, PSRAM canvases (3 x w x h x 2 bytes) | ~990 KB; at 480x480 ~1.38 MB, fits in 8 MB |
| `main.cpp` 1194, 1254, 2008, 2105-2135 | `setBrightness`, `displayOn/Off` | |
| `ui_pages.cpp` (409 calls), `main.cpp` (97), `game_vpet.cpp` (26) | Drawing with absolute coordinates; ~60 literal 368/448/184/224 plus many layout constants | Needs a layout pass for 480x480 |
| `game_vpet.cpp` (8 sites, e.g. 497) | Direct full-frame blits | |
| `tools/sim/render_harness.cpp` | Uses `LCD_WIDTH/HEIGHT` | Follows `pin_config.h` |

### Touch
| Where | What | 1.8" value |
|---|---|---|
| `pin_config.h` | `IIC_SDA`, `IIC_SCL`, `TP_INT` | 15, 14, 21 |
| `main.cpp` ~94 | `Arduino_CST816x` at `CST816T_DEVICE_ADDRESS`, no RST pin | README says CST820 (CLAUDE.md says FT3168; the code uses CST816x) |
| `main.cpp` ~2262, ~2459-2470 | Init in interrupt mode; reads finger count and X/Y | No rotation or scaling applied |
| `lib/Arduino_DriveBus/src/touch_chip/` | Only `CST816x` and `CST2xxSE` drivers | **No CST9220 driver. One is needed for the 2.16".** |

### Buttons and sleep/wake
| Where | What | 1.8" value |
|---|---|---|
| `main.cpp` 2148, 2277 | GPIO0 (BOOT) is the power key, polled every 25 ms | 2.16" also has GPIO18 user key + PWR via AXP2101 |
| `main.cpp` 2080-2103 | Deep sleep, EXT1 wake on GPIO0 only, RTC pull-up | No touch, IMU or RTC wake |

### Power (AXP2101)
| Where | What | 1.8" value |
|---|---|---|
| `main.cpp` 329-357 | Read-only raw registers at I2C 0x34: VBAT 0x34/0x35, gauge 0xA4, charge state 0x01 | Relies on PMIC power-on defaults; no rail configuration |
| `main.cpp` 343-470 | Battery estimate, `RTC_DATA_ATTR` drain history | |
| `power_telemetry.cpp` | Event log in NVS `powerlog` | Hardware-independent |

### Audio (ES8311)
| Where | What | 1.8" value |
|---|---|---|
| `pin_config.h` | `AUDIO_PA`, I2S MCLK/BCLK/DIN/LRCLK/DOUT | 46, 16, 9, 10, 45, 8 |
| `main.cpp` 305-320, 487-520 | Codec at I2C 0x18, init table "from Waveshare's V2 example", 16 kHz | |
| `main.cpp` 536-645 | `playTone`, boot clip task; ~25 `playTone` calls in `game_vpet.cpp` | |

### RTC (PCF85063) and IMU (QMI8658)
| Where | What | 1.8" value |
|---|---|---|
| `main.cpp` 655-694 | Raw BCD read/write at I2C 0x51, registers 0x04+ | RTC INT not used |
| `main.cpp` 858-960 | NTP sync writes the RTC; resume reads it | |
| `main.cpp` 1331-1350 | IMU presence check only at 0x6B; **IMU is not used** | |

### Storage
| Where | What | 1.8" value |
|---|---|---|
| NVS namespace `digipet` | Keys `state`, `settings`, `battleStats`, `reconLog`, `friends`, `inventory`, `copiedGenome`, `lastSync` | Portable as is (same flash/partitions) |
| `pin_config.h`, `main.cpp` 702-767 | **SD_MMC 1-bit** (CLK 2, CMD 1, D0 3); card power on expander pin 7 | The 2.16" docs list SD over **SPI** with CS on GPIO41, so this needs the `SD`/SPI driver instead |
| `game_vpet.cpp` 429-460 | `/digipet/genome.txt` export/import | |
| `main.cpp` 702 | `/digipet/wifi.ini` (SSID, password, timezone, NTP) | |

### Networking
| Where | What |
|---|---|
| `main.cpp` 858-960 | Wi-Fi STA connect (12 s timeout) + NTP on boot |
| `game_vpet.cpp` 518-560 | Feed scan: passive `WiFi.scanNetworks`, then `battle.scanNearbyBle(4000)` |
| `familiar_battle_service.cpp` 100-120, 349 | NimBLE init/deinit (device name "Ghostwire VPet"), host/join/battle GATT, BLE scan |
| `ota_updater.cpp` 31, 43-180 | Signed OTA from `DIGIPET_OTA_REPO`. `kTarget = "waveshare-esp32-s3-touch-amoled-1.8-v2"` must become board-specific, or a 2.16" device would accept 1.8" images |

Networking is board-independent apart from the antenna, so it should port unchanged.

## Port implications (input for Phase 1)

1. Move `pin_config.h` behind a board switch (`BOARD_AMOLED_216` -> `include/board_amoled_216.h`).
2. Write or vendor a CST9220 touch driver. The vendored DriveBus does not have one.
3. Find out what the 2.16" uses its I/O expander for (if it has one): display/touch reset, SD power.
4. Replace SD_MMC with SPI SD for the 2.16".
5. Make the UI layout resolution-aware (biggest job: ~530 drawing calls with fixed coordinates).
6. Confirm I2C addresses for AXP2101/ES8311/PCF85063/QMI8658 and the ES8311 init sequence from Waveshare's 2.16" demo.
7. Give the OTA `kTarget` a per-board value.
