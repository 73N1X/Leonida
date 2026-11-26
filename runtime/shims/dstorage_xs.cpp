// XCRDAPI.dll: the console's DirectStorage. Ported from VIctory's crates/victory-hle/src/modules/dstorage.rs
// (GPL-3.0-or-later; Marvisak and contributors, DexNom fork); Leonida is GPL-3.0-or-later too.
//
// A factory that opens files and creates request queues and status arrays the title polls. Its interfaces have grown
// since 2020 (new interface ids, new slots); the slots here are the ones the title calls, which VIctory read from its
// call sites (dstorage_xs.h has the layouts). A slot nobody has needed yet logs its offset once and returns E_NOTIMPL.
//
// Submitted requests of every queue run on one pool of workers, as the console's hardware runs them: in parallel,
// started in queue order, queues served by weighted round robin over their priorities (about 10x per level, counted in
// bytes), with at most the staging buffer's worth of compressed source in flight. Each request reads its source (a
// file or memory), inflates zlib data and/or unpacks BCPack data, and writes the destination (an address, or physical
// pages). Completion is what stays in order: a status entry is written once every request enqueued before it on its
// queue has finished, with the first failure (in queue order) since the previous status entry, or success.
//
// What is Leonida's here: files open through the CreateFileW a title's import binds to (shims/file_system.cpp), so
// GAME: paths read the overlay copy or the package file; zlib is leonida::zlib_inflate (src/shader_library.h); BCPack
// is leonida_bcpack_decode (src/bcpack_decode.h, a stub that fails until the decoder is ported); physical pages are
// written through xmem's own view of their section (rt::xmem::physical_span, write_physical).
//
// Diagnostics: LEONIDA_DSTORAGE_TRACE=1 prints a line for every request that fails, with its file; 2 for every request
// (two runs are compared by the list of their reads, by file and offset), plus the 5 s summaries the startup trace
// gets. LEONIDA_DSTORAGE_RETRIES (default 12) is how often a write into physical pages is tried again.
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "../src/bcpack_decode.h"
#include "../src/shader_library.h"
#include "dstorage_xs.h"
#include "shims.h"
#include "startup_trace.h"
#include "xmem.h"

