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

static std::string trim_sp(const std::string& s) {
    std::string p = s;
    while (!p.empty() && p.front() == ' ') p.erase(p.begin());
    while (!p.empty() && p.back()  == ' ') p.pop_back();
    return p;
}

static bool match_glob(const std::string& text, const std::string& pat) {
    const std::size_t n = text.size(), m = pat.size();
    std::vector<std::vector<bool>> dp(n + 1, std::vector<bool>(m + 1, false));
    dp[0][0] = true;
    for (std::size_t j = 1; j <= m; ++j)
        if (pat[j-1] == '*') dp[0][j] = dp[0][j-1];
    for (std::size_t i = 1; i <= n; ++i)
        for (std::size_t j = 1; j <= m; ++j) {
            if (pat[j-1] == '*')
                dp[i][j] = dp[i-1][j] || dp[i][j-1];
            else if (pat[j-1] == '?' || pat[j-1] == text[i-1])
                dp[i][j] = dp[i-1][j-1];
        }
    return dp[n][m];
}

static std::vector<std::string> split_args(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool in_sq = false, in_dq = false;
    for (char c : s) {
        if      (c == '\'' && !in_dq) in_sq = !in_sq;
        else if (c == '"'  && !in_sq) in_dq = !in_dq;
        else if ((c == ' ' || c == '\t') && !in_sq && !in_dq) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else { cur += c; }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string Terminal::resolve_vpath(const std::string& raw) const {
    std::string p = trim_sp(raw);
    if (p.empty() || p == ".") return cwd_;
    if (p == "~") return std::string("/home/jade");
    if (p.size() >= 2 && p[0] == '~' && p[1] == '/')
        return std::string("/home/jade") + p.substr(1);
    if (p[0] == '/') return p;
    if (p == "..") {
        const auto sl = cwd_.rfind('/');
        return (sl == 0) ? "/" : cwd_.substr(0, sl);
    }
    if (p.size() >= 3 && p[0] == '.' && p[1] == '.' && p[2] == '/') {
        const auto sl = cwd_.rfind('/');
        const std::string up = (sl == 0) ? "/" : cwd_.substr(0, sl);
        return resolve_vpath(up + "/" + p.substr(3));
    }
    if (p.size() >= 2 && p[0] == '.' && p[1] == '/')
        return cwd_ + "/" + p.substr(2);
    return cwd_ + "/" + p;
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
    if (capture_buf_) {
        *capture_buf_ += s;
        *capture_buf_ += '\n';
        return;
    }
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

    print(make_prompt() + cmd);

    // Split on unquoted '|' into pipeline stages.
    std::vector<std::string> stages;
    {
        std::string cur;
        bool in_sq = false, in_dq = false;
        for (char c : cmd) {
            if      (c == '\'' && !in_dq) in_sq = !in_sq;
            else if (c == '"'  && !in_sq) in_dq = !in_dq;
            else if (c == '|'  && !in_sq && !in_dq) { stages.push_back(cur); cur.clear(); continue; }
            cur += c;
        }
        stages.push_back(cur);
    }

    std::string pipe_stdin;
    for (std::size_t i = 0; i < stages.size(); ++i) {
        const std::string stage = trim_sp(stages[i]);
        if (stage.empty()) continue;
        std::string verb, arg;
        const auto sp = stage.find(' ');
        if (sp == std::string::npos) { verb = stage; }
        else { verb = stage.substr(0, sp); arg = trim_sp(stage.substr(sp + 1)); }

        if (i + 1 < stages.size()) {
            pipe_stdin = run_stage(verb, arg, pipe_stdin);
        } else {
            dispatch(verb, arg, pipe_stdin);
            if (nano_mode_) return;  // nano entered; skip trailing blank line
        }
    }
    print("");
}

std::string Terminal::make_prompt() const {
    std::string display = cwd_;
    static const std::string HOME = "/home/jade";
    if (display.rfind(HOME, 0) == 0)
        display = "~" + display.substr(HOME.size());
    return "jade@jadeos:" + display + "$ ";
}

std::string Terminal::run_stage(const std::string& verb, const std::string& arg,
                                const std::string& stdin_text) {
    std::string buf;
    capture_buf_ = &buf;
    dispatch(verb, arg, stdin_text);
    capture_buf_ = nullptr;
    return buf;
}

void Terminal::dispatch(const std::string& verb, const std::string& arg,
                        const std::string& stdin_text) {
    if (verb.empty())          { /* blank line */ }
    else if (verb == "help")   cmd_help();
    else if (verb == "about")  cmd_about();
    else if (verb == "skills") cmd_skills();
    else if (verb == "projects") cmd_projects();
    else if (verb == "contact") cmd_contact();
    else if (verb == "ls")     cmd_ls(arg);
    else if (verb == "cat")    cmd_cat(arg);
    else if (verb == "cd")     cmd_cd(arg);
    else if (verb == "grep")   cmd_grep(arg, stdin_text);
    else if (verb == "find")   cmd_find(arg);
    else if (verb == "cp")     cmd_cp(arg);
    else if (verb == "mv")     cmd_mv(arg);
    else if (verb == "rm")     cmd_rm(arg);
    else if (verb == "touch")  cmd_touch(arg);
    else if (verb == "head")   cmd_head(arg, stdin_text);
    else if (verb == "tail")   cmd_tail(arg, stdin_text);
    else if (verb == "uname")  cmd_uname(arg == "-a" || arg == "-r");
    else if (verb == "whoami") cmd_whoami();
    else if (verb == "ps")     cmd_ps(arg == "aux" || arg == "-aux" || arg == "-A");
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
        }
    }
    else if (verb == "clear")  { ring_head_ = 0; ring_count_ = 0; }
    else if (verb == "pwd")    print(cwd_);
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
}

