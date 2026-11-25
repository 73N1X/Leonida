// acphal.dll: Audio Control Processor HAL. Scalable Hardware Audio Processing Engine (SHAPE) and APU (Audio Processing
// Unit) memory, and the audio coprocessor's command and message queues, for Xbox Series.
//
// On a PC there is no SHAPE/ACP hardware and APU physical memory does not exist. This shim creates the APU heap from
// process memory and manages allocations within it; the SHAPE context arrays get device addresses of their own.
//
// The coprocessor and the XMA decoder are ported from VIctory (crates/victory-hle/src/modules/audio.rs),
// GPL-3.0-or-later, by Marvisak and contributors (DexNom fork); Leonida is GPL-3.0-or-later. VIctory ran the title with
// them; its command ids, message layout and SHAPE_CONTEXTS size are kept:
//   - IAcpHal (AcpHalCreate): every command completes at once and is acknowledged as the hardware would. Message
//     registrations take effect, a flowgraph completes unprocessed, a command with an id posts COMMAND_COMPLETED, and
//     its notification word is set. fmod's console build sets up the coprocessor inside System_Create and waits there
//     for its first command to be acknowledged.
//   - XMA: enabled XMA contexts are decoded on a thread of ours into their output rings, like the hardware does
//     (src/xma_context.h, src/xma_decode.h); the title uses only XMA contexts.
//   SHAPE's other units (rate conversion, EQ, filters, mixing through flowgraphs) are not emulated, so voices that need
//   them stay silent: their commands are acknowledged, not carried out (each kind is logged once). Mixed audio leaves
//   through the host's WASAPI, which the title's middleware reaches through COM like on PC (com_shim.cpp forwards
//   CoCreateInstance to the host), as in VIctory.
//
// Signatures from the public GDK reference (devdocs.xbox.com): AcpHal, APU reference, SHAPE_CONTEXTS, APU_ADDRESS.
// Measured call-site shapes and findings from the retail executable (1.0.2.1):
//   - docs/audio_hal_exe_calls.md, docs/retail_boot_chain.md, docs/retail_boot_frame_loop.md (Stop 28)
//   - PR #6885 (call sites and parameter analysis), PR #7009 (audio/HAL bring-up).

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "../src/xma_context.h"
#include "shims.h"
#include "startup_trace.h"

namespace {

// ---- SHAPE context structures (from the public GDK documentation at devdocs.xbox.com) ----
//
// These structures are defined here with their documented GDK field layouts so that sizeof() returns the correct
// per-context byte count: the stride of the arrays the title indexes.

// UINT32 bitfields packed into 32-bit words
struct SHAPE_SRC_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t blocksToSkip : 2;
    uint32_t internalSaturate : 1;
    uint32_t mixBufPeakMag : 4;
    uint32_t peakMag : 4;
    uint32_t samplingIncrement : 21;
    uint32_t reserved0 : 1;
    uint32_t cmd : 2;
    uint32_t mixBufPeakMag2 : 4;
    uint32_t peakMag2 : 4;
    uint32_t sampleCount;
    uint32_t samplePointer : 25;
    uint32_t reserved1 : 7;
    uint32_t samplingIncrementTarget : 21;
    uint32_t reserved2 : 11;
    uint32_t reserved[3];
};
static_assert(sizeof(SHAPE_SRC_CONTEXT) == 32, "SHAPE_SRC_CONTEXT is 32 bytes per GDK");

// The EQ/Comp context: 88 UINT32s = 352 bytes per GDK definition.
#pragma pack(push, 1)
struct alignas(4) SHAPE_EQCOMP_CONTEXT {
    uint8_t data[352];
};
#pragma pack(pop)
static_assert(sizeof(SHAPE_EQCOMP_CONTEXT) == 352, "SHAPE_EQCOMP_CONTEXT is 352 bytes per GDK");

struct SHAPE_FILTVOL_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t reserved0 : 2;
    uint32_t internalSaturate : 1;
    uint32_t mixBufPeakMag : 4;
    uint32_t peakMag : 4;
    uint32_t delay1 : 30;
    uint32_t headroom : 2;
    uint32_t delay2 : 30;
    uint32_t svfMode : 2;
    uint32_t gainTarget : 16;
    uint32_t gain : 16;
    uint32_t qRecip : 24;
    uint32_t reserved1 : 8;
    uint32_t qRecipTarget : 24;
    uint32_t reserved2 : 8;
    uint32_t fc : 24;
    uint32_t reserved3 : 8;
    uint32_t fcTarget : 24;
    uint32_t reserved4 : 8;
};
static_assert(sizeof(SHAPE_FILTVOL_CONTEXT) == 32, "SHAPE_FILTVOL_CONTEXT is 32 bytes per GDK");

