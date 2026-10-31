#include <medicon/gui/EdgeLit.hpp>
#include <medicon/gui/LiquidGlass.hpp>

#include <d3d11on12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// The pane drawn by real shaders, on the two routes the client has: plain D3D11,
// and D3D11On12 over a wrapped back buffer, which is how the game presents.

namespace {

using Microsoft::WRL::ComPtr;
using medicon::gui::LiquidGlassD3D11;
using medicon::gui::LiquidGlassFrame;

constexpr UINT width = 512, height = 256, stripe = 16;
constexpr std::uint8_t light = 200, dark = 60;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void succeed(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + " failed");
}

constexpr int scene_at(UINT x) { return (x / stripe) % 2 ? light : dark; }

// A neutral scene: any colour in the result was made by the glass.
std::vector<std::uint32_t> stripes() {
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const auto v = static_cast<std::uint32_t>(scene_at(x));
            pixels[static_cast<std::size_t>(y) * width + x] = 0xFF000000U | v << 16 | v << 8 | v;
        }
    }
    return pixels;
}

struct Target {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11Texture2D* texture;
    std::function<void()> acquire = [] {};
    std::function<void()> release = [] {};
};

struct Image {
    std::vector<std::uint32_t> pixels;
    bool bgra = false;
    // Red, green, blue.
    [[nodiscard]] std::array<int, 3> at(UINT x, UINT y) const {
        const auto value = pixels[static_cast<std::size_t>(y) * width + x];
        const int first = static_cast<int>(value & 255U), third = static_cast<int>(value >> 16 & 255U);
        return {bgra ? third : first, static_cast<int>(value >> 8 & 255U), bgra ? first : third};
    }
};

Image render_once(const Target& target, LiquidGlassD3D11& glass, ID3D11RenderTargetView* view,
                  const LiquidGlassFrame& frame, ID3D11Texture2D* staging) {
    const auto scene = stripes();
    target.acquire();
    target.context->UpdateSubresource(target.texture, 0, nullptr, scene.data(), width * 4, 0);
    std::string error;
    const bool drawn = glass.render(target.device, target.context, target.texture, view, frame, error);
    if (drawn) target.context->CopyResource(staging, target.texture);
    target.release();
    target.context->Flush();
    check(drawn, error);
    D3D11_MAPPED_SUBRESOURCE map{};
    succeed(target.context->Map(staging, 0, D3D11_MAP_READ, 0, &map), "Map result");
    D3D11_TEXTURE2D_DESC desc{};
    staging->GetDesc(&desc);
    Image image;
    image.bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    image.pixels.resize(scene.size());
    for (UINT y = 0; y < height; ++y)
        std::memcpy(image.pixels.data() + static_cast<std::size_t>(y) * width,
                    static_cast<const std::uint8_t*>(map.pData) + static_cast<std::size_t>(y) * map.RowPitch, width * 4);
    target.context->Unmap(staging, 0);
    return image;
}

