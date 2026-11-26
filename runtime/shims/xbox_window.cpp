// The title's window, its caption, PresentX onto it, and the screenshots on request (xbox_window.h). Ported from
// VIctory's victory-gpu/src/host.rs and capture.rs (GPL-3.0-or-later, Marvisak and contributors, DexNom fork).
//
// No object here is a static with a destructor or a thread_local (AGENTS.md, "Runtime traps"): the presentation a
// title attaches lives until the process ends, and its threads end with it.
#include "xbox_window.h"

#include "d3d12_com.h"
#include "d3d12_xs_device.h"
#include "xbox_display.h"
#include "xbox_present.h"

#include "../src/frame_meter.h"
#include "../src/png_write.h"
#include "../src/vulkan_present.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace rt::d3d12 {
namespace {

std::uint64_t now_ns() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
}

// Looked up at run time: Windows 10 1607 and later have both, Wine may not.
void name_thread(const wchar_t* name) {
    using Fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (const auto set = reinterpret_cast<Fn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))))
        set(GetCurrentThread(), name);
}

// Only the window's thread is per-monitor DPI aware, so the window's client area is in pixels and Windows does not
// stretch it on a scaled monitor; the rest of the process keeps its awareness.
void make_thread_dpi_aware() {
    using Fn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (const auto set = reinterpret_cast<Fn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"))))
        set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

std::string utf8(const std::filesystem::path& path) {
    const std::wstring& wide = path.native();
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(std::size_t(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), out.data(), bytes, nullptr, nullptr);
    return out;
}

std::wstring environment(const wchar_t* name) {
    wchar_t value[1024];
    const DWORD length = GetEnvironmentVariableW(name, value, DWORD(std::size(value)));
    return length && length < std::size(value) ? std::wstring(value, length) : std::wstring();
}

constexpr wchar_t kWindowClass[] = L"LeonidaWindow";
constexpr UINT kDestroyRequest = WM_APP + 1;  // ~GameWindow: the window is destroyed by its own thread

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_CLOSE:
        // A console title has no way to be closed; the user closing its window ends the run, at once (the title's
        // threads are not asked: they would not know how to stop).
        std::fprintf(stderr, "[rt] window: closed; the run ends\n");
        std::fflush(nullptr);
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    case kDestroyRequest:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, w, l);
    }
}

// V host.rs `make_window`: a WS_OVERLAPPEDWINDOW of the given client size, here shrunk (keeping its shape) to the
// primary monitor's work area and centred in it.
HWND make_window(std::uint32_t width, std::uint32_t height) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    window_class.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));      // IDI_APPLICATION
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::fprintf(stderr, "[rt] window: RegisterClassExW failed (%lu)\n", GetLastError());
        return nullptr;
    }
    constexpr DWORD style = WS_OVERLAPPEDWINDOW;
    RECT work{0, 0, 1920, 1080};
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor)) work = monitor.rcWork;
    RECT frame{0, 0, 1000, 1000};
    AdjustWindowRectEx(&frame, style, FALSE, 0);
    const LONG frame_w = (frame.right - frame.left) - 1000, frame_h = (frame.bottom - frame.top) - 1000;
    const LONG work_w = work.right - work.left, work_h = work.bottom - work.top;
    const double scale = std::min({1.0, double(work_w - frame_w) / double(width), double(work_h - frame_h) / double(height)});
    const LONG client_w = std::max(1L, LONG(double(width) * scale)), client_h = std::max(1L, LONG(double(height) * scale));
    const LONG outer_w = client_w + frame_w, outer_h = client_h + frame_h;
    const HWND hwnd = CreateWindowExW(0, kWindowClass, L"Leonida", style | WS_VISIBLE,
                                      work.left + std::max(0L, (work_w - outer_w) / 2),
                                      work.top + std::max(0L, (work_h - outer_h) / 2), outer_w, outer_h, nullptr, nullptr,
                                      instance, nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "[rt] window: CreateWindowExW failed (%lu)\n", GetLastError());
        return nullptr;
    }
    // The frame of a DPI-aware window can differ from the system-DPI guess above: make the client area exact.
    RECT client{};
    if (GetClientRect(hwnd, &client) && (client.right != client_w || client.bottom != client_h)) {
        RECT outer{};
        GetWindowRect(hwnd, &outer);
        SetWindowPos(hwnd, nullptr, 0, 0, (outer.right - outer.left) + client_w - client.right,
                     (outer.bottom - outer.top) + client_h - client.bottom, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    std::fprintf(stderr, "[rt] window: %ldx%ld client area for the title's %ux%u frames%s\n", client_w, client_h, width,
                 height, scale < 1.0 ? " (shrunk to fit the monitor)" : "");
    return hwnd;
}

}  // namespace

