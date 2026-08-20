#include "game/vpet/pet_genome.h"

#include <cstdio>
#include <esp_system.h>

namespace {
uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
  return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3);
}

uint8_t clampChannel(int value) {
  return static_cast<uint8_t>(constrain(value, 0, 255));
}

uint16_t tint(uint8_t red, uint8_t green, uint8_t blue, int amount) {
  return rgb565(clampChannel(red + amount), clampChannel(green + amount),
                clampChannel(blue + amount));
}

struct RgbPalette {
  uint8_t red, green, blue;
  uint8_t secondaryRed, secondaryGreen, secondaryBlue;
  uint8_t accentRed, accentGreen, accentBlue;
};

constexpr RgbPalette kLineagePalettes[] = {
    {205, 55, 32, 74, 18, 25, 255, 174, 42},   // Ember
    {28, 132, 210, 18, 52, 115, 101, 244, 255},// Tidal
    {43, 153, 83, 18, 74, 48, 170, 246, 83},   // Verdant
    {230, 194, 28, 43, 47, 69, 108, 244, 255}, // Volt
    {92, 45, 150, 31, 16, 55, 235, 80, 210},   // Umbral
    {31, 190, 177, 28, 54, 80, 226, 89, 255},  // Digital
    {108, 101, 220, 43, 37, 98, 225, 222, 255},// Crystal
    {102, 122, 139, 39, 49, 62, 246, 139, 48}, // Alloy
    {35, 61, 151, 18, 25, 67, 245, 211, 107},  // Celestial
    {143, 91, 53, 66, 47, 35, 242, 181, 94},   // Primal
};

uint32_t nextGene(uint32_t &state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

uint32_t crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
}

void writeWord(uint8_t *output, uint32_t value) {
  output[0] = value >> 24;
  output[1] = value >> 16;
  output[2] = value >> 8;
  output[3] = value;
}

uint32_t readWord(const uint8_t *input) {
  return (static_cast<uint32_t>(input[0]) << 24) |
         (static_cast<uint32_t>(input[1]) << 16) |
         (static_cast<uint32_t>(input[2]) << 8) | input[3];
}

int8_t base32Value(char character) {
  if (character >= 'a' && character <= 'z') character -= 32;
  if (character == 'O') character = '0';
  if (character == 'I' || character == 'L') character = '1';
  constexpr char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
  const char *found = strchr(alphabet, character);
  return found ? found - alphabet : -1;
}
}  // namespace

PetGenome derivePetGenome(const uint32_t seed[4], const uint32_t evolutionSeed[4]) {
  PetGenome genome{};
  for (uint8_t i = 0; i < 4; ++i) {
    genome.seed[i] = seed[i];
    genome.evolutionSeed[i] = evolutionSeed[i];
  }
  uint32_t state = genome.seed[0] ^ genome.seed[1] ^ genome.seed[2] ^ genome.seed[3];
  if (!state) state = 0xA53C9E17;
  genome.lineage = nextGene(state) % static_cast<uint8_t>(EggLineage::Count);
  // Lineages suggest an element without completely dictating later forms.
  constexpr uint8_t preferredElements[] = {0, 1, 2, 3, 4, 5, 5, 3, 4, 2};
  genome.element = (nextGene(state) % 5 == 0)
      ? nextGene(state) % 6 : preferredElements[genome.lineage];
  genome.bodyType = nextGene(state) % 5;
  genome.widthGene = 72 + nextGene(state) % 113;
  genome.heightGene = 72 + nextGene(state) % 113;
  genome.headGene = 72 + nextGene(state) % 113;
  genome.limbGene = 72 + nextGene(state) % 113;
  genome.featureGenes = nextGene(state) & 0x0FFF;
  genome.faceGene = nextGene(state) & 0x1F;
  genome.markingGene = nextGene(state) & 0x1F;
  genome.paletteGene = nextGene(state) & 0x3F;
  genome.temperament = nextGene(state) % 6;
  // Roughly one pet in sixteen begins with a visible rare mutation.
  genome.mutationGenes = (nextGene(state) & 0x0F) == 0
      ? 1u << (nextGene(state) % 5) : 0;
  return genome;
}

