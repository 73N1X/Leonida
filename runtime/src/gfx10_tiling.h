#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace leonida::gfx10 {

enum class TileMode { sw_4kb_s, sw_64kb_d };

namespace tiling_detail {

// Mesa addrlib GFX10_SW_4K_S_RBPLUS_PATINFO / GFX10_SW_64K_D_RBPLUS_PATINFO;
// tools/ttpf_mips.py patterns, corpus validation in docs/textures.md. Zero bits address bytes within an element.
inline constexpr int patterns_4k[5][12] = {
    {1, 2, 3, 4, -1, -2, -3, -4, -5, 5, -6, 6},
    {0, 1, 2, 3, -1, -2, -3, 4, -4, 5, -5, 6},
    {0, 0, 1, 2, -1, -2, -3, 3, -4, 4, -5, 5},
    {0, 0, 0, 1, -1, -2, 2, 3, -3, 4, -4, 5},
    {0, 0, 0, 0, -1, -2, 1, 2, -3, 3, -4, 4}};
inline constexpr int patterns_64k[5][16] = {
    {1, 2, 3, -2, -1, -3, 4, -4, -5, 5, -6, 6, -7, 7, -8, 8},
    {0, 1, 2, 3, -1, -2, -3, 4, -4, 5, -5, 6, -6, 7, -7, 8},
    {0, 0, 1, 2, -1, -2, 3, -3, -4, 4, -5, 5, -6, 6, -7, 7},
    {0, 0, 0, 1, -1, 2, 3, -2, -3, 4, -4, 5, -5, 6, -6, 7},
    {0, 0, 0, 0, 1, -1, 2, -2, -3, 3, -4, 4, -5, 5, -6, 6}};

inline bool overlap(std::span<const std::byte> source, std::span<std::byte> destination) {
    const auto a = reinterpret_cast<std::uintptr_t>(source.data());
    const auto b = reinterpret_cast<std::uintptr_t>(destination.data());
    return a <= b ? b - a < source.size() : a - b < destination.size();
}

}  // namespace tiling_detail

// One standalone 2D level, no mip tail or tile XOR. BC formats use a compressed block as one element.
class TiledSurface2D {
public:
    static bool make(TileMode mode, std::size_t element_bytes, std::size_t width, std::size_t height,
                     TiledSurface2D& out) {
        if (!width || !height) return false;
        unsigned index = 0;
        switch (element_bytes) {
        case 1: break;
        case 2: index = 1; break;
        case 4: index = 2; break;
        case 8: index = 3; break;
        case 16: index = 4; break;
        default: return false;
        }
        TiledSurface2D next;
        switch (mode) {
        case TileMode::sw_4kb_s:
            next.pattern_ = tiling_detail::patterns_4k[index];
            next.bits_ = 12;
            break;
        case TileMode::sw_64kb_d:
            next.pattern_ = tiling_detail::patterns_64k[index];
            next.bits_ = 16;
            break;
        default: return false;
        }
        unsigned x_bits = 0, y_bits = 0;
        for (unsigned bit = 0; bit < next.bits_; ++bit) {
            x_bits += next.pattern_[bit] > 0;
            y_bits += next.pattern_[bit] < 0;
        }
        next.tile_width_ = std::size_t{1} << x_bits;
        next.tile_height_ = std::size_t{1} << y_bits;
        next.tiles_x_ = (width - 1) / next.tile_width_ + 1;
        const std::size_t tiles_y = (height - 1) / next.tile_height_ + 1;
        const std::size_t tile_bytes = std::size_t{1} << next.bits_;
        constexpr auto max = std::numeric_limits<std::size_t>::max();
        if (next.tiles_x_ > max / tiles_y || next.tiles_x_ * tiles_y > max / tile_bytes) return false;
        next.size_ = next.tiles_x_ * tiles_y * tile_bytes;
        next.width_ = width;
        next.height_ = height;
        next.element_bytes_ = element_bytes;
        out = next;
        return true;
    }

    std::size_t tiled_size() const { return size_; }
    std::size_t tile_width() const { return tile_width_; }
    std::size_t tile_height() const { return tile_height_; }

    bool element_offset(std::size_t x, std::size_t y, std::size_t& offset) const {
        if (!size_ || x >= width_ || y >= height_) return false;
        offset = address(x, y);
        return true;
    }

    // Buffers must not overlap. Padding in either destination is left untouched; failure writes nothing.
    bool detile(std::span<const std::byte> tiled, std::span<std::byte> linear, std::size_t row_pitch) const {
        return copy(tiled, linear, row_pitch, false);
    }

    bool tile(std::span<const std::byte> linear, std::span<std::byte> tiled, std::size_t row_pitch) const {
        return copy(linear, tiled, row_pitch, true);
    }

private:
    std::size_t address(std::size_t x, std::size_t y) const {
        std::size_t offset = ((y / tile_height_) * tiles_x_ + x / tile_width_) << bits_;
        for (unsigned bit = 0; bit < bits_; ++bit) {
            const int source = pattern_[bit];
            if (source > 0) offset |= ((x >> (source - 1)) & 1) << bit;
            else if (source < 0) offset |= ((y >> (-source - 1)) & 1) << bit;
        }
        return offset;
    }

