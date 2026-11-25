// The DirectStorage shim (XCRDAPI.dll, shims/dstorage_xs.cpp) without game files: a synthetic file opened as a GAME:
// path and read through a queue, zlib streams compressed here, status arrays and the order they complete in, failed
// requests, the scheduler's choices (no workers), and destinations in xmem's physical pages.
//
// Ported with the shim from VIctory's crates/victory-hle/src/modules/dstorage.rs (GPL-3.0-or-later; Marvisak and
// contributors, DexNom fork); most cases are VIctory's own tests. Leonida is GPL-3.0-or-later too.
//
// Everything the title calls is reached the way it reaches it: DStorageGetFactory through rt::shims::find, then the
// method tables by byte offset.
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "dstorage_xs.h"
#include "file_system.h"
#include "shims.h"
#include "test_temp_dir.h"

namespace {

namespace ds = rt::dstorage;

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

std::string hex(HRESULT hr) {
    char b[12];
    std::snprintf(b, sizeof b, "0x%08lx", static_cast<unsigned long>(hr));
    return b;
}

void check_hr(HRESULT got, std::int32_t want, const char* what) {
    const bool ok = got == want;
    if (!ok) std::printf("  got %s, want %s\n", hex(got).c_str(), hex(want).c_str());
    check(ok, what);
}

// ---- Calling the shim as title code does

using GetFactoryFn = HRESULT(__stdcall*)(const GUID*, void**);
using RefFn = ULONG(__stdcall*)(void*);
using OpenFileFn = HRESULT(__stdcall*)(void*, const wchar_t*, const GUID*, void**);
using CreateStatusArrayFn = HRESULT(__stdcall*)(void*, std::uint32_t, const char*, const GUID*, void**);
using SetStagingFn = HRESULT(__stdcall*)(void*, std::uint32_t);
using CreateQueueFn = HRESULT(__stdcall*)(void*, const ds::QueueDesc*, const GUID*, void**);
using EnqueueRequestFn = void(__stdcall*)(void*, const ds::Request*);
using EnqueueStatusFn = void(__stdcall*)(void*, void*, std::uint32_t);
using VoidFn = void(__stdcall*)(void*);
using IsCompleteFn = bool(__stdcall*)(void*, std::uint32_t);
using ResultFn = HRESULT(__stdcall*)(void*, std::uint32_t);

// Method `slot` (a byte offset) of `object`.
template <typename F>
F method(void* object, std::size_t slot) {
    const void* const* table = *static_cast<const void* const* const*>(object);
    return reinterpret_cast<F>(const_cast<void*>(table[slot / 8]));
}

const GUID& factory_iid() { return *reinterpret_cast<const GUID*>(&ds::kFactoryIid); }

void* factory() {
    static void* const f = [] {
        const auto get = reinterpret_cast<GetFactoryFn>(rt::shims::find("xcrdapi.dll", "DStorageGetFactory"));
        void* out = nullptr;
        if (get) get(&factory_iid(), &out);
        return out;
    }();
    return f;
}

HRESULT open(const wchar_t* path, void** file) {
    *file = nullptr;
    return method<OpenFileFn>(factory(), ds::slot::kOpenFile)(factory(), path, nullptr, file);
}

void* status_array(std::uint32_t capacity) {
    void* array = nullptr;
    method<CreateStatusArrayFn>(factory(), ds::slot::kCreateStatusArray)(factory(), capacity, "test", nullptr, &array);
    return array;
}

void* new_queue(std::uint64_t source_type, std::uint16_t capacity = 0x80, std::int8_t priority = 0) {
    const ds::QueueDesc desc{source_type, capacity, priority, "test", 1};
    void* queue = nullptr;
    method<CreateQueueFn>(factory(), ds::slot::kCreateQueue)(factory(), &desc, nullptr, &queue);
    return queue;
}

ds::Request request(std::uint64_t options, void* destination, std::size_t destination_size, const void* source,
                    std::uint64_t offset, std::size_t source_size) {
    ds::Request r{};
    r.options = options;
    r.destination = destination;
    r.destination_size = static_cast<std::uint32_t>(destination_size);
    r.source = source;
    r.offset = offset;
    r.source_size = static_cast<std::uint32_t>(source_size);
    return r;
}

void enqueue(void* queue, const ds::Request& r) {
    method<EnqueueRequestFn>(queue, ds::slot::kEnqueueRequest)(queue, &r);
}
void enqueue_status(void* queue, void* array, std::uint32_t index) {
    method<EnqueueStatusFn>(queue, ds::slot::kEnqueueStatus)(queue, array, index);
}
void submit(void* queue) { method<VoidFn>(queue, ds::slot::kSubmit)(queue); }
void close(void* queue) { method<VoidFn>(queue, ds::slot::kClose)(queue); }
ULONG release(void* object) { return method<RefFn>(object, ds::slot::kRelease)(object); }

HRESULT result(void* array, std::uint32_t index) {
    return method<ResultFn>(array, ds::slot::kGetResult)(array, index);
}

// Polls as the title does, then gives the entry's result.
HRESULT wait(void* array, std::uint32_t index) {
    const auto done = method<IsCompleteFn>(array, ds::slot::kIsComplete);
    const auto start = std::chrono::steady_clock::now();
    while (!done(array, index)) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
            std::printf("  status entry %u never completed\n", index);
            return ds::kPending;
        }
        Sleep(1);
    }
    return result(array, index);
}

