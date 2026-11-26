#pragma once

// PNG files for the presented-frame screenshots (shims/xbox_window.cpp, LEONIDA_CAPTURE_DIR): 8-bit RGB, one IDAT
// chunk, a zlib stream (RFC 1950) of one deflate block (RFC 1951) with the fixed Huffman codes and greedy LZ77
// matches over a 32 KiB window. Each row gets the PNG filter (None, Sub, Up or Paeth) whose output has the smallest sum
// of absolute values, the usual heuristic. No third-party code and no Windows headers; the round trip is checked
// against leonida::zlib_inflate (src/shader_library.h) by png_write_test.
//
// VIctory (GPL-3.0-or-later, Marvisak and contributors, DexNom fork; victory/crates/victory-gpu/src/capture.rs
// `save_as`, `chunk`, `crc32`) writes its frames as 8-bit RGB PNGs with the same chunk layout and CRC; it compresses
// with zlib-rs and halves the frame. This writer keeps the full size (a check reads the presented pixels) and carries
// its own deflate.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

namespace leonida::png {

// CRC-32 (ISO 3309, the one PNG chunks carry), continued from `crc` (0 to start).
inline std::uint32_t crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc = 0) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (const std::uint8_t b : bytes) crc = table[(crc ^ b) & 0xff] ^ (crc >> 8);
    return ~crc;
}

inline std::uint32_t adler32(std::span<const std::uint8_t> bytes) {
    std::uint32_t a = 1, b = 0;
    std::size_t at = 0;
    while (at < bytes.size()) {
        const std::size_t run = std::min<std::size_t>(bytes.size() - at, 5552);  // no overflow before the modulo
        for (std::size_t i = 0; i < run; ++i) {
            a += bytes[at + i];
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
        at += run;
    }
    return (b << 16) | a;
}

namespace detail {

class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}
    // `count` bits of `value`, least significant first (deflate's order for everything but Huffman codes).
    void bits(std::uint32_t value, unsigned count) {
        held_ |= std::uint64_t(value) << used_;
        used_ += count;
        while (used_ >= 8) {
            out_.push_back(std::uint8_t(held_));
            held_ >>= 8;
            used_ -= 8;
        }
    }
    // A Huffman code of `length` bits, most significant bit first.
    void code(std::uint32_t code, unsigned length) {
        std::uint32_t reversed = 0;
        for (unsigned i = 0; i < length; ++i) reversed |= ((code >> i) & 1u) << (length - 1 - i);
        bits(reversed, length);
    }
    void flush() {
        if (used_) out_.push_back(std::uint8_t(held_));
        held_ = 0;
        used_ = 0;
    }

private:
    std::vector<std::uint8_t>& out_;
    std::uint64_t held_ = 0;
    unsigned used_ = 0;
};

// The fixed literal/length code (RFC 1951 3.2.6).
inline void literal(BitWriter& w, unsigned symbol) {
    if (symbol < 144) w.code(0x30 + symbol, 8);
    else if (symbol < 256) w.code(0x190 + (symbol - 144), 9);
    else if (symbol < 280) w.code(symbol - 256, 7);
    else w.code(0xc0 + (symbol - 280), 8);
}

inline constexpr std::uint16_t length_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
inline constexpr std::uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
inline constexpr std::uint16_t distance_base[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                                     33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                                     1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
inline constexpr std::uint8_t distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

inline void match(BitWriter& w, unsigned length, unsigned distance) {
    unsigned l = 28;
    while (length_base[l] > length) --l;
    literal(w, 257 + l);
    if (length_extra[l]) w.bits(length - length_base[l], length_extra[l]);
    unsigned d = 29;
    while (distance_base[d] > distance) --d;
    w.code(d, 5);
    if (distance_extra[d]) w.bits(distance - distance_base[d], distance_extra[d]);
}

}  // namespace detail

// Raw deflate data (one final block, fixed Huffman codes) for `in`, appended to `out`. `chain` is how many earlier
// positions with the same three bytes are tried for a match (more: smaller and slower).
inline void deflate_fixed(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, unsigned chain = 16) {
    constexpr std::size_t window = 32768, min_match = 3, max_match = 258;
    constexpr unsigned hash_bits = 15;
    detail::BitWriter w(out);
    w.bits(1, 1);  // BFINAL
    w.bits(1, 2);  // BTYPE 01: fixed Huffman codes
    std::vector<std::uint32_t> head(std::size_t(1) << hash_bits, 0);  // position + 1 of the newest with this hash
    std::vector<std::uint32_t> previous(window, 0);                   // the one before it, by position % window
    const std::size_t n = in.size();
    const auto hash = [&](std::size_t at) {
        const std::uint32_t v = std::uint32_t(in[at]) | std::uint32_t(in[at + 1]) << 8 | std::uint32_t(in[at + 2]) << 16;
        return (v * 2654435761u) >> (32 - hash_bits);
    };
    const auto insert = [&](std::size_t at) {
        const std::uint32_t h = hash(at);
        previous[at % window] = head[h];
        head[h] = std::uint32_t(at + 1);
    };
    std::size_t at = 0;
    while (at < n) {
        std::size_t best_length = 0, best_distance = 0;
        if (at + min_match <= n) {
            const std::size_t limit = std::min(max_match, n - at);
            std::uint32_t candidate = head[hash(at)];
            for (unsigned tries = 0; candidate && tries < chain; ++tries) {
                const std::size_t from = candidate - 1;
                if (at - from > window - 1) break;  // deflate's distances stop at 32768; keep one back for safety
                if (in[from + best_length] == in[at + best_length]) {
                    std::size_t length = 0;
                    while (length < limit && in[from + length] == in[at + length]) ++length;
                    if (length > best_length) {
                        best_length = length;
                        best_distance = at - from;
                        if (length == limit) break;
                    }
                }
                const std::uint32_t next = previous[from % window];
                if (next >= candidate) break;  // the slot was reused by a newer position: the chain ends here
                candidate = next;
            }
        }
        if (best_length >= min_match) {
            detail::match(w, unsigned(best_length), unsigned(best_distance));
            for (std::size_t i = 0; i < best_length; ++i)
                if (at + i + min_match <= n) insert(at + i);
            at += best_length;
        } else {
            detail::literal(w, in[at]);
            if (at + min_match <= n) insert(at);
            ++at;
        }
    }
    detail::literal(w, 256);  // end of block
    w.flush();
}

