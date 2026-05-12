#include "calculator.hpp"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>

namespace os::apps {

// Per-cell colours: col_bg / col_fg are 0xAARRGGBB (GPU packed format).
struct CalcBtn { const char* label; uint32_t col_bg; uint32_t col_fg; };

static constexpr CalcBtn BTNS[5][4] = {
    { {"C",  0xFF'1E'3A'58, 0xFF'E0'60'60},
      {"±",  0xFF'1E'3A'58, 0xFF'89'B4'FA},
      {"%",  0xFF'1E'3A'58, 0xFF'89'B4'FA},
      {"÷",  0xFF'2A'50'80, 0xFF'C8'E8'FF} },
    { {"7",  0xFF'0E'1E'30, 0xFF'C6'D0'F5},
      {"8",  0xFF'0E'1E'30, 0xFF'C6'D0'F5},
      {"9",  0xFF'0E'1E'30, 0xFF'C6'D0'F5},
      {"*",  0xFF'2A'50'80, 0xFF'C8'E8'FF} },
    { {"4",  0xFF'0E'1E'30, 0xFF'C6'D0'F5},
      {"5",  0xFF'0E'1E'30, 0xFF'C6'D0'F5},
      {"6",  0xFF'0E'1E'30, 0xFF'C6'D0'F5},
      {"-",  0xFF'2A'50'80, 0xFF'C8'E8'FF} },
    { {"1",  0xFF'0E'1E'30, 0xFF'C6'D0'F5},
      {"2",  0xFF'0E'1E'30, 0xFF'C6'D0'F5},
      {"3",  0xFF'0E'1E'30, 0xFF'C6'D0'F5},
      {"+",  0xFF'2A'50'80, 0xFF'C8'E8'FF} },
    { {"0",  0xFF'0E'1E'30, 0xFF'C6'D0'F5},
      {".",  0xFF'0E'1E'30, 0xFF'C6'D0'F5},
      {"⌫",  0xFF'1A'30'48, 0xFF'89'B4'FA},
      {"=",  0xFF'1C'60'AA, 0xFF'E8'F4'FF} },
};

static void btn_rect(WinRect area, int row, int col,
                     int PAD, int BTN_H, int BTN_W,
                     int& bx, int& by, int& bw, int& bh)
{
    bw = BTN_W - PAD;
    bh = BTN_H - PAD;
    bx = area.x + PAD / 2 + col * BTN_W;
    by = area.y + PAD / 2 + row * BTN_H;
}

void render_calculator(gpu::GPU& g, WinRect area,
                       const CalculatorState& st, float dpr)
{
    auto sc = [dpr](int n) { return int(n * dpr + 0.5f); };
    const int x0 = area.x, y0 = area.y;

    g_rect(g, x0, y0, area.w, area.h, 0xFF'06'0E'1A);
    g_text(g, x0 + sc(10), y0 + sc(14), 0xFF'2A'90'CF, 0, "CALCULATOR.APP");
    g_line(g, x0 + sc(8), y0 + sc(20), x0 + area.w - sc(8), y0 + sc(20), 0xFF'1A'3A'55);

    const int disp_h = sc(54);
    const int disp_y = y0 + sc(24);
    g_rect(g, x0 + sc(8), disp_y, area.w - sc(16), disp_h, 0xFF'03'08'12);
    g_line(g, x0 + sc(8), disp_y + disp_h, x0 + area.w - sc(8), disp_y + disp_h, 0xFF'2A'4A'70);

    const std::string& disp = st.display;
    uint8_t  dsz  = (disp.size() > 9) ? 0 : 1;
    uint32_t dcol = st.error ? 0xFF'CC'44'44 : 0xFF'C6'D0'F5;
    const float dw = g.font(dsz).measure_width(disp.c_str());
    g_text(g, x0 + area.w - sc(16) - int(dw), disp_y + (dsz == 1 ? sc(36) : sc(40)), dcol, dsz, disp);

    // Pending operator indicator
    if (!st.op.empty() && !st.error) {
        g_text(g, x0 + sc(12), disp_y + sc(14), 0xFF'4A'8A'CF, 0, st.op);
    }

    const int grid_top = disp_y + disp_h + sc(6);
    const int grid_h   = area.y + area.h - grid_top - sc(4);
    const int BTN_H    = grid_h / 5;
    const int BTN_W    = area.w / 4;
    const int PAD      = sc(3);

    for (int r = 0; r < 5; ++r) {
        for (int c = 0; c < 4; ++c) {
            int bx, by, bw, bh;
            btn_rect({ area.x, grid_top, area.w, grid_h }, r, c, PAD, BTN_H, BTN_W, bx, by, bw, bh);
            if (bw <= 0 || bh <= 0) continue;
            g_rect(g, bx, by, bw, bh, BTNS[r][c].col_bg);
            const char* lbl = BTNS[r][c].label;
            // Handle multi-byte label "⌫" by showing "<"
            const char* show = (lbl[0] < 0) ? "<" : lbl;
            const float lw = g.font(0).measure_width(show);
            g_text(g, bx + int((bw - lw) * 0.5f), by + bh / 2 + sc(5),
                   BTNS[r][c].col_fg, 0, show);
        }
    }
}

bool click_calculator(int lx, int ly, WinRect area,
                      CalculatorState& st, float dpr)
{
    auto sc = [dpr](int n) { return int(n * dpr + 0.5f); };

    const int disp_h   = sc(54);
    const int disp_y   = sc(24);
    const int grid_top = disp_y + disp_h + sc(6);
    const int grid_h   = area.h - grid_top - sc(4);
    if (ly < grid_top || grid_h <= 0) return false;

    const int BTN_H = grid_h / 5;
    const int BTN_W = area.w / 4;
    if (BTN_H <= 0 || BTN_W <= 0) return false;

    const int row = (ly - grid_top) / BTN_H;
    const int col = lx / BTN_W;
    if (row < 0 || row > 4 || col < 0 || col > 3) return false;

    const char* lbl = BTNS[row][col].label;

    if (lbl[0] >= '0' && lbl[0] <= '9') {
        if (st.next_clears) { st.display = lbl; st.next_clears = false; st.had_dot = false; }
        else if (st.display == "0") st.display = lbl;
        else st.display += lbl;
        return true;
    }

    if (lbl[0] == '.') {
        if (st.next_clears) { st.display = "0."; st.next_clears = false; st.had_dot = true; }
        else if (!st.had_dot) { st.display += '.'; st.had_dot = true; }
        return true;
    }

    if (lbl[0] < 0) { // "⌫"
        if (!st.display.empty() && !st.next_clears) {
            if (st.display.back() == '.') st.had_dot = false;
            st.display.pop_back();
            if (st.display.empty() || st.display == "-") st.display = "0";
        }
        return true;
    }

    if (lbl[0] == 'C') {
        st.display = "0"; st.accum = 0.0; st.op = "";
        st.next_clears = true; st.had_dot = false; st.error = false;
        return true;
    }

    if (lbl[0] == '\xc2') { // ± (UTF-8: 0xC2 0xB1)
        if (st.display != "0" && !st.error) {
            if (st.display[0] == '-') st.display.erase(0,1);
            else st.display.insert(0, "-");
        }
        return true;
    }

    if (lbl[0] == '%') {
        if (!st.error) {
            double v = std::atof(st.display.c_str()) / 100.0;
            char buf[24]; std::snprintf(buf, sizeof(buf), "%.10g", v);
            st.display = buf; st.next_clears = true;
        }
        return true;
    }

    if ((lbl[0] == '\xc3' && lbl[1] == '\xb7') || lbl[0] == '*' ||
        lbl[0] == '-' || lbl[0] == '+') {
        if (!st.error) {
            if (!st.op.empty() && !st.next_clears) {
                double b = std::atof(st.display.c_str());
                if (st.op == "÷" && b == 0.0) { st.display = "ERR"; st.error = true; st.op = ""; return true; }
                if (st.op == "÷") st.accum /= b;
                else if (st.op == "*") st.accum *= b;
                else if (st.op == "+") st.accum += b;
                else if (st.op == "-") st.accum -= b;
                char buf[24]; std::snprintf(buf, sizeof(buf), "%.10g", st.accum);
                st.display = buf;
            } else {
                st.accum = std::atof(st.display.c_str());
            }
            if      (lbl[0] == '-') st.op = "-";
            else if (lbl[0] == '+') st.op = "+";
            else if (lbl[0] == '\xc3' && lbl[1] == '\xb7') st.op = "÷";
            else if (lbl[0] == '*') st.op = "*";
            st.next_clears = true; st.had_dot = false;
        }
        return true;
    }

    if (lbl[0] == '=') {
        if (!st.error && !st.op.empty()) {
            double b = std::atof(st.display.c_str());
            if (st.op == "÷" && b == 0.0) { st.display = "ERR"; st.error = true; st.op = ""; return true; }
            if (st.op == "÷") st.accum /= b;
            else if (st.op == "*") st.accum *= b;
            else if (st.op == "+") st.accum += b;
            else if (st.op == "-") st.accum -= b;
            char buf[24]; std::snprintf(buf, sizeof(buf), "%.10g", st.accum);
            st.display = buf; st.op = ""; st.next_clears = true; st.had_dot = false;
        }
        return true;
    }

    return false;
}

} // namespace os::apps
