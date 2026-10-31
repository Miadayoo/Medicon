#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>

#include "DemoModules.hpp"
#include "ImageIO.hpp"
#include "Scenarios.hpp"
#include "ReferenceBackground.hpp"

#include <medicon/core/ConfigStore.hpp>
#include <medicon/core/ModuleManager.hpp>
#include <medicon/gui/Log.hpp>
#include <medicon/gui/EdgeLit.hpp>
#include <medicon/gui/InjectSplash.hpp>
#include <medicon/gui/Overlay.hpp>
#include <medicon/gui/Textures.hpp>
#include <medicon/gui/Theme.hpp>
#include <medicon/modules/BuiltinModules.hpp>
#include <medicon/modules/ClientModules.hpp>
#include <medicon/social/Service.hpp>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include <algorithm>
#include <functional>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

using medicon::preview::Image;

// Small ownership wrapper keeps this host buildable with both MSVC and MinGW.
template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    [[nodiscard]] T* Get() const noexcept { return pointer_; }
    [[nodiscard]] T* operator->() const noexcept { return pointer_; }
    [[nodiscard]] T** GetAddressOf() noexcept { return &pointer_; }
    [[nodiscard]] T** ReleaseAndGetAddressOf() noexcept {
        Reset();
        return &pointer_;
    }
    void Reset() noexcept {
        if (pointer_) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

private:
    T* pointer_ = nullptr;
};

std::string hresult_message(const char* operation, HRESULT result) {
    std::array<char, 128> message{};
    std::snprintf(message.data(), message.size(), "%s failed (HRESULT 0x%08lX).", operation, static_cast<unsigned long>(result));
    return message.data();
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::filesystem::path config_path() {
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (length > 0) {
        std::wstring directory(length, L'\0');
        const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", directory.data(), length);
        if (written > 0 && written < length) {
            directory.resize(written);
            return std::filesystem::path(directory) / L"Medicon" / L"config.json";
        }
    }
    return std::filesystem::current_path() / "config" / "medicon.json";
}

int virtual_key(WPARAM key, LPARAM parameters) {
    if (key == VK_SHIFT) {
        const auto scan_code = static_cast<UINT>((parameters >> 16) & 0xFF);
        return static_cast<int>(MapVirtualKeyW(scan_code, MAPVK_VSC_TO_VK_EX));
    }
    if (key == VK_CONTROL) return (parameters & (1LL << 24)) ? VK_RCONTROL : VK_LCONTROL;
    if (key == VK_MENU) return (parameters & (1LL << 24)) ? VK_RMENU : VK_LMENU;
    return static_cast<int>(key);
}

const char* format_name(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    default: return "other";
    }
}

struct Options {
    bool smoke = false;
    bool input_test = false;
    bool demo = false;
    int width = 0;
    int height = 0;
    float dpi = 0.0F;
    std::string backend = "d3d11";
    std::string scenario;
    std::string background = "default";
    std::filesystem::path background_image;
    std::filesystem::path screenshot;
    std::filesystem::path log_file;
    std::optional<double> splash_time;
    std::optional<double> main_menu_time;
    bool main_menu = false;
    float fixed_dt = 0.0F;
    int fps = 0;
    bool gpu_timing = false;
    int gpu_frames = 600;
    int device_lost_frame = -1;
    bool debug_layer = false;
    bool no_shadow = false;
    float arraylist_scale = 1.0F;
    // Runs the real Social service (social.json/license.bin in social_dir, or %APPDATA%\Medicon).
    bool social = false;
    std::filesystem::path social_dir;

    bool hidden() const { return smoke || input_test || !scenario.empty() || gpu_timing || splash_time.has_value() || main_menu_time.has_value(); }
    bool offscreen() const { return !smoke && hidden(); }
};

