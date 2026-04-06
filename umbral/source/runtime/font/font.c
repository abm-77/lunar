// runtime font module — loads pre-baked MTSDF atlas + glyph metrics from umpack.
// all baking (FreeType, msdfgen) happens offline in ul.

#include "font.h"
#include <runtime/gfx/gfx.h>
#include <string.h>
#include <stdlib.h>

#define FONT_ALLOC_TAG 0x464E5400u // 'FNT\0'

// declared in mem.c
extern uint64_t rt_alloc(uint64_t size, uint64_t align, uint32_t tag,
                         uint32_t site);
extern void rt_free(uint64_t h, uint32_t site);
extern um_slice_u8_t rt_slice_from_alloc(uint64_t h, uint64_t elem_size,
                                         uint64_t elem_align, uint64_t elem_len,
                                         uint32_t site, uint32_t mut_flag);

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
    uint64_t glyphs_handle; // rt_alloc handle for the glyph metrics array
    packed_glyph_t *glyphs;
} atlas_obj_t;

// helper: allocate and return a pointer via the runtime mem system.
static void *font_alloc(uint64_t size, uint64_t align, uint64_t *out_handle) {
    uint64_t h = rt_alloc(size, align, FONT_ALLOC_TAG, 0);
    if (!h) return NULL;
    um_slice_u8_t s = rt_slice_from_alloc(h, 1, 1, size, 0, 1);
    if (!s.ptr) { rt_free(h, 0); return NULL; }
    *out_handle = h;
    return (void *)s.ptr;
}

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

    uint64_t obj_handle = 0;
    atlas_obj_t *a = (atlas_obj_t *)font_alloc(
        sizeof(atlas_obj_t), _Alignof(atlas_obj_t), &obj_handle);
    if (!a) return 0;

    a->glyph_count = glyph_count;
    uint64_t glyphs_handle = 0;
    a->glyphs = (packed_glyph_t *)font_alloc(
        metrics_bytes, _Alignof(packed_glyph_t), &glyphs_handle);
    if (!a->glyphs) { rt_free(obj_handle, 0); return 0; }
    a->glyphs_handle = glyphs_handle;

    memcpy(a->glyphs, data.ptr + pixel_bytes, metrics_bytes);
    qsort(a->glyphs, glyph_count, sizeof(packed_glyph_t), cmp_glyph);

    // upload pre-baked RGBA8 atlas to GPU
    um_slice_u8_t debug_name = { (const uint8_t *)"font_atlas", 10 };
    a->tex_handle = rt_gfx_texture2d_create_rgba8(
        dev, atlas_w, atlas_h, data.ptr, pixel_bytes, debug_name);

    // pack the rt_alloc handle into the returned handle so we can free it later.
    // we stash it after the atlas_obj_t pointer; recover both from the same u64.
    // since atlas_obj_t* fits in a pointer, we use the obj_handle directly.
    return (font_atlas_handle_t)obj_handle;
}

void rt_font_atlas_destroy(font_atlas_handle_t handle, uint64_t dev) {
    if (!handle) return;
    um_slice_u8_t s = rt_slice_from_alloc(handle, 1, 1, sizeof(atlas_obj_t), 0, 0);
    if (!s.ptr) return;
    atlas_obj_t *a = (atlas_obj_t *)s.ptr;
    if (a->tex_handle) rt_gfx_texture_destroy(dev, a->tex_handle);
    if (a->glyphs_handle) rt_free(a->glyphs_handle, 0);
    rt_free(handle, 0);
}

bool rt_font_atlas_glyph(font_atlas_handle_t handle, uint32_t codepoint,
                          font_glyph_metrics_t *out) {
    if (!handle || !out) return false;
    um_slice_u8_t s = rt_slice_from_alloc(handle, 1, 1, sizeof(atlas_obj_t), 0, 0);
    if (!s.ptr) return false;
    atlas_obj_t *a = (atlas_obj_t *)s.ptr;
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
    if (!handle) return 0;
    um_slice_u8_t s = rt_slice_from_alloc(handle, 1, 1, sizeof(atlas_obj_t), 0, 0);
    if (!s.ptr) return 0;
    return ((atlas_obj_t *)s.ptr)->tex_handle;
}
