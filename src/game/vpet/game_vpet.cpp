#include "game_module.h"

#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <cstring>
#include <esp_system.h>

#include "familiar_battle_service.h"
#include "persist_blob.h"

// vpet's implementation of the console/game seam (include/game_module.h).
//
// Stage 1 step 5: the vpet-specific PetState mutation and touch-handling
// functions that used to live in main.cpp move here verbatim (same bodies,
// same comments) -- this step is a mechanical relocation, not a rewrite.
// main.cpp still calls every one of these functions directly by name (its
// forward declarations now point at definitions in this file instead of
// itself) rather than through game_module.h's gameXxx() hooks -- that
// dispatch cutover is deliberately a later step (see the "Stage 1" plan),
// so gameInit()/gameTick()/etc. below are still placeholders.
//
// Every extern below is a console-owned global or function this file needs
// cross-TU access to; main.cpp keeps owning the actual definitions (same
// "declare what a second translation unit needs as extern" pattern
// ui_pages.h already uses for pet/battleStats/etc.).

extern Preferences preferences;
extern FamiliarBattleService battle;

extern PetGenome copiedGenome;
extern bool battleGenomeCopied;
extern bool showingBattleResults;
extern uint8_t battleResultsPage;
extern bool showingRivals;
extern bool showingFriends;
extern bool showingItems;
extern bool fleeArmed;
extern uint32_t fleeArmedUntil;

extern bool sleeping;
extern bool screenOff;
extern Page currentPage;
extern uint16_t creatureFrameInterval;
extern bool transitionsReady;
extern Arduino_Canvas pageCanvasA;
extern Arduino_Canvas pageCanvasB;
extern Arduino_CO5300 *panel;

extern uint8_t addClamped(uint8_t value, uint8_t amount);
extern uint8_t subtractClamped(uint8_t value, uint8_t amount);
extern void showToast(const char *text);
extern void showToastf(const char *format, ...);
extern void playTone(uint16_t frequency, uint16_t durationMs, uint8_t level = 55);
extern void drawHome();
extern void drawSleep();
extern void presentBattlePage();
extern void presentOverlayEntrance(Page fromPage, void (*drawOverlay)());
extern void presentOverlayExit(void (*drawOverlay)(), Page toPage);
extern void applyTheme();
extern void presentCoherentPageFrame(Page page);
extern bool mountGenomeCard();
extern void closeGenomeCard();
extern void paintBootBackdrop(const PetPalette &palette);
extern void drawBootParticles(const PetPalette &palette, uint32_t frame,
                              const uint32_t seed[4]);
extern void drawBootCore(const PetPalette &palette, uint32_t frame, float progress);

// --- Save schema versioning -----------------------------------------------

constexpr uint32_t PET_MAGIC_V1 = 0x44504731;
constexpr uint32_t PET_MAGIC_V2 = 0x44504732;
constexpr uint32_t PET_MAGIC_V3 = 0x44504733;  // no bonusHp/Attack/Defense/Special fields yet
// Not part of the normal V1..V3 lineage: the pocket-pedometer feature added
// a stepsTotal field under this magic and shipped to the one device this
// firmware actually runs on, then got pulled back out the same day. Kept
// here solely so loadPet() can downgrade that one device's save gracefully
// instead of failing to recognize it and silently starting a new egg.
constexpr uint32_t PET_MAGIC_PEDOMETER = 0x44504735;
constexpr uint32_t PET_MAGIC = 0x44504734;

// A per-device record, not a per-pet one -- like playerId itself (derived
// from ESP.getEfuseMac(), not saved in `pet`), it persists across egg
// hatches/blends rather than resetting with the current companion.
constexpr uint32_t BATTLE_STATS_MAGIC = 0x42535401;  // "BST" + schema 1
// Same reasoning as BATTLE_STATS_MAGIC just above.
constexpr uint32_t RECON_LOG_MAGIC = 0x52434C31;  // "RCL" + schema 1
// Same reasoning as BATTLE_STATS_MAGIC above.
constexpr uint32_t FRIENDS_LIST_MAGIC = 0x46524E31;  // "FRN" + schema 1
// Not a per-device record like the ones above -- items are earned by and
// belong to the current companion the same way food/joy do, so this reads
// more like DeviceSettings' shape (single magic-guarded blob, no capped
// array of entries) than BattleStats'.
constexpr uint32_t INVENTORY_MAGIC = 0x49544D31;  // "ITM" + schema 1

uint32_t battlePlayerId() {
  const uint64_t mac = ESP.getEfuseMac();
  return static_cast<uint32_t>(mac ^ (mac >> 32));
}

// FNV-1a, used only for hashing a WiFi BSSID into a ReconEntry idHash --
// FamiliarBattleService::scanNearbyBle() needs the exact same constants for
// its own BLE-address hashing but isn't shared code with this: keeping BLE
// fully encapsulated inside that class (see its own comment) is worth a
// second four-line copy of a hash loop this small.
uint32_t fnv1aHash(const uint8_t *data, size_t length) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < length; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

uint8_t calculateStage() {
  if (pet.ageMinutes < 2 || pet.actions < 2) return 0;
  if (pet.ageMinutes < 30 || pet.actions < 8) return 1;
  if (pet.ageMinutes < 360 || pet.actions < 40) return 2;
  if (pet.ageMinutes < 1440 || pet.actions < 120 || pet.training < 25) return 3;
  return 4;
}

void playActionSound(uint8_t action) {
  if (action == 0) {
    playTone(440, 55); playTone(523, 80);
  } else if (action == 1) {
    playTone(659, 55); playTone(880, 55); playTone(1047, 90);
  } else {
    playTone(330, 55); playTone(440, 55); playTone(659, 110);
  }
}

