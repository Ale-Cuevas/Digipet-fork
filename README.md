# Digipet

```
┌──────────────────────────────────────────┐
│  D I G I P E T                           │
│  companion firmware // ESP32-S3          │
└──────────────────────────────────────────┘
```

A virtual pet that lives entirely on an ESP32-S3. No app, no account, no
cloud: just a touchscreen, a companion built from its own procedurally
generated genome, and firmware written to make it feel like something is
actually in there.

It targets the Waveshare ESP32-S3 Touch AMOLED 1.8 inch board (V2: CO5300
display, CST820 touch). The presentation takes cues from Digivice-era
hardware pets, but every creature, egg, and animation here is generated at
runtime. No Digimon artwork, no proprietary assets, nothing borrowed.

## What it does

Power it on and a five-second boot sequence (real audio, BIOS-style sync
log) hands off to your companion. From there:

- Feed, play, and train it through animated Companion and Status pages
- Battle another Digipet live over BLE on a dedicated PVP page, with
  stats, elements, and move matchups negotiated between the two devices
- Scan for nearby WiFi and BLE devices to earn food, and rarer items that
  permanently boost a stat or shift the pet's type
- Trade friend codes over a live BLE handshake, and track past opponents
  on a Rivals page
- Hatch new eggs by cloning or blending a genome, with a chance to
  inherit stats from a companion that reached its final stage
- Inspect a full Genome Profile, export it to SD, or import one to clone
  or blend into a new egg
- Read real hardware diagnostics for the IMU, RTC, power controller, and
  audio codec
- Check for and install signed firmware updates, manually, from Settings

Every pet, egg, and battle record persists through ESP32 Preferences/NVS.
Poop and illness mechanics are intentionally excluded.

## Hardware

This firmware targets the V2 board:

```text
Waveshare ESP32-S3-Touch-AMOLED-1.8
Display: CO5300, 368 x 448
Touch:   CST820/CST816-compatible controller
MCU:     ESP32-S3 with 8 MB PSRAM and 16 MB flash
```

It is not intended for the visually similar V1 display variant.

## Build and flash

Install [PlatformIO](https://platformio.org/), connect the board, then run:

```bash
pio run
pio run -t upload --upload-port /dev/ttyACM0
pio device monitor --port /dev/ttyACM0 --baud 115200
```

Change the serial device where required for your operating system.

The first build downloads NimBLE-Arduino. Board-specific display, touch, and
bus libraries are vendored under `lib/` because this board requires Waveshare
drivers that are not fully represented by the generic PlatformIO board target.

## Optional Wi-Fi and time setup

Copy the example tree under [`sd-card`](sd-card) to the microSD card, rename
`digipet/wifi.ini.example` to `digipet/wifi.ini`, and fill in your own
credentials.

The device reads the file during boot, connects just long enough to
synchronise its clock, then disables Wi-Fi again. If the card is missing,
the connection fails, or sync doesn't happen, the device carries on offline
without complaint.

Genome Lab can also export the active pet to `digipet/genome.txt` or import
that file as a copied genome. A credential-free format example is included
at `sd-card/digipet/genome.txt.example`. Imported genomes can be cloned
exactly or blended with the active pet when hatching a new egg.

Never commit a populated `wifi.ini`. The `provisioning/` directory is
ignored, for developers who want a local staging area for private device
files.

## Cross-device battle package

The live firmware's BLE battle logic lives in
`include/familiar_battle_service.h` and `src/familiar_battle_service.cpp`.
A self-contained handoff package for other ESP32 developers is available
under [`share/vpet-battle`](share/vpet-battle), with integration
instructions and a compatible Python BLE simulator included.

Battle rendering is device-specific; the shared service owns everything
else: discovery, handshaking, moves, deterministic turn resolution, stats,
and outcomes. Optional body, element, speed, special, and move-matchup
rules are negotiated per battle, so a peer that only implements HP, attack,
and defense still battles fine on the unmodified core rules.

## Testing

`pio run` builds the real firmware. `pio test -e native` runs a separate
host-native suite covering everything hardware-independent: the genome
codec, blending, and evolution drift in `pet_genome.cpp` (with a stubbed
`esp_random()` for determinism, defined in `test/native/fakes`), the Direct
Challenge stat formulas, matchup tables, and turn resolution in
`familiar_battle_rules.cpp`, and the contrast-aware label color picker in
`color_utils.cpp`. None of it touches the ESP32 toolchain or NimBLE/Wi-Fi,
so it stays fast and needs no board. CI (`.github/workflows/build.yml`) runs
both suites on every push and PR.

`tools/sim/render.sh <page> [output.png] [stage] [theme]` renders any page
with the real drawing code (`src/ui_pages.cpp`, extracted from `main.cpp`)
compiled natively, for a visual look at a UI change without flashing a
board. Pass `[theme]` (`cyber-mint`, `amber-core`, `violet-link`, or
`mono-signal`) to check it against a specific fixed palette. See
`tools/sim/README.md` and [`docs/style-guide.md`](docs/style-guide.md) for
the UI conventions, color and contrast rules especially, a page renderer is
expected to follow.

## Repository layout

```text
include/             Public firmware headers
src/                 Digipet firmware and battle service
lib/                 Vendored Waveshare-compatible device libraries
examples/            Credential-free configuration examples
share/vpet-battle/   Reusable battle service handoff package
sd-card/             Credential-free SD-card layout example
release/             Public firmware verification material
scripts/             Release-manifest tooling
test/                Host-native unit tests (env:native)
platformio.ini       PlatformIO build configuration
```

## Signed releases

Tagged releases are built and signed by GitHub Actions. See
[`RELEASES.md`](RELEASES.md) for key custody, release creation, asset
formats, and the security rules planned for the self-updater.

## License

Digipet is released under the MIT License. Vendored libraries retain their
own license files and copyright notices.
