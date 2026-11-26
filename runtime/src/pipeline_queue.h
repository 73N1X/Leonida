// Pipelines made by worker threads instead of the thread that creates or first binds them, a per-frame budget for
// waiting on them at the bind, and the list of pipelines earlier runs bound, started early.
//
// Ported from VIctory (victory/crates/victory-gpu/src/pipeline/make.rs and pipeline/library.rs; GPL-3.0-or-later,
// Marvisak and contributors, DexNom fork). The title creates some 60,000 pipelines in its first 20 s and draws with a
// few dozen of them (pipeline.rs:6-11); its first world frame wants over 1,200 new ones, and made on the recording
// threads that frame took more than 30 s, past the title's GPU watchdog (make.rs:1-8, 181-185). So, as there:
//   - A pipeline state records what its pipeline is made from and returns; a Job makes the pipeline on a worker.
//   - Creation starts a pipeline an earlier run bound (EarlyList, library.rs:10-13 and make.rs `early`); any other
//     pipeline is started by its first bind (make.rs `ready`). Requests go first, then early depth-only pipelines,
//     then the other early ones (make.rs Queue).
//   - A bind waits for its pipeline while the frame's budget lasts (make.rs `wait`): per frame and kind (graphics,
//     compute), a window of waiting time the binding threads share (FrameWait), never for a pipeline the driver has
//     held for long (held_too_long). A draw or dispatch whose pipeline is not made then is dropped and counted
//     (pipeline.rs `note_drop`), and the counts are logged once a second.
// Not ported: VIctory's loading allowance (make.rs:193-223, waits of up to 4 s / 12 s per frame while a world loads)
// and its 12 s default; the budget here is VIctory's earlier 500 ms (LEONIDA_PIPELINE_WAIT_MS).
// Leonida's own rule: on a device of no title (the PC interface, the tests) a bind waits for its pipeline however long
// it takes until the console frame clock begins a frame (device slot 80, frame.rs:120-184), so nothing is dropped
// there: D3D12 has a PSO usable once it is created. The title's device is bounded from its creation (title_running).
//
// The worker threads are the process's and are never stopped; the builder is never destroyed (a static destructor
// at exit would run under them). Nothing here is thread_local (AGENTS.md, "Runtime traps").
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace leonida::pipelines {

using Clock = std::chrono::steady_clock;

// ---- Frames ---------------------------------------------------------------------------------------------------

// Frames the console frame clock has begun (origin waits that hand out a token, frame.rs:163-171). 0: none yet.
inline std::atomic<std::uint64_t>& frame_counter() {
    static std::atomic<std::uint64_t> frames{0};
    return frames;
}
inline std::uint64_t current_frame() { return frame_counter().load(std::memory_order_relaxed); }
inline std::uint64_t begin_frame() { return frame_counter().fetch_add(1, std::memory_order_relaxed) + 1; }

// A title is running: D3D12XboxCreateDevice made its device in the console layout. From then on a bind's wait is
// bounded even before the frame clock's first frame, which is VIctory's frame 0 (make.rs `wait` with frame::current()
// 0): the title creates and uses pipelines while it sets up, and a 30 s watchdog of its own ends a main thread that
// stops for that long (the first CreateComputePipelineState stalled ~28 s on the synchronous compile).
inline std::atomic<bool>& title_running() {
    static std::atomic<bool> running{false};
    return running;
}
// Whether binds wait within a frame's budget (else as long as the build takes: the PC interface, the tests).
inline bool waits_bounded() { return title_running().load(std::memory_order_relaxed) || current_frame() > 0; }

// ---- Settings -------------------------------------------------------------------------------------------------

// How the threads that wait in one frame share its budget (make.rs:290-332, VICTORY_PIPELINE_WAIT_SHARED; here
// LEONIDA_PIPELINE_WAIT_SHARED): a window of waiting time by default, `reserve` or `0` as VIctory had them before.
enum class Sharing { window, reserve, per_thread };

inline Sharing sharing_from(const char* value) {
    if (!value) return Sharing::window;
    std::string v(value);
    const auto first = v.find_first_not_of(" \t");
    v = first == std::string::npos ? std::string() : v.substr(first, v.find_last_not_of(" \t") - first + 1);
    if (v == "0") return Sharing::per_thread;
    if (v == "reserve") return Sharing::reserve;
    return Sharing::window;
}

inline constexpr std::int64_t kDefaultWaitMs = 500;

inline long long env_number(const char* name, long long fallback) {
    char text[32] = {};
#ifdef _WIN32
    std::size_t length = 0;
    if (getenv_s(&length, text, sizeof(text), name) != 0 || length <= 1) return fallback;
#else
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    std::snprintf(text, sizeof(text), "%s", value);
#endif
    char* end = nullptr;
    const long long n = std::strtoll(text, &end, 10);
    return end == text || n < 0 ? fallback : n;
}

// LEONIDA_PIPELINE_WAIT_MS: the waiting a frame allows per kind of pipeline, over all threads (make.rs:154-179).
// 0 waits for nothing once frames have begun. Read once; set_wait_budget overrides it (tests).
inline std::atomic<std::int64_t>& wait_budget_ms() {
    static std::atomic<std::int64_t> ms{env_number("LEONIDA_PIPELINE_WAIT_MS", kDefaultWaitMs)};
    return ms;
}
inline Clock::duration wait_budget() { return std::chrono::milliseconds(wait_budget_ms().load(std::memory_order_relaxed)); }
inline void set_wait_budget(std::chrono::milliseconds budget) { wait_budget_ms().store(budget.count()); }

inline Sharing shared_budget() {
#ifdef _WIN32
    char text[16] = {};
    std::size_t length = 0;
    static const Sharing sharing =
        getenv_s(&length, text, sizeof(text), "LEONIDA_PIPELINE_WAIT_SHARED") == 0 && length > 1 ? sharing_from(text)
                                                                                                    : Sharing::window;
#else
    static const Sharing sharing = sharing_from(std::getenv("LEONIDA_PIPELINE_WAIT_SHARED"));
#endif
    return sharing;
}

// LEONIDA_PIPELINE_WORKERS: build threads (make.rs:528-561: half the logical processors, 2 to 8). 0 builds each
// pipeline on the thread that starts it, as before.
inline unsigned worker_count() {
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const long long fallback = std::clamp(hardware / 2, 2u, 8u);
    return unsigned(std::min<long long>(env_number("LEONIDA_PIPELINE_WORKERS", fallback), 64));
}

// ---- One frame's waiting (make.rs:334-394) ---------------------------------------------------------------------

struct FrameWait {
    std::uint64_t frame = UINT64_MAX;
    // The waiting counted against the budget so far (the window: time in which some thread waited, up to `since`).
    Clock::duration spent{};
    // The window: threads waiting now, and since when one has been.
    std::uint32_t waiting = 0;
    bool open = false;
    Clock::time_point since{};

    // A thread is to wait in `frame` at `now`: how long it may.
    Clock::duration begin(Sharing sharing, std::uint64_t at_frame, Clock::duration budget, Clock::time_point now) {
        if (frame != at_frame) *this = FrameWait{at_frame};
        if (sharing == Sharing::window) {
            const Clock::duration opened = open ? now - since : Clock::duration::zero();
            const Clock::duration left = std::max(Clock::duration::zero(), budget - (spent + opened));
            if (left > Clock::duration::zero()) {
                if (waiting == 0) {
                    since = now;
                    open = true;
                }
                ++waiting;
            }
            return left;
        }
        const Clock::duration left = std::max(Clock::duration::zero(), budget - spent);
        if (sharing == Sharing::reserve) spent += left;
        return left;
    }

    // The thread `begin` gave `left` (more than nothing) in `at_frame` has waited `waited` and stops at `now`. A frame
    // that began meanwhile has its own budget.
    void end(Sharing sharing, std::uint64_t at_frame, Clock::duration left, Clock::duration waited,
             Clock::time_point now) {
        if (frame != at_frame) return;
        switch (sharing) {
        case Sharing::window:
            if (waiting) --waiting;
            if (waiting == 0 && open) {
                spent += now - since;
                open = false;
            }
            break;
        case Sharing::reserve: spent = std::max(Clock::duration::zero(), spent - (left - waited)); break;
        case Sharing::per_thread: spent += waited; break;
        }
    }
};

// Whether the driver has held a pipeline longer than most take, so no frame waits for it (make.rs:225-234): 10 s for
// a compute pipeline, 3 s for others, while the frame budget is shorter than that.
inline bool held_too_long(Clock::duration held, bool compute, Clock::duration base) {
    const Clock::duration most = std::chrono::seconds(compute ? 10 : 3);
    return base < most && held > most;
}

// ---- A pipeline to make ----------------------------------------------------------------------------------------

inline constexpr std::uint8_t kNotQueued = 0, kEarly = 1, kRequested = 2, kMaking = 3;

class Builder;

// What a pipeline state's Vulkan pipeline is made from (a function the state sets once), and whether it is made.
class Job {
public:
    Job(bool compute, bool depth_only) : compute_(compute), depth_only_(depth_only) {}
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;

    // The pipeline is made: what the draws and dispatches recorded with it check.
    bool ready() const { return made_.load(std::memory_order_acquire); }
    // The build ran, and made the pipeline or did not (refused).
    bool finished() const { return finished_.load(std::memory_order_acquire); }
    bool compute() const { return compute_; }
    bool depth_only() const { return depth_only_; }
    std::uint8_t queue_state() const { return queue_.load(std::memory_order_relaxed); }

    // Set once, before the job is started. `work` returns whether it made the pipeline.
    void set_work(std::function<bool()> work) { work_ = std::move(work); }

    // The pipeline state is going away: a queued build is not run, and a running one is waited for.
    void retire() {
        std::unique_lock lock(mutex_);
        retired_ = true;
        done_.wait(lock, [&] { return !running_; });
        work_ = nullptr;
    }

    // Makes it on this thread unless a worker has it, then waits for that worker (make.rs `now`); whether it is made.
    // The builder's counts leave such a build out.
    bool run_now() {
        run();
        wait_finished();
        return ready();
    }

    // Waits however long the build takes (a retired job that never ran ends the wait too).
    void wait_finished() {
        std::unique_lock lock(mutex_);
        done_.wait(lock, [&] { return finished_.load(std::memory_order_relaxed) || (retired_ && !running_); });
    }
    bool wait_finished_for(Clock::duration limit) {
        std::unique_lock lock(mutex_);
        return done_.wait_for(lock, limit, [&] { return finished_.load(std::memory_order_relaxed); });
    }

    // How long a worker has been at it; zero when none has begun.
    Clock::duration held() const {
        const std::int64_t begun = begun_.load(std::memory_order_relaxed);
        if (!begun || finished()) return Clock::duration::zero();
        return Clock::now().time_since_epoch() - Clock::duration(begun);
    }

    // How long the build took (zero before it finished).
    Clock::duration took() const { return Clock::duration(took_.load(std::memory_order_relaxed)); }

private:
    friend class Builder;

    // Runs the build unless it ran or the job was retired; whether it ran here.
    bool run() {
        {
            std::lock_guard lock(mutex_);
            if (retired_ || claimed_) return false;
            claimed_ = true;
            running_ = true;
        }
        const Clock::time_point begun = Clock::now();
        begun_.store(begun.time_since_epoch().count(), std::memory_order_relaxed);
        bool made = false;
        try {
            made = work_ && work_();
        } catch (...) {
            made = false;  // nothing may leave a worker
        }
        took_.store((Clock::now() - begun).count(), std::memory_order_relaxed);
        std::lock_guard lock(mutex_);
        running_ = false;
        made_.store(made, std::memory_order_release);
        finished_.store(true, std::memory_order_release);
        done_.notify_all();
        return true;
    }

    const bool compute_;
    const bool depth_only_;
    std::atomic<std::uint8_t> queue_{kNotQueued};
    std::atomic<bool> made_{false};
    std::atomic<bool> finished_{false};
    std::atomic<Clock::rep> begun_{0};
    std::atomic<Clock::rep> took_{0};
    std::mutex mutex_;
    std::condition_variable done_;
    bool retired_ = false;
    bool claimed_ = false;
    bool running_ = false;
    std::function<bool()> work_;
};

// ---- Drops (pipeline.rs:461-564) ---------------------------------------------------------------------------------

struct DropCounts {
    std::uint64_t draws = 0;       // a draw whose pipeline was still being made
    std::uint64_t dispatches = 0;  // a dispatch whose pipeline was still being made
    std::uint64_t refused = 0;     // either, whose pipeline could not be made
};

class Drops {
public:
    static Drops& get() {
        static Drops* drops = new Drops();  // never destroyed: see the file's notes
        return *drops;
    }

    // A draw or dispatch recorded with `job` (not ready) is dropped. Logged once a second, with what was dropped
    // since the last line.
    void note(const Job& job, bool dispatch, std::uint64_t waiting) {
        if (job.finished()) refused_.fetch_add(1, std::memory_order_relaxed);
        else (dispatch ? dispatches_ : draws_).fetch_add(1, std::memory_order_relaxed);
        const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     Clock::now().time_since_epoch()).count();
        std::int64_t last = last_ms_.load(std::memory_order_relaxed);
        if (now - last < 1000 || !last_ms_.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
        const DropCounts total = counts();
        std::lock_guard lock(log_mutex_);
        std::fprintf(stderr,
                     "[rt] d3d12: pipelines: %llu draws and %llu dispatches dropped while their pipelines are made, "
                     "%llu whose pipelines could not be made (since the last line; %llu pipelines waiting to be "
                     "made)\n",
                     static_cast<unsigned long long>(total.draws - logged_.draws),
                     static_cast<unsigned long long>(total.dispatches - logged_.dispatches),
                     static_cast<unsigned long long>(total.refused - logged_.refused),
                     static_cast<unsigned long long>(waiting));
        logged_ = total;
    }

    DropCounts counts() const {
        return {draws_.load(std::memory_order_relaxed), dispatches_.load(std::memory_order_relaxed),
                refused_.load(std::memory_order_relaxed)};
    }

private:
    std::atomic<std::uint64_t> draws_{0}, dispatches_{0}, refused_{0};
    std::atomic<std::int64_t> last_ms_{0};
    std::mutex log_mutex_;
    DropCounts logged_;
};

// ---- Pipelines earlier runs bound (library.rs:10-13, 431-438, 567-714) -------------------------------------------

// Keys (known at creation: a hash of the description) of the pipelines this and earlier runs bound, one per line in
// `early-pipelines.txt` beside the pipeline cache. A run starts those at creation. The file keeps what earlier runs
// listed (library.rs:613-616), at most kMax keys; it is written whole to a temporary file and renamed over the list,
// so a reader never sees a part (library.rs:676-682).
class EarlyList {
public:
    static constexpr std::size_t kMax = 8192;  // library.rs:437
    static constexpr std::chrono::seconds kSaveEvery{20};  // library.rs:571

    static EarlyList& get() {
        static EarlyList* list = new EarlyList();  // never destroyed: see the file's notes
        return *list;
    }

    // Where the list is kept (empty: nowhere). `always`: written whatever runs (a path the user or a test named);
    // otherwise only for a title (waits_bounded), so the PC-interface tests leave no file beside their executables.
    // The first call wins, unless `replace`.
    void configure(const std::filesystem::path& file, bool always, bool replace = false) {
        std::lock_guard lock(mutex_);
        if (configured_ && !replace) return;
        configured_ = true;
        file_ = file;
        always_ = always;
        loaded_ = false;
        keys_.clear();
        changed_ = false;
        last_save_ = Clock::time_point{};
    }

    // Whether a pipeline with `key` was bound in this or an earlier run (library.rs:702-705).
    bool wanted(const std::string& key) {
        std::lock_guard lock(mutex_);
        load_locked();
        return keys_.count(key) != 0;
    }

    // A bind asked for the pipeline with `key` (library.rs:707-714).
    void note_used(const std::string& key) {
        if (key.empty()) return;
        std::lock_guard lock(mutex_);
        load_locked();
        if (keys_.size() < kMax && keys_.insert(key).second) changed_ = true;
    }

    // Writes the list if it changed since the last save and kSaveEvery has passed (or `now`); whether it wrote. One save
    // at a time, start to rename: a call returns after any save under way has put its file in place.
    bool save(bool now) {
        std::lock_guard saving(save_mutex_);
        std::vector<std::string> keys;
        std::filesystem::path file;
        {
            std::lock_guard lock(mutex_);
            if (!changed_ || file_.empty() || (!always_ && !waits_bounded())) return false;
            const Clock::time_point at = Clock::now();
            if (!now && last_save_ != Clock::time_point{} && at - last_save_ < kSaveEvery) return false;
            last_save_ = at;
            changed_ = false;
            keys.assign(keys_.begin(), keys_.end());
            file = file_;
        }
        std::sort(keys.begin(), keys.end());
        std::string text;
        for (const std::string& key : keys) text += key + "\n";
        std::error_code ec;
        std::filesystem::path tmp = file;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(text.data(), std::streamsize(text.size()));
            if (!out) {
                std::fprintf(stderr, "[rt] d3d12: pipelines: the early list could not be written\n");
                std::lock_guard lock(mutex_);
                changed_ = true;
                return false;
            }
        }
        std::filesystem::rename(tmp, file, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            std::lock_guard lock(mutex_);
            changed_ = true;
            return false;
        }
        saves_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    std::size_t size() {
        std::lock_guard lock(mutex_);
        load_locked();
        return keys_.size();
    }
    std::uint64_t saves() const { return saves_.load(std::memory_order_relaxed); }

    // Reads the file again, as the next run would (tests).
    void reload() {
        std::lock_guard lock(mutex_);
        loaded_ = false;
        keys_.clear();
        changed_ = false;
        load_locked();
    }

private:
    void load_locked() {
        if (loaded_) return;
        loaded_ = true;
        if (file_.empty()) return;
        std::ifstream in(file_, std::ios::binary);
        std::string line;
        while (in && std::getline(in, line) && keys_.size() < kMax) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) keys_.insert(line);
        }
        if (!keys_.empty())
            std::fprintf(stderr, "[rt] d3d12: pipelines: %zu pipelines earlier runs bound are started at creation\n",
                         keys_.size());
    }

    std::mutex mutex_;
    std::mutex save_mutex_;  // held through a whole save; taken before mutex_
    std::filesystem::path file_;
    bool configured_ = false;
    bool always_ = false;
    bool loaded_ = false;
    bool changed_ = false;
    Clock::time_point last_save_{};
    std::unordered_set<std::string> keys_;
    std::atomic<std::uint64_t> saves_{0};
};

