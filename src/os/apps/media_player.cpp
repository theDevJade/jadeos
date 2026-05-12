#include "media_player.hpp"
#include "apputil.hpp"
#include "../../audio/jade_audio.hpp"
#include "../../cpu/memory.hpp"
#include "../../gpu/gpu.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace os {
struct MediaPlayerImpl;
}

namespace {

constexpr std::uint32_t kStagingPhys = 0x600000u;

static std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(0, 1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

static std::uint32_t read_le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8u) |
           (static_cast<std::uint32_t>(p[2]) << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u);
}

static std::uint16_t read_le16(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8u);
}

/* Minimal PCM WAVE (format 1) reader for export script output (48 kHz s16le). */
struct PcmWavReader {
    std::FILE*     f            = nullptr;
    std::uint32_t sample_rate = 48000;
    std::uint16_t channels    = 2;
    std::uint16_t bits        = 16;
    long           data_begin = 0;
    long           data_end   = 0;
    long           pcm_pos    = 0;
    std::int64_t  rem_num     = 0;

    void close() noexcept {
        if (f) {
            std::fclose(f);
            f = nullptr;
        }
        data_begin = data_end = pcm_pos = 0;
        rem_num = 0;
    }

    bool open(const char* path) {
        close();
        f = std::fopen(path, "rb");
        if (!f)
            return false;
        unsigned char hdr[12];
        if (std::fread(hdr, 1, 12, f) != 12u) {
            close();
            return false;
        }
        if (std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
            close();
            return false;
        }

        bool          got_fmt = false;
        bool          got_data = false;
        unsigned char chunk_head[8];
        unsigned char fmtbuf[40];

        while (true) {
            if (std::fread(chunk_head, 1, 8, f) != 8u)
                break;
            const std::uint32_t chunk_sz = read_le32(chunk_head + 4);
            const long          data_start = std::ftell(f);

            if (std::memcmp(chunk_head, "fmt ", 4) == 0) {
                if (chunk_sz < 16u || chunk_sz > sizeof(fmtbuf)) {
                    close();
                    return false;
                }
                if (std::fread(fmtbuf, 1, chunk_sz, f) != chunk_sz) {
                    close();
                    return false;
                }
                const std::uint16_t audio_format = read_le16(fmtbuf);
                channels                         = read_le16(fmtbuf + 2);
                sample_rate                      = read_le32(fmtbuf + 4);
                bits                             = read_le16(fmtbuf + 14);
                if (audio_format != 1u || bits != 16u || (channels != 1u && channels != 2u) ||
                    sample_rate < 8000u || sample_rate > 192000u) {
                    close();
                    return false;
                }
                got_fmt = true;
                if (chunk_sz % 2u == 1u && std::fseek(f, 1, SEEK_CUR) != 0) {
                    close();
                    return false;
                }
            } else if (std::memcmp(chunk_head, "data", 4) == 0) {
                data_begin = data_start;
                data_end   = data_begin + static_cast<long>(chunk_sz);
                if (std::fseek(f, data_begin + static_cast<long>(chunk_sz), SEEK_SET) != 0) {
                    close();
                    return false;
                }
                got_data = true;
                break;
            } else {
                if (chunk_sz > 100u * 1024u * 1024u) {
                    close();
                    return false;
                }
                if (std::fseek(f, data_start + static_cast<long>(chunk_sz), SEEK_SET) != 0) {
                    close();
                    return false;
                }
                if (chunk_sz % 2u == 1u && std::fseek(f, 1, SEEK_CUR) != 0) {
                    close();
                    return false;
                }
            }
        }

        if (!got_fmt || !got_data) {
            close();
            return false;
        }
        pcm_pos = data_begin;
        if (std::fseek(f, pcm_pos, SEEK_SET) != 0) {
            close();
            return false;
        }
        rem_num = 0;
        return true;
    }

    void rewind_pcm() noexcept {
        rem_num = 0;
        pcm_pos = data_begin;
        if (f)
            std::fseek(f, data_begin, SEEK_SET);
    }

