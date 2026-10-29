#include "OverlayRenderer.hpp"

#include <medicon/platform/OverlayApi.h>
#include <medicon/platform/MouseCaptureController.hpp>
#include <medicon/platform/RawMouseWheel.hpp>
#include <medicon/platform/RelativeMousePointer.hpp>
#include <medicon/platform/MenuInputValidation.hpp>
#include <medicon/platform/OverlayInput.hpp>
#include <medicon/client/Client.hpp>
#include <medicon/gui/Ime.hpp>
#include <medicon/gui/Log.hpp>
#include <medicon/modules/ClickGuiModule.hpp>
#include <medicon/modules/AimbotModule.hpp>
#include <medicon/modules/KillauraModule.hpp>
#include <medicon/modules/RotationsModule.hpp>
#include <medicon/modules/SprintModule.hpp>
#include <medicon/modules/CustomModelProbeModule.hpp>
#include <medicon/sdk/BedrockChat.hpp>
#include <medicon/sdk/BedrockInput.hpp>
#include <medicon/sdk/BedrockGameplay.hpp>
#include <medicon/sdk/BedrockMenu.hpp>
#include <medicon/sdk/VanillaNavigation.hpp>
#include <medicon/sdk/CustomModelProbe.hpp>
#include <medicon/sdk/CustomModelBox.hpp>
#include <medicon/sdk/CustomModelAnimation.hpp>
#include <medicon/sdk/CustomModelView.hpp>
#include <medicon/update/HostApi.h>
#include <medicon/social/Service.hpp>
#include <backends/imgui_impl_win32.h>
#include <d3d11.h>
#include <imm.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <fstream>
#include <cfloat>
#include <mutex>
#include <thread>
#include <vector>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace medicon::platform {
namespace {
using Microsoft::WRL::ComPtr;
std::atomic_bool auto_unload_enabled{};
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Resize1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using CreateFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using CreateCoreFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using CreateCompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using ClipFn = BOOL(WINAPI*)(const RECT*);
using WarpFn = BOOL(WINAPI*)(int, int);
using GetCursorFn = BOOL(WINAPI*)(LPPOINT);
using ShowCursorFn = int(WINAPI*)(BOOL);
using ForegroundFn = HWND(WINAPI*)();
using RawInputFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using RawBufferFn = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
using KeyStateFn = SHORT(WINAPI*)(int);
using SendPacketFn = void(*)(void*, void*);
using BaseTickFn = void(*)(void*);
using GrabMouseFn = void(*)(void*);
using MainMenuUpdateFn = std::uint32_t(*)(void*);
using YawInterpolationFn = float(__fastcall*)(void*, float);
namespace model_native = sdk::custom_model::bedrock_1_26_52_3;

constexpr GUID queue_guid{0xf6fe7dea, 0x438b, 0x49d6, {0x95, 0xb8, 0xe3, 0x67, 0x45, 0xd4, 0xea, 0xa2}};
constexpr IID core_interop_iid{0x45d64a29, 0xa63e, 0x4cb6, {0xb4, 0x98, 0x57, 0x81, 0xd2, 0x98, 0xcb, 0x4f}};
struct CoreWindowInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_WindowHandle(HWND*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MessageHandled(BOOL) = 0;
};

struct ContextScope {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    explicit ContextScope(ImGuiContext* context) { ImGui::SetCurrentContext(context); }
    ~ContextScope() { ImGui::SetCurrentContext(previous); }
};
thread_local bool inside_overlay{};
thread_local bool inside_present{};
thread_local bool inside_navigation{};
thread_local bool inside_gameplay{};
thread_local bool inside_model_probe{};
struct FlagScope {
    bool& flag;
    bool previous;
    explicit FlagScope(bool& value) : flag(value), previous(value) { flag = true; }
    ~FlagScope() { flag = previous; }
};
struct InputMessage { HWND window; UINT message; WPARAM wparam; LPARAM lparam; std::uint64_t menu_epoch; std::uint64_t opening_request{}; };
struct InputCapture { std::uint64_t epoch; bool menu; bool open; bool movement; };
struct WindowBinding { HWND window; WNDPROC original; bool restored{}; };
struct QueueCandidate { ComPtr<ID3D12CommandQueue> queue; ComPtr<IUnknown> device; DWORD thread; ULONGLONG last_seen; };

bool window_focused(HWND window) noexcept {
    const auto foreground = GetForegroundWindow();
    return window && foreground && GetAncestor(window, GA_ROOT) == GetAncestor(foreground, GA_ROOT);
}

// Diagnostic only. In-game cost was never measured; summarize where each
// game frame goes into inject.log so a slowdown can be attributed.
struct Cost {
    std::atomic<std::uint64_t> ticks{}, max{}, count{};
    void add(std::uint64_t value) noexcept {
        ticks.fetch_add(value, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
        auto seen = max.load(std::memory_order_relaxed);
        while (value > seen && !max.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {}
    }
    std::string take(const char* name) {
        const auto n = count.exchange(0, std::memory_order_relaxed);
        const auto total = ticks.exchange(0, std::memory_order_relaxed);
        const auto peak = max.exchange(0, std::memory_order_relaxed);
        char text[160]{};
        std::snprintf(text, sizeof text, " %s n=%llu avg=%.3f max=%.3f sum=%.1f", name,
                      static_cast<unsigned long long>(n), n ? perf_ms(total) / static_cast<double>(n) : 0.0,
                      perf_ms(peak), perf_ms(total));
        return text;
    }
};
struct Perf {
    Cost interval, present_call, draw, menu, fonts, rebuild, build, submit;
    Cost wndproc, wndproc_menu, exec;
    std::atomic<std::uint64_t> wm_input{}, last_present{}, window_start{};
    std::atomic<DWORD> present_thread{}, window_thread{};
};

class Runtime {
public:
    // Before any window is subclassed, so the window thread never sees it change.
    Runtime() { ime.set_reporter([this](std::string_view message) { report(message); }); }

    std::atomic_uint status{};
    std::atomic_bool stopping{true};
    std::atomic_bool gui_open{};
    std::atomic_bool modal_open{}, movement_enabled{};
    std::atomic_bool social_available{};
    // Reserve input on the window thread as soon as an opening key arrives,
    // before Present can draw the screen. Low bits: 1 ClickGUI, 2 Social.
    std::atomic<std::uint64_t> opening_input{};
    std::uint64_t next_opening_request{}; // Window thread only
    // Keep the cursor hooks engaged until the window thread restores the
    // game's physical cursor baseline, even after Present closes the GUI.
    std::atomic_bool cursor_owned{};
    std::atomic_uint owned_mouse_buttons{};
    std::atomic_bool menu_visible{};
    std::atomic<std::uint64_t> menu_action_pending{0}; // The owning request's epoch, or no request.
    std::atomic<std::uint64_t> menu_epoch{0};
    std::uint64_t applied_menu_epoch = 0; // Present thread, under render_mutex
    InputPressOwnership<256> owned_keys; // Window thread only
    InputPressOwnership<256> owned_raw_keys;
    InputPressOwnership<5> owned_raw_buttons;
    std::array<bool, 5> raw_buttons_down{}; // Window thread. Hardware capture only.
    std::atomic_int open_key{VK_INSERT};
    std::atomic_uint callbacks{};
    std::atomic_uint feedback_count{};
    std::atomic_bool mouse_ready{}, mouse_active{}, mouse_message_pending{};
    std::atomic<HWND> mouse_window{};
    const UINT mouse_message = RegisterWindowMessageW(L"Medicon.MouseCaptureSync.8494B7C3");
    const UINT menu_action_message = RegisterWindowMessageW(L"Medicon.MainMenuAction.8494B7C3");
    const UINT cursor_message = RegisterWindowMessageW(L"Medicon.CursorPositionSync.8494B7C3");
    std::atomic_bool cursor_message_pending{};
    std::mutex lifecycle_mutex, queue_mutex, input_mutex, log_mutex, window_mutex, command_mutex, feedback_mutex;
    // Timed so a game tick can wait out one Present instead of being skipped.
    std::recursive_timed_mutex render_mutex;
    std::mutex feedback_dispatch_mutex;
    std::mutex mouse_mutex;
    std::mutex menu_mutex;
    HookManager hooks;
    HookManager chat_hooks;
    HookManager tick_hooks;
    HookManager mouse_hooks;
    HookManager native_input_hooks;
    sdk::input::MouseFeedFn mouse_feed{};
    sdk::input::LookInputFn look_input{};
    bool native_input_registered{};
    std::atomic_bool native_input_ready{};
    HookManager navigation_hooks;
    HookManager rotation_hooks;
    HookManager model_hooks;
    HookManager model_box_hooks;
    HookManager model_static_hooks;
    medicon::custom_model::ModelRegistry model_assets;
    sdk::custom_model::TextureGetFn model_texture_get{};
    bool model_static_registered{};
    sdk::custom_model::Probe model_probe;
    sdk::custom_model::BoxRenderer model_box;
    sdk::custom_model::WorldRenderFn model_world_render{};
    sdk::custom_model::MeshImmediateFn model_mesh_immediate{};
    sdk::custom_model::MeshImmediateViewFn model_mesh_view{};
    sdk::custom_model::MeshSubmitFn model_native_submit{};
    bool model_box_registered{};
    model_native::NearActorQueueFn model_near_queue{};
    model_native::EcsActorQueueFn model_ecs_queue{};
    model_native::DataDrivenRenderFn model_data_render{};
    bool model_registered{}; // Bootstrap worker; original retained across stop/start.
    // Present-thread diagnostic samples, excluding intervals crossing ON/OFF.
    std::array<double, 2> model_sample_seconds{};
    std::array<std::uint64_t, 2> model_sample_frames{};
    bool model_previous_enabled{}, model_has_interval{};
    // Present thread: inject.log is all a remote player can send, so each
    // model load outcome and the first custom draw are written there once.
    medicon::custom_model::ModelRegistry::Status model_logged_status{};
    bool model_logged_drawing{}, model_logged_wait{};
    ULONGLONG model_wait_since{};
    sdk::BedrockChat bedrock_chat;
    sdk::BedrockGameplay gameplay;
    YawInterpolationFn body_yaw_interpolator{};
    YawInterpolationFn head_yaw_interpolator{};
    std::atomic_bool rotation_override_enabled{};
    std::atomic<std::uintptr_t> rotation_override_actor{};
    std::atomic<std::uint64_t> rotation_override_yaws{};
    std::atomic<std::uint64_t> rotation_override_pitches{};
    std::atomic<std::uint64_t> rotation_visual_angles{};
    std::atomic_bool rotation_visual_ready{};
    std::atomic<std::uintptr_t> rotation_override_render_component{};
    std::atomic<std::uintptr_t> rotation_override_component{};
    std::atomic<sdk::custom_model::ViewMode> rotation_view_mode{sdk::custom_model::ViewMode::unknown};
    std::atomic_bool sprint_packet_override_enabled{};
    std::atomic_bool sprint_packet_requested{};
    std::atomic_bool sprint_packet_route_reported{};
    std::atomic_bool sprint_packet_layout_error_reported{};
    std::atomic<ULONGLONG> pitch_render_diagnostic_last{};
    bool rotation_getter_identity_reported{}; // Game thread.
    ULONGLONG killaura_rotation_diagnostic_last{}; // Game thread, under render_mutex.
    ULONGLONG pitch_diagnostic_last{}; // Game thread, under render_mutex.
    unsigned pitch_diagnostic_count{};
    bool rotation_visual_initialized{}; // Present thread, under render_mutex.
    std::uintptr_t rotation_visual_actor{};
    std::chrono::steady_clock::time_point rotation_visual_updated{};
    std::atomic_bool gameplay_ready{false};
    std::chrono::steady_clock::time_point last_game_tick{}; // Under render_mutex.
    std::string gameplay_diagnostic; // Under render_mutex.
    sdk::BedrockMenu bedrock_menu;
    sdk::VanillaNavigation navigation;
    std::atomic_bool navigation_ready{false};
    bool navigation_registered{}; // Bootstrap worker only; callbacks use navigation_ready.
    MainMenuUpdateFn main_menu_update{};
    std::atomic<DWORD> navigation_thread{0};
    std::string menu_diagnostic;
    // Set by Medicon_Initialize when MediconLoader hosts the Core: the version
    // that was just downloaded, so Social greets with an "updated" notice.
    std::string update_version;
    // Set by Medicon_Initialize: the newest Core on the update server, as the
    // loader read it from the signed manifest (else the hosted Core's own
    // version). Empty without the loader: the Watermark keeps the build's.
    std::string server_version;
    // Set by Medicon_Initialize: the release this Core was published as.
    // Empty when the DLL was injected directly.
    std::string core_version;
    sdk::MenuScreenState cached_menu_screen_{sdk::MenuScreenState::unknown};
    sdk::MenuSessionIdentity cached_menu_session_{};
    void* cached_menu_client_{};
    ULONGLONG cached_menu_tick_{};
    std::atomic_bool window_iconic{false};
    std::atomic_bool window_gone{false};
    UINT overlay_display_w{};
    UINT overlay_display_h{};
    SendPacketFn send_packet{};
    BaseTickFn base_tick{};
    GrabMouseFn grab_mouse{};
    bool mouse_registered{};
    // Only the window owner thread accesses these transition fields.
    MouseCaptureController mouse_capture;
    // Accessed on the presentation thread under render_mutex.
    MouseSyncSchedule mouse_sync;
    // Published for the grab hook, which can run on the game thread.
    std::atomic<std::uintptr_t> mouse_identity{0};
    std::atomic_bool window_focused_cached{false};
    std::atomic<long> spoof_x{0};
    std::atomic<long> spoof_y{0};
    std::atomic_bool spoof_valid{false};
    HWND mouse_owner{};
    bool win32_mouse_open{};
    std::string mouse_error;
    bool chat_installed{};
    bool chat_registered{};
    bool tick_installed{};
    bool tick_registered{};
    std::string feedback_error;
    HANDLE worker{};
    std::atomic_bool eject_requested{};
    std::atomic<HANDLE> eject_worker{};
    bool window_callbacks_restored{true};
    bool installed{};
    std::ofstream log;
    std::string last_error;
    std::vector<QueueCandidate> queues;
    std::deque<InputMessage> messages;
    std::deque<std::string> pending_commands;
    std::deque<std::string> pending_feedback;
    std::vector<WindowBinding> windows;
    std::unique_ptr<Client> client;
    // Assigned and reset under render_mutex; the client's overlay holds its channel.
    std::unique_ptr<social::SocialService> social;
    std::unique_ptr<OverlayRenderer> renderer;
    ImGuiContext* context{};
    IDXGISwapChain* active_swap{}; // identity only, never dereferenced outside Present/Resize
    HWND window{};
    bool platform_ready{};
    bool clip_saved{};
    RECT saved_clip{};
    std::filesystem::path config_path;
    ULONGLONG retry_at{};

    PresentFn present{};
    Present1Fn present1{};
    ResizeFn resize{};
    Resize1Fn resize1{};
    ExecuteFn execute{};
    CreateFn create{};
    CreateHwndFn create_hwnd{};
    CreateCoreFn create_core{};
    CreateCompositionFn create_composition{};
    ClipFn clip{};
    WarpFn warp{};
    GetCursorFn get_cursor{};
    ShowCursorFn show_cursor{};
    ForegroundFn get_foreground{};
    RawInputFn get_raw_input{};
    RawBufferFn get_raw_buffer{};
    KeyStateFn get_async_key_state{}, get_key_state{};

    std::atomic<std::uint64_t> client_cursor{0};
    std::atomic_bool client_cursor_valid{false};
    // Window thread only. -1 unknown, 0 legacy mouse messages exist, 1 RIDEV_NOLEGACY.
    int raw_mouse_nolegacy = -1;
    RelativeMousePointer relative_menu_pointer;
    std::uint64_t relative_pointer_epoch = 0; // Window thread only
    int os_cursor_show_boost = 0;
    bool hw_cursor_valid = false;
    bool ime_composing = false; // Window thread only.
    bool ime_escape_owned = false;
    // Social text inputs' IME (spec 4.8). Lives as long as the runtime, which the
    // window procedure may still call after the client is gone.
    gui::ImeBridge ime;
    int hw_cursor_x = 0;
    int hw_cursor_y = 0;
    Perf perf;
    RenderCounters render_counters; // Present thread, under render_mutex

    void report_perf(std::uint64_t now) {
        auto start = perf.window_start.load(std::memory_order_relaxed);
        if (!start) { perf.window_start.compare_exchange_strong(start, now); return; }
        const double elapsed = perf_ms(now - start);
        if (elapsed < 2000.0 || !perf.window_start.compare_exchange_strong(start, now)) return;
        const auto frames = perf.interval.count.load(std::memory_order_relaxed);
        RenderCounters counters;
        gui::SocialUi::Timing social_timing;
        {
            std::lock_guard lock(render_mutex);
            counters = std::exchange(render_counters, {});
            if (client) social_timing = client->overlay().social().take_timing();
        }
        char head[200]{};
        std::snprintf(head, sizeof head, "Perf %.1fs: fps=%.1f present_thread=%lu window_thread=%lu menu=%d gui=%d |",
                      elapsed / 1000.0, frames * 1000.0 / elapsed,
                      static_cast<unsigned long>(perf.present_thread.load()),
                      static_cast<unsigned long>(perf.window_thread.load()),
                      menu_visible.load() ? 1 : 0, gui_open.load() ? 1 : 0);
        char tail[200]{};
        std::snprintf(tail, sizeof tail, " | effects n=%u avg=%.3f max=%.3f gpu_skipped=%u | wm_input=%llu",
                      counters.effects_frames,
                      counters.effects_frames ? perf_ms(counters.effects_ticks) / counters.effects_frames : 0.0,
                      perf_ms(counters.effects_max), counters.skipped_frames,
                      static_cast<unsigned long long>(perf.wm_input.exchange(0)));
        // Present only with social.json "debugTiming" (spec 4.9); microseconds per frame.
        char social[96]{};
        if (social_timing.frames) {
            std::snprintf(social, sizeof social, " | social n=%u avg=%.1fus max=%.1fus", social_timing.frames,
                          social_timing.total_us / social_timing.frames, social_timing.max_us);
        }
        report(std::string(head) + perf.interval.take("interval") + perf.present_call.take("Present()") +
               perf.draw.take("draw") + perf.menu.take("draw.menu") + perf.fonts.take("draw.fonts") +
               perf.rebuild.take("draw.font_rebuild") + perf.build.take("draw.imgui") +
               perf.submit.take("draw.submit") + tail + perf.wndproc.take("wndproc_own") +
               perf.wndproc_menu.take("wndproc.menu") + perf.exec.take("ExecuteCommandLists") + social);
    }

    void publish_focus(HWND target) noexcept {
        window_focused_cached.store(window_focused(target), std::memory_order_release);
    }
    void publish_client_cursor(long x, long y) noexcept {
        const auto packed = static_cast<std::uint32_t>(x) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) << 32);
        client_cursor.store(packed, std::memory_order_relaxed);
        client_cursor_valid.store(true, std::memory_order_release);
    }
    bool read_client_cursor(long& x, long& y) const noexcept {
        if (!client_cursor_valid.load(std::memory_order_acquire)) return false;
        const auto packed = client_cursor.load(std::memory_order_relaxed);
        x = static_cast<long>(static_cast<std::uint32_t>(packed));
        y = static_cast<long>(static_cast<std::uint32_t>(packed >> 32));
        return true;
    }
    void clear_client_cursor() noexcept {
        client_cursor_valid.store(false, std::memory_order_release);
    }
    // True only while the ClickGUI should own the cursor. Hooks must use this
    // cache: GetForegroundWindow from inside ClipCursor/SetCursorPos stalls the
    // render thread against the window thread and drops FPS while the GUI is open.
    bool cursor_captured_by_gui() const noexcept {
        return !stopping.load(std::memory_order_acquire) &&
               (input_open() || cursor_owned.load(std::memory_order_acquire)) &&
               window_focused_cached.load(std::memory_order_acquire);
    }
    bool input_open() const noexcept {
        return gui_open.load(std::memory_order_acquire) || menu_visible.load(std::memory_order_acquire) ||
            opening_input.load(std::memory_order_acquire) != 0;
    }
    bool modal_input() const noexcept {
        return modal_open.load(std::memory_order_acquire) || menu_visible.load(std::memory_order_acquire) ||
            (opening_input.load(std::memory_order_acquire) & 3U) == 2;
    }
    bool blocks_native_mouse() const noexcept {
        // Game callbacks can precede the window's focus notifications. Their
        // input must not depend on a cached foreground-window observation.
        return !stopping.load(std::memory_order_acquire) &&
            (input_open() || cursor_owned.load(std::memory_order_acquire));
    }
    bool blocks_key(int key, const InputCapture& capture) const noexcept {
        return key == open_key.load(std::memory_order_acquire) ||
            (key == gui::Overlay::social_key && social_available.load(std::memory_order_acquire)) ||
            (capture.open && !(capture.movement && clickgui_movement_key(key)));
    }
    void publish_client_input() {
        const auto& overlay = client->overlay();
        social_available.store(overlay.social().attached(), std::memory_order_release);
        // The closing Social panel still covers the game and owns its clicks.
        const bool modal = overlay.social().screen_open() || overlay.social().screen_visible();
        const bool modal_changed = modal_open.exchange(modal, std::memory_order_acq_rel) != modal;
        movement_enabled.store(overlay.allows_game_movement(), std::memory_order_release);
        set_open(client->gui().open() || modal);
        if (modal_changed) request_mouse_sync();
    }
    // Resolved by the bootstrap worker before Present is hooked, so the first
    // frame already knows whether the replacement menu can navigate.
    bool navigation_usable() const noexcept {
        return navigation_ready.load(std::memory_order_acquire) && navigation.ready();
    }
    InputCapture input_capture() const noexcept {
        const auto epoch = menu_epoch.load(std::memory_order_acquire);
        const auto opening = opening_input.load(std::memory_order_acquire) & 3U;
        const bool menu = menu_visible.load(std::memory_order_acquire);
        const bool open = menu || gui_open.load(std::memory_order_acquire) || opening || cursor_owned.load(std::memory_order_acquire);
        // A concurrent transition consumes this boundary message and leaves it
        // stamped with the earlier epoch, so Present will discard it.
        if (epoch != menu_epoch.load(std::memory_order_acquire)) return {epoch, true, true, false};
        return {epoch, menu, open, !menu && opening != 2 && !modal_open.load(std::memory_order_acquire) &&
            (opening == 1 || movement_enabled.load(std::memory_order_acquire))};
    }
    void release_opening_input(std::uint64_t request) noexcept {
        if (request) opening_input.compare_exchange_strong(request, 0, std::memory_order_acq_rel);
    }

