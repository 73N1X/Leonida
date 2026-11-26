// xg_xs.dll: the console's texture layout library. The game executable (1.0.2.1) imports two functions from it.
//
// Measured from the call sites in the executable, read statically (shapes and checks only):
//   HRESULT XGSuggestSwizzleMode(const desc*, 0, uint32_t* mode)
//     2 call sites, both in the game's helper that fills a texture description before XGCreateTextureComputer. The
//     out pointer is a 32-bit field of that description, which the helper first sets to 0 and, when the mode is
//     known, writes itself as 0x100 | mode. The HRESULT is only passed back up; no caller branches on it.
//   HRESULT XGCreateTextureComputer(const desc*, computer** out)
//     4 call sites. Two test the HRESULT and take their failure path when it is negative; two ignore it and call
//     through *out at once. On the computer the callers use vtable slot 1 after they are done (3 sites, read as
//     Release; no site calls slot 0, read as AddRef, nor QueryInterface), slot 2 with one out pointer to a large
//     stack structure (2 sites), slot 8 with six arguments in a loop over subresources (1 site) and slot 10 with
//     seven arguments (1 site). No site reads the return value of slots 2, 8 or 10.
//   With both returning E_NOTIMPL (before this port), the caller at game+0x979c40 took its failure path about 19.8 s
//   into boot, destroyed its object, and game+0x1e24e80 then called a method through the null pointer.
//
// Ported from VIctory (GPL-3.0-or-later, Marvisak and contributors, DexNom fork), victory-hle src/modules/xg.rs, backed
// by the console layout of victory-formats texture.rs, which runtime/src/gfx10_tiling.h already carries
// (leonida::gfx10::ConsoleTexture: swizzle modes, mip chains and tails, depth and stencil planes, element sizes per DXGI
// format). It is the same layout the D3D12 layer detiles title textures with (d3d12_vulkan_device.cpp
// fill_from_title), with the same layout-word rule (gfx10::swizzle_from_layout_word) and the same Series X / S pipe
// tables (LEONIDA_DEVICE_TYPE=6), so what the title is told is what the layer reads back.
//   - XGSuggestSwizzleMode writes 0x100 | the suggested mode: linear for 1D, SW_64KB_Z_X for depth, SW_64KB_R_X for a
//     render target, else SW_4KB_S. shortcut (VIctory's): the rule of the library's older optimal-mode function (any
//     size, 2D, 3D and cubes alike); XGSuggestSwizzleMode itself is newer than every copy VIctory could check. Revisit
//     if streamed textures come out scrambled.
//   - XGCreateTextureComputer lays out the description (layout word 0x100 | mode, 1 row major, 0 the suggestion; at
//     most 15 mips, the room of the layout structure) and returns a reference-counted computer. A description it
//     cannot lay out returns E_INVALIDARG, as VIctory does, but *out still gets a computer whose every method fails
//     (Release counts nothing): two measured callers use *out without checking the HRESULT. COM says *out is null on
//     failure; this breaks that on purpose, because the measured callers need it.
//   - The computer's vtable, as VIctory has it: 0x00 AddRef, 0x08 Release, 0x10 the layout (VIctory's LayoutOut: size,
//     alignment, mips, planes, and per-plane mips with offsets and pitches), 0x30 an element's byte offset, 0x40 copy
//     linear rows into a subresource, 0x48 copy a subresource out to linear rows, 0x50 the 64 KiB tiling (2D 64 KiB
//     modes only; E_FAIL for others, as on the console). Slots 0x18-0x28, 0x38 and 0x58 up to kSlots - 1 print their
//     index and caller once and return E_NOTIMPL.
//   - Copies and element offsets need an address equation: linear and the S and D modes (thin and thick), and
//     SW_64KB_R_X for one-mip one-slice 2D textures of 2-, 4- or 8-byte elements. Other XOR modes (Z_X depth, the 4 KiB
//     XOR modes, R_X with mips) are laid out but their copies return E_NOTIMPL, VIctory's Error::Unsupported.
// Hypothesis, not measured here: slots 0x18-0x28, 0x38, 0x58, 0x60 (VIctory's names: size, alignment, mip offset,
// element coordinate, view descriptor, tiled resource) and that callers reach no slot above kSlots - 1.
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <utility>

#include "shims.h"
#include "../src/gfx10_tiling.h"