// Finds `playerId`'s rival slot, adding a new one if this is a first-time
// opponent. Once all kMaxBattleRivals slots are full, reuses whichever
// rival has the fewest recorded battles -- a simple "keep the rivalries
// you've actually built up" eviction rule rather than tracking timestamps.
BattleRival &findOrAddRival(uint32_t playerId) {
  for (uint8_t i = 0; i < battleStats.rivalCount; ++i) {
    if (battleStats.rivals[i].playerId == playerId) return battleStats.rivals[i];
  }
  if (battleStats.rivalCount < kMaxBattleRivals) {
    BattleRival &rival = battleStats.rivals[battleStats.rivalCount++];
    rival = BattleRival{};
    rival.playerId = playerId;
    return rival;
  }
  uint8_t leastIndex = 0;
  uint32_t leastTotal = 0xFFFFFFFF;
  for (uint8_t i = 0; i < kMaxBattleRivals; ++i) {
    const uint32_t total = static_cast<uint32_t>(battleStats.rivals[i].wins) +
                           battleStats.rivals[i].losses;
    if (total < leastTotal) {
      leastTotal = total;
      leastIndex = i;
    }
  }
  battleStats.rivals[leastIndex] = BattleRival{};
  battleStats.rivals[leastIndex].playerId = playerId;
  return battleStats.rivals[leastIndex];
}

// --- Persistence -----------------------------------------------------------

void savePet() {
  preferences.putBytes("state", &pet, sizeof(pet));
}

void saveBattleStats() {
  saveBlob(preferences, "battleStats", battleStats);
}

void loadBattleStats() {
  if (!loadBlob(preferences, "battleStats", battleStats, BATTLE_STATS_MAGIC)) {
    // First boot, or a stored blob from a different (and thus incompatible)
    // schema -- start clean rather than risk misreading old bytes as a
    // BattleStats of the current shape.
    battleStats = BattleStats{};
    battleStats.magic = BATTLE_STATS_MAGIC;
  }
}

void saveReconLog() {
  saveBlob(preferences, "reconLog", reconLog);
}

void loadReconLog() {
  if (!loadBlob(preferences, "reconLog", reconLog, RECON_LOG_MAGIC)) {
    reconLog = ReconLog{};
    reconLog.magic = RECON_LOG_MAGIC;
  }
}

void saveFriendsList() {
  saveBlob(preferences, "friends", friendsList);
}

void loadFriendsList() {
  if (!loadBlob(preferences, "friends", friendsList, FRIENDS_LIST_MAGIC)) {
    friendsList = FriendsList{};
    friendsList.magic = FRIENDS_LIST_MAGIC;
  }
}

void saveInventory() {
  saveBlob(preferences, "inventory", inventory);
}

void loadInventory() {
  if (!loadBlob(preferences, "inventory", inventory, INVENTORY_MAGIC)) {
    inventory = Inventory{};
    inventory.magic = INVENTORY_MAGIC;
  }
}

// copiedGenome/hasCopiedGenome don't fit loadBlob/saveBlob's shape --
// PetGenome has no magic field of its own (it's also the wire format
// streamed over BLE and written to SD, see pet_genome.h, so adding one
// would be a format change with a much bigger blast radius than this
// helper is worth), and validity here is "did the read/write come back the
// exact right size," not a magic check, plus the genomeTransferStatus side
// effect neither generic helper has a hook for. Stays hand-rolled, same
// reasoning as savePet()/loadPet() below.
void saveCopiedGenome(const PetGenome &genome) {
  copiedGenome = genome;
  hasCopiedGenome = preferences.putBytes("copiedGenome", &copiedGenome,
                                         sizeof(copiedGenome)) == sizeof(copiedGenome);
  strcpy(genomeTransferStatus, hasCopiedGenome ? "COPIED GENOME READY" :
                                                "COPY SAVE FAILED");
}

void loadCopiedGenome() {
  hasCopiedGenome = preferences.getBytesLength("copiedGenome") == sizeof(copiedGenome);
  if (hasCopiedGenome) {
    preferences.getBytes("copiedGenome", &copiedGenome, sizeof(copiedGenome));
    strcpy(genomeTransferStatus, "COPIED GENOME READY");
  }
}

// --- Recon / Friends / Items ------------------------------------------------

// Records one scan hit, evicting the oldest entry (by firstSeenAge) once
// kMaxReconEntries is full -- the same "keep what's still relevant" shape
// findOrAddRival() uses above, just keyed by recency instead of battle
// count since a signal has no equivalent tally. Returns true the first
// time `idHash` is ever seen (a "new device" bonus for performFeedScan()),
// false on every scan after that.
bool reconLogRecord(uint32_t idHash, uint8_t kind, const char *label) {
  for (uint8_t i = 0; i < reconLog.entryCount; ++i) {
    if (reconLog.entries[i].idHash == idHash) return false;
  }
  ReconEntry entry{};
  entry.idHash = idHash;
  entry.kind = kind;
  entry.firstSeenAge = pet.ageMinutes;
  if (label) {
    strncpy(entry.label, label, sizeof(entry.label) - 1);
  }
  if (reconLog.entryCount < kMaxReconEntries) {
    reconLog.entries[reconLog.entryCount++] = entry;
  } else {
    uint8_t oldestIndex = 0;
    uint32_t oldestAge = 0xFFFFFFFF;
    for (uint8_t i = 0; i < kMaxReconEntries; ++i) {
      if (reconLog.entries[i].firstSeenAge < oldestAge) {
        oldestAge = reconLog.entries[i].firstSeenAge;
        oldestIndex = i;
      }
    }
    reconLog.entries[oldestIndex] = entry;
  }
  ++reconLog.totalUniqueSeen;
  return true;
}

// Called on the Idle-state edge into FamiliarBattleState::Exchanged (see
// loop()'s battle-state-change handling, mirroring recordBattleOutcome()'s
// own edge-into-Result call). No-op if already a friend or the list is
// full -- unlike findOrAddRival(), a full list just stops accepting new
// friends rather than evicting an old one: a rival's slot can meaningfully
// go stale (you stopped battling them), but there's no equivalent
// staleness signal for a friend, so evicting one silently on your behalf
// would be a surprising thing for the app to decide alone.
void addFriend(uint32_t playerId) {
  for (uint8_t i = 0; i < friendsList.count; ++i) {
    if (friendsList.friends[i].playerId == playerId) return;
  }
  if (friendsList.count >= kMaxFriends) return;
  FriendEntry &entry = friendsList.friends[friendsList.count++];
  entry.playerId = playerId;
  entry.addedAge = pet.ageMinutes;
  saveFriendsList();
}