namespace rt::dstorage {
namespace detail {

// One of our objects as title code calls it: the method table first. `area` names it in the log when the title calls
// a slot we lack.
struct Object {
    const void* const* vtable;
    const char* area;
};

// A status array: the entries the title polls, each the result of the requests before it (pending, the first
// failure, or success).
struct StatusArray {
    Object base;
    std::atomic<std::uint32_t> refs{1};
    std::uint32_t capacity = 0;
    std::unique_ptr<std::atomic<std::int32_t>[]> entries;
};

}  // namespace detail

namespace {

using detail::Command;
using detail::Job;
using detail::Lane;
using detail::Lanes;
using detail::Object;
using detail::Status;
using detail::StatusArray;
using Clock = std::chrono::steady_clock;

// ---- Logging

std::string text(const char* s) { return s ? std::string(s) : std::string(); }

std::string utf8(const wchar_t* w) {
    const int n = w ? WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr) : 0;
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string iid_text(const GUID* iid) {
    if (!iid) return "null";
    char b[40];
    std::snprintf(b, sizeof b, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", iid->Data1, iid->Data2, iid->Data3,
                  iid->Data4[0], iid->Data4[1], iid->Data4[2], iid->Data4[3], iid->Data4[4], iid->Data4[5],
                  iid->Data4[6], iid->Data4[7]);
    return b;
}

std::string hex32(std::int32_t hr) {
    char b[12];
    std::snprintf(b, sizeof b, "0x%08x", static_cast<unsigned>(hr));
    return b;
}

void trace(const char* api, const std::string& fields) {
    if (!rt::startup_trace::enabled()) return;
    rt::startup_trace::event("dstorage", std::string("\"api\":\"") + api + "\"" + fields);
}

// LEONIDA_DSTORAGE_TRACE: 0 or unset off; 1 a line for every request that fails, with its file; 2 for every request,
// and the 5 s summaries on stderr too. VIctory's VICTORY_WALLS_TRACE.
int request_trace_level() {
    static const int level = [] {
        char buf[8] = {};
        const DWORD n = GetEnvironmentVariableA("LEONIDA_DSTORAGE_TRACE", buf, sizeof buf);
        return n > 0 && n < sizeof buf ? std::atoi(buf) : 0;
    }();
    return level;
}

// HRESULT_FROM_WIN32 of a host error.
std::int32_t from_win32(DWORD error) { return static_cast<std::int32_t>(0x80070000u | (error & 0xffffu)); }

// ---- Objects

template <typename T>
ULONG __stdcall add_ref_instance(T* self) {
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

template <typename T>
ULONG __stdcall release(T* self) {
    const std::uint32_t left = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) delete self;
    return left;
}

// The factory is static: its AddRef and Release count nothing.
ULONG __stdcall static_add_ref(Object*) { return 1; }

// A slot nobody has needed yet: logged once per object kind and slot, with its byte offset. Wrong for a slot that
// returns bool or a value.
void note_missing(const char* area, unsigned offset) {
    static std::mutex mutex;
    static std::set<std::pair<std::string, unsigned>> seen;
    {
        const std::lock_guard lock(mutex);
        if (!seen.emplace(area, offset).second) return;
    }
    char b[24];
    std::snprintf(b, sizeof b, "0x%x", offset);
    std::fprintf(stderr, "[rt] dstorage: %s method at %s not implemented (E_NOTIMPL; logged once)\n", area, b);
    trace("missing", ",\"object\":" + rt::startup_trace::quote(area) + ",\"slot\":\"" + b + "\"");
}

template <unsigned Slot>
HRESULT __stdcall missing(const Object* self) {
    note_missing(self ? self->area : "?", Slot * 8);
    return kNotImpl;
}

template <typename F>
const void* method(F* f) {
    return reinterpret_cast<const void*>(f);
}

// ---- Files

struct File {
    Object base;
    std::atomic<std::uint32_t> refs{1};
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::string path;  // as the title named it, for the request trace
    ~File() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};

const void* const kFileVtable[8] = {
    method(&missing<0>),
    method(&add_ref_instance<File>),
    method(&release<File>),
    method(&missing<3>),
    method(&missing<4>),
    method(&missing<5>),
    method(&missing<6>),
    method(&missing<7>),
};

// ---- Status arrays

// The result of the requests before entry `index`: pending, the first failure, or success.
HRESULT __stdcall status_result(StatusArray* self, std::uint32_t index) {
    if (index >= self->capacity) return kInvalidArg;
    return self->entries[index].load(std::memory_order_acquire);
}

// The title polls this (entry 0, a Sleep(1) loop) and never reads the result itself.
bool __stdcall is_complete(StatusArray* self, std::uint32_t index) { return status_result(self, index) != kPending; }

const void* const kStatusVtable[5] = {
    method(&missing<0>),
    method(&add_ref_instance<StatusArray>),
    method(&release<StatusArray>),
    method(&is_complete),   // 0x18
    method(&status_result), // 0x20
};

// ---- The scheduler

// Every queue's requests, run by a pool of workers. Immortal: the workers wait on it until the process ends.
struct Scheduler {
    std::mutex mutex;
    Lanes lanes;
    // Requests to start, or staging space freed.
    std::condition_variable work;
    // A request finished (for full queues and close).
    std::condition_variable finished;
};

void work(Scheduler* scheduler, unsigned index);

// The scheduler, with its workers started on first use; null only if it could not be allocated.
Scheduler* scheduler() {
    static Scheduler* const instance = new (std::nothrow) Scheduler;
    static std::once_flag started;
    if (!instance) return nullptr;
    std::call_once(started, [] {
        unsigned workers = std::thread::hardware_concurrency();
        if (workers == 0) workers = 4;
        unsigned running = 0;
        for (unsigned i = 0; i < workers; ++i) {
            try {
                std::thread(work, instance, i).detach();
                ++running;
            } catch (...) {
            }
        }
        std::fprintf(stderr, "[rt] dstorage: %u workers\n", running);
        trace("workers", ",\"count\":" + std::to_string(running));
    });
    return instance;
}

// Ends a queue: what was not started is dropped, and this waits for the reads already running (they write title
// memory).
void close_lane(std::uint64_t id) {
    Scheduler* const s = scheduler();
    if (!s) return;
    std::unique_lock lock(s->mutex);
    const auto it = s->lanes.lanes.find(id);
    if (it == s->lanes.lanes.end()) return;
    Lane& lane = it->second;
    lane.closed = true;
    lane.unsubmitted.clear();
    lane.reads.clear();
    lane.statuses.clear();  // releases their arrays
    s->finished.wait(lock, [&] {
        const auto found = s->lanes.lanes.find(id);
        return found == s->lanes.lanes.end() || found->second.running.empty();
    });
    s->lanes.lanes.erase(id);
    s->finished.notify_all();
}

// A queue object: its lane in the scheduler. The last Release ends the lane, as close does.
struct Queue {
    Object base;
    std::atomic<std::uint32_t> refs{1};
    std::uint64_t id = 0;
    ~Queue() { close_lane(id); }
};

// Adds a command to the queue; blocks while the queue is full, and submits on its own once half of it is enqueued, as
// the console does.
void queue(Queue* self, Command command) {
    Scheduler* const s = scheduler();
    if (!s) return;
    std::unique_lock lock(s->mutex);
    for (;;) {
        const auto it = s->lanes.lanes.find(self->id);
        if (it == s->lanes.lanes.end() || it->second.closed) return;  // closed: dropped (a status releases its array)
        // One slot stays free, as the console's count of empty slots says.
        if (it->second.entries() + 1 < (std::max<std::size_t>)(it->second.capacity, 2)) break;
        s->finished.wait(lock);
    }
    Lane& lane = s->lanes.lanes.at(self->id);
    lane.unsubmitted.push_back(std::move(command));
    if (lane.unsubmitted.size() >= (std::max<std::size_t>)(lane.capacity / 2, 1)) {
        s->lanes.submit(self->id);
        s->work.notify_all();
    }
}

void __stdcall enqueue_request(Queue* self, const Request* request) {
    if (!request) return;
    try {
        queue(self, Command(std::in_place_type<Request>, *request));
    } catch (...) {
        std::fprintf(stderr, "[rt] dstorage: a request could not be enqueued (out of memory); it never completes\n");
    }
}

void __stdcall enqueue_status(Queue* self, StatusArray* array, std::uint32_t index) {
    if (!array) return;
    if (index >= array->capacity) {
        std::fprintf(stderr, "[rt] dstorage: status entry %u out of range (the array holds %u)\n", index,
                     array->capacity);
        return;
    }
    array->entries[index].store(kPending, std::memory_order_release);
    detail::add_ref(array);
    try {
        queue(self, Command(std::in_place_type<Status>, array, index));
    } catch (...) {
        std::fprintf(stderr, "[rt] dstorage: a status entry could not be enqueued (out of memory); it stays pending\n");
    }
}

void __stdcall submit(Queue* self) {
    Scheduler* const s = scheduler();
    if (!s) return;
    const std::lock_guard lock(s->mutex);
    s->lanes.submit(self->id);
    s->work.notify_all();
}

// Ends the queue whatever its reference count: what was not started never completes, and this returns once the reads
// already running have finished. The title calls this, not Release.
void __stdcall close(Queue* self) { close_lane(self->id); }

// Slots by the 2020 order, which the argument shapes at the title's call sites match (0x18, 0x20, 0x30 and 0x40 are
// called).
const void* const kQueueVtable[12] = {
    method(&missing<0>),
    method(&add_ref_instance<Queue>),
    method(&release<Queue>),
    method(&enqueue_request),  // 0x18
    method(&enqueue_status),   // 0x20
    method(&missing<5>),       // 0x28 (2020: fence signal)
    method(&submit),           // 0x30
    method(&missing<7>),       // 0x38 (2020: cancel by tag)
    method(&close),            // 0x40
    method(&missing<9>),       // 0x48 (2020: error event)
    method(&missing<10>),      // 0x50 (2020: error record)
    method(&missing<11>),      // 0x58 (2020: queue info)
};

// ---- Running a request

// A buffer for one stage of a request, kept for the worker's next request (up to 8 MiB) so it is not allocated and
// cleared again each time.
class Scratch {
public:
    std::uint8_t* get(std::size_t n) {
        if (n > size_) {
            data_.reset();
            size_ = 0;
            data_.reset(new std::uint8_t[n]);
            size_ = n;
        }
        return data_.get();
    }
    void trim() {
        if (size_ > kKeep) {
            data_.reset();
            size_ = 0;
        }
    }

private:
    static constexpr std::size_t kKeep = std::size_t(8) << 20;
    std::unique_ptr<std::uint8_t[]> data_;
    std::size_t size_ = 0;
};

// A worker's buffers, owned by its loop and handed down. Not thread_local: a thread_local with a destructor makes
// gta6rt.exe's CRT register its thread-local destructor callback with the UCRT at startup
// (_register_thread_local_exe_atexit_callback), which takes one per process, and the title's CRT startup then
// registering its own (through the host's UCRT) ends the process with 0xC0000409, right after the title's static
// initializers (measured on the retail executable).
struct Buffers {
    Scratch source;  // a compressed source read from its file
    Scratch bounce;  // a raw read, before it is copied in
    Scratch pages;   // a destination in physical pages that are not one allocation's run
    std::vector<std::uint8_t> inflated;