Options parse_options() {
    Options options;
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv) return options;
    std::vector<std::string> args;
    for (int i = 1; i < count; ++i) args.push_back(narrow(argv[i]));
    std::vector<std::wstring> wide;
    for (int i = 1; i < count; ++i) wide.emplace_back(argv[i]);
    LocalFree(argv);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= args.size()) throw std::runtime_error("Missing value for " + a);
            return args[++i];
        };
        const auto next_wide = [&]() -> std::wstring {
            if (i + 1 >= wide.size()) throw std::runtime_error("Missing value for " + a);
            ++i;
            return wide[i];
        };
        if (a == "--smoke-test") options.smoke = true;
        else if (a == "--input-test") options.input_test = true;
        else if (a == "--demo") options.demo = true;
        else if (a == "--size") {
            const std::string value = next();
            if (std::sscanf(value.c_str(), "%dx%d", &options.width, &options.height) != 2 || options.width < 64 || options.height < 64) {
                throw std::runtime_error("--size expects WxH");
            }
        } else if (a == "--dpi") options.dpi = std::stof(next());
        else if (a == "--backend") options.backend = next();
        else if (a == "--scenario") options.scenario = next();
        else if (a == "--main-menu") options.main_menu = true;
        else if (a == "--main-menu-time") {
            const auto value = next();
            std::size_t consumed = 0;
            const double seconds = std::stod(value, &consumed);
            if (consumed != value.size() || !std::isfinite(seconds) || seconds < 0.0)
                throw std::runtime_error("--main-menu-time expects finite, non-negative seconds");
            options.main_menu_time = seconds;
        }
        else if (a == "--splash-time") {
            const std::string value = next();
            std::size_t consumed = 0;
            const double seconds = std::stod(value, &consumed);
            if (consumed != value.size() || !std::isfinite(seconds) || seconds < 0.0)
                throw std::runtime_error("--splash-time expects finite, non-negative seconds");
            options.splash_time = seconds;
        }
        else if (a == "--bg") {
            const std::wstring value = next_wide();
            options.background = narrow(value);
            if (options.background != "white" && options.background != "gray" && options.background != "black" &&
                options.background != "default" && options.background != "day" && options.background != "night") {
                options.background_image = value;
                options.background = "image";
            }
        } else if (a == "--screenshot") options.screenshot = next_wide();
        else if (a == "--log") options.log_file = next_wide();
        else if (a == "--fixed-dt") options.fixed_dt = std::stof(next());
        else if (a == "--fps") options.fps = std::stoi(next());
        else if (a == "--gpu-timing") options.gpu_timing = true;
        else if (a == "--gpu-frames") options.gpu_frames = std::stoi(next());
        else if (a == "--simulate-device-lost") options.device_lost_frame = std::stoi(next());
        else if (a == "--debug-layer") options.debug_layer = true;
        else if (a == "--no-shadow") options.no_shadow = true;
        else if (a == "--social") options.social = true;
        else if (a == "--social-dir") {
            options.social = true;
            options.social_dir = next_wide();
        }
        else if (a == "--arraylist-scale") {
            options.arraylist_scale = std::stof(next());
            if (!std::isfinite(options.arraylist_scale) || options.arraylist_scale < 0.5F || options.arraylist_scale > 2.0F)
                throw std::runtime_error("--arraylist-scale expects 0.5..2");
        }
        else throw std::runtime_error("Unknown argument: " + a);
    }
    if (options.backend != "d3d11") {
        throw std::runtime_error("--backend " + options.backend + " is not available: the Preview only implements d3d11.");
    }
    // --main-menu-time with --splash-time renders the in-game handoff: the menu
    // underneath the intro. Interactive --main-menu stays exclusive.
    if ((options.main_menu || options.main_menu_time) && (options.smoke || options.input_test || !options.scenario.empty()))
        throw std::runtime_error("Main Menu preview cannot be combined with smoke, input, or scenario modes");
    if (options.main_menu && options.splash_time)
        throw std::runtime_error("--main-menu cannot be combined with --splash-time; use --main-menu-time");
    if (options.splash_time && (options.smoke || options.input_test || !options.scenario.empty()))
        throw std::runtime_error("--splash-time cannot be combined with smoke, input, or scenario modes");
    if (options.gpu_frames < 1) throw std::runtime_error("--gpu-frames expects a positive frame count");
    if (options.offscreen() && options.width == 0) {
        options.width = 1920;
        options.height = 1080;
    }
    if ((options.offscreen() || options.input_test) && options.fixed_dt <= 0.0F) options.fixed_dt = 1.0F / 60.0F;
    return options;
}

// D3D11 timestamp queries around the whole UI pass and around every shadow draw
// (via ImDrawList callbacks). The opt-in benchmark waits only when its query
// ring fills; normal rendering never creates or waits on these queries.
class GpuTimer final : public medicon::gui::ShadowProfiler {
public:
    static constexpr int ring = 16;
    static constexpr int max_marks = 64;

    void create(ID3D11Device* device, ID3D11DeviceContext* context) {
        release();
        context_ = context;
        D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        D3D11_QUERY_DESC stamp{D3D11_QUERY_TIMESTAMP, 0};
        for (auto& slot : slots_) {
            device->CreateQuery(&disjoint, slot.disjoint.ReleaseAndGetAddressOf());
            device->CreateQuery(&stamp, slot.begin.ReleaseAndGetAddressOf());
            device->CreateQuery(&stamp, slot.end.ReleaseAndGetAddressOf());
            for (auto& mark : slot.marks) device->CreateQuery(&stamp, mark.ReleaseAndGetAddressOf());
            slot.pending = false;
        }
        active_ = this;
    }
    void release() {
        for (auto& slot : slots_) {
            slot.disjoint.Reset();
            slot.begin.Reset();
            slot.end.Reset();
            for (auto& mark : slot.marks) mark.Reset();
            slot.pending = false;
        }
        context_ = nullptr;
    }
    void begin(ImDrawList* list) override { mark(list); }
    void end(ImDrawList* list) override { mark(list); }