// Called once per battle, on the edge into FamiliarBattleState::Result --
// see loop()'s battle-state-change handling. Only Victory/Defeat update a
// specific rival's record: fleeing or disconnecting isn't a decisive result
// against that opponent, so those only move the aggregate counters.
void recordBattleOutcome(FamiliarBattleOutcome outcome, uint32_t opponentPlayerId) {
  switch (outcome) {
    case FamiliarBattleOutcome::Victory:
      battleStats.wins++;
      if (opponentPlayerId) findOrAddRival(opponentPlayerId).wins++;
      break;
    case FamiliarBattleOutcome::Defeat:
      battleStats.losses++;
      if (opponentPlayerId) findOrAddRival(opponentPlayerId).losses++;
      break;
    case FamiliarBattleOutcome::Fled:
      battleStats.fled++;
      break;
    case FamiliarBattleOutcome::OpponentFled:
      battleStats.opponentFled++;
      break;
    case FamiliarBattleOutcome::Disconnected:
      battleStats.disconnected++;
      break;
    default:
      return;  // None -- shouldn't reach here, but don't spend a flash write on it.
  }
  saveBattleStats();
}

// Each point of boost this app hands out this way is deliberately small
// (matching addClamped()'s own care-stat scale being 0-100, not this) --
// the cap keeps FEED-farmed items from turning battles into a pure
// time-investment contest.
constexpr uint8_t kItemBonusStep = 2;
constexpr uint8_t kItemBonusCap = 30;

// Consumes one of `item`, applying its effect immediately -- items are used
// the instant they're tapped in drawItemsPage(), not equipped/held for
// later. No-op if none are held. See ItemType's own comment in ui_pages.h
// for why HP/ATK/DEF/SPECIAL are a shared numeric-boost shape and
// TYPE_SHIFT (a genome.element reroll) isn't.
//
// pet.bonusHp/Attack/Defense/Special only ever affect this device's own
// displays and locally-initiated battles (drawBattlePage()'s Idle-state
// stat line, battleCapabilities() for Special) -- a deliberate scope
// decision, not a limitation of how the values could flow: capabilities.
// special is transmitted directly to an opponent already (sendCapabilities())
// so a Special boost would in fact reach a live PvP battle correctly with
// no protocol change, but HP/ATK/DEF are independently *recomputed* by
// each side from transmitted level/stage (deriveMaxHp/deriveAttack/
// deriveDefense), not transmitted as raw numbers -- boosting only this
// device's own myMaxHp_/myAttack_/myDefense_ without also telling the peer
// would desync what each side thinks this pet's stats are. Keeping all
// four consistent (none of them affect a live battle yet) avoids the
// confusing "why did my Special item work in a real battle but not my ATK
// one" asymmetry that treating them differently would create.
void useItem(uint8_t item) {
  if (item >= kItemTypeCount || inventory.counts[item] == 0) return;
  --inventory.counts[item];
  saveInventory();

  if (item == ITEM_TYPE_SHIFT) {
    const uint8_t previous = pet.genome.element;
    uint8_t next = previous;
    while (next == previous) next = esp_random() % 6;
    pet.genome.element = next;
    showToastf("Element shifted to %s", elementName(pet.genome.element));
  } else {
    uint8_t *bonus = item == ITEM_HP_BOOST ? &pet.bonusHp :
                     item == ITEM_ATK_BOOST ? &pet.bonusAttack :
                     item == ITEM_DEF_BOOST ? &pet.bonusDefense : &pet.bonusSpecial;
    *bonus = min<uint8_t>(kItemBonusCap, *bonus + kItemBonusStep);
    const char *labels[] = {"HP", "ATK", "DEF", "SPECIAL"};
    showToastf("%s boosted to +%u", labels[item], *bonus);
  }
  playTone(659, 60, 40);
  playTone(880, 90, 45);
  savePet();
}

// Rolled once per FEED scan that turns up at least one never-seen-before
// device (performFeedScan(), see its own comment) -- a flat chance, not
// scaled by how many new devices were found, so spamming scans in a
// device-dense area isn't strictly better than one scan somewhere new.
constexpr uint8_t kItemDropPercent = 20;

void maybeAwardItem() {
  if (esp_random() % 100 >= kItemDropPercent) return;
  const uint8_t item = esp_random() % kItemTypeCount;
  ++inventory.counts[item];
  saveInventory();
  static const char *names[] = {"HP BOOST", "ATK BOOST", "DEF BOOST", "SPECIAL BOOST",
                                "TYPE SHIFT"};
  showToastf("Mutation trigger found: %s", names[item]);
  playTone(988, 50, 40);
  playTone(1319, 90, 50);
}

// --- Battle ------------------------------------------------------------------

FamiliarBattleCapabilities battleCapabilities() {
  FamiliarBattleCapabilities capabilities;
  capabilities.flags = FamiliarCapBodyType | FamiliarCapElement |
      FamiliarCapSpeed | FamiliarCapSpecial | FamiliarCapMoveMatchups;
  capabilities.bodyType = pet.genome.bodyType % 5;
  capabilities.element = pet.genome.element % 6;
  const int bodySpeedBonus = capabilities.bodyType == 2 ? 12 :
                             (capabilities.bodyType == 3 ? -8 : 0);
  capabilities.speed = constrain(20 + pet.genome.limbGene * 55 / 255 +
                                 pet.stage * 6 + bodySpeedBonus, 1, 100);
  // pet.bonusSpecial folds in here -- unlike ATK/DEF/HP (recomputed
  // independently by each side from transmitted level/stage, see
  // useItem()'s own comment), capabilities.special is transmitted directly
  // to a peer as one of these raw fields (sendCapabilities()), so an item
  // boost genuinely does reach a real opponent correctly through this,
  // no protocol change needed.
  capabilities.special = constrain(25 + pet.genome.faceGene + pet.stage * 9 +
                                   pet.training / 4 + pet.bonusSpecial, 1, 100);
  return capabilities;
}

// --- Genome export/import (SD) ----------------------------------------------

bool exportActiveGenome() {
  if (!mountGenomeCard()) {
    strcpy(genomeTransferStatus, "SD CARD NOT READY");
    return false;
  }
  SD_MMC.mkdir("/digipet");
  File file = SD_MMC.open("/digipet/genome.txt", FILE_WRITE);
  char code[PET_GENOME_CODE_LENGTH + 1]{};
  const bool encoded = encodePetGenome(pet.genome, code, sizeof(code));
  const bool written = file && encoded && file.println(code) > 0;
  if (file) file.close();
  closeGenomeCard();
  strcpy(genomeTransferStatus, written ? "EXPORTED TO SD" : "EXPORT FAILED");
  return written;
}

