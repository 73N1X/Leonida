// Xbox-ABI proxy around a PC ID3D12Device. See d3d12_xs_device.h for the measured slots and the limits.
//
// How forwarding works: the proxy object's first word is a vtable built once from one thunk per slot. A thunk
// receives the proxy in RCX, replaces it with the PC device pointer from offset 8, and tail-calls the PC method at
// the same index through a 15-argument integer prototype. Every PC D3D12 device method takes integer/pointer
// arguments only, so the x64 registers and stack words line up exactly; methods that return a struct keep their
// hidden return pointer in RDX, which is passed through as the first argument unchanged. The measured exceptions
// are IUnknown (0-2), slot 14 (which wraps the heap), 62 and 76, plus the heap proxy's 9/10 conversion from the
// PC hidden-pointer form to the Xbox RAX form. Slots 8/75 (command queues), 36 (fences) and 32 (a shared fence) wrap
// what they create in queue and fence proxies built the same way, and slots 31/34/35/45/46/50 hand the real objects
// behind heap, queue and fence proxies to the real device (see the command queue and fence section).
//
// A thunk declares 14 argument words, but the title passes only as many as the method takes, so the caller's frame
// may end right after the four register homes. The compiler owns a function's incoming stack-argument slots and may
// write them, a tail call included. Every function a thunk hands its words to therefore takes them in the same
// positions (slot 8: desc, riid, out as a1-a3; slot 50: a1-a5; and so on). A tail call then writes back only the
// values it read from those slots. A helper that moved a word to another stack position would overwrite the title's
// frame. xs_proxy_mock_test's mock hit this in an optimized build, and its thunks now pass their words by pointer.
//
// Which method a slot is comes from the slot tables (d3d12_xs_slots.h): one table each for the device, the queue, the
// list and the fence, taken from VIctory (victory/crates/victory-gpu) and Leonida's own measurements. A device
// created as ID3D12Device11 has the console layouts, as the title's does (V device.rs:149-168); any other keeps the PC
// order its queues, fences and slot-12 lists had, for Leonida's own PC-interface tests. The thunks read the table at
// compile time (one entry per slot) and pick the console or PC entry from the proxy's layout at run time.
//
// The Xbox routes 48 CreateCommandQueueX, 73 CreateCommandListX and the frame calls 78-81 run before anything is
// forwarded, and can be moved with LEONIDA_XS_*_SLOT for a trace that disagrees. The frame calls are VIctory's frame
// clock (frame.rs): a tick per interval, waits that end on ticks, tokens that count frames.
#include "d3d12_xs_device.h"
#include "d3d12_xs_slots.h"
#include "d3d12_trace.h"
#include "../src/shader_library.h"  // leonida::fspx_payload_from_parts for slot 67
#include "../src/pipeline_queue.h"  // the pipelines' frame budget follows the frame clock (slot 80)
#ifdef LEONIDA_VULKAN_DEVICE
#include "d3d12_com.h"  // rt::d3d12::create_console_descriptor_heap for slot 14 (Vulkan device only)
#endif

#include <windows.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace rt::d3d12_xs {
namespace {

// The retail title calls device slots well past ID3D12Device14 (slot 133 at least); every slot past the PC device's
// vtable goes through the generic thunk and is refused with E_NOTIMPL, so the table must outlast the highest one.
constexpr unsigned kMaxSlots = 192;
static_assert(kMaxSlots > 133, "the proxy table must cover the slots the retail title calls (up to 133)");
static_assert(kMaxSlots > 83, "the proxy table must cover ID3D12Device14 (83 slots)");
static_assert(sizeof(void*) == 8, "the Xbox-ABI proxy is x64-only");
static_assert(sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) == 8,
              "the Xbox handle ABI is an 8-byte value in RAX; the heap thunk converts the PC hidden-pointer form");
static_assert(sizeof(D3D12_RESOURCE_DESC) == 56, "the measured Xbox description prefix is 56 bytes");
static_assert(slots::kDeviceSlots == kMaxSlots, "the device slot table covers the proxy's table");

using slots::Layout;

// ID3D12Device11 (d3d12.h, SDK 10.0.26100; V device.rs:83-88): the interface the title creates its device as, which
// selects the console layouts.
constexpr GUID kDevice11Iid{0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};

// A page pool registered through console device slot 74 (V resource.rs:2187-2223).
struct PagePool {
    std::uint64_t address;
    std::uint32_t pages;
    bool live;
};

// The first 0x58 bytes are the console device's as a title's inlined code reads them (ported from VIctory,
// victory-gpu device.rs:103-119, GPL-3.0-or-later): +0x10 the GPU address of a null constant buffer (none: 0), and
// +0x18 a block whose first byte, non-zero, sends the calls the title would otherwise inline to the table ("the
// title tests byte 0x18 before inlining a device call", device.rs:479). Before, +0x10 was the PC vtable's address
// and +0x18 the PC slot count, whose low byte only happened to be non-zero.
struct DeviceProxy {
    void** vtable;          // offset 0: the caller's ID3D12Device* points here
    ID3D12Device* real;     // offset 8: the PC device every thunk forwards to
    std::uint64_t null_constant_buffer;  // +0x10: 0
    std::uint32_t out_of_line[16];       // +0x18: word 0 is 1, the rest reserved (0)
    void** real_vtable;
    unsigned slot_count;    // number of valid entries in real_vtable
    std::atomic<unsigned long> refs;
    // The device's IDXGIDevice1 (see "IDXGIDevice1 on the device" below): a title's IDXGIDevice1* points here.
    void** dxgi_vtable;
    DxgiAdapterProvider adapter_provider;
    std::atomic<int> gpu_thread_priority;
    std::atomic<unsigned> max_frame_latency;
    // The layout of this device's queues and lists, from the interface it was created as (V device.rs:149-168).
    Layout layout;
    // The frame clock of slots 78-81 (V frame.rs; see "The frame clock"). The clock fields are under frame_lock.
    SRWLOCK frame_lock;
    bool frame_clock_set;
    std::int64_t frame_start;          // QueryPerformanceCounter count when the interval was set
    std::uint64_t frame_interval_ns;
    std::uint64_t frame_first;         // the tick number at frame_start: ticks go on across interval changes
    // Per event kind (origin, vblank, flip and later kinds): the last tick a wait for it ended on.
    std::atomic<std::uint64_t> frame_last[3];
    std::atomic<std::uint64_t> frames_begun;   // tokens handed out
    // Console slots 74/75, under pool_lock.
    SRWLOCK pool_lock;
    std::vector<PagePool> page_pools;
    // Console queue slot 32 (PresentX): set_present_handler.
    std::atomic<PresentHandler> present_handler;
    std::atomic<void*> present_context;
};

static_assert(offsetof(DeviceProxy, null_constant_buffer) == 0x10 && offsetof(DeviceProxy, out_of_line) == 0x18 &&
                  offsetof(DeviceProxy, real_vtable) == 0x58,
              "V device.rs:112: the console device's fields end at 0x58");

constexpr unsigned kHeapSlots = 11;  // IUnknown + ID3D12Object + DeviceChild + Pageable + GetDesc + 2 handles

// The console heap's fields after its header, as VIctory lays them out (victory-gpu descriptor.rs:49-54): the
// description at +0x10 and the first descriptor's address at +0x20 (the CPU start, which on the console is the GPU
// start too). Filled once from the real heap when the proxy is made.
struct HeapProxy {
    void** vtable;          // offset 0: the caller's ID3D12DescriptorHeap* points here
    ID3D12DescriptorHeap* real;
    D3D12_DESCRIPTOR_HEAP_DESC desc;  // +0x10
    std::uint64_t start;              // +0x20
    DeviceProxy* device;    // keeps the Xbox-ABI device alive; GetDevice returns it
    void** real_vtable;
    std::atomic<unsigned long> refs;
};
static_assert(offsetof(HeapProxy, desc) == 0x10 && offsetof(HeapProxy, start) == 0x20, "V descriptor.rs:49-54");

std::atomic<std::uint64_t> g_hits[kMaxSlots];
std::atomic<bool> g_first[kMaxSlots];
std::atomic<bool> g_beyond_warned[kMaxSlots];
std::atomic<std::uint64_t> g_heap_hits[kHeapSlots];
std::atomic<bool> g_heap_first[kHeapSlots];
// The hit counters are for tests. They count from the first call of a *_slot_hits reader or of
// reset_device_proxy_stats, so the calls of a title, which reads no counter, write no shared counter.
std::atomic<bool> g_count_hits{false};

// For the readers and the reset. The flag is written one time only.
void start_hit_counts() {
    if (!g_count_hits.load(std::memory_order_relaxed)) g_count_hits.store(true, std::memory_order_relaxed);
}

void count_hit(std::atomic<std::uint64_t>& hits) {
    if (g_count_hits.load(std::memory_order_relaxed)) hits.fetch_add(1, std::memory_order_relaxed);
}

// True for the first call of a slot only. The flag is read first: a compare-exchange takes the cache line for
// writing also when it fails, and the flags of all slots share one or two lines.
bool first_call(std::atomic<bool>& seen) {
    if (seen.load(std::memory_order_relaxed)) return false;
    bool expected = false;
    return seen.compare_exchange_strong(expected, true, std::memory_order_relaxed);
}

bool trace_enabled() {
    static const bool enabled = [] {
        char value[8] = {};
        const DWORD length = GetEnvironmentVariableA("XS_TRACE", value, sizeof(value));
        return length > 0 && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

// The device slot table's names (d3d12_xs_slots.h), built once.
const char* slot_name(unsigned slot) {
    static const auto names = [] {
        static const char* table[kMaxSlots];
        for (unsigned s = 0; s < kMaxSlots; ++s) {
            const slots::DeviceSlot entry = slots::device_slot(s);
            table[s] = entry.name ? entry.name : "not named by the slot table";
        }
        return &table;
    }();
    return slot < kMaxSlots ? (*names)[slot] : "past the table";
}

void note_slot(unsigned slot) {
    if (slot >= kMaxSlots) return;
    count_hit(g_hits[slot]);
    const bool first = first_call(g_first[slot]);
    if (first) {
        std::fprintf(stderr, "[rt] d3d12_xs: device slot %u (%s) first call\n", slot, slot_name(slot));
    } else if (slot >= 3 && trace_enabled()) {
        std::fprintf(stderr, "[rt] d3d12_xs: device slot %u (%s) call\n", slot, slot_name(slot));
    }
}

// Xbox device slots: the public HLE Freminet/ScarlettX's observation, which VIctory's table confirms on the title
// (V device.rs:437, 445, 451-454). An environment variable that names another slot in 3..127 replaces one (a
// whole-table move for a trace that disagrees). A value outside that range is logged once and ignored. The table is
// read once, on the first generic slot call, so a test that wants other slots sets the variables before it creates a
// device.
struct XboxDeviceSlotRoutes {
    unsigned frame_interval = 78;    // SetFrameIntervalX(output, interval_us, period, flags)
    unsigned schedule_event = 79;    // ScheduleFrameEventX(kind, offset_us, list, flags)
    unsigned wait_event = 80;        // WaitFrameEventX(kind, timeout_ms, list, flags, &token)
    unsigned frame_statistics = 81;  // GetFrameStatisticsX(token, kinds, &count, out)
    unsigned queue_x = 48;           // CreateCommandQueueX(desc, riid, out)
    unsigned list_x = 73;            // CreateCommandListX(desc, allocator, initial_state, riid, out)
};

unsigned env_device_slot(const char* name, unsigned fallback) {
    char value[32] = {};
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    if (length == 0) return fallback;
    if (length >= sizeof(value)) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s is too long for a device slot; the default is kept\n", name);
        return fallback;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long slot = std::strtoul(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' || slot < 3 || slot > 127) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s=%s is not a device slot in 3..127; the default is kept\n", name,
                     value);
        return fallback;
    }
    return static_cast<unsigned>(slot);
}

const XboxDeviceSlotRoutes& xbox_device_slot_routes() {
    static const XboxDeviceSlotRoutes routes = [] {
        XboxDeviceSlotRoutes r;
        const unsigned frame_interval = env_device_slot("LEONIDA_XS_FRAME_INTERVAL_SLOT", r.frame_interval);
        const unsigned schedule_event = env_device_slot("LEONIDA_XS_SCHEDULE_EVENT_SLOT", r.schedule_event);
        const unsigned wait_event = env_device_slot("LEONIDA_XS_WAIT_EVENT_SLOT", r.wait_event);
        const unsigned frame_statistics = env_device_slot("LEONIDA_XS_FRAME_STATISTICS_SLOT", r.frame_statistics);
        const unsigned queue_x = env_device_slot("LEONIDA_XS_QUEUE_X_SLOT", r.queue_x);
        const unsigned list_x = env_device_slot("LEONIDA_XS_LIST_X_SLOT", r.list_x);
        const bool moved = frame_interval != r.frame_interval || schedule_event != r.schedule_event ||
                           wait_event != r.wait_event || frame_statistics != r.frame_statistics ||
                           queue_x != r.queue_x || list_x != r.list_x;
        r.frame_interval = frame_interval;
        r.schedule_event = schedule_event;
        r.wait_event = wait_event;
        r.frame_statistics = frame_statistics;
        r.queue_x = queue_x;
        r.list_x = list_x;
        if (moved)
            std::fprintf(stderr,
                         "[rt] d3d12_xs: Xbox device slots: SetFrameIntervalX=%u ScheduleFrameEventX=%u "
                         "WaitFrameEventX=%u GetFrameStatisticsX=%u CreateCommandQueueX=%u CreateCommandListX=%u\n",
                         r.frame_interval, r.schedule_event, r.wait_event, r.frame_statistics, r.queue_x, r.list_x);
        return r;
    }();
    return routes;
}

// The public HLE call shapes. SetFrameIntervalX's output and flags are unused while there is no output or pacing;
// ScheduleFrameEventX's reserved event pointer is a3 in the console call and a4 is its flags. The logged words are the
// caller's, so a trace names the slot even when an override moved it.
void log_xbox_route_once(unsigned slot, const char* name, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                         std::uint64_t a4, std::uint64_t a5) {
    static std::atomic<bool> logged[kMaxSlots];
    if (slot >= kMaxSlots) return;
    bool expected = false;
    if (!logged[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: device slot %u routed to %s (args 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx)\n", slot,
                 name, static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                 static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4),
                 static_cast<unsigned long long>(a5));
}

const char* heap_slot_name(unsigned slot) {
    switch (slot) {
        case 0: return "QueryInterface";
        case 1: return "AddRef";
        case 2: return "Release";
        case 7: return "GetDevice";
        case 8: return "GetDesc";
        case 9: return "GetCPUDescriptorHandleForHeapStart";
        case 10: return "GetGPUDescriptorHandleForHeapStart";
        default: return "forwarded";
    }
}

void note_heap_slot(unsigned slot) {
    if (slot >= kHeapSlots) return;
    count_hit(g_heap_hits[slot]);
    const bool first = first_call(g_heap_first[slot]);
    if (first) {
        std::fprintf(stderr, "[rt] d3d12_xs: heap slot %u (%s) first call\n", slot, heap_slot_name(slot));
    } else if (slot >= 3 && trace_enabled()) {
        std::fprintf(stderr, "[rt] d3d12_xs: heap slot %u (%s) call\n", slot, heap_slot_name(slot));
    }
}

std::uint64_t hr_to_u64(HRESULT hr) {
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(hr));
}

unsigned long proxy_add_ref(DeviceProxy* self) {
    note_slot(1);
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

unsigned long proxy_release(DeviceProxy* self) {
    note_slot(2);
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) {
        ID3D12Device* real = self->real;
        delete self;
        if (real) real->Release();
    }
    return refs;
}

bool is_device_iid(const IID& iid) {
    if (IsEqualIID(iid, __uuidof(ID3D12Device))) return true;
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device14))) return true;
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device13))) return true;
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device12))) return true;
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device11))) return true;
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device10))) return true;
#endif
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device9))) return true;
#endif
    if (IsEqualIID(iid, __uuidof(ID3D12Device8))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device7))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device6))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device5))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device4))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device3))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device2))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device1))) return true;
    return false;
}

