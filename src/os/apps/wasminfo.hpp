#pragma once
#include "apputil.hpp"
#include <cstdint>

namespace os::apps {

// Stateless  -  reads live GPU/CPU stats.
void render_wasminfo(gpu::GPU& g, WinRect area,
                     uint64_t cpu_cycles,
                     uint64_t gpu_draw_calls,
                     uint32_t fb_w, uint32_t fb_h,
                     std::size_t mem_bytes,
                     uint32_t tick,
                     float dpr);

} // namespace os::apps
