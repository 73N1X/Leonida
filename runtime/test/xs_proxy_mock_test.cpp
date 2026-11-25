// Xbox-ABI queue and fence proxies (d3d12_xs_device.h) over synthetic COM objects, with no D3D12 runtime and no GPU:
//   - the slot tables (d3d12_xs_slots.h): every remapped device, queue and list slot, as data and through the
//     proxies of a device created as ID3D12Device11 (the console layouts): which PC method each one reaches with which
//     arguments, and that the slots no source names reach none; the console fence's value word following the fence;
//     the struct returns that take the return buffer first; the console queue description of slot 48; the frame
//     clock of slots 78-81; PresentX through a present handler;
//   - devices that stop at ID3D12Device, Device1, Device3 and Device9: the slots of a missing interface (45/46,
//     50, 75) are refused without reaching the device, the others get the real objects behind the proxies;
//   - fences with and without ID3D12Fence1: 11 or 12 public slots, QueryInterface and slot 11 to match;
//   - creation that fails, that returns S_OK with no object, a null IID or output, and an interface the real object
//     has but the proxy does not model;
//   - OpenSharedHandle of a fence (as ID3D12Fence, IUnknown, Fence1), of a non-fence object, of an invalid handle,
//     and with a null output or IID, with the references of every object it touches;
//   - allocation failures in the proxy (operator new, the arrays it unwraps), injected in this executable only;
//   - --fail-virtualalloc / --fail-virtualprotect / --fail-veh: the guard region's setup failing, injected by
//     patching this executable's own import table before the first proxy table is built (one process each).
// Every device, queue, fence and resource here is a mock that records the calls it gets. No game data, no Xbox DLLs.
#include "../shims/d3d12_xs_device.h"

#include <d3d12.h>
#include <windows.h>

#include <crtdbg.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>
#include <utility>

namespace {

const char* g_label = "xs_proxy_mock";

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "%s line %d: %s\n", g_label, __LINE__, #condition); return 1; } \
} while (0)

// ---- Allocation failures: this executable's global operator new fails the next g_fail_news calls ----

int g_fail_news = 0;     // single-threaded: armed right before the call it is meant for
int g_failed_news = 0;   // allocations refused so far

}  // namespace

// The annotations are vcruntime_new.h's, so /analyze sees the same contract as for the CRT's own operator new.
_NODISCARD _Ret_notnull_ _Post_writable_byte_size_(size) _VCRT_ALLOCATOR
void* __CRTDECL operator new(std::size_t size) {
    if (g_fail_news > 0) {
        --g_fail_news;
        ++g_failed_news;
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
_NODISCARD _Ret_maybenull_ _Success_(return != NULL) _Post_writable_byte_size_(size) _VCRT_ALLOCATOR
void* __CRTDECL operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}
void __CRTDECL operator delete(void* p) noexcept { std::free(p); }
void __CRTDECL operator delete(void* p, std::size_t) noexcept { std::free(p); }
void __CRTDECL operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

namespace xs = rt::d3d12_xs;

// ---- Mocks ----

enum class Kind { device, queue, fence, resource, allocator, command_list, heap };

constexpr unsigned kMockSlots = 128;
constexpr std::uint64_t kFenceHandle = 0x1000, kResourceHandle = 0x2000;  // the handles the mock device opens
constexpr std::uint64_t kCompletedValue = 7;                            // what a mock fence's slot 8 returns
// {5d1e0c4a-2b7f-4e61-9a3d-8c4b2f1e6a70}: an interface the mock queue implements that the proxy does not model.
constexpr GUID kExtraIid{0x5d1e0c4a, 0x2b7f, 0x4e61, {0x9a, 0x3d, 0x8c, 0x4b, 0x2f, 0x1e, 0x6a, 0x70}};

struct Mock {
    void** vtable;
    Kind kind;
    long refs;
    int device_level;        // device: highest ID3D12DeviceN it answers (0 = ID3D12Device only)
    int list_level;          // command_list: 0=base (60 slots), 7=GCL7 (81 slots), 8=GCL8 (82 slots)
    bool fence1;             // fence: answers ID3D12Fence1; device: the fences it makes do
    bool allocator_tag;      // allocator: answers kLeonidaCommandAllocatorTag; device: the allocators it makes do
    bool create_null;        // device: creation returns create_result with no object
    HRESULT create_result;   // device: what creation returns (S_OK makes the object)
    Mock* last_child;        // device: the object its last creation or open made
    unsigned calls[kMockSlots];
    std::uint64_t args[kMockSlots][5];
    std::uint64_t array[kMockSlots][4];  // device: the first entries of the array argument, as received
    // fence: the completed value (8 reads it, 10 and a queue's 14 set it; the proxy's thread-pool wait reads it too, so
    // it is only touched with Interlocked calls) and the events of SetEventOnCompletion (9) not set yet.
    volatile LONG64 value;
    std::uint64_t event_values[16];
    HANDLE events[16];
    unsigned event_count;
    // queue: Signal (14) is held, as a GPU that has not got there yet, until complete_signals.
    bool defer_signals;
    std::uint64_t held_fences[8], held_values[8];
    unsigned held_count;
    D3D12_COMMAND_QUEUE_DESC queue_desc;  // device: the last CreateCommandQueue description, as received
    D3D12_RESOURCE_DESC1 resource_desc;   // device: the last resource description of 27/76 (and 25/68's first)
    float floats[2];                      // command list: 47's depth, 62's bounds
};

Mock g_pool[1024];
unsigned g_pool_used = 0;

void** mock_vtable();

Mock* new_mock(Kind kind) {
    if (g_pool_used == std::size(g_pool)) {
        std::fprintf(stderr, "%s: mock pool exhausted\n", g_label);
        std::exit(1);
    }
    Mock* m = &g_pool[g_pool_used++];
    std::memset(m, 0, sizeof(*m));
    m->vtable = mock_vtable();
    m->kind = kind;
    if (kind == Kind::fence) InterlockedExchange64(&m->value, static_cast<LONG64>(kCompletedValue));
    return m;
}

Mock* as_mock(std::uint64_t word, Kind kind) {
    auto* m = reinterpret_cast<Mock*>(word);
    if (m < g_pool || m >= g_pool + std::size(g_pool) || m->kind != kind) return nullptr;
    return m;
}

std::uint64_t mock_fence_value(const Mock* fence) {
    return static_cast<std::uint64_t>(InterlockedCompareExchange64(const_cast<volatile LONG64*>(&fence->value), 0, 0));
}

// The fence reaches `value`: the events waiting for it are set, as the runtime does.
void signal_mock_fence(Mock* fence, std::uint64_t value) {
    InterlockedExchange64(&fence->value, static_cast<LONG64>(value));
    unsigned kept = 0;
    for (unsigned i = 0; i < fence->event_count; ++i) {
        if (fence->event_values[i] <= value) {
            SetEvent(fence->events[i]);
        } else {
            fence->event_values[kept] = fence->event_values[i];
            fence->events[kept++] = fence->events[i];
        }
    }
    fence->event_count = kept;
}

// The signals a deferring queue holds reach their fences, as the GPU getting there.
void complete_signals(Mock* queue) {
    for (unsigned i = 0; i < queue->held_count; ++i)
        if (Mock* fence = as_mock(queue->held_fences[i], Kind::fence)) signal_mock_fence(fence, queue->held_values[i]);
    queue->held_count = 0;
}

bool mock_has(const Mock* m, REFIID iid) {
    if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object))) return true;
    if (m->kind == Kind::device) {
        if (IsEqualIID(iid, __uuidof(ID3D12Device))) return true;
        const IID levels[] = {__uuidof(ID3D12Device1), __uuidof(ID3D12Device2), __uuidof(ID3D12Device3),
                              __uuidof(ID3D12Device4), __uuidof(ID3D12Device5), __uuidof(ID3D12Device6),
                              __uuidof(ID3D12Device7), __uuidof(ID3D12Device8), __uuidof(ID3D12Device9),
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device10),
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device11),
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device12),
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device13),
#endif
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device14),
#endif
        };
        for (int i = 0; i < m->device_level && i < static_cast<int>(std::size(levels)); ++i)
            if (IsEqualIID(iid, levels[i])) return true;
        return false;
    }
    if (IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Pageable))) return true;
    switch (m->kind) {
        case Kind::queue: return IsEqualIID(iid, __uuidof(ID3D12CommandQueue)) || IsEqualIID(iid, kExtraIid);
        case Kind::fence:
            return IsEqualIID(iid, __uuidof(ID3D12Fence)) || (m->fence1 && IsEqualIID(iid, __uuidof(ID3D12Fence1)));
        case Kind::resource: return IsEqualIID(iid, __uuidof(ID3D12Resource));
        case Kind::heap: return IsEqualIID(iid, __uuidof(ID3D12DescriptorHeap));
        case Kind::command_list:
            if (IsEqualIID(iid, __uuidof(ID3D12CommandList)) ||
                IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList))) return true;
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
            if (m->list_level >= 7 && (IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList7)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList6)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList5)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList4)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList3)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList2)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList1))))
                return true;
#endif
            return false;
        case Kind::allocator:
            return IsEqualIID(iid, __uuidof(ID3D12CommandAllocator)) ||
                   (m->allocator_tag && IsEqualIID(iid, xs::kLeonidaCommandAllocatorTag));
        default: return false;
    }
}

HRESULT mock_query(Mock* m, const IID* riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid || !mock_has(m, *riid)) return E_NOINTERFACE;
    *out = m;
    ++m->refs;
    return S_OK;
}

// A device creating (or opening) a `kind` object as `riid`: S_FALSE without an output, as the PC runtime answers a
// capability check; the device's create_result and create_null otherwise.
HRESULT mock_create(Mock* device, Kind kind, std::uint64_t riid, std::uint64_t out_word) {
    auto** out = reinterpret_cast<void**>(out_word);
    if (!out) return S_FALSE;
    *out = nullptr;
    if (device->create_null || FAILED(device->create_result)) return device->create_result;
    Mock* child = new_mock(kind);
    child->fence1 = device->fence1;
    child->allocator_tag = device->allocator_tag;
    device->last_child = child;
    return mock_query(child, reinterpret_cast<const IID*>(riid), out);
}

void copy_array(Mock* m, unsigned slot, std::uint64_t objects, std::uint64_t count) {
    const auto* in = reinterpret_cast<const std::uint64_t*>(objects);
    for (std::uint32_t i = 0; in && i < static_cast<std::uint32_t>(count) && i < 4; ++i) m->array[slot][i] = in[i];
}

// What the mock device's struct returns write (25/68 GetResourceAllocationInfo/2, 26 GetCustomHeapProperties).
constexpr D3D12_RESOURCE_ALLOCATION_INFO kMockAllocation{0x30000, 0x10000};
constexpr D3D12_HEAP_PROPERTIES kMockHeapProperties{D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK,
                                                    D3D12_MEMORY_POOL_L0, 1, 1};
// What the mock queue's GetDesc (18) writes.
constexpr D3D12_COMMAND_QUEUE_DESC kMockQueueDesc{D3D12_COMMAND_LIST_TYPE_COMPUTE, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 1};

// `words` are the call's first five words (a1..a5), copied into the thunk's own frame.
std::uint64_t mock_call(Mock* m, unsigned slot, const std::uint64_t* words) {
    const std::uint64_t a1 = words[0], a2 = words[1], a3 = words[2], a4 = words[3];
    if (m->kind == Kind::device) {
        switch (slot) {
            case 8:                                                                          // CreateCommandQueue
                if (a1) std::memcpy(&m->queue_desc, reinterpret_cast<const void*>(a1), sizeof(m->queue_desc));
                return static_cast<std::uint32_t>(mock_create(m, Kind::queue, a2, a3));
            case 9: return static_cast<std::uint32_t>(mock_create(m, Kind::allocator, a2, a3));  // CreateCmdAllocator
            case 14: return static_cast<std::uint32_t>(mock_create(m, Kind::heap, a2, a3));       // CreateDescriptorHeap
            case 75: return static_cast<std::uint32_t>(mock_create(m, Kind::queue, a3, a4));   // CreateCommandQueue1
            case 36: return static_cast<std::uint32_t>(mock_create(m, Kind::fence, a3, a4));   // CreateFence
            case 32:                                                                          // OpenSharedHandle
                if (a1 == kFenceHandle) return static_cast<std::uint32_t>(mock_create(m, Kind::fence, a2, a3));
                if (a1 == kResourceHandle) return static_cast<std::uint32_t>(mock_create(m, Kind::resource, a2, a3));
                return static_cast<std::uint32_t>(E_INVALIDARG);
            case 34: case 35: case 46: copy_array(m, slot, a2, a1); break;  // (count, objects, ...)
            case 45: copy_array(m, slot, a1, a3); break;                    // (fences, values, count, ...)
            case 50: copy_array(m, slot, a3, a2); break;                    // (flags, count, objects, fence, value)
            case 25: case 68:  // GetResourceAllocationInfo(2) (this, buffer, visible, count, descs[, each])
                if (a4) std::memcpy(&m->resource_desc, reinterpret_cast<const void*>(a4),
                                    slot == 25 ? sizeof(D3D12_RESOURCE_DESC) : sizeof(D3D12_RESOURCE_DESC1));
                std::memcpy(reinterpret_cast<void*>(a1), &kMockAllocation, sizeof(kMockAllocation));
                return a1;
            case 26:  // GetCustomHeapProperties(this, buffer, node_mask, heap_type)
                std::memcpy(reinterpret_cast<void*>(a1), &kMockHeapProperties, sizeof(kMockHeapProperties));
                return a1;
            case 27:  // CreateCommittedResource(heap, flags, desc, state, clear, riid, out)
                std::memcpy(&m->resource_desc, reinterpret_cast<const void*>(a3), sizeof(D3D12_RESOURCE_DESC));
                break;
            case 76:  // ID3D12Device10::CreateCommittedResource3(heap, flags, desc1, layout, ...)
                if (m->device_level >= 10)
                    std::memcpy(&m->resource_desc, reinterpret_cast<const void*>(a3), sizeof(D3D12_RESOURCE_DESC1));
                break;
            default: break;
        }
        return S_OK;
    }
    if (m->kind == Kind::fence) {
        switch (slot) {
            case 8: return mock_fence_value(m);  // GetCompletedValue
            case 9:                               // SetEventOnCompletion(value, event)
                if (mock_fence_value(m) >= a1) {
                    SetEvent(reinterpret_cast<HANDLE>(a2));
                } else if (m->event_count < std::size(m->events)) {
                    m->event_values[m->event_count] = a1;
                    m->events[m->event_count++] = reinterpret_cast<HANDLE>(a2);
                }
                return S_OK;
            case 10: signal_mock_fence(m, a1); return S_OK;  // Signal(value)
            default: return S_OK;
        }
    }
    if (m->kind == Kind::queue && slot == 10) copy_array(m, slot, a2, a1);  // ExecuteCommandLists(count, lists)
    if (m->kind == Kind::queue && slot == 14) {                                 // Signal(fence, value)
        if (m->defer_signals && m->held_count < std::size(m->held_fences)) {
            m->held_fences[m->held_count] = a1;
            m->held_values[m->held_count++] = a2;
        } else if (Mock* fence = as_mock(a1, Kind::fence)) {
            signal_mock_fence(fence, a2);
        }
    }
    if (m->kind == Kind::queue && slot == 18) {  // GetDesc(this, buffer)
        std::memcpy(reinterpret_cast<void*>(a1), &kMockQueueDesc, sizeof(kMockQueueDesc));
        return a1;
    }
    if (m->kind == Kind::command_list && slot == 28) copy_array(m, slot, a2, a1);  // SetDescriptorHeaps(count, heaps)
    return S_OK;
}

