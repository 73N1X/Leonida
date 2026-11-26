#pragma once

// The size the title outputs and renders at: LEONIDA_DISPLAY_MODE=WxH (and LEONIDA_RENDER_SCALE), applied to the mapped
// executable before any of its code runs.
//
// Ported from VIctory (GPL-3.0-or-later, Marvisak and contributors, DexNom fork; upstream commit ab34a8b21,
// apps/victory/src/native_1440p.rs `prepare`). The title does not size its frames by the display mode DXGI reports
// (shims/xbox_display.h answers that too): the Series X SDR mode is a constant in the executable, 3840x2160 at 60 Hz,
// and the title builds its window, its two scanout buffers and its frontend from it. Its scene is rendered at 2/3 of
// that and scaled up temporally (2560x1440 into 3840x2160). VIctory's native 1440p profile changes four places of the
// retail executable, each checked against its stock bytes first:
//   - the SDR mode (W, H, 60, 1);
//   - the size reserved for the two scanout buffers (`mov ecx, imm32`): two 64 KiB-aligned RGBA8 surfaces in the
//     64 KiB D_X swizzle, rounded up to 2 MiB, "matching the title's allocation check" (V). The stock value,
//     0x04000000, is what this gives for 3840x2160, which scanout_pool_bytes checks;
//   - the temporal scene scale (a float, stock 2/3): the render size over the output size;
//   - the Settings screen's viewport load, which reads a separate 3840x2160 pair: pointed at the SDR mode instead.
// VIctory checks the build by the BLAKE3 hash of the file as well; here every patch is checked against the bytes it
// replaces, all before any is written, so another build is left alone (and says so).
//
// VIctory applies it for 2560x1440 output, rendered at 2560x1440 (scale 1) or 1920x1080 (VICTORY_RENDER_1080P, scale
// 0.75); so does this, with LEONIDA_RENDER_SCALE (0.25..1, default 1) for the scale. Other outputs are refused: with
// the SDR mode set to 1920x1080 the 1.0.2.1 title still makes a 2560x1440 window and 2560x1440 scanout buffers
// (measured under gta6rt --run), which a reservation sized for 1920x1080 would not hold. 3840x2160 (the default)
// changes nothing.

#include "display_mode.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX  // std::min and std::max, not the macros
#endif
#include <windows.h>
#endif

namespace leonida::display_profile {

// RVAs in the 1.0.2.1 retail executable (V native_1440p.rs).
inline constexpr std::uint32_t kSdrModeRva = 0x0615c100;
inline constexpr std::uint32_t kScanoutReservationRva = 0x0247ad4a;
inline constexpr std::uint32_t kSceneScaleRva = 0x028f1e7e;
inline constexpr std::uint32_t kSettingsViewportLoadRva = 0x058048d4;

// Two scanout surfaces of `width` x `height` RGBA8 in the 64 KiB D_X swizzle (128x128 texels a 64 KiB block), each
// 64 KiB aligned, together rounded up to 2 MiB.
inline std::uint32_t scanout_pool_bytes(std::uint32_t width, std::uint32_t height) {
    const std::uint64_t blocks = std::uint64_t((width + 127) / 128) * ((height + 127) / 128);
    const std::uint64_t surface = blocks * 65536;  // already a multiple of 64 KiB
    const std::uint64_t two_mib = std::uint64_t(2) << 20;
    return std::uint32_t((surface * 2 + two_mib - 1) / two_mib * two_mib);
}

struct Patch {
    std::uint32_t rva = 0;
    std::vector<std::uint8_t> stock;  // what must be there
    std::vector<std::uint8_t> bytes;  // what goes there
    const char* what = "";
};

struct Plan {
    DisplayMode output;
    std::uint32_t render_width = 0, render_height = 0;
    float scale = 2.0f / 3.0f;
    std::uint32_t pool = 0;
    std::vector<Patch> patches;  // empty: the stock 3840x2160 profile, or a refused one (`refused` says why)
    const char* refused = nullptr;
};

namespace detail {
inline void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(v >> (8 * i)));
}
inline void put_float(std::vector<std::uint8_t>& out, float f) {
    std::uint32_t v = 0;
    std::memcpy(&v, &f, sizeof v);
    put32(out, v);
}
}  // namespace detail

