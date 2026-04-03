#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <common/c_types.h>

#ifndef UM_DEBUG
#define UM_DEBUG 1
#endif

#define LOG2_POW2_U64(x) ((unsigned)__builtin_ctzll((unsigned long long)(x)))

enum { SLAB_SIZE = 256 * 1024 };         // 256 KiB slabs
enum { MIN_BLOCK_SIZE = 16 };            // must be power-of-two
enum { FIRST_POWER_OF_TWO_SIZE = 128 };  // must be power-of-two
enum { LAST_POWER_OF_TWO_SIZE = 65536 }; // must be power-of-two

enum { NUM_SMALL_BLOCKS = (FIRST_POWER_OF_TWO_SIZE / MIN_BLOCK_SIZE) - 1 };

enum {
  NUM_SIZE_CLASSES =
      NUM_SMALL_BLOCKS + (LOG2_POW2_U64(LAST_POWER_OF_TWO_SIZE) -
                          LOG2_POW2_U64(FIRST_POWER_OF_TWO_SIZE) + 1)
};

enum { MAX_CACHED_SLABS = 4 };

typedef uint8_t byte_t;

static uint64_t page_size(void) {
  static uint64_t ps = 0;
  if (!ps) ps = (uint64_t)sysconf(_SC_PAGESIZE);
  return ps;
}

static uint64_t ceil_to_page(uint64_t n) {
  uint64_t ps = page_size();
  return (n + ps - 1) & ~(ps - 1);
}

static void *os_alloc_pages(uint64_t bytes) {
  bytes = ceil_to_page(bytes);
  void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return (p == MAP_FAILED) ? NULL : p;
}

static void os_free_pages(void *p, uint64_t bytes) {
  if (!p) return;
  bytes = ceil_to_page(bytes);
  munmap(p, bytes);
}

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
  return 63u - (uint32_t)__builtin_clzll(x);
#else
  uint32_t r = 0;
  while (x >>= 1) ++r;
  return r;
#endif
}

static uint32_t size_class(uint64_t n, uint64_t align) {
  if (align > n) n = align;
  if (n == 0) n = MIN_BLOCK_SIZE;

  const uint64_t small_max =
      (uint64_t)NUM_SMALL_BLOCKS * (uint64_t)MIN_BLOCK_SIZE;

  if (n <= small_max) {
    uint64_t r = ceil_to_min_block(n);
    return (uint32_t)(r / MIN_BLOCK_SIZE) - 1; // 16->0
  }

  uint64_t p = ceil_pow2_u64(n);
  if (p < FIRST_POWER_OF_TWO_SIZE) p = FIRST_POWER_OF_TWO_SIZE;

  return NUM_SMALL_BLOCKS + (log2_u64(p) - log2_u64(FIRST_POWER_OF_TWO_SIZE));
}

static inline uint64_t size_from_class(uint32_t class_id) {
  if (class_id < NUM_SMALL_BLOCKS)
    return (uint64_t)(class_id + 1) * (uint64_t)MIN_BLOCK_SIZE; // 16..112

  uint32_t k =
      (class_id - NUM_SMALL_BLOCKS) + log2_u64(FIRST_POWER_OF_TWO_SIZE);
  return 1ull << k;
}

typedef struct free_node_t {
  struct free_node_t *next;
} free_node_t;

typedef enum slab_state_t {
  SLAB_PARTIAL = 0,
  SLAB_FULL = 1,
  SLAB_CACHED = 2,
} slab_state_t;

typedef struct slab_t {
  struct slab_t *next;
  struct slab_t *prev;

  uint32_t size_class;
  uint32_t block_size;

  uint32_t total_blocks;
  uint32_t next_index; // bump index for fresh blocks
  uint32_t in_use;     // live allocated blocks

  free_node_t *free_head;
  slab_state_t state;
} slab_t;

typedef struct class_state_t {
  slab_t *partial;                 // slabs with available blocks
  slab_t *full;                    // slabs with no available blocks
  slab_t *cache[MAX_CACHED_SLABS]; // stack of cached empty slabs
  uint32_t cache_count;
} class_state_t;

