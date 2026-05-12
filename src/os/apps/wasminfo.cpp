#include "wasminfo.hpp"
#include <cstdio>
#include <string>

namespace os::apps {

static void row(gpu::GPU& g, int x0, int vy, int col_val,
                const char* label, const char* value,
                uint32_t label_col, uint32_t val_col)
{
    g_text(g, x0, vy, label_col, 0, label);
    g_text(g, col_val, vy, val_col, 0, value);
}

void render_wasminfo(gpu::GPU& g, WinRect area,
                     uint64_t cpu_cycles,
                     uint64_t gpu_draw_calls,
                     uint32_t fb_w, uint32_t fb_h,
                     std::size_t mem_bytes,
                     uint32_t tick,
                     float dpr)
{
    auto sc = [dpr](int n) { return int(n * dpr + 0.5f); };
    const int x0 = area.x, y0 = area.y;

    g_rect(g, x0, y0, area.w, area.h, 0xFF'06'0E'1A);
    g_text(g, x0 + sc(10), y0 + sc(14), 0xFF'2A'90'CF, 0, "WASM INFO");
    g_line(g, x0 + sc(8), y0 + sc(20), x0 + area.w - sc(8), y0 + sc(20), 0xFF'1A'3A'55);

    const int COL_L   = x0 + sc(12);
    const int COL_V   = x0 + sc(140);
    const int ROW_H   = sc(18);
    int vy = y0 + sc(36);

    char buf[48];

    g_text(g, COL_L, vy, 0xFF'50'C8'FF, 0, "RUNTIME");
    vy += sc(4);
    g_line(g, COL_L, vy, x0 + area.w - sc(12), vy, 0xFF'1A'3A'55);
    vy += sc(4);

    row(g, COL_L, vy += ROW_H, COL_V, "Platform",  "WebAssembly (Emscripten)", 0xFF'50'78'90, 0xFF'89'B4'FA);
    row(g, COL_L, vy += ROW_H, COL_V, "ISA",       "JadeISA rv32 8-GPR ring0/3",0xFF'50'78'90, 0xFF'89'B4'FA);

    std::snprintf(buf, sizeof(buf), "%zu MiB linear", mem_bytes / (1024u * 1024u));
    row(g, COL_L, vy += ROW_H, COL_V, "WASM Mem",  buf, 0xFF'50'78'90, 0xFF'89'B4'FA);

    std::snprintf(buf, sizeof(buf), "%u x %u px", fb_w, fb_h);
    row(g, COL_L, vy += ROW_H, COL_V, "Framebuffer", buf, 0xFF'50'78'90, 0xFF'89'B4'FA);

    row(g, COL_L, vy += ROW_H, COL_V, "Renderer",  "WebGPU (WGSL instanced)",  0xFF'50'78'90, 0xFF'40'CC'60);
    row(g, COL_L, vy += ROW_H, COL_V, "Fonts",     "stb_truetype  3 atlas sizes",0xFF'50'78'90, 0xFF'89'B4'FA);

    vy += sc(8);

    g_text(g, COL_L, vy, 0xFF'50'C8'FF, 0, "CPU STATS");
    vy += sc(4);
    g_line(g, COL_L, vy, x0 + area.w - sc(12), vy, 0xFF'1A'3A'55);
    vy += sc(4);

    std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)cpu_cycles);
    row(g, COL_L, vy += ROW_H, COL_V, "Total cycles", buf, 0xFF'50'78'90, 0xFF'F0'C0'50);

    std::snprintf(buf, sizeof(buf), "%u ticks (~%.1f s)",
                  tick, float(tick) / 120.0f);
    row(g, COL_L, vy += ROW_H, COL_V, "Uptime", buf, 0xFF'50'78'90, 0xFF'F0'C0'50);

    vy += sc(8);

    g_text(g, COL_L, vy, 0xFF'50'C8'FF, 0, "GPU STATS");
    vy += sc(4);
    g_line(g, COL_L, vy, x0 + area.w - sc(12), vy, 0xFF'1A'3A'55);
    vy += sc(4);

    std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)gpu_draw_calls);
    row(g, COL_L, vy += ROW_H, COL_V, "Draw calls",  buf, 0xFF'50'78'90, 0xFF'A0'60'D0);

    vy += sc(8);

    g_text(g, COL_L, vy, 0xFF'50'C8'FF, 0, "BUILD");
    vy += sc(4);
    g_line(g, COL_L, vy, x0 + area.w - sc(12), vy, 0xFF'1A'3A'55);
    vy += sc(4);

    row(g, COL_L, vy += ROW_H, COL_V, "Toolchain", "Emscripten 3.x / Clang",   0xFF'50'78'90, 0xFF'89'B4'FA);
    row(g, COL_L, vy += ROW_H, COL_V, "Standard",  "C++20",                    0xFF'50'78'90, 0xFF'89'B4'FA);
    row(g, COL_L, vy += ROW_H, COL_V, "Build sys", "Meson + Ninja",            0xFF'50'78'90, 0xFF'89'B4'FA);
    row(g, COL_L, vy += ROW_H, COL_V, "Target",    "wasm32-unknown-emscripten",0xFF'50'78'90, 0xFF'40'CC'60);
    row(g, COL_L, vy += ROW_H, COL_V, "Features",  "SIMD off / threads off",   0xFF'50'78'90, 0xFF'89'B4'FA);
}

} // namespace os::apps
