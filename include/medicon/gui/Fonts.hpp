#pragma once

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace medicon::gui {

enum class FontRole { Header, Module, KeyHint, Label, Value, ArrayList, ArrayListMode, Splash, MainMenuLogo, MainMenuButton,
                      Watermark, WatermarkSmall, WatermarkJapanese };
enum class FontWeight { DynaPuff, DynaPuffEm, Kawai, DynaPuffLogo };

// Metrics of one face in a font file or collection (.ttc), found by its family
// and subfamily names because TTC face indices differ between Windows builds.
// Empty names accept the first valid face.
struct FontFaceMetrics {
    int index = 0;
    int ascender = 0;
    int descender = 0;
    int cap_height = 0;
    int units_per_em = 0;
    bool valid = false;
};
FontFaceMetrics find_font_face(const unsigned char* data, std::size_t size, std::string_view family,
                               std::string_view subfamily);
std::filesystem::path system_fonts_directory();
// The bundled Japanese face (kawai.otf), embedded in the binary.
std::span<const unsigned char> bundled_japanese_font() noexcept;

// Pixel sizes requested for one frame. ArrayList sizes are CSS-style em pixels
// and retain fractional values so setting.scale does not enlarge an old bitmap.
struct FontSizes {
    float header = 14.0F;
    float module = 13.0F;
    float key_hint = 11.0F;
    float label = 12.0F;
    float value = 12.0F;
    float arraylist = 15.0F;
    float arraylist_mode = 12.0F;
    float splash = 64.0F;
    // Zero leaves these optional faces out of the atlas while the menu is off.
    float main_menu_logo = 0.0F;
    float main_menu_button = 0.0F;
    // The in-game Watermark: its name and the small version / FPS / ping text.
    float watermark = 0.0F;
    float watermark_small = 0.0F;
    float watermark_japanese = 0.0F;
};

// A font face ready for drawing. Measurement and drawing must both go through
// the same {font, size} pair so advance widths match what is rendered.
struct FontFace {
    ImFont* font = nullptr;
    float size = 13.0F;
    float baseline = 10.0F; // px from the text origin (top) to the baseline
    float cap_height = 9.0F; // px
};

// The bundled splash picture shares the font atlas texture, so both renderer
// backends can draw it with atlas.TexID without separate GPU resources.
struct SplashImage {
    bool valid = false;
    ImVec2 uv_min{};
    ImVec2 uv_max{};
    ImVec2 size{};
};

// ImGui 1.91 has no dynamic font sizes, so faces are rasterised per requested
// pixel size with glyph ranges limited to the characters actually used. Call
// prepare() before ImGui::NewFrame(); if it returns true the renderer must
// re-upload the font texture.
class FontManager {
public:
    FontManager() = default;
    ~FontManager();
    FontManager(const FontManager&) = delete;
    FontManager& operator=(const FontManager&) = delete;

    // When the GUI owns the atlas (Preview) old faces are discarded on rebuild.
    // With a host-owned atlas faces are only ever appended, so host ImFont
    // pointers remain valid.
    void set_owns_atlas(bool owns) { owns_atlas_ = owns; }
    void add_text(std::string_view utf8);
    bool prepare(ImFontAtlas& atlas, const FontSizes& sizes);

    // Frame API.
    void set_sizes(const FontSizes& sizes) { sizes_ = sizes; }
    FontFace face(FontRole role) const;
    float measure(FontRole role, std::string_view text);
    // Draws with the baseline at baseline_y. Disable snapping while animating.
    void draw(ImDrawList* list, FontRole role, float x, float baseline_y, ImU32 color, std::string_view text,
              bool snap_to_pixels = true) const;
    // Returns `text` when it fits. Otherwise writes the shortened form into `scratch`.
    std::string_view ellipsize(FontRole role, std::string_view text, float max_width, std::string& scratch);
    // Baseline for text vertically centred on cap height inside [top, top + height].
    float centered_baseline(FontRole role, float top, float height, bool snap_to_pixels = true) const;
    const SplashImage& splash_image() const { return splash_image_; }

    bool using_system_fonts() const { return !built_.empty(); }
    const std::string& description() const { return description_; }

private:
    struct FileData {
        void* data = nullptr; // IM_ALLOC'd; never owned by the atlas
        int size = 0;
    };
    struct FaceInfo {
        std::string file;
        int index = 0;
        int ascender = 0;
        int descender = 0;
        int cap_height = 0;
        int units_per_em = 0;
        bool valid = false;
    };
    struct Built {
        ImFont* font = nullptr;
        float baseline = 0.0F;
        float cap = 0.0F;
    };

    float role_size(FontRole role) const;
    static FontWeight role_weight(FontRole role);
    const FileData* load_file(const std::string& name);
    FaceInfo find_face(const std::string& file, std::string_view family, std::string_view subfamily);
    void resolve_faces();
    ImWchar* make_ranges(const std::set<char32_t>& codepoints, bool cjk_only);
    void release_ranges();
    void load_splash_image();
    bool prepare_splash_rect(ImFontAtlas& atlas);
    void fill_splash_rect(ImFontAtlas& atlas);

    bool owns_atlas_ = false;
    bool faces_resolved_ = false;
    FaceInfo dynapuff_{};
    FaceInfo kawai_{};
    FaceInfo cjk_{};
    std::set<char32_t> codepoints_;
    std::size_t built_codepoints_ = 0;
    std::map<std::pair<int, int>, Built> built_; // {face, requested px * 64}
    std::unordered_map<std::string, FileData> files_;
    std::vector<ImWchar*> ranges_;
    struct MeasureKey {
        const void* font = nullptr;
        int size = 0;
        std::uint64_t hash = 0;
        bool operator==(const MeasureKey&) const = default;
    };
    struct MeasureKeyHash {
        std::size_t operator()(const MeasureKey& key) const noexcept {
            return static_cast<std::size_t>(key.hash) ^ (static_cast<std::size_t>(key.size) << 1) ^
                   reinterpret_cast<std::uintptr_t>(key.font);
        }
    };
    std::unordered_map<MeasureKey, float, MeasureKeyHash> measure_cache_;
    FontSizes sizes_{};
    std::string description_;
    bool splash_image_loaded_ = false;
    int splash_image_width_ = 0;
    int splash_image_height_ = 0;
    std::vector<unsigned char> splash_pixels_;
    ImFontAtlas* splash_atlas_ = nullptr;
    int splash_rect_ = -1;
    SplashImage splash_image_{};
};

} // namespace medicon::gui