static struct {
  class_state_t classes[NUM_SIZE_CLASSES];
} g_alloc;

static inline void list_remove(slab_t **head, slab_t *s) {
  if (s->prev) s->prev->next = s->next;
  else *head = s->next;
  if (s->next) s->next->prev = s->prev;
  s->next = s->prev = NULL;
}

static inline void list_push_front(slab_t **head, slab_t *s) {
  s->prev = NULL;
  s->next = *head;
  if (*head) (*head)->prev = s;
  *head = s;
}

static inline int slab_has_available(slab_t *s) {
  if (s->free_head) return 1;
  return s->next_index < s->total_blocks;
}

static inline uintptr_t align_up_pow2(uintptr_t p, uint64_t a) {
  return (p + a - 1) & ~(uintptr_t)(a - 1);
}

static inline byte_t *slab_blocks_base(slab_t *s) {
  uintptr_t base = (uintptr_t)s;
  uintptr_t p = base + sizeof(slab_t);
  p = align_up_pow2(p, MIN_BLOCK_SIZE);
  return (byte_t *)p;
}

static slab_t *slab_create(uint32_t class_id) {
  slab_t *s = (slab_t *)os_alloc_pages(SLAB_SIZE);
  if (!s) return NULL;

  memset(s, 0, sizeof(*s));
  s->size_class = class_id;
  s->block_size = (uint32_t)size_from_class(class_id);
  s->state = SLAB_PARTIAL;

  byte_t *base = slab_blocks_base(s);
  uint64_t usable = ((byte_t *)s + SLAB_SIZE) - base;
  s->total_blocks = (uint32_t)(usable / s->block_size);

  // next_index=0, free_head=NULL, in_use=0 already
  return s;
}

static void slab_reset_for_reuse(slab_t *s) {
  s->next_index = 0;
  s->in_use = 0;
  s->free_head = NULL;
  s->state = SLAB_PARTIAL;
  s->next = s->prev = NULL;
}

// we can safely madvise the payload pages of a cached empty slab. do NOT
// madvise the first page so the header remains intact.
static void slab_madvise_empty(slab_t *s) {
  uint64_t ps = page_size();
  if (SLAB_SIZE <= ps) return;
  void *payload = (byte_t *)s + ps;
  uint64_t len = SLAB_SIZE - ps;
  (void)madvise(payload, len, MADV_DONTNEED);
}

typedef struct block_t {
  byte_t *ptr;
  slab_t *owner;
  uint32_t class_id;
} block_t;

static slab_t *class_get_slab_for_alloc(uint32_t class_id) {
  class_state_t *cs = &g_alloc.classes[class_id];

  // reuse partial
  if (cs->partial) return cs->partial;

  // reuse from cached
  if (cs->cache_count) {
    slab_t *s = cs->cache[--cs->cache_count];
    slab_reset_for_reuse(s);
    list_push_front(&cs->partial, s);
    return s;
  }

  // create a new slab
  slab_t *s = slab_create(class_id);
  if (!s) return NULL;
  list_push_front(&cs->partial, s);
  return s;
}

// sentinel class_id for allocations that exceed LAST_POWER_OF_TWO_SIZE.
// these go directly to os_alloc_pages and bypass the slab layer entirely.
enum { LARGE_CLASS_ID = UINT32_MAX };

