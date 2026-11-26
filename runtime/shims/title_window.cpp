// The title's own window, ported from VIctory (GPL-3.0-or-later, Marvisak and contributors, DexNom fork;
// victory/crates/victory-hle/src/modules/lifecycle.rs `CreateWindowExA`, `CreateWindowExW`, `fit`, `fit_into`, `hook`,
// `window_proc`).
//
// A GDKX title makes a Win32 window (the title imports CreateWindowExA, RegisterClassExA, ShowWindow and a
// PeekMessageA loop from the ext-ms-win-rtcore-ntuser-* sets) and sizes it to its render target: 3840x2160 on a
// Series X, larger than most screens. CreateWindowExA/W here make the window the title asks for, with two changes for a
// top-level one:
//   - with LEONIDA_ASPECT (src/aspect_fit.h, docs/ultrawide.md) it is first widened to that aspect, keeping its height,
//     so an ultrawide monitor gets an ultrawide window (a 16:9 frame is then pillarboxed in it, src/vulkan_present.h);
//   - a size that does not fit the primary monitor's work area is shrunk to it, keeping its shape, and centred (what the
//     window shows is scaled to it: the swapchain follows the client area);
//   - a window procedure goes in front of the title's: Close (the close button, Alt+F4, the window menu) ends the run,
//     since a console title is never told it is closing: as in VIctory, the title's suspend routines run first
//     (app_state.cpp) and the process ends when they return or after a second, and Alt or F10 do not put the window into menu mode: the system would run its
//     modal menu loop on the title's thread, which then stops until a key ends the loop.
// The window is then the one PresentX shows the frames in (xbox_window.h, note_title_window).
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstdio>

#include "shims.h"
#include "xbox_display.h"
#include "xbox_window.h"

namespace {

constexpr wchar_t kTitleProc[] = L"leonida.wndproc";  // the title's own procedure, kept on the window

bool top_level(HWND parent, DWORD style) { return !parent && !(style & WS_CHILD); }

// V lifecycle.rs `fit`: only a top-level window that does not fit, and not CW_USEDEFAULT sizes. `caller` is where the
// title called from, logged so a trace can find what sized the window.
void fit(HWND parent, DWORD style, int& x, int& y, int& width, int& height, const void* caller) {
    if (!top_level(parent, style) || width <= 0 || height <= 0 || width == CW_USEDEFAULT || height == CW_USEDEFAULT)
        return;
    if (const double ratio = rt::d3d12::title_aspect_ratio(); ratio > 0.0) {
        const leonida::aspect::Size wide = leonida::aspect::widen({std::uint32_t(width), std::uint32_t(height)}, ratio);
        if (int(wide.width) != width) {
            std::fprintf(stderr, "[rt] window: the title's %dx%d window is widened to %ux%d (LEONIDA_ASPECT)\n", width,
                         height, wide.width, height);
            width = int(wide.width);
        }
    }
    RECT area{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0)) return;
    const int aw = area.right - area.left, ah = area.bottom - area.top;
    if (aw <= 0 || ah <= 0 || (width <= aw && height <= ah)) return;
    const double scale = std::min(double(aw) / double(width), double(ah) / double(height));
    const int w = int(double(width) * scale), h = int(double(height) * scale);
    std::fprintf(stderr, "[rt] window: the title's %dx%d window (CreateWindowEx called from %p) is fitted to the screen "
                         "as %dx%d\n", width, height, caller, w, h);
    x = area.left + (aw - w) / 2;
    y = area.top + (ah - h) / 2;
    width = w;
    height = h;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_SYSCOMMAND && (w & 0xFFF0) == SC_CLOSE) {
        // As the console ends a title: its suspend routines first, then the process (app_state.cpp).
        rt::shims::app_state::end_title("the title's window was closed");
        return 0;
    }
    if (message == WM_SYSCOMMAND && (w & 0xFFF0) == SC_KEYMENU) return 0;
    const auto title = reinterpret_cast<WNDPROC>(GetPropW(hwnd, kTitleProc));
    if (message == WM_NCDESTROY) RemovePropW(hwnd, kTitleProc);
    return title ? CallWindowProcW(title, hwnd, message, w, l) : DefWindowProcW(hwnd, message, w, l);
}

// V lifecycle.rs `hook`: the title's procedure is stored before ours can run.
void hook(HWND hwnd, HWND parent, DWORD style) {
    if (!hwnd || !top_level(parent, style)) return;
    const LONG_PTR title = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    SetPropW(hwnd, kTitleProc, reinterpret_cast<HANDLE>(title));
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_proc));
    rt::d3d12::note_title_window(hwnd);
}

HWND WINAPI create_window_ex_a(DWORD ex_style, LPCSTR class_name, LPCSTR window_name, DWORD style, int x, int y,
                               int width, int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
    fit(parent, style, x, y, width, height, _ReturnAddress());
    const HWND hwnd = CreateWindowExA(ex_style, class_name, window_name, style, x, y, width, height, parent, menu,
                                      instance, param);
    hook(hwnd, parent, style);
    return hwnd;
}

HWND WINAPI create_window_ex_w(DWORD ex_style, LPCWSTR class_name, LPCWSTR window_name, DWORD style, int x, int y,
                               int width, int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
    fit(parent, style, x, y, width, height, _ReturnAddress());
    const HWND hwnd = CreateWindowExW(ex_style, class_name, window_name, style, x, y, width, height, parent, menu,
                                      instance, param);
    hook(hwnd, parent, style);
    return hwnd;
}

}  // namespace

// The sets the title imports them from (V lifecycle.rs: "ext-ms-win-rtcore-ntuser-window-*").
SHIM_TABLE(title_window,
    {"ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll", "CreateWindowExA", reinterpret_cast<void*>(&create_window_ex_a)},
    {"ext-ms-win-rtcore-ntuser-window-l1-1-0.dll", "CreateWindowExW", reinterpret_cast<void*>(&create_window_ex_w)},
);
