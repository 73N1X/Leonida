// xapu.dll through the registry: connect, the per-stream command queues, results in command order, the result event,
// and the Opus packet framing the placeholder decoder uses (src/opus_packet.h). The flow follows VIctory's test
// (crates/victory-hle/src/modules/apu.rs, decodes_opus_as_the_title_drives_it), GPL-3.0-or-later, by Marvisak and
// contributors (DexNom fork); Leonida is GPL-3.0-or-later. Packets are built by hand: no game data, no encoder.
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "opus_packet.h"
#include "shims.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

constexpr HRESULT kNotImpl = static_cast<HRESULT>(0x80004001u);
constexpr HRESULT kInvalidArg = static_cast<HRESULT>(0x80070057u);
constexpr HRESULT kQueueFull = static_cast<HRESULT>(0x8AC81000u);
constexpr HRESULT kPendingResults = static_cast<HRESULT>(0x8AC81001u);
constexpr HRESULT kInvalidCommand = static_cast<HRESULT>(0x8AC81002u);
constexpr HRESULT kInvalidStream = static_cast<HRESULT>(0x8AC81003u);
constexpr HRESULT kAlreadyActive = static_cast<HRESULT>(0x8AC81005u);
constexpr HRESULT kBadPacket = static_cast<HRESULT>(0x8AC81008u);

constexpr uint32_t kActivate = 1, kDeactivate = 2, kProcess = 3, kNotify = 4, kReset = 5;

// Test mirrors of the measured layouts (VIctory apu.rs).
struct ConnectInput {
    uint32_t process, streams, commands_per_stream, memory, frames_per_output, options;
};
struct ConnectOutput {
    void* memory;
    HANDLE result_event;
};
#pragma pack(push, 1)
struct CommandId {
    uint32_t kind, stream, sequence, options;
    uint64_t context;
};
struct Activate {
    CommandId id;
    uint32_t channels, init_length;
    const void* init;
    float start_pitch;
    uint32_t processing_length;
    void* processing;
};
struct Process {
    CommandId id;
    float pitch_ramp, target_pitch;
    uint32_t first_frame, frame_count, input_length;
    const uint8_t* input;
    uint32_t output_length;
    uint8_t* output;
};
struct ApuResult {
    CommandId id;
    uint32_t state;
    uint64_t state_data, usec;
    HRESULT error;
    uint32_t frames, output_length;
    uint8_t* output;
    float pitch;
    uint32_t input_length;
    const uint8_t* input;
};
#pragma pack(pop)
static_assert(sizeof(ConnectInput) == 24 && sizeof(ConnectOutput) == 16);
static_assert(sizeof(CommandId) == 0x18 && sizeof(Activate) == 0x38 && sizeof(Process) == 0x40);
static_assert(sizeof(ApuResult) == 0x50, "VIctory asserts 0x50");
static_assert(offsetof(Process, input) == 0x2c && offsetof(Process, output_length) == 0x34);
static_assert(offsetof(ApuResult, error) == 0x2c && offsetof(ApuResult, frames) == 0x30 &&
              offsetof(ApuResult, output) == 0x38 && offsetof(ApuResult, pitch) == 0x40 &&
              offsetof(ApuResult, input) == 0x48);

using ConnectFn = HRESULT(__stdcall*)(const ConnectInput*, ConnectOutput*, void**);
using DisconnectFn = HRESULT(__stdcall*)(void*);
using EnqueueFn = HRESULT(__stdcall*)(void*, const void*, uint32_t*);
using DequeueFn = HRESULT(__stdcall*)(void*, ApuResult*);
using CountFn = uint32_t(__stdcall*)(void*, uint32_t*);

ConnectFn g_connect;
DisconnectFn g_disconnect;
EnqueueFn g_enqueue;
DequeueFn g_dequeue;
CountFn g_count;

CommandId id(uint32_t kind, uint32_t stream, uint32_t options = 0) { return {kind, stream, 7, options, 0x1234}; }

Activate activate(uint32_t stream, uint32_t channels) {
    return {id(kActivate, stream), channels, 0, nullptr, 1.0f, 0, nullptr};
}