    void trim() {
        source.trim();
        bounce.trim();
        pages.trim();
        if (inflated.capacity() > (std::size_t(8) << 20)) std::vector<std::uint8_t>().swap(inflated);
    }
};

// Logs a failed request (the first 64, then at powers of two) and gives its result back.
std::int32_t fail(const Request& request, std::int32_t hr, const char* why) {
    static std::atomic<std::uint64_t> failures{0};
    const std::uint64_t n = failures.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 64 || (n & (n - 1)) == 0) {
        std::fprintf(stderr,
                     "[rt] dstorage: request failed: %s (options 0x%llx, offset %llu, source_size %u, "
                     "destination_size %u, intermediate_size %u, tag %llu, name \"%s\", hr %s)%s\n",
                     why, static_cast<unsigned long long>(request.options),
                     static_cast<unsigned long long>(request.offset), request.source_size, request.destination_size,
                     request.intermediate_size, static_cast<unsigned long long>(request.tag),
                     text(request.name).c_str(), hex32(hr).c_str(),
                     n > 64 ? " [later failures are logged at powers of two]" : "");
    }
    if (n <= 256)
        trace("request_failed", ",\"why\":" + rt::startup_trace::quote(why) + ",\"options\":\"" +
                                    rt::startup_trace::hex(request.options) + "\",\"offset\":" +
                                    std::to_string(request.offset) + ",\"hr\":\"" + hex32(hr) + "\"");
    return hr;
}

// A BCPack stage that failed: logged once per BC format. With the stub decoder every BCPack request comes here.
std::int32_t bcpack_failed(const Request& request, unsigned bc) {
    static std::atomic<std::uint32_t> seen{0};
    const std::uint32_t bit = 1u << (bc & 15);
    if ((seen.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) {
        std::fprintf(stderr,
                     "[rt] dstorage: BCPack (BC%u) requests fail with %s: no BCPack decoder in this build, or a stream "
                     "it rejects (first: offset %llu, source_size %u, destination_size %u; logged once per format)\n",
                     bc, hex32(kInvalidData).c_str(), static_cast<unsigned long long>(request.offset),
                     request.source_size, request.destination_size);
        trace("bcpack_failed", ",\"format\":" + std::to_string(bc));
    }
    return kInvalidData;
}

// Nanoseconds workers spent reading files, for the summary.
std::atomic<std::uint64_t> g_reading{0};

// Fills `out` (the source size) from the request's file or memory.
std::int32_t read_into(const Request& request, std::span<std::uint8_t> out) {
    if (request.options & kMemorySource) {
        // The title's source holds out.size() bytes until completion.
        if (!out.empty()) std::memcpy(out.data(), request.source, out.size());
        return kOk;
    }
    const auto* file = static_cast<const File*>(request.source);
    if (file->base.vtable != kFileVtable) return kInvalidArg;  // not a file this factory opened
    const auto started = Clock::now();
    std::int32_t result = kOk;
    std::size_t done = 0;
    while (done < out.size()) {
        const std::uint64_t at = request.offset + done;
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(at);
        position.OffsetHigh = static_cast<DWORD>(at >> 32);
        const DWORD want = static_cast<DWORD>((std::min<std::size_t>)(out.size() - done, std::size_t(1) << 30));
        DWORD got = 0;
        if (!ReadFile(file->handle, out.data() + done, want, &got, &position)) {
            const DWORD error = GetLastError();
            result = error == ERROR_HANDLE_EOF ? kEndOfFile : from_win32(error);
            break;
        }
        if (got == 0) {
            result = kEndOfFile;
            break;
        }
        done += got;
    }
    g_reading.fetch_add(
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count()),
        std::memory_order_relaxed);
    return result;
}

// A compressed request's source: the title's memory, or read from the file into the worker's buffer.
std::int32_t read_source(const Request& request, Buffers& buffers, std::span<const std::uint8_t>& source) {
    const std::size_t size = request.source_size;
    if (request.options & kMemorySource) {
        // The title's source holds `size` bytes until completion.
        source = {static_cast<const std::uint8_t*>(request.source), size};
        return kOk;
    }
    std::uint8_t* const bytes = buffers.source.get(size);
    const std::int32_t hr = read_into(request, {bytes, size});
    source = {bytes, size};
    return hr;
}

// Inflates a zlib stream into the worker's buffer, at most `limit` bytes. A stream that ends short of the limit
// passes: the title's chunks fill their destination exactly, and nothing says that is required. A valid stream that is
// longer gives its first `limit` bytes; a malformed or truncated stream, or a wrong Adler-32, fails with
// ERROR_INVALID_DATA.
std::int32_t inflate(std::span<const std::uint8_t> source, std::size_t limit, std::vector<std::uint8_t>& out) {
    if (out.capacity() < limit) out.reserve(limit);
    if (leonida::zlib_inflate(source, out, limit)) return kOk;
    // A whole, valid stream that inflates past the destination: the title asks for its first `limit` bytes (a
    // 1731-byte stream of 16384 bytes into an 8192-byte destination, the request at offset 1413311488 of
    // core_restofworld after the start screen), and gets them. Inflated whole, up to 64 times the limit, and cut.
    if (leonida::zlib_inflate(source, out, (std::min)(limit * 64, std::size_t(256) << 20)) && out.size() > limit) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true))
            std::fprintf(stderr, "[rt] dstorage: a zlib stream inflates past its destination (%zu of %zu bytes); "
                                 "its first bytes are given (logged once)\n", limit, out.size());
        out.resize(limit);
        return kOk;
    }
    // LEONIDA_DSTORAGE_DUMP=<folder> (diagnostic): the first 16 sources that do not inflate are written there, to
    // tell a stream this inflater misreads from one the title's request misdescribes. They are the title's bytes:
    // the folder stays on the player's machine.
    static const std::string folder = [] {
        char buf[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableA("LEONIDA_DSTORAGE_DUMP", buf, sizeof buf);
        return n && n < sizeof buf ? std::string(buf) : std::string();
    }();
    static std::atomic<unsigned> dumped{0};
    if (!folder.empty()) {
        const unsigned n = dumped.fetch_add(1, std::memory_order_relaxed);
        if (n < 16) {
            const std::string path = folder + "\\zlib-" + std::to_string(n) + "-limit" + std::to_string(limit) + ".bin";
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(source.data(), 1, source.size(), f);
                std::fclose(f);
            }
        }
    }
    return kInvalidData;
}

