// mfplat.dll: Media Foundation is the host's (the title's other mfplat imports bind to it), except the source
// resolver. The host's resolver would fetch http, rtsp and other URLs over the network; the title gets a proxy that
// opens local files only, with console paths (GAME:, G:, T:) rewritten to their host files, and forwards everything
// else to the host's resolver.
//
// Ported from VIctory (crates/victory-hle/src/modules/media.rs), GPL-3.0-or-later, by Marvisak and contributors
// (DexNom fork). Leonida is GPL-3.0-or-later. VIctory's URL rule, slot order and refusal (MF_E_UNSUPPORTED_SCHEME,
// logged once per scheme and host) are kept. Differences: console paths are Leonida's (rt::fs::read_path, which
// knows GAME: as well as the G: and T: drive letters) rather than VIctory's virtual drives, and a GAME: path is a
// local path, not a URL scheme. QueryInterface answers only the resolver's own interfaces, as in VIctory: anything
// else could hand out the host object unguarded.
//
// Not covered, as in VIctory: CreateObjectFromByteStream and its async pair pass through (the URL is only a hint for
// picking a handler; the title's own byte stream holds the data), and so does a title that loads mfplat.dll at run
// time and calls the host's MFCreateSourceResolver through a pointer it got elsewhere than GetProcAddress
// (library_loader's GetProcAddress serves this shim's name for the host module too).
#include "media_foundation.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "file_system.h"
#include "shims.h"

namespace rt::media {
namespace {

constexpr HRESULT kUnsupportedScheme = static_cast<HRESULT>(0xC00D36C3u);  // MF_E_UNSUPPORTED_SCHEME

constexpr GUID kIUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIResolver = {0xFBE5A32D, 0xA497, 0x4B61, {0xBB, 0x85, 0x97, 0xB1, 0xA8, 0x48, 0xA6, 0xE3}};

bool is_alpha(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }
bool is_alnum(wchar_t c) { return is_alpha(c) || (c >= L'0' && c <= L'9'); }
wchar_t lower(wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c; }

bool starts_with(const std::wstring& s, const wchar_t* prefix) { return s.rfind(prefix, 0) == 0; }

bool drive(const std::wstring& s) { return s.size() >= 2 && is_alpha(s[0]) && s[1] == L':'; }

// "GAME:" in any case: a console path, not a URL scheme.
bool console(const std::wstring& s) {
    static const wchar_t kGame[] = L"game:";
    if (s.size() < 5) return false;
    for (size_t i = 0; i < 5; ++i)
        if (lower(s[i]) != kGame[i]) return false;
    return true;
}

// UTF-16 to UTF-8 and back, by hand so this stays portable; unpaired surrogates become U+FFFD.
std::string utf8(const std::wstring& w) {
    std::string out;
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t c = static_cast<uint16_t>(w[i]);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF)
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<uint16_t>(w[++i]) - 0xDC00);
        else if (c >= 0xD800 && c <= 0xDFFF)
            c = 0xFFFD;
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::wstring utf16(const std::string& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size();) {
        const auto b = static_cast<uint8_t>(s[i]);
        const size_t n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
        uint32_t c = n == 1 ? b : n == 2 ? (b & 0x1F) : n == 3 ? (b & 0x0F) : (b & 0x07);
        bool ok = n != 0 && i + n <= s.size();
        for (size_t k = 1; ok && k < n; ++k) {
            const auto t = static_cast<uint8_t>(s[i + k]);
            ok = (t >> 6) == 2;
            c = (c << 6) | (t & 0x3F);
        }
        if (!ok) {
            out += L'\xFFFD';
            ++i;
            continue;
        }
        i += n;
        if (c >= 0x10000) {
            c -= 0x10000;
            out += static_cast<wchar_t>(0xD800 + (c >> 10));
            out += static_cast<wchar_t>(0xDC00 + (c & 0x3FF));
        } else {
            out += static_cast<wchar_t>(c);
        }
    }
    return out;
}

// %XX escapes decoded as UTF-8 bytes; a % without two hex digits stays as it is.
std::wstring percent_decode(const std::wstring& s) {
    const std::string bytes = utf8(s);
    std::string out;
    auto hex = [](char c) -> int {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '%' && i + 2 < bytes.size() && hex(bytes[i + 1]) >= 0 && hex(bytes[i + 2]) >= 0) {
            out += static_cast<char>(hex(bytes[i + 1]) * 16 + hex(bytes[i + 2]));
            i += 2;
        } else {
            out += bytes[i];
        }
    }
    return utf16(out);
}

std::string host_of(const std::wstring& rest) {
    const size_t end = rest.find_first_of(L"/\\");
    return utf8(rest.substr(0, end));
}