    void report(std::string_view message, bool failure = false) {
        std::lock_guard lock(log_mutex);
        if (failure) last_error = message;
        SYSTEMTIME time{};
        GetLocalTime(&time);
        char stamp[40]{};
        std::snprintf(stamp, sizeof stamp, "[%02u:%02u:%02u] ", time.wHour, time.wMinute, time.wSecond);
        if (log) { log << stamp << message << '\n'; log.flush(); }
        std::string output = "[Medicon] ";
        output.append(message).push_back('\n');
        OutputDebugStringA(output.c_str());
    }

    void capture_queue(ID3D12CommandQueue* queue) {
        if (!queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
        std::lock_guard lock(queue_mutex);
        for (auto& entry : queues) {
            if (entry.queue.Get() == queue) {
                entry.thread = GetCurrentThreadId();
                entry.last_seen = GetTickCount64();
                return;
            }
        }
        ComPtr<IUnknown> device;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) return;
        if (queues.size() == 32) queues.erase(queues.begin());
        queues.push_back({queue, std::move(device), GetCurrentThreadId(), GetTickCount64()});
    }

    void associate_queue(IDXGISwapChain* swap, IUnknown* source) {
        if (!swap || !source) return;
        ComPtr<ID3D12CommandQueue> queue;
        if (SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&queue)))) {
            swap->SetPrivateDataInterface(queue_guid, queue.Get());
            capture_queue(queue.Get());
        }
    }

    ComPtr<ID3D12CommandQueue> find_queue(IDXGISwapChain* swap, ID3D12Device* device) {
        ComPtr<ID3D12CommandQueue> associated;
        UINT size = sizeof(ID3D12CommandQueue*);
        if (SUCCEEDED(swap->GetPrivateData(queue_guid, &size, associated.GetAddressOf())) && associated) return associated;
        ComPtr<IUnknown> identity;
        device->QueryInterface(IID_PPV_ARGS(&identity));
        std::lock_guard lock(queue_mutex);
        QueueCandidate* same_thread = nullptr;
        QueueCandidate* only = nullptr;
        unsigned matches = 0, thread_matches = 0;
        for (auto& entry : queues) {
            if (entry.device.Get() != identity.Get() || GetTickCount64() - entry.last_seen > 5000) continue;
            only = &entry;
            ++matches;
            if (entry.thread == GetCurrentThreadId()) { same_thread = &entry; ++thread_matches; }
        }
        // Late injection cannot query the presentation queue through DXGI. Use
        // only an unambiguous direct queue on this device, preferring this thread.
        if (thread_matches == 1) return same_thread->queue;
        if (matches == 1) return only->queue;
        return {};
    }

    void set_open(bool open) {
        gui_open = open;
        // Discovery validates foreign objects and native code. Keep it off the
        // per-Present path while still detecting session changes/retrying failed
        // transitions. The native grab hook prevents recapture between polls;
        // opening, closing and window-focus events still synchronize immediately.
        if (mouse_sync.update(input_open(), mouse_active, GetTickCount64())) request_mouse_sync();
    }

    sdk::MenuScreenState menu_screen(bool force = false) {
        // Both Present and the window thread use this read-only bridge. Never
        // reuse a game object between observations or call a string-returning
        // native getter with our toolchain's STL ABI.
        std::lock_guard lock(menu_mutex);
        const auto client = bedrock_chat.mouse_client();
        const auto now = GetTickCount64();
        if (!force && cached_menu_client_ == client && cached_menu_tick_ != 0) {
            const auto age = now - cached_menu_tick_;
            // A click on the window thread must reuse Present's observation.
            // A fresh poll here takes the menu lock and stalls the message pump.
            const ULONGLONG budget = cached_menu_screen_ == sdk::MenuScreenState::main_menu ? 32 : 50;
            if (age <= budget) return cached_menu_screen_;
        }
        std::string diagnostic;
        sdk::MenuSessionIdentity session;
        auto screen = bedrock_menu.poll(client, diagnostic, &session);
        // The replacement blocks native input, and only the native route makes
        // its buttons do anything. Detection alone accepts executables the
        // navigation profile rejects (same FileVersion, different packaging),
        // which left a menu where no button responded. Keep the vanilla title.
        if (screen == sdk::MenuScreenState::main_menu && !navigation_usable()) {
            screen = sdk::MenuScreenState::other;
            session = {};
            diagnostic = "Main Menu replacement withheld: native navigation is unavailable; "
                         "the vanilla title screen keeps its input.";
        }
        if (diagnostic != menu_diagnostic) {
            if (!diagnostic.empty()) report("MainMenu: " + diagnostic);
            menu_diagnostic = std::move(diagnostic);
        }
        // start_screen can be replaced by another start_screen without a frame
        // observing an intermediate screen. Cancel presses and posted actions
        // when the owning objects change even though the visible bool stays true.
        if (menu_visible.load(std::memory_order_acquire) && cached_menu_session_ != session) {
            advance_menu_epoch();
        }
        cached_menu_session_ = session;
        cached_menu_screen_ = screen;
        cached_menu_client_ = client;
        cached_menu_tick_ = now;
        return screen;
    }

    void publish_menu_visible(bool visible) noexcept {
        if (menu_visible.exchange(visible, std::memory_order_acq_rel) == visible) return;
        advance_menu_epoch();
        if (!visible) {
            clear_client_cursor();
        }
        request_mouse_sync();
    }

    void advance_menu_epoch() noexcept {
        const auto retired_epoch = menu_epoch.fetch_add(1, std::memory_order_acq_rel);
        navigation.cancel(retired_epoch);
        auto owner = menu_action_pending.load(std::memory_order_acquire);
        // Another thread can already have queued for the newly published epoch.
        // Retire old owners only; never erase that newer request's ownership.
        while (owner && owner <= retired_epoch &&
               !menu_action_pending.compare_exchange_weak(owner, 0, std::memory_order_acq_rel)) {}
    }

    void release_menu_action(std::uint64_t epoch) noexcept {
        // An old window message may finish after a new session queued an action.
        // Release only the request that owns this epoch.
        if (epoch) menu_action_pending.compare_exchange_strong(epoch, 0, std::memory_order_acq_rel);
    }

    void request_mouse_sync() noexcept {
        const auto target = mouse_window.load();
        if (!target || !mouse_message || mouse_message_pending.exchange(true)) return;
        if (!PostMessageW(target, mouse_message, 0, 0)) mouse_message_pending = false;
    }
    void request_cursor_sample() noexcept {
        const auto target = mouse_window.load(std::memory_order_acquire);
        if (!target || !cursor_message || cursor_message_pending.exchange(true)) return;
        if (!PostMessageW(target, cursor_message, 0, 0)) cursor_message_pending = false;
    }
    bool sync_mouse(HWND target, bool reset);

    void suspend_menu() {
        publish_menu_visible(false);
        set_open(false);
        if (client) client->overlay().set_main_menu_active(false);
    }

    void release_graphics() {
        if (client) client->overlay().set_texture_host(nullptr);
        suspend_menu();
        if (!context) return;
        ContextScope scope(context);
        std::string error;
        if (renderer && !renderer->wait_idle(error)) report(error, true);
        renderer.reset();
        active_swap = nullptr;
        overlay_display_w = overlay_display_h = 0;
    }

    void teardown() {
        set_open(false);
        release_graphics();
        if (context) {
            ContextScope scope(context);
            if (client && !client->stop()) report(client->last_error(), true);
            client.reset();
            if (platform_ready) ImGui_ImplWin32_Shutdown();
            platform_ready = false;
        }
        if (context) { ImGui::DestroyContext(context); context = nullptr; }
        window = nullptr;
        gameplay.clear();
        last_game_tick = {};
        gameplay_diagnostic.clear();
        model_probe.reset(); // Stop has already disabled the hook and drained callbacks.
        model_box.set_options(false,0,1);
        model_box.set_ready(false);
        model_box.release_resources();
        model_assets.stop();
        (void)model_box.take_frame();
        model_sample_seconds = {};
        model_sample_frames = {};
        model_has_interval = false;
        model_logged_status = {};
        model_logged_drawing = false;
        model_logged_wait = false;
        model_wait_since = 0;
        std::lock_guard lock(input_mutex);
        messages.clear();
    }

    bool attach_window(HWND target);
    bool draw(IDXGISwapChain* swap, UINT flags);
    void before_resize(IDXGISwapChain* swap) {
        std::lock_guard lock(render_mutex);
        FlagScope submission(inside_overlay);
        if (active_swap == swap) {
            release_graphics();
            retry_at = 0;
            status = 2;
            report("Swap chain resized; overlay GPU resources released.");
        }
    }
    bool install(std::string& error);
    void connect_chat();
    void connect_chat_feedback();
    void connect_mouse();
    void connect_native_input();
    void connect_navigation();
    void connect_rotation();
    void connect_model_probe();
    void connect_model_box();
    void connect_model_static();
    void draw_model_probe();
};

// Intentionally process-lifetime. Never destroy hook trampolines, C++ locks or
// GPU objects from DllMain during process teardown.
Runtime& runtime() { static Runtime* instance = new Runtime; return *instance; }
struct CallbackScope {
    Runtime& state = runtime();
    CallbackScope() { state.callbacks.fetch_add(1); }
    ~CallbackScope() { state.callbacks.fetch_sub(1); }
};

DWORD WINAPI eject_overlay_worker(void*) {
    auto& state = runtime();
    DWORD retry_delay = 250;
    for (;;) {
        try {
            if (MediconStopOverlay()) return 0;
        } catch (const std::exception& error) {
            try { state.report(std::string("Safe eject failed: ") + error.what(), true); }
            catch (...) { OutputDebugStringA("[Medicon] Safe eject failed; see debugger output.\n"); }
        } catch (...) {
            try { state.report("Safe eject failed with an unknown error.", true); }
            catch (...) { OutputDebugStringA("[Medicon] Safe eject failed; see debugger output.\n"); }
        }
        Sleep(retry_delay);
        retry_delay = retry_delay >= 2500 ? 5000 : retry_delay * 2;
    }
    return 1;
}

bool request_eject(Runtime& state) {
    bool expected = false;
    if (!state.eject_requested.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return true;
    HANDLE thread = CreateThread(nullptr, 0, eject_overlay_worker, nullptr, 0, nullptr);
    if (thread) {
        state.eject_worker.store(thread, std::memory_order_release);
        return true;
    }
    state.eject_requested.store(false, std::memory_order_release);
    const DWORD error = GetLastError();
    try {
        state.report("Could not start the safe-eject worker (Windows error " +
                     std::to_string(error) + ").", true);
    } catch (...) {
        OutputDebugStringA("[Medicon] Could not start the safe-eject worker.\n");
    }
    return false;
}

void cache_client_cursor(Runtime& state, HWND window);
void reset_relative_cursor(Runtime& state) noexcept;
void reveal_os_cursor(Runtime& state);
void conceal_os_cursor(Runtime& state, bool restore_arrow);
void warp_os_cursor(Runtime& state, HWND window, int x, int y);
bool clip_cursor_to_client(Runtime& state, HWND window);
bool raw_mouse_suppresses_legacy();
void present_safe_imgui_new_frame(IDXGISwapChain* swap);

bool Runtime::sync_mouse(HWND target, bool reset) {
    // Native capture may synchronously reenter WndProc. Let a later posted
    // update retry instead of waiting recursively on our own transition.
    std::unique_lock lock(mouse_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    if (target != mouse_window) return true;
    if (mouse_owner != target) {
        mouse_capture.reset(false, {});
        mouse_identity.store(0, std::memory_order_release);
        spoof_valid.store(false, std::memory_order_release);
        cursor_owned.store(false, std::memory_order_release);
        conceal_os_cursor(*this, false);
        reset_relative_cursor(*this);
        win32_mouse_open = clip_saved = false;
        mouse_owner = target;
    }
    const bool focused = window_focused(target);
    window_focused_cached.store(focused, std::memory_order_release);
    const bool open = !reset && !stopping && input_open();
    if (open && focused && !win32_mouse_open) {
        // Save the game's baseline before any native release or UI warp.
        POINT frozen{};
        if (get_cursor && get_cursor(&frozen)) {
            spoof_x.store(frozen.x, std::memory_order_relaxed);
            spoof_y.store(frozen.y, std::memory_order_relaxed);
            spoof_valid.store(true, std::memory_order_release);
        }
        cursor_owned.store(true, std::memory_order_release);
    }
    const auto cursor_epoch = menu_epoch.load(std::memory_order_acquire);
    if (reset || !open || !focused || relative_pointer_epoch != cursor_epoch) {
        reset_relative_cursor(*this);
        relative_pointer_epoch = cursor_epoch;
    }
    // Capture/ReleaseCapture and native mouse calls must run on this window's
    // input thread, not Present (Minecraft can present on a different thread).
    if (win32_mouse_open && (!open || !focused)) {
        if (GetCapture() == target) ReleaseCapture();
        raw_buttons_down.fill(false);
        if (clip_saved && focused && clip) clip(&saved_clip);
        else if (clip) clip(nullptr);
        conceal_os_cursor(*this, focused);
        clip_saved = false;
        win32_mouse_open = false;
    }
    bool success = true;
    if (mouse_ready) {
        const auto identity = reinterpret_cast<std::uintptr_t>(bedrock_chat.mouse_client());
        if (identity != mouse_identity.load(std::memory_order_acquire)) {
            // Never restore capture through a client from a previous session.
            mouse_capture.reset(false, {});
            mouse_identity.store(identity, std::memory_order_release);
        }
        std::string diagnostic;
        const auto release = [&] { return bedrock_chat.release_mouse(diagnostic); };
        const auto grab = [&] { return bedrock_chat.grab_mouse(diagnostic); };
        if (reset) success = mouse_capture.reset(focused && identity, grab);
        else if (identity) {
            const auto grabbed = bedrock_chat.mouse_grabbed(diagnostic);
            // Releasing the native mouse also disables player movement. Only
            // modal screens need that transition; ClickGUI owns the OS cursor
            // through our Win32/raw-input hooks while the game stays grabbed.
            success = mouse_capture.update(open && (modal_input() || !native_input_ready.load(std::memory_order_acquire)),
                                           focused, grabbed, release, grab);
        }
        mouse_active = mouse_capture.active();
        if (!success && !diagnostic.empty() && diagnostic != mouse_error) {
            report("Native ClickGUI mouse: " + diagnostic, true);
            mouse_error = diagnostic;
        } else if (success) mouse_error.clear();
    }
    if (open && focused) {
        raw_mouse_nolegacy = raw_mouse_suppresses_legacy() ? 1 : 0;
        if (raw_mouse_nolegacy != 1) reset_relative_cursor(*this);
        cache_client_cursor(*this, target);
        if (!win32_mouse_open) {
            clip_saved = GetClipCursor(&saved_clip) != FALSE;
            win32_mouse_open = true;
            RECT bounds{};
            GetClientRect(target, &bounds);
            const int width = bounds.right - bounds.left;
            const int height = bounds.bottom - bounds.top;
            long x = 0, y = 0;
            const bool have = read_client_cursor(x, y);
            const bool outside = !have || x < 0 || y < 0 || x >= width || y >= height;
            if (outside && width > 0 && height > 0) {
                x = width / 2;
                y = height / 2;
                publish_client_cursor(x, y);
            }
            // Confine to the client once. Later game ClipCursor calls are swallowed.
            clip_cursor_to_client(*this, target);
            reveal_os_cursor(*this);
            if (raw_mouse_nolegacy == 1 || outside)
                warp_os_cursor(*this, target, static_cast<int>(x), static_cast<int>(y));
        }
    } else {
        // GetCursorPos must stay frozen until the real OS cursor matches the
        // last position the game requested. Otherwise the first game frame
        // after close interprets the entire GUI drag as relative camera input.
        if (focused && warp && spoof_valid.load(std::memory_order_acquire))
            warp(spoof_x.load(std::memory_order_relaxed), spoof_y.load(std::memory_order_relaxed));
        cursor_owned.store(false, std::memory_order_release);
        spoof_valid.store(false, std::memory_order_release);
        clear_client_cursor();
    }
    return success;
}

bool keyboard_message(UINT message) {
    return message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP;
}
bool mouse_message(UINT message) {
    return message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
}
bool character_message(UINT message) {
    return message == WM_CHAR || message == WM_SYSCHAR || message == WM_DEADCHAR ||
        message == WM_SYSDEADCHAR || message == WM_UNICHAR ||
        (message >= WM_IME_STARTCOMPOSITION && message <= WM_IME_COMPOSITION) ||
        (message >= WM_IME_SETCONTEXT && message <= WM_IME_KEYUP);
}

int mouse_key(UINT message, WPARAM parameters) {
    switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP: return VK_LBUTTON;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_RBUTTONUP: return VK_RBUTTON;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_MBUTTONUP: return VK_MBUTTON;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: case WM_XBUTTONUP:
        return HIWORD(parameters) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
    default: return 0;
    }
}

// Mouse packets are applied on Present without the Win32 backend. That backend
// calls GetCapture/SetCapture/GetMessageExtraInfo, which take the USER lock on
// the render thread and hitch the frame that contains the click.
bool overlay_mouse_message(UINT message) noexcept {
    switch (message) {
    case WM_MOUSEMOVE: case WM_NCMOUSEMOVE: case WM_MOUSELEAVE: case WM_NCMOUSELEAVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        return true;
    default:
        return false;
    }
}

void submit_overlay_mouse(UINT message, WPARAM wparam) {
    auto& io = ImGui::GetIO();
    int button = -1;
    bool down = false;
    switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: button = 0; down = true; break;
    case WM_LBUTTONUP: button = 0; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: button = 1; down = true; break;
    case WM_RBUTTONUP: button = 1; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: button = 2; down = true; break;
    case WM_MBUTTONUP: button = 2; break;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
        button = HIWORD(wparam) == XBUTTON1 ? 3 : 4;
        down = true;
        break;
    case WM_XBUTTONUP:
        button = HIWORD(wparam) == XBUTTON1 ? 3 : 4;
        break;
    case WM_MOUSEWHEEL:
        io.AddMouseWheelEvent(0.0F, static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / static_cast<float>(WHEEL_DELTA));
        return;
    case WM_MOUSEHWHEEL:
        io.AddMouseWheelEvent(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / static_cast<float>(WHEEL_DELTA), 0.0F);
        return;
    default:
        return; // Motion is published from the hardware-cursor cache.
    }
    io.AddMouseButtonEvent(button, down);
}

