// XMA2 frame decoder. Ported from VIctory (crates/victory-formats/src/xma.rs), GPL-3.0-or-later, by Marvisak and
// contributors (DexNom fork). Leonida is GPL-3.0-or-later. See xma_decode.h for what it covers and which of VIctory's
// later fixes are kept.
#include "xma_decode.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>

#include "xma_tables.h"

namespace rt::xma {

namespace detail {

// Bitstream reading inside one gathered frame, MSB first. Reads past the data see zeros; `end` is where the frame's
// bits end, so a decoder that read further has overread.
struct BitReader {
    std::span<const uint8_t> data;
    size_t pos = 0;
    size_t end = 0;

    uint32_t peek32() const {
        uint64_t v = 0;
        const size_t byte = pos / 8;
        for (size_t i = 0; i < 5; ++i) v = v << 8 | (byte + i < data.size() ? data[byte + i] : 0u);
        return static_cast<uint32_t>((v << (24 + pos % 8)) >> 32);
    }

    uint32_t read(uint32_t n) {
        if (n == 0) return 0;
        const uint32_t v = peek32() >> (32 - n);
        pos += n;
        return v;
    }

    bool bit() { return read(1) != 0; }

    int32_t sign_extend(uint32_t n) { return static_cast<int32_t>(read(n) << (32 - n)) >> (32 - n); }

    bool overread() const { return pos > end; }
};

}  // namespace detail

namespace {

using detail::BitReader;

// 512 >> 2: the smallest subframe.
constexpr size_t kMinSubframe = kSubframeSamples;

uint32_t log2_floor(size_t v) {
    v = std::max<size_t>(v, 1);
    uint32_t r = 0;
    while (v >>= 1) ++r;
    return r;
}

// Packets.

// Bit positions in a buffer of packets, skipping each packet's header and the packets of other streams.
struct Cursor {
    std::span<const uint8_t> data;
    size_t pos = 0;

    bool bit(uint32_t& out) {
        // Past the data nothing is read, whatever the skips would say; checking first keeps the packet arithmetic
        // below from overflowing for a position far past the end.
        if (pos / 8 >= data.size()) return false;
        if (pos % kPacketBits == 0) {
            // Leaving a packet: jump over the other streams' packets.
            const size_t previous = pos / kPacketBits;
            if (previous > 0) {
                const auto header = packet(data, previous - 1);
                if (!header) return false;
                pos = (previous + header->skip) * kPacketBits;
            }
            pos += kHeaderBits;
        }
        if (pos / 8 >= data.size()) return false;
        out = (data[pos / 8] >> (7 - pos % 8)) & 1u;
        ++pos;
        return true;
    }

    bool bits(uint32_t n, uint32_t& out) {
        uint32_t v = 0;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t b = 0;
            if (!bit(b)) return false;
            v = v << 1 | b;
        }
        out = v;
        return true;
    }

    // A payload byte, MSB first. At a packet edge the individual bits may be separated by a header and other
    // streams' packets.
    bool byte(uint8_t& out) {
        const size_t in_packet = pos % kPacketBits;
        if (in_packet == 0 || in_packet > kPacketBits - 8) {
            uint32_t v = 0;
            if (!bits(8, v)) return false;
            out = static_cast<uint8_t>(v);
            return true;
        }
        const size_t at = pos / 8;
        const unsigned shift = static_cast<unsigned>(pos % 8);
        if (at >= data.size()) return false;
        const unsigned first = data[at];
        if (shift == 0) {
            out = static_cast<uint8_t>(first);
        } else {
            if (at + 1 >= data.size()) return false;
            out = static_cast<uint8_t>(((first << shift) | (data[at + 1] >> (8 - shift))) & 0xffu);
        }
        pos += 8;
        return true;
    }
};

// Where the frame after the one at `start`..`end` begins: right after it, unless it was the last frame to begin in its
// packet and the rest of that packet is padding.
std::optional<size_t> next_frame(std::span<const uint8_t> data, size_t start, size_t end) {
    const size_t index = start / kPacketBits;
    const auto header = packet(data, index);
    if (!header) return std::nullopt;
    size_t at = index * kPacketBits + kHeaderBits + header->first_frame_bit;
    uint32_t number = 0;
    while (at < start) {
        Cursor cursor{data, at};
        uint32_t bits = 0;
        if (!cursor.bits(kLengthBits, bits)) return std::nullopt;
        // The same rule as read_frame: a length that does not cover its own field is not a frame. A zero length
        // would never move `at`, so a damaged packet would loop here for good.
        if (bits <= kLengthBits) return std::nullopt;
        at += bits;
        ++number;
    }
    const bool last = number + 1 >= header->frames;
    // A frame that ends in a packet where no frame begins (the tail of a frame that crossed packets, then padding)
    // is the last one before the next packet that has frames.
    bool ends_in_packet_without_frames = false;
    if (end / kPacketBits != index && end % kPacketBits != 0) {
        const auto there = packet(data, end / kPacketBits);
        if (!there) return std::nullopt;
        ends_in_packet_without_frames = there->frames == 0;
    }
    if (end % kPacketBits == 0 || (last && end / kPacketBits == index) || ends_in_packet_without_frames) {
        // The next frame begins in the next packet of this stream that has one; a packet with none (a frame spans
        // all of it) holds no start.
        const auto ending = packet(data, (end - 1) / kPacketBits);
        if (!ending) return std::nullopt;
        size_t next = (end - 1) / kPacketBits + 1 + ending->skip;
        auto following = packet(data, next);
        if (!following) return std::nullopt;
        while (following->frames == 0) {
            next += 1 + following->skip;
            following = packet(data, next);
            if (!following) return std::nullopt;
        }
        end = next * kPacketBits + kHeaderBits + following->first_frame_bit;
    }
    if (end / 8 < data.size()) return end;
    return std::nullopt;
}

// A prefix code built like FFmpeg's ff_vlc_init_from_lengths: codes are handed out in table order, each the next free
// one of its length.
class Vlc {
public:
    void add(int32_t symbol, uint8_t length) {
        codes_.push_back({static_cast<uint32_t>(next_), length, symbol});
        next_ += uint64_t{1} << (32 - length);
    }