bool importCopiedGenome() {
  if (!mountGenomeCard()) {
    strcpy(genomeTransferStatus, "SD CARD NOT READY");
    return false;
  }
  File file = SD_MMC.open("/digipet/genome.txt", FILE_READ);
  String code;
  while (file && file.available() && !code.length()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() && !line.startsWith("#") && !line.startsWith(";")) code = line;
  }
  if (file) file.close();
  closeGenomeCard();
  code.trim();
  PetGenome imported{};
  if (!decodePetGenome(code.c_str(), imported)) {
    strcpy(genomeTransferStatus, "INVALID GENOME CODE");
    return false;
  }
  saveCopiedGenome(imported);
  return hasCopiedGenome;
}

// --- FEED's passive signal scan --------------------------------------------

void drawScanningPage(uint8_t phase) {
  paintPageBackdrop();
  drawCentered("SIGNAL SCAN", 40, 3, COLOR_MINT);
  drawCentered("PASSIVE WIFI + BLE SWEEP", 76, 1, COLOR_CYAN);
  drawPanelGlow(24, 120, 320, 190, 26, COLOR_PURPLE);
  display->fillRoundRect(24, 120, 320, 190, 26, COLOR_CARD);
  display->drawRoundRect(32, 128, 304, 174, 20, COLOR_PURPLE);
  drawCentered(phase == 0 ? "WIFI SWEEP" : "BLE SWEEP", 175, 2, COLOR_TEXT);
  drawCentered(phase == 0 ? "READING BEACON FRAMES" : "READING ADVERTISEMENTS",
               208, 1, COLOR_MUTED);
  for (uint8_t i = 0; i < 2; ++i) {
    display->fillCircle(160 + i * 48, 260, 10, i <= phase ? COLOR_MINT : COLOR_MUTED);
  }
  drawCentered("HOLD STILL...", 340, 1, COLOR_WARNING);
}

// Blits drawScanningPage() straight to the panel (through the transition
// canvas pipeline where available, same as presentUpdatePage()) -- called
// once per phase, not on a redraw cadence, since both scans below are
// blocking calls with nothing else running meanwhile to animate against.
void presentScanningPage(uint8_t phase) {
  if (transitionsReady) {
    Arduino_GFX *previousDisplay = display;
    display = &pageCanvasA;
    drawScanningPage(phase);
    display = previousDisplay;
    panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);
  } else {
    drawScanningPage(phase);
  }
}

struct FeedScanResult {
  uint8_t wifiCount = 0;
  uint8_t bleCount = 0;
  uint8_t newCount = 0;
};

// FEED's mechanic: a passive ~4s WiFi beacon scan followed by a passive
// ~4s BLE advertisement scan -- sequential, never simultaneous, because the
// ESP32-S3 has exactly one 2.4GHz radio shared between Wi-Fi and BLE (see
// FamiliarBattleService::beginRadio()'s own comment, which this mirrors).
// Purely passive: WiFi.scanNetworks() only reads public beacon frames and
// scanNearbyBle() only reads public advertisements -- this never
// associates, pairs, connects, or transmits anything at any discovered
// device, the same "just listening" scan any phone already does to list
// nearby networks.
FeedScanResult performFeedScan() {
  FeedScanResult result;
  presentScanningPage(0);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  const int16_t found = WiFi.scanNetworks(false, false, false, 300);
  if (found > 0) {
    result.wifiCount = static_cast<uint8_t>(min<int16_t>(found, 255));
    for (int16_t i = 0; i < found; ++i) {
      const uint8_t *bssid = WiFi.BSSID(i);
      if (bssid != nullptr &&
          reconLogRecord(fnv1aHash(bssid, 6), 0, WiFi.SSID(i).c_str())) {
        ++result.newCount;
      }
    }
  }
  WiFi.scanDelete();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(150);

  presentScanningPage(1);
  const std::vector<uint32_t> bleHashes = battle.scanNearbyBle(4000);
  result.bleCount = static_cast<uint8_t>(min<size_t>(bleHashes.size(), 255));
  for (uint32_t hash : bleHashes) {
    if (reconLogRecord(hash, 1, nullptr)) ++result.newCount;
  }

  ++reconLog.totalScans;
  saveReconLog();
  return result;
}

// --- Evolution ---------------------------------------------------------------

// Reuses the boot sequence's own visual language (paintBootBackdrop/
// drawBootParticles/drawBootCore, same screen position) rather than
// inventing a second one, so evolving reads as the same kind of "genesis"
// moment hatching/booting already are. Composed entirely in pageCanvasA
// and blitted in one shot per frame -- same double-buffering the rest of
// the app uses -- instead of drawing straight to the live panel.
void playEvolutionAnimation() {
  const PetPalette palette = paletteForGenome(pet.genome);
  Arduino_GFX *previousDisplay = display;
  paintBootBackdrop(palette);
  const size_t frameBytes = static_cast<size_t>(LCD_WIDTH) * LCD_HEIGHT * sizeof(uint16_t);

  // Phase 1: a converging energy burst at the core, echoing the boot
  // sequence's helix-converge finale. Held past drawBootCore's own 0.55
  // convergence threshold throughout so the pulsing rings play the whole
  // phase instead of only kicking in near the end.
  constexpr uint32_t kBurstMs = 850;
  const uint32_t burstStarted = millis();
  for (uint32_t frame = 0; ; ++frame) {
    const uint32_t elapsed = millis() - burstStarted;
    const float progress = min(1.0f, elapsed / static_cast<float>(kBurstMs));
    display = &pageCanvasA;
    memcpy(pageCanvasA.getFramebuffer(), pageCanvasB.getFramebuffer(), frameBytes);
    drawBootParticles(palette, frame, pet.genome.seed);
    drawBootCore(palette, frame, 0.6f + progress * 0.4f);
    drawCentered("DATA EVOLUTION", 60, 3,
                lerpRgb565(COLOR_BACKGROUND, COLOR_TEXT, bootSmoothstep(0.0f, 0.3f, progress)));
    display = previousDisplay;
    panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);
    if (progress >= 1.0f) break;
    delay(18);
  }

  // Phase 2: reveal the evolved creature itself (not just its stage name)
  // growing and fading in at the same spot the core just pulsed at.
  constexpr uint32_t kRevealMs = 600;
  const uint32_t revealStarted = millis();
  for (uint32_t frame = 0; ; ++frame) {
    const uint32_t elapsed = millis() - revealStarted;
    const float progress = min(1.0f, elapsed / static_cast<float>(kRevealMs));
    const float eased = bootSmoothstep(0.0f, 1.0f, progress);
    display = &pageCanvasA;
    memcpy(pageCanvasA.getFramebuffer(), pageCanvasB.getFramebuffer(), frameBytes);
    drawBootParticles(palette, kBurstMs / 18 + frame, pet.genome.seed);
    drawCentered("DATA EVOLUTION", 60, 3, COLOR_TEXT);
    drawCreaturePortrait(pet.genome, pet.stage, BOOT_CX, BOOT_CORE_Y,
                        lroundf(40 + eased * 62),
                        lerpRgb565(COLOR_BACKGROUND, palette.glow, eased));
    drawCentered(STAGE_NAMES[pet.stage], 330, 3,
                lerpRgb565(COLOR_BACKGROUND, COLOR_MINT, eased));
    display = previousDisplay;
    panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);
    if (progress >= 1.0f) break;
    delay(18);
  }
  delay(750);  // Hold on the reveal before the caller returns to normal play.
}