// A local path to hand the host: a console path becomes its host file; anything else, or a refused console path
// (which the host then fails to open), passes as it is.
std::wstring translated(const std::wstring& path) {
    std::filesystem::path host;
    if (rt::fs::read_path(path, host)) return host.wstring();
    return path;
}

}  // namespace

bool local_url(const std::wstring& url, std::wstring& out, std::string& scheme, std::string& host) {
    std::wstring path;
    if (starts_with(url, L"\\\\?\\")) {
        std::wstring rest = url.substr(4);
        if (!drive(rest)) {
            if (starts_with(rest, L"UNC\\")) rest = rest.substr(4);
            scheme = "unc";
            host = host_of(rest);
            return false;
        }
        path = url;
    } else if (starts_with(url, L"\\\\") || starts_with(url, L"//")) {
        scheme = "unc";
        host = host_of(url.substr(2));
        return false;
    } else if (drive(url) || console(url)) {
        path = url;
    } else if (const size_t colon = url.find(L':'); colon != std::wstring::npos && colon > 1 &&
               [&] {
                   for (size_t i = 0; i < colon; ++i)
                       if (!is_alnum(url[i]) && url[i] != L'+' && url[i] != L'-' && url[i] != L'.') return false;
                   return true;
               }()) {
        std::wstring s;
        for (size_t i = 0; i < colon; ++i) s += lower(url[i]);
        const std::wstring rest = url.substr(colon + 1);
        const bool authority = starts_with(rest, L"//");
        if (s != L"file") {
            scheme = utf8(s);
            host = authority ? host_of(rest.substr(2)) : std::string();
            return false;
        }
        std::wstring after = rest;
        if (authority) {
            const std::wstring a = rest.substr(2);
            const size_t end = a.find_first_of(L"/\\");
            const std::wstring name = a.substr(0, end);
            std::wstring lowered;
            for (wchar_t c : name) lowered += lower(c);
            if (!name.empty() && lowered != L"localhost") {
                scheme = "file";
                host = utf8(name);
                return false;
            }
            after = end == std::wstring::npos ? std::wstring() : a.substr(end);
        }
        const size_t first = after.find_first_not_of(L"/\\");
        path = percent_decode(first == std::wstring::npos ? std::wstring() : after.substr(first));
        for (wchar_t& c : path)
            if (c == L'/') c = L'\\';
        if (!drive(path) && !console(path)) {
            out = url;  // a local file without a drive: the host resolves it as is
            return true;
        }
    } else {
        out = url;  // a relative path
        return true;
    }
    out = translated(path);
    return true;
}

