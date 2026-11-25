// xapu.dll: the console's audio decoder. Opus packets in, float PCM out.
//
// Ported from VIctory (crates/victory-hle/src/modules/apu.rs), GPL-3.0-or-later, by Marvisak and contributors (DexNom
// fork). Leonida is GPL-3.0-or-later. The structure layouts and sizes, command kinds, option bits, HRESULTs and result
// records are VIctory's, which ran the title with them; three fixes from VIctory's history in this repository that the
// DexNom snapshot lacks are kept (643fc6dfe: a failed result event fails the connect, a stream's channel count is
// checked before it is activated, output may be unaligned).
//
// A title connects once for one kind of processing, gets a block of memory for its packets and output, and then queues
// per-stream commands (activate, process, reset, deactivate) and dequeues one result per command. Each command is
// carried out when it is queued, so its result is ready at once; the order of results is the order of commands. The
// title (1.0.2.1) connects 256 streams of two commands each with 0x104000 bytes and options 1 (VIctory's test, "as the
// title connects"), and tests XApuDequeueResult for a negative result (docs/audio_hal_exe_calls.md).
//
// PLACEHOLDER: Leonida has no Opus decoder (VIctory uses the opus-pure crate, which is not in this repository). A
// process command parses its packet (src/opus_packet.h): a malformed one fails with the decoder's error, a valid one
// completes with the frame count and output length a decoder gives and silence for the samples. Rate conversion
// (processing kinds other than plain decoding) is refused with E_NOTIMPL, as in VIctory.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "../src/opus_packet.h"
#include "shims.h"
#include "startup_trace.h"