void checkEvolution() {
  const uint8_t newStage = calculateStage();
  if (newStage > pet.stage) {
    pet.stage = newStage;
    savePet();
    playEvolutionAnimation();
    showToastf("Evolved into %s", STAGE_NAMES[pet.stage]);
  }
}

// --- FEED/PLAY/TRAIN and the once-a-minute care tick -------------------------

void performAction(int action) {
  if (action == 0) {
    const FeedScanResult scan = performFeedScan();
    const uint16_t totalSeen = static_cast<uint16_t>(scan.wifiCount) + scan.bleCount;
    const uint8_t deviceGain = static_cast<uint8_t>(min<uint16_t>(totalSeen, 20));
    const uint8_t foodGain = static_cast<uint8_t>(6 + deviceGain + scan.newCount * 3);
    pet.food = addClamped(pet.food, foodGain);
    pet.joy = addClamped(pet.joy, static_cast<uint8_t>(scan.newCount * 2));
    pet.energy = addClamped(pet.energy, 2);
    showToastf("SCAN: %u seen, %u new (+%u food)", totalSeen, scan.newCount, foodGain);
    // Overwrites the toast just set above with its own if it hits -- an
    // item find is the more exciting outcome, worth being the one shown.
    if (scan.newCount > 0) maybeAwardItem();
  } else if (action == 1) {
    if (pet.energy < 8) {
      showToast("Too tired to play");
      drawHome();
      return;
    }
    pet.joy = addClamped(pet.joy, 16);
    pet.energy = subtractClamped(pet.energy, 8);
    pet.food = subtractClamped(pet.food, 3);
    showToast("Connection strengthened");
  } else {
    if (pet.energy < 12) {
      showToast("Needs idle time first");
      drawHome();
      return;
    }
    pet.training = addClamped(pet.training, 4);
    pet.energy = subtractClamped(pet.energy, 12);
    pet.food = subtractClamped(pet.food, 4);
    pet.joy = addClamped(pet.joy, 3);
    showToast("Training complete!");
  }
  playActionSound(action);
  pet.actions++;
  checkEvolution();
  savePet();
  drawHome();
}

void agePet() {
  pet.ageMinutes++;
  if (pet.ageMinutes % 5 == 0) pet.food = subtractClamped(pet.food, 1);
  if (pet.ageMinutes % 8 == 0) pet.joy = subtractClamped(pet.joy, 1);
  if (sleeping) {
    pet.energy = addClamped(pet.energy, 5);
    if (pet.health < 100) pet.health = addClamped(pet.health, 1);
  } else if (pet.ageMinutes % 3 == 0) {
    pet.energy = subtractClamped(pet.energy, 1);
  }
  // Low stats lower general vitality; there is deliberately no illness state.
  if (pet.food < 15 || pet.joy < 15 || pet.energy < 5) {
    pet.health = subtractClamped(pet.health, 1);
  } else if (pet.health < 100 && pet.ageMinutes % 10 == 0) {
    pet.health = addClamped(pet.health, 1);
  }
  checkEvolution();
  savePet();
  if (!sleeping) drawHome();
  else if (!screenOff) drawSleep();
}

void updateCreatureAnimation() {
  if (sleeping) {
    // Keep idle visually quiet: only the two dream marks move.
    display->fillRect(158, 184, 72, 72, RGB565_BLACK);
    display->setTextColor(COLOR_CYAN);
    display->setTextSize(2);
    display->setCursor(animationFrame ? 170 : 174, 226);
    display->print("z");
    display->setTextSize(3);
    display->setCursor(animationFrame ? 193 : 197, 196);
    display->print("Z");
  } else if (currentPage == PAGE_COMPANION) {
    // Compose the entire frame in PSRAM. Directly streaming overlapping
    // primitives to the CO5300 causes visible scanline tearing.
    const uint32_t started = millis();
    presentCoherentPageFrame(PAGE_COMPANION);
    const uint16_t renderTime = millis() - started;
    const uint16_t nextInterval = constrain(renderTime + 6, 40, 90);
    creatureFrameInterval = (creatureFrameInterval * 3 + nextInterval) / 4;
  }
}

// --- Egg hatching --------------------------------------------------------

