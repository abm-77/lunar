#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <common/c_types.h>

typedef uint64_t font_atlas_handle_t;

// UV rectangle + typographic metrics for one glyph.
// uv_* are normalized atlas coordinates [0,1].
// bearing_* and advance are in pixels at the atlas em_size.
typedef struct {
    float uv_x0, uv_y0, uv_x1, uv_y1; // atlas UV bounds
    float bearing_x, bearing_y;        // pen offset from cursor baseline
    float advance;                      // horizontal advance
    float width, height;               // glyph pixel dimensions
} font_glyph_metrics_t;

// load a pre-baked MTSDF font atlas from umpack data.
//   data:       raw bytes from rt_asset_load (atlas pixels + glyph metrics table)
//   atlas_w/h:  atlas texture dimensions (from umpack meta[0]/meta[1])
//   glyph_count: number of glyphs (from umpack meta[2])
//   dev:        gfx device handle for GPU texture upload
// returns 0 on failure.
font_atlas_handle_t rt_font_atlas_load(um_slice_u8_t data,
                                       uint32_t atlas_w, uint32_t atlas_h,
                                       uint32_t glyph_count, uint64_t dev);

// free atlas memory and queue the GPU texture for deferred destruction.
void rt_font_atlas_destroy(font_atlas_handle_t atlas, uint64_t dev);

// look up per-glyph metrics by Unicode codepoint.
// returns false if the codepoint was not baked into this atlas.
bool rt_font_atlas_glyph(font_atlas_handle_t atlas, uint32_t codepoint,
                         font_glyph_metrics_t *out);

// return the gfx texture handle; lower 32 bits = bindless descriptor index.
uint64_t rt_font_atlas_texture(font_atlas_handle_t atlas);

#ifdef __cplusplus
}
#endif
