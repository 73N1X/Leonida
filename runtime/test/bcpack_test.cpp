// rt::bcpack (runtime/src/bcpack.h), the BCPack decoder ported from VIctory's victory-formats (GPL-3.0-or-later,
// Marvisak and contributors): SHA-256, table parsing and loading, and decodes of streams this test encodes itself. The
// tables, the encoder and the expected blocks follow VIctory's own tests in victory-formats/src/bcpack.rs and
// victory-hle/src/modules/dstorage.rs. No game data.
//
//   bcpack_test             tables the test makes (uniform models); needs nothing
//   bcpack_test --tables    the real format tables: LEONIDA_BCPACK_TABLES, else bcpack-tables.bin next to this
//                           executable; prints SKIP and exits 77 without them
//   bcpack_test --tables --dump DIR
//                           also writes the streams it built as DIR/<name>.bc<n> with this decoder's output as
//                           DIR/<name>.out, the layout VIctory's ignored reference_chunks test reads, so that its
//                           decoder can check them (VICTORY_BCPACK_TABLES, VICTORY_BCPACK_CASES=DIR). Keep DIR outside
//                           the repository: the blocks can quote dictionary entries of the tables.
#include "bcpack.h"

#include "bcpack_decode.h"
#include "test_temp_dir.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

namespace bcpack = rt::bcpack;
using Bytes = std::vector<std::uint8_t>;
using Records = std::vector<Bytes>;

int g_failures = 0;

void expect(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

std::string hex(const std::uint8_t* p, std::size_t n) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    for (std::size_t k = 0; k < n; ++k) {
        s += kHex[p[k] >> 4];
        s += kHex[p[k] & 15];
    }
    return s;
}

std::string sha256_hex(const void* data, std::size_t size) {
    std::uint8_t digest[32];
    bcpack::sha256(data, size, digest);
    return hex(digest, 32);
}