struct SHAPE_DMA_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t reserved0 : 3;
    uint32_t mixBufPeakMag : 4;
    uint32_t peakMag : 4;
    uint32_t readPointer : 5;
    uint32_t reserved1 : 3;
    uint32_t writePointer : 5;
    uint32_t reserved2 : 3;
    uint32_t full : 1;
    uint32_t reserved3 : 15;
    uint32_t reserved4 : 9;
    uint32_t address : 23;
    uint32_t numFrames : 5;
    uint32_t reserved5 : 3;
    uint32_t floatConvert : 1;
    uint32_t reserved6 : 7;
    uint32_t numChannels : 3;
    uint32_t reserved7 : 5;
    uint32_t channel : 3;
    uint32_t reserved8 : 5;
};
static_assert(sizeof(SHAPE_DMA_CONTEXT) == 16, "SHAPE_DMA_CONTEXT is 16 bytes per GDK");

struct SHAPE_XMA_CONTEXT {
    uint32_t sizeRead0 : 12;
    uint32_t numLoops : 8;
    uint32_t validBuffer : 2;
    uint32_t sizeWrite : 5;
    uint32_t offsetWrite : 5;
    uint32_t sizeRead1 : 12;
    uint32_t sequenceNumber : 12;
    uint32_t stopSampleAddress : 8;
    uint32_t subframeOffset : 4;
    uint32_t reserved0 : 1;
    uint32_t isLooping : 1;
    uint32_t subframeSkip : 2;
    uint32_t packetMetadata : 16;
    uint32_t currentByteOffset : 26;
    uint32_t reserved1 : 6;
    uint32_t loopStartOffset : 26;
    uint32_t loopEndOffset : 6;
    // The bit fields above fill five dwords (the third holds 24 bits, so currentByteOffset starts a new one) = 20
    // bytes. The GDK's size is 64, so 11 reserved dwords follow, not 10 (with 10 the struct was 60 bytes and the
    // assert below failed on MSVC). Only the size (the array stride) is used here; the XMA decoder reads the words as
    // VIctory documents them (src/xma_context.h), not these fields.
    uint32_t reserved[11];
};
static_assert(sizeof(SHAPE_XMA_CONTEXT) == 64, "SHAPE_XMA_CONTEXT is 64 bytes per GDK");
static_assert(sizeof(SHAPE_XMA_CONTEXT) == rt::xma::kContextBytes);

struct SHAPE_PCM_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t reserved0 : 11;
    uint32_t loopCount : 8;
    uint32_t reserved1 : 8;
    uint32_t full : 1;
    uint32_t reserved2 : 15;
    uint32_t readPointer : 21;
    uint32_t reserved3 : 11;
    uint32_t loopStartWritePointer : 21;
    uint32_t reserved4 : 11;
    uint32_t loopEnd : 21;
    uint32_t reserved5 : 11;
    uint32_t mode : 1;
    uint32_t reserved6 : 15;
    uint32_t format : 2;
    uint32_t reserved7 : 14;
    // Six dwords of bit fields (24 bytes); the GDK's size is 32, so two reserved dwords, not one (28 bytes failed
    // the assert below). As for SHAPE_XMA_CONTEXT, only the size (the array stride) is used.
    uint32_t reserved[2];
};
static_assert(sizeof(SHAPE_PCM_CONTEXT) == 32, "SHAPE_PCM_CONTEXT is 32 bytes per GDK");

// Device addresses are 32 bits: ApuAlloc gives one through a 32-bit out parameter, ApuMapVirtualAddress returns one
// and ApuMapApuAddress takes one, and the executable stores and passes them as 32-bit values
// (docs/audio_hal_exe_calls.md). VIctory measured SHAPE_CONTEXTS at 0x60 bytes (audio.rs ShapeContexts, asserted in
// its startup_as_fmod_does_it test): six counts, six arrays, six 32-bit device addresses. With 64-bit addresses (120
// bytes, as this file had them) the shim wrote 24 bytes past the title's structure.
using APU_ADDRESS = uint32_t;