namespace {

constexpr HRESULT kNotImpl = static_cast<HRESULT>(0x80004001u);
constexpr HRESULT kOutOfMemory = static_cast<HRESULT>(0x8007000Eu);
constexpr HRESULT kInvalidArg = static_cast<HRESULT>(0x80070057u);
constexpr HRESULT kQueueFull = static_cast<HRESULT>(0x8AC81000u);
// No result yet, which titles poll for.
constexpr HRESULT kPendingResults = static_cast<HRESULT>(0x8AC81001u);
constexpr HRESULT kInvalidCommand = static_cast<HRESULT>(0x8AC81002u);
constexpr HRESULT kInvalidStream = static_cast<HRESULT>(0x8AC81003u);
constexpr HRESULT kAlreadyActive = static_cast<HRESULT>(0x8AC81005u);
constexpr HRESULT kBadPacket = static_cast<HRESULT>(0x8AC81008u);

// Processing kinds; only plain decoding is implemented.
constexpr uint32_t kDecodeOpus = 1;

constexpr uint32_t kActivate = 1;
constexpr uint32_t kDeactivate = 2;
constexpr uint32_t kProcess = 3;
constexpr uint32_t kNotify = 4;
constexpr uint32_t kReset = 5;

// Command option: signal the connection's event when the result is ready.
constexpr uint32_t kSignal = 1;
// Connect options.
constexpr uint32_t kDeinterleavedOutput = 4;
constexpr uint32_t kResultEvent = 8;

// The longest Opus packet duration at 48 kHz (120 ms).
constexpr int kMaxFrames = rt::opus::kMaxPacketSamples;

struct ConnectInput {
    uint32_t process;
    uint32_t streams;
    uint32_t commands_per_stream;
    uint32_t memory;
    uint32_t frames_per_output;
    uint32_t options;
};
static_assert(sizeof(ConnectInput) == 24);

struct ConnectOutput {
    void* memory;
    HANDLE result_event;
};
static_assert(sizeof(ConnectOutput) == 16);

#pragma pack(push, 1)
// What every command starts with, and what its result echoes.
struct CommandId {
    uint32_t kind;
    uint32_t stream;
    uint32_t sequence;
    uint32_t options;
    uint64_t context;
};
static_assert(sizeof(CommandId) == 0x18);

struct Activate {
    CommandId id;
    uint32_t channels;
    uint32_t init_length;
    const void* init;
    float start_pitch;
    uint32_t processing_length;
    void* processing;
};
static_assert(sizeof(Activate) == 0x38 && offsetof(Activate, channels) == 0x18);

struct Process {
    CommandId id;
    float pitch_ramp;
    float target_pitch;
    uint32_t first_frame;
    uint32_t frame_count;
    uint32_t input_length;
    const uint8_t* input;
    uint32_t output_length;
    uint8_t* output;
};
static_assert(sizeof(Process) == 0x40 && offsetof(Process, input) == 0x2c && offsetof(Process, output) == 0x38);

struct ApuResult {
    CommandId id;
    uint32_t state;
    uint64_t state_data;
    uint64_t usec;
    HRESULT error;
    uint32_t frames;
    uint32_t output_length;
    uint8_t* output;
    float pitch;
    uint32_t input_length;
    const uint8_t* input;
};
static_assert(sizeof(ApuResult) == 0x50 && offsetof(ApuResult, error) == 0x2c && offsetof(ApuResult, input) == 0x48);
#pragma pack(pop)

// An active stream: what its decoder was opened with.
struct Stream {
    bool active = false;
    uint32_t channels = 0;
};

struct Connection {
    uint8_t* memory = nullptr;
    HANDLE event = nullptr;
    uint32_t options = 0;
    uint32_t per_stream = 0;
    std::mutex mutex;
    std::vector<Stream> streams;
    std::vector<uint32_t> queued;  // results not yet dequeued, per stream
    std::deque<ApuResult> results;
    uint32_t commands = 0;
    uint32_t dequeued = 0;
};

void trace_call(const char* api, const std::string& detail) {
    if (!rt::startup_trace::enabled()) return;
    rt::startup_trace::event("xapu", std::string("\"api\":\"") + api + "\"" + detail);
}

void warn_silence_once() {
    static std::atomic<bool> warned{false};
    if (warned.exchange(true, std::memory_order_relaxed)) return;
    std::fprintf(stderr,
                 "[rt] xapu: Opus packets decode to silence (no Opus decoder in Leonida yet); frame counts, lengths "
                 "and errors are the packets' own (logged once)\n");
}

// Writes `frames` frames of interleaved PCM to the title's output: interleaved, or for stereo with
// kDeinterleavedOutput the left channel's frames, then the right's. As bytes: the output need not be aligned for
// floats.
void write_output(uint8_t* out, const float* pcm, size_t frames, size_t channels, bool deinterleaved) {
    if (deinterleaved && channels == 2) {
        for (size_t i = 0; i < frames; ++i) {
            std::memcpy(out + 4 * i, &pcm[2 * i], 4);
            std::memcpy(out + 4 * (frames + i), &pcm[2 * i + 1], 4);
        }
    } else {
        std::memcpy(out, pcm, frames * channels * 4);
    }
}

// Decodes one packet into the command's output buffer, keeping the frames from first_frame on, at most frame_count of
// them. Placeholder: the decoded samples are silence (see the top of the file).
void process(const Stream& stream, uint32_t options, const Process& command, ApuResult& result) {
    static const float silence[kMaxFrames * 2] = {};
    const size_t channels = stream.channels;
    result.input = command.input;
    result.input_length = command.input_length;
    result.output = command.output;
    if (!command.input || !command.output || command.input_length == 0) {
        result.error = kInvalidArg;
        return;
    }
    const int decoded = rt::opus::packet_samples(command.input, command.input_length, kMaxFrames);
    if (decoded < 0) {
        result.error = kBadPacket;
        return;
    }
    warn_silence_once();
    const float* pcm = silence;  // `decoded` frames of `channels` samples
    const size_t first = std::min<size_t>(command.first_frame, static_cast<size_t>(decoded));
    const size_t frames = std::min({static_cast<size_t>(decoded) - first, static_cast<size_t>(command.frame_count),
                                    command.output_length / (4 * channels)});
    write_output(command.output, pcm + first * channels, frames, channels, (options & kDeinterleavedOutput) != 0);
    result.frames = static_cast<uint32_t>(frames);
    result.output_length = static_cast<uint32_t>(frames * channels * 4);
}

HRESULT __stdcall XApuConnect(const ConnectInput* input, ConnectOutput* output, Connection** handle) {
    if (!input || !output || !handle) return kInvalidArg;
    if (input->streams < 1 || input->streams > 500 || input->commands_per_stream < 1 ||
        input->commands_per_stream > 16 || input->memory == 0)
        return kInvalidArg;
    if (input->process != kDecodeOpus) {
        // Rate conversion (decode-convert, PCM convert) is not implemented, as in VIctory; a resampler would add it.
        std::fprintf(stderr, "[rt] xapu: processing kind %u not implemented; E_NOTIMPL\n", input->process);
        return kNotImpl;
    }
    if (input->frames_per_output != 0) return kInvalidArg;
    // Zeroed, page-aligned (VIctory asks for 16).
    auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, input->memory, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!memory) return kOutOfMemory;
    HANDLE event = nullptr;
    if (input->options & kResultEvent) {
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset, not signalled, no name
        if (!event) {
            VirtualFree(memory, 0, MEM_RELEASE);
            return kOutOfMemory;
        }
    }
    auto* connection = new Connection;
    connection->memory = memory;
    connection->event = event;
    connection->options = input->options;
    connection->per_stream = input->commands_per_stream;
    connection->streams.resize(input->streams);
    connection->queued.assign(input->streams, 0);
    output->memory = memory;
    output->result_event = event;
    *handle = connection;
    std::fprintf(stderr, "[rt] xapu: audio decoder connected (%u streams, %u commands each, 0x%x bytes, options 0x%x)\n",
                 input->streams, input->commands_per_stream, input->memory, input->options);
    trace_call("XApuConnect", ",\"streams\":" + std::to_string(input->streams) +
                                  ",\"memory\":" + std::to_string(input->memory) +
                                  ",\"options\":" + std::to_string(input->options));
    return S_OK;
}