struct GameWindow::State {
    HWND hwnd = nullptr;
    bool owned = false;  // made here (and destroyed here), not the title's
    std::thread pump;
    std::thread caption;
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    leonida::FrameMeter meter;
};

namespace {

// V host.rs "victory-caption": setting the text waits for the window's thread, so a thread of ours does it, once a
// second, from the counts the present path leaves. For the title's window that thread is the title's, which may not
// pump for a while: the wait is bounded, and only this thread waits.
void start_caption(GameWindow::State* s) {
    s->caption = std::thread([s] {
        name_thread(L"leonida-caption");
        leonida::FrameRateSampler sampler;
        std::string shown;
        std::unique_lock lock(s->mutex);
        while (!s->wake.wait_for(lock, std::chrono::seconds(1), [s] { return s->stop; })) {
            lock.unlock();
            leonida::FrameFigures figures;
            if (sampler.sample(s->meter.read(), now_ns(), &figures)) {
                const std::string text = leonida::caption_text(figures);
                if (text != shown) {
                    const std::wstring wide(text.begin(), text.end());  // ASCII
                    DWORD_PTR result = 0;
                    SendMessageTimeoutW(s->hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(wide.c_str()),
                                        SMTO_ABORTIFHUNG, 1000, &result);
                    shown = text;
                }
            }
            lock.lock();
        }
    });
}

std::atomic<HWND> g_title_window{nullptr};
std::atomic<HWND> g_own_window{nullptr};

BOOL CALLBACK find_visible_window(HWND hwnd, LPARAM found) {
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    if (process != GetCurrentProcessId() || !IsWindowVisible(hwnd) || hwnd == g_own_window.load()) return TRUE;
    // A console window can be reported as this process's: never present into it.
    char window_class[32] = {};
    GetClassNameA(hwnd, window_class, sizeof(window_class));
    if (hwnd == GetConsoleWindow() || !std::strcmp(window_class, "ConsoleWindowClass") ||
        !std::strcmp(window_class, "PseudoConsoleWindow"))
        return TRUE;
    *reinterpret_cast<HWND*>(found) = hwnd;
    return FALSE;
}

}  // namespace

void note_title_window(HWND window) { g_title_window.store(window); }

HWND title_window() {
    if (const HWND noted = g_title_window.load(); noted && IsWindow(noted)) return noted;
    HWND found = nullptr;
    EnumWindows(&find_visible_window, reinterpret_cast<LPARAM>(&found));
    return found;
}

GameWindow::GameWindow(std::unique_ptr<State> state) : state_(std::move(state)) {}

std::unique_ptr<GameWindow> GameWindow::adopt(HWND window) {
    if (!window || !IsWindow(window)) return nullptr;
    auto state = std::make_unique<State>();
    state->hwnd = window;
    RECT client{};
    GetClientRect(window, &client);
    std::fprintf(stderr, "[rt] window: the title's own window shows its frames (%ldx%ld client area)\n", client.right,
                 client.bottom);
    start_caption(state.get());
    return std::unique_ptr<GameWindow>(new GameWindow(std::move(state)));
}

std::unique_ptr<GameWindow> GameWindow::create(std::uint32_t width, std::uint32_t height) {
    if (!width || !height) return nullptr;
    auto state = std::make_unique<State>();
    std::promise<HWND> made;
    std::future<HWND> hwnd = made.get_future();
    // V host.rs `window` and `pump`: the window belongs to a thread of ours that does nothing but pump its messages,
    // so it answers (moves, resizes, the caption) while the title's threads are busy.
    state->pump = std::thread([made = std::move(made), width, height]() mutable {
        name_thread(L"leonida-window");
        make_thread_dpi_aware();
        const HWND window = make_window(width, height);
        made.set_value(window);
        if (!window) return;
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    });
    state->hwnd = hwnd.get();
    if (!state->hwnd) {
        state->pump.join();
        return nullptr;
    }
    state->owned = true;
    g_own_window.store(state->hwnd);
    start_caption(state.get());
    return std::unique_ptr<GameWindow>(new GameWindow(std::move(state)));
}

