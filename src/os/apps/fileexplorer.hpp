#pragma once
#include "apputil.hpp"
#include "../filesystem.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace os::apps {

struct FileExplorerState {
    std::string              cur_path  = "/home/jade";
    int                      selected  = 0;
    int                      scroll    = 0;
    std::string              preview;   // content of last opened file
    bool                     in_preview = false;
    std::vector<std::string> entries;   // cached directory listing
};

void render_fileexplorer(gpu::GPU& g, WinRect area,
                         const FileExplorerState& st, float dpr);

// Returns true if state changed.
bool click_fileexplorer(int lx, int ly, WinRect area,
                        FileExplorerState& st, const Filesystem& fs, float dpr);

bool scroll_fileexplorer(int delta, FileExplorerState& st, const Filesystem& fs);

} // namespace os::apps
