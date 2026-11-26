#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace rt::xmem {

bool commit_physical_range(const void* address, std::size_t size);
bool commit_physical_pages(const std::uint64_t* page_array, std::size_t count);

// The title's GPU-visible memory (src/title_memory.h): what it commits with XMemVirtualAlloc with graphics attributes
// (or with kernel32!VirtualAlloc and MEM_GRAPHICS, or inside a graphics reservation), and the physical pages it maps
// into windows (committed when mapped). The listener, the D3D12 layer, imports each range as it becomes visible and
// releases it from the GPU before the title frees, decommits, unmaps or remaps it: `gone` is called first, the pages
// change after it returns. Both run under xmem's own lock, so a listener must not call back into xmem. Setting a
// listener replays every visible range to it; null detaches.
struct GraphicsListener {
    void (*visible)(std::uintptr_t start, std::size_t size);
    void (*gone)(std::uintptr_t start, std::size_t size);
};
void set_graphics_listener(const GraphicsListener* listener);

// The physical pages mapped at [from, from + len) mapped again at `to` (both 64 KiB aligned; the last page in full), or
// with `from` 0 the pages at [to, to + len) unmapped: VIctory's page mapper (victory-hle xmem.rs:754-791), for the
// console queue's CopyPageMappingsBatchX (rt::d3d12_xs::set_page_mapper). The title memory at `to` leaves the GPU and
// comes back as it does for XMemMapPhysicalPages. False when a page of `from` has no physical page mapped, or a page
// of `to` is not in a physical-page window (the pages before it are mapped).
bool map_alias(std::uint64_t to, std::uint64_t from, std::uint64_t len);
// Bytes visible now, and how many ranges hold them. For tests and logs.
std::size_t graphics_visible_bytes(std::size_t* ranges = nullptr);

// Physical pages as a DirectStorage request names its destination (shims/dstorage_xs.cpp): `pages` holds `count` page
// numbers as XMemAllocatePhysicalPages handed them out, one per 64 KiB, and the bytes meant are [offset, offset + size)
// from the first page's start. They are written through their allocation's own view of its section, whether or not
// the title has mapped them anywhere, after committing them (otherwise a page is committed on its first touch).

// The bytes in place when they lie in consecutive pages of one allocation (how titles usually hand them out): `data`
// points at them, committed and writable, and `keep` holds the allocation while the caller writes. False for pages of
// several allocations or out of order, an unknown page, no bytes at all, or a commit the host refuses.
struct PhysicalSpan {
    std::uint8_t* data = nullptr;
    std::shared_ptr<void> keep;
};
bool physical_span(const std::uint64_t* pages, std::size_t count, std::size_t offset, std::size_t size,
                   PhysicalSpan& out);

// Copies `size` bytes into the pages from `offset` on, one run of consecutive pages of an allocation at a time. False
// for an unknown page or a commit the host refuses; the runs before it are written.
bool write_physical(const std::uint64_t* pages, std::size_t count, std::size_t offset, const void* data,
                    std::size_t size);

// Whether every page under [offset, offset + size) is a live page number (a write that fails with them all known is
// a host failure).
bool physical_pages_known(const std::uint64_t* pages, std::size_t count, std::size_t offset, std::size_t size);

}
