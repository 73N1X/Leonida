// Xbox-ABI proxy around a PC ID3D12Device.
//
// Which method each slot of the device, queue, list and fence is comes from the slot tables in d3d12_xs_slots.h,
// taken from VIctory (victory/crates/victory-gpu, which runs the title) and Leonida's own measurements, each entry
// with its source. The layout switch is VIctory's (device.rs:149-168): a device created as ID3D12Device11, as the
// title's is, has the console layouts (queue slots from 14 and list slots from 59 two later, the console's own
// methods past them, and a fence value address at fence slot 12); a device created as any other interface keeps the
// PC order its queues and fences forwarded before, and its slot 12 hands out the PC list, for Leonida's own tests
// that drive the proxies through the PC C++ interfaces. Lists from slot 73, and from slot 12 in the console layout,
// are console list proxies. The comments below describe the slots measured before the tables; the tables supersede
// them where they differ (slot 48 takes the console queue description, slots 78-81 are VIctory's frame clock).
//
// Measured Xbox device deviations used here (vtable indices, x64):
//   slot 62   CreateComputePipelineState(desc, w2, w3, iid, out): two words (0 from XboxBvhBuilder, meaning unknown)
//             before the IID. The PC slot 62 is CreateStateObject.
//   slot 67   CreateComputePipelineState from native code (desc, iid, out): converts the Xbox native-code desc
//             (+0x00 root signature, +0x28 code, +0x30 size) to D3D12_COMPUTE_PIPELINE_STATE_DESC and forwards
//             to PC CreateComputePipelineState (slot 11). The PC slot 67 is CreateProtectedResourceSession1.
//   slot 76   resource creation -> PC CreateCommittedResource (PC slot 27), with the Xbox layout word ignored.
//   slot 48   CreateCommandQueueX(desc, riid, out) -> the same queue proxy as slot 8. The Xbox desc's first fields are
//             D3D12_COMMAND_QUEUE_DESC's (public HLE).
//   slot 73   CreateCommandListX(desc, allocator, initialState, riid, out) -> PC CreateCommandList(0, type, allocator,
//             initialState, riid, out). The type is the first word of the Xbox desc; there is no node-mask argument
//             in the console call (public HLE; the PC six-argument form differs).
//   slots 78-81  SetFrameIntervalX, ScheduleFrameEventX, WaitFrameEventX, GetFrameStatisticsX: VIctory's frame clock
//             (frame.rs), answered per device before the PC vtable is indexed (80 and 81 are outside a PC
//             ID3D12Device11 table, so they are checked before the beyond-table refusal).
// The slots 48/73/78-81 are the public HLE Freminet/ScarlettX's observation, which VIctory's table confirms on the
// title (device.rs:437, 445, 451-454), and can be moved with LEONIDA_XS_QUEUE_X_SLOT, LEONIDA_XS_LIST_X_SLOT,
// LEONIDA_XS_FRAME_INTERVAL_SLOT, LEONIDA_XS_SCHEDULE_EVENT_SLOT, LEONIDA_XS_WAIT_EVENT_SLOT and
// LEONIDA_XS_FRAME_STATISTICS_SLOT (3..127; anything else is logged once and ignored).
// The measured slots 9/10 (GetCPU/GPUDescriptorHandleForHeapStart) are ID3D12DescriptorHeap methods, not device
// methods: the PC device's 9/10 are CreateCommandAllocator/CreateGraphicsPipelineState and are forwarded unchanged.
// On the PC heap those getters use the hidden return pointer in RDX; the Xbox ABI returns the 8-byte handle in
// RAX. Device slot 14 therefore wraps the created heap in a heap proxy that performs that conversion. The heap
// proxy holds a reference to the device proxy, and its ID3D12DeviceChild::GetDevice returns that same proxy.
// The proxy's heap slots 9/10 expose only the Xbox shape: a PC C++ caller using the PC header's return convention
// would not receive the value, while GetDesc and the other heap slots forward the PC convention unchanged. A
// direct one-argument call against a real PC heap is invalid, so the device test exercises the conversion on the
// wrapper and compares it with a hidden-pointer call against the real heap.
//
// Every other PC device slot is forwarded by a generated thunk that swaps `this` to the PC device. The proxy's
// vtable therefore follows the PC device order, with slots 62 and 76 and the public HLE's 48/73/78/79/80 deliberately
// shadowed. The full Xbox vtable order is not known from the spec; slots beyond the PC device's own vtable return
// E_NOTIMPL and log once.
//
// Command queues and fences (device slots 8, 75, 36, and 32 for a shared fence) come back wrapped in queue and fence
// proxies of the same kind: the public chain (19 queue slots, 11 or 12 fence slots in SDK 10.0.26100) is forwarded
// in PC order, and a slot past it, up to the end of the proxy's table (128 queue, 64 fence slots), is logged with its
// first four argument words and refused with E_NOTIMPL. A slot past the table faults on the vtable read inside a
// reserved, never-committed region, and the fault is logged with the slot. That is where the console's queue methods
// (PresentX, SuspendX, ResumeX) will show up; their slots are not known and are not guessed, and that the console
// keeps the public slots is an assumption (docs/d3d12_command_objects.md §5.2). Heap, queue and fence proxies are
// unwrapped wherever a device or queue method hands them to the real runtime: queue Signal/Wait (14/15), device
// CreateSharedHandle (31), MakeResident/Evict (34/35), SetEventOnMultipleFenceCompletion (45), SetResidencyPriority
// (46) and EnqueueMakeResident (50). List proxies are unwrapped by ExecuteCommandLists, and unwrap the heap proxies
// given to their SetDescriptorHeaps and the allocators given to Reset. QueryInterface answers only the modelled
// interfaces; one the real object has but the proxy does not model is logged once and refused.
#pragma once

