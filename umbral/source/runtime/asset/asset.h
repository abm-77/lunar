#pragma once
// asset.h — runtime asset registry backed by .umpack v2 files.
// call rt_assets_init with the path to a .umpack bundle; it returns a pack
// handle. use rt_asset_load to get decompressed bytes (views for uncompressed
// entries, rt_alloc'd buffers for compressed entries). rt_assets_cleanup frees
// everything. metadata accessors provide image/audio properties without
// extra parsing.

#ifdef __cplusplus
extern "C" {
#endif

#include <common/c_types.h>
#include <stdint.h>

typedef uint64_t asset_pack_handle_t;

#define ASSET_NULL_HANDLE ((uint64_t)0)

// rt_assets_init — load a .umpack file and return a pack handle.
asset_pack_handle_t rt_assets_init(const uint8_t *path_ptr, uint64_t path_len);

// rt_assets_cleanup — free all decompressed buffers, the pack data, and the
// pack struct. safe to call on ASSET_NULL_HANDLE (no-op).
void rt_assets_cleanup(asset_pack_handle_t pack);

// rt_asset_load — get the decompressed bytes of an asset by id.
// for uncompressed entries this is a direct view into the pack buffer.
// for compressed entries this is an rt_alloc'd buffer (freed by cleanup).
um_slice_u8_t rt_asset_load(asset_pack_handle_t pack, uint64_t id);

// rt_asset_id_from_name — compute FNV-1a 64-bit hash of a name string.
uint64_t rt_asset_id_from_name(const uint8_t *name, uint64_t len);

// rt_asset_image_meta — get width and height for an image asset.
// out_w/out_h are written on success; left unchanged if asset is not an image.
void rt_asset_image_meta(asset_pack_handle_t pack, uint64_t id,
                         uint32_t *out_w, uint32_t *out_h);

// rt_asset_audio_meta — get frame_count, channels, sample_rate for audio.
void rt_asset_audio_meta(asset_pack_handle_t pack, uint64_t id,
                         uint64_t *out_frame_count, uint32_t *out_channels,
                         uint32_t *out_sample_rate);

#ifdef __cplusplus
}
#endif