std::vector<std::uint8_t> pattern(std::size_t n) {
    std::vector<std::uint8_t> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::uint8_t>(i * 7 + i / 251);
    return out;
}

void write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                               static_cast<std::streamsize>(bytes.size()));
}

// ---- A zlib stream (RFC 1950) of one fixed-Huffman deflate block (RFC 1951 3.2.6), with greedy LZ77 matches from a
// 3-byte hash: literals, lengths and distances, for the shim's inflate to undo.

class BitWriter {
public:
    void bits(std::uint32_t value, int count) {  // header fields and extra bits: least significant bit first
        for (int i = 0; i < count; ++i) put((value >> i) & 1u);
    }
    void code(std::uint32_t value, int length) {  // Huffman codes: most significant bit first
        for (int i = length - 1; i >= 0; --i) put((value >> i) & 1u);
    }
    void byte(std::uint8_t b) {
        used_ = 0;
        bytes.push_back(b);
    }
    std::vector<std::uint8_t> bytes;

private:
    void put(std::uint32_t bit) {
        if (used_ == 0) bytes.push_back(0);
        bytes.back() = static_cast<std::uint8_t>(bytes.back() | (bit << used_));
        used_ = (used_ + 1) & 7u;
    }
    unsigned used_ = 0;
};

void fixed_symbol(BitWriter& w, unsigned symbol) {
    if (symbol < 144) w.code(0x30 + symbol, 8);
    else if (symbol < 256) w.code(0x190 + symbol - 144, 9);
    else if (symbol < 280) w.code(symbol - 256, 7);
    else w.code(0xc0 + symbol - 280, 8);
}

void fixed_match(BitWriter& w, std::size_t length, std::size_t distance) {
    static constexpr std::uint16_t length_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr std::uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr std::uint16_t distance_base[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                                        33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                                        1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
    static constexpr std::uint8_t distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    int l = 28;
    while (length_base[l] > length) --l;
    fixed_symbol(w, 257 + static_cast<unsigned>(l));
    w.bits(static_cast<std::uint32_t>(length - length_base[l]), length_extra[l]);
    int d = 29;
    while (distance_base[d] > distance) --d;
    w.code(static_cast<std::uint32_t>(d), 5);
    w.bits(static_cast<std::uint32_t>(distance - distance_base[d]), distance_extra[d]);
}

std::vector<std::uint8_t> compress_zlib(const std::vector<std::uint8_t>& plain) {
    BitWriter w;
    w.byte(0x78);  // deflate, 32 KiB window
    w.byte(0x01);  // no dictionary; 0x7801 is a multiple of 31
    w.bits(1, 1);  // the last block
    w.bits(1, 2);  // fixed Huffman codes
    std::vector<std::int64_t> head(std::size_t(1) << 15, -1);
    std::size_t i = 0;
    while (i < plain.size()) {
        std::size_t best = 0, distance = 0;
        if (i + 3 <= plain.size()) {
            const std::size_t h = ((std::size_t(plain[i]) << 10) ^ (std::size_t(plain[i + 1]) << 5) ^ plain[i + 2]) &
                                  (head.size() - 1);
            const std::int64_t candidate = head[h];
            head[h] = static_cast<std::int64_t>(i);
            if (candidate >= 0 && i - static_cast<std::size_t>(candidate) <= 32768) {
                const std::size_t from = static_cast<std::size_t>(candidate);
                std::size_t n = 0;
                while (n < 258 && i + n < plain.size() && plain[from + n] == plain[i + n]) ++n;
                if (n >= 3) {
                    best = n;
                    distance = i - from;
                }
            }
        }
        if (best) {
            fixed_match(w, best, distance);
            i += best;
        } else {
            fixed_symbol(w, plain[i]);
            ++i;
        }
    }
    fixed_symbol(w, 256);
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t x : plain) {
        a = (a + x) % 65521;
        b = (b + a) % 65521;
    }
    const std::uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) w.byte(static_cast<std::uint8_t>(adler >> s));
    return w.bytes;
}

