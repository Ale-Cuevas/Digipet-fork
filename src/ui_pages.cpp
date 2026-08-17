#include "ui_pages.h"

// The battle-stat formulas are declared here (not FamiliarBattleService's
// own header) precisely so drawBattleStatsView() can call the pure
// deriveMaxHp/Attack/Defense math without pulling in the rest of
// FamiliarBattleService -- that class's .cpp brings in NimBLE, which this
// file (and tools/sim's render harness that links it) has no business
// depending on. familiar_battle_rules.cpp is the same NimBLE-free file the
// native test suite already links for exactly this reason.
#include "familiar_battle_rules.h"

uint16_t scaleRgb565(uint16_t color, uint8_t percent) {
  const uint16_t red = min<uint16_t>(31, ((color >> 11) & 0x1F) * percent / 100);
  const uint16_t green = min<uint16_t>(63, ((color >> 5) & 0x3F) * percent / 100);
  const uint16_t blue = min<uint16_t>(31, (color & 0x1F) * percent / 100);
  return (red << 11) | (green << 5) | blue;
}

// Blends two RGB565 colors channel-wise; t is clamped to [0, 1]. Used to fake
// smooth fades, gradients and bloom on hardware with no alpha compositing.

uint16_t lerpRgb565(uint16_t from, uint16_t to, float t) {
  t = constrain(t, 0.0f, 1.0f);
  const int fr = (from >> 11) & 0x1F, tr = (to >> 11) & 0x1F;
  const int fg = (from >> 5) & 0x3F, tg = (to >> 5) & 0x3F;
  const int fb = from & 0x1F, tb = to & 0x1F;
  const uint16_t red = static_cast<uint16_t>(lroundf(fr + (tr - fr) * t));
  const uint16_t green = static_cast<uint16_t>(lroundf(fg + (tg - fg) * t));
  const uint16_t blue = static_cast<uint16_t>(lroundf(fb + (tb - fb) * t));
  return (red << 11) | (green << 5) | blue;
}