// The page numbers of a destination. The title fills the array after it enqueued the request (a copy taken then holds
// zeros, or the numbers of the array's last use), so it is read when the request runs; a number that is still zero
// then is waited for, a tenth of a second at most.
std::vector<std::uint64_t> filled(const std::uint64_t* array, std::size_t n) {
    static std::atomic<std::uint64_t> late{0};
    const auto read = [&] {
        // A page number per 64 KiB of destination; the title keeps the array until the request completes and may
        // write it from another thread meanwhile.
        std::vector<std::uint64_t> pages(n);
        const volatile std::uint64_t* const numbers = array;
        for (std::size_t i = 0; i < n; ++i) pages[i] = numbers[i];
        return pages;
    };
    const auto has_zero = [](const std::vector<std::uint64_t>& pages) {
        return std::find(pages.begin(), pages.end(), std::uint64_t{0}) != pages.end();
    };
    std::vector<std::uint64_t> pages = read();
    if (!has_zero(pages)) return pages;
    const auto began = Clock::now();
    while (has_zero(pages) && Clock::now() - began < std::chrono::milliseconds(100)) {
        Sleep(2);
        pages = read();
    }
    const std::uint64_t count = late.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 8 || (count & (count - 1)) == 0) {
        std::fprintf(stderr,
                     "[rt] dstorage: a request ran before its page array was filled (requests %llu, pages %zu, "
                     "waited %lld ms, filled %s)\n",
                     static_cast<unsigned long long>(count), n,
                     static_cast<long long>(
                         std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count()),
                     has_zero(pages) ? "no" : "yes");
    }
    return pages;
}

// LEONIDA_DSTORAGE_RETRIES: how often a write into physical pages is tried again before its request fails.
unsigned page_retries() {
    static const unsigned retries = [] {
        char buf[16] = {};
        const DWORD n = GetEnvironmentVariableA("LEONIDA_DSTORAGE_RETRIES", buf, sizeof buf);
        if (n == 0 || n >= sizeof buf) return 12u;
        char* end = nullptr;
        const unsigned long value = std::strtoul(buf, &end, 10);
        return end && *end == '\0' && end != buf ? static_cast<unsigned>(value) : 12u;
    }();
    return retries;
}

