# Native render harness

Renders the Companion, Status, Settings, Genome Lab, and Battle screens
using the *real* production drawing code (`src/ui_pages.cpp`, extracted
verbatim from `main.cpp`) compiled for the host instead of the ESP32 -- no
board, no flash cycle. Useful for visually reviewing a UI change before
flashing, or for generating reference images to compare across changes.

## Use

```bash
tools/sim/render.sh companion /tmp/companion.png     # stage defaults to 2
tools/sim/render.sh status /tmp/status.png 4          # stage 4 = Titan
tools/sim/render.sh egg /tmp/egg.png                  # always stage 0
tools/sim/render.sh settings /tmp/settings.png        # home grid, page 1/2
tools/sim/render.sh settings2 /tmp/settings2.png      # home grid, page 2/2
tools/sim/render.sh settings-brightness /tmp/b.png    # any settingsView sub-view:
tools/sim/render.sh settings-idle /tmp/i.png          #   brightness|idle|volume|
tools/sim/render.sh settings-volume /tmp/v.png        #   theme|boot
tools/sim/render.sh settings-theme /tmp/t.png
tools/sim/render.sh settings-boot /tmp/o.png
tools/sim/render.sh genomelab /tmp/genomelab.png
tools/sim/render.sh battle-fight /tmp/fight.png                 # mid-battle;
tools/sim/render.sh battle-fight-highstats /tmp/f2.png          #  also -lowhp|
tools/sim/render.sh battle-result /tmp/result.png                #  -submitted|
tools/sim/render.sh battle-result-copied /tmp/result2.png        #  -fleearmed|
tools/sim/render.sh battle-picker3 /tmp/picker.png                #  -nogenome
tools/sim/render.sh battle-picker7 /tmp/picker7-p2.png 1   # [stage] arg = results page
tools/sim/render.sh rivals /tmp/rivals.png 3          # [stage] arg = rival count (max 8)
tools/sim/render.sh rivals-empty /tmp/rivals-empty.png
tools/sim/render.sh status-actions /tmp/actions.png   # Status page's action grid, page 1
tools/sim/render.sh status-actions2 /tmp/actions2.png #  ...page 2 (ITEMS + 3 locked tiles)
tools/sim/render.sh recon /tmp/recon.png 5            # [stage] arg = signal count (max 8)
tools/sim/render.sh recon-empty /tmp/recon-empty.png
tools/sim/render.sh friends /tmp/friends.png 5        # [stage] arg = friend count (max 8)
tools/sim/render.sh friends-empty /tmp/friends-empty.png
tools/sim/render.sh friends-discover /tmp/discover.png # Discoverable page
tools/sim/render.sh friends-nearby /tmp/nearby.png      # radar empty state
tools/sim/render.sh items /tmp/items.png              # fixed sample counts/bonuses
tools/sim/render.sh items-empty /tmp/items-empty.png
tools/sim/render.sh genome-identity /tmp/genome-id.png    # Genome Profile, identity sub-view
tools/sim/render.sh genome-data /tmp/genome-data.png      #  ...genome-code/EXPORT/IMPORT sub-view

# Any of the above also takes a 4th [theme] arg -- cyber-mint|amber-core|
# violet-link|mono-signal, or a kThemes[] index 1-4 (see include/ui_pages.h)
# -- to re-point every COLOR_* global the same way Settings > Theme does,
# without touching the file's own Cyber Mint-seeded globals. Omit it for
# that default. See ../../docs/style-guide.md for why checking a UI change
# against all 4 fixed themes, not just this default one, matters.
tools/sim/render.sh battle-fight /tmp/fight_amber.png 2 amber-core
```

