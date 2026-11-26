// Instructions the console's CPU (Zen 2) has and a PC's CPU may lack. A title compiled for the console runs them as
// they are; on an Intel CPU (CLZERO, RDPRU, MONITORX/MWAITX and SSE4a's EXTRQ/INSERTQ are AMD's, CLWB and RDPID are
// missing before Ice Lake / Alder Lake) each raises STATUS_ILLEGAL_INSTRUCTION, and the retail title zeroes memory
// with a CLZERO loop from its first frames. The vectored handler in console_insn.cpp decodes the faulting
// instruction here, emulates it and steps over it.
//
// Which instructions to emulate and how is VIctory's (victory-loader src/console_insn.rs, GPL-3.0-or-later),
// measured on the same title: CLWB is a no-op (a PC's memory is coherent) and is patched to NOPs, MONITORX/MWAITX
// are a skipped wait, RDPRU reads a counter, RDPID the processor number, a whole `clzero; add rax,40h; cmp rax,reg;
// jb` loop is done at once, and the high quadword of an EXTRQ/INSERTQ destination is zeroed (as a Zen 5 does; AMD
// leaves it undefined). This decoder is written from the encodings in AMD's Architecture Programmer's Manual
// (volume 3 for CLZERO, RDPRU, MONITORX, MWAITX, CLWB, RDPID; volume 4 for EXTRQ and INSERTQ), not translated from
// VIctory's, whose decoder is XCL's (GPL-2.0-only); the prefix rules VIctory measured on a Zen 5 (the last of F2/F3
// decides, 66 only without either, a REX counts only right before the opcode) are followed as facts.
//
// Portable: no Win32 here. The handler and its memory checks are the Win32 boundary.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
struct _CONTEXT;   // <windows.h>'s CONTEXT
#endif

namespace rt::console_insn {

enum class Kind : uint8_t {
    None,        // not one of ours: the exception goes on to the title's handlers and the crash report
    Ud2,         // the title's own trap, never emulated
    Clwb,        // 66 0F AE /6 (memory operand): write back a cache line
    Clzero,      // 0F 01 FC: zero the 64-byte line at rAX (EAX with a 67 prefix)
    ClzeroLoop,  // clzero; add rax,40h; cmp rax,<reg>; jb back to the clzero: zero [rax, reg)
    Rdpru,       // 0F 01 FD: ECX 0 MPERF, 1 APERF into EDX:EAX
    Monitorx,    // 0F 01 FA
    Mwaitx,      // 0F 01 FB
    Rdpid,       // F3 0F C7 /7 (register operand): TSC_AUX, the processor number
    Extrq,       // 66 0F 78 /0 ib ib, or 66 0F 79 /r
    Insertq,     // F2 0F 78 /r ib ib, or F2 0F 79 /r
};

struct Decoded {
    Kind kind = Kind::None;
    uint8_t length = 0;     // bytes to step over (for ClzeroLoop the whole loop)
    uint8_t reg = 0;        // Rdpid's destination, ClzeroLoop's end register (CONTEXT order: rax, rcx, rdx, rbx, rsp,
                            // rbp, rsi, rdi, r8-r15); Extrq/Insertq's destination XMM register
    uint8_t src = 0;        // Extrq (register form) / Insertq: the source XMM register
    bool from_register = false;  // Extrq/Insertq: length and index come from the source register, not immediates
    uint8_t field_length = 0;    // immediate forms: the field's length (0 means 64) and index, low 6 bits each
    uint8_t field_index = 0;
    bool address32 = false;      // Clzero: a 67 prefix, the line is at EAX
};

// The console-only instruction at the start of `code` (up to `size` readable bytes), or Kind::None.
Decoded decode(const uint8_t* code, size_t size);

// SSE4a on a low quadword. A field length of 0 is 64; a field past bit 63 gives what the shifts give.
uint64_t extrq(uint64_t value, unsigned field_length, unsigned field_index);
uint64_t insertq(uint64_t dst, uint64_t src, unsigned field_length, unsigned field_index);

// Installs the vectored handler (Windows; a no-op elsewhere). Emulates only in images the loader mapped. Once.
void install();

// How many instructions the handler has emulated, for the log and tests.
uint64_t emulated_count();

#if defined(_WIN32)
// The handler's work on a faulting thread's context, with `size` readable code bytes at its RIP (`code`): true when
// the instruction was emulated (RIP is past it, or at it again for a CLZERO loop stopped by memory it cannot write).
bool emulate(::_CONTEXT* ctx, const uint8_t* code, size_t size);
#endif

}  // namespace rt::console_insn
