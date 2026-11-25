// The console's DirectStorage (XCRDAPI.dll) as the title calls it: the structures it builds, the interface id it asks
// for, the method slots it calls, and the scheduler behind the queues. The shim is shims/dstorage_xs.cpp.
//
// Ported from VIctory's crates/victory-hle/src/modules/dstorage.rs (GPL-3.0-or-later; Marvisak and contributors,
// DexNom fork); Leonida is GPL-3.0-or-later too. The layouts and slots are VIctory's, read from the title's call sites.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <variant>
#include <vector>

namespace rt::dstorage {

// ---- What the title passes

struct Guid {
    std::uint32_t data1;
    std::uint16_t data2;
    std::uint16_t data3;
    std::uint8_t data4[8];
};

// The factory interface the title asks DStorageGetFactory for; any other id is E_NOINTERFACE.
inline constexpr Guid kFactoryIid = {0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};

// The queue's description: as in 2020 (source type, capacity, priority, name), plus a field the title sets to 1.
struct QueueDesc {
    std::uint64_t source_type;
    std::uint16_t capacity;
    std::int8_t priority;  // -1 low, 0 normal, 1 high, 2 real-time
    const char* name;
    std::uint64_t unknown;
};
static_assert(sizeof(QueueDesc) == 0x20 && offsetof(QueueDesc, name) == 0x10 && offsetof(QueueDesc, unknown) == 0x18);

// A read request as the title builds it, 0x48 bytes.
struct Request {
    std::uint64_t options;
    // An address, or with kPageDestination an array of 64 KiB page numbers (one per page the bytes touch).
    void* destination;
    // With kPageDestination, the low 16 bits are where in the first page the destination starts.
    std::uint64_t page_offset;
    std::uint32_t destination_size;
    // Our file object, or with kMemorySource an address.
    const void* source;
    std::uint64_t offset;
    std::uint32_t source_size;
    // The size between two decompression stages (zlib, then BCPack).
    std::uint32_t intermediate_size;
    // Matched by cancellation; the title leaves it 0.
    std::uint64_t tag;
    const char* name;
};
static_assert(sizeof(Request) == 0x48);
static_assert(offsetof(Request, destination_size) == 0x18 && offsetof(Request, source) == 0x20 &&
              offsetof(Request, source_size) == 0x30 && offsetof(Request, intermediate_size) == 0x34 &&
              offsetof(Request, name) == 0x40);

// Request options.
inline constexpr std::uint64_t kZlib = 1;                // inflate zlib (RFC 1950) data
inline constexpr std::uint64_t kBcpack = 0xfull << 1;    // bits 1-4: unpack BCPack data of this BC format (0: none)
inline constexpr std::uint64_t kPageDestination = 1ull << 21;
inline constexpr std::uint64_t kMemorySource = 1ull << 22;
// Bits 5-20 select a further transform, bit 23 a source in physical pages; the title's packages need neither, so a
// request with them fails with E_NOTIMPL.

inline constexpr std::size_t kPage = 64 * 1024;

// Results, as HRESULT bits.
inline constexpr std::int32_t kOk = 0;
inline constexpr std::int32_t kPending = static_cast<std::int32_t>(0x8000000Au);      // E_PENDING
inline constexpr std::int32_t kNotImpl = static_cast<std::int32_t>(0x80004001u);      // E_NOTIMPL
inline constexpr std::int32_t kNoInterface = static_cast<std::int32_t>(0x80004002u);  // E_NOINTERFACE
inline constexpr std::int32_t kInvalidArg = static_cast<std::int32_t>(0x80070057u);   // E_INVALIDARG
inline constexpr std::int32_t kOutOfMemory = static_cast<std::int32_t>(0x8007000Eu);  // E_OUTOFMEMORY
// HRESULT_FROM_WIN32(ERROR_HANDLE_EOF): the source ends early.
inline constexpr std::int32_t kEndOfFile = static_cast<std::int32_t>(0x80070026u);
// HRESULT_FROM_WIN32(ERROR_INVALID_DATA): a bad compressed stream (zlib or BCPack).
inline constexpr std::int32_t kInvalidData = static_cast<std::int32_t>(0x8007000Du);

// The slots the title calls, as byte offsets into the method tables (QueryInterface, AddRef and Release first). The
// newer interfaces moved them from their 2020 places; slots in between that nobody has called log their offset.
namespace slot {
inline constexpr std::size_t kAddRef = 0x08;
inline constexpr std::size_t kRelease = 0x10;
// The factory: (path, iid, out), (capacity, name, iid, out), (bytes), (desc, iid, out).
inline constexpr std::size_t kOpenFile = 0x20;
inline constexpr std::size_t kCreateStatusArray = 0x28;
inline constexpr std::size_t kSetStagingBufferSize = 0x40;
inline constexpr std::size_t kCreateQueue = 0x50;
// A queue: (request), (array, index), (), ().
inline constexpr std::size_t kEnqueueRequest = 0x18;
inline constexpr std::size_t kEnqueueStatus = 0x20;
inline constexpr std::size_t kSubmit = 0x30;
inline constexpr std::size_t kClose = 0x40;
// A status array: bool (index), HRESULT (index).
inline constexpr std::size_t kIsComplete = 0x18;
inline constexpr std::size_t kGetResult = 0x20;
}  // namespace slot

// ---- The scheduler (exposed for its test; everything below runs under the scheduler's lock)
//
// Submitted requests of every queue run on one pool of workers, as the console's hardware runs them: in parallel,
// started in queue order, queues served by weighted round robin over their priorities (about 10x per level, counted
// in bytes), with at most the staging buffer's worth of compressed source in flight. Completion is what stays in
// order: a status entry is written once every request enqueued before it on its queue has finished, with the first
// failure (in queue order) since the previous status entry, or success.
namespace detail {

struct StatusArray;  // dstorage_xs.cpp
void add_ref(StatusArray* array);
void release(StatusArray* array);
std::atomic<std::int32_t>& entry(StatusArray* array, std::uint32_t index);

// A status entry to write, holding a reference on its array (taken over from the caller).
class Status {
public:
    Status(StatusArray* array, std::uint32_t index) : array_(array), index_(index) {}
    Status(Status&& other) noexcept : array_(std::exchange(other.array_, nullptr)), index_(other.index_) {}
    Status& operator=(Status&& other) noexcept {
        if (this != &other) {
            reset();
            array_ = std::exchange(other.array_, nullptr);
            index_ = other.index_;
        }
        return *this;
    }
    Status(const Status&) = delete;
    Status& operator=(const Status&) = delete;
    ~Status() { reset(); }

