// d3d12_xs.dll's data and memory-setup exports, through the shim registry as the loader binds them, with no device
// and no GPU (ported behaviour from VIctory, victory-hle d3d12x.rs and victory-gpu device.rs, GPL-3.0-or-later):
//   - IID_ID3D12Device is a data import: the registry entry is the address of the 16-byte id the title reads;
//   - D3D12ConfigureVirtualMemoryX writes the range graphics addresses fall in (the process's user space) into the
//     second half of the title's 32-byte block, keeps its policy words, and refuses a null block.
// No game data.
#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "d3d12_xs_device.h"
#include "shims.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

}  // namespace

int main() {
    const void* iid = rt::shims::find("d3d12_xs.dll", "IID_ID3D12Device");
    check(iid && std::memcmp(iid, &__uuidof(ID3D12Device), sizeof(GUID)) == 0,
          "IID_ID3D12Device is bound to the device's interface id");

    using ConfigureFn = HRESULT(WINAPI*)(void*);
    const auto configure = reinterpret_cast<ConfigureFn>(rt::shims::find("d3d12_xs.dll", "D3D12ConfigureVirtualMemoryX"));
    check(configure != nullptr, "D3D12ConfigureVirtualMemoryX is a shim");
    if (!configure) return 1;
    rt::d3d12_xs::VirtualMemoryConfig config{};
    config.page_table_pages = 7;  // the title zeroes the block and sets +4
    check(configure(&config) == S_OK, "the title's configuration is accepted");
    check(config.page_table_pages == 7 && config.flags == 0 && config.preserved_bytes == 0,
          "the policy words are left as the title set them");
    check(config.lowest > 0 && config.lowest < config.highest, "a range is written");
    int local = 0;
    const auto stack = reinterpret_cast<std::uint64_t>(&local);
    const auto heap = reinterpret_cast<std::uint64_t>(GetProcessHeap());
    check(config.lowest <= stack && stack <= config.highest && config.lowest <= heap && heap <= config.highest,
          "the range holds the process's own memory");
    std::uint64_t lowest = 0, highest = 0;
    rt::d3d12_xs::user_address_range(lowest, highest);
    check(lowest == config.lowest && highest == config.highest, "the range is the user address space");
    check(configure(nullptr) == E_INVALIDARG, "a null block is refused");

    std::printf("d3d12_xs_exports_test: %s\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
