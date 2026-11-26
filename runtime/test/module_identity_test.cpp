// GetModuleHandle(NULL) and GetModuleFileName in mapped code name the title's image (shims/library_loader.cpp,
// ported from VIctory victory-hle src/modules/kernel.rs). No game files: a stand-in loader answers for one fake image
// base with a made-up path.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "shims.h"

namespace {

int g_failures = 0;
void check(bool condition, const char* what) {
    std::printf("%s: %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) ++g_failures;
}

// Any address no host module has; never dereferenced.
alignas(64) unsigned char g_fake_image[16];
void* const kImage = g_fake_image;
const wchar_t kPath[] = L"C:\\Games\\Title\\title_master.exe";

void* find_loaded(void*, const std::string&) { return nullptr; }
void* load_bundled(void*, const std::string&, uint32_t, bool& handled) {
    handled = false;
    return nullptr;
}
void* get_proc(void*, void* module, const std::string&, bool& owns) {
    owns = module == kImage;
    return nullptr;
}
bool owns_handle(void*, void* module) { return module == kImage; }
bool module_path(void*, void* module, std::wstring& path) {
    if (module != kImage) return false;
    path = kPath;
    return true;
}
const rt::shims::BundledModuleOps kOps{&find_loaded, &load_bundled, &get_proc, &owns_handle, nullptr, nullptr,
                                       &module_path};

}  // namespace

int main() {
    using HandleW = HMODULE(WINAPI*)(LPCWSTR);
    using HandleA = HMODULE(WINAPI*)(LPCSTR);
    using NameW = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);
    using NameA = DWORD(WINAPI*)(HMODULE, LPSTR, DWORD);
    const auto handle_w = reinterpret_cast<HandleW>(rt::shims::find("kernel32.dll", "GetModuleHandleW"));
    const auto handle_a = reinterpret_cast<HandleA>(rt::shims::find("kernelbase.dll", "GetModuleHandleA"));
    const auto name_w = reinterpret_cast<NameW>(rt::shims::find("kernel32.dll", "GetModuleFileNameW"));
    const auto name_a =
        reinterpret_cast<NameA>(rt::shims::find("api-ms-win-core-libraryloader-l1-2-0.dll", "GetModuleFileNameA"));
    check(handle_w && handle_a && name_w && name_a, "the four imports are served");
    if (!(handle_w && handle_a && name_w && name_a)) return 1;

    // Before the main image is set, everything is the host's.
    check(handle_w(nullptr) == GetModuleHandleW(nullptr), "no main image yet: GetModuleHandleW(NULL) is the host's");
    wchar_t host[MAX_PATH] = {}, got[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, host, MAX_PATH);
    check(name_w(nullptr, got, MAX_PATH) > 0 && std::wcscmp(host, got) == 0,
          "no main image yet: GetModuleFileNameW(NULL) is the host's");

    rt::shims::add_bundled_loader(&g_failures, &kOps);
    rt::shims::set_main_image(kImage);
    check(handle_w(nullptr) == kImage && handle_a(nullptr) == kImage, "GetModuleHandleA/W(NULL) is the title's image");
    check(handle_w(L"kernel32.dll") == GetModuleHandleW(L"kernel32.dll"), "a named host module is still the host's");

    const DWORD length = static_cast<DWORD>(std::wcslen(kPath));
    std::memset(got, 0, sizeof(got));
    check(name_w(nullptr, got, MAX_PATH) == length && std::wcscmp(got, kPath) == 0,
          "GetModuleFileNameW(NULL) is the title's file, its length without the terminator");
    std::memset(got, 0, sizeof(got));
    check(name_w(static_cast<HMODULE>(kImage), got, MAX_PATH) == length && std::wcscmp(got, kPath) == 0,
          "GetModuleFileNameW(title base) is the same file");
    char narrow[MAX_PATH] = {};
    check(name_a(nullptr, narrow, MAX_PATH) == length && std::strcmp(narrow, "C:\\Games\\Title\\title_master.exe") == 0,
          "GetModuleFileNameA(NULL) is the same file in the ANSI code page");
    wchar_t small[8] = {};
    SetLastError(0);
    check(name_w(nullptr, small, 8) == 8 && small[7] == 0 && std::wcsncmp(small, kPath, 7) == 0 &&
              GetLastError() == ERROR_INSUFFICIENT_BUFFER,
          "a short buffer gets the truncated, terminated path, its size and ERROR_INSUFFICIENT_BUFFER");
    const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    GetModuleFileNameW(k32, host, MAX_PATH);
    check(name_w(k32, got, MAX_PATH) > 0 && std::wcscmp(host, got) == 0, "a host module's file is the host's answer");

    rt::shims::set_main_image(nullptr);
    rt::shims::remove_bundled_loader(&g_failures);
    std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