template <unsigned Slot>
std::uint64_t WINAPI mock_slot(Mock* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4,
                               std::uint64_t a5) {
    if constexpr (Slot == 0) {
        return static_cast<std::uint32_t>(mock_query(self, reinterpret_cast<const IID*>(a1),
                                                     reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return static_cast<std::uint64_t>(++self->refs);
    } else if constexpr (Slot == 2) {
        return static_cast<std::uint64_t>(--self->refs);
    } else {
        // A caller may pass fewer words than this thunk declares (OpenSharedHandle passes three), so its frame
        // has no room past the four register homes. The words are copied into this frame and passed by pointer:
        // forwarding them as stack arguments let MSVC tail-call mock_call and write into the caller's saved
        // registers (an optimized build corrupted the test's own locals that way).
        ++self->calls[Slot];
        const std::uint64_t words[] = {a1, a2, a3, a4, a5};
        std::memcpy(self->args[Slot], words, sizeof(words));
        return mock_call(self, Slot, words);
    }
}

// PC ID3D12Device::CreateCommandList takes six words after `this`, one more than the generic mock thunk declares, so
// it gets its own entry in the table: the out pointer is a6 (first stack argument on x64) and has to be written.
HRESULT WINAPI mock_create_command_list(Mock* self, std::uint64_t node_mask, std::uint64_t type,
                                        std::uint64_t allocator, std::uint64_t initial_state, const IID* riid,
                                        void** out) {
    ++self->calls[12];
    self->args[12][0] = node_mask;
    self->args[12][1] = type;
    self->args[12][2] = allocator;
    self->args[12][3] = initial_state;
    self->args[12][4] = reinterpret_cast<std::uint64_t>(riid);
    if (!out) return S_FALSE;
    *out = nullptr;
    if (self->create_null || FAILED(self->create_result)) return self->create_result;
    Mock* child = new_mock(Kind::command_list);
    child->list_level = self->list_level;
    self->last_child = child;
    return mock_query(child, riid, out);
}

// PC ID3D12GraphicsCommandList slots 47 (ClearDepthStencilView) and 62 (OMSetDepthBounds) take floats in XMM registers:
// typed entries, so the floats the list proxy hands on can be checked. (The table is every mock's; nothing else here
// calls these slots on another kind of mock.)
void WINAPI mock_clear_depth_stencil(Mock* self, D3D12_CPU_DESCRIPTOR_HANDLE view, D3D12_CLEAR_FLAGS flags, FLOAT depth,
                                     UINT8 stencil, UINT count, const D3D12_RECT* rects) {
    ++self->calls[47];
    self->args[47][0] = view.ptr;
    self->args[47][1] = static_cast<std::uint64_t>(flags);
    self->args[47][2] = stencil;
    self->args[47][3] = count;
    self->args[47][4] = reinterpret_cast<std::uint64_t>(rects);
    self->floats[0] = depth;
}

void WINAPI mock_depth_bounds(Mock* self, FLOAT min, FLOAT max) {
    ++self->calls[62];
    self->floats[0] = min;
    self->floats[1] = max;
}

template <std::size_t... I>
void fill_mock_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&mock_slot<static_cast<unsigned>(I)>)), ...);
}

void** mock_vtable() {
    static void* table[kMockSlots];
    static const bool filled = [] {
        fill_mock_vtable(table, std::make_index_sequence<kMockSlots>{});
        table[12] = reinterpret_cast<void*>(&mock_create_command_list);
        table[47] = reinterpret_cast<void*>(&mock_clear_depth_stencil);
        table[62] = reinterpret_cast<void*>(&mock_depth_bounds);
        return true;
    }();
    (void)filled;
    return table;
}

// ---- Calls by slot, the way a title compiled against the console headers makes them ----

using SlotFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

std::uint64_t raw(const void* object, unsigned slot, std::uint64_t a1 = 0, std::uint64_t a2 = 0,
                  std::uint64_t a3 = 0, std::uint64_t a4 = 0, std::uint64_t a5 = 0, std::uint64_t a6 = 0,
                  std::uint64_t a7 = 0, std::uint64_t a8 = 0, std::uint64_t a9 = 0, std::uint64_t a10 = 0) {
    void* const* vtable = *reinterpret_cast<void* const* const*>(object);
    return reinterpret_cast<SlotFn>(vtable[slot])(const_cast<void*>(object), a1, a2, a3, a4, a5, a6, a7, a8, a9, a10,
                                                  0, 0, 0, 0);
}

HRESULT hr_of(std::uint64_t value) {
    return static_cast<HRESULT>(static_cast<std::uint32_t>(value));
}

template <class T>
std::uint64_t word(const T* p) {
    return reinterpret_cast<std::uint64_t>(p);
}

const IID kQueueIid = __uuidof(ID3D12CommandQueue);
const IID kFenceIid = __uuidof(ID3D12Fence);
const IID kFence1Iid = __uuidof(ID3D12Fence1);
const IID kUnknownIid = __uuidof(IUnknown);
const IID kResourceIid = __uuidof(ID3D12Resource);
const IID kDevice1Iid = __uuidof(ID3D12Device1);
const D3D12_COMMAND_QUEUE_DESC kQueueDesc{};

// A device proxy over a new mock device, created as `riid`. The proxy takes the mock's one reference.
void* make_device(Mock*& mock, int level, bool fence1, bool allocator_tag = true,
                  const IID& riid = __uuidof(ID3D12Device)) {
    mock = new_mock(Kind::device);
    mock->device_level = level;
    mock->fence1 = fence1;
    mock->allocator_tag = allocator_tag;
    mock->refs = 1;
    void* device = nullptr;
    if (FAILED(xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(mock), riid, &device))) return nullptr;
    return device;
}

// A device created as ID3D12Device11, as the title creates its own: the console layouts. Its allocators answer no tag,
// as the host runtime's do (the console layout wraps them all the same).
void* make_console_device(Mock*& mock, int level = 11, bool fence1 = true) {
    return make_device(mock, level, fence1, false, __uuidof(ID3D12Device11));
}

void* make_queue(void* device) {
    void* queue = nullptr;
    return SUCCEEDED(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&queue)))) ? queue : nullptr;
}

void* make_fence(void* device, const IID& iid = kFenceIid) {
    void* fence = nullptr;
    return SUCCEEDED(hr_of(raw(device, 36, 0, 0, word(&iid), word(&fence)))) ? fence : nullptr;
}

unsigned long release(void* object) {
    return static_cast<unsigned long>(raw(object, 2));
}

// ---- 1. Device interface levels ----

int check_levels() {
    struct Level { int level; unsigned slots; } const levels[] = {{0, 44}, {1, 47}, {3, 51}, {9, 76}};
    for (const Level& l : levels) {
        Mock* mock = nullptr;
        void* device = make_device(mock, l.level, true);
        EXPECT(device && xs::device_proxy_slot_count(device) == l.slots);
        void* queue = make_queue(device);
        void* fence = make_fence(device);
        EXPECT(xs::is_queue_proxy(queue) && xs::is_fence_proxy(fence));
        const auto real_queue = word(xs::queue_proxy_real_queue(queue));
        const auto real_fence = word(xs::fence_proxy_real_fence(fence));
        EXPECT(real_queue && real_fence && real_queue != word(queue) && real_fence != word(fence));
        // What every residency call gets, with the proxies in it replaced by their real objects.
        const std::uint64_t objects[4] = {word(queue), word(fence), real_queue, 0};
        const std::uint64_t unwrapped[4] = {real_queue, real_fence, real_queue, 0};
        const std::uint64_t fences[2] = {word(fence), word(fence)};
        const std::uint64_t values[2] = {1, 2};
        const D3D12_RESIDENCY_PRIORITY priorities[4] = {};
        const auto same = [](const std::uint64_t* got, const std::uint64_t* want, int n) {
            return std::memcmp(got, want, n * sizeof(std::uint64_t)) == 0;
        };

        // 34 MakeResident, with a count whose high dword is not zero: the PC prototype takes a UINT.
        EXPECT(hr_of(raw(device, 34, 0xdeadbeef00000002ull, word(objects))) == S_OK);
        EXPECT(mock->calls[34] == 1 && static_cast<std::uint32_t>(mock->args[34][0]) == 2);
        EXPECT(same(mock->array[34], unwrapped, 2) && mock->array[34][2] == 0);
        // 31 CreateSharedHandle
        EXPECT(hr_of(raw(device, 31, word(fence), 0, GENERIC_ALL, 0, 0)) == S_OK);
        EXPECT(mock->calls[31] == 1 && mock->args[31][0] == real_fence);

        // 45/46: ID3D12Device1
        const HRESULT r45 = hr_of(raw(device, 45, word(fences), word(values), 2, 0, 0));
        const HRESULT r46 = hr_of(raw(device, 46, 4, word(objects), word(priorities)));
        if (l.level >= 1) {
            EXPECT(r45 == S_OK && mock->calls[45] == 1 && mock->array[45][0] == real_fence &&
                   mock->array[45][1] == real_fence);
            EXPECT(r46 == S_OK && mock->calls[46] == 1 && same(mock->array[46], unwrapped, 4));
        } else {
            EXPECT(r45 == E_NOTIMPL && r46 == E_NOTIMPL && mock->calls[45] == 0 && mock->calls[46] == 0);
            void* device1 = nullptr;
            EXPECT(FAILED(hr_of(raw(device, 0, word(&kDevice1Iid), word(&device1)))) && !device1);
        }
        // 50: ID3D12Device3, with the fence argument unwrapped too
        const HRESULT r50 = hr_of(raw(device, 50, 0, 4, word(objects), word(fence), 1));
        if (l.level >= 3) {
            EXPECT(r50 == S_OK && mock->calls[50] == 1 && same(mock->array[50], unwrapped, 4) &&
                   mock->args[50][3] == real_fence);
        } else {
            EXPECT(r50 == E_NOTIMPL && mock->calls[50] == 0);
        }
        // 75: ID3D12Device9::CreateCommandQueue1
        void* queue1 = reinterpret_cast<void*>(1);  // cleared on every failure (bc59ef46's convention)
        const HRESULT r75 = hr_of(raw(device, 75, word(&kQueueDesc), 0, word(&kQueueIid), word(&queue1)));
        if (l.level >= 9) {
            EXPECT(r75 == S_OK && xs::is_queue_proxy(queue1) && mock->calls[75] == 1);
            EXPECT(release(queue1) == 0 && mock->last_child->refs == 0);
        } else {
            EXPECT(r75 == E_NOTIMPL && !queue1 && mock->calls[75] == 0);
        }

        // 133: past every PC device vtable; the retail title calls it. Refused, not a jump through a missing entry.
        EXPECT(hr_of(raw(device, 133)) == E_NOTIMPL && mock->calls[127] == 0);

        // 133 with a handler (CreatePlacedResourceX): the arguments in the title's positions (va, desc, state,
        // clear, riid, out) reach the handler with the real device, the Xbox flag bits masked to the PC ones the host
        // takes (0x20004 -> 0x4, and 0x200 dropped) and the buffer's Xbox layout word made ROW_MAJOR.
        {
            static void* seen_real = nullptr;
            static void* seen_va = nullptr;
            static D3D12_RESOURCE_DESC seen_desc{};
            static GUID seen_iid{};
            static std::uint32_t seen_layout = 0;
            const xs::PlacedAtVaHandler handler = [](ID3D12Device* real_device, void* va,
                                                     const D3D12_RESOURCE_DESC& desc, std::uint32_t layout,
                                                     REFIID iid, void** out) -> HRESULT {
                seen_layout = layout;
                seen_real = real_device;
                seen_va = va;
                seen_desc = desc;
                seen_iid = iid;
                *out = reinterpret_cast<void*>(0x1330);
                return S_OK;
            };
            xs::set_placed_at_va_handler(handler);
            D3D12_RESOURCE_DESC xbox_desc{};
            xbox_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            xbox_desc.Width = 0x10000;
            xbox_desc.Height = xbox_desc.DepthOrArraySize = xbox_desc.MipLevels = 1;
            xbox_desc.SampleDesc.Count = 1;
            xbox_desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x1);  // the Xbox row-major word
            xbox_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x20204);
            const GUID resource_iid = __uuidof(ID3D12Resource);
            void* placed = nullptr;
            void* const va = reinterpret_cast<void*>(0x7f0000000000ull);
            const HRESULT r133 = hr_of(raw(device, 133, word(va), word(&xbox_desc), 0xFFFFFFFFull, 0,
                                           word(&resource_iid), word(&placed)));
            xs::set_placed_at_va_handler(nullptr);
            EXPECT(r133 == S_OK && placed == reinterpret_cast<void*>(0x1330));
            EXPECT(seen_real == xs::device_proxy_real_device(device) && seen_va == va);
            EXPECT(static_cast<std::uint32_t>(seen_desc.Flags) == 0x4);
            EXPECT(seen_desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR && seen_desc.Width == 0x10000);
            EXPECT(std::memcmp(&seen_iid, &resource_iid, sizeof(GUID)) == 0);
            EXPECT(seen_layout == 0xFFFFFFFFu);  // the title's third word, passed through as the barrier layout
            EXPECT(xs::device_slot_overridden(133));
        }

        // 133 with a pages hook (set_placed_at_va_pages_hook): a buffer's range [va, va + width) is handed to it
        // before the handler runs; a texture is not; a hook that fails still leaves the call to the handler.
        {
            static int order = 0;
            static int hook_at = 0;
            static int handler_at = 0;
            static const void* hook_va = nullptr;
            static std::size_t hook_bytes = 0;
            static bool hook_result = true;
            order = hook_at = handler_at = 0;
            const xs::PlacedAtVaPagesHook hook = [](const void* va, std::size_t bytes) -> bool {
                hook_at = ++order;
                hook_va = va;
                hook_bytes = bytes;
                return hook_result;
            };
            const xs::PlacedAtVaHandler handler = [](ID3D12Device*, void*, const D3D12_RESOURCE_DESC&, std::uint32_t,
                                                     REFIID, void** out) -> HRESULT {
                handler_at = ++order;
                *out = reinterpret_cast<void*>(0x1331);
                return S_OK;
            };
            xs::set_placed_at_va_handler(handler);
            xs::set_placed_at_va_pages_hook(hook);
            D3D12_RESOURCE_DESC xbox_desc{};
            xbox_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            xbox_desc.Width = 0xadf0000;
            xbox_desc.Height = xbox_desc.DepthOrArraySize = xbox_desc.MipLevels = 1;
            xbox_desc.SampleDesc.Count = 1;
            xbox_desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x1);
            const GUID resource_iid = __uuidof(ID3D12Resource);
            void* placed = nullptr;
            void* const va = reinterpret_cast<void*>(0x7f0000010000ull);
            auto place = [&]() {
                placed = nullptr;
                return hr_of(raw(device, 133, word(va), word(&xbox_desc), 0xFFFFFFFFull, 0, word(&resource_iid),
                                 word(&placed)));
            };
            const HRESULT buffer_hr = place();
            EXPECT(buffer_hr == S_OK && placed == reinterpret_cast<void*>(0x1331));
            EXPECT(hook_at == 1 && handler_at == 2 && hook_va == va && hook_bytes == 0xadf0000);

            hook_result = false;
            order = hook_at = handler_at = 0;
            const HRESULT refused_hr = place();
            EXPECT(refused_hr == S_OK && hook_at == 1 && handler_at == 2);
            hook_result = true;

            xbox_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            xbox_desc.Width = 64;
            xbox_desc.Height = 64;
            order = hook_at = handler_at = 0;
            const HRESULT texture_hr = place();
            EXPECT(texture_hr == S_OK && hook_at == 0 && handler_at == 1);

            xs::set_placed_at_va_pages_hook(nullptr);
            xs::set_placed_at_va_handler(nullptr);
        }

        Mock* const queue_mock = reinterpret_cast<Mock*>(real_queue);
        Mock* const fence_mock = reinterpret_cast<Mock*>(real_fence);
        EXPECT(release(queue) == 0 && release(fence) == 0);
        EXPECT(queue_mock->refs == 0 && fence_mock->refs == 0);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: devices up to ID3D12Device, Device1, Device3, Device9: 44/47/51/76 slots, 45/46/50/75 refused "
                "without the interface, residency arrays and fences unwrapped otherwise\n", g_label);
    return 0;
}

