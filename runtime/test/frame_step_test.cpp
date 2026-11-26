// src/frame_step.h on a sparse stand-in for the 1.0.2.1 image: the loads it expects, what it writes, what it leaves
// alone, LEONIDA_TIME_STEP's values, and what the step comes to before and after, by running the title's step formula
// on the floats the (patched) loads read. No game files: the stand-in holds only the bytes the header checks.

#include "../src/frame_step.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
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

namespace fs = leonida::frame_step;
using Image = std::map<std::uint32_t, std::vector<std::uint8_t>>;

std::vector<std::uint8_t> float_bytes(float f) {
    std::vector<std::uint8_t> out(4);
    std::memcpy(out.data(), &f, 4);
    return out;
}

// The places the header looks at, as the retail executable has them (and the floats the loads read, at the values the
// timer runs with: its init has written 1/3000 s to the minimum step, the settings 1/30 s to the nominal frame time).
Image stock_image() {
    Image image;
    image[fs::kTimeSetCapLoadRva] = {0xc5, 0xfa, 0x10, 0x1d, 0x73, 0x3e, 0xf3, 0x03};
    image[fs::kTimeSetFloorLoadRva] = {0xc5, 0xfa, 0x10, 0x25, 0xe2, 0x37, 0x62, 0x04};
    image[fs::kCameraCapLoadRva] = {0xc5, 0xfa, 0x10, 0x05, 0x85, 0x36, 0xf3, 0x03};
    image[fs::kCameraFloorLoadRva] = {0xc5, 0xfa, 0x10, 0x0d, 0xe8, 0x2f, 0x62, 0x04};
    image[fs::kOneRva] = float_bytes(1.0f);
    image[fs::kMinStepRva] = float_bytes(1.0f / 30.0f);  // the file's value
    image[fs::kStockCapRva] = float_bytes(1.0f / 15.0f);
    image[fs::kNominalRva] = float_bytes(1.0f / 30.0f);
    return image;
}

auto access(Image& image) {
    return [&image](std::uint32_t rva, std::size_t length) -> std::uint8_t* {
        const auto found = image.find(rva);
        return found != image.end() && found->second.size() >= length ? found->second.data() : nullptr;
    };
}

// The float a `vmovss xmm, [rip + disp32]` at `rva` reads (NaN when it reads nothing in the stand-in).
float loaded(Image& image, std::uint32_t rva) {
    std::int32_t disp = 0;
    std::memcpy(&disp, image[rva].data() + 4, 4);
    const auto target = std::uint32_t(std::int64_t(rva) + fs::kLoadLength + disp);
    const auto found = image.find(target);
    if (found == image.end() || found->second.size() < 4) return std::nanf("");
    float f = 0;
    std::memcpy(&f, found->second.data(), 4);
    return f;
}

// The time sets' step as the title computes it (clip on, the nominal-floor switch set, scale 1): the floor and cap
// are what the two loads read.
float title_step(Image& image, float measured) {
    const float cap = loaded(image, fs::kTimeSetCapLoadRva);
    const float floor = loaded(image, fs::kTimeSetFloorLoadRva);
    const float x = std::fmin(cap, measured);
    return measured < floor ? floor : x;
}

// Game seconds per wall second at a steady `fps`.
float speed(Image& image, float fps) { return title_step(image, 1.0f / fps) * fps; }

bool close_to(float a, float b) { return std::fabs(a - b) <= 1e-3f * std::fmax(1.0f, std::fabs(b)); }

