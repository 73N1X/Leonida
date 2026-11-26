#include "../src/gfx10_tiling.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#define CHECK(condition) do {                                      \
    if (!(condition)) {                                            \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        return 1;                                                  \
    }                                                              \
} while (0)

using leonida::gfx10::TileMode;
using leonida::gfx10::TiledSurface2D;

int check_surface(TileMode mode, std::size_t bpe, std::size_t width, std::size_t height) {
    TiledSurface2D surface;
    CHECK(TiledSurface2D::make(mode, bpe, width, height, surface));
    const auto bw = surface.tile_width(), bh = surface.tile_height();
    const auto pw = (width + bw - 1) / bw * bw, ph = (height + bh - 1) / bh * bh;
    CHECK(surface.tiled_size() == pw * ph * bpe);
    TiledSurface2D padded;
    CHECK(TiledSurface2D::make(mode, bpe, pw, ph, padded));
    std::vector<bool> seen(surface.tiled_size());
    for (std::size_t y = 0; y < ph; ++y) {
        for (std::size_t x = 0; x < pw; ++x) {
            std::size_t offset;
            CHECK(padded.element_offset(x, y, offset));
            CHECK(offset % bpe == 0 && offset <= seen.size() - bpe);
            for (std::size_t byte = 0; byte < bpe; ++byte) {
                CHECK(!seen[offset + byte]);
                seen[offset + byte] = true;
            }
        }
    }
    CHECK(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }));

    const std::size_t row_bytes = width * bpe;
    for (const auto pitch : {row_bytes, row_bytes + 13}) {
        const std::size_t linear_size = (height - 1) * pitch + row_bytes;
        std::vector<std::byte> linear(linear_size), tiled(surface.tiled_size(), std::byte{0xcc});
        for (std::size_t i = 0; i < linear.size(); ++i) linear[i] = std::byte((i * 37 + i / 251) & 255);
        std::vector<std::byte> decoded(linear_size, std::byte{0xab});
        CHECK(surface.tile(linear, tiled, pitch));
        CHECK(surface.detile(tiled, decoded, pitch));
        std::vector<std::byte> expected(linear_size, std::byte{0xab});
        for (std::size_t y = 0; y < height; ++y)
            std::copy_n(linear.begin() + y * pitch, row_bytes, expected.begin() + y * pitch);
        CHECK(decoded == expected);
        std::vector<std::byte> reference(linear_size, std::byte{0xab});
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                std::size_t offset;
                CHECK(surface.element_offset(x, y, offset));
                std::copy_n(tiled.begin() + offset, bpe, reference.begin() + y * pitch + x * bpe);
            }
        }
        CHECK(decoded == reference);
        std::vector<std::byte> encoded(tiled.size(), std::byte{0xcc});
        CHECK(surface.tile(decoded, encoded, pitch));
        CHECK(encoded == tiled);
        std::vector<bool> active(tiled.size());
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                std::size_t offset;
                CHECK(surface.element_offset(x, y, offset));
                for (std::size_t byte = 0; byte < bpe; ++byte) {
                    CHECK(tiled[offset + byte] == linear[y * pitch + x * bpe + byte]);
                    active[offset + byte] = true;
                }
            }
        }
        for (std::size_t i = 0; i < tiled.size(); ++i) CHECK(active[i] || tiled[i] == std::byte{0xcc});

        const auto before_tiled = tiled, before_decoded = decoded;
        CHECK(!surface.tile(std::span(linear).first(linear_size - 1), tiled, pitch));
        CHECK(!surface.tile(linear, std::span(tiled).first(tiled.size() - 1), pitch));
        CHECK(!surface.detile(std::span(tiled).first(tiled.size() - 1), decoded, pitch));
        CHECK(!surface.detile(tiled, std::span(decoded).first(linear_size - 1), pitch));
        CHECK(!surface.tile(linear, tiled, row_bytes - 1));
        CHECK(!surface.detile(tiled, decoded, row_bytes - 1));
        CHECK(!surface.tile({}, tiled, pitch));
        CHECK(!surface.detile(tiled, {}, pitch));
        CHECK(tiled == before_tiled && decoded == before_decoded);
    }
    std::size_t offset = 123;
    CHECK(!surface.element_offset(width, 0, offset));
    CHECK(!surface.element_offset(0, height, offset));
    CHECK(offset == 123);
    std::vector<std::byte> shared(std::max(surface.tiled_size(), width * height * bpe) + 1);
    const auto before = shared;
    CHECK(!surface.tile(shared, shared, row_bytes));
    CHECK(!surface.detile(shared, shared, row_bytes));
    CHECK(!surface.tile(std::span(shared).subspan(1), std::span(shared).first(shared.size() - 1), row_bytes));
    CHECK(!surface.detile(std::span(shared).first(shared.size() - 1), std::span(shared).subspan(1), row_bytes));
    CHECK(shared == before);
    return 0;
}

