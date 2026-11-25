// The XMA2 frame decoder (src/xma_decode.h) and the SHAPE XMA context machine (src/xma_context.h). Most cases are
// ported with the code from VIctory's tests (crates/victory-formats/src/xma.rs and
// crates/victory-hle/src/modules/audio.rs), GPL-3.0-or-later, by Marvisak and contributors (DexNom fork); Leonida is
// GPL-3.0-or-later. Every stream is built in memory: no game data, no files.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <numbers>
#include <optional>
#include <thread>
#include <vector>

#include "xma_context.h"
#include "xma_decode.h"
#include "xma_tables.h"

using namespace rt::xma;

namespace {

int g_checks = 0;
int g_failures = 0;

template <class... Args>
void check(bool ok, const char* format, Args... args) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::fputs("FAIL ", stdout);
    if constexpr (sizeof...(Args) == 0) std::fputs(format, stdout);
    else std::printf(format, args...);
    std::fputs("\n", stdout);
}

void run(const char* name, void (*test)()) {
    const int before = g_failures;
    test();
    std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name);
}

constexpr double kPi = std::numbers::pi;

// Writes `value` as `n` bits, MSB first, at bit `pos`, over what is there.
void set_bits(std::vector<uint8_t>& data, size_t pos, uint32_t n, uint32_t value) {
    for (uint32_t i = 0; i < n; ++i) {
        const size_t p = pos + i;
        const unsigned bit = (value >> (n - 1 - i)) & 1u;
        data[p / 8] = static_cast<uint8_t>((data[p / 8] & ~(0x80u >> (p % 8))) | (bit << (7 - p % 8)));
    }
}

// Appends bits MSB first.
struct BitWriter {
    std::vector<uint8_t> data;
    size_t bits = 0;
    void put(uint32_t value, uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) {
            if (bits % 8 == 0) data.push_back(0);
            if ((value >> (n - 1 - i)) & 1u) data[bits / 8] |= static_cast<uint8_t>(0x80u >> (bits % 8));
            ++bits;
        }
    }
};

// ---- The inverse transform ----

// The direct sum imdct_half stands for (FFmpeg's naive inverse MDCT: the standard formula, negated).
std::vector<double> imdct_reference(const std::vector<float>& input) {
    const size_t n = input.size();
    std::vector<double> out(n);
    for (size_t j = 0; j < n; ++j) {
        const double t = static_cast<double>(j + n / 2) + 0.5 + static_cast<double>(n) / 2.0;
        double sum = 0;
        for (size_t k = 0; k < n; ++k)
            sum += input[k] * std::cos(kPi / static_cast<double>(n) * t * (static_cast<double>(k) + 0.5));
        out[j] = -sum;
    }
    return out;
}

void fast_imdct_matches_the_direct_sum() {
    for (const size_t n : {size_t{128}, size_t{256}, size_t{512}, size_t{64}}) {
        std::vector<float> input(n);
        for (size_t i = 0; i < n; ++i) input[i] = static_cast<float>((i * 7919) % 97) - 48.0f;
        std::vector<float> fast(n);
        imdct_half(input, fast, 1.0f);
        const auto reference = imdct_reference(input);
        for (size_t j = 0; j < n; ++j)
            check(std::fabs(fast[j] - reference[j]) < 1e-2 * (1.0 + std::fabs(reference[j])), "n %zu, %zu: %f vs %f", n,
                  j, fast[j], reference[j]);
    }
}

// Windowed MDCT blocks of a signal, put back together the way the decoder does (half IMDCT, then window and overlap),
// give the signal.
void imdct_and_window_reconstruct_a_signal() {
    const size_t n = 256;
    std::vector<float> signal(n * 6);
    for (size_t i = 0; i < signal.size(); ++i) signal[i] = std::sin(static_cast<float>(i) * 0.05f) * 0.3f;
    // The 2n-sample sine window; its rising half is sine_window(n).
    const auto window = [n](size_t i) { return std::sin((static_cast<double>(i) + 0.5) * kPi / (2.0 * n)); };
    std::vector<float> out(n * 7);
    const auto rising = sine_window(n);
    for (size_t block = 0; block < 5; ++block) {
        const size_t base = block * n;
        std::vector<float> coeffs(n);
        for (size_t k = 0; k < n; ++k) {
            double sum = 0;
            for (size_t i = 0; i < 2 * n; ++i) {
                const double t = static_cast<double>(i) + 0.5 + static_cast<double>(n) / 2.0;
                // FFmpeg's forward MDCT pairs with its inverse by sign.
                sum += -static_cast<double>(signal[base + i]) * window(i) *
                       std::cos(kPi / static_cast<double>(n) * t * (static_cast<double>(k) + 0.5));
            }
            coeffs[k] = static_cast<float>(sum);
        }
        // The half output is the middle of the 2n window.
        const size_t at = base + n / 2;
        imdct_half(coeffs, std::span<float>(out.data() + at, n), 2.0f / static_cast<float>(n));
        if (block > 0) window_overlap(std::span<float>(out.data() + at - n / 2, n), rising);
    }
    for (size_t i = n; i < 5 * n; ++i)
        check(std::fabs(out[i] - signal[i]) < 1e-3, "%zu: %f vs %f", i, out[i], signal[i]);
}