int normalized_key(WPARAM key, LPARAM parameters) {
    if (key == VK_SHIFT) return static_cast<int>(MapVirtualKeyW((parameters >> 16) & 0xFF, MAPVK_VSC_TO_VK_EX));
    if (key == VK_CONTROL) return (parameters & (1LL << 24)) ? VK_RCONTROL : VK_LCONTROL;
    if (key == VK_MENU) return (parameters & (1LL << 24)) ? VK_RMENU : VK_LMENU;
    return static_cast<int>(key);
}

bool artifact_wheel(UINT message, WPARAM parameters) noexcept {
    return (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) &&
        !genuine_legacy_wheel_delta(static_cast<short>(GET_WHEEL_DELTA_WPARAM(parameters)));
}

void queue_input(Runtime& state, HWND window, UINT message, WPARAM wparam, LPARAM lparam, std::uint64_t epoch,
                 unsigned opening_mode = 0) {
    std::lock_guard lock(state.input_mutex);
    if (state.messages.size() >= 4096) {
        for (const auto& event : state.messages) state.release_opening_input(event.opening_request);
        state.messages.clear();
    }
    const auto request = opening_mode ? (++state.next_opening_request << 2) | opening_mode : 0;
    state.messages.push_back({window, message, wparam, lparam, epoch, request});
    if (request) {
        state.opening_input.store(request, std::memory_order_release);
        state.request_mouse_sync();
    }
}

void reset_relative_cursor(Runtime& state) noexcept {
    state.relative_menu_pointer.reset();
    state.hw_cursor_valid = false;
}

void reveal_os_cursor(Runtime& state) {
    if (state.show_cursor) {
        while (state.os_cursor_show_boost < 32) {
            const int count = state.show_cursor(TRUE);
            ++state.os_cursor_show_boost;
            if (count >= 0) break;
        }
    }
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
}

void conceal_os_cursor(Runtime& state, bool restore_arrow) {
    if (state.show_cursor) {
        while (state.os_cursor_show_boost > 0) {
            state.show_cursor(FALSE);
            --state.os_cursor_show_boost;
        }
    } else {
        state.os_cursor_show_boost = 0;
    }
    SetCursor(restore_arrow ? LoadCursorW(nullptr, IDC_ARROW) : nullptr);
}

void warp_os_cursor(Runtime& state, HWND window, int x, int y) {
    if (state.hw_cursor_valid && state.hw_cursor_x == x && state.hw_cursor_y == y) return;
    if (!state.warp) return;
    POINT screen{x, y};
    if (!ClientToScreen(window, &screen) || !state.warp(screen.x, screen.y)) return;
    state.hw_cursor_x = x;
    state.hw_cursor_y = y;
    state.hw_cursor_valid = true;
}

bool clip_cursor_to_client(Runtime& state, HWND window) {
    if (!state.clip) return false;
    RECT bounds{};
    if (!GetClientRect(window, &bounds)) return false;
    POINT top_left{bounds.left, bounds.top};
    POINT bottom_right{bounds.right, bounds.bottom};
    if (!ClientToScreen(window, &top_left) || !ClientToScreen(window, &bottom_right)) return false;
    RECT screen{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
    return state.clip(&screen) != FALSE;
}

void present_safe_imgui_new_frame(IDXGISwapChain* swap) {
    auto& io = ImGui::GetIO();
    auto& state = runtime();
    // DXGI GetDesc stays on the presentation thread. ImGui_ImplWin32_NewFrame
    // calls GetClientRect/GetForegroundWindow/GetKeyState/XInput, which take
    // the USER lock and hitch against the window thread while the overlay owns
    // input (hardware cursor warps, raw mouse, WM_SETCURSOR).
    if (state.overlay_display_w == 0 || state.overlay_display_h == 0) {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (swap && SUCCEEDED(swap->GetDesc(&desc)) && desc.BufferDesc.Width > 0 && desc.BufferDesc.Height > 0) {
            state.overlay_display_w = desc.BufferDesc.Width;
            state.overlay_display_h = desc.BufferDesc.Height;
        }
    }
    if (state.overlay_display_w > 0 && state.overlay_display_h > 0)
        io.DisplaySize = ImVec2(static_cast<float>(state.overlay_display_w),
                                static_cast<float>(state.overlay_display_h));
    thread_local LARGE_INTEGER frequency{};
    thread_local LARGE_INTEGER last{};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (last.QuadPart != 0 && frequency.QuadPart != 0) {
        const double dt = static_cast<double>(now.QuadPart - last.QuadPart) /
                          static_cast<double>(frequency.QuadPart);
        io.DeltaTime = static_cast<float>(std::clamp(dt, 0.0001, 0.1));
    } else {
        io.DeltaTime = 1.0F / 60.0F;
    }
    last = now;
}

void cache_client_cursor(Runtime& state, HWND window) {
    // In relative mode the OS cursor may be stationary or outside the client.
    // Periodic native-mouse synchronization must not overwrite our accumulator.
    if (state.relative_menu_pointer.active()) return;
    POINT point{};
    const auto query = state.get_cursor ? state.get_cursor : &GetCursorPos;
    // The hooked GetCursorPos returns the game's spoofed center. The GUI needs
    // the real cursor, sampled here so Present never takes the USER lock for it.
    if (!query(&point) || !ScreenToClient(window, &point)) {
        state.clear_client_cursor();
        return;
    }
    state.publish_client_cursor(point.x, point.y);
}

bool raw_mouse_suppresses_legacy() {
#ifndef RIDEV_NOLEGACY
    constexpr DWORD RIDEV_NOLEGACY = 0x00000030;
#endif
    UINT count = 0;
    GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE));
    if (count == 0 || count > 32) return true;
    std::vector<RAWINPUTDEVICE> devices(count);
    const UINT read = GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE));
    if (read == static_cast<UINT>(-1)) return true;
    bool found = false;
    bool suppressed = false;
    for (UINT i = 0; i < read; ++i) {
        if (devices[i].usUsagePage != 0x01 || devices[i].usUsage != 0x02) continue;
        found = true;
        suppressed = suppressed || (devices[i].dwFlags & RIDEV_NOLEGACY) != 0;
    }
    return !found || suppressed;
}

void queue_raw_wheel(Runtime& state, HWND window, UINT message, short delta, std::uint64_t epoch) {
    if (!genuine_legacy_wheel_delta(delta)) return;
    queue_input(state, window, message, MAKEWPARAM(0, static_cast<WORD>(delta)), 0, epoch);
}

bool decode_raw_input(LPARAM lparam, RAWINPUT& raw) noexcept {
    UINT size = sizeof(raw);
    // The overlay needs the original packet; the public API hook neutralizes
    // mouse input for game-side readers, including readers before WndProc.
    const auto query = runtime().get_raw_input ? runtime().get_raw_input : &GetRawInputData;
    const UINT read = query(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER));
    return read != static_cast<UINT>(-1) && read >= sizeof(RAWINPUTHEADER);
}

// Decode once, update the UI cursor/buttons, and keep owned packets away from
// the game's input path. DefWindowProc still performs foreground-packet cleanup.
bool consume_raw_input(Runtime& state, HWND window, LPARAM lparam, const InputCapture& capture,
                       const RAWINPUT* decoded = nullptr) {
    RAWINPUT raw{};
    if (decoded) raw = *decoded;
    else if (!decode_raw_input(lparam, raw)) return false;
    if (raw.header.dwType == RIM_TYPEKEYBOARD) {
        const auto& keyboard = raw.data.keyboard;
        unsigned key = keyboard.VKey;
        if (key == VK_SHIFT) key = MapVirtualKeyW(keyboard.MakeCode, MAPVK_VSC_TO_VK_EX);
        if (key == VK_CONTROL) key = (keyboard.Flags & RI_KEY_E0) ? VK_RCONTROL : VK_LCONTROL;
        if (key == VK_MENU) key = (keyboard.Flags & RI_KEY_E0) ? VK_RMENU : VK_LMENU;
        const bool down = (keyboard.Flags & RI_KEY_BREAK) == 0;
        return state.owned_raw_keys.consume(key, down, state.blocks_key(static_cast<int>(key), capture));
    }
    if (raw.header.dwType != RIM_TYPEMOUSE) return false;
    bool owned_tail = false;
    constexpr std::array<USHORT, 5> down_flags{RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN,
        RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN};
    constexpr std::array<USHORT, 5> up_flags{RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP,
        RI_MOUSE_MIDDLE_BUTTON_UP, RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP};
    for (std::size_t i = 0; i < down_flags.size(); ++i) {
        if (raw.data.mouse.usButtonFlags & down_flags[i])
            owned_tail = state.owned_raw_buttons.consume(i, true, capture.open) || owned_tail;
        if (raw.data.mouse.usButtonFlags & up_flags[i])
            owned_tail = state.owned_raw_buttons.consume(i, false, capture.open) || owned_tail;
    }
    if (!capture.open) {
        state.raw_buttons_down.fill(false);
        return owned_tail;
    }
    if (state.raw_mouse_nolegacy < 0) state.raw_mouse_nolegacy = raw_mouse_suppresses_legacy() ? 1 : 0;
    const bool focused = state.window_focused_cached.load(std::memory_order_acquire);
    const bool absolute = (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0;
    if (relative_pointer_owns_motion(capture.open, focused, state.raw_mouse_nolegacy == 1, absolute)) {
        if (state.relative_pointer_epoch != capture.epoch) {
            reset_relative_cursor(state);
            state.relative_pointer_epoch = capture.epoch;
        }
        long seed_x = 0, seed_y = 0;
        if (!state.read_client_cursor(seed_x, seed_y)) {
            cache_client_cursor(state, window);
            state.read_client_cursor(seed_x, seed_y);
        }
        RECT bounds{};
        if (GetClientRect(window, &bounds) && state.relative_menu_pointer.move(raw.data.mouse.lLastX, raw.data.mouse.lLastY,
                bounds.right - bounds.left, bounds.bottom - bounds.top, seed_x, seed_y)) {
            const int x = state.relative_menu_pointer.x();
            const int y = state.relative_menu_pointer.y();
            state.publish_client_cursor(x, y);
            warp_os_cursor(state, window, x, y);
        }
    } else {
        reset_relative_cursor(state);
        cache_client_cursor(state, window);
    }
    if (state.raw_mouse_nolegacy != 1) return true;
    const auto buttons = raw.data.mouse.usButtonFlags;
    // Legacy clicks capture on the window thread. NOLEGACY never delivers
    // WM_LBUTTONDOWN, so capture here. Present must not call SetCapture.
    bool held_before = false;
    for (const bool down : state.raw_buttons_down) held_before = held_before || down;
    for (std::size_t i = 0; i < down_flags.size(); ++i) {
        if (buttons & down_flags[i]) state.raw_buttons_down[i] = true;
        if (buttons & up_flags[i]) state.raw_buttons_down[i] = false;
    }
    bool held_after = false;
    for (const bool down : state.raw_buttons_down) held_after = held_after || down;
    if (focused && !held_before && held_after) SetCapture(window);
    if (focused && held_before && !held_after && GetCapture() == window) ReleaseCapture();
    const auto raw_button = [&](USHORT down_flag, USHORT up_flag, UINT down_message, UINT up_message, WPARAM key) {
        if (buttons & down_flag) queue_input(state, window, down_message, key, 0, capture.epoch);
        if (buttons & up_flag) queue_input(state, window, up_message, key, 0, capture.epoch);
    };
    raw_button(RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, WM_LBUTTONDOWN, WM_LBUTTONUP, 0);
    raw_button(RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, WM_RBUTTONDOWN, WM_RBUTTONUP, 0);
    raw_button(RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, WM_MBUTTONDOWN, WM_MBUTTONUP, 0);
    raw_button(RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, WM_XBUTTONDOWN, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1));
    raw_button(RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, WM_XBUTTONDOWN, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2));
    const auto wheel = wheel_from_raw_mouse(raw.data.mouse.usButtonFlags, raw.data.mouse.usButtonData);
    if (wheel.vertical) queue_raw_wheel(state, window, WM_MOUSEWHEEL, wheel.vertical_delta, capture.epoch);
    if (wheel.horizontal) queue_raw_wheel(state, window, WM_MOUSEHWHEEL, wheel.horizontal_delta, capture.epoch);
    return true;
}

// Time spent in the game's own window procedure, excluded from our cost.
thread_local std::uint64_t wndproc_excluded = 0;

LRESULT overlay_window_proc_body(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

LRESULT CALLBACK overlay_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto& state = runtime();
    const auto start = perf_now();
    const auto saved = std::exchange(wndproc_excluded, 0);
    const auto result = overlay_window_proc_body(window, message, wparam, lparam);
    const auto excluded = std::exchange(wndproc_excluded, saved);
    state.perf.wndproc.add(perf_now() - start - excluded);
    state.perf.window_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (message == WM_INPUT) state.perf.wm_input.fetch_add(1, std::memory_order_relaxed);
    return result;
}

LRESULT overlay_window_proc_body(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    CallbackScope callback;
    auto& state = callback.state;
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && wparam == VK_END) {
        request_eject(state);
        return 0;
    }
    if (message == WM_IME_STARTCOMPOSITION) state.ime_composing = true;
    else if (message == WM_IME_ENDCOMPOSITION || message == WM_KILLFOCUS) state.ime_composing = false;
    if (!state.stopping && state.input_open() && keyboard_message(message) && wparam == VK_ESCAPE) {
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        if (down && state.ime_escape_owned) return 0;
        if (down && state.ime_composing) {
            if (HIMC context = ImmGetContext(window)) {
                ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                ImmReleaseContext(window, context);
            }
            state.ime_escape_owned = true;
            return 0;
        }
        if (!down && state.ime_escape_owned) {
            state.ime_escape_owned = false;
            return 0;
        }
    }
    // While a Social text input is focused its IME messages end here; otherwise
    // the bridge leaves every message alone.
    if (!state.stopping) {
        gui::ImeBridge::LParam ime_result = 0;
        try {
            if (state.ime.window_message(window, message, wparam, lparam, ime_result)) return static_cast<LRESULT>(ime_result);
        } catch (...) { /* Never unwind into the game window procedure. */ }
    }
    if (message == WM_SIZE)
        state.window_iconic.store(wparam == SIZE_MINIMIZED, std::memory_order_release);
    if (message == WM_NCDESTROY && window == state.mouse_window.load(std::memory_order_acquire))
        state.window_gone.store(true, std::memory_order_release);
    if (state.cursor_message && message == state.cursor_message) {
        if (window == state.mouse_window) {
            state.cursor_message_pending = false;
            // Position-only sampling does not invoke native mouse transitions
            // or inspect game objects, and stays on the window owner thread.
            if (!state.stopping && state.input_open() && state.window_focused_cached.load(std::memory_order_acquire))
                cache_client_cursor(state, window);
        }
        return 0;
    }
    if (state.menu_action_message && message == state.menu_action_message) {
        // Queue only. A dedicated native controller-update hook performs the
        // verified game call; the window thread never invokes native UI here.
        try {
            if (!state.stopping && state.menu_visible &&
                static_cast<std::uint64_t>(lparam) == state.menu_epoch.load(std::memory_order_acquire) &&
                state.menu_screen(true) == sdk::MenuScreenState::main_menu &&
                static_cast<std::uint64_t>(lparam) == state.menu_epoch.load(std::memory_order_acquire)) {
                bool queued = false;
                const auto epoch = static_cast<std::uint64_t>(lparam);
                if (state.navigation_ready.load(std::memory_order_acquire)) {
                    switch (static_cast<gui::MainMenuAction>(wparam)) {
                    case gui::MainMenuAction::Play: queued = state.navigation.openPlay(epoch); break;
                    case gui::MainMenuAction::Settings: queued = state.navigation.openSettings(epoch); break;
                    case gui::MainMenuAction::Quit: queued = state.navigation.quitGame(epoch); break;
                    default: break;
                    }
                }
                const auto* name = gui::main_menu_action_name(static_cast<gui::MainMenuAction>(wparam));
                state.report(std::string("MainMenu ") + name + (queued ? " queued for native UI update." :
                    " request unavailable or busy; no native UI call was made."));
            }
        } catch (...) { /* Never unwind into the game window procedure. */ }
        state.release_menu_action(static_cast<std::uint64_t>(lparam));
        return 0;
    }
    RAWINPUT raw{};
    const bool decoded_raw = !state.stopping && state.menu_visible && message == WM_INPUT && decode_raw_input(lparam, raw);
    if (decoded_raw && raw.header.dwType == RIM_TYPEKEYBOARD &&
            raw.data.keyboard.VKey == VK_END && !(raw.data.keyboard.Flags & RI_KEY_BREAK))
        request_eject(state);
    // High-rate motion uses the state published by Present. State-changing
    // input still gets a fresh screen check before taking its capture snapshot.
    if (!state.stopping && state.menu_visible && menu_input_requires_screen_validation(message, decoded_raw ? &raw : nullptr)) {
        const auto menu_start = perf_now();
        try {
            if (state.menu_screen() != sdk::MenuScreenState::main_menu) state.publish_menu_visible(false);
        } catch (...) { state.publish_menu_visible(false); }
        state.perf.wndproc_menu.add(perf_now() - menu_start);
    }
    if (state.mouse_message && message == state.mouse_message) {
        state.mouse_message_pending = false;
        if (wparam != 0) {
            state.owned_keys.reset();
            state.owned_raw_keys.reset();
            state.owned_raw_buttons.reset();
        }
        try { return state.sync_mouse(window, wparam != 0) ? 1 : 0; }
        catch (...) { return 0; }
    }
    WNDPROC original = DefWindowProcW;
    const auto capture = state.input_capture();
    {
        std::lock_guard lock(state.window_mutex);
        for (const auto& entry : state.windows) if (entry.window == window) { original = entry.original; break; }
    }
    if (message == WM_KILLFOCUS) {
        state.ime_escape_owned = false;
        reset_relative_cursor(state);
        state.owned_keys.reset();
        state.owned_raw_keys.reset();
        state.owned_raw_buttons.reset();
        state.owned_mouse_buttons.store(0, std::memory_order_release);
        state.raw_buttons_down.fill(false);
    }
    bool consume_key = false;
    unsigned opening_mode = 0;
    if (!state.stopping) {
        const bool keyboard = keyboard_message(message);
        const int key = keyboard ? normalized_key(wparam, lparam) : mouse_key(message, wparam);
        if (key > 0 && key < 256) {
            const bool up = keyboard ? (message == WM_KEYUP || message == WM_SYSKEYUP) :
                (message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP || message == WM_XBUTTONUP);
            if (!up && !capture.open && !(keyboard && (lparam & (1LL << 30)))) {
                if (key == gui::Overlay::social_key && state.social_available.load(std::memory_order_acquire)) opening_mode = 2;
                else if (key == state.open_key.load(std::memory_order_acquire)) opening_mode = 1;
            }
            consume_key = state.owned_keys.consume(static_cast<std::size_t>(key), !up,
                state.blocks_key(key, capture));
            const auto button = mouse_button_mask(key);
            if (button && !up && capture.open) state.owned_mouse_buttons.fetch_or(button, std::memory_order_acq_rel);
            if (button && up) state.owned_mouse_buttons.fetch_and(~button, std::memory_order_acq_rel);
            if (!capture.open && consume_key && !opening_mode) return 0;
        }
        if (!capture.open && message == WM_INPUT &&
            (state.owned_raw_buttons.active() || state.owned_raw_keys.active()) &&
            consume_raw_input(state, window, lparam, capture, decoded_raw ? &raw : nullptr))
            return DefWindowProcW(window, message, wparam, lparam);
    }
    if (!state.stopping && (keyboard_message(message) || mouse_message(message) || character_message(message) ||
            message == WM_KILLFOCUS || message == WM_SETFOCUS || message == WM_MOUSELEAVE)) {
        const bool open_now = capture.open;
        // Cursor position is published directly. Queueing every move made Present
        // walk a backlog and call into Win32 on the click frame.
        const bool pointer_move = message == WM_MOUSEMOVE || message == WM_NCMOUSEMOVE;
        if (!pointer_move && !(open_now && artifact_wheel(message, wparam)))
            queue_input(state, window, message, wparam, lparam, capture.epoch, opening_mode);
    }
    const bool open = !state.stopping && capture.open;
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ACTIVATE || message == WM_ACTIVATEAPP) {
        if (message == WM_KILLFOCUS || (message == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE) ||
            (message == WM_ACTIVATEAPP && wparam == 0))
            state.window_focused_cached.store(false, std::memory_order_release);
        else if (message == WM_SETFOCUS || message == WM_ACTIVATE || message == WM_ACTIVATEAPP)
            state.window_focused_cached.store(true, std::memory_order_release);
    }
    if ((message == WM_KILLFOCUS || message == WM_SETFOCUS) && window == state.mouse_window) {
        if (message == WM_KILLFOCUS && GetCapture() == window) ReleaseCapture();
        state.request_mouse_sync();
    }
    if (open && message == WM_MOUSEMOVE && !state.relative_menu_pointer.active())
        state.publish_client_cursor(static_cast<long>(static_cast<short>(LOWORD(lparam))),
                                    static_cast<long>(static_cast<short>(HIWORD(lparam))));
    else if (open && mouse_message(message))
        cache_client_cursor(state, window);
    if (open) {
        // Capture belongs to the window thread; ImGui input itself is queued to
        // Present, which may run on a separate thread in Minecraft.
        const bool focused = state.window_focused_cached.load(std::memory_order_acquire);
        if (focused && (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN))
            SetCapture(window);
        if ((message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP || message == WM_XBUTTONUP) &&
                !(wparam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)) && GetCapture() == window)
            ReleaseCapture();
        if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT && focused) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        if (message == WM_INPUT) {
            if (consume_raw_input(state, window, lparam, capture, decoded_raw ? &raw : nullptr))
                return DefWindowProcW(window, message, wparam, lparam);
        }
        if (mouse_message(message) || character_message(message)) return 0;
    }
    if (consume_key) return 0;
    const auto original_start = perf_now();
    const auto result = CallWindowProcW(original, window, message, wparam, lparam);
    wndproc_excluded += perf_now() - original_start;
    return result;
}

