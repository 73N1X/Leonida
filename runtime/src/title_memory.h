#pragma once

// Title memory as GPU memory: a GPU address is the title's CPU address (docs/d3d12_vulkan.md section 3.3, "Title
// memory"). The translated shaders reach memory at heap_base + VA (push-constant dwords 32-33), so the D3D12 layer
// gives every piece of GPU-visible memory the device address heap_base + its VA:
//   - the GPU address arena (default-heap buffers, the layer's own descriptor heaps, shader code) at VA = its offset,
//     so its device memory sits at heap_base;
//   - the title's graphics memory (xmem) at VA = its CPU address, imported with VK_EXT_external_memory_host at
//     heap_base + that address.
// Both are allocated with an opaque capture address (bufferDeviceAddressCaptureReplay), which asks the driver for that
// device address. Measured on an RTX 4060 Ti, driver 616.64 (Vulkan 1.4.351; probes run 2026-11-25):
//   - capture-replay memory, plain device-local, BAR and imported host memory alike, gets the device address asked for
//     anywhere in about [0x2_FFF1_0000, 0xFF_FFFF_FFFF] (12 GiB to 1 TiB); an address at or above 2^40 is refused
//     with VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS, and so is one the driver already uses;
//   - a buffer bound at offset X of such memory has the memory's address + X; two imports at heap_base + their host
//     addresses both get them;
//   - the buffer's own opaque capture address reads 0: the address comes with the memory;
//   - imported host memory cannot be bound into a sparse buffer there (a sparse buffer takes memory type 1, the device
//     heap, only; host pointers import as types 2 and 3), so one sparse 2^40 mirror buffer (docs/port_plan.md, Frame
//     time) is not possible on this driver. test/vk_sparse_title_memory_test.cpp asks again (2026-11-25): every sparse
//     buffer, with or without residency, capture replay or VkExternalMemoryBufferCreateInfo for host allocations,
//     lists type 1 alone, although vkGetPhysicalDeviceExternalBufferProperties calls host allocations importable for a
//     sparse buffer (features 0x6). Bound anyway (--force-bind, invalid usage: the validation layer reports the memory
//     type), the pages work: reads, GPU writes the CPU sees, CPU writes the GPU sees, one frame at two VAs, rebinds,
//     and unbound pages read zero. One vkQueueBindSparse of 16,384 64 KiB pages took 70 ms to idle, 16,384 calls of one
//     page 1.7 s; a sparse buffer at a capture-replay address may be up to 64 GiB (128 GiB was refused). The AMD
//     driver (2026.04.17) lists host memory types for sparse buffers but has no bufferDeviceAddressCaptureReplay;
//   - an import pins its pages (256 MiB never touched: 17.9 ms and +256 MiB of working set) but does not stop the CPU
//     from decommitting or protecting them: the import keeps the old physical pages, so a range the title frees has to
//     leave the GPU first (xmem tells the layer, rt::xmem::set_graphics_listener).
//   - every host import but a device's first also gets an ordinary device address from the driver, of its size,
//     first fit right after the arena: the retail title's 4.5 GB of pools took [heap_base + 256 MiB,
//     heap_base + 4.7 GiB) that way, so its 32-bit memory, imported later, found its addresses taken. A sparse buffer
//     at a capture-replay address holds addresses without memory, and keeps those mappings out (one of 384 GiB was
//     refused, 16 GiB pieces were not);
//   - device addresses go to imports 2 MiB at a time: an import that ends inside a 2 MiB block takes the rest of it,
//     one that starts inside a block another holds is refused, and from 64 MiB on one must start on a 2 MiB line. The
//     retail title's 32-bit windows lie side by side on 1 MiB lines, so imports that share a block are merged.
// So heap_base is fixed at 512 GiB and the arena lies at [heap_base, heap_base + arena size). xmem places the title's
// 32-bit graphics memory (VIctory xmem.rs:69-88, attribute bit 2) in [1 GiB, 4 GiB), where the descriptor-table
// pointers the shaders truncate to 32 bits need it (docs/d3d12_vulkan.md, "Descriptor-table high-dword truncation"),
// and its other memory in [64 GiB, 448 GiB). The layer holds both windows' device addresses with sparse buffers and
// gives an import the part it needs; the driver's own mappings get [heap_base + 4 GiB, heap_base + 64 GiB) and what
// lies above 448 GiB, and heap_base + every title address stays below 1 TiB.
//
// No Windows or Vulkan dependency.

#include <cstdint>