// Runs `fill(out, produced)` on the destination's first `len` bytes; `fill` says how many it produced. Destinations
// are title memory at the title's addresses (a GPU address is the CPU address), so an address is written in place;
// physical pages are too when they are consecutive pages of one allocation, and otherwise go through a buffer.
template <typename Fill>
std::int32_t into_destination(const Request& request, std::size_t len, Scratch& buffer, Fill&& fill) {
    if (!(request.options & kPageDestination)) {
        // The title's destination holds destination_size bytes, at least `len`, and nothing else writes it until
        // completion. VIctory opens its GPU side's guards over it first and afterwards publishes the bytes produced;
        // Leonida's D3D12 layer reads title memory where it lies, so there is nobody to tell. What stays is the check
        // that the producer kept within the destination.
        std::size_t produced = 0;
        const std::int32_t hr = fill(std::span<std::uint8_t>(static_cast<std::uint8_t*>(request.destination), len),
                                     produced);
        if (hr != kOk) return hr;
        return produced <= len ? kOk : kInvalidData;
    }
    const std::size_t offset = static_cast<std::size_t>(request.page_offset & 0xffff);
    const std::vector<std::uint64_t> pages =
        filled(static_cast<const std::uint64_t*>(request.destination), (offset + len + kPage - 1) / kPage);
    // Consecutive pages of one allocation (the usual case): written in place through the allocation's view.
    rt::xmem::PhysicalSpan span;
    if (rt::xmem::physical_span(pages.data(), pages.size(), offset, len, span)) {
        std::size_t produced = 0;
        return fill(std::span<std::uint8_t>(span.data, len), produced);
    }
    std::uint8_t* const data = buffer.get(len);
    std::size_t n = 0;
    const std::int32_t hr = fill(std::span<std::uint8_t>(data, len), n);
    if (hr != kOk) return hr;
    n = (std::min)(n, len);
    // A page the title has just made, or a commit the host refuses for a moment (commit short while a load maps many
    // at once), can come right a little later: tried again before the request fails (LEONIDA_DSTORAGE_RETRIES,
    // default 12, about 1.5 s in all).
    bool written = rt::xmem::write_physical(pages.data(), pages.size(), offset, data, n);
    unsigned tries = 0;
    while (!written && tries < page_retries()) {
        Sleep((std::min)(2u << (std::min)(tries, 6u), 128u));
        ++tries;
        written = rt::xmem::write_physical(pages.data(), pages.size(), offset, data, n);
    }
    if (written) {
        if (tries > 0)
            std::fprintf(stderr, "[rt] dstorage: a destination's physical pages written after %u retries (offset %zu, "
                                 "len %zu)\n", tries, offset, len);
        return kOk;
    }
    const DWORD last_error = GetLastError();
    const bool known = rt::xmem::physical_pages_known(pages.data(), pages.size(), offset, len);
    // Said aloud: the title never reads a request's result (it only polls for completion), so a destination left
    // unwritten is a model or texture that silently holds whatever its pages held before.
    std::fprintf(stderr,
                 "[rt] dstorage: a destination's physical pages could not be written (%s, last error %lu); its request "
                 "fails: offset %zu, len %zu, pages %zu, first 0x%llx, options 0x%llx\n",
                 known ? "a host failure" : "an unknown page", last_error, offset, len, pages.size(),
                 static_cast<unsigned long long>(pages.empty() ? 0 : pages.front()),
                 static_cast<unsigned long long>(request.options));
    return kInvalidArg;
}

// Reads, inflates and/or unpacks, and writes one request.
std::int32_t execute(const Request& request, Buffers& buffers) {
    const std::uint64_t options = request.options;
    if (options & ~(kZlib | kBcpack | kPageDestination | kMemorySource))
        return fail(request, kNotImpl, "unsupported options");
    if (!request.source || !request.destination) return fail(request, kInvalidArg, "no source or destination");
    const unsigned bc = static_cast<unsigned>((options & kBcpack) >> 1);
    const std::size_t size = request.destination_size;
    if (!(options & (kZlib | kBcpack))) {
        if (request.source_size > request.destination_size)
            return fail(request, kInvalidArg, "source larger than destination");
        const std::size_t n = request.source_size;
        // A file read into a title address is the kernel's write, and the kernel does not fault into a user-mode
        // handler. VIctory met it with its GPU side's guard: a block guarded again between kernel_will_write and the
        // read (the guard over all the views of a page protects other views from other threads) made the read fail
        // with ERROR_INVALID_USER_BUFFER, and the title stops at the failed request (runs amd-C and amd-C2, seconds
        // after the world's first frame). Here the same holds for xmem's physical pages, which are committed when
        // first touched, by a vectored handler: a ReadFile into an untouched mapped page fails with ERROR_NOACCESS
        // (Windows 11 26200; dstorage_xs_test prints what the host does). So it is read beside and copied in: our copy
        // faults into the handler like the title's own writes. Physical-page destinations are committed before they
        // are written, and memory sources are copied anyway.
        const bool bounce = !(options & (kPageDestination | kMemorySource));
        const std::int32_t hr = into_destination(request, n, buffers.pages, [&](std::span<std::uint8_t> out,
                                                                              std::size_t& produced) -> std::int32_t {
            produced = n;
            if (!bounce) return read_into(request, out.first(n));
            std::uint8_t* const data = buffers.bounce.get(n);
            const std::int32_t read = read_into(request, {data, n});
            if (read != kOk) return read;
            if (n) std::memcpy(out.data(), data, n);
            return kOk;
        });
        return hr == kOk ? kOk : fail(request, hr, "copying");
    }
    std::span<const std::uint8_t> source;
    if (const std::int32_t hr = read_source(request, buffers, source); hr != kOk)
        return fail(request, hr, "reading the source");
    std::vector<std::uint8_t>& inflated = buffers.inflated;
    // The console's order: zlib first, then BCPack.
    if (bc != 0 && (options & kZlib)) {
        if (const std::int32_t hr = inflate(source, request.intermediate_size, inflated); hr != kOk)
            return fail(request, hr, "zlib");
        source = inflated;
    }
    return into_destination(request, size, buffers.pages,
                            [&](std::span<std::uint8_t> out, std::size_t& produced) -> std::int32_t {
        if (bc != 0) {
            if (!leonida_bcpack_decode(bc, source.data(), source.size(), out.data(), out.size()))
                return bcpack_failed(request, bc);
            produced = out.size();
            return kOk;
        }
        if (const std::int32_t hr = inflate(source, out.size(), inflated); hr != kOk) return fail(request, hr, "zlib");
        if (!inflated.empty()) std::memcpy(out.data(), inflated.data(), inflated.size());
        produced = inflated.size();
        return kOk;
    });
}

