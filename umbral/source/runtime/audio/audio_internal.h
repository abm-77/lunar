#pragma once
// audio_internal.h — internal types for the audio runtime.
// not part of the public ABI; only included by audio.c, audio_thread.c,
// audio_device.c, and the unit test suite.

#include <audio.h>
#include <common/c_types.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// um_alloc_handle_t is a uint64_t (same as defined in mem.c)
typedef uint64_t um_alloc_handle_t;

#ifndef UM_DEBUG
#define UM_DEBUG 1
#endif

// ---- constants ----

#define AUDIO_MAX_BUSES           16
#define AUDIO_MAX_ROUTES          64
#define AUDIO_MAX_EFFECTS         64
#define AUDIO_MAX_BUS_NAME        32   // bytes including null terminator
#define AUDIO_MAX_VOICES_DEFAULT  64
#define AUDIO_BLOCK_FRAMES        512
#define AUDIO_CHANNELS            2
#define AUDIO_CMD_RING_SIZE       1024  // must be power of 2
#define AUDIO_DEFAULT_SAMPLE_RATE 48000

// rt_alloc tag for all audio allocations
#define AUDIO_ALLOC_TAG 0x41554400u  // 'AUD\0'

// sub-object handle type tags (upper 16 bits of the lower u32)
#define AUDIO_TAG_BUS        0x0100u
#define AUDIO_TAG_ROUTE      0x0200u
#define AUDIO_TAG_VOICE      0x0300u
#define AUDIO_TAG_STREAM     0x0400u
#define AUDIO_TAG_LIMITER    0x0500u
#define AUDIO_TAG_LOWPASS    0x0600u
#define AUDIO_TAG_COMPRESSOR 0x0700u

// encode a sub-object handle: type_tag in upper 16 bits, index in lower 16
static inline uint64_t audio_make_sub_handle(uint32_t tag, uint32_t idx) {
    return (uint64_t)(((uint32_t)tag << 16) | (idx & 0xFFFFu));
}

static inline uint32_t audio_sub_tag(uint64_t h) {
    return (uint32_t)((h >> 16) & 0xFFFFu);
}

static inline uint32_t audio_sub_idx(uint64_t h) {
    return (uint32_t)(h & 0xFFFFu);
}

// ---- effect types ----

typedef enum {
    AUDIO_EFFECT_NONE       = 0,
    AUDIO_EFFECT_LIMITER    = 1,
    AUDIO_EFFECT_LOWPASS    = 2,
    AUDIO_EFFECT_COMPRESSOR = 3,
} audio_effect_type_t;

typedef struct {
    audio_effect_type_t type;
    uint32_t            bus_idx;    // index into builder buses[]
    float               cutoff_hz; // lowpass only
    float               threshold_db; // compressor only
    float               ratio;        // compressor only (>= 1.0)
} audio_effect_def_t;

// ---- bus/route definitions (builder phase) ----

typedef struct {
    char     name[AUDIO_MAX_BUS_NAME];
    uint32_t id;   // 1-based unique id assigned at creation time; 0 = unused
} audio_bus_def_t;

typedef struct {
    uint32_t from_id;  // bus id of source
    uint32_t to_id;    // bus id of destination
    float    gain;     // linear amplitude multiplier
} audio_route_def_t;

// ---- builder state (heap-allocated via rt_alloc) ----

typedef struct {
    audio_bus_def_t    buses[AUDIO_MAX_BUSES];
    uint32_t           bus_count;
    audio_route_def_t  routes[AUDIO_MAX_ROUTES];
    uint32_t           route_count;
    audio_effect_def_t effects[AUDIO_MAX_EFFECTS];
    uint32_t           effect_count;
    uint32_t           output_bus_id;  // 0 = not set
    uint32_t           next_bus_id;    // incremented on each create_bus
} audio_builder_state_t;

// ---- compiled graph state (heap-allocated via rt_alloc) ----

// route entry in the compiled graph: stored per (from_bus_idx, to_bus_idx)
typedef struct {
    uint32_t from_idx;
    uint32_t to_idx;
    float    gain;
} audio_compiled_route_t;

typedef struct {
    // buses in topological order: leaves first, output last
    uint32_t           bus_order[AUDIO_MAX_BUSES];
    uint32_t           bus_count;
    audio_bus_def_t    buses[AUDIO_MAX_BUSES]; // ordered by original index
    audio_compiled_route_t routes[AUDIO_MAX_ROUTES];
    uint32_t           route_count;
    audio_effect_def_t effects[AUDIO_MAX_EFFECTS]; // bus_idx re-mapped to order index
    uint32_t           effect_count;
    uint32_t           output_bus_idx; // index into bus_order
    uint32_t           sample_rate;    // set at context creation
} audio_graph_state_t;

// ---- DSP effect per-frame state (in execution context) ----

typedef struct {
    float gain_env;          // limiter: current gain multiplier (smoothed)
} audio_limiter_state_t;

typedef struct {
    float y_prev_l;          // lowpass: previous output left channel
    float y_prev_r;          // lowpass: previous output right channel
    float alpha;             // lowpass: filter coefficient (computed at context create)
} audio_lowpass_state_t;

typedef struct {
    float gain_db_prev;      // compressor: previous output gain in dB
} audio_compressor_state_t;

typedef union {
    audio_limiter_state_t    limiter;
    audio_lowpass_state_t    lowpass;
    audio_compressor_state_t compressor;
} audio_effect_state_t;

// ---- voice state (preallocated pool in execution context) ----

typedef struct {
    const float *pcm;          // pointer to caller-owned PCM data (stereo float32)
    uint64_t     frame_count;  // total frames
    uint64_t     frame_pos;    // current read position in frames
    uint32_t     bus_idx;      // target bus index in graph
    float        gain_linear;  // current gain (linear)
    float        pan;          // [-1.0, 1.0]; 0 = center
    float        pitch;        // pitch ratio; 1.0 = normal speed (v0: always 1.0)
    bool         active;
} audio_voice_state_t;

