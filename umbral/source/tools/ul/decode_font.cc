// offline MTSDF font atlas baking for the asset linker.
// takes raw TTF/OTF bytes, produces an RGBA8 atlas + glyph metrics table.

#include "decode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <msdfgen.h>
#include <core/edge-coloring.h>

struct ShapeCtx {
    msdfgen::Shape   *shape;
    msdfgen::Contour *contour;
    double            scale;
    msdfgen::Point2   current;
};

static msdfgen::Point2 ft_pt(const FT_Vector *v, double scale) {
    return msdfgen::Point2(v->x * scale, v->y * scale);
}

static int ft_move_to(const FT_Vector *to, void *user) {
    ShapeCtx *ctx = static_cast<ShapeCtx *>(user);
    if (!(ctx->contour && ctx->contour->edges.empty()))
        ctx->contour = &ctx->shape->addContour();
    ctx->current = ft_pt(to, ctx->scale);
    return 0;
}

static int ft_line_to(const FT_Vector *to, void *user) {
    ShapeCtx *ctx = static_cast<ShapeCtx *>(user);
    msdfgen::Point2 ep = ft_pt(to, ctx->scale);
    if (ep != ctx->current) {
        ctx->contour->addEdge(msdfgen::EdgeHolder(ctx->current, ep));
        ctx->current = ep;
    }
    return 0;
}

static int ft_conic_to(const FT_Vector *ctrl, const FT_Vector *to, void *user) {
    ShapeCtx *ctx = static_cast<ShapeCtx *>(user);
    msdfgen::Point2 ep = ft_pt(to, ctx->scale);
    if (ep != ctx->current) {
        ctx->contour->addEdge(msdfgen::EdgeHolder(ctx->current, ft_pt(ctrl, ctx->scale), ep));
        ctx->current = ep;
    }
    return 0;
}

static int ft_cubic_to(const FT_Vector *c1, const FT_Vector *c2,
                        const FT_Vector *to, void *user) {
    ShapeCtx *ctx = static_cast<ShapeCtx *>(user);
    msdfgen::Point2 ep = ft_pt(to, ctx->scale);
    if (ep != ctx->current) {
        ctx->contour->addEdge(msdfgen::EdgeHolder(ctx->current,
            ft_pt(c1, ctx->scale), ft_pt(c2, ctx->scale), ep));
        ctx->current = ep;
    }
    return 0;
}

static const FT_Outline_Funcs k_ft_funcs = {
    ft_move_to, ft_line_to, ft_conic_to, ft_cubic_to,
    0, 0
};

