#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common/types.h"

#ifndef UM_DEBUG
#define UM_DEBUG 1
#endif

static const uint64_t SLAB_SIZE = 256 * 1024 * 1024; // 256K Slabs

static const uint64_t MIN_BLOCK_SIZE = 16; // MUST BE POWER OF TWO
static const uint64_t FIRST_POWER_OF_TWO_SIZE =
    128; // PREFER MULTIPLE OF MIN_BLOCK_SIZE
static const uint64_t NUM_SMALL_BLOCKS =
    (FIRST_POWER_OF_TWO_SIZE / MIN_BLOCK_SIZE) - 1;

typedef struct {
  void *mem;
  uint32_t block_size;
} slab_t;

struct free_list_node {
  struct free_list_node *next;

} free_list_node_t;

static inline uint64_t ceil_to_min_block(uint64_t n) {
  return (n + MIN_BLOCK_SIZE - 1) & ~(MIN_BLOCK_SIZE - 1);
}

static inline uint64_t ceil_pow2_u64(uint64_t n) {
  n -= 1;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  n |= n >> 32;
  return n + 1;
}

static inline uint32_t log2_u64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
  return 63 - (uint32_t)__builtin_clzll(x);
#else
  uint32_t r = 0;
  while (x >>= 1) ++r;
  return r;
#endif
}

static size_t page_size() {
  static size_t ps = 0;
  if (!ps) ps = (size_t)sysconf(_SC_PAGESIZE);
  return ps;
}

static size_t round_up_to_pages(size_t n) {
  size_t ps = page_size();
  return (n + ps - 1) & ~(ps - 1);
}

static void *os_alloc_pages(size_t bytes) {
  bytes = round_up_to_pages(bytes);
  void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return (p == MAP_FAILED) ? NULL : p;
}

static void os_free_pages(void *p, size_t bytes) {
  if (!p) return;
  bytes = round_up_to_pages(bytes);
  munmap(p, bytes);
}

static uint32_t size_class(uint64_t n, uint64_t align) {
  if (align > n) n = align;
  if (n == 0) n = MIN_BLOCK_SIZE;

  if (n < NUM_SMALL_BUCKETS * MIN_BLOCK_SIZE) {
  }
}

static slab_t alloc_slab() {
  void *p = os_alloc_pages(SLAB_SIZE);
  if (!p) assert(0 && "System out of memory");
  slab_t s = {.mem = p};
  return s;
}

typedef uint64_t um_alloc_handle_t;

typedef struct {
  void *ptr;
  uint64_t size;
  uint32_t align;
  uint32_t gen;
  uint32_t tag;
  uint32_t alloc_site;
  uint32_t free_site;
  uint8_t live;
} um_alloc_entry_t;

static um_alloc_entry_t *g_tbl = NULL;
static uint32_t g_tbl_cap = 0;

typedef struct {
  const char *file;
  uint32_t line;
  uint32_t col;
} um_site_info_t;

extern const um_site_info_t __um_sites[];
extern const uint32_t __um_sites_count;

static uint32_t *g_free = NULL;
static uint32_t g_free_len = 0;
static uint32_t g_free_cap = 0;

static void ensure_tables() {
  if (g_tbl) return;
  g_tbl_cap = 1024;
  g_tbl = (um_alloc_entry_t *)calloc(g_tbl_cap, sizeof(um_alloc_entry_t));
  g_free_cap = g_tbl_cap;
  g_free = (uint32_t *)malloc(sizeof(uint32_t) * g_free_cap);
  g_free_len = 0;
  // Slot 0 is reserved: make_handle(0, 0) == 0, which is the null sentinel.
  for (uint32_t i = 1; i < g_tbl_cap; ++i)
    g_free[g_free_len++] = (g_tbl_cap - 1 - (i - 1));
}

static void grow_tables() {
  uint32_t old = g_tbl_cap;
  uint32_t new = old * 2;
  g_tbl = (um_alloc_entry_t *)realloc(g_tbl, sizeof(um_alloc_entry_t) * new);
  memset(g_tbl + old, 0, sizeof(um_alloc_entry_t) * (new - old));
  g_tbl_cap = new;

  g_free = (uint32_t *)realloc(g_free, sizeof(uint32_t) * new);
  for (uint32_t i = old; i < new; ++i)
    g_free[g_free_len++] = (new - 1 - (i - old));
  g_free_cap = new;
}

static inline um_alloc_handle_t make_handle(uint32_t index, uint32_t gen) {
  return (((uint64_t)gen) << 32) | ((uint64_t)index);
}
static inline uint32_t h_index(um_alloc_handle_t handle) {
  return (uint32_t)(handle & 0xFFFFFFFFu);
}
static inline uint32_t h_gen(um_alloc_handle_t handle) {
  return (uint32_t)(handle >> 32);
}

#if UM_DEBUG
static void print_site(uint32_t site, const char *label) {
  if (site == 0 || site >= __um_sites_count) {
    fprintf(stderr, "\t%s: <unknown> (site=%u)\n", label, site);
    return;
  }
  um_site_info_t s = __um_sites[site];
  fprintf(stderr, "\t%s: %s:%u:%u (site=%u)\n", label, s.file, s.line, s.col,
          site);
}

