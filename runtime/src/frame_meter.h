#pragma once

// The frame rate in the title's window caption: "Leonida | <fps> fps <ms> ms/frame", refreshed once a second.
//
// Ported from VIctory (GPL-3.0-or-later, Marvisak and contributors, DexNom fork;
// victory/crates/victory-gpu/src/host.rs `Meter::frame` and the "victory-caption" thread): the present path only
// counts the frame, and a thread of the runtime's own sets the caption, so presenting never waits for the window's
// thread. VIctory computes its figures at the first present a second after the last report; here the caption thread
// samples once a second and measures the frames between the newest present before its previous sample and the newest
// present now, so a figure always spans whole frames. A second without a frame keeps the previous figures until the
// frame being waited for has taken longer than they say, and then shows that wait ("0 fps 2400.0 ms/frame"), where
// VIctory's caption would stand still. No Windows headers: frame_meter_test drives it with made-up times.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace leonida {

class FrameMeter {
public:
    // A frame was presented at `now_ns` (one monotonic clock, nanoseconds). Any thread; lock-free.
    void presented(std::uint64_t now_ns) {
        std::uint64_t none = 0;
        first_ns_.compare_exchange_strong(none, now_ns, std::memory_order_relaxed);
        last_ns_.store(now_ns, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_release);
    }
    struct Reading {
        std::uint64_t count = 0;     // frames presented so far
        std::uint64_t first_ns = 0;  // when the first was
        std::uint64_t last_ns = 0;   // when the newest was
    };
    Reading read() const {
        Reading r;
        r.count = count_.load(std::memory_order_acquire);
        r.first_ns = first_ns_.load(std::memory_order_relaxed);
        r.last_ns = last_ns_.load(std::memory_order_relaxed);
        return r;
    }

private:
    std::atomic<std::uint64_t> count_{0};
    std::atomic<std::uint64_t> first_ns_{0};
    std::atomic<std::uint64_t> last_ns_{0};
};

struct FrameFigures {
    double fps = 0.0;
    double ms = 0.0;  // per frame
};

// One sampling thread's view of a FrameMeter (see the top comment).
class FrameRateSampler {
public:
    // False while there is no figure yet: no frame, or one frame that has not been waited on for a second.
    bool sample(const FrameMeter::Reading& r, std::uint64_t now_ns, FrameFigures* out) {
        if (r.count == 0) return false;
        if (count_ == 0) {  // the first sample that sees a frame: measure from the first one
            count_ = 1;
            last_ns_ = r.first_ns;
        }
        if (r.count > count_ && r.last_ns > last_ns_) {
            const double frames = double(r.count - count_);
            const double span_ns = double(r.last_ns - last_ns_);
            figures_ = {frames * 1e9 / span_ns, span_ns / 1e6 / frames};
            have_ = true;
            count_ = r.count;
            last_ns_ = r.last_ns;
        } else if (now_ns > r.last_ns) {
            // No new frame: the one being waited for, once it has taken longer than the last figures say.
            const double waited_ms = double(now_ns - r.last_ns) / 1e6;
            if ((have_ && waited_ms > figures_.ms) || (!have_ && waited_ms >= 1000.0)) {
                figures_ = {1000.0 / waited_ms, waited_ms};
                have_ = true;
            }
        }
        if (have_ && out) *out = figures_;
        return have_;
    }

private:
    std::uint64_t count_ = 0;
    std::uint64_t last_ns_ = 0;
    FrameFigures figures_{};
    bool have_ = false;
};

// "Leonida | 60 fps 16.7 ms/frame" (V host.rs: `{fps:.0} fps {ms:.1} ms/frame`).
inline std::string caption_text(const FrameFigures& f) {
    char text[96];
    std::snprintf(text, sizeof text, "Leonida | %.0f fps %.1f ms/frame", f.fps, f.ms);
    return text;
}

}  // namespace leonida
