#include "filesystem.hpp"
#include "scheduler.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace os {

#ifdef __EMSCRIPTEN__
EM_JS(void, js_ls_set, (const char* key, const char* val), {
    try { localStorage.setItem(UTF8ToString(key), UTF8ToString(val)); } catch(e) {}
});

EM_JS(void, js_ls_remove, (const char* key), {
    try { localStorage.removeItem(UTF8ToString(key)); } catch(e) {}
});

EM_JS(char*, js_ls_get, (const char* key), {
    const v = localStorage.getItem(UTF8ToString(key));
    if (v === null) return 0;
    const n = lengthBytesUTF8(v) + 1;
    const p = _malloc(n);
    stringToUTF8(v, p, n);
    return p;
});

static constexpr const char* LS_INDEX  = "jadeos:__index__";
static constexpr const char* LS_PREFIX = "jadeos:file:";

static void ls_persist(const std::string& name, const std::string& text) {
    js_ls_set((std::string(LS_PREFIX) + name).c_str(), text.c_str());
    char* raw = js_ls_get(LS_INDEX);
    std::string idx = raw ? std::string(raw) : "";
    if (raw) std::free(raw);
    const std::string entry = name + "\n";
    if (idx.find(entry) == std::string::npos) idx += entry;
    js_ls_set(LS_INDEX, idx.c_str());
}

static void ls_forget(const std::string& name) {
    js_ls_remove((std::string(LS_PREFIX) + name).c_str());
    char* raw = js_ls_get(LS_INDEX);
    std::string idx = raw ? std::string(raw) : "";
    if (raw) std::free(raw);
    const std::string entry = name + "\n";
    const auto pos = idx.find(entry);
    if (pos != std::string::npos) idx.erase(pos, entry.size());
    js_ls_set(LS_INDEX, idx.c_str());
}

static void ls_load_all(Filesystem& fs) {
    char* raw = js_ls_get(LS_INDEX);
    if (!raw) return;
    std::string idx(raw);
    std::free(raw);
    std::istringstream ss(idx);
    std::string name;
    while (std::getline(ss, name)) {
        if (name.empty()) continue;
        char* vraw = js_ls_get((std::string(LS_PREFIX) + name).c_str());
        if (!vraw) continue;
        fs.write_file(name, std::string(vraw));
        std::free(vraw);
    }
}
#endif

bool Filesystem::add_file(std::string name, std::vector<uint8_t> data) {
    if (files_.count(name)) return false;
    files_.emplace(name, FileEntry{std::move(name), std::move(data)});
    return true;
}

void Filesystem::write_file(std::string name, std::string text) {
    std::vector<uint8_t> data(text.begin(), text.end());
    auto it = files_.find(name);
    if (it != files_.end()) {
        it->second.data = std::move(data);
    } else {
        files_.emplace(name, FileEntry{ name, std::move(data) });
        // Create a VFS inode so the file shows up in readdir / ls.
        if (lookup(name) == 0) {
            const auto slash = name.rfind('/');
            const std::string parent_path =
                (slash == std::string::npos || slash == 0) ? "/" : name.substr(0, slash);
            const std::string fname =
                (slash == std::string::npos) ? name : name.substr(slash + 1);
            const uint32_t parent_ino = lookup(parent_path);
            if (parent_ino != 0) make_file(parent_ino, fname.c_str());
        }
    }
#ifdef __EMSCRIPTEN__
    if (persist_enabled_) ls_persist(name, text);
#endif
}

std::string Filesystem::read_text(const std::string& name) const {
    auto it = files_.find(name);
    if (it == files_.end()) return "";
    return std::string(reinterpret_cast<const char*>(it->second.data.data()),
                       it->second.data.size());
}

const FileEntry* Filesystem::find(const std::string& name) const {
    auto it = files_.find(name);
    return (it == files_.end()) ? nullptr : &it->second;
}

bool Filesystem::remove_file(const std::string& name) {
    const bool ok = files_.erase(name) > 0;
#ifdef __EMSCRIPTEN__
    if (persist_enabled_ && ok) ls_forget(name);
#endif
    return ok;
}

bool Filesystem::rename_file(const std::string& from, const std::string& to) {
    auto it = files_.find(from);
    if (it == files_.end()) return false;
    FileEntry entry = std::move(it->second);
    entry.name = to;
    files_.erase(it);
    files_.emplace(to, std::move(entry));
#ifdef __EMSCRIPTEN__
    if (persist_enabled_) {
        ls_forget(from);
        ls_persist(to, read_text(to));
    }
#endif
    return true;
}

