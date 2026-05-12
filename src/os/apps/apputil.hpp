#pragma once
#include "../../gpu/gpu.hpp"
#include "../wm.hpp"
#include <string>
#include <cstdio>

namespace os::apps {

inline void g_rect(gpu::GPU& g, int x, int y, int w, int h, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    gpu::CmdPacket p;
    p.cmd    = gpu::Command::DRAW_RECT;
    p.args   = { uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h), c, 0 };
    g.push_command(p);
}

inline void g_rect_a(gpu::GPU& g, int x, int y, int w, int h, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    gpu::CmdPacket p;
    p.cmd    = gpu::Command::DRAW_RECT_ALPHA;
    p.args   = { uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h), c, 0 };
    g.push_command(p);
}

inline void g_line(gpu::GPU& g, int x0, int y0, int x1, int y1, uint32_t c) {
    gpu::CmdPacket p;
    p.cmd    = gpu::Command::DRAW_LINE;
    p.args   = { uint32_t(x0), uint32_t(y0), uint32_t(x1), uint32_t(y1), c, 0 };
    g.push_command(p);
}

inline void g_circle(gpu::GPU& g, int cx, int cy, int r, uint32_t c) {
    if (r <= 0) return;
    gpu::CmdPacket p;
    p.cmd    = gpu::Command::DRAW_CIRCLE;
    p.args   = { uint32_t(cx), uint32_t(cy), uint32_t(r), 0, c, 0 };
    g.push_command(p);
}

inline void g_text(gpu::GPU& g, int x, int y, uint32_t col, uint8_t sz, const std::string& s) {
    if (s.empty()) return;
    gpu::TextRequest r;
    r.x            = float(x);
    r.y            = float(y);
    r.colour       = gpu::RGBA::from_u32(col);
    r.font_size_id = sz;
    r.text         = s;
    g.draw_text(r);
}

inline void g_scissor(gpu::GPU& g, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    gpu::CmdPacket p;
    p.cmd    = gpu::Command::SET_SCISSOR;
    p.args   = { uint32_t(x), uint32_t(y), uint32_t(x + w), uint32_t(y + h), 0, 0 };
    g.push_command(p);
}

inline void g_scissor_clear(gpu::GPU& g) {
    gpu::CmdPacket p;
    p.cmd = gpu::Command::CLEAR_SCISSOR;
    g.push_command(p);
}

} // namespace os::apps
