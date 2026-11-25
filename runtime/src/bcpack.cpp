// BCPack decoder (bcpack.h), ported from VIctory's victory-formats (victory/crates/victory-formats/src/bcpack.rs,
// GPL-3.0-or-later, Marvisak and contributors); the table loading follows bcpack_tables() in
// victory/crates/victory-hle/src/modules/dstorage.rs. Leonida is GPL-3.0-or-later too.
//
// The arithmetic is VIctory's step for step (wrapping u8, u16 and u32 sums, the same shifts and casts), so the blocks
// are bit-exact. What differs is the plumbing around it:
// - A block is rebuilt as soon as the lanes have delivered its record, instead of after a first pass that keeps every
//   record (1.5-2 times the output size). The lanes deliver records strictly in order, so the blocks are the same;
//   only a failing stream may leave some blocks written.
// - Symbol 0's bucket is folded into the other buckets' arithmetic when the tables are parsed (first slot 1,
//   frequency f0 - 1, reciprocal 0), which takes a data-dependent branch out of the decode step.
// - Nothing allocates while decoding; the 256-block history is on the stack.
// Speed: one core decodes the title's chunks at about 65 MB/s of BC1 and 30 MB/s of BC7, a little ahead of VIctory's
// build (Ryzen 5 7600X, MSVC 2022, RelWithDebInfo). Writing the lane step's read, the byte handout or the delivery
// without branches, and stepping all lanes of a round before handing out its bytes, were each measured slower: the
// loop is bound by each lane's chain of states, which predicted branches keep short.
#include "bcpack.h"

#include "bcpack_decode.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(_MSC_VER)
#define BCPACK_INLINE __forceinline
#define BCPACK_NOINLINE __declspec(noinline)
#else
#define BCPACK_INLINE inline __attribute__((always_inline))
#define BCPACK_NOINLINE __attribute__((noinline))
#endif

