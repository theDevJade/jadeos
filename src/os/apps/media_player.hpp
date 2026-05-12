#pragma once
#include "../wm.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gpu {
class GPU;
}
namespace cpu {
class Memory;
}

namespace os {

struct MediaPlayerImpl;

class MediaPlayerApp {
public:
    MediaPlayerApp();
    ~MediaPlayerApp();

    MediaPlayerApp(const MediaPlayerApp&)            = delete;
    MediaPlayerApp& operator=(const MediaPlayerApp&) = delete;

    void set_context(gpu::GPU* g, cpu::Memory* m) noexcept;

    void set_fs_ready(bool v) noexcept;

    void set_clip_fs_ready(bool v) noexcept;

    void step(std::uint32_t tick_count) noexcept;

    [[nodiscard]] bool wants_always_dirty() const noexcept;

    void render(gpu::GPU& g, WinRect area, std::uint32_t tick_count, float dpr);

    void on_click(int cx, int cy, float dpr);

private:
    std::unique_ptr<MediaPlayerImpl> impl_;
};

}  // namespace os
