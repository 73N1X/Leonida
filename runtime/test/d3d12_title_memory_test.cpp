// Title memory as GPU memory (src/title_memory.h): a GPU address is the title's CPU address, on the RTX 4060 Ti's
// capture-replay device addresses. Through the Xbox-ABI proxy over the standalone Vulkan device, with xmem connected
// to the layer as gta6rt connects it (rt::xmem::set_graphics_listener):
//   - heap_base is title_memory::kHeapBase, xmem places graphics memory in [4 GiB, 512 GiB) and 32-bit graphics
//     memory in [1 GiB, 4 GiB), and both are imported at heap_base + their addresses;
//   - device slot 70 writes a V# whose base is the title address into a console descriptor heap that lies in the
//     title's 32-bit memory, and a compute shader written to the translator's contract (test/glsl/cs_user_data.comp)
//     reads the title's bytes through it and writes them into title memory through a root UAV at a title address,
//     which the CPU reads back with no copy;
//   - device slot 70 makes a texture from title memory, detiled from SW_64KB_D, whose image reads back as the
//     linear texels;
//   - console list slots 62 (CopyMemoryToMemoryX), 71/72 (Write32/Write64), 73 (WaitValue32, recorded) and 148
//     (AllocateUploadMemory) on title addresses;
//   - device slot 133 on mapped xmem physical pages: the buffer is bound to their import, at heap_base + its address;
//   - the title's VirtualFree takes the imports off the GPU first;
//   - with the title's allocator (its __XMemAllocationHooks__ through xmem, rt::d3d12::set_title_allocator), a slot-127
//     texture and committed buffers take their memory from it with VIctory's attributes, zeroed; the buffers sit on
//     that title memory at heap_base + the address (a GPU copy lands in the title's pages), the texture reports the
//     address, and each goes back through the title's free routine when released.
// 77 when the GPU has no bufferDeviceAddressCaptureReplay or VK_EXT_external_memory_host (title memory off). No game
// data. Set LEONIDA_VK_VALIDATION=1 for the validation layer.
#include "shader_fixture_path.h"
#include "test_temp_dir.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../shims/shims.h"
#include "../shims/xmem.h"
#include "../src/gfx10_descriptors.h"
#include "../src/gfx10_tiling.h"
#include "../src/shader_library.h"
#include "../src/title_memory.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

#ifndef LEONIDA_TEST_CS_SPV
#error "LEONIDA_TEST_CS_SPV must name the compiled test shader"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "title memory line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

namespace tm = leonida::title_memory;

template<class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T* operator->() const { return p; }
};

using SerializeFn = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
using XMemAllocFn = void*(__stdcall*)(std::size_t, std::uint64_t);
using XMemFreeFn = void(__stdcall*)(void*, std::uint64_t);
using XMemVirtualAllocFn = void*(WINAPI*)(void*, SIZE_T, DWORD, ULONGLONG, DWORD);
using XMemAllocatePhysicalPagesFn = BOOL(WINAPI*)(std::uint64_t, std::uint64_t*, std::uint64_t*);
using XMemFreePhysicalPagesFn = BOOL(WINAPI*)(std::uint64_t, const std::uint64_t*);
using XMemMapPhysicalPagesFn = void*(WINAPI*)(void*, std::uint64_t, const std::uint64_t*);
using VirtualFreeFn = BOOL(WINAPI*)(void*, SIZE_T, DWORD);
using Slot = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                    std::uint64_t);

// ID3D12Device11, which gives the proxy the console layouts (d3d12_xs_slots.h).
constexpr GUID kDevice11{0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};
constexpr DWORD kXmem64kPages = 0x20400000;
constexpr std::uint64_t kGraphics = 0x183, kGraphics32 = 0x7;  // the retail title's attribute words

std::uint64_t call(void* object, unsigned slot, std::uint64_t a1 = 0, std::uint64_t a2 = 0, std::uint64_t a3 = 0,
                   std::uint64_t a4 = 0, std::uint64_t a5 = 0, std::uint64_t a6 = 0) {
    return reinterpret_cast<Slot>((*static_cast<void***>(object))[slot])(object, a1, a2, a3, a4, a5, a6);
}
template<class T>
std::uint64_t word(T* p) { return reinterpret_cast<std::uint64_t>(p); }
HRESULT hr_from(std::uint64_t r) { return static_cast<HRESULT>(static_cast<std::uint32_t>(r)); }

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(v >> (8 * i)));
}

