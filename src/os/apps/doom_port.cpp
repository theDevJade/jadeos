// Freedoom player (doomgeneric / Chocolate-derived). Emscripten-only TU.

#include "doom_port.hpp"
#include "apputil.hpp"
#include "cpu/memory.hpp"
#include "gpu/gpu.hpp"

#include "doomkeys.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unistd.h>

extern "C" {
#include "doomgeneric.h"
void doomgeneric_Create(int argc, char** argv);
void doomgeneric_Tick(void);
}

extern "C" void jade_doom_feed_key(int pressed, unsigned char doom_key);
extern "C" void jade_doom_feed_mouse(int buttons, int dx, int dy);
extern "C" unsigned int D_GrabMouseCallback(void);

#include <cstdint>

namespace {

constexpr uint32_t kStagingPhys = 0x700000u;
// Cap RGBA staging (scale_buf_): maximized HiDPI windows can otherwise ask for >1 GiB and abort WASM.
constexpr std::size_t kMaxDoomScalePixels = 10u * 1024u * 1024u;  // ~40 MiB at 4 bytes/px
constexpr std::size_t kMaxDoomScaleBytes = kMaxDoomScalePixels * 4u;

gpu::GPU*          g_blit_gpu = nullptr;
cpu::Memory*       g_blit_mem = nullptr;
int                g_blit_dx = 0, g_blit_dy = 0, g_blit_dw = 0, g_blit_dh = 0;
std::vector<uint8_t>* g_blit_buf = nullptr;

static unsigned char dom_vk_to_doom(uint32_t vk)
{
    switch (vk) {
    case 37:  return KEY_LEFTARROW;
    case 39:  return KEY_RIGHTARROW;
    case 38:  return KEY_UPARROW;
    case 40:  return KEY_DOWNARROW;
    case 32:  return KEY_USE;
    case 27:  return KEY_ESCAPE;
    case 13:  return KEY_ENTER;
    case 9:   return KEY_TAB;
    case 8:   return KEY_BACKSPACE;
    case 17:  return KEY_FIRE;
    default:  break;
    }
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<unsigned char>(std::tolower(static_cast<int>(vk)));
    if (vk >= '0' && vk <= '9')
        return static_cast<unsigned char>(vk);
    return 0;
}

}  // namespace

extern "C" void jade_doom_present(const uint32_t* src, int sw, int sh)
{
    if (!g_blit_gpu || !g_blit_mem || !g_blit_buf || !src || sw <= 0 || sh <= 0)
        return;
    if (g_blit_dw < 2 || g_blit_dh < 2)
        return;

    gpu::GPU&    gpu = *g_blit_gpu;
    cpu::Memory& mem = *g_blit_mem;

    const std::size_t need = static_cast<std::size_t>(g_blit_dw) * static_cast<std::size_t>(g_blit_dh) * 4u;
    if (need > kMaxDoomScaleBytes || kStagingPhys + need > mem.size())
        return;

    g_blit_buf->resize(need);
    uint8_t* dst = g_blit_buf->data();

    for (int y = 0; y < g_blit_dh; ++y) {
        const int sy = static_cast<int>((int64_t)y * sh / g_blit_dh);
        for (int x = 0; x < g_blit_dw; ++x) {
            const int sx = static_cast<int>((int64_t)x * sw / g_blit_dw);
            const uint32_t p = src[sy * sw + sx];
            const std::size_t o = (static_cast<std::size_t>(y) * static_cast<std::size_t>(g_blit_dw) + static_cast<std::size_t>(x)) * 4u;
            dst[o + 0] = static_cast<uint8_t>((p >> 16) & 0xFFu);
            dst[o + 1] = static_cast<uint8_t>((p >> 8) & 0xFFu);
            dst[o + 2] = static_cast<uint8_t>(p & 0xFFu);
            dst[o + 3] = 255;
        }
    }

    if (uint8_t* phys = mem.raw_ptr_mut(kStagingPhys))
        std::memcpy(phys, dst, need);
    else
        return;

    gpu::CmdPacket pkt;
    pkt.cmd = gpu::Command::BLIT;
    pkt.args = {
        kStagingPhys,
        static_cast<uint32_t>(g_blit_dx),
        static_cast<uint32_t>(g_blit_dy),
        static_cast<uint32_t>(g_blit_dw),
        static_cast<uint32_t>(g_blit_dh),
        0,
    };
    gpu.push_command(pkt);
}

