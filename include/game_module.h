#pragma once

#include <cstdint>

#include "ui_pages.h"

// The console/game seam. Exactly one game_*.cpp implements every function
// declared below -- which one links into a given firmware build is a
// compile-time choice (which .cpp files that build's PlatformIO env pulls
// out of src/, the same build_src_filter mechanism [env:native] already
// uses to cherry-pick pet_genome.cpp/familiar_battle_rules.cpp/
// color_utils.cpp), not a runtime one. There's exactly one game module
// linked per physical device, ever, so this is free functions rather than
// an abstract-base-class/vtable interface -- see the "Stage 1" plan this
// was extracted under for the reasoning.
//
// Right now game_vpet.cpp is the only implementation, and (as of this
// header's introduction) still just a placeholder: the console shell in
// main.cpp hasn't been wired to call any of these yet -- that happens as
// the actual vpet mutation/touch-handling logic moves out of main.cpp and
// into game_vpet.cpp, and main.cpp's dispatch code is cut over to call
// through this header instead of the vpet-specific functions directly.

// Called once from setup(), after preferences.begin() -- loads whatever
// persisted game state exists (or seeds fresh defaults) the same way
// main.cpp's loadPet()/loadBattleStats()/etc. do today.
void gameInit();

// Called once per loop() iteration, after the console's own per-tick work
// (clock redraw, sleep-timeout check, ...). Owns whatever the game needs to
// do on a timer -- vpet's agePet()'s once-a-minute decay tick, its idle
// creature animation, etc.
void gameTick(uint32_t nowMs);

// Replaces drawActivePage()'s per-page switch for every non-PAGE_SETTINGS
// page -- the console still owns Settings (the one genuinely generic page)
// and calls this for everything else.
void gameDrawPage(Page page);

// dx/dy/touchDuration are the same press-to-release deltas/duration
// loop()'s touch-release handler already computes -- several per-page tap
// branches need them to distinguish a genuine short tap from a long-press
// or a small drag (e.g. the Companion page's tap-the-creature-for-Genome-
// Profile gesture), so they're passed through rather than re-derived.
void gameHandleTap(Page page, int16_t x, int16_t y, int16_t dx, int16_t dy,
                   uint32_t touchDuration);
void gameHandleSwipe(Page page, int16_t dx, int16_t dy);

// True whenever a game-owned full-screen overlay (Rivals/Recon Log/
// Friends/Items/Genome Profile/evolution debug, for vpet) should intercept
// touch ahead of normal per-page dispatch. main.cpp's touch-release handler
// checks its own two console-owned overlays (Player ID, OTA update) first,
// then this, before falling through to gameHandleTap()/gameHandleSwipe().
bool gameOverlayActive();
void gameOverlayTouch(int16_t x, int16_t y, int16_t dx, int16_t dy, bool released);

// Replaces applyTheme()'s direct paletteForGenome(pet.genome) call for the
// AUTO theme -- console code no longer references any game-specific type.
void gameApplyAutoTheme(ThemeColors &out);

// Replaces the boot sequence's direct pet.genome/paletteForGenome() reads.
// The boot sequence is console-owned and runs before any game state is
// meaningfully "loaded," but still wants a palette to paint with and a
// deterministic-but-varied seed for particle placement -- both come from
// the game module instead of console code reaching into game-specific
// globals directly. PetPalette itself (pet_genome.h) stays a shared type
// here rather than being flattened to raw colors: the boot/evolution
// animation drawing primitives (paintBootBackdrop/drawBootParticles/
// drawBootCore) already take one as a parameter, and sharing that struct
// shape is a much lighter coupling than reading live game state would be.
PetPalette gameBootAnimationPalette();
void gameBootAnimationSeed(uint32_t seed[4]);

// Hook for loop()'s battle-state-edge-detection block: called whenever
// FamiliarBattleService's state has just changed while the Battle page is
// active, so the game module can react (vpet: recordBattleOutcome() on the
// edge into Result, addFriend() on the edge into Exchanged) without the
// console needing to know either of those exist.
void gameOnBattleStateChanged();