// ---- xmem's physical pages, as the title gets them

using XMemAllocatePhysicalPagesFn = BOOL(WINAPI*)(std::uint64_t, std::uint64_t*, std::uint64_t*);
using XMemFreePhysicalPagesFn = BOOL(WINAPI*)(std::uint64_t, const std::uint64_t*);
using XMemVirtualAllocFn = void*(WINAPI*)(void*, SIZE_T, DWORD, ULONGLONG, DWORD);
using XMemMapPhysicalPagesFn = void*(WINAPI*)(void*, std::uint64_t, const std::uint64_t*);
constexpr DWORD kXmem64kPages = 0x20400000;

struct Xmem {
    XMemAllocatePhysicalPagesFn allocate = nullptr;
    XMemFreePhysicalPagesFn free = nullptr;
    XMemVirtualAllocFn virtual_alloc = nullptr;
    XMemMapPhysicalPagesFn map = nullptr;
    bool ok() const { return allocate && free && virtual_alloc && map; }

    std::vector<std::uint64_t> pages(std::uint64_t n) const {
        std::vector<std::uint64_t> out(n);
        std::uint64_t given = n;
        if (!allocate(0, &given, out.data()) || given != n) out.clear();
        return out;
    }
    // A reserve-only window of 64 KiB pages with `pages` mapped in it, where the title would see them.
    std::uint8_t* window(const std::vector<std::uint64_t>& pages) const {
        void* const at = virtual_alloc(nullptr, pages.size() * ds::kPage, MEM_RESERVE | kXmem64kPages, 0x21,
                                       PAGE_READWRITE);
        if (!at || map(at, pages.size(), pages.data()) != at) return nullptr;
        return static_cast<std::uint8_t*>(at);
    }
    void unmap(std::uint8_t* at, std::size_t pages) const {
        map(at, pages, nullptr);
        VirtualFree(at, 0, MEM_RELEASE);
    }
};

Xmem xmem() {
    Xmem x;
    x.allocate = reinterpret_cast<XMemAllocatePhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemAllocatePhysicalPages"));
    x.free = reinterpret_cast<XMemFreePhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemFreePhysicalPages"));
    x.virtual_alloc = reinterpret_cast<XMemVirtualAllocFn>(rt::shims::find("xmem.dll", "XMemVirtualAlloc"));
    x.map = reinterpret_cast<XMemMapPhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemMapPhysicalPages"));
    return x;
}

// ---- The cases

