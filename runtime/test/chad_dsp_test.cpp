// chad.dll through the registry: connect limits, stream shapes, the stream limit, and the status block a submitted
// command completes. The flow follows VIctory's test (crates/victory-hle/src/modules/dsp.rs,
// convolution_stream_as_the_title_drives_it), GPL-3.0-or-later, by Marvisak and contributors (DexNom fork); Leonida is
// GPL-3.0-or-later. No game data.
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "shims.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

constexpr HRESULT kPointer = static_cast<HRESULT>(0x80004003u);
constexpr HRESULT kOutOfMemory = static_cast<HRESULT>(0x8007000Eu);
constexpr HRESULT kInvalidArg = static_cast<HRESULT>(0x80070057u);

// Test mirrors of the measured layouts (VIctory dsp.rs).
#pragma pack(push, 1)
struct StreamDesc {
    uint32_t kind, options, frames, channels, ir_floats;
    const float* ir;
};
#pragma pack(pop)
struct Status {
    uint32_t submitted, completed, reserved;
    HRESULT result;
};
struct Command {
    float gains[2];
    const float* input;
    float* output;
    uint64_t reserved;
};
static_assert(sizeof(StreamDesc) == 0x1c && sizeof(Status) == 16 && sizeof(Command) == 0x20);

using ConnectFn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, void**);
using DisconnectFn = HRESULT(__stdcall*)(void*);
using ActivateFn = HRESULT(__stdcall*)(void*, const StreamDesc*, const Status**, void**);
using DeactivateFn = HRESULT(__stdcall*)(void*);
using SubmitFn = HRESULT(__stdcall*)(void*, const Command*, uint32_t*);

}  // namespace

