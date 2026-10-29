#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// This executable deliberately has no dependency on the Medicon or ImGui APIs.
// The only DLL calls are the automatic-overlay lifecycle and diagnostic exports.
namespace {
constexpr UINT initial_width = 1280;
constexpr UINT initial_height = 800;
constexpr float clear_color[]{0.02F, 0.03F, 0.04F, 1.0F};
constexpr auto splash_wait = std::chrono::milliseconds(3200);

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void check(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) return;
    char message[160]{};
    std::snprintf(message, sizeof message, "%s failed (HRESULT 0x%08lX)",
                  operation, static_cast<unsigned long>(result));
    throw std::runtime_error(message);
}

template<class T> class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T* get() const { return pointer_; }
    T* operator->() const { return pointer_; }
    T** put() { reset(); return &pointer_; }
    void reset() { if (pointer_) { pointer_->Release(); pointer_ = nullptr; } }
private:
    T* pointer_ = nullptr;
};

std::atomic_uint game_movement_down{}, game_movement_up{}, game_mouse_down{}, game_mouse_up{}, game_chat_down{};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_KEYDOWN && wparam == 'W') ++game_movement_down;
    if (message == WM_KEYUP && wparam == 'W') ++game_movement_up;
    if (message == WM_KEYDOWN && wparam == 'T') ++game_chat_down;
    if (message == WM_LBUTTONDOWN) ++game_mouse_down;
    if (message == WM_LBUTTONUP) ++game_mouse_up;
    return DefWindowProcW(window, message, wparam, lparam);
}

class TestWindow {
public:
    explicit TestWindow(bool threaded_input) {
        if (!threaded_input) { create(); return; }
        std::promise<void> initialized;
        auto ready = initialized.get_future();
        ui_thread_ = std::thread([this, initialized = std::move(initialized)]() mutable {
            try {
                create();
                thread_id_ = GetCurrentThreadId();
                initialized.set_value();
            } catch (...) {
                initialized.set_exception(std::current_exception());
                return;
            }
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            destroy();
        });
        try { ready.get(); }
        catch (...) { ui_thread_.join(); throw; }
    }
    ~TestWindow() {
        if (ui_thread_.joinable()) {
            PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
            ui_thread_.join();
        } else destroy();
    }
    HWND get() const { return window_; }
    void resize(UINT width, UINT height) {
        require(SetWindowPos(window_, nullptr, 0, 0, static_cast<int>(width),
                             static_cast<int>(height), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != 0,
                "SetWindowPos");
    }
private:
    void create() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = window_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MediconAutomaticOverlayIntegrationHost";
        require(RegisterClassW(&wc) != 0, "RegisterClassW");
        window_ = CreateWindowExW(0, wc.lpszClassName, L"Medicon overlay integration host",
                                 WS_POPUP, 0, 0, initial_width, initial_height,
                                 nullptr, nullptr, wc.hInstance, nullptr);
        require(window_ != nullptr, "CreateWindowExW");
        // The window is intentionally never shown or focused. Input is sent only
        // to this host's own message queue, and the desktop is not affected.
    }
    void destroy() {
        if (window_) DestroyWindow(window_);
        UnregisterClassW(L"MediconAutomaticOverlayIntegrationHost", GetModuleHandleW(nullptr));
    }
    HWND window_ = nullptr;
    DWORD thread_id_ = 0;
    std::thread ui_thread_;
};