struct SHAPE_CONTEXTS {
    uint32_t numSrcContexts;
    uint32_t numEqCompContexts;
    uint32_t numFiltVolContexts;
    uint32_t numDmaContexts;
    uint32_t numXmaContexts;
    uint32_t numPcmContexts;
    SHAPE_SRC_CONTEXT* srcContextArray;
    SHAPE_EQCOMP_CONTEXT* eqCompContextArray;
    SHAPE_FILTVOL_CONTEXT* filtVolContextArray;
    SHAPE_DMA_CONTEXT* dmaContextArray;
    SHAPE_XMA_CONTEXT* xmaContextArray;
    SHAPE_PCM_CONTEXT* pcmContextArray;
    APU_ADDRESS apuSrcContextArray;
    APU_ADDRESS apuEqCompContextArray;
    APU_ADDRESS apuFiltVolContextArray;
    APU_ADDRESS apuDmaContextArray;
    APU_ADDRESS apuXmaContextArray;
    APU_ADDRESS apuPcmContextArray;
};
static_assert(sizeof(SHAPE_CONTEXTS) == 0x60, "SHAPE_CONTEXTS is 0x60 bytes (VIctory audio.rs)");
static_assert(offsetof(SHAPE_CONTEXTS, xmaContextArray) == 0x38,
              "xmaContextArray is at offset +0x38 per retail binary measurement");
static_assert(offsetof(SHAPE_CONTEXTS, apuSrcContextArray) == 0x48);

// Bytes per context: the GDK sizes above, or VIctory's bounds where those are larger (audio.rs CONTEXT_BYTES: rate
// conversion, DMA and XMA as fmod clears them, generous bounds for the units not seen yet). More than the stride only
// leaves room at an array's end.
constexpr std::array<size_t, 6> kContextBytes = {
    std::max<size_t>(sizeof(SHAPE_SRC_CONTEXT), 32),    std::max<size_t>(sizeof(SHAPE_EQCOMP_CONTEXT), 256),
    std::max<size_t>(sizeof(SHAPE_FILTVOL_CONTEXT), 256), std::max<size_t>(sizeof(SHAPE_DMA_CONTEXT), 16),
    std::max<size_t>(sizeof(SHAPE_XMA_CONTEXT), 64),    std::max<size_t>(sizeof(SHAPE_PCM_CONTEXT), 256),
};

// ---- Startup trace helper ----

void trace_call(const char* api, const std::string& detail) {
    if (!rt::startup_trace::enabled()) return;
    static std::mutex mutex;
    static std::set<std::string> seen;
    {
        const std::lock_guard lock(mutex);
        if (!seen.insert(std::string(api) + detail).second) return;
    }
    rt::startup_trace::event("acphal",
                             std::string("\"api\":\"") + api + "\"" + detail);
}

// ---- The APU heap ----
//
// Device addresses: the cached heap from 0x40000000, the non-cached heap after it from the next 64 KiB boundary (so a
// device address is aligned as its host address is), and the SHAPE context arrays from 0x10000000. Every buffer,
// ring and array the title hands the coprocessor maps back (ApuMapApuAddress), and the XMA decoder reaches input and
// output buffers by their device addresses.

constexpr uint32_t kApuBaseAddress = 0x40000000u;
constexpr uint32_t kContextApuBase = 0x10000000u;
constexpr uint64_t kContextApuSpan = kApuBaseAddress - kContextApuBase;
constexpr uint64_t kApuRegionAlign = 0x10000;

constexpr uint64_t round_up(uint64_t v, uint64_t align) { return (v + align - 1) & ~(align - 1); }

struct ApuRegion {
    void* base = nullptr;
    uint64_t size = 0;
    std::map<uint32_t, uint32_t> blocks;  // offset -> size of active allocations

    bool contains(const void* ptr) const {
        if (!base || !ptr) return false;
        const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
        const uintptr_t b = reinterpret_cast<uintptr_t>(base);
        return p >= b && p < b + size;
    }

    bool is_valid_address(const void* ptr, uint32_t alignment) const {
        if (!base || !ptr) return false;
        const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
        const uintptr_t b = reinterpret_cast<uintptr_t>(base);
        if (p < b || p >= b + size) return false;
        if (alignment == 0) alignment = 1;
        return ((p - b) & (static_cast<uintptr_t>(alignment) - 1)) == 0;
    }

