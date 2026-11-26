// gameinput (no SDL3): GameInput v0 for gamepads and the keyboard, from XInput, a wired DualSense and the keyboard.
//
// Ported from VIctory (crates/victory-hle/src/modules/gameinput.rs and gameinput/dualsense.rs), GPL-3.0-or-later, by
// Marvisak and contributors (DexNom fork). Leonida is GPL-3.0-or-later. VIctory runs the title with this design; its
// slot order, error codes, device ids at +0x20, reading history and callback rules are kept:
//
//   - devices: XInput slots 0-3, a stand-in pad the keyboard drives while no other pad is connected, the keyboard
//     itself (GameInputKindKeyboard readings: scan codes and virtual keys), and a wired DualSense read over HID when
//     no XInput slot holds a pad (Steam Input's virtual pad, which is XInput, wins over it);
//   - a poll thread reads the host every 8 ms, makes a new reading per device whenever its state changes (32 kept
//     per device for GetNextReading / GetPreviousReading), and runs device callbacks when a device comes or goes;
//   - RegisterDeviceCallback with blocking enumeration reports the devices already there on the caller's thread
//     before it returns (the title registers holding a lock its callback takes again); asynchronous enumeration
//     reports them on the next poll; UnregisterCallback waits for a callback that is running, and fails from inside it;
//   - SetRumbleState drives an XInput pad's two motors; trigger motors are dropped (XInput has none).
//
// Not implemented, as in VIctory: reading callbacks never run (the title polls), the dispatcher and aggregate devices
// (E_NOTIMPL), force feedback, haptics and raw device I/O (E_NOTIMPL); mouse, touch, motion, sticks and wheels have no
// readings. Deviations: QueryInterface answers the object's own v0 IIDs (VIctory answers none), GetBatteryState writes
// a zeroed state (not present) instead of nothing, and the keyboard pad can be turned off (LEONIDA_KEYBOARD_PAD=off).
//
// gameinput.cpp (SDL3) is the shim when SDL3 is available; this file is built in its place otherwise (CMakeLists.txt),
// instead of binding the title's import to the host's GameInput.dll, which has no keyboard pad.
// LEONIDA_GAMEINPUT=host forwards GameInputCreate to the host's GameInput.dll as before.
//
// The keyboard pad's keys (VIctory's README): Esc Menu, Tab View, Enter A, Backspace B, X X, Y Y, arrows the d-pad,
// Q/E the shoulders, F/H the stick clicks, W/A/S/D the left stick, I/J/K/L the right stick, Z/C the triggers.
//
// Portable outside the host source (XInput, GetAsyncKeyState, HID), which is Win32 and stays behind _WIN32.
#include "gameinput_xinput.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <xinput.h>
#ifdef _MSC_VER
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#endif
#endif

#include "shims.h"

namespace rt::gameinput_xinput {
namespace {

using namespace gi;
using Clock = std::chrono::steady_clock;

constexpr GUID kIUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

constexpr uint32_t kKeyboardKind = GameInputKindKeyboard;
constexpr uint32_t kGamepadKind = GameInputKindGamepad;
// The status of a connected device: connected, input enabled.
constexpr uint32_t kStatusConnected = GameInputDeviceConnected | GameInputDeviceInputEnabled;
constexpr size_t kHistory = 32;
constexpr auto kPollPeriod = std::chrono::milliseconds(8);

// Microseconds since the first call: the clock of every timestamp handed out.
uint64_t now_us() {
    static const Clock::time_point start = Clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

uint32_t kind_of(int index) { return index == kKeyboard ? kKeyboardKind : kGamepadKind; }

bool same_pad(const PadState& a, const PadState& b) {
    return a.buttons == b.buttons && a.leftTrigger == b.leftTrigger && a.rightTrigger == b.rightTrigger &&
           a.leftThumbstickX == b.leftThumbstickX && a.leftThumbstickY == b.leftThumbstickY &&
           a.rightThumbstickX == b.rightThumbstickX && a.rightThumbstickY == b.rightThumbstickY;
}

bool same_keys(const std::vector<KeyState>& a, const std::vector<KeyState>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].scan_code != b[i].scan_code || a[i].code_point != b[i].code_point ||
            a[i].virtual_key != b[i].virtual_key || a[i].dead_key != b[i].dead_key)
            return false;
    return true;
}

// What a reading holds: a pad's state, or the keys held.
struct Value {
    bool keys = false;
    PadState pad{};
    std::vector<KeyState> held;