std::vector<std::uint8_t> zlib_stored(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out = {0x78, 0x01, 0x01};
    const auto length = std::uint16_t(data.size());
    out.insert(out.end(), {std::uint8_t(length), std::uint8_t(length >> 8), std::uint8_t(~length),
                           std::uint8_t(~length >> 8)});
    out.insert(out.end(), data.begin(), data.end());
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t x : data) {
        a = (a + x) % 65521;
        b = (b + a) % 65521;
    }
    const std::uint32_t adler = (b << 16) | a;
    out.insert(out.end(), {std::uint8_t(adler >> 24), std::uint8_t(adler >> 16), std::uint8_t(adler >> 8),
                           std::uint8_t(adler)});
    return out;
}

// DXBC { DXIL { stub, XDAT { 0x0D: XBCH + zlib(fssm) } } } (d3d12_compute_test's container).
std::vector<std::uint8_t> shader_container(const std::vector<std::uint8_t>& fssm) {
    const auto stream = zlib_stored(fssm);
    std::vector<std::uint8_t> xbch = {'X', 'B', 'C', 'H'};
    for (const std::uint32_t v : {1u, std::uint32_t(stream.size()), 0u, std::uint32_t(fssm.size()), 0u}) put32(xbch, v);
    xbch.insert(xbch.end(), stream.begin(), stream.end());
    std::vector<std::uint8_t> dxil(16, 0);
    dxil.insert(dxil.end(), {'X', 'D', 'A', 'T'});
    put32(dxil, 1);
    put32(dxil, 12);
    put32(dxil, 0x0d);
    put32(dxil, 16 + 12 + 12);
    put32(dxil, std::uint32_t(xbch.size()));
    dxil.insert(dxil.end(), xbch.begin(), xbch.end());
    std::vector<std::uint8_t> out = {'D', 'X', 'B', 'C'};
    out.resize(28, 0);
    put32(out, 1);
    put32(out, 36);
    out.insert(out.end(), {'D', 'X', 'I', 'L'});
    put32(out, std::uint32_t(dxil.size()));
    out.insert(out.end(), dxil.begin(), dxil.end());
    return out;
}

std::vector<std::uint8_t> serialize(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc) {
    const HMODULE d3d12 = LoadLibraryExA("d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto fn = d3d12 ? reinterpret_cast<SerializeFn>(
                                reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature")))
                          : nullptr;
    std::vector<std::uint8_t> out;
    ID3DBlob* blob = nullptr;
    if (fn && SUCCEEDED(fn(&desc, &blob, nullptr)) && blob) {
        const auto* p = static_cast<const std::uint8_t*>(blob->GetBufferPointer());
        out.assign(p, p + blob->GetBufferSize());
        blob->Release();
    }
    return out;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

// The device's queue, a fence, and a submission waited for.
struct Runner {
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12Fence> fence;
    UINT64 value = 0;
    bool init(ID3D12Device* device) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        return SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue.p))) &&
               SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p)));
    }
    bool run(ID3D12GraphicsCommandList* list) {
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.p, ++value))) return false;
        const HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const bool ok = SUCCEEDED(fence->SetEventOnCompletion(value, done)) && WaitForSingleObject(done, 10000) == 0;
        CloseHandle(done);
        return ok && rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success;
    }
};

// A title's __XMemAllocationHooks__ (rt::xmem::set_allocation_hooks), as the retail title's render-target pool does
// it: the first allocation takes a 64 MiB pool from XMemAllocDefault with the D3D runtime's attributes, the rest are
// handed out of it in order (each 64 KiB-aligned). The pool's bytes are dirtied first: a committed resource must start
// zeroed whatever the allocator gives.
struct TitleAllocatorLog {
    XMemAllocFn alloc_default = nullptr;
    XMemFreeFn free_default = nullptr;
    std::uint8_t* pool = nullptr;
    std::uint64_t used = 0;
    int allocs = 0, frees = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> given;  // (address, attributes)
    std::vector<std::pair<std::uint64_t, std::uint64_t>> freed;
    std::vector<std::uint64_t> sizes;
};
TitleAllocatorLog g_title;
constexpr std::uint64_t kTitlePool = 64 << 20;

void* __stdcall title_pool_alloc(std::size_t size, std::uint64_t attributes) {
    ++g_title.allocs;
    if (!g_title.pool) {
        g_title.pool = static_cast<std::uint8_t*>(g_title.alloc_default(kTitlePool, attributes));
        if (!g_title.pool) return nullptr;
        std::memset(g_title.pool, 0xcd, kTitlePool);
    }
    const std::uint64_t at = (g_title.used + 0xffff) & ~std::uint64_t(0xffff);
    if (at + size > kTitlePool) return nullptr;
    g_title.used = at + size;
    void* const p = g_title.pool + at;
    g_title.given.emplace_back(reinterpret_cast<std::uint64_t>(p), attributes);
    g_title.sizes.push_back(size);
    return p;
}

