#include "fileexplorer.hpp"
#include <cstdio>
#include <string>
#include <algorithm>

namespace os::apps {

static std::string virtual_file_content(const std::string& path)
{
    if (path == "/home/jade/README.md") return
        "# JadeOS\n\nCustom RISC CPU + software GPU OS.\nRunning in your browser via WebAssembly.\n\nSee `projects.md` for full project list.";
    if (path == "/home/jade/about.txt") return
        "Systems programmer & CS student.\nBuilds OS kernels, CPU emulators, compilers,\ngraphics rasterizers  -  anything close to the metal.";
    if (path == "/home/jade/skills.txt") return
        "C++20/C  Rust  WebAssembly  GLSL/WGSL\nPython  TypeScript\n\nOS kernels  CPU emulation  Compilers\nGPU rasterizers  Lock-free data structures";
    if (path == "/home/jade/projects.md") return
        "# Projects\n\n- JadeOS: custom RISC CPU + GPU (this!)\n- RustVM: JIT-capable VM in Rust\n- NetFlow: 10Gbps packet analyser\n- ShaderForge: real-time WGSL editor\n- KernelKit: bare-metal AArch64 kernel";
    if (path == "/home/jade/contact.txt") return
        "Email:    jade@example.com\nGitHub:   github.com/jade\nSite:     jade.dev\nMatrix:   @jade:matrix.org";
    return "(binary or empty file)";
}

void render_fileexplorer(gpu::GPU& g, WinRect area,
                         const FileExplorerState& st, float dpr)
{
    auto sc = [dpr](int n) { return int(n * dpr + 0.5f); };
    const int x0 = area.x, y0 = area.y;

    g_rect(g, x0, y0, area.w, area.h, 0xFF'06'0E'1A);
    g_text(g, x0 + sc(10), y0 + sc(14), 0xFF'2A'90'CF, 0, "FILES.APP");
    g_line(g, x0 + sc(8), y0 + sc(20), x0 + area.w - sc(8), y0 + sc(20), 0xFF'1A'3A'55);

    // Path bar
    g_rect(g, x0 + sc(8), y0 + sc(24), area.w - sc(16), sc(18), 0xFF'0A'14'20);
    g_text(g, x0 + sc(14), y0 + sc(36), 0xFF'4A'8A'CF, 0, st.cur_path);

    const int list_y = y0 + sc(46);
    const int list_h = area.h - sc(46);

    g_scissor(g, x0, list_y, area.w, list_h);

    if (st.in_preview) {
        g_rect(g, x0, list_y, area.w, list_h, 0xFF'03'08'10);
        g_text(g, x0 + sc(10), list_y + sc(14), 0xFF'50'C8'FF, 0, "ESC or click [..] to go back");

        const int LINE_H = sc(16);
        int vy = list_y + sc(28) - st.scroll;
        // Split preview on newlines
        const std::string& pv = st.preview;
        std::string line;
        for (std::size_t i = 0; i <= pv.size(); ++i) {
            char c = (i < pv.size()) ? pv[i] : '\n';
            if (c == '\n') {
                if (vy >= list_y && vy < list_y + list_h)
                    g_text(g, x0 + sc(10), vy + sc(12), 0xFF'90'B8'D0, 0, line);
                line.clear();
                vy += LINE_H;
                if (vy > list_y + list_h + sc(20)) break;
            } else {
                line += c;
            }
        }
    } else {
        const int ENTRY_H = sc(20);

        // ".." parent entry
        const bool at_root = (st.cur_path == "/" || st.cur_path.rfind('/') == 0);
        int vy = list_y + sc(2) - st.scroll;

        // Parent row
        if (vy + ENTRY_H > list_y && vy < list_y + list_h) {
            uint32_t bg = (st.selected == -1) ? 0xFF'12'28'40 : 0xFF'03'08'10;
            g_rect(g, x0, vy, area.w, ENTRY_H, bg);
            g_text(g, x0 + sc(10), vy + sc(14), 0xFF'3A'6A'9A, 0, at_root ? "/" : "..");
        }
        vy += ENTRY_H;
    }

    g_scissor_clear(g);
}

bool click_fileexplorer(int lx, int ly, WinRect area,
                        FileExplorerState& st, const Filesystem& fs, float dpr)
{
    auto sc = [dpr](int n) { return int(n * dpr + 0.5f); };
    (void)lx;

    const int list_y = sc(46);
    if (ly < list_y) return false;

    if (st.in_preview) {
        st.in_preview = false;
        st.scroll     = 0;
        return true;
    }

    const int ENTRY_H = sc(20);
    const int idx_raw = (ly - list_y + st.scroll - sc(2)) / ENTRY_H;
    // idx_raw -1 = ".." parent, 0+ = actual entries
    if (idx_raw < 0) {
        // Click on parent (..)
        if (st.cur_path == "/") return false;
        auto pos = st.cur_path.rfind('/');
        st.cur_path = (pos == 0) ? "/" : st.cur_path.substr(0, pos);
        st.selected = 0; st.scroll = 0;
        return true;
    }

    auto entries = fs.readdir(st.cur_path);
    if (idx_raw >= int(entries.size())) return false;

    st.selected = idx_raw;
    const std::string& name = entries[idx_raw];
    const bool is_dir = (!name.empty() && name.back() == '/');

    if (is_dir) {
        // Navigate into directory
        std::string newpath = st.cur_path;
        if (newpath.back() != '/') newpath += '/';
        newpath += name.substr(0, name.size() - 1);
        st.cur_path = std::move(newpath);
        st.scroll   = 0; st.selected = 0;
    } else {
        // Open file preview
        const std::string full = [&]{
            std::string p = st.cur_path;
            if (p.back() != '/') p += '/';
            return p + name;
        }();
        // Try flat-file store first, then virtual content
        const FileEntry* fe = fs.find(full);
        if (fe) {
            st.preview = std::string(reinterpret_cast<const char*>(fe->data.data()), fe->data.size());
        } else {
            st.preview = virtual_file_content(full);
        }
        st.in_preview = true; st.scroll = 0;
    }
    return true;
}

bool scroll_fileexplorer(int delta, FileExplorerState& st, const Filesystem& /*fs*/)
{
    st.scroll = std::max(0, st.scroll + delta * 16);
    return true;
}

} // namespace os::apps
