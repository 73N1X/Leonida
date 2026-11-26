// leonida::splat_output_alpha (src/spirv_output_splat.h) over hand-built SPIR-V: a fragment output at a Location gets
// each store's value as .wwww (an A8_UNORM target drawn into an R8 attachment), other outputs are left alone, and a
// module without a four-component output there is refused. No Vulkan, no GPU, no shader files.
#include "../src/spirv_output_splat.h"

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "spirv_output_splat_test: FAILED: %s\n", what);
        ++failures;
    }
}

using Words = std::vector<std::uint32_t>;

void inst(Words& words, std::uint16_t op, std::initializer_list<std::uint32_t> operands) {
    words.push_back((static_cast<std::uint32_t>(operands.size() + 1) << 16) | op);
    words.insert(words.end(), operands);
}

// Opcodes and enumerants, from the SPIR-V specification.
constexpr std::uint16_t op_type_void = 19, op_type_float = 22, op_type_vector = 23, op_type_pointer = 32,
                        op_type_function = 33, op_constant = 43, op_constant_composite = 44, op_function = 54,
                        op_function_end = 56, op_variable = 59, op_load = 61, op_store = 62, op_decorate = 71,
                        op_vector_shuffle = 79, op_label = 248, op_return = 253;
constexpr std::uint32_t decoration_location = 30, class_output = 3;

// Two vec4 outputs (Location 0 and 1), each stored once; Location 0 is also loaded and stored again, as the
// translator keeps an earlier export. Ids: 1 void, 2 float, 3 vec4, 4 pointer, 5 fn type, 6/7 outputs, 8 one,
// 9 vec4 constant, 10 function, 11 label, 12 load.
Words module(std::uint32_t second_location = 1) {
    Words w = {0x07230203, 0x00010600, 0, 13, 0};
    inst(w, op_decorate, {6, decoration_location, 0});
    inst(w, op_decorate, {7, decoration_location, second_location});
    inst(w, op_type_void, {1});
    inst(w, op_type_float, {2, 32});
    inst(w, op_type_vector, {3, 2, 4});
    inst(w, op_type_pointer, {4, class_output, 3});
    inst(w, op_type_function, {5, 1});
    inst(w, op_variable, {4, 6, class_output});
    inst(w, op_variable, {4, 7, class_output});
    inst(w, op_constant, {2, 8, 0x3f800000});
    inst(w, op_constant_composite, {3, 9, 8, 8, 8, 8});
    inst(w, op_function, {1, 10, 0, 5});
    inst(w, op_label, {11});
    inst(w, op_store, {6, 9});
    inst(w, op_store, {7, 9});
    inst(w, op_load, {3, 12, 6});
    inst(w, op_store, {6, 12});
    inst(w, op_return, {});
    inst(w, op_function_end, {});
    return w;
}

struct Store {
    std::uint32_t pointer, value;
};

// The stores of a module, and the shuffles that make each value (result id -> its components and source).
void scan(const Words& w, std::vector<Store>& stores, std::vector<Words>& shuffles) {
    for (std::size_t at = 5; at < w.size();) {
        const std::uint32_t count = w[at] >> 16;
        const std::uint16_t op = w[at] & 0xffff;
        if (op == op_store) stores.push_back({w[at + 1], w[at + 2]});
        if (op == op_vector_shuffle) shuffles.push_back(Words(w.begin() + std::ptrdiff_t(at), w.begin() + std::ptrdiff_t(at + count)));
        at += count;
    }
}

}  // namespace

int main() {
    {
        std::string why;
        const Words out = leonida::splat_output_alpha(module(), 0, why);
        check(!out.empty(), "a vec4 output at Location 0 is patched");
        std::vector<Store> stores;
        std::vector<Words> shuffles;
        scan(out, stores, shuffles);
        check(shuffles.size() == 2, "each store to Location 0 gets a shuffle (two stores)");
        check(stores.size() == 3, "the store count is unchanged");
        check(out[3] == 15, "the id bound grows by the shuffles");
        bool all_alpha = true;
        for (const Words& s : shuffles)
            all_alpha &= s.size() == 9 && s[1] == 3 && s[5] == 3 && s[6] == 3 && s[7] == 3 && s[8] == 3;
        check(all_alpha, "the shuffles are vec4 .wwww of the stored value");
        bool location0_splatted = true, location1_unchanged = false;
        for (const Store& s : stores) {
            if (s.pointer == 6) location0_splatted &= s.value >= 13;
            if (s.pointer == 7) location1_unchanged = s.value == 9;
        }
        check(location0_splatted, "Location 0's stores store the shuffles");
        check(location1_unchanged, "Location 1's store is left alone");
    }
    {
        std::string why;
        check(leonida::splat_output_alpha(module(), 3, why).empty() && !why.empty(),
              "no output at the Location: refused with a reason");
    }
    {
        std::string why;
        Words broken = module();
        broken.back() = (100u << 16) | op_function_end;  // runs past the end
        check(leonida::splat_output_alpha(broken, 0, why).empty(), "a malformed module is refused");
    }
    if (failures) return 1;
    std::printf("spirv_output_splat_test: ok\n");
    return 0;
}
