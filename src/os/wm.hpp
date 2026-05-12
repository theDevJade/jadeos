#pragma once
#include "../gpu/gpu.hpp"
#include <functional>
#include <string>
#include <vector>

namespace os {

constexpr int TITLE_H  = 22;   // title bar height in pixels
constexpr int BORDER   = 2;    // chrome border width
constexpr int RESIZE_BORDER = 6; // interactive resize zone width

struct WinRect {
    int x, y, w, h;
    bool contains(int px, int py) const noexcept {
        return px >= x && px < x+w && py >= y && py < y+h;
    }
    bool operator==(const WinRect& o) const noexcept {
        return x==o.x && y==o.y && w==o.w && h==o.h;
    }
    bool operator!=(const WinRect& o) const noexcept { return !(*this==o); }
};

enum class ResizeEdge { None, N, S, E, W, NE, NW, SE, SW };

struct Window {
    int  id       = 0;
    WinRect frame{};        // outer rect including chrome
    std::string title;
    bool focused   = false;
    bool visible   = true;
    bool minimized = false;
    WinRect saved_frame{};  // saved for restore after min/max

    using RenderFn = std::function<void(gpu::GPU&, WinRect)>;
    using ClickFn  = std::function<void(int, int)>;           // local coords
    using KeyFn    = std::function<void(uint32_t, uint32_t)>;
    using ScrollFn = std::function<void(int delta)>;

    RenderFn on_render;
    ClickFn  on_click;
    KeyFn    on_key;
    ScrollFn on_scroll;

    int chrome_title_h = TITLE_H;  // DPR-scaled at creation by WindowManager
    int chrome_border   = BORDER;   // DPR-scaled border width
    WinRect target_frame{};
    bool    closing = false;

    bool     always_dirty        = true;
    bool     needs_redraw        = true;   // one-shot redraw request
    WinRect  last_rendered_frame {};

    bool        title_editing    = false;
    std::string title_edit_buf;

    // Float geometry for springs; rounded to int Window::frame after each step_animations().
    float fx = 0.f, fy = 0.f, fw = 0.f, fh = 0.f;
    float vx = 0.f, vy = 0.f, vsw = 0.f, vsh = 0.f;

    bool floated = false;

    WinRect content_rect() const noexcept {
        return { frame.x + chrome_border,
                 frame.y + chrome_title_h,
                 frame.w - chrome_border * 2,
                 frame.h - chrome_title_h - chrome_border };
    }
};

struct DragState {
    int  win_id  = -1;
    int  off_x   = 0;
    int  off_y   = 0;
};

struct ResizeState {
    int        win_id    = -1;
    ResizeEdge edge      = ResizeEdge::None;
    WinRect    orig_frame{};
    int        start_x   = 0;
    int        start_y   = 0;
};

class WindowManager {
public:
    WindowManager(gpu::GPU& gpu, int screen_w, int screen_h) noexcept
        : gpu_(gpu), screen_w_(screen_w), screen_h_(screen_h) {}

    void set_screen_size(int w, int h) noexcept { screen_w_ = w; screen_h_ = h; }
    void set_dpr(float dpr) noexcept { dpr_ = std::max(0.5f, std::min(dpr, 4.0f)); }
    int  taskbar_h() const noexcept  { return int(TASKBAR_H * dpr_); }

    void step_animations(float dt) noexcept;

    void recompute_layout() noexcept;

    void reopen_window(int id);

    int add_window(WinRect frame, std::string title,
                   Window::RenderFn on_render,
                   Window::ClickFn  on_click  = {},
                   Window::KeyFn    on_key    = {},
                   Window::ScrollFn on_scroll = {});

    void focus(int id);
    void cycle_focus();

    void close_focused();
    void toggle_float_focused();
    void toggle_maximize_focused();

    void toggle_launcher();
    bool launcher_open() const noexcept { return launcher_open_; }
    void draw_launcher();

    void mark_dirty(int id) noexcept;
    WinRect content_rect_for(int id) const noexcept;
    void set_always_dirty(int id, bool v) noexcept;

    void render_all();

    void draw_taskbar(uint32_t tick_count, uint64_t cpu_cycles, std::size_t ram_mib);

    void set_wall_time(uint32_t unix_sec) noexcept;

    uint32_t wall_unix_sec() const noexcept { return wall_unix_sec_; }

    uint8_t wall_hour() const noexcept { return wall_hour_; }
    uint8_t wall_min()  const noexcept { return wall_min_;  }
    uint8_t wall_sec()  const noexcept { return wall_sec_;  }

    void close_window(int id);
    void toggle_minimize(int id);
    void toggle_maximize(int id);
    void toggle_float(int id);

    void mouse_down(int x, int y);
    void mouse_move(int x, int y);
    void mouse_up();

    void handle_scroll(int delta);

    void handle_key(uint32_t keycode, uint32_t charcode);

    void tile_horizontal(int x, int y, int w, int h);

    int focused_id() const noexcept { return focused_id_; }

    // True when the window exists and would be painted (visible, not minimized/closing).
    bool window_shown(int id) const noexcept;

    static constexpr int TASKBAR_H   = 28;  // waybar height
    static constexpr int STATUSBAR_H  = 0;   // no bottom bar (waybar only)

private:
    gpu::GPU&           gpu_;
    int                 screen_w_;
    int                 screen_h_;
    float               dpr_          = 1.0f;
    float               master_ratio_ = 0.50f;  // fraction of width for master tile
    std::vector<Window> windows_;
    std::vector<int>    tile_order_;             // window ids in tile order (independent of z-order)
    int                 next_id_      = 1;
    int                 focused_id_   = -1;

    uint32_t            wall_unix_sec_ = 0;

    uint8_t             wall_hour_    = 12;
    uint8_t             wall_min_     = 0;
    uint8_t             wall_sec_     = 0;

    DragState           drag_{};
    ResizeState         resize_{};

    uint32_t            wm_tick_        = 0;
    uint32_t            dbl_click_tick_ = 0;
    int                 dbl_click_win_  = -1;

    bool                launcher_open_  = false;
    int                 launcher_sel_   = 0;
    std::string         launcher_query_;

    Window* find(int id);
    void draw_chrome(const Window& win);
    ResizeEdge hit_edge(const Window& w, int x, int y) const noexcept;
    bool       hit_titlebar(const Window& w, int x, int y) const noexcept;
    void       bring_to_front(int id);

    // Returns indices into windows_ matching launcher_query_ (case-insensitive substring).
    std::vector<std::size_t> launcher_filtered_indices() const noexcept;

    void g_rect(int x, int y, int w, int h, uint32_t col);
    void g_rect_a(int x, int y, int w, int h, uint32_t col);
    void g_line(int x0, int y0, int x1, int y1, uint32_t col);
    void g_text(int x, int y, uint32_t col, uint8_t sz, const std::string& s);
    void g_circle(int cx, int cy, int r, uint32_t col);
};

}  // namespace os
