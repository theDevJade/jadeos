#include "gpu.hpp"
#include "rasterizer.hpp"
#include <cstring>
#include <algorithm>

namespace gpu {

GPU::GPU(cpu::Memory& mem) : mem_(mem) {}

void GPU::init(uint32_t w, uint32_t h) {
    fb_w_ = w ? w : FB_W_DEFAULT;
    fb_h_ = h ? h : FB_H_DEFAULT;
    const std::size_t bytes = fb_bytes();
    back_buf_.assign(bytes, 0);
    front_buf_.assign(bytes, 0);
}

bool GPU::load_font(std::span<const uint8_t> ttf_data, float dpr) {
    if (dpr < 0.5f) dpr = 1.0f;
    bool ok = true;
    for (int i = 0; i < NUM_FONT_SIZES; ++i) {
        ok &= fonts_[i].load(ttf_data, FONT_SIZE_PX[i] * dpr);
        // Build persistent glyph metrics buffer for WebGPU text rendering.
        constexpr int N = FontAtlas::kNumChars;
        glyph_metrics_[i].resize(N * 9);
        const GlyphInfo* ga = fonts_[i].glyph_array();
        for (int j = 0; j < N; ++j) {
            float* m = glyph_metrics_[i].data() + j * 9;
            m[0]=ga[j].xoff;   m[1]=ga[j].yoff;
            m[2]=ga[j].xoff2;  m[3]=ga[j].yoff2;
            m[4]=ga[j].advance_x;
            m[5]=float(ga[j].atlas_x0); m[6]=float(ga[j].atlas_y0);
            m[7]=float(ga[j].atlas_x1); m[8]=float(ga[j].atlas_y1);
        }
    }
    return ok;
}

const FontAtlas& GPU::font(uint8_t size_id) const noexcept {
    if (size_id >= NUM_FONT_SIZES) size_id = 1;
    return fonts_[size_id];
}

std::span<uint8_t> GPU::fb_span() {
    if (back_buf_.empty()) return {};
    return { back_buf_.data(), back_buf_.size() };
}

const uint8_t* GPU::framebuffer_data() const noexcept {
    return front_buf_.empty() ? nullptr : front_buf_.data();
}

void GPU::push_command(CmdPacket pkt) { fifo_.push_back(std::move(pkt)); }
void GPU::draw_text(TextRequest req)  { fifo_.push_back(std::move(req)); }

void GPU::flush() {
    if (webgpu_mode_) {
        // Serialise FIFO to a compact binary buffer; skip software rasterisation.
        cmd_buf_.clear();
        str_pool_.clear();
        blit_pool_.clear();
        cmd_buf_.reserve(fifo_.size() * 8);
        for (auto& dc : fifo_) {
            std::visit([this](auto&& cmd) {
                using T = std::decay_t<decltype(cmd)>;
                if constexpr (std::is_same_v<T, TextRequest>) serialize_text(cmd);
                else serialize_cmd(cmd);
            }, dc);
        }
        fifo_.clear();
        ++draw_calls_;
        return;
    }
    // Original software-rasterisation path.
    while (!fifo_.empty()) {
        std::visit([this](auto&& cmd){ exec(cmd); }, fifo_.front());
        fifo_.pop_front();
        ++draw_calls_;
    }
}

// Record layout: 8 x uint32_t (32 bytes).
//   [0] type (u8) | size_id (u8)<<8 | str_len (u16)<<16
//   [1] x0  as float bits   [2] y0  as float bits
//   [3] x1/w as float bits  [4] y1/h as float bits
//   [5] packed color 0xAARRGGBB
//   [6] str_off (string pool byte offset, TEXT only)
//   [7] flags (0x01 = alpha-blend rect)

static inline uint32_t f2u(float f) noexcept {
    uint32_t bits; std::memcpy(&bits, &f, 4); return bits;
}

void GPU::serialize_cmd(const CmdPacket& pkt) {
    const auto& a = pkt.args;
    const uint32_t t = static_cast<uint32_t>(pkt.cmd);
    auto pf = [this](float v){ cmd_buf_.push_back(f2u(v)); };
    auto pi = [this](uint32_t v){ cmd_buf_.push_back(v); };
    auto pz = [this](){ cmd_buf_.push_back(0); };

    switch (pkt.cmd) {
    case Command::CLEAR:
        pi(t); pf(0.f); pf(0.f); pf(float(fb_w_)); pf(float(fb_h_));
        pi(a[0]); pz(); pz();
        break;
    case Command::DRAW_RECT:
        pi(t);
        pf(float(int32_t(a[0]))); pf(float(int32_t(a[1])));
        pf(float(int32_t(a[2]))); pf(float(int32_t(a[3])));
        pi(a[4]); pz(); pz();
        break;
    case Command::DRAW_RECT_ALPHA:
        pi(t);
        pf(float(int32_t(a[0]))); pf(float(int32_t(a[1])));
        pf(float(int32_t(a[2]))); pf(float(int32_t(a[3])));
        pi(a[4]); pz(); pi(0x01u); // flags: alpha-blend
        break;
    case Command::DRAW_LINE:
    case Command::DRAW_LINE_AA:
        pi(t);
        pf(float(int32_t(a[0]))); pf(float(int32_t(a[1])));
        pf(float(int32_t(a[2]))); pf(float(int32_t(a[3])));
        pi(a[4]); pz(); pz();
        break;
    case Command::DRAW_CIRCLE:
        pi(t);
        pf(float(int32_t(a[0]))); pf(float(int32_t(a[1])));
        pf(float(int32_t(a[2]))); pf(float(int32_t(a[3])));
        pi(a[4]); pz(); pz();
        break;
    case Command::SET_SCISSOR:
        pi(t);
        pf(float(int32_t(a[0]))); pf(float(int32_t(a[1])));
        pf(float(int32_t(a[2]))); pf(float(int32_t(a[3])));
        pz(); pz(); pz();
        break;
    case Command::BLIT: {
        const uint8_t* src = mem_.raw_ptr(a[0]);
        if (!src) break;
        const int32_t bw = static_cast<int32_t>(a[3]);
        const int32_t bh = static_cast<int32_t>(a[4]);
        if (bw < 1 || bh < 1) break;
        const std::size_t nbytes =
            static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh) * 4u;
        // Match DoomPort staging cap
        constexpr std::size_t kMaxBlitBytes = 48u << 20;
        if (nbytes > kMaxBlitBytes) break;
        const std::size_t off = blit_pool_.size();
        blit_pool_.resize(off + nbytes);
        std::memcpy(blit_pool_.data() + off, src, nbytes);
        pi(t);
        pf(float(static_cast<int32_t>(a[1])));
        pf(float(static_cast<int32_t>(a[2])));
        pf(float(bw));
        pf(float(bh));
        pi(static_cast<uint32_t>(off));
        pi(static_cast<uint32_t>(nbytes));
        pz();
        break;
    }
    case Command::CLEAR_SCISSOR:
    case Command::FLIP:
        pi(t); pz(); pz(); pz(); pz(); pz(); pz(); pz();
        break;
    default:
        break;
    }
}

