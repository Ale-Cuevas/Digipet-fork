# Pending tests: run these when the board arrives

Everything below has been written and compiled, but has never run on a real
Waveshare ESP32-S3-Touch-AMOLED-2.16. Work through it top to bottom on day
one. The step-by-step checks for each item are in
[DEVICE_TESTS.md](DEVICE_TESTS.md); this file is the to-do list and the
place to record results.

When an item passes, tick it here and in `DEVICE_TESTS.md`, and tick the
matching Phase box in `CLAUDE.md`. When one fails, write what you saw on
the line, so it can be fixed.

## Before you start

- [ ] USB-C cable that carries data (not a charge-only cable).
- [ ] A 3.7 V LiPo with an **MX1.25 2-pin** plug, polarity checked against the board silkscreen before plugging it in. Optional for the first flash.
- [ ] Repo on branch `dev`, `pio run -e waveshare_amoled_216` builds.

## First flash

```
pio run -e waveshare_amoled_216 -t upload
pio device monitor -e waveshare_amoled_216
```

If the upload can't find the board, hold **BOOT**, tap **RESET** (or plug in USB while holding BOOT), release BOOT, and upload again.

## Pending: Phase 1 bring-up (firmware in `src/bringup_216/`)

| # | Item | Details | Result |
|---|---|---|---|
| 1 | Power chip (AXP2101) found, voltages plausible | [Power](DEVICE_TESTS.md#power-axp2101) | |
| 2 | Display lights up, colors correct, no offset | [Display](DEVICE_TESTS.md#display-co5300-480x480) | |
| 3 | Touch found, dot appears under finger | [Touch](DEVICE_TESTS.md#touch-cst9220) | |
| 4 | IMU found, ~1 g when flat | [IMU](DEVICE_TESTS.md#imu-qmi8658) | |
| 5 | RTC found, keeps time across reset | [RTC](DEVICE_TESTS.md#rtc-pcf85063) | |
| 6 | BOOT, USER and PWR buttons register | [Buttons](DEVICE_TESTS.md#buttons) | |

## Questions only the board can answer

These were taken from the schematic or demo code but not confirmed. Write the answer next to each one.

- [ ] Where is the USB-C port when the red corner square is top-left? (top / bottom / left / right): ____
- [ ] Do touch dots land under the finger, or mirrored/rotated? If wrong, which way: ____
- [ ] Touch model name printed at boot: ____
- [ ] PWR button on GPIO16: reads `down` only while pressed (expected), or inverted? ____
- [ ] IMU lying flat face up: which axis reads +1 g, and with what sign? ____
- [ ] RTC: does the time survive a full power-off (no USB, no battery)? ____
- [ ] Anything else odd (flicker, noise bands, hot chips, brownouts on battery): ____

## Pending from later phases

Nothing yet. As Phase 2+ work lands compile-only, add a row here pointing to its section in `DEVICE_TESTS.md`.
