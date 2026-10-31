#include <medicon/gui/MainMenu.hpp>
#include <medicon/gui/Fonts.hpp>
#include <medicon/gui/GuiContext.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace medicon::gui;
int checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
bool near(float a, float b, float epsilon = 0.001F) { return std::abs(a - b) <= epsilon; }

struct ImGuiScope {
    ImGuiScope() {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1280, 720};
        io.DeltaTime = 1.0F / 60.0F;
    }
    ~ImGuiScope() { ImGui::DestroyContext(); }
};

void test_motion_and_layout() {
    const ImVec2 size(1280, 720);
    const auto& specs = main_menu_blob_specs();
    const auto start = evaluate_main_menu(size, 0.0);
    check(specs.size() == 5, "background uses five blobs");
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const auto& spec = specs[i];
        check(spec.radius_fraction >= 0.5F && spec.radius_fraction <= 0.7F, "every radius is within the requested range");
        check(spec.period_x >= 6.0 && spec.period_x <= 12.0 && spec.period_y >= 6.0 && spec.period_y <= 12.0,
              "primary motion periods are between six and twelve seconds");
        check(near(start.blobs[i].radius, 720.0F * spec.radius_fraction), "radius scales with the short side");
    }
    std::array<double, 5> squares{};
    std::array<double, 5> distances{};
    constexpr int samples = 12000;
    for (int step = 0; step < samples; ++step) {
        const auto frame = evaluate_main_menu(size, step * 0.05);
        for (std::size_t i = 0; i < frame.blobs.size(); ++i) {
            const auto& blob = frame.blobs[i];
            const double speed = std::hypot(blob.velocity.x / size.x, blob.velocity.y / size.y);
            squares[i] += speed * speed;
            distances[i] += speed;
            check(std::isfinite(blob.center.x) && std::isfinite(blob.center.y), "motion is finite");
        }
    }
    for (std::size_t i = 0; i < squares.size(); ++i) {
        const double rms = std::sqrt(squares[i] / samples), average = distances[i] / samples;
        check(rms >= 0.063 && rms <= 0.067, "sampled RMS speed agrees with the analytical 6.5 percent target");
        check(average >= 0.05 && average <= 0.08, "mean motion speed is five to eight percent of the screen per second");
        std::cout << "blob " << i << " normalized mean=" << average * 100.0 << "%/s RMS=" << rms * 100.0 << "%/s\n";
    }
    const auto a = evaluate_main_menu(size, 19.875);
    (void)evaluate_main_menu({500, 1000}, 110.0);
    const auto b = evaluate_main_menu(size, 19.875);
    const auto doubled = evaluate_main_menu({2560, 1440}, 19.875);
    for (std::size_t i = 0; i < a.blobs.size(); ++i) {
        check(a.blobs[i].center.x == b.blobs[i].center.x && a.blobs[i].center.y == b.blobs[i].center.y,
              "elapsed-time evaluation is independent of frame history");
        check(near(a.blobs[i].center.x * 2, doubled.blobs[i].center.x) && near(a.blobs[i].radius * 2, doubled.blobs[i].radius),
              "resizing preserves normalized motion and radius");
        const auto before = evaluate_main_menu(size, 19.874), after = evaluate_main_menu(size, 19.876);
        check(near((after.blobs[i].center.x - before.blobs[i].center.x) / 0.002F, a.blobs[i].velocity.x, 0.15F) &&
              near((after.blobs[i].center.y - before.blobs[i].center.y) / 0.002F, a.blobs[i].velocity.y, 0.15F),
              "reported velocity is the derivative of position");
    }
    for (const ImVec2 dimensions : {ImVec2(1280, 720), ImVec2(720, 1280), ImVec2(3440, 1440), ImVec2(320, 180)}) {
        const auto frame = evaluate_main_menu(dimensions, 1.0);
        for (std::size_t i = 0; i < frame.buttons.size(); ++i) {
            const auto& rect = frame.buttons[i];
            check(rect.min.x > 0 && rect.max.x < dimensions.x && rect.min.y >= 0 && rect.max.y <= dimensions.y,
                  "all buttons fit inside the window after resize");
            check(near(rect.min.x, frame.buttons[0].min.x) && near(rect.max.x, frame.buttons[0].max.x),
                  "button edges are left aligned and widths are equal");
            if (i) check(rect.min.y > frame.buttons[i - 1].max.y, "buttons remain separated");
        }
    }
    check(evaluate_main_menu({std::numeric_limits<float>::infinity(), 720}, 1).logo_size == 0,
          "invalid dimensions disable drawing geometry");
    check(evaluate_main_menu(size, std::numeric_limits<double>::infinity()).blobs[0].center.x == start.blobs[0].center.x,
          "invalid time is sanitized");
}