    int32_t read(BitReader& b) const {
        const uint32_t v = b.peek32();
        // The last code at or below v; the first code is 0, so there is always one.
        auto it = std::upper_bound(codes_.begin(), codes_.end(), v,
                                   [](uint32_t value, const Code& c) { return value < c.code; });
        const Code& c = *(it - 1);
        b.pos += c.length;
        return c.symbol;
    }

private:
    struct Code {
        uint32_t code;  // left-aligned
        uint8_t length;
        int32_t symbol;
    };
    std::vector<Code> codes_;  // sorted by code
    uint64_t next_ = 0;
};

struct Codes {
    Vlc scale, scale_rl, coef[2], vec4, vec2, vec1;
};

template <size_t N>
void add_pairs(Vlc& vlc, const tables::SymbolLength (&pairs)[N], int32_t offset) {
    for (const auto& p : pairs) vlc.add(static_cast<int32_t>(p.symbol) + offset, p.length);
}

const Codes& codes() {
    static const Codes built = [] {
        Codes c;
        add_pairs(c.scale, tables::kScale, -60);
        add_pairs(c.scale_rl, tables::kScaleRl, 0);
        static_assert(std::size(tables::kCoef0Syms) == std::size(tables::kCoef0Lens));
        for (size_t i = 0; i < std::size(tables::kCoef0Syms); ++i)
            c.coef[0].add(static_cast<int32_t>(tables::kCoef0Syms[i]), tables::kCoef0Lens[i]);
        add_pairs(c.coef[1], tables::kCoef1, 0);
        static_assert(std::size(tables::kVec4Syms) == std::size(tables::kVec4Lens));
        for (size_t i = 0; i < std::size(tables::kVec4Syms); ++i)
            c.vec4.add(static_cast<int32_t>(tables::kVec4Syms[i]) - 1, tables::kVec4Lens[i]);
        add_pairs(c.vec2, tables::kVec2, -1);
        add_pairs(c.vec1, tables::kVec1, 0);
        return c;
    }();
    return built;
}

// ff_wma_get_large_val.
uint32_t large_value(BitReader& b) {
    uint32_t n = 8;
    if (b.bit()) {
        n += 8;
        if (b.bit()) {
            n += 8;
            if (b.bit()) n += 7;
        }
    }
    return b.read(n);
}

// The inverse transform.

// Everything imdct_half needs for one size `n` that does not depend on the coefficients: the pre-rotation
// exp(-iπk/n) and post-rotation exp(-iπ(4k+1)/4n) of each of the n/2 FFT points, the FFT's bit-reversed order and its
// butterfly rotations. Each value is computed with the expression (for the butterflies the running product) the
// transform would work out on every call, so it gives the same bits.
struct Rotations {
    std::vector<double> pre_re, pre_im, post_re, post_im;
    std::vector<uint16_t> reversed;  // where the bit-reversal permutation puts each point
    // The butterfly rotations of every stage, the stage of length `len` starting at len / 2 - 1.
    std::vector<double> twiddle_re, twiddle_im;