GameWindow::~GameWindow() {
    {
        std::lock_guard lock(state_->mutex);
        state_->stop = true;
    }
    state_->wake.notify_all();
    if (state_->caption.joinable()) state_->caption.join();
    if (state_->owned && state_->hwnd) {
        HWND expected = state_->hwnd;
        g_own_window.compare_exchange_strong(expected, nullptr);
        PostMessageW(state_->hwnd, kDestroyRequest, 0, 0);
    }
    if (state_->pump.joinable()) state_->pump.join();
}

HWND GameWindow::hwnd() const { return state_->hwnd; }

void GameWindow::frame_presented() { state_->meter.presented(now_ns()); }

namespace {

// The presented frame in a snapshot's swapchain format as 8-bit RGB, `width * 3` bytes a row. False for a format the
// presenter does not make.
bool to_rgb8(const leonida::PresentSnapshot& snapshot, std::vector<std::uint8_t>& rgb) {
    const std::size_t texels = std::size_t(snapshot.extent.width) * snapshot.extent.height;
    const std::uint8_t* src = snapshot.pixels();
    if (!src || !texels) return false;
    rgb.resize(texels * 3);
    switch (snapshot.format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        for (std::size_t i = 0; i < texels; ++i) {
            rgb[i * 3 + 0] = src[i * 4 + 2];
            rgb[i * 3 + 1] = src[i * 4 + 1];
            rgb[i * 3 + 2] = src[i * 4 + 0];
        }
        return true;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        for (std::size_t i = 0; i < texels; ++i) std::memcpy(&rgb[i * 3], src + i * 4, 3);
        return true;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: {
        // An HDR10 swapchain: its PQ code values, scaled to 8 bits as they are (the PNG looks flat; it is not tone mapped).
        const bool blue_low = snapshot.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        for (std::size_t i = 0; i < texels; ++i) {
            std::uint32_t word = 0;
            std::memcpy(&word, src + i * 4, sizeof word);
            const std::uint32_t low = word & 0x3ffu, mid = (word >> 10) & 0x3ffu, high = (word >> 20) & 0x3ffu;
            const auto to8 = [](std::uint32_t v) { return std::uint8_t((v * 255u + 511u) / 1023u); };
            rgb[i * 3 + 0] = to8(blue_low ? high : low);
            rgb[i * 3 + 1] = to8(mid);
            rgb[i * 3 + 2] = to8(blue_low ? low : high);
        }
        return true;
    }
    default:
        return false;
    }
}

// LEONIDA_CAPTURE_DIR: see xbox_window.h.
class FrameCapture {
public:
    static std::unique_ptr<FrameCapture> from_environment() {
        const std::wstring dir = environment(L"LEONIDA_CAPTURE_DIR");
        if (dir.empty()) return nullptr;
        std::wstring request = environment(L"LEONIDA_CAPTURE_REQUEST");
        auto capture = std::unique_ptr<FrameCapture>(new FrameCapture(dir, request));
        capture->thread_ = std::thread([c = capture.get()] { c->work(); });
        return capture;
    }

    ~FrameCapture() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    // On the present thread, before each present: a request read since the last one asks for the next frame.
    void before_present(WindowPresenter& presenter) {
        const std::uint64_t wanted = requested_.load(std::memory_order_acquire);
        std::uint64_t armed = armed_.load(std::memory_order_relaxed);
        do {
            if (wanted <= armed) return;
        } while (!armed_.compare_exchange_weak(armed, wanted, std::memory_order_relaxed));
        presenter.request_snapshot([this, wanted](leonida::PresentSnapshot snapshot) {
            {
                std::lock_guard lock(mutex_);
                jobs_.push_back(Job{wanted, std::move(snapshot)});
            }
            wake_.notify_all();
        });
    }

private:
    struct Job {
        std::uint64_t request;
        leonida::PresentSnapshot snapshot;
    };

    FrameCapture(const std::filesystem::path& dir, const std::wstring& request)
        : dir_(dir), request_file_(request.empty() ? dir / L"request.txt" : std::filesystem::path(request)) {
        std::error_code ignored;
        std::filesystem::create_directories(dir_, ignored);
        std::fprintf(stderr,
                     "[rt] capture: screenshots on request into %s: a number in %s above the last one saves the next "
                     "presented frame as capture-<number>.png\n",
                     utf8(dir_).c_str(), utf8(request_file_).c_str());
    }