// ---- Packets and frames ----

void packet_rejects_out_of_range_indices_and_partial_headers() {
    const std::vector<uint8_t> four(4), three(3);
    check(!packet(four, SIZE_MAX / kPacketBytes + 1), "an index past size_t");
    check(!packet(three, 0), "a partial header");
    check(packet(four, 0).has_value(), "a whole header");
}

void packet_headers_and_frame_walk() {
    // Two packets: frame A (100 bits) and B (16300 bits, crossing into packet 1 by 48 bits), then C (60 bits) in
    // packet 1; the rest is padding.
    std::vector<uint8_t> data(2 * kPacketBytes, 0xff);
    const auto header = [](uint32_t frames, uint32_t first) { return frames << 26 | first << 11; };
    set_bits(data, 0, 32, header(2, 0));
    set_bits(data, kPacketBits, 32, header(1, 48));
    set_bits(data, 32, 15, 100);
    set_bits(data, 132, 15, 16300);
    const size_t c_start = kPacketBits + 32 + 48;
    set_bits(data, c_start, 15, 60);
    const auto a = read_frame(data, 32);
    check(a && a->bits == 100 && a->next == size_t{132}, "frame A and the next");
    const auto b = read_frame(data, 132);
    check(b && b->bits == 16300 && b->next == c_start, "frame B and the next");
    // Its last bits come from after packet 1's header; the last byte's unused bits stay zero.
    check(b && b->data.size() == (16300 + 7) / 8 && b->data.back() == 0xf0, "frame B's bytes");
    const auto c = read_frame(data, c_start);
    // C was packet 1's only frame: the next would be in packet 2.
    check(c && c->bits == 60 && !c->next, "frame C is the last");
}

// A damaged packet whose first frame says it is 0 bits long: finding the frame after a later one must give up, not
// walk in place forever (found by random-input testing).
void a_zero_length_frame_before_the_start_ends_the_search() {
    std::vector<uint8_t> data(kPacketBytes, 0);
    set_bits(data, 0, 32, 2u << 26);  // two frames, the first at payload bit 0; its 15-bit length stays 0
    const size_t start = kHeaderBits + 100;
    set_bits(data, start, kLengthBits, 200);
    const auto frame = read_frame(data, start);
    check(frame && frame->bits == 200 && !frame->next, "the frame at start reads; the walk gives up");
}

// A frame that crosses a packet where no frame begins (a full continuation) and ends in another such packet, followed
// by padding: the next frame is the first one of the next packet that has frames, not the padding the frame ended in.
void the_frame_after_continuation_packets_starts_in_the_next_packet_with_frames() {
    std::vector<uint8_t> data(4 * kPacketBytes, 0);
    const auto header = [](uint32_t frames, uint32_t first) { return frames << 26 | first << 11; };
    set_bits(data, 0, 32, header(2, 0));
    // Packet 1 is all of one frame; packet 2 holds only its last 64 bits.
    set_bits(data, kPacketBits, 32, header(0, 0x7fff));
    set_bits(data, 2 * kPacketBits, 32, header(0, 64));
    set_bits(data, 3 * kPacketBits, 32, header(1, 0));
    // A first frame of 16000 bits, then one from the end of its payload through packets 1 and 2.
    const size_t second = kHeaderBits + 16000;
    set_bits(data, kHeaderBits, kLengthBits, 16000);
    const size_t tail = kPacketBits - second;
    const size_t bits = tail + (kPacketBits - kHeaderBits) + 64;
    set_bits(data, second, kLengthBits, static_cast<uint32_t>(bits));
    set_bits(data, 3 * kPacketBits + kHeaderBits, kLengthBits, 100);
    const auto first = read_frame(data, kHeaderBits);
    check(first && first->next == second, "the first frame's next");
    const auto frame = read_frame(data, second);
    check(frame && frame->bits == bits && frame->next == 3 * kPacketBits + kHeaderBits, "after the continuation");
    const auto last = read_frame(data, 3 * kPacketBits + kHeaderBits);
    check(last && !last->next, "the last frame");
}

