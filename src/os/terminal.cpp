#include "terminal.hpp"
#include "scheduler.hpp"
#include "filesystem.hpp"
#include "wm.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <sstream>
#include <vector>
#if defined(__EMSCRIPTEN__)
extern "C" {
void wasm_net_request_curl(const char* url);
void wasm_net_request_ping(const char* host);
}
#else
#include <array>
#endif

namespace os {

static constexpr uint32_t TC_BG        = 0xFF'03'08'10;
static constexpr uint32_t TC_TEXT      = 0xFF'B8'D8'F0;
static constexpr uint32_t TC_PROMPT    = 0xFF'28'B0'60;
static constexpr uint32_t TC_CMD       = 0xFF'E8'E8'A8;
static constexpr uint32_t TC_HEADING   = 0xFF'50'C8'FF;
static constexpr uint32_t TC_ACCENT    = 0xFF'F0'A0'30;
static constexpr uint32_t TC_SEP       = 0xFF'0D'25'40;
static constexpr uint32_t TC_CURSOR    = 0xFF'60'D0'FF;
static constexpr uint32_t TC_DIM       = 0xFF'40'60'80;

static void t_rect(gpu::GPU& g, int x, int y, int w, int h, uint32_t col) {
    gpu::CmdPacket p;
    p.cmd     = gpu::Command::DRAW_RECT;
    p.args[0] = static_cast<uint32_t>(x);
    p.args[1] = static_cast<uint32_t>(y);
    p.args[2] = static_cast<uint32_t>(w);
    p.args[3] = static_cast<uint32_t>(h);
    p.args[4] = col;
    g.push_command(p);
}

static void t_line(gpu::GPU& g, int x0, int y0, int x1, int y1, uint32_t col) {
    gpu::CmdPacket p;
    p.cmd     = gpu::Command::DRAW_LINE;
    p.args[0] = static_cast<uint32_t>(x0);
    p.args[1] = static_cast<uint32_t>(y0);
    p.args[2] = static_cast<uint32_t>(x1);
    p.args[3] = static_cast<uint32_t>(y1);
    p.args[4] = col;
    g.push_command(p);
}

static void t_text(gpu::GPU& g, int x, int y, uint32_t col, const std::string& s) {
    if (s.empty()) return;
    gpu::TextRequest r;
    r.x = x; r.y = y;
    r.colour = gpu::RGBA::from_u32(col);
    r.font_size_id = 0;
    r.text = s;
    g.draw_text(r);
}

static const char* term_cwd() { return "/home/jade"; }

static std::string trim_sp(const std::string& s) {
    std::string p = s;
    while (!p.empty() && p.front() == ' ') p.erase(p.begin());
    while (!p.empty() && p.back()  == ' ') p.pop_back();
    return p;
}

static std::string resolve_vpath(std::string raw) {
    std::string p = trim_sp(raw);
    if (p.empty()) return p;
    if (p == "~") return term_cwd();
    if (p.size() >= 2 && p[0] == '~' && p[1] == '/')
        return std::string("/home/jade") + p.substr(1);
    if (p[0] == '/') return p;
    if (p.size() >= 2 && p[0] == '.' && p[1] == '/')
        return std::string(term_cwd()) + "/" + p.substr(2);
    return std::string(term_cwd()) + "/" + p;
}

static double sim_seconds(const uint32_t* sim_tick) noexcept {
    if (!sim_tick) return 0.0;
    return static_cast<double>(*sim_tick) / 120.0;
}

struct MemLayout {
    std::size_t total_kb;
    std::size_t free_kb;
    std::size_t avail_kb;
    std::size_t used_kb;
};

static MemLayout mem_layout(std::size_t mem_bytes) noexcept {
    const std::size_t total_kb = mem_bytes / 1024;
    const std::size_t free_kb  = total_kb / 4;
    const std::size_t avail_kb = free_kb + total_kb / 8;
    const std::size_t used_kb  = (total_kb > avail_kb) ? (total_kb - avail_kb) : 0;
    return { total_kb, free_kb, avail_kb, used_kb };
}

static std::string first_line(const std::string& s) {
    const auto n = s.find('\n');
    return (n == std::string::npos) ? s : s.substr(0, n);
}

#if defined(__EMSCRIPTEN__)
static std::string normalize_http_url(std::string u) {
    u = trim_sp(u);
    if (u.empty() || u[0] == '/') return u;
    if (u.find("://") == std::string::npos) return "https://" + u;
    return u;
}
#endif

#ifndef __EMSCRIPTEN__
static std::string popen_read_all(const char* cmd) {
    std::unique_ptr<FILE, int(*)(FILE*)> f(popen(cmd, "r"), &pclose);
    if (!f) return {};
    std::string out;
    std::array<char, 4096> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), f.get()))
        out += buf.data();
    return out;
}

static bool safe_url_token(const std::string& u) noexcept {
    if (u.empty() || u.size() > 900) return false;
    for (unsigned char c : u) {
        if (std::isalnum(c)) continue;
        if (c == '-' || c == '_' || c == '.' || c == ':' || c == '/' ||
            c == '?' || c == '#' || c == '[' || c == ']' || c == '@' ||
            c == '!' || c == '$' || c == '&' || c == '\'' || c == '(' ||
            c == ')' || c == '*' || c == '+' || c == ',' || c == ';' ||
            c == '=' || c == '%') continue;
        return false;
    }
    return true;
}
#endif

