# DIY Virtual Pet — Waveshare ESP32-S3-Touch-AMOLED-2.16

A pocket-sized, modern Tamagotchi-style pet. This repo is a **fork of Digipet**. The goal is to **port it to the Waveshare ESP32-S3-Touch-AMOLED-2.16 board, then customize how the pet behaves.**

This file is the working brief for Claude Code. Read it fully before changing anything.

---

## 1. Goal and scope

1. Get the Digipet fork building and running on the target board.
2. Port the display, touch, power and input code to this board.
3. Customize pet behavior (stats, evolution, events, day-to-day helper features).
4. Out of scope for now: the 3D-printed case, final battery sizing.

**Working style requested:** small verifiable steps. After each step the firmware must compile (`pio run`). Commit after each working step.

---

## 2. Target hardware (from Waveshare docs)

Source: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16
Source2: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16

| Item | Spec |
|---|---|
| MCU | ESP32-S3R8, up to 240 MHz, 8 MB PSRAM |
| Flash | 16 MB |
| Display | 2.16" capacitive AMOLED, **480×480**, **CO5300** driver, **QSPI** |
| Touch | **CST9220** (I2C, INT + RST lines) |
| IMU | QMI8658 (6-axis) |
| RTC | PCF85063ATL, interrupt on GPIO13 |
| Audio | ES8311 codec (speaker), ES7210 ADC (dual mic), I2S |
| Power | AXP2101 PMIC, MX1.25 2-pin LiPo header (3.7 V, charge + discharge) |
| Buttons | PWR, user button on GPIO18 (Key3), BOOT on GPIO0 |
| SD card | TF slot over SPI (GPIO1/2/3, CS on GPIO41) |
| Wireless | 2.4 GHz Wi-Fi, Bluetooth 5 LE, onboard antenna |
| USB | USB-C (flashing, logs) |
| Dimensions | Not in the text docs; check the dimensions diagram on the Waveshare page |

**Resolved from the demo repo and schematic (2026-10-28):** all GPIOs and I2C addresses are in `include/board_amoled_216.h`, each tagged with its source. There is **no I/O expander** on this board; display reset (GPIO39), touch reset (GPIO40) and SD CS (GPIO41) are plain GPIOs. Note the CLAUDE.md table's touch/display pins differ from the 1.8" board (LCD SCLK 38, TP_INT 11, I2S MCLK 42).

**Still open:**
- Recommended battery size and board outline.
- Official demo repo: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16

### Key porting insight
The 1.8" board Digipet targets also uses a **CO5300-class QSPI AMOLED**. So Digipet's display driver path is the closest match. Expect differences in:
- resolution (368×448 → 480×480, so layout/scaling code changes),
- touch controller (FT3168 → **CST9220**),
- pin map and possibly the power/IO setup.

`TFT_eSPI` does **not** support QSPI AMOLED. Keep Digipet's display stack (vendored Waveshare CO5300 QSPI driver); if a new driver is needed, use `Arduino_GFX` or Espressif's `esp_lcd` components.

---

## 3. Base repo: Digipet (decided)

We fork **Digipet** (keep the original repo as the `upstream` git remote).