namespace os {

void DoomPort::set_context(gpu::GPU* g, cpu::Memory* m) noexcept
{
    gpu_ = g;
    mem_ = m;
}

void DoomPort::draw_placeholder(gpu::GPU& g, WinRect area, float dpr) const
{
    auto sc = [dpr](int n) { return int(n * dpr + 0.5f); };
    apps::g_rect(g, area.x, area.y, area.w, area.h, 0xFF'0A'0C'10);
    apps::g_text(g, area.x + sc(12), area.y + sc(16), 0xFF'88'44'22, 1, "FREEDOOM.APP");
    apps::g_text(g, area.x + sc(12), area.y + sc(44), 0xFF'B0'90'70, 0,
           "Doomgeneric (GPL) in-process. Add web/freedoom1.wad (extract from");
    apps::g_text(g, area.x + sc(12), area.y + sc(62), 0xFF'70'60'50, 0,
           "the official Freedoom release zip) then reload.");
    if (!err_.empty())
        apps::g_text(g, area.x + sc(12), area.y + sc(90), 0xFF'CC'44'44, 0, err_);
}

void DoomPort::try_boot()
{
    if (started_ || failed_)
        return;
    if (access("/freedoom1.wad", F_OK) != 0 && !iwad_ready_) {
        err_ = "IWAD not found at /freedoom1.wad";
        failed_ = true;
        return;
    }

    static char a0[] = "jadeportfolio";
    static char a1[] = "-iwad";
    static char a2[] = "/freedoom1.wad";
    static char a3[] = "-nogui";
    char* argv[] = { a0, a1, a2, a3, nullptr };
    doomgeneric_Create(4, argv);
    started_ = true;
}

void DoomPort::render(gpu::GPU& g, WinRect area, uint32_t tick, float dpr)
{
    (void)tick;
    g_blit_gpu = &g;
    g_blit_mem = mem_;
    g_blit_buf = &scale_buf_;

    constexpr int kSrcW = 640;
    constexpr int kSrcH = 400;
    double scale = std::min(area.w / double(kSrcW), area.h / double(kSrcH));
    int dw = std::max(2, int(kSrcW * scale + 0.5));
    int dh = std::max(2, int(kSrcH * scale + 0.5));

    while (static_cast<std::size_t>(dw) * static_cast<std::size_t>(dh) > kMaxDoomScalePixels
           && dw > 64 && dh > 40) {
        dw = dw * 9 / 10;
        dh = dh * 9 / 10;
    }

    if (mem_) {
        while (static_cast<std::size_t>(dw) * static_cast<std::size_t>(dh) * 4u + kStagingPhys > mem_->size()
               && dw > 64 && dh > 40) {
            dw = dw * 9 / 10;
            dh = dh * 9 / 10;
        }
    }

    g_blit_dx = area.x + (area.w - dw) / 2;
    g_blit_dy = area.y + (area.h - dh) / 2;
    g_blit_dw = dw;
    g_blit_dh = dh;

    if (!started_ && !failed_)
        try_boot();

    if (failed_) {
        g_blit_gpu = nullptr;
        g_blit_mem = nullptr;
        g_blit_buf = nullptr;
        draw_placeholder(g, area, dpr);
        return;
    }

    if (!started_) {
        g_blit_gpu = nullptr;
        g_blit_mem = nullptr;
        g_blit_buf = nullptr;
        draw_placeholder(g, area, dpr);
        return;
    }

    doomgeneric_Tick();

    g_blit_gpu = nullptr;
    g_blit_mem = nullptr;
    g_blit_buf = nullptr;
}

void DoomPort::on_key(uint32_t keycode, uint32_t charcode)
{
    if (!started_ || failed_)
        return;

    const bool down = ((keycode >> 20) & 1u) != 0;
    // Browser keydown autorepeat: would stack key-downs in Doom without matching key-ups.
    if (down && ((keycode >> 21) & 1u) != 0)
        return;

    const uint32_t vk = keycode & 0xFFFFu;

    unsigned char dk = dom_vk_to_doom(vk);

    if (dk == 0 && charcode >= 32u && charcode < 127u && std::isalpha(static_cast<int>(charcode)))
        dk = static_cast<unsigned char>(std::tolower(static_cast<int>(charcode)));

    if (dk == 0)
        return;

    jade_doom_feed_key(down ? 1 : 0, dk);
}

void DoomPort::on_mouse(int32_t dx, int32_t dy, uint32_t buttons) noexcept
{
    if (!started_ || failed_)
        return;
    const int cdx = std::max(-2048, std::min(2048, dx));
    const int cdy = std::max(-2048, std::min(2048, dy));
    const int b   = static_cast<int>(buttons & 7u);
    jade_doom_feed_mouse(b, cdx, cdy);
}

bool DoomPort::wants_pointer_lock() const noexcept
{
    if (!started_ || failed_)
        return false;
    return D_GrabMouseCallback() != 0u;
}

}  // namespace os
