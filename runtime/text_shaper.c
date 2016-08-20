#include "text_shaper.h"
#include "font_ranges.h"
#include "text_shaper_lut.h"
#include "utf8.h"

typedef enum ShaperState { STATE_INITIAL, STATE_MEDIAL } ShaperState;

typedef struct ShaperLUTEntry {
  uint16_t true_codept;
  uint16_t isolated_codept;
  uint16_t initial_codept;
  uint16_t medial_codept;
  uint16_t final_codept;
} ShaperLUTEntry;

const int LIG_REPLACEMENT_CODEPT_MASK = (1 << 15);
// NB much of this file assumes we'll be shaping 2-byte runes to other 2-byte
// runes.
const int RUNE_SIZE = 2;

static const ShaperLUTEntry *find_lut_entry_by_codept(uint16_t codept) {
  if (codept < 0x600 && codept > 0x6ff) {
    return NULL;
  }
  const ShaperLUTEntry *shaper_lut = (ShaperLUTEntry *)ARABIC_SHAPER_LUT;
  for (int i = 0; i < ARABIC_SHAPER_LUT_SIZE / sizeof(ShaperLUTEntry); ++i) {
    if (shaper_lut[i].true_codept == codept) {
      return &shaper_lut[i];
    }
  }
  return NULL;
}

static uint16_t find_base_codept_by_shaped(uint16_t codept) {
  const ShaperLUTEntry *shaper_lut = (ShaperLUTEntry *)ARABIC_SHAPER_LUT;
  for (int i = 0; i < ARABIC_SHAPER_LUT_SIZE / sizeof(ShaperLUTEntry); ++i) {
    if (shaper_lut[i].isolated_codept == codept ||
        shaper_lut[i].initial_codept == codept ||
        shaper_lut[i].medial_codept == codept ||
        shaper_lut[i].final_codept == codept) {
      return shaper_lut[i].true_codept;
    }
  }
  return 0;
}

static uint16_t find_ligature_by_codepts(uint16_t *pattern,
                                         size_t pattern_size) {
  bool searching = true;
  size_t pattern_idx = 0;
  const uint16_t *ligature_lut = (uint16_t *)ARABIC_LIGATURE_LUT;
  for (int i = 0; i < ARABIC_LIGATURE_LUT_SIZE / sizeof(uint16_t); ++i) {
    if (searching) {
      if (ligature_lut[i] == pattern[pattern_idx] &&
          pattern_idx < pattern_size) {
        pattern_idx++;
      } else {
        // Is this the replacement codept?
        if (ligature_lut[i] & LIG_REPLACEMENT_CODEPT_MASK) {
          return ligature_lut[i] & ~LIG_REPLACEMENT_CODEPT_MASK;
        }
        searching = false;
        pattern_idx = 0;
      }
    } else if (ligature_lut[i] & LIG_REPLACEMENT_CODEPT_MASK) {
      searching = true;
    }
  }
  return 0;
}

static bool expand_ligature(uint16_t codept, char *ptr) {
  const uint16_t *ligature_lut = (uint16_t *)ARABIC_LIGATURE_LUT;
  int last_pattern_start = 0;
  for (int i = 0; i < ARABIC_LIGATURE_LUT_SIZE / sizeof(uint16_t); ++i) {
    if (ligature_lut[i] & LIG_REPLACEMENT_CODEPT_MASK) {
      if ((ligature_lut[i] & ~LIG_REPLACEMENT_CODEPT_MASK) == codept) {
        // Write out the original pattern that produced this ligature.
        ptr -= RUNE_SIZE * (i - last_pattern_start - 1);
        while (
            !(ligature_lut[last_pattern_start] & LIG_REPLACEMENT_CODEPT_MASK)) {
          write_utf8(ptr, ligature_lut[last_pattern_start++]);
          ptr += RUNE_SIZE;
        }
        return true;
      }
      last_pattern_start = i + 1;
    }
  }
  return false;
}

bool shape_text(char *text) {
  ShaperState state = STATE_INITIAL;
  bool did_shape = false;
  char *ptr = text;

  const int NEXT_CODEPT = 1;
  const int THIS_CODEPT = 0;

  char *next_codept_ptr = ptr;
  uint16_t codept_buffer[2];
  codept_buffer[NEXT_CODEPT] = read_utf8(&ptr);
  const ShaperLUTEntry *next_lut_entry =
      find_lut_entry_by_codept(codept_buffer[NEXT_CODEPT]);
  char *this_codept_ptr = NULL, *last_codept_ptr = NULL;
  int ligature_span = 0;
  const ShaperLUTEntry *this_lut_entry;
  do {
    // Read forward one.
    last_codept_ptr = this_codept_ptr;
    codept_buffer[THIS_CODEPT] = codept_buffer[NEXT_CODEPT];
    this_codept_ptr = next_codept_ptr;
    this_lut_entry = next_lut_entry;
    if (*ptr) {
      next_codept_ptr = ptr;
      codept_buffer[NEXT_CODEPT] = read_utf8(&ptr);

      // Check ligature state.
      uint16_t lig_codept = find_ligature_by_codepts(codept_buffer, 2);
      if (lig_codept) {
        codept_buffer[NEXT_CODEPT] = lig_codept;
        // This only works with 2-char ligatures.
        ligature_span = 1;
      }

      next_lut_entry = find_lut_entry_by_codept(codept_buffer[NEXT_CODEPT]);
    } else {
      codept_buffer[NEXT_CODEPT] = 0;
      next_codept_ptr = 0;
      next_lut_entry = NULL;
    }

    if (ligature_span) {
      ligature_span--;
      write_utf8(this_codept_ptr, ZERO_WIDTH_CODEPT);
    } else if (is_zero_width(codept_buffer[THIS_CODEPT])) {
      // Don't do anything rash.
    } else if (this_lut_entry) {
      did_shape = true;
      if (
          // If we're about to change into an unshapable span, finish up.
          (!next_lut_entry && !is_zero_width(codept_buffer[NEXT_CODEPT])) ||
          // Or, if this character has no medial form.
          (
              // Indicated by identical medial and final forms.
              this_lut_entry->medial_codept == this_lut_entry->final_codept &&
              // Contraindication for stuff like kashida.
              this_lut_entry->isolated_codept !=
                  this_lut_entry->final_codept)) {
        // Final, or isolated form.
        if (state == STATE_INITIAL) {
          write_utf8(this_codept_ptr, this_lut_entry->isolated_codept);
        } else {
          write_utf8(this_codept_ptr, this_lut_entry->final_codept);
        }
        state = STATE_INITIAL;
      } else if (state == STATE_INITIAL) {
        state = STATE_MEDIAL;
        write_utf8(this_codept_ptr, this_lut_entry->initial_codept);
      } else {
        write_utf8(this_codept_ptr, this_lut_entry->medial_codept);
      }
    } else {
      // Not a shapable character - reset the state.
      state = STATE_INITIAL;
    }
  } while (codept_buffer[NEXT_CODEPT]);

  return did_shape;
}

void unshape_text(char *iter) {
  while (*iter) {
    char *iter_pre = iter;
    uint16_t codept = read_utf8(&iter);
    if (!ARABIC_SHAPER_RANGE(codept)) {
      continue;
    }
    uint16_t old_codept = find_base_codept_by_shaped(codept);
    if (old_codept) {
      if (expand_ligature(old_codept, iter_pre)) {
        // Ligature was expanded - nothing left to do.
      } else {
        // Write back the original codept.
        write_utf8(iter_pre, old_codept);
      }
    }
  }
}