static block_t block_allocate(uint64_t n, uint64_t align) {
  if (align > n) n = align;
  if (n == 0) n = MIN_BLOCK_SIZE;

  // large allocation: directly mmap; owner=NULL signals this to block_free
  if (n > LAST_POWER_OF_TWO_SIZE) {
    uint64_t alloc_bytes = ceil_to_page(n);
    byte_t *p = (byte_t *)os_alloc_pages(alloc_bytes);
    if (!p) return (block_t){0};
    return (block_t){.ptr = p, .owner = NULL, .class_id = LARGE_CLASS_ID};
  }

  const uint32_t class_id = size_class(n, align);
  class_state_t *cs = &g_alloc.classes[class_id];

  slab_t *s = class_get_slab_for_alloc(class_id);
  if (!s) return (block_t){0};

  // check free list
  if (s->free_head) {
    free_node_t *node = s->free_head;
    s->free_head = node->next;
    s->in_use++;

    if (!slab_has_available(s)) {
      list_remove(&cs->partial, s);
      s->state = SLAB_FULL;
      list_push_front(&cs->full, s);
    }

    return (block_t){.ptr = (byte_t *)node, .owner = s, .class_id = class_id};
  }

  // allocate out of slab
  if (s->next_index < s->total_blocks) {
    byte_t *base = slab_blocks_base(s);
    byte_t *ptr = base + (uint64_t)s->next_index * (uint64_t)s->block_size;
    s->next_index++;
    s->in_use++;

    if (!slab_has_available(s)) {
      list_remove(&cs->partial, s);
      s->state = SLAB_FULL;
      list_push_front(&cs->full, s);
    }

    return (block_t){.ptr = ptr, .owner = s, .class_id = class_id};
  }

  // UNREACHABLE: partial slab with no availability. move it to full and retry.
  list_remove(&cs->partial, s);
  s->state = SLAB_FULL;
  list_push_front(&cs->full, s);
  return block_allocate(n, align);
}

static void block_free(uint32_t class_id, slab_t *owner, void *p, uint64_t size) {
  if (class_id == LARGE_CLASS_ID) {
    os_free_pages(p, size);
    return;
  }
  class_state_t *cs = &g_alloc.classes[class_id];

  // push into  free list
  free_node_t *node = (free_node_t *)p;
  node->next = owner->free_head;
  owner->free_head = node;

  if (owner->in_use) owner->in_use--;

  // if slab was FULL and now has availability, move it back to PARTIAL
  if (owner->state == SLAB_FULL && slab_has_available(owner)) {
    list_remove(&cs->full, owner);
    owner->state = SLAB_PARTIAL;
    list_push_front(&cs->partial, owner);
  }

  // if slab becomes empty of live blocks, cache or unmap
  if (owner->in_use == 0) {
    if (owner->state == SLAB_PARTIAL) list_remove(&cs->partial, owner);
    else if (owner->state == SLAB_FULL) list_remove(&cs->full, owner);

    owner->state = SLAB_CACHED;

    if (cs->cache_count < MAX_CACHED_SLABS) {
      slab_reset_for_reuse(owner);
      slab_madvise_empty(owner);
      cs->cache[cs->cache_count++] = owner;
    } else {
      os_free_pages(owner, SLAB_SIZE);
    }
  }
}

typedef uint64_t um_alloc_handle_t;

typedef struct um_alloc_entry_t {
  void *ptr;
  slab_t *owner;
  uint64_t size; // requested bytes
  uint32_t align;
  uint32_t gen;
  uint32_t tag;
  uint32_t alloc_site;
  uint32_t free_site;
  uint32_t size_class;
  uint8_t live;
} um_alloc_entry_t;

static um_alloc_entry_t *g_tbl = NULL;
static uint32_t g_tbl_cap = 0;

typedef struct um_site_info_t {
  const char *file;
  uint32_t line;
  uint32_t col;
} um_site_info_t;

extern const um_site_info_t __um_sites[];
extern const uint32_t __um_sites_count;

static uint32_t *g_free = NULL;
static uint32_t g_free_len = 0;
static uint32_t g_free_cap = 0;

static void ensure_tables(void) {
  if (g_tbl) return;

  g_tbl_cap = 1024;
  g_tbl = (um_alloc_entry_t *)calloc(g_tbl_cap, sizeof(um_alloc_entry_t));

  g_free_cap = g_tbl_cap;
  g_free = (uint32_t *)malloc(sizeof(uint32_t) * g_free_cap);
  g_free_len = 0;

  // slot 0 reserved for null handle
  for (uint32_t i = 1; i < g_tbl_cap; ++i)
    g_free[g_free_len++] = (g_tbl_cap - 1 - (i - 1));
}