bool Runtime::attach_window(HWND target) {
    if (window == target && platform_ready) return true;
    if (platform_ready) { ImGui_ImplWin32_Shutdown(); platform_ready = false; }
    if (!ImGui_ImplWin32_Init(target)) { report("ImGui Win32 backend initialization failed.", true); return false; }
    platform_ready = true;
    window = target;
    ime.set_window(target);
    {
        std::lock_guard lock(window_mutex);
        auto found = std::find_if(windows.begin(), windows.end(), [target](const auto& entry) { return entry.window == target; });
        if (found == windows.end() || found->restored) {
            SetLastError(0);
            auto original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(target, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(overlay_window_proc)));
            if (!original && GetLastError()) {
                report("Cannot attach the game window procedure: Win32 error " + std::to_string(GetLastError()), true);
                return false;
            }
            if (found != windows.end()) { found->original = original; found->restored = false; }
            else windows.push_back({target, original});
        }
    }
    // Do not hold window_mutex while waiting on a native mouse transition:
    // SetCapture/ReleaseCapture can reenter the window-procedure chain.
    {
        std::lock_guard mouse_lock(mouse_mutex);
        mouse_message_pending = false;
        cursor_message_pending = false;
        mouse_window = target;
        window_gone.store(false, std::memory_order_release);
    }
    window_iconic.store(IsIconic(target) != FALSE, std::memory_order_release);
    mouse_sync = {};
    publish_focus(target);
    report("Game window input attached.");
    return true;
}

HWND swap_window(IDXGISwapChain* swap) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (SUCCEEDED(swap->GetDesc(&desc)) && desc.OutputWindow) return desc.OutputWindow;
    ComPtr<IDXGISwapChain1> swap1;
    if (SUCCEEDED(swap->QueryInterface(IID_PPV_ARGS(&swap1)))) {
        HWND window{};
        if (SUCCEEDED(swap1->GetHwnd(&window)) && window) return window;
        ComPtr<CoreWindowInterop> interop;
        if (SUCCEEDED(swap1->GetCoreWindow(core_interop_iid, reinterpret_cast<void**>(interop.GetAddressOf()))) &&
                SUCCEEDED(interop->get_WindowHandle(&window)) && window) return window;
    }
    struct Search { HWND window{}; LONG area{}; } search;
    const auto visit = [](HWND window, LPARAM data) -> BOOL {
        auto& result = *reinterpret_cast<Search*>(data);
        DWORD process{};
        GetWindowThreadProcessId(window, &process);
        RECT rect{};
        if (process == GetCurrentProcessId() && IsWindowVisible(window) && GetClientRect(window, &rect)) {
            const LONG area = (rect.right - rect.left) * (rect.bottom - rect.top);
            if (area > result.area) { result.window = window; result.area = area; }
        }
        return TRUE;
    };
    EnumWindows(visit, reinterpret_cast<LPARAM>(&search));
    if (search.window) EnumChildWindows(search.window, visit, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

void advance_rotation_render(Runtime& state) noexcept {
    const auto reset = [&] {
        state.rotation_visual_initialized = false;
        state.rotation_visual_actor = 0;
        state.rotation_visual_updated = {};
        state.rotation_visual_ready.store(false, std::memory_order_release);
    };
    if (!state.client || !state.rotation_override_enabled.load(std::memory_order_acquire) ||
        !sdk::custom_model::third_person(state.rotation_view_mode.load(std::memory_order_acquire))) {
        reset();
        return;
    }

    const auto actor = state.rotation_override_actor.load(std::memory_order_acquire);
    const auto yaws = state.rotation_override_yaws.load(std::memory_order_acquire);
    const auto pitches = state.rotation_override_pitches.load(std::memory_order_acquire);
    const float previous_yaw = std::bit_cast<float>(static_cast<std::uint32_t>(yaws));
    const float target_yaw = std::bit_cast<float>(static_cast<std::uint32_t>(yaws >> 32));
    const float previous_pitch = std::bit_cast<float>(static_cast<std::uint32_t>(pitches));
    const float target_pitch = std::bit_cast<float>(static_cast<std::uint32_t>(pitches >> 32));
    if (!actor || !std::isfinite(previous_yaw) || !std::isfinite(target_yaw) ||
        !std::isfinite(previous_pitch) || !std::isfinite(target_pitch)) {
        reset();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    float yaw = previous_yaw;
    float pitch = previous_pitch;
    if (state.rotation_visual_initialized && state.rotation_visual_actor == actor) {
        const auto current = state.rotation_visual_angles.load(std::memory_order_acquire);
        yaw = std::bit_cast<float>(static_cast<std::uint32_t>(current));
        pitch = std::bit_cast<float>(static_cast<std::uint32_t>(current >> 32));
    } else {
        state.rotation_visual_initialized = true;
        state.rotation_visual_actor = actor;
    }

    float delta_seconds = 0.0F;
    if (state.rotation_visual_updated != std::chrono::steady_clock::time_point{}) {
        delta_seconds = std::clamp(
            std::chrono::duration<float>(now - state.rotation_visual_updated).count(), 0.0F, 0.1F);
    }
    state.rotation_visual_updated = now;

    float maximum_speed = 1300.0F;
    if (const auto* module = dynamic_cast<const RotationsModule*>(
            state.client->modules().find(RotationsModule::module_id))) {
        maximum_speed = module->speed.get<float>();
    }
    if (!std::isfinite(maximum_speed)) maximum_speed = 1300.0F;
    maximum_speed = std::clamp(maximum_speed, 0.0F, 10000.0F);

    const float yaw_delta = std::remainder(target_yaw - yaw, 360.0F);
    const float pitch_delta = std::clamp(target_pitch, -90.0F, 90.0F) - pitch;
    const float distance = std::hypot(yaw_delta, pitch_delta);
    if (distance > 0.0001F && delta_seconds > 0.0F) {
        const float speed_scale = std::clamp(distance / 180.0F, 0.0F, 1.0F);
        const float step = maximum_speed * speed_scale * delta_seconds;
        const float fraction = std::min(1.0F, step / distance);
        yaw = std::remainder(yaw + yaw_delta * fraction, 360.0F);
        pitch = std::clamp(pitch + pitch_delta * fraction, -90.0F, 90.0F);
        if (fraction >= 1.0F) {
            yaw = std::remainder(target_yaw, 360.0F);
            pitch = std::clamp(target_pitch, -90.0F, 90.0F);
        }
    }

    const auto packed = static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(yaw)) |
        (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(pitch)) << 32);
    state.rotation_visual_angles.store(packed, std::memory_order_release);
    state.rotation_visual_ready.store(true, std::memory_order_release);
}

bool Runtime::draw(IDXGISwapChain* swap, UINT flags) {
    if ((flags & DXGI_PRESENT_TEST) || stopping || status == 4) return false;
    std::lock_guard lock(render_mutex);
    if (stopping || GetTickCount64() < retry_at) return false;
    rotation_view_mode.store(sdk::custom_model::read_local_view(bedrock_chat.mouse_client()),
        std::memory_order_release);
    FlagScope submission(inside_overlay);
    if (window_iconic.load(std::memory_order_acquire)) { suspend_menu(); return false; }
    const bool gone = window_gone.load(std::memory_order_acquire);
    HWND target = (!gone && window && active_swap == swap) ? window : swap_window(swap);
    if (!target) { suspend_menu(); return false; }
    const bool bound = !gone && window == target && active_swap == swap && platform_ready;
    if (!bound) {
        if (!IsWindow(target) || IsIconic(target)) { suspend_menu(); return false; }
        DWORD process{};
        GetWindowThreadProcessId(target, &process);
        if (process != GetCurrentProcessId()) return false;
    }
    if (active_swap && active_swap != swap && window != target && IsWindow(window)) return false;
    if (active_swap && active_swap != swap) release_graphics();

    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    const bool dx12 = SUCCEEDED(swap->GetDevice(IID_PPV_ARGS(&device12)));
    if (dx12 && !renderer) {
        queue = find_queue(swap, device12.Get());
        if (!queue) {
            if (status.exchange(2) != 2) report("Waiting for an unambiguous D3D12 direct command queue.");
            return false;
        }
    }

    if (!context) {
        ContextScope preserve(nullptr);
        context = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        // The OS compositor updates a hardware cursor between game Presents.
        ImGui::GetIO().MouseDrawCursor = false;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        ClickGui::apply_theme();
        client = std::make_unique<Client>(config_path);
        client->overlay().set_owns_font_atlas(true);
        if (social) {
            client->overlay().set_social(&social->channel(), &social::play_notification_sound);
            client->overlay().social().set_startup_sign_in(true);
            if (!update_version.empty()) client->overlay().social().set_update_notice(update_version);
        }
        if (!server_version.empty()) client->overlay().set_client_version(server_version);
        client->overlay().set_ime(&ime);
        if (!client->start()) { report(client->last_error(), true); status = 4; return false; }
    }
    advance_rotation_render(*this);
    ContextScope scope(context);
    if (!attach_window(target)) { status = 4; return false; }
    if (!renderer) {
        renderer = dx12 ? make_d3d12_renderer() : make_d3d11_renderer();
        std::string error;
        if (!renderer->initialize(swap, queue.Get(), error)) {
            report(error, true);
            renderer.reset();
            retry_at = GetTickCount64() + 2000;
            return false;
        }
        client->overlay().set_texture_host(renderer->textures());
        active_swap = swap;
        report(dx12 ? "D3D12 overlay renderer initialized." : "D3D11 overlay renderer initialized.");
    }
    std::deque<std::string> commands;
    { std::lock_guard lock(command_mutex); commands.swap(pending_commands); }
    for (const auto& command : commands) {
        const auto result = client->execute_command(command);
        {
            std::lock_guard lock(feedback_mutex);
            if (pending_feedback.size() == 64) pending_feedback.pop_front();
            pending_feedback.push_back("[Medicon] " + result.message);
            feedback_count = static_cast<unsigned>(pending_feedback.size());
        }
        report(result.message);
    }
    std::deque<InputMessage> pending;
    const auto menu_start = perf_now();
    const auto screen = menu_screen();
    perf.menu.add(perf_now() - menu_start);
    const bool main_menu = screen == sdk::MenuScreenState::main_menu;
    const bool menu_changed = client->overlay().main_menu().active() != main_menu;
    if (menu_changed) report(main_menu ? "MainMenu active: verified start_screen; native input is blocked." :
        "MainMenu inactive: replacement hidden; native input restored.");
    client->overlay().set_main_menu_active(main_menu);
    if (main_menu) client->clear_gameplay();
    publish_client_input();
    publish_menu_visible(main_menu);
    const auto current_menu_epoch = menu_epoch.load(std::memory_order_acquire);
    if (menu_changed || applied_menu_epoch != current_menu_epoch) {
        // Even an away-and-back observation between frames cancels old presses.
        client->overlay().set_main_menu_active(false);
        client->overlay().set_main_menu_active(main_menu);
        client->modules().reset_keys();
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
        applied_menu_epoch = current_menu_epoch;
    }
    {
        std::lock_guard lock(input_mutex);
        pending.swap(messages);
    }
    for (const auto& event : pending) {
        if (event.window != window || event.menu_epoch != menu_epoch.load(std::memory_order_acquire)) {
            release_opening_input(event.opening_request);
            continue;
        }
        // key_event above this message may already have opened the GUI. ImGui
        // applies wheel events in NewFrame, so drop artifacts before forwarding.
        if (client->capturing_input() && artifact_wheel(event.message, event.wparam)) continue;
        // Mouse is submitted after the cursor position so the click lands on
        // this frame's pointer, and so Present never calls SetCapture.
        if (!overlay_mouse_message(event.message))
            ImGui_ImplWin32_WndProcHandler(event.window, event.message, event.wparam, event.lparam);
        if (keyboard_message(event.message)) {
            const bool down = event.message == WM_KEYDOWN || event.message == WM_SYSKEYDOWN;
            const bool repeat = down && (event.lparam & (1LL << 30));
            const int key = normalized_key(event.wparam, event.lparam);
            client->key_event(key, down, repeat);
            // WM_KEY uses generic Shift/Ctrl/Alt. Support both generic names
            // and sided binds while preserving fresh-press hotkey semantics.
            if (key != static_cast<int>(event.wparam)) client->key_event(static_cast<int>(event.wparam), down, repeat);
        } else if (const int key = mouse_key(event.message, event.wparam)) {
            const bool up = event.message == WM_LBUTTONUP || event.message == WM_RBUTTONUP ||
                event.message == WM_MBUTTONUP || event.message == WM_XBUTTONUP;
            client->key_event(key, !up);
        } else if (event.message == WM_CHAR) {
            client->overlay().char_event(static_cast<wchar_t>(event.wparam));
        } else if (event.message == WM_KILLFOCUS) {
            client->modules().reset_keys();
            ImGui::GetIO().ClearInputKeys();
            ImGui::GetIO().ClearInputMouse();
        }
        if (event.opening_request) {
            publish_client_input();
            release_opening_input(event.opening_request);
        }
    }
    if (auto* module = client->modules().find(ClickGuiModule::module_id)) open_key = module->keybind() ? module->keybind() : VK_INSERT;
    publish_client_input();
    const bool cursor_visible = input_open() && window_focused_cached.load(std::memory_order_acquire);
    ImGui::GetIO().MouseDrawCursor = false;
    present_safe_imgui_new_frame(swap);
    if (input_open()) {
        const auto submit_cursor = [&](long x, long y) {
            const auto size = ImGui::GetIO().DisplaySize;
            if (menu_visible.load(std::memory_order_acquire) &&
                (x < 0 || y < 0 || x >= size.x || y >= size.y)) {
                // Before the first relative packet, a free OS cursor may be
                // outside this window. Seed a visible pointer for the menu.
                x = static_cast<long>(size.x * 0.5F);
                y = static_cast<long>(size.y * 0.5F);
            }
            ImGui::GetIO().AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
        };
        long x = 0, y = 0;
        if (cursor_visible && read_client_cursor(x, y))
            submit_cursor(x, y);
        else
            ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        for (const auto& event : pending) {
            if (event.window != window || event.menu_epoch != menu_epoch.load(std::memory_order_acquire)) continue;
            if (artifact_wheel(event.message, event.wparam)) continue;
            submit_overlay_mouse(event.message, event.wparam);
        }
    }
    std::string error;
    const auto fonts_start = perf_now();
    if (client->prepare_frame()) {
        const auto rebuild_start = perf_now();
        const bool rebuilt = renderer->rebuild_fonts(error);
        perf.rebuild.add(perf_now() - rebuild_start);
        if (!rebuilt) { report(error, true); release_graphics(); retry_at = GetTickCount64() + 2000; return false; }
    }
    const auto build_start = perf_now();
    perf.fonts.add(build_start - fonts_start);
    ImGui::NewFrame();
    // Input may have observed a scene switch while this frame was prepared.
    if (client->overlay().main_menu().active() && !menu_visible.load(std::memory_order_acquire))
        client->overlay().set_main_menu_active(false);
    client->tick(ImGui::GetIO().DeltaTime);
    // The camera moves every frame but game ticks come 20 times a second.
    // Reread its plain fields so ESP and ChinaHat stay on the picture.
    if (gameplay_ready.load(std::memory_order_acquire))
        if (auto* view = client->gameplay_view()) gameplay.refresh_view(*view);
    client->render();
    draw_model_probe();
    publish_client_input();
    ImGui::Render();
    const auto submit_start = perf_now();
    perf.build.add(submit_start - build_start);
    // The screen was already observed before this build. Polling again here
    // walks native menu state after the mesh exists, and a slow build expires
    // the cache so the poll can discard the frame it just paid for.
    const bool rendered = renderer->render(ImGui::GetDrawData(), error);
    perf.submit.add(perf_now() - submit_start);
    {
        const auto counters = renderer->take_counters();
        render_counters.effects_ticks += counters.effects_ticks;
        render_counters.effects_max = std::max(render_counters.effects_max, counters.effects_max);
        render_counters.effects_frames += counters.effects_frames;
        render_counters.skipped_frames += counters.skipped_frames;
    }
    if (!rendered) {
        report(error, true); release_graphics(); retry_at = GetTickCount64() + 2000; status = 2; return false;
    }
    if (const auto action = client->overlay().main_menu().consume_action(); action && menu_visible &&
            current_menu_epoch == menu_epoch.load(std::memory_order_acquire) && menu_action_message) {
        std::uint64_t no_request = 0;
        if (menu_action_pending.compare_exchange_strong(no_request, current_menu_epoch, std::memory_order_acq_rel)) {
            // Stamp the frame's original epoch. A concurrent observation must
            // never relabel an old click as belonging to a replacement menu.
            if (current_menu_epoch != menu_epoch.load(std::memory_order_acquire)) {
                release_menu_action(current_menu_epoch);
            } else if (!PostMessageW(window, menu_action_message, static_cast<WPARAM>(*action),
                                     static_cast<LPARAM>(current_menu_epoch))) {
                release_menu_action(current_menu_epoch);
                report("MainMenu action could not be queued to the window thread.", true);
            }
        }
    }
    if (status.exchange(3) != 3) report("Overlay drawing. Insert opens/closes the GUI.");
    return true;
}

const char* model_status_name(medicon::custom_model::ModelRegistry::Status status) noexcept {
    using S=medicon::custom_model::ModelRegistry::Status;
    return status==S::ready?"ready":status==S::failed?"failed":status==S::loading?"loading":"not started";
}