// Two packets of three silent mono frames each: a fixed one-subframe layout, no transmitted coefficients
// (VIctory audio.rs, silent_stream).
std::vector<uint8_t> silent_stream() {
    std::vector<uint8_t> data(2 * kPacketBytes, 0xff);
    for (size_t p = 0; p < 2; ++p) {
        const size_t base = p * kPacketBits;
        set_bits(data, base, 32, 3u << 26);
        for (size_t frame = 0; frame < 3; ++frame) {
            const size_t at = base + 32 + frame * 31;
            set_bits(data, at, 15, 31);              // length
            set_bits(data, at + 15, 2, 0b10);        // fixed layout, one 512-sample subframe
            set_bits(data, at + 17, 8 + 1 + 3 + 2, 0);  // gain, no trim, empty subframe
        }
    }
    return data;
}

void silent_frames_decode_to_silence() {
    const auto data = silent_stream();
    Decoder decoder(1, 48000);
    std::vector<int16_t> pcm;
    size_t frames = 0;
    std::optional<size_t> at = kHeaderBits + packet(data, 0)->first_frame_bit;
    while (at && frames < 16) {
        const auto frame = read_frame(data, *at);
        if (!frame) break;
        const char* error = decoder.decode(frame->data, frame->bits, pcm);
        check(error == nullptr, "silent frame %zu: %s", frames, error ? error : "");
        ++frames;
        at = frame->next;
    }
    decoder.flush(pcm);
    check(frames == 6, "six frames walked (%zu)", frames);
    check(pcm.size() == 6 * kFrameSamples + kFrameSamples / 2, "a frame of samples each, half a frame flushed");
    check(std::all_of(pcm.begin(), pcm.end(), [](int16_t s) { return s == 0; }), "all silence");

    // A frame of zeros after its length and a fixed layout decodes, mono and stereo (no coefficients, an empty
    // channel transform for stereo).
    for (const size_t channels : {size_t{1}, size_t{2}}) {
        BitWriter w;
        w.put(100, 15);
        w.put(1, 1);
        w.put(0, 100 - 16);
        Decoder d(channels, 44100);
        std::vector<int16_t> out;
        const char* error = d.decode(w.data, w.bits, out);
        check(error == nullptr && out.size() == kFrameSamples * channels, "a zero frame, %zu channels: %s", channels,
              error ? error : "");
    }
}

// The canonical code of `symbol` in a table of (symbol, length) entries in code order, as ff_vlc_init_from_lengths
// assigns them.
struct Code {
    uint32_t bits = 0;
    uint32_t length = 0;
};

template <class Symbol, class Length>
Code canonical_code(size_t count, Symbol symbol_at, Length length_at, int32_t symbol) {
    uint64_t code = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t length = length_at(i);
        if (symbol_at(i) == symbol) return {static_cast<uint32_t>(code >> (32 - length)), length};
        code += uint64_t{1} << (32 - length);
    }
    return {};
}

// The scale factor bands of a 512-sample subframe at 48 kHz, worked out again here.
size_t bands_512_at_48k() {
    size_t offsets[Decoder::kMaxBands + 1] = {};
    size_t band = 1;
    for (size_t f = 0; f < Decoder::kMaxBands - 1; ++f) {
        if (offsets[band - 1] >= 512) break;
        const size_t offset = ((512 * 2 * rt::xma::tables::kCriticalFreq[f]) / 48000 + 2) & ~size_t{3};
        if (offset > offsets[band - 1]) offsets[band++] = offset;
        if (offset >= 512) break;
    }
    return band - 1;
}

