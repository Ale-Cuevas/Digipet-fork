#pragma once

#include <Arduino.h>

#include "game/vpet/pet_genome.h"

// The vpet game's own persisted-state shapes, split out of ui_pages.h as
// part of the console/game seam (see the "Stage 1" plan this repo's git
// history was extracted under). ui_pages.h still declares `extern PetState
// pet;` etc. -- those globals are owned by whichever game module is linked
// in (currently only game_vpet's main.cpp-resident definitions); this
// header only owns what the *shape* of that state is, so a future,
// structurally different game (its own character-sheet struct, its own
// magic-guarded blobs) never needs to touch this file or anything that
// includes it.

struct PetState {
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
  // Permanent battle-stat boosts from consumed items (see main.cpp's
  // Inventory/useItem()) -- HP/ATK/DEF/Special are all boostable the same
  // way; Type isn't a numeric stat, so an item that changes it (TYPE_SHIFT)
  // writes genome.element directly instead of a field here. Added in
  // PET_MAGIC's 4th schema (see main.cpp's own PET_MAGIC_V3/PET_MAGIC
  // comment) -- not reserved[]'s remaining byte, since these are 4 fields,
  // not 1.
  uint8_t bonusHp;
  uint8_t bonusAttack;
  uint8_t bonusDefense;
  uint8_t bonusSpecial;
  PetGenome genome;
};

// A per-device record, not a per-pet one -- persists across egg hatches/
// blends. Definition (magic-guarded, persisted through Preferences/NVS)
// stays in main.cpp; the type lives here because drawRivalsPage() reads it.
constexpr uint8_t kMaxBattleRivals = 8;

struct BattleRival {
  uint32_t playerId = 0;
  uint16_t wins = 0;
  uint16_t losses = 0;
};

struct BattleStats {
  uint32_t magic = 0;
  uint32_t wins = 0;
  uint32_t losses = 0;
  uint32_t fled = 0;
  uint32_t opponentFled = 0;
  uint32_t disconnected = 0;
  uint8_t rivalCount = 0;
  uint8_t reserved[3]{};
  BattleRival rivals[kMaxBattleRivals]{};
};

// Also a per-device record, same shape of reasoning as BattleStats just
// above -- persists across egg hatches/blends, magic-guarded definition
// stays in main.cpp. Populated by FEED's WiFi/BLE signal scan
// (performFeedScan(), main.cpp); drawReconLogPage() reads it directly.
constexpr uint8_t kMaxReconEntries = 8;

struct ReconEntry {
  uint32_t idHash = 0;        // FNV-1a of the BSSID (WiFi) or address string (BLE).
  uint32_t firstSeenAge = 0;  // pet.ageMinutes at first sighting -- a relative,
                              // always-valid clock, unlike wall time (no RTC needed).
  uint8_t kind = 0;           // 0 = WiFi AP, 1 = BLE device.
  char label[13] = {};        // WiFi: truncated SSID. BLE: empty -- a passive scan
                              // never connects, so no name is ever exchanged.
};

struct ReconLog {
  uint32_t magic = 0;
  uint32_t totalScans = 0;
  uint32_t totalUniqueSeen = 0;
  uint8_t entryCount = 0;
  uint8_t reserved[3]{};
  ReconEntry entries[kMaxReconEntries]{};
};

// Same per-device-record reasoning as BattleStats/ReconLog above. Added via
// a live BLE "Exchange ID" handshake (FamiliarBattleMode::FriendExchange,
// familiar_battle_service.h) rather than a battle outcome -- see main.cpp's
// addFriend(), called on the Idle-state edge into
// FamiliarBattleState::Exchanged the same way recordBattleOutcome() is
// called on the edge into Result.
constexpr uint8_t kMaxFriends = 8;

struct FriendEntry {
  uint32_t playerId = 0;
  uint32_t addedAge = 0;  // pet.ageMinutes when this friend was added.
};

struct FriendsList {
  uint32_t magic = 0;
  uint8_t count = 0;
  uint8_t reserved[3]{};
  FriendEntry friends[kMaxFriends]{};
};

// A rare bonus drop from FEED's WiFi/BLE signal scan (see main.cpp's
// performFeedScan()) when it turns up a device never seen before --
// "mutation trigger" hardware in the wild, in this app's own fiction.
// HP/ATK/DEF/SPECIAL are numeric boosts, all mandatory in the sense that
// every one of them maps onto a stat this app's battle math already has
// (deriveMaxHp/deriveAttack/deriveDefense, FamiliarBattleCapabilities::
// special) -- a future cross-device sync of these wouldn't need any peer
// to support something new. TYPE_SHIFT is deliberately last/separate: it
// rerolls genome.element (a categorical pick, not a number to add to) and
// isn't a stat any wire-protocol capability negotiation is about, so nothing
// here assumes a peer device understands it either.
enum ItemType : uint8_t {
  ITEM_HP_BOOST,
  ITEM_ATK_BOOST,
  ITEM_DEF_BOOST,
  ITEM_SPECIAL_BOOST,
  ITEM_TYPE_SHIFT,
};
constexpr uint8_t kItemTypeCount = 5;

struct Inventory {
  uint32_t magic = 0;
  uint8_t counts[kItemTypeCount]{};
};
