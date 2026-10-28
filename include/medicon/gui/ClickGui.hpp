#pragma once

#include <medicon/core/Setting.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace medicon {

class Module;
class ModuleManager;

namespace gui {
struct GuiContext;
}

/// Dropdown ClickGUI: one panel per category, generated from the ModuleManager.
/// Registering a module and its settings is enough to expose it here.
/// Constructing ClickGui does not require an ImGui context; rendering does.
class ClickGui {
public:
    using PanelCallback = std::function<void()>;
    using ModuleSettingsCallback = std::function<void(Module&)>;

    explicit ClickGui(ModuleManager& modules);
    ~ClickGui();
    ClickGui(const ClickGui&) = delete;
    ClickGui& operator=(const ClickGui&) = delete;

    // Driven by gui::Overlay.
    void render(gui::GuiContext& context);
    /// Returns true when the key must not reach the game or module hotkeys.
    bool key_event(int virtual_key, bool down, bool repeat);
    /// GUI Scale value used for layout; held constant while its slider is dragged.
    [[nodiscard]] float effective_gui_scale() const;
    [[nodiscard]] bool consume_save_request() noexcept;

    [[nodiscard]] bool open() const noexcept;
    void set_open(bool open);
    [[nodiscard]] bool capturing_keybind() const noexcept;
    /// The Social button at the bottom centre, like the Main Menu's. Hidden
    /// without a Social service; `badge` marks unread DMs or pending requests.
    void set_social(bool shown, bool badge) noexcept;
    /// True once after the Social button was clicked.
    [[nodiscard]] bool consume_social_request() noexcept;

    /// Adds an independent ImGui window, visible while the client GUI is open.
    void add_panel(std::string title, PanelCallback draw);
    /// Replaces the automatic settings of one module with an ImGui window shown while
    /// the module is expanded. Call render_settings() inside to include the defaults.
    void set_module_settings_renderer(std::string module_id, ModuleSettingsCallback draw);
    void set_status(std::string status);
    [[nodiscard]] const std::string& status() const noexcept;

    /// Styles ImGui windows (add_panel / custom settings) to match the GUI theme.
    static void apply_theme();
    /// ImGui widgets for a module's settings, for use inside custom renderers.
    static void render_settings(Module& module);

    // Automation for the Preview scenarios and tests.
    void set_panel_position(Category category, float x_rpx, float y_rpx);
    void set_panel_collapsed(Category category, bool collapsed);
    bool set_module_expanded(std::string_view module_id, bool expanded);
    bool open_overlay(std::string_view module_id, std::string_view setting_id);
    void set_panel_scroll(Category category, float px);
    void set_debug_panel_height(Category category, float rpx);
    void finish_animations();
    struct PanelRect {
        Category category;
        float x, y, w, h; // physical px, as last drawn
    };
    [[nodiscard]] std::vector<PanelRect> panel_rects() const;
    [[nodiscard]] float ui_scale() const noexcept;
    [[nodiscard]] bool overlay_open() const noexcept;
    [[nodiscard]] float panel_scroll_target(Category category) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace medicon
