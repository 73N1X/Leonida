// chad.dll: the console's audio DSP. Streams that convolve blocks of float samples with an impulse response (reverb),
// or run other block transforms, on a coprocessor.
//
// Ported from VIctory (crates/victory-hle/src/modules/dsp.rs), GPL-3.0-or-later, by Marvisak and contributors (DexNom
// fork). Leonida is GPL-3.0-or-later. The structure layouts, limits, option bits and HRESULTs are VIctory's, which ran
// the title with them.
//
// A title connects once with a memory block for the DSP, activates streams, and submits one command per block (gains,
// an input and an output buffer). Activation hands back a status block whose second word is the ticket of the last
// completed command and whose fourth is its result; the title checks it one block later and treats a late command as
// an overrun. Commands complete when submitted here. The title (1.0.2.1) connects for 0x80 streams (its fourth
// argument, docs/audio_hal_exe_calls.md) and activates stereo convolutions with option 2 (VIctory's test, "as the
// title asks for").
//
// PLACEHOLDER, as in VIctory: the transforms themselves are not computed. The hardware takes impulse responses already
// in its own partitioned frequency-domain layout, which is not known. Outputs are silence, so reverb returns are dry;
// the completion (status words and tickets) is real.
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "shims.h"
#include "startup_trace.h"

