// A stand-in title for `gta6rt --run` that presents the way a GDKX title does, on the Vulkan device
// (check_tiny_present.py sets LEONIDA_D3D12=vulkan): D3D12XboxCreateDevice as ID3D12Device11 (the console layouts),
// the display mode from the console output (IDXGIDevice1 -> GetAdapter -> EnumOutputs(0) -> output slot 21), a back
// buffer of that size, the frame clock (device slots 78-80: SetFrameIntervalX at 60 Hz, ScheduleFrameEventX, a blocking
// WaitFrameEventX per frame), and per frame clears into the back buffer, ExecuteCommandLists (queue slot 10), Signal
// (16) and PresentX (32), through the console list and queue slots. The frame has three regions (the top-left quarter,
// the bottom-left quarter, the right half), so the checker can tell a presented frame from a flipped or scaled one.
//
// It presents until the screenshot the checker asked for (LEONIDA_CAPTURE_DIR, request 1) is on disk and at least 90
// frames are shown, then reads the caption of the window the frames went to ("Leonida | <fps> fps <ms> ms/frame").
// With --title-window it first makes a 3840x2160 window of its own through CreateWindowExA imported from the API set a
// title imports it from (the shim fits it to the screen) and pumps its messages, as a title does: the frames must then go
// there. Hand-written test code, public D3D12 and Win32 API only; no game data and no shader.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_2.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" __declspec(dllimport) HRESULT __stdcall D3D12XboxCreateDevice(IUnknown* adapter, const void* params,
                                                                        const IID* riid, void** device);

namespace {

int g_failures = 0;

bool check(bool ok, const char* what) {
    std::printf("tiny_present: %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
    return ok;
}

int finish() {
    std::printf("tiny_present: %s\n", g_failures ? "FAILED" : "OK");
    std::fflush(stdout);
    return g_failures ? 1 : 0;
}

void** vtable(void* object) { return *static_cast<void***>(object); }

// The console slots the stand-in calls (runtime/shims/d3d12_xs_slots.h, from VIctory's tables).
using Word = std::uint64_t;
template <class Fn>
Fn slot(void* object, unsigned index) {
    return reinterpret_cast<Fn>(vtable(object)[index]);
}
constexpr unsigned kDeviceSetFrameInterval = 78, kDeviceScheduleFrameEvent = 79, kDeviceWaitFrameEvent = 80;
constexpr unsigned kQueueExecute = 10, kQueueSignal = 16, kQueuePresentX = 32;
constexpr unsigned kListClose = 9, kListReset = 10, kListBarrier = 26, kListClearRtv = 52;
constexpr unsigned kOutputModes = 21;

// D3D12XBOX_PRESENT_PLANE_PARAMETERS (runtime/shims/xbox_present.h: 0x48 bytes on x64).
struct PresentPlane {
    std::uint64_t token;
    UINT resource_count;
    ID3D12Resource* const* resources;
    const D3D12_RECT* source_rects;
    const void* placement;
    int color_space;
    int scale_filter;
    UINT extended_count;
    const void* extended;
    UINT flags;
};
static_assert(sizeof(PresentPlane) == 0x48, "the console's plane parameters");

struct Mode {  // the console output's slot-21 entries (runtime/shims/xbox_display.h)
    std::uint32_t width, height, refresh_numerator, refresh_denominator, format, scanlines, scaling, flags;
};

using HeapStartFn = std::uint64_t(__stdcall*)(void* heap);

LRESULT CALLBACK title_proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    return DefWindowProcA(hwnd, message, w, l);
}

bool interactive_window_station() {
    HWINSTA station = GetProcessWindowStation();
    USEROBJECTFLAGS flags{};
    DWORD bytes = 0;
    if (!station || !GetUserObjectInformationW(station, UOI_FLAGS, &flags, sizeof(flags), &bytes)) return true;
    return (flags.dwFlags & WSF_VISIBLE) != 0;
}

void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    slot<void(__stdcall*)(void*, UINT, const D3D12_RESOURCE_BARRIER*)>(list, kListBarrier)(list, 1, &b);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool title_window = argc > 1 && !std::strcmp(argv[1], "--title-window");
    if (!interactive_window_station()) {
        std::printf("tiny_present: SKIP no interactive window station\n");
        return 77;
    }

    HWND own = nullptr;
    if (title_window) {
        WNDCLASSEXA window_class{sizeof(window_class)};
        window_class.lpfnWndProc = title_proc;
        window_class.hInstance = GetModuleHandleA(nullptr);
        window_class.lpszClassName = "TinyPresentTitle";
        RegisterClassExA(&window_class);
        own = CreateWindowExA(0, "TinyPresentTitle", "tiny_present", WS_OVERLAPPEDWINDOW, 0, 0, 3840, 2160, nullptr,
                              nullptr, window_class.hInstance, nullptr);
        if (!check(own != nullptr, "CreateWindowExA (through the title's API set) makes the title's window"))
            return finish();
        ShowWindow(own, SW_SHOW);
    }
    const auto pump = [] {
        MSG message;
        while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    };

