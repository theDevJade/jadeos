#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace cpu {

// Custom RISC-like 32-bit ISA (8 general-purpose registers + PC + SP + FLAGS)
static constexpr std::size_t NUM_REGS = 8;

enum class Reg : uint8_t {
    R0 = 0, R1, R2, R3, R4, R5, R6, R7,
};

// FLAGS register bit positions
enum Flags : uint32_t {
    FLAG_ZERO     = 1u << 0,
    FLAG_CARRY    = 1u << 1,
    FLAG_NEGATIVE = 1u << 2,
    FLAG_OVERFLOW = 1u << 3,
};

enum class Opcode : uint8_t {
    NOP  = 0x00,
    HALT = 0x01,
    LOAD = 0x10,  // LOAD  Rx, imm16
    STORE= 0x11,  // STORE [addr], Rx
    MOV  = 0x12,  // MOV   Rx, Ry
    ADD  = 0x20,
    SUB  = 0x21,
    MUL  = 0x22,
    DIV  = 0x23,  // DIV Rx,Ry,Rz: exception vector 0 on divide-by-zero
    AND  = 0x30,
    OR   = 0x31,
    XOR  = 0x32,
    NOT  = 0x33,
    SHL  = 0x34,
    SHR  = 0x35,
    JMP  = 0x40,  // JMP  addr
    JZ   = 0x41,  // JZ   addr  (jump if ZERO)
    JNZ  = 0x42,
    CALL = 0x50,
    RET  = 0x51,
    INT  = 0x60,  // software interrupt (OS syscall gate)
    SYSCALL = 0x61,  // fast syscall (user->kernel, dispatches via R0)
    SYSRET  = 0x62,  // return from syscall (ring 0 -> ring 3)
};

struct RegisterFile {
    std::array<uint32_t, NUM_REGS> gpr{};  // general-purpose
    uint32_t pc   = 0;
    uint32_t sp   = 0;
    uint32_t flags= 0;
    uint8_t  ring = 0;   // privilege ring: 0=kernel, 3=user

    void      reset();
    uint32_t  get(Reg r) const noexcept { return gpr[static_cast<uint8_t>(r)]; }
    void      set(Reg r, uint32_t v)    { gpr[static_cast<uint8_t>(r)] = v;    }
    bool      flag(Flags f) const noexcept { return (flags & f) != 0; }
    void      set_flag(Flags f, bool v) noexcept;
    std::string_view reg_name(Reg r) const noexcept;
};

}  // namespace cpu