    bool copy(std::span<const std::byte> source, std::span<std::byte> destination, std::size_t pitch,
              bool to_tiled) const {
        if (!size_) return false;
        const std::size_t row_bytes = width_ * element_bytes_;
        if (pitch < row_bytes || (height_ - 1 &&
            pitch > (std::numeric_limits<std::size_t>::max() - row_bytes) / (height_ - 1))) return false;
        const std::size_t linear_size = (height_ - 1) * pitch + row_bytes;
        if (source.size() < (to_tiled ? linear_size : size_) ||
            destination.size() < (to_tiled ? size_ : linear_size) ||
            tiling_detail::overlap(source, destination)) return false;
        const auto tile_x_bits = std::countr_zero(tile_width_);
        std::size_t x_offsets[256]{};
        for (std::size_t x = 0; x < tile_width_; ++x) {
            for (unsigned bit = 0; bit < bits_; ++bit) {
                const int source_bit = pattern_[bit];
                if (source_bit > 0) x_offsets[x] |= ((x >> (source_bit - 1)) & 1) << bit;
            }
        }
        for (std::size_t y = 0; y < height_; ++y) {
            std::size_t y_offset = 0;
            for (unsigned bit = 0; bit < bits_; ++bit) {
                const int source_bit = pattern_[bit];
                if (source_bit < 0) y_offset |= ((y >> (-source_bit - 1)) & 1) << bit;
            }
            const std::size_t tile_row = (y / tile_height_) * tiles_x_;
            for (std::size_t x = 0; x < width_; ++x) {
                const std::size_t linear = y * pitch + x * element_bytes_;
                const std::size_t tiled = ((tile_row + (x >> tile_x_bits)) << bits_) +
                                          y_offset + x_offsets[x & (tile_width_ - 1)];
                std::memcpy(destination.data() + (to_tiled ? tiled : linear),
                            source.data() + (to_tiled ? linear : tiled), element_bytes_);
            }
        }
        return true;
    }

    const int* pattern_ = nullptr;
    unsigned bits_ = 0;
    std::size_t width_ = 0, height_ = 0, element_bytes_ = 0;
    std::size_t tile_width_ = 0, tile_height_ = 0, tiles_x_ = 0, size_ = 0;
};

// ---- Whole textures: mips, mip tails, array slices, 3D, depth/stencil planes ----------------------------------------
//
// Ported from VIctory (GPL-3.0-or-later, Marvisak and contributors, DexNom fork), victory-formats texture.rs, which
// follows AMD's address library (addrlib, as shipped in Mesa under src/amd/addrlib: block dimensions, mip placement
// and mip tails from gfx10addrlib.cpp and addrlib2.cpp, the 256-byte micro tile patterns from gfx10SwizzlePattern.h,
// the RB+ tables as on RDNA2). addrlib carries this notice:
//
//   Copyright (C) 2007-2026 Advanced Micro Devices, Inc. All rights reserved.
//   SPDX-License-Identifier: MIT
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
//   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
//   to permit persons to whom the Software is furnished to do so, subject to the following conditions: the above
//   copyright notice and this permission notice shall be included in all copies or substantial portions of the
//   Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
//
// Units: an element is a texel, or a 4x4 block of a block-compressed format. Mips of one array slice (or one depth slab
// of a thick 3D texture) are stored together, smallest first: the mip tail (the small mips sharing one block) at
// offset 0, mip 0 last. Addressable: linear, and the S and D modes without pipe/bank XOR (256 B, 4 KiB, 64 KiB, thin
// and thick); SW_64KB_R_X for one-mip, one-slice, one-sample 2D textures of 2-, 4- or 8-byte elements (Series X or S
// pipe tables). Other XOR modes and MSAA layouts are sized but have no address equation (addressable() is false).

// A swizzle mode by its hardware number (addrlib's ADDR_SW_* order).
namespace swizzle {
inline constexpr std::uint32_t linear = 0, sw_256b_s = 1, sw_256b_d = 2, sw_4kb_s = 5, sw_4kb_d = 6, sw_64kb_s = 9,
                               sw_64kb_d = 10, sw_64kb_s_t = 17, sw_64kb_d_t = 18, sw_4kb_s_x = 21, sw_4kb_d_x = 22,
                               sw_4kb_r_x = 23, sw_64kb_z_x = 24, sw_64kb_s_x = 25, sw_64kb_d_x = 26,
                               sw_64kb_r_x = 27;
// log2 of the block size in bytes (0 for linear), or -1 for a mode GFX10 does not have.
constexpr int block_log2(std::uint32_t mode) {
    switch (mode) {
    case 0: return 0;
    case 1: case 2: return 8;
    case 5: case 6: case 21: case 22: case 23: return 12;
    case 9: case 10: case 17: case 18: case 24: case 25: case 26: case 27: return 16;
    default: return -1;
    }
}
constexpr bool standard(std::uint32_t mode) { return mode == 1 || mode == 5 || mode == 9 || mode == 17 || mode == 21 || mode == 25; }
constexpr bool display(std::uint32_t mode) { return mode == 2 || mode == 6 || mode == 10 || mode == 18 || mode == 22 || mode == 26; }
constexpr bool z_order(std::uint32_t mode) { return mode == 24; }
// Pipe and bank XOR modes (_T, _X): their addresses depend on the GPU's pipe configuration.
constexpr bool xor_mode(std::uint32_t mode) { return mode >= 16; }
}  // namespace swizzle