// ---- Whole-texture layouts (TextureLayout, ported from VIctory victory-formats texture.rs, whose tests these are) ----

namespace g = leonida::gfx10;

g::LayoutDesc desc(std::uint32_t w, std::uint32_t h, std::uint32_t mips, std::uint32_t bpe, std::uint32_t swizzle) {
    g::LayoutDesc d;
    d.width = w;
    d.height = h;
    d.mips = mips;
    d.element_bytes = bpe;
    d.swizzle = swizzle;
    return d;
}

g::TextureLayout layout_of(const g::LayoutDesc& d, bool series_s = false) {
    g::TextureLayout l;
    if (!g::TextureLayout::make(d, series_s, l)) std::fprintf(stderr, "layout refused\n");
    return l;
}

std::uint64_t offset_of(const g::TextureLayout& l, std::uint32_t mip, std::uint32_t x, std::uint32_t y,
                        std::uint32_t z = 0) {
    std::uint64_t at = ~0ull;
    l.offset(mip, x, y, z, at);
    return at;
}

int check_texture_layouts() {
    namespace sw = g::swizzle;
    // Blocks hold their size in elements.
    for (const std::uint32_t log2 : {8u, 12u, 16u}) {
        const std::uint32_t mode = log2 == 8 ? sw::sw_256b_s : log2 == 12 ? sw::sw_4kb_s : sw::sw_64kb_d;
        for (std::uint32_t e = 0; e < 5; ++e) {
            g::TextureLayout l;
            CHECK(g::TextureLayout::make(desc(1, 1, 1, 1u << e, mode), false, l));
            const auto& b = l.block();
            CHECK((std::uint64_t(b[0]) * b[1] * b[2] << e) == (std::uint64_t(1) << log2));
        }
    }
    {
        const g::TextureLayout l = layout_of(desc(64, 64, 1, 16, sw::sw_64kb_d));
        CHECK(l.block()[0] == 64 && l.block()[1] == 64 && l.size() == 65536 && l.alignment() == 65536);
    }
    // One mip of SW_4KB_S and SW_64KB_D is TiledSurface2D's addressing (Mesa's patterns, corpus-checked).
    for (const auto mode : {TileMode::sw_4kb_s, TileMode::sw_64kb_d})
        for (const std::uint32_t bpe : {1u, 2u, 4u, 8u, 16u}) {
            TiledSurface2D surface;
            CHECK(TiledSurface2D::make(mode, bpe, 300, 130, surface));
            const g::TextureLayout l =
                layout_of(desc(300, 130, 1, bpe, mode == TileMode::sw_4kb_s ? sw::sw_4kb_s : sw::sw_64kb_d));
            CHECK(l.addressable() && l.size() == surface.tiled_size());
            for (std::uint32_t y = 0; y < 130; y += 7)
                for (std::uint32_t x = 0; x < 300; x += 5) {
                    std::size_t want = 0;
                    CHECK(surface.element_offset(x, y, want));
                    CHECK(offset_of(l, 0, x, y) == want);
                }
        }
    // Hand-checked addresses: 64 KiB D with 4-byte elements; 4 KiB S with 16-byte elements is 16x16, y first.
    {
        const g::TextureLayout l = layout_of(desc(128, 128, 1, 4, sw::sw_64kb_d));
        CHECK(offset_of(l, 0, 1, 0) == 1u << 2 && offset_of(l, 0, 4, 0) == 1u << 6 && offset_of(l, 0, 0, 4) == 1u << 7);
        CHECK(offset_of(l, 0, 0, 8) == 1u << 8 && offset_of(l, 0, 64, 64) == ((1u << 15) | (1u << 14)));
        const g::TextureLayout s = layout_of(desc(32, 32, 1, 16, sw::sw_4kb_s));
        CHECK(s.block()[0] == 16 && s.block()[1] == 16);
        CHECK(offset_of(s, 0, 0, 1) == 1u << 4 && offset_of(s, 0, 1, 0) == 1u << 6);
        CHECK(offset_of(s, 0, 16, 0) == 4096 && offset_of(s, 0, 0, 16) == 8192);
    }
    // The mip tail comes first: a 256x256 BC7 chain in 64 KiB D has one tail block holding mips 1-8, then mip 0; mip 1
    // sits at x = 32 elements, 32 KiB into the tail. 4 KiB blocks hold fewer tail mips.
    {
        const g::TextureLayout l = layout_of(desc(64, 64, 9, 16, sw::sw_64kb_d));
        CHECK(l.first_tail_mip() == 1 && l.size() == 2 * 65536 && l.mips()[0].offset == 65536);
        const g::MipLayout& m1 = l.mips()[1];
        CHECK(m1.in_tail && m1.tail_offset == 32768 && m1.tail_x == 32 && m1.tail_y == 0);
        CHECK(offset_of(l, 1, 0, 0) == 32768);
        const g::TextureLayout s = layout_of(desc(64, 64, 9, 16, sw::sw_4kb_s));
        CHECK(s.first_tail_mip() == 3 && s.mips()[2].offset == 4096 && s.mips()[1].offset == 8192 &&
              s.mips()[0].offset == 8192 + 16384);
    }
    // Every element of every mip and slice lands on its own address inside the texture, and tiling then detiling
    // gives the rows back (padding in the rows untouched).
    std::vector<g::LayoutDesc> cases = {desc(64, 64, 7, 16, sw::sw_64kb_d), desc(100, 60, 7, 4, sw::sw_64kb_d),
                                        desc(250, 175, 8, 8, sw::sw_4kb_s),  desc(33, 17, 6, 1, sw::sw_4kb_d),
                                        desc(40, 9, 3, 2, sw::sw_64kb_s),    desc(30, 20, 5, 4, sw::linear),
                                        desc(83, 31, 6, 1, sw::sw_256b_s)};
    cases.push_back(desc(109, 71, 6, 16, sw::sw_256b_d));
    cases.back().block = 4;
    cases.push_back(desc(70, 70, 4, 16, sw::sw_4kb_s));
    cases.back().depth = 3;
    cases.push_back(desc(64, 32, 7, 4, sw::sw_4kb_s));
    cases.back().dimension = g::Dimension::tex3d;
    cases.back().depth = 16;
    for (const g::LayoutDesc& d : cases) {
        g::TextureLayout l;
        CHECK(g::TextureLayout::make(d, false, l) && l.addressable());
        std::vector<bool> seen(l.size());
        std::vector<std::byte> tiled(l.size());
        const std::size_t n = d.element_bytes;
        const std::uint32_t slices = d.dimension == g::Dimension::tex3d ? 1 : d.depth;
        for (std::uint32_t slice = 0; slice < slices; ++slice)
            for (std::uint32_t mip = 0; mip < d.mips; ++mip) {
                const g::MipLayout& m = l.mips()[mip];
                const std::size_t active = m.width * n, row = active + 13, plane = row * m.height + 11;
                std::vector<std::byte> linear(plane * m.depth), back(linear.size(), std::byte{0xcc});
                for (std::size_t i = 0; i < linear.size(); ++i) linear[i] = std::byte(i * 7 + mip * 13 + slice);
                CHECK(l.tile(tiled, mip, slice, linear, row, plane));
                CHECK(l.detile(tiled, mip, slice, back, row, plane));
                for (std::uint32_t z = 0; z < m.depth; ++z) {
                    const std::size_t base = z * plane;
                    for (std::uint32_t y = 0; y < m.height; ++y) {
                        const std::size_t start = base + y * row;
                        CHECK(std::equal(back.begin() + start, back.begin() + start + active, linear.begin() + start));
                        CHECK(std::all_of(back.begin() + start + active, back.begin() + start + row,
                                          [](std::byte b) { return b == std::byte{0xcc}; }));
                        for (std::uint32_t x = 0; x < m.width; ++x) {
                            std::uint64_t at = 0;
                            CHECK(l.offset(mip, x, y, slice + z, at));
                            CHECK(at + n <= seen.size() && at % n == 0 && !seen[at]);
                            CHECK(std::equal(tiled.begin() + at, tiled.begin() + at + n,
                                             linear.begin() + start + x * n));
                            seen[at] = true;
                        }
                    }
                }
            }
    }
    // The smaller mips need only the start of the texture: 1024x1024 BC1 in 4 KiB blocks.
    {
        g::LayoutDesc d = desc(1024, 1024, 9, 8, sw::sw_4kb_s);
        d.block = 4;
        const g::TextureLayout l = layout_of(d);
        CHECK(l.bytes_from(0) == l.size() && l.bytes_from(4) == 4096 && l.bytes_from(8) == 4096);
        for (std::uint32_t mip = 1; mip < 9; ++mip) CHECK(l.bytes_from(mip - 1) >= l.bytes_from(mip));
        std::vector<std::byte> tiled(l.size());
        for (std::size_t i = 0; i < tiled.size(); ++i) tiled[i] = std::byte((i * 7) >> 3);
        for (std::uint32_t mip = 1; mip < 9; ++mip) {
            const g::MipLayout& m = l.mips()[mip];
            const std::size_t pitch = m.width * 8;
            std::vector<std::byte> whole(pitch * m.height), part(whole.size());
            const std::size_t len = std::size_t(l.bytes_from(mip));
            CHECK(l.detile(tiled, mip, 0, whole, pitch, whole.size()));
            CHECK(l.detile(std::span<const std::byte>(tiled).first(len), mip, 0, part, pitch, part.size()));
            CHECK(whole == part);
            CHECK(!l.detile(std::span<const std::byte>(tiled).first(len - 1), mip, 0, part, pitch, part.size()));
        }
        d.depth = 2;
        const g::TextureLayout two = layout_of(d);
        CHECK(two.bytes_from(4) == two.size());
    }
    // Depth/stencil is two planes: 4-byte depth in 128x128 blocks, 1-byte stencil in 256x256 blocks; Z_X has no
    // address equation here.
    {
        g::ConsoleTextureDesc d;
        d.width = 1920;
        d.height = 1080;
        d.format = 20;  // D32_FLOAT_S8X24_UINT
        d.swizzle = sw::sw_64kb_z_x;
        g::ConsoleTexture t;
        CHECK(g::ConsoleTexture::make(d, false, t) && t.planes.size() == 2);
        CHECK(t.planes[0].layout.size() == 1920ull * 1152 * 4 && t.planes[1].offset == t.planes[0].layout.size());
        CHECK(t.planes[1].layout.size() == 2048ull * 1280 && !t.planes[0].layout.addressable());
        d.format = 28;  // R8G8B8A8_UNORM, the full chain
        d.mips = 0;
        d.swizzle = sw::sw_4kb_s;
        CHECK(g::ConsoleTexture::make(d, false, t) && t.planes.size() == 1 &&
              t.planes[0].layout.mips().size() == 11);
        d.format = 6;  // R32G32B32_FLOAT: not laid out
        CHECK(!g::ConsoleTexture::make(d, false, t));
    }
    // SW_64KB_R_X: FNV-1a of the little-endian u32 addresses in row order, as AMD's Addr2ComputeSurfaceAddrFromCoord
    // gives them (VIctory's hashes, GFX10.3 PAL c5e8000); odd sizes check the high coordinate bits and a final partial
    // run. Tiling then detiling gives the rows back.
    struct XorCase {
        bool series_s;
        std::uint32_t bpe;
        std::uint64_t hashes[2];
    };
    constexpr XorCase xor_cases[] = {
        {false, 2, {0x10514417061b8725ull, 0x11a55308ad39a7b9ull}},
        {false, 4, {0xd4556429b544c925ull, 0x7a2302116719e58full}},
        {false, 8, {0x30aa25589f795b25ull, 0xb5f483c6ddf11404ull}},
        {true, 8, {0xe107baa89506c525ull, 0x7b7f2a812bdc611bull}},
        {true, 2, {0x67cde4ff6e728f25ull, 0x2eb48d52f015da36ull}},
        {true, 4, {0xce798e86f5ab1d25ull, 0x6cddf048fc97e768ull}}};
    for (const XorCase& c : xor_cases) {
        const std::uint32_t sizes[2][2] = {{256, 256}, {513, 357}};
        for (int s = 0; s < 2; ++s) {
            const std::uint32_t w = sizes[s][0], h = sizes[s][1];
            g::TextureLayout l;
            CHECK(g::TextureLayout::make(desc(w, h, 1, c.bpe, sw::sw_64kb_r_x), c.series_s, l) && l.addressable());
            std::uint64_t hash = 0xcbf29ce484222325ull;
            const std::size_t pitch = std::size_t(w) * c.bpe + 32;
            std::vector<std::byte> linear(pitch * h, std::byte{0x55}), tiled(l.size(), std::byte{0xee});
            std::vector<std::byte> back(linear.size(), std::byte{0x55});
            for (std::uint32_t y = 0; y < h; ++y)
                for (std::uint32_t x = 0; x < w; ++x) {
                    std::uint64_t at = 0;
                    CHECK(l.offset(0, x, y, 0, at));
                    for (int b = 0; b < 4; ++b)
                        hash = (hash ^ ((std::uint32_t(at) >> (8 * b)) & 0xffu)) * 0x100000001b3ull;
                    const std::uint64_t seed = std::uint64_t(x + w * y) * 0x9e3779b97f4a7c15ull;
                    std::memcpy(linear.data() + y * pitch + x * c.bpe, &seed, c.bpe);
                }
            CHECK(hash == c.hashes[s]);
            CHECK(l.tile(tiled, 0, 0, linear, pitch, 0) && l.detile(tiled, 0, 0, back, pitch, 0) && back == linear);
        }
    }
    // Bad descriptions and buffers are refused.
    {
        g::TextureLayout l;
        CHECK(!g::TextureLayout::make(desc(0, 4, 1, 4, sw::sw_4kb_s), false, l));
        CHECK(!g::TextureLayout::make(desc(4, 4, 1, 12, sw::sw_4kb_s), false, l));
        CHECK(!g::TextureLayout::make(desc(4, 4, 1, 4, 3), false, l));
        CHECK(!g::TextureLayout::make(desc(16385, 1, 1, 4, sw::sw_4kb_s), false, l));
        CHECK(g::TextureLayout::make(desc(64, 64, 1, 4, sw::sw_4kb_s), false, l));
        std::vector<std::byte> small(100), out(64 * 64 * 4);
        CHECK(!l.detile(small, 0, 0, out, 256, 0));
        std::uint64_t at = 0;
        CHECK(!l.offset(0, 64, 0, 0, at));
        g::TextureLayout lin;
        CHECK(g::TextureLayout::make(desc(4, 2, 1, 4, sw::linear), false, lin));
        std::vector<std::byte> t(lin.size()), row(32);
        CHECK(!lin.detile(t, 0, 0, row, std::numeric_limits<std::size_t>::max(), 0));
        CHECK(!lin.tile(t, 0, 0, row, std::numeric_limits<std::size_t>::max(), 0));
    }
    std::puts("gfx10_tiling: whole-texture layouts (mips, tails, slices, 3D, planes, SW_64KB_R_X) as VIctory's tests "
              "give them");
    return 0;
}

