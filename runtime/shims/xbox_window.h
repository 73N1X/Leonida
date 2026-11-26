#pragma once

// The title's window, and what puts the frames the title presents there (docs/d3d12_vulkan.md section 4.5).
//
// A GDKX title makes no window and presents through the console queue's PresentX (queue slot 32 in the console
// layout). attach_title_presentation sets the device proxy's present handler (d3d12_xs::set_present_handler): at the
// first PresentX a window is made, a Vulkan swapchain on it on the queue the title presents on (create_window_presenter),
// and an XboxPresentation over that, which composes the title's planes into the swapchain. The title's frame calls stay
// with the device's frame clock (device slots 78-81, VIctory's frame.rs); the presentation takes its tokens from there
// and does not pace (XboxPresentation::use_device_frame_clock).
//
// Ported from VIctory (GPL-3.0-or-later, Marvisak and contributors, DexNom fork; victory/crates/victory-gpu/src/
// host.rs `presenter`, `title_window`, `window`, `make_window`, `pump`, `Meter` and the "victory-caption" thread;
// queue.rs:757-841 `present`):
//   - The frames go to the title's own window when it has one (V `title_window`): the title makes a top-level window
//     itself (CreateWindowExA, which title_window.cpp fits to the screen) and pumps its messages. Otherwise a window is
//     made at the first present, with a client area of the first plane's size, on a thread of its own that pumps its
//     messages ("leonida-window"; V "victory-window"). That size is shrunk, keeping its shape, to fit the primary
//     monitor's work area (a 3840x2160 back buffer on a 2560x1440 monitor would not fit), the window is centred and
//     per-monitor DPI aware (only its own thread is), and closing it ends the run.
//   - Its caption is "Leonida | <fps> fps <ms> ms/frame" (src/frame_meter.h), set once a second by a thread of its own
//     ("leonida-caption"), so presenting never waits for the window's thread.
//   - A PresentX that cannot be shown (no window, no swapchain, a refused plane) is answered S_OK, as VIctory's present
//     always is, and logged; the d3d12 trace still says `XboxPresentation::PresentX failed`.
//
// Screenshots for automation (V capture.rs:170-185 VICTORY_CAPTURE_REQUEST, `save_as`): with LEONIDA_CAPTURE_DIR=<dir>,
// a thread ("leonida-capture") reads the request file every 100 ms: LEONIDA_CAPTURE_REQUEST, else <dir>\request.txt,
// holding a number. When the number is above the last one it took (0 at the start), the next frame presented after it is
// copied (VulkanPresenter::request_snapshot: in the frame's own command buffer, nothing read on the present path) and
// that thread writes it as <dir>\capture-<number>.png: 8-bit RGB at the swapchain's size (src/png_write.h), written to a
// .part file and renamed, so a file that exists is whole. VIctory halves its frames and reads the request file on the
// present path; this keeps the size and reads it off that path.
#include <windows.h>

#include <cstdint>
#include <memory>

namespace rt::d3d12 {

// The window: see the top comment. Destroying it joins its threads, and closes the window if it made it.
class GameWindow {
public:
    // A window whose client area is `width` x `height`, shrunk to fit the primary monitor's work area. Null on failure
    // (logged).
    static std::unique_ptr<GameWindow> create(std::uint32_t width, std::uint32_t height);
    // The title's own window: only the caption is ours. Null for something that is not a window.
    static std::unique_ptr<GameWindow> adopt(HWND window);
    ~GameWindow();
    GameWindow(const GameWindow&) = delete;
    GameWindow& operator=(const GameWindow&) = delete;

    HWND hwnd() const;
    // A frame was shown: counted for the caption. Lock-free; any thread.
    void frame_presented();

    struct State;

private:
    explicit GameWindow(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

// Sets the present handler of `device` (an Xbox-ABI device proxy over the Vulkan device, or its DXGI pointer) to the
// title presentation above. False, with nothing set, for anything else. The window opens at the first PresentX.
bool attach_title_presentation(void* device);

// The title made `window`, a top-level window (title_window.cpp): the frames go there.
void note_title_window(HWND window);
// The window the frames go to if the title presents now: the one noted above while it exists, else a visible top-level
// window of this process that is not the runtime's (V host.rs `title_window`), else null.
HWND title_window();

}  // namespace rt::d3d12