void Terminal::cmd_help() {
    print_lines({
        "JadeOS command reference",
        "  about       -  who I am",
        "  skills      -  technical skills breakdown",
        "  projects    -  portfolio project list",
        "  contact     -  contact details",
        "",
        "  ls [dir]    -  list directory",
        "  cat <path>  -  print file (incl. /proc/*)",
        "  cd [dir]    -  change directory",
        "  pwd         -  print working directory",
        "  find [path] [-name pat] [-type f|d]",
        "  grep [-i] [-n] pattern [file]",
        "  head [-n N] [file]",
        "  tail [-n N] [file]",
        "  cp <src> <dst>",
        "  mv <src> <dst>",
        "  rm [-rf] <path>",
        "  touch <path>",
        "",
        "  ps [aux]    -  process list",
        "  top         -  live process snapshot",
        "  free [-h]   -  memory usage",
        "  df [-h]     -  disk usage",
        "  uptime      -  system uptime",
        "  uname [-a]  -  kernel info",
        "  neofetch    -  system summary",
        "",
        "  curl <url>  -  HTTP GET",
        "  ping <host> -  RTT probe",
        "  echo <str>  -  echo string",
        "  date        -  current date",
        "  nano <path> -  text editor",
        "  clear       -  clear scrollback",
        "  exit        -  end session",
        "",
        "  pipe:  cmd1 | cmd2 | cmd3",
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
    const std::string path = resolve_vpath(arg);

    if (fs_) {
        const uint32_t ino = fs_->lookup(path);
        if (ino == 0) {
            print("ls: cannot access '" + (arg.empty() ? path : arg) + "': No such file or directory");
            return;
        }
        const Inode* node = fs_->stat(ino);
        if (node && node->type != InodeType::Directory) {
            print("ls: '" + (arg.empty() ? path : arg) + "': Not a directory");
            return;
        }
        const auto entries = fs_->readdir(path);
        if (!entries.empty()) {
            std::string line;
            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (i > 0) line += "  ";
                line += entries[i];
                if (line.size() > 72) { print(line); line.clear(); }
            }
            if (!line.empty()) print(line);
        }
        return;
    }

    print("ls: cannot access '" + (arg.empty() ? path : arg) + "': No such file or directory");
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

// ---- cd ---------------------------------------------------------------

void Terminal::cmd_cd(const std::string& arg) {
    const std::string target = resolve_vpath(arg.empty() ? "~" : arg);
    if (!fs_) { cwd_ = target; return; }
    const uint32_t ino = fs_->lookup(target);
    if (ino == 0) { print("cd: " + arg + ": No such file or directory"); return; }
    const Inode* node = fs_->stat(ino);
    if (node && node->type != InodeType::Directory) {
        print("cd: " + arg + ": Not a directory"); return;
    }
    cwd_ = target;
}

// ---- grep -------------------------------------------------------------

void Terminal::cmd_grep(const std::string& arg, const std::string& stdin_text) {
    const auto args = split_args(arg);
    bool flag_i = false, flag_n = false;
    std::string pattern;
    std::vector<std::string> files;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "-i")               flag_i = true;
        else if (a == "-n")          flag_n = true;
        else if (a == "-in" || a == "-ni") { flag_i = true; flag_n = true; }
        else if (pattern.empty())    pattern = a;
        else                         files.push_back(a);
    }
    if (pattern.empty()) { print("grep: missing pattern"); return; }

    auto to_lower_str = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    auto grep_text = [&](const std::string& text, const std::string& filename) {
        std::istringstream ss(text);
        std::string ln;
        int line_num = 0;
        while (std::getline(ss, ln)) {
            ++line_num;
            const std::string haystack = flag_i ? to_lower_str(ln)      : ln;
            const std::string needle   = flag_i ? to_lower_str(pattern) : pattern;
            if (haystack.find(needle) == std::string::npos) continue;
            std::string out;
            if (!filename.empty() && files.size() > 1) out += filename + ":";
            if (flag_n) out += std::to_string(line_num) + ":";
            out += ln;
            print(out);
        }
    };

    if (files.empty()) {
        grep_text(stdin_text, "");
    } else {
        for (const auto& f : files) {
            const std::string path = resolve_vpath(f);
            if (!fs_) continue;
            const std::string text = fs_->read_text(path);
            if (text.empty() && fs_->lookup(path) == 0) {
                print("grep: " + f + ": No such file or directory");
                continue;
            }
            grep_text(text, f);
        }
    }
}

