// FamiliarBattleService for the simulator: the same public behaviour as
// src/familiar_battle_service.cpp, but the peer is a local bot instead of a
// second badge over BLE. Turns resolve through the real rules
// (resolveBattleTurn), so damage, speed and misses match a live match;
// only the radio and the opponent's choices are made up.
//
// Find lists three bots; Host gets challenged by one after a few seconds;
// FriendExchange modes complete the exchange instead of battling.

#include "familiar_battle_service.h"

#include <algorithm>
#include <cstring>

#include "game/vpet/familiar_battle_rules.h"
#include "game/vpet/pet_genome.h"

namespace {

constexpr uint32_t kHostChallengeDelayMs = 3500;
constexpr uint32_t kConnectDelayMs = 700;
constexpr uint32_t kBotThinkMs = 900;
constexpr uint32_t kScanDurationMs = 4000;  // same bounded scan as the real service

// What a challenger connecting to us in Host mode would have advertised.
FamiliarBattleOpponent makeBot(uint8_t index) {
  FamiliarBattleOpponent bot;
  char address[18];
  snprintf(address, sizeof(address), "02:B0:75:00:00:%02X", index);
  bot.address = address;
  bot.playerId = 0xB0700000u | (esp_random() & 0xFFFF);
  bot.stageIndex = 1 + esp_random() % 4;
  bot.level = 5;
  bot.rssi = -45 - static_cast<int>(esp_random() % 40);
  bot.capabilities.flags = FamiliarCapBodyType | FamiliarCapElement | FamiliarCapSpeed |
                           FamiliarCapSpecial | FamiliarCapMoveMatchups;
  bot.capabilities.bodyType = esp_random() % 5;
  bot.capabilities.element = esp_random() % 6;
  bot.capabilities.speed = esp_random() % 16;
  bot.capabilities.special = esp_random() % 16;
  return bot;
}

BattleRulesCapabilities toRulesCapabilities(const FamiliarBattleCapabilities &c) {
  BattleRulesCapabilities rules;
  rules.bodyType = c.bodyType;
  rules.element = c.element;
  rules.speed = c.speed;
  rules.special = c.special;
  return rules;
}

// Bot timeline: when the next scripted peer event fires (0 = none pending).
uint32_t botEventAtMs = 0;

}  // namespace

uint16_t FamiliarBattleService::deriveMaxHp(uint8_t level) { return battleRulesDeriveMaxHp(level); }

uint8_t FamiliarBattleService::deriveAttack(uint8_t level, uint8_t stageIndex) {
  return battleRulesDeriveAttack(level, stageIndex);
}

uint8_t FamiliarBattleService::deriveDefense(uint8_t level, uint8_t stageIndex) {
  return battleRulesDeriveDefense(level, stageIndex);
}

void FamiliarBattleService::beginRadio() {}
void FamiliarBattleService::teardownRadio() {}

void FamiliarBattleService::resetForNewBattle() {
  opponent_ = FamiliarBattleOpponent{};
  opponentAttack_ = opponentDefense_ = 0;
  myHp_ = myMaxHp_ = 0;
  opponentHp_ = opponentMaxHp_ = 0;
  turnNumber_ = 0;
  myMoveSubmitted_ = opponentMoveSubmitted_ = false;
  helloSent_ = helloReceived_ = false;
  prngState_ = esp_random() | 1;
  outcome_ = FamiliarBattleOutcome::None;
  log_.clear();
  myGenomeCode_[0] = '\0';
  memset(opponentGenomeCode_, 0, sizeof(opponentGenomeCode_));
  opponentGenomeChunks_ = 0;
  botEventAtMs = 0;
}

bool FamiliarBattleService::beginHost(uint32_t playerId, uint8_t stageIndex, uint8_t level,
                                      const FamiliarBattleCapabilities &capabilities,
                                      const char *genomeCode, FamiliarBattleMode mode) {
  end();
  resetForNewBattle();
  isHost_ = true;
  myMode_ = mode;
  myPlayerId_ = playerId;
  myStageIndex_ = stageIndex;
  myLevel_ = level;
  myCapabilities_ = capabilities;
  if (genomeCode && strlen(genomeCode) == 60)
    strncpy(myGenomeCode_, genomeCode, sizeof(myGenomeCode_) - 1);
  myAttack_ = deriveAttack(level, stageIndex);
  myDefense_ = deriveDefense(level, stageIndex);
  myMaxHp_ = myHp_ = deriveMaxHp(level);
  state_ = FamiliarBattleState::Hosting;
  status_ = "Waiting for a challenger...";
  // Friend discovery stays visible until the user leaves; a bot drops by
  // the same way a challenger does.
  botEventAtMs = millis() + kHostChallengeDelayMs;
  return true;
}

