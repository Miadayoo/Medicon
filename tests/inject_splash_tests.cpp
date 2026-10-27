#include <medicon/gui/Fonts.hpp>
#include <medicon/gui/InjectSplash.hpp>
#include <medicon/gui/Overlay.hpp>
#include <medicon/core/ModuleManager.hpp>
#include <medicon/modules/BuiltinModules.hpp>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace medicon::gui;

int checks = 0;
void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
bool near(float value, float expected, float tolerance = 0.00001F) {
    return std::fabs(value - expected) <= tolerance;
}
std::array<float, 10> values(const InjectSplashFrame& frame) {
    return {frame.slide, frame.text_scale, frame.text_alpha, frame.text_shake.x, frame.text_shake.y,
            frame.image_scale, frame.image_alpha, frame.scale, frame.alpha, frame.finished ? 1.0F : 0.0F};
}

void test_timeline() {
    check(std::fabs(INJECT_SPLASH_DURATION - 2.00) <= 0.00001,
          "injection splash lasts 2 seconds in total");
    const auto start = evaluate_inject_splash(0.0);
    check(!start.finished && start.slide == 0.0F && start.text_alpha == 0.0F && start.text_scale == 1.0F,
          "initial title is hidden below the screen at its normal size");
    check(start.image_scale == 0.0F && start.image_alpha == 0.0F && start.scale == 1.0F && start.alpha == 1.0F,
          "initial background is visible while the picture is hidden");

    const auto sliding = evaluate_inject_splash(SLIDE_IN_DURATION * 0.5);
    check(sliding.slide > 0.5F && sliding.slide < 1.0F && sliding.text_alpha > 0.0F && sliding.text_alpha < 1.0F,
          "slide eases toward the center and fades in before arrival");
    check(sliding.text_scale == 1.0F && sliding.image_scale == 0.0F,
          "pop and picture wait for the title to arrive");

    const auto arrival = evaluate_inject_splash(SLIDE_IN_DURATION);
    check(near(arrival.slide, 1.0F) && near(arrival.text_alpha, 1.0F) && near(arrival.text_scale, 1.0F),
          "title reaches the center at the slide boundary without a scale jump");
    check(near(arrival.image_scale, 0.0F) && near(arrival.image_alpha, 0.0F),
          "picture starts expanding at title arrival");

    const auto popped = evaluate_inject_splash(SLIDE_IN_DURATION + 0.25);
    check(near(popped.text_scale, 1.6F) && popped.image_scale > 0.9F && popped.image_alpha > 0.0F && popped.image_alpha < 1.0F,
          "title finishes its 0.25-second pop while the picture is still fading in");
    bool overshot = false;
    for (int i = 1; i < 25; ++i) overshot = overshot || evaluate_inject_splash(SLIDE_IN_DURATION + i * 0.01).text_scale > 1.6F;
    check(overshot, "pop briefly overshoots the final title size");

    const auto settled = evaluate_inject_splash(SLIDE_IN_DURATION + 0.45);
    check(near(settled.text_scale, 1.6F) && near(settled.image_scale, 1.0F) && near(settled.image_alpha, 1.0F),
          "both title and picture settle at their held sizes");
    check(settled.text_shake.x == 0.0F && settled.text_shake.y == 0.0F,
          "shake ends exactly at the center after 0.45 seconds");
    const auto hold_end = evaluate_inject_splash(SLIDE_IN_DURATION + HOLD_DURATION);
    check(!hold_end.finished && near(hold_end.scale, 1.0F) && near(hold_end.alpha, 1.0F),
          "collapse waits until the hold duration after arrival");
    const auto collapsing = evaluate_inject_splash(SLIDE_IN_DURATION + HOLD_DURATION + COLLAPSE_DURATION * 0.5);
    check(!collapsing.finished && collapsing.scale > 0.0F && collapsing.scale < 1.0F &&
              collapsing.alpha > 0.0F && collapsing.alpha < 1.0F,
          "the full composition shrinks and fades during collapse");
    const auto done = evaluate_inject_splash(INJECT_SPLASH_DURATION);
    check(done.finished && done.scale == 0.0F && done.alpha == 0.0F,
          "splash completely disappears at the total duration");
}

