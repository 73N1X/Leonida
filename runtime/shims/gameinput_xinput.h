// GameInput without SDL3: XInput pads, a wired DualSense, the keyboard, and a stand-in pad the keyboard drives.
// Ported from VIctory (crates/victory-hle/src/modules/gameinput.rs, gameinput/dualsense.rs), GPL-3.0-or-later; see
// gameinput_xinput.cpp. Built into gta6rt when the SDL3 shim (gameinput.cpp) is not. These are its test seams.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "gameinput_abi.h"

namespace rt::gameinput_xinput {

using PadState = gi::GameInputGamepadState;

// GameInputKeyState (v0): one key held down. The scan code is 0xE0xx for an extended key; no characters are
// reported (code point 0), as in VIctory: the title reads scan codes and virtual keys.
struct KeyState {
    uint32_t scan_code = 0;
    uint32_t code_point = 0;
    uint8_t virtual_key = 0;
    bool dead_key = false;
};
static_assert(sizeof(KeyState) == 12);

// Device indices: XInput slots 0-3, the keyboard's stand-in pad, the keyboard, a wired DualSense.
constexpr int kPads = 4;
constexpr int kStandIn = kPads;
constexpr int kKeyboard = kPads + 1;
constexpr int kDualSense = kPads + 2;
constexpr int kDevices = kPads + 3;

// Host input, or a fake in tests. Called on the poll thread with the source lock held.
class Source {
public:
    virtual ~Source() = default;
    // The pad in XInput slot `slot`; false when the slot is empty.
    virtual bool pad(int slot, PadState* out) = 0;
    // A pad of our own (a wired DualSense), asked only when no XInput slot holds one.
    virtual bool native_pad(PadState*) { return false; }
    // Keys held down; none while no window of this process has focus.
    virtual std::vector<KeyState> keys() = 0;
    // Motor speeds of an XInput pad: low frequency (left), high frequency (right).
    virtual void rumble(int slot, uint16_t low, uint16_t high) = 0;
};

// The stand-in pad for the keys held: Esc Menu, Tab View, Enter A, Backspace B, X, Y, arrows the d-pad, Q/E the
// shoulders, F/H the stick clicks; W/A/S/D the left stick, I/J/K/L the right one; Z and C the triggers.
PadState stand_in(const std::vector<KeyState>& keys);

// A wired DualSense's USB input report (64 bytes with report id 1); false for anything else.
bool decode_dualsense(const uint8_t* report, size_t size, PadState* out);

// A hub over `source` that no thread polls: a test calls poll(). `stand_in` false leaves the keyboard's pad out.
// Never freed, like the process's own hub.
gi::IGameInput* create_hub(std::unique_ptr<Source> source, bool stand_in = true);
// Reads the source once and runs device callbacks on the calling thread.
void poll(gi::IGameInput* hub);
// The hub's device `index` (above).
gi::IGameInputDevice* device(gi::IGameInput* hub, int index);

}  // namespace rt::gameinput_xinput
