#include "../os/kernel.hpp"
#include "../gpu/framebuffer.hpp"
#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <cstdint>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static os::Kernel g_kernel;

#ifdef __EMSCRIPTEN__

EM_JS(void, js_net_curl_start, (const char* url_ptr), {
    var url = UTF8ToString(url_ptr);
    (async function() {
        try {
            var resp = await fetch(url, { credentials: 'omit', cache: 'no-store' });
            var hdrLines = [];
            resp.headers.forEach(function(value, key) { hdrLines.push(key + ': ' + value); });
            var txt = await resp.text();
            if (txt.length > 400000)
                txt = txt.slice(0, 400000) + '\n...[truncated]\n';
            var out = 'HTTP/1.1 ' + resp.status + ' ' + resp.statusText + '\n'
                + hdrLines.join('\n') + '\n\n' + txt;
            ccall('wasm_net_text_to_terminal', null, ['string'], [out]);
        } catch (e) {
            ccall('wasm_net_text_to_terminal', null, ['string'],
                ['curl: error - ' + String(e) + '\n(cross-origin requests need CORS from the target server.)\n']);
        }
    })();
});

EM_JS(void, js_net_ping_start, (const char* host_ptr), {
    var host = UTF8ToString(host_ptr);
    (async function() {
        var body = 'PING ' + host + ': 56 data bytes\n';
        var times = [];
        var isLoop = (host === 'localhost' || host === '127.0.0.1' || host === '::1');
        if (isLoop) {
            for (var i = 0; i < 4; i++) {
                var t0 = performance.now();
                var s = 0;
                for (var j = 0; j < 50000; j++) s += j;
                var ms = performance.now() - t0;
                times.push(ms);
                body += '64 bytes from ' + host + ': icmp_seq=' + i
                    + ' ttl=64 time=' + ms.toFixed(3) + ' ms (synthetic)\n';
            }
        } else {
            var u = (host.indexOf('://') >= 0) ? host : ('https://' + host);
            for (var i = 0; i < 4; i++) {
                try {
                    var t0 = performance.now();
                    await fetch(u, { method: 'HEAD', mode: 'cors', cache: 'no-store' });
                    var ms = performance.now() - t0;
                    times.push(ms);
                    body += '64 bytes from ' + host + ': icmp_seq=' + i
                        + ' ttl=64 time=' + ms.toFixed(3) + ' ms\n';
                } catch (e) {
                    body += 'From ' + host + ' icmp_seq=' + i
                        + ' Destination Host Unreachable\n';
                }
            }
        }
        body += '--- ' + host + ' ping statistics ---\n';
        var rx = times.length;
        body += '4 packets transmitted, ' + rx + ' received, '
            + Math.round(100 * (4 - rx) / 4) + '% packet loss\n';
        if (times.length > 0) {
            var mn = Math.min.apply(null, times);
            var mx = Math.max.apply(null, times);
            var av = times.reduce(function(a, b) { return a + b; }, 0) / times.length;
            body += 'rtt min/avg/max = ' + mn.toFixed(3) + '/' + av.toFixed(3) + '/' + mx.toFixed(3) + ' ms\n';
        } else {
            body += 'rtt min/avg/max = 0/0/0 ms\n';
        }
        ccall('wasm_net_text_to_terminal', null, ['string'], [body]);
    })();
});

extern "C" {

EMSCRIPTEN_KEEPALIVE void wasm_net_text_to_terminal(const char* text) {
    if (!text) return;
    g_kernel.terminal().print(std::string(text));
    g_kernel.mark_terminal_dirty();
}

EMSCRIPTEN_KEEPALIVE void wasm_net_request_curl(const char* url) {
    if (!url) return;
    js_net_curl_start(url);
}

EMSCRIPTEN_KEEPALIVE void wasm_net_request_ping(const char* host) {
    if (!host) return;
    js_net_ping_start(host);
}

} // extern "C"

#endif

// JS fills this via HEAPU8.set(bytes, allocFontBuffer(len)) then calls
// commitFontBuffer() before boot().
static std::vector<uint8_t> g_font_staging;

// Allocate (or reuse) a C++ buffer of 'size' bytes and return its address in
// WASM linear memory so JS can write into it via HEAPU8.set().
uint32_t wasm_alloc_font_buffer(uint32_t size) {
    g_font_staging.resize(size);
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_font_staging.data()));
}

