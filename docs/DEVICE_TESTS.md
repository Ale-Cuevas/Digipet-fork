# Device tests

Checks that can only be done on the physical board. Tick each box once it
passes on the device. If one fails, note what you saw next to it.

## Waveshare ESP32-S3-Touch-AMOLED-2.16: Phase 1 bring-up

Flash and watch the log:

```
pio run -e waveshare_amoled_216 -t upload
pio device monitor -e waveshare_amoled_216
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
