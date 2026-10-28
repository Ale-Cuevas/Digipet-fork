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
