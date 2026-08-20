#!/usr/bin/env bash
# Builds the native render harness and renders one page to a PNG.
#
# Usage: tools/sim/render.sh <companion|status|egg> [output.png] [stage] [theme]
#
# [theme] is one of cyber-mint|amber-core|violet-link|mono-signal (or a
# 0-3 index), forwarded to the harness as --theme=; omit it to get the
# harness's default Cyber Mint palette.
#
# Example:
#   tools/sim/render.sh companion /tmp/companion.png 3
#   tools/sim/render.sh battle-fight /tmp/battle_amber.png 2 amber-core
#
# See tools/sim/README.md for what this is, what it covers, and how to
# extend it to more pages.
set -euo pipefail
cd "$(dirname "$0")/../.."

PAGE="${1:?usage: render.sh <companion|status|egg> [output.png] [stage] [theme]}"
OUT_PNG="${2:-/tmp/digipet_sim_${PAGE}.png}"
STAGE="${3:-2}"
THEME="${4:-}"

BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

g++ -std=c++17 -O1 \
  tools/sim/render_harness.cpp \
  src/ui_pages.cpp \
  src/game/vpet/pet_genome.cpp \
  src/color_utils.cpp \
  src/game/vpet/familiar_battle_rules.cpp \
  lib/GFX_Library_for_Arduino/src/Arduino_G.cpp \
  lib/GFX_Library_for_Arduino/src/Arduino_GFX.cpp \
  lib/GFX_Library_for_Arduino/src/canvas/Arduino_Canvas.cpp \
  -I tools/sim/fakes -I lib/GFX_Library_for_Arduino/src -I include -I src \
  -o "$BUILD_DIR/render_harness"

PPM="$BUILD_DIR/frame.ppm"
if [ -n "$THEME" ]; then
  "$BUILD_DIR/render_harness" "$PAGE" "$PPM" "$STAGE" "--theme=$THEME"
else
  "$BUILD_DIR/render_harness" "$PAGE" "$PPM" "$STAGE"
fi

python3 - "$PPM" "$OUT_PNG" <<'EOF'
import sys
from PIL import Image
Image.open(sys.argv[1]).save(sys.argv[2])
print(f"wrote {sys.argv[2]}")
EOF