HRESULT proxy_query_interface(DeviceProxy* self, const IID* riid, void** out) {
    note_slot(0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (IsEqualIID(*riid, __uuidof(IUnknown)) || IsEqualIID(*riid, __uuidof(ID3D12Object))) {
        *out = self;
        proxy_add_ref(self);
        return S_OK;
    }
    if (IsEqualIID(*riid, __uuidof(IDXGIObject)) || IsEqualIID(*riid, __uuidof(IDXGIDevice)) ||
        IsEqualIID(*riid, __uuidof(IDXGIDevice1))) {
        *out = &self->dxgi_vtable;
        proxy_add_ref(self);
        return S_OK;
    }
    if (!is_device_iid(*riid)) return E_NOINTERFACE;

    // Only claim an interface the PC device really implements; the proxy vtable covers its slots.
    void* supported = nullptr;
    HRESULT hr = self->real->QueryInterface(*riid, &supported);
    if (FAILED(hr)) return hr;
    if (!supported) return E_NOINTERFACE;
    static_cast<IUnknown*>(supported)->Release();
    *out = self;
    proxy_add_ref(self);
    return S_OK;
}

// ---- IDXGIDevice1 on the device ---------------------------------------------------------------------------------
//
// A GDKX title finds its output through the device: QueryInterface for IDXGIDevice1, GetAdapter, EnumOutputs(0),
// and hands that output to SetFrameIntervalX (docs/d3d12_vulkan.md section 4.5, from the public samples). A PC
// ID3D12Device has no IDXGIDevice, so the proxy answers it itself: a second vtable pointer inside the proxy object,
// with the proxy's reference count and QueryInterface, so the two pointers are one COM object. Only IDXGIDevice and
// IDXGIDevice1 are answered (what the samples ask for). The 14 slots are the public dxgi.h order; that the console
// keeps it is the same assumption as for the queue and the fence.
//
// GetAdapter and GetParent hand out the adapter of the device behind the proxy. The provider says how: the default
// asks the host's DXGI for the adapter with the device's LUID (the host backend); set_device_adapter_provider puts
// the Vulkan layer's own adapter there, so that backend never loads the host's dxgi.dll for it.
constexpr unsigned kDxgiDeviceSlots = 14;  // IUnknown 3, IDXGIObject 4, IDXGIDevice 5, IDXGIDevice1 2

DeviceProxy* dxgi_owner(void* self) {
    return reinterpret_cast<DeviceProxy*>(static_cast<char*>(self) - offsetof(DeviceProxy, dxgi_vtable));
}

HRESULT host_adapter(ID3D12Device* real, REFIID riid, void** out) {
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    static const CreateFactoryFn create_factory = [] {
        const HMODULE dxgi = LoadLibraryExA("dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return dxgi ? reinterpret_cast<CreateFactoryFn>(
                          reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
                    : nullptr;
    }();
    if (!create_factory) return DXGI_ERROR_NOT_FOUND;
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = create_factory(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) return FAILED(hr) ? hr : DXGI_ERROR_NOT_FOUND;
    hr = factory->EnumAdapterByLuid(real->GetAdapterLuid(), riid, out);
    factory->Release();
    return hr;
}

HRESULT dxgi_adapter(DeviceProxy* self, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    const DxgiAdapterProvider provider = self->adapter_provider ? self->adapter_provider : &host_adapter;
    const HRESULT hr = provider(self->real, riid, out);
    if (FAILED(hr)) *out = nullptr;
    return hr;
}

void note_dxgi(const char* method) {
    d3d12_trace::record(method);
}

HRESULT WINAPI dxgi_query_interface(void* self, const IID* riid, void** out) {
    return proxy_query_interface(dxgi_owner(self), riid, out);
}
ULONG WINAPI dxgi_add_ref(void* self) { return proxy_add_ref(dxgi_owner(self)); }
ULONG WINAPI dxgi_release(void* self) { return proxy_release(dxgi_owner(self)); }

// IDXGIObject's private data is the device's: ID3D12Object has the same three methods with the same arguments.
HRESULT WINAPI dxgi_set_private_data(void* self, const GUID* name, UINT size, const void* data) {
    if (!name) return E_INVALIDARG;
    return dxgi_owner(self)->real->SetPrivateData(*name, size, data);
}
HRESULT WINAPI dxgi_set_private_data_interface(void* self, const GUID* name, const IUnknown* object) {
    if (!name) return E_INVALIDARG;
    return dxgi_owner(self)->real->SetPrivateDataInterface(*name, object);
}
HRESULT WINAPI dxgi_get_private_data(void* self, const GUID* name, UINT* size, void* data) {
    if (!name) return E_INVALIDARG;
    return dxgi_owner(self)->real->GetPrivateData(*name, size, data);
}
HRESULT WINAPI dxgi_get_parent(void* self, const IID* riid, void** out) {
    note_dxgi("XboxIDXGIDevice::GetParent");
    if (!riid) return E_POINTER;
    return dxgi_adapter(dxgi_owner(self), *riid, out);
}
HRESULT WINAPI dxgi_get_adapter(void* self, IDXGIAdapter** out) {
    note_dxgi("XboxIDXGIDevice::GetAdapter");
    return dxgi_adapter(dxgi_owner(self), __uuidof(IDXGIAdapter), reinterpret_cast<void**>(out));
}
// CreateSurface and QueryResourceResidency are D3D10/11-era methods no D3D12 title has a use for; what the console
// answers is not measured, so they are refused and say so once.
HRESULT WINAPI dxgi_create_surface(void*, const DXGI_SURFACE_DESC*, UINT, DXGI_USAGE, const DXGI_SHARED_RESOURCE*,
                                   IDXGISurface** out) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: IDXGIDevice::CreateSurface is not implemented; returning E_NOTIMPL\n");
    if (out) *out = nullptr;
    return E_NOTIMPL;
}
HRESULT WINAPI dxgi_query_resource_residency(void*, IUnknown* const*, DXGI_RESIDENCY*, UINT) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: IDXGIDevice::QueryResourceResidency is not implemented; returning E_NOTIMPL\n");
    return E_NOTIMPL;
}
// The priority and the frame latency are kept and read back; nothing acts on them (the Xbox presenter's latency is
// SetFrameIntervalX's). The ranges are the documented ones: -7..7, and 1..16 with 0 for the default of 3.
HRESULT WINAPI dxgi_set_gpu_thread_priority(void* self, INT priority) {
    if (priority < -7 || priority > 7) return E_INVALIDARG;
    dxgi_owner(self)->gpu_thread_priority.store(priority, std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI dxgi_get_gpu_thread_priority(void* self, INT* priority) {
    if (!priority) return E_POINTER;
    *priority = dxgi_owner(self)->gpu_thread_priority.load(std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI dxgi_set_maximum_frame_latency(void* self, UINT latency) {
    if (latency > 16) return DXGI_ERROR_INVALID_CALL;
    dxgi_owner(self)->max_frame_latency.store(latency ? latency : 3, std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI dxgi_get_maximum_frame_latency(void* self, UINT* latency) {
    if (!latency) return E_POINTER;
    *latency = dxgi_owner(self)->max_frame_latency.load(std::memory_order_relaxed);
    return S_OK;
}

void** get_dxgi_vtable() {
    static void* table[kDxgiDeviceSlots] = {
        reinterpret_cast<void*>(&dxgi_query_interface),          // 0
        reinterpret_cast<void*>(&dxgi_add_ref),                  // 1
        reinterpret_cast<void*>(&dxgi_release),                  // 2
        reinterpret_cast<void*>(&dxgi_set_private_data),         // 3  IDXGIObject
        reinterpret_cast<void*>(&dxgi_set_private_data_interface),
        reinterpret_cast<void*>(&dxgi_get_private_data),
        reinterpret_cast<void*>(&dxgi_get_parent),
        reinterpret_cast<void*>(&dxgi_get_adapter),              // 7  IDXGIDevice
        reinterpret_cast<void*>(&dxgi_create_surface),
        reinterpret_cast<void*>(&dxgi_query_resource_residency),
        reinterpret_cast<void*>(&dxgi_set_gpu_thread_priority),
        reinterpret_cast<void*>(&dxgi_get_gpu_thread_priority),
        reinterpret_cast<void*>(&dxgi_set_maximum_frame_latency),  // 12 IDXGIDevice1
        reinterpret_cast<void*>(&dxgi_get_maximum_frame_latency),
    };
    return table;
}

unsigned long heap_add_ref(HeapProxy* self) {
    note_heap_slot(1);
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

unsigned long heap_release(HeapProxy* self) {
    note_heap_slot(2);
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) {
        ID3D12DescriptorHeap* real = self->real;
        DeviceProxy* device = self->device;
        delete self;
        if (real) real->Release();
        if (device) proxy_release(device);
    }
    return refs;
}

bool is_heap_iid(const IID& iid) {
    return IsEqualIID(iid, __uuidof(ID3D12DescriptorHeap)) || IsEqualIID(iid, __uuidof(ID3D12Pageable)) ||
           IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
           IsEqualIID(iid, __uuidof(IUnknown));
}

HRESULT heap_query_interface(HeapProxy* self, const IID* riid, void** out) {
    note_heap_slot(0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (!is_heap_iid(*riid)) return E_NOINTERFACE;
    *out = self;
    heap_add_ref(self);
    return S_OK;
}

// ID3D12DeviceChild::GetDevice on the heap must hand back the Xbox-ABI device, not the raw PC device.
HRESULT heap_get_device(HeapProxy* self, const IID* riid, void** out) {
    note_heap_slot(7);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (!self->device) return E_NOINTERFACE;
    return proxy_query_interface(self->device, riid, out);
}

// The PC methods write through the hidden return pointer (RDX) on the measured runtime; the Xbox ABI wants the
// 8-byte value in RAX. Call with both conventions and keep whichever produced a value, so a runtime that really
// returns in RAX also works. The extra register is harmless when the callee ignores it.
std::uint64_t real_heap_cpu_start(HeapProxy* self) {
    constexpr SIZE_T kUnset = ~static_cast<SIZE_T>(0);
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = kUnset;
    using Fn = D3D12_CPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*, D3D12_CPU_DESCRIPTOR_HANDLE*);
    const D3D12_CPU_DESCRIPTOR_HANDLE returned = reinterpret_cast<Fn>(self->real_vtable[9])(self->real, &handle);
    return handle.ptr != kUnset ? handle.ptr : returned.ptr;
}

D3D12_CPU_DESCRIPTOR_HANDLE heap_get_cpu_handle(HeapProxy* self) {
    note_heap_slot(9);
    constexpr SIZE_T kUnset = ~static_cast<SIZE_T>(0);
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = kUnset;
    using Fn = D3D12_CPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*, D3D12_CPU_DESCRIPTOR_HANDLE*);
    const D3D12_CPU_DESCRIPTOR_HANDLE returned =
        reinterpret_cast<Fn>(self->real_vtable[9])(self->real, &handle);
    return handle.ptr != kUnset ? handle : returned;
}

D3D12_GPU_DESCRIPTOR_HANDLE heap_get_gpu_handle(HeapProxy* self) {
    note_heap_slot(10);
    constexpr UINT64 kUnset = ~static_cast<UINT64>(0);
    D3D12_GPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = kUnset;
    using Fn = D3D12_GPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*, D3D12_GPU_DESCRIPTOR_HANDLE*);
    const D3D12_GPU_DESCRIPTOR_HANDLE returned =
        reinterpret_cast<Fn>(self->real_vtable[10])(self->real, &handle);
    return handle.ptr != kUnset ? handle : returned;
}

template <unsigned Slot>
std::uint64_t WINAPI heap_slot(HeapProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                               std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                               std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                               std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        return hr_to_u64(heap_query_interface(self, reinterpret_cast<const IID*>(a1),
                                              reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return heap_add_ref(self);
    } else if constexpr (Slot == 2) {
        return heap_release(self);
    } else if constexpr (Slot == 7) {
        return hr_to_u64(heap_get_device(self, reinterpret_cast<const IID*>(a1),
                                         reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 9) {
        return static_cast<std::uint64_t>(heap_get_cpu_handle(self).ptr);
    } else if constexpr (Slot == 10) {
        return static_cast<std::uint64_t>(heap_get_gpu_handle(self).ptr);
    } else {
        note_heap_slot(Slot);
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
        return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }
}

template <std::size_t... I>
void fill_heap_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&heap_slot<static_cast<unsigned>(I)>) ), ...);
}

void** get_heap_vtable() {
    static void** vtable = [] {
        static void* storage[kHeapSlots];
        fill_heap_vtable(storage, std::make_index_sequence<kHeapSlots>{});
        return storage;
    }();
    return vtable;
}

HRESULT create_heap_proxy(ID3D12DescriptorHeap* real, DeviceProxy* device, REFIID riid, void** out) {
    if (!real || !device) return E_POINTER;
    if (!out) return E_POINTER;
    *out = nullptr;

    HeapProxy* proxy = new (std::nothrow) HeapProxy;
    if (!proxy) return E_OUTOFMEMORY;
    proxy->vtable = get_heap_vtable();
    proxy->real = real;
    proxy->device = device;
    proxy->real_vtable = *reinterpret_cast<void***>(real);
    proxy->refs.store(1, std::memory_order_relaxed);
    // +0x10 and +0x20 (V descriptor.rs:49-54), from the real heap's GetDesc (slot 8) and CPU start (slot 9), called
    // without counting them as the title's calls. GetDesc is a struct return through the hidden pointer on the PC.
    proxy->desc = {};
    using DescFn = void*(WINAPI*)(ID3D12DescriptorHeap*, D3D12_DESCRIPTOR_HEAP_DESC*);
    reinterpret_cast<DescFn>(proxy->real_vtable[8])(real, &proxy->desc);
    proxy->start = real_heap_cpu_start(proxy);
    proxy_add_ref(device);  // the heap keeps its Xbox-ABI device alive for GetDevice

    const HRESULT hr = heap_query_interface(proxy, &riid, out);
    if (FAILED(hr)) {
        // No reference escaped, and the caller still owns its references to `real` and `device`.
        delete proxy;
        proxy_release(device);
        return hr;
    }
    if (!d3d12_trace::enabled())
        std::fprintf(stderr, "[rt] d3d12_xs: heap proxy %p wraps PC heap %p (device proxy %p)\n",
                     static_cast<void*>(proxy), static_cast<void*>(real), static_cast<void*>(device));
    heap_release(proxy);
    return S_OK;
}

// ---- Command queue and fence proxies --------------------------------------------------------------------------
//
// Device slots 8 (CreateCommandQueue), 48 (CreateCommandQueueX), 36 (CreateFence), 32 (OpenSharedHandle, for a fence)
// and 75 in the PC layout (CreateCommandQueue1) wrap what they create, so a title never holds a PC or Vulkan-layer
// queue or fence. Each proxy has its device's layout.
//
// The PC layout forwards the public chain in Windows SDK 10.0.26100 order (ID3D12CommandQueue 19 slots, ID3D12Fence
// 11, ID3D12Fence1 12), and a slot past it is logged and refused with E_NOTIMPL. The console layout is the slot table
// (d3d12_xs_slots.h): the queue methods from 14 on are two later than the PC's (V queue.rs:978-987), PresentX is 32,
// Signal2X 34 and ExecuteX 35, markers are ignored, and a slot no source names is refused like the PC tail. A
// console fence has one method more, slot 12, the address of a 64-bit word the proxy keeps at the fence's completed
// value (see "Fence value words"). GetDesc returns its struct through a hidden pointer, which the console queue
// takes in either order (as device slots 25/26 do; see struct_return_this).
//
// The refused range is the proxy's own table: 128 queue slots and 64 fence slots, every one a thunk. The table sits
// at the end of the first page of a 64 KiB reservation whose other pages are never committed, so a slot index past
// the table makes the vtable read itself fault, at an address that names the slot (a vectored handler logs it and
// lets the fault continue), instead of jumping through whatever memory follows. A title whose vtable read lands
// further away than that reservation is not covered.
//
// Objects cross back into the real runtime wherever a device or queue method takes them, and the real object is
// passed there: queue Signal/Wait (PC 14/15, console 16/17 and Signal2X 34) take a fence, ExecuteCommandLists (10)
// and ExecuteX (console 35) lists; device CreateSharedHandle (31) a device child, MakeResident/Evict (34/35),
// SetResidencyPriority (46) and EnqueueMakeResident (50) pageables, and SetEventOnMultipleFenceCompletion (45) and
// EnqueueMakeResident fences (45/46/50 in the PC layout only). The heap proxies of slot 14 are pageables and device
// children too and are unwrapped in the same places; so are command-list proxies (list Reset, ExecuteCommandLists).
//
// QueryInterface answers only the interfaces the proxy models (the base chain and its own interface, Fence1 when
// the real fence has it), like the device and heap proxies, so the COM identity holds and no real object leaks to
// the title. Leonida's own code reaches the real object through queue_proxy_real_queue / fence_proxy_real_fence /
// allocator_proxy_real_allocator.
//
// Command allocators (device slot 9) are the third kind: their public chain is 9 slots (Reset at 8). In the PC layout
// slot 9 wraps only an allocator that answers kLeonidaCommandAllocatorTag, because a list takes the title's allocator
// in CreateCommandList and Reset and only the Vulkan layer's lists resolve a proxy there (CommandAllocator::from); a
// PC runtime allocator is handed out unchanged. In the console layout every list is a list proxy, which unwraps the
// allocator at Reset/ResetX and creation, so every allocator is wrapped, and its console slot 9 (an allocation
// callback, V list.rs:1378-1387) is accepted.

enum class ChildKind { queue, fence, allocator };

constexpr unsigned kQueueSlots = 128;        // the table: 19 public slots plus a refused tail
constexpr unsigned kFenceSlots = 64;         // the table: 12 public slots plus a refused tail
constexpr unsigned kQueuePublicSlots = 19;   // ID3D12CommandQueue
constexpr unsigned kFencePublicSlots = 11;   // ID3D12Fence
constexpr unsigned kFence1PublicSlots = 12;  // ID3D12Fence1
constexpr unsigned kAllocatorSlots = 64;     // the table: 9 public slots plus a refused tail
constexpr unsigned kAllocatorPublicSlots = 9;  // ID3D12CommandAllocator: the base chain and Reset (slot 8)
constexpr SIZE_T kTableRegion = 64 * 1024;   // one allocation granule: the table page plus 15 never-committed pages
static_assert(kQueueSlots > kQueuePublicSlots && kFenceSlots > kFence1PublicSlots &&
                  kAllocatorSlots > kAllocatorPublicSlots,
              "the tables need a tail");
static_assert(kQueueSlots * sizeof(void*) <= 4096 && kFenceSlots * sizeof(void*) <= 4096 &&
                  kAllocatorSlots * sizeof(void*) <= 4096,
              "a table must fit in the first page of its reservation");

static_assert(kQueueSlots == slots::kQueueSlots, "the queue slot table covers the proxy's table");
static_assert(slots::kFenceValueAddressSlot < kFenceSlots && slots::kAllocatorCallbackSlot < kAllocatorSlots);

constexpr unsigned kQueueSignal = 14, kQueueWait = 15;  // PC slots (ID3D12Fence*, UINT64)

struct ChildProxy {
    void** vtable;          // offset 0: the caller's ID3D12CommandQueue* / ID3D12Fence* points here
    IUnknown* real;         // offset 8: the PC or Vulkan-layer object every thunk forwards to
    // +0x10, right after the console object header, is what a title's inlined code reads (ported from VIctory,
    // victory-gpu queue.rs:31-35 "the title reads the list type right after the header", list.rs:90-94, fence.rs:
    // 24-29): a queue's and an allocator's command list type (the low word), and a console fence's value word (fence
    // slot 12's address; written under g_fence_words_lock). Before, +0x10 was the device pointer below.
    std::atomic<std::uint64_t> value_word;
    DeviceProxy* device;    // keeps the Xbox-ABI device alive; GetDevice returns it
    void** real_vtable;
    unsigned slot_count;    // public slots the real object implements; the rest of the table is refused
    std::atomic<unsigned long> refs;
    Layout layout;          // the device's
    // A console fence's place in the list of value words kept up to date (value_word above); written under
    // g_fence_words_lock. Unused for queues, allocators and PC-layout fences (word_listed false).
    ChildProxy* word_prev;
    ChildProxy* word_next;
    bool word_listed;
    // A console queue's own fence for Write64's system timestamps (queue_system_stamps), made on first use.
    std::atomic<ID3D12Fence*> stamp_fence;
    std::atomic<std::uint64_t> stamp_value;
};
static_assert(offsetof(ChildProxy, value_word) == 0x10, "V queue.rs:1048: the title reads the type at +0x10");

template <ChildKind K>
struct ChildStats {
    static constexpr unsigned kSlots = K == ChildKind::queue   ? kQueueSlots
                                       : K == ChildKind::fence ? kFenceSlots
                                                               : kAllocatorSlots;
    static inline std::atomic<std::uint64_t> hits[kSlots];
    static inline std::atomic<bool> first[kSlots];
    static inline std::atomic<bool> beyond_warned[kSlots];
    static inline std::atomic<std::uint64_t> overruns{0};
    // Where the table and its reservation are: written once, before the table is handed out.
    static inline std::atomic<std::uintptr_t> table_begin{0}, table_end{0}, region_end{0};
};

template <ChildKind K>
constexpr const char* child_kind_name() {
    return K == ChildKind::queue ? "queue" : K == ChildKind::fence ? "fence" : "allocator";
}

template <ChildKind K>
const char* child_public_name(unsigned slot) {
    static constexpr const char* object[] = {"QueryInterface", "AddRef", "Release", "GetPrivateData",
                                             "SetPrivateData", "SetPrivateDataInterface", "SetName", "GetDevice"};
    static constexpr const char* queue[] = {"UpdateTileMappings", "CopyTileMappings", "ExecuteCommandLists",
                                            "SetMarker", "BeginEvent", "EndEvent", "Signal", "Wait",
                                            "GetTimestampFrequency", "GetClockCalibration", "GetDesc"};
    static constexpr const char* fence[] = {"GetCompletedValue", "SetEventOnCompletion", "Signal",
                                            "GetCreationFlags"};
    if (slot < 8) return object[slot];
    if constexpr (K == ChildKind::queue) {
        if (slot < kQueuePublicSlots) return queue[slot - 8];
    } else if constexpr (K == ChildKind::fence) {
        if (slot < kFence1PublicSlots) return fence[slot - 8];
    } else {
        if (slot == 8) return "Reset";
    }
    return nullptr;
}

// The console layout's names (d3d12_xs_slots.h): the queue's table, and the fence's and allocator's one console slot.
template <ChildKind K>
const char* child_console_name(unsigned slot) {
    if (slot < 3 || slot == 7) return child_public_name<K>(slot);
    if constexpr (K == ChildKind::queue) {
        return slots::console_queue_slot(slot).name;
    } else if constexpr (K == ChildKind::fence) {
        return slot == slots::kFenceValueAddressSlot ? "ValueAddressX" : child_public_name<K>(slot);
    } else {
        return slot == slots::kAllocatorCallbackSlot ? "AllocationCallbackX" : child_public_name<K>(slot);
    }
}

template <ChildKind K>
const char* child_method_name(Layout layout, unsigned slot) {
    return layout == Layout::console ? child_console_name<K>(slot) : child_public_name<K>(slot);
}

// Fixed names for the trace channel (d3d12_trace::record takes fixed strings): one per table slot and layout, built
// once, in the trace grammar of runtime/README.md and check_d3d12_trace.py (an identifier with `::`, no spaces):
// XboxID3D12CommandQueue::Signal, and XboxID3D12CommandQueue::Slot40 for a slot no table names.
template <ChildKind K>
const char* child_trace_name(unsigned slot, Layout layout = Layout::pc) {
    static const auto names = [] {
        static char storage[2][ChildStats<K>::kSlots][64];
        const char* iface = K == ChildKind::queue   ? "XboxID3D12CommandQueue"
                            : K == ChildKind::fence ? "XboxID3D12Fence"
                                                    : "XboxID3D12CommandAllocator";
        for (unsigned l = 0; l < 2; ++l) {
            for (unsigned s = 0; s < ChildStats<K>::kSlots; ++s) {
                const char* method = child_method_name<K>(l ? Layout::console : Layout::pc, s);
                if (method)
                    std::snprintf(storage[l][s], sizeof(storage[l][s]), "%s::%s", iface, method);
                else
                    std::snprintf(storage[l][s], sizeof(storage[l][s]), "%s::Slot%u", iface, s);
            }
        }
        return &storage;
    }();
    return (*names)[layout == Layout::console ? 1 : 0][slot < ChildStats<K>::kSlots ? slot : 0];
}

template <ChildKind K>
void note_child_slot(unsigned slot, Layout layout = Layout::pc) {
    using S = ChildStats<K>;
    if (slot >= S::kSlots) return;
    count_hit(S::hits[slot]);
    const bool first = first_call(S::first[slot]);
    const char* method = child_method_name<K>(layout, slot);
    if (first) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s slot %u (%s%s) first call\n", child_kind_name<K>(), slot,
                     method ? method : "not in the table", layout == Layout::console ? ", console layout" : "");
    } else if (slot >= 3 && trace_enabled()) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s slot %u (%s) call\n", child_kind_name<K>(), slot,
                     method ? method : "not in the table");
    }
}

// The counters reset_device_proxy_stats clears for one kind: per-slot hits, first-call and beyond-table flags, and
// the faulted reads past the table.
template <ChildKind K>
void reset_child_stats() {
    using S = ChildStats<K>;
    for (unsigned slot = 0; slot < S::kSlots; ++slot) {
        S::hits[slot].store(0, std::memory_order_relaxed);
        S::first[slot].store(false, std::memory_order_relaxed);
        S::beyond_warned[slot].store(false, std::memory_order_relaxed);
    }
    S::overruns.store(0, std::memory_order_relaxed);
}

// ---- Fence value words (console fence slot 12) ------------------------------------------------------------------
//
// A console fence's value lives in memory, and titles read it there instead of calling GetCompletedValue (V
// fence.rs:24-29); slot 12 hands out its address (fence.rs:136-139, 152). A fence proxy wraps a PC or Vulkan-layer
// fence whose value is not in such a word, so the proxy keeps one: a 64-bit word set to the real fence's completed
// value when the fence proxy is made, after each of its own calls that can move or read the value (Signal,
// GetCompletedValue, slot 12 itself), and when the real fence reaches a value a console queue signalled. For that
// last part each Signal through a console queue proxy asks the real fence for one event, SetEventOnCompletion(value),
// and a thread-pool wait on that event re-reads every listed fence. The event is one for the process and is never
// closed, so a real fence that sets it after its proxy is gone sets nothing else.
//
// Every store reads the completed value and writes the word under g_fence_words_lock (exclusive), so stores land in
// the order the reads were made: the word follows the fence, a CPU Signal to a lower value included. What is not done:
// a title that writes the word itself, or has the GPU write it (a list's Write64 at that address, still open),
// does not move the real fence.
SRWLOCK g_fence_words_lock = SRWLOCK_INIT;
ChildProxy* g_fence_words = nullptr;  // the listed console fences
bool g_fence_words_stopped = false;   // set at process exit: the real fences may be going away

// With g_fence_words_lock held exclusively.
std::uint64_t store_fence_word_locked(ChildProxy* fence) {
    using Fn = std::uint64_t(WINAPI*)(IUnknown*);
    const std::uint64_t value = reinterpret_cast<Fn>(fence->real_vtable[8])(fence->real);  // GetCompletedValue
    fence->value_word.store(value, std::memory_order_release);
    return value;
}

std::uint64_t refresh_fence_word(ChildProxy* fence) {
    AcquireSRWLockExclusive(&g_fence_words_lock);
    const std::uint64_t value = store_fence_word_locked(fence);
    ReleaseSRWLockExclusive(&g_fence_words_lock);
    return value;
}

VOID CALLBACK fence_words_signalled(PVOID, BOOLEAN) {
    AcquireSRWLockExclusive(&g_fence_words_lock);
    if (!g_fence_words_stopped)
        for (ChildProxy* fence = g_fence_words; fence; fence = fence->word_next) store_fence_word_locked(fence);
    ReleaseSRWLockExclusive(&g_fence_words_lock);
}

struct FenceWordWatch {
    HANDLE event = nullptr;
    HANDLE wait = nullptr;
    FenceWordWatch() {
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset: one wake per SetEvent
        if (event && !RegisterWaitForSingleObject(&wait, event, &fence_words_signalled, nullptr, INFINITE,
                                                  WT_EXECUTEDEFAULT))
            wait = nullptr;
        if (!wait)
            std::fprintf(stderr, "[rt] d3d12_xs: no wait for fence completions (error %lu); a console fence's value "
                                 "word moves only with its own calls\n", GetLastError());
    }
    // At exit, before the statics made earlier (the Vulkan device's among them) are destroyed: no more re-reads.
    ~FenceWordWatch() {
        AcquireSRWLockExclusive(&g_fence_words_lock);
        g_fence_words_stopped = true;
        ReleaseSRWLockExclusive(&g_fence_words_lock);
    }
};

// The completion event, or null without the thread-pool wait. Made when the first console fence is.
HANDLE fence_word_event() {
    static FenceWordWatch watch;
    return watch.wait ? watch.event : nullptr;
}

void list_fence_word(ChildProxy* fence) {
    fence_word_event();  // the watch exists before any fence is listed, so it is destroyed after none is re-read
    AcquireSRWLockExclusive(&g_fence_words_lock);
    store_fence_word_locked(fence);
    fence->word_prev = nullptr;
    fence->word_next = g_fence_words;
    if (g_fence_words) g_fence_words->word_prev = fence;
    g_fence_words = fence;
    fence->word_listed = true;
    ReleaseSRWLockExclusive(&g_fence_words_lock);
}

// Waits for a re-read that is using `fence`.
void unlist_fence_word(ChildProxy* fence) {
    if (!fence->word_listed) return;
    AcquireSRWLockExclusive(&g_fence_words_lock);
    if (fence->word_prev) fence->word_prev->word_next = fence->word_next;
    else g_fence_words = fence->word_next;
    if (fence->word_next) fence->word_next->word_prev = fence->word_prev;
    fence->word_listed = false;
    ReleaseSRWLockExclusive(&g_fence_words_lock);
}

// Frees a proxy whose last reference is gone, with the real object and the device reference it held.
void destroy_child(ChildProxy* self) {
    unlist_fence_word(self);
    if (ID3D12Fence* stamps = self->stamp_fence.exchange(nullptr)) stamps->Release();
    IUnknown* real = self->real;
    DeviceProxy* device = self->device;
    delete self;
    if (real) real->Release();
    if (device) proxy_release(device);
}

template <ChildKind K>
unsigned long child_add_ref(ChildProxy* self) {
    note_child_slot<K>(1);
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

template <ChildKind K>
unsigned long child_release(ChildProxy* self) {
    note_child_slot<K>(2);
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) destroy_child(self);
    return refs;
}

// The interfaces the proxy answers: the base chain and the object's own interface.
template <ChildKind K>
bool is_child_iid(const IID& iid) {
    if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
        IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Pageable)))
        return true;
    if constexpr (K == ChildKind::queue) {
        return IsEqualIID(iid, __uuidof(ID3D12CommandQueue));
    } else if constexpr (K == ChildKind::fence) {
        return IsEqualIID(iid, __uuidof(ID3D12Fence)) || IsEqualIID(iid, __uuidof(ID3D12Fence1));
    } else {
        return IsEqualIID(iid, __uuidof(ID3D12CommandAllocator));
    }
}

bool supports_iid(IUnknown* real, REFIID iid) {
    void* found = nullptr;
    const HRESULT hr = real->QueryInterface(iid, &found);
    if (FAILED(hr) || !found) return false;
    static_cast<IUnknown*>(found)->Release();
    return true;
}

// An interface the real object implements but the proxy does not model is refused where the PC runtime would answer:
// logged once per interface (the first 32 of them), so a title that stops after such a refusal shows what it asked
// for. Also reached from wrap_child, for the interface a title creates a queue or fence as.
void log_refused_iid(const char* kind, const IID& iid) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static IID seen[32];
    static unsigned count = 0;
    AcquireSRWLockExclusive(&lock);
    bool known = false;
    for (unsigned i = 0; i < count && !known; ++i) known = IsEqualIID(seen[i], iid) != 0;
    const bool log = !known && count < std::size(seen);
    if (log) seen[count++] = iid;
    ReleaseSRWLockExclusive(&lock);
    if (!log) return;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: %s proxy refuses {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}, which the real "
                 "object implements; E_NOINTERFACE\n",
                 kind, iid.Data1, iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3],
                 iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
}

template <ChildKind K>
HRESULT child_query_interface(ChildProxy* self, const IID* riid, void** out) {
    note_child_slot<K>(0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    // Only what the proxy models, and of that only what the real object implements (ID3D12Fence1 is optional).
    if (!is_child_iid<K>(*riid)) {
        if (supports_iid(self->real, *riid)) log_refused_iid(child_kind_name<K>(), *riid);
        return E_NOINTERFACE;
    }
    if (!supports_iid(self->real, *riid)) return E_NOINTERFACE;
    *out = self;
    child_add_ref<K>(self);
    return S_OK;
}

// ID3D12DeviceChild::GetDevice hands back the Xbox-ABI device, not the real one. Without an output it is the real
// object's own answer (what the PC runtime returns for a null pointer).
template <ChildKind K>
HRESULT child_get_device(ChildProxy* self, const IID* riid, void** out) {
    note_child_slot<K>(7);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure (bc59ef46)
    if (!riid) return E_POINTER;
    if (!out) {
        using Fn = HRESULT(WINAPI*)(IUnknown*, const IID*, void**);
        return reinterpret_cast<Fn>(self->real_vtable[7])(self->real, riid, nullptr);
    }
    if (!self->device) return E_NOINTERFACE;
    return proxy_query_interface(self->device, riid, out);
}

void** get_heap_vtable();
void** get_list_vtable();

template <ChildKind K>
void** get_child_vtable();

template <ChildKind K>
bool is_child_proxy(const void* object) {
    return object && *reinterpret_cast<void* const*>(object) == reinterpret_cast<void*>(get_child_vtable<K>());
}

// After a console queue's Signal(fence, value) went to the real queue: the fence's value word follows once the real
// fence gets there (see "Fence value words"). A fence that is not a console fence proxy has no word.
void arm_fence_word(std::uint64_t fence_word, std::uint64_t value) {
    if (!is_child_proxy<ChildKind::fence>(reinterpret_cast<const void*>(fence_word))) return;
    auto* fence = reinterpret_cast<ChildProxy*>(fence_word);
    if (!fence->word_listed) return;
    const HANDLE event = fence_word_event();
    if (!event) return;
    using Fn = HRESULT(WINAPI*)(IUnknown*, UINT64, HANDLE);
    const HRESULT hr = reinterpret_cast<Fn>(fence->real_vtable[9])(fence->real, value, event);  // SetEventOnCompletion
    if (FAILED(hr)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: SetEventOnCompletion for a fence's value word failed (0x%08lx); the "
                                 "word moves only with the fence's own calls\n", static_cast<unsigned long>(hr));
    }
}

constexpr unsigned kListSlots = 160;
static_assert(kListSlots == slots::kListSlots, "the list slot table covers the proxy's table");

// The console's own ID3D12GraphicsCommandList5 and ID3D12GraphicsCommandList6 (V list.rs:33-88; the title asks for
// List6). Their ids differ from the PC's in the last eight bytes. A list proxy answers them with the console layout.
constexpr GUID kConsoleList5Iid{0x55050859, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
constexpr GUID kConsoleList6Iid{0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};

struct ListProxy {
    void** vtable;                      // +0x00
    ID3D12GraphicsCommandList* real;    // +0x08
    // +0x10: the list's type, right after the console object header, where the title's inlined code reads it (ported
    // from VIctory, victory-gpu list.rs:96-100, GPL-3.0-or-later). Before, +0x10 was the device pointer.
    std::uint32_t list_type;
    std::uint32_t reserved;
    // +0x18..: the console object body a title's inline writers read. +0x18 holds the write cursor, +0x28 and
    // +0x30 the record limits; the rest is room for the measured flags, header table and pipeline record.
    alignas(8) std::uint8_t console_fields[0x600];
    DeviceProxy* device;                // keeps the Xbox-ABI device alive; GetDevice returns it
    std::atomic<unsigned long> refs;    // past the console body
    unsigned pc_slot_count;             // slots the real list implements; mapped slots past it are refused
    std::vector<ValueWait> waits;       // slots 73/74 since the last reset, for the queue's ExecuteX
    std::vector<std::uint64_t> system_stamps;  // slot 72 with flag 2 since the last reset: written after ExecuteX

    void pin_cursor_at_limit() {
        const std::uint64_t at_limit = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(this) + 0x18 + 0x500);
        for (const size_t offset : {0x18, 0x28, 0x30})
            std::memcpy(reinterpret_cast<std::uint8_t*>(this) + offset, &at_limit, sizeof(at_limit));
    }
};
static_assert(offsetof(ListProxy, console_fields) == 0x18, "the console fields start at +0x18");
static_assert(offsetof(ListProxy, list_type) == 0x10, "V list.rs:98: the type follows the header");

// The real object behind a heap, queue, fence or list proxy, or null when `object` is none of them.
IUnknown* proxy_real_object(const void* object) {
    if (!object) return nullptr;
    void* const vtable = *reinterpret_cast<void* const*>(object);
    if (vtable == reinterpret_cast<void*>(get_heap_vtable())) return static_cast<const HeapProxy*>(object)->real;
    if (vtable == reinterpret_cast<void*>(get_child_vtable<ChildKind::queue>()) ||
        vtable == reinterpret_cast<void*>(get_child_vtable<ChildKind::fence>()) ||
        vtable == reinterpret_cast<void*>(get_child_vtable<ChildKind::allocator>()))
        return static_cast<const ChildProxy*>(object)->real;
    if (vtable == reinterpret_cast<void*>(get_list_vtable()))
        return static_cast<const ListProxy*>(object)->real;
    return nullptr;
}

