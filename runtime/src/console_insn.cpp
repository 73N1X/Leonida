// The console-only instruction emulation of console_insn.h: the decoder (portable) and the vectored handler that
// uses it (Win32). Behaviour ported from VIctory victory-loader src/console_insn.rs (GPL-3.0-or-later); see the header
// for what is VIctory's and where the encodings come from.
#include "console_insn.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>

#include "../shims/shims.h"
#endif

namespace rt::console_insn {

namespace {

// The ModRM operand's length (ModRM, SIB and displacement) at `b`, 0 when the bytes run out.
size_t modrm_length(const uint8_t* b, size_t size) {
    if (size < 1) return 0;
    const uint8_t mod = b[0] >> 6, rm = b[0] & 7;
    if (mod == 3) return 1;
    size_t length = 1;
    uint8_t base = rm;
    if (rm == 4) {
        if (size < 2) return 0;
        base = b[1] & 7;
        ++length;
    }
    if (mod == 1) length += 1;
    else if (mod == 2 || (mod == 0 && base == 5)) length += 4;   // mod 0 base 5: disp32 (RIP-relative without a SIB)
    return length <= size ? length : 0;
}

}  // namespace

Decoded decode(const uint8_t* code, size_t size) {
    Decoded none;
    if (!code) return none;
    size_t i = 0;
    bool p66 = false, p67 = false;
    uint8_t rep = 0;   // the last of F2 / F3: the one that selects the instruction
    uint8_t rex = 0;   // a REX counts only right before the opcode; a legacy prefix after it cancels it
    for (; i < size && i < 15; ++i) {
        const uint8_t b = code[i];
        if (b >= 0x40 && b <= 0x4F) {
            rex = b;
            continue;
        }
        if (b == 0x66) p66 = true;
        else if (b == 0x67) p67 = true;
        else if (b == 0xF2 || b == 0xF3) rep = b;
        else if (b != 0x26 && b != 0x2E && b != 0x36 && b != 0x3E && b != 0x64 && b != 0x65) break;
        rex = 0;
    }
    if (i + 2 > size || code[i] != 0x0F) return none;
    const uint8_t op = code[i + 1];
    const bool rex_r = (rex & 4) != 0, rex_b = (rex & 1) != 0;

    if (op == 0x0B) return {Kind::Ud2, static_cast<uint8_t>(i + 2)};
    if (i + 3 > size) return none;
    const uint8_t m = code[i + 2];
    const uint8_t mod = m >> 6, reg = (m >> 3) & 7, rm = m & 7;

    switch (op) {
    case 0x01:
        if (m == 0xFC) {
            // clzero; add rax, 40h (48 83 C0 40); cmp rax, r64 (48|4C 39 /r, r/m = rax); jb -12 (72 F4), the jb
            // landing on the clzero. Not after a 67: that CLZERO is at EAX, and it is stepped alone.
            const uint8_t* l = code + i;
            if (!p67 && size >= i + 12 && l[3] == 0x48 && l[4] == 0x83 && l[5] == 0xC0 && l[6] == 0x40 &&
                (l[7] == 0x48 || l[7] == 0x4C) && l[8] == 0x39 && (l[9] >> 6) == 3 && (l[9] & 7) == 0 &&
                l[10] == 0x72 && l[11] == 0xF4) {
                Decoded d{Kind::ClzeroLoop, static_cast<uint8_t>(i + 12)};
                d.reg = static_cast<uint8_t>(((l[9] >> 3) & 7) | (l[7] == 0x4C ? 8 : 0));
                return d;
            }
            Decoded d{Kind::Clzero, static_cast<uint8_t>(i + 3)};
            d.address32 = p67;
            return d;
        }
        if (m == 0xFD) return {Kind::Rdpru, static_cast<uint8_t>(i + 3)};
        if (m == 0xFA) return {Kind::Monitorx, static_cast<uint8_t>(i + 3)};
        if (m == 0xFB) return {Kind::Mwaitx, static_cast<uint8_t>(i + 3)};
        return none;
    case 0xAE: {
        // 66 0F AE /6 with a memory operand; without the 66 it is XSAVEOPT, with F3 another instruction.
        if (!p66 || rep || reg != 6 || mod == 3) return none;
        const size_t operand = modrm_length(code + i + 2, size - (i + 2));
        if (!operand) return none;
        return {Kind::Clwb, static_cast<uint8_t>(i + 2 + operand)};
    }
    case 0xC7: {
        if (rep != 0xF3 || reg != 7 || mod != 3) return none;
        Decoded d{Kind::Rdpid, static_cast<uint8_t>(i + 3)};
        d.reg = static_cast<uint8_t>(rm | (rex_b ? 8 : 0));
        return d;
    }
    case 0x78:
    case 0x79: {
        // SSE4a, register operands only. F2 selects INSERTQ, F3 nothing; 66 without either selects EXTRQ.
        if (mod != 3) return none;
        const bool insert = rep == 0xF2;
        if (!insert && !(rep == 0 && p66)) return none;
        const uint8_t xreg = static_cast<uint8_t>(reg | (rex_r ? 8 : 0));
        const uint8_t xrm = static_cast<uint8_t>(rm | (rex_b ? 8 : 0));
        Decoded d{insert ? Kind::Insertq : Kind::Extrq};
        if (op == 0x78) {
            if (i + 5 > size) return none;
            if (!insert && reg != 0) return none;   // EXTRQ xmm, imm8, imm8 is /0
            d.reg = insert ? xreg : xrm;
            d.src = xrm;
            d.field_length = code[i + 3] & 63;
            d.field_index = code[i + 4] & 63;
            d.length = static_cast<uint8_t>(i + 5);
        } else {
            d.reg = xreg;
            d.src = xrm;
            d.from_register = true;
            d.length = static_cast<uint8_t>(i + 3);
        }
        return d;
    }
    default:
        return none;
    }
}

namespace {
unsigned field_bits(unsigned length) { return (length & 63) ? (length & 63) : 64; }
uint64_t low_ones(unsigned bits) { return bits >= 64 ? ~0ull : (1ull << bits) - 1; }
}  // namespace

uint64_t extrq(uint64_t value, unsigned field_length, unsigned field_index) {
    const unsigned bits = field_bits(field_length), index = field_index & 63;
    return (value >> index) & low_ones(bits);
}

uint64_t insertq(uint64_t dst, uint64_t src, unsigned field_length, unsigned field_index) {
    const unsigned bits = field_bits(field_length), index = field_index & 63;
    const uint64_t mask = low_ones(bits) << index;   // the bits of the field that fall inside the quadword
    return (dst & ~mask) | ((src << index) & mask);
}

namespace {
std::atomic<uint64_t> g_emulated{0};
}
uint64_t emulated_count() { return g_emulated.load(std::memory_order_relaxed); }

#if defined(_WIN32)

namespace {

// Where the committed, writable memory `at` lies in ends, at most `end`; 0 when `at` is not in such memory.
uintptr_t writable_until(uintptr_t at, uintptr_t end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    if (!(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))) return 0;
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return region_end < end ? region_end : end;
}

DWORD64* gpr(CONTEXT* c, unsigned index) {
    DWORD64* const regs[16] = {&c->Rax, &c->Rcx, &c->Rdx, &c->Rbx, &c->Rsp, &c->Rbp, &c->Rsi, &c->Rdi,
                               &c->R8,  &c->R9,  &c->R10, &c->R11, &c->R12, &c->R13, &c->R14, &c->R15};
    return regs[index & 15];
}

const char* name(Kind k) {
    switch (k) {
    case Kind::Clwb: return "CLWB";
    case Kind::Clzero: return "CLZERO";
    case Kind::ClzeroLoop: return "a CLZERO loop";
    case Kind::Rdpru: return "RDPRU";
    case Kind::Monitorx: return "MONITORX";
    case Kind::Mwaitx: return "MWAITX";
    case Kind::Rdpid: return "RDPID";
    case Kind::Extrq: return "EXTRQ";
    case Kind::Insertq: return "INSERTQ";
    default: return "?";
    }
}

}  // namespace