Terminal::Terminal() {
}

void Terminal::configure(const Scheduler* sched, Filesystem* fs,
                         std::size_t mem_bytes, float dpr,
                         const WindowManager* wm,
                         const uint32_t* sim_tick)
{
    sched_     = sched;
    fs_        = fs;
    mem_bytes_ = mem_bytes;
    dpr_       = (dpr > 0.0f) ? dpr : 1.0f;
    wm_        = wm;
    sim_tick_  = sim_tick;
}

void Terminal::print(const std::string& s) {
    std::istringstream ss(s);
    std::string ln;
    while (std::getline(ss, ln)) {
        const int slot = (ring_head_ + ring_count_) % MAX_LINES;
        lines_ring_[static_cast<std::size_t>(slot)] = std::move(ln);
        if (ring_count_ < MAX_LINES) {
            ++ring_count_;
        } else {
            
            ring_head_ = (ring_head_ + 1) % MAX_LINES;
        }
    }
}

void Terminal::print_lines(std::initializer_list<const char*> ls) {
    for (auto* l : ls) print(l);
}

void Terminal::execute(const std::string& raw) {
    std::string cmd = raw;
    while (!cmd.empty() && cmd.front() == ' ') cmd.erase(cmd.begin());
    while (!cmd.empty() && cmd.back()  == ' ') cmd.pop_back();

    if (!cmd.empty()) {
        if (history_.empty() || history_.back() != cmd)
            history_.push_back(cmd);
        if (history_.size() > 200) history_.erase(history_.begin());
    }
    hist_idx_ = -1;

    print("jade@jadeos:~$ " + cmd);

    std::string verb, arg;
    {
        const auto sp = cmd.find(' ');
        if (sp == std::string::npos) { verb = cmd; }
        else { verb = cmd.substr(0, sp); arg = cmd.substr(sp + 1); }
        while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
        while (!arg.empty() && arg.back()  == ' ') arg.pop_back();
    }

    if (verb.empty())          { /* blank line */ }
    else if (verb == "help")   cmd_help();
    else if (verb == "about")  cmd_about();
    else if (verb == "skills") cmd_skills();
    else if (verb == "projects") cmd_projects();
    else if (verb == "contact") cmd_contact();
    else if (verb == "ls")     cmd_ls(arg);
    else if (verb == "cat")    cmd_cat(arg);
    else if (verb == "uname") {
        // uname and uname -a
        cmd_uname(arg == "-a" || arg == "-r");
    }
    else if (verb == "whoami") cmd_whoami();
    else if (verb == "ps") {
        cmd_ps(arg == "aux" || arg == "-aux" || arg == "-A");
    }
    else if (verb == "top")    cmd_top();
    else if (verb == "free")   cmd_free(arg == "-h" || arg == "-human");
    else if (verb == "df")     cmd_df(arg == "-h" || arg == "-human");
    else if (verb == "uptime") cmd_uptime();
    else if (verb == "neofetch") cmd_neofetch();
    else if (verb == "curl")   cmd_curl(arg);
    else if (verb == "wget")   cmd_curl(arg);
    else if (verb == "ping")   cmd_ping(arg);
    else if (verb == "nano" || verb == "vi" || verb == "vim") {
        if (arg.empty()) { print("Usage: nano <path>"); }
        else {
            const std::string path = resolve_vpath(arg);
            std::string content;
            if (fs_) content = fs_->read_text(path);
            nano_path_     = path;
            nano_buf_      = content;
            nano_line_     = 0;
            nano_col_      = 0;
            nano_scroll_   = 0;
            nano_modified_ = false;
            nano_mode_     = true;
            return;
        }
    }
    else if (verb == "clear")  { ring_head_ = 0; ring_count_ = 0; }
    else if (verb == "pwd")    print("/home/jade");
    else if (verb == "hostname") print("jadeos");
    else if (verb == "id")     print("uid=1000(jade) gid=1000(jade) groups=1000(jade),4(adm),24(cdrom)");
    else if (verb == "echo")   print(arg);
    else if (verb == "date") {
        std::time_t now = std::time(nullptr);
        if (wm_) {
            const uint32_t w = wm_->wall_unix_sec();
            if (w != 0) now = static_cast<std::time_t>(w);
        }
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S %Z %Y", &tm);
        print(buf);
    }
    else if (verb == "exit" || verb == "logout") {
        print("logout");
        print("");
        print("JadeOS session ended - refresh to restart.");
    } else {
        print("bash: " + verb + ": command not found");
        print("(type 'help' for available commands)");
    }
    print("");
}

