// The console DXGI adapter and output (xbox_display.h), ported from VIctory's victory-gpu/src/dxgi.rs
// (GPL-3.0-or-later, Marvisak and contributors, DexNom fork).
//
// Both are raw vtables over the layer's PC objects, built the way the device proxy builds its own: the object's first
// word is the table, and a slot the console keeps in PC order goes to the PC method at the same slot. The thunks take
// four argument words after `this`, the most any forwarded DXGI method has, and hand them on in the same positions
// (see d3d12_xs_device.cpp on why a forwarding thunk must not move a caller's stack words).
#include "xbox_display.h"

#include "d3d12_com.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

namespace rt::d3d12 {
namespace {

using Word = std::uint64_t;
constexpr unsigned kTableSlots = 64;

enum class Kind : unsigned { adapter = 0, output = 1 };

struct ConsoleObject {
    void** vtable;  // offset 0: the title's IDXGIAdapter* / IDXGIOutput* points here
    IUnknown* inner;  // the PC adapter or output (one reference)
    std::atomic<ULONG> refs;
    Kind kind;
    ConsoleObject* adapter;  // an output's adapter (one reference); null for an adapter
};

std::atomic<bool> g_first[2][kTableSlots];
std::atomic<bool> g_refused[2][kTableSlots];

const char* kind_name(Kind kind) { return kind == Kind::adapter ? "adapter" : "output"; }

const char* slot_name(Kind kind, unsigned slot) {
    static const char* const shared[] = {"QueryInterface", "AddRef",          "Release",       "SetPrivateData",
                                         "SetPrivateDataInterface",           "GetPrivateData", "GetParent"};
    static const char* const adapter[] = {"EnumOutputs", "GetDesc", "CheckInterfaceSupport", "GetDesc1", "GetDesc2"};
    static const char* const output[] = {"GetDesc",
                                         "GetDisplayModeList",
                                         "FindClosestMatchingMode",
                                         "WaitForVBlank",
                                         "TakeOwnership",
                                         "ReleaseOwnership",
                                         "GetGammaControlCapabilities",
                                         "SetGammaControl",
                                         "GetGammaControl",
                                         "SetDisplaySurface",
                                         "GetDisplaySurfaceData",
                                         "GetFrameStatistics",
                                         "console method (not named)",
                                         "console method (not named)",
                                         "console display modes (V dxgi.rs:243-279)"};
    if (slot < 7) return shared[slot];
    if (kind == Kind::adapter) return slot < 12 ? adapter[slot - 7] : "past the console adapter's 12 slots";
    return slot < 22 ? output[slot - 7] : "past the console output's 22 slots";
}

void note(Kind kind, unsigned slot) {
    bool expected = false;
    if (slot < kTableSlots && !g_first[unsigned(kind)][slot].load(std::memory_order_relaxed) &&
        g_first[unsigned(kind)][slot].compare_exchange_strong(expected, true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] dxgi_xs: console %s slot %u (%s) first call\n", kind_name(kind), slot,
                     slot_name(kind, slot));
}

Word hr_word(HRESULT hr) { return Word(std::uint32_t(hr)); }

ConsoleObject* make(Kind kind, IUnknown* inner, ConsoleObject* adapter);

ULONG add_ref(ConsoleObject* self) { return self->refs.fetch_add(1, std::memory_order_relaxed) + 1; }

ULONG release(ConsoleObject* self) {
    const ULONG left = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (!left) {
        IUnknown* inner = self->inner;
        ConsoleObject* adapter = self->adapter;
        delete self;
        inner->Release();
        if (adapter) release(adapter);
    }
    return left;
}

bool answers(Kind kind, const IID& iid) {
    if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(IDXGIObject))) return true;
    // V dxgi.rs:32-51 and 72-77: the adapter answers IDXGIAdapter to IDXGIAdapter2, the output IDXGIOutput.
    if (kind == Kind::adapter)
        return IsEqualIID(iid, __uuidof(IDXGIAdapter)) || IsEqualIID(iid, __uuidof(IDXGIAdapter1)) ||
               IsEqualIID(iid, __uuidof(IDXGIAdapter2));
    return IsEqualIID(iid, __uuidof(IDXGIOutput));
}

HRESULT query(ConsoleObject* self, const IID* riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (!answers(self->kind, *riid)) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
            std::fprintf(stderr,
                         "[rt] dxgi_xs: console %s QueryInterface {%08lx-%04x-%04x-...}: not an interface it has; "
                         "E_NOINTERFACE\n",
                         kind_name(self->kind), static_cast<unsigned long>(riid->Data1), riid->Data2, riid->Data3);
        return E_NOINTERFACE;
    }
    add_ref(self);
    *out = self;
    return S_OK;
}