// ---- find -------------------------------------------------------------

void Terminal::cmd_find(const std::string& arg) {
    const auto args = split_args(arg);
    std::string root = cwd_;
    std::string name_pat;
    char type_filter = '\0';  // 'f' = files, 'd' = dirs

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-name" && i + 1 < args.size())
            name_pat = args[++i];
        else if (args[i] == "-type" && i + 1 < args.size())
            type_filter = args[++i].empty() ? '\0' : args[++i][0];
        else if (args[i][0] != '-')
            root = resolve_vpath(args[i]);
    }
    if (!fs_) return;

    // Recursively walk the VFS inode tree.
    std::function<void(const std::string&)> walk = [&](const std::string& path) {
        const auto entries = fs_->readdir(path);
        for (const auto& entry : entries) {
            const bool is_dir = !entry.empty() && entry.back() == '/';
            const std::string name = is_dir ? entry.substr(0, entry.size() - 1) : entry;
            const std::string full = path + (path.back() == '/' ? "" : "/") + name;

            const bool name_ok = name_pat.empty() || match_glob(name, name_pat);
            const bool type_ok = type_filter == '\0' ||
                                 (type_filter == 'f' && !is_dir) ||
                                 (type_filter == 'd' &&  is_dir);
            if (name_ok && type_ok) print(full);
            if (is_dir) walk(full);
        }
    };
    walk(root);
}

// ---- cp ---------------------------------------------------------------

void Terminal::cmd_cp(const std::string& arg) {
    const auto args = split_args(arg);
    if (args.size() < 2) { print("cp: missing destination operand"); return; }
    if (!fs_) return;
    const std::string src = resolve_vpath(args[0]);
    const std::string dst = resolve_vpath(args[1]);
    const std::string text = fs_->read_text(src);
    if (text.empty() && fs_->lookup(src) == 0) {
        print("cp: " + args[0] + ": No such file or directory"); return;
    }
    fs_->write_file(dst, text);
}

// ---- mv ---------------------------------------------------------------

void Terminal::cmd_mv(const std::string& arg) {
    const auto args = split_args(arg);
    if (args.size() < 2) { print("mv: missing destination operand"); return; }
    if (!fs_) return;
    const std::string src = resolve_vpath(args[0]);
    const std::string dst = resolve_vpath(args[1]);
    if (!fs_->rename_file(src, dst))
        print("mv: " + args[0] + ": No such file or directory");
}

// ---- rm ---------------------------------------------------------------

void Terminal::cmd_rm(const std::string& arg) {
    const auto args = split_args(arg);
    std::vector<std::string> paths;
    for (const auto& a : args) {
        if (a == "-f" || a == "-r" || a == "-rf" || a == "-fr" ||
            a == "-v" || a == "-rv" || a == "-fv") continue;
        paths.push_back(a);
    }
    if (paths.empty()) { print("rm: missing operand"); return; }
    if (!fs_) return;
    for (const auto& p : paths) {
        const std::string path = resolve_vpath(p);
        if (!fs_->remove_file(path))
            print("rm: cannot remove '" + p + "': No such file or directory");
    }
}

