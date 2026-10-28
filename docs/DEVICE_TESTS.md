# Device tests

Checks that can only be done on the physical board. Tick each box once it
passes on the device. If one fails, note what you saw next to it.

## Waveshare ESP32-S3-Touch-AMOLED-2.16: Phase 1 bring-up

The bring-up firmware has its own env. Flash it and watch the log:

```
pio run -e waveshare_amoled_216_bringup -t upload
pio device monitor -e waveshare_amoled_216_bringup
```

### Power (AXP2101)
- [ ] Serial shows `[pmu] AXP2101 ok, chip id 0x..`.
- [ ] Every 2 s, a `[pmu]` line shows plausible values: on USB, `vbus in` at about 5000 mV and `sys` at about 5000 mV.
- [ ] With a LiPo plugged in: `batt yes`, a voltage between 3300 and 4200 mV, and a percentage. `charging yes` while on USB with the battery below full.
- [ ] Unplug USB while running on battery: the board keeps running and `vbus out` shows.

### Display (CO5300, 480x480)
- [ ] Serial shows `[lcd] CO5300 ok, 480x480`.
- [ ] At boot the screen flashes full **red**, then **green**, then **blue**. If red and blue are swapped, the color order is wrong. Note what you saw.
- [ ] Then a black screen with a white 1 px border on all four edges and nothing cut off. If one edge is missing or there is a band of noise, the column or row offset is wrong.
- [ ] Corner squares: **red top-left, green top-right, blue bottom-left, yellow bottom-right**, with the USB-C port at the ____ (fill in: top / bottom / left / right). This fixes which way "up" is.
- [ ] Text "Digipet", "AMOLED 2.16 bring-up" and "PMIC: ok" reads left to right and is not mirrored.

### Touch (CST9220)
- [ ] Serial shows `[touch] CST92xx ok` (the model name may say CST9217 or CST9220) and a point count.
- [ ] The test screen shows `Touch: ok, draw here`.
- [ ] Touching the screen prints `[touch] #0 x=.. y=..` and draws a cyan dot **under your finger**.
- [ ] Touching each corner square gives coordinates near that corner: red ~(0,0), green ~(479,0), blue ~(0,479), yellow ~(479,479). If a dot shows up mirrored or rotated, note which way. The fix is the `BOARD_TP_*` swap/mirror flags.
- [ ] A slow drag leaves a continuous trail of dots (checks that INT keeps firing while the finger is down).

### IMU (QMI8658)
- [ ] Serial shows `[imu] QMI8658 ok, chip id 0x05`. 0x05 is the QMI8658 WHO_AM_I value; note it if yours differs.
- [ ] Lying flat, face up: acceleration is about `0 0 +1 g` on screen and serial. Note which axis reads +1 g and its sign, and which axis changes when you tilt toward the USB port. Later shake/step detection needs this.
- [ ] Rotating the board makes the `gyr` values move away from 0 and settle back when it is still.

### RTC (PCF85063)
- [ ] Serial shows `[rtc] PCF85063 ok, running`.
- [ ] On first boot the time may show `(unset)`. Send `settime 2026-10-28 12:00:00` (use the real time) in the serial monitor: it prints `[rtc] time set` and the clock ticks once a second.
- [ ] Press RESET: the time carries on and is not reset.
- [ ] Unplug USB with the battery connected, wait a minute, plug back in: the time is still correct. (Shows the RTC has backup power.)
- [ ] Power off fully (no USB, no battery), wait, power on: note whether the time survived. This depends on whether VCC-RTC has its own backup; the schematic doesn't settle it.

### Buttons
- [ ] At boot, serial lists BOOT (GPIO0), USER (GPIO18) and PWR (GPIO16) as `up`.
- [ ] Press and release **BOOT**: `[btn] BOOT down` then `[btn] BOOT up`.
- [ ] Press and release the **user key** (Key3): `[btn] USER down` / `up`.
- [ ] Short-press **PWR**: `[btn] PWR down` / `up`. If it reads inverted (down at rest), the GPIO16 sense level in `BOARD_BTN_PWR_PRESSED_LEVEL` is wrong; note it. **Don't hold PWR**: a long press may make the AXP2101 cut power.
- [ ] The `BTN:` line on screen follows each press within 2 s.

## Waveshare ESP32-S3-Touch-AMOLED-2.16: Phase 2 pet app

Do the Phase 1 bring-up checks first: if touch or the display is wrong
there, fix the `BOARD_*` values in `include/board_amoled_216.h` before
testing the app. Then flash the pet firmware and watch the log:

```
pio run -e waveshare_amoled_216 -t upload
pio device monitor -e waveshare_amoled_216
```

