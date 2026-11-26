// The console model, as the title reads it from the registry.
//
// Ported from VIctory's crates/victory-hle/src/modules/xsystem.rs (its registry part; GPL-3.0-or-later, Marvisak and
// contributors, DexNom fork). Leonida is GPL-3.0-or-later.
//
// The title does not take its console model from XSystemGetDeviceType alone: its own code opens
// HKLM\Software\Microsoft\Windows NT\CurrentVersion\OEM and maps the DWORD "ConsoleMode" to a device type (VIctory
// measured 0x64 for Series X and 0x60 for Series S). A PC has no such value, so the title fell into its Series S
// path and chose the Series S render-target pool budget: with the Series X package's two 3840x2160 back buffers its
// own pool check (game_scarlett_master.exe+0x247af2d, about 2.7 s in) then stopped the title at a breakpoint.
//
// The title imports only RegOpenKeyW, RegQueryValueExW and RegCloseKey from advapi32.dll (RegOpenKeyExW is answered
// too, as VIctory does). Our key is a pseudo handle that no other call can reach; every other key and value goes to
// the host. advapi32.dll stays a host module for LoadLibrary (owns_dll in registry.cpp).
#include "shims.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace {

// Stands for the OEM key; never a host handle value.
char g_oem_marker;
HKEY oem_key() { return reinterpret_cast<HKEY>(&g_oem_marker); }

constexpr const wchar_t* kOemKey = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\OEM";
constexpr const wchar_t* kConsoleMode = L"ConsoleMode";

bool same(const wchar_t* text, const wchar_t* expected) { return text && _wcsicmp(text, expected) == 0; }

// What the title's code maps to each console model (VIctory xsystem.rs console_mode): Series X 0x64, Series S 0x60.
// LEONIDA_DEVICE_TYPE=6 (XboxScarlettLockhart) selects Series S here too, as it does for XSystemGetDeviceType.
DWORD console_mode() {
    char s[16];
    const DWORD n = GetEnvironmentVariableA("LEONIDA_DEVICE_TYPE", s, sizeof(s));
    if (n > 0 && n < sizeof(s) && std::atoi(s) == 6) return 0x60;
    return 0x64;
}

FARPROC host_proc(const char* name) {
    static const HMODULE advapi = LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return advapi ? GetProcAddress(advapi, name) : nullptr;
}

// Our key for its path (the key has no subkeys); nullopt-like -1: not ours, ask the host.
LONG open_oem(HKEY root, const wchar_t* sub, HKEY* out) {
    if (root == oem_key()) return ERROR_FILE_NOT_FOUND;
    if (root == HKEY_LOCAL_MACHINE && same(sub, kOemKey) && out) {
        *out = oem_key();
        return ERROR_SUCCESS;
    }
    return -1;
}

LONG WINAPI reg_open_key_w(HKEY root, const wchar_t* sub, HKEY* out) {
    if (const LONG r = open_oem(root, sub, out); r != -1) return r;
    auto host = reinterpret_cast<decltype(&RegOpenKeyW)>(host_proc("RegOpenKeyW"));
    return host ? host(root, sub, out) : ERROR_CALL_NOT_IMPLEMENTED;
}

LONG WINAPI reg_open_key_ex_w(HKEY root, const wchar_t* sub, DWORD options, REGSAM access, HKEY* out) {
    if (const LONG r = open_oem(root, sub, out); r != -1) return r;
    auto host = reinterpret_cast<decltype(&RegOpenKeyExW)>(host_proc("RegOpenKeyExW"));
    return host ? host(root, sub, options, access, out) : ERROR_CALL_NOT_IMPLEMENTED;
}

LONG WINAPI reg_query_value_ex_w(HKEY key, const wchar_t* name, DWORD* reserved, DWORD* kind, BYTE* data,
                                 DWORD* size) {
    if (key != oem_key()) {
        auto host = reinterpret_cast<decltype(&RegQueryValueExW)>(host_proc("RegQueryValueExW"));
        return host ? host(key, name, reserved, kind, data, size) : ERROR_CALL_NOT_IMPLEMENTED;
    }
    if (!same(name, kConsoleMode)) return ERROR_FILE_NOT_FOUND;
    if (data && !size) return ERROR_INVALID_PARAMETER;
    if (kind) *kind = REG_DWORD;
    if (!size) return ERROR_SUCCESS;
    const DWORD room = *size;
    *size = sizeof(DWORD);
    if (!data) return ERROR_SUCCESS;
    if (room < sizeof(DWORD)) return ERROR_MORE_DATA;
    const DWORD mode = console_mode();
    std::memcpy(data, &mode, sizeof(mode));
    return ERROR_SUCCESS;
}

LONG WINAPI reg_close_key(HKEY key) {
    if (key == oem_key()) return ERROR_SUCCESS;
    auto host = reinterpret_cast<decltype(&RegCloseKey)>(host_proc("RegCloseKey"));
    return host ? host(key) : ERROR_CALL_NOT_IMPLEMENTED;
}

}  // namespace

SHIM_TABLE(registry_oem,
    {"advapi32.dll", "RegOpenKeyW", reinterpret_cast<void*>(&reg_open_key_w)},
    {"advapi32.dll", "RegOpenKeyExW", reinterpret_cast<void*>(&reg_open_key_ex_w)},
    {"advapi32.dll", "RegQueryValueExW", reinterpret_cast<void*>(&reg_query_value_ex_w)},
    {"advapi32.dll", "RegCloseKey", reinterpret_cast<void*>(&reg_close_key)},
);