void test_frame_independence() {
    const double pop_time = SLIDE_IN_DURATION + 0.187;
    const auto expected = evaluate_inject_splash(pop_time);
    // Visiting time in another order simulates frame drops, retries and captures.
    for (double seconds : {INJECT_SPLASH_DURATION - 0.01, 0.0, 0.016, SLIDE_IN_DURATION + 0.45,
                           SLIDE_IN_DURATION + HOLD_DURATION, 0.1, 1000.0}) evaluate_inject_splash(seconds);
    const auto actual = evaluate_inject_splash(pop_time);
    check(values(expected) == values(actual), "elapsed time alone determines the complete animation frame");
    check(values(evaluate_inject_splash(-10.0)) == values(evaluate_inject_splash(0.0)),
          "negative elapsed time clamps to the first frame");
    check(values(evaluate_inject_splash(std::numeric_limits<double>::quiet_NaN())) == values(evaluate_inject_splash(0.0)),
          "an invalid elapsed time cannot introduce NaN geometry");
    check(evaluate_inject_splash(std::numeric_limits<double>::infinity()).finished,
          "elapsed time past the end completes immediately");

    bool shook = false;
    const int sample_end = static_cast<int>(std::ceil(INJECT_SPLASH_DURATION * 1000.0)) + 150;
    for (int millisecond = -50; millisecond <= sample_end; ++millisecond) {
        const double seconds = millisecond / 1000.0;
        const auto frame = evaluate_inject_splash(seconds);
        const auto components = values(frame);
        check(std::all_of(components.begin(), components.end(), [](float value) { return std::isfinite(value); }),
              "all frame values remain finite");
        check(frame.slide >= 0.0F && frame.slide <= 1.0F && frame.text_alpha >= 0.0F && frame.text_alpha <= 1.0F &&
                  frame.image_alpha >= 0.0F && frame.image_alpha <= 1.0F && frame.alpha >= 0.0F && frame.alpha <= 1.0F &&
                  frame.scale >= 0.0F && frame.scale <= 1.0F,
              "progress, opacity and collapse scale stay in their valid ranges");
        if (seconds >= SLIDE_IN_DURATION && seconds < SLIDE_IN_DURATION + 0.45) {
            const float envelope = 8.0F * static_cast<float>(1.0 - (seconds - SLIDE_IN_DURATION) / 0.45);
            check(std::fabs(frame.text_shake.x) <= envelope + 0.0001F && std::fabs(frame.text_shake.y) <= envelope + 0.0001F,
                  "shake remains inside its linearly decaying eight-pixel envelope");
            shook = shook || std::fabs(frame.text_shake.x) > 1.0F;
        } else {
            check(frame.text_shake.x == 0.0F && frame.text_shake.y == 0.0F,
                  "shake is absent outside its allotted time");
        }
    }
    check(shook, "title visibly shakes during the pop");
}

struct ImGuiScope {
    ImGuiScope() {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 720);
        io.DeltaTime = 1.0F / 60.0F;
    }
    ~ImGuiScope() { ImGui::DestroyContext(); }
};

std::uint64_t check_assets(FontManager& fonts) {
    const auto face = fonts.face(FontRole::Splash);
    check(face.font && std::string(face.font->GetDebugName()).find("kawai") != std::string::npos,
          "splash uses the bundled kawai face");
    for (const ImWchar codepoint : {0x30E1, 0x30B8, 0x30B3, 0x30F3}) {
        const auto* glyph = face.font->FindGlyphNoFallback(codepoint);
        check(glyph && glyph->Visible && glyph->AdvanceX > 0.0F, "all four Japanese title glyphs exist in kawai");
    }
    const auto image = fonts.splash_image();
    check(image.valid && image.size.x > 100.0F && image.size.y > 100.0F,
          "the supplied mez picture decodes to a useful full-color image");
    check(image.uv_min.x >= 0.0F && image.uv_min.y >= 0.0F && image.uv_max.x <= 1.0F && image.uv_max.y <= 1.0F &&
              image.uv_min.x < image.uv_max.x && image.uv_min.y < image.uv_max.y,
          "picture UVs identify a nonempty rectangle in the font atlas");
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const int left = static_cast<int>(std::lround(image.uv_min.x * width));
    const int top = static_cast<int>(std::lround(image.uv_min.y * height));
    const int right = static_cast<int>(std::lround(image.uv_max.x * width));
    const int bottom = static_cast<int>(std::lround(image.uv_max.y * height));
    check(right - left == static_cast<int>(image.size.x) && bottom - top == static_cast<int>(image.size.y),
          "picture UVs preserve the source dimensions and aspect ratio");
    std::uint64_t hash = 14695981039346656037ULL;
    bool has_purple = false, has_white = false;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const auto* pixel = pixels + (static_cast<std::size_t>(y) * width + x) * 4;
            for (int channel = 0; channel < 4; ++channel) hash = (hash ^ pixel[channel]) * 1099511628211ULL;
            has_purple = has_purple || (pixel[0] > 35 && pixel[2] > pixel[1] + 20 && pixel[3] > 200);
            has_white = has_white || (pixel[0] > 230 && pixel[1] > 230 && pixel[2] > 230 && pixel[3] > 200);
        }
    }
    check(has_purple && has_white, "atlas retains the supplied picture's purple packaging and white lettering");
    return hash;
}

