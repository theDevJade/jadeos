#include "cpu.hpp"
#include <stdexcept>

namespace cpu {


void RegisterFile::reset() {
    gpr.fill(0);
    pc = sp = flags = 0;
}

void RegisterFile::set_flag(Flags f, bool v) noexcept {
    if (v) flags |= f;
    else   flags &= ~static_cast<uint32_t>(f);
}

// i am too lazy to write out reg names in the disassembler, so here we are
std::string_view RegisterFile::reg_name(Reg r) const noexcept {
    static constexpr std::array<std::string_view, NUM_REGS> names{
        "R0","R1","R2","R3","R4","R5","R6","R7"
    };
    return names[static_cast<uint8_t>(r)];
}


CPU::CPU(Memory& mem) : mem_(mem) {}

void CPU::set_interrupt_handler(uint8_t vector, InterruptHandler handler) {
    int_handlers_[vector] = std::move(handler);
}

void CPU::reset() {
    regs_.reset();
    halted_      = false;
    total_cycles_= 0;
}


uint8_t CPU::fetch8() {
    uint8_t v = mem_.read8(regs_.pc);
    regs_.pc += 1;
    return v;
}

uint16_t CPU::fetch16() {
    uint16_t v = mem_.read16(regs_.pc);
    regs_.pc += 2;
    return v;
}

uint32_t CPU::fetch32() {
    uint32_t v = mem_.read32(regs_.pc);
    regs_.pc += 4;
    return v;
}


void CPU::update_flags_add(uint32_t a, uint32_t b, uint64_t result) {
    regs_.set_flag(FLAG_ZERO,     (result & 0xFFFF'FFFF) == 0);
    regs_.set_flag(FLAG_CARRY,    result > 0xFFFF'FFFF);
    regs_.set_flag(FLAG_NEGATIVE, (result >> 31) & 1);
    // sooo.. what the hell is this magic boolean expression for overflow detection? well, it works ¯\_(ツ)_/¯
    bool ov = (~(a ^ b) & (a ^ static_cast<uint32_t>(result)) & 0x8000'0000) != 0;
    regs_.set_flag(FLAG_OVERFLOW, ov);
}

void CPU::update_flags_sub(uint32_t a, uint32_t b, uint64_t result) {
    regs_.set_flag(FLAG_ZERO,     (result & 0xFFFF'FFFF) == 0);
    regs_.set_flag(FLAG_CARRY,    b > a);
    regs_.set_flag(FLAG_NEGATIVE, (result >> 31) & 1);
    bool ov = ((a ^ b) & (a ^ static_cast<uint32_t>(result)) & 0x8000'0000) != 0;
    regs_.set_flag(FLAG_OVERFLOW, ov);
}

void CPU::update_flags_logic(uint32_t result) {
    regs_.set_flag(FLAG_ZERO,     result == 0);
    regs_.set_flag(FLAG_NEGATIVE, (result >> 31) & 1);
    regs_.set_flag(FLAG_CARRY,    false);
    regs_.set_flag(FLAG_OVERFLOW, false);
}


// heh this is such an oversimplified instruction set, it all fits in one giant switch statement.
void CPU::execute(Opcode op) {
    switch (op) {
    case Opcode::NOP:
        break;

    case Opcode::HALT:
        halted_ = true;
        break;

    case Opcode::LOAD: {
        auto dst = static_cast<Reg>(fetch8());
        auto imm = fetch32();
        regs_.set(dst, imm);
        break;
    }
    case Opcode::STORE: {
        uint32_t addr = fetch32();
        auto src = static_cast<Reg>(fetch8());
        mem_.write32(addr, regs_.get(src));
        break;
    }
    case Opcode::MOV: {
        auto dst = static_cast<Reg>(fetch8());
        auto src = static_cast<Reg>(fetch8());
        regs_.set(dst, regs_.get(src));
        break;
    }
    case Opcode::ADD: {
        auto dst = static_cast<Reg>(fetch8());
        auto lhs = static_cast<Reg>(fetch8());
        auto rhs = static_cast<Reg>(fetch8());
        uint64_t res = static_cast<uint64_t>(regs_.get(lhs)) + regs_.get(rhs);
        update_flags_add(regs_.get(lhs), regs_.get(rhs), res);
        regs_.set(dst, static_cast<uint32_t>(res));
        break;
    }
    case Opcode::SUB: {
        auto dst = static_cast<Reg>(fetch8());
        auto lhs = static_cast<Reg>(fetch8());
        auto rhs = static_cast<Reg>(fetch8());
        uint64_t res = static_cast<uint64_t>(regs_.get(lhs)) - regs_.get(rhs);
        update_flags_sub(regs_.get(lhs), regs_.get(rhs), res);
        regs_.set(dst, static_cast<uint32_t>(res));
        break;
    }
    case Opcode::MUL: {
        auto dst = static_cast<Reg>(fetch8());
        auto lhs = static_cast<Reg>(fetch8());
        auto rhs = static_cast<Reg>(fetch8());
        regs_.set(dst, regs_.get(lhs) * regs_.get(rhs));
        break;
    }
    case Opcode::DIV: {
        auto dst    = static_cast<Reg>(fetch8());
        auto lhs    = static_cast<Reg>(fetch8());
        auto rhs    = static_cast<Reg>(fetch8());
        uint32_t divisor = regs_.get(rhs);
        if (divisor == 0) { fire_exception(0); break; }  // #DE divide-by-zero
        regs_.set(dst, regs_.get(lhs) / divisor);
        break;
    }
    case Opcode::AND: {
        auto dst = static_cast<Reg>(fetch8());
        auto lhs = static_cast<Reg>(fetch8());
        auto rhs = static_cast<Reg>(fetch8());
        uint32_t res = regs_.get(lhs) & regs_.get(rhs);
        update_flags_logic(res);
        regs_.set(dst, res);
        break;
    }
    case Opcode::OR: {
        auto dst = static_cast<Reg>(fetch8());
        auto lhs = static_cast<Reg>(fetch8());
        auto rhs = static_cast<Reg>(fetch8());
        uint32_t res = regs_.get(lhs) | regs_.get(rhs);
        update_flags_logic(res);
        regs_.set(dst, res);
        break;
    }
    case Opcode::XOR: {
        auto dst = static_cast<Reg>(fetch8());
        auto lhs = static_cast<Reg>(fetch8());
        auto rhs = static_cast<Reg>(fetch8());
        uint32_t res = regs_.get(lhs) ^ regs_.get(rhs);
        update_flags_logic(res);
        regs_.set(dst, res);
        break;
    }
    case Opcode::NOT: {
        auto dst = static_cast<Reg>(fetch8());
        auto src = static_cast<Reg>(fetch8());
        uint32_t res = ~regs_.get(src);
        update_flags_logic(res);
        regs_.set(dst, res);
        break;
    }
    case Opcode::SHL: {
        auto dst = static_cast<Reg>(fetch8());
        auto src = static_cast<Reg>(fetch8());
        uint8_t  amt = fetch8();
        regs_.set(dst, regs_.get(src) << amt);
        break;
    }
    case Opcode::SHR: {
        auto dst = static_cast<Reg>(fetch8());
        auto src = static_cast<Reg>(fetch8());
        uint8_t  amt = fetch8();
        regs_.set(dst, regs_.get(src) >> amt);
        break;
    }
    case Opcode::JMP: {
        regs_.pc = fetch32();
        break;
    }
    case Opcode::JZ: {
        uint32_t addr = fetch32();
        if (regs_.flag(FLAG_ZERO)) regs_.pc = addr;
        break;
    }
    case Opcode::JNZ: {
        uint32_t addr = fetch32();
        if (!regs_.flag(FLAG_ZERO)) regs_.pc = addr;
        break;
    }
    case Opcode::CALL: {
        uint32_t addr = fetch32();
        regs_.sp -= 4;
        mem_.write32(regs_.sp, regs_.pc);
        regs_.pc = addr;
        break;
    }
    case Opcode::RET: {
        regs_.pc = mem_.read32(regs_.sp);
        regs_.sp += 4;
        break;
    }
    case Opcode::INT: {
        uint8_t vector = fetch8();
        const uint8_t saved_ring = regs_.ring;
        regs_.ring = 0;                    // escalate to ring 0 (kernel)
        if (int_handlers_[vector]) {
            int_handlers_[vector](vector, regs_, mem_);
        } else {
            fire_exception(13);            // #GP: no handler for this vector
        }
        regs_.ring = saved_ring;           // return to caller's ring
        break;
    }
    case Opcode::SYSCALL: {
        // Fast syscall path: R0 = syscall nr, same handler table as INT.
        const uint8_t saved_ring = regs_.ring;
        regs_.ring = 0;
        uint8_t nr = static_cast<uint8_t>(regs_.get(Reg::R0));
        if (int_handlers_[nr]) int_handlers_[nr](nr, regs_, mem_);
        regs_.ring = saved_ring;
        break;
    }
    case Opcode::SYSRET: {
        regs_.ring = 3;                    // return to user mode
        break;
    }
    default:
        fire_exception(6);   // #UD invalid opcode
        break;
    }
}

// Hardware exception table (subset of x86 IDT convention):
//   0  -  #DE  Divide Error
//   6  -  #UD  Invalid Opcode
//  13  -  #GP  General Protection Fault
//  14  -  #PF  Page Fault
void CPU::fire_exception(uint8_t vector) {
    if (int_handlers_[vector]) {
        const uint8_t saved_ring = regs_.ring;
        regs_.ring = 0;
        int_handlers_[vector](vector, regs_, mem_);
        regs_.ring = saved_ring;
    } else {
        halted_ = true;   // unhandled exception  -  CPU halts
    }
}


void CPU::step_once() {
    if (halted_) return;
    auto op = static_cast<Opcode>(fetch8());
    execute(op);
    ++total_cycles_;
}

uint64_t CPU::step(uint64_t cycles) {
    uint64_t done = 0;
    while (!halted_ && done < cycles) {
        step_once();
        ++done;
    }
    return done;
}

}  // namespace cpu
