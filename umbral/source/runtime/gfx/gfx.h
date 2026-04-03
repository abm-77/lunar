#pragma once
// gfx.h — public C ABI for the Umbral graphics runtime.
// no Vulkan types appear here; all state is hidden behind opaque uint64_t
// handles. threading: main/render thread only; no internal locking. ownership:
// all handles are device-owned; pair every create with matching destroy. handle
// validity: GFX_NULL_HANDLE (0) is invalid; generation mismatch is asserted
//   in debug (UM_DEBUG=1), undefined behavior in release.

#ifdef __cplusplus
extern "C" {
#endif

#include <common/c_types.h>
#include <stdbool.h>
#include <stdint.h>

typedef uint64_t
    gfx_device_handle_t;           // logical device + swapchain + sync objects
typedef uint64_t gfx_cmd_handle_t; // single-frame command recording token;
                                   // invalid after end_frame
typedef uint64_t gfx_pipeline_handle_t; // compiled raster pipeline; valid until
                                        // pipeline_destroy
typedef uint64_t gfx_texture_handle_t;  // 2D RGBA8 sampled image; lower 32 bits
                                        // = descriptor index
typedef uint64_t
    gfx_sampler_handle_t; // immutable sampler; lower 32 bits = descriptor index

#define GFX_NULL_HANDLE ((uint64_t)0)

typedef enum {
  GFX_PRESENT_MODE_FIFO =
      0, // wait for vertical blank; always supported; no tearing
  GFX_PRESENT_MODE_IMMEDIATE = 1, // no wait; may tear; lowest latency
  GFX_PRESENT_MODE_MAILBOX =
      2, // single-entry queue; no tear; preferred when available
} gfx_present_mode_t;

// draw_packet_t.flags bitmask
#define GFX_DRAW_FLAG_INDEXED                                                  \
  (1u << 0) // use index_buffer_handle / index_count / first_index
#define GFX_DRAW_FLAG_INSTANCED (1u << 1) // assert instance_count > 1 in debug

typedef struct {
  uint32_t frames_in_flight; // simultaneous GPU frames in flight; range [1,
                             // GFX_MAX_FRAMES_IN_FLIGHT]
  uint32_t max_textures; // bindless texture descriptor array capacity; must be
                         // <= 4096
  uint32_t max_samplers; // bindless sampler descriptor array capacity; must be
                         // <= 256
  uint64_t frame_arena_bytes; // per-frame transient upload arena in bytes; must
                              // be a multiple of 256
  uint32_t draw_packets_max;  // max draw_packet_t entries per
                              // rt_gfx_submit_draw_packets call
  bool enable_validation; // enable VK_LAYER_KHRONOS_validation; adds overhead;
                          // false in release
  bool enable_depth;      // allocate a D32_SFLOAT depth buffer and enable depth
                          // test/write; false by default
  gfx_present_mode_t present_mode; // falls back to FIFO if unavailable
} rt_gfx_config_t;

// sensible defaults; override individual fields as needed:
//   rt_gfx_config_t cfg = { RT_GFX_CONFIG_DEFAULTS };
#define RT_GFX_CONFIG_DEFAULTS                                                 \
  .frames_in_flight = 2, .max_textures = 4096, .max_samplers = 256,            \
  .frame_arena_bytes = (64ull << 20), .draw_packets_max = 65536,               \
  .enable_validation = false, .enable_depth = false,                            \
  .present_mode = GFX_PRESENT_MODE_MAILBOX

// draw_packet_t — one logical draw command uploaded to draw_packets_ssbo per
// frame. ABI between CPU (runtime) and GPU (shaders); field order is fixed. do
// NOT reorder or insert padding fields without updating the GLSL struct mirror.
// shaders access textures/samplers via tex2d_index/sampler_index into the
// bindless descriptor arrays (set=0 binding=0/1); per-draw constants at
// draw_data_offset in frame_arena_ssbo (set=0 binding=2); draw packets at set=0
// binding=3.
typedef struct {
  uint64_t pipeline_handle; // compiled pipeline; runtime binds it before the
                            // draw call
  uint64_t vertex_buffer_handle; // GFX_NULL_HANDLE for procedural/vertex-pull
                                 // geometry
  uint64_t index_buffer_handle;  // GFX_NULL_HANDLE for non-indexed draws

  uint32_t
      first_index; // first index in index buffer; ignored for non-indexed draws
  uint32_t index_count;    // indices per instance; for a sprite quad use 6
  uint32_t vertex_count;   // vertices per instance for non-indexed draws
  uint32_t instance_count; // must be >= 1; shaders use gl_InstanceIndex within
                           // a batch
  uint32_t first_instance; // gl_InstanceIndex for the first instance; 0 in the
                           // common case

  // byte offset into frame_arena_ssbo (set=0, binding=2) for per-draw
  // constants. must be aligned to
  // VkPhysicalDeviceLimits.minStorageBufferOffsetAlignment (<=256). pass the
  // uint32_t *out_offset from rt_gfx_frame_alloc unchanged.
  uint32_t draw_data_offset;

  // byte offset into material_data_ssbo (set=0, binding=4); 0 = no material.
  uint32_t material_data_offset;

  uint32_t tex2d_index;   // lower 32 bits of gfx_texture_handle_t; indexes
                          // textures_2d[]
  uint32_t sampler_index; // lower 32 bits of gfx_sampler_handle_t; indexes
                          // samplers[]
  uint32_t flags;         // bitmask of GFX_DRAW_FLAG_*
} draw_packet_t;

// rt_gfx_init — initialize Vulkan and create the device + swapchain.
// params mirror rt_gfx_config_t fields (flattened to avoid C struct ABI
// concerns at the language boundary). returns a device handle on success;
// GFX_NULL_HANDLE on failure (details to stderr).
gfx_device_handle_t rt_gfx_init(uint64_t window_handle,
                                uint32_t frames_in_flight,
                                uint32_t max_textures, uint32_t max_samplers,
                                uint64_t frame_arena_bytes,
                                uint32_t draw_packets_max,
                                bool enable_validation, uint32_t present_mode,
                                bool enable_depth);

// rt_gfx_shutdown — destroy all Vulkan resources and free the device context.
// dev must not be GFX_NULL_HANDLE; after this call it is invalid.
void rt_gfx_shutdown(gfx_device_handle_t dev);

// rt_gfx_begin_frame — acquire the next swapchain image and open a command
// buffer. returns a cmd handle valid only until the matching rt_gfx_end_frame
// call. returns GFX_NULL_HANDLE if the swapchain is out-of-date (window
// resized); retry next frame.
gfx_cmd_handle_t rt_gfx_begin_frame(gfx_device_handle_t dev);

// rt_gfx_end_frame — end recording, submit, and present.
// cmd is invalid after this call.
void rt_gfx_end_frame(gfx_device_handle_t dev, gfx_cmd_handle_t cmd);

// rt_gfx_pipeline_create — compile a raster pipeline from SPIR-V bytecode +
// UMRF reflection. vs_spv/fs_spv: 4-byte-aligned SPIR-V words; vs_len/fs_len
// must be multiples of 4. refl: UMRF blob (see gfx_refl.h); borrowed for the
// duration of this call only. returns a pipeline handle on success;
// GFX_NULL_HANDLE on failure.
gfx_pipeline_handle_t
rt_gfx_pipeline_create(gfx_device_handle_t dev, const uint8_t *vs_spv,
                       uint64_t vs_len, const uint8_t *fs_spv, uint64_t fs_len,
                       const uint8_t *refl, uint64_t refl_len);

// rt_gfx_pipeline_create_from_umsh — create a raster pipeline from a .umsh
// blob. umsh: raw bytes of a .umsh bundle (see runtime/gfx/umsh.h); borrowed
// for this call only. umsh_len: byte count of umsh; must equal the total_bytes
// in the .umsh header. variant_key: reserved; pass 0. returns a pipeline handle
// on success; GFX_NULL_HANDLE on failure.
gfx_pipeline_handle_t rt_gfx_pipeline_create_from_umsh(gfx_device_handle_t dev,
                                                       um_slice_u8_t umsh,
                                                       uint32_t variant_key);

// rt_gfx_pipeline_destroy — destroy a pipeline.
// the pipeline must not be in use by any in-flight frame when this is called.
void rt_gfx_pipeline_destroy(gfx_device_handle_t dev,
                             gfx_pipeline_handle_t pipe);

// rt_gfx_texture2d_create_rgba8 — upload a 2D RGBA8 texture.
// rgba: pointer to w*h*4 bytes of RGBA8 pixel data in row-major order.
// debug_name: optional UTF-8 label for VK_EXT_debug_utils; may be NULL; runtime
// copies the string. returns a handle on success; lower 32 bits are the
// bindless descriptor index for draw_packet_t. returns GFX_NULL_HANDLE on
// allocation failure.
gfx_texture_handle_t rt_gfx_texture2d_create_rgba8(gfx_device_handle_t dev,
                                                   uint32_t w, uint32_t h,
                                                   const uint8_t *rgba,
                                                   uint64_t rgba_len,
                                                   um_slice_u8_t debug_name);

// rt_gfx_texture_destroy — queue a texture for deferred destruction.
// destruction is deferred by cfg.frames_in_flight frames to avoid
// GPU-visible-while-in-use. do not pass tex2d_index to any new draw packets
// after this call.
void rt_gfx_texture_destroy(gfx_device_handle_t dev, gfx_texture_handle_t tex);

// rt_gfx_sampler_create_linear — create a bilinear clamp-to-edge sampler.
// debug_name: optional UTF-8 label; may be NULL.
// returns a handle on success; lower 32 bits are the bindless descriptor index.
gfx_sampler_handle_t rt_gfx_sampler_create_linear(gfx_device_handle_t dev,
                                                  um_slice_u8_t debug_name);

// rt_gfx_sampler_destroy — queue a sampler for deferred destruction (same rules
// as texture_destroy).
void rt_gfx_sampler_destroy(gfx_device_handle_t dev, gfx_sampler_handle_t samp);

// rt_gfx_frame_alloc — sub-allocate bytes from the current frame's arena.
// must be called between begin_frame and end_frame.
// align: power-of-two; use
// VkPhysicalDeviceLimits.minStorageBufferOffsetAlignment (<=256) for SSBO
// offsets. out_offset: receives byte offset from start of frame_arena_ssbo;
// pass into draw_packet_t.draw_data_offset. returns a CPU-writable pointer
// valid until gfx_arena_next_frame recycles this slot. returns NULL if the
// arena is exhausted; out_offset set to 0.
void *rt_gfx_frame_alloc(gfx_device_handle_t dev, uint64_t size, uint64_t align,
                         uint32_t *out_offset);

// rt_gfx_submit_draw_packets — upload draw packets and record draw calls.
// cmd: the current frame's cmd handle (between begin/end).
// pipe: pipeline for all packets in this call; runtime binds it once.
// packets: array of draw_packet_t; read synchronously before returning; may be
// stack or heap. packet_count: must be > 0 and <= cfg.draw_packets_max.
void rt_gfx_submit_draw_packets(gfx_cmd_handle_t cmd,
                                gfx_pipeline_handle_t pipe,
                                const draw_packet_t *packets,
                                uint32_t packet_count);

#ifdef __cplusplus
}
#endif