void beginNewEgg(uint8_t mode) {
  PetGenome nextGenome = generatePetGenome();
  if (mode == 2 && hasCopiedGenome) nextGenome = copiedGenome;
  if (mode == 3 && hasCopiedGenome)
    nextGenome = blendPetGenomes(pet.genome, copiedGenome);

  // A companion that reached the final stage has a 50% chance to pass half
  // its item-earned battle stats on to the next egg -- reaching stage 4 is
  // the bar, not a guarantee of inheritance itself, so hatching still
  // feels like starting over most of the time rather than every stat grind
  // compounding forever across generations.
  uint8_t inheritedHp = 0, inheritedAttack = 0, inheritedDefense = 0, inheritedSpecial = 0;
  if (pet.stage == 4 && esp_random() % 100 < 50) {
    inheritedHp = pet.bonusHp / 2;
    inheritedAttack = pet.bonusAttack / 2;
    inheritedDefense = pet.bonusDefense / 2;
    inheritedSpecial = pet.bonusSpecial / 2;
  }
  const bool inherited =
      inheritedHp || inheritedAttack || inheritedDefense || inheritedSpecial;

  pet = {PET_MAGIC, 0, 0, 82, 82, 82, 100, 0, 0, {5, 0},
         inheritedHp, inheritedAttack, inheritedDefense, inheritedSpecial, nextGenome};
  savePet();
  if (settings.themeIndex == 0) applyTheme();
  newEggConfirmation = false;
  pendingHatchMode = 0;
  newEggConfirmationUntil = 0;
  showToastf(inherited ? "%s signal acquired -- stats inherited!"
                       : "%s signal acquired",
             eggLineageName(pet.genome.lineage));
  playTone(392, 70, 40);
  playTone(523, 70, 45);
  playTone(784, 130, 50);
  Serial.printf("Genome Lab: started %s, element=%s\n",
                eggLineageName(pet.genome.lineage),
                elementName(pet.genome.element));
}

// --- Loading the pet's own save (with its multi-generation migration chain) --

void loadPet() {
  const size_t storedSize = preferences.getBytesLength("state");
  if (storedSize == sizeof(pet)) {
    preferences.getBytes("state", &pet, sizeof(pet));
  } else if (storedSize > 0) {
    // See PET_MAGIC_PEDOMETER's own comment above -- this is the shape that
    // magic was saved under, present only to migrate off of it gracefully.
    struct StatePedometer {
      uint32_t magic;
      uint32_t ageMinutes;
      uint32_t actions;
      uint8_t food;
      uint8_t joy;
      uint8_t energy;
      uint8_t health;
      uint8_t stage;
      uint8_t training;
      uint8_t reserved[2];
      uint8_t bonusHp;
      uint8_t bonusAttack;
      uint8_t bonusDefense;
      uint8_t bonusSpecial;
      uint32_t stepsTotal;
      PetGenome genome;
    } pedometer{};
    // PET_MAGIC's own current layout, minus the bonusHp/Attack/Defense/
    // Special fields the Items feature added -- everyone's stored pet was
    // this shape immediately before today.
    struct StateV3 {
      uint32_t magic;
      uint32_t ageMinutes;
      uint32_t actions;
      uint8_t food;
      uint8_t joy;
      uint8_t energy;
      uint8_t health;
      uint8_t stage;
      uint8_t training;
      uint8_t reserved[2];
      PetGenome genome;
    } v3{};
    struct StateV2 {
      uint32_t magic;
      uint32_t ageMinutes;
      uint32_t actions;
      uint8_t food;
      uint8_t joy;
      uint8_t energy;
      uint8_t health;
      uint8_t stage;
      uint8_t training;
      uint8_t reserved[2];
    } previous{};
    struct OldState {
      uint32_t magic, ageMinutes, actions;
      uint8_t food, joy, energy, health;
    } old{};
    if (storedSize == sizeof(pedometer)) {
      preferences.getBytes("state", &pedometer, sizeof(pedometer));
    } else if (storedSize == sizeof(v3)) {
      preferences.getBytes("state", &v3, sizeof(v3));
    } else if (storedSize == sizeof(previous)) {
      preferences.getBytes("state", &previous, sizeof(previous));
    }
    if (pedometer.magic == PET_MAGIC_PEDOMETER) {
      pet = {PET_MAGIC, pedometer.ageMinutes, pedometer.actions, pedometer.food,
             pedometer.joy, pedometer.energy, pedometer.health, pedometer.stage,
             pedometer.training, {pedometer.reserved[0], pedometer.reserved[1]},
             pedometer.bonusHp, pedometer.bonusAttack, pedometer.bonusDefense,
             pedometer.bonusSpecial, pedometer.genome};
      Serial.println("Pet: existing companion kept (pedometer field dropped)");
    } else if (v3.magic == PET_MAGIC_V3) {
      pet = {PET_MAGIC, v3.ageMinutes, v3.actions, v3.food, v3.joy, v3.energy,
             v3.health, v3.stage, v3.training, {v3.reserved[0], v3.reserved[1]},
             0, 0, 0, 0, v3.genome};
      Serial.println("Pet: existing companion gained battle-stat item slots");
    } else if (previous.magic == PET_MAGIC_V2) {
      pet = {PET_MAGIC, previous.ageMinutes, previous.actions, previous.food,
             previous.joy, previous.energy, previous.health, previous.stage,
             previous.training, {previous.reserved[0], previous.reserved[1]},
             0, 0, 0, 0, generatePetGenome()};
      Serial.println("Pet: existing companion received a persistent genome");
    } else if (storedSize >= sizeof(old)) {
      preferences.getBytes("state", &old, sizeof(old));
    }
    if (old.magic == PET_MAGIC_V1) {
      pet = {PET_MAGIC, old.ageMinutes, old.actions, old.food, old.joy,
             old.energy, old.health, 1, 0, {0, 0}, 0, 0, 0, 0, generatePetGenome()};
    }
  }
  if (pet.magic != PET_MAGIC) {
    pet = {PET_MAGIC, 0, 0, 82, 82, 82, 100, 0, 0, {0, 0}, 0, 0, 0, 0,
           generatePetGenome()};
    Serial.printf("Pet: new %s selected (%s affinity)\n",
                  eggLineageName(pet.genome.lineage),
                  elementName(pet.genome.element));
  }
  // Battle-system bring-up profile. reserved[0] is the pet's level until
  // the persistent progression schema lands.
  pet.reserved[0] = 5;
  savePet();
}

// --- Battle touch handling ---------------------------------------------------