namespace rt::bcpack {
namespace format {

constexpr unsigned kModels = 128;
constexpr unsigned kBuckets = 9;
constexpr unsigned kHistory = 256;
constexpr unsigned kDictionary = 255;
// rANS: decode while the state is above this, read while below.
constexpr std::uint32_t kLow = 1u << 24;
// Input bytes per schedule round.
constexpr unsigned kRoundBytes = 8;

// Endpoints and indices of BC1, BC3, BC4 and BC5: small unsigned values, summed per element modulo its width.
struct Plain {
    std::uint8_t ep[8];
    std::uint8_t ix[32];
};

struct Bc6Ep {
    // Endpoint 0 R G B, then endpoints 1-3, left-aligned and biased.
    std::uint16_t e[4][3];
};

struct Bc6Ix {
    std::uint8_t mode;
    std::int16_t ix[16];
};

struct Bc6Entry {
    Bc6Ep ep;
    Bc6Ix ix;
};

struct Bc7Ep {
    std::uint16_t ctrl;
    // Per subset: R0 R1 G0 G1 B0 B1 A0 A1, left-aligned.
    std::uint16_t rows[3][8];
};

struct Bc7Ix {
    std::uint8_t bits[2];
    std::int16_t ix[2][16];
};

struct Bc7Entry {
    Bc7Ep ep;
    Bc7Ix ix;
};

// One of the 128 static rANS models over 65536 slots. Bucket 0 holds symbol 0; bucket b > 0 holds 2^(b-1) symbols
// of one frequency, from its first slot on.
struct Model {
    // ceil(2^32 / f): exact division for the offsets inside a bucket. 0 for bucket 0, whose quotient is always 0.
    std::uint64_t recip[kBuckets];
    // Each bucket's first slot: 1 for bucket 0 (slot 0 is never used; VIctory keeps 0 there and subtracts 1 itself),
    // 65536 past the model's last bucket and once more at the end, so a bucket always has a next start.
    std::uint32_t start[kBuckets + 1];
    // The frequency of each of the bucket's symbols: f0 - 1 for symbol 0, which owns slots 1 .. f0 - 1.
    std::uint32_t freq[kBuckets];
    // The bucket of the first slot of each 256-slot window: the search for a slot's bucket starts there and seldom
    // moves on.
    std::uint8_t bucket[256];
};

}  // namespace format

using namespace format;

// The format's constant tables.
struct Tables {
    Model models[kModels];
    std::uint8_t rank1[256];
    std::uint8_t rank3a[64];
    std::uint8_t rank3c[256];
    std::uint8_t rank4[64];
    std::uint8_t rank5[64];
    std::uint16_t ctrl7[512];
    Plain bc1[kDictionary];
    Plain bc3[kDictionary];
    Plain bc4[kDictionary];
    Plain bc5[kDictionary];
    Bc6Entry bc6[kDictionary];
    Bc7Entry bc7[kDictionary];
};

void TablesDeleter::operator()(const Tables* tables) const {
    delete tables;
}

namespace {

// Bytes of the table file: the model frequencies, the five rank tables, the BC7 control words, then per format 255
// dictionary entries of BC1 6 + 16 B, BC3 8 + 32 B, BC4 2 + 16 B, BC5 4 + 32 B, BC6H 25 + 33 B, BC7 50 + 66 B.
static_assert(kTablesSize == kModels * kBuckets * 2 + 256 + 64 + 256 + 64 + 64 + 512 * 2 +
                                 kDictionary * (6 + 16 + 8 + 32 + 2 + 16 + 4 + 32 + 25 + 33 + 50 + 66));

// Lane widths (symbol bits - 1 at the full alphabet) in stream order; docs/textures.md lists the symbol bits.
constexpr std::uint8_t kBc1Lanes[12] = {7, 7, 4, 5, 4, 4, 5, 4, 7, 7, 7, 7};
constexpr std::uint8_t kBc3Lanes[12] = {3, 3, 7, 5, 5, 5, 5, 4, 5, 4, 7, 7};
constexpr std::uint8_t kBc4Lanes[12] = {7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5};
constexpr std::uint8_t kBc5Lanes[12] = {3, 3, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5};
constexpr std::uint8_t kBc6Lanes[16] = {3, 3, 7, 7, 7, 4, 4, 4, 5, 5, 5, 7, 7, 7, 7, 4};
constexpr std::uint8_t kBc7Lanes[16] = {3, 3, 7, 7, 7, 7, 7, 6, 6, 4, 5, 7, 7, 7, 7, 4};

std::string utf8(const std::filesystem::path& path) {
    const std::u8string s = path.u8string();
    return std::string(s.begin(), s.end());
}

BCPACK_INLINE void store_le16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}

BCPACK_INLINE void store_le32(std::uint8_t* p, std::uint32_t v) {
    if constexpr (std::endian::native == std::endian::little) {
        std::memcpy(p, &v, 4);
    } else {
        for (unsigned k = 0; k < 4; ++k) p[k] = static_cast<std::uint8_t>(v >> (8 * k));
    }
}

BCPACK_INLINE void store_le64(std::uint8_t* p, std::uint64_t v) {
    if constexpr (std::endian::native == std::endian::little) {
        std::memcpy(p, &v, 8);
    } else {
        for (unsigned k = 0; k < 8; ++k) p[k] = static_cast<std::uint8_t>(v >> (8 * k));
    }
}

// Lanes.

struct Lane {
    std::uint32_t x;
    std::uint32_t left;
    // The byte taken for the next step, or -1.
    std::int16_t latch;
    // A symbol decoded but not yet delivered, or -1.
    std::int16_t out;
    // Symbol bits - 1 at the full alphabet.
    std::uint8_t width;
    // 0 header, 1 length (low byte), 2 length high byte, 3 payload.
    std::uint8_t phase;
    std::uint8_t model;
    bool long_form;
    bool reset;
    bool need;
    bool stalled;
};

// One step: take the latched byte (header, length or payload) and decode at most one symbol.
BCPACK_INLINE void step(Lane& lane, const Tables& t) {
    if (lane.phase != 3) {
        if (lane.latch < 0) return;
        const std::uint32_t b = static_cast<std::uint8_t>(lane.latch);
        lane.latch = -1;
        lane.need = true;
        switch (lane.phase) {
        case 0:
            // The top two bits shrink the alphabet; width >= 3.
            lane.model = static_cast<std::uint8_t>((lane.width - (b >> 6)) * 16 + (b & 15));
            lane.reset = (b & 0x10) != 0;
            lane.long_form = (b & 0x20) != 0;
            lane.left = 0;
            lane.phase = 1;
            break;
        case 1:
            if (lane.long_form) {
                lane.left = b;
                lane.phase = 2;
            } else {
                lane.left = b + 1;
                lane.phase = 3;
            }
            break;
        default:
            lane.left = ((b << 8) | lane.left) + 257;
            lane.phase = 3;
            break;
        }
        return;
    }
    std::uint32_t x = lane.x;
    if (x < kLow && lane.latch >= 0) {
        lane.left -= 1;
        x = (x << 8) | static_cast<std::uint8_t>(lane.latch);
        lane.latch = -1;
    }
    lane.need = lane.left != 0 && lane.latch < 0;
    if (x > kLow) {
        const Model& m = t.models[lane.model];
        const std::uint32_t slot = x & 0xffff;
        // The bucket: starts are ascending, and 65536 past the model's last.
        unsigned b = m.bucket[slot >> 8];
        while (m.start[b + 1] <= slot) ++b;
        // Offset and frequency are below 2^16, where the reciprocal divides exactly. Bucket 0: offset slot - 1
        // (wrapping at slot 0, as VIctory's does), quotient 0, frequency f0 - 1.
        const std::uint32_t offset = slot - m.start[b];
        const std::uint32_t q = static_cast<std::uint32_t>((std::uint64_t{offset} * m.recip[b]) >> 32);
        const std::uint32_t f = m.freq[b];
        lane.x = (x >> 16) * f + (offset - q * f);
        lane.out = static_cast<std::int16_t>((((1u << b) >> 1) + q) & 0xff);
    } else if (lane.left != 0) {
        // At exactly 2^24 the lane neither reads nor decodes again; only a lane that has delivered its symbols may
        // end up there.
        lane.x = x;
    } else {
        // The fragment ends; a byte read in this step does not reach the carried state.
        lane.phase = 0;
        lane.need = lane.latch < 0;
        if (lane.reset) lane.x = 0;
    }
}

// Input from a buffer: past its end nothing arrives.
struct BufferFetch {
    const std::uint8_t* p;
    const std::uint8_t* end;
    BCPACK_INLINE int operator()(unsigned) { return p < end ? *p++ : -1; }
};

// Input from a caller's function (unpack_records).
struct CallbackFetch {
    int (*fn)(void*, unsigned);
    void* context;
    int operator()(unsigned lane) {
        const int b = fn(context, lane);
        return b < 0 ? -1 : (b & 0xff);
    }
};

// Decodes Per symbols per block on each of the N lanes, handing each block's record (N * Per symbols, lane j's at
// j * Per) to `sink` once all its symbols are there, taking input from `fetch` (given the lane that takes the byte)
// in the order of the schedule: each round the lanes step in order and take at most 8 bytes between them.
template <unsigned N, unsigned Per, class Fetch, class Sink>
BCPACK_INLINE Error run_lanes(const Tables& t, const std::uint8_t* widths, std::size_t blocks, Fetch& fetch,
                              Sink& sink) {
    Lane lanes[N];
    for (unsigned j = 0; j < N; ++j) {
        lanes[j] = Lane{};
        lanes[j].latch = -1;
        lanes[j].out = -1;
        lanes[j].width = widths[j];
        lanes[j].need = true;
    }
    constexpr std::uint32_t kFull = (1u << N) - 1;
    std::uint8_t record[N * Per];
    const std::size_t total = blocks * Per;
    std::size_t pos = 0;
    // pos % Per: which of its block's symbols each lane delivers now.
    unsigned h = 0;
    std::uint32_t done = 0;
    unsigned idle = 0;
    while (pos < total) {
        unsigned taken = 0;
        bool moved = false;
        for (unsigned j = 0; j < N; ++j) {
            Lane& lane = lanes[j];
            const std::uint32_t bit = 1u << j;
            const unsigned before = taken;
            if (!lane.stalled) step(lane, t);
            const bool waiting = lane.out >= 0 && (done & bit) != 0;
            if (lane.need && !waiting && taken < kRoundBytes) {
                // Past the end nothing arrives; in a valid stream only finished lanes ask there.
                const int b = fetch(j);
                if (b >= 0) {
                    lane.latch = static_cast<std::int16_t>(b);
                    ++taken;
                }
            }
            lane.stalled = (lane.need && before >= kRoundBytes) || waiting;
            if (!lane.stalled && lane.out >= 0) {
                record[j * Per + h] = static_cast<std::uint8_t>(lane.out);
                lane.out = -1;
                done |= bit;
                moved = true;
            }
        }
        if (done == kFull) {
            ++pos;
            if (++h == Per) {
                h = 0;
                if (const Error error = sink(static_cast<const std::uint8_t*>(record))) return error;
            }
            done = 0;
            moved = true;
        }
        // Nothing read and nothing delivered for a few rounds: no lane can go on.
        idle = (taken > 0 || moved) ? 0 : idle + 1;
        if (idle > 3) return "the stream ends early or is corrupt";
    }
    return nullptr;
}

// Blocks.

struct References {
    unsigned ep;
    unsigned ix;
};

// Two 4-bit symbols, the first on the even bits.
BCPACK_INLINE std::uint8_t interleave(std::uint8_t a, std::uint8_t b) {
    const auto spread = [](unsigned v) {
        return (v & 1) | ((v >> 1 & 1) << 2) | ((v >> 2 & 1) << 4) | ((v >> 3 & 1) << 6);
    };
    return static_cast<std::uint8_t>(spread(a) | (spread(b) << 1));
}

// The record's endpoint and index references.
BCPACK_INLINE References references(const std::uint8_t* s, bool paired) {
    std::uint8_t d;
    std::uint8_t ix;
    if (paired) {
        d = interleave(s[0], s[1]);
        ix = interleave(s[2], s[3]);
    } else {
        d = s[0];
        ix = s[1];
    }
    const std::uint8_t delta = static_cast<std::uint8_t>((d >> 1) ^ (0u - (d & 1u)));
    return {static_cast<std::uint8_t>(ix + delta), ix};
}

// Block `i`'s reference `r`: 0 none, else `r` blocks back, before the first block into the dictionary.
template <class T>
BCPACK_INLINE const T& pick(std::size_t i, unsigned r, const T* history, const T* dictionary, const T& zero) {
    if (r == 0) return zero;
    if (r <= i) return history[(i - r) % kHistory];
    return dictionary[r - i - 1];
}

// Zigzag residual (0, -1, 1, -2, ...) modulo 2^bits.
BCPACK_INLINE std::uint8_t zz(std::uint8_t v, unsigned bits) {
    const unsigned mask = (1u << bits) - 1;
    return static_cast<std::uint8_t>(((v >> 1) ^ ((v & 1) ? mask : 0u)) & mask);
}

constexpr std::uint8_t mask_of(unsigned bits) {
    return static_cast<std::uint8_t>((1u << bits) - 1);
}

// 2-bit index residuals: one symbol per 2x2 quad.
BCPACK_INLINE void quads(const std::uint8_t* rank, const std::uint8_t* s, std::uint8_t* ix) {
    for (unsigned q = 0; q < 4; ++q) {
        const unsigned v = rank[s[q]];
        const unsigned base = (q >> 1) * 8 + (q & 1) * 2;
        ix[base + 0] = static_cast<std::uint8_t>(v & 3);
        ix[base + 1] = static_cast<std::uint8_t>(v >> 2 & 3);
        ix[base + 4] = static_cast<std::uint8_t>(v >> 4 & 3);
        ix[base + 5] = static_cast<std::uint8_t>(v >> 6 & 3);
    }
}

// 3-bit index residuals: one symbol per two pixels in raster order.
BCPACK_INLINE void pairs(const std::uint8_t* rank, const std::uint8_t* s, unsigned symbols, std::uint8_t* ix) {
    for (unsigned k = 0; k < symbols; ++k) {
        const unsigned v = rank[s[k] & 63];
        ix[2 * k] = static_cast<std::uint8_t>(v & 7);
        ix[2 * k + 1] = static_cast<std::uint8_t>(v >> 4 & 7);
    }
}

// Indices along the endpoint line -> BC1 and BC4 codes.
constexpr std::uint32_t kBc1Code[4] = {0, 2, 3, 1};
constexpr std::uint64_t kBc4Code[8] = {0, 2, 3, 4, 5, 6, 7, 1};

BCPACK_INLINE void bc1_color(const std::uint8_t* ep, const std::uint8_t* ix, std::uint8_t* out) {
    const auto c0 = static_cast<std::uint16_t>((ep[0] << 11) | (ep[1] << 5) | ep[2]);
    const auto c1 = static_cast<std::uint16_t>((ep[3] << 11) | (ep[4] << 5) | ep[5]);
    std::uint32_t bits = 0;
    for (unsigned p = 0; p < 16; ++p) bits |= kBc1Code[ix[p] & 3] << (2 * p);
    store_le16(out, c0);
    store_le16(out + 2, c1);
    store_le32(out + 4, bits);
}

// BC4 half: endpoints are kept 128 below their stored value.
BCPACK_INLINE void bc4_alpha(std::uint8_t e0, std::uint8_t e1, const std::uint8_t* ix, std::uint8_t* out) {
    std::uint64_t bits = 0;
    for (unsigned p = 0; p < 16; ++p) bits |= kBc4Code[ix[p] & 7] << (16 + 3 * p);
    const std::uint64_t v = std::uint64_t{static_cast<std::uint8_t>(e0 ^ 0x80)} |
                            std::uint64_t{static_cast<std::uint8_t>(e1 ^ 0x80)} << 8 | bits;
    store_le64(out, v);
}

// The plain formats: the record's residuals, the element widths and the BCn packing.
struct Bc1 {
    static constexpr unsigned kBlock = 8;
    static constexpr bool kPaired = false;
    static constexpr std::uint8_t kEp[8] = {5, 6, 5, 5, 6, 5, 0, 0};
    static constexpr unsigned ix_bits(unsigned k) { return k < 16 ? 2 : 0; }
    static const Plain* dictionary(const Tables& t) { return t.bc1; }
    static void residual(const Tables& t, const std::uint8_t* s, Plain& v) {
        constexpr unsigned kBits[6] = {5, 6, 5, 5, 6, 5};
        for (unsigned k = 0; k < 6; ++k) v.ep[k] = zz(s[2 + k], kBits[k]);
        quads(t.rank1, s + 8, v.ix);
    }
    static void pack(const Plain& v, std::uint8_t* out) { bc1_color(v.ep, v.ix, out); }
};

struct Bc3 {
    static constexpr unsigned kBlock = 16;
    static constexpr bool kPaired = true;
    static constexpr std::uint8_t kEp[8] = {5, 6, 5, 5, 6, 5, 8, 8};
    static constexpr unsigned ix_bits(unsigned k) { return k < 16 ? 3 : 2; }
    static const Plain* dictionary(const Tables& t) { return t.bc3; }
    static void residual(const Tables& t, const std::uint8_t* s, Plain& v) {
        // Colour lanes carry each channel's two endpoints together.
        v.ep[0] = zz(s[14], 5);
        v.ep[1] = zz(s[16], 6);
        v.ep[2] = zz(s[18], 5);
        v.ep[3] = zz(s[15], 5);
        v.ep[4] = zz(s[17], 6);
        v.ep[5] = zz(s[19], 5);
        v.ep[6] = zz(s[4], 8);
        v.ep[7] = zz(s[5], 8);
        pairs(t.rank3a, s + 6, 8, v.ix);
        quads(t.rank3c, s + 20, v.ix + 16);
    }
    static void pack(const Plain& v, std::uint8_t* out) {
        bc4_alpha(v.ep[6], v.ep[7], v.ix, out);
        bc1_color(v.ep, v.ix + 16, out + 8);
    }
};

struct Bc4 {
    static constexpr unsigned kBlock = 8;
    static constexpr bool kPaired = false;
    static constexpr std::uint8_t kEp[8] = {8, 8, 0, 0, 0, 0, 0, 0};
    static constexpr unsigned ix_bits(unsigned k) { return k < 16 ? 3 : 0; }
    static const Plain* dictionary(const Tables& t) { return t.bc4; }
    static void residual(const Tables& t, const std::uint8_t* s, Plain& v) {
        v.ep[0] = zz(s[2], 8);
        v.ep[1] = zz(s[3], 8);
        pairs(t.rank4, s + 4, 8, v.ix);
    }
    static void pack(const Plain& v, std::uint8_t* out) { bc4_alpha(v.ep[0], v.ep[1], v.ix, out); }
};

struct Bc5 {
    static constexpr unsigned kBlock = 16;
    static constexpr bool kPaired = true;
    static constexpr std::uint8_t kEp[8] = {8, 8, 8, 8, 0, 0, 0, 0};
    static constexpr unsigned ix_bits(unsigned) { return 3; }
    static const Plain* dictionary(const Tables& t) { return t.bc5; }
    static void residual(const Tables& t, const std::uint8_t* s, Plain& v) {
        for (unsigned k = 0; k < 4; ++k) v.ep[k] = zz(s[4 + k], 8);
        pairs(t.rank5, s + 8, 16, v.ix);
    }
    static void pack(const Plain& v, std::uint8_t* out) {
        bc4_alpha(v.ep[0], v.ep[1], v.ix, out);
        bc4_alpha(v.ep[2], v.ep[3], v.ix + 16, out + 8);
    }
};

constexpr Plain kZeroPlain{};

template <class F>
struct PlainSink {
    const Tables& t;
    std::uint8_t* out;
    std::size_t i = 0;
    // Only entries of blocks already rebuilt are ever read, so none needs clearing first.
    Plain history[kHistory];

