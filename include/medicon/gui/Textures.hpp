#pragma once

#include <imgui.h>

#include <cstdint>
#include <vector>

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace medicon::gui {

// RGBA8 textures the GUI creates for itself beyond the ImGui font atlas (the
// Social background and font atlas). Each host renderer implements it. Render
// thread only. Textures die with the host: the GUI drops its ids when the host
// is replaced (see Overlay::set_texture_host) instead of releasing them.
class TextureHost {
public:
    virtual ~TextureHost() = default;
    // Returns 0 when the texture cannot be created.
    virtual ImTextureID create(const std::uint8_t* rgba, int width, int height) = 0;
    // Frames already submitted may still sample it; the host frees it after them.
    virtual void release(ImTextureID texture) = 0;
};

// D3D11: the DLL's D3D11 renderer and the Preview. ImTextureID is the SRV,
// which is what the ImGui D3D11 backend binds.
class D3D11TextureHost final : public TextureHost {
public:
    explicit D3D11TextureHost(ID3D11Device* device) : device_(device) {}
    ~D3D11TextureHost() override;
    D3D11TextureHost(const D3D11TextureHost&) = delete;
    D3D11TextureHost& operator=(const D3D11TextureHost&) = delete;

    ImTextureID create(const std::uint8_t* rgba, int width, int height) override;
    // D3D11 keeps a resource alive while queued commands reference it.
    void release(ImTextureID texture) override;

private:
    ID3D11Device* device_;
    std::vector<ID3D11ShaderResourceView*> views_;
};

} // namespace medicon::gui
