#pragma once

// The display mode the console reports to the title: LEONIDA_DISPLAY_MODE=WxH, default 3840x2160.
//
// Ported from VIctory (GPL-3.0-or-later, Marvisak and contributors, DexNom fork;
// victory/crates/victory-gpu/src/dxgi.rs:222-240 `display_size`, VICTORY_DISPLAY_MODE): a console reports the
// television it is plugged into (a Series X on a 1080p set lists a 1080p mode). VIctory accepts W in 640..7680 and H in
// 360..4320 and otherwise reports 3840x2160; so does this. It does not follow the host monitor either (V: "not yet the
// host display's own size by default"). The title reads the mode but does not size its frames by it: what does is
// display_profile.h. No Windows headers; present_window_parts_test checks the parser.

#include <cstdint>
#include <string_view>

namespace leonida {

struct DisplayMode {
    std::uint32_t width = 3840;
    std::uint32_t height = 2160;
    std::uint32_t refresh_hz = 60;  // V dxgi.rs `modes`: one mode at 60 Hz
};

// "WxH" (an `x` or `X` between, spaces around the numbers allowed), W in 640..7680 and H in 360..4320. False, with
// `*out` untouched, for anything else.
inline bool parse_display_mode(std::string_view text, DisplayMode* out) {
    const auto trim = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
            s.remove_suffix(1);
        return s;
    };
    const auto number = [](std::string_view s, std::uint32_t* value) {
        if (s.empty() || s.size() > 5) return false;
        std::uint32_t v = 0;
        for (const char c : s) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + std::uint32_t(c - '0');
        }
        *value = v;
        return true;
    };
    text = trim(text);
    const std::size_t x = text.find_first_of("xX");
    if (x == std::string_view::npos) return false;
    std::uint32_t width = 0, height = 0;
    if (!number(trim(text.substr(0, x)), &width) || !number(trim(text.substr(x + 1)), &height)) return false;
    if (width < 640 || width > 7680 || height < 360 || height > 4320) return false;
    if (out) {
        out->width = width;
        out->height = height;
    }
    return true;
}

}  // namespace leonida