bool FamiliarBattleService::beginFind(uint32_t playerId, uint8_t stageIndex, uint8_t level,
                                      const FamiliarBattleCapabilities &capabilities,
                                      const char *genomeCode, FamiliarBattleMode mode) {
  end();
  resetForNewBattle();
  isHost_ = false;
  myMode_ = mode;
  myPlayerId_ = playerId;
  myStageIndex_ = stageIndex;
  myLevel_ = level;
  myCapabilities_ = capabilities;
  if (genomeCode && strlen(genomeCode) == 60)
    strncpy(myGenomeCode_, genomeCode, sizeof(myGenomeCode_) - 1);
  myAttack_ = deriveAttack(level, stageIndex);
  myDefense_ = deriveDefense(level, stageIndex);
  myMaxHp_ = myHp_ = deriveMaxHp(level);

  // Blocking, like the real bounded BLE scan.
  state_ = FamiliarBattleState::Scanning;
  status_ = "Scanning...";
  scanResults_.clear();
  delay(kScanDurationMs);
  for (uint8_t i = 0; i < 3; ++i) scanResults_.push_back(makeBot(i));
  std::sort(scanResults_.begin(), scanResults_.end(),
            [](const FamiliarBattleOpponent &a, const FamiliarBattleOpponent &b) {
              return a.rssi > b.rssi;
            });
  state_ = FamiliarBattleState::Idle;
  status_ = String(scanResults_.size()) + " found";
  return true;
}

std::vector<uint32_t> FamiliarBattleService::scanNearbyBle(uint32_t durationMs) {
  std::vector<uint32_t> hashes;
  if (state_ != FamiliarBattleState::Idle) return hashes;
  delay(std::min<uint32_t>(durationMs, 2000));
  // A stable neighbourhood of 10 advertisers, a random handful in range.
  for (uint32_t i = 0; i < 10; ++i) {
    if (esp_random() % 100 < 50) hashes.push_back(0x51B1E000u + i * 7919u);
  }
  return hashes;
}

bool FamiliarBattleService::connectTo(size_t resultIndex) {
  if (resultIndex >= scanResults_.size()) return false;
  delay(150);
  opponent_ = scanResults_[resultIndex];
  opponentAttack_ = deriveAttack(opponent_.level, opponent_.stageIndex);
  opponentDefense_ = deriveDefense(opponent_.level, opponent_.stageIndex);
  opponentMaxHp_ = opponentHp_ = deriveMaxHp(opponent_.level);
  state_ = FamiliarBattleState::Connecting;
  status_ = "Connected - starting battle...";
  botEventAtMs = millis() + kConnectDelayMs;
  return true;
}

void FamiliarBattleService::end() {
  state_ = FamiliarBattleState::Idle;
  status_ = "Idle";
  isHost_ = false;
  botEventAtMs = 0;
}

void FamiliarBattleService::onRawInbound(const uint8_t *, size_t) {}
void FamiliarBattleService::onPeerConnected() {}
void FamiliarBattleService::onPeerDisconnected() {}