namespace {

namespace g = leonida::gfx10;

constexpr HRESULT kNotImpl = static_cast<HRESULT>(rt::shims::kENotImpl);
constexpr size_t kSlots = 32;

// D3D12_RESOURCE_DESC, whose layout field carries 0x100 | swizzle mode on the console (1 row major, 0 "pick one").
struct ResourceDesc {
    uint32_t dimension;
    uint64_t alignment;
    uint64_t width;
    uint32_t height;
    uint16_t depth_or_array;
    uint16_t mips;
    uint32_t format;
    uint32_t samples;
    uint32_t quality;
    uint32_t layout;
    uint32_t flags;
};
static_assert(sizeof(ResourceDesc) == 0x38 && offsetof(ResourceDesc, width) == 0x10 &&
              offsetof(ResourceDesc, format) == 0x20 && offsetof(ResourceDesc, layout) == 0x2c);

constexpr uint32_t kSwizzled = 0x100;
constexpr uint32_t kAllowRenderTarget = 1;
constexpr uint32_t kAllowDepthStencil = 2;
constexpr uint32_t kTexture1d = 2;
constexpr uint32_t kTexture2d = 3;
constexpr uint32_t kTexture3d = 4;
constexpr size_t kMips = 15;  // the layout structure has room for 15 mips
constexpr uint64_t kTile = 64u << 10;

// One mip in the layout the title reads (offsets 0x00, 0x08, 0x10 and 0x1c are read by the title; the rest follow the
// library's older public shape). VIctory xg.rs MipOut.
struct MipOut {
    uint64_t size;
    uint64_t offset;
    uint64_t slice_size;
    uint32_t pitch_texels;
    uint32_t pitch_bytes;
    uint32_t alignment;
    uint32_t padded[3];  // width, height in elements, depth or array size
    uint32_t elements[2];
    uint32_t texels[2];
    uint32_t depth_or_array;
    uint32_t samples;
    uint32_t swizzle;
    uint64_t bank_rotation[2];
    uint32_t slice_depth;
    uint32_t in_tail;
    uint32_t tail_position;  // where a tail mip starts in elements: x bits 0-10, y 11-21
};

struct PlaneOut {
    uint32_t usage;  // 1 data, 7 depth, 8 stencil (metadata planes are not made)
    uint64_t size;
    uint64_t slice_size;
    uint64_t offset;
    uint64_t alignment;
    uint32_t element_bytes;
    uint32_t standard_mips;
    uint32_t tail_mips;
    MipOut mips[kMips];
};

struct LayoutOut {
    uint64_t size;
    uint64_t alignment;
    uint32_t mips;
    uint32_t planes;
    PlaneOut plane[4];
    uint32_t dimension;
};
static_assert(sizeof(MipOut) == 0x70 && offsetof(MipOut, pitch_bytes) == 0x1c && offsetof(MipOut, bank_rotation) == 0x50);
static_assert(sizeof(PlaneOut) == 0x6c8 && offsetof(PlaneOut, mips) == 0x38);
static_assert(sizeof(LayoutOut) == 0x1b40 && offsetof(LayoutOut, plane) == 0x18 &&
              offsetof(LayoutOut, dimension) == 0x1b38);

struct PackedMips {
    uint8_t standard;
    uint8_t packed;
    uint32_t tiles;
    uint32_t start;
};
struct SubresourceTiling {
    uint32_t width;
    uint16_t height;
    uint16_t depth;
    uint32_t start;
};
static_assert(sizeof(PackedMips) == 12 && sizeof(SubresourceTiling) == 12);

struct Computer {
    const void* const* vtable;
    std::atomic<uint32_t> refs{1};
    bool fallback = false;  // the computer a failed XGCreateTextureComputer stores: every method fails
    ResourceDesc desc{};
    uint32_t mips = 0;
    g::ConsoleTexture texture;
};

// The console model's pipe tables for SW_64KB_R_X (VIctory console.rs:41), as the D3D12 layer picks them
// (d3d12_vulkan_device.cpp series_s_tiling): Series S with LEONIDA_DEVICE_TYPE=6 (XboxScarlettLockhart).
bool series_s_tiling() {
    static const bool series_s = [] {
        char text[8] = {};
        const DWORD n = GetEnvironmentVariableA("LEONIDA_DEVICE_TYPE", text, sizeof(text));
        return n && n < sizeof(text) && std::strtoul(text, nullptr, 10) == 6;
    }();
    return series_s;
}

// Prints once per entry point; `slot` is the vtable index for a computer method, or -1 for an export.
void warn_once(std::atomic<bool>& warned, const char* what, int slot, const void* caller) {
    if (warned.exchange(true, std::memory_order_relaxed)) return;
    char name[64];
    if (slot < 0) std::snprintf(name, sizeof(name), "%s", what);
    else std::snprintf(name, sizeof(name), "%s%d", what, slot);
    std::fprintf(stderr, "[rt] xg_xs: %s: not implemented; E_NOTIMPL (caller %s, logged once)\n", name,
                 rt::shims::module_offset(reinterpret_cast<uint64_t>(caller)).c_str());
}

// The first few of a kind of failure, with the description that failed.
void log_desc(std::atomic<int>& count, const char* what, const ResourceDesc& d, const void* caller) {
    constexpr int kMaxLogged = 8;
    if (count.fetch_add(1, std::memory_order_relaxed) >= kMaxLogged) return;
    std::fprintf(stderr,
                 "[rt] xg_xs: %s (dimension %u, %llux%u, depth/array %u, %u mips, format %u, %u samples, layout "
                 "0x%x, flags 0x%x; caller %s; first %d logged)\n",
                 what, d.dimension, static_cast<unsigned long long>(d.width), d.height, d.depth_or_array, d.mips,
                 d.format, d.samples, d.layout, d.flags,
                 rt::shims::module_offset(reinterpret_cast<uint64_t>(caller)).c_str(), kMaxLogged);
}

g::Dimension dimension_of(uint32_t dimension) {
    return dimension == kTexture1d ? g::Dimension::tex1d
           : dimension == kTexture3d ? g::Dimension::tex3d
                                     : g::Dimension::tex2d;
}

uint32_t suggested(const ResourceDesc& d) {
    return g::suggested_swizzle(dimension_of(d.dimension), (d.flags & kAllowDepthStencil) != 0,
                                (d.flags & kAllowRenderTarget) != 0);
}

// VIctory xg.rs texture(): the console layout of a description, or false.
bool texture_of(const ResourceDesc& d, g::ConsoleTexture& out) {
    if (d.dimension != kTexture1d && d.dimension != kTexture2d && d.dimension != kTexture3d) return false;
    if (d.width > UINT32_MAX) return false;
    g::ConsoleTextureDesc td;
    td.dimension = dimension_of(d.dimension);
    if (!g::swizzle_from_layout_word(d.layout, td.dimension, (d.flags & kAllowDepthStencil) != 0,
                                     (d.flags & kAllowRenderTarget) != 0, td.swizzle))
        return false;
    td.width = static_cast<uint32_t>(d.width);
    td.height = d.height;
    td.depth_or_array = d.depth_or_array;
    td.mips = d.mips;
    td.format = d.format;
    td.samples = std::max(d.samples, 1u);
    if (!g::ConsoleTexture::make(td, series_s_tiling(), out)) return false;
    return out.planes[0].layout.mips().size() <= kMips;
}

// ---- The computer's methods ----------------------------------------------------------------------------------------

ULONG __stdcall add_ref(Computer* c) {
    if (c->fallback) return 1;
    return c->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG __stdcall release(Computer* c) {
    if (c->fallback) return 1;
    const uint32_t left = c->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) delete c;  // the last reference is gone
    return left;
}

// The mode a tail mip is reported in: the 256-byte one of its kind (3 for the render kind, which GFX10 lacks), the
// depth mode itself.
uint32_t tail_swizzle(uint32_t mode) {
    switch (mode) {
    case 2: case 6: case 10: case 18: case 22: case 26: return g::swizzle::sw_256b_d;
    case 23: case 27: return 3;
    case 24: return mode;
    default: return g::swizzle::sw_256b_s;
    }
}

MipOut mip_out(const ResourceDesc& desc, const g::TextureLayout& l, size_t i) {
    const g::MipLayout& m = l.mips()[i];
    const uint64_t bpe = l.desc().element_bytes;
    const uint32_t block = l.desc().block;
    const bool three_d = desc.dimension == kTexture3d;
    const uint32_t array = std::max<uint32_t>(desc.depth_or_array, 1);
    MipOut out{};
    out.pitch_texels = m.pitch * block;
    out.pitch_bytes = m.pitch * l.desc().element_bytes;
    out.padded[0] = m.pitch;
    out.padded[1] = m.rows;
    out.padded[2] = three_d ? m.depth_pitch : array;
    out.elements[0] = m.width;
    out.elements[1] = m.height;
    out.texels[0] = std::max(static_cast<uint32_t>(desc.width >> i), 1u);
    out.texels[1] = std::max(desc.height >> i, 1u);
    out.depth_or_array = three_d ? m.depth : array;
    out.samples = l.desc().samples;
    out.slice_depth = l.block()[2];
    const uint64_t slice = uint64_t(m.pitch) * m.rows * bpe;
    const uint64_t depth = three_d ? m.depth_pitch : 1;
    if (!m.in_tail) {
        out.offset = m.offset;
        out.slice_size = slice * l.desc().samples;
        out.size = out.slice_size * depth;
        out.alignment = static_cast<uint32_t>(l.alignment());
        out.swizzle = l.desc().swizzle;
    } else {
        out.in_tail = 1;
        out.alignment = 256;
        out.swizzle = tail_swizzle(l.desc().swizzle);
        if (l.thick()) {
            // A 3D tail mip is placed by position, not by offset.
            out.slice_size = l.alignment() / l.block()[2];
            out.tail_position = m.tail_x | m.tail_y << 11;
        } else {
            out.offset = m.tail_offset;
            out.slice_size = slice;
        }
        out.size = out.slice_size * depth;
    }
    return out;
}

// 0x10: the whole layout. VIctory xg.rs layout().
HRESULT __stdcall layout(Computer* c, LayoutOut* out) {
    if (!out || c->fallback) return E_INVALIDARG;
    std::memset(out, 0, sizeof(*out));
    const auto& planes = c->texture.planes;
    out->size = c->texture.size;
    out->alignment = c->texture.alignment;
    out->mips = c->mips;
    out->planes = static_cast<uint32_t>(planes.size());
    out->dimension = c->desc.dimension;
    // The library calls a depth plane "depth" only beside stencil or without mips.
    const bool depth = (c->desc.flags & kAllowDepthStencil) && (planes.size() > 1 || c->mips == 1);
    for (size_t i = 0; i < planes.size() && i < 4; ++i) {
        const g::TexturePlane& p = planes[i];
        const g::TextureLayout& l = p.layout;
        PlaneOut& plane = out->plane[i];
        plane.usage = p.stencil ? 8 : depth ? 7 : 1;
        plane.size = l.size();
        plane.slice_size = l.slice_size();
        plane.offset = p.offset;
        plane.alignment = l.alignment();
        plane.element_bytes = l.desc().element_bytes;
        plane.standard_mips = l.first_tail_mip();
        plane.tail_mips = c->mips - l.first_tail_mip();
        for (size_t mip = 0; mip < l.mips().size() && mip < kMips; ++mip) plane.mips[mip] = mip_out(c->desc, l, mip);
    }
    return S_OK;
}

// 0x30 (plane, mip, x, y, z or slice, sample, bit position): an element's byte offset from the texture's start. VIctory
// measured the title reading texels of a 128x4 4KB_S texture on the CPU through it; an error returned as an offset made
// it read wild memory, so it is 0 outside.
uint64_t __stdcall element_offset(Computer* c, uint32_t plane, uint32_t mip, uint64_t x, uint32_t y, uint32_t z,
                                  uint32_t /*sample*/, uint32_t* bit) {
    if (bit) *bit = 0;
    if (c->fallback || plane >= c->texture.planes.size()) return 0;
    const g::TexturePlane& p = c->texture.planes[plane];
    const uint32_t x32 = x > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(x);
    uint64_t at = 0;
    if (!p.layout.offset(mip, x32, y, z, at)) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
            std::fprintf(stderr, "[rt] xg_xs: element offset outside its texture or without an address equation "
                                 "(mip %u, %llu, %u, %u; swizzle %u); 0 (first 8 logged)\n",
                         mip, static_cast<unsigned long long>(x), y, z, p.layout.desc().swizzle);
        return 0;
    }
    return p.offset + at;
}