    void pump_for_video_tick(std::uint32_t seq_tick_div) {
        if (!f || data_end <= data_begin || seq_tick_div == 0)
            return;
        if (jade::audio::queued_stereo_frames() > 48000u * 2u)
            return;

        rem_num += static_cast<std::int64_t>(sample_rate) * static_cast<std::int64_t>(seq_tick_div);
        std::int32_t want_stereo = static_cast<std::int32_t>(rem_num / 120);
        rem_num %= 120;
        if (want_stereo < 1)
            return;

        std::vector<std::int16_t> interleaved(
            static_cast<std::size_t>(want_stereo) * 2u);

        std::uint32_t out_frames = 0;
        while (out_frames < static_cast<std::uint32_t>(want_stereo) && pcm_pos < data_end) {
            if (channels == 2u) {
                std::int16_t lr[2];
                if (pcm_pos + static_cast<long>(sizeof(lr)) > data_end)
                    break;
                if (std::fread(lr, 1, sizeof(lr), f) != sizeof(lr))
                    break;
                interleaved[out_frames * 2u]     = lr[0];
                interleaved[out_frames * 2u + 1] = lr[1];
                pcm_pos += static_cast<long>(sizeof(lr));
            } else {
                std::int16_t m;
                if (pcm_pos + static_cast<long>(sizeof(m)) > data_end)
                    break;
                if (std::fread(&m, 1, sizeof(m), f) != sizeof(m))
                    break;
                interleaved[out_frames * 2u]     = m;
                interleaved[out_frames * 2u + 1] = m;
                pcm_pos += static_cast<long>(sizeof(m));
            }
            ++out_frames;
        }

        if (out_frames > 0)
            jade::audio::submit_stereo_i16(interleaved.data(), out_frames);
    }
};

}  // namespace

struct os::MediaPlayerImpl {
    gpu::GPU*    gpu = nullptr;
    cpu::Memory* mem = nullptr;
    bool         fs_ready        = false;
    bool         clip_fs_ready   = false;

    enum class Panel : std::uint8_t { Image, Video };
    Panel        panel = Panel::Image;
    bool         playing = false;

    int                    last_cw = 0;
    int                    last_ch = 0;
    int                    img_w = 0, img_h = 0;
    std::vector<std::uint8_t> img_rgba;

    bool                       sequence_ready = false;
    double                     seq_fps_d = 30.0;
    std::uint32_t             seq_tick_div = 4;
    int                        seq_frame_count = 0;
    char                       seq_pattern[128] = "frame_%06d.png";
    int                        seq_current = 0;
    char                       audio_rel[128] = "";

    PcmWavReader wav{};

    int                        pic_w = 0, pic_h = 0;
    std::vector<std::uint32_t> last_rgba;
    std::vector<std::uint8_t>  blit_staging;

    std::string err;

    void set_context(gpu::GPU* g, cpu::Memory* m) noexcept {
        gpu = g;
        mem = m;
    }

    void set_fs_ready(bool v) noexcept { fs_ready = v; }

    void set_clip_fs_ready(bool v) noexcept {
        clip_fs_ready = v;
        if (clip_fs_ready && panel == Panel::Video && fs_ready) {
            err.clear();
            try_open_sequence();
        }
    }

    void close_sequence() noexcept {
        sequence_ready = false;
        seq_fps_d = 30.0;
        seq_tick_div = 4;
        seq_frame_count = 0;
        std::strcpy(seq_pattern, "frame_%06d.png");
        seq_current = 0;
        audio_rel[0] = '\0';
        wav.close();
        last_rgba.clear();
        pic_w = pic_h = 0;
    }