uint32_t Filesystem::make_dir(uint32_t parent_ino, const char* name) {
    const uint32_t ino = next_ino_++;
    Inode n;
    n.ino        = ino;
    n.type       = InodeType::Directory;
    n.parent_ino = parent_ino;
    n.name       = name;
    n.mode       = 0755;
    if (parent_ino && inodes_.count(parent_ino))
        inodes_[parent_ino].children.push_back(ino);
    inodes_.emplace(ino, std::move(n));
    return ino;
}

uint32_t Filesystem::make_file(uint32_t parent_ino, const char* name) {
    const uint32_t ino = next_ino_++;
    Inode n;
    n.ino        = ino;
    n.type       = InodeType::Regular;
    n.parent_ino = parent_ino;
    n.name       = name;
    n.mode       = 0644;
    if (parent_ino && inodes_.count(parent_ino))
        inodes_[parent_ino].children.push_back(ino);
    inodes_.emplace(ino, std::move(n));
    return ino;
}

uint32_t Filesystem::make_device(uint32_t parent_ino, const char* name, DeviceId dev) {
    const uint32_t ino = next_ino_++;
    Inode n;
    n.ino        = ino;
    n.type       = InodeType::CharDevice;
    n.dev_id     = dev;
    n.parent_ino = parent_ino;
    n.name       = name;
    n.mode       = 0666;
    if (parent_ino && inodes_.count(parent_ino))
        inodes_[parent_ino].children.push_back(ino);
    inodes_.emplace(ino, std::move(n));
    return ino;
}

void Filesystem::init_vfs() {
    // Inode 1 = root directory.
    {
        Inode root;
        root.ino = 1; root.type = InodeType::Directory;
        root.parent_ino = 0; root.name = "/"; root.mode = 0755;
        inodes_.emplace(1u, std::move(root));
    }

    // Level-1 directories.
    uint32_t bin_ino  = make_dir(1, "bin");
    uint32_t dev_ino  = make_dir(1, "dev");
    uint32_t etc_ino  = make_dir(1, "etc");
    uint32_t home_ino = make_dir(1, "home");
    uint32_t proc_ino = make_dir(1, "proc");   // virtual  -  procfs
    uint32_t sbin_ino = make_dir(1, "sbin");
    uint32_t tmp_ino  = make_dir(1, "tmp");
    uint32_t usr_ino  = make_dir(1, "usr");
    /* uint32_t var_ino  = */ make_dir(1, "var");

    // /dev  -  character devices.
    make_device(dev_ino, "null",    DeviceId::Null);
    make_device(dev_ino, "zero",    DeviceId::Zero);
    make_device(dev_ino, "random",  DeviceId::Random);
    make_device(dev_ino, "urandom", DeviceId::Random);
    make_device(dev_ino, "tty",     DeviceId::Tty);
    make_device(dev_ino, "tty0",    DeviceId::Tty);

    // /home/jade  -  user home.
    uint32_t jade_ino = make_dir(home_ino, "jade");
    make_dir (jade_ino, "Desktop");
    make_dir (jade_ino, "Documents");
    make_dir (jade_ino, "Downloads");
    make_dir (jade_ino, "Projects");
    make_file(jade_ino, "README.md");
    make_file(jade_ino, "about.txt");
    make_file(jade_ino, "skills.txt");
    make_file(jade_ino, "projects.md");
    make_file(jade_ino, "contact.txt");

    // /usr/bin  -  user programs.
    uint32_t usrbin_ino = make_dir(usr_ino, "bin");
    make_file(usrbin_ino, "bash");
    make_file(usrbin_ino, "jade-wm");
    make_file(usrbin_ino, "terminal");
    make_file(usrbin_ino, "skills");
    make_file(usrbin_ino, "projects");
    make_file(usrbin_ino, "about");

    // /bin   -  core utils stubs.
    make_file(bin_ino, "ls");
    make_file(bin_ino, "cat");
    make_file(bin_ino, "ps");
    make_file(bin_ino, "echo");

    // /sbin  -  init.
    make_file(sbin_ino, "init");

    // /etc  -  configuration files.
    make_file(etc_ino, "keybindings.conf");
    make_file(etc_ino, "jadeos.conf");

    // Default keybindings content (stored in flat-file store; editable via nano).
    static constexpr const char KEYBINDINGS_DEFAULT[] =
        "# JadeOS Window Manager Keybindings\n"
        "# Edit with: nano /etc/keybindings.conf\n"
        "# Syntax:  action = ModKey+Key\n"
        "# Modifiers: Alt  Ctrl  Shift  Meta\n"
        "# Note: 'Alt' = Option on macOS.\n"
        "# Alt+Tab is captured by the OS on Windows/Linux so Alt+F1 is used instead.\n"
        "#\n"
        "close_window      = Alt+Q\n"
        "float_window      = Alt+Space\n"
        "maximize_window   = Alt+Return\n"
        "open_launcher     = Alt+F1\n"
        "cycle_focus       = Alt+F1\n"
        "minimize_window   = Alt+H\n"
        "terminal          = (click taskbar icon 1)\n";
    write_file("/etc/keybindings.conf",
               std::string(KEYBINDINGS_DEFAULT, sizeof(KEYBINDINGS_DEFAULT) - 1));

    (void)tmp_ino;
    (void)proc_ino;

    // Mount point registry.
    mounts_ = {
        { "/",     "jadefs",  1         },
        { "/proc", "procfs",  proc_ino  },
        { "/dev",  "devfs",   dev_ino   },
        { "/tmp",  "tmpfs",   tmp_ino   },
    };

#ifdef __EMSCRIPTEN__
    // Load user-persisted files from localStorage, overriding ROM defaults.
    ls_load_all(*this);
    persist_enabled_ = true;
#endif
}

