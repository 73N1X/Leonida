// app_state (shims/app_state.cpp): the title's suspend routines are kept and run with Quiesced = TRUE, on a thread
// of their own, within a deadline. end_title() ends the process, so it is not called here. No game files.
#include <windows.h>

#include <atomic>
#include <cstdio>

#include "shims.h"

namespace {

int g_failures = 0;
void check(bool condition, const char* what) {
    std::printf("%s: %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) ++g_failures;
}

std::atomic<int> g_calls{0};
std::atomic<int> g_quiesced{0};
std::atomic<DWORD> g_thread{0};
void CALLBACK routine(BOOLEAN quiesced, void* context) {
    g_calls += *static_cast<int*>(context);
    if (quiesced) ++g_quiesced;
    g_thread = GetCurrentThreadId();
}
void CALLBACK slow_routine(BOOLEAN, void*) { Sleep(2000); }

}  // namespace

int main() {
    using Register = DWORD(WINAPI*)(void*, void*, void**);
    using Unregister = void(WINAPI*)(void*);
    const auto reg = reinterpret_cast<Register>(
        rt::shims::find("api-ms-win-core-psm-appnotify-l1-1-0.dll", "RegisterAppStateChangeNotification"));
    const auto unreg = reinterpret_cast<Unregister>(
        rt::shims::find("api-ms-win-core-psm-appnotify-l1-1-0.dll", "UnregisterAppStateChangeNotification"));
    check(reg && unreg, "RegisterAppStateChangeNotification and its Unregister are served");
    if (!reg || !unreg) return 1;

    int one = 1, ten = 10;
    void* a = nullptr;
    void* b = nullptr;
    check(reg(reinterpret_cast<void*>(&routine), &one, &a) == ERROR_SUCCESS && a &&
              reg(reinterpret_cast<void*>(&routine), &ten, &b) == ERROR_SUCCESS && b && a != b,
          "two routines registered, two registrations");
    check(reg(nullptr, &one, &a) == ERROR_INVALID_PARAMETER && reg(reinterpret_cast<void*>(&routine), &one, nullptr) ==
                                                                   ERROR_INVALID_PARAMETER,
          "a null routine or registration pointer is ERROR_INVALID_PARAMETER");
    check(rt::shims::app_state::registered_routines() == 2, "both are kept");

    check(rt::shims::app_state::run_suspend_routines(5000) && g_calls == 11 && g_quiesced == 2 &&
              g_thread != GetCurrentThreadId(),
          "a suspend runs every routine with Quiesced = TRUE, on another thread");
    unreg(b);
    g_calls = 0;
    check(rt::shims::app_state::registered_routines() == 1 && rt::shims::app_state::run_suspend_routines(5000) &&
              g_calls == 1,
          "an unregistered routine no longer runs");
    unreg(a);

    void* slow = nullptr;
    reg(reinterpret_cast<void*>(&slow_routine), nullptr, &slow);
    check(!rt::shims::app_state::run_suspend_routines(100), "a suspend that outlives the deadline is reported late");
    unreg(slow);

    std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