class TemporaryConfig {
public:
    TemporaryConfig() {
        const DWORD size = GetEnvironmentVariableW(L"MEDICON_OVERLAY_CONFIG", nullptr, 0);
        if (size != 0) {
            previous_.resize(size);
            const DWORD copied = GetEnvironmentVariableW(L"MEDICON_OVERLAY_CONFIG", previous_.data(), size);
            previous_.resize(copied);
        }
        path_ = std::filesystem::temp_directory_path() /
            (L"medicon-overlay-host-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()) + L".json");
        // A visible module is required to exercise Edge-lit's actual shaders,
        // background capture, Gaussian passes and D3D11On12 resource lifetime.
        // Category panels alone would leave that complete path untested.
        std::ofstream config(path_, std::ios::binary);
        // The closed-GUI pixel assertion below deliberately expects only the
        // right-hand Example row. Watermark defaults ON and must be isolated
        // out of this fixture, rather than mistaken for an opened ClickGUI.
        config << R"({"version":1,"modules":{"example":{"enabled":true},"arraylist":{"enabled":true},"watermark":{"enabled":false},"clickgui":{"enabled":true}}})";
        config.close();
        require(static_cast<bool>(config), "Write isolated config with an enabled HUD module");
        require(SetEnvironmentVariableW(L"MEDICON_OVERLAY_CONFIG", path_.c_str()) != 0,
                "Set isolated overlay configuration path");
        // Social creates social.json on start; keep it out of the user's %APPDATA%.
        social_dir_ = path_;
        social_dir_.replace_extension(L".social");
        require(SetEnvironmentVariableW(L"MEDICON_SOCIAL_DIR", social_dir_.c_str()) != 0,
                "Set isolated Social directory");
    }
    ~TemporaryConfig() {
        SetEnvironmentVariableW(L"MEDICON_OVERLAY_CONFIG", previous_.empty() ? nullptr : previous_.c_str());
        SetEnvironmentVariableW(L"MEDICON_SOCIAL_DIR", nullptr);
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove_all(social_dir_, ignored);
    }
private:
    std::wstring previous_;
    std::filesystem::path path_;
    std::filesystem::path social_dir_;
};

void pump_messages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

struct Image {
    UINT width = 0;
    UINT height = 0;
    std::vector<std::uint8_t> pixels;
};

void save_debug_image(const Image& image, const char* name) {
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(image.pixels.size());
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(image.width);
    info.biHeight = -static_cast<LONG>(image.height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    auto pixels = image.pixels;
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
    std::ofstream output(name, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
}

std::size_t changed_pixels(const Image& image, UINT right) {
    std::size_t changed = 0;
    // Restrict the proof to the upper half, where the category panels appear.
    // Ignore alpha, since presentation may replace it on some DXGI drivers.
    for (UINT y = 0; y < image.height / 2; ++y) {
        for (UINT x = 0; x < right; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * 4;
            bool different = false;
            for (UINT c = 0; c < 3; ++c) {
                const int expected = static_cast<int>(std::lround(clear_color[c] * 255.0F));
                different |= std::abs(static_cast<int>(image.pixels[offset + c]) - expected) > 3;
            }
            if (different) ++changed;
        }
    }
    return changed;
}

std::size_t overlay_pixels(const Image& image) { return changed_pixels(image, image.width); }

std::size_t category_panel_pixels(const Image& image) {
    // Exclude the right-hand ArrayList. A closed GUI must leave the host's
    // original clear color throughout the category-panel area after the intro.
    return changed_pixels(image, image.width * 3 / 4);
}

std::size_t pastel_pixels(const Image& image) {
    std::size_t pastel = 0;
    for (std::size_t offset = 0; offset + 3 < image.pixels.size(); offset += 4) {
        if (image.pixels[offset] >= 110 && image.pixels[offset + 1] >= 170 && image.pixels[offset + 2] >= 190)
            ++pastel;
    }
    return pastel;
}

std::size_t edge_lit_tube_rows(const Image& image) {
    if (image.width < 9 || image.height < 32) return 0;
    std::size_t bright = 0;
    // The single Example row has a 24 px tube at right-9..right-6, y=8..32.
    // Its white core covers x=right-8, y=10..30. Check every interior scanline
    // in that exact physical location, independent of category-panel pixels.
    for (UINT y = 10; y < 30; ++y) {
        const auto offset = (static_cast<std::size_t>(y) * image.width + image.width - 8) * 4;
        if (image.pixels[offset] > 100 && image.pixels[offset + 2] > 100) ++bright;
    }
    return bright;
}

class GraphicsHost {
public:
    virtual ~GraphicsHost() = default;
    virtual void render() = 0;
    virtual Image read_last_presented() = 0;
    virtual void resize(UINT width, UINT height) = 0;
};

class D3D11Host final : public GraphicsHost {
public:
    explicit D3D11Host(HWND window, bool present1) : present1_(present1) {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = initial_width;
        desc.BufferDesc.Height = initial_height;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 1;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        // Sequential preserves the buffer after Present, allowing readback on
        // the next host frame instead of racing the overlay's Present hook.
        desc.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        HRESULT result = E_FAIL;
        for (const auto driver : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
            result = D3D11CreateDeviceAndSwapChain(nullptr, driver, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION,
                &desc, swapchain_.put(), device_.put(), nullptr, context_.put());
            if (SUCCEEDED(result)) break;
        }
        check(result, "D3D11CreateDeviceAndSwapChain (hardware and WARP)");
        if (present1_) check(swapchain_->QueryInterface(IID_PPV_ARGS(advanced_swapchain_.put())),
                             "D3D11 query IDXGISwapChain3 for Present1/ResizeBuffers1");
        create_targets();
    }
    ~D3D11Host() override {
        if (context_.get()) { context_->ClearState(); context_->Flush(); }
    }
    void render() override {
        ID3D11RenderTargetView* target = target_.get();
        context_->OMSetRenderTargets(1, &target, nullptr);
        context_->ClearRenderTargetView(target, clear_color);
        if (present1_) {
            const DXGI_PRESENT_PARAMETERS parameters{};
            check(advanced_swapchain_->Present1(0, 0, &parameters), "D3D11 Present1");
        } else check(swapchain_->Present(0, 0), "D3D11 Present");
    }
    Image read_last_presented() override {
        context_->CopyResource(readback_.get(), backbuffer_.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context_->Map(readback_.get(), 0, D3D11_MAP_READ, 0, &mapped), "D3D11 readback Map");
        Image image{width_, height_, std::vector<std::uint8_t>(static_cast<std::size_t>(width_) * height_ * 4)};
        for (UINT y = 0; y < height_; ++y) {
            std::memcpy(image.pixels.data() + static_cast<std::size_t>(y) * width_ * 4,
                        static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
                        static_cast<std::size_t>(width_) * 4);
        }
        context_->Unmap(readback_.get(), 0);
        return image;
    }
    void resize(UINT width, UINT height) override {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        target_.reset();
        backbuffer_.reset();
        readback_.reset();
        context_->Flush();
        if (present1_) check(advanced_swapchain_->ResizeBuffers1(1, width, height, DXGI_FORMAT_UNKNOWN, 0,
                                                                nullptr, nullptr), "D3D11 ResizeBuffers1");
        else check(swapchain_->ResizeBuffers(1, width, height, DXGI_FORMAT_UNKNOWN, 0), "D3D11 ResizeBuffers");
        create_targets();
    }
private:
    void create_targets() {
        check(swapchain_->GetBuffer(0, IID_PPV_ARGS(backbuffer_.put())), "D3D11 GetBuffer");
        check(device_->CreateRenderTargetView(backbuffer_.get(), nullptr, target_.put()), "D3D11 CreateRenderTargetView");
        D3D11_TEXTURE2D_DESC desc{};
        backbuffer_->GetDesc(&desc);
        width_ = desc.Width;
        height_ = desc.Height;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        check(device_->CreateTexture2D(&desc, nullptr, readback_.put()), "D3D11 create staging texture");
    }
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swapchain_;
    ComPtr<IDXGISwapChain3> advanced_swapchain_;
    ComPtr<ID3D11Texture2D> backbuffer_;
    ComPtr<ID3D11Texture2D> readback_;
    ComPtr<ID3D11RenderTargetView> target_;
    UINT width_ = 0;
    UINT height_ = 0;
    bool present1_ = false;
};

class D3D12Host final : public GraphicsHost {
public:
    explicit D3D12Host(HWND window, bool present1) : present1_(present1) {
        ComPtr<IDXGIFactory4> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "CreateDXGIFactory1");
        HRESULT result = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device_.put()));
        if (FAILED(result)) {
            ComPtr<IDXGIAdapter> warp;
            check(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.put())), "EnumWarpAdapter");
            result = D3D12CreateDevice(warp.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device_.put()));
        }
        check(result, "D3D12CreateDevice (hardware and WARP)");
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue_.put())), "D3D12 CreateCommandQueue");
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = initial_width;
        desc.Height = initial_height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = buffer_count;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        ComPtr<IDXGISwapChain1> swapchain;
        check(factory->CreateSwapChainForHwnd(queue_.get(), window, &desc, nullptr, nullptr, swapchain.put()),
              "D3D12 CreateSwapChainForHwnd");
        check(swapchain->QueryInterface(IID_PPV_ARGS(swapchain_.put())), "Query IDXGISwapChain3");
        check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator_.put())),
              "D3D12 CreateCommandAllocator");
        check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.get(), nullptr,
                                        IID_PPV_ARGS(commands_.put())), "D3D12 CreateCommandList");
        check(commands_->Close(), "D3D12 initial command list Close");
        check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.put())), "D3D12 CreateFence");
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(event_ != nullptr, "CreateEventW for D3D12 fence");
        create_targets();
    }
    ~D3D12Host() override {
        if (queue_.get() && fence_.get()) {
            // Destructors must not hide the test's original failure.
            try { wait_for_gpu(); } catch (...) {}
        }
        if (event_) CloseHandle(event_);
    }
    void render() override {
        begin_commands();
        last_presented_ = swapchain_->GetCurrentBackBufferIndex();
        auto* buffer = buffers_[last_presented_].get();
        barrier(buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto target = targets_->GetCPUDescriptorHandleForHeapStart();
        target.ptr += static_cast<SIZE_T>(last_presented_) * descriptor_size_;
        commands_->ClearRenderTargetView(target, clear_color, 0, nullptr);
        barrier(buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        submit_commands();
        if (present1_) {
            const DXGI_PRESENT_PARAMETERS parameters{};
            check(swapchain_->Present1(0, 0, &parameters), "D3D12 Present1");
        } else check(swapchain_->Present(0, 0), "D3D12 Present");
        // Signal after Present: this also waits for commands the overlay hook
        // submitted to the captured host queue.
        wait_for_gpu();
    }
    Image read_last_presented() override {
        begin_commands();
        auto* buffer = buffers_[last_presented_].get();
        const auto desc = buffer->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 size = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC readback_desc{};
        readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback_desc.Width = size;
        readback_desc.Height = 1;
        readback_desc.DepthOrArraySize = 1;
        readback_desc.MipLevels = 1;
        readback_desc.SampleDesc.Count = 1;
        readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(readback.put())), "D3D12 create readback buffer");
        barrier(buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = buffer;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback.get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        commands_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        barrier(buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        submit_commands();
        wait_for_gpu();
        void* mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(size)};
        check(readback->Map(0, &range, &mapped), "D3D12 readback Map");
        const UINT width = static_cast<UINT>(desc.Width);
        const UINT height = desc.Height;
        Image image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
        for (UINT y = 0; y < height; ++y) {
            std::memcpy(image.pixels.data() + static_cast<std::size_t>(y) * width * 4,
                        static_cast<const std::uint8_t*>(mapped) + footprint.Offset +
                            static_cast<std::size_t>(y) * footprint.Footprint.RowPitch,
                        static_cast<std::size_t>(width) * 4);
        }
        const D3D12_RANGE written{0, 0};
        readback->Unmap(0, &written);
        return image;
    }
    void resize(UINT width, UINT height) override {
        wait_for_gpu();
        for (auto& buffer : buffers_) buffer.reset();
        targets_.reset();
        if (present1_) {
            const UINT node_masks[buffer_count]{1, 1};
            IUnknown* queues[buffer_count]{queue_.get(), queue_.get()};
            check(swapchain_->ResizeBuffers1(buffer_count, width, height, DXGI_FORMAT_UNKNOWN, 0,
                                             node_masks, queues), "D3D12 ResizeBuffers1");
        } else check(swapchain_->ResizeBuffers(buffer_count, width, height, DXGI_FORMAT_UNKNOWN, 0), "D3D12 ResizeBuffers");
        create_targets();
    }
private:
    void create_targets() {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = buffer_count;
        check(device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(targets_.put())), "D3D12 CreateDescriptorHeap");
        descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto target = targets_->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < buffer_count; ++i) {
            check(swapchain_->GetBuffer(i, IID_PPV_ARGS(buffers_[i].put())), "D3D12 GetBuffer");
            device_->CreateRenderTargetView(buffers_[i].get(), nullptr, target);
            target.ptr += descriptor_size_;
        }
    }
    void begin_commands() {
        wait_for_gpu();
        check(allocator_->Reset(), "D3D12 allocator Reset");
        check(commands_->Reset(allocator_.get(), nullptr), "D3D12 command list Reset");
    }
    void submit_commands() {
        check(commands_->Close(), "D3D12 command list Close");
        ID3D12CommandList* lists[]{commands_.get()};
        queue_->ExecuteCommandLists(1, lists);
    }
    void barrier(ID3D12Resource* buffer, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER value{};
        value.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        value.Transition.pResource = buffer;
        value.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        value.Transition.StateBefore = before;
        value.Transition.StateAfter = after;
        commands_->ResourceBarrier(1, &value);
    }
    void wait_for_gpu() {
        const UINT64 value = ++fence_value_;
        check(queue_->Signal(fence_.get(), value), "D3D12 queue Signal");
        if (fence_->GetCompletedValue() >= value) return;
        check(fence_->SetEventOnCompletion(value, event_), "D3D12 SetEventOnCompletion");
        require(WaitForSingleObject(event_, 15000) == WAIT_OBJECT_0, "D3D12 GPU fence timed out");
    }
    static constexpr UINT buffer_count = 2;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapchain_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> commands_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12DescriptorHeap> targets_;
    std::array<ComPtr<ID3D12Resource>, buffer_count> buffers_;
    HANDLE event_ = nullptr;
    UINT64 fence_value_ = 0;
    UINT descriptor_size_ = 0;
    UINT last_presented_ = 0;
    bool present1_ = false;
};