void factory_and_its_slots() {
    const auto get = reinterpret_cast<GetFactoryFn>(rt::shims::find("xcrdapi.dll", "DStorageGetFactory"));
    check(get != nullptr, "the registry serves xcrdapi.dll!DStorageGetFactory");
    if (!get) return;
    void* out = nullptr;
    check_hr(get(&factory_iid(), &out), ds::kOk, "DStorageGetFactory with the factory's interface id");
    check(out != nullptr && out == factory(), "the factory is one static object");
    GUID other = factory_iid();
    other.Data1 ^= 1;
    void* none = nullptr;
    check_hr(get(&other, &none), ds::kNoInterface, "another interface id is E_NOINTERFACE");
    check(none == nullptr, "and nothing is written for it");
    check_hr(get(&factory_iid(), nullptr), ds::kInvalidArg, "a null out pointer is E_INVALIDARG");
    check(method<RefFn>(out, ds::slot::kAddRef)(out) == 1 && method<RefFn>(out, ds::slot::kRelease)(out) == 1,
          "the factory's AddRef and Release count nothing");
    check_hr(method<SetStagingFn>(out, ds::slot::kSetStagingBufferSize)(out, 32u << 20), ds::kOk,
             "a 32 MiB staging buffer is accepted (the title stops if not)");
    // A slot nobody has called answers E_NOTIMPL (and logs its offset once).
    check_hr(method<ResultFn>(out, 0x30)(out, 0), ds::kNotImpl, "an unknown factory slot is E_NOTIMPL");
}

void raw_and_zlib_reads_from_a_game_file(const std::filesystem::path& package) {
    const std::vector<std::uint8_t> plain = pattern(300000);
    const std::vector<std::uint8_t> packed = compress_zlib(plain);
    std::vector<std::uint8_t> contents = plain;
    contents.insert(contents.end(), packed.begin(), packed.end());
    write_file(package / "data" / "stream.bin", contents);

    void* missing = nullptr;
    check_hr(open(L"GAME:\\data\\absent.bin", &missing), static_cast<std::int32_t>(0x80070002u),
             "opening a missing GAME: file is HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)");
    void* file = nullptr;
    check_hr(open(L"GAME:\\data\\stream.bin", &file), ds::kOk, "a GAME: file opens through the file system shim");
    if (!file) return;

    void* const queue = new_queue(0);
    void* const array = status_array(1);
    check(queue && array, "a queue and a status array");
    std::vector<std::uint8_t> raw(1000), inflated(plain.size());
    enqueue(queue, request(0, raw.data(), raw.size(), file, 5000, raw.size()));
    enqueue(queue, request(ds::kZlib, inflated.data(), inflated.size(), file, plain.size(), packed.size()));
    enqueue_status(queue, array, 0);
    check_hr(result(array, 0), ds::kPending, "the status is pending until its requests are done");
    submit(queue);
    check_hr(wait(array, 0), ds::kOk, "both requests complete with S_OK");
    check(std::equal(raw.begin(), raw.end(), plain.begin() + 5000), "the raw read holds the file's bytes");
    check(inflated == plain, "the zlib request inflates the stream the test compressed");
    check(packed.size() < plain.size() / 4, "and the stream was compressed, not stored");
    close(queue);
    // Not the array: the worker drops its reference just after writing the entry, so it may still hold one here.
    check(release(queue) == 0 && release(file) == 0, "a queue and a file go with their last Release");
    release(array);
}

void memory_sources_inflate_too() {
    const std::vector<std::uint8_t> plain = pattern(70000);
    const std::vector<std::uint8_t> packed = compress_zlib(plain);
    void* const queue = new_queue(1);
    void* const array = status_array(1);
    std::vector<std::uint8_t> out(plain.size());
    enqueue(queue, request(ds::kZlib | ds::kMemorySource, out.data(), out.size(), packed.data(), 0, packed.size()));
    enqueue_status(queue, array, 0);
    submit(queue);
    check_hr(wait(array, 0), ds::kOk, "a zlib request from memory completes");
    check(out == plain, "and inflates into its destination");
    release(queue);
    release(array);
}

