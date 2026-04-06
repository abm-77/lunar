// runtime font module — loads pre-baked MTSDF atlas + glyph metrics from umpack.
// all baking (FreeType, msdfgen) happens offline in ul.

#include "font.h"
#include <runtime/gfx/gfx.h>
#include <string.h>
#include <stdlib.h>

// packed glyph metrics entry, matches PackedGlyphMetrics in ul/decode.h.
// 40 bytes: u32 codepoint + 9 floats.
typedef struct {
    uint32_t codepoint;
    float uv_x0, uv_y0, uv_x1, uv_y1;
    float bearing_x, bearing_y;
    float advance;
    float width, height;
} packed_glyph_t;

typedef struct {
    uint64_t tex_handle;
    uint32_t glyph_count;
    packed_glyph_t *glyphs; // sorted by codepoint for binary search
} atlas_obj_t;

static int cmp_glyph(const void *a, const void *b) {
    uint32_t ca = ((const packed_glyph_t *)a)->codepoint;
    uint32_t cb = ((const packed_glyph_t *)b)->codepoint;
    return (ca > cb) - (ca < cb);
}

static const packed_glyph_t *find_glyph(const atlas_obj_t *a, uint32_t cp) {
    packed_glyph_t key;
    key.codepoint = cp;
    return (const packed_glyph_t *)bsearch(&key, a->glyphs, a->glyph_count,
                                            sizeof(packed_glyph_t), cmp_glyph);
}

font_atlas_handle_t rt_font_atlas_load(um_slice_u8_t data,
                                       uint32_t atlas_w, uint32_t atlas_h,
                                       uint32_t glyph_count, uint64_t dev) {
    if (!data.ptr || data.len == 0) return 0;

    uint64_t pixel_bytes = (uint64_t)atlas_w * atlas_h * 4;
    uint64_t metrics_bytes = (uint64_t)glyph_count * sizeof(packed_glyph_t);
    if (data.len < pixel_bytes + metrics_bytes) return 0;

    atlas_obj_t *a = (atlas_obj_t *)malloc(sizeof(atlas_obj_t));
    if (!a) return 0;

    a->glyph_count = glyph_count;
    a->glyphs = (packed_glyph_t *)malloc(metrics_bytes);
    if (!a->glyphs) { free(a); return 0; }
    memcpy(a->glyphs, data.ptr + pixel_bytes, metrics_bytes);
    qsort(a->glyphs, glyph_count, sizeof(packed_glyph_t), cmp_glyph);

    // upload pre-baked RGBA8 atlas to GPU
    um_slice_u8_t debug_name = { (const uint8_t *)"font_atlas", 10 };
    a->tex_handle = rt_gfx_texture2d_create_rgba8(
        dev, atlas_w, atlas_h, data.ptr, pixel_bytes, debug_name);

    return (font_atlas_handle_t)(uintptr_t)a;
}

void rt_font_atlas_destroy(font_atlas_handle_t handle, uint64_t dev) {
    atlas_obj_t *a = (atlas_obj_t *)(uintptr_t)handle;
    if (!a) return;
    if (a->tex_handle) rt_gfx_texture_destroy(dev, a->tex_handle);
    free(a->glyphs);
    free(a);
}

bool rt_font_atlas_glyph(font_atlas_handle_t handle, uint32_t codepoint,
                          font_glyph_metrics_t *out) {
    atlas_obj_t *a = (atlas_obj_t *)(uintptr_t)handle;
    if (!a || !out) return false;
    const packed_glyph_t *g = find_glyph(a, codepoint);
    if (!g) return false;
    out->uv_x0     = g->uv_x0;
    out->uv_y0     = g->uv_y0;
    out->uv_x1     = g->uv_x1;
    out->uv_y1     = g->uv_y1;
    out->bearing_x  = g->bearing_x;
    out->bearing_y  = g->bearing_y;
    out->advance    = g->advance;
    out->width      = g->width;
    out->height     = g->height;
    return true;
}

uint64_t rt_font_atlas_texture(font_atlas_handle_t handle) {
    atlas_obj_t *a = (atlas_obj_t *)(uintptr_t)handle;
    return a ? a->tex_handle : 0;
}
