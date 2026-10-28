#pragma once

#include <vector>

#include <Arduino.h>
#include <Arduino_GFX.h>

#include "color_utils.h"
#include "familiar_battle_service.h"
#include "game/vpet/pet_genome.h"
#include "game/vpet/vpet_state.h"
#include "ui_layout.h"

// The Companion/Status page renderers and their shared drawing helpers,
// factored out of main.cpp so they can be linked into something other than
// the firmware -- specifically tools/sim's native render harness, which
// renders the real page-drawing code to PNG for visual review without
// flashing a physical board. See tools/sim/README.md.
//
// This split follows the same "move code verbatim, declare what it reads
// as extern" pattern already used for familiar_battle_rules.h: main.cpp
// still owns every one of these globals (constructs them, mutates them,
// persists them) -- this header only declares their shape and existence so
// a second translation unit (either main.cpp's own, now living in
// src/game/vpet/ui_pages.cpp, or the sim harness with its own fake
// definitions) can link against them.
//
// PetState/BattleStats/ReconLog/FriendsList/Inventory themselves are
// defined in game/vpet/vpet_state.h, not here -- those are vpet's own
// state shapes (part of the console/game seam), while Page below is a
// console-owned slot enum every game module's pages get dispatched
// through, so it stays in this, the console-facing header.

enum Page : uint8_t {
  PAGE_COMPANION,
  PAGE_STATUS,
  PAGE_BATTLE,
  PAGE_SETTINGS,
  PAGE_GENOME_LAB,
};

enum StatIcon : uint8_t { ICON_FOOD, ICON_JOY, ICON_ENERGY, ICON_HEALTH };

// --- State these renderers read ---------------------------------------------
// Definitions live in src/main.cpp for the real firmware; tools/sim's
// harness provides its own (see tools/sim/render_harness.cpp).

extern Arduino_GFX *display;
extern PetState pet;
extern FamiliarBattleService battle;

// Status page has two sub-views, the same "peer sub-views, swipe between
// them, no back button" shape as Settings' own home-grid paging (see
// transitionSettingsGrid()) rather than a drill-down: false is the stats
// summary (identity, stat bars, diagnostics), true is the action grid.
// drawStatusPage() reads this directly; main.cpp's transitionStatusView()
// is what actually flips it.
extern bool statusShowingActions;

// The action grid itself is *also* Settings-home-grid-shaped: two pages of
// four tiles, only meaningful while statusShowingActions is true. Page 0 is
// FEED/PLAY/TRAIN/RECON LOG; page 1 is ITEMS plus three tiles reserved for
// future actions (drawn locked/dimmed, same visual language Genome Lab's
// own unavailable CLONE/BLEND tiles already use). main.cpp's
// transitionStatusActionsGrid() is what changes this.
extern uint8_t statusActionsPage;

// Same "peer sub-views, swipe between them" shape as statusShowingActions
// just above, applied to the Battle page's Idle state: false is the
// battle-stats sheet (LV/HP/ATK/DEF/SPECIAL/TYPE, all at once, no
// cramped one-line header), true is the HOST/FIND/RIVALS/FRIENDS action
// grid. Only meaningful while battle.state() == Idle -- every other
// battle state (Scanning/Hosting/Connecting/Battling/Result/Exchanged)
// draws its own dedicated full-screen content regardless of this flag.
// drawBattleStatsView()/drawBattleActionsView() don't read this
// themselves (main.cpp's drawBattlePage() is the dispatcher, same
// division of responsibility drawBattlingLayout() already has); main.cpp's
// transitionBattleView() is what flips it.
extern bool battleShowingActions;

// Same "peer sub-views, swipe between them" shape again, for the Genome
// Profile overlay: false is identity (FORM/ELEMENT/BODY/TEMPERAMENT/
// TRAITS/MUTATION, one per row, all at once instead of two per row jammed
// together with "//"), true is genome data (the 60-character code,
// design ID, and the EXPORT/IMPORT COPY actions). drawGenomeProfilePage()
// is the dispatcher (this file); main.cpp's transitionGenomeProfileView()
// flips this and its own exportActiveGenome()/importCopiedGenome() (SD
// card I/O, hardware territory) stay there.
extern bool genomeProfileShowingData;
// SD export/import's own one-line status ("EXPORTED TO SD", "COPIED
// GENOME READY", ...), shown on the data sub-view above. Definition
// (main.cpp) also updates it from exportActiveGenome()/
// importCopiedGenome(), which is why it isn't just a return value.
extern char genomeTransferStatus[32];