Process process(uint32_t stream, const uint8_t* input, uint32_t input_length, uint8_t* output, uint32_t output_length,
                uint32_t frame_count = 960, uint32_t first_frame = 0) {
    return {id(kProcess, stream), 0.0f, 1.0f, first_frame, frame_count, input_length, input, output_length, output};
}

ApuResult dequeue(void* handle, HRESULT* hr = nullptr) {
    ApuResult r{};
    const HRESULT h = g_dequeue(handle, &r);
    if (hr) *hr = h;
    return r;
}

// A CELT-only fullband stereo packet of one 20 ms frame (TOC config 31, code 0) with `payload` bytes.
std::vector<uint8_t> celt_packet(size_t payload, uint8_t code = 0) {
    std::vector<uint8_t> p(1 + payload, 0x5a);
    p[0] = static_cast<uint8_t>(31 << 3 | 1 << 2 | code);
    return p;
}

void opus_framing() {
    using namespace rt::opus;
    // Section 3.1: every configuration's frame duration.
    const int expected[32] = {480, 960, 1920, 2880, 480, 960, 1920, 2880, 480, 960, 1920, 2880,  // SILK
                              480, 960, 480, 960,                                            // hybrid
                              120, 240, 480, 960, 120, 240, 480, 960, 120, 240, 480, 960, 120, 240, 480, 960};  // CELT
    bool all = true;
    for (int config = 0; config < 32; ++config)
        all &= samples_per_frame(static_cast<uint8_t>(config << 3)) == expected[config];
    check(all, "opus: samples per frame for all 32 configurations");

    const uint8_t code0[] = {0xfc, 1, 2, 3};
    check(packet_samples(code0, sizeof code0) == 960, "opus: code 0, one 20 ms frame");
    const uint8_t empty_frame[] = {0xfc};
    check(packet_samples(empty_frame, 1) == 960, "opus: a TOC byte alone is one (empty) frame");
    check(packet_samples(code0, 0) == kInvalidPacket, "opus: an empty packet is invalid");
    const uint8_t code1[] = {0xfd, 1, 2, 3, 4};
    check(packet_samples(code1, sizeof code1) == 1920, "opus: code 1, two equal frames");
    check(packet_samples(code1, 4) == kInvalidPacket, "opus: code 1 with an odd payload");
    const uint8_t code2[] = {0xfe, 2, 9, 9, 8, 8, 8};
    check(packet_samples(code2, sizeof code2) == 1920, "opus: code 2, two frames, the first's size coded");
    const uint8_t code2_long[] = {0xfe, 9, 1, 2};
    check(packet_samples(code2_long, sizeof code2_long) == kInvalidPacket, "opus: code 2, a first frame past the end");
    // Code 3: CBR, VBR, padding, count limits.
    const uint8_t cbr[] = {0xff, 3, 1, 2, 3, 4, 5, 6};
    check(packet_samples(cbr, sizeof cbr) == 3 * 960, "opus: code 3 CBR, three frames");
    check(packet_samples(cbr, 7) == kInvalidPacket, "opus: code 3 CBR not divisible");
    const uint8_t vbr[] = {0xff, 0x82, 1, 7, 8, 8};
    check(packet_samples(vbr, sizeof vbr) == 1920, "opus: code 3 VBR, two frames");
    const uint8_t padded[] = {0xff, 0x41, 2, 5, 0, 0};
    check(packet_samples(padded, sizeof padded) == 960, "opus: code 3 with padding");
    const uint8_t overpadded[] = {0xff, 0x41, 9, 5};
    check(packet_samples(overpadded, sizeof overpadded) == kInvalidPacket, "opus: padding past the end");
    const uint8_t no_frames[] = {0xff, 0x00};
    check(packet_samples(no_frames, sizeof no_frames) == kInvalidPacket, "opus: code 3 with no frames");
    const uint8_t silk60x2[] = {0x1b, 2, 0, 0};  // SILK 60 ms, two frames: 120 ms
    check(packet_samples(silk60x2, sizeof silk60x2) == 5760, "opus: 120 ms is the most");
    const uint8_t silk60x3[] = {0x1b, 3, 0, 0, 0};
    check(packet_samples(silk60x3, sizeof silk60x3) == kInvalidPacket, "opus: more than 120 ms is invalid");
    std::vector<uint8_t> celt48(2 + 48, 0);  // CELT 2.5 ms, 48 frames: 120 ms
    celt48[0] = 0x83;
    celt48[1] = 48;
    check(packet_samples(celt48.data(), celt48.size()) == 5760, "opus: 48 frames of 2.5 ms");
    const auto big = celt_packet(1276);
    check(packet_samples(big.data(), big.size()) == kInvalidPacket, "opus: a frame over 1275 bytes");
    const auto biggest = celt_packet(1275);
    check(packet_samples(biggest.data(), biggest.size()) == 960, "opus: a frame of 1275 bytes");
    const uint8_t hybrid[] = {0x68, 0};
    check(packet_samples(hybrid, sizeof hybrid) == 960, "opus: hybrid 20 ms");
}

