#include <medicon/gui/ClickGui.hpp>

#include <medicon/core/Module.hpp>
#include <medicon/core/ModuleManager.hpp>
#include <medicon/gui/Draw.hpp>
#include <medicon/gui/GuiContext.hpp>
#include <medicon/gui/GuiMath.hpp>
#include <medicon/gui/KeyNames.hpp>
#include <medicon/gui/Log.hpp>
#include <medicon/gui/Theme.hpp>
#include <medicon/modules/ClientModules.hpp>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace medicon {

using namespace gui;
namespace mt = theme::metrics;
namespace tm = theme::timing;

namespace {

enum class HitKind {
    None, Header, Module, Setting, Scrollbar, Body,
    OverlayItem, OverlayBody, OverlayScrollbar, PickerSV, PickerHue, PickerAlpha
};

struct Hit {
    HitKind kind = HitKind::None;
    int panel = -1;
    Module* module = nullptr;
    int setting = -2; // -1 = keybind row
    int item = -1;
    bool operator==(const Hit&) const = default;
    bool in_overlay() const { return kind >= HitKind::OverlayItem; }
};

enum class DragKind { None, Panel, Slider, Scrollbar, OverlayScrollbar, PickerSV, PickerHue, PickerAlpha };
constexpr int keybind_row = -1;

struct SettingRow {
    int index = keybind_row;
    float top = 0.0F;
    float bottom = 0.0F;
};

struct ModuleLayout {
    Module* module = nullptr;
    float top = 0.0F;
    float bottom = 0.0F;
    bool expandable = false;
    float extra_rpx = 0.0F; // animated settings height, computed once per layout
    float extra_bottom = 0.0F; // visible end of the (animated) settings region
    float block_top = 0.0F;
    float block_bottom = 0.0F;
    std::vector<SettingRow> rows;
};

struct PanelLayout {
    float x = 0, y = 0, w = 0, h = 0, header_h = 0, sep = 0, bw = 0;
    float view_top = 0, view_bottom = 0, rows_px = 0, max_scroll = 0;
    bool scrollable = false;
    float bar_left = 0, bar_right = 0, thumb_top = 0, thumb_bottom = 0;
    std::vector<ModuleLayout> modules;
};

struct ModuleUi {
    bool expanded = false;
    Reversible expand;
    float hover = 0.0F;
    float enabled = 0.0F;
    bool initialised = false;
    std::vector<Reversible> toggles;
};

struct Panel {
    Category category = Category::Misc;
    bool present = false;
    std::vector<Module*> modules;
    Reversible expand;
    bool initialised = false;
    float scroll = 0.0F;
    float scroll_target = 0.0F;
    float debug_height = 0.0F;
    PanelLayout layout;
};

enum class OverlayKind { None, Dropdown, Color };

struct OverlayState {
    OverlayKind kind = OverlayKind::None;
    int panel = -1;
    Module* module = nullptr;
    int setting = -1;
    HsvColor hsv{};
    float alpha = 1.0F;
    float scroll = 0.0F;
    float scroll_target = 0.0F;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool scrollable = false;
    float max_scroll = 0, thumb_top = 0, thumb_bottom = 0, bar_left = 0, bar_right = 0;
};

struct Press {
    int button = 0;
    Hit target;
    ImVec2 origin;
    bool moved = false;
    bool swallowed = false;
    DragKind drag = DragKind::None;
    ImVec2 start_panel;
    float start_scroll = 0.0F;
};

struct CustomWindow {
    std::string title;
    ClickGui::PanelCallback draw;
};

int visible_setting_count(const Module& module) {
    int count = 0;
    for (const auto& setting : module.settings()) count += setting.visible ? 1 : 0;
    return count;
}

bool is_slider(const Setting& setting) {
    return std::holds_alternative<int>(setting.value) || std::holds_alternative<float>(setting.value);
}

std::string setting_value_text(const Setting& setting) {
    return std::visit([&](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, int>) return std::to_string(value);
        else if constexpr (std::is_same_v<T, float>) return format_number(value, float_precision(setting.step));
        else if constexpr (std::is_same_v<T, Choice>) {
            return value.index >= 0 && static_cast<std::size_t>(value.index) < value.options.size()
                ? value.options[static_cast<std::size_t>(value.index)] : std::string{};
        } else if constexpr (std::is_same_v<T, std::string>) return value;
        else if constexpr (std::is_same_v<T, bool>) return value ? "On" : "Off";
        else return std::string{};
    }, setting.value);
}

std::string hex_text(const Color& c) {
    const auto byte = [](float v) { return static_cast<unsigned>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F)); };
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "#%02X%02X%02X%02X", byte(c.r), byte(c.g), byte(c.b), byte(c.a));
    return buffer;
}

} // namespace

struct ClickGui::Impl {
    explicit Impl(ModuleManager& m) : modules(m) {
        for (std::size_t i = 0; i < category_count; ++i) panels[i].category = static_cast<Category>(i);
    }

    ModuleManager& modules;
    ClickGuiModule* cfg = nullptr;
    GuiContext* ctx = nullptr;
    bool local_open = true;
    bool was_open = false;
    Reversible fade;
    std::array<Panel, category_count> panels;
    std::vector<int> z_order; // back to front, panel indices
    std::string applied_order = "\x01";
    bool checked_legacy_layout = false;
    std::unordered_map<const Module*, ModuleUi> module_ui;
    OverlayState overlay;
    std::optional<Press> press;
    Hit hovered;
    Module* capture = nullptr;
    bool save_requested = false;
    float scale = 1.0F;
    float latched_gui_scale = 1.0F;
    float last_scale = 0.0F, last_w = 0.0F, last_h = 0.0F, last_dpi = 0.0F;
    FrameInfo frame;
    struct Local {
        std::array<float, category_count> x{};
        std::array<float, category_count> y{};
        std::array<bool, category_count> collapsed{};
        std::string order;
    } local;
    std::vector<CustomWindow> windows;
    std::unordered_map<std::string, ModuleSettingsCallback> renderers;
    std::string status;
    bool warned_unsupported = false;
    std::pair<Module*, int> pending_overlay{nullptr, -1}; // automation: opened after the next layout
    // Cached parse of the persisted panel order. placed()/display_position run
    // per panel per layout (dozens of times per frame); reparsing and
    // allocating a vector each time showed up hot. Reparse only on change.
    std::string parsed_order_text = "\x02";
    std::vector<int> parsed_order;
    // Row pitch for the current frame. panel_height_limit used to recompute it
    // per panel (scanning all panels + rounding); compute once per render().
    float frame_pitch = mt::panel_wrap_step_y;

    // --- State storage (ClickGuiModule settings, or local fallback) ---------
    void resolve_cfg() { cfg = dynamic_cast<ClickGuiModule*>(modules.find(ClickGuiModule::module_id)); }
    float& stored_x(int i) { return cfg ? cfg->panel(static_cast<Category>(i)).x->get<float>() : local.x[static_cast<std::size_t>(i)]; }
    float& stored_y(int i) { return cfg ? cfg->panel(static_cast<Category>(i)).y->get<float>() : local.y[static_cast<std::size_t>(i)]; }
    bool& stored_collapsed(int i) {
        return cfg ? cfg->panel(static_cast<Category>(i)).collapsed->get<bool>() : local.collapsed[static_cast<std::size_t>(i)];
    }
    std::string& order_string() { return cfg ? cfg->panel_order.get<std::string>() : local.order; }

    bool is_open() const { return cfg ? cfg->enabled() : local_open; }
    void set_open(bool value) {
        if (cfg) cfg->set_enabled(value);
        else local_open = value;
    }
    void request_save() { save_requested = true; }

    ModuleUi& ui(const Module* module) {
        auto& state = module_ui[module];
        if (!state.initialised) {
            state.initialised = true;
            state.enabled = module->enabled() ? 1.0F : 0.0F;
            state.toggles.resize(module->settings().size());
            for (std::size_t i = 0; i < module->settings().size(); ++i) {
                if (const auto* b = std::get_if<bool>(&module->settings()[i].value)) state.toggles[i].snap(*b);
            }
        }
        if (state.toggles.size() < module->settings().size()) state.toggles.resize(module->settings().size());
        return state;
    }

    bool has_renderer(const Module* module) const { return renderers.contains(module->id()); }
    bool expandable(const Module* module) const { return visible_setting_count(*module) > 0 || has_renderer(module); }
    int row_count(const Module* module) const {
        return has_renderer(module) ? 1 : visible_setting_count(*module) + 1; // + keybind row
    }
    float extra_rpx(const Module* module) const {
        if (!expandable(module)) return 0.0F;
        return mt::setting_gap_top + static_cast<float>(row_count(module)) * mt::setting_height + mt::setting_gap_bottom;
    }

    // --- Panels and order ------------------------------------------------
    void sync_panels() {
        for (auto& panel : panels) {
            panel.modules.clear();
            panel.present = false;
        }
        for (const auto& module : modules.all()) {
            auto& panel = panels[static_cast<std::size_t>(module->category())];
            panel.modules.push_back(module.get());
            panel.present = true;
        }
        for (std::size_t i = 0; i < panels.size(); ++i) {
            auto& panel = panels[i];
            if (panel.present && !panel.initialised) {
                panel.initialised = true;
                panel.expand.snap(!stored_collapsed(static_cast<int>(i)));
            }
        }
    }

    static int category_from_id(std::string_view id) {
        for (std::size_t i = 0; i < category_count; ++i) {
            if (id == category_id(static_cast<Category>(i))) return static_cast<int>(i);
        }
        return -1;
    }

