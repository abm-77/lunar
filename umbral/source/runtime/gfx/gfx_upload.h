#pragma once
// per-frame host-visible ring arena for transient GPU data (draw constants,
// etc.).
//
// the backing VkBuffer is frame_arena_bytes * frames_in_flight total, divided
// into equal per-frame slices.  each begin_frame advances to the next slice
// after waiting on that slot's fence, so the CPU can overwrite data the GPU
// finished reading.
//
// shaders access the arena as frame_arena_ssbo (set=0, binding=2):
//   layout(set=0, binding=2) readonly buffer FrameArena { uint8_t data[]; };
//   MyData d = unpackMyData(data, packet.draw_data_offset);
//
// threading: NOT thread-safe; all calls must originate from the main/render
// thread.
//
// invariant: all allocations within a frame must fit within frame_arena_bytes.
//   if gfx_arena_alloc returns NULL the frame is over-budget; crash in debug,
//   skip in release.

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#define GFX_ARENA_DEFAULT_BYTES                                                \
  (64ull * 1024ull * 1024ull) // 64 MiB per frame slot

// contract: frame_arena_bytes must be a multiple of
//   VkPhysicalDeviceLimits.minStorageBufferOffsetAlignment (at most 256 bytes
//   in practice). align each sub-allocation to the same limit so SSBO offsets
//   are always legal.

typedef struct {
  // persistent CPU mapping of the entire ring (total = frame_size *
  // frames_in_flight). written by the CPU; read by the GPU as a storage buffer.
  // NULL until gfx_arena_init succeeds.
  void *mapped_ptr;

  // total ring capacity in bytes (frame_size * frames_in_flight)
  uint64_t capacity;

  // bytes available to allocate in a single frame slot
  uint64_t frame_size;

  // byte offset of the current frame's slice from the start of the VkBuffer.
  // = frame_index * frame_size
  uint64_t frame_base_offset;

  // allocation cursor relative to frame_base_offset; advanced on each
  // gfx_arena_alloc call.
  uint64_t head;

  // current frame slot index in [0, frames_in_flight)
  uint32_t frame_index;
  uint32_t frames_in_flight;

  // backing_buffer — created with STORAGE_BUFFER | TRANSFER_DST usage
  VkBuffer buffer;
  // backing_mem    — HOST_VISIBLE | HOST_COHERENT memory type
  VkDeviceMemory memory;

} gfx_frame_arena_t;

// create and persistently map the ring buffer.
// frame_arena_bytes: per-frame capacity in bytes; must be > 0 and a multiple of
// 256. vk_device/physical_device: opaque void* to keep this header Vulkan-free.
void gfx_arena_init(gfx_frame_arena_t *arena, uint64_t frame_arena_bytes,
                    uint32_t frames_in_flight, void *vk_device,
                    void *physical_device);

// advance the ring to the next slot and reset the head.
// must be called at the start of rt_gfx_begin_frame, AFTER vkWaitForFences on
// the new slot. increments frame_index modulo frames_in_flight and updates
// frame_base_offset.
void gfx_arena_next_frame(gfx_frame_arena_t *arena);

// sub-allocate bytes from the current frame's slice.
// align: power-of-two; use minStorageBufferOffsetAlignment (<=256) for SSBO
// offsets. out_offset: byte offset from start of backing_buffer; pass into
// draw_packet_t.draw_data_offset. returns CPU pointer into the persistent
// mapping, valid until gfx_arena_next_frame recycles this slot. returns NULL if
// the slice is exhausted; out_offset set to 0.
void *gfx_arena_alloc(gfx_frame_arena_t *arena, uint64_t size, uint64_t align,
                      uint32_t *out_offset);

// unmap and destroy the ring buffer.
// must be called only after vkDeviceWaitIdle() in rt_gfx_shutdown.
void gfx_arena_destroy(gfx_frame_arena_t *arena, void *vk_device);
