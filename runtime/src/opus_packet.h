// Opus packet framing (RFC 6716, section 3): how many samples a packet decodes to, and whether its framing is valid.
//
// Leonida has no Opus decoder yet. VIctory's xapu.dll (crates/victory-hle/src/modules/apu.rs, GPL-3.0-or-later, by
// Marvisak and contributors, DexNom fork) decodes with the opus-pure crate (BSD-3-Clause), which is not in this
// repository; the xapu shim ported from it uses this parser instead, so a packet gives the sample count and the
// errors a decoder would report, and silence for its samples. The checks are libopus's opus_packet_parse for a packet
// that is not self-delimited (the requirements R1-R7 of RFC 6716 section 3.4); the sample count is what opus_decode
// returns for such a packet at 48 kHz. Written for Leonida from the RFC; GPL-3.0-or-later. Pure C++: no OS headers.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rt::opus {

inline constexpr int kBufferTooSmall = -2;  // OPUS_BUFFER_TOO_SMALL
inline constexpr int kInvalidPacket = -4;   // OPUS_INVALID_PACKET
inline constexpr int kMaxFrameBytes = 1275;
inline constexpr int kMaxPacketSamples = 5760;  // 120 ms at 48 kHz

// Samples per channel at 48 kHz in each frame of a packet with this TOC byte (configuration, bits 7-3; section 3.1).
constexpr int samples_per_frame(uint8_t toc) {
    if (toc & 0x80) return (48000 << ((toc >> 3) & 3)) / 400;  // CELT-only: 2.5, 5, 10, 20 ms
    if ((toc & 0x60) == 0x60) return (toc & 0x08) ? 960 : 480;  // hybrid: 10, 20 ms
    const int size = (toc >> 3) & 3;                             // SILK-only: 10, 20, 40, 60 ms
    return size == 3 ? 2880 : (48000 << size) / 100;
}

namespace detail {

// A frame length (section 3.2.1): one byte below 252, else two. Returns the bytes used, -1 (and size -1) without them.
inline int parse_size(const uint8_t* data, ptrdiff_t len, int* size) {
    if (len < 1) {
        *size = -1;
        return -1;
    }
    if (data[0] < 252) {
        *size = data[0];
        return 1;
    }
    if (len < 2) {
        *size = -1;
        return -1;
    }
    *size = 4 * data[1] + data[0];
    return 2;
}

}  // namespace detail

// The number of frames in the packet (frame count codes 0-3, section 3.2), or kInvalidPacket for one RFC 6716 does
// not allow.
inline int frame_count(const uint8_t* data, size_t length) {
    if (!data || length == 0) return kInvalidPacket;
    const uint8_t toc = data[0];
    const int framesize = samples_per_frame(toc);
    const uint8_t* p = data + 1;
    ptrdiff_t len = static_cast<ptrdiff_t>(length) - 1;
    ptrdiff_t last_size = len;
    int count = 0;
    switch (toc & 3) {
        case 0:  // one frame
            count = 1;
            break;
        case 1:  // two frames of the same size
            count = 2;
            if (len & 1) return kInvalidPacket;
            last_size = len / 2;
            break;
        case 2: {  // two frames, the first's size given
            count = 2;
            int size = 0;
            const int bytes = detail::parse_size(p, len, &size);
            len -= bytes;
            if (size < 0 || size > len) return kInvalidPacket;
            p += bytes;
            last_size = len - size;
            break;
        }
        default: {  // a frame count byte: 1 to 48 frames, CBR or VBR, optional padding
            if (len < 1) return kInvalidPacket;
            const uint8_t ch = *p++;
            --len;
            count = ch & 0x3f;
            if (count <= 0 || framesize * count > kMaxPacketSamples) return kInvalidPacket;
            if (ch & 0x40) {
                int pad = 0;
                do {
                    if (len <= 0) return kInvalidPacket;
                    pad = *p++;
                    --len;
                    len -= pad == 255 ? 254 : pad;
                } while (pad == 255);
            }
            if (len < 0) return kInvalidPacket;
            if (ch & 0x80) {  // VBR: every frame's size but the last
                last_size = len;
                for (int i = 0; i < count - 1; ++i) {
                    int size = 0;
                    const int bytes = detail::parse_size(p, len, &size);
                    len -= bytes;
                    if (size < 0 || size > len) return kInvalidPacket;
                    p += bytes;
                    last_size -= bytes + size;
                }
                if (last_size < 0) return kInvalidPacket;
            } else {  // CBR
                last_size = len / count;
                if (last_size * count != len) return kInvalidPacket;
            }
            break;
        }
    }
    // The last frame's size (all of them, CBR) is not coded and may exceed what a frame can hold.
    if (last_size > kMaxFrameBytes) return kInvalidPacket;
    return count;
}

// Samples per channel at 48 kHz that decoding the packet gives, as opus_decode returns it: its frames times the
// samples per frame, kInvalidPacket for a malformed packet, kBufferTooSmall when that is more than `max_samples`.
inline int packet_samples(const uint8_t* data, size_t length, int max_samples = kMaxPacketSamples) {
    const int count = frame_count(data, length);
    if (count < 0) return count;
    const int samples = count * samples_per_frame(data[0]);
    return samples > max_samples ? kBufferTooSmall : samples;
}

}  // namespace rt::opus