void Terminal::cmd_help() {
    print_lines({
        "JadeOS command reference",
        "────────────────────────────────────────",
        "  about       -  who I am",
        "  skills      -  technical skills breakdown",
        "  projects    -  portfolio project list",
        "  contact     -  contact details",
        "",
        "  ls [dir]    -  list directory",
        "  cat <path>  -  print file (incl. /proc/*)",
        "  pwd         -  print working directory",
        "",
        "  ps [aux]    -  process list",
        "  top         -  live process snapshot",
        "  free [-h]   -  memory usage",
        "  df [-h]     -  disk usage",
        "  uptime      -  system uptime",
        "  uname [-a]  -  kernel info",
        "  neofetch    -  system summary",
        "",
        "  curl <url>  -  HTTP GET (WASM sync XHR; native uses curl(1))",
        "  ping <host> -  RTT via HTTP HEAD (WASM) or system ping (native)",
        "  echo <str>  -  echo string",
        "  date        -  current date",
        "  clear       -  clear scrollback",
        "  exit        -  end session",
    });
}

void Terminal::cmd_about() {
    if (fs_) {
        const std::string t = fs_->read_text("/home/jade/about.txt");
        if (!t.empty()) { print(t); return; }
    }
    print("about.txt missing");
}

void Terminal::cmd_skills() {
    if (fs_) {
        const std::string t = fs_->read_text("/home/jade/skills.txt");
        if (!t.empty()) { print(t); return; }
    }
    print("skills.txt missing");
}

void Terminal::cmd_projects() {
    if (fs_) {
        const std::string t = fs_->read_text("/home/jade/projects.md");
        if (!t.empty()) { print(t); return; }
    }
    print("projects.md missing");
}

void Terminal::cmd_contact() {
    if (fs_) {
        const std::string t = fs_->read_text("/home/jade/contact.txt");
        if (!t.empty()) { print(t); return; }
    }
    print("contact.txt missing");
}

void Terminal::cmd_ls(const std::string& arg) {
    const std::string path = resolve_vpath(arg.empty() || arg == "." ? term_cwd() : arg);

    if (fs_) {
        const auto entries = fs_->readdir(path);
        if (!entries.empty()) {
            std::string line;
            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (i > 0) line += "  ";
                line += entries[i];
                if (line.size() > 72) { print(line); line.clear(); }
            }
            if (!line.empty()) print(line);
            return;
        }
        if (fs_->lookup(path) != 0) {
            print("ls: '" + arg + "': Not a directory");
            return;
        }
    }

    print("ls: cannot access '" + arg + "': No such file or directory");
}

void Terminal::cmd_cat(const std::string& arg) {
    if (arg.empty()) { print("cat: missing file operand"); return; }

    const std::string path = resolve_vpath(arg);
    const std::size_t mem  = mem_bytes_ ? mem_bytes_ : 8u * 1024u * 1024u;
    const uint64_t    ut   = static_cast<uint64_t>(sys_tick_);

    if (fs_ && sched_) {
        if (path.rfind("/proc", 0) == 0) {
            const std::string v = fs_->virtual_read(path, *sched_, mem, ut);
            if (!v.empty()) {
                print(v);
                return;
            }
            print("cat: " + path + ": No such file or directory");
            return;
        }
        const std::string t = fs_->read_text(path);
        if (!t.empty()) {
            print(t);
            return;
        }
    }

    print("cat: " + arg + ": No such file or directory");
}

void Terminal::cmd_uname(bool full) {
    if (fs_ && sched_) {
        const std::size_t mem = mem_bytes_ ? mem_bytes_ : 8u * 1024u * 1024u;
        const std::string v = fs_->virtual_read("/proc/version", *sched_, mem,
                                                  static_cast<uint64_t>(sys_tick_));
        if (!v.empty()) {
            if (full) {
                std::string line = first_line(v);
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
                print(line + " GNU/Linux");
            } else {
                print("Linux");
            }
            return;
        }
    }
    if (full) {
        print("Linux jadeos 6.6.0-jadeos #1 SMP PREEMPT_DYNAMIC "
              "JadeISA (WebAssembly / Emscripten) GNU/Linux");
    } else {
        print("Linux");
    }
}

void Terminal::cmd_whoami() { print("jade"); }

void Terminal::cmd_ps(bool aux) {
    if (aux) {
        print("USER       PID  PPID  %CPU  %MEM  VSZ    RSS    STAT  COMMAND");
    } else {
        print("  PID TTY          TIME CMD");
    }

    if (!sched_) {
        if (aux) {
            print("jade         0     0   0.0   0.1   4096   4096  Ss    [kernel]");
            print("jade         1     0   0.0   0.2   8192   8192  Ss    /sbin/init");
            print("jade         2     1   0.1   0.4  16384  16384  Sl    jade-wm");
            print("jade         3     2   0.0   0.1   4096   4096  S+    terminal");
            print("jade         4     2   0.0   0.1   2048   2048  Sl    portfolio");
        } else {
            print("    0 ?        00:00:00 [kernel]");
            print("    1 ?        00:00:01 systemd");
            print("    2 ?        00:00:00 jade-wm");
            print("    3 pts/0    00:00:00 terminal");
            print("    4 pts/0    00:00:00 portfolio");
        }
        return;
    }

    const auto entries = sched_->ps();
    uint64_t cpu_sum = 0;
    for (const auto& e : entries) cpu_sum += e.cpu_ticks;
    char buf[256];
    for (const auto& e : entries) {
        const double cpu_pct = (cpu_sum > 0)
            ? (100.0 * static_cast<double>(e.cpu_ticks) / static_cast<double>(cpu_sum))
            : 0.0;
        const double mem_pct = mem_bytes_
            ? (static_cast<double>(e.mem_kb) * 1024.0 / static_cast<double>(mem_bytes_) * 100.0)
            : 0.0;
        if (aux) {
            std::snprintf(buf, sizeof(buf),
                "jade   %5u %5u   %4.1f  %4.1f  %6u %6u  %-4s  %s",
                e.pid, e.ppid, cpu_pct, mem_pct,
                e.mem_kb, e.mem_kb,
                (e.state == ProcessState::Running ? "R" :
                 e.state == ProcessState::Blocked  ? "S" : "Z"),
                e.cmd.empty() ? e.name.c_str() : e.cmd.c_str());
        } else {
            std::snprintf(buf, sizeof(buf),
                "%5u pts/0   00:00:%02u %s",
                e.pid, static_cast<unsigned>(e.cpu_ticks % 60),
                e.name.c_str());
        }
        print(buf);
    }
}