// ---- 2. Fences with and without ID3D12Fence1 ----

int check_fence1() {
    for (const bool fence1 : {false, true}) {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, fence1);
        EXPECT(device);
        const unsigned long device_refs = xs::device_proxy_ref_count(device);
        void* fence = make_fence(device);
        EXPECT(xs::is_fence_proxy(fence) && xs::fence_proxy_slot_count(fence) == (fence1 ? 12u : 11u));
        Mock* const fence_mock = mock->last_child;
        void* as_fence1 = nullptr;
        const HRESULT qi = hr_of(raw(fence, 0, word(&kFence1Iid), word(&as_fence1)));
        if (fence1) {
            EXPECT(qi == S_OK && as_fence1 == fence && release(as_fence1) == 1);
        } else {
            EXPECT(qi == E_NOINTERFACE && !as_fence1);
        }
        EXPECT(raw(fence, 8) == kCompletedValue);  // GetCompletedValue forwarded
        const HRESULT slot11 = hr_of(raw(fence, 11));
        if (fence1) {
            EXPECT(slot11 == S_OK && fence_mock->calls[11] == 1);
        } else {
            EXPECT(slot11 == E_NOTIMPL && fence_mock->calls[11] == 0);  // not forwarded past an 11-slot vtable
        }
        EXPECT(release(fence) == 0 && fence_mock->refs == 0);

        // Asked for as ID3D12Fence1: made as ID3D12Fence, then refused when the fence lacks Fence1, and released.
        void* created1 = nullptr;
        const HRESULT made1 = hr_of(raw(device, 36, 0, 0, word(&kFence1Iid), word(&created1)));
        if (fence1) {
            EXPECT(made1 == S_OK && xs::is_fence_proxy(created1) && release(created1) == 0);
        } else {
            EXPECT(made1 == E_NOINTERFACE && !created1);
        }
        EXPECT(mock->last_child->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: fences without and with ID3D12Fence1: 11/12 slots, QueryInterface, slot 11 and creation as "
                "Fence1 refused or forwarded to match\n", g_label);
    return 0;
}

// ---- 3. Creation failures and refused interfaces ----

int check_create_failures() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    void* out = reinterpret_cast<void*>(1);

    mock->create_null = true;  // S_OK with no object
    mock->create_result = S_OK;
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))) == E_FAIL && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 36, 0, 0, word(&kFenceIid), word(&out))) == E_FAIL && !out);
    mock->create_null = false;
    mock->create_result = E_OUTOFMEMORY;  // the real failure, passed on
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))) == E_OUTOFMEMORY && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 75, word(&kQueueDesc), 0, word(&kQueueIid), word(&out))) == E_OUTOFMEMORY && !out);
    mock->create_result = S_OK;

    const unsigned calls8 = mock->calls[8];
    out = reinterpret_cast<void*>(1);  // a null IID: E_POINTER, the output cleared, nothing created
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), 0, word(&out))) == E_POINTER && !out && mock->calls[8] == calls8);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 36, 0, 0, 0, word(&out))) == E_POINTER && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 75, word(&kQueueDesc), 0, 0, word(&out))) == E_POINTER && !out);
    // No output: the device's own answer (S_FALSE here), as the PC runtime gives it.
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), 0)) == S_FALSE && mock->calls[8] == calls8 + 1);
    EXPECT(hr_of(raw(device, 36, 0, 0, word(&kFenceIid), 0)) == S_FALSE);

    // An interface the real queue implements but the proxy does not model: created, refused, released.
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kExtraIid), word(&out))) == E_NOINTERFACE && !out);
    EXPECT(mock->last_child->refs == 0);
    void* queue = make_queue(device);
    EXPECT(queue);
    Mock* const queue_mock = mock->last_child;
    void* extra = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(queue, 0, word(&kExtraIid), word(&extra))) == E_NOINTERFACE && !extra);
    EXPECT(xs::command_object_proxy_ref_count(queue) == 1 && queue_mock->refs == 1);
    EXPECT(release(queue) == 0 && queue_mock->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: creation with no object (E_FAIL), a failing device, null IID and output, and an interface the "
                "proxy does not model leave no reference\n", g_label);
    return 0;
}

// ---- 4. OpenSharedHandle ----

int check_open_shared() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, false);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);

    for (const IID* iid : {&kFenceIid, &kUnknownIid}) {  // a fence, whatever it is asked for as, comes back wrapped
        void* opened = nullptr;
        EXPECT(hr_of(raw(device, 32, kFenceHandle, word(iid), word(&opened))) == S_OK);
        Mock* const fence_mock = mock->last_child;
        EXPECT(xs::is_fence_proxy(opened) && word(xs::fence_proxy_real_fence(opened)) == word(fence_mock));
        EXPECT(xs::command_object_proxy_ref_count(opened) == 1 && fence_mock->refs == 1);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs + 1);
        EXPECT(release(opened) == 0 && fence_mock->refs == 0);
    }
    void* out = reinterpret_cast<void*>(1);  // as Fence1, which this fence lacks: the real open fails, passed on
    EXPECT(hr_of(raw(device, 32, kFenceHandle, word(&kFence1Iid), word(&out))) == E_NOINTERFACE && !out);
    EXPECT(mock->last_child->refs == 0);

    for (const IID* iid : {&kResourceIid, &kUnknownIid}) {  // not a fence: the real object, unchanged
        void* opened = nullptr;
        EXPECT(hr_of(raw(device, 32, kResourceHandle, word(iid), word(&opened))) == S_OK);
        Mock* const resource_mock = mock->last_child;
        EXPECT(opened == resource_mock && !xs::is_fence_proxy(opened) && !xs::is_queue_proxy(opened));
        EXPECT(resource_mock->refs == 1 && release(opened) == 0);
    }
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);

    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 32, 0xbad, word(&kFenceIid), word(&out))) == E_INVALIDARG && !out);
    EXPECT(hr_of(raw(device, 32, kFenceHandle, word(&kFenceIid), 0)) == S_FALSE);  // the device's own answer
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 32, kFenceHandle, 0, word(&out))) == E_POINTER && !out);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: OpenSharedHandle wraps a fence (as ID3D12Fence and IUnknown), passes a resource, a failed open, "
                "an invalid handle and a null output through, with balanced references\n", g_label);
    return 0;
}

// ---- 5. Allocation failures in the proxy ----

int check_alloc_failures() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    void* queue = make_queue(device);
    void* fence = make_fence(device);
    EXPECT(queue && fence);
    const std::uint64_t objects[2] = {word(queue), word(fence)};

    const auto failing = [&](auto call) {  // one allocation refused, the result of the call
        const int before = g_failed_news;
        g_fail_news = 1;
        const HRESULT hr = call();
        g_fail_news = 0;
        return g_failed_news == before + 1 ? hr : S_OK;
    };
    void* out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 36, 0, 0, word(&kFenceIid), word(&out))); }) == E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);  // the real fence was made and released
    out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))); }) ==
           E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);
    out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 32, kFenceHandle, word(&kFenceIid), word(&out))); }) ==
           E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);
    // Up to 16 entries the unwrapped copy is inline: an armed failure is not even reached, and the call goes on.
    g_fail_news = 1;
    const HRESULT inline_result = hr_of(raw(device, 34, 2, word(objects)));
    const bool untouched = g_fail_news == 1;
    g_fail_news = 0;
    EXPECT(inline_result == S_OK && untouched && mock->array[34][0] == word(xs::queue_proxy_real_queue(queue)));

    // A longer array is allocated; refused, the call returns E_OUTOFMEMORY and reaches no device.
    constexpr unsigned kLong = 32;
    std::uint64_t many[kLong], many_fences[kLong], values[kLong];
    D3D12_RESIDENCY_PRIORITY priorities[kLong] = {};
    for (unsigned i = 0; i < kLong; ++i) {
        many[i] = (i % 2 != 0) ? word(fence) : word(queue);
        many_fences[i] = word(fence);
        values[i] = 1;
    }
    const unsigned calls34 = mock->calls[34], calls45 = mock->calls[45], calls46 = mock->calls[46];
    const unsigned calls50 = mock->calls[50];
    EXPECT(failing([&] { return hr_of(raw(device, 34, kLong, word(many))); }) == E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 45, word(many_fences), word(values), kLong, 0, 0)); }) ==
           E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 46, kLong, word(many), word(priorities))); }) == E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 50, 0, kLong, word(many), word(fence), 1)); }) == E_OUTOFMEMORY);
    EXPECT(mock->calls[34] == calls34 && mock->calls[45] == calls45 && mock->calls[46] == calls46 &&
           mock->calls[50] == calls50);  // nothing reached the device
    // And without the failure the long array arrives unwrapped.
    EXPECT(hr_of(raw(device, 34, kLong, word(many))) == S_OK && mock->calls[34] == calls34 + 1);
    EXPECT(mock->array[34][0] == word(xs::queue_proxy_real_queue(queue)) &&
           mock->array[34][1] == word(xs::fence_proxy_real_fence(fence)));

    // The device proxy itself: on failure the caller keeps its reference.
    Mock* other = new_mock(Kind::device);
    other->refs = 1;
    void* second = reinterpret_cast<void*>(1);
    EXPECT(failing([&] {
               return xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(other), __uuidof(ID3D12Device),
                                              &second);
           }) == E_OUTOFMEMORY);
    EXPECT(!second && other->refs == 1);

    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    Mock* const fence_mock = reinterpret_cast<Mock*>(xs::fence_proxy_real_fence(fence));
    EXPECT(release(queue) == 0 && release(fence) == 0 && queue_mock->refs == 0 && fence_mock->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: a refused allocation in queue, fence, shared-fence and device proxy creation and in the four "
                "unwrapped arrays returns E_OUTOFMEMORY, reaches no device and leaks nothing\n", g_label);
    return 0;
}

// ---- 6. Command allocators (device slot 9) ----

