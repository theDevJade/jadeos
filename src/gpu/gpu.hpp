#pragma once
#include "framebuffer.hpp"
#include "font.hpp"
#include "../cpu/memory.hpp"
#include <array>
#include <climits>
#include <cstdint>
#include <deque>
#include <string>
#include <variant>
#include <vector>

namespace gpu {

// GPU command packet in the FIFO.
struct CmdPacket {
    Command  cmd;
    std::array<uint32_t, 6> args{};
};

// Text draw request - queued alongside raster commands to preserve draw order.
struct TextRequest {
    float    x, y;
    RGBA     colour;
    uint8_t  font_size_id;  // 0=14px  1=20px  2=32px
    std::string text;
};

using DrawCmd = std::variant<CmdPacket, TextRequest>;

class GPU {
public:
    explicit GPU(cpu::Memory& mem);

    // Set framebuffer dimensions and allocate front/back buffers.
    // Must be called (via Kernel::boot) before any draw commands.
    void init(uint32_t w, uint32_t h);

    // Load the font from raw TTF bytes, scaled by dpr (1.0 = CSS pixels, 2.0 = HiDPI).
    bool load_font(std::span<const uint8_t> ttf_data, float dpr = 1.0f);

    // Enqueue a raster command.
    void push_command(CmdPacket pkt);

    // Enqueue a text draw (ordered with raster commands in the same FIFO).
    void draw_text(TextRequest req);

    // Process all pending commands and return.
    void flush();

    // WebGPU mode: flush() serialises commands to a binary buffer instead of
    // rasterising. JavaScript reads the buffer each frame via cmd_buf_ptr().
    void set_webgpu_mode(bool enable) noexcept { webgpu_mode_ = enable; }
    bool webgpu_mode() const noexcept { return webgpu_mode_; }

    // Serialised command records (8 x uint32_t = 32 bytes per record).
    // Valid until the next flush() call.
    const uint32_t* cmd_buf_ptr() const noexcept { return cmd_buf_.data(); }
    uint32_t        cmd_buf_len() const noexcept { return static_cast<uint32_t>(cmd_buf_.size()); }

    // String pool: concatenated UTF-8 text strings referenced by TEXT records.
    const uint8_t*  str_pool_ptr() const noexcept { return reinterpret_cast<const uint8_t*>(str_pool_.data()); }
    uint32_t        str_pool_len() const noexcept { return static_cast<uint32_t>(str_pool_.size()); }

    // RGBA8 pixel blobs for BLIT commands (WebGPU path only).
    const uint8_t*  blit_pool_ptr() const noexcept { return blit_pool_.data(); }
    uint32_t        blit_pool_len() const noexcept { return static_cast<uint32_t>(blit_pool_.size()); }

    // Persistent glyph metric buffer: 9 floats x 96 glyphs per font size.
    // Populated by load_font(); valid for the lifetime of the GPU object.
    const float*    glyph_metrics_ptr(uint8_t size_id) const noexcept;
    uint32_t        glyph_metrics_len(uint8_t size_id) const noexcept;

    // Returns the front buffer (the last FLIP'd frame  -  tear-free for display).
    const uint8_t* framebuffer_data() const noexcept;

    uint32_t framebuffer_width()  const noexcept { return fb_w_; }
    uint32_t framebuffer_height() const noexcept { return fb_h_; }
    std::size_t fb_bytes() const noexcept {
        return static_cast<std::size_t>(fb_w_) * fb_h_ * BYTES_PER_PIXEL;
    }

    uint64_t draw_calls() const noexcept { return draw_calls_; }

    const FontAtlas& font(uint8_t size_id) const noexcept;

private:
    static constexpr int   NUM_FONT_SIZES = 3;
    static constexpr float FONT_SIZE_PX[NUM_FONT_SIZES] = { 14.f, 20.f, 32.f };

    cpu::Memory&         mem_;
    uint32_t             fb_w_ = FB_W_DEFAULT;
    uint32_t             fb_h_ = FB_H_DEFAULT;
    std::vector<uint8_t> back_buf_;   // active drawing target
    std::vector<uint8_t> front_buf_;  // display buffer (swapped on FLIP)
    std::deque<DrawCmd>  fifo_;
    uint64_t             draw_calls_ = 0;
    FontAtlas            fonts_[NUM_FONT_SIZES];

    // WebGPU serialisation state
    bool                  webgpu_mode_ = false;
    std::vector<uint32_t> cmd_buf_;
    std::string           str_pool_;
    std::vector<uint8_t>  blit_pool_;
    std::vector<float>    glyph_metrics_[NUM_FONT_SIZES];

    // Active scissor rectangle (pixel-exclusive x1/y1). Full FB when has_scissor_==false.
    bool    has_scissor_ = false;
    int32_t sc_x0_ = 0, sc_y0_ = 0, sc_x1_ = INT32_MAX, sc_y1_ = INT32_MAX;

    std::span<uint8_t> fb_span();
    void exec(const CmdPacket& pkt);
    void exec(const TextRequest& req);
    void serialize_cmd(const CmdPacket& pkt);
    void serialize_text(const TextRequest& req);
};

}  // namespace gpu
