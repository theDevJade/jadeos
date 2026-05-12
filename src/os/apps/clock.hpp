#pragma once
#include "apputil.hpp"
#include <cstdint>

namespace os::apps {

// Stateless  -  reads wall-clock time passed in from Kernel.
void render_clock(gpu::GPU& g, WinRect area,
                  uint8_t wall_h, uint8_t wall_m, uint8_t wall_s,
                  uint32_t tick, float dpr);

} // namespace os::apps