    explicit Rotations(size_t n) {
        const size_t m = n / 2;
        constexpr double pi = std::numbers::pi;
        for (size_t k = 0; k < m; ++k) {
            const double a = -pi * static_cast<double>(k) / static_cast<double>(n);
            pre_re.push_back(std::cos(a));
            pre_im.push_back(std::sin(a));
            const double b = -pi * static_cast<double>(4 * k + 1) / static_cast<double>(4 * n);
            post_re.push_back(std::cos(b));
            post_im.push_back(std::sin(b));
        }
        // The swaps of the in-place bit reversal, applied to the identity.
        for (size_t i = 0; i < m; ++i) reversed.push_back(static_cast<uint16_t>(i));
        size_t j = 0;
        for (size_t i = 1; i < m; ++i) {
            size_t bit = m >> 1;
            while (j & bit) {
                j ^= bit;
                bit >>= 1;
            }
            j |= bit;
            if (i < j) std::swap(reversed[i], reversed[j]);
        }
        for (size_t len = 2; len <= m; len <<= 1) {
            const double a = -2.0 * pi / static_cast<double>(len);
            const double ws = std::sin(a), wc = std::cos(a);
            double cr = 1.0, ci = 0.0;
            for (size_t k = 0; k < len / 2; ++k) {
                twiddle_re.push_back(cr);
                twiddle_im.push_back(ci);
                const double nr = cr * wc - ci * ws;
                const double ni = cr * ws + ci * wc;
                cr = nr;
                ci = ni;
            }
        }
    }

    // The butterfly stages of an in-place radix-2 forward DFT (exp(-2πi nk/N)) on m points already in bit-reversed
    // order.
    void butterflies(double* re, double* im, size_t m) const {
        for (size_t len = 2; len <= m; len <<= 1) {
            const size_t half = len / 2;
            const double* wr = twiddle_re.data() + (half - 1);
            const double* wi = twiddle_im.data() + (half - 1);
            for (size_t start = 0; start < m; start += len) {
                for (size_t k = 0; k < half; ++k) {
                    const size_t a = start + k, b = a + half;
                    const double tr = re[b] * wr[k] - im[b] * wi[k];
                    const double ti = re[b] * wi[k] + im[b] * wr[k];
                    re[b] = re[a] - tr;
                    im[b] = im[a] - ti;
                    re[a] = re[a] + tr;
                    im[a] = im[a] + ti;
                }
            }
        }
    }
};

// Rotations for subframes of 128, 256 and 512 samples.
const Rotations* cached_rotations(size_t n) {
    static const Rotations r128(128), r256(256), r512(512);
    switch (n) {
        case 128: return &r128;
        case 256: return &r256;
        case 512: return &r512;
        default: return nullptr;
    }
}

// Sine windows for blocks of 128, 256 and 512 samples.
const std::vector<float>& window_for(size_t winlen) {
    static const std::vector<float> w[3] = {sine_window(128), sine_window(256), sine_window(512)};
    return w[log2_floor(winlen) - 7];
}

int16_t to_i16(float v) {
    const float s = std::round(v * 32768.0f);
    if (s != s) return 0;  // NaN, as Rust's saturating cast gives
    if (s <= -32768.0f) return -32768;
    if (s >= 32767.0f) return 32767;
    return static_cast<int16_t>(s);
}

}  // namespace

std::optional<Packet> packet(std::span<const uint8_t> data, size_t index) {
    if (index > (SIZE_MAX - 4) / kPacketBytes) return std::nullopt;
    const size_t start = index * kPacketBytes;
    if (start > data.size() || data.size() - start < 4) return std::nullopt;
    const uint32_t h = static_cast<uint32_t>(data[start]) << 24 | static_cast<uint32_t>(data[start + 1]) << 16 |
                       static_cast<uint32_t>(data[start + 2]) << 8 | data[start + 3];
    return Packet{h >> 26, (h >> 11) & 0x7fffu, h & 0xffu};
}

std::optional<Frame> read_frame(std::span<const uint8_t> data, size_t start) {
    Cursor cursor{data, start};
    uint32_t length = 0;
    if (!cursor.bits(kLengthBits, length)) return std::nullopt;
    const size_t bits = length;
    if (bits <= kLengthBits || bits == 0x7fff) return std::nullopt;
    Frame frame;
    frame.data.reserve((bits + 7) / 8);
    Cursor gather{data, start};
    for (size_t i = 0; i < bits / 8; ++i) {
        uint8_t v = 0;
        if (!gather.byte(v)) return std::nullopt;
        frame.data.push_back(v);
    }
    const uint32_t tail = static_cast<uint32_t>(bits % 8);
    if (tail != 0) {
        uint32_t v = 0;
        if (!gather.bits(tail, v)) return std::nullopt;
        frame.data.push_back(static_cast<uint8_t>(v << (8 - tail)));
    }
    frame.bits = bits;
    frame.start = start;
    frame.next = next_frame(data, start, gather.pos);
    return frame;
}