// The plane, its mip and array slice for a subresource index, and the linear bytes the copy touches.
struct CopyArgs {
    const g::TexturePlane* plane = nullptr;
    uint32_t mip = 0, slice = 0;
    size_t linear = 0;
};

bool copy_args(const Computer& c, uint32_t plane, uint32_t subresource, uint32_t row, uint32_t depth, CopyArgs& a) {
    if (c.fallback || plane >= c.texture.planes.size() || !c.mips) return false;
    const g::TexturePlane& p = c.texture.planes[plane];
    a.plane = &p;
    a.mip = subresource % c.mips;
    a.slice = subresource / c.mips;
    if (a.mip >= p.layout.mips().size()) return false;
    const g::MipLayout& m = p.layout.mips()[a.mip];
    const size_t bytes = size_t(m.width) * p.layout.desc().element_bytes;
    // Extents are at most 16384 and pitches 32-bit, so this stays well inside 64 bits.
    a.linear = size_t(m.depth - 1) * depth + size_t(m.height - 1) * row + bytes;
    return true;
}

HRESULT copy_result(const g::TextureLayout& l, bool ok) {
    return ok ? S_OK : !l.addressable() ? kNotImpl : E_INVALIDARG;
}

// 0x40 (tiled base, plane, subresource, linear, row pitch, depth pitch).
HRESULT __stdcall copy_into(Computer* c, uint8_t* tiled, uint32_t plane, uint32_t subresource, const uint8_t* linear,
                            uint32_t row, uint32_t depth) {
    CopyArgs a;
    if (!copy_args(*c, plane, subresource, row, depth, a) || !tiled || !linear) return E_INVALIDARG;
    const g::TextureLayout& l = a.plane->layout;
    // The title's memory holds the whole texture and its rows.
    const bool ok = l.tile(std::span(reinterpret_cast<std::byte*>(tiled + a.plane->offset), size_t(l.size())), a.mip,
                           a.slice, std::span(reinterpret_cast<const std::byte*>(linear), a.linear), row, depth);
    if (!ok) {
        static std::atomic<int> logged{0};
        log_desc(logged, "copy into a texture failed", c->desc, _ReturnAddress());
    }
    return copy_result(l, ok);
}