    PlainSink(const Tables& tables, std::uint8_t* dst) : t(tables), out(dst) {}

    Error operator()(const std::uint8_t* s) {
        const References r = references(s, F::kPaired);
        const Plain* dictionary = F::dictionary(t);
        const std::uint8_t* ep_ref = pick(i, r.ep, history, dictionary, kZeroPlain).ep;
        const std::uint8_t* ix_ref = pick(i, r.ix, history, dictionary, kZeroPlain).ix;
        Plain v{};
        F::residual(t, s, v);
        for (unsigned k = 0; k < 8; ++k)
            v.ep[k] = static_cast<std::uint8_t>((v.ep[k] + ep_ref[k]) & mask_of(F::kEp[k]));
        for (unsigned k = 0; k < 32; ++k)
            v.ix[k] = static_cast<std::uint8_t>((v.ix[k] + ix_ref[k]) & mask_of(F::ix_bits(k)));
        history[i % kHistory] = v;
        F::pack(v, out);
        out += F::kBlock;
        ++i;
        return nullptr;
    }
};

// A byte's even and odd bits as two zigzag-coded 4-bit residuals.
BCPACK_INLINE void nibbles(unsigned b, bool odd_first, std::int16_t pair[2]) {
    const auto half = [](unsigned v) {
        return (v & 1) | ((v >> 2 & 1) << 1) | ((v >> 4 & 1) << 2) | ((v >> 6 & 1) << 3);
    };
    const auto z = [](unsigned u) { return static_cast<std::int16_t>((u >> 1) ^ (0u - (u & 1))); };
    const std::int16_t even = z(half(b));
    const std::int16_t odd = z(half(b >> 1));
    pair[0] = odd_first ? odd : even;
    pair[1] = odd_first ? even : odd;
}

// Index residual plus reference, both left-aligned to 16 bits at their own widths, back to `bits` bits (signed).
BCPACK_INLINE std::int16_t add_index(std::int16_t cur, unsigned bits, std::int16_t ref, unsigned ref_bits) {
    const auto up = [](std::int16_t v, unsigned b) -> std::uint16_t {
        return (b == 0 || b > 16) ? 0 : static_cast<std::uint16_t>(static_cast<std::uint16_t>(v) << (16 - b));
    };
    const auto sum = static_cast<std::int16_t>(static_cast<std::uint16_t>(up(cur, bits) + up(ref, ref_bits)));
    return bits == 0 ? std::int16_t{0} : static_cast<std::int16_t>(sum >> (16 - bits));
}

// Little-endian bit writer for one 128-bit block; bits past 128 are dropped.
struct Bits {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    unsigned at = 0;