const Inode* Filesystem::stat(uint32_t ino) const {
    auto it = inodes_.find(ino);
    return (it == inodes_.end()) ? nullptr : &it->second;
}

uint32_t Filesystem::lookup(const std::string& path) const {
    if (path == "/" || path.empty()) return 1;
    // Tokenise the path.
    std::vector<std::string> parts;
    std::string tok;
    for (char c : path) {
        if (c == '/') { if (!tok.empty()) { parts.push_back(tok); tok.clear(); } }
        else tok += c;
    }
    if (!tok.empty()) parts.push_back(tok);

    uint32_t cur = 1;
    for (const auto& part : parts) {
        const Inode* d = stat(cur);
        if (!d || d->type != InodeType::Directory) return 0;
        bool found = false;
        for (uint32_t child_ino : d->children) {
            const Inode* c = stat(child_ino);
            if (c && c->name == part) { cur = child_ino; found = true; break; }
        }
        if (!found) return 0;
    }
    return cur;
}

std::vector<std::string> Filesystem::readdir(const std::string& path) const {
    // /proc is virtual  -  generate dynamically.
    if (path == "/proc") {
        std::vector<std::string> out = {
            "self", "thread-self", "version", "cpuinfo", "meminfo", "uptime",
            "loadavg", "mounts",
        };
        if (proc_sched_) {
            auto snap = proc_sched_->ps();
            std::sort(snap.begin(), snap.end(),
                      [](const PsEntry& a, const PsEntry& b) { return a.pid < b.pid; });
            for (const auto& e : snap)
                out.push_back(std::to_string(e.pid) + "/");
        }
        return out;
    }
    const uint32_t ino = lookup(path);
    if (ino == 0) return {};
    const Inode* dir = stat(ino);
    if (!dir || dir->type != InodeType::Directory) return {};
    std::vector<std::string> names;
    for (uint32_t cino : dir->children) {
        const Inode* c = stat(cino);
        if (!c) continue;
        std::string n = c->name;
        if (c->type == InodeType::Directory) n += '/';
        names.push_back(std::move(n));
    }
    return names;
}

std::string Filesystem::dev_read(DeviceId dev, std::size_t len) const {
    switch (dev) {
    case DeviceId::Null:
        return "";  // EOF
    case DeviceId::Zero:
        return std::string(len, '\0');
    case DeviceId::Random: {
        // xorshift32 PRNG  -  deterministic but looks random.
        std::string out(len, '\0');
        for (std::size_t i = 0; i < len; ++i) {
            rand_state_ ^= rand_state_ << 13;
            rand_state_ ^= rand_state_ >> 17;
            rand_state_ ^= rand_state_ << 5;
            out[i] = static_cast<char>(rand_state_ & 0xFF);
        }
        return out;
    }
    case DeviceId::Tty:
        return "";  // reads on /dev/tty block (not implemented)
    default:
        return "";
    }
}

