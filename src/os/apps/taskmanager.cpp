#include "taskmanager.hpp"
#include <cstdio>
#include <string>
#include <algorithm>

namespace os::apps {

void render_taskmanager(gpu::GPU& g, WinRect area,
                        const std::vector<PsEntry>& procs,
                        std::size_t mem_bytes,
                        uint32_t /*tick*/,
                        const TaskManagerState& st,
                        float dpr)
{
    auto sc = [dpr](int n) { return int(n * dpr + 0.5f); };
    const int x0 = area.x, y0 = area.y;

    g_rect(g, x0, y0, area.w, area.h, 0xFF'06'0E'1A);
    g_text(g, x0 + sc(10), y0 + sc(14), 0xFF'2A'90'CF, 0, "TASK MANAGER");
    g_line(g, x0 + sc(8), y0 + sc(20), x0 + area.w - sc(8), y0 + sc(20), 0xFF'1A'3A'55);

    const std::size_t mem_mib  = mem_bytes / (1024u * 1024u);
    // Estimate used: kernel + process memory
    std::size_t used_kb = 0;
    for (const auto& p : procs) used_kb += p.mem_kb;
    const std::size_t used_mib = used_kb / 1024 + 1;

    char membuf[48];
    std::snprintf(membuf, sizeof(membuf), "MEM  %zu / %zu MiB", used_mib, mem_mib);
    g_text(g, x0 + sc(10), y0 + sc(34), 0xFF'70'A0'C0, 0, membuf);

    const int bar_y = y0 + sc(38);
    const int bar_w = area.w - sc(16);
    g_rect(g, x0 + sc(8), bar_y, bar_w, sc(6), 0xFF'0A'18'28);
    const int fill_w = (mem_mib > 0) ? int(bar_w * float(used_mib) / float(mem_mib)) : 0;
    g_rect(g, x0 + sc(8), bar_y, std::max(1, fill_w), sc(6), 0xFF'2A'70'CF);

    const int hdr_y  = y0 + sc(52);
    const int list_y = hdr_y + sc(18);
    const int list_h = area.h - (list_y - y0) - sc(4);

    // Column positions
    const int C_PID   = x0 + sc(8);
    const int C_STATE = C_PID  + sc(38);
    const int C_MEM   = C_STATE + sc(22);
    const int C_CPU   = C_MEM   + sc(50);
    const int C_NAME  = C_CPU   + sc(44);

    g_rect(g, x0, hdr_y, area.w, sc(18), 0xFF'0A'18'28);
    g_text(g, C_PID,   hdr_y + sc(13), 0xFF'50'78'90, 0, "PID");
    g_text(g, C_STATE, hdr_y + sc(13), 0xFF'50'78'90, 0, "ST");
    g_text(g, C_MEM,   hdr_y + sc(13), 0xFF'50'78'90, 0, "MEM(KB)");
    g_text(g, C_CPU,   hdr_y + sc(13), 0xFF'50'78'90, 0, "CPU");
    g_text(g, C_NAME,  hdr_y + sc(13), 0xFF'50'78'90, 0, "COMMAND");

    g_scissor(g, x0, list_y, area.w, list_h);

    const int ROW_H = sc(18);
    int vy = list_y - st.scroll;

    for (const auto& p : procs) {
        if (vy + ROW_H < list_y) { vy += ROW_H; continue; }
        if (vy > list_y + list_h) break;

        const bool running = (p.state == ProcessState::Running);
        const uint32_t row_bg = running ? 0xFF'0C'20'38 : 0xFF'04'0A'14;
        const uint32_t name_col = running ? 0xFF'89'B4'FA : 0xFF'70'A0'C0;

        g_rect(g, x0, vy, area.w, ROW_H - sc(1), row_bg);

        char pid_s[8];   std::snprintf(pid_s,   sizeof(pid_s),   "%u",  p.pid);
        char mem_s[12];  std::snprintf(mem_s,    sizeof(mem_s),   "%u",  p.mem_kb);
        char cpu_s[12];  std::snprintf(cpu_s,    sizeof(cpu_s),   "%llu", (unsigned long long)p.cpu_ticks);

        const char* state_s = (p.state == ProcessState::Running)    ? "R" :
                              (p.state == ProcessState::Blocked)     ? "S" :
                              (p.state == ProcessState::Terminated)  ? "Z" : "r";
        const uint32_t st_col = (p.state == ProcessState::Running)   ? 0xFF'40'CC'60 :
                                (p.state == ProcessState::Blocked)    ? 0xFF'90'90'30 : 0xFF'80'40'40;

        g_text(g, C_PID,   vy + sc(13), 0xFF'80'A0'C0, 0, pid_s);
        g_text(g, C_STATE, vy + sc(13), st_col,         0, state_s);
        g_text(g, C_MEM,   vy + sc(13), 0xFF'60'90'B0, 0, mem_s);
        g_text(g, C_CPU,   vy + sc(13), 0xFF'50'80'A0, 0, cpu_s);
        g_text(g, C_NAME,  vy + sc(13), name_col,       0, p.name);

        vy += ROW_H;
    }

    g_scissor_clear(g);
}

} // namespace os::apps