// `object` with a proxy replaced by its real object; anything else (a real object, null) unchanged.
inline std::uint64_t unwrap_object(std::uint64_t object) {
    IUnknown* real = proxy_real_object(reinterpret_cast<const void*>(object));
    return real ? reinterpret_cast<std::uint64_t>(real) : object;
}

// A copy of an array of interface pointers with every proxy replaced by its real object. Nothing here throws, since
// an exception must not leave a thunk for the title's code: up to 16 entries are kept inline (no allocation, which
// also suits a per-frame ExecuteCommandLists), a longer array is allocated with new (std::nothrow).
class UnwrappedArray {
public:
    // Copies `count` entries (a UINT in every PC prototype) of `objects`. False on allocation failure.
    bool fill(std::uint64_t objects, std::uint64_t count) {
        const auto n = static_cast<std::uint32_t>(count);
        if (!objects || !n) return true;
        std::uint64_t* copy = inline_;
        if (n > std::size(inline_)) {
            heap_.reset(new (std::nothrow) std::uint64_t[n]);
            if (!heap_) return false;
            copy = heap_.get();
        }
        const auto* in = reinterpret_cast<const std::uint64_t*>(objects);
        for (std::uint32_t i = 0; i < n; ++i) copy[i] = unwrap_object(in[i]);
        data_ = copy;
        return true;
    }
    // The copy to pass on, or 0 when there was no array (the caller's argument then goes on unchanged).
    std::uint64_t data() const { return reinterpret_cast<std::uint64_t>(data_); }

private:
    std::uint64_t inline_[16];
    std::unique_ptr<std::uint64_t[]> heap_;
    std::uint64_t* data_ = nullptr;
};

std::atomic<const ListTitleHandlers*> g_list_title{nullptr};
std::atomic<PlacedViewHandler> g_placed_view{nullptr};
std::atomic<SamplerXHandler> g_sampler_x{nullptr};
std::atomic<PageMapper> g_page_mapper{nullptr};
std::atomic<RefusedPipelineHandler> g_refused_pipeline{nullptr};
std::atomic<std::uint64_t> g_unmet_waits{0};

// Reads the 32 or 64 bits at `address` without faulting on memory that is not there (false then).
bool read_value(std::uint64_t address, bool wide, std::uint64_t& value) {
    value = 0;
    SIZE_T read = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), &value,
                                        wide ? 8 : 4, &read) &&
           read == (wide ? 8u : 4u);
}

// The list's upload memory (slot 148) goes back to the device, once the GPU has finished with it.
void release_list_uploads(const void* list, DeviceProxy* device) {
    const ListTitleHandlers* h = g_list_title.load(std::memory_order_acquire);
    if (h && h->release_uploads && device) h->release_uploads(device->real, list);
}

struct ListStats {
    static inline std::atomic<std::uint64_t> hits[kListSlots];
    static inline std::atomic<bool> first[kListSlots];
};

// The list slot table's name for `slot`, for the logs.
const char* list_slot_name(unsigned slot) {
    const char* name = slot < kListSlots ? slots::console_list_slot(slot).name : nullptr;
    return name ? name : slot <= 2 ? (slot == 0 ? "QueryInterface" : slot == 1 ? "AddRef" : "Release")
                      : slot == 7  ? "GetDevice"
                                   : "not in the table";
}

inline void note_list_slot(unsigned slot, std::uint64_t a1 = 0, std::uint64_t a2 = 0, std::uint64_t a3 = 0, std::uint64_t a4 = 0) {
    if (slot >= kListSlots) return;
    ListStats::hits[slot].fetch_add(1, std::memory_order_relaxed);
    bool expected = false;
    if (ListStats::first[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: list slot %u (%s) first call (args 0x%llx 0x%llx 0x%llx 0x%llx)\n", slot,
                     list_slot_name(slot), static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                     static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4));
}

inline bool is_console_list_iid(const IID& iid) {
    return IsEqualIID(iid, kConsoleList5Iid) || IsEqualIID(iid, kConsoleList6Iid);
}

inline bool is_list_iid(const IID& iid) {
    return IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
           IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12CommandList)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList1)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList2)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList3)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList4)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList5)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList6)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList7)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList8));
}

inline unsigned long list_add_ref(ListProxy* self) { return self->refs.fetch_add(1, std::memory_order_relaxed) + 1; }

inline void destroy_list(ListProxy* self) {
    IUnknown* real = self->real;
    DeviceProxy* device = self->device;
    release_list_uploads(self, device);
    delete self;
    if (real) real->Release();
    if (device) proxy_release(device);
}

inline unsigned long list_release(ListProxy* self) {
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) destroy_list(self);
    return refs;
}

inline HRESULT list_query_interface(ListProxy* self, const IID* riid, void** out) {
    note_list_slot(0, reinterpret_cast<std::uint64_t>(riid), reinterpret_cast<std::uint64_t>(out), 0, 0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    // The console list interfaces are the proxy's own: it was made over at least an ID3D12GraphicsCommandList, and a
    // slot its table sends past what the real list implements is refused there.
    if (is_console_list_iid(*riid)) {
        *out = self;
        list_add_ref(self);
        return S_OK;
    }
    if (!is_list_iid(*riid)) {
        if (supports_iid(self->real, *riid)) log_refused_iid("list", *riid);
        return E_NOINTERFACE;
    }
    if (!supports_iid(self->real, *riid)) return E_NOINTERFACE;
    *out = self;
    list_add_ref(self);
    return S_OK;
}

inline unsigned detect_list_slot_count(IUnknown* real) {
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList8))) return 82;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList7))) return 81;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList6))) return 80;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList5))) return 79;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList4))) return 77;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList3))) return 68;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList2))) return 67;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList1))) return 66;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList))) return 60;
    return 9;
}

// A console list method that works on title addresses (d3d12_xs_slots.h). Copies, writes and upload memory are the
// device's (set_list_title_handlers); waits are kept on the proxy for the queue; DispatchMeshX is the PC DispatchMesh.
template <slots::ListMethod M, unsigned Slot>
std::uint64_t list_title_method(ListProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                std::uint64_t a4) {
    using slots::ListMethod;
    const ListTitleHandlers* h = g_list_title.load(std::memory_order_acquire);
    ID3D12Device* const device = self->device ? self->device->real : nullptr;
    const auto not_done = [](const char* what) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: list slot %u (%s): %s; nothing is recorded (logged once)\n", Slot,
                         list_slot_name(Slot), what);
        return std::uint64_t{0};
    };
    if constexpr (M == ListMethod::copy_memory) {
        // V list.rs:1294-1314: (destination, source, size, flags), title GPU addresses.
        if (!a1 || !a2 || !a3) return 0;
        if (!h || !h->copy_memory) return not_done("no device handler");
        if (!h->copy_memory(device, self->real, a1, a2, a3)) not_done("an address is not GPU memory");
        return 0;
    } else if constexpr (M == ListMethod::write32 || M == ListMethod::write64) {
        // V list.rs:1964-1983: (address, value, flush, flags), done when the list has run.
        if (!a1) return 0;
        const auto flags = M == ListMethod::write64 ? static_cast<std::uint32_t>(a4) : 0u;
        if constexpr (M == ListMethod::write64) {
            if (flags == 2) {
                // V queue.rs:697-731 (Written::SystemTimestamp): the CPU's performance counter when the list has run;
                // the queue writes it after the submission (queue_system_stamps).
                try {
                    self->system_stamps.push_back(a1);
                } catch (const std::bad_alloc&) {
                }
                return 0;
            }
        }
        if (!h || !h->write_value) return not_done("no device handler");
        const std::uint64_t value = M == ListMethod::write32 ? (a2 & 0xffffffffu) : a2;
        if (!h->write_value(device, self->real, a1, value, M == ListMethod::write32 ? 4 : 8, flags))
            not_done("the address is not GPU memory");
        return 0;
    } else if constexpr (M == ListMethod::wait32 || M == ListMethod::wait64) {
        // V list.rs:1891-1934: (address, comparison, reference). The list holds the rest of its commands at the wait
        // where the device can (ListTitleHandlers::wait_value: the queue that executes it stands there until the value
        // is in memory, V's cut); otherwise the queue looks at the wait when the list is executed (wait_for_values).
        const bool wide = M == ListMethod::wait64;
        const auto comparison = static_cast<std::uint32_t>(a2);
        const bool held = a1 && h && h->wait_value && h->wait_value(device, self->real, a1, comparison, a3, wide);
        try {
            self->waits.push_back(ValueWait{a1, comparison, a3, wide, held});
        } catch (const std::bad_alloc&) {
        }
        return 0;
    } else if constexpr (M == ListMethod::dispatch_mesh_x) {
        // V list.rs:1596-1613: (x groups, flags, indirect arguments, payload). Dispatches whose group count is in GPU
        // memory are dropped, as VIctory drops them; the others are the PC DispatchMesh(x, 1, 1).
        if (a3) return not_done("an indirect mesh dispatch (its group count in GPU memory) is dropped");
        constexpr unsigned pc = slots::console_list_slot(Slot).pc;
        if (pc >= self->pc_slot_count) return not_done("the real list has no DispatchMesh");
        using Fn = void(WINAPI*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
        reinterpret_cast<Fn>((*reinterpret_cast<void***>(self->real))[pc])(self->real, static_cast<UINT>(a1), 1, 1);
        return 0;
    } else {
        static_assert(M == ListMethod::allocate_upload);
        // V list.rs:1409-1431: (size, alignment) -> memory the list owns until it is reset; null for an alignment that
        // is not a power of two.
        if (!h || !h->allocate_upload) return not_done("no device handler");
        return reinterpret_cast<std::uint64_t>(h->allocate_upload(device, self, a1, a2));
    }
}

template <unsigned Slot>
std::uint64_t WINAPI list_slot(ListProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                               std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                               std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                               std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        return hr_to_u64(list_query_interface(self, reinterpret_cast<const IID*>(a1),
                                              reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return list_add_ref(self);
    } else if constexpr (Slot == 2) {
        return list_release(self);
    } else if constexpr (Slot == 7) {  // GetDevice
        note_list_slot(7, a1, a2, 0, 0);
        if (a2) *reinterpret_cast<void**>(a2) = nullptr;
        if (!a1) return hr_to_u64(E_POINTER);
        if (!a2) {
            using Fn = HRESULT(WINAPI*)(IUnknown*, const IID*, void**);
            return hr_to_u64(reinterpret_cast<Fn>((*reinterpret_cast<void***>(self->real))[7])(
                self->real, reinterpret_cast<const IID*>(a1), nullptr));
        }
        return hr_to_u64(proxy_query_interface(self->device, reinterpret_cast<const IID*>(a1),
                                               reinterpret_cast<void**>(a2)));
    } else {
        // The console list table (d3d12_xs_slots.h). Every list proxy has the console layout: lists the PC layout's
        // device slot 12 makes are not wrapped.
        constexpr slots::ListSlot entry = slots::console_list_slot(Slot);
        using slots::ListMethod;
        note_list_slot(Slot, a1, a2, a3, a4);
        if constexpr (entry.method == ListMethod::ignore) {
            return 0;
        } else if constexpr (entry.method == ListMethod::copy_memory || entry.method == ListMethod::write32 ||
                             entry.method == ListMethod::write64 || entry.method == ListMethod::wait32 ||
                             entry.method == ListMethod::wait64 || entry.method == ListMethod::dispatch_mesh_x ||
                             entry.method == ListMethod::allocate_upload) {
            return list_title_method<entry.method, Slot>(self, a1, a2, a3, a4);
        } else if constexpr (entry.method == ListMethod::open || entry.method == ListMethod::unknown) {
            // Today's answer for every slot the proxy does not route: nothing recorded, 0 (no memory from slot 148,
            // which the BVH builder reports as a failed allocation, docs/d3d12_command_objects.md §3.3a).
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr,
                             "[rt] d3d12_xs: list slot %u (%s) is %s; nothing is recorded (args 0x%llx 0x%llx 0x%llx "
                             "0x%llx)\n",
                             Slot, list_slot_name(Slot),
                             entry.method == ListMethod::open ? "a console method not implemented yet"
                                                              : "not in the slot table",
                             static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                             static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4));
            return 0;
        } else {
            // forward, reset, heaps (and typed, whose table entry the vtable replaces with a typed thunk)
            constexpr unsigned pc = entry.pc;
            if (pc >= self->pc_slot_count) {
                static std::atomic<bool> beyond_warned{false};
                if (!beyond_warned.exchange(true, std::memory_order_relaxed))
                    std::fprintf(stderr,
                                 "[rt] d3d12_xs: list slot %u (%s) maps to PC slot %u, beyond the real list's %u "
                                 "slots; returning E_NOTIMPL\n",
                                 Slot, list_slot_name(Slot), pc, self->pc_slot_count);
                return hr_to_u64(E_NOTIMPL);
            }
            using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t);
            auto** const real_vtable = *reinterpret_cast<void***>(self->real);
            UnwrappedArray heaps;
            if constexpr (entry.method == ListMethod::reset) {  // Reset, ResetX (allocator, pipeline[, flags])
                a1 = unwrap_object(a1);
                self->pin_cursor_at_limit();
                self->waits.clear();
                self->system_stamps.clear();
                release_list_uploads(self, self->device);
            } else if constexpr (entry.method == ListMethod::heaps) {  // SetDescriptorHeaps(count, heaps)
                if (!heaps.fill(a2, a1)) return 0;
                if (heaps.data()) a2 = heaps.data();
            }
            return reinterpret_cast<ForwardFn>(real_vtable[pc])(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9,
                                                              a10, a11, a12, a13, a14);
        }
    }
}

// The two console list methods with float arguments. A thunk declared with integer words leaves XMM1-XMM3 to chance
// (its first-call log, for one, may use them), so these have the real prototype and call the PC method with it.
constexpr unsigned kListClearDepthStencil = 51;  // ClearDepthStencilView(view, flags, FLOAT depth, stencil, n, rects)
constexpr unsigned kListDepthBounds = 70;        // OMSetDepthBounds(FLOAT min, FLOAT max)
static_assert(slots::console_list_slot(kListClearDepthStencil).method == slots::ListMethod::typed &&
              slots::console_list_slot(kListDepthBounds).method == slots::ListMethod::typed);

template <unsigned Slot>
bool list_typed_slot_reaches(ListProxy* self) {
    constexpr unsigned pc = slots::console_list_slot(Slot).pc;
    if (pc < self->pc_slot_count) return true;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: list slot %u (%s) maps to PC slot %u, beyond the real list's %u slots; "
                             "not recorded\n", Slot, list_slot_name(Slot), pc, self->pc_slot_count);
    return false;
}

void WINAPI list_clear_depth_stencil_view(ListProxy* self, D3D12_CPU_DESCRIPTOR_HANDLE view, D3D12_CLEAR_FLAGS flags,
                                          FLOAT depth, UINT8 stencil, UINT count, const D3D12_RECT* rects) {
    note_list_slot(kListClearDepthStencil, view.ptr, static_cast<std::uint64_t>(flags), stencil, count);
    if (!list_typed_slot_reaches<kListClearDepthStencil>(self)) return;
    using Fn = void(WINAPI*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8,
                             UINT, const D3D12_RECT*);
    constexpr unsigned pc = slots::console_list_slot(kListClearDepthStencil).pc;
    reinterpret_cast<Fn>((*reinterpret_cast<void***>(self->real))[pc])(self->real, view, flags, depth, stencil, count,
                                                                        rects);
}

void WINAPI list_om_set_depth_bounds(ListProxy* self, FLOAT min, FLOAT max) {
    note_list_slot(kListDepthBounds);
    if (!list_typed_slot_reaches<kListDepthBounds>(self)) return;
    using Fn = void(WINAPI*)(ID3D12GraphicsCommandList*, FLOAT, FLOAT);
    constexpr unsigned pc = slots::console_list_slot(kListDepthBounds).pc;
    reinterpret_cast<Fn>((*reinterpret_cast<void***>(self->real))[pc])(self->real, min, max);
}

template <std::size_t... I>
void fill_list_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&list_slot<static_cast<unsigned>(I)>)), ...);
}

void** get_list_vtable() {
    static void** vtable = [] {
        static void* storage[kListSlots];
        fill_list_vtable(storage, std::make_index_sequence<kListSlots>{});
        storage[kListClearDepthStencil] = reinterpret_cast<void*>(&list_clear_depth_stencil_view);
        storage[kListDepthBounds] = reinterpret_cast<void*>(&list_om_set_depth_bounds);
        return storage;
    }();
    return vtable;
}

HRESULT wrap_list(IUnknown* created, DeviceProxy* device, const IID* riid, void** out, std::uint32_t type) {
    if (out) *out = nullptr;
    if (!created) return E_FAIL;
    auto* proxy = new (std::nothrow) ListProxy();
    if (!proxy) {
        created->Release();
        return E_OUTOFMEMORY;
    }
    proxy->vtable = get_list_vtable();
    proxy->real = static_cast<ID3D12GraphicsCommandList*>(created);
    proxy->list_type = type;
    proxy->reserved = 0;
    proxy->device = device;
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy->pc_slot_count = detect_list_slot_count(created);
    proxy->pin_cursor_at_limit();
    proxy_add_ref(device);

    const HRESULT hr = list_query_interface(proxy, riid, out);
    if (FAILED(hr)) {
        destroy_list(proxy);
        return hr;
    }
    static std::atomic<bool> logged{false};
    if (!d3d12_trace::enabled() && (!logged.exchange(true, std::memory_order_relaxed) || trace_enabled()))
        std::fprintf(stderr, "[rt] d3d12_xs: list proxy %p wraps %p (%u PC vtable slots)\n",
                     static_cast<void*>(proxy), static_cast<void*>(created), proxy->pc_slot_count);
    proxy->refs.fetch_sub(1, std::memory_order_relaxed);
    return S_OK;
}

// The vtable read for a slot past a table faults in the reservation behind it; name the slot. Only reads the
// exception record and writes with WriteFile, so it is safe whatever the faulting thread holds.
template <ChildKind K>
void report_table_overrun(std::uintptr_t address) {
    using S = ChildStats<K>;
    const std::uintptr_t begin = S::table_begin.load(std::memory_order_acquire);
    const std::uintptr_t end = S::table_end.load(std::memory_order_acquire);
    const std::uintptr_t limit = S::region_end.load(std::memory_order_acquire);
    if (!end || address < end || address >= limit) return;
    S::overruns.fetch_add(1, std::memory_order_relaxed);
    char line[160];
    unsigned long long slot = (address - begin) / sizeof(void*);
    char digits[24];
    int d = 0;
    do {
        digits[d++] = static_cast<char>('0' + slot % 10);
        slot /= 10;
    } while (slot && d < 23);
    int n = 0;
    const auto put = [&](const char* text) {
        while (*text && n < static_cast<int>(sizeof(line)) - 1) line[n++] = *text++;
    };
    put("[rt] d3d12_xs: ");
    put(child_kind_name<K>());
    put(" slot ");
    while (d > 0 && n < static_cast<int>(sizeof(line)) - 1) line[n++] = digits[--d];
    put(" is past the proxy table; the vtable read faults\n");
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(n), &written, nullptr);
}

LONG CALLBACK table_overrun_handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
    if (record && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const auto address = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
        report_table_overrun<ChildKind::queue>(address);
        report_table_overrun<ChildKind::fence>(address);
        report_table_overrun<ChildKind::allocator>(address);
    }
    return EXCEPTION_CONTINUE_SEARCH;  // diagnostics only: the fault goes on to the title's handlers or the crash
}

// ---- The console queue (d3d12_xs_slots.h, V queue.rs:957-1001) -------------------------------------------------

// Console queue slot 32, PresentX(plane count, planes, parameters): the device's present handler (set_present_handler;
// the Xbox presentation is the intended one), or accepted with nothing shown, as V queue.rs:757-841 always answers
// S_OK (logged once).
HRESULT queue_present_x(ChildProxy* self, std::uint64_t plane_count, std::uint64_t planes, std::uint64_t parameters) {
    DeviceProxy* const device = self->device;
    // What the queues hold for memory values runs first (V queue.rs:113-119, VICTORY_WAIT_FORCE): the present shows
    // the frame's work, and a wait is not kept across frames.
    const ListTitleHandlers* const title = g_list_title.load(std::memory_order_acquire);
    if (device && title && title->force_waits) title->force_waits(device->real);
    const PresentHandler handler = device ? device->present_handler.load(std::memory_order_acquire) : nullptr;
    if (handler)
        return handler(device->present_context.load(std::memory_order_acquire),
                       reinterpret_cast<ID3D12CommandQueue*>(self->real), static_cast<std::uint32_t>(plane_count),
                       reinterpret_cast<const void*>(planes), reinterpret_cast<const void*>(parameters));
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: queue slot 32 PresentX (%u planes) with no present handler: accepted, "
                             "nothing shown\n", static_cast<unsigned>(plane_count));
    return S_OK;
}

// The value waits (slots 73/74) of the lists a queue executes that the lists do not hold themselves. On the Vulkan
// device every list holds its waits (ListTitleHandlers::wait_value: its queue runs the parts between them as the
// values arrive, V queue.rs:94-210 and list.rs:290-450); this is for a device without that. VIctory holds the
// submission until the memory holds what a list waits for (V queue.rs:529-546, list.rs:1836-1871: the title's compute
// queue waits for a value the graphics queue's depth pass writes at the bottom of the pipe). Here the submitting thread
// waits for it, up to LEONIDA_VALUE_WAIT_MS (100 by default): a value earlier work of another queue writes (Write32/64,
// into title memory the GPU reaches at its address) arrives meanwhile. One that only later work would write never
// comes, so after the limit the submission goes on, counted (unmet_value_wait_submissions) and logged for the first
// ones.
unsigned value_wait_limit_ms() {
    static const unsigned limit = [] {
        char text[16] = {};
        const DWORD length = GetEnvironmentVariableA("LEONIDA_VALUE_WAIT_MS", text, sizeof(text));
        if (!length || length >= sizeof(text)) return 100u;
        return static_cast<unsigned>(std::strtoul(text, nullptr, 10));
    }();
    return limit;
}

void wait_for_values(std::uint64_t lists, std::uint64_t count) {
    if (!lists) return;
    const auto* objects = reinterpret_cast<void* const*>(lists);
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(count); ++i) {
        void* const object = objects[i];
        if (!object || *reinterpret_cast<void* const*>(object) != reinterpret_cast<void*>(get_list_vtable())) continue;
        for (const ValueWait& wait : static_cast<const ListProxy*>(object)->waits) {
            if (wait.held) continue;  // the list stands at it itself
            std::uint64_t value = 0;
            const ULONGLONG start = GetTickCount64();
            bool met = !read_value(wait.address, wait.wide, value) || wait.met_by(value);
            while (!met && GetTickCount64() - start < value_wait_limit_ms()) {
                Sleep(0);
                met = !read_value(wait.address, wait.wide, value) || wait.met_by(value);
            }
            if (met) continue;
            const std::uint64_t n = g_unmet_waits.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 4 || (n & (n - 1)) == 0)
                std::fprintf(stderr,
                             "[rt] d3d12_xs: a list waits for the value at 0x%llx (comparison %u, reference 0x%llx, %s) "
                             "and it is 0x%llx after %u ms: submitted all the same (%llu so far)\n",
                             static_cast<unsigned long long>(wait.address), wait.comparison,
                             static_cast<unsigned long long>(wait.reference), wait.wide ? "64-bit" : "32-bit",
                             static_cast<unsigned long long>(value), value_wait_limit_ms(),
                             static_cast<unsigned long long>(n));
        }
    }
}

// Console queue slot 31, CopyPageMappingsBatchX(count, batches, pool, pool pages, flags), ported from VIctory
// (GPL-3.0-or-later, Marvisak and contributors, DexNom fork), victory-gpu queue.rs:897-962: maps pages of a pool (64 KiB
// pages the title mapped physical pages into, device slot 74) at the batches' addresses, through the page mapper
// (set_page_mapper: xmem's). The title streams textures into pool pages and maps them into its tiled heap this way
// (V: ranges of one constant page or of unmapped pages). A range is a kind, a first pool page and a page count: 0 maps
// that many pool pages in order, 1 maps the one pool page at each page, 2 unmaps, 3 leaves the pages as they are. Done
// at once on the CPU, as V does: work already submitted keeps the pages it had (title memory leaves the GPU before it
// is remapped, src/title_memory.h), and work submitted later sees the new ones. A range that reaches past the pool, or
// a page the mapper refuses, is logged once; the call still answers S_OK, as V's does.
// shortcut: work the queue holds (behind a value or fence wait) runs after the remap, not before it.
struct PageRange {
    std::uint32_t kind, start, count;
};
struct PageBatch {
    std::uint64_t destination;
    std::uint32_t count;
    const PageRange* ranges;
};
static_assert(sizeof(PageRange) == 12 && sizeof(PageBatch) == 24 && offsetof(PageBatch, ranges) == 16);
constexpr std::uint64_t kMappedPage = 64 * 1024;

HRESULT queue_copy_page_mappings(std::uint64_t count_word, std::uint64_t batches_word, std::uint64_t pool,
                                 std::uint64_t pool_pages_word) {
    const PageMapper map = g_page_mapper.load(std::memory_order_acquire);
    if (!map) return E_INVALIDARG;
    const auto count = static_cast<std::uint32_t>(count_word);
    const auto pool_pages = static_cast<std::uint32_t>(pool_pages_word);
    const auto* const batches = reinterpret_cast<const PageBatch*>(batches_word);
    if (!batches && count) return E_INVALIDARG;
    bool ok = true;
    std::uint64_t mapped = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const PageBatch& batch = batches[i];
        if (!batch.ranges) continue;
        std::uint64_t at = batch.destination;
        for (std::uint32_t j = 0; j < batch.count; ++j) {
            const PageRange& r = batch.ranges[j];
            const std::uint64_t len = std::uint64_t(r.count) * kMappedPage;
            const auto inside = [&](std::uint32_t pages) { return std::uint64_t(r.start) + pages <= pool_pages; };
            const std::uint64_t source = pool + std::uint64_t(r.start) * kMappedPage;
            switch (r.kind) {
                case 0:
                    ok = inside(r.count) && map(at, source, len) && ok;
                    break;
                case 1: {
                    bool all = inside(1);
                    for (std::uint32_t k = 0; all && k < r.count; ++k) all = map(at + k * kMappedPage, source, kMappedPage);
                    ok = all && ok;
                    break;
                }
                case 2:
                    ok = map(at, 0, len) && ok;
                    break;
                case 3:
                    break;
                default:
                    ok = false;
                    break;
            }
            if (r.kind <= 2) mapped += r.count;
            at += len;
        }
    }
    static std::atomic<unsigned> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 4 || trace_enabled())
        std::fprintf(stderr,
                     "[rt] d3d12_xs: queue slot 31 CopyPageMappingsBatchX: %u batches, %llu pages from pool 0x%llx "
                     "(%u pages)%s\n",
                     count, static_cast<unsigned long long>(mapped), static_cast<unsigned long long>(pool),
                     pool_pages, ok ? "" : "; a mapping failed");
    if (!ok) {
        static std::atomic<bool> failed{false};
        if (!failed.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: a page mapping from pool 0x%llx failed (logged once)\n",
                         static_cast<unsigned long long>(pool));
    }
    return S_OK;
}

