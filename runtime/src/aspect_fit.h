#pragma once

// Ultrawide support (docs/ultrawide.md): the aspect the user asks for, the wider display mode the title is told about,
// and where a frame goes in a window of another shape. Pure arithmetic, no Windows or Vulkan headers;
// test/aspect_fit_test.cpp checks it.
//
//   LEONIDA_ASPECT=W:H (or a decimal, or `auto` for the primary monitor's shape): the console output's display mode
//     (shims/xbox_display.cpp) and the title's window (shims/title_window.cpp) are widened to that aspect, keeping their
//     height. Unset, empty, `off` or `none`: nothing changes. A ratio at or below the mode's own aspect changes nothing.
//   LEONIDA_PRESENT_SCALE=fit|stretch: how a frame is put in a window of another shape (src/vulkan_present.h). `fit`
//     keeps the frame's aspect: it is centred, as large as fits, with black bars beside it (pillarbox) or above and below
//     (letterbox). `stretch` fills the window whatever the shapes, as before. Default: `fit` when LEONIDA_ASPECT asks for
//     an aspect, else `stretch`.

#include "display_mode.h"

#include <cmath>
#include <cstdint>
#include <string_view>

namespace leonida::aspect {

// The ratios LEONIDA_ASPECT takes: square up to four times as wide as high (32:9 is 3.56).
inline constexpr double kMinRatio = 1.0;
inline constexpr double kMaxRatio = 4.0;

enum class Kind { none, ratio, automatic };

struct Request {
    Kind kind = Kind::none;
    double ratio = 0.0;  // width over height, for Kind::ratio
};

namespace detail {
inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}
inline bool same_word(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if (c != b[i]) return false;
    }
    return true;
}
// A decimal number without sign or exponent ("21", "2.39", ".5"), locale-independent. False for anything else.
inline bool number(std::string_view s, double* out) {
    s = trim(s);
    if (s.empty() || s.size() > 12) return false;
    double value = 0.0, scale = 1.0;
    bool dot = false, digits = false;
    for (const char c : s) {
        if (c == '.') {
            if (dot) return false;
            dot = true;
        } else if (c >= '0' && c <= '9') {
            digits = true;
            if (dot) {
                scale /= 10.0;
                value += scale * double(c - '0');
            } else {
                value = value * 10.0 + double(c - '0');
            }
        } else {
            return false;
        }
    }
    if (!digits) return false;
    *out = value;
    return true;
}
inline std::uint32_t round_even(double v) { return std::uint32_t(std::lround(v / 2.0)) * 2u; }
}  // namespace detail

// LEONIDA_ASPECT's text: empty, `off` or `none` (any case) is Kind::none; `auto` is Kind::automatic; `W:H` or `W/H`
// (decimals allowed on either side) or a single decimal is Kind::ratio when the ratio is in kMinRatio..kMaxRatio.
// False, with `*out` untouched, for anything else.
inline bool parse_request(std::string_view text, Request* out) {
    text = detail::trim(text);
    Request r;
    if (text.empty() || detail::same_word(text, "off") || detail::same_word(text, "none")) {
        r.kind = Kind::none;
    } else if (detail::same_word(text, "auto")) {
        r.kind = Kind::automatic;
    } else {
        const std::size_t colon = text.find_first_of(":/");
        double ratio = 0.0;
        if (colon == std::string_view::npos) {
            if (!detail::number(text, &ratio)) return false;
        } else {
            double w = 0.0, h = 0.0;
            if (!detail::number(text.substr(0, colon), &w) || !detail::number(text.substr(colon + 1), &h) || !(h > 0.0))
                return false;
            ratio = w / h;
        }
        if (!(ratio >= kMinRatio && ratio <= kMaxRatio)) return false;
        r.kind = Kind::ratio;
        r.ratio = ratio;
    }
    if (out) *out = r;
    return true;
}

// The ratio a request stands for: the monitor's `monitor_width` / `monitor_height` for Kind::automatic (0 when that is
// not known or outside kMinRatio..kMaxRatio), 0 for Kind::none.
inline double resolve(const Request& request, std::uint32_t monitor_width = 0, std::uint32_t monitor_height = 0) {
    if (request.kind == Kind::ratio) return request.ratio;
    if (request.kind == Kind::automatic && monitor_width && monitor_height) {
        const double ratio = double(monitor_width) / double(monitor_height);
        return ratio >= kMinRatio && ratio <= kMaxRatio ? ratio : 0.0;
    }
    return 0.0;
}

struct Size {
    std::uint32_t width = 0, height = 0;
};

// `size` made `ratio` wide by widening it (the height kept, the width rounded to an even number). Unchanged when the
// ratio is not wider than the size already is, or for an empty size.
inline Size widen(Size size, double ratio) {
    if (!size.width || !size.height || !(ratio > 0.0) || !std::isfinite(ratio)) return size;
    const double width = double(size.height) * ratio;
    if (width < double(size.width) + 1.0) return size;
    size.width = detail::round_even(width);
    return size;
}

