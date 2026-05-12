#pragma once
#include "../gpu/gpu.hpp"
#include "wm.hpp"
#include <array>
#include <string>
#include <vector>

namespace os {

class Scheduler;
class Filesystem;
class WindowManager;

class Terminal {
public:
    Terminal();

    void configure(const Scheduler* sched, Filesystem* fs,
                   std::size_t mem_bytes, float dpr,
                   const WindowManager* wm,
                   const uint32_t* sim_tick);

    void send_key(uint32_t keycode, uint32_t charcode);

    void render(gpu::GPU& gpu, WinRect area, uint32_t tick);

    void print(const std::string& s);

private:
    static constexpr int MAX_LINES  = 1000;
    static constexpr int LINE_H     = 17;
    static constexpr int PAD        = 8;
    static constexpr int PROMPT_H   = 24;

    std::array<std::string, MAX_LINES> lines_ring_{};
    int                       ring_head_   = 0;
    int                       ring_count_  = 0;

    std::string               input_;
    int                       cursor_pos_  = 0;
    uint32_t                  blink_tick_  = 0;
    bool                      blink_on_    = true;
    std::vector<std::string>  history_;
    int                       hist_idx_    = -1;
    std::string               hist_stash_;

    float                     dpr_         = 1.0f;
    const Scheduler*  sched_     = nullptr;
    Filesystem*       fs_        = nullptr;
    const WindowManager* wm_     = nullptr;
    const uint32_t*     sim_tick_ = nullptr;
    std::size_t       mem_bytes_ = 0;
    uint32_t          sys_tick_  = 0;

    bool        nano_mode_   = false;
    std::string nano_path_;
    std::string nano_buf_;
    int         nano_line_   = 0;   // cursor line
    int         nano_col_    = 0;   // cursor column
    int         nano_scroll_ = 0;
    bool        nano_modified_ = false;

    void print_lines(std::initializer_list<const char*> ls);

    void execute(const std::string& cmd);

    void cmd_help();
    void cmd_about();
    void cmd_skills();
    void cmd_projects();
    void cmd_contact();
    void cmd_ls(const std::string& arg);
    void cmd_cat(const std::string& arg);
    void cmd_uname(bool full);
    void cmd_whoami();
    void cmd_ps(bool aux);
    void cmd_top();
    void cmd_free(bool human);
    void cmd_df(bool human);
    void cmd_uptime();
    void cmd_neofetch();
    void cmd_curl(const std::string& arg);
    void cmd_ping(const std::string& arg);
};

} // namespace os