PetGenome generatePetGenome() {
  uint32_t seed[4];
  uint32_t evolutionSeed[4];
  for (uint8_t i = 0; i < 4; ++i) {
    seed[i] = esp_random();
    evolutionSeed[i] = esp_random();
  }
  return derivePetGenome(seed, evolutionSeed);
}

PetGenome blendPetGenomes(const PetGenome &first, const PetGenome &second) {
  uint32_t seed[4];
  uint32_t evolutionSeed[4];
  for (uint8_t i = 0; i < 4; ++i) {
    const uint32_t mask = esp_random();
    seed[i] = (first.seed[i] & mask) | (second.seed[i] & ~mask);
    evolutionSeed[i] = first.evolutionSeed[i] ^ second.evolutionSeed[(i + 1) & 3] ^
                       esp_random();
  }
  return derivePetGenome(seed, evolutionSeed);
}

bool encodePetGenome(const PetGenome &genome, char *output, size_t outputSize) {
  if (!output || outputSize <= PET_GENOME_CODE_LENGTH) return false;
  uint8_t payload[37]{};
  payload[0] = 1;
  for (uint8_t i = 0; i < 4; ++i) writeWord(payload + 1 + i * 4, genome.seed[i]);
  for (uint8_t i = 0; i < 4; ++i)
    writeWord(payload + 17 + i * 4, genome.evolutionSeed[i]);
  writeWord(payload + 33, crc32(payload, 33));

  constexpr char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
  uint32_t buffer = 0;
  uint8_t bits = 0;
  size_t written = 0;
  for (uint8_t byte : payload) {
    buffer = (buffer << 8) | byte;
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      output[written++] = alphabet[(buffer >> bits) & 0x1F];
    }
  }
  if (bits) output[written++] = alphabet[(buffer << (5 - bits)) & 0x1F];
  output[written] = '\0';
  return written == PET_GENOME_CODE_LENGTH;
}

bool decodePetGenome(const char *code, PetGenome &genome) {
  if (!code) return false;
  uint8_t payload[37]{};
  uint32_t buffer = 0;
  uint8_t bits = 0;
  size_t written = 0;
  size_t symbols = 0;
  for (const char *cursor = code; *cursor; ++cursor) {
    if (*cursor == '-' || *cursor == ' ' || *cursor == '\r' || *cursor == '\n') continue;
    const int8_t value = base32Value(*cursor);
    if (value < 0 || symbols++ >= PET_GENOME_CODE_LENGTH) return false;
    buffer = (buffer << 5) | value;
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      if (written >= sizeof(payload)) return false;
      payload[written++] = buffer >> bits;
    }
  }
  if (symbols != PET_GENOME_CODE_LENGTH || written != sizeof(payload) || payload[0] != 1)
    return false;
  if (readWord(payload + 33) != crc32(payload, 33)) return false;
  uint32_t seed[4];
  uint32_t evolutionSeed[4];
  for (uint8_t i = 0; i < 4; ++i) seed[i] = readWord(payload + 1 + i * 4);
  for (uint8_t i = 0; i < 4; ++i) evolutionSeed[i] = readWord(payload + 17 + i * 4);
  genome = derivePetGenome(seed, evolutionSeed);
  return true;
}

uint64_t petGenomeDesignId(const PetGenome &genome) {
  // FNV-1a provides a compact, deterministic identity for the complete saved
  // blueprint. The genome, rather than this identifier, remains authoritative
  // for reconstructing every procedural feature.
  constexpr uint64_t offset = 14695981039346656037ULL;
  constexpr uint64_t prime = 1099511628211ULL;
  uint64_t hash = offset;
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&genome);
  for (size_t i = 0; i < sizeof(genome); ++i) {
    hash ^= bytes[i];
    hash *= prime;
  }
  return hash;
}