// A zlib stream (RFC 1950) of `in`.
inline std::vector<std::uint8_t> zlib_compress(std::span<const std::uint8_t> in) {
    std::vector<std::uint8_t> out{0x78, 0x01};  // deflate, 32 KiB window, no dictionary; (0x7801 % 31) == 0
    out.reserve(in.size() / 2 + 64);
    deflate_fixed(in, out);
    const std::uint32_t adler = adler32(in);
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(std::uint8_t(adler >> shift));
    return out;
}

namespace detail {

inline std::uint8_t paeth(std::uint8_t a, std::uint8_t b, std::uint8_t c) {
    const int p = int(a) + int(b) - int(c);
    const int pa = std::abs(p - int(a)), pb = std::abs(p - int(b)), pc = std::abs(p - int(c));
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

inline void put_chunk(std::vector<std::uint8_t>& png, const char (&kind)[5], std::span<const std::uint8_t> data) {
    const std::uint32_t length = std::uint32_t(data.size());
    for (int shift = 24; shift >= 0; shift -= 8) png.push_back(std::uint8_t(length >> shift));
    const std::size_t start = png.size();
    png.insert(png.end(), kind, kind + 4);
    png.insert(png.end(), data.begin(), data.end());
    const std::uint32_t crc = crc32(std::span<const std::uint8_t>(png.data() + start, png.size() - start));
    for (int shift = 24; shift >= 0; shift -= 8) png.push_back(std::uint8_t(crc >> shift));
}

}  // namespace detail

// A PNG of 8-bit RGB pixels: `height` rows of `width * 3` bytes, `pitch` bytes apart. Empty for a zero size.
inline std::vector<std::uint8_t> encode_rgb8(std::uint32_t width, std::uint32_t height, const std::uint8_t* rgb,
                                             std::size_t pitch) {
    if (!width || !height || !rgb) return {};
    const std::size_t row = std::size_t(width) * 3;
    std::vector<std::uint8_t> filtered;
    filtered.reserve((row + 1) * height);
    std::array<std::vector<std::uint8_t>, 4> candidates;  // None, Sub, Up and Paeth (Average is not tried)
    for (auto& c : candidates) c.resize(row);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* line = rgb + std::size_t(y) * pitch;
        const std::uint8_t* above = y ? rgb + std::size_t(y - 1) * pitch : nullptr;
        for (std::size_t i = 0; i < row; ++i) {
            const std::uint8_t left = i >= 3 ? line[i - 3] : 0;
            const std::uint8_t up = above ? above[i] : 0;
            const std::uint8_t corner = above && i >= 3 ? above[i - 3] : 0;
            candidates[0][i] = line[i];
            candidates[1][i] = std::uint8_t(line[i] - left);
            candidates[2][i] = std::uint8_t(line[i] - up);
            candidates[3][i] = std::uint8_t(line[i] - detail::paeth(left, up, corner));
        }
        static constexpr std::uint8_t type[4] = {0, 1, 2, 4};
        std::size_t best = 0;
        std::uint64_t best_sum = ~std::uint64_t(0);
        for (std::size_t c = 0; c < 4; ++c) {
            std::uint64_t sum = 0;
            for (const std::uint8_t v : candidates[c]) sum += v < 128 ? v : 256 - v;
            if (sum < best_sum) {
                best_sum = sum;
                best = c;
            }
        }
        filtered.push_back(type[best]);
        filtered.insert(filtered.end(), candidates[best].begin(), candidates[best].end());
    }
    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::uint8_t header[13] = {};
    for (int i = 0; i < 4; ++i) {
        header[i] = std::uint8_t(width >> (24 - 8 * i));
        header[4 + i] = std::uint8_t(height >> (24 - 8 * i));
    }
    header[8] = 8;  // bits per channel
    header[9] = 2;  // colour type 2: RGB
    detail::put_chunk(png, "IHDR", header);
    detail::put_chunk(png, "IDAT", zlib_compress(filtered));
    detail::put_chunk(png, "IEND", {});
    return png;
}

}  // namespace leonida::png
