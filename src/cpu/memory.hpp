#pragma once
#include <vector>
#include <cstdint>
#include <span>

namespace cpu {

class Memory {
public:
    Memory() = default;

    // Allocate (or re-allocate) and zero all memory.
    // Must be called before any read/write.
    void reset(std::size_t size_bytes);

    std::size_t size() const noexcept { return data_.size(); }

    // All accessors silently clamp on OOB: reads return 0, writes are no-ops.
    uint8_t  read8 (uint32_t addr) const;
    uint16_t read16(uint32_t addr) const;
    uint32_t read32(uint32_t addr) const;

    void write8 (uint32_t addr, uint8_t  val);
    void write16(uint32_t addr, uint16_t val);
    void write32(uint32_t addr, uint32_t val);

    void load(uint32_t base, std::span<const uint8_t> data);

    // Direct views used by GPU for the VRAM region.
    const uint8_t* raw_ptr    (uint32_t addr) const;
    uint8_t*       raw_ptr_mut(uint32_t addr);

private:
    std::vector<uint8_t> data_;

    bool in_bounds(uint32_t addr, std::size_t width) const noexcept;
};

// Bitmap-based physical page allocator.  Tracks the region above the kernel
// image; does not modify Memory's byte array (allocation record only).
class PageAllocator {
public:
    static constexpr uint32_t kPageSize = 4096;

    // Initialise for region [base_addr, base_addr + total_bytes).
    void init(uint32_t base_addr, uint32_t total_bytes) noexcept;

    // Allocate n_pages contiguous pages.
    // Returns base physical address on success, 0 on OOM.
    uint32_t alloc(uint32_t n_pages = 1) noexcept;

    // Free n_pages starting at addr.
    void free(uint32_t addr, uint32_t n_pages = 1) noexcept;

    uint32_t total_pages() const noexcept { return total_pages_; }
    uint32_t free_pages()  const noexcept { return free_count_;  }
    uint32_t used_pages()  const noexcept { return total_pages_ - free_count_; }

private:
    uint32_t             base_addr_   = 0;
    uint32_t             total_pages_ = 0;
    uint32_t             free_count_  = 0;
    std::vector<uint8_t> bitmap_;        // 0=free, 1=used, one byte per page

    bool valid_range(uint32_t addr, uint32_t n) const noexcept;
};

}  // namespace cpu