// A mono frame with one coefficient, +1 at frequency 0, every scale factor equal and quantisation step 90: the
// decoder's samples are that coefficient through the inverse MDCT and the sine window, worked out here from the
// direct sum. Then a frame without coefficients gives the windowed overlap of the first.
void a_coded_frame_decodes_to_its_transform() {
    namespace t = rt::xma::tables;
    const Code scale_zero = canonical_code(
        std::size(t::kScale), [](size_t i) { return static_cast<int32_t>(t::kScale[i].symbol) - 60; },
        [](size_t i) { return static_cast<uint32_t>(t::kScale[i].length); }, 0);
    const auto coef0_symbol = [](size_t i) { return static_cast<int32_t>(t::kCoef0Syms[i]); };
    const auto coef0_length = [](size_t i) { return static_cast<uint32_t>(t::kCoef0Lens[i]); };
    const Code level_one = canonical_code(std::size(t::kCoef0Syms), coef0_symbol, coef0_length, 2);  // run 0, level 1
    const Code end = canonical_code(std::size(t::kCoef0Syms), coef0_symbol, coef0_length, 1);
    check(scale_zero.length > 0 && level_one.length > 0 && end.length > 0, "codes found");
    check(t::kCoef0Run[2] == 0 && t::kCoef0Level[2] == 1.0f, "symbol 2 is run 0, level 1");

    const auto build = [&](uint32_t length) {
        BitWriter w;
        w.put(length, 15);
        w.put(1, 1);    // fixed layout
        w.put(0, 1);    // one subframe of 512
        w.put(0, 8);    // gain
        w.put(0, 1);    // no trim
        w.put(0, 1);    // no fill
        w.put(0, 1);    // reserved
        w.put(1, 1);    // transmit coefficients
        w.put(1, 1);    // transmit the vector coefficient count
        w.put(0, 8);    // of 0: run-level coding from the first coefficient
        w.put(0, 6);    // quantisation step 90 + 0
        w.put(0, 2);    // scale factor step 1
        for (size_t band = 0; band < bands_512_at_48k(); ++band) w.put(scale_zero.bits, scale_zero.length);
        w.put(0, 1);    // coefficient table 0
        w.put(level_one.bits, level_one.length);
        w.put(1, 1);    // positive
        w.put(end.bits, end.length);
        w.put(0, 1);    // no more frames
        return w;
    };
    const BitWriter first = build(static_cast<uint32_t>(build(0).bits));
    check(first.bits < 0x7fff, "the frame fits a length field");

    Decoder decoder(1, 48000);
    std::vector<int16_t> pcm;
    const char* error = decoder.decode(first.data, first.bits, pcm);
    check(error == nullptr, "the coded frame decodes: %s", error ? error : "");

    // The reference: y = the scaled inverse transform of the one coefficient; the frame's output is the window's
    // overlap of silence with y's first half.
    const size_t n = 512;
    const float quant = std::pow(10.0f, 90.0f / 20.0f);
    const double scale = 2.0 / 512.0 / 32768.0;
    std::vector<double> y(n);
    for (size_t j = 0; j < n; ++j)
        y[j] = -scale * quant * std::cos(kPi / static_cast<double>(n) * (static_cast<double>(j) + n + 0.5) * 0.5);
    const auto w = [n](size_t i) { return std::sin((static_cast<double>(i) + 0.5) * kPi / (2.0 * n)); };
    const auto i16 = [](double v) { return std::clamp(std::round(v * 32768.0), -32768.0, 32767.0); };
    std::vector<double> expected(n);
    for (size_t k = 0; k < n / 2; ++k) {
        expected[k] = i16(-y[n / 2 - 1 - k] * w(k));
        expected[n - 1 - k] = i16(y[n / 2 - 1 - k] * w(n - 1 - k));
    }
    int peak = 0;
    size_t off = 0;
    for (size_t i = 0; i < n && i < pcm.size(); ++i) {
        peak = std::max(peak, std::abs(static_cast<int>(pcm[i])));
        off += std::fabs(pcm[i] - expected[i]) > 1.0 ? 1 : 0;
    }
    check(pcm.size() == n && off == 0 && peak > 50, "the first frame is the windowed transform (%zu off, peak %d)",
          off, peak);

    // A frame without coefficients: its output is the first frame's second half, windowed out.
    BitWriter second;
    second.put(100, 15);
    second.put(1, 1);
    second.put(0, 100 - 16);
    pcm.clear();
    error = decoder.decode(second.data, second.bits, pcm);
    off = 0;
    for (size_t i = 0; i < n / 2 && pcm.size() == n; ++i) {
        off += std::fabs(pcm[i] - i16(y[n / 2 + i] * w(n - 1 - i))) > 1.0 ? 1 : 0;
        off += std::fabs(pcm[n - 1 - i] - i16(y[n / 2 + i] * w(i))) > 1.0 ? 1 : 0;
    }
    check(error == nullptr && pcm.size() == n && off == 0, "the next frame overlaps the first (%zu off)", off);
}

// ---- Noise (VIctory's noise tests) ----

// Deterministic noise: xorshift64.
struct Noise {
    uint64_t state;

    uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }

    // A number below `n`.
    size_t below(size_t n) { return static_cast<size_t>(next() % n); }

    // `len` bytes of even noise, or of mostly zeros (`density` 1) or mostly ones (2), which take other paths through a
    // frame.
    std::vector<uint8_t> bytes(size_t len, int density) {
        std::vector<uint8_t> out(len);
        for (auto& byte : out) {
            const uint64_t r = next();
            const uint8_t a = static_cast<uint8_t>(r), b = static_cast<uint8_t>(r >> 8), c = static_cast<uint8_t>(r >> 16);
            byte = density == 0 ? a : density == 1 ? static_cast<uint8_t>(a & b & c) : static_cast<uint8_t>(a | b | c);
        }
        return out;
    }
};

constexpr int kDensities[4] = {0, 0, 1, 2};