std::string Filesystem::virtual_read(const std::string& path,
                                      const Scheduler& sched,
                                      std::size_t mem_bytes,
                                      uint64_t uptime_ticks) const
{
    char buf[2048];

    if (path == "/proc/version") {
        return "Linux version 6.6.0-jadeos (jade@jadeos) "
               "(gcc 13.2.0) #1 SMP PREEMPT_DYNAMIC\n";
    }

    if (path == "/proc/cpuinfo") {
        return "processor\t: 0\n"
               "vendor_id\t: JadeISA\n"
               "model name\t: JadeOS Custom RISC (8-reg, 32-bit, ring0/ring3)\n"
               "cpu MHz\t\t: 1000.000\n"
               "cache size\t: 256 KB\n"
               "flags\t\t: wasm emulated software-rasterised-gpu\n";
    }

    if (path == "/proc/meminfo") {
        const std::size_t total_kb = mem_bytes / 1024;
        const std::size_t free_kb  = total_kb / 4;
        std::snprintf(buf, sizeof(buf),
            "MemTotal:\t%8zu kB\n"
            "MemFree:\t%8zu kB\n"
            "MemAvailable:\t%8zu kB\n"
            "Buffers:\t%8zu kB\n"
            "Cached:\t\t%8zu kB\n"
            "SwapTotal:\t       0 kB\n"
            "SwapFree:\t       0 kB\n",
            total_kb, free_kb, free_kb + total_kb / 8,
            total_kb / 16, total_kb / 8);
        return buf;
    }

    if (path == "/proc/uptime") {
        const double secs = static_cast<double>(uptime_ticks) / 120.0;
        std::snprintf(buf, sizeof(buf), "%.2f %.2f\n", secs, secs * 0.02);
        return buf;
    }

    if (path == "/proc/loadavg") {
        const double up = static_cast<double>(uptime_ticks) / 120.0;
        const double t  = std::fmod(up, 3600.0) / 3600.0;
        std::snprintf(buf, sizeof(buf), "%.2f %.2f %.2f %zu/%zu 1\n",
            0.05 + 0.02*t, 0.08, 0.07,
            sched.process_count(), sched.process_count() + 2);
        return buf;
    }

    if (path == "/proc/mounts") {
        std::string out;
        for (const auto& m : mounts_) {
            out += m.fstype + " " + m.path + " " + m.fstype + " rw 0 0\n";
        }
        return out;
    }

    // /proc/<pid>/status
    if (path.rfind("/proc/", 0) == 0) {
        const std::string sub = path.substr(6);
        const std::size_t slash = sub.find('/');
        if (slash != std::string::npos) {
            const std::string pidstr = sub.substr(0, slash);
            const std::string rest   = sub.substr(slash + 1);
            uint32_t pid = 0;
            for (char c : pidstr) {
                if (c < '0' || c > '9') { pid = 0xFFFFFFFF; break; }
                pid = pid * 10 + static_cast<uint32_t>(c - '0');
            }
            if (pid != 0xFFFFFFFF && rest == "status") {
                const auto entries = sched.ps();
                for (const auto& e : entries) {
                    if (e.pid == pid) {
                        const char* state_str =
                            (e.state == ProcessState::Running  ? "R (running)"  :
                             e.state == ProcessState::Blocked  ? "S (sleeping)" :
                             e.state == ProcessState::Ready    ? "R (runnable)" :
                                                                 "Z (zombie)");
                        std::snprintf(buf, sizeof(buf),
                            "Name:\t%s\n"
                            "Pid:\t%u\n"
                            "PPid:\t%u\n"
                            "State:\t%s\n"
                            "VmRSS:\t%u kB\n"
                            "VmSize:\t%u kB\n"
                            "Threads:\t1\n"
                            "SigPnd:\t%08x\n"
                            "SigBlk:\t00000000\n"
                            "nice:\t%d\n",
                            e.name.c_str(), e.pid, e.ppid,
                            state_str, e.mem_kb, e.mem_kb * 2,
                            e.pending_signals,
                            static_cast<int>(e.nice));
                        return buf;
                    }
                }
                return "cat: /proc/" + pidstr + "/status: No such file\n";
            }
            // /proc/<pid>/maps  -  virtual memory layout stub
            if (pid != 0xFFFFFFFF && rest == "maps") {
                const auto entries = sched.ps();
                for (const auto& e : entries) {
                    if (e.pid == pid) {
                        std::snprintf(buf, sizeof(buf),
                            "00001000-00002000 r-xp 00000000 00:00 0  [text]\n"
                            "00002000-00003000 r--p 00001000 00:00 0  [rodata]\n"
                            "00003000-00004000 rw-p 00002000 00:00 0  [data]\n"
                            "%08x-%08x rw-p 00000000 00:00 0  [stack]\n",
                            0x80000000u - e.mem_kb * 1024u,
                            0x80000000u);
                        return buf;
                    }
                }
                return "";
            }
        }
    }

    return "";
}

std::uint64_t Filesystem::jadefs_used_kb() const noexcept {
    std::size_t bytes = 0;
    for (const auto& pr : files_)
        bytes += pr.second.data.size();
    bytes += inodes_.size() * 256u;
    return static_cast<std::uint64_t>((bytes + 1023) / 1024);
}

}  // namespace os
