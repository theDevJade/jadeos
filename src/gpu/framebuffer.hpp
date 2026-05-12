#pragma once
#include <cstdint>

namespace gpu {

// VRAM starts at 2 MiB so the first 2 MiB is always safe for code/stack.
static constexpr uint32_t VRAM_BASE       = 0x200000;  // 2 MiB
static constexpr uint32_t BYTES_PER_PIXEL = 4;         // RGBA8

// Default framebuffer dimensions; may be overridden at runtime via GPU::init().
static constexpr uint32_t FB_W_DEFAULT = 1280;
static constexpr uint32_t FB_H_DEFAULT = 720;

// Minimum emulated RAM bytes for a given framebuffer size.
inline uint32_t min_ram_bytes(uint32_t w, uint32_t h) noexcept {
    return VRAM_BASE + w * h * BYTES_PER_PIXEL;
}

enum class Command : uint32_t {
    CLEAR            = 0x01,  // args[0]: RGBA colour
    DRAW_PIXEL       = 0x02,  // args: x, y, colour
    DRAW_LINE        = 0x03,  // args: x0, y0, x1, y1, colour
    DRAW_RECT        = 0x04,  // args: x, y, w, h, colour (filled, opaque)
    DRAW_TRIANGLE    = 0x05,  // args: x0,y0, x1,y1, packed(x2|y2<<16), colour
    BLIT             = 0x06,  // args: src_addr, x, y, w, h
    DRAW_RECT_ALPHA  = 0x07,  // args: x, y, w, h, colour (alpha-composited)
    DRAW_CIRCLE      = 0x08,  // args: cx, cy, r, inner_r (0=filled), colour
    DRAW_LINE_AA     = 0x09,  // args: x0, y0, x1, y1, colour  (Wu antialiased)
    SET_SCISSOR      = 0x0A,  // args: x0, y0, x1, y1 (exclusive end; clips following draws)
    CLEAR_SCISSOR    = 0x0B,  // no args; clears scissor to full framebuffer
    FLIP             = 0xFF,
};

struct RGBA {
    uint8_t r, g, b, a;
    static RGBA from_u32(uint32_t c) noexcept {
        // Color literals are 0xAARRGGBB.
        return { uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c), uint8_t(c >> 24) };
    }
    uint32_t to_u32() const noexcept {
        return (uint32_t(r)<<24)|(uint32_t(g)<<16)|(uint32_t(b)<<8)|a;
    }
};

}  // namespace gpu