// 0x48 (linear, plane, subresource, sample, tiled base, row pitch, depth pitch).
HRESULT __stdcall copy_from(Computer* c, uint8_t* linear, uint32_t plane, uint32_t subresource, uint32_t /*sample*/,
                            const uint8_t* tiled, uint32_t row, uint32_t depth) {
    CopyArgs a;
    if (!copy_args(*c, plane, subresource, row, depth, a) || !tiled || !linear) return E_INVALIDARG;
    const g::TextureLayout& l = a.plane->layout;
    const bool ok =
        l.detile(std::span(reinterpret_cast<const std::byte*>(tiled + a.plane->offset), size_t(l.size())), a.mip,
                 a.slice, std::span(reinterpret_cast<std::byte*>(linear), a.linear), row, depth);
    if (!ok) {
        static std::atomic<int> logged{0};
        log_desc(logged, "copy out of a texture failed", c->desc, _ReturnAddress());
    }
    return copy_result(l, ok);
}

// 0x50 (total tiles, packed mips, tile shape in texels, in: room / out: count, first, subresource tilings), in 64 KiB
// tiles of plane 0, for 2D 64 KiB modes only (others fail, as on the console). The mip tail is the packed part, tile 0
// of each slice; tilings list the standard mips of every slice in turn. With a tail the console's library lists two
// slices at most and zeroes all but the first; so does this (VIctory: the title reads slice 0 only).
HRESULT __stdcall tiling(Computer* c, uint32_t* tiles, PackedMips* packed, uint32_t (*shape)[3], uint32_t* count,
                         uint32_t first, SubresourceTiling* out) {
    if (c->fallback || c->texture.planes.empty()) return E_INVALIDARG;
    const g::TextureLayout& l = c->texture.planes[0].layout;
    if (l.alignment() != kTile) return E_FAIL;
    if (l.thick()) return kNotImpl;
    const uint32_t standard = l.first_tail_mip();
    const bool tail = standard < c->mips;
    const uint64_t tail_tiles = standard == 0       ? l.slice_size() / kTile
                                : standard < c->mips ? l.mips()[standard - 1].offset / kTile
                                                     : 0;
    const uint32_t block = l.desc().block;
    if (tiles) *tiles = static_cast<uint32_t>(l.size() / kTile);
    if (packed)
        *packed = {static_cast<uint8_t>(standard), static_cast<uint8_t>(c->mips - standard),
                   static_cast<uint32_t>(tail_tiles), 0};
    if (shape) {
        (*shape)[0] = l.block()[0] * block;
        (*shape)[1] = l.block()[1] * block;
        (*shape)[2] = l.block()[2];
    }
    if (count) {
        const uint32_t slices = tail ? std::min(l.slices(), 2u) : l.slices();
        const uint64_t all = uint64_t(standard) * slices;
        const uint32_t wanted =
            static_cast<uint32_t>(std::min<uint64_t>(all > first ? all - first : 0, *count));
        for (uint32_t i = 0; i < wanted && out; ++i) {
            const uint32_t slice = (first + i) / standard, mip = (first + i) % standard;
            const g::MipLayout& m = l.mips()[mip];
            SubresourceTiling entry{};
            if (!(tail && slice > 0)) {
                entry.width = m.pitch / l.block()[0];
                entry.height = static_cast<uint16_t>(m.rows / l.block()[1]);
                entry.depth = 1;
                entry.start = static_cast<uint32_t>((uint64_t(slice) * l.slice_size() + m.offset) / kTile);
            }
            out[i] = entry;
        }
        *count = wanted;
    }
    return S_OK;
}

