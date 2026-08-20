#pragma once

#include <cstdint>

// Pure Direct Challenge battle math -- stat formulas, matchup tables, and
// turn resolution -- factored out of FamiliarBattleService so it can be
// unit-tested on the host without pulling in NimBLE/Arduino. Nothing in
// this header or its .cpp touches BLE, radios, or any device API; it is
// mirrored byte-for-byte in share/vpet-battle for the same reason
// familiar_battle_service.h/.cpp are (see that header's top comment).
//
// FamiliarBattleService owns the wire protocol, negotiation, and per-turn
// bookkeeping; it converts its own state into the plain structs below,
// calls resolveBattleTurn(), and turns the result back into HP updates and
// log lines.

// Mirrors FamiliarBattleMove's numeric values exactly (Attack=0, Defend=1,
// Special=2, Flee=3) so callers can `static_cast` between them; each side
// that includes both headers should static_assert the two stay in sync
// rather than trusting the comment alone.
enum class BattleRulesMove : uint8_t {
    Attack = 0,
    Defend = 1,
    Special = 2,
    Flee = 3,
};

// Bit-for-bit identical to FamiliarBattleCapability in
// familiar_battle_service.h. Duplicated (rather than shared) because that
// header pulls in Arduino.h; kept in sync via a static_assert where both
// headers are visible together (familiar_battle_service.cpp).
namespace BattleRulesCapability {
constexpr uint16_t BodyType = 1u << 0;
constexpr uint16_t Element = 1u << 1;
constexpr uint16_t Speed = 1u << 2;
constexpr uint16_t Special = 1u << 3;
constexpr uint16_t MoveMatchups = 1u << 4;
}  // namespace BattleRulesCapability

struct BattleRulesCapabilities {
    uint8_t bodyType = 0;
    uint8_t element = 0;
    uint8_t speed = 0;
    uint8_t special = 0;
};

struct BattleRulesCombatant {
    uint32_t playerId = 0;
    uint8_t attack = 0;
    uint8_t defense = 0;
    BattleRulesCapabilities capabilities;
    BattleRulesMove move = BattleRulesMove::Attack;
};

// What one combatant did during a turn. `acted` is false for Defend/Flee
// (no roll is drawn) and also for whichever combatant never got to act
// because Speed is negotiated and the turn already ended after the other
// combatant's hit -- both are "nothing to log", so callers only need to
// branch on `acted`/`missed`, not know which case it was.
struct BattleRulesActionResult {
    bool acted = false;
    bool missed = false;
    bool special = false;
    int32_t damage = 0;  // valid only when acted && !missed
};

struct BattleRulesTurnResult {
    BattleRulesActionResult a;  // what combatant `a` did
    BattleRulesActionResult b;  // what combatant `b` did
    bool aActedFirst = true;
    uint16_t hpA = 0;
    uint16_t hpB = 0;
};

uint16_t battleRulesDeriveMaxHp(uint8_t level);
uint8_t battleRulesDeriveAttack(uint8_t level, uint8_t stageIndex);
uint8_t battleRulesDeriveDefense(uint8_t level, uint8_t stageIndex);

// xorshift32. Both peers must call this the same number of times in the
// same order to stay in sync -- see resolveBattleTurn's own comment.
uint32_t battleRulesNextRandom(uint32_t &prngState);

// Resolves one full turn between `a` and `b`, given their already-submitted
// moves and the negotiated (intersected) capability mask. Determines
// action order internally (higher Speed first when negotiated, tie or no
// Speed falls back to lower playerId first, matching the protocol's core
// rule), draws from `prngState` in that same order, and stops before the
// second actor's action when Speed is negotiated and the first action
// already brought a combatant to 0 HP. Both peers computing this from
// identical inputs and an identically-seeded prngState is what keeps a
// battle's outcome agreeing on both devices without either trusting the
// other's math.
BattleRulesTurnResult resolveBattleTurn(const BattleRulesCombatant &a, uint16_t hpA,
                                        const BattleRulesCombatant &b, uint16_t hpB,
                                        uint16_t activeCapabilities, uint32_t &prngState);