template<typename Function> Function load(HMODULE module, const char* name) {
    const auto address = GetProcAddress(module, name);
    require(address != nullptr, std::string("Missing overlay export: ") + name);
    return reinterpret_cast<Function>(address);
}

HWND concurrent_input_target = nullptr;

void frames(GraphicsHost& host, unsigned count = 24) {
    for (unsigned frame = 0; frame < count; ++frame) {
        pump_messages();
        if (concurrent_input_target) {
            // The owner thread dispatches these while Present runs on this
            // thread. This catches backends that touch ImGui from WndProc.
            PostMessageW(concurrent_input_target, WM_MOUSEMOVE, 0, MAKELPARAM(1, 1));
        }
        host.render();
        Sleep(16);
    }
}

void insert(HWND window) {
    const LPARAM key = 1 | (static_cast<LPARAM>(0x52) << 16) | (static_cast<LPARAM>(1) << 24);
    require(PostMessageW(window, WM_KEYDOWN, VK_INSERT, key) != 0, "Post Insert key down");
    require(PostMessageW(window, WM_KEYUP, VK_INSERT,
                        key | (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31)) != 0,
            "Post Insert key up");
}

void model_probe_key(HWND window) {
    const LPARAM key = 1 | (static_cast<LPARAM>(0x42) << 16);
    require(PostMessageW(window, WM_KEYDOWN, VK_F8, key) != 0, "Post P0 key down");
    require(PostMessageW(window, WM_KEYUP, VK_F8,
                        key | (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31)) != 0,
            "Post P0 key up");
}

} // namespace