Why Digipet:
- It already targets a Waveshare ESP32-S3 AMOLED board (the 1.8" V2, CO5300 QSPI), so the port is the smallest.
- PlatformIO project (`pio run`, `pio run -t upload`) with vendored Waveshare libs.
- Features we want to keep: BLE battles, friend codes, and Wi-Fi/BLE scanning for food.
- 64 commits, CI that builds and tests, signed releases.

Known caveats:
- 0 stars/forks, so it is lightly tested by others; expect bugs unrelated to our port.
- It supports only the 1.8" V2 board. Do not assume its pins, touch chip or power setup match ours.
- No case files and no custom PCB (not needed).

Other projects (TamaFi, Catode32, Pixel Pets) are **reference only** for ideas; do not port their code. Check a repo's license before borrowing anything.

## 4. Suggested toolchain

- **PlatformIO** (`pio run`, `pio run -t upload`, `pio device monitor`), as Digipet uses it.
- Framework: Arduino on ESP32-S3 (or ESP-IDF if the base repo uses it).
- Board settings to confirm: 16 MB flash, 8 MB PSRAM (OPI), USB CDC on boot enabled, partition table that fits 16 MB.
- Display lib: the one Digipet already vendors (Waveshare CO5300 QSPI driver); switch only if it cannot handle 480×480.
- Graphics buffer: allocate in PSRAM; 480×480×2 bytes ≈ 450 KB per full frame, so use partial updates or a sprite for the pet area.

---

## 5. Work plan (do in order)

### Phase 0 — Setup
- [x] Fork Digipet on GitHub, clone it locally, and add the original repo as the `upstream` remote. (`upstream` = `https://github.com/Zetascrub/Digipet.git`)
- [x] Read the README, `platformio.ini`, and the source tree. Write a short architecture summary in `docs/ARCHITECTURE.md` (modules, main loop, how display/input/save/network are separated).
- [x] Confirm the project builds unchanged (`pio run`) for its original board. (2026-10-28: `waveshare_amoled_v2` builds, RAM 20.3%, flash 46.5%; `pio test -e native` 82/82 pass.)

### Phase 1 — Bring up the new board (hardware abstraction)
- [x] Add a new PlatformIO env, e.g. `[env:waveshare_amoled_216]`, with build flags `-DBOARD_AMOLED_216`. (Builds the bring-up firmware in `src/bringup_216/` for now.)
- [x] Create a board config header (`include/board_amoled_216.h`) holding **all** pin numbers and screen size constants. No magic numbers elsewhere.
- [x] Get pins from Waveshare's official demo code or schematic. **Ask me if anything is ambiguous; do not invent pin numbers.** (Each value in the header is tagged with its source.)
- [ ] Minimal "hello" firmware: init power chip (AXP2101), init display, fill screen with color, print text. Verify on the device. (Compiles; device check in `docs/DEVICE_TESTS.md`.)
- [ ] Init touch (CST9220) and print coordinates to serial. (Compiles; device check in `docs/DEVICE_TESTS.md`.)
- [ ] Init IMU (QMI8658), RTC (PCF85063), buttons (GPIO18, BOOT).

### Phase 2 — Port the pet app
- [ ] Replace/adjust the display layer so the pet UI renders at 480×480 (scale sprites or redesign layout; keep UI sizes in constants).
- [ ] Map the base repo's input to touch + buttons on this board.
- [ ] Port save/load (flash or SD) and the timekeeping (use the RTC for time between boots).
- [ ] Verify pet stats decay and persist across reboots.
- [ ] Verify Wi-Fi/BLE features still work on this board.

### Phase 3 — Power
- [ ] Read battery voltage/percent from the AXP2101 and show it in the UI.
- [ ] Add screen dimming/off after inactivity (AMOLED burn-in risk: avoid static high-brightness elements; shift pixels or sleep the screen).
- [ ] Light sleep / wake on touch, button, or IMU motion to extend battery life.

### Phase 4 — Customize behavior
Fill in the desired behavior here (edit this list as ideas come):
- [ ] Stats and decay rates: hunger, happiness, energy, health.
- [ ] Evolution tree / life stages.
- [ ] Use IMU: shake to wake/play, step-like activity gives the pet energy.
- [ ] Use RTC: day/night cycle, pet sleeps at night.
- [ ] Day-to-day helper features (ideas): reminders, Pomodoro/focus timer, hydration nudge, weather via Wi-Fi.
- [ ] Wi-Fi/BLE feeding: Digipet already scans for food; tune what counts as good food (idea from TamaFi: food quality depends on networks detected).
- [ ] Pet-to-pet: keep Digipet's BLE battles and friend codes working on the new board.

### Phase 5 — Polish
- [ ] Sound via ES8311 (simple beeps/jingles).
- [ ] Settings screen (brightness, volume, time).
- [ ] OTA or USB update flow, release builds in CI.

---

## 6. Rules for Claude Code in this repo

1. **Never guess hardware facts** (pins, I2C addresses, register values). Use the Waveshare docs/demo code, or ask me.
2. Keep board-specific code behind the board config header and a thin HAL; game logic must not touch hardware directly.
3. One logical change per commit, with a message saying what was tested on device vs. only compiled.
4. Keep the project compiling after every step. Run `pio run` before reporting done.
5. Note anything that can only be verified on the physical device in `docs/DEVICE_TESTS.md` as a checklist for me to run.
6. Don't add large dependencies without telling me why.
7. Respect the base repo's license; keep attribution and the original LICENSE file in the fork.

---

## 7. Known risks

- Digipet is lightly tested (no stars/forks); expect bugs unrelated to the port.
- Pin maps, I/O expander usage and touch init sequence for this exact board must come from Waveshare resources.
- 480×480 is a bigger frame than the base UI was designed for; RAM use and refresh speed need checking.
- AMOLED burn-in: avoid static bright UI.
- Don't mix this board's code with the 1.8" board's code assuming they are identical; they differ in touch chip and pins.

---

## 8. Reference links

- Waveshare docs for this board: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16
- Waveshare GitHub (demo code): https://github.com/waveshareteam
- Digipet upstream: the repo this fork came from (keep as the `upstream` remote).
- TamaFi (reference only, Wi-Fi feeding idea): https://github.com/cifertech/TamaFi
- Earlier comparison spreadsheet: `virtual_pet_repos_comparison.xlsx`

---

## 9. First prompt to give Claude Code

> Read CLAUDE.md. Then do Phase 0: summarize the repo architecture in `docs/ARCHITECTURE.md`, confirm it builds, and list every place the code touches display, touch, power, storage and networking so we can plan the port to the Waveshare ESP32-S3-Touch-AMOLED-2.16. Don't change any code yet.
