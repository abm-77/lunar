#pragma once

#include <cstddef>
#include <cstdint>

// fnv1a64 — FNV-1a 64-bit hash. used for asset IDs: fnv1a64("Name.umsh", 10).
inline uint64_t fnv1a64(const char *s, size_t n) {
  uint64_t h = 14695981039346656037ULL;
  for (size_t i = 0; i < n; ++i)
    h = (h ^ (uint8_t)s[i]) * 1099511628211ULL;
  return h;
}