void Runtime::draw_model_probe() {
    auto* module = dynamic_cast<CustomModelProbeModule*>(client->modules().find(CustomModelProbeModule::module_id));
    const auto selected=static_cast<medicon::custom_model::BuiltinModel>(module?module->model.get<Choice>().index:0);
    if (model_assets.select(selected)) {
        model_logged_status={};model_logged_drawing=false;model_logged_wait=false;model_wait_since=0;
    }
    const bool requested = module && module->enabled();
    const auto mode=module?module->mode.get<Choice>().index:0;
    const auto progress=model_box.static_progress();
    const bool visible=requested&&mode==0&&progress.ready&&model_box.view_mode()!=sdk::custom_model::ViewMode::unknown;
    using ModelStatus=medicon::custom_model::ModelRegistry::Status;
    if (const auto loaded=model_assets.status(); loaded!=model_logged_status) {
        model_logged_status=loaded;
        if (loaded==ModelStatus::ready||loaded==ModelStatus::failed)
            report(std::string("CustomModel model ")+(loaded==ModelStatus::ready?"loaded: ":"failed: ")+model_assets.diagnostic(),
                loaded==ModelStatus::failed);
    }
    if (visible&&!model_logged_drawing) {
        model_logged_drawing=true;
        report("CustomModel drawing: meshes and textures ready, local player replaced.");
    }
    // Enabled in a world but still vanilla: record which stage never finished, once.
    const bool in_world=client->gameplay_view()!=nullptr;
    if (requested&&mode==0&&in_world&&!visible&&!model_logged_drawing&&!model_logged_wait) {
        const auto now=GetTickCount64();
        if (!model_wait_since) model_wait_since=now;
        else if (now-model_wait_since>15000) {
            model_logged_wait=true;
            const auto asset=model_assets.snapshot();
            const auto status=model_probe.status();
            report("CustomModel still vanilla after 15 s: P0 "+std::string(status==sdk::custom_model::Probe::Status::ready?"ready":
                status==sdk::custom_model::Probe::Status::unavailable?"unavailable":"waiting")+
                ", P1 "+(model_box.ready()?"ready":"unavailable")+", model "+model_status_name(model_assets.status())+
                ", meshes "+std::to_string(progress.meshes)+"/"+std::to_string(asset?asset->parts.size():0)+
                ", textures "+std::to_string(progress.textures)+"/"+std::to_string(asset?asset->textures.size():0)+
                ", stock texture "+(model_box.texture_ready()?"ready":"waiting")+
                ", view "+sdk::custom_model::view_mode_name(model_box.view_mode())+".",true);
        }
    } else if (!requested||mode!=0||!in_world) model_wait_since=0;
    model_box.set_static_mode(mode==0,visible);
    model_box.set_animation(module&&module->animation.get<bool>());
    model_probe.set_suppress_data_driven(mode != 0 || (visible&&model_box.first_person_ready()));
    model_probe.set_enabled(requested&&(mode!=0||visible));
    model_box.set_options(requested && module && (mode==0||(mode==1&&module->test_box.get<bool>())),
        module ? static_cast<unsigned>(module->material.get<Choice>().index) : 0,
        module ? module->brightness.get<float>() : 1.0F);
    const auto sample = model_probe.take_frame();
    const auto box = model_box.take_frame();
    const auto animation=model_box.take_animation_frame();
    const auto& io = ImGui::GetIO();
    const auto index = requested ? 1U : 0U;
    // DeltaTime belongs to the preceding interval. Do not assign a transition
    // interval to either mode, or mix the splash/title screen into FPS samples.
    if (model_has_interval && requested == model_previous_enabled && client->gameplay_view() &&
        !client->overlay().splash_active() && !client->capturing_input() &&
        io.DeltaTime > 0.0F && io.DeltaTime < 1.0F) {
        model_sample_seconds[index] += io.DeltaTime;
        ++model_sample_frames[index];
    }
    model_has_interval = true;
    model_previous_enabled = requested;
    if (!module || !module->show_debug.get<bool>()) return;
    const auto status = model_probe.status();
    const char* readiness = status == sdk::custom_model::Probe::Status::ready ? "hook ready" :
        status == sdk::custom_model::Probe::Status::unavailable ? "unavailable (see inject.log)" : "waiting for native profile";
    ImGui::SetNextWindowPos(ImVec2(18.0F, 90.0F), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.82F);
    constexpr auto flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("CustomModel diagnostic", nullptr, flags)) {
        ImGui::Text("Core: %s", core_version.empty() ? "injected directly" : core_version.c_str());
        ImGui::Text("Skip local: %s | %s", requested&&(mode!=0||visible) ?
            (status == sdk::custom_model::Probe::Status::ready ? "ON" : "BLOCKED") : "OFF", readiness);
        ImGui::TextUnformatted("Toggle: CustomModel keybind (default F8)");
        ImGui::TextUnformatted("World queue candidates: near 0x4716E80 / ECS 0x47172D0");
        ImGui::Text("DataDriven 0x558D300: %s", mode != 0 || model_box.first_person_ready() ? "native arms suppressed" : "native arms while custom arms prepare");
        ImGui::Text("Custom model view: %s", sdk::custom_model::view_mode_name(model_box.view_mode()));
        ImGui::Text("Candidates / Present: %llu | local: %llu | skipped: %llu | invalid: %llu",
            static_cast<unsigned long long>(sample.calls), static_cast<unsigned long long>(sample.local),
            static_cast<unsigned long long>(sample.skipped), static_cast<unsigned long long>(sample.invalid));
        for (std::size_t path = 0; path < sample.paths.size(); ++path) {
            const auto& part = sample.paths[path];
            ImGui::Text("%s: calls %llu | local %llu | skipped %llu | invalid %llu",
                path == 0 ? "Near" : path == 1 ? "ECS" : "DataDriven", static_cast<unsigned long long>(part.calls),
                static_cast<unsigned long long>(part.local), static_cast<unsigned long long>(part.skipped),
                static_cast<unsigned long long>(part.invalid));
            ImGui::Text("  Actor: 0x%llX | callback: 0x%llX",
                static_cast<unsigned long long>(part.actor), static_cast<unsigned long long>(part.callback));
        }
        ImGui::Text("Probe CPU / Present: %.4f ms (excludes original callback)", perf_ms(sample.overhead_ticks));
        ImGui::Text("Current FPS: %.1f", io.Framerate);
        for (unsigned mode = 0; mode < 2; ++mode) {
            if (model_sample_seconds[mode] > 0)
                ImGui::Text("%s mean FPS: %.1f (%llu frames)", mode ? "ON" : "OFF",
                    model_sample_frames[mode] / model_sample_seconds[mode],
                    static_cast<unsigned long long>(model_sample_frames[mode]));
            else ImGui::Text("%s mean FPS: --", mode ? "ON" : "OFF");
        }
        ImGui::Separator();
        ImGui::Text("P1: %s | %s", model_box.ready() ? "hooks ready" : "unavailable (see inject.log)",
            module->test_box.get<bool>() ? "test box enabled" : "test box disabled");
        ImGui::Text("World / submits / matched / drawn: %llu / %llu / %llu / %llu",
            static_cast<unsigned long long>(box.worlds), static_cast<unsigned long long>(box.submits),
            static_cast<unsigned long long>(box.in_world), static_cast<unsigned long long>(box.drawn));
        ImGui::Text("Native MeshSubmit / in world / other screen: %llu / %llu / %llu",
            static_cast<unsigned long long>(box.native_submits),static_cast<unsigned long long>(box.scoped_submits),
            static_cast<unsigned long long>(box.other_screen));
        for (unsigned i=1; i<box.failures.size(); ++i) if (box.failures[i])
            ImGui::Text("P1 rejected %s: %llu", sdk::custom_model::box_failure_name(static_cast<sdk::custom_model::BoxFailure>(i)),
                static_cast<unsigned long long>(box.failures[i]));
        ImGui::Text("P1 draw CPU / Present: %.4f ms", perf_ms(box.ticks));
        ImGui::Text("P1 retained stock texture: %s",model_box.texture_ready() ? "ready" : "waiting for textured native submit with empty metadata");
        ImGui::TextUnformatted("P1 retains a stock binding; custom models use their own PNGs.");
        ImGui::Separator();
        const auto asset=model_assets.snapshot();
        ImGui::Text("P2: %s | native meshes %u / %u | PNG %u / %u",progress.ready?"ready":"loading / vanilla fallback",
            progress.meshes,asset?static_cast<unsigned>(asset->parts.size()):0,progress.textures,asset?static_cast<unsigned>(asset->textures.size()):0);
        ImGui::TextWrapped("%s",model_assets.diagnostic().c_str());
        if (asset) ImGui::Text("Triangles: %llu | cache: %s | scale %.2f | yaw %.1f",
            static_cast<unsigned long long>(asset->triangle_count),asset->from_cache?"hit":"imported",asset->scale,asset->yaw_offset);
        ImGui::Text("P2 texture requests: %llu | total upload/draw CPU: %.3f ms",static_cast<unsigned long long>(progress.requests),perf_ms(progress.ticks));
        ImGui::Text("P3: %s | %s | input %s | frames %llu",module->animation.get<bool>()?"ON":"OFF",
            medicon::custom_model::motion_name(static_cast<medicon::custom_model::Motion>(animation.motion)),animation.input_ready?"ready":"waiting",static_cast<unsigned long long>(animation.frames));
        if(asset&&asset->rig)ImGui::Text("Bones %u | skin vertices %u | rig cache %s",static_cast<unsigned>(asset->rig->bones.size()),static_cast<unsigned>(asset->rig->vertices.size()),asset->rig->from_cache?"hit":"imported");
        if(asset&&asset->rig)ImGui::Text("First-person arms: %s | vertices %u",model_box.first_person_ready()?"drawn":"preparing / native fallback",static_cast<unsigned>(asset->rig->first_person.vertices.size()));
        if(animation.frames)ImGui::Text("P3 CPU skin %.4f ms | native draw %.4f ms / world",perf_ms(animation.cpu_ticks)/animation.frames,perf_ms(animation.draw_ticks)/animation.frames);
        ImGui::Text("P3 bulk/fallback batches %llu/%llu | input failures %llu",static_cast<unsigned long long>(animation.bulk_batches),static_cast<unsigned long long>(animation.fallback_batches),static_cast<unsigned long long>(animation.read_failures));
    }
    ImGui::End();
}

bool model_actor_is_local(void* actor, void* owner) noexcept {
    return static_cast<Runtime*>(owner)->bedrock_chat.is_local_player(actor);
}

bool restore_rotation_render_pitch(Runtime& state, void* actor) noexcept {
    if (reinterpret_cast<std::uintptr_t>(actor) != state.rotation_override_actor.load(std::memory_order_relaxed)) return false;
    const auto render_component = state.rotation_override_render_component.load(std::memory_order_acquire);
    const auto rotation_component = state.rotation_override_component.load(std::memory_order_acquire);
    if (!render_component || !rotation_component || render_component > UINTPTR_MAX - sizeof(float) ||
        rotation_component > UINTPTR_MAX - sizeof(float)) return false;
    float view_pitch{};
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(rotation_component),
            &view_pitch, sizeof(view_pitch), &read) || read != sizeof(view_pitch) || !std::isfinite(view_pitch)) return false;
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(render_component),
        &view_pitch, sizeof(view_pitch), &written) && written == sizeof(view_pitch);
}

float held_rotation_yaw(Runtime& state, void* actor, float alpha, float native_yaw) noexcept {
    if (state.stopping.load(std::memory_order_acquire) ||
        reinterpret_cast<std::uintptr_t>(actor) != state.rotation_override_actor.load(std::memory_order_relaxed))
        return native_yaw;

    const auto view_mode = state.rotation_view_mode.load(std::memory_order_acquire);
    if (!sdk::custom_model::third_person(view_mode)) {
        const bool override_enabled = state.rotation_override_enabled.load(std::memory_order_acquire);
        const bool restored = restore_rotation_render_pitch(state, actor);
        const auto now = GetTickCount64();
        auto last = state.pitch_render_diagnostic_last.load(std::memory_order_relaxed);
        if (now - last >= 500 && state.pitch_render_diagnostic_last.compare_exchange_strong(
                last, now, std::memory_order_relaxed)) {
            char message[160]{};
            std::snprintf(message, sizeof(message),
                "Rotations first-person render guard: view=%s yaw=native pitch=%s",
                sdk::custom_model::view_mode_name(view_mode),
                restored ? "restored" : (override_enabled ? "restore failed" : "native"));
            state.report(message, override_enabled && !restored);
        }
        return native_yaw;
    }

    if (!state.rotation_override_enabled.load(std::memory_order_acquire)) {
        restore_rotation_render_pitch(state, actor);
        return native_yaw;
    }

    const auto yaws = state.rotation_override_yaws.load(std::memory_order_acquire);
    const float previous_yaw = std::bit_cast<float>(static_cast<std::uint32_t>(yaws));
    const float yaw = std::bit_cast<float>(static_cast<std::uint32_t>(yaws >> 32));
    const auto render_component = state.rotation_override_render_component.load(std::memory_order_acquire);
    const auto pitches = state.rotation_override_pitches.load(std::memory_order_acquire);
    const float previous_pitch = std::bit_cast<float>(static_cast<std::uint32_t>(pitches));
    const float pitch = std::bit_cast<float>(static_cast<std::uint32_t>(pitches >> 32));
    float render_yaw = sdk::interpolate_yaw(previous_yaw, yaw, alpha);
    float render_pitch = std::isfinite(alpha)
        ? previous_pitch + (pitch - previous_pitch) * std::clamp(alpha, 0.0F, 1.0F)
        : pitch;
    const bool visual_ready = state.rotation_visual_ready.load(std::memory_order_acquire);
    if (visual_ready) {
        const auto visual = state.rotation_visual_angles.load(std::memory_order_acquire);
        render_yaw = std::bit_cast<float>(static_cast<std::uint32_t>(visual));
        render_pitch = std::bit_cast<float>(static_cast<std::uint32_t>(visual >> 32));
    }
    bool render_write_ok = false;
    if (std::isfinite(render_pitch)) {
        // The same render rotation can affect first-person arms, so this
        // branch is reached only after a validated third-person view check.
        if (render_component && render_component <= UINTPTR_MAX - sizeof(float)) {
            SIZE_T written = 0;
            render_write_ok = WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(render_component),
                &render_pitch, sizeof(render_pitch), &written) && written == sizeof(render_pitch);
        }
        const auto now = GetTickCount64();
        auto last = state.pitch_render_diagnostic_last.load(std::memory_order_relaxed);
        if (now - last >= 500 && state.pitch_render_diagnostic_last.compare_exchange_strong(
                last, now, std::memory_order_relaxed)) {
            char message[224]{};
            std::snprintf(message, sizeof(message),
                "Rotations render: view=%s mode=%s pitch=%.2f->%.2f yaw=%.2f render_write=%s",
                sdk::custom_model::view_mode_name(view_mode), visual_ready ? "smooth" : "tick-fallback",
                render_pitch, pitch, render_yaw, render_write_ok ? "ok" : "failed");
            state.report(message, !render_write_ok);
        }
    }
    return render_yaw;
}

float hook_body_yaw(void* actor, float alpha) {
    CallbackScope callback;
    auto& state = callback.state;
    const auto original = state.body_yaw_interpolator;
    if (!original) return 0.0F;
    return held_rotation_yaw(state, actor, alpha, original(actor, alpha));
}

float hook_head_yaw(void* actor, float alpha) {
    CallbackScope callback;
    auto& state = callback.state;
    const auto original = state.head_yaw_interpolator;
    if (!original) return 0.0F;
    return held_rotation_yaw(state, actor, alpha, original(actor, alpha));
}

void hook_model_near_queue(model_native::NearActorQueueCallback* candidate_callback,
                           model_native::PlayerCandidate* actor) {
    CallbackScope callback;
    auto& state = callback.state;
    const auto original = state.model_near_queue;
    if (!original) return;
    if (state.stopping.load(std::memory_order_acquire) || inside_model_probe) {
        return original(candidate_callback, actor);
    }
    FlagScope entered(inside_model_probe);
    // Caller 0x144692C93 ignores return and advances to the next actor. This
    // is not the rejected addPlayers bool callback. No game fields are written.
    if (state.model_probe.inspect_near(candidate_callback, actor, model_actor_is_local, &state)) return;
    return original(candidate_callback, actor);
}

void hook_model_ecs_queue(model_native::EcsActorQueueContext* context,
                          model_native::ActorOwnerRef* component) {
    CallbackScope callback;
    auto& state = callback.state;
    const auto original = state.model_ecs_queue;
    if (!original) return;
    if (state.stopping.load(std::memory_order_acquire) || inside_model_probe)
        return original(context, component);
    FlagScope entered(inside_model_probe);
    // Native 0x1447172E9 reads the actor at component +0. Probe performs a
    // checked copy only while enabled; failure/nonlocal/OFF passes unchanged.
    if (state.model_probe.inspect_ecs(context, component, model_actor_is_local, &state)) return;
    return original(context, component);
}

void hook_model_data_render(model_native::DataDrivenRenderer* renderer,
                            model_native::BaseActorRenderContext* context,
                            model_native::ActorRenderData* data) {
    CallbackScope callback;
    auto& state = callback.state;
    const auto original = state.model_data_render;
    if (!original) return;
    if (state.stopping.load(std::memory_order_acquire) || inside_model_probe)
        return original(renderer, context, data);
    FlagScope entered(inside_model_probe);
    // CustomModel replaces native arms only after its arm-only draw succeeded.
    // Failed/missing current pose input immediately restores native hands.
    // Proven void(this, context*, renderData*) ABI; actor at renderData +0.
    const bool allow_suppression=!state.model_box.custom_model_mode()||state.model_box.first_person_ready();
    if (state.model_probe.inspect_render(renderer, context, data, model_actor_is_local, &state,allow_suppression)) return;
    return original(renderer, context, data);
}

void Runtime::connect_rotation() {
    if (stopping || !gameplay_ready.load(std::memory_order_acquire)) return;
    std::string view_diagnostic;
    const bool view_ready = sdk::custom_model::initialize_view_reader(view_diagnostic);
    report("Rotations view-mode guard: " + view_diagnostic, !view_ready);
    const auto body_target = gameplay.body_yaw_interpolator_target();
    const auto head_target = gameplay.head_yaw_interpolator_target();
    if (!body_target || !head_target) {
        report("Rotations: verified head/body-yaw interpolation targets are unavailable.", true);
        return;
    }
    std::string error;
    if (rotation_hooks.size() && rotation_hooks.size() != 2) {
        if (!rotation_hooks.clear(error)) {
            report("Rotations hook cleanup failed: " + error, true);
            return;
        }
        body_yaw_interpolator = nullptr;
        head_yaw_interpolator = nullptr;
    }
    if (!rotation_hooks.size()) {
        if (!rotation_hooks.create("Gameplay body-yaw interpolation", reinterpret_cast<void*>(body_target),
                reinterpret_cast<void*>(&hook_body_yaw), reinterpret_cast<void**>(&body_yaw_interpolator), error)) {
            report("Rotations hook unavailable: " + error, true);
            return;
        }
        if (!rotation_hooks.create("Gameplay head-yaw interpolation", reinterpret_cast<void*>(head_target),
                reinterpret_cast<void*>(&hook_head_yaw), reinterpret_cast<void**>(&head_yaw_interpolator), error)) {
            report("Rotations hook unavailable: " + error, true);
            std::string cleanup;
            if (!rotation_hooks.clear(cleanup)) report("Rotations hook cleanup failed: " + cleanup, true);
            else {
                body_yaw_interpolator = nullptr;
                head_yaw_interpolator = nullptr;
            }
            return;
        }
    }
    if (rotation_hooks.size() != 2) {
        report("Rotations hook unavailable: expected both verified head/body-yaw hooks.", true);
        return;
    }
    if (!rotation_hooks.enable_all(error)) {
        report("Rotations hook could not be enabled: " + error, true);
        std::string cleanup;
        if (!rotation_hooks.clear(cleanup)) report("Rotations hook cleanup failed: " + cleanup, true);
        else {
            body_yaw_interpolator = nullptr;
            head_yaw_interpolator = nullptr;
        }
        return;
    }
    report("Rotations head/body render interpolation hooks ready (Minecraft 1.26.52.3).");
}

void Runtime::connect_model_probe() {
    if (stopping) return;
    // connect_chat has finished publishing its immutable, validated current-
    // client/local-player bridge before any render callback can use it.
    if (!chat_installed) {
        model_probe.set_ready(false);
        report("P0 probe unavailable: validated local-player bridge is not ready.");
        return;
    }
    std::string error;
    if (!model_registered) {
        const auto targets = model_probe.locate(error);
        if (!targets) { report("CustomModel: " + error); return; }
        report("CustomModel: " + error);
        // Register all three disabled before enabling any. A missing/ambiguous
        // target prevents a partially connected combined experiment.
        if (!model_hooks.create("CustomModel P0 near actor queue", reinterpret_cast<void*>(targets.near_actor),
                reinterpret_cast<void*>(&hook_model_near_queue), reinterpret_cast<void**>(&model_near_queue), error) ||
            !model_hooks.create("CustomModel P0 ECS actor queue", reinterpret_cast<void*>(targets.ecs_actor),
                reinterpret_cast<void*>(&hook_model_ecs_queue), reinterpret_cast<void**>(&model_ecs_queue), error) ||
            !model_hooks.create("CustomModel P0 combined DataDriven render", reinterpret_cast<void*>(targets.data_driven),
                reinterpret_cast<void*>(&hook_model_data_render), reinterpret_cast<void**>(&model_data_render), error)) {
            model_probe.set_ready(false);
            report("P0 queue hooks unavailable: " + error, true);
            std::string cleanup_error;
            if (!model_hooks.clear(cleanup_error)) report("P0 queue registration cleanup: " + cleanup_error, true);
            else { model_near_queue = nullptr; model_ecs_queue = nullptr; model_data_render = nullptr; }
            return;
        }
        model_registered = true;
    }
    if (!model_hooks.enable_all(error)) {
        model_probe.set_ready(false);
        report("P0 queue hooks unavailable: " + error, true);
        return;
    }
    model_probe.set_ready(true);
    report("CustomModel P0 combined hooks ready (RVAs 0x4716E80 / 0x47172D0 / 0x558D300); F8 toggles local body and arms. Combined coverage unverified.");
}