    bool parse_sequence_txt(const std::string& text) {
        std::vector<std::string> lines;
        std::string              cur;
        for (char c : text) {
            if (c == '\r')
                continue;
            if (c == '\n') {
                cur = trim(cur);
                if (!cur.empty() && cur[0] != '#')
                    lines.push_back(cur);
                cur.clear();
            } else
                cur += c;
        }
        cur = trim(cur);
        if (!cur.empty() && cur[0] != '#')
            lines.push_back(cur);
        if (lines.size() < 3)
            return false;

        if (lines[0].size() >= 4 && lines[0].compare(0, 4, "fps ") == 0)
            seq_fps_d = std::atof(lines[0].c_str() + 4);
        else
            seq_fps_d = std::atof(lines[0].c_str());
        if (seq_fps_d < 1.0 || seq_fps_d > 120.0)
            seq_fps_d = 30.0;
        seq_tick_div = static_cast<std::uint32_t>(
            std::max(1, static_cast<int>(std::lround(120.0 / seq_fps_d))));

        if (lines[1].size() >= sizeof(seq_pattern))
            return false;
        std::memcpy(seq_pattern, lines[1].c_str(), lines[1].size() + 1);
        if (std::strstr(seq_pattern, "%") == nullptr)
            return false;
        seq_frame_count = std::atoi(lines[2].c_str());
        if (seq_frame_count < 1 || seq_frame_count > 500000)
            return false;

        audio_rel[0] = '\0';
        if (lines.size() >= 4) {
            if (lines[3].size() >= sizeof(audio_rel))
                return false;
            std::memcpy(audio_rel, lines[3].c_str(), lines[3].size() + 1);
        }
        return true;
    }

    void try_open_sequence() {
        err.clear();
        if (!clip_fs_ready) {
            err = "Waiting for clip preload in the browser.";
            return;
        }
        close_sequence();
        if (!fs_ready || access("/media/badapple/sequence.txt", F_OK) != 0) {
            err = "missing /media/badapple/sequence.txt (run scripts/export-badapple-media.sh)";
            return;
        }
        std::FILE* sf = std::fopen("/media/badapple/sequence.txt", "rb");
        if (!sf) {
            err = "fopen sequence.txt failed";
            return;
        }
        std::string buf;
        char        chunk[4096];
        while (true) {
            const std::size_t n = std::fread(chunk, 1, sizeof(chunk), sf);
            if (n > 0)
                buf.append(chunk, chunk + n);
            if (n < sizeof(chunk))
                break;
        }
        std::fclose(sf);
        if (!parse_sequence_txt(buf)) {
            err = "sequence.txt: need fps, pattern, frames; optional line 4 = wav name";
            return;
        }
        char fmt[192];
        int  nf = std::snprintf(fmt, sizeof(fmt), "/media/badapple/%s", seq_pattern);
        if (nf < 0 || static_cast<std::size_t>(nf) >= sizeof(fmt)) {
            err = "pattern path too long";
            return;
        }
        char path[256];
        nf = std::snprintf(path, sizeof(path), fmt, 1);
        if (nf < 0 || static_cast<std::size_t>(nf) >= sizeof(path)) {
            err = "resolved path too long";
            return;
        }
        if (access(path, F_OK) != 0) {
            err = "first frame missing (run export script)";
            return;
        }
        sequence_ready = true;

        if (audio_rel[0] != '\0') {
            char ap[256];
            nf = std::snprintf(ap, sizeof(ap), "/media/badapple/%s", audio_rel);
            if (nf < 0 || static_cast<std::size_t>(nf) >= sizeof(ap)) {
                err = "audio path too long";
                sequence_ready = false;
                return;
            }
            if (access(ap, F_OK) != 0) {
                err = std::string("audio missing: ") + ap;
                sequence_ready = false;
                return;
            }
            if (!wav.open(ap)) {
                err = "unsupported or corrupt WAV (need PCM s16le mono/stereo)";
                sequence_ready = false;
                return;
            }
            if (wav.sample_rate != jade::audio::sample_rate()) {
                err = "WAV sample rate must match audio engine (export uses 48 kHz)";
                sequence_ready = false;
                wav.close();
                return;
            }
        }
    }