    // The thread: the request file every 100 ms, and the snapshots its sink hands over, encoded and written here.
    void work() {
        name_thread(L"leonida-capture");
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait_for(lock, std::chrono::milliseconds(100), [this] { return stop_ || !jobs_.empty(); });
            while (!jobs_.empty()) {
                Job job = std::move(jobs_.front());
                jobs_.pop_front();
                lock.unlock();
                write(job);
                lock.lock();
            }
            if (stop_) return;
            lock.unlock();
            poll();
            lock.lock();
        }
    }

    void poll() {
        const HANDLE file = CreateFileW(request_file_.c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        char text[32] = {};
        DWORD read = 0;
        const BOOL ok = ReadFile(file, text, sizeof(text) - 1, &read, nullptr);
        CloseHandle(file);
        if (!ok) return;
        std::uint64_t number = 0;
        std::size_t at = 0;
        while (at < read && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n')) ++at;
        bool digits = false;
        for (; at < read && text[at] >= '0' && text[at] <= '9' && number < 100000000000000000ull; ++at, digits = true)
            number = number * 10 + std::uint64_t(text[at] - '0');
        if (!digits || number <= taken_) return;
        taken_ = number;
        requested_.store(number, std::memory_order_release);
        std::fprintf(stderr, "[rt] capture: request %llu: the next presented frame is saved\n",
                     static_cast<unsigned long long>(number));
    }

    void write(Job& job) {
        const auto start = std::chrono::steady_clock::now();
        const VkExtent2D extent = job.snapshot.extent;
        const std::uint64_t frame = job.snapshot.frame;
        std::vector<std::uint8_t> rgb;
        const bool converted = to_rgb8(job.snapshot, rgb);
        job.snapshot.reset();  // the copy's memory goes back before the encoding
        if (!converted) {
            std::fprintf(stderr, "[rt] capture: request %llu: the swapchain format cannot be saved\n",
                         static_cast<unsigned long long>(job.request));
            return;
        }
        const std::vector<std::uint8_t> png = leonida::png::encode_rgb8(extent.width, extent.height, rgb.data(),
                                                                         std::size_t(extent.width) * 3);
        const std::filesystem::path target = dir_ / (L"capture-" + std::to_wstring(job.request) + L".png");
        std::filesystem::path part = target;
        part += L".part";
        {
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
            if (!out) {
                std::fprintf(stderr, "[rt] capture: request %llu: cannot write %s\n",
                             static_cast<unsigned long long>(job.request), utf8(part).c_str());
                return;
            }
        }
        if (!MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            std::fprintf(stderr, "[rt] capture: request %llu: cannot rename %s (%lu)\n",
                         static_cast<unsigned long long>(job.request), utf8(part).c_str(), GetLastError());
            return;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::fprintf(stderr, "[rt] capture: request %llu: frame %llu (%ux%u) saved as %s in %.0f ms\n",
                     static_cast<unsigned long long>(job.request), static_cast<unsigned long long>(frame), extent.width,
                     extent.height, utf8(target).c_str(), ms);
    }

    std::filesystem::path dir_;
    std::filesystem::path request_file_;
    std::atomic<std::uint64_t> requested_{0};  // the newest number read from the request file
    std::uint64_t taken_ = 0;                  // the same, for the capture thread
    std::atomic<std::uint64_t> armed_{0};      // the newest asked of the presenter (present threads)
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    bool stop_ = false;
    std::thread thread_;
};

// The present handler's context: the device proxy, and what the first PresentX makes.
class TitlePresentation {
public:
    explicit TitlePresentation(void* device) : device_(device), capture_(FrameCapture::from_environment()) {}

    HRESULT present(ID3D12CommandQueue* queue, std::uint32_t plane_count, const void* planes_word,
                    const void* params_word) {
        const auto* planes = static_cast<const XboxPresentPlaneParameters*>(planes_word);
        const auto* params = static_cast<const XboxPresentParameters*>(params_word);
        XboxPresentation* presentation = presentation_.load(std::memory_order_acquire);
        if (!presentation) presentation = ready(queue, plane_count, planes);
        if (!presentation) return S_OK;  // nothing can be shown (logged once by ready)
        if (capture_) capture_->before_present(presentation->presenter());
        const HRESULT hr = presentation->PresentX(plane_count, planes, params);
        if (SUCCEEDED(hr)) {
            window_->frame_presented();
            return hr;
        }
        // V queue.rs:757-841 answers S_OK whatever it showed; a console title does not expect PresentX to fail.
        if (failures_logged_.fetch_add(1, std::memory_order_relaxed) < 8)
            std::fprintf(stderr, "[rt] present: PresentX with %u plane(s) was not shown (0x%08lx); answered S_OK\n",
                         plane_count, static_cast<unsigned long>(hr));
        return S_OK;
    }

private:
    XboxPresentation* ready(ID3D12CommandQueue* queue, std::uint32_t plane_count,
                            const XboxPresentPlaneParameters* planes) {
        std::lock_guard lock(setup_);
        if (XboxPresentation* made = presentation_.load(std::memory_order_acquire)) return made;
        if (failed_) return nullptr;
        failed_ = true;  // until everything below is made
        // V host.rs `presenter`: the window has the first plane's size (the display mode for a blank first present).
        const leonida::DisplayMode mode = title_display_mode();
        std::uint32_t width = mode.width, height = mode.height;
        if (plane_count && planes && planes[0].ResourceCount && planes[0].ppResources && planes[0].ppResources[0]) {
            const D3D12_RESOURCE_DESC desc = planes[0].ppResources[0]->GetDesc();
            if (desc.Width && desc.Height && desc.Width <= 16384 && desc.Height <= 16384) {
                width = std::uint32_t(desc.Width);
                height = desc.Height;
            }
        }
        // LEONIDA_ASPECT: a window the runtime makes is that wide too, as the title's own is (title_window.cpp).
        if (const double ratio = title_aspect_ratio(); ratio > 0.0) width = leonida::aspect::widen({width, height}, ratio).width;
        const HWND title = title_window();
        window_ = title ? GameWindow::adopt(title) : GameWindow::create(width, height);
        if (!window_) {
            std::fprintf(stderr, "[rt] present: no window: the title's frames are not shown\n");
            return nullptr;
        }
        std::unique_ptr<WindowPresenter> presenter =
            create_window_presenter(static_cast<ID3D12Device*>(device_), window_->hwnd(), queue);
        if (!presenter) {
            std::fprintf(stderr, "[rt] present: no swapchain on the window: the title's frames are not shown\n");
            return nullptr;
        }
        if (capture_ && !presenter->enable_snapshots())
            std::fprintf(stderr, "[rt] capture: this swapchain cannot be copied from: no screenshots\n");
        owned_ = std::make_unique<XboxPresentation>(std::move(presenter));
        void* const device = device_;
        owned_->use_device_frame_clock([device] { return d3d12_xs::device_frames_begun(device); });
        failed_ = false;
        presentation_.store(owned_.get(), std::memory_order_release);
        std::fprintf(stderr, "[rt] present: the title's frames go to its window (swapchain %ux%u), paced by the device's "
                             "frame clock\n",
                     owned_->presenter().extent().width, owned_->presenter().extent().height);
        return owned_.get();
    }

    void* const device_;
    std::unique_ptr<FrameCapture> capture_;
    std::mutex setup_;
    bool failed_ = false;
    std::unique_ptr<GameWindow> window_;
    std::unique_ptr<XboxPresentation> owned_;
    std::atomic<XboxPresentation*> presentation_{nullptr};
    std::atomic<int> failures_logged_{0};
};

HRESULT title_present(void* context, ID3D12CommandQueue* queue, std::uint32_t plane_count, const void* planes,
                      const void* parameters) {
    return static_cast<TitlePresentation*>(context)->present(queue, plane_count, planes, parameters);
}

}  // namespace

bool attach_title_presentation(void* device) {
    void* const proxy = d3d12_xs::device_proxy_primary(device);
    if (!proxy) {
        std::fprintf(stderr, "[rt] present: %p is not a device proxy; PresentX is not shown\n", device);
        return false;
    }
    // Lives as long as the process: the device proxy keeps a pointer to it and its threads run to the end.
    auto* presentation = new (std::nothrow) TitlePresentation(proxy);
    if (!presentation) return false;
    if (!d3d12_xs::set_present_handler(proxy, &title_present, presentation)) {
        delete presentation;
        return false;
    }
    std::fprintf(stderr, "[rt] present: PresentX (console queue slot 32) is shown in a window, opened at the first "
                         "frame\n");
    return true;
}

}  // namespace rt::d3d12