void imdct_half(std::span<const float> input, std::span<float> out, float scale) {
    const size_t n = input.size();
    const size_t m = n / 2;
    if (m == 0 || out.size() < n) return;
    const Rotations* rotations = cached_rotations(n);
    std::optional<Rotations> built;
    if (!rotations) rotations = &built.emplace(n);
    double stack[kFrameSamples];
    std::vector<double> heap;
    double* re = stack;
    if (m > kFrameSamples / 2) {
        heap.resize(2 * m);
        re = heap.data();
    }
    double* im = re + m;
    // Pre-rotation, each point stored at its bit-reversed place, which is where the FFT's input permutation would
    // move it.
    for (size_t k = 0; k < m; ++k) {
        const double a = input[2 * k], b = input[n - 1 - 2 * k];
        const double c = rotations->pre_re[k], s = rotations->pre_im[k];
        const size_t to = rotations->reversed[k];
        re[to] = a * c - b * s;
        im[to] = a * s + b * c;
    }
    rotations->butterflies(re, im, m);
    for (size_t k = 0; k < m; ++k) {
        const double c = rotations->post_re[k], s = rotations->post_im[k];
        const double wr = re[k] * c - im[k] * s;
        const double wi = re[k] * s + im[k] * c;
        // DCT-IV: c[2k] = Re, c[n-1-2k] = -Im; output j is c[n-1-j].
        out[n - 1 - 2 * k] = static_cast<float>(wr) * scale;
        out[2 * k] = -static_cast<float>(wi) * scale;
    }
}

std::vector<float> sine_window(size_t n) {
    std::vector<float> w(n);
    for (size_t i = 0; i < n; ++i)
        w[i] = static_cast<float>(
            std::sin((static_cast<double>(i) + 0.5) * std::numbers::pi / (2.0 * static_cast<double>(n))));
    return w;
}

void window_overlap(std::span<float> block, std::span<const float> window) {
    const size_t len = block.size() / 2;
    if (window.size() < 2 * len) return;
    for (size_t k = 0; k < len; ++k) {
        const size_t i = k, j = 2 * len - 1 - k;
        const float s0 = block[i], s1 = block[j];
        const float wi = window[i], wj = window[j];
        block[i] = s0 * wj - s1 * wi;
        block[j] = s0 * wi + s1 * wj;
    }
}

// The frame decoder.

Decoder::Decoder(size_t channels, uint32_t sample_rate) {
    channels_ = std::clamp<size_t>(channels, 1, 2);
    const size_t rate = sample_rate > 44100 ? 48000 : sample_rate > 32000 ? 44100 : sample_rate > 24000 ? 32000 : 24000;
    for (size_t i = 0; i < kBlockSizes; ++i) {
        const size_t subframe_len = kFrameSamples >> i;
        auto& offsets = sfb_offsets_[i];
        size_t band = 1;
        for (size_t f = 0; f < kMaxBands - 1; ++f) {
            if (offsets[band - 1] >= subframe_len) break;
            const size_t offset = ((subframe_len * 2 * tables::kCriticalFreq[f]) / rate + 2) & ~size_t{3};
            if (offset > offsets[band - 1]) {
                offsets[band] = offset;
                ++band;
            }
            if (offset >= subframe_len) break;
        }
        offsets[band - 1] = subframe_len;
        num_sfb_[i] = band - 1;
    }
    for (size_t i = 0; i < kBlockSizes; ++i) {
        for (size_t b = 0; b < num_sfb_[i]; ++b) {
            const size_t offset = ((sfb_offsets_[i][b] + sfb_offsets_[i][b + 1] - 1) << i) >> 1;
            for (size_t x = 0; x < kBlockSizes; ++x) {
                size_t v = 0;
                while (v + 1 < kMaxBands && (sfb_offsets_[x][v + 1] << x) < offset) ++v;
                sf_offsets_[i][x][b] = v;
            }
        }
    }
}

void Decoder::reset() { ch_ = {}; }

