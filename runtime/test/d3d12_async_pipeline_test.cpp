// GPU test of pipelines made off the creating thread (src/pipeline_queue.h, after VIctory's pipeline/make.rs and
// pipeline/library.rs):
//   1. Before the frame clock's first frame (as through the PC interface) a bind waits for its pipeline, so a draw
//      right after creation runs.
//   2. Once frames run: many PSOs are created at once and none is made at creation; with the builder held, each bind
//      waits within the frame's budget (one window per frame and kind) and every draw and dispatch is dropped and
//      counted, the list staying valid; once they are made, the next frame's draws all run.
//   3. The early list: the PSOs a run bound are written to the list; read back as the next run reads it, the same
//      descriptions are started at creation without a bind, and one that was not bound is not.
//   4. Console device slot 86 (CreatePipelineState from a stream) reaches the layer's stream PSO.
// Shaders: the draw test's VS/PS and the compute test's CS, under synthetic FSSM keys. No game data.
#include "shader_fixture_path.h"
#include "test_temp_dir.h"
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/pipeline_queue.h"
#include "../src/shader_library.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if !defined(LEONIDA_TEST_CS_SPV) || !defined(LEONIDA_TEST_VS_SPV) || !defined(LEONIDA_TEST_PS_SPV)
#error "LEONIDA_TEST_CS_SPV, LEONIDA_TEST_VS_SPV and LEONIDA_TEST_PS_SPV must be defined"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "d3d12_async_pipeline_test line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;
using namespace std::chrono_literals;
namespace pipelines = leonida::pipelines;

namespace {

struct Vertex {
    float x, y;
    std::uint8_t r, g, b, a;
};

D3D12_RESOURCE_DESC tex2d_desc(UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc = {1, 0};
    d.Flags = flags;
    return d;
}

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// A subobject of a pipeline state stream as d3dx12 lays it out: its type, the description at its own alignment, the
// next subobject at pointer alignment.
template<class T>
void append(std::vector<std::uint8_t>& stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const T& value) {
    const auto round = [](std::size_t v, std::size_t a) { return (v + a - 1) / a * a; };
    const std::size_t at = stream.size();
    const std::size_t payload = round(at + sizeof(type), alignof(T));
    stream.resize(round(payload + sizeof(T), sizeof(void*)), 0);
    std::memcpy(stream.data() + at, &type, sizeof(type));
    std::memcpy(stream.data() + payload, &value, sizeof(T));
}

std::size_t count_draws(ID3D12GraphicsCommandList* list, bool dispatch) {
    std::vector<leonida::Command> commands;
    if (!rt::d3d12::recorded_commands(list, commands)) return ~std::size_t(0);
    std::size_t n = 0;
    for (const auto& c : commands) n += dispatch ? std::holds_alternative<leonida::Dispatch>(c) : std::holds_alternative<leonida::Draw>(c);
    return n;
}

}  // namespace