// Transfer the staged bytes into the GPU font atlas, scaled for the given DPR.
void wasm_commit_font_buffer(float dpr) {
    if (!g_font_staging.empty())
        g_kernel.gpu().load_font(
            std::span<const uint8_t>(g_font_staging.data(), g_font_staging.size()), dpr);
    g_font_staging.clear();
    g_font_staging.shrink_to_fit();
}


void wasm_boot(uint32_t mem_mib, uint32_t fb_w, uint32_t fb_h, float dpr) {
    // Font is loaded separately via allocFontBuffer/commitFontBuffer before boot.
    g_kernel.boot(mem_mib, {}, fb_w ? fb_w : gpu::FB_W_DEFAULT,
                                   fb_h ? fb_h : gpu::FB_H_DEFAULT,
                                   dpr > 0.0f ? dpr : 1.0f);
}

void wasm_tick(uint32_t cycles_per_tick) {
    g_kernel.tick(cycles_per_tick);
}

emscripten::val wasm_get_framebuffer() {
    const uint8_t* ptr = g_kernel.gpu().framebuffer_data();
    const std::size_t bytes = g_kernel.gpu().fb_bytes();
    return emscripten::val(emscripten::typed_memory_view(bytes, ptr));
}

uint32_t wasm_framebuffer_width()  { return g_kernel.gpu().framebuffer_width(); }
uint32_t wasm_framebuffer_height() { return g_kernel.gpu().framebuffer_height(); }
bool     wasm_is_running()         { return g_kernel.is_running(); }

emscripten::val wasm_get_cpu_state() {
    const auto& r = g_kernel.cpu().regs();
    auto obj = emscripten::val::object();
    for (uint8_t i = 0; i < cpu::NUM_REGS; ++i) {
        auto reg = static_cast<cpu::Reg>(i);
        obj.set(std::string(r.reg_name(reg)), r.get(reg));
    }
    obj.set("pc",    r.pc);
    obj.set("sp",    r.sp);
    obj.set("flags", r.flags);
    obj.set("cycles", g_kernel.cpu().total_cycles());
    return obj;
}

emscripten::val wasm_get_gpu_state() {
    auto obj = emscripten::val::object();
    obj.set("drawCalls", g_kernel.gpu().draw_calls());
    return obj;
}

void wasm_gpu_push(uint32_t cmd,
                   uint32_t a0, uint32_t a1, uint32_t a2,
                   uint32_t a3, uint32_t a4, uint32_t a5) {
    gpu::CmdPacket pkt;
    pkt.cmd    = static_cast<gpu::Command>(cmd);
    pkt.args   = { a0, a1, a2, a3, a4, a5 };
    g_kernel.gpu().push_command(pkt);
    g_kernel.gpu().flush();
}

// Mouse events (framebuffer pixel coords).
void wasm_send_mousedown(uint32_t x, uint32_t y) { g_kernel.send_mousedown(x, y); }
void wasm_send_mousemove(uint32_t x, uint32_t y) { g_kernel.send_mousemove(x, y); }
void wasm_send_mouseup()                          { g_kernel.send_mouseup(); }
void wasm_send_scroll(int delta)                  { g_kernel.send_scroll(delta); }

// Forward keyboard input to the focused window.
void wasm_send_key(uint32_t keycode, uint32_t charcode) {
    g_kernel.send_key(keycode, charcode);
}

// Set real wall-clock time for the taskbar clock.
void wasm_set_wall_clock(uint32_t unix_sec) {
    g_kernel.set_wall_time(unix_sec);
}

// Resize the framebuffer (call when the browser window is resized).
void wasm_resize(uint32_t fb_w, uint32_t fb_h) {
    g_kernel.resize(fb_w, fb_h);
}


// Enable WebGPU mode: flush() serialises commands instead of rasterising.
void wasm_set_webgpu_mode(bool enable) {
    g_kernel.gpu().set_webgpu_mode(enable);
}