void verify(const Target& target) {
    ComPtr<ID3D11RenderTargetView> view;
    succeed(target.device->CreateRenderTargetView(target.texture, nullptr, view.GetAddressOf()), "Create target view");
    D3D11_TEXTURE2D_DESC desc{};
    target.texture->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    succeed(target.device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()), "Create staging texture");

    LiquidGlassFrame frame;
    frame.center = {256.0F, 128.0F};
    frame.size = {160.0F, 64.0F};
    frame.light = -2.19F;

    // With nothing to show, the first effects pass still loads the glass.
    medicon::gui::EdgeLitD3D11 effects;
    check(!effects.loaded(), "Effects loaded before their first frame");
    target.acquire();
    std::string error;
    check(effects.render(target.device, target.context, target.texture, view.Get(), error), error);
    target.release();
    check(effects.loaded(), "The first effects pass did not load Liquid Glass");

    LiquidGlassD3D11 glass;
    const Image image = render_once(target, glass, view.Get(), frame, staging.Get());
    check(glass.loaded(), "A drawn pane is not loaded");
    check(image.at(256, 128)[1] != scene_at(256), "The pane did not draw on its first frame");

    // Nothing outside the pane and its shadow changes.
    check(image.at(4, 4)[0] == scene_at(4) && image.at(4 + stripe, 4)[0] == scene_at(4 + stripe),
          "The scene outside the pane changed");

    // The flat middle is clear: the edges of the scene stay one pixel wide, and
    // the stripes keep their contrast through the tint.
    int low = 255, high = 0, steepest = 0;
    for (UINT x = 232; x < 280; ++x) {
        low = std::min(low, image.at(x, 128)[1]);
        high = std::max(high, image.at(x, 128)[1]);
        steepest = std::max(steepest, std::abs(image.at(x, 128)[1] - image.at(x - 1, 128)[1]));
    }
    check(high * dark > low * light * 9 / 10, "The middle of the pane lost the contrast of the scene");
    check(steepest > (high - low) * 9 / 10, "The middle of the pane is not pixel-sharp");

    // Refraction: the bezel shows the stripes squeezed, so one row near the top
    // edge crosses more of them than the same width of the flat middle does.
    const auto crossings = [&](UINT row) {
        int count = 0;
        const auto mean = (light + dark) / 2;
        for (UINT x = 201; x < 311; ++x)
            if ((image.at(x - 1, row)[1] < mean) != (image.at(x, row)[1] < mean)) ++count;
        return count;
    };
    check(crossings(101) >= 4, "The rim does not show the scene through it");

    // Prism: the tint is blue, so on a neutral scene only displaced channels can
    // turn a pixel red. That must happen around the rim, and only there.
    int split = 0;
    for (UINT y = 96; y < 160; ++y)
        for (UINT x = 176; x < 336; ++x)
            if (const auto c = image.at(x, y); c[0] > c[2] + 14) ++split;
    check(split > 150, "The rim has no prismatic colour: " + std::to_string(split));
    for (UINT x = 232; x < 280; ++x) {
        const auto c = image.at(x, 128);
        check(c[0] <= c[2], "The flat middle split its channels");
    }

    // The pane lays a shadow on the scene just below it.
    check(scene_at(280) == light && image.at(280, 168)[0] < light - 12, "The pane has no shadow");

    // Fully dissolved, nothing of it is left on the scene.
    frame.lens = 0.0F;
    frame.opacity = 0.0F;
    const Image gone = render_once(target, glass, view.Get(), frame, staging.Get());
    check(gone.at(256, 128)[1] == scene_at(256) && gone.at(280, 168)[0] == light, "A dissolved pane still covered the scene");
}

void d3d11_route() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const auto create = [&](D3D_DRIVER_TYPE type) {
        return D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                 device.GetAddressOf(), nullptr, context.GetAddressOf());
    };
    if (FAILED(create(D3D_DRIVER_TYPE_HARDWARE))) succeed(create(D3D_DRIVER_TYPE_WARP), "Create D3D11 device");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture;
    succeed(device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()), "Create D3D11 target");
    verify({device.Get(), context.Get(), texture.Get()});
}

void d3d11on12_route() {
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf())))) {
        ComPtr<IDXGIFactory4> factory;
        succeed(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())), "Create DXGI factory");
        ComPtr<IDXGIAdapter> warp;
        succeed(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.GetAddressOf())), "Find WARP");
        succeed(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf())), "Create D3D12 device");
    }
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    succeed(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.GetAddressOf())), "Create D3D12 queue");

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> back_buffer;
    succeed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                                            IID_PPV_ARGS(back_buffer.GetAddressOf())), "Create back buffer");

    ComPtr<ID3D11Device> interop_device;
    ComPtr<ID3D11DeviceContext> interop_context;
    IUnknown* queues[]{queue.Get()};
    succeed(D3D11On12CreateDevice(device.Get(), D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, queues, 1, 0,
                                  interop_device.GetAddressOf(), interop_context.GetAddressOf(), nullptr), "Create D3D11On12 device");
    ComPtr<ID3D11On12Device> interop;
    succeed(interop_device.As(&interop), "Query D3D11On12");
    D3D11_RESOURCE_FLAGS flags{};
    flags.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> wrapped;
    succeed(interop->CreateWrappedResource(back_buffer.Get(), &flags, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT,
                                           IID_PPV_ARGS(wrapped.GetAddressOf())), "Wrap back buffer");
    ID3D11Resource* resource = wrapped.Get();
    verify({interop_device.Get(), interop_context.Get(), wrapped.Get(),
            [&] { interop->AcquireWrappedResources(&resource, 1); },
            [&] { interop->ReleaseWrappedResources(&resource, 1); }});
}

} // namespace

int main() {
    const std::pair<const char*, void (*)()> routes[]{{"D3D11", d3d11_route}, {"D3D11On12", d3d11on12_route}};
    int failures = 0;
    for (const auto& [name, route] : routes) {
        try {
            route();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