    BCPACK_INLINE void put(std::uint64_t value, unsigned n) {
        if (n > 0 && at < 128) {
            const std::uint64_t v = n >= 64 ? value : value & ((std::uint64_t{1} << n) - 1);
            if (at < 64) {
                lo |= v << at;
                if (at > 0) hi |= v >> (64 - at);
            } else {
                hi |= v << (at - 64);
            }
        }
        at += n;
    }

    BCPACK_INLINE void store(std::uint8_t* out) const {
        store_le64(out, lo);
        store_le64(out + 8, hi);
    }
};

// BC7.

// Public BC7 anchor indices: second subset of two; second and third of three.
constexpr std::uint8_t kAnchor2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
                                       15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,
                                       15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,
                                       6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};
constexpr std::uint8_t kAnchor3A[64] = {3,  3,  15, 15, 8,  3,  15, 15, 8,  8,  6,  6,  6,  5,  3,  3,
                                        3,  3,  8,  15, 3,  3,  6,  10, 5,  8,  8,  6,  8,  5,  15, 15,
                                        8,  15, 3,  5,  6,  10, 8,  15, 15, 3,  15, 5,  15, 15, 15, 15,
                                        3,  15, 5,  5,  5,  8,  5,  10, 5,  10, 8,  13, 15, 12, 3,  3};
constexpr std::uint8_t kAnchor3B[64] = {15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,
                                        15, 8,  15, 3,  15, 8,  15, 8,  3,  15, 6,  10, 15, 15, 10, 8,
                                        15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15, 3,  6,  6,  8,
                                        15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};

// Endpoint bits of R G B A per mode (public), before p-bits and rotation.
constexpr std::uint8_t kBc7Bits[8][4] = {{4, 4, 4, 0}, {6, 6, 6, 0}, {5, 5, 5, 0}, {7, 7, 7, 0},
                                         {5, 5, 5, 6}, {7, 7, 7, 8}, {7, 7, 7, 7}, {5, 5, 5, 5}};
// Index bits of the primary and secondary sets per mode.
constexpr std::uint8_t kBc7Index[8][2] = {{3, 0}, {3, 0}, {2, 0}, {2, 0}, {3, 2}, {2, 2}, {4, 0}, {2, 0}};

constexpr bool pbits(unsigned mode) {
    return mode == 0 || mode == 1 || mode == 3 || mode == 6 || mode == 7;
}

// Channel precisions: the p-bit is red's lowest bit; rotation swaps alpha with the rotated channel.
BCPACK_INLINE void bc7_precision(unsigned ctrl, std::uint8_t p[4]) {
    const unsigned mode = ctrl & 7;
    const unsigned rot = ctrl >> 3 & 3;
    for (unsigned k = 0; k < 4; ++k) p[k] = kBc7Bits[mode][k];
    if (rot != 0) {
        std::swap(p[3], p[rot - 1]);
    } else if (pbits(mode)) {
        p[0] = static_cast<std::uint8_t>(p[0] + 1);
    }
}

void bc7_pack(const Bc7Ep& ep, const Bc7Ix& ix, const std::uint8_t prec[4], std::uint8_t* out) {
    const unsigned ctrl = ep.ctrl;
    const unsigned mode = ctrl & 7;
    const unsigned subsets = (ctrl & 0xff) >> 6;
    const unsigned part = ctrl >> 8 & 63;
    const unsigned rot = ctrl >> 3 & 3;
    Bits w;
    w.put(std::uint64_t{1} << mode, mode + 1);
    switch (mode) {
    case 0:
        w.put(part, 4);
        break;
    case 1:
    case 2:
    case 3:
    case 7:
        w.put(part, 6);
        break;
    case 4:
        w.put(rot, 2);
        w.put(ctrl >> 5 & 1, 1);
        break;
    case 5:
        w.put(rot, 2);
        break;
    default:
        break;
    }
    const unsigned ends = 2 * subsets;
    // A precision of 0 would shift by 16; VIctory's release build shifts by 0 there (the shift count wraps), and so
    // does this. No control code of the pinned tables gets there.
    const auto field = [&](unsigned row, unsigned col, unsigned p) -> std::uint64_t {
        return static_cast<unsigned>(ep.rows[row][col]) >> ((16 - p) & 15);
    };
    unsigned order[4] = {0, 1, 2, 3};
    if (rot != 0) std::swap(order[3], order[rot - 1]);
    for (const unsigned ch : order) {
        const unsigned p = prec[ch];
        if (p == 0) continue;
        const unsigned drop = (ch == 0 && pbits(mode)) ? 1 : 0;
        for (unsigned e = 0; e < ends; ++e) w.put(field(e / 2, 2 * ch + e % 2, p) >> drop, p - drop);
    }
    if (mode == 1) {
        for (unsigned row = 0; row < subsets; ++row) w.put(field(row, 0, prec[0]) & 1, 1);
    } else if (pbits(mode)) {
        for (unsigned e = 0; e < ends; ++e) w.put(field(e / 2, e % 2, prec[0]) & 1, 1);
    }
    unsigned anchors[3] = {0, 0, 0};
    unsigned count = 1;
    if (subsets == 2) {
        anchors[1] = kAnchor2[part];
        count = 2;
    } else if (subsets == 3) {
        anchors[1] = kAnchor3A[part];
        anchors[2] = kAnchor3B[part];
        count = 3;
    }
    const auto indices = [&](unsigned set) {
        const unsigned bits = ix.bits[set];
        for (unsigned p = 0; p < 16; ++p) {
            bool anchor = false;
            for (unsigned a = 0; a < count; ++a) anchor = anchor || anchors[a] == p;
            w.put(static_cast<std::uint16_t>(ix.ix[set][p]), bits - (anchor ? 1 : 0));
        }
    };
    if (mode == 4) {
        indices(1);
        indices(0);
    } else if (mode == 5) {
        indices(0);
        indices(1);
    } else {
        indices(0);
    }
    w.store(out);
}

constexpr Bc7Entry kZeroBc7{};

struct Bc7Sink {
    const Tables& t;
    std::uint8_t* out;
    std::size_t i = 0;
    Bc7Entry history[kHistory];

    Bc7Sink(const Tables& tables, std::uint8_t* dst) : t(tables), out(dst) {}

