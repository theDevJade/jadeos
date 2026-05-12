#include "rasterizer.hpp"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <vector>

namespace gpu {

static void plot(std::span<uint8_t> fb, uint32_t w, uint32_t h,
                 int32_t x, int32_t y, RGBA col)
{
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= w ||
                           static_cast<uint32_t>(y) >= h) return;
    std::size_t off = (static_cast<std::size_t>(y) * w + x) * 4;
    fb[off+0] = col.r; fb[off+1] = col.g;
    fb[off+2] = col.b; fb[off+3] = col.a;
}

// Alpha-blend col at coverage 'alpha' over whatever is in fb.
static void plot_aa(std::span<uint8_t> fb, uint32_t w, uint32_t h,
                    int32_t x, int32_t y, RGBA col, uint8_t alpha)
{
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= w ||
                           static_cast<uint32_t>(y) >= h) return;
    std::size_t off = (static_cast<std::size_t>(y) * w + x) * 4;
    uint32_t a = alpha, ia = 255 - a;
    fb[off+0] = static_cast<uint8_t>((col.r * a + fb[off+0] * ia) >> 8);
    fb[off+1] = static_cast<uint8_t>((col.g * a + fb[off+1] * ia) >> 8);
    fb[off+2] = static_cast<uint8_t>((col.b * a + fb[off+2] * ia) >> 8);
    fb[off+3] = 0xFF;
}

