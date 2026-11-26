// src/pipeline_queue.h without a GPU: the frame budget's window (VIctory make.rs loading_tests, ported), the order the
// builder takes pipelines in, a retired job that never runs, drops, and the early list written and read back.
// No game data.
#include "../src/pipeline_queue.h"
#include "test_temp_dir.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "pipeline_queue_test line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace leonida::pipelines;
using namespace std::chrono_literals;

namespace {

// make.rs two_threads_in_one_frame_both_wait_inside_the_window: two threads waiting in one frame share one window, a
// thread that asks once the window is used up gets nothing, and a new frame has a window of its own.
int frame_window() {
    const Sharing sharing = sharing_from(nullptr);
    EXPECT(sharing == Sharing::window);
    const Clock::duration budget = 12s, ms = 1ms;
    const Clock::time_point t0 = Clock::now();
    FrameWait w;
    const Clock::duration a = w.begin(sharing, 7, budget, t0);
    const Clock::duration b = w.begin(sharing, 7, budget, t0 + ms);
    EXPECT(a == budget);
    EXPECT(b == budget - ms);
    w.end(sharing, 7, a, 2s, t0 + 2s);
    w.end(sharing, 7, b, 2s - ms, t0 + 2s);
    EXPECT(w.begin(sharing, 7, budget, t0 + 5s) == 10s);
    w.end(sharing, 7, 10s, 10s, t0 + 15s);
    EXPECT(w.begin(sharing, 7, budget, t0 + 16s) == Clock::duration::zero());
    EXPECT(w.begin(sharing, 8, budget, t0 + 17s) == budget);
    // make.rs the_old_ways_of_counting_stay_behind_the_switch.
    FrameWait r;
    EXPECT(r.begin(Sharing::reserve, 1, budget, t0) == budget);
    EXPECT(r.begin(Sharing::reserve, 1, budget, t0) == Clock::duration::zero());
    FrameWait p;
    EXPECT(p.begin(Sharing::per_thread, 1, budget, t0) == budget);
    EXPECT(p.begin(Sharing::per_thread, 1, budget, t0) == budget);
    p.end(Sharing::per_thread, 1, budget, 3s, t0);
    EXPECT(p.begin(Sharing::per_thread, 1, budget, t0) == 9s);
    EXPECT(sharing_from("0") == Sharing::per_thread && sharing_from(" reserve ") == Sharing::reserve);
    // make.rs a_pipeline_held_too_long_is_not_waited_for.
    const Clock::duration base = 500ms;
    EXPECT(!held_too_long(10s, true, base) && held_too_long(10001ms, true, base));
    EXPECT(!held_too_long(3s, false, base) && held_too_long(3001ms, false, base));
    EXPECT(!held_too_long(60s, true, 12s) && !held_too_long(60s, false, 12s));
    EXPECT(held_too_long(60s, true, 5s) && !held_too_long(60s, false, 5s));
    return 0;
}

std::shared_ptr<Job> job(std::vector<int>& order, std::mutex& mutex, int id, bool depth_only = false) {
    auto j = std::make_shared<Job>(false, depth_only);
    j->set_work([&order, &mutex, id] {
        std::lock_guard lock(mutex);
        order.push_back(id);
        return id >= 0;
    });
    return j;
}

// make.rs draws_promote_early_pipelines_without_claiming_them_twice and early_depth_precedes_other_prewarming: a
// request goes first, then early depth-only pipelines, then the other early ones, and a promoted early job runs once.
int queue_order() {
    Builder& builder = Builder::get();
    builder.hold(true);
    std::vector<int> order;
    std::mutex mutex;
    const auto early = job(order, mutex, 1), promoted = job(order, mutex, 2), depth = job(order, mutex, 3, true);
    const auto requested = job(order, mutex, 4), refused = job(order, mutex, -5), retired = job(order, mutex, 6);
    const BuilderStats before = builder.stats();
    builder.start(early, kEarly);
    builder.start(promoted, kEarly);
    builder.start(depth, kEarly);
    builder.start(retired, kEarly);
    builder.start(requested, kRequested);
    builder.start(promoted, kRequested);  // promoted: queued again as a request, still made once
    builder.start(refused, kRequested);
    EXPECT(early->queue_state() == kEarly && promoted->queue_state() == kRequested);
    retired->retire();  // its pipeline state went away while it was queued: never run
    EXPECT(builder.stats().started_early == before.started_early + 4);
    if (builder.stats().waiting == before.waiting) {
        // LEONIDA_PIPELINE_WORKERS=0: everything ran when started; the order is the starts'.
        builder.hold(false);
        return 0;
    }
    builder.hold(false);
    for (const auto& j : {early, promoted, depth, requested, refused}) EXPECT(j->wait_finished_for(10s));
    EXPECT(!retired->finished());
    std::lock_guard lock(mutex);
    // Workers run in parallel: the order is the order they were taken in only per worker. With one worker it is exact;
    // with more, each job still ran exactly once.
    EXPECT(order.size() == 5);
    for (int id : {1, 2, 3, 4, -5}) EXPECT(std::count(order.begin(), order.end(), id) == 1);
    if (worker_count() == 1) EXPECT((order == std::vector<int>{4, 2, -5, 3, 1}));
    EXPECT(early->ready() && !refused->ready() && refused->finished());
    return 0;
}

// The budget: before the first frame a bind waits however long the build takes; after, at most the frame's window,
// then nothing in that frame, and a new frame has a new window.
int budget() {
    Builder& builder = Builder::get();
    std::vector<int> order;
    std::mutex mutex;
    const auto slow = std::make_shared<Job>(false, false);
    slow->set_work([] {
        std::this_thread::sleep_for(100ms);
        return true;
    });
    EXPECT(current_frame() == 0);
    builder.start(slow, kRequested);
    EXPECT(builder.wait(*slow) && slow->ready());  // no frame yet: as long as it takes

    begin_frame();
    set_wait_budget(60ms);
    builder.hold(true);
    const auto held = job(order, mutex, 7);
    builder.start(held, kRequested);
    Clock::time_point t = Clock::now();
    EXPECT(!builder.wait(*held));
    const auto first = Clock::now() - t;
    t = Clock::now();
    EXPECT(!builder.wait(*held));
    const auto second = Clock::now() - t;
    begin_frame();
    t = Clock::now();
    EXPECT(!builder.wait(*held));
    const auto third = Clock::now() - t;
    std::printf("pipeline_queue_test: waits of %.0f, %.0f and %.0f ms for a held pipeline (budget 60 ms per frame)\n",
                std::chrono::duration<double, std::milli>(first).count(),
                std::chrono::duration<double, std::milli>(second).count(),
                std::chrono::duration<double, std::milli>(third).count());
    EXPECT(first >= 50ms && first < 1s);
    EXPECT(second < 20ms);
    EXPECT(third >= 50ms && third < 1s);
    builder.hold(false);
    EXPECT(held->wait_finished_for(10s) && held->ready());
    EXPECT(builder.wait(*held));

    // Drops are counted by whether the pipeline is still being made.
    const DropCounts before = Drops::get().counts();
    const auto pending = std::make_shared<Job>(true, false);
    Drops::get().note(*pending, true, 1);
    Drops::get().note(*held, false, 0);  // made: counted as refused only when finished without a pipeline
    const DropCounts after = Drops::get().counts();
    EXPECT(after.dispatches == before.dispatches + 1);
    EXPECT(after.refused == before.refused + 1);
    return 0;
}

// The early list: written as sorted lines, the keys of earlier runs kept, read back as the next run reads it.
// LEONIDA_PIPELINE_WAIT_MS stays under the title's 30 s hang watchdog.
int budget_cap() {
    EXPECT(wait_budget_from_env(0) == 0);
    EXPECT(wait_budget_from_env(kDefaultWaitMs) == kDefaultWaitMs);
    EXPECT(wait_budget_from_env(kMaxWaitMs) == kMaxWaitMs);
    EXPECT(wait_budget_from_env(600000) == kMaxWaitMs);
    EXPECT(kMaxWaitMs * 2 < 30000);
    return 0;
}

int early_list() {
    const testfiles::ScratchDir scratch("leonida_pipeline_queue_test");
    const std::filesystem::path file = scratch.path() / "early-pipelines.txt";
    {
        std::ofstream out(file, std::ios::binary);
        out << "from-an-earlier-run\r\n\n";
    }
    EarlyList& list = EarlyList::get();
    list.configure(file, true, true);
    EXPECT(list.wanted("from-an-earlier-run"));
    EXPECT(!list.wanted("b"));
    list.note_used("b");
    list.note_used("a");
    list.note_used("a");
    list.note_used("");
    EXPECT(list.size() == 3);
    const std::uint64_t saves = list.saves();
    EXPECT(list.save(true) && list.saves() == saves + 1);
    EXPECT(!list.save(true));  // nothing changed since
    std::stringstream text;
    {
        std::ifstream in(file, std::ios::binary);  // closed before the next save renames over the file
        text << in.rdbuf();
    }
    EXPECT(text.str() == "a\nb\nfrom-an-earlier-run\n");
    EXPECT(!std::filesystem::exists(file.string() + ".tmp"));
    list.reload();
    EXPECT(list.size() == 3 && list.wanted("a") && list.wanted("b"));
    list.note_used("c");
    EXPECT(!list.save(false));  // within 20 s of the last save
    EXPECT(list.save(true));
    // Without `always`, nothing is written before the first frame; this process has begun frames by now.
    list.configure(scratch.path() / "other.txt", false, true);
    list.note_used("d");
    EXPECT(current_frame() > 0 && list.save(true) && std::filesystem::exists(scratch.path() / "other.txt"));
    list.configure({}, false, true);
    return 0;
}

}  // namespace

int main() {
    if (frame_window() || queue_order() || budget() || budget_cap() || early_list()) return 1;
    std::printf("pipeline_queue_test: ok\n");
    return 0;
}