void connect_validation() {
    void* handle = nullptr;
    ConnectOutput out{};
    ConnectInput in{1, 256, 2, 0x104000, 0, 1};
    check(g_connect(nullptr, &out, &handle) == kInvalidArg, "connect: null input");
    check(g_connect(&in, nullptr, &handle) == kInvalidArg, "connect: null output");
    check(g_connect(&in, &out, nullptr) == kInvalidArg, "connect: null handle");
    ConnectInput bad = in;
    bad.streams = 0;
    check(g_connect(&bad, &out, &handle) == kInvalidArg, "connect: no streams");
    bad.streams = 501;
    check(g_connect(&bad, &out, &handle) == kInvalidArg, "connect: 501 streams");
    bad = in;
    bad.commands_per_stream = 17;
    check(g_connect(&bad, &out, &handle) == kInvalidArg, "connect: 17 commands per stream");
    bad = in;
    bad.memory = 0;
    check(g_connect(&bad, &out, &handle) == kInvalidArg, "connect: no memory");
    bad = in;
    bad.process = 2;
    check(g_connect(&bad, &out, &handle) == kNotImpl, "connect: rate conversion is not implemented");
    bad = in;
    bad.frames_per_output = 960;
    check(g_connect(&bad, &out, &handle) == kInvalidArg, "connect: frames per output need rate conversion");
    check(g_disconnect(nullptr) == kInvalidArg, "disconnect: null");
}