// Returns Uint32Array view into the serialised command buffer.
// Valid until the next tick() call.  Each record is 8 x uint32_t (32 bytes).
//   record[0] = type(u8)|size_id(u8)<<8|str_len(u16)<<16
//   record[1..4] = x0,y0,x1/w,y1/h as float bits
//   record[5] = packed color 0xAARRGGBB
//   record[6] = string pool byte offset (TEXT cmds)
//   record[7] = flags (0x01 = alpha-blend)
emscripten::val wasm_get_cmd_buf() {
    const uint32_t* p = g_kernel.gpu().cmd_buf_ptr();
    const uint32_t  n = g_kernel.gpu().cmd_buf_len();
    if (!p || !n) return emscripten::val::null();
    return emscripten::val(emscripten::typed_memory_view(n, p));
}

// Returns Uint8Array view into the string pool for TEXT commands.
emscripten::val wasm_get_str_pool() {
    const uint8_t* p = g_kernel.gpu().str_pool_ptr();
    const uint32_t n = g_kernel.gpu().str_pool_len();
    if (!p || !n) return emscripten::val::null();
    return emscripten::val(emscripten::typed_memory_view(n, p));
}

// Returns Uint8Array view into the greyscale font atlas bitmap (R8, A8).
// size_id: 0=14px  1=20px  2=32px (all scaled by DPR).
emscripten::val wasm_get_atlas_bitmap(uint8_t size_id) {
    const auto& f = g_kernel.gpu().font(size_id);
    const auto& px = f.atlas_pixels();
    if (px.empty()) return emscripten::val::null();
    return emscripten::val(emscripten::typed_memory_view(px.size(), px.data()));
}

// Returns a JS object with atlas dimensions, line height, and glyph metrics.
// glyphMetrics is a Float32Array: 9 floats x 96 glyphs (ASCII 0x20..0x7F).
//   [0]=xoff [1]=yoff [2]=xoff2 [3]=yoff2 [4]=advance_x
//   [5]=atlas_x0 [6]=atlas_y0 [7]=atlas_x1 [8]=atlas_y1
emscripten::val wasm_get_atlas_info(uint8_t size_id) {
    const auto& f = g_kernel.gpu().font(size_id);
    auto obj = emscripten::val::object();
    obj.set("width",      f.atlas_width());
    obj.set("height",     f.atlas_height());
    obj.set("lineHeight", f.line_height());
    obj.set("ascent",     f.ascent());
    obj.set("loaded",     f.is_loaded());

    const float*    mp = g_kernel.gpu().glyph_metrics_ptr(size_id);
    const uint32_t  mn = g_kernel.gpu().glyph_metrics_len(size_id);
    if (mp && mn)
        obj.set("glyphMetrics", emscripten::val(emscripten::typed_memory_view(mn, mp)));
    else
        obj.set("glyphMetrics", emscripten::val::null());
    return obj;
}

// heh spaghetti bindings
EMSCRIPTEN_BINDINGS(jadeportfolio) {
    emscripten::function("boot",               &wasm_boot);
    emscripten::function("tick",               &wasm_tick);
    emscripten::function("getFramebuffer",     &wasm_get_framebuffer);
    emscripten::function("framebufferWidth",   &wasm_framebuffer_width);
    emscripten::function("framebufferHeight",  &wasm_framebuffer_height);
    emscripten::function("isRunning",          &wasm_is_running);
    emscripten::function("getCpuState",        &wasm_get_cpu_state);
    emscripten::function("getGpuState",        &wasm_get_gpu_state);
    emscripten::function("gpuPush",            &wasm_gpu_push);
    emscripten::function("sendMouseDown",      &wasm_send_mousedown);
    emscripten::function("sendMouseMove",      &wasm_send_mousemove);
    emscripten::function("sendMouseUp",        &wasm_send_mouseup);
    emscripten::function("sendScroll",         &wasm_send_scroll);
    emscripten::function("sendKey",            &wasm_send_key);
    emscripten::function("allocFontBuffer",    &wasm_alloc_font_buffer);
    emscripten::function("commitFontBuffer",   &wasm_commit_font_buffer);
    emscripten::function("setWallClock",       &wasm_set_wall_clock);
    emscripten::function("resize",             &wasm_resize);
    emscripten::function("setWebGpuMode",      &wasm_set_webgpu_mode);
    emscripten::function("getCmdBuf",          &wasm_get_cmd_buf);
    emscripten::function("getStrPool",         &wasm_get_str_pool);
    emscripten::function("getAtlasBitmap",     &wasm_get_atlas_bitmap);
    emscripten::function("getAtlasInfo",       &wasm_get_atlas_info);
}
