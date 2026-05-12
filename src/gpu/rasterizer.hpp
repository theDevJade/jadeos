#pragma once
#include "framebuffer.hpp"
#include <cstdint>
#include <span>

namespace gpu {

// Software triangle rasterizer (scanline fill).
void rasterize_triangle(
    std::span<uint8_t> fb,
    uint32_t fb_w, uint32_t fb_h,
    int32_t x0, int32_t y0,
    int32_t x1, int32_t y1,
    int32_t x2, int32_t y2,
    RGBA colour
);

// Bresenham line rasterizer.
void rasterize_line(
    std::span<uint8_t> fb,
    uint32_t fb_w, uint32_t fb_h,
    int32_t x0, int32_t y0,
    int32_t x1, int32_t y1,
    RGBA colour
);

// Wu's antialiased line.
void rasterize_line_aa(
    std::span<uint8_t> fb,
    uint32_t fb_w, uint32_t fb_h,
    int32_t x0, int32_t y0,
    int32_t x1, int32_t y1,
    RGBA colour
);

// Axis-aligned filled rectangle (opaque, fast row memset).
void rasterize_rect(
    std::span<uint8_t> fb,
    uint32_t fb_w, uint32_t fb_h,
    int32_t x, int32_t y,
    int32_t w, int32_t h,
    RGBA colour
);

// Axis-aligned filled rectangle with alpha compositing.
void rasterize_rect_alpha(
    std::span<uint8_t> fb,
    uint32_t fb_w, uint32_t fb_h,
    int32_t x, int32_t y,
    int32_t w, int32_t h,
    RGBA colour
);

// Filled circle.  inner_r == 0 -> solid disk;  inner_r > 0 -> annulus (ring).
void rasterize_circle(
    std::span<uint8_t> fb,
    uint32_t fb_w, uint32_t fb_h,
    int32_t cx, int32_t cy,
    int32_t r, int32_t inner_r,
    RGBA colour
);

}  // namespace gpu