enum class Dimension : std::uint8_t { tex1d, tex2d, tex3d };

// One plane of a texture.
struct LayoutDesc {
    Dimension dimension = Dimension::tex2d;
    std::uint32_t width = 1, height = 1;  // texels
    std::uint32_t depth = 1;              // depth of a 3D texture, else the array size
    std::uint32_t mips = 1;
    std::uint32_t element_bytes = 4;      // 1, 2, 4, 8 or 16
    std::uint32_t block = 1;              // texels per element side: 1, or 4 for block compression
    std::uint32_t samples = 1;
    std::uint32_t swizzle = swizzle::linear;
};

// Where one mip lives inside a slice.
struct MipLayout {
    std::uint32_t width = 0, height = 0, depth = 0;  // elements (depth 1 unless the texture is 3D)
    // Padded size in elements: whole blocks, or the tail's share of one.
    std::uint32_t pitch = 0, rows = 0, depth_pitch = 0;
    std::uint64_t offset = 0;  // byte offset of its first block in the slice (0 in the tail)
    // In the mip tail: byte offset inside the tail block and the element position the mip starts at.
    bool in_tail = false;
    std::uint32_t tail_offset = 0, tail_x = 0, tail_y = 0;
};

class TextureLayout {
public:
    // False for a description that makes no texture (zero or oversized extent, a bad element size, a mode this
    // dimension cannot use, too many mips). `series_s` picks the Series S pipe tables of SW_64KB_R_X.
    static bool make(const LayoutDesc& d, bool series_s, TextureLayout& out) {
        const int block_log2 = swizzle::block_log2(d.swizzle);
        const auto in = [](std::uint32_t v) { return v >= 1 && v <= kMaxExtent; };
        const bool valid =
            block_log2 >= 0 && in(d.width) && in(d.height) && in(d.depth) && d.mips >= 1 && d.mips <= kMaxMips &&
            std::has_single_bit(d.element_bytes) && d.element_bytes <= 16 && (d.block == 1 || d.block == 4) &&
            std::has_single_bit(d.samples) && d.samples <= 8 &&
            (d.samples == 1 || (d.dimension == Dimension::tex2d && d.mips == 1)) &&
            (d.dimension != Dimension::tex1d || d.height == 1) &&
            (d.dimension != Dimension::tex3d || d.swizzle == 0 || d.swizzle == 5 || d.swizzle == 9 || d.swizzle == 17 ||
             d.swizzle == 21 || (d.swizzle >= 24 && d.swizzle <= 27));
        if (!valid) return false;
        TextureLayout layout;
        if (block_log2 == 0) layout.linear(d);
        else layout.tiled(d, std::uint32_t(block_log2), series_s);
        out = std::move(layout);
        return true;
    }

    const LayoutDesc& desc() const { return desc_; }
    // Block dimensions in elements (linear: the pitch alignment, 1, 1).
    const std::array<std::uint32_t, 3>& block() const { return block_; }
    bool thick() const { return thick_; }  // 3D blocks span block()[2] depth slices (GFX10 3D S and D modes)
    // Array slices, depth slabs of block()[2] slices (thick), or depth slices (thin and linear 3D).
    std::uint32_t slices() const { return slices_; }
    std::uint64_t slice_size() const { return slice_size_; }  // bytes of one slice: its whole mip chain
    std::uint64_t size() const { return size_; }
    std::uint64_t alignment() const { return alignment_; }
    std::uint32_t first_tail_mip() const { return first_tail_mip_; }  // mips().size() without a tail
    const std::vector<MipLayout>& mips() const { return mips_; }

    // Whether offsets and copies have a known address equation.
    bool addressable() const { return desc_.swizzle == swizzle::linear || !pattern_[0].empty(); }

    // Byte offset of element (x, y) of mip `mip` in array slice or depth slice `z`; false outside the mip or for a
    // layout that is not addressable.
    bool offset(std::uint32_t mip, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint64_t& out) const {
        if (mip >= mips_.size() || !addressable()) return false;
        const MipLayout& m = mips_[mip];
        const std::uint32_t depth = desc_.dimension == Dimension::tex3d ? m.depth : slices_;
        if (x >= m.width || y >= m.height || z >= depth) return false;
        const auto [slice, inner] = split(z);
        const std::uint64_t base = std::uint64_t(slice) * slice_size_ + m.offset;
        if (desc_.swizzle == swizzle::linear) {
            out = base + (std::uint64_t(y) * m.pitch + x) * desc_.element_bytes;
        } else if (desc_.swizzle == swizzle::sw_64kb_r_x) {
            out = render_xor_offset(m, x, y);
        } else {
            out = base + row(m, y, inner) + column(m, x);
        }
        return true;
    }

    // The bytes from the texture's start that mip `first` and the smaller ones take: the smaller a mip, the nearer
    // the start it lies (a title that streams textures has memory for the smaller mips only). All of the layout
    // where slices follow each other.
    std::uint64_t bytes_from(std::uint32_t first) const {
        if (first == 0 || slices_ != 1 || thick_ || desc_.dimension == Dimension::tex3d || desc_.samples != 1 ||
            first >= desc_.mips)
            return size_;
        std::uint64_t most = 0;
        for (std::size_t i = first; i < mips_.size(); ++i) {
            const MipLayout& m = mips_[i];
            most = std::max(most, m.in_tail ? alignment_
                                            : m.offset + std::uint64_t(m.pitch) * m.rows * desc_.element_bytes);
        }
        return std::min(most ? most : size_, size_);
    }