// Write64 with flag 2, the system timestamp (V queue.rs:697-731, Written::SystemTimestamp: QueryPerformanceCounter,
// stored when the list has run). After an ExecuteX whose lists asked for some, the queue signals a fence of its own
// behind the submission, and when the real fence gets there a thread-pool wait stores the counter at each address
// (in place: title memory is host memory; an address that is not writable is skipped). Like VIctory's, the time is when
// the whole submission has run, not when the GPU reached the write.
struct StampJob {
    std::vector<std::uint64_t> addresses;
    HANDLE event = nullptr;
    ID3D12Fence* fence = nullptr;
};

VOID CALLBACK system_stamps_due(PTP_CALLBACK_INSTANCE, PVOID context, PTP_WAIT wait, TP_WAIT_RESULT) {
    std::unique_ptr<StampJob> job(static_cast<StampJob*>(context));
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    for (const std::uint64_t address : job->addresses)
        WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &now.QuadPart, sizeof(now.QuadPart),
                           nullptr);
    CloseHandle(job->event);
    job->fence->Release();
    CloseThreadpoolWait(wait);
}

void queue_system_stamps(ChildProxy* self, std::uint64_t lists, std::uint64_t count) {
    if (!lists || !self->device) return;
    std::vector<std::uint64_t> addresses;
    const auto* objects = reinterpret_cast<void* const*>(lists);
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(count); ++i) {
        void* const object = objects[i];
        if (!object || *reinterpret_cast<void* const*>(object) != reinterpret_cast<void*>(get_list_vtable())) continue;
        const auto& stamps = static_cast<const ListProxy*>(object)->system_stamps;
        try {
            addresses.insert(addresses.end(), stamps.begin(), stamps.end());
        } catch (const std::bad_alloc&) {
            return;
        }
    }
    if (addresses.empty()) return;
    const auto refuse = [](const char* why) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: Write64 system timestamps: %s; they are written now (logged once)\n",
                         why);
    };
    const auto write_now = [&] {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        for (const std::uint64_t address : addresses)
            WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &now.QuadPart,
                               sizeof(now.QuadPart), nullptr);
    };
    ID3D12Fence* fence = self->stamp_fence.load(std::memory_order_acquire);
    if (!fence) {
        ID3D12Fence* made = nullptr;
        if (FAILED(self->device->real->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&made))) || !made) {
            refuse("no fence for the queue");
            return write_now();
        }
        ID3D12Fence* expected = nullptr;
        if (self->stamp_fence.compare_exchange_strong(expected, made, std::memory_order_acq_rel)) {
            fence = made;
        } else {
            made->Release();
            fence = expected;
        }
    }
    const std::uint64_t value = self->stamp_value.fetch_add(1, std::memory_order_relaxed) + 1;
    using SignalFn = HRESULT(WINAPI*)(void*, ID3D12Fence*, UINT64);
    std::unique_ptr<StampJob> job(new (std::nothrow) StampJob);  // nothing may throw out of a thunk
    if (!job) {
        refuse("out of memory");
        return write_now();
    }
    job->addresses = std::move(addresses);
    job->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    PTP_WAIT wait = job->event ? CreateThreadpoolWait(&system_stamps_due, job.get(), nullptr) : nullptr;
    if (!wait || FAILED(reinterpret_cast<SignalFn>(self->real_vtable[kQueueSignal])(self->real, fence, value)) ||
        FAILED(fence->SetEventOnCompletion(value, job->event))) {
        if (wait) CloseThreadpoolWait(wait);
        if (job->event) CloseHandle(job->event);
        refuse("the queue's fence could not be signalled or waited for");
        addresses = std::move(job->addresses);
        return write_now();
    }
    fence->AddRef();
    job->fence = fence;
    SetThreadpoolWait(wait, job.release()->event, nullptr);
}

// GetDesc into `out`: the PC method (this, hidden return pointer), which returns the pointer.
std::uint64_t queue_get_desc(ChildProxy* self, std::uint64_t out) {
    using Fn = std::uint64_t(WINAPI*)(void*, std::uint64_t);
    reinterpret_cast<Fn>(self->real_vtable[18])(self->real, out);
    return out;
}

template <unsigned Slot>
std::uint64_t console_queue_call(ChildProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                 std::uint64_t a4, std::uint64_t a5) {
    using slots::QueueMethod;
    constexpr slots::QueueSlot entry = slots::console_queue_slot(Slot);
    constexpr ChildKind K = ChildKind::queue;
    note_child_slot<K>(Slot, Layout::console);
    const char* const trace = child_trace_name<K>(Slot, Layout::console);
    if constexpr (entry.method == QueueMethod::refuse || entry.method == QueueMethod::open) {
        bool expected = false;
        if (ChildStats<K>::beyond_warned[Slot].compare_exchange_strong(expected, true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: queue slot %u (%s) is %s in the console layout; returning E_NOTIMPL (args "
                         "0x%llx 0x%llx 0x%llx 0x%llx)\n",
                         Slot, entry.name ? entry.name : "not in the table",
                         entry.method == QueueMethod::open ? "not implemented yet" : "unknown",
                         static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                         static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4));
        d3d12_trace::record(trace, "unsupported");
        return hr_to_u64(E_NOTIMPL);
    } else if constexpr (entry.method == QueueMethod::marker) {
        d3d12_trace::record(trace, "ignored");
        return 0;
    } else {
        d3d12_trace::record(trace);
        if constexpr (entry.method == QueueMethod::forward) {
            using Fn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                              std::uint64_t);
            return reinterpret_cast<Fn>(self->real_vtable[entry.pc])(self->real, a1, a2, a3, a4, a5);
        } else if constexpr (entry.method == QueueMethod::execute) {
            // ExecuteCommandLists(count, lists) and ExecuteX(count, lists, token, flags): the frame token and the
            // flags are not used (V queue.rs:595-599).
            UnwrappedArray lists;
            if (!lists.fill(a2, a1)) return hr_to_u64(E_OUTOFMEMORY);
            wait_for_values(a2, a1);
            using Fn = void(WINAPI*)(void*, UINT, std::uint64_t);
            reinterpret_cast<Fn>(self->real_vtable[entry.pc])(self->real, static_cast<UINT>(a1),
                                                              lists.data() ? lists.data() : a2);
            queue_system_stamps(self, a2, a1);
            return 0;
        } else if constexpr (entry.method == QueueMethod::signal) {
            // Signal(fence, value) and Signal2X(fence, value, bottom, top): the sync settings are not used
            // (V queue.rs:600-610). The fence's value word follows once the real fence gets there.
            using Fn = HRESULT(WINAPI*)(void*, std::uint64_t, UINT64);
            const HRESULT hr = reinterpret_cast<Fn>(self->real_vtable[entry.pc])(self->real, unwrap_object(a1), a2);
            if (SUCCEEDED(hr)) arm_fence_word(a1, a2);
            return hr_to_u64(hr);
        } else if constexpr (entry.method == QueueMethod::wait) {
            using Fn = HRESULT(WINAPI*)(void*, std::uint64_t, UINT64);
            return hr_to_u64(reinterpret_cast<Fn>(self->real_vtable[entry.pc])(self->real, unwrap_object(a1), a2));
        } else if constexpr (entry.method == QueueMethod::get_desc) {
            return queue_get_desc(self, a1);
        } else if constexpr (entry.method == QueueMethod::page_mappings) {
            return hr_to_u64(queue_copy_page_mappings(a1, a2, a3, a4));
        } else if constexpr (entry.method == QueueMethod::accept) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr, "[rt] d3d12_xs: queue slot %u (%s) accepted: nothing suspends a title here\n",
                             Slot, entry.name);
            return hr_to_u64(S_OK);
        } else {
            static_assert(entry.method == QueueMethod::present);
            return hr_to_u64(queue_present_x(self, a1, a2, a3));
        }
    }
}

template <ChildKind K, unsigned Slot>
std::uint64_t WINAPI child_slot(ChildProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                                std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                                std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        d3d12_trace::record(child_trace_name<K>(Slot));
        return hr_to_u64(child_query_interface<K>(self, reinterpret_cast<const IID*>(a1),
                                                  reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return child_add_ref<K>(self);
    } else if constexpr (Slot == 2) {
        return child_release<K>(self);
    } else if constexpr (Slot == 7) {
        d3d12_trace::record(child_trace_name<K>(Slot));
        return hr_to_u64(child_get_device<K>(self, reinterpret_cast<const IID*>(a1),
                                             reinterpret_cast<void**>(a2)));
    } else {
        if constexpr (K == ChildKind::queue) {
            if constexpr (slots::console_queue_slot(Slot).method == slots::QueueMethod::get_desc) {
                // The return buffer may come first, with `this` second, as device slots 25/26 get it (V
                // resource.rs:2583-2593; L retail_boot_chain §3.1). V queue.rs:950 takes `this` first. A call is taken
                // as swapped only when the first register is not a queue proxy and the second is; both point at
                // readable memory either way (the queue, or the caller's 16-byte return buffer).
                if (!is_child_proxy<K>(self) && is_child_proxy<K>(reinterpret_cast<const void*>(a1))) {
                    auto* const queue = reinterpret_cast<ChildProxy*>(a1);
                    note_child_slot<K>(Slot, Layout::console);
                    d3d12_trace::record(child_trace_name<K>(Slot, Layout::console));
                    return queue_get_desc(queue, reinterpret_cast<std::uint64_t>(self));
                }
            }
            if (self->layout == Layout::console) return console_queue_call<Slot>(self, a1, a2, a3, a4, a5);
        } else if constexpr (K == ChildKind::fence) {
            if constexpr (Slot == slots::kFenceValueAddressSlot || Slot == 8) {
                if (self->word_listed) {  // a console fence: its value word (see "Fence value words")
                    note_child_slot<K>(Slot, Layout::console);
                    d3d12_trace::record(child_trace_name<K>(Slot, Layout::console));
                    const std::uint64_t value = refresh_fence_word(self);
                    if constexpr (Slot == 8) return value;  // GetCompletedValue
                    else return reinterpret_cast<std::uint64_t>(&self->value_word);
                }
            }
        } else if constexpr (Slot == slots::kAllocatorCallbackSlot) {
            if (self->layout == Layout::console) {  // V list.rs:1378-1387: an allocation callback, ignored
                note_child_slot<K>(Slot, Layout::console);
                d3d12_trace::record(child_trace_name<K>(Slot, Layout::console), "ignored");
                return hr_to_u64(S_OK);
            }
        }
        note_child_slot<K>(Slot, self->layout);
        if (Slot >= self->slot_count) {
            bool expected = false;
            if (ChildStats<K>::beyond_warned[Slot].compare_exchange_strong(expected, true,
                                                                            std::memory_order_relaxed)) {
                std::fprintf(stderr,
                             "[rt] d3d12_xs: %s slot %u is beyond the PC %s vtable (%u slots); returning E_NOTIMPL "
                             "(args 0x%llx 0x%llx 0x%llx 0x%llx)\n",
                             child_kind_name<K>(), Slot, child_kind_name<K>(), self->slot_count,
                             static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                             static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4));
            }
            d3d12_trace::record(child_trace_name<K>(Slot, self->layout), "unsupported");
            return hr_to_u64(E_NOTIMPL);
        }
        // "called": the real object's result is not inspected
        d3d12_trace::record(child_trace_name<K>(Slot, self->layout));
        if constexpr (K == ChildKind::queue && (Slot == kQueueSignal || Slot == kQueueWait)) a1 = unwrap_object(a1);
        UnwrappedArray unwrapped_lists;
        if constexpr (K == ChildKind::queue && Slot == 10) {
            if (!unwrapped_lists.fill(a2, a1)) return hr_to_u64(E_OUTOFMEMORY);
            if (unwrapped_lists.data()) a2 = unwrapped_lists.data();
        }

        // The PC vtable entry with the real object as `this`; integer/pointer arguments, the hidden struct-return
        // pointer and stack arguments keep their positions, as in the device proxy.
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
        if constexpr (K == ChildKind::fence && Slot == 10) {  // Signal(value) from the CPU: the word moves with it
            if (self->word_listed) {
                const std::uint64_t result =
                    fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
                refresh_fence_word(self);
                return result;
            }
        }
        return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }
}

template <ChildKind K, std::size_t... I>
void fill_child_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&child_slot<K, static_cast<unsigned>(I)>)), ...);
}

// True when the fault logger is installed (once per process; a failure is logged once).
bool install_table_overrun_handler() {
    static const bool installed = [] {
        if (AddVectoredExceptionHandler(0, &table_overrun_handler)) return true;
        std::fprintf(stderr, "[rt] d3d12_xs: AddVectoredExceptionHandler failed (error %lu); a slot past a queue or "
                     "fence proxy table faults without being named\n", GetLastError());
        return false;
    }();
    return installed;
}

template <ChildKind K>
void** get_child_vtable() {
    static void** vtable = [] {
        using S = ChildStats<K>;
        constexpr std::size_t bytes = S::kSlots * sizeof(void*);
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const SIZE_T page = system.dwPageSize;
        if (void* base = page >= bytes ? VirtualAlloc(nullptr, kTableRegion, MEM_RESERVE, PAGE_NOACCESS) : nullptr) {
            if (VirtualAlloc(base, page, MEM_COMMIT, PAGE_READWRITE)) {
                auto* table = reinterpret_cast<void**>(static_cast<char*>(base) + page - bytes);
                fill_child_vtable<K>(table, std::make_index_sequence<S::kSlots>{});
                DWORD old = 0;
                if (!VirtualProtect(base, page, PAGE_READONLY, &old))  // the table works, but stays writable
                    std::fprintf(stderr, "[rt] d3d12_xs: %s proxy table stays writable (VirtualProtect error %lu)\n",
                                 child_kind_name<K>(), GetLastError());
                S::table_begin.store(reinterpret_cast<std::uintptr_t>(table), std::memory_order_release);
                S::region_end.store(reinterpret_cast<std::uintptr_t>(base) + kTableRegion, std::memory_order_release);
                // table_end is what "guarded" means (command_object_tables_guarded, the fault logger): set only when
                // a fault past the table will be named. Without the logger the region still faults, unnamed.
                if (install_table_overrun_handler())
                    S::table_end.store(reinterpret_cast<std::uintptr_t>(base) + page, std::memory_order_release);
                return table;
            }
            VirtualFree(base, 0, MEM_RELEASE);
        }
        // No reservation: the table still works, but a slot past it is not caught.
        std::fprintf(stderr, "[rt] d3d12_xs: %s proxy table without a guard region\n", child_kind_name<K>());
        static void* storage[S::kSlots];
        fill_child_vtable<K>(storage, std::make_index_sequence<S::kSlots>{});
        return storage;
    }();
    return vtable;
}

// Wraps `created` (one reference, owned by the proxy on success, released on failure) and returns `riid` from the
// proxy through `out`.
template <ChildKind K>
HRESULT wrap_child(IUnknown* created, DeviceProxy* device, const IID* riid, void** out, std::uint32_t type = 0) {
    if (!created) return E_FAIL;
    ChildProxy* proxy = new (std::nothrow) ChildProxy;
    if (!proxy) {
        created->Release();
        return E_OUTOFMEMORY;
    }
    proxy->vtable = get_child_vtable<K>();
    proxy->real = created;
    proxy->device = device;
    proxy->real_vtable = *reinterpret_cast<void***>(created);
    if constexpr (K == ChildKind::queue) {
        proxy->slot_count = kQueuePublicSlots;
    } else if constexpr (K == ChildKind::fence) {
        proxy->slot_count = supports_iid(created, __uuidof(ID3D12Fence1)) ? kFence1PublicSlots : kFencePublicSlots;
    } else {
        proxy->slot_count = kAllocatorPublicSlots;
    }
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy->layout = device->layout;
    // A queue's or allocator's list type (V queue.rs:31-35, list.rs:90-94); a fence's value word is set when it is
    // listed (list_fence_word, below).
    proxy->value_word.store(K == ChildKind::fence ? 0 : type, std::memory_order_relaxed);
    proxy->word_prev = proxy->word_next = nullptr;
    proxy->word_listed = false;
    proxy->stamp_fence.store(nullptr, std::memory_order_relaxed);
    proxy->stamp_value.store(0, std::memory_order_relaxed);
    proxy_add_ref(device);  // the object keeps its Xbox-ABI device alive for GetDevice

    const HRESULT hr = child_query_interface<K>(proxy, riid, out);
    if (FAILED(hr)) {
        destroy_child(proxy);  // no reference escaped
        return hr;
    }
    if constexpr (K == ChildKind::fence) {
        if (proxy->layout == Layout::console) list_fence_word(proxy);  // slot 12's word, from the completed value
    }
    // Titles create fences at run time: the first proxy of each kind is logged, every one only with XS_TRACE.
    static std::atomic<bool> logged{false};
    if (!d3d12_trace::enabled() && (!logged.exchange(true, std::memory_order_relaxed) || trace_enabled()))
        std::fprintf(stderr, "[rt] d3d12_xs: %s proxy %p wraps %p (device proxy %p, %u public slots, %s layout)\n",
                     child_kind_name<K>(), static_cast<void*>(proxy), static_cast<void*>(created),
                     static_cast<void*>(device), proxy->slot_count,
                     proxy->layout == Layout::console ? "console" : "PC");
    proxy->refs.fetch_sub(1, std::memory_order_relaxed);  // drop the creation reference; `out` holds the caller's
    return S_OK;
}

// Calls the real device's slot `Slot` with these five words in the positions the thunk received them (see "A thunk
// declares 14 argument words" at the top: no word may move to another stack position on the way).
template <unsigned Slot>
std::uint64_t forward_device(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                             std::uint64_t a4, std::uint64_t a5) {
    using Fn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t);
    return reinterpret_cast<Fn>(self->real_vtable[Slot])(self->real, a1, a2, a3, a4, a5);
}

// A device slot past the PC device's vtable (the real device predates the interface): E_NOTIMPL, logged once per
// slot. The generic thunk and the slots the proxy handles itself (31/34/35/45/46/50, 75) log the same line.
HRESULT refuse_beyond_device(unsigned slot, unsigned slot_count) {
    bool expected = false;
    if (slot < kMaxSlots && g_beyond_warned[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: device slot %u (%s) is beyond the PC device vtable (%u slots); returning "
                     "E_NOTIMPL\n",
                     slot, slot_name(slot), slot_count);
    return E_NOTIMPL;
}

// A queue proxy over a new real queue made from a PC description. Without an output it is the device's own capability
// answer (S_FALSE for a valid description on the PC), as #558 does for slot 14.
HRESULT create_queue_proxy(DeviceProxy* self, const D3D12_COMMAND_QUEUE_DESC* desc, const IID* riid, void** out) {
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure, as slots 14/62/76
    if (!riid) return E_POINTER;
    // The caller's null output goes to the real device on purpose: its answer (S_FALSE, E_UNEXPECTED, ...) is the
    // one to give, although the SDK annotates the parameter as required.
#pragma warning(suppress : 6387)
    if (!out) return self->real->CreateCommandQueue(desc, *riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateCommandQueue(desc, __uuidof(ID3D12CommandQueue), &created);
    if (FAILED(hr)) return hr;
    return wrap_child<ChildKind::queue>(static_cast<IUnknown*>(created), self, riid, out,
                                        desc ? static_cast<std::uint32_t>(desc->Type) : 0);
}

// Device slot 8: CreateCommandQueue(desc, riid, out), the PC description.
HRESULT proxy_create_command_queue(DeviceProxy* self, const D3D12_COMMAND_QUEUE_DESC* desc, const IID* riid,
                                   void** out) {
    note_slot(8);
    return create_queue_proxy(self, desc, riid, out);
}

// Xbox device slot 48: CreateCommandQueueX(desc, riid, out). The console description is {type, flags, engine or pipe,
// queue} (V queue.rs:464-495; the title's compute queue is {COMPUTE, 0, pipe 1, queue 0}, queue.rs:1049-1064), not the
// PC {Type, Priority, Flags, NodeMask}: read as a PC one, the pipe and queue numbers land in Flags and NodeMask, which
// the PC runtime refuses (L retail_boot_chain §3.2 had to limit Flags; on the title the PC runtime threw). The queue
// is made from the type, as VIctory does: the host picks its engines. Of the console flags only bit 0 is kept, as the
// PC's DISABLE_GPU_TIMEOUT, the one PC queue flag; the others are logged once.
HRESULT proxy_create_command_queue_x(DeviceProxy* self, std::uint64_t desc_word, const IID* riid, void** out) {
    if (out) *out = nullptr;
    if (!desc_word) return E_INVALIDARG;  // V queue.rs:483-486
    std::uint32_t words[4] = {};
    std::memcpy(words, reinterpret_cast<const void*>(desc_word), sizeof(words));
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = static_cast<D3D12_COMMAND_LIST_TYPE>(words[0]);
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = static_cast<D3D12_COMMAND_QUEUE_FLAGS>(words[1] & D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT);
    desc.NodeMask = 0;
    static std::atomic<unsigned> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8 || trace_enabled())
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 48 CreateCommandQueueX {type %u, flags 0x%x, engine/pipe %u, queue %u} -> PC "
                     "queue of type %u, flags 0x%x\n",
                     words[0], words[1], words[2], words[3], static_cast<unsigned>(desc.Type),
                     static_cast<unsigned>(desc.Flags));
    return create_queue_proxy(self, &desc, riid, out);
}

// ---- The frame clock (device slots 78-81; V frame.rs) ------------------------------------------------------------
//
// VIctory's clock, which paces the title: started by the first SetFrameIntervalX (60 Hz until then), it ticks once per
// interval; the token is the frame number. Each event kind (origin, vblank, flip, and later kinds with the flip)
// happens once per tick, so two waits for the same kind never end on the same tick: the title polls the origin with
// "token on time-out" (starting its next frame at once) and is paced by a blocking wait for the flip before each
// present (frame.rs:1-10). Event offsets are ignored, as in VIctory: every event is at the tick. A wait that times
// out returns S_FALSE (frame.rs:19-20). This replaces the public HLE's stand-in, whose WaitFrameEventX returned at once
// with a fresh token for any event: that never paced, and it made a token on a time-out poll, where VIctory makes one
// only with the token-on-time-out flag.
//
// The shims' own presentation (xbox_present.h's FramePacer) models only the origin event and refuses others, so the
// title's flip waits could not go there; it is not what these slots answer.
constexpr std::uint64_t kSixtiethNs = 16'667'000;  // frame.rs:26
constexpr std::uint32_t kFrameOrigin = 0;          // frame.rs:21-22
constexpr std::uint32_t kTokenOnTimeout = 2;       // frame.rs:23-25

std::int64_t qpc_now() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

std::uint64_t ns_since(std::int64_t start) {
    static const std::uint64_t frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? static_cast<std::uint64_t>(f.QuadPart) : 1;
    }();
    const std::int64_t now = qpc_now();
    const std::uint64_t ticks = now > start ? static_cast<std::uint64_t>(now - start) : 0;
    return ticks / frequency * 1'000'000'000ull + ticks % frequency * 1'000'000'000ull / frequency;
}