int check_allocators() {
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    // An allocator that answers Leonida's tag (its lists resolve a proxy) comes back wrapped.
    {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, true, true);
        EXPECT(device);
        const unsigned long device_refs = xs::device_proxy_ref_count(device);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, word(&allocator_iid), word(&allocator))) == S_OK);
        Mock* const real = mock->last_child;
        EXPECT(xs::is_allocator_proxy(allocator) && word(xs::allocator_proxy_real_allocator(allocator)) == word(real));
        EXPECT(mock->args[9][0] == D3D12_COMMAND_LIST_TYPE_DIRECT && real->refs == 1);
        EXPECT(xs::allocator_proxy_slot_count(allocator) == 9 && xs::device_proxy_ref_count(device) == device_refs + 1);
        // Reset (slot 8) reaches the real allocator; every slot past it up to the table end is refused.
        EXPECT(hr_of(raw(allocator, 8)) == S_OK && real->calls[8] == 1);
        for (unsigned slot = 9; slot < xs::allocator_proxy_table_size(); ++slot)
            EXPECT(hr_of(raw(allocator, slot)) == E_NOTIMPL && real->calls[slot] == 0);
        // The tag and the real object stay hidden: QueryInterface answers only the modelled interfaces.
        void* found = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(allocator, 0, word(&xs::kLeonidaCommandAllocatorTag), word(&found))) == E_NOINTERFACE &&
               !found);
        EXPECT(hr_of(raw(allocator, 0, word(&allocator_iid), word(&found))) == S_OK && found == allocator);
        EXPECT(release(found) == 1);
        // An allocator is a pageable: MakeResident gives the real runtime the real allocator.
        const std::uint64_t objects[1] = {word(allocator)};
        EXPECT(hr_of(raw(device, 34, 1, word(objects))) == S_OK && mock->array[34][0] == word(real));
        EXPECT(release(allocator) == 0 && real->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        // Null IID, null output, a failing or empty creation, an IID the allocator lacks.
        void* out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, 0, word(&out))) == E_POINTER && !out);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), 0)) == S_FALSE);  // the device's own answer
        mock->create_result = E_OUTOFMEMORY;
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out))) == E_OUTOFMEMORY && !out);
        mock->create_result = S_OK;
        mock->create_null = true;
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out))) == E_FAIL && !out);
        mock->create_null = false;
        const IID fence_iid = __uuidof(ID3D12Fence);
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&fence_iid), word(&out))) == E_NOINTERFACE && !out);
        EXPECT(mock->last_child->refs == 0);
        // A refused allocation for the proxy: E_OUTOFMEMORY, and the real allocator is released.
        g_fail_news = 1;
        out = reinterpret_cast<void*>(1);
        const HRESULT starved = hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out)));
        g_fail_news = 0;
        EXPECT(starved == E_OUTOFMEMORY && !out && mock->last_child->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    // An allocator without the tag (a PC runtime allocator, whose lists could not resolve a proxy) comes back as it
    // is, as the interface asked for.
    {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, true, false);
        EXPECT(device);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
        EXPECT(allocator == mock->last_child && !xs::is_allocator_proxy(allocator) && mock->last_child->refs == 1);
        Mock* const real = mock->last_child;
        EXPECT(release(allocator) == 0 && real->refs == 0);
        const IID fence_iid = __uuidof(ID3D12Fence);
        void* out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&fence_iid), word(&out))) == E_NOINTERFACE && !out);
        EXPECT(mock->last_child->refs == 0);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: allocators answering Leonida's tag are wrapped (Reset forwarded, tail refused, residency "
                "unwrapped, references balanced); others come back unwrapped; failures leave no reference\n", g_label);
    return 0;
}

// ---- 7. Xbox device slots from the public HLE (48, 73, 78, 79, 80) ----
//
// A device that answers ID3D12Device11 (80 PC slots) is the Vulkan layer's shape: slots 78/79 are inside its table
// (CreateReservedResource2/CreateSampler2 there) and 80 is just past it. None of the five may reach the mock's own
// slot: the mock's call counter proves the route, and the returned token/queue/list proves the handler ran.
int check_xbox_slots() {
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true);
    EXPECT(device && xs::device_proxy_slot_count(device) == 80);
    xs::reset_device_proxy_stats();

    // 78 SetFrameIntervalX(output, interval_us, period, flags): S_OK, not forwarded to PC slot 78.
    EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK);
    EXPECT(mock->calls[78] == 0 && xs::device_slot_hits(78) == 1);
    // 79 ScheduleFrameEventX(kind, offset_us, list, flags): S_OK, not forwarded to PC slot 79.
    EXPECT(hr_of(raw(device, 79, 0, 0, 0x1234, 0)) == S_OK);
    EXPECT(mock->calls[79] == 0 && xs::device_slot_hits(79) == 1);

    // 80 WaitFrameEventX(kind, timeout_ms, list, flags, &token): VIctory's clock (frame.rs). A blocking origin wait
    // ends on the next tick with the next token; a poll that times out answers S_FALSE and makes no token unless it
    // asks for one (flag 2); a null token pointer takes the frame without a write. 81 writes a count of 0.
    std::uint64_t token = 0, token2 = 0;
    EXPECT(hr_of(raw(device, 80, 0, 0xffffffffu, 0, 0, word(&token))) == S_OK && token == 1);
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, word(&token2))) == S_FALSE && token2 == 0);
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 2, word(&token2))) == S_FALSE && token2 == 2);
    EXPECT(hr_of(raw(device, 80, 0, 0xffffffffu, 0, 0, 0)) == S_OK && xs::device_frames_begun(device) == 3);
    EXPECT(mock->calls[80] == 0 && xs::device_slot_hits(80) == 4);
    std::uint32_t count = 7;
    EXPECT(hr_of(raw(device, 81, token, 0xff, word(&count), 0)) == S_OK && count == 0 && mock->calls[81] == 0);

    // 48 CreateCommandQueueX(desc, riid, out): the console description {type, flags, engine or pipe, queue} (V
    // queue.rs:464-495) becomes a PC queue of that type, the queue proxy made through the slot 8 path.
    const std::uint32_t compute_x[4] = {D3D12_COMMAND_LIST_TYPE_COMPUTE, 0, 1, 0};  // the title's compute queue
    void* queue = nullptr;
    EXPECT(hr_of(raw(device, 48, word(compute_x), word(&kQueueIid), word(&queue))) == S_OK);
    EXPECT(xs::is_queue_proxy(queue) && mock->calls[48] == 0 && mock->calls[8] == 1);
    EXPECT(mock->queue_desc.Type == D3D12_COMMAND_LIST_TYPE_COMPUTE && mock->queue_desc.Priority == 0 &&
           mock->queue_desc.Flags == D3D12_COMMAND_QUEUE_FLAG_NONE && mock->queue_desc.NodeMask == 0);
    // Pipe and queue numbers never reach the PC Flags or NodeMask (L retail_boot_chain §3.2: the PC runtime refuses
    // them); of the console flags only bit 0 goes on, as DISABLE_GPU_TIMEOUT.
    const std::uint32_t direct_x[4] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0x3, 3, 2};
    void* queue2 = nullptr;
    EXPECT(hr_of(raw(device, 48, word(direct_x), word(&kQueueIid), word(&queue2))) == S_OK && mock->calls[8] == 2);
    EXPECT(mock->queue_desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && mock->queue_desc.NodeMask == 0 &&
           mock->queue_desc.Flags == D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT);
    EXPECT(release(queue2) == 0);
    void* refused = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 48, 0, word(&kQueueIid), word(&refused))) == E_INVALIDARG && !refused &&
           mock->calls[8] == 2);

    // 73 CreateCommandListX(desc, allocator, initialState, riid, out): the type is desc's first word, the allocator
    // proxy is unwrapped, the node mask is 0 and the PC six-argument CreateCommandList (slot 12) is reached.
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
    EXPECT(xs::is_allocator_proxy(allocator));
    Mock* const real_allocator = reinterpret_cast<Mock*>(xs::allocator_proxy_real_allocator(allocator));
    struct XboxCommandListDesc {
        std::uint32_t type;
        std::uint32_t node_mask;
        std::uint64_t reserved[2];
    } desc{};
    desc.type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.node_mask = 7;  // ignored by the console call, as the HLE does
    const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
    void* list = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), word(&list))) == S_OK);
    EXPECT(xs::is_command_list_proxy(list));
    EXPECT(xs::command_list_proxy_real_list(list) &&
           reinterpret_cast<Mock*>(xs::command_list_proxy_real_list(list))->kind == Kind::command_list);
    EXPECT(mock->calls[73] == 0 && mock->calls[12] == 1 && xs::device_slot_hits(73) == 1);
    EXPECT(mock->args[12][0] == 0 && mock->args[12][1] == D3D12_COMMAND_LIST_TYPE_DIRECT);
    EXPECT(mock->args[12][2] == word(real_allocator) && mock->args[12][3] == 0);
    // The real list is made as ID3D12GraphicsCommandList whatever the title asks for (a console list interface).
    EXPECT(mock->args[12][4] && *reinterpret_cast<const IID*>(mock->args[12][4]) == list_iid);

    // Release the list before the refusal calls below: the COM convention (and the handler) nulls the caller's
    // output on failure, so an invalid call would clear this pointer.
    EXPECT(release(list) == 0);

    // Missing description, allocator or output: E_INVALIDARG/E_POINTER before the real device is called.
    EXPECT(hr_of(raw(device, 73, 0, word(allocator), 0, word(&list_iid), word(&list))) == E_INVALIDARG);
    EXPECT(hr_of(raw(device, 73, word(&desc), 0, 0, word(&list_iid), word(&list))) == E_INVALIDARG);
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), 0)) == E_POINTER);

    EXPECT(xs::device_slot_overridden(48) && xs::device_slot_overridden(73) && xs::device_slot_overridden(78) &&
           xs::device_slot_overridden(79) && xs::device_slot_overridden(80) && xs::device_slot_overridden(81));

    EXPECT(release(allocator) == 0 && real_allocator->refs == 0);
    EXPECT(release(queue) == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: device slots 48/73/78-81 route to a queue (console description), a list and VIctory's frame "
                "clock without reaching the mock's own PC slots\n", g_label);
    return 0;
#else
    std::printf("%s: ID3D12Device11 is not in this SDK; Xbox slot 80's beyond-table case is not built\n", g_label);
    return 0;
#endif
}

// ---- 8. Environment overrides for the Xbox device slots (one process each, read once) ----

int run_env_slots(bool valid) {
    const char* frame_interval = valid ? "96" : "2";
    const char* schedule_event = valid ? "97" : "2";
    const char* wait_event = valid ? "98" : "2";
    const char* queue_x = valid ? "99" : "2";
    const char* list_x = valid ? "100" : "2";
    const char* frame_statistics = valid ? "101" : "2";
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_FRAME_INTERVAL_SLOT", frame_interval) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_SCHEDULE_EVENT_SLOT", schedule_event) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_WAIT_EVENT_SLOT", wait_event) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_QUEUE_X_SLOT", queue_x) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_LIST_X_SLOT", list_x) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_FRAME_STATISTICS_SLOT", frame_statistics) != FALSE);

    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true);
    EXPECT(device);
    xs::reset_device_proxy_stats();
    std::uint64_t token = 0;
    std::uint32_t count = 7;
    if (valid) {
        // Every route moved: the new slot is handled, the default slot is forwarded to the mock.
        EXPECT(hr_of(raw(device, 96, 0, 16667, 1, 0)) == S_OK && mock->calls[96] == 0);
        EXPECT(hr_of(raw(device, 97, 0, 0, 0, 0)) == S_OK && mock->calls[97] == 0);
        EXPECT(hr_of(raw(device, 98, 0, 0xffffffffu, 0, 0, word(&token))) == S_OK && token && mock->calls[98] == 0);
        EXPECT(hr_of(raw(device, 101, token, 0xff, word(&count), 0)) == S_OK && count == 0 && mock->calls[101] == 0);
        void* queue = nullptr;
        const std::uint32_t direct_x[4] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
        EXPECT(hr_of(raw(device, 99, word(direct_x), word(&kQueueIid), word(&queue))) == S_OK);
        EXPECT(xs::is_queue_proxy(queue) && mock->calls[99] == 0);
        const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
        struct {
            std::uint32_t type, node_mask;
        } desc{static_cast<std::uint32_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), 0};
        const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
        void* list = nullptr;
        EXPECT(hr_of(raw(device, 100, word(&desc), word(allocator), 0, word(&list_iid), word(&list))) == S_OK);
        EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK && mock->calls[78] == 1);
        EXPECT(release(list) == 0);
        EXPECT(release(allocator) == 0);
        EXPECT(release(queue) == 0);
    } else {
        // Values outside 3..127 are ignored: the six defaults still answer, and a PC slot that is not a default is
        // forwarded unchanged.
        EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK && mock->calls[78] == 0);
        EXPECT(hr_of(raw(device, 79, 0, 0, 0, 0)) == S_OK && mock->calls[79] == 0);
        EXPECT(hr_of(raw(device, 80, 0, 0xffffffffu, 0, 0, word(&token))) == S_OK && token && mock->calls[80] == 0);
        EXPECT(hr_of(raw(device, 81, token, 0xff, word(&count), 0)) == S_OK && count == 0 && mock->calls[81] == 0);
        EXPECT(hr_of(raw(device, 70)) == S_OK && mock->calls[70] == 1);  // not an Xbox slot: forwarded
    }
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: environment slot overrides %s\n", g_label,
                valid ? "move all six routes" : "outside 3..127 are ignored");
    return 0;
}

// ---- 9. The refused tail and the guard region ----

int check_tail(bool want_guarded) {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    void* queue = make_queue(device);
    void* fence = make_fence(device);
    EXPECT(queue && fence);
    EXPECT(xs::command_object_tables_guarded() == want_guarded);
    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    // Signal (14) still reaches the real queue, with the real fence.
    EXPECT(hr_of(raw(queue, 14, word(fence), 5)) == S_OK && queue_mock->calls[14] == 1 &&
           queue_mock->args[14][0] == word(xs::fence_proxy_real_fence(fence)) && queue_mock->args[14][1] == 5);
    for (unsigned slot = 19; slot < xs::queue_proxy_table_size(); ++slot)
        EXPECT(hr_of(raw(queue, slot)) == E_NOTIMPL && queue_mock->calls[slot] == 0);
    for (unsigned slot = 12; slot < xs::fence_proxy_table_size(); ++slot) EXPECT(hr_of(raw(fence, slot)) == E_NOTIMPL);
    EXPECT(xs::queue_table_overruns() == 0 && xs::fence_table_overruns() == 0);
    EXPECT(release(queue) == 0 && release(fence) == 0 && release(device) == 0 && mock->refs == 0);
    return 0;
}