void FamiliarBattleService::update() {
  if (botEventAtMs == 0 || static_cast<int32_t>(millis() - botEventAtMs) < 0) return;
  botEventAtMs = 0;

  if (state_ == FamiliarBattleState::Hosting) {
    opponent_ = makeBot(9);
    opponentAttack_ = deriveAttack(opponent_.level, opponent_.stageIndex);
    opponentDefense_ = deriveDefense(opponent_.level, opponent_.stageIndex);
    opponentMaxHp_ = opponentHp_ = deriveMaxHp(opponent_.level);
    state_ = FamiliarBattleState::Connecting;
    status_ = "Challenger connected - awaiting handshake...";
    botEventAtMs = millis() + kConnectDelayMs;
    return;
  }

  if (state_ == FamiliarBattleState::Connecting) {
    // HELLO both ways, then capabilities and the genome code, as the real
    // handshake delivers them.
    helloSent_ = helloReceived_ = true;
    if (myMode_ == FamiliarBattleMode::FriendExchange) {
      state_ = FamiliarBattleState::Exchanged;
      status_ = "Friend added!";
      return;
    }
    state_ = FamiliarBattleState::Battling;
    status_ = "Battle!";
    turnNumber_ = 1;
    addLog("Lv " + String(opponent_.level) + " Familiar appeared!");
    if (negotiatedCapabilities()) addLog("Enhanced link active.");
    if (myGenomeCode_[0]) {
      const PetGenome botGenome = generatePetGenome();
      if (encodePetGenome(botGenome, opponentGenomeCode_, sizeof(opponentGenomeCode_))) {
        opponentGenomeChunks_ = 0x1F;
        addLog("Genome received.");
      }
    }
    return;
  }

  if (state_ == FamiliarBattleState::Battling && myMoveSubmitted_ && !opponentMoveSubmitted_) {
    const uint32_t roll = esp_random() % 100;
    opponentMove_ = roll < 55 ? FamiliarBattleMove::Attack :
                    roll < 80 ? FamiliarBattleMove::Defend :
                                FamiliarBattleMove::Special;
    opponentMoveSubmitted_ = true;
    resolveTurnIfReady();
  }
}

void FamiliarBattleService::submitMove(FamiliarBattleMove move) {
  if (state_ != FamiliarBattleState::Battling || myMoveSubmitted_) return;
  myMove_ = move;
  myMoveSubmitted_ = true;
  if (move == FamiliarBattleMove::Flee) {
    addLog("You fled!");
    concludeBattle(FamiliarBattleOutcome::Fled);
    return;
  }
  botEventAtMs = millis() + kBotThinkMs;
}

void FamiliarBattleService::logAction(bool actorIsMe, const BattleRulesActionResult &action) {
  if (!action.acted) return;
  if (action.missed) {
    addLog(String(actorIsMe ? "You" : "Opp") + ": SPECIAL MISSED");
    return;
  }
  addLog(String(actorIsMe ? "You" : "Opp") + ": " + (action.special ? "SPECIAL" : "ATTACK") +
         " -" + String(action.damage) + " dmg");
}

void FamiliarBattleService::resolveTurnIfReady() {
  if (!(myMoveSubmitted_ && opponentMoveSubmitted_)) return;

  BattleRulesCombatant me;
  me.playerId = myPlayerId_;
  me.attack = myAttack_;
  me.defense = myDefense_;
  me.capabilities = toRulesCapabilities(myCapabilities_);
  me.move = static_cast<BattleRulesMove>(myMove_);

  BattleRulesCombatant opp;
  opp.playerId = opponent_.playerId;
  opp.attack = opponentAttack_;
  opp.defense = opponentDefense_;
  opp.capabilities = toRulesCapabilities(opponent_.capabilities);
  opp.move = static_cast<BattleRulesMove>(opponentMove_);

  const BattleRulesTurnResult result =
      resolveBattleTurn(me, myHp_, opp, opponentHp_, negotiatedCapabilities(), prngState_);
  myHp_ = result.hpA;
  opponentHp_ = result.hpB;
  if (result.aActedFirst) {
    logAction(true, result.a);
    logAction(false, result.b);
  } else {
    logAction(false, result.b);
    logAction(true, result.a);
  }

  ++turnNumber_;
  myMoveSubmitted_ = opponentMoveSubmitted_ = false;
  if (myHp_ == 0 || opponentHp_ == 0) {
    concludeBattle(opponentHp_ == 0 ? FamiliarBattleOutcome::Victory
                                    : FamiliarBattleOutcome::Defeat);
  }
}

void FamiliarBattleService::concludeBattle(FamiliarBattleOutcome outcome) {
  outcome_ = outcome;
  state_ = FamiliarBattleState::Result;
  status_ = "Battle over";
}

void FamiliarBattleService::addLog(const String &line) {
  log_.push_back(line);
  if (log_.size() > 40) log_.erase(log_.begin());
}

// Declared private in the header for the BLE path; unused here.
void FamiliarBattleService::parseAdvertisement(const NimBLEAdvertisedDevice *) {}
void FamiliarBattleService::sendHello() {}
void FamiliarBattleService::sendCapabilities() {}
void FamiliarBattleService::sendGenomeCode() {}
void FamiliarBattleService::sendMove(FamiliarBattleMove) {}
bool FamiliarBattleService::sendRaw(const uint8_t *, size_t) { return false; }
void FamiliarBattleService::handleIncomingMessage(const uint8_t *, size_t) {}