    static Value of_pad(const PadState& p) {
        Value v;
        v.pad = p;
        return v;
    }
    static Value of_keys(std::vector<KeyState> k) {
        Value v;
        v.keys = true;
        v.held = std::move(k);
        return v;
    }
    bool operator==(const Value& o) const {
        return keys == o.keys && (keys ? same_keys(held, o.held) : same_pad(pad, o.pad));
    }
    uint32_t kind() const { return keys ? kKeyboardKind : kGamepadKind; }
};

class Hub;

HRESULT answer(void* self, REFIID iid, const GUID& own, void** out) {
    if (!out) return E_POINTER;
    if (iid == kIUnknown || iid == own) {
        *out = self;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

// A device lives as long as the process: AddRef and Release count nothing, as in VIctory.
class Device final : public IGameInputDevice {
public:
    Hub* hub = nullptr;
    int index = 0;
    GameInputDeviceInfo info{};

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        return answer(static_cast<IGameInputDevice*>(this), iid, IID_IGameInputDevice, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    const GameInputDeviceInfo* STDMETHODCALLTYPE GetDeviceInfo() override { return &info; }
    uint32_t STDMETHODCALLTYPE GetDeviceStatus() override;
    void STDMETHODCALLTYPE GetBatteryState(GameInputBatteryState* state) override {
        if (state) *state = GameInputBatteryState{};  // wired, no battery (GameInputBatteryNotPresent)
    }
    HRESULT STDMETHODCALLTYPE CreateForceFeedbackEffect(uint32_t, const void*, void** effect) override {
        if (effect) *effect = nullptr;
        return unsupported("CreateForceFeedbackEffect");
    }
    bool STDMETHODCALLTYPE IsForceFeedbackMotorPoweredOn(uint32_t) override { return false; }
    void STDMETHODCALLTYPE SetForceFeedbackMotorGain(uint32_t, float) override {}
    HRESULT STDMETHODCALLTYPE SetHapticMotorState(uint32_t, const void*) override {
        return unsupported("SetHapticMotorState");
    }
    void STDMETHODCALLTYPE SetRumbleState(const GameInputRumbleParams* params) override;
    void STDMETHODCALLTYPE SetInputSynchronizationState(bool) override {}
    void STDMETHODCALLTYPE SendInputSynchronizationHint() override {}
    void STDMETHODCALLTYPE PowerOff() override {}
    HRESULT STDMETHODCALLTYPE CreateRawDeviceReport(uint32_t, int32_t, void** report) override {
        if (report) *report = nullptr;
        return unsupported("CreateRawDeviceReport");
    }
    HRESULT STDMETHODCALLTYPE GetRawDeviceFeature(uint32_t, void** report) override {
        if (report) *report = nullptr;
        return unsupported("GetRawDeviceFeature");
    }
    HRESULT STDMETHODCALLTYPE SetRawDeviceFeature(void*) override { return unsupported("SetRawDeviceFeature"); }
    HRESULT STDMETHODCALLTYPE SendRawDeviceOutput(void*) override { return unsupported("SendRawDeviceOutput"); }
    HRESULT STDMETHODCALLTYPE SendRawDeviceOutputWithResponse(void*, void** response) override {
        if (response) *response = nullptr;
        return unsupported("SendRawDeviceOutputWithResponse");
    }
    HRESULT STDMETHODCALLTYPE ExecuteRawDeviceIoControl(uint32_t, size_t, const void*, size_t, void*,
                                                        size_t* size) override {
        if (size) *size = 0;
        return unsupported("ExecuteRawDeviceIoControl");
    }
    bool STDMETHODCALLTYPE AcquireExclusiveRawDeviceAccess(uint64_t) override { return false; }
    void STDMETHODCALLTYPE ReleaseExclusiveRawDeviceAccess() override {}

private:
    // Logged once per method.
    static HRESULT unsupported(const char* method) {
        static std::mutex mu;
        static std::vector<const char*> logged;
        std::lock_guard lock(mu);
        if (std::find(logged.begin(), logged.end(), method) == logged.end()) {
            logged.push_back(method);
            std::fprintf(stderr, "[rt] gameinput: IGameInputDevice::%s is not implemented (E_NOTIMPL)\n", method);
        }
        return E_NOTIMPL;
    }
};

// A reading: counted, freed with its last reference. The hub's history holds one.
class Reading final : public IGameInputReading {
public:
    Reading(Device* device, uint64_t sequence, uint64_t timestamp, Value value)
        : device_(device), sequence_(sequence), timestamp_(timestamp), value_(std::move(value)) {}

    const Value& value() const { return value_; }
    uint64_t sequence() const { return sequence_; }
    uint64_t timestamp() const { return timestamp_; }
    Device* device() const { return device_; }
    uint32_t kind() const { return value_.kind(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        const HRESULT hr = answer(static_cast<IGameInputReading*>(this), iid, IID_IGameInputReading, out);
        if (hr == S_OK) AddRef();
        return hr;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (left == 0) delete this;
        return left;
    }
    uint32_t STDMETHODCALLTYPE GetInputKind() override { return kind(); }
    uint64_t STDMETHODCALLTYPE GetSequenceNumber(uint32_t kind_mask) override {
        return (kind() & kind_mask) ? sequence_ : 0;
    }
    uint64_t STDMETHODCALLTYPE GetTimestamp() override { return timestamp_; }
    // Devices need no reference: they live as long as the process.
    void STDMETHODCALLTYPE GetDevice(IGameInputDevice** device) override {
        if (device) *device = device_;
    }
    bool STDMETHODCALLTYPE GetRawReport(void** report) override {
        if (report) *report = nullptr;
        return false;
    }
    uint32_t STDMETHODCALLTYPE GetControllerAxisCount() override { return 0; }
    uint32_t STDMETHODCALLTYPE GetControllerAxisState(uint32_t, float*) override { return 0; }
    uint32_t STDMETHODCALLTYPE GetControllerButtonCount() override { return 0; }
    uint32_t STDMETHODCALLTYPE GetControllerButtonState(uint32_t, bool*) override { return 0; }
    uint32_t STDMETHODCALLTYPE GetControllerSwitchCount() override { return 0; }
    uint32_t STDMETHODCALLTYPE GetControllerSwitchState(uint32_t, GameInputSwitchPosition*) override { return 0; }
    uint32_t STDMETHODCALLTYPE GetKeyCount() override {
        return value_.keys ? static_cast<uint32_t>(value_.held.size()) : 0;
    }
    uint32_t STDMETHODCALLTYPE GetKeyState(uint32_t count, void* out) override {
        if (!value_.keys || !out) return 0;
        const size_t n = std::min<size_t>(value_.held.size(), count);
        std::memcpy(out, value_.held.data(), n * sizeof(KeyState));
        return static_cast<uint32_t>(n);
    }
    bool STDMETHODCALLTYPE GetMouseState(void*) override { return false; }
    uint32_t STDMETHODCALLTYPE GetTouchCount() override { return 0; }
    uint32_t STDMETHODCALLTYPE GetTouchState(uint32_t, void*) override { return 0; }
    bool STDMETHODCALLTYPE GetMotionState(void*) override { return false; }
    bool STDMETHODCALLTYPE GetArcadeStickState(void*) override { return false; }
    bool STDMETHODCALLTYPE GetFlightStickState(void*) override { return false; }
    bool STDMETHODCALLTYPE GetGamepadState(GameInputGamepadState* state) override {
        if (!state || value_.keys) return false;
        *state = value_.pad;
        return true;
    }
    bool STDMETHODCALLTYPE GetRacingWheelState(void*) override { return false; }
    bool STDMETHODCALLTYPE GetUiNavigationState(GameInputUiNavigationState*) override { return false; }

private:
    ~Reading() = default;
    std::atomic<ULONG> refs_{1};
    Device* device_;
    uint64_t sequence_;
    uint64_t timestamp_;
    Value value_;
};

struct Registration {
    uint64_t token = 0;
    IGameInputDevice* device = nullptr;  // one device, or null for any
    uint32_t kind = 0;
    uint32_t status = 0;
    void* context = nullptr;
    GameInputDeviceCallback callback = nullptr;
};

struct Event {
    Registration registration;
    int device = 0;
    uint64_t timestamp = 0;
    uint32_t current = 0;
    uint32_t previous = 0;
};

struct Slot {
    bool connected = false;
    uint64_t sequence = 0;
    std::deque<Reading*> history;  // one reference each
};

std::atomic<uint64_t> g_next_token{1};

class Hub final : public IGameInput {
public:
    Hub(std::unique_ptr<Source> source, bool stand_in_enabled)
        : source_(std::move(source)), stand_in_enabled_(stand_in_enabled) {
        for (int i = 0; i < kDevices; ++i) {
            Device& d = devices_[i];
            d.hub = this;
            d.index = i;
            d.info.infoSize = sizeof(GameInputDeviceInfo);
            static const char kTag[] = "leonida";
            std::memcpy(d.info.deviceId.value, kTag, sizeof kTag - 1);
            d.info.deviceId.value[31] = static_cast<uint8_t>(i + 1);
            d.info.deviceRootId = d.info.deviceId;
            d.info.supportedInput = kind_of(i);
            d.info.supportedRumbleMotors = i < kPads ? (GameInputRumbleLowFrequency | GameInputRumbleHighFrequency) : 0;
        }
    }

    Device* device(int index) { return index >= 0 && index < kDevices ? &devices_[index] : nullptr; }

    bool connected(int index) {
        std::lock_guard lock(mu_);
        return slots_[index].connected;
    }

    // Reads the host once; device callbacks run on the calling thread.
    void poll() {
        PadState pads[kPads] = {};
        bool have[kPads] = {};
        PadState native{};
        bool have_native = false;
        std::vector<KeyState> keys;
        bool any = false;
        {
            std::lock_guard lock(source_mu_);
            for (int slot = 0; slot < kPads; ++slot) any |= have[slot] = source_->pad(slot, &pads[slot]);
            // XInput first, including a virtual pad Steam Input already supplies.
            if (!any) have_native = source_->native_pad(&native);
            keys = source_->keys();
        }
        // The keyboard's pad stands in only while no other pad is there.
        const bool stand_in_on = stand_in_enabled_ && !any && !have_native;
        std::vector<Event> events;
        {
            std::lock_guard lock(mu_);
            const uint64_t now = now_us();
            events = std::exchange(pending_, {});
            for (int slot = 0; slot < kPads; ++slot) {
                const Value v = Value::of_pad(pads[slot]);
                update(slot, have[slot] ? &v : nullptr, now, events);
            }
            const Value pad = Value::of_pad(stand_in(keys));
            update(kStandIn, stand_in_on ? &pad : nullptr, now, events);
            const Value held = Value::of_keys(keys);
            update(kKeyboard, &held, now, events);
            const Value ds = Value::of_pad(native);
            update(kDualSense, have_native ? &ds : nullptr, now, events);
        }
        deliver(std::move(events));
    }

    void rumble(int index, uint16_t low, uint16_t high) {
        std::lock_guard lock(source_mu_);
        source_->rumble(index, low, high);
    }

    // ---- IGameInput ----

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        return answer(static_cast<IGameInput*>(this), iid, IID_IGameInput, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    uint64_t STDMETHODCALLTYPE GetCurrentTimestamp() override { return now_us(); }

    HRESULT STDMETHODCALLTYPE GetCurrentReading(uint32_t kind, IGameInputDevice* device,
                                                IGameInputReading** out) override {
        std::lock_guard lock(mu_);
        auto newest = [&](int i) -> Reading* {
            const auto& h = slots_[i].history;
            return !h.empty() && (h.back()->kind() & kind) ? h.back() : nullptr;
        };
        Reading* found = nullptr;
        if (!device) {
            for (int i = 0; i < kDevices; ++i)
                if (slots_[i].connected)
                    if (Reading* r = newest(i); r && (!found || r->timestamp() >= found->timestamp())) found = r;
        } else {
            const int i = index_of(device);
            if (i < 0) return fail(out, E_INVALIDARG);
            if (!slots_[i].connected) return fail(out, GAMEINPUT_E_DEVICE_DISCONNECTED);
            found = newest(i);
        }
        return hand(out, found);
    }

    HRESULT STDMETHODCALLTYPE GetNextReading(IGameInputReading* reference, uint32_t kind, IGameInputDevice* device,
                                             IGameInputReading** out) override {
        return relative(reference, kind, device, out, true);
    }
    HRESULT STDMETHODCALLTYPE GetPreviousReading(IGameInputReading* reference, uint32_t kind,
                                                 IGameInputDevice* device, IGameInputReading** out) override {
        return relative(reference, kind, device, out, false);
    }

    // The device's reading at `timestamp`, among those still kept.
    HRESULT STDMETHODCALLTYPE GetTemporalReading(uint64_t timestamp, IGameInputDevice* device,
                                                 IGameInputReading** out) override {
        std::lock_guard lock(mu_);
        const int i = index_of(device);
        if (i < 0) return fail(out, E_INVALIDARG);
        const auto& h = slots_[i].history;
        for (auto it = h.rbegin(); it != h.rend(); ++it)
            if ((*it)->timestamp() <= timestamp) return hand(out, *it);
        return hand(out, nullptr);
    }

    // Reading callbacks never run; the title registers none (it polls), as VIctory found.
    HRESULT STDMETHODCALLTYPE RegisterReadingCallback(IGameInputDevice*, uint32_t, float, void*,
                                                      GameInputReadingCallback, GameInputCallbackToken* token) override {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            std::fprintf(stderr, "[rt] gameinput: reading callbacks are registered but never called\n");
        return new_token(token);
    }

    HRESULT STDMETHODCALLTYPE RegisterDeviceCallback(IGameInputDevice* device, uint32_t kind, uint32_t status,
                                                     GameInputEnumerationKind enumeration, void* context,
                                                     GameInputDeviceCallback callback,
                                                     GameInputCallbackToken* token) override {
        if (!callback) return E_INVALIDARG;
        Registration r;
        r.token = g_next_token.fetch_add(1, std::memory_order_relaxed);
        r.device = device;
        r.kind = kind;
        r.status = status;
        r.context = context;
        r.callback = callback;
        std::vector<Event> found;
        {
            std::lock_guard lock(mu_);
            if (enumeration != GameInputNoEnumeration && (status & kStatusConnected)) {
                const uint64_t now = now_us();
                for (int i = 0; i < kDevices; ++i)
                    if (slots_[i].connected && matches(r, i)) found.push_back({r, i, now, kStatusConnected, 0});
            }
            std::fprintf(stderr, "[rt] gameinput: device callback for kinds 0x%x, enumeration %d, %zu devices now\n",
                         kind, static_cast<int>(enumeration), found.size());
            callbacks_.push_back(r);
            if (enumeration == GameInputAsyncEnumeration) {
                pending_.insert(pending_.end(), found.begin(), found.end());
                found.clear();
            }
        }
        if (token) *token = r.token;
        deliver(std::move(found));  // blocking enumeration: before this returns
        return S_OK;
    }

    // The guide button and the keyboard layout never change here.
    HRESULT STDMETHODCALLTYPE RegisterGuideButtonCallback(IGameInputDevice*, void*, GameInputGuideButtonCallback,
                                                          GameInputCallbackToken* token) override {
        return new_token(token);
    }
    HRESULT STDMETHODCALLTYPE RegisterKeyboardLayoutCallback(IGameInputDevice*, void*, GameInputKeyboardLayoutCallback,
                                                             GameInputCallbackToken* token) override {
        return new_token(token);
    }

    void STDMETHODCALLTYPE StopCallback(GameInputCallbackToken token) override {
        std::lock_guard lock(mu_);
        forget(token);
    }

    // Stops the callback, then waits up to `timeout` microseconds for a call of it that is running; false on time-out,
    // or from inside that callback (it cannot wait for itself), as documented. Either way it is never called again.
    bool STDMETHODCALLTYPE UnregisterCallback(GameInputCallbackToken token, uint64_t timeout) override {
        const std::thread::id me = std::this_thread::get_id();
        std::unique_lock lock(mu_);
        forget(token);
        // A timeout past a year is "forever" (VIctory's checked_add overflows to an endless wait for u64::MAX).
        const bool forever = timeout > 365ull * 24 * 3600 * 1000000;
        const Clock::time_point deadline = Clock::now() + std::chrono::microseconds(forever ? 0 : timeout);
        for (;;) {
            bool any = false;
            for (const auto& [t, thread] : running_) {
                if (t != token) continue;
                if (thread == me) return false;
                any = true;
            }
            if (!any) return true;
            if (forever) {
                returned_.wait(lock);
            } else if (returned_.wait_until(lock, deadline) == std::cv_status::timeout) {
                // One last look: the callback may have returned just as the wait ran out.
                bool still = false;
                for (const auto& entry : running_) still |= entry.first == token;
                return !still;
            }
        }
    }

    // No title of ours asks for these (VIctory).
    HRESULT STDMETHODCALLTYPE CreateDispatcher(IGameInputDispatcher** out) override {
        std::fprintf(stderr, "[rt] gameinput: CreateDispatcher is not implemented (E_NOTIMPL)\n");
        return fail(out, E_NOTIMPL);
    }
    HRESULT STDMETHODCALLTYPE CreateAggregateDevice(uint32_t, IGameInputDevice** out) override {
        std::fprintf(stderr, "[rt] gameinput: CreateAggregateDevice is not implemented (E_NOTIMPL)\n");
        return fail(out, E_NOTIMPL);
    }

    HRESULT STDMETHODCALLTYPE FindDeviceFromId(const AppLocalDeviceId* id, IGameInputDevice** out) override {
        if (!id || !out) return fail(out, E_INVALIDARG);
        for (Device& d : devices_)
            if (std::memcmp(d.info.deviceId.value, id->value, sizeof id->value) == 0) {
                *out = &d;
                return S_OK;
            }
        return fail(out, GAMEINPUT_E_DEVICE_NOT_FOUND);
    }
    // Our devices have no object, platform handle or platform string.
    HRESULT STDMETHODCALLTYPE FindDeviceFromObject(IUnknown*, IGameInputDevice** out) override {
        return fail(out, GAMEINPUT_E_DEVICE_NOT_FOUND);
    }
    HRESULT STDMETHODCALLTYPE FindDeviceFromPlatformHandle(HANDLE, IGameInputDevice** out) override {
        return fail(out, GAMEINPUT_E_DEVICE_NOT_FOUND);
    }
    HRESULT STDMETHODCALLTYPE FindDeviceFromPlatformString(LPCWSTR, IGameInputDevice** out) override {
        return fail(out, GAMEINPUT_E_DEVICE_NOT_FOUND);
    }
    HRESULT STDMETHODCALLTYPE EnableOemDeviceSupport(uint16_t, uint16_t, uint8_t, uint8_t) override { return S_OK; }
    void STDMETHODCALLTYPE SetFocusPolicy(uint32_t) override {}

private:
    template <class T>
    static HRESULT fail(T** out, HRESULT hr) {
        if (out) *out = nullptr;
        return hr;
    }

    // Gives the title a reference to `reading`, or fails as with none.
    static HRESULT hand(IGameInputReading** out, Reading* reading) {
        if (!out) return E_INVALIDARG;
        if (!reading) return fail(out, GAMEINPUT_E_READING_NOT_FOUND);
        reading->AddRef();
        *out = reading;
        return S_OK;
    }

    static HRESULT new_token(GameInputCallbackToken* token) {
        if (token) *token = g_next_token.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }

    int index_of(IGameInputDevice* device) const {
        for (int i = 0; i < kDevices; ++i)
            if (device == &devices_[i]) return i;
        return -1;
    }

    bool matches(const Registration& r, int index) const {
        return (kind_of(index) & r.kind) != 0 && (!r.device || r.device == &devices_[index]);
    }

    void forget(uint64_t token) {  // under mu_
        callbacks_.erase(std::remove_if(callbacks_.begin(), callbacks_.end(),
                                        [token](const Registration& r) { return r.token == token; }),
                         callbacks_.end());
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                      [token](const Event& e) { return e.registration.token == token; }),
                       pending_.end());
    }

    // The reading after (forward) or before `reference` on its device. With no device given only the reference's
    // device is walked (VIctory's shortcut: the title walks only keyboard readings and the current pad's previous one).
    HRESULT relative(IGameInputReading* reference, uint32_t kind, IGameInputDevice* device, IGameInputReading** out,
                     bool forward) {
        if (!reference) return fail(out, E_INVALIDARG);
        const auto* ref = static_cast<Reading*>(reference);
        const int i = index_of(ref->device());
        if (i < 0) return fail(out, E_INVALIDARG);
        if (device && device != ref->device()) return fail(out, E_INVALIDARG);
        std::lock_guard lock(mu_);
        const auto& h = slots_[i].history;
        if (h.empty() || ref->sequence() < h.front()->sequence()) return fail(out, GAMEINPUT_E_REFERENCE_READING_TOO_OLD);
        Reading* found = nullptr;
        if (forward) {
            for (Reading* r : h)
                if (r->sequence() > ref->sequence() && (r->kind() & kind)) {
                    found = r;
                    break;
                }
        } else {
            for (auto it = h.rbegin(); it != h.rend(); ++it)
                if ((*it)->sequence() < ref->sequence() && ((*it)->kind() & kind)) {
                    found = *it;
                    break;
                }
        }
        return hand(out, found);
    }

    // Records a device's state: a reading when it changed, callbacks when the device came or went. Under mu_.
    void update(int index, const Value* value, uint64_t now, std::vector<Event>& events) {
        Slot& s = slots_[index];
        const bool was = s.connected;
        s.connected = value != nullptr;
        if (value && (!was || s.history.empty() || !(s.history.back()->value() == *value))) {
            ++s.sequence;
            if (s.history.size() == kHistory) {
                s.history.front()->Release();
                s.history.pop_front();
            }
            s.history.push_back(new Reading(&devices_[index], s.sequence, now, *value));
        }
        if (was == s.connected) return;
        std::fprintf(stderr, "[rt] gameinput: device %d (%s) %s\n", index, name(index),
                     s.connected ? "connected" : "disconnected");
        const uint32_t current = was ? 0 : kStatusConnected, previous = was ? kStatusConnected : 0;
        for (const Registration& r : callbacks_)
            if (matches(r, index) && (r.status & kStatusConnected)) events.push_back({r, index, now, current, previous});
    }

    static const char* name(int index) {
        return index < kPads ? "XInput pad" : index == kStandIn ? "keyboard pad" : index == kKeyboard ? "keyboard"
                                                                                                       : "DualSense";
    }

    // Runs callbacks, never under the lock: they call back into the hub. One unregistered since the event was taken
    // does not run, and UnregisterCallback waits for one that runs.
    void deliver(std::vector<Event> events) {
        const std::thread::id me = std::this_thread::get_id();
        for (const Event& e : events) {
            const Registration& r = e.registration;
            {
                std::lock_guard lock(mu_);
                if (std::none_of(callbacks_.begin(), callbacks_.end(),
                                 [&](const Registration& c) { return c.token == r.token; }))
                    continue;
                running_.emplace_back(r.token, me);
            }
            r.callback(r.token, r.context, &devices_[e.device], e.timestamp, e.current, e.previous);
            {
                std::lock_guard lock(mu_);
                for (auto it = running_.begin(); it != running_.end(); ++it)
                    if (it->first == r.token && it->second == me) {
                        running_.erase(it);
                        break;
                    }
            }
            returned_.notify_all();
        }
    }

    Device devices_[kDevices];
    // Apart from the state: XInput can be slow, and the title reads readings every frame.
    std::mutex source_mu_;
    std::unique_ptr<Source> source_;
    bool stand_in_enabled_;
    std::mutex mu_;
    Slot slots_[kDevices];
    std::vector<Registration> callbacks_;
    std::vector<Event> pending_;  // asynchronous enumerations, delivered by the next poll
    std::vector<std::pair<uint64_t, std::thread::id>> running_;  // device callbacks running now
    std::condition_variable returned_;
};

uint32_t Device::GetDeviceStatus() { return hub->connected(index) ? kStatusConnected : 0; }

// Trigger motors are dropped: XInput has none.
void Device::SetRumbleState(const GameInputRumbleParams* params) {
    if (index >= kPads) return;
    const GameInputRumbleParams p = params ? *params : GameInputRumbleParams{};
    auto speed = [](float level) { return static_cast<uint16_t>(std::clamp(level, 0.0f, 1.0f) * 65535.0f); };
    hub->rumble(index, speed(p.lowFrequency), speed(p.highFrequency));
}

// ---- the host (Win32) ----

#ifdef _WIN32

bool env_is(const char* name, const char* value) {
    char buf[16] = {};
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
    return n > 0 && n < sizeof buf && _stricmp(buf, value) == 0;
}

// The foreground window is one of this process's.
bool focused() {
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}

// A wired DualSense through the Windows HID class driver (VIctory's dualsense.rs, which uses hidapi).
class DualSense {
public:
    ~DualSense() { close(); }

    bool poll(PadState* out) {
        if (file_ == INVALID_HANDLE_VALUE) {
            const Clock::time_point now = Clock::now();
            if (now < retry_at_) return false;
            retry_at_ = now + std::chrono::seconds(1);
            if (!open()) return false;
        }
        // Drain queued reports without waiting, bounded even when the pad sends faster than this reads.
        for (int i = 0; i < 32; ++i) {
            DWORD n = 0;
            if (!pending_) {
                ResetEvent(overlapped_.hEvent);
                if (ReadFile(file_, buffer_, report_length_, &n, &overlapped_)) {
                    take(n);
                    continue;
                }
                if (GetLastError() != ERROR_IO_PENDING) return lost();
                pending_ = true;
            }
            if (!GetOverlappedResult(file_, &overlapped_, &n, FALSE)) {
                if (GetLastError() == ERROR_IO_INCOMPLETE) break;
                return lost();
            }
            pending_ = false;
            take(n);
        }
        if (have_) *out = state_;
        return have_;
    }

private:
    static constexpr USHORT kSony = 0x054c, kDualSenseId = 0x0ce6;
    static constexpr ULONG kReportLength = 64;  // the USB input report; Bluetooth's is longer

    void take(DWORD n) {
        PadState s{};
        if (decode_dualsense(buffer_, n, &s)) {
            if (!have_) std::fprintf(stderr, "[rt] gameinput: wired DualSense input connected\n");
            state_ = s;
            have_ = true;
        }
    }

    bool lost() {
        std::fprintf(stderr, "[rt] gameinput: DualSense input disconnected\n");
        close();
        retry_at_ = Clock::now() + std::chrono::seconds(1);
        return false;
    }

    // The first present HID interface that is a DualSense's game-pad collection (usage page 1, usage 5) with the
    // USB report length.
    bool open() {
        GUID hid;
        HidD_GetHidGuid(&hid);
        const HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE) return false;
        SP_DEVICE_INTERFACE_DATA iface{};
        iface.cbSize = sizeof iface;
        bool found = false;
        for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &iface); ++i) {
            DWORD size = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &size, nullptr);
            if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
            std::vector<uint8_t> storage(size);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, nullptr, nullptr)) continue;
            // Access 0 reads the attributes of a device another process holds open; the read handle comes after.
            const HANDLE probe = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                             OPEN_EXISTING, 0, nullptr);
            if (probe == INVALID_HANDLE_VALUE) continue;
            HIDD_ATTRIBUTES attributes{};
            attributes.Size = sizeof attributes;
            bool wanted = HidD_GetAttributes(probe, &attributes) && attributes.VendorID == kSony &&
                          attributes.ProductID == kDualSenseId;
            HIDP_CAPS caps{};
            PHIDP_PREPARSED_DATA preparsed = nullptr;
            if (wanted && HidD_GetPreparsedData(probe, &preparsed)) {
                wanted = HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == 0x01 &&
                         caps.Usage == 0x05 && caps.InputReportByteLength == kReportLength;
                HidD_FreePreparsedData(preparsed);
            } else {
                wanted = false;
            }
            CloseHandle(probe);
            if (!wanted) continue;
            file_ = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            found = file_ != INVALID_HANDLE_VALUE;
        }
        SetupDiDestroyDeviceInfoList(set);
        if (!found) return false;
        overlapped_ = OVERLAPPED{};
        overlapped_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped_.hEvent) {
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
            return false;
        }
        report_length_ = kReportLength;
        pending_ = false;
        return true;
    }

    void close() {
        if (file_ != INVALID_HANDLE_VALUE) {
            if (pending_) {  // the driver must not write into the buffer after it is gone
                CancelIoEx(file_, &overlapped_);
                DWORD n = 0;
                GetOverlappedResult(file_, &overlapped_, &n, TRUE);
            }
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
        if (overlapped_.hEvent) CloseHandle(overlapped_.hEvent);
        overlapped_ = OVERLAPPED{};
        pending_ = false;
        have_ = false;
    }

    HANDLE file_ = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped_{};
    uint8_t buffer_[kReportLength] = {};
    DWORD report_length_ = kReportLength;
    bool pending_ = false;
    bool have_ = false;
    PadState state_{};
    Clock::time_point retry_at_{};
};