    uint32_t to_offset(const void* ptr) const {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr) -
                                     reinterpret_cast<uintptr_t>(base));
    }

    void* from_offset(uint64_t offset) const {
        if (offset >= size) return nullptr;
        return static_cast<uint8_t*>(base) + offset;
    }

    // Whether one live block holds all of [offset, offset + len).
    bool holds(uint64_t offset, size_t len) const {
        auto it = blocks.upper_bound(static_cast<uint32_t>(offset));
        if (it == blocks.begin()) return false;
        --it;
        return offset + len <= static_cast<uint64_t>(it->first) + it->second;
    }

    void* allocate(uint32_t alloc_size, uint32_t alignment, uint32_t* out_offset) {
        if (alloc_size == 0 || alignment == 0) return nullptr;
        uint64_t at = 0;
        for (auto it = blocks.begin();; ++it) {
            at = (at + alignment - 1) & ~static_cast<uint64_t>(alignment - 1);
            const uint64_t limit = (it == blocks.end()) ? size : it->first;
            if (at + alloc_size <= limit) break;
            if (it == blocks.end()) return nullptr;  // out of memory
            at = static_cast<uint64_t>(it->first) + it->second;
        }
        const uint32_t offset = static_cast<uint32_t>(at);
        blocks.emplace(offset, alloc_size);
        uint8_t* ptr = static_cast<uint8_t*>(base) + offset;
        std::memset(ptr, 0, alloc_size);
        if (out_offset) *out_offset = offset;
        return ptr;
    }

    bool free_block(void* ptr) {
        if (!contains(ptr)) return false;
        const uint32_t offset = to_offset(ptr);
        return blocks.erase(offset) > 0;
    }

    void release() {
        if (base) {
            VirtualFree(base, 0, MEM_RELEASE);
            base = nullptr;
        }
        size = 0;
        blocks.clear();
    }
};

struct Heap {
    std::mutex mutex;
    bool created = false;
    ApuRegion cached;
    ApuRegion noncached;
    ApuRegion contexts;  // the SHAPE context arrays, one block each
    bool contexts_allocated = false;

    uint32_t noncached_apu() const {
        return kApuBaseAddress + static_cast<uint32_t>(round_up(cached.size, kApuRegionAlign));
    }

    struct Mapped {
        ApuRegion* region;
        uint32_t apu;  // the device address of the region's first byte
    };
    std::array<Mapped, 3> regions() {
        return {{{&cached, kApuBaseAddress}, {&noncached, noncached_apu()}, {&contexts, kContextApuBase}}};
    }
};

// Immortal: the XMA decoding thread reads it until the process ends, also while static destructors run.
Heap& heap() {
    static Heap* h = new Heap;
    return *h;
}

// Serializes allocating and releasing the context arrays, which also attach and detach the XMA voices. Taken before
// the heap's lock and the XMA engine's, never inside them.
std::mutex& contexts_mutex() {
    static auto* m = new std::mutex;
    return *m;
}

// `len` bytes at a device address when one live block holds them all (VIctory's heap_bytes), else null.
uint8_t* apu_bytes(uint32_t apu, size_t len) {
    Heap& h = heap();
    std::lock_guard lock(h.mutex);
    for (const auto& m : h.regions()) {
        const ApuRegion& r = *m.region;
        if (!r.base || apu < m.apu || apu - m.apu >= r.size) continue;
        const uint64_t offset = apu - m.apu;
        return r.holds(offset, len) ? static_cast<uint8_t*>(r.base) + offset : nullptr;
    }
    return nullptr;
}

// ---- The XMA decoder ----

void wake_decoder();

// Immortal, for the same reason as the heap.
rt::xma::Engine& xma_engine() {
    static auto* engine = new rt::xma::Engine(&apu_bytes, &wake_decoder);
    return *engine;
}

// Starts the decoding thread on the first enabled voice, and has it fill rings now: a voice just enabled or read from.
// Otherwise it looks every 2 ms; a 256-byte output block lasts 2.7 ms at 48 kHz mono.
void wake_decoder() {
    static const HANDLE event = []() -> HANDLE {
        const HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!e) {
            std::fprintf(stderr, "[rt] acphal: no event for the XMA decoding thread (error %lu): XMA voices stay silent\n",
                         GetLastError());
            return nullptr;
        }
        try {
            std::thread([e] {
                for (;;) {
                    xma_engine().pump();
                    WaitForSingleObject(e, 2);
                }
            }).detach();
        } catch (const std::system_error& error) {
            std::fprintf(stderr, "[rt] acphal: the XMA decoding thread did not start (%s): XMA voices stay silent\n",
                         error.what());
            CloseHandle(e);
            return nullptr;
        }
        std::fprintf(stderr, "[rt] acphal: XMA decoding thread started\n");
        return e;
    }();
    if (event) SetEvent(event);
}

