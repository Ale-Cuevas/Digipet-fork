// Native render harness: links the real ui_pages.cpp (extracted verbatim
// from main.cpp -- see include/ui_pages.h's own comment) against
// Arduino_GFX/Arduino_Canvas compiled for the host, feeds it a fixed test
// PetState, and dumps whichever page was asked for as a PPM (converted to
// PNG by tools/sim/render.sh). Not part of the firmware or the pio test
// suite -- see tools/sim/README.md for how and when to use this.

#include <Arduino_GFX.h>
#include <canvas/Arduino_Canvas.h>

#include <cstdio>
#include <cstring>

#include "pet_genome.h"
#include "ui_pages.h"

// --- Definitions for the globals ui_pages.h declares extern -------------------
// The real firmware (src/main.cpp) defines these backed by actual hardware
// state (theme colors, sensor detection, RTC, etc.); this harness defines
// its own fixed/fake versions purely for rendering.

Arduino_GFX *display = nullptr;
PetState pet{};

uint16_t COLOR_BACKGROUND = 0x0823;
uint16_t COLOR_CARD = 0x18E8;
uint16_t COLOR_MINT = 0x6718;
uint16_t COLOR_TEXT = 0xE73C;
uint16_t COLOR_MUTED = 0x8413;
uint16_t COLOR_WARNING = 0xFE48;
uint16_t COLOR_DANGER = 0xF2CB;
uint16_t COLOR_CYAN = 0x269F;
uint16_t COLOR_PURPLE = 0xA81F;

const char *STAGE_NAMES[] = {"BIT EGG", "HATCHLING", "SCOUT", "GUARDIAN", "TITAN"};

bool clockValid = true;
char clockText[6] = "12:34";

bool imuDetected = true;
bool rtcDetected = true;
bool pmuDetected = true;
bool codecDetected = true;
uint8_t i2cDeviceCount = 7;

bool animationFrame = false;

DeviceSettings settings{};
const char *SLEEP_LABELS[] = {"15 SEC", "30 SEC", "1 MIN", "2 MIN"};
const char *VOLUME_LABELS[] = {"MUTE", "LOW", "MEDIUM", "HIGH", "MAX"};
const char *WAKE_LABELS[] = {"TOUCH", "BOOT KEY"};
const char *THEME_LABELS[] = {"AUTO // PET", "CYBER MINT", "AMBER CORE",
                              "VIOLET LINK", "MONO SIGNAL"};
uint8_t settingsGridPage = 0;
SettingsView settingsView = SETTINGS_HOME;

BattleStats battleStats{};
ReconLog reconLog{};
FriendsList friendsList{};
Inventory inventory{};
bool genomeProfileShowingData = false;
char genomeTransferStatus[32] = "NO COPIED GENOME";
bool statusShowingActions = false;
uint8_t statusActionsPage = 0;

bool hasCopiedGenome = false;
bool newEggConfirmation = false;
uint32_t newEggConfirmationUntil = 0;
uint8_t pendingHatchMode = 0;

namespace {

void seedTestPet(uint8_t stage) {
  // Fixed, arbitrary seeds -- deterministic output is the point of a
  // visual-diff tool. Swap these (or expose more CLI flags) to preview a
  // different genome/lineage/element.
  const uint32_t seed[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
  const uint32_t evolutionSeed[4] = {0x55555555, 0x66666666, 0x77777777, 0x88888888};
  pet = PetState{};
  pet.ageMinutes = 1440;
  pet.actions = 50;
  pet.food = 80;
  pet.joy = 65;
  pet.energy = 90;
  pet.health = 100;
  pet.stage = stage;
  pet.training = 40;
  pet.genome = derivePetGenome(seed, evolutionSeed);
}

PetGenome opponentTestGenome() {
  const uint32_t seed[4] = {0xCAFEBABE, 0xDEADBEEF, 0x0BADF00D, 0x8BADF00D};
  const uint32_t evolutionSeed[4] = {0xFEEDFACE, 0x12345678, 0x9ABCDEF0, 0x01234567};
  return derivePetGenome(seed, evolutionSeed);
}

bool writePpm(Arduino_Canvas &canvas, const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  fprintf(f, "P6\n%d %d\n255\n", LCD_WIDTH, LCD_HEIGHT);
  const uint16_t *fb = canvas.getFramebuffer();
  for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; ++i) {
    const uint16_t px = fb[i];
    fputc(((px >> 11) & 0x1F) * 255 / 31, f);
    fputc(((px >> 5) & 0x3F) * 255 / 63, f);
    fputc((px & 0x1F) * 255 / 31, f);
  }
  fclose(f);
  return true;
}

}  // namespace

