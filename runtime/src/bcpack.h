// BCPack: the Xbox Series DirectStorage codec that packs BC1-BC7 block streams losslessly. Decode only; BC2 is left
// out (no title data uses it).
//
// Ported from VIctory's victory-formats (victory/crates/victory-formats/src/bcpack.rs, GPL-3.0-or-later, Marvisak
// and contributors), and the table loading and request handling around bcpack::decode in
// victory/crates/victory-hle/src/modules/dstorage.rs. Leonida is GPL-3.0-or-later too. The output is bit-exact with
// the Rust decoder; where the C++ differs it says so. Format facts cross-checked with Leonida's own research:
// docs/textures.md (chunk entries, the codec field as DSTORAGE_BCPACK_MODE, lane widths) and tools/bcpack.py.
//
// A packed stream has no header. It is 12 or 16 interleaved rANS lanes; each lane is a run of fragments (header byte,
// length, payload) coded with one of 128 static models, and a fixed schedule decides which lane takes the next input
// byte. The lanes' symbols form one record per block: two back references (to one of the last 256 blocks, or to a
// per-format dictionary that stands in for blocks -1 to -255) and the residuals of the endpoints and indices against
// them. The models, the index rank tables, the BC7 control codes and the dictionaries are constants of the format.
// They are not part of Leonida's (or VIctory's) license and are never committed: the runtime reads them from a local
// file at run time (process_tables) and refuses any file whose SHA-256 is not the pinned one.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace rt::bcpack {

// Why a call failed: a static message. nullptr means it succeeded.
using Error = const char*;

// The table file: its size, the SHA-256 VIctory's Initialize-FormatTables.ps1 pins, its name beside the executable
// and the environment variable that names another path.
constexpr std::size_t kTablesSize = 77982;
constexpr const char* kTablesSha256 = "1F1D4760419202BB5672F0C428CE51E456B5D08FBAEC8F371D043FD94E01C36B";
constexpr const char* kTablesFile = "bcpack-tables.bin";
constexpr const char* kTablesVariable = "LEONIDA_BCPACK_TABLES";

// The parsed tables (about 140 KB). Immutable once parsed, so any number of threads may decode with one.
struct Tables;
struct TablesDeleter {
    void operator()(const Tables* tables) const;
};
using TablesPtr = std::unique_ptr<const Tables, TablesDeleter>;

// Parses table bytes without the hash check (tests build tables of their own): 128 x 9 u16 model frequencies, the
// rank tables, 512 u16 BC7 control words and 255 dictionary entries per format (the layout is in bcpack.cpp).
// Every model must tile 65536 slots. nullptr, with *error set, when they do not parse.
TablesPtr parse_tables(const std::uint8_t* bytes, std::size_t size, Error* error = nullptr);

// Reads a table file and refuses one of another size or SHA-256 than kTablesSha256. nullptr, with *error set to the
// reason, when it cannot be read or is refused.
TablesPtr load_tables(const std::filesystem::path& path, std::string* error = nullptr);

// Where the process takes its tables from: the path in LEONIDA_BCPACK_TABLES when it is set (a relative path is
// taken from the current directory when the tables are first needed), else bcpack-tables.bin next to the running
// executable. Empty when neither can be found out.
std::filesystem::path tables_path();

// The process's tables, loaded from tables_path() on first use (thread-safe) and kept for the life of the process.
// nullptr when they are missing or refused; the reason is printed once on stderr.
const Tables* process_tables();

// Unpacks `src` (BC number `bc`, 1 and 3-7) into `dst`, whose size gives the block count (8-byte blocks for BC1 and
// BC4, 16-byte blocks otherwise). The stream carries no size: bytes after the last one the lanes need are ignored, a
// stream that runs out is an error. No allocation; about 30 KB of stack. On failure `dst` may hold some decoded
// blocks (VIctory's decoder writes none when the lanes fail; the result is the same either way).
Error decode(unsigned bc, const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst, std::size_t dst_size,
             const Tables& tables);

// A format's lane layout: per lane its symbol bits - 1 at the full alphabet, the number of lanes, the symbols each
// lane gives per block (a record is lanes * per symbols, lane j's at j * per) and the block size in bytes.
struct Layout {
    const std::uint8_t* widths;
    unsigned lanes;
    unsigned per;
    unsigned block_bytes;
};
bool layout(unsigned bc, Layout* out);

// The lane layer alone, for tests and tools: decodes `blocks` records of the format's layout into `records`
// (blocks * lanes * per bytes), taking each input byte from fetch(context, lane), which returns the byte or -1 when
// the input has ended. decode() is this followed by the block rebuild.
Error unpack_records(unsigned bc, const Tables& tables, std::size_t blocks, int (*fetch)(void* context, unsigned lane),
                     void* context, std::uint8_t* records);

// SHA-256 of `size` bytes (FIPS 180-4), for the table check.
void sha256(const void* data, std::size_t size, std::uint8_t digest[32]);

}  // namespace rt::bcpack
