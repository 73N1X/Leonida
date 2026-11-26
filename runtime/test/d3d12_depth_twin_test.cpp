// Depth textures that also allow unordered access, through the Xbox-ABI proxy over the Vulkan device. The console
// lets a texture be ALLOW_DEPTH_STENCIL | ALLOW_UNORDERED_ACCESS (the title makes a 2560x1440 R32G8X24_TYPELESS one,
// flags 0x6, and its tiled lighting writes each pixel's material class into the stencil plane through a UAV); the PC
// runtime and Vulkan do not. The layer gives such a texture colour twins that carry its UAVs (VIctory's, victory-gpu
// resource.rs twin_texture): R32 for the depth plane, R8_UINT for the stencil plane, and copies one side into the
// other where the title switches between them (docs/d3d12_vulkan.md, "Depth textures with unordered access").
//
//   1. Console slot 76 makes a 64x64 R32G8X24_TYPELESS texture with flags 0x6; the PC CreateCommittedResource of the
//      same description is refused, as the PC runtime refuses it.
//   2. Its UAV of plane 1 is an 8_UINT T#, of plane 0 a 32_FLOAT one: views of the twins.
//   3. List A clears the depth to 0.25 and the stencil to 0x11, moves the texture to UNORDERED_ACCESS and runs a
//      dispatch that writes a pattern into the left half of the stencil plane through its UAV. List B (executed after
//      it, in a second ExecuteCommandLists) writes the left three quarters of the depth plane through its UAV, moves the
//      texture to COPY_SOURCE and copies both planes into a readback buffer: the shaders' texels where they wrote,
//      the clear values elsewhere (the twins were given the depth texture when it entered UNORDERED_ACCESS, and the
//      depth texture was given the twins when it left).
//   4. List C clears to 0.75 / 0x22, writes another stencil pattern into the left quarter and copies both planes out
//      again: the twins were given the new clear before the dispatch, and nothing of list A's pattern is left.
// Run with LEONIDA_DEPTH_TWIN_COPY=buffer, the copies go through a buffer instead of vkCmdCopyImage between the
// depth and colour images (VK_KHR_maintenance8); the results must be the same.
// No game data: the program is a few marker words and the module is GLSL written to the translator's contract.
// Set LEONIDA_VK_VALIDATION=1 to run under VK_LAYER_KHRONOS_validation (0 errors required).
#include "shader_fixture_path.h"
#include "test_temp_dir.h"
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/gfx10_descriptors.h"
#include "../src/shader_library.h"
#include "../src/vulkan_replay.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#ifndef LEONIDA_TEST_CS_DEPTH_TWIN_SPV
#error "LEONIDA_TEST_CS_DEPTH_TWIN_SPV must name the compiled test shader"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slots 9/10: handle in RAX
using XboxCreateResourceFn = HRESULT(WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                              const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                              const D3D12_CLEAR_VALUE*, const IID*, void**);

namespace {

namespace g = leonida::gfx10;

constexpr UINT kSize = 64, kPitch = 256, kDepthOffset = kPitch * kSize, kReadbackBytes = 2 * kPitch * kSize;

std::uint8_t stencil_pattern(UINT x, UINT y, UINT seed) { return std::uint8_t((x * 7 + y * 13 + seed) & 255); }
float depth_pattern(UINT x, UINT y) { return float((x + 64 * y) % 997) / 1024.0f; }

// GFX10 T# word 1 bits 20..28: the FORMAT field.
std::uint32_t tsharp_format(std::uint64_t descriptor) {
    std::uint32_t words[8] = {};
    std::memcpy(words, reinterpret_cast<const void*>(descriptor), sizeof(words));
    return (words[1] >> 20) & 0x1ff;
}

// The readback of one round: stencil bytes at 0, depth floats at kDepthOffset, both with a 256-byte pitch.
unsigned check(const std::uint8_t* bytes, const char* round, UINT stencil_columns, UINT seed, std::uint8_t stencil_clear,
               UINT depth_columns, float depth_clear) {
    unsigned wrong = 0;
    for (UINT y = 0; y < kSize; ++y) {
        for (UINT x = 0; x < kSize; ++x) {
            const std::uint8_t stencil = bytes[y * kPitch + x];
            const std::uint8_t want_stencil = x < stencil_columns ? stencil_pattern(x, y, seed) : stencil_clear;
            if (stencil != want_stencil && wrong++ < 8)
                std::fprintf(stderr, "%s: stencil (%u, %u) = 0x%02x, want 0x%02x\n", round, x, y, stencil, want_stencil);
            float depth = 0;
            std::memcpy(&depth, bytes + kDepthOffset + y * kPitch + x * 4, sizeof(depth));
            const float want_depth = x < depth_columns ? depth_pattern(x, y) : depth_clear;
            if (depth != want_depth && wrong++ < 8)
                std::fprintf(stderr, "%s: depth (%u, %u) = %.6f, want %.6f\n", round, x, y, depth, want_depth);
        }
    }
    return wrong;
}

}  // namespace

