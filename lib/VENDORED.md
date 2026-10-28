# Vendored libraries

These libraries are copied in rather than fetched by PlatformIO because this
board needs Waveshare's driver set. Last audited 2026-10-28 by diffing each
directory against its source. Nothing found beyond what is listed here.

| Library | Version | Copied from | Matches |
|---|---|---|---|
| `Adafruit_XCA9554` | 1.0.0 | [waveshareteam/ESP32-S3-Touch-AMOLED-1.8](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8) `examples/arduino-v2/libraries/` | Identical to Waveshare and to upstream [adafruit/Adafruit_XCA9554](https://github.com/adafruit/Adafruit_XCA9554) tag `1.0.0` |
| `Arduino_DriveBus` | 1.0.1 | same Waveshare path | Identical to Waveshare. The `library.properties` upstream (`Xk-w/Arduino_DriveBus`) no longer exists; the author now publishes as [Llgok/Arduino_DriveBus](https://github.com/Llgok/Arduino_DriveBus) at a newer version |
| `GFX_Library_for_Arduino` | 1.6.4 | same Waveshare path | Identical to Waveshare. Against upstream [moononournation/Arduino_GFX](https://github.com/moononournation/Arduino_GFX) tag `v1.6.4`: see below |

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