void test_social_layout() {
    // Spec 4.6: bottom centre, 24 px (at 1080) above the bottom, the same size as the other buttons.
    for (const ImVec2 size : {ImVec2(1920, 1080), ImVec2(1440, 1080), ImVec2(2560, 1080), ImVec2(1280, 1024),
                              ImVec2(800, 450)}) {
        const auto frame = evaluate_main_menu(size, 1.0);
        const auto& social = frame.social;
        const auto& play = frame.buttons[0];
        check(near((social.min.x + social.max.x) * 0.5F, size.x * 0.5F), "Social is centred horizontally");
        check(near(social.max.y, size.y - 24.0F * size.y / 1080.0F), "24 px above the bottom edge");
        const float s = std::min(size.x / 1920.0F, size.y / 1080.0F);
        check(near(social.max.x - social.min.x, 220.0F * s) && near(social.max.y - social.min.y, 58.0F * s) &&
              social.max.x - social.min.x < play.max.x - play.min.x, "220 x 58 at 1080p, smaller than the menu buttons");
        check(social.min.x >= 0 && social.max.x <= size.x && social.min.y > frame.buttons[2].max.y,
              "inside the window and below the left column");
        check(!frame.show_social, "evaluate_main_menu does not show Social by itself");
        const auto& alts = frame.alts;
        check(alts.min.x > social.max.x && alts.max.x <= size.x, "AltManager sits right of Social, inside the window");
        check(near(alts.min.y, social.min.y) && near(alts.max.y, social.max.y), "AltManager shares Social's row");
        check(!frame.show_alts, "evaluate_main_menu does not show AltManager by itself");
    }
}

void test_alts_button() {
    ImGuiScope context;
    FontManager fonts;
    MainMenu menu;
    const ImVec2 size(1920, 1080);
    const auto layout = evaluate_main_menu(size, 0.0);
    PointerInput pointer;
    pointer.valid = true;
    pointer.position = {(layout.alts.min.x + layout.alts.max.x) * 0.5F, (layout.alts.min.y + layout.alts.max.y) * 0.5F};
    const auto frame = [&](double time, bool down, bool pressed = false, bool released = false) {
        pointer.buttons[0] = {down, pressed, released};
        menu.render_at(nullptr, fonts, size, pointer, time);
    };
    menu.set_active(true);
    frame(0.0, false);
    frame(0.1, true, true);
    frame(0.2, false, false, true);
    check(!menu.consume_alts_request() && !menu.consume_action(), "a hidden AltManager button does nothing");
    menu.set_social(true, false);
    menu.set_alts(true);
    frame(0.6, true, true);
    frame(0.7, false, false, true);
    check(menu.consume_alts_request(), "a click on AltManager is reported to the overlay");
    check(!menu.consume_social_request() && !menu.consume_alts_request() && !menu.consume_action(),
          "once, not as Social and never as a game action");
    menu.set_alts(false);
    frame(1.2, true, true);
    frame(1.3, false, false, true);
    check(!menu.consume_alts_request(), "hiding the button stops its clicks");
}