// Sleeps `ns`. A high-resolution waitable timer (Windows 10 1803 and later) keeps a frame wait well under a
// millisecond late; Sleep's 15.6 ms default granularity would not. Made per wait (no thread_local in this module:
// gta6rt.exe must not register a thread-exit callback with the CRT, which takes one per process).
void sleep_ns(std::uint64_t ns) {
    if (!ns) return;
    if (HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_ALL_ACCESS)) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(ns / 100 + (ns % 100 != 0));  // relative, in 100 ns units
        const bool waited = SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) &&
                            WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0;
        CloseHandle(timer);
        if (waited) return;
    }
    Sleep(static_cast<DWORD>(std::min<std::uint64_t>((ns + 999'999) / 1'000'000, 0xfffffffeull)));
}

std::atomic<std::uint64_t>& frame_last(DeviceProxy* self, std::uint32_t kind) {
    return self->frame_last[kind < 2 ? kind : 2];  // frame.rs:91-97
}

// Device slot 78: SetFrameIntervalX(output, length_us, period, flags), frame.rs:46-73. A frame every `length`
// microseconds (0: 60 Hz); the period is pipeline depth, not part of the rate. A new interval starts from now, after
// the last tick handed out.
HRESULT frame_set_interval(DeviceProxy* self, std::uint64_t length_us) {
    const auto us = static_cast<std::uint32_t>(length_us);
    std::uint64_t first = 0;
    for (const auto& last : self->frame_last) first = std::max(first, last.load(std::memory_order_relaxed));
    AcquireSRWLockExclusive(&self->frame_lock);
    self->frame_clock_set = true;
    self->frame_start = qpc_now();
    self->frame_interval_ns = us ? std::uint64_t{us} * 1000 : kSixtiethNs;
    self->frame_first = first;
    ReleaseSRWLockExclusive(&self->frame_lock);
    return S_OK;
}

// frame.rs:99-118: the next tick after now and after the last one a wait for `kind` ended on, and how long until it.
std::uint64_t next_frame_tick(DeviceProxy* self, std::uint32_t kind, std::uint64_t* wait_ns) {
    AcquireSRWLockExclusive(&self->frame_lock);
    if (!self->frame_clock_set) {
        self->frame_clock_set = true;
        self->frame_start = qpc_now();
        self->frame_interval_ns = kSixtiethNs;
        self->frame_first = 0;
    }
    const std::uint64_t elapsed = ns_since(self->frame_start);
    const std::uint64_t interval = std::max<std::uint64_t>(self->frame_interval_ns, 1);
    const std::uint64_t first = self->frame_first;
    ReleaseSRWLockExclusive(&self->frame_lock);
    const std::uint64_t tick =
        std::max(first + elapsed / interval + 1, frame_last(self, kind).load(std::memory_order_relaxed) + 1);
    const std::uint64_t at = (tick - first) * interval;
    *wait_ns = at > elapsed ? at - elapsed : 0;
    return tick;
}

// Device slot 80: WaitFrameEventX(kind, timeout_ms, list, flags, &token), frame.rs:120-184. Blocks until the next
// tick (or the time-out); an origin wait then hands out the new frame's token. Ticks are absolute, so lateness never
// adds up.
HRESULT frame_wait(DeviceProxy* self, std::uint64_t kind_word, std::uint64_t timeout_word, std::uint64_t flags_word,
                   std::uint64_t token_word) {
    const auto kind = static_cast<std::uint32_t>(kind_word);
    const auto flags = static_cast<std::uint32_t>(flags_word);
    const std::uint64_t limit = std::uint64_t{static_cast<std::uint32_t>(timeout_word)} * 1'000'000;
    std::atomic<std::uint64_t>& last = frame_last(self, kind);
    std::uint64_t wait = 0;
    std::uint64_t tick = next_frame_tick(self, kind, &wait);
    // A blocking wait for an event that has happened since the last wait for it ended (a frame took longer than a
    // tick) ends at once, on the tick that passed (frame.rs:135-148). Polls answer as before.
    if (limit >= wait && tick > last.load(std::memory_order_relaxed) + 1) {
        --tick;
        wait = 0;
    }
    HRESULT hr = S_OK;
    if (wait > limit) {
        sleep_ns(limit);
        hr = S_FALSE;  // the time-out
    } else {
        sleep_ns(wait);
        std::uint64_t seen = last.load(std::memory_order_relaxed);
        while (seen < tick && !last.compare_exchange_weak(seen, tick, std::memory_order_relaxed)) {
        }
    }
    // A frame begun on a time-out takes no tick: its flip still does. Tokens count frames, not ticks.
    if (kind == kFrameOrigin && (hr == S_OK || (flags & kTokenOnTimeout))) {
        const std::uint64_t frame = self->frames_begun.fetch_add(1, std::memory_order_relaxed) + 1;
        if (auto* token = reinterpret_cast<std::uint64_t*>(token_word)) *token = frame;
        leonida::pipelines::begin_frame();  // a new budget for waiting on pipelines (make.rs `wait`)
    }
    return hr;
}

// Device slot 81: GetFrameStatisticsX(token, kinds, &count, out), frame.rs:186-199: no statistics are kept.
HRESULT frame_statistics(std::uint64_t count_word) {
    if (auto* count = reinterpret_cast<std::uint32_t*>(count_word)) *count = 0;
    return S_OK;
}

// Xbox device slot 73: CreateCommandListX(desc, allocator, initial_state, riid, out). The console call is not the PC
// six-argument form: the description is {type, flags, node mask, barrier batch size} (V list.rs:525-550) and there is
// no node-mask argument. The list is made as an ID3D12GraphicsCommandList and handed out as a console list proxy, as
// the interface the title asks for: that is a console list interface the PC runtime does not know (V list.rs:33-88;
// L retail_boot_chain §3.3).
HRESULT proxy_create_command_list_x(DeviceProxy* self, std::uint64_t desc_word, std::uint64_t allocator_word,
                                    std::uint64_t initial_state_word, const IID* riid, void** out) {
    if (out) *out = nullptr;
    if (!desc_word || !allocator_word) return E_INVALIDARG;
    if (!riid) return E_POINTER;
    if (!out) return E_POINTER;

    std::uint32_t words[2] = {};
    std::memcpy(words, reinterpret_cast<const void*>(desc_word), sizeof(words));
    auto type = static_cast<D3D12_COMMAND_LIST_TYPE>(words[0]);
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_BUNDLE &&
        type != D3D12_COMMAND_LIST_TYPE_COMPUTE && type != D3D12_COMMAND_LIST_TYPE_COPY)
        type = D3D12_COMMAND_LIST_TYPE_DIRECT;  // as the public HLE does

    static std::atomic<bool> logged{false};
    if (first_call(logged))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 73 CreateCommandListX desc words 0x%08x 0x%08x -> node mask 0, type %d\n",
                     words[0], words[1], static_cast<int>(type));

    // Pipeline states are not wrapped by this proxy, so initial_state is already the real object.
    const auto allocator = reinterpret_cast<ID3D12CommandAllocator*>(unwrap_object(allocator_word));
    auto* initial_state = reinterpret_cast<ID3D12PipelineState*>(initial_state_word);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateCommandList(0, type, allocator, initial_state,
                                                     __uuidof(ID3D12GraphicsCommandList), &created);
    if (FAILED(hr)) return hr;
    return wrap_list(static_cast<IUnknown*>(created), self, riid, out, static_cast<std::uint32_t>(type));
}

// Device slot 12: CreateCommandList(node_mask, type, allocator, initial_state, riid, out). The console layout hands
// out a console list proxy (V device.rs:413, list.rs:566); the PC layout the real list, as before, for Leonida's own
// tests and fixtures that record through the PC interface.
HRESULT proxy_create_command_list(DeviceProxy* self, std::uint64_t node_mask, std::uint64_t type,
                                  std::uint64_t allocator, std::uint64_t initial_state, const IID* riid, void** out) {
    note_slot(12);
    allocator = unwrap_object(allocator);
    if (12 >= self->slot_count) return refuse_beyond_device(12, self->slot_count);
    using Fn = HRESULT(WINAPI*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
                                ID3D12PipelineState*, const IID*, void**);
    const auto create = reinterpret_cast<Fn>(self->real_vtable[12]);
    const auto list_type = static_cast<D3D12_COMMAND_LIST_TYPE>(static_cast<std::uint32_t>(type));
    auto* const list_allocator = reinterpret_cast<ID3D12CommandAllocator*>(allocator);
    auto* const pipeline = reinterpret_cast<ID3D12PipelineState*>(initial_state);
    if (self->layout != Layout::console)
        return create(self->real, static_cast<UINT>(node_mask), list_type, list_allocator, pipeline, riid, out);
    if (out) *out = nullptr;
    if (!riid) return E_POINTER;
    if (!out)  // a capability check: the device's own answer for the interface the proxy is made over
        return create(self->real, static_cast<UINT>(node_mask), list_type, list_allocator, pipeline,
                      &__uuidof(ID3D12GraphicsCommandList), nullptr);
    void* created = nullptr;
    const HRESULT hr = create(self->real, static_cast<UINT>(node_mask), list_type, list_allocator, pipeline,
                              &__uuidof(ID3D12GraphicsCommandList), &created);
    if (FAILED(hr)) return hr;
    return wrap_list(static_cast<IUnknown*>(created), self, riid, out, static_cast<std::uint32_t>(list_type));
}

// Device slot 75: ID3D12Device9::CreateCommandQueue1(desc, creator_id, riid, out), called by slot index.
HRESULT proxy_create_command_queue1(DeviceProxy* self, std::uint64_t desc, std::uint64_t creator_id,
                                    const IID* riid, void** out) {
    note_slot(75);
    if (out) *out = nullptr;  // as slot 8
    if (self->slot_count <= 75) return refuse_beyond_device(75, self->slot_count);  // predates ID3D12Device9
    if (!riid) return E_POINTER;
    using Fn = HRESULT(WINAPI*)(ID3D12Device*, std::uint64_t, std::uint64_t, const IID*, void**);
    const auto create = reinterpret_cast<Fn>(self->real_vtable[75]);
    if (!out) return create(self->real, desc, creator_id, riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = create(self->real, desc, creator_id, &__uuidof(ID3D12CommandQueue), &created);
    if (FAILED(hr)) return hr;
    const std::uint32_t type =
        desc ? static_cast<std::uint32_t>(reinterpret_cast<const D3D12_COMMAND_QUEUE_DESC*>(desc)->Type) : 0;
    return wrap_child<ChildKind::queue>(static_cast<IUnknown*>(created), self, riid, out, type);
}

// Device slot 36: CreateFence(initial_value, flags, riid, out).
HRESULT proxy_create_fence(DeviceProxy* self, std::uint64_t initial_value, std::uint64_t flags, const IID* riid,
                           void** out) {
    note_slot(36);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    const auto fence_flags = static_cast<D3D12_FENCE_FLAGS>(static_cast<std::uint32_t>(flags));
#pragma warning(suppress : 6387)  // the caller's null output, on purpose (see slot 8)
    if (!out) return self->real->CreateFence(initial_value, fence_flags, *riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateFence(initial_value, fence_flags, __uuidof(ID3D12Fence), &created);
    if (FAILED(hr)) return hr;
    return wrap_child<ChildKind::fence>(static_cast<IUnknown*>(created), self, riid, out);
}

// True when `allocator` is one whose consumers resolve allocator proxies: Leonida's own Vulkan-layer allocator,
// whose lists (CreateCommandList, Reset) find the real allocator through allocator_proxy_real_allocator. A PC
// runtime allocator does not answer the tag, and its lists would take a proxy for their own allocator.
bool resolves_allocator_proxies(IUnknown* allocator) {
    void* found = nullptr;
    if (FAILED(allocator->QueryInterface(kLeonidaCommandAllocatorTag, &found)) || !found) return false;
    static_cast<IUnknown*>(found)->Release();  // the implementation pointer starts with its IUnknown chain
    return true;
}

// Device slot 9: CreateCommandAllocator(type, riid, out). In the PC layout an allocator proxy where the list side
// resolves it (see resolves_allocator_proxies); any other allocator goes back unchanged, logged once, so the host
// runtime's own lists never see a proxy. In the console layout always a proxy (the lists are proxies too). Null
// output and null IID as slot 8.
HRESULT proxy_create_command_allocator(DeviceProxy* self, std::uint64_t type, const IID* riid, void** out) {
    note_slot(9);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    const auto list_type = static_cast<D3D12_COMMAND_LIST_TYPE>(static_cast<std::uint32_t>(type));
#pragma warning(suppress : 6387)  // the caller's null output, on purpose (see slot 8)
    if (!out) return self->real->CreateCommandAllocator(list_type, *riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateCommandAllocator(list_type, __uuidof(ID3D12CommandAllocator), &created);
    if (FAILED(hr)) return hr;
    if (!created) return E_FAIL;
    auto* allocator = static_cast<IUnknown*>(created);
    // In the console layout every list is a list proxy, which unwraps the allocator wherever it reaches the runtime
    // (creation, Reset, ResetX), so every allocator is wrapped there.
    if (self->layout != Layout::console && !resolves_allocator_proxies(allocator)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: allocators of this device are not wrapped: its command lists "
                                 "cannot resolve an allocator proxy\n");
        const HRESULT asked = allocator->QueryInterface(*riid, out);
        allocator->Release();
        return asked;
    }
    return wrap_child<ChildKind::allocator>(allocator, self, riid, out, static_cast<std::uint32_t>(list_type));
}

// Device slot 32: OpenSharedHandle(handle, riid, out). The object is opened as asked; when it is a fence (whatever
// interface was asked for, IUnknown included) the title gets a fence proxy, anything else (heaps, resources) is
// returned unchanged.
HRESULT proxy_open_shared_handle(DeviceProxy* self, std::uint64_t handle, const IID* riid, void** out) {
    note_slot(32);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    const auto shared = reinterpret_cast<HANDLE>(handle);
    if (!out) return self->real->OpenSharedHandle(shared, *riid, nullptr);
    void* opened = nullptr;
    HRESULT hr = self->real->OpenSharedHandle(shared, *riid, &opened);
    if (FAILED(hr) || !opened) return FAILED(hr) ? hr : E_FAIL;
    void* fence = nullptr;
    if (FAILED(static_cast<IUnknown*>(opened)->QueryInterface(__uuidof(ID3D12Fence), &fence)) || !fence) {
        *out = opened;  // not a fence
        return hr;
    }
    static_cast<IUnknown*>(opened)->Release();
    return wrap_child<ChildKind::fence>(static_cast<IUnknown*>(fence), self, riid, out);
}

// Device slots that take heap, queue or fence objects: the real runtime gets the real objects.
//   31 CreateSharedHandle(object, attributes, access, name, handle)
//   34 MakeResident(count, objects)      35 Evict(count, objects)
//   45 SetEventOnMultipleFenceCompletion(fences, values, count, flags, event)            (ID3D12Device1)
//   46 SetResidencyPriority(count, objects, priorities)                                  (ID3D12Device1)
//   50 EnqueueMakeResident(flags, count, objects, fence, value)                          (ID3D12Device3)
template <unsigned Slot>
HRESULT proxy_unwrapping_call(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                              std::uint64_t a4, std::uint64_t a5) {
    note_slot(Slot);
    if (Slot >= self->slot_count) return refuse_beyond_device(Slot, self->slot_count);  // predates the interface
    UnwrappedArray objects;
    std::uint64_t r1 = a1, r2 = a2, r3 = a3, r4 = a4;
    if constexpr (Slot == 31) {
        r1 = unwrap_object(a1);
    } else if constexpr (Slot == 34 || Slot == 35 || Slot == 46) {
        if (!objects.fill(a2, a1)) return E_OUTOFMEMORY;
        if (objects.data()) r2 = objects.data();
    } else if constexpr (Slot == 45) {
        if (!objects.fill(a1, a3)) return E_OUTOFMEMORY;
        if (objects.data()) r1 = objects.data();
    } else if constexpr (Slot == 50) {
        if (!objects.fill(a3, a2)) return E_OUTOFMEMORY;
        if (objects.data()) r3 = objects.data();
        r4 = unwrap_object(a4);
    }
    // r1-r4 differ from a1-a4 only in a position the method takes, so the title's frame has that word.
    return static_cast<HRESULT>(static_cast<std::uint32_t>(forward_device<Slot>(self, r1, r2, r3, r4, a5)));
}

static bool readable_range(const void* ptr, std::size_t bytes) {
    if (!ptr || !bytes) return false;
    const auto* p = static_cast<const std::uint8_t*>(ptr);
    std::size_t remaining = bytes;
    while (remaining) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
        const DWORD protect = mbi.Protect & 0xffu;
        if (protect == PAGE_NOACCESS || protect == PAGE_GUARD || protect == PAGE_EXECUTE) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto addr = reinterpret_cast<std::uintptr_t>(p);
        const std::size_t avail = static_cast<std::size_t>(base + mbi.RegionSize - addr);
        if (avail >= remaining) return true;
        remaining -= avail;
        p += avail;
    }
    return true;
}

HRESULT proxy_create_descriptor_heap(DeviceProxy* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc, const IID* riid,
                                     void** out) {
    note_slot(14);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure, as the PC device does
    if (!riid) return E_POINTER;
    // No output: a capability check, answered by the device (S_FALSE, as on the PC).
    if (!out) return self->real->CreateDescriptorHeap(desc, *riid, nullptr);

    void* created = nullptr;
    HRESULT hr = E_INVALIDARG;
    bool console_extended = false;
#ifdef LEONIDA_VULKAN_DEVICE
    if (desc && desc->Type <= D3D12_DESCRIPTOR_HEAP_TYPE_DSV &&
        (desc->Flags & ~D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == 0) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(desc);
        const void* tail = bytes + sizeof(D3D12_DESCRIPTOR_HEAP_DESC);
        if (readable_range(tail, sizeof(std::uint64_t) * 2)) {
            std::uint64_t address = 0;
            std::uint64_t size = 0;
            std::memcpy(&address, tail, sizeof(address));
            std::memcpy(&size, bytes + sizeof(D3D12_DESCRIPTOR_HEAP_DESC) + sizeof(address), sizeof(size));
            // Console-extended slot 14 (issue #313): address/size follow the PC desc. The title also passes the plain
            // 16-byte description (its CPU-only view heaps of 3128 descriptors), and then the words after it are its
            // stack: 0x20, 0xa00 or a stack address were taken as heap starts, and descriptors were written at 0x200
            // (game+0x4d9f113). The tail is a placement only when its size is the heap's, count times increment, as
            // VIctory tells them apart (victory-gpu descriptor.rs create_heap).
            const std::uint64_t heap_bytes = std::uint64_t(desc->NumDescriptors) *
                                             self->real->GetDescriptorHandleIncrementSize(desc->Type);
            if (address != 0 && size == heap_bytes && heap_bytes != 0) {
                console_extended = true;
                hr = rt::d3d12::create_console_descriptor_heap(
                    self->real, desc, static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(address), size,
                    __uuidof(ID3D12DescriptorHeap), &created);
                // Only a device from create_vulkan_device* takes the console address; on the host's D3D12 device
                // (LEONIDA_D3D12 not "vulkan") the helper refuses it, and the PC call below makes the heap from the
                // 16-byte PC desc and ignores the address/size tail, as this slot did before.
                if (hr == E_INVALIDARG && !created) console_extended = false;
            }
        }
    }
#endif
    if (!console_extended)
        hr = self->real->CreateDescriptorHeap(desc, __uuidof(ID3D12DescriptorHeap), &created);
    if (FAILED(hr) || !created) return FAILED(hr) ? hr : E_FAIL;

    ID3D12DescriptorHeap* heap = static_cast<ID3D12DescriptorHeap*>(created);
    // Each heap the title makes, with the start it will be told: its descriptor tables are built from these starts
    // (a binding record holding 0x200 sent the title to game+0x4d9f113 with a read of 0x200).
    static std::atomic<unsigned> heaps_logged{0};
    if (desc && heaps_logged.fetch_add(1, std::memory_order_relaxed) < 24) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(desc);
        std::uint64_t tail[2]{};
        if (readable_range(bytes + sizeof(D3D12_DESCRIPTOR_HEAP_DESC), sizeof(tail)))
            std::memcpy(tail, bytes + sizeof(D3D12_DESCRIPTOR_HEAP_DESC), sizeof(tail));
        std::fprintf(stderr,
                     "[rt] d3d12_xs: descriptor heap type %d, %u descriptors, flags 0x%x, console address 0x%llx size "
                     "0x%llx -> CPU start 0x%llx, GPU start 0x%llx\n",
                     static_cast<int>(desc->Type), desc->NumDescriptors, static_cast<unsigned>(desc->Flags),
                     static_cast<unsigned long long>(tail[0]), static_cast<unsigned long long>(tail[1]),
                     static_cast<unsigned long long>(heap->GetCPUDescriptorHandleForHeapStart().ptr),
                     static_cast<unsigned long long>((desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
                                                         ? heap->GetGPUDescriptorHandleForHeapStart().ptr
                                                         : 0));
    }
    hr = create_heap_proxy(heap, self, *riid, out);
    if (FAILED(hr)) heap->Release();
    return hr;
}

// The Xbox slot 62 takes two more words between the description and the IID: (desc, w2, w3, riid, out). Measured
// on the package's XboxBvhBuilder.dll (its constructor passes w2 = w3 = 0 and IID_ID3D12PipelineState in the fourth
// argument), as the prior art had recorded. Their meaning is unknown; non-zero values are logged once.
HRESULT proxy_create_compute_pipeline_state(DeviceProxy* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                            std::uint64_t w2, std::uint64_t w3, const IID* riid, void** out) {
    note_slot(62);
    if (w2 || w3) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: slot 62 extra words 0x%llx 0x%llx (ignored)\n",
                         static_cast<unsigned long long>(w2), static_cast<unsigned long long>(w3));
    }
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    return self->real->CreateComputePipelineState(desc, *riid, out);
}

// The Xbox slot 67 creates a compute pipeline from native GPU code: (desc, riid, out), named
// DeserializeComputePipelineStateX in amd_fidelityfx_upscaler_xbox.dll's messages (call at RVA 0x56d9). The desc
// passes the root signature at +0x00, followed by pointer/size pairs for the shader's FSPX entries (docs/fspx.md):
// kind 0 (dispatch state) at +0x08/+0x10, kind 1 at +0x18/+0x20, kind 6 (the RDNA2 program) at +0x28/+0x30, and
// kind 7 (records) at +0x38/+0x40. Translates the console description to a standard D3D12_COMPUTE_PIPELINE_STATE_DESC
// with CS bytecode pointing at kind 6 and routes it to CreateComputePipelineState on the PC device (slot 11), which
// on the Vulkan device resolves through the translator. The PC slot 67 is CreateProtectedResourceSession1.
HRESULT proxy_create_compute_pipeline_from_native_code(DeviceProxy* self, const void* desc,
                                                       const IID* riid, void** out) {
    note_slot(67);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!desc) return E_INVALIDARG;
    if (!riid) return E_POINTER;

    // FSPX description layout (docs/fspx.md §How the DLL reads it):
    struct XboxNativeComputePipelineStateDesc {
        ID3D12RootSignature* root_signature;  // +0x00
        const void* kind0;                    // +0x08: dispatch state
        std::uint64_t kind0_size;             // +0x10: 28 bytes
        const void* kind1;                    // +0x18: unknown
        std::uint64_t kind1_size;             // +0x20: 20 bytes
        const void* kind6;                    // +0x28: RDNA2 program code
        std::uint64_t kind6_size;             // +0x30: program size in bytes
        const void* kind7;                    // +0x38: resource/hash records
        std::uint64_t kind7_size;             // +0x40: records size in bytes
    };
    const auto* xbox_desc = static_cast<const XboxNativeComputePipelineStateDesc*>(desc);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pc_desc{};
    pc_desc.pRootSignature = xbox_desc->root_signature;
    pc_desc.CS.pShaderBytecode = xbox_desc->kind6;
    pc_desc.CS.BytecodeLength = static_cast<SIZE_T>(xbox_desc->kind6_size);
    // With kind 0 (the DLL always passes it: its reader refuses a payload without one), the four pairs go on as one
    // FSPX payload, so the device matches the program by its kind-6 and kind-0 keys (docs/fspx.md, "Build and run-time
    // lookup"): two programs with the same code and different dispatch state stay apart. Every pair is checked before
    // it is read (leonida::fspx_payload_from_parts); an inconsistent one refuses the call. Without kind 0 the kind-6
    // bytes go on alone, as before.
    std::vector<std::uint8_t> payload;
    if (xbox_desc->kind0 || xbox_desc->kind0_size) {
        const leonida::FspxPart parts[] = {{0, xbox_desc->kind0, xbox_desc->kind0_size},
                                           {1, xbox_desc->kind1, xbox_desc->kind1_size},
                                           {6, xbox_desc->kind6, xbox_desc->kind6_size},
                                           {7, xbox_desc->kind7, xbox_desc->kind7_size}};
        payload = leonida::fspx_payload_from_parts(parts);
        if (payload.empty()) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr,
                             "[rt] d3d12_xs: slot 67 refused: a pointer/size pair is inconsistent or too large "
                             "(kind0 %llu B, kind1 %llu B, kind6 %llu B, kind7 %llu B)\n",
                             static_cast<unsigned long long>(xbox_desc->kind0_size),
                             static_cast<unsigned long long>(xbox_desc->kind1_size),
                             static_cast<unsigned long long>(xbox_desc->kind6_size),
                             static_cast<unsigned long long>(xbox_desc->kind7_size));
            return E_INVALIDARG;
        }
        pc_desc.CS.pShaderBytecode = payload.data();
        pc_desc.CS.BytecodeLength = payload.size();
    }

    pc_desc.NodeMask = 0;
    pc_desc.CachedPSO = {nullptr, 0};
    pc_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    // Every failed call is logged with its number: the DLL creates its pipelines one after the other, and only the
    // first call of a slot is announced, so a miss on a later program would otherwise read as the first call's.
    static std::atomic<std::uint64_t> calls{0};
    const std::uint64_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const HRESULT hr = self->real->CreateComputePipelineState(&pc_desc, *riid, out);
    if (FAILED(hr))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 67 call %llu failed with 0x%08lx (kind 0 %llu B, kind 1 %llu B, kind 6 %llu B, "
                     "kind 7 %llu B)\n",
                     static_cast<unsigned long long>(call), static_cast<unsigned long>(hr),
                     static_cast<unsigned long long>(xbox_desc->kind0_size),
                     static_cast<unsigned long long>(xbox_desc->kind1_size),
                     static_cast<unsigned long long>(xbox_desc->kind6_size),
                     static_cast<unsigned long long>(xbox_desc->kind7_size));
    return hr;
}

// The console lets a depth texture allow unordered access as well (the title asks for a 2560x1440 R32G8X24_TYPELESS
// one, flags 0x6, and asserts at game+0x344196 when its size is refused); the PC runtime refuses the pair. A device
// behind the proxy that takes it (set_depth_unordered_handler: the Vulkan layer gives such a texture colour twins
// that carry its UAVs, as VIctory does, victory-gpu resource.rs twin_texture) gets the description as it is, and the
// call is made inside a DepthUnorderedScope, so the device knows it is the console's. For any other device the UAV
// flag is dropped: the texture is made and its unordered-access views are left null (CreateUnorderedAccessView logs
// them).
std::atomic<DepthUnorderedHandler> g_depth_unordered{nullptr};
// Plain data: no thread-exit callback (AGENTS.md, runtime traps).
thread_local bool t_console_depth_unordered = false;

class DepthUnorderedScope {
public:
    explicit DepthUnorderedScope(bool on) : before_(t_console_depth_unordered) {
        if (on) t_console_depth_unordered = true;
    }
    ~DepthUnorderedScope() { t_console_depth_unordered = before_; }
    DepthUnorderedScope(const DepthUnorderedScope&) = delete;
    DepthUnorderedScope& operator=(const DepthUnorderedScope&) = delete;

private:
    bool before_;
};

// The console layout word of the description the proxy is making a resource for (slots 76, 127 and 133), which the
// PC description it passes on replaces: the device reads the title's bytes of a texture on the title's memory with
// it (rt::d3d12_xs::console_texture_layout). Plain data: no thread-exit callback (AGENTS.md, runtime traps).
thread_local std::uint32_t t_console_texture_layout = 0;

class ConsoleLayoutScope {
public:
    explicit ConsoleLayoutScope(std::uint32_t layout) : before_(t_console_texture_layout) {
        t_console_texture_layout = layout;
    }
    ~ConsoleLayoutScope() { t_console_texture_layout = before_; }
    ConsoleLayoutScope(const ConsoleLayoutScope&) = delete;
    ConsoleLayoutScope& operator=(const ConsoleLayoutScope&) = delete;

private:
    std::uint32_t before_;
};

// Whether the device behind `self` takes depth textures that allow unordered access.
bool takes_depth_unordered(const DeviceProxy* self) {
    const DepthUnorderedHandler handler = g_depth_unordered.load(std::memory_order_acquire);
    return handler && self && self->real && handler(self->real);
}