void __stdcall title_pool_free(void* p, std::uint64_t attributes) {
    ++g_title.frees;
    g_title.freed.emplace_back(reinterpret_cast<std::uint64_t>(p), attributes);
}

void* g_title_hooks[2] = {reinterpret_cast<void*>(&title_pool_alloc), reinterpret_cast<void*>(&title_pool_free)};

const rt::xmem::GraphicsListener kListener{
    [](std::uintptr_t start, std::size_t size) { rt::d3d12::title_memory_visible(start, size); },
    [](std::uintptr_t start, std::size_t size) { rt::d3d12::title_memory_gone(start, size); }};

int run(ID3D12Device* inner, void* device, const std::vector<std::uint8_t>& container) {
    const auto virtual_alloc = reinterpret_cast<XMemVirtualAllocFn>(rt::shims::find("xmem.dll", "XMemVirtualAlloc"));
    const auto allocate_pages =
        reinterpret_cast<XMemAllocatePhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemAllocatePhysicalPages"));
    const auto free_pages = reinterpret_cast<XMemFreePhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemFreePhysicalPages"));
    const auto map_pages = reinterpret_cast<XMemMapPhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemMapPhysicalPages"));
    const auto title_free = reinterpret_cast<VirtualFreeFn>(rt::shims::find("kernel32.dll", "VirtualFree"));
    EXPECT(virtual_alloc && allocate_pages && free_pages && map_pages && title_free);

    // ---- Placement and imports.
    constexpr SIZE_T kPool = 4 << 20;
    auto* pool = static_cast<std::uint8_t*>(virtual_alloc(nullptr, kPool, MEM_RESERVE | MEM_COMMIT, kGraphics,
                                                          PAGE_READWRITE));
    auto* low = static_cast<std::uint8_t*>(virtual_alloc(nullptr, 1 << 20, MEM_RESERVE | MEM_COMMIT, kGraphics32,
                                                         PAGE_READWRITE));
    EXPECT(pool && low);
    const auto A = reinterpret_cast<std::uint64_t>(pool), T = reinterpret_cast<std::uint64_t>(low);
    EXPECT(A >= tm::kWindowStart && A + kPool <= tm::kWindowEnd);
    EXPECT(T >= tm::kLow32Start && T + (1 << 20) <= tm::kLow32End);
    const rt::d3d12::TitleMemoryStats stats = rt::d3d12::title_memory_stats(inner);
    EXPECT(stats.enabled && stats.heap_base == tm::kHeapBase && stats.imports >= 2 && !stats.failed);
    EXPECT(rt::d3d12::title_memory_device_address(inner, A) == tm::kHeapBase + A);
    EXPECT(rt::d3d12::title_memory_device_address(inner, A + kPool - 4) == tm::kHeapBase + A + kPool - 4);
    EXPECT(rt::d3d12::title_memory_device_address(inner, T + 0x40) == tm::kHeapBase + T + 0x40);
    std::printf("title memory: heap base 0x%llx, graphics memory at 0x%llx, 32-bit memory at 0x%llx, %llu imports\n",
                static_cast<unsigned long long>(stats.heap_base), static_cast<unsigned long long>(A),
                static_cast<unsigned long long>(T), static_cast<unsigned long long>(stats.imports));

    // ---- Slot 70, a buffer: the V# is in a console heap in the 32-bit memory, its base the title address.
    auto* words = reinterpret_cast<std::uint32_t*>(pool);
    for (std::uint32_t i = 0; i < 64; ++i) words[i] = 0x5eed0000u + i * 7u;
    std::uint32_t resource_words[14] = {};
    {
        const D3D12_RESOURCE_DESC rd = buffer_desc(kPool);
        std::memcpy(resource_words, &rd, sizeof(rd));
        resource_words[11] = 1;           // the Xbox row-major layout word
        resource_words[1] = 0xdeadbeef;   // the padding, which the title leaves as its stack had it
    }
    std::uint32_t view_words[16] = {};
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = 64;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        std::memcpy(view_words, &srv, sizeof(srv));
        view_words[10] = static_cast<std::uint32_t>(A);
        view_words[11] = static_cast<std::uint32_t>(A >> 32);
    }
    EXPECT(call(device, 70, word(resource_words), word(view_words), T) == 0);
    leonida::gfx10::BufferWords vsharp{};
    std::memcpy(vsharp.data(), low, sizeof(vsharp));
    leonida::gfx10::Buffer decoded{};
    EXPECT(leonida::gfx10::decode(vsharp, decoded));
    EXPECT(decoded.address == A && decoded.num_records == 256);

    // A compute dispatch that reads it: s4 = 0, s5 = ~0 (V# mode), s6:s7 = a root UAV at title address A + 2 MiB,
    // s8 = the table at T (cs_user_data.comp).
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 2};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor = {0, 0};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = {1, &range};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    root_desc.Desc_1_0 = {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    const auto root_blob = serialize(root_desc);
    EXPECT(!root_blob.empty());
    Ref<ID3D12RootSignature> root;
    EXPECT(SUCCEEDED(inner->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root.p;
    pso_desc.CS = {container.data(), container.size()};
    Ref<ID3D12PipelineState> pso;
    EXPECT(SUCCEEDED(inner->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso.p))));
    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64,
                                               D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap;
    EXPECT(SUCCEEDED(rt::d3d12::create_console_descriptor_heap(inner, &heap_desc, T, 64 * 32,
                                                               IID_PPV_ARGS(&heap.p))));
    Runner runner;
    EXPECT(runner.init(inner));
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(SUCCEEDED(inner->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(inner->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                              IID_PPV_ARGS(&list.p))));
    std::memset(pool + (2 << 20), 0, 256);
    ID3D12DescriptorHeap* heaps[] = {heap.p};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.p);
    list->SetPipelineState(pso.p);
    const std::uint32_t srv_mode[2] = {0u, 0xFFFFFFFFu};
    list->SetComputeRoot32BitConstants(0, 2, srv_mode, 0);
    list->SetComputeRootUnorderedAccessView(1, A + (2 << 20));
    list->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{T});
    list->Dispatch(1, 1, 1);
    EXPECT(SUCCEEDED(list->Close()));
    EXPECT(runner.run(list.p));
    EXPECT(std::memcmp(pool + (2 << 20), pool, 256) == 0);
    std::printf("title memory: a shader read 256 bytes through a slot-70 V# at title address 0x%llx in a heap at "
                "0x%llx and wrote them through a root UAV at 0x%llx; the CPU sees them in place\n",
                static_cast<unsigned long long>(A), static_cast<unsigned long long>(T),
                static_cast<unsigned long long>(A + (2 << 20)));

    // ---- Slot 70, a texture: 64x64 RGBA8 in SW_64KB_D at A + 1 MiB, detiled into the texture it makes.
    constexpr UINT kSide = 64;
    std::vector<std::byte> linear(kSide * kSide * 4);
    for (std::size_t i = 0; i < linear.size(); ++i) linear[i] = std::byte(i * 13 + (i >> 8));
    leonida::gfx10::TiledSurface2D surface;
    EXPECT(leonida::gfx10::TiledSurface2D::make(leonida::gfx10::TileMode::sw_64kb_d, 4, kSide, kSide, surface));
    EXPECT(surface.tile(linear, std::span<std::byte>(reinterpret_cast<std::byte*>(pool + (1 << 20)),
                                                     surface.tiled_size()), kSide * 4));
    std::uint32_t texture_words[14] = {};
    {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kSide;
        td.Height = kSide;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        std::memcpy(texture_words, &td, sizeof(td));
        texture_words[11] = 0x10a;  // 0x100 | SW_64KB_D
    }
    std::uint32_t texture_view[16] = {};
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        std::memcpy(texture_view, &srv, sizeof(srv));
        const std::uint64_t at = A + (1 << 20);
        texture_view[10] = static_cast<std::uint32_t>(at);
        texture_view[11] = static_cast<std::uint32_t>(at >> 32);
    }
    EXPECT(call(device, 70, word(texture_words), word(texture_view), T + 32) == 0);
    std::uint32_t tsharp[8] = {};
    std::memcpy(tsharp, low + 32, sizeof(tsharp));
    EXPECT(rt::d3d12::bindless_hash_lookup(inner, tsharp) != 0);
    texture_words[13] = 0x1234;  // other padding: the same texture (V resource.rs:2383-2396)
    Ref<ID3D12Resource> texture;
    texture.p = rt::d3d12::title_placed_texture(inner, A + (1 << 20), texture_words);
    EXPECT(texture.p);
    {
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        const auto readback_desc = buffer_desc(linear.size());
        Ref<ID3D12Resource> readback;
        EXPECT(SUCCEEDED(inner->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&readback.p))));
        EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, nullptr)));
        D3D12_TEXTURE_COPY_LOCATION src{texture.p, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        D3D12_TEXTURE_COPY_LOCATION dst{readback.p, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kSide, kSide, 1, kSide * 4};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        EXPECT(SUCCEEDED(list->Close()));
        EXPECT(runner.run(list.p));
        void* mapped = nullptr;
        EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
        const bool same = std::memcmp(mapped, linear.data(), linear.size()) == 0;
        readback->Unmap(0, nullptr);
        EXPECT(same);
    }
    std::printf("title memory: a slot-70 texture view of a 64x64 SW_64KB_D texture in title memory reads back as "
                "its linear texels\n");

    // ---- Slot 71: a structured buffer UAV (a V# at the title address plus the first element) and a texture UAV.
    {
        std::uint32_t uav_resource[14] = {};
        D3D12_RESOURCE_DESC rd = buffer_desc(kPool);
        rd.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x20004);  // ALLOW_UNORDERED_ACCESS and an Xbox bit, as slot 133's
        std::memcpy(uav_resource, &rd, sizeof(rd));
        std::uint32_t uav_view[16] = {};
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement = 16;
        uav.Buffer.NumElements = 32;
        uav.Buffer.StructureByteStride = 16;
        std::memcpy(uav_view, &uav, sizeof(uav));
        uav_view[10] = static_cast<std::uint32_t>(A);
        uav_view[11] = static_cast<std::uint32_t>(A >> 32);
        EXPECT(call(device, 71, word(uav_resource), word(uav_view), T + 64) == 0);
        leonida::gfx10::BufferWords uav_words{};
        std::memcpy(uav_words.data(), low + 64, sizeof(uav_words));
        leonida::gfx10::Buffer uav_buffer{};
        EXPECT(leonida::gfx10::decode(uav_words, uav_buffer));
        EXPECT(uav_buffer.address == A + 16 * 16 && uav_buffer.stride == 16 && uav_buffer.num_records == 32);

        std::uint32_t uav_texture[14] = {};
        std::memcpy(uav_texture, texture_words, sizeof(uav_texture));
        uav_texture[12] = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        std::uint32_t texture_uav_view[16] = {};
        D3D12_UNORDERED_ACCESS_VIEW_DESC tuav{};
        tuav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        tuav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        std::memcpy(texture_uav_view, &tuav, sizeof(tuav));
        texture_uav_view[10] = texture_view[10];
        texture_uav_view[11] = texture_view[11];
        EXPECT(call(device, 71, word(uav_texture), word(texture_uav_view), T + 96) == 0);
        std::uint32_t uav_tsharp[8] = {};
        std::memcpy(uav_tsharp, low + 96, sizeof(uav_tsharp));
        EXPECT(rt::d3d12::bindless_hash_lookup(inner, uav_tsharp) != 0);
    }
    std::printf("title memory: slot 71 writes a structured V# at the title address and a texture UAV's T#\n");

    // ---- Console list slots on title addresses, on a list from device slot 73.
    void* console_allocator = nullptr;
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    EXPECT(call(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, word(&allocator_iid), word(&console_allocator)) == S_OK);
    struct {
        std::uint32_t type, node_mask;
        std::uint64_t reserved[2];
    } list_desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, {0, 0}};
    void* console_list = nullptr;
    const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
    EXPECT(call(device, 73, word(&list_desc), word(console_allocator), 0, word(&list_iid), word(&console_list)) == S_OK);
    EXPECT(rt::d3d12_xs::is_command_list_proxy(console_list));
    for (std::uint32_t i = 0; i < 1024; ++i) words[(3 << 20) / 4 + i] = 0;
    call(console_list, 62, A + (3 << 20), A, 4096, 0);                    // CopyMemoryToMemoryX
    call(console_list, 71, A + 0x3F0000, 0xCAFEF00Du, 0, 0);             // Write32
    call(console_list, 72, A + 0x3F0008, 0x0123456789ABCDEFull, 0, 0);   // Write64
    call(console_list, 73, A + 0x3F0000, 3, 0xCAFEF00Du);                // WaitValue32: equal
    auto* upload = reinterpret_cast<std::uint8_t*>(call(console_list, 148, 256, 256));
    EXPECT(upload && reinterpret_cast<std::uint64_t>(upload) % 256 == 0);
    EXPECT(call(console_list, 148, 8, 48) == 0);  // not a power of two
    const auto U = reinterpret_cast<std::uint64_t>(upload);
    EXPECT(U >= tm::kWindowStart && U + 256 <= tm::kWindowEnd);
    EXPECT(rt::d3d12::title_memory_device_address(inner, U) == tm::kHeapBase + U);
    for (int i = 0; i < 256; ++i) upload[i] = std::uint8_t(255 - i);
    call(console_list, 62, A + 0x3E0000, U, 256, 0);  // from the list's upload memory
    rt::d3d12_xs::ValueWait waits[2] = {};
    EXPECT(rt::d3d12_xs::command_list_value_waits(console_list, waits, 2) == 1);
    EXPECT(waits[0].address == A + 0x3F0000 && waits[0].comparison == 3 && !waits[0].wide);
    EXPECT(!waits[0].met_by(0) && waits[0].met_by(0xCAFEF00Du) && waits[0].met_by(0x1CAFEF00Dull));
    EXPECT(hr_from(call(console_list, 9)) == S_OK);  // Close
    EXPECT(runner.run(rt::d3d12_xs::command_list_proxy_real_list(console_list)));
    EXPECT(std::memcmp(pool + (3 << 20), pool, 4096) == 0);
    EXPECT(words[0x3F0000 / 4] == 0xCAFEF00Du);
    EXPECT(words[0x3F0008 / 4] == 0x89ABCDEFu && words[0x3F000C / 4] == 0x01234567u);
    for (int i = 0; i < 256; ++i) EXPECT(pool[0x3E0000 + i] == std::uint8_t(255 - i));
    EXPECT(waits[0].met_by(words[0x3F0000 / 4]));
    // Reset gives the upload memory back and forgets the waits.
    EXPECT(hr_from(call(console_list, 10, word(console_allocator), 0)) == S_OK);
    EXPECT(rt::d3d12_xs::command_list_value_waits(console_list, waits, 2) == 0);
    std::printf("title memory: list slots 62, 71, 72 and 148 on title addresses, 73 recorded for the queue\n");
    EXPECT(hr_from(call(console_list, 9)) == S_OK);
    static_cast<IUnknown*>(console_list)->Release();
    static_cast<IUnknown*>(console_allocator)->Release();

    // ---- Slot 133 on mapped physical pages: bound to their import.
    constexpr std::uint64_t kPages = 4;
    auto* window = static_cast<std::uint8_t*>(virtual_alloc(nullptr, kPages << 16, MEM_RESERVE | kXmem64kPages, 0x23,
                                                            PAGE_READWRITE));
    EXPECT(window);
    std::uint64_t pages[kPages]{};
    std::uint64_t count = kPages;
    EXPECT(allocate_pages(0x280, &count, pages) && count == kPages);
    EXPECT(map_pages(window, kPages, pages) == window);
    const auto W = reinterpret_cast<std::uint64_t>(window);
    EXPECT(rt::d3d12::title_memory_device_address(inner, W + 0x20000) == tm::kHeapBase + W + 0x20000);
    {
        D3D12_RESOURCE_DESC desc = buffer_desc(kPages << 16);
        desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(1);
        const GUID resource_iid = __uuidof(ID3D12Resource);
        Ref<ID3D12Resource> placed;
        EXPECT(call(device, 133, W, word(&desc), 0xFFFFFFFFull, 0, word(&resource_iid), word(&placed.p)) == S_OK);
        EXPECT(placed.p && placed->GetGPUVirtualAddress() == W);
        // Remapping the pages takes the import off the GPU; the buffer keeps its memory until it goes.
    }
    EXPECT(map_pages(window, kPages, nullptr) == window);
    EXPECT(rt::d3d12::title_memory_device_address(inner, W) == 0);
    EXPECT(free_pages(count, pages));

    // ---- The title's VirtualFree takes the pool's imports off the GPU first.
    const std::uint64_t before = rt::d3d12::title_memory_stats(inner).imports;
    EXPECT(title_free(pool, 0, MEM_RELEASE));
    EXPECT(rt::d3d12::title_memory_device_address(inner, A) == 0);
    EXPECT(rt::d3d12::title_memory_stats(inner).imports < before);
    EXPECT(title_free(low, 0, MEM_RELEASE));
    EXPECT(rt::d3d12::title_memory_device_address(inner, T) == 0);
    std::printf("title memory: slot 133 on mapped physical pages shares their import; unmapping and the title's "
                "VirtualFree release the imports\n");
    return 0;
}

