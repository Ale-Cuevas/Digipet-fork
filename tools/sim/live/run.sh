#!/usr/bin/env bash
# Builds and starts the interactive simulator: the real firmware in a
# 480x480 window, mouse = touch, keyboard = keys. Saves live in .sim/.
#
#   tools/sim/live/run.sh                 play (window; needs libsdl2-dev)
#   tools/sim/live/run.sh --usb --scale 2 on USB power, 960x960 window
#   tools/sim/live/run.sh --fresh         start over with a new egg
#   tools/sim/live/run.sh --script s.txt  headless scripted run
#
# See tools/sim/README.md ("Interactive simulator") for keys and options.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BUILD="$ROOT/.pio/sim-live"

binary=digipet-sim
for arg in "$@"; do
  [ "$arg" = "--script" ] && binary=digipet-sim-headless
done

if [ "$binary" = digipet-sim ]; then
  make -s -C "$ROOT/tools/sim/live" -j"$(nproc)" window || {
    echo "The window build needs SDL2: sudo apt install libsdl2-dev pkg-config" >&2
    exit 1
  }
else
  make -s -C "$ROOT/tools/sim/live" -j"$(nproc)" headless
fi

has_data=false
for arg in "$@"; do [ "$arg" = "--data" ] && has_data=true; done
if $has_data; then
  exec "$BUILD/$binary" "$@"
else
  exec "$BUILD/$binary" --data "$ROOT/.sim" "$@"
fi