void Terminal::cmd_top() {
    char buf[256];
    const MemLayout ml = mem_layout(mem_bytes_);
    std::string load = "0.00 0.00 0.00";
    if (fs_ && sched_) {
        const std::string la = fs_->virtual_read("/proc/loadavg", *sched_, mem_bytes_,
                                                  static_cast<uint64_t>(sys_tick_));
        if (!la.empty()) load = first_line(la);
    }
    std::snprintf(buf, sizeof(buf),
        "top - JadeOS  Tasks: %zu total  Load: %s",
        sched_ ? sched_->process_count() : 0u, load.c_str());
    print(buf);
    std::snprintf(buf, sizeof(buf),
        "Mem: %zu kB total  %zu kB used  %zu kB free",
        ml.total_kb, ml.used_kb, ml.free_kb);
    print(buf);
    print("  PID  USER       %CPU  %MEM  COMMAND");
    cmd_ps(true);
}

void Terminal::cmd_free(bool human) {
    const MemLayout ml = mem_layout(mem_bytes_);
    char buf[256];
    if (human) {
        const double total_mib = ml.total_kb / 1024.0;
        const double used_mib  = ml.used_kb  / 1024.0;
        const double free_mib  = ml.free_kb  / 1024.0;
        const double bc_mib    = static_cast<double>(ml.total_kb - ml.avail_kb) / 1024.0;
        print("              total        used        free      buff/cache");
        std::snprintf(buf, sizeof(buf),
            "Mem:          %5.1fMi    %5.1fMi    %5.1fMi    %5.1fMi",
            total_mib, used_mib, free_mib, bc_mib);
        print(buf);
        print("Swap:             0          0          0");
    } else {
        const std::size_t bc_kb = (ml.total_kb > ml.avail_kb) ? (ml.total_kb - ml.avail_kb) : 0;
        print("              total        used        free      shared  buff/cache   available");
        std::snprintf(buf, sizeof(buf),
            "Mem:      %8zu    %8zu    %8zu         0    %8zu    %8zu",
            ml.total_kb, ml.used_kb, ml.free_kb, bc_kb, ml.avail_kb);
        print(buf);
        print("Swap:            0           0           0");
    }
}

void Terminal::cmd_df(bool human) {
    if (!fs_) {
        print("df: no filesystem");
        return;
    }
    const std::uint64_t used_kb = fs_->jadefs_used_kb();
    const std::uint64_t total_kb = std::max<std::uint64_t>(used_kb + 4096,
                                   std::max<std::uint64_t>(8192,
                                       (used_kb * 11 + 9) / 10));
    const std::uint64_t avail_kb = (total_kb > used_kb) ? (total_kb - used_kb) : 0;
    const unsigned use_pct = (total_kb > 0)
        ? static_cast<unsigned>((100ull * used_kb + total_kb / 2) / total_kb) : 0;
    const std::size_t tmp_total = mem_bytes_ / 2048;
    print("Filesystem       Size  Used Avail Use% Mounted on");
    char buf[160];
    if (human) {
        std::snprintf(buf, sizeof(buf),
            "jadefs           %4lluM  %4lluM  %4lluM  %3u%% /",
            static_cast<unsigned long long>((total_kb + 1023) / 1024),
            static_cast<unsigned long long>((used_kb + 1023) / 1024),
            static_cast<unsigned long long>((avail_kb + 1023) / 1024),
            use_pct);
        print(buf);
        std::snprintf(buf, sizeof(buf),
            "tmpfs            %4zuM     0  %4zuM   0%% /tmp",
            (tmp_total + 1023) / 1024, (tmp_total + 1023) / 1024);
        print(buf);
        print("procfs              0     0     0   -  /proc");
    } else {
        std::snprintf(buf, sizeof(buf),
            "jadefs        %7llu %7llu %7llu  %3u%% /",
            static_cast<unsigned long long>(total_kb),
            static_cast<unsigned long long>(used_kb),
            static_cast<unsigned long long>(avail_kb),
            use_pct);
        print(buf);
        std::snprintf(buf, sizeof(buf),
            "tmpfs         %7zu     0 %7zu   0%% /tmp", tmp_total, tmp_total);
        print(buf);
        print("procfs            0       0       0   -  /proc");
    }
}