Output is a real PNG at the panel's native 480x480, pixel-accurate RGB565
(same 16-bit color the panel actually gets sent -- not the AMOLED's own
color reproduction, which this can't capture).

## What this does and doesn't prove

**Proves:** layout, procedural creature/egg rendering across stage and
genome, color/theme values, text placement -- anything that's a pure
function of `pet`/theme-color state. If a change moves something off
Compositions screen or breaks a creature's silhouette, this will show it
immediately without a flash cycle.

**Doesn't prove:** touch feel, real elapsed-time animation, BLE battles
between two peers, or how the AMOLED itself reproduces these colors. Those
still need the physical board (see the root README's flashing instructions)
or, for protocol-only testing, `share/vpet-battle/vpet_battle_simulator.py`.

## How it works

`Arduino_Canvas` (the library's in-memory framebuffer class the real
firmware already renders every page into before blitting to the panel) has
no hardware dependency of its own -- it's pure pixel-array arithmetic. The
only thing standing between that and a native build was the rest of the
Arduino core (`String`, `Print`, `PROGMEM`, `millis()`) that
`Arduino_GFX`/the drawing code expect to exist. `tools/sim/fakes/` supplies
minimal stand-ins for exactly that surface (see each file's own comment) --
not a general Arduino compatibility layer, just enough for this.

`render_harness.cpp` defines the same global variables `include/ui_pages.h`
declares `extern` (colors, `pet`, sensor-detected flags, etc.) with fixed
test values, then calls the real page functions directly and dumps the
resulting canvas to a PPM (converted to PNG by `render.sh` via Pillow).

## Extending to more pages

Currently covers Companion (all 5 stages and all 5 body types' procedural
creature rendering), Status, the egg-stage Companion screen (all 10 egg
lineages), Settings (both home grid pages and all 5 settingsView
sub-views), Genome Lab, and Battle's Battling/Result/opponent-picker
states (drawBattlingLayout/drawBattleResultsPage/drawOpponentRow/
drawBattleButton -- already parameter-driven rather than reading the live
`battle` object, the same design that already let main.cpp's own
DUMPBATTLE/DUMPSCAN debug commands preview them, so no
`FamiliarBattleService` stand-in was actually needed). Battle's Idle/
Scanning/Hosting/Connecting states (drawBattlePage() itself, which *does*
read `battle` directly) and the boot sequence aren't extracted yet -- the
former needs a renderable `FamiliarBattleService` stand-in, the latter is
inherently animated-over-time rather than one static frame. To extend:

1. Identify the page function's dependencies the same way this pass did:
   `grep` the function body for globals it reads that aren't already
   declared in `include/ui_pages.h`.
2. Move the function (and any drawing-only helpers it calls that aren't
   already moved) from `src/main.cpp` into `src/ui_pages.cpp`, verbatim --
   no logic changes.
3. Add `extern` declarations for any newly-needed state to
   `include/ui_pages.h`, and function prototypes for anything moved.
4. `pio run -e waveshare_amoled_216` to confirm the firmware still builds
   (should be a near-zero size delta -- code moved, not changed).
5. Define harness-side fakes for the new state in `render_harness.cpp`
   (or a fake object matching the real type's public interface, the way a
   `FamiliarBattleService` stand-in would need to for Battle).

# Interactive simulator

`tools/sim/live/` runs the **whole firmware** on your PC: the real
`setup()`/`loop()` from `src/main.cpp` and the vpet game module, unchanged,
against a modelled AMOLED-2.16. You play it in a 480x480 window: the mouse
is your finger and the keyboard is the board's keys. Boot animation, page
swipes, overlays, sleep/wake, saves, feeding scans and battles all run the
same code the board runs.

## Setup (once)

```bash
sudo apt install libsdl2-dev pkg-config
```

## Use

```bash
tools/sim/live/run.sh                  # play
tools/sim/live/run.sh --usb            # start on USB power (no deep sleep on idle)
tools/sim/live/run.sh --scale 2        # 960x960 window
tools/sim/live/run.sh --fresh          # wipe the save, new egg
tools/sim/live/run.sh --speed 60       # start fast-forwarded (pet decay 60x)
```

| Input | Board equivalent |
|---|---|
| click / drag | touch tap / swipe (hold ~1 s on the pet = long press) |
| Space or B | BOOT key (power key: sleep now / wake) |
| N | user key (GPIO18, same role) |
| P | plug / unplug USB power |
| F | fast-forward on/off (`--ff`, default 60x) |
| `-` / `=` | battery -10% / +10% |
| S | screenshot to `.sim/` |
| R | power-cycle (cold boot) |
| Esc | quit |
| type in the terminal | Serial debug commands: `WHOAMI`, `POWERLOG`, `SIMBATTLE 1`, ... |

The save (NVS) lives in `.sim/nvs.bin` and the SD card in `.sim/sd/`
(e.g. `.sim/sd/digipet/genome.txt` after a genome export), so the pet
persists between runs exactly as it does between reboots.

## What's modelled

- **Display:** the CO5300 is a framebuffer the window shows; brightness and
  display on/off follow the firmware's calls.
- **Touch, keys:** CST9220 one-finger touch from the mouse (with its INT
  line), BOOT/USER keys active-low.
- **I2C:** the six real addresses answer the boot scan. The PCF85063 RTC
  keeps host local time; the AXP2101 status, voltage and fuel-gauge registers
  report the simulated battery/USB state; the ES8311 volume register scales
  the sound.
- **Audio:** tones and the boot clip play on your speakers, blocking like
  the real I2S does.
- **Deep sleep:** like the board, sleep on battery is a reboot. The window
  goes dark until you press a key, then the sim re-executes itself as a
  deep-sleep resume. `RTC_DATA_ATTR` memory is carried across, as RTC slow
  memory is on the chip.
- **Wi-Fi:** scans see a fixed set of fake access points; joining never
  succeeds, so NTP sync and OTA report a Wi-Fi failure.
- **BLE battles:** `sim_battle_service.cpp` replaces the BLE transport with
  a bot. FIND lists three bots; HOST gets a bot challenger after ~3.5 s;
  Friends discovery completes an exchange. Turns resolve through the real
  `resolveBattleTurn()`, so damage and misses match a real match.

**Not modelled:** the AMOLED's own colour/brightness response, real touch
feel, the IMU (detected but never read by the app), and the wire protocol
between two badges (use `share/vpet-battle/vpet_battle_simulator.py`).
`time(nullptr)` is the host clock, whereas on the board it only becomes
wall time after an NTP sync. That only shows in the player-ID timestamp and
power telemetry.

## Scripted runs (no window)

`--script FILE` runs headless (no SDL needed) and drives the firmware from a
list of steps, in emulated milliseconds:

```
wait 9000              # boot animation + boot log
shot /tmp/home.bmp
swipe 400 240 80 240   # to Status
hold 240 230 1000      # long press
tap 130 160
key boot               # also wakes it from deep sleep
usb on                 # | usb off
battery 15
speed 60
quit
```

```bash
tools/sim/live/run.sh --usb --fresh --script my_flow.txt
```

Use `--usb` unless the script is testing sleep: on battery the default
15-second idle timeout puts the board into deep sleep. A script does keep
running through it, and its next `key` step wakes the board.

## How it builds

`make -C tools/sim/live` compiles the firmware sources listed in its
Makefile with `tools/sim/live/fakes/` (ahead of `tools/sim/fakes/`) on the
include path. Those headers stand in for the ESP32 Arduino core, NVS, I2C,
SD, Wi-Fi, I2S, XPowersLib and SensorLib. Nothing in `src/` is modified or
`#ifdef`ed for the sim. The boot clip and mascot frames are linked in with
`ld -r -b binary`, under the same symbol names `board_build.embed_files`
gives them. Objects go to `.pio/sim-live/`, and only changed files rebuild.