    // One subresource out of tiled memory (the whole texture, size() bytes, or bytes_from(mip)) into linear rows: mip
    // `mip` of array slice `slice` (3D: every depth slice of the mip, `slice` 0). Rows are `row_pitch` bytes apart,
    // depth slices `depth_pitch`. False, with nothing written past what was copied, for a bad mip or slice, a layout
    // that is not addressable, or buffers too small.
    bool detile(std::span<const std::byte> tiled, std::uint32_t mip, std::uint32_t slice, std::span<std::byte> linear,
                std::size_t row_pitch, std::size_t depth_pitch) const {
        return walk(tiled.size(), mip, slice, linear.size(), row_pitch, depth_pitch,
                    [&](std::size_t t, std::size_t l, std::size_t n) { std::memcpy(linear.data() + l, tiled.data() + t, n); });
    }

    // The reverse of detile: linear rows into tiled memory.
    bool tile(std::span<std::byte> tiled, std::uint32_t mip, std::uint32_t slice, std::span<const std::byte> linear,
              std::size_t row_pitch, std::size_t depth_pitch) const {
        return walk(tiled.size(), mip, slice, linear.size(), row_pitch, depth_pitch,
                    [&](std::size_t t, std::size_t l, std::size_t n) { std::memcpy(tiled.data() + t, linear.data() + l, n); });
    }

private:
    static constexpr std::uint32_t kMaxMips = 16;
    // GFX10's image descriptors hold at most 16384 texels per side; the bound also keeps every size computed here in
    // range for values a title passes unchecked.
    static constexpr std::uint32_t kMaxExtent = 1u << 14;

    static constexpr std::uint32_t div_ceil(std::uint32_t a, std::uint32_t b) { return (a + b - 1) / b; }
    static constexpr std::uint32_t round_up(std::uint32_t a, std::uint32_t b) { return div_ceil(a, b) * b; }
    // Elements along one side of mip `mip`, as data and copies see it: texels halve rounding down (to at least 1),
    // then round up to whole elements.
    static constexpr std::uint32_t dim(std::uint32_t texels, std::uint32_t mip, std::uint32_t block) {
        return div_ceil(std::max(texels >> mip, 1u), block);
    }
    // Elements along one side for placement (padding, tail, offsets): addrlib halves mip 0's elements rounding up,
    // which can be one more than dim() for odd sizes.
    static constexpr std::uint32_t padded_dim(std::uint32_t texels, std::uint32_t mip, std::uint32_t block) {
        return div_ceil(div_ceil(std::max(texels, 1u), block), 1u << mip);
    }

    // 256-byte micro tile patterns by log2 of the element size: for each address bit, the element coordinate bit it
    // takes, "--" for the bits inside an element. Thin S, thin D, thick S (also the D 3D modes), from addrlib's
    // GFX10_SW_256_S_RBPLUS, GFX10_SW_256_D_RBPLUS and GFX10_SW_4K_S3_RBPLUS; larger blocks add Y, X (thin) or Y, X, Z
    // (thick) in turn.
    static constexpr const char* kMicro[3][5] = {
        {"x0x1x2x3y0y1y2y3", "--x0x1x2y0y1y2x3", "----x0x1y0y1y2x2", "------x0y0y1x1x2", "--------y0y1x0x1"},
        {"x0x1x2y1y0y2x3y3", "--x0x1x2y0y1y2x3", "----x0x1y0y1x2y2", "------x0y0x1x2y1", "--------x0y0x1y1"},
        {"x0x1z0y0z1y1x2z2", "--x0z0y0z1y1x1z2", "----x0y0z0y1x1z1", "------x0z0y0x1z1", "--------z0y0x0z1"},
    };
    static constexpr std::uint32_t kBlock256_2d[5][2] = {{16, 16}, {16, 8}, {8, 8}, {8, 4}, {4, 4}};
    static constexpr std::uint32_t kBlock256_3d[5][3] = {{8, 4, 8}, {4, 4, 8}, {4, 4, 4}, {4, 2, 4}, {2, 2, 4}};
    static constexpr std::uint32_t kBlock1k_3d[5][3] = {{16, 8, 8}, {8, 8, 8}, {8, 8, 4}, {8, 4, 4}, {4, 4, 4}};

    // For each axis, the address bit each coordinate bit lands on.
    static std::array<std::vector<std::uint32_t>, 3> pattern(int kind, std::uint32_t element_log2,
                                                             std::uint32_t block_log2) {
        std::array<std::vector<std::uint32_t>, 3> axes;
        const char* micro = kMicro[kind][element_log2];
        for (std::uint32_t bit = 0; bit < 8; ++bit) {
            const char axis = micro[2 * bit];
            const int to = axis == 'x' ? 0 : axis == 'y' ? 1 : axis == 'z' ? 2 : -1;
            if (to < 0) continue;
            const std::size_t index = std::size_t(micro[2 * bit + 1] - '0');
            if (axes[to].size() <= index) axes[to].resize(index + 1, 0);
            axes[to][index] = bit;
        }
        static constexpr int thin[2] = {1, 0}, thick_order[3] = {1, 0, 2};
        for (std::uint32_t bit = 8, i = 0; bit < block_log2; ++bit, ++i)
            axes[kind == 2 ? thick_order[i % 3] : thin[i % 2]].push_back(bit);
        return axes;
    }