void hook_model_world_render(void* camera, void* context, const void* view, void* client) {
    CallbackScope callback;
    auto& state = callback.state;
    const auto original = state.model_world_render;
    if (!original) return;
    if (state.stopping.load(std::memory_order_acquire) || !state.model_box.enabled())
        return original(camera,context,view,client);
    const auto view_mode = sdk::custom_model::read_local_view(state.bedrock_chat.mouse_client());
    sdk::custom_model::BoxRenderer::WorldScope world(state.model_box,context,state.bedrock_chat.local_player_for_render(),view_mode);
    original(camera,context,view,client);
    if (!state.stopping.load(std::memory_order_acquire)) state.model_box.finish_world(state.model_mesh_immediate);
}

void hook_model_mesh_immediate(void* screen, void* tess, const void* material, const void* texture, const void* metadata) {
    CallbackScope callback;
    auto& state = callback.state;
    const auto original = state.model_mesh_immediate;
    if (!original) return;
    original(screen,tess,material,texture,metadata);
    if (!state.stopping.load(std::memory_order_acquire)) state.model_box.after_submit(screen,tess,texture,metadata,original);
}

void hook_model_mesh_view(void* screen, void* tess, const void* material, const void* texture, std::uint16_t view, const void* metadata) {
    CallbackScope callback;
    auto& state = callback.state;
    const auto original = state.model_mesh_view;
    if (!original) return;
    original(screen,tess,material,texture,view,metadata);
    if (!state.stopping.load(std::memory_order_acquire))
        state.model_box.after_submit(screen,tess,texture,metadata,state.model_mesh_immediate);
}

void hook_model_native_submit(void* mesh, void* context, const void* material, void* bindings,
    std::uintptr_t unused5, std::uintptr_t unused6, const void* metadata, std::uintptr_t unused8, void* optional) {
    CallbackScope callback;
    auto& state=callback.state;
    const auto original=state.model_native_submit;
    if (!original) return;
    if (!state.stopping.load(std::memory_order_acquire))
        state.model_box.before_mesh_submit(context,bindings,metadata,state.model_mesh_immediate);
    return original(mesh,context,material,bindings,unused5,unused6,metadata,unused8,optional);
}

void Runtime::connect_model_box() {
    if (stopping || model_probe.status() != sdk::custom_model::Probe::Status::ready) return;
    std::string error;
    if (!model_box_registered) {
        const auto targets = sdk::custom_model::locate_box_targets(error);
        if (!targets) { report("CustomModel " + error,true); return; }
        model_box.configure(targets);
        std::string view_diagnostic;
        const bool view_ready=sdk::custom_model::initialize_view_reader(view_diagnostic);
        report(view_diagnostic,!view_ready);
        // Append disabled P1 hooks after P0; existing hook order is preserved.
        if (!model_box_hooks.create("CustomModel P1 world render",reinterpret_cast<void*>(targets.world),
                reinterpret_cast<void*>(&hook_model_world_render),reinterpret_cast<void**>(&model_world_render),error) ||
            !model_box_hooks.create("CustomModel P1 native mesh submit",reinterpret_cast<void*>(targets.mesh),
                reinterpret_cast<void*>(&hook_model_mesh_immediate),reinterpret_cast<void**>(&model_mesh_immediate),error) ||
            !model_box_hooks.create("CustomModel P1 native mesh view submit",reinterpret_cast<void*>(targets.mesh_view),
                reinterpret_cast<void*>(&hook_model_mesh_view),reinterpret_cast<void**>(&model_mesh_view),error) ||
            !model_box_hooks.create("CustomModel P1 native MeshSubmit",reinterpret_cast<void*>(targets.submit),
                reinterpret_cast<void*>(&hook_model_native_submit),reinterpret_cast<void**>(&model_native_submit),error)) {
            model_box.set_ready(false);
            report("CustomModel P1 hook registration failed: " + error,true);
            std::string cleanup;
            if (!model_box_hooks.clear(cleanup)) report("P1 registration cleanup: " + cleanup,true);
            else { model_world_render=nullptr; model_mesh_immediate=nullptr; model_mesh_view=nullptr; model_native_submit=nullptr; }
            return;
        }
        model_box_registered=true;
        report("CustomModel " + error);
    }
    if (!model_box_hooks.enable_all(error)) { model_box.set_ready(false); report("P1 enable failed: " + error,true); return; }
    model_box.set_ready(true);
    report("CustomModel P1 hooks ready. P1 Test Box / Material / Brightness / Show Debug are in CustomModel. Visual validation pending.");
}

void* hook_model_texture_get(void* group,void* output,const void* location,bool flag,std::uint64_t optional,std::uint8_t mode) {
    CallbackScope callback;auto& state=callback.state;const auto original=state.model_texture_get;
    if (!original) return output;
    auto* result=original(group,output,location,flag,optional,mode);
    if (!state.stopping.load(std::memory_order_acquire)) state.model_box.observe_texture_group(group,original);
    return result;
}
void Runtime::connect_model_static() {
    if (stopping||!model_box.ready()) return;
    std::string error;
    if (!model_static_registered) {
        const auto targets=sdk::custom_model::locate_static_targets(error);
        if (!targets) { report("CustomModel "+error,true);return; }
        model_box.configure_static(targets,model_assets);
        std::string animation_diagnostic;const bool animation_reader=sdk::custom_model::initialize_animation_reader(animation_diagnostic);
        report(animation_diagnostic,!animation_reader);
        if (!model_static_hooks.create("CustomModel P2 async texture request",reinterpret_cast<void*>(targets.texture_get),
            reinterpret_cast<void*>(&hook_model_texture_get),reinterpret_cast<void**>(&model_texture_get),error)) {
            report("P2 hook registration failed: "+error,true);return;
        }
        model_static_registered=true;
    }
    if (!model_static_hooks.enable_all(error)) { report("P2 enable failed: "+error,true);return; }
    const auto data_directory=Client::default_config_path().parent_path();
    std::array<std::filesystem::path,medicon::custom_model::builtin_model_count> directories;
    for(unsigned i=0;i<directories.size();++i)
        directories[i]=data_directory/L"models"/std::filesystem::u8path(medicon::custom_model::builtin_model_directory(static_cast<medicon::custom_model::BuiltinModel>(i)));
    HMODULE own{};std::array<wchar_t,32768> filename{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&hook_model_texture_get),&own)&&GetModuleFileNameW(own,filename.data(),static_cast<DWORD>(filename.size()))) {
        for(unsigned i=0;i<directories.size();++i) {
            const auto external=std::filesystem::path(filename.data()).parent_path()/L"models"/
                std::filesystem::u8path(medicon::custom_model::builtin_model_directory(static_cast<medicon::custom_model::BuiltinModel>(i)));
            if (std::filesystem::is_regular_file(external/L"model.fbx")) directories[i]=external;
        }
    }
    model_assets.start_catalog(std::move(directories),data_directory/L"cache"/L"models");
    report("CustomModel worker started: bundled Neon / Chocolat; Model selects the character. Mode=Custom model; vanilla retained until ready.");
}

void hook_send_packet(void* sender, void* packet) {
    CallbackScope callback;
    auto& state = callback.state;
    if (!state.stopping && state.bedrock_chat.consume(packet, [&state](std::string input) {
            std::lock_guard lock(state.command_mutex);
            // Commands stay local even if a producer overruns the bounded queue.
            if (state.pending_commands.size() < 64) state.pending_commands.push_back(std::move(input));
        })) return;
    if (!state.stopping.load(std::memory_order_acquire)) {
        const auto sprint_result = state.gameplay.rewrite_sprint_input(packet,
            state.sprint_packet_override_enabled.load(std::memory_order_acquire),
            state.sprint_packet_requested.load(std::memory_order_acquire));
        if (sprint_result == sdk::SprintInputRewrite::rewritten &&
            !state.sprint_packet_route_reported.exchange(true, std::memory_order_acq_rel))
            state.report("Sprint PlayerAuthInput rewrite active (packet id 144, input flags at +0x98).");
        else if (sprint_result == sdk::SprintInputRewrite::layout_rejected &&
            !state.sprint_packet_layout_error_reported.exchange(true, std::memory_order_acq_rel))
            state.report("Sprint PlayerAuthInput layout check failed; packet left unchanged.", true);
    }
    state.send_packet(sender, packet);
}

void hook_base_tick(void* actor) {
    CallbackScope callback;
    auto& state = callback.state;
    std::unique_lock dispatch(state.feedback_dispatch_mutex, std::try_to_lock);
    if (dispatch.owns_lock() && !state.stopping && state.feedback_count && state.bedrock_chat.is_local_player(actor)) {
        try {
            std::deque<std::string> replies;
            { std::lock_guard lock(state.feedback_mutex); replies.swap(state.pending_feedback); state.feedback_count = 0; }
            for (auto reply = replies.begin(); reply != replies.end(); ++reply) {
                std::string error;
                if (!state.bedrock_chat.display_message(actor, *reply, error)) {
                    if (!error.empty() && state.feedback_error != error) { state.report(error, true); state.feedback_error = error; }
                    std::lock_guard lock(state.feedback_mutex);
                    // Keep undelivered responses for the next valid player tick.
                    state.pending_feedback.insert(state.pending_feedback.begin(), reply, replies.end());
                    while (state.pending_feedback.size() > 64) state.pending_feedback.pop_back();
                    state.feedback_count = static_cast<unsigned>(state.pending_feedback.size());
                    break;
                }
            }
        } catch (const std::exception& error) { state.report(std::string("Native chat feedback: ") + error.what(), true); }
        catch (...) { /* Never interrupt the game's actor update. */ }
    }
    state.base_tick(actor);
    // The original actor update must finish before reading the current world.
    // Native mutations stay on this thread; Present only receives copied data.
    if (state.stopping || inside_gameplay || !state.gameplay_ready.load(std::memory_order_acquire)) return;
    FlagScope recursion(inside_gameplay);
    // Present holds this while it builds a frame. Skipping the tick then made
    // Scaffold and Killaura miss ticks and ESP/ChinaHat go stale; a short,
    // bounded wait keeps them on every tick without risking a deadlock.
    std::unique_lock render(state.render_mutex, std::defer_lock);
    (void)render.try_lock_for(std::chrono::milliseconds(4));
    if (!render.owns_lock() || !state.client || !state.bedrock_chat.is_local_player(actor)) return;
    const auto* sprint = dynamic_cast<const SprintModule*>(state.client->modules().find(SprintModule::module_id));
    bool wanted = sprint && sprint->needs_restore();
    for (const auto* id : {"chinahat", "derp", "aimbot", "killaura", "scaffold", "esp", "rotations", "sprint"}) {
        const auto* module = state.client->modules().find(id);
        if (module && module->enabled()) { wanted = true; break; }
    }
    // A slot Scaffold's Auto Select switched to is put back by a later capture,
    // also after every gameplay module was turned off.
    if (!wanted && !state.gameplay.slot_restore_pending()) {
        state.sprint_packet_override_enabled.store(false, std::memory_order_release);
        (void)restore_rotation_render_pitch(state, actor);
        state.rotation_override_render_component.store(0, std::memory_order_release);
        state.rotation_override_component.store(0, std::memory_order_release);
        state.rotation_override_enabled.store(false, std::memory_order_release);
        state.client->clear_gameplay();
        state.gameplay.clear();
        state.last_game_tick = {};
        return;
    }
    struct EndTick {
        sdk::BedrockGameplay& game;
        ~EndTick() { game.end_tick(); }
    } end_tick{state.gameplay};
    try {
        const auto* aim = dynamic_cast<AimbotModule*>(state.client->modules().find(AimbotModule::module_id));
        const auto* aura = dynamic_cast<KillauraModule*>(state.client->modules().find(KillauraModule::module_id));
        const bool visibility = (aim && aim->enabled() && aim->visible_only.get<bool>()) ||
            (aura && aura->enabled() && aura->visible_only.get<bool>());
        if (!state.gameplay.capture(state.bedrock_chat.mouse_client(), actor, visibility)) {
            state.sprint_packet_override_enabled.store(false, std::memory_order_release);
            (void)restore_rotation_render_pitch(state, actor);
            state.rotation_override_render_component.store(0, std::memory_order_release);
            state.rotation_override_component.store(0, std::memory_order_release);
            state.rotation_override_enabled.store(false, std::memory_order_release);
            state.client->clear_gameplay();
            state.last_game_tick = {};
            const auto& diagnostic = state.gameplay.diagnostic();
            if (!diagnostic.empty() && diagnostic != state.gameplay_diagnostic) {
                state.report("Gameplay: " + diagnostic);
                state.gameplay_diagnostic = diagnostic;
            }
            return;
        }
        const auto* hat = state.client->modules().find("chinahat");
        const auto* esp = state.client->modules().find("esp");
        const bool missing_camera = !state.gameplay.frame().projection_ready &&
            ((hat && hat->enabled()) || (esp && esp->enabled()));
        if (missing_camera) {
            const auto& diagnostic = state.gameplay.diagnostic();
            if (diagnostic != state.gameplay_diagnostic) {
                state.report("Gameplay: " + diagnostic);
                state.gameplay_diagnostic = diagnostic;
            }
        } else if (!state.gameplay_diagnostic.empty()) {
            state.report("Gameplay world components ready.");
            state.gameplay_diagnostic.clear();
        }
        const auto now = std::chrono::steady_clock::now();
        const float seconds = state.last_game_tick == std::chrono::steady_clock::time_point{}
            ? 0.05F : std::clamp(std::chrono::duration<float>(now - state.last_game_tick).count(), 0.001F, 0.1F);
        state.last_game_tick = now;
        std::string diagnostic;
        std::optional<bool> grabbed;
        {
            // The window thread owns native mouse transitions and their cached
            // validation state. Never wait for it while holding render_mutex.
            std::unique_lock mouse(state.mouse_mutex, std::try_to_lock);
            if (mouse.owns_lock()) grabbed = state.bedrock_chat.mouse_grabbed(diagnostic);
        }
        const bool allow_actions = !state.input_open() && grabbed.value_or(false) &&
            state.window_focused_cached.load(std::memory_order_acquire);
        state.client->game_tick(state.gameplay, seconds, allow_actions);
        const auto* killaura_module = dynamic_cast<const KillauraModule*>(
            state.client->modules().find(KillauraModule::module_id));
        std::uintptr_t killaura_target_id = 0;
        float killaura_target_yaw = 0.0F, killaura_target_pitch = 0.0F;
        bool killaura_rotation_applied = false;
        if (allow_actions && killaura_module && killaura_module->enabled() &&
            killaura_module->rotation_diagnostic(killaura_target_id, killaura_target_yaw,
                                                  killaura_target_pitch, killaura_rotation_applied)) {
            const auto now_ms = GetTickCount64();
            if (state.killaura_rotation_diagnostic_last == 0 ||
                now_ms - state.killaura_rotation_diagnostic_last >= 500) {
                char message[224]{};
                std::snprintf(message, sizeof(message),
                    "Killaura rotation target: id=0x%llX yaw=%.2f pitch=%.2f view_pitch=%.2f applied=%s",
                    static_cast<unsigned long long>(killaura_target_id), killaura_target_yaw,
                    killaura_target_pitch, state.gameplay.frame().pitch,
                    killaura_rotation_applied ? "yes" : "no");
                state.report(message);
                state.killaura_rotation_diagnostic_last = now_ms;
            }
        } else {
            state.killaura_rotation_diagnostic_last = 0;
        }
        const auto* sprint_module = dynamic_cast<const SprintModule*>(
            state.client->modules().find(SprintModule::module_id));
        state.sprint_packet_requested.store(sprint_module && sprint_module->requested_sprinting(),
            std::memory_order_release);
        state.sprint_packet_override_enabled.store(sprint_module && sprint_module->packet_sprint_override(),
            std::memory_order_release);
        sdk::RotationDiagnostic pitch{};
        const bool pitch_diagnostic_ready = state.gameplay.rotation_diagnostic(pitch);
        float previous_yaw = 0.0F, requested_yaw = 0.0F;
        const bool rotation_requested = state.gameplay.rotation_override(previous_yaw, requested_yaw);
        const bool body_getter_matches = rotation_requested &&
            state.gameplay.validates_body_yaw_interpolator(actor);
        const bool head_getter_matches = rotation_requested &&
            state.gameplay.validates_head_yaw_interpolator(actor);
        if (rotation_requested && body_getter_matches && head_getter_matches) {
            state.rotation_override_actor.store(reinterpret_cast<std::uintptr_t>(actor), std::memory_order_relaxed);
            const auto yaws = static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(previous_yaw)) |
                (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(requested_yaw)) << 32);
            state.rotation_override_yaws.store(yaws, std::memory_order_release);
            if (pitch_diagnostic_ready && pitch.render_component) {
                const auto pitches = static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(pitch.previous_pitch)) |
                    (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(pitch.requested_pitch)) << 32);
                state.rotation_override_pitches.store(pitches, std::memory_order_release);
                state.rotation_override_render_component.store(pitch.render_component, std::memory_order_release);
                state.rotation_override_component.store(pitch.rotation_component, std::memory_order_release);
            } else {
                state.rotation_override_render_component.store(0, std::memory_order_release);
                state.rotation_override_component.store(0, std::memory_order_release);
            }
            state.rotation_override_enabled.store(true, std::memory_order_release);
        } else {
            (void)restore_rotation_render_pitch(state, actor);
            state.rotation_override_render_component.store(0, std::memory_order_release);
            state.rotation_override_component.store(0, std::memory_order_release);
            state.rotation_override_enabled.store(false, std::memory_order_release);
            if (rotation_requested && !body_getter_matches && !state.rotation_getter_identity_reported) {
                state.report("Rotations render hold withheld: local actor body-yaw getter did not match its verified profile. " +
                    state.gameplay.rotation_getter_diagnostic(actor), true);
                state.rotation_getter_identity_reported = true;
            } else if (rotation_requested && !head_getter_matches && !state.rotation_getter_identity_reported) {
                state.report("Rotations render hold withheld: verified head-yaw getter was not found in the local actor vtable. " +
                    state.gameplay.rotation_getter_diagnostic(actor), true);
                state.rotation_getter_identity_reported = true;
            }
        }
        if (rotation_requested && state.rotation_override_enabled.load(std::memory_order_acquire)) {
            const auto now = GetTickCount64();
            if (state.pitch_diagnostic_count < 12 || now - state.pitch_diagnostic_last >= 500) {
                if (pitch_diagnostic_ready) {
                    char message[256]{};
                    std::snprintf(message, sizeof(message),
                        "Rotations pitch: requested=%.2f previous=%.2f | RotationComponent current=%.2f previous=%.2f | RenderRotation current=%.2f yaw=%.2f",
                        pitch.requested_pitch, pitch.previous_pitch, pitch.view_pitch, pitch.view_previous_pitch,
                        pitch.render_pitch, pitch.render_yaw);
                    state.report(message);
                } else {
                    state.report("Rotations pitch: diagnostic components unavailable.");
                }
                state.pitch_diagnostic_last = now;
                ++state.pitch_diagnostic_count;
            }
        } else {
            state.pitch_diagnostic_last = 0;
            state.pitch_diagnostic_count = 0;
        }
    } catch (const std::exception& error) {
        state.sprint_packet_override_enabled.store(false, std::memory_order_release);
        (void)restore_rotation_render_pitch(state, actor);
        state.rotation_override_render_component.store(0, std::memory_order_release);
        state.rotation_override_component.store(0, std::memory_order_release);
        state.rotation_override_enabled.store(false, std::memory_order_release);
        state.client->clear_gameplay();
        state.report(std::string("Gameplay update: ") + error.what(), true);
    } catch (...) {
        state.sprint_packet_override_enabled.store(false, std::memory_order_release);
        (void)restore_rotation_render_pitch(state, actor);
        state.rotation_override_render_component.store(0, std::memory_order_release);
        state.rotation_override_component.store(0, std::memory_order_release);
        state.rotation_override_enabled.store(false, std::memory_order_release);
        state.client->clear_gameplay();
    }
}

