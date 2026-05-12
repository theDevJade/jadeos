// stb_truetype implementation unit - compiled exactly once.
#define STB_TRUETYPE_IMPLEMENTATION
#include "../vendor/stb_truetype.h"

#include "font.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace gpu {

bool FontAtlas::load(std::span<const uint8_t> ttf_data, float pixel_height) {
    loaded_ = false;
    if (ttf_data.empty()) return false;

    stbtt_fontinfo info{};
    if (!stbtt_InitFont(&info,
                        ttf_data.data(),
                        stbtt_GetFontOffsetForIndex(ttf_data.data(), 0))) {
        return false;
    }

    float scale = stbtt_ScaleForPixelHeight(&info, pixel_height);

    int asc, desc, line_gap;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &line_gap);
    ascent_      = std::roundf(static_cast<float>(asc)  * scale);
    line_height_ = std::roundf(static_cast<float>(asc - desc + line_gap) * scale);

    // Start with ATLAS_W_INIT x ATLAS_H_INIT; double height if needed.
    atlas_w_ = ATLAS_W_INIT;
    atlas_h_ = ATLAS_H_INIT;

    std::vector<stbtt_packedchar> packed(NUM_CHARS);
    stbtt_pack_context ctx{};

    bool packed_ok = false;
    for (int attempt = 0; attempt < 4 && !packed_ok; ++attempt) {
        pixels_.assign(static_cast<std::size_t>(atlas_w_ * atlas_h_), 0);
        stbtt_PackBegin(&ctx, pixels_.data(), atlas_w_, atlas_h_,
                        /*stride*/ 0, /*padding*/ 2, nullptr);
        // 2x horizontal oversampling  -  sub-pixel accuracy for cleaner text.
        stbtt_PackSetOversampling(&ctx, 2, 1);
        if (stbtt_PackFontRange(&ctx, ttf_data.data(), 0, pixel_height,
                                FIRST_CHAR, NUM_CHARS, packed.data())) {
            packed_ok = true;
        } else {
            stbtt_PackEnd(&ctx);
            atlas_h_ *= 2;  // grow and retry
        }
    }
    if (!packed_ok) return false;
    stbtt_PackEnd(&ctx);

    for (int i = 0; i < NUM_CHARS; ++i) {
        const auto& pc = packed[i];
        GlyphInfo& g   = glyphs_[i];
        g.xoff      = pc.xoff;
        g.yoff      = pc.yoff;
        g.xoff2     = pc.xoff2;
        g.yoff2     = pc.yoff2;
        g.advance_x = pc.xadvance;
        g.atlas_x0  = pc.x0;
        g.atlas_y0  = pc.y0;
        g.atlas_x1  = pc.x1;
        g.atlas_y1  = pc.y1;
    }

    loaded_ = true;
    return true;
}

float FontAtlas::draw_char(std::span<uint8_t> fb,
                           uint32_t fb_w, uint32_t fb_h,
                           float cx, float cy,
                           char ch, RGBA colour,
                           const FontClipRect& clip) const {
    if (!loaded_) return cx;

    const int idx = static_cast<unsigned char>(ch) - FIRST_CHAR;
    if (idx < 0 || idx >= NUM_CHARS) return cx;

    const GlyphInfo& g = glyphs_[idx];

    // Screen-space glyph rectangle (pen position + bearing offsets).
    const int sx0 = static_cast<int>(std::roundf(cx + g.xoff));
    const int sy0 = static_cast<int>(std::roundf(cy + g.yoff));
    const int sw  = static_cast<int>(std::roundf(g.xoff2 - g.xoff));
    const int sh  = static_cast<int>(std::roundf(g.yoff2 - g.yoff));

    if (sw <= 0 || sh <= 0) return cx + g.advance_x;

    // With 2x horizontal oversampling the atlas is wider than sw.
    // Map screen pixel px -> atlas column proportionally.
    const int atlas_sw = g.atlas_x1 - g.atlas_x0;
    const int atlas_sh = g.atlas_y1 - g.atlas_y0;

    for (int py = 0; py < sh; ++py) {
        const int fb_y = sy0 + py;
        if (fb_y < 0 || static_cast<uint32_t>(fb_y) >= fb_h) continue;
        if (fb_y < clip.y0 || fb_y >= clip.y1) continue;

        const int ay = g.atlas_y0 + (atlas_sh > sh
            ? py * atlas_sh / sh : py);

        for (int px = 0; px < sw; ++px) {
            const int fb_x = sx0 + px;
            if (fb_x < 0 || static_cast<uint32_t>(fb_x) >= fb_w) continue;
            if (fb_x < clip.x0 || fb_x >= clip.x1) continue;

            // Proportional atlas-x lookup handles any oversampling ratio.
            const int ax = g.atlas_x0 + (sw > 0 ? px * atlas_sw / sw : px);
            if (ax >= atlas_w_ || ay >= atlas_h_) continue;

            const uint8_t alpha = pixels_[static_cast<std::size_t>(ay) * atlas_w_ + ax];
            if (alpha == 0) continue;

            const std::size_t off = (static_cast<std::size_t>(fb_y) * fb_w + fb_x) * 4;
            const uint32_t a = alpha, ia = 255 - a;
            fb[off+0] = static_cast<uint8_t>((colour.r * a + fb[off+0] * ia) >> 8);
            fb[off+1] = static_cast<uint8_t>((colour.g * a + fb[off+1] * ia) >> 8);
            fb[off+2] = static_cast<uint8_t>((colour.b * a + fb[off+2] * ia) >> 8);
            fb[off+3] = 0xFF;
        }
    }

    return cx + g.advance_x;
}

float FontAtlas::draw_string(std::span<uint8_t> fb,
                             uint32_t fb_w, uint32_t fb_h,
                             float x, float y,
                             std::string_view text, RGBA colour,
                             const FontClipRect& clip) const {
    if (!loaded_) return x;
    float cx = x;
    for (char ch : text) {
        if (ch == '\n') {
            cx = x;
            y += line_height_;
            continue;
        }
        cx = draw_char(fb, fb_w, fb_h, cx, y, ch, colour, clip);
    }
    return cx;
}

float FontAtlas::measure_width(std::string_view text) const {
    if (!loaded_) return 0.f;
    float w = 0.f, line_w = 0.f;
    for (char ch : text) {
        if (ch == '\n') { w = std::max(w, line_w); line_w = 0.f; continue; }
        const int idx = static_cast<unsigned char>(ch) - FIRST_CHAR;
        if (idx >= 0 && idx < NUM_CHARS)
            line_w += glyphs_[idx].advance_x;
    }
    return std::max(w, line_w);
}

}  // namespace gpu