uint8_t evolvedGenomeGene(const PetGenome &genome, uint8_t base, uint8_t stage,
                          uint8_t channel, uint8_t maximumDrift) {
  stage = min<uint8_t>(stage, 4);
  uint32_t state = genome.evolutionSeed[channel & 3] ^
                   (0x9E3779B9u * (channel + 1));
  int value = base;
  for (uint8_t form = 1; form <= stage; ++form) {
    const int span = maximumDrift * form / 4;
    const int delta = static_cast<int>(nextGene(state) % (span * 2 + 1)) - span;
    value = constrain(static_cast<int>(base) + delta, 0, 255);
  }
  return value;
}

uint16_t evolvedGenomeFeatures(const PetGenome &genome, uint8_t stage) {
  uint16_t features = genome.featureGenes;
  stage = min<uint8_t>(stage, 4);
  for (uint8_t form = 1; form <= stage; ++form) {
    uint32_t state = genome.evolutionSeed[(form - 1) & 3] ^ (0xA511E9B3u * form);
    const uint8_t bit = nextGene(state) % 12;
    // Most stages reveal a recessive feature; rarer branches suppress one.
    if ((nextGene(state) & 3) == 0) features &= ~(1u << bit);
    else features |= 1u << bit;
  }
  return features;
}

PetPalette paletteForGenome(const PetGenome &genome) {
  const RgbPalette &base = kLineagePalettes[
      genome.lineage % static_cast<uint8_t>(EggLineage::Count)];
  const int variation = static_cast<int>(genome.paletteGene % 17) - 8;
  return {
      tint(base.red, base.green, base.blue, -72 + variation),
      tint(base.red, base.green, base.blue, variation),
      tint(base.red, base.green, base.blue, 58 + variation),
      tint(base.secondaryRed, base.secondaryGreen, base.secondaryBlue, variation),
      tint(base.accentRed, base.accentGreen, base.accentBlue, variation),
      tint(base.accentRed, base.accentGreen, base.accentBlue, 35 + variation),
  };
}

const char *eggLineageName(uint8_t lineage) {
  static const char *names[] = {"EMBER EGG", "TIDAL EGG", "VERDANT EGG",
      "VOLT EGG", "UMBRAL EGG", "DIGITAL EGG", "CRYSTAL EGG",
      "ALLOY EGG", "CELESTIAL EGG", "PRIMAL EGG"};
  return names[lineage % static_cast<uint8_t>(EggLineage::Count)];
}

const char *elementName(uint8_t element) {
  static const char *names[] = {"FIRE", "WATER", "NATURE", "ELECTRIC", "DARK", "DIGITAL"};
  return names[element % 6];
}

void petDisplayName(const PetGenome &genome, char *output, size_t outputSize) {
  static const char *firstWords[] = {"NOVA", "ECHO", "ONYX", "VOLT", "EMBER", "CIPHER",
      "QUARTZ", "FLUX", "RUST", "IRIS", "ZERO", "GLITCH", "PIXEL", "COBALT", "VESPER",
      "NEON", "CRYPT", "HALO", "RIFT", "AMBER"};
  static const char *secondWords[] = {"BYTE", "SPARK", "WISP", "CORE", "DRIFT", "PULSE",
      "SHARD", "GLOW", "NODE", "FLARE", "LOOP", "VEIL", "FANG", "WING", "RUNE", "TIDE"};
  // Drawn from the seed bytes that already deterministically drive the rest
  // of the genome (derivePetGenome()), not any of the visible trait genes
  // -- so the name doesn't read as secretly just restating the element or
  // temperament under a different label. Different byte positions within
  // seed[0]/seed[1] than derivePetGenome() itself leans on most heavily,
  // for a name that varies independently of the genome's other early
  // derivations even when two seeds are close.
  const uint8_t firstIndex = static_cast<uint8_t>(genome.seed[0] >> 8);
  const uint8_t secondIndex = static_cast<uint8_t>(genome.seed[1] >> 16);
  snprintf(output, outputSize, "%s-%s",
           firstWords[firstIndex % (sizeof(firstWords) / sizeof(firstWords[0]))],
           secondWords[secondIndex % (sizeof(secondWords) / sizeof(secondWords[0]))]);
}