// ---- Guard-region setup failures: this executable's import table, patched before the first proxy table ----

std::atomic<int> g_injected{0};
bool g_armed = false;
void* g_real = nullptr;

LPVOID WINAPI fail_virtual_alloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
    if (g_armed && (type & MEM_RESERVE) && size == 64 * 1024) {
        ++g_injected;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    return reinterpret_cast<decltype(&VirtualAlloc)>(g_real)(address, size, type, protect);
}

BOOL WINAPI fail_virtual_protect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old) {
    if (g_armed && protect == PAGE_READONLY) {
        ++g_injected;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return reinterpret_cast<decltype(&VirtualProtect)>(g_real)(address, size, protect, old);
}

PVOID WINAPI fail_add_veh(ULONG, PVECTORED_EXCEPTION_HANDLER) {
    ++g_injected;
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
}

// Points this executable's import of `function` (by name) at `replacement`; the proxy code is linked into it.
bool patch_import(const char* function, void* replacement) {
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) return false;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress); desc->Name; ++desc) {
        const DWORD lookup = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + lookup);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), function) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            g_real = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);  // through the replacement, not armed yet
            return true;
        }
    }
    return false;
}

int run_fault_mode(const char* mode) {
    struct Mode { const char* flag; const char* function; void* replacement; bool guarded; } const modes[] = {
        {"--fail-virtualalloc", "VirtualAlloc", reinterpret_cast<void*>(&fail_virtual_alloc), false},
        {"--fail-virtualprotect", "VirtualProtect", reinterpret_cast<void*>(&fail_virtual_protect), true},
        {"--fail-veh", "AddVectoredExceptionHandler", reinterpret_cast<void*>(&fail_add_veh), false},
    };
    for (const Mode& m : modes) {
        if (std::strcmp(mode, m.flag) != 0) continue;
        g_label = m.flag;
        EXPECT(patch_import(m.function, m.replacement));
        g_armed = true;
        const int failed = check_tail(m.guarded);
        // Disarmed once the tables exist: with a static CRT the CRT's own calls go through this import table too
        // (its exit path asks VirtualProtect for read-only pages), and only the proxy's calls are meant here.
        g_armed = false;
        if (failed) return 1;
        EXPECT(g_injected.load() > 0);  // the proxy's own call was the one refused
        std::printf("%s: %s refused %d time(s): the tables %s guarded, the proxies forward and refuse their tail as "
                    "before\n", m.flag, m.function, g_injected.load(), m.guarded ? "stay" : "are not");
        return 0;
    }
    return 2;
}

}  // namespace

// A failure ends the process with a message on stderr, never in a dialog a test run would wait on: abort() (an
// exception that escapes, for one), the debug CRT's reports, and Windows' fault and critical-error boxes.
static void no_dialogs() {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG  // the report functions exist only in the debug CRT (empty macros otherwise)
    for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

int check_console_command_list() {
    // 1. Slot mapping checks (the whole table is swept in check_list_table)
    EXPECT(xs::console_to_pc_list_slot(14) == 14);   // Dispatch
    EXPECT(xs::console_to_pc_list_slot(26) == 26);   // ResourceBarrier
    EXPECT(xs::console_to_pc_list_slot(31) == xs::kListNotForwarded);  // named by no source (+4 said ExecuteBundle)
    EXPECT(xs::console_to_pc_list_slot(32) == 28);   // SetDescriptorHeaps
    EXPECT(xs::console_to_pc_list_slot(33) == 29);   // SetComputeRootSignature
    EXPECT(xs::console_to_pc_list_slot(35) == 31);   // SetComputeRootDescriptorTable
    EXPECT(xs::console_to_pc_list_slot(60) == xs::kListNotForwarded);  // no source names it (the old map said EndEvent)
    EXPECT(xs::console_to_pc_list_slot(61) == 59);   // ExecuteIndirect
    EXPECT(xs::console_to_pc_list_slot(85) == 16);   // CopyTextureRegion
    EXPECT(xs::console_to_pc_list_slot(146) == 80);  // GCL7 Barrier
    EXPECT(xs::console_to_pc_list_slot(148) == xs::kListNotForwarded); // AllocateUploadMemory

    Mock* mock_device = nullptr;
    void* device = make_device(mock_device, 11, true);
    EXPECT(device != nullptr);

    // 2. PC Slot 12 vs Xbox Slot 73
    // PC Slot 12 returns unwrapped list directly, so PC tests like tiny_frame can call slot 48 (ClearRenderTargetView)
    void* pc_list = nullptr;
    const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
    EXPECT(hr_of(raw(device, 12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, word(&list_iid), word(&pc_list))) == S_OK);
    EXPECT(pc_list != nullptr);
    EXPECT(!xs::is_command_list_proxy(pc_list));
    Mock* real_pc_list = reinterpret_cast<Mock*>(pc_list);
    EXPECT(real_pc_list->kind == Kind::command_list);
    // In PC ABI, slot 48 is ClearRenderTargetView
    raw(pc_list, 48, 0x1111, 0x2222, 0, 0);
    EXPECT(real_pc_list->calls[48] == 1);
    EXPECT(release(pc_list) == 0);

    // Xbox Slot 73 returns wrapped console command list proxy
    void* allocator = nullptr;
    const IID alloc_iid = __uuidof(ID3D12CommandAllocator);
    EXPECT(hr_of(raw(device, 9, 0, word(&alloc_iid), word(&allocator))) == S_OK);
    EXPECT(xs::is_allocator_proxy(allocator));
    Mock* real_allocator = reinterpret_cast<Mock*>(xs::allocator_proxy_real_allocator(allocator));

    struct XboxCommandListDesc {
        std::uint32_t type;
        std::uint32_t node_mask;
        std::uint64_t reserved[2];
    } desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, {0, 0}};

    void* list_proxy = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), word(&list_proxy))) == S_OK);
    EXPECT(xs::is_command_list_proxy(list_proxy));
    EXPECT(xs::command_list_proxy_table_size() == 160);

    // 3. Inline cursor pinning: +0x18, +0x28, +0x30
    const auto base = reinterpret_cast<std::uintptr_t>(list_proxy);
    std::uint64_t c18 = 0, c28 = 0, c30 = 0;
    std::memcpy(&c18, reinterpret_cast<const void*>(base + 0x18), sizeof(c18));
    std::memcpy(&c28, reinterpret_cast<const void*>(base + 0x28), sizeof(c28));
    std::memcpy(&c30, reinterpret_cast<const void*>(base + 0x30), sizeof(c30));
    const std::uint64_t expected_limit = static_cast<std::uint64_t>(base + 0x18 + 0x500);
    EXPECT(c18 == expected_limit);
    EXPECT(c28 == expected_limit);
    EXPECT(c30 == expected_limit);

    // 4. Console slot routing on ListProxy
    Mock* real_list = reinterpret_cast<Mock*>(xs::command_list_proxy_real_list(list_proxy));
    EXPECT(real_list != nullptr && real_list->kind == Kind::command_list);

    // Console slot 32 (SetDescriptorHeaps) -> PC slot 28
    // Pass descriptor heap proxy to ensure UnwrappedArray unwrap_object works
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    void* heap_proxy = nullptr;
    const IID heap_iid = __uuidof(ID3D12DescriptorHeap);
    EXPECT(hr_of(raw(device, 14, word(&heap_desc), word(&heap_iid), word(&heap_proxy))) == S_OK);
    EXPECT(heap_proxy != nullptr && xs::is_heap_proxy(heap_proxy));
    ID3D12DescriptorHeap* real_heap = xs::heap_proxy_real_heap(heap_proxy);
    void* heap_ptrs[2] = {heap_proxy, real_heap};
    raw(list_proxy, 32, 2, word(heap_ptrs), 0, 0);
    EXPECT(real_list->calls[28] == 1);
    EXPECT(real_list->args[28][0] == 2);
    // The heap proxy should have been unwrapped in the array passed to PC slot 28:
    EXPECT(real_list->array[28][0] == word(real_heap));
    EXPECT(real_list->array[28][1] == word(real_heap));
    EXPECT(xs::list_slot_hits(32) == 1);
    EXPECT(xs::list_slot_first_call_seen(32));

    // Console slot 33 (SetComputeRootSignature) -> PC slot 29
    raw(list_proxy, 33, 0x5555, 0, 0, 0);
    EXPECT(real_list->calls[29] == 1);
    EXPECT(real_list->args[29][0] == 0x5555);
    EXPECT(xs::list_slot_hits(33) == 1);
    EXPECT(xs::list_slot_first_call_seen(33));

    // Console slot 60: no source names it, so it reaches nothing (the old map sent it to PC EndEvent, 58)
    EXPECT(raw(list_proxy, 60, 0, 0, 0, 0) == 0);
    EXPECT(real_list->calls[58] == 0);

    // Console slot 61 (ExecuteIndirect) -> PC slot 59
    raw(list_proxy, 61, 0x10, 0x20, 0x30, 0x40);
    EXPECT(real_list->calls[59] == 1);
    EXPECT(real_list->args[59][0] == 0x10);

    // Console slot 148 (AllocateUploadMemory stub) -> returns 0 without faulting
    EXPECT(raw(list_proxy, 148, 2048, 512, 0, 0) == 0);
    EXPECT(xs::list_slot_hits(148) == 1);
    EXPECT(xs::list_slot_first_call_seen(148));

    // Console slot 146 (Barrier) on base 60-slot list returns E_NOTIMPL (PC slot 80 >= 60)
    EXPECT(hr_of(raw(list_proxy, 146, 0, 0, 0, 0)) == E_NOTIMPL);

    // Console slot 10 (Reset) unwraps allocator proxy and re-pins inline cursor
    std::uint64_t zero = 0;
    std::memcpy(reinterpret_cast<void*>(base + 0x18), &zero, sizeof(zero));
    EXPECT(hr_of(raw(list_proxy, 10, word(allocator), 0, 0, 0)) == S_OK);
    EXPECT(real_list->calls[10] == 1);
    EXPECT(real_list->args[10][0] == word(real_allocator));
    std::memcpy(&c18, reinterpret_cast<const void*>(base + 0x18), sizeof(c18));
    EXPECT(c18 == expected_limit);

    // Console slot 7 (GetDevice) returns device proxy
    void* list_dev = nullptr;
    const IID dev_iid = __uuidof(ID3D12Device);
    const auto dev_refs_before = xs::device_proxy_ref_count(device);
    EXPECT(hr_of(raw(list_proxy, 7, word(&dev_iid), word(&list_dev))) == S_OK);
    EXPECT(list_dev == device);
    EXPECT(xs::device_proxy_ref_count(device) == dev_refs_before + 1);
    EXPECT(release(list_dev) == dev_refs_before);

    // Console slot 0 (QueryInterface)
    void* list_qi = nullptr;
    EXPECT(hr_of(raw(list_proxy, 0, word(&list_iid), word(&list_qi))) == S_OK);
    EXPECT(list_qi == list_proxy);
    EXPECT(release(list_qi) == 1);

    // 5. Queue slot 10 (ExecuteCommandLists) unwraps ListProxy in array
    void* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    const IID queue_iid = __uuidof(ID3D12CommandQueue);
    EXPECT(hr_of(raw(device, 8, word(&qdesc), word(&queue_iid), word(&queue))) == S_OK);
    EXPECT(xs::is_queue_proxy(queue));
    Mock* real_queue = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));

    Mock* unwrapped_list = new_mock(Kind::command_list);
    void* lists_to_exec[2] = {unwrapped_list, list_proxy};
    raw(queue, 10, 2, word(lists_to_exec), 0, 0);
    EXPECT(real_queue->calls[10] == 1);
    EXPECT(real_queue->args[10][0] == 2);
    EXPECT(real_queue->array[10][0] == word(unwrapped_list));
    EXPECT(real_queue->array[10][1] == word(real_list));

    // 6. Test with list_level = 7 (supporting ID3D12GraphicsCommandList7, 81 slots)
    mock_device->list_level = 7;
    void* list7_proxy = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), word(&list7_proxy))) == S_OK);
    EXPECT(xs::is_command_list_proxy(list7_proxy));
    Mock* real_list7 = reinterpret_cast<Mock*>(xs::command_list_proxy_real_list(list7_proxy));
    EXPECT(real_list7 != nullptr);
    // Console slot 146 -> PC slot 80 (since 80 < 81 slots)
    raw(list7_proxy, 146, 0x9999, 0, 0, 0);
    EXPECT(real_list7->calls[80] == 1);
    EXPECT(real_list7->args[80][0] == 0x9999);
    EXPECT(release(list7_proxy) == 0 && real_list7->refs == 0);

    // Clean up
    EXPECT(release(list_proxy) == 0 && real_list->refs == 0);
    EXPECT(release(heap_proxy) == 0);
    EXPECT(release(allocator) == 0 && real_allocator->refs == 0);
    EXPECT(release(queue) == 0 && real_queue->refs == 0);
    EXPECT(release(device) == 0 && mock_device->refs == 0);

    // 7. Verify stats reset
    EXPECT(xs::list_slot_hits(32) > 0);
    xs::reset_device_proxy_stats();
    EXPECT(xs::list_slot_hits(32) == 0);
    EXPECT(!xs::list_slot_first_call_seen(32));

    std::printf("%s: console command list routing, cursor pinning, queue unwrapping, and slot mapping passed\n", g_label);
    return 0;
}

// ---- 11. The slot tables as data (d3d12_xs_slots.h), every slot ----

namespace slots = rt::d3d12_xs::slots;