// True when `desc` asks for both flags and `keep` says the device takes them; else the UAV flag is dropped from a
// description that asks for both, and false.
template <class Desc>
bool depth_unordered_access(Desc& desc, bool keep) {
    const auto flags = static_cast<std::uint32_t>(desc.Flags);
    constexpr std::uint32_t depth_and_uav = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if ((flags & depth_and_uav) != depth_and_uav) return false;
    if (keep) return true;
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(flags & ~std::uint32_t(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: a depth texture that allows unordered access (format %d, %llux%u) is made without "
                     "it; its UAVs stay null\n",
                     static_cast<int>(desc.Format), static_cast<unsigned long long>(desc.Width), desc.Height);
    return false;
}

HRESULT proxy_create_resource(DeviceProxy* self, const D3D12_HEAP_PROPERTIES* heap_properties,
                              D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC* xbox_desc,
                              D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE* clear_value,
                              const IID* riid, void** out) {
    note_slot(76);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!xbox_desc) return E_POINTER;
    if (!riid) return E_POINTER;

    // The measured description is 56 bytes plus Xbox extension words; D3D12_RESOURCE_DESC is exactly 56 bytes.
    // The Xbox layout word is 0x101 (tiled) or 0x1 (row-major buffer), and is not a PC layout value. Committed
    // resources take ROW_MAJOR for buffers and UNKNOWN for everything else, so the word is ignored and replaced.
    D3D12_RESOURCE_DESC pc_desc = *xbox_desc;
    const auto xbox_layout = static_cast<std::uint32_t>(pc_desc.Layout);
    pc_desc.Layout = pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                         : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const DepthUnorderedScope scope(depth_unordered_access(pc_desc, takes_depth_unordered(self)));
    const ConsoleLayoutScope layout_scope(xbox_layout);

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 76 resource: dimension=%d xbox layout=0x%x -> PC layout=0x%x\n",
                     static_cast<int>(pc_desc.Dimension), xbox_layout, static_cast<unsigned>(pc_desc.Layout));
    }
    return self->real->CreateCommittedResource(heap_properties, heap_flags, &pc_desc, initial_state, clear_value,
                                               *riid, out);
}

// Device slot 133 is the console's CreatePlacedResourceX: (va, desc, state, clear, riid, out), measured on the retail
// executable (docs/retail_boot_chain.md, section 3.4). `va` is the start of a physical-page range the title has just
// mapped, `desc` a 56-byte description with Xbox bits above the PC resource flags and an Xbox layout word, and
// `state` is 0xFFFFFFFF, which is no PC state. The resource is a buffer on those pages, made by the handler the
// device behind the proxy registered (set_placed_at_va_handler), with the PC flags kept and the layout made
// ROW_MAJOR, as slot 76 does.
std::atomic<PlacedAtVaHandler> g_placed_at_va{nullptr};
std::atomic<PlacedAtVaPagesHook> g_placed_at_va_pages{nullptr};

HRESULT proxy_create_placed_resource_x(DeviceProxy* self, void* va, const D3D12_RESOURCE_DESC* xbox_desc,
                                       std::uint32_t state, const IID* riid, void** out) {
    note_slot(133);
    if (out) *out = nullptr;
    // Without a handler (a PC device behind the proxy, or none registered) the slot refuses like any slot past the
    // PC vtable, whatever its arguments.
    const PlacedAtVaHandler handler = g_placed_at_va.load(std::memory_order_acquire);
    if (!handler) return E_NOTIMPL;
    if (!out || !xbox_desc || !riid) return E_POINTER;
    // An integer mask: a D3D12_RESOURCE_FLAGS constant past the SDK's last enumerator is not a constant expression
    // for clang-cl with SDK 10.0.26100 (found in review). The cast is applied to the masked value at run time.
    // The PC flags up to RAYTRACING_ACCELERATION_STRUCTURE (0x100) and USE_TIGHT_ALIGNMENT (0x400); 0x200 is
    // dropped, as committed_resource_refusal says the host refuses it (found in review).
    constexpr std::uint32_t kPcResourceFlags = 0x5ff;
    D3D12_RESOURCE_DESC pc_desc = *xbox_desc;
    const auto xbox_flags = static_cast<std::uint32_t>(pc_desc.Flags);
    pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(xbox_flags & kPcResourceFlags);
    // The Xbox layout word (0x101 tiled, 0x1 row-major buffer) is no PC layout: ROW_MAJOR for a buffer and UNKNOWN
    // for everything else, as slot 76 does.
    pc_desc.Layout = pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                         : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // A depth texture with unordered access on the title's pages (a 768x768 R16_TYPELESS one, flags 0x6) keeps both
    // flags where the device gives it colour twins, as slots 76 and 127 do; dropping the UAV flag left its
    // UNORDERED_ACCESS initial state refused, and the title went on without the texture.
    const DepthUnorderedScope scope(depth_unordered_access(pc_desc, takes_depth_unordered(self)));
    const ConsoleLayoutScope layout_scope(static_cast<std::uint32_t>(xbox_desc->Layout));
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 133 CreatePlacedResourceX: va=%p dimension=%d width=%llu xbox flags=0x%x "
                     "state=0x%x -> a buffer on the title's pages\n",
                     va, static_cast<int>(pc_desc.Dimension), static_cast<unsigned long long>(pc_desc.Width),
                     xbox_flags, state);
    }
    // The buffer's pages are committed before the handler walks them (set_placed_at_va_pages_hook).
    const PlacedAtVaPagesHook pages = g_placed_at_va_pages.load(std::memory_order_acquire);
    if (pages && pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && pc_desc.Width &&
        !pages(va, static_cast<std::size_t>(pc_desc.Width))) {
        static std::atomic<bool> refused{false};
        if (!refused.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr, "[rt] d3d12_xs: slot 133: committing the pages of va=%p width=%llu failed (error %lu)\n",
                         va, static_cast<unsigned long long>(pc_desc.Width), GetLastError());
        }
    }
    return handler(self->real, va, pc_desc, state, *riid, out);
}

// ---- Console device methods (d3d12_xs_slots.h) --------------------------------------------------------------------

// A console resource description as the PC takes it: the Xbox layout word (0x101 tiled, 0x1 row-major buffer) becomes
// ROW_MAJOR for a buffer and UNKNOWN for anything else, as slot 76 does, and the Xbox flag bits above the PC's are
// cleared, as slot 133 does (the integer mask is slot 133's kPcResourceFlags). Depth with unordered access is kept
// when `keep` (takes_depth_unordered); the return says it was.
constexpr std::uint32_t kPcResourceFlagMask = 0x5ff;

bool make_pc_desc(D3D12_RESOURCE_DESC& desc, bool keep) {
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<std::uint32_t>(desc.Flags) & kPcResourceFlagMask);
    desc.Layout = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                    : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return depth_unordered_access(desc, keep);
}

// D3D12_RESOURCE_DESC1 starts like the plain description (V resource.rs:2225-2231); its sampler feedback region is
// not used by the title's resources and is cleared.
bool make_pc_desc(D3D12_RESOURCE_DESC1& desc, bool keep) {
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<std::uint32_t>(desc.Flags) & kPcResourceFlagMask);
    desc.Layout = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                    : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.SamplerFeedbackMipRegion = {};
    return depth_unordered_access(desc, keep);
}

// A copy of `count` descriptions with make_pc_desc applied, for the allocation-info calls. Up to 8 inline, more
// allocated (nothing here throws).
template <class Desc>
class PcDescs {
public:
    bool fill(std::uint64_t descs, std::uint64_t count, bool keep_depth_unordered) {
        const auto n = static_cast<std::uint32_t>(count);
        if (!descs || !n) return true;
        Desc* copy = inline_;
        if (n > std::size(inline_)) {
            heap_.reset(new (std::nothrow) Desc[n]);
            if (!heap_) return false;
            copy = heap_.get();
        }
        std::memcpy(copy, reinterpret_cast<const void*>(descs), sizeof(Desc) * n);
        for (std::uint32_t i = 0; i < n; ++i) make_pc_desc(copy[i], keep_depth_unordered);
        data_ = copy;
        return true;
    }
    // The copy, or 0 when there was no array (the caller's argument then goes on unchanged).
    std::uint64_t data() const { return reinterpret_cast<std::uint64_t>(data_); }

private:
    Desc inline_[8];
    std::unique_ptr<Desc[]> heap_;
    Desc* data_ = nullptr;
};

// Device struct returns, slots 25 GetResourceAllocationInfo, 26 GetCustomHeapProperties and 108
// GetResourceAllocationInfo2 (PC slot 68). The title passes the return buffer first and `this` second (V
// resource.rs:802-811, 840-849, 2583-2593; L retail_boot_chain §3.1 measured 26, and the title faults reading the
// buffer as the device without this). The PC method takes (this, buffer, ...): the call is taken as swapped when the
// first register is not a device proxy and the second one is, so a PC-order caller (Leonida's tests) works as before.
// Both registers point at readable memory either way: the proxy, or the caller's buffer of 16 or 20 bytes. The other
// words keep their positions. Returns the buffer, as both conventions do. In the console layout the descriptions of
// 25 and 108 are made PC ones (make_pc_desc); without the PC method (a device older than ID3D12Device8 for 108) the
// buffer gets what D3D12 answers for a description it cannot place, SizeInBytes UINT64_MAX.
template <unsigned Slot>
std::uint64_t device_struct_return(DeviceProxy* first, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                   std::uint64_t a4, std::uint64_t a5) {
    static_assert(Slot == 25 || Slot == 26 || Slot == 108);
    DeviceProxy* self = first;
    std::uint64_t out = a1;
    if (!is_device_proxy(first) && is_device_proxy(reinterpret_cast<const void*>(a1))) {
        self = reinterpret_cast<DeviceProxy*>(a1);
        out = reinterpret_cast<std::uint64_t>(first);
    }
    note_slot(Slot);
    constexpr unsigned pc = Slot == 108 ? 68 : Slot;
    if (pc >= self->slot_count) {
        refuse_beyond_device(Slot, self->slot_count);
        if constexpr (Slot != 26) {
            if (auto* info = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(out)) *info = {~0ull, 0};
        }
        return out;
    }
    using Fn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t);
    const auto fn = reinterpret_cast<Fn>(self->real_vtable[pc]);
    if constexpr (Slot == 26) {
        fn(self->real, out, a2, a3, a4, a5);  // (node_mask, heap_type)
    } else {
        // (visible_mask, count, descs[, each allocation's info for 108])
        PcDescs<std::conditional_t<Slot == 25, D3D12_RESOURCE_DESC, D3D12_RESOURCE_DESC1>> descs;
        if (self->layout == Layout::console && !descs.fill(a4, a3, takes_depth_unordered(self))) {  // the copy could not be allocated
            if (auto* info = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(out)) *info = {~0ull, 0};
            return out;
        }
        fn(self->real, out, a2, a3, descs.data() ? descs.data() : a4, a5);
        // What the title is told, for the first calls: it sizes its own pools by these answers (a run stops at a
        // pool check right after the back buffers, game+0x247af2d: docs/port_status.md).
        static std::atomic<unsigned> logged{0};
        const auto* info = reinterpret_cast<const D3D12_RESOURCE_ALLOCATION_INFO*>(out);
        const auto* first = reinterpret_cast<const D3D12_RESOURCE_DESC*>(a4);
        if (info && first && a3 && (logged.fetch_add(1, std::memory_order_relaxed) < 8 || trace_enabled()))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: device slot %u: %u description(s), the first dimension %d %llux%u, %u mips, "
                         "format %d, layout word 0x%x, flags 0x%x -> %llu bytes, alignment %llu\n",
                         Slot, static_cast<unsigned>(a3), static_cast<int>(first->Dimension),
                         static_cast<unsigned long long>(first->Width), first->Height,
                         static_cast<unsigned>(first->MipLevels), static_cast<int>(first->Format),
                         static_cast<unsigned>(first->Layout), static_cast<unsigned>(first->Flags),
                         static_cast<unsigned long long>(info->SizeInBytes),
                         static_cast<unsigned long long>(info->Alignment));
        // A refusal (SizeInBytes UINT64_MAX) stops the title at its check game+0x344196, so each one is logged with
        // the PC description it was answered for.
        static std::atomic<unsigned> refused{0};
        const auto* pc_first = descs.data() ? reinterpret_cast<const D3D12_RESOURCE_DESC*>(descs.data()) : first;
        if (info && pc_first && a3 && info->SizeInBytes == ~0ull &&
            refused.fetch_add(1, std::memory_order_relaxed) < 32)
            std::fprintf(stderr,
                         "[rt] d3d12_xs: device slot %u refused a description: dimension %d, %llux%ux%u, %u mips, "
                         "format %d, samples %u/%u, layout %d, alignment %llu, flags 0x%x (%u description(s))\n",
                         Slot, static_cast<int>(pc_first->Dimension), static_cast<unsigned long long>(pc_first->Width),
                         pc_first->Height, static_cast<unsigned>(pc_first->DepthOrArraySize),
                         static_cast<unsigned>(pc_first->MipLevels), static_cast<int>(pc_first->Format),
                         pc_first->SampleDesc.Count, pc_first->SampleDesc.Quality, static_cast<int>(pc_first->Layout),
                         static_cast<unsigned long long>(pc_first->Alignment), static_cast<unsigned>(pc_first->Flags),
                         static_cast<unsigned>(a3));
    }
    return out;
}

// A console device slot that no source names, or a console method not written yet (the device slot table): E_NOTIMPL,
// logged once with the first argument words.
HRESULT refuse_console_device(unsigned slot, bool open, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                              std::uint64_t a4) {
    bool expected = false;
    if (slot < kMaxSlots && g_beyond_warned[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: device slot %u (%s) is %s in the console layout; returning E_NOTIMPL (args 0x%llx "
                     "0x%llx 0x%llx 0x%llx)\n",
                     slot, slot_name(slot), open ? "not implemented yet" : "unknown",
                     static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                     static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4));
    return E_NOTIMPL;
}

// Console device slot 57 (V device.rs:349-400): a pipeline creation, (description, riid, out), that nobody has
// written. Refused with the output nulled, as VIctory does: S_OK without a pipeline would hand the title whatever its
// stack held as an object, and on a failure the title goes without that pipeline.
// LEONIDA_CAPTURE_SLOT57=<folder> (diagnostic): the parts of the first 400 slot-57 descriptions, each pair of the
// description's words 1..32 that names a readable blob, saved as <call>-w<word>.bin, so the graphics pipelines the
// title deserializes can be read offline. They are the title's bytes: the folder stays on the player's machine.
void capture_slot57(std::uint64_t desc, unsigned call) {
    static const std::string folder = [] {
        char buf[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableA("LEONIDA_CAPTURE_SLOT57", buf, sizeof buf);
        return n && n < sizeof buf ? std::string(buf) : std::string();
    }();
    if (folder.empty() || (call > 400 && call <= 100000) || call > 100300 || !desc) return;  // 100001..: refused
    std::uint64_t d[33] = {};
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(desc), d, sizeof d, &got) ||
        got != sizeof d)
        return;
    const auto write = [&](const std::string& name, const void* data, std::size_t size) {
        if (FILE* f = std::fopen((folder + "\\" + name).c_str(), "wb")) {
            std::fwrite(data, 1, size, f);
            std::fclose(f);
        }
    };
    write(std::to_string(call) + "-desc.bin", d, sizeof d);
    for (unsigned i = 1; i + 1 < 33; i += 2) {
        if (d[i] < 0x10000 || d[i + 1] == 0 || d[i + 1] > 0x1000000) continue;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(d[i + 1]));
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(d[i]), bytes.data(), bytes.size(),
                               &got) || got != bytes.size())
            continue;
        write(std::to_string(call) + "-w" + std::to_string(i) + ".bin", bytes.data(), bytes.size());
    }
}

// ---- Console device slot 57: a graphics pipeline from native GPU code (the graphics side of slot 67) ------------
//
// The title deserializes its graphics pipelines here, (desc, riid, out), from title+0x2477578 in a worker job, and
// draws everything past the start screen with them. Measured over the first 400 descriptions of a 1.0.2.1 boot
// (docs/fspx.md, "Graphics pipelines"): +0x00 the root signature, then (pointer, size) pairs: +0x08 kind 0 (the
// pipeline's register state, 0x17c-0x184 bytes), +0x18 kind 1 (the VS-PS semantic table, 0x98-0xbc bytes), +0x28
// the pixel stage's program (kind 6, null in a depth-only pipeline: 4 of 400) and +0x38 its records (kind 7), +0x48
// the vertex stage's program and +0x58 its records. The programs are bare kind-6 code with the FSPX prologue; the
// vertex stage fetches its own vertices (buffer loads through s[8:9]) and exports pos0 and params, the pixel stage
// reads them with v_interp.
//
// Kind 0 holds gfx10 context registers at fixed offsets (the census in docs/fspx.md): CB_BLEND0..7_CONTROL at
// +0xd0, DB_DEPTH_CONTROL at +0xb8, DB_STENCIL_CONTROL at +0xbc, DB_STENCILREFMASK(_BF) at +0xc0/+0xc4 and, read as
// PA_SU_SC_MODE_CNTL, +0x84. No attachment format is in it: the hardware takes those from the bound targets, so the
// pipeline is made with rt::d3d12::kPipelineFormatsFromTargets and built per format set at draw time.
namespace console_pso {

// rt::d3d12::kPipelineFormatsFromTargets (d3d12_com.h, which this file includes only with the Vulkan device).
constexpr UINT kFormatsFromTargets = 0x40000000u;
#ifdef LEONIDA_VULKAN_DEVICE
static_assert(kFormatsFromTargets == rt::d3d12::kPipelineFormatsFromTargets);
#endif

D3D12_BLEND blend_factor(std::uint32_t amd) {
    static constexpr D3D12_BLEND map[] = {
        D3D12_BLEND_ZERO,           D3D12_BLEND_ONE,           D3D12_BLEND_SRC_COLOR,      D3D12_BLEND_INV_SRC_COLOR,
        D3D12_BLEND_SRC_ALPHA,      D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA,     D3D12_BLEND_INV_DEST_ALPHA,
        D3D12_BLEND_DEST_COLOR,     D3D12_BLEND_INV_DEST_COLOR, D3D12_BLEND_SRC_ALPHA_SAT, D3D12_BLEND_ONE,
        D3D12_BLEND_ONE,            D3D12_BLEND_BLEND_FACTOR,  D3D12_BLEND_INV_BLEND_FACTOR, D3D12_BLEND_SRC1_COLOR,
        D3D12_BLEND_INV_SRC1_COLOR, D3D12_BLEND_SRC1_ALPHA,    D3D12_BLEND_INV_SRC1_ALPHA, D3D12_BLEND_ALPHA_FACTOR,
        D3D12_BLEND_INV_ALPHA_FACTOR};
    return amd < std::size(map) ? map[amd] : D3D12_BLEND_ONE;
}

D3D12_BLEND_OP blend_op(std::uint32_t amd) {
    switch (amd) {
    case 1: return D3D12_BLEND_OP_SUBTRACT;      // COMB_SRC_MINUS_DST
    case 2: return D3D12_BLEND_OP_MIN;
    case 3: return D3D12_BLEND_OP_MAX;
    case 4: return D3D12_BLEND_OP_REV_SUBTRACT;  // COMB_DST_MINUS_SRC
    default: return D3D12_BLEND_OP_ADD;          // COMB_DST_PLUS_SRC
    }
}

D3D12_STENCIL_OP stencil_op(std::uint32_t amd) {
    switch (amd) {
    case 1: return D3D12_STENCIL_OP_ZERO;
    case 2: case 3: case 4: return D3D12_STENCIL_OP_REPLACE;  // ONES, REPLACE_TEST, REPLACE_OP
    case 5: return D3D12_STENCIL_OP_INCR_SAT;                 // ADD_CLAMP
    case 6: return D3D12_STENCIL_OP_DECR_SAT;                 // SUB_CLAMP
    case 7: return D3D12_STENCIL_OP_INVERT;
    case 8: return D3D12_STENCIL_OP_INCR;                     // ADD_WRAP
    case 9: return D3D12_STENCIL_OP_DECR;                     // SUB_WRAP
    default: return D3D12_STENCIL_OP_KEEP;
    }
}

// AMD's compare functions are D3D12's minus one (NEVER 0 .. ALWAYS 7).
D3D12_COMPARISON_FUNC compare(std::uint32_t amd) { return D3D12_COMPARISON_FUNC((amd & 7) + 1); }

bool enabled(const char* name, bool fallback) {
    char text[8] = {};
    const DWORD n = GetEnvironmentVariableA(name, text, sizeof text);
    if (!n || n >= sizeof text) return fallback;
    return text[0] != '0';
}

}  // namespace console_pso

HRESULT proxy_create_graphics_pipeline_from_native_code(DeviceProxy* self, std::uint64_t desc_word, const IID* riid,
                                                        void** out) {
    note_slot(57);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (!desc_word) return E_INVALIDARG;
    static std::atomic<unsigned> calls{0};
    const unsigned call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    capture_slot57(desc_word, call);
    // LEONIDA_CONSOLE_PIPELINES=0: refused as before (the title goes without them).
    static const bool on = console_pso::enabled("LEONIDA_CONSOLE_PIPELINES", true);
    if (!on) return E_NOTIMPL;
    struct Pair {
        const void* data;
        std::uint64_t size;
    };
    struct Desc {
        ID3D12RootSignature* root_signature;
        Pair state, semantics, ps, ps_records, vs, vs_records, ms, ms_records;
    };
    const auto* d = reinterpret_cast<const Desc*>(desc_word);
    const auto fail = [&](const char* why) {
        static std::atomic<unsigned> failures{0};
        const unsigned n = failures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || (n & (n - 1)) == 0)
            std::fprintf(stderr, "[rt] d3d12_xs: slot 57 pipeline %u refused: %s (%u so far)\n", call, why, n);
        return E_INVALIDARG;
    };
    if (!d->root_signature) return fail("no root signature");
    // A mesh pipeline: no vertex stage, the mesh stage's pair at +0x68/+0x78 (an NGG program in mesh mode: wave64,
    // MSG_GS_ALLOC_REQ, exp prim, group index v5 >> 7 as the game's own mesh shaders). Its kind 0 is the same
    // register block 0x54 bytes further on (measured over 300 mesh descriptions, docs/fspx.md).
    const bool mesh = !(d->vs.data && d->vs.size) && d->ms.data && d->ms.size;
    // LEONIDA_CONSOLE_MESH=0 (diagnostic): mesh pipelines are refused, to tell what they do from the rest.
    static const bool mesh_on = console_pso::enabled("LEONIDA_CONSOLE_MESH", true);
    if (mesh && !mesh_on) return E_NOTIMPL;
    if (!mesh && !(d->vs.data && d->vs.size)) {
        static std::atomic<unsigned> refused{0};
        capture_slot57(desc_word, 100001 + refused.fetch_add(1, std::memory_order_relaxed));
        return fail("no vertex or mesh stage");
    }
    if (!d->state.data || d->state.size < 0xf0 || d->state.size > 0x1000) return fail("kind 0 is not the expected size");
    std::uint32_t w[0x1000 / 4] = {};
    std::memcpy(w, d->state.data, static_cast<std::size_t>(d->state.size) & ~std::size_t(3));
    const std::uint32_t shift = mesh ? 0x54 : 0;
    const auto reg = [&](std::uint32_t offset) { return w[(offset + shift) / 4]; };

    // Each stage goes to the device as one FSPX payload (kinds 0, 1, 6, 7), which its graphics path takes apart:
    // the kind-6 bytes are the key and the code base (d3d12_vulkan_device.cpp, create_graphics).
    const auto payload = [&](const Pair& code, const Pair& records) {
        const leonida::FspxPart parts[] = {{0, d->state.data, d->state.size},
                                           {1, d->semantics.data, d->semantics.size},
                                           {6, code.data, code.size},
                                           {7, records.data, records.size}};
        return leonida::fspx_payload_from_parts(parts);
    };
    const std::vector<std::uint8_t> vs = mesh ? payload(d->ms, d->ms_records) : payload(d->vs, d->vs_records);
    const std::vector<std::uint8_t> ps = d->ps.data && d->ps.size ? payload(d->ps, d->ps_records)
                                                                  : std::vector<std::uint8_t>{};
    if (vs.empty() || (d->ps.data && d->ps.size && ps.empty())) return fail("a stage's parts are inconsistent");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pc{};
    pc.pRootSignature = d->root_signature;
    pc.VS = {vs.data(), vs.size()};
    if (!ps.empty()) pc.PS = {ps.data(), ps.size()};
    pc.BlendState.IndependentBlendEnable = TRUE;
    for (unsigned i = 0; i < 8; ++i) {
        const std::uint32_t b = reg(0xd0 + 4 * i);  // CB_BLENDi_CONTROL
        D3D12_RENDER_TARGET_BLEND_DESC& t = pc.BlendState.RenderTarget[i];
        t.BlendEnable = (b >> 30) & 1;
        t.SrcBlend = console_pso::blend_factor(b & 31);
        t.BlendOp = console_pso::blend_op((b >> 5) & 7);
        t.DestBlend = console_pso::blend_factor((b >> 8) & 31);
        const bool separate = (b >> 29) & 1;
        t.SrcBlendAlpha = separate ? console_pso::blend_factor((b >> 16) & 31) : t.SrcBlend;
        t.BlendOpAlpha = separate ? console_pso::blend_op((b >> 21) & 7) : t.BlendOp;
        t.DestBlendAlpha = separate ? console_pso::blend_factor((b >> 24) & 31) : t.DestBlend;
        t.LogicOp = D3D12_LOGIC_OP_NOOP;
        // CB_TARGET_MASK (+0xcc, 4 bits a target; 0 in 179 of 400 vertex pipelines that still export colour, so
        // read as "all" there), measured against SPI_SHADER_COL_FORMAT at +0xf4.
        const std::uint32_t targets = reg(0xcc);
        t.RenderTargetWriteMask = targets ? static_cast<UINT8>((targets >> (4 * i)) & 15) : D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    pc.SampleMask = UINT_MAX;
    // PA_SU_SC_MODE_CNTL: CULL_FRONT bit 0, CULL_BACK bit 1, FACE bit 2 (1: clockwise is front). Culling is off unless
    // LEONIDA_CONSOLE_CULL=1 until the reading is confirmed on screen: a wrong one would hide the world.
    static const bool cull = console_pso::enabled("LEONIDA_CONSOLE_CULL", false);
    const std::uint32_t mode = reg(0x84);
    pc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pc.RasterizerState.CullMode = !cull ? D3D12_CULL_MODE_NONE
                                  : (mode & 2) ? D3D12_CULL_MODE_BACK
                                  : (mode & 1) ? D3D12_CULL_MODE_FRONT
                                               : D3D12_CULL_MODE_NONE;
    pc.RasterizerState.FrontCounterClockwise = !((mode >> 2) & 1);
    pc.RasterizerState.DepthClipEnable = TRUE;
    // DB_DEPTH_CONTROL: STENCIL_ENABLE 0, Z_ENABLE 1, Z_WRITE_ENABLE 2, ZFUNC 6:4, BACKFACE_ENABLE 7, STENCILFUNC
    // 10:8, STENCILFUNC_BF 22:20. DB_STENCIL_CONTROL: front FAIL/ZPASS/ZFAIL in 4-bit fields from bit 0, the back
    // face's from bit 12. DB_STENCILREFMASK: test mask 15:8, write mask 23:16.
    const std::uint32_t depth = reg(0xb8), stencil = reg(0xbc), refmask = reg(0xc0);
    D3D12_DEPTH_STENCIL_DESC& ds = pc.DepthStencilState;
    ds.DepthEnable = (depth >> 1) & 1;
    ds.DepthWriteMask = ((depth >> 2) & 1) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = console_pso::compare(depth >> 4);
    ds.StencilEnable = depth & 1;
    ds.StencilReadMask = static_cast<UINT8>(refmask >> 8);
    ds.StencilWriteMask = static_cast<UINT8>(refmask >> 16);
    ds.FrontFace = {console_pso::stencil_op(stencil & 15), console_pso::stencil_op((stencil >> 8) & 15),
                    console_pso::stencil_op((stencil >> 4) & 15), console_pso::compare(depth >> 8)};
    if ((depth >> 7) & 1)
        ds.BackFace = {console_pso::stencil_op((stencil >> 12) & 15), console_pso::stencil_op((stencil >> 20) & 15),
                       console_pso::stencil_op((stencil >> 16) & 15), console_pso::compare(depth >> 20)};
    else
        ds.BackFace = ds.FrontFace;
    pc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pc.NumRenderTargets = 8;  // formats from the targets each draw binds
    pc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pc.SampleDesc = {1, 0};
    pc.Flags = static_cast<D3D12_PIPELINE_STATE_FLAGS>(console_pso::kFormatsFromTargets);
    HRESULT hr = E_FAIL;
    if (!mesh) {
        hr = self->real->CreateGraphicsPipelineState(&pc, *riid, out);
    } else {
        // A mesh pipeline goes through the PC stream method (ID3D12Device2::CreatePipelineState, PC slot 47) with an
        // MS subobject, each subobject pointer-aligned as the device's parser reads it.
        constexpr unsigned kPcCreatePipelineState = 47;
        if (kPcCreatePipelineState >= self->slot_count) return refuse_beyond_device(57, self->slot_count);
        std::vector<std::uint8_t> stream;
        const auto add = [&](D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const auto& value) {
            using T = std::remove_cv_t<std::remove_reference_t<decltype(value)>>;
            const std::size_t at = (stream.size() + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
            const std::size_t payload_at = (at + sizeof(type) + alignof(T) - 1) & ~(alignof(T) - 1);
            stream.resize((payload_at + sizeof(T) + sizeof(void*) - 1) & ~(sizeof(void*) - 1), 0);
            std::memcpy(stream.data() + at, &type, sizeof(type));
            std::memcpy(stream.data() + payload_at, &value, sizeof(T));
        };
        D3D12_RT_FORMAT_ARRAY formats{};
        formats.NumRenderTargets = pc.NumRenderTargets;
        add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, pc.pRootSignature);
        add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, pc.VS);
        if (pc.PS.BytecodeLength) add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, pc.PS);
        add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, pc.BlendState);
        add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, pc.SampleMask);
        add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, pc.RasterizerState);
        add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, pc.DepthStencilState);
        add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, pc.PrimitiveTopologyType);
        add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, formats);
        add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, pc.DSVFormat);
        add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, pc.SampleDesc);
        add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, pc.Flags);
        const D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{stream.size(), stream.data()};
        using Fn = HRESULT(WINAPI*)(ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, const IID*, void**);
        hr = reinterpret_cast<Fn>(self->real_vtable[kPcCreatePipelineState])(self->real, &stream_desc, riid, out);
    }
    if (FAILED(hr)) {
        if (out) *out = nullptr;
        static std::atomic<unsigned> failures{0};
        const unsigned n = failures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || (n & (n - 1)) == 0)
            std::fprintf(stderr, "[rt] d3d12_xs: slot 57 pipeline %u: the device refused it (0x%08lx, %u so far)\n",
                         call, static_cast<unsigned long>(hr), n);
    } else {
        static std::atomic<unsigned> made{0};
        const unsigned n = made.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || (n & (n - 1)) == 0)
            std::fprintf(stderr, "[rt] d3d12_xs: slot 57: %u console graphics pipelines made\n", n);
    }
    return hr;
}