    std::uint32_t params[24] = {};
    const IID device_iid = __uuidof(ID3D12Device11);
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12XboxCreateDevice(nullptr, params, &device_iid, reinterpret_cast<void**>(&device));
    if (!check(hr == S_OK && device, "D3D12XboxCreateDevice returns an ID3D12Device11 (console layouts)")) {
        std::printf("tiny_present: hr=0x%08lx\n", static_cast<unsigned long>(hr));
        return finish();
    }

    // The display mode, as the title finds it.
    UINT width = 0, height = 0;
    {
        IDXGIDevice1* dxgi = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIOutput* output = nullptr;
        check(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && dxgi, "the device answers IDXGIDevice1");
        if (dxgi) check(SUCCEEDED(dxgi->GetAdapter(&adapter)) && adapter, "GetAdapter");
        if (adapter) check(SUCCEEDED(adapter->EnumOutputs(0, &output)) && output, "EnumOutputs(0)");
        if (output) {
            using ModesFn = HRESULT(__stdcall*)(void*, UINT, UINT, UINT*, Mode*);
            UINT count = 0;
            check(slot<ModesFn>(output, kOutputModes)(output, 0, 0, &count, nullptr) == S_OK && count == 1,
                  "output slot 21 counts one display mode");
            Mode mode{};
            count = 1;
            if (check(slot<ModesFn>(output, kOutputModes)(output, 0, 0, &count, &mode) == S_OK && count == 1,
                      "output slot 21 lists it")) {
                width = mode.width;
                height = mode.height;
                std::printf("tiny_present: display mode %ux%u at %u/%u Hz\n", width, height, mode.refresh_numerator,
                            mode.refresh_denominator);
            }
            output->Release();
        }
        if (adapter) adapter->Release();
        if (dxgi) dxgi->Release();
    }
    if (!width || !height) return finish();

    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) && queue, "a direct command queue");
    check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
              allocator, "a command allocator");
    if (allocator)
        check(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                                  IID_PPV_ARGS(&list))) && list, "a console command list");
    check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && fence, "a fence");
    // The console's heap description: the PC one, then the heap's address and size in title memory (0: none).
    struct {
        D3D12_DESCRIPTOR_HEAP_DESC desc;
        std::uint64_t address, size;
    } heap_desc{};
    heap_desc.desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.desc.NumDescriptors = 1;
    check(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc.desc, IID_PPV_ARGS(&rtv_heap))) && rtv_heap,
          "an RTV heap (console description)");
    if (!queue || !list || !fence || !rtv_heap) return finish();
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv{
        static_cast<SIZE_T>(reinterpret_cast<HeapStartFn>(vtable(rtv_heap)[9])(rtv_heap))};

    // The back buffer: the display mode's size, ALLOW_DISPLAY, in PRESENT, as the public samples make them.
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource* back = nullptr;
    check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_ALLOW_DISPLAY, &desc,
                                                    D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&back))) &&
              back, "a display-sized back buffer");
    if (!back) return finish();
    device->CreateRenderTargetView(back, nullptr, rtv);

    // The frame clock: 60 Hz, the origin event.
    using SetIntervalFn = HRESULT(__stdcall*)(void*, void*, UINT, UINT, UINT);
    using ScheduleFn = HRESULT(__stdcall*)(void*, UINT, UINT, void*, UINT);
    using WaitFn = HRESULT(__stdcall*)(void*, UINT, DWORD, void*, UINT, std::uint64_t*);
    check(slot<SetIntervalFn>(device, kDeviceSetFrameInterval)(device, nullptr, 16667, 1, 0) == S_OK,
          "SetFrameIntervalX at 60 Hz");
    check(slot<ScheduleFn>(device, kDeviceScheduleFrameEvent)(device, 0, 0, nullptr, 0) == S_OK, "ScheduleFrameEventX");

    using ResetFn = HRESULT(__stdcall*)(void*, ID3D12CommandAllocator*, ID3D12PipelineState*);
    using CloseFn = HRESULT(__stdcall*)(void*);
    using ClearFn = void(__stdcall*)(void*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*);
    using ExecuteFn = void(__stdcall*)(void*, UINT, ID3D12CommandList* const*);
    using SignalFn = HRESULT(__stdcall*)(void*, ID3D12Fence*, UINT64);
    using PresentFn = HRESULT(__stdcall*)(void*, UINT, const PresentPlane*, const void*);
    slot<CloseFn>(list, kListClose)(list);  // created open, as on PC

    const FLOAT left[4] = {64 / 255.0f, 128 / 255.0f, 191 / 255.0f, 1.0f};
    const FLOAT corner[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    const FLOAT right[4] = {0.0f, 200 / 255.0f, 0.0f, 1.0f};
    const D3D12_RECT right_half{LONG(width / 2), 0, LONG(width), LONG(height)};
    const D3D12_RECT top_left{0, 0, LONG(width / 2), LONG(height / 2)};

    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    char capture[MAX_PATH] = {};
    const DWORD capture_length = GetEnvironmentVariableA("LEONIDA_CAPTURE_DIR", capture, MAX_PATH);
    const std::string captured = capture_length ? std::string(capture) + "\\capture-1.png" : std::string();
    std::uint64_t last_token = 0;
    unsigned presented = 0, waits_ok = 0, tokens_in_order = 0;
    bool capture_seen = false;
    auto sixty_start = std::chrono::steady_clock::now();
    double sixty_ms = 0.0;
    for (unsigned frame = 1; frame <= 900; ++frame) {
        if (title_window) pump();
        std::uint64_t token = 0;
        if (slot<WaitFn>(device, kDeviceWaitFrameEvent)(device, 0, INFINITE, nullptr, 0, &token) == S_OK) ++waits_ok;
        if (token == last_token + 1) ++tokens_in_order;
        last_token = token;
        if (frame == 11) sixty_start = std::chrono::steady_clock::now();
        if (frame == 71)
            sixty_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sixty_start).count();

        allocator->Reset();
        slot<ResetFn>(list, kListReset)(list, allocator, nullptr);
        barrier(list, back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        slot<ClearFn>(list, kListClearRtv)(list, rtv, left, 0, nullptr);
        slot<ClearFn>(list, kListClearRtv)(list, rtv, right, 1, &right_half);
        slot<ClearFn>(list, kListClearRtv)(list, rtv, corner, 1, &top_left);
        barrier(list, back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        slot<CloseFn>(list, kListClose)(list);
        ID3D12CommandList* lists[] = {list};
        slot<ExecuteFn>(queue, kQueueExecute)(queue, 1, lists);
        ID3D12Resource* const resources[] = {back};
        const PresentPlane plane{token, 1, resources, nullptr, nullptr, 0, 0, 0, nullptr, 0};
        if (slot<PresentFn>(queue, kQueuePresentX)(queue, 1, &plane, nullptr) == S_OK) ++presented;
        slot<SignalFn>(queue, kQueueSignal)(queue, fence, frame);
        if (fence->GetCompletedValue() < frame) {
            fence->SetEventOnCompletion(frame, done);
            WaitForSingleObject(done, 10000);
        }
        if (!capture_seen && !captured.empty() && GetFileAttributesA(captured.c_str()) != INVALID_FILE_ATTRIBUTES) {
            capture_seen = true;
            std::printf("tiny_present: the screenshot is on disk after %u frames\n", frame);
        }
        if (capture_seen && frame >= 90) break;
    }
    check(waits_ok == presented && presented >= 90, "every WaitFrameEventX returned S_OK and every PresentX S_OK");
    check(tokens_in_order == presented, "the frame clock's tokens count the frames (1, 2, 3, ...)");
    std::printf("tiny_present: 60 frames at 60 Hz took %.0f ms\n", sixty_ms);
    check(sixty_ms > 800.0 && sixty_ms < 1500.0, "the frame clock paces the frames to about 60 a second");
    check(capture_seen, "the requested screenshot was written while the title presented");

    // The caption of the window the frames went to.
    HWND shown = title_window ? own : FindWindowW(L"LeonidaWindow", nullptr);
    check(shown != nullptr, title_window ? "the title's window" : "the runtime's window (class LeonidaWindow)");
    if (shown) {
        // The caption thread set it at least once in the 1.5 s of frames above (it sends WM_SETTEXT, which the title's
        // own window takes while its thread pumps).
        if (title_window) pump();
        char caption[128] = {};
        GetWindowTextA(shown, caption, sizeof(caption));
        std::printf("tiny_present: caption '%s'\n", caption);
        double fps = 0, ms = 0;
        check(std::sscanf(caption, "Leonida | %lf fps %lf ms/frame", &fps, &ms) == 2 && fps > 20.0 && fps < 120.0 &&
                  ms > 0.0,
              "the caption reads Leonida | <fps> fps <ms> ms/frame, at about 60 fps");
        RECT client{};
        GetClientRect(shown, &client);
        std::printf("tiny_present: window client area %ldx%ld\n", client.right, client.bottom);
    }
    if (done) CloseHandle(done);
    return finish();
}
