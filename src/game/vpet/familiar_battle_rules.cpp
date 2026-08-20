#include "game/vpet/familiar_battle_rules.h"

#include <algorithm>

namespace {

uint8_t bodyCombatStyle(uint8_t bodyType) {
    // Power, Guard, Tactical. Multiple silhouettes may share a style.
    constexpr uint8_t styles[] = {0, 1, 2, 1, 2};
    return styles[bodyType % 5];
}

int matchupPercent(uint8_t attacker, uint8_t defender) {
    if (attacker == defender) return 100;
    // Power > Tactical > Guard > Power.
    return ((attacker == 0 && defender == 2) ||
            (attacker == 2 && defender == 1) ||
            (attacker == 1 && defender == 0)) ? 115 : 90;
}

int elementPercent(uint8_t attacker, uint8_t defender) {
    if (attacker == defender) return 100;
    // Fire > Nature > Water > Fire; Electric > Digital > Dark > Electric.
    constexpr uint8_t beats[] = {2, 0, 1, 5, 3, 4};
    if (beats[attacker % 6] == defender % 6) return 125;
    if (beats[defender % 6] == attacker % 6) return 80;
    return 100;
}

BattleRulesActionResult resolveAction(const BattleRulesCombatant &attacker,
                                      const BattleRulesCombatant &defender,
                                      uint16_t activeCapabilities,
                                      uint32_t &prngState) {
    BattleRulesActionResult result;
    if (attacker.move != BattleRulesMove::Attack &&
        attacker.move != BattleRulesMove::Special) {
        return result;  // acted stays false: Defend/Flee never draw a roll.
    }
    result.acted = true;
    result.special = attacker.move == BattleRulesMove::Special;

    const uint32_t roll = battleRulesNextRandom(prngState);
    if (result.special && roll % 100 < 15) {
        result.missed = true;
        return result;
    }

    int32_t damage =
        static_cast<int32_t>(attacker.attack) - static_cast<int32_t>(defender.defense) / 2;
    if (result.special) damage = damage * 3 / 2;

    if (activeCapabilities & BattleRulesCapability::MoveMatchups) {
        int movePercent = 100;
        if (attacker.move == BattleRulesMove::Attack &&
            defender.move == BattleRulesMove::Special) movePercent = 125;
        if (attacker.move == BattleRulesMove::Special &&
            defender.move == BattleRulesMove::Defend) movePercent = 125;
        damage = damage * movePercent / 100;
    }
    if (activeCapabilities & BattleRulesCapability::BodyType) {
        damage = damage * matchupPercent(bodyCombatStyle(attacker.capabilities.bodyType),
                                         bodyCombatStyle(defender.capabilities.bodyType)) / 100;
    }
    if (activeCapabilities & BattleRulesCapability::Element) {
        damage = damage *
                 elementPercent(attacker.capabilities.element, defender.capabilities.element) /
                 100;
    }
    if (result.special && (activeCapabilities & BattleRulesCapability::Special)) {
        damage = damage * (100 + std::min<uint8_t>(attacker.capabilities.special, 100) / 4) / 100;
    }

    const int32_t variance = static_cast<int32_t>(roll % 41) - 20;  // +/-20%
    damage += damage * variance / 100;

    const bool defenderDefending = defender.move == BattleRulesMove::Defend;
    const bool specialBreaksGuard =
        (activeCapabilities & BattleRulesCapability::MoveMatchups) && result.special &&
        defenderDefending;
    if (defenderDefending && !specialBreaksGuard) damage /= 2;
    if (damage < 1) damage = 1;

    result.damage = damage;
    return result;
}

}  // namespace

uint16_t battleRulesDeriveMaxHp(uint8_t level) {
    return static_cast<uint16_t>(20 + static_cast<uint16_t>(level) * 3);
}

uint8_t battleRulesDeriveAttack(uint8_t level, uint8_t stageIndex) {
    return static_cast<uint8_t>(
        std::min<uint16_t>(255, 4 + level + static_cast<uint16_t>(stageIndex) * 2));
}

uint8_t battleRulesDeriveDefense(uint8_t level, uint8_t stageIndex) {
    return static_cast<uint8_t>(std::min<uint16_t>(255, 2 + level / 2 + stageIndex));
}

uint32_t battleRulesNextRandom(uint32_t &prngState) {
    uint32_t x = prngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    prngState = x;
    return x;
}

BattleRulesTurnResult resolveBattleTurn(const BattleRulesCombatant &a, uint16_t hpA,
                                        const BattleRulesCombatant &b, uint16_t hpB,
                                        uint16_t activeCapabilities, uint32_t &prngState) {
    BattleRulesTurnResult result;
    result.hpA = hpA;
    result.hpB = hpB;

    // Fixed processing order so both devices draw from the shared PRNG in
    // the same sequence: higher Speed acts first when negotiated (ties,
    // like the no-Speed case, fall back to lower playerId first).
    bool aFirst = true;
    if (activeCapabilities & BattleRulesCapability::Speed) {
        if (a.capabilities.speed < b.capabilities.speed ||
            (a.capabilities.speed == b.capabilities.speed && a.playerId > b.playerId)) {
            aFirst = false;
        }
    } else if (a.playerId > b.playerId) {
        aFirst = false;
    }
    result.aActedFirst = aFirst;

    const bool speedNegotiated = activeCapabilities & BattleRulesCapability::Speed;

    if (aFirst) {
        result.a = resolveAction(a, b, activeCapabilities, prngState);
        if (result.a.acted && !result.a.missed) {
            result.hpB = result.hpB > result.a.damage ? result.hpB - result.a.damage : 0;
        }
        if (!(speedNegotiated && (result.hpA == 0 || result.hpB == 0))) {
            result.b = resolveAction(b, a, activeCapabilities, prngState);
            if (result.b.acted && !result.b.missed) {
                result.hpA = result.hpA > result.b.damage ? result.hpA - result.b.damage : 0;
            }
        }
    } else {
        result.b = resolveAction(b, a, activeCapabilities, prngState);
        if (result.b.acted && !result.b.missed) {
            result.hpA = result.hpA > result.b.damage ? result.hpA - result.b.damage : 0;
        }
        if (!(speedNegotiated && (result.hpA == 0 || result.hpB == 0))) {
            result.a = resolveAction(a, b, activeCapabilities, prngState);
            if (result.a.acted && !result.a.missed) {
                result.hpB = result.hpB > result.a.damage ? result.hpB - result.a.damage : 0;
            }
        }
    }

    return result;
}