Useful serial commands while testing (see `serviceSerialDebug()` in
`src/main.cpp`): `DUMP`, `DUMPBIOS <phase>`, `DUMPBATTLE <phase>` and
`DUMPSCAN <page> <count>` render a screen off-panel and send it back as
`FBDUMP 480 480` followed by the raw RGB565 frame. That is the way to look
at screens tools/sim can't render (boot log, battle scanning/hosting)
without catching them live. `POWERLOG` prints the power event log.

### Boot and display
- [ ] Serial shows `Digipet firmware v...`, `PMU` / `QMI8658` / `RTC` / `CODEC` detection lines, `Touch: CST92xx ready` (model name may differ) and `PSRAM: ... smooth transitions: ready`.
- [ ] The boot animation plays centered on the screen, with nothing cut off at any edge.
- [ ] The Companion page fills the whole 480x480 screen: title at the top, creature or egg centered in its card, the clock at top right, page dots near the bottom edge and fully visible.
- [ ] Swiping left/right moves through Companion, Status, PVP, Settings and Genome Lab. Each page fits the screen; compare with `tools/sim/render.sh <page>` output if unsure.
- [ ] The slide transitions between pages are smooth (no tearing bands or half-drawn frames).

### Touch targets
- [ ] Status page, swipe up: tapping each of the 4 action tiles flashes that tile (the highlight lines up with the tile edges) and opens the right action.
- [ ] Settings home: each tile opens its own sub-view, on both grid pages (swipe up/down to switch).
- [ ] Settings > Brightness: every one of the 11 cells is tappable and the brightness changes. Settings > Idle timer, Volume, Theme, Boot effect: every row selects, and BACK returns.
- [ ] Companion: tapping the creature card opens the Genome Profile. Long-pressing the creature (about 1 s) opens Evolution Debug; ADVANCE and CANCEL both work.
- [ ] Genome Profile data view: EXPORT and IMPORT COPY respond, BACK returns.
- [ ] Genome Lab: RANDOM, CLONE and BLEND each respond to a tap in their own third of the screen.
- [ ] Items and Recon Log open from the Status action grid; Items rows and BACK respond.
- [ ] A swipe that starts right at the screen edge still counts as a swipe.

### Buttons, sleep and wake
- [ ] Short-press **BOOT** while awake: the screen goes to sleep. Press **BOOT** again: it wakes with the "Link restored" toast.
- [ ] Same with the **user key** (Key3, GPIO18).
- [ ] On battery (USB unplugged): sleep enters deep sleep (serial ends with `entering deep sleep`). Each key on its own wakes it, and the log shows `Wake cause: ...; EXT1 status: ...` with the matching GPIO bit (bit 0 for BOOT, bit 18 for USER).
- [ ] On USB: sleep only turns the display off; a key press brings it back.
- [ ] Let the idle timer run out: the device sleeps by itself.
- [ ] The device does not wake on its own in deep sleep (leave it 5 minutes). If it does, a key line is floating low.

### Audio (ES8311)
- [ ] The boot clip plays during the boot animation; tones play on taps and page changes.
- [ ] Settings > Volume changes loudness; MUTE silences everything.
- [ ] No constant hiss or hum from the speaker while idle.

### SD card over SPI
- [ ] With a card holding `/digipet/wifi.ini` (see `sd-card/`), a cold boot logs `SD: Wi-Fi configuration valid`, connects and syncs the clock (Status/Companion shows the right time).
- [ ] With no card, boot logs `SD: card mount failed or no card present` and carries on.
- [ ] Genome Profile > EXPORT writes `/digipet/genome.txt`; IMPORT COPY reads it back ("COPIED GENOME READY").

### Time, stats and saves
- [ ] After an NTP sync, press RESET: the clock is still right (read from the PCF85063).
- [ ] After a deep-sleep cycle on battery, the clock is still right without Wi-Fi.
- [ ] Leave the pet awake for 5+ minutes: FOOD/JOY/ENERGY drop on the Status page.
- [ ] Feed or play, note the stats, press RESET: the stats, stage and settings come back the same.

### Battery readout (AXP2101)
- [ ] Status page shows a battery percentage close to the bring-up firmware's `[pmu]` reading, and serial logs `Battery: AXP2101 ready (...mV, ...%)` at boot.
- [ ] Plugging/unplugging USB updates the charging state within about 30 s.

### Wi-Fi and BLE
- [ ] Status > FEED runs the Wi-Fi + BLE scan and finds nearby networks/devices; Recon Log lists them.
- [ ] With a second Digipet (or `share/vpet-battle/vpet_battle_simulator.py`): HOST on one, FIND on the other, pick it from the opponent list, play a battle through to BATTLE COMPLETE. Move buttons respond to taps and flash when pressed.
- [ ] Friends: DISCOVER ME on one device, SCAN / PAIR BEST on the other: "FRIEND ADDED" appears and the friend shows in the directory.

### OTA
- [ ] Settings > FIRMWARE checks for an update without crashing. Only images whose manifest target is `waveshare-esp32-s3-touch-amoled-2.16` are accepted.