// Committed resources from the title's allocator (rt::d3d12::set_title_allocator), wired as gta6rt wires it: xmem's
// XMemAlloc/XMemFree, which call the title's __XMemAllocationHooks__ routines (title_pool_alloc above).
int run_title_allocator(ID3D12Device* inner, void* device) {
    g_title = TitleAllocatorLog{};
    g_title.alloc_default = reinterpret_cast<XMemAllocFn>(rt::shims::find("xmem.dll", "XMemAllocDefault"));
    g_title.free_default = reinterpret_cast<XMemFreeFn>(rt::shims::find("xmem.dll", "XMemFreeDefault"));
    EXPECT(g_title.alloc_default && g_title.free_default);
    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
    const D3D12_RESOURCE_DESC one_block = buffer_desc(1 << 16);

    // Without an allocator a committed buffer is the layer's own, in the arena, as before.
    {
        Ref<ID3D12Resource> before_hooks;
        EXPECT(SUCCEEDED(inner->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &one_block,
                                                        D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                        IID_PPV_ARGS(&before_hooks.p))));
        EXPECT(before_hooks->GetGPUVirtualAddress() < tm::kLow32Start && g_title.allocs == 0);
    }

    rt::xmem::set_allocation_hooks(g_title_hooks);
    rt::d3d12::set_title_allocator(&rt::xmem::title_alloc, &rt::xmem::title_free);

    // A back buffer through the console's slot 127 (CreateCommittedResource3), with the title's arguments: heap flag
    // ALLOW_DISPLAY, flags RENDER_TARGET | UNORDERED_ACCESS, barrier layout 3.
    D3D12_RESOURCE_DESC1 target{};
    target.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target.Width = 256;
    target.Height = 256;
    target.DepthOrArraySize = 1;
    target.MipLevels = 1;
    target.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target.SampleDesc.Count = 1;
    target.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x5);
    D3D12_RESOURCE_DESC plain{};
    std::memcpy(&plain, &target, sizeof(plain));
    const D3D12_RESOURCE_ALLOCATION_INFO target_info = inner->GetResourceAllocationInfo(0, 1, &plain);
    using Slot127 = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                           std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                           std::uint64_t);
    const GUID resource_iid = __uuidof(ID3D12Resource);
    Ref<ID3D12Resource> back_buffer;
    EXPECT(hr_from(reinterpret_cast<Slot127>((*static_cast<void***>(device))[127])(
               device, word(&default_heap), D3D12_HEAP_FLAG_ALLOW_DISPLAY, word(&target), 3, 0, 0, 0, 0,
               word(&resource_iid), word(&back_buffer.p))) == S_OK);
    EXPECT(back_buffer.p && g_title.allocs == 1 && g_title.given.size() == 1);
    const std::uint64_t texture_at = g_title.given[0].first;
    EXPECT(g_title.sizes[0] == target_info.SizeInBytes);
    EXPECT(g_title.given[0].second ==
           tm::resource_attributes(target_info.SizeInBytes, target_info.Alignment, true));
    EXPECT(back_buffer->GetGPUVirtualAddress() == texture_at);
    EXPECT(rt::d3d12::vulkan_image(back_buffer.p) != VK_NULL_HANDLE);
    const auto pool = reinterpret_cast<std::uint64_t>(g_title.pool);
    EXPECT(pool && (pool & 0xffff) == 0 && tm::addressable(pool, kTitlePool));  // 64 KiB pages: under 2 MiB
    for (std::uint64_t i = 0; i < target_info.SizeInBytes; i += 4096)
        EXPECT(g_title.pool[texture_at - pool + i] == 0);  // zeroed over the pool's 0xcd

    // Buffers: on their title memory, at heap_base + the title address. The upload buffer maps at its title address;
    // a copy into the default-heap buffer lands in the title's pages.
    constexpr UINT64 kBytes = 1 << 16;
    const D3D12_RESOURCE_DESC upload_desc = buffer_desc(kBytes);
    D3D12_RESOURCE_DESC uav = buffer_desc(kBytes);
    uav.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Ref<ID3D12Resource> source, destination;
    EXPECT(SUCCEEDED(inner->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&source.p))));
    EXPECT(SUCCEEDED(inner->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav,
                                                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&destination.p))));
    EXPECT(g_title.allocs == 3 && g_title.given.size() == 3);
    const std::uint64_t S = g_title.given[1].first, D = g_title.given[2].first;
    EXPECT(g_title.given[1].second == tm::resource_attributes(kBytes, 0x10000, false));
    EXPECT(g_title.given[2].second == tm::resource_attributes(kBytes, 0x10000, true));
    EXPECT(source->GetGPUVirtualAddress() == S && destination->GetGPUVirtualAddress() == D);
    EXPECT(rt::d3d12::title_memory_device_address(inner, S) == tm::kHeapBase + S);
    EXPECT(rt::d3d12::title_memory_device_address(inner, D + kBytes - 4) == tm::kHeapBase + D + kBytes - 4);
    void* mapped = nullptr;
    EXPECT(SUCCEEDED(source->Map(0, nullptr, &mapped)) && reinterpret_cast<std::uint64_t>(mapped) == S);
    EXPECT(destination->Map(0, nullptr, &mapped) == E_INVALIDARG);  // a default heap, as on the PC
    auto* src = reinterpret_cast<std::uint8_t*>(S);
    auto* dst = reinterpret_cast<std::uint8_t*>(D);
    for (UINT64 i = 0; i < kBytes; ++i) EXPECT(src[i] == 0 && dst[i] == 0);
    for (UINT64 i = 0; i < kBytes; ++i) src[i] = std::uint8_t(i * 7 + 3);
    {
        Runner runner;
        EXPECT(runner.init(inner));
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        EXPECT(SUCCEEDED(inner->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        EXPECT(SUCCEEDED(inner->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                                  IID_PPV_ARGS(&list.p))));
        list->CopyBufferRegion(destination.p, 0, source.p, 0, kBytes);
        EXPECT(SUCCEEDED(list->Close()));
        EXPECT(runner.run(list.p));
    }
    EXPECT(std::memcmp(dst, src, kBytes) == 0);
    std::printf("title memory: committed resources from the title's allocator: a slot-127 texture reports 0x%llx, "
                "buffers sit at 0x%llx and 0x%llx on their title memory; a GPU copy lands in the title's pages\n",
                static_cast<unsigned long long>(texture_at), static_cast<unsigned long long>(S),
                static_cast<unsigned long long>(D));

    // Released: each goes back through the title's free routine with its address and attributes.
    EXPECT(g_title.frees == 0);
    back_buffer.reset();
    EXPECT(g_title.frees == 1 && g_title.freed[0] == g_title.given[0]);
    destination.reset();
    source.reset();
    EXPECT(g_title.frees == 3 && g_title.freed[1] == g_title.given[2] && g_title.freed[2] == g_title.given[1]);

    // A refusal from the allocator is E_OUTOFMEMORY, with nothing made.
    const D3D12_RESOURCE_DESC too_big_desc = buffer_desc(kTitlePool * 2);
    Ref<ID3D12Resource> too_big;
    EXPECT(inner->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &too_big_desc,
                                          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&too_big.p)) ==
           E_OUTOFMEMORY);
    EXPECT(!too_big.p && g_title.frees == 3);

    rt::d3d12::set_title_allocator(nullptr, nullptr);
    rt::xmem::set_allocation_hooks(nullptr);
    {
        Ref<ID3D12Resource> after_hooks;
        EXPECT(SUCCEEDED(inner->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &one_block,
                                                        D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                        IID_PPV_ARGS(&after_hooks.p))));
        EXPECT(after_hooks->GetGPUVirtualAddress() < tm::kLow32Start && g_title.allocs == 4);
    }
    g_title.free_default(g_title.pool, 0);
    EXPECT(rt::d3d12::title_memory_device_address(inner, pool) == 0);
    std::printf("title memory: released committed resources went back through the title's free routine\n");
    return 0;
}

}  // namespace