// `packets` packets of one stream: back-to-back frames of noise after their length fields, 300 to 4299 bits long (so
// every packet but the last has a frame start, and at most 55 do), then padding of ones. The headers count the frames
// that begin in each packet. Also gives the number of frames.
std::vector<uint8_t> noise_stream(Noise& noise, size_t packets, int density, size_t& frames) {
    const size_t payload = kPacketBits - kHeaderBits;
    std::vector<uint8_t> data = noise.bytes(packets * kPacketBytes, density);
    const auto at = [payload](size_t q) { return q / payload * kPacketBits + kHeaderBits + q % payload; };
    std::vector<std::pair<uint32_t, uint32_t>> headers(packets, {0, 0});
    size_t q = 0;
    frames = 0;
    for (;;) {
        const size_t len = 300 + noise.below(4000);
        if (q + len > packets * payload) break;
        auto& header = headers[q / payload];
        if (header.first == 0) header.second = static_cast<uint32_t>(q % payload);
        ++header.first;
        for (uint32_t i = 0; i < kLengthBits; ++i)
            set_bits(data, at(q + i), 1, static_cast<uint32_t>(len >> (kLengthBits - 1 - i)) & 1u);
        q += len;
        ++frames;
    }
    for (size_t padding = q; padding < packets * payload; ++padding) set_bits(data, at(padding), 1, 1);
    for (size_t p = 0; p < packets; ++p)
        set_bits(data, p * kPacketBits, 32, headers[p].first << 26 | headers[p].second << 11);
    return data;
}

// Packets of noise, streams of frames of noise, and such streams with headers and bits damaged: walking the frames
// from the first packet and decoding each, mono and stereo, gives a frame of samples per frame and ends within the
// frames the buffer can hold (each is over 15 bits and the walk only moves on).
void noise_packets_walk_and_decode() {
    Noise noise{0x2545f4914f6cdd1dull};
    for (size_t round = 0; round < 60; ++round) {
        const size_t packets = 1 + round % 4;
        const int density = kDensities[round / 3 % 4];
        size_t written = 0;
        std::vector<uint8_t> data = round % 3 == 0 ? noise.bytes(packets * kPacketBytes, density)
                                                   : noise_stream(noise, packets, density, written);
        if (round % 3 == 2) {
            const size_t headers = 1 + noise.below(3);
            for (size_t i = 0; i < headers; ++i) {
                const size_t p = noise.below(packets);
                set_bits(data, p * kPacketBits, 32, static_cast<uint32_t>(noise.next()));
            }
            const size_t flips = noise.below(16);
            for (size_t i = 0; i < flips; ++i) {
                const size_t bit = noise.below(data.size() * 8);
                data[bit / 8] ^= static_cast<uint8_t>(0x80u >> (bit % 8));
            }
        }
        const size_t budget = data.size() * 8 / 16;
        for (const size_t channels : {size_t{1}, size_t{2}}) {
            Decoder decoder(channels, 48000);
            const size_t per_frame = kFrameSamples * channels;
            std::vector<int16_t> pcm;
            size_t frames = 0;
            std::optional<size_t> at;
            if (const auto first = packet(data, 0)) at = kHeaderBits + first->first_frame_bit;
            while (at) {
                const auto frame = read_frame(data, *at);
                if (!frame) break;
                ++frames;
                const bool moves_on = !frame->next || *frame->next > frame->start;
                check(frames <= budget && moves_on, "round %zu: the walk moves on within its budget", round);
                if (frames > budget || !moves_on) break;
                decoder.decode(frame->data, frame->bits, pcm);
                check(pcm.size() == frames * per_frame, "round %zu: a frame of samples per frame", round);
                at = frame->next;
            }
            // A clean stream: every frame is walked.
            if (round % 3 == 1) check(frames == written, "round %zu: %zu of %zu frames walked", round, frames, written);
            decoder.flush(pcm);
            check(pcm.size() == frames * per_frame + per_frame / 2, "round %zu: the flush", round);
        }
    }
}

// Frames of noise, mono and stereo at each sample rate, with bit counts up to the end of the data: every decode gives
// one frame of samples, whether the frame decodes or not.
void noise_frames_decode_to_a_frame_of_samples() {
    Noise noise{0x9e3779b97f4a7c15ull};
    for (const size_t channels : {size_t{1}, size_t{2}}) {
        for (const uint32_t rate : {24000u, 32000u, 44100u, 48000u}) {
            Decoder decoder(channels, rate);
            std::vector<int16_t> pcm;
            for (size_t round = 0; round < 48; ++round) {
                const auto data = noise.bytes(round * 89 % 4096, kDensities[round % 4]);
                const size_t bits = round % 2 == 0 ? data.size() * 8 : noise.below(data.size() * 8 + 1);
                pcm.clear();
                decoder.decode(data, bits, pcm);
                check(pcm.size() == kFrameSamples * channels, "%zu channels, %u Hz, round %zu", channels, rate, round);
                if (round % 16 == 15) decoder.reset();
            }
            pcm.clear();
            decoder.flush(pcm);
            check(pcm.size() == kFrameSamples / 2 * channels, "%zu channels, %u Hz: the flush", channels, rate);
        }
    }
}

