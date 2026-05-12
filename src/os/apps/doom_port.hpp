#pragma once
#include "../wm.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace gpu {
class GPU;
}
namespace cpu {
class Memory;
}

namespace os {

// Freedoom / Doom IWAD player using vendored doomgeneric (GPL-2.0+).
class DoomPort {
public:
    void set_context(gpu::GPU* g, cpu::Memory* m) noexcept;

    void set_iwad_ready(bool v) noexcept { iwad_ready_ = v; }

    void render(gpu::GPU& g, WinRect area, uint32_t tick, float dpr);

    void on_key(uint32_t keycode, uint32_t charcode);

    void on_mouse(int32_t dx, int32_t dy, uint32_t buttons) noexcept;

    [[nodiscard]] bool wants_pointer_lock() const noexcept;

private:
    void try_boot();
    void draw_placeholder(gpu::GPU& g, WinRect area, float dpr) const;

    gpu::GPU*     gpu_     = nullptr;
    cpu::Memory*  mem_     = nullptr;
    bool          iwad_ready_ = false;
    bool          started_    = false;
    bool          failed_     = false;
    std::string   err_;
    std::vector<uint8_t> scale_buf_;
};

}  // namespace os
