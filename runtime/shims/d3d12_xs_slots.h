// Console D3D12 slot tables for the Xbox-ABI proxies (d3d12_xs_device.cpp): which method every vtable slot of the
// device, the command queue, the command list and the fence is, as data. A slot is the absolute vtable index (x64,
// 8 bytes each, QueryInterface = 0); the byte offset a title calls through is slot * 8.
//
// Sources, cited per block:
//   V = victory/crates/victory-gpu/src/, which runs the title on host D3D12: device.rs:402-458, queue.rs:957-1001,
//       list.rs:1378-1387 and 2055-2173, fence.rs:141-154. Its tables are what the title was seen to call.
//   L = Leonida's own measurements: docs/d3d12_command_objects.md (the package's BVH builder and FSR DLL, compiled
//       against the same console headers) and docs/retail_boot_chain.md (the title under gta6rt).
// Where the two disagree, the table follows the one with evidence from the title and says so.
//
// The layout switch (V device.rs:149-168): a title that creates its device as ID3D12Device11 gets the newer console
// layouts. In them every queue slot from 14 and every list slot from 59 is two later (V queue.rs:978-987,
// list.rs:2127-2136), and the list has three methods the older layout lacks (V list.rs:2138-2148). The device's own
// table does not change with the layout (V device.rs:402-458 is one table).
#pragma once

#include <cstdint>

