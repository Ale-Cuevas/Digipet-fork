#pragma once

// Size of the pet UI and the geometry that both drawing code and touch
// hit-tests use, so the two can't drift apart. Kept free of Arduino
// includes so tools/sim's host render harness can use it too.

#include "board_amoled_216.h"

// The UI fills the whole panel.
#define UI_WIDTH BOARD_LCD_WIDTH
#define UI_HEIGHT BOARD_LCD_HEIGHT
#define UI_CENTER_X (UI_WIDTH / 2)

// Companion page: where the egg's center and the creature's feet sit,
// inside the Companion card.
constexpr int16_t COMPANION_PET_CY = 234;
constexpr int16_t COMPANION_PET_BASE_Y = 339;
// A tap inside the Companion card opens the Genome Profile; a long press
// on the creature itself opens the evolution debug overlay.
constexpr int16_t COMPANION_PROFILE_HIT_X0 = 72, COMPANION_PROFILE_HIT_X1 = 408;
constexpr int16_t COMPANION_PROFILE_HIT_Y0 = 96, COMPANION_PROFILE_HIT_Y1 = 384;
constexpr int16_t COMPANION_DEBUG_HIT_X0 = 137, COMPANION_DEBUG_HIT_X1 = 343;
constexpr int16_t COMPANION_DEBUG_HIT_Y0 = 134, COMPANION_DEBUG_HIT_Y1 = 327;

// 2x2 tile grid shared by the Status actions, Battle actions and Settings
// home pages.
constexpr int16_t UI_TILE_W = 209, UI_TILE_H = 147, UI_TILE_RADIUS = 20;
constexpr int16_t UI_TILE_X[] = {23, 248};
constexpr int16_t UI_TILE_Y[] = {84, 243};

// Which tile (0-3, row by row) a point falls in, or -1 if it is above or
// below the grid. Columns split at the screen's midpoint and rows at the
// top of the second row, so a tap in a gap still picks the nearest tile.
constexpr int8_t uiTileAt(int32_t x, int32_t y) {
  return (y < UI_TILE_Y[0] || y >= UI_TILE_Y[1] + UI_TILE_H)
             ? -1
             : (y >= UI_TILE_Y[1] ? 2 : 0) + (x >= UI_CENTER_X ? 1 : 0);
}

// Battle: the 2x2 move grid (ATK/DEF, SPEC/FLEE) and the two buttons on
// the result screen.
constexpr int16_t BATTLE_BTN_W = 208, BATTLE_MOVE_H = 58, BATTLE_BTN_RADIUS = 14;
constexpr int16_t BATTLE_BTN_X[] = {24, 248};
constexpr int16_t BATTLE_MOVE_Y[] = {296, 362};
constexpr int16_t BATTLE_RESULT_BTN_Y = 330, BATTLE_RESULT_BTN_H = 58;

// Which move button (0-3, row by row) a point falls in, or -1. Same
// nearest-button rule as uiTileAt().
constexpr int8_t battleMoveAt(int32_t x, int32_t y) {
  return (y < BATTLE_MOVE_Y[0] - 4 || y > BATTLE_MOVE_Y[1] + BATTLE_MOVE_H + 4)
             ? -1
             : (y >= BATTLE_MOVE_Y[1] - 4 ? 2 : 0) + (x >= UI_CENTER_X ? 1 : 0);
}

// BACK/CANCEL button at the bottom of overlays and settings sub-views. Any
// tap from UI_BACK_HIT_Y down counts as BACK.
constexpr int16_t UI_BACK_X = 110, UI_BACK_Y = 412, UI_BACK_W = 260, UI_BACK_H = 47;
constexpr int16_t UI_BACK_HIT_Y = UI_BACK_Y - 10;

// Battle opponent picker: up to kBattleResultsPerPage rows, CANCEL at the
// UI_BACK_* position.
constexpr int16_t BATTLE_PICKER_Y0 = 71, BATTLE_PICKER_PITCH = 81, BATTLE_PICKER_ROW_H = 73;

// Friends overlay: BACK, the DISCOVER ME button (page 2) and the SCAN /
// PAIR BEST buttons (page 3).
constexpr int16_t FRIENDS_BACK_Y = 416;
constexpr int16_t FRIENDS_DISCOVER_BTN_Y = 289, FRIENDS_DISCOVER_BTN_H = 69;
constexpr int16_t FRIENDS_NEARBY_BTN_Y = 328, FRIENDS_NEARBY_BTN_H = 68;

// Items overlay rows.
constexpr int16_t ITEMS_ROW_Y0 = 76, ITEMS_ROW_PITCH = 52;

// Genome Profile data view: EXPORT / IMPORT COPY row and BACK.
constexpr int16_t GENOME_DATA_BTN_Y = 289, GENOME_DATA_BTN_H = 50, GENOME_DATA_BACK_Y = 426;

// Evolution debug overlay: ADVANCE STAGE button and CANCEL below it.
constexpr int16_t EVO_DEBUG_ADVANCE_Y = 317, EVO_DEBUG_ADVANCE_H = 62, EVO_DEBUG_CANCEL_Y = 402;

// Settings sub-views: the brightness grid (3 columns, 11 cells) and the
// one-per-row choice lists.
constexpr int16_t SETTINGS_BRIGHT_X0 = 27, SETTINGS_BRIGHT_PITCH_X = 147, SETTINGS_BRIGHT_W = 132;
constexpr int16_t SETTINGS_BRIGHT_Y0 = 89, SETTINGS_BRIGHT_PITCH_Y = 73, SETTINGS_BRIGHT_H = 57;
constexpr int16_t SETTINGS_CHOICE_H = 51;
constexpr int16_t SETTINGS_IDLE_Y0 = 95, SETTINGS_IDLE_PITCH = 66;  // 4 rows
constexpr int16_t SETTINGS_LIST_Y0 = 84, SETTINGS_LIST_PITCH = 61;  // volume, theme: 5 rows
constexpr int16_t SETTINGS_BOOT_ON_Y = 139, SETTINGS_BOOT_OFF_Y = 213;

// Genome Lab: the RANDOM / CLONE / BLEND row along the bottom. Taps pick
// a button by which third of the screen they land in.
constexpr int16_t GENOME_LAB_BTN_X0 = 23, GENOME_LAB_BTN_PITCH = 148;
constexpr int16_t GENOME_LAB_BTN_W = 136, GENOME_LAB_BTN_Y = 402, GENOME_LAB_BTN_H = 46;
