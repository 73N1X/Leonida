// The audio coprocessor's XMA decoder: enabled SHAPE XMA contexts are decoded into their output rings, like the
// hardware does.
//
// Ported from VIctory (crates/victory-hle/src/modules/audio.rs, the XMA part), GPL-3.0-or-later, by Marvisak and
// contributors (DexNom fork). Leonida is GPL-3.0-or-later. The DexNom snapshot in victory/ is the base; three fixes
// from VIctory's own history in this repository that the snapshot lacks are kept: the write block (7a5d85e6a) and the
// read block (1054fe87d) are taken modulo the ring, so neither writes past the ring's end nor fills it for good, and
// a loop's start skip applies to the loop's first frame, not its last (985d8d4b0). Pure C++: no OS headers; the
// acphal shim owns the decoding thread and the APU memory lookup.
//
// An XMA context (16 words in the shared context array) names up to two input buffers of 2 KiB packets, a ring of
// 256-byte output blocks the hardware fills with 16-bit PCM, and the read offset the title advances by command. Words
// and bit fields, as titles write them:
//   0: input 0 size in packets (0-11), loop count (12-19, 255 forever), valid inputs (20-21), output size in blocks
//      (22-26), write block (27-31, hardware);
//   1: input 1 size (0-11), loop end subframe (12-13), loop start subframes to skip (17-19), subframes to skip at the
//      start (24-26), sample rate (27-28: 24, 32, 44.1, 48 kHz), stereo (29), output writable (31);
//   2: read position in bits (0-25), error status (26-30), error (31);
//   3: loop start in bits (0-25); 4: loop end in bits (0-25), current input (31); 5, 6: input addresses; 7: output
//      address; 8: overlap buffer; 9: read block (0-4).
// Titles read back only the write block, "writable" and the read block, and take whatever lies between read and
// write. Each enabled context decodes from a cold state and the overlap buffer is ignored (titles copy it between
// consecutive contexts of one voice), as in VIctory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <vector>

#include "xma_decode.h"

namespace rt::xma {

// Coprocessor commands that drive XMA contexts (IAcpHal::SubmitCommand ids, VIctory audio.rs).
inline constexpr uint32_t kEnableXma = 5;       // word 0: the context index
inline constexpr uint32_t kEnableXmas = 6;      // words: a bit per context
inline constexpr uint32_t kDisableXma = 7;      // word 0: the context index
inline constexpr uint32_t kDisableXmas = 8;     // words: a bit per context
inline constexpr uint32_t kAdvanceXmaRead = 18; // word 0: the context index, word 1: blocks the title took

inline constexpr size_t kContextBytes = 64;
inline constexpr size_t kBlockBytes = 256;
inline constexpr size_t kSamplesPerBlock = kBlockBytes / 2;
// Error status (word 2, bits 26-30): the input ran out.
inline constexpr uint32_t kInputInvalid = 4;

// `len` host bytes of APU memory at a device address when one live heap block holds them all, else null.
using ApuBytes = uint8_t* (*)(uint32_t apu, size_t len);

// A context's words in title memory. Read and written as volatile: the title reads and writes them concurrently, as
// it does the hardware's.
class Context {
public:
    explicit Context(uint32_t* words) : words_(words) {}

    uint32_t get(size_t word) const { return static_cast<volatile const uint32_t*>(words_)[word]; }
    void set(size_t word, uint32_t value) const { static_cast<volatile uint32_t*>(words_)[word] = value; }
    uint32_t field(size_t word, uint32_t shift, uint32_t bits) const {
        return (get(word) >> shift) & ((uint32_t{1} << bits) - 1);
    }
    void set_field(size_t word, uint32_t shift, uint32_t bits, uint32_t value) const {
        const uint32_t mask = ((uint32_t{1} << bits) - 1) << shift;
        set(word, (get(word) & ~mask) | ((value << shift) & mask));
    }
    size_t channels() const { return 1 + field(1, 29, 1); }

private:
    uint32_t* words_;
};

// What a voice did.
struct Trace {
    size_t produced = 0;  // samples per channel written to the ring
    size_t taken = 0;     // blocks the title took (command 18), how often, and how often it found the ring not full
    uint32_t takes = 0;
    uint32_t partial_takes = 0;
    uint32_t frames = 0;
    uint32_t bad_frames = 0;
    int16_t peak = 0;     // the loudest sample written
    bool input_ended = false;
};

// An enabled context's decoding state.
struct Voice {
    std::optional<Decoder> decoder;
    std::deque<int16_t> pcm;
    size_t skip = 0;  // samples per channel still to drop (start or loop subframe skips)
    bool ended = false;
    Trace trace;
    std::vector<int16_t> frame;  // one frame's samples, reused
};

// Decodes the next frame of the context's current input into voice.pcm; false when there is nothing left to decode.
bool decode(Context context, Voice& voice, ApuBytes memory);

// Fills the context's output ring as far as it has room.
void pump(Context context, Voice& voice, ApuBytes memory);

// The voices of the XMA context array: one per context, set while the context is enabled. Each has its own lock, so a
// command for one voice never waits for others to decode (the title sends them from its time-critical mixer thread).
// The array is attached when the context arrays are allocated and detached when they are released.
class Engine {
public:
    // `wake` (optional) is called when a voice was enabled or read from, to have the decoding thread fill rings now.
    explicit Engine(ApuBytes memory, void (*wake)() = nullptr) : memory_(memory), wake_(wake) {}

    void attach(uint32_t* array, uint32_t count);
    void detach();

    // Runs an XMA command whose data words start at `data` (null: none); false for a command this does not know.
    bool command(uint32_t command, const void* data);

    // Fills every enabled context's output as far as it has room.
    void pump();

    // The trace of an enabled voice; none when the context is not enabled.
    std::optional<Trace> trace(uint32_t index);

private:
    struct Slot {
        std::mutex mutex;
        std::optional<Voice> voice;
    };

    std::optional<Context> context(uint32_t index) const;
    void enable(uint32_t index);
    void disable(uint32_t index);
    void advance_read(uint32_t index, uint32_t blocks);

    ApuBytes memory_;
    void (*wake_)();
    std::shared_mutex lock_;  // exclusive only to attach and detach
    uint32_t* array_ = nullptr;
    uint32_t count_ = 0;
    std::unique_ptr<Slot[]> slots_;
};

}  // namespace rt::xma