static void trap_bad_handle(const char *op, um_alloc_handle_t h,
                            uint32_t use_site, const um_alloc_entry_t *e,
                            const char *why) {
  fprintf(stderr, "ALLOC ERROR: %s: %s\n", op, why);
  print_site(use_site, "used at");
  fprintf(stderr, "\thandle: index=%u gen=%u raw=0x%016llx\n", h_index(h),
          h_gen(h), (unsigned long long)h);
  if (e) {
    fprintf(stderr, "\tentry: live=%u gen=%u size=%llu align=%u tag=%u\n",
            (unsigned)e->live, (unsigned)e->gen, (unsigned long long)e->size,
            (unsigned)e->align, (unsigned)e->tag);
    print_site(e->alloc_site, "allocated at");
    if (!e->live) print_site(e->free_site, "freed at");
  }
  abort();
}
#endif

static um_alloc_entry_t *get_alloc(um_alloc_handle_t h, uint32_t use_site,
                                   const char *op) {
  ensure_tables();
  uint32_t idx = h_index(h);
  uint32_t gen = h_gen(h);

  if (h == 0) {
#if UM_DEBUG
    trap_bad_handle(op, h, use_site, NULL, "null handle");
#endif
    return NULL;
  }

  if (idx >= g_tbl_cap) {
#if UM_DEBUG
    trap_bad_handle(op, h, use_site, NULL, "index out of range");
#endif
    return NULL;
  }

  um_alloc_entry_t *e = &g_tbl[idx];
  if (!e->live) {
#if UM_DEBUG
    trap_bad_handle(op, h, use_site, e, "use after free / free slot");
#endif
    return NULL;
  }
  if (e->gen != gen) {
#if UM_DEBUG
    trap_bad_handle(op, h, use_site, e, "generation mismatch (stale handle)");
#endif
    return NULL;
  }

  return e;
}

static void *um_aligned_alloc(uint64_t align, uint64_t size) {
  uint64_t s = size;
  if (align && (s % align != 0)) s += (align - (s % align));
  return aligned_alloc((size_t)align, (size_t)s);
}

um_alloc_handle_t rt_alloc(uint64_t size, uint64_t align, uint32_t tag,
                           uint32_t site) {
  ensure_tables();
  if (g_free_len == 0) grow_tables();

  uint32_t idx = g_free[--g_free_len];
  um_alloc_entry_t *e = &g_tbl[idx];

  if (align == 0) align = 8;
  void *p = um_aligned_alloc(align, size);
  if (!p) {
#if UM_DEBUG
    fprintf(stderr, "ALLOC ERROR: out of memory (%llu bytes align %llu)\n",
            (unsigned long long)size, (unsigned long long)align);
#endif
    return 0;
  }

  e->ptr = p;
  e->size = size;
  e->align = (uint32_t)align;
  e->tag = tag;
  e->alloc_site = site;
  e->free_site = 0;
  e->live = 1;
  return make_handle(idx, e->gen);
}

void rt_free(uint64_t h, uint32_t site) {
  ensure_tables();
  uint32_t idx = h_index(h);
  if (h == 0 || idx >= g_tbl_cap) {
#if UM_DEBUG
    trap_bad_handle("free", h, site, NULL, "invalid handle");
#endif
    return;
  }

  um_alloc_entry_t *e = &g_tbl[idx];
  if (!e->live || e->gen != h_gen(h)) {
#if UM_DEBUG
    trap_bad_handle("free", h, site, NULL, "double free / stale handle");
#endif
    return;
  }

  free(e->ptr);
  e->size = 0;
  e->align = 0;
  e->tag = 0;
  e->free_site = site;
  e->live = 0;
  e->gen += 1;

  if (g_free_len == g_free_cap) {
    g_free_cap *= 2;
    g_free = (uint32_t *)realloc(g_free, sizeof(uint32_t) * g_free_cap);
  }
  g_free[g_free_len++] = idx;
}

um_slice_u8_t rt_slice_from_alloc(uint64_t h, uint64_t elem_size,
                                  uint64_t elem_align, uint64_t elem_len,
                                  uint32_t site, uint32_t mut_flag) {

  (void)mut_flag;
  um_slice_u8_t empty = {NULL, 0};

  um_alloc_entry_t *e = get_alloc(h, site, "slice");
  if (!e) {
    return empty;
  }

  uint64_t need = elem_len * elem_size;
  if (need > e->size || elem_align > e->align) {
#if UM_DEBUG
    trap_bad_handle("slice", h, site, e,
                    "requested view exceeds allocation or align requirements");
#endif
    return empty;
  }

  um_slice_u8_t out;
  out.ptr = (uint8_t *)e->ptr;
  out.len = elem_len * elem_size;
  return out;
}

#if UM_DEBUG
static void report_leaks() {
  if (!g_tbl) return;
  uint64_t leaks = 0, bytes = 0;
  for (uint32_t i = 0; i < g_tbl_cap; ++i) {
    um_alloc_entry_t *e = &g_tbl[i];
    if (e->live) {
      leaks++;
      bytes += e->size;
    }
  }
  if (leaks == 0) return;

  fprintf(stderr, "LEAKS DETECTED: %llu allocations, %llu bytes total\n",
          (unsigned long long)leaks, (unsigned long long)bytes);
}
#endif

__attribute__((constructor)) static void rt_init() {
#if UM_DEBUG
  atexit(report_leaks);
#endif
}

#ifdef UM_TESTING
void rt_reset_for_testing(void) {
  if (g_tbl) {
    free(g_tbl);
    g_tbl = NULL;
  }
  if (g_free) {
    free(g_free);
    g_free = NULL;
  }
  g_tbl_cap = g_free_len = g_free_cap = 0;
}
#endif