void hook_grab_mouse(void* client) {
    CallbackScope callback;
    auto& state = callback.state;
    if (!state.stopping.load(std::memory_order_acquire) && state.mouse_ready.load(std::memory_order_acquire) &&
        state.input_open() && (state.modal_input() || !state.native_input_ready.load(std::memory_order_acquire))) {
        const auto identity = state.mouse_identity.load(std::memory_order_acquire);
        // Pointer compare only. Revalidating vtables here runs on the game's
        // grab path and hitches every frame while the ClickGUI is open.
        if (!identity || client == reinterpret_cast<void*>(identity)) return;
    }
    state.grab_mouse(client);
}

void hook_mouse_feed(void* device, std::uint8_t button, int data, std::int16_t x, std::int16_t y,
                     std::int16_t dx, std::int16_t dy, std::uint8_t motionless) {
    CallbackScope callback;
    auto& state = callback.state;
    sdk::input::forward_mouse_feed(state.blocks_native_mouse(), state.mouse_feed,
                                   device, button, data, x, y, dx, dy, motionless);
}

void hook_look_input(void* entity, const void* camera, void* direct_look, const sdk::input::LookDelta* delta) {
    CallbackScope callback;
    auto& state = callback.state;
    sdk::input::forward_look_input(state.blocks_native_mouse(), state.look_input, entity, camera, direct_look, delta);
}

void Runtime::connect_native_input() {
    if (stopping) return;
    std::string error;
    if (!native_input_registered) {
        const auto targets = sdk::input::locate_targets(error);
        if (!targets) { report(error); return; }
        if (!native_input_hooks.create("Native GUI mouse feed gate", reinterpret_cast<void*>(targets.mouse_feed),
                reinterpret_cast<void*>(&hook_mouse_feed), reinterpret_cast<void**>(&mouse_feed), error) ||
            !native_input_hooks.create("Native GUI camera look gate", reinterpret_cast<void*>(targets.look_input),
                reinterpret_cast<void*>(&hook_look_input), reinterpret_cast<void**>(&look_input), error)) {
            report("Native input gate: " + error, true);
            std::string ignored;
            native_input_hooks.clear(ignored);
            return;
        }
        native_input_registered = true;
    }
    if (!native_input_hooks.enable_all(error)) { report("Native input gate: " + error, true); return; }
    native_input_ready.store(true, std::memory_order_release);
    report("Native GUI input gate ready: MouseDevice::feed and camera look deltas blocked while GUI owns input.");
}

std::uint32_t hook_main_menu_update(void* controller) {
    // MinHook jumps here, retaining the native caller's return address. Capture
    // it in this function, not an out-of-line helper.
#if defined(_MSC_VER)
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#endif
    CallbackScope callback;
    auto& state = callback.state;
    FlagScope recursion(inside_navigation);
    const auto result = state.main_menu_update(controller);
    if (recursion.previous || inside_overlay || inside_present || state.stopping ||
        !state.navigation_ready.load(std::memory_order_acquire) || !state.navigation.has_pending()) return result;
    try {
        if (!state.menu_visible.load(std::memory_order_acquire) ||
            !state.window_focused_cached.load(std::memory_order_acquire)) {
            state.navigation.cancel();
            // Once per request: cancel() clears the pending flag checked above.
            state.report(state.menu_visible.load(std::memory_order_acquire) ?
                "MainMenu navigation: request cancelled; the game window is not focused." :
                "MainMenu navigation: request cancelled; the replacement menu is hidden.");
            return result;
        }
        // The original update can itself push a popup or leave Main Menu.
        // Reacquire the entire route after it returns. Never reuse controller
        // or callable pointers from a button callback or a previous frame.
        if (state.menu_screen(true) != sdk::MenuScreenState::main_menu) {
            state.publish_menu_visible(false);
            state.navigation.cancel();
            return result;
        }
        sdk::MenuSessionIdentity session;
        {
            std::lock_guard lock(state.menu_mutex);
            session = state.cached_menu_session_;
        }
        std::string diagnostic;
        const bool invoked = state.navigation.pump(controller, caller, session, state.menu_epoch,
                                                   state.stopping, diagnostic);
        if (invoked) {
            const auto thread = GetCurrentThreadId();
            if (state.navigation_thread.exchange(thread) != thread)
                state.report("MainMenu native UI update thread observed: " + std::to_string(thread) + ".");
        }
        if (!diagnostic.empty()) state.report("MainMenu navigation: " + diagnostic);
        // Invocation may have destroyed the old scene/controller. Only local
        // values and our process-lifetime Runtime are used after this point.
    } catch (...) {
        state.navigation.cancel();
        state.report("MainMenu navigation inspection failed; request cancelled.", true);
    }
    return result; // Preserve the original native update result mask.
}

void Runtime::connect_navigation() {
    if (stopping || navigation_ready.load(std::memory_order_acquire)) return;
    std::string error;
    if (!navigation.initialize(error)) {
        if (!error.empty()) report("MainMenu navigation disabled: " + error);
        return;
    }
    if (!navigation_registered) {
        const auto target = navigation.update_hook_target();
        if (!target || !navigation_hooks.create("MainMenu native controller update", reinterpret_cast<void*>(target),
                reinterpret_cast<void*>(&hook_main_menu_update), reinterpret_cast<void**>(&main_menu_update), error)) {
            navigation.stop();
            report("MainMenu navigation hook unavailable: " + error, true);
            return;
        }
        navigation_registered = true;
    }
    if (!navigation_hooks.enable_all(error)) {
        navigation.stop();
        report("MainMenu navigation hook unavailable: " + error, true);
        return;
    }
    navigation_ready.store(true, std::memory_order_release);
    report("MainMenu native controller update hook ready; buttons queue actions only.");
}

void Runtime::connect_mouse() {
    if (stopping || mouse_ready) return;
    std::string error;
    if (!mouse_registered) {
        const auto target = bedrock_chat.grab_mouse_target(error);
        if (!target) return; // The native profile/client can appear before chat.
        if (!mouse_hooks.create("ClickGUI native mouse grab", reinterpret_cast<void*>(target),
                reinterpret_cast<void*>(&hook_grab_mouse), reinterpret_cast<void**>(&grab_mouse), error)) {
            report("Native ClickGUI mouse hook: " + error, true);
            return;
        }
        mouse_registered = true;
    }
    if (!mouse_hooks.enable_all(error)) { report("Native ClickGUI mouse hook: " + error, true); return; }
    mouse_ready = true;
    request_mouse_sync();
    report("Minecraft native ClickGUI releaseMouse/grabMouse ready.");
}

void Runtime::connect_chat_feedback() {
    if (!chat_installed && !gameplay_ready.load(std::memory_order_acquire)) return;
    if (tick_registered) {
        std::string error;
        tick_installed = tick_hooks.enable_all(error);
        if (!tick_installed) report("Native chat feedback hook: " + error, true);
        return;
    }
    std::string previous;
    while (!stopping) {
        std::string diagnostic;
        const auto target = bedrock_chat.tick_target(diagnostic, !gameplay_ready.load(std::memory_order_acquire));
        if (!diagnostic.empty() && diagnostic != previous) { report(diagnostic); previous = diagnostic; }
        if (target) {
            std::string error;
            if (!tick_hooks.create("Local player gameplay and chat", reinterpret_cast<void*>(target),
                    reinterpret_cast<void*>(&hook_base_tick), reinterpret_cast<void**>(&base_tick), error)) {
                report("Native chat feedback hook: " + error, true);
                return;
            }
            tick_registered = true;
            tick_installed = tick_hooks.enable_all(error);
            if (!tick_installed) report("Native chat feedback hook: " + error, true);
            else report("Minecraft local-player gameplay and chat tick ready.");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void Runtime::connect_chat() {
    if (chat_registered) {
        std::string error;
        chat_installed = chat_hooks.enable_all(error);
        if (!chat_installed) report("Chat hook: " + error, true);
        return;
    }
    std::string previous;
    // A player may stay at the title screen for minutes before the packet
    // sender exists. Keep waiting until the SDK is ready or the overlay stops.
    while (!stopping) {
        std::string diagnostic;
        const auto state = bedrock_chat.locate(diagnostic);
        if (!diagnostic.empty() && diagnostic != previous) { report(diagnostic); previous = diagnostic; }
        connect_mouse();
        if (state == sdk::BedrockChat::State::unsupported) return;
        if (state == sdk::BedrockChat::State::ready) {
            std::string error;
            if (!chat_hooks.create("Bedrock outgoing chat", reinterpret_cast<void*>(bedrock_chat.send_target()),
                    reinterpret_cast<void*>(&hook_send_packet), reinterpret_cast<void**>(&send_packet), error)) {
                report("Chat hook: " + error, true);
                return;
            }
            chat_registered = true;
            chat_installed = chat_hooks.enable_all(error);
            if (!chat_installed) { report("Chat hook: " + error, true); return; }
            report("Minecraft native chat commands ready: .bind <module> <key>.");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void draw_safely(IDXGISwapChain* swap, UINT flags, bool& drew) noexcept {
    try { drew = runtime().draw(swap, flags); }
    catch (const std::exception& error) {
        auto& state = runtime();
        std::lock_guard lock(state.render_mutex);
        state.suspend_menu();
        state.report(error.what(), true); state.status = 4;
    }
    catch (...) {
        auto& state = runtime();
        std::lock_guard lock(state.render_mutex);
        state.suspend_menu();
        state.report("Unexpected overlay exception.", true); state.status = 4;
    }
}

// Returns the entry timestamp for the Present() duration measurement.
std::uint64_t perf_present_begin(Runtime& state, IDXGISwapChain* swap, UINT flags, bool& drew) {
    const auto entry = perf_now();
    if (const auto last = state.perf.last_present.exchange(entry, std::memory_order_relaxed))
        state.perf.interval.add(entry - last);
    state.perf.present_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    draw_safely(swap, flags, drew);
    const auto drawn = perf_now();
    state.perf.draw.add(drawn - entry);
    return drawn;
}
void perf_present_end(Runtime& state, std::uint64_t call_start) {
    const auto now = perf_now();
    state.perf.present_call.add(now - call_start);
    try { state.report_perf(now); } catch (...) {}
}

HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain* swap, UINT sync, UINT flags) {
    CallbackScope callback;
    FlagScope recursion(inside_present);
    bool drew = false;
    if (recursion.previous) return callback.state.present(swap, sync, flags);
    const auto call_start = perf_present_begin(callback.state, swap, flags, drew);
    const auto result = callback.state.present(swap, sync, flags);
    perf_present_end(callback.state, call_start);
    return result;
}
HRESULT STDMETHODCALLTYPE hook_present1(IDXGISwapChain1* swap, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    CallbackScope callback;
    FlagScope recursion(inside_present);
    bool drew = false;
    if (recursion.previous) return callback.state.present1(swap, sync, flags, params);
    const auto call_start = perf_present_begin(callback.state, swap, flags, drew);
    DXGI_PRESENT_PARAMETERS full{};
    const auto result = callback.state.present1(swap, sync, flags, drew && params ? &full : params);
    perf_present_end(callback.state, call_start);
    return result;
}
HRESULT STDMETHODCALLTYPE hook_resize(IDXGISwapChain* swap, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    CallbackScope callback;
    // Keep a concurrent Present from reacquiring back buffers between resource
    // release and the real ResizeBuffers call. DXGI can nest ResizeBuffers1.
    std::lock_guard lock(callback.state.render_mutex);
    if (!callback.state.stopping) callback.state.before_resize(swap);
    return callback.state.resize(swap, count, width, height, format, flags);
}
HRESULT STDMETHODCALLTYPE hook_resize1(IDXGISwapChain3* swap, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags, const UINT* masks, IUnknown* const* queues) {
    CallbackScope callback;
    std::lock_guard lock(callback.state.render_mutex);
    if (!callback.state.stopping) callback.state.before_resize(swap);
    const auto result = callback.state.resize1(swap, count, width, height, format, flags, masks, queues);
    if (SUCCEEDED(result) && queues && count) callback.state.associate_queue(swap, queues[0]);
    return result;
}
void STDMETHODCALLTYPE hook_execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    CallbackScope callback;
    if (!inside_overlay && !callback.state.stopping) {
        const auto start = perf_now();
        try { callback.state.capture_queue(queue); } catch (...) { /* Never interrupt host submission. */ }
        callback.state.perf.exec.add(perf_now() - start);
    }
    callback.state.execute(queue, count, lists);
}
HRESULT STDMETHODCALLTYPE hook_create(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swap) {
    CallbackScope callback;
    const auto result = callback.state.create(factory, device, desc, swap);
    if (SUCCEEDED(result) && swap) callback.state.associate_queue(*swap, device);
    return result;
}
HRESULT STDMETHODCALLTYPE hook_create_hwnd(IDXGIFactory2* factory, IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output, IDXGISwapChain1** swap) {
    CallbackScope callback;
    const auto result = callback.state.create_hwnd(factory, device, window, desc, fullscreen, output, swap);
    if (SUCCEEDED(result) && swap) callback.state.associate_queue(*swap, device);
    return result;
}
HRESULT STDMETHODCALLTYPE hook_create_core(IDXGIFactory2* factory, IUnknown* device, IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** swap) {
    CallbackScope callback;
    const auto result = callback.state.create_core(factory, device, window, desc, output, swap);
    if (SUCCEEDED(result) && swap) callback.state.associate_queue(*swap, device);
    return result;
}
HRESULT STDMETHODCALLTYPE hook_create_composition(IDXGIFactory2* factory, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** swap) {
    CallbackScope callback;
    const auto result = callback.state.create_composition(factory, device, desc, output, swap);
    if (SUCCEEDED(result) && swap) callback.state.associate_queue(*swap, device);
    return result;
}
BOOL WINAPI hook_clip(const RECT* rect) {
    CallbackScope callback;
    auto& state = callback.state;
    // Swallow instead of calling ClipCursor(nullptr) on every game attempt.
    // That syscall, issued from the render thread, is what stutters while the GUI is open.
    if (state.cursor_captured_by_gui()) return TRUE;
    return state.clip(rect);
}
BOOL WINAPI hook_warp(int x, int y) {
    CallbackScope callback;
    auto& state = callback.state;
    if (state.cursor_captured_by_gui()) {
        state.spoof_x.store(x, std::memory_order_relaxed);
        state.spoof_y.store(y, std::memory_order_relaxed);
        state.spoof_valid.store(true, std::memory_order_release);
        return TRUE;
    }
    return state.warp(x, y);
}
BOOL WINAPI hook_get_cursor(LPPOINT point) {
    CallbackScope callback;
    auto& state = callback.state;
    if (!point || !state.cursor_captured_by_gui() || !state.spoof_valid.load(std::memory_order_acquire))
        return state.get_cursor(point);
    point->x = state.spoof_x.load(std::memory_order_relaxed);
    point->y = state.spoof_y.load(std::memory_order_relaxed);
    return TRUE;
}
int WINAPI hook_show_cursor(BOOL show) {
    CallbackScope callback;
    auto& state = callback.state;
    // Games hide the cursor with a ShowCursor(FALSE) loop. Returning immediately
    // stops that loop from running every frame while the ClickGUI is open.
    if (state.cursor_captured_by_gui()) return show ? 0 : -1;
    return state.show_cursor(show);
}
HWND WINAPI hook_get_foreground() {
    CallbackScope callback;
    auto& state = callback.state;
    // GetForegroundWindow from Present serializes on the window thread. While
    // the overlay owns input that thread is busy with raw mouse and cursor warps.
    if (state.cursor_captured_by_gui()) {
        const auto window = state.mouse_window.load(std::memory_order_acquire);
        if (window) return window;
    }
    return state.get_foreground ? state.get_foreground() : GetForegroundWindow();
}

void filter_game_raw_mouse(Runtime& state, RAWINPUT& raw) noexcept {
    if (raw.header.dwType != RIM_TYPEMOUSE || state.stopping.load(std::memory_order_acquire)) return;
    const bool captured = state.cursor_captured_by_gui();
    const auto flags = raw.data.mouse.usButtonFlags;
    constexpr std::array<USHORT, 5> down{RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN,
        RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN};
    constexpr std::array<USHORT, 5> up{RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP,
        RI_MOUSE_MIDDLE_BUTTON_UP, RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP};
    auto owned = state.owned_mouse_buttons.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < down.size(); ++i) {
        const unsigned bit = 1U << i;
        if (captured && (flags & down[i])) {
            state.owned_mouse_buttons.fetch_or(bit, std::memory_order_acq_rel);
            owned |= bit;
        }
        if (owned & bit) raw.data.mouse.usButtonFlags &= static_cast<USHORT>(~(down[i] | up[i]));
        if (flags & up[i]) state.owned_mouse_buttons.fetch_and(~bit, std::memory_order_acq_rel);
    }
    if (captured) suppress_raw_mouse(raw);
}

UINT WINAPI hook_get_raw_input(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT header_size) {
    CallbackScope callback;
    auto& state = callback.state;
    const UINT read = state.get_raw_input(input, command, data, size, header_size);
    if (command == RID_INPUT && data && read != static_cast<UINT>(-1) &&
        read >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE))
        filter_game_raw_mouse(state, *static_cast<RAWINPUT*>(data));
    return read;
}

UINT WINAPI hook_get_raw_buffer(PRAWINPUT data, PUINT size, UINT header_size) {
    CallbackScope callback;
    auto& state = callback.state;
    const UINT capacity = size ? *size : 0;
    const UINT count = state.get_raw_buffer(data, size, header_size);
    if (!data || count == static_cast<UINT>(-1)) return count;
    auto* packet = reinterpret_cast<std::byte*>(data);
    const auto* end = packet + capacity;
    for (UINT i = 0; i < count && static_cast<std::size_t>(end - packet) >= sizeof(RAWINPUTHEADER); ++i) {
        auto& raw = *reinterpret_cast<RAWINPUT*>(packet);
        const auto bytes = raw.header.dwSize;
        if (bytes < sizeof(RAWINPUTHEADER) || bytes > static_cast<std::size_t>(end - packet)) break;
        if (bytes >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE)) filter_game_raw_mouse(state, raw);
        const auto next_address = (reinterpret_cast<std::uintptr_t>(packet) + bytes + 7U) & ~std::uintptr_t{7};
        auto* next = reinterpret_cast<std::byte*>(next_address);
        if (next > end) break;
        packet = next;
    }
    return count;
}

SHORT filter_game_mouse_button(Runtime& state, int key, SHORT value, bool physical) noexcept {
    const unsigned button = mouse_button_mask(key);
    if (!button || state.stopping.load(std::memory_order_acquire)) return value;
    if (state.cursor_captured_by_gui()) {
        if (value & 0x8000) state.owned_mouse_buttons.fetch_or(button, std::memory_order_acq_rel);
        return 0;
    }
    if (state.owned_mouse_buttons.load(std::memory_order_acquire) & button) {
        if (physical && !(value & 0x8000)) state.owned_mouse_buttons.fetch_and(~button, std::memory_order_acq_rel);
        return 0;
    }
    return value;
}

SHORT WINAPI hook_get_async_key_state(int key) {
    CallbackScope callback;
    auto& state = callback.state;
    return filter_game_mouse_button(state, key, state.get_async_key_state(key), true);
}

SHORT WINAPI hook_get_key_state(int key) {
    CallbackScope callback;
    auto& state = callback.state;
    return filter_game_mouse_button(state, key, state.get_key_state(key), false);
}

template<typename T> void* method(T* object, unsigned index) { return (*reinterpret_cast<void***>(object))[index]; }
template<typename Fn> bool add_hook(Runtime& state, const char* name, void* address, Fn detour, Fn& original, std::string& error) {
    return state.hooks.create(name, address, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&original), error);
}