HRESULT refuse_unknown_pipeline(unsigned slot, std::uint64_t desc, std::uint64_t riid, std::uint64_t out_word) {
    if (auto** out = reinterpret_cast<void**>(out_word)) *out = nullptr;
    static std::atomic<unsigned> calls{0};
    const unsigned call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call <= 4 || (call & (call - 1)) == 0) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: device slot %u (a console pipeline creation, not implemented) call %u refused "
                     "(description 0x%llx, riid 0x%llx); the title goes without that pipeline\n",
                     slot, call, static_cast<unsigned long long>(desc), static_cast<unsigned long long>(riid));
        // As VIctory logs them (V device.rs:372-392): the description's first 16 words and those its first word
        // points at, read through the kernel since neither is known to be mapped, so the method can be written.
        const auto words = [](std::uint64_t at, std::uint64_t* out, std::size_t n) {
            SIZE_T got = 0;
            return at && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), out, n * 8,
                                           &got) && got == n * 8;
        };
        std::uint64_t d[40] = {};
        if (call <= 8 && words(desc, d, 40)) {
            std::string line;
            char part[24];
            for (const std::uint64_t w : d) {
                std::snprintf(part, sizeof part, " %llx", static_cast<unsigned long long>(w));
                line += part;
            }
            std::fprintf(stderr, "[rt] d3d12_xs: slot %u description:%s\n", slot, line.c_str());
            // The first 4 words of each blob a (pointer, size) pair of the description names.
            for (unsigned i = 1; i + 1 < 40; i += 2) {
                std::uint64_t head[4] = {};
                if (d[i] < 0x10000 || d[i + 1] == 0 || d[i + 1] > 0x1000000 || !words(d[i], head, 4)) continue;
                std::fprintf(stderr, "[rt] d3d12_xs: slot %u   word %u blob 0x%llx size 0x%llx: %016llx %016llx %016llx "
                                     "%016llx\n", slot, i, static_cast<unsigned long long>(d[i]),
                             static_cast<unsigned long long>(d[i + 1]), static_cast<unsigned long long>(head[0]),
                             static_cast<unsigned long long>(head[1]), static_cast<unsigned long long>(head[2]),
                             static_cast<unsigned long long>(head[3]));
            }
        }
    }
    return E_NOTIMPL;
}

// Console device slot 61, CreateSamplerX(desc, handle) (V device.rs:439, descriptor.rs:828-832): the S# the device's
// handler writes from the console description (the Vulkan device), or else the D3D12 sampler VIctory gives the host
// (console_sampler_desc), through the real device's CreateSampler.
void proxy_create_sampler_x(DeviceProxy* self, std::uint64_t desc, std::uint64_t handle) {
    if (!desc || !handle) return;
    const SamplerXHandler handler = g_sampler_x.load(std::memory_order_acquire);
    if (handler && handler(self->real, reinterpret_cast<const void*>(desc), handle)) return;
    const D3D12_SAMPLER_DESC host = console_sampler_desc(reinterpret_cast<const std::uint32_t*>(desc));
    self->real->CreateSampler(&host, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(handle)});
}

// Console device slot 86: CreatePipelineState(stream desc, riid, out), V device.rs:454 and pipeline.rs:671-758. The
// console's stream is the PC's D3D12_PIPELINE_STATE_STREAM_DESC, its subobjects laid out the same way (a type, then
// the description at its alignment, each subobject pointer-aligned), except that the console numbers the amplification
// and mesh shaders 23 and 24 where the PC has 24 and 25 (V pipeline.rs:679-705, whose sizes are taken here). A copy of
// the stream gets the PC numbers and goes to the PC ID3D12Device2::CreatePipelineState (PC slot 47), which on the
// Vulkan device is the layer's stream PSO. A type past 24 is the console's own and of unknown size: the stream is cut
// there, logged once, as VIctory ignores the rest.
std::size_t console_subobject_size(std::uint32_t type) {
    switch (type) {
    case 1: case 2: case 3: case 4: case 5: case 6: case 23: case 24: case 12: case 19: return 24;
    case 0: case 17: return 16;
    case 7: case 15: return 40;
    case 8: return 336;
    case 10: return 48;
    case 11: return 56;
    case 21: return 64;
    case 22: return 32;
    case 9: case 13: case 14: case 16: case 18: case 20: return 8;
    default: return 0;
    }
}

HRESULT proxy_create_pipeline_state_stream(DeviceProxy* self, std::uint64_t desc_word, const IID* riid, void** out) {
    note_slot(86);
    if (out) *out = nullptr;
    const auto* desc = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_DESC*>(desc_word);
    if (!desc || !desc->pPipelineStateSubobjectStream) return E_INVALIDARG;
    if (!riid || !out) return E_POINTER;
    constexpr unsigned kPcCreatePipelineState = 47;  // ID3D12Device2's first method
    if (kPcCreatePipelineState >= self->slot_count) return refuse_beyond_device(86, self->slot_count);
    const auto* in = static_cast<const std::uint8_t*>(desc->pPipelineStateSubobjectStream);
    std::vector<std::uint8_t> stream(in, in + desc->SizeInBytes);
    std::size_t at = 0, cut = stream.size();
    unsigned shaders = 0;
    char types[256] = {};  // the console's subobject types in order (shaders with their lengths), for the log
    std::size_t types_used = 0;
    while (at + 8 <= stream.size()) {
        std::uint32_t type = 0;
        std::memcpy(&type, stream.data() + at, sizeof(type));
        // A shader subobject's bytecode length after the colon (its bytecode pointer is at +8, its length at +16).
        std::uint64_t code_bytes = 0;
        const bool shader = (type >= 1 && type <= 6) || type == 23 || type == 24;
        if (shader && at + 24 <= stream.size()) std::memcpy(&code_bytes, stream.data() + at + 16, sizeof(code_bytes));
        if (types_used + 24 < sizeof(types))
            types_used += shader ? std::snprintf(types + types_used, sizeof(types) - types_used, "%s%u:%llu",
                                                 types_used ? " " : "", type, static_cast<unsigned long long>(code_bytes))
                                 : std::snprintf(types + types_used, sizeof(types) - types_used, "%s%u",
                                                 types_used ? " " : "", type);
        const std::size_t size = console_subobject_size(type);
        if (!size) {
            static std::atomic<bool> logged{false};
            if (first_call(logged))
                std::fprintf(stderr, "[rt] d3d12_xs: slot 86: pipeline subobject type %u at byte %zu has no known size; "
                                     "the stream is cut there (V pipeline.rs:729-735)\n", type, at);
            cut = at;
            break;
        }
        if (type == 23 || type == 24) {
            const std::uint32_t pc = type + 1;  // D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS / _MS
            std::memcpy(stream.data() + at, &pc, sizeof(pc));
        }
        if (shader) ++shaders;
        at += size;
    }
    const D3D12_PIPELINE_STATE_STREAM_DESC pc_desc{cut, stream.data()};
    using Fn = HRESULT(WINAPI*)(ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, const IID*, void**);
    const HRESULT hr = reinterpret_cast<Fn>(self->real_vtable[kPcCreatePipelineState])(self->real, &pc_desc, riid, out);
    // A stream the layer refuses still gets a pipeline object, one that is never made (its draws are dropped), as
    // VIctory hands one out for every stream: the title's own streams include some with no shader at all, and it writes
    // through what it is given (a null pipeline faulted at once).
    HRESULT answer = hr;
    if (const RefusedPipelineHandler refused = g_refused_pipeline.load(std::memory_order_acquire);
        refused && FAILED(hr) && hr != E_OUTOFMEMORY && SUCCEEDED(refused(self->real, *riid, out)))
        answer = S_OK;
    // The first four calls, and refusals: the first 16, then every power of two.
    static std::atomic<unsigned> calls{0}, failures{0};
    const unsigned call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const unsigned failure = FAILED(hr) ? failures.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    if (call <= 4 || trace_enabled() || (failure && (failure <= 16 || !(failure & (failure - 1)))))
        std::fprintf(stderr, "[rt] d3d12_xs: slot 86 CreatePipelineState call %u: %llu bytes, %u shader subobjects "
                             "(types %s) -> 0x%08lx%s, %u refused so far\n",
                     call, static_cast<unsigned long long>(desc->SizeInBytes), shaders, types,
                     static_cast<unsigned long>(hr), answer != hr ? " (handed a pipeline that is never made)" : "",
                     failures.load(std::memory_order_relaxed));
    return answer;
}

// Console device slots 74 and 75 (V resource.rs:2187-2223): page pools, 64 KiB pages the title maps into its tiled
// resources by page index (through queue slot 31, CopyPageMappingsBatchX, which takes the pool by address). Kept per
// device: the first address and the page count; a handle is an index plus one, and a removed pool keeps its index.
HRESULT register_page_pool(DeviceProxy* self, std::uint64_t address, std::uint64_t pages_word, std::uint64_t out_word) {
    const auto pages = static_cast<std::uint32_t>(pages_word);
    auto* const out = reinterpret_cast<std::uint64_t*>(out_word);  // a usize in V
    if (!out || !pages || address % (64 * 1024) != 0) return E_INVALIDARG;
    HRESULT hr = S_OK;
    std::uint64_t handle = 0;
    AcquireSRWLockExclusive(&self->pool_lock);
    try {
        self->page_pools.push_back(PagePool{address, pages, true});
        handle = self->page_pools.size();
    } catch (const std::bad_alloc&) {
        hr = E_OUTOFMEMORY;
    }
    ReleaseSRWLockExclusive(&self->pool_lock);
    if (SUCCEEDED(hr)) *out = handle;
    static std::atomic<unsigned> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 4 || trace_enabled())
        std::fprintf(stderr,
                     "[rt] d3d12_xs: device slot 74 RegisterPagePoolX(0x%llx, %u pages) -> handle %llu (0x%08lx)\n",
                     static_cast<unsigned long long>(address), pages, static_cast<unsigned long long>(handle),
                     static_cast<unsigned long>(hr));
    return hr;
}

HRESULT unregister_page_pool(DeviceProxy* self, std::uint64_t handle) {
    HRESULT hr = E_INVALIDARG;
    AcquireSRWLockExclusive(&self->pool_lock);
    if (handle >= 1 && handle <= self->page_pools.size() && self->page_pools[handle - 1].live) {
        self->page_pools[handle - 1].live = false;
        hr = S_OK;
    }
    ReleaseSRWLockExclusive(&self->pool_lock);
    return hr;
}

// Device slot 127: the console's CreateCommittedResource3, from the title's calls for its back buffers (V
// resource.rs:2163-2185, with the heap flag ALLOW_DISPLAY): (heap, heap flags, desc1, initial layout, clear, protected
// session, castable format count, castable formats, riid, out), the arguments of PC ID3D12Device10's
// CreateCommittedResource3 (PC slot 76). The description becomes a PC one (make_pc_desc); ALLOW_DISPLAY is cleared,
// since the presentation here copies from the resource and the PC runtime keeps that flag for its own swapchains; the
// protected session is not passed on (none is modelled). Without ID3D12Device10 the resource is made with
// CreateCommittedResource in the common state, as VIctory makes it (layouts are not tracked there). On the Vulkan
// device either call is the title's own, so the memory comes from the title's allocator when it exports one
// (rt::d3d12::set_title_allocator): the title counts its back buffers against its render-target pool that way.
HRESULT proxy_create_committed_resource3(DeviceProxy* self, std::uint64_t heap, std::uint64_t heap_flags,
                                         std::uint64_t desc1, std::uint64_t layout, std::uint64_t clear,
                                         std::uint64_t protected_session, std::uint64_t castable_count,
                                         std::uint64_t castable, std::uint64_t riid_word, std::uint64_t out_word) {
    note_slot(127);
    auto** const out = reinterpret_cast<void**>(out_word);
    const auto* const riid = reinterpret_cast<const IID*>(riid_word);
    if (out) *out = nullptr;
    if (!heap || !desc1) return E_INVALIDARG;  // V resource.rs:2133-2136
    if (!riid) return E_POINTER;
    D3D12_RESOURCE_DESC1 pc_desc{};
    std::memcpy(&pc_desc, reinterpret_cast<const void*>(desc1), sizeof(D3D12_RESOURCE_DESC));
    const auto xbox_flags = static_cast<std::uint32_t>(pc_desc.Flags);
    const auto xbox_layout = static_cast<std::uint32_t>(pc_desc.Layout);
    const DepthUnorderedScope scope(make_pc_desc(pc_desc, takes_depth_unordered(self)));
    const ConsoleLayoutScope layout_scope(xbox_layout);
    const auto xbox_heap_flags = static_cast<std::uint32_t>(heap_flags);
    const auto pc_heap_flags =
        static_cast<D3D12_HEAP_FLAGS>(xbox_heap_flags & ~static_cast<std::uint32_t>(D3D12_HEAP_FLAG_ALLOW_DISPLAY));
    const bool has3 = self->slot_count > 76;
    static std::atomic<unsigned> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 4 || trace_enabled())
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 127 CreateCommittedResource3: dimension %d %llux%u, heap flags 0x%x -> 0x%x, "
                     "flags 0x%x -> 0x%x, layout word 0x%x -> 0x%x, barrier layout 0x%x%s -> PC %s\n",
                     static_cast<int>(pc_desc.Dimension), static_cast<unsigned long long>(pc_desc.Width),
                     pc_desc.Height, xbox_heap_flags, static_cast<unsigned>(pc_heap_flags), xbox_flags,
                     static_cast<unsigned>(pc_desc.Flags), xbox_layout, static_cast<unsigned>(pc_desc.Layout),
                     static_cast<unsigned>(layout), protected_session ? ", a protected session (not passed on)" : "",
                     has3 ? "CreateCommittedResource3" : "CreateCommittedResource (COMMON)");
    const auto* const heap_properties = reinterpret_cast<const D3D12_HEAP_PROPERTIES*>(heap);
    const auto* const clear_value = reinterpret_cast<const D3D12_CLEAR_VALUE*>(clear);
    if (has3) {
        using Fn = HRESULT(WINAPI*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                    const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*,
                                    ID3D12ProtectedResourceSession*, UINT32, const DXGI_FORMAT*, const IID*, void**);
        return reinterpret_cast<Fn>(self->real_vtable[76])(
            self->real, heap_properties, pc_heap_flags, &pc_desc,
            static_cast<D3D12_BARRIER_LAYOUT>(static_cast<std::uint32_t>(layout)), clear_value, nullptr,
            static_cast<UINT32>(castable_count), reinterpret_cast<const DXGI_FORMAT*>(castable), riid, out);
    }
    D3D12_RESOURCE_DESC plain{};
    std::memcpy(&plain, &pc_desc, sizeof(plain));
    return self->real->CreateCommittedResource(heap_properties, pc_heap_flags, &plain, D3D12_RESOURCE_STATE_COMMON,
                                               clear_value, *riid, out);
}

// The Xbox routes (48, 73 and 78-81 unless moved, see XboxDeviceSlotRoutes): true, with `result` set, when `slot` is
// one of them. Checked before the PC vtable is indexed: the routes would otherwise reach unrelated PC methods (78/79
// are CreateReservedResource2/CreateSampler2 on an ID3D12Device11, 80 and 81 are past its table).
// Device slot 13 in the console layout: CheckFeatureSupport(feature, data, size), ported from VIctory (victory-gpu
// device.rs:240-347, GPL-3.0-or-later).
//   - Feature 0 (OPTIONS): the console's structure has 16 words, one more than the PC's 15: a console-only field at
//     index 11, after StandardSwizzle64KBSupported (V device.rs:254-273). A 64-byte query gets the device's PC answer
//     with that word 0 inserted; the PC runtime refuses the size outright (E_INVALIDARG), which left the title
//     without its options. A 60-byte query is the PC's and goes on unchanged.
//   - Feature 2 (FEATURE_LEVELS): the console hardware is feature level 12_2 (V device.rs:97-98, 320-347): the
//     highest requested level up to 12_2, or DXGI_ERROR_UNSUPPORTED (with 0 written) when none is. The PC device
//     behind the proxy caps at 12_1; the console title was built for 12_2 hardware and asks the device it runs on.
//   - Everything else is the PC method's, as before.
constexpr std::uint32_t kFeatureOptions = 0, kFeatureLevels = 2;
constexpr std::uint32_t kConsoleOptionsWords = 16, kConsoleOnlyOptionsWord = 11;
constexpr std::uint32_t kConsoleHighestFeatureLevel = 0xc200;  // D3D_FEATURE_LEVEL_12_2
static_assert(sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS) == (kConsoleOptionsWords - 1) * 4,
              "the PC options are the console's without its word 11");
static_assert(offsetof(D3D12_FEATURE_DATA_D3D12_OPTIONS, CrossNodeSharingTier) == kConsoleOnlyOptionsWord * 4,
              "the console-only word sits where the PC's CrossNodeSharingTier is");

HRESULT console_check_feature_support(DeviceProxy* self, std::uint64_t feature_word, std::uint64_t data_word,
                                      std::uint64_t size_word) {
    const auto feature = static_cast<std::uint32_t>(feature_word);
    const auto size = static_cast<std::uint32_t>(size_word);
    void* const data = reinterpret_cast<void*>(data_word);
    if (feature == kFeatureOptions && size == kConsoleOptionsWords * 4) {
        if (!data) return E_INVALIDARG;
        D3D12_FEATURE_DATA_D3D12_OPTIONS pc{};
        const HRESULT hr = self->real->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &pc, sizeof(pc));
        if (FAILED(hr)) return hr;
        std::uint32_t words[kConsoleOptionsWords] = {};
        std::memcpy(words, &pc, kConsoleOnlyOptionsWord * 4);
        std::memcpy(words + kConsoleOnlyOptionsWord + 1,
                    reinterpret_cast<const std::uint8_t*>(&pc) + kConsoleOnlyOptionsWord * 4,
                    sizeof(pc) - kConsoleOnlyOptionsWord * 4);
        std::memcpy(data, words, sizeof(words));
        static std::atomic<bool> logged{false};
        if (first_call(logged))
            std::fprintf(stderr, "[rt] d3d12_xs: CheckFeatureSupport(OPTIONS) in the console's 16-word layout\n");
        return hr;
    }
    if (feature == kFeatureLevels && size == sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS)) {
        auto* levels = static_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS*>(data);
        if (!levels || !levels->pFeatureLevelsRequested) return E_INVALIDARG;
        std::uint32_t best = 0;
        for (UINT i = 0; i < levels->NumFeatureLevels; ++i) {
            const auto level = static_cast<std::uint32_t>(levels->pFeatureLevelsRequested[i]);
            if (level <= kConsoleHighestFeatureLevel && level > best) best = level;
        }
        levels->MaxSupportedFeatureLevel = static_cast<D3D_FEATURE_LEVEL>(best);
        return best ? S_OK : DXGI_ERROR_UNSUPPORTED;
    }
    return self->real->CheckFeatureSupport(static_cast<D3D12_FEATURE>(feature), data, size);
}

bool routed_call(DeviceProxy* self, unsigned slot, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                 std::uint64_t a4, std::uint64_t a5, std::uint64_t& result) {
    const XboxDeviceSlotRoutes& routes = xbox_device_slot_routes();
    if (slot == routes.frame_interval) {
        log_xbox_route_once(slot, "SetFrameIntervalX", a1, a2, a3, a4, a5);
        result = hr_to_u64(frame_set_interval(self, a2));  // (output, length_us, period, flags)
    } else if (slot == routes.schedule_event) {
        log_xbox_route_once(slot, "ScheduleFrameEventX", a1, a2, a3, a4, a5);
        result = hr_to_u64(S_OK);  // frame.rs:75-84: every event is at the tick
    } else if (slot == routes.wait_event) {
        log_xbox_route_once(slot, "WaitFrameEventX", a1, a2, a3, a4, a5);
        result = hr_to_u64(frame_wait(self, a1, a2, a4, a5));  // (kind, timeout_ms, list, flags, &token)
    } else if (slot == routes.frame_statistics) {
        log_xbox_route_once(slot, "GetFrameStatisticsX", a1, a2, a3, a4, a5);
        result = hr_to_u64(frame_statistics(a3));  // (token, kinds, &count, out)
    } else if (slot == routes.queue_x) {
        log_xbox_route_once(slot, "CreateCommandQueueX", a1, a2, a3, a4, a5);
        result = hr_to_u64(proxy_create_command_queue_x(self, a1, reinterpret_cast<const IID*>(a2),
                                                        reinterpret_cast<void**>(a3)));
    } else if (slot == routes.list_x) {
        log_xbox_route_once(slot, "CreateCommandListX", a1, a2, a3, a4, a5);
        result = hr_to_u64(proxy_create_command_list_x(self, a1, a2, a3, reinterpret_cast<const IID*>(a4),
                                                       reinterpret_cast<void**>(a5)));
    } else {
        return false;
    }
    return true;
}

bool is_xbox_route(unsigned slot) {
    const XboxDeviceSlotRoutes& routes = xbox_device_slot_routes();
    return slot == routes.frame_interval || slot == routes.schedule_event || slot == routes.wait_event ||
           slot == routes.frame_statistics || slot == routes.queue_x || slot == routes.list_x;
}