// Bit counts past the end of a frame's data, up to SIZE_MAX: the decoding ends, with a frame of samples. A stereo tile
// header that ran out of data used to loop on the zeros past it for good (found by this test in VIctory).
void noise_frames_with_bit_counts_past_the_data() {
    Noise noise{0xd1b54a32d192ed03ull};
    constexpr size_t kLengths[8] = {0, 1, 2, 3, 5, 8, 40, 300};
    for (const size_t channels : {size_t{1}, size_t{2}}) {
        Decoder decoder(channels, 48000);
        std::vector<int16_t> pcm;
        for (size_t round = 0; round < 48; ++round) {
            const auto data = noise.bytes(kLengths[round % 8], kDensities[round / 8 % 4]);
            size_t bits = SIZE_MAX;
            if (round % 3 == 0) bits = data.size() * 8 + 1 + noise.below(size_t{1} << 16);
            else if (round % 3 == 1) bits = SIZE_MAX / 2 + noise.below(size_t{1} << 20);
            pcm.clear();
            decoder.decode(data, bits, pcm);
            check(pcm.size() == kFrameSamples * channels, "%zu channels, round %zu", channels, round);
        }
    }
}

// read_frame from any bit of a buffer of noise and past it, and packet at any index: a frame read stays inside its
// limits.
void noise_offsets_read_a_frame_or_nothing() {
    Noise noise{0x6a09e667f3bcc908ull};
    for (size_t round = 0; round < 24; ++round) {
        const size_t packets = 1 + round % 3;
        const auto data = noise.bytes(packets * kPacketBytes, kDensities[round % 4]);
        const size_t all = data.size() * 8;
        std::vector<size_t> starts = {0,           1,         kHeaderBits - 1,   kHeaderBits,
                                      kPacketBits - kLengthBits, kPacketBits - 1, kPacketBits, all - 1,
                                      all,         all + 1,   SIZE_MAX / 2,      SIZE_MAX - kPacketBits + 1,
                                      SIZE_MAX};
        for (int i = 0; i < 32; ++i) starts.push_back(noise.below(all));
        for (const size_t start : starts) {
            const auto frame = read_frame(data, start);
            if (!frame) continue;
            check(frame->bits > kLengthBits && frame->bits < 0x7fff, "start %zu: the length", start);
            check(frame->data.size() == (frame->bits + 7) / 8, "start %zu: the bytes", start);
            if (frame->next) check(*frame->next > start && *frame->next / 8 < data.size(), "start %zu: the next", start);
        }
        const size_t far = SIZE_MAX / kPacketBytes;
        for (const size_t index : {size_t{0}, packets - 1, packets, far, SIZE_MAX})
            check(packet(data, index).has_value() == (index < packets), "round %zu: packet %zu", round, index);
    }
}

// ---- The XMA contexts (VIctory audio.rs) ----

// APU memory for the context tests: blocks with device addresses.
struct Block {
    uint32_t apu;
    std::vector<uint8_t> bytes;
};
std::vector<Block*> g_blocks;

uint8_t* test_memory(uint32_t apu, size_t len) {
    for (Block* b : g_blocks) {
        if (apu < b->apu) continue;
        const size_t offset = apu - b->apu;
        if (offset <= b->bytes.size() && len <= b->bytes.size() - offset) return b->bytes.data() + offset;
    }
    return nullptr;
}

struct Fixture {
    Block input{0x10000, {}};
    Block output{0x20000, {}};
    uint32_t array[4 * 16] = {};
    Engine engine{&test_memory};