DecodedFont decode_font(const uint8_t *data, size_t len,
                        float em_size, uint32_t atlas_w, uint32_t atlas_h) {
    DecodedFont out;

    FT_Library lib;
    if (FT_Init_FreeType(&lib) != 0) {
        fprintf(stderr, "decode_font: FT_Init_FreeType failed\n");
        return out;
    }

    FT_Face face;
    if (FT_New_Memory_Face(lib, data, (FT_Long)len, 0, &face) != 0) {
        fprintf(stderr, "decode_font: FT_New_Memory_Face failed\n");
        FT_Done_FreeType(lib);
        return out;
    }

    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)em_size);

    std::vector<uint8_t> pixels(atlas_w * atlas_h * 4, 0);
    std::vector<PackedGlyphMetrics> glyphs;

    const int pad = 2;
    const float px_range = 8.0f;
    int pen_x = 1, pen_y = 1, row_h = 0;

    // bake ASCII printable range (32–126)
    for (uint32_t cp = 32; cp <= 126; ++cp) {
        FT_UInt gi = FT_Get_Char_Index(face, cp);
        if (FT_Load_Glyph(face, gi, FT_LOAD_NO_BITMAP) != 0) continue;

        FT_GlyphSlot slot    = face->glyph;
        const double scale   = 1.0 / 64.0;
        const double bx      = slot->metrics.horiBearingX * scale;
        const double by      = slot->metrics.horiBearingY * scale;
        const double gw      = slot->metrics.width        * scale;
        const double gh      = slot->metrics.height       * scale;
        const double advance = slot->metrics.horiAdvance  * scale;

        int cell_w = (int)std::ceil(gw) + 2 * pad;
        int cell_h = (int)std::ceil(gh) + 2 * pad;

        // whitespace — record metrics only
        msdfgen::Shape shape;
        shape.inverseYAxis = false;
        ShapeCtx ctx{ &shape, nullptr, scale, {0, 0} };
        FT_Outline_Decompose(&slot->outline, &k_ft_funcs, &ctx);
        if (!shape.contours.empty() && shape.contours.back().edges.empty())
            shape.contours.pop_back();

        if (shape.contours.empty()) {
            PackedGlyphMetrics gm{};
            gm.codepoint = cp;
            gm.uv_x0 = gm.uv_y0 = gm.uv_x1 = gm.uv_y1 = 0.0f;
            gm.bearing_x = (float)bx;
            gm.bearing_y = (float)by;
            gm.advance   = (float)advance;
            gm.width     = (float)gw;
            gm.height    = (float)gh;
            glyphs.push_back(gm);
            continue;
        }

        // shelf wrap
        if (pen_x + cell_w + 1 > (int)atlas_w) {
            pen_x  = 1;
            pen_y += row_h + 1;
            row_h  = 0;
        }
        if (pen_y + cell_h + 1 > (int)atlas_h) continue;

        row_h = std::max(row_h, cell_h);

        shape.normalize();
        msdfgen::edgeColoringInkTrap(shape, 3.0);

        msdfgen::Vector2 proj_scale(1.0, 1.0);
        msdfgen::Vector2 proj_trans(pad - bx, pad - by + gh);
        msdfgen::Projection proj(proj_scale, proj_trans);
        msdfgen::Range      range(px_range);

        msdfgen::Bitmap<float, 4> bmp(cell_w, cell_h);
        msdfgen::generateMTSDF(bmp, shape, proj, range);

        for (int row = 0; row < cell_h; ++row) {
            for (int col = 0; col < cell_w; ++col) {
                const float *src = bmp(col, cell_h - 1 - row);
                int ax = pen_x + col;
                int ay = pen_y + row;
                uint8_t *dst = &pixels[(ay * (int)atlas_w + ax) * 4];
                auto clamp8 = [](float v) -> uint8_t {
                    int i = (int)(v * 255.0f + 0.5f);
                    return (uint8_t)(i < 0 ? 0 : i > 255 ? 255 : i);
                };
                dst[0] = clamp8(src[0]);
                dst[1] = clamp8(src[1]);
                dst[2] = clamp8(src[2]);
                dst[3] = clamp8(src[3]);
            }
        }

        PackedGlyphMetrics gm{};
        gm.codepoint = cp;
        gm.uv_x0 = (pen_x + pad)                       / (float)atlas_w;
        gm.uv_y0 = (pen_y + pad)                       / (float)atlas_h;
        gm.uv_x1 = (pen_x + pad + (int)std::ceil(gw)) / (float)atlas_w;
        gm.uv_y1 = (pen_y + pad + (int)std::ceil(gh)) / (float)atlas_h;
        gm.bearing_x = (float)bx;
        gm.bearing_y = (float)by;
        gm.advance   = (float)advance;
        gm.width     = (float)gw;
        gm.height    = (float)gh;
        glyphs.push_back(gm);

        pen_x += cell_w + 1;
    }

    FT_Done_Face(face);
    FT_Done_FreeType(lib);

    // pack: atlas pixels followed by glyph metrics table
    size_t pixel_bytes = (size_t)atlas_w * atlas_h * 4;
    size_t metrics_bytes = glyphs.size() * sizeof(PackedGlyphMetrics);
    out.data.resize(pixel_bytes + metrics_bytes);
    memcpy(out.data.data(), pixels.data(), pixel_bytes);
    memcpy(out.data.data() + pixel_bytes, glyphs.data(), metrics_bytes);
    out.atlas_w = atlas_w;
    out.atlas_h = atlas_h;
    out.glyph_count = (uint32_t)glyphs.size();
    out.em_size = em_size;

    return out;
}