    // Byte offsets inside a block for coordinates 0..2^bits of one axis.
    static std::vector<std::uint32_t> spread(const std::vector<std::uint32_t>& bits) {
        std::vector<std::uint32_t> out(std::size_t(1) << bits.size());
        for (std::uint32_t c = 0; c < out.size(); ++c)
            for (std::size_t i = 0; i < bits.size(); ++i) out[c] |= ((c >> i) & 1u) << bits[i];
        return out;
    }

    // AMD GFX10_SW_64K_R_X_1xaa_RBPLUS with no additional base pipe XOR: the in-block byte offset each coordinate bit
    // toggles. 32 pipes / 8 packers on Series X, 8 / 4 on Series S.
    static std::array<std::vector<std::uint32_t>, 3> render_xor_pattern(bool series_s, std::uint32_t element_bytes) {
        std::vector<std::uint16_t> x, y;
        if (!series_s && element_bytes == 2) {
            x = {2, 4, 8, 0x80, 0x200, 0x800, 0x1400, 0x8100, 0x1000};
            y = {0x10, 0x20, 0x40, 0x2000, 0x300, 0x400, 0x4800, 0x8100, 0x1000};
        } else if (!series_s && element_bytes == 4) {
            x = {4, 8, 0x40, 0x2000, 0x200, 0x800, 0x1400, 0x8100, 0x1000};
            y = {0x10, 0x20, 0x80, 0x4000, 0x300, 0x400, 0x8800, 0x100, 0x1000};
        } else if (!series_s && element_bytes == 8) {
            x = {8, 0x20, 0x40, 0x4000, 0x200, 0x800, 0x1400, 0x8100, 0x1000};
            y = {0x10, 0x80, 0x2000, 0x8000, 0x300, 0x400, 0x800, 0x100, 0x1000};
        } else if (series_s && element_bytes == 2) {
            x = {2, 4, 8, 0x80, 0x200, 0x1400, 0x2100, 0x8000, 0x4000};
            y = {0x10, 0x20, 0x40, 0x800, 0x300, 0x400, 0x4100, 0x8000, 0x2000};
        } else if (series_s && element_bytes == 4) {
            x = {4, 8, 0x40, 0x800, 0x200, 0x2400, 0x4100, 0x8000};
            y = {0x10, 0x20, 0x80, 0x1000, 0x300, 0x400, 0x8100, 0x4000, 0x2000};
        } else if (series_s && element_bytes == 8) {
            x = {8, 0x20, 0x40, 0x1000, 0x200, 0x4400, 0x8100, 0x2000};
            y = {0x10, 0x80, 0x800, 0x2000, 0x300, 0x400, 0x8100, 0x4000};
        } else {
            return {};
        }
        const auto contributions = [](const std::vector<std::uint16_t>& basis) {
            std::vector<std::uint32_t> offsets(std::size_t(1) << basis.size(), 0);
            for (std::size_t bit = 0; bit < basis.size(); ++bit) {
                const std::size_t n = std::size_t(1) << bit;
                for (std::size_t i = 0; i < n; ++i) offsets[n + i] = offsets[i] ^ basis[bit];
            }
            return offsets;
        };
        return {contributions(x), contributions(y), {}};
    }

    // HwlComputeSurfaceInfoLinear: rows aligned to 256 bytes, mips packed smallest first, a 3D texture's depth slices
    // each hold the chain.
    void linear(const LayoutDesc& d) {
        desc_ = d;
        const std::uint64_t bpe = d.element_bytes;
        const std::uint32_t align = 256 / d.element_bytes;
        const bool three_d = d.dimension == Dimension::tex3d;
        std::uint64_t offset = 0;
        mips_.resize(d.mips);
        for (std::uint32_t i = d.mips; i-- > 0;) {
            MipLayout& m = mips_[i];
            const std::uint32_t h = padded_dim(d.height, i, d.block);
            m.pitch = round_up(padded_dim(d.width, i, d.block), align);
            m.width = dim(d.width, i, d.block);
            m.height = dim(d.height, i, d.block);
            m.depth = three_d ? dim(d.depth, i, 1) : 1;
            m.rows = h;
            m.depth_pitch = three_d ? d.depth : 1;
            m.offset = offset;
            offset += std::uint64_t(m.pitch) * h * bpe;
        }
        block_ = {align, 1, 1};
        thick_ = false;
        slices_ = d.depth;
        slice_size_ = offset;
        size_ = offset * d.depth;
        alignment_ = 256;
        first_tail_mip_ = d.mips;
    }