// ---- The builder (make.rs:440-580) ---------------------------------------------------------------------------------

struct BuilderStats {
    std::uint64_t made = 0;           // pipelines made
    std::uint64_t refused = 0;        // builds that made none
    std::uint64_t started_early = 0;  // started at creation from the early list
    std::uint64_t waiting = 0;        // queued and not made yet
    std::uint64_t waited_us = 0;      // waiting at binds in all
};

class Builder {
public:
    static Builder& get() {
        static Builder* builder = new Builder();  // never destroyed: see the file's notes
        return *builder;
    }

    // Starts `job` (kEarly at creation, kRequested at a bind), or promotes one queued early (make.rs `start`).
    void start(const std::shared_ptr<Job>& job, std::uint8_t priority) {
        if (job->queue_.load(std::memory_order_relaxed) >= priority) return;
        if (!workers()) {
            if (job->queue_.exchange(kMaking, std::memory_order_relaxed) == kNotQueued && job->run()) count(*job);
            return;
        }
        std::lock_guard lock(mutex_);
        const std::uint8_t previous = job->queue_.load(std::memory_order_relaxed);
        if (previous >= priority) return;
        job->queue_.store(priority, std::memory_order_relaxed);
        if (priority == kRequested) requested_.push_back(job);
        else if (job->depth_only()) early_depth_.push_back(job);  // shadow maps and depth prepasses feed later draws
        else early_.push_back(job);
        if (previous == kNotQueued) {
            if (priority == kEarly) started_early_.fetch_add(1, std::memory_order_relaxed);
            peak_ = std::max(peak_, ++waiting_);
        }
        available_.notify_one();
    }