extern uint16_t COLOR_BACKGROUND;
extern uint16_t COLOR_CARD;
extern uint16_t COLOR_MINT;
extern uint16_t COLOR_TEXT;
extern uint16_t COLOR_MUTED;
extern uint16_t COLOR_WARNING;
extern uint16_t COLOR_DANGER;
extern uint16_t COLOR_CYAN;
extern uint16_t COLOR_PURPLE;

extern const char *STAGE_NAMES[];

extern bool clockValid;
extern char clockText[6];

extern bool imuDetected;
extern bool rtcDetected;
extern bool pmuDetected;
extern bool codecDetected;
extern uint8_t i2cDeviceCount;
// Battery state, read from the AXP2101 PMU over I2C -- see main.cpp's
// updateBatteryReading(). Only meaningful once pmuDetected is true.
extern bool batteryCharging;
extern bool externalPowerPresent;
extern uint8_t batteryPercent;
extern uint16_t batteryVoltageMv;
// Runway estimate derived from the drain rate between recent battery
// percentage transitions -- see main.cpp's updateBatteryTimeEstimate().
// batteryMinutesRemaining is only meaningful when batteryTimeEstimateValid
// is true (a synced clock, at least one observed transition, and not
// currently charging).
extern bool batteryTimeEstimateValid;
extern uint32_t batteryMinutesRemaining;
// Minutes waited so far since the drain-rate tracking last reset its
// anchor point -- shown on-screen while !batteryTimeEstimateValid so
// there's visible progress instead of a static "still calculating" that
// looks the same whether it's been a minute or an hour.
extern uint32_t batteryMinutesSinceTransition;

extern bool animationFrame;

// BattleStats/ReconLog/FriendsList/Inventory's shapes live in
// game/vpet/vpet_state.h (included above) -- these per-device records are
// magic-guarded, persisted through Preferences/NVS, defined in main.cpp;
// drawRivalsPage()/drawReconLogPage()/drawFriendsPage()/drawItemsPage()
// read them directly via these externs.
extern BattleStats battleStats;
extern ReconLog reconLog;
extern FriendsList friendsList;
extern uint8_t friendsPage;
extern bool friendDiscoverable;
extern Inventory inventory;

struct DeviceSettings {
  uint32_t magic;
  uint8_t brightnessIndex;
  uint8_t sleepIndex;
  bool soundEnabled;
  bool bootAnimationEnabled;
  uint8_t volumeIndex;
  // No longer read anywhere -- Wake Mode was retired (touch can't wake the
  // device at all now, only the power key). Left in place rather than
  // removed so this struct's layout, and every offset after it, doesn't
  // need yet another settings-schema migration for a single dead byte.
  uint8_t wakeMode;
  uint8_t themeIndex;
};
extern DeviceSettings settings;

extern const char *SLEEP_LABELS[];
extern const char *VOLUME_LABELS[];
extern const char *THEME_LABELS[];
extern uint8_t settingsGridPage;
uint8_t brightnessPercent();