// ---- The audio coprocessor (IAcpHal) ----

constexpr uint32_t kLoadFlowgraph = 1;
constexpr uint32_t kRegisterMessages = 2;
constexpr uint32_t kUnregisterMessages = 3;

constexpr uint32_t kFlowgraphCompleted = 0x02;
constexpr uint32_t kCommandCompleted = 0x10;
constexpr uint32_t kDisconnected = 0x80;

// Audio frames of 128 samples at 48 kHz.
constexpr uint64_t kFramesPerSecond = 375;

#pragma pack(push, 1)
struct Message {
    uint32_t kind;
    uint32_t dropped;
    uint32_t usec;
    uint8_t data[16];
};
#pragma pack(pop)
static_assert(sizeof(Message) == 28, "a coprocessor message is 28 bytes (VIctory audio.rs)");

// The interface object: its method table, then our state.
struct Acp {
    const void* const* vtable;
    std::mutex mutex;
    bool connected = false;
    uint32_t mask = 0;  // message kinds the title registered for
    std::deque<Message> messages;
    size_t capacity = 0;   // queue size given to Connect
    uint32_t dropped = 0;  // messages lost to a full queue since the last one queued
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();

    uint64_t usec() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count());
    }

    uint32_t frame() const { return static_cast<uint32_t>(usec() * kFramesPerSecond / 1'000'000); }

    // Queues a registered message; a full queue drops it and counts it in the next one that fits.
    void post(uint32_t kind, const void* data, size_t size) {
        if ((mask & kind) == 0) return;
        if (messages.size() >= capacity) {
            if (dropped != UINT32_MAX) ++dropped;
            return;
        }
        Message message{};
        message.kind = kind;
        message.dropped = std::exchange(dropped, 0);
        message.usec = static_cast<uint32_t>(usec());
        if (data) std::memcpy(message.data, data, std::min(size, sizeof message.data));
        messages.push_back(message);
    }
};

// Each command kind once: whether it was carried out or only acknowledged.
void note_command(uint32_t command, bool carried_out) {
    static std::atomic<uint64_t> seen{0};
    if (command >= 64) return;
    const uint64_t bit = uint64_t{1} << command;
    if (seen.fetch_or(bit, std::memory_order_relaxed) & bit) return;
    std::fprintf(stderr, "[rt] acphal: first coprocessor command %u: %s\n", command,
                 carried_out ? "carried out" : "acknowledged, not carried out");
    trace_call("AcpHal::SubmitCommand",
               ",\"command\":" + std::to_string(command) + ",\"carried_out\":" + (carried_out ? "true" : "false"));
}

// Slot 0. The title connects at boot with (0x1000, 0x1000) and checks the result.
HRESULT __stdcall acp_connect(Acp* self, uint32_t commands, uint32_t messages) {
    if (commands == 0) return E_INVALIDARG;
    std::lock_guard lock(self->mutex);
    self->connected = true;
    self->capacity = messages;
    trace_call("AcpHal::Connect", ",\"commands\":" + std::to_string(commands) +
                                      ",\"messages\":" + std::to_string(messages));
    return S_OK;
}

// Slot 1.
HRESULT __stdcall acp_disconnect(Acp* self) {
    std::lock_guard lock(self->mutex);
    if (!self->connected) return E_FAIL;
    self->post(kDisconnected, nullptr, 0);
    self->connected = false;
    trace_call("AcpHal::Disconnect", "");
    return S_OK;
}

// Slot 2. Done at once: message registrations take effect, a flowgraph completes unprocessed, XMA commands drive the
// decoder, a command with an id is acknowledged, and its notification word is set. The title's first call is
// (2, 0xf0f0f0f0, -1, config, 0), and a negative result is fatal to it (an int3 at +0x43ad94e).
HRESULT __stdcall acp_submit(Acp* self, uint32_t command, uint64_t id, uint32_t /*frame*/, const void* data,
                             uint32_t notification) {
    // The command's data starts with these 32-bit fields.
    const auto word = [data](size_t at) -> uint32_t {
        uint32_t v = 0;
        if (data) std::memcpy(&v, static_cast<const uint8_t*>(data) + at * 4, sizeof v);
        return v;
    };
    std::lock_guard lock(self->mutex);
    bool carried_out = true;
    switch (command) {
        case kRegisterMessages: self->mask |= word(0); break;
        case kUnregisterMessages: self->mask &= ~word(0); break;
        case kLoadFlowgraph: {
            const uint32_t flowgraph = word(1);
            self->post(kFlowgraphCompleted, &flowgraph, sizeof flowgraph);
            break;
        }
        default:
            // Context updates and the other SHAPE units' commands are acknowledged but not carried out.
            carried_out = xma_engine().command(command, data);
            break;
    }
    note_command(command, carried_out);
    if (id != 0) {
        uint8_t done[16];
        const uint32_t frame = self->frame();
        std::memcpy(done, &command, 4);
        std::memcpy(done + 4, &id, 8);
        std::memcpy(done + 12, &frame, 4);
        self->post(kCommandCompleted, done, sizeof done);
    }
    if (notification != 0) {
        // A 32-bit word in a heap block, which the title reads as volatile.
        if (uint8_t* word_at = apu_bytes(notification, 4)) *reinterpret_cast<volatile uint32_t*>(word_at) = 1;
    }
    return S_OK;
}