void statuses_complete_in_order_with_the_first_failure(const std::filesystem::path& package) {
    const std::vector<std::uint8_t> contents = pattern(4096);
    write_file(package / "small.bin", contents);
    void* file = nullptr;
    open(L"GAME:\\small.bin", &file);
    check(file != nullptr, "a second GAME: file opens");
    if (!file) return;
    void* const queue = new_queue(0);
    void* const array = status_array(5);
    // Requests in flight together run in any order, so none of them shares a destination (failed ones leave theirs
    // undefined).
    std::array<std::array<std::uint8_t, 16>, 9> buffers{};
    auto at = [&](std::size_t i) { return static_cast<void*>(buffers[i].data()); };

    // Segment 0: a read past the end, then a bad zlib stream; the first in queue order wins.
    enqueue(queue, request(0, at(0), 16, file, 4090, 16));
    enqueue(queue, request(ds::kZlib, at(1), 16, file, 0, 16));
    enqueue_status(queue, array, 0);
    // Segment 1: two reads.
    enqueue(queue, request(0, at(2), 16, file, 0, 16));
    enqueue(queue, request(0, at(3), 16, file, 100, 16));
    enqueue_status(queue, array, 1);
    submit(queue);
    // Segment 2: BCPack (BC7) from the file, and zlib then BCPack (BC4) from memory: no decoder is linked
    // (bcpack_decode_stub.cpp), so both fail as a stream the decoder rejects.
    enqueue(queue, request(7 << 1, at(4), 16, file, 0, 16));
    const std::vector<std::uint8_t> blocks = pattern(64);
    const std::vector<std::uint8_t> packed = compress_zlib(blocks);
    ds::Request two_stages = request(ds::kZlib | (4 << 1) | ds::kMemorySource, at(5), 16, packed.data(), 0,
                                     packed.size());
    two_stages.intermediate_size = static_cast<std::uint32_t>(blocks.size());
    enqueue(queue, two_stages);
    enqueue_status(queue, array, 2);
    // Segment 3: an option bit no title package uses (bit 5, a further transform).
    enqueue(queue, request(std::uint64_t{1} << 5, at(6), 16, file, 0, 16));
    enqueue_status(queue, array, 3);
    // Segment 4: a raw read larger than its destination.
    enqueue(queue, request(0, at(7), 8, file, 0, 16));
    enqueue_status(queue, array, 4);
    submit(queue);

    check_hr(wait(array, 4), ds::kInvalidArg, "a raw read larger than its destination is E_INVALIDARG");
    check_hr(result(array, 3), ds::kNotImpl, "an unsupported option is E_NOTIMPL");
    check_hr(result(array, 2), ds::kInvalidData,
             "a BCPack request the decoder refuses is HRESULT_FROM_WIN32(ERROR_INVALID_DATA)");
    check_hr(result(array, 0), ds::kEndOfFile,
             "the first failure in queue order: a read past the end is HRESULT_FROM_WIN32(ERROR_HANDLE_EOF)");
    check_hr(result(array, 1), ds::kOk, "the next segment's status is S_OK");
    check(std::equal(buffers[2].begin(), buffers[2].end(), contents.begin()) &&
              std::equal(buffers[3].begin(), buffers[3].end(), contents.begin() + 100),
          "the successful reads hold the file's bytes");
    check_hr(result(array, 5), ds::kInvalidArg, "an entry past the array's end is E_INVALIDARG");
    check(method<IsCompleteFn>(array, ds::slot::kIsComplete)(array, 5), "and counts as complete");
    release(queue);
    release(array);
    release(file);
}

void half_a_queue_submits_itself(const std::filesystem::path& package) {
    write_file(package / "tiny.bin", pattern(64));
    void* file = nullptr;
    open(L"GAME:\\tiny.bin", &file);
    void* const queue = new_queue(0, 4, 1);
    void* const array = status_array(1);
    std::array<std::uint8_t, 16> out{};
    enqueue(queue, request(0, out.data(), out.size(), file, 8, 16));
    enqueue_status(queue, array, 0);  // the second of four: no submit needed
    check_hr(wait(array, 0), ds::kOk, "half a queue's capacity enqueued submits without Submit");
    const std::vector<std::uint8_t> want = pattern(64);
    check(std::equal(out.begin(), out.end(), want.begin() + 8), "and the read lands");
    release(queue);
    release(array);
    if (file) release(file);
}

void a_closed_queue_completes_nothing(const std::filesystem::path& package) {
    write_file(package / "closed.bin", pattern(64));
    void* file = nullptr;
    open(L"GAME:\\closed.bin", &file);
    void* const queue = new_queue(0);
    void* const array = status_array(1);
    std::array<std::uint8_t, 16> out{};
    enqueue(queue, request(0, out.data(), out.size(), file, 0, 16));
    enqueue_status(queue, array, 0);
    close(queue);
    submit(queue);
    Sleep(50);
    check_hr(result(array, 0), ds::kPending, "a closed queue's status never completes");
    check(std::all_of(out.begin(), out.end(), [](std::uint8_t b) { return b == 0; }),
          "and its request is not run");
    release(queue);
    release(array);
    if (file) release(file);
}

