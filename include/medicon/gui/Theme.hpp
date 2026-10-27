#pragma once

// Single source of truth for GUI colours, reference-pixel metrics and timings.
// No other file may contain colour literals. Metrics are in rpx (1920x1080 at
// UI scale 1.0); multiply by the current scale when drawing.

#include "medicon/core/Setting.hpp"

#include <array>
#include <cstdint>

namespace medicon::theme {

constexpr Color rgb(std::uint32_t hex, float alpha = 1.0F) {
    return Color{static_cast<float>((hex >> 16U) & 0xFFU) / 255.0F,
                 static_cast<float>((hex >> 8U) & 0xFFU) / 255.0F,
                 static_cast<float>(hex & 0xFFU) / 255.0F, alpha};
}

// Category order follows the existing persistence IDs, from violet to vermilion.
inline constexpr std::array<Color, category_count> category_accents{
    rgb(0x9916FF), rgb(0xD51BE8), rgb(0xF016B7), rgb(0xFF286B), rgb(0xFF4028)};
constexpr Color category_accent(Category category) {
    return category_accents[static_cast<std::size_t>(category)];
}
inline constexpr Color panel_header = category_accents.front();
inline constexpr Color panel_background = rgb(0x333333);
inline constexpr Color module_background = rgb(0x363636);
inline constexpr Color module_hover = rgb(0x484848);
inline constexpr Color module_enabled = rgb(0xB317C1);
inline constexpr Color setting_background = rgb(0x2D2D2D);
inline constexpr Color border = rgb(0x474747);
inline constexpr Color primary_text = rgb(0xF5F3F7);
inline constexpr Color secondary_text = rgb(0xBDB9C1);
inline constexpr Color accent = rgb(0xF016B7);
inline constexpr Color backdrop_top = rgb(0x06060C, 0.38F);
inline constexpr Color backdrop_bottom = rgb(0x100814, 0.58F);
inline constexpr Color backdrop_violet = rgb(0x6515A7, 0.22F);
inline constexpr Color backdrop_red = rgb(0xB11C39, 0.16F);

inline constexpr Color module_enabled_hover = rgb(0xCC20DB);
inline constexpr Color scrollbar_thumb = border;
inline constexpr Color scrollbar_thumb_active = secondary_text;
inline constexpr Color toggle_track_off = border;
inline constexpr Color toggle_knob_off = secondary_text;
inline constexpr Color toggle_track_on = accent;
inline constexpr Color toggle_knob_on = primary_text;
inline constexpr Color slider_track = border;
inline constexpr Color slider_fill = accent;
inline constexpr Color slider_knob = primary_text;
inline constexpr Color dropdown_selected_text = accent;
inline constexpr Color dropdown_item_hover = module_hover;
inline constexpr Color overlay_background = setting_background;
inline constexpr Color overlay_border = border;
inline constexpr Color keybind_waiting = accent;
inline constexpr Color checker_light = secondary_text;
inline constexpr Color checker_dark = border;
inline constexpr Color shadow = rgb(0x000000);
inline constexpr Color white = rgb(0xFFFFFF);
inline constexpr Color black = rgb(0x000000);

// Preview background presets (host only; kept here so colours stay in one file).
inline constexpr Color preview_white = rgb(0xFFFFFF);
inline constexpr Color preview_gray = rgb(0x808080);
inline constexpr Color preview_black = rgb(0x000000);
inline constexpr Color preview_default = rgb(0x06060B);
// Default values of Preview-only demo module settings.
inline constexpr Color demo_outline = rgb(0x79B4F8, 0.8F);
inline constexpr Color demo_hit = rgb(0xFF4040, 0.5F);

struct ShadowPreset {
    float opacity;
    float blur;     // CSS blur radius in rpx (= 2 sigma)
    float offset_y; // rpx
};
inline constexpr ShadowPreset shadow_panel{0.38F, 14.0F, 4.0F};
inline constexpr ShadowPreset shadow_small{0.30F, 8.0F, 2.0F};
inline constexpr ShadowPreset shadow_row{0.30F, 6.0F, 1.0F};

// The Main Menu's glass plates (buttons), shared with Social through Glass.hpp.
namespace glass {
inline constexpr Color drop = rgb(0x7FCBFF, 0.14F);   // soft blue under the plate
inline constexpr Color accent = rgb(0x7FCBFF);
inline constexpr Color accent_hover = rgb(0xFFD1E3);
inline constexpr Color glow = rgb(0xBFE6FF);           // pale blue light around a plate
inline constexpr float fill_idle = 0.59F, fill_hover = 0.82F, fill_pressed = 0.88F;
inline constexpr float rim_idle = 0.65F, rim_hover = 0.95F;
inline constexpr float glow_idle = 0.35F, glow_hover = 0.75F;
} // namespace glass

// Social (spec 4.6 / 4.7), styled after the Main Menu alone: the pastel blue,
// white and pink palette, glass plates, blue light instead of black shadows,
// and dusty blues instead of grey for text.
// Sizes are reference px at a 1080 px tall window; multiply by height / 1080.
namespace social {
inline constexpr Color pink = rgb(0xFFD1E3);
inline constexpr Color blue = rgb(0x7FCBFF);
inline constexpr Color pale_blue = rgb(0xD9F2FF);
inline constexpr Color title = blue;                    // "Medicon Social", like the Main Menu labels
inline constexpr Color title_outline = rgb(0xFFFFFF, 0.95F);
inline constexpr Color heading = rgb(0x5FAEE8);
inline constexpr Color text = rgb(0x5884AE);            // body text: deep dusty blue, never grey
inline constexpr Color text_secondary = rgb(0x7D9EC2);  // descriptions and dates
inline constexpr Color error = rgb(0xEE86B0);           // soft pink, not red
// Deeper than the pastel pink so it stays legible as a badge.
inline constexpr Color alert = rgb(0xE5457E);
inline constexpr Color toast_background = rgb(0xF6FBFF, 0.88F);
inline constexpr Color toast_band_dm = blue;
inline constexpr Color toast_band_friend = pink;
inline constexpr Color toast_band_news = pale_blue;
inline constexpr Color toast_band_system = rgb(0xF5A3C4);
inline constexpr ShadowPreset shadow_toast{0.30F, 22.0F, 5.0F};
inline constexpr Color shadow_tint = rgb(0x5FA3E0);     // blue drop shadows (no black)
// Over the Main Menu gradient: the blurred back.png, then a milky veil (no dark scrim).
inline constexpr float background_picture = 0.38F;
inline constexpr Color background_veil = rgb(0xF4FAFF, 0.16F);
// back.png again inside the panel, right of the sidebar: only lightly blurred,
// under a veil that keeps text on it readable.
inline constexpr float panel_picture = 1.0F;
inline constexpr Color panel_picture_veil = rgb(0xF6FBFF, 0.12F);
// Shown for the few frames before the blurred picture exists.
inline constexpr Color background_fallback = pale_blue;
inline constexpr Color panel_fill = rgb(0xE7F4FF, 0.74F);
inline constexpr Color panel_rim = rgb(0xFFFFFF, 0.70F);
inline constexpr ShadowPreset panel_shadow{0.30F, 64.0F, 16.0F};
inline constexpr Color glow = rgb(0xBFE6FF);             // pale blue light, as theme::glass::glow
inline constexpr Color panel_glow = glow;
inline constexpr Color description = rgb(0x6A8FB7);      // the sign-in hint: a slightly darker dusty blue
inline constexpr float panel_glow_opacity = 0.75F;
inline constexpr float panel_glow_blur = 28.0F;         // CSS blur radius, reference px
inline constexpr Color sidebar_fill = rgb(0xFFFFFF, 0.30F);
inline constexpr Color divider = rgb(0xFFFFFF, 0.85F);
inline constexpr Color plate = rgb(0xF5FBFF);           // glass fill: a white with a little blue
inline constexpr Color card_fill = rgb(0xFFFFFF);
inline constexpr Color input_fill = rgb(0xFFFFFF, 0.55F);
inline constexpr Color input_border = rgb(0xA9D8FA, 0.85F);
inline constexpr Color input_focus = blue;
inline constexpr float input_glow_opacity = 0.80F;
inline constexpr Color selection = rgb(0x7FCBFF, 0.35F);
inline constexpr Color danger_accent = rgb(0xF5A3C4);
inline constexpr Color toggle_off = rgb(0xCFE6F8);
inline constexpr Color toggle_on = blue;
inline constexpr Color knob = rgb(0xFFFFFF);
inline constexpr Color knob_drop = rgb(0x7FCBFF, 0.30F);
inline constexpr Color scrollbar = rgb(0x7FCBFF, 0.45F);
inline constexpr Color online = rgb(0x7DD8A8);
inline constexpr Color connecting = rgb(0xFFCF8A);
inline constexpr Color offline = rgb(0xBFD0E2);
inline constexpr Color placeholder = rgb(0x9DBBD8);
inline constexpr Color tag_fill = rgb(0x7FCBFF, 0.25F);
inline constexpr Color icon = rgb(0x6FB8EE);
// Chat (phases 4-6).
inline constexpr Color my_name = blue;                  // my name in Global (spec 4.6: the accent colour)
inline constexpr Color bubble_mine = rgb(0xCDEBFF, 0.95F);
inline constexpr Color bubble_theirs = rgb(0xFFFFFF);
inline constexpr Color unread_badge = alert;
inline constexpr Color dialog_veil = rgb(0xF4FAFF, 0.55F);
// Change Log lines: "+ Added", "- Removed", "* Fixed" show their mark in colour.
inline constexpr Color change_added = rgb(0x2FB873);
inline constexpr Color change_removed = rgb(0xE5506A);
inline constexpr Color change_fixed = rgb(0xE0A21B);
// The OWNER tag next to an owner's name.
inline constexpr Color owner_tag = rgb(0xFFD1E3, 0.95F);
inline constexpr Color owner_text = rgb(0xD94F86);
// Typed characters start this much toward grey, slightly transparent.
inline constexpr Color typing_grey = rgb(0x8A8A8A);
inline constexpr float typing_grey_mix = 0.30F;
inline constexpr float typing_start_opacity = 0.85F;

namespace metrics {
inline constexpr float reference_height = 1080.0F;
inline constexpr float background_blur = 8.0F; // Gaussian standard deviation
inline constexpr float panel_picture_blur = 2.5F; // the panel picture's, "a little"
inline constexpr float toast_top = 16.0F;
inline constexpr float toast_height = 56.0F;
inline constexpr float toast_min_width = 360.0F;
inline constexpr float toast_max_width_ratio = 0.40F; // of the window width
inline constexpr float toast_gap = 8.0F;
inline constexpr float toast_radius = 16.0F;
inline constexpr float toast_band = 5.0F;
inline constexpr float toast_padding_left = 18.0F;   // from the card edge, past the band
inline constexpr float toast_padding_right = 16.0F;
inline constexpr float toast_line_gap = 1.0F;       // between the title and body lines
inline constexpr float toast_count_gap = 8.0F;      // between the title and "+N件"
inline constexpr float toast_slide = 20.0F;
// Social screen. Smaller and denser than spec 4.6's 70 % x 75 % panel: a fixed
// reference size (scaled like everything else), at most 92 % of the window.
inline constexpr float panel_width = 1000.0F;
inline constexpr float panel_height = 620.0F;
inline constexpr float panel_max_ratio = 0.92F;
// The sign-in screen is a small card of its own.
inline constexpr float login_panel_width = 480.0F;
inline constexpr float login_panel_height = 400.0F;
inline constexpr float login_padding = 44.0F;
inline constexpr float title_outline = 1.6F;   // white rim around "Medicon Social"
inline constexpr float panel_radius = 20.0F;
inline constexpr float panel_open_scale = 0.96F;
inline constexpr float sidebar_width = 200.0F;
inline constexpr float sidebar_padding = 10.0F;
inline constexpr float sidebar_item_height = 36.0F;
inline constexpr float sidebar_footer_height = 52.0F;
inline constexpr float item_radius = 11.0F;
inline constexpr float accent_width = 3.0F;
inline constexpr float accent_inset = 0.26F;   // of a plate's height, above and below its accent line
inline constexpr float content_padding = 20.0F;
inline constexpr float close_size = 26.0F;
inline constexpr float tab_height = 30.0F;
inline constexpr float tab_padding = 14.0F;
inline constexpr float card_padding = 14.0F;
inline constexpr float card_gap = 10.0F;
inline constexpr float card_radius = 14.0F;
inline constexpr float card_shadow_offset = 3.0F;
inline constexpr float text_gap = 4.0F;
inline constexpr float button_height = 40.0F;
inline constexpr float button_radius = 12.0F;
inline constexpr float button_glow = 16.0F;        // CSS blur radius of the hover glow
inline constexpr float button_press_scale = 0.98F;
inline constexpr float input_height = 40.0F;
inline constexpr float input_radius = 12.0F;
inline constexpr float input_padding = 12.0F;
inline constexpr float row_height = 36.0F;
inline constexpr float toggle_width = 34.0F;
inline constexpr float toggle_height = 18.0F;
inline constexpr float slider_width = 170.0F;
inline constexpr float status_dot = 7.0F;
inline constexpr float scrollbar_width = 4.0F;
inline constexpr float wheel_step = 80.0F;
inline constexpr float load_more_distance = 240.0F; // from the end of a list
inline constexpr float login_width = 360.0F;
// Chat.
inline constexpr float chat_header_height = 52.0F;
inline constexpr float chat_input_row = 60.0F;
inline constexpr float chat_group_gap = 10.0F;
inline constexpr float chat_line_gap = 3.0F;
inline constexpr float bubble_padding_x = 11.0F;
inline constexpr float bubble_padding_y = 7.0F;
inline constexpr float bubble_radius = 13.0F;
inline constexpr float bubble_max_ratio = 0.72F; // of the list width
inline constexpr float send_width = 84.0F;
inline constexpr float friend_row_height = 30.0F;
inline constexpr float badge_height = 16.0F;
} // namespace metrics

namespace timing {
inline constexpr float toast_fade_in = 0.200F;
inline constexpr float toast_fade_out = 0.200F;
inline constexpr float toast_hold_menu = 4.0F;
inline constexpr float toast_hold_game = 3.0F;
inline constexpr float toast_reflow_tau = 0.060F;   // stacked toasts sliding up
inline constexpr float panel_open = 0.220F;   // 180-250 ms, ease-out cubic
inline constexpr float panel_close = 0.140F;
inline constexpr float hover_tau = 0.060F;    // hover light and glow easing
inline constexpr float press_tau = 0.035F;    // the 0.98 press
inline constexpr float focus_glow = 0.160F;   // input focus glow fade
inline constexpr float scroll_tau = 0.050F;
inline constexpr float caret_blink = 0.53F;
} // namespace timing
} // namespace social

namespace metrics {
inline constexpr float reference_width = 1920.0F;
inline constexpr float reference_height = 1080.0F;
inline constexpr float min_scale = 0.75F;
inline constexpr float max_scale = 3.0F;

// Fonts (rpx).
inline constexpr float font_header = 20.0F;
inline constexpr float font_module = 16.0F;
inline constexpr float font_key_hint = 11.0F;
inline constexpr float font_label = 12.0F;
inline constexpr float font_value = 12.0F;

// Panels.
inline constexpr float start_x = 20.0F;
inline constexpr float start_y = 88.0F;
inline constexpr float panel_width = 190.0F;
inline constexpr float panel_gap = 12.0F;
inline constexpr float panel_wrap_step_y = 540.0F;
inline constexpr float header_height = 36.0F;
inline constexpr float header_text_x = 39.0F;
inline constexpr float header_icon_x = 18.0F;
inline constexpr float header_icon_size = 18.0F;
inline constexpr float separator = 0.0F;
inline constexpr float module_height = 32.0F;
inline constexpr float module_text_x = 10.0F;
inline constexpr float module_text_max = panel_width - 20.0F;
inline constexpr float module_text_max_chevron = panel_width - 35.0F;
inline constexpr float chevron_right = 10.0F;   // gap from the panel's right edge
inline constexpr float chevron_slot = 7.0F;     // box reserved for the rotating chevron
inline constexpr float chevron_w = 9.0F;
inline constexpr float chevron_h = 5.0F;
inline constexpr float chevron_gap = 6.0F;      // gap between text and chevron slot
inline constexpr float setting_inset = 0.0F;
inline constexpr float setting_gap_top = 2.0F;
inline constexpr float setting_gap_bottom = 4.0F;
inline constexpr float setting_height = 32.0F;
inline constexpr float setting_padding = 10.0F; // inside the setting block
inline constexpr float bottom_padding = 2.0F;
inline constexpr float panel_radius = 0.0F;
inline constexpr float setting_radius = 0.0F;
inline constexpr float border_width = 0.0F;
inline constexpr float max_height_ratio = 0.80F;
inline constexpr float scrollbar_width = 3.0F;
inline constexpr float scrollbar_margin = 2.0F;
inline constexpr float scrollbar_min_thumb = 16.0F;
inline constexpr float scrollbar_radius = 1.5F;
inline constexpr float wheel_step = 54.0F;
inline constexpr float keep_visible_x = 40.0F;

// Controls.
inline constexpr float toggle_width = 12.0F;
inline constexpr float toggle_height = 12.0F;
inline constexpr float toggle_knob = 8.0F;
inline constexpr float slider_label_height = 18.0F;
inline constexpr float slider_track_from_bottom = 8.0F;
inline constexpr float slider_track_height = 3.0F;
inline constexpr float slider_knob = 4.0F;
inline constexpr float slider_knob_height = 12.0F;
inline constexpr float swatch = 14.0F;
inline constexpr float swatch_radius = 2.0F;
inline constexpr float checker = 4.0F;
inline constexpr float dropdown_item_height = 24.0F;
inline constexpr float dropdown_padding = 4.0F;
inline constexpr int dropdown_max_items = 8;
inline constexpr float overlay_gap = 2.0F;
inline constexpr float overlay_screen_margin = 8.0F;
inline constexpr float picker_padding = 8.0F;
inline constexpr float picker_square = 124.0F;
inline constexpr float picker_bar = 10.0F;
inline constexpr float picker_spacing = 6.0F;
inline constexpr float picker_hex_line = 16.0F;
inline constexpr float control_gap = 6.0F;

// Input (physical px).
inline constexpr float click_slop_px = 3.0F;

} // namespace metrics

namespace timing {
inline constexpr float gui_fade = 0.120F;
inline constexpr float panel_expand = 0.160F;
inline constexpr float settings_expand = 0.140F;
inline constexpr float hover = 0.080F;
inline constexpr float enabled = 0.120F;
inline constexpr float toggle = 0.120F;
inline constexpr float scroll_tau = 0.050F;
inline constexpr float max_dt = 0.1F;
} // namespace timing

} // namespace medicon::theme