void test_atlas_rebuilds() {
    for (bool owns_atlas : {true, false}) {
        ImGuiScope context;
        FontManager fonts;
        fonts.set_owns_atlas(owns_atlas);
        auto& atlas = *ImGui::GetIO().Fonts;
        ImFont* host_font = owns_atlas ? nullptr : atlas.AddFontDefault();
        FontSizes sizes;
        check(fonts.prepare(atlas, sizes), "first prepare builds splash assets");
        const auto image_hash = check_assets(fonts);
        check(!fonts.prepare(atlas, sizes), "unchanged splash assets do not rebuild the atlas every frame");
        sizes.splash = 95.5F;
        check(fonts.prepare(atlas, sizes), "a resized splash face rebuilds the atlas");
        check(check_assets(fonts) == image_hash, "picture pixels and UV mapping survive a font-size rebuild");
        fonts.add_text("Additional glyphs: \xCE\xA9");
        check(fonts.prepare(atlas, sizes), "new GUI glyphs rebuild the shared atlas");
        check(check_assets(fonts) == image_hash, "picture pixels survive an unrelated glyph rebuild");
        if (host_font) check(std::find(atlas.Fonts.begin(), atlas.Fonts.end(), host_font) != atlas.Fonts.end(),
                             "splash updates preserve host-owned font objects");
    }
}

std::vector<ImDrawVert> draw_frame(const FontManager& fonts, ImVec2 size, double seconds) {
    ImGui::GetIO().DisplaySize = size;
    ImGui::NewFrame();
    auto* list = ImGui::GetForegroundDrawList();
    draw_inject_splash(list, fonts, size, seconds);
    std::vector<ImDrawVert> vertices(list->VtxBuffer.begin(), list->VtxBuffer.end());
    for (const auto& vertex : vertices) {
        check(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y) && std::isfinite(vertex.uv.x) && std::isfinite(vertex.uv.y),
              "drawing emits finite coordinates after resize and during collapse");
    }
    ImGui::Render();
    return vertices;
}

