#pragma once

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace medicon::gui {

class TextureHost;

// Social text sizes: em px at a 1080 px tall window (spec 4.6 / 4.9).
// Display is the sign-in title "Medicon".
enum class SocialText { Display, Heading, Title, Body, Small };
inline constexpr std::size_t social_text_count = 5;

// The Main Menu's faces: the bundled DynaPuff, with the bundled kawai.otf for
// Japanese, as FontManager merges them. Built in an atlas and texture of its own. Social text never grows the ClickGUI/HUD atlas, and new
// glyphs are rasterised on a worker thread instead of stalling a frame: until a
// build lands, missing glyphs draw as blanks. Render thread only, apart from that
// build, which shares nothing mutable with the render thread.
class SocialFonts {
public:
    SocialFonts();
    ~SocialFonts();
    SocialFonts(const SocialFonts&) = delete;
    SocialFonts& operator=(const SocialFonts&) = delete;

    // Glyphs `utf8` needs at `role`'s size; the next background build adds them.
    void request(SocialText role, std::string_view utf8);
    // Once per frame: follows the window height, starts a build when glyphs or
    // sizes changed, and uploads a finished build through `host` (may be null).
    void update(float display_height, TextureHost* host);
    // The texture host went away together with its textures. The next update
    // uploads the current atlas to the new host.
    void forget_texture() noexcept;

    // An atlas at the current sizes is uploaded.
    bool ready() const noexcept;
    // Every glyph of `utf8` at `role` is in the uploaded atlas.
    bool has(SocialText role, std::string_view utf8) const;
    // A build is running or waiting for its upload.
    bool busy() const noexcept { return building_.valid() || pending_ || dirty_; }
    // Changes whenever another atlas is drawn from: cached measurements and line
    // breaks must be redone.
    std::uint32_t revision() const noexcept { return revision_; }

    float line_height(SocialText role) const noexcept;
    // The em size in px that `role` is rasterised at.
    float em(SocialText role) const noexcept;
    float measure(SocialText role, std::string_view text) const;
    // `top` is the top of the line box. Must run inside a Scope.
    void draw(ImDrawList* list, SocialText role, float x, float top, ImU32 color, std::string_view text) const;
    // Returns `text` when it fits; otherwise writes a shortened copy ending in … to `scratch`.
    std::string_view ellipsize(SocialText role, std::string_view text, float max_width, std::string& scratch) const;

    struct Line {
        std::uint32_t begin = 0, end = 0; // byte offsets into the wrapped text
    };
    // Breaks at spaces and between Japanese characters, never before closing or
    // after opening punctuation. Explicit newlines are kept.
    void wrap(SocialText role, std::string_view text, float width, std::vector<Line>& lines) const;

    // Binds the Social atlas to `list` while it lives: its texture, plus the
    // white-pixel and line UVs ImGui shapes use, so shapes and text drawn inside
    // share one draw call. Does nothing until the atlas is ready.
    class Scope {
    public:
        Scope(const SocialFonts& fonts, ImDrawList* list);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        ImDrawList* list_ = nullptr;
        ImVec2 white_{};
        const ImVec4* lines_ = nullptr;
    };

private:
    struct Face;
    struct Build;
    using Sizes = std::array<float, social_text_count>;
    using Glyphs = std::array<std::set<char32_t>, social_text_count>;

    static std::shared_ptr<const Face> load_face();
    static std::unique_ptr<Build> build(std::shared_ptr<const Face> face, Sizes sizes, Glyphs glyphs);
    ImFont* font(SocialText role) const noexcept;
    float scale(SocialText role) const noexcept;
    void upload(TextureHost* host);

    std::future<std::shared_ptr<const Face>> face_loading_;
    std::shared_ptr<const Face> face_;
    bool face_failed_ = false;
    Glyphs requested_;
    bool dirty_ = false;
    Sizes sizes_{};
    std::future<std::unique_ptr<Build>> building_;
    std::unique_ptr<Build> current_;  // drawn from once uploaded
    std::unique_ptr<Build> pending_;  // built, waiting for a texture host
    ImTextureID texture_ = 0;
    TextureHost* host_ = nullptr;
    std::uint32_t revision_ = 0;
};

} // namespace medicon::gui