    std::vector<int> parse_order(const std::string& text) const {
        std::vector<int> result;
        result.reserve(category_count);
        std::size_t start = 0;
        while (start <= text.size()) {
            const auto end = text.find(',', start);
            const auto token = std::string_view(text).substr(start, end == std::string::npos ? std::string::npos : end - start);
            const int index = category_from_id(token);
            if (index >= 0 && std::find(result.begin(), result.end(), index) == result.end()) result.push_back(index);
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return result;
    }

    const std::vector<int>& cached_order() {
        const std::string& text = order_string();
        if (text != parsed_order_text) {
            parsed_order_text = text;
            parsed_order = parse_order(text);
        }
        return parsed_order;
    }

    bool placed(int index) {
        const auto& order = cached_order();
        return std::find(order.begin(), order.end(), index) != order.end();
    }

    void sync_z_order() {
        if (order_string() != applied_order) {
            applied_order = order_string();
            z_order.clear();
            for (const int index : cached_order()) {
                if (panels[static_cast<std::size_t>(index)].present) z_order.push_back(index);
            }
        }
        std::erase_if(z_order, [&](int index) { return !panels[static_cast<std::size_t>(index)].present; });
        for (std::size_t i = 0; i < panels.size(); ++i) {
            if (panels[i].present && std::find(z_order.begin(), z_order.end(), static_cast<int>(i)) == z_order.end()) {
                z_order.push_back(static_cast<int>(i));
            }
        }
    }

    // Persists the current order; panels never saved before keep their initial position.
    void write_order() {
        for (std::size_t i = 0; i < panels.size(); ++i) {
            if (!panels[i].present || placed(static_cast<int>(i))) continue;
            const auto point = initial_position(static_cast<int>(i));
            stored_x(static_cast<int>(i)) = point.x;
            stored_y(static_cast<int>(i)) = point.y;
        }
        std::string text;
        for (const int index : z_order) {
            if (!text.empty()) text.push_back(',');
            text += category_id(static_cast<Category>(index));
        }
        order_string() = text;
        applied_order = text;
        parsed_order_text = text;
        parsed_order = parse_order(text);
    }

    void bring_to_front(int index) {
        const auto found = std::find(z_order.begin(), z_order.end(), index);
        if (found == z_order.end()) return;
        z_order.erase(found);
        z_order.push_back(index);
        write_order();
    }

    float initial_row_pitch() const {
        int count = 0;
        for (const auto& panel : panels) count += panel.present ? 1 : 0;
        if (count == 0) return mt::panel_wrap_step_y;
        const auto last = initial_panel_position(count - 1, frame.client_w / scale);
        const int rows = 1 + static_cast<int>(std::lround((last.y - mt::start_y) / mt::panel_wrap_step_y));
        if (rows <= 1) return mt::panel_wrap_step_y;
        const float available = frame.client_h / scale - mt::start_y - mt::overlay_screen_margin;
        const float slot = std::max(mt::header_height + mt::module_height,
            (available - static_cast<float>(rows - 1) * mt::panel_gap) / static_cast<float>(rows));
        return std::min(mt::panel_wrap_step_y, slot + mt::panel_gap);
    }

    void update_frame_pitch() { frame_pitch = initial_row_pitch(); }

    PanelPoint initial_position(int index) const {
        int ordinal = 0;
        for (int i = 0; i < index; ++i) ordinal += panels[static_cast<std::size_t>(i)].present ? 1 : 0;
        auto point = initial_panel_position(ordinal, frame.client_w / scale);
        const float row = std::round((point.y - mt::start_y) / mt::panel_wrap_step_y);
        point.y = mt::start_y + row * frame_pitch;
        return point;
    }

    void migrate_default_layout() {
        if (checked_legacy_layout) return;
        checked_legacy_layout = true;
        // The old GUI saved its automatic grid too. Widening those panels in
        // place would overlap them; migrate only a completely untouched grid.
        PanelPoint old{36.0F, 42.0F};
        int count = 0;
        for (std::size_t i = 0; i < panels.size(); ++i) {
            if (!panels[i].present) continue;
            if (!placed(static_cast<int>(i)) || stored_x(static_cast<int>(i)) != old.x || stored_y(static_cast<int>(i)) != old.y) return;
            ++count;
            old.x += 162.0F;
            if (old.x + 150.0F > frame.client_w / scale) { old.x = 36.0F; old.y += 540.0F; }
        }
        if (count == 0) return;
        for (std::size_t i = 0; i < panels.size(); ++i) {
            if (!panels[i].present) continue;
            const auto point = initial_position(static_cast<int>(i));
            stored_x(static_cast<int>(i)) = point.x;
            stored_y(static_cast<int>(i)) = point.y;
        }
        request_save();
    }

    // Stored (rpx) position, or the initial layout for panels never saved.
    PanelPoint logical_position(int index) {
        if (placed(index)) return {stored_x(index), stored_y(index)};
        return initial_position(index);
    }

    PanelPoint display_position(int index) {
        const PanelPoint logical = logical_position(index);
        return clamp_panel_px({std::round(logical.x * scale), std::round(logical.y * scale)}, px(mt::panel_width, scale),
                              px(mt::header_height, scale), px(mt::keep_visible_x, scale), frame.client_w, frame.client_h);
    }

    float panel_height_limit(int index) {
        const auto position = display_position(index);
        return panel_height_limit_for(position, index);
    }

    float panel_height_limit_for(const PanelPoint& position, int index) {
        float limit = std::min(std::floor(mt::max_height_ratio * frame.client_h),
                               frame.client_h - position.y - px(mt::overlay_screen_margin, scale));
        const float pitch = frame_pitch;
        const auto initial = initial_position(index);
        const auto logical = logical_position(index);
        if (pitch < mt::panel_wrap_step_y && logical.x == initial.x && logical.y == initial.y) {
            // Defaults on multiple rows share the available height. Each long
            // list scrolls inside its slot instead of covering the next row.
            limit = std::min(limit, px(pitch - mt::panel_gap, scale));
        }
        return std::max(px(mt::header_height, scale), limit);
    }

    // --- Layout -------------------------------------------------------------
    void layout(int index) {
        auto& panel = panels[static_cast<std::size_t>(index)];
        auto& L = panel.layout;
        // Layout runs before input and again after animation. Keep the module
        // and setting-row buffers instead of freeing them twice every frame.
        L.modules.resize(panel.modules.size());
        const float s = scale;
        const PanelPoint pos = display_position(index);
        L.x = pos.x;
        L.y = pos.y;
        L.w = px(mt::panel_width, s);
        L.header_h = px(mt::header_height, s);
        L.sep = px(mt::separator, s);
        L.bw = px(mt::border_width, s);
        const float pad = px(mt::bottom_padding, s);
        const float v = panel.expand.value();

        float rows_rpx = 0.0F;
        for (std::size_t i = 0; i < panel.modules.size(); ++i) {
            auto& ml = L.modules[i];
            Module* module = panel.modules[i];
            ml.module = module;
            const bool custom = has_renderer(module);
            const int settings = visible_setting_count(*module);
            ml.expandable = custom || settings > 0;
            ml.extra_rpx = ml.expandable
                ? (mt::setting_gap_top + static_cast<float>(custom ? 1 : settings + 1) * mt::setting_height +
                   mt::setting_gap_bottom) * ui(module).expand.value()
                : 0.0F;
            rows_rpx += mt::module_height + ml.extra_rpx;
        }
        L.rows_px = px(rows_rpx, s);
        const float body = L.sep + L.rows_px + L.bw + pad;
        const float max_h = panel_height_limit_for(pos, index);
        float full_h = L.header_h + body;
        if (panel.debug_height > 0.0F) full_h = px(panel.debug_height, s);
        full_h = std::max(L.header_h, std::min(full_h, max_h));
        L.h = L.header_h + std::round((full_h - L.header_h) * v);
        L.view_top = L.y + L.header_h + L.sep;
        L.view_bottom = std::max(L.view_top, L.y + L.h - L.bw - pad);
        const float full_view = std::max(0.0F, full_h - L.header_h - L.sep - L.bw - pad);
        L.max_scroll = std::max(0.0F, L.rows_px - full_view);
        panel.scroll_target = std::clamp(panel.scroll_target, 0.0F, L.max_scroll);
        panel.scroll = std::clamp(panel.scroll, 0.0F, L.max_scroll);
        L.scrollable = L.max_scroll > 0.5F && v >= 1.0F;

        const float sc = std::round(panel.scroll);
        float off = 0.0F;
        std::size_t module_index = 0;
        for (Module* module : panel.modules) {
            auto& ml = L.modules[module_index++];
            ml.rows.clear();
            ml.block_top = ml.block_bottom = 0.0F;
            const float extra = ml.extra_rpx;
            ml.top = L.view_top + px(off, s) - sc;
            ml.bottom = L.view_top + px(off + mt::module_height, s) - sc;
            ml.extra_bottom = L.view_top + px(off + mt::module_height + extra, s) - sc;
            // Collapsed settings cannot be drawn or hit. Do not reconstruct all
            // their row coordinates twice per frame just to clip them away.
            if (extra > 0.0F) {
                const float block = off + mt::module_height + mt::setting_gap_top;
                ml.block_top = L.view_top + px(block, s) - sc;
                int row = 0;
                const auto add_row = [&](int setting_index) {
                    ml.rows.push_back({setting_index, L.view_top + px(block + static_cast<float>(row) * mt::setting_height, s) - sc,
                                       L.view_top + px(block + static_cast<float>(row + 1) * mt::setting_height, s) - sc});
                    ++row;
                };
                if (!has_renderer(module)) {
                    for (std::size_t i = 0; i < module->settings().size(); ++i) {
                        if (module->settings()[i].visible) add_row(static_cast<int>(i));
                    }
                }
                add_row(keybind_row);
                ml.block_bottom = L.view_top + px(block + static_cast<float>(row) * mt::setting_height, s) - sc;
            }
            off += mt::module_height + extra;
        }

        L.bar_right = L.x + L.w - L.bw - px(mt::scrollbar_margin, s);
        L.bar_left = L.bar_right - px(mt::scrollbar_width, s);
        const float vh = L.view_bottom - L.view_top;
        if (L.scrollable && L.rows_px > 0.0F) {
            const float thumb = std::max(px(mt::scrollbar_min_thumb, s), vh * vh / L.rows_px);
            L.thumb_top = L.view_top + (vh - thumb) * (panel.scroll / L.max_scroll);
            L.thumb_bottom = L.thumb_top + thumb;
        }
    }

    const ModuleLayout* find_module_layout(int panel, const Module* module) const {
        for (const auto& ml : panels[static_cast<std::size_t>(panel)].layout.modules) {
            if (ml.module == module) return &ml;
        }
        return nullptr;
    }

    const SettingRow* find_row(const ModuleLayout& ml, int setting) const {
        for (const auto& row : ml.rows) if (row.index == setting) return &row;
        return nullptr;
    }

    void layout_overlay() {
        if (overlay.kind == OverlayKind::None) return;
        const auto& L = panels[static_cast<std::size_t>(overlay.panel)].layout;
        const ModuleLayout* ml = find_module_layout(overlay.panel, overlay.module);
        const SettingRow* row = ml ? find_row(*ml, overlay.setting) : nullptr;
        if (!row || row->bottom <= L.view_top || row->top >= std::min(L.view_bottom, ml->extra_bottom)) {
            close_overlay();
            return;
        }
        const float s = scale;
        overlay.x0 = L.x + px(mt::setting_inset + 1.0F, s);
        overlay.x1 = L.x + px(mt::panel_width - mt::setting_inset - 1.0F, s);
        float height = 0.0F;
        if (overlay.kind == OverlayKind::Dropdown) {
            const auto& choice = overlay.module->settings()[static_cast<std::size_t>(overlay.setting)].get<Choice>();
            const int count = static_cast<int>(choice.options.size());
            const int visible = std::min(count, mt::dropdown_max_items);
            height = px(2.0F * mt::dropdown_padding + static_cast<float>(visible) * mt::dropdown_item_height, s);
            const float content = static_cast<float>(count) * mt::dropdown_item_height * s;
            const float view = static_cast<float>(visible) * mt::dropdown_item_height * s;
            overlay.max_scroll = std::max(0.0F, content - view);
            overlay.scrollable = overlay.max_scroll > 0.5F;
        } else {
            height = px(2.0F * mt::picker_padding + mt::picker_square + 3.0F * mt::picker_spacing + 2.0F * mt::picker_bar +
                        mt::picker_hex_line, s);
            overlay.scrollable = false;
            overlay.max_scroll = 0.0F;
        }
        float top = row->bottom + px(mt::overlay_gap, s);
        if (top + height > frame.client_h - px(mt::overlay_screen_margin, s)) top = row->top - px(mt::overlay_gap, s) - height;
        overlay.y0 = top;
        overlay.y1 = top + height;
        overlay.scroll_target = std::clamp(overlay.scroll_target, 0.0F, overlay.max_scroll);
        overlay.scroll = std::clamp(overlay.scroll, 0.0F, overlay.max_scroll);
        if (overlay.scrollable) {
            overlay.bar_right = overlay.x1 - line_px(mt::border_width, s) - px(mt::scrollbar_margin, s);
            overlay.bar_left = overlay.bar_right - px(mt::scrollbar_width, s);
            const float view_top = overlay.y0 + px(mt::dropdown_padding, s);
            const float view_h = overlay.y1 - px(mt::dropdown_padding, s) - view_top;
            const float content = view_h + overlay.max_scroll;
            const float thumb = std::max(px(mt::scrollbar_min_thumb, s), view_h * view_h / content);
            overlay.thumb_top = view_top + (view_h - thumb) * (overlay.scroll / overlay.max_scroll);
            overlay.thumb_bottom = overlay.thumb_top + thumb;
        }
    }

    struct PickerRects {
        ImVec2 sv0, sv1, hue0, hue1, alpha0, alpha1;
        float hex_top, hex_bottom;
    };
    PickerRects picker_rects() const {
        const float s = scale;
        PickerRects r{};
        const float x0 = overlay.x0 + px(mt::picker_padding, s);
        const float x1 = x0 + px(mt::picker_square, s);
        float y = overlay.y0 + px(mt::picker_padding, s);
        r.sv0 = {x0, y};
        r.sv1 = {x1, y + px(mt::picker_square, s)};
        y = overlay.y0 + px(mt::picker_padding + mt::picker_square + mt::picker_spacing, s);
        r.hue0 = {x0, y};
        r.hue1 = {x1, y + px(mt::picker_bar, s)};
        y = overlay.y0 + px(mt::picker_padding + mt::picker_square + 2.0F * mt::picker_spacing + mt::picker_bar, s);
        r.alpha0 = {x0, y};
        r.alpha1 = {x1, y + px(mt::picker_bar, s)};
        r.hex_top = overlay.y0 + px(mt::picker_padding + mt::picker_square + 3.0F * mt::picker_spacing + 2.0F * mt::picker_bar, s);
        r.hex_bottom = r.hex_top + px(mt::picker_hex_line, s);
        return r;
    }

    // --- Hit testing ----------------------------------------------------------
    static bool inside(ImVec2 p, float x0, float y0, float x1, float y1) { return p.x >= x0 && p.x < x1 && p.y >= y0 && p.y < y1; }

    Hit hit_overlay(ImVec2 p) const {
        Hit hit;
        hit.panel = overlay.panel;
        hit.module = overlay.module;
        hit.setting = overlay.setting;
        if (overlay.kind == OverlayKind::Dropdown) {
            if (overlay.scrollable && inside(p, overlay.bar_left - 2.0F, overlay.thumb_top, overlay.bar_right + 2.0F, overlay.thumb_bottom)) {
                hit.kind = HitKind::OverlayScrollbar;
                return hit;
            }
            const float top = overlay.y0 + px(mt::dropdown_padding, scale);
            const float bottom = overlay.y1 - px(mt::dropdown_padding, scale);
            if (p.y >= top && p.y < bottom) {
                const auto& choice = overlay.module->settings()[static_cast<std::size_t>(overlay.setting)].get<Choice>();
                const int item = static_cast<int>(std::floor((p.y - top + std::round(overlay.scroll)) / (mt::dropdown_item_height * scale)));
                if (item >= 0 && item < static_cast<int>(choice.options.size())) {
                    hit.kind = HitKind::OverlayItem;
                    hit.item = item;
                    return hit;
                }
            }
        } else {
            const auto r = picker_rects();
            if (inside(p, r.sv0.x, r.sv0.y, r.sv1.x, r.sv1.y)) { hit.kind = HitKind::PickerSV; return hit; }
            if (inside(p, r.hue0.x, r.hue0.y, r.hue1.x, r.hue1.y)) { hit.kind = HitKind::PickerHue; return hit; }
            if (inside(p, r.alpha0.x, r.alpha0.y, r.alpha1.x, r.alpha1.y)) { hit.kind = HitKind::PickerAlpha; return hit; }
        }
        hit.kind = HitKind::OverlayBody;
        return hit;
    }

    Hit hit_test(ImVec2 p) {
        if (overlay.kind != OverlayKind::None && inside(p, overlay.x0, overlay.y0, overlay.x1, overlay.y1)) return hit_overlay(p);
        for (auto it = z_order.rbegin(); it != z_order.rend(); ++it) {
            const int index = *it;
            const auto& L = panels[static_cast<std::size_t>(index)].layout;
            if (!inside(p, L.x, L.y, L.x + L.w, L.y + L.h)) continue;
            Hit hit;
            hit.panel = index;
            if (p.y < L.y + L.header_h) {
                hit.kind = HitKind::Header;
                return hit;
            }
            if (L.scrollable && inside(p, L.bar_left - 2.0F, L.thumb_top, L.bar_right + 2.0F, L.thumb_bottom)) {
                hit.kind = HitKind::Scrollbar;
                return hit;
            }
            if (p.y >= L.view_top && p.y < L.view_bottom) {
                for (const auto& ml : L.modules) {
                    if (p.y >= ml.top && p.y < ml.bottom) {
                        hit.kind = HitKind::Module;
                        hit.module = ml.module;
                        return hit;
                    }
                    if (p.y >= ml.bottom && p.y < ml.extra_bottom) {
                        hit.module = ml.module;
                        const float bx0 = L.x + px(mt::setting_inset + 1.0F, scale);
                        const float bx1 = L.x + px(mt::panel_width - mt::setting_inset - 1.0F, scale);
                        for (const auto& row : ml.rows) {
                            if (p.y >= row.top && p.y < row.bottom && p.x >= bx0 && p.x < bx1) {
                                hit.kind = HitKind::Setting;
                                hit.setting = row.index;
                                return hit;
                            }
                        }
                        hit.kind = HitKind::Body; // gaps around the setting block
                        return hit;
                    }
                }
            }
            hit.kind = HitKind::Body;
            return hit;
        }
        return Hit{};
    }

    // --- Actions --------------------------------------------------------------
    void close_overlay() { overlay = OverlayState{}; }

    void open_overlay_for(int panel, Module* module, int setting) {
        const auto& value = module->settings()[static_cast<std::size_t>(setting)].value;
        OverlayState next;
        next.panel = panel;
        next.module = module;
        next.setting = setting;
        if (const auto* choice = std::get_if<Choice>(&value)) {
            next.kind = OverlayKind::Dropdown;
            const float item = mt::dropdown_item_height * scale;
            const float visible = static_cast<float>(std::min(static_cast<int>(choice->options.size()), mt::dropdown_max_items));
            next.scroll = next.scroll_target = std::max(0.0F, (static_cast<float>(choice->index) - visible + 1.0F) * item);
        } else if (const auto* color = std::get_if<Color>(&value)) {
            next.kind = OverlayKind::Color;
            next.hsv = rgb_to_hsv(*color);
            next.alpha = color->a;
        } else {
            return;
        }
        overlay = next;
        layout_overlay();
    }

    void toggle_collapse(int index) {
        auto& collapsed = stored_collapsed(index);
        collapsed = !collapsed;
        if (overlay.panel == index) close_overlay();
        write_order();
        request_save();
    }

    void set_module_expanded(int panel_index, Module* module, bool expanded) {
        auto& state = ui(module);
        if (state.expanded == expanded) return;
        state.expanded = expanded;
        if (!expanded && overlay.module == module) close_overlay();
        if (expanded) ensure_visible(panel_index, module);
    }

    // Scroll so the expanded block is fully visible (or its row is at the top).
    void ensure_visible(int panel_index, const Module* target) {
        auto& panel = panels[static_cast<std::size_t>(panel_index)];
        const float s = scale;
        float off = 0.0F;
        float rows = 0.0F;
        float target_top = 0.0F;
        float target_bottom = 0.0F;
        for (const Module* module : panel.modules) {
            const float extra = extra_rpx(module) * ((module == target || ui(module).expanded) ? 1.0F : 0.0F);
            if (module == target) {
                target_top = off;
                target_bottom = off + mt::module_height + extra - mt::setting_gap_bottom;
            }
            off += mt::module_height + extra;
            rows = off;
        }
        const auto& L = panel.layout;
        const float pad = px(mt::bottom_padding, s);
        const float max_h = panel_height_limit(panel_index);
        const float full_h = std::min(L.header_h + L.sep + px(rows, s) + L.bw + pad, max_h);
        const float view = std::max(0.0F, full_h - L.header_h - L.sep - L.bw - pad);
        const float top_px = px(target_top, s);
        const float bottom_px = px(target_bottom, s);
        float scroll = panel.scroll_target;
        if (bottom_px - scroll > view) scroll = bottom_px - view;
        if (bottom_px - top_px > view || top_px < scroll) scroll = top_px;
        panel.scroll_target = std::clamp(scroll, 0.0F, std::max(0.0F, px(rows, s) - view));
    }

    void start_capture(Module* module) {
        capture = module;
        close_overlay();
    }

    void apply_slider(const Hit& target, float mouse_x) {
        auto& setting = target.module->settings()[static_cast<std::size_t>(target.setting)];
        const auto& L = panels[static_cast<std::size_t>(target.panel)].layout;
        const float left = L.x + px(mt::setting_inset + 1.0F + mt::setting_padding, scale);
        const float right = L.x + px(mt::panel_width - mt::setting_inset - 1.0F - mt::setting_padding, scale);
        const bool integer = std::holds_alternative<int>(setting.value);
        const double value = slider_value(mouse_x, left, right, setting.minimum, setting.maximum,
                                          integer ? std::max(1.0, static_cast<double>(setting.step)) : setting.step,
                                          integer, float_precision(setting.step));
        if (integer) setting.get<int>() = static_cast<int>(value);
        else setting.get<float>() = static_cast<float>(value);
    }

    void write_picker_color() {
        auto& setting = overlay.module->settings()[static_cast<std::size_t>(overlay.setting)];
        setting.get<Color>() = hsv_to_rgb(overlay.hsv, overlay.alpha);
    }

    void apply_picker(DragKind kind, ImVec2 p) {
        const auto r = picker_rects();
        const auto fraction = [](float v, float a, float b) { return b > a ? std::clamp((v - a) / (b - a), 0.0F, 1.0F) : 0.0F; };
        switch (kind) {
        case DragKind::PickerSV:
            overlay.hsv.s = fraction(p.x, r.sv0.x, r.sv1.x);
            overlay.hsv.v = 1.0F - fraction(p.y, r.sv0.y, r.sv1.y);
            break;
        case DragKind::PickerHue: overlay.hsv.h = std::min(fraction(p.x, r.hue0.x, r.hue1.x), 0.9999F); break;
        case DragKind::PickerAlpha: overlay.alpha = fraction(p.x, r.alpha0.x, r.alpha1.x); break;
        default: return;
        }
        write_picker_color();
    }

    bool dragging_scale_slider() const {
        return press && press->drag == DragKind::Slider && cfg && press->target.module == cfg &&
               &cfg->settings()[static_cast<std::size_t>(press->target.setting)] == &cfg->scale;
    }

    // --- Input ------------------------------------------------------------------
    void begin_press(int button, ImVec2 p) {
        Press next;
        next.button = button;
        next.origin = p;
        if (capture) {
            capture = nullptr; // a click cancels keybind capture and is consumed
            next.swallowed = true;
            press = next;
            return;
        }
        const Hit hit = hit_test(p);
        next.target = hit;
        if (overlay.kind != OverlayKind::None && !hit.in_overlay()) {
            close_overlay(); // outside click only closes the overlay
            next.swallowed = true;
            press = next;
            return;
        }
        if (hit.kind == HitKind::None) {
            next.swallowed = true;
            press = next;
            return;
        }
        if (hit.panel >= 0 && !hit.in_overlay()) bring_to_front(hit.panel);
        if (button == 0) {
            auto& panel = panels[static_cast<std::size_t>(std::max(0, hit.panel))];
            switch (hit.kind) {
            case HitKind::Header:
                next.start_panel = {panel.layout.x, panel.layout.y};
                break;
            case HitKind::Scrollbar:
                next.drag = DragKind::Scrollbar;
                next.start_scroll = panel.scroll_target;
                break;
            case HitKind::OverlayScrollbar:
                next.drag = DragKind::OverlayScrollbar;
                next.start_scroll = overlay.scroll_target;
                break;
            case HitKind::Setting:
                if (hit.setting >= 0 && is_slider(hit.module->settings()[static_cast<std::size_t>(hit.setting)])) {
                    next.drag = DragKind::Slider;
                    apply_slider(hit, p.x);
                }
                break;
            case HitKind::PickerSV: next.drag = DragKind::PickerSV; break;
            case HitKind::PickerHue: next.drag = DragKind::PickerHue; break;
            case HitKind::PickerAlpha: next.drag = DragKind::PickerAlpha; break;
            default: break;
            }
            if (next.drag >= DragKind::PickerSV) apply_picker(next.drag, p);
        }
        press = next;
    }

    void update_press(ImVec2 p) {
        auto& current = *press;
        if (current.swallowed) return;
        const float dx = p.x - current.origin.x;
        const float dy = p.y - current.origin.y;
        if (dx * dx + dy * dy >= mt::click_slop_px * mt::click_slop_px) current.moved = true;
        switch (current.drag) {
        case DragKind::None:
            if (current.moved && current.button == 0 && current.target.kind == HitKind::Header) {
                current.drag = DragKind::Panel;
                if (overlay.panel == current.target.panel) close_overlay();
            } else {
                break;
            }
            [[fallthrough]];
        case DragKind::Panel: {
            const int index = current.target.panel;
            const PanelPoint clamped = clamp_panel_px({current.start_panel.x + dx, current.start_panel.y + dy},
                px(mt::panel_width, scale), px(mt::header_height, scale), px(mt::keep_visible_x, scale),
                frame.client_w, frame.client_h);
            if (!placed(index)) write_order();
            stored_x(index) = clamped.x / scale;
            stored_y(index) = clamped.y / scale;
            break;
        }
        case DragKind::Slider: apply_slider(current.target, p.x); break;
        case DragKind::Scrollbar: {
            auto& panel = panels[static_cast<std::size_t>(current.target.panel)];
            const auto& L = panel.layout;
            const float track = (L.view_bottom - L.view_top) - (L.thumb_bottom - L.thumb_top);
            if (track > 0.0F) {
                panel.scroll_target = std::clamp(current.start_scroll + dy * L.max_scroll / track, 0.0F, L.max_scroll);
                panel.scroll = panel.scroll_target;
            }
            break;
        }
        case DragKind::OverlayScrollbar: {
            const float view = overlay.y1 - overlay.y0 - 2.0F * px(mt::dropdown_padding, scale);
            const float track = view - (overlay.thumb_bottom - overlay.thumb_top);
            if (track > 0.0F) {
                overlay.scroll_target = std::clamp(current.start_scroll + dy * overlay.max_scroll / track, 0.0F, overlay.max_scroll);
                overlay.scroll = overlay.scroll_target;
            }
            break;
        }
        default:
            if (overlay.kind == OverlayKind::Color) apply_picker(current.drag, p);
            break;
        }
    }

    void finish_press(ImVec2 p, bool released) {
        const Press current = *press;
        press.reset();
        if (current.swallowed) return;
        switch (current.drag) {
        case DragKind::Panel:
        case DragKind::Slider:
        case DragKind::PickerSV:
        case DragKind::PickerHue:
        case DragKind::PickerAlpha:
            request_save(); // persist once, on release (never per frame)
            return;
        case DragKind::Scrollbar:
        case DragKind::OverlayScrollbar:
            return;
        case DragKind::None: break;
        }
        if (!released || current.moved) return;
        if (hit_test(p) == current.target) click(current.button, current.target);
    }

    void click(int button, const Hit& hit) {
        switch (hit.kind) {
        case HitKind::Header:
            if (button == 1) toggle_collapse(hit.panel);
            break;
        case HitKind::Module:
            if (button == 0) {
                hit.module->toggle();
                request_save();
            } else if (button == 1) {
                if (expandable(hit.module)) set_module_expanded(hit.panel, hit.module, !ui(hit.module).expanded);
            } else if (button == 2) {
                start_capture(hit.module);
            }
            break;
        case HitKind::Setting: {
            if (button != 0) break;
            if (hit.setting == keybind_row) {
                start_capture(hit.module);
                break;
            }
            auto& setting = hit.module->settings()[static_cast<std::size_t>(hit.setting)];
            if (auto* value = std::get_if<bool>(&setting.value)) {
                *value = !*value;
                request_save();
            } else if (std::holds_alternative<Choice>(setting.value) || std::holds_alternative<Color>(setting.value)) {
                const bool same = overlay.module == hit.module && overlay.setting == hit.setting;
                if (same) close_overlay();
                else open_overlay_for(hit.panel, hit.module, hit.setting);
            }
            break;
        }
        case HitKind::OverlayItem:
            if (button == 0) {
                auto& choice = overlay.module->settings()[static_cast<std::size_t>(overlay.setting)].get<Choice>();
                choice.index = hit.item;
                close_overlay();
                request_save();
            }
            break;
        default: break;
        }
    }

    void handle_wheel(const PointerInput& in) {
        const Hit hit = hit_test(in.position);
        if (hit.in_overlay()) {
            if (overlay.scrollable) {
                overlay.scroll_target = std::clamp(overlay.scroll_target - in.wheel * mt::dropdown_item_height * scale,
                                                   0.0F, overlay.max_scroll);
            }
            return;
        }
        if (hit.panel < 0) return;
        auto& panel = panels[static_cast<std::size_t>(hit.panel)];
        const float before = panel.scroll_target;
        panel.scroll_target = std::clamp(panel.scroll_target - in.wheel * mt::wheel_step * scale, 0.0F, panel.layout.max_scroll);
        if (panel.scroll_target != before && overlay.panel == hit.panel) close_overlay();
    }

    void handle_input(const PointerInput& in) {
        if (capture) {
            if (in.buttons[3].pressed) { capture->set_keybind(vk::xbutton1); capture = nullptr; request_save(); }
            else if (in.buttons[4].pressed) { capture->set_keybind(vk::xbutton2); capture = nullptr; request_save(); }
        }
        if (press) {
            const auto& button = in.buttons[static_cast<std::size_t>(press->button)];
            if (!button.down && !button.released) finish_press(in.position, false); // focus lost: keep values
        }
        if (!press && in.valid) {
            for (int b = 0; b < 3; ++b) {
                if (!in.buttons[static_cast<std::size_t>(b)].pressed) continue;
                if (in.owned_by_imgui && !capture && overlay.kind == OverlayKind::None) break;
                begin_press(b, in.position);
                break;
            }
        }
        if (press) update_press(in.position);
        if (press && in.buttons[static_cast<std::size_t>(press->button)].released) finish_press(in.position, true);

        hovered = Hit{};
        if (press && press->drag == DragKind::Scrollbar) hovered = press->target;
        else if (!press && in.valid && !in.owned_by_imgui) hovered = hit_test(in.position);
        if (in.wheel != 0.0F && in.valid && !in.owned_by_imgui) handle_wheel(in);
    }

    // --- Animation --------------------------------------------------------------
    // True when a value that feeds layout actually moved. Hover and toggle
    // colours do not, so a settled frame must not lay every panel out twice.
    bool animate(float dt) {
        bool geometry = false;
        const auto note = [&](float before, float after) { geometry = geometry || before != after; };
        for (auto& panel : panels) {
            if (!panel.present) continue;
            const int index = static_cast<int>(panel.category);
            const float expand_before = panel.expand.p;
            panel.expand.step(!stored_collapsed(index), dt, tm::panel_expand, tm::panel_expand);
            note(expand_before, panel.expand.p);
            const float scroll_before = panel.scroll;
            panel.scroll = follow(panel.scroll, panel.scroll_target, dt, tm::scroll_tau);
            note(scroll_before, panel.scroll);
            for (const Module* module : panel.modules) {
                auto& state = ui(module);
                const float row_before = state.expand.p;
                state.expand.step(state.expanded, dt, tm::settings_expand, tm::settings_expand);
                note(row_before, state.expand.p);
                state.hover = tween(state.hover, hovered.kind == HitKind::Module && hovered.module == module, dt, tm::hover);
                state.enabled = tween(state.enabled, module->enabled(), dt, tm::enabled);
                for (std::size_t i = 0; i < module->settings().size() && i < state.toggles.size(); ++i) {
                    if (const auto* b = std::get_if<bool>(&module->settings()[i].value)) {
                        state.toggles[i].step(*b, dt, tm::toggle, tm::toggle);
                    }
                }
            }
        }
        const float overlay_before = overlay.scroll;
        overlay.scroll = follow(overlay.scroll, overlay.scroll_target, dt, tm::scroll_tau);
        note(overlay_before, overlay.scroll);
        return geometry;
    }

    void skip_animations() {
        for (auto& panel : panels) {
            panel.expand.snap(!stored_collapsed(static_cast<int>(panel.category)));
            panel.scroll = panel.scroll_target;
            for (const Module* module : panel.modules) {
                auto& state = ui(module);
                state.expand.snap(state.expanded);
                state.enabled = module->enabled() ? 1.0F : 0.0F;
                for (std::size_t i = 0; i < module->settings().size() && i < state.toggles.size(); ++i) {
                    if (const auto* b = std::get_if<bool>(&module->settings()[i].value)) state.toggles[i].snap(*b);
                }
            }
        }
        overlay.scroll = overlay.scroll_target;
        fade.snap(is_open());
    }

    // --- Drawing ------------------------------------------------------------------
    void draw_backdrop(float alpha) {
        auto* list = ctx->draw_list;
        const float width = frame.client_w, height = frame.client_h;
        if (!list || width <= 0.0F || height <= 0.0F || alpha <= 0.0F) return;
        // The dim used to be two fullscreen blends (vertical wash, then a lower
        // colour). Fold both into one mesh so the framebuffer is read once.
        struct Premultiplied { float r, g, b, a; };
        const auto unpack = [](ImU32 color) {
            const auto channel = [](ImU32 value, ImU32 shift) { return static_cast<float>((value >> shift) & 0xFFu) / 255.0F; };
            const float a = channel(color, IM_COL32_A_SHIFT);
            return Premultiplied{channel(color, IM_COL32_R_SHIFT) * a, channel(color, IM_COL32_G_SHIFT) * a,
                                  channel(color, IM_COL32_B_SHIFT) * a, a};
        };
        const auto mix = [](ImU32 from, ImU32 to, float t) {
            const auto channel = [](ImU32 value, ImU32 shift) { return static_cast<int>((value >> shift) & 0xFFu); };
            const auto blend = [t](int a, int b) { return static_cast<int>(std::lround(a + (b - a) * t)); };
            return IM_COL32(blend(channel(from, IM_COL32_R_SHIFT), channel(to, IM_COL32_R_SHIFT)),
                            blend(channel(from, IM_COL32_G_SHIFT), channel(to, IM_COL32_G_SHIFT)),
                            blend(channel(from, IM_COL32_B_SHIFT), channel(to, IM_COL32_B_SHIFT)),
                            blend(channel(from, IM_COL32_A_SHIFT), channel(to, IM_COL32_A_SHIFT)));
        };
        const auto over = [](Premultiplied dst, Premultiplied src) {
            const float keep = 1.0F - src.a;
            return Premultiplied{src.r + dst.r * keep, src.g + dst.g * keep, src.b + dst.b * keep, src.a + dst.a * keep};
        };
        const auto pack = [](Premultiplied color) {
            if (color.a <= 1.0e-4F) return IM_COL32(0, 0, 0, 0);
            const float inv = 255.0F / color.a;
            const auto byte = [](float value) { return static_cast<int>(std::lround(std::clamp(value, 0.0F, 255.0F))); };
            return IM_COL32(byte(color.r * inv), byte(color.g * inv), byte(color.b * inv), byte(color.a * 255.0F));
        };
        const ImU32 top = to_u32(theme::backdrop_top, alpha);
        const ImU32 bottom = to_u32(theme::backdrop_bottom, alpha);
        const ImU32 clear = to_u32(theme::black, 0.0F);
        const ImU32 red = to_u32(theme::backdrop_red, alpha);
        const ImU32 violet = to_u32(theme::backdrop_violet, alpha);
        const auto sample = [&](float x, float y) {
            Premultiplied color = unpack(mix(top, bottom, std::clamp(y / height, 0.0F, 1.0F)));
            const float band = height * 0.35F;
            if (y >= band && height > band) {
                const float v = std::clamp((y - band) / (height - band), 0.0F, 1.0F);
                const float u = std::clamp(x / width, 0.0F, 1.0F);
                // Same corners as the old lower rect: upper clear, lower-left violet, lower-right red.
                const ImU32 left = mix(clear, violet, v);
                const ImU32 right = mix(clear, red, v);
                color = over(color, unpack(mix(left, right, u)));
            }
            return pack(color);
        };
        constexpr int rows = 16;
        if (list->_VtxCurrentIdx > 60000u) return;
        list->PrimReserve(rows * 6, (rows + 1) * 2);
        const auto base = static_cast<unsigned>(list->_VtxCurrentIdx);
        const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
        for (int row = 0; row <= rows; ++row) {
            const float y = height * static_cast<float>(row) / static_cast<float>(rows);
            list->PrimWriteVtx(ImVec2(0.0F, y), uv, sample(0.0F, y));
            list->PrimWriteVtx(ImVec2(width, y), uv, sample(width, y));
        }
        for (int row = 0; row < rows; ++row) {
            const auto i = static_cast<ImDrawIdx>(base + static_cast<unsigned>(row) * 2u);
            list->PrimWriteIdx(i);
            list->PrimWriteIdx(static_cast<ImDrawIdx>(i + 1));
            list->PrimWriteIdx(static_cast<ImDrawIdx>(i + 2));
            list->PrimWriteIdx(static_cast<ImDrawIdx>(i + 1));
            list->PrimWriteIdx(static_cast<ImDrawIdx>(i + 3));
            list->PrimWriteIdx(static_cast<ImDrawIdx>(i + 2));
        }
    }

    void draw_panel(int index, float alpha) {
        auto& panel = panels[static_cast<std::size_t>(index)];
        const auto& L = panel.layout;
        auto* list = ctx->draw_list;
        auto& fonts = ctx->fonts;
        const float s = scale;
        const float radius = mt::panel_radius * s;
        const float v = panel.expand.value();
        const ImVec2 a(L.x, L.y);
        const ImVec2 b(L.x + L.w, L.y + L.h);

        if (!cfg || cfg->panel_shadow.get<bool>()) {
            ShadowParams params{theme::shadow_panel.opacity, theme::shadow_panel.blur * 0.5F * s, 0.0F, theme::shadow_panel.offset_y * s};
            if (cfg) {
                params.opacity = cfg->shadow_opacity.get<float>();
                params.sigma = cfg->shadow_blur.get<float>() * 0.5F * s;
                params.offset_y = cfg->shadow_offset_y.get<float>() * s;
            }
            ctx->shadows.draw(list, a, b, CornerRadii::all(radius), params, alpha);
        }
        list->AddRectFilled(a, b, to_u32(theme::panel_background, alpha), radius);
        const Color accent = theme::category_accent(panel.category);
        const ImU32 header_left = to_u32(lerp(accent, theme::black, 0.12F), alpha);
        const ImU32 header_right = to_u32(accent, alpha);
        list->AddRectFilledMultiColor(a, ImVec2(b.x, L.y + L.header_h), header_left, header_right, header_right, header_left);
        draw_category_icon(list, panel.category, ImVec2(L.x + px(mt::header_icon_x, s), L.y + L.header_h * 0.5F),
                           mt::header_icon_size * s, to_u32(theme::primary_text, alpha));
        std::string title_buf;
        const auto title = fonts.ellipsize(FontRole::Header, category_name(panel.category),
            (mt::panel_width - mt::header_text_x - mt::chevron_right - mt::chevron_slot - mt::chevron_gap) * s, title_buf);
        fonts.draw(list, FontRole::Header, L.x + px(mt::header_text_x, s), fonts.centered_baseline(FontRole::Header, L.y, L.header_h),
                   to_u32(theme::primary_text, alpha), title);
        draw_chevron(list, ImVec2(L.x + L.w - (mt::chevron_right + mt::chevron_slot * 0.5F) * s, L.y + L.header_h * 0.5F), s, v,
                     to_u32(theme::primary_text, alpha * 0.7F));

        if (L.view_bottom > L.view_top) {
            list->PushClipRect(ImVec2(L.x + L.bw, L.view_top), ImVec2(b.x - L.bw, L.view_bottom), true);
            for (const auto& ml : L.modules) {
                if (ml.extra_bottom < L.view_top || ml.top > L.view_bottom) continue;
                draw_module(panel, ml, alpha);
            }
            list->PopClipRect();
        }

        if (L.scrollable) {
            const bool active = hovered.kind == HitKind::Scrollbar && hovered.panel == index;
            list->AddRectFilled(ImVec2(L.bar_left, L.thumb_top), ImVec2(L.bar_right, L.thumb_bottom),
                                to_u32(active ? accent : theme::scrollbar_thumb, alpha),
                                mt::scrollbar_radius * s);
        }
    }

    void draw_module(const Panel& panel, const ModuleLayout& ml, float alpha) {
        const auto& L = panel.layout;
        auto* list = ctx->draw_list;
        auto& fonts = ctx->fonts;
        const float s = scale;
        Module* module = ml.module;
        auto& state = ui(module);

        const Color accent = theme::category_accent(panel.category);
        const Color idle = lerp(theme::module_background, theme::module_hover, state.hover);
        const Color enabled_left = lerp(lerp(accent, theme::black, 0.28F), theme::white, state.hover * 0.08F);
        const Color enabled_right = lerp(lerp(accent, theme::black, 0.16F), theme::white, state.hover * 0.08F);
        const ImU32 left = to_u32(lerp(idle, enabled_left, state.enabled), alpha);
        const ImU32 right = to_u32(lerp(idle, enabled_right, state.enabled), alpha);
        list->AddRectFilledMultiColor(ImVec2(L.x, ml.top), ImVec2(L.x + L.w, ml.bottom), left, right, right, left);
        const float row_h = ml.bottom - ml.top;
        const float name_max = (ml.expandable ? mt::module_text_max_chevron : mt::module_text_max) * s;
        std::string name_buf;
        const auto name = fonts.ellipsize(FontRole::Module, module->name(), name_max, name_buf);
        const float name_x = L.x + px(mt::module_text_x, s);
        fonts.draw(list, FontRole::Module, name_x, fonts.centered_baseline(FontRole::Module, ml.top, row_h),
                   to_u32(lerp(theme::primary_text, theme::white, state.enabled), alpha * (0.88F + 0.12F * state.enabled)), name);
        if (ml.expandable) {
            draw_chevron(list, ImVec2(L.x + L.w - (mt::chevron_right + mt::chevron_slot * 0.5F) * s, ml.top + row_h * 0.5F), s,
                         state.expand.value(), to_u32(theme::primary_text, alpha * 0.9F));
        }
        const bool capturing = capture == module;
        const bool row_hovered = hovered.kind == HitKind::Module && hovered.module == module;
        if (capturing || (row_hovered && module->keybind() != 0)) {
            const std::string_view hint = capturing ? std::string_view("[...]") : key_name(module->keybind());
            const float right = ml.expandable ? L.x + L.w - px(mt::chevron_right + mt::chevron_slot + mt::chevron_gap, s)
                                              : L.x + L.w - px(mt::chevron_right, s);
            const float width = fonts.measure(FontRole::KeyHint, hint);
            const float left = std::round(right - width);
            if (left >= name_x + fonts.measure(FontRole::Module, name) + px(mt::control_gap, s)) {
                fonts.draw(list, FontRole::KeyHint, left, fonts.centered_baseline(FontRole::KeyHint, ml.top, row_h),
                           to_u32(capturing ? theme::keybind_waiting : theme::secondary_text, alpha), hint);
            }
        }

        if (!ml.expandable || ml.extra_bottom <= ml.bottom) return;
        list->PushClipRect(ImVec2(L.x + L.bw, ml.bottom), ImVec2(L.x + L.w - L.bw, std::min(ml.extra_bottom, L.view_bottom)), true);
        const float bx0 = L.x + px(mt::setting_inset, s);
        const float bx1 = L.x + px(mt::panel_width - mt::setting_inset, s);
        list->AddRectFilled(ImVec2(bx0, ml.block_top), ImVec2(bx1, ml.block_bottom), to_u32(theme::setting_background, alpha),
                            mt::setting_radius * s);
        const float settings_top = std::max(ml.bottom, L.view_top);
        const float settings_bottom = std::min(ml.extra_bottom, L.view_bottom);
        for (const auto& row : ml.rows) {
            if (row.bottom <= settings_top || row.top >= settings_bottom) continue;
            draw_setting(L, module, state, row, alpha);
        }
        list->PopClipRect();
    }

    void draw_setting(const PanelLayout& L, Module* module, ModuleUi& state, const SettingRow& row, float alpha) {
        auto* list = ctx->draw_list;
        auto& fonts = ctx->fonts;
        const float s = scale;
        const float label_x = L.x + px(mt::setting_inset + 1.0F + mt::setting_padding, s);
        const float right = L.x + px(mt::panel_width - mt::setting_inset - 1.0F - mt::setting_padding, s);
        const float row_h = row.bottom - row.top;
        const float gap = px(mt::control_gap, s);
        const ImU32 primary = to_u32(theme::primary_text, alpha);
        const ImU32 secondary = to_u32(theme::secondary_text, alpha);
        const Color accent = theme::category_accent(module->category());
        if (hovered.kind == HitKind::Setting && hovered.module == module && hovered.setting == row.index) {
            list->AddRectFilled(ImVec2(L.x, row.top), ImVec2(L.x + L.w, row.bottom), to_u32(theme::module_hover, alpha * 0.35F));
        }
        std::string label_buf, value_buf;
        const auto label = [&](std::string_view text, float control_left, float top, float height) {
            const auto shown = fonts.ellipsize(FontRole::Label, text, std::max(0.0F, control_left - gap - label_x), label_buf);
            fonts.draw(list, FontRole::Label, label_x, fonts.centered_baseline(FontRole::Label, top, height), primary, shown);
        };
        const auto value_text = [&](std::string_view text, float right_edge, ImU32 color, float max_width) {
            const auto shown = fonts.ellipsize(FontRole::Value, text, max_width, value_buf);
            const float x = std::round(right_edge - fonts.measure(FontRole::Value, shown));
            fonts.draw(list, FontRole::Value, x, fonts.centered_baseline(FontRole::Value, row.top, row_h), color, shown);
            return x;
        };

        if (row.index == keybind_row) {
            const bool waiting = capture == module;
            char text[64];
            if (waiting) std::snprintf(text, sizeof text, "[...]");
            else {
                const auto name = key_name(module->keybind());
                std::snprintf(text, sizeof text, "[%.*s]", static_cast<int>(name.size()), name.data());
            }
            const float x = value_text(text, right, waiting ? to_u32(accent, alpha) : secondary, right - label_x);
            label("Keybind", x, row.top, row_h);
            return;
        }
        auto& setting = module->settings()[static_cast<std::size_t>(row.index)];
        if (const auto* toggle = std::get_if<bool>(&setting.value)) {
            (void)toggle;
            const float t = state.toggles[static_cast<std::size_t>(row.index)].value();
            const float x0 = right - px(mt::toggle_width, s);
            const float y0 = row.top + std::round((row_h - px(mt::toggle_height, s)) * 0.5F);
            const float y1 = y0 + px(mt::toggle_height, s);
            list->AddRectFilled(ImVec2(x0, y0), ImVec2(right, y1), to_u32(lerp(theme::border, accent, t), alpha));
            if (t > 0.0F) {
                const ImU32 tick = to_u32(theme::white, alpha * t);
                const float cy = (y0 + y1) * 0.5F;
                list->AddLine(ImVec2(x0 + 3.0F * s, cy), ImVec2(x0 + 5.0F * s, cy + 2.0F * s), tick, 1.4F * s);
                list->AddLine(ImVec2(x0 + 5.0F * s, cy + 2.0F * s), ImVec2(right - 2.0F * s, cy - 3.0F * s), tick, 1.4F * s);
            }
            label(setting.name, x0, row.top, row_h);
        } else if (is_slider(setting)) {
            const bool integer = std::holds_alternative<int>(setting.value);
            const double value = integer ? setting.get<int>() : setting.get<float>();
            char text[64];
            if (integer) std::snprintf(text, sizeof text, "%d", setting.get<int>());
            else {
                std::snprintf(text, sizeof text, "%.*f", std::max(0, float_precision(setting.step)), value);
                const bool negative_zero = text[0] == '-' && (text[1] == '0') && (text[2] == '\0' || text[2] == '.');
                if (negative_zero) {
                    bool digits = false;
                    for (const char* cursor = text; *cursor; ++cursor) digits = digits || (*cursor >= '1' && *cursor <= '9');
                    if (!digits) std::memmove(text, text + 1, std::strlen(text));
                }
            }
            const float label_h = px(mt::slider_label_height, s);
            const float text_w = fonts.measure(FontRole::Value, text);
            const float tx = std::round(right - text_w);
            fonts.draw(list, FontRole::Value, tx, fonts.centered_baseline(FontRole::Value, row.top, label_h), secondary, text);
            label(setting.name, tx, row.top, label_h);
            const float cy = row.bottom - mt::slider_track_from_bottom * s;
            const float half = mt::slider_track_height * 0.5F * s;
            const float t = slider_fraction(value, setting.minimum, setting.maximum);
            const float knob_x = label_x + (right - label_x) * t;
            list->AddRectFilled(ImVec2(label_x, cy - half), ImVec2(right, cy + half), to_u32(theme::slider_track, alpha));
            if (knob_x > label_x) {
                list->AddRectFilled(ImVec2(label_x, cy - half), ImVec2(knob_x, cy + half), to_u32(accent, alpha));
            }
            list->AddRectFilled(ImVec2(knob_x - mt::slider_knob * 0.5F * s, cy - mt::slider_knob_height * 0.5F * s),
                                ImVec2(knob_x + mt::slider_knob * 0.5F * s, cy + mt::slider_knob_height * 0.5F * s), to_u32(accent, alpha));
        } else if (std::holds_alternative<Choice>(setting.value)) {
            const float chevron_left = right - mt::chevron_w * s;
            const bool expanded = overlay.kind == OverlayKind::Dropdown && overlay.module == module && overlay.setting == row.index;
            draw_chevron(list, ImVec2(right - mt::chevron_w * 0.5F * s, row.top + row_h * 0.5F), s, expanded ? 1.0F : 0.0F, secondary);
            const float value_right = std::round(chevron_left - gap);
            const auto& choice = std::get<Choice>(setting.value);
            const std::string_view selected = choice.index >= 0 && static_cast<std::size_t>(choice.index) < choice.options.size()
                ? std::string_view{choice.options[static_cast<std::size_t>(choice.index)]} : std::string_view{};
            const float x = value_text(selected, value_right, secondary, (value_right - label_x) * 0.6F);
            label(setting.name, x, row.top, row_h);
        } else if (const auto* color = std::get_if<Color>(&setting.value)) {
            const float x0 = right - px(mt::swatch, s);
            const float y0 = row.top + std::round((row_h - px(mt::swatch, s)) * 0.5F);
            const ImVec2 a(x0, y0);
            const ImVec2 b(right, y0 + px(mt::swatch, s));
            const float r = mt::swatch_radius * s;
            if (color->a < 1.0F) {
                list->PushClipRect(a, b, true);
                draw_checker(list, a, b, mt::checker * s, to_u32(theme::checker_light, alpha), to_u32(theme::checker_dark, alpha));
                list->PopClipRect();
            }
            list->AddRectFilled(a, b, to_u32(*color, alpha), r);
            const float half = line_px(mt::border_width, s) * 0.5F;
            list->AddRect(ImVec2(a.x + half - 0.5F, a.y + half - 0.5F), ImVec2(b.x - half + 0.5F, b.y - half + 0.5F),
                          to_u32(theme::border, alpha), r, 0, line_px(mt::border_width, s));
            label(setting.name, x0, row.top, row_h);
        } else {
            if (!warned_unsupported) {
                warned_unsupported = true;
                log_line("Setting type without a GUI control shown read-only (first: " + module->id() + "." + setting.id + ")");
            }
            const float x = value_text(setting_value_text(setting), right, secondary, (right - label_x) * 0.5F);
            label(setting.name, x, row.top, row_h);
        }
    }

    void draw_overlay(float alpha) {
        if (overlay.kind == OverlayKind::None) return;
        auto* list = ctx->draw_list;
        auto& fonts = ctx->fonts;
        const float s = scale;
        const ImVec2 a(overlay.x0, overlay.y0);
        const ImVec2 b(overlay.x1, overlay.y1);
        const float radius = mt::setting_radius * s;
        const float bw = line_px(mt::border_width, s);
        list->PushClipRect(ImVec2(0.0F, 0.0F), ImVec2(frame.client_w, frame.client_h), false);
        ShadowParams params{theme::shadow_small.opacity, theme::shadow_small.blur * 0.5F * s, 0.0F, theme::shadow_small.offset_y * s};
        ctx->shadows.draw(list, a, b, CornerRadii::all(radius), params, alpha);
        list->AddRectFilled(a, b, to_u32(theme::overlay_background, alpha), radius);

        if (overlay.kind == OverlayKind::Dropdown) {
            const auto& choice = overlay.module->settings()[static_cast<std::size_t>(overlay.setting)].get<Choice>();
            const float pad = px(mt::dropdown_padding, s);
            const float view_top = overlay.y0 + pad;
            const float view_bottom = overlay.y1 - pad;
            list->PushClipRect(ImVec2(a.x + bw, view_top), ImVec2(b.x - bw, view_bottom), true);
            const float sc = std::round(overlay.scroll);
            for (std::size_t i = 0; i < choice.options.size(); ++i) {
                const float top = view_top + px(static_cast<float>(i) * mt::dropdown_item_height, s) - sc;
                const float bottom = view_top + px(static_cast<float>(i + 1) * mt::dropdown_item_height, s) - sc;
                if (bottom < view_top || top > view_bottom) continue;
                if (hovered.kind == HitKind::OverlayItem && hovered.item == static_cast<int>(i)) {
                    list->AddRectFilled(ImVec2(a.x + bw, top), ImVec2(b.x - bw, bottom), to_u32(theme::dropdown_item_hover, alpha));
                }
                const bool selected = choice.index == static_cast<int>(i);
                std::string option_buf;
                const auto text = fonts.ellipsize(FontRole::Label, choice.options[i],
                    overlay.x1 - overlay.x0 - 2.0F * px(mt::setting_padding, s), option_buf);
                fonts.draw(list, FontRole::Label, overlay.x0 + px(mt::setting_padding, s),
                           fonts.centered_baseline(FontRole::Label, top, bottom - top),
                           to_u32(selected ? theme::category_accent(overlay.module->category()) : theme::primary_text, alpha), text);
            }
            list->PopClipRect();
            if (overlay.scrollable) {
                const bool active = (hovered.kind == HitKind::OverlayScrollbar) || (press && press->drag == DragKind::OverlayScrollbar);
                list->AddRectFilled(ImVec2(overlay.bar_left, overlay.thumb_top), ImVec2(overlay.bar_right, overlay.thumb_bottom),
                                    to_u32(active ? theme::scrollbar_thumb_active : theme::scrollbar_thumb, alpha),
                                    mt::scrollbar_radius * s);
            }
        } else {
            const auto r = picker_rects();
            const Color hue = hsv_to_rgb({overlay.hsv.h, 1.0F, 1.0F}, 1.0F);
            const ImU32 white = to_u32(theme::white, alpha);
            const ImU32 black = to_u32(theme::black, alpha);
            const ImU32 clear_black = to_u32(theme::black, 0.0F);
            list->AddRectFilledMultiColor(r.sv0, r.sv1, white, to_u32(hue, alpha), to_u32(hue, alpha), white);
            list->AddRectFilledMultiColor(r.sv0, r.sv1, clear_black, clear_black, black, black);
            const ImVec2 marker(r.sv0.x + (r.sv1.x - r.sv0.x) * overlay.hsv.s, r.sv0.y + (r.sv1.y - r.sv0.y) * (1.0F - overlay.hsv.v));
            list->AddCircle(marker, 4.0F * s, black, 16, 2.0F * s);
            list->AddCircle(marker, 4.0F * s, to_u32(theme::primary_text, alpha), 16, 1.0F * s);

            const float segment = (r.hue1.x - r.hue0.x) / 6.0F;
            for (int i = 0; i < 6; ++i) {
                const ImU32 c0 = to_u32(hsv_to_rgb({static_cast<float>(i) / 6.0F, 1.0F, 1.0F}, 1.0F), alpha);
                const ImU32 c1 = to_u32(hsv_to_rgb({static_cast<float>(i + 1) / 6.0F >= 1.0F ? 0.0F : static_cast<float>(i + 1) / 6.0F,
                                                    1.0F, 1.0F}, 1.0F), alpha);
                list->AddRectFilledMultiColor(ImVec2(r.hue0.x + segment * static_cast<float>(i), r.hue0.y),
                                              ImVec2(r.hue0.x + segment * static_cast<float>(i + 1), r.hue1.y), c0, c1, c1, c0);
            }
            const auto bar_marker = [&](const ImVec2& p0, const ImVec2& p1, float t) {
                const float x = std::round(p0.x + (p1.x - p0.x) * t);
                list->AddRectFilled(ImVec2(x - 2.0F * s, p0.y - 1.0F * s), ImVec2(x + 2.0F * s, p1.y + 1.0F * s), black);
                list->AddRectFilled(ImVec2(x - 1.0F * s, p0.y - 1.0F * s), ImVec2(x + 1.0F * s, p1.y + 1.0F * s),
                                    to_u32(theme::primary_text, alpha));
            };
            bar_marker(r.hue0, r.hue1, overlay.hsv.h);

            const Color current = hsv_to_rgb(overlay.hsv, overlay.alpha);
            list->PushClipRect(r.alpha0, r.alpha1, true);
            draw_checker(list, r.alpha0, r.alpha1, mt::checker * s, to_u32(theme::checker_light, alpha), to_u32(theme::checker_dark, alpha));
            list->PopClipRect();
            Color transparent = current;
            transparent.a = 0.0F;
            Color opaque = current;
            opaque.a = 1.0F;
            list->AddRectFilledMultiColor(r.alpha0, r.alpha1, to_u32(transparent, alpha), to_u32(opaque, alpha), to_u32(opaque, alpha),
                                          to_u32(transparent, alpha));
            bar_marker(r.alpha0, r.alpha1, overlay.alpha);

            fonts.draw(list, FontRole::Value, r.sv0.x, fonts.centered_baseline(FontRole::Value, r.hex_top, r.hex_bottom - r.hex_top),
                       to_u32(theme::secondary_text, alpha), hex_text(current));
        }
        const float half = bw * 0.5F;
        list->PathRect(ImVec2(a.x + half, a.y + half), ImVec2(b.x - half, b.y - half), std::max(0.0F, radius - half));
        list->PathStroke(to_u32(theme::overlay_border, alpha), ImDrawFlags_Closed, bw);
        list->PopClipRect();
    }

    void on_closed() {
        close_overlay();
        capture = nullptr;
        if (press) finish_press(press->origin, false);
        hovered = Hit{};
        write_order();
        request_save();
    }

    void render(GuiContext& context) {
        ctx = &context;
        resolve_cfg();
        frame = context.frame;
        scale = context.frame.ui_scale;
        sync_panels();
        if (!dragging_scale_slider()) latched_gui_scale = cfg ? cfg->scale.get<float>() : 1.0F;

        const bool open_now = is_open();
        if (was_open && !open_now) on_closed();
        was_open = open_now;
        fade.step(open_now, frame.dt, tm::gui_fade, tm::gui_fade);

        if (scale != last_scale || frame.client_w != last_w || frame.client_h != last_h || frame.dpi != last_dpi) {
            if (last_scale != 0.0F) close_overlay();
            last_scale = scale;
            last_w = frame.client_w;
            last_h = frame.client_h;
            last_dpi = frame.dpi;
        }
        if (fade.p <= 0.0F) {
            ctx = nullptr;
            return;
        }
        sync_z_order();
        update_frame_pitch();
        migrate_default_layout();
        for (const int index : z_order) layout(index);
        if (pending_overlay.first) {
            open_overlay_for(static_cast<int>(pending_overlay.first->category()), pending_overlay.first, pending_overlay.second);
            pending_overlay = {nullptr, -1};
        }
        layout_overlay();
        if (open_now) handle_input(context.pointer);
        else hovered = Hit{};
        // Input reads the layout above. Animation then moves scroll and expand,
        // so only a frame that actually moved needs the coordinates rebuilt
        // before drawing. Opening a popup still needs its own placement.
        if (animate(frame.dt)) {
            for (const int index : z_order) layout(index);
        }
        layout_overlay();

        const float alpha = fade.value();
        draw_backdrop(alpha);
        for (const int index : z_order) draw_panel(index, alpha);
        draw_overlay(alpha);

        if (open_now) {
            for (auto& window : windows) {
                if (ImGui::Begin(window.title.c_str()) && window.draw) window.draw();
                ImGui::End();
            }
            for (auto& [id, renderer] : renderers) {
                Module* module = modules.find(id);
                if (!module || !renderer || !ui(module).expanded) continue;
                const std::string title = module->name() + " Settings##medicon-" + module->id();
                if (ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) renderer(*module);
                ImGui::End();
            }
        }
        ctx = nullptr;
    }
};

ClickGui::ClickGui(ModuleManager& modules) : impl_(std::make_unique<Impl>(modules)) {}
ClickGui::~ClickGui() = default;

void ClickGui::render(gui::GuiContext& context) { impl_->render(context); }

bool ClickGui::key_event(int key, bool down, bool repeat) {
    auto& d = *impl_;
    d.resolve_cfg();
    if (key <= 0 || key > 255) return d.is_open();
    if (!down) return d.is_open();
    if (d.capture) {
        if (repeat) return true;
        if (key == vk::escape) {
            // cancel
        } else if (key == vk::back || key == vk::del) {
            d.capture->set_keybind(0);
            d.request_save();
        } else {
            d.capture->set_keybind(key); // includes the GUI open key; the GUI stays open
            d.request_save();
        }
        d.capture = nullptr;
        return true;
    }
    const int open_key = d.cfg ? d.cfg->open_key() : ClickGuiModule::default_key;
    if (key == open_key) {
        if (!repeat) d.set_open(!d.is_open());
        return d.is_open();
    }
    if (!d.is_open()) return false;
    if (key == vk::escape) {
        if (!repeat) {
            if (d.overlay.kind != OverlayKind::None) d.close_overlay();
            else d.set_open(false);
        }
        return true; // never forward Esc to the game while the GUI handles it
    }
    return true;
}

float ClickGui::effective_gui_scale() const {
    const auto& d = *impl_;
    auto* cfg = dynamic_cast<ClickGuiModule*>(d.modules.find(ClickGuiModule::module_id));
    if (!cfg) return 1.0F;
    if (d.dragging_scale_slider()) return d.latched_gui_scale;
    return cfg->scale.get<float>();
}

bool ClickGui::consume_save_request() noexcept { return std::exchange(impl_->save_requested, false); }

bool ClickGui::open() const noexcept {
    auto& d = *impl_;
    d.resolve_cfg();
    return d.is_open();
}

void ClickGui::set_open(bool value) {
    impl_->resolve_cfg();
    impl_->set_open(value);
}

bool ClickGui::capturing_keybind() const noexcept { return impl_->capture != nullptr; }

void ClickGui::add_panel(std::string title, PanelCallback draw) { impl_->windows.push_back({std::move(title), std::move(draw)}); }

void ClickGui::set_module_settings_renderer(std::string module_id, ModuleSettingsCallback draw) {
    impl_->renderers.insert_or_assign(std::move(module_id), std::move(draw));
}

void ClickGui::set_status(std::string status) {
    if (!status.empty() && status != impl_->status) log_line(status);
    impl_->status = std::move(status);
}

const std::string& ClickGui::status() const noexcept { return impl_->status; }

void ClickGui::apply_theme() {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = mt::panel_radius;
    style.ChildRounding = mt::setting_radius;
    style.FrameRounding = mt::setting_radius;
    style.PopupRounding = mt::setting_radius;
    style.ScrollbarRounding = mt::scrollbar_radius;
    style.GrabRounding = mt::setting_radius;
    style.WindowBorderSize = mt::border_width;
    const auto c = [](const Color& color) { return ImVec4(color.r, color.g, color.b, color.a); };
    auto* colors = style.Colors;
    colors[ImGuiCol_Text] = c(theme::primary_text);
    colors[ImGuiCol_TextDisabled] = c(theme::secondary_text);
    colors[ImGuiCol_WindowBg] = c(theme::panel_background);
    colors[ImGuiCol_ChildBg] = c(theme::setting_background);
    colors[ImGuiCol_PopupBg] = c(theme::overlay_background);
    colors[ImGuiCol_Border] = c(theme::border);
    colors[ImGuiCol_FrameBg] = c(theme::module_background);
    colors[ImGuiCol_FrameBgHovered] = c(theme::module_hover);
    colors[ImGuiCol_FrameBgActive] = c(theme::module_enabled);
    colors[ImGuiCol_TitleBg] = c(theme::panel_header);
    colors[ImGuiCol_TitleBgActive] = c(theme::panel_header);
    colors[ImGuiCol_TitleBgCollapsed] = c(theme::panel_header);
    colors[ImGuiCol_Button] = c(theme::module_background);
    colors[ImGuiCol_ButtonHovered] = c(theme::module_hover);
    colors[ImGuiCol_ButtonActive] = c(theme::module_enabled);
    colors[ImGuiCol_Header] = c(theme::module_enabled);
    colors[ImGuiCol_HeaderHovered] = c(theme::module_hover);
    colors[ImGuiCol_HeaderActive] = c(theme::module_enabled_hover);
    colors[ImGuiCol_CheckMark] = c(theme::accent);
    colors[ImGuiCol_SliderGrab] = c(theme::accent);
    colors[ImGuiCol_SliderGrabActive] = c(theme::primary_text);
    colors[ImGuiCol_Separator] = c(theme::border);
    colors[ImGuiCol_ScrollbarGrab] = c(theme::scrollbar_thumb);
    colors[ImGuiCol_ScrollbarGrabHovered] = c(theme::scrollbar_thumb_active);
}

void ClickGui::render_settings(Module& module) {
    bool any = false;
    for (auto& setting : module.settings()) {
        if (!setting.visible) continue;
        any = true;
        ImGui::PushID(setting.id.c_str());
        std::visit([&](auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, bool>) {
                ImGui::Checkbox(setting.name.c_str(), &value);
            } else if constexpr (std::is_same_v<T, int>) {
                int edited = value;
                if (ImGui::SliderInt(setting.name.c_str(), &edited, static_cast<int>(std::ceil(setting.minimum)),
                                     static_cast<int>(std::floor(setting.maximum)), "%d", ImGuiSliderFlags_AlwaysClamp)) {
                    const double step = std::max(1.0, static_cast<double>(setting.step));
                    const double snapped = setting.minimum + std::round((edited - setting.minimum) / step) * step;
                    value = static_cast<int>(std::clamp(std::round(snapped), std::ceil(static_cast<double>(setting.minimum)),
                                                        std::floor(static_cast<double>(setting.maximum))));
                }
            } else if constexpr (std::is_same_v<T, float>) {
                const int digits = float_precision(setting.step);
                char format[16];
                std::snprintf(format, sizeof format, "%%.%df", digits);
                if (ImGui::SliderFloat(setting.name.c_str(), &value, setting.minimum, setting.maximum, format,
                                       ImGuiSliderFlags_AlwaysClamp)) {
                    const double snapped = setting.minimum + std::round((value - setting.minimum) / setting.step) * setting.step;
                    value = static_cast<float>(std::clamp(round_to_digits(snapped, digits), static_cast<double>(setting.minimum),
                                                          static_cast<double>(setting.maximum)));
                }
            } else if constexpr (std::is_same_v<T, Choice>) {
                if (!value.options.empty()) {
                    value.index = std::clamp(value.index, 0, static_cast<int>(value.options.size()) - 1);
                    if (ImGui::BeginCombo(setting.name.c_str(), value.options[static_cast<std::size_t>(value.index)].c_str())) {
                        for (std::size_t i = 0; i < value.options.size(); ++i) {
                            if (ImGui::Selectable(value.options[i].c_str(), value.index == static_cast<int>(i))) {
                                value.index = static_cast<int>(i);
                            }
                        }
                        ImGui::EndCombo();
                    }
                }
            } else if constexpr (std::is_same_v<T, Color>) {
                float components[]{value.r, value.g, value.b, value.a};
                if (ImGui::ColorEdit4(setting.name.c_str(), components, ImGuiColorEditFlags_AlphaBar)) {
                    value = {components[0], components[1], components[2], components[3]};
                }
            } else if constexpr (std::is_same_v<T, std::string>) {
                ImGui::InputText(setting.name.c_str(), &value);
            }
        }, setting.value);
        if (!setting.description.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", setting.description.c_str());
        }
        ImGui::PopID();
    }
    if (!any) ImGui::TextDisabled("This module has no settings.");
}