namespace rt::d3d12_xs::slots {

enum class Layout : std::uint8_t {
    // Not a console layout: the device keeps forwarding in PC order past its measured slots, queues and fences forward
    // the public chain in PC order, and device slot 12 hands out the PC list. A device created as any interface other
    // than ID3D12Device11 gets it: Leonida's own tests and fixtures drive the proxies through the PC C++ interfaces.
    // VIctory's older console layout (V's FUNCTIONS tables, Minecraft 2022) is not modelled; no title here uses it.
    pc,
    // The newer console layouts (V device.rs:149-168, NEWER_FUNCTIONS): the device was created as ID3D12Device11.
    console,
};

// ---- Device ----------------------------------------------------------------------------------------------------

enum class DeviceMethod : std::uint8_t {
    forward,             // the PC method at the same slot (refused when the real device's table is shorter)
    refuse,              // E_NOTIMPL, logged once with the first argument words: no source names the slot
    open,                // E_NOTIMPL, logged once: a console method the proxy does not implement yet
    ignore,              // accepted and ignored: S_OK
    query_interface,
    add_ref,
    release,
    create_queue,        // 8: a queue proxy
    create_allocator,    // 9: an allocator proxy
    create_list,         // 12: the PC list (pc layout) or a console list proxy (console layout)
    create_heap,         // 14: a descriptor-heap proxy
    struct_return,       // 25, 26, 108: the PC method; the title passes the return buffer first (resolved per call)
    unwrap,              // the PC method, with heap/queue/fence/allocator proxies replaced by their real objects
    open_shared_handle,  // 32: a fence comes back wrapped
    create_fence,        // 36: a fence proxy
    routed,              // 48, 73, 78-81: the Xbox routes, movable with LEONIDA_XS_*_SLOT
    create_queue1,       // 75 in the pc layout: PC ID3D12Device9::CreateCommandQueue1
    unknown_pipeline,    // 57: refused with the output nulled (V device.rs:349-400)
    compute_pso_x,       // 62
    native_compute_pso,  // 67
    committed_resource_x,  // 76
    register_page_pool,    // 74 in the console layout
    unregister_page_pool,  // 75 in the console layout
    committed_resource3,   // 127
    placed_resource_x,     // 133
};

struct DeviceSlot {
    DeviceMethod pc;       // the method in the pc layout
    DeviceMethod console;  // the method in the console layout
    const char* name;      // null when no source names the slot
};

inline constexpr unsigned kDeviceSlots = 192;  // the proxy's table; the title calls slot 133 (L retail_boot_chain §3.4)

struct DeviceEntry {
    std::uint8_t slot;
    DeviceMethod pc, console;
    const char* name;
};

// The named slots. Every other slot is forward in the pc layout; in the console layout it is forward below 44 and
// refused from 44 on. The console device keeps the PC ID3D12Device order through slot 43 and has its own methods
// after it (V device.rs:1-6 and 402-405: 76 base device methods, slots 7-82, then ID3D12Device1-8's at 83-116; L
// retail_boot_chain §3.2-3.4 measured 48, 73, 78, 79 and 133 as console methods where the PC has others). So a
// console slot from 44 on that no source names is refused, not sent to the PC method that happens to be there.
inline constexpr DeviceEntry kDeviceEntries[] = {
    {0, DeviceMethod::query_interface, DeviceMethod::query_interface, "QueryInterface"},
    {1, DeviceMethod::add_ref, DeviceMethod::add_ref, "AddRef"},
    {2, DeviceMethod::release, DeviceMethod::release, "Release"},
    {7, DeviceMethod::forward, DeviceMethod::forward, "GetNodeCount"},
    {8, DeviceMethod::create_queue, DeviceMethod::create_queue, "CreateCommandQueue -> queue proxy"},
    {9, DeviceMethod::create_allocator, DeviceMethod::create_allocator, "CreateCommandAllocator -> allocator proxy"},
    {10, DeviceMethod::forward, DeviceMethod::forward, "CreateGraphicsPipelineState"},
    {11, DeviceMethod::forward, DeviceMethod::forward, "CreateComputePipelineState"},
    // V device.rs:413 and list.rs:566: the list has the console layout (L: the PC list in the pc layout).
    {12, DeviceMethod::create_list, DeviceMethod::create_list, "CreateCommandList"},
    {13, DeviceMethod::forward, DeviceMethod::forward, "CheckFeatureSupport"},
    {14, DeviceMethod::create_heap, DeviceMethod::create_heap, "CreateDescriptorHeap -> heap proxy"},
    {15, DeviceMethod::forward, DeviceMethod::forward, "GetDescriptorHandleIncrementSize"},
    {16, DeviceMethod::forward, DeviceMethod::forward, "CreateRootSignature"},
    {17, DeviceMethod::forward, DeviceMethod::forward, "CreateConstantBufferView"},
    {18, DeviceMethod::forward, DeviceMethod::forward, "CreateShaderResourceView"},
    {19, DeviceMethod::forward, DeviceMethod::forward, "CreateUnorderedAccessView"},
    {20, DeviceMethod::forward, DeviceMethod::forward, "CreateRenderTargetView"},
    {21, DeviceMethod::forward, DeviceMethod::forward, "CreateDepthStencilView"},
    {22, DeviceMethod::forward, DeviceMethod::forward, "CreateSampler"},
    {23, DeviceMethod::forward, DeviceMethod::forward, "CopyDescriptors"},
    {24, DeviceMethod::forward, DeviceMethod::forward, "CopyDescriptorsSimple"},
    // Struct returns take the return buffer first: V resource.rs:802-811 (25) and 2583-2593 (26); L retail_boot_chain
    // §3.1 measured 26. The thunk tells the two orders apart, so a PC-order caller still works.
    {25, DeviceMethod::struct_return, DeviceMethod::struct_return, "GetResourceAllocationInfo (return buffer first)"},
    {26, DeviceMethod::struct_return, DeviceMethod::struct_return, "GetCustomHeapProperties (return buffer first)"},
    {27, DeviceMethod::forward, DeviceMethod::forward, "CreateCommittedResource"},
    {28, DeviceMethod::forward, DeviceMethod::forward, "CreateHeap"},
    {29, DeviceMethod::forward, DeviceMethod::forward, "CreatePlacedResource"},
    {30, DeviceMethod::forward, DeviceMethod::forward, "CreateReservedResource"},
    {31, DeviceMethod::unwrap, DeviceMethod::unwrap, "CreateSharedHandle (proxies unwrapped)"},
    {32, DeviceMethod::open_shared_handle, DeviceMethod::open_shared_handle,
     "OpenSharedHandle (a fence comes back wrapped)"},
    {34, DeviceMethod::unwrap, DeviceMethod::unwrap, "MakeResident (proxies unwrapped)"},
    {35, DeviceMethod::unwrap, DeviceMethod::unwrap, "Evict (proxies unwrapped)"},
    {36, DeviceMethod::create_fence, DeviceMethod::create_fence, "CreateFence -> fence proxy"},
    {38, DeviceMethod::forward, DeviceMethod::forward, "GetCopyableFootprints"},
    {39, DeviceMethod::forward, DeviceMethod::forward, "CreateQueryHeap"},
    {41, DeviceMethod::forward, DeviceMethod::forward, "CreateCommandSignature"},
    {43, DeviceMethod::forward, DeviceMethod::forward, "GetAdapterLuid"},
    // PC ID3D12Device1/3 methods in the pc layout. On the console these slots are console methods nobody has named
    // (V device.rs:402-405; the PC ones would be at 84, 85 and 89).
    {45, DeviceMethod::unwrap, DeviceMethod::refuse, "SetEventOnMultipleFenceCompletion (pc layout)"},
    {46, DeviceMethod::unwrap, DeviceMethod::refuse, "SetResidencyPriority (pc layout)"},
    {48, DeviceMethod::routed, DeviceMethod::routed, "Xbox CreateCommandQueueX -> queue proxy"},
    {50, DeviceMethod::unwrap, DeviceMethod::refuse, "EnqueueMakeResident (pc layout)"},
    {57, DeviceMethod::forward, DeviceMethod::unknown_pipeline,
     "Xbox pipeline creation (V device.rs:438, not written)"},
    {61, DeviceMethod::forward, DeviceMethod::open, "Xbox CreateSamplerX (V device.rs:439, open)"},
    {62, DeviceMethod::compute_pso_x, DeviceMethod::compute_pso_x, "Xbox CreateComputePipelineState"},
    // V device.rs:441 does not implement 67 (same call shape, an unknown pipeline); L §3.5a measured it in the FSR DLL.
    {67, DeviceMethod::native_compute_pso, DeviceMethod::native_compute_pso, "Xbox DeserializeComputePipelineStateX"},
    {69, DeviceMethod::forward, DeviceMethod::ignore, "Xbox debug error filter (V device.rs:442, ignored)"},
    {70, DeviceMethod::forward, DeviceMethod::open, "Xbox placed SRV over title memory (V device.rs:443, open)"},
    {71, DeviceMethod::forward, DeviceMethod::open, "Xbox placed UAV over title memory (V device.rs:444, open)"},
    {73, DeviceMethod::routed, DeviceMethod::routed, "Xbox CreateCommandListX -> console list proxy"},
    {74, DeviceMethod::forward, DeviceMethod::register_page_pool, "Xbox RegisterPagePoolX (V resource.rs:2196-2212)"},
    {75, DeviceMethod::create_queue1, DeviceMethod::unregister_page_pool,
     "CreateCommandQueue1 (pc layout) / Xbox UnregisterPagePoolX (V resource.rs:2214-2223)"},
    {76, DeviceMethod::committed_resource_x, DeviceMethod::committed_resource_x, "Xbox CreateCommittedResourceX"},
    {78, DeviceMethod::routed, DeviceMethod::routed, "Xbox SetFrameIntervalX"},
    {79, DeviceMethod::routed, DeviceMethod::routed, "Xbox ScheduleFrameEventX"},
    {80, DeviceMethod::routed, DeviceMethod::routed, "Xbox WaitFrameEventX"},
    {81, DeviceMethod::routed, DeviceMethod::routed, "Xbox GetFrameStatisticsX"},
    {86, DeviceMethod::forward, DeviceMethod::open, "Xbox CreatePipelineState from streams (V pipeline.rs, open)"},
    // V resource.rs:840-849: ID3D12Device8's GetResourceAllocationInfo2 (PC slot 68), return buffer first.
    {108, DeviceMethod::struct_return, DeviceMethod::struct_return, "GetResourceAllocationInfo2 (return buffer first)"},
    {127, DeviceMethod::committed_resource3, DeviceMethod::committed_resource3, "Xbox CreateCommittedResource3"},
    {133, DeviceMethod::placed_resource_x, DeviceMethod::placed_resource_x, "Xbox CreatePlacedResourceX"},
};

constexpr DeviceSlot device_slot(unsigned slot) {
    for (const DeviceEntry& e : kDeviceEntries)
        if (e.slot == slot) return {e.pc, e.console, e.name};
    return {DeviceMethod::forward, slot < 44 ? DeviceMethod::forward : DeviceMethod::refuse, nullptr};
}

// ---- Command queue (console layout) ----------------------------------------------------------------------------

enum class QueueMethod : std::uint8_t {
    refuse,    // E_NOTIMPL, logged once with the first argument words: no source names the slot
    open,      // E_NOTIMPL, logged once: a console method the proxy does not implement yet
    forward,   // the PC method at `pc`
    execute,   // the PC ExecuteCommandLists with list proxies unwrapped (ExecuteCommandLists, ExecuteX)
    marker,    // ignored: V queue.rs:611-612 ("markers and events only matter to profilers")
    signal,    // the PC Signal with the fence unwrapped, and the fence's value word kept up to date (Signal, Signal2X)
    wait,      // the PC Wait with the fence unwrapped
    get_desc,  // the PC GetDesc; the return buffer may come first (resolved per call)
    present,   // PresentX: the device's present handler, or accepted (V queue.rs:757-841 always answers S_OK)
};

struct QueueSlot {
    QueueMethod method;
    std::uint8_t pc;   // the PC slot for forward, execute, signal, wait and get_desc
    const char* name;  // null when no source names the slot
};

// The proxy's queue table (the console layout's last known slot is 35; the rest is the refused tail).
inline constexpr unsigned kQueueSlots = 128;

struct QueueEntry {
    std::uint8_t older_slot;  // the older layout's slot, as V lists it
    QueueMethod method;
    std::uint8_t pc;
    const char* name;
};

// V queue.rs:957-976 (METHODS), in the older layout. QueryInterface, AddRef, Release and GetDevice are the proxy's own.
inline constexpr QueueEntry kQueueEntries[] = {
    {3, QueueMethod::forward, 3, "GetPrivateData"},
    {4, QueueMethod::forward, 4, "SetPrivateData"},
    {5, QueueMethod::forward, 5, "SetPrivateDataInterface"},
    {6, QueueMethod::forward, 6, "SetName"},
    {10, QueueMethod::execute, 10, "ExecuteCommandLists"},
    {11, QueueMethod::marker, 0, "SetMarker"},
    {12, QueueMethod::marker, 0, "BeginEvent"},
    {13, QueueMethod::marker, 0, "EndEvent"},
    {14, QueueMethod::signal, 14, "Signal"},
    {15, QueueMethod::wait, 15, "Wait"},
    {16, QueueMethod::forward, 16, "GetTimestampFrequency"},
    {21, QueueMethod::open, 0, "SuspendX"},  // V queue.rs:983: suspend and resume at 0xb8/0xc0 in the newer layout
    {22, QueueMethod::open, 0, "ResumeX"},
    {26, QueueMethod::get_desc, 18, "GetDesc"},
    {29, QueueMethod::open, 0, "CopyPageMappingsBatchX"},  // V queue.rs:897-948: needs the page mapper (open)
    {30, QueueMethod::present, 0, "PresentX"},
    {32, QueueMethod::signal, 14, "Signal2X"},   // V queue.rs:600-610: signals at once, the sync settings unused
    {33, QueueMethod::execute, 10, "ExecuteX"},  // V queue.rs:595-599: the frame token and flags unused
};

// V queue.rs:978-987: in the newer layout every slot from 14 (0x70) on is two later.
constexpr unsigned newer_queue_slot(unsigned older) { return older >= 14 ? older + 2 : older; }

constexpr QueueSlot console_queue_slot(unsigned slot) {
    for (const QueueEntry& e : kQueueEntries)
        if (newer_queue_slot(e.older_slot) == slot) return {e.method, e.pc, e.name};
    return {QueueMethod::refuse, 0, nullptr};
}

// ---- Command list (console layout; every list proxy has it) ----------------------------------------------------

enum class ListMethod : std::uint8_t {
    unknown,  // no source names the slot: returns 0, logged once with the first argument words
    open,     // a console method not written yet: returns 0 (no memory from 148), logged once
    ignore,   // accepted, nothing recorded: V's no-ops (hints, syncs, markers)
    forward,  // the PC method at `pc`
    reset,    // the PC Reset at `pc`: the allocator proxy unwrapped and the inline cursor pinned again
    heaps,    // the PC SetDescriptorHeaps at `pc` with heap proxies unwrapped
    typed,    // the PC method at `pc` through a thunk with the real prototype: the method takes floats in XMM
              // registers, which a forwarding thunk declared with integer words does not keep
};

struct ListSlot {
    ListMethod method;
    std::uint8_t pc;   // the PC slot for forward, reset, heaps and typed
    const char* name;  // null when no source names the slot
};

inline constexpr unsigned kListSlots = 160;

struct ListEntry {
    std::uint8_t slot;
    bool older;  // the slot as V lists it in the older layout (newer_list_slot applies); else a newer-layout slot
    ListMethod method;
    std::uint8_t pc;
    const char* name;
};

// V list.rs:2134-2136: in the newer layout every slot from 59 (0x1d8) on is two later.
constexpr unsigned newer_list_slot(unsigned older) { return older >= 59 ? older + 2 : older; }

inline constexpr ListEntry kListEntries[] = {
    // PC order up to ResourceBarrier: V list.rs:2059-2079 names 7-26 but 11 and 18, L §3.3/§3.5a measured 14, 15, 25
    // and 26. ClearState (11) and CopyTiles (18) sit inside that run, so they keep their PC slots too.
    {3, false, ListMethod::forward, 3, "GetPrivateData"},
    {4, false, ListMethod::forward, 4, "SetPrivateData"},
    {5, false, ListMethod::forward, 5, "SetPrivateDataInterface"},
    {6, false, ListMethod::forward, 6, "SetName"},
    {8, false, ListMethod::forward, 8, "GetType"},
    {9, false, ListMethod::forward, 9, "Close"},
    {10, false, ListMethod::reset, 10, "Reset"},
    {11, false, ListMethod::forward, 11, "ClearState"},
    {12, false, ListMethod::forward, 12, "DrawInstanced"},
    {13, false, ListMethod::forward, 13, "DrawIndexedInstanced"},
    {14, false, ListMethod::forward, 14, "Dispatch"},
    {15, false, ListMethod::forward, 15, "CopyBufferRegion"},
    {16, false, ListMethod::forward, 16, "CopyTextureRegion"},
    {17, false, ListMethod::forward, 17, "CopyResource"},
    {18, false, ListMethod::forward, 18, "CopyTiles"},
    {19, false, ListMethod::forward, 19, "ResolveSubresource"},
    {20, false, ListMethod::forward, 20, "IASetPrimitiveTopology"},
    {21, false, ListMethod::forward, 21, "RSSetViewports"},
    {22, false, ListMethod::forward, 22, "RSSetScissorRects"},
    {23, false, ListMethod::forward, 23, "OMSetBlendFactor"},
    {24, false, ListMethod::forward, 24, "OMSetStencilRef"},
    {25, false, ListMethod::forward, 25, "SetPipelineState"},
    {26, false, ListMethod::forward, 26, "ResourceBarrier"},
    // Queries right after the barrier: V list.rs:2114-2117 (PC 52-54). The +4 rule this table replaces sent 56-58
    // there instead.
    {28, false, ListMethod::forward, 52, "BeginQuery"},
    {29, false, ListMethod::forward, 53, "EndQuery"},
    {30, false, ListMethod::forward, 54, "ResolveQueryData"},
    // PC 28-51 four slots later: V list.rs:2080-2099 names 32-48 and 50-52; L §3.3, §3.3a and §3.5a measured 32,
    // 33, 35, 37, 39, 41, 43, 45 and 53-55. SOSetTargets (49) sits inside that run.
    {32, false, ListMethod::heaps, 28, "SetDescriptorHeaps"},
    {33, false, ListMethod::forward, 29, "SetComputeRootSignature"},
    {34, false, ListMethod::forward, 30, "SetGraphicsRootSignature"},
    {35, false, ListMethod::forward, 31, "SetComputeRootDescriptorTable"},
    {36, false, ListMethod::forward, 32, "SetGraphicsRootDescriptorTable"},
    {37, false, ListMethod::forward, 33, "SetComputeRoot32BitConstant"},
    {38, false, ListMethod::forward, 34, "SetGraphicsRoot32BitConstant"},
    {39, false, ListMethod::forward, 35, "SetComputeRoot32BitConstants"},
    {40, false, ListMethod::forward, 36, "SetGraphicsRoot32BitConstants"},
    {41, false, ListMethod::forward, 37, "SetComputeRootConstantBufferView"},
    {42, false, ListMethod::forward, 38, "SetGraphicsRootConstantBufferView"},
    {43, false, ListMethod::forward, 39, "SetComputeRootShaderResourceView"},
    {44, false, ListMethod::forward, 40, "SetGraphicsRootShaderResourceView"},
    {45, false, ListMethod::forward, 41, "SetComputeRootUnorderedAccessView"},
    {46, false, ListMethod::forward, 42, "SetGraphicsRootUnorderedAccessView"},
    {47, false, ListMethod::forward, 43, "IASetIndexBuffer"},
    {48, false, ListMethod::forward, 44, "IASetVertexBuffers"},
    {49, false, ListMethod::forward, 45, "SOSetTargets"},
    {50, false, ListMethod::forward, 46, "OMSetRenderTargets"},
    {51, false, ListMethod::typed, 47, "ClearDepthStencilView"},  // its depth is a float (XMM3)
    {52, false, ListMethod::forward, 48, "ClearRenderTargetView"},
    {53, false, ListMethod::forward, 49, "ClearUnorderedAccessViewUint"},
    {54, false, ListMethod::forward, 50, "ClearUnorderedAccessViewFloat"},
    {55, false, ListMethod::forward, 51, "DiscardResource"},
    // L §3.5a (docs/d3d12_command_objects.md:565): a console GPU-event marker, (bool, blob, size). Not a PC method.
    {59, false, ListMethod::ignore, 0, "GPU event marker (console)"},
    // V list.rs:2100-2111, older layout.
    {59, true, ListMethod::forward, 59, "ExecuteIndirect"},
    {60, true, ListMethod::open, 0, "CopyMemoryToMemoryX"},  // V list.rs:1294-1314: needs title memory on the GPU
    {68, true, ListMethod::typed, 62, "OMSetDepthBounds"},    // two floats (XMM1, XMM2)
    {69, true, ListMethod::open, 0, "Write32 (bottom-of-pipe value)"},  // V list.rs:1964-1983; L §3.3a slot 71
    {70, true, ListMethod::open, 0, "Write64 (bottom-of-pipe value or timestamp)"},
    {71, true, ListMethod::open, 0, "WaitValue32 (wait for a memory value)"},  // V list.rs:1891-1934; L §3.3a slot 73
    {72, true, ListMethod::open, 0, "WaitValue64"},
    {73, true, ListMethod::ignore, 0, "pixel shader depth order hint"},
    {78, true, ListMethod::forward, 14, "DispatchX"},  // V list.rs:1321-1328: (x, y, z, flags); L §3.3a slot 80
    {83, true, ListMethod::forward, 16, "CopyTextureRegionX"},  // V list.rs:1113-1124: a trailing flags word
    {84, true, ListMethod::forward, 17, "CopyResourceX"},       // V list.rs:887-889: a trailing flags word
    {98, true, ListMethod::open, 0, "DispatchMeshX"},           // V list.rs:1596-1613: indirect form unknown
    {99, true, ListMethod::ignore, 0, "DriverHintX"},           // V list.rs:1316-1319: returns 0
    {124, true, ListMethod::ignore, 0, "shading rate hint"},
    {127, true, ListMethod::forward, 79, "DispatchMesh"},
    {129, true, ListMethod::ignore, 0, "pipeline sync, top of pipe"},  // V list.rs:1397-1401
    {130, true, ListMethod::ignore, 0, "pipeline sync, bottom of pipe"},
    {137, true, ListMethod::reset, 10, "ResetX"},  // V list.rs:1403-1407: a trailing flags word; asserts on failure
    // Newer layout only: V list.rs:2138-2148, L §3.3a (146, 148) and §3.5a (141).
    {141, false, ListMethod::open, 0, "compute scratch binding (FSR DLL)"},
    {142, false, ListMethod::ignore, 0, "state flush word"},
    {146, false, ListMethod::forward, 80, "Barrier"},
    {148, false, ListMethod::open, 0, "AllocateUploadMemory"},  // V list.rs:1409-1431: CPU and GPU address alike
};

constexpr ListSlot console_list_slot(unsigned slot) {
    for (const ListEntry& e : kListEntries)
        if ((e.older ? newer_list_slot(e.slot) : e.slot) == slot) return {e.method, e.pc, e.name};
    return {ListMethod::unknown, 0, nullptr};
}

// ---- Fence and command allocator -------------------------------------------------------------------------------

// V fence.rs:141-154: IUnknown 3, object 4, device child 1, fence 3, Fence1 1, then the console's own: slot 12 hands
// out the address of the fence's 64-bit value, which titles read straight from memory (fence.rs:24-29, 136-139).
inline constexpr unsigned kFenceValueAddressSlot = 12;
// V list.rs:1378-1387: allocator slot 9 is an allocation callback, accepted and ignored.
inline constexpr unsigned kAllocatorCallbackSlot = 9;

// ---- Checks on the data ----------------------------------------------------------------------------------------

namespace detail {
// No two entries of a table name the same slot, and every slot fits the proxy's table.
constexpr bool device_entries_ok() {
    for (const DeviceEntry& a : kDeviceEntries) {
        if (a.slot >= kDeviceSlots) return false;
        unsigned seen = 0;
        for (const DeviceEntry& b : kDeviceEntries) seen += a.slot == b.slot;
        if (seen != 1) return false;
    }
    return true;
}
constexpr bool queue_entries_ok() {
    for (const QueueEntry& a : kQueueEntries) {
        if (newer_queue_slot(a.older_slot) >= kQueueSlots) return false;
        unsigned seen = 0;
        for (const QueueEntry& b : kQueueEntries)
            seen += newer_queue_slot(a.older_slot) == newer_queue_slot(b.older_slot);
        if (seen != 1) return false;
    }
    return true;
}
constexpr unsigned list_entry_slot(const ListEntry& e) { return e.older ? newer_list_slot(e.slot) : e.slot; }
constexpr bool list_entries_ok() {
    for (const ListEntry& a : kListEntries) {
        if (list_entry_slot(a) >= kListSlots) return false;
        unsigned seen = 0;
        for (const ListEntry& b : kListEntries) seen += list_entry_slot(a) == list_entry_slot(b);
        if (seen != 1) return false;
    }
    return true;
}
}  // namespace detail
static_assert(detail::device_entries_ok(), "kDeviceEntries: a slot twice or past the table");
static_assert(detail::queue_entries_ok(), "kQueueEntries: a slot twice or past the table");
static_assert(detail::list_entries_ok(), "kListEntries: a slot twice or past the table");
// The anchors the port plan names (docs/port_plan.md step 2): the title's Signal, ExecuteIndirect and Barrier.
static_assert(console_queue_slot(16).method == QueueMethod::signal && console_queue_slot(16).pc == 14);
static_assert(console_queue_slot(32).method == QueueMethod::present && console_queue_slot(35).pc == 10);
static_assert(console_list_slot(61).pc == 59 && console_list_slot(146).pc == 80 && console_list_slot(139).pc == 10);
static_assert(console_list_slot(59).method == ListMethod::ignore &&
              console_list_slot(56).method == ListMethod::unknown);

}  // namespace rt::d3d12_xs::slots