void tests() {
    // The plan: four loads (opcode and register kept, only the displacement moves) and two checked-only targets.
    const auto plan = fs::plan();
    int loads = 0, checked_only = 0;
    for (const fs::Patch& p : plan) {
        if (p.bytes.empty()) {
            ++checked_only;
            continue;
        }
        ++loads;
        check(p.stock.size() == fs::kLoadLength && p.bytes.size() == fs::kLoadLength, "a load is 8 bytes");
        check(std::memcmp(p.stock.data(), p.bytes.data(), 4) == 0, "a load keeps its opcode and register");
    }
    check(loads == 4 && checked_only == 2, "four loads, two values checked");

    // Stock: the step is 1/30 s at 30 fps and above, 1/15 s at 15 fps and below.
    Image image = stock_image();
    check(close_to(speed(image, 2.0f), 2.0f / 15.0f), "stock: 2 fps runs at 2/15 of real time");
    check(close_to(speed(image, 15.0f), 1.0f) && close_to(speed(image, 30.0f), 1.0f), "stock: real time at 15 and 30 fps");
    check(close_to(speed(image, 60.0f), 2.0f), "stock: a doubled frame rate doubles the speed");
    check(close_to(speed(image, 120.0f), 4.0f), "stock: 120 fps runs four times too fast");

    std::string error;
    check(fs::apply(access(image), plan, &error), "the patch applies to the stock bytes");
    check(error.empty(), "and says nothing");
    check(image[fs::kOneRva] == float_bytes(1.0f) && image[fs::kMinStepRva] == float_bytes(1.0f / 30.0f) &&
              image[fs::kStockCapRva] == float_bytes(1.0f / 15.0f) && image[fs::kNominalRva] == float_bytes(1.0f / 30.0f),
          "no data is written: only the loads move");
    check(loaded(image, fs::kTimeSetCapLoadRva) == 1.0f && loaded(image, fs::kCameraCapLoadRva) == 1.0f,
          "both caps read the timer's 1.0");
    // The minimum step as the timer's init leaves it.
    image[fs::kMinStepRva] = float_bytes(1.0f / 3000.0f);
    check(loaded(image, fs::kTimeSetFloorLoadRva) == 1.0f / 3000.0f &&
              loaded(image, fs::kCameraFloorLoadRva) == 1.0f / 3000.0f,
          "both floors read the timer's minimum step");
    for (const float fps : {1.0f, 2.0f, 4.0f, 15.0f, 30.0f, 60.0f, 120.0f, 240.0f, 1000.0f})
        check(close_to(speed(image, fps), 1.0f), "patched: real time from 1 to 1000 fps");
    check(close_to(title_step(image, 0.5f), 0.5f), "patched: a 2 fps frame steps its 0.5 s");
    check(close_to(title_step(image, 3.0f), 1.0f), "patched: a hitch steps 1 s at most");

    // Patched once, the bytes are not stock any more: a second pass writes nothing.
    image[fs::kMinStepRva] = float_bytes(1.0f / 30.0f);
    const Image patched = image;
    check(!fs::apply(access(image), plan, &error) && image == patched && !error.empty(), "a second pass is refused");

    // Another build: one target value differs, or one load differs. Nothing is written.
    Image other = stock_image();
    other[fs::kOneRva] = float_bytes(0.5f);
    const Image other_before = other;
    check(!fs::apply(access(other), plan, &error) && other == other_before, "another 1.0 constant: nothing written");
    other = stock_image();
    other[fs::kCameraFloorLoadRva][5] ^= 1;
    const Image other_load = other;
    check(!fs::apply(access(other), plan, &error) && other == other_load, "another camera load: nothing written");
    Image empty;
    check(!fs::apply(access(empty), plan, &error), "an image without these places: nothing written");

    // LEONIDA_TIME_STEP.
    bool known = false;
    check(fs::wanted(nullptr, &known) && known, "unset: measured");
    check(fs::wanted("", &known) && known, "empty: measured");
    check(fs::wanted("Measured", &known) && known, "measured, any case");
    check(!fs::wanted("stock", &known) && known && !fs::wanted("0") && !fs::wanted("OFF"), "stock, 0, off: the title's own");
    check(fs::wanted("sometimes", &known) && !known, "an unknown value is reported and measured");
}

#ifdef _WIN32
// apply_to_image on memory laid out like the mapped image: the stock bytes at their RVAs in committed pages, the code
// pages execute-read and the data pages read-only, as the loader leaves them. Two loads share a page, so a protection
// put back too early would fault on the next write.
void image_tests() {
    const std::uint32_t size = (fs::kNominalRva & ~0xfffu) + 0x2000;
    auto* base = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS));
    check(base != nullptr, "the stand-in image is reserved");
    if (!base) return;
    const Image stock = stock_image();
    for (const auto& [rva, bytes] : stock) {
        std::uint8_t* page = base + (rva & ~0xfffu);
        VirtualAlloc(page, 0x2000, MEM_COMMIT, PAGE_READWRITE);
        std::memcpy(base + rva, bytes.data(), bytes.size());
    }
    const auto protection = [&](std::uint32_t rva) {
        MEMORY_BASIC_INFORMATION info{};
        VirtualQuery(base + rva, &info, sizeof info);
        return info.Protect;
    };
    DWORD old = 0;
    for (const auto& [rva, bytes] : stock)
        VirtualProtect(base + (rva & ~0xfffu), 0x2000,
                       rva < fs::kOneRva ? PAGE_EXECUTE_READ : PAGE_READONLY, &old);
    std::string error;
    check(fs::apply_to_image(base, size, fs::plan(), &error), "the patch applies to the mapped stand-in");
    Image image = stock_image();
    check(fs::apply(access(image), fs::plan(), &error), "the expected bytes");
    bool same = true;
    for (const auto& [rva, bytes] : image) same = same && std::memcmp(base + rva, bytes.data(), bytes.size()) == 0;
    check(same, "the mapped stand-in holds the patched loads and unchanged data");
    check(protection(fs::kTimeSetCapLoadRva) == PAGE_EXECUTE_READ && protection(fs::kTimeSetFloorLoadRva) == PAGE_EXECUTE_READ &&
              protection(fs::kCameraCapLoadRva) == PAGE_EXECUTE_READ && protection(fs::kOneRva) == PAGE_READONLY,
          "the protections are put back");
    check(!fs::apply_to_image(base, size, fs::plan(), &error), "a second pass is refused");
    check(!fs::apply_to_image(base, fs::kOneRva, fs::plan(), &error), "places past the image are refused");
    VirtualFree(base, 0, MEM_RELEASE);
}
#endif

}  // namespace

int main() {
    tests();
#ifdef _WIN32
    image_tests();
#endif
    if (failures) {
        std::fprintf(stderr, "frame_step_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("frame_step_test: OK\n");
    return 0;
}
