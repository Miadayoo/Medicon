#include "OverlayRenderer.hpp"
#include "medicon/gui/EdgeLit.hpp"

#include <d3d11_1.h>
#include <wrl/client.h>
#include <backends/imgui_impl_dx11.h>

#include <array>
#include <cstdio>

namespace medicon::platform {
namespace {

using Microsoft::WRL::ComPtr;

std::string describe_failure(const char* operation, HRESULT result) {
    std::array<char, 160> message{};
    std::snprintf(message.data(), message.size(), "%s failed (HRESULT 0x%08lX).",
                  operation, static_cast<unsigned long>(result));
    return message.data();
}

// The ImGui backend restores its own state changes, except for the hull, domain,
// and compute shader stages. It does not bind or restore output targets either.
// Keep those additional pieces of the game's pipeline intact around the overlay.
class HostPipelineState final {
public:
    HostPipelineState(ID3D11Device* device, ID3D11DeviceContext* context) : context_(context) {
        context_->OMGetRenderTargets(static_cast<UINT>(render_targets_.size()),
                                     render_targets_.data(), depth_target_.GetAddressOf());
        for (UINT i = 0; i < render_targets_.size(); ++i) {
            if (render_targets_[i]) render_target_count_ = i + 1;
        }
        const auto feature_level = device->GetFeatureLevel();
        if (feature_level >= D3D_FEATURE_LEVEL_11_0) {
            uav_slot_count_ = feature_level >= D3D_FEATURE_LEVEL_11_1
                ? D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;
            if (uav_slot_count_ > render_target_count_) {
                context_->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr,
                    render_target_count_, uav_slot_count_ - render_target_count_, unordered_views_.data());
            }
        }
        context_->HSGetShader(hull_shader_.GetAddressOf(), hull_instances_.data(), &hull_count_);
        context_->DSGetShader(domain_shader_.GetAddressOf(), domain_instances_.data(), &domain_count_);
        context_->CSGetShader(compute_shader_.GetAddressOf(), compute_instances_.data(), &compute_count_);
    }

    ~HostPipelineState() {
        if (uav_slot_count_ > render_target_count_) {
            std::array<UINT, D3D11_1_UAV_SLOT_COUNT> preserved_counts;
            preserved_counts.fill(static_cast<UINT>(-1));
            context_->OMSetRenderTargetsAndUnorderedAccessViews(render_target_count_,
                render_targets_.data(), depth_target_.Get(), render_target_count_,
                uav_slot_count_ - render_target_count_, unordered_views_.data(), preserved_counts.data());
        } else {
            context_->OMSetRenderTargets(render_target_count_, render_targets_.data(), depth_target_.Get());
        }
        context_->HSSetShader(hull_shader_.Get(), hull_instances_.data(), hull_count_);
        context_->DSSetShader(domain_shader_.Get(), domain_instances_.data(), domain_count_);
        context_->CSSetShader(compute_shader_.Get(), compute_instances_.data(), compute_count_);
        for (auto* target : render_targets_) if (target) target->Release();
        for (auto* view : unordered_views_) if (view) view->Release();
        for (UINT i = 0; i < hull_count_; ++i) if (hull_instances_[i]) hull_instances_[i]->Release();
        for (UINT i = 0; i < domain_count_; ++i) if (domain_instances_[i]) domain_instances_[i]->Release();
        for (UINT i = 0; i < compute_count_; ++i) if (compute_instances_[i]) compute_instances_[i]->Release();
    }

    HostPipelineState(const HostPipelineState&) = delete;
    HostPipelineState& operator=(const HostPipelineState&) = delete;

private:
    ID3D11DeviceContext* context_;
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> render_targets_{};
    std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> unordered_views_{};
    UINT render_target_count_ = 0;
    UINT uav_slot_count_ = 0;
    ComPtr<ID3D11DepthStencilView> depth_target_;
    ComPtr<ID3D11HullShader> hull_shader_;
    ComPtr<ID3D11DomainShader> domain_shader_;
    ComPtr<ID3D11ComputeShader> compute_shader_;
    std::array<ID3D11ClassInstance*, 256> hull_instances_{};
    std::array<ID3D11ClassInstance*, 256> domain_instances_{};
    std::array<ID3D11ClassInstance*, 256> compute_instances_{};
    UINT hull_count_ = 256;
    UINT domain_count_ = 256;
    UINT compute_count_ = 256;
};

class D3D11Renderer final : public OverlayRenderer {
public:
    ~D3D11Renderer() override {
        textures_.reset();
        effects_.reset();
        gui::forget_edge_lit_frame(imgui_context_);
        if (backend_initialized_) ImGui_ImplDX11_Shutdown();
        render_target_.Reset();
        back_buffer_.Reset();
        // D3D11 owns the lifetime of objects referenced by submitted commands.
        // Flush after dropping our views to process their deferred destruction;
        // no CPU fence wait is needed before DXGI resizes the released buffers.
        if (context_) context_->Flush();
    }

    bool initialize(IDXGISwapChain* swap, ID3D12CommandQueue*, std::string& error) override {
        if (!swap || !ImGui::GetCurrentContext()) {
            error = "D3D11 overlay requires a swap chain and an ImGui context.";
            return false;
        }
        if (backend_initialized_) {
            error = "D3D11 overlay renderer is already initialized.";
            return false;
        }
        HRESULT result = swap->GetDevice(IID_PPV_ARGS(device_.GetAddressOf()));
        if (FAILED(result)) {
            error = describe_failure("IDXGISwapChain::GetDevice(D3D11)", result);
            return false;
        }
        device_->GetImmediateContext(context_.GetAddressOf());
        if (!context_) {
            error = "D3D11 device did not provide an immediate context.";
            return false;
        }
        swap_ = swap;
        imgui_context_ = ImGui::GetCurrentContext();
        if (!update_render_target(error)) return false;
        if (!ImGui_ImplDX11_Init(device_.Get(), context_.Get())) {
            error = "Could not initialize the ImGui D3D11 renderer.";
            return false;
        }
        backend_initialized_ = true;
        textures_ = std::make_unique<gui::D3D11TextureHost>(device_.Get());
        return rebuild_fonts(error);
    }

