// mfplat.dll: Media Foundation is the host's, except the source resolver, which opens local files only. See
// media_foundation.cpp. These are its test seams.
#pragma once
#include <string>

namespace rt::media {

// Where a resolver may read `url` from. True with `out` the path or URL to hand the host resolver: a console path
// (GAME:, G:, T:) or a file: URL naming one becomes its host file, other local files pass as they are. False for
// anything that is not a local file (a network scheme, a UNC share, a file: URL with a host), with the scheme
// ("unc" for a share) and host named for the log.
bool local_url(const std::wstring& url, std::wstring& out, std::string& scheme, std::string& host);

// Wraps a host IMFSourceResolver (one reference taken over) in the guarding proxy, as MFCreateSourceResolver does.
long wrap_resolver(void* host_resolver, void** out);

}  // namespace rt::media