// LEONIDA_DSTORAGE_TRACE's line for a request that is done.
void trace_request(const Request& request, std::int32_t result, Clock::time_point started) {
    const int level = request_trace_level();
    if (level == 0 || (level == 1 && result == kOk)) return;
    std::string file = "memory";
    if (!(request.options & kMemorySource)) {
        const auto* f = static_cast<const File*>(request.source);
        file = f && f->base.vtable == kFileVtable ? f->path : std::string("?");
    }
    std::fprintf(stderr,
                 "[rt] dstorage: request done: file %s, offset %llu, source_size %u, destination_size %u, to_pages %d, "
                 "options 0x%llx, %lld ms, ok %d, hr %s\n",
                 file.c_str(), static_cast<unsigned long long>(request.offset), request.source_size,
                 request.destination_size, (request.options & kPageDestination) ? 1 : 0,
                 static_cast<unsigned long long>(request.options),
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()),
                 result == kOk ? 1 : 0, hex32(result).c_str());
}

void name_thread(unsigned index) {
    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const auto set = reinterpret_cast<SetThreadDescriptionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (!set) return;
    wchar_t name[32];
    std::swprintf(name, 32, L"dstorage %u", index);
    set(GetCurrentThread(), name);
}

// A worker: runs the next read of the most deserving queue.
void work(Scheduler* s, unsigned index) {
    name_thread(index);
    Buffers buffers;
    std::unique_lock lock(s->mutex);
    for (;;) {
        std::optional<Job> job = s->lanes.start();
        if (!job) {
            s->work.wait(lock);
            continue;
        }
        lock.unlock();
        const auto started = Clock::now();
        std::int32_t result;
        try {
            result = execute(job->request, buffers);
        } catch (...) {
            result = fail(job->request, kOutOfMemory, "out of memory");
        }
        trace_request(job->request, result, started);
        buffers.trim();
        lock.lock();
        s->lanes.finish(*job, result);
        s->lanes.count(*job, started);
        if (std::exchange(s->lanes.starved, false)) s->work.notify_all();
        s->finished.notify_all();
    }
}

// ---- The factory

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

// The CreateFileW a mapped title's import binds to (shims/file_system.cpp): GAME: (and G:, T:) paths open the
// overlay copy or the package file, as the title's own opens do, and anything else is a host path. A build without
// that shim opens host paths only.
CreateFileWFn title_create_file() {
    static const CreateFileWFn fn = [] {
        const auto shim = reinterpret_cast<CreateFileWFn>(rt::shims::find("kernel32.dll", "CreateFileW"));
        return shim ? shim : &::CreateFileW;
    }();
    return fn;
}

// Opens `path` for reading; the file object owns the handle.
HRESULT __stdcall open_file(Object*, const wchar_t* path, const GUID* iid, void** out) {
    if (!path) return kInvalidArg;
    try {
        const HANDLE handle =
            title_create_file()(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        std::string name = utf8(path);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            std::fprintf(stderr, "[rt] dstorage: opening a file failed: %s (error %lu)\n", name.c_str(), error);
            trace("open_failed", ",\"path\":" + rt::startup_trace::quote(name) + ",\"error\":" + std::to_string(error));
            return from_win32(error);
        }
        std::fprintf(stderr, "[rt] dstorage: file opened: %s (iid %s)\n", name.c_str(), iid_text(iid).c_str());
        trace("file_opened", ",\"path\":" + rt::startup_trace::quote(name));
        auto* file = new (std::nothrow) File;
        if (!file) {
            CloseHandle(handle);
            return kOutOfMemory;
        }
        file->base = {kFileVtable, "dstorage file"};
        file->handle = handle;
        file->path = std::move(name);
        if (!out) {
            release(file);
            return kInvalidArg;
        }
        *out = file;
        return kOk;
    } catch (...) {
        return kOutOfMemory;
    }
}

