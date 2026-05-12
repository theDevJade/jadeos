#pragma once
#include "apputil.hpp"
#include <string>
#include <cstdint>

namespace os::apps {

// Stateful 4-function calculator.
struct CalculatorState {
    std::string display  = "0";
    double      accum    = 0.0;
    std::string op;          // pending operator: "+","-","*","/"
    bool        next_clears = true;
    bool        had_dot     = false;
    bool        error       = false;
};

void render_calculator(gpu::GPU& g, WinRect area,
                       const CalculatorState& st, float dpr);

// Returns true if state changed (caller should mark window dirty).
bool click_calculator(int lx, int ly, WinRect area,
                      CalculatorState& st, float dpr);

} // namespace os::apps
