#pragma once
#include "framebuffer.hpp"
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <array>

namespace gpu {

// Scissor rectangle for clipped draw calls (x1/y1 are exclusive).
// Defined outside FontAtlas so it can be used as a default argument.
struct FontClipRect { int32_t x0=0, y0=0, x1=0x7FFFFFFF, y1=0x7FFFFFFF; };

// Bakes a TrueType font at a fixed pixel size into a greyscale atlas texture,
// then provides fast glyph-blitting into RGBA framebuffers.
//
// Supported codepoint range: ASCII 0x20-0x7E (printable + DEL).
// Atlas is stored as a flat vector<uint8_t> (1 byte per pixel, A8 format).
//
// Thread-safety: none (single-threaded emulator loop).

struct GlyphInfo {
    // Screen-space metrics (in pixels, already include oversampling correction)
    float xoff;       // pen_x + xoff  = left edge of glyph rect
    float yoff;       // pen_y + yoff  = top  edge of glyph rect
    float xoff2;      // pen_x + xoff2 = right  edge of glyph rect
    float yoff2;      // pen_y + yoff2 = bottom edge of glyph rect
    float advance_x;  // how far to advance the pen after this glyph
    // Atlas location (texels, 1:1 with screen pixels, oversampling=1)
    int atlas_x0, atlas_y0, atlas_x1, atlas_y1;
};

class FontAtlas {
public:
    FontAtlas() = default;

    // Load a TTF/OTF from raw bytes and bake at `pixel_height`.
    // Returns true on success.
    bool load(std::span<const uint8_t> ttf_data, float pixel_height);

    bool is_loaded() const noexcept { return loaded_; }
    float line_height() const noexcept { return line_height_; }
    int   atlas_width()  const noexcept { return atlas_w_; }
    int   atlas_height() const noexcept { return atlas_h_; }
    const std::vector<uint8_t>& atlas_pixels() const noexcept { return pixels_; }

    // Scissor rectangle used by clipped draw calls (x1/y1 are exclusive).
    using ClipRect = FontClipRect;

    // Draw a single ASCII character at (cx, cy) - cy is the baseline.
    // Returns the new X cursor position after drawing.
    float draw_char(std::span<uint8_t> fb, uint32_t fb_w, uint32_t fb_h,
                    float cx, float cy,
                    char ch, RGBA colour,
                    const FontClipRect& clip = {}) const;

    // Draw a UTF-8 string. Newlines advance by line_height_.
    // Returns final cursor X.
    float draw_string(std::span<uint8_t> fb, uint32_t fb_w, uint32_t fb_h,
                      float x, float y,
                      std::string_view text, RGBA colour,
                      const FontClipRect& clip = {}) const;

    // Measure a string without drawing (for layout / centering).
    float measure_width(std::string_view text) const;

    // Access raw glyph data for WebGPU text rendering.
    const GlyphInfo* glyph_array() const noexcept { return glyphs_.data(); }
    static constexpr int kFirstChar = 0x20;
    static constexpr int kNumChars  = 96;
    float ascent() const noexcept { return ascent_; }

private:
    static constexpr int FIRST_CHAR   = 0x20;  // space
    static constexpr int NUM_CHARS     = 96;    // 0x20-0x7F
    static constexpr int ATLAS_W_INIT  = 2048;
    static constexpr int ATLAS_H_INIT  = 2048;

    bool loaded_      = false;
    float line_height_= 0.f;
    float ascent_     = 0.f;
    int   atlas_w_    = 0;
    int   atlas_h_    = 0;
    std::vector<uint8_t>                    pixels_;
    std::array<GlyphInfo, NUM_CHARS>        glyphs_{};
};

}  // namespace gpu