    gui::TextureHost* textures() noexcept override { return textures_.get(); }

    bool rebuild_fonts(std::string& error) override {
        if (!backend_initialized_) {
            error = "D3D11 overlay renderer is not initialized.";
            return false;
        }
        ImGui_ImplDX11_InvalidateDeviceObjects();
        effects_.invalidate_fonts();
        if (!ImGui_ImplDX11_CreateDeviceObjects()) {
            error = "Could not create the ImGui D3D11 font texture and pipeline.";
            return false;
        }
        return true;
    }

    bool render(ImDrawData* data, std::string& error) override {
        if (!backend_initialized_) {
            error = "D3D11 overlay renderer is not initialized.";
            return false;
        }
        if (!data || data->DisplaySize.x <= 0.0F || data->DisplaySize.y <= 0.0F) return true;
        const auto* edge_frame = gui::current_edge_lit_frame();
        if (data->CmdListsCount == 0 && (!edge_frame || edge_frame->height <= 0.0F)) return true;
        if (!update_render_target(error)) return false;
        const auto effects_start = perf_now();
        if (!effects_.render(device_.Get(), context_.Get(), back_buffer_.Get(), render_target_.Get(), error)) return false;
        if ((edge_frame && edge_frame->height > 0) || gui::current_liquid_glass()) count_effects(perf_now() - effects_start);
        if (data->CmdListsCount > 0) {
            HostPipelineState host_state(device_.Get(), context_.Get());
            ID3D11RenderTargetView* target = render_target_.Get();
            context_->OMSetRenderTargets(1, &target, nullptr);
            // Draw on the host's completed frame without clearing its contents.
            ImGui_ImplDX11_RenderDrawData(data);
        }
        const HRESULT result = device_->GetDeviceRemovedReason();
        if (FAILED(result)) {
            error = describe_failure("D3D11 device during overlay rendering", result);
            return false;
        }
        return true;
    }

    bool wait_idle(std::string& error) override {
        if (!context_ || !device_) return true;
        // Resize/shutdown only need to submit our immediate-context work and
        // release the overlay's COM references. D3D11 defers GPU destruction.
        // Waiting on an EVENT query here can stall presentation on some drivers.
        context_->Flush();
        const HRESULT removed = device_->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            error = describe_failure("D3D11 device while flushing overlay", removed);
            return false;
        }
        return true;
    }

private:
    bool update_render_target(std::string& error) {
        ComPtr<ID3D11Texture2D> buffer;
        const HRESULT result = swap_->GetBuffer(0, IID_PPV_ARGS(buffer.GetAddressOf()));
        if (FAILED(result)) {
            error = describe_failure("IDXGISwapChain::GetBuffer(D3D11)", result);
            return false;
        }
        // D3D11 rotates flip-model buffers through the zero-index buffer view.
        // Reacquire the texture every Present, but reuse the view for a buffer
        // we have already wrapped. A new view every frame is a driver stall on
        // the same thread that is trying to present the menu and the intro.
        for (int i = 0; i < cached_count_; ++i) {
            if (cached_targets_[static_cast<std::size_t>(i)].texture == buffer.Get()) {
                back_buffer_ = buffer;
                render_target_ = cached_targets_[static_cast<std::size_t>(i)].view;
                return true;
            }
        }
        ComPtr<ID3D11RenderTargetView> created_view;
        const HRESULT created = device_->CreateRenderTargetView(buffer.Get(), nullptr, created_view.GetAddressOf());
        if (FAILED(created)) {
            error = describe_failure("ID3D11Device::CreateRenderTargetView", created);
            return false;
        }
        if (cached_count_ == static_cast<int>(cached_targets_.size())) {
            for (int i = 1; i < cached_count_; ++i) cached_targets_[static_cast<std::size_t>(i - 1)] = cached_targets_[static_cast<std::size_t>(i)];
            --cached_count_;
        }
        cached_targets_[static_cast<std::size_t>(cached_count_++)] = {buffer.Get(), created_view};
        back_buffer_ = buffer;
        render_target_ = created_view;
        return true;
    }

    ComPtr<IDXGISwapChain> swap_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    struct CachedTarget {
        ID3D11Texture2D* texture = nullptr;
        ComPtr<ID3D11RenderTargetView> view;
    };

    ComPtr<ID3D11Texture2D> back_buffer_;
    ComPtr<ID3D11RenderTargetView> render_target_;
    // Flip-model GetBuffer(0) returns a different texture every Present. Creating
    // the view each time stalls the game thread; the swap chain only rotates a
    // few buffers, so keep one view per texture.
    std::array<CachedTarget, 4> cached_targets_{};
    int cached_count_ = 0;
    gui::EdgeLitD3D11 effects_;
    std::unique_ptr<gui::D3D11TextureHost> textures_;
    ImGuiContext* imgui_context_ = nullptr;
    bool backend_initialized_ = false;
};

} // namespace

std::unique_ptr<OverlayRenderer> make_d3d11_renderer() {
    return std::make_unique<D3D11Renderer>();
}

} // namespace medicon::platform
