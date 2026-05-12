#pragma once
#include "apputil.hpp"
#include "../scheduler.hpp"
#include <cstdint>

namespace os::apps {

struct TaskManagerState {
    int scroll = 0;
};

void render_taskmanager(gpu::GPU& g, WinRect area,
                        const std::vector<PsEntry>& procs,
                        std::size_t mem_bytes,
                        uint32_t tick,
                        const TaskManagerState& st,
                        float dpr);

} // namespace os::apps
