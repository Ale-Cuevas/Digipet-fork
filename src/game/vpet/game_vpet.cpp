#include "game_module.h"

// vpet's implementation of the console/game seam (include/game_module.h).
//
// Stage 1 step 4: this file exists purely to prove the seam's shape
// compiles and links against real types (Page, ThemeColors) before the
// bigger cut-and-paste move -- main.cpp isn't wired to call any of these
// yet, and every body below is a placeholder. The real vpet mutation/
// touch-handling logic (agePet, checkEvolution, performAction,
// handleBattleTap, ...) still lives in main.cpp at this point; it moves
// here function-by-function in the steps that follow, each one replacing
// the matching placeholder body and, once every caller has been cut over,
// main.cpp stops calling the vpet-specific functions directly and goes
// through this header instead.

void gameInit() {
  // TODO(stage1): call vpet's loadPet/loadCopiedGenome/loadBattleStats/
  // loadReconLog/loadFriendsList/loadInventory once they've moved here.
}

void gameTick(uint32_t /*nowMs*/) {
  // TODO(stage1): agePet()'s once-a-minute tick, updateCreatureAnimation().
}

void gameDrawPage(Page /*page*/) {
  // TODO(stage1): the vpet branches currently in main.cpp's
  // drawActivePage() switch.
}

void gameHandleTap(Page /*page*/, int16_t /*x*/, int16_t /*y*/) {
  // TODO(stage1): the vpet branches of loop()'s per-page tap dispatch.
}

void gameHandleSwipe(Page /*page*/, int16_t /*dx*/, int16_t /*dy*/) {
  // TODO(stage1): the vpet branches of loop()'s per-page swipe dispatch.
}

bool gameOverlayActive() {
  // TODO(stage1): showingEvolutionDebug/showingRivals/showingReconLog/
  // showingFriends/showingItems/showingGenomeProfile, once moved here.
  return false;
}

void gameOverlayTouch(int16_t /*x*/, int16_t /*y*/, int16_t /*dx*/, int16_t /*dy*/,
                      bool /*released*/) {
  // TODO(stage1): the moved overlay if-chain's touch handling.
}

void gameApplyAutoTheme(ThemeColors & /*out*/) {
  // TODO(stage1): out = colors derived from paletteForGenome(pet.genome).
}

uint32_t gameBootAnimationSeed() {
  // TODO(stage1): return pet.genome.seed[0] (or similar) once pet has
  // moved here. A fixed placeholder is fine meanwhile -- nothing calls
  // this yet.
  return 0;
}

void gameOnBattleStateChanged() {
  // TODO(stage1): recordBattleOutcome() on the edge into Result,
  // addFriend() on the edge into Exchanged.
}
