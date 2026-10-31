#include "OverlayRenderer.hpp"
#include "medicon/gui/EdgeLit.hpp"

#include <backends/imgui_impl_dx12.h>
#include <d3d11on12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace medicon::platform {
namespace {
using Microsoft::WRL::ComPtr;

bool failed(std::string& error, const char* operation, HRESULT result) {
    char code[16]{};
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(result));
    error = std::string(operation) + " failed (" + code + ").";
    return false;
}

class ContextScope final {
public:
    explicit ContextScope(ImGuiContext* context) : previous_(ImGui::GetCurrentContext()) {
        ImGui::SetCurrentContext(context);
    }
    ~ContextScope() { ImGui::SetCurrentContext(previous_); }
private:
    ImGuiContext* previous_;
};

// Slot 0 of the shader-visible heap is the ImGui font; the rest hold GUI textures.
constexpr UINT srv_slots = 16;

class D3D12Renderer final : public OverlayRenderer, public gui::TextureHost {
public:
    ~D3D12Renderer() override {
        gui::forget_edge_lit_frame(context_);
        // A stalled queue must not hang the game's render thread during shutdown.
        // If it is still using these objects, retain them until process exit rather
        // than freeing command allocators/upload buffers underneath the GPU.
        bool safe_to_release = !gpu_work_submitted_;
        try {
            if (!safe_to_release) {
                std::string ignored;
                safe_to_release = wait_idle(ignored) ||
                    (device_ && FAILED(device_->GetDeviceRemovedReason()));
            }
        } catch (...) {
            safe_to_release = false;
        }
        if (!safe_to_release) {
            abandon_in_flight_resources();
            return;
        }
        effects_.reset();
        if (interop_context_) { interop_context_->ClearState(); interop_context_->Flush(); }
        if (backend_initialized_) {
            ContextScope scope(context_);
            ImGui_ImplDX12_Shutdown();
        }
        if (fence_event_) CloseHandle(fence_event_);
    }

    bool initialize(IDXGISwapChain* swap, ID3D12CommandQueue* queue, std::string& error) override {
        if (device_ || backend_initialized_) {
            error = "The D3D12 overlay renderer is already initialized.";
            return false;
        }
        if (!swap || !queue || !ImGui::GetCurrentContext()) {
            error = "D3D12 rendering requires a swap chain, direct command queue, and ImGui context.";
            return false;
        }
        if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            error = "The captured D3D12 queue is not a direct graphics queue.";
            return false;
        }
        if (ImGui::GetIO().BackendRendererUserData) {
            error = "The ImGui context already has a renderer backend.";
            return false;
        }

        context_ = ImGui::GetCurrentContext();
        HRESULT hr = swap->GetDevice(IID_PPV_ARGS(device_.GetAddressOf()));
        if (FAILED(hr)) return failed(error, "Get D3D12 swap-chain device", hr);
        hr = swap->QueryInterface(IID_PPV_ARGS(swap_.GetAddressOf()));
        if (FAILED(hr)) return failed(error, "Query IDXGISwapChain3", hr);

        ComPtr<ID3D12Device> queue_device;
        hr = queue->GetDevice(IID_PPV_ARGS(queue_device.GetAddressOf()));
        if (FAILED(hr)) return failed(error, "Get D3D12 command-queue device", hr);
        ComPtr<IUnknown> swap_identity;
        ComPtr<IUnknown> queue_identity;
        hr = device_.As(&swap_identity);
        if (FAILED(hr)) return failed(error, "Query swap-chain device identity", hr);
        hr = queue_device.As(&queue_identity);
        if (FAILED(hr)) return failed(error, "Query command-queue device identity", hr);
        if (swap_identity.Get() != queue_identity.Get()) {
            error = "The captured D3D12 queue belongs to a different device.";
            return false;
        }
        queue_ = queue;

        DXGI_SWAP_CHAIN_DESC desc{};
        hr = swap_->GetDesc(&desc);
        if (FAILED(hr)) return failed(error, "Get D3D12 swap-chain description", hr);
        if (!desc.BufferCount || desc.SampleDesc.Count != 1) {
            error = "The D3D12 overlay requires a non-multisampled swap chain with back buffers.";
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = desc.BufferCount;
        hr = device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(rtv_heap_.GetAddressOf()));
        if (FAILED(hr)) return failed(error, "Create D3D12 render-target descriptor heap", hr);