    Error operator()(const std::uint8_t* s) {
        const References r = references(s, true);
        const unsigned ctrl = t.ctrl7[(s[31] & 15u) << 5 | (s[30] & 31u)];
        if (ctrl == 0) return "unused BC7 control code";
        const unsigned mode = ctrl & 7;
        const unsigned subsets = (ctrl & 0xff) >> 6;
        std::uint8_t prec[4];
        bc7_precision(ctrl, prec);
        const bool three = subsets == 3;
        const std::uint8_t or0_10 = three ? 0 : s[10], or0_11 = three ? 0 : s[11];
        const std::uint8_t or0_18 = three ? 0 : s[18], or0_19 = three ? 0 : s[19];
        const std::uint8_t if3_10 = three ? s[10] : 0, if3_11 = three ? s[11] : 0;
        const std::uint8_t if3_18 = three ? s[18] : 0, if3_19 = three ? s[19] : 0;
        const std::uint8_t stored[3][8] = {
            {s[4], s[5], s[6], s[7], s[8], s[9], or0_10, or0_11},
            {s[12], s[13], s[14], s[15], s[16], s[17], or0_18, or0_19},
            {s[20], s[21], if3_10, if3_11, if3_18, if3_19, 0, 0},
        };
        const Bc7Ep& rep = pick(i, r.ep, history, t.bc7, kZeroBc7).ep;
        Bc7Entry entry{};
        entry.ep.ctrl = static_cast<std::uint16_t>(ctrl);
        // Rows the reference lacks build on its first row.
        const unsigned common = std::min(subsets, (rep.ctrl & 0xffu) >> 6);
        const unsigned columns = prec[3] > 0 ? 8 : 6;
        for (unsigned c = 0; c < columns; ++c) {
            const unsigned p = prec[c / 2];
            if (p == 0) continue;
            const auto mask = static_cast<std::uint16_t>(0xffffu << (16 - p));
            for (unsigned row = 0; row < subsets; ++row) {
                const unsigned v = stored[row][c];
                const auto residual = static_cast<std::uint16_t>(((v >> 1) ^ ((v & 1) ? 0xffffu : 0u)) << (16 - p));
                const auto base = static_cast<std::uint16_t>(rep.rows[row < common ? row : 0][c] & mask);
                entry.ep.rows[row][c] = static_cast<std::uint16_t>(base + residual);
            }
        }
        const std::uint8_t* bits = kBc7Index[mode];
        const Bc7Ix& rx = pick(i, r.ix, history, t.bc7, kZeroBc7).ix;
        entry.ix.bits[0] = bits[0];
        entry.ix.bits[1] = bits[1];
        constexpr unsigned kOffsets[2][4] = {{22, 24, 26, 28}, {12, 14, 16, 20}};
        for (unsigned set = 0; set < 2; ++set) {
            if (bits[set] == 0) continue;
            for (unsigned k = 0; k < 4; ++k) {
                const unsigned o = kOffsets[set][k];
                for (unsigned b = 0; b < 2; ++b) {
                    std::int16_t pair[2];
                    nibbles(s[o + b], k < 3, pair);
                    for (unsigned h = 0; h < 2; ++h) {
                        const unsigned p = 4 * k + 2 * b + h;
                        entry.ix.ix[set][p] = add_index(pair[h], bits[set], rx.ix[set][p], rx.bits[set]);
                    }
                }
            }
        }
        history[i % kHistory] = entry;
        bc7_pack(entry.ep, entry.ix, prec, out);
        out += 16;
        ++i;
        return nullptr;
    }
};

// BC6H.

// Bits of endpoint 0, then of the other endpoints' R, G, B, per mode 1-14.
constexpr std::uint8_t kBc6Bits[15][4] = {{0, 0, 0, 0},     {10, 5, 5, 5},   {7, 6, 6, 6},   {11, 5, 4, 4},
                                          {11, 4, 5, 4},    {11, 4, 4, 5},   {9, 5, 5, 5},   {8, 6, 5, 5},
                                          {8, 5, 6, 5},     {8, 5, 5, 6},    {6, 6, 6, 6},   {10, 10, 10, 10},
                                          {11, 9, 9, 9},    {12, 8, 8, 8},   {16, 4, 4, 4}};

// Mode code and its width, modes 1-14.
struct Bc6Code {
    std::uint8_t code;
    std::uint8_t bits;
};
constexpr Bc6Code kBc6Code[15] = {{0, 0},  {0, 2},  {1, 2},  {2, 5},  {6, 5},  {10, 5}, {14, 5}, {18, 5},
                                  {22, 5}, {26, 5}, {30, 5}, {3, 5},  {7, 5},  {11, 5}, {15, 5}};

// A run of the public BC6H layout: `len` bits of channel `ch` (0-2; 3 is the partition) of endpoint `ep`, from bit
// `first` up.
struct Run {
    std::uint8_t ch;
    std::uint8_t ep;
    std::uint8_t first;
    std::uint8_t len;
};