void Terminal::cmd_uptime() {
    const double up = sim_seconds(sim_tick_);
    const uint64_t up_sec = static_cast<uint64_t>(up + 0.5);
    const uint64_t days = up_sec / 86400ull;
    const uint32_t rem = static_cast<uint32_t>(up_sec % 86400ull);
    const uint32_t hrs = rem / 3600u;
    const uint32_t mins = (rem % 3600u) / 60u;
    char clk[16] = "";
    if (wm_ && wm_->wall_unix_sec() != 0)
        std::snprintf(clk, sizeof(clk), "%02u:%02u:%02u",
            static_cast<unsigned>(wm_->wall_hour()),
            static_cast<unsigned>(wm_->wall_min()),
            static_cast<unsigned>(wm_->wall_sec()));
    char buf[192];
    std::string load = "0.00 0.00 0.00";
    if (fs_ && sched_) {
        const std::string la = fs_->virtual_read("/proc/loadavg", *sched_, mem_bytes_,
                                                  static_cast<uint64_t>(sys_tick_));
        if (!la.empty()) {
            char a[16]{}, b[16]{}, c[16]{};
            if (std::sscanf(la.c_str(), "%15s %15s %15s", a, b, c) == 3)
                load = std::string(a) + " " + b + " " + c;
        }
    }
    if (clk[0]) {
        if (days > 0)
            std::snprintf(buf, sizeof(buf),
                "%s up %llu days, %u:%02u,  1 user,  load average: %s",
                clk, static_cast<unsigned long long>(days), hrs, mins, load.c_str());
        else if (hrs > 0)
            std::snprintf(buf, sizeof(buf),
                "%s up %u:%02u,  1 user,  load average: %s",
                clk, hrs, mins, load.c_str());
        else
            std::snprintf(buf, sizeof(buf),
                "%s up %u min,  1 user,  load average: %s",
                clk, mins, load.c_str());
    } else {
        std::snprintf(buf, sizeof(buf),
            "up %.0f s,  1 user,  load average: %s", up, load.c_str());
    }
    print(buf);
}

void Terminal::cmd_neofetch() {
    const std::size_t mem_mib = mem_bytes_ / (1024 * 1024);
    const double up = sim_seconds(sim_tick_);
    char buf[256];
    print_lines({
        "      .....          jade@jadeos",
        "    .+:::::+.        ───────────────────────────",
        "   +::: OS ::+       OS: JadeOS 1.0.0",
        "  +:: Kernel :+      Kernel: Custom RISC 32-bit ISA",
    });
    std::snprintf(buf, sizeof(buf),
        "  +:  Jade  :+       Uptime: %.1f s (sim)", up);
    print(buf);
    print_lines({
        "  +:::::::::::+      Shell: /bin/bash",
        "   ++++++++++++      WM: jade-wm (tiling + drag)",
        "    ..........       GPU: JadeGPU (software rasteriser)",
        "                     Fonts: Hack TrueType (stb_truetype)",
    });
    std::snprintf(buf, sizeof(buf),
        "                     Memory: %zu MiB total", mem_mib);
    print(buf);
#ifdef __EMSCRIPTEN__
    print("                     Backend: C++20 -> Emscripten -> WASM");
#else
    print("                     Backend: native smoke build");
#endif
}

void Terminal::cmd_curl(const std::string& arg) {
    const std::string url = trim_sp(arg);
    if (url.empty()) {
        print("curl: missing URL");
        return;
    }
    if (url[0] == '/') {
        if (!fs_) { print("curl: no filesystem"); return; }
        const std::string body = fs_->read_text(url);
        if (body.empty()) {
            print("curl: failed to read file");
            return;
        }
        print(body);
        return;
    }
#ifdef __EMSCRIPTEN__
    const std::string target = normalize_http_url(url);
    wasm_net_request_curl(target.c_str());
    print("(response appears when the fetch completes)");
#else
    if (!safe_url_token(url)) {
        print("curl: unsupported characters in URL");
        return;
    }
    const std::string cmd = std::string("curl -sS -m 12 -- ") + url;
    const std::string out = popen_read_all(cmd.c_str());
    if (out.empty()) print("curl: empty response or curl(1) unavailable");
    else print(out);
#endif
}

void Terminal::cmd_ping(const std::string& arg) {
    const std::string host = trim_sp(arg.empty() ? "localhost" : arg);
    char buf[256];
#ifndef __EMSCRIPTEN__
    std::snprintf(buf, sizeof(buf), "PING %s: 56 data bytes", host.c_str());
    print(buf);
#endif
#ifdef __EMSCRIPTEN__
    wasm_net_request_ping(host.c_str());
    print("(ping output appears when requests complete)");
#else
    if (!safe_url_token(host)) {
        print("ping: invalid host");
        return;
    }
    const std::string cmd = std::string("ping -c 4 -W 2 ") + host;
    const std::string out = popen_read_all(cmd.c_str());
    if (out.empty()) print("ping: no response (is ping(8) installed?)");
    else print(out);
#endif
}