static void grow_tables(void) {
  uint32_t old = g_tbl_cap;
  uint32_t neu = old * 2;

  g_tbl = (um_alloc_entry_t *)realloc(g_tbl, sizeof(um_alloc_entry_t) * neu);
  memset(g_tbl + old, 0, sizeof(um_alloc_entry_t) * (neu - old));
  g_tbl_cap = neu;

  g_free = (uint32_t *)realloc(g_free, sizeof(uint32_t) * neu);
  for (uint32_t i = old; i < neu; ++i)
    g_free[g_free_len++] = (neu - 1 - (i - old));
  g_free_cap = neu;
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

static um_alloc_entry_t *get_alloc_entry(um_alloc_handle_t h, uint32_t use_site,
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

um_alloc_handle_t rt_alloc(uint64_t size, uint64_t align, uint32_t tag,
                           uint32_t site) {
  ensure_tables();
  if (g_free_len == 0) grow_tables();

  uint32_t idx = g_free[--g_free_len];
  um_alloc_entry_t *e = &g_tbl[idx];

  if (align == 0) align = 8;

  block_t blk = block_allocate(size, align);
  if (!blk.ptr) {
#if UM_DEBUG
    fprintf(stderr, "ALLOC ERROR: out of memory (%llu bytes align %llu)\n",
            (unsigned long long)size, (unsigned long long)align);
    print_site(site, "alloc at");
#endif
    return 0;
  }

  e->ptr = blk.ptr;
  e->owner = blk.owner;
  e->size = size;
  e->align = (uint32_t)align;
  e->tag = tag;
  e->alloc_site = site;
  e->free_site = 0;
  e->size_class = blk.class_id;
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
    trap_bad_handle("free", h, site, e, "double free / stale handle");
#endif
    return;
  }

  block_free(e->size_class, e->owner, e->ptr, e->size);

  e->ptr = NULL;
  e->owner = NULL;
  e->size = 0;
  e->align = 0;
  e->tag = 0;
  e->free_site = site;
  e->size_class = 0;
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

  um_slice_u8_t empty = {.ptr = NULL, .len = 0};

  um_alloc_entry_t *e = get_alloc_entry(h, site, "slice");
  if (!e) return empty;

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
static void report_leaks(void) {
  if (!g_tbl) return;

  uint64_t leaks = 0, bytes = 0;
  for (uint32_t i = 0; i < g_tbl_cap; ++i) {
    um_alloc_entry_t *e = &g_tbl[i];
    if (e->live) {
      leaks++;
      bytes += e->size;
    }
  }

  if (!leaks) return;

  fprintf(stderr, "LEAKS DETECTED: %llu allocations, %llu bytes total\n",
          (unsigned long long)leaks, (unsigned long long)bytes);
}
#endif

static void allocator_shutdown(void) {
  // free cached slabs + partial + full
  for (uint32_t c = 0; c < NUM_SIZE_CLASSES; ++c) {
    class_state_t *cs = &g_alloc.classes[c];

    for (uint32_t i = 0; i < cs->cache_count; ++i) {
      os_free_pages(cs->cache[i], SLAB_SIZE);
      cs->cache[i] = NULL;
    }
    cs->cache_count = 0;

    slab_t *s = cs->partial;
    while (s) {
      slab_t *next = s->next;
      os_free_pages(s, SLAB_SIZE);
      s = next;
    }
    cs->partial = NULL;

    s = cs->full;
    while (s) {
      slab_t *next = s->next;
      os_free_pages(s, SLAB_SIZE);
      s = next;
    }
    cs->full = NULL;
  }

  free(g_tbl);
  g_tbl = NULL;

  free(g_free);
  g_free = NULL;

  g_tbl_cap = 0;
  g_free_len = 0;
  g_free_cap = 0;
}

__attribute__((constructor)) static void rt_init(void) {
#if UM_DEBUG
  // LIFO: report leaks before tearing down allocator internals.
  atexit(allocator_shutdown);
  atexit(report_leaks);
#endif
}

#ifdef UM_TESTING
void rt_reset_for_testing(void) { allocator_shutdown(); }
#endif