bool emulate(CONTEXT* ctx, const uint8_t* code, size_t size) {
    const Decoded d = decode(code, size);
    if (d.kind == Kind::None || d.kind == Kind::Ud2) return false;

    static std::atomic<bool> s_logged[16];
    if (!s_logged[static_cast<unsigned>(d.kind) & 15].exchange(true))
        std::fprintf(stderr, "[rt] cpu: %s is a console CPU instruction this CPU lacks: emulated (logged once)\n",
                     name(d.kind));

    switch (d.kind) {
    case Kind::Clwb: {
        // Nothing to write back on a PC: the instruction becomes NOPs, so it never faults again.
        DWORD old = 0;
        void* at = reinterpret_cast<void*>(ctx->Rip);
        if (VirtualProtect(at, d.length, PAGE_EXECUTE_READWRITE, &old)) {
            std::memset(at, 0x90, d.length);
            VirtualProtect(at, d.length, old, &old);
            FlushInstructionCache(GetCurrentProcess(), at, d.length);
        }
        break;
    }
    case Kind::Monitorx:
        break;
    case Kind::Mwaitx:
        YieldProcessor();
        break;
    case Kind::Clzero: {
        const uintptr_t line = static_cast<uintptr_t>(d.address32 ? (ctx->Rax & 0xFFFFFFFFull) : ctx->Rax) & ~uintptr_t{63};
        if (writable_until(line, line + 64) != line + 64) return false;   // the real instruction faults there too
        std::memset(reinterpret_cast<void*>(line), 0, 64);
        break;
    }
    case Kind::ClzeroLoop: {
        // The loop's effect: every line from rAX's up to the first rAX + 64k at or past the end is zeroed, and rAX
        // is left there with the jb not taken (CF clear; ZF as the last compare set it).
        const uint64_t start = ctx->Rax, end = *gpr(ctx, d.reg);
        if (end <= start || end - start >= (uint64_t{2} << 30)) {
            // One line and once round, as the CPU would take it.
            const uintptr_t line = static_cast<uintptr_t>(start) & ~uintptr_t{63};
            if (writable_until(line, line + 64) != line + 64) return false;
            std::memset(reinterpret_cast<void*>(line), 0, 64);
            ctx->Rax = start + 64;
            ctx->Rip += (d.length - 12) + 7;   // past clzero and add: the cmp and jb run as code
            g_emulated.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        const uint64_t final_rax = start + ((end - start + 63) / 64) * 64;
        const uintptr_t from = static_cast<uintptr_t>(start) & ~uintptr_t{63};
        const uintptr_t to = static_cast<uintptr_t>((final_rax - 64) & ~uint64_t{63}) + 64;
        uintptr_t at = from;
        while (at < to) {
            const uintptr_t until = writable_until(at, to);
            if (!until) break;
            std::memset(reinterpret_cast<void*>(at), 0, until - at);
            at = until;
        }
        if (at == from) return false;
        if (at < to) {
            // What follows is not writable: the loop resumes at that line (the same instruction again), where the
            // real one faults too. rAX keeps its offset in the line.
            ctx->Rax = at + (start & 63);
            g_emulated.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        ctx->Rax = final_rax;
        constexpr DWORD kCarry = 0x1, kZero = 0x40;
        ctx->EFlags = (ctx->EFlags & ~(kCarry | kZero)) | (final_rax == end ? kZero : 0);
        break;
    }
    case Kind::Rdpru: {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        const uint64_t v = (ctx->Rcx & 0xFFFFFFFFull) <= 1 ? static_cast<uint64_t>(counter.QuadPart) : 0;
        ctx->Rax = v & 0xFFFFFFFFull;
        ctx->Rdx = v >> 32;
        break;
    }
    case Kind::Rdpid:
        *gpr(ctx, d.reg) = GetCurrentProcessorNumber();
        break;
    case Kind::Extrq: {
        M128A* x = ctx->FltSave.XmmRegisters;
        unsigned length = d.field_length, index = d.field_index;
        if (d.from_register) {
            const uint64_t control = x[d.src & 15].Low;
            length = control & 63;
            index = (control >> 8) & 63;
        }
        x[d.reg & 15].Low = extrq(x[d.reg & 15].Low, length, index);
        x[d.reg & 15].High = 0;
        break;
    }
    case Kind::Insertq: {
        M128A* x = ctx->FltSave.XmmRegisters;
        const M128A source = x[d.src & 15];
        unsigned length = d.field_length, index = d.field_index;
        if (d.from_register) {
            const uint64_t control = static_cast<uint64_t>(source.High);
            length = control & 63;
            index = (control >> 8) & 63;
        }
        x[d.reg & 15].Low = insertq(x[d.reg & 15].Low, source.Low, length, index);
        x[d.reg & 15].High = 0;
        break;
    }
    default:
        return false;
    }
    ctx->Rip += d.length;
    g_emulated.fetch_add(1, std::memory_order_relaxed);
    return true;
}

namespace {

LONG CALLBACK illegal_instruction(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord ||
        info->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t rip = static_cast<uintptr_t>(info->ContextRecord->Rip);
    uint64_t base = 0;
    if (!rt::shims::mapped_image_base(rip, base)) return EXCEPTION_CONTINUE_SEARCH;   // only the title's code
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(rip), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    const size_t readable = region_end - rip < 16 ? region_end - rip : 16;
    return emulate(info->ContextRecord, reinterpret_cast<const uint8_t*>(rip), readable) ? EXCEPTION_CONTINUE_EXECUTION
                                                                                        : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void install() {
    static const bool installed = AddVectoredExceptionHandler(1, &illegal_instruction) != nullptr;
    (void)installed;
}

#else

void install() {}

#endif

}  // namespace rt::console_insn