const char* Decoder::decode(std::span<const uint8_t> data, size_t bits, std::vector<int16_t>& out) {
    // Past the data there are only zeros to read. A count beyond it would never let a stereo tile header overread,
    // and its loop takes no subframe on zero bits, so it would spin for good.
    const size_t data_bits = data.size() > SIZE_MAX / 8 ? SIZE_MAX : data.size() * 8;
    BitReader b{data, 0, std::min(bits, data_bits)};
    const size_t len = b.read(kLengthBits);
    const char* result = frame(b, len);
    if (result) {
        // Whatever the frame held is lost; keep the stream's timing.
        for (size_t c = 0; c < channels_; ++c) ch_[c].out.fill(0.0f);
    }
    out.reserve(out.size() + kFrameSamples * channels_);
    for (size_t i = 0; i < kFrameSamples; ++i)
        for (size_t c = 0; c < channels_; ++c) out.push_back(to_i16(ch_[c].out[i]));
    for (size_t c = 0; c < channels_; ++c)
        std::copy(ch_[c].out.begin() + kFrameSamples, ch_[c].out.end(), ch_[c].out.begin());
    return result;
}

void Decoder::flush(std::vector<int16_t>& out) {
    for (size_t i = 0; i < kFrameSamples / 2; ++i)
        for (size_t c = 0; c < channels_; ++c) out.push_back(to_i16(ch_[c].out[i]));
    reset();
}

const char* Decoder::frame(BitReader& b, size_t len) {
    if (const char* e = tile_header(b)) return e;
    // Post-processing transform.
    if (channels_ > 1 && b.bit() && b.bit()) b.read(static_cast<uint32_t>(channels_ * channels_ * 4));
    // Dynamic range compression gain, unused.
    b.read(8);
    if (b.bit()) {
        // Trim at start and end, which only WMA Pro files apply.
        if (b.bit()) b.read(log2_floor(kFrameSamples * 2));
        if (b.bit()) b.read(log2_floor(kFrameSamples * 2));
    }
    parsed_all_subframes_ = false;
    for (size_t c = 0; c < channels_; ++c) {
        ch_[c].decoded_samples = 0;
        ch_[c].cur_subframe = 0;
        ch_[c].reuse_sf = false;
    }
    while (!parsed_all_subframes_)
        if (const char* e = subframe(b)) return e;
    // FFmpeg flags `len != consumed + 2` but keeps the samples; so do we.
    if (b.overread() || b.pos > len) return "frame overread";
    return nullptr;
}

const char* Decoder::subframe_length(BitReader& b, size_t offset, size_t& len) {
    if (offset == kFrameSamples - kMinSubframe) {
        len = kMinSubframe;
        return nullptr;
    }
    // Four subframe sizes: a 1-bit "not maximal", then 1 bit of shift.
    const uint32_t shift = b.bit() ? 1 + b.read(1) : 0;
    len = kFrameSamples >> shift;
    if (len < kMinSubframe) return "subframe length";
    return nullptr;
}

const char* Decoder::tile_header(BitReader& b) {
    size_t num_samples[2] = {0, 0};
    bool contains[2] = {false, false};
    size_t channels_for_cur_subframe = channels_;
    size_t min_channel_len = 0;
    for (size_t c = 0; c < channels_; ++c) ch_[c].num_subframes = 0;
    const bool fixed = b.bit();
    for (;;) {
        for (size_t c = 0; c < channels_; ++c)
            contains[c] = num_samples[c] == min_channel_len &&
                          (fixed || channels_for_cur_subframe == 1 ||
                           min_channel_len == kFrameSamples - kMinSubframe || b.bit());
        size_t subframe_len = 0;
        if (const char* e = subframe_length(b, min_channel_len, subframe_len)) return e;
        min_channel_len += subframe_len;
        for (size_t c = 0; c < channels_; ++c) {
            Channel& ch = ch_[c];
            if (contains[c]) {
                if (ch.num_subframes >= kMaxSubframes) return "too many subframes";
                ch.subframe_len[ch.num_subframes] = subframe_len;
                num_samples[c] += subframe_len;
                ++ch.num_subframes;
                if (num_samples[c] > kFrameSamples) return "channel longer than a frame";
            } else if (num_samples[c] <= min_channel_len) {
                if (num_samples[c] < min_channel_len) {
                    channels_for_cur_subframe = 0;
                    min_channel_len = num_samples[c];
                }
                ++channels_for_cur_subframe;
            }
        }
        if (min_channel_len >= kFrameSamples || b.overread()) break;
    }
    if (b.overread()) return "tile header";
    return nullptr;
}

const char* Decoder::channel_transform(BitReader& b) {
    transform_ = false;
    if (channels_ == 1) return nullptr;
    if (b.bit()) return "channel transform bit";
    // One group holding every channel of the subframe.
    if (channels_for_cur_subframe_ == 2) {
        if (b.bit()) {
            if (b.bit()) return "channel transform type";
        } else {
            transform_ = true;
        }
    }
    if (transform_) {
        if (!b.bit()) {
            for (size_t i = 0; i < num_bands_; ++i) transform_band_[i] = b.bit();
        } else {
            transform_band_.fill(true);
        }
    }
    return nullptr;
}