// ---- The scheduler without workers: what `start` picks, and when statuses land.

void read(ds::detail::Lanes& l, std::uint64_t id, std::uint64_t options, std::uint32_t size) {
    ds::Request r{};
    r.options = options;
    r.destination_size = size;
    r.source_size = size;
    l.lanes.at(id).unsubmitted.emplace_back(std::in_place_type<ds::Request>, r);
}

void priorities_share_by_bytes_about_tenfold() {
    ds::detail::Lanes l;
    l.lanes.emplace(0, ds::detail::Lane(0, 4096));
    l.lanes.emplace(1, ds::detail::Lane(1, 4096));
    l.lanes.emplace(2, ds::detail::Lane(2, 4096));  // real-time: as high
    for (int i = 0; i < 400; ++i)
        for (std::uint64_t id = 0; id < 3; ++id) read(l, id, 0, 4096);
    for (std::uint64_t id = 0; id < 3; ++id) l.submit(id);
    std::array<int, 3> started{};
    for (int i = 0; i < 330; ++i) {
        const auto job = l.start();
        if (!job) break;
        ++started[job->lane];
        l.finish(*job, ds::kOk);
    }
    std::printf("  started per lane: normal %d, high %d, real-time %d\n", started[0], started[1], started[2]);
    check(started[1] + started[2] == 300 && started[0] == 30,
          "normal gets about a tenth of what high and real-time get together");
    check(std::abs(started[1] - started[2]) <= 1, "high and real-time share one class");
}

void staging_bounds_compressed_bytes_in_flight() {
    ds::detail::Lanes l;
    l.staging = 100;
    l.lanes.emplace(0, ds::detail::Lane(0, 64));
    read(l, 0, ds::kZlib, 60);
    read(l, 0, ds::kZlib, 60);
    read(l, 0, 0, 1000);  // raw: straight to its destination
    l.submit(0);
    const auto first = l.start();
    check(first.has_value(), "the first compressed read starts");
    check(!l.start().has_value() && l.starved, "60 + 60 > 100: the second waits for staging");
    l.finish(*first, ds::kOk);
    const auto second = l.start();
    const auto raw = l.start();
    check(second && raw && second->staging == 60 && raw->staging == 0, "then it starts, and the raw read beside it");
    // Alone, a request larger than the buffer still runs.
    read(l, 0, ds::kZlib, 500);
    l.submit(0);
    check(!l.start().has_value(), "a 500-byte source waits while 60 are staged");
    l.finish(*second, ds::kOk);
    check(l.start().has_value(), "and runs alone once nothing else is staged");
}

void a_status_waits_for_every_earlier_read() {
    ds::detail::Lanes l;
    l.lanes.emplace(0, ds::detail::Lane(0, 64));
    auto* const array = static_cast<ds::detail::StatusArray*>(status_array(2));
    for (std::uint32_t i = 0; i < 2; ++i) ds::detail::entry(array, i).store(ds::kPending);
    const auto status = [&](std::uint32_t index) {
        ds::detail::add_ref(array);
        l.lanes.at(0).unsubmitted.emplace_back(std::in_place_type<ds::detail::Status>, array, index);
    };
    read(l, 0, 0, 16);
    read(l, 0, 0, 16);
    status(0);
    read(l, 0, 0, 16);
    status(1);
    l.submit(0);
    std::vector<ds::detail::Job> jobs;
    for (int i = 0; i < 3; ++i)
        if (auto job = l.start()) jobs.push_back(*job);
    check(jobs.size() == 3, "three reads start at once");
    if (jobs.size() != 3) return;
    // The last read and the first finish; the second then fails.
    l.finish(jobs[2], ds::kOk);
    l.finish(jobs[0], ds::kOk);
    check(result(array, 0) == ds::kPending && result(array, 1) == ds::kPending,
          "no status lands while an earlier read runs");
    l.finish(jobs[1], ds::kInvalidData);
    check_hr(result(array, 0), ds::kInvalidData, "the first status gets the failure before it");
    check_hr(result(array, 1), ds::kOk, "the second, with none since, S_OK");
    release(array);
}

