// Checks the registered acphal.dll shim against the measured call shapes
// (docs/audio_hal_exe_calls.md, docs/retail_boot_chain.md, docs/retail_boot_frame_loop.md Stop 28, PR #6885,
// PR #7009) and the coprocessor and XMA behaviour ported from VIctory (crates/victory-hle/src/modules/audio.rs tests,
// GPL-3.0-or-later, Marvisak and contributors, DexNom fork).
//
// All 9 exports are resolved through rt::shims::find so the test exercises
// the exact shim table and calling convention the runtime installs. No game data: the XMA stream is built here.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "shims.h"

namespace {

using AcpHalCreateFn = HRESULT(__stdcall*)(void**);
using AcpHalAllocShapeCtxFn = HRESULT(__stdcall*)(void*);
using AcpHalReleaseShapeCtxFn = HRESULT(__stdcall*)(void*);
using ApuCreateHeapFn = HRESULT(__stdcall*)(uint32_t, uint32_t);
// Measured: ApuAlloc(void** va, uint32_t* apu, uint32_t size, uint32_t align, uint32_t flags)
using ApuAllocFn = HRESULT(__stdcall*)(void**, uint32_t*, uint32_t, uint32_t, uint32_t);
using ApuFreeFn = void(__stdcall*)(void*);
// Measured: ApuIsVirtualAddressValid(void* va, uint32_t align)
using ApuIsValidFn = BOOL(__stdcall*)(void*, uint32_t);
using ApuMapApuAddrFn = void*(__stdcall*)(uint32_t);
using ApuMapVirtualAddrFn = uint32_t(__stdcall*)(void*);

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

// Test mirror of SHAPE_CONTEXTS: six counts, six arrays, six 32-bit device addresses (0x60 bytes, VIctory audio.rs).
struct TestShapeContexts {
    uint32_t numSrcContexts;
    uint32_t numEqCompContexts;
    uint32_t numFiltVolContexts;
    uint32_t numDmaContexts;
    uint32_t numXmaContexts;
    uint32_t numPcmContexts;
    void* srcContextArray;
    void* eqCompContextArray;
    void* filtVolContextArray;
    void* dmaContextArray;
    void* xmaContextArray;
    void* pcmContextArray;
    uint32_t apuSrcContextArray;
    uint32_t apuEqCompContextArray;
    uint32_t apuFiltVolContextArray;
    uint32_t apuDmaContextArray;
    uint32_t apuXmaContextArray;
    uint32_t apuPcmContextArray;
};

static_assert(sizeof(TestShapeContexts) == 0x60, "TestShapeContexts is 0x60 bytes");
static_assert(offsetof(TestShapeContexts, xmaContextArray) == 0x38,
              "xmaContextArray is at offset +0x38");

// The coprocessor's message (VIctory audio.rs): 28 bytes.
#pragma pack(push, 1)
struct Message {
    uint32_t kind;
    uint32_t dropped;
    uint32_t usec;
    uint8_t data[16];
};
#pragma pack(pop)
static_assert(sizeof(Message) == 28);

constexpr uint32_t kLoadFlowgraph = 1, kRegisterMessages = 2, kUnregisterMessages = 3;
constexpr uint32_t kEnableXma = 5, kDisableXma = 7, kAdvanceXmaRead = 18;
constexpr uint32_t kFlowgraphCompleted = 0x02, kCommandCompleted = 0x10, kDisconnected = 0x80;

using ConnectFn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t);
using DisconnectFn = HRESULT(__stdcall*)(void*);
using SubmitFn = HRESULT(__stdcall*)(void*, uint32_t, uint64_t, uint32_t, const void*, uint32_t);
using PopFn = bool(__stdcall*)(void*, Message*);
using CountFn = uint32_t(__stdcall*)(void*);
using ReleaseFn = HRESULT(__stdcall*)(void*);

struct Acp {
    void* self = nullptr;
    ConnectFn connect = nullptr;
    DisconnectFn disconnect = nullptr;
    SubmitFn submit = nullptr;
    PopFn pop = nullptr;
    CountFn count = nullptr;
    ReleaseFn release = nullptr;