void Terminal::send_key(uint32_t keycode, uint32_t charcode) {
    // JS sends keyup too (bit 20 clear); shell/nano only react to keydown.
    if (((keycode >> 20) & 1u) == 0) return;

    blink_on_   = true;
    blink_tick_ = 0;

    if (nano_mode_) {
        // Ctrl+X  (keycode 88 with ctrl bit set, or raw charcode 24)
        const bool ctrl_held = (keycode >> 17) & 1;
        const uint32_t k     = keycode & 0xFFFF;
        const bool is_ctrl_x = (ctrl_held && (k == 88 || k == 120)) ||
                                (charcode == 24);
        if (is_ctrl_x) {
            // Save file via flat-file store.
            if (fs_) fs_->write_file(nano_path_, nano_buf_);
            nano_mode_ = false;
            print("Saved " + nano_path_);
            return;
        }

        // Split buffer into lines for manipulation
        std::vector<std::string> lines;
        {
            std::size_t s = 0;
            while (true) {
                auto e = nano_buf_.find('\n', s);
                if (e == std::string::npos) { lines.push_back(nano_buf_.substr(s)); break; }
                lines.push_back(nano_buf_.substr(s, e - s));
                s = e + 1;
            }
        }
        // Clamp cursor
        nano_line_ = std::max(0, std::min(nano_line_, static_cast<int>(lines.size()) - 1));
        nano_col_  = std::max(0, std::min(nano_col_,  static_cast<int>(lines[nano_line_].size())));

        auto rebuild = [&]() {
            nano_buf_.clear();
            for (std::size_t i = 0; i < lines.size(); ++i) {
                if (i) nano_buf_ += '\n';
                nano_buf_ += lines[i];
            }
        };

        switch (k) {
        case 38: // Up
            if (nano_line_ > 0) { --nano_line_; nano_col_ = std::min(nano_col_, static_cast<int>(lines[nano_line_].size())); }
            break;
        case 40: // Down
            if (nano_line_ < static_cast<int>(lines.size()) - 1) { ++nano_line_; nano_col_ = std::min(nano_col_, static_cast<int>(lines[nano_line_].size())); }
            break;
        case 37: // Left
            if (nano_col_ > 0) --nano_col_;
            else if (nano_line_ > 0) { --nano_line_; nano_col_ = static_cast<int>(lines[nano_line_].size()); }
            break;
        case 39: // Right
            if (nano_col_ < static_cast<int>(lines[nano_line_].size())) ++nano_col_;
            else if (nano_line_ < static_cast<int>(lines.size()) - 1) { ++nano_line_; nano_col_ = 0; }
            break;
        case 36: nano_col_ = 0; break; // Home
        case 35: nano_col_ = static_cast<int>(lines[nano_line_].size()); break; // End
        case 8:  // Backspace
            if (nano_col_ > 0) {
                lines[nano_line_].erase(static_cast<std::size_t>(nano_col_ - 1), 1);
                --nano_col_;
            } else if (nano_line_ > 0) {
                // Merge with previous line.
                int prev_len = static_cast<int>(lines[nano_line_ - 1].size());
                lines[nano_line_ - 1] += lines[nano_line_];
                lines.erase(lines.begin() + nano_line_);
                --nano_line_;
                nano_col_ = prev_len;
            }
            nano_modified_ = true; rebuild(); break;
        case 46: // Delete
            if (nano_col_ < static_cast<int>(lines[nano_line_].size())) {
                lines[nano_line_].erase(static_cast<std::size_t>(nano_col_), 1);
            } else if (nano_line_ < static_cast<int>(lines.size()) - 1) {
                lines[nano_line_] += lines[nano_line_ + 1];
                lines.erase(lines.begin() + nano_line_ + 1);
            }
            nano_modified_ = true; rebuild(); break;
        case 13: // Enter  -  split line
            {
                std::string rest = lines[nano_line_].substr(static_cast<std::size_t>(nano_col_));
                lines[nano_line_] = lines[nano_line_].substr(0, static_cast<std::size_t>(nano_col_));
                lines.insert(lines.begin() + nano_line_ + 1, rest);
                ++nano_line_; nano_col_ = 0;
            }
            nano_modified_ = true; rebuild(); break;
        default:
            if (charcode >= 32 && charcode < 127) {
                lines[nano_line_].insert(static_cast<std::size_t>(nano_col_), 1,
                                         static_cast<char>(charcode));
                ++nano_col_;
                nano_modified_ = true; rebuild();
            }
            break;
        }
        return;
    }

    switch (keycode & 0xFFFF) {
    case 13: // Enter
        execute(input_);
        input_.clear();
        cursor_pos_ = 0;
        hist_idx_   = -1;
        break;
    case 8:  // Backspace
        if (cursor_pos_ > 0) {
            input_.erase(static_cast<std::size_t>(cursor_pos_ - 1), 1);
            --cursor_pos_;
        }
        break;
    case 46: // Delete
        if (cursor_pos_ < static_cast<int>(input_.size()))
            input_.erase(static_cast<std::size_t>(cursor_pos_), 1);
        break;
    case 37: // Left arrow
        if (cursor_pos_ > 0) --cursor_pos_;
        break;
    case 39: // Right arrow
        if (cursor_pos_ < static_cast<int>(input_.size())) ++cursor_pos_;
        break;
    case 36: // Home
        cursor_pos_ = 0;
        break;
    case 35: // End
        cursor_pos_ = static_cast<int>(input_.size());
        break;
    case 38: // Up arrow  -  history back
        if (!history_.empty()) {
            if (hist_idx_ == -1) {
                hist_stash_ = input_;
                hist_idx_   = static_cast<int>(history_.size()) - 1;
            } else if (hist_idx_ > 0) {
                --hist_idx_;
            }
            input_      = history_[static_cast<std::size_t>(hist_idx_)];
            cursor_pos_ = static_cast<int>(input_.size());
        }
        break;
    case 40: // Down arrow  -  history forward
        if (hist_idx_ >= 0) {
            if (hist_idx_ < static_cast<int>(history_.size()) - 1) {
                ++hist_idx_;
                input_      = history_[static_cast<std::size_t>(hist_idx_)];
            } else {
                hist_idx_   = -1;
                input_      = hist_stash_;
            }
            cursor_pos_ = static_cast<int>(input_.size());
        }
        break;
    case 9: { // Tab  -  complete command prefix
        static constexpr const char* CMDS[] = {
            "about", "cat", "clear", "contact", "curl", "date",
            "df", "echo", "exit", "free", "help", "hostname",
            "id", "ls", "logout", "neofetch", "ping", "projects",
            "ps", "pwd", "skills", "top", "uname", "uptime",
            "wget", "whoami",
        };
        // Only complete when cursor is at end and input has no space.
        if (cursor_pos_ == static_cast<int>(input_.size()) &&
            input_.find(' ') == std::string::npos && !input_.empty()) {
            std::vector<const char*> matches;
            for (auto* c : CMDS)
                if (std::strncmp(c, input_.c_str(), input_.size()) == 0)
                    matches.push_back(c);
            if (matches.size() == 1) {
                input_      = matches[0];
                cursor_pos_ = static_cast<int>(input_.size());
            } else if (matches.size() > 1) {
                print("jade@jadeos:~$ " + input_);
                std::string row;
                for (auto* m : matches) { row += m; row += "  "; }
                print(row);
            }
        }
        break;
    }
    default:
        if (charcode >= 0x20 && charcode < 0x7F) {
            char c = static_cast<char>(charcode);
            input_.insert(static_cast<std::size_t>(cursor_pos_), 1, c);
            ++cursor_pos_;
        }
        break;
    }
}