    void tiled(const LayoutDesc& d, std::uint32_t block_log2, bool series_s) {
        desc_ = d;
        const std::uint32_t element_log2 = std::uint32_t(std::countr_zero(d.element_bytes));
        const std::uint64_t bpe = d.element_bytes;
        const std::uint64_t block_bytes = std::uint64_t(1) << block_log2;
        const bool three_d = d.dimension == Dimension::tex3d;
        thick_ = three_d && (swizzle::standard(d.swizzle) || swizzle::display(d.swizzle));
        if (thick_) {  // ComputeThickBlockDimension
            const std::uint32_t amp = (block_log2 - 10) / 3, rest = (block_log2 - 10) % 3;
            const auto& b = kBlock1k_3d[element_log2];
            block_ = {b[0] << amp, b[1] << (amp + rest / 2), b[2] << (amp + (rest != 0 ? 1 : 0))};
        } else {  // ComputeThinBlockDimension
            const std::uint32_t samples_log2 = std::uint32_t(std::countr_zero(d.samples));
            const std::uint32_t elements_log2 = block_log2 - element_log2 - samples_log2;
            const bool wide = (samples_log2 & 1) == 0 || (block_log2 & 1) != 0;
            const std::uint32_t w = (elements_log2 + (wide ? 1 : 0)) / 2;
            block_ = {1u << w, 1u << (elements_log2 - w), 1};
        }
        std::uint32_t slices = round_up(d.depth, block_[2]);
        const auto mip_depth = [&](std::uint32_t i) { return thick_ ? padded_dim(d.depth, i, 1) : 1u; };
        const auto depth_of = [&](std::uint32_t i) { return three_d ? dim(d.depth, i, 1) : 1u; };
        first_tail_mip_ = d.mips;
        std::uint64_t slice_size = 0;
        if (d.mips == 1) {
            MipLayout m;
            m.width = dim(d.width, 0, d.block);
            m.height = dim(d.height, 0, d.block);
            m.depth = depth_of(0);
            m.pitch = round_up(m.width, block_[0]);
            m.rows = round_up(m.height, block_[1]);
            m.depth_pitch = three_d ? slices : 1;
            mips_.push_back(m);
            slice_size = std::uint64_t(m.pitch) * m.rows * bpe * d.samples;
        } else {
            // GetMipTailDim: the largest mip the tail takes.
            std::array<std::uint32_t, 3> tail_max = block_;
            if (thick_) {
                switch (block_log2 % 3) {
                case 0: tail_max[1] >>= 1; break;
                case 1: tail_max[0] >>= 1; break;
                default: tail_max[2] >>= 1; break;
                }
            } else {
                tail_max[0] >>= 1;
            }
            // GetMaxNumMipsInTail; 256-byte blocks have no tail (ComputeSurfaceInfoMicroTiled).
            const std::uint32_t effective = thick_ ? block_log2 - (block_log2 - 8) / 3 : block_log2;
            const std::uint32_t max_in_tail = effective == 8                     ? 0
                                              : effective >= 9 && effective <= 11 ? 1 + (1u << (effective - 9))
                                                                                  : effective - 4;
            // dsMipmapHtileFix: 8- and 16-bit depth enter the tail as 32-bit.
            std::array<std::uint32_t, 3> fixed = tail_max;
            if (swizzle::z_order(d.swizzle) && element_log2 <= 1) {
                fixed[0] /= kBlock256_2d[element_log2][0] / kBlock256_2d[2][0];
                fixed[1] /= kBlock256_2d[element_log2][1] / kBlock256_2d[2][1];
            }
            std::vector<std::uint64_t> sizes;
            for (std::uint32_t i = 0; i < d.mips; ++i) {
                const std::uint32_t w = padded_dim(d.width, i, d.block), h = padded_dim(d.height, i, d.block);
                if (w <= fixed[0] && h <= fixed[1] && d.mips - i <= max_in_tail) {
                    first_tail_mip_ = i;
                    slice_size += block_bytes / block_[2];
                    break;
                }
                MipLayout m;
                m.pitch = round_up(w, block_[0]);
                m.rows = round_up(h, block_[1]);
                const std::uint32_t depth = round_up(mip_depth(i), block_[2]);
                const std::uint64_t one = std::uint64_t(m.pitch) * m.rows * bpe;
                slice_size += one;
                sizes.push_back(one * block_[2]);
                m.width = dim(d.width, i, d.block);
                m.height = dim(d.height, i, d.block);
                m.depth = depth_of(i);
                m.depth_pitch = three_d ? depth : 1;
                mips_.push_back(m);
            }
            // Standard mips from the smallest up, after the tail block.
            std::uint64_t offset = first_tail_mip_ < d.mips ? block_bytes : 0;
            for (std::uint32_t i = first_tail_mip_; i-- > 0;) {
                mips_[i].offset = offset;
                offset += sizes[i];
            }
            const std::uint32_t micro_w = thick_ ? kBlock256_3d[element_log2][0] : kBlock256_2d[element_log2][0];
            const std::uint32_t micro_h = thick_ ? kBlock256_3d[element_log2][1] : kBlock256_2d[element_log2][1];
            std::uint32_t pitch = tail_max[0], rows = tail_max[1];
            for (std::uint32_t i = first_tail_mip_; i < d.mips; ++i) {
                const std::uint32_t m = max_in_tail - 1 - (i - first_tail_mip_);
                const std::uint32_t tail_offset = m > 6 ? 16u << m : m << 8;
                // Address bits 8.. of the offset, odd ones x, even ones y.
                const auto bits = [&](std::uint32_t from) {
                    std::uint32_t v = 0;
                    for (std::uint32_t b = 0; b < 6; ++b) v |= ((tail_offset >> (from + 2 * b)) & 1u) << b;
                    return v;
                };
                std::uint32_t x = bits(9), y = bits(8);
                if (block_log2 & 1) {
                    std::swap(x, y);
                    if (element_log2 & 1) {
                        y = (y << 1) | (x & 1);
                        x >>= 1;
                    }
                }
                MipLayout mip;
                mip.width = dim(d.width, i, d.block);
                mip.height = dim(d.height, i, d.block);
                mip.depth = depth_of(i);
                mip.pitch = pitch;
                mip.rows = rows;
                mip.depth_pitch = thick_ ? round_up(mip_depth(i), kBlock256_3d[element_log2][2]) : 1;
                mip.in_tail = true;
                mip.tail_offset = tail_offset;
                mip.tail_x = x * micro_w;
                mip.tail_y = y * micro_h;
                mips_.push_back(mip);
                pitch = std::max(pitch >> 1, micro_w);
                rows = std::max(rows >> 1, micro_h);
            }
        }
        // Thick layouts store each depth slab's chain together.
        slice_size_ = slice_size * (thick_ ? block_[2] : 1);
        slices_ = slices / block_[2];
        size_ = slice_size_ * slices_;
        alignment_ = block_bytes;
        if (!swizzle::xor_mode(d.swizzle) && d.samples == 1) {
            const int kind = thick_ ? 2 : swizzle::display(d.swizzle) ? 1 : 0;
            const auto bits = pattern(kind, element_log2, block_log2);
            for (int axis = 0; axis < 3; ++axis) pattern_[axis] = spread(bits[axis]);
        } else if (d.swizzle == swizzle::sw_64kb_r_x && d.dimension == Dimension::tex2d && d.depth == 1 &&
                   d.mips == 1 && d.samples == 1 && d.block == 1) {
            pattern_ = render_xor_pattern(series_s, d.element_bytes);
        }
    }