int main() {
    // The translator-style library cs_user_data.comp comes from, as d3d12_compute_test makes it.
    constexpr std::uint32_t code_word = 7000;
    std::vector<std::uint8_t> fssm = {'F', 'S', 'S', 'M'};
    put32(fssm, code_word);
    for (std::uint32_t i = 0; i < 14; ++i) put32(fssm, 0xbf810000u + i);
    const auto container = shader_container(fssm);
    const testfiles::ScratchDir scratch("leonida_title_memory_test");
    const auto& library = scratch.path();
    EXPECT(testfiles::copy_file(vktest::shader_fixture_path(LEONIDA_TEST_CS_SPV), library / "cs_user_data.spv"));
    {
        std::ofstream index(library / "index.json", std::ios::binary);
        index << "{\"cs_user_data\": {\"stage\": \"cs\", \"fssm_sha256\": \"" << leonida::sha256_hex(fssm)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"local_size\": [64, 1, 1], "
                 "\"uses_code_base\": true, \"wave64\": false}}";
    }
    SetEnvironmentVariableW(L"LEONIDA_SHADERS", library.wstring().c_str());

    ID3D12Device* inner = nullptr;
    if (FAILED(rt::d3d12::create_vulkan_device_standalone(__uuidof(ID3D12Device), reinterpret_cast<void**>(&inner)))) {
        std::printf("SKIP: no Vulkan device\n");
        return 77;
    }
    if (!rt::d3d12::title_memory_stats(inner).enabled) {
        std::printf("SKIP: title memory is off on this GPU (no bufferDeviceAddressCaptureReplay or "
                    "VK_EXT_external_memory_host, or the driver refused the heap base)\n");
        inner->Release();
        return 77;
    }
    void* device = nullptr;
    if (FAILED(rt::d3d12_xs::create_device_proxy(inner, kDevice11, &device))) {
        inner->Release();
        return 1;
    }
    rt::d3d12::set_device_outer(inner, static_cast<IUnknown*>(device));
    rt::xmem::set_graphics_listener(&kListener);
    int failures = run(inner, device, container);
    if (!failures) failures = run_title_allocator(inner, device);
    rt::xmem::set_graphics_listener(nullptr);
    static_cast<IUnknown*>(device)->Release();
    if (!failures) std::printf("d3d12_title_memory_test: PASS\n");
    return failures ? 1 : 0;
}