namespace leonida::title_memory {

inline constexpr std::uint64_t kHeapBase = std::uint64_t(1) << 39;   // 512 GiB: heap_base on a capture-replay device
inline constexpr std::uint64_t kDeviceAddressEnd = std::uint64_t(1) << 40;  // the driver's capture-replay limit
// Where xmem puts the title's memory (VIctory xmem.rs:73-88, which uses [4 GiB, 2^40) and the low 4 GiB for 32-bit
// memory; narrowed here to leave the driver's mappings room, as above).
inline constexpr std::uint64_t kLow32Start = std::uint64_t(1) << 30;  // above any arena of up to 1 GiB
inline constexpr std::uint64_t kLow32End = std::uint64_t(1) << 32;
inline constexpr std::uint64_t kWindowStart = std::uint64_t(64) << 30;
inline constexpr std::uint64_t kWindowEnd = std::uint64_t(448) << 30;
static_assert(kHeapBase + kWindowEnd <= kDeviceAddressEnd);

// The largest single import. Bigger visible ranges are imported in pieces of this size, which keeps one failed or
// released import from taking more than this with it (Windows drivers cap a process at 4,096 allocations, so imports
// are kept few and large: docs/port_plan.md, Frame time).
inline constexpr std::uint64_t kMaxImport = std::uint64_t(1) << 30;

// The device address of title VA `va` on a capture-replay device.
constexpr std::uint64_t device_address(std::uint64_t va) { return kHeapBase + va; }

// Whether [va, va + bytes) can have the device address heap_base + va: inside one window, non-empty, no wrap.
constexpr bool addressable(std::uint64_t va, std::uint64_t bytes) {
    const auto inside = [&](std::uint64_t first, std::uint64_t end) {
        return va >= first && va < end && bytes <= end - va;
    };
    return bytes && (inside(kLow32Start, kLow32End) || inside(kWindowStart, kWindowEnd));
}

// XMem attribute words (VIctory xmem.rs:63-88): bits 0-1 the memory type, 3 for graphics memory (both processors);
// bit 2 a 32-bit address; bits 12-14, when set, the address width as 47 minus the field. The retail title passes
// 0x21, 0x3, 0x183, 0x283, 0x181, 0x7 and 0x27 (gta6rt --trace, xmem_attributes events).
constexpr bool graphics_attributes(std::uint64_t attributes) { return (attributes & 3) == 3; }
constexpr bool address_32bit(std::uint64_t attributes) { return (attributes & 4) != 0; }

// The address range xmem places an allocation with these attributes in: [first, last] (VIctory's address_range,
// narrowed to the device-address window above).
struct Range {
    std::uint64_t first;
    std::uint64_t last;
};
constexpr Range placement(std::uint64_t attributes) {
    if (address_32bit(attributes)) return {kLow32Start, kLow32End - 1};
    const std::uint64_t field = attributes >> 12 & 7;
    std::uint64_t end = kWindowEnd;
    if (field) {
        const std::uint64_t bits = 47 - field;
        if (bits < 63 && (std::uint64_t(1) << bits) < end) end = std::uint64_t(1) << bits;
    }
    return {kWindowStart, end - 1};
}

// XMemAlloc attribute words, another layout than XMemVirtualAlloc's above (VIctory xmem.rs:228-233 and resource.rs:
// 951-963, GPL-3.0-or-later, Marvisak and contributors, DexNom fork): bits 14-15 the page size (1: 64 KiB, 2: 2 MiB),
// 16-23 the allocator id, 24-28 the alignment as log2, 29-32 the memory type, bit 33 a 32-bit address.
// ASSUMPTION: memory type 0 is the CPU heap and every other type memory both processors reach. VIctory gives its
// committed resources type 1 (a default heap, write-combined on the console) or 3 (the CPU-visible heaps, cacheable);
// no other type has been seen.
constexpr unsigned alloc_page_size(std::uint64_t attributes) { return unsigned(attributes >> 14 & 3); }
constexpr unsigned alloc_allocator_id(std::uint64_t attributes) { return unsigned(attributes >> 16 & 0xff); }
constexpr unsigned alloc_alignment_log2(std::uint64_t attributes) { return unsigned(attributes >> 24 & 0x1f); }
constexpr unsigned alloc_memory_type(std::uint64_t attributes) { return unsigned(attributes >> 29 & 0xf); }
constexpr bool alloc_32bit(std::uint64_t attributes) { return (attributes >> 33 & 1) != 0; }
// Whether XMemAllocDefault gives GPU memory (pages in the window above, visible to the D3D12 layer) for these
// attributes: a graphics memory type or a 32-bit address (VIctory's XMemAllocDefault takes 32-bit graphics memory
// for the latter).
constexpr bool alloc_gpu_memory(std::uint64_t attributes) {
    return alloc_memory_type(attributes) != 0 || alloc_32bit(attributes);
}

// The attributes of a committed resource's memory, as the console's D3D runtime asks the title's allocator for it
// (VIctory resource.rs:951-963, resource_attributes): the graphics allocator id 128, 2 MiB pages from 2 MiB up (else
// 64 KiB), the alignment as log2, memory type 1 for a default heap and 3 for the CPU-visible ones. VIctory's own
// note: the console runtime's page size and memory type are not known, these are the plausible ones.
inline constexpr std::uint64_t kGraphicsAllocatorId = 128;
constexpr std::uint64_t resource_attributes(std::uint64_t size, std::uint64_t alignment, bool default_heap) {
    std::uint64_t log2 = 0;
    for (std::uint64_t a = alignment ? alignment : 1; !(a & 1) && log2 < 63; a >>= 1) ++log2;  // trailing zeros
    const std::uint64_t pages = size >= (std::uint64_t(2) << 20) ? 2 : 1;
    const std::uint64_t memory = default_heap ? 1 : 3;
    return kGraphicsAllocatorId << 16 | pages << 14 | (log2 & 0x1f) << 24 | memory << 29;
}
static_assert(resource_attributes(33423360, 0x10000, true) == 0x30808000);  // a 3840x2160 RGBA8 back buffer
static_assert(resource_attributes(0x10000, 0x10000, false) == 0x70804000);

}  // namespace leonida::title_memory