// Slots the title is not seen calling (VIctory: size, alignment, offsets, coordinates, view descriptor, tiled-resource
// query, and anything past them).
template <size_t Slot>
HRESULT __stdcall unused_slot(Computer*) {
    static std::atomic<bool> warned{false};
    warn_once(warned, "texture computer slot ", static_cast<int>(Slot), _ReturnAddress());
    return kNotImpl;
}

template <size_t Slot>
void* slot_entry() {
    if constexpr (Slot == 0) return reinterpret_cast<void*>(&add_ref);
    else if constexpr (Slot == 1) return reinterpret_cast<void*>(&release);
    else if constexpr (Slot == 2) return reinterpret_cast<void*>(&layout);
    else if constexpr (Slot == 6) return reinterpret_cast<void*>(&element_offset);
    else if constexpr (Slot == 8) return reinterpret_cast<void*>(&copy_into);
    else if constexpr (Slot == 9) return reinterpret_cast<void*>(&copy_from);
    else if constexpr (Slot == 10) return reinterpret_cast<void*>(&tiling);
    else return reinterpret_cast<void*>(&unused_slot<Slot>);
}

template <size_t... Slot>
std::array<void*, sizeof...(Slot)> make_vtable(std::index_sequence<Slot...>) {
    return {slot_entry<Slot>()...};
}