void rasterize_line(std::span<uint8_t> fb, uint32_t w, uint32_t h,
                    int32_t x0, int32_t y0, int32_t x1, int32_t y1, RGBA col)
{
    int32_t dx =  std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int32_t dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    while (true) {
        plot(fb, w, h, x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void rasterize_rect(std::span<uint8_t> fb, uint32_t fw, uint32_t fh,
                    int32_t x, int32_t y, int32_t rw, int32_t rh, RGBA col)
{
    // Clamp to framebuffer bounds.
    const int32_t x0 = std::max(x,  0);
    const int32_t y0 = std::max(y,  0);
    const int32_t x1 = std::min(x + rw, static_cast<int32_t>(fw));
    const int32_t y1 = std::min(y + rh, static_cast<int32_t>(fh));
    if (x0 >= x1 || y0 >= y1) return;

    const int32_t span_w = x1 - x0;

    // Build a single row of pixels then memcpy each row.
    // For solid colours this is much faster than plot() per pixel.
    std::vector<uint8_t> row(static_cast<std::size_t>(span_w) * 4);
    for (int32_t i = 0; i < span_w; ++i) {
        row[i*4+0] = col.r; row[i*4+1] = col.g;
        row[i*4+2] = col.b; row[i*4+3] = col.a;
    }

    for (int32_t row_y = y0; row_y < y1; ++row_y) {
        std::size_t off = (static_cast<std::size_t>(row_y) * fw + x0) * 4;
        std::memcpy(&fb[off], row.data(), row.size());
    }
}

void rasterize_rect_alpha(std::span<uint8_t> fb, uint32_t fw, uint32_t fh,
                          int32_t x, int32_t y, int32_t rw, int32_t rh, RGBA col)
{
    if (col.a == 0xFF) { rasterize_rect(fb, fw, fh, x, y, rw, rh, col); return; }
    if (col.a == 0x00) return;

    const int32_t x0 = std::max(x,  0);
    const int32_t y0 = std::max(y,  0);
    const int32_t x1 = std::min(x + rw, static_cast<int32_t>(fw));
    const int32_t y1 = std::min(y + rh, static_cast<int32_t>(fh));
    if (x0 >= x1 || y0 >= y1) return;

    const uint32_t a  = col.a;
    const uint32_t ia = 255 - a;

    for (int32_t py = y0; py < y1; ++py) {
        std::size_t off = (static_cast<std::size_t>(py) * fw + x0) * 4;
        for (int32_t px = x0; px < x1; ++px, off += 4) {
            fb[off+0] = static_cast<uint8_t>((col.r * a + fb[off+0] * ia) >> 8);
            fb[off+1] = static_cast<uint8_t>((col.g * a + fb[off+1] * ia) >> 8);
            fb[off+2] = static_cast<uint8_t>((col.b * a + fb[off+2] * ia) >> 8);
            fb[off+3] = 0xFF;
        }
    }
}

void rasterize_triangle(std::span<uint8_t> fb, uint32_t fw, uint32_t fh,
                        int32_t x0, int32_t y0,
                        int32_t x1, int32_t y1,
                        int32_t x2, int32_t y2, RGBA col)
{
    // Sort vertices by Y (bubble).
    if (y0 > y1) { std::swap(x0,x1); std::swap(y0,y1); }
    if (y0 > y2) { std::swap(x0,x2); std::swap(y0,y2); }
    if (y1 > y2) { std::swap(x1,x2); std::swap(y1,y2); }

    auto interp = [](int32_t ya, int32_t yb, int32_t xa, int32_t xb, int32_t y) -> int32_t {
        if (yb == ya) return xa;
        return xa + (xb - xa) * (y - ya) / (yb - ya);
    };

    for (int32_t y = y0; y <= y2; ++y) {
        int32_t lx, rx;
        if (y < y1) {
            lx = interp(y0, y1, x0, x1, y);
            rx = interp(y0, y2, x0, x2, y);
        } else {
            lx = interp(y1, y2, x1, x2, y);
            rx = interp(y0, y2, x0, x2, y);
        }
        if (lx > rx) std::swap(lx, rx);
        for (int32_t xi = lx; xi <= rx; ++xi)
            plot(fb, fw, fh, xi, y, col);
    }
}

void rasterize_line_aa(std::span<uint8_t> fb,
                       uint32_t fb_w, uint32_t fb_h,
                       int32_t x0, int32_t y0,
                       int32_t x1, int32_t y1,
                       RGBA colour)
{
    auto ipart  = [](float v) { return static_cast<int>(std::floor(v)); };
    auto fpart  = [](float v) { return v - std::floor(v); };
    auto rfpart = [&fpart](float v) { return 1.f - fpart(v); };

    float fx0 = static_cast<float>(x0), fy0 = static_cast<float>(y0);
    float fx1 = static_cast<float>(x1), fy1 = static_cast<float>(y1);

    const bool steep = std::abs(fy1 - fy0) > std::abs(fx1 - fx0);
    if (steep) { std::swap(fx0, fy0); std::swap(fx1, fy1); }
    if (fx0 > fx1) { std::swap(fx0, fx1); std::swap(fy0, fy1); }

    const float dx = fx1 - fx0, dy = fy1 - fy0;
    const float gradient = dx == 0.f ? 1.f : dy / dx;

    float intery = fy0 + gradient * (std::round(fx0) - fx0);

    auto draw_pixel = [&](int xi, int yi, float brightness) {
        uint8_t a = static_cast<uint8_t>(brightness * colour.a);
        if (steep) plot_aa(fb, fb_w, fb_h, yi, xi, colour, a);
        else       plot_aa(fb, fb_w, fb_h, xi, yi, colour, a);
    };

    for (int xi = ipart(fx0); xi <= ipart(fx1); ++xi, intery += gradient) {
        draw_pixel(xi, ipart(intery),     rfpart(intery));
        draw_pixel(xi, ipart(intery) + 1, fpart(intery));
    }
}

void rasterize_circle(std::span<uint8_t> fb,
                      uint32_t fb_w, uint32_t fb_h,
                      int32_t cx, int32_t cy,
                      int32_t r, int32_t inner_r,
                      RGBA colour)
{
    if (r <= 0) return;
    const int32_t r2     = r * r;
    const int32_t inner2 = inner_r * inner_r;

    for (int32_t dy = -r; dy <= r; ++dy) {
        const int32_t dist2_row_end = r2 - dy * dy;
        if (dist2_row_end < 0) continue;
        const int32_t row_r = static_cast<int32_t>(std::sqrt(static_cast<float>(dist2_row_end)));

        for (int32_t dx = -row_r; dx <= row_r; ++dx) {
            const int32_t d2 = dx * dx + dy * dy;
            if (d2 > r2) continue;
            if (inner_r > 0 && d2 < inner2) continue;
            plot(fb, fb_w, fb_h, cx + dx, cy + dy, colour);
        }
    }
}

}  // namespace gpu
