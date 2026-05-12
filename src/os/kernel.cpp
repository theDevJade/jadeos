#include "kernel.hpp"
#include "../audio/jade_audio.hpp"
#include "../gpu/framebuffer.hpp"
#include "apps/clock.hpp"
#include "apps/calculator.hpp"
#include "apps/fileexplorer.hpp"
#include "apps/taskmanager.hpp"
#include "apps/wasminfo.hpp"
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <string>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// Background clear colour: 32-bit 0xAARRGGBB (same layout as gpu::RGBA::from_u32).
static constexpr uint32_t COL_BG = 0xFF'08'0F'18;

namespace os {

// Fake boot ROM: CLEAR to COL_BG via GPU syscall 0x10, then HALT.
static constexpr uint8_t INIT_ROM[] = {
    static_cast<uint8_t>(cpu::Opcode::LOAD), 0x00,
    0x01, 0x00, 0x00, 0x00,
    static_cast<uint8_t>(cpu::Opcode::LOAD), 0x01,
    // Little-endian u32 for COL_BG: bytes are B, G, R, A (0x18,0x0F,0x08,0xFF -> 0xFF080F18).
    0x18, 0x0F, 0x08, 0xFF,
    static_cast<uint8_t>(cpu::Opcode::INT), 0x10,
    static_cast<uint8_t>(cpu::Opcode::HALT),
};

Kernel::Kernel()
    : memory_(), cpu_(memory_), gpu_(memory_),
      wm_(gpu_, gpu::FB_W_DEFAULT, gpu::FB_H_DEFAULT)
{}

void Kernel::boot(uint32_t mem_mib, std::vector<uint8_t> ttf_data,
                  uint32_t fb_w, uint32_t fb_h, float dpr) {
    gpu_.init(fb_w, fb_h);
    dpr_ = std::max(0.5f, std::min(dpr, 4.0f));
    wm_.set_screen_size(static_cast<int>(fb_w), static_cast<int>(fb_h));
    wm_.set_dpr(dpr_);

    std::size_t mem_bytes = static_cast<std::size_t>(mem_mib) * 1024u * 1024u;
    const std::size_t min_ram = gpu::min_ram_bytes(fb_w, fb_h);
    if (mem_bytes < min_ram) mem_bytes = min_ram;
    memory_.reset(mem_bytes);
    cpu_.reset();

    cpu_.set_pc(0x1000);
    memory_.load(0x1000, std::span<const uint8_t>(INIT_ROM, sizeof(INIT_ROM)));

    if (!ttf_data.empty())
        gpu_.load_font(ttf_data, dpr_);

#ifdef __EMSCRIPTEN__
    jade::audio::init(48000, 4);
    doom_port_.set_context(&gpu_, &memory_);
    media_player_.set_context(&gpu_, &memory_);
#endif

    const std::size_t kernel_reserved = 256 * 1024;
    if (mem_bytes > kernel_reserved)
        page_alloc_.init(static_cast<uint32_t>(kernel_reserved),
                         static_cast<uint32_t>(mem_bytes - kernel_reserved));

    fs_.init_vfs();
    fs_.set_proc_scheduler(&sched_);

    fd_table_[0] = { FdType::Tty, 0, 0, true,  false };
    fd_table_[1] = { FdType::Tty, 0, 0, false, true  };
    fd_table_[2] = { FdType::Tty, 0, 0, false, true  };

    terminal_.configure(&sched_, &fs_, mem_bytes, dpr_, &wm_, &tick_count_);

    install_syscalls();
    load_init_program();
    running_ = true;
}

void Kernel::install_syscalls() {
    // x86 vector numbers: #DE 0, #UD 6, #GP 13, #PF 14. Handlers reset PC to 0.
    cpu_.set_interrupt_handler(0,
        [](uint8_t, cpu::RegisterFile& r, cpu::Memory&) { r.pc = 0; });
    cpu_.set_interrupt_handler(6,
        [](uint8_t, cpu::RegisterFile& r, cpu::Memory&) { r.pc = 0; });
    cpu_.set_interrupt_handler(13,
        [](uint8_t, cpu::RegisterFile& r, cpu::Memory&) { r.pc = 0; });
    cpu_.set_interrupt_handler(14,
        [](uint8_t, cpu::RegisterFile& r, cpu::Memory&) { r.pc = 0; });

    cpu_.set_interrupt_handler(
        static_cast<uint8_t>(Syscall::YIELD),
        [this](uint8_t, cpu::RegisterFile&, cpu::Memory&) { sched_.tick(); }
    );
    cpu_.set_interrupt_handler(
        static_cast<uint8_t>(Syscall::GPU_CMD),
        [this](uint8_t, cpu::RegisterFile& regs, cpu::Memory&) {
            gpu::CmdPacket pkt;
            pkt.cmd    = static_cast<gpu::Command>(regs.get(cpu::Reg::R0));
            pkt.args[0]= regs.get(cpu::Reg::R1);
            pkt.args[1]= regs.get(cpu::Reg::R2);
            pkt.args[2]= regs.get(cpu::Reg::R3);
            pkt.args[3]= regs.get(cpu::Reg::R4);
            pkt.args[4]= regs.get(cpu::Reg::R5);
            pkt.args[5]= regs.get(cpu::Reg::R6);
            gpu_.push_command(pkt);
        }
    );
    cpu_.set_interrupt_handler(
        static_cast<uint8_t>(Syscall::EXIT),
        [this](uint8_t, cpu::RegisterFile&, cpu::Memory&) { running_ = false; }
    );

    cpu_.set_interrupt_handler(
        static_cast<uint8_t>(Syscall::OPEN),
        [this](uint8_t, cpu::RegisterFile& regs, cpu::Memory& mem) {
            uint32_t addr = regs.get(cpu::Reg::R0);
            std::string path;
            for (uint32_t i = 0; i < 4096; ++i) {
                uint8_t c = mem.read8(addr + i);
                if (!c) break;
                path += static_cast<char>(c);
            }
            int fd = alloc_fd();
            if (fd < 0) { regs.set(cpu::Reg::R0, ~0u); return; }
            if      (path == "/dev/null")    fd_table_[fd] = { FdType::DevNull,   0, 0, true, true  };
            else if (path == "/dev/zero")    fd_table_[fd] = { FdType::DevZero,   0, 0, true, true  };
            else if (path == "/dev/random"
                  || path == "/dev/urandom") fd_table_[fd] = { FdType::DevRandom, 0, 0, true, false };
            else if (path == "/dev/tty"
                  || path == "/dev/tty0")    fd_table_[fd] = { FdType::Tty,       0, 0, true, true  };
            else {
                uint32_t ino = fs_.lookup(path);
                if (ino == 0) { fd_table_[fd] = {}; regs.set(cpu::Reg::R0, ~0u); return; }
                fd_table_[fd] = { FdType::VfsFile, ino, 0, true, false };
            }
            regs.set(cpu::Reg::R0, static_cast<uint32_t>(fd));
        }
    );
    cpu_.set_interrupt_handler(
        static_cast<uint8_t>(Syscall::CLOSE),
        [this](uint8_t, cpu::RegisterFile& regs, cpu::Memory&) {
            const uint32_t fd = regs.get(cpu::Reg::R0);
            if (fd < FD_MAX) fd_table_[fd] = {};
            regs.set(cpu::Reg::R0, 0);
        }
    );
    cpu_.set_interrupt_handler(
        static_cast<uint8_t>(Syscall::READ),
        [this](uint8_t, cpu::RegisterFile& regs, cpu::Memory& mem) {
            const uint32_t fd  = regs.get(cpu::Reg::R0);
            const uint32_t buf = regs.get(cpu::Reg::R1);
            const uint32_t len = regs.get(cpu::Reg::R2);
            if (fd >= FD_MAX || !fd_table_[fd].readable) {
                regs.set(cpu::Reg::R0, ~0u); return;
            }
            switch (fd_table_[fd].type) {
            case FdType::DevNull:   regs.set(cpu::Reg::R0, 0); break;
            case FdType::DevZero:
                for (uint32_t i = 0; i < len; ++i) mem.write8(buf + i, 0);
                regs.set(cpu::Reg::R0, len); break;
            case FdType::DevRandom: {
                for (uint32_t i = 0; i < len; ++i)
                    mem.write8(buf + i, static_cast<uint8_t>(i * 0x6C + 0xB5));
                regs.set(cpu::Reg::R0, len); break;
            }
            default: regs.set(cpu::Reg::R0, 0); break;
            }
        }
    );
    cpu_.set_interrupt_handler(
        static_cast<uint8_t>(Syscall::WRITE_FD),
        [](uint8_t, cpu::RegisterFile& regs, cpu::Memory&) {
            const uint32_t len = regs.get(cpu::Reg::R2);
            regs.set(cpu::Reg::R0, len);
        }
    );
    cpu_.set_interrupt_handler(
        static_cast<uint8_t>(Syscall::MMAP),
        [this](uint8_t, cpu::RegisterFile& regs, cpu::Memory&) {
            const uint32_t len = regs.get(cpu::Reg::R0);
            const uint32_t n   = (len + cpu::PageAllocator::kPageSize - 1)
                                 / cpu::PageAllocator::kPageSize;
            regs.set(cpu::Reg::R0, page_alloc_.alloc(n));
        }
    );
    cpu_.set_interrupt_handler(
        static_cast<uint8_t>(Syscall::MUNMAP),
        [this](uint8_t, cpu::RegisterFile& regs, cpu::Memory&) {
            const uint32_t addr = regs.get(cpu::Reg::R0);
            const uint32_t len  = regs.get(cpu::Reg::R1);
            const uint32_t n    = (len + cpu::PageAllocator::kPageSize - 1)
                                  / cpu::PageAllocator::kPageSize;
            page_alloc_.free(addr, n);
            regs.set(cpu::Reg::R0, 0);
        }
    );
    cpu_.set_interrupt_handler(
        static_cast<uint8_t>(Syscall::GETPID),
        [this](uint8_t, cpu::RegisterFile& regs, cpu::Memory&) {
            regs.set(cpu::Reg::R0, sched_.current_pid());
        }
    );
    cpu_.set_interrupt_handler(
        static_cast<uint8_t>(Syscall::KILL),
        [this](uint8_t, cpu::RegisterFile& regs, cpu::Memory&) {
            const uint32_t pid = regs.get(cpu::Reg::R0);
            const int      sig = static_cast<int>(regs.get(cpu::Reg::R1));
            sched_.send_signal(pid, sig);
            regs.set(cpu::Reg::R0, 0);
        }
    );
    cpu_.set_interrupt_handler(
        static_cast<uint8_t>(Syscall::FORK),
        [](uint8_t, cpu::RegisterFile& regs, cpu::Memory&) {
            regs.set(cpu::Reg::R0, ~0u);
        }
    );
}

int Kernel::alloc_fd() noexcept {
    for (int i = 3; i < FD_MAX; ++i)
        if (fd_table_[i].type == FdType::None) return i;
    return -1;
}

void Kernel::send_signal(uint32_t pid, int sig) noexcept {
    sched_.send_signal(pid, sig);
}

namespace {

void push(gpu::GPU& g, gpu::Command cmd,
          uint32_t a0=0,uint32_t a1=0,uint32_t a2=0,
          uint32_t a3=0,uint32_t a4=0,uint32_t a5=0)
{
    gpu::CmdPacket pkt;
    pkt.cmd  = cmd;
    pkt.args = { a0, a1, a2, a3, a4, a5 };
    g.push_command(pkt);
}

void text(gpu::GPU& g, int x, int y, uint32_t col, uint8_t sz,
          const std::string& s)
{
    gpu::TextRequest r;
    r.x = static_cast<float>(x);
    r.y = static_cast<float>(y);
    r.colour = gpu::RGBA::from_u32(col);
    r.font_size_id = sz;
    r.text = s;
    g.draw_text(r);
}

struct Project {
    const char* name; uint32_t col;
    const char* desc; const char* tech; const char* url;
};
constexpr Project PROJS[] = {
    { "JadeOS",
      0xFF'2A'90'CF,
      "Custom RISC CPU + software-rasterised GPU running in your browser.",
      "C++20  Emscripten  WebAssembly  Meson",
      "github.com/theDevJade/jadeos" },
    { "Mixtape",
      0xFF'F0'70'20,
      "Open source music player for all platforms, written in Flutter.",
      "Flutter  Rust  x86-64  steamvr",
      "github.com/theDevJade/mixtape" },
    { "Flow",
      0xFF'20'B0'50,
      "A programming language with JIT compilation, transpilation, and a language server.",
      "Rust  Cranelift  compiled-lang",
      "github.com/flowlangteam/flow" },
    { "Labber",
      0xFF'A0'40'D0,
      "PXE lab boot server (DHCP + TFTP + HTTP + registry + dashboard).",
      "C++ CMake  Linux  sockets",
      "github.com/theDevJade/labber" }
};
constexpr int N_PROJS = static_cast<int>(sizeof(PROJS) / sizeof(PROJS[0]));

struct SkillRow {
    const char* name;
    const char* level;
    const char* years;
    const char* context;
    uint32_t    col;
};
static constexpr SkillRow SKILL_ROWS[] = {
    { "C++20 / C",   "Expert",     "7 yrs", "OS kernels, emulators, compilers, bare-metal", 0xFF'2A'90'CF },
    { "Rust",        "Advanced",   "5 yrs", "Systems tooling, WebAssembly, unsafe FFI",     0xFF'F0'70'20 },
    { "WebAssembly", "Advanced",     "3 yrs", "Emscripten, WASI, custom toolchains",            0xFF'80'40'D0 },
    { "GLSL / WGSL", "Proficient", "5 yrs", "Shaders, graphics, jazz",      0xFF'20'B0'50 },
    { "TypeScript",  "Expert", "7 yrs", "Frontend tooling, build systems, WebGPU",        0xFF'30'80'D0 },
    { "Dart",  "Proficient", "4 yrs", "Flutter, applications",        0xFF'30'80'D0 },
    { "Go",  "Expert", "4 yrs", "Backend APIs",        0xFF'30'80'D0 }
};

static constexpr const char* ABOUT_LINES[] = {
    "Systems programmer & CS enjoyer.",
    "Obsessed with the lowest level things: graphics, OS kernels, and demolishing school IT departments.",
    "OS kernels, CPU emulators, compilers,",
    "GPU rasterizers, and anything that touches bare metal.",
    "Vulkan enjoyer, and number 1 enemy.",
    "",
    "Currently: Knull + Vixen, a huge space game written in scratch in Rust,",
    " and Mixtape, an open source music player for all platforms written in Flutter."
};

struct ContactRow {
    const char* label;
    const char* val;
    uint32_t    col;
};
static constexpr ContactRow CONTACT_ROWS[] = {
    { "Email",  "kotlindevjade@gmail.com",    0xFF'B8'D8'F0 },
    { "GitHub", "github.com/theDevJade",     0xFF'2A'90'CF },
    { "Web",    "alyx.jet-ware.com",            0xFF'60'B0'E0 },
    { "Discord", "aballofnewsies",    0xFF'50'A0'D0 },
};

static void populate_portfolio_home(Filesystem& fs) {
    fs.write_file("/home/jade/README.md",
        "# JadeOS\n\n"
        "Custom RISC CPU + software-rasterised GPU portfolio OS.\n"
        "Runs in the browser via WebAssembly (Emscripten, Meson).\n\n"
        "Open the Skills, Projects, or About windows, or read the files in ~/.\n");
    std::string about;
    for (const char* ln : ABOUT_LINES) {
        if (!about.empty()) about += '\n';
        about += ln;
    }
    fs.write_file("/home/jade/about.txt", about);
    std::string skills;
    for (const auto& sk : SKILL_ROWS) {
        skills += sk.name;
        skills += "  |  ";
        skills += sk.level;
        skills += "  |  ";
        skills += sk.years;
        skills += "\n  ";
        skills += sk.context;
        skills += "\n\n";
    }
    fs.write_file("/home/jade/skills.txt", skills);
    std::string pm = "# Projects\n\n";
    for (const auto& p : PROJS) {
        pm += "## ";
        pm += p.name;
        pm += "\n";
        pm += p.desc;
        pm += "\n\n`";
        pm += p.tech;
        pm += "`\n\nhttps://";
        pm += p.url;
        pm += "\n\n";
    }
    fs.write_file("/home/jade/projects.md", pm);
    std::string ct;
    for (const auto& c : CONTACT_ROWS) {
        ct += c.label;
        ct += ":  ";
        ct += c.val;
        ct += '\n';
    }
    fs.write_file("/home/jade/contact.txt", ct);
}

} // anon ns

static void render_skills(gpu::GPU& g, os::WinRect area, uint32_t /*frame*/, float dpr)
{
    auto sc = [dpr](int n) { return int(n * dpr + 0.5f); };
    const int x0 = area.x, y0 = area.y, W = area.w, H = area.h;

    push(g, gpu::Command::DRAW_RECT,
         static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
         static_cast<uint32_t>(W),  static_cast<uint32_t>(H), 0xFF'06'0E'1A);

    text(g, x0+sc(14), y0+sc(36), 0xFF'E8'F8'FF, 2, "Jade");
    text(g, x0+sc(14), y0+sc(56), 0xFF'2A'90'CF, 0,
         "Systems Programmer  |  C++20  |  WebAssembly  |  Embedded");
    push(g, gpu::Command::DRAW_LINE,
         static_cast<uint32_t>(x0+sc(14)),   static_cast<uint32_t>(y0+sc(68)),
         static_cast<uint32_t>(x0+W-sc(14)), static_cast<uint32_t>(y0+sc(68)),
         0xFF'1A'3A'55);

    text(g, x0+sc(14), y0+sc(88), 0xFF'50'C8'FF, 1, "SKILLS");

    const int col_level = x0 + sc(170);
    const int col_years = x0 + sc(258);

    const int row_h = sc(38);
    int sy = y0 + sc(102);

    for (const auto& sk : SKILL_ROWS) {
        if (sy + row_h > y0 + H) break;
        push(g, gpu::Command::DRAW_RECT,
             static_cast<uint32_t>(x0+sc(8)),  static_cast<uint32_t>(sy+sc(2)),
             static_cast<uint32_t>(sc(3)),      static_cast<uint32_t>(sc(28)), sk.col);
        text(g, x0+sc(18), sy+sc(14), 0xFF'C8'E8'FF, 0, sk.name);
        if (col_level < x0 + W - sc(70))
            text(g, col_level, sy+sc(14), sk.col, 0, sk.level);
        if (col_years < x0 + W - sc(40))
            text(g, col_years, sy+sc(14), 0xFF'70'A0'C0, 0, sk.years);
        text(g, x0+sc(18), sy+sc(28), 0xFF'35'60'80, 0, sk.context);
        sy += row_h;
    }
}

static void render_projects(gpu::GPU& g, os::WinRect area,
                            uint32_t /*frame*/, int scroll, float dpr)
{
    auto sc = [dpr](int n) { return int(n * dpr + 0.5f); };
    const int x0 = area.x, y0 = area.y;
    const int W = area.w, H = area.h;
    const int clip_b = y0 + H;

    push(g, gpu::Command::DRAW_RECT,
         static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
         static_cast<uint32_t>(W),  static_cast<uint32_t>(H), 0xFF'06'0E'1A);

    text(g, x0+sc(14), y0+sc(24), 0xFF'50'C8'FF, 1, "PROJECTS");
    text(g, x0+W-sc(86), y0+sc(24), 0xFF'1E'40'60, 0, "click to open");
    push(g, gpu::Command::DRAW_LINE,
         static_cast<uint32_t>(x0+sc(14)),   static_cast<uint32_t>(y0+sc(34)),
         static_cast<uint32_t>(x0+W-sc(14)), static_cast<uint32_t>(y0+sc(34)),
         0xFF'1A'3A'55);

    const int card_h = sc(80);
    int vy = sc(42);
    for (const auto& p : PROJS) {
        const int ry = y0 + vy - scroll;

        if (ry + card_h > y0 && ry < clip_b) {
            push(g, gpu::Command::DRAW_RECT,
                 static_cast<uint32_t>(x0+sc(14)), static_cast<uint32_t>(ry+sc(4)),
                 static_cast<uint32_t>(sc(3)), static_cast<uint32_t>(card_h - sc(10)), p.col);
            if (ry + sc(20) >= y0 && ry + sc(20) < clip_b)
                text(g, x0+sc(24), ry+sc(20), p.col, 1, p.name);
            if (ry + sc(38) >= y0 && ry + sc(38) < clip_b)
                text(g, x0+sc(24), ry+sc(38), 0xFF'90'B8'D0, 0, p.desc);
            if (ry + sc(52) >= y0 && ry + sc(52) < clip_b)
                text(g, x0+sc(24), ry+sc(52), 0xFF'50'78'90, 0, p.tech);
            if (ry + sc(66) >= y0 && ry + sc(66) < clip_b)
                text(g, x0+sc(24), ry+sc(66), 0xFF'40'80'C8, 0, p.url);
        }

        vy += card_h;
        const int div_y = y0 + vy - scroll - sc(4);
        if (div_y >= y0 && div_y < clip_b)
            push(g, gpu::Command::DRAW_LINE,
                 static_cast<uint32_t>(x0+sc(14)), static_cast<uint32_t>(div_y),
                 static_cast<uint32_t>(x0+W-sc(14)), static_cast<uint32_t>(div_y),
                 0xFF'0D'1E'30);
    }
}

static void render_about(gpu::GPU& g, os::WinRect area, int scroll, float dpr)
{
    auto sc = [dpr](int n) { return int(n * dpr + 0.5f); };
    const int x0 = area.x, y0 = area.y;
    const int W = area.w, H = area.h;
    const int clip_b = y0 + H;

    push(g, gpu::Command::DRAW_RECT,
         static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
         static_cast<uint32_t>(W),  static_cast<uint32_t>(H), 0xFF'06'0E'1A);

    auto ry = [&](int vy) { return y0 + vy - scroll; };
    auto vis = [&](int vy) { return ry(vy) >= y0 && ry(vy) < clip_b; };

    if (vis(sc(24))) text(g, x0+sc(14), ry(sc(24)), 0xFF'50'C8'FF, 1, "ABOUT");
    if (vis(sc(38))) push(g, gpu::Command::DRAW_LINE,
                          static_cast<uint32_t>(x0+sc(14)), static_cast<uint32_t>(ry(sc(38))),
                          static_cast<uint32_t>(x0+W-sc(14)), static_cast<uint32_t>(ry(sc(38))),
                          0xFF'1A'3A'55);

    int vy = sc(50);
    for (const char* ln : ABOUT_LINES) {
        if (vis(vy)) text(g, x0+sc(14), ry(vy), 0xFF'90'B8'D0, 0, ln);
        vy += sc(18);
    }

    vy += sc(8);
    if (vis(vy))   text(g, x0+sc(14), ry(vy), 0xFF'50'C8'FF, 1, "CONTACT");
    vy += sc(24);
    if (vis(vy))   push(g, gpu::Command::DRAW_LINE,
                         static_cast<uint32_t>(x0+sc(14)), static_cast<uint32_t>(ry(vy)),
                         static_cast<uint32_t>(x0+W-sc(14)), static_cast<uint32_t>(ry(vy)),
                         0xFF'1A'3A'55);
    vy += sc(10);

    for (const auto& c : CONTACT_ROWS) {
        if (vis(vy)) {
            text(g, x0+sc(14), ry(vy), 0xFF'3A'6A'8A, 0, c.label);
            text(g, x0+sc(80), ry(vy), c.col,          0, c.val);
        }
        vy += sc(20);
    }
}

void Kernel::load_init_program() {
    populate_portfolio_home(fs_);
    const std::size_t mem_mib = memory_.size() / (1024 * 1024);
    char boot_buf[160];

    terminal_.print("");
    terminal_.print("[    0.000] JadeOS 1.0.0 - Custom RISC/Software-GPU OS");
    std::snprintf(boot_buf, sizeof(boot_buf),
        "[    0.001] init_mm: %zu MiB RAM, %u pages (%u used)",
        mem_mib, page_alloc_.total_pages(), page_alloc_.used_pages());
    terminal_.print(boot_buf);
    terminal_.print("[    0.002] init_cpu: JadeISA 8-GPR 32-bit, ring0/ring3, vecs 0/6/13/14");
    terminal_.print("[    0.003] init_vfs: jadefs / | procfs /proc | devfs /dev | tmpfs /tmp");
    terminal_.print("[    0.004] init_sched: SCHED_NORMAL, quanta 4t, prio nice-20..+19");
    terminal_.print("[    0.005] init_ipc: signals SIGHUP/SIGINT/SIGKILL/SIGTERM/SIGCHLD");
    terminal_.print("[    0.006] init_fd: fd[0]=stdin fd[1]=stdout fd[2]=stderr (/dev/tty)");
    terminal_.print("[    0.007] Starting /sbin/init (PID 1) -> jade-wm -> session manager");
    terminal_.print("[    0.008] I have no mouth and I must scream.");
    terminal_.print("[    0.009] Boot complete.  Type 'help' for commands.");
    terminal_.print("");

    sched_.spawn("[kernel]",  "[kthreadd]", 0, 0,    4096, {});
    sched_.spawn("systemd",   "/sbin/init", 0, 1,    8192, {});

    term_win_id_ = wm_.add_window(
        { 0, 0, 0, 0 },
        "terminal  jade@jadeos",
        [this](gpu::GPU& g, os::WinRect area) {
            terminal_.render(g, area, tick_count_);
        },
        {},
        [this](uint32_t kc, uint32_t ch) {
            terminal_.send_key(kc, ch);
        }
    );

    wm_.add_window(
        { 0, 0, 0, 0 },
        "skills.app",
        [this](gpu::GPU& g, os::WinRect area) {
            render_skills(g, area, tick_count_, dpr_);
        }
    );

    proj_win_id_ = wm_.add_window(
        { 0, 0, 0, 0 },
        "projects.app",
        [this](gpu::GPU& g, os::WinRect area) {
            render_projects(g, area, tick_count_, projects_scroll_, dpr_);
        },
        [this](int /*cx*/, int cy) {
            // start vy=sc(42), card_h=sc(80), scroll projects_scroll_.
            const int card_h = static_cast<int>(80 * dpr_ + 0.5f);
            const int start  = static_cast<int>(42 * dpr_ + 0.5f);
            const int rel    = cy + projects_scroll_ - start;
            if (rel < 0) return;
            const int idx = rel / card_h;
            if (idx < 0 || idx >= N_PROJS) return;
#ifdef __EMSCRIPTEN__
            const std::string url = std::string("https://") + PROJS[idx].url;
            EM_ASM({
                window.open(UTF8ToString($0), '_blank');
            }, url.c_str());
#endif
        },
        {},
        [this](int delta) {
            const int step  = static_cast<int>(20 * dpr_ + 0.5f);
            const int max_s = N_PROJS * static_cast<int>(80 * dpr_ + 0.5f);
            const int s = projects_scroll_ + delta * step;
            projects_scroll_ = (s < 0) ? 0 : (s > max_s) ? max_s : s;
            wm_.mark_dirty(proj_win_id_);
        }
    );

    about_win_id_ = wm_.add_window(
        { 0, 0, 0, 0 },
        "about.app",
        [this](gpu::GPU& g, os::WinRect area) {
            render_about(g, area, about_scroll_, dpr_);
        },
        {},
        {},
        [this](int delta) {
            const int s = about_scroll_ + delta * 16;
            about_scroll_ = (s < 0) ? 0 : (s > 300) ? 300 : s;
            wm_.mark_dirty(about_win_id_);
        }
    );

    wm_.focus(3);

    clock_win_id_ = wm_.add_window(
        { 0, 0, 0, 0 },
        "clock.app",
        [this](gpu::GPU& g, os::WinRect area) {
            apps::render_clock(g, area,
                wm_.wall_hour(), wm_.wall_min(), wm_.wall_sec(),
                tick_count_, dpr_);
        }
    );
    wm_.toggle_minimize(clock_win_id_);

    calc_win_id_ = wm_.add_window(
        { 0, 0, 0, 0 },
        "calculator.app",
        [this](gpu::GPU& g, os::WinRect area) {
            apps::render_calculator(g, area, calc_state_, dpr_);
        },
        [this](int cx, int cy) {
            const os::WinRect cr = wm_.content_rect_for(calc_win_id_);
            const os::WinRect rel = {0, 0, cr.w, cr.h};
            if (apps::click_calculator(cx, cy, rel, calc_state_, dpr_))
                wm_.mark_dirty(calc_win_id_);
        }
    );
    wm_.toggle_minimize(calc_win_id_);

    files_win_id_ = wm_.add_window(
        { 0, 0, 0, 0 },
        "files.app",
        [this](gpu::GPU& g, os::WinRect area) {
            apps::render_fileexplorer(g, area, files_state_, dpr_);
        },
        [this](int cx, int cy) {
            const os::WinRect cr = wm_.content_rect_for(files_win_id_);
            const os::WinRect rel = {0, 0, cr.w, cr.h};
            if (apps::click_fileexplorer(cx, cy, rel,
                                          files_state_, fs_, dpr_))
                wm_.mark_dirty(files_win_id_);
        },
        {},
        [this](int delta) {
            if (apps::scroll_fileexplorer(delta, files_state_, fs_))
                wm_.mark_dirty(files_win_id_);
        }
    );
    {
        auto entries = fs_.readdir("/home/jade");
        files_state_.entries.assign(entries.begin(), entries.end());
    }
    wm_.toggle_minimize(files_win_id_);

    tasks_win_id_ = wm_.add_window(
        { 0, 0, 0, 0 },
        "taskman.app",
        [this](gpu::GPU& g, os::WinRect area) {
            apps::render_taskmanager(g, area, sched_.all_processes(),
                                     memory_.size(), tick_count_,
                                     tasks_state_, dpr_);
        }
    );
    wm_.toggle_minimize(tasks_win_id_);

    wasm_win_id_ = wm_.add_window(
        { 0, 0, 0, 0 },
        "wasminfo.app",
        [this](gpu::GPU& g, os::WinRect area) {
            apps::render_wasminfo(g, area,
                cpu_.total_cycles(), gpu_.draw_calls(),
                gpu_.framebuffer_width(), gpu_.framebuffer_height(),
                memory_.size(), tick_count_, dpr_);
        }
    );
    wm_.toggle_minimize(wasm_win_id_);

    sched_.spawn("clock",       "/usr/bin/clock",       2, 7,  512,  {});
    sched_.spawn("calculator",  "/usr/bin/calculator",  2, 8,  1024, {});
    sched_.spawn("files",       "/usr/bin/files",       2, 9,  2048, {});
    sched_.spawn("taskmanager", "/usr/bin/taskmanager", 2, 10, 1024, {});
    sched_.spawn("wasminfo",    "/usr/bin/wasminfo",    2, 11, 512,  {});

#ifdef __EMSCRIPTEN__
    doom_win_id_ = wm_.add_window(
        { 0, 0, 0, 0 },
        "freedoom.app",
        [this](gpu::GPU& g, os::WinRect area) {
            doom_port_.render(g, area, tick_count_, dpr_);
        },
        {},
        [this](uint32_t kc, uint32_t ch) {
            doom_port_.on_key(kc, ch);
        }
    );
    wm_.set_always_dirty(doom_win_id_, true);
    wm_.toggle_minimize(doom_win_id_);
    sched_.spawn("freedoom", "/usr/games/freedoom", 2, 12, 8192, {});

    media_win_id_ = wm_.add_window(
        { 0, 0, 0, 0 },
        "media.app",
        [this](gpu::GPU& g, os::WinRect area) {
            media_player_.render(g, area, tick_count_, dpr_);
        },
        [this](int cx, int cy) {
            media_player_.on_click(cx, cy, dpr_);
            wm_.mark_dirty(media_win_id_);
        }
    );
    wm_.toggle_minimize(media_win_id_);
    sched_.spawn("media", "/usr/bin/media", 2, 13, 4096, {});
#endif
}

void Kernel::tick(uint64_t cpu_cycles_per_tick) {
    if (!running_) return;

    ++tick_count_;

#ifdef __EMSCRIPTEN__
    media_player_.step(tick_count_);
    if (media_win_id_ >= 0)
        wm_.set_always_dirty(media_win_id_, media_player_.wants_always_dirty());
#endif

    // Spring physics and frame render run every kernel tick so window
    // positions in the GPU command buffer are always up-to-date.
    wm_.step_animations(1.0f / 120.0f);

    push(gpu_, gpu::Command::CLEAR, COL_BG);
    wm_.draw_taskbar(tick_count_, cpu_.total_cycles(),
                     memory_.size() / (1024u * 1024u));
    wm_.render_all();
    push(gpu_, gpu::Command::FLIP);

    // CPU after FLIP: guest output scheduled here appears on the following frame, not mid-GPU batch.
    cpu_.step(cpu_cycles_per_tick);
    sched_.tick();

    gpu_.flush();
}

void Kernel::send_mousedown(uint32_t x, uint32_t y) {
    wm_.mouse_down(static_cast<int>(x), static_cast<int>(y));
}

void Kernel::send_mousemove(uint32_t x, uint32_t y) {
    wm_.mouse_move(static_cast<int>(x), static_cast<int>(y));
}

void Kernel::send_mouseup() {
    wm_.mouse_up();
}

#ifdef __EMSCRIPTEN__
void Kernel::send_mouse_game(int32_t dx, int32_t dy, uint32_t buttons) noexcept
{
    if (doom_win_id_ < 0 || wm_.focused_id() != doom_win_id_)
        return;
    doom_port_.on_mouse(dx, dy, buttons);
}

bool Kernel::doom_pointer_lock_desired() const noexcept
{
    if (doom_win_id_ < 0 || wm_.focused_id() != doom_win_id_)
        return false;
    return doom_port_.wants_pointer_lock();
}
#endif

void Kernel::send_scroll(int delta) {
    wm_.handle_scroll(delta);
}

void Kernel::send_key(uint32_t keycode, uint32_t charcode) {
    // Modifier bits packed in high half by JS (see index.html):
    //   bit16 = Shift, bit17 = Ctrl, bit18 = Alt, bit19 = Meta
    //   bit20 = keydown (1) vs keyup (0); bit21 = DOM autorepeat (keydown only)
    const uint32_t key  = keycode & 0xFFFF;
    const bool     keydown = (keycode >> 20) & 1;
    const bool     shift = (keycode >> 16) & 1;
    const bool     ctrl  = (keycode >> 17) & 1;
    const bool     alt   = (keycode >> 18) & 1;
    (void)shift;

    // Alt keybindings (global, handled before forwarding to WM).
    if (alt && keydown) {
        if (key == 9) {
            wm_.toggle_launcher();
            return;
        }
        if (key == 81 || key == 113) {
            wm_.close_focused();
            return;
        }
        if (key == 32) {
            wm_.toggle_float_focused();
            return;
        }
        if (key == 13) {
            wm_.toggle_maximize_focused();
            return;
        }
    }

    wm_.handle_key(keycode, charcode);
    (void)ctrl;
}

void Kernel::set_wall_time(uint32_t unix_sec) noexcept {
    wm_.set_wall_time(unix_sec);
}

void Kernel::resize(uint32_t fb_w, uint32_t fb_h) noexcept {
    if (fb_w == 0 || fb_h == 0) return;
    gpu_.init(fb_w, fb_h);
    wm_.set_screen_size(static_cast<int>(fb_w), static_cast<int>(fb_h));
    wm_.recompute_layout();
}

void Kernel::mark_terminal_dirty() noexcept {
    if (term_win_id_ >= 0)
        wm_.mark_dirty(term_win_id_);
}

}  // namespace os