    // The slice and the depth inside it for `z` (an array slice or a 3D depth slice).
    std::pair<std::uint32_t, std::uint32_t> split(std::uint32_t z) const {
        return thick_ ? std::pair{z / block_[2], z % block_[2]} : std::pair{z, 0u};
    }

    std::uint64_t render_xor_offset(const MipLayout& m, std::uint32_t x, std::uint32_t y) const {
        const std::uint64_t block = std::uint64_t(y / block_[1]) * (m.pitch / block_[0]) + x / block_[0];
        const auto& ox = pattern_[0];
        const auto& oy = pattern_[1];
        return block * alignment_ + (ox[x & (ox.size() - 1)] ^ oy[y & (oy.size() - 1)]);
    }

    // The part of an address that depends on the row (and the depth in a thick block). In the tail the pattern of the
    // shifted position gives the mip's offset inside the tail block.
    std::uint64_t row(const MipLayout& m, std::uint32_t y, std::uint32_t z) const {
        const unsigned log2 = unsigned(std::countr_zero(alignment_));
        y += m.in_tail ? m.tail_y : 0;
        const std::uint64_t blocks = std::uint64_t(y / block_[1]) * (m.pitch / block_[0]);
        return (blocks << log2) + (pattern_[1][y % block_[1]] | pattern_[2][z]);
    }

    // The part of an address that depends on the column.
    std::uint64_t column(const MipLayout& m, std::uint32_t x) const {
        x += m.in_tail ? m.tail_x : 0;
        return (std::uint64_t(x / block_[0]) << std::countr_zero(alignment_)) + pattern_[0][x % block_[0]];
    }

    // Visits contiguous runs as (tiled byte offset, linear byte offset, length).
    template<class Copy>
    bool walk(std::size_t tiled_len, std::uint32_t mip, std::uint32_t slice, std::size_t linear_len,
              std::size_t row_pitch, std::size_t depth_pitch, Copy&& copy) const {
        if (mip >= mips_.size()) return false;
        const MipLayout& m = mips_[mip];
        std::uint32_t first = slice;
        if (desc_.dimension == Dimension::tex3d) {
            if (slice != 0) return false;
            first = 0;
        }
        if (first >= slices_ || !addressable()) return false;
        const std::size_t n = desc_.element_bytes;
        const std::size_t bytes = std::size_t(m.width) * n;
        constexpr std::size_t max = std::numeric_limits<std::size_t>::max();
        const std::size_t dz = m.depth - 1, dy = m.height - 1;
        if ((dz && depth_pitch > max / dz) || (dy && row_pitch > max / dy)) return false;
        const std::size_t a = dz * depth_pitch, b = dy * row_pitch;
        if (a > max - b || a + b > max - bytes) return false;
        const std::size_t need = a + b + bytes;
        if (tiled_len < bytes_from(mip) || linear_len < need || row_pitch < bytes) return false;
        if (desc_.swizzle == swizzle::sw_64kb_r_x) {
            // The low four address bits are column and element bits only, so every element size has 16-byte runs.
            for (std::uint32_t y = 0; y < m.height; ++y)
                for (std::uint32_t x = 0; x < m.width; x += std::uint32_t(16 / n)) {
                    const std::size_t lx = std::size_t(x) * n;
                    copy(std::size_t(render_xor_offset(m, x, y)), std::size_t(y) * row_pitch + lx,
                         std::min<std::size_t>(16, bytes - lx));
                }
            return true;
        }
        const bool is_linear = desc_.swizzle == swizzle::linear;
        // The row and depth contribute one common base to every column, so the same runs repeat on every row.
        std::vector<std::array<std::size_t, 3>> runs;  // (tiled, linear, length) within a row
        if (!is_linear) {
            std::size_t first_linear = 0, first_tiled = std::size_t(column(m, 0));
            for (std::uint32_t x = 1; x < m.width; ++x) {
                const std::size_t at_tiled = std::size_t(column(m, x)), at_linear = std::size_t(x) * n;
                if (at_tiled != first_tiled + (at_linear - first_linear)) {
                    runs.push_back({first_tiled, first_linear, at_linear - first_linear});
                    first_tiled = at_tiled;
                    first_linear = at_linear;
                }
            }
            runs.push_back({first_tiled, first_linear, bytes - first_linear});
        }
        for (std::uint32_t z = 0; z < m.depth; ++z) {
            const auto [s, inner] = split(first + z);
            const std::uint64_t base = std::uint64_t(s) * slice_size_ + m.offset;
            for (std::uint32_t y = 0; y < m.height; ++y) {
                const std::size_t linear_row = std::size_t(z) * depth_pitch + std::size_t(y) * row_pitch;
                if (is_linear) {
                    copy(std::size_t(base + std::uint64_t(y) * m.pitch * n), linear_row, bytes);
                } else {
                    const std::size_t tiled_row = std::size_t(base + row(m, y, inner));
                    for (const auto& run : runs) copy(tiled_row + run[0], linear_row + run[1], run[2]);
                }
            }
        }
        return true;
    }