#include "d3d12_xs_slots.h"

#include <d3d12.h>

#include <cstddef>
#include <cstdint>

namespace rt::d3d12_xs {

// Wraps `real` and returns the requested device interface through `out`. Takes ownership of one reference to
// `real` on success; on failure the caller keeps its reference. Returns E_NOINTERFACE if `riid` is not a D3D12
// device interface the PC device implements.
HRESULT create_device_proxy(ID3D12Device* real, REFIID riid, void** out);

// True when `device` points at a proxy created by create_device_proxy. Used by the tests.
bool is_device_proxy(const void* device);

// IDXGIDevice1 on the device. A GDKX title asks its device for IDXGIDevice1, then GetAdapter and EnumOutputs(0),
// to find the output it gives SetFrameIntervalX (docs/d3d12_vulkan.md section 4.5). The proxy answers IDXGIObject,
// IDXGIDevice and IDXGIDevice1 itself, as one COM object with the device (same reference count, QueryInterface
// both ways). GetAdapter and GetParent return the adapter of the device behind the proxy: by default the host DXGI
// adapter with that device's LUID. A provider replaces that: `real` is the device behind the proxy, and it returns
// the adapter as `riid`. rt::d3d12::set_device_outer sets one for the Vulkan-backed device (the layer's own adapter).
//
// A title may ask D3D12XboxCreateDevice (so create_device_proxy) for IDXGIDevice1 directly. What comes back then is
// the proxy's DXGI pointer, not the proxy: is_device_proxy is false for it. Code that takes "the proxy" from a
// creation call goes through device_proxy_primary first.
using DxgiAdapterProvider = HRESULT (*)(ID3D12Device* real, REFIID riid, void** out);
// `device` is the proxy or its DXGI pointer. False, with a line on stderr, when it is neither: nothing is set then.
bool set_device_adapter_provider(void* device, DxgiAdapterProvider provider);
// The device proxy an IDXGIDevice pointer handed out by one belongs to, or null.
void* device_proxy_from_dxgi_device(void* dxgi_device);
// The device proxy itself, given the proxy or its DXGI pointer; null for anything else.
void* device_proxy_primary(void* device);

// True when `heap` is a descriptor-heap proxy created by device slot 14.
bool is_heap_proxy(const void* heap);
ID3D12DescriptorHeap* heap_proxy_real_heap(const void* heap);

// Number of PC vtable slots the proxy forwards (detected from the interfaces the PC device supports).
unsigned device_proxy_slot_count(const void* device);

// The PC device behind a proxy. Null when `device` is not a proxy.
ID3D12Device* device_proxy_real_device(const void* device);

// Device slot 133 (the console's CreatePlacedResourceX) makes a buffer on the title's pages at `va`. The proxy does
// not create it itself: the device behind it does, through this handler, which the Vulkan layer registers
// (rt::d3d12::create_buffer_at_host_address). With none registered, or a PC device behind the proxy, the slot
// answers E_NOTIMPL.
// `layout` is the title's third argument, a D3D12_BARRIER_LAYOUT (0xFFFFFFFF = UNDEFINED), measured on a retail
// boot: 0x6 SHADER_RESOURCE, 0x3 UNORDERED_ACCESS, as PC CreatePlacedResource2 takes it.
using PlacedAtVaHandler = HRESULT (*)(ID3D12Device* real, void* va, const D3D12_RESOURCE_DESC& desc,
                                      std::uint32_t layout, REFIID riid, void** out);
void set_placed_at_va_handler(PlacedAtVaHandler handler);

// Called by slot 133 for a buffer, before the handler, with the buffer's range [va, va + width). gta6rt registers
// rt::xmem::commit_physical_range: xmem commits physical pages on first touch, so a range the title has mapped but
// not written yet is still reserved, and the handler's import refuses reserved pages (it would answer with its
// fallback buffer, which the title's writes never reach). A false return is logged once and the handler is still
// called. With none registered nothing is done.
using PlacedAtVaPagesHook = bool (*)(const void* va, std::size_t bytes);
void set_placed_at_va_pages_hook(PlacedAtVaPagesHook hook);

// Console device slots 70 and 71 (V device.rs:443-444, descriptor.rs:795-818): views of memory the title placed itself,
// `(resource desc, view desc, handle)`, with no result (V's methods return nothing; the proxy answers 0). The device
// behind the proxy writes the view through this handler, which the Vulkan layer registers
// (rt::d3d12::create_placed_view). With none registered the slots stay refused as before.
using PlacedViewHandler = void (*)(ID3D12Device* real, bool uav, const void* resource_desc, const void* view_desc,
                                   std::uint64_t handle);
void set_placed_view_handler(PlacedViewHandler handler);

// Console device slot 61, CreateSamplerX(desc, handle) (V device.rs:439, descriptor.rs:828-832): the console's 27-word
// sampler description, the S# written at `handle`. The Vulkan device sets a handler that writes the S# from the
// description's hardware fields; it returns false for a device that is not its own. Without a handler, or when it
// returns false, the proxy makes the D3D12 sampler VIctory makes for the host (V sampler.rs) with the real device's
// CreateSampler (console_sampler_desc).
using SamplerXHandler = bool (*)(ID3D12Device* real, const void* desc, std::uint64_t handle);
void set_sampler_x_handler(SamplerXHandler handler);
// V sampler.rs:28-96, Fields::host and console: the D3D12 sampler of a console sampler description (27 words).
D3D12_SAMPLER_DESC console_sampler_desc(const std::uint32_t* words);

// Console device slot 86: a pipeline object for a stream the device behind the proxy refused, as VIctory hands one out
// for every stream (rt::d3d12::create_refused_pipeline_state, which the Vulkan layer registers). With none registered
// the refusal reaches the title.
using RefusedPipelineHandler = HRESULT (*)(ID3D12Device* real, REFIID riid, void** out);
void set_refused_pipeline_handler(RefusedPipelineHandler handler);

// Depth textures that also allow unordered access (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | ALLOW_UNORDERED_ACCESS):
// the console allows the pair (the title makes a 2560x1440 R32G8X24_TYPELESS one with flags 0x6), the PC runtime
// refuses it. The device behind the proxy says whether it takes them through this handler, which the Vulkan layer
// registers (rt::d3d12::accepts_depth_unordered: it gives such a texture colour twins for its UAVs). When it answers
// true, slots 76 and 127 and the allocation-info slots 25 and 108 keep the flag, and slots 76 and 127 make their call
// with console_depth_unordered() true on the calling thread, so the device accepts the pair for the title while it
// still refuses it to PC callers. With no handler, or false (a PC device behind the proxy), the UAV flag is dropped
// as before and the texture's UAVs stay null. Slot 133 always drops it (it makes buffers).
using DepthUnorderedHandler = bool (*)(ID3D12Device* real);
void set_depth_unordered_handler(DepthUnorderedHandler handler);
// True while this thread is in a resource call the proxy makes for a console description that keeps both flags.
bool console_depth_unordered();

// Console list methods that work on title addresses (V list.rs:1294-1314, 1409-1431, 1891-1983), done by the device
// behind the list's device proxy, which registers them (the Vulkan layer). `real` is the PC list, `device` the PC
// device. With no handler a slot records nothing and answers 0, as before.
struct ListTitleHandlers {
    // Slot 62, CopyMemoryToMemoryX(dst, src, size, flags): a GPU copy between title addresses. False: not recorded.
    bool (*copy_memory)(ID3D12Device* device, ID3D12GraphicsCommandList* real, std::uint64_t dst, std::uint64_t src,
                        std::uint64_t size);
    // Slots 71/72, Write32/Write64(address, value, flush, flags): `bytes` (4 or 8) of `value` at the address once the
    // list's earlier work has finished; flags 1 and 2 (64-bit form) ask for the GPU and the system timestamp instead.
    bool (*write_value)(ID3D12Device* device, ID3D12GraphicsCommandList* real, std::uint64_t address,
                        std::uint64_t value, unsigned bytes, std::uint32_t flags);
    // Slot 148, AllocateUploadMemory(size, alignment): memory the list owns until it is reset, at an address the CPU
    // writes and the GPU reads alike; null on failure. `owner` names the list.
    void* (*allocate_upload)(ID3D12Device* device, const void* owner, std::uint64_t size, std::uint64_t alignment);
    // The list `owner` was reset or released: its upload memory goes back once the GPU has finished with it.
    void (*release_uploads)(ID3D12Device* device, const void* owner);
    // Slots 73/74, WaitValue32/WaitValue64(address, comparison, reference): the list holds the rest of its commands
    // until the value is in memory (VIctory's cut, V list.rs:290-345 and 1891-1928): the queue that executes it runs
    // what comes before and stands at the wait. False: the list cannot hold it, and ExecuteX checks it instead.
    bool (*wait_value)(ID3D12Device* device, ID3D12GraphicsCommandList* real, std::uint64_t address,
                       std::uint32_t comparison, std::uint64_t reference, bool wide);
    // Queue slot 32, PresentX: what the device's queues hold for memory values runs before the present, met or not,
    // as VIctory's queues do at a present (V queue.rs:113-119: a wait is not kept across frames).
    void (*force_waits)(ID3D12Device* device);
};
void set_list_title_handlers(const ListTitleHandlers* handlers);

// The value waits (slots 73/74) a console list recorded since it was last reset, for the queue's ExecuteX: each is
// (address, comparison, reference, 64-bit). Tests read them through this.
struct ValueWait {
    std::uint64_t address;
    std::uint32_t comparison;
    std::uint64_t reference;
    bool wide;
    bool held;  // the list holds it itself (ListTitleHandlers::wait_value): ExecuteX does not check it
    // Whether `value` (the 32 or 64 bits at the address) meets the wait: the comparison codes are the D3D12 comparison
    // functions' (1 never .. 8 always, the value against the reference), as V list.rs:1844-1857 takes them; a code
    // not known counts as met.
    bool met_by(std::uint64_t value) const;
};
std::size_t command_list_value_waits(const void* list, ValueWait* out, std::size_t capacity);
// Submissions that went to the GPU with a value wait no list held and that was not met yet (ExecuteX checks those for
// up to LEONIDA_VALUE_WAIT_MS and then runs them).
std::uint64_t unmet_value_wait_submissions();

// True for slots the proxy handles itself rather than forwarding verbatim in the PC layout: IUnknown (0-2), 8 and 75
// (queue proxies), 9 (allocator proxies), 12 (lists), 14, 25/26 (struct returns), 31/34/35/45/46/50 (proxies
// unwrapped), 32 and 36 (fence proxies), the Xbox routes 48/73/78-81 at their configured indices, 62, 67, 76, 108,
// 127 and 133. Slot 10 is intentionally false: the measured 9/10 handle conversion is on the heap proxy, and device
// slot 9 is CreateCommandAllocator, which the proxy handles for the allocator proxy. With `console_layout`, the same
// for a device created as ID3D12Device11: every slot from 44 on is handled there (refused when no source names it).
bool device_slot_overridden(unsigned slot);
bool device_slot_overridden(unsigned slot, bool console_layout);

// True when `device` (the proxy or its DXGI pointer) was created as ID3D12Device11: its queues, fences, allocators and
// lists have the console layouts (d3d12_xs_slots.h).
bool device_proxy_console_layout(const void* device);
// True for a queue, fence or allocator proxy of such a device.
bool command_object_proxy_console_layout(const void* object);

// Console queue slot 32, PresentX(plane count, planes, parameters): what the device's queues call. `queue` is the real
// queue behind the proxy the title presented on; `planes` and `parameters` are the title's (the layouts are
// rt::d3d12::XboxPresentPlaneParameters and XboxPresentParameters, shims/xbox_present.h). Without a handler PresentX
// is accepted and nothing is shown, as VIctory does (queue.rs:757-841 always answers S_OK). On the Vulkan device,
// D3D12XboxCreateDevice sets rt::d3d12::attach_title_presentation's handler (xbox_window.h): an XboxPresentation in a
// window that takes its tokens from the device's frame calls (78-81, VIctory's clock; see device_frames_begun).
using PresentHandler = HRESULT (*)(void* context, ID3D12CommandQueue* queue, std::uint32_t plane_count,
                                   const void* planes, const void* parameters);
// `device` is the proxy or its DXGI pointer; false (nothing set) for anything else. A null handler detaches.
bool set_present_handler(void* device, PresentHandler handler, void* context);

// Console queue slot 31, CopyPageMappingsBatchX (V queue.rs:897-962): the physical pages behind one range of title
// memory mapped again at another, (to, from, len), `from` 0 unmapping (V resource.rs:935-948, use_page_mapper). gta6rt
// sets xmem's (rt::xmem::map_alias). Without one the slot answers E_INVALIDARG, as V's does.
using PageMapper = bool (*)(std::uint64_t to, std::uint64_t from, std::uint64_t len);
void set_page_mapper(PageMapper mapper);

// A page pool the device's console slot 74 registered and slot 75 has not removed: its first address and page count
// (64 KiB pages). For the tests: queue slot 31 takes the pool's address and page count from the title.
bool device_page_pool(const void* device, std::uint64_t handle, std::uint64_t* address, std::uint32_t* pages);

// The frame tokens the device's WaitFrameEventX (slot 80) handed out so far, and the last tick a wait for event
// `kind` ended on (kinds from 2 on share one, as in V frame.rs:91-97). Test helpers.
std::uint64_t device_frames_begun(const void* device);
std::uint64_t device_frame_last_tick(const void* device, unsigned kind);

// Queue and fence proxies created by device slots 8/75 and 36. The real_* accessors return the wrapped object
// (no reference added), or null when the pointer is not such a proxy.
bool is_queue_proxy(const void* queue);
bool is_fence_proxy(const void* fence);
ID3D12CommandQueue* queue_proxy_real_queue(const void* queue);
ID3D12Fence* fence_proxy_real_fence(const void* fence);
// Public slots forwarded (19 for a queue; 11 or 12 for a fence), and the size of the proxy's vtable.
unsigned queue_proxy_slot_count(const void* queue);
unsigned fence_proxy_slot_count(const void* fence);
unsigned queue_proxy_table_size();
unsigned fence_proxy_table_size();
// Live references on a queue, fence or allocator proxy (0 for anything else). Test helper.
unsigned long command_object_proxy_ref_count(const void* object);
// Per-slot call counters and first-call flags, shared by all queue proxies and all fence proxies.
std::uint64_t queue_slot_hits(unsigned slot);
bool queue_slot_first_call_seen(unsigned slot);
std::uint64_t fence_slot_hits(unsigned slot);
bool fence_slot_first_call_seen(unsigned slot);
// Vtable reads past a queue or fence proxy table that faulted in its guard region (counted by the fault logger).
std::uint64_t queue_table_overruns();
std::uint64_t fence_table_overruns();
// True when the queue, fence and allocator tables sit in front of their guard regions (false only if a reservation
// or the fault logger failed).
bool command_object_tables_guarded();

// Command allocator proxies (device slot 9): the public chain (9 slots: the base chain and Reset) is forwarded, a
// slot past it up to the end of the table (64 slots) is logged and refused with E_NOTIMPL, and a read past the
// table faults in a guard region, as for queues and fences. The console allocator's extra methods are not
// measured and not guessed. Slot 9 wraps only an allocator that answers kLeonidaCommandAllocatorTag, Leonida's
// own Vulkan-layer allocator: its command lists resolve the proxy (CreateCommandList, Reset). A PC runtime
// allocator goes back unchanged, because the PC runtime's lists would take a proxy for their own allocator; a
// command-list proxy that unwraps at Reset is what lets the host backend wrap allocators too.
// {7d0e6b51-34a2-4f0c-9a61-0c5b2e8f4a10}: the Vulkan layer's CommandAllocator answers it with its implementation.
inline constexpr GUID kLeonidaCommandAllocatorTag{
    0x7d0e6b51, 0x34a2, 0x4f0c, {0x9a, 0x61, 0x0c, 0x5b, 0x2e, 0x8f, 0x4a, 0x10}};
bool is_allocator_proxy(const void* allocator);
// The wrapped allocator (no reference added), or null when `allocator` is not an allocator proxy.
ID3D12CommandAllocator* allocator_proxy_real_allocator(const void* allocator);
unsigned allocator_proxy_slot_count(const void* allocator);
unsigned allocator_proxy_table_size();
std::uint64_t allocator_slot_hits(unsigned slot);
bool allocator_slot_first_call_seen(unsigned slot);
std::uint64_t allocator_table_overruns();
// Command list proxies created by device slot 73 (CreateCommandListX):
bool is_command_list_proxy(const void* list);
ID3D12GraphicsCommandList* command_list_proxy_real_list(const void* list);
unsigned command_list_proxy_table_size();
std::uint64_t list_slot_hits(unsigned slot);
bool list_slot_first_call_seen(unsigned slot);

// Heap slots 0-2, 7 (GetDevice -> device proxy) and 9/10 (handle conversion).
bool heap_slot_overridden(unsigned slot);

// The PC list slot a console list slot reaches (the list table in d3d12_xs_slots.h), or kListNotForwarded for one the
// proxy answers itself (ignored, not implemented yet, or not named by any source). Slots 0-2 and 7 are the proxy's
// own (IUnknown, GetDevice) and also give kListNotForwarded.
inline constexpr unsigned kListNotForwarded = ~0u;
inline constexpr unsigned console_to_pc_list_slot(unsigned slot) {
    const slots::ListSlot entry = slots::console_list_slot(slot);
    switch (entry.method) {
        case slots::ListMethod::forward:
        case slots::ListMethod::reset:
        case slots::ListMethod::heaps:
        case slots::ListMethod::typed:
            return entry.pc;
        default:
            return kListNotForwarded;
    }
}


// Number of live references on a device proxy. Test helper for the heap's GetDevice/lifetime checks.
unsigned long device_proxy_ref_count(const void* device);

// Per-slot call counters and first-call flags for the device and heap proxies. Slot 0 also counts the
// QueryInterface call made while a proxy is created, so tests reset the counters after creation.
// reset_device_proxy_stats also resets the queue and fence counters.
// The hit counters (device, heap, queue, fence and allocator) are for tests, and they are off at the start.
// A test must read a counter (any *_slot_hits function) or call reset_device_proxy_stats BEFORE the calls that it
// counts. Calls made before that are not counted (the counter stays 0); they set only the first-call flags.
std::uint64_t device_slot_hits(unsigned slot);
bool device_slot_first_call_seen(unsigned slot);
std::uint64_t heap_slot_hits(unsigned slot);
bool heap_slot_first_call_seen(unsigned slot);
void reset_device_proxy_stats();

}  // namespace rt::d3d12_xs
