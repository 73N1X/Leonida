// XMA2, the console's hardware audio codec: mono or stereo WMA Pro frames of 512 samples per channel, packed into
// 2048-byte packets.
//
// Ported from VIctory (crates/victory-formats/src/xma.rs), GPL-3.0-or-later, by Marvisak and contributors (DexNom
// fork). Leonida is GPL-3.0-or-later. The DexNom snapshot in victory/ is the base; four fixes from VIctory's own
// history in this repository that the snapshot lacks are kept (ba42db643: the frame after packets a frame spans
// whole; d4d182204: a frame's bit count is clamped to its data; 03a6f2290 and 4eb0898b3: payload bytes gathered a byte
// at a time and the inverse transform's rotations built once per size, which give the same bits).
//
// A packet starts with a 32-bit header (MSB first): the number of frames that begin in it (6 bits), the number of
// bits that finish the previous frame before the first new one (15), metadata (3) and how many packets of other
// streams follow before this stream's next packet (8). Every frame starts with its length in bits (15) and ends with a
// "more frames" bit.
//
// The frame decoder follows FFmpeg's wmaprodec.c (LGPL-2.1-or-later, used under GPL-3.0-or-later) restricted to what
// XMA needs: at most two channels, four subframe sizes (512 down to 128), no LFE channel. Pure C++: no OS headers.
// wmaprodec.c's own notices, kept as the LGPL asks: Copyright (c) 2007 Baptiste Coudurier, Benjamin Larsson, Ulion;
// Copyright (c) 2008 - 2011 Sascha Sommer, Benjamin Larsson (the same lines are in third_party/xma/NOTICE.md).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rt::xma {

inline constexpr size_t kPacketBytes = 2048;
inline constexpr size_t kPacketBits = kPacketBytes * 8;
inline constexpr size_t kHeaderBits = 32;
// Samples per channel in one frame.
inline constexpr size_t kFrameSamples = 512;
// Samples per channel in one subframe, the unit of skips and loop ends.
inline constexpr size_t kSubframeSamples = 128;
inline constexpr uint32_t kLengthBits = 15;

// A packet's header fields.
struct Packet {
    uint32_t frames = 0;
    uint32_t first_frame_bit = 0;
    uint32_t skip = 0;
};

// The header of packet `index` of `data`; none when the packet's header is not all inside `data`.
std::optional<Packet> packet(std::span<const uint8_t> data, size_t index);

// One frame's bits gathered out of its packets.
struct Frame {
    std::vector<uint8_t> data;  // the frame from its length field on, MSB first; unused bits of the last byte are 0
    size_t bits = 0;
    size_t start = 0;            // bit offset in the buffer where it starts
    std::optional<size_t> next;  // where the next frame starts; none when the buffer has no further frame
};

// Reads the frame starting at bit `start` (a frame start inside a packet's payload) of `data`, a buffer of whole
// packets of one stream.
std::optional<Frame> read_frame(std::span<const uint8_t> data, size_t start);

namespace detail {
struct BitReader;  // xma_decode.cpp
}

// Decodes the frames of one mono or stereo stream.
class Decoder {
public:
    // The stream format: the channel count (clamped to 1..2) and the sample rate, which sets the scale factor band
    // layout.
    Decoder(size_t channels, uint32_t sample_rate);

    size_t channels() const { return channels_; }

    // Forgets the overlap with earlier frames (a new stream or a seek).
    void reset();

    // Decodes one frame (Frame::data, Frame::bits) and appends kFrameSamples interleaved samples per channel to
    // `out`, whether it decodes or not. Null on success, else what was wrong; a frame that does not decode leaves
    // silence, so the stream keeps its timing.
    const char* decode(std::span<const uint8_t> data, size_t bits, std::vector<int16_t>& out);

    // The overlap left after the last frame, then silence: half a frame of samples per channel.
    void flush(std::vector<int16_t>& out);

    static constexpr size_t kMaxBands = 29;
    static constexpr size_t kMaxSubframes = 32;
    static constexpr size_t kBlockSizes = 3;

private:
    using Bits = detail::BitReader;

    struct Channel {
        size_t prev_block_len = kFrameSamples;
        bool transmit_coefs = false;
        size_t num_subframes = 0;
        std::array<size_t, kMaxSubframes> subframe_len{};
        size_t cur_subframe = 0;
        size_t decoded_samples = 0;
        int32_t quant_step = 0;
        bool reuse_sf = false;
        int32_t scale_factor_step = 1;
        int32_t max_scale_factor = 0;
        std::array<std::array<int32_t, kMaxBands>, 2> saved_scale_factors{};
        size_t scale_factor_idx = 0;
        size_t scale_factors = 0;  // which of the two saved sets is in use for this subframe
        size_t table_idx = 0;
        size_t num_vec_coeffs = 0;
        std::array<float, kFrameSamples> coeffs{};
        // The frame's output, then half a frame of overlap for the next one.
        std::array<float, kFrameSamples + kFrameSamples / 2> out{};
    };

    const char* frame(Bits& b, size_t len);
    const char* subframe_length(Bits& b, size_t offset, size_t& len);
    const char* tile_header(Bits& b);
    const char* channel_transform(Bits& b);
    const char* coefficients(Bits& b, size_t c);
    const char* scale_factors(Bits& b);
    const char* subframe(Bits& b);
    void inverse_channel_transform();

    size_t channels_ = 1;
    std::array<size_t, kBlockSizes> num_sfb_{};
    std::array<std::array<size_t, kMaxBands + 1>, kBlockSizes> sfb_offsets_{};
    std::array<std::array<std::array<size_t, kMaxBands>, kBlockSizes>, kBlockSizes> sf_offsets_{};
    std::array<Channel, 2> ch_{};
    // Per subframe.
    size_t subframe_len_ = kFrameSamples;
    size_t channels_for_cur_subframe_ = 0;
    std::array<size_t, 2> channel_indexes_{0, 1};
    size_t num_bands_ = 0;
    size_t table_idx_ = 0;
    uint32_t esc_len_ = 0;
    bool transmit_num_vec_coeffs_ = false;
    bool transform_ = false;
    std::array<bool, kMaxBands> transform_band_{};
    bool parsed_all_subframes_ = false;
};

// Exposed for the tests.

// Half-length inverse MDCT as FFmpeg defines it (av_tx MDCT, inverse): `n` coefficients (input.size(), a power of
// two) to the middle `n` samples of the 2n-sample window, negated and scaled by `scale`. Computed as a DCT-IV through
// an n/2-point complex FFT.
void imdct_half(std::span<const float> input, std::span<float> out, float scale);

// ff_sine_windows: the rising half of a sine window of length `n`.
std::vector<float> sine_window(size_t n);

// FFmpeg's vector_fmul_window in place: the first half of `block` holds the previous block's tail, the second half the
// current block's head.
void window_overlap(std::span<float> block, std::span<const float> window);

}  // namespace rt::xma