    explicit Fixture(size_t output_bytes) {
        input.bytes = silent_stream();
        input.bytes.resize(0x1000, 0);
        output.bytes.assign(output_bytes, 0x55);
        g_blocks = {&input, &output};
        engine.attach(array, 4);
    }
    ~Fixture() {
        engine.detach();
        g_blocks.clear();
    }
    Context context(uint32_t index) { return Context(array + index * 16); }
    void submit(uint32_t command, std::initializer_list<uint32_t> words) {
        const std::vector<uint32_t> data(words);
        check(engine.command(command, data.data()), "command %u is an XMA command", command);
    }
    // A pass on a thread of its own, so one that never ends fails the test instead of hanging it.
    bool pump_within(std::chrono::seconds limit, Context context) {
        std::atomic<bool> done{false};
        std::thread pass([&] {
            engine.pump();
            done = true;
        });
        const auto until = std::chrono::steady_clock::now() + limit;
        while (!done && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool ended = done;
        if (!ended) context.set_field(1, 31, 1, 0);  // end it as a full ring would
        pass.join();
        return ended;
    }
};

void xma_contexts_decode_into_their_output_rings() {
    Fixture f(0x400);
    const Context context = f.context(1);
    // As the title sets one up: two packets, input 0 valid, a ring of four blocks, writable, 48 kHz mono, reading from
    // the first frame.
    context.set(0, 2 | 1u << 20 | 4u << 22);
    context.set(1, 1u << 31 | 3u << 27);
    context.set(2, 32);
    context.set(5, f.input.apu);
    context.set(7, f.output.apu);
    f.submit(kEnableXma, {1});
    f.engine.pump();
    // Full: all four blocks written (silence), the write block wrapped onto the read block, and the ring no longer
    // writable.
    check(context.field(0, 27, 5) == 0 && context.field(1, 31, 1) == 0, "a full ring");
    check(std::all_of(f.output.bytes.begin(), f.output.bytes.end(), [](uint8_t b) { return b == 0; }), "silence");

    // Read four blocks at a time until the input has run out and the ring keeps room.
    for (int i = 0; i < 20; ++i) {
        f.submit(kAdvanceXmaRead, {1, 4});
        f.engine.pump();
        if (context.field(2, 31, 1) == 1 && context.field(1, 31, 1) == 1) break;
    }
    // Six frames and the flushed tail: 3,328 samples, 26 blocks.
    const auto trace = f.engine.trace(1);
    check(trace && trace->produced == 26 * kSamplesPerBlock && trace->input_ended, "26 blocks produced (%zu samples)",
          trace ? trace->produced : 0);
    check(trace && trace->frames == 6 && trace->bad_frames == 0, "six good frames");
    check(context.field(0, 20, 2) == 0, "input used up");
    check(context.field(2, 26, 5) == kInputInvalid, "input invalid status");
    check(context.field(0, 27, 5) == 26 % 4, "the write block after 26 blocks");

    // A count near UINT32_MAX wraps the read block like any other (3 + 0xffffffff blocks on a ring of four is block
    // 2); the sum used to overflow.
    context.set_field(9, 0, 5, 3);
    f.submit(kAdvanceXmaRead, {1, UINT32_MAX});
    check(context.field(9, 0, 5) == 2, "the read block wraps");

    f.submit(kDisableXma, {1});
    check(!f.engine.trace(1), "disabled");
}

// A read block past the end of the ring (6 on a ring of four) is block 2 of it: a stream that loops forever fills
// blocks 0 and 1 and the pass ends. It used to fill the ring for good, holding the voice.
void xma_read_blocks_past_the_ring_end_wrap() {
    Fixture f(0x400);
    const Context context = f.context(1);
    context.set(0, 2 | 255u << 12 | 1u << 20 | 4u << 22);
    context.set(1, 1u << 31 | 3u << 27);
    context.set(2, 32);
    context.set(3, 32);
    context.set(4, static_cast<uint32_t>(kPacketBits + 32 + 2 * 31));
    context.set(5, f.input.apu);
    context.set(7, f.output.apu);
    context.set(9, 6);
    f.submit(kEnableXma, {1});
    const bool ended = f.pump_within(std::chrono::seconds(5), context);
    check(ended, "the XMA pass ended");
    check(context.field(0, 27, 5) == 2 && context.field(1, 31, 1) == 0, "blocks 0 and 1 written, the ring full");
    check(std::all_of(f.output.bytes.begin(), f.output.bytes.begin() + 0x200, [](uint8_t b) { return b == 0; }) &&
              std::all_of(f.output.bytes.begin() + 0x200, f.output.bytes.end(), [](uint8_t b) { return b == 0x55; }),
          "blocks 2 and 3 left for the title");
}

// A write block past the end of the ring (6 on a ring of four) is block 2 of it: blocks 2, 3 and 0 are filled, up to
// the read block (1). It used to write block 6 first, past the ring's end.
void xma_write_blocks_past_the_ring_end_wrap() {
    Fixture f(0x800);  // the ring (four blocks) and a guard area after it, which the pass must not touch
    const Context context = f.context(1);
    context.set(0, 2 | 255u << 12 | 1u << 20 | 4u << 22 | 6u << 27);
    context.set(1, 1u << 31 | 3u << 27);
    context.set(2, 32);
    context.set(3, 32);
    context.set(4, static_cast<uint32_t>(kPacketBits + 32 + 2 * 31));
    context.set(5, f.input.apu);
    context.set(7, f.output.apu);
    context.set(9, 1);
    f.submit(kEnableXma, {1});
    const bool ended = f.pump_within(std::chrono::seconds(5), context);
    check(ended, "the XMA pass ended");
    check(context.field(0, 27, 5) == 1 && context.field(1, 31, 1) == 0, "up to the read block");
    const auto& ring = f.output.bytes;
    const auto all = [&ring](size_t from, size_t to, uint8_t v) {
        return std::all_of(ring.begin() + static_cast<ptrdiff_t>(from), ring.begin() + static_cast<ptrdiff_t>(to),
                           [v](uint8_t b) { return b == v; });
    };
    check(all(0x400, 0x800, 0x55), "nothing written past the ring");
    check(all(0, 0x100, 0) && all(0x200, 0x400, 0) && all(0x100, 0x200, 0x55), "blocks 2, 3 and 0 written, 1 left");
}

// A loop over the first two frames, which ends after the second frame's second subframe and starts two subframes into
// the first. Each end of the loop is cut in its own frame.
void a_loop_skips_subframes_of_its_first_frame() {
    Block input{0x10000, silent_stream()};
    g_blocks = {&input};
    // Frames start at bits 32, 63 and 94 of each packet.
    const uint32_t first = 32, second = 63, third = 94;
    const uint32_t end_subframe = 1, skip_subframes = 2;
    uint32_t words[16] = {};
    const Context context(words);
    // Two packets, one loop, input 0 valid; 48 kHz mono.
    context.set(0, 2 | 1u << 12 | 1u << 20 | 4u << 22);
    context.set(1, 1u << 31 | 3u << 27 | end_subframe << 12 | skip_subframes << 17);
    context.set(2, first);
    context.set(3, first);
    context.set(4, second);
    context.set(5, input.apu);
    Voice voice;
    const auto step = [&](uint32_t expect_next) {
        const size_t before = voice.pcm.size();
        const bool decoded = decode(context, voice, &test_memory);
        check(decoded && context.field(2, 0, 26) == expect_next, "the read position after a frame: %u, not %u",
              context.field(2, 0, 26), expect_next);
        return voice.pcm.size() - before;
    };
    const size_t subframe = kSubframeSamples;
    // Before the loop: the first frame whole.
    check(step(second) == 4 * subframe, "the first frame whole");
    // The loop's last frame: its first two subframes, then back.
    check(step(first) == 2 * subframe, "the loop's last frame");
    check(context.field(0, 12, 8) == 0, "one loop, taken");
    // The loop's first frame again: without its first two subframes.
    check(step(second) == 2 * subframe, "the loop's first frame");
    // No loops left: both frames whole, and on to the third.
    check(step(third) == 4 * subframe, "the second frame whole");
    g_blocks.clear();
}

void xma_commands_select_contexts() {
    Fixture f(0x400);
    for (uint32_t i = 0; i < 4; ++i) f.context(i).set(2, 0xfc000000u);  // error status and error set
    f.submit(kEnableXmas, {0b101});
    check(f.engine.trace(0) && !f.engine.trace(1) && f.engine.trace(2) && !f.engine.trace(3), "contexts 0 and 2");
    check(f.context(0).field(2, 26, 6) == 0 && f.context(1).field(2, 26, 6) == 0x3f, "enabling clears the error");
    f.submit(kDisableXmas, {0b100});
    check(f.engine.trace(0) && !f.engine.trace(2), "context 2 disabled");
    f.submit(kEnableXma, {4});  // past the array: nothing
    f.submit(kAdvanceXmaRead, {9, 1});
    check(!f.engine.command(12, nullptr), "another command is not the decoder's");
    check(f.engine.command(kEnableXma, nullptr), "without data: no context");
    f.engine.detach();
    f.submit(kEnableXma, {0});
    check(!f.engine.trace(0), "detached: no voices");
}

}  // namespace