// One device method of the slot table, with the thunk's words in the positions it received them.
template <slots::DeviceMethod M, unsigned Slot>
__forceinline std::uint64_t device_call(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                        std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                                        std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                                        std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    using slots::DeviceMethod;
    if constexpr (M == DeviceMethod::query_interface) {
        return hr_to_u64(proxy_query_interface(self, reinterpret_cast<const IID*>(a1),
                                               reinterpret_cast<void**>(a2)));
    } else if constexpr (M == DeviceMethod::add_ref) {
        return proxy_add_ref(self);
    } else if constexpr (M == DeviceMethod::release) {
        return proxy_release(self);
    } else if constexpr (M == DeviceMethod::create_queue) {
        return hr_to_u64(proxy_create_command_queue(self, reinterpret_cast<const D3D12_COMMAND_QUEUE_DESC*>(a1),
                                                    reinterpret_cast<const IID*>(a2), reinterpret_cast<void**>(a3)));
    } else if constexpr (M == DeviceMethod::create_allocator) {
        return hr_to_u64(proxy_create_command_allocator(self, a1, reinterpret_cast<const IID*>(a2),
                                                        reinterpret_cast<void**>(a3)));
    } else if constexpr (M == DeviceMethod::create_list) {
        return hr_to_u64(proxy_create_command_list(self, a1, a2, a3, a4, reinterpret_cast<const IID*>(a5),
                                                   reinterpret_cast<void**>(a6)));
    } else if constexpr (M == DeviceMethod::create_heap) {
        return hr_to_u64(proxy_create_descriptor_heap(self, reinterpret_cast<const D3D12_DESCRIPTOR_HEAP_DESC*>(a1),
                                                      reinterpret_cast<const IID*>(a2),
                                                      reinterpret_cast<void**>(a3)));
    } else if constexpr (M == DeviceMethod::unwrap) {
        return hr_to_u64(proxy_unwrapping_call<Slot>(self, a1, a2, a3, a4, a5));
    } else if constexpr (M == DeviceMethod::open_shared_handle) {
        return hr_to_u64(proxy_open_shared_handle(self, a1, reinterpret_cast<const IID*>(a2),
                                                  reinterpret_cast<void**>(a3)));
    } else if constexpr (M == DeviceMethod::create_fence) {
        return hr_to_u64(proxy_create_fence(self, a1, a2, reinterpret_cast<const IID*>(a3),
                                            reinterpret_cast<void**>(a4)));
    } else if constexpr (M == DeviceMethod::create_queue1) {
        return hr_to_u64(proxy_create_command_queue1(self, a1, a2, reinterpret_cast<const IID*>(a3),
                                                     reinterpret_cast<void**>(a4)));
    } else if constexpr (M == DeviceMethod::unknown_pipeline) {
        if constexpr (Slot == 57)
            return hr_to_u64(proxy_create_graphics_pipeline_from_native_code(
                self, a1, reinterpret_cast<const IID*>(a2), reinterpret_cast<void**>(a3)));
        note_slot(Slot);
        return hr_to_u64(refuse_unknown_pipeline(Slot, a1, a2, a3));
    } else if constexpr (M == DeviceMethod::compute_pso_x) {
        return hr_to_u64(proxy_create_compute_pipeline_state(
            self, reinterpret_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(a1), a2, a3,
            reinterpret_cast<const IID*>(a4), reinterpret_cast<void**>(a5)));
    } else if constexpr (M == DeviceMethod::stream_pso) {
        return hr_to_u64(proxy_create_pipeline_state_stream(self, a1, reinterpret_cast<const IID*>(a2),
                                                            reinterpret_cast<void**>(a3)));
    } else if constexpr (M == DeviceMethod::native_compute_pso) {
        return hr_to_u64(proxy_create_compute_pipeline_from_native_code(
            self, reinterpret_cast<const void*>(a1), reinterpret_cast<const IID*>(a2), reinterpret_cast<void**>(a3)));
    } else if constexpr (M == DeviceMethod::committed_resource_x) {
        return hr_to_u64(proxy_create_resource(
            self, reinterpret_cast<const D3D12_HEAP_PROPERTIES*>(a1),
            static_cast<D3D12_HEAP_FLAGS>(static_cast<std::uint32_t>(a2)),
            reinterpret_cast<const D3D12_RESOURCE_DESC*>(a3),
            static_cast<D3D12_RESOURCE_STATES>(static_cast<std::uint32_t>(a4)),
            reinterpret_cast<const D3D12_CLEAR_VALUE*>(a5), reinterpret_cast<const IID*>(a6),
            reinterpret_cast<void**>(a7)));
    } else if constexpr (M == DeviceMethod::register_page_pool) {
        note_slot(Slot);
        return hr_to_u64(register_page_pool(self, a1, a2, a3));  // (address, pages, &handle)
    } else if constexpr (M == DeviceMethod::unregister_page_pool) {
        note_slot(Slot);
        return hr_to_u64(unregister_page_pool(self, a1));  // (handle)
    } else if constexpr (M == DeviceMethod::placed_view) {
        // V device.rs:443-444, descriptor.rs:795-818: (resource desc, view desc, handle); the view is the device's.
        note_slot(Slot);
        const PlacedViewHandler handler = g_placed_view.load(std::memory_order_acquire);
        if (!handler) return hr_to_u64(refuse_console_device(Slot, true, a1, a2, a3, a4));
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 4 || trace_enabled()) {
            const auto* r = reinterpret_cast<const std::uint32_t*>(a1);
            const auto* v = reinterpret_cast<const std::uint32_t*>(a2);
            if (r && v)
                std::fprintf(stderr,
                             "[rt] d3d12_xs: device slot %u %s view: dimension %u %ux%u, depth/mips 0x%x, format %u, "
                             "layout 0x%x, flags 0x%x; view format %u dimension %u at 0x%llx -> handle 0x%llx\n",
                             Slot, Slot == 70 ? "shader-resource" : "unordered-access", r[0], r[4], r[6], r[7], r[8],
                             r[11], r[12], v[0], v[1],
                             static_cast<unsigned long long>(v[10] | std::uint64_t(v[11]) << 32),
                             static_cast<unsigned long long>(a3));
        }
        if (a1 && a2 && a3) handler(self->real, Slot == 71, reinterpret_cast<const void*>(a1),
                                    reinterpret_cast<const void*>(a2), a3);
        return 0;
    } else if constexpr (M == DeviceMethod::sampler_x) {
        note_slot(Slot);
        proxy_create_sampler_x(self, a1, a2);  // (desc, handle), no result
        return 0;
    } else if constexpr (M == DeviceMethod::feature_support) {
        note_slot(Slot);
        return hr_to_u64(console_check_feature_support(self, a1, a2, a3));  // (feature, data, size)
    } else if constexpr (M == DeviceMethod::committed_resource3) {
        return hr_to_u64(proxy_create_committed_resource3(self, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10));
    } else if constexpr (M == DeviceMethod::placed_resource_x) {
        return hr_to_u64(proxy_create_placed_resource_x(self, reinterpret_cast<void*>(a1),
                                                        reinterpret_cast<const D3D12_RESOURCE_DESC*>(a2),
                                                        static_cast<std::uint32_t>(a3), reinterpret_cast<const IID*>(a5),
                                                        reinterpret_cast<void**>(a6)));
    } else {
        static_assert(M == DeviceMethod::forward || M == DeviceMethod::refuse || M == DeviceMethod::open ||
                          M == DeviceMethod::ignore || M == DeviceMethod::routed,
                      "every other method is handled above (struct_return in proxy_slot)");
        note_slot(Slot);
        std::uint64_t result = 0;
        if (routed_call(self, Slot, a1, a2, a3, a4, a5, result)) return result;
        if constexpr (M == DeviceMethod::ignore) {
            // V device.rs:442 (object::ignore): a debug setting, accepted.
            log_xbox_route_once(Slot, "an ignored console method", a1, a2, a3, a4, a5);
            return hr_to_u64(S_OK);
        } else if constexpr (M == DeviceMethod::refuse || M == DeviceMethod::open) {
            return hr_to_u64(refuse_console_device(Slot, M == DeviceMethod::open, a1, a2, a3, a4));
        } else {
            if constexpr (M == DeviceMethod::routed) {
                // The route was moved off this slot (LEONIDA_XS_*_SLOT): the console layout refuses it, the PC layout
                // forwards it in PC order, as before.
                if (self->layout == Layout::console)
                    return hr_to_u64(refuse_console_device(Slot, false, a1, a2, a3, a4));
            }
            if (Slot >= self->slot_count) return hr_to_u64(refuse_beyond_device(Slot, self->slot_count));
            // The PC vtable entry, called with the PC device as `this`. Integer/pointer arguments, the hidden
            // struct-return pointer and any stack arguments keep their positions; XMM registers are untouched.
            using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t);
            auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
            return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
        }
    }
}

// The device thunk: the slot table's method for this slot, the console or the PC one by the proxy's layout.
template <unsigned Slot>
std::uint64_t WINAPI proxy_slot(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                                std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                                std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    using slots::DeviceMethod;
    constexpr slots::DeviceSlot entry = slots::device_slot(Slot);
    if constexpr (entry.pc == DeviceMethod::struct_return) {
        // `self` may be the caller's return buffer: resolved before anything of the proxy is read.
        static_assert(entry.console == DeviceMethod::struct_return);
        return device_struct_return<Slot>(self, a1, a2, a3, a4, a5);
    } else if constexpr (entry.pc == entry.console) {
        return device_call<entry.pc, Slot>(self, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    } else {
        if (self->layout == Layout::console)
            return device_call<entry.console, Slot>(self, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
        return device_call<entry.pc, Slot>(self, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }
}

template <std::size_t... I>
void fill_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&proxy_slot<static_cast<unsigned>(I)>) ), ...);
}

void** get_vtable() {
    static void** vtable = [] {
        static void* storage[kMaxSlots];
        fill_vtable(storage, std::make_index_sequence<kMaxSlots>{});
        return storage;
    }();
    return vtable;
}

bool supports(ID3D12Device* real, REFIID iid) {
    void* found = nullptr;
    const HRESULT hr = real->QueryInterface(iid, &found);
    if (FAILED(hr) || !found) return false;
    static_cast<IUnknown*>(found)->Release();
    return true;
}

// Highest device interface the PC runtime exposes; its cumulative slot count (from d3d12.h) bounds forwarding.
// Windows SDK 10.0.26100 defines ID3D12Device through ID3D12Device14 only; there is no Device15/Device16 header.
unsigned detect_slot_count(ID3D12Device* real) {
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device14))) return 83;
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device13))) return 82;
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device12))) return 81;
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device11))) return 80;
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device10))) return 79;
#endif
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device9))) return 76;
#endif
    if (supports(real, __uuidof(ID3D12Device8))) return 73;
    if (supports(real, __uuidof(ID3D12Device7))) return 68;
    if (supports(real, __uuidof(ID3D12Device6))) return 66;
    if (supports(real, __uuidof(ID3D12Device5))) return 65;
    if (supports(real, __uuidof(ID3D12Device4))) return 57;
    if (supports(real, __uuidof(ID3D12Device3))) return 51;
    if (supports(real, __uuidof(ID3D12Device2))) return 48;
    if (supports(real, __uuidof(ID3D12Device1))) return 47;
    return 44;
}

}  // namespace

HRESULT create_device_proxy(ID3D12Device* real, REFIID riid, void** out) {
    if (!real) return E_POINTER;
    if (!out) return E_POINTER;
    *out = nullptr;

    DeviceProxy* proxy = new (std::nothrow) DeviceProxy;
    if (!proxy) return E_OUTOFMEMORY;
    proxy->vtable = get_vtable();
    proxy->real = real;
    proxy->null_constant_buffer = 0;
    std::memset(proxy->out_of_line, 0, sizeof(proxy->out_of_line));
    proxy->out_of_line[0] = 1;  // V device.rs:118: every call through the table
    proxy->real_vtable = *reinterpret_cast<void***>(real);
    proxy->slot_count = detect_slot_count(real);
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy->dxgi_vtable = get_dxgi_vtable();
    proxy->adapter_provider = nullptr;
    proxy->gpu_thread_priority.store(0, std::memory_order_relaxed);
    proxy->max_frame_latency.store(3, std::memory_order_relaxed);
    // V device.rs:153-155: created as ID3D12Device11, the device's lists and queues have the newer console layouts.
    proxy->layout = IsEqualIID(riid, kDevice11Iid) ? Layout::console : Layout::pc;
    InitializeSRWLock(&proxy->frame_lock);
    proxy->frame_clock_set = false;
    proxy->frame_start = 0;
    proxy->frame_interval_ns = kSixtiethNs;
    proxy->frame_first = 0;
    for (auto& last : proxy->frame_last) last.store(0, std::memory_order_relaxed);
    proxy->frames_begun.store(0, std::memory_order_relaxed);
    InitializeSRWLock(&proxy->pool_lock);
    proxy->present_handler.store(nullptr, std::memory_order_relaxed);
    proxy->present_context.store(nullptr, std::memory_order_relaxed);

    const unsigned slot_count = proxy->slot_count;
    const Layout layout = proxy->layout;
    HRESULT hr = proxy_query_interface(proxy, &riid, out);
    if (FAILED(hr)) {
        // No reference escaped, and the caller still owns its reference to `real`.
        delete proxy;
        return hr;
    }
    if (!d3d12_trace::enabled())
        std::fprintf(stderr, "[rt] d3d12_xs: device proxy %p wraps PC device %p (%u PC vtable slots, %s layout)\n",
                     static_cast<void*>(proxy), static_cast<void*>(real), slot_count,
                     layout == Layout::console ? "console (created as ID3D12Device11)" : "PC");
    proxy_release(proxy);  // drop the creation reference; `out` holds the caller's reference
    return S_OK;
}

bool is_device_proxy(const void* device) {
    return device && *reinterpret_cast<void* const*>(device) == reinterpret_cast<void*>(get_vtable());
}

void* device_proxy_from_dxgi_device(void* dxgi_device) {
    if (!dxgi_device || *static_cast<void***>(dxgi_device) != get_dxgi_vtable()) return nullptr;
    return dxgi_owner(dxgi_device);
}

void* device_proxy_primary(void* device) {
    if (is_device_proxy(device)) return device;
    return device_proxy_from_dxgi_device(device);
}

bool set_device_adapter_provider(void* device, DxgiAdapterProvider provider) {
    auto* proxy = static_cast<DeviceProxy*>(device_proxy_primary(device));
    if (!proxy) {
        // Not installing one silently would leave the host's adapter in its place.
        std::fprintf(stderr, "[rt] d3d12_xs: set_device_adapter_provider: %p is not a device proxy; no provider set\n",
                     device);
        return false;
    }
    proxy->adapter_provider = provider;
    return true;
}

bool is_heap_proxy(const void* heap) {
    return heap && *reinterpret_cast<void* const*>(heap) == reinterpret_cast<void*>(get_heap_vtable());
}

ID3D12DescriptorHeap* heap_proxy_real_heap(const void* heap) {
    if (!is_heap_proxy(heap)) return nullptr;
    return static_cast<const HeapProxy*>(heap)->real;
}

unsigned device_proxy_slot_count(const void* device) {
    if (!is_device_proxy(device)) return 0;
    return static_cast<const DeviceProxy*>(device)->slot_count;
}

void set_placed_at_va_handler(PlacedAtVaHandler handler) {
    g_placed_at_va.store(handler, std::memory_order_release);
}

void set_depth_unordered_handler(DepthUnorderedHandler handler) {
    g_depth_unordered.store(handler, std::memory_order_release);
}

bool console_depth_unordered() { return t_console_depth_unordered; }
std::uint32_t console_texture_layout() { return t_console_texture_layout; }

void set_sampler_x_handler(SamplerXHandler handler) {
    g_sampler_x.store(handler, std::memory_order_release);
}

// Ported from VIctory (GPL-3.0-or-later, Marvisak and contributors, DexNom fork), victory-gpu sampler.rs:28-96:
// words 1-3 the magnification, minification and mip filters, 5 the filter mode, 6-8 the address modes (D3D12's), 9-11
// the minimum and maximum LOD and the bias, 13 the anisotropy, 16 the comparison (D3D12's), 17-20 the border colour.
D3D12_SAMPLER_DESC console_sampler_desc(const std::uint32_t* w) {
    D3D12_SAMPLER_DESC desc{};
    if (!w) return desc;
    const auto real = [&](int i) {
        float value = 0;
        std::memcpy(&value, &w[i], sizeof(value));
        return value;
    };
    const std::uint32_t mag = w[1], min = w[2], mip = w[3], mode = w[5], compare = w[16];
    const auto linear = [](std::uint32_t f) { return std::uint32_t(f == 1 || f == 3); };
    const bool aniso = mag >= 2 || min >= 2;
    // The console compares when the instruction says so, with the sampler's function; the host needs a comparison
    // sampler for that. A real test (LESS .. GREATER_EQUAL) makes one; NEVER, ALWAYS and none are plain samplers.
    const bool compares = compare >= D3D12_COMPARISON_FUNC_LESS && compare <= D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    const std::uint32_t reduction = mode == 1 ? 2u : mode == 2 ? 3u : compares ? 1u : 0u;
    const std::uint32_t filter = aniso ? 0x55u : linear(min) << 4 | linear(mag) << 2 | std::uint32_t(mip == 2);
    desc.Filter = static_cast<D3D12_FILTER>(reduction << 7 | filter);
    desc.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(w[6]);
    desc.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(w[7]);
    desc.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(w[8]);
    desc.MipLODBias = real(11);
    desc.MaxAnisotropy = aniso ? std::clamp<std::uint32_t>(w[13], 1, 16) : 1;
    desc.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(std::max<std::uint32_t>(compare, 1));
    for (int i = 0; i < 4; ++i) desc.BorderColor[i] = real(17 + i);
    desc.MinLOD = real(9);
    desc.MaxLOD = mip == 0 ? desc.MinLOD : real(10);  // without a mip filter the console samples mip 0 only
    return desc;
}

void set_page_mapper(PageMapper mapper) {
    g_page_mapper.store(mapper, std::memory_order_release);
}

void set_placed_view_handler(PlacedViewHandler handler) {
    g_placed_view.store(handler, std::memory_order_release);
}

void set_refused_pipeline_handler(RefusedPipelineHandler handler) {
    g_refused_pipeline.store(handler, std::memory_order_release);
}

void set_list_title_handlers(const ListTitleHandlers* handlers) {
    g_list_title.store(handlers, std::memory_order_release);
}

bool ValueWait::met_by(std::uint64_t value) const {
    const std::uint64_t v = wide ? value : (value & 0xffffffffu);
    const std::uint64_t r = wide ? reference : (reference & 0xffffffffu);
    switch (comparison) {
        case 1: return false;
        case 2: return v < r;
        case 3: return v == r;
        case 4: return v <= r;
        case 5: return v > r;
        case 6: return v != r;
        case 7: return v >= r;
        default: return true;
    }
}

std::size_t command_list_value_waits(const void* list, ValueWait* out, std::size_t capacity) {
    if (!is_command_list_proxy(list)) return 0;
    const auto& waits = static_cast<const ListProxy*>(list)->waits;
    for (std::size_t i = 0; i < waits.size() && i < capacity; ++i) out[i] = waits[i];
    return waits.size();
}

std::uint64_t unmet_value_wait_submissions() { return g_unmet_waits.load(std::memory_order_relaxed); }

void set_placed_at_va_pages_hook(PlacedAtVaPagesHook hook) {
    g_placed_at_va_pages.store(hook, std::memory_order_release);
}

ID3D12Device* device_proxy_real_device(const void* device) {
    if (!is_device_proxy(device)) return nullptr;
    return static_cast<const DeviceProxy*>(device)->real;
}

bool device_slot_overridden(unsigned slot, bool console_layout) {
    if (slot >= kMaxSlots) return false;
    const slots::DeviceSlot entry = slots::device_slot(slot);
    const slots::DeviceMethod method = console_layout ? entry.console : entry.pc;
    // A route answers wherever it is (LEONIDA_XS_*_SLOT); a slot it moved off forwards in the PC layout.
    if (method == slots::DeviceMethod::forward || method == slots::DeviceMethod::routed)
        return is_xbox_route(slot) || (console_layout && method == slots::DeviceMethod::routed);
    return true;
}

bool device_slot_overridden(unsigned slot) { return device_slot_overridden(slot, false); }

bool device_proxy_console_layout(const void* device) {
    const void* primary = device_proxy_primary(const_cast<void*>(device));
    return primary && static_cast<const DeviceProxy*>(primary)->layout == Layout::console;
}

bool command_object_proxy_console_layout(const void* object) {
    if (!is_queue_proxy(object) && !is_fence_proxy(object) && !is_allocator_proxy(object)) return false;
    return static_cast<const ChildProxy*>(object)->layout == Layout::console;
}

bool set_present_handler(void* device, PresentHandler handler, void* context) {
    auto* proxy = static_cast<DeviceProxy*>(device_proxy_primary(device));
    if (!proxy) {
        std::fprintf(stderr, "[rt] d3d12_xs: set_present_handler: %p is not a device proxy; no handler set\n", device);
        return false;
    }
    proxy->present_context.store(context, std::memory_order_release);
    proxy->present_handler.store(handler, std::memory_order_release);
    return true;
}

bool device_page_pool(const void* device, std::uint64_t handle, std::uint64_t* address, std::uint32_t* pages) {
    auto* proxy = static_cast<DeviceProxy*>(device_proxy_primary(const_cast<void*>(device)));
    if (!proxy) return false;
    bool found = false;
    AcquireSRWLockShared(&proxy->pool_lock);
    if (handle >= 1 && handle <= proxy->page_pools.size() && proxy->page_pools[handle - 1].live) {
        if (address) *address = proxy->page_pools[handle - 1].address;
        if (pages) *pages = proxy->page_pools[handle - 1].pages;
        found = true;
    }
    ReleaseSRWLockShared(&proxy->pool_lock);
    return found;
}

std::uint64_t device_frames_begun(const void* device) {
    const void* primary = device_proxy_primary(const_cast<void*>(device));
    return primary ? static_cast<const DeviceProxy*>(primary)->frames_begun.load(std::memory_order_relaxed) : 0;
}

std::uint64_t device_frame_last_tick(const void* device, unsigned kind) {
    auto* proxy = static_cast<DeviceProxy*>(device_proxy_primary(const_cast<void*>(device)));
    return proxy ? frame_last(proxy, kind).load(std::memory_order_relaxed) : 0;
}

bool is_queue_proxy(const void* queue) { return is_child_proxy<ChildKind::queue>(queue); }
bool is_fence_proxy(const void* fence) { return is_child_proxy<ChildKind::fence>(fence); }
bool is_allocator_proxy(const void* allocator) { return is_child_proxy<ChildKind::allocator>(allocator); }

ID3D12CommandAllocator* allocator_proxy_real_allocator(const void* allocator) {
    if (!is_allocator_proxy(allocator)) return nullptr;
    return reinterpret_cast<ID3D12CommandAllocator*>(static_cast<const ChildProxy*>(allocator)->real);
}

ID3D12CommandQueue* queue_proxy_real_queue(const void* queue) {
    if (!is_queue_proxy(queue)) return nullptr;
    return reinterpret_cast<ID3D12CommandQueue*>(static_cast<const ChildProxy*>(queue)->real);
}

ID3D12Fence* fence_proxy_real_fence(const void* fence) {
    if (!is_fence_proxy(fence)) return nullptr;
    return reinterpret_cast<ID3D12Fence*>(static_cast<const ChildProxy*>(fence)->real);
}

unsigned queue_proxy_slot_count(const void* queue) {
    return is_queue_proxy(queue) ? static_cast<const ChildProxy*>(queue)->slot_count : 0;
}

unsigned fence_proxy_slot_count(const void* fence) {
    return is_fence_proxy(fence) ? static_cast<const ChildProxy*>(fence)->slot_count : 0;
}

unsigned queue_proxy_table_size() { return kQueueSlots; }
unsigned fence_proxy_table_size() { return kFenceSlots; }
unsigned allocator_proxy_table_size() { return kAllocatorSlots; }
unsigned allocator_proxy_slot_count(const void* allocator) {
    return is_allocator_proxy(allocator) ? static_cast<const ChildProxy*>(allocator)->slot_count : 0;
}

unsigned long command_object_proxy_ref_count(const void* object) {
    if (!is_queue_proxy(object) && !is_fence_proxy(object) && !is_allocator_proxy(object)) return 0;
    return static_cast<const ChildProxy*>(object)->refs.load(std::memory_order_relaxed);
}

std::uint64_t queue_slot_hits(unsigned slot) {
    using S = ChildStats<ChildKind::queue>;
    start_hit_counts();
    return slot < S::kSlots ? S::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool queue_slot_first_call_seen(unsigned slot) {
    using S = ChildStats<ChildKind::queue>;
    return slot < S::kSlots && S::first[slot].load(std::memory_order_relaxed);
}

std::uint64_t fence_slot_hits(unsigned slot) {
    using S = ChildStats<ChildKind::fence>;
    start_hit_counts();
    return slot < S::kSlots ? S::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool fence_slot_first_call_seen(unsigned slot) {
    using S = ChildStats<ChildKind::fence>;
    return slot < S::kSlots && S::first[slot].load(std::memory_order_relaxed);
}

std::uint64_t queue_table_overruns() {
    return ChildStats<ChildKind::queue>::overruns.load(std::memory_order_relaxed);
}

std::uint64_t fence_table_overruns() {
    return ChildStats<ChildKind::fence>::overruns.load(std::memory_order_relaxed);
}

bool command_object_tables_guarded() {
    get_child_vtable<ChildKind::queue>();
    get_child_vtable<ChildKind::fence>();
    get_child_vtable<ChildKind::allocator>();
    return ChildStats<ChildKind::queue>::table_end.load(std::memory_order_acquire) &&
           ChildStats<ChildKind::fence>::table_end.load(std::memory_order_acquire) &&
           ChildStats<ChildKind::allocator>::table_end.load(std::memory_order_acquire);
}

std::uint64_t allocator_slot_hits(unsigned slot) {
    using S = ChildStats<ChildKind::allocator>;
    start_hit_counts();
    return slot < S::kSlots ? S::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool allocator_slot_first_call_seen(unsigned slot) {
    using S = ChildStats<ChildKind::allocator>;
    return slot < S::kSlots && S::first[slot].load(std::memory_order_relaxed);
}

std::uint64_t allocator_table_overruns() {
    return ChildStats<ChildKind::allocator>::overruns.load(std::memory_order_relaxed);
}

bool heap_slot_overridden(unsigned slot) {
    return slot <= 2 || slot == 7 || slot == 9 || slot == 10;
}

std::uint64_t device_slot_hits(unsigned slot) {
    start_hit_counts();
    return slot < kMaxSlots ? g_hits[slot].load(std::memory_order_relaxed) : 0;
}

bool device_slot_first_call_seen(unsigned slot) {
    return slot < kMaxSlots && g_first[slot].load(std::memory_order_relaxed);
}

unsigned long device_proxy_ref_count(const void* device) {
    if (!is_device_proxy(device)) return 0;
    return static_cast<const DeviceProxy*>(device)->refs.load(std::memory_order_relaxed);
}

std::uint64_t heap_slot_hits(unsigned slot) {
    start_hit_counts();
    return slot < kHeapSlots ? g_heap_hits[slot].load(std::memory_order_relaxed) : 0;
}

bool heap_slot_first_call_seen(unsigned slot) {
    return slot < kHeapSlots && g_heap_first[slot].load(std::memory_order_relaxed);
}

bool is_command_list_proxy(const void* list) {
    return list && *reinterpret_cast<void* const*>(list) == reinterpret_cast<void*>(get_list_vtable());
}

ID3D12GraphicsCommandList* command_list_proxy_real_list(const void* list) {
    if (!is_command_list_proxy(list)) return nullptr;
    return static_cast<const ListProxy*>(list)->real;
}

unsigned command_list_proxy_table_size() { return kListSlots; }

std::uint64_t list_slot_hits(unsigned slot) {
    start_hit_counts();
    return slot < kListSlots ? ListStats::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool list_slot_first_call_seen(unsigned slot) {
    return slot < kListSlots && ListStats::first[slot].load(std::memory_order_relaxed);
}

void reset_device_proxy_stats() {
    start_hit_counts();
    for (unsigned slot = 0; slot < kMaxSlots; ++slot) {
        g_hits[slot].store(0, std::memory_order_relaxed);
        g_first[slot].store(false, std::memory_order_relaxed);
        g_beyond_warned[slot].store(false, std::memory_order_relaxed);
    }
    for (unsigned slot = 0; slot < kHeapSlots; ++slot) {
        g_heap_hits[slot].store(0, std::memory_order_relaxed);
        g_heap_first[slot].store(false, std::memory_order_relaxed);
    }
    for (unsigned slot = 0; slot < kListSlots; ++slot) {
        ListStats::hits[slot].store(0, std::memory_order_relaxed);
        ListStats::first[slot].store(false, std::memory_order_relaxed);
    }
    reset_child_stats<ChildKind::queue>();
    reset_child_stats<ChildKind::fence>();
    reset_child_stats<ChildKind::allocator>();
}

}  // namespace rt::d3d12_xs
