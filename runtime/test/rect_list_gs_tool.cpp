// The rectangle-list geometry stage generator (src/rect_list_gs.h) as a command, for tests/test_rect_list_gs.py:
//
//     rect_list_gs_tool <vs.spv> <gs.spv> [<vs.spv> <gs.spv> ...]
//
// Each vertex shader's stage is made through the same cached call the D3D12 layer uses (geometry_shader) and written
// to the file after it. One line per pair on stdout: "<pair> <first pair with the same words> <signature key>", so a
// caller sees which pairs the cache served the same words. A refused vertex shader prints
// "<pair> refused: <reason>" and ends the run with exit code 2; a file that cannot be read or written, 1.
// No GPU, no Vulkan loader, no game data.
#include "../src/rect_list_gs.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3 || (argc - 1) % 2) {
        std::fprintf(stderr, "usage: rect_list_gs_tool <vs.spv> <gs.spv> [<vs.spv> <gs.spv> ...]\n");
        return 1;
    }
    std::vector<std::shared_ptr<const std::vector<std::uint32_t>>> made;
    for (int pair = 0; 2 * pair + 2 <= argc; ++pair) {
        const char* vs_path = argv[1 + 2 * pair];
        const char* gs_path = argv[2 + 2 * pair];
        std::ifstream in(vs_path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "%s: cannot be read\n", vs_path);
            return 1;
        }
        const std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        std::vector<std::uint32_t> vs(bytes.size() / 4);
        if (!vs.empty()) std::memcpy(vs.data(), bytes.data(), vs.size() * 4);
        std::string why;
        leonida::rect_list::Signature signature;
        const bool read = leonida::rect_list::read_signature(vs, signature, why);
        const auto words = read ? leonida::rect_list::geometry_shader(vs, why) : nullptr;
        if (!words) {
            std::printf("%d refused: %s\n", pair, why.c_str());
            return 2;
        }
        int first = int(made.size());
        for (int i = 0; i < int(made.size()); ++i)
            if (made[i] == words) {
                first = i;
                break;
            }
        made.push_back(words);
        std::ofstream out(gs_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(words->data()), std::streamsize(words->size() * 4));
        if (!out) {
            std::fprintf(stderr, "%s: cannot be written\n", gs_path);
            return 1;
        }
        std::printf("%d %d %s\n", pair, first, signature.key().c_str());
    }
    return 0;
}
