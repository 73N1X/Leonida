#pragma once

// A fragment shader's colour output whose alpha is stored in all four components (`.wwww`), for a render target the
// GPU has no format for: an A8_UNORM target is an R8_UNORM attachment, which stores the first component, while the
// pixel shader writes the target's one channel, alpha, in the fourth (D3D12's SV_Target.a for an A8 target).
//
// The translator (tools/rdna2spirv.py `export`) writes each colour output as one whole-vector OpStore to an Output
// variable decorated with its Location. Each such store is given an OpVectorShuffle of the stored value first. Loads
// of the output (the translator keeps an earlier export for lanes that do not export) read the splatted value, which
// splats to itself, so a later partial export still ends with the latest alpha in every component.
//
// No Vulkan headers; the caller has checked the binary's framing (spirv_words.h). Portable C++20.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace leonida {

// `words` with every store to the Output variable at `location` made `.wwww`, or an empty vector with `why` set when
// the module has no such four-component output (then the caller leaves the target as it is).
inline std::vector<std::uint32_t> splat_output_alpha(std::span<const std::uint32_t> words, std::uint32_t location,
                                                     std::string& why) {
    constexpr std::uint32_t magic = 0x07230203;
    constexpr std::uint16_t op_type_vector = 23, op_type_pointer = 32, op_variable = 59, op_store = 62,
                            op_decorate = 71, op_vector_shuffle = 79;
    constexpr std::uint32_t decoration_location = 30, class_output = 3;
    if (words.size() < 5 || words[0] != magic) {
        why = "not a SPIR-V module";
        return {};
    }
    const auto for_each = [&](auto&& visit) {
        for (std::size_t at = 5; at < words.size();) {
            const std::uint32_t count = words[at] >> 16;
            if (!count || at + count > words.size()) return false;
            visit(at, static_cast<std::uint16_t>(words[at] & 0xffff), count);
            at += count;
        }
        return true;
    };
    // The Location decorations, the Output variables and their pointer types, the vector types.
    std::vector<std::uint32_t> at_location;
    struct Variable {
        std::uint32_t id, pointer;
    };
    std::vector<Variable> outputs;
    struct Pointer {
        std::uint32_t id, pointee;
    };
    std::vector<Pointer> output_pointers;
    struct Vector {
        std::uint32_t id, count;
    };
    std::vector<Vector> vectors;
    const bool framed = for_each([&](std::size_t at, std::uint16_t op, std::uint32_t count) {
        if (op == op_decorate && count >= 4 && words[at + 2] == decoration_location && words[at + 3] == location)
            at_location.push_back(words[at + 1]);
        else if (op == op_variable && count >= 4 && words[at + 3] == class_output)
            outputs.push_back({words[at + 2], words[at + 1]});
        else if (op == op_type_pointer && count >= 4 && words[at + 2] == class_output)
            output_pointers.push_back({words[at + 1], words[at + 3]});
        else if (op == op_type_vector && count >= 4)
            vectors.push_back({words[at + 1], words[at + 3]});
    });
    if (!framed) {
        why = "an instruction runs past the module";
        return {};
    }
    std::uint32_t variable = 0, vector_type = 0;
    for (const Variable& v : outputs) {
        bool located = false;
        for (std::uint32_t id : at_location) located |= id == v.id;
        if (!located) continue;
        for (const Pointer& p : output_pointers)
            if (p.id == v.pointer)
                for (const Vector& vec : vectors)
                    if (vec.id == p.pointee && vec.count == 4) vector_type = vec.id;
        if (vector_type) {
            variable = v.id;
            break;
        }
    }
    if (!variable) {
        why = "no four-component Output variable at Location " + std::to_string(location);
        return {};
    }
    std::vector<std::uint32_t> out;
    out.reserve(words.size() + 64);
    out.insert(out.end(), words.begin(), words.begin() + 5);
    std::uint32_t bound = words[3];
    unsigned stores = 0;
    for_each([&](std::size_t at, std::uint16_t op, std::uint32_t count) {
        if (op == op_store && count >= 3 && words[at + 1] == variable) {
            const std::uint32_t splat = bound++;
            const std::uint32_t value = words[at + 2];
            out.insert(out.end(), {(9u << 16) | op_vector_shuffle, vector_type, splat, value, value, 3u, 3u, 3u, 3u});
            out.push_back(words[at]);
            out.push_back(variable);
            out.push_back(splat);
            out.insert(out.end(), words.begin() + std::ptrdiff_t(at + 3), words.begin() + std::ptrdiff_t(at + count));
            ++stores;
            return;
        }
        out.insert(out.end(), words.begin() + std::ptrdiff_t(at), words.begin() + std::ptrdiff_t(at + count));
    });
    if (!stores) {
        why = "the output at Location " + std::to_string(location) + " is never stored";
        return {};
    }
    out[3] = bound;
    return out;
}

}  // namespace leonida