    LayoutDesc desc_{};
    std::array<std::uint32_t, 3> block_{};
    bool thick_ = false;
    std::uint32_t slices_ = 0;
    std::uint64_t slice_size_ = 0, size_ = 0, alignment_ = 0;
    std::uint32_t first_tail_mip_ = 0;
    std::vector<MipLayout> mips_;
    // Byte offsets inside a block of each element coordinate, per axis; empty for linear and unaddressable modes.
    std::array<std::vector<std::uint32_t>, 3> pattern_;
};

// What a DXGI format stores per element on the console: bytes, texels per element side (4 for block compression), and
// whether its stencil is a second plane (the GPU keeps depth and stencil apart). False for 96-bit, 1-bit, packed 4:2:2
// and video formats (VIctory texture.rs format(): not laid out there either).
struct ElementFormat {
    std::uint32_t bytes = 0, block = 1;
    bool stencil = false;
};
constexpr bool element_format(std::uint32_t dxgi, ElementFormat& out) {
    const auto in = [&](std::uint32_t a, std::uint32_t b) { return dxgi >= a && dxgi <= b; };
    if (in(1, 4)) out = {16, 1, false};
    else if (in(9, 18)) out = {8, 1, false};
    else if (in(19, 22) || in(44, 47)) out = {4, 1, true};
    else if (in(23, 43) || dxgi == 67 || in(87, 93) || dxgi == 116 || dxgi == 117) out = {4, 1, false};
    else if (in(48, 59) || dxgi == 85 || dxgi == 86 || dxgi == 115) out = {2, 1, false};
    else if (in(118, 120)) out = {2, 1, true};
    else if (in(60, 65)) out = {1, 1, false};
    else if (in(70, 72) || in(79, 81)) out = {8, 4, false};
    else if (in(73, 78) || in(82, 84) || in(94, 99)) out = {16, 4, false};
    else return false;
    return true;
}

// A texture as the console allocates it: every data plane (depth, then stencil), one after the other.
// shortcut (as VIctory's): no compression metadata planes (HTILE, CMASK, FMASK, DCC).
struct ConsoleTextureDesc {
    Dimension dimension = Dimension::tex2d;
    std::uint32_t width = 1, height = 1, depth_or_array = 1;
    std::uint32_t mips = 1;  // 0: the full chain
    std::uint32_t format = 0;  // DXGI format number
    std::uint32_t samples = 1;
    std::uint32_t swizzle = swizzle::linear;
};

struct TexturePlane {
    std::uint64_t offset = 0;  // from the start of the texture
    bool stencil = false;
    TextureLayout layout;
};

struct ConsoleTexture {
    std::vector<TexturePlane> planes;
    std::uint64_t size = 0, alignment = 1;

    static bool make(const ConsoleTextureDesc& d, bool series_s, ConsoleTexture& out) {
        ElementFormat f;
        if (!element_format(d.format, f)) return false;
        const std::uint32_t depth3d = d.dimension == Dimension::tex3d ? d.depth_or_array : 1;
        const std::uint32_t mips =
            d.mips ? d.mips : std::uint32_t(std::bit_width(std::max({d.width, d.height, depth3d, 1u})));
        LayoutDesc plane{d.dimension, d.width, d.height, d.depth_or_array, mips, f.bytes, f.block,
                         std::max(d.samples, 1u), d.swizzle};
        ConsoleTexture t;
        TexturePlane first;
        if (!TextureLayout::make(plane, series_s, first.layout)) return false;
        t.planes.push_back(std::move(first));
        if (f.stencil) {
            plane.element_bytes = 1;
            TexturePlane second;
            if (!TextureLayout::make(plane, series_s, second.layout)) return false;
            const std::uint64_t a = second.layout.alignment();
            second.offset = (t.planes[0].layout.size() + a - 1) / a * a;
            second.stencil = true;
            t.planes.push_back(std::move(second));
        }
        t.size = t.planes.back().offset + t.planes.back().layout.size();
        for (const TexturePlane& p : t.planes) t.alignment = std::max(t.alignment, p.layout.alignment());
        out = std::move(t);
        return true;
    }
};

}  // namespace leonida::gfx10