// The console's own ID3D12GraphicsCommandList5/6 (V list.rs:33-88); the title asks for List6.
constexpr GUID kConsoleList5Iid{0x55050859, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
constexpr GUID kConsoleList6Iid{0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};

int check_slot_tables() {
    using slots::DeviceMethod;
    using slots::ListMethod;
    using slots::QueueMethod;

    // Device (V device.rs:402-458; L retail_boot_chain §3): the named slots, in both layouts.
    struct DeviceCase {
        unsigned slot;
        DeviceMethod pc, console;
    } const device_cases[] = {
        {8, DeviceMethod::create_queue, DeviceMethod::create_queue},
        {9, DeviceMethod::create_allocator, DeviceMethod::create_allocator},
        {12, DeviceMethod::create_list, DeviceMethod::create_list},
        {14, DeviceMethod::create_heap, DeviceMethod::create_heap},
        {25, DeviceMethod::struct_return, DeviceMethod::struct_return},
        {26, DeviceMethod::struct_return, DeviceMethod::struct_return},
        {31, DeviceMethod::unwrap, DeviceMethod::unwrap},
        {32, DeviceMethod::open_shared_handle, DeviceMethod::open_shared_handle},
        {36, DeviceMethod::create_fence, DeviceMethod::create_fence},
        {45, DeviceMethod::unwrap, DeviceMethod::refuse},
        {46, DeviceMethod::unwrap, DeviceMethod::refuse},
        {48, DeviceMethod::routed, DeviceMethod::routed},
        {50, DeviceMethod::unwrap, DeviceMethod::refuse},
        {57, DeviceMethod::forward, DeviceMethod::unknown_pipeline},
        {61, DeviceMethod::forward, DeviceMethod::open},
        {62, DeviceMethod::compute_pso_x, DeviceMethod::compute_pso_x},
        {67, DeviceMethod::native_compute_pso, DeviceMethod::native_compute_pso},
        {69, DeviceMethod::forward, DeviceMethod::ignore},
        {70, DeviceMethod::forward, DeviceMethod::open},
        {71, DeviceMethod::forward, DeviceMethod::open},
        {73, DeviceMethod::routed, DeviceMethod::routed},
        {74, DeviceMethod::forward, DeviceMethod::register_page_pool},
        {75, DeviceMethod::create_queue1, DeviceMethod::unregister_page_pool},
        {76, DeviceMethod::committed_resource_x, DeviceMethod::committed_resource_x},
        {78, DeviceMethod::routed, DeviceMethod::routed},
        {79, DeviceMethod::routed, DeviceMethod::routed},
        {80, DeviceMethod::routed, DeviceMethod::routed},
        {81, DeviceMethod::routed, DeviceMethod::routed},
        {86, DeviceMethod::forward, DeviceMethod::open},
        {108, DeviceMethod::struct_return, DeviceMethod::struct_return},
        {127, DeviceMethod::committed_resource3, DeviceMethod::committed_resource3},
        {133, DeviceMethod::placed_resource_x, DeviceMethod::placed_resource_x},
    };
    for (const DeviceCase& c : device_cases) {
        const slots::DeviceSlot s = slots::device_slot(c.slot);
        if (s.pc != c.pc || s.console != c.console || !s.name) {
            std::fprintf(stderr, "%s: device slot %u is not as the table says\n", g_label, c.slot);
            return 1;
        }
    }
    // Every other slot: PC order in the PC layout; in the console layout PC order below 44, refused from 44 on.
    for (unsigned slot = 3; slot < slots::kDeviceSlots; ++slot) {
        const slots::DeviceSlot s = slots::device_slot(slot);
        if (s.pc == DeviceMethod::forward && s.console == DeviceMethod::forward) EXPECT(slot < 44);
        if (!s.name) EXPECT(s.pc == DeviceMethod::forward &&
                            s.console == (slot < 44 ? DeviceMethod::forward : DeviceMethod::refuse));
    }

    // Queue, console layout (V queue.rs:957-987: the older slots two later from 14): every slot of the table.
    struct QueueWant {
        QueueMethod method;
        unsigned pc;
    } queue_want[slots::kQueueSlots];
    for (QueueWant& w : queue_want) w = {QueueMethod::refuse, 0};
    for (unsigned s = 3; s <= 6; ++s) queue_want[s] = {QueueMethod::forward, s};
    queue_want[10] = {QueueMethod::execute, 10};
    queue_want[11] = queue_want[12] = queue_want[13] = {QueueMethod::marker, 0};
    queue_want[16] = {QueueMethod::signal, 14};
    queue_want[17] = {QueueMethod::wait, 15};
    queue_want[18] = {QueueMethod::forward, 16};  // GetTimestampFrequency
    queue_want[23] = queue_want[24] = {QueueMethod::open, 0};  // SuspendX, ResumeX
    queue_want[28] = {QueueMethod::get_desc, 18};
    queue_want[31] = {QueueMethod::open, 0};  // CopyPageMappingsBatchX
    queue_want[32] = {QueueMethod::present, 0};
    queue_want[34] = {QueueMethod::signal, 14};   // Signal2X
    queue_want[35] = {QueueMethod::execute, 10};  // ExecuteX
    for (unsigned slot = 3; slot < slots::kQueueSlots; ++slot) {
        if (slot == 7) continue;  // GetDevice, the proxy's own
        const slots::QueueSlot s = slots::console_queue_slot(slot);
        if (s.method != queue_want[slot].method || (queue_want[slot].pc && s.pc != queue_want[slot].pc)) {
            std::fprintf(stderr, "%s: console queue slot %u is not as the table says\n", g_label, slot);
            return 1;
        }
    }

    // List, console layout (V list.rs:2059-2149; L §3.3-3.5a): every slot of the table.
    struct ListWant {
        ListMethod method;
        unsigned pc;
    } list_want[slots::kListSlots];
    for (ListWant& w : list_want) w = {ListMethod::unknown, 0};
    for (unsigned s = 3; s <= 26; ++s) list_want[s] = {ListMethod::forward, s};  // PC order up to ResourceBarrier
    list_want[10] = {ListMethod::reset, 10};
    list_want[28] = {ListMethod::forward, 52};  // BeginQuery
    list_want[29] = {ListMethod::forward, 53};  // EndQuery
    list_want[30] = {ListMethod::forward, 54};  // ResolveQueryData
    for (unsigned s = 32; s <= 55; ++s) list_want[s] = {ListMethod::forward, s - 4};  // PC 28-51 four later
    list_want[32] = {ListMethod::heaps, 28};
    list_want[51] = {ListMethod::typed, 47};  // ClearDepthStencilView (a float)
    list_want[59] = {ListMethod::ignore, 0};  // the console GPU-event marker
    list_want[61] = {ListMethod::forward, 59};  // ExecuteIndirect
    list_want[62] = {ListMethod::open, 0};      // CopyMemoryToMemoryX
    list_want[70] = {ListMethod::typed, 62};    // OMSetDepthBounds (two floats)
    for (unsigned s = 71; s <= 74; ++s) list_want[s] = {ListMethod::open, 0};  // Write32/64, WaitValue32/64
    list_want[75] = {ListMethod::ignore, 0};
    list_want[80] = {ListMethod::forward, 14};  // DispatchX
    list_want[85] = {ListMethod::forward, 16};  // CopyTextureRegionX
    list_want[86] = {ListMethod::forward, 17};  // CopyResourceX
    list_want[100] = {ListMethod::open, 0};     // DispatchMeshX
    list_want[101] = {ListMethod::ignore, 0};   // DriverHintX
    list_want[126] = {ListMethod::ignore, 0};
    list_want[129] = {ListMethod::forward, 79};  // DispatchMesh
    list_want[131] = list_want[132] = {ListMethod::ignore, 0};  // pipeline syncs
    list_want[139] = {ListMethod::reset, 10};    // ResetX
    list_want[141] = {ListMethod::open, 0};
    list_want[142] = {ListMethod::ignore, 0};
    list_want[146] = {ListMethod::forward, 80};  // Barrier
    list_want[148] = {ListMethod::open, 0};      // AllocateUploadMemory
    for (unsigned slot = 3; slot < slots::kListSlots; ++slot) {
        if (slot == 7) continue;  // GetDevice, the proxy's own
        const slots::ListSlot s = slots::console_list_slot(slot);
        if (s.method != list_want[slot].method || (list_want[slot].pc && s.pc != list_want[slot].pc)) {
            std::fprintf(stderr, "%s: console list slot %u is not as the table says\n", g_label, slot);
            return 1;
        }
        const bool reaches = s.method == ListMethod::forward || s.method == ListMethod::reset ||
                             s.method == ListMethod::heaps || s.method == ListMethod::typed;
        EXPECT(xs::console_to_pc_list_slot(slot) == (reaches ? s.pc : xs::kListNotForwarded));
    }
    std::printf("%s: slot tables: %zu named device slots, every console queue (128) and list (160) slot as the "
                "VIctory/Leonida sources give it\n", g_label, std::size(slots::kDeviceEntries));
    return 0;
}

// ---- 12. Every console list slot through a list proxy ----

int check_list_table() {
    Mock* mock_device = nullptr;
    void* device = make_console_device(mock_device);
    EXPECT(device && xs::device_proxy_console_layout(device));
    mock_device->list_level = 8;  // GCL8, 82 PC slots: every PC slot the table names exists
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, word(&allocator_iid), word(&allocator))) == S_OK);
    EXPECT(xs::is_allocator_proxy(allocator) && xs::command_object_proxy_console_layout(allocator));
    Mock* const real_allocator = reinterpret_cast<Mock*>(xs::allocator_proxy_real_allocator(allocator));

    // Device slot 12 in the console layout (V device.rs:413, list.rs:566): a console list proxy, as the console's own
    // List6, over a list made as ID3D12GraphicsCommandList with the allocator unwrapped.
    void* list = nullptr;
    EXPECT(hr_of(raw(device, 12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, word(allocator), 0, word(&kConsoleList6Iid),
                     word(&list))) == S_OK);
    EXPECT(xs::is_command_list_proxy(list) && mock_device->calls[12] == 1);
    EXPECT(mock_device->args[12][2] == word(real_allocator) &&
           *reinterpret_cast<const IID*>(mock_device->args[12][4]) == __uuidof(ID3D12GraphicsCommandList));
    Mock* const real = reinterpret_cast<Mock*>(xs::command_list_proxy_real_list(list));
    EXPECT(real && real->kind == Kind::command_list);
    void* same = nullptr;
    EXPECT(hr_of(raw(list, 0, word(&kConsoleList5Iid), word(&same))) == S_OK && same == list && release(same) == 1);
    // Slot 73 hands out the same kind of proxy for the console interface.
    const std::uint32_t list_desc[4] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    void* list73 = nullptr;
    EXPECT(hr_of(raw(device, 73, word(list_desc), word(allocator), 0, word(&kConsoleList6Iid), word(&list73))) == S_OK);
    EXPECT(xs::is_command_list_proxy(list73) && release(list73) == 0);

    // Every slot from 3 on (7 is GetDevice): the one PC method the table names, with the title's words, or nothing.
    // SetDescriptorHeaps reads its array (proxies in it are unwrapped): two plain objects.
    const std::uint64_t heaps[2] = {word(new_mock(Kind::heap)), word(new_mock(Kind::heap))};
    for (unsigned slot = 3; slot < xs::command_list_proxy_table_size(); ++slot) {
        if (slot == 7) continue;
        const slots::ListSlot entry = slots::console_list_slot(slot);
        unsigned before[kMockSlots];
        std::memcpy(before, real->calls, sizeof(before));
        std::uint64_t a1 = 0x1000 + slot, a2 = 0x2000 + slot;
        const std::uint64_t a3 = 0x3000 + slot, a4 = 0x4000 + slot;
        if (entry.method == slots::ListMethod::reset) a1 = word(allocator);
        if (entry.method == slots::ListMethod::heaps) {
            a1 = 2;
            a2 = word(heaps);
        }
        const std::uint64_t result = raw(list, slot, a1, a2, a3, a4);
        unsigned changed = 0, at = 0;
        for (unsigned s = 0; s < kMockSlots; ++s)
            if (real->calls[s] != before[s]) {
                ++changed;
                at = s;
            }
        switch (entry.method) {
            case slots::ListMethod::forward:
                EXPECT(changed == 1 && at == entry.pc && real->args[at][0] == a1 && real->args[at][1] == a2 &&
                       real->args[at][2] == a3 && real->args[at][3] == a4);
                break;
            case slots::ListMethod::reset:  // Reset/ResetX: the real allocator, the cursor pinned again
                EXPECT(changed == 1 && at == 10 && real->args[10][0] == word(real_allocator) &&
                       real->args[10][1] == a2);
                break;
            case slots::ListMethod::heaps:
                EXPECT(changed == 1 && at == 28 && real->args[28][0] == 2 && real->array[28][0] == heaps[0] &&
                       real->array[28][1] == heaps[1]);
                break;
            case slots::ListMethod::typed:
                EXPECT(changed == 1 && at == entry.pc);
                break;
            default:  // ignore, open, unknown: nothing reaches the real list, and the answer is 0
                if (changed != 0 || result != 0) {
                    std::fprintf(stderr, "%s: console list slot %u reached PC slot %u\n", g_label, slot, at);
                    return 1;
                }
        }
    }
    // The float methods with their real prototypes: the floats arrive (they travel in XMM registers).
    using ClearDsvFn = void(WINAPI*)(void*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT,
                                     const D3D12_RECT*);
    using BoundsFn = void(WINAPI*)(void*, FLOAT, FLOAT);
    void* const* vtable = *reinterpret_cast<void* const* const*>(list);
    const D3D12_RECT rect{1, 2, 3, 4};
    reinterpret_cast<ClearDsvFn>(vtable[51])(list, D3D12_CPU_DESCRIPTOR_HANDLE{0x77}, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 3,
                                             1, &rect);
    EXPECT(real->floats[0] == 0.25f && real->args[47][0] == 0x77 && real->args[47][1] == D3D12_CLEAR_FLAG_DEPTH &&
           real->args[47][2] == 3 && real->args[47][3] == 1 && real->args[47][4] == word(&rect));
    reinterpret_cast<BoundsFn>(vtable[70])(list, 0.125f, 0.75f);
    EXPECT(real->floats[0] == 0.125f && real->floats[1] == 0.75f);

    EXPECT(release(list) == 0 && real->refs == 0);
    EXPECT(release(allocator) == 0 && real_allocator->refs == 0);
    EXPECT(release(device) == 0 && mock_device->refs == 0);
    std::printf("%s: every console list slot reaches the PC method its table names, with the title's words (floats "
                "too), or nothing; slot 12 makes console lists in the console layout\n", g_label);
    return 0;
}