// The patches for an output of `mode` rendered at `render_scale` of it (0: 1).
inline Plan plan(const DisplayMode& mode, float render_scale = 0.0f) {
    Plan p;
    p.output = mode;
    const DisplayMode stock;
    if (mode.width == stock.width && mode.height == stock.height) {
        p.render_width = 2560;
        p.render_height = 1440;
        p.pool = scanout_pool_bytes(stock.width, stock.height);
        if (render_scale > 0.0f) p.refused = "a render scale is only applied with a 2560x1440 output";
        return p;
    }
    if (mode.width != 2560 || mode.height != 1440) {
        p.refused = "only 2560x1440 is an output the title takes (VIctory's profile)";
        return p;
    }
    p.scale = render_scale > 0.0f ? render_scale : 1.0f;
    p.render_width = std::uint32_t(float(mode.width) * p.scale);
    p.render_height = std::uint32_t(float(mode.height) * p.scale);
    p.pool = scanout_pool_bytes(mode.width, mode.height);

    Patch sdr{kSdrModeRva, {}, {}, "SDR mode"};
    for (const std::uint32_t v : {stock.width, stock.height, 60u, 1u}) detail::put32(sdr.stock, v);
    for (const std::uint32_t v : {mode.width, mode.height, 60u, 1u}) detail::put32(sdr.bytes, v);
    Patch pool{kScanoutReservationRva, {0xb9}, {0xb9}, "scanout reservation"};
    detail::put32(pool.stock, scanout_pool_bytes(stock.width, stock.height));
    detail::put32(pool.bytes, p.pool);
    Patch scale{kSceneScaleRva, {}, {}, "temporal scene scale"};
    detail::put_float(scale.stock, 2.0f / 3.0f);
    detail::put_float(scale.bytes, p.scale);
    // vmovsd xmm1, qword [rip + disp32]: the Settings viewport's 3840x2160 pair, then the SDR mode's.
    Patch viewport{kSettingsViewportLoadRva, {0xc5, 0xfb, 0x10, 0x0d, 0x10, 0xf0, 0x13, 0x05}, {0xc5, 0xfb, 0x10, 0x0d},
                   "Settings viewport load"};
    detail::put32(viewport.bytes, std::uint32_t(std::int32_t(kSdrModeRva) - std::int32_t(kSettingsViewportLoadRva + 8)));
    p.patches = {sdr, pool, scale, viewport};
    return p;
}

// True when every patch's stock bytes are in the image, read through `at(rva, length)` (a pointer to that many bytes
// of the image, or null past it); else false with `*error` set.
template <class Access>
bool check(Access&& at, const Plan& p, std::string* error) {
    for (const Patch& patch : p.patches) {
        const std::uint8_t* there = at(patch.rva, patch.stock.size());
        if (!there || std::memcmp(there, patch.stock.data(), patch.stock.size()) != 0) {
            if (error) *error = std::string("the ") + patch.what + " is not the 1.0.2.1 executable's";
            return false;
        }
    }
    return true;
}

// check(), then every patch written through `at`. Nothing is written when the check fails.
template <class Access>
bool apply(Access&& at, const Plan& p, std::string* error) {
    if (!check(at, p, error)) return false;
    for (const Patch& patch : p.patches) std::memcpy(at(patch.rva, patch.bytes.size()), patch.bytes.data(), patch.bytes.size());
    return true;
}

#ifdef _WIN32
// apply() on a mapped image of `size` bytes at `base`, each write made writable for its length and its protection put
// back after.
inline bool apply_to_image(std::uint8_t* base, std::uint32_t size, const Plan& p, std::string* error) {
    const auto at = [&](std::uint32_t rva, std::size_t length) -> std::uint8_t* {
        return std::uint64_t(rva) + length <= size ? base + rva : nullptr;
    };
    if (!check(at, p, error)) return false;
    for (const Patch& patch : p.patches) {
        std::uint8_t* where = at(patch.rva, patch.bytes.size());
        DWORD old = 0;
        if (!VirtualProtect(where, patch.bytes.size(), PAGE_EXECUTE_READWRITE, &old)) {
            if (error) *error = std::string("the ") + patch.what + " could not be made writable";
            return false;
        }
        std::memcpy(where, patch.bytes.data(), patch.bytes.size());
        VirtualProtect(where, patch.bytes.size(), old, &old);
        FlushInstructionCache(GetCurrentProcess(), where, patch.bytes.size());
    }
    return true;
}
#endif

}  // namespace leonida::display_profile