namespace {

constexpr HRESULT kPointer = static_cast<HRESULT>(0x80004003u);
constexpr HRESULT kOutOfMemory = static_cast<HRESULT>(0x8007000Eu);
constexpr HRESULT kInvalidArg = static_cast<HRESULT>(0x80070057u);

constexpr uint32_t kConvolution = 1;
// Option bits that need a stereo stream, and one convolution refuses.
constexpr uint32_t kStereoOptions = 2 | 8;
constexpr uint32_t kNotForConvolution = 4;

struct Dsp {
    uint32_t max_streams = 0;
    std::mutex mutex;
    uint32_t active = 0;
};

#pragma pack(push, 1)
// What activation describes.
struct StreamDesc {
    uint32_t kind;
    uint32_t options;
    uint32_t frames;
    uint32_t channels;
    uint32_t ir_floats;
    const float* ir;
};
#pragma pack(pop)
static_assert(sizeof(StreamDesc) == 0x1c && offsetof(StreamDesc, ir) == 0x14);

// The status block a stream's owner polls.
struct Status {
    uint32_t submitted;
    uint32_t completed;
    uint32_t reserved;
    HRESULT result;
};
static_assert(sizeof(Status) == 16 && offsetof(Status, completed) == 4 && offsetof(Status, result) == 12);

struct Stream {
    Dsp* dsp = nullptr;
    uint32_t frames = 0;
    uint32_t channels = 0;
    Status status{};  // handed to the title; lives as long as the stream
};

// One block of work.
struct Command {
    float gains[2];
    const float* input;
    float* output;
    uint64_t reserved;
};
static_assert(sizeof(Command) == 0x20 && offsetof(Command, input) == 8 && offsetof(Command, output) == 0x10);

void trace_call(const char* api, const std::string& detail) {
    if (!rt::startup_trace::enabled()) return;
    rt::startup_trace::event("chad", std::string("\"api\":\"") + api + "\"" + detail);
}

bool valid(const StreamDesc& desc) {
    const bool shape = desc.kind == kConvolution
                           ? desc.channels >= 1 && desc.channels <= 2 && (desc.frames == 0x200 || desc.frames == 0x400) &&
                                 (desc.options & kNotForConvolution) == 0
                           : desc.channels == 1 && (desc.frames == 0x200 || desc.frames == 0x400 || desc.frames == 0x800);
    return shape && ((desc.options & kStereoOptions) == 0 || desc.channels == 2);
}

HRESULT __stdcall XDspConnectWithMaximumStreamLimit(void* base, uint32_t base_length, uint32_t ir_seconds,
                                                    uint32_t max_streams, uint32_t max_channels, Dsp** out) {
    if (!base || base_length < 0x10000 || base_length > 0x1F400000 || ir_seconds > 0x80 || max_streams > 0x100 ||
        max_channels > 2)
        return kInvalidArg;
    if (!out) return kPointer;
    auto* dsp = new Dsp;
    dsp->max_streams = max_streams;
    *out = dsp;
    std::fprintf(stderr,
                 "[rt] chad: audio DSP connected (0x%x bytes, %u s of impulse response, %u streams, %u channels); "
                 "block transforms output silence\n",
                 base_length, ir_seconds, max_streams, max_channels);
    trace_call("XDspConnectWithMaximumStreamLimit",
               ",\"length\":" + std::to_string(base_length) + ",\"streams\":" + std::to_string(max_streams));
    return S_OK;
}

// Disconnected once, after its streams.
HRESULT __stdcall XDspDisconnect(Dsp* dsp) {
    if (!dsp) return kInvalidArg;
    delete dsp;
    trace_call("XDspDisconnect", "");
    return S_OK;
}

HRESULT __stdcall XDspActivate(Dsp* dsp, const StreamDesc* desc, const Status** status, Stream** stream) {
    if (!status || !stream) return kPointer;
    if (!dsp || !desc) return kInvalidArg;
    StreamDesc d;
    std::memcpy(&d, desc, sizeof d);
    if (!valid(d)) return kInvalidArg;
    {
        std::lock_guard lock(dsp->mutex);
        if (dsp->active >= dsp->max_streams) return kOutOfMemory;
        ++dsp->active;
    }
    auto* created = new Stream;
    created->dsp = dsp;
    created->frames = d.frames;
    created->channels = d.channels;
    created->status.result = S_OK;
    *status = &created->status;
    *stream = created;
    trace_call("XDspActivate", ",\"kind\":" + std::to_string(d.kind) + ",\"options\":" + std::to_string(d.options) +
                                   ",\"frames\":" + std::to_string(d.frames) +
                                   ",\"channels\":" + std::to_string(d.channels));
    return S_OK;
}

// Deactivated once; the connection outlives its streams.
HRESULT __stdcall XDspDeactivate(Stream* stream) {
    if (!stream) return kInvalidArg;
    {
        std::lock_guard lock(stream->dsp->mutex);
        --stream->dsp->active;
    }
    delete stream;
    return S_OK;
}

// Completes the command at once: its output block is silence.
HRESULT __stdcall XDspSubmitCommand(Stream* stream, const Command* command, uint32_t* ticket) {
    if (!stream || !command) return kInvalidArg;
    // The title's output buffer holds one block of the stream.
    if (command->output) std::memset(command->output, 0, size_t{stream->frames} * stream->channels * sizeof(float));
    // The status block is read by title code on other threads: the result before the completed ticket.
    Status& status = stream->status;
    const uint32_t next = status.submitted + 1;
    std::atomic_ref<uint32_t>(status.submitted).store(next, std::memory_order_release);
    std::atomic_ref<HRESULT>(status.result).store(S_OK, std::memory_order_release);
    std::atomic_ref<uint32_t>(status.completed).store(next, std::memory_order_release);
    if (ticket) *ticket = next;
    return S_OK;
}

}  // namespace

SHIM_TABLE(chad_dsp,
    {"chad.dll", "XDspActivate", reinterpret_cast<void*>(&XDspActivate)},
    {"chad.dll", "XDspConnectWithMaximumStreamLimit", reinterpret_cast<void*>(&XDspConnectWithMaximumStreamLimit)},
    {"chad.dll", "XDspDeactivate", reinterpret_cast<void*>(&XDspDeactivate)},
    {"chad.dll", "XDspDisconnect", reinterpret_cast<void*>(&XDspDisconnect)},
    {"chad.dll", "XDspSubmitCommand", reinterpret_cast<void*>(&XDspSubmitCommand)},
);
