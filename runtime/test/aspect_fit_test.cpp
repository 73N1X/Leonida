// src/aspect_fit.h: LEONIDA_ASPECT's and LEONIDA_PRESENT_SCALE's parsers, the widened display mode, and where a frame
// goes in a window of another shape (pillarbox, letterbox, fill). No window, no GPU, no game data.
#include "../src/aspect_fit.h"

#include <cmath>
#include <cstdio>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace a = leonida::aspect;

static bool near(double x, double y) { return std::fabs(x - y) < 1e-9; }
static bool box_is(const a::Box& b, int l, int t, int r, int bottom) {
    return b.left == l && b.top == t && b.right == r && b.bottom == bottom;
}

int main() {
    // LEONIDA_ASPECT.
    a::Request r;
    EXPECT(a::parse_request("21:9", &r) && r.kind == a::Kind::ratio && near(r.ratio, 21.0 / 9.0));
    EXPECT(a::parse_request(" 32:9 ", &r) && r.kind == a::Kind::ratio && near(r.ratio, 32.0 / 9.0));
    EXPECT(a::parse_request("43/18", &r) && near(r.ratio, 43.0 / 18.0));
    EXPECT(a::parse_request("2.39", &r) && near(r.ratio, 2.39));
    EXPECT(a::parse_request("2.39:1", &r) && near(r.ratio, 2.39));
    EXPECT(a::parse_request("16:9", &r) && near(r.ratio, 16.0 / 9.0));
    EXPECT(a::parse_request("AUTO", &r) && r.kind == a::Kind::automatic);
    EXPECT(a::parse_request("off", &r) && r.kind == a::Kind::none);
    EXPECT(a::parse_request("", &r) && r.kind == a::Kind::none);
    r = {a::Kind::ratio, 3.0};
    for (const char* bad : {"wide", "21:", ":9", "21:0", "1:2", "5:1", "2,39", "-2", "21x9", "1..2", "."})
        EXPECT(!a::parse_request(bad, &r) && r.kind == a::Kind::ratio && r.ratio == 3.0);  // untouched

    EXPECT(near(a::resolve({a::Kind::ratio, 2.0}), 2.0));
    EXPECT(a::resolve({a::Kind::none, 0.0}, 3440, 1440) == 0.0);
    EXPECT(near(a::resolve({a::Kind::automatic, 0.0}, 3440, 1440), 3440.0 / 1440.0));
    EXPECT(a::resolve({a::Kind::automatic, 0.0}, 0, 0) == 0.0);
    EXPECT(a::resolve({a::Kind::automatic, 0.0}, 1080, 1920) == 0.0);  // a portrait monitor is not widened to

    // The widened display mode: the height kept, the width even.
    const leonida::DisplayMode uhd;  // 3840x2160
    leonida::DisplayMode m = a::widen_display_mode(uhd, 21.0 / 9.0);
    EXPECT(m.width == 5040 && m.height == 2160 && m.refresh_hz == 60);
    m = a::widen_display_mode(uhd, 32.0 / 9.0);
    EXPECT(m.width == 7680 && m.height == 2160);
    m = a::widen_display_mode({2560, 1440, 60}, 21.0 / 9.0);
    EXPECT(m.width == 3360 && m.height == 1440);
    m = a::widen_display_mode({2560, 1440, 60}, 3440.0 / 1440.0);
    EXPECT(m.width == 3440 && m.height == 1440);
    m = a::widen_display_mode({2560, 1440, 60}, 32.0 / 9.0);
    EXPECT(m.width == 5120 && m.height == 1440);
    m = a::widen_display_mode({1920, 1080, 60}, 64.0 / 27.0);
    EXPECT(m.width == 2560 && m.height == 1080);
    m = a::widen_display_mode(uhd, 16.0 / 9.0);  // not wider: unchanged
    EXPECT(m.width == 3840 && m.height == 2160);
    m = a::widen_display_mode(uhd, 1.5);  // narrower: unchanged (only widened)
    EXPECT(m.width == 3840 && m.height == 2160);
    m = a::widen_display_mode({7680, 4320, 60}, 21.0 / 9.0);  // past 7680: the height gives way
    EXPECT(m.width == 7680 && m.height == 3292);
    EXPECT(leonida::parse_display_mode("7680x3292", nullptr));  // still a mode the parser's range holds
    m = a::widen_display_mode(uhd, 0.0);
    EXPECT(m.width == 3840 && m.height == 2160);

    const a::Size window = a::widen({2560, 1440}, 21.0 / 9.0);
    EXPECT(window.width == 3360 && window.height == 1440);
    EXPECT(a::widen({0, 1440}, 3.0).width == 0);

    // LEONIDA_PRESENT_SCALE, and its default from LEONIDA_ASPECT.
    a::Scaling s = a::Scaling::fit;
    EXPECT(a::parse_scaling("Stretch", &s) && s == a::Scaling::stretch);
    EXPECT(a::parse_scaling("fit", &s) && s == a::Scaling::fit);
    EXPECT(!a::parse_scaling("zoom", &s) && s == a::Scaling::fit);
    EXPECT(a::scaling_from(nullptr, nullptr) == a::Scaling::stretch);
    EXPECT(a::scaling_from(nullptr, "21:9") == a::Scaling::fit);
    EXPECT(a::scaling_from(nullptr, "auto") == a::Scaling::fit);
    EXPECT(a::scaling_from(nullptr, "off") == a::Scaling::stretch);
    EXPECT(a::scaling_from(nullptr, "nonsense") == a::Scaling::stretch);
    EXPECT(a::scaling_from("stretch", "21:9") == a::Scaling::stretch);
    EXPECT(a::scaling_from("fit", nullptr) == a::Scaling::fit);
    EXPECT(a::scaling_from("bogus", "32:9") == a::Scaling::fit);

    // Where a frame goes. 16:9 in 21:9 (3440x1440): pillarboxed, 2560 wide, centred.
    const double sixteen_nine = 16.0 / 9.0;
    EXPECT(box_is(a::fit_box(sixteen_nine, 3440, 1440), 440, 0, 3000, 1440));
    // 16:9 in 32:9 (5120x1440): half the width, a quarter each side.
    EXPECT(box_is(a::fit_box(sixteen_nine, 5120, 1440), 1280, 0, 3840, 1440));
    // 16:9 in 16:9 at another size: the whole window.
    EXPECT(box_is(a::fit_box(sixteen_nine, 1920, 1080), 0, 0, 1920, 1080));
    EXPECT(a::is_whole(a::fit_box(3840.0 / 2160.0, 2560, 1440), 2560, 1440));
    // A frame rendered wide fills a window of its shape (and one that rounds within a pixel of it).
    EXPECT(box_is(a::fit_box(3440.0 / 1440.0, 3440, 1440), 0, 0, 3440, 1440));
    EXPECT(box_is(a::fit_box(21.0 / 9.0, 3440, 1474), 0, 0, 3440, 1474));
    // A frame wider than the window: letterboxed.
    EXPECT(box_is(a::fit_box(32.0 / 9.0, 1920, 1080), 0, 270, 1920, 810));
    // A window shrunk to fit a work area (3440x1400): the box keeps 16:9 and is centred.
    const a::Box shrunk = a::fit_box(sixteen_nine, 3440, 1400);
    EXPECT(shrunk.top == 0 && shrunk.bottom == 1400 && shrunk.width() == 2489 && shrunk.left == (3440 - 2489) / 2);
    // Nothing to fit: the whole output.
    EXPECT(box_is(a::fit_box(0.0, 800, 600), 0, 0, 800, 600));
    EXPECT(box_is(a::fit_box(NAN, 800, 600), 0, 0, 800, 600));
    EXPECT(box_is(a::fit_box(sixteen_nine, 0, 0), 0, 0, 0, 0));
    // Extreme shapes never give an empty box.
    EXPECT(a::fit_box(1e-6, 1000, 10).width() == 1 && a::fit_box(1e6, 10, 1000).height() == 1);

    // The output a plane was composed for: its source shape over its placement's.
    const float whole[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    EXPECT(near(a::content_aspect(3840, 2160, whole), sixteen_nine));
    const float left_half[4] = {0.0f, 0.0f, 0.5f, 1.0f};  // 8:9 shown in the left half of a 16:9 output
    EXPECT(near(a::content_aspect(1920, 2160, left_half), sixteen_nine));
    const float empty[4] = {0.5f, 0.0f, 0.5f, 1.0f};
    EXPECT(a::content_aspect(1920, 2160, empty) == 0.0);
    EXPECT(a::content_aspect(0, 2160, whole) == 0.0);

    // Placements into the box: the whole output is the box, a half is half of it.
    float px[4];
    const a::Box pillar = a::fit_box(sixteen_nine, 3440, 1440);
    a::place(whole, pillar, px);
    EXPECT(px[0] == 440.0f && px[1] == 0.0f && px[2] == 3000.0f && px[3] == 1440.0f);
    a::place(left_half, pillar, px);
    EXPECT(px[0] == 440.0f && px[2] == 1720.0f);
    a::place(whole, a::Box{0, 0, 3440, 1440}, px);  // stretch: the fractions times the window, as before
    EXPECT(px[0] == 0.0f && px[2] == 3440.0f && px[3] == 1440.0f);

    std::printf("aspect_fit: ok\n");
    return 0;
}
