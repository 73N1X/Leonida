// app_state: the title's suspend routines, and how a run ends.
//
// A console title is never told it is closing: quitting it first suspends it (the routines it registered with
// RegisterAppStateChangeNotification run with Quiesced = TRUE, and it has about a second to save), and then the
// process ends. Ported from VIctory victory-hle src/modules/lifecycle.rs (GPL-3.0-or-later):
// RegisterAppStateChangeNotification / UnregisterAppStateChangeNotification keep the title's routines here (the
// host's psm-appnotify set is for packaged apps and never calls them), and end_title(), which the title's window
// calls when it is closed (title_window.cpp), runs them on a thread of their own (titles hand the suspend to their
// window thread and wait for it inside the routine, so not on the caller's) and ends the process when they return or
// after kSuspendDeadlineMs, whichever is first.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

#include "shims.h"

namespace rt::shims::app_state {
namespace {

using Routine = void(CALLBACK*)(BOOLEAN quiesced, void* context);

struct Registration {
    uintptr_t id;
    Routine routine;
    void* context;
};

std::mutex& routines_mutex() {
    static auto* m = new std::mutex;
    return *m;
}
std::vector<Registration>& routines() {
    static auto* v = new std::vector<Registration>;   // immortal: read during process teardown
    return *v;
}
std::atomic<uintptr_t> g_next{1};

constexpr DWORD kSuspendDeadlineMs = 1000;   // how long the console lets suspend routines run

// DWORD RegisterAppStateChangeNotification(PAPPSTATE_CHANGE_ROUTINE routine, PVOID context,
//                                          PAPPSTATE_REGISTRATION* registration)
DWORD WINAPI register_app_state(Routine routine, void* context, void** registration) {
    if (!routine || !registration) return ERROR_INVALID_PARAMETER;
    const uintptr_t id = g_next.fetch_add(1);
    {
        std::lock_guard lock(routines_mutex());
        routines().push_back({id, routine, context});
    }
    *registration = reinterpret_cast<void*>(id);
    return ERROR_SUCCESS;
}

// void UnregisterAppStateChangeNotification(PAPPSTATE_REGISTRATION registration)
void WINAPI unregister_app_state(void* registration) {
    const auto id = reinterpret_cast<uintptr_t>(registration);
    std::lock_guard lock(routines_mutex());
    auto& v = routines();
    for (auto it = v.begin(); it != v.end(); ++it)
        if (it->id == id) {
            v.erase(it);
            break;
        }
}

DWORD WINAPI suspend_thread(void*) {
    std::vector<Registration> copy;
    {
        std::lock_guard lock(routines_mutex());
        copy = routines();
    }
    for (const Registration& r : copy) r.routine(TRUE, r.context);
    return 0;
}

}  // namespace

size_t registered_routines() {
    std::lock_guard lock(routines_mutex());
    return routines().size();
}

bool run_suspend_routines(unsigned deadline_ms) {
    HANDLE thread = CreateThread(nullptr, 0, &suspend_thread, nullptr, 0, nullptr);
    if (!thread) return false;
    const bool finished = WaitForSingleObject(thread, deadline_ms) == WAIT_OBJECT_0;
    CloseHandle(thread);
    return finished;
}

namespace {
DWORD WINAPI end_thread(void* why) {
    const size_t count = registered_routines();
    std::fprintf(stderr, "[rt] app: %s: suspending the title (%zu suspend routine%s)\n", static_cast<const char*>(why),
                 count, count == 1 ? "" : "s");
    const bool finished = run_suspend_routines(kSuspendDeadlineMs);
    std::fprintf(stderr, "[rt] app: %s; the run ends\n",
                 finished ? "the title suspended" : "the title did not finish suspending in time");
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
}  // namespace

void end_title(const char* why) {
    static std::atomic<bool> s_ending{false};
    if (s_ending.exchange(true)) return;
    HANDLE thread = CreateThread(nullptr, 0, &end_thread, const_cast<char*>(why), 0, nullptr);
    if (thread) {
        CloseHandle(thread);
        return;
    }
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), 0);
}

}  // namespace rt::shims::app_state

namespace {
#define APP_STATE(dll)                                                                                     \
    {dll, "RegisterAppStateChangeNotification", reinterpret_cast<void*>(&rt::shims::app_state::register_app_state)}, \
    {dll, "UnregisterAppStateChangeNotification",                                                          \
     reinterpret_cast<void*>(&rt::shims::app_state::unregister_app_state)}
}  // namespace

SHIM_TABLE(app_state,
    APP_STATE("api-ms-win-core-psm-appnotify-l1-1-0.dll"),
    APP_STATE("api-ms-win-core-psm-appnotify-l1-1-1.dll"),
);