// ---- Destinations in physical pages

void physical_page_destinations(const Xmem& x) {
    std::vector<std::uint64_t> pages = x.pages(2);
    std::uint8_t* const mapped = pages.empty() ? nullptr : x.window(pages);
    check(mapped != nullptr, "two physical pages mapped where the title would see them");
    if (!mapped) return;
    const std::vector<std::uint8_t> plain = pattern(100);
    void* const queue = new_queue(1);
    void* const array = status_array(1);
    ds::Request r = request(ds::kPageDestination | ds::kMemorySource, pages.data(), plain.size(), plain.data(), 0,
                            plain.size());
    r.page_offset = ds::kPage - 40;  // across the page boundary
    enqueue(queue, r);
    enqueue_status(queue, array, 0);
    submit(queue);
    check_hr(wait(array, 0), ds::kOk, "a request into physical pages completes");
    check(std::memcmp(mapped + ds::kPage - 40, plain.data(), plain.size()) == 0,
          "and the title sees the bytes across the page boundary");
    release(queue);
    release(array);
    x.unmap(mapped, 2);
    x.free(pages.size(), pages.data());
}

void zlib_into_pages_in_place_and_across_allocations(const Xmem& x) {
    const std::vector<std::uint64_t> a = x.pages(3), b = x.pages(1);
    check(a.size() == 3 && b.size() == 1, "two allocations of physical pages");
    if (a.size() != 3 || b.size() != 1) return;
    const std::vector<std::uint8_t> plain = pattern(2 * ds::kPage + 100);
    const std::vector<std::uint8_t> packed = compress_zlib(plain);
    // One allocation's pages in order (written in place), then pages of two (through a buffer), both from 50 bytes
    // into the first.
    std::array<std::vector<std::uint64_t>, 2> destinations = {a, std::vector<std::uint64_t>{a[0], a[1], b[0]}};
    void* const queue = new_queue(1);
    void* const array = status_array(1);
    for (auto& pages : destinations) {
        ds::Request r = request(ds::kZlib | ds::kPageDestination | ds::kMemorySource, pages.data(), plain.size(),
                                packed.data(), 0, packed.size());
        r.page_offset = 50;
        enqueue(queue, r);
    }
    enqueue_status(queue, array, 0);
    submit(queue);
    check_hr(wait(array, 0), ds::kOk, "zlib into one allocation's pages and into pages of two completes");
    for (const auto& pages : destinations) {
        std::uint8_t* const seen = x.window(pages);
        check(seen && std::all_of(seen, seen + 50, [](std::uint8_t v) { return v == 0; }) &&
                  std::memcmp(seen + 50, plain.data(), plain.size()) == 0,
              "the inflated bytes are in the pages, from 50 bytes into the first");
        if (seen) x.unmap(seen, 3);
    }
    release(queue);
    release(array);
    x.free(a.size(), a.data());
    x.free(b.size(), b.data());
}

void a_page_array_filled_late_is_waited_for(const Xmem& x) {
    const std::vector<std::uint64_t> pages = x.pages(2);
    check(pages.size() == 2, "two pages for a late page array");
    if (pages.size() != 2) return;
    // The title fills the array after it enqueued the request; the request reads it when it runs.
    static std::atomic<std::uint64_t> late_array[2];
    late_array[0].store(pages[0]);
    late_array[1].store(0);
    static_assert(sizeof(std::atomic<std::uint64_t>) == sizeof(std::uint64_t));
    const std::vector<std::uint8_t> plain = pattern(2 * ds::kPage);
    void* const queue = new_queue(1);
    void* const array = status_array(1);
    enqueue(queue, request(ds::kPageDestination | ds::kMemorySource, late_array, plain.size(), plain.data(), 0,
                           plain.size()));
    enqueue_status(queue, array, 0);
    submit(queue);
    std::thread title([&] {
        Sleep(10);
        late_array[1].store(pages[1], std::memory_order_release);
    });
    title.join();
    check_hr(wait(array, 0), ds::kOk, "a request whose page array is filled after it was submitted completes");
    std::uint8_t* const seen = x.window(pages);
    check(seen && std::memcmp(seen, plain.data(), plain.size()) == 0, "and writes the pages the title put there");
    if (seen) x.unmap(seen, 2);
    release(queue);
    release(array);
    x.free(pages.size(), pages.data());
}

