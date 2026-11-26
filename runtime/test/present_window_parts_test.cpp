// The title window's pieces that need no window and no GPU: the PNG writer of the screenshots (src/png_write.h),
// checked by decoding its files with the runtime's own inflate (src/shader_library.h) and undoing the row filters; the
// caption's figures (src/frame_meter.h) on made-up present times; and LEONIDA_DISPLAY_MODE's parser
// (src/display_mode.h). Hand-made data only.
#include "../src/display_mode.h"
#include "../src/display_profile.h"
#include "../src/frame_meter.h"
#include "../src/png_write.h"
#include "../src/shader_library.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

std::uint32_t be32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}

// A PNG of colour type 2 back to RGB rows; false when a chunk, its CRC, the stream or a filter is wrong.
bool decode_rgb8(const std::vector<std::uint8_t>& png, std::uint32_t* width, std::uint32_t* height,
                 std::vector<std::uint8_t>* rgb) {
    static const std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (png.size() < 8 || std::memcmp(png.data(), signature, 8) != 0) return false;
    std::vector<std::uint8_t> idat;
    bool header = false, end = false;
    for (std::size_t at = 8; at + 12 <= png.size() && !end;) {
        const std::uint32_t length = be32(&png[at]);
        if (at + 12 + length > png.size()) return false;
        const std::uint8_t* kind = &png[at + 4];
        const std::uint8_t* data = &png[at + 8];
        if (leonida::png::crc32({kind, length + 4}) != be32(data + length)) return false;
        if (!std::memcmp(kind, "IHDR", 4)) {
            if (length != 13 || data[8] != 8 || data[9] != 2 || data[10] || data[11] || data[12]) return false;
            *width = be32(data);
            *height = be32(data + 4);
            header = true;
        } else if (!std::memcmp(kind, "IDAT", 4)) {
            idat.insert(idat.end(), data, data + length);
        } else if (!std::memcmp(kind, "IEND", 4)) {
            end = true;
        }
        at += 12 + length;
    }
    if (!header || !end) return false;
    std::vector<std::uint8_t> raw;
    if (!leonida::zlib_inflate(idat, raw)) return false;
    const std::size_t row = std::size_t(*width) * 3;
    if (raw.size() != (row + 1) * *height) return false;
    rgb->assign(row * *height, 0);
    for (std::uint32_t y = 0; y < *height; ++y) {
        const std::uint8_t filter = raw[y * (row + 1)];
        const std::uint8_t* in = &raw[y * (row + 1) + 1];
        std::uint8_t* out = &(*rgb)[y * row];
        const std::uint8_t* up = y ? &(*rgb)[(y - 1) * row] : nullptr;
        for (std::size_t i = 0; i < row; ++i) {
            const int a = i >= 3 ? out[i - 3] : 0, b = up ? up[i] : 0, c = up && i >= 3 ? up[i - 3] : 0;
            int predicted = 0;
            switch (filter) {
            case 0: break;
            case 1: predicted = a; break;
            case 2: predicted = b; break;
            case 3: predicted = (a + b) / 2; break;
            case 4: {
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                predicted = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                break;
            }
            default: return false;
            }
            out[i] = std::uint8_t(in[i] + predicted);
        }
    }
    return true;
}