void decodes_as_the_title_drives_it() {
    // 256 mono/stereo streams, two commands each, as the title connects (VIctory's test).
    const ConnectInput input{1, 256, 2, 0x104000, 0, 1};
    ConnectOutput output{};
    void* handle = nullptr;
    check(g_connect(&input, &output, &handle) == S_OK && handle && output.memory, "connect as the title does");
    check(output.result_event == nullptr, "no result event without option 8");
    auto* base = static_cast<uint8_t*>(output.memory);
    bool zeroed = true;
    for (size_t i = 0; i < input.memory; i += 4096) zeroed &= base[i] == 0;
    check(zeroed, "the connection memory is zeroed");

    const Activate a = activate(3, 2);
    check(g_enqueue(handle, &a, nullptr) == S_OK, "activate stream 3, stereo");

    // A 20 ms stereo packet in the connection memory, output 0x14000 further, prefilled to see what is written.
    const auto packet = celt_packet(100);
    std::memcpy(base, packet.data(), packet.size());
    uint8_t* out = base + 0x14000;
    std::memset(out, 0xab, 0xf00 * 2 + 64);
    const Process p = process(3, base, static_cast<uint32_t>(packet.size()), out, 0xf00 * 2);
    uint32_t total = 0;
    check(g_enqueue(handle, &p, &total) == S_OK && total == 2, "process: two commands so far");
    // Two results queued for stream 3: its queue is full.
    check(g_enqueue(handle, &p, &total) == kQueueFull, "a third command for stream 3: queue full");
    check(g_count(handle, nullptr) == 2, "two results waiting");

    HRESULT hr = 0;
    ApuResult r = dequeue(handle, &hr);
    check(hr == S_OK && r.id.kind == kActivate && r.error == S_OK && r.id.context == 0x1234 && r.id.sequence == 7 &&
              r.pitch == 1.0f,
          "the activate's result first, the id echoed");
    r = dequeue(handle, &hr);
    check(hr == S_OK && r.id.kind == kProcess && r.error == S_OK && r.frames == 960 && r.output_length == 960 * 8,
          "the process's result: 960 frames, 7680 bytes");
    check(r.input == base && r.input_length == packet.size() && r.output == out, "the result names its buffers");
    bool silent = true, guarded = true;
    for (size_t i = 0; i < 960 * 8; ++i) silent &= out[i] == 0;
    for (size_t i = 960 * 8; i < 0xf00 * 2 + 64; ++i) guarded &= out[i] == 0xab;
    check(silent, "the output is silence (placeholder decoder)");
    check(guarded, "nothing written past the frames");
    dequeue(handle, &hr);
    check(hr == kPendingResults, "then no result: pending");
    uint32_t dequeued = 0;
    check(g_count(handle, &dequeued) == 0 && dequeued == 2, "two results dequeued in all");

    // A process on a stream that was never activated fails in its result.
    Process stray = p;
    stray.id.stream = 4;
    check(g_enqueue(handle, &stray, &total) == S_OK, "process on stream 4 is queued");
    r = dequeue(handle, &hr);
    check(hr == S_OK && r.error == kInvalidStream, "stream 4 was never activated");

    // Commands that are refused queue no result.
    CommandId unknown = id(9, 3);
    check(g_enqueue(handle, &unknown, &total) == kInvalidCommand, "an unknown command kind");
    CommandId past = id(kNotify, 256);
    check(g_enqueue(handle, &past, &total) == kInvalidStream, "a stream past the connection's");
    check(g_count(handle, nullptr) == 0, "neither queued a result");

    // Activation checks: once per stream, one or two channels.
    check(g_enqueue(handle, &a, nullptr) == S_OK && dequeue(handle).error == kAlreadyActive, "activate twice");
    const Activate mono = activate(5, 1), none = activate(6, 0), three = activate(7, 3);
    check(g_enqueue(handle, &mono, nullptr) == S_OK && dequeue(handle).error == S_OK, "a mono stream");
    check(g_enqueue(handle, &none, nullptr) == S_OK && dequeue(handle).error == kInvalidArg, "no channels");
    check(g_enqueue(handle, &three, nullptr) == S_OK && dequeue(handle).error == kInvalidArg, "three channels");
    const Process on_three = process(7, base, static_cast<uint32_t>(packet.size()), out, 0xf00 * 2);
    check(g_enqueue(handle, &on_three, nullptr) == S_OK && dequeue(handle).error == kInvalidStream,
          "a stream that failed to activate is not active");

    // Frames kept: from first_frame on, at most frame_count, as many as the output holds.
    Process clipped = process(5, base, static_cast<uint32_t>(packet.size()), out, 0xf00 * 2, 960, 100);
    check(g_enqueue(handle, &clipped, nullptr) == S_OK, "process from frame 100");
    r = dequeue(handle);
    check(r.error == S_OK && r.frames == 860 && r.output_length == 860 * 4, "mono: 860 frames from frame 100");
    clipped = process(5, base, static_cast<uint32_t>(packet.size()), out, 0xf00 * 2, 500);
    g_enqueue(handle, &clipped, nullptr);
    r = dequeue(handle);
    check(r.frames == 500, "at most frame_count");
    clipped = process(3, base, static_cast<uint32_t>(packet.size()), out, 803);
    g_enqueue(handle, &clipped, nullptr);
    r = dequeue(handle);
    check(r.frames == 100 && r.output_length == 800, "as many stereo frames as the output holds");
    // Unaligned output is fine.
    clipped = process(3, base, static_cast<uint32_t>(packet.size()), out + 1, 0xf00 * 2);
    g_enqueue(handle, &clipped, nullptr);
    check(dequeue(handle).frames == 960, "output at an odd address");

    // Packets the decoder refuses, and arguments it needs.
    const auto odd = celt_packet(5, 1);  // code 1 needs an even payload
    std::memcpy(base, odd.data(), odd.size());
    clipped = process(3, base, static_cast<uint32_t>(odd.size()), out, 0xf00 * 2);
    g_enqueue(handle, &clipped, nullptr);
    r = dequeue(handle);
    check(r.error == kBadPacket && r.frames == 0 && r.output_length == 0, "a malformed packet: bad packet");
    clipped = process(3, base, 0, out, 0xf00 * 2);
    g_enqueue(handle, &clipped, nullptr);
    check(dequeue(handle).error == kInvalidArg, "an empty packet: invalid argument");
    clipped = process(3, base, 4, nullptr, 0xf00 * 2);
    g_enqueue(handle, &clipped, nullptr);
    check(dequeue(handle).error == kInvalidArg, "no output: invalid argument");

    // Reset and notify complete; deactivate frees the stream for another activation.
    const CommandId reset = id(kReset, 3), notify = id(kNotify, 3), deactivate = id(kDeactivate, 3);
    check(g_enqueue(handle, &reset, nullptr) == S_OK && dequeue(handle).error == S_OK, "reset");
    check(g_enqueue(handle, &notify, nullptr) == S_OK && dequeue(handle).id.kind == kNotify, "notify");
    check(g_enqueue(handle, &deactivate, nullptr) == S_OK && dequeue(handle).error == S_OK, "deactivate");
    std::memcpy(base, packet.data(), packet.size());
    const Process after = process(3, base, static_cast<uint32_t>(packet.size()), out, 0xf00 * 2);
    check(g_enqueue(handle, &after, nullptr) == S_OK && dequeue(handle).error == kInvalidStream,
          "a deactivated stream");
    check(g_enqueue(handle, &a, nullptr) == S_OK && dequeue(handle).error == S_OK, "and it activates again");

    check(g_disconnect(handle) == S_OK, "disconnect");
}