// The display mode the console output reports with LEONIDA_ASPECT: `mode` widened to `ratio` (widen above), kept inside
// parse_display_mode's range: a width above 7680 is 7680, with the height made smaller to keep the ratio.
inline DisplayMode widen_display_mode(DisplayMode mode, double ratio) {
    const Size wide = widen({mode.width, mode.height}, ratio);
    if (wide.width == mode.width) return mode;
    mode.width = wide.width;
    if (mode.width > 7680) {
        mode.width = 7680;
        mode.height = detail::round_even(7680.0 / ratio);
        if (mode.height < 360) mode.height = 360;
    }
    return mode;
}

enum class Scaling { stretch, fit };

// LEONIDA_PRESENT_SCALE's text: `fit` or `stretch` (any case). False, with `*out` untouched, for anything else.
inline bool parse_scaling(std::string_view text, Scaling* out) {
    text = detail::trim(text);
    Scaling s;
    if (detail::same_word(text, "fit")) s = Scaling::fit;
    else if (detail::same_word(text, "stretch")) s = Scaling::stretch;
    else return false;
    if (out) *out = s;
    return true;
}

// The scaling from the two variables' values (null: unset): LEONIDA_PRESENT_SCALE when it parses, else `fit` when
// LEONIDA_ASPECT asks for an aspect (a ratio or `auto`), else `stretch`.
inline Scaling scaling_from(const char* present_scale, const char* aspect_text) {
    Scaling s = Scaling::stretch;
    if (present_scale && parse_scaling(present_scale, &s)) return s;
    Request request;
    if (aspect_text && parse_request(aspect_text, &request) && request.kind != Kind::none) return Scaling::fit;
    return Scaling::stretch;
}

// A rectangle of the output in pixels: left/top inclusive, right/bottom exclusive.
struct Box {
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;
    std::int32_t width() const { return right - left; }
    std::int32_t height() const { return bottom - top; }
};

inline bool is_whole(const Box& box, std::uint32_t out_width, std::uint32_t out_height) {
    return box.left == 0 && box.top == 0 && box.right == std::int32_t(out_width) && box.bottom == std::int32_t(out_height);
}

// Where content of aspect `content` (width over height) goes in an output of `out_width` x `out_height` with
// Scaling::fit: centred, as large as fits, so the bars are beside it when the content is narrower (pillarbox) and
// above and below when it is wider (letterbox). The whole output when the two shapes differ by less than a pixel, and
// for an empty output or a content aspect that is not a positive finite number.
inline Box fit_box(double content, std::uint32_t out_width, std::uint32_t out_height) {
    const Box whole{0, 0, std::int32_t(out_width), std::int32_t(out_height)};
    if (!out_width || !out_height || !(content > 0.0) || !std::isfinite(content)) return whole;
    const double ow = double(out_width), oh = double(out_height);
    if (content < ow / oh) {  // narrower: pillarbox
        const double width = oh * content;
        if (ow - width < 1.0) return whole;
        std::int32_t w = std::int32_t(std::lround(width));
        if (w < 1) w = 1;
        const std::int32_t left = (std::int32_t(out_width) - w) / 2;
        return {left, 0, left + w, std::int32_t(out_height)};
    }
    const double height = ow / content;  // wider (or the same): letterbox
    if (oh - height < 1.0) return whole;
    std::int32_t h = std::int32_t(std::lround(height));
    if (h < 1) h = 1;
    const std::int32_t top = (std::int32_t(out_height) - h) / 2;
    return {0, top, std::int32_t(out_width), top + h};
}

// The aspect of the output a plane was composed for: a source of `source_width` x `source_height` texels shown
// undistorted in `dest` (left, top, right, bottom as fractions of that output) makes it
// (source_width / source_height) * (dest height / dest width). 0 for an empty source or destination.
inline double content_aspect(double source_width, double source_height, const float dest[4]) {
    const double dw = double(dest[2]) - double(dest[0]), dh = double(dest[3]) - double(dest[1]);
    if (!(source_width > 0.0) || !(source_height > 0.0) || !(dw > 0.0) || !(dh > 0.0)) return 0.0;
    return source_width / source_height * dh / dw;
}

// `dest` (fractions of the title's output) in output pixels once that output is `box`.
inline void place(const float dest[4], const Box& box, float out[4]) {
    const float w = float(box.width()), h = float(box.height());
    out[0] = float(box.left) + dest[0] * w;
    out[1] = float(box.top) + dest[1] * h;
    out[2] = float(box.left) + dest[2] * w;
    out[3] = float(box.top) + dest[3] * h;
}

}  // namespace leonida::aspect