void png_tests() {
    const char* digits = "123456789";
    check(leonida::png::crc32({reinterpret_cast<const std::uint8_t*>(digits), 9}) == 0xcbf43926u, "CRC-32 check value");
    const char* wiki = "Wikipedia";
    check(leonida::png::adler32({reinterpret_cast<const std::uint8_t*>(wiki), 9}) == 0x11e60398u, "Adler-32 of Wikipedia");

    // Deflate round trips: empty, tiny, runs longer than 258, repeats further back than the window, noise.
    std::uint32_t seed = 12345;
    const auto next = [&seed] { return seed = seed * 1664525u + 1013904223u; };
    std::vector<std::vector<std::uint8_t>> inputs(6);
    inputs[1] = {7};
    inputs[2] = {1, 2, 3, 1, 2, 3, 1, 2, 3, 1};
    inputs[3].assign(100000, 0x42);
    for (int i = 0; i < 70000; ++i) inputs[4].push_back(std::uint8_t((i % 50000) * 7 % 251));
    for (int i = 0; i < 50000; ++i) inputs[5].push_back(std::uint8_t(next() >> 24));
    for (const auto& input : inputs) {
        const std::vector<std::uint8_t> stream = leonida::png::zlib_compress(input);
        std::vector<std::uint8_t> back;
        check(leonida::zlib_inflate(stream, back) && back == input, "zlib round trip");
    }
    check(leonida::png::zlib_compress(inputs[3]).size() < 2000, "a run of 100000 bytes compresses");

    // An image with flat regions, gradients and noise, at an odd width.
    const std::uint32_t width = 333, height = 77;
    std::vector<std::uint8_t> image(std::size_t(width) * height * 3);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t* p = &image[(std::size_t(y) * width + x) * 3];
            if (x < width / 3) {
                p[0] = 64, p[1] = 128, p[2] = 191;
            } else if (x < 2 * width / 3) {
                p[0] = std::uint8_t(x), p[1] = std::uint8_t(y * 3), p[2] = std::uint8_t(x + y);
            } else {
                const std::uint32_t r = next();
                p[0] = std::uint8_t(r), p[1] = std::uint8_t(r >> 8), p[2] = std::uint8_t(r >> 16);
            }
        }
    const std::vector<std::uint8_t> png = leonida::png::encode_rgb8(width, height, image.data(), std::size_t(width) * 3);
    std::uint32_t w = 0, h = 0;
    std::vector<std::uint8_t> decoded;
    check(decode_rgb8(png, &w, &h, &decoded), "the PNG decodes");
    check(w == width && h == height && decoded == image, "the PNG holds the image");
    // A pitch wider than the row: only the row's bytes are read.
    std::vector<std::uint8_t> padded(std::size_t(width * 3 + 5) * height, 0xee);
    for (std::uint32_t y = 0; y < height; ++y)
        std::memcpy(&padded[std::size_t(y) * (width * 3 + 5)], &image[std::size_t(y) * width * 3], width * 3);
    check(leonida::png::encode_rgb8(width, height, padded.data(), width * 3 + 5) == png, "the pitch is honoured");
    check(leonida::png::encode_rgb8(0, 10, image.data(), 0).empty(), "an empty image gives no file");
}

void meter_tests() {
    constexpr std::uint64_t ms = 1'000'000;
    {
        leonida::FrameMeter meter;
        leonida::FrameRateSampler sampler;
        leonida::FrameFigures f;
        check(!sampler.sample(meter.read(), 1000 * ms, &f), "no figure before a frame");
        for (int i = 0; i <= 60; ++i) meter.presented(std::uint64_t(1000.0 * ms + i * 16.6667 * ms));
        check(sampler.sample(meter.read(), 2010 * ms, &f), "a figure after a second of frames");
        check(std::fabs(f.fps - 60.0) < 0.1 && std::fabs(f.ms - 16.667) < 0.01, "60 frames a second");
        check(leonida::caption_text(f) == "Leonida | 60 fps 16.7 ms/frame", "the caption's words");
        // No frame for three seconds: the wait shows once it is longer than a frame.
        check(sampler.sample(meter.read(), 2010 * ms, &f) && std::fabs(f.fps - 60.0) < 0.1, "a short wait keeps them");
        check(sampler.sample(meter.read(), 5000 * ms, &f) && f.fps < 1.0 && std::fabs(f.ms - 3000.0) < 1.0,
              "a long wait is shown");
        check(leonida::caption_text(f) == "Leonida | 0 fps 3000.0 ms/frame", "the wait's caption");
        // The frame that ends the wait counts its whole time.
        meter.presented(5200 * ms);
        check(sampler.sample(meter.read(), 5300 * ms, &f) && std::fabs(f.ms - 3200.0) < 1.0, "the long frame");
    }
    {
        // Slow frames (V host.rs's test: about 1 fps): each sample spans whole frames.
        leonida::FrameMeter meter;
        leonida::FrameRateSampler sampler;
        leonida::FrameFigures f;
        meter.presented(0 + 1);
        check(!sampler.sample(meter.read(), 500 * ms, &f), "one frame, half a second: nothing yet");
        meter.presented(953 * ms);
        check(sampler.sample(meter.read(), 1000 * ms, &f) && std::fabs(f.ms - 953.0) < 0.01, "953 ms a frame");
        check(leonida::caption_text(f) == "Leonida | 1 fps 953.0 ms/frame", "about one frame a second");
    }
}