void GPU::serialize_text(const TextRequest& req) {
    const auto str_off = static_cast<uint32_t>(str_pool_.size());
    const auto str_len = static_cast<uint32_t>(
        std::min(req.text.size(), static_cast<std::size_t>(0xFFFFu)));
    str_pool_.append(req.text.data(), str_len);

    const uint32_t col =
        (uint32_t(req.colour.a) << 24) | (uint32_t(req.colour.r) << 16) |
        (uint32_t(req.colour.g) << 8)  |  uint32_t(req.colour.b);
    const uint8_t sid = req.font_size_id < NUM_FONT_SIZES ? req.font_size_id : 1;

    cmd_buf_.push_back(0x80u | (uint32_t(sid) << 8) | (str_len << 16));
    cmd_buf_.push_back(f2u(req.x));
    cmd_buf_.push_back(f2u(req.y));
    cmd_buf_.push_back(0); cmd_buf_.push_back(0);
    cmd_buf_.push_back(col);
    cmd_buf_.push_back(str_off);
    cmd_buf_.push_back(0);
}

const float* GPU::glyph_metrics_ptr(uint8_t size_id) const noexcept {
    if (size_id >= NUM_FONT_SIZES) size_id = 0;
    return glyph_metrics_[size_id].data();
}
uint32_t GPU::glyph_metrics_len(uint8_t size_id) const noexcept {
    if (size_id >= NUM_FONT_SIZES) size_id = 0;
    return static_cast<uint32_t>(glyph_metrics_[size_id].size());
}

