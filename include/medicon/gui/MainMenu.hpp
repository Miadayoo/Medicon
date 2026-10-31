#pragma once

#include <imgui.h>

#include <array>
#include <chrono>
#include <optional>

namespace medicon::gui {

class FontManager;
struct PointerInput;

enum class MainMenuAction { Play, Settings, Quit };
const char* main_menu_action_name(MainMenuAction action) noexcept;

struct MainMenuRect {
    ImVec2 min{}, max{};
    bool contains(ImVec2 point) const noexcept;
};

struct MainMenuBlobSpec {
    ImVec2 anchor{};                 // fractions of the window dimensions
    float radius_fraction = 0.6F;    // fraction of the shorter dimension
    double period_x = 8.0, period_y = 9.0;
    double phase_x = 0.0, phase_x2 = 0.0, phase_y = 0.0, phase_y2 = 0.0;
    ImU32 color = 0;
};

struct MainMenuBlob {
    ImVec2 center{}, velocity{};     // physical pixels, physical pixels/second
    float radius = 0.0F;
    ImU32 color = 0;
};

struct MainMenuFrame {
    std::array<MainMenuBlob, 5> blobs{};
    std::array<MainMenuRect, 3> buttons{};
    // Social (spec 4.6): bottom centre, apart from the left-aligned column.
    // Laid out always; drawn and clickable only when show_social is set.
    MainMenuRect social{};
    bool show_social = false;
    bool social_badge = false;
    // AltManager: the Social row, right of the Social button. Laid out always;
    // drawn and clickable only when show_alts is set.
    MainMenuRect alts{};
    bool show_alts = false;
    ImVec2 logo_center{};
    float logo_size = 0.0F, button_text_size = 0.0F;
    float corner_radius = 0.0F;
};

const std::array<MainMenuBlobSpec, 5>& main_menu_blob_specs() noexcept;
// Pure real-time evaluation also used by preview and tests. Nominal blob speed
// is 6.5%/second RMS in normalized window coordinates; sinusoidal motion has
// naturally varying instantaneous speed, including slow turning points.
MainMenuFrame evaluate_main_menu(ImVec2 display_size, double elapsed) noexcept;
// Button, logo and Social rectangles for a window size (no blob motion).
void layout_main_menu_text(ImVec2 display_size, MainMenuFrame& frame) noexcept;
// Cheap text metrics without blob sinusoids, for per-frame font sizing while
// the menu is active. Produces the same logo/button sizes as evaluate_main_menu.
void main_menu_text_sizes(ImVec2 display_size, float& logo_size, float& button_text_size) noexcept;
ImU32 main_menu_logo_color(float x, float wavelength, double elapsed) noexcept;
// The pastel gradient alone (no logo or buttons), as the Main Menu draws it at
// `elapsed`; the Social screen keeps it behind its panel.
void draw_main_menu_backdrop(ImDrawList* list, ImVec2 display_size, double elapsed, float alpha = 1.0F);
void draw_main_menu(ImDrawList* list, const FontManager& fonts, ImVec2 display_size,
                    double elapsed, int hovered_button = -1, int pressed_button = -1);

class MainMenu {
public:
    void set_active(bool active) noexcept;
    bool active() const noexcept { return active_; }
    void render(ImDrawList* list, const FontManager& fonts, ImVec2 display_size, const PointerInput& pointer);
    void render_at(ImDrawList* list, const FontManager& fonts, ImVec2 display_size,
                   const PointerInput& pointer, double elapsed);
    // Drain outside the graphics hook. No game pointer or callback is retained.
    std::optional<MainMenuAction> consume_action() noexcept;
    // Seconds since the menu was last activated: the time its gradient is drawn at.
    double elapsed() const noexcept;
    // The Social button is shown only while Social is connected to a service.
    void set_social(bool shown, bool badge) noexcept;
    bool social_shown() const noexcept { return social_shown_; }
    // True once per click on the Social button. Handled by the overlay, not the game.
    bool consume_social_request() noexcept;
    // The AltManager button, shown only while an alt service is attached.
    void set_alts(bool shown) noexcept;
    bool alts_shown() const noexcept { return alts_shown_; }
    // True once per click on AltManager. Handled by the overlay, not the game.
    bool consume_alts_request() noexcept;

    static constexpr int social_button = 3; // hover/press index of the Social button
    static constexpr int alts_button = 4;   // hover/press index of the AltManager button

private:
    bool active_ = false;
    bool social_shown_ = false;
    bool social_badge_ = false;
    bool social_requested_ = false;
    bool alts_shown_ = false;
    bool alts_requested_ = false;
    bool pointer_armed_ = false;
    int pressed_button_ = -1;
    double last_action_time_ = -1.0;
    std::optional<MainMenuAction> pending_action_;
    std::chrono::steady_clock::time_point start_time_{};
};

} // namespace medicon::gui