    // Call after ImGui::Render() and before RenderDrawData.
    void begin_frame() {
        auto& slot = slots_[static_cast<std::size_t>(current_)];
        // This is an opt-in benchmark only. Never overwrite a pending query:
        // offscreen CPU submission can lap the GPU and lose most samples.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!collect(slot)) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("GPU timing query timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        context_->Begin(slot.disjoint.Get());
        context_->End(slot.begin.Get());
    }
    void end_frame() {
        auto& slot = slots_[static_cast<std::size_t>(current_)];
        context_->End(slot.end.Get());
        context_->End(slot.disjoint.Get());
        slot.pending = true;
        slot.used = next_mark_;
        next_mark_ = 0;
        current_ = (current_ + 1) % ring;
    }
    void flush() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (int i = 0; i < ring; ++i) {
            auto& slot = slots_[static_cast<std::size_t>(i)];
            while (slot.pending) {
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("GPU timing flush timed out");
                if (!collect(slot)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    std::vector<double> total_ms;
    std::vector<double> shadow_ms;
    bool recording = false;

private:
    struct Slot {
        ComPtr<ID3D11Query> disjoint, begin, end;
        std::array<ComPtr<ID3D11Query>, max_marks> marks;
        int used = 0;
        bool pending = false;
    };
    static void callback(const ImDrawList*, const ImDrawCmd* command) {
        if (!active_ || !active_->context_) return;
        const auto index = static_cast<std::size_t>(reinterpret_cast<std::intptr_t>(command->UserCallbackData));
        auto& slot = active_->slots_[static_cast<std::size_t>(active_->current_)];
        if (index < slot.marks.size()) active_->context_->End(slot.marks[index].Get());
    }
    void mark(ImDrawList* list) {
        if (next_mark_ >= max_marks) return;
        list->AddCallback(&GpuTimer::callback, reinterpret_cast<void*>(static_cast<std::intptr_t>(next_mark_++)));
    }
    bool collect(Slot& slot) {
        if (!slot.pending) return true;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        // Flag 0 lets the runtime submit pending work (offscreen runs never call
        // Present); this polls and never blocks the CPU. Only used with --gpu-timing.
        if (context_->GetData(slot.disjoint.Get(), &disjoint, sizeof disjoint, 0) != S_OK) return false;
        slot.pending = false;
        if (disjoint.Disjoint || !recording) return true;
        UINT64 begin = 0, end = 0;
        context_->GetData(slot.begin.Get(), &begin, sizeof begin, 0);
        context_->GetData(slot.end.Get(), &end, sizeof end, 0);
        double shadow = 0.0;
        for (int i = 0; i + 1 < slot.used; i += 2) {
            UINT64 a = 0, b = 0;
            context_->GetData(slot.marks[static_cast<std::size_t>(i)].Get(), &a, sizeof a, 0);
            context_->GetData(slot.marks[static_cast<std::size_t>(i + 1)].Get(), &b, sizeof b, 0);
            if (b > a) shadow += static_cast<double>(b - a);
        }
        const double to_ms = 1000.0 / static_cast<double>(disjoint.Frequency);
        total_ms.push_back(static_cast<double>(end - begin) * to_ms);
        shadow_ms.push_back(shadow * to_ms);
        return true;
    }

    inline static GpuTimer* active_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    std::array<Slot, ring> slots_{};
    int current_ = 0;
    int next_mark_ = 0;
};

double mean(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    double sum = 0.0;
    for (const double value : values) sum += value;
    return sum / static_cast<double>(values.size());
}

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::clamp(std::ceil(p * static_cast<double>(values.size())) - 1.0, 0.0,
                                                           static_cast<double>(values.size() - 1)));
    return values[index];
}

class PreviewApplication final : public medicon::preview::ScenarioHost {
public:
    explicit PreviewApplication(Options options)
        : options_(std::move(options)), config_(config_path()), overlay_(modules_) {
        medicon::register_builtin_modules(modules_);
        if (options_.demo || options_.input_test || !options_.scenario.empty() || options_.gpu_timing) {
            medicon::preview::register_demo_modules(modules_);
        }
        if (options_.hidden()) {
            // Deterministic captures never depend on desktop media sessions.
            overlay_.social().music().set_preview(std::make_shared<medicon::music::MusicSnapshot>());
        }
        overlay_.set_owns_font_atlas(true);
        overlay_.set_save_handler([this] { if (uses_config()) save_config(); });
        if (options_.dpi > 0.0F) overlay_.set_dpi_override(options_.dpi);
        if (options_.fixed_dt > 0.0F) overlay_.clock().set_fixed_step(options_.fixed_dt);
        if (!options_.log_file.empty()) log_stream_.open(options_.log_file, std::ios::out | std::ios::trunc);
        medicon::gui::set_log_sink([this](std::string_view line) { log(std::string(line)); });
    }

    ~PreviewApplication() override {
        if (session_started_ && uses_config() && config_writable_) {
            save_config();
            if (!config_error_.empty()) MessageBoxA(window_, config_error_.c_str(), "Medicon: configuration was not saved", MB_OK | MB_ICONWARNING);
        }
        if (!options_.hidden() && log_stream_) report_shadow_stats();
        medicon::gui::set_log_sink({});
        modules_.shutdown();
        if (dx11_initialized_) ImGui_ImplDX11_Shutdown();
        if (win32_initialized_) ImGui_ImplWin32_Shutdown();
        if (imgui_initialized_) {
            medicon::gui::forget_edge_lit_frame(ImGui::GetCurrentContext());
            ImGui::DestroyContext();
        }
        release_device(true);
        if (window_) DestroyWindow(window_);
        if (window_class_) UnregisterClassW(class_name_, instance_);
    }