// Slot 3.
bool __stdcall acp_pop_message(Acp* self, Message* out) {
    std::lock_guard lock(self->mutex);
    if (self->messages.empty()) return false;
    const Message message = self->messages.front();
    self->messages.pop_front();
    if (out) std::memcpy(out, &message, sizeof message);  // the title passes room for one message
    return true;
}

// Slot 4.
uint32_t __stdcall acp_message_count(Acp* self) {
    std::lock_guard lock(self->mutex);
    return static_cast<uint32_t>(self->messages.size());
}

// Slot 5. VIctory's returns nothing; S_OK is what a caller that reads rax anyway sees.
HRESULT __stdcall acp_release(Acp* self) {
    trace_call("AcpHal::Release", "");
    delete self;
    return S_OK;
}

const void* const kAcpVtable[] = {
    reinterpret_cast<const void*>(&acp_connect),     reinterpret_cast<const void*>(&acp_disconnect),
    reinterpret_cast<const void*>(&acp_submit),      reinterpret_cast<const void*>(&acp_pop_message),
    reinterpret_cast<const void*>(&acp_message_count), reinterpret_cast<const void*>(&acp_release),
};

// ---- AcpHalCreate ----

HRESULT __stdcall AcpHalCreate(void** ppAcpHal) {
    if (!ppAcpHal) return E_POINTER;
    *ppAcpHal = new Acp{kAcpVtable};
    trace_call("AcpHalCreate", "");
    return S_OK;
}

// ---- AcpHalAllocateShapeContexts ----

// Once until released, as in VIctory: the XMA decoder works on the one XMA array.
HRESULT __stdcall AcpHalAllocateShapeContexts(SHAPE_CONTEXTS* ctx) {
    if (!ctx) return E_POINTER;
    const std::lock_guard serial(contexts_mutex());
    Heap& h = heap();
    const uint32_t counts[6] = {ctx->numSrcContexts, ctx->numEqCompContexts, ctx->numFiltVolContexts,
                                ctx->numDmaContexts, ctx->numXmaContexts, ctx->numPcmContexts};
    void* arrays[6] = {};
    uint32_t apu[6] = {};
    {
        std::lock_guard lock(h.mutex);
        if (h.contexts_allocated) return E_FAIL;
        uint64_t offsets[6] = {};
        uint64_t total = 0;
        for (size_t i = 0; i < 6; ++i) {
            offsets[i] = total;
            total += round_up(uint64_t{counts[i]} * kContextBytes[i], 256);
        }
        if (total > kContextApuSpan) return E_OUTOFMEMORY;
        if (total > 0) {
            void* base = VirtualAlloc(nullptr, static_cast<SIZE_T>(total), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!base) return E_OUTOFMEMORY;  // zeroed
            h.contexts.base = base;
            h.contexts.size = total;
            for (size_t i = 0; i < 6; ++i) {
                if (counts[i] == 0) continue;
                h.contexts.blocks.emplace(static_cast<uint32_t>(offsets[i]),
                                          static_cast<uint32_t>(uint64_t{counts[i]} * kContextBytes[i]));
                arrays[i] = static_cast<uint8_t*>(base) + offsets[i];
                apu[i] = kContextApuBase + static_cast<uint32_t>(offsets[i]);
            }
        }
        h.contexts_allocated = true;
    }
    ctx->srcContextArray = static_cast<SHAPE_SRC_CONTEXT*>(arrays[0]);
    ctx->eqCompContextArray = static_cast<SHAPE_EQCOMP_CONTEXT*>(arrays[1]);
    ctx->filtVolContextArray = static_cast<SHAPE_FILTVOL_CONTEXT*>(arrays[2]);
    ctx->dmaContextArray = static_cast<SHAPE_DMA_CONTEXT*>(arrays[3]);
    ctx->xmaContextArray = static_cast<SHAPE_XMA_CONTEXT*>(arrays[4]);
    ctx->pcmContextArray = static_cast<SHAPE_PCM_CONTEXT*>(arrays[5]);
    ctx->apuSrcContextArray = apu[0];
    ctx->apuEqCompContextArray = apu[1];
    ctx->apuFiltVolContextArray = apu[2];
    ctx->apuDmaContextArray = apu[3];
    ctx->apuXmaContextArray = apu[4];
    ctx->apuPcmContextArray = apu[5];
    xma_engine().attach(static_cast<uint32_t*>(arrays[4]), counts[4]);

    trace_call("AcpHalAllocateShapeContexts",
               ",\"src\":" + std::to_string(counts[0]) + ",\"dma\":" + std::to_string(counts[3]) +
                   ",\"xma\":" + std::to_string(counts[4]) + ",\"pcm\":" + std::to_string(counts[5]));
    return S_OK;
}