namespace {

const char *kPageNames =
    "companion|status|status-actions[2]|egg|settings|settings2|"
    "settings-brightness|settings-idle|settings-volume|settings-wake|"
    "settings-theme|settings-boot|genomelab|"
    "battle-idle-stats|battle-idle-actions|"
    "battle-fight[-highstats|-lowhp|-submitted|-fleearmed|-nogenome]|"
    "battle-result[-copied|-nogenome]|battle-pickerN (N=result count, "
    "[stage] arg becomes the results page)|rivals[-empty]|recon[-empty]|"
    "friends[-empty]|items[-empty]|genome-identity|genome-data";

void seedTestSettings() {
  settings = DeviceSettings{};
  settings.brightnessIndex = 7;
  settings.sleepIndex = 1;
  settings.soundEnabled = true;
  settings.bootAnimationEnabled = true;
  settings.volumeIndex = 3;
  settings.wakeMode = 0;
  settings.themeIndex = 0;
}

// Slugs for ui_pages.h's shared kThemes table, index-aligned with it (and
// with settings.themeIndex/THEME_LABELS) so index 0/AUTO -- genome-derived,
// no fixed palette -- is correctly not offered here. Lets --theme=N (or
// one of these names) re-point every COLOR_* global the same way the real
// Settings > Theme picker does, so button-contrast fixes like
// readableTextColor() can be checked against all 4 fixed themes, not just
// the Cyber Mint default this file's own globals above are seeded with.
struct HarnessThemeName {
  const char *slug;
  int index;
};
constexpr HarnessThemeName kHarnessThemeNames[] = {
    {"cyber-mint", 1},
    {"amber-core", 2},
    {"violet-link", 3},
    {"mono-signal", 4},
};

void applyHarnessTheme(int index) {
  if (index <= 0 || index >= kThemeCount) {
    fprintf(stderr, "unknown --theme index %d (1-%d; 0/AUTO has no fixed palette)\n", index,
            kThemeCount - 1);
    exit(1);
  }
  const ThemeColors &t = kThemes[index];
  COLOR_BACKGROUND = t.background;
  COLOR_CARD = t.card;
  COLOR_MINT = t.primary;
  COLOR_TEXT = t.text;
  COLOR_MUTED = t.muted;
  COLOR_WARNING = t.warning;
  COLOR_DANGER = t.danger;
  COLOR_CYAN = t.cyan;
  COLOR_PURPLE = t.secondary;
}

}  // namespace

