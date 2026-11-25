// The BCPack stage of a DirectStorage request: the one call the DirectStorage shim (shims/dstorage_xs.cpp) makes for
// it. That shim is ported from VIctory's crates/victory-hle/src/modules/dstorage.rs (GPL-3.0-or-later; Marvisak and
// contributors, DexNom fork), which unpacks BCPack with victory-formats' decoder; Leonida is GPL-3.0-or-later too.
//
// BCPack is the Xbox Series' packed form of BCn texture blocks, which the console's DirectStorage hardware unpacks
// while it reads. The decoder is ported on its own (docs/port_plan.md, step 1); until it lands, gta6rt links
// bcpack_decode_stub.cpp, whose answer is always false.
#pragma once

#include <cstddef>
#include <cstdint>

// Unpacks `src_size` bytes of BCPack data of BC format `format` (a request's option bits 1-4: 1 BC1 to 7 BC7, as
// DSTORAGE_BCPACK_MODE numbers them) into exactly `dst_size` bytes of BCn blocks at `dst`. False when the format is
// not supported, the stream is malformed or does not fill `dst_size`, or no decoder is linked; `dst` is then
// undefined, and the request completes with HRESULT_FROM_WIN32(ERROR_INVALID_DATA).
bool leonida_bcpack_decode(std::uint32_t format, const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst,
                           std::size_t dst_size);
