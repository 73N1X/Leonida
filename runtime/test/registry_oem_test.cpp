// registry_oem.cpp: the console model the title reads from HKLM\...\OEM\ConsoleMode, through the shim table as the
// loader binds it. CPU only; the real registry is only read for a key that is not ours.
#include "shims.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

template <class F>
F shim(const char* name) {
    return reinterpret_cast<F>(rt::shims::find("advapi32.dll", name));
}

}  // namespace

int main() {
    auto open = shim<decltype(&RegOpenKeyW)>("RegOpenKeyW");
    auto open_ex = shim<decltype(&RegOpenKeyExW)>("RegOpenKeyExW");
    auto query = shim<decltype(&RegQueryValueExW)>("RegQueryValueExW");
    auto close = shim<decltype(&RegCloseKey)>("RegCloseKey");
    check(open && open_ex && query && close, "all four registry shims are registered");
    if (failures) return 1;

    const wchar_t* path = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\OEM";
    HKEY key = nullptr;
    check(open(HKEY_LOCAL_MACHINE, path, &key) == ERROR_SUCCESS && key, "the OEM key opens");

    DWORD kind = 0, size = sizeof(DWORD), mode = 0;
    check(query(key, L"ConsoleMode", nullptr, &kind, reinterpret_cast<BYTE*>(&mode), &size) == ERROR_SUCCESS,
          "ConsoleMode reads");
    check(kind == REG_DWORD && size == 4 && mode == 0x64, "Series X is 0x64, a DWORD");

    size = 0;
    check(query(key, L"ConsoleMode", nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS && size == 4,
          "a size query gives 4 bytes");
    size = 2;
    check(query(key, L"ConsoleMode", nullptr, nullptr, reinterpret_cast<BYTE*>(&mode), &size) == ERROR_MORE_DATA &&
              size == 4,
          "a short buffer gets ERROR_MORE_DATA and the size");
    check(query(key, L"Other", nullptr, nullptr, nullptr, &size) == ERROR_FILE_NOT_FOUND, "other values are absent");

    HKEY sub = nullptr;
    check(open(key, L"Sub", &sub) == ERROR_FILE_NOT_FOUND, "the OEM key has no subkeys");

    HKEY key_ex = nullptr;
    check(open_ex(HKEY_LOCAL_MACHINE, L"software\\microsoft\\windows nt\\currentversion\\oem", 0, KEY_READ,
                  &key_ex) == ERROR_SUCCESS &&
              key_ex == key,
          "RegOpenKeyExW opens it too, ignoring case");
    check(close(key) == ERROR_SUCCESS, "the OEM key closes");

    _putenv_s("LEONIDA_DEVICE_TYPE", "6");
    size = sizeof(DWORD);
    check(query(key, L"ConsoleMode", nullptr, nullptr, reinterpret_cast<BYTE*>(&mode), &size) == ERROR_SUCCESS &&
              mode == 0x60,
          "LEONIDA_DEVICE_TYPE=6 reads as Series S (0x60)");
    _putenv_s("LEONIDA_DEVICE_TYPE", "");

    // A key that is not ours reaches the host registry.
    HKEY host = nullptr;
    check(open(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion", &host) == ERROR_SUCCESS &&
              host && host != key,
          "other keys open on the host");
    if (host) check(close(host) == ERROR_SUCCESS, "a host key closes on the host");

    check(!rt::shims::owns_dll("advapi32.dll"), "advapi32.dll stays a host module for LoadLibrary");

    if (failures) return 1;
    std::printf("registry_oem: all checks passed\n");
    return 0;
}