    int run(HINSTANCE instance, int show_command) {
        instance_ = instance;
        ImGui_ImplWin32_EnableDpiAwareness();
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_CLASSDC | CS_DBLCLKS;
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance_;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = class_name_;
        window_class_ = RegisterClassExW(&window_class);
        if (!window_class_) throw std::runtime_error("Could not register the preview window class.");
        RECT rect{0, 0, options_.width > 0 ? options_.width : 1240, options_.height > 0 ? options_.height : 820};
        const bool explicit_size = options_.width > 0;
        if (explicit_size) AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
        window_ = CreateWindowExW(0, class_name_, L"Medicon Preview", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance_, this);
        if (!window_) throw std::runtime_error("Could not create the preview window.");
        ime_.set_window(window_);
        overlay_.set_ime(&ime_);
        create_device();

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        imgui_initialized_ = true;
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; // keep layout persistence separate from the user's module config
        medicon::ClickGui::apply_theme();
        if (!ImGui_ImplWin32_Init(window_)) throw std::runtime_error("Could not initialize ImGui's Win32 backend.");
        win32_initialized_ = true;
        if (!ImGui_ImplDX11_Init(device_.Get(), context_.Get())) throw std::runtime_error("Could not initialize ImGui's Direct3D 11 backend.");
        dx11_initialized_ = true;
        if (options_.gpu_timing) {
            timer_.create(device_.Get(), context_.Get());
            overlay_.shadows().set_profiler(&timer_);
        }
        if (options_.no_shadow) {
            if (auto* cfg = dynamic_cast<medicon::ClickGuiModule*>(modules_.find(medicon::ClickGuiModule::module_id))) {
                cfg->panel_shadow.get<bool>() = false;
            }
        }
        load_background();
        if (uses_config()) load_config();
        if (options_.arraylist_scale != 1.0F) {
            if (auto* cfg = dynamic_cast<medicon::ArrayListModule*>(modules_.find(medicon::ArrayListModule::module_id)))
                cfg->scale.get<float>() = options_.arraylist_scale;
        }
        overlay_.gui().set_open(true);
        // A fixed-time splash still needs its dedicated glyphs prepared before
        // draw_inject_splash(), just like the injected runtime's first frame.
        if (options_.splash_time) overlay_.start_splash();
        overlay_.set_main_menu_active(options_.main_menu || options_.main_menu_time.has_value());
        if (options_.social) start_social();
        session_started_ = true;

        ShowWindow(window_, options_.hidden() ? SW_HIDE : show_command);
        if (!options_.hidden()) UpdateWindow(window_);

        if (options_.splash_time || options_.main_menu_time) {
            frame();
            if (!options_.screenshot.empty()) capture({});
            if (options_.gpu_timing) run_gpu_timing();
            return 0;
        }
        if (options_.input_test) {
            const int failures = medicon::preview::run_input_tests(*this);
            return failures == 0 ? 0 : 3;
        }
        if (!options_.scenario.empty()) {
            const bool capture = !options_.screenshot.empty();
            if (!medicon::preview::run_scenario(options_.scenario, *this, capture)) {
                std::string names;
                for (const auto& name : medicon::preview::scenario_names()) names += " " + name;
                throw std::runtime_error("Unknown scenario '" + options_.scenario + "'. Available:" + names);
            }
            if (options_.gpu_timing) run_gpu_timing();
            report_shadow_stats();
            return 0;
        }
        if (options_.gpu_timing) {
            run_gpu_timing();
            return 0;
        }

        auto previous = std::chrono::steady_clock::now();
        while (!done_) {
            if (minimized_) {
                WaitMessage();
                pump();
                continue;
            }
            frame();
            if (options_.smoke && frame_index_ >= 5) break;
            if (options_.fps > 0) {
                const auto period = std::chrono::duration<double>(1.0 / options_.fps);
                const auto next = previous + std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
                std::this_thread::sleep_until(next);
                previous = std::max(next, std::chrono::steady_clock::now() - std::chrono::milliseconds(100));
            }
        }
        return 0;
    }

    // --- ScenarioHost --------------------------------------------------------
    medicon::ModuleManager& modules() override { return modules_; }
    medicon::gui::Overlay& overlay() override { return overlay_; }
    void recreate_graphics() override { recreate_device(); }
    float dt() const override { return options_.fixed_dt > 0.0F ? options_.fixed_dt : 1.0F / 60.0F; }
    float width() const override { return static_cast<float>(target_width_); }
    float height() const override { return static_cast<float>(target_height_); }
    LRESULT send(UINT message, WPARAM wparam, LPARAM lparam) override { return SendMessageW(window_, message, wparam, lparam); }
    void set_frame_hook(std::function<void()> hook) override { frame_hook_ = std::move(hook); }
    void log(const std::string& line) override {
        // Social reports from its network threads.
        std::lock_guard lock(log_mutex_);
        std::fprintf(stdout, "%s\n", line.c_str());
        std::fflush(stdout);
        if (log_stream_) log_stream_ << line << '\n' << std::flush;
    }

    void start_social() {
        medicon::social::SocialService::Options social_options;
        social_options.directory = options_.social_dir.empty() ? medicon::social::SocialService::default_directory()
                                                               : options_.social_dir;
        social_options.client_version = MEDICON_VERSION;
        social_options.log = [this](std::string_view line) { log(std::string(line)); };
        social_ = std::make_unique<medicon::social::SocialService>(std::move(social_options));
        overlay_.set_social(&social_->channel(), &medicon::social::play_notification_sound);
        overlay_.set_alts(&social_->alt_channel());
        social_->start();
    }