// ---- 13. The console queue ----

int check_console_queue() {
    Mock* mock = nullptr;
    void* device = make_console_device(mock);
    EXPECT(device && !xs::device_proxy_console_layout(nullptr));
    void* queue = make_queue(device);  // slot 8
    void* fence = make_fence(device);
    EXPECT(queue && fence && xs::command_object_proxy_console_layout(queue) &&
           xs::command_object_proxy_console_layout(fence));
    Mock* const real_queue = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    Mock* const real_fence = reinterpret_cast<Mock*>(xs::fence_proxy_real_fence(fence));
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
    const std::uint32_t list_desc[4] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
    void* list = nullptr;
    EXPECT(hr_of(raw(device, 73, word(list_desc), word(allocator), 0, word(&list_iid), word(&list))) == S_OK);
    Mock* const real_list = reinterpret_cast<Mock*>(xs::command_list_proxy_real_list(list));
    Mock* const plain = new_mock(Kind::command_list);
    const std::uint64_t lists[2] = {word(list), word(plain)};

    // Every slot from 3 on (7 is GetDevice): what it reaches on the real queue, which has the PC order.
    for (unsigned slot = 3; slot < xs::queue_proxy_table_size(); ++slot) {
        if (slot == 7) continue;
        const slots::QueueSlot entry = slots::console_queue_slot(slot);
        unsigned before[kMockSlots];
        std::memcpy(before, real_queue->calls, sizeof(before));
        std::uint64_t a1 = 0x1000 + slot, a2 = 0x2000 + slot;
        const std::uint64_t a3 = 0x3000 + slot, a4 = 0x4000 + slot;
        D3D12_COMMAND_QUEUE_DESC desc{};
        if (entry.method == slots::QueueMethod::execute) {
            a1 = 2;
            a2 = word(lists);
        } else if (entry.method == slots::QueueMethod::signal || entry.method == slots::QueueMethod::wait) {
            a1 = word(fence);
            a2 = 40 + slot;
        } else if (entry.method == slots::QueueMethod::get_desc) {
            a1 = word(&desc);
        }
        const std::uint64_t result = raw(queue, slot, a1, a2, a3, a4);
        unsigned changed = 0, at = 0;
        for (unsigned s = 0; s < kMockSlots; ++s)
            if (real_queue->calls[s] != before[s]) {
                ++changed;
                at = s;
            }
        bool ok = false;
        switch (entry.method) {
            case slots::QueueMethod::forward:
                ok = changed == 1 && at == entry.pc && real_queue->args[at][0] == a1 && real_queue->args[at][1] == a2;
                break;
            case slots::QueueMethod::execute:  // ExecuteCommandLists, ExecuteX: the list proxy unwrapped
                ok = changed == 1 && at == 10 && real_queue->args[10][0] == 2 &&
                     real_queue->array[10][0] == word(real_list) && real_queue->array[10][1] == word(plain);
                break;
            case slots::QueueMethod::signal:  // Signal, Signal2X: the real fence
                ok = changed == 1 && at == 14 && hr_of(result) == S_OK && real_queue->args[14][0] == word(real_fence) &&
                     real_queue->args[14][1] == a2;
                break;
            case slots::QueueMethod::wait:
                ok = changed == 1 && at == 15 && real_queue->args[15][0] == word(real_fence) &&
                     real_queue->args[15][1] == a2;
                break;
            case slots::QueueMethod::get_desc:
                ok = changed == 1 && at == 18 && result == word(&desc) && desc.Type == kMockQueueDesc.Type &&
                     desc.NodeMask == kMockQueueDesc.NodeMask;
                break;
            case slots::QueueMethod::present:  // no present handler: accepted, nothing reached
                ok = changed == 0 && hr_of(result) == S_OK;
                break;
            case slots::QueueMethod::marker:
                ok = changed == 0 && result == 0;
                break;
            default:  // refuse, open
                ok = changed == 0 && hr_of(result) == E_NOTIMPL;
                break;
        }
        if (!ok) {
            std::fprintf(stderr, "%s: console queue slot %u: %u PC calls (last %u), result 0x%llx\n", g_label, slot,
                         changed, at, static_cast<unsigned long long>(result));
            return 1;
        }
    }
    EXPECT(real_fence->calls[9] >= 2);  // each Signal asked for the fence's completion (its value word)

    // GetDesc with the return buffer first and the queue second, as the device's struct returns are called.
    D3D12_COMMAND_QUEUE_DESC swapped{};
    const unsigned desc_calls = real_queue->calls[18];
    using GetDescFn = std::uint64_t(WINAPI*)(void*, void*);
    EXPECT(reinterpret_cast<GetDescFn>((*reinterpret_cast<void* const* const*>(queue))[28])(&swapped, queue) ==
           word(&swapped));
    EXPECT(swapped.Type == kMockQueueDesc.Type && real_queue->calls[18] == desc_calls + 1);

    // PresentX (32) through the device's present handler, which gets the real queue and the title's words.
    static void* seen_context = nullptr;
    static ID3D12CommandQueue* seen_queue = nullptr;
    static std::uint32_t seen_planes = 0;
    static const void* seen_plane_parameters = nullptr;
    static const void* seen_parameters = nullptr;
    const xs::PresentHandler handler = [](void* context, ID3D12CommandQueue* q, std::uint32_t planes,
                                          const void* plane_parameters, const void* parameters) -> HRESULT {
        seen_context = context;
        seen_queue = q;
        seen_planes = planes;
        seen_plane_parameters = plane_parameters;
        seen_parameters = parameters;
        return DXGI_STATUS_OCCLUDED;
    };
    int context = 0;
    EXPECT(xs::set_present_handler(device, handler, &context));
    EXPECT(hr_of(raw(queue, 32, 1, 0x5000, 0x6000)) == DXGI_STATUS_OCCLUDED && seen_context == &context);
    EXPECT(seen_queue == reinterpret_cast<ID3D12CommandQueue*>(real_queue) && seen_planes == 1 &&
           seen_plane_parameters == reinterpret_cast<const void*>(0x5000) &&
           seen_parameters == reinterpret_cast<const void*>(0x6000));
    EXPECT(xs::set_present_handler(device, nullptr, nullptr) && hr_of(raw(queue, 32, 0, 0, 0)) == S_OK);
    EXPECT(!xs::set_present_handler(queue, handler, nullptr));  // not a device

    // A queue of the PC layout keeps the PC order: 16 is GetTimestampFrequency, 14 Signal.
    Mock* pc_mock = nullptr;
    void* pc_device = make_device(pc_mock, 11, true);
    void* pc_queue = make_queue(pc_device);
    Mock* const pc_real = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(pc_queue));
    EXPECT(!xs::command_object_proxy_console_layout(pc_queue));
    EXPECT(hr_of(raw(pc_queue, 16, 0x99)) == S_OK && pc_real->calls[16] == 1 && pc_real->calls[14] == 0);
    EXPECT(hr_of(raw(pc_queue, 32)) == E_NOTIMPL);
    EXPECT(release(pc_queue) == 0 && release(pc_device) == 0 && pc_mock->refs == 0);

    EXPECT(release(list) == 0 && release(allocator) == 0);
    EXPECT(release(queue) == 0 && release(fence) == 0 && real_queue->refs == 0 && real_fence->refs == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: console queue: Signal 16, Wait 17, timestamp frequency 18, GetDesc 28 (either order), PresentX 32 "
                "through the present handler, Signal2X 34, ExecuteX 35, markers ignored, every other slot refused\n",
                g_label);
    return 0;
}

// ---- 14. The console fence's value word (slot 12) ----

// The word, read until it shows `value` or 5 s have passed: the proxy's thread-pool wait sets it.
bool word_reaches(const volatile std::uint64_t* word12, std::uint64_t value) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (*word12 != value) {
        if (std::chrono::steady_clock::now() > until) return false;
        Sleep(1);
    }
    return true;
}

int check_console_fence() {
    Mock* mock = nullptr;
    void* device = make_console_device(mock);
    void* queue = make_queue(device);
    void* fence = make_fence(device);
    EXPECT(queue && fence);
    Mock* const real_queue = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    Mock* const real_fence = reinterpret_cast<Mock*>(xs::fence_proxy_real_fence(fence));

    // Slot 12 (V fence.rs:136-139): the address of a 64-bit word at the fence's completed value; the same each time.
    const auto* word12 = reinterpret_cast<const volatile std::uint64_t*>(raw(fence, 12));
    EXPECT(word12 && *word12 == kCompletedValue && raw(fence, 12) == word(word12));
    // A CPU Signal (10) moves it at once; GetCompletedValue (8) is the real fence's.
    EXPECT(hr_of(raw(fence, 10, 9)) == S_OK && *word12 == 9 && raw(fence, 8) == 9);
    // A console queue's Signal (16) the GPU has not reached leaves it; when the GPU gets there it follows with no
    // call from the title: the proxy waits on the fence's completion.
    real_queue->defer_signals = true;
    EXPECT(hr_of(raw(queue, 16, word(fence), 11)) == S_OK && *word12 == 9 && real_fence->event_count == 1);
    complete_signals(real_queue);
    EXPECT(word_reaches(word12, 11));
    // Signal2X (34) the same way; a CPU Signal to a lower value is followed as well.
    EXPECT(hr_of(raw(queue, 34, word(fence), 13, 0, 0)) == S_OK && *word12 == 11);
    complete_signals(real_queue);
    EXPECT(word_reaches(word12, 13));
    EXPECT(hr_of(raw(fence, 10, 5)) == S_OK && *word12 == 5);
    // A fence whose proxy is gone before its completion: the event comes later and finds only the listed fences.
    void* fence2 = make_fence(device);
    Mock* const real_fence2 = reinterpret_cast<Mock*>(xs::fence_proxy_real_fence(fence2));
    EXPECT(hr_of(raw(queue, 16, word(fence2), 20)) == S_OK && real_fence2->event_count == 1);
    EXPECT(release(fence2) == 0 && real_fence2->refs == 0);
    complete_signals(real_queue);
    EXPECT(mock_fence_value(real_fence2) == 20);
    EXPECT(hr_of(raw(fence, 10, 6)) == S_OK && *word12 == 6);
    real_queue->defer_signals = false;

    // A fence of a device in the PC layout has no slot 12 (the PC chain ends at 11).
    Mock* pc_mock = nullptr;
    void* pc_device = make_device(pc_mock, 11, true);
    void* pc_fence = make_fence(pc_device);
    EXPECT(pc_fence && !xs::command_object_proxy_console_layout(pc_fence));
    EXPECT(hr_of(raw(pc_fence, 12)) == E_NOTIMPL && raw(pc_fence, 8) == kCompletedValue);
    EXPECT(release(pc_fence) == 0 && release(pc_device) == 0 && pc_mock->refs == 0);

    EXPECT(release(queue) == 0 && release(fence) == 0 && real_fence->refs == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: console fence slot 12: a value word that follows CPU signals at once and queue signals when the "
                "fence completes\n", g_label);
    return 0;
}

// ---- 15. The console device ----