void handleBattleTap(int16_t x, int16_t y) {
  const FamiliarBattleState state = battle.state();
  if (showingBattleResults) {
    const auto &results = battle.scanResults();
    const uint8_t start = battleResultsPage * kBattleResultsPerPage;
    if (y >= 66 && y < 66 + kBattleResultsPerPage * 76) {
      const uint8_t row = (y - 66) / 76;
      const size_t index = start + row;
      // Rows are 68 tall inside each 76 slot -- a tap in the 8px gap still
      // resolves to the nearest row, which is fine here.
      if (index < results.size()) {
        showingBattleResults = false;
        playTone(659, 60, 40);
        // connectTo() blocks through the BLE connect handshake and service
        // discovery (can take a couple of seconds) -- show feedback first
        // so the tap doesn't look ignored while that runs.
        Arduino_GFX *previousDisplay = display;
        if (transitionsReady) display = &pageCanvasA;
        paintPageBackdrop();
        const PetPalette palette = paletteForGenome(pet.genome);
        drawCreaturePortrait(pet.genome, max<uint8_t>(pet.stage, 1), 184, 170, 70,
                             palette.glow);
        drawCentered("CONNECTING...", 260, 2, COLOR_CYAN);
        drawCentered("ESTABLISHING LINK", 296, 1, COLOR_MUTED);
        if (transitionsReady) {
          display = previousDisplay;
          panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);
        }
        battle.connectTo(index);
        presentBattlePage();
      }
    } else if (y >= 385 && y <= 428) {
      showingBattleResults = false;
      playTone(392, 60, 35);
      presentBattlePage();
    }
    return;
  }
  if (state == FamiliarBattleState::Battling && y >= 276 && y <= 382 &&
      !battle.myMoveSubmitted()) {
    // Matches the 2x2 grid in drawBattlingLayout: columns split at the
    // midpoint, rows split at the gap between the two button rows. Row
    // index doubled gives [ATK,DEF]/[SPEC,FLEE] = FamiliarBattleMove's own
    // Attack=0/Defend=1/Special=2/Flee=3 ordering directly.
    const uint8_t col = x < LCD_WIDTH / 2 ? 0 : 1;
    const uint8_t row = y < 329 ? 0 : 1;
    const uint8_t move = row * 2 + col;
    if (move == static_cast<uint8_t>(FamiliarBattleMove::Flee)) {
      if (fleeArmed && millis() < fleeArmedUntil) {
        fleeArmed = false;
        battle.submitMove(FamiliarBattleMove::Flee);
        playTone(220, 90, 40);
        presentBattlePage();
      } else {
        // First tap only arms it -- a second tap on Flee within the window
        // actually flees, so a stray tap can't end the battle by accident.
        fleeArmed = true;
        fleeArmedUntil = millis() + 4000;
        playTone(330, 60, 35);
        presentBattlePage();
      }
    } else {
      fleeArmed = false;
      battle.submitMove(static_cast<FamiliarBattleMove>(move));
      playTone(560 + move * 110, 55, 35);
      presentBattlePage();
    }
  } else if (state == FamiliarBattleState::Result) {
    if (battle.opponentGenomeCodeAvailable() && y >= 292 && x < LCD_WIDTH / 2) {
      PetGenome received{};
      if (decodePetGenome(battle.opponentGenomeCode(), received)) {
        saveCopiedGenome(received);
        battleGenomeCopied = hasCopiedGenome;
        playTone(784, 70, 45);
        playTone(1047, 110, 55);
        presentBattlePage();
      }
    } else {
      battle.end();
      presentBattlePage();
    }
  } else if ((state == FamiliarBattleState::Hosting ||
              state == FamiliarBattleState::Connecting) && y >= 340) {
    battle.end();
    presentBattlePage();
  } else if (state == FamiliarBattleState::Exchanged) {
    battle.end();
    presentBattlePage();
  }
}

// The Battle Idle state's action grid taps (see drawBattleActionsView()'s
// own comment in ui_pages.h for the tile layout) -- only reachable while
// battleShowingActions is true, same relationship handleStatusGridTap()
// has with the Status action grid.
void handleBattleGridTap(int16_t x, int16_t y) {
  if (y < 78 || y >= 364) return;
  const uint8_t column = x >= LCD_WIDTH / 2;
  const uint8_t row = y >= 227;
  const auto tile = static_cast<BattleGridTile>(row * 2 + column);
  if (tile == GRID_RIVALS) {
    showingRivals = true;
    presentOverlayEntrance(PAGE_BATTLE, drawRivalsPage);
    return;
  }
  if (tile == GRID_FRIENDS) {
    showingFriends = true;
    presentOverlayEntrance(PAGE_BATTLE, drawFriendsPage);
    return;
  }

  // tile == GRID_HOST or GRID_FIND -- verbatim what used to be the Idle
  // screen's own HOST/FIND button taps (handleBattleTap), just reached
  // through the grid now instead of two fixed buttons on the same screen
  // the stats used to share.
  const FamiliarBattleCapabilities capabilities = battleCapabilities();
  char genomeCode[PET_GENOME_CODE_LENGTH + 1]{};
  encodePetGenome(pet.genome, genomeCode, sizeof(genomeCode));
  battleGenomeCopied = false;
  fleeArmed = false;
  if (tile == GRID_HOST) {
    battle.beginHost(battlePlayerId(), pet.stage, 5, capabilities, genomeCode);
  } else {
    // beginFind() blocks for its whole scan window, so this is a single
    // static frame rather than an animated one -- still routed through
    // the canvas so it matches the rest of the app instead of the bare
    // direct-to-panel draw this used to be.
    Arduino_GFX *previousDisplay = display;
    if (transitionsReady) display = &pageCanvasA;
    paintPageBackdrop();
    const PetPalette palette = paletteForGenome(pet.genome);
    drawCreaturePortrait(pet.genome, max<uint8_t>(pet.stage, 1), 184, 170, 70, palette.glow);
    drawCentered("SCANNING BATTLE LINKS", 260, 2, COLOR_CYAN);
    drawCentered("4 SECOND SEARCH", 296, 1, COLOR_MUTED);
    if (transitionsReady) {
      display = previousDisplay;
      panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);
    }
    battle.beginFind(battlePlayerId(), pet.stage, 5, capabilities, genomeCode);
    // Shows a picker instead of auto-connecting to whatever was found
    // first -- battleResultsPage was already reset the last time results
    // were cleared (beginFind()) or a pick was made, so it only needs
    // resetting here if this is a fresh search.
    battleResultsPage = 0;
    showingBattleResults = !battle.scanResults().empty();
  }
  presentBattlePage();
}