void result_event() {
    // Option 8: a result event, signalled for commands with option 1, deinterleaved output with option 4.
    const ConnectInput input{1, 4, 4, 0x10000, 0, 8 | 4};
    ConnectOutput output{};
    void* handle = nullptr;
    check(g_connect(&input, &output, &handle) == S_OK && output.result_event, "connect with a result event");
    const HANDLE event = output.result_event;
    const Activate quiet = activate(0, 2);
    g_enqueue(handle, &quiet, nullptr);
    check(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "no signal without option 1");
    Activate loud = activate(1, 2);
    loud.id.options = 1;
    g_enqueue(handle, &loud, nullptr);
    check(WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "signalled for option 1");
    check(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "an auto-reset event");
    auto* base = static_cast<uint8_t*>(output.memory);
    const auto packet = celt_packet(10);
    std::memcpy(base, packet.data(), packet.size());
    std::memset(base + 0x1000, 0xab, 960 * 8);
    const Process p = process(1, base, static_cast<uint32_t>(packet.size()), base + 0x1000, 960 * 8);
    g_enqueue(handle, &p, nullptr);
    g_dequeue(handle, nullptr);
    check(g_dequeue(handle, nullptr) == kInvalidArg, "dequeue needs room for a result");
    ApuResult r{};
    g_dequeue(handle, &r);  // the first activate's
    g_dequeue(handle, &r);  // the second's
    g_dequeue(handle, &r);
    bool silent = true;
    for (size_t i = 0; i < 960 * 8; ++i) silent &= base[0x1000 + i] == 0;
    check(r.frames == 960 && r.output_length == 960 * 8 && silent, "deinterleaved stereo: both channels' frames");
    check(g_disconnect(handle) == S_OK, "disconnect closes the event");
}

}  // namespace

int main() {
    std::printf("xapu_test: registered xapu.dll shim\n");
    g_connect = reinterpret_cast<ConnectFn>(rt::shims::find("xapu.dll", "XApuConnect"));
    g_disconnect = reinterpret_cast<DisconnectFn>(rt::shims::find("xapu.dll", "XApuDisconnect"));
    g_enqueue = reinterpret_cast<EnqueueFn>(rt::shims::find("xapu.dll", "XApuEnqueueCommand"));
    g_dequeue = reinterpret_cast<DequeueFn>(rt::shims::find("xapu.dll", "XApuDequeueResult"));
    g_count = reinterpret_cast<CountFn>(rt::shims::find("xapu.dll", "XApuGetResultCount"));
    check(g_connect && g_disconnect && g_enqueue && g_dequeue && g_count, "all five exports are registered");
    check(rt::shims::owns_dll("xapu.dll"), "the registry owns xapu.dll");
    if (!g_connect || !g_disconnect || !g_enqueue || !g_dequeue || !g_count) return 1;

    opus_framing();
    connect_validation();
    decodes_as_the_title_drives_it();
    result_event();
    std::printf("xapu_test complete: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