template <Kind K>
Word WINAPI thunk_query(ConsoleObject* self, Word riid, Word out, Word, Word) {
    note(K, 0);
    return hr_word(query(self, reinterpret_cast<const IID*>(riid), reinterpret_cast<void**>(out)));
}
template <Kind K>
Word WINAPI thunk_add_ref(ConsoleObject* self, Word, Word, Word, Word) {
    note(K, 1);
    return add_ref(self);
}
template <Kind K>
Word WINAPI thunk_release(ConsoleObject* self, Word, Word, Word, Word) {
    note(K, 2);
    return release(self);
}

template <Kind K, unsigned Slot>
Word WINAPI thunk_forward(ConsoleObject* self, Word a1, Word a2, Word a3, Word a4) {
    note(K, Slot);
    using Fn = Word(WINAPI*)(void*, Word, Word, Word, Word);
    void** table = *reinterpret_cast<void***>(self->inner);
    return reinterpret_cast<Fn>(table[Slot])(self->inner, a1, a2, a3, a4);
}

template <Kind K, unsigned Slot>
Word WINAPI thunk_refuse(ConsoleObject*, Word a1, Word a2, Word a3, Word a4) {
    note(K, Slot);
    bool expected = false;
    if (g_refused[unsigned(K)][Slot].compare_exchange_strong(expected, true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] dxgi_xs: console %s slot %u (%s) is not implemented; returning E_NOTIMPL (args 0x%llx "
                     "0x%llx 0x%llx 0x%llx)\n",
                     kind_name(K), Slot, slot_name(K, Slot), static_cast<unsigned long long>(a1),
                     static_cast<unsigned long long>(a2), static_cast<unsigned long long>(a3),
                     static_cast<unsigned long long>(a4));
    return hr_word(E_NOTIMPL);
}

// Adapter slot 7: EnumOutputs(index, &output), the output in the console layout.
Word WINAPI adapter_enum_outputs(ConsoleObject* self, Word index, Word out_word, Word, Word) {
    note(Kind::adapter, 7);
    auto** out = reinterpret_cast<IDXGIOutput**>(out_word);
    if (!out) return hr_word(E_INVALIDARG);
    *out = nullptr;
    IDXGIOutput* pc = nullptr;
    const HRESULT hr = static_cast<IDXGIAdapter*>(self->inner)->EnumOutputs(static_cast<UINT>(index), &pc);
    if (FAILED(hr) || !pc) return hr_word(FAILED(hr) ? hr : DXGI_ERROR_NOT_FOUND);
    ConsoleObject* output = make(Kind::output, pc, self);
    pc->Release();
    if (!output) return hr_word(E_OUTOFMEMORY);
    *out = reinterpret_cast<IDXGIOutput*>(output);
    return hr_word(S_OK);
}

// Output slot 6: GetParent(riid, &parent), the console adapter it came from.
Word WINAPI output_get_parent(ConsoleObject* self, Word riid, Word out, Word, Word) {
    note(Kind::output, 6);
    return hr_word(query(self->adapter, reinterpret_cast<const IID*>(riid), reinterpret_cast<void**>(out)));
}

// Output slot 7: GetDesc, with the display mode's size as the desktop coordinates.
Word WINAPI output_get_desc(ConsoleObject* self, Word desc_word, Word, Word, Word) {
    note(Kind::output, 7);
    auto* desc = reinterpret_cast<DXGI_OUTPUT_DESC*>(desc_word);
    const HRESULT hr = static_cast<IDXGIOutput*>(self->inner)->GetDesc(desc);
    if (SUCCEEDED(hr) && desc) {
        const leonida::DisplayMode mode = title_display_mode();
        desc->DesktopCoordinates = {0, 0, LONG(mode.width), LONG(mode.height)};
    }
    return hr_word(hr);
}

DXGI_MODE_DESC pc_mode(DXGI_FORMAT format) {
    const leonida::DisplayMode mode = title_display_mode();
    DXGI_MODE_DESC m{};
    m.Width = mode.width;
    m.Height = mode.height;
    m.RefreshRate = {mode.refresh_hz, 1};
    m.Format = format;
    m.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
    m.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    return m;
}

// Output slot 8: the PC GetDisplayModeList(format, flags, &count, modes): the display mode alone.
Word WINAPI output_get_display_mode_list(ConsoleObject*, Word format, Word, Word count_word, Word out_word) {
    note(Kind::output, 8);
    auto* count = reinterpret_cast<UINT*>(count_word);
    auto* out = reinterpret_cast<DXGI_MODE_DESC*>(out_word);
    if (!count) return hr_word(DXGI_ERROR_INVALID_CALL);
    if (!out) {
        *count = 1;
        return hr_word(S_OK);
    }
    if (*count < 1) return hr_word(DXGI_ERROR_MORE_DATA);
    *count = 1;
    out[0] = pc_mode(static_cast<DXGI_FORMAT>(format));
    return hr_word(S_OK);
}

// Output slot 9: the PC FindClosestMatchingMode(match, &closest, device): the display mode, in the asked format.
Word WINAPI output_find_closest_mode(ConsoleObject*, Word match_word, Word closest_word, Word, Word) {
    note(Kind::output, 9);
    const auto* match = reinterpret_cast<const DXGI_MODE_DESC*>(match_word);
    auto* closest = reinterpret_cast<DXGI_MODE_DESC*>(closest_word);
    if (!match || !closest) return hr_word(DXGI_ERROR_INVALID_CALL);
    *closest = pc_mode(match->Format == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_B8G8R8A8_UNORM : match->Format);
    return hr_word(S_OK);
}

// Output slot 21: modes(format, flags, &count, out), V dxgi.rs:243-279. A null count is E_INVALIDARG; a null `out`
// asks for the count (1); room for none is DXGI_ERROR_MORE_DATA; else the one mode: the display mode at 60 Hz, format
// 0 and progressive scanlines, as VIctory writes it.
Word WINAPI output_modes(ConsoleObject*, Word format, Word flags, Word count_word, Word out_word) {
    note(Kind::output, kConsoleOutputModesSlot);
    auto* count = reinterpret_cast<std::uint32_t*>(count_word);
    auto* out = reinterpret_cast<ConsoleDisplayMode*>(out_word);
    static std::atomic<bool> logged{false};
    const leonida::DisplayMode mode = title_display_mode();
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] dxgi_xs: console output slot 21 (display modes, format %u, flags 0x%x): %ux%u at %u Hz "
                     "reported\n",
                     static_cast<unsigned>(format), static_cast<unsigned>(flags), mode.width, mode.height,
                     mode.refresh_hz);
    if (!count) return hr_word(E_INVALIDARG);
    if (!out) {
        *count = 1;
        return hr_word(S_OK);
    }
    if (*count < 1) return hr_word(DXGI_ERROR_MORE_DATA);
    *count = 1;
    out[0] = ConsoleDisplayMode{mode.width, mode.height, mode.refresh_hz, 1, 0, 1, 0, 0};
    return hr_word(S_OK);
}