// The header after the mode bits, per mode 1-14 (mode m's runs are kBc6Runs[kBc6RunStart[m] ..
// kBc6RunStart[m + 1]]). Modes 13 and 14 write the high base bits in ascending order (what the console's packer
// does). Generated from VIctory's BC6_LAYOUT; every mode's mode bits, runs and indices add up to 128 bits.
constexpr Run kBc6Runs[246] = {
    // mode 1
    {1, 2, 4, 1}, {2, 2, 4, 1}, {2, 3, 4, 1}, {0, 0, 0, 10}, {1, 0, 0, 10}, {2, 0, 0, 10}, {0, 1, 0, 5},
    {1, 3, 4, 1}, {1, 2, 0, 4}, {1, 1, 0, 5}, {2, 3, 0, 1}, {1, 3, 0, 4}, {2, 1, 0, 5}, {2, 3, 1, 1}, {2, 2, 0, 4},
    {0, 2, 0, 5}, {2, 3, 2, 1}, {0, 3, 0, 5}, {2, 3, 3, 1}, {3, 0, 0, 5},
    // mode 2
    {1, 2, 5, 1}, {1, 3, 4, 1}, {1, 3, 5, 1}, {0, 0, 0, 7}, {2, 3, 0, 1}, {2, 3, 1, 1}, {2, 2, 4, 1}, {1, 0, 0, 7},
    {2, 2, 5, 1}, {2, 3, 2, 1}, {1, 2, 4, 1}, {2, 0, 0, 7}, {2, 3, 3, 1}, {2, 3, 5, 1}, {2, 3, 4, 1}, {0, 1, 0, 6},
    {1, 2, 0, 4}, {1, 1, 0, 6}, {1, 3, 0, 4}, {2, 1, 0, 6}, {2, 2, 0, 4}, {0, 2, 0, 6}, {0, 3, 0, 6}, {3, 0, 0, 5},
    // mode 3
    {0, 0, 0, 10}, {1, 0, 0, 10}, {2, 0, 0, 10}, {0, 1, 0, 5}, {0, 0, 10, 1}, {1, 2, 0, 4}, {1, 1, 0, 4},
    {1, 0, 10, 1}, {2, 3, 0, 1}, {1, 3, 0, 4}, {2, 1, 0, 4}, {2, 0, 10, 1}, {2, 3, 1, 1}, {2, 2, 0, 4}, {0, 2, 0, 5},
    {2, 3, 2, 1}, {0, 3, 0, 5}, {2, 3, 3, 1}, {3, 0, 0, 5},
    // mode 4
    {0, 0, 0, 10}, {1, 0, 0, 10}, {2, 0, 0, 10}, {0, 1, 0, 4}, {0, 0, 10, 1}, {1, 3, 4, 1}, {1, 2, 0, 4},
    {1, 1, 0, 5}, {1, 0, 10, 1}, {1, 3, 0, 4}, {2, 1, 0, 4}, {2, 0, 10, 1}, {2, 3, 1, 1}, {2, 2, 0, 4}, {0, 2, 0, 4},
    {2, 3, 0, 1}, {2, 3, 2, 1}, {0, 3, 0, 4}, {1, 2, 4, 1}, {2, 3, 3, 1}, {3, 0, 0, 5},
    // mode 5
    {0, 0, 0, 10}, {1, 0, 0, 10}, {2, 0, 0, 10}, {0, 1, 0, 4}, {0, 0, 10, 1}, {2, 2, 4, 1}, {1, 2, 0, 4},
    {1, 1, 0, 4}, {1, 0, 10, 1}, {2, 3, 0, 1}, {1, 3, 0, 4}, {2, 1, 0, 5}, {2, 0, 10, 1}, {2, 2, 0, 4}, {0, 2, 0, 4},
    {2, 3, 1, 1}, {2, 3, 2, 1}, {0, 3, 0, 4}, {2, 3, 4, 1}, {2, 3, 3, 1}, {3, 0, 0, 5},
    // mode 6
    {0, 0, 0, 9}, {2, 2, 4, 1}, {1, 0, 0, 9}, {1, 2, 4, 1}, {2, 0, 0, 9}, {2, 3, 4, 1}, {0, 1, 0, 5}, {1, 3, 4, 1},
    {1, 2, 0, 4}, {1, 1, 0, 5}, {2, 3, 0, 1}, {1, 3, 0, 4}, {2, 1, 0, 5}, {2, 3, 1, 1}, {2, 2, 0, 4}, {0, 2, 0, 5},
    {2, 3, 2, 1}, {0, 3, 0, 5}, {2, 3, 3, 1}, {3, 0, 0, 5},
    // mode 7
    {0, 0, 0, 8}, {1, 3, 4, 1}, {2, 2, 4, 1}, {1, 0, 0, 8}, {2, 3, 2, 1}, {1, 2, 4, 1}, {2, 0, 0, 8}, {2, 3, 3, 1},
    {2, 3, 4, 1}, {0, 1, 0, 6}, {1, 2, 0, 4}, {1, 1, 0, 5}, {2, 3, 0, 1}, {1, 3, 0, 4}, {2, 1, 0, 5}, {2, 3, 1, 1},
    {2, 2, 0, 4}, {0, 2, 0, 6}, {0, 3, 0, 6}, {3, 0, 0, 5},
    // mode 8
    {0, 0, 0, 8}, {2, 3, 0, 1}, {2, 2, 4, 1}, {1, 0, 0, 8}, {1, 2, 5, 1}, {1, 2, 4, 1}, {2, 0, 0, 8}, {1, 3, 5, 1},
    {2, 3, 4, 1}, {0, 1, 0, 5}, {1, 3, 4, 1}, {1, 2, 0, 4}, {1, 1, 0, 6}, {1, 3, 0, 4}, {2, 1, 0, 5}, {2, 3, 1, 1},
    {2, 2, 0, 4}, {0, 2, 0, 5}, {2, 3, 2, 1}, {0, 3, 0, 5}, {2, 3, 3, 1}, {3, 0, 0, 5},
    // mode 9
    {0, 0, 0, 8}, {2, 3, 1, 1}, {2, 2, 4, 1}, {1, 0, 0, 8}, {2, 2, 5, 1}, {1, 2, 4, 1}, {2, 0, 0, 8}, {2, 3, 5, 1},
    {2, 3, 4, 1}, {0, 1, 0, 5}, {1, 3, 4, 1}, {1, 2, 0, 4}, {1, 1, 0, 5}, {2, 3, 0, 1}, {1, 3, 0, 4}, {2, 1, 0, 6},
    {2, 2, 0, 4}, {0, 2, 0, 5}, {2, 3, 2, 1}, {0, 3, 0, 5}, {2, 3, 3, 1}, {3, 0, 0, 5},
    // mode 10
    {0, 0, 0, 6}, {1, 3, 4, 1}, {2, 3, 0, 1}, {2, 3, 1, 1}, {2, 2, 4, 1}, {1, 0, 0, 6}, {1, 2, 5, 1}, {2, 2, 5, 1},
    {2, 3, 2, 1}, {1, 2, 4, 1}, {2, 0, 0, 6}, {1, 3, 5, 1}, {2, 3, 3, 1}, {2, 3, 5, 1}, {2, 3, 4, 1}, {0, 1, 0, 6},
    {1, 2, 0, 4}, {1, 1, 0, 6}, {1, 3, 0, 4}, {2, 1, 0, 6}, {2, 2, 0, 4}, {0, 2, 0, 6}, {0, 3, 0, 6}, {3, 0, 0, 5},
    // mode 11
    {0, 0, 0, 10}, {1, 0, 0, 10}, {2, 0, 0, 10}, {0, 1, 0, 10}, {1, 1, 0, 10}, {2, 1, 0, 10},
    // mode 12
    {0, 0, 0, 10}, {1, 0, 0, 10}, {2, 0, 0, 10}, {0, 1, 0, 9}, {0, 0, 10, 1}, {1, 1, 0, 9}, {1, 0, 10, 1},
    {2, 1, 0, 9}, {2, 0, 10, 1},
    // mode 13
    {0, 0, 0, 10}, {1, 0, 0, 10}, {2, 0, 0, 10}, {0, 1, 0, 8}, {0, 0, 10, 2}, {1, 1, 0, 8}, {1, 0, 10, 2},
    {2, 1, 0, 8}, {2, 0, 10, 2},
    // mode 14
    {0, 0, 0, 10}, {1, 0, 0, 10}, {2, 0, 0, 10}, {0, 1, 0, 4}, {0, 0, 10, 6}, {1, 1, 0, 4}, {1, 0, 10, 6},
    {2, 1, 0, 4}, {2, 0, 10, 6},
};
constexpr std::uint16_t kBc6RunStart[16] = {0, 0, 20, 44, 63, 84, 105, 125, 145, 167, 189, 213, 219, 228, 237, 246};

constexpr bool bc6_layout_fills_128_bits() {
    for (unsigned mode = 1; mode <= 14; ++mode) {
        unsigned n = kBc6Code[mode].bits + (mode < 11 ? 46 : 63);
        for (unsigned r = kBc6RunStart[mode]; r < kBc6RunStart[mode + 1]; ++r) n += kBc6Runs[r].len;
        if (n != 128) return false;
    }
    return true;
}
static_assert(bc6_layout_fills_128_bits());

constexpr unsigned bc6_index_bits(unsigned mode) {
    return mode >= 1 && mode <= 10 ? 3 : mode >= 11 && mode <= 14 ? 4 : 0;
}

constexpr Bc6Entry kZeroBc6{{}, {11, {}}};

struct Bc6Sink {
    const Tables& t;
    std::uint8_t* out;
    std::size_t i = 0;
    Bc6Entry history[kHistory];

    Bc6Sink(const Tables& tables, std::uint8_t* dst) : t(tables), out(dst) {}

    static std::uint16_t z(unsigned v) { return static_cast<std::uint16_t>((v >> 1) ^ (0u - (v & 1))); }

    static unsigned wide(unsigned a, unsigned b) {
        unsigned acc = 0;
        for (unsigned k = 0; k < 8; ++k) acc |= (a >> k & 1) << (2 * k) | (b >> k & 1) << (2 * k + 1);
        return acc;
    }

