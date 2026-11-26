// Rectangle lists through the Xbox-ABI proxy over the Vulkan device (src/rect_list_gs.h, ported from VIctory,
// GPL-3.0-or-later, Marvisak and contributors, DexNom fork: victory/crates/victory-shader/src/emit/rect.rs).
//
// A pipeline of topology type 19 (D3D12.X's rectangle lists) draws, in one DrawInstanced(9) with list topology 17,
// three rectangles of three vertices each, one per quadrant of a 64x64 target cleared green:
//   - top left: top left, bottom left, bottom right (the corner second, as the title's full-screen passes);
//   - top right: top left, bottom left, top right (the corner first, as its tiled lighting);
//   - bottom left: bottom left, top right, top left (the corner third).
// Each vertex's colour is (u, 0, v) over its rectangle, so the colour of the fourth corner's half is the generated
// stage's continuation of the three corners: every pixel of the three quadrants must hold (255u, 0, 255v), with no
// green left. The same three vertices of the bottom-right quadrant drawn by a triangle pipeline cover only the half
// below the diagonal (the other half stays green), which is what a rectangle pipeline drew without the stage. Every
// corner order runs counter-clockwise on the screen, which both pipelines take as the front while culling back faces,
// so a second triangle turned the other way would be missing.
// Shaders: test/glsl/vs_draw.vert (glslang's gl_PerVertex block: the stage mirrors it) and ps_draw.frag, as in
// d3d12_draw_test. Skipped where the GPU lacks geometryShader. No game data. Set LEONIDA_VK_VALIDATION=1 to run under
// VK_LAYER_KHRONOS_validation.
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/rect_list_gs.h"
#include "../src/shader_library.h"

#include <array>
#include <cstdlib>
#include <cstring>

#if !defined(LEONIDA_TEST_VS_SPV) || !defined(LEONIDA_TEST_PS_SPV)
#error "LEONIDA_TEST_VS_SPV and LEONIDA_TEST_PS_SPV must name the compiled test shaders"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slot 9: handle in RAX

namespace {

struct Vertex {
    float x, y;
    std::uint8_t r, g, b, a;
};

// A corner of the quadrant whose top-left corner is (left, top) in clip space, at (u, v) across it (v downwards).
Vertex corner(float left, float top, int u, int v) {
    return {left + float(u), top - float(v), std::uint8_t(255 * u), 0, std::uint8_t(255 * v), 255};
}

}  // namespace