// ---- AcpHalReleaseShapeContexts ----

// Takes no arguments, as in VIctory: the title passes a handle read from AcpHalCreate's global
// (docs/audio_hal_exe_calls.md), which is not a SHAPE_CONTEXTS to write to.
HRESULT __stdcall AcpHalReleaseShapeContexts() {
    const std::lock_guard serial(contexts_mutex());
    Heap& h = heap();
    {
        std::lock_guard lock(h.mutex);
        if (!h.contexts_allocated) return E_FAIL;
    }
    xma_engine().detach();  // waits for a decoding pass that is under way
    {
        std::lock_guard lock(h.mutex);
        h.contexts.release();
        h.contexts_allocated = false;
    }
    trace_call("AcpHalReleaseShapeContexts", "");
    return S_OK;
}

// ---- ApuCreateHeap ----

// The console reserves both parts up front, and so does this shim. Fails once a heap exists, created here or by an
// allocation before it (VIctory's ALREADY_ALLOCATED).
HRESULT __stdcall ApuCreateHeap(uint32_t cachedSizeInBytes,
                                uint32_t nonCachedSizeInBytes) {
    constexpr HRESULT kAlreadyAllocated = static_cast<HRESULT>(0x8ACA0001u);
    Heap& h = heap();
    std::lock_guard lock(h.mutex);
    if (h.created) return kAlreadyAllocated;
    // Both parts' device addresses must fit in 32 bits from 0x40000000.
    if (kApuBaseAddress + round_up(cachedSizeInBytes, kApuRegionAlign) + nonCachedSizeInBytes > (uint64_t{1} << 32))
        return E_OUTOFMEMORY;

    if (cachedSizeInBytes > 0) {
        h.cached.base = VirtualAlloc(
            nullptr, cachedSizeInBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!h.cached.base) return E_OUTOFMEMORY;
        h.cached.size = cachedSizeInBytes;
    }

    if (nonCachedSizeInBytes > 0) {
        h.noncached.base = VirtualAlloc(
            nullptr, nonCachedSizeInBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!h.noncached.base) {
            h.cached.release();
            return E_OUTOFMEMORY;
        }
        h.noncached.size = nonCachedSizeInBytes;
    }

    h.created = true;
    trace_call("ApuCreateHeap",
               ",\"cachedSize\":" + std::to_string(cachedSizeInBytes) +
               ",\"nonCachedSize\":" + std::to_string(nonCachedSizeInBytes));
    return S_OK;
}

// ---- ApuAlloc ----