void ClickGui::set_panel_position(Category category, float x_rpx, float y_rpx) {
    auto& d = *impl_;
    d.resolve_cfg();
    const int index = static_cast<int>(category);
    d.sync_panels();
    d.sync_z_order();
    if (!d.placed(index)) d.write_order();
    d.stored_x(index) = x_rpx;
    d.stored_y(index) = y_rpx;
}

void ClickGui::set_panel_collapsed(Category category, bool collapsed) {
    auto& d = *impl_;
    d.resolve_cfg();
    d.stored_collapsed(static_cast<int>(category)) = collapsed;
}

bool ClickGui::set_module_expanded(std::string_view module_id, bool expanded) {
    auto& d = *impl_;
    Module* module = d.modules.find(module_id);
    if (!module || !d.expandable(module)) return false;
    d.ui(module).expanded = expanded;
    return true;
}

bool ClickGui::open_overlay(std::string_view module_id, std::string_view setting_id) {
    auto& d = *impl_;
    Module* module = d.modules.find(module_id);
    if (!module) return false;
    for (std::size_t i = 0; i < module->settings().size(); ++i) {
        if (module->settings()[i].id != setting_id) continue;
        const auto& value = module->settings()[i].value;
        if (!std::holds_alternative<Choice>(value) && !std::holds_alternative<Color>(value)) return false;
        d.ui(module).expanded = true;
        d.pending_overlay = {module, static_cast<int>(i)};
        return true;
    }
    return false;
}

void ClickGui::set_panel_scroll(Category category, float scroll_px) {
    auto& panel = impl_->panels[static_cast<std::size_t>(category)];
    panel.scroll = panel.scroll_target = scroll_px;
}

void ClickGui::set_debug_panel_height(Category category, float rpx) {
    impl_->panels[static_cast<std::size_t>(category)].debug_height = rpx;
}

void ClickGui::finish_animations() {
    auto& d = *impl_;
    d.resolve_cfg();
    d.sync_panels();
    d.skip_animations();
}

std::vector<ClickGui::PanelRect> ClickGui::panel_rects() const {
    std::vector<PanelRect> rects;
    for (const int index : impl_->z_order) {
        const auto& L = impl_->panels[static_cast<std::size_t>(index)].layout;
        rects.push_back({static_cast<Category>(index), L.x, L.y, L.w, L.h});
    }
    return rects;
}

float ClickGui::ui_scale() const noexcept { return impl_->scale; }

bool ClickGui::overlay_open() const noexcept { return impl_->overlay.kind != OverlayKind::None; }

float ClickGui::panel_scroll_target(Category category) const noexcept {
    return impl_->panels[static_cast<std::size_t>(category)].scroll_target;
}

} // namespace medicon
