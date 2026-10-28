#pragma once

// Size and placement of the pet UI on the panel. Kept free of Arduino
// includes so tools/sim's host render harness can use it too.

#include "board_amoled_216.h"

// The UI is still laid out for the 368x448 screen Digipet was designed on.
// Until it is relaid out for the full panel it runs in a window of that
// size, centered on the 480x480 panel; the display driver applies the
// offset, so drawing code only ever sees UI coordinates.
#define UI_WIDTH 368
#define UI_HEIGHT 448
#define UI_ORIGIN_X ((BOARD_LCD_WIDTH - UI_WIDTH) / 2)
#define UI_ORIGIN_Y ((BOARD_LCD_HEIGHT - UI_HEIGHT) / 2)