    explicit Acp(void* object) : self(object) {
        auto vtable = *static_cast<void* const* const*>(object);
        connect = reinterpret_cast<ConnectFn>(vtable[0]);
        disconnect = reinterpret_cast<DisconnectFn>(vtable[1]);
        submit = reinterpret_cast<SubmitFn>(vtable[2]);
        pop = reinterpret_cast<PopFn>(vtable[3]);
        count = reinterpret_cast<CountFn>(vtable[4]);
        release = reinterpret_cast<ReleaseFn>(vtable[5]);
    }
};

// Writes `value` as `n` bits, MSB first, at bit `pos`.
void put_bits(std::vector<uint8_t>& data, size_t pos, uint32_t n, uint32_t value) {
    for (uint32_t i = 0; i < n; ++i) {
        const size_t p = pos + i;
        const unsigned bit = (value >> (n - 1 - i)) & 1u;
        data[p / 8] = static_cast<uint8_t>((data[p / 8] & ~(0x80u >> (p % 8))) | (bit << (7 - p % 8)));
    }
}

// Two 2 KiB packets of three silent mono frames each (VIctory audio.rs, silent_stream).
std::vector<uint8_t> silent_stream() {
    std::vector<uint8_t> data(2 * 2048, 0xff);
    for (size_t p = 0; p < 2; ++p) {
        const size_t base = p * 2048 * 8;
        put_bits(data, base, 32, 3u << 26);
        for (size_t frame = 0; frame < 3; ++frame) {
            const size_t at = base + 32 + frame * 31;
            put_bits(data, at, 15, 31);
            put_bits(data, at + 15, 2, 0b10);
            put_bits(data, at + 17, 14, 0);
        }
    }
    return data;
}

uint32_t field(const volatile uint32_t* words, size_t word, uint32_t shift, uint32_t bits) {
    return (words[word] >> shift) & ((1u << bits) - 1);
}

// Waits up to five seconds for the decoding thread to bring a context to `done`.
template <class Done>
bool wait_for(Done done) {
    for (int i = 0; i < 5000; ++i) {
        if (done()) return true;
        Sleep(1);
    }
    return done();
}

}  // namespace