    // At a bind of a pipeline `start` was given: waits for it while the current frame's budget lasts (make.rs `wait`),
    // or without a limit where waits are not bounded (waits_bounded). Whether it is made.
    bool wait(Job& job) {
        if (job.finished()) return job.ready();
        const std::uint64_t frame = current_frame();
        const Clock::time_point started = Clock::now();
        if (!waits_bounded()) {
            job.wait_finished();
            add_wait(Clock::now() - started);
            return job.ready();
        }
        const Clock::duration base = wait_budget();
        if (base <= Clock::duration::zero() || held_too_long(job.held(), job.compute(), base)) return job.ready();
        const Sharing sharing = shared_budget();
        FrameWait& frame_wait = frames_[job.compute() ? 1 : 0];
        Clock::duration left;
        {
            std::lock_guard lock(frames_mutex_);
            left = frame_wait.begin(sharing, frame, base, started);
        }
        if (left <= Clock::duration::zero()) return job.ready();
        {
            std::unique_lock lock(job.mutex_);
            while (!job.finished_.load(std::memory_order_relaxed) && !job.retired_ &&
                   !held_too_long(job.held(), job.compute(), base)) {
                const Clock::duration rest = left - (Clock::now() - started);
                if (rest <= Clock::duration::zero()) break;
                // Woken now and then to see whether it has become a slow one.
                job.done_.wait_for(lock, std::min<Clock::duration>(rest, std::chrono::milliseconds(250)));
            }
        }
        const Clock::time_point ended = Clock::now();
        const Clock::duration waited = std::min(ended - started, left);
        {
            std::lock_guard lock(frames_mutex_);
            frame_wait.end(sharing, frame, left, waited, ended);
        }
        add_wait(waited);
        return job.ready();
    }