// Canonical color tables for the app's 5 themes, index-aligned with
// THEME_LABELS/settings.themeIndex. Index 0 (AUTO) has no fixed palette --
// it's derived per-pet from paletteForGenome() instead (see applyTheme()
// and drawSettingsControlPage()'s own handling of it) -- so its entry here
// is a placeholder, not a color to draw with.
//
// This is the one place this data lives, specifically so it can't drift
// out of sync with itself: applyTheme() (src/main.cpp) reads it to actually
// switch themes, the Settings > Theme picker's preview swatches
// (drawSettingsControlPage(), ui_pages.cpp) read it to show what a theme
// looks like before it's selected, and tools/sim's render harness reads it
// for its --theme= flag. A `constexpr` array in a header gives every
// translation unit that includes this file its own copy, but they're all
// generated from this same literal, so there's no risk of the copies
// disagreeing the way three hand-maintained ones could.
struct ThemeColors {
  uint16_t background, card, primary, text, muted, warning, danger, cyan, secondary;
};
// muted (5th field) was lightened for every fixed theme from its original
// ship value -- the originals were never actually checked against WCAG
// contrast math and landed at 3.1-4.4:1 on COLOR_CARD (below the 4.5:1
// floor for normal-size text) in every one of them, worst in Violet Link.
// Each new value is the *minimal* lerp toward that theme's own COLOR_TEXT
// (as little as 3%, at most Violet Link's 30%) that clears 4.5:1 against
// both COLOR_CARD and COLOR_BACKGROUND, so the hue/character each theme's
// muted tone had is preserved as closely as the contrast floor allows.
constexpr ThemeColors kThemes[] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0},  // AUTO -- see comment above.
    {0x0823, 0x18E8, 0x6718, 0xE73C, 0x8433, 0xFE48, 0xF2CB, 0x269F, 0xA81F},  // Cyber Mint
    {0x1000, 0x28C2, 0xFD20, 0xFF9C, 0xABCB, 0xFFE0, 0xF260, 0xFBA0, 0xB940},  // Amber Core
    {0x080F, 0x2019, 0xC35F, 0xF73F, 0xAD1A, 0xFD86, 0xF1CB, 0x6DFF, 0x91FF},  // Violet Link
    {0x0000, 0x18C3, 0xC618, 0xFFFF, 0x8410, 0xDEFB, 0xD69A, 0xBDF7, 0x8410},  // Mono Signal
};
constexpr int kThemeCount = sizeof(kThemes) / sizeof(kThemes[0]);

enum SettingsView : uint8_t {
  SETTINGS_HOME,
  SETTINGS_BRIGHTNESS,
  SETTINGS_IDLE,
  SETTINGS_VOLUME,
  // Retired along with the Wake Mode tile/picker -- never assigned to
  // settingsView anymore. Kept as a placeholder in the enum so
  // changeSetting()'s tile-index-to-view arithmetic (main.cpp) doesn't
  // need to renumber SETTINGS_THEME/SETTINGS_BOOT.
  SETTINGS_WAKE,
  SETTINGS_THEME,
  SETTINGS_BOOT,
};
extern SettingsView settingsView;

extern bool hasCopiedGenome;
extern bool newEggConfirmation;
extern uint32_t newEggConfirmationUntil;
extern uint8_t pendingHatchMode;

// --- Color/easing helpers ------------------------------------------------------

uint16_t scaleRgb565(uint16_t color, uint8_t percent);
uint16_t lerpRgb565(uint16_t from, uint16_t to, float t);
// This app's own wrapper around color_utils.h's pickReadableColor(), fixed
// to this app's light/dark pair. See docs/style-guide.md's "Text and icons
// on a colored fill" rule: any label/icon drawn on a fill color that isn't
// COLOR_CARD/COLOR_BACKGROUND itself (a button, a selected-state highlight,
// a badge) must get its color from this, never a hardcoded COLOR_TEXT --
// COLOR_TEXT is tuned to read on this app's dark page chrome, and goes
// close to unreadable against several themes' brighter accent fills.
uint16_t readableTextColor(uint16_t fillColor);
// Smoothstep, remapped from an arbitrary [from, to] input range instead of a
// fixed [0, 1] one -- shared by the boot sequence, the toast slide, and the
// evolution animation, all in main.cpp, plus this header's own extracted
// callers, hence living here rather than next to any one of them.
float bootSmoothstep(float from, float to, float value);

// Same reasoning as bootSmoothstep: the boot sequence's on-screen "core"
// position, reused by playEvolutionAnimation() (main.cpp) for the evolved
// creature's portrait.
constexpr int16_t BOOT_CX = UI_WIDTH / 2;
constexpr int16_t BOOT_TOP_Y = 131;
constexpr int16_t BOOT_BOTTOM_Y = 339;
constexpr int16_t BOOT_CORE_Y = (BOOT_TOP_Y + BOOT_BOTTOM_Y) / 2;
constexpr uint8_t BOOT_NODE_COUNT = 16;