int main() {
    vktest::Device d;
    const auto status = d.initialize();
    if (status == vktest::DeviceStatus::unavailable) {
        std::fprintf(stderr, "SKIP: no usable Vulkan 1.3 device\n");
        return 77;
    }
    EXPECT(status == vktest::DeviceStatus::ready);

    const auto cs_prog = fssm_program({0x43532001, 0xbf810000});
    const auto vs_prog = fssm_program({0x56532001, 0xbf810000});
    const auto ps_prog = fssm_program({0x50532001, 0xbf810000});
    const auto cs_dxbc = shader_container(cs_prog);
    const auto vs_dxbc = shader_container(vs_prog);
    const auto ps_dxbc = shader_container(ps_prog);

    const testfiles::ScratchDir scratch("leonida_async_pipeline_test");
    const auto& lib_dir = scratch.path();
    EXPECT(testfiles::copy_file(vktest::shader_fixture_path(LEONIDA_TEST_CS_SPV), lib_dir / "cs.spv"));
    EXPECT(testfiles::copy_file(vktest::shader_fixture_path(LEONIDA_TEST_VS_SPV), lib_dir / "vs.spv"));
    EXPECT(testfiles::copy_file(vktest::shader_fixture_path(LEONIDA_TEST_PS_SPV), lib_dir / "ps.spv"));
    {
        std::ofstream idx(lib_dir / "index.json", std::ios::binary);
        idx << "{\"cs\": {\"stage\": \"cs\", \"fssm_sha256\": \"" << leonida::sha256_hex(cs_prog)
            << "\", \"bindings\": [], \"push_constant_bytes\": 144},\n"
               " \"vs\": {\"stage\": \"vs\", \"fssm_sha256\": \"" << leonida::sha256_hex(vs_prog)
            << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"input_signature\": ["
               "{\"semantic\": \"COLOR\", \"semantic_index\": 0, \"register\": 0, \"mask\": 15, \"rw_mask\": 15, "
               "\"system_value\": 0, \"component_type\": \"float\"}, "
               "{\"semantic\": \"POSITION\", \"semantic_index\": 0, \"register\": 1, \"mask\": 3, \"rw_mask\": 3, "
               "\"system_value\": 0, \"component_type\": \"float\"}]},\n"
               " \"ps\": {\"stage\": \"ps\", \"fssm_sha256\": \"" << leonida::sha256_hex(ps_prog)
            << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"outputs\": [0]}}";
    }
    // Before any PSO: the list goes to the scratch folder, written whatever the frame clock does.
    const std::filesystem::path early_file = lib_dir / "early-pipelines.txt";
    pipelines::EarlyList::get().configure(early_file, true, true);

    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = d.instance_proc;
    info.instance = d.instance;
    info.physical = d.physical;
    info.device = d.device;
    info.queue = d.queue;
    info.queue_family = d.family;
    info.buffer_device_address = true;
    info.shader_dir = lib_dir.string();
    info.features = d.enabled;  // as the runtime's own device: the store features where the GPU has them
    Ref<ID3D12Device2> device;
    EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&device.p))));
    if (rt::d3d12::device_push_constants_need_fallback(device.p)) {
        std::printf("d3d12_async_pipeline_test: skipped, the PSOs need 144 push-constant bytes\n");
        return 77;
    }

    D3D12_DESCRIPTOR_RANGE uav_range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, 0};
    D3D12_ROOT_PARAMETER cs_params[3]{};
    cs_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    cs_params[0].Constants = {0, 0, 1};
    cs_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    cs_params[1].Descriptor = {0, 0};
    cs_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    cs_params[2].DescriptorTable = {1, &uav_range};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC cs_rs_desc{};
    cs_rs_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    cs_rs_desc.Desc_1_0 = {3, cs_params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    const auto cs_rs_blob = serialize_root_signature(cs_rs_desc);
    Ref<ID3D12RootSignature> cs_rs;
    EXPECT(SUCCEEDED(device->CreateRootSignature(0, cs_rs_blob.data(), cs_rs_blob.size(), IID_PPV_ARGS(&cs_rs.p))));
    D3D12_COMPUTE_PIPELINE_STATE_DESC cs_desc{};
    cs_desc.pRootSignature = cs_rs.p;
    cs_desc.CS = {cs_dxbc.data(), cs_dxbc.size()};

    D3D12_ROOT_PARAMETER gfx_params[2]{};
    gfx_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    gfx_params[0].Constants = {0, 0, 2};
    gfx_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    gfx_params[1].Descriptor = {1, 0};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC gfx_rs_desc{};
    gfx_rs_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    gfx_rs_desc.Desc_1_0 = {2, gfx_params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    const auto gfx_rs_blob = serialize_root_signature(gfx_rs_desc);
    Ref<ID3D12RootSignature> gfx_rs;
    EXPECT(SUCCEEDED(device->CreateRootSignature(0, gfx_rs_blob.data(), gfx_rs_blob.size(), IID_PPV_ARGS(&gfx_rs.p))));
    const D3D12_INPUT_ELEMENT_DESC input_elems[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gfx_desc{};
    gfx_desc.pRootSignature = gfx_rs.p;
    gfx_desc.VS = {vs_dxbc.data(), vs_dxbc.size()};
    gfx_desc.PS = {ps_dxbc.data(), ps_dxbc.size()};
    gfx_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gfx_desc.SampleMask = 0xFFFFFFFFu;
    gfx_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gfx_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    gfx_desc.RasterizerState.DepthClipEnable = TRUE;
    gfx_desc.InputLayout = {input_elems, 2};
    gfx_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gfx_desc.NumRenderTargets = 1;
    gfx_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gfx_desc.SampleDesc = {1, 0};
    // PSO number i: its own description (a depth bias, which changes nothing without a depth target), so its own
    // pipeline and its own key.
    const auto make_pso = [&](int i, ID3D12PipelineState** out) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = gfx_desc;
        desc.RasterizerState.DepthBias = i;
        return device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(out));
    };

    Ref<ID3D12CommandQueue> queue;
    const D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    EXPECT(SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue.p))));
    Ref<ID3D12CommandAllocator> alloc;
    EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc.p))));
    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(SUCCEEDED(
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.p, nullptr, IID_PPV_ARGS(&list.p))));
    EXPECT(SUCCEEDED(list->Close()));
    Ref<ID3D12Fence> fence;
    EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
    UINT64 fence_value = 0;

    constexpr UINT W = 16, H = 16;
    const D3D12_HEAP_PROPERTIES hp_def{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_HEAP_PROPERTIES hp_up{D3D12_HEAP_TYPE_UPLOAD};
    const D3D12_HEAP_PROPERTIES hp_rb{D3D12_HEAP_TYPE_READBACK};
    const auto rtd = tex2d_desc(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const Vertex verts[4] = {{-1.0f, 1.0f, 255, 0, 0, 255}, {0.0f, 1.0f, 255, 0, 0, 255},
                             {0.0f, 0.0f, 255, 0, 0, 255}, {-1.0f, 0.0f, 255, 0, 0, 255}};
    const std::uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    const float tint[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    const auto vbd = buffer_desc(sizeof(verts));
    const auto ibd = buffer_desc(sizeof(indices));
    const auto cbd = buffer_desc(256);
    Ref<ID3D12Resource> rt, vb, ib, cb, rb;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &rtd,
                                                     D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt.p))));
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&hp_up, D3D12_HEAP_FLAG_NONE, &vbd,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vb.p))));
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&hp_up, D3D12_HEAP_FLAG_NONE, &ibd,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ib.p))));
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&hp_up, D3D12_HEAP_FLAG_NONE, &cbd,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cb.p))));
    void* p = nullptr;
    EXPECT(SUCCEEDED(vb->Map(0, nullptr, &p)));
    std::memcpy(p, verts, sizeof(verts));
    vb->Unmap(0, nullptr);
    EXPECT(SUCCEEDED(ib->Map(0, nullptr, &p)));
    std::memcpy(p, indices, sizeof(indices));
    ib->Unmap(0, nullptr);
    EXPECT(SUCCEEDED(cb->Map(0, nullptr, &p)));
    std::memcpy(p, tint, sizeof(tint));
    cb->Unmap(0, nullptr);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(&rtd, 0, 1, 0, &fp, nullptr, nullptr, &total_bytes);
    const auto rbd = buffer_desc(total_bytes);
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&hp_rb, D3D12_HEAP_FLAG_NONE, &rbd,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb.p))));
    Ref<ID3D12DescriptorHeap> rtv_heap;
    const D3D12_DESCRIPTOR_HEAP_DESC rhd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap.p))));
    const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(rt.p, nullptr, rtv);

    // One frame's list: a blue clear, then a red quad with each PSO; the colour at the quad comes back.
    const auto draw_with = [&](const std::vector<ID3D12PipelineState*>& psos, std::uint32_t& colour,
                               std::size_t& recorded) -> bool {
        if (FAILED(alloc->Reset()) || FAILED(list->Reset(alloc.p, nullptr))) return false;
        const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        list->ClearRenderTargetView(rtv, blue, 0, nullptr);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT vp{0, 0, float(W), float(H), 0.0f, 1.0f};
        const D3D12_RECT sc{0, 0, LONG(W), LONG(H)};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VERTEX_BUFFER_VIEW vbv{vb->GetGPUVirtualAddress(), sizeof(verts), sizeof(Vertex)};
        const D3D12_INDEX_BUFFER_VIEW ibv{ib->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R16_UINT};
        list->IASetVertexBuffers(0, 1, &vbv);
        list->IASetIndexBuffer(&ibv);
        list->SetGraphicsRootSignature(gfx_rs.p);
        const float offset[2] = {0.0f, 0.0f};
        list->SetGraphicsRoot32BitConstants(0, 2, offset, 0);
        list->SetGraphicsRootConstantBufferView(1, cb->GetGPUVirtualAddress());
        for (ID3D12PipelineState* pso : psos) {
            list->SetPipelineState(pso);
            list->DrawIndexedInstanced(6, 1, 0, 0, 0);
        }
        recorded = count_draws(list.p, false);
        const auto to_src = transition(rt.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const auto to_rt = transition(rt.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &to_src);
        D3D12_TEXTURE_COPY_LOCATION src_loc{rt.p, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        D3D12_TEXTURE_COPY_LOCATION dst_loc{rb.p, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dst_loc.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
        list->ResourceBarrier(1, &to_rt);
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        if (rt::d3d12::last_execute_result(queue.p) != leonida::ReplayResult::success) return false;
        if (FAILED(queue->Signal(fence.p, ++fence_value)) || FAILED(fence->SetEventOnCompletion(fence_value, nullptr)))
            return false;
        void* mapped = nullptr;
        if (FAILED(rb->Map(0, nullptr, &mapped))) return false;
        std::memcpy(&colour, static_cast<const std::uint8_t*>(mapped) + 2 * fp.Footprint.RowPitch + 2 * 4, 4);
        rb->Unmap(0, nullptr);
        return true;
    };
    constexpr std::uint32_t kRed = 0xFF0000FFu, kBlue = 0xFFFF0000u;
    std::uint32_t colour = 0;
    std::size_t recorded = 0;

    // 1. No frame yet: creation returns without the pipeline, the bind waits for it, the draw runs.
    EXPECT(pipelines::current_frame() == 0);
    {
        Ref<ID3D12PipelineState> pso;
        const auto t = std::chrono::steady_clock::now();
        EXPECT(SUCCEEDED(make_pso(0, &pso.p)));
        const double created = ms_since(t);
        const pipelines::Job* job = rt::d3d12::pipeline_job(pso.p);
        EXPECT(job && job->queue_state() == pipelines::kNotQueued && !job->finished());
        const pipelines::DropCounts before = pipelines::Drops::get().counts();
        EXPECT(draw_with({pso.p}, colour, recorded));
        EXPECT(recorded == 1 && colour == kRed && job->ready());
        EXPECT(pipelines::Drops::get().counts().draws == before.draws);
        std::printf("d3d12_async_pipeline_test: before the first frame, creation took %.2f ms, the bind waited for the "
                    "pipeline (made in %.1f ms) and the draw ran\n", created,
                    std::chrono::duration<double, std::milli>(job->took()).count());
    }

    // 2. Frames run: many PSOs at once, all dropped while they are being made, all drawn once made.
    constexpr int kMany = 192;
    pipelines::begin_frame();
    pipelines::set_wait_budget(40ms);
    pipelines::Builder::get().hold(true);
    std::vector<Ref<ID3D12PipelineState>> many(kMany);
    std::vector<ID3D12PipelineState*> raw;
    auto t = std::chrono::steady_clock::now();
    for (int i = 0; i < kMany; ++i) EXPECT(SUCCEEDED(make_pso(i + 1, &many[i].p)));
    const double created_ms = ms_since(t);
    for (auto& pso : many) {
        raw.push_back(pso.p);
        EXPECT(!rt::d3d12::pipeline_job(pso.p)->finished());
    }
    Ref<ID3D12PipelineState> cs_pso;
    EXPECT(SUCCEEDED(device->CreateComputePipelineState(&cs_desc, IID_PPV_ARGS(&cs_pso.p))));
    const pipelines::DropCounts before = pipelines::Drops::get().counts();
    t = std::chrono::steady_clock::now();
    EXPECT(draw_with(raw, colour, recorded));
    const double frame_ms = ms_since(t);
    EXPECT(recorded == 0 && colour == kBlue);
    pipelines::DropCounts after = pipelines::Drops::get().counts();
    EXPECT(after.draws == before.draws + kMany);
    // The frame's graphics window is used up; a compute bind has its own, and its dispatch is dropped too.
    EXPECT(SUCCEEDED(alloc->Reset()) && SUCCEEDED(list->Reset(alloc.p, nullptr)));
    list->SetComputeRootSignature(cs_rs.p);
    t = std::chrono::steady_clock::now();
    list->SetPipelineState(cs_pso.p);
    const double compute_wait_ms = ms_since(t);
    list->Dispatch(1, 1, 1);
    EXPECT(count_draws(list.p, true) == 0 && SUCCEEDED(list->Close()));
    after = pipelines::Drops::get().counts();
    EXPECT(after.dispatches == before.dispatches + 1);
    std::printf("d3d12_async_pipeline_test: %d graphics PSOs created in %.1f ms (%.0f us each), none made; their frame "
                "took %.0f ms with the builder held (budget 40 ms per frame and kind), all %d draws dropped; a compute "
                "bind then waited %.0f ms and its dispatch was dropped\n",
                kMany, created_ms, 1000.0 * created_ms / kMany, frame_ms, kMany, compute_wait_ms);
    EXPECT(frame_ms < 2000.0);
    EXPECT(compute_wait_ms >= 30.0);
    pipelines::Builder::get().hold(false);
    t = std::chrono::steady_clock::now();
    for (ID3D12PipelineState* pso : raw) EXPECT(const_cast<pipelines::Job*>(rt::d3d12::pipeline_job(pso))->wait_finished_for(60s));
    const double made_ms = ms_since(t);
    for (ID3D12PipelineState* pso : raw) EXPECT(rt::d3d12::pipeline_job(pso)->ready());
    EXPECT(const_cast<pipelines::Job*>(rt::d3d12::pipeline_job(cs_pso.p))->wait_finished_for(60s));
    pipelines::begin_frame();
    EXPECT(draw_with(raw, colour, recorded));
    EXPECT(recorded == std::size_t(kMany) && colour == kRed);
    EXPECT(pipelines::Drops::get().counts().draws == after.draws);
    EXPECT(SUCCEEDED(alloc->Reset()) && SUCCEEDED(list->Reset(alloc.p, nullptr)));
    list->SetComputeRootSignature(cs_rs.p);
    list->SetPipelineState(cs_pso.p);  // waits within the new frame's compute window, then records
    list->Dispatch(1, 1, 1);
    EXPECT(count_draws(list.p, true) == 1 && SUCCEEDED(list->Close()));
    std::printf("d3d12_async_pipeline_test: the %d pipelines were made in %.0f ms on the workers; the next frame drew "
                "all %d\n", kMany, made_ms, kMany);

    // 3. The early list: what this run bound is written; the next run starts it at creation, and not what it did not
    // bind.
    std::string keys[3];
    pipelines::begin_frame();
    pipelines::set_wait_budget(10s);  // the binds wait for these, so their draws run
    {
        Ref<ID3D12PipelineState> a, b, c;
        EXPECT(SUCCEEDED(make_pso(1001, &a.p)) && SUCCEEDED(make_pso(1002, &b.p)) && SUCCEEDED(make_pso(1003, &c.p)));
        keys[0] = rt::d3d12::pipeline_early_key(a.p);
        keys[1] = rt::d3d12::pipeline_early_key(b.p);
        keys[2] = rt::d3d12::pipeline_early_key(c.p);
        EXPECT(keys[0].size() == 32 && keys[0] != keys[1] && keys[1] != keys[2]);
        EXPECT(draw_with({a.p, b.p}, colour, recorded) && recorded == 2 && colour == kRed);
    }
    EXPECT(pipelines::EarlyList::get().save(true) || std::filesystem::exists(early_file));
    std::set<std::string> listed;
    {
        std::ifstream in(early_file, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) listed.insert(line);
    }
    EXPECT(listed.count(keys[0]) && listed.count(keys[1]) && !listed.count(keys[2]));
    EXPECT(listed.count(rt::d3d12::pipeline_early_key(raw.front())));
    std::printf("d3d12_async_pipeline_test: the early list holds %zu pipelines this run bound\n", listed.size());
    many.clear();
    raw.clear();
    pipelines::EarlyList::get().reload();  // as the next run reads it
    pipelines::Builder::get().hold(true);
    const std::uint64_t early_before = pipelines::Builder::get().stats().started_early;
    {
        Ref<ID3D12PipelineState> a, b, c;
        EXPECT(SUCCEEDED(make_pso(1001, &a.p)) && SUCCEEDED(make_pso(1002, &b.p)) && SUCCEEDED(make_pso(1003, &c.p)));
        const pipelines::Job* ja = rt::d3d12::pipeline_job(a.p);
        const pipelines::Job* jb = rt::d3d12::pipeline_job(b.p);
        const pipelines::Job* jc = rt::d3d12::pipeline_job(c.p);
        const bool workers = pipelines::worker_count() > 0;
        if (workers) {
            EXPECT(ja->queue_state() == pipelines::kEarly && jb->queue_state() == pipelines::kEarly);
            EXPECT(pipelines::Builder::get().stats().started_early == early_before + 2);
        }
        EXPECT(jc->queue_state() == pipelines::kNotQueued);
        pipelines::Builder::get().hold(false);
        EXPECT(const_cast<pipelines::Job*>(ja)->wait_finished_for(60s) && ja->ready());
        EXPECT(const_cast<pipelines::Job*>(jb)->wait_finished_for(60s) && jb->ready());
        EXPECT(jc->queue_state() == pipelines::kNotQueued && !jc->finished());  // never bound, so not started
        std::printf("d3d12_async_pipeline_test: read back, the 2 listed PSOs were made at creation without a bind; the "
                    "unlisted one was not started\n");
    }

    // 4. Console device slot 86: a stream reaches the layer's stream PSO, the same as ID3D12Device2's.
    {
        std::vector<std::uint8_t> stream;
        append(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, gfx_rs.p);
        append(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, gfx_desc.VS);
        append(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, gfx_desc.PS);
        append(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, gfx_desc.InputLayout);
        append(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, gfx_desc.PrimitiveTopologyType);
        D3D12_RT_FORMAT_ARRAY formats{};
        formats.NumRenderTargets = 1;
        formats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        append(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, formats);
        const D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{stream.size(), stream.data()};
        Ref<ID3D12PipelineState> pc;
        EXPECT(SUCCEEDED(device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&pc.p))));
        device->AddRef();  // the proxy takes this reference
        void* proxy = nullptr;
        EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(device.p, __uuidof(ID3D12Device11), &proxy)) && proxy);
        EXPECT(rt::d3d12_xs::device_proxy_console_layout(proxy));
        using CreateStreamFn = HRESULT(WINAPI*)(void*, const D3D12_PIPELINE_STATE_STREAM_DESC*, const IID*, void**);
        const IID iid = __uuidof(ID3D12PipelineState);
        void* console = nullptr;
        EXPECT(SUCCEEDED(reinterpret_cast<CreateStreamFn>(slot(proxy, 86))(proxy, &stream_desc, &iid, &console)) &&
               console);
        auto* console_pso = static_cast<ID3D12PipelineState*>(console);
        EXPECT(rt::d3d12::pipeline_job(console_pso) != nullptr);
        EXPECT(rt::d3d12::pipeline_early_key(console_pso) == rt::d3d12::pipeline_early_key(pc.p));
        EXPECT(rt::d3d12::make_pipeline_now(console_pso));
        console_pso->Release();
        // A console stream with no shader (the title makes such streams): the layer refuses it, the title still gets
        // a pipeline, and a draw with it is dropped and counted as refused.
        std::vector<std::uint8_t> empty;
        append(empty, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, gfx_rs.p);
        append(empty, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE{});
        append(empty, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE{});
        append(empty, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, formats);
        const D3D12_PIPELINE_STATE_STREAM_DESC empty_desc{empty.size(), empty.data()};
        Ref<ID3D12PipelineState> refused_pc;
        EXPECT(device->CreatePipelineState(&empty_desc, IID_PPV_ARGS(&refused_pc.p)) == E_INVALIDARG && !refused_pc.p);
        void* refused = nullptr;
        EXPECT(SUCCEEDED(reinterpret_cast<CreateStreamFn>(slot(proxy, 86))(proxy, &empty_desc, &iid, &refused)) &&
               refused);
        auto* refused_pso = static_cast<ID3D12PipelineState*>(refused);
        const pipelines::Job* refused_job = rt::d3d12::pipeline_job(refused_pso);
        EXPECT(refused_job && refused_job->finished() && !refused_job->ready());
        const std::uint64_t refused_before = pipelines::Drops::get().counts().refused;
        EXPECT(draw_with({refused_pso}, colour, recorded) && recorded == 0 && colour == kBlue);
        EXPECT(pipelines::Drops::get().counts().refused == refused_before + 1);
        refused_pso->Release();
        static_cast<IUnknown*>(proxy)->Release();
        std::printf("d3d12_async_pipeline_test: console slot 86 made the stream PSO, and handed a pipeline that is never "
                    "made for a stream without shaders (its draw dropped as refused)\n");
    }

    pipelines::set_wait_budget(std::chrono::milliseconds(pipelines::kDefaultWaitMs));
    EXPECT(d.validation_errors() == 0);
    std::printf("d3d12_async_pipeline_test: ok\n");
    return 0;
}