    void try_load_png() {
        err.clear();
        img_rgba.clear();
        img_w = img_h = 0;
        if (!fs_ready || access("/media/amazingimage.png", F_OK) != 0) {
            err = "amazingimage.png missing (/media/amazingimage.png)";
            return;
        }
        int w = 0, h = 0, ch = 0;
        unsigned char* pix = stbi_load("/media/amazingimage.png", &w, &h, &ch, 4);
        if (!pix || w < 1 || h < 1) {
            if (pix)
                stbi_image_free(pix);
            err = "stbi_load failed";
            return;
        }
        img_w = w;
        img_h = h;
        img_rgba.assign(pix, pix + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
        stbi_image_free(pix);
    }

    void load_sequence_frame(int frame_index) {
        if (!sequence_ready || frame_index < 1 || frame_index > seq_frame_count)
            return;
        char fmt[192];
        int  nf = std::snprintf(fmt, sizeof(fmt), "/media/badapple/%s", seq_pattern);
        if (nf < 0 || static_cast<std::size_t>(nf) >= sizeof(fmt))
            return;
        char path[256];
        nf = std::snprintf(path, sizeof(path), fmt, frame_index);
        if (nf < 0 || static_cast<std::size_t>(nf) >= sizeof(path))
            return;
        int w = 0, h = 0, ch = 0;
        unsigned char* pix = stbi_load(path, &w, &h, &ch, 4);
        if (!pix || w < 1 || h < 1) {
            if (pix)
                stbi_image_free(pix);
            err = std::string("stbi_load failed: ") + path;
            return;
        }
        err.clear();
        pic_w = w;
        pic_h = h;
        last_rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        std::memcpy(last_rgba.data(), pix, last_rgba.size() * sizeof(std::uint32_t));
        stbi_image_free(pix);
    }

    void restart_sequence() {
        err.clear();
        seq_current = 0;
        wav.rewind_pcm();
        if (sequence_ready) {
            load_sequence_frame(1);
            seq_current = 1;
        }
    }

    void advance_frame() {
        if (!sequence_ready)
            return;
        if (seq_current < 1 || seq_current >= seq_frame_count) {
            wav.rewind_pcm();
            load_sequence_frame(1);
            seq_current = 1;
        } else {
            load_sequence_frame(seq_current + 1);
            ++seq_current;
        }
    }

    void step(std::uint32_t tick_count) noexcept {
        if (!playing || panel != Panel::Video || !sequence_ready)
            return;
        if ((tick_count % seq_tick_div) != 0u)
            return;
        advance_frame();
        wav.pump_for_video_tick(seq_tick_div);
    }

    bool wants_always_dirty() const noexcept { return true; }

    void blit_rgba_to_window(gpu::GPU& g, WinRect area, const std::uint32_t* src, int sw,
                             int sh) {
        if (!gpu || !mem || !src || sw < 2 || sh < 2 || area.w < 2 || area.h < 2)
            return;

        const double aw  = static_cast<double>(area.w);
        const double ah  = static_cast<double>(area.h);
        const double swd = static_cast<double>(sw);
        const double shd = static_cast<double>(sh);
        const double scale = std::min(aw / swd, ah / shd);
        int          dw    = std::max(2, static_cast<int>(std::floor(scale * swd + 1e-9)));
        int          dh    = std::max(2, static_cast<int>(std::lround(static_cast<double>(dw) * shd / swd)));
        if (dh > area.h) {
            dh = std::max(2, area.h);
            dw = std::max(2, static_cast<int>(std::lround(static_cast<double>(dh) * swd / shd)));
        }
        if (dw > area.w) {
            dw = std::max(2, area.w);
            dh = std::max(2, static_cast<int>(std::lround(static_cast<double>(dw) * shd / swd)));
        }
        const int dx = area.x + (area.w - dw) / 2;
        const int dy = area.y + (area.h - dh) / 2;

        const std::size_t need =
            static_cast<std::size_t>(dw) * static_cast<std::size_t>(dh) * 4u;
        if (kStagingPhys + need > mem->size())
            return;

        blit_staging.resize(need);
        auto* dst32 = reinterpret_cast<std::uint32_t*>(blit_staging.data());
        for (int y = 0; y < dh; ++y) {
            const int sy = static_cast<int>((static_cast<std::int64_t>(y) * 2 + 1) * sh /
                                            (static_cast<std::int64_t>(dh) * 2));
            for (int x = 0; x < dw; ++x) {
                const int sx = static_cast<int>((static_cast<std::int64_t>(x) * 2 + 1) * sw /
                                                (static_cast<std::int64_t>(dw) * 2));
                dst32[static_cast<std::size_t>(y) * static_cast<std::size_t>(dw) +
                      static_cast<std::size_t>(x)] =
                    src[static_cast<std::size_t>(sy) * static_cast<std::size_t>(sw) +
                        static_cast<std::size_t>(sx)];
            }
        }

        if (std::uint8_t* phys = mem->raw_ptr_mut(kStagingPhys))
            std::memcpy(phys, blit_staging.data(), need);
        else
            return;

        gpu::CmdPacket pkt;
        pkt.cmd  = gpu::Command::BLIT;
        pkt.args = {kStagingPhys, static_cast<std::uint32_t>(dx),
                    static_cast<std::uint32_t>(dy), static_cast<std::uint32_t>(dw),
                    static_cast<std::uint32_t>(dh), 0};
        g.push_command(pkt);
    }

    static WinRect viewport_below_tabs(const WinRect& area, Panel panel, bool seq_ok,
                                       float dpr) {
        auto sc = [dpr](int n) { return static_cast<int>(n * dpr + 0.5f); };
        const int tab_top_pad = sc(8);
        const int tab_row_h   = sc(28);
        const int tab_gap     = sc(6);
        const int top_off     = tab_top_pad + tab_row_h + tab_gap;
        const int bottom_off =
            (panel == Panel::Video && seq_ok) ? (sc(40) + sc(8)) : sc(8);
        const int h = area.h - top_off - bottom_off;
        if (area.w < 2 || h < 2)
            return {area.x, area.y + top_off, std::max(0, area.w), 0};
        return {area.x, area.y + top_off, area.w, h};
    }

    void draw_ui(gpu::GPU& g, WinRect area, float dpr) const {
        auto sc = [dpr](int n) { return static_cast<int>(n * dpr + 0.5f); };
        const int tab_y    = area.y + sc(8);
        const int tab_h    = sc(28);
        const int margin   = sc(8);
        const int tab_w    = std::max(40, (area.w - margin * 3) / 2);
        const int x0       = area.x + margin;
        const int x1       = x0 + tab_w + margin;

        const gpu::FontAtlas& f0 = g.font(0);
        const int             tab_ty =
            tab_y + static_cast<int>((tab_h - f0.line_height()) * 0.5f + f0.ascent() + 0.5f);

        apps::g_rect(g, x0, tab_y, tab_w, tab_h,
                     panel == Panel::Image ? 0xFF'3A'5E'8C : 0xFF'20'28'34);
        apps::g_text(g, x0 + sc(8), tab_ty, 0xFF'E0'E8'F0, 0, "Image");

        apps::g_rect(g, x1, tab_y, tab_w, tab_h,
                     panel == Panel::Video ? 0xFF'3A'5E'8C : 0xFF'20'28'34);
        apps::g_text(g, x1 + sc(8), tab_ty, 0xFF'E0'E8'F0, 0, "Video");

        if (panel == Panel::Video && sequence_ready) {
            const int py      = area.y + area.h - sc(40);
            const int btn_h   = sc(28);
            const int play_ty = py + static_cast<int>(
                                   (btn_h - f0.line_height()) * 0.5f + f0.ascent() + 0.5f);
            apps::g_rect(g, area.x + margin, py, sc(100), btn_h, 0xFF'2D'6A'4F);
            apps::g_text(g, area.x + margin + sc(10), play_ty, 0xFF'FF'FF'FF, 0,
                         playing ? "Pause" : "Play");
        }

        if (!err.empty()) {
            apps::g_text(g, area.x + margin, area.y + area.h - sc(72), 0xFF'88'44'44, 0,
                          err.size() > 100 ? err.substr(0, 100) + "..." : err);
        }
    }

    void render(gpu::GPU& g, WinRect area, std::uint32_t /*tick*/, float dpr) {
        last_cw = area.w;
        last_ch = area.h;
        apps::g_rect(g, area.x, area.y, area.w, area.h, 0xFF'0C'10'14);

        const WinRect view = viewport_below_tabs(area, panel, sequence_ready, dpr);

        if (fs_ready) {
            if (panel == Panel::Image) {
                if (img_rgba.empty())
                    try_load_png();
                if (!img_rgba.empty())
                    blit_rgba_to_window(
                        g, view, reinterpret_cast<const std::uint32_t*>(img_rgba.data()),
                        img_w, img_h);
            } else {
                if (!sequence_ready)
                    try_open_sequence();
                if (sequence_ready && last_rgba.empty() && seq_current == 0)
                    restart_sequence();
                if (!last_rgba.empty() && pic_w > 0 && pic_h > 0)
                    blit_rgba_to_window(g, view, last_rgba.data(), pic_w, pic_h);
            }
        } else {
            auto sc = [dpr](int n) { return static_cast<int>(n * dpr + 0.5f); };
            const int msg_y =
                view.h > 0 ? (view.y + sc(12))
                           : (area.y + sc(8) + sc(28) + sc(12));
            apps::g_text(g, area.x + sc(12), msg_y, 0xFF'A0'B0'C0, 1,
                         "Waiting for /media assets...");
        }

        draw_ui(g, area, dpr);
    }

    void on_click(int cx, int cy, float dpr) {
        auto sc = [dpr](int n) { return static_cast<int>(n * dpr + 0.5f); };
        const int margin = sc(8);
        const int tab_y  = sc(8);
        const int tab_h  = sc(28);
        if (cy >= tab_y && cy < tab_y + tab_h) {
            const int tab_w = std::max(40, (last_cw - margin * 3) / 2);
            const int x0    = margin;
            const int x1    = x0 + tab_w + margin;
            if (cx >= x0 && cx < x0 + tab_w) {
                panel   = Panel::Image;
                playing = false;
            } else if (cx >= x1 && cx < x1 + tab_w) {
                panel = Panel::Video;
            }
            return;
        }

        if (panel == Panel::Video && sequence_ready && last_ch > 0) {
            const int py = last_ch - sc(40);
            if (cy >= py && cy < py + sc(28) && cx >= margin && cx < margin + sc(100)) {
                if (playing) {
                    playing = false;
                } else {
                    restart_sequence();
                    playing = true;
                }
            }
        }
    }
};

os::MediaPlayerApp::MediaPlayerApp() : impl_(std::make_unique<MediaPlayerImpl>()) {}

os::MediaPlayerApp::~MediaPlayerApp() = default;

void os::MediaPlayerApp::set_context(gpu::GPU* g, cpu::Memory* m) noexcept {
    impl_->set_context(g, m);
}

void os::MediaPlayerApp::set_fs_ready(bool v) noexcept {
    impl_->set_fs_ready(v);
}

void os::MediaPlayerApp::set_clip_fs_ready(bool v) noexcept {
    impl_->set_clip_fs_ready(v);
}

void os::MediaPlayerApp::step(std::uint32_t tick_count) noexcept {
    impl_->step(tick_count);
}

bool os::MediaPlayerApp::wants_always_dirty() const noexcept {
    return impl_->wants_always_dirty();
}

void os::MediaPlayerApp::render(gpu::GPU& g, WinRect area, std::uint32_t tick_count,
                                float dpr) {
    impl_->render(g, area, tick_count, dpr);
}

void os::MediaPlayerApp::on_click(int cx, int cy, float dpr) {
    impl_->on_click(cx, cy, dpr);
}