int main() {
    std::printf("xma_test: XMA2 frames and SHAPE XMA contexts, built in memory\n");
    run("fast_imdct_matches_the_direct_sum", fast_imdct_matches_the_direct_sum);
    run("imdct_and_window_reconstruct_a_signal", imdct_and_window_reconstruct_a_signal);
    run("packet_rejects_out_of_range_indices_and_partial_headers",
        packet_rejects_out_of_range_indices_and_partial_headers);
    run("packet_headers_and_frame_walk", packet_headers_and_frame_walk);
    run("a_zero_length_frame_before_the_start_ends_the_search", a_zero_length_frame_before_the_start_ends_the_search);
    run("the_frame_after_continuation_packets_starts_in_the_next_packet_with_frames",
        the_frame_after_continuation_packets_starts_in_the_next_packet_with_frames);
    run("silent_frames_decode_to_silence", silent_frames_decode_to_silence);
    run("a_coded_frame_decodes_to_its_transform", a_coded_frame_decodes_to_its_transform);
    run("noise_packets_walk_and_decode", noise_packets_walk_and_decode);
    run("noise_frames_decode_to_a_frame_of_samples", noise_frames_decode_to_a_frame_of_samples);
    run("noise_frames_with_bit_counts_past_the_data", noise_frames_with_bit_counts_past_the_data);
    run("noise_offsets_read_a_frame_or_nothing", noise_offsets_read_a_frame_or_nothing);
    run("xma_contexts_decode_into_their_output_rings", xma_contexts_decode_into_their_output_rings);
    run("xma_read_blocks_past_the_ring_end_wrap", xma_read_blocks_past_the_ring_end_wrap);
    run("xma_write_blocks_past_the_ring_end_wrap", xma_write_blocks_past_the_ring_end_wrap);
    run("a_loop_skips_subframes_of_its_first_frame", a_loop_skips_subframes_of_its_first_frame);
    run("xma_commands_select_contexts", xma_commands_select_contexts);
    std::printf("xma_test complete: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
