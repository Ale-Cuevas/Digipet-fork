// Unit tests for the hardware-independent genome logic in pet_genome.cpp:
// the codec (encode/decode + checksum), blending, and evolution drift.
// Runs on the native platform -- see test/native/fakes for what stands in
// for Arduino.h/esp_system.h, and platformio.ini's [env:native] for how
// this is wired up. Run with `pio test -e native`.

#include <cstring>

#include <unity.h>

#include "esp_system.h"  // esp_random_reseed -- fakes/, deterministic RNG
#include "game/vpet/pet_genome.h"

namespace {

const uint32_t kSeedA[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
const uint32_t kEvoSeedA[4] = {0x55555555, 0x66666666, 0x77777777, 0x88888888};
const uint32_t kSeedB[4] = {0xCAFEBABE, 0xDEADBEEF, 0x0BADF00D, 0x8BADF00D};
const uint32_t kEvoSeedB[4] = {0xFEEDFACE, 0x12345678, 0x9ABCDEF0, 0x01234567};

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// --- derivePetGenome -------------------------------------------------------

void test_derive_is_deterministic(void) {
  PetGenome first = derivePetGenome(kSeedA, kEvoSeedA);
  PetGenome second = derivePetGenome(kSeedA, kEvoSeedA);
  TEST_ASSERT_EQUAL_MEMORY(&first, &second, sizeof(PetGenome));
}

void test_derive_differs_across_seeds(void) {
  PetGenome first = derivePetGenome(kSeedA, kEvoSeedA);
  PetGenome second = derivePetGenome(kSeedB, kEvoSeedB);
  TEST_ASSERT_NOT_EQUAL(0, memcmp(&first, &second, sizeof(PetGenome)));
}

void test_derive_preserves_seed_fields(void) {
  PetGenome genome = derivePetGenome(kSeedA, kEvoSeedA);
  TEST_ASSERT_EQUAL_UINT32_ARRAY(kSeedA, genome.seed, 4);
  TEST_ASSERT_EQUAL_UINT32_ARRAY(kEvoSeedA, genome.evolutionSeed, 4);
}

// --- encodePetGenome / decodePetGenome --------------------------------------

void test_encode_produces_exact_code_length(void) {
  const PetGenome genome = derivePetGenome(kSeedA, kEvoSeedA);
  char code[PET_GENOME_CODE_LENGTH + 1];
  TEST_ASSERT_TRUE(encodePetGenome(genome, code, sizeof(code)));
  TEST_ASSERT_EQUAL_size_t(PET_GENOME_CODE_LENGTH, strlen(code));
}

void test_encode_rejects_undersized_buffer(void) {
  const PetGenome genome = derivePetGenome(kSeedA, kEvoSeedA);
  char tooSmall[PET_GENOME_CODE_LENGTH];  // needs to be strictly larger
  TEST_ASSERT_FALSE(encodePetGenome(genome, tooSmall, sizeof(tooSmall)));
}

void test_decode_round_trips_through_encode(void) {
  const PetGenome original = derivePetGenome(kSeedA, kEvoSeedA);
  char code[PET_GENOME_CODE_LENGTH + 1];
  TEST_ASSERT_TRUE(encodePetGenome(original, code, sizeof(code)));

  PetGenome decoded{};
  TEST_ASSERT_TRUE(decodePetGenome(code, decoded));
  TEST_ASSERT_EQUAL_MEMORY(&original, &decoded, sizeof(PetGenome));
}

void test_decode_ignores_lowercase_dashes_and_whitespace(void) {
  const PetGenome original = derivePetGenome(kSeedB, kEvoSeedB);
  char code[PET_GENOME_CODE_LENGTH + 1];
  TEST_ASSERT_TRUE(encodePetGenome(original, code, sizeof(code)));

  // Rebuild the same code lowercased and broken up with separators, as a
  // human transcribing it by hand might produce.
  char decorated[PET_GENOME_CODE_LENGTH * 2 + 16];
  size_t out = 0;
  for (size_t i = 0; code[i]; ++i) {
    char c = code[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    decorated[out++] = c;
    if ((i + 1) % 5 == 0 && code[i + 1]) decorated[out++] = '-';
  }
  decorated[out] = '\0';

  PetGenome decoded{};
  TEST_ASSERT_TRUE(decodePetGenome(decorated, decoded));
  TEST_ASSERT_EQUAL_MEMORY(&original, &decoded, sizeof(PetGenome));
}

void test_decode_rejects_flipped_character(void) {
  const PetGenome original = derivePetGenome(kSeedA, kEvoSeedA);
  char code[PET_GENOME_CODE_LENGTH + 1];
  TEST_ASSERT_TRUE(encodePetGenome(original, code, sizeof(code)));

  // Flip a symbol well past the version byte (the first ~1.6 symbols) so
  // this exercises the CRC32 checksum specifically, not the version check.
  code[30] = (code[30] == '0') ? '1' : '0';

  PetGenome decoded{};
  TEST_ASSERT_FALSE(decodePetGenome(code, decoded));
}

void test_decode_rejects_wrong_length(void) {
  const PetGenome original = derivePetGenome(kSeedA, kEvoSeedA);
  char code[PET_GENOME_CODE_LENGTH + 1];
  TEST_ASSERT_TRUE(encodePetGenome(original, code, sizeof(code)));
  code[PET_GENOME_CODE_LENGTH - 1] = '\0';  // one symbol short

  PetGenome decoded{};
  TEST_ASSERT_FALSE(decodePetGenome(code, decoded));
}

void test_decode_rejects_invalid_characters(void) {
  // 'U' is deliberately excluded from the Crockford-style alphabet
  // (0123456789ABCDEFGHJKMNPQRSTVWXYZ) and isn't one of the normalized
  // look-alikes ('O'->'0', 'I'/'L'->'1'), so it must be rejected outright.
  char code[PET_GENOME_CODE_LENGTH + 1];
  memset(code, 'U', PET_GENOME_CODE_LENGTH);
  code[PET_GENOME_CODE_LENGTH] = '\0';

  PetGenome decoded{};
  TEST_ASSERT_FALSE(decodePetGenome(code, decoded));
}

void test_decode_rejects_null(void) {
  PetGenome decoded{};
  TEST_ASSERT_FALSE(decodePetGenome(nullptr, decoded));
}

// --- blendPetGenomes ---------------------------------------------------------

void test_blend_is_deterministic_given_same_entropy(void) {
  const PetGenome first = derivePetGenome(kSeedA, kEvoSeedA);
  const PetGenome second = derivePetGenome(kSeedB, kEvoSeedB);

  esp_random_reseed(42);
  const PetGenome blendA = blendPetGenomes(first, second);

  esp_random_reseed(42);
  const PetGenome blendB = blendPetGenomes(first, second);

  TEST_ASSERT_EQUAL_MEMORY(&blendA, &blendB, sizeof(PetGenome));
}

void test_blend_result_encodes_and_decodes_cleanly(void) {
  const PetGenome first = derivePetGenome(kSeedA, kEvoSeedA);
  const PetGenome second = derivePetGenome(kSeedB, kEvoSeedB);
  esp_random_reseed(7);
  const PetGenome blended = blendPetGenomes(first, second);

  char code[PET_GENOME_CODE_LENGTH + 1];
  TEST_ASSERT_TRUE(encodePetGenome(blended, code, sizeof(code)));
  PetGenome decoded{};
  TEST_ASSERT_TRUE(decodePetGenome(code, decoded));
  TEST_ASSERT_EQUAL_MEMORY(&blended, &decoded, sizeof(PetGenome));
}

// --- evolvedGenomeGene / evolvedGenomeFeatures ------------------------------

void test_evolved_gene_stage_zero_is_unchanged(void) {
  const PetGenome genome = derivePetGenome(kSeedA, kEvoSeedA);
  const uint8_t base = 128;
  TEST_ASSERT_EQUAL_UINT8(base, evolvedGenomeGene(genome, base, /*stage=*/0,
                                                   /*channel=*/0, /*maximumDrift=*/64));
}

void test_evolved_gene_stays_in_byte_range(void) {
  const PetGenome genome = derivePetGenome(kSeedA, kEvoSeedA);
  for (uint8_t stage = 0; stage <= 4; ++stage) {
    for (uint8_t channel = 0; channel < 4; ++channel) {
      const uint8_t value = evolvedGenomeGene(genome, 255, stage, channel, 255);
      TEST_ASSERT_LESS_OR_EQUAL_UINT8(255, value);
      const uint8_t lowValue = evolvedGenomeGene(genome, 0, stage, channel, 255);
      TEST_ASSERT_GREATER_OR_EQUAL_UINT8(0, lowValue);
    }
  }
}

void test_evolved_gene_clamps_stage_above_four(void) {
  const PetGenome genome = derivePetGenome(kSeedA, kEvoSeedA);
  const uint8_t atFour = evolvedGenomeGene(genome, 100, 4, 1, 40);
  const uint8_t atTen = evolvedGenomeGene(genome, 100, 10, 1, 40);
  TEST_ASSERT_EQUAL_UINT8(atFour, atTen);
}

void test_evolved_features_stage_zero_is_unchanged(void) {
  const PetGenome genome = derivePetGenome(kSeedA, kEvoSeedA);
  TEST_ASSERT_EQUAL_UINT16(genome.featureGenes, evolvedGenomeFeatures(genome, 0));
}

// --- petGenomeDesignId -------------------------------------------------------

void test_design_id_is_deterministic(void) {
  const PetGenome genome = derivePetGenome(kSeedA, kEvoSeedA);
  TEST_ASSERT_EQUAL_UINT64(petGenomeDesignId(genome), petGenomeDesignId(genome));
}

void test_design_id_changes_with_genome(void) {
  const PetGenome first = derivePetGenome(kSeedA, kEvoSeedA);
  const PetGenome second = derivePetGenome(kSeedB, kEvoSeedB);
  TEST_ASSERT_NOT_EQUAL(petGenomeDesignId(first), petGenomeDesignId(second));
}

// --- naming helpers -----------------------------------------------------------

void test_egg_lineage_name_wraps_at_count(void) {
  TEST_ASSERT_EQUAL_STRING(eggLineageName(0), eggLineageName(10));
}

void test_element_name_wraps_at_six(void) {
  TEST_ASSERT_EQUAL_STRING(elementName(0), elementName(6));
}

int main(int argc, char **argv) {
  UNITY_BEGIN();
  RUN_TEST(test_derive_is_deterministic);
  RUN_TEST(test_derive_differs_across_seeds);
  RUN_TEST(test_derive_preserves_seed_fields);
  RUN_TEST(test_encode_produces_exact_code_length);
  RUN_TEST(test_encode_rejects_undersized_buffer);
  RUN_TEST(test_decode_round_trips_through_encode);
  RUN_TEST(test_decode_ignores_lowercase_dashes_and_whitespace);
  RUN_TEST(test_decode_rejects_flipped_character);
  RUN_TEST(test_decode_rejects_wrong_length);
  RUN_TEST(test_decode_rejects_invalid_characters);
  RUN_TEST(test_decode_rejects_null);
  RUN_TEST(test_blend_is_deterministic_given_same_entropy);
  RUN_TEST(test_blend_result_encodes_and_decodes_cleanly);
  RUN_TEST(test_evolved_gene_stage_zero_is_unchanged);
  RUN_TEST(test_evolved_gene_stays_in_byte_range);
  RUN_TEST(test_evolved_gene_clamps_stage_above_four);
  RUN_TEST(test_evolved_features_stage_zero_is_unchanged);
  RUN_TEST(test_design_id_is_deterministic);
  RUN_TEST(test_design_id_changes_with_genome);
  RUN_TEST(test_egg_lineage_name_wraps_at_count);
  RUN_TEST(test_element_name_wraps_at_six);
  return UNITY_END();
}