// --- Generic drawing helpers ---------------------------------------------------

void drawCentered(const char *text, int16_t y, uint8_t size, uint16_t color);
void drawCenteredInRect(const char *text, int16_t x, int16_t y, int16_t width,
                        int16_t height, uint8_t size, uint16_t color);
uint16_t backdropColorAt(int16_t y);
void paintPageBackdrop();
void drawPanelGlow(int16_t x, int16_t y, int16_t w, int16_t h, int16_t radius,
                   uint16_t color);
void drawGlyph16(const uint16_t *rows, int16_t x, int16_t y, uint16_t color);
void drawStatIcon(StatIcon icon, int16_t x, int16_t y, uint16_t color);
void drawStatRow(StatIcon icon, uint8_t value, int16_t y);
void drawButton(const char *label, int16_t x);
// The Status page's action grid tile, same chrome as drawSettingsTile()
// (card + glow + accent outline, alternating COLOR_CYAN/COLOR_PURPLE) but
// its own icon set: 0-2 reuse drawStatIcon's FOOD/JOY/ENERGY glyphs (FEED/
// PLAY/TRAIN restore exactly those stats), 3 is a new radar glyph for
// RECON LOG, 4 reuses the Settings "theme palette" icon for ITEMS, drawn
// inline rather than added to STAT_ICONS since none of these are pet
// stats. `locked` (page 2's 3 reserved-for-later tiles) dims the whole
// tile the same way Genome Lab's own unavailable CLONE/BLEND tiles do.
void drawActionTile(uint8_t action, int16_t x, int16_t y, const char *label,
                    const char *value, bool locked = false);
void drawPageDots(Page active);

// --- Companion creature rendering -----------------------------------------------

void drawEgg(bool frame, uint16_t bg);
int geneScale(uint8_t gene, int minimum, int maximum);
void drawElementAura(const PetPalette &palette, int cx, int cy, int radius,
                     uint8_t phase);
void drawGenomeMarkings(const PetPalette &palette, int cx, int cy, int width,
                        int height, uint8_t stage, uint8_t markingGene);
void drawGenomeFace(const PetPalette &palette, int cx, int cy, int headWidth,
                    bool blink, uint8_t stage, uint8_t faceGene);
void drawCreaturePortrait(const PetGenome &genome, uint8_t stage, int cx, int cy,
                          int headWidth, uint16_t ringColor);
void drawProceduralCreature(bool asleep);
void drawCreature(bool frame, bool asleep);

// --- Battle --------------------------------------------------------------------
// drawBattlingLayout/drawBattleResultsPage/drawOpponentRow/drawBattleButton
// take every value as a parameter rather than reading the live
// FamiliarBattleService `battle` object directly (see each one's own
// comment in ui_pages.cpp) -- the same design that already let main.cpp's
// DUMPBATTLE/DUMPSCAN debug serial commands preview them with synthetic
// data, since a live BLE match needs two physical devices to test at all.
// That means they need no live-battle stand-in to render here either.
// drawBattlePage() itself, which dispatches on battle.state(), stays in
// main.cpp for exactly that reason.

constexpr uint8_t kBattleResultsPerPage = 3;

enum BattleMoveIcon : uint8_t { ICON_ATTACK, ICON_DEFEND, ICON_SPECIAL, ICON_FLEE };

void drawBattleMoveIcon(BattleMoveIcon icon, int16_t x, int16_t y, uint16_t color);
void drawOpponentRow(const FamiliarBattleOpponent &opponent, int16_t y);
void drawBattleResultsPage(const std::vector<FamiliarBattleOpponent> &results,
                           uint8_t page);
void drawBattleButton(int16_t x, int16_t y, int16_t w, int16_t h, int16_t radius,
                      uint16_t color, BattleMoveIcon icon, const char *label);
