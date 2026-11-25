// The BCPack entry point DirectStorage calls. Defined in bcpack.cpp, a port of VIctory's victory-formats BCPack
// decoder (GPL-3.0-or-later, Marvisak and contributors); Leonida is GPL-3.0-or-later too.
//
// format: the BC number of the request (DSTORAGE_BCPACK_MODE, options bits 1-4): 1 and 3-7; BC2 is not supported.
// dst_size: the decoded size, a whole number of blocks. Bytes after the last one the stream needs are ignored.
// Returns false when the format tables are missing or refused (see bcpack.h), for an unsupported format, a bad size
// or a stream that ends early or is corrupt. Thread-safe.
#pragma once

#include <cstddef>
#include <cstdint>

bool leonida_bcpack_decode(std::uint32_t format, const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst,
                           std::size_t dst_size);