int main() {
    vktest::Device d;
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) return 77;  // no Vulkan GPU (hosted CI): ctest skips
    EXPECT(device_status == vktest::DeviceStatus::ready);
    if (!d.enabled.geometryShader) {
        std::printf("SKIP: no geometryShader on this GPU\n");
        return 77;  // ctest's SKIP_RETURN_CODE
    }

    const auto vs_program = fssm_program({0x56530001, 0xbf810000});
    const auto ps_program = fssm_program({0x50530001, 0xbf810000});
    const auto vs_container = shader_container(vs_program), ps_container = shader_container(ps_program);
    const auto library = std::filesystem::temp_directory_path() / "leonida_rect_list_test";
    std::filesystem::create_directories(library);
    const auto overwrite = std::filesystem::copy_options::overwrite_existing;
    std::filesystem::copy_file(LEONIDA_TEST_VS_SPV, library / "vs_draw.spv", overwrite);
    std::filesystem::copy_file(LEONIDA_TEST_PS_SPV, library / "ps_draw.spv", overwrite);
    {
        std::ofstream index(library / "index.json", std::ios::binary);
        index << "{\"vs_draw\": {\"stage\": \"vs\", \"fssm_sha256\": \"" << leonida::sha256_hex(vs_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"input_signature\": ["
                 "{\"semantic\": \"COLOR\", \"semantic_index\": 0, \"register\": 0, \"mask\": 15, \"rw_mask\": 15, "
                 "\"system_value\": 0, \"component_type\": \"float\"}, "
                 "{\"semantic\": \"POSITION\", \"semantic_index\": 0, \"register\": 1, \"mask\": 3, \"rw_mask\": 3, "
                 "\"system_value\": 0, \"component_type\": \"float\"}]},\n"
                 " \"ps_draw\": {\"stage\": \"ps\", \"fssm_sha256\": \"" << leonida::sha256_hex(ps_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"outputs\": [0]}}";
    }

    constexpr UINT size = 64, half = size / 2;
    std::vector<std::uint8_t> pixels(size * 256);
    {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        info.buffer_device_address = true;
        info.features = d.enabled;
        info.subgroup = d.subgroup;
        info.shader_dir = library.string();
        ID3D12Device* inner = nullptr;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        Ref<ID3D12Device> device;
        EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                           reinterpret_cast<void**>(&device.p))));
        rt::d3d12::set_device_outer(inner, device.p);

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 2};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor = {1, 0};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        const auto root_blob = serialize_root_signature(root_desc);
        EXPECT(!root_blob.empty());
        Ref<ID3D12RootSignature> root;
        EXPECT(SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));

        const D3D12_INPUT_ELEMENT_DESC elements[2] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root.p;
        pso_desc.VS = {vs_container.data(), vs_container.size()};
        pso_desc.PS = {ps_container.data(), ps_container.size()};
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.SampleMask = 0xffffffff;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        // The corners below run counter-clockwise on the screen: that is the front, and back faces are culled.
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        pso_desc.RasterizerState.FrontCounterClockwise = TRUE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.InputLayout = {elements, 2};
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.SampleDesc.Count = 1;
        Ref<ID3D12PipelineState> rects, triangles;
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(leonida::rect_list::kPipelineTopologyType);
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&rects.p))));
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&triangles.p))));

        const Vertex vertices[12] = {
            corner(-1, 1, 0, 0), corner(-1, 1, 0, 1), corner(-1, 1, 1, 1),  // top left: corner second
            corner(0, 1, 0, 0),  corner(0, 1, 0, 1),  corner(0, 1, 1, 0),   // top right: corner first
            corner(-1, 0, 0, 1), corner(-1, 0, 1, 0), corner(-1, 0, 0, 0),  // bottom left: corner third
            corner(0, 0, 0, 0),  corner(0, 0, 0, 1),  corner(0, 0, 1, 1),   // bottom right, as a triangle
        };
        const float tint[3] = {1, 0, 1};  // the pixel shader writes (r, 0, b, 1)

        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        Ref<ID3D12Resource> upload, readback, target;
        const auto upload_desc = buffer_desc(4096);
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&upload.p))));
        const auto readback_desc = buffer_desc(size * 256);
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&readback.p))));
        D3D12_RESOURCE_DESC rt_desc{};
        rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_desc.Width = rt_desc.Height = size;
        rt_desc.DepthOrArraySize = 1;
        rt_desc.MipLevels = 1;
        rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rt_desc.SampleDesc.Count = 1;
        rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                                         D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                         IID_PPV_ARGS(&target.p))));
        std::uint8_t* up = nullptr;
        EXPECT(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&up))));
        std::memcpy(up, vertices, sizeof(vertices));
        std::memcpy(up + 512, tint, sizeof(tint));
        const D3D12_GPU_VIRTUAL_ADDRESS upload_va = upload->GetGPUVirtualAddress();

        const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
                                                   0};
        Ref<ID3D12DescriptorHeap> rtv_heap;
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap.p))));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv{
            SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(rtv_heap.p, 9))(rtv_heap.p))};
        device->CreateRenderTargetView(target.p, nullptr, rtv);

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        Ref<ID3D12CommandQueue> queue;
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        Ref<ID3D12Fence> fence;
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, rects.p,
                                                   IID_PPV_ARGS(&list.p))));
        EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));

        const FLOAT green[4] = {0, 1, 0, 1};
        list->ClearRenderTargetView(rtv, green, 0, nullptr);
        list->SetGraphicsRootSignature(root.p);
        const D3D12_VERTEX_BUFFER_VIEW vbv{upload_va, sizeof(vertices), sizeof(Vertex)};
        list->IASetVertexBuffers(0, 1, &vbv);
        const D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
        const D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float no_offset[2] = {0, 0};
        list->SetGraphicsRoot32BitConstants(0, 2, no_offset, 0);
        list->SetGraphicsRootConstantBufferView(1, upload_va + 512);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY(leonida::rect_list::kListTopology));
        list->DrawInstanced(9, 1, 0, 0);  // three rectangles
        list->SetPipelineState(triangles.p);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 9, 0);  // the same corners as one triangle

        D3D12_RESOURCE_BARRIER to_copy = transition(target.p, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                    D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_copy);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.p;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, size, size, 1, 256}};
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = target.p;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        EXPECT(list->Close() == S_OK);
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
        EXPECT(SUCCEEDED(queue->Signal(fence.p, 1)));
        EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, nullptr)));
        void* mapped = nullptr;
        EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
        std::memcpy(pixels.data(), mapped, pixels.size());

        for (auto* r : {&upload, &readback, &target}) r->reset();
        rects.reset();
        triangles.reset();
        root.reset();
        rtv_heap.reset();
        list.reset();
        allocator.reset();
        queue.reset();
        fence.reset();
        EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 1);
    }
    std::filesystem::remove_all(library);

    unsigned mismatches = 0, covered = 0, left_green = 0;
    for (UINT row = 0; row < size; ++row) {
        for (UINT col = 0; col < size; ++col) {
            const UINT u = col % half, v = row % half;
            const bool triangle = col >= half && row >= half;
            const std::uint8_t* got = pixels.data() + row * 256 + col * 4;
            if (triangle && u == v) continue;  // on the triangle's diagonal: the fill rule decides
            const bool drawn = !triangle || u < v;
            std::array<int, 4> want{0, 255, 0, 255};
            if (drawn)
                want = {int(255.0f * (float(u) + 0.5f) / float(half) + 0.5f), 0,
                        int(255.0f * (float(v) + 0.5f) / float(half) + 0.5f), 255};
            bool ok = true;
            for (int c = 0; c < 4; ++c) ok &= std::abs(int(got[c]) - want[c]) <= 2;
            covered += !triangle && got[1] == 0;
            left_green += triangle && !drawn && got[1] == 255;
            if (ok) continue;
            if (mismatches++ < 8)
                std::fprintf(stderr, "pixel (%u, %u): got %u,%u,%u,%u want %d,%d,%d,%d\n", col, row, got[0], got[1],
                             got[2], got[3], want[0], want[1], want[2], want[3]);
        }
    }
    if (mismatches) {  // a map of the target, a character per 4x4 pixels: '.' green, '#' drawn
        std::fprintf(stderr, "%u pixels wrong; the target:\n", mismatches);
        for (UINT row = 2; row < size; row += 4) {
            for (UINT col = 2; col < size; col += 4) std::fputc(pixels[row * 256 + col * 4 + 1] ? '.' : '#', stderr);
            std::fputc('\n', stderr);
        }
    }
    EXPECT(!mismatches);
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("rect lists: three rectangles (corner first, second and third) drawn whole by the generated geometry "
                "stage, %u of %u pixels with the colour continued into the fourth corner; the triangle pipeline left "
                "%u pixels of its rectangle undrawn\n",
                covered, 3 * half * half, left_green);
    return 0;
}