// XInput pads (xinput1_4, or xinput9_1_0 where it is missing), the keyboard while this process has focus.
class HostSource final : public Source {
public:
    HostSource() {
        HMODULE x = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!x) x = LoadLibraryExW(L"xinput9_1_0.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (x) {
            get_state_ = reinterpret_cast<GetState>(reinterpret_cast<void*>(GetProcAddress(x, "XInputGetState")));
            set_state_ = reinterpret_cast<SetState>(reinterpret_cast<void*>(GetProcAddress(x, "XInputSetState")));
        }
        if (!get_state_) std::fprintf(stderr, "[rt] gameinput: no XInput on this host: no XInput pads\n");
    }

    bool pad(int slot, PadState* out) override {
        if (!get_state_) return false;
        const Clock::time_point now = Clock::now();
        // Asking XInput about an empty slot is slow; look again once a second.
        if (now < empty_until_[slot]) return false;
        XINPUT_STATE state{};
        if (get_state_(static_cast<DWORD>(slot), &state) != ERROR_SUCCESS) {
            empty_until_[slot] = now + std::chrono::seconds(1);
            return false;
        }
        empty_until_[slot] = {};
        *out = convert(state.Gamepad);
        return true;
    }

    bool native_pad(PadState* out) override { return dualsense_.poll(out); }

    std::vector<KeyState> keys() override {
        std::vector<KeyState> down;
        if (!focused()) return down;
        // Left/right modifier keys only, as devices report them (not VK_SHIFT/CONTROL/MENU); no mouse buttons.
        for (int vk = 0x08; vk <= 0xFE; ++vk) {
            if (vk >= 0x10 && vk <= 0x12) continue;
            if (GetAsyncKeyState(vk) >= 0) continue;
            KeyState k;
            k.scan_code = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC_EX);
            k.virtual_key = static_cast<uint8_t>(vk);
            down.push_back(k);
        }
        return down;
    }

    void rumble(int slot, uint16_t low, uint16_t high) override {
        if (!set_state_) return;
        XINPUT_VIBRATION v{low, high};
        set_state_(static_cast<DWORD>(slot), &v);  // an empty slot just fails
    }

    // Raw values, no dead zone: titles apply their own.
    static PadState convert(const XINPUT_GAMEPAD& g) {
        struct Map {
            WORD host;
            uint32_t bit;
        };
        static constexpr Map kButtons[] = {
            {XINPUT_GAMEPAD_START, GameInputGamepadMenu},
            {XINPUT_GAMEPAD_BACK, GameInputGamepadView},
            {XINPUT_GAMEPAD_A, GameInputGamepadA},
            {XINPUT_GAMEPAD_B, GameInputGamepadB},
            {XINPUT_GAMEPAD_X, GameInputGamepadX},
            {XINPUT_GAMEPAD_Y, GameInputGamepadY},
            {XINPUT_GAMEPAD_DPAD_UP, GameInputGamepadDPadUp},
            {XINPUT_GAMEPAD_DPAD_DOWN, GameInputGamepadDPadDown},
            {XINPUT_GAMEPAD_DPAD_LEFT, GameInputGamepadDPadLeft},
            {XINPUT_GAMEPAD_DPAD_RIGHT, GameInputGamepadDPadRight},
            {XINPUT_GAMEPAD_LEFT_SHOULDER, GameInputGamepadLeftShoulder},
            {XINPUT_GAMEPAD_RIGHT_SHOULDER, GameInputGamepadRightShoulder},
            {XINPUT_GAMEPAD_LEFT_THUMB, GameInputGamepadLeftThumbstick},
            {XINPUT_GAMEPAD_RIGHT_THUMB, GameInputGamepadRightThumbstick},
        };
        auto stick = [](SHORT v) { return std::max(-1.0f, static_cast<float>(v) / 32767.0f); };
        PadState s{};
        for (const Map& m : kButtons)
            if (g.wButtons & m.host) s.buttons |= m.bit;
        s.leftTrigger = static_cast<float>(g.bLeftTrigger) / 255.0f;
        s.rightTrigger = static_cast<float>(g.bRightTrigger) / 255.0f;
        s.leftThumbstickX = stick(g.sThumbLX);
        s.leftThumbstickY = stick(g.sThumbLY);
        s.rightThumbstickX = stick(g.sThumbRX);
        s.rightThumbstickY = stick(g.sThumbRY);
        return s;
    }

