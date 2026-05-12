#pragma once
#include "../cpu/cpu.hpp"
#include "../cpu/memory.hpp"
#include "../gpu/gpu.hpp"
#include "scheduler.hpp"
#include "filesystem.hpp"
#include "wm.hpp"
#include "terminal.hpp"
#include "apps/calculator.hpp"
#include "apps/fileexplorer.hpp"
#include "apps/taskmanager.hpp"
#ifdef __EMSCRIPTEN__
#include "apps/doom_port.hpp"
#include "apps/media_player.hpp"
#endif
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace os {

// Syscall numbers follow Linux ABI where possible (see /proc-facing strings).
enum class Syscall : uint8_t {
    YIELD        = 0x00,
    WRITE_STR    = 0x01,  // R0 = RAM addr of NUL-terminated string (legacy)
    READ         = 0x03,  // R0=fd R1=buf R2=len; result in R0=bytes_read
    WRITE_FD     = 0x04,  // R0=fd R1=buf R2=len; result in R0=bytes_written
    OPEN         = 0x05,  // R0=path; result in R0=fd or -1
    CLOSE        = 0x06,  // R0=fd; result R0=0
    MMAP         = 0x09,  // R0=len; result R0=phys_addr (page allocator)
    MUNMAP       = 0x0B,  // R0=addr R1=len
    GPU_CMD      = 0x10,  // R0=Command  R1-R6=args
    SPAWN        = 0x20,
    GETPID       = 0x27,  // result R0=current_pid
    CLONE        = 0x38,
    FORK         = 0x39,  // stub: not on WASM; handler sets R0=-1
    KILL         = 0x3E,  // R0=pid  R1=sig
    GETTIMEOFDAY = 0x60,
    EXIT         = 0xFF,
    EXIT_GROUP   = 0xE7,
};

enum class FdType : uint8_t { None, VfsFile, Tty, DevNull, DevZero, DevRandom };

struct FileDescriptor {
    FdType   type    = FdType::None;
    uint32_t ino     = 0;      // inode number (for VfsFile)
    uint32_t offset  = 0;      // current read position
    bool     readable = true;
    bool     writable = false;
};

class Kernel {
public:
    Kernel();

    void boot(uint32_t mem_mib = 8,
              std::vector<uint8_t> ttf_data = {},
              uint32_t fb_w = gpu::FB_W_DEFAULT,
              uint32_t fb_h = gpu::FB_H_DEFAULT,
              float dpr = 1.0f);

    void tick(uint64_t cpu_cycles_per_tick);

    cpu::Memory&  memory()    { return memory_; }
    cpu::CPU&     cpu()       { return cpu_;    }
    gpu::GPU&     gpu()       { return gpu_;    }
    Scheduler&    scheduler() { return sched_;  }
    Filesystem&   fs()        { return fs_;     }
    Terminal&     terminal()  noexcept { return terminal_; }

    bool is_running() const { return running_; }

    void send_mousedown(uint32_t x, uint32_t y);
    void send_mousemove(uint32_t x, uint32_t y);
    void send_mouseup();
    void send_scroll(int delta);
    void send_key(uint32_t keycode, uint32_t charcode);

    void set_wall_time(uint32_t unix_sec) noexcept;
    void resize(uint32_t fb_w, uint32_t fb_h) noexcept;

    // Deliver a signal to a running process (from WM or external trigger).
    void send_signal(uint32_t pid, int sig) noexcept;

    void mark_terminal_dirty() noexcept;

#ifdef __EMSCRIPTEN__
    void set_freedoom_iwad_ready(bool v) noexcept { doom_port_.set_iwad_ready(v); }
    void set_media_assets_ready(bool v) noexcept { media_player_.set_fs_ready(v); }
    void set_media_clip_ready(bool v) noexcept { media_player_.set_clip_fs_ready(v); }
    void send_mouse_game(int32_t dx, int32_t dy, uint32_t buttons) noexcept;
    [[nodiscard]] bool doom_pointer_lock_desired() const noexcept;
#endif

private:
    static constexpr int FD_MAX = 256;

    cpu::Memory       memory_;
    cpu::CPU          cpu_;
    gpu::GPU          gpu_;
    cpu::PageAllocator page_alloc_;
    Scheduler         sched_;
    Filesystem        fs_;
    WindowManager     wm_;
    Terminal          terminal_;
    bool              running_         = false;
    uint32_t          tick_count_      = 0;
    int               projects_scroll_ = 0;
    int               about_scroll_    = 0;
    float             dpr_             = 1.0f;

    int               term_win_id_     = -1;

    // Window IDs (set in load_init_program; used for dirty marking).
    int               proj_win_id_     = -1;
    int               about_win_id_    = -1;
    int               clock_win_id_    = -1;
    int               calc_win_id_     = -1;
    int               files_win_id_    = -1;
    int               tasks_win_id_    = -1;
    int               wasm_win_id_     = -1;

#ifdef __EMSCRIPTEN__
    int               doom_win_id_     = -1;
    int               media_win_id_    = -1;
#endif

    // Per-app state (non-trivial apps only).
    apps::CalculatorState   calc_state_;
    apps::FileExplorerState files_state_;
    apps::TaskManagerState  tasks_state_;
#ifdef __EMSCRIPTEN__
    DoomPort          doom_port_;
    MediaPlayerApp    media_player_;
#endif

    // Open file descriptor table (fd 0/1/2 = stdin/stdout/stderr = Tty).
    std::array<FileDescriptor, FD_MAX> fd_table_{};

    // Allocate the next free fd slot.  Returns -1 if table full.
    int alloc_fd() noexcept;

    void install_syscalls();
    void load_init_program();
};

}  // namespace os
