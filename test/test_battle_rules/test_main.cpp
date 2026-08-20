// Unit tests for the pure Direct Challenge battle math extracted into
// familiar_battle_rules.h/.cpp: stat formulas, the xorshift32 step, and
// resolveBattleTurn's damage/ordering rules (core and negotiated). Runs on
// the native platform -- see platformio.ini's [env:native]. Run with
// `pio test -e native`.
//
// Where a test needs a specific PRNG roll (to force a Special miss, or to
// zero out the +/-20% variance so a damage value is exact), it searches for
// a seed that produces one via battleRulesNextRandom itself rather than
// hardcoding a magic seed -- self-documenting, and immune to the PRNG
// implementation changing shape (as long as it stays a PRNG).

#include <unity.h>

#include "game/vpet/familiar_battle_rules.h"

namespace {

template <typename Predicate>
uint32_t findSeedWhere(Predicate predicate) {
    for (uint32_t seed = 1; seed < 200000; ++seed) {
        uint32_t state = seed;
        const uint32_t roll = battleRulesNextRandom(state);
        if (predicate(roll)) return seed;
    }
    TEST_FAIL_MESSAGE("no PRNG seed found matching the requested roll predicate");
    return 0;
}

// roll % 41 == 20 makes the formula's +/-20% variance term zero, so the
// resulting damage is the pre-variance value exactly -- used whenever a
// test wants to check a multiplier in isolation.
uint32_t zeroVarianceSeed() {
    return findSeedWhere([](uint32_t roll) { return roll % 41 == 20 && roll % 100 >= 15; });
}

BattleRulesCombatant makeCombatant(uint32_t playerId, uint8_t attack, uint8_t defense,
                                   BattleRulesMove move) {
    BattleRulesCombatant combatant;
    combatant.playerId = playerId;
    combatant.attack = attack;
    combatant.defense = defense;
    combatant.move = move;
    return combatant;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// --- stat formulas -----------------------------------------------------------

void test_derive_max_hp(void) {
    TEST_ASSERT_EQUAL_UINT16(20, battleRulesDeriveMaxHp(0));
    TEST_ASSERT_EQUAL_UINT16(50, battleRulesDeriveMaxHp(10));
}

void test_derive_attack_and_defense_spot_values(void) {
    TEST_ASSERT_EQUAL_UINT8(4, battleRulesDeriveAttack(0, 0));
    TEST_ASSERT_EQUAL_UINT8(2, battleRulesDeriveDefense(0, 0));
}

void test_derive_attack_clamps_at_255(void) {
    // 4 + 255 + 4*2 = 267, must clamp rather than wrap a uint8_t.
    TEST_ASSERT_EQUAL_UINT8(255, battleRulesDeriveAttack(255, 4));
}

void test_derive_defense_clamps_at_255(void) {
    // 2 + 255/2 + 255 = 384, must clamp rather than wrap.
    TEST_ASSERT_EQUAL_UINT8(255, battleRulesDeriveDefense(255, 255));
}

// --- battleRulesNextRandom ----------------------------------------------------

void test_next_random_is_deterministic(void) {
    uint32_t stateA = 12345;
    uint32_t stateB = 12345;
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT_EQUAL_UINT32(battleRulesNextRandom(stateA), battleRulesNextRandom(stateB));
    }
}

void test_next_random_advances_state(void) {
    uint32_t state = 777;
    const uint32_t before = state;
    battleRulesNextRandom(state);
    TEST_ASSERT_NOT_EQUAL(before, state);
}

// --- ordering ------------------------------------------------------------------

void test_core_rules_lower_player_id_acts_first(void) {
    BattleRulesCombatant a = makeCombatant(1, 50, 0, BattleRulesMove::Defend);
    BattleRulesCombatant b = makeCombatant(2, 50, 0, BattleRulesMove::Defend);
    uint32_t prng = 1;
    const BattleRulesTurnResult result = resolveBattleTurn(a, 100, b, 100, 0, prng);
    TEST_ASSERT_TRUE(result.aActedFirst);
}

void test_core_rules_higher_player_id_goes_second(void) {
    BattleRulesCombatant a = makeCombatant(5, 50, 0, BattleRulesMove::Defend);
    BattleRulesCombatant b = makeCombatant(2, 50, 0, BattleRulesMove::Defend);
    uint32_t prng = 1;
    const BattleRulesTurnResult result = resolveBattleTurn(a, 100, b, 100, 0, prng);
    TEST_ASSERT_FALSE(result.aActedFirst);
}

void test_speed_negotiated_overrides_player_id(void) {
    BattleRulesCombatant a = makeCombatant(1, 50, 0, BattleRulesMove::Defend);
    a.capabilities.speed = 10;
    BattleRulesCombatant b = makeCombatant(99, 50, 0, BattleRulesMove::Defend);
    b.capabilities.speed = 90;
    uint32_t prng = 1;
    const BattleRulesTurnResult result =
        resolveBattleTurn(a, 100, b, 100, BattleRulesCapability::Speed, prng);
    TEST_ASSERT_FALSE(result.aActedFirst);  // b is faster despite the higher playerId
}

void test_speed_tie_falls_back_to_lower_player_id(void) {
    BattleRulesCombatant a = makeCombatant(5, 50, 0, BattleRulesMove::Defend);
    a.capabilities.speed = 50;
    BattleRulesCombatant b = makeCombatant(2, 50, 0, BattleRulesMove::Defend);
    b.capabilities.speed = 50;
    uint32_t prng = 1;
    const BattleRulesTurnResult result =
        resolveBattleTurn(a, 100, b, 100, BattleRulesCapability::Speed, prng);
    TEST_ASSERT_FALSE(result.aActedFirst);  // tie -> b (lower playerId) first
}

// --- Defend / Flee draw no roll -----------------------------------------------

void test_defend_and_flee_consume_no_random_draw(void) {
    BattleRulesCombatant a = makeCombatant(1, 50, 10, BattleRulesMove::Defend);
    BattleRulesCombatant b = makeCombatant(2, 50, 10, BattleRulesMove::Flee);
    uint32_t prng = 424242;
    const uint32_t before = prng;
    const BattleRulesTurnResult result = resolveBattleTurn(a, 100, b, 100, 0, prng);
    TEST_ASSERT_EQUAL_UINT32(before, prng);
    TEST_ASSERT_FALSE(result.a.acted);
    TEST_ASSERT_FALSE(result.b.acted);
    TEST_ASSERT_EQUAL_UINT16(100, result.hpA);
    TEST_ASSERT_EQUAL_UINT16(100, result.hpB);
}

// --- damage/HP floors ------------------------------------------------------------

void test_damage_never_drops_below_one(void) {
    BattleRulesCombatant a = makeCombatant(1, 0, 250, BattleRulesMove::Attack);
    BattleRulesCombatant b = makeCombatant(2, 0, 0, BattleRulesMove::Attack);
    uint32_t prng = 1;
    const BattleRulesTurnResult result = resolveBattleTurn(a, 100, b, 100, 0, prng);
    TEST_ASSERT_TRUE(result.a.acted);
    TEST_ASSERT_FALSE(result.a.missed);
    TEST_ASSERT_GREATER_OR_EQUAL_INT32(1, result.a.damage);
}

void test_hp_floors_at_zero_instead_of_underflowing(void) {
    BattleRulesCombatant a = makeCombatant(1, 100, 0, BattleRulesMove::Attack);
    BattleRulesCombatant b = makeCombatant(2, 0, 0, BattleRulesMove::Attack);
    uint32_t prng = 1;
    // b has 1 HP; a's hit is guaranteed to be well over that.
    const BattleRulesTurnResult result = resolveBattleTurn(a, 100, b, 1, 0, prng);
    TEST_ASSERT_EQUAL_UINT16(0, result.hpB);
}

// --- core damage formula, isolated from variance --------------------------------

void test_core_damage_formula_with_zero_variance(void) {
    const uint32_t seed = zeroVarianceSeed();
    BattleRulesCombatant a = makeCombatant(1, 100, 0, BattleRulesMove::Attack);
    BattleRulesCombatant b = makeCombatant(2, 100, 0, BattleRulesMove::Attack);
    uint32_t prng = seed;
    const BattleRulesTurnResult result = resolveBattleTurn(a, 100, b, 100, 0, prng);
    // atk(100) - def(0)/2 = 100, no matchups negotiated, zero variance.
    TEST_ASSERT_EQUAL_INT32(100, result.a.damage);
}

void test_defending_halves_damage(void) {
    const uint32_t seed = zeroVarianceSeed();
    BattleRulesCombatant a = makeCombatant(1, 100, 0, BattleRulesMove::Attack);
    BattleRulesCombatant b = makeCombatant(2, 100, 0, BattleRulesMove::Defend);
    uint32_t prng = seed;
    const BattleRulesTurnResult result = resolveBattleTurn(a, 100, b, 100, 0, prng);
    // Base 100, halved for Defend, no MoveMatchups negotiated to break the guard.
    TEST_ASSERT_EQUAL_INT32(50, result.a.damage);
}

// --- Special: miss chance and the base 1.5x multiplier --------------------------

void test_special_can_miss(void) {
    const uint32_t seed = findSeedWhere([](uint32_t roll) { return roll % 100 < 15; });
    BattleRulesCombatant a = makeCombatant(1, 100, 0, BattleRulesMove::Special);
    BattleRulesCombatant b = makeCombatant(2, 100, 0, BattleRulesMove::Attack);
    uint32_t prng = seed;
    const BattleRulesTurnResult result = resolveBattleTurn(a, 100, b, 100, 0, prng);
    TEST_ASSERT_TRUE(result.a.acted);
    TEST_ASSERT_TRUE(result.a.missed);
}

void test_special_hits_for_one_and_a_half_times_base(void) {
    const uint32_t seed =
        findSeedWhere([](uint32_t roll) { return roll % 100 >= 15 && roll % 41 == 20; });
    BattleRulesCombatant a = makeCombatant(1, 100, 0, BattleRulesMove::Special);
    BattleRulesCombatant b = makeCombatant(2, 100, 0, BattleRulesMove::Attack);
    uint32_t prng = seed;
    const BattleRulesTurnResult result = resolveBattleTurn(a, 100, b, 100, 0, prng);
    TEST_ASSERT_TRUE(result.a.acted);
    TEST_ASSERT_FALSE(result.a.missed);
    TEST_ASSERT_EQUAL_INT32(150, result.a.damage);  // 100 base * 1.5
}

// --- Move matchups (negotiated) --------------------------------------------------

void test_move_matchup_attack_beats_special(void) {
    const uint32_t seed = zeroVarianceSeed();
    BattleRulesCombatant a = makeCombatant(1, 100, 0, BattleRulesMove::Attack);
    BattleRulesCombatant b = makeCombatant(2, 100, 0, BattleRulesMove::Special);
    uint32_t prng = seed;
    const BattleRulesTurnResult result =
        resolveBattleTurn(a, 100, b, 100, BattleRulesCapability::MoveMatchups, prng);
    TEST_ASSERT_EQUAL_INT32(125, result.a.damage);  // 100 base * 125%
}

void test_move_matchup_special_breaks_guard(void) {
    const uint32_t seed =
        findSeedWhere([](uint32_t roll) { return roll % 100 >= 15 && roll % 41 == 20; });
    BattleRulesCombatant a = makeCombatant(1, 100, 0, BattleRulesMove::Special);
    BattleRulesCombatant b = makeCombatant(2, 100, 0, BattleRulesMove::Defend);
    uint32_t prng = seed;
    const BattleRulesTurnResult result =
        resolveBattleTurn(a, 100, b, 100, BattleRulesCapability::MoveMatchups, prng);
    // 100 base * 1.5 (special) * 125% (special-vs-defend) = 187 (truncating), and NOT
    // halved for Defend because Special is documented to break Guard.
    TEST_ASSERT_EQUAL_INT32(187, result.a.damage);
}

void test_special_vs_defend_is_still_halved_without_move_matchups(void) {
    const uint32_t seed =
        findSeedWhere([](uint32_t roll) { return roll % 100 >= 15 && roll % 41 == 20; });
    BattleRulesCombatant a = makeCombatant(1, 100, 0, BattleRulesMove::Special);
    BattleRulesCombatant b = makeCombatant(2, 100, 0, BattleRulesMove::Defend);
    uint32_t prng = seed;
    const BattleRulesTurnResult result = resolveBattleTurn(a, 100, b, 100, 0, prng);
    // 100 base * 1.5 (special) = 150, halved for Defend since MoveMatchups isn't
    // negotiated so nothing breaks the guard.
    TEST_ASSERT_EQUAL_INT32(75, result.a.damage);
}

// --- Body type matchups (negotiated) ---------------------------------------------

void test_body_type_advantage(void) {
    const uint32_t seed = zeroVarianceSeed();
    // Quadruped (Power) beats Avian (Tactical).
    BattleRulesCombatant a = makeCombatant(1, 100, 0, BattleRulesMove::Attack);
    a.capabilities.bodyType = 0;
    BattleRulesCombatant b = makeCombatant(2, 100, 0, BattleRulesMove::Attack);
    b.capabilities.bodyType = 2;
    uint32_t prng = seed;
    const BattleRulesTurnResult result =
        resolveBattleTurn(a, 100, b, 100, BattleRulesCapability::BodyType, prng);
    TEST_ASSERT_EQUAL_INT32(115, result.a.damage);
}

void test_body_type_disadvantage(void) {
    const uint32_t seed = zeroVarianceSeed();
    // Quadruped (Power) loses to Humanoid (Guard) -- Guard beats Power.
    BattleRulesCombatant a = makeCombatant(1, 100, 0, BattleRulesMove::Attack);
    a.capabilities.bodyType = 0;
    BattleRulesCombatant b = makeCombatant(2, 100, 0, BattleRulesMove::Attack);
    b.capabilities.bodyType = 1;
    uint32_t prng = seed;
    const BattleRulesTurnResult result =
        resolveBattleTurn(a, 100, b, 100, BattleRulesCapability::BodyType, prng);
    TEST_ASSERT_EQUAL_INT32(90, result.a.damage);
}

void test_body_type_same_style_is_neutral(void) {
    const uint32_t seed = zeroVarianceSeed();
    BattleRulesCombatant a = makeCombatant(1, 100, 0, BattleRulesMove::Attack);
    a.capabilities.bodyType = 0;  // Quadruped
    BattleRulesCombatant b = makeCombatant(2, 100, 0, BattleRulesMove::Attack);
    b.capabilities.bodyType = 0;  // also Quadruped -> same style
    uint32_t prng = seed;
    const BattleRulesTurnResult result =
        resolveBattleTurn(a, 100, b, 100, BattleRulesCapability::BodyType, prng);
    TEST_ASSERT_EQUAL_INT32(100, result.a.damage);
}

// --- Element matchups (negotiated) ------------------------------------------------

void test_element_advantage(void) {
    const uint32_t seed = zeroVarianceSeed();
    BattleRulesCombatant a = makeCombatant(1, 100, 0, BattleRulesMove::Attack);
    a.capabilities.element = 0;  // Fire
    BattleRulesCombatant b = makeCombatant(2, 100, 0, BattleRulesMove::Attack);
    b.capabilities.element = 2;  // Nature -- Fire beats Nature
    uint32_t prng = seed;
    const BattleRulesTurnResult result =
        resolveBattleTurn(a, 100, b, 100, BattleRulesCapability::Element, prng);
    TEST_ASSERT_EQUAL_INT32(125, result.a.damage);
}

void test_element_disadvantage(void) {
    const uint32_t seed = zeroVarianceSeed();
    BattleRulesCombatant a = makeCombatant(1, 100, 0, BattleRulesMove::Attack);
    a.capabilities.element = 2;  // Nature, attacking...
    BattleRulesCombatant b = makeCombatant(2, 100, 0, BattleRulesMove::Attack);
    b.capabilities.element = 0;  // ...Fire, which beats Nature
    uint32_t prng = seed;
    const BattleRulesTurnResult result =
        resolveBattleTurn(a, 100, b, 100, BattleRulesCapability::Element, prng);
    TEST_ASSERT_EQUAL_INT32(80, result.a.damage);
}

void test_element_cross_triangle_is_neutral(void) {
    const uint32_t seed = zeroVarianceSeed();
    BattleRulesCombatant a = makeCombatant(1, 100, 0, BattleRulesMove::Attack);
    a.capabilities.element = 0;  // Fire triangle
    BattleRulesCombatant b = makeCombatant(2, 100, 0, BattleRulesMove::Attack);
    b.capabilities.element = 3;  // Electric triangle
    uint32_t prng = seed;
    const BattleRulesTurnResult result =
        resolveBattleTurn(a, 100, b, 100, BattleRulesCapability::Element, prng);
    TEST_ASSERT_EQUAL_INT32(100, result.a.damage);
}

// --- Special stat scaling (negotiated) --------------------------------------------

void test_special_capability_scales_damage(void) {
    const uint32_t seed =
        findSeedWhere([](uint32_t roll) { return roll % 100 >= 15 && roll % 41 == 20; });
    BattleRulesCombatant a = makeCombatant(1, 100, 0, BattleRulesMove::Special);
    a.capabilities.special = 100;
    BattleRulesCombatant b = makeCombatant(2, 100, 0, BattleRulesMove::Attack);
    uint32_t prng = seed;
    const BattleRulesTurnResult result =
        resolveBattleTurn(a, 100, b, 100, BattleRulesCapability::Special, prng);
    // 100 base * 1.5 (special) * 125% (100/100 -> +25% max scaling) = 187 (truncating).
    TEST_ASSERT_EQUAL_INT32(187, result.a.damage);
}

void test_special_capability_value_is_clamped_to_100(void) {
    const uint32_t seed =
        findSeedWhere([](uint32_t roll) { return roll % 100 >= 15 && roll % 41 == 20; });
    BattleRulesCombatant withMax = makeCombatant(1, 100, 0, BattleRulesMove::Special);
    withMax.capabilities.special = 100;
    BattleRulesCombatant withOverMax = makeCombatant(1, 100, 0, BattleRulesMove::Special);
    withOverMax.capabilities.special = 200;  // out of the documented 1-100 range
    BattleRulesCombatant opponent = makeCombatant(2, 100, 0, BattleRulesMove::Attack);

    uint32_t prngA = seed;
    const BattleRulesTurnResult resultMax =
        resolveBattleTurn(withMax, 100, opponent, 100, BattleRulesCapability::Special, prngA);
    uint32_t prngB = seed;
    const BattleRulesTurnResult resultOverMax = resolveBattleTurn(
        withOverMax, 100, opponent, 100, BattleRulesCapability::Special, prngB);

    TEST_ASSERT_EQUAL_INT32(resultMax.a.damage, resultOverMax.a.damage);
}

// --- Speed's early stop ------------------------------------------------------------

void test_speed_negotiated_skips_second_actor_after_a_knockout(void) {
    BattleRulesCombatant a = makeCombatant(1, 0, 0, BattleRulesMove::Attack);
    a.capabilities.speed = 10;
    BattleRulesCombatant b = makeCombatant(2, 100, 0, BattleRulesMove::Attack);
    b.capabilities.speed = 90;  // b acts first and will knock a out
    uint32_t prng = 1;
    const BattleRulesTurnResult result =
        resolveBattleTurn(a, 1, b, 100, BattleRulesCapability::Speed, prng);
    TEST_ASSERT_EQUAL_UINT16(0, result.hpA);
    TEST_ASSERT_FALSE(result.aActedFirst);
    TEST_ASSERT_TRUE(result.b.acted);
    TEST_ASSERT_FALSE(result.a.acted);  // pre-empted -- never got a turn
}

void test_core_rules_both_act_even_after_a_knockout(void) {
    // Without Speed negotiated, the original protocol always lets both
    // combatants act so both peers consume the same number of PRNG draws
    // regardless of how the fight already looks -- this looks surprising
    // next to the Speed behavior above, so it's worth pinning down.
    BattleRulesCombatant a = makeCombatant(1, 0, 0, BattleRulesMove::Attack);
    BattleRulesCombatant b = makeCombatant(2, 100, 0, BattleRulesMove::Attack);
    uint32_t prng = 1;
    const BattleRulesTurnResult result = resolveBattleTurn(a, 1, b, 100, 0, prng);
    TEST_ASSERT_EQUAL_UINT16(0, result.hpA);
    TEST_ASSERT_TRUE(result.a.acted);
    TEST_ASSERT_TRUE(result.b.acted);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_derive_max_hp);
    RUN_TEST(test_derive_attack_and_defense_spot_values);
    RUN_TEST(test_derive_attack_clamps_at_255);
    RUN_TEST(test_derive_defense_clamps_at_255);
    RUN_TEST(test_next_random_is_deterministic);
    RUN_TEST(test_next_random_advances_state);
    RUN_TEST(test_core_rules_lower_player_id_acts_first);
    RUN_TEST(test_core_rules_higher_player_id_goes_second);
    RUN_TEST(test_speed_negotiated_overrides_player_id);
    RUN_TEST(test_speed_tie_falls_back_to_lower_player_id);
    RUN_TEST(test_defend_and_flee_consume_no_random_draw);
    RUN_TEST(test_damage_never_drops_below_one);
    RUN_TEST(test_hp_floors_at_zero_instead_of_underflowing);
    RUN_TEST(test_core_damage_formula_with_zero_variance);
    RUN_TEST(test_defending_halves_damage);
    RUN_TEST(test_special_can_miss);
    RUN_TEST(test_special_hits_for_one_and_a_half_times_base);
    RUN_TEST(test_move_matchup_attack_beats_special);
    RUN_TEST(test_move_matchup_special_breaks_guard);
    RUN_TEST(test_special_vs_defend_is_still_halved_without_move_matchups);
    RUN_TEST(test_body_type_advantage);
    RUN_TEST(test_body_type_disadvantage);
    RUN_TEST(test_body_type_same_style_is_neutral);
    RUN_TEST(test_element_advantage);
    RUN_TEST(test_element_disadvantage);
    RUN_TEST(test_element_cross_triangle_is_neutral);
    RUN_TEST(test_special_capability_scales_damage);
    RUN_TEST(test_special_capability_value_is_clamped_to_100);
    RUN_TEST(test_speed_negotiated_skips_second_actor_after_a_knockout);
    RUN_TEST(test_core_rules_both_act_even_after_a_knockout);
    return UNITY_END();
}
