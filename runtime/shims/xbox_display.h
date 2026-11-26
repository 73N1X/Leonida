#pragma once

// The console's DXGI adapter and output, as a title on the Vulkan device reaches them: device QueryInterface for
// IDXGIDevice1, GetAdapter, EnumOutputs(0), then the output's console method at slot 21, which lists the display
// modes: LEONIDA_DISPLAY_MODE=WxH, 3840x2160 by default (src/display_mode.h). The 1.0.2.1 title calls it just before its
// first SetFrameIntervalX, but its window and scanout buffers keep the size compiled into it whatever this reports
// (measured with 1920x1080, at 60 and 120 Hz); the size is changed in the executable (src/display_profile.h, applied by
// main.cpp), and this answer is kept in step with it.
//
// Ported from VIctory (GPL-3.0-or-later, Marvisak and contributors, DexNom fork; victory/crates/victory-gpu/src/
// dxgi.rs): its tables are what the title was seen to call. The adapter keeps the PC IDXGIAdapter2 order (IUnknown 3,
// object 4, adapter 3 + 1 + 1 = 12 slots; the title calls 6 GetParent, 7 EnumOutputs and 8 GetDesc). The output has
// the 12 PC IDXGIOutput methods at 7-18 and then three console ones (V dxgi.rs:281-288: "12 as on PC, then 3 console
// ones"); the title calls 6 GetParent and 21, `modes(format, flags, &count, out)` (V dxgi.rs:243-279), whose entries are
// 32 bytes: width, height, refresh rate (numerator, denominator), format, scanline order, scaling, flags. VIctory
// writes one 60 Hz mode with format 0 and progressive scanlines; so does this. On a PC IDXGIOutput1 slot 21 is
// GetDisplaySurfaceData1, which is what a title reached before: the call failed and the title kept its 4K default.
//
// What the wrappers do with the rest: the PC methods at the same slots forward to the layer's own adapter and output
// (dxgi_com.cpp), except that the output's GetDesc reports desktop coordinates of the display mode's size and its
// GetDisplayModeList lists the display mode, so no answer contradicts slot 21. The console output's slots 19 and 20 and
// every slot past the known ones are refused with E_NOTIMPL and logged once with their first argument words; each
// slot's first call is logged.

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>

#include "../src/display_mode.h"

#include <cstdint>

namespace rt::d3d12 {

// The display mode reported to the title: LEONIDA_DISPLAY_MODE when it parses (read and logged once), else 3840x2160.
leonida::DisplayMode title_display_mode();

// `pc` (an IDXGIAdapter of any implementation; one reference is taken) in the console adapter layout described above.
HRESULT create_console_adapter(IDXGIAdapter* pc, REFIID riid, void** out);

// The device proxy's adapter provider (d3d12_xs::set_device_adapter_provider) for a title on the Vulkan device: this
// layer's adapter (create_dxgi_factory, EnumAdapters(0)) in the console layout.
HRESULT console_adapter_provider(ID3D12Device* real, REFIID riid, void** out);

// True for an adapter or output made here.
bool is_console_dxgi_object(const void* object);

// The console output's slot 21, in the layout V dxgi.rs:208-220 gives (32 bytes).
struct ConsoleDisplayMode {
    std::uint32_t width, height;
    std::uint32_t refresh_numerator, refresh_denominator;
    std::uint32_t format, scanline_order, scaling, flags;
};
static_assert(sizeof(ConsoleDisplayMode) == 32, "V dxgi.rs:220");
inline constexpr unsigned kConsoleOutputModesSlot = 21;

}  // namespace rt::d3d12
