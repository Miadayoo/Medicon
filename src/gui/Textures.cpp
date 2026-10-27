#include <medicon/gui/Textures.hpp>

#include <d3d11.h>

#include <algorithm>

namespace medicon::gui {

D3D11TextureHost::~D3D11TextureHost() {
    for (auto* view : views_) view->Release();
}

ImTextureID D3D11TextureHost::create(const std::uint8_t* rgba, int width, int height) {
    if (!device_ || !rgba || width <= 0 || height <= 0) return 0;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{rgba, static_cast<UINT>(width) * 4, 0};
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(device_->CreateTexture2D(&desc, &data, &texture))) return 0;
    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{};
    view_desc.Format = desc.Format;
    view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* view = nullptr;
    const HRESULT result = device_->CreateShaderResourceView(texture, &view_desc, &view);
    texture->Release(); // the view holds the texture
    if (FAILED(result)) return 0;
    views_.push_back(view);
    return reinterpret_cast<ImTextureID>(view);
}

void D3D11TextureHost::release(ImTextureID texture) {
    auto* view = reinterpret_cast<ID3D11ShaderResourceView*>(texture);
    const auto found = std::find(views_.begin(), views_.end(), view);
    if (found == views_.end()) return;
    views_.erase(found);
    view->Release();
}

} // namespace medicon::gui
