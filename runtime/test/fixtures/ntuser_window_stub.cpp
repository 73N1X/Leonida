// Link-time stand-in for the API set a GDKX title imports CreateWindowExA from
// (ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll), so tiny_present.exe gets the import the title has. Under
// `gta6rt --run` the shim registry answers it (shims/title_window.cpp) and this DLL never loads; it is built into a
// folder the loader does not search. Test code, not game content.
#include <cstdint>

extern "C" void* __stdcall stub_create_window_ex_a(std::uint32_t, const char*, const char*, std::uint32_t, int, int,
                                                   int, int, void*, void*, void*, void*) {
    return nullptr;
}