bool Runtime::install(std::string& error) {
    if (installed) return hooks.enable_all(error);
    // Obtain implementation addresses from a disposable local DXGI host; never
    // scan game memory or depend on version-specific offsets.
    HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* class_name = L"MediconHookProbe";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = instance; wc.lpszClassName = class_name;
    const ATOM atom = RegisterClassW(&wc);
    HWND probe = CreateWindowExW(0, class_name, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, instance, nullptr);
    struct ProbeWindow { HWND window; HINSTANCE instance; const wchar_t* name; ATOM atom;
        ~ProbeWindow() { if (window) DestroyWindow(window); if (atom) UnregisterClassW(name, instance); }
    } cleanup{probe, instance, class_name, atom};
    if (!probe) { error = "Cannot create DXGI probe window: " + std::to_string(GetLastError()); return false; }
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = 64; desc.BufferDesc.Height = 64;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2; desc.OutputWindow = probe; desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate;
    auto result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swap, &device, nullptr, &immediate);
    if (FAILED(result)) result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swap, &device, nullptr, &immediate);
    if (FAILED(result)) { error = "D3D11/DXGI hook probe failed: " + std::to_string(static_cast<unsigned>(result)); return false; }
    ComPtr<IDXGISwapChain1> swap1;
    ComPtr<IDXGISwapChain3> swap3;
    ComPtr<IDXGIFactory2> factory;
    swap.As(&swap1); swap.As(&swap3);
    swap->GetParent(IID_PPV_ARGS(&factory));
    if (!swap1 || !swap3 || !factory) { error = "DXGI 1.4 or newer is required for the overlay."; return false; }
    if (!add_hook(*this, "DXGI Present", method(swap.Get(), 8), hook_present, present, error) ||
        !add_hook(*this, "DXGI Present1", method(swap1.Get(), 22), hook_present1, present1, error) ||
        !add_hook(*this, "DXGI ResizeBuffers", method(swap.Get(), 13), hook_resize, resize, error) ||
        !add_hook(*this, "DXGI ResizeBuffers1", method(swap3.Get(), 39), hook_resize1, resize1, error) ||
        !add_hook(*this, "DXGI CreateSwapChain", method(factory.Get(), 10), hook_create, create, error) ||
        !add_hook(*this, "DXGI CreateSwapChainForHwnd", method(factory.Get(), 15), hook_create_hwnd, create_hwnd, error) ||
        !add_hook(*this, "DXGI CreateSwapChainForCoreWindow", method(factory.Get(), 16), hook_create_core, create_core, error) ||
        !add_hook(*this, "DXGI CreateSwapChainForComposition", method(factory.Get(), 24), hook_create_composition, create_composition, error)) return false;

    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    result = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12));
    if (FAILED(result)) {
        ComPtr<IDXGIAdapter> warp_adapter;
        ComPtr<IDXGIFactory4> factory4;
        factory.As(&factory4);
        if (factory4 && SUCCEEDED(factory4->EnumWarpAdapter(IID_PPV_ARGS(&warp_adapter))))
            result = D3D12CreateDevice(warp_adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12));
    }
    if (SUCCEEDED(result)) {
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        result = device12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    }
    if (queue && !add_hook(*this, "D3D12 ExecuteCommandLists", method(queue.Get(), 10), hook_execute, execute, error)) return false;
    if (!queue) report("D3D12 queue probe unavailable; only D3D11 rendering can start.");
    auto user32 = GetModuleHandleW(L"user32.dll");
    if (!add_hook(*this, "ClipCursor", reinterpret_cast<void*>(GetProcAddress(user32, "ClipCursor")), hook_clip, clip, error) ||
        !add_hook(*this, "SetCursorPos", reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos")), hook_warp, warp, error) ||
        !add_hook(*this, "GetCursorPos", reinterpret_cast<void*>(GetProcAddress(user32, "GetCursorPos")), hook_get_cursor, get_cursor, error) ||
        !add_hook(*this, "ShowCursor", reinterpret_cast<void*>(GetProcAddress(user32, "ShowCursor")), hook_show_cursor, show_cursor, error) ||
        !add_hook(*this, "GetRawInputData", reinterpret_cast<void*>(GetProcAddress(user32, "GetRawInputData")),
                  hook_get_raw_input, get_raw_input, error) ||
        !add_hook(*this, "GetRawInputBuffer", reinterpret_cast<void*>(GetProcAddress(user32, "GetRawInputBuffer")),
                  hook_get_raw_buffer, get_raw_buffer, error) ||
        !add_hook(*this, "GetAsyncKeyState", reinterpret_cast<void*>(GetProcAddress(user32, "GetAsyncKeyState")),
                  hook_get_async_key_state, get_async_key_state, error) ||
        !add_hook(*this, "GetKeyState", reinterpret_cast<void*>(GetProcAddress(user32, "GetKeyState")),
                  hook_get_key_state, get_key_state, error) ||
        !add_hook(*this, "GetForegroundWindow", reinterpret_cast<void*>(GetProcAddress(user32, "GetForegroundWindow")),
                  hook_get_foreground, get_foreground, error)) return false;
    if (!hooks.enable_all(error)) return false;
    installed = true;
    return true;
}

DWORD WINAPI start_worker(void*) {
    auto& state = runtime();
    try {
        if (!auto_unload_enabled.load(std::memory_order_acquire)) {
            HMODULE pinned{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(&start_worker), &pinned))
                throw std::runtime_error("Cannot keep overlay callbacks mapped.");
        }
        const auto default_config = Client::default_config_path();
        std::filesystem::create_directories(default_config.parent_path());
        {
            std::lock_guard lock(state.log_mutex);
            if (!state.log.is_open()) state.log.open(default_config.parent_path() / L"inject.log", std::ios::app);
            state.last_error.clear();
        }
        wchar_t override_path[32768]{};
        const auto length = GetEnvironmentVariableW(L"MEDICON_OVERLAY_CONFIG", override_path, static_cast<DWORD>(std::size(override_path)));
        state.config_path = length && length < std::size(override_path) ? std::filesystem::path(override_path) : default_config;
        state.report("Starting automatic overlay in process " + std::to_string(GetCurrentProcessId()) + (state.core_version.empty()
            ? std::string(" (DLL injected directly).") : " (Core " + state.core_version + " via MediconLoader)."));
        // The first overlay frame starts the intro clock. Resolve every native
        // profile (signature scan, menu PE profile, navigation hook) before
        // Present is hooked, so that frame can already observe the title
        // screen and the menu sits under the whole intro. Detecting it from
        // the later chat loop made the menu pop in as the intro ended.
        const auto profile_start = GetTickCount64();
        try { state.connect_navigation(); }
        catch (const std::exception& error) { state.report(std::string("MainMenu navigation unavailable: ") + error.what(), true); }
        try {
            std::string diagnostic;
            // Publishes the immutable client profile used by menu detection.
            // connect_chat() below reports the outcome and keeps waiting.
            (void)state.bedrock_chat.locate(diagnostic);
            if (!state.bedrock_menu.prepare(diagnostic) && !diagnostic.empty()) {
                state.report("MainMenu: " + diagnostic);
                std::lock_guard lock(state.menu_mutex);
                state.menu_diagnostic = diagnostic;
            }
        } catch (const std::exception& error) { state.report(std::string("Native profile preparation failed: ") + error.what(), true); }
        try {
            std::string diagnostic;
            state.gameplay_ready.store(state.gameplay.locate(diagnostic), std::memory_order_release);
            if (!diagnostic.empty()) state.report("Gameplay: " + diagnostic);
        } catch (const std::exception& error) { state.report(std::string("Gameplay profile unavailable: ") + error.what(), true); }
        state.report("Native profiles prepared in " + std::to_string(GetTickCount64() - profile_start) + " ms.");
        if (state.stopping) return 0;
        state.connect_native_input();
        // Created before Present is hooked so the first frame can attach Social
        // to the overlay; its threads start only once initialization is done.
        try {
            // Like MEDICON_OVERLAY_CONFIG: tests keep social.json/license.bin out of %APPDATA%.
            wchar_t social_override[32768]{};
            const auto social_length = GetEnvironmentVariableW(L"MEDICON_SOCIAL_DIR", social_override,
                                                               static_cast<DWORD>(std::size(social_override)));
            auto directory = social_length && social_length < std::size(social_override)
                ? std::filesystem::path(social_override) : social::SocialService::default_directory();
            auto service = std::make_unique<social::SocialService>(social::SocialService::Options{
                std::move(directory), MEDICON_VERSION, [&state](std::string_view message) { state.report(message); }});
            std::lock_guard lock(state.render_mutex);
            state.social = std::move(service);
        } catch (const std::exception& error) { state.report(std::string("Social unavailable: ") + error.what(), true); }
        std::string error;
        if (!state.install(error)) { state.report(error, true); state.status = 4; state.stopping = true; return 1; }
        // Present can already be drawing while install() finishes on this
        // worker. Never overwrite its rendering status with the ready state.
        unsigned starting = 1;
        state.status.compare_exchange_strong(starting, 2);
        state.report("DXGI hooks ready.");
        try { state.connect_rotation(); }
        catch (const std::exception& error) {
            state.report(std::string("Rotations hook unavailable: ") + error.what(), true);
        }
        // Spec 4.4: after Medicon finished initializing, never from DllMain.
        if (state.social) state.social->start();
        try { state.connect_mouse(); state.connect_chat(); state.connect_chat_feedback(); }
        catch (const std::exception& error) { state.report(std::string("Minecraft native chat SDK unavailable: ") + error.what(), true); }
        // Append P0 after the existing native startup sequence; preserve the
        // relative order and readiness contracts of every existing hook.
        try { state.connect_model_probe(); state.connect_model_box(); state.connect_model_static(); }
        catch (const std::exception& error) {
            state.model_probe.set_ready(false);
            state.model_box.set_ready(false);
            state.report(std::string("CustomModel P0 unavailable: ") + error.what(), true);
        }
        return 0;
    } catch (const std::exception& error) { state.report(error.what(), true); }
    catch (...) { state.report("Overlay bootstrap failed.", true); }
    state.stopping = true;
    state.model_probe.set_enabled(false);
    state.model_box.set_options(false,0,1);
    state.navigation_ready.store(false, std::memory_order_release);
    state.navigation.stop();
    state.status = 4;
    return 1;
}

} // namespace
} // namespace medicon::platform

extern "C" int MediconAutoUnloadReady() {
    auto& state = medicon::platform::runtime();
    if (!state.eject_requested.load(std::memory_order_acquire) || state.status.load(std::memory_order_acquire) != 0)
        return 0;
    HANDLE worker = state.eject_worker.load(std::memory_order_acquire);
    if (!worker || WaitForSingleObject(worker, 0) != WAIT_OBJECT_0) return 0;
    worker = state.eject_worker.exchange(nullptr, std::memory_order_acq_rel);
    if (worker) CloseHandle(worker);
    if (!state.window_callbacks_restored) {
        state.eject_requested.store(false, std::memory_order_release);
        try {
            state.report("Overlay stopped, but it remains mapped because another window procedure may still call it.", true);
        } catch (...) {
            OutputDebugStringA("[Medicon] DLL remains mapped: another window procedure may call it.\n");
        }
        return -1;
    }
    const std::array<medicon::HookManager*, 9> managers{
        &state.model_static_hooks, &state.model_box_hooks, &state.model_hooks, &state.navigation_hooks,
        &state.rotation_hooks, &state.mouse_hooks, &state.tick_hooks, &state.chat_hooks, &state.hooks};
    try {
        for (auto* manager : managers) {
            std::string error;
            if (!manager->shutdown(error)) {
                state.eject_requested.store(false, std::memory_order_release);
                try { state.report("Overlay stopped, but hook cleanup failed; DLL remains mapped: " + error, true); }
                catch (...) { OutputDebugStringA("[Medicon] Hook cleanup failed; DLL remains mapped.\n"); }
                return -1;
            }
        }
    } catch (...) {
        state.eject_requested.store(false, std::memory_order_release);
        try { state.report("Overlay stopped, but hook cleanup threw; DLL remains mapped.", true); }
        catch (...) { OutputDebugStringA("[Medicon] Hook cleanup threw; DLL remains mapped.\n"); }
        return -1;
    }
    {
        std::lock_guard lock(state.log_mutex);
        state.log.close();
    }
    return 1;
}

extern "C" void MediconEnableAutoUnload() noexcept {
    medicon::platform::auto_unload_enabled.store(true, std::memory_order_release);
}

int MediconStartOverlay() {
    using namespace medicon::platform;
    auto& state = runtime();
    std::lock_guard lock(state.lifecycle_mutex);
    if (auto_unload_enabled.load(std::memory_order_acquire) &&
            state.eject_requested.load(std::memory_order_acquire)) return 0;
    if (state.status >= 1 && state.status <= 3) return 1;
    if (HANDLE eject = state.eject_worker.load(std::memory_order_acquire)) {
        if (WaitForSingleObject(eject, 0) != WAIT_OBJECT_0) return 0;
        if (state.eject_worker.compare_exchange_strong(eject, nullptr, std::memory_order_acq_rel))
            CloseHandle(eject);
    }
    if (state.worker) {
        if (WaitForSingleObject(state.worker, 0) != WAIT_OBJECT_0) return 0;
        CloseHandle(state.worker); state.worker = nullptr;
    }
    state.stopping = false;
    state.eject_requested = false;
    state.status = 1;
    state.worker = CreateThread(nullptr, 0, start_worker, nullptr, 0, nullptr);
    if (!state.worker) { state.status = 4; state.stopping = true; state.report("Cannot start overlay worker.", true); return 0; }
    return 1;
}

// The C ABI MediconLoader uses (medicon/update/HostApi.h). When the loader
// hosts the Core, it calls Medicon_Initialize instead of relying on the DllMain
// auto-start (which is suppressed by MEDICON_LOADER); both reuse the same worker.
extern "C" MEDICON_API uint32_t Medicon_GetAPIVersion(void) { return 1; }

namespace {
// Version strings are ASCII ("1.0.5"); anything else is dropped.
std::string ascii(const wchar_t* text) {
    std::string out;
    for (const wchar_t* c = text; c && *c; ++c) {
        if (*c >= 128) return {};
        out.push_back(static_cast<char>(*c));
    }
    return out;
}

// The latest version the loader found on the update server this launch, or
// empty when it was offline or the manifest was rejected. No network here:
// the loader already fetched and verified the manifest before starting us.
std::string loader_latest_version() {
    HMODULE loader = GetModuleHandleW(L"MediconLoader.dll");
    if (!loader) return {};
    const auto get_status = reinterpret_cast<MediconLoaderGetStatusFn>(
        reinterpret_cast<void*>(GetProcAddress(loader, "MediconLoaderGetStatus")));
    if (!get_status) return {};
    MediconLoaderStatus status{};
    status.size = sizeof status;
    if (!get_status(&status)) return {};
    status.latest_version[std::size(status.latest_version) - 1] = 0;
    return ascii(status.latest_version);
}
} // namespace

extern "C" MEDICON_API int Medicon_Initialize(const MediconHostAPI* host) {
    using namespace medicon::platform;
    auto& state = runtime();
    if (host && host->size >= sizeof(MediconHostAPI) && host->api_version == 1 && host->core_version) {
        std::lock_guard lock(state.lifecycle_mutex);
        state.core_version = ascii(host->core_version);
        state.server_version = loader_latest_version();
        if (state.server_version.empty()) state.server_version = ascii(host->core_version);
        // state 1 == MEDICON_UPDATE_UPDATED: a newer Core was downloaded this launch.
        if (host->update_state == 1) {
            const auto* version = host->core_version;
            std::string utf8;
            for (const wchar_t* c = version; *c && *c < 128; ++c) utf8.push_back(static_cast<char>(*c));
            state.update_version = utf8;
        }
    }
    return MediconStartOverlay();
}

extern "C" MEDICON_API void Medicon_Shutdown(void) { MediconStopOverlay(); }

uint32_t MediconOverlayStatus() { return medicon::platform::runtime().status.load(); }
uint32_t MediconOverlayLastError(char* buffer, uint32_t capacity) {
    auto& state = medicon::platform::runtime();
    std::lock_guard lock(state.log_mutex);
    if (buffer && capacity) {
        const auto length = std::min<std::size_t>(state.last_error.size(), capacity - 1);
        std::memcpy(buffer, state.last_error.data(), length); buffer[length] = 0;
    }
    return static_cast<uint32_t>(state.last_error.size() + 1);
}

int MediconStopOverlay() {
    using namespace medicon::platform;
    auto& state = runtime();
    std::lock_guard lifecycle(state.lifecycle_mutex);
    state.stopping = true;
    state.rotation_override_enabled.store(false, std::memory_order_release);
    state.model_probe.set_enabled(false);
    state.model_box.set_options(false,0,1);
    state.navigation_ready.store(false, std::memory_order_release);
    state.navigation.stop();
    state.publish_menu_visible(false);
    state.gui_open = false;
    state.menu_action_pending = 0;
    if (state.worker) {
        if (WaitForSingleObject(state.worker, 15000) != WAIT_OBJECT_0) { state.report("Overlay bootstrap has not finished.", true); return 0; }
        CloseHandle(state.worker); state.worker = nullptr;
    }
    if (const auto target = state.mouse_window.load(); target && IsWindow(target)) {
        DWORD_PTR restored{};
        if (!SendMessageTimeoutW(target, state.mouse_message, 1, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &restored) || !restored) {
            state.report("Waiting for the game window to restore native mouse capture; stop can be retried.", true);
            return 0;
        }
    }
    std::string error;
    if (!state.model_static_hooks.disable_all(error)) { state.report(error, true); return 0; }
    if (!state.model_box_hooks.disable_all(error)) { state.report(error, true); return 0; }
    if (!state.model_hooks.disable_all(error)) { state.report(error, true); return 0; }
    if (!state.navigation_hooks.disable_all(error)) { state.report(error, true); return 0; }
    if (!state.mouse_hooks.disable_all(error)) { state.report(error, true); return 0; }
    state.mouse_ready = false;
    if (!state.native_input_hooks.disable_all(error)) { state.report(error, true); return 0; }
    state.native_input_ready.store(false, std::memory_order_release);
    if (!state.rotation_hooks.disable_all(error)) { state.report(error, true); return 0; }
    if (!state.tick_hooks.disable_all(error)) { state.report(error, true); return 0; }
    if (!state.chat_hooks.disable_all(error)) { state.report(error, true); return 0; }
    if (!state.hooks.disable_all(error)) { state.report(error, true); return 0; }
    {
        std::lock_guard lock(state.window_mutex);
        state.window_callbacks_restored = true;
        for (auto& entry : state.windows) {
            // Respect another plugin installed after us: its chain can continue
            // forwarding through our stopped procedure, so do not unload in that case.
            if (!IsWindow(entry.window)) continue;
            if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(entry.window, GWLP_WNDPROC)) == overlay_window_proc) {
                entry.restored = SetWindowLongPtrW(entry.window, GWLP_WNDPROC,
                    reinterpret_cast<LONG_PTR>(entry.original)) != 0;
            }
            if (!entry.restored) state.window_callbacks_restored = false;
        }
    }
    state.mouse_window = nullptr;
    state.mouse_message_pending = false;
    state.cursor_message_pending = false;
    const auto deadline = GetTickCount64() + 5000;
    while (state.callbacks && GetTickCount64() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (state.callbacks) { state.report("Overlay callbacks are still in flight; stop can be retried.", true); return 0; }
    {
        std::lock_guard lock(state.mouse_mutex);
        state.mouse_capture.reset(false, {});
        state.mouse_identity.store(0, std::memory_order_release);
        state.spoof_valid.store(false, std::memory_order_release);
        state.cursor_owned.store(false, std::memory_order_release);
        state.owned_mouse_buttons.store(0, std::memory_order_release);
        state.modal_open.store(false, std::memory_order_release);
        state.movement_enabled.store(false, std::memory_order_release);
        state.social_available.store(false, std::memory_order_release);
        state.opening_input.store(0, std::memory_order_release);
        state.window_focused_cached.store(false, std::memory_order_release);
        state.mouse_owner = nullptr;
        state.mouse_active = false;
        state.win32_mouse_open = state.clip_saved = false;
        if (state.show_cursor) {
            while (state.os_cursor_show_boost > 0) {
                state.show_cursor(FALSE);
                --state.os_cursor_show_boost;
            }
        } else {
            state.os_cursor_show_boost = 0;
        }
        state.relative_menu_pointer.reset();
        state.hw_cursor_valid = false;
    }
    std::unique_ptr<medicon::social::SocialService> social;
    {
        std::lock_guard lock(state.render_mutex);
        FlagScope submission(inside_overlay);
        state.teardown();
        // The overlay that read its channel is gone with the client.
        social = std::move(state.social);
    }
    // Closes the WebSocket and joins its threads within 2 s, outside every lock.
    if (social && !social->stop()) state.report("Social network threads were detached while stopping.");
    social.reset();
    { std::lock_guard lock(state.queue_mutex); state.queues.clear(); }
    { std::lock_guard lock(state.command_mutex); state.pending_commands.clear(); }
    { std::lock_guard lock(state.feedback_mutex); state.pending_feedback.clear(); state.feedback_count = 0; }
    state.status = 0;
    state.report("Overlay stopped; hooks and window input restored.");
    return 1;
}