// (capacity, name, iid, out). Entries start complete (S_OK).
HRESULT __stdcall create_status_array(Object*, std::uint32_t capacity, const char* name, const GUID* iid, void** out) {
    try {
        std::fprintf(stderr, "[rt] dstorage: status array: capacity %u, name \"%s\", iid %s\n", capacity,
                     text(name).c_str(), iid_text(iid).c_str());
        trace("status_array", ",\"capacity\":" + std::to_string(capacity) + ",\"name\":" +
                                  rt::startup_trace::quote(text(name)));
        auto array = std::make_unique<StatusArray>();
        array->base = {kStatusVtable, "dstorage status array"};
        array->capacity = capacity;
        array->entries.reset(new std::atomic<std::int32_t>[capacity ? capacity : 1]);
        for (std::uint32_t i = 0; i < capacity; ++i) array->entries[i].store(kOk, std::memory_order_relaxed);
        if (!out) return kInvalidArg;
        *out = array.release();
        return kOk;
    } catch (...) {
        return kOutOfMemory;
    }
}

HRESULT __stdcall create_queue(Object*, const QueueDesc* desc, const GUID* iid, void** out) {
    if (!desc) return kInvalidArg;
    try {
        std::fprintf(stderr,
                     "[rt] dstorage: queue: source %llu, capacity %u, priority %d, name \"%s\", unknown %llu, iid %s\n",
                     static_cast<unsigned long long>(desc->source_type), desc->capacity, desc->priority,
                     text(desc->name).c_str(), static_cast<unsigned long long>(desc->unknown), iid_text(iid).c_str());
        trace("queue", ",\"source\":" + std::to_string(desc->source_type) + ",\"capacity\":" +
                           std::to_string(desc->capacity) + ",\"priority\":" + std::to_string(desc->priority) +
                           ",\"name\":" + rt::startup_trace::quote(text(desc->name)));
        Scheduler* const s = scheduler();
        if (!s) return kOutOfMemory;
        auto queue = std::make_unique<Queue>();  // its destructor ends the lane
        queue->base = {kQueueVtable, "dstorage queue"};
        {
            const std::lock_guard lock(s->mutex);
            queue->id = s->lanes.next_id++;
            s->lanes.lanes.emplace(queue->id, Lane(desc->priority, desc->capacity));
        }
        if (!out) return kInvalidArg;
        *out = queue.release();
        return kOk;
    } catch (...) {
        return kOutOfMemory;
    }
}

// The title asks for 32 MiB and stops if this fails.
HRESULT __stdcall set_staging_buffer_size(Object*, std::uint32_t size) {
    std::fprintf(stderr, "[rt] dstorage: staging buffer size %u\n", size);
    trace("staging_buffer_size", ",\"size\":" + std::to_string(size));
    if (Scheduler* const s = scheduler()) {
        const std::lock_guard lock(s->mutex);
        s->lanes.staging = size;
    }
    return kOk;
}

const void* const kFactoryVtable[16] = {
    method(&missing<0>),
    method(&static_add_ref),
    method(&static_add_ref),
    method(&missing<3>),                // 0x18 (2020: queue creation)
    method(&open_file),                 // 0x20
    method(&create_status_array),       // 0x28
    method(&missing<6>),                // 0x30 (2020: CPU affinity)
    method(&missing<7>),                // 0x38 (2020: debug flags)
    method(&set_staging_buffer_size),   // 0x40
    method(&missing<9>),
    method(&create_queue),              // 0x50
    method(&missing<11>),
    method(&missing<12>),
    method(&missing<13>),
    method(&missing<14>),
    method(&missing<15>),
};

Object g_factory = {kFactoryVtable, "dstorage factory"};

HRESULT __stdcall DStorageGetFactory(const GUID* iid, void** out) {
    if (iid && std::memcmp(iid, &kFactoryIid, sizeof(GUID)) == 0) {
        if (!out) return kInvalidArg;
        *out = &g_factory;
        static std::atomic<bool> told{false};
        if (!told.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr, "[rt] dstorage: DStorageGetFactory: the factory\n");
            trace("DStorageGetFactory", "");
        }
        return kOk;
    }
    std::fprintf(stderr, "[rt] dstorage: DStorageGetFactory: unknown interface %s (E_NOINTERFACE)\n",
                 iid_text(iid).c_str());
    trace("DStorageGetFactory", ",\"unknown_iid\":" + rt::startup_trace::quote(iid_text(iid)));
    return kNoInterface;
}

static_assert(sizeof(GUID) == sizeof(Guid));

}  // namespace

// ---- The scheduler's parts (dstorage_xs.h)

