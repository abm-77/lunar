// Stub site table for runtime tests.
// mem.c references __um_sites / __um_sites_count as extern; with count = 0
// the runtime never accesses the array, so a single-element placeholder
// suffices.
#include <stdint.h>

typedef struct {
  const char *file;
  uint32_t line;
  uint32_t col;
} um_site_info_t;

const um_site_info_t __um_sites[1] = {{0, 0, 0}};
const uint32_t __um_sites_count = 0;