    Error operator()(const std::uint8_t* s) {
        const References r = references(s, true);
        const auto mode = static_cast<std::uint8_t>(s[30] + 1);
        if (mode < 1 || mode > 14) return "BC6H mode out of range";
        const bool two = mode < 11;
        unsigned shift[4];
        for (unsigned k = 0; k < 4; ++k) shift[k] = 16u - kBc6Bits[mode][k];
        std::uint16_t res[4][3];
        for (unsigned c = 0; c < 3; ++c) {
            res[0][c] = z(wide(s[4 + 2 * c], s[5 + 2 * c]));
            res[1][c] = z(wide(s[10 + 2 * c], s[11 + 2 * c]));
            // Only the two-region modes use the residuals of endpoints 2 and 3.
            res[2][c] = z(s[16 + 2 * c]);
            res[3][c] = z(s[17 + 2 * c]);
        }
        const Bc6Ep& rep = pick(i, r.ep, history, t.bc6, kZeroBc6).ep;
        Bc6Entry entry{};
        // Endpoint 0 at the base precision, the others per channel; the reference is cut by the mask after the sum.
        const auto mask = [](unsigned sh) { return static_cast<std::uint16_t>(0xffffu << sh); };
        for (unsigned c = 0; c < 3; ++c)
            entry.ep.e[0][c] = static_cast<std::uint16_t>(
                (rep.e[0][c] + static_cast<std::uint16_t>(res[0][c] << shift[0])) & mask(shift[0]));
        const unsigned others = two ? 3 : 1;
        for (unsigned k = 1; k <= others; ++k)
            for (unsigned c = 0; c < 3; ++c)
                entry.ep.e[k][c] = static_cast<std::uint16_t>(
                    (rep.e[k][c] + static_cast<std::uint16_t>(res[k][c] << shift[1 + c])) & mask(shift[1 + c]));
        if (!two) {
            for (unsigned c = 0; c < 3; ++c)
                entry.ep.e[2][c] = static_cast<std::uint16_t>((entry.ep.e[0][c] >> shift[0]) << shift[1]);
            for (unsigned c = 0; c < 3; ++c) entry.ep.e[3][c] = entry.ep.e[1][c];
        }
        const Bc6Ix& rx = pick(i, r.ix, history, t.bc6, kZeroBc6).ix;
        const unsigned bits = bc6_index_bits(mode);
        const unsigned ref_bits = bc6_index_bits(rx.mode);
        entry.ix.mode = mode;
        for (unsigned k = 0; k < 8; ++k) {
            std::int16_t pair[2];
            nibbles(s[22 + k], k < 6, pair);
            for (unsigned h = 0; h < 2; ++h) {
                const unsigned p = 2 * k + h;
                entry.ix.ix[p] = add_index(pair[h], bits, rx.ix[p], ref_bits);
            }
        }
        history[i % kHistory] = entry;
        // Fields: the working value has its top bit flipped.
        const auto field = [&](unsigned k, unsigned c) -> std::uint64_t {
            const unsigned sh = k == 0 ? shift[0] : shift[1 + c];
            return static_cast<std::uint16_t>(entry.ep.e[k][c] ^ 0x8000) >> sh;
        };
        Bits w;
        w.put(kBc6Code[mode].code, kBc6Code[mode].bits);
        const unsigned part = s[31] & 31u;
        for (unsigned n = kBc6RunStart[mode]; n < kBc6RunStart[mode + 1]; ++n) {
            const Run& run = kBc6Runs[n];
            if (run.ch == 3) {
                w.put(part, 5);
            } else {
                w.put(field(run.ep, run.ch) >> run.first, run.len);
            }
        }
        const unsigned anchor = two ? kAnchor2[part] : 0;
        for (unsigned p = 0; p < 16; ++p) {
            const bool short_index = p == 0 || p == anchor;
            const auto v = static_cast<std::uint16_t>(static_cast<std::uint16_t>(entry.ix.ix[p]) + (1u << (bits - 1)));
            w.put(v, bits - (short_index ? 1 : 0));
        }
        w.store(out);
        out += 16;
        ++i;
        return nullptr;
    }
};

// A record per block, for unpack_records.
template <unsigned Stride>
struct RecordSink {
    std::uint8_t* out;
    Error operator()(const std::uint8_t* record) {
        std::memcpy(out, record, Stride);
        out += Stride;
        return nullptr;
    }
};

// One format's decode, in a function of its own so that only its history is on the stack.
template <unsigned N, unsigned Per, class Sink>
BCPACK_NOINLINE Error decode_as(const Tables& t, const std::uint8_t* widths, std::size_t blocks,
                                const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst) {
    BufferFetch fetch{src, src + src_size};
    Sink sink(t, dst);
    return run_lanes<N, Per>(t, widths, blocks, fetch, sink);
}

}  // namespace

TablesPtr parse_tables(const std::uint8_t* bytes, std::size_t size, Error* error) {
    const auto fail = [&](Error why) {
        if (error) *error = why;
        return TablesPtr();
    };
    if (!bytes || size != kTablesSize) return fail("table file has the wrong size");
    std::unique_ptr<Tables> t(new (std::nothrow) Tables());
    if (!t) return fail("out of memory for the tables");
    const std::uint8_t* p = bytes;
    const auto u8 = [&p] { return *p++; };
    const auto u16 = [&p] {
        const auto v = static_cast<std::uint16_t>(p[0] | p[1] << 8);
        p += 2;
        return v;
    };
    for (unsigned m = 0; m < kModels; ++m) {
        Model& model = t->models[m];
        std::uint32_t f[kBuckets];
        for (unsigned b = 0; b < kBuckets; ++b) f[b] = u16();
        // Model 16 g + shape has g + 2 buckets; bucket b > 0 holds 2^(b-1) symbols.
        const unsigned used = m / 16 + 2;
        std::uint32_t start[kBuckets + 1];
        std::uint32_t at = 0;
        for (unsigned b = 0; b < used; ++b) {
            if (f[b] == 0) return fail("a model has an empty bucket");
            start[b] = at;
            at += f[b] << (b > 0 ? b - 1 : 0);
        }
        if (at != 1u << 16) return fail("a model does not tile 65536");
        for (unsigned b = used; b <= kBuckets; ++b) start[b] = 1u << 16;
        for (unsigned window = 0; window < 256; ++window) {
            const std::uint32_t first = window << 8;
            unsigned bucket = 0;
            for (unsigned b = 1; b <= kBuckets; ++b) bucket += start[b] <= first ? 1 : 0;
            model.bucket[window] = static_cast<std::uint8_t>(bucket);
        }
        for (unsigned b = 0; b <= kBuckets; ++b) model.start[b] = start[b];
        for (unsigned b = 0; b < kBuckets; ++b) {
            model.freq[b] = f[b];
            model.recip[b] = b > 0 && b < used ? ((std::uint64_t{1} << 32) + f[b] - 1) / f[b] : 0;
        }
        model.start[0] = 1;
        model.freq[0] = f[0] - 1;
    }
    const auto bytes_into = [&p](std::uint8_t* dst, std::size_t n) {
        std::memcpy(dst, p, n);
        p += n;
    };
    bytes_into(t->rank1, sizeof t->rank1);
    bytes_into(t->rank3a, sizeof t->rank3a);
    bytes_into(t->rank3c, sizeof t->rank3c);
    bytes_into(t->rank4, sizeof t->rank4);
    bytes_into(t->rank5, sizeof t->rank5);
    for (std::uint16_t& c : t->ctrl7) c = u16();
    const auto plain = [&](Plain* dictionary, unsigned ep, unsigned ix) {
        for (unsigned k = 0; k < kDictionary; ++k) {
            for (unsigned v = 0; v < ep; ++v) dictionary[k].ep[v] = u8();
            for (unsigned v = 0; v < ix; ++v) dictionary[k].ix[v] = u8();
        }
    };
    plain(t->bc1, 6, 16);
    plain(t->bc3, 8, 32);
    plain(t->bc4, 2, 16);
    plain(t->bc5, 4, 32);
    for (Bc6Entry& e : t->bc6) {
        // The endpoint entry's mode byte plays no part in decoding.
        u8();
        for (unsigned v = 0; v < 12; ++v) e.ep.e[v / 3][v % 3] = u16();
        e.ix.mode = u8();
        for (std::int16_t& v : e.ix.ix) v = static_cast<std::int16_t>(u16());
    }
    for (Bc7Entry& e : t->bc7) {
        e.ep.ctrl = u16();
        for (unsigned v = 0; v < 24; ++v) e.ep.rows[v / 8][v % 8] = u16();
        e.ix.bits[0] = u8();
        e.ix.bits[1] = u8();
        // Unused: the widths again in all entries but the first.
        u16();
        // Only 15 slots for set 1, which no entry uses (0 bits).
        for (unsigned v = 0; v < 31; ++v) e.ix.ix[v / 16][v % 16] = static_cast<std::int16_t>(u16());
    }
    if (p != bytes + size) return fail("table layout does not add up");
    return TablesPtr(t.release());
}

