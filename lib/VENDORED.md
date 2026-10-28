# Vendored libraries

This library is copied in rather than fetched by PlatformIO because the
CO5300 driver comes from Waveshare's driver set. Its CO5300 driver was
checked to match the copy in Waveshare's 2.16" demo. `Adafruit_XCA9554`
and `Arduino_DriveBus` (1.8" expander and touch) were removed in the
2.16" port. Last audited 2026-10-28 by diffing each
directory against its source. Nothing found beyond what is listed here.

| Library | Version | Copied from | Matches |
|---|---|---|---|
| `GFX_Library_for_Arduino` | 1.6.4 | [waveshareteam/ESP32-S3-Touch-AMOLED-1.8](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8) `examples/arduino-v2/libraries/` | Identical to Waveshare. Against upstream [moononournation/Arduino_GFX](https://github.com/moononournation/Arduino_GFX) tag `v1.6.4`: see below |

Only non-code files (examples, `keywords.txt`, `gen_keywords.pl`,
`code-of-conduct.md`) were left out of the copies.

## Differences from upstream Arduino_GFX v1.6.4

- Most `src/` files use CRLF line endings; otherwise identical.
- `src/databus/Arduino_ESP32SPI.cpp` and `src/databus/Arduino_ESP32SPIDMA.cpp`
  add a `gfxSpiFrequencyToClockDiv()` wrapper that calls the two-argument
  `spiFrequencyToClockDiv(spi, freq)` on arduino-esp32 >= 3.3.10 and the old
  one-argument form otherwise. Needed for the pinned 3.3.11 core.

## Re-auditing

Download the Waveshare repo and the upstream tags into a scratch directory
and run `diff -rq --strip-trailing-cr <upstream> lib/<name>`. Update this file
whenever a library is replaced or patched.

## Registry libraries

`[env:waveshare_amoled_216]` and `[env:waveshare_amoled_216_bringup]` take these from the PlatformIO registry instead
of vendoring them, pinned to the versions Waveshare's
[ESP32-S3-Touch-AMOLED-2.16](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16)
demo ships in `examples/arduino/libraries/` (checked at commit `225a62b`):

| Library | Version | Against the Waveshare copy |
|---|---|---|
| `lewisxhe/SensorLib` (MIT) | 0.3.3 | Identical `src/` (ignoring line endings) |
| `lewisxhe/XPowersLib` (MIT) | 0.2.6 | Waveshare fixes the spelling of a few method names in the AXP192/AXP202/AXP2101 drivers (`eanblePowerOffDelay` -> `enablePowerOffDelay`, `...PowerDowm` -> `...PowerDown`, and others). No behaviour changes. Use the registry spellings. |