template <Kind K, unsigned Slot>
void* entry() {
    if constexpr (Slot == 0) return reinterpret_cast<void*>(&thunk_query<K>);
    else if constexpr (Slot == 1) return reinterpret_cast<void*>(&thunk_add_ref<K>);
    else if constexpr (Slot == 2) return reinterpret_cast<void*>(&thunk_release<K>);
    else if constexpr (K == Kind::adapter) {
        if constexpr (Slot == 7) return reinterpret_cast<void*>(&adapter_enum_outputs);
        else if constexpr (Slot < 12) return reinterpret_cast<void*>(&thunk_forward<K, Slot>);
        else return reinterpret_cast<void*>(&thunk_refuse<K, Slot>);
    } else {
        if constexpr (Slot == 6) return reinterpret_cast<void*>(&output_get_parent);
        else if constexpr (Slot == 7) return reinterpret_cast<void*>(&output_get_desc);
        else if constexpr (Slot == 8) return reinterpret_cast<void*>(&output_get_display_mode_list);
        else if constexpr (Slot == 9) return reinterpret_cast<void*>(&output_find_closest_mode);
        else if constexpr (Slot == kConsoleOutputModesSlot) return reinterpret_cast<void*>(&output_modes);
        else if constexpr (Slot < 19) return reinterpret_cast<void*>(&thunk_forward<K, Slot>);
        else return reinterpret_cast<void*>(&thunk_refuse<K, Slot>);
    }
}

template <Kind K, std::size_t... Slots>
std::array<void*, kTableSlots> build(std::index_sequence<Slots...>) {
    return {entry<K, unsigned(Slots)>()...};
}

void** table(Kind kind) {
    static std::array<void*, kTableSlots> adapter = build<Kind::adapter>(std::make_index_sequence<kTableSlots>{});
    static std::array<void*, kTableSlots> output = build<Kind::output>(std::make_index_sequence<kTableSlots>{});
    return kind == Kind::adapter ? adapter.data() : output.data();
}

ConsoleObject* make(Kind kind, IUnknown* inner, ConsoleObject* adapter) {
    auto* object = new (std::nothrow) ConsoleObject{table(kind), inner, {1}, kind, adapter};
    if (!object) return nullptr;
    inner->AddRef();
    if (adapter) add_ref(adapter);
    return object;
}

}  // namespace