const char* Decoder::coefficients(BitReader& b, size_t c) {
    const Codes& vlc = codes();
    const size_t table = b.bit() ? 1 : 0;
    const uint16_t* run = table == 1 ? tables::kCoef1Run : tables::kCoef0Run;
    const float* level = table == 1 ? tables::kCoef1Level : tables::kCoef0Level;
    const size_t subframe_len = subframe_len_;
    Channel& ch = ch_[c];
    bool rl_mode = false;
    size_t cur = 0;
    size_t num_zeros = 0;
    while ((transmit_num_vec_coeffs_ || !rl_mode) && cur + 3 < ch.num_vec_coeffs) {
        uint32_t vals[4] = {0, 0, 0, 0};
        const int32_t idx = vlc.vec4.read(b);
        if (idx < 0) {
            for (size_t i = 0; i < 4; i += 2) {
                const int32_t idx2 = vlc.vec2.read(b);
                if (idx2 < 0) {
                    for (size_t k = i; k < i + 2; ++k) {
                        uint32_t x = static_cast<uint32_t>(vlc.vec1.read(b));
                        if (x == std::size(tables::kVec1) - 1) x += large_value(b);
                        vals[k] = x;
                    }
                } else {
                    vals[i] = static_cast<uint32_t>(idx2 >> 4);
                    vals[i + 1] = static_cast<uint32_t>(idx2 & 0xf);
                }
            }
        } else {
            vals[0] = static_cast<uint32_t>(idx >> 12);
            vals[1] = static_cast<uint32_t>((idx >> 8) & 0xf);
            vals[2] = static_cast<uint32_t>((idx >> 4) & 0xf);
            vals[3] = static_cast<uint32_t>(idx & 0xf);
        }
        for (const uint32_t v : vals) {
            if (v != 0) {
                const bool negative = !b.bit();
                ch.coeffs[cur] = negative ? -static_cast<float>(v) : static_cast<float>(v);
                num_zeros = 0;
            } else {
                ch.coeffs[cur] = 0.0f;
                ++num_zeros;
                rl_mode |= num_zeros > (subframe_len >> 8);
            }
            ++cur;
        }
        if (b.overread()) return "vector coefficients";
    }
    if (cur < subframe_len) {
        std::fill(ch.coeffs.begin() + static_cast<ptrdiff_t>(cur),
                  ch.coeffs.begin() + static_cast<ptrdiff_t>(subframe_len), 0.0f);
        // ff_wma_run_level_decode, version 1.
        const size_t mask = subframe_len - 1;
        size_t offset = cur;
        while (offset < subframe_len) {
            const int32_t code = vlc.coef[table].read(b);
            if (code > 1) {
                offset += run[code];
                const bool negative = !b.bit();
                const float v = level[code];
                ch.coeffs[offset & mask] = negative ? -v : v;
            } else if (code == 1) {
                break;
            } else {
                const int32_t value = static_cast<int32_t>(large_value(b));
                if (b.bit()) {
                    if (b.bit()) {
                        // A broken escape ends the channel's coefficients, as in FFmpeg.
                        if (b.bit()) break;
                        offset += b.read(esc_len_) + 4;
                    } else {
                        offset += b.read(2) + 1;
                    }
                }
                const bool negative = !b.bit();
                ch.coeffs[offset & mask] = static_cast<float>(negative ? -value : value);
            }
            if (b.overread()) return "run level coefficients";
            ++offset;
        }
    }
    return nullptr;
}

