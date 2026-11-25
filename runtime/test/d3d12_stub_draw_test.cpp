// LEONIDA_STUB_PIPELINES draws (MVP-GPU-04). With no shader library, a graphics PSO is created from the built-in
// stages in src/stub_pipeline.h as a real, registered Vulkan pipeline, and its draws run through replay:
//   - LEONIDA_STUB_PIPELINES=1: the vertex stage puts every vertex outside the clip volume, so a draw leaves both
//     render targets (R8G8B8A8_UNORM and R32_UINT, cleared to blue and 7) as they were;
//   - LEONIDA_STUB_PIPELINES=color: three vertices cover the viewport and the pixel stage writes leonida::stub_colour,
//     magenta to the UNORM target and 1 to the integer one (an integer output for an integer format);
//   - a point-list PSO (whose vertex stage must write PointSize) is created and drawn too.
// The PSO keeps an input layout and a bound vertex buffer the stub stages do not read. No game data, no shader
// library. Set LEONIDA_VK_VALIDATION=1 to run under VK_LAYER_KHRONOS_validation: the draws must raise no message.
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/shader_library.h"
#include "../src/stub_pipeline.h"

#include <cstdlib>
#include <cstring>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slot 9: handle in RAX

namespace {

D3D12_CPU_DESCRIPTOR_HANDLE heap_start(ID3D12DescriptorHeap* heap) {
    return {SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(heap, 9))(heap))};
}

constexpr UINT size = 64;
constexpr UINT pitch = 256;  // size * 4 bytes, already D3D12_TEXTURE_DATA_PITCH_ALIGNMENT

// Every pixel of both targets: RGBA8 `colour` and R32_UINT `value`.
bool targets_hold(const std::uint8_t* rgba, const std::uint8_t* uints, const std::uint8_t (&colour)[4],
                  std::uint32_t value, const char* what) {
    unsigned bad = 0;
    for (UINT row = 0; row < size; ++row) {
        for (UINT col = 0; col < size; ++col) {
            const std::uint8_t* got = rgba + row * pitch + col * 4;
            std::uint32_t word;
            std::memcpy(&word, uints + row * pitch + col * 4, 4);
            if (std::memcmp(got, colour, 4) == 0 && word == value) continue;
            if (bad++ < 4)
                std::fprintf(stderr, "%s: pixel (%u, %u) got %u,%u,%u,%u / %u, want %u,%u,%u,%u / %u\n", what, col, row,
                             got[0], got[1], got[2], got[3], word, colour[0], colour[1], colour[2], colour[3], value);
        }
    }
    return bad == 0;
}

}  // namespace