double title_aspect_ratio() {
    static const double ratio = [] {
        char value[64] = {};
        const DWORD length = GetEnvironmentVariableA("LEONIDA_ASPECT", value, sizeof(value));
        if (length == 0) return 0.0;
        leonida::aspect::Request request;
        if (length >= sizeof(value) || !leonida::aspect::parse_request(value, &request)) {
            std::fprintf(stderr, "[rt] display: LEONIDA_ASPECT=%s is not W:H, a ratio in 1..4, auto or off; ignored\n",
                         length < sizeof(value) ? value : "(too long)");
            return 0.0;
        }
        // `auto`: the primary monitor's shape (its size in whatever DPI units this thread has; the ratio is the same).
        std::uint32_t monitor_w = 0, monitor_h = 0;
        MONITORINFO monitor{sizeof(monitor)};
        if (request.kind == leonida::aspect::Kind::automatic &&
            GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
            monitor_w = std::uint32_t(monitor.rcMonitor.right - monitor.rcMonitor.left);
            monitor_h = std::uint32_t(monitor.rcMonitor.bottom - monitor.rcMonitor.top);
        }
        const double r = leonida::aspect::resolve(request, monitor_w, monitor_h);
        if (request.kind == leonida::aspect::Kind::automatic)
            std::fprintf(stderr, "[rt] display: LEONIDA_ASPECT=auto: the primary monitor is %ux%u (%.3f:1)%s\n", monitor_w,
                         monitor_h, r, r > 0.0 ? "" : ", not a ratio in 1..4; ignored");
        else if (r > 0.0)
            std::fprintf(stderr, "[rt] display: LEONIDA_ASPECT=%s (%.3f:1)\n", value, r);
        return r;
    }();
    return ratio;
}

leonida::DisplayMode title_display_mode() {
    static const leonida::DisplayMode mode = [] {
        leonida::DisplayMode m;
        char value[64] = {};
        const DWORD length = GetEnvironmentVariableA("LEONIDA_DISPLAY_MODE", value, sizeof(value));
        if (length == 0) {
            std::fprintf(stderr, "[rt] display: the title is told its display is %ux%u (LEONIDA_DISPLAY_MODE=WxH "
                                 "changes it)\n", m.width, m.height);
        } else if (length < sizeof(value) && leonida::parse_display_mode(value, &m)) {
            std::fprintf(stderr, "[rt] display: the title is told its display is %ux%u (LEONIDA_DISPLAY_MODE)\n",
                         m.width, m.height);
        } else {
            std::fprintf(stderr, "[rt] display: LEONIDA_DISPLAY_MODE=%s is not WxH with W in 640..7680 and H in "
                                 "360..4320; the title is told %ux%u\n",
                         length < sizeof(value) ? value : "(too long)", m.width, m.height);
        }
        // LEONIDA_ASPECT (src/aspect_fit.h): the same height, wider. Only the answers here change; the size the title
        // renders at is compiled into it (src/display_profile.h), so whether it follows is for a run to show.
        if (const double ratio = title_aspect_ratio(); ratio > 0.0) {
            const leonida::DisplayMode wide = leonida::aspect::widen_display_mode(m, ratio);
            if (wide.width != m.width || wide.height != m.height)
                std::fprintf(stderr, "[rt] display: the title is told its display is %ux%u instead (LEONIDA_ASPECT)\n",
                             wide.width, wide.height);
            else
                std::fprintf(stderr, "[rt] display: LEONIDA_ASPECT is not wider than %ux%u; the mode is kept\n", m.width,
                             m.height);
            m = wide;
        }
        return m;
    }();
    return mode;
}

HRESULT create_console_adapter(IDXGIAdapter* pc, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!pc) return E_INVALIDARG;
    ConsoleObject* adapter = make(Kind::adapter, pc, nullptr);
    if (!adapter) return E_OUTOFMEMORY;
    const HRESULT hr = query(adapter, &riid, out);
    release(adapter);
    return hr;
}

HRESULT console_adapter_provider(ID3D12Device* real, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = create_dxgi_factory(real, __uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) return hr;
    IDXGIAdapter* adapter = nullptr;
    hr = factory->EnumAdapters(0, &adapter);
    factory->Release();
    if (FAILED(hr)) return hr;
    hr = create_console_adapter(adapter, riid, out);
    adapter->Release();
    return hr;
}

bool is_console_dxgi_object(const void* object) {
    if (!object) return false;
    void* const* vtable = *static_cast<void* const* const*>(object);
    return vtable == table(Kind::adapter) || vtable == table(Kind::output);
}

}  // namespace rt::d3d12