private:
    using GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    using SetState = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
    GetState get_state_ = nullptr;
    SetState set_state_ = nullptr;
    Clock::time_point empty_until_[kPads] = {};
    DualSense dualsense_;
};

// The process's hub, polled by a thread of its own from the first GameInputCreate on.
Hub& instance() {
    static Hub* hub = [] {
        const bool stand_in_on = !env_is("LEONIDA_KEYBOARD_PAD", "off") && !env_is("LEONIDA_KEYBOARD_PAD", "0");
        auto* h = new Hub(std::make_unique<HostSource>(), stand_in_on);
        std::fprintf(stderr, "[rt] gameinput: XInput, wired DualSense and keyboard%s (no SDL3 shim in this build)\n",
                     stand_in_on ? ", with the keyboard pad" : "");
        h->poll();
        std::thread([h] {
            for (;;) {
                std::this_thread::sleep_for(kPollPeriod);
                h->poll();
            }
        }).detach();
        return h;
    }();
    return *hub;
}

// LEONIDA_GAMEINPUT=host: the host's GameInput.dll, as the loader bound it before this shim existed.
GameInputCreateFn host_create() {
    static const GameInputCreateFn create = []() -> GameInputCreateFn {
        HMODULE m = LoadLibraryExW(L"GameInput.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto fn = m ? reinterpret_cast<GameInputCreateFn>(reinterpret_cast<void*>(GetProcAddress(m, "GameInputCreate")))
                    : nullptr;
        std::fprintf(stderr, "[rt] gameinput: LEONIDA_GAMEINPUT=host: %s\n",
                     fn ? "the host's GameInput.dll" : "no host GameInput.dll; using the shim");
        return fn;
    }();
    return create;
}

// HRESULT GameInputCreate(IGameInput** gameInput): the one hub, static and shared.
HRESULT WINAPI GameInputCreate(IGameInput** out) {
    if (!out) return E_INVALIDARG;
    if (env_is("LEONIDA_GAMEINPUT", "host"))
        if (const GameInputCreateFn create = host_create()) return create(out);
    *out = &instance();
    return S_OK;
}

#endif  // _WIN32

}  // namespace