// ---- lock-free SPSC command ring ----

typedef enum {
    AUDIO_CMD_STOP_VOICE     = 1,
    AUDIO_CMD_VOICE_GAIN     = 2,
    AUDIO_CMD_VOICE_PAN      = 3,
    AUDIO_CMD_VOICE_PITCH    = 4,
    AUDIO_CMD_BUS_GAIN       = 5,
    AUDIO_CMD_BUS_GAIN_RAMP  = 6,
    AUDIO_CMD_PLAY_VOICE     = 7,  // enqueued by play_clip on RT-safe path
} audio_cmd_type_t;

typedef struct {
    audio_cmd_type_t type;
    uint32_t         index;   // voice_idx or bus_idx
    float            f0;      // gain_linear or pan or pitch
    float            f1;      // ramp target gain for BUS_GAIN_RAMP
    uint64_t         u0;      // ramp_samples for BUS_GAIN_RAMP; frame_count for PLAY_VOICE
    const float     *ptr;     // pcm pointer for PLAY_VOICE
} audio_cmd_t;

typedef struct {
    _Atomic uint32_t head;
    _Atomic uint32_t tail;
    audio_cmd_t      entries[AUDIO_CMD_RING_SIZE];
} audio_cmd_ring_t;

static inline bool audio_ring_push(audio_cmd_ring_t *ring,
                                   const audio_cmd_t *cmd) {
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    uint32_t next = (tail + 1u) & (AUDIO_CMD_RING_SIZE - 1u);
    if (next == atomic_load_explicit(&ring->head, memory_order_acquire))
        return false; // full
    ring->entries[tail] = *cmd;
    atomic_store_explicit(&ring->tail, next, memory_order_release);
    return true;
}

static inline bool audio_ring_pop(audio_cmd_ring_t *ring, audio_cmd_t *out) {
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    if (head == atomic_load_explicit(&ring->tail, memory_order_acquire))
        return false; // empty
    *out = ring->entries[head];
    atomic_store_explicit(&ring->head,
                          (head + 1u) & (AUDIO_CMD_RING_SIZE - 1u),
                          memory_order_release);
    return true;
}

// ---- bus RT state (per-bus gain and ramp, live in the context) ----

typedef struct {
    float    gain_linear;     // current linear gain applied during routing
    float    ramp_target;     // target gain for ramp (0 = no ramp in progress)
    uint64_t ramp_samples_left; // samples remaining in ramp
} audio_bus_rt_state_t;

// ---- execution context state (heap-allocated via rt_alloc) ----

// forward-declare ma_device so we don't pull miniaudio into every header
typedef struct ma_device ma_device;

typedef struct {
    audio_graph_state_t const *graph;  // owned graph (freed in ctx_destroy)
    audio_graph_handle_t       graph_handle; // kept for rt_free at destroy

    audio_voice_state_t  *voices;       // rt_alloc'd array of max_voices entries
    uint64_t              voices_handle; // um_alloc_handle for voices array
    uint32_t              max_voices;

    // per-bus mixing buffers: [bus_count][AUDIO_BLOCK_FRAMES * AUDIO_CHANNELS]
    float                *mix_bufs;     // rt_alloc'd flat array
    uint64_t              mix_bufs_handle;

    audio_effect_state_t  effect_state[AUDIO_MAX_EFFECTS];
    audio_bus_rt_state_t  bus_rt[AUDIO_MAX_BUSES];

    audio_cmd_ring_t      cmd_ring;

    ma_device            *device;       // rt_alloc'd (avoid pulling miniaudio into header)
    uint64_t              device_handle;
    bool                  device_started;

    uint32_t              sample_rate;
    uint32_t              block_frames;
} audio_ctx_state_t;

// ---- forward declarations used across translation units ----

// get a typed pointer from an alloc handle (wrapper around rt_slice_from_alloc)
um_alloc_handle_t rt_alloc(uint64_t size, uint64_t align, uint32_t tag,
                            uint32_t site);
void              rt_free(uint64_t h, uint32_t site);
um_slice_u8_t     rt_slice_from_alloc(uint64_t h, uint64_t elem_size,
                                      uint64_t elem_align, uint64_t elem_len,
                                      uint32_t site, uint32_t mut_flag);

static inline audio_builder_state_t *builder_ptr(audio_builder_handle_t h) {
    um_slice_u8_t s =
        rt_slice_from_alloc(h, 1, 1, sizeof(audio_builder_state_t), 0, 1);
    return (audio_builder_state_t *)(void *)(uintptr_t)s.ptr;
}

static inline audio_graph_state_t *graph_ptr(audio_graph_handle_t h) {
    um_slice_u8_t s =
        rt_slice_from_alloc(h, 1, 1, sizeof(audio_graph_state_t), 0, 1);
    return (audio_graph_state_t *)(void *)(uintptr_t)s.ptr;
}

static inline audio_ctx_state_t *ctx_ptr(audio_ctx_handle_t h) {
    um_slice_u8_t s =
        rt_slice_from_alloc(h, 1, 1, sizeof(audio_ctx_state_t), 0, 1);
    return (audio_ctx_state_t *)(void *)(uintptr_t)s.ptr;
}

// audio_device.c interface (called from audio.c context create/destroy)
void audio_device_open(audio_ctx_state_t *ctx);
void audio_device_close(audio_ctx_state_t *ctx);

// audio_thread.c interface (called from audio_device.c callback)
void audio_process_block(audio_ctx_state_t *ctx, float *output,
                         uint32_t frame_count);
