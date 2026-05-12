#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace os {

class Scheduler;  // forward

struct FileEntry {
    std::string           name;
    std::vector<uint8_t>  data;
};

enum class InodeType : uint8_t {
    Regular = 0,
    Directory,
    Symlink,
    CharDevice,   // /dev/* character devices
};

// Character device identifiers (kept separate from inode numbers).
enum class DeviceId : uint8_t {
    None = 0,
    Null,     // /dev/null   -  writes discarded, reads return EOF
    Zero,     // /dev/zero   -  reads return 0 bytes, writes discarded
    Random,   // /dev/random, /dev/urandom  -  reads return PRNG bytes
    Tty,      // /dev/tty, /dev/tty0  -  maps to active terminal
};

struct Inode {
    uint32_t              ino        = 0;
    InodeType             type       = InodeType::Regular;
    DeviceId              dev_id     = DeviceId::None;
    uint32_t              parent_ino = 0;
    std::string           name;
    uint32_t              mode       = 0644;  // octal permissions
    uint32_t              uid        = 1000;
    uint32_t              gid        = 1000;
    std::vector<uint32_t> children;           // directory: child inode numbers
};

// Mount-point record (echoes /proc/mounts).
struct MountPoint {
    std::string path;     // e.g. "/proc"
    std::string fstype;   // "jadefs" | "procfs" | "devfs" | "tmpfs"
    uint32_t    root_ino; // inode of this mount's root directory
};

class Filesystem {
public:
    // Build the initial VFS tree and register all mount points.
    // Call once from Kernel::boot() after Memory is ready.
    void init_vfs();

    // Flat-file API (jadefs ROM assets).
    bool             add_file(std::string name, std::vector<uint8_t> data);
    void             write_file(std::string name, std::string text);  // create or overwrite
    std::string      read_text(const std::string& name) const;        // "" if not found
    const FileEntry* find(const std::string& name) const;

    // VFS path lookup  -  returns inode number (0 = not found).
    uint32_t lookup(const std::string& path) const;

    // Directory listing for path.  Returns entry names (dirs end in '/').
    // Falls back to virtual /proc listing if path is under /proc.
    std::vector<std::string> readdir(const std::string& path) const;

    // Inode metadata by number.  Returns nullptr if not found.
    const Inode* stat(uint32_t ino) const;

    // Character device read (returns synthetic content of requested length).
    std::string dev_read(DeviceId dev, std::size_t len) const;

    // Virtual /proc read (unchanged API for terminal cmd_cat compatibility).
    std::string virtual_read(const std::string& path,
                             const Scheduler& sched,
                             std::size_t mem_bytes,
                             uint64_t uptime_ticks) const;

    const std::unordered_map<std::string, FileEntry>& entries() const { return files_; }
    const std::vector<MountPoint>&                    mounts()  const { return mounts_; }

    void set_proc_scheduler(const Scheduler* s) noexcept { proc_sched_ = s; }

    std::uint64_t jadefs_used_kb() const noexcept;

private:
    std::unordered_map<std::string, FileEntry> files_;
    std::unordered_map<uint32_t, Inode>        inodes_;
    std::vector<MountPoint>                    mounts_;
    uint32_t                                   next_ino_  = 2; // 1 = root
    mutable uint32_t                           rand_state_= 0x8B4F2C1D;
    const Scheduler*                           proc_sched_ = nullptr;

    uint32_t make_dir   (uint32_t parent_ino, const char* name);
    uint32_t make_file  (uint32_t parent_ino, const char* name);
    uint32_t make_device(uint32_t parent_ino, const char* name, DeviceId dev);
};

}  // namespace os
