#pragma once

#include <cstddef>
#include <cstdlib>

inline bool psramFound() { return true; }
inline void *ps_malloc(size_t size) { return malloc(size); }
