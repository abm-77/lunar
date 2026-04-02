#pragma once

// asset.h — runtime asset registry.
// call rt_assets_init once at startup with the path to a .umpack bundle.
// then use rt_asset_load(id) to borrow a slice of bytes, and rt_asset_release
// when done. asset IDs are FNV-1a 64-bit hashes of the asset's filename
// (e.g., fnv1a64("TriShader.umsh")).

#ifdef __cplusplus
extern "C" {
#endif

#include <common/c_types.h>
#include <stdint.h>

// rt_assets_init — load a .umpack file and build the asset ID table.
// called from Umbral with a []u8 slice (ptr + len); path need not be
// null-terminated.
void rt_assets_init(const uint8_t *path_ptr, uint64_t path_len);

// rt_asset_load — return a borrowed slice of the asset's bytes.
// returns {NULL, 0} if the id is not found.
um_slice_u8_t rt_asset_load(uint64_t id);

// rt_asset_release — decrement reference count (no-op in v0).
void rt_asset_release(uint64_t id);

#ifdef __cplusplus
}
#endif
