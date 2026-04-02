#pragma once
// resource tables for textures and samplers.
//
// textures and samplers are stored in fixed-size arrays whose slot indices are
// the bindless descriptor array indices (set=0, binding=0 and binding=1
// respectively). the lower 32 bits of a gfx_texture_handle_t /
// gfx_sampler_handle_t equal the slot index, so shaders use pkt.tex2d_index
// directly as a non-uniform descriptor array index:
//
//   layout(set=0, binding=0) uniform texture2D textures_2d[];
//   layout(set=0, binding=1) uniform sampler   samplers[];
//   vec4 c = texture(sampler2D(textures_2d[pkt.tex2d_index],
//                              samplers[pkt.sampler_index]), uv);
//
// resources are never destroyed immediately; they are enqueued for deferred
// destruction with frames_remaining = cfg.frames_in_flight so in-flight frames
// can still read them. gfx_deferred_tick() must be called each begin_frame to
// drain the queue.

#include "gfx_handles.h"
#include <stdbool.h>
#include <stdint.h>

typedef void *gfx_vk_image_t;
typedef void *gfx_vk_image_view_t;
typedef void *gfx_vk_device_memory_t;
typedef void *gfx_vk_sampler_t;
typedef void *gfx_vk_descriptor_set_t;
typedef void *gfx_vk_descriptor_pool_t;
typedef void *gfx_vk_descriptor_set_layout_t;

// one entry in the texture table; slot 0 is always empty (null sentinel)
typedef struct {
  gfx_vk_image_t image;
  gfx_vk_device_memory_t memory;
  gfx_vk_image_view_t view;
  uint32_t width;
  uint32_t height;
  // VkFormat stored as u32 to avoid Vulkan header dependency here
  uint32_t vk_format;
  uint8_t _pad[4];
} gfx_texture_entry_t;

// one entry in the sampler table
typedef struct {
  gfx_vk_sampler_t sampler;
} gfx_sampler_entry_t;

// deferred-destroy queue entry.
// kind: GFX_DEFERRED_TEXTURE=0, GFX_DEFERRED_SAMPLER=1
// frames_remaining decremented each gfx_deferred_tick(); actual Vulkan destroy
// happens at 0.
#define GFX_DEFERRED_TEXTURE 0u
#define GFX_DEFERRED_SAMPLER 1u

typedef struct {
  uint64_t handle;
  uint8_t kind;
  uint8_t frames_remaining;
  uint16_t _pad;
  uint32_t _pad2;
} gfx_deferred_entry_t;

typedef struct {
  // binding 0: textures_2d[GFX_MAX_TEXTURES]
  gfx_texture_entry_t textures[GFX_MAX_TEXTURES];
  gfx_slot_t tex_slots[GFX_MAX_TEXTURES];

  // binding 1: samplers[GFX_MAX_SAMPLERS]
  gfx_sampler_entry_t samplers[GFX_MAX_SAMPLERS];
  gfx_slot_t samp_slots[GFX_MAX_SAMPLERS];

  // ring queue; new entries written at (deferred_head + deferred_count) %
  // GFX_DEFERRED_QUEUE_MAX
  gfx_deferred_entry_t deferred[GFX_DEFERRED_QUEUE_MAX];
  uint32_t deferred_head;  // oldest entry index
  uint32_t deferred_count; // entries currently in queue

  // one descriptor set per frame-in-flight; all share the same layout.
  // bindings: 0=textures(SAMPLED_IMAGE), 1=samplers(SAMPLER),
  //           2=frame_arena_ssbo, 3=draw_packets_ssbo, 4=material_data_ssbo.
  // pool/layout/sets live in gfx_device_ctx_t (gfx_vulkan.c); frames_in_flight
  // is a runtime value.
} gfx_resource_table_t;

// returns a fresh slot index in [1, GFX_MAX_TEXTURES); 0 = table full.
uint32_t gfx_tex_alloc_slot(gfx_resource_table_t *rt);
// mark tex_slots[slot] free; bump generation.
void gfx_tex_free_slot(gfx_resource_table_t *rt, uint32_t slot);

// returns a fresh slot index in [1, GFX_MAX_SAMPLERS); 0 = table full.
uint32_t gfx_samp_alloc_slot(gfx_resource_table_t *rt);
// mark samp_slots[slot] free; bump generation.
void gfx_samp_free_slot(gfx_resource_table_t *rt, uint32_t slot);

// enqueue h for deferred destruction.
//   kind: GFX_DEFERRED_TEXTURE or GFX_DEFERRED_SAMPLER.
//   frames_remaining: set to cfg.frames_in_flight so the resource outlives all
//   live frames.
// asserts deferred_count < GFX_DEFERRED_QUEUE_MAX.
void gfx_deferred_push(gfx_resource_table_t *rt, uint64_t handle, uint8_t kind,
                       uint8_t frames_remaining);

// called once per frame in rt_gfx_begin_frame before recording.
//   vk_device: the VkDevice (cast to void*); forwarded to Vulkan destroy calls.
// iterates the deferred queue; decrements frames_remaining for each entry;
// destroys and pops entries whose frames_remaining reaches 0.
void gfx_deferred_tick(gfx_resource_table_t *rt, void *vk_device);

// rebuild descriptor writes for bindings 0 and 1 of sets[frame_index] to
// reflect the current contents of the texture and sampler tables.
//   vk_device:   the VkDevice.
//   frame_index: which descriptor set to update ([0, frames_in_flight)).
// must be called once per frame after any texture/sampler create or destroy,
// before vkQueueSubmit for that frame.
void gfx_resources_update_descriptors(gfx_resource_table_t *rt, void *vk_device,
                                      void *vk_descriptor_set,
                                      uint32_t frame_index);