int check_console_device() {
    Mock* mock = nullptr;
    void* device = make_console_device(mock);
    EXPECT(device && xs::device_proxy_console_layout(device));
    {  // only ID3D12Device11 selects the console layouts (V device.rs:153-155)
        Mock* other = nullptr;
        void* device10 = make_device(other, 11, true, false, __uuidof(ID3D12Device10));
        EXPECT(device10 && !xs::device_proxy_console_layout(device10) && release(device10) == 0);
    }
    void* const* vtable = *reinterpret_cast<void* const* const*>(device);

    // 26 GetCustomHeapProperties: the return buffer first and `this` second (V resource.rs:2583-2593; L
    // retail_boot_chain §3.1), or in PC order; the buffer comes back in RAX either way.
    using HeapPropertiesFn = std::uint64_t(WINAPI*)(void*, void*, UINT, UINT);
    D3D12_HEAP_PROPERTIES properties{};
    EXPECT(reinterpret_cast<HeapPropertiesFn>(vtable[26])(&properties, device, 1, D3D12_HEAP_TYPE_UPLOAD) ==
           word(&properties));
    EXPECT(properties.Type == kMockHeapProperties.Type && properties.MemoryPoolPreference == D3D12_MEMORY_POOL_L0);
    EXPECT(mock->calls[26] == 1 && mock->args[26][1] == 1 && mock->args[26][2] == D3D12_HEAP_TYPE_UPLOAD);
    D3D12_HEAP_PROPERTIES pc_order{};
    EXPECT(reinterpret_cast<HeapPropertiesFn>(vtable[26])(device, &pc_order, 0, D3D12_HEAP_TYPE_DEFAULT) ==
           word(&pc_order));
    EXPECT(pc_order.Type == kMockHeapProperties.Type && mock->calls[26] == 2 &&
           mock->args[26][2] == D3D12_HEAP_TYPE_DEFAULT);

    // 25 GetResourceAllocationInfo (V resource.rs:802-811): the same order, and the console description's layout
    // word and Xbox flag bits do not reach the PC method.
    D3D12_RESOURCE_DESC tiled{};
    tiled.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tiled.Width = 256;
    tiled.Height = 128;
    tiled.DepthOrArraySize = tiled.MipLevels = 1;
    tiled.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tiled.SampleDesc.Count = 1;
    tiled.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);
    tiled.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x20004);
    using AllocationInfoFn = std::uint64_t(WINAPI*)(void*, void*, UINT, UINT, const void*);
    D3D12_RESOURCE_ALLOCATION_INFO info{};
    EXPECT(reinterpret_cast<AllocationInfoFn>(vtable[25])(&info, device, 0, 1, &tiled) == word(&info));
    EXPECT(info.SizeInBytes == kMockAllocation.SizeInBytes && mock->calls[25] == 1);
    EXPECT(mock->resource_desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN &&
           static_cast<std::uint32_t>(mock->resource_desc.Flags) == 0x4 && mock->resource_desc.Width == 256);
    // 108 GetResourceAllocationInfo2 (V resource.rs:840-849): PC slot 68, with D3D12_RESOURCE_DESC1s.
    D3D12_RESOURCE_DESC1 tiled1{};
    std::memcpy(&tiled1, &tiled, sizeof(tiled));
    tiled1.SamplerFeedbackMipRegion = {9, 9, 9};
    using AllocationInfo2Fn = std::uint64_t(WINAPI*)(void*, void*, UINT, UINT, const void*, void*);
    info = {};
    EXPECT(reinterpret_cast<AllocationInfo2Fn>(vtable[108])(&info, device, 0, 1, &tiled1, nullptr) == word(&info));
    EXPECT(info.SizeInBytes == kMockAllocation.SizeInBytes && mock->calls[68] == 1);
    EXPECT(mock->resource_desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN &&
           mock->resource_desc.SamplerFeedbackMipRegion.Width == 0);
    {  // without ID3D12Device8: what D3D12 answers for a description it cannot place, and no call
        Mock* old = nullptr;
        void* device3 = make_device(old, 3, true, false);
        void* const* old_vtable = *reinterpret_cast<void* const* const*>(device3);
        D3D12_RESOURCE_ALLOCATION_INFO none{1, 1};
        EXPECT(reinterpret_cast<AllocationInfo2Fn>(old_vtable[108])(&none, device3, 0, 1, &tiled1, nullptr) ==
               word(&none));
        EXPECT(none.SizeInBytes == ~0ull && old->calls[68] == 0 && release(device3) == 0);
    }

    // 69, the debug error filter: accepted (V device.rs:442), never PC CreateCommittedResource2.
    EXPECT(hr_of(raw(device, 69, 1, 2, 3)) == S_OK && mock->calls[69] == 0);

    // 127 CreateCommittedResource3 (V resource.rs:2163-2185): PC ID3D12Device10::CreateCommittedResource3 (PC slot 76)
    // with a PC description and ALLOW_DISPLAY cleared.
    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC1 back_buffer{};
    back_buffer.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    back_buffer.Width = 1920;
    back_buffer.Height = 1080;
    back_buffer.DepthOrArraySize = back_buffer.MipLevels = 1;
    back_buffer.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    back_buffer.SampleDesc.Count = 1;
    back_buffer.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);
    back_buffer.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x20001);  // ALLOW_RENDER_TARGET and an Xbox bit
    const IID resource_iid = __uuidof(ID3D12Resource);
    void* resource = reinterpret_cast<void*>(1);
    const std::uint64_t display_heap = D3D12_HEAP_FLAG_ALLOW_DISPLAY | D3D12_HEAP_FLAG_SHARED;
    EXPECT(hr_of(raw(device, 127, word(&default_heap), display_heap, word(&back_buffer), 0, 0, 0x7777, 0, 0,
                     word(&resource_iid), word(&resource))) == S_OK);
    EXPECT(mock->calls[76] == 1 && mock->args[76][0] == word(&default_heap) &&
           mock->args[76][1] == D3D12_HEAP_FLAG_SHARED && mock->args[76][3] == 0);
    EXPECT(mock->resource_desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN &&
           static_cast<std::uint32_t>(mock->resource_desc.Flags) == 0x1 && mock->resource_desc.Width == 1920);
    EXPECT(!resource);  // nulled first; the mock makes no object
    EXPECT(hr_of(raw(device, 127, word(&default_heap), 0, 0, 0, 0, 0, 0, 0, word(&resource_iid), word(&resource))) ==
           E_INVALIDARG);
    EXPECT(hr_of(raw(device, 127, word(&default_heap), 0, word(&back_buffer), 0, 0, 0, 0, 0, 0, word(&resource))) ==
           E_POINTER && mock->calls[76] == 1);
    {  // without ID3D12Device10: CreateCommittedResource in the common state, as VIctory makes it
        Mock* m9 = nullptr;
        void* device9 = make_device(m9, 9, true, false);
        EXPECT(hr_of(raw(device9, 127, word(&default_heap), display_heap, word(&back_buffer), 0, 0, 0, 0, 0,
                         word(&resource_iid), word(&resource))) == S_OK);
        EXPECT(m9->calls[76] == 0 && m9->calls[27] == 1 && m9->args[27][1] == D3D12_HEAP_FLAG_SHARED &&
               m9->args[27][3] == D3D12_RESOURCE_STATE_COMMON &&
               m9->resource_desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN);
        EXPECT(release(device9) == 0);
    }

    // 74/75 page pools (V resource.rs:2187-2223).
    std::uint64_t pool = 0, pool2 = 0, address = 0;
    std::uint32_t pages = 0;
    EXPECT(hr_of(raw(device, 74, 0x7f0000000000ull, 16, word(&pool))) == S_OK && pool == 1 && mock->calls[74] == 0);
    EXPECT(xs::device_page_pool(device, pool, &address, &pages) && address == 0x7f0000000000ull && pages == 16);
    EXPECT(hr_of(raw(device, 74, 0x7f0000100000ull, 4, word(&pool2))) == S_OK && pool2 == 2);
    EXPECT(hr_of(raw(device, 74, 0x7f0000001000ull, 4, word(&pool2))) == E_INVALIDARG && pool2 == 2);  // not 64 KiB
    EXPECT(hr_of(raw(device, 74, 0x7f0000100000ull, 0, word(&pool2))) == E_INVALIDARG);                // no pages
    EXPECT(hr_of(raw(device, 74, 0x7f0000100000ull, 4, 0)) == E_INVALIDARG);                           // no handle
    EXPECT(hr_of(raw(device, 75, pool)) == S_OK && !xs::device_page_pool(device, pool, nullptr, nullptr));
    EXPECT(hr_of(raw(device, 75, pool)) == E_INVALIDARG && hr_of(raw(device, 75, 9)) == E_INVALIDARG);
    EXPECT(xs::device_page_pool(device, pool2, nullptr, nullptr) && mock->calls[75] == 0);

    // 57, a console pipeline creation nobody has written: refused with the output nulled (V device.rs:349-400).
    const IID pipeline_iid = __uuidof(ID3D12PipelineState);
    void* pipeline = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 57, 0x1234, word(&pipeline_iid), word(&pipeline))) == E_NOTIMPL && !pipeline &&
           mock->calls[57] == 0);
    // From 44 on the console slots no source names, and the console methods not written yet (61, 70, 71, 86), are
    // refused: never the PC method at the same number (45/46/50 among them, PC ID3D12Device1/3 methods).
    for (unsigned slot = 44; slot < slots::kDeviceSlots; ++slot) {
        const slots::DeviceSlot entry = slots::device_slot(slot);
        if (entry.console != slots::DeviceMethod::refuse && entry.console != slots::DeviceMethod::open) continue;
        const unsigned before = slot < kMockSlots ? mock->calls[slot] : 0;
        EXPECT(hr_of(raw(device, slot, 1, 2, 3, 4)) == E_NOTIMPL);
        if (slot < kMockSlots) EXPECT(mock->calls[slot] == before);
        EXPECT(xs::device_slot_overridden(slot, true));
    }
    // Below 44 the PC order holds: an unnamed slot is the PC method (30 CreateReservedResource, 42 GetResourceTiling).
    EXPECT(hr_of(raw(device, 30)) == S_OK && mock->calls[30] == 1);
    EXPECT(hr_of(raw(device, 42)) == S_OK && mock->calls[42] == 1);
    EXPECT(!xs::device_slot_overridden(30, true) && !xs::device_slot_overridden(69) &&
           xs::device_slot_overridden(69, true) && xs::device_slot_overridden(26) && xs::device_slot_overridden(127) &&
           !xs::device_slot_overridden(44) && xs::device_slot_overridden(44, true));

    // 9: every allocator is wrapped (this one answers no tag), and its console slot 9 is accepted (V
    // list.rs:1378-1387); Reset (8) is forwarded and slot 10 refused.
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK &&
           xs::is_allocator_proxy(allocator));
    Mock* const real_allocator = reinterpret_cast<Mock*>(xs::allocator_proxy_real_allocator(allocator));
    EXPECT(!real_allocator->allocator_tag && xs::command_object_proxy_console_layout(allocator));
    EXPECT(hr_of(raw(allocator, 9, 0x99)) == S_OK && real_allocator->calls[9] == 0);
    EXPECT(hr_of(raw(allocator, 10)) == E_NOTIMPL && real_allocator->calls[10] == 0);
    EXPECT(hr_of(raw(allocator, 8)) == S_OK && real_allocator->calls[8] == 1);
    EXPECT(release(allocator) == 0 && real_allocator->refs == 0);

    // 48 in the console layout, as in the PC one: the console description.
    const std::uint32_t copy_x[4] = {D3D12_COMMAND_LIST_TYPE_COPY, 0, 2, 1};
    void* queue = nullptr;
    EXPECT(hr_of(raw(device, 48, word(copy_x), word(&kQueueIid), word(&queue))) == S_OK);
    EXPECT(xs::command_object_proxy_console_layout(queue) && mock->queue_desc.Type == D3D12_COMMAND_LIST_TYPE_COPY &&
           mock->queue_desc.NodeMask == 0 && mock->queue_desc.Flags == D3D12_COMMAND_QUEUE_FLAG_NONE);
    EXPECT(release(queue) == 0);

    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: console device: struct returns 25/26/108 in either order, 69 ignored, 127 "
                "CreateCommittedResource3, 74/75 page pools, 57 refused with a null output, every unnamed slot from 44 "
                "refused, every allocator wrapped\n", g_label);
    return 0;
}

// ---- 16. The frame clock (device slots 78-81; V frame.rs) ----

int check_frame_clock() {
    Mock* mock = nullptr;
    void* device = make_console_device(mock);
    EXPECT(device);
    // V frame.rs:205-250, on a 2 ms interval.
    EXPECT(hr_of(raw(device, 78, 0, 2000, 1, 0)) == S_OK);
    std::uint64_t first = 0, second = 0;
    EXPECT(hr_of(raw(device, 80, 0, 1000, 0, 0, word(&first))) == S_OK && first == 1);
    const auto before = std::chrono::steady_clock::now();
    EXPECT(hr_of(raw(device, 80, 0, 1000, 0, 0, word(&second))) == S_OK && second == first + 1);
    EXPECT(std::chrono::steady_clock::now() - before <= std::chrono::milliseconds(50));
    // A poll whose tick has not come: S_FALSE and no token; with "token on time-out" (2) the frame begins anyway.
    std::uint64_t untouched = 0, early = 0, next = 0;
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, word(&untouched))) == S_FALSE && untouched == 0);
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 2, word(&early))) == S_FALSE && early == second + 1);
    EXPECT(hr_of(raw(device, 80, 0, 1000, 0, 0, word(&next))) == S_OK && next == early + 1);
    EXPECT(xs::device_frame_last_tick(device, 0) >= 2 && xs::device_frames_begun(device) == next);

    // The title's loop on a 5 ms interval: an origin poll that times out (and begins the frame), then a blocking wait
    // for the flip (kind 2). Each flip takes its own tick.
    EXPECT(hr_of(raw(device, 78, 0, 5000, 3, 0)) == S_OK);
    std::uint64_t flips[4] = {};
    std::uint64_t token = 0;
    for (std::uint64_t& flip : flips) {
        raw(device, 80, 0, 0, 0, 2, word(&token));
        EXPECT(hr_of(raw(device, 80, 2, 0xffffffffu, 0, 0, word(&token))) == S_OK);
        flip = xs::device_frame_last_tick(device, 2);
    }
    for (unsigned i = 1; i < std::size(flips); ++i) EXPECT(flips[i] == flips[i - 1] + 1);
    // Kinds past the flip share its tick (frame.rs:91-97), and only an origin wait makes a token.
    const std::uint64_t frames = xs::device_frames_begun(device);
    EXPECT(hr_of(raw(device, 80, 5, 0xffffffffu, 0, 0, word(&token))) == S_OK);
    EXPECT(xs::device_frame_last_tick(device, 5) == xs::device_frame_last_tick(device, 2) &&
           xs::device_frame_last_tick(device, 2) == flips[3] + 1 && xs::device_frames_begun(device) == frames);
    // 79: every event is at the tick; 81: no statistics are kept (count 0).
    EXPECT(hr_of(raw(device, 79, 0, 100, 0, 0)) == S_OK);
    std::uint32_t count = 3;
    EXPECT(hr_of(raw(device, 81, token, 0xff, word(&count), 0)) == S_OK && count == 0);
    EXPECT(mock->calls[78] == 0 && mock->calls[79] == 0 && mock->calls[80] == 0 && mock->calls[81] == 0);
    // Each device has its own clock: a new one starts at 60 Hz with its first token.
    Mock* other_mock = nullptr;
    void* other = make_console_device(other_mock);
    std::uint64_t other_token = 0;
    EXPECT(hr_of(raw(other, 80, 0, 1000, 0, 0, word(&other_token))) == S_OK && other_token == 1);
    EXPECT(release(other) == 0 && release(device) == 0 && mock->refs == 0);
    std::printf("%s: frame clock: ticks per interval, blocking waits end on them, time-outs answer S_FALSE, tokens "
                "count frames, flips take consecutive ticks\n", g_label);
    return 0;
}

int main(int argc, char** argv) {
    no_dialogs();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2) {
        if (std::strcmp(argv[1], "--env-slots") == 0) return run_env_slots(true);
        if (std::strcmp(argv[1], "--env-slots-invalid") == 0) return run_env_slots(false);
        return run_fault_mode(argv[1]);
    }
    if (argc != 1) return 2;
    if (check_tail(true) || check_levels() || check_fence1() || check_create_failures() || check_open_shared() ||
        check_alloc_failures() || check_allocators() || check_xbox_slots() || check_console_command_list() ||
        check_slot_tables() || check_list_table() || check_console_queue() || check_console_fence() ||
        check_console_device() || check_frame_clock())
        return 1;
    std::printf("xs_proxy_mock_test: PASS (%d allocations refused on purpose)\n", g_failed_news);
    return 0;
}