void test_social_button() {
    ImGuiScope context;
    FontManager fonts;
    MainMenu menu;
    const ImVec2 size(1920, 1080);
    const auto layout = evaluate_main_menu(size, 0.0);
    PointerInput pointer;
    pointer.valid = true;
    pointer.position = {(layout.social.min.x + layout.social.max.x) * 0.5F, (layout.social.min.y + layout.social.max.y) * 0.5F};
    const auto frame = [&](double time, bool down, bool pressed = false, bool released = false) {
        pointer.buttons[0] = {down, pressed, released};
        menu.render_at(nullptr, fonts, size, pointer, time);
    };
    menu.set_active(true);
    frame(0.0, false);
    frame(0.1, true, true);
    frame(0.2, false, false, true);
    check(!menu.consume_social_request() && !menu.consume_action(), "a hidden Social button does nothing");
    menu.set_social(true, false);
    frame(0.6, true, true);
    frame(0.7, false, false, true);
    check(menu.consume_social_request(), "a click on Social is reported to the overlay");
    check(!menu.consume_social_request() && !menu.consume_action(), "once, and never as a game action");
    menu.set_social(false, false);
    frame(1.2, true, true);
    frame(1.3, false, false, true);
    check(!menu.consume_social_request(), "hiding the button stops its clicks");
}

void test_logo_direction() {
    constexpr float wavelength = 280.0F;
    check(main_menu_logo_color(0, wavelength, 0) == IM_COL32(255, 209, 227, 255), "gradient begins in pale pink");
    check(main_menu_logo_color(wavelength * 0.5F, wavelength, 0) == IM_COL32(255, 255, 255, 255), "gradient reaches white");
    check(main_menu_logo_color(wavelength, wavelength, 0) == main_menu_logo_color(0, wavelength, 0),
          "gradient joins seamlessly at its wrap boundary");
    for (float x : {0.0F, 40.0F, 100.0F, 200.0F, 280.0F})
        check(main_menu_logo_color(x - wavelength * 0.25F, wavelength, 0.9) == main_menu_logo_color(x, wavelength, 0),
              "a fixed colour band moves right to left with elapsed time");
}

void test_actions() {
    ImGuiScope context;
    FontManager fonts;
    MainMenu menu;
    const ImVec2 size(1280, 720);
    const auto layout = evaluate_main_menu(size, 0.0);
    PointerInput pointer;
    pointer.valid = true;
    const auto move = [&](int index) {
        const auto& rect = layout.buttons[static_cast<std::size_t>(index)];
        pointer.position = {(rect.min.x + rect.max.x) * 0.5F, (rect.min.y + rect.max.y) * 0.5F};
    };
    const auto frame = [&](double time, bool down, bool pressed = false, bool released = false) {
        pointer.buttons[0] = {down, pressed, released};
        menu.render_at(nullptr, fonts, size, pointer, time);
    };
    move(0);
    frame(0, true, true);
    frame(0.1, false, false, true);
    check(!menu.consume_action(), "inactive menu cannot queue an action");
    menu.set_active(true);
    frame(0, true, true);
    frame(0.1, false, false, true);
    check(!menu.consume_action(), "a held click from the previous screen is ignored");
    frame(0.2, false);
    frame(0.3, true, true);
    check(!menu.consume_action(), "press alone does not trigger a button");
    move(1);
    frame(0.4, false, false, true);
    check(!menu.consume_action(), "press and release on different buttons do not trigger");
    move(0);
    frame(0.5, true, true);
    frame(0.6, false, false, true);
    check(menu.consume_action() == MainMenuAction::Play, "matching press and release queues Play");
    check(!menu.consume_action(), "each action is consumed exactly once");
    frame(0.7, true, true);
    frame(0.8, false, false, true);
    check(!menu.consume_action(), "fast repeated clicks are debounced");
    move(1);
    frame(1.1, true, true);
    frame(1.2, false, false, true);
    move(2);
    frame(1.7, true, true);
    frame(1.8, false, false, true);
    check(menu.consume_action() == MainMenuAction::Settings, "pending actions are never overwritten by another click");
    frame(2.2, true, true);
    frame(2.3, false, false, true);
    menu.set_active(false);
    check(!menu.consume_action(), "deactivation discards queued work");
    menu.set_active(true);
    frame(0.0, false);
    frame(0.1, true, true);
    pointer.valid = false;
    frame(0.2, false, false, true);
    pointer.valid = true;
    frame(0.3, false);
    frame(0.4, false, false, true);
    check(!menu.consume_action(), "focus loss cancels the active press");
    frame(0.5, true, true);
    frame(0.6, false, false, true);
    check(menu.consume_action() == MainMenuAction::Quit, "reactivated menu accepts a fresh Quit click");
}

