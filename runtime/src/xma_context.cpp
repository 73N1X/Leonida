// The SHAPE XMA context machine. Ported from VIctory (crates/victory-hle/src/modules/audio.rs), GPL-3.0-or-later, by
// Marvisak and contributors (DexNom fork). Leonida is GPL-3.0-or-later. See xma_context.h.
#include "xma_context.h"

#include <algorithm>
#include <cstring>

namespace rt::xma {

namespace {

constexpr uint32_t kRates[4] = {24000, 32000, 44100, 48000};

int16_t saturating_abs(int16_t v) { return v == INT16_MIN ? INT16_MAX : static_cast<int16_t>(v < 0 ? -v : v); }

}  // namespace

void pump(Context context, Voice& voice, ApuBytes memory) {
    const uint32_t size = context.field(0, 22, 5);
    if (size == 0) return;
    while (context.field(1, 31, 1) == 1) {
        if (voice.pcm.size() < kSamplesPerBlock && !decode(context, voice, memory)) return;
        // The write block is the title's too: taken modulo the ring before the first write, which would otherwise
        // land past the ring's end.
        uint32_t write = context.field(0, 27, 5) % size;
        const uint32_t address = static_cast<uint32_t>(context.get(7) + uint64_t{write} * kBlockBytes);
        uint8_t* out = memory(address, kBlockBytes);
        if (!out) return;
        for (size_t i = 0; i < kSamplesPerBlock; ++i) {
            int16_t sample = 0;
            if (!voice.pcm.empty()) {
                sample = voice.pcm.front();
                voice.pcm.pop_front();
            }
            voice.trace.peak = std::max(voice.trace.peak, saturating_abs(sample));
            const uint16_t bits = static_cast<uint16_t>(sample);
            out[2 * i] = static_cast<uint8_t>(bits & 0xff);
            out[2 * i + 1] = static_cast<uint8_t>(bits >> 8);
        }
        voice.trace.produced += kSamplesPerBlock / context.channels();
        write = (write + 1) % size;
        context.set_field(0, 27, 5, write);
        // The read block is the title's: taken modulo the ring like the write block and advance_read's. Compared as
        // it is, one at or past the end would never be met, and a stream that loops would keep this voice (and its
        // lock) filling forever.
        if (write == context.field(9, 0, 5) % size) {
            // Full: nothing more until the title advances its read block.
            context.set_field(1, 31, 1, 0);
        }
    }
}

bool decode(Context context, Voice& voice, ApuBytes memory) {
    if (voice.ended) return false;
    const size_t channels = context.channels();
    uint32_t current = context.field(4, 31, 1);
    const uint32_t valid = context.field(0, 20, 2);
    if ((valid & (1u << current)) == 0) {
        if ((valid & (1u << (1 - current))) == 0) {
            // Out of input: the overlap tail, then nothing.
            if (voice.decoder) {
                voice.frame.clear();
                voice.decoder->flush(voice.frame);
                voice.pcm.insert(voice.pcm.end(), voice.frame.begin(), voice.frame.end());
            }
            voice.ended = true;
            voice.trace.input_ended = true;
            context.set_field(2, 26, 5, kInputInvalid);
            context.set_field(2, 31, 1, 1);
            return !voice.pcm.empty();
        }
        current = 1 - current;
        context.set_field(4, 31, 1, current);
        context.set_field(2, 0, 26, 0);
    }
    const uint32_t address = current == 0 ? context.get(5) : context.get(6);
    const uint32_t packets = current == 0 ? context.field(0, 0, 12) : context.field(1, 0, 12);
    const size_t bytes = size_t{packets} * kPacketBytes;
    const uint8_t* data = memory(address & ~0x7ffu, bytes);
    if (!data) {
        context.set_field(0, 20 + current, 1, 0);
        return true;
    }
    const std::span<const uint8_t> input(data, bytes);
    size_t at = context.field(2, 0, 26);
    if (at < kHeaderBits) {
        const auto first = packet(input, 0);
        at = kHeaderBits + (first ? first->first_frame_bit : 0);
    }
    const auto frame = read_frame(input, at);
    if (!frame) {
        // This input is used up.
        context.set_field(0, 20 + current, 1, 0);
        return true;
    }
    if (!voice.decoder) voice.decoder.emplace(channels, kRates[context.field(1, 27, 2)]);
    voice.frame.clear();
    ++voice.trace.frames;
    if (voice.decoder->decode(frame->data, frame->bits, voice.frame)) ++voice.trace.bad_frames;
    size_t next = frame->next.value_or(0);
    const uint32_t loops = context.field(0, 12, 8);
    const size_t loop_end = context.field(4, 0, 26);
    const bool ends_loop = loop_end != 0 && (frame->start == loop_end ||
                                             (frame->start < loop_end && (!frame->next || loop_end < *frame->next)));
    const bool looping = loops > 0 && ends_loop;
    if (looping) {
        const size_t keep = (context.field(1, 12, 2) + size_t{1}) * kSubframeSamples * channels;
        if (voice.frame.size() > keep) voice.frame.resize(keep);
        next = context.field(3, 0, 26);
        if (loops != 255) context.set_field(0, 12, 8, loops - 1);
    }
    // What is still to skip is this frame's (the stream's first, or a loop's first when it is its last too).
    const size_t skip = std::min(voice.skip * channels, voice.frame.size());
    voice.skip -= skip / channels;
    voice.pcm.insert(voice.pcm.end(), voice.frame.begin() + static_cast<ptrdiff_t>(skip), voice.frame.end());
    if (looping) {
        // The loop's own skip is of its first frame, decoded next: not of this one, its last.
        voice.skip = context.field(1, 17, 3) * kSubframeSamples;
    }
    if (next == 0) context.set_field(0, 20 + current, 1, 0);
    context.set_field(2, 0, 26, static_cast<uint32_t>(next));
    return true;
}

void Engine::attach(uint32_t* array, uint32_t count) {
    std::unique_lock lock(lock_);
    array_ = array;
    count_ = array ? count : 0;
    slots_ = count_ ? std::make_unique<Slot[]>(count_) : nullptr;
}

void Engine::detach() {
    std::unique_lock lock(lock_);
    array_ = nullptr;
    count_ = 0;
    slots_.reset();
}

std::optional<Context> Engine::context(uint32_t index) const {
    if (index >= count_ || !array_) return std::nullopt;
    return Context(array_ + size_t{index} * (kContextBytes / 4));
}

void Engine::enable(uint32_t index) {
    const auto context = this->context(index);
    if (!context) return;
    {
        std::lock_guard slot(slots_[index].mutex);
        context->set_field(2, 26, 6, 0);
        Voice& voice = slots_[index].voice.emplace();
        voice.skip = context->field(1, 24, 3) * kSubframeSamples;
    }
    if (wake_) wake_();
}

void Engine::disable(uint32_t index) {
    if (!this->context(index)) return;
    std::lock_guard slot(slots_[index].mutex);
    slots_[index].voice.reset();
}

void Engine::advance_read(uint32_t index, uint32_t blocks) {
    const auto context = this->context(index);
    if (!context) return;
    {
        // The decoder writes the same words.
        std::lock_guard slot(slots_[index].mutex);
        const uint32_t size = std::max<uint32_t>(context->field(0, 22, 5), 1);
        // The count is the title's: reduced first, so the sum stays small.
        const uint32_t read = (context->field(9, 0, 5) + blocks % size) % size;
        context->set_field(9, 0, 5, read);
        if (blocks > 0) context->set_field(1, 31, 1, 1);
        if (auto& voice = slots_[index].voice) {
            voice->trace.taken += blocks;
            ++voice->trace.takes;
            voice->trace.partial_takes += blocks < size ? 1 : 0;
        }
    }
    if (wake_) wake_();
}

bool Engine::command(uint32_t command, const void* data) {
    const auto word = [data](size_t at) -> std::optional<uint32_t> {
        if (!data) return std::nullopt;
        uint32_t v = 0;
        std::memcpy(&v, static_cast<const uint8_t*>(data) + at * 4, sizeof v);
        return v;
    };
    std::shared_lock lock(lock_);
    switch (command) {
        case kEnableXma: enable(word(0).value_or(UINT32_MAX)); break;
        case kDisableXma: disable(word(0).value_or(UINT32_MAX)); break;
        case kEnableXmas:
        case kDisableXmas:
            for (uint32_t i = 0; i < count_; ++i) {
                if ((word(i / 32).value_or(0) & (1u << (i % 32))) == 0) continue;
                if (command == kEnableXmas) enable(i);
                else disable(i);
            }
            break;
        case kAdvanceXmaRead: advance_read(word(0).value_or(UINT32_MAX), word(1).value_or(0)); break;
        default: return false;
    }
    return true;
}

void Engine::pump() {
    std::shared_lock lock(lock_);
    for (uint32_t index = 0; index < count_; ++index) {
        const auto context = this->context(index);
        std::lock_guard slot(slots_[index].mutex);
        if (context && slots_[index].voice) xma::pump(*context, *slots_[index].voice, memory_);
    }
}

std::optional<Trace> Engine::trace(uint32_t index) {
    std::shared_lock lock(lock_);
    if (!this->context(index)) return std::nullopt;
    std::lock_guard slot(slots_[index].mutex);
    if (!slots_[index].voice) return std::nullopt;
    return slots_[index].voice->trace;
}

}  // namespace rt::xma
