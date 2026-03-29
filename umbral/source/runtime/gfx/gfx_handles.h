#pragma once
// slot 0 is never allocated; index 0 always means "no resource".
// stale handles (freed slot with bumped generation) are caught by gfx_handle_check().

#include <stdbool.h>
#include <stdint.h>

typedef uint64_t gfx_device_handle_t;
typedef uint64_t gfx_cmd_handle_t;
typedef uint64_t gfx_pipeline_handle_t;
typedef uint64_t gfx_texture_handle_t;
typedef uint64_t gfx_sampler_handle_t;
typedef uint64_t gfx_buffer_handle_t; // reserved; not exposed in public API

#define GFX_NULL_HANDLE ((uint64_t)0)

// changing these requires rebuilding the descriptor set layout in gfx_vulkan.c.
// max_textures and max_samplers must match the descriptorCount in the layout
// bindings.

#define GFX_MAX_TEXTURES 4096u
#define GFX_MAX_SAMPLERS 256u
#define GFX_MAX_PIPELINES 256u
#define GFX_MAX_BUFFERS 1024u
#define GFX_MAX_FRAMES_IN_FLIGHT 3u
#define GFX_DEFERRED_QUEUE_MAX 1024u

static inline uint64_t gfx_handle_make(uint32_t index, uint32_t gen) {
  return ((uint64_t)gen << 32) | (uint64_t)index;
}

// extract the slot index from a handle (lower 32 bits).
// equal to the bindless descriptor array index for textures and samplers.
static inline uint32_t gfx_handle_index(uint64_t h) {
  return (uint32_t)(h & 0xFFFFFFFFu);
}

static inline uint32_t gfx_handle_gen(uint64_t h) {
  return (uint32_t)(h >> 32);
}

static inline bool gfx_handle_valid(uint64_t h) { return h != GFX_NULL_HANDLE; }

typedef struct {
  uint32_t gen;
  bool allocated;
  uint8_t _pad[3];
} gfx_slot_t;

static inline bool gfx_handle_check(uint64_t h, const gfx_slot_t *table,
                                    uint32_t table_len) {
  uint32_t idx = gfx_handle_index(h);
  if (idx == 0 || idx >= table_len) return false;
  return table[idx].allocated && (table[idx].gen == gfx_handle_gen(h));
}

// find the first free slot in [1, table_len). returns 0 if the table is full.
// TODO: replace with an intrusive free-list for O(1) alloc/free
uint32_t gfx_slot_alloc(gfx_slot_t *table, uint32_t table_len);

// mark slot[index] as free and bump its generation to invalidate existing
// handles. asserts index != 0 and slot is currently allocated.
void gfx_slot_free(gfx_slot_t *table, uint32_t index);