bool write_file(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// xorshift64, as VIctory's tests use.
struct Random {
    std::uint64_t seed;
    std::uint64_t next() {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    }
};

void put16(Bytes& t, unsigned v) {
    t.push_back(static_cast<std::uint8_t>(v));
    t.push_back(static_cast<std::uint8_t>(v >> 8));
}

void uniform_models(Bytes& t) {
    for (unsigned m = 0; m < 128; ++m) {
        const unsigned g = m / 16;
        for (unsigned b = 0; b < 9; ++b) put16(t, b < g + 2 ? 1u << (15 - g) : 0);
    }
}

// Tables we made: uniform models (model 16 g + shape: g + 2 buckets of 2^(15 - g)), rank tables that pass the
// residual bits through, one BC7 control code (0: mode 6, one subset) and empty dictionaries. VIctory's tables().
Bytes synthetic_tables() {
    Bytes t;
    uniform_models(t);
    for (unsigned v = 0; v < 256; ++v) t.push_back(static_cast<std::uint8_t>(v & 0x77));               // rank1
    for (unsigned v = 0; v < 64; ++v) t.push_back(static_cast<std::uint8_t>(v));                       // rank3a
    for (unsigned v = 0; v < 256; ++v) t.push_back(static_cast<std::uint8_t>(v));                      // rank3c
    for (unsigned v = 0; v < 64; ++v) t.push_back(static_cast<std::uint8_t>((v & 7) | (v >> 3) << 4));  // rank4
    for (unsigned v = 0; v < 64; ++v) t.push_back(static_cast<std::uint8_t>(v));                       // rank5
    for (unsigned code = 0; code < 512; ++code) put16(t, code == 0 ? 0x46 : 0);
    t.resize(bcpack::kTablesSize, 0);
    return t;
}

// The tables of VIctory's DirectStorage tests: the same models, BC4 index pairs (low 3 bits, next 3 bits), nothing
// else.
Bytes dstorage_tables() {
    Bytes t;
    uniform_models(t);
    t.resize(t.size() + 256 + 64 + 256, 0);
    for (unsigned v = 0; v < 64; ++v) t.push_back(static_cast<std::uint8_t>((v & 7) | (v >> 3) << 4));
    t.resize(bcpack::kTablesSize, 0);
    return t;
}

// The test encoder. A model's slots, from the table bytes: symbol 0 owns slots 1 .. f0 - 1, bucket b > 0 holds
// 2^(b-1) symbols of frequency f[b] each. Frequency 0: the model cannot code the symbol.
void slots_of(const Bytes& table, unsigned model, unsigned sym, std::uint32_t* freq, std::uint32_t* start) {
    std::uint32_t f[9];
    for (unsigned b = 0; b < 9; ++b)
        f[b] = std::uint32_t{table[(model * 9 + b) * 2]} | std::uint32_t{table[(model * 9 + b) * 2 + 1]} << 8;
    const unsigned used = model / 16 + 2;
    if (sym == 0) {
        *freq = f[0] - 1;
        *start = 1;
        return;
    }
    const auto k = static_cast<unsigned>(std::bit_width(sym));
    if (k >= used) {
        *freq = 0;
        *start = 0;
        return;
    }
    std::uint32_t at = f[0];
    for (unsigned b = 1; b < k; ++b) at += f[b] << (b - 1);
    *freq = f[k];
    *start = at + (sym - (1u << (k - 1))) * f[k];
}

// A lane's symbols as one fragment: header byte `header` (model width * 16 + header), rANS from 2^24, no reset, so the
// lane keeps 2^24 when it ends. VIctory's fragment(), for any model.
Bytes fragment(const Bytes& table, unsigned width, unsigned header, const Bytes& symbols, bool* ok) {
    const unsigned model = width * 16 + header;
    std::uint32_t x = 1u << 24;
    Bytes bytes;
    for (auto it = symbols.rbegin(); it != symbols.rend(); ++it) {
        std::uint32_t freq = 0, start = 0;
        slots_of(table, model, *it, &freq, &start);
        if (freq == 0) {
            *ok = false;
            return {};
        }
        while (x >= (freq << 16)) {
            bytes.push_back(static_cast<std::uint8_t>(x));
            x >>= 8;
        }
        x = ((x / freq) << 16) + x % freq + start;
    }
    for (unsigned k = 0; k < 4; ++k) bytes.push_back(static_cast<std::uint8_t>(x >> (8 * k)));
    std::reverse(bytes.begin(), bytes.end());
    if (bytes.size() > 65792) {
        *ok = false;
        return {};
    }
    Bytes out{static_cast<std::uint8_t>(header)};
    if (bytes.size() > 256) {
        out[0] |= 0x20;
        const std::size_t n = bytes.size() - 257;
        out.push_back(static_cast<std::uint8_t>(n));
        out.push_back(static_cast<std::uint8_t>(n >> 8));
    } else {
        out.push_back(static_cast<std::uint8_t>(bytes.size() - 1));
    }
    out.insert(out.end(), bytes.begin(), bytes.end());
    return out;
}

struct Queues {
    std::deque<std::uint8_t> lane[16];
    Bytes out;
    bool starved = false;
};

int queue_fetch(void* context, unsigned lane) {
    auto* q = static_cast<Queues*>(context);
    if (q->lane[lane].empty()) {
        q->starved = true;  // a parked lane asked for input
        return -1;
    }
    const std::uint8_t b = q->lane[lane].front();
    q->lane[lane].pop_front();
    q->out.push_back(b);
    return b;
}

Bytes concat(const Records& records) {
    Bytes all;
    for (const Bytes& r : records) all.insert(all.end(), r.begin(), r.end());
    return all;
}

// Interleaves per-lane fragments the way the decoder takes bytes. A lane that has finished still asks: a filler
// fragment (two payload bytes) brings its state to 2^24 with a byte latched, which parks it for good. Checks that the
// lanes give the records back. VIctory's stream(); `headers` picks each lane's model (nullptr: shape 0).
Bytes stream(const bcpack::Tables& t, const Bytes& table, unsigned bc, const Records& records,
             const unsigned* headers = nullptr) {
    bcpack::Layout l{};
    if (!bcpack::layout(bc, &l)) {
        expect(false, "stream: a layout for BC" + std::to_string(bc));
        return {};
    }
    Queues q;
    for (unsigned j = 0; j < l.lanes; ++j) {
        Bytes symbols;
        for (const Bytes& r : records)
            for (unsigned h = 0; h < l.per; ++h) symbols.push_back(r[j * l.per + h]);
        bool ok = true;
        Bytes bytes = fragment(table, l.widths[j], headers ? headers[j] : 0, symbols, &ok);
        if (!ok) {
            expect(false, "stream: lane " + std::to_string(j) + " fits one fragment of its model");
            return {};
        }
        bytes.insert(bytes.end(), {0, 1, 0, 0});
        q.lane[j].assign(bytes.begin(), bytes.end());
    }
    Bytes check(records.size() * l.lanes * l.per);
    const bcpack::Error error = bcpack::unpack_records(bc, t, records.size(), &queue_fetch, &q, check.data());
    expect(!error, std::string("stream: the lanes decode (") + (error ? error : "") + ")");
    expect(!q.starved, "stream: no parked lane asks for input");
    expect(check == concat(records), "stream: the lanes give the records back, BC" + std::to_string(bc));
    return q.out;
}

std::uint8_t zigzag(int v) {
    return static_cast<std::uint8_t>(v >= 0 ? 2 * v : -2 * v - 1);
}

constexpr std::uint64_t kBc4Code[8] = {0, 2, 3, 4, 5, 6, 7, 1};

// A BC4 block: endpoints as stored, pixel p -> code[(p + add) & 7].
Bytes bc4_block(unsigned e0, unsigned e1, unsigned add) {
    std::uint64_t v = e0 | std::uint64_t{e1} << 8;
    for (unsigned p = 0; p < 16; ++p) v |= kBc4Code[(p + add) & 7] << (16 + 3 * p);
    Bytes out(8);
    for (unsigned k = 0; k < 8; ++k) out[k] = static_cast<std::uint8_t>(v >> (8 * k));
    return out;
}

// Pseudo-random records that fit each lane's alphabet.
Records random_records(unsigned bc, std::size_t blocks, std::uint64_t seed) {
    bcpack::Layout l{};
    bcpack::layout(bc, &l);
    Random r{seed};
    Records records(blocks);
    for (Bytes& rec : records) {
        rec.resize(l.lanes * l.per);
        for (unsigned k = 0; k < rec.size(); ++k)
            rec[k] = static_cast<std::uint8_t>(r.next() % (std::uint64_t{1} << (l.widths[k / l.per] + 1)));
    }
    return records;
}

bcpack::TablesPtr parse(const Bytes& bytes) {
    bcpack::Error error = nullptr;
    bcpack::TablesPtr t = bcpack::parse_tables(bytes.data(), bytes.size(), &error);
    expect(t != nullptr, std::string("synthetic tables parse (") + (error ? error : "") + ")");
    return t;
}

std::string error_of(bcpack::Error e) {
    return e ? e : "";
}

void test_sha256() {
    const auto check = [](const std::string& message, const char* want) {
        expect(sha256_hex(message.data(), message.size()) == want, "SHA-256 of a " + std::to_string(message.size()) +
                                                                       "-byte message");
    };
    check("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 56 bytes: the length no longer fits the first padding block.
    check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    check("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
          "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
          "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    check(std::string(1000000, 'a'), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

void test_parse() {
    bcpack::Error error = nullptr;
    const Bytes zeros(16, 0);
    expect(!bcpack::parse_tables(zeros.data(), zeros.size(), &error) && contains(error_of(error), "wrong size"),
           "16 bytes are refused (wrong size)");
    expect(!bcpack::parse_tables(nullptr, bcpack::kTablesSize, &error), "no bytes are refused");
    Bytes t = synthetic_tables();
    expect(!bcpack::parse_tables(t.data(), t.size() - 1, &error), "one byte short is refused");
    expect(bcpack::parse_tables(t.data(), t.size(), &error) != nullptr, "the synthetic tables parse");
    Bytes no_tiling = t;
    no_tiling[0] = 1;
    expect(!bcpack::parse_tables(no_tiling.data(), no_tiling.size(), &error) &&
               contains(error_of(error), "tile 65536"),
           "a model that does not tile 65536 is refused");
    Bytes empty_bucket = t;
    empty_bucket[2] = 0;  // model 0, bucket 1
    empty_bucket[3] = 0;
    expect(!bcpack::parse_tables(empty_bucket.data(), empty_bucket.size(), &error) &&
               contains(error_of(error), "empty bucket"),
           "a model with an empty bucket is refused");
    // One-slot buckets where they fit, bucket 0 takes the rest (the narrowest windows); VIctory's narrow tables.
    Bytes narrow = t;
    for (unsigned m = 0; m < 128; ++m) {
        const unsigned used = m / 16 + 2;
        unsigned rest = 1u << 16;
        for (unsigned b = 1; b < used; ++b) rest -= 1u << (b - 1);
        for (unsigned b = 0; b < used; ++b) {
            const unsigned f = b == 0 ? rest : 1;
            narrow[(m * 9 + b) * 2] = static_cast<std::uint8_t>(f);
            narrow[(m * 9 + b) * 2 + 1] = static_cast<std::uint8_t>(f >> 8);
        }
    }
    expect(bcpack::parse_tables(narrow.data(), narrow.size(), &error) != nullptr, "one-slot buckets parse");
}

void test_layouts() {
    const struct {
        unsigned bc, lanes, per, block;
    } want[] = {{1, 12, 1, 8}, {3, 12, 2, 16}, {4, 12, 1, 8}, {5, 12, 2, 16}, {6, 16, 2, 16}, {7, 16, 2, 16}};
    for (const auto& w : want) {
        bcpack::Layout l{};
        expect(bcpack::layout(w.bc, &l) && l.lanes == w.lanes && l.per == w.per && l.block_bytes == w.block,
               "the BC" + std::to_string(w.bc) + " layout");
    }
    bcpack::Layout l{};
    expect(!bcpack::layout(2, &l) && !bcpack::layout(0, &l) && !bcpack::layout(8, &l), "no layout for BC2, 0 or 8");
}

// Two BC4 blocks from VIctory's test encoder, as its DirectStorage tests keep them: endpoints 10, -3 with indices
// p -> p & 7, then the same block plus 1 everywhere. Fixed bytes, so this checks the decoder without the encoder here.
void test_fixed_bc4_stream() {
    static constexpr std::uint8_t kStream[88] = {
        0, 0, 0, 0, 0, 0, 0, 0, 5, 5, 5, 5, 4, 4, 4, 4, 1, 1, 1, 1, 0x10, 0x10, 0x10, 0x10, 2, 1,
        0, 0, 0, 0, 0, 0, 0, 0, 0x14, 5, 0x20, 0x68, 0xb0, 0xf8, 3, 3, 2, 2, 0x24, 0x24, 0x24,
        0x24, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 0x10, 0x10, 0x10, 0x10, 0, 0, 0, 0,
        0x20, 0x68, 0xb0, 0xf8, 0x24, 0x24, 0x24, 0x24, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    const bcpack::TablesPtr t = parse(dstorage_tables());
    if (!t) return;
    Bytes want = bc4_block(138, 125, 0);
    const Bytes second = bc4_block(139, 126, 1);
    want.insert(want.end(), second.begin(), second.end());
    Bytes dst(16, 0xcc);
    const bcpack::Error error = bcpack::decode(4, kStream, sizeof kStream, dst.data(), dst.size(), *t);
    expect(!error && dst == want, "VIctory's fixed two-block BC4 stream (" + error_of(error) + ")");
    // Bytes after the last one the lanes need are ignored.
    Bytes longer(kStream, kStream + sizeof kStream);
    longer.resize(longer.size() + 100, 0xee);
    std::fill(dst.begin(), dst.end(), std::uint8_t{0});
    expect(!bcpack::decode(4, longer.data(), longer.size(), dst.data(), dst.size(), *t) && dst == want,
           "trailing bytes are ignored");
}

void test_bc4_references() {
    const Bytes table = synthetic_tables();
    const bcpack::TablesPtr t = parse(table);
    if (!t) return;
    // Block 0: endpoints 10, -3 (+128 stored), indices p -> p & 7.
    Bytes b0 = {0, 0, zigzag(10), zigzag(-3)};
    for (unsigned k = 0; k < 8; ++k) b0.push_back(static_cast<std::uint8_t>(((2 * k) & 7) | ((2 * k + 1) & 7) << 3));
    // Block 1 refers to block 0 for both: residual endpoints +1, +1; indices +1.
    Bytes b1 = {0, 1, zigzag(1), zigzag(1)};
    for (unsigned k = 0; k < 8; ++k) b1.push_back(1 | 1 << 3);
    const Bytes src = stream(*t, table, 4, {b0, b1});
    Bytes dst(16);
    const bcpack::Error error = bcpack::decode(4, src.data(), src.size(), dst.data(), dst.size(), *t);
    expect(!error, "BC4 blocks with and without references decode (" + error_of(error) + ")");
    expect(Bytes(dst.begin(), dst.begin() + 8) == bc4_block(138, 125, 0), "BC4 block 0");
    expect(Bytes(dst.begin() + 8, dst.end()) == bc4_block(139, 126, 1), "BC4 block 1 builds on block 0");
}

void test_round_trips() {
    const Bytes table = synthetic_tables();
    const bcpack::TablesPtr t = parse(table);
    if (!t) return;
    // Long fragments (over 256 payload bytes) on the wide lanes, with VIctory's records.
    Records records;
    for (unsigned i = 0; i < 400; ++i) {
        Bytes r = {0, 0, static_cast<std::uint8_t>(i * 7), static_cast<std::uint8_t>(i * 13)};
        for (unsigned k = 0; k < 8; ++k) r.push_back(static_cast<std::uint8_t>((i + k) * 5 % 64));
        records.push_back(r);
    }
    stream(*t, table, 4, records);
    // Every layout, short and long fragments.
    for (const unsigned bc : {1u, 3u, 4u, 5u, 6u, 7u})
        for (const std::size_t blocks : {1u, 2u, 17u, 300u, 1000u})
            stream(*t, table, bc, random_records(bc, blocks, 0x9e3779b97f4a7c15ull));
    Bytes none;
    expect(bcpack::unpack_records(1, *t, 0, &queue_fetch, nullptr, none.data()) == nullptr, "no blocks, no input");
}

void test_bc7_mode6() {
    const Bytes table = synthetic_tables();
    const bcpack::TablesPtr t = parse(table);
    if (!t) return;
    // Control code 0 -> mode 6; endpoints R 8 bits (p-bit low), G B A 7 bits.
    Bytes s(32, 0);
    s[4] = zigzag(0x55);  // R0 = 0x55: field 0x2a, p-bit 1
    s[5] = zigzag(-1);    // R1 = 0xff (8 bits)
    s[6] = zigzag(3);
    s[10] = zigzag(0x7f >> 1);
    const Bytes src = stream(*t, table, 7, {s});
    Bytes dst(16);
    const bcpack::Error error = bcpack::decode(7, src.data(), src.size(), dst.data(), dst.size(), *t);
    // Mode 6 (7 bits), R0 R1 G0 G1 B0 B1 A0 A1 at 7 bits each, then the two p-bits; the indices are all 0.
    std::uint64_t lo = 0, hi = 0;
    unsigned at = 0;
    const auto put = [&](std::uint64_t v, unsigned n) {
        for (unsigned k = 0; k < n; ++k, ++at)
            if (v >> k & 1) (at < 64 ? lo : hi) |= std::uint64_t{1} << (at & 63);
    };
    put(1 << 6, 7);
    for (const unsigned v : {0x2au, 0x7fu, 3u, 0u, 0u, 0u, 0x3fu, 0u}) put(v, 7);
    put(1, 1);
    put(1, 1);
    Bytes want(16);
    for (unsigned k = 0; k < 8; ++k) {
        want[k] = static_cast<std::uint8_t>(lo >> (8 * k));
        want[8 + k] = static_cast<std::uint8_t>(hi >> (8 * k));
    }
    expect(!error && dst == want, "a BC7 mode 6 block (" + error_of(error) + ")");
}

void test_malformed() {
    const Bytes table = synthetic_tables();
    const bcpack::TablesPtr t = parse(table);
    if (!t) return;
    Records records;
    for (unsigned i = 0; i < 64; ++i)
        records.push_back({0, 0, static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i), 1, 2, 3, 4, 5, 6, 7, 8});
    const Bytes src = stream(*t, table, 4, records);
    Bytes dst(64 * 8);
    expect(!bcpack::decode(4, src.data(), src.size(), dst.data(), dst.size(), *t), "64 BC4 blocks decode");
    expect(bcpack::decode(4, src.data(), src.size() / 2, dst.data(), dst.size(), *t) != nullptr, "truncated: error");
    // Headers promising more payload than the stream holds. (Other corrupt headers decode to garbage: the format has
    // no redundancy.)
    Bytes headers(8, 0x30);
    headers.resize(16, 0xff);
    expect(bcpack::decode(4, headers.data(), headers.size(), dst.data(), 8, *t) != nullptr, "headers only: error");
    expect(bcpack::decode(4, nullptr, 0, dst.data(), dst.size(), *t) != nullptr, "no input: error");
    // Destination not a whole number of blocks, empty, more blocks than coded, missing.
    expect(bcpack::decode(4, src.data(), src.size(), dst.data(), 12, *t) != nullptr, "12 bytes of BC4: error");
    expect(bcpack::decode(4, src.data(), src.size(), dst.data(), 0, *t) != nullptr, "no blocks: error");
    Bytes more(65 * 8);
    expect(bcpack::decode(4, src.data(), src.size(), more.data(), more.size(), *t) != nullptr,
           "more blocks than coded: error");
    expect(bcpack::decode(4, src.data(), src.size(), nullptr, dst.size(), *t) != nullptr, "no destination: error");
    expect(bcpack::decode(4, nullptr, src.size(), dst.data(), dst.size(), *t) != nullptr, "no source: error");
    expect(bcpack::decode(7, src.data(), src.size(), dst.data(), 24, *t) != nullptr, "24 bytes of BC7: error");
    // Unsupported formats.
    for (const unsigned bc : {0u, 2u, 8u, 0x104u})
        expect(contains(error_of(bcpack::decode(bc, src.data(), src.size(), dst.data(), dst.size(), *t)),
                        "unsupported"),
               "BC" + std::to_string(bc) + " is unsupported");
    // A BC7 block whose control code the tables leave unused, and a BC6H block whose mode is out of range.
    Bytes s(32, 0);
    s[30] = 1;
    const Bytes bad7 = stream(*t, table, 7, {s});
    expect(contains(error_of(bcpack::decode(7, bad7.data(), bad7.size(), dst.data(), 16, *t)), "control code"),
           "an unused BC7 control code is an error");
    s[30] = 14;
    const Bytes bad6 = stream(*t, table, 6, {s});
    expect(contains(error_of(bcpack::decode(6, bad6.data(), bad6.size(), dst.data(), 16, *t)), "mode"),
           "BC6H mode 15 is an error");
}

// Noise must never crash, whatever it decodes to.
void test_noise(const bcpack::Tables& t) {
    Random r{0x2545f4914f6cdd1dull};
    Bytes dst(16 * 40);
    unsigned decoded = 0;
    for (unsigned round = 0; round < 300; ++round) {
        Bytes src(round * 7 % 900);
        for (std::uint8_t& b : src) b = static_cast<std::uint8_t>(r.next());
        for (const unsigned bc : {1u, 3u, 4u, 5u, 6u, 7u})
            decoded += bcpack::decode(bc, src.data(), src.size(), dst.data(), 16 * (1 + round % 40), t) ? 0 : 1;
    }
    std::printf("noise: %u of 1800 random streams decoded without an error\n", decoded);
}

void test_load(const std::filesystem::path& dir) {
    std::string why;
    expect(!bcpack::load_tables(dir / "missing.bin", &why) && contains(why, "cannot open"), "a missing file: " + why);
    write_file(dir / "synthetic.bin", synthetic_tables());
    expect(!bcpack::load_tables(dir / "synthetic.bin", &why) && contains(why, "SHA-256") &&
               contains(why, bcpack::kTablesSha256),
           "tables of the right size but another SHA-256 are refused: " + why);
    write_file(dir / "short.bin", Bytes(100, 0));
    expect(!bcpack::load_tables(dir / "short.bin", &why) && contains(why, "100 bytes"), "a 100-byte file: " + why);
    write_file(dir / "long.bin", Bytes(bcpack::kTablesSize + 1, 0));
    expect(!bcpack::load_tables(dir / "long.bin", &why) && contains(why, "larger than"), "a longer file: " + why);
}

// Without tables the entry point refuses every request (VIctory: every BCPack request fails).
void test_entry_point_without_tables(const std::filesystem::path& dir) {
    set_env(bcpack::kTablesVariable, (dir / "missing.bin").string());
    expect(bcpack::tables_path() == dir / "missing.bin", "LEONIDA_BCPACK_TABLES names the table file");
    std::uint8_t dst[8] = {};
    const std::uint8_t src[4] = {};
    expect(!leonida_bcpack_decode(4, src, sizeof src, dst, sizeof dst), "no tables: leonida_bcpack_decode is false");
    expect(bcpack::process_tables() == nullptr, "no tables: process_tables is null");
}

// Records the real tables accept: BC6H modes 1-14, BC7 control codes the tables define.
Records real_records(const Bytes& table, unsigned bc, std::size_t blocks, std::uint64_t seed) {
    Records records = random_records(bc, blocks, seed);
    Random r{seed ^ 0x5bd1e995};
    constexpr std::size_t kCtrl = 128 * 9 * 2 + 256 + 64 + 256 + 64 + 64;
    for (Bytes& s : records) {
        if (bc == 6) s[30] = static_cast<std::uint8_t>(r.next() % 14);
        if (bc == 7) {
            for (;;) {
                s[30] = static_cast<std::uint8_t>(r.next() % 32);
                s[31] = static_cast<std::uint8_t>(r.next() % 32);
                const std::size_t at = kCtrl + 2 * (((s[31] & 15u) << 5) | (s[30] & 31u));
                if (table[at] | table[at + 1]) break;
            }
        }
    }
    return records;
}

// SHA-256 of the blocks VIctory's decoder makes of the streams below (2000 blocks per format, built against the real
// tables): its reference_chunks test passes on the files --dump writes.
struct Golden {
    unsigned bc;
    const char* sha256;
};
constexpr Golden kGolden[] = {
    {1, "dfef7317a1b0f3db05575b1f6ed75dcb7bd1aae2c5c587c364501818d4e7752f"},
    {3, "685ba5fe566f870497ca8d024ad243c75cf2c626df80fe8fe7abbd2063385c79"},
    {4, "5e9c1b2dcce6f8b8e3b6c2801b353829e082c9be107a75b6407db7324f745758"},
    {5, "7287431529fad53f6611bbebee3b35fb36bc50ee37df8f2aa4eb946a0e5c39ed"},
    {6, "f2d6ff19566b79492df0e9fc600386059552444db1e99e18592fdcbcaee604cc"},
    {7, "75a9d7370d55872d24573a2a6a993f5a4a46d9d1648f4e3fb7db6c8b3aef0817"},
};

int run_with_real_tables(const std::filesystem::path& scratch, const char* dump) {
    const std::filesystem::path path = bcpack::tables_path();
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(path, ec)) {
        std::printf("SKIP (no BCPack format tables: set %s or put %s next to the test)\n", bcpack::kTablesVariable,
                    bcpack::kTablesFile);
        return 77;
    }
    std::string why;
    const bcpack::TablesPtr t = bcpack::load_tables(path, &why);
    expect(t != nullptr, "the format tables load: " + why);
    if (!t) return 1;
    const Bytes table = read_file(path);
    expect(sha256_hex(table.data(), table.size()) == [] {
        std::string s = bcpack::kTablesSha256;
        for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c);
        return s;
    }(), "the file hashes to the pinned SHA-256");
    // A changed byte or a shorter file is refused.
    Bytes changed = table;
    changed[changed.size() / 2] ^= 1;
    write_file(scratch / "changed.bin", changed);
    expect(!bcpack::load_tables(scratch / "changed.bin", &why) && contains(why, "SHA-256"),
           "one changed byte is refused: " + why);
    write_file(scratch / "short.bin", Bytes(table.begin(), table.end() - 1));
    expect(!bcpack::load_tables(scratch / "short.bin", &why), "one byte short is refused: " + why);
    const bcpack::Tables* process = bcpack::process_tables();
    expect(process != nullptr, "process_tables loads them");
    // Streams built against the real models: every lane on its own model shape.
    unsigned headers[16];
    for (unsigned j = 0; j < 16; ++j) headers[j] = (j * 5 + 3) & 15;
    for (const Golden& g : kGolden) {
        bcpack::Layout l{};
        bcpack::layout(g.bc, &l);
        const Records records = real_records(table, g.bc, 2000, 0x9e3779b97f4a7c15ull + g.bc);
        const Bytes src = stream(*t, table, g.bc, records, headers);
        Bytes dst(2000 * l.block_bytes), entry(dst.size());
        const bcpack::Error error = bcpack::decode(g.bc, src.data(), src.size(), dst.data(), dst.size(), *t);
        expect(!error, "BC" + std::to_string(g.bc) + " against the real tables decodes (" + error_of(error) + ")");
        expect(process && leonida_bcpack_decode(g.bc, src.data(), src.size(), entry.data(), entry.size()) &&
                   entry == dst,
               "leonida_bcpack_decode gives the same blocks, BC" + std::to_string(g.bc));
        expect(bcpack::decode(g.bc, src.data(), src.size() - 64, dst.data(), dst.size(), *t) != nullptr,
               "a truncated BC" + std::to_string(g.bc) + " stream is an error");
        bcpack::decode(g.bc, src.data(), src.size(), dst.data(), dst.size(), *t);
        const std::string got = sha256_hex(dst.data(), dst.size());
        std::printf("BC%u: %zu bytes packed, blocks %s\n", g.bc, src.size(), got.c_str());
        if (*g.sha256) expect(got == g.sha256, "BC" + std::to_string(g.bc) + " blocks are VIctory's");
        if (dump) {
            const std::string name = "synthetic_bc" + std::to_string(g.bc);
            write_file(std::filesystem::path(dump) / (name + ".bc" + std::to_string(g.bc)), src);
            write_file(std::filesystem::path(dump) / (name + ".out"), dst);
        }
    }
    test_noise(*t);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool tables = false;
    const char* dump = nullptr;
    for (int k = 1; k < argc; ++k) {
        if (std::strcmp(argv[k], "--tables") == 0) {
            tables = true;
        } else if (std::strcmp(argv[k], "--dump") == 0 && k + 1 < argc) {
            dump = argv[++k];
        } else {
            std::fprintf(stderr, "usage: bcpack_test [--tables [--dump DIR]]\n");
            return 2;
        }
    }
    const testfiles::ScratchDir scratch("leonida_bcpack_test");
    int result = 0;
    if (tables) {
        result = run_with_real_tables(scratch.path(), dump);
        if (result == 77) return result;
    } else {
        test_sha256();
        test_parse();
        test_layouts();
        test_fixed_bc4_stream();
        test_bc4_references();
        test_round_trips();
        test_bc7_mode6();
        test_malformed();
        if (const bcpack::TablesPtr t = parse(synthetic_tables())) test_noise(*t);
        test_load(scratch.path());
        test_entry_point_without_tables(scratch.path());
    }
    if (g_failures || result) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("bcpack_test%s: all passed\n", tables ? " --tables" : "");
    return 0;
}