const std::array<void*, kSlots> kVtable = make_vtable(std::make_index_sequence<kSlots>{});

Computer g_fallback{kVtable.data(), {1}, true};

// ---- Exports -------------------------------------------------------------------------------------------------------

// (desc, flags, out layout word); the title passes flags 0 and puts the result in the description's layout field.
HRESULT __stdcall XGSuggestSwizzleMode(const ResourceDesc* desc, uint32_t /*flags*/, uint32_t* out) {
    if (!desc || !out) return E_INVALIDARG;
    *out = kSwizzled | suggested(*desc);
    return S_OK;
}

HRESULT __stdcall XGCreateTextureComputer(const ResourceDesc* desc, Computer** out) {
    if (!out) return E_INVALIDARG;
    *out = &g_fallback;  // see the top of the file: two measured callers use *out without checking the HRESULT
    if (!desc) return E_INVALIDARG;
    Computer* c = nullptr;
    try {
        c = new Computer{kVtable.data()};
        c->desc = *desc;
        if (!texture_of(*desc, c->texture)) {
            static std::atomic<int> logged{0};
            log_desc(logged, "XGCreateTextureComputer: no console layout for the description; E_INVALIDARG", *desc,
                     _ReturnAddress());
            delete c;
            return E_INVALIDARG;
        }
    } catch (const std::bad_alloc&) {
        delete c;
        return E_OUTOFMEMORY;
    }
    c->mips = static_cast<uint32_t>(c->texture.planes[0].layout.mips().size());
    *out = c;
    return S_OK;
}

}  // namespace

SHIM_TABLE(xg_xs,
    {"xg_xs.dll", "XGCreateTextureComputer", reinterpret_cast<void*>(&XGCreateTextureComputer)},
    {"xg_xs.dll", "XGSuggestSwizzleMode", reinterpret_cast<void*>(&XGSuggestSwizzleMode)},
);