// ---- touch ------------------------------------------------------------

void Terminal::cmd_touch(const std::string& arg) {
    const auto args = split_args(arg);
    if (args.empty()) { print("touch: missing file operand"); return; }
    if (!fs_) return;
    for (const auto& a : args) {
        const std::string path = resolve_vpath(a);
        if (!fs_->find(path)) fs_->write_file(path, "");
    }
}

// ---- head -------------------------------------------------------------

void Terminal::cmd_head(const std::string& arg, const std::string& stdin_text) {
    const auto args = split_args(arg);
    int n = 10;
    std::string file;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-n" && i + 1 < args.size())
            n = std::atoi(args[++i].c_str());
        else if (args[i].size() > 1 && args[i][0] == '-' &&
                 std::isdigit(static_cast<unsigned char>(args[i][1])))
            n = std::atoi(args[i].c_str() + 1);
        else
            file = args[i];
    }

    std::string text;
    if (file.empty()) {
        text = stdin_text;
    } else {
        if (!fs_) return;
        const std::string path = resolve_vpath(file);
        text = fs_->read_text(path);
        if (text.empty() && fs_->lookup(path) == 0) {
            print("head: " + file + ": No such file or directory"); return;
        }
    }

    std::istringstream ss(text);
    std::string ln;
    for (int count = 0; count < n && std::getline(ss, ln); ++count)
        print(ln);
}

// ---- tail -------------------------------------------------------------

void Terminal::cmd_tail(const std::string& arg, const std::string& stdin_text) {
    const auto args = split_args(arg);
    int n = 10;
    std::string file;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-n" && i + 1 < args.size())
            n = std::atoi(args[++i].c_str());
        else if (args[i].size() > 1 && args[i][0] == '-' &&
                 std::isdigit(static_cast<unsigned char>(args[i][1])))
            n = std::atoi(args[i].c_str() + 1);
        else
            file = args[i];
    }

    std::string text;
    if (file.empty()) {
        text = stdin_text;
    } else {
        if (!fs_) return;
        const std::string path = resolve_vpath(file);
        text = fs_->read_text(path);
        if (text.empty() && fs_->lookup(path) == 0) {
            print("tail: " + file + ": No such file or directory"); return;
        }
    }

    std::vector<std::string> lines;
    std::istringstream ss(text);
    std::string ln;
    while (std::getline(ss, ln)) lines.push_back(ln);

    const int start = std::max(0, static_cast<int>(lines.size()) - n);
    for (int i = start; i < static_cast<int>(lines.size()); ++i)
        print(lines[i]);
}