void check_image_placement(const std::vector<ImDrawVert>& vertices, const SplashImage& image, ImVec2 display_size) {
    ImVec2 minimum(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    ImVec2 maximum(-minimum.x, -minimum.y);
    int image_vertices = 0;
    for (const auto& vertex : vertices) {
        if (vertex.uv.x >= image.uv_min.x && vertex.uv.x <= image.uv_max.x &&
            vertex.uv.y >= image.uv_min.y && vertex.uv.y <= image.uv_max.y) {
            minimum.x = std::min(minimum.x, vertex.pos.x);
            minimum.y = std::min(minimum.y, vertex.pos.y);
            maximum.x = std::max(maximum.x, vertex.pos.x);
            maximum.y = std::max(maximum.y, vertex.pos.y);
            ++image_vertices;
        }
    }
    check(image_vertices == 4, "picture is drawn once as its own quad");
    const float width = maximum.x - minimum.x, height = maximum.y - minimum.y;
    const float extent = std::max(width, height) / std::min(display_size.x, display_size.y);
    check(extent >= 0.40F && extent <= 0.50F, "picture stays between 40 and 50 percent of the window's short side");
    check(near(width / height, image.size.x / image.size.y, 0.001F), "resize preserves the picture's aspect ratio");
    check(near((minimum.x + maximum.x) * 0.5F, display_size.x * 0.5F, 0.001F) &&
              near((minimum.y + maximum.y) * 0.5F, display_size.y * 0.5F, 0.001F),
          "picture remains centered after resize");
}

void test_drawing() {
    ImGuiScope context;
    FontManager fonts;
    fonts.set_owns_atlas(true);
    fonts.prepare(*ImGui::GetIO().Fonts, FontSizes{});
    const auto initial = draw_frame(fonts, ImVec2(1280, 720), 0.0);
    check(!initial.empty(), "background is rendered from the first frame");
    const auto sliding = draw_frame(fonts, ImVec2(1280, 720), SLIDE_IN_DURATION * 0.5);
    check(sliding.size() >= initial.size() && !std::equal(initial.begin(), initial.end(), sliding.begin(), [](const auto& a, const auto& b) {
              return a.col == b.col;
          }), "background gradient moves while the title slides in");
    const double hold_time = SLIDE_IN_DURATION + 0.60;
    const auto held = draw_frame(fonts, ImVec2(1280, 720), hold_time);
    check(!held.empty(), "expanded composition draws during the hold");
    check_image_placement(held, fonts.splash_image(), ImVec2(1280, 720));
    const auto portrait = draw_frame(fonts, ImVec2(720, 1280), hold_time);
    check(!portrait.empty(), "composition draws after switching to a portrait window");
    check_image_placement(portrait, fonts.splash_image(), ImVec2(720, 1280));
    const auto collapse = draw_frame(fonts, ImVec2(1920, 1080), INJECT_SPLASH_DURATION - COLLAPSE_DURATION * 0.25);
    check(!collapse.empty(), "shrinking composition continues drawing before completion");
    check(draw_frame(fonts, ImVec2(1920, 1080), INJECT_SPLASH_DURATION).empty(), "finished splash emits no geometry");
    const auto retried = draw_frame(fonts, ImVec2(1280, 720), hold_time);
    check(held.size() == retried.size() && std::equal(held.begin(), held.end(), retried.begin(), [](const auto& a, const auto& b) {
              return a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.uv.x == b.uv.x && a.uv.y == b.uv.y && a.col == b.col;
          }), "drawing at the same elapsed time is independent of earlier frames and resizes");
}

void test_menu_prewarm() {
    for (const bool owns_atlas : {true, false}) {
        for (const ImVec2 size : {ImVec2(1280, 720), ImVec2(3840, 2160)}) {
            ImGuiScope context;
            ImGui::GetIO().DisplaySize = size;
            medicon::ModuleManager modules;
            medicon::register_builtin_modules(modules);
            Overlay overlay(modules);
            overlay.set_owns_font_atlas(owns_atlas);
            overlay.set_dpi_override(96.0F);
            overlay.start_splash();
            check(overlay.prepare_frame(), "startup prepares the shared texture before animation begins");

            float logo_size = 0.0F, button_size = 0.0F;
            main_menu_text_sizes(size, logo_size, button_size);
            const auto logo = overlay.fonts().face(FontRole::MainMenuLogo);
            const auto button = overlay.fonts().face(FontRole::MainMenuButton);
            check(near(logo.size, logo_size) && logo.font->FindGlyphNoFallback('M'),
                  "menu logo is rasterized at the window's full resolution before the first splash frame");
            check(near(button.size, button_size) && button.font->FindGlyphNoFallback('P'),
                  "menu buttons are already rasterized before the first splash frame");
            check_assets(overlay.fonts());
            check(!overlay.main_menu().active(), "prewarming does not display or activate the menu");
            overlay.set_main_menu_active(true);
            check(!overlay.prepare_frame(), "late menu detection cannot rebuild or upload the shared texture");
            check(overlay.fonts().face(FontRole::MainMenuLogo).font == logo.font,
                  "late menu detection reuses the prewarmed logo face");
        }
    }
}

void test_menu_transition() {
    ImGuiScope context;
    medicon::ModuleManager modules;
    medicon::register_builtin_modules(modules);
    Overlay overlay(modules);
    overlay.set_owns_font_atlas(true);
    overlay.set_dpi_override(96.0F);
    overlay.start_splash();

    int atlas_rebuilds = 0;
    const auto frame = [&]() {
        if (overlay.prepare_frame()) ++atlas_rebuilds;
        ImGui::NewFrame();
        overlay.render();
        const auto* list = ImGui::GetForegroundDrawList();
        std::vector<ImDrawVert> vertices(list->VtxBuffer.begin(), list->VtxBuffer.end());
        ImGui::Render();
        return vertices;
    };
    // The intro mesh starts at the window center; the menu background at the
    // origin. The intro must be drawn after (on top of) a detected menu.
    const auto layered = [](const std::vector<ImDrawVert>& vertices) {
        const auto splash = std::find_if(vertices.begin(), vertices.end(), [](const ImDrawVert& vertex) {
            return near(vertex.pos.x, 640.0F) && near(vertex.pos.y, 360.0F);
        });
        return !vertices.empty() && vertices.front().pos.x == 0.0F && vertices.front().pos.y == 0.0F &&
               splash != vertices.end() && splash != vertices.begin();
    };
    const auto first = frame();
    check(!first.empty() && near(first.front().pos.x, 640.0F) && near(first.front().pos.y, 360.0F),
          "the intro alone is drawn before a title screen is detected");
    const auto start = std::chrono::steady_clock::now();
    // Detection normally precedes the first frame, but can arrive later.
    // It must not end the intro or discard its Japanese font.
    overlay.set_main_menu_active(true);
    overlay.prepare_frame();
    const auto title = overlay.fonts().face(FontRole::Splash);
    check(title.size > 0.0F && std::string(title.font->GetDebugName()).find("kawai") != std::string::npos,
          "detecting the main menu during injection preserves the splash font");
    const auto intro = frame();
    check(overlay.splash_active() && layered(intro),
          "a detected main menu is drawn underneath the running startup animation");
    // A full click on the hidden Play button (1280x720 layout: x 74-384,
    // y 324-386) must not reach the menu while the intro covers it.
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(200.0F, 355.0F);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
    check(!overlay.main_menu().consume_action(), "a click during the startup animation never presses the hidden menu");

    const auto held_at = start + std::chrono::duration<double>(SLIDE_IN_DURATION + HOLD_DURATION * 0.5);
    std::this_thread::sleep_until(held_at);
    const auto held = frame();
    check(overlay.splash_active(), "startup remains visible for its configured central hold");
    check_image_placement(held, overlay.fonts().splash_image(), {1280, 720});

    const auto collapse_at = start + std::chrono::duration<double>(SLIDE_IN_DURATION + HOLD_DURATION + COLLAPSE_DURATION * 0.5);
    std::this_thread::sleep_until(collapse_at);
    const auto collapsing = frame();
    check(overlay.splash_active() && layered(collapsing),
          "the startup collapse reveals the Medicon menu, never the vanilla screen");

    std::this_thread::sleep_until(start + std::chrono::duration<double>(INJECT_SPLASH_DURATION + 0.05));
    const auto menu = frame();
    check(!overlay.splash_active() && overlay.main_menu().active() && !overlay.gui().open(),
          "completed startup reveals the detected menu without opening ClickGUI");
    check(!menu.empty() && menu.front().pos.x == 0.0F && menu.front().pos.y == 0.0F,
          "main-menu background appears after the full startup duration");
    check(overlay.fonts().face(FontRole::MainMenuLogo).size > 0.0F,
          "the menu font is prepared for the transition after startup");
    std::cout << "startup to menu atlas builds: " << atlas_rebuilds << '\n';
    check(atlas_rebuilds == 1,
          "startup builds its shared font texture once, with no rebuild or GPU upload at menu handoff");
}

} // namespace

int main() {
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"timeline", test_timeline},
        {"frame independence", test_frame_independence},
        {"atlas rebuilds", test_atlas_rebuilds},
        {"drawing", test_drawing},
        {"menu font prewarm", test_menu_prewarm},
        {"startup to main menu", test_menu_transition},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << checks << " checks, " << failures << " failed test group(s)\n";
    return failures == 0 ? 0 : 1;
}