const char* Decoder::scale_factors(BitReader& b) {
    const Codes& vlc = codes();
    for (size_t i = 0; i < channels_for_cur_subframe_; ++i) {
        const size_t c = channel_indexes_[i];
        const auto sf_offsets = sf_offsets_[table_idx_][ch_[c].table_idx];
        Channel& ch = ch_[c];
        const size_t target = 1 - ch.scale_factor_idx;
        ch.scale_factors = target;
        if (ch.reuse_sf) {
            const auto from = ch.saved_scale_factors[ch.scale_factor_idx];
            for (size_t band = 0; band < num_bands_; ++band)
                ch.saved_scale_factors[target][band] = from[sf_offsets[band]];
        }
        if (ch.cur_subframe == 0 || b.bit()) {
            if (!ch.reuse_sf) {
                ch.scale_factor_step = static_cast<int32_t>(b.read(2)) + 1;
                int32_t val = 45 / ch.scale_factor_step;
                for (size_t band = 0; band < num_bands_; ++band) {
                    val += vlc.scale.read(b);
                    ch.saved_scale_factors[target][band] = val;
                }
            } else {
                size_t band = 0;
                while (band < num_bands_) {
                    const int32_t idx = vlc.scale_rl.read(b);
                    size_t skip = 0;
                    int32_t val = 0;
                    bool negative = false;
                    if (idx == 0) {
                        const uint32_t code = b.read(14);
                        skip = (code & 0x3f) >> 1;
                        val = static_cast<int32_t>(code >> 6);
                        negative = (code & 1) == 0;
                    } else if (idx == 1) {
                        break;
                    } else {
                        skip = tables::kScaleRlRun[idx];
                        val = tables::kScaleRlLevel[idx];
                        negative = !b.bit();
                    }
                    band += skip;
                    if (band >= num_bands_) return "scale factor run";
                    ch.saved_scale_factors[target][band] += negative ? -val : val;
                    ++band;
                }
            }
            ch.scale_factor_idx = target;
            ch.table_idx = table_idx_;
            ch.reuse_sf = true;
        }
        const auto& sf = ch.saved_scale_factors[ch.scale_factors];
        int32_t max = 0;
        for (size_t band = 0; band < num_bands_; ++band) max = band == 0 ? sf[band] : std::max(max, sf[band]);
        ch.max_scale_factor = max;
    }
    return nullptr;
}