bool layout(unsigned bc, Layout* out) {
    switch (bc) {
    case 1:
        *out = {kBc1Lanes, 12, 1, 8};
        return true;
    case 3:
        *out = {kBc3Lanes, 12, 2, 16};
        return true;
    case 4:
        *out = {kBc4Lanes, 12, 1, 8};
        return true;
    case 5:
        *out = {kBc5Lanes, 12, 2, 16};
        return true;
    case 6:
        *out = {kBc6Lanes, 16, 2, 16};
        return true;
    case 7:
        *out = {kBc7Lanes, 16, 2, 16};
        return true;
    default:
        return false;
    }
}

Error decode(unsigned bc, const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst, std::size_t dst_size,
             const Tables& tables) {
    Layout l{};
    if (!layout(bc, &l)) return "unsupported BC format";
    if (dst_size == 0 || dst_size % l.block_bytes != 0) return "destination is not a whole number of blocks";
    if (!dst) return "no destination";
    if (!src && src_size != 0) return "no source";
    const std::size_t blocks = dst_size / l.block_bytes;
    switch (bc) {
    case 1:
        return decode_as<12, 1, PlainSink<Bc1>>(tables, l.widths, blocks, src, src_size, dst);
    case 3:
        return decode_as<12, 2, PlainSink<Bc3>>(tables, l.widths, blocks, src, src_size, dst);
    case 4:
        return decode_as<12, 1, PlainSink<Bc4>>(tables, l.widths, blocks, src, src_size, dst);
    case 5:
        return decode_as<12, 2, PlainSink<Bc5>>(tables, l.widths, blocks, src, src_size, dst);
    case 6:
        return decode_as<16, 2, Bc6Sink>(tables, l.widths, blocks, src, src_size, dst);
    default:
        return decode_as<16, 2, Bc7Sink>(tables, l.widths, blocks, src, src_size, dst);
    }
}

Error unpack_records(unsigned bc, const Tables& tables, std::size_t blocks, int (*fetch)(void* context, unsigned lane),
                     void* context, std::uint8_t* records) {
    Layout l{};
    if (!layout(bc, &l)) return "unsupported BC format";
    if (blocks == 0) return nullptr;
    if (!fetch || !records) return "no input or no output";
    CallbackFetch input{fetch, context};
    if (l.lanes == 12 && l.per == 1) {
        RecordSink<12> sink{records};
        return run_lanes<12, 1>(tables, l.widths, blocks, input, sink);
    }
    if (l.lanes == 12) {
        RecordSink<24> sink{records};
        return run_lanes<12, 2>(tables, l.widths, blocks, input, sink);
    }
    RecordSink<32> sink{records};
    return run_lanes<16, 2>(tables, l.widths, blocks, input, sink);
}

void sha256(const void* data, std::size_t size, std::uint8_t digest[32]) {
    static constexpr std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto compress = [&h](const std::uint8_t* block) {
        std::uint32_t w[64];
        for (unsigned i = 0; i < 16; ++i)
            w[i] = std::uint32_t{block[4 * i]} << 24 | std::uint32_t{block[4 * i + 1]} << 16 |
                   std::uint32_t{block[4 * i + 2]} << 8 | block[4 * i + 3];
        for (unsigned i = 16; i < 64; ++i) {
            const std::uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (unsigned i = 0; i < 64; ++i) {
            const std::uint32_t t1 =
                hh + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 =
                (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    };
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t n = size;
    for (; n >= 64; n -= 64, bytes += 64) compress(bytes);
    std::uint8_t tail[128] = {};
    if (n != 0) std::memcpy(tail, bytes, n);
    tail[n] = 0x80;
    const std::size_t padded = n + 9 <= 64 ? 64 : 128;
    const std::uint64_t bit_count = std::uint64_t{size} * 8;
    for (unsigned i = 0; i < 8; ++i) tail[padded - 1 - i] = static_cast<std::uint8_t>(bit_count >> (8 * i));
    compress(tail);
    if (padded == 128) compress(tail + 64);
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned j = 0; j < 4; ++j) digest[4 * i + j] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * j));
}

TablesPtr load_tables(const std::filesystem::path& path, std::string* error) {
    const auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return TablesPtr();
    };
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("cannot open " + utf8(path));
    // One byte more than the file should hold tells a longer file apart without reading it all.
    std::vector<std::uint8_t> bytes(kTablesSize + 1);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    const auto n = static_cast<std::size_t>(in.gcount());
    if (n != kTablesSize)
        return fail(utf8(path) + " is not the BCPack table file (" +
                    (n > kTablesSize ? std::string("larger than") : std::to_string(n) + " bytes,") + " expected " +
                    std::to_string(kTablesSize) + " bytes)");
    std::uint8_t digest[32];
    sha256(bytes.data(), n, digest);
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string hex;
    for (const std::uint8_t b : digest) {
        hex += kHex[b >> 4];
        hex += kHex[b & 15];
    }
    if (hex != kTablesSha256) return fail(utf8(path) + " has SHA-256 " + hex + ", not the pinned " + kTablesSha256);
    Error why = nullptr;
    TablesPtr tables = parse_tables(bytes.data(), n, &why);
    if (!tables) return fail(utf8(path) + ": " + why);
    return tables;
}

std::filesystem::path tables_path() {
#if defined(_WIN32)
    std::wstring value(32768, L'\0');
    DWORD n = GetEnvironmentVariableW(L"LEONIDA_BCPACK_TABLES", value.data(), static_cast<DWORD>(value.size()));
    if (n != 0 && n < value.size()) {
        value.resize(n);
        return std::filesystem::path(value);
    }
    std::wstring self(32768, L'\0');
    n = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    if (n != 0 && n < self.size()) {
        self.resize(n);
        return std::filesystem::path(self).parent_path() / kTablesFile;
    }
    return {};
#else
    if (const char* value = std::getenv(kTablesVariable); value && *value) return std::filesystem::path(value);
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && !self.empty()) return self.parent_path() / kTablesFile;
    return {};
#endif
}

const Tables* process_tables() {
    // Loaded once, by the first caller, and kept for the life of the process (VIctory's OnceLock).
    static const Tables* const tables = [] {
        const std::filesystem::path path = tables_path();
        std::string why = "cannot find the executable's folder";
        TablesPtr loaded;
        if (!path.empty()) loaded = load_tables(path, &why);
        if (loaded) {
            std::fprintf(stderr, "[rt] bcpack: format tables from %s\n", utf8(path).c_str());
        } else {
            std::fprintf(stderr,
                         "[rt] bcpack: BCPack requests fail: %s. Set %s to the verified %s (SHA-256 %s) or put it "
                         "next to the executable (logged once)\n",
                         why.c_str(), kTablesVariable, kTablesFile, kTablesSha256);
        }
        return loaded.release();
    }();
    return tables;
}

}  // namespace rt::bcpack

bool leonida_bcpack_decode(std::uint32_t format, const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst,
                           std::size_t dst_size) {
    const rt::bcpack::Tables* tables = rt::bcpack::process_tables();
    if (!tables) return false;
    const rt::bcpack::Error error = rt::bcpack::decode(format, src, src_size, dst, dst_size, *tables);
    if (!error) return true;
    // VIctory warns for every failed request; the first few are enough to see what goes wrong.
    static std::atomic<unsigned> reported{0};
    if (reported.fetch_add(1, std::memory_order_relaxed) < 16)
        std::fprintf(stderr,
                     "[rt] bcpack: a BC%u request (%zu bytes packed, %zu decoded) fails: %s (first 16 logged)\n",
                     format, src_size, dst_size, error);
    return false;
}