// The Friends overlay's HOST/FIND buttons (see drawFriendsPage()'s own
// prototype comment) -- hit-test regions key off that function's exact
// 18/190,378 pair of rects. Any other tap closes the overlay, the same
// "tap anywhere to return" Rivals/Recon Log already use.
void handleFriendsTap(int16_t x, int16_t y) {
  if (y >= 378 && y < 432) {
    showingFriends = false;
    const FamiliarBattleCapabilities capabilities = battleCapabilities();
    if (x < LCD_WIDTH / 2) {
      battle.beginHost(battlePlayerId(), pet.stage, 5, capabilities, nullptr,
                       FamiliarBattleMode::FriendExchange);
    } else {
      // Same blocking-scan treatment as the Battle Idle screen's own FIND
      // button (handleBattleTap) -- a static frame while beginFind() blocks
      // for its whole scan window -- but auto-connects to the
      // first/strongest signal instead of showing the battle picker: a
      // friend exchange is a "we're both here right now" interaction, not
      // a pick-an-opponent one, and drawBattleResultsPage's "SELECT
      // OPPONENT"/level framing doesn't fit it.
      Arduino_GFX *previousDisplay = display;
      if (transitionsReady) display = &pageCanvasA;
      paintPageBackdrop();
      const PetPalette palette = paletteForGenome(pet.genome);
      drawCreaturePortrait(pet.genome, max<uint8_t>(pet.stage, 1), 184, 170, 70,
                           palette.glow);
      drawCentered("SCANNING FOR FRIENDS", 260, 2, COLOR_CYAN);
      drawCentered("4 SECOND SEARCH", 296, 1, COLOR_MUTED);
      if (transitionsReady) {
        display = previousDisplay;
        panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);
      }
      battle.beginFind(battlePlayerId(), pet.stage, 5, capabilities, nullptr,
                       FamiliarBattleMode::FriendExchange);
      if (!battle.scanResults().empty()) battle.connectTo(0);
    }
    presentBattlePage();
  } else {
    showingFriends = false;
    presentOverlayExit(drawFriendsPage, PAGE_BATTLE);
  }
}

// --- Items touch handling -----------------------------------------------------

// Refreshes the still-open Items overlay in place after useItem() changes
// its count/bonus display -- same "plain hard blit, no slide" shape as
// presentGenomeProfilePage()'s own EXPORT/IMPORT-triggered refresh, for the
// same reason: this is a redraw of an already-open overlay, not an
// open/close transition.
void presentItemsPage() {
  if (transitionsReady) {
    Arduino_GFX *previousDisplay = display;
    display = &pageCanvasA;
    drawItemsPage();
    display = previousDisplay;
    panel->draw16bitRGBBitmap(0, 0, pageCanvasA.getFramebuffer(), LCD_WIDTH, LCD_HEIGHT);
  } else {
    drawItemsPage();
  }
}

// The Items overlay's row taps (see drawItemsPage()'s own prototype
// comment for why this uses an explicit BACK button, y >= 375 here,
// instead of Rivals/Recon Log/Friends' tap-anywhere dismissal) and its
// per-item hit-test rows, keyed off that function's exact kFirstRowY/
// kRowHeight geometry.
void handleItemsTap(int16_t x, int16_t y) {
  if (y >= 375) {
    showingItems = false;
    presentOverlayExit(drawItemsPage, PAGE_STATUS);
    return;
  }
  constexpr int16_t kFirstRowY = 76;
  constexpr int16_t kRowHeight = 52;
  if (y < kFirstRowY) return;
  const uint8_t row = (y - kFirstRowY) / kRowHeight;
  if (row >= kItemTypeCount) return;
  useItem(row);
  presentItemsPage();
}

// --- The console/game seam itself -- gameApplyAutoTheme()/
// --- gameBootAnimationPalette()/gameBootAnimationSeed() are wired for real
// --- (Stage 1's landmine-fix step); the rest are still placeholders, same
// --- as this file's top comment describes -- the dispatch cutover that
// --- wires those for real is a later Stage 1 step. ------------------------

void gameInit() {
  // TODO(stage1): call loadPet/loadCopiedGenome/loadBattleStats/
  // loadReconLog/loadFriendsList/loadInventory -- setup() still calls each
  // of these directly today.
}

void gameTick(uint32_t /*nowMs*/) {
  // TODO(stage1): agePet()'s once-a-minute tick, updateCreatureAnimation()
  // -- loop() still calls each of these directly today.
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
  // showingFriends/showingItems/showingGenomeProfile.
  return false;
}

void gameOverlayTouch(int16_t /*x*/, int16_t /*y*/, int16_t /*dx*/, int16_t /*dy*/,
                      bool /*released*/) {
  // TODO(stage1): the moved overlay if-chain's touch handling.
}

// Was applyTheme()'s AUTO branch, verbatim -- moved rather than rewritten
// so this stays exactly the palette-to-theme mapping it always was.
void gameApplyAutoTheme(ThemeColors &out) {
  const PetPalette petPalette = paletteForGenome(pet.genome);
  out.background = scaleRgb565(petPalette.primaryDark, 22);
  out.card = scaleRgb565(petPalette.primaryDark, 55);
  out.primary = petPalette.primaryLight;
  out.text = 0xF7BE;
  // Blended toward `text` rather than just darkened off petPalette.secondary
  // (the old scaleRgb565(..., 82) here) -- AUTO's background/card are
  // genome-derived and can land anywhere, so unlike the 4 fixed themes' own
  // muted tone (see kThemes[]' own comment in ui_pages.h) there's no single
  // contrast fix that's provably correct for every possible genome. Leaning
  // on the one fixed, always-bright color this branch has (`text`) is the
  // best available heuristic improvement over a flat darken, which had no
  // relationship to either backdrop's actual brightness at all.
  out.muted = lerpRgb565(petPalette.secondary, out.text, 0.35f);
  out.warning = petPalette.accent;
  out.danger = 0xF2CB;
  out.cyan = petPalette.glow;
  out.secondary = petPalette.secondary;
}

PetPalette gameBootAnimationPalette() {
  return paletteForGenome(pet.genome);
}

void gameBootAnimationSeed(uint32_t seed[4]) {
  for (uint8_t i = 0; i < 4; ++i) seed[i] = pet.genome.seed[i];
}

void gameOnBattleStateChanged() {
  // TODO(stage1): recordBattleOutcome() on the edge into Result,
  // addFriend() on the edge into Exchanged -- loop() still calls each of
  // these directly today.
}