int main(int argc, char **argv) {
  // Pull out an optional "--theme=N" or "--theme=name" token from anywhere
  // in argv, wherever positional [stage]/[resultPage] falls -- it isn't
  // itself positional, so this runs before the usual argv[1]/[2]/[3] reads.
  int themeIndex = -1;
  int outArgc = 0;
  char *outArgv[16];
  for (int i = 0; i < argc && outArgc < 16; ++i) {
    if (strncmp(argv[i], "--theme=", 8) == 0) {
      const char *value = argv[i] + 8;
      themeIndex = -1;
      for (const auto &named : kHarnessThemeNames) {
        if (strcmp(value, named.slug) == 0) themeIndex = named.index;
      }
      if (themeIndex < 0) themeIndex = atoi(value);
    } else {
      outArgv[outArgc++] = argv[i];
    }
  }
  argc = outArgc;
  argv = outArgv;

  if (argc < 3) {
    fprintf(stderr, "usage: %s <%s> <output.ppm> [stage] [--theme=N|name]\n", argv[0],
            kPageNames);
    return 1;
  }
  const char *page = argv[1];
  const char *outPath = argv[2];
  const uint8_t stage = argc > 3 ? static_cast<uint8_t>(atoi(argv[3])) : 2;

  Arduino_Canvas canvas(LCD_WIDTH, LCD_HEIGHT, nullptr);
  canvas.begin();
  display = &canvas;
  seedTestSettings();
  if (themeIndex >= 0) applyHarnessTheme(themeIndex);

  if (strcmp(page, "companion") == 0) {
    seedTestPet(stage);
    drawCompanionPage();
  } else if (strcmp(page, "status") == 0) {
    seedTestPet(stage);
    statusShowingActions = false;
    drawStatusPage();
  } else if (strcmp(page, "status-actions") == 0) {
    seedTestPet(stage);
    statusShowingActions = true;
    statusActionsPage = 0;
    drawStatusPage();
  } else if (strcmp(page, "status-actions2") == 0) {
    seedTestPet(stage);
    statusShowingActions = true;
    statusActionsPage = 1;
    drawStatusPage();
  } else if (strcmp(page, "egg") == 0) {
    seedTestPet(0);
    drawCompanionPage();
  } else if (strcmp(page, "settings") == 0) {
    seedTestPet(stage);
    settingsGridPage = 0;
    settingsView = SETTINGS_HOME;
    drawSettingsPage();
  } else if (strcmp(page, "settings2") == 0) {
    seedTestPet(stage);
    settingsGridPage = 1;
    settingsView = SETTINGS_HOME;
    drawSettingsPage();
  } else if (strncmp(page, "settings-", 9) == 0) {
    seedTestPet(stage);
    const char *sub = page + 9;
    if (strcmp(sub, "brightness") == 0) settingsView = SETTINGS_BRIGHTNESS;
    else if (strcmp(sub, "idle") == 0) settingsView = SETTINGS_IDLE;
    else if (strcmp(sub, "volume") == 0) settingsView = SETTINGS_VOLUME;
    else if (strcmp(sub, "wake") == 0) settingsView = SETTINGS_WAKE;
    else if (strcmp(sub, "theme") == 0) settingsView = SETTINGS_THEME;
    else if (strcmp(sub, "boot") == 0) settingsView = SETTINGS_BOOT;
    else {
      fprintf(stderr, "unknown settings sub-view '%s'\n", sub);
      return 1;
    }
    drawSettingsPage();
  } else if (strcmp(page, "genomelab") == 0) {
    seedTestPet(stage);
    drawGenomeLabPage();
  } else if (strncmp(page, "battle-", 7) == 0) {
    seedTestPet(2);
    const char *scenario = page + 7;
    const PetGenome opponentGenome = opponentTestGenome();

    const bool isIdleView = strcmp(scenario, "idle-stats") == 0 ||
                            strcmp(scenario, "idle-actions") == 0;
    const bool isFightOrResult =
        strncmp(scenario, "fight", 5) == 0 || strncmp(scenario, "result", 6) == 0;
    if (isFightOrResult || isIdleView) {
      // drawBattlingLayout/drawBattleStatsView/drawBattleActionsView are
      // sub-components (see each one's own comment) -- the real
      // drawBattlePage() always wraps them in the backdrop/title/page-dots
      // every other page gets. drawBattleResultsPage (the picker), below,
      // already does this itself.
      paintPageBackdrop();
      drawCentered("PVP", 16, 3, COLOR_MINT);
    }
    if (strcmp(scenario, "idle-stats") == 0) {
      drawBattleStatsView();
    } else if (strcmp(scenario, "idle-actions") == 0) {
      drawBattleActionsView();
    } else if (strcmp(scenario, "fight") == 0 || strcmp(scenario, "fight-highstats") == 0 ||
        strcmp(scenario, "fight-lowhp") == 0 || strcmp(scenario, "fight-submitted") == 0 ||
        strcmp(scenario, "fight-fleearmed") == 0 || strcmp(scenario, "fight-nogenome") == 0) {
      uint16_t myHp = 24, myMaxHp = 40, oppHp = 31, oppMaxHp = 40;
      uint8_t oppLevel = 5;
      bool moveSubmitted = false, fleeArmed = false, hasGenome = true;
      const char *log1 = "You: ATTACK -6 dmg";
      const char *log2 = "Opp: DEFEND";
      if (strcmp(scenario, "fight-highstats") == 0) {
        myHp = 317; myMaxHp = 317; oppHp = 317; oppMaxHp = 317; oppLevel = 99;
        log1 = "You: SPECIAL -142 dmg"; log2 = "Opp: SPECIAL MISSED";
      } else if (strcmp(scenario, "fight-lowhp") == 0) {
        myHp = 2; myMaxHp = 40; oppHp = 1; oppMaxHp = 40;
      } else if (strcmp(scenario, "fight-submitted") == 0) {
        moveSubmitted = true;
      } else if (strcmp(scenario, "fight-fleearmed") == 0) {
        fleeArmed = true;
      } else if (strcmp(scenario, "fight-nogenome") == 0) {
        hasGenome = false;
      }
      drawBattlingLayout(myHp, myMaxHp, oppHp, oppMaxHp, oppLevel, true, log1, log2,
                         /*isResult=*/false, moveSubmitted, fleeArmed,
                         /*opponentGenomeAvailable=*/hasGenome, /*genomeCopied=*/false,
                         hasGenome ? &opponentGenome : nullptr);
    } else if (strcmp(scenario, "result") == 0 || strcmp(scenario, "result-copied") == 0 ||
               strcmp(scenario, "result-nogenome") == 0) {
      const bool hasGenome = strcmp(scenario, "result-nogenome") != 0;
      const bool copied = strcmp(scenario, "result-copied") == 0;
      drawBattlingLayout(0, 40, 40, 40, 5, true, "", "Opp: ATTACK -24 dmg",
                         /*isResult=*/true, false, false, hasGenome, copied,
                         hasGenome ? &opponentGenome : nullptr);
    } else if (strncmp(scenario, "picker", 6) == 0) {
      const int count = atoi(scenario + 6);
      const uint8_t resultPage = argc > 3 ? static_cast<uint8_t>(atoi(argv[3])) : 0;
      std::vector<FamiliarBattleOpponent> results;
      const int8_t rssiSamples[] = {-45, -68, -80, -50, -72};
      for (int i = 0; i < count; ++i) {
        FamiliarBattleOpponent opponent;
        opponent.stageIndex = (i % 4) + 1;
        opponent.level = 3 + i;
        opponent.rssi = rssiSamples[i % 5];
        results.push_back(opponent);
      }
      drawBattleResultsPage(results, resultPage);
    } else {
      fprintf(stderr, "unknown battle scenario '%s'\n", scenario);
      return 1;
    }
    if (isFightOrResult || isIdleView) drawPageDots(PAGE_BATTLE);
  } else if (strcmp(page, "rivals-empty") == 0) {
    battleStats = BattleStats{};
    drawRivalsPage();
  } else if (strcmp(page, "rivals") == 0) {
    battleStats = BattleStats{};
    const uint32_t ids[] = {0x7B6CCCu, 0xA1B2C3D4u, 0x00001234u, 0xDEADBEEFu,
                            0x00FFAAu, 0x5A5A5A5Au, 0x1u, 0xFFFFFFFFu};
    const uint16_t wins[] = {3, 1, 0, 12, 2, 5, 0, 99};
    const uint16_t losses[] = {1, 0, 4, 3, 2, 5, 1, 0};
    const int count = argc > 3 ? atoi(argv[3]) : kMaxBattleRivals;
    battleStats.rivalCount = static_cast<uint8_t>(
        std::min(count, static_cast<int>(kMaxBattleRivals)));
    for (uint8_t i = 0; i < battleStats.rivalCount; ++i) {
      battleStats.rivals[i] = {ids[i], wins[i], losses[i]};
    }
    drawRivalsPage();
  } else if (strcmp(page, "recon-empty") == 0) {
    reconLog = ReconLog{};
    drawReconLogPage();
  } else if (strcmp(page, "recon") == 0) {
    reconLog = ReconLog{};
    const char *labels[] = {"HomeNet_5G", "", "Coffee_Free_WiFi", "",
                            "xfinitywifi", "", "PixelBuds_Pro", ""};
    const uint8_t kinds[] = {0, 1, 0, 1, 0, 1, 0, 1};
    const int count = argc > 3 ? atoi(argv[3]) : kMaxReconEntries;
    reconLog.entryCount = static_cast<uint8_t>(
        std::min(count, static_cast<int>(kMaxReconEntries)));
    for (uint8_t i = 0; i < reconLog.entryCount; ++i) {
      ReconEntry &entry = reconLog.entries[i];
      entry.idHash = 0x1000u + i;
      entry.kind = kinds[i];
      entry.firstSeenAge = i * 40;
      strncpy(entry.label, labels[i], sizeof(entry.label) - 1);
    }
    drawReconLogPage();
  } else if (strcmp(page, "friends-empty") == 0) {
    friendsList = FriendsList{};
    drawFriendsPage();
  } else if (strcmp(page, "friends") == 0) {
    friendsList = FriendsList{};
    const uint32_t ids[] = {0x7B6CCCu, 0xA1B2C3D4u, 0x00001234u, 0xDEADBEEFu,
                            0x00FFAAu, 0x5A5A5A5Au, 0x1u, 0xFFFFFFFFu};
    const int count = argc > 3 ? atoi(argv[3]) : kMaxFriends;
    friendsList.count = static_cast<uint8_t>(std::min(count, static_cast<int>(kMaxFriends)));
    for (uint8_t i = 0; i < friendsList.count; ++i) {
      friendsList.friends[i] = {ids[i], static_cast<uint32_t>(i * 60)};
    }
    drawFriendsPage();
  } else if (strcmp(page, "items-empty") == 0) {
    seedTestPet(stage);
    inventory = Inventory{};
    drawItemsPage();
  } else if (strcmp(page, "items") == 0) {
    seedTestPet(stage);
    inventory = Inventory{};
    inventory.counts[ITEM_HP_BOOST] = 3;
    inventory.counts[ITEM_ATK_BOOST] = 1;
    inventory.counts[ITEM_SPECIAL_BOOST] = 2;
    pet.bonusHp = 8;
    pet.bonusAttack = 2;
    pet.bonusDefense = 0;
    pet.bonusSpecial = 4;
    drawItemsPage();
  } else if (strcmp(page, "genome-identity") == 0) {
    seedTestPet(stage);
    genomeProfileShowingData = false;
    drawGenomeProfilePage();
  } else if (strcmp(page, "genome-data") == 0) {
    seedTestPet(stage);
    genomeProfileShowingData = true;
    strncpy(genomeTransferStatus, "COPIED GENOME READY", sizeof(genomeTransferStatus) - 1);
    drawGenomeProfilePage();
  } else {
    fprintf(stderr, "unknown page '%s' (want %s)\n", page, kPageNames);
    return 1;
  }

  if (!writePpm(canvas, outPath)) {
    fprintf(stderr, "failed to write %s\n", outPath);
    return 1;
  }
  printf("wrote %s\n", outPath);
  return 0;
}