const char* Decoder::subframe(BitReader& b) {
    size_t offset = kFrameSamples;
    size_t subframe_len = kFrameSamples;
    int64_t total_samples = static_cast<int64_t>(kFrameSamples * channels_);
    for (size_t c = 0; c < channels_; ++c) {
        const Channel& ch = ch_[c];
        if (offset > ch.decoded_samples) {
            offset = ch.decoded_samples;
            subframe_len = ch.subframe_len[std::min(ch.cur_subframe, kMaxSubframes - 1)];
        }
    }
    channels_for_cur_subframe_ = 0;
    for (size_t c = 0; c < channels_; ++c) {
        Channel& ch = ch_[c];
        total_samples -= static_cast<int64_t>(ch.decoded_samples);
        if (offset == ch.decoded_samples && ch.cur_subframe < ch.num_subframes &&
            subframe_len == ch.subframe_len[ch.cur_subframe]) {
            total_samples -= static_cast<int64_t>(subframe_len);
            ch.decoded_samples += subframe_len;
            channel_indexes_[channels_for_cur_subframe_] = c;
            ++channels_for_cur_subframe_;
        }
    }
    // A tile header that parsed gives every channel subframes of 128 to 512 samples; a length outside that is a
    // channel without one, which no subframe can take.
    if (channels_for_cur_subframe_ == 0 || subframe_len < kMinSubframe) return "no channel in subframe";
    if (total_samples == 0) parsed_all_subframes_ = true;
    table_idx_ = log2_floor(kFrameSamples / subframe_len);
    num_bands_ = num_sfb_[table_idx_];
    subframe_len_ = subframe_len;
    esc_len_ = log2_floor(subframe_len - 1) + 1;

    // Extended header: fill bits.
    if (b.bit()) {
        size_t fill = b.read(2);
        if (fill == 0) {
            const uint32_t len = b.read(4);
            fill = static_cast<size_t>(b.read(len)) + 1;
        }
        b.pos += fill;
    }
    if (b.bit()) return "reserved bit";
    if (const char* e = channel_transform(b)) return e;

    bool transmit_coeffs = false;
    for (size_t i = 0; i < channels_for_cur_subframe_; ++i) {
        const size_t c = channel_indexes_[i];
        ch_[c].transmit_coefs = b.bit();
        transmit_coeffs |= ch_[c].transmit_coefs;
    }
    if (transmit_coeffs) {
        transmit_num_vec_coeffs_ = b.bit();
        if (transmit_num_vec_coeffs_) {
            const uint32_t num_bits = log2_floor((subframe_len + 3) / 4) + 1;
            for (size_t i = 0; i < channels_for_cur_subframe_; ++i) {
                const size_t c = channel_indexes_[i];
                const size_t n = static_cast<size_t>(b.read(num_bits)) << 2;
                if (n > subframe_len) return "vector coefficient count";
                ch_[c].num_vec_coeffs = n;
            }
        } else {
            for (size_t i = 0; i < channels_for_cur_subframe_; ++i)
                ch_[channel_indexes_[i]].num_vec_coeffs = subframe_len;
        }
        // Quantisation step (16-bit samples: 90 * 16 >> 4).
        int32_t quant_step = 90;
        int32_t step = b.sign_extend(6);
        quant_step += step;
        if (step == -32 || step == 31) {
            const bool negative = step == -32;
            int32_t quant = 0;
            while (b.pos + 5 < b.end) {
                step = static_cast<int32_t>(b.read(5));
                if (step != 31) break;
                quant += 31;
            }
            quant_step += negative ? -(quant + step) : quant + step;
        }
        if (channels_for_cur_subframe_ == 1) {
            ch_[channel_indexes_[0]].quant_step = quant_step;
        } else {
            const uint32_t modifier_len = b.read(3);
            for (size_t i = 0; i < channels_for_cur_subframe_; ++i) {
                const size_t c = channel_indexes_[i];
                ch_[c].quant_step = quant_step;
                if (b.bit())
                    ch_[c].quant_step += modifier_len > 0 ? static_cast<int32_t>(b.read(modifier_len)) + 1 : 1;
            }
        }
        if (const char* e = scale_factors(b)) return e;
    }

    for (size_t i = 0; i < channels_for_cur_subframe_; ++i) {
        const size_t c = channel_indexes_[i];
        if (ch_[c].transmit_coefs && b.pos < b.end) {
            if (const char* e = coefficients(b, c)) return e;
        } else {
            std::fill(ch_[c].coeffs.begin(), ch_[c].coeffs.begin() + static_cast<ptrdiff_t>(subframe_len), 0.0f);
        }
    }

    // Where this subframe's samples go in the channel output.
    const size_t at = offset + kFrameSamples / 2;
    if (transmit_coeffs) {
        inverse_channel_transform();
        // Output in [-1, 1] for 16-bit streams.
        const float scale = 2.0f / static_cast<float>(subframe_len) / 32768.0f;
        const auto& sfb = sfb_offsets_[table_idx_];
        float tmp[kFrameSamples] = {};
        for (size_t i = 0; i < channels_for_cur_subframe_; ++i) {
            Channel& ch = ch_[channel_indexes_[i]];
            const auto& sf = ch.saved_scale_factors[ch.scale_factors];
            for (size_t band = 0; band < num_bands_; ++band) {
                const size_t end = std::min(sfb[band + 1], subframe_len);
                const int32_t exp = ch.quant_step - (ch.max_scale_factor - sf[band]) * ch.scale_factor_step;
                const float quant = std::pow(10.0f, static_cast<float>(exp) / 20.0f);
                for (size_t k = sfb[band]; k < end; ++k) tmp[k] = ch.coeffs[k] * quant;
            }
            imdct_half({tmp, subframe_len}, {ch.out.data() + at, subframe_len}, scale);
        }
    } else {
        for (size_t i = 0; i < channels_for_cur_subframe_; ++i) {
            Channel& ch = ch_[channel_indexes_[i]];
            std::fill(ch.out.begin() + static_cast<ptrdiff_t>(at),
                      ch.out.begin() + static_cast<ptrdiff_t>(at + subframe_len), 0.0f);
        }
    }

    // Window and overlap-add with the previous block.
    for (size_t i = 0; i < channels_for_cur_subframe_; ++i) {
        Channel& ch = ch_[channel_indexes_[i]];
        size_t winlen = ch.prev_block_len;
        size_t start = at - winlen / 2;
        if (subframe_len < winlen) {
            start += (winlen - subframe_len) / 2;
            winlen = subframe_len;
        }
        window_overlap({ch.out.data() + start, winlen}, window_for(winlen));
        ch.prev_block_len = subframe_len;
    }

    for (size_t i = 0; i < channels_for_cur_subframe_; ++i) {
        Channel& ch = ch_[channel_indexes_[i]];
        if (ch.cur_subframe >= ch.num_subframes) return "subframe overrun";
        ++ch.cur_subframe;
    }
    if (b.overread()) return "subframe overread";
    return nullptr;
}

void Decoder::inverse_channel_transform() {
    if (!transform_ || channels_for_cur_subframe_ != 2) return;
    const auto& sfb = sfb_offsets_[table_idx_];
    Channel& a = ch_[0];
    Channel& b = ch_[1];
    for (size_t band = 0; band < num_bands_; ++band) {
        const size_t end = std::min(sfb[band + 1], subframe_len_);
        if (transform_band_[band]) {
            for (size_t y = sfb[band]; y < end; ++y) {
                const float l = a.coeffs[y], r = b.coeffs[y];
                a.coeffs[y] = l - r;
                b.coeffs[y] = l + r;
            }
        } else {
            for (size_t y = sfb[band]; y < end; ++y) {
                a.coeffs[y] *= 181.0f / 128.0f;
                b.coeffs[y] *= 181.0f / 128.0f;
            }
        }
    }
}

}  // namespace rt::xma