namespace {

// The host's IMFSourceResolver behind the proxy, called through its method table.
template <class F>
F method(void* host, size_t slot) {
    return reinterpret_cast<F>((*static_cast<void***>(host))[slot]);
}

struct Resolver {
    const void* const* vtable;
    void* host;
    std::atomic<ULONG> refs{1};
};

void* host_resolver(void* self) { return static_cast<Resolver*>(self)->host; }

// The URL to give the host, or the refusal (logged once per scheme and host).
HRESULT checked(const wchar_t* url, std::wstring& out) {
    if (!url) return E_POINTER;
    std::string scheme, host;
    if (local_url(url, out, scheme, host)) return S_OK;
    static std::mutex mu;
    static std::set<std::pair<std::string, std::string>> refused;
    std::lock_guard lock(mu);
    if (refused.emplace(scheme, host).second)
        std::fprintf(stderr, "[rt] media: refused a URL that is not a local file (scheme %s, host %s)\n",
                     scheme.c_str(), host.empty() ? "none" : host.c_str());
    return kUnsupportedScheme;
}

ULONG STDMETHODCALLTYPE add_ref(void* self) { return ++static_cast<Resolver*>(self)->refs; }

ULONG STDMETHODCALLTYPE release(void* self) {
    auto* r = static_cast<Resolver*>(self);
    const ULONG left = --r->refs;
    if (left == 0) {
        method<ULONG(STDMETHODCALLTYPE*)(void*)>(r->host, 2)(r->host);  // the host's Release
        delete r;
    }
    return left;
}

HRESULT STDMETHODCALLTYPE query_interface(void* self, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    if (iid == kIUnknown || iid == kIResolver) {
        *out = self;
        add_ref(self);
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

// 3: CreateObjectFromURL(url, flags, props, MF_OBJECT_TYPE* type, IUnknown** object)
HRESULT STDMETHODCALLTYPE create_from_url(void* self, const wchar_t* url, DWORD flags, void* props, DWORD* type,
                                          void** object) {
    std::wstring local;
    if (const HRESULT hr = checked(url, local); hr != S_OK) {
        if (object) *object = nullptr;
        return hr;
    }
    void* h = host_resolver(self);
    return method<HRESULT(STDMETHODCALLTYPE*)(void*, const wchar_t*, DWORD, void*, DWORD*, void**)>(h, 3)(
        h, local.c_str(), flags, props, type, object);
}

// 4: CreateObjectFromByteStream(stream, url, flags, props, type, object): the URL is only a hint.
HRESULT STDMETHODCALLTYPE create_from_stream(void* self, void* stream, const wchar_t* url, DWORD flags, void* props,
                                             DWORD* type, void** object) {
    void* h = host_resolver(self);
    return method<HRESULT(STDMETHODCALLTYPE*)(void*, void*, const wchar_t*, DWORD, void*, DWORD*, void**)>(h, 4)(
        h, stream, url, flags, props, type, object);
}

// 5: BeginCreateObjectFromURL(url, flags, props, IUnknown** cancel, callback, state)
HRESULT STDMETHODCALLTYPE begin_from_url(void* self, const wchar_t* url, DWORD flags, void* props, void** cancel,
                                         void* callback, void* state) {
    std::wstring local;
    if (const HRESULT hr = checked(url, local); hr != S_OK) {
        if (cancel) *cancel = nullptr;
        return hr;
    }
    void* h = host_resolver(self);
    return method<HRESULT(STDMETHODCALLTYPE*)(void*, const wchar_t*, DWORD, void*, void**, void*, void*)>(h, 5)(
        h, local.c_str(), flags, props, cancel, callback, state);
}

// 6: EndCreateObjectFromURL(result, type, object); 8: EndCreateObjectFromByteStream, the same shape.
template <size_t Slot>
HRESULT STDMETHODCALLTYPE end_create(void* self, void* result, DWORD* type, void** object) {
    void* h = host_resolver(self);
    return method<HRESULT(STDMETHODCALLTYPE*)(void*, void*, DWORD*, void**)>(h, Slot)(h, result, type, object);
}

// 7: BeginCreateObjectFromByteStream(stream, url, flags, props, cancel, callback, state)
HRESULT STDMETHODCALLTYPE begin_from_stream(void* self, void* stream, const wchar_t* url, DWORD flags, void* props,
                                            void** cancel, void* callback, void* state) {
    void* h = host_resolver(self);
    return method<HRESULT(STDMETHODCALLTYPE*)(void*, void*, const wchar_t*, DWORD, void*, void**, void*, void*)>(h, 7)(
        h, stream, url, flags, props, cancel, callback, state);
}

// 9: CancelObjectCreation(cancel)
HRESULT STDMETHODCALLTYPE cancel(void* self, void* cookie) {
    void* h = host_resolver(self);
    return method<HRESULT(STDMETHODCALLTYPE*)(void*, void*)>(h, 9)(h, cookie);
}

const void* const kVtable[] = {
    reinterpret_cast<const void*>(&query_interface),   reinterpret_cast<const void*>(&add_ref),
    reinterpret_cast<const void*>(&release),           reinterpret_cast<const void*>(&create_from_url),
    reinterpret_cast<const void*>(&create_from_stream), reinterpret_cast<const void*>(&begin_from_url),
    reinterpret_cast<const void*>(&end_create<6>),     reinterpret_cast<const void*>(&begin_from_stream),
    reinterpret_cast<const void*>(&end_create<8>),     reinterpret_cast<const void*>(&cancel),
};
static_assert(sizeof kVtable / sizeof kVtable[0] == 10, "IUnknown + IMFSourceResolver's 7 methods");

// HRESULT MFCreateSourceResolver(IMFSourceResolver** resolver): the host's resolver behind the proxy.
HRESULT WINAPI MFCreateSourceResolver(void** out) {
    if (!out) return E_POINTER;
    using Create = HRESULT(WINAPI*)(void**);
    static const Create create = []() -> Create {
        HMODULE m = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m ? reinterpret_cast<Create>(reinterpret_cast<void*>(GetProcAddress(m, "MFCreateSourceResolver")))
                 : nullptr;
    }();
    if (!create) return kUnsupportedScheme;  // no Media Foundation on this host (VIctory's answer)
    void* host = nullptr;
    const HRESULT hr = create(&host);
    if (FAILED(hr) || !host) return hr;
    return wrap_resolver(host, out);
}

}  // namespace

long wrap_resolver(void* host_resolver, void** out) {
    if (!out) return E_POINTER;
    if (!host_resolver) return E_INVALIDARG;
    *out = new Resolver{kVtable, host_resolver};
    return S_OK;
}

}  // namespace rt::media

SHIM_TABLE(media_foundation,
    {"mfplat.dll", "MFCreateSourceResolver", reinterpret_cast<void*>(&rt::media::MFCreateSourceResolver)},
);