namespace detail {

void add_ref(StatusArray* array) { add_ref_instance(array); }
void release(StatusArray* array) { rt::dstorage::release(array); }
std::atomic<std::int32_t>& entry(StatusArray* array, std::uint32_t index) { return array->entries[index]; }

Lane::Lane(std::int8_t priority, std::uint16_t lane_capacity)
    : cls(static_cast<std::size_t>(std::clamp<int>(priority, -1, 1) + 1)), capacity(lane_capacity) {}

std::size_t Lane::entries() const { return unsubmitted.size() + reads.size() + statuses.size() + running.size(); }

void Lane::flush() {
    while (!statuses.empty()) {
        const std::uint64_t seq = statuses.front().first;
        if (!reads.empty() && reads.front().first < seq) return;
        if (!running.empty() && *running.begin() < seq) return;
        // The first failure (in queue order) since the previous status entry, or success.
        const auto later = failures.lower_bound(seq);
        const std::int32_t first = later == failures.begin() ? kOk : failures.begin()->second;
        failures.erase(failures.begin(), later);
        statuses.front().second.entry().store(first, std::memory_order_release);
        statuses.pop_front();  // releases the array
    }
}

std::uint64_t staging_bytes(const Request& request) {
    const bool buffered = (request.options & (kZlib | kBcpack | kPageDestination)) != 0;
    return buffered && !(request.options & kMemorySource) ? request.source_size : 0;
}

std::optional<Job> Lanes::start() {
    // The class with the least virtual time, then its lane that has started the fewest bytes.
    std::optional<std::pair<std::uint64_t, std::size_t>> best_class;
    for (const auto& [id, lane] : lanes) {
        if (lane.reads.empty()) continue;
        const std::pair<std::uint64_t, std::size_t> key{clock[lane.cls], lane.cls};
        if (!best_class || key < *best_class) best_class = key;
    }
    if (!best_class) return std::nullopt;
    const std::size_t cls = best_class->second;
    Lane* lane = nullptr;
    std::uint64_t lane_id = 0;
    std::tuple<std::uint64_t, std::uint64_t, std::uint64_t> best{};
    for (auto& [id, candidate] : lanes) {
        if (candidate.reads.empty() || candidate.cls != cls) continue;
        const std::tuple<std::uint64_t, std::uint64_t, std::uint64_t> key{candidate.clock,
                                                                          candidate.reads.front().first, id};
        if (!lane || key < best) {
            best = key;
            lane = &candidate;
            lane_id = id;
        }
    }
    if (!lane) return std::nullopt;
    const Request request = lane->reads.front().second;
    const std::uint64_t staging_needed = staging_bytes(request);
    if (staged > 0 && staged + staging_needed > staging) {
        starved = true;
        ++refused;
        return std::nullopt;
    }
    const Job job{lane_id, lane->reads.front().first, request, staging_needed};
    lane->reads.pop_front();
    lane->running.insert(job.seq);
    staged += staging_needed;
    const std::uint64_t size =
        (std::max<std::uint64_t>)((std::max)(request.destination_size, request.source_size), 1);
    now = clock[cls];
    clock[cls] += size * kCost[cls];
    lane_now[cls] = lane->clock;
    lane->clock += size;
    return job;
}

void Lanes::finish(const Job& job, std::int32_t result) {
    staged -= job.staging;
    const auto it = lanes.find(job.lane);
    if (it == lanes.end()) return;
    it->second.running.erase(job.seq);
    if (result != kOk) it->second.failures.emplace(job.seq, result);
    it->second.flush();
}

void Lanes::submit(std::uint64_t id) {
    const auto it = lanes.find(id);
    if (it == lanes.end()) return;
    const std::size_t cls = it->second.cls;
    // A class that had nothing waiting starts from now, not from the credit it left behind.
    const bool waiting = std::any_of(lanes.begin(), lanes.end(), [&](const auto& entry) {
        return entry.second.cls == cls && !entry.second.reads.empty();
    });
    if (!waiting) clock[cls] = (std::max)(clock[cls], now);
    Lane& lane = it->second;
    if (lane.reads.empty()) lane.clock = (std::max)(lane.clock, lane_now[cls]);
    std::vector<Command> commands = std::move(lane.unsubmitted);
    lane.unsubmitted.clear();
    for (Command& command : commands) {
        const std::uint64_t seq = next_seq++;
        if (const Request* request = std::get_if<Request>(&command)) {
            lane.reads.emplace_back(seq, *request);
        } else {
            lane.statuses.emplace_back(seq, std::move(std::get<Status>(command)));
        }
    }
    lane.flush();
}

void Lanes::count(const Job& job, Clock::time_point started) {
    const auto spent = Clock::now() - started;
    ++done_requests;
    done_bytes += job.request.destination_size;
    done_busy += spent;
    if (job.request.options & kBcpack) {
        bcpack_bytes += job.request.destination_size;
        bcpack_busy += spent;
    }
    if (!since) since = started;
    const double elapsed = std::chrono::duration<double>(Clock::now() - *since).count();
    if (elapsed < 5.0) return;
    std::size_t waiting = 0;
    for (const auto& [id, lane] : lanes) waiting += lane.reads.size();
    const double reading = static_cast<double>(g_reading.exchange(0, std::memory_order_relaxed)) / 1e9;
    char line[320];
    std::snprintf(line, sizeof line,
                  "\"seconds\":%.1f,\"requests\":%llu,\"mb_per_s\":%.1f,\"busy_workers\":%.2f,\"reading_workers\":%.2f,"
                  "\"staging_waits\":%llu,\"bcpack_mb\":%llu,\"bcpack_workers\":%.2f,\"waiting\":%zu",
                  elapsed, static_cast<unsigned long long>(done_requests),
                  static_cast<double>(done_bytes) / elapsed / 1e6,
                  std::chrono::duration<double>(done_busy).count() / elapsed, reading / elapsed,
                  static_cast<unsigned long long>(refused), static_cast<unsigned long long>(bcpack_bytes / 1000000),
                  std::chrono::duration<double>(bcpack_busy).count() / elapsed, waiting);
    if (rt::startup_trace::enabled()) rt::startup_trace::event("dstorage_summary", line);
    if (request_trace_level() >= 2) std::fprintf(stderr, "[rt] dstorage: summary {%s}\n", line);
    done_requests = done_bytes = refused = bcpack_bytes = 0;
    done_busy = bcpack_busy = {};
    since.reset();
}

}  // namespace detail
}  // namespace rt::dstorage

SHIM_TABLE(dstorage_xs,
    {"xcrdapi.dll", "DStorageGetFactory", reinterpret_cast<void*>(&rt::dstorage::DStorageGetFactory)},
);