int main() {
    std::printf("chad_dsp_test: registered chad.dll shim\n");
    const auto connect = reinterpret_cast<ConnectFn>(rt::shims::find("chad.dll", "XDspConnectWithMaximumStreamLimit"));
    const auto disconnect = reinterpret_cast<DisconnectFn>(rt::shims::find("chad.dll", "XDspDisconnect"));
    const auto activate = reinterpret_cast<ActivateFn>(rt::shims::find("chad.dll", "XDspActivate"));
    const auto deactivate = reinterpret_cast<DeactivateFn>(rt::shims::find("chad.dll", "XDspDeactivate"));
    const auto submit = reinterpret_cast<SubmitFn>(rt::shims::find("chad.dll", "XDspSubmitCommand"));
    check(connect && disconnect && activate && deactivate && submit, "all five exports are registered");
    check(rt::shims::owns_dll("chad.dll"), "the registry owns chad.dll");
    if (!connect || !disconnect || !activate || !deactivate || !submit) return 1;

    std::vector<uint8_t> base(0x100000);
    void* dsp = nullptr;
    check(connect(nullptr, 0x100000, 24, 0x80, 1, &dsp) == kInvalidArg, "connect: no memory");
    check(connect(base.data(), 0xffff, 24, 0x80, 1, &dsp) == kInvalidArg, "connect: under 64 KiB");
    check(connect(base.data(), 0x1F400001, 24, 0x80, 1, &dsp) == kInvalidArg, "connect: over 500 MiB");
    check(connect(base.data(), 0x100000, 0x81, 0x80, 1, &dsp) == kInvalidArg, "connect: over 128 s of response");
    check(connect(base.data(), 0x100000, 24, 0x101, 1, &dsp) == kInvalidArg, "connect: over 256 streams");
    check(connect(base.data(), 0x100000, 24, 0x80, 3, &dsp) == kInvalidArg, "connect: over two channels");
    check(connect(base.data(), 0x100000, 24, 0x80, 1, nullptr) == kPointer, "connect: no out pointer");
    check(connect(base.data(), 0x100000, 24, 0x80, 1, &dsp) == S_OK && dsp, "connect as VIctory's test does");

    // Stereo convolution with option 2, as the title asks for.
    std::vector<float> ir(0x800);
    const StreamDesc desc{1, 2, 0x400, 2, 0x800, ir.data()};
    const Status* status = nullptr;
    void* stream = nullptr;
    check(activate(dsp, &desc, nullptr, &stream) == kPointer, "activate: no status pointer");
    check(activate(dsp, &desc, &status, nullptr) == kPointer, "activate: no stream pointer");
    check(activate(nullptr, &desc, &status, &stream) == kInvalidArg, "activate: no connection");
    check(activate(dsp, nullptr, &status, &stream) == kInvalidArg, "activate: no description");
    check(activate(dsp, &desc, &status, &stream) == S_OK && status && stream, "activate a stereo convolution");
    check(status->submitted == 0 && status->completed == 0 && status->result == S_OK, "a fresh status block");

    std::vector<float> output(0x800 + 16, 1.0f);
    const std::vector<float> input(0x800, 0.5f);
    const Command command{{1.0f, 1.0f}, input.data(), output.data(), 0};
    uint32_t ticket = 0;
    check(submit(stream, &command, &ticket) == S_OK && ticket == 1, "submit: ticket 1");
    check(status->completed == ticket && status->submitted == ticket && status->result == S_OK,
          "completed at once: the status names the ticket");
    bool silent = true, guarded = true;
    for (size_t i = 0; i < 0x800; ++i) silent &= output[i] == 0.0f;
    for (size_t i = 0x800; i < output.size(); ++i) guarded &= output[i] == 1.0f;
    check(silent, "the output block is silence (placeholder transform)");
    check(guarded, "only one block of the stream written");
    check(submit(stream, &command, nullptr) == S_OK && status->completed == 2, "a second command, no ticket asked");
    const Command no_output{{1.0f, 1.0f}, input.data(), nullptr, 0};
    check(submit(stream, &no_output, &ticket) == S_OK && ticket == 3 && status->completed == 3, "a command without output");
    check(submit(nullptr, &command, &ticket) == kInvalidArg && submit(stream, nullptr, &ticket) == kInvalidArg,
          "submit: null stream or command");
    check(deactivate(stream) == S_OK, "deactivate");
    check(deactivate(nullptr) == kInvalidArg, "deactivate: null");

    // Mono streams refuse stereo options; convolutions take 512 or 1024 frames and not option 4; other transforms are
    // mono only, of 512 to 2048 frames.
    const auto shape = [&](uint32_t kind, uint32_t options, uint32_t frames, uint32_t channels) {
        const StreamDesc d{kind, options, frames, channels, 0, nullptr};
        void* s = nullptr;
        const HRESULT hr = activate(dsp, &d, &status, &s);
        if (hr == S_OK) deactivate(s);
        return hr;
    };
    check(shape(1, 2, 0x400, 1) == kInvalidArg, "a mono convolution with option 2");
    check(shape(1, 8, 0x400, 1) == kInvalidArg, "a mono convolution with option 8");
    check(shape(1, 0, 0x400, 1) == S_OK, "a mono convolution");
    check(shape(1, 0, 0x200, 2) == S_OK, "a stereo convolution of 512 frames");
    check(shape(1, 0, 0x800, 2) == kInvalidArg, "a convolution of 2048 frames");
    check(shape(1, 4, 0x400, 2) == kInvalidArg, "a convolution with option 4");
    check(shape(1, 0, 0x400, 3) == kInvalidArg, "three channels");
    check(shape(2, 0, 0x400, 1) == S_OK, "a mono transform");
    check(shape(2, 4, 0x800, 1) == S_OK, "a transform of 2048 frames with option 4");
    check(shape(2, 0, 0x400, 2) == kInvalidArg, "a stereo transform");
    check(shape(2, 0, 0x100, 1) == kInvalidArg, "a transform of 256 frames");

    // The stream limit, and a stream freed for another.
    void* limited = nullptr;
    check(connect(base.data(), 0x10000, 0, 2, 2, &limited) == S_OK, "connect for two streams");
    const StreamDesc mono{2, 0, 0x200, 1, 0, nullptr};
    void* a = nullptr;
    void* b = nullptr;
    void* c = nullptr;
    check(activate(limited, &mono, &status, &a) == S_OK && activate(limited, &mono, &status, &b) == S_OK,
          "two streams");
    check(activate(limited, &mono, &status, &c) == kOutOfMemory, "a third: out of memory");
    check(deactivate(a) == S_OK && activate(limited, &mono, &status, &c) == S_OK, "one freed, another activated");
    deactivate(b);
    deactivate(c);
    check(disconnect(limited) == S_OK, "disconnect the limited connection");
    check(disconnect(dsp) == S_OK, "disconnect");
    check(disconnect(nullptr) == kInvalidArg, "disconnect: null");

    std::printf("chad_dsp_test complete: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
