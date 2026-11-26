// The GameInput shim without SDL3 (shims/gameinput_xinput.cpp) over a fake host: pads connecting and disconnecting
// with their callbacks, readings and the walk between them, the keyboard and its stand-in pad, the DualSense between
// XInput and the stand-in, asynchronous enumeration, unregistering a running callback, devices by id, rumble, and the
// DualSense report decoder. The cases follow VIctory's tests (crates/victory-hle/src/modules/gameinput.rs and
// gameinput/dualsense.rs), GPL-3.0-or-later, by Marvisak and contributors (DexNom fork); Leonida is
// GPL-3.0-or-later. No game data, no controller, no window.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>

#include "gameinput_xinput.h"
#include "shims.h"

namespace {

using namespace rt::gameinput_xinput;
using namespace gi;

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

struct FakeHost {
    std::mutex mu;
    bool pads[kPads] = {};
    PadState pad[kPads] = {};
    bool native = false;
    PadState native_pad{};
    std::vector<KeyState> keys;
    std::vector<std::tuple<int, uint16_t, uint16_t>> rumble;
};

class Fake final : public Source {
public:
    explicit Fake(std::shared_ptr<FakeHost> host) : host_(std::move(host)) {}
    bool pad(int slot, PadState* out) override {
        std::lock_guard lock(host_->mu);
        if (host_->pads[slot]) *out = host_->pad[slot];
        return host_->pads[slot];
    }
    bool native_pad(PadState* out) override {
        std::lock_guard lock(host_->mu);
        if (host_->native) *out = host_->native_pad;
        return host_->native;
    }
    std::vector<KeyState> keys() override {
        std::lock_guard lock(host_->mu);
        return host_->keys;
    }
    void rumble(int slot, uint16_t low, uint16_t high) override {
        std::lock_guard lock(host_->mu);
        host_->rumble.emplace_back(slot, low, high);
    }

private:
    std::shared_ptr<FakeHost> host_;
};

struct Hub {
    IGameInput* gi;
    std::shared_ptr<FakeHost> host;
};

Hub fake() {
    auto host = std::make_shared<FakeHost>();
    return {create_hub(std::make_unique<Fake>(host)), host};
}

KeyState key(uint8_t vk) {
    KeyState k;
    k.scan_code = vk + 0x100u;
    k.virtual_key = vk;
    return k;
}

// By value, as VIctory's PartialEq compares: -0.0 (a centred stick flipped to +Y up) equals 0.0.
bool same(const PadState& a, const PadState& b) {
    return a.buttons == b.buttons && a.leftTrigger == b.leftTrigger && a.rightTrigger == b.rightTrigger &&
           a.leftThumbstickX == b.leftThumbstickX && a.leftThumbstickY == b.leftThumbstickY &&
           a.rightThumbstickX == b.rightThumbstickX && a.rightThumbstickY == b.rightThumbstickY;
}

// Device callbacks seen: device, current and previous status.
struct Log {
    std::mutex mu;
    std::vector<std::tuple<IGameInputDevice*, uint32_t, uint32_t>> calls;
    size_t size() {
        std::lock_guard lock(mu);
        return calls.size();
    }
    std::tuple<IGameInputDevice*, uint32_t, uint32_t> at(size_t i) {
        std::lock_guard lock(mu);
        return calls.at(i);
    }
};

void CALLBACK record(GameInputCallbackToken, void* context, IGameInputDevice* device, uint64_t, uint32_t current,
                     uint32_t previous) {
    auto* log = static_cast<Log*>(context);
    std::lock_guard lock(log->mu);
    log->calls.emplace_back(device, current, previous);
}

constexpr uint32_t kConnected = 3;

GameInputCallbackToken register_devices(IGameInput* gi, uint32_t kind, GameInputEnumerationKind enumeration, Log* log) {
    GameInputCallbackToken token = 0;
    const HRESULT hr = gi->RegisterDeviceCallback(nullptr, kind, 1, enumeration, log, record, &token);
    check(hr == S_OK && token != 0, "RegisterDeviceCallback succeeds with a token");
    return token;
}

bool call_is(Log& log, size_t i, IGameInputDevice* device, uint32_t current, uint32_t previous) {
    if (log.size() <= i) return false;
    return log.at(i) == std::make_tuple(device, current, previous);
}

HRESULT current(IGameInput* gi, uint32_t kind, int index, IGameInputReading** out) {
    *out = reinterpret_cast<IGameInputReading*>(1);  // must be cleared on failure
    return gi->GetCurrentReading(kind, device(gi, index), out);
}

PadState pad_of(IGameInputReading* r) {
    PadState s{};
    check(r->GetGamepadState(&s), "GetGamepadState answers for a pad reading");
    return s;
}

void pads_connect_report_and_disconnect() {
    std::printf("-- pads connect, report and disconnect\n");
    Hub h = fake();
    IGameInput* gi = h.gi;
    Log log;
    register_devices(gi, GameInputKindGamepad, GameInputBlockingEnumeration, &log);
    // Blocking enumeration ran before registration returned: with no host pad the keyboard's stand-in is the only one.
    check(log.size() == 1 && call_is(log, 0, device(gi, kStandIn), kConnected, 0),
          "blocking enumeration reports the stand-in pad before returning");

    {
        std::lock_guard lock(h.host->mu);
        h.host->pads[1] = true;
        h.host->pad[1] = PadState{GameInputGamepadA, 0, 0, 0.5f, 0, 0, 0};
    }
    poll(gi);
    check(log.size() == 3 && call_is(log, 1, device(gi, 1), kConnected, 0) &&
              call_is(log, 2, device(gi, kStandIn), 0, kConnected),
          "a host pad connects and the stand-in leaves");

    IGameInputReading* first = nullptr;
    check(current(gi, GameInputKindGamepad, 1, &first) == S_OK, "the pad has a current reading");
    check(pad_of(first).buttons == GameInputGamepadA && pad_of(first).leftThumbstickX == 0.5f,
          "the reading carries the host pad's state");
    poll(gi);  // no change, no new reading
    IGameInputReading* again = nullptr;
    current(gi, GameInputKindGamepad, 1, &again);
    check(again == first, "an unchanged pad keeps its reading");
    again->Release();

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    {
        std::lock_guard lock(h.host->mu);
        h.host->pad[1].buttons = GameInputGamepadB;
    }
    poll(gi);
    IGameInputReading* second = nullptr;
    current(gi, GameInputKindGamepad, 1, &second);
    check(second != first && pad_of(second).buttons == GameInputGamepadB, "a change makes a new reading");
    check(second->GetTimestamp() > first->GetTimestamp() &&
              second->GetSequenceNumber(GameInputKindGamepad) == first->GetSequenceNumber(GameInputKindGamepad) + 1,
          "timestamps and sequence numbers move on");
    check(second->GetTimestamp() <= gi->GetCurrentTimestamp(), "a reading is not from the future");
    check(first->GetSequenceNumber(GameInputKindKeyboard) == 0, "no sequence number for a kind the reading lacks");

    // The title's edge check: previous of current, and back again.
    IGameInputDevice* pad1 = device(gi, 1);
    IGameInputReading* walked = nullptr;
    check(gi->GetPreviousReading(second, GameInputKindGamepad, pad1, &walked) == S_OK && walked == first,
          "GetPreviousReading of the newest is the one before");
    walked->Release();
    check(gi->GetNextReading(first, GameInputKindGamepad, pad1, &walked) == S_OK && walked == second,
          "GetNextReading of that is the newest again");
    walked->Release();
    walked = reinterpret_cast<IGameInputReading*>(1);
    check(gi->GetNextReading(second, GameInputKindGamepad, pad1, &walked) == GAMEINPUT_E_READING_NOT_FOUND &&
              !walked,
          "nothing after the newest");
    check(gi->GetPreviousReading(first, GameInputKindGamepad, pad1, &walked) == GAMEINPUT_E_READING_NOT_FOUND,
          "nothing before the first");
    check(gi->GetPreviousReading(first, GameInputKindGamepad, device(gi, 2), &walked) == E_INVALIDARG,
          "a device other than the reference's is refused");
    IGameInputReading* temporal = nullptr;
    check(gi->GetTemporalReading(first->GetTimestamp(), pad1, &temporal) == S_OK && temporal == first,
          "GetTemporalReading finds the reading in force at a time");
    temporal->Release();
    IGameInputDevice* of = nullptr;
    first->GetDevice(&of);
    check(of == pad1 && first->GetInputKind() == GameInputKindGamepad, "a reading names its device and kind");
    first->Release();
    second->Release();

    {
        std::lock_guard lock(h.host->mu);
        h.host->pads[1] = false;
    }
    poll(gi);
    check(log.size() == 5 && call_is(log, 3, device(gi, 1), 0, kConnected) &&
              call_is(log, 4, device(gi, kStandIn), kConnected, 0),
          "the pad leaves and the stand-in comes back");
    IGameInputReading* gone = nullptr;
    check(current(gi, GameInputKindGamepad, 1, &gone) == GAMEINPUT_E_DEVICE_DISCONNECTED && !gone,
          "a disconnected pad has no current reading");
    check(device(gi, 1)->GetDeviceStatus() == 0 && device(gi, kStandIn)->GetDeviceStatus() == kConnected,
          "GetDeviceStatus follows the connection");
    IGameInputReading* rest = nullptr;
    check(current(gi, GameInputKindGamepad, kStandIn, &rest) == S_OK && same(pad_of(rest), PadState{}),
          "the stand-in is back, at rest");
    rest->Release();
    IGameInputReading* any = nullptr;
    check(gi->GetCurrentReading(GameInputKindGamepad, nullptr, &any) == S_OK && any, "any pad: the stand-in's reading");
    any->Release();
}

void native_pad_sits_between_xinput_and_keyboard() {
    std::printf("-- the DualSense sits between XInput and the stand-in\n");
    Hub h = fake();
    IGameInput* gi = h.gi;
    {
        std::lock_guard lock(h.host->mu);
        h.host->native = true;
        h.host->native_pad.buttons = GameInputGamepadA;
    }
    poll(gi);
    IGameInputReading* r = nullptr;
    check(current(gi, GameInputKindGamepad, kStandIn, &r) == GAMEINPUT_E_DEVICE_DISCONNECTED,
          "a DualSense sends the stand-in away");
    check(current(gi, GameInputKindGamepad, kDualSense, &r) == S_OK && pad_of(r).buttons == GameInputGamepadA,
          "the DualSense's reading");
    r->Release();
    {
        std::lock_guard lock(h.host->mu);
        h.host->pads[0] = true;
    }
    poll(gi);
    check(current(gi, GameInputKindGamepad, kDualSense, &r) == GAMEINPUT_E_DEVICE_DISCONNECTED,
          "an XInput pad wins over the DualSense");
    check(current(gi, GameInputKindGamepad, 0, &r) == S_OK, "the XInput pad reads");
    r->Release();
    {
        std::lock_guard lock(h.host->mu);
        h.host->pads[0] = false;
        h.host->native = false;
    }
    poll(gi);
    check(current(gi, GameInputKindGamepad, kStandIn, &r) == S_OK, "with neither, the stand-in is back");
    r->Release();
}

void keys_feed_the_keyboard_and_the_stand_in() {
    std::printf("-- keys feed the keyboard and the stand-in pad\n");
    Hub h = fake();
    IGameInput* gi = h.gi;
    Log log;
    register_devices(gi, GameInputKindKeyboard, GameInputBlockingEnumeration, &log);
    check(log.size() == 1 && call_is(log, 0, device(gi, kKeyboard), kConnected, 0), "the keyboard is enumerated");
    const std::vector<KeyState> keys = {key('W'), key(0x0D), key('L'), key('C')};
    {
        std::lock_guard lock(h.host->mu);
        h.host->keys = keys;
    }
    poll(gi);
    IGameInputReading* r = nullptr;
    check(current(gi, GameInputKindKeyboard, kKeyboard, &r) == S_OK, "the keyboard has a reading");
    check(r->GetKeyCount() == 4, "GetKeyCount counts the keys held");
    KeyState got[8] = {};
    check(r->GetKeyState(8, got) == 4 && std::memcmp(got, keys.data(), 4 * sizeof(KeyState)) == 0,
          "GetKeyState copies them out");
    check(r->GetKeyState(2, got) == 2, "GetKeyState stops at the room given");
    PadState none{};
    check(!r->GetGamepadState(&none), "a keyboard reading has no pad state");
    r->Release();

    check(current(gi, GameInputKindGamepad, kStandIn, &r) == S_OK, "the stand-in has a reading");
    const PadState expected{GameInputGamepadA, 0, 1.0f, 0, 1.0f, 1.0f, 0};
    check(same(pad_of(r), expected), "W, Enter, L and C: up, A, right stick right, right trigger");
    r->Release();
    check(current(gi, GameInputKindGamepad, kKeyboard, &r) == GAMEINPUT_E_READING_NOT_FOUND,
          "a keyboard reading is no gamepad reading");
    check(device(gi, kKeyboard)->GetDeviceInfo()->supportedInput == GameInputKindKeyboard &&
              device(gi, kStandIn)->GetDeviceInfo()->supportedInput == GameInputKindGamepad,
          "the devices say what they are");
}

void async_enumeration_and_unregistering() {
    std::printf("-- asynchronous enumeration waits for the poll; unregistering stops callbacks\n");
    Hub h = fake();
    IGameInput* gi = h.gi;
    Log log;
    const GameInputCallbackToken token = register_devices(gi, GameInputKindGamepad, GameInputAsyncEnumeration, &log);
    check(log.size() == 0, "nothing before the next poll");
    poll(gi);
    check(log.size() == 1 && call_is(log, 0, device(gi, kStandIn), kConnected, 0), "the poll delivers it");
    check(gi->UnregisterCallback(token, 5000), "UnregisterCallback succeeds");
    {
        std::lock_guard lock(h.host->mu);
        h.host->pads[0] = true;
    }
    poll(gi);
    check(log.size() == 1, "no callback after unregistering");
}

std::atomic<bool> g_inside{false}, g_leave{false};
std::atomic<int> g_from_inside{0};

void CALLBACK slow(GameInputCallbackToken token, void* context, IGameInputDevice*, uint64_t, uint32_t, uint32_t) {
    auto* gi = static_cast<IGameInput*>(context);
    if (!gi->UnregisterCallback(token, 0)) g_from_inside.fetch_add(1);  // it cannot wait for itself
    g_inside = true;
    while (!g_leave) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void unregistering_waits_for_a_running_callback() {
    std::printf("-- unregistering waits for a running callback\n");
    Hub h = fake();
    IGameInput* gi = h.gi;
    GameInputCallbackToken token = 0;
    check(gi->RegisterDeviceCallback(nullptr, GameInputKindGamepad, 1, GameInputNoEnumeration, gi, slow, &token) ==
              S_OK,
          "register a slow callback");
    {
        std::lock_guard lock(h.host->mu);
        h.host->pads[1] = true;
    }
    std::thread poller([gi] { poll(gi); });
    while (!g_inside) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(!gi->UnregisterCallback(token, 5000), "times out while it runs");
    std::thread leave([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        g_leave = true;
    });
    const auto start = std::chrono::steady_clock::now();
    check(gi->UnregisterCallback(token, UINT64_MAX), "waits for it");
    check(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(40), "it really waited");
    check(g_from_inside == 1, "from inside its own callback it fails at once");
    poller.join();
    leave.join();
}

void devices_by_id_and_rumble() {
    std::printf("-- devices are found by id; rumble reaches the host\n");
    Hub h = fake();
    IGameInput* gi = h.gi;
    {
        std::lock_guard lock(h.host->mu);
        h.host->pads[2] = true;
    }
    poll(gi);
    IGameInputDevice* pad = device(gi, 2);
    const GameInputDeviceInfo* info = pad->GetDeviceInfo();
    AppLocalDeviceId zero{};
    check(std::memcmp(&info->deviceId, &zero, sizeof zero) != 0, "a device has an id");
    check(reinterpret_cast<const uint8_t*>(&info->deviceId) - reinterpret_cast<const uint8_t*>(info) == 0x20,
          "the id the title reads at +0x20");
    check(info->infoSize == sizeof(GameInputDeviceInfo) &&
              info->supportedRumbleMotors == (GameInputRumbleLowFrequency | GameInputRumbleHighFrequency),
          "XInput pads have two motors");
    IGameInputDevice* found = nullptr;
    check(gi->FindDeviceFromId(&info->deviceId, &found) == S_OK && found == pad, "FindDeviceFromId finds it");
    AppLocalDeviceId other{};
    std::memset(other.value, 1, sizeof other.value);
    check(gi->FindDeviceFromId(&other, &found) == GAMEINPUT_E_DEVICE_NOT_FOUND && !found, "an unknown id is not found");

    const GameInputRumbleParams half{0.5f, 1.0f, 1.0f, 1.0f}, full{1, 1, 1, 1};
    pad->SetRumbleState(&half);
    device(gi, kStandIn)->SetRumbleState(&full);  // the stand-in has no motors
    pad->SetRumbleState(nullptr);
    {
        std::lock_guard lock(h.host->mu);
        const std::vector<std::tuple<int, uint16_t, uint16_t>> want = {
            std::make_tuple(2, uint16_t{32767}, uint16_t{65535}), std::make_tuple(2, uint16_t{0}, uint16_t{0})};
        check(h.host->rumble == want, "the two body motors reach slot 2; null stops them; trigger motors are dropped");
    }
    void* effect = reinterpret_cast<void*>(1);
    check(pad->CreateForceFeedbackEffect(0, nullptr, &effect) == E_NOTIMPL && !effect,
          "force feedback is refused with a null effect");
    GameInputBatteryState battery{1, 1, 1, 1, GameInputBatteryCharging};
    pad->GetBatteryState(&battery);
    check(battery.status == GameInputBatteryNotPresent, "no battery");
    void* qi = nullptr;
    check(gi->QueryInterface(IID_IGameInput, &qi) == S_OK && qi == gi, "IGameInput answers its own IID");
    check(pad->QueryInterface(IID_IGameInputReading, &qi) == E_NOINTERFACE && !qi, "a device is no reading");
    IGameInputDispatcher* dispatcher = reinterpret_cast<IGameInputDispatcher*>(1);
    check(gi->CreateDispatcher(&dispatcher) == E_NOTIMPL && !dispatcher, "no dispatcher, with a null out pointer");
}

void stand_in_can_be_turned_off() {
    std::printf("-- the stand-in pad can be left out\n");
    auto host = std::make_shared<FakeHost>();
    IGameInput* gi = create_hub(std::make_unique<Fake>(host), false);
    IGameInputReading* r = nullptr;
    check(current(gi, GameInputKindGamepad, kStandIn, &r) == GAMEINPUT_E_DEVICE_DISCONNECTED,
          "no stand-in when it is off");
    check(current(gi, GameInputKindKeyboard, kKeyboard, &r) == S_OK, "the keyboard is still there");
    r->Release();
}

void dualsense_reports_decode() {
    std::printf("-- DualSense USB reports\n");
    uint8_t report[64] = {1, 255, 0, 128, 255, 0, 255, 0, 0x21, 0x51};
    PadState s{};
    check(decode_dualsense(report, sizeof report, &s), "a USB input report decodes");
    const PadState want{GameInputGamepadA | GameInputGamepadDPadUp | GameInputGamepadDPadRight |
                            GameInputGamepadLeftShoulder | GameInputGamepadView | GameInputGamepadLeftThumbstick,
                        0, 1.0f, 1.0f, 1.0f, 0, -1.0f};
    check(same(s, want), "buttons, hat and axes as VIctory maps them");
    const uint8_t rest[10] = {1, 128, 128, 128, 128, 0, 0, 0, 8, 0};
    std::memcpy(report, rest, sizeof rest);
    check(decode_dualsense(report, sizeof report, &s) && same(s, PadState{}), "hat 8 and centred sticks: at rest");
    check(!decode_dualsense(report, 10, &s), "a short report is refused");
    report[0] = 0x31;
    check(!decode_dualsense(report, sizeof report, &s), "a Bluetooth report id is refused");
}

void stand_in_mapping() {
    std::printf("-- the stand-in pad's keys\n");
    const PadState s = stand_in({key(0x1B), key(0x09), key('A'), key('S'), key('Q'), key('H'), key('Z'), key('J')});
    check(s.buttons == (GameInputGamepadMenu | GameInputGamepadView | GameInputGamepadLeftShoulder |
                        GameInputGamepadRightThumbstick),
          "Esc Menu, Tab View, Q left shoulder, H right stick click");
    check(s.leftThumbstickX == -1.0f && s.leftThumbstickY == -1.0f && s.rightThumbstickX == -1.0f &&
              s.leftTrigger == 1.0f && s.rightTrigger == 0.0f,
          "A and S push the left stick left and down, J the right stick left, Z the left trigger");
    const PadState both = stand_in({key('A'), key('D')});
    check(both.leftThumbstickX == 0.0f, "opposite keys cancel");
}

void registered_by_ordinal() {
    std::printf("-- registry\n");
    void* by_name = rt::shims::find("gameinput.dll", "GameInputCreate");
    check(by_name && rt::shims::find("gameinput.dll", "#1") == by_name, "GameInputCreate by name and by ordinal 1");
    check(rt::shims::find("xgameruntime.dll", "GameInputCreate") == by_name, "and through xgameruntime.dll");
    using Create = HRESULT(WINAPI*)(IGameInput**);
    check(reinterpret_cast<Create>(by_name)(nullptr) == E_INVALIDARG, "a null out pointer is refused");
}

}  // namespace

int main() {
    std::printf("gameinput_xinput_test\n");
    pads_connect_report_and_disconnect();
    native_pad_sits_between_xinput_and_keyboard();
    keys_feed_the_keyboard_and_the_stand_in();
    async_enumeration_and_unregistering();
    unregistering_waits_for_a_running_callback();
    devices_by_id_and_rumble();
    stand_in_can_be_turned_off();
    dualsense_reports_decode();
    stand_in_mapping();
    registered_by_ordinal();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
