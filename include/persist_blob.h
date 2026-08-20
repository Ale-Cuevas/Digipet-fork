#pragma once

#include <Preferences.h>

// A "magic-guarded blob": a fixed-size struct whose first member is a
// uint32_t magic, saved/loaded as raw bytes under one Preferences key. This
// is the shape pet/battleStats/reconLog/friendsList/inventory/settings/
// copiedGenome already hand-roll independently (same ~15 lines, copy-pasted
// per struct) -- collapses each pair to a couple of lines.
//
// Deliberately NOT used by savePet()/loadPet(): that pair carries several
// generations of migration structs with field-by-field remapping, not a
// single exact-sizeof match, and forcing a schema-migrating load through a
// same-shape-in-same-shape-out helper like this one would either strip the
// migration behavior or require a generic "migrate N->N+1" mechanism this
// codebase has exactly one caller for -- not worth designing until there's
// a second one to validate it against.
template <typename T>
bool loadBlob(Preferences &prefs, const char *key, T &out, uint32_t expectedMagic) {
  if (prefs.getBytesLength(key) == sizeof(T)) prefs.getBytes(key, &out, sizeof(T));
  return out.magic == expectedMagic;  // false -- caller resets to default
}

template <typename T>
void saveBlob(Preferences &prefs, const char *key, const T &value) {
  prefs.putBytes(key, &value, sizeof(T));
}