    BuilderStats stats() const {
        std::lock_guard lock(mutex_);
        return {made_.load(std::memory_order_relaxed), refused_.load(std::memory_order_relaxed),
                started_early_.load(std::memory_order_relaxed), waiting_,
                waited_us_.load(std::memory_order_relaxed)};
    }
    std::uint64_t waiting() const {
        std::lock_guard lock(mutex_);
        return waiting_;
    }

    // Tests: while held, workers take no job (queued ones wait), so a test can see draws dropped deterministically.
    void hold(bool held) {
        std::lock_guard lock(mutex_);
        held_ = held;
        if (!held_) available_.notify_all();
    }

private:
    Builder() = default;

    void add_wait(Clock::duration waited) {
        waited_us_.fetch_add(std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(waited).count()),
                             std::memory_order_relaxed);
    }

    void count(const Job& job) {
        (job.ready() ? made_ : refused_).fetch_add(1, std::memory_order_relaxed);
        const auto took = job.took();
        if (took > std::chrono::seconds(10))
            std::fprintf(stderr, "[rt] d3d12: pipelines: one %s pipeline took %.1f s to make\n",
                         job.compute() ? "compute" : "graphics", std::chrono::duration<double>(took).count());
    }

    // Starts the workers on first use; false when none could be started (or LEONIDA_PIPELINE_WORKERS=0).
    bool workers() {
        std::call_once(workers_once_, [this] {
            const unsigned count = worker_count();
            unsigned started = 0;
            for (unsigned i = 0; i < count; ++i) {
                try {
                    std::thread([this, i] { loop(i); }).detach();
                    ++started;
                } catch (const std::exception&) {
                    break;
                }
            }
            running_ = started > 0;
            std::fprintf(stderr,
                         "[rt] d3d12: pipelines: made on %u threads; a bind waits up to %lld ms per frame and kind "
                         "(LEONIDA_PIPELINE_WAIT_MS) once the frame clock runs\n",
                         started, static_cast<long long>(wait_budget_ms().load()));
        });
        return running_;
    }

    void loop(unsigned index) {
        (void)index;
#ifdef _WIN32
        // Below the title's own threads (loading, recording), as make.rs:538-543.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        for (;;) {
            std::shared_ptr<Job> job = next();
            if (job->run()) count(*job);
            job.reset();
            bool drained = false;
            std::uint64_t peak = 0;
            {
                std::lock_guard lock(mutex_);
                if (waiting_) --waiting_;
                if (waiting_ == 0) {
                    drained = true;
                    peak = peak_;
                    peak_ = 0;
                }
            }
            if (drained && peak >= 10) {
                const BuilderStats s = stats();
                std::fprintf(stderr,
                             "[rt] d3d12: pipelines: all made (%llu made, %llu could not be made, %llu started early; "
                             "%.1f s waited for at binds in all)\n",
                             static_cast<unsigned long long>(s.made), static_cast<unsigned long long>(s.refused),
                             static_cast<unsigned long long>(s.started_early), double(s.waited_us) / 1e6);
            }
            EarlyList::get().save(drained);
        }
    }

    std::shared_ptr<Job> next() {
        std::unique_lock lock(mutex_);
        for (;;) {
            if (!held_) {
                if (std::shared_ptr<Job> job = pop()) return job;
            }
            available_.wait(lock);
        }
    }

    std::shared_ptr<Job> pop() {
        std::shared_ptr<Job> job;
        if (!requested_.empty()) {
            job = std::move(requested_.front());
            requested_.pop_front();
        } else {
            // An early entry can have been promoted and claimed already.
            for (auto* queue : {&early_depth_, &early_}) {
                while (!job && !queue->empty()) {
                    std::shared_ptr<Job> front = std::move(queue->front());
                    queue->pop_front();
                    if (front->queue_.load(std::memory_order_relaxed) == kEarly) job = std::move(front);
                }
                if (job) break;
            }
            if (!job) return nullptr;
        }
        job->queue_.store(kMaking, std::memory_order_relaxed);
        return job;
    }

    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<std::shared_ptr<Job>> requested_, early_depth_, early_;
    std::uint64_t waiting_ = 0;
    std::uint64_t peak_ = 0;
    bool held_ = false;
    std::once_flag workers_once_;
    bool running_ = false;
    std::atomic<std::uint64_t> made_{0}, refused_{0}, started_early_{0}, waited_us_{0};
    std::mutex frames_mutex_;
    FrameWait frames_[2];  // graphics, compute
};

}  // namespace leonida::pipelines
