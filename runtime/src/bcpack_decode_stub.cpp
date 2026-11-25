// No BCPack decoder yet: the stand-in for leonida_bcpack_decode (bcpack_decode.h) until the decoder is ported. Part of
// the port of VIctory's crates/victory-hle/src/modules/dstorage.rs (GPL-3.0-or-later; Marvisak and contributors,
// DexNom fork); Leonida is GPL-3.0-or-later too.
//
// Every call fails, so a DirectStorage request with a BCPack stage completes with an error status, which the shim logs
// once per BC format, instead of leaving whatever the destination held. The decoder's own file takes this one's place
// in the gta6rt target.
#include "bcpack_decode.h"

bool leonida_bcpack_decode(std::uint32_t, const std::uint8_t*, std::size_t, std::uint8_t*, std::size_t) {
    return false;
}