void an_unknown_page_fails_its_request() {
    // LEONIDA_DSTORAGE_RETRIES is 1 for this process (main), so the one retry takes 2 ms.
    std::uint64_t unknown[1] = {0xfffff0};
    const std::vector<std::uint8_t> plain = pattern(100);
    void* const queue = new_queue(1);
    void* const array = status_array(1);
    enqueue(queue, request(ds::kPageDestination | ds::kMemorySource, unknown, plain.size(), plain.data(), 0,
                           plain.size()));
    enqueue_status(queue, array, 0);
    submit(queue);
    check_hr(wait(array, 0), ds::kInvalidArg, "a destination page nobody allocated fails its request");
    release(queue);
    release(array);
}

// The read-beside-then-copy fix: a raw file read into title memory the kernel cannot write. Untouched physical pages
// are reserved until xmem's handler commits them on the first user-mode touch, which the kernel's write never makes.
void a_raw_read_into_untouched_pages(const Xmem& x, const std::filesystem::path& package) {
    const std::vector<std::uint8_t> contents = pattern(4096);
    write_file(package / "pages.bin", contents);
    std::vector<std::uint64_t> pages = x.pages(2);
    std::uint8_t* const mapped = pages.empty() ? nullptr : x.window(pages);
    check(mapped != nullptr, "two untouched physical pages mapped");
    if (!mapped) return;

    // What the kernel does with them (for the log; the host decides): ReadFile straight into the second page.
    const std::wstring host_path = (package / "pages.bin").wstring();
    const HANDLE h = CreateFileW(host_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD got = 0;
        const BOOL ok = ReadFile(h, mapped + ds::kPage, 512, &got, nullptr);
        std::printf("  note: ReadFile into an untouched mapped physical page %s (error %lu)\n",
                    ok ? "succeeded" : "failed", ok ? 0ul : GetLastError());
        CloseHandle(h);
    }

    void* file = nullptr;
    open(L"GAME:\\pages.bin", &file);
    void* const queue = new_queue(0);
    void* const array = status_array(1);
    enqueue(queue, request(0, mapped + 123, 1000, file, 7, 1000));
    enqueue_status(queue, array, 0);
    submit(queue);
    check_hr(wait(array, 0), ds::kOk, "a raw read into untouched physical pages completes");
    check(std::memcmp(mapped + 123, contents.data() + 7, 1000) == 0, "and the title sees the file's bytes");
    release(queue);
    release(array);
    if (file) release(file);
    x.unmap(mapped, 2);
    x.free(pages.size(), pages.data());
}

}  // namespace

int main() {
    // Read once per process by the shim, before the first request into physical pages.
    SetEnvironmentVariableW(L"LEONIDA_DSTORAGE_RETRIES", L"1");
    testfiles::ScratchDir scratch("leonida_dstorage_test");
    const std::filesystem::path package = scratch.path() / "package";
    std::filesystem::create_directories(package);
    rt::fs::configure(package, scratch.path() / "overlay");

    factory_and_its_slots();
    if (!factory()) {
        std::printf("FAIL no factory: %d of %d checks failed\n", g_failures + 1, g_checks + 1);
        return 1;
    }
    raw_and_zlib_reads_from_a_game_file(package);
    memory_sources_inflate_too();
    statuses_complete_in_order_with_the_first_failure(package);
    half_a_queue_submits_itself(package);
    a_closed_queue_completes_nothing(package);
    priorities_share_by_bytes_about_tenfold();
    staging_bounds_compressed_bytes_in_flight();
    a_status_waits_for_every_earlier_read();
    const Xmem x = xmem();
    check(x.ok(), "the registry serves the xmem.dll physical-page exports");
    if (x.ok()) {
        physical_page_destinations(x);
        zlib_into_pages_in_place_and_across_allocations(x);
        a_page_array_filled_late_is_waited_for(x);
        an_unknown_page_fails_its_request();
        a_raw_read_into_untouched_pages(x, package);
    }
    rt::fs::configure({}, {});
    std::printf("%d of %d checks failed\n", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