void display_mode_tests() {
    leonida::DisplayMode m;
    check(m.width == 3840 && m.height == 2160 && m.refresh_hz == 60, "the default is VIctory's: 3840x2160 at 60 Hz");
    check(leonida::parse_display_mode("1920x1080", &m) && m.width == 1920 && m.height == 1080, "1920x1080");
    check(leonida::parse_display_mode(" 2560 X 1440 \r\n", &m) && m.width == 2560 && m.height == 1440, "spaces and X");
    check(leonida::parse_display_mode("640x360", &m) && leonida::parse_display_mode("7680x4320", &m), "the bounds");
    const char* refused[] = {"", "1920", "x1080", "1920x", "1920x1080x", "639x360", "640x359", "7681x4320",
                             "7680x4321", "abcxdef", "-1920x1080", "123456x1080", "1920 1080", "1920x10 80"};
    m = {};
    for (const char* text : refused) {
        const bool parsed = leonida::parse_display_mode(text, &m);
        if (parsed) std::fprintf(stderr, "parsed: '%s'\n", text);
        check(!parsed, "a value outside the rules is refused");
    }
    check(m.width == 3840 && m.height == 2160, "a refused value leaves the mode alone");
}

// The display profile on a sparse stand-in for the image: the bytes VIctory's native_1440p.rs expects at each place.
void display_profile_tests() {
    namespace dp = leonida::display_profile;
    check(dp::scanout_pool_bytes(3840, 2160) == 0x04000000u, "the formula gives the stock reservation for 3840x2160");
    check(dp::scanout_pool_bytes(2560, 1440) == 0x01e00000u, "and 30 MiB for 2560x1440");
    check(dp::plan(leonida::DisplayMode{}).patches.empty() && !dp::plan(leonida::DisplayMode{}).refused,
          "the stock mode changes nothing");
    leonida::DisplayMode fhd;
    fhd.width = 1920;
    fhd.height = 1080;
    check(dp::plan(fhd).patches.empty() && dp::plan(fhd).refused, "an output VIctory has not checked is refused");
    check(dp::plan(leonida::DisplayMode{}, 0.5f).patches.empty() && dp::plan(leonida::DisplayMode{}, 0.5f).refused,
          "a render scale without the 1440p output is refused");

    std::map<std::uint32_t, std::vector<std::uint8_t>> image;
    image[dp::kSdrModeRva] = {0, 15, 0, 0, 112, 8, 0, 0, 60, 0, 0, 0, 1, 0, 0, 0};
    image[dp::kScanoutReservationRva] = {0xb9, 0, 0, 0, 4};
    const float two_thirds = 2.0f / 3.0f;
    image[dp::kSceneScaleRva].resize(4);
    std::memcpy(image[dp::kSceneScaleRva].data(), &two_thirds, 4);
    image[dp::kSettingsViewportLoadRva] = {0xc5, 0xfb, 0x10, 0x0d, 0x10, 0xf0, 0x13, 0x05};
    const auto at = [&image](std::uint32_t rva, std::size_t length) -> std::uint8_t* {
        const auto found = image.find(rva);
        return found != image.end() && found->second.size() >= length ? found->second.data() : nullptr;
    };
    std::string error;
    leonida::DisplayMode qhd;
    qhd.width = 2560;
    qhd.height = 1440;
    const dp::Plan plan = dp::plan(qhd);
    check(plan.render_width == 2560 && plan.render_height == 1440 && plan.scale == 1.0f, "1440p renders at 1440p");
    check(dp::apply(at, plan, &error), "the profile applies to the stock bytes");
    check(image[dp::kSdrModeRva] == std::vector<std::uint8_t>{0, 10, 0, 0, 160, 5, 0, 0, 60, 0, 0, 0, 1, 0, 0, 0},
          "the SDR mode is 2560x1440 at 60 Hz");
    check(image[dp::kScanoutReservationRva] == std::vector<std::uint8_t>{0xb9, 0, 0, 0xe0, 1}, "30 MiB reserved");
    float scale = 0;
    std::memcpy(&scale, image[dp::kSceneScaleRva].data(), 4);
    check(scale == 1.0f, "the scene scale is 1");
    std::int32_t displacement = 0;
    std::memcpy(&displacement, image[dp::kSettingsViewportLoadRva].data() + 4, 4);
    check(std::int64_t(dp::kSettingsViewportLoadRva) + 8 + displacement == dp::kSdrModeRva,
          "the Settings viewport reads the SDR mode");
    // V: 1080p rendering inside the 1440p output is a scale of 0.75.
    const dp::Plan p1080 = dp::plan(qhd, 0.75f);
    check(p1080.render_width == 1920 && p1080.render_height == 1080, "0.75 of 1440p renders 1080p");
    // Patched once, the bytes are not stock any more: a second pass (another build, in effect) writes nothing.
    const auto before = image;
    check(!dp::apply(at, p1080, &error) && image == before && !error.empty(), "other bytes are left alone");
}

}  // namespace

int main() {
    png_tests();
    meter_tests();
    display_mode_tests();
    display_profile_tests();
    if (failures) {
        std::fprintf(stderr, "present_window_parts_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("present_window_parts_test: OK\n");
    return 0;
}