int main() {
    // Source byte offsets reconstructed by calling ttpf_mips.detile on synthetic address-labelled bytes,
    // three passes (address >> 0/8/16). Extent 300x130; points (4,4), (32,16), (299,129).
    constexpr std::size_t answers[2][5][3] = {
        {{68, 2304, 59419}, {72, 3072, 100502}, {192, 5120, 201244},
         {384, 45056, 365144}, {768, 86016, 698576}},
        {{36, 2304, 84051}, {72, 3072, 198806}, {192, 9216, 336412},
         {320, 12288, 533048}, {768, 36864, 952432}}};
    constexpr std::array<std::array<std::size_t, 2>, 3> points{{{4, 4}, {32, 16}, {299, 129}}};
    unsigned mode_index = 0;
    for (const auto mode : {TileMode::sw_4kb_s, TileMode::sw_64kb_d}) {
        unsigned bpe_index = 0;
        for (const std::size_t bpe : {1, 2, 4, 8, 16}) {
            TiledSurface2D surface;
            CHECK(TiledSurface2D::make(mode, bpe, 300, 130, surface));
            for (unsigned i = 0; i < points.size(); ++i) {
                std::size_t offset;
                CHECK(surface.element_offset(points[i][0], points[i][1], offset));
                CHECK(offset == answers[mode_index][bpe_index][i]);
            }
            CHECK(check_surface(mode, bpe, 1, 1) == 0);
            CHECK(check_surface(mode, bpe, 33, 17) == 0);
            CHECK(check_surface(mode, bpe, 300, 130) == 0);
            CHECK(check_surface(mode, bpe, surface.tile_width() * 2, surface.tile_height() * 2) == 0);
            ++bpe_index;
        }
        ++mode_index;
    }
    TiledSurface2D invalid;
    for (const std::size_t bpe : {0, 3, 12, 32})
        CHECK(!TiledSurface2D::make(TileMode::sw_4kb_s, bpe, 1, 1, invalid));
    CHECK(!TiledSurface2D::make(TileMode(99), 4, 1, 1, invalid));
    CHECK(!TiledSurface2D::make(TileMode::sw_4kb_s, 4, 0, 1, invalid));
    CHECK(!TiledSurface2D::make(TileMode::sw_4kb_s, 4, 1, 0, invalid));
    constexpr auto max = std::numeric_limits<std::size_t>::max();
    for (const auto mode : {TileMode::sw_4kb_s, TileMode::sw_64kb_d}) {
        CHECK(!TiledSurface2D::make(mode, 16, max, 1, invalid));
        CHECK(!TiledSurface2D::make(mode, 1, max, max, invalid));
        CHECK(!TiledSurface2D::make(mode, 1, 1, max, invalid));
    }
    CHECK(invalid.tiled_size() == 0);
    std::size_t offset;
    CHECK(!invalid.element_offset(0, 0, offset));
    CHECK(!invalid.tile({}, {}, 0));
    CHECK(!invalid.detile({}, {}, 0));
    TiledSurface2D surface;
    CHECK(TiledSurface2D::make(TileMode::sw_4kb_s, 4, 1, 3, surface));
    std::vector<std::byte> tiled(surface.tiled_size()), linear(12);
    CHECK(!surface.tile(linear, tiled, max));
    CHECK(!surface.detile(tiled, linear, max));
    std::puts("gfx10_tiling: 40 surface round trips, padded bijections, 30 Python offsets and invalid arguments passed");
    return check_texture_layouts();
}