std::vector<ImDrawVert> draw_frame(const FontManager& fonts, ImVec2 size, double elapsed) {
    ImGui::GetIO().DisplaySize = size;
    ImGui::NewFrame();
    auto* list = ImGui::GetForegroundDrawList();
    draw_main_menu(list, fonts, size, elapsed);
    std::vector<ImDrawVert> vertices(list->VtxBuffer.begin(), list->VtxBuffer.end());
    check(vertices.size() > 400 && vertices.size() < 8192,
          "soft background is one screen grid inside a 16-bit batch");
    for (const auto& vertex : vertices)
        check(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y), "mesh coordinates remain finite");
    bool covers_far = false;
    for (const auto& vertex : vertices)
        if (vertex.pos.x == size.x && vertex.pos.y == size.y) covers_far = true;
    check(vertices.front().pos.x == 0.0F && vertices.front().pos.y == 0.0F &&
              (vertices.front().col & IM_COL32_A_MASK) == IM_COL32_A_MASK && covers_far,
          "background grid is opaque and covers the screen");
    for (const auto& command : list->CmdBuffer) check(!command.UserCallback, "menu does not install blend-state or resource callbacks");
    ImGui::Render();
    return vertices;
}

void test_drawing() {
    ImGuiScope context;
    FontManager fonts;
    fonts.set_owns_atlas(true);
    FontSizes sizes;
    const auto layout = evaluate_main_menu({1280, 720}, 0);
    sizes.main_menu_logo = layout.logo_size;
    sizes.main_menu_button = layout.button_text_size;
    fonts.prepare(*ImGui::GetIO().Fonts, sizes);
    const auto logo_face = fonts.face(FontRole::MainMenuLogo);
    check(logo_face.font && std::string(logo_face.font->GetDebugName()).find("dynapuff") != std::string::npos,
          "menu uses the existing bundled DynaPuff font");
    check(near(logo_face.font->FontSize, sizes.main_menu_logo), "large logo glyphs are rasterized at their requested size");
    for (const char glyph : std::string("Medicon"))
        check(logo_face.font->FindGlyphNoFallback(static_cast<ImWchar>(glyph)) != nullptr, "all Medicon glyphs are present");
    check(logo_face.font->FindGlyphNoFallback('Z') == nullptr, "large logo atlas omits unrelated glyphs");
    const auto first = draw_frame(fonts, {1280, 720}, 0.0);
    const auto later = draw_frame(fonts, {1280, 720}, 1.5);
    check(first.size() == later.size() && !std::equal(first.begin(), first.end(), later.begin(), [](const auto& a, const auto& b) {
              return a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.col == b.col;
          }), "motion changes the composition while retaining reusable geometry topology");
    draw_frame(fonts, {720, 1280}, 21.0);
    const auto repeated = draw_frame(fonts, {1280, 720}, 0.0);
    check(first.size() == repeated.size() && std::equal(first.begin(), first.end(), repeated.begin(), [](const auto& a, const auto& b) {
              return a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.col == b.col;
          }), "drawing is deterministic after unrelated frames and resizes");
    check(!fonts.prepare(*ImGui::GetIO().Fonts, sizes), "stable menu frames do not rebuild the font atlas");
}
} // namespace

int main() {
    const std::pair<const char*, std::function<void()>> tests[]{
        {"motion and layout", test_motion_and_layout}, {"logo direction", test_logo_direction},
        {"actions", test_actions}, {"drawing", test_drawing},
        {"social layout", test_social_layout}, {"social button", test_social_button}, {"alts button", test_alts_button},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cout << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << checks << " checks, " << failures << " failed test group(s)\n";
    return failures == 0 ? 0 : 1;
}