int main() {
    vktest::Device d;
    d.request_maintenance8 = true;
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) return 77;   // no Vulkan GPU: ctest skips
    if (device_status != vktest::DeviceStatus::ready) return 1;
    if (!d.bindless) {
        std::printf("SKIP: the device lacks the descriptor indexing features of the bindless set 0\n");
        return 77;
    }
    const char* copy_mode = std::getenv("LEONIDA_DEPTH_TWIN_COPY");
    const bool through_buffer = !d.maintenance8 || (copy_mode && std::strcmp(copy_mode, "buffer") == 0);

    const auto program = fssm_program({0x43530004, 0xbf810000, 0x0d0e0f10});
    const auto container = shader_container(program);
    const testfiles::ScratchDir scratch("leonida_depth_twin_test");
    const auto& library = scratch.path();
    EXPECT(testfiles::copy_file(vktest::shader_fixture_path(LEONIDA_TEST_CS_DEPTH_TWIN_SPV),
                                library / "cs_depth_twin.spv"));
    {
        std::ofstream index(library / "index.json", std::ios::binary);
        index << "{\"cs_depth_twin\": {\"stage\": \"cs\", \"fssm_sha256\": \"" << leonida::sha256_hex(program)
              << "\", \"bindings\": [2, 8, 9], \"push_constant_bytes\": 144, \"local_size\": [8, 8, 1], "
                 "\"uses_code_base\": false, \"wave64\": false}}";
    }

    std::vector<std::uint8_t> round1(kReadbackBytes), round2(kReadbackBytes);
    const leonida::DepthTwinCounts before = leonida::depth_twin_counts();
    leonida::DepthTwinCounts after_round1{};
    {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        info.buffer_device_address = true;
        info.bindless = d.bindless;
        info.bindless_partially_bound = d.partially_bound;
        info.image_view_min_lod = d.image_view_min_lod;
        info.maintenance8 = d.maintenance8;
        info.shader_dir = library.string();
        ID3D12Device* inner = nullptr;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        Ref<ID3D12Device> device;
        EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                           reinterpret_cast<void**>(&device.p))));
        rt::d3d12::set_device_outer(inner, device.p);
        if (rt::d3d12::device_push_constants_need_fallback(inner)) {
            std::printf("d3d12_depth_twin_test: skipped, the PSOs need 144 push-constant bytes and this GPU has %u\n",
                        rt::d3d12::device_max_push_constants_size(inner));
            return 77;
        }

        // 1. The texture through console slot 76; the PC method refuses the same description.
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = desc.Height = kSize;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        Ref<ID3D12Resource> refused;
        EXPECT(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr,
                                               IID_PPV_ARGS(&refused.p)) == E_INVALIDARG && !refused.p);
        D3D12_RESOURCE_DESC xbox_desc = desc;
        xbox_desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);  // the Xbox tiled layout word
        const IID resource_iid = __uuidof(ID3D12Resource);
        Ref<ID3D12Resource> depth, readback;
        EXPECT(SUCCEEDED(reinterpret_cast<XboxCreateResourceFn>(slot(device.p, 76))(
            device.p, &default_heap, D3D12_HEAP_FLAG_NONE, &xbox_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr,
            &resource_iid, reinterpret_cast<void**>(&depth.p))));
        EXPECT(depth->GetDesc().Flags == desc.Flags);
        const auto readback_desc = buffer_desc(kReadbackBytes);
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&readback.p))));

        // Root signature, in the console compute layout: [0] four constants (s4..s7), [1] the table of views (s8).
        D3D12_DESCRIPTOR_RANGE view_range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0};
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 4};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &view_range};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        const auto root_blob = serialize_root_signature(root_desc);
        EXPECT(!root_blob.empty());
        Ref<ID3D12RootSignature> root;
        EXPECT(SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root.p;
        pso_desc.CS = {container.data(), container.size()};
        Ref<ID3D12PipelineState> pso;
        EXPECT(SUCCEEDED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso.p))));

        // The views: the two UAVs in a shader-visible heap (slot 0 the stencil plane, slot 1 the depth plane) and a
        // DSV. Heap starts come back in RAX (heap slots 9 and 10).
        const D3D12_DESCRIPTOR_HEAP_DESC view_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        const D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
                                                       0};
        Ref<ID3D12DescriptorHeap> view_heap, dsv_heap;
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&view_heap_desc, IID_PPV_ARGS(&view_heap.p))));
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&dsv_heap.p))));
        const std::uint64_t view_cpu = reinterpret_cast<XboxHeapHandleFn>(slot(view_heap.p, 9))(view_heap.p);
        const std::uint64_t view_gpu = reinterpret_cast<XboxHeapHandleFn>(slot(view_heap.p, 10))(view_heap.p);
        const std::uint64_t dsv_cpu = reinterpret_cast<XboxHeapHandleFn>(slot(dsv_heap.p, 9))(dsv_heap.p);
        const UINT view_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        EXPECT(view_cpu && dsv_cpu && view_gpu >= 0x10000 && view_gpu < (1ull << 32) && view_step % 4 == 0);

        // 2. The UAVs: of the twins, in their colour formats.
        D3D12_UNORDERED_ACCESS_VIEW_DESC stencil_uav{};
        stencil_uav.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        stencil_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        stencil_uav.Texture2D = {0, 1};
        D3D12_UNORDERED_ACCESS_VIEW_DESC depth_uav{};
        depth_uav.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        depth_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        depth_uav.Texture2D = {0, 0};
        device->CreateUnorderedAccessView(depth.p, nullptr, &stencil_uav, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(view_cpu)});
        device->CreateUnorderedAccessView(depth.p, nullptr, &depth_uav,
                                          D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(view_cpu + view_step)});
        EXPECT(tsharp_format(view_cpu) == g::format_from_vulkan(VK_FORMAT_R8_UINT));
        EXPECT(tsharp_format(view_cpu + view_step) == g::format_from_vulkan(VK_FORMAT_R32_SFLOAT));
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle{SIZE_T(dsv_cpu)};
        device->CreateDepthStencilView(depth.p, &dsv, dsv_handle);

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        Ref<ID3D12CommandQueue> queue;
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        Ref<ID3D12Fence> fence;
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                                   IID_PPV_ARGS(&list.p))));
        EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
        UINT64 fence_value = 0;
        const auto execute = [&]() -> bool {
            if (list->Close() != S_OK) return false;
            ID3D12CommandList* lists[] = {list.p};
            queue->ExecuteCommandLists(1, lists);
            if (rt::d3d12::last_execute_result(queue.p) != leonida::ReplayResult::success) return false;
            if (FAILED(queue->Signal(fence.p, ++fence_value)) || FAILED(fence->SetEventOnCompletion(fence_value, nullptr)))
                return false;
            return SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, nullptr));
        };
        ID3D12DescriptorHeap* heaps[] = {view_heap.p};
        const auto bind = [&] {
            list->SetDescriptorHeaps(1, heaps);
            list->SetComputeRootSignature(root.p);
            list->SetPipelineState(pso.p);
            list->SetComputeRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{view_gpu});
        };
        const auto dispatch = [&](UINT view, UINT plane_mode, UINT columns, UINT seed) {
            const std::uint32_t constants[4] = {view * view_step / 4, plane_mode, columns, seed};
            list->SetComputeRoot32BitConstants(0, 4, constants, 0);
            list->Dispatch(kSize / 8, kSize / 8, 1);
        };
        const auto copy_out = [&] {
            for (UINT plane = 0; plane < 2; ++plane) {
                D3D12_TEXTURE_COPY_LOCATION dst{readback.p, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                dst.PlacedFootprint = {plane ? 0u : kDepthOffset,
                                       {plane ? DXGI_FORMAT_R8_TYPELESS : DXGI_FORMAT_R32_TYPELESS, kSize, kSize, 1,
                                        kPitch}};
                D3D12_TEXTURE_COPY_LOCATION src{depth.p, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                src.SubresourceIndex = plane;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
        };
        const auto read = [&](std::vector<std::uint8_t>& out) -> bool {
            void* mapped = nullptr;
            if (FAILED(readback->Map(0, nullptr, &mapped))) return false;
            std::memcpy(out.data(), mapped, out.size());
            readback->Unmap(0, nullptr);
            return true;
        };
        const D3D12_CLEAR_FLAGS both = D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL;

        // 3. List A: clear, into unordered access, the stencil plane's left half. List B: the depth plane's left
        // three quarters, out of unordered access, both planes copied out.
        list->ClearDepthStencilView(dsv_handle, both, 0.25f, 0x11, 0, nullptr);
        D3D12_RESOURCE_BARRIER to_uav =
            transition(depth.p, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &to_uav);
        bind();
        dispatch(0, 0, kSize / 2, 5);
        EXPECT(execute());
        bind();
        dispatch(1, 1, kSize * 3 / 4, 0);
        D3D12_RESOURCE_BARRIER to_copy =
            transition(depth.p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_copy);
        copy_out();
        EXPECT(execute());
        EXPECT(read(round1));
        after_round1 = leonida::depth_twin_counts();

        // 4. List C: a new clear, the stencil plane's left quarter, both planes copied out again.
        D3D12_RESOURCE_BARRIER to_depth =
            transition(depth.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        list->ResourceBarrier(1, &to_depth);
        list->ClearDepthStencilView(dsv_handle, both, 0.75f, 0x22, 0, nullptr);
        list->ResourceBarrier(1, &to_uav);
        bind();
        dispatch(0, 0, kSize / 4, 9);
        list->ResourceBarrier(1, &to_copy);
        copy_out();
        EXPECT(execute());
        EXPECT(read(round2));

        depth.reset();
        readback.reset();
        pso.reset();
        root.reset();
        view_heap.reset();
        dsv_heap.reset();
        list.reset();
        allocator.reset();
        queue.reset();
        fence.reset();
        EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 1);
    }
    const leonida::DepthTwinCounts after = leonida::depth_twin_counts();

    EXPECT(check(round1.data(), "lists A and B", kSize / 2, 5, 0x11, kSize * 3 / 4, 0.25f) == 0);
    EXPECT(check(round2.data(), "list C", kSize / 4, 9, 0x22, 0, 0.75f) == 0);
    // Each round copied the depth texture into its twins once (into unordered access, after the clear) and the twins
    // back once (out of it), both planes each time.
    EXPECT(after_round1.to_twin - before.to_twin == 1 && after_round1.to_depth - before.to_depth == 1);
    EXPECT(after.to_twin - before.to_twin == 2 && after.to_depth - before.to_depth == 2);
    const std::uint64_t buffered = after.through_buffer - before.through_buffer;
    EXPECT(buffered == (through_buffer ? 4u : 0u));
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("d3d12_depth_twin_test: OK, a 64x64 R32G8X24 depth texture's stencil and depth planes written "
                "through UAVs (its twins) and read back by copies, two rounds; 2 copies into the twins and 2 back, %s\n",
                through_buffer ? "through a buffer" : "with vkCmdCopyImage (VK_KHR_maintenance8)");
    return 0;
}