// The title passes a second argument (0x20, docs/audio_hal_exe_calls.md), which VIctory does not read either.
HRESULT __stdcall XApuDisconnect(Connection* handle) {
    if (!handle) return kInvalidArg;
    if (handle->event) CloseHandle(handle->event);
    VirtualFree(handle->memory, 0, MEM_RELEASE);
    delete handle;
    trace_call("XApuDisconnect", "");
    return S_OK;
}

HRESULT __stdcall XApuEnqueueCommand(Connection* handle, const void* command, uint32_t* total) {
    if (!handle || !command) return kInvalidArg;
    CommandId id;
    std::memcpy(&id, command, sizeof id);  // every command starts with an id
    std::lock_guard lock(handle->mutex);
    const size_t stream = id.stream;
    if (stream >= handle->streams.size()) return kInvalidStream;
    if (handle->queued[stream] >= handle->per_stream) return kQueueFull;
    ApuResult result{};
    result.id = id;
    result.error = S_OK;
    result.pitch = 1.0f;
    Stream& decoder = handle->streams[stream];
    switch (id.kind) {
        case kActivate: {
            Activate activate;
            std::memcpy(&activate, command, sizeof activate);
            if (decoder.active) {
                result.error = kAlreadyActive;
            } else if (activate.channels < 1 || activate.channels > 2) {
                result.error = kInvalidArg;
            } else {
                decoder.active = true;
                decoder.channels = activate.channels;
            }
            break;
        }
        case kDeactivate: decoder = Stream{}; break;
        case kReset: break;  // no decoder state to reset (placeholder)
        case kNotify: break;
        case kProcess: {
            Process process_command;
            std::memcpy(&process_command, command, sizeof process_command);
            if (decoder.active) process(decoder, handle->options, process_command, result);
            else result.error = kInvalidStream;
            break;
        }
        default: return kInvalidCommand;
    }
    ++handle->queued[stream];
    ++handle->commands;
    handle->results.push_back(result);
    if (total) *total = handle->commands;
    if ((id.options & kSignal) && handle->event) SetEvent(handle->event);
    return S_OK;
}

HRESULT __stdcall XApuDequeueResult(Connection* handle, ApuResult* out) {
    if (!handle || !out) return kInvalidArg;
    std::lock_guard lock(handle->mutex);
    if (handle->results.empty()) return kPendingResults;
    const ApuResult result = handle->results.front();
    handle->results.pop_front();
    --handle->queued[result.id.stream];
    ++handle->dequeued;
    std::memcpy(out, &result, sizeof result);  // the title passes room for one result
    return S_OK;
}

uint32_t __stdcall XApuGetResultCount(Connection* handle, uint32_t* total) {
    if (!handle) return 0;
    std::lock_guard lock(handle->mutex);
    if (total) *total = handle->dequeued;
    return static_cast<uint32_t>(handle->results.size());
}

}  // namespace

SHIM_TABLE(xapu,
    {"xapu.dll", "XApuConnect", reinterpret_cast<void*>(&XApuConnect)},
    {"xapu.dll", "XApuDequeueResult", reinterpret_cast<void*>(&XApuDequeueResult)},
    {"xapu.dll", "XApuDisconnect", reinterpret_cast<void*>(&XApuDisconnect)},
    {"xapu.dll", "XApuEnqueueCommand", reinterpret_cast<void*>(&XApuEnqueueCommand)},
    {"xapu.dll", "XApuGetResultCount", reinterpret_cast<void*>(&XApuGetResultCount)},
);