void drawBattlingLayout(uint16_t myHp, uint16_t myMaxHp, uint16_t opponentHp,
                        uint16_t opponentMaxHp, uint8_t opponentLevel,
                        bool enhancedLink, const char *logLine1, const char *logLine2,
                        bool isResult, bool moveSubmitted, bool fleeArmed,
                        bool opponentGenomeAvailable, bool genomeCopied,
                        const PetGenome *opponentGenome);

// The Battle Idle state's two sub-views -- see battleShowingActions' own
// comment above for the split. Read pet/battleStats/friendsList directly
// (all already extern above) rather than taking them as parameters, same
// as drawRivalsPage()/drawFriendsPage() do -- unlike drawBattlingLayout()
// these don't need any live FamiliarBattleService state, only what's
// already reachable this way, so there's nothing to parameterize.
void drawBattleStatsView();
void drawRivalsIcon(int16_t cx, int16_t cy, uint16_t color);
void drawFriendsIcon(int16_t cx, int16_t cy, uint16_t color);
enum BattleGridTile : uint8_t { GRID_HOST, GRID_FIND, GRID_RIVALS, GRID_FRIENDS };
void drawBattleActionTile(BattleGridTile tile, int16_t x, int16_t y, const char *label,
                          const char *value);
void drawBattleActionsView();

// A full-screen overlay (tapped open from the Battle Idle action grid's
// RIVALS tile, dismissed by tapping anywhere -- see main.cpp), not one of
// the 5 swipeable pages, same as the Player ID/OTA update/evolution debug
// overlays already are.
void drawRivalsPage();

// Also a full-screen overlay, opened from the Status page's action grid's
// RECON LOG tile (see include/ui_pages.h's own note on statusShowingActions
// and main.cpp's performFeedScan()). Reads reconLog directly, same
// relationship drawRivalsPage() has with battleStats.
void drawReconLogPage();

// Also a full-screen overlay, opened from a tappable line on the Battle
// Idle screen (see main.cpp). Reads friendsList directly, same
// relationship drawRivalsPage() has with battleStats -- but unlike Rivals/
// Recon Log, this one isn't purely a passive list: it also draws its own
// HOST/FIND buttons (main.cpp wires their taps to
// FamiliarBattleMode::FriendExchange) so starting an exchange doesn't
// require leaving the page you're looking at the list from.
void drawFriendsPage();
void drawFriendsDirectoryPage();
void drawFriendDiscoverablePage();
void drawFriendsNearbyPage();

// Also a full-screen overlay, opened from the Status page's action grid's
// (page 2) ITEMS tile. Reads inventory directly; tapping an item tile with
// count > 0 consumes it (main.cpp's useItem()) and re-draws in place --
// unlike Rivals/Recon Log/Friends this overlay never closes itself on a
// generic tap, only via the same explicit BACK affordance drawSettingsBack()
// already established, since accidentally dismissing it mid-Type-reroll-
// confirmation would be a worse mistake to make than any of those other
// overlays' contents.
void drawItemIcon(uint8_t item, int16_t cx, int16_t cy, uint16_t color);
void drawItemsPage();

// --- Settings ------------------------------------------------------------------

void drawSettingsIcon(uint8_t item, int16_t cx, int16_t cy, uint16_t color);
void drawSettingsTile(uint8_t item, int16_t x, int16_t y, const char *label,
                      const char *value, bool locked = false);
void drawSettingsBack();
void drawChoiceRow(const char *label, int16_t y, bool selected);
void drawThemeSwatch(int16_t cx, int16_t cy, uint8_t themeIndex);
void drawSettingsControlPage();

// --- Pages --------------------------------------------------------------------

void drawCompanionPage();
void drawStatusPage();
void drawSettingsPage();
void drawGenomeLabPage();

// A full-screen overlay (tapped open from the Companion page's creature
// portrait, dismissed by swiping right -- see main.cpp), not one of the 5
// swipeable pages, same as the Player ID/OTA update/evolution debug
// overlays already are. Dispatches between its own two sub-views -- see
// genomeProfileShowingData's own comment above.
void drawGenomeProfilePage();