HRESULT __stdcall ApuAlloc(void** virtualAddress, uint32_t* physicalAddress,
                            uint32_t sizeInBytes, uint32_t alignmentInBytes,
                            uint32_t flags) {
    if (!virtualAddress) return E_POINTER;
    *virtualAddress = nullptr;
    if (physicalAddress) *physicalAddress = 0;

    if (sizeInBytes == 0) return E_INVALIDARG;
    if (alignmentInBytes == 0) alignmentInBytes = 1;
    if ((alignmentInBytes & (alignmentInBytes - 1)) != 0) return E_INVALIDARG;

    Heap& h = heap();
    std::lock_guard lock(h.mutex);

    // Routing: flags=2 routes to non-cached heap; flags=0 or 1 routes to cached heap.
    // If non-cached heap wasn't created or has no room, fall back to cached heap
    // because PC memory is unified and cache-coherent.
    const bool prefer_noncached = (flags == 2);
    ApuRegion* target = prefer_noncached ? &h.noncached : &h.cached;

    if (!target->base) {
        if (!prefer_noncached) {
            // Lazy default cached heap if ApuCreateHeap wasn't called yet
            constexpr uint32_t kDefaultHeapSize = 64 * 1024 * 1024;
            target->base = VirtualAlloc(nullptr, kDefaultHeapSize,
                                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!target->base) return E_OUTOFMEMORY;
            target->size = kDefaultHeapSize;
            h.created = true;
        } else {
            // Fall back to cached heap
            target = &h.cached;
            if (!target->base) return E_OUTOFMEMORY;
        }
    }

    uint32_t offset = 0;
    void* ptr = target->allocate(sizeInBytes, alignmentInBytes, &offset);
    if (!ptr && prefer_noncached && h.cached.base) {
        // Fall back to cached heap if non-cached heap is full
        target = &h.cached;
        ptr = target->allocate(sizeInBytes, alignmentInBytes, &offset);
    }

    if (!ptr) return E_OUTOFMEMORY;

    *virtualAddress = ptr;
    if (physicalAddress) {
        // Disjoint device address ranges:
        //   cached:     0x40000000 .. 0x40000000 + cached_size - 1
        //   non-cached: from the next 64 KiB boundary after the cached range
        *physicalAddress = (target == &h.noncached ? h.noncached_apu() : kApuBaseAddress) + offset;
    }

    return S_OK;
}

// ---- ApuFree ----

void __stdcall ApuFree(void* virtualAddress) {
    if (!virtualAddress) return;
    Heap& h = heap();
    {
        std::lock_guard lock(h.mutex);
        if (!h.cached.free_block(virtualAddress)) h.noncached.free_block(virtualAddress);
    }
    trace_call("ApuFree", "");
}

// ---- ApuIsVirtualAddressValid ----

BOOL __stdcall ApuIsVirtualAddressValid(void* virtualAddress, uint32_t alignment) {
    if (!virtualAddress) return FALSE;
    if (alignment == 0) alignment = 1;
    Heap& h = heap();
    std::lock_guard lock(h.mutex);
    for (const auto& m : h.regions())
        if (m.region->is_valid_address(virtualAddress, alignment)) return TRUE;
    return FALSE;
}

// ---- ApuMapApuAddress ----

void* __stdcall ApuMapApuAddress(uint32_t apu) {
    if (apu == 0) return nullptr;
    Heap& h = heap();
    std::lock_guard lock(h.mutex);
    for (const auto& m : h.regions())
        if (m.region->base && apu >= m.apu && apu - m.apu < m.region->size) return m.region->from_offset(apu - m.apu);
    return nullptr;
}

// ---- ApuMapVirtualAddress ----

uint32_t __stdcall ApuMapVirtualAddress(void* virtualAddress) {
    if (!virtualAddress) return 0;
    Heap& h = heap();
    std::lock_guard lock(h.mutex);
    for (const auto& m : h.regions())
        if (m.region->contains(virtualAddress)) return m.apu + m.region->to_offset(virtualAddress);
    return 0;
}

}  // namespace

SHIM_TABLE(acphal,
    {"acphal.dll", "AcpHalAllocateShapeContexts", reinterpret_cast<void*>(&AcpHalAllocateShapeContexts)},
    {"acphal.dll", "AcpHalCreate", reinterpret_cast<void*>(&AcpHalCreate)},
    {"acphal.dll", "AcpHalReleaseShapeContexts", reinterpret_cast<void*>(&AcpHalReleaseShapeContexts)},
    {"acphal.dll", "ApuAlloc", reinterpret_cast<void*>(&ApuAlloc)},
    {"acphal.dll", "ApuCreateHeap", reinterpret_cast<void*>(&ApuCreateHeap)},
    {"acphal.dll", "ApuFree", reinterpret_cast<void*>(&ApuFree)},
    {"acphal.dll", "ApuIsVirtualAddressValid", reinterpret_cast<void*>(&ApuIsVirtualAddressValid)},
    {"acphal.dll", "ApuMapApuAddress", reinterpret_cast<void*>(&ApuMapApuAddress)},
    {"acphal.dll", "ApuMapVirtualAddress", reinterpret_cast<void*>(&ApuMapVirtualAddress)},
);
