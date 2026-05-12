#include "memory.hpp"
#include <cstring>

namespace cpu {

void Memory::reset(std::size_t size_bytes) {
    data_.assign(size_bytes, 0);
}

bool Memory::in_bounds(uint32_t addr, std::size_t width) const noexcept {
    return !data_.empty() &&
           static_cast<std::size_t>(addr) + width <= data_.size();
}

uint8_t Memory::read8(uint32_t addr) const {
    if (!in_bounds(addr, 1)) return 0;
    return data_[addr];
}

uint16_t Memory::read16(uint32_t addr) const {
    if (!in_bounds(addr, 2)) return 0;
    uint16_t v{};
    std::memcpy(&v, &data_[addr], 2);
    return v;
}

uint32_t Memory::read32(uint32_t addr) const {
    if (!in_bounds(addr, 4)) return 0;
    uint32_t v{};
    std::memcpy(&v, &data_[addr], 4);
    return v;
}

void Memory::write8(uint32_t addr, uint8_t val) {
    if (!in_bounds(addr, 1)) return;
    data_[addr] = val;
}

void Memory::write16(uint32_t addr, uint16_t val) {
    if (!in_bounds(addr, 2)) return;
    std::memcpy(&data_[addr], &val, 2);
}

void Memory::write32(uint32_t addr, uint32_t val) {
    if (!in_bounds(addr, 4)) return;
    std::memcpy(&data_[addr], &val, 4);
}

void Memory::load(uint32_t base, std::span<const uint8_t> bytes) {
    if (!in_bounds(base, bytes.size())) return;
    std::memcpy(&data_[base], bytes.data(), bytes.size());
}

const uint8_t* Memory::raw_ptr(uint32_t addr) const {
    if (!in_bounds(addr, 1)) return nullptr;
    return &data_[addr];
}

uint8_t* Memory::raw_ptr_mut(uint32_t addr) {
    if (!in_bounds(addr, 1)) return nullptr;
    return &data_[addr];
}

}  // namespace cpu

namespace cpu {

void PageAllocator::init(uint32_t base_addr, uint32_t total_bytes) noexcept {
    base_addr_   = base_addr;
    total_pages_ = total_bytes / kPageSize;
    free_count_  = total_pages_;
    bitmap_.assign(total_pages_, 0);
}

bool PageAllocator::valid_range(uint32_t addr, uint32_t n) const noexcept {
    if (n == 0 || total_pages_ == 0) return false;
    if (addr < base_addr_) return false;
    const uint32_t idx = (addr - base_addr_) / kPageSize;
    return idx + n <= total_pages_;
}

uint32_t PageAllocator::alloc(uint32_t n_pages) noexcept {
    if (n_pages == 0 || n_pages > free_count_) return 0;
    // Linear scan for n_pages contiguous free pages.
    uint32_t run = 0;
    for (uint32_t i = 0; i < total_pages_; ++i) {
        if (bitmap_[i] == 0) {
            if (++run >= n_pages) {
                const uint32_t start = i + 1 - n_pages;
                for (uint32_t j = start; j <= i; ++j) bitmap_[j] = 1;
                free_count_ -= n_pages;
                return base_addr_ + start * kPageSize;
            }
        } else {
            run = 0;
        }
    }
    return 0;  // OOM
}

void PageAllocator::free(uint32_t addr, uint32_t n_pages) noexcept {
    if (!valid_range(addr, n_pages)) return;
    const uint32_t start = (addr - base_addr_) / kPageSize;
    for (uint32_t i = start; i < start + n_pages; ++i) {
        if (bitmap_[i] == 1) { bitmap_[i] = 0; ++free_count_; }
    }
}

}  // namespace cpu
