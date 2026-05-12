#include "clock.hpp"
#include <cstdio>
#include <cmath>
#include <string>

namespace os::apps {

void render_clock(gpu::GPU& g, WinRect area,
                  uint8_t wall_h, uint8_t wall_m, uint8_t wall_s,
                  uint32_t /*tick*/, float dpr)
{
    auto sc = [dpr](int n) { return int(n * dpr + 0.5f); };
    const int x0 = area.x, y0 = area.y;

    g_rect(g, x0, y0, area.w, area.h, 0xFF'06'0E'1A);

    g_text(g, x0 + sc(10), y0 + sc(14), 0xFF'2A'90'CF, 0, "CLOCK.APP");
    g_line(g, x0 + sc(8), y0 + sc(20), x0 + area.w - sc(8), y0 + sc(20), 0xFF'1A'3A'55);

    const uint32_t h12  = (wall_h % 12 == 0) ? 12 : (wall_h % 12);
    const char*    ampm = (wall_h < 12) ? "AM" : "PM";

    char timebuf[10];
    std::snprintf(timebuf, sizeof(timebuf), "%02u:%02u:%02u", h12, wall_m, wall_s);

    const float dw = g.font(1).measure_width(timebuf);
    g_text(g, x0 + int((area.w - dw) * 0.5f), y0 + sc(48), 0xFF'89'B4'FA, 1, timebuf);

    const float aw = g.font(0).measure_width(ampm);
    g_text(g, x0 + int((area.w - aw) * 0.5f), y0 + sc(66), 0xFF'4A'6A'8A, 0, ampm);

    const int avail_h = area.h - sc(82);
    const int R = std::min(area.w / 2, avail_h / 2) - sc(14);
    if (R < sc(18)) return;

    const int cx = x0 + area.w / 2;
    const int cy = y0 + sc(82) + avail_h / 2;

    // Face ring
    g_circle(g, cx, cy, R + sc(3), 0xFF'89'B4'FA);
    g_circle(g, cx, cy, R, 0xFF'0B'16'24);

    // Hour ticks
    for (int i = 0; i < 12; ++i) {
        float a = float(i) * (3.14159265f / 6.0f);
        float sa = std::sin(a), ca = std::cos(a);
        const bool major = (i % 3 == 0);
        const int  r0    = R - (major ? sc(10) : sc(5));
        const int  r1    = R - sc(2);
        g_line(g,
               cx + int(sa * r0 + 0.5f), cy - int(ca * r0 + 0.5f),
               cx + int(sa * r1 + 0.5f), cy - int(ca * r1 + 0.5f),
               major ? 0xFF'89'B4'FA : 0xFF'1E'42'62);
    }

    // Hour hand
    {
        float a  = (float(wall_h % 12) + float(wall_m) / 60.f) * (3.14159265f / 6.0f);
        int   ln = int(R * 0.52f);
        g_line(g, cx, cy,
               cx + int(std::sin(a) * ln + 0.5f),
               cy - int(std::cos(a) * ln + 0.5f), 0xFF'C6'D0'F5);
    }
    // Minute hand
    {
        float a  = (float(wall_m) + float(wall_s) / 60.f) * (3.14159265f / 30.0f);
        int   ln = int(R * 0.75f);
        g_line(g, cx, cy,
               cx + int(std::sin(a) * ln + 0.5f),
               cy - int(std::cos(a) * ln + 0.5f), 0xFF'89'B4'FA);
    }
    // Second hand
    {
        float a  = float(wall_s) * (3.14159265f / 30.0f);
        int   ln = int(R * 0.88f);
        g_line(g, cx, cy,
               cx + int(std::sin(a) * ln + 0.5f),
               cy - int(std::cos(a) * ln + 0.5f), 0xFF'CC'44'44);
    }
    // Pivot
    g_circle(g, cx, cy, sc(5), 0xFF'CC'44'44);
    g_circle(g, cx, cy, sc(2), 0xFF'FF'FF'FF);
}

} // namespace os::apps