int main(int argc, char** argv) {
    bool d3d12 = false;
    bool late_load = false;
    bool present1 = false;
    bool threaded_input = false;
    bool valid_options = argc >= 2 && argc <= 6;
    for (int argument = 2; argument < argc; ++argument) {
        const std::string option = argv[argument];
        if (option == "--d3d12" && !d3d12) d3d12 = true;
        else if (option == "--late-load" && !late_load) late_load = true;
        else if (option == "--present1" && !present1) present1 = true;
        else if (option == "--threaded-input" && !threaded_input) threaded_input = true;
        else valid_options = false;
    }
    if (!valid_options) {
        std::cerr << "Usage: medicon_overlay_host_tests <Medicon.dll> [--d3d12] [--late-load]"
                     " [--present1] [--threaded-input]\n";
        return 2;
    }
    HMODULE module = nullptr;
    int (*stop)() = nullptr;
    std::uint32_t (*status)() = nullptr;
    std::uint32_t (*last_error)(char*, std::uint32_t) = nullptr;
    std::unique_ptr<TestWindow> window;
    std::unique_ptr<GraphicsHost> host;
    std::unique_ptr<TemporaryConfig> configuration;
    try {
        configuration = std::make_unique<TemporaryConfig>();
        window = std::make_unique<TestWindow>(threaded_input);
        if (threaded_input) concurrent_input_target = window->get();
        const auto create_host = [&] {
            if (d3d12) host = std::make_unique<D3D12Host>(window->get(), present1);
            else host = std::make_unique<D3D11Host>(window->get(), present1);
        };
        if (late_load) {
            create_host();
            frames(*host, 2);
            require(overlay_pixels(host->read_last_presented()) == 0, "Host clear-color baseline was not clean");
        }
        module = LoadLibraryW(std::filesystem::absolute(argv[1]).c_str());
        require(module != nullptr, "LoadLibraryW failed; Win32 error " + std::to_string(GetLastError()));
        const auto start = load<int(*)()>(module, "MediconStartOverlay");
        stop = load<int(*)()>(module, "MediconStopOverlay");
        status = load<std::uint32_t(*)()>(module, "MediconOverlayStatus");
        last_error = reinterpret_cast<decltype(last_error)>(GetProcAddress(module, "MediconOverlayLastError"));
        if (!last_error) last_error = reinterpret_cast<decltype(last_error)>(GetProcAddress(module, "MediconLastError"));
        require(start() == 1 && start() == 1, "Idempotent automatic-overlay start request");
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (status() == 1 && std::chrono::steady_clock::now() < deadline) {
            pump_messages();
            Sleep(10);
        }
        require(status() == 2 || status() == 3, "Overlay hook bootstrap did not become ready before device creation");

        // Default: hooks precede creation of the host device and queue. With
        // --late-load: capture an already-running host, like DLL injection.
        // Neither path supplies an ImGui context or renderer to the DLL.
        if (!late_load) create_host();
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        do {
            frames(*host, 1);
            require(status() != 4, "Overlay entered failed state during Present");
        } while (status() != 3 && std::chrono::steady_clock::now() < deadline);
        require(status() == 3, "Present hook did not start rendering within 15 seconds");
        const auto intro_image = host->read_last_presented();
        require(pastel_pixels(intro_image) > static_cast<std::size_t>(intro_image.width) * intro_image.height * 8 / 10,
                "First overlay frame did not show the full-screen pastel injection intro");
        // Measure from the first actual Present, after renderer/font setup.
        // Rendering continues throughout; a frame counter cannot time the intro.
        deadline = std::chrono::steady_clock::now() + splash_wait;
        do {
            frames(*host, 1);
            require(status() == 3, "Overlay stopped rendering during the injection intro");
        } while (std::chrono::steady_clock::now() < deadline);
        const auto startup_image = host->read_last_presented();
        const auto startup_closed = overlay_pixels(startup_image);
        if (category_panel_pixels(startup_image) != 0) save_debug_image(startup_image, "overlay-startup-failure.bmp");
        require(category_panel_pixels(startup_image) == 0,
                "Injection intro automatically opened ClickGUI despite its saved enabled state");
        require(edge_lit_tube_rows(startup_image) == 20,
                "ArrayList did not resume after the injection intro while ClickGUI stayed closed");

        insert(window->get());
        frames(*host);
        const auto visible = overlay_pixels(host->read_last_presented());
        require(visible > 3000 && visible > startup_closed + 2000,
                "The bound Insert key did not open ClickGUI after injection (" + std::to_string(visible) + ")");

        const auto movement_down = game_movement_down.load();
        const auto movement_up = game_movement_up.load();
        const auto chat_down = game_chat_down.load();
        const auto mouse_down = game_mouse_down.load();
        const auto mouse_up = game_mouse_up.load();
        SendMessageW(window->get(), WM_KEYDOWN, 'W', 1);
        SendMessageW(window->get(), WM_KEYUP, 'W', (1LL << 31) | (1LL << 30) | 1);
        SendMessageW(window->get(), WM_KEYDOWN, 'T', 1);
        SendMessageW(window->get(), WM_KEYUP, 'T', (1LL << 31) | (1LL << 30) | 1);
        require(game_movement_down == movement_down + 1 && game_movement_up == movement_up + 1,
                "ClickGUI did not forward movement down/up through the actual game WndProc");
        require(game_chat_down == chat_down, "ClickGUI leaked the game chat key");
        SendMessageW(window->get(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(1, 1));
        require(game_mouse_down == mouse_down, "ClickGUI click reached the game WndProc");

        insert(window->get());
        frames(*host);
        SendMessageW(window->get(), WM_LBUTTONUP, 0, MAKELPARAM(1, 1));
        require(game_mouse_up == mouse_up, "Closing ClickGUI leaked a GUI-owned mouse release");
        SendMessageW(window->get(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(1, 1));
        SendMessageW(window->get(), WM_LBUTTONUP, 0, MAKELPARAM(1, 1));
        require(game_mouse_down == mouse_down + 1 && game_mouse_up == mouse_up + 1,
                "A fresh game click remained blocked after ClickGUI closed");
        const auto closed_image = host->read_last_presented();
        const auto closed = overlay_pixels(closed_image);
        require(closed + 2000 < visible, "Insert did not close the rendered GUI (visible=" +
                std::to_string(visible) + ", closed=" + std::to_string(closed) + ")");
        require(edge_lit_tube_rows(closed_image) == 20,
                "GUI closed but Edge-lit tube/core was absent or discontinuous; effects path did not render");
        insert(window->get());
        // The opening key and following clicks can reach the message pump
        // together, before the next Present has opened the actual GUI.
        PostMessageW(window->get(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(1, 1));
        PostMessageW(window->get(), WM_LBUTTONUP, 0, MAKELPARAM(1, 1));
        frames(*host);
        require(game_mouse_down == mouse_down + 1 && game_mouse_up == mouse_up + 1,
                "A click immediately after the GUI opening key leaked before Present");
        require(overlay_pixels(host->read_last_presented()) > closed + 2000, "Insert did not reopen the rendered GUI");

        const auto before_social_movement = game_movement_down.load();
        SendMessageW(window->get(), WM_KEYDOWN, VK_HOME, 1);
        SendMessageW(window->get(), WM_KEYUP, VK_HOME, (1LL << 31) | (1LL << 30) | 1);
        SendMessageW(window->get(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(1, 1));
        SendMessageW(window->get(), WM_LBUTTONUP, 0, MAKELPARAM(1, 1));
        frames(*host);
        SendMessageW(window->get(), WM_KEYDOWN, 'W', 1);
        SendMessageW(window->get(), WM_KEYUP, 'W', (1LL << 31) | (1LL << 30) | 1);
        require(game_movement_down == before_social_movement && game_mouse_down == mouse_down + 1 && game_mouse_up == mouse_up + 1,
                "Social leaked attack or movement input through the actual game WndProc");
        SendMessageW(window->get(), WM_KEYDOWN, VK_ESCAPE, 1);
        SendMessageW(window->get(), WM_KEYUP, VK_ESCAPE, (1LL << 31) | (1LL << 30) | 1);
        frames(*host);
        SendMessageW(window->get(), WM_KEYDOWN, 'W', 1);
        SendMessageW(window->get(), WM_KEYUP, 'W', (1LL << 31) | (1LL << 30) | 1);
        require(game_movement_down == before_social_movement + 1,
                "ClickGUI movement did not resume after Social closed");

        window->resize(1024, 720);
        pump_messages();
        host->resize(1024, 720);
        frames(*host);
        const auto resized_image = host->read_last_presented();
        require(resized_image.width == 1024 && resized_image.height == 720, "Host backbuffer did not resize");
        const auto resized = overlay_pixels(resized_image);
        require(status() == 3 && resized > 3000, "Overlay did not render after ResizeBuffers");
        insert(window->get());
        frames(*host);
        require(edge_lit_tube_rows(host->read_last_presented()) == 20,
                "Edge-lit did not release/recreate back-buffer resources and render at the resized right edge");
        // F8 toggles suppression only. Show Debug defaults OFF independently,
        // so neither edge may add a floating diagnostic window.
        model_probe_key(window->get());
        frames(*host);
        require(category_panel_pixels(host->read_last_presented()) == 0,
                "F8 unexpectedly displayed the debug window with Show Debug OFF");
        model_probe_key(window->get());
        frames(*host);
        require(category_panel_pixels(host->read_last_presented()) == 0,
                "F8 OFF unexpectedly displayed the debug window");
        insert(window->get());
        frames(*host);

        require(stop() == 1 && stop() == 1, "Idempotent synchronous overlay stop");
        require(status() == 0, "Overlay status was not stopped after stop returned");
        frames(*host, 4);
        require(overlay_pixels(host->read_last_presented()) == 0, "Overlay pixels remained after synchronous stop");

        require(start() == 1 && start() == 1, "Overlay restart after synchronous stop");
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        do {
            frames(*host, 1);
            require(status() != 4, "Overlay entered failed state after restart");
        } while (status() != 3 && std::chrono::steady_clock::now() < deadline);
        require(status() == 3, "Overlay did not start rendering again after restart");
        const auto restart_intro = host->read_last_presented();
        require(pastel_pixels(restart_intro) > static_cast<std::size_t>(restart_intro.width) * restart_intro.height * 8 / 10,
                "Overlay restart did not display a fresh pastel intro");
        deadline = std::chrono::steady_clock::now() + splash_wait;
        do {
            frames(*host, 1);
            require(status() == 3, "Overlay stopped rendering during the restart intro");
        } while (std::chrono::steady_clock::now() < deadline);
        const auto restart_closed = host->read_last_presented();
        require(category_panel_pixels(restart_closed) == 0,
                "Overlay restart automatically opened the saved ClickGUI");
        require(edge_lit_tube_rows(restart_closed) == 20,
                "Edge-lit effects did not initialize again after the restart intro");
        insert(window->get());
        frames(*host);
        require(overlay_pixels(host->read_last_presented()) > 3000,
                "The bound Insert key did not open ClickGUI after the restart intro");
        require(stop() == 1, "Synchronous stop after restart");
        require(status() == 0, "Overlay status was not stopped after second stop");
        frames(*host, 4);
        require(overlay_pixels(host->read_last_presented()) == 0, "Overlay pixels remained after second stop");

        require(FreeLibrary(module) != 0, "FreeLibrary after overlay stop");
        module = nullptr;
        stop = nullptr;
        status = nullptr;
        last_error = nullptr;
        // A pinned module may remain mapped, but its hooks must be inactive.
        frames(*host, 4);
        require(overlay_pixels(host->read_last_presented()) == 0, "Rendering did not remain clean after FreeLibrary");
        host.reset();
        concurrent_input_target = nullptr;
        window.reset();
        std::cout << (d3d12 ? "D3D12" : "D3D11") << (late_load ? " late-load" : " early-load")
                  << (present1 ? " Present1/ResizeBuffers1" : " Present/ResizeBuffers")
                  << (threaded_input ? " separate UI thread" : " shared UI thread")
                  << " automatic overlay host passed: "
                  << visible << " GUI pixels, " << closed << " with GUI closed, " << resized
                  << " after resize; clean stop, restart and unload.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << (d3d12 ? "D3D12" : "D3D11") << " overlay host: " << error.what();
        if (module && status) std::cerr << " (overlay status " << status() << ")";
        std::cerr << '\n';
        if (module && last_error) {
            char diagnostic[2048]{};
            last_error(diagnostic, sizeof diagnostic);
            if (diagnostic[0]) std::cerr << "DLL diagnostic: " << diagnostic << '\n';
        }
        if (module && stop && stop() != 1) {
            // Do not destroy the HWND, device, or DLL beneath callbacks whose
            // lifetime could not be quiesced. Terminating this failed test
            // process avoids turning its original failure into an unload race.
            std::cerr << "Overlay cleanup could not quiesce callbacks; exiting without unloading.\n" << std::flush;
            std::_Exit(1);
        }
        host.reset();
        concurrent_input_target = nullptr;
        window.reset();
        if (module) FreeLibrary(module);
        return 1;
    }
}
