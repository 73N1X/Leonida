// mfplat.dll's source resolver proxy (shims/media_foundation.cpp): the URL rule, and the proxy over a fake host
// resolver that records what reaches it. The URL cases follow VIctory's test (crates/victory-hle/src/modules/media.rs,
// only_local_files_reach_the_host), GPL-3.0-or-later, by Marvisak and contributors (DexNom fork); Leonida is
// GPL-3.0-or-later. No game data, no network, no Media Foundation.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "file_system.h"
#include "media_foundation.h"
#include "shims.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

constexpr HRESULT kUnsupportedScheme = static_cast<HRESULT>(0xC00D36C3u);

bool refused(const wchar_t* url, const char* scheme, const char* host) {
    std::wstring out;
    std::string s, h;
    return !rt::media::local_url(url, out, s, h) && s == scheme && h == host;
}

std::wstring local(const wchar_t* url) {
    std::wstring out;
    std::string s, h;
    return rt::media::local_url(url, out, s, h) ? out : L"<refused>";
}

// A fake IMFSourceResolver: records the URL of CreateObjectFromURL / BeginCreateObjectFromURL and its releases.
struct FakeResolver {
    const void* const* vtable;
    std::wstring last_url;
    int calls = 0;
    int released = 0;
};

HRESULT STDMETHODCALLTYPE fake_qi(void*, REFIID, void**) { return E_NOINTERFACE; }
ULONG STDMETHODCALLTYPE fake_add_ref(void*) { return 2; }
ULONG STDMETHODCALLTYPE fake_release(void* self) {
    ++static_cast<FakeResolver*>(self)->released;
    return 0;
}
HRESULT STDMETHODCALLTYPE fake_create(void* self, const wchar_t* url, DWORD, void*, DWORD* type, void** object) {
    auto* f = static_cast<FakeResolver*>(self);
    f->last_url = url;
    ++f->calls;
    if (type) *type = 1;
    if (object) *object = self;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE fake_begin(void* self, const wchar_t* url, DWORD, void*, void**, void*, void*) {
    auto* f = static_cast<FakeResolver*>(self);
    f->last_url = url;
    ++f->calls;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE fake_other(void* self) {
    ++static_cast<FakeResolver*>(self)->calls;
    return S_FALSE;
}

const void* const kFakeVtable[] = {
    reinterpret_cast<const void*>(&fake_qi),    reinterpret_cast<const void*>(&fake_add_ref),
    reinterpret_cast<const void*>(&fake_release), reinterpret_cast<const void*>(&fake_create),
    reinterpret_cast<const void*>(&fake_other), reinterpret_cast<const void*>(&fake_begin),
    reinterpret_cast<const void*>(&fake_other), reinterpret_cast<const void*>(&fake_other),
    reinterpret_cast<const void*>(&fake_other), reinterpret_cast<const void*>(&fake_other),
};

template <class F>
F slot(void* object, size_t n) {
    return reinterpret_cast<F>((*static_cast<void***>(object))[n]);
}

}  // namespace

int main() {
    std::printf("media_foundation_test\n");

    // Network schemes and shares are refused, naming scheme and host.
    check(refused(L"https://cdn.example.com/a.mp4", "https", "cdn.example.com"), "https is refused, with its host");
    check(refused(L"RTSP://10.0.0.1:554/x", "rtsp", "10.0.0.1:554"), "rtsp in any case is refused");
    check(refused(L"mms://h/x", "mms", "h"), "mms is refused");
    check(refused(L"\\\\server\\share\\a.mp4", "unc", "server"), "a UNC share is refused");
    check(refused(L"\\\\?\\UNC\\server\\share\\a.mp4", "unc", "server"), "a long-path UNC share is refused");
    check(refused(L"file://server/share/a.mp4", "file", "server"), "a file: URL with a host is refused");

    // Other local files pass as they are, a file: URL becomes its path.
    check(local(L"file:///C:/Windows/a.wav") == L"C:\\Windows\\a.wav", "file:///C:/... becomes C:\\...");
    check(local(L"file://localhost/C:/a%20b.wav") == L"C:\\a b.wav", "localhost and %20 are understood");
    check(local(L"file:///C:/%C3%A9.wav") == L"C:\\\u00e9.wav", "percent escapes are UTF-8");
    check(local(L"clip.mp4") == L"clip.mp4", "a relative path passes as it is");
    check(local(L"D:\\movies\\x.mp4") == L"D:\\movies\\x.mp4", "a host path passes as it is");

    // A console path goes to its host file, also as a file: URL.
    const std::filesystem::path root = std::filesystem::temp_directory_path() / L"leonida_media_foundation_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / L"game" / L"movies");
    { std::ofstream(root / L"game" / L"movies" / L"intro.mp4") << "1"; }
    rt::fs::configure(root / L"game", root / L"overlay");
    const std::wstring want = (root / L"game" / L"movies" / L"intro.mp4").wstring();
    const std::wstring got = local(L"GAME:\\movies\\intro.mp4");
    check(std::filesystem::equivalent(got, want, ec), "GAME:\\ goes to the package folder");
    check(std::filesystem::equivalent(local(L"file:///GAME:/movies/intro.mp4"), want, ec), "so does file:///GAME:/");
    check(std::filesystem::equivalent(local(L"G:\\movies\\intro.mp4"), want, ec), "and the G: drive");

    // The proxy over a fake host resolver.
    FakeResolver fake{kFakeVtable};
    void* proxy = nullptr;
    check(rt::media::wrap_resolver(&fake, &proxy) == S_OK && proxy, "the proxy wraps a host resolver");
    using CreateFn = HRESULT(STDMETHODCALLTYPE*)(void*, const wchar_t*, DWORD, void*, DWORD*, void**);
    using BeginFn = HRESULT(STDMETHODCALLTYPE*)(void*, const wchar_t*, DWORD, void*, void**, void*, void*);
    DWORD type = 0;
    void* object = reinterpret_cast<void*>(1);
    check(slot<CreateFn>(proxy, 3)(proxy, L"http://example.com/a.mp4", 1, nullptr, &type, &object) ==
                  kUnsupportedScheme &&
              !object && fake.calls == 0,
          "CreateObjectFromURL refuses http before the host sees it, with a null object");
    check(slot<CreateFn>(proxy, 3)(proxy, L"GAME:\\movies\\intro.mp4", 1, nullptr, &type, &object) == S_OK &&
              fake.calls == 1 && std::filesystem::equivalent(fake.last_url, want, ec) && object == &fake && type == 1,
          "a console path reaches the host as its host file");
    void* cancel = reinterpret_cast<void*>(1);
    check(slot<BeginFn>(proxy, 5)(proxy, L"rtsp://h/x", 1, nullptr, &cancel, nullptr, nullptr) == kUnsupportedScheme &&
              !cancel && fake.calls == 1,
          "BeginCreateObjectFromURL refuses rtsp");
    check(slot<BeginFn>(proxy, 5)(proxy, L"file:///C:/a.mp4", 1, nullptr, &cancel, nullptr, nullptr) == S_OK &&
              fake.last_url == L"C:\\a.mp4",
          "BeginCreateObjectFromURL forwards a local file");
    using CancelFn = HRESULT(STDMETHODCALLTYPE*)(void*, void*);
    check(slot<CancelFn>(proxy, 9)(proxy, nullptr) == S_FALSE, "CancelObjectCreation reaches the host");
    void* other = nullptr;
    constexpr GUID kIMFMediaSource = {0x279a808d, 0xaec7, 0x40c8, {0x9c, 0x6b, 0xa6, 0xb4, 0x92, 0xc7, 0x8a, 0x66}};
    using QiFn = HRESULT(STDMETHODCALLTYPE*)(void*, REFIID, void**);
    check(slot<QiFn>(proxy, 0)(proxy, kIMFMediaSource, &other) == E_NOINTERFACE && !other,
          "QueryInterface hands out nothing but the resolver");
    using RefFn = ULONG(STDMETHODCALLTYPE*)(void*);
    check(slot<RefFn>(proxy, 1)(proxy) == 2 && slot<RefFn>(proxy, 2)(proxy) == 1 && fake.released == 0,
          "AddRef and Release count");
    check(slot<RefFn>(proxy, 2)(proxy) == 0 && fake.released == 1, "the last Release releases the host resolver");

    check(rt::shims::find("mfplat.dll", "MFCreateSourceResolver") != nullptr, "MFCreateSourceResolver is registered");
    check(!rt::shims::owns_dll("mfplat.dll"), "mfplat.dll stays a host module for LoadLibrary");

    std::filesystem::remove_all(root, ec);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