PadState stand_in(const std::vector<KeyState>& keys) {
    struct Map {
        uint8_t vk;
        uint32_t bit;
    };
    static constexpr Map kButtons[] = {
        {0x1B, GameInputGamepadMenu},  // Esc
        {0x09, GameInputGamepadView},  // Tab
        {0x0D, GameInputGamepadA},     // Enter
        {0x08, GameInputGamepadB},     // Backspace
        {'X', GameInputGamepadX},
        {'Y', GameInputGamepadY},
        {0x26, GameInputGamepadDPadUp},  // arrows
        {0x28, GameInputGamepadDPadDown},
        {0x25, GameInputGamepadDPadLeft},
        {0x27, GameInputGamepadDPadRight},
        {'Q', GameInputGamepadLeftShoulder},
        {'E', GameInputGamepadRightShoulder},
        {'F', GameInputGamepadLeftThumbstick},
        {'H', GameInputGamepadRightThumbstick},
    };
    auto on = [&](uint8_t vk) {
        return std::any_of(keys.begin(), keys.end(), [vk](const KeyState& k) { return k.virtual_key == vk; }) ? 1.0f
                                                                                                               : 0.0f;
    };
    PadState s{};
    for (const Map& m : kButtons)
        if (on(m.vk) != 0.0f) s.buttons |= m.bit;
    s.leftTrigger = on('Z');
    s.rightTrigger = on('C');
    s.leftThumbstickX = on('D') - on('A');
    s.leftThumbstickY = on('W') - on('S');
    s.rightThumbstickX = on('L') - on('J');
    s.rightThumbstickY = on('I') - on('K');
    return s;
}