        // This pinned ImGui backend uses one font SRV. Its invalidation path frees
        // the slot, while recreation reuses the same handle, so reserve it for the
        // entire renderer lifetime instead of returning it to a general allocator.
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = srv_slots;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(srv_heap_.GetAddressOf()));
        if (FAILED(hr)) return failed(error, "Create D3D12 font descriptor heap", hr);
        srv_increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        frames_.resize(desc.BufferCount);
        backend_fences_.resize(desc.BufferCount);
        auto rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        const UINT descriptor_size = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT i = 0; i < desc.BufferCount; ++i) {
            auto& frame = frames_[i];
            hr = swap_->GetBuffer(i, IID_PPV_ARGS(frame.back_buffer.GetAddressOf()));
            if (FAILED(hr)) return failed(error, "Get D3D12 back buffer", hr);
            hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(frame.allocator.GetAddressOf()));
            if (FAILED(hr)) return failed(error, "Create D3D12 command allocator", hr);
            frame.rtv = rtv;
            device_->CreateRenderTargetView(frame.back_buffer.Get(), nullptr, rtv);
            rtv.ptr += descriptor_size;
        }

        hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].allocator.Get(),
            nullptr, IID_PPV_ARGS(commands_.GetAddressOf()));
        if (FAILED(hr)) return failed(error, "Create D3D12 overlay command list", hr);
        hr = commands_->Close();
        if (FAILED(hr)) return failed(error, "Close initial D3D12 overlay command list", hr);
        hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.GetAddressOf()));
        if (FAILED(hr)) return failed(error, "Create D3D12 overlay fence", hr);
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event_) return failed(error, "Create D3D12 fence event", HRESULT_FROM_WIN32(GetLastError()));

        ImGui_ImplDX12_InitInfo info{};
        info.Device = device_.Get();
        info.CommandQueue = queue_.Get();
        info.NumFramesInFlight = static_cast<int>(desc.BufferCount);
        info.RTVFormat = desc.BufferDesc.Format;
        info.SrvDescriptorHeap = srv_heap_.Get();
        info.UserData = this;
        info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* init,
            D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
            const auto* self = static_cast<D3D12Renderer*>(init->UserData);
            *cpu = self->srv_heap_->GetCPUDescriptorHandleForHeapStart();
            *gpu = self->srv_heap_->GetGPUDescriptorHandleForHeapStart();
        };
        info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE,
            D3D12_GPU_DESCRIPTOR_HANDLE) {};
        if (!ImGui_ImplDX12_Init(&info)) {
            error = "Initializing the ImGui D3D12 renderer failed.";
            return false;
        }
        backend_initialized_ = true;
        gpu_work_submitted_ = true; // Font creation submits an upload on the host queue.
        if (!ImGui_ImplDX12_CreateDeviceObjects()) {
            error = "Creating ImGui D3D12 font and pipeline objects failed.";
            return false;
        }
        hr = device_->GetDeviceRemovedReason();
        if (FAILED(hr)) return failed(error, "Create D3D12 overlay device objects", hr);
        return true;
    }

    gui::TextureHost* textures() noexcept override { return this; }

    // Copies through the captured queue. Later overlay frames are submitted to the
    // same queue, so they always sample the finished copy; nothing waits here.
    ImTextureID create(const std::uint8_t* rgba, int width, int height) override {
        if (!device_ || !queue_ || !rgba || width <= 0 || height <= 0) return 0;
        collect_retired();
        UINT slot = 0;
        for (UINT i = 1; i < srv_slots && !slot; ++i) if (!slot_used_[i]) slot = i;
        if (!slot) return 0;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(width);
        desc.Height = static_cast<UINT>(height);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> texture;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(texture.GetAddressOf()))))
            return 0;

        const UINT row = static_cast<UINT>(width) * 4;
        const UINT pitch = (row + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = static_cast<UINT64>(pitch) * static_cast<UINT64>(height);
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> upload;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr, IID_PPV_ARGS(upload.GetAddressOf()))))
            return 0;
        void* mapped = nullptr;
        const D3D12_RANGE nothing_read{0, 0};
        if (FAILED(upload->Map(0, &nothing_read, &mapped))) return 0;
        for (int y = 0; y < height; ++y)
            std::memcpy(static_cast<std::uint8_t*>(mapped) + static_cast<std::size_t>(y) * pitch,
                        rgba + static_cast<std::size_t>(y) * row, row);
        upload->Unmap(0, nullptr);

        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf()))) ||
            FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                              IID_PPV_ARGS(list.GetAddressOf()))))
            return 0;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = texture.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, static_cast<UINT>(width), static_cast<UINT>(height), 1, pitch};
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &barrier);
        if (FAILED(list->Close())) return 0;
        ID3D12CommandList* lists[]{list.Get()};
        gpu_work_submitted_ = true;
        queue_->ExecuteCommandLists(1, lists);
        const auto value = ++next_fence_;
        if (FAILED(queue_->Signal(fence_.Get(), value))) return 0;
        // The staging copy and its command list are freed once the copy has run.
        retired_.push_back({std::move(upload), std::move(allocator), std::move(list), 0, value});

        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(texture.Get(), &view, cpu_slot(slot));
        slot_used_[slot] = true;
        textures_.push_back({std::move(texture), slot});
        return static_cast<ImTextureID>(gpu_slot(slot).ptr);
    }

    void release(ImTextureID id) override {
        const auto found = std::find_if(textures_.begin(), textures_.end(),
                                        [&](const Texture& texture) { return gpu_slot(texture.slot).ptr == id; });
        if (found == textures_.end()) return;
        // Frames up to the last signal may still sample it.
        retired_.push_back({std::move(found->resource), {}, {}, found->slot, next_fence_});
        textures_.erase(found);
    }

    bool rebuild_fonts(std::string& error) override {
        if (!backend_initialized_) {
            error = "The D3D12 overlay renderer is not initialized.";
            return false;
        }
        if (!wait_idle(error)) return false;
        if (effects_) effects_->invalidate_fonts();
        ContextScope scope(context_);
        ImGui_ImplDX12_InvalidateDeviceObjects();
        if (!ImGui_ImplDX12_CreateDeviceObjects()) {
            error = "Rebuilding ImGui D3D12 fonts failed.";
            return false;
        }
        const HRESULT hr = device_->GetDeviceRemovedReason();
        if (FAILED(hr)) return failed(error, "Rebuild D3D12 overlay fonts", hr);
        return true;
    }

    bool render(ImDrawData* data, std::string& error) override {
        if (!backend_initialized_) {
            error = "The D3D12 overlay renderer is not initialized.";
            return false;
        }
        if (!data || data->DisplaySize.x <= 0.0f || data->DisplaySize.y <= 0.0f) return true;
        const auto* edge_frame = gui::current_edge_lit_frame();
        if (data->CmdListsCount == 0 && (!edge_frame || edge_frame->height <= 0.0F)) return true;
        const UINT index = swap_->GetCurrentBackBufferIndex();
        if (index >= frames_.size()) {
            error = "The D3D12 swap-chain back-buffer index changed; recreate the renderer after resizing.";
            return false;
        }
        auto& frame = frames_[index];
        // Never block the game's Present thread. A previous overlay submission
        // that is still using this buffer or ImGui upload slot is skipped for
        // one frame; waiting here serializes the game to the overlay GPU time
        // and is what makes opening the ClickGUI hitch.
        const auto frame_ready = poll_fence(frame.fence_value, error);
        if (frame_ready == FencePoll::lost) return false;
        const auto backend_slot = static_cast<std::size_t>(render_count_ % backend_fences_.size());
        const auto backend_ready = poll_fence(backend_fences_[backend_slot], error);
        if (backend_ready == FencePoll::lost) return false;
        if (frame_ready == FencePoll::pending || backend_ready == FencePoll::pending) {
            ++counters_.skipped_frames;
            return true;
        }
        if (!retired_.empty()) collect_retired();
        if ((edge_frame && edge_frame->height > 0) || gui::current_liquid_glass() || !effects_ || !effects_->loaded()) {
            const auto effects_start = perf_now();
            if (!ensure_effects(error)) return false;
            ID3D11Resource* wrapped = frame.wrapped.Get();
            interop_->AcquireWrappedResources(&wrapped, 1);
            const bool rendered = effects_->render(interop_device_.Get(), interop_context_.Get(),
                frame.wrapped.Get(), frame.wrapped_target.Get(), error);
            interop_->ReleaseWrappedResources(&wrapped, 1);
            // Both APIs submit to the captured direct queue. Release emits the
            // PRESENT transition; Flush orders it before the ImGui command list.
            interop_context_->Flush();
            gpu_work_submitted_ = true;
            count_effects(perf_now() - effects_start);
            if (!rendered) return false;
        }
        HRESULT hr = frame.allocator->Reset();
        if (FAILED(hr)) return failed(error, "Reset D3D12 overlay command allocator", hr);
        hr = commands_->Reset(frame.allocator.Get(), nullptr);
        if (FAILED(hr)) return failed(error, "Reset D3D12 overlay command list", hr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = frame.back_buffer.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commands_->ResourceBarrier(1, &barrier);
        commands_->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[]{srv_heap_.Get()};
        commands_->SetDescriptorHeaps(1, heaps);
        {
            ContextScope scope(context_);
            ImGui_ImplDX12_RenderDrawData(data, commands_.Get());
        }
        ++render_count_;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        commands_->ResourceBarrier(1, &barrier);
        hr = commands_->Close();
        if (FAILED(hr)) return failed(error, "Close D3D12 overlay command list", hr);

        ID3D12CommandList* lists[]{commands_.Get()};
        gpu_work_submitted_ = true;
        queue_->ExecuteCommandLists(1, lists);
        const auto value = ++next_fence_;
        hr = queue_->Signal(fence_.Get(), value);
        if (FAILED(hr)) return failed(error, "Signal D3D12 overlay completion", hr);
        frame.fence_value = value;
        backend_fences_[backend_slot] = value;
        return true;
    }

    bool wait_idle(std::string& error) override {
        if (!queue_ || !fence_ || !gpu_work_submitted_) return true;
        const auto value = ++next_fence_;
        const HRESULT hr = queue_->Signal(fence_.Get(), value);
        if (FAILED(hr)) return failed(error, "Signal D3D12 overlay idle fence", hr);
        return wait_for(value, error);
    }

private:
    struct Frame {
        ComPtr<ID3D12Resource> back_buffer;
        ComPtr<ID3D11Texture2D> wrapped;
        ComPtr<ID3D11RenderTargetView> wrapped_target;
        ComPtr<ID3D12CommandAllocator> allocator;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        std::uint64_t fence_value{};
    };

    bool ensure_effects(std::string& error) {
        if (effects_) return true;
        const HMODULE library = GetModuleHandleW(L"d3d11.dll");
        if (!library) { error = "Edge-lit could not locate the D3D11 runtime for D3D12 interop."; return false; }
        const auto create = reinterpret_cast<decltype(&D3D11On12CreateDevice)>(GetProcAddress(library, "D3D11On12CreateDevice"));
        if (!create) { error = "Edge-lit requires the Windows D3D11On12 runtime for D3D12 Gaussian effects."; return false; }
        IUnknown* queues[]{queue_.Get()};
        HRESULT hr = create(device_.Get(), D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
            queues, 1, 0, interop_device_.ReleaseAndGetAddressOf(), interop_context_.ReleaseAndGetAddressOf(), nullptr);
        if (FAILED(hr)) return failed(error, "Create Edge-lit D3D11On12 device", hr);
        hr = interop_device_.As(&interop_);
        if (FAILED(hr)) return failed(error, "Query Edge-lit D3D11On12 interface", hr);
        D3D11_RESOURCE_FLAGS flags{};
        flags.BindFlags = D3D11_BIND_RENDER_TARGET;
        for (auto& frame : frames_) {
            hr = interop_->CreateWrappedResource(frame.back_buffer.Get(), &flags,
                D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT,
                IID_PPV_ARGS(frame.wrapped.ReleaseAndGetAddressOf()));
            if (FAILED(hr)) return failed(error, "Wrap D3D12 Edge-lit back buffer", hr);
            hr = interop_device_->CreateRenderTargetView(frame.wrapped.Get(), nullptr,
                frame.wrapped_target.ReleaseAndGetAddressOf());
            if (FAILED(hr)) return failed(error, "Create D3D12 Edge-lit effect target", hr);
        }
        effects_ = std::make_unique<gui::EdgeLitD3D11>();
        return true;
    }

    struct Texture {
        ComPtr<ID3D12Resource> resource;
        UINT slot = 0;
    };
    // Kept until the GPU passed `fence`: a released texture, or an upload's
    // staging buffer and command list. slot 0 means no descriptor to free.
    struct Retired {
        ComPtr<ID3D12Resource> resource;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        UINT slot = 0;
        std::uint64_t fence = 0;
    };

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_slot(UINT slot) const {
        auto handle = srv_heap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot) * srv_increment_;
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_slot(UINT slot) const {
        auto handle = srv_heap_->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(slot) * srv_increment_;
        return handle;
    }

    void collect_retired() {
        if (!fence_) return;
        const auto completed = fence_->GetCompletedValue();
        if (completed == std::numeric_limits<std::uint64_t>::max()) return; // device removed; keep everything
        std::erase_if(retired_, [&](const Retired& entry) {
            if (entry.fence > completed) return false;
            if (entry.slot) slot_used_[entry.slot] = false;
            return true;
        });
    }

    enum class FencePoll { ready, pending, lost };

    FencePoll poll_fence(std::uint64_t value, std::string& error) const {
        if (!value) return FencePoll::ready;
        const auto completed = fence_->GetCompletedValue();
        if (completed == std::numeric_limits<std::uint64_t>::max()) {
            const HRESULT removed = device_->GetDeviceRemovedReason();
            failed(error, "Wait for D3D12 overlay GPU work", FAILED(removed) ? removed : DXGI_ERROR_DEVICE_REMOVED);
            return FencePoll::lost;
        }
        return completed >= value ? FencePoll::ready : FencePoll::pending;
    }

    bool wait_for(std::uint64_t value, std::string& error) {
        if (!value) return true;
        const auto started = GetTickCount64();
        bool event_registered = false;
        while (true) {
            const auto state = poll_fence(value, error);
            if (state == FencePoll::ready) return true;
            if (state == FencePoll::lost) return false;
            const HRESULT device_status = device_->GetDeviceRemovedReason();
            if (FAILED(device_status)) return failed(error, "Wait for D3D12 overlay GPU work", device_status);
            if (GetTickCount64() - started >= 5000) {
                error = "Timed out after 5 seconds waiting for D3D12 overlay GPU work.";
                return false;
            }
            // Shutdown and font rebuild may wait. Present must not: render() polls
            // and skips a frame instead of blocking the game.
            if (!event_registered) {
                const HRESULT hr = fence_->SetEventOnCompletion(value, fence_event_);
                if (FAILED(hr)) return failed(error, "Set D3D12 overlay fence completion event", hr);
                event_registered = true;
            }
            const DWORD result = WaitForSingleObject(fence_event_, 100);
            if (result == WAIT_FAILED) return failed(error, "Wait for D3D12 overlay fence event", HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    void abandon_in_flight_resources() noexcept {
        if (backend_initialized_) {
            ContextScope scope(context_);
            auto& io = ImGui::GetIO();
            // Keep the backend's GPU allocations alive too. Detach its state from
            // this context so shutdown/retry cannot accidentally release them.
            io.BackendRendererUserData = nullptr;
            io.BackendRendererName = nullptr;
            io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
            io.Fonts->SetTexID(0);
        }
        for (auto& frame : frames_) {
            (void)frame.wrapped_target.Detach();
            (void)frame.wrapped.Detach();
            (void)frame.back_buffer.Detach();
            (void)frame.allocator.Detach();
        }
        for (auto& texture : textures_) (void)texture.resource.Detach();
        for (auto& entry : retired_) {
            (void)entry.resource.Detach();
            (void)entry.allocator.Detach();
            (void)entry.list.Detach();
        }
        (void)effects_.release();
        (void)interop_.Detach();
        (void)interop_context_.Detach();
        (void)interop_device_.Detach();
        (void)commands_.Detach();
        (void)rtv_heap_.Detach();
        (void)srv_heap_.Detach();
        (void)fence_.Detach();
        (void)queue_.Detach();
        (void)swap_.Detach();
        (void)device_.Detach();
        fence_event_ = nullptr;
    }

    ImGuiContext* context_{};
    std::unique_ptr<gui::EdgeLitD3D11> effects_;
    ComPtr<ID3D11Device> interop_device_;
    ComPtr<ID3D11DeviceContext> interop_context_;
    ComPtr<ID3D11On12Device> interop_;
    ComPtr<ID3D12Device> device_;
    ComPtr<IDXGISwapChain3> swap_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12DescriptorHeap> srv_heap_;
    ComPtr<ID3D12GraphicsCommandList> commands_;
    ComPtr<ID3D12Fence> fence_;
    std::vector<Frame> frames_;
    std::vector<std::uint64_t> backend_fences_;
    std::vector<Texture> textures_;
    std::vector<Retired> retired_;
    std::array<bool, srv_slots> slot_used_{}; // slot 0 belongs to the ImGui backend
    UINT srv_increment_ = 0;
    HANDLE fence_event_{};
    std::uint64_t next_fence_{};
    std::uint64_t render_count_{};
    bool backend_initialized_{};
    bool gpu_work_submitted_{};
};
} // namespace

std::unique_ptr<OverlayRenderer> make_d3d12_renderer() {
    return std::make_unique<D3D12Renderer>();
}
} // namespace medicon::platform
