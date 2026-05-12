#include "wm.hpp"
#include "../gpu/framebuffer.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>

namespace os {

// Window chrome colours: packed 0xAARRGGBB (alpha in high byte; see gpu::RGBA::from_u32).
static constexpr uint32_t CLR_CLOSE_BTN    = 0xFF'CC'44'44;
static constexpr uint32_t CLR_MIN_BTN      = 0xFF'CC'A0'20;
static constexpr uint32_t CLR_MAX_BTN      = 0xFF'30'A0'40;
static constexpr uint32_t CLR_BG_WIN       = 0xFF'06'0E'1A;

// ALT+TAB launcher: Spotlight-style centered card (layout shared by draw + hit-test).
struct LauncherLayout {
    int px = 0;
    int py = 0;
    int pw = 0;
    int ph = 0;
    int pad = 0;
    int header_h = 0;
    int row_h = 0;
    int list_y = 0;
    int footer_h = 0;
};

static LauncherLayout compute_launcher_layout(int W, int H, float dpr, int n) noexcept {
    LauncherLayout L;
    auto sc = [dpr](int v) { return static_cast<int>(v * dpr + 0.5f); };
    L.pad       = sc(14);
    L.header_h  = sc(48);
    L.footer_h  = sc(30);
    const int side_m = sc(24);
    L.pw        = (std::max)(sc(280), (std::min)(W - side_m * 2, sc(420)));
    const int row_ideal = sc(50);
    const int max_ph    = (std::max)(sc(160), H - sc(40));
    const int base      = L.pad + L.header_h + L.footer_h + L.pad;
    L.row_h = row_ideal;
    if (n > 0 && base + n * L.row_h > max_ph)
        L.row_h = (std::max)(sc(34), (max_ph - base) / n);
    L.ph = base + n * L.row_h;
    L.px = (W - L.pw) / 2;
    L.py = (std::max)(sc(24), (H - L.ph) / 3);
    L.list_y = L.py + L.pad + L.header_h;
    return L;
}

static std::string ellipsize_to_width(const gpu::FontAtlas& f, const std::string& s,
                                      float max_w) {
    if (s.empty() || f.measure_width(s.c_str()) <= max_w)
        return s;
    static const char ell[] = "...";
    const float ell_w = f.measure_width(ell);
    if (ell_w > max_w)
        return "";
    std::string t = s;
    while (t.size() > 1) {
        t.pop_back();
        if (f.measure_width(t.c_str()) + ell_w <= max_w)
            return t + ell;
    }
    return t + ell;
}

void WindowManager::g_rect(int x, int y, int w, int h, uint32_t col) {
    gpu::CmdPacket p;
    p.cmd     = gpu::Command::DRAW_RECT;
    p.args[0] = static_cast<uint32_t>(x);
    p.args[1] = static_cast<uint32_t>(y);
    p.args[2] = static_cast<uint32_t>(w);
    p.args[3] = static_cast<uint32_t>(h);
    p.args[4] = col;
    gpu_.push_command(p);
}

void WindowManager::g_rect_a(int x, int y, int w, int h, uint32_t col) {
    gpu::CmdPacket p;
    p.cmd     = gpu::Command::DRAW_RECT_ALPHA;
    p.args[0] = static_cast<uint32_t>(x);
    p.args[1] = static_cast<uint32_t>(y);
    p.args[2] = static_cast<uint32_t>(w);
    p.args[3] = static_cast<uint32_t>(h);
    p.args[4] = col;
    gpu_.push_command(p);
}

void WindowManager::g_line(int x0, int y0, int x1, int y1, uint32_t col) {
    gpu::CmdPacket p;
    p.cmd     = gpu::Command::DRAW_LINE;
    p.args[0] = static_cast<uint32_t>(x0);
    p.args[1] = static_cast<uint32_t>(y0);
    p.args[2] = static_cast<uint32_t>(x1);
    p.args[3] = static_cast<uint32_t>(y1);
    p.args[4] = col;
    gpu_.push_command(p);
}

void WindowManager::g_text(int x, int y, uint32_t col, uint8_t sz, const std::string& s) {
    if (s.empty()) return;
    gpu::TextRequest r;
    r.x = static_cast<float>(x);
    r.y = static_cast<float>(y);
    r.colour = gpu::RGBA::from_u32(col);
    r.font_size_id = sz;
    r.text = s;
    gpu_.draw_text(r);
}

void WindowManager::g_circle(int cx, int cy, int r, uint32_t col) {
    gpu::CmdPacket p;
    p.cmd     = gpu::Command::DRAW_CIRCLE;
    p.args[0] = static_cast<uint32_t>(cx);
    p.args[1] = static_cast<uint32_t>(cy);
    p.args[2] = static_cast<uint32_t>(r);
    p.args[3] = 0;  // filled
    p.args[4] = col;
    gpu_.push_command(p);
}

int WindowManager::add_window(WinRect /*frame_hint*/, std::string title,
                               Window::RenderFn on_render,
                               Window::ClickFn  on_click,
                               Window::KeyFn    on_key,
                               Window::ScrollFn on_scroll)
{
    Window w;
    w.id             = next_id_++;
    w.title          = std::move(title);
    w.on_render      = std::move(on_render);
    w.on_click       = std::move(on_click);
    w.on_key         = std::move(on_key);
    w.on_scroll      = std::move(on_scroll);
    w.visible        = true;
    w.minimized      = false;
    w.chrome_title_h = int(TITLE_H * dpr_);
    w.chrome_border  = int(BORDER  * dpr_);

    tile_order_.push_back(w.id);
    windows_.push_back(std::move(w));

    if (focused_id_ == -1) {
        focused_id_ = windows_.back().id;
        windows_.back().focused = true;
    }

    // Compute tile targets for ALL windows (including this new one).
    recompute_layout();

    // Opening animation: start from 93% of the computed target.
    Window* wp = &windows_.back();
    const WinRect& tf = wp->target_frame;
    const int adw = tf.w / 15, adh = tf.h / 15;
    wp->frame = { tf.x + adw, tf.y + adh, tf.w - adw*2, tf.h - adh*2 };
    wp->fx = float(wp->frame.x); wp->fy = float(wp->frame.y);
    wp->fw = float(wp->frame.w); wp->fh = float(wp->frame.h);
    wp->vx = wp->vy = wp->vsw = wp->vsh = 0.0f;

    return wp->id;
}

// Master-stack tiling: writes target_frame per visible, non-floated window. Call after tile_order_ or master_ratio_ changes.
void WindowManager::recompute_layout() noexcept {
    // Collect active windows in stable tile order (independent of z-order).
    std::vector<Window*> active;
    active.reserve(tile_order_.size());
    for (int id : tile_order_) {
        Window* w = find(id);
        if (w && w->visible && !w->minimized && !w->closing)
            active.push_back(w);
    }

    const int n = static_cast<int>(active.size());
    if (n == 0) return;

    std::vector<Window*> tiled;
    tiled.reserve(active.size());
    for (auto* w : active)
        if (!w->floated) tiled.push_back(w);
    const int nt = static_cast<int>(tiled.size());
    if (nt == 0) return;

    const int ty = taskbar_h();
    const int tw = screen_w_;
    const int th = screen_h_ - ty;

    if (nt == 1) {
        tiled[0]->target_frame = { 0, ty, tw, th };
        return;
    }

    // Master (index 0): left portion.  Stack (indices 1..nt-1): right portion.
    const int master_w = std::max(80, static_cast<int>(float(tw) * master_ratio_));
    const int stack_w  = tw - master_w;
    const int stack_n  = nt - 1;

    tiled[0]->target_frame = { 0, ty, master_w, th };

    // Equal stack row heights; last row absorbs rounding so sum matches th.
    const int each_h = th / stack_n;
    for (int i = 0; i < stack_n; ++i) {
        const int sy = ty + i * each_h;
        const int sh = (i == stack_n - 1) ? (ty + th - sy) : each_h;
        tiled[i + 1]->target_frame = { master_w, sy, stack_w, sh };
    }
}

// Critically damped springs (K=1800, D=90, ~90ms settle). Non-overlapping layout avoids a PBD pass.
void WindowManager::step_animations(float dt) noexcept {
    constexpr float K       = 1800.0f;
    constexpr float D       =   90.0f;
    constexpr float VEL_EPS =    0.6f;
    constexpr float POS_EPS =    0.4f;

    const float tbh = float(taskbar_h());

    for (auto& w : windows_) {
        if (!w.visible && !w.closing) continue;
        if (drag_.win_id == w.id || resize_.win_id == w.id) continue;

        const float tx = float(w.target_frame.x), ty = float(w.target_frame.y);
        const float tw = float(w.target_frame.w), th = float(w.target_frame.h);

        w.vx  += (K * (tx - w.fx)  - D * w.vx)  * dt;
        w.vy  += (K * (ty - w.fy)  - D * w.vy)  * dt;
        w.vsw += (K * (tw - w.fw)  - D * w.vsw) * dt;
        w.vsh += (K * (th - w.fh)  - D * w.vsh) * dt;

        w.fx += w.vx  * dt;
        w.fy += w.vy  * dt;
        w.fw += w.vsw * dt;
        w.fh += w.vsh * dt;

        if (w.closing) {
            if (w.fw < 0.0f) { w.fw = 0.0f; w.vsw = 0.0f; }
            if (w.fh < 0.0f) { w.fh = 0.0f; w.vsh = 0.0f; }
        } else {
            if (w.fw < 20.0f) { w.fw = 20.0f; w.vsw = std::max(w.vsw, 0.0f); }
            if (w.fh < 20.0f) { w.fh = 20.0f; w.vsh = std::max(w.vsh, 0.0f);             }
        }

        if (std::abs(w.fx - tx)  < POS_EPS && std::abs(w.vx)  < VEL_EPS &&
            std::abs(w.fy - ty)  < POS_EPS && std::abs(w.vy)  < VEL_EPS &&
            std::abs(w.fw - tw)  < POS_EPS && std::abs(w.vsw) < VEL_EPS &&
            std::abs(w.fh - th)  < POS_EPS && std::abs(w.vsh) < VEL_EPS) {
            w.fx = tx; w.fy = ty; w.fw = tw; w.fh = th;
            w.vx = w.vy = w.vsw = w.vsh = 0.0f;
            if (w.closing) {
                w.visible  = false;
                w.closing  = false;
                w.frame    = w.saved_frame;
                w.target_frame = w.saved_frame;
                w.fx = float(w.saved_frame.x); w.fy = float(w.saved_frame.y);
                w.fw = float(w.saved_frame.w); w.fh = float(w.saved_frame.h);
            }
        }
    }

    for (auto& w : windows_) {
        if (!w.visible || w.minimized) continue;
        if (w.fy < tbh) { w.fy = tbh; if (w.vy < 0.0f) w.vy *= -0.05f; }
        if (w.fx < 0.0f)                       w.fx = 0.0f;
        if (w.fx + w.fw > float(screen_w_))    w.fx = float(screen_w_) - w.fw;
        if (w.fy + w.fh > float(screen_h_))    w.fy = float(screen_h_) - w.fh;
    }

    for (auto& w : windows_) {
        if (!w.visible && !w.closing) continue;
        w.frame.x = static_cast<int>(w.fx + 0.5f);
        w.frame.y = static_cast<int>(w.fy + 0.5f);
        w.frame.w = static_cast<int>(w.fw + 0.5f);
        w.frame.h = static_cast<int>(w.fh + 0.5f);
    }
    ++wm_tick_;
}

void WindowManager::reopen_window(int id) {
    Window* w = find(id);
    if (!w) return;
    // Add back to tile order if absent (was removed on close/minimize).
    if (std::find(tile_order_.begin(), tile_order_.end(), id) == tile_order_.end())
        tile_order_.push_back(id);
    w->closing   = false;
    w->minimized = false;
    w->visible   = true;
    w->chrome_title_h = int(TITLE_H * dpr_);
    w->chrome_border  = int(BORDER  * dpr_);
    focus(id);
    bring_to_front(id);
    recompute_layout();
    // Opening animation: spring from 93% of computed target.
    const WinRect& tf = w->target_frame;
    const int adw = tf.w / 15, adh = tf.h / 15;
    w->frame = { tf.x + adw, tf.y + adh, tf.w - adw*2, tf.h - adh*2 };
    w->fx = float(w->frame.x); w->fy = float(w->frame.y);
    w->fw = float(w->frame.w); w->fh = float(w->frame.h);
    w->vx = w->vy = w->vsw = w->vsh = 0.0f;
}

Window* WindowManager::find(int id) {
    for (auto& w : windows_)
        if (w.id == id) return &w;
    return nullptr;
}

bool WindowManager::window_shown(int id) const noexcept {
    for (const auto& w : windows_)
        if (w.id == id)
            return w.visible && !w.minimized && !w.closing;
    return false;
}

void WindowManager::focus(int id) {
    for (auto& w : windows_) w.focused = (w.id == id);
    focused_id_ = id;
}

void WindowManager::bring_to_front(int id) {
    for (std::size_t i = 0; i < windows_.size(); ++i) {
        if (windows_[i].id == id) {
            Window tmp = std::move(windows_[i]);
            windows_.erase(windows_.begin() + static_cast<std::ptrdiff_t>(i));
            windows_.push_back(std::move(tmp));
            return;
        }
    }
}

void WindowManager::cycle_focus() {
    if (windows_.empty()) return;
    // Find next visible, non-minimized window.
    const int n = static_cast<int>(windows_.size());
    int cur = -1;
    for (int i = 0; i < n; ++i)
        if (windows_[i].id == focused_id_) { cur = i; break; }
    for (int d = 1; d < n; ++d) {
        int idx = ((cur < 0 ? 0 : cur) + d) % n;
        if (windows_[idx].visible && !windows_[idx].minimized) {
            focus(windows_[idx].id);
            bring_to_front(windows_[idx].id);
            return;
        }
    }
}

bool WindowManager::hit_titlebar(const Window& w, int x, int y) const noexcept {
    return x >= w.frame.x && x < w.frame.x + w.frame.w
        && y >= w.frame.y && y < w.frame.y + w.chrome_title_h;
}

ResizeEdge WindowManager::hit_edge(const Window& w, int x, int y) const noexcept {
    if (!w.frame.contains(x, y)) return ResizeEdge::None;
    const int rz = int(RESIZE_BORDER * dpr_);
    const bool onL = x < w.frame.x + rz;
    const bool onR = x >= w.frame.x + w.frame.w - rz;
    const bool onT = y < w.frame.y + rz;
    const bool onB = y >= w.frame.y + w.frame.h - rz;
    if (onT && onL) return ResizeEdge::NW;
    if (onT && onR) return ResizeEdge::NE;
    if (onB && onL) return ResizeEdge::SW;
    if (onB && onR) return ResizeEdge::SE;
    if (onT) return ResizeEdge::N;
    if (onB) return ResizeEdge::S;
    if (onL) return ResizeEdge::W;
    if (onR) return ResizeEdge::E;
    return ResizeEdge::None;
}

void WindowManager::mouse_down(int x, int y) {
    if (launcher_open_) {
        const std::vector<std::size_t> fidx = launcher_filtered_indices();
        const int n = static_cast<int>(fidx.size());
        const LauncherLayout L =
            compute_launcher_layout(screen_w_, screen_h_, dpr_, (std::max)(n, 1));
        const bool on_panel =
            x >= L.px && x < L.px + L.pw && y >= L.py && y < L.py + L.ph;
        if (on_panel && y >= L.list_y && y < L.list_y + n * L.row_h && n > 0) {
            const int idx = (y - L.list_y) / L.row_h;
            if (idx >= 0 && idx < n)
                reopen_window(windows_[fidx[static_cast<std::size_t>(idx)]].id);
        }
        launcher_open_ = false;
        return;
    }
    if (y >= 0 && y < taskbar_h()) {
        int bx = int(72 * dpr_);
        for (auto& w : windows_) {
            if (x >= bx && x < bx + int(22 * dpr_)) {
                if (!w.visible || w.minimized || w.closing) {
                    reopen_window(w.id);
                } else if (w.id == focused_id_) {
                    toggle_minimize(w.id);
                } else {
                    focus(w.id);
                    bring_to_front(w.id);
                }
                return;
            }
            bx += int(26 * dpr_);
        }
        return;
    }

    for (int i = static_cast<int>(windows_.size()) - 1; i >= 0; --i) {
        auto& w = windows_[i];
        if (!w.visible || w.minimized) continue;
        if (!w.frame.contains(x, y)) continue;

        focus(w.id);
        bring_to_front(w.id);
        Window* wp = find(w.id);
        if (!wp) return;

        const int btn_cy = wp->frame.y + wp->chrome_title_h / 2;
        const int btn_r  = int(6 * dpr_);
        const int step   = int(12 * dpr_);
        if (x >= wp->frame.x + wp->frame.w - step    && x < wp->frame.x + wp->frame.w
            && y >= btn_cy - btn_r && y < btn_cy + btn_r) {
            close_window(wp->id);
            return;
        }
        if (x >= wp->frame.x + wp->frame.w - step*2  && x < wp->frame.x + wp->frame.w - step
            && y >= btn_cy - btn_r && y < btn_cy + btn_r) {
            toggle_minimize(wp->id);
            return;
        }
        if (x >= wp->frame.x + wp->frame.w - step*3  && x < wp->frame.x + wp->frame.w - step*2
            && y >= btn_cy - btn_r && y < btn_cy + btn_r) {
            toggle_maximize(wp->id);
            return;
        }

        ResizeEdge edge = hit_edge(*wp, x, y);
        if (edge == ResizeEdge::None && hit_titlebar(*wp, x, y)) {
            if (wp->id == dbl_click_win_ && wm_tick_ - dbl_click_tick_ < 30) {
                wp->title_editing    = true;
                wp->title_edit_buf   = wp->title;
                dbl_click_win_       = -1;
            } else {
                dbl_click_win_  = wp->id;
                dbl_click_tick_ = wm_tick_;
                drag_ = { wp->id,
                          x - static_cast<int>(wp->fx + 0.5f),
                          y - static_cast<int>(wp->fy + 0.5f) };
            }
            return;
        }
        if (edge != ResizeEdge::None) {
            resize_ = { wp->id, edge, wp->frame, x, y };
            return;
        }

        auto cr = wp->content_rect();
        if (cr.contains(x, y) && wp->on_click)
            wp->on_click(x - cr.x, y - cr.y);
        return;
    }

    focused_id_ = -1;
    for (auto& w : windows_) w.focused = false;
}

void WindowManager::mouse_move(int x, int y) {
    if (drag_.win_id != -1) {
        Window* w = find(drag_.win_id);
        if (w) {
            w->fx = float(x - drag_.off_x);
            w->fy = float(std::clamp(y - drag_.off_y, 0, screen_h_ - TITLE_H));
            w->vx = 0.0f; w->vy = 0.0f;
        }
        return;
    }

    if (resize_.win_id != -1) {
        int active_idx = -1, ai = 0;
        for (int tid : tile_order_) {
            Window* tw = find(tid);
            if (!tw || !tw->visible || tw->minimized || tw->closing) continue;
            if (tw->id == resize_.win_id) { active_idx = ai; break; }
            ++ai;
        }

        if (active_idx == 0 && (resize_.edge == ResizeEdge::E  ||
                                 resize_.edge == ResizeEdge::NE ||
                                 resize_.edge == ResizeEdge::SE)) {
            master_ratio_ = std::clamp(float(x) / float(screen_w_), 0.15f, 0.85f);
            recompute_layout();
            return;
        }
        return;
    }
}

void WindowManager::mouse_up() {
    if (drag_.win_id != -1) {
        Window* dragged = find(drag_.win_id);
        if (dragged) {
            const int cx = static_cast<int>(dragged->fx + dragged->fw * 0.5f);
            const int cy = static_cast<int>(dragged->fy + dragged->fh * 0.5f);
            for (int tid : tile_order_) {
                if (tid == drag_.win_id) continue;
                Window* other = find(tid);
                if (!other || !other->visible || other->minimized || other->closing) continue;
                if (other->target_frame.contains(cx, cy)) {
                    auto ia = std::find(tile_order_.begin(), tile_order_.end(), drag_.win_id);
                    auto ib = std::find(tile_order_.begin(), tile_order_.end(), tid);
                    if (ia != tile_order_.end() && ib != tile_order_.end())
                        std::iter_swap(ia, ib);
                    break;
                }
            }
        }
        if (dragged && dragged->floated) {
            dragged->target_frame = dragged->frame;
        } else {
            recompute_layout();
        }
        drag_ = {};
    }
    resize_ = {};
}

void WindowManager::mark_dirty(int id) noexcept {
    Window* w = find(id);
    if (w) w->needs_redraw = true;
}

WinRect WindowManager::content_rect_for(int id) const noexcept {
    for (const auto& w : windows_)
        if (w.id == id) return w.content_rect();
    return {};
}

void WindowManager::set_always_dirty(int id, bool v) noexcept {
    Window* w = find(id);
    if (w) { w->always_dirty = v; if (v) w->needs_redraw = true; }
}

void WindowManager::close_window(int id) {
    Window* w = find(id);
    if (!w) return;
    tile_order_.erase(std::remove(tile_order_.begin(), tile_order_.end(), id),
                      tile_order_.end());
    w->saved_frame = w->target_frame;
    const int cx = w->frame.x + w->frame.w / 2;
    const int cy = w->frame.y + w->frame.h / 2;
    w->target_frame = { cx, cy, 1, 1 };
    w->closing = true;
    recompute_layout();
    if (focused_id_ == id) {
        focused_id_ = -1;
        for (auto& win : windows_)
            if (win.visible && !win.minimized && win.id != id) { focus(win.id); break; }
    }
}

void WindowManager::toggle_minimize(int id) {
    Window* w = find(id);
    if (!w) return;
    if (w->minimized) {
        if (std::find(tile_order_.begin(), tile_order_.end(), id) == tile_order_.end())
            tile_order_.push_back(id);
        w->minimized = false;
        w->visible   = true;
        focus(id);
        bring_to_front(id);
        recompute_layout();
        const WinRect& tf = w->target_frame;
        const int adw = tf.w / 15, adh = tf.h / 15;
        w->frame = { tf.x + adw, tf.y + adh, tf.w - adw*2, tf.h - adh*2 };
        w->fx = float(w->frame.x); w->fy = float(w->frame.y);
        w->fw = float(w->frame.w); w->fh = float(w->frame.h);
        w->vx = w->vy = w->vsw = w->vsh = 0.0f;
    } else {
        tile_order_.erase(std::remove(tile_order_.begin(), tile_order_.end(), id),
                          tile_order_.end());
        w->saved_frame = w->target_frame;
        w->minimized   = true;
        recompute_layout();
        if (focused_id_ == id) {
            focused_id_ = -1;
            for (auto& win : windows_)
                if (win.visible && !win.minimized) { focus(win.id); break; }
        }
    }
}

void WindowManager::toggle_maximize(int id) {
    auto it = std::find(tile_order_.begin(), tile_order_.end(), id);
    if (it == tile_order_.end() || it == tile_order_.begin()) return; // already master
    std::iter_swap(it, tile_order_.begin());
    recompute_layout();
}

void WindowManager::toggle_float(int id) {
    Window* w = find(id);
    if (!w) return;
    w->floated = !w->floated;
    if (w->floated) {
        w->target_frame = w->frame;
    } else {
        recompute_layout();
    }
}

void WindowManager::close_focused() {
    if (focused_id_ != -1) close_window(focused_id_);
}

void WindowManager::toggle_float_focused() {
    if (focused_id_ != -1) toggle_float(focused_id_);
}

void WindowManager::toggle_maximize_focused() {
    if (focused_id_ != -1) toggle_maximize(focused_id_);
}

void WindowManager::toggle_launcher() {
    launcher_open_ = !launcher_open_;
    if (launcher_open_) {
        launcher_sel_   = 0;
        launcher_query_.clear();
    }
}

std::vector<std::size_t> WindowManager::launcher_filtered_indices() const noexcept {
    std::vector<std::size_t> result;
    // Lowercase the query for case-insensitive matching.
    std::string q = launcher_query_;
    for (char& c : q) if (c >= 'A' && c <= 'Z') c |= 0x20;
    for (std::size_t idx = 0; idx < windows_.size(); ++idx) {
        if (q.empty()) {
            result.push_back(idx);
        } else {
            std::string t = windows_[idx].title;
            for (char& c : t) if (c >= 'A' && c <= 'Z') c |= 0x20;
            if (t.find(q) != std::string::npos)
                result.push_back(idx);
        }
    }
    return result;
}

void WindowManager::draw_launcher() {
    if (!launcher_open_) return;

    const int W = screen_w_;
    const int H = screen_h_;

    // Build filtered window index list and use its count for layout.
    const std::vector<std::size_t> fidx = launcher_filtered_indices();
    const int n = static_cast<int>(fidx.size());
    // Pass at least 1 so the card always reserves list-area space.
    const LauncherLayout L = compute_launcher_layout(W, H, dpr_, (std::max)(n, 1));
    auto sc = [this](int v) { return static_cast<int>(v * dpr_ + 0.5f); };

    // Dim the background.
    auto dim_pkt = gpu::CmdPacket{};
    dim_pkt.cmd  = gpu::Command::DRAW_RECT_ALPHA;
    dim_pkt.args = { 0, 0, uint32_t(W), uint32_t(H), 0xCC'00'00'00, 0 };
    gpu_.push_command(dim_pkt);

    // Drop shadow layers.
    g_rect_a(L.px + sc(4), L.py + sc(6), L.pw, L.ph, 0x50'00'10'30);
    g_rect_a(L.px + sc(2), L.py + sc(3), L.pw, L.ph, 0x30'00'10'30);

    // ── Theme colours (matching window chrome palette) ──────────────────
    constexpr uint32_t kCard       = 0xFF'0D'15'26;  // dark-navy card bg
    constexpr uint32_t kAccent     = 0xFF'89'B4'FA;  // Catppuccin blue accent
    constexpr uint32_t kBorderSide = 0xFF'2A'3D'5E;  // subtle side/bottom border
    constexpr uint32_t kField      = 0xFF'06'0A'14;  // search field bg
    constexpr uint32_t kFieldBr    = 0xFF'45'85'C8;  // field border / separator
    constexpr uint32_t kRowSel     = 0xFF'17'28'44;  // selected-row highlight
    constexpr uint32_t kText       = 0xFF'C6'D0'F5;  // primary text (light lavender)
    constexpr uint32_t kMuted      = 0xFF'4A'6A'8A;  // muted / hint text
    constexpr uint32_t kStatusOpen = 0xFF'89'B4'FA;  // "open" status colour
    constexpr uint32_t kStatusMin  = 0xFF'45'85'C8;  // "min" status colour
    constexpr uint32_t kStatusCls  = 0xFF'4A'6A'8A;  // "closed" status colour
    constexpr uint32_t kIconBg     = 0xFF'17'28'44;  // icon circle (normal)
    constexpr uint32_t kIconBgSel  = 0xFF'1F'35'60;  // icon circle (selected)
    constexpr uint32_t kSeparator  = 0xFF'14'20'38;  // row separator line

    // Card background.
    g_rect(L.px, L.py, L.pw, L.ph, kCard);

    // Top accent bar (2 px thick).
    g_line(L.px, L.py,     L.px + L.pw, L.py,     kAccent);
    g_line(L.px, L.py + 1, L.px + L.pw, L.py + 1, kAccent);

    // Side and bottom borders (subtle).
    const int brd = L.px + L.pw;
    const int btm = L.py + L.ph;
    g_line(L.px,    L.py, L.px,    btm, kBorderSide);
    g_line(brd - 1, L.py, brd - 1, btm, kBorderSide);
    g_line(L.px,    btm - 1, brd,  btm - 1, kBorderSide);

    // ── Search input field ───────────────────────────────────────────────
    const int inner_x = L.px + L.pad;
    const int inner_w = L.pw - L.pad * 2;
    const int head_y  = L.py + L.pad;
    const int field_h = L.header_h - sc(4);
    g_rect(inner_x, head_y, inner_w, field_h, kField);
    // Field borders: accent top, subtle sides/bottom.
    g_line(inner_x,             head_y,            inner_x + inner_w, head_y,            kAccent);
    g_line(inner_x,             head_y + field_h,  inner_x + inner_w, head_y + field_h,  kFieldBr);
    g_line(inner_x,             head_y,            inner_x,           head_y + field_h,  kFieldBr);
    g_line(inner_x + inner_w - 1, head_y,          inner_x + inner_w - 1, head_y + field_h, kFieldBr);

    const gpu::FontAtlas& f0 = gpu_.font(0);
    const gpu::FontAtlas& f1 = gpu_.font(1);
    const int field_text_x = inner_x + sc(10);
    const int field_text_y =
        head_y + static_cast<int>((field_h - f1.line_height()) * 0.5f + f1.ascent() + 0.5f);

    if (launcher_query_.empty()) {
        g_text(field_text_x, field_text_y, kMuted, 1, "Search windows...");
    } else {
        g_text(field_text_x, field_text_y, kText, 1, launcher_query_);
        // Blinking cursor: toggle every ~30 frames (~500 ms at 60 fps).
        if ((wm_tick_ / 30) % 2 == 0) {
            const float qw  = f1.measure_width(launcher_query_.c_str());
            const int   cx  = field_text_x + static_cast<int>(qw) + sc(1);
            const int   ch  = static_cast<int>(f1.line_height() * 0.8f);
            const int   cy  = head_y + (field_h - ch) / 2;
            g_line(cx, cy, cx, cy + ch, kAccent);
        }
    }

    // Separator between search field and window list.
    g_line(L.px, L.list_y - sc(1), L.px + L.pw, L.list_y - sc(1), kBorderSide);

    // ── Window list ──────────────────────────────────────────────────────
    const int inset  = sc(6);
    const int icon_r = sc(16);

    if (n == 0) {
        // No windows match the current query.
        const std::string msg = launcher_query_.empty()
            ? "No windows open"
            : "No match for \"" + launcher_query_ + "\"";
        const float mw  = f0.measure_width(msg.c_str());
        const int   mx  = L.px + static_cast<int>((L.pw - mw) * 0.5f + 0.5f);
        const int   my  = L.list_y
            + static_cast<int>((L.row_h - f0.line_height()) * 0.5f + f0.ascent() + 0.5f);
        g_text(mx, my, kMuted, 0, msg);
    }

    for (int i = 0; i < n; ++i) {
        const auto& w   = windows_[fidx[static_cast<std::size_t>(i)]];
        const int   ry  = L.list_y + i * L.row_h;
        const bool  sel = (i == launcher_sel_);

        if (sel) {
            g_rect(L.px + inset, ry + sc(1), L.pw - inset * 2, L.row_h - sc(2), kRowSel);
            // Accent line along the top of the selected row.
            g_line(L.px + inset, ry + sc(1),
                   L.px + inset + (L.pw - inset * 2), ry + sc(1), kAccent);
        }

        // Icon circle with uppercased first letter of the window title.
        const int icon_cx = inner_x + sc(18);
        const int icon_cy = ry + L.row_h / 2;
        g_circle(icon_cx, icon_cy, icon_r, sel ? kIconBgSel : kIconBg);

        char icon_ch = w.title.empty() ? '?' : w.title.front();
        if (icon_ch >= 'a' && icon_ch <= 'z') icon_ch = static_cast<char>(icon_ch - 32);
        const std::string icon(1, icon_ch);
        const float       iw  = f0.measure_width(icon.c_str());
        const int         itx = icon_cx - static_cast<int>(iw * 0.5f + 0.5f);
        const int         iby =
            icon_cy + static_cast<int>(f0.ascent() - f0.line_height() * 0.5f + 0.5f);
        g_text(itx, iby, sel ? kAccent : kText, 0, icon);

        // Window state badge.
        const bool     open      = (w.visible && !w.minimized && !w.closing);
        const bool     minimized = w.minimized;
        const char*    stat      = open ? "open" : minimized ? "min" : "closed";
        const uint32_t stat_col  = open ? kStatusOpen : minimized ? kStatusMin : kStatusCls;

        const float stat_w   = f0.measure_width(stat);
        const int   text_lft = inner_x + sc(44);
        const int   text_rgt =
            static_cast<int>(L.px + L.pw - L.pad - stat_w - sc(6));
        const float max_w    =
            static_cast<float>((std::max)(sc(32), text_rgt - text_lft - sc(4)));
        const std::string name = ellipsize_to_width(f0, w.title, max_w);

        const uint32_t name_col = open ? kText : kMuted;
        const int      row_ty   =
            ry + static_cast<int>((L.row_h - f0.line_height()) * 0.5f + f0.ascent() + 0.5f);
        g_text(text_lft, row_ty, name_col, 0, name);
        g_text(text_rgt, row_ty, stat_col, 0, stat);

        // Subtle row separator (skip after last row).
        if (i < n - 1)
            g_line(L.px + inset, ry + L.row_h - 1,
                   L.px + L.pw - inset, ry + L.row_h - 1, kSeparator);
    }

    // ── Footer hint ──────────────────────────────────────────────────────
    const int foot_b =
        L.list_y + (std::max)(n, 1) * L.row_h + sc(6)
        + static_cast<int>((L.footer_h - sc(6) - f0.line_height()) * 0.5f + f0.ascent() + 0.5f);
    const std::string hint = "type to filter   Tab/arrows move   Enter switch   Esc close";
    const float hint_w = f0.measure_width(hint.c_str());
    g_text(L.px + static_cast<int>((L.pw - hint_w) * 0.5f + 0.5f), foot_b, kMuted, 0, hint);
}

void WindowManager::handle_scroll(int delta) {
    if (focused_id_ == -1) return;
    Window* w = find(focused_id_);
    if (w && w->on_scroll) w->on_scroll(delta);
}

void WindowManager::handle_key(uint32_t keycode, uint32_t charcode) {
    const uint32_t vk      = keycode & 0xFFFFu;
    const bool     keydown = (keycode >> 20) & 1u;
    if (launcher_open_) {
        if (!keydown) return;
        const std::vector<std::size_t> fidx = launcher_filtered_indices();
        const int n = static_cast<int>(fidx.size());
        if (vk == 9 /* Tab */ || vk == 40 /* ArrowDown */) {
            launcher_sel_ = (n > 0) ? (launcher_sel_ + 1) % n : 0;
        } else if (vk == 38 /* ArrowUp */) {
            launcher_sel_ = (n > 0) ? (launcher_sel_ - 1 + n) % n : 0;
        } else if (vk == 13 /* Enter */) {
            if (launcher_sel_ >= 0 && launcher_sel_ < n)
                reopen_window(windows_[fidx[static_cast<std::size_t>(launcher_sel_)]].id);
            launcher_open_ = false;
        } else if (vk == 27 /* Escape */) {
            if (!launcher_query_.empty()) {
                launcher_query_.clear();
                launcher_sel_ = 0;
            } else {
                launcher_open_ = false;
            }
        } else if (vk == 8 /* Backspace */) {
            if (!launcher_query_.empty()) {
                launcher_query_.pop_back();
                launcher_sel_ = 0;
            }
        } else if (charcode >= 0x20 && charcode < 0x7F) {
            launcher_query_ += static_cast<char>(charcode);
            launcher_sel_ = 0;  // reset selection on each new character
        }
        return;
    }

    if (focused_id_ == -1) return;
    Window* w = find(focused_id_);
    if (!w) return;

    if (w->title_editing) {
        if (!keydown) return;
        if (vk == 13) {  // Enter: commit
            w->title         = w->title_edit_buf;
            w->title_editing = false;
        } else if (vk == 27) {  // Escape: cancel
            w->title_editing = false;
        } else if (vk == 8) {   // Backspace
            if (!w->title_edit_buf.empty())
                w->title_edit_buf.pop_back();
        } else if (charcode >= 0x20 && charcode < 0x7F) {
            w->title_edit_buf += static_cast<char>(charcode);
        }
        return;
    }

    if (w->on_key) w->on_key(keycode, charcode);
}

void WindowManager::draw_chrome(const Window& win) {
    const auto& f   = win.frame;
    const int   th  = win.chrome_title_h;
    const int   bd  = int(BORDER * dpr_);
    const uint32_t bd_col = win.focused ? 0xFF'89'B4'FA : 0xFF'2A'2D'3E;
    const uint32_t tb_col = win.focused ? 0xFF'11'1A'2C : 0xFF'0A'0E'18;

    g_rect(f.x, f.y, f.w, f.h, bd_col);
    g_rect(f.x + bd, f.y + bd, f.w - bd * 2, th - bd, tb_col);

    if (win.focused)
        g_line(f.x + bd, f.y + th, f.x + f.w - bd, f.y + th, 0xFF'45'85'C8);

    auto cr = win.content_rect();
    g_rect(cr.x, cr.y, cr.w, cr.h, CLR_BG_WIN);

    // While shrinking to close, hide title and window buttons immediately so they
    // do not float over the collapsing frame until the animation ends.
    if (!win.closing) {
        const int btn_cy = f.y + th / 2;
        const int r      = int(5 * dpr_);
        const int step   = int(12 * dpr_);
        g_circle(f.x + f.w - step,     btn_cy, r, CLR_CLOSE_BTN);
        g_circle(f.x + f.w - step * 2, btn_cy, r, CLR_MIN_BTN);
        g_circle(f.x + f.w - step * 3, btn_cy, r, CLR_MAX_BTN);

        const gpu::FontAtlas& tf  = gpu_.font(0);
        const float           tlh = tf.line_height();
        const float           tas = tf.ascent();
        const int title_y =
            f.y + static_cast<int>((th - tlh) * 0.5f + tas + 0.5f);
        const uint32_t title_col = win.focused ? 0xFF'C6'D0'F5 : 0xFF'4A'5A'70;
        if (win.title_editing) {
            const std::string display = win.title_edit_buf + "|";
            g_text(f.x + int(8 * dpr_), title_y, 0xFF'F0'D0'50, 0, display);
        } else {
            g_text(f.x + int(8 * dpr_), title_y, title_col, 0, win.title);
        }
    }
}

void WindowManager::render_all() {
    auto push_scissor = [this](const WinRect& cr) {
        gpu::CmdPacket p{};
        p.cmd    = gpu::Command::SET_SCISSOR;
        p.args[0] = static_cast<uint32_t>(cr.x);
        p.args[1] = static_cast<uint32_t>(cr.y);
        p.args[2] = static_cast<uint32_t>(cr.x + cr.w);
        p.args[3] = static_cast<uint32_t>(cr.y + cr.h);
        gpu_.push_command(p);
    };
    auto pop_scissor = [this]() {
        gpu::CmdPacket p{};
        p.cmd = gpu::Command::CLEAR_SCISSOR;
        gpu_.push_command(p);
    };

    for (auto& w : windows_)
        if ((w.visible || w.closing) && !w.minimized && !w.focused) {
            draw_chrome(w);
            if (w.on_render && w.visible &&
                (w.always_dirty || w.needs_redraw || w.frame != w.last_rendered_frame)) {
                push_scissor(w.content_rect());
                w.on_render(gpu_, w.content_rect());
                pop_scissor();
                w.needs_redraw       = false;
                w.last_rendered_frame = w.frame;
            }
        }
    for (auto& w : windows_)
        if ((w.visible || w.closing) && !w.minimized && w.focused) {
            draw_chrome(w);
            if (w.on_render && w.visible &&
                (w.always_dirty || w.needs_redraw || w.frame != w.last_rendered_frame)) {
                push_scissor(w.content_rect());
                w.on_render(gpu_, w.content_rect());
                pop_scissor();
                w.needs_redraw        = false;
                w.last_rendered_frame = w.frame;
            }
        }

    draw_launcher();
}

void WindowManager::draw_taskbar(uint32_t tick_count, uint64_t cpu_cycles, std::size_t ram_mib) {
    const int W  = screen_w_;
    const int TH = taskbar_h();
    const float dp = dpr_;
    auto sc = [dp](int n){ return int(n * dp + 0.5f); };

    const gpu::FontAtlas& f0   = gpu_.font(0);
    const float           lh   = f0.line_height();
    const float           asc  = f0.ascent();
    const int             ty   = static_cast<int>((TH - lh) * 0.5f + asc + 0.5f);

    g_text(sc(10), ty, 0xFF'89'B4'FA, 0, "JadeOS");

    int bx = sc(72);
    int win_idx = 1;
    for (const auto& w : windows_) {
        const bool active   = (w.id == focused_id_) && w.visible && !w.minimized;
        const bool minimized = w.minimized;
        const bool closed   = !w.visible && !w.closing;
        const bool animating = w.closing;
        const uint32_t dot_col = active    ? 0xFF'89'B4'FA
                               : animating ? 0xFF'88'44'44   // closing (dim red)
                               : closed    ? 0xFF'28'2C'3C   // closed (very dim)
                               : minimized ? 0xFF'31'32'44   // minimized (dim)
                               :             0xFF'45'55'6E;  // visible, unfocused
        const uint32_t bg = active     ? 0xFF'17'28'44
                          : closed     ? 0xFF'08'09'0E
                          :              0xFF'0C'10'1A;
        g_rect(bx, sc(5), sc(22), TH - sc(10), bg);
        g_line(bx,        sc(5),     bx + sc(21), sc(5),     dot_col);
        g_line(bx,        TH-sc(5),  bx + sc(21), TH-sc(5),  dot_col);
        g_line(bx,        sc(5),     bx,          TH-sc(5),  dot_col);
        g_line(bx+sc(21), sc(5),     bx + sc(21), TH-sc(5),  dot_col);
        char num[3];
        std::snprintf(num, sizeof(num), "%d", win_idx);
        g_text(bx + sc(7), ty, dot_col, 0, num);
        bx += sc(26);
        ++win_idx;
    }

    std::string centre_title;
    for (const auto& w : windows_)
        if (w.id == focused_id_ && w.visible && !w.minimized)
            centre_title = w.title;
    if (!centre_title.empty()) {
        const float tw = gpu_.font(0).measure_width(centre_title.c_str());
        const int tx = static_cast<int>((W - tw) / 2.f);
        g_rect(tx - sc(8), sc(5), static_cast<int>(tw) + sc(16), TH - sc(10), 0xFF'0F'1A'28);
        g_text(tx, ty, 0xFF'C6'D0'F5, 0, centre_title);
    }

    const uint32_t h24 = wall_hour_;
    const uint32_t m   = wall_min_;
    const uint32_t s   = wall_sec_;
    const uint32_t h12 = (h24 % 12 == 0) ? 12 : (h24 % 12);
    const char* ampm   = (h24 < 12) ? "AM" : "PM";
    char clk[16];
    std::snprintf(clk, sizeof(clk), "%u:%02u:%02u %s", h12, m, s, ampm);
    (void)tick_count;
    const float clk_w = gpu_.font(0).measure_width(clk);
    g_text(static_cast<int>(W - clk_w - sc(10)), ty, 0xFF'C6'D0'F5, 0, clk);

    (void)cpu_cycles;
    char stat[32];
    std::snprintf(stat, sizeof(stat), "ram:%zuM", ram_mib);
    const float stat_w = gpu_.font(0).measure_width(stat);
    g_text(static_cast<int>(W - clk_w - stat_w - sc(22)), ty, 0xFF'4A'6A'8A, 0, stat);
}

void WindowManager::set_wall_time(uint32_t unix_sec) noexcept {
    wall_unix_sec_ = unix_sec;
    const std::time_t tt = static_cast<std::time_t>(unix_sec);
    std::tm           tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    wall_hour_ = static_cast<uint8_t>(tm.tm_hour);
    wall_min_  = static_cast<uint8_t>(tm.tm_min);
    wall_sec_  = static_cast<uint8_t>(tm.tm_sec);
}

void WindowManager::tile_horizontal(int x, int y, int total_w, int total_h) {
    if (windows_.empty()) return;
    const int gap = 6;
    const int n   = static_cast<int>(windows_.size());
    const int each = (total_w - gap * (n + 1)) / n;
    int cx = x + gap;
    for (auto& w : windows_) {
        w.frame = { cx, y + gap, each, total_h - gap * 2 };
        cx += each + gap;
    }
}

}  // namespace os