int main() {
    std::printf("acphal_test: registered acphal.dll shim verification\n");

    const auto create = reinterpret_cast<AcpHalCreateFn>(
        rt::shims::find("acphal.dll", "AcpHalCreate"));
    const auto alloc_ctx = reinterpret_cast<AcpHalAllocShapeCtxFn>(
        rt::shims::find("acphal.dll", "AcpHalAllocateShapeContexts"));
    const auto release_ctx = reinterpret_cast<AcpHalReleaseShapeCtxFn>(
        rt::shims::find("acphal.dll", "AcpHalReleaseShapeContexts"));
    const auto create_heap = reinterpret_cast<ApuCreateHeapFn>(
        rt::shims::find("acphal.dll", "ApuCreateHeap"));
    const auto alloc = reinterpret_cast<ApuAllocFn>(
        rt::shims::find("acphal.dll", "ApuAlloc"));
    const auto free_mem = reinterpret_cast<ApuFreeFn>(
        rt::shims::find("acphal.dll", "ApuFree"));
    const auto is_valid = reinterpret_cast<ApuIsValidFn>(
        rt::shims::find("acphal.dll", "ApuIsVirtualAddressValid"));
    const auto map_apu = reinterpret_cast<ApuMapApuAddrFn>(
        rt::shims::find("acphal.dll", "ApuMapApuAddress"));
    const auto map_virtual = reinterpret_cast<ApuMapVirtualAddrFn>(
        rt::shims::find("acphal.dll", "ApuMapVirtualAddress"));

    check(create != nullptr, "AcpHalCreate is registered in registry");
    check(alloc_ctx != nullptr, "AcpHalAllocateShapeContexts is registered in registry");
    check(release_ctx != nullptr, "AcpHalReleaseShapeContexts is registered in registry");
    check(create_heap != nullptr, "ApuCreateHeap is registered in registry");
    check(alloc != nullptr, "ApuAlloc is registered in registry");
    check(free_mem != nullptr, "ApuFree is registered in registry");
    check(is_valid != nullptr, "ApuIsVirtualAddressValid is registered in registry");
    check(map_apu != nullptr, "ApuMapApuAddress is registered in registry");
    check(map_virtual != nullptr, "ApuMapVirtualAddress is registered in registry");
    check(rt::shims::owns_dll("acphal.dll"), "the registry owns acphal.dll");

    if (!create || !alloc_ctx || !release_ctx || !create_heap || !alloc ||
        !free_mem || !is_valid || !map_apu || !map_virtual) {
        std::printf("Fatal: Missing export bindings for acphal.dll\n");
        return 1;
    }

    // 1. AcpHalCreate and the IAcpHal slots
    void* acp = nullptr;
    check(create(&acp) == S_OK && acp != nullptr,
          "AcpHalCreate returns S_OK with a non-null object");
    check(create(nullptr) == E_POINTER,
          "AcpHalCreate(nullptr) returns E_POINTER");

    if (acp) {
        auto vtable = *static_cast<void* const* const*>(acp);
        for (int slot = 0; slot < 6; ++slot) check(vtable[slot] != nullptr, "IAcpHal vtable slot is present");
        Acp m(acp);

        check(m.connect(acp, 0, 0x1000) == E_INVALIDARG, "IAcpHal slot 0 (connect) refuses a queue of no commands");
        // Slot 0: Connect (called at boot with (0x1000, 0x1000))
        check(m.connect(acp, 0x1000, 0x1000) == S_OK, "IAcpHal slot 0 (connect) returns S_OK");

        // Slot 2: SubmitCommand. Critical boot requirement: The title calls
        // slot 2 as (2, 0xf0f0f0f0, -1, config, 0) and treats failure as fatal int3.
        check(m.submit(acp, 2, 0xf0f0f0f0, 0xffffffffu, nullptr, 0) == S_OK,
              "IAcpHal slot 2 (submit_command) returns S_OK (vital boot unblocker)");

        // Slots 3 and 4: nothing registered, so no message
        Message message{};
        check(!m.pop(acp, &message), "IAcpHal slot 3 (pop_message) has no message");
        check(m.count(acp) == 0, "IAcpHal slot 4 (message_count) is 0");

        // Slot 1: Disconnect, then a second one fails
        check(m.disconnect(acp) == S_OK, "IAcpHal slot 1 (disconnect) returns S_OK");
        check(m.disconnect(acp) == E_FAIL, "IAcpHal slot 1 fails when not connected");

        // Slot 5: Release returns S_OK
        check(m.release(acp) == S_OK, "IAcpHal slot 5 returns S_OK");
    }

    // 2. ApuCreateHeap: title passes (cachedSize, nonCachedSize=0x226e00)
    check(create_heap(4 * 1024 * 1024, 0x226e00) == S_OK,
          "ApuCreateHeap(4 MiB, 0x226e00) succeeds");
    check(create_heap(4 * 1024 * 1024, 0x226e00) == static_cast<HRESULT>(0x8ACA0001u),
          "ApuCreateHeap second call fails (heap already created)");

    // 3. ApuAlloc argument validation
    void* dummy = nullptr;
    check(alloc(nullptr, nullptr, 1024, 0x800, 0) == E_POINTER,
          "ApuAlloc(nullptr, ...) returns E_POINTER");
    check(alloc(&dummy, nullptr, 0, 0x800, 0) == E_INVALIDARG,
          "ApuAlloc with size=0 returns E_INVALIDARG");
    check(alloc(&dummy, nullptr, 1024, 3, 0) == E_INVALIDARG,
          "ApuAlloc with unaligned alignment (3) returns E_INVALIDARG");

    // 4. ApuAlloc real boot sequence:
    // First call: flags=2, size=0x222e00, align=0x800 (noncached heap)
    void* va_nc = nullptr;
    uint32_t apu_nc = 0;
    check(alloc(&va_nc, &apu_nc, 0x222e00, 0x800, 2) == S_OK && va_nc != nullptr,
          "ApuAlloc flags=2 at title's non-cached size 0x222e00 succeeds");
    check(apu_nc != 0 && apu_nc % 0x800 == 0, "ApuAlloc provides a non-zero, aligned APU address");

    // Second call: flags=1, size=4096, align=0x800 (cached heap)
    void* va_c = nullptr;
    uint32_t apu_c = 0;
    check(alloc(&va_c, &apu_c, 4096, 0x800, 1) == S_OK && va_c != nullptr,
          "ApuAlloc flags=1 succeeds on cached heap");
    check(apu_c != 0, "ApuAlloc flags=1 provides non-zero APU address");

    // Address verification & disjoint ranges
    check(va_nc != va_c, "Cached and non-cached allocations return different virtual pointers");
    check(apu_nc != apu_c, "Cached and non-cached APU addresses are distinct");
    check(is_valid(va_nc, 0x800) == TRUE, "Non-cached allocation is in a valid heap");
    check(is_valid(va_c, 0x800) == TRUE, "Cached allocation is in a valid heap");
    check(is_valid(nullptr, 0x800) == FALSE, "ApuIsVirtualAddressValid(nullptr) returns FALSE");
    check(is_valid(reinterpret_cast<void*>(0x1234), 0x800) == FALSE,
          "ApuIsVirtualAddressValid(bogus) returns FALSE");

    // Round-trip mapping
    check(map_virtual(va_nc) == apu_nc, "ApuMapVirtualAddress matches apu_nc");
    check(map_virtual(va_c) == apu_c, "ApuMapVirtualAddress matches apu_c");
    check(map_apu(apu_nc) == va_nc, "ApuMapApuAddress round-trips non-cached address");
    check(map_apu(apu_c) == va_c, "ApuMapApuAddress round-trips cached address");
    check(map_apu(apu_nc + 0x40) == static_cast<uint8_t*>(va_nc) + 0x40, "ApuMapApuAddress maps inside a block");
    check(map_apu(0) == nullptr, "ApuMapApuAddress(0) returns nullptr");
    check(map_virtual(nullptr) == 0, "ApuMapVirtualAddress(nullptr) returns 0");

    // 5. Genuine State Tracking: Free & Memory Reuse
    void* reusable_block1 = nullptr;
    check(alloc(&reusable_block1, nullptr, 0x1000, 0x800, 1) == S_OK && reusable_block1 != nullptr,
          "Allocate 4 KiB reusable block");
    free_mem(reusable_block1);
    check(true, "ApuFree on valid address succeeds without error");
    free_mem(nullptr);
    check(true, "ApuFree(nullptr) is a safe no-op");

    // Reallocate in the freed slot:
    void* reusable_block2 = nullptr;
    check(alloc(&reusable_block2, nullptr, 0x1000, 0x800, 1) == S_OK && reusable_block2 != nullptr,
          "Re-allocate in cached heap succeeds (memory successfully reused)");
    check(reusable_block2 == reusable_block1,
          "Free block was genuinely reused at the same offset by the allocator");
    free_mem(reusable_block2);

    // 6. AcpHalAllocateShapeContexts & AcpHalReleaseShapeContexts
    check(alloc_ctx(nullptr) == E_POINTER,
          "AcpHalAllocateShapeContexts(nullptr) returns E_POINTER");
    check(release_ctx(nullptr) == E_FAIL, "AcpHalReleaseShapeContexts fails with nothing allocated");

    // Real executable call: 0 SRC, 350 XMA (+0x38), 0 PCM
    TestShapeContexts ctx{};
    ctx.numXmaContexts = 350;
    check(alloc_ctx(&ctx) == S_OK, "AcpHalAllocateShapeContexts succeeds");
    check(ctx.xmaContextArray != nullptr,
          "xmaContextArray (+0x38) is non-null after allocation");
    check(ctx.srcContextArray == nullptr && ctx.apuSrcContextArray == 0, "srcContextArray is null when count is 0");
    check(ctx.apuXmaContextArray != 0 && map_apu(ctx.apuXmaContextArray) == ctx.xmaContextArray &&
              map_virtual(ctx.xmaContextArray) == ctx.apuXmaContextArray,
          "the XMA array's device address maps both ways");
    check(is_valid(ctx.xmaContextArray, 0x100) == TRUE, "the XMA array is coprocessor memory, 256-byte aligned");

    // Verify zeroed memory across all 350 * 64 bytes
    const uint8_t* xma_bytes = static_cast<const uint8_t*>(ctx.xmaContextArray);
    bool all_zero = true;
    for (size_t i = 0; i < 350 * 64; ++i) {
        if (xma_bytes[i] != 0) {
            all_zero = false;
            break;
        }
    }
    check(all_zero, "SHAPE context buffer is zero-initialized for title's stride");
    TestShapeContexts again{};
    again.numXmaContexts = 4;
    check(alloc_ctx(&again) == E_FAIL && again.xmaContextArray == nullptr, "a second allocation before release fails");
    // The title passes a handle, not the structure: nothing is written through the argument.
    TestShapeContexts witness = ctx;
    check(release_ctx(&witness) == S_OK && witness.xmaContextArray == ctx.xmaContextArray,
          "AcpHalReleaseShapeContexts succeeds and writes nothing through its argument");
    check(map_apu(ctx.apuXmaContextArray) == nullptr, "a released array no longer maps");

    // Test multi-context allocation, as fmod's startup asks (rate conversion, DMA and XMA) plus the others
    TestShapeContexts ctx_all{};
    ctx_all.numSrcContexts = 0x200;
    ctx_all.numEqCompContexts = 5;
    ctx_all.numFiltVolContexts = 8;
    ctx_all.numDmaContexts = 0x400;
    ctx_all.numXmaContexts = 0x200;
    ctx_all.numPcmContexts = 6;
    check(alloc_ctx(&ctx_all) == S_OK, "AcpHalAllocateShapeContexts with all 6 types succeeds");
    check(ctx_all.srcContextArray != nullptr, "srcContextArray is allocated");
    check(ctx_all.eqCompContextArray != nullptr, "eqCompContextArray is allocated");
    check(ctx_all.filtVolContextArray != nullptr, "filtVolContextArray is allocated");
    check(ctx_all.dmaContextArray != nullptr, "dmaContextArray is allocated");
    check(ctx_all.xmaContextArray != nullptr, "xmaContextArray is allocated");
    check(ctx_all.pcmContextArray != nullptr, "pcmContextArray is allocated");
    std::memset(ctx_all.srcContextArray, 0, 0x200 << 5);  // as fmod clears them
    const uint32_t apus[6] = {ctx_all.apuSrcContextArray,     ctx_all.apuEqCompContextArray,
                              ctx_all.apuFiltVolContextArray, ctx_all.apuDmaContextArray,
                              ctx_all.apuXmaContextArray,     ctx_all.apuPcmContextArray};
    void* const arrays[6] = {ctx_all.srcContextArray, ctx_all.eqCompContextArray, ctx_all.filtVolContextArray,
                             ctx_all.dmaContextArray, ctx_all.xmaContextArray,    ctx_all.pcmContextArray};
    bool mapped = true;
    for (int i = 0; i < 6; ++i) mapped &= apus[i] != 0 && apus[i] % 256 == 0 && map_apu(apus[i]) == arrays[i];
    check(mapped, "every array has an aligned device address of its own that maps back");

    // 7. The coprocessor's messages
    {
        void* object = nullptr;
        check(create(&object) == S_OK, "a coprocessor for the message tests");
        Acp m(object);
        // fmod: connect, register every message but frame starts, wait for the registration to be acknowledged.
        check(m.connect(object, 0x804, 0x804) == S_OK, "connect as fmod does");
        const uint32_t mask = 0xfe;
        check(m.submit(object, kRegisterMessages, 1, 0xffffffffu, &mask, 0) == S_OK, "register messages with id 1");
        Message message{};
        check(m.pop(object, &message) && message.kind == kCommandCompleted && message.dropped == 0,
              "the registration is acknowledged");
        uint32_t command = 0;
        uint64_t id = 0;
        std::memcpy(&command, message.data, 4);
        std::memcpy(&id, message.data + 4, 8);
        check(command == kRegisterMessages && id == 1, "the acknowledgement names the command and its id");
        check(!m.pop(object, &message), "then nothing");

        // A flowgraph completes unprocessed, naming itself.
        const uint32_t flowgraph[2] = {0, 0x1234};
        check(m.submit(object, kLoadFlowgraph, 0, 0xffffffffu, flowgraph, 0) == S_OK, "load a flowgraph");
        uint32_t named = 0;
        check(m.pop(object, &message) && message.kind == kFlowgraphCompleted &&
                  (std::memcpy(&named, message.data, 4), named == 0x1234),
              "the flowgraph completes");

        // A command's notification word is set.
        void* word = nullptr;
        uint32_t word_apu = 0;
        check(alloc(&word, &word_apu, 16, 16, 1) == S_OK, "a notification word in the heap");
        check(m.submit(object, 12, 0, 0xffffffffu, nullptr, word_apu) == S_OK &&
                  *static_cast<volatile uint32_t*>(word) == 1,
              "the notification word is set");
        free_mem(word);

        // Unregistered: no acknowledgement.
        const uint32_t completed = kCommandCompleted;
        m.submit(object, kUnregisterMessages, 0, 0xffffffffu, &completed, 0);
        m.submit(object, 12, 7, 0xffffffffu, nullptr, 0);
        check(m.count(object) == 0, "unregistered messages are not queued");

        check(m.disconnect(object) == S_OK && m.count(object) == 1 && m.pop(object, &message) &&
                  message.kind == kDisconnected,
              "disconnecting posts its message");
        m.release(object);
    }
    {
        // A full queue drops messages and counts them in the next one that fits.
        void* object = nullptr;
        create(&object);
        Acp m(object);
        m.connect(object, 16, 2);
        const uint32_t mask = kCommandCompleted;
        m.submit(object, kRegisterMessages, 0, 0xffffffffu, &mask, 0);
        for (uint64_t id = 1; id <= 4; ++id) m.submit(object, 12, id, 0xffffffffu, nullptr, 0);
        check(m.count(object) == 2, "two messages fit");
        Message message{};
        check(m.pop(object, &message) && m.pop(object, &message), "both are taken");
        m.submit(object, 12, 5, 0xffffffffu, nullptr, 0);
        check(m.pop(object, &message) && message.dropped == 2, "the next names the two dropped");
        check(m.pop(object, nullptr) == false, "pop without room on an empty queue");
        m.release(object);
    }

    // 8. XMA: an enabled context is decoded by the shim's thread into its ring (VIctory's
    // xma_contexts_decode_into_their_output_rings, with the decoding thread doing every pass).
    {
        auto* words = static_cast<volatile uint32_t*>(ctx_all.xmaContextArray) + 16;  // context 1
        void* input = nullptr;
        uint32_t input_apu = 0;
        void* output = nullptr;
        uint32_t output_apu = 0;
        check(alloc(&input, &input_apu, 0x1000, 0x800, 1) == S_OK, "an XMA input buffer");
        check(alloc(&output, &output_apu, 0x400, 0x100, 2) == S_OK, "an XMA output ring");
        const auto stream = silent_stream();
        std::memcpy(input, stream.data(), stream.size());
        std::memset(output, 0x55, 0x400);
        // Two packets, input 0 valid, a ring of four blocks, writable, 48 kHz mono, reading from the first frame.
        words[0] = 2 | 1u << 20 | 4u << 22;
        words[1] = 1u << 31 | 3u << 27;
        words[2] = 32;
        words[5] = input_apu;
        words[7] = output_apu;

        void* object = nullptr;
        create(&object);
        Acp m(object);
        m.connect(object, 0x1000, 0x1000);
        const auto submit = [&](uint32_t command, uint32_t a, uint32_t b) {
            const uint32_t data[2] = {a, b};
            return m.submit(object, command, 0xf1, 0xffffffffu, data, 0) == S_OK;
        };
        check(submit(kEnableXma, 1, 0), "enable XMA context 1");
        const bool full = wait_for([&] { return field(words, 1, 31, 1) == 0; });
        check(full && field(words, 0, 27, 5) == 0, "the decoding thread fills the ring: four blocks, write block 0");
        const uint8_t* ring = static_cast<const uint8_t*>(output);
        bool silence = true;
        for (size_t i = 0; i < 0x400; ++i) silence &= ring[i] == 0;
        check(silence, "the ring holds the stream's silence");

        // Take four blocks at a time until the input has run out: six frames and the flushed tail are 26 blocks, so
        // the write block ends at 2 with room left in the ring.
        bool ended = false;
        for (int i = 0; i < 20 && !ended; ++i) {
            // Advancing makes the ring writable at once; the thread then fills it again, or finds the input ended.
            check(submit(kAdvanceXmaRead, 1, 4), "advance the read block");
            wait_for([&] { return field(words, 1, 31, 1) == 0 || field(words, 2, 31, 1) == 1; });
            ended = field(words, 2, 31, 1) == 1;
        }
        check(ended && field(words, 2, 26, 5) == 4, "the input ran out: error status 4 (input invalid)");
        check(field(words, 0, 20, 2) == 0, "both inputs used up");
        check(wait_for([&] { return field(words, 0, 27, 5) == 2 && field(words, 1, 31, 1) == 1; }),
              "26 blocks in all: write block 2, the ring writable");
        check(submit(kDisableXma, 1, 0), "disable it");
        m.release(object);
        free_mem(input);
        free_mem(output);
    }
    check(release_ctx(nullptr) == S_OK, "AcpHalReleaseShapeContexts succeeds");
    check(release_ctx(nullptr) == E_FAIL, "and only once");

    std::printf("acphal_test complete: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