void GPU::exec(const TextRequest& req) {
    uint8_t sid = req.font_size_id < NUM_FONT_SIZES ? req.font_size_id : 1;
    auto fb = fb_span();
    if (fb.empty()) return;
    FontClipRect clip;
    if (has_scissor_) clip = { sc_x0_, sc_y0_, sc_x1_, sc_y1_ };
    fonts_[sid].draw_string(fb, fb_w_, fb_h_, req.x, req.y, req.text, req.colour, clip);
}

void GPU::exec(const CmdPacket& pkt) {
    auto& a = pkt.args;
    switch (pkt.cmd) {

    case Command::CLEAR: {
        RGBA col = RGBA::from_u32(a[0]);
        auto fb  = fb_span();
        if (fb.empty()) return;
        const std::size_t n = fb_bytes();
        for (std::size_t i = 0; i < n; i += 4) {
            fb[i+0] = col.r; fb[i+1] = col.g;
            fb[i+2] = col.b; fb[i+3] = col.a;
        }
        break;
    }
    case Command::DRAW_PIXEL: {
        auto fb = fb_span();
        if (fb.empty()) return;
        RGBA col = RGBA::from_u32(a[2]);
        std::size_t off = (static_cast<std::size_t>(a[1]) * fb_w_ + a[0]) * 4;
        if (off + 3 < fb_bytes()) {
            fb[off+0]=col.r; fb[off+1]=col.g; fb[off+2]=col.b; fb[off+3]=col.a;
        }
        break;
    }
    case Command::DRAW_LINE:
        rasterize_line(fb_span(), fb_w_, fb_h_,
            static_cast<int32_t>(a[0]), static_cast<int32_t>(a[1]),
            static_cast<int32_t>(a[2]), static_cast<int32_t>(a[3]),
            RGBA::from_u32(a[4]));
        break;

    case Command::DRAW_LINE_AA:
        rasterize_line_aa(fb_span(), fb_w_, fb_h_,
            static_cast<int32_t>(a[0]), static_cast<int32_t>(a[1]),
            static_cast<int32_t>(a[2]), static_cast<int32_t>(a[3]),
            RGBA::from_u32(a[4]));
        break;

    case Command::DRAW_RECT: {
        int32_t rx = static_cast<int32_t>(a[0]), ry = static_cast<int32_t>(a[1]);
        int32_t rw = static_cast<int32_t>(a[2]), rh = static_cast<int32_t>(a[3]);
        if (has_scissor_) {
            const int32_t cx = std::max(rx, sc_x0_), cy = std::max(ry, sc_y0_);
            const int32_t cx2 = std::min(rx + rw, sc_x1_), cy2 = std::min(ry + rh, sc_y1_);
            if (cx2 <= cx || cy2 <= cy) break;
            rx = cx; ry = cy; rw = cx2 - cx; rh = cy2 - cy;
        }
        rasterize_rect(fb_span(), fb_w_, fb_h_, rx, ry, rw, rh, RGBA::from_u32(a[4]));
        break;
    }

    case Command::DRAW_RECT_ALPHA: {
        int32_t rx = static_cast<int32_t>(a[0]), ry = static_cast<int32_t>(a[1]);
        int32_t rw = static_cast<int32_t>(a[2]), rh = static_cast<int32_t>(a[3]);
        if (has_scissor_) {
            const int32_t cx = std::max(rx, sc_x0_), cy = std::max(ry, sc_y0_);
            const int32_t cx2 = std::min(rx + rw, sc_x1_), cy2 = std::min(ry + rh, sc_y1_);
            if (cx2 <= cx || cy2 <= cy) break;
            rx = cx; ry = cy; rw = cx2 - cx; rh = cy2 - cy;
        }
        rasterize_rect_alpha(fb_span(), fb_w_, fb_h_, rx, ry, rw, rh, RGBA::from_u32(a[4]));
        break;
    }

    case Command::DRAW_TRIANGLE:
        rasterize_triangle(fb_span(), fb_w_, fb_h_,
            static_cast<int32_t>(a[0]), static_cast<int32_t>(a[1]),
            static_cast<int32_t>(a[2]), static_cast<int32_t>(a[3]),
            static_cast<int32_t>(a[4] & 0xFFFFu),
            static_cast<int32_t>(a[4] >> 16),
            RGBA::from_u32(a[5]));
        break;

    case Command::DRAW_CIRCLE:
        rasterize_circle(fb_span(), fb_w_, fb_h_,
            static_cast<int32_t>(a[0]), static_cast<int32_t>(a[1]),
            static_cast<int32_t>(a[2]), static_cast<int32_t>(a[3]),
            RGBA::from_u32(a[4]));
        break;

    case Command::BLIT: {
        const uint8_t* src = mem_.raw_ptr(a[0]);
        if (!src) break;
        auto fb = fb_span();
        if (fb.empty()) break;
        const int32_t bx = static_cast<int32_t>(a[1]);
        const int32_t by = static_cast<int32_t>(a[2]);
        const int32_t bw = static_cast<int32_t>(a[3]);
        const int32_t bh = static_cast<int32_t>(a[4]);
        for (int32_t row = 0; row < bh; ++row) {
            int32_t fy = by + row;
            if (fy < 0 || static_cast<uint32_t>(fy) >= fb_h_) continue;
            for (int32_t col = 0; col < bw; ++col) {
                int32_t fx = bx + col;
                if (fx < 0 || static_cast<uint32_t>(fx) >= fb_w_) continue;
                std::size_t si = static_cast<std::size_t>(row * bw + col) * 4;
                std::size_t di = (static_cast<std::size_t>(fy) * fb_w_ + fx) * 4;
                if (di + 3 < fb_bytes()) {
                    fb[di+0]=src[si+0]; fb[di+1]=src[si+1];
                    fb[di+2]=src[si+2]; fb[di+3]=src[si+3];
                }
            }
        }
        break;
    }

    case Command::SET_SCISSOR:
        has_scissor_ = true;
        sc_x0_ = static_cast<int32_t>(a[0]);
        sc_y0_ = static_cast<int32_t>(a[1]);
        sc_x1_ = static_cast<int32_t>(a[2]);
        sc_y1_ = static_cast<int32_t>(a[3]);
        break;

    case Command::CLEAR_SCISSOR:
        has_scissor_ = false;
        sc_x0_ = 0; sc_y0_ = 0;
        sc_x1_ = INT32_MAX; sc_y1_ = INT32_MAX;
        break;

    case Command::FLIP: {
        // Swap front/back buffers (tear-free display).
        std::swap(front_buf_, back_buf_);
        // Keep back buffer content for incremental rendering next frame.
        std::memcpy(back_buf_.data(), front_buf_.data(), fb_bytes());
        // Sync VRAM in emulated memory so CPU can read pixel data.
        if (uint8_t* vram = mem_.raw_ptr_mut(VRAM_BASE)) {
            const std::size_t avail = mem_.size() > VRAM_BASE
                ? mem_.size() - VRAM_BASE : 0;
            std::memcpy(vram, front_buf_.data(), std::min(fb_bytes(), avail));
        }
        break;
    }

    default: break;
    }
}

}  // namespace gpu

