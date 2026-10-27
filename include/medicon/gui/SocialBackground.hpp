#pragma once

#include <imgui.h>

#include <cstdint>
#include <future>
#include <memory>
#include <span>
#include <vector>

namespace medicon::gui {

class TextureHost;

// back.png in the Social screen, in two renders that share one decoded image:
// - the backdrop (spec 4.6): covering the window, sigma = 8 x height / 1080;
// - the panel picture: covering the content area right of the sidebar, only
//   lightly blurred.
// Each is scaled to cover its size, cropped and Gaussian-blurred on a worker
// thread, once per size; the render thread only uploads the result and draws
// one textured quad. Render thread only.
class SocialBackground {
public:
    // The back.png embedded in the binary, like the other bundled assets.
    SocialBackground();
    // Another encoded image (tests). `png` must outlive this object.
    explicit SocialBackground(std::span<const std::uint8_t> png);
    ~SocialBackground();
    SocialBackground(const SocialBackground&) = delete;
    SocialBackground& operator=(const SocialBackground&) = delete;

    // Starts or finishes the backdrop for a window of `size`; uploads through `host` (may be null).
    void update(ImVec2 size, TextureHost* host);
    // The same for the panel picture: `size` in px, blurred with `sigma` px.
    void update_panel(ImVec2 size, float sigma, TextureHost* host);
    void forget_texture() noexcept;
    bool ready(ImVec2 size) const noexcept;
    bool panel_ready(ImVec2 size) const noexcept;
    // The image could not be decoded; only the gradient and veil are drawn.
    bool failed() const noexcept { return failed_; }
    // The blurred image, partly transparent over the Main Menu gradient
    // (stretched while a resize is being processed), then a milky veil.
    void draw(ImDrawList* list, ImVec2 size, float alpha) const;
    // The panel picture in [min, max] with the given rounded corners, then a
    // light veil that keeps text on it readable. Nothing until it exists.
    void draw_panel(ImDrawList* list, ImVec2 min, ImVec2 max, float rounding, ImDrawFlags corners, float alpha) const;

    struct Image {
        std::vector<std::uint8_t> rgba;
        int width = 0, height = 0;
    };
    // The blur itself, exposed for tests: `width` x `height` RGBA, sigma in pixels,
    // edges clamped so the border keeps the picture's own colours.
    static void gaussian_blur(Image& image, float sigma);

private:
    // One blurred render of the source for one size.
    struct Layer {
        std::future<std::unique_ptr<Image>> rendering;
        ImVec2 rendering_size{};
        std::unique_ptr<Image> blurred; // kept for a re-upload after the renderer changes
        ImVec2 blurred_size{};
        ImTextureID texture = 0;
    };

    static std::shared_ptr<const Image> decode(std::span<const std::uint8_t> png);
    static std::unique_ptr<Image> render(std::shared_ptr<const Image> source, ImVec2 size, float sigma);
    bool source_ready();
    void update_layer(Layer& layer, ImVec2 size, float sigma, TextureHost* host);

    std::span<const std::uint8_t> png_;
    std::future<std::shared_ptr<const Image>> decoding_;
    std::shared_ptr<const Image> source_;
    bool failed_ = false;
    Layer backdrop_, panel_;
    TextureHost* host_ = nullptr;
};

} // namespace medicon::gui