    // The index was checked when the status was enqueued.
    std::atomic<std::int32_t>& entry() const { return detail::entry(array_, index_); }

private:
    void reset() {
        if (array_) release(std::exchange(array_, nullptr));
    }
    StatusArray* array_;
    std::uint32_t index_;
};

using Command = std::variant<Request, Status>;

// Priority weights per class (low, normal, high and real-time), as the console's: about 10x per level, counted
// against request sizes.
inline constexpr std::array<std::uint64_t, 3> kCost = {100, 10, 1};
// The console's default staging buffer.
inline constexpr std::uint64_t kStaging = 32ull << 20;

// A queue's requests.
struct Lane {
    Lane(std::int8_t priority, std::uint16_t capacity);

    std::size_t cls;  // 0 low, 1 normal, 2 high and real-time
    std::size_t capacity;
    // Bytes started, for round robin among the lanes of a class.
    std::uint64_t clock = 0;
    // Enqueued, not yet submitted.
    std::vector<Command> unsubmitted;
    // Submitted, by sequence number: reads not started, status entries not written, reads running, and failures not
    // yet reported.
    std::deque<std::pair<std::uint64_t, Request>> reads;
    std::deque<std::pair<std::uint64_t, Status>> statuses;
    std::set<std::uint64_t> running;
    std::map<std::uint64_t, std::int32_t> failures;
    bool closed = false;

    // Requests and status entries held, submitted or not.
    std::size_t entries() const;
    // Writes the status entries whose earlier reads have all finished.
    void flush();
};

// A read a worker runs.
struct Job {
    std::uint64_t lane;
    std::uint64_t seq;
    Request request;
    std::uint64_t staging;
};

struct Lanes {
    std::map<std::uint64_t, Lane> lanes;
    std::uint64_t next_id = 0;
    // Submission order over all queues.
    std::uint64_t next_seq = 0;
    // Bytes of staging held by running requests, and the limit.
    std::uint64_t staged = 0;
    std::uint64_t staging = kStaging;
    // A start waited for staging space since the last finish.
    bool starved = false;
    // Weighted round robin: each class's virtual time (bytes x cost), and the time of the last start; within a class,
    // the lane clock of the last start.
    std::array<std::uint64_t, 3> clock{};
    std::uint64_t now = 0;
    std::array<std::uint64_t, 3> lane_now{};
    // Since the last summary: requests, bytes written and time spent running them, starts that waited for staging,
    // and the BCPack share.
    std::uint64_t done_requests = 0;
    std::uint64_t done_bytes = 0;
    std::chrono::steady_clock::duration done_busy{};
    std::uint64_t refused = 0;
    std::uint64_t bcpack_bytes = 0;
    std::chrono::steady_clock::duration bcpack_busy{};
    std::optional<std::chrono::steady_clock::time_point> since;

    // Starts the next read of the class with the least virtual time, from its lane that has started the fewest bytes,
    // if the staging buffer has room for it (or nothing else holds any).
    std::optional<Job> start();
    // A read is done: kOk or its failure.
    void finish(const Job& job, std::int32_t result);
    // Moves a queue's enqueued commands to the workers.
    void submit(std::uint64_t id);
    // Counts a finished request; every 5 s a summary goes to the startup trace.
    void count(const Job& job, std::chrono::steady_clock::time_point started);
};

// Staging bytes a request holds while it runs: what it buffers between the file and the destination. Raw reads go
// straight to their destination, memory sources need none (as on the console).
std::uint64_t staging_bytes(const Request& request);

}  // namespace detail
}  // namespace rt::dstorage