    void frame() override {
        pump();
        if (done_) return;
        if (frame_hook_) frame_hook_();
        if (frame_index_ == options_.device_lost_frame) {
            log("simulating device loss at frame " + std::to_string(frame_index_));
            recreate_device();
        }
        if (!options_.offscreen()) resize_if_needed();
        const auto now = std::chrono::steady_clock::now();
        const float measured = std::chrono::duration<float>(now - previous_frame_).count();
        previous_frame_ = now;
        const float delta = options_.fixed_dt > 0.0F ? options_.fixed_dt : std::min(measured, 0.1F);

        const auto cpu_now = [&] { return options_.gpu_timing ? std::chrono::steady_clock::now() :
            std::chrono::steady_clock::time_point{}; };
        const auto cpu_begin = cpu_now();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        auto& io = ImGui::GetIO();
        if (options_.offscreen()) io.DisplaySize = ImVec2(static_cast<float>(target_width_), static_cast<float>(target_height_));
        if (options_.fixed_dt > 0.0F) io.DeltaTime = options_.fixed_dt;
        if (overlay_.prepare_frame()) {
            // Font atlas rebuilt (scale/DPI/glyph change): re-upload the texture.
            ImGui_ImplDX11_InvalidateDeviceObjects();
            edge_lit_.invalidate_fonts();
            ImGui_ImplDX11_CreateDeviceObjects();
            ++font_uploads_;
        }
        const auto cpu_prepared = cpu_now();
        ImGui::NewFrame();
        ImDrawList backdrop(ImGui::GetDrawListSharedData());
        backdrop._ResetForNewFrame();
        backdrop.PushClipRect(ImVec2(0, 0), io.DisplaySize);
        if (background_srv_.Get()) {
            backdrop.AddImage(reinterpret_cast<ImTextureID>(background_srv_.Get()), ImVec2(0, 0), io.DisplaySize);
        }
        if (options_.main_menu_time || options_.splash_time) {
            if (options_.main_menu_time)
                overlay_.main_menu().render_at(ImGui::GetForegroundDrawList(), overlay_.fonts(), io.DisplaySize,
                                              medicon::gui::read_pointer_input(), *options_.main_menu_time);
            if (options_.splash_time)
                medicon::gui::draw_inject_splash(ImGui::GetForegroundDrawList(), overlay_.fonts(), io.DisplaySize,
                                                *options_.splash_time);
        } else {
            modules_.tick(delta);
            modules_.render();
            overlay_.render();
            if (const auto action = overlay_.main_menu().consume_action()) {
                const char* label = *action == medicon::gui::MainMenuAction::Play ? "Play" :
                    (*action == medicon::gui::MainMenuAction::Settings ? "Settings" : "Quit");
                log(std::string("MainMenu ") + label + " pressed (preview; navigation unavailable).");
            }
        }
        if (!options_.hidden() && !overlay_.gui().open() && !overlay_.main_menu().active()) {
            ImGui::SetNextWindowPos({16.0F, io.DisplaySize.y - 48.0F}, ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.75F);
            if (ImGui::Begin("##show-gui-hint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav)) {
                ImGui::TextUnformatted("Medicon Preview  |  Press Insert to open the client.");
            }
            ImGui::End();
        }
        ImGui::Render();
        const auto cpu_built = cpu_now();

        ID3D11RenderTargetView* target = options_.offscreen() ? offscreen_rtv_.Get() : render_target_.Get();
        context_->OMSetRenderTargets(1, &target, nullptr);
        const medicon::Color clear = background_color();
        const float clear_color[]{clear.r, clear.g, clear.b, clear.a};
        context_->ClearRenderTargetView(target, clear_color);
        if (options_.gpu_timing) timer_.begin_frame();
        const auto cpu_submit_begin = cpu_now();
        // Paint the world reference before Edge-lit captures the background.
        // Keeping this separate prevents the final ImGui pass covering the HUD.
        if (background_srv_.Get()) {
            ImDrawData background;
            background.Valid = true;
            background.DisplaySize = io.DisplaySize;
            background.FramebufferScale = io.DisplayFramebufferScale;
            background.AddDrawList(&backdrop);
            ImGui_ImplDX11_RenderDrawData(&background);
        }
        ComPtr<ID3D11Texture2D> edge_back_buffer;
        ID3D11Texture2D* edge_source = offscreen_.Get();
        if (!options_.offscreen()) {
            const HRESULT acquired = swap_chain_->GetBuffer(0, IID_PPV_ARGS(edge_back_buffer.GetAddressOf()));
            if (FAILED(acquired)) throw std::runtime_error(hresult_message("Acquire Edge-lit back buffer", acquired));
            edge_source = edge_back_buffer.Get();
        }
        std::string edge_error;
        if (!edge_lit_.render(device_.Get(), context_.Get(), edge_source, target, edge_error)) throw std::runtime_error(edge_error);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (options_.gpu_timing && timer_.recording) {
            const auto elapsed_ms = [](auto start, auto end) {
                return std::chrono::duration<double, std::milli>(end - start).count();
            };
            cpu_prepare_ms_.push_back(elapsed_ms(cpu_begin, cpu_prepared));
            cpu_build_ms_.push_back(elapsed_ms(cpu_prepared, cpu_built));
            cpu_submit_ms_.push_back(elapsed_ms(cpu_submit_begin, std::chrono::steady_clock::now()));
        }
        if (options_.gpu_timing) timer_.end_frame();
        ++frame_index_;
        if (!options_.offscreen()) {
            const HRESULT result = swap_chain_->Present(options_.fps > 0 ? 0 : 1, 0);
            if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
                log(hresult_message("Present (device lost, recreating)", result));
                recreate_device();
            } else if (result == DXGI_STATUS_OCCLUDED) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            } else if (FAILED(result)) {
                throw std::runtime_error(hresult_message("Present", result));
            }
        }
    }

    void capture(const std::string& suffix) override {
        ID3D11Texture2D* source = options_.offscreen() ? offscreen_.Get() : nullptr;
        ComPtr<ID3D11Texture2D> back_buffer;
        if (!source) {
            swap_chain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.GetAddressOf()));
            source = back_buffer.Get();
        }
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        HRESULT result = device_->CreateTexture2D(&desc, nullptr, staging.GetAddressOf());
        if (FAILED(result)) throw std::runtime_error(hresult_message("Create staging texture", result));
        context_->CopyResource(staging.Get(), source);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        result = context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(result)) throw std::runtime_error(hresult_message("Map staging texture", result));
        Image image;
        image.width = static_cast<int>(desc.Width);
        image.height = static_cast<int>(desc.Height);
        image.rgba.resize(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
        for (UINT y = 0; y < desc.Height; ++y) {
            const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
            auto* out = image.rgba.data() + static_cast<std::size_t>(y) * desc.Width * 4;
            std::copy(row, row + desc.Width * 4, out);
            for (UINT x = 0; x < desc.Width; ++x) out[x * 4 + 3] = 255;
        }
        context_->Unmap(staging.Get(), 0);
        std::filesystem::path path = options_.screenshot;
        if (!suffix.empty()) path.replace_filename(path.stem().wstring() + L"_" + std::wstring(suffix.begin(), suffix.end()) + path.extension().wstring());
        medicon::preview::save_png(path, image);
        log("screenshot " + narrow(path.wstring()));
    }