void Terminal::send_key(uint32_t keycode, uint32_t charcode) {
    // JS sends keyup too (bit 20 clear); shell/nano only react to keydown.
    if (((keycode >> 20) & 1u) == 0) return;

    blink_on_   = true;
    blink_tick_ = 0;

    if (nano_mode_) {
        const bool ctrl_held = (keycode >> 17) & 1u;
        const uint32_t k     = keycode & 0xFFFFu;

        // Helper: split buf into lines (used throughout)
        auto get_lines = [&]() -> std::vector<std::string> {
            std::vector<std::string> lines;
            std::size_t s = 0;
            while (true) {
                auto e = nano_buf_.find('\n', s);
                if (e == std::string::npos) { lines.push_back(nano_buf_.substr(s)); break; }
                lines.push_back(nano_buf_.substr(s, e - s));
                s = e + 1;
            }
            return lines;
        };

        auto rebuild = [&](const std::vector<std::string>& lines) {
            nano_buf_.clear();
            for (std::size_t i = 0; i < lines.size(); ++i) {
                if (i) nano_buf_ += '\n';
                nano_buf_ += lines[i];
            }
        };

        auto do_save = [&]() {
            if (fs_) fs_->write_file(nano_path_, nano_buf_);
            if (save_callback_) save_callback_(nano_path_);
            nano_modified_ = false;
            const int lc = static_cast<int>(std::count(nano_buf_.begin(), nano_buf_.end(), '\n')) + 1;
            nano_status_ = "Wrote " + std::to_string(lc) + " line" + (lc == 1 ? "." : "s.");
        };

        // ----- Search mode (Ctrl+W active) -----
        if (nano_search_mode_) {
            if (k == 27 || (ctrl_held && (k == 'G' || k == 'g' || k == 87 || k == 119))) {
                // Escape or Ctrl+G = cancel search
                nano_search_mode_ = false;
                nano_status_ = "";
            } else if (k == 13) {
                // Enter: find next occurrence
                nano_search_mode_ = false;
                if (!nano_search_str_.empty()) {
                    const auto lines = get_lines();
                    const int total = static_cast<int>(lines.size());
                    bool found = false;
                    for (int i = 1; i <= total; ++i) {
                        const int r = (nano_line_ + i) % total;
                        const auto pos = lines[r].find(nano_search_str_);
                        if (pos != std::string::npos) {
                            nano_line_ = r; nano_col_ = static_cast<int>(pos);
                            nano_search_match_ = r;
                            nano_status_ = "";
                            found = true; break;
                        }
                    }
                    if (!found) nano_status_ = "\"" + nano_search_str_ + "\": Not found";
                }
            } else if (k == 8 && !nano_search_str_.empty()) {
                nano_search_str_.pop_back();
            } else if (charcode >= 32 && charcode < 127) {
                nano_search_str_ += static_cast<char>(charcode);
            }
            return;
        }

        // ----- Exit-save prompt (Ctrl+X when modified) -----
        if (nano_exit_prompt_) {
            const char ch = static_cast<char>(charcode);
            if (ch == 'y' || ch == 'Y') {
                do_save();
                nano_mode_        = false;
                nano_exit_prompt_ = false;
                print("Saved " + nano_path_);
            } else if (ch == 'n' || ch == 'N') {
                nano_mode_        = false;
                nano_exit_prompt_ = false;
            } else if (k == 27 || (ctrl_held && (k == 'C' || k == 'c'))) {
                // Ctrl+C / Escape = cancel
                nano_exit_prompt_ = false;
                nano_status_ = "";
            }
            return;
        }

        // ----- Normal editing -----

        // Ctrl+O  - Write Out (save without exiting)
        if (ctrl_held && (k == 'O' || k == 'o' || k == 79 || k == 111)) {
            do_save();
            return;
        }
        // Ctrl+X  - Exit
        if (ctrl_held && (k == 'X' || k == 'x' || k == 88 || k == 120)) {
            if (!nano_modified_) {
                nano_mode_ = false;
            } else {
                nano_exit_prompt_ = true;
                nano_status_ = "";
            }
            return;
        }
        // Ctrl+K  - Cut current line
        if (ctrl_held && (k == 'K' || k == 'k' || k == 75 || k == 107)) {
            auto lines = get_lines();
            nano_cut_buf_ = lines[nano_line_];
            lines.erase(lines.begin() + nano_line_);
            if (lines.empty()) lines.push_back("");
            nano_line_ = std::min(nano_line_, static_cast<int>(lines.size()) - 1);
            nano_col_  = 0;
            nano_modified_ = true; rebuild(lines);
            nano_status_ = "";
            return;
        }
        // Ctrl+U  - Uncut / Paste
        if (ctrl_held && (k == 'U' || k == 'u' || k == 85 || k == 117)) {
            if (!nano_cut_buf_.empty()) {
                auto lines = get_lines();
                lines.insert(lines.begin() + nano_line_, nano_cut_buf_);
                nano_modified_ = true; rebuild(lines);
                nano_status_ = "";
            }
            return;
        }
        // Ctrl+W  - Where Is (search)
        if (ctrl_held && (k == 'W' || k == 'w' || k == 87 || k == 119)) {
            nano_search_mode_ = true;
            nano_search_str_  = "";
            return;
        }
        // Ctrl+G  - Help (show shortcuts in status)
        if (ctrl_held && (k == 'G' || k == 'g')) {
            nano_status_ = "^O Write  ^X Exit  ^K Cut  ^U Paste  ^W Search";
            return;
        }
        // Ctrl+A / Home on line  - First line
        if (ctrl_held && (k == 'A' || k == 'a')) {
            nano_line_ = 0; nano_col_ = 0; return;
        }
        // Ctrl+E / End  - Last line
        if (ctrl_held && (k == 'E' || k == 'e')) {
            const auto lines = get_lines();
            nano_line_ = static_cast<int>(lines.size()) - 1;
            nano_col_  = static_cast<int>(lines[nano_line_].size());
            return;
        }
        // Page Up (Ctrl+Y or PgUp key 33)
        if ((ctrl_held && (k == 'Y' || k == 'y')) || k == 33) {
            nano_line_ = std::max(0, nano_line_ - 20); return;
        }
        // Page Down (Ctrl+V or PgDn key 34)
        if ((ctrl_held && (k == 'V' || k == 'v')) || k == 34) {
            const auto lines = get_lines();
            nano_line_ = std::min(static_cast<int>(lines.size()) - 1, nano_line_ + 20);
            return;
        }

        // Arrow / editing keys
        auto lines = get_lines();
        nano_line_ = std::max(0, std::min(nano_line_, static_cast<int>(lines.size()) - 1));
        nano_col_  = std::max(0, std::min(nano_col_,  static_cast<int>(lines[nano_line_].size())));

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
                const int prev_len = static_cast<int>(lines[nano_line_ - 1].size());
                lines[nano_line_ - 1] += lines[nano_line_];
                lines.erase(lines.begin() + nano_line_);
                --nano_line_; nano_col_ = prev_len;
            }
            nano_modified_ = true; rebuild(lines); nano_status_ = ""; break;
        case 46: // Delete
            if (nano_col_ < static_cast<int>(lines[nano_line_].size())) {
                lines[nano_line_].erase(static_cast<std::size_t>(nano_col_), 1);
            } else if (nano_line_ < static_cast<int>(lines.size()) - 1) {
                lines[nano_line_] += lines[nano_line_ + 1];
                lines.erase(lines.begin() + nano_line_ + 1);
            }
            nano_modified_ = true; rebuild(lines); nano_status_ = ""; break;
        case 13: // Enter
            {
                std::string rest = lines[nano_line_].substr(static_cast<std::size_t>(nano_col_));
                lines[nano_line_] = lines[nano_line_].substr(0, static_cast<std::size_t>(nano_col_));
                lines.insert(lines.begin() + nano_line_ + 1, rest);
                ++nano_line_; nano_col_ = 0;
            }
            nano_modified_ = true; rebuild(lines); nano_status_ = ""; break;
        default:
            if (!ctrl_held && charcode >= 32 && charcode < 127) {
                lines[nano_line_].insert(static_cast<std::size_t>(nano_col_), 1,
                                         static_cast<char>(charcode));
                ++nano_col_;
                nano_modified_ = true; rebuild(lines); nano_status_ = "";
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
            "about", "cat", "cd", "clear", "contact", "cp", "curl", "date",
            "df", "echo", "exit", "find", "free", "grep", "head", "help",
            "hostname", "id", "ls", "logout", "mv", "nano", "neofetch",
            "ping", "projects", "ps", "pwd", "rm", "skills", "tail", "top",
            "touch", "uname", "uptime", "vi", "vim", "wget", "whoami",
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
                print(make_prompt() + input_);
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
        const int sc_lh    = static_cast<int>(LINE_H * dpr_ + 0.5f);
        const int sc_pad   = static_cast<int>(6 * dpr_ + 0.5f);
        const int BAR_H    = sc_lh + sc_pad;  // one row tall, matches line height

        // Background
        t_rect(gpu, area.x, area.y, area.w, area.h, 0xFF'07'1A'2E);

        t_rect(gpu, area.x, area.y, area.w, BAR_H, 0xFF'89'B4'FA);
        {
            std::string fname = nano_path_;
            static const std::string HOME = "/home/jade";
            if (fname.rfind(HOME, 0) == 0) fname = "~" + fname.substr(HOME.size());
            const std::string mod_tag = nano_modified_ ? " [Modified]" : "";

            // Left: "GNU nano 7.2"  Centre: filename  Right: "Modified"
            const std::string left_str  = "  GNU nano 7.2";
            const std::string right_str = mod_tag + "  ";
            const int lw = static_cast<int>(gpu.font(0).measure_width(left_str));
            const int rw = static_cast<int>(gpu.font(0).measure_width(right_str));
            const int fw = static_cast<int>(gpu.font(0).measure_width(fname));
            const int cy = area.y + sc_pad;
            t_text(gpu, area.x,                              cy, 0xFF'07'1A'2E, left_str);
            t_text(gpu, area.x + (area.w - fw) / 2,         cy, 0xFF'07'1A'2E, fname);
            t_text(gpu, area.x + area.w - rw,                cy, 0xFF'07'1A'2E, right_str);
            (void)lw;
        }

        const int foot_y  = area.y + area.h - BAR_H * 2;
        const int foot_y2 = foot_y + BAR_H;
        t_rect(gpu, area.x, foot_y,  area.w, BAR_H, 0xFF'89'B4'FA);
        t_rect(gpu, area.x, foot_y2, area.w, BAR_H, 0xFF'89'B4'FA);

        if (nano_exit_prompt_) {
            t_text(gpu, area.x + sc_pad, foot_y  + sc_pad, 0xFF'07'1A'2E,
                   "  Save modified buffer?  (Y)es  (N)o  ^C Cancel");
        } else if (nano_search_mode_) {
            t_text(gpu, area.x + sc_pad, foot_y  + sc_pad, 0xFF'07'1A'2E,
                   "  Search: " + nano_search_str_ + "_");
            t_text(gpu, area.x + sc_pad, foot_y2 + sc_pad, 0xFF'07'1A'2E,
                   "  Enter: find next    ^G: cancel");
        } else if (!nano_status_.empty()) {
            t_text(gpu, area.x + sc_pad, foot_y  + sc_pad, 0xFF'07'1A'2E,
                   "  " + nano_status_);
            t_text(gpu, area.x + sc_pad, foot_y2 + sc_pad, 0xFF'07'1A'2E,
                   "  ^G Help   ^X Exit   ^O Write Out   ^K Cut   ^U Paste   ^W Search");
        } else {
            // Real nano two-row shortcut bar
            t_text(gpu, area.x + sc_pad, foot_y  + sc_pad, 0xFF'07'1A'2E,
                   "  ^G Help   ^X Exit   ^O Write Out   ^K Cut Line   ^W Search");
            t_text(gpu, area.x + sc_pad, foot_y2 + sc_pad, 0xFF'07'1A'2E,
                   "  ^U Paste  ^Y Pg Up  ^V Pg Down     ^A First Line  ^E Last Line");
        }

        {
            char pos_buf[32];
            std::snprintf(pos_buf, sizeof(pos_buf), "[ line %d/%zu, col %d ]  ",
                          nano_line_ + 1,
                          std::count(nano_buf_.begin(), nano_buf_.end(), '\n') + 1,
                          nano_col_ + 1);
            const int pw = static_cast<int>(gpu.font(0).measure_width(pos_buf));
            t_text(gpu, area.x + area.w - pw, foot_y2 + sc_pad, 0xFF'07'1A'2E, pos_buf);
        }

        const int content_y = area.y + BAR_H;
        const int content_h = area.h - BAR_H - BAR_H * 2;
        const int max_lines = (sc_lh > 0) ? (content_h / sc_lh) : 1;
        const int text_x    = area.x + sc_pad * 2;

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

        // Scroll to keep cursor visible
        if (nano_line_ < nano_scroll_) nano_scroll_ = nano_line_;
        if (nano_line_ >= nano_scroll_ + max_lines) nano_scroll_ = nano_line_ - max_lines + 1;

        const int L0 = nano_scroll_;
        const int L1 = std::min(L0 + max_lines, static_cast<int>(lines.size()));
        for (int r = L0; r < L1; ++r) {
            const int row_y  = content_y + (r - L0) * sc_lh;
            const int text_y = row_y + sc_pad;
            const bool cur_row = (r == nano_line_);

            if (cur_row)
                t_rect(gpu, area.x, row_y, area.w, sc_lh, 0xFF'0A'20'3A);

            t_text(gpu, text_x, text_y, cur_row ? 0xFF'C6'D0'F5 : TC_TEXT, lines[r]);

            if (cur_row && blink_on_) {
                const std::string before =
                    lines[r].substr(0, std::min(static_cast<std::size_t>(nano_col_), lines[r].size()));
                const int cx = text_x + static_cast<int>(gpu.font(0).measure_width(before));
                const gpu::FontAtlas& nf = gpu.font(0);
                const int curs_y = text_y - static_cast<int>(nf.ascent() + 0.5f);
                const int curs_h = static_cast<int>(nf.line_height() + 0.5f);
                t_rect(gpu, cx, curs_y, 2, curs_h, 0xFF'89'B4'FA);
            }
        }

        // Blink toggle
        if ((tick - blink_tick_) >= 30) { blink_on_ = !blink_on_; blink_tick_ = tick; }
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

    const std::string PROMPT = make_prompt();

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