float bootSmoothstep(float from, float to, float value) {
  const float t = constrain((value - from) / (to - from), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

uint16_t readableTextColor(uint16_t fillColor) {
  return pickReadableColor(fillColor, COLOR_TEXT, COLOR_BACKGROUND);
}

void drawCentered(const char *text, int16_t y, uint8_t size, uint16_t color) {
  int16_t x1, y1;
  uint16_t w, h;
  display->setTextSize(size);
  display->getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  display->setCursor((LCD_WIDTH - w) / 2, y);
  display->setTextColor(color);
  display->print(text);
}

// A soft vertical wash used as the base of every full-screen page instead of
// a flat fill, so the whole UI reads as one layered surface rather than flat
// panels pasted on a single color. Every caller already renders through the
// PSRAM canvas, so the extra per-row work costs nothing on the wire.
// The vertical gradient color paintPageBackdrop() paints at a given y, so
// anything drawn after it (glows, halos) can blend toward its own color
// relative to that instead of guessing a fixed tone that can end up darker
// than the page around it.

uint16_t backdropColorAt(int16_t y) {
  const uint16_t horizon = lerpRgb565(COLOR_BACKGROUND, COLOR_CARD, 0.4f);
  return lerpRgb565(COLOR_BACKGROUND, horizon,
                    static_cast<float>(y) / (LCD_HEIGHT - 1));
}


void paintPageBackdrop() {
  for (int16_t y = 0; y < LCD_HEIGHT; ++y) {
    display->drawFastHLine(0, y, LCD_WIDTH, backdropColorAt(y));
  }
}

// A soft halo escaping from behind a card's edges, faked with a couple of
// widening, dimming outlines drawn before the card itself. Makes the hero
// panel on each page read as raised instead of flat-pasted.

void drawPanelGlow(int16_t x, int16_t y, int16_t w, int16_t h, int16_t radius,
                   uint16_t color) {
  display->drawRoundRect(x - 6, y - 6, w + 12, h + 12, radius + 6, scaleRgb565(color, 12));
  display->drawRoundRect(x - 3, y - 3, w + 6, h + 6, radius + 3, scaleRgb565(color, 22));
}

// Hand-pixelled 16x16 silhouettes, enlarged 2x for crisp 32x32 icons.
const uint16_t STAT_ICONS[4][16] PROGMEM = {
  {0x2C18, 0x2C18, 0x2C18, 0x2C18, 0x3C18, 0x1818, 0x1818, 0x1818,
   0x1818, 0x1818, 0x1818, 0x1818, 0x1818, 0x1818, 0x1818, 0x1818}, // fork + knife
  {0x03C0, 0x0FF0, 0x1FF8, 0x381C, 0x6006, 0x6666, 0x6666, 0x6006,
   0x6186, 0x6306, 0x360C, 0x1FF8, 0x0FF0, 0x03C0, 0x0000, 0x0000}, // smile
  {0x0180, 0x0300, 0x0600, 0x0C00, 0x1FF0, 0x3FE0, 0x07C0, 0x0780,
   0x0F00, 0x1E00, 0x3C00, 0x7800, 0x3000, 0x2000, 0x0000, 0x0000}, // bolt
  {0x0000, 0x0C30, 0x1E78, 0x3FFC, 0x7FFE, 0x7FFE, 0x7FFE, 0x3FFC,
   0x1FF8, 0x0FF0, 0x07E0, 0x03C0, 0x0180, 0x0000, 0x0000, 0x0000}  // heart
};

const char *STAT_LABELS[] = {"FOOD", "JOY", "ENERGY", "HEALTH"};

// Shared renderer for any 16x16 1-bit glyph table, drawn 2x for 32x32 icons.
void drawGlyph16(const uint16_t *rows, int16_t x, int16_t y, uint16_t color) {
  for (uint8_t row = 0; row < 16; row++) {
    const uint16_t pixels = pgm_read_word(&rows[row]);
    for (uint8_t column = 0; column < 16; column++) {
      if (pixels & (0x8000 >> column)) {
        display->fillRect(x + column * 2, y + row * 2, 2, 2, color);
      }
    }
  }
}


void drawStatIcon(StatIcon icon, int16_t x, int16_t y, uint16_t color) {
  drawGlyph16(STAT_ICONS[icon], x, y, color);
}


void drawStatRow(StatIcon icon, uint8_t value, int16_t y) {
  const uint16_t color = value < 25 ? COLOR_DANGER :
      (value < 50 ? COLOR_WARNING : COLOR_MINT);
  drawStatIcon(icon, 22, y - 17, color);
  // Label matches the value's own size 2 now (was 1) -- see
  // docs/style-guide.md's typography section: a label sitting right next
  // to the value it names reads as broken/mismatched at any different
  // size from that value, not just smaller-and-therefore-harder-to-read.
  // The bar shifts right/narrows accordingly (138-280 instead of
  // 112-280) to leave room for "ENERGY", the longest label, at the
  // larger size.
  display->setTextSize(2);
  display->setTextColor(COLOR_TEXT);
  display->setCursor(59, y - 7);
  display->print(STAT_LABELS[icon]);
  display->drawRoundRect(138, y - 7, 142, 18, 8, COLOR_MUTED);
  if (value > 0) {
    const int16_t filled = (136 * value) / 100;
    display->fillRoundRect(141, y - 4, filled, 12, 6, color);
    if (filled > 6) display->fillCircle(141 + filled - 6, y + 2, 5, lerpRgb565(color, RGB565_WHITE, 0.35f));
  }
  display->setTextColor(COLOR_TEXT);
  display->setCursor(294, y - 7);
  display->printf("%3u%%", value);
}


// A simple radar sweep -- three concentric rings, a center dot, and one
// sweep line -- for the RECON LOG action tile. Not added to STAT_ICONS
// since it isn't a pet stat glyph; drawn with plain primitives the same
// way drawSettingsIcon()'s tile icons are, rather than as a hand-encoded
// 16x16 bitmap like STAT_ICONS' glyphs are.
void drawRadarIcon(int16_t cx, int16_t cy, uint16_t color) {
  display->drawCircle(cx, cy, 9, color);
  display->drawCircle(cx, cy, 17, color);
  display->drawCircle(cx, cy, 25, color);
  display->fillCircle(cx, cy, 3, color);
  display->drawLine(cx, cy, cx + 21, cy - 15, color);
}

// The Status page's action grid tile -- see this function's own prototype
// comment in ui_pages.h for the icon-reuse rationale. Same chrome as
// drawSettingsTile() (card + glow + accent outline, alternating
// COLOR_CYAN/COLOR_PURPLE) so the two grids -- Settings' and this one --
// read as the same kind of control.
void drawActionTile(uint8_t action, int16_t x, int16_t y, const char *label,
                    const char *value, bool locked) {
  const uint16_t accent = locked ? COLOR_MUTED : (action & 1 ? COLOR_PURPLE : COLOR_CYAN);
  const uint16_t fill = locked ? scaleRgb565(COLOR_CARD, 55) : COLOR_CARD;
  drawPanelGlow(x, y, 160, 137, 20, accent);
  display->fillRoundRect(x, y, 160, 137, 20, fill);
  display->drawRoundRect(x, y, 160, 137, 20, accent);
  const uint16_t iconColor = locked ? COLOR_MUTED : COLOR_MINT;
  if (action == 3) {
    drawRadarIcon(x + 80, y + 43, iconColor);
  } else if (action == 4) {
    drawSettingsIcon(4, x + 80, y + 43, iconColor);  // "theme palette" icon, reused for ITEMS.
  } else if (action < 3) {
    drawStatIcon(static_cast<StatIcon>(action), x + 64, y + 18, iconColor);
  }
  // action >= 5 (a reserved-for-later placeholder) gets no icon at all.
  // label is size 2 (was 1) -- it's the tile's actual name (FEED, RECON
  // LOG, ...), the same "title" role drawCentered()'s own size-3 page
  // titles play, just one step down; value stays size 1 as its
  // description, the same "subtitle" role a page's own size-1 subtitle
  // plays. Not a mismatch to fix (see drawBattleStatsView()'s own comment
  // for that pattern) -- these two lines are deliberately different roles,
  // and value's longer strings ("SCAN WIFI+BLE") would overflow the
  // tile's width at size 2.
  display->setTextSize(2);
  display->setTextColor(locked ? COLOR_MUTED : COLOR_TEXT);
  display->setCursor(x + 16, y + 82);
  display->print(label);
  display->setTextSize(1);
  display->setTextColor(COLOR_MUTED);
  display->setCursor(x + 16, y + 104);
  display->print(value);
}

// Instant touch-down feedback for a button/tile, drawn as a bright double
// outline on top of whatever's already there. Called the moment a finger
// lands (see loop()'s touch-down handling), straight to the live panel
// rather than through the canvas/present pipeline the rest of the app
// uses -- the whole point is to beat the latency of a full recompose, and
// every action this covers already ends in one on release, which erases
// it for free. Doesn't need to know the button's own fill/label/color, so
// it works the same for any button without risking drift from its style.

void drawEgg(bool frame, uint16_t bg) {
  const PetPalette palette = paletteForGenome(pet.genome);
  const uint8_t lineage = pet.genome.lineage % 10;
  const uint8_t phase = (millis() / 90) % 32;
  const int y = 218 + ((phase < 16 ? phase : 31 - phase) / 8);

  // A compact aura and contact shadow establish depth without redrawing the UI.
  for (uint8_t i = 0; i < 6; ++i) {
    const int angleIndex = (phase + i * 5) & 31;
    const int px = 184 + ((angleIndex < 16 ? angleIndex : 31 - angleIndex) - 8) * 8;
    const int py = y - 8 + ((phase * 7 + i * 19) % 126) - 63;
    display->fillCircle(px, py, i & 1 ? 2 : 3,
                        i & 1 ? palette.glow : palette.accent);
  }
  display->fillEllipse(184, y + 67, 72, 13, COLOR_CARD);
  display->fillEllipse(184, y + 65, 56, 7, palette.primaryDark);

  // Offset nested shells produce a shaded, rounded volume on the AMOLED.
  display->fillEllipse(184, y, 62, 79, COLOR_TEXT);
  display->fillEllipse(184, y, 58, 75, palette.primaryDark);
  display->fillEllipse(188, y - 3, 53, 70, palette.primary);
  display->fillEllipse(194, y - 11, 43, 57, palette.primary);
  display->fillEllipse(201, y - 22, 27, 37, palette.primaryLight);
  display->fillEllipse(169, y + 19, 18, 42, palette.secondary);
  display->fillEllipse(202, y - 31, 9, 17, palette.primaryLight);
  display->fillCircle(200, y - 40, 3, lerpRgb565(palette.primaryLight, RGB565_WHITE, 0.7f));
  display->fillEllipse(205, y - 36, 3, 6, COLOR_TEXT);

  // Each lineage owns a visual language; genes vary placement and palette.
  switch (lineage) {
    case 0: {  // Ember: animated molten fractures.
      display->drawLine(171, y - 55, 183, y - 29, palette.glow);
      display->drawLine(183, y - 29, 173, y - 5, palette.glow);
      display->drawLine(173, y - 5, 188, y + 22, palette.accent);
      display->drawLine(188, y + 22, 180, y + 48, palette.glow);
      display->fillCircle(183, y - 29, frame ? 5 : 3, palette.accent);
      break;
    }
    case 1:  // Tidal: layered wave bands and bubbles.
      for (int row = -30; row <= 34; row += 18) {
        display->drawLine(151, y + row, 166, y + row - 5, palette.accent);
        display->drawLine(166, y + row - 5, 184, y + row + 2, palette.accent);
        display->drawLine(184, y + row + 2, 210, y + row - 4, palette.accent);
      }
      display->drawCircle(204, y + 30 - phase / 3, 5, palette.glow);
      display->drawCircle(164, y - 18 - phase / 4, 3, palette.glow);
      break;
    case 2:  // Verdant: branching leaf veins.
      display->drawLine(184, y + 53, 184, y - 48, palette.accent);
      for (int branch = -34; branch <= 34; branch += 17) {
        display->drawLine(184, y + branch, 158, y + branch - 14, palette.secondary);
        display->drawLine(184, y + branch - 7, 211, y + branch - 23, palette.accent);
      }
      display->fillTriangle(177, y - 65, 184, y - 82, 190, y - 62, palette.glow);
      break;
    case 3:  // Volt: angular circuit paths.
      display->drawLine(158, y - 42, 181, y - 42, palette.accent);
      display->drawLine(181, y - 42, 171, y - 15, palette.accent);
      display->drawLine(171, y - 15, 197, y - 15, palette.glow);
      display->drawLine(197, y - 15, 185, y + 17, palette.glow);
      display->drawLine(185, y + 17, 210, y + 17, palette.accent);
      for (int i = 0; i < 4; ++i)
        display->fillCircle(158 + i * 17, y + 42 - (i & 1) * 10, 3,
                            (phase + i) & 2 ? palette.glow : palette.accent);
      break;
    case 4:  // Umbral: iris-like sigil with an orbiting glint.
      display->fillEllipse(184, y + 1, 31, 19, palette.secondary);
      display->fillEllipse(184, y + 1, 20, 14, palette.accent);
      display->fillEllipse(184, y + 1, 8, 14, palette.primaryDark);
      display->fillCircle(188, y - 4, 3, palette.glow);
      display->drawCircle(184, y + 1, 37 + (frame ? 2 : 0), palette.glow);
      break;
    case 5:  // Digital: asymmetric data blocks and traces.
      for (uint8_t i = 0; i < 9; ++i) {
        const int bx = 151 + ((pet.genome.seed[0] >> (i * 3 % 24)) & 0x2F);
        const int by = y - 49 + ((pet.genome.seed[1] >> (i * 3 % 24)) & 0x5F);
        display->fillRect(bx, by, 5 + (i & 3) * 2, 5,
                          i & 1 ? palette.accent : palette.secondary);
      }
      display->drawRect(169, y - 25, 31, 46, palette.glow);
      display->fillRect(178, y - 16 + phase / 4, 13, 4, RGB565_WHITE);
      break;
    case 6:  // Crystal: luminous facets.
      display->fillTriangle(184, y - 59, 157, y - 4, 184, y + 9, palette.secondary);
      display->fillTriangle(184, y - 59, 214, y - 7, 184, y + 9, palette.accent);
      display->fillTriangle(157, y - 4, 184, y + 9, 166, y + 49, palette.primaryDark);
      display->fillTriangle(214, y - 7, 184, y + 9, 204, y + 46, palette.glow);
      display->drawLine(184, y - 59, 184, y + 55, RGB565_WHITE);
      break;
    case 7:  // Alloy: segmented armour shell.
      display->drawLine(145, y - 25, 223, y - 25, palette.accent);
      display->drawLine(135, y + 8, 232, y + 8, palette.secondary);
      display->drawLine(148, y + 40, 218, y + 40, palette.accent);
      display->drawLine(184, y - 71, 184, y + 62, palette.primaryDark);
      for (int sy : {-25, 8, 40}) {
        display->fillCircle(153, y + sy, 4, palette.glow);
        display->fillCircle(215, y + sy, 4, palette.glow);
      }
      break;
    case 8:  // Celestial: constellation joined across the shell.
      for (uint8_t i = 0; i < 6; ++i) {
        const int sx = 157 + ((pet.genome.seed[2] >> (i * 4)) & 0x37);
        const int sy = y - 47 + ((pet.genome.seed[3] >> (i * 4)) & 0x5F);
        display->fillCircle(sx, sy, i == (phase / 6) % 6 ? 4 : 2, palette.accent);
        if (i) {
          const int px = 157 + ((pet.genome.seed[2] >> ((i - 1) * 4)) & 0x37);
          const int py = y - 47 + ((pet.genome.seed[3] >> ((i - 1) * 4)) & 0x5F);
          display->drawLine(px, py, sx, sy, palette.glow);
        }
      }
      break;
    default:  // Primal: scale plates and a claw glyph.
      for (int row = -37; row <= 35; row += 18)
        for (int column = -24; column <= 24; column += 16)
          display->drawCircle(184 + column + ((row / 18) & 1) * 8,
                              y + row, 9, palette.secondary);
      display->drawLine(170, y - 38, 181, y + 32, palette.accent);
      display->drawLine(184, y - 42, 190, y + 31, palette.accent);
      display->drawLine(198, y - 35, 198, y + 28, palette.accent);
      break;
  }

  if (pet.genome.mutationGenes) {
    display->drawCircle(184, y, 66 + (frame ? 2 : 0), palette.glow);
  }
}


int geneScale(uint8_t gene, int minimum, int maximum) {
  return minimum + (static_cast<int>(gene) * (maximum - minimum)) / 255;
}


void drawElementAura(const PetPalette &palette, int cx, int cy, int radius,
                     uint8_t phase) {
  for (uint8_t i = 0; i < 8; ++i) {
    const uint8_t orbit = (phase + i * 4) & 31;
    const int wave = orbit < 16 ? orbit : 31 - orbit;
    const int x = cx + (wave - 8) * radius / 8;
    const int y = cy - radius + ((phase * 5 + i * 17) % (radius * 2));
    const uint16_t color = i & 1 ? palette.glow : palette.accent;
    switch (pet.genome.element % 6) {
      case 0:
        display->fillTriangle(x - 4, y + 6, x + 4, y + 6, x, y - 7, color);
        break;
      case 1:
        display->drawCircle(x, y, 3 + (i & 1), color);
        break;
      case 2:
        display->fillEllipse(x, y, 5, 2, color);
        break;
      case 3:
        display->drawLine(x - 4, y - 5, x + 1, y, color);
        display->drawLine(x + 1, y, x - 2, y + 6, color);
        break;
      case 4:
        display->fillCircle(x, y, 2 + (i & 1), color);
        display->drawCircle(x, y, 6, palette.secondary);
        break;
      default:
        display->fillRect(x - 3, y - 3, 6, 6, color);
        break;
    }
  }
}


void drawGenomeMarkings(const PetPalette &palette, int cx, int cy,
                        int width, int height, uint8_t stage, uint8_t markingGene) {
  switch (markingGene % 5) {
    case 0:  // stripes
      for (int y = cy - height / 4; y <= cy + height / 4; y += 13)
        display->fillRoundRect(cx - width / 3, y, width * 2 / 3, 4, 2,
                               palette.secondary);
      break;
    case 1:  // spots
      for (uint8_t i = 0; i < 5 + stage; ++i) {
        const int x = cx - width / 3 + ((pet.genome.seed[1] >> (i * 3 % 24)) %
                                        max(1, width * 2 / 3));
        const int y = cy - height / 4 + ((pet.genome.seed[2] >> (i * 3 % 24)) %
                                         max(1, height / 2));
        display->fillCircle(x, y, 3 + (i & 2), palette.secondary);
      }
      break;
    case 2:  // circuit
      display->drawLine(cx - width / 3, cy, cx - 8, cy, palette.accent);
      display->drawLine(cx - 8, cy, cx - 8, cy + height / 4, palette.accent);
      display->drawLine(cx - 8, cy + height / 4, cx + width / 3,
                        cy + height / 4, palette.accent);
      display->fillCircle(cx - width / 3, cy, 4, palette.glow);
      display->fillCircle(cx + width / 3, cy + height / 4, 4, palette.glow);
      break;
    case 3: {  // scales: overlapping two-tone shingles, sized to the body
      const int rows = max(2, height / 24);
      const int cols = max(3, width / 20);
      for (int row = -rows / 2; row <= rows / 2; ++row) {
        for (int column = -cols / 2; column <= cols / 2; ++column) {
          const int sx = cx + column * 11 + (row & 1) * 6;
          const int sy = cy + row * 10;
          display->fillCircle(sx, sy, 6, palette.primaryDark);
          display->fillCircle(sx, sy - 1, 4, palette.secondary);
        }
      }
      break;
    }
    default:  // elemental core
      display->drawCircle(cx, cy + 4, 14 + stage * 2, palette.accent);
      display->fillCircle(cx, cy + 4, 7 + stage, palette.glow);
      break;
  }
}


void drawGenomeFace(const PetPalette &palette, int cx, int cy, int headWidth,
                    bool blink, uint8_t stage, uint8_t faceGene) {
  const int spread = max(12, headWidth / 4);
  const int eyeWidth = 8 + (faceGene % 5);
  const int eyeHeight = 13 + ((faceGene >> 2) % 6);
  for (int direction : {-1, 1}) {
    const int x = cx + direction * spread;
    if (blink) {
      display->fillRoundRect(x - eyeWidth, cy, eyeWidth * 2, 4, 2,
                             palette.primaryDark);
    } else {
      display->fillEllipse(x, cy, eyeWidth, eyeHeight, COLOR_TEXT);
      display->fillEllipse(x + direction * 2, cy + 2, eyeWidth - 3,
                           eyeHeight - 4, palette.primaryDark);
      display->fillCircle(x + direction * 3, cy - 4, 3, palette.glow);
      display->fillCircle(x + direction * 4, cy - 6, 1, RGB565_WHITE);
    }
  }
  if ((faceGene & 1) == 0) {
    display->drawLine(cx - 10, cy + 24, cx, cy + 29, palette.primaryDark);
    display->drawLine(cx, cy + 29, cx + 10, cy + 24, palette.primaryDark);
  } else {
    display->fillTriangle(cx - 8, cy + 23, cx + 8, cy + 23,
                          cx, cy + 31, palette.primaryDark);
    if (stage >= 3) display->fillTriangle(cx - 5, cy + 24, cx, cy + 31,
                                          cx + 5, cy + 24, COLOR_TEXT);
  }
}

// A compact head-and-face "portrait" bust, used to give a combatant a face
// on the battle screen without needing the full per-body-type creature
// layout at a different scale -- more like a fighter-select badge than the
// companion page's full creature.

void drawCreaturePortrait(const PetGenome &genome, uint8_t stage, int cx, int cy,
                          int headWidth, uint16_t ringColor) {
  const PetPalette palette = paletteForGenome(genome);
  const int headHeight = headWidth * 4 / 5;
  display->fillCircle(cx, cy, headWidth / 2 + 12, scaleRgb565(ringColor, 16));
  display->drawCircle(cx, cy, headWidth / 2 + 10, ringColor);
  display->fillEllipse(cx, cy, headWidth / 2 + 4, headHeight / 2 + 4, COLOR_TEXT);
  display->fillEllipse(cx + 3, cy - 2, headWidth / 2, headHeight / 2, palette.primary);
  display->fillEllipse(cx + headWidth / 6, cy - headHeight / 6, headWidth / 5,
                       headHeight / 7, palette.primaryLight);
  display->fillCircle(cx + headWidth / 6 + 2, cy - headHeight / 6 - 2, 2,
                      lerpRgb565(palette.primaryLight, RGB565_WHITE, 0.6f));
  drawGenomeFace(palette, cx, cy, headWidth, false, stage, genome.faceGene);
}


void drawProceduralCreature(bool asleep) {
  const PetPalette palette = paletteForGenome(pet.genome);
  const uint8_t stage = constrain(pet.stage, 1, 4);
  const uint8_t phase = (millis() / 45) & 31;
  const int wave = phase < 16 ? phase : 31 - phase;
  const int bob = asleep ? 5 : (wave - 8) / 4;
  const bool blink = asleep || (millis() % 3400) > 3260;
  const int cx = 184;
  const int baseY = 316 + bob;
  const int growth = (stage - 1) * 9;
  const uint8_t widthGene = evolvedGenomeGene(pet.genome, pet.genome.widthGene,
                                               stage, 0, 24);
  const uint8_t heightGene = evolvedGenomeGene(pet.genome, pet.genome.heightGene,
                                                stage, 1, 24);
  const uint8_t headGene = evolvedGenomeGene(pet.genome, pet.genome.headGene,
                                              stage, 2, 20);
  const uint8_t faceGene = evolvedGenomeGene(pet.genome, pet.genome.faceGene,
                                              stage, 3, 7);
  const uint8_t markingGene = evolvedGenomeGene(pet.genome, pet.genome.markingGene,
                                                 stage, 1, 6);
  const uint16_t featureGenes = evolvedGenomeFeatures(pet.genome, stage);
  const int bodyWidth = geneScale(widthGene, 82, 132) + growth;
  const int bodyHeight = geneScale(heightGene, 75, 126) + growth;
  const int headWidth = geneScale(headGene, 76, 118) + growth / 2;
  const int headHeight = headWidth * 4 / 5;
  const int headY = baseY - bodyHeight - headHeight / 2 + 24;
  const uint8_t bodyType = pet.genome.bodyType % 5;

  drawElementAura(palette, cx, baseY - 105, 90 + stage * 5, phase);

  // A soft life-signal glow bleeds out from behind the silhouette, echoing
  // the boot sequence's core so the creature reads as the same living thing.
  // Blended relative to the actual backdrop color so it always brightens,
  // even against pale palettes where a flat scaled tone can read as a smudge.
  const uint16_t glowBase = backdropColorAt(baseY - bodyHeight / 2);
  display->fillEllipse(cx, baseY - bodyHeight / 2, bodyWidth / 2 + 30,
                       bodyHeight / 2 + 26, lerpRgb565(glowBase, palette.glow, 0.22f));
  display->fillEllipse(cx, baseY - bodyHeight / 2, bodyWidth / 2 + 14,
                       bodyHeight / 2 + 12, lerpRgb565(glowBase, palette.glow, 0.4f));

  display->fillEllipse(cx, baseY + 6, 68 + growth, 10 + stage, COLOR_CARD);
  display->fillEllipse(cx + 7, baseY + 4, 48 + growth, 5 + stage / 2,
                       palette.primaryDark);

  // Rear silhouette: tails, wings and stage-dependent mutations.
  const int tailSwing = (wave - 8) * 3;
  if ((featureGenes & 0x20) || bodyType == 0 || bodyType == 4) {
    display->fillTriangle(cx + bodyWidth / 3, baseY - 64,
                          cx + bodyWidth / 2 + 54, baseY - 80 + tailSwing,
                          cx + bodyWidth / 2 - 3, baseY - 33,
                          COLOR_TEXT);
    display->fillTriangle(cx + bodyWidth / 3 + 3, baseY - 62,
                          cx + bodyWidth / 2 + 43, baseY - 77 + tailSwing,
                          cx + bodyWidth / 2 - 1, baseY - 39,
                          palette.secondary);
  }
  if ((featureGenes & 0x08) || bodyType == 2) {
    const int wingLift = asleep ? 8 : (wave - 8);
    display->fillTriangle(cx - bodyWidth / 3, baseY - 112,
                          cx - bodyWidth / 2 - 58, baseY - 144 - wingLift,
                          cx - bodyWidth / 2 + 5, baseY - 56, COLOR_TEXT);
    display->fillTriangle(cx + bodyWidth / 3, baseY - 112,
                          cx + bodyWidth / 2 + 58, baseY - 144 - wingLift,
                          cx + bodyWidth / 2 - 5, baseY - 56, COLOR_TEXT);
    display->fillTriangle(cx - bodyWidth / 3 - 3, baseY - 108,
                          cx - bodyWidth / 2 - 45, baseY - 137 - wingLift,
                          cx - bodyWidth / 2 + 8, baseY - 64, palette.secondary);
    display->fillTriangle(cx + bodyWidth / 3 + 3, baseY - 108,
                          cx + bodyWidth / 2 + 45, baseY - 137 - wingLift,
                          cx + bodyWidth / 2 - 8, baseY - 64, palette.secondary);
    // Feather seams: a couple of short strokes between tip and body so each
    // wing reads as layered feathers instead of one flat triangle.
    for (int direction : {-1, 1}) {
      const int tipX = cx + direction * (bodyWidth / 2 + 45);
      const int tipY = baseY - 137 - wingLift;
      const int rootX = cx + direction * (bodyWidth / 2 - 8);
      const int rootY = baseY - 64;
      for (int i = 1; i <= 2; ++i) {
        const int fx = tipX + (rootX - tipX) * i / 3;
        const int fy = tipY + (rootY - tipY) * i / 3;
        display->drawLine(fx, fy, fx + direction * 9, fy + 12, palette.primaryDark);
      }
    }
  }

  int bodyCx = cx;
  int bodyCy = baseY - bodyHeight / 2;
  if (bodyType == 4) {  // serpent: overlapping coils and raised torso
    display->fillEllipse(cx, baseY - 10, bodyWidth / 2 + 25, 25, COLOR_TEXT);
    display->fillEllipse(cx + 4, baseY - 12, bodyWidth / 2 + 17, 18,
                         palette.primaryDark);
    display->fillEllipse(cx - 25, baseY - 34, bodyWidth / 2, 31, COLOR_TEXT);
    display->fillEllipse(cx - 20, baseY - 37, bodyWidth / 2 - 6, 24,
                         palette.primary);
    bodyCy -= 8;
    display->fillRoundRect(cx - bodyWidth / 3, bodyCy - bodyHeight / 2,
                           bodyWidth * 2 / 3, bodyHeight, bodyWidth / 3, COLOR_TEXT);
    display->fillRoundRect(cx - bodyWidth / 3 + 6, bodyCy - bodyHeight / 2 + 5,
                           bodyWidth * 2 / 3 - 12, bodyHeight - 11,
                           bodyWidth / 3 - 5, palette.primary);
    display->fillEllipse(cx - bodyWidth / 10, bodyCy - bodyHeight / 4,
                         bodyWidth / 7, bodyHeight / 10, palette.primaryLight);
    display->fillCircle(cx - bodyWidth / 10 - 3, bodyCy - bodyHeight / 4 - 2, 2,
                        lerpRgb565(palette.primaryLight, RGB565_WHITE, 0.6f));
  } else if (bodyType == 0) {  // quadruped: broad torso and four grounded legs
    bodyCy = baseY - 67;
    display->fillRoundRect(cx - bodyWidth / 2, bodyCy - bodyHeight / 3,
                           bodyWidth, bodyHeight * 2 / 3, bodyHeight / 3, COLOR_TEXT);
    display->fillRoundRect(cx - bodyWidth / 2 + 6, bodyCy - bodyHeight / 3 + 5,
                           bodyWidth - 12, bodyHeight * 2 / 3 - 10,
                           bodyHeight / 3 - 4, palette.primary);
    display->fillEllipse(cx - bodyWidth / 6, bodyCy - bodyHeight / 5,
                         bodyWidth / 5, bodyHeight / 9, palette.primaryLight);
    display->fillCircle(cx - bodyWidth / 6 - 3, bodyCy - bodyHeight / 5 - 2, 2,
                        lerpRgb565(palette.primaryLight, RGB565_WHITE, 0.6f));
    for (int direction : {-1, 1}) {
      // The outer leg of each pair is drawn a touch smaller and higher than
      // the inner one, a cheap forced-perspective cue so four legs stamped
      // in a flat row still read as a body with some girth to it.
      for (int inner : {0, 1}) {
        const int legX = cx + direction * (bodyWidth / 4 + inner * 17);
        const int legW = inner ? 24 : 28;
        const int legH = inner ? 62 : 68;
        const int legTop = baseY - 67 + (inner ? 5 : 0);
        display->fillRoundRect(legX - legW / 2, legTop, legW, legH, 12, COLOR_TEXT);
        display->fillRoundRect(legX - legW / 2 + 5, legTop + 3, legW - 10, legH - 6,
                               8, inner ? palette.primaryDark : palette.primary);
        display->fillEllipse(legX + direction * 4, baseY - (inner ? 3 : 0), 18, 8,
                             COLOR_TEXT);
      }
    }
  } else if (bodyType == 2) {  // avian: tapered feather body and talons
    bodyCy = baseY - bodyHeight / 2;
    display->fillTriangle(cx, bodyCy - bodyHeight / 2, cx - bodyWidth / 2,
                          baseY - 29, cx, baseY - 2, COLOR_TEXT);
    display->fillTriangle(cx, bodyCy - bodyHeight / 2 + 7,
                          cx - bodyWidth / 2 + 8, baseY - 31,
                          cx, baseY - 10, palette.primary);
    display->fillTriangle(cx, bodyCy - bodyHeight / 2, cx + bodyWidth / 2,
                          baseY - 29, cx, baseY - 2, COLOR_TEXT);
    display->fillTriangle(cx, bodyCy - bodyHeight / 2 + 7,
                          cx + bodyWidth / 2 - 8, baseY - 31,
                          cx, baseY - 10, palette.primaryLight);
    for (int direction : {-1, 1}) {
      display->drawLine(cx + direction * 20, baseY - 25,
                        cx + direction * 24, baseY, COLOR_TEXT);
      display->drawLine(cx + direction * 24, baseY,
                        cx + direction * 35, baseY + 3, COLOR_TEXT);
    }
  } else if (bodyType == 3) {  // blob: teardrop body tapering toward the head
    bodyCy = baseY - bodyHeight / 2;
    display->fillEllipse(cx, bodyCy, bodyWidth / 2, bodyHeight / 2, COLOR_TEXT);
    display->fillEllipse(cx, bodyCy - bodyHeight / 3, bodyWidth / 3,
                         bodyHeight / 3, COLOR_TEXT);
    display->fillEllipse(cx + 5, bodyCy - 4, bodyWidth / 2 - 6,
                         bodyHeight / 2 - 6, palette.primary);
    display->fillEllipse(cx + 4, bodyCy - bodyHeight / 3 - 3, bodyWidth / 3 - 5,
                         bodyHeight / 3 - 5, palette.primary);
    display->fillEllipse(cx - bodyWidth / 6, bodyCy - bodyHeight / 5,
                         bodyWidth / 6, bodyHeight / 9, palette.primaryLight);
    display->fillCircle(cx - bodyWidth / 6 - 3, bodyCy - bodyHeight / 5 - 2, 2,
                        lerpRgb565(palette.primaryLight, RGB565_WHITE, 0.6f));
    display->fillEllipse(cx - 24, baseY - 3, 25, 10, COLOR_TEXT);
    display->fillEllipse(cx + 24, baseY - 3, 25, 10, COLOR_TEXT);
  } else {  // humanoid: torso, articulated arms and legs
    bodyCy = baseY - bodyHeight / 2;
    display->fillRoundRect(cx - bodyWidth / 2, bodyCy - bodyHeight / 2,
                           bodyWidth, bodyHeight, bodyWidth / 4, COLOR_TEXT);
    display->fillRoundRect(cx - bodyWidth / 2 + 6, bodyCy - bodyHeight / 2 + 6,
                           bodyWidth - 12, bodyHeight - 12, bodyWidth / 4 - 3,
                           palette.primary);
    display->fillEllipse(cx - bodyWidth / 6, bodyCy - bodyHeight / 4,
                         bodyWidth / 6, bodyHeight / 10, palette.primaryLight);
    display->fillCircle(cx - bodyWidth / 6 - 3, bodyCy - bodyHeight / 4 - 2, 2,
                        lerpRgb565(palette.primaryLight, RGB565_WHITE, 0.6f));
    for (int direction : {-1, 1}) {
      // Pushed out enough to leave a sliver of background between torso and
      // arm outlines, plus a joint accent, so they read as separate limbs
      // instead of fusing into one white silhouette.
      const int armX = cx + direction * (bodyWidth / 2 + 15);
      display->fillRoundRect(armX - 13, bodyCy - bodyHeight / 3,
                             26, bodyHeight * 2 / 3, 12, COLOR_TEXT);
      display->fillRoundRect(armX - 8, bodyCy - bodyHeight / 3 + 5,
                             16, bodyHeight * 2 / 3 - 10, 8, palette.secondary);
      display->fillCircle(cx + direction * (bodyWidth / 2 + 3),
                          bodyCy - bodyHeight / 3 + 8, 8, palette.accent);
      const int legX = cx + direction * bodyWidth / 4;
      display->fillRoundRect(legX - 17, baseY - 58, 34, 62, 13, COLOR_TEXT);
      display->fillRoundRect(legX - 11, baseY - 54, 22, 51, 9,
                             palette.primaryDark);
      display->fillEllipse(legX + direction * 5, baseY + 1, 22, 8, COLOR_TEXT);
    }
  }

  drawGenomeMarkings(palette, bodyCx, bodyCy, bodyWidth, bodyHeight, stage,
                     markingGene);

  // Evolved forms earn actual armor instead of just scaling up: a plate or
  // guard keyed to body type, with a glowing rivet once fully matured.
  if (stage >= 3) {
    if (bodyType == 0) {  // quadruped: shoulder guard over the front torso
      display->fillTriangle(bodyCx - bodyWidth / 2 - 4, bodyCy - bodyHeight / 3,
                            bodyCx, bodyCy - bodyHeight / 3 - 24,
                            bodyCx + bodyWidth / 2 + 4, bodyCy - bodyHeight / 3,
                            COLOR_TEXT);
      display->fillTriangle(bodyCx - bodyWidth / 2 + 3, bodyCy - bodyHeight / 3 - 3,
                            bodyCx, bodyCy - bodyHeight / 3 - 17,
                            bodyCx + bodyWidth / 2 - 3, bodyCy - bodyHeight / 3 - 3,
                            palette.accent);
    } else if (bodyType == 1) {  // humanoid: pauldrons over each shoulder
      for (int direction : {-1, 1}) {
        const int sx = cx + direction * (bodyWidth / 2 + 3);
        const int sy = bodyCy - bodyHeight / 3 + 8;
        display->fillCircle(sx, sy, 14, COLOR_TEXT);
        display->fillCircle(sx, sy, 10, palette.accent);
      }
    } else if (bodyType == 2) {  // avian: breastplate wedge
      display->fillTriangle(cx, bodyCy - bodyHeight / 2 - 6, cx - bodyWidth / 4,
                            bodyCy + 4, cx + bodyWidth / 4, bodyCy + 4, COLOR_TEXT);
      display->fillTriangle(cx, bodyCy - bodyHeight / 2, cx - bodyWidth / 5 + 2,
                            bodyCy - 2, cx + bodyWidth / 5 - 2, bodyCy - 2,
                            palette.accent);
    } else if (bodyType == 3) {  // blob: dorsal shell ridge
      display->fillEllipse(cx, bodyCy - bodyHeight / 3, bodyWidth / 3 + 4,
                           bodyHeight / 6 + 3, COLOR_TEXT);
      display->fillEllipse(cx, bodyCy - bodyHeight / 3 - 2, bodyWidth / 3 - 3,
                           bodyHeight / 6 - 2, palette.accent);
    } else {  // serpent: banded collar
      display->fillEllipse(cx, bodyCy - bodyHeight / 2 + 4, bodyWidth / 2 + 8, 11,
                           COLOR_TEXT);
      display->fillEllipse(cx, bodyCy - bodyHeight / 2 + 2, bodyWidth / 2 + 2, 7,
                           palette.accent);
    }
    if (stage >= 4) {
      const float platePulse = (sinf(phase * 0.2f) + 1.0f) * 0.5f;
      display->fillCircle(bodyCx, bodyCy - bodyHeight / 3, 3 + lroundf(platePulse * 2),
                          lerpRgb565(palette.glow, RGB565_WHITE, platePulse * 0.5f));
    }
  }

  // Head, ears/horns and foreground identity features.
  if ((featureGenes & 0x01) || stage >= 3) {
    // A tapered three-point crest, sized relative to the head so it scales
    // cleanly across stages instead of sprawling into competing shards.
    const int crownBase = headY - headHeight / 2 + 6;
    const int crownWidth = max(6, headWidth / 8);
    auto crestSpike = [&](int offsetX, int height) {
      const int bx = cx + offsetX;
      display->fillTriangle(bx - crownWidth, crownBase, bx + crownWidth, crownBase,
                            bx, crownBase - height, COLOR_TEXT);
      display->fillTriangle(bx - crownWidth + 3, crownBase - 2,
                            bx + crownWidth - 3, crownBase - 2,
                            bx, crownBase - height + 6, palette.accent);
    };
    crestSpike(0, headHeight / 2 + 10);
    crestSpike(-headWidth / 4, headHeight / 3 + 6);
    crestSpike(headWidth / 4, headHeight / 3 + 6);
  }
  display->fillEllipse(cx, headY, headWidth / 2 + 5, headHeight / 2 + 5, COLOR_TEXT);
  display->fillEllipse(cx + 4, headY - 3, headWidth / 2 - 2,
                       headHeight / 2 - 2, palette.primary);
  display->fillEllipse(cx + headWidth / 6, headY - headHeight / 6,
                       headWidth / 5, headHeight / 7, palette.primaryLight);
  display->fillCircle(cx + headWidth / 6 + 2, headY - headHeight / 6 - 3, 2,
                      lerpRgb565(palette.primaryLight, RGB565_WHITE, 0.6f));

  if ((featureGenes & 0x02) || bodyType == 2) {
    // Restrained, two-toned so they read as ears against pale palettes
    // instead of blending into the crown's shards.
    const int earSize = 15 + stage * 2;
    for (int direction : {-1, 1}) {
      const int bx = cx + direction * (headWidth / 2 - 4);
      const int by = headY - headHeight / 3;
      display->fillTriangle(bx, by + 10, bx + direction * earSize,
                            by - earSize * 3 / 4, bx + direction * 5, by - 4,
                            COLOR_TEXT);
      display->fillTriangle(bx + direction * 2, by + 5,
                            bx + direction * (earSize - 4), by - earSize * 3 / 4 + 4,
                            bx + direction * 4, by - 1, palette.secondary);
    }
  }
  if ((featureGenes & 0x80) && stage >= 2) {
    for (int direction : {-1, 1})
      display->fillTriangle(cx + direction * headWidth / 3, headY - headHeight / 3,
                            cx + direction * (headWidth / 2 + 15), headY - 8,
                            cx + direction * headWidth / 2, headY + 18,
                            palette.accent);
  }
  if (stage >= 3 || (featureGenes & 0x100)) {
    display->drawRoundRect(cx - headWidth / 2 + 3, headY - headHeight / 2 + 4,
                           headWidth - 6, headHeight - 8, headHeight / 3,
                           palette.accent);
  }
  drawGenomeFace(palette, cx, headY, headWidth, blink, stage, faceGene);

  if (bodyType == 2) {  // avian: a beak over the mouth position
    display->fillTriangle(cx - 9, headY + 20, cx + 9, headY + 20,
                          cx, headY + 32, COLOR_TEXT);
    display->fillTriangle(cx - 6, headY + 21, cx + 6, headY + 21,
                          cx, headY + 29, palette.accent);
  }

  if (pet.genome.mutationGenes) {
    display->drawCircle(cx, headY, headWidth / 2 + 12 + (wave > 7), palette.glow);
    if (pet.genome.mutationGenes & 0x02)
      display->fillCircle(cx, headY - headHeight / 2 - 15, 8, palette.glow);
  }
}


void drawCreature(bool frame, bool asleep) {
  const uint16_t bg = asleep ? RGB565_BLACK : COLOR_BACKGROUND;
  if (pet.stage == 0) {
    drawEgg(frame, bg);
    return;
  }
  drawProceduralCreature(asleep);
}


void drawPageDots(Page active) {
  // PAGE_STATUS used to sit at y=355, tuned for the old single-view Status
  // page's FEED/PLAY/TRAIN row starting at y=375 -- now that those moved
  // into the action grid (drawStatusActionsView(), which runs to y=364)
  // and the summary view's own hints sit at y=374/399 (drawStatusSummaryView(),
  // matching the Companion page's identical hint-line convention), the
  // default 425 clears both sub-views cleanly and there's no more reason
  // for Status to be the odd one out.
  const int16_t y = active >= PAGE_SETTINGS ? 437 : 425;
  const Page pages[] = {PAGE_COMPANION, PAGE_STATUS, PAGE_BATTLE,
                        PAGE_SETTINGS, PAGE_GENOME_LAB};
  for (uint8_t i = 0; i < 5; ++i) {
    display->fillCircle(152 + i * 16, y, 4,
                        active == pages[i] ? COLOR_MINT : COLOR_CARD);
  }
}


void drawCompanionPage() {
  paintPageBackdrop();
  // A generated callsign-style name (petDisplayName(), pet_genome.cpp)
  // instead of the old static "DIGIPET // 001" -- deterministic from the
  // genome's own seed, so it stays the same for this companion across
  // reboots without needing anything new persisted, the same way
  // eggLineageName()/elementName() below don't need their own storage
  // either.
  char name[PET_NAME_LENGTH];
  petDisplayName(pet.genome, name, sizeof(name));
  drawCentered(name, 15, 3, COLOR_MINT);
  display->setTextSize(1);
  display->setTextColor(COLOR_CYAN);
  display->setCursor(28, 49);
  display->print(pet.stage == 0 ? eggLineageName(pet.genome.lineage)
                                : STAGE_NAMES[pet.stage]);
  display->setTextSize(2);
  display->setTextColor(clockValid ? COLOR_TEXT : COLOR_MUTED);
  display->setCursor(288, 45);
  display->print(clockText);
  drawPanelGlow(20, 82, 328, 276, 32, COLOR_CYAN);
  display->drawRoundRect(20, 82, 328, 276, 32, COLOR_CARD);
  display->drawRoundRect(27, 89, 314, 262, 27, COLOR_CYAN);
  display->fillRect(14, 126, 18, 5, COLOR_PURPLE);
  display->fillRect(336, 309, 18, 5, COLOR_MINT);
  display->drawCircle(54, 111, 8, COLOR_MINT);
  display->drawCircle(332, 329, 8, COLOR_PURPLE);
  drawCreature(false, false);
  drawCentered("TAP PET FOR PROFILE", 374, 1, COLOR_MUTED);
  drawCentered("SWIPE FOR STATUS  >", 399, 1, COLOR_CYAN);
  drawPageDots(PAGE_COMPANION);
}


// The stats-summary sub-view (statusShowingActions == false) -- identity,
// the 4 stat bars, and hardware diagnostics. This is everything the old
// single-view Status page showed except its FEED/PLAY/TRAIN row, which
// moved into the action grid below (see drawStatusActionsView()) once
// FEED stopped being an instant tap and started being a several-second
// scan (performFeedScan(), main.cpp) worth a swipe away from the numbers.
void drawStatusSummaryView() {
  drawCentered("COMPANION STATUS", 18, 3, COLOR_MINT);
  char identity[40];
  if (pet.stage == 0) {
    snprintf(identity, sizeof(identity), "%s // %s",
             eggLineageName(pet.genome.lineage), elementName(pet.genome.element));
  } else {
    snprintf(identity, sizeof(identity), "%s // %s",
             STAGE_NAMES[pet.stage], elementName(pet.genome.element));
  }
  drawCentered(identity, 49, 1, COLOR_CYAN);
  drawStatRow(ICON_FOOD, pet.food, 82);
  drawStatRow(ICON_JOY, pet.joy, 128);
  drawStatRow(ICON_ENERGY, pet.energy, 174);
  drawStatRow(ICON_HEALTH, pet.health, 220);

  drawPanelGlow(25, 251, 318, 91, 18, COLOR_CYAN);
  display->fillRoundRect(25, 251, 318, 91, 18, COLOR_CARD);
  display->setTextSize(2);
  display->setCursor(45, 265);
  display->setTextColor(imuDetected ? COLOR_MINT : COLOR_DANGER);
  display->print(imuDetected ? "IMU OK" : "IMU --");
  display->setCursor(195, 265);
  display->setTextColor(rtcDetected ? COLOR_MINT : COLOR_DANGER);
  display->print(rtcDetected ? "RTC OK" : "RTC --");
  display->setCursor(45, 294);
  display->setTextColor(pmuDetected ? COLOR_MINT : COLOR_DANGER);
  display->print(pmuDetected ? "PWR OK" : "PWR --");
  display->setCursor(195, 294);
  display->setTextColor(codecDetected ? COLOR_MINT : COLOR_DANGER);
  display->print(codecDetected ? "SOUND OK" : "SOUND --");
  display->setTextSize(1);
  display->setTextColor(COLOR_MUTED);
  display->setCursor(45, 324);
  display->printf("AGE %lumin  CARE %lu  TRAIN %u%%  //  %u DEV",
                  (unsigned long)pet.ageMinutes, (unsigned long)pet.actions,
                  pet.training, i2cDeviceCount);

  drawCentered("SWIPE UP FOR ACTIONS", 374, 1, COLOR_MUTED);
  drawCentered("FEED // PLAY // TRAIN // RECON", 399, 1, COLOR_CYAN);
}

// The action grid sub-view (statusShowingActions == true) -- same 2x2
// tile layout Settings' own home grid uses (drawSettingsPage()), reusing
// its exact tile geometry so the two grids feel like the same control.
// Unlike Settings' grid this is a single page (4 items, no paging needed).
void drawStatusActionsView() {
  drawCentered("COMPANION ACTIONS", 18, 3, COLOR_MINT);
  drawCentered(statusActionsPage == 0 ? "SELECT AN ACTION // SWIPE UP"
                                      : "SELECT AN ACTION // SWIPE DOWN",
               51, 1, COLOR_CYAN);

  if (statusActionsPage == 0) {
    drawActionTile(0, 18, 78, "FEED", "SCAN WIFI+BLE");
    drawActionTile(1, 190, 78, "PLAY", "BOOST JOY");
    drawActionTile(2, 18, 227, "TRAIN", "BOOST STATS");
    char reconValue[16];
    snprintf(reconValue, sizeof(reconValue), "%u SIGNALS", reconLog.entryCount);
    drawActionTile(3, 190, 227, "RECON LOG", reconValue);
  } else {
    char itemsValue[16];
    const uint16_t itemCount = inventory.counts[0] + inventory.counts[1] +
        inventory.counts[2] + inventory.counts[3] + inventory.counts[4];
    snprintf(itemsValue, sizeof(itemsValue), "%u HELD", itemCount);
    drawActionTile(4, 18, 78, "ITEMS", itemsValue);
    drawActionTile(5, 190, 78, "LOCKED", "COMING SOON", /*locked=*/true);
    drawActionTile(6, 18, 227, "LOCKED", "COMING SOON", /*locked=*/true);
    drawActionTile(7, 190, 227, "LOCKED", "COMING SOON", /*locked=*/true);
  }
  drawCentered(statusActionsPage == 0 ? "1  /  2" : "2  /  2", 382, 1, COLOR_MUTED);
}

void drawStatusPage() {
  paintPageBackdrop();
  if (statusShowingActions) {
    drawStatusActionsView();
  } else {
    drawStatusSummaryView();
  }
  drawPageDots(PAGE_STATUS);
}


void drawSettingsIcon(uint8_t item, int16_t cx, int16_t cy, uint16_t color) {
  switch (item) {
    case 0:  // brightness
      display->drawCircle(cx, cy, 15, color);
      display->fillCircle(cx, cy, 8, color);
      for (uint8_t i = 0; i < 8; ++i) {
        const float angle = i * 0.785398f;
        display->drawLine(cx + lroundf(cosf(angle) * 20), cy + lroundf(sinf(angle) * 20),
                          cx + lroundf(cosf(angle) * 27), cy + lroundf(sinf(angle) * 27), color);
      }
      break;
    case 1:  // idle timer
      display->drawCircle(cx, cy, 24, color);
      display->drawLine(cx, cy, cx, cy - 14, color);
      display->drawLine(cx, cy, cx + 12, cy + 7, color);
      break;
    case 2:  // speaker
      display->fillRect(cx - 25, cy - 9, 12, 18, color);
      display->fillTriangle(cx - 13, cy - 9, cx + 3, cy - 22, cx + 3, cy + 22, color);
      display->drawCircle(cx + 3, cy, 17, color);
      display->drawCircle(cx + 3, cy, 25, color);
      break;
    case 3:  // wake
      display->drawCircle(cx, cy, 24, color);
      display->fillCircle(cx, cy, 7, color);
      display->drawLine(cx, cy - 8, cx, cy - 27, color);
      display->drawLine(cx - 20, cy + 16, cx - 29, cy + 23, color);
      display->drawLine(cx + 20, cy + 16, cx + 29, cy + 23, color);
      break;
    case 4:  // theme palette
      display->drawCircle(cx, cy, 25, color);
      display->fillCircle(cx - 10, cy - 8, 5, COLOR_CYAN);
      display->fillCircle(cx + 8, cy - 11, 5, COLOR_PURPLE);
      display->fillCircle(cx + 13, cy + 7, 5, COLOR_WARNING);
      display->fillCircle(cx - 7, cy + 12, 5, COLOR_MINT);
      break;
    case 5:  // boot effect
      display->drawLine(cx, cy - 28, cx, cy + 28, color);
      display->drawLine(cx - 28, cy, cx + 28, cy, color);
      display->drawLine(cx - 19, cy - 19, cx + 19, cy + 19, color);
      display->drawLine(cx + 19, cy - 19, cx - 19, cy + 19, color);
      display->fillCircle(cx, cy, 8, color);
      break;
    case 6:  // identity card
      display->drawRoundRect(cx - 28, cy - 20, 56, 40, 6, color);
      display->fillCircle(cx - 14, cy - 5, 7, color);
      display->drawLine(cx - 23, cy + 12, cx - 5, cy + 12, color);
      display->drawLine(cx + 3, cy - 8, cx + 20, cy - 8, color);
      display->drawLine(cx + 3, cy + 2, cx + 20, cy + 2, color);
      break;
    default:  // update
      display->drawLine(cx, cy - 25, cx, cy + 13, color);
      display->fillTriangle(cx - 15, cy + 4, cx + 15, cy + 4, cx, cy + 23, color);
      display->drawLine(cx - 25, cy + 28, cx + 25, cy + 28, color);
      break;
  }
}


void drawSettingsTile(uint8_t item, int16_t x, int16_t y,
                      const char *label, const char *value) {
  const uint16_t accent = item & 1 ? COLOR_PURPLE : COLOR_CYAN;
  drawPanelGlow(x, y, 160, 137, 20, accent);
  display->fillRoundRect(x, y, 160, 137, 20, COLOR_CARD);
  display->drawRoundRect(x, y, 160, 137, 20, accent);
  drawSettingsIcon(item, x + 80, y + 43, COLOR_MINT);
  // Same label-is-size-2/value-is-size-1 "title/subtitle" role split as
  // drawActionTile() -- see that function's own comment. "BOOT EFFECT",
  // the longest label here, is 132px at size 2 starting from x+16 in a
  // 160px-wide tile -- fits with a few px to spare before the edge.
  display->setTextSize(2);
  display->setTextColor(COLOR_TEXT);
  display->setCursor(x + 16, y + 82);
  display->print(label);
  display->setTextSize(1);
  display->setTextColor(COLOR_MUTED);
  display->setCursor(x + 16, y + 104);
  display->print(value);
}


void drawCenteredInRect(const char *text, int16_t x, int16_t y,
                        int16_t width, int16_t height, uint8_t size,
                        uint16_t color) {
  int16_t x1, y1;
  uint16_t textWidth, textHeight;
  display->setTextSize(size);
  display->getTextBounds(text, 0, 0, &x1, &y1, &textWidth, &textHeight);
  display->setTextColor(color);
  display->setCursor(x + (width - textWidth) / 2, y + (height - textHeight) / 2);
  display->print(text);
}


void drawSettingsBack() {
  drawPanelGlow(84, 385, 200, 43, 14, COLOR_CYAN);
  display->fillRoundRect(84, 385, 200, 43, 14, COLOR_CARD);
  display->drawRoundRect(84, 385, 200, 43, 14, COLOR_CYAN);
  drawCenteredInRect("<  BACK", 84, 385, 200, 43, 2, COLOR_TEXT);
}


void drawChoiceRow(const char *label, int16_t y, bool selected) {
  const uint16_t fill = selected ? COLOR_PURPLE : COLOR_CARD;
  display->fillRoundRect(28, y, 312, 48, 14, fill);
  display->drawRoundRect(28, y, 312, 48, 14,
                         selected ? COLOR_MINT : COLOR_MUTED);
  display->setTextSize(2);
  display->setTextColor(readableTextColor(fill));
  display->setCursor(47, y + 17);
  display->print(label);
  if (selected) {
    display->fillCircle(316, y + 24, 8, COLOR_MINT);
    display->fillCircle(316, y + 24, 3, COLOR_BACKGROUND);
  }
}

// Four small preview dots (cyan/purple/warning/mint, the same color order
// as the "theme palette" Settings-home tile icon -- drawSettingsIcon()'s
// case 4) so picking a theme from THEME_LABELS' plain names on the Theme
// choice row isn't a blind guess at what it actually looks like. AUTO
// (themeIndex 0) has no fixed palette of its own -- see kThemes' own
// comment in ui_pages.h -- so it previews the current pet's own
// genome-derived colors instead, matching what AUTO actually resolves to
// via applyTheme().
void drawThemeSwatch(int16_t cx, int16_t cy, uint8_t themeIndex) {
  uint16_t cyan, purple, warning, mint;
  if (themeIndex == 0) {
    const PetPalette palette = paletteForGenome(pet.genome);
    cyan = palette.glow;
    purple = palette.secondary;
    warning = palette.accent;
    mint = palette.primaryLight;
  } else {
    const ThemeColors &theme = kThemes[themeIndex];
    cyan = theme.cyan;
    purple = theme.secondary;
    warning = theme.warning;
    mint = theme.primary;
  }
  const uint16_t colors[4] = {cyan, purple, warning, mint};
  for (uint8_t i = 0; i < 4; ++i) {
    display->fillCircle(cx + i * 14, cy, 5, colors[i]);
  }
}


void drawSettingsControlPage() {
  paintPageBackdrop();
  const char *titles[] = {"SETTINGS", "BRIGHTNESS", "IDLE TIMER", "VOLUME",
                          "WAKE MODE", "THEME", "BOOT EFFECT"};
  drawCentered(titles[settingsView], 18, 3, COLOR_MINT);
  drawCentered("SELECT AN OPTION", 51, 1, COLOR_CYAN);

  if (settingsView == SETTINGS_BRIGHTNESS) {
    for (uint8_t value = 0; value <= 10; ++value) {
      const uint8_t column = value % 3;
      const uint8_t row = value / 3;
      const int16_t x = 22 + column * 113;
      const int16_t y = 83 + row * 68;
      const bool selected = settings.brightnessIndex == value;
      const uint16_t fill = selected ? COLOR_PURPLE : COLOR_CARD;
      display->fillRoundRect(x, y, 98, 53, 13, fill);
      display->drawRoundRect(x, y, 98, 53, 13,
                             selected ? COLOR_MINT : COLOR_MUTED);
      char percent[8];
      snprintf(percent, sizeof(percent), "%u%%", value * 10);
      drawCenteredInRect(percent, x, y, 98, 53, 2, readableTextColor(fill));
    }
  } else if (settingsView == SETTINGS_IDLE) {
    for (uint8_t i = 0; i < 4; ++i)
      drawChoiceRow(SLEEP_LABELS[i], 89 + i * 62, settings.sleepIndex == i);
  } else if (settingsView == SETTINGS_VOLUME) {
    for (uint8_t i = 0; i < 5; ++i)
      drawChoiceRow(VOLUME_LABELS[i], 78 + i * 57, settings.volumeIndex == i);
  } else if (settingsView == SETTINGS_WAKE) {
    drawChoiceRow("TOUCH TO WAKE", 121, settings.wakeMode == 0);
    drawChoiceRow("BOOT KEY TO WAKE", 190, settings.wakeMode == 1);
  } else if (settingsView == SETTINGS_THEME) {
    for (uint8_t i = 0; i < 5; ++i) {
      const int16_t y = 78 + i * 57;
      drawChoiceRow(THEME_LABELS[i], y, settings.themeIndex == i);
      drawThemeSwatch(240, y + 24, i);
    }
  } else if (settingsView == SETTINGS_BOOT) {
    drawChoiceRow("BOOT EFFECT ON", 130, settings.bootAnimationEnabled);
    drawChoiceRow("BOOT EFFECT OFF", 199, !settings.bootAnimationEnabled);
  }
  drawSettingsBack();
}

uint8_t brightnessPercent() { return settings.brightnessIndex * 10; }

void drawSettingsPage() {
  if (settingsView != SETTINGS_HOME) {
    drawSettingsControlPage();
    return;
  }
  paintPageBackdrop();
  drawCentered("DEVICE SETTINGS", 18, 3, COLOR_MINT);
  drawCentered(settingsGridPage == 0 ? "CORE CONTROLS // SWIPE UP" :
                                      "SYSTEM OPTIONS // SWIPE DOWN",
               50, 1, COLOR_CYAN);

  if (settingsGridPage == 0) {
    char brightness[12];
    snprintf(brightness, sizeof(brightness), "%u%%", brightnessPercent());
    drawSettingsTile(0, 18, 78, "BRIGHTNESS", brightness);
    drawSettingsTile(1, 190, 78, "IDLE TIMER", SLEEP_LABELS[settings.sleepIndex]);
    drawSettingsTile(2, 18, 227, "VOLUME", VOLUME_LABELS[settings.volumeIndex]);
    drawSettingsTile(3, 190, 227, "WAKE MODE", WAKE_LABELS[settings.wakeMode]);
  } else {
    drawSettingsTile(4, 18, 78, "THEME", THEME_LABELS[settings.themeIndex]);
    drawSettingsTile(5, 190, 78, "BOOT EFFECT", settings.bootAnimationEnabled ? "ENABLED" : "DISABLED");
    drawSettingsTile(6, 18, 227, "PLAYER ID", "VIEW IDENTITY");
    drawSettingsTile(7, 190, 227, "FIRMWARE", "CHECK UPDATE");
  }
  drawCentered(settingsGridPage == 0 ? "1  /  2" : "2  /  2", 382, 1, COLOR_MUTED);
  drawPageDots(PAGE_SETTINGS);
}


void drawGenomeLabPage() {
  static const char *bodyNames[] = {"QUADRUPED", "HUMANOID", "AVIAN", "BLOB", "SERPENT"};
  static const char *temperaments[] = {"CALM", "BOLD", "CURIOUS", "LOYAL", "WILD", "CLEVER"};
  paintPageBackdrop();
  drawCentered("GENOME LAB", 15, 3, COLOR_MINT);
  drawCentered(hasCopiedGenome ? "COPIED GENOME READY" :
                                "IMPORT A GENOME TO CLONE OR BLEND",
               48, 1, COLOR_CYAN);

  drawPanelGlow(20, 74, 328, 244, 28, COLOR_PURPLE);
  display->drawRoundRect(20, 74, 328, 244, 28, COLOR_CARD);
  display->drawRoundRect(27, 81, 314, 230, 23, COLOR_PURPLE);
  drawEgg(animationFrame, COLOR_BACKGROUND);

  char fingerprint[28];
  snprintf(fingerprint, sizeof(fingerprint), "DESIGN %016llX",
           static_cast<unsigned long long>(petGenomeDesignId(pet.genome)));
  display->fillRoundRect(22, 325, 324, 43, 13, COLOR_CARD);
  display->setTextSize(1);
  display->setTextColor(COLOR_TEXT);
  display->setCursor(34, 337);
  display->printf("%s  //  %s", eggLineageName(pet.genome.lineage),
                  elementName(pet.genome.element));
  display->setTextColor(COLOR_MUTED);
  display->setCursor(34, 352);
  display->printf("%s  %s  %s", bodyNames[pet.genome.bodyType % 5],
                  temperaments[pet.genome.temperament % 6], fingerprint);

  const bool confirming = newEggConfirmation && millis() < newEggConfirmationUntil;
  const char *labels[] = {"RANDOM", "CLONE", "BLEND"};
  for (uint8_t i = 0; i < 3; ++i) {
    const uint8_t mode = i + 1;
    const int16_t x = 11 + i * 119;
    const bool available = mode == 1 || hasCopiedGenome;
    const bool selected = confirming && pendingHatchMode == mode;
    const uint16_t fill = !available ? COLOR_MUTED :
                          (selected ? COLOR_DANGER : COLOR_PURPLE);
    display->fillRoundRect(x, 378, 108, 46, 13, fill);
    // Size 2 (was 1) -- "CONFIRM", the longest of these, is 84px at size 2
    // and fits easily inside this 108px-wide button.
    drawCenteredInRect(selected ? "CONFIRM" : labels[i], x, 378, 108, 46,
                       2, readableTextColor(fill));
  }
  drawPageDots(PAGE_GENOME_LAB);
}


static const char *kGenomeBodyNames[] = {"QUADRUPED", "HUMANOID", "AVIAN", "BLOB", "SERPENT"};
static const char *kGenomeTemperaments[] = {"CALM", "BOLD", "CURIOUS", "LOYAL", "WILD", "CLEVER"};

// The identity sub-view -- see genomeProfileShowingData's own comment in
// ui_pages.h. Six rows, one trait each, all label-size-matches-value (see
// docs/style-guide.md's Typography section) -- replaces the old single
// panel that crammed two traits per row together with "//" because there
// wasn't room for six rows at a readable size. There is now: this is the
// exact row geometry drawBattleStatsView() uses (kFirstRowY=96,
// kRowPitch=36, panel 22,76,324,260), just with genome traits instead of
// battle stats.
void drawGenomeIdentityView() {
  drawCentered("IDENTITY", 51, 1, COLOR_CYAN);

  drawPanelGlow(22, 76, 324, 260, 26, COLOR_CYAN);
  display->fillRoundRect(22, 76, 324, 260, 26, COLOR_CARD);

  char traitCount[4];
  snprintf(traitCount, sizeof(traitCount), "%u", __builtin_popcount(pet.genome.featureGenes));

  const char *labels[] = {"FORM", "ELEMENT", "BODY", "TEMPERAMENT", "TRAITS", "MUTATION"};
  const char *values[] = {
      STAGE_NAMES[pet.stage],
      elementName(pet.genome.element),
      kGenomeBodyNames[pet.genome.bodyType % 5],
      kGenomeTemperaments[pet.genome.temperament % 6],
      traitCount,
      pet.genome.mutationGenes ? "RARE" : "STABLE",
  };
  constexpr int16_t kFirstRowY = 96;
  constexpr int16_t kRowPitch = 36;
  for (uint8_t i = 0; i < 6; ++i) {
    const int16_t y = kFirstRowY + i * kRowPitch;
    display->setTextSize(2);
    display->setTextColor(COLOR_MUTED);
    display->setCursor(46, y - 7);
    display->print(labels[i]);
    drawCenteredInRect(values[i], 190, y - 7, 136, 22, 2, COLOR_TEXT);
  }

  drawCentered("SWIPE UP FOR GENOME DATA", 356, 1, COLOR_MUTED);
}

// The genome-data sub-view -- the 60-character code (now at size 2, was 1
// -- "IPMFC25BCC7E" etc. is exactly the kind of string that needs to be
// unambiguous, not just legible), design ID, and the EXPORT/IMPORT COPY
// actions (also now both size 2 -- they were a 1-vs-2 mismatch before,
// the same bug class as drawBattlingLayout()'s old COPY GENOME/RETURN
// pair). BACK button hit-test region (main.cpp) keys off this exact
// 84,398,200,36 rect, same as before this page's redesign.
void drawGenomeDataView() {
  drawCentered(genomeTransferStatus, 51, 1, COLOR_CYAN);

  char code[PET_GENOME_CODE_LENGTH + 1]{};
  encodePetGenome(pet.genome, code, sizeof(code));
  drawPanelGlow(14, 82, 340, 116, 22, COLOR_PURPLE);
  display->fillRoundRect(14, 82, 340, 116, 22, COLOR_CARD);
  display->setTextSize(2);
  display->setTextColor(COLOR_CYAN);
  for (uint8_t row = 0; row < 3; ++row) {
    char segment[21]{};
    memcpy(segment, code + row * 20, 20);
    drawCenteredInRect(segment, 14, 92 + row * 34, 340, 24, 2, COLOR_CYAN);
  }

  char designLine[32];
  snprintf(designLine, sizeof(designLine), "DESIGN %016llX",
           static_cast<unsigned long long>(petGenomeDesignId(pet.genome)));
  drawCentered(designLine, 218, 1, COLOR_MUTED);

  drawCentered("SWIPE DOWN FOR IDENTITY", 244, 1, COLOR_MUTED);

  display->fillRoundRect(18, 270, 160, 46, 14, COLOR_PURPLE);
  display->fillRoundRect(190, 270, 160, 46, 14, COLOR_CYAN);
  drawCenteredInRect("EXPORT", 18, 270, 160, 46, 2, readableTextColor(COLOR_PURPLE));
  drawCenteredInRect("IMPORT COPY", 190, 270, 160, 46, 2, readableTextColor(COLOR_CYAN));

  display->fillRoundRect(84, 398, 200, 36, 12, COLOR_CARD);
  drawCenteredInRect("BACK", 84, 398, 200, 36, 2, COLOR_TEXT);
}

void drawGenomeProfilePage() {
  paintPageBackdrop();
  drawCentered("GENOME PROFILE", 18, 3, COLOR_MINT);
  if (genomeProfileShowingData) {
    drawGenomeDataView();
  } else {
    drawGenomeIdentityView();
  }
}


// Hand-pixelled to match STAT_ICONS' style: a blade for Attack, a shield
// outline for Defend, a burst for Special, and an exit arrow for Flee.

const uint16_t BATTLE_MOVE_ICONS[4][16] PROGMEM = {
  {0x00E0, 0x0180, 0x0300, 0x0600, 0x0C00, 0x1800, 0x3000, 0x6000,
   0xC000, 0xFF80, 0x1800, 0x1800, 0x1800, 0x3C00, 0x1800, 0x0000},  // attack
  {0x0000, 0x3FF0, 0x7FF8, 0xFFFC, 0xFFFC, 0xFFFC, 0xFFFC, 0x7FF8,
   0x7FF8, 0x3FF0, 0x3FF0, 0x1FE0, 0x0FC0, 0x0780, 0x0300, 0x0000},  // defend
  {0x0080, 0x0080, 0x01C0, 0x01C0, 0x03E0, 0xC7F1, 0x67F3, 0x3FFE,
   0x3FFE, 0x67F3, 0xC7F1, 0x03E0, 0x01C0, 0x01C0, 0x0080, 0x0000},  // special
  {0x0000, 0x07F0, 0x0800, 0x1000, 0x2000, 0x41FE, 0xFF82, 0x4104,
   0x21FC, 0x1000, 0x0800, 0x07F0, 0x0000, 0x0000, 0x0000, 0x0000},  // flee
};


void drawBattleMoveIcon(BattleMoveIcon icon, int16_t x, int16_t y, uint16_t color) {
  drawGlyph16(BATTLE_MOVE_ICONS[icon], x, y, color);
}


void drawOpponentRow(const FamiliarBattleOpponent &opponent, int16_t y) {
  drawPanelGlow(24, y, 320, 68, 18, COLOR_CYAN);
  display->fillRoundRect(24, y, 320, 68, 18, COLOR_CARD);
  display->drawRoundRect(24, y, 320, 68, 18, COLOR_CYAN);
  display->setTextSize(2);
  display->setTextColor(COLOR_TEXT);
  display->setCursor(44, y + 14);
  display->print(opponent.stageIndex < 5 ? STAGE_NAMES[opponent.stageIndex] : "UNKNOWN");
  display->setTextSize(1);
  display->setTextColor(COLOR_MUTED);
  display->setCursor(44, y + 42);
  display->printf("LEVEL %u", opponent.level);
  const uint8_t bars = opponent.rssi > -60 ? 3 : opponent.rssi > -75 ? 2 : 1;
  for (uint8_t b = 0; b < 3; ++b) {
    const int16_t bh = 8 + b * 6;
    display->fillRect(298 + b * 10, y + 50 - bh, 6, bh,
                      b < bars ? COLOR_MINT : COLOR_MUTED);
  }
}

// The Find picker: a scrollable (swipe up/down) list of scan results instead
// of auto-connecting to the first one found. Takes the result vector as a
// parameter rather than reading battle.scanResults() directly so the exact
// same drawing code can be previewed with synthetic data (DUMPSCAN).

void drawBattleResultsPage(const std::vector<FamiliarBattleOpponent> &results,
                           uint8_t page) {
  paintPageBackdrop();
  drawCentered("SELECT OPPONENT", 20, 2, COLOR_MINT);
  const uint8_t totalPages = max<uint8_t>(
      1, (results.size() + kBattleResultsPerPage - 1) / kBattleResultsPerPage);
  const uint8_t start = page * kBattleResultsPerPage;
  for (uint8_t i = 0; i < kBattleResultsPerPage; ++i) {
    const size_t index = start + i;
    if (index >= results.size()) break;
    drawOpponentRow(results[index], 66 + i * 76);
  }
  if (totalPages > 1) {
    char pageLabel[12];
    snprintf(pageLabel, sizeof(pageLabel), "%u / %u", page + 1, totalPages);
    drawCentered(pageLabel, 300, 1, COLOR_MUTED);
    drawCentered("SWIPE FOR MORE", 320, 1, COLOR_MUTED);
  }
  display->fillRoundRect(84, 385, 200, 43, 14, COLOR_CARD);
  display->drawRoundRect(84, 385, 200, 43, 14, COLOR_CYAN);
  drawCenteredInRect("CANCEL", 84, 385, 200, 43, 2, COLOR_TEXT);
  // No drawPageDots() here: this button sits at the same (84, 385, 200, 43)
  // geometry as drawSettingsControlPage()'s own back button, which omits the
  // dots for exactly this reason -- drawPageDots(PAGE_BATTLE) draws at
  // y=425 with radius 4 (spanning y:421-429), overlapping this button's
  // y:385-428 span. Was previously called anyway (an inconsistency with the
  // Settings sub-view, not a deliberate difference), producing a page-dot
  // row visibly overlapping the Cancel button.
}

// A small icon-and-glow button, matching the settings tile treatment, used
// for both the Idle state's HOST/FIND choice and (elsewhere) move buttons.

void drawBattleButton(int16_t x, int16_t y, int16_t w, int16_t h, int16_t radius,
                      uint16_t color, BattleMoveIcon icon, const char *label) {
  drawPanelGlow(x, y, w, h, radius, color);
  display->fillRoundRect(x, y, w, h, radius, color);
  const uint16_t labelColor = readableTextColor(color);
  drawBattleMoveIcon(icon, x + w / 2 - 16, y + 6, labelColor);
  display->setTextSize(2);
  display->setTextColor(labelColor);
  drawCenteredInRect(label, x, y + h - 20, w, 18, 2, labelColor);
}

// Shared "in battle" layout for both the live Battling/Result states and the
// DUMPBATTLE debug preview -- takes every value as a parameter rather than
// reading the live `battle` object, so the exact same drawing code can be
// exercised with synthetic data for visual verification without a live BLE
// match (which needs two physical devices to test at all).

void drawBattlingLayout(uint16_t myHp, uint16_t myMaxHp, uint16_t opponentHp,
                        uint16_t opponentMaxHp, uint8_t opponentLevel,
                        bool enhancedLink, const char *logLine1, const char *logLine2,
                        bool isResult, bool moveSubmitted, bool fleeArmed,
                        bool opponentGenomeAvailable, bool genomeCopied,
                        const PetGenome *opponentGenome) {
  const PetPalette myPalette = paletteForGenome(pet.genome);
  drawCreaturePortrait(pet.genome, max<uint8_t>(pet.stage, 1), 90, 96, 62, myPalette.glow);
  if (opponentGenome) {
    drawCreaturePortrait(*opponentGenome, 4, 278, 96, 62, COLOR_DANGER);
  } else {
    // Opponent genome hasn't finished arriving over BLE yet -- a neutral
    // placeholder rather than guessing at their creature's appearance.
    display->fillCircle(278, 96, 43, scaleRgb565(COLOR_DANGER, 16));
    display->drawCircle(278, 96, 41, COLOR_DANGER);
    display->fillCircle(278, 96, 35, COLOR_CARD);
    display->setTextSize(3);
    display->setTextColor(COLOR_MUTED);
    display->setCursor(270, 80);
    display->print("?");
  }

  display->setTextSize(1);
  display->setTextColor(COLOR_TEXT);
  display->setCursor(74, 138); display->print("YOU");
  display->setCursor(240, 138); display->printf("OPP LV%u", opponentLevel);

  auto hpBar = [&](int16_t x, uint16_t hp, uint16_t maxHp, uint16_t color) {
    display->drawRoundRect(x, 150, 144, 17, 7, COLOR_MUTED);
    if (hp && maxHp) {
      const int16_t filled = 138 * hp / maxHp;
      display->fillRoundRect(x + 3, 153, filled, 11, 5, color);
      if (filled > 6) {
        display->fillCircle(x + 3 + filled - 6, 158, 5,
                            lerpRgb565(color, RGB565_WHITE, 0.35f));
      }
    }
  };
  const uint16_t myHpColor =
      myHp * 100 / max<uint16_t>(1, myMaxHp) < 30 ? COLOR_DANGER : COLOR_MINT;
  const uint16_t oppHpColor =
      opponentHp * 100 / max<uint16_t>(1, opponentMaxHp) < 30 ? COLOR_WARNING : COLOR_DANGER;
  hpBar(24, myHp, myMaxHp, myHpColor);
  hpBar(200, opponentHp, opponentMaxHp, oppHpColor);
  display->setCursor(24, 172); display->printf("%u / %u", myHp, myMaxHp);
  display->setCursor(268, 172); display->printf("%u / %u", opponentHp, opponentMaxHp);
  drawCentered(enhancedLink ? "ENHANCED LINK" : "CORE LINK", 188, 1,
              enhancedLink ? COLOR_MINT : COLOR_MUTED);

  if (!isResult) {
    // A compact two-line log ("you did this, they did that") instead of a
    // big decorative VS panel, freeing up height for a proper 2x2 move
    // grid -- bigger, easier-to-hit buttons instead of four squeezed into
    // one row. Text size bumped to 2 for readability; the log messages
    // themselves (familiar_battle_service.cpp's addLog calls) were shortened
    // to fit at this size.
    drawPanelGlow(24, 196, 320, 72, 18, COLOR_PURPLE);
    display->fillRoundRect(24, 196, 320, 72, 18, COLOR_CARD);
    display->setTextSize(2);
    display->setTextColor(COLOR_MUTED);
    display->setCursor(36, 208);
    display->print(logLine1);
    display->setTextColor(COLOR_TEXT);
    display->setCursor(36, 234);
    display->print(logLine2);

    const char *labels[] = {"ATK", "DEF", "SPEC", fleeArmed ? "CONFIRM?" : "FLEE"};
    const uint16_t colors[] = {COLOR_DANGER, COLOR_CYAN, COLOR_PURPLE,
                               fleeArmed ? COLOR_WARNING : COLOR_MUTED};
    constexpr int16_t colX[] = {24, 194};
    constexpr int16_t rowY[] = {280, 336};
    for (uint8_t i = 0; i < 4; ++i) {
      drawBattleButton(colX[i % 2], rowY[i / 2], 150, 50, 14, colors[i],
                       static_cast<BattleMoveIcon>(i), labels[i]);
    }
    drawCentered(fleeArmed ? "TAP FLEE AGAIN TO CONFIRM" :
                moveSubmitted ? "WAITING FOR OPPONENT" : "SELECT MOVE", 396, 1,
                fleeArmed ? COLOR_WARNING :
                moveSubmitted ? COLOR_WARNING : COLOR_MINT);
    return;
  }

  drawPanelGlow(24, 198, 320, 98, 22, COLOR_PURPLE);
  display->fillRoundRect(24, 198, 320, 98, 22, COLOR_CARD);
  drawCentered("BATTLE COMPLETE", 224, 3, COLOR_WARNING);
  drawCentered(logLine2, 278, 1, COLOR_TEXT);

  if (opponentGenomeAvailable) {
    drawPanelGlow(18, 305, 160, 54, 14, COLOR_PURPLE);
    drawPanelGlow(190, 305, 160, 54, 14, COLOR_CYAN);
    display->fillRoundRect(18, 305, 160, 54, 14, COLOR_PURPLE);
    display->fillRoundRect(190, 305, 160, 54, 14, COLOR_CYAN);
    // Both buttons at size 2 -- was 1 vs. 2, an inconsistency between two
    // equally-weighted CTAs on the same row with no reason for one to
    // read smaller than the other. "COPY GENOME", the longer of the two
    // labels either button ever shows, is 132px at size 2 and fits inside
    // this 160px-wide button.
    drawCenteredInRect(genomeCopied ? "COPIED" : "COPY GENOME", 18, 305, 160, 54, 2,
                       readableTextColor(COLOR_PURPLE));
    drawCenteredInRect("RETURN", 190, 305, 160, 54, 2, readableTextColor(COLOR_CYAN));
  } else {
    drawCentered("TAP TO RETURN", 330, 2, COLOR_CYAN);
  }
}

// The battle-stats sheet -- see battleShowingActions' own comment in
// ui_pages.h. LEVEL/HP/ATTACK/DEFENSE/SPECIAL/TYPE all at once, replacing
// the single cramped "LV 5 HP.. ATK.. DEF.." header line the old one-
// screen Idle state used to draw.
void drawBattleStatsView() {
  drawCentered("BATTLE STATS", 51, 1, COLOR_CYAN);

  drawPanelGlow(22, 76, 324, 260, 26, COLOR_CYAN);
  display->fillRoundRect(22, 76, 324, 260, 26, COLOR_CARD);

  // Mirrors main.cpp's battleCapabilities() special-value formula -- the
  // actual value sent into a real battle. Unlike HP/ATK/DEF just below,
  // there's no shared free function for this one to call instead
  // (battleCapabilities() computes it inline), so this one small pure
  // formula really is duplicated -- same reasoning the FNV-1a hash
  // main.cpp/familiar_battle_service.cpp each carry their own copy of.
  const uint8_t special = constrain(25 + pet.genome.faceGene + pet.stage * 9 +
                                    pet.training / 4 + pet.bonusSpecial, 1, 100);

  char hp[6], atk[5], def[5], specialText[5];
  snprintf(hp, sizeof(hp), "%u", battleRulesDeriveMaxHp(5) + pet.bonusHp);
  snprintf(atk, sizeof(atk), "%u", battleRulesDeriveAttack(5, pet.stage) + pet.bonusAttack);
  snprintf(def, sizeof(def), "%u", battleRulesDeriveDefense(5, pet.stage) + pet.bonusDefense);
  snprintf(specialText, sizeof(specialText), "%u", special);

  const char *labels[] = {"LEVEL", "HP", "ATTACK", "DEFENSE", "SPECIAL", "TYPE"};
  const char *values[] = {"5", hp, atk, def, specialText, elementName(pet.genome.element)};
  constexpr int16_t kFirstRowY = 96;
  constexpr int16_t kRowPitch = 36;
  // Label and value are the same size 2 -- see docs/style-guide.md's
  // typography section: this was the exact size-1-label-next-to-size-2-
  // value mismatch reported as hard to read. "DEFENSE", the longest
  // label, is 84px at size 2 starting from x=46, comfortably inside the
  // gap before the value column starts at x=190.
  for (uint8_t i = 0; i < 6; ++i) {
    const int16_t y = kFirstRowY + i * kRowPitch;
    display->setTextSize(2);
    display->setTextColor(COLOR_MUTED);
    display->setCursor(46, y - 7);
    display->print(labels[i]);
    drawCenteredInRect(values[i], 190, y - 7, 136, 22, 2, COLOR_TEXT);
  }

  char recordLine[32];
  snprintf(recordLine, sizeof(recordLine), "RECORD %luW - %luL",
           (unsigned long)battleStats.wins, (unsigned long)battleStats.losses);
  drawCentered(recordLine, 312, 1, COLOR_MUTED);

  drawCentered("SWIPE UP FOR BATTLE ACTIONS", 356, 1, COLOR_MUTED);
}

// Three ascending bars (a podium/leaderboard look, echoing
// drawOpponentRow's own RSSI-bar shape) for the RIVALS action tile.
void drawRivalsIcon(int16_t cx, int16_t cy, uint16_t color) {
  display->fillRect(cx - 18, cy + 4, 10, 14, color);
  display->fillRect(cx - 5, cy - 6, 10, 24, color);
  display->fillRect(cx + 8, cy, 10, 18, color);
}

// Two overlapping circles -- a conventional "people" glyph -- for the
// FRIENDS action tile.
void drawFriendsIcon(int16_t cx, int16_t cy, uint16_t color) {
  display->drawCircle(cx - 8, cy, 14, color);
  display->drawCircle(cx + 8, cy, 14, color);
}

// Same chrome as drawActionTile() (Status page's own action grid) --
// deliberately a separate function rather than a shared one: this grid's
// icon set (battle-move glyphs, RIVALS' bars, FRIENDS' circles) doesn't
// fit drawActionTile()'s existing action-index-to-icon mapping, and
// growing that one to cover both would mean overloading the same index
// space with two unrelated meanings depending on which page called it.
void drawBattleActionTile(BattleGridTile tile, int16_t x, int16_t y, const char *label,
                          const char *value) {
  const uint16_t accent = (tile == GRID_HOST || tile == GRID_RIVALS) ? COLOR_PURPLE : COLOR_CYAN;
  drawPanelGlow(x, y, 160, 137, 20, accent);
  display->fillRoundRect(x, y, 160, 137, 20, COLOR_CARD);
  display->drawRoundRect(x, y, 160, 137, 20, accent);
  switch (tile) {
    case GRID_HOST: drawBattleMoveIcon(ICON_DEFEND, x + 64, y + 18, COLOR_MINT); break;
    case GRID_FIND: drawBattleMoveIcon(ICON_SPECIAL, x + 64, y + 18, COLOR_MINT); break;
    case GRID_RIVALS: drawRivalsIcon(x + 80, y + 43, COLOR_MINT); break;
    case GRID_FRIENDS: drawFriendsIcon(x + 80, y + 43, COLOR_MINT); break;
  }
  // Same label-is-size-2/value-is-size-1 "title/subtitle" role split as
  // drawActionTile() -- see that function's own comment.
  display->setTextSize(2);
  display->setTextColor(COLOR_TEXT);
  display->setCursor(x + 16, y + 82);
  display->print(label);
  display->setTextSize(1);
  display->setTextColor(COLOR_MUTED);
  display->setCursor(x + 16, y + 104);
  display->print(value);
}

// The HOST/FIND/RIVALS/FRIENDS action grid -- see battleShowingActions'
// own comment in ui_pages.h. Hit-test regions (main.cpp's
// handleBattleGridTap()) key off this exact 18/190,78/227 pair of rects,
// same relationship the Status action grid's tiles have with
// handleStatusGridTap().
void drawBattleActionsView() {
  drawCentered("SELECT AN ACTION // SWIPE DOWN", 51, 1, COLOR_CYAN);

  drawBattleActionTile(GRID_HOST, 18, 78, "HOST", "OPEN SIGNAL");
  drawBattleActionTile(GRID_FIND, 190, 78, "FIND", "SCAN FOR HOSTS");
  char recordValue[16];
  snprintf(recordValue, sizeof(recordValue), "%luW-%luL", (unsigned long)battleStats.wins,
           (unsigned long)battleStats.losses);
  drawBattleActionTile(GRID_RIVALS, 18, 227, "RIVALS", recordValue);
  char friendsValue[16];
  snprintf(friendsValue, sizeof(friendsValue), "%u ADDED", friendsList.count);
  drawBattleActionTile(GRID_FRIENDS, 190, 227, "FRIENDS", friendsValue);
}



// A full-screen overlay, not one of the 5 swipeable pages -- see this
// function's own declaration in ui_pages.h. playerId is shown as raw hex
// (there's no friendly nickname exchanged over BLE, just the same 32-bit
// identity WHOAMI and the Player ID screen already surface as hex).
void drawRivalsPage() {
  paintPageBackdrop();
  drawCentered("RIVALS", 18, 3, COLOR_MINT);
  drawCentered("WIN-LOSS BY OPPONENT", 51, 1, COLOR_CYAN);

  drawPanelGlow(20, 70, 328, 310, 26, COLOR_PURPLE);
  display->fillRoundRect(20, 70, 328, 310, 26, COLOR_CARD);
  display->drawRoundRect(27, 77, 314, 296, 21, COLOR_PURPLE);

  if (battleStats.rivalCount == 0) {
    drawCentered("NO RIVALS YET", 205, 2, COLOR_TEXT);
    drawCentered("BATTLE SOMEONE TO START", 234, 1, COLOR_MUTED);
  } else {
    constexpr int16_t kFirstRowY = 92;
    constexpr int16_t kRowPitch = 35;
    for (uint8_t i = 0; i < battleStats.rivalCount; ++i) {
      const BattleRival &rival = battleStats.rivals[i];
      const int16_t y = kFirstRowY + i * kRowPitch;
      display->setTextSize(2);
      display->setTextColor(COLOR_TEXT);
      display->setCursor(46, y);
      display->printf("%08lX", (unsigned long)rival.playerId);
      display->setTextColor(rival.wins >= rival.losses ? COLOR_MINT : COLOR_DANGER);
      display->setCursor(216, y);
      display->printf("%uW-%uL", rival.wins, rival.losses);
    }
  }

  drawCentered("TAP TO RETURN", 396, 2, COLOR_CYAN);
}

// A full-screen overlay, opened from the Status page's action grid's
// RECON LOG tile -- see this function's own prototype comment in
// ui_pages.h. Same layout as drawRivalsPage() just above (empty state,
// two-column rows, "TAP TO RETURN") since it's the same kind of screen: a
// small persistent discovery log, not a live view of anything.
void drawReconLogPage() {
  paintPageBackdrop();
  drawCentered("RECON LOG", 18, 3, COLOR_MINT);
  drawCentered("SIGNALS DISCOVERED", 51, 1, COLOR_CYAN);

  drawPanelGlow(20, 70, 328, 310, 26, COLOR_PURPLE);
  display->fillRoundRect(20, 70, 328, 310, 26, COLOR_CARD);
  display->drawRoundRect(27, 77, 314, 296, 21, COLOR_PURPLE);

  if (reconLog.entryCount == 0) {
    drawCentered("NO SIGNALS YET", 205, 2, COLOR_TEXT);
    drawCentered("FEED YOUR PET TO SCAN", 234, 1, COLOR_MUTED);
  } else {
    constexpr int16_t kFirstRowY = 92;
    constexpr int16_t kRowPitch = 35;
    for (uint8_t i = 0; i < reconLog.entryCount; ++i) {
      const ReconEntry &entry = reconLog.entries[i];
      const int16_t y = kFirstRowY + i * kRowPitch;
      display->setTextSize(2);
      display->setTextColor(COLOR_TEXT);
      display->setCursor(46, y);
      display->print(entry.kind == 0 && entry.label[0] ? entry.label
                     : entry.kind == 0 ? "HIDDEN AP" : "BLE DEVICE");
      display->setTextColor(entry.kind == 0 ? COLOR_CYAN : COLOR_PURPLE);
      display->setCursor(272, y);
      display->print(entry.kind == 0 ? "WIFI" : "BLE");
    }
  }

  drawCentered("TAP TO RETURN", 396, 2, COLOR_CYAN);
}

// A full-screen overlay, opened from a tappable line on the Battle Idle
// screen -- see this function's own prototype comment in ui_pages.h for
// why it's not purely a passive list like Rivals/Recon Log. HOST/FIND
// button hit-test regions (main.cpp) key off this exact 18/190,378 pair of
// rects, same relationship drawBattlePage()'s own comment on its Idle
// state's HOST/FIND rects has with handleBattleTap().
void drawFriendsPage() {
  paintPageBackdrop();
  drawCentered("FRIENDS", 18, 3, COLOR_MINT);
  drawCentered("ADDED VIA EXCHANGE ID", 51, 1, COLOR_CYAN);

  drawPanelGlow(20, 70, 328, 296, 26, COLOR_PURPLE);
  display->fillRoundRect(20, 70, 328, 296, 26, COLOR_CARD);
  display->drawRoundRect(27, 77, 314, 282, 21, COLOR_PURPLE);

  if (friendsList.count == 0) {
    drawCentered("NO FRIENDS YET", 205, 2, COLOR_TEXT);
    drawCentered("HOST OR FIND BELOW TO EXCHANGE", 234, 1, COLOR_MUTED);
  } else {
    constexpr int16_t kFirstRowY = 92;
    constexpr int16_t kRowPitch = 35;
    for (uint8_t i = 0; i < friendsList.count; ++i) {
      const FriendEntry &friendEntry = friendsList.friends[i];
      const int16_t y = kFirstRowY + i * kRowPitch;
      display->setTextSize(2);
      display->setTextColor(COLOR_TEXT);
      display->setCursor(46, y);
      display->printf("%08lX", (unsigned long)friendEntry.playerId);
    }
  }

  drawBattleButton(18, 378, 154, 54, 16, COLOR_PURPLE, ICON_DEFEND, "HOST");
  drawBattleButton(190, 378, 154, 54, 16, COLOR_CYAN, ICON_SPECIAL, "FIND");
}

// HP/ATK/DEF/SPECIAL reuse existing glyph icons (drawStatIcon/
// drawBattleMoveIcon take top-left coords; drawSettingsIcon takes a
// center, hence the -16 offset on the first three -- see each one's own
// signature) rather than adding new bitmaps, since they're literally the
// same stats those icons already represent elsewhere. TYPE_SHIFT reuses
// the Settings "theme palette" tile icon (multiple colors, already reads
// as "changes what color/kind this is").
void drawItemIcon(uint8_t item, int16_t cx, int16_t cy, uint16_t color) {
  switch (item) {
    case ITEM_HP_BOOST: drawStatIcon(ICON_HEALTH, cx - 16, cy - 16, color); break;
    case ITEM_ATK_BOOST: drawBattleMoveIcon(ICON_ATTACK, cx - 16, cy - 16, color); break;
    case ITEM_DEF_BOOST: drawBattleMoveIcon(ICON_DEFEND, cx - 16, cy - 16, color); break;
    case ITEM_SPECIAL_BOOST: drawBattleMoveIcon(ICON_SPECIAL, cx - 16, cy - 16, color); break;
    default: drawSettingsIcon(4, cx, cy, color); break;
  }
}

// A full-screen overlay, opened from the Status page's action grid's
// (page 2) ITEMS tile -- see this function's own prototype comment in
// ui_pages.h for why it uses an explicit BACK button instead of Rivals/
// Recon Log/Friends' tap-anywhere dismissal. Row hit-test regions
// (main.cpp's handleItemsTap()) key off this exact 20/kFirstRowY,kRowHeight
// grid; the BACK button's off drawSettingsBack()'s own fixed rect.
void drawItemsPage() {
  paintPageBackdrop();
  drawCentered("ITEMS", 18, 3, COLOR_MINT);
  drawCentered("MUTATION TRIGGERS FOUND VIA SCAN", 51, 1, COLOR_CYAN);

  static const char *names[] = {"HP BOOST", "ATK BOOST", "DEF BOOST", "SPECIAL BOOST",
                                "TYPE SHIFT"};
  constexpr int16_t kFirstRowY = 76;
  constexpr int16_t kRowHeight = 52;
  for (uint8_t i = 0; i < kItemTypeCount; ++i) {
    const int16_t y = kFirstRowY + i * kRowHeight;
    const bool available = inventory.counts[i] > 0;
    const uint16_t accent = i & 1 ? COLOR_PURPLE : COLOR_CYAN;
    const uint16_t fill = available ? COLOR_CARD : scaleRgb565(COLOR_CARD, 55);
    display->fillRoundRect(20, y, 328, 46, 14, fill);
    display->drawRoundRect(20, y, 328, 46, 14, available ? accent : COLOR_MUTED);
    drawItemIcon(i, 46, y + 23, available ? accent : COLOR_MUTED);

    // Name is size 2 (was 1) -- the row's actual title, same "title" role
    // drawActionTile()'s own label plays; description stays size 1 as its
    // subtitle. "SPECIAL BOOST", the longest name, is 156px at size 2 and
    // fits in the ~194px gap before the count column at x=268.
    display->setTextSize(2);
    display->setTextColor(available ? COLOR_TEXT : COLOR_MUTED);
    display->setCursor(74, y + 8);
    display->print(names[i]);
    display->setTextSize(1);
    display->setTextColor(COLOR_MUTED);
    display->setCursor(74, y + 32);
    if (i == ITEM_TYPE_SHIFT) {
      display->print("REROLLS ELEMENT");
    } else {
      const uint8_t currentBonus = i == ITEM_HP_BOOST ? pet.bonusHp :
                                   i == ITEM_ATK_BOOST ? pet.bonusAttack :
                                   i == ITEM_DEF_BOOST ? pet.bonusDefense : pet.bonusSpecial;
      display->printf("+2 (CURRENT +%u)", currentBonus);
    }

    char countText[6];
    snprintf(countText, sizeof(countText), "x%u", inventory.counts[i]);
    drawCenteredInRect(countText, 268, y, 66, 46, 2, available ? COLOR_MINT : COLOR_MUTED);
  }

  drawSettingsBack();
}