void Terminal::render(gpu::GPU& gpu, WinRect area, uint32_t tick) {
    sys_tick_ = tick;
    if (nano_mode_) {
        const int sc_lh   = static_cast<int>(LINE_H   * dpr_ + 0.5f);
        const int sc_pad  = static_cast<int>(4  * dpr_ + 0.5f);
        const int HEADER_H = static_cast<int>(20 * dpr_ + 0.5f);
        const int FOOTER_H = static_cast<int>(20 * dpr_ + 0.5f);

        // Background
        t_rect(gpu, area.x, area.y, area.w, area.h, 0xFF'07'1A'2E);

        // Header bar (inverted)
        t_rect(gpu, area.x, area.y, area.w, HEADER_H, 0xFF'89'B4'FA);
        std::string hdr = "  GNU nano   -   ";
        hdr += nano_path_;
        if (nano_modified_) hdr += "  [Modified]";
        t_text(gpu, area.x + sc_pad, area.y + sc_pad, 0xFF'07'1A'2E, hdr);

        // Footer bar
        const int foot_y = area.y + area.h - FOOTER_H;
        t_rect(gpu, area.x, foot_y, area.w, FOOTER_H, 0xFF'1A'2E'48);
        t_text(gpu, area.x + sc_pad, foot_y + sc_pad, 0xFF'50'C8'FF,
               "^X  Save+Exit    ^G  Help    Arrow keys to move");

        // Content area
        const int content_y = area.y + HEADER_H;
        const int content_h = area.h - HEADER_H - FOOTER_H;
        const int max_lines = (sc_lh > 0) ? (content_h / sc_lh) : 1;

        // Split buffer into lines
        std::vector<std::string> lines;
        {
            std::size_t s = 0;
            while (s <= nano_buf_.size()) {
                auto e = nano_buf_.find('\n', s);
                if (e == std::string::npos) { lines.push_back(nano_buf_.substr(s)); break; }
                lines.push_back(nano_buf_.substr(s, e - s));
                s = e + 1;
            }
        }

        // Adjust scroll to keep cursor visible
        if (nano_line_ < nano_scroll_) nano_scroll_ = nano_line_;
        if (nano_line_ >= nano_scroll_ + max_lines) nano_scroll_ = nano_line_ - max_lines + 1;

        const int L0 = nano_scroll_;
        const int L1 = std::min(L0 + max_lines, static_cast<int>(lines.size()));
        for (int r = L0; r < L1; ++r) {
            const int ty = content_y + (r - L0) * sc_lh + sc_pad;
            const bool cur_row = (r == nano_line_);
            if (cur_row) {
                t_rect(gpu, area.x, content_y + (r - L0) * sc_lh,
                       area.w, sc_lh, 0xFF'0A'20'3A);
            }
            t_text(gpu, area.x + sc_pad * 2, ty,
                   cur_row ? 0xFF'C6'D0'F5 : TC_TEXT, lines[r]);

            // Cursor block
            if (cur_row) {
                const std::string before =
                    (nano_col_ <= static_cast<int>(lines[r].size()))
                    ? lines[r].substr(0, static_cast<std::size_t>(nano_col_))
                    : lines[r];
                const int cx = area.x + sc_pad * 2 +
                               static_cast<int>(gpu.font(0).measure_width(before));
                if (blink_on_) {
                    const gpu::FontAtlas& nf = gpu.font(0);
                    const int row_base = content_y + (r - L0) * sc_lh + sc_pad;
                    const int curs_y =
                        row_base - static_cast<int>(nf.ascent() + 0.5f);
                    const int curs_h = static_cast<int>(nf.line_height() + 0.5f);
                    t_rect(gpu, cx, curs_y, 2, curs_h, 0xFF'89'B4'FA);
                }
            }
        }

        // Blink toggle
        if ((tick - blink_tick_) >= 30) {
            blink_on_   = !blink_on_;
            blink_tick_ = tick;
        }
        return;
    }

    // Background fill
    t_rect(gpu, area.x, area.y, area.w, area.h, TC_BG);

    // Blink toggle every ~30 ticks
    if ((tick - blink_tick_) >= 30) {
        blink_on_   = !blink_on_;
        blink_tick_ = tick;
    }

    // DPR-scaled layout constants
    const int sc_pad      = static_cast<int>(14 * dpr_ + 0.5f);  // 14 logical px -> baseline clears glyph top
    const int sc_line_h   = static_cast<int>(LINE_H * dpr_ + 0.5f);
    const int sc_prompt_h = static_cast<int>(PROMPT_H * dpr_ + 0.5f);

    const int input_bar_y = area.y + area.h - sc_prompt_h;
    const int text_area_h = area.h - sc_prompt_h - 2;
    const int max_lines   = (sc_line_h > 0) ? (text_area_h / sc_line_h) : 1;

    // Separator above input bar
    t_line(gpu, area.x, input_bar_y - 1,
           area.x + area.w - 1, input_bar_y - 1, TC_SEP);

    // Render scrollback (last max_lines lines from ring buffer)
    const int n     = ring_count_;
    const int start = std::max(0, n - max_lines);
    for (int i = start; i < n; ++i) {
        const int row = i - start;
        const int ty  = area.y + sc_pad + row * sc_line_h;
        const auto& ln = lines_ring_[static_cast<std::size_t>(
            (ring_head_ + i) % MAX_LINES)];

        uint32_t col = TC_TEXT;
        if (ln.find("jade@jadeos") != std::string::npos) col = TC_PROMPT;
        else if (!ln.empty() && static_cast<unsigned char>(ln[0]) == 0xE2)
            col = TC_DIM;  // UTF-8 box-drawing etc. (lead byte 0xE2)
        else if (ln.find("github.com") != std::string::npos ||
                 ln.find("jade.dev")   != std::string::npos) col = TC_ACCENT;
        else if (!ln.empty() && ln[0] == '=') col = TC_HEADING;

        // Strip simple ANSI escape codes (\033[...m)
        std::string clean;
        clean.reserve(ln.size());
        for (std::size_t j = 0; j < ln.size(); ) {
            if (ln[j] == '\033' && j + 1 < ln.size() && ln[j+1] == '[') {
                j += 2;
                while (j < ln.size() && ln[j] != 'm') ++j;
                if (j < ln.size()) ++j;
            } else {
                clean += ln[j++];
            }
        }
        t_text(gpu, area.x + sc_pad, ty, col, clean);
    }

    // Input bar background
    t_rect(gpu, area.x, input_bar_y, area.w, sc_prompt_h, TC_BG);

    static const std::string PROMPT = "jade@jadeos:~$ ";

    // Prompt + input share one baseline vertically centered in the prompt bar.
    const gpu::FontAtlas& f0    = gpu.font(0);
    const float           lh    = f0.line_height();
    const float           asc   = f0.ascent();
    const int input_base =
        input_bar_y + static_cast<int>((sc_prompt_h - lh) * 0.5f + asc + 0.5f);

    t_text(gpu, area.x + sc_pad, input_base, TC_PROMPT, PROMPT);

    // Input text
    const int prompt_w = static_cast<int>(f0.measure_width(PROMPT));
    const int input_x = area.x + sc_pad + prompt_w;
    t_text(gpu, input_x, input_base, TC_CMD, input_);

    // Cursor (glyph box: baseline minus ascent, height = line height)
    if (blink_on_) {
        const std::string before = input_.substr(0, static_cast<std::size_t>(cursor_pos_));
        const int cx = input_x + static_cast<int>(f0.measure_width(before));
        const int curs_y = input_base - static_cast<int>(asc + 0.5f);
        const int curs_h = static_cast<int>(lh + 0.5f);
        t_rect(gpu, cx, curs_y, 2, curs_h, TC_CURSOR);
    }
}

} // namespace os
