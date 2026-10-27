#include <medicon/core/ConfigStore.hpp>
#include <medicon/core/ModuleManager.hpp>
#include <medicon/gui/ArrayList.hpp>
#include <medicon/gui/EdgeLit.hpp>
#include <medicon/gui/GuiMath.hpp>
#include <medicon/gui/Overlay.hpp>
#include <medicon/gui/Shadow.hpp>
#include <medicon/modules/BuiltinModules.hpp>
#include <medicon/modules/ClientModules.hpp>
#include <medicon/modules/ExampleModule.hpp>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {

using namespace medicon;
using namespace medicon::gui;
using Json = nlohmann::json;

int checks = 0;
void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
bool close_to(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("medicon-gui-test-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() { std::filesystem::create_directories(path); }
    ~TempDir() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

class Named final : public Module {
public:
    Named(std::string id, std::string name, Category category = Category::Misc)
        : Module(std::move(id), std::move(name), "", category) {}
    Named(std::string id, std::string name, std::vector<Setting> settings)
        : Module(std::move(id), std::move(name), "", Category::Visual) {
        for (auto& s : settings) add_setting(std::move(s));
    }
};

// ImGui context with the GUI fonts, as the Preview sets it up.
struct ImGuiScope {
    explicit ImGuiScope(float w = 1920.0F, float h = 1080.0F) {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(w, h);
        io.DeltaTime = 1.0F / 60.0F;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~ImGuiScope() { ImGui::DestroyContext(); }
    void frame(Overlay& overlay) {
        overlay.prepare_frame();
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ImGui::NewFrame();
        overlay.render();
        ImGui::Render();
    }
};

// --- 9.1 Scale ---------------------------------------------------------------------
void test_scale() {
    struct Case { float w, h, dpi, expected; };
    const Case cases[] = {
        {1920, 1080, 96, 1.0F}, {2560, 1440, 96, 1.3333F}, {1920, 1080, 144, 1.5F},
        {3840, 2160, 144, 2.0F}, {3840, 2160, 192, 2.0F}, {1280, 720, 96, 1.0F},
    };
    for (const auto& c : cases) {
        const float s = ui_scale(c.w, c.h, c.dpi, 1.0F);
        check(close_to(s, c.expected, 0.001), "ui_scale " + std::to_string(c.w) + "x" + std::to_string(c.h) + "@" +
                                              std::to_string(c.dpi) + " = " + std::to_string(s));
    }
    check(close_to(ui_scale(1920, 1080, 96, 0.5F), 0.75, 1e-6), "scale clamps to 0.75");
    check(close_to(ui_scale(3840, 2160, 192, 2.0F), 3.0, 1e-6), "scale clamps to 3.0");
    check(px(10.0F, 1.5F) == 15.0F && line_px(1.0F, 1.4F) == 1.0F && line_px(1.0F, 0.75F) == 1.0F, "pixel rounding");
}

// --- 9.1 Slider ----------------------------------------------------------------------
void test_slider() {
    check(float_precision(0.1F) == 1 && float_precision(0.05F) == 2 && float_precision(1.0F) == 0 &&
          float_precision(0.01F) == 2 && float_precision(5.0F) == 0, "float precision from step");
    check(close_to(slider_value(15, 15, 135, 0, 10, 0.1, false, 1), 0.0, 1e-9), "slider left edge");
    check(close_to(slider_value(135, 15, 135, 0, 10, 0.1, false, 1), 10.0, 1e-9), "slider right edge");
    check(close_to(slider_value(500, 15, 135, 0, 10, 0.1, false, 1), 10.0, 1e-9), "slider clamps right");
    check(close_to(slider_value(-50, 15, 135, 0, 10, 0.1, false, 1), 0.0, 1e-9), "slider clamps left");
    const double mid = slider_value(75, 15, 135, 1, 10, 0.1, false, 1);
    check(close_to(mid, 5.5, 1e-9) && format_number(mid, 1) == "5.5", "slider midpoint snaps to step");
    const double third = slider_value(55, 15, 135, 0, 1, 0.05, false, 2);
    check(close_to(third, 0.35, 1e-9) && format_number(third, 2) == "0.35", "slider 0.05 step (no 0.30000000000000004)");
    const double integer = slider_value(70, 15, 135, 0, 24000, 100, true, 0);
    check(integer == 11000.0, "int slider snaps to step 100: " + std::to_string(integer));
    check(slider_value(80, 15, 135, 90, 180, 90, true, 0) == 180.0, "int slider two-value step");
    check(format_number(-0.0001, 2) == "0.00", "no negative zero");
    check(close_to(slider_fraction(5, 0, 10), 0.5, 1e-6) && slider_fraction(5, 5, 5) == 0.0F, "slider fraction");
}

// --- Animation primitives ------------------------------------------------------------
void test_animation_primitives() {
    Reversible r;
    r.step(true, 0.08F, 0.16F, 0.16F);
    const float half = r.value();
    r.step(false, 0.0F, 0.16F, 0.16F);
    check(r.value() == half, "reversing keeps progress");
    check(close_to(ease_out_cubic(0.5F), 0.875, 1e-6), "easeOutCubic");
    check(clamp_dt(5.0) == 0.1F && clamp_dt(-1.0) == 0.0F, "dt clamp");
}

// --- 9.1 ArrayList sort ----------------------------------------------------------------
void test_arraylist_sort() {
    ArrayListAnimator::Row a, b, c;
    a.width = 50.0F; a.order = 2;
    b.width = 50.0F; b.order = 1;
    c.width = 80.0F; c.order = 3;
    check(ArrayListAnimator::before(b, a, true) && !ArrayListAnimator::before(a, b, true), "equal width -> registration order");
    check(ArrayListAnimator::before(c, a, true), "wider first");
    check(ArrayListAnimator::before(b, c, false), "registration order when width sort is off");
    a.width = 50.000F; a.order = 0;
    b.width = 50.006F; b.order = 1;
    c.width = 50.012F; c.order = 2;
    check(ArrayListAnimator::before(c, b, true) && ArrayListAnimator::before(b, a, true) &&
          ArrayListAnimator::before(c, a, true), "near-equal fractional widths retain transitive ordering");

    // Real measurement with the GUI fonts, including a Japanese name and a suffix.
    ImGuiScope imgui;
    ModuleManager modules;
    register_builtin_modules(modules);
    auto& jp = modules.add<Named>("jp", "自動装備");
    auto& wide = modules.add<Named>("wide", "Environment Changer");
    auto& narrow = modules.add<Named>("narrow", "Zoom");
    auto& twin_a = modules.add<Named>("twin_a", "Reach");
    auto& twin_b = modules.add<Named>("twin_b", "Reach");
    auto& mode = modules.add<Named>("mode", "Sprint", std::vector<Setting>{Setting{"mode", "Mode", "", Choice{1, {"Hold", "Toggle Extended"}}}});
    for (Module* m : std::initializer_list<Module*>{&jp, &wide, &narrow, &twin_a, &twin_b, &mode}) m->set_enabled(true);
    Overlay overlay(modules);
    overlay.set_owns_font_atlas(true);
    overlay.set_dpi_override(96.0F);
    overlay.clock().set_fixed_step(1.0F / 60.0F);
    overlay.gui().set_open(false);
    for (int i = 0; i < 30; ++i) imgui.frame(overlay);
    check(overlay.fonts().using_system_fonts(), "GUI fonts available for measurement");
    const auto& rows = overlay.hud().animator().rows();
    check(rows.size() == 6, "six rows");
    for (std::size_t i = 1; i < rows.size(); ++i) {
        check(rows[i - 1].width > rows[i].width - 0.01F, "rows sorted by measured width");
        if (std::fabs(rows[i - 1].width - rows[i].width) < 0.01F) check(rows[i - 1].order < rows[i].order, "tie -> registration order");
    }
    const float jp_width = overlay.fonts().measure(FontRole::ArrayList, "自動装備");
    check(jp_width > overlay.fonts().measure(FontRole::ArrayList, "????") * 1.2F, "Japanese glyphs are real (not '?')");
    // Mode text uses its own face and participates in the complete plate width.
    check(mode.suffix() == "Toggle Extended", "default suffix = Mode setting");
    // Font changes can make Environment Changer wider than Sprint + its mode.
    // Check the actual mode row instead of assuming it must sort first.
    const auto mode_it = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.id == "mode"; });
    check(mode_it != rows.end(), "mode row is present");
    const auto& mode_row = *mode_it;
    const float expected_width = 10.0F + overlay.fonts().measure(FontRole::ArrayList, "Sprint") +
        7.0F + overlay.fonts().measure(FontRole::ArrayListMode, "Toggle Extended") + 12.0F;
    check(close_to(mode_row.width, expected_width, 0.001), "plate width includes separate name/mode advances and exact padding");
}

// --- 9.1 ArrayList animation ----------------------------------------------------------
void test_arraylist_animation() {
    ArrayListAnimator::Params params;
    check(params.top == 8.0F && params.row_height == 24.0F && params.gap == 0.0F && params.pulse,
          "Edge-lit layout defaults");
    const auto advance = [&](ArrayListAnimator& animator, const std::vector<ArrayListAnimator::Entry>& entries,
                             float seconds, float dt) {
        while (seconds > 0.000001F) {
            const float step = std::min(dt, seconds);
            animator.update(entries, params, step);
            seconds -= step;
        }
    };
    for (const float fps : {30.0F, 60.0F, 144.0F, 240.0F}) {
        const float dt = 1.0F / fps;
        ArrayListAnimator animator;
        std::vector<ArrayListAnimator::Entry> entries{{"a", "Alpha", "", 40.0F, 0, true}};
        animator.update(entries, params, 0.0F);
        check(animator.rows().front().height == 0.0F && animator.rows().front().slide == 1.0F &&
              animator.rows().front().text_alpha == 0.0F, "ON starts collapsed beyond the right edge");
        advance(animator, entries, 0.050F, dt);
        check(animator.rows().front().text_alpha == 0.0F, "ON text waits 60 ms");
        advance(animator, entries, 0.050F, dt);
        check(close_to(animator.rows().front().height, 21.0F, 0.001) &&
              close_to(animator.rows().front().slide, 1.0F - ease_out_cubic(0.100F / 0.240F), 0.001),
              "ON height and slide use their independent easeOutCubic clocks @" + std::to_string(fps));
        check(animator.rows().front().activity == 0.0F && animator.pulses().front().travelling &&
              animator.pulses().front().y > params.top && animator.pulses().front().y < 20.0F,
              "light travels before illuminating the row");
        advance(animator, entries, 0.100F, dt);
        check(close_to(animator.rows().front().height, 24.0F, 0.001), "ON height completes at 200 ms");
        advance(animator, entries, 0.020F, dt);
        check(close_to(animator.rows().front().text_alpha, 1.0F, 0.001) &&
              close_to(animator.rows().front().activity, 1.0F, 0.001), "pulse arrives and text completes at 220 ms");
        check(!animator.pulses().front().travelling && close_to(animator.pulses().front().y, 20.0F, 0.001),
              "pulse stops at the row center");
        advance(animator, entries, 0.020F, dt);
        check(close_to(animator.rows().front().slide, 0.0F, 0.00001), "ON slide completes at 240 ms");
        advance(animator, entries, 0.580F, dt);
        check(close_to(animator.rows().front().activity, 0.125F, 0.001), "light follows a 1200 ms easeOutCubic decay");
        advance(animator, entries, 0.610F, dt);
        check(animator.rows().front().activity == 0.0F && animator.pulses().empty(), "light and pulse stop completely at rest");

        // From a settled row, OFF first waits for the traveling pulse.
        entries[0].enabled = false;
        animator.update(entries, params, 0.0F);
        advance(animator, entries, 0.210F, dt);
        check(animator.rows().front().slide == 0.0F && animator.rows().front().height == 24.0F,
              "OFF waits 220 ms before departing");
        advance(animator, entries, 0.010F, dt);
        check(close_to(animator.rows().front().activity, 0.6F, 0.001), "OFF pulse flashes at L=0.6");
        advance(animator, entries, 0.100F, dt);
        check(close_to(animator.rows().front().slide, 0.125F, 0.001) &&
              close_to(animator.rows().front().height, 24.0F * 0.999F, 0.001),
              "OFF slide is easeInCubic; collapse starts 80 ms later");
        advance(animator, entries, 0.100F, dt);
        check(animator.rows().front().opacity == 0.0F && animator.rows().front().height > 0.0F,
              "OFF surface vanishes at 420 ms while layout collapse continues");
        advance(animator, entries, 0.090F, dt);
        check(animator.rows().empty() && animator.pulses().empty(), "OFF releases its slot after 500 ms @" + std::to_string(fps));
        check(animator.height() > 0.0F, "last-row removal retains the tube's smooth height tail");
        advance(animator, entries, 0.600F, dt);
        check(animator.height() == 0.0F, "empty list height settles completely without an idle animation");

        // Sampling whole frames completes within one frame of the specified times.
        entries[0].enabled = true;
        animator.clear();
        int frames = 0;
        do {
            animator.update(entries, params, dt);
            ++frames;
        } while (animator.rows().front().slide > 0.0F && frames < 1000);
        const float in_time = frames * dt;
        check(in_time >= 0.240F - 1e-4F && in_time <= 0.240F + dt + 1e-4F, "ON completes in 240 ms @" + std::to_string(fps));
        entries[0].enabled = false;
        frames = 0;
        while (!animator.rows().empty() && frames < 1000) {
            animator.update(entries, params, dt);
            ++frames;
        }
        const float out_time = frames * dt;
        check(out_time >= 0.500F - 1e-4F && out_time <= 0.500F + dt + 1e-4F, "OFF completes in 500 ms @" + std::to_string(fps));
    }

    // Rapid toggles preserve every visible quantity at reversal, including mid-entry.
    for (const float fps : {30.0F, 60.0F, 144.0F, 240.0F}) {
        const float dt = 1.0F / fps;
        ArrayListAnimator animator;
        std::vector<ArrayListAnimator::Entry> entries{{"a", "Alpha", "", 40.0F, 0, true}, {"b", "Beta", "", 30.0F, 1, true}};
        float elapsed = 0.0F;
        bool state = true;
        animator.update(entries, params, 0.0F);
        for (int frame = 0; frame < static_cast<int>(fps * 2.0F); ++frame) {
            if (elapsed >= 0.1F) {
                elapsed -= 0.1F;
                state = !state;
                entries[0].enabled = state;
                const auto before = animator.rows().front();
                animator.update(entries, params, 0.0F);
                const auto& after = animator.rows().front();
                check(before.height == after.height && before.slide == after.slide &&
                      before.text_alpha == after.text_alpha && before.opacity == after.opacity && before.y == after.y,
                      "reversal keeps the current geometry and opacity");
            }
            animator.update(entries, params, dt);
            elapsed += dt;
            int count = 0;
            const ArrayListAnimator::Row* row = nullptr;
            for (const auto& r : animator.rows()) if (r.id == "a") { ++count; row = &r; }
            check(count <= 1, "no duplicate rows");
            check(row && row->height >= 0.0F && row->height <= 24.0F && row->slide >= 0.0F && row->slide <= 1.0F,
                  "rapid toggles keep geometry bounded");
            check(animator.pulses().size() <= 4, "at most four simultaneous pulses");
        }
        for (const bool final_state : {true, false}) {
            entries[0].enabled = final_state;
            for (int i = 0; i < static_cast<int>(fps); ++i) animator.update(entries, params, dt);
            int count = 0;
            for (const auto& r : animator.rows()) if (r.id == "a") { ++count; check(r.slide == 0.0F, "converges to visible"); }
            check(final_state ? count == 1 : count == 0, "final state converges @" + std::to_string(fps));
        }
    }
    // Width, ordering, positions and the complete tube height follow a 60 ms exponential.
    ArrayListAnimator animator;
    params.animate = false;
    std::vector<ArrayListAnimator::Entry> entries{{"w", "Wide", "", 90.0F, 0, true}, {"n", "N", "", 20.0F, 1, true}};
    animator.update(entries, params, 0.016F);
    check(animator.rows()[0].id == "w" && animator.rows()[1].y == 32.0F && animator.height() == 48.0F,
          "rows touch and start eight pixels below the screen top");

    // A mode change animates the surface width, while sorting still uses the new text width.
    params.animate = true;
    entries[1].suffix = "Extended";
    entries[1].width = 120.0F;
    const float dt = 1.0F / 60.0F;
    animator.update(entries, params, dt);
    check(animator.rows().front().id == "n", "mode change immediately updates width sorting");
    check(animator.rows().front().display_width > 20.0F && animator.rows().front().display_width < 120.0F,
          "surface expands smoothly after a mode change");
    check(close_to(animator.rows().front().display_width, follow(20.0F, 120.0F, dt, 0.060F), 0.0001) &&
          close_to(animator.rows().front().y, follow(32.0F, 8.0F, dt, 0.060F), 0.0001),
          "width and reordered y use the exact 60 ms follow");
    check(animator.rows().front().activity == 0.0F && animator.pulses().empty(), "mode changes do not create unsolicited pulses");
    for (int i = 0; i < 90; ++i) animator.update(entries, params, 1.0F / 60.0F);
    check(close_to(animator.rows().front().display_width, 120.0F, 0.01), "surface converges to the full mode width");
    check(animator.rows().front().activity == 0.0F, "rail settles without an idle pulse");

    entries.push_back({"third", "Third", "", 10.0F, 2, true});
    animator.update(entries, params, 0.100F);
    check(close_to(animator.height(), follow(48.0F, 69.0F, 0.100F, 0.060F), 0.001),
          "tube height follows the sum of animated occupied heights");

    entries[1].width = 24.0F;
    params.animate = false;
    animator.update(entries, params, 1.0F / 60.0F);
    const auto snapped = std::find_if(animator.rows().begin(), animator.rows().end(), [](const auto& row) { return row.id == "n"; });
    check(snapped != animator.rows().end() && snapped->display_width == 24.0F && snapped->activity == 0.0F,
          "Animation off snaps width and suppresses rail animation");

    // Pulse OFF leaves the insertion/departure motion, with no wait or light.
    params.animate = true;
    params.pulse = false;
    entries[1].enabled = false;
    animator.update(entries, params, 0.100F);
    const auto leaving = std::find_if(animator.rows().begin(), animator.rows().end(), [](const auto& row) { return row.id == "n"; });
    check(leaving != animator.rows().end() && close_to(leaving->slide, 0.125F, 0.001) && leaving->activity == 0.0F &&
          animator.pulses().empty(), "pulse OFF departs immediately and suppresses all light");
    animator.update(entries, params, 0.190F);
    check(std::none_of(animator.rows().begin(), animator.rows().end(), [](const auto& row) { return row.id == "n"; }),
          "pulse OFF completes departure and collapse after 280 ms");

    animator.clear();
    params.pulse = true;
    entries.clear();
    for (int i = 0; i < 6; ++i) entries.push_back({std::to_string(i), "Row", "", 40.0F, i, true});
    animator.update(entries, params, 0.0F);
    check(animator.pulses().size() == 4 && animator.pulses().front().row_id == "2" && animator.pulses().back().row_id == "5",
          "a simultaneous burst evicts the oldest pulses");
    advance(animator, entries, 2.0F, 1.0F / 60.0F);
    const auto idle_rows = animator.rows();
    const float idle_height = animator.height();
    animator.update(entries, params, 1.0F / 60.0F);
    check(animator.pulses().empty() && animator.height() == idle_height, "no idle pulse or tube motion");
    for (std::size_t i = 0; i < idle_rows.size(); ++i) {
        check(animator.rows()[i].settled && animator.rows()[i].activity == 0.0F &&
              animator.rows()[i].y == idle_rows[i].y && animator.rows()[i].display_width == idle_rows[i].display_width,
              "idle row geometry is stationary");
    }
}

void test_arraylist_updates_at_rest() {
    ArrayListAnimator animator;
    ArrayListAnimator::Params params;
    params.animate = false;
    std::vector<ArrayListAnimator::Entry> entries{
        {"a", "Alpha", "", 60.0F, 1, true}, {"b", "Beta", "", 40.0F, 0, true}};
    animator.update(entries, params, 0.0F);
    for (int i = 0; i < 20; ++i) animator.update(entries, params, 1.0F / 60.0F);

    params.sort_by_width = false;
    animator.update(entries, params, 0.0F);
    check(animator.rows().front().id == "b", "changing sort mode updates a stationary list");
    params.top = 20.0F;
    params.row_height = 30.0F;
    params.gap = 3.0F;
    animator.update(entries, params, 0.0F);
    check(animator.rows()[0].y == 20.0F && animator.rows()[1].y == 53.0F && animator.height() == 63.0F,
          "changing layout parameters updates stationary positions and height");

    params.sort_by_width = true;
    entries[1].name = "Renamed";
    entries[1].suffix = "Extended";
    entries[1].width = 100.0F;
    animator.update(entries, params, 0.0F);
    check(animator.rows().front().id == "b" && animator.rows().front().name == "Renamed" &&
          animator.rows().front().suffix == "Extended" && animator.rows().front().display_width == 100.0F,
          "stationary input changes update text, measured width and ordering");
    entries.pop_back();
    animator.update(entries, params, 0.0F);
    check(animator.rows().size() == 1 && animator.rows().front().id == "a" && animator.height() == 30.0F,
          "removing a module releases a stationary row");
    entries[0].enabled = false;
    animator.update(entries, params, 0.0F);
    check(animator.rows().empty() && animator.height() == 0.0F, "disabling the final stationary row clears the rail");
    entries[0].enabled = true;
    animator.update(entries, params, 0.0F);
    animator.clear();
    animator.update(entries, params, 0.0F);
    check(animator.rows().size() == 1 && animator.rows().front().id == "a" && animator.height() == 30.0F,
          "clear followed by unchanged module inputs rebuilds the rail");
}

// --- Edge-lit render packet: physical layout, typefaces, colors and exposed corners --------
void test_edgelit_frame() {
    ImGuiScope imgui;
    ModuleManager modules;
    register_builtin_modules(modules);
    auto* settings = dynamic_cast<ArrayListModule*>(modules.find("arraylist"));
    check(settings->settings().size() == 6, "ArrayList exposes exactly six settings");
    check((settings->accent_a.get<Color>() == Color{1.0F, 79.0F / 255, 160.0F / 255, 1.0F}) &&
          (settings->accent_b.get<Color>() == Color{139.0F / 255, 107.0F / 255, 1.0F, 1.0F}) &&
          settings->glow.get<float>() == 1.0F && settings->shadow.get<float>() == 1.0F &&
          settings->pulse.get<bool>() && settings->scale.get<float>() == 1.0F, "exact Edge-lit setting defaults");
    auto& wide = modules.add<Named>("wide", "Auto Crystal Extended", std::vector<Setting>{Setting{"mode", "Mode", "", Choice{0, {"Smart"}}}});
    auto& twin_a = modules.add<Named>("twin_a", "Velocity");
    auto& twin_b = modules.add<Named>("twin_b", "Velocity");
    auto& short_row = modules.add<Named>("short", "Timer");
    for (Module* module : std::initializer_list<Module*>{&wide, &twin_a, &twin_b, &short_row}) module->set_enabled(true);
    Overlay overlay(modules);
    overlay.set_owns_font_atlas(true);
    overlay.set_dpi_override(96.0F);
    overlay.clock().set_fixed_step(1.0F / 60.0F);
    overlay.gui().set_open(false);
    for (int i = 0; i < 120; ++i) imgui.frame(overlay);
    const auto* frame = current_edge_lit_frame();
    check(frame && frame->rows.size() == 4 && frame->pulses.empty(), "settled render packet contains only visible modules and no idle pulses");
    check(frame->screenWidth == 1920.0F && frame->top == 8.0F && frame->height == 96.0F && frame->scale == 1.0F,
          "tube covers the complete physical list height");
    const auto name_face = overlay.fonts().face(FontRole::ArrayList);
    const auto mode_face = overlay.fonts().face(FontRole::ArrayListMode);
    check(name_face.font && mode_face.font && name_face.font != mode_face.font &&
          close_to(name_face.size, name_face.font->FontSize, 0.0001) &&
          close_to(mode_face.size, mode_face.font->FontSize, 0.0001), "name and mode use independent native-size rasterized fonts");
    const auto same_rgb = [](const ImVec4& actual, const ImVec4& expected) {
        return close_to(actual.x, expected.x, 0.00001) && close_to(actual.y, expected.y, 0.00001) && close_to(actual.z, expected.z, 0.00001);
    };
    check(same_rgb(frame->rows.front().color, frame->accentA) && same_rgb(frame->rows.back().color, frame->accentB),
          "first and last row use exact A and B endpoints");
    for (std::size_t i = 0; i < frame->rows.size(); ++i) {
        const auto& row = frame->rows[i];
        const float t = static_cast<float>(i) / static_cast<float>(frame->rows.size() - 1);
        const ImVec4 expected_color{frame->accentA.x + (frame->accentB.x - frame->accentA.x) * t,
            frame->accentA.y + (frame->accentB.y - frame->accentA.y) * t,
            frame->accentA.z + (frame->accentB.z - frame->accentA.z) * t, 1};
        check(row.right == 1911.0F && row.top == 8.0F + 24.0F * i && row.bottom - row.top == 24.0F,
              "plates touch the tube and each other without coordinate rounding drift");
        check(row.opacity == 1.0F && row.lighting == 0.0F && same_rgb(row.color, expected_color),
              "resting colors use a linear sRGB vertical gradient");
        check(close_to(row.name.pos.x - row.left, 10.0F, 0.0001), "name has exact left padding");
        const ImVec4 expected_name{row.color.x + (1.0F - row.color.x) * 0.88F,
            row.color.y + (1.0F - row.color.y) * 0.88F,
            row.color.z + (1.0F - row.color.z) * 0.88F, 1};
        check(same_rgb(row.name.color, expected_name) && row.name.color.w == 1.0F, "normal names use mix(c, white, 0.88)");
        check(row.roundTopLeft == (i == 0 ? 5.0F : 0.0F), "only the first top-left corner is round");
        check(close_to(row.name.pos.y + name_face.baseline,
                       std::round(row.top + (24.0F + name_face.cap_height) * 0.5F), 0.0001),
              "stationary name baseline snaps after independent vertical centering");
    }
    check(frame->rows[0].roundBottomLeft == 5.0F && frame->rows[1].roundBottomLeft == 0.0F &&
          frame->rows[2].roundBottomLeft == 5.0F && frame->rows[3].roundBottomLeft == 5.0F,
          "bottom-left rounding follows exposed steps, including equal-width rows");
    const auto& first = frame->rows.front();
    check(first.mode.value == "Smart" &&
          close_to(first.mode.pos.x - first.name.pos.x - overlay.fonts().measure(FontRole::ArrayList, first.name.value), 7.0F, 0.001) &&
          close_to(first.right - first.mode.pos.x - overlay.fonts().measure(FontRole::ArrayListMode, first.mode.value), 12.0F, 0.001),
          "suffix has a seven-pixel gap and twelve-pixel right padding");
    check(close_to(first.mode.pos.y + mode_face.baseline,
                   std::round(first.top + (24.0F + mode_face.cap_height) * 0.5F), 0.0001),
          "mode is vertically centered independently of the name");

    const auto initial_rows = frame->rows;
    const float initial_name_size = name_face.size;
    const float initial_mode_size = mode_face.size;
    overlay.set_dpi_override(192.0F);
    dynamic_cast<ClickGuiModule*>(modules.find("clickgui"))->scale.get<float>() = 1.5F;
    imgui.frame(overlay);
    frame = current_edge_lit_frame();
    check(frame && frame->rows.front().left == initial_rows.front().left && frame->rows.front().top == 8.0F &&
          frame->rows.back().bottom == 104.0F && overlay.fonts().face(FontRole::ArrayList).size == initial_name_size,
          "HUD physical layout and fonts remain independent of DPI and ClickGUI scale");

    settings->glow.get<float>() = settings->shadow.get<float>() = 0.0F;
    imgui.frame(overlay);
    frame = current_edge_lit_frame();
    check(frame && frame->glow == 0.0F && frame->shadow == 0.0F &&
          same_rgb(frame->rows.front().name.color, initial_rows.front().name.color) && frame->rows.front().opacity == 1.0F,
          "glow/shadow zero preserves the plate and text body colors");

    settings->scale.get<float>() = 1.25F;
    for (int i = 0; i < 60; ++i) imgui.frame(overlay);
    frame = current_edge_lit_frame();
    const auto scaled_name = overlay.fonts().face(FontRole::ArrayList);
    const auto scaled_mode = overlay.fonts().face(FontRole::ArrayListMode);
    check(frame && frame->top == 10.0F && frame->height == 120.0F && frame->rows.front().right == 1908.75F &&
          frame->rows.front().bottom - frame->rows.front().top == 30.0F && frame->rows.front().roundTopLeft == 6.25F,
          "scale multiplies offsets, row dimensions and corner radii exactly");
    check(close_to(scaled_name.size / initial_name_size, 1.25F, 0.0001) &&
          close_to(scaled_mode.size / initial_mode_size, 1.25F, 0.0001) &&
          scaled_name.size == scaled_name.font->FontSize && scaled_mode.size == scaled_mode.font->FontSize,
          "fractional-scale fonts are rasterized at their exact requested size");
    settings->pulse.get<bool>() = false;
    wide.set_enabled(false);
    imgui.frame(overlay);
    frame = current_edge_lit_frame();
    check(frame && frame->pulses.empty() && frame->rows.front().lighting == 0.0F,
          "pulse setting suppresses traveling pulses and row lighting in render packets");
    settings->set_enabled(false);
    imgui.frame(overlay);
    frame = current_edge_lit_frame();
    check(frame && frame->rows.empty() && frame->pulses.empty(), "disabling the HUD publishes an empty packet");

    // The final tube contraction outlives the last departing plate. Keep that
    // nonzero-height packet available to both D3D renderers until it settles.
    twin_a.set_enabled(false);
    twin_b.set_enabled(false);
    short_row.set_enabled(false);
    wide.set_enabled(true);
    settings->pulse.get<bool>() = true;
    settings->scale.get<float>() = 1.0F;
    settings->set_enabled(true);
    for (int i = 0; i < 120; ++i) imgui.frame(overlay);
    wide.set_enabled(false);
    for (int i = 0; i < 30; ++i) imgui.frame(overlay);
    frame = current_edge_lit_frame();
    check(frame && frame->rows.empty() && frame->pulses.empty() && frame->height > 0.0F,
          "last OFF plate disappears at 500 ms while the render packet preserves the contracting tube");
    for (int i = 0; i < 66; ++i) imgui.frame(overlay);
    frame = current_edge_lit_frame();
    check(frame && frame->rows.empty() && frame->height == 0.0F,
          "empty HUD render packet settles completely by 1600 ms");
}

// --- Shadow model ------------------------------------------------------------------------
void test_shadow_model() {
    // Spec 5.3 profile expectations (white background, 8 bit), 150x600 panel, radius 5.
    const float w = 150.0F, h = 600.0F, sigma = 7.0F, off = 4.0F, opacity = 0.38F;
    const auto pixel = [&](float x, float y) {
        return 255.0F * (1.0F - opacity * blurred_rounded_rect(w, h, CornerRadii::all(5.0F), sigma, x, y - off));
    };
    struct Case { float x, y; float expected; const char* name; };
    const Case cases[] = {
        {75.0F, h + 0.5F, 188.0F, "bottom d=0.5"}, {75.0F, h + 4.5F, 209.0F, "bottom d=4.5"},
        {75.0F, h + 10.5F, 238.0F, "bottom d=10.5"}, {75.0F, h + 17.5F, 252.0F, "bottom d=17.5"},
        {-0.5F, 300.0F, 209.0F, "left d=0.5"}, {-7.5F, 300.0F, 241.0F, "left d=7.5"}, {-14.5F, 300.0F, 253.0F, "left d=14.5"},
        {75.0F, -0.5F, 230.0F, "top d=0.5"}, {75.0F, -3.5F, 241.0F, "top d=3.5"},
    };
    for (const auto& c : cases) {
        const float value = pixel(c.x, c.y);
        check(std::fabs(value - c.expected) <= 1.0F, std::string("shadow ") + c.name + " = " + std::to_string(value));
    }
    check(normal_cdf(-20.0F) >= 0.0F && normal_cdf(20.0F) <= 1.0F && std::isfinite(blurred_rounded_rect(10, 10, CornerRadii::all(5), 0.01F, 5, 5)),
          "tiny sigma does not overflow");
    check(rounded_rect_sdf(10, 10, CornerRadii::all(5), 5, 5) < 0 && rounded_rect_sdf(10, 10, CornerRadii::all(5), 0.2F, 0.2F) > 0,
          "rounded rect sdf");
}

float mesh_shadow_alpha(const ImDrawList& list, ImVec2 point) {
    for (int i = 0; i + 2 < list.IdxBuffer.Size; i += 3) {
        const auto& a = list.VtxBuffer[list.IdxBuffer[i]];
        const auto& b = list.VtxBuffer[list.IdxBuffer[i + 1]];
        const auto& c = list.VtxBuffer[list.IdxBuffer[i + 2]];
        const float bx = b.pos.x - a.pos.x, by = b.pos.y - a.pos.y;
        const float cx = c.pos.x - a.pos.x, cy = c.pos.y - a.pos.y;
        const float px = point.x - a.pos.x, py = point.y - a.pos.y;
        const float determinant = bx * cy - by * cx;
        if (std::fabs(determinant) < 1e-6F) continue;
        const float u = (px * cy - py * cx) / determinant;
        const float v = (bx * py - by * px) / determinant;
        if (u < -1e-5F || v < -1e-5F || u + v > 1.0F + 1e-5F) continue;
        const auto alpha = [](ImU32 color) { return static_cast<float>((color & IM_COL32_A_MASK) >> IM_COL32_A_SHIFT); };
        return (1.0F - u - v) * alpha(a.col) + u * alpha(b.col) + v * alpha(c.col);
    }
    return 0.0F;
}

void check_shadow_mesh_profile(const ImDrawList& list, float w, float h, const CornerRadii& radii,
                               const ShadowParams& params) {
    const ImVec2 samples[] = {
        {w * 0.5F, -0.5F}, {w * 0.5F, h + 0.5F}, {-0.5F, h * 0.5F}, {w + 0.5F, h * 0.5F},
        {w * 0.5F, h + 0.5F * params.sigma}, {w * 0.5F, h + 1.5F * params.sigma},
        {-0.5F * params.sigma, h * 0.5F}, {-1.5F * params.sigma, h * 0.5F},
        {-1.5F, -1.5F}, {w + 1.5F, -1.5F}, {w + 1.5F, h + 1.5F}, {-1.5F, h + 1.5F},
    };
    for (const auto point : samples) {
        const float expected = 255.0F * params.opacity * blurred_rounded_rect(w, h, radii, params.sigma,
            point.x - params.offset_x, point.y - params.offset_y);
        const float actual = mesh_shadow_alpha(list, ImVec2(100.0F + point.x, 100.0F + point.y));
        check(close_to(actual, expected, 2.0), "rendered shadow profile differs from Gaussian: " +
            std::to_string(actual) + " vs " + std::to_string(expected));
    }
}

void test_shadow_mesh() {
    ImGuiScope imgui;
    MeshShadowRenderer renderer;
    const auto radii = CornerRadii::all(5.0F);
    const ShadowParams params{1.0F, 7.0F, 0.0F, 4.0F};
    int vertices = 0;
    // Many distinct animated dimensions must share one integration, rather than
    // churning the 96-entry LRU and rebuilding six panel shadows every frame.
    for (int frame = 0; frame < 160; ++frame) {
        const float w = 150.0F + static_cast<float>(frame % 11) * 1.25F;
        const float h = 120.0F + static_cast<float>(frame) * 2.25F;
        ImGui::NewFrame();
        auto* list = ImGui::GetBackgroundDrawList();
        renderer.draw(list, ImVec2(100, 100), ImVec2(100 + w, 100 + h), radii, params, 1.0F);
        if (frame == 0) {
            vertices = list->VtxBuffer.Size;
            std::vector<bool> referenced(static_cast<std::size_t>(vertices), false);
            for (const auto index : list->IdxBuffer) referenced[index] = true;
            check(std::all_of(referenced.begin(), referenced.end(), [](bool used) { return used; }),
                  "shadow emits only vertices referenced by exterior triangles");
        }
        check(list->VtxBuffer.Size == vertices && vertices > 0 && vertices < 4096,
              "animated shadow has a fixed, bounded exterior mesh");
        if (frame == 0 || frame == 159) check_shadow_mesh_profile(*list, w, h, radii, params);
        ImGui::Render();
    }
    check(renderer.stats().geometry_builds == 1 && renderer.stats().draws == 160,
          "long-span resizing builds one cached shadow geometry");

    const CornerRadii uneven{8, 3, 11, 0};
    const ShadowParams shifted{1, 7, -3, -4};
    renderer.reset_stats();
    for (int frame = 0; frame < 2; ++frame) {
        const float w = 150.0F + 25.0F * frame, h = 120.0F + 102.0F * frame;
        ImGui::NewFrame();
        auto* list = ImGui::GetBackgroundDrawList();
        renderer.draw(list, ImVec2(100, 100), ImVec2(100 + w, 100 + h), uneven, shifted, 1.0F);
        check_shadow_mesh_profile(*list, w, h, uneven, shifted);
        ImGui::Render();
    }
    check(renderer.stats().geometry_builds == 1, "stretch cache preserves asymmetric corners and negative offsets");

    // Clamped radii and overlapping Gaussian profiles must retain the exact
    // dimension-specific path, including nonuniform radii and signed offsets.
    struct Case { float w, h; CornerRadii radii; ShadowParams params; };
    const Case fallback[] = {
        {40, 22, CornerRadii::all(15), {1, 7, 0, 4}},
        {40, 24, CornerRadii::all(15), {1, 7, 0, 4}},
        {90, 95, {8, 3, 11, 0}, {1, 30, 3, -6}},
        {91, 96, {8, 3, 11, 0}, {1, 30, 3, -6}},
    };
    renderer.reset_stats();
    for (const auto& item : fallback) {
        ImGui::NewFrame();
        auto* list = ImGui::GetBackgroundDrawList();
        renderer.draw(list, ImVec2(100, 100), ImVec2(100 + item.w, 100 + item.h), item.radii, item.params, 1.0F);
        check_shadow_mesh_profile(*list, item.w, item.h, item.radii, item.params);
        ImGui::Render();
    }
    check(renderer.stats().geometry_builds == std::size(fallback), "small and wide-blur shadows keep distinct meshes");

    renderer.reset_stats();
    ImGui::NewFrame();
    auto* list = ImGui::GetBackgroundDrawList();
    renderer.draw(list, ImVec2(5000, 100), ImVec2(5200, 400), radii, params, 1.0F);
    check(renderer.stats().geometry_builds == 0 && renderer.stats().draws == 0 && list->VtxBuffer.empty(),
          "fully clipped shadows neither build nor emit geometry");
    ImGui::Render();
}

// --- Panel placement: display clamp is never written back ---------------------------------
void test_panel_clamp() {
    ImGuiScope imgui(1280.0F, 720.0F);
    ModuleManager modules;
    register_builtin_modules(modules);
    Overlay overlay(modules);
    overlay.set_dpi_override(96.0F);
    overlay.clock().set_fixed_step(1.0F / 60.0F);
    overlay.gui().set_open(true);
    imgui.frame(overlay);
    overlay.gui().set_panel_position(Category::Misc, 5000.0F, -300.0F);
    imgui.frame(overlay);
    auto* cfg = dynamic_cast<ClickGuiModule*>(modules.find("clickgui"));
    ClickGui::PanelRect misc{};
    for (const auto& r : overlay.gui().panel_rects()) if (r.category == Category::Misc) misc = r;
    check(misc.x == 1280.0F - px(theme::metrics::keep_visible_x, overlay.gui().ui_scale()) && misc.y == 0.0F,
          "off-screen panel is clamped for display");
    check(cfg->panel(Category::Misc).x->get<float>() == 5000.0F && cfg->panel(Category::Misc).y->get<float>() == -300.0F,
          "clamped position is not written back");
    // Unsaved panels use the theme's origin and spacing, then wrap at the client edge.
    using namespace theme::metrics;
    const auto first = initial_panel_position(0, 1920);
    const auto fourth = initial_panel_position(3, 1920);
    check(first.x == start_x && first.y == start_y &&
          fourth.x == start_x + 3 * (panel_width + panel_gap) && fourth.y == start_y, "initial layout");
    const auto wrapped = initial_panel_position(4, 600.0F);
    check(wrapped.x <= 600.0F - panel_width && wrapped.y >= start_y + panel_wrap_step_y, "wraps to the next row");
}

void test_legacy_panel_layout() {
    for (const bool customised : {false, true}) {
        ImGuiScope imgui;
        ModuleManager modules;
        register_builtin_modules(modules);
        modules.add<Named>("visual", "Visual fixture", Category::Visual);
        modules.add<Named>("movement", "Movement fixture", Category::Movement);
        modules.add<Named>("player", "Player fixture", Category::Player);
        auto* cfg = dynamic_cast<ClickGuiModule*>(modules.find("clickgui"));
        cfg->panel_order.get<std::string>() = "visual,movement,player,misc,client";
        for (std::size_t i = 0; i < category_count; ++i) {
            auto& panel = cfg->panel(static_cast<Category>(i));
            panel.x->get<float>() = 36.0F + static_cast<float>(i) * 162.0F;
            panel.y->get<float>() = 42.0F;
        }
        if (customised) cfg->panel(Category::Player).x->get<float>() = 399.0F;
        Overlay overlay(modules);
        overlay.set_dpi_override(96.0F);
        overlay.clock().set_fixed_step(1.0F / 60.0F);
        int saves = 0;
        overlay.set_save_handler([&] { ++saves; });
        overlay.gui().set_open(true);
        imgui.frame(overlay);
        if (customised) {
            check(cfg->panel(Category::Player).x->get<float>() == 399.0F &&
                  cfg->panel(Category::Visual).x->get<float>() == 36.0F && saves == 0,
                  "custom panel positions are preserved during the redesign");
        } else {
            const auto rects = overlay.gui().panel_rects();
            check(rects.size() == category_count && saves == 1, "old automatic layout migration is saved once");
            for (std::size_t i = 1; i < rects.size(); ++i) {
                check(rects[i].x >= rects[i - 1].x + rects[i - 1].w,
                      "wider panels do not overlap after migrating the old grid");
            }
            imgui.frame(overlay);
            check(saves == 1, "migration does not repeat on the next frame");
        }
    }
}

void test_wrapped_default_panels() {
    constexpr float width = 1280.0F, height = 720.0F;
    ImGuiScope imgui(width, height);
    ModuleManager modules;
    register_builtin_modules(modules);
    // A long first column must scroll within its slot instead of covering the
    // second row. The built-ins already provide Misc and Client categories.
    for (int i = 0; i < 24; ++i) {
        modules.add<Named>("visual-" + std::to_string(i), "Visual " + std::to_string(i), Category::Visual);
    }
    for (const auto category : {Category::Movement, Category::Player}) {
        for (int i = 0; i < 6; ++i) {
            modules.add<Named>(std::string(category_id(category)) + "-" + std::to_string(i),
                               "Module " + std::to_string(i), category);
        }
    }
    Overlay overlay(modules);
    overlay.set_dpi_override(144.0F);
    overlay.clock().set_fixed_step(1.0F / 60.0F);
    overlay.gui().set_open(true);
    for (int i = 0; i < 20; ++i) imgui.frame(overlay);
    const auto rects = overlay.gui().panel_rects();
    check(rects.size() == category_count, "all five categories are present in the wrapped default layout");
    const float scale = overlay.gui().ui_scale();
    const float header = px(theme::metrics::header_height, scale);
    const float module_row = px(theme::metrics::module_height, scale);
    for (const auto& r : rects) {
        check(r.x >= 0.0F && r.y >= 0.0F && r.x + r.w <= width && r.y + r.h <= height,
              std::string(category_name(r.category)) + " default panel body stays inside the client area");
        check(r.h >= header + module_row,
              std::string(category_name(r.category)) + " default panel exposes at least one complete module row");
    }
    for (std::size_t i = 0; i < rects.size(); ++i) {
        for (std::size_t j = i + 1; j < rects.size(); ++j) {
            const auto& a = rects[i];
            const auto& b = rects[j];
            const bool overlap = a.x < b.x + b.w && b.x < a.x + a.w &&
                                 a.y < b.y + b.h && b.y < a.y + a.h;
            check(!overlap, "default category panels do not overlap within or between rows");
        }
    }
    const auto find_rect = [&](Category category) {
        const auto found = std::find_if(rects.begin(), rects.end(), [&](const auto& r) { return r.category == category; });
        check(found != rects.end(), "expected category has a panel rectangle");
        return *found;
    };
    const auto visual = find_rect(Category::Visual);
    const auto client = find_rect(Category::Client);
    check(client.y > visual.y && visual.y + visual.h <= client.y,
          "wrapped Client panel starts below the complete first-row Visual panel");

    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(visual.x + visual.w * 0.5F, visual.y + header + module_row * 0.5F);
    imgui.frame(overlay);
    io.AddMouseWheelEvent(0.0F, -1.0F);
    imgui.frame(overlay);
    check(overlay.gui().panel_scroll_target(Category::Visual) > 0.0F,
          "tall default panels retain working wheel scrolling within their visible row slot");

    // ClickGUI is the first Client module. A real pointer click on the second
    // row must reach it and toggle the GUI, rather than a first-row panel.
    io.AddMousePosEvent(client.x + client.w * 0.5F, client.y + header + module_row * 0.5F);
    imgui.frame(overlay);
    io.AddMouseButtonEvent(0, true);
    imgui.frame(overlay);
    io.AddMouseButtonEvent(0, false);
    imgui.frame(overlay);
    check(!overlay.gui().open(), "the first module in the wrapped Client panel is visible and clickable");
}

// --- 9.1 Config round trip and compatibility ----------------------------------------------
void test_config() {
    TempDir dir;
    const auto path = dir.path / "config.json";
    std::string error;
    {
        ModuleManager modules;
        register_builtin_modules(modules);
        auto* cfg = dynamic_cast<ClickGuiModule*>(modules.find("clickgui"));
        auto* list = dynamic_cast<ArrayListModule*>(modules.find("arraylist"));
        cfg->panel(Category::Visual).x->get<float>() = 123.5F;
        cfg->panel(Category::Visual).y->get<float>() = 77.0F;
        cfg->panel(Category::Misc).collapsed->get<bool>() = true;
        cfg->panel_order.get<std::string>() = "misc,visual,client";
        cfg->scale.get<float>() = 1.25F;
        cfg->shadow_blur.get<float>() = 10.0F;
        list->accent_a.get<Color>() = Color{0.1F, 0.2F, 0.3F, 0.4F};
        list->accent_b.get<Color>() = Color{0.5F, 0.6F, 0.7F, 1.0F};
        list->glow.get<float>() = 1.4F;
        list->shadow.get<float>() = 0.7F;
        list->pulse.get<bool>() = false;
        list->scale.get<float>() = 1.75F;
        list->set_enabled(false);
        check(ConfigStore(path).save(modules, error), "save: " + error);
    }
    ModuleManager restored;
    register_builtin_modules(restored);
    check(ConfigStore(path).load(restored, error), "load: " + error);
    auto* cfg = dynamic_cast<ClickGuiModule*>(restored.find("clickgui"));
    auto* list = dynamic_cast<ArrayListModule*>(restored.find("arraylist"));
    check(cfg->panel(Category::Visual).x->get<float>() == 123.5F && cfg->panel(Category::Visual).y->get<float>() == 77.0F,
          "panel position round trip");
    check(cfg->panel(Category::Misc).collapsed->get<bool>() && cfg->panel_order.get<std::string>() == "misc,visual,client",
          "collapsed state and order round trip");
    check(cfg->scale.get<float>() == 1.25F && cfg->shadow_blur.get<float>() == 10.0F, "ClickGUI settings round trip");
    check((list->accent_a.get<Color>() == Color{0.1F, 0.2F, 0.3F, 0.4F}) &&
          (list->accent_b.get<Color>() == Color{0.5F, 0.6F, 0.7F, 1.0F}) &&
          list->glow.get<float>() == 1.4F && list->shadow.get<float>() == 0.7F &&
          !list->pulse.get<bool>() && list->scale.get<float>() == 1.75F && !list->enabled(),
          "all six Edge-lit settings round trip");
}

void compare_subset(const Json& expected, const Json& actual, const std::string& where) {
    if (expected.is_object()) {
        check(actual.is_object(), where + " is no longer an object");
        for (const auto& [key, value] : expected.items()) {
            check(actual.contains(key), where + "." + key + " was removed");
            compare_subset(value, actual.at(key), where + "." + key);
        }
    } else if (expected.is_number_float() || actual.is_number_float()) {
        check(close_to(expected.get<double>(), actual.get<double>(), 1e-6), where + " changed");
    } else {
        check(expected == actual, where + " changed: " + expected.dump() + " -> " + actual.dump());
    }
}

void test_config_compat() {
    TempDir dir;
    const auto path = dir.path / "config.json";
    std::filesystem::copy_file(std::filesystem::path(MEDICON_FIXTURE_DIR) / "baseline_config.json", path);
    std::ifstream original_stream(path);
    const Json original = Json::parse(original_stream);
    original_stream.close();
    ModuleManager modules;
    register_builtin_modules(modules);
    std::string error;
    check(ConfigStore(path).load(modules, error), "load baseline config: " + error);
    check(ConfigStore(path).save(modules, error), "save: " + error);
    std::ifstream saved_stream(path);
    const Json saved = Json::parse(saved_stream);
    compare_subset(original, saved, "config");
    check(saved["modules"].contains("clickgui") && saved["modules"].contains("arraylist"), "new keys are added");
    check(saved["modules"]["clickgui"]["settings"].contains("panels.visual.x"), "panel layout keys are added");
}

// --- Key routing ---------------------------------------------------------------------------
void test_keys() {
    ModuleManager modules;
    register_builtin_modules(modules);
    auto& probe = modules.add<Named>("probe", "Probe");
    probe.set_keybind('P');
    Overlay overlay(modules);
    overlay.gui().set_open(false);
    check(!overlay.key_event('P', true, false) && probe.enabled(), "hotkey toggles a module while the GUI is closed");
    overlay.key_event('P', false, false);
    check(overlay.key_event(0x2D, true, false) && overlay.gui().open(), "Insert opens the GUI");
    overlay.key_event(0x2D, false, false);
    check(overlay.key_event('P', true, false) && probe.enabled(), "hotkeys are blocked while the GUI is open");
    overlay.key_event('P', false, false);
    check(!overlay.key_event(0x2D, true, false) && !overlay.gui().open(), "Insert closes the GUI");
    overlay.key_event(0x2D, false, false);
    check(!modules.find("clickgui")->enabled(), "open key never reaches module hotkeys");
    overlay.gui().set_open(true);
    check(overlay.key_event(0x1B, true, false) && !overlay.gui().open(), "Esc closes the GUI and is consumed");
    overlay.social().open_screen();
    check(overlay.social().screen_open(), "Social opens independently of ClickGUI");
    check(overlay.key_event('P', true, false) && probe.enabled(), "Social owns keys and blocks module hotkeys");
    overlay.key_event('P', false, false);
    check(overlay.key_event(0x1B, true, false) && !overlay.social().screen_open(), "Esc closes Social and is consumed");
    check(!overlay.key_event('P', true, false) && !probe.enabled(), "module input resumes after Social closes");
    overlay.key_event('P', false, false);
}

// --- Existing extension API: independent ImGui panels -------------------------------------
void test_custom_panels() {
    ImGuiScope imgui;
    ModuleManager modules;
    register_builtin_modules(modules);
    Overlay overlay(modules);
    overlay.set_dpi_override(96.0F);
    int calls = 0;
    overlay.gui().add_panel("Debug", [&] { ++calls; });
    int custom = 0;
    overlay.gui().set_module_settings_renderer("example", [&](Module& module) { ++custom; ClickGui::render_settings(module); });
    overlay.gui().set_open(true);
    overlay.gui().set_module_expanded("example", true);
    imgui.frame(overlay); // a new ImGui window may skip its contents on the first (auto-fit) frame
    imgui.frame(overlay);
    check(calls >= 1 && custom >= 1, "add_panel and custom settings windows render while the GUI is open");
    const int before = calls;
    overlay.gui().set_open(false);
    imgui.frame(overlay);
    check(calls == before, "panels are hidden while the GUI is closed");
}

} // namespace

int main() {
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"scale", test_scale},
        {"slider", test_slider},
        {"animation primitives", test_animation_primitives},
        {"arraylist sort", test_arraylist_sort},
        {"arraylist animation", test_arraylist_animation},
        {"arraylist updates at rest", test_arraylist_updates_at_rest},
        {"Edge-lit render packet", test_edgelit_frame},
        {"shadow model", test_shadow_model},
        {"shadow mesh cache and profiles", test_shadow_mesh},
        {"panel clamp", test_panel_clamp},
        {"legacy panel layout", test_legacy_panel_layout},
        {"wrapped default panels", test_wrapped_default_panels},
        {"config round trip", test_config},
        {"config compatibility", test_config_compat},
        {"keys", test_keys},
        {"custom panels", test_custom_panels},
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