// The USB report begins with its id, four stick axes, two trigger axes, a sequence byte, and the hat/face and
// shoulder/system button fields.
bool decode_dualsense(const uint8_t* r, size_t size, PadState* out) {
    if (!r || size != 64 || r[0] != 0x01 || !out) return false;
    static constexpr uint32_t kHat[8] = {
        GameInputGamepadDPadUp,
        GameInputGamepadDPadUp | GameInputGamepadDPadRight,
        GameInputGamepadDPadRight,
        GameInputGamepadDPadDown | GameInputGamepadDPadRight,
        GameInputGamepadDPadDown,
        GameInputGamepadDPadDown | GameInputGamepadDPadLeft,
        GameInputGamepadDPadLeft,
        GameInputGamepadDPadUp | GameInputGamepadDPadLeft,
    };
    static constexpr std::pair<int, uint32_t> kBits[] = {
        {4, GameInputGamepadX},
        {5, GameInputGamepadA},
        {6, GameInputGamepadB},
        {7, GameInputGamepadY},
        {8, GameInputGamepadLeftShoulder},
        {9, GameInputGamepadRightShoulder},
        {12, GameInputGamepadView},
        {13, GameInputGamepadMenu},
        {14, GameInputGamepadLeftThumbstick},
        {15, GameInputGamepadRightThumbstick},
    };
    auto axis = [](uint8_t v) { return std::max(-1.0f, (static_cast<float>(v) - 128.0f) / 127.0f); };
    const uint8_t face = r[8], shoulders = r[9];
    PadState s{};
    if ((face & 0xF) < 8) s.buttons = kHat[face & 0xF];
    const uint32_t bits = face | static_cast<uint32_t>(shoulders) << 8;
    for (const auto& [host, title] : kBits)
        if (bits & (1u << host)) s.buttons |= title;
    s.leftTrigger = static_cast<float>(r[5]) / 255.0f;
    s.rightTrigger = static_cast<float>(r[6]) / 255.0f;
    s.leftThumbstickX = axis(r[1]);
    s.leftThumbstickY = -axis(r[2]);
    s.rightThumbstickX = axis(r[3]);
    s.rightThumbstickY = -axis(r[4]);
    *out = s;
    return true;
}

IGameInput* create_hub(std::unique_ptr<Source> source, bool stand_in_on) {
    auto* hub = new Hub(std::move(source), stand_in_on);
    hub->poll();
    return hub;
}

void poll(IGameInput* hub) { static_cast<Hub*>(hub)->poll(); }

IGameInputDevice* device(IGameInput* hub, int index) { return static_cast<Hub*>(hub)->device(index); }

}  // namespace rt::gameinput_xinput

#ifdef _WIN32
// Titles import GameInputCreate by ordinal only (VIctory); the name is registered too, as gameinput.cpp does.
SHIM_TABLE(gameinput_xinput,
    {"gameinput.dll", "GameInputCreate", reinterpret_cast<void*>(&rt::gameinput_xinput::GameInputCreate)},
    {"gameinput.dll", "#1", reinterpret_cast<void*>(&rt::gameinput_xinput::GameInputCreate)},
    {"xgameruntime.dll", "GameInputCreate", reinterpret_cast<void*>(&rt::gameinput_xinput::GameInputCreate)},
);
#endif
