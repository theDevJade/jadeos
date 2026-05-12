#pragma once
#include "registers.hpp"
#include "memory.hpp"
#include <functional>
#include <cstdint>

namespace cpu {

// Interrupt handler signature: called when INT n is executed.
using InterruptHandler = std::function<void(uint8_t vector, RegisterFile&, Memory&)>;

class CPU {
public:
    explicit CPU(Memory& mem);

    // Register a handler for software interrupt vector n (0-255).
    void set_interrupt_handler(uint8_t vector, InterruptHandler handler);

    // Execute up to `cycles` clock cycles. Returns actual cycles consumed.
    uint64_t step(uint64_t cycles);

    // Single instruction step (useful for debugger).
    void step_once();

    bool is_halted() const noexcept { return halted_; }

    void reset();
    void set_pc(uint32_t addr) { regs_.pc = addr; }

    const RegisterFile& regs() const noexcept { return regs_; }
    uint64_t total_cycles()    const noexcept { return total_cycles_; }

    // Fire exception vector (saves/restores ring, calls handler or halts).
    void fire_exception(uint8_t vector);

private:
    Memory&      mem_;
    RegisterFile regs_{};
    bool         halted_   = false;
    uint64_t     total_cycles_ = 0;

    std::array<InterruptHandler, 256> int_handlers_{};

    // Instruction fetch / decode helpers
    uint8_t  fetch8();
    uint16_t fetch16();
    uint32_t fetch32();

    void execute(Opcode op);
    void update_flags_add(uint32_t a, uint32_t b, uint64_t result);
    void update_flags_sub(uint32_t a, uint32_t b, uint64_t result);
    void update_flags_logic(uint32_t result);
};

}  // namespace cpu