private:
    bool uses_config() const { return !options_.hidden(); }

    void pump() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) done_ = true;
        }
    }

    medicon::Color background_color() const {
        namespace theme = medicon::theme;
        if (options_.background == "white") return theme::preview_white;
        if (options_.background == "gray") return theme::preview_gray;
        if (options_.background == "black") return theme::preview_black;
        return theme::preview_default;
    }

    void load_background() {
        if (options_.background_image.empty() && options_.background != "day" && options_.background != "night") return;
        const Image image = options_.background == "day" || options_.background == "night"
            ? medicon::preview::reference_background(target_width_, target_height_, options_.background == "night")
            : medicon::preview::load_image(options_.background_image);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(image.width);
        desc.Height = static_cast<UINT>(image.height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{image.rgba.data(), static_cast<UINT>(image.width * 4), 0};
        ComPtr<ID3D11Texture2D> texture;
        HRESULT result = device_->CreateTexture2D(&desc, &data, texture.GetAddressOf());
        if (FAILED(result)) throw std::runtime_error(hresult_message("Create background texture", result));
        result = device_->CreateShaderResourceView(texture.Get(), nullptr, background_srv_.ReleaseAndGetAddressOf());
        if (FAILED(result)) throw std::runtime_error(hresult_message("Create background view", result));
        background_pixels_ = image;
    }

    void load_config() {
        config_error_.clear();
        if (config_.load(modules_, config_error_)) {
            config_writable_ = true;
        } else {
            // Preserve malformed or unreadable files; never overwrite them automatically.
            config_writable_ = false;
            overlay_.gui().set_status("Load failed: " + config_error_);
        }
        modules_.reset_keys();
    }

    void save_config() {
        if (!config_writable_) return;
        config_error_.clear();
        if (!config_.save(modules_, config_error_)) overlay_.gui().set_status("Save failed: " + config_error_);
    }

    void create_device() {
        RECT client{};
        GetClientRect(window_, &client);
        const UINT width = options_.offscreen() ? static_cast<UINT>(options_.width) : std::max<UINT>(1, static_cast<UINT>(client.right));
        const UINT height = options_.offscreen() ? static_cast<UINT>(options_.height) : std::max<UINT>(1, static_cast<UINT>(client.bottom));
        DXGI_SWAP_CHAIN_DESC description{};
        description.BufferCount = 2;
        description.BufferDesc.Width = width;
        description.BufferDesc.Height = height;
        description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.OutputWindow = window_;
        description.SampleDesc.Count = 1;
        description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        if (options_.debug_layer) flags |= D3D11_CREATE_DEVICE_DEBUG;
        auto create = [&](D3D_DRIVER_TYPE driver) {
            return D3D11CreateDeviceAndSwapChain(nullptr, driver, nullptr, flags, levels, static_cast<UINT>(std::size(levels)),
                D3D11_SDK_VERSION, &description, swap_chain_.ReleaseAndGetAddressOf(), device_.ReleaseAndGetAddressOf(), nullptr,
                context_.ReleaseAndGetAddressOf());
        };
        HRESULT result = create(D3D_DRIVER_TYPE_HARDWARE);
        if (FAILED(result) && !options_.debug_layer) result = create(D3D_DRIVER_TYPE_WARP);
        if (FAILED(result)) {
            throw std::runtime_error(hresult_message(options_.debug_layer
                ? "D3D11CreateDeviceAndSwapChain with the debug layer (install Graphics Tools)" : "D3D11CreateDeviceAndSwapChain", result));
        }
        textures_ = std::make_unique<medicon::gui::D3D11TextureHost>(device_.Get());
        overlay_.set_texture_host(textures_.get());
        create_render_target();
        target_width_ = static_cast<int>(width);
        target_height_ = static_cast<int>(height);
        if (options_.offscreen()) create_offscreen();
        if (!logged_device_) {
            logged_device_ = true;
            ComPtr<IDXGIDevice> dxgi;
            if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(dxgi.GetAddressOf())))) {
                ComPtr<IDXGIAdapter> adapter;
                if (SUCCEEDED(dxgi->GetAdapter(adapter.GetAddressOf()))) {
                    DXGI_ADAPTER_DESC desc{};
                    adapter->GetDesc(&desc);
                    LARGE_INTEGER version{};
                    std::string driver = "unknown";
                    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version))) {
                        driver = std::to_string(HIWORD(version.HighPart)) + "." + std::to_string(LOWORD(version.HighPart)) + "." +
                                 std::to_string(HIWORD(version.LowPart)) + "." + std::to_string(LOWORD(version.LowPart));
                    }
                    log("adapter: " + narrow(desc.Description) + " driver " + driver);
                }
            }
            log(std::string("backbuffer format: ") + format_name(description.BufferDesc.Format) + ", RTV format: " +
                format_name(DXGI_FORMAT_R8G8B8A8_UNORM) + (options_.offscreen() ? " (offscreen target, same format)" : ""));
        }
    }

    void create_render_target() {
        ComPtr<ID3D11Texture2D> back_buffer;
        HRESULT result = swap_chain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.GetAddressOf()));
        if (FAILED(result)) throw std::runtime_error(hresult_message("GetBuffer", result));
        result = device_->CreateRenderTargetView(back_buffer.Get(), nullptr, render_target_.ReleaseAndGetAddressOf());
        if (FAILED(result)) throw std::runtime_error(hresult_message("CreateRenderTargetView", result));
    }

    void create_offscreen() {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(options_.width);
        desc.Height = static_cast<UINT>(options_.height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT result = device_->CreateTexture2D(&desc, nullptr, offscreen_.ReleaseAndGetAddressOf());
        if (FAILED(result)) throw std::runtime_error(hresult_message("Create offscreen target", result));
        result = device_->CreateRenderTargetView(offscreen_.Get(), nullptr, offscreen_rtv_.ReleaseAndGetAddressOf());
        if (FAILED(result)) throw std::runtime_error(hresult_message("Create offscreen RTV", result));
    }

    void release_device(bool final_release) {
        // Forget texture IDs before destroying the host which owns the SRVs.
        overlay_.set_texture_host(nullptr);
        textures_.reset();
        edge_lit_.reset();
        if (context_.Get()) {
            context_->OMSetRenderTargets(0, nullptr, nullptr);
            context_->ClearState();
            context_->Flush();
        }
        timer_.release();
        background_srv_.Reset();
        offscreen_rtv_.Reset();
        offscreen_.Reset();
        render_target_.Reset();
        swap_chain_.Reset();
        if (final_release && options_.debug_layer && device_.Get()) report_debug_layer();
        context_.Reset();
        device_.Reset();
    }

    // Device loss: drop every GPU object; CPU-side GUI state (positions, scroll,
    // animation, config) lives in the Overlay and survives untouched.
    void recreate_device() {
        ImGui_ImplDX11_Shutdown();
        dx11_initialized_ = false;
        release_device(false);
        create_device();
        if (!ImGui_ImplDX11_Init(device_.Get(), context_.Get())) throw std::runtime_error("Re-initialize ImGui DX11 backend");
        dx11_initialized_ = true;
        if (options_.gpu_timing) timer_.create(device_.Get(), context_.Get());
        if (!background_pixels_.rgba.empty()) load_background();
        ++device_recreations_;
        log("device recreated (" + std::to_string(device_recreations_) + ")");
    }

    void report_debug_layer() {
        ComPtr<ID3D11InfoQueue> queue;
        if (FAILED(device_->QueryInterface(IID_PPV_ARGS(queue.GetAddressOf())))) {
            log("debug layer: ID3D11InfoQueue unavailable");
            return;
        }
        ComPtr<ID3D11Debug> debug;
        if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(debug.GetAddressOf())))) {
            debug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL);
        }
        int errors = 0, warnings = 0, live = 0;
        const UINT64 count = queue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size = 0;
            queue->GetMessage(i, nullptr, &size);
            std::vector<char> buffer(size);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(buffer.data());
            if (FAILED(queue->GetMessage(i, message, &size))) continue;
            const std::string text(message->pDescription, message->DescriptionByteLength > 0 ? message->DescriptionByteLength - 1 : 0);
            const bool live_report = text.find("Live ") != std::string::npos;
            // The device itself (and its implicit objects) is alive while reporting.
            const bool device_self = live_report && (text.find("ID3D11Device") != std::string::npos ||
                                                     text.find("Live                         Object") != std::string::npos);
            if (live_report && !device_self) ++live;
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR && !live_report) ++errors;
            else if (message->Severity == D3D11_MESSAGE_SEVERITY_WARNING && !live_report) ++warnings;
            if (!device_self) log("d3d11: " + text);
        }
        debug_summary_ = "debug layer: errors=" + std::to_string(errors) + " warnings=" + std::to_string(warnings) +
                         " live objects (excluding device)=" + std::to_string(live);
        log(debug_summary_);
    }

    void resize_if_needed() {
        if (pending_width_ == 0 || pending_height_ == 0) return;
        // Release every reference to the back buffer before ResizeBuffers.
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        render_target_.Reset();
        const HRESULT result = swap_chain_->ResizeBuffers(0, pending_width_, pending_height_, DXGI_FORMAT_UNKNOWN, 0);
        target_width_ = static_cast<int>(pending_width_);
        target_height_ = static_cast<int>(pending_height_);
        pending_width_ = 0;
        pending_height_ = 0;
        if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
            recreate_device();
            return;
        }
        if (FAILED(result)) throw std::runtime_error(hresult_message("ResizeBuffers", result));
        create_render_target();
        ++resizes_;
    }

    void run_gpu_timing() {
        if (options_.scenario.empty() && !options_.main_menu && !options_.main_menu_time && !options_.splash_time)
            medicon::preview::run_scenario("open", *this, false);
        for (int i = 0; i < 60; ++i) frame(); // warm-up, discarded
        timer_.flush();
        timer_.recording = true;
        for (int i = 0; i < options_.gpu_frames; ++i) frame();
        timer_.flush();
        if (timer_.total_ms.size() != static_cast<std::size_t>(options_.gpu_frames))
            throw std::runtime_error("GPU timing did not collect every requested frame");
        char line[320];
        std::snprintf(line, sizeof line,
                      "gpu_timing frames=%zu ui_total_ms mean=%.4f median=%.4f p95=%.4f max=%.4f shadow_ms median=%.4f p95=%.4f shadows=%s",
                      timer_.total_ms.size(), mean(timer_.total_ms), percentile(timer_.total_ms, 0.5),
                      percentile(timer_.total_ms, 0.95), percentile(timer_.total_ms, 1.0),
                      percentile(timer_.shadow_ms, 0.5), percentile(timer_.shadow_ms, 0.95), options_.no_shadow ? "off" : "on");
        log(line);
        std::vector<double> cpu_total(cpu_build_ms_.size());
        for (std::size_t i = 0; i < cpu_total.size(); ++i) cpu_total[i] = cpu_prepare_ms_[i] + cpu_build_ms_[i] + cpu_submit_ms_[i];
        std::snprintf(line, sizeof line, "cpu_total_ms mean=%.4f median=%.4f p95=%.4f max=%.4f", mean(cpu_total),
                      percentile(cpu_total, 0.5), percentile(cpu_total, 0.95), percentile(cpu_total, 1.0));
        log(line);
        char cpu_line[384];
        std::snprintf(cpu_line, sizeof cpu_line,
            "cpu_timing frames=%zu prepare_ms median=%.4f p95=%.4f build_ms median=%.4f p95=%.4f submit_ms median=%.4f p95=%.4f",
            cpu_build_ms_.size(), percentile(cpu_prepare_ms_, 0.5), percentile(cpu_prepare_ms_, 0.95),
            percentile(cpu_build_ms_, 0.5), percentile(cpu_build_ms_, 0.95),
            percentile(cpu_submit_ms_, 0.5), percentile(cpu_submit_ms_, 0.95));
        log(cpu_line);
    }

    void report_shadow_stats() {
        const auto& stats = overlay_.shadows().stats();
        log("shadow: draws=" + std::to_string(stats.draws) + " vertices/draw=" +
            std::to_string(stats.draws ? stats.vertices / stats.draws : 0) + " mesh builds=" + std::to_string(stats.geometry_builds) +
            " font uploads=" + std::to_string(font_uploads_) + " device recreations=" + std::to_string(device_recreations_) +
            " resizes=" + std::to_string(resizes_) + " frames=" + std::to_string(frame_index_));
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* app = reinterpret_cast<PreviewApplication*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            app = static_cast<PreviewApplication*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        // Synthetic input tests run in a hidden window: TrackMouseEvent posts an
        // immediate WM_MOUSELEAVE there, which would invalidate the injected position.
        if (app && (app->options_.input_test || !app->options_.scenario.empty()) && message == WM_MOUSELEAVE) return 0;
        // A focused Social input takes the IME's messages, as in the game window.
        if (app) {
            medicon::gui::ImeBridge::LParam ime_result = 0;
            if (app->ime_.window_message(window, message, wparam, lparam, ime_result)) return static_cast<LRESULT>(ime_result);
        }
        const bool has_imgui = ImGui::GetCurrentContext() != nullptr;
        const bool handled = has_imgui && ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
        if (app) {
            switch (message) {
            case WM_GETMINMAXINFO: {
                // Allow client areas larger than the desktop (e.g. --size 3840x2160).
                auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
                info->ptMaxTrackSize = {16384, 16384};
                return 0;
            }
            case WM_SIZE:
                app->minimized_ = wparam == SIZE_MINIMIZED;
                if (!app->minimized_) {
                    app->pending_width_ = LOWORD(lparam);
                    app->pending_height_ = HIWORD(lparam);
                }
                return 0;
            case WM_KILLFOCUS:
                app->modules_.reset_keys();
                break;
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                app->overlay_.key_event(virtual_key(wparam, lparam), true, (lparam & (1LL << 30)) != 0);
                break;
            case WM_KEYUP:
            case WM_SYSKEYUP:
                app->overlay_.key_event(virtual_key(wparam, lparam), false, false);
                break;
            case WM_CHAR:
                app->overlay_.char_event(static_cast<wchar_t>(wparam));
                break;
            case WM_XBUTTONDOWN:
            case WM_XBUTTONDBLCLK:
            case WM_XBUTTONUP: {
                const int key = GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
                app->overlay_.key_event(key, message != WM_XBUTTONUP, false);
                break;
            }
            case WM_SYSCOMMAND:
                if ((wparam & 0xFFF0) == SC_KEYMENU) return 0;
                break;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            case WM_NCDESTROY:
                app->window_ = nullptr;
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                break;
            }
        }
        return handled ? 1 : DefWindowProcW(window, message, wparam, lparam);
    }

    static constexpr const wchar_t* class_name_ = L"MediconPreviewWindow";
    Options options_;
    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    ATOM window_class_ = 0;
    bool imgui_initialized_ = false;
    bool win32_initialized_ = false;
    bool dx11_initialized_ = false;
    bool session_started_ = false;
    bool config_writable_ = true;
    bool minimized_ = false;
    bool done_ = false;
    bool logged_device_ = false;
    UINT pending_width_ = 0;
    UINT pending_height_ = 0;
    int target_width_ = 0;
    int target_height_ = 0;
    int frame_index_ = 0;
    int font_uploads_ = 0;
    std::function<void()> frame_hook_;
    medicon::gui::ImeBridge ime_;
    int device_recreations_ = 0;
    int resizes_ = 0;
    std::chrono::steady_clock::time_point previous_frame_ = std::chrono::steady_clock::now();
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swap_chain_;
    ComPtr<ID3D11RenderTargetView> render_target_;
    ComPtr<ID3D11Texture2D> offscreen_;
    ComPtr<ID3D11RenderTargetView> offscreen_rtv_;
    ComPtr<ID3D11ShaderResourceView> background_srv_;
    Image background_pixels_;
    GpuTimer timer_;
    std::vector<double> cpu_prepare_ms_, cpu_build_ms_, cpu_submit_ms_;
    medicon::ModuleManager modules_;
    medicon::ConfigStore config_;
    medicon::gui::Overlay overlay_;
    std::unique_ptr<medicon::gui::D3D11TextureHost> textures_;
    medicon::gui::EdgeLitD3D11 edge_lit_;
    std::string config_error_;
    std::string debug_summary_;
    std::mutex log_mutex_;
    std::ofstream log_stream_;
    // Destroyed first: its threads stop before anything they report to.
    std::unique_ptr<medicon::social::SocialService> social_;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    bool quiet = false;
    try {
        Options options = parse_options();
        quiet = options.hidden();
        PreviewApplication application(std::move(options));
        return application.run(instance, show_command);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Medicon Preview failed: %s\n", error.what());
        if (!quiet) MessageBoxA(nullptr, error.what(), "Medicon Preview", MB_OK | MB_ICONERROR);
        return 1;
    }
}
