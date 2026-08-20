#pragma once

#include <Arduino.h>

enum class EggLineage : uint8_t {
  Ember,
  Tidal,
  Verdant,
  Volt,
  Umbral,
  Digital,
  Crystal,
  Alloy,
  Celestial,
  Primal,
  Count,
};

enum class PetBodyType : uint8_t { Quadruped, Humanoid, Avian, Blob, Serpent };
enum class PetElement : uint8_t { Fire, Water, Nature, Electric, Dark, Digital };

struct PetGenome {
  uint32_t seed[4];
  uint32_t evolutionSeed[4];
  uint16_t featureGenes;
  uint8_t lineage;
  uint8_t element;
  uint8_t bodyType;
  uint8_t widthGene;
  uint8_t heightGene;
  uint8_t headGene;
  uint8_t limbGene;
  uint8_t faceGene;
  uint8_t markingGene;
  uint8_t paletteGene;
  uint8_t temperament;
  uint8_t mutationGenes;
  uint8_t reserved[3];
};

struct PetPalette {
  uint16_t primaryDark;
  uint16_t primary;
  uint16_t primaryLight;
  uint16_t secondary;
  uint16_t accent;
  uint16_t glow;
};

constexpr size_t PET_GENOME_CODE_LENGTH = 60;

PetGenome generatePetGenome();
PetGenome derivePetGenome(const uint32_t seed[4], const uint32_t evolutionSeed[4]);
PetGenome blendPetGenomes(const PetGenome &first, const PetGenome &second);
bool encodePetGenome(const PetGenome &genome, char *output, size_t outputSize);
bool decodePetGenome(const char *code, PetGenome &genome);
uint64_t petGenomeDesignId(const PetGenome &genome);
uint8_t evolvedGenomeGene(const PetGenome &genome, uint8_t base, uint8_t stage,
                          uint8_t channel, uint8_t maximumDrift);
uint16_t evolvedGenomeFeatures(const PetGenome &genome, uint8_t stage);
PetPalette paletteForGenome(const PetGenome &genome);
const char *eggLineageName(uint8_t lineage);
const char *elementName(uint8_t element);

// A two-part callsign-style name ("NOVA-BYTE", "GLITCH-WISP", ...),
// deterministic from the genome's own seed bytes -- same "pure function of
// genome" shape as eggLineageName()/elementName() above, just combining
// two words instead of indexing one, so it needs an out-buffer rather than
// returning a fixed string. Nothing new is persisted for this: it's
// re-derivable from the genome PetState already stores, the same way the
// egg lineage and element names are.
constexpr size_t PET_NAME_LENGTH = 24;
void petDisplayName(const PetGenome &genome, char *output, size_t outputSize);