int main() {
    vktest::Device d;
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) return 77;  // no Vulkan GPU (hosted CI): ctest skips
    EXPECT(device_status == vktest::DeviceStatus::ready);

    // No shader library, whatever the environment says.
    SetEnvironmentVariableW(L"LEONIDA_SHADERS", nullptr);
    const auto vs_program = fssm_program({0x56530001, 0xbf810000});
    const auto ps_program = fssm_program({0x50530001, 0xbf810000});
    const auto vs_container = shader_container(vs_program), ps_container = shader_container(ps_program);

    std::vector<std::uint8_t> empty_pixels(2 * size * pitch), colour_pixels(2 * size * pitch);
    {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        info.buffer_device_address = true;
        ID3D12Device* inner = nullptr;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        Ref<ID3D12Device> device;
        EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                           reinterpret_cast<void**>(&device.p), /*console_lists=*/false)));
        rt::d3d12::set_device_outer(inner, device.p);
        if (rt::d3d12::device_push_constants_need_fallback(inner)) {
            std::printf("d3d12_stub_draw_test: skipped, the PSOs need 144 push-constant bytes and this GPU has %u\n",
                        rt::d3d12::device_max_push_constants_size(inner));
            return 77;
        }

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        const auto root_blob = serialize_root_signature(root_desc);
        EXPECT(!root_blob.empty());
        Ref<ID3D12RootSignature> root;
        EXPECT(SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));

        const D3D12_INPUT_ELEMENT_DESC elements[1] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root.p;
        pso_desc.VS = {vs_container.data(), vs_container.size()};
        pso_desc.PS = {ps_container.data(), ps_container.size()};
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.SampleMask = 0xffffffff;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.InputLayout = {elements, 1};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 2;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.RTVFormats[1] = DXGI_FORMAT_R32_UINT;
        pso_desc.SampleDesc = {1, 0};

        Ref<ID3D12PipelineState> off, empty, colour, points;
        _putenv_s("LEONIDA_STUB_PIPELINES", "");
        EXPECT(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&off.p)) == E_FAIL && !off.p);
        _putenv_s("LEONIDA_STUB_PIPELINES", "1");
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&empty.p))));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC point_desc = pso_desc;
        point_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&point_desc, IID_PPV_ARGS(&points.p))));
        _putenv_s("LEONIDA_STUB_PIPELINES", "color");
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&colour.p))));
        _putenv_s("LEONIDA_STUB_PIPELINES", "");
        for (ID3D12PipelineState* pso : {empty.p, points.p, colour.p}) {
            EXPECT(rt::d3d12::graphics_pipeline_id(pso) != 0);  // registered: SetPipelineState binds it
            EXPECT(rt::d3d12::compute_pipeline_id(pso) == 0);
        }

        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        Ref<ID3D12Resource> vertices, readback, rgba, uints;
        const auto vb_desc = buffer_desc(256);
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&vertices.p))));
        const auto readback_desc = buffer_desc(2 * size * pitch);
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&readback.p))));
        D3D12_RESOURCE_DESC rt_desc{};
        rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_desc.Width = rt_desc.Height = size;
        rt_desc.DepthOrArraySize = 1;
        rt_desc.MipLevels = 1;
        rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rt_desc.SampleDesc = {1, 0};
        rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                                         D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                         IID_PPV_ARGS(&rgba.p))));
        rt_desc.Format = DXGI_FORMAT_R32_UINT;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                                         D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                         IID_PPV_ARGS(&uints.p))));
        void* mapped_vertices = nullptr;
        EXPECT(SUCCEEDED(vertices->Map(0, nullptr, &mapped_vertices)));
        std::memset(mapped_vertices, 0, 256);  // never read by the stub stages

        const D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2,
                                                       D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        Ref<ID3D12DescriptorHeap> rtv_heap;
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap.p))));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {heap_start(rtv_heap.p),
                                                     {heap_start(rtv_heap.p).ptr + rt::d3d12::descriptor_size}};
        device->CreateRenderTargetView(rgba.p, nullptr, rtvs[0]);
        device->CreateRenderTargetView(uints.p, nullptr, rtvs[1]);

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

        // Clear, draw with each PSO in `draws`, and read both targets back into `pixels`.
        const auto run = [&](std::initializer_list<std::pair<ID3D12PipelineState*, D3D_PRIMITIVE_TOPOLOGY>> draws,
                             std::vector<std::uint8_t>& pixels) -> bool {
            if (fence_value && (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.p, nullptr)))) return false;
            const FLOAT blue[4] = {0, 0, 1, 1}, seven[4] = {7, 0, 0, 0};
            list->ClearRenderTargetView(rtvs[0], blue, 0, nullptr);
            list->ClearRenderTargetView(rtvs[1], seven, 0, nullptr);
            list->SetGraphicsRootSignature(root.p);
            const D3D12_VERTEX_BUFFER_VIEW vbv{vertices->GetGPUVirtualAddress(), 256, 8};
            list->IASetVertexBuffers(0, 1, &vbv);
            const D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
            const D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
            for (const auto& [pso, topology] : draws) {
                list->SetPipelineState(pso);
                list->IASetPrimitiveTopology(topology);
                list->DrawInstanced(topology == D3D_PRIMITIVE_TOPOLOGY_POINTLIST ? 4 : 3, 1, 0, 0);
            }
            ID3D12Resource* targets[2] = {rgba.p, uints.p};
            const DXGI_FORMAT formats[2] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_UINT};
            for (UINT i = 0; i < 2; ++i) {
                D3D12_RESOURCE_BARRIER to_copy = transition(targets[i], D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                            D3D12_RESOURCE_STATE_COPY_SOURCE);
                list->ResourceBarrier(1, &to_copy);
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = readback.p;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = {i * size * pitch, {formats[i], size, size, 1, pitch}};
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = targets[i];
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                D3D12_RESOURCE_BARRIER back = transition(targets[i], D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                         D3D12_RESOURCE_STATE_RENDER_TARGET);
                list->ResourceBarrier(1, &back);
            }
            if (list->Close() != S_OK) return false;
            ID3D12CommandList* lists[] = {list.p};
            queue->ExecuteCommandLists(1, lists);
            if (rt::d3d12::last_execute_result(queue.p) != leonida::ReplayResult::success) return false;
            if (FAILED(queue->Signal(fence.p, ++fence_value)) || FAILED(fence->SetEventOnCompletion(fence_value, nullptr)))
                return false;
            void* mapped = nullptr;
            if (FAILED(readback->Map(0, nullptr, &mapped))) return false;
            std::memcpy(pixels.data(), mapped, pixels.size());
            readback->Unmap(0, nullptr);
            return true;
        };
        EXPECT(run({{empty.p, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}, {points.p, D3D_PRIMITIVE_TOPOLOGY_POINTLIST}},
                   empty_pixels));
        EXPECT(run({{colour.p, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}}, colour_pixels));

        for (auto* r : {&vertices, &readback, &rgba, &uints}) r->reset();
        for (auto* p : {&empty, &points, &colour}) p->reset();
        root.reset();
        rtv_heap.reset();
        list.reset();
        allocator.reset();
        queue.reset();
        fence.reset();
        EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 1);
    }

    const std::uint8_t blue[4] = {0, 0, 255, 255};
    std::uint8_t magenta[4];
    for (int i = 0; i < 4; ++i) magenta[i] = std::uint8_t(leonida::stub_colour[i] * 255.0f);
    EXPECT(targets_hold(empty_pixels.data(), empty_pixels.data() + size * pitch, blue, 7, "LEONIDA_STUB_PIPELINES=1"));
    EXPECT(targets_hold(colour_pixels.data(), colour_pixels.data() + size * pitch, magenta, 1,
                        "LEONIDA_STUB_PIPELINES=color"));
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("stub pipelines: %u pixels per target; =1 drew nothing (triangles and points), =color drew magenta "
                "and integer 1 over the whole viewport\n",
                size * size);
    return 0;
}
