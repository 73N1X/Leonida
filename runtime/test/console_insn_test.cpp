// Console-only CPU instructions (src/console_insn.*): the decoder against AMD's encodings, the SSE4a bit fields, and
// on Windows the handler's emulation on a hand-made CONTEXT (the code bytes are written here; nothing is executed,
// so the test means the same on an AMD host that has these instructions and an Intel one that lacks them).
#include "console_insn.h"

#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

int g_failures = 0;
void check(bool condition, const char* what) {
    std::printf("%s: %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) ++g_failures;
}

using rt::console_insn::Decoded;
using rt::console_insn::Kind;

Decoded dec(std::initializer_list<uint8_t> bytes) {
    const std::vector<uint8_t> v(bytes);
    return rt::console_insn::decode(v.data(), v.size());
}
bool is(const Decoded& d, Kind k, unsigned length) { return d.kind == k && d.length == length; }

}  // namespace

int main() {
    // ---- decoder ----
    check(is(dec({0x0F, 0x01, 0xFC}), Kind::Clzero, 3), "CLZERO is 0F 01 FC");
    const Decoded a32 = dec({0x67, 0x0F, 0x01, 0xFC});
    check(is(a32, Kind::Clzero, 4) && a32.address32, "with a 67 prefix CLZERO addresses EAX");
    const Decoded loop = dec({0x0F, 0x01, 0xFC, 0x48, 0x83, 0xC0, 0x40, 0x4C, 0x39, 0xC8, 0x72, 0xF4});
    check(is(loop, Kind::ClzeroLoop, 12) && loop.reg == 9, "clzero; add rax,40h; cmp rax,r9; jb: one loop to r9");
    check(is(dec({0x0F, 0x01, 0xFC, 0x48, 0x83, 0xC0, 0x40, 0x48, 0x39, 0xD9, 0x72, 0xF4}), Kind::Clzero, 3),
          "a cmp whose first operand is not rax is no such loop");
    check(is(dec({0x0F, 0x01, 0xFD}), Kind::Rdpru, 3) && is(dec({0x0F, 0x01, 0xFA}), Kind::Monitorx, 3) &&
              is(dec({0x0F, 0x01, 0xFB}), Kind::Mwaitx, 3),
          "RDPRU, MONITORX, MWAITX");
    check(is(dec({0x0F, 0x0B}), Kind::Ud2, 2), "UD2 is recognised (and never emulated)");
    check(is(dec({0x66, 0x0F, 0xAE, 0x33}), Kind::Clwb, 4), "CLWB [rbx]");
    check(is(dec({0x66, 0x0F, 0xAE, 0x74, 0x8B, 0x10}), Kind::Clwb, 6), "CLWB [rbx+rcx*4+10h]");
    check(is(dec({0x66, 0x0F, 0xAE, 0x35, 0, 0, 0, 0}), Kind::Clwb, 8), "CLWB [rip+disp32]");
    check(is(dec({0x66, 0x0F, 0xAE, 0xB0, 0, 1, 0, 0}), Kind::Clwb, 8), "CLWB [rax+disp32]");
    check(is(dec({0x66, 0x49, 0x0F, 0xAE, 0x34, 0x24}), Kind::Clwb, 6), "CLWB [r12] (REX.B, SIB)");
    check(dec({0x0F, 0xAE, 0x33}).kind == Kind::None, "without 66 it is XSAVEOPT, not ours");
    check(dec({0x66, 0x0F, 0xAE, 0x3B}).kind == Kind::None, "66 0F AE /7 is CLFLUSHOPT, not ours");
    check(dec({0x66, 0x0F, 0xAE, 0x74, 0x8B}).kind == Kind::None, "a truncated operand is not decoded");
    const Decoded rdpid = dec({0xF3, 0x49, 0x0F, 0xC7, 0xFB});
    check(is(rdpid, Kind::Rdpid, 5) && rdpid.reg == 11, "RDPID r11");
    check(dec({0xF3, 0x0F, 0xC7, 0x3B}).kind == Kind::None, "F3 0F C7 /7 with memory is not RDPID");

    const Decoded ext = dec({0x66, 0x0F, 0x78, 0xC3, 0x10, 0x08});
    check(is(ext, Kind::Extrq, 6) && ext.reg == 3 && !ext.from_register && ext.field_length == 16 &&
              ext.field_index == 8,
          "EXTRQ xmm3, 16, 8");
    check(dec({0x66, 0x0F, 0x78, 0xCB, 0x10, 0x08}).kind == Kind::None, "EXTRQ's immediate form is /0 only");
    const Decoded ext_r = dec({0x66, 0x45, 0x0F, 0x79, 0xCA});
    check(is(ext_r, Kind::Extrq, 5) && ext_r.reg == 9 && ext_r.src == 10 && ext_r.from_register,
          "EXTRQ xmm9, xmm10 (REX.R, REX.B)");
    const Decoded ins = dec({0xF2, 0x0F, 0x78, 0xCA, 0x04, 0x3C});
    check(is(ins, Kind::Insertq, 6) && ins.reg == 1 && ins.src == 2 && ins.field_length == 4 && ins.field_index == 60,
          "INSERTQ xmm1, xmm2, 4, 60");
    const Decoded ins_r = dec({0xF2, 0x0F, 0x79, 0xC1});
    check(is(ins_r, Kind::Insertq, 4) && ins_r.reg == 0 && ins_r.src == 1 && ins_r.from_register, "INSERTQ xmm0, xmm1");
    check(dec({0xF2, 0x0F, 0x78, 0xCA, 0x04}).kind == Kind::None, "INSERTQ with an immediate missing");
    check(dec({0x0F, 0x78, 0xC1, 0x08, 0x10}).kind == Kind::None, "0F 78 without a prefix is VMREAD, not SSE4a");
    check(is(dec({0x66, 0xF2, 0x0F, 0x78, 0xC1, 0x10, 0x10}), Kind::Insertq, 7) &&
              is(dec({0xF3, 0xF2, 0x0F, 0x78, 0xC1, 0x10, 0x10}), Kind::Insertq, 7) &&
              dec({0xF2, 0xF3, 0x0F, 0x78, 0xC1, 0x10, 0x10}).kind == Kind::None,
          "the last of F2/F3 decides, and 66 beside F2 does not");
    check(is(dec({0x41, 0xF2, 0x0F, 0x78, 0xC1, 0x10, 0x10}), Kind::Insertq, 7) &&
              dec({0x41, 0xF2, 0x0F, 0x78, 0xC1, 0x10, 0x10}).reg == 0,
          "a REX before a legacy prefix counts for nothing");
    check(dec({0x48, 0x8B, 0x01}).kind == Kind::None && dec({0x0F}).kind == Kind::None && dec({}).kind == Kind::None,
          "anything else, and too few bytes, is not ours");

    // ---- SSE4a fields (AMD APM vol. 4: a length of 0 is 64) ----
    using rt::console_insn::extrq;
    using rt::console_insn::insertq;
    check(extrq(0x0123456789ABCDEFull, 8, 4) == 0xDE && extrq(0x0123456789ABCDEFull, 0, 0) == 0x0123456789ABCDEFull &&
              extrq(0x8000000000000000ull, 1, 63) == 1,
          "EXTRQ takes the field and zero-extends it");
    check(insertq(~0ull, 0x12, 8, 8) == 0xFFFFFFFFFFFF12FFull && insertq(0, 0xFF, 4, 60) == 0xF000000000000000ull &&
              insertq(0x55, 0xAAAAull, 0, 0) == 0xAAAAull,
          "INSERTQ puts the source's low bits in the field");

#if defined(_WIN32)
    // ---- the handler's emulation on a context ----
    using rt::console_insn::emulate;
    CONTEXT ctx{};
    alignas(64) static unsigned char block[4096 + 128];
    std::memset(block, 0xCC, sizeof(block));
    const uint8_t clzero[] = {0x0F, 0x01, 0xFC};
    ctx.Rax = reinterpret_cast<DWORD64>(block + 64 + 5);
    ctx.Rip = 0x1000;
    check(emulate(&ctx, clzero, sizeof(clzero)) && ctx.Rip == 0x1003 && block[64] == 0 && block[127] == 0 &&
              block[63] == 0xCC && block[128] == 0xCC,
          "CLZERO zeroes the 64-byte line rAX is in and steps over");

    std::memset(block, 0xCC, sizeof(block));
    const uint8_t loop_code[] = {0x0F, 0x01, 0xFC, 0x48, 0x83, 0xC0, 0x40, 0x48, 0x39, 0xD0, 0x72, 0xF4};
    ctx.Rax = reinterpret_cast<DWORD64>(block);
    ctx.Rdx = reinterpret_cast<DWORD64>(block + 4096);
    ctx.Rip = 0x2000;
    ctx.EFlags = 0x1;
    check(emulate(&ctx, loop_code, sizeof(loop_code)) && ctx.Rip == 0x200C && ctx.Rax == ctx.Rdx &&
              (ctx.EFlags & 1) == 0 && block[4096] == 0xCC,
          "a CLZERO loop is done at once: rAX at the end, the jb not taken");
    bool zero = true;
    for (size_t i = 0; i < 4096; ++i) zero = zero && block[i] == 0;
    check(zero, "and every line up to the end is zeroed");

    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
    ctx.Rax = reinterpret_cast<DWORD64>(page);
    ctx.Rip = 0x3000;
    check(!emulate(&ctx, clzero, sizeof(clzero)) && ctx.Rip == 0x3000,
          "CLZERO on memory it cannot write is not emulated (the real one faults too)");
    VirtualFree(page, 0, MEM_RELEASE);

    const uint8_t rdpid_code[] = {0xF3, 0x0F, 0xC7, 0xF9};   // rdpid rcx
    ctx.Rcx = ~0ull;
    ctx.Rip = 0x4000;
    check(emulate(&ctx, rdpid_code, sizeof(rdpid_code)) && ctx.Rcx < 4096 && ctx.Rip == 0x4004,
          "RDPID writes the processor number");

    const uint8_t ins_code[] = {0xF2, 0x0F, 0x78, 0xCA, 0x08, 0x10};   // insertq xmm1, xmm2, 8, 16
    ctx.FltSave.XmmRegisters[1].Low = 0;
    ctx.FltSave.XmmRegisters[1].High = 77;
    ctx.FltSave.XmmRegisters[2].Low = 0xAB;
    ctx.Rip = 0x5000;
    check(emulate(&ctx, ins_code, sizeof(ins_code)) && ctx.FltSave.XmmRegisters[1].Low == 0xAB0000 &&
              ctx.FltSave.XmmRegisters[1].High == 0 && ctx.Rip == 0x5006,
          "INSERTQ on the context's XMM registers, the high quadword zeroed");
    const uint8_t ext_code[] = {0x66, 0x0F, 0x79, 0xCA};   // extrq xmm1, xmm2
    ctx.FltSave.XmmRegisters[2].Low = 0x0808;   // length 8, index 8
    ctx.FltSave.XmmRegisters[1].Low = 0x123456;
    check(emulate(&ctx, ext_code, sizeof(ext_code)) && ctx.FltSave.XmmRegisters[1].Low == 0x34,
          "EXTRQ with the field in a register");

    unsigned char* code = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    const uint8_t clwb[] = {0x66, 0x0F, 0xAE, 0x30};   // clwb [rax]
    std::memcpy(code, clwb, sizeof(clwb));
    DWORD old = 0;
    VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old);
    ctx.Rip = reinterpret_cast<DWORD64>(code);
    check(emulate(&ctx, code, sizeof(clwb)) && ctx.Rip == reinterpret_cast<DWORD64>(code) + 4 && code[0] == 0x90 &&
              code[3] == 0x90,
          "CLWB is stepped over and patched to NOPs in read-only code");
    VirtualFree(code, 0, MEM_RELEASE);

    const uint8_t ud2[] = {0x0F, 0x0B};
    ctx.Rip = 0x6000;
    check(!emulate(&ctx, ud2, sizeof(ud2)) && ctx.Rip == 0x6000, "UD2 is left to the title's handlers");
    check(rt::console_insn::emulated_count() >= 6, "the emulations are counted");
#endif

    std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
