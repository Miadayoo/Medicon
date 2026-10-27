#include "DemoModules.hpp"

#include <medicon/core/ModuleManager.hpp>
#include <medicon/gui/Theme.hpp>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace medicon::preview {
namespace {

class DemoModule final : public Module {
public:
    DemoModule(std::string id, std::string name, Category category, std::vector<Setting> settings, int key = 0)
        : Module(std::move(id), std::move(name), "Preview demo module (no effect).", category, key) {
        for (auto& setting : settings) add_setting(std::move(setting));
    }
};

Setting choice(std::string id, std::string name, int index, std::vector<std::string> options) {
    return Setting{std::move(id), std::move(name), "", Choice{index, std::move(options)}};
}

} // namespace

void register_demo_modules(ModuleManager& modules) {
    const auto add = [&](std::string id, std::string name, Category category, std::vector<Setting> settings = {}, int key = 0) {
        modules.add<DemoModule>(std::move(id), std::move(name), category, std::move(settings), key);
    };
    using C = Category;
    add("demo.fullbright", "Fullbright", C::Visual, {}, 'B');
    add("demo.crosshair", "Crosshair", C::Visual, {choice("mode", "Mode", 0, {"Dot", "Cross", "Circle"})});
    add("demo.zoom", "Zoom", C::Visual, {
        Setting{"factor", "Factor", "", 3.0F, 1.0F, 10.0F, 0.1F},
        choice("mode", "Mode", 0, {"Smooth", "Instant"}),
        Setting{"smooth_camera", "Smooth Camera", "", true},
    }, 'C');
    add("demo.block_outline", "Block Outline", C::Visual, {
        Setting{"width", "Width", "", 2.0F, 0.5F, 5.0F, 0.5F},
        Setting{"segments", "Segments", "", 12, 4, 64, 4},
        Setting{"color", "Color", "", theme::demo_outline},
        Setting{"through_walls", "Through Walls", "", false},
    });
    add("demo.freelook", "Freelook", C::Visual, {}, 'F');
    add("demo.time_changer", "Time Changer", C::Visual, {Setting{"time", "Time", "", 6000, 0, 24000, 100}});
    add("demo.hit_color", "Hit Color", C::Visual, {Setting{"color", "Color", "", theme::demo_hit}});
    add("demo.motion_blur", "Motion Blur", C::Visual, {Setting{"strength", "Strength", "", 0.35F, 0.0F, 1.0F, 0.05F}});
    add("demo.nametags", "Nametags", C::Visual);
    add("demo.particles", "Particle Multiplier", C::Visual, {Setting{"multiplier", "Multiplier", "", 2, 1, 10, 1}});
    add("demo.item_physics", "Item Physics", C::Visual);
    add("demo.environment", "Environment Changer Extended", C::Visual, {
        choice("weather", "Weather", 0, {"Clear", "Rain", "Thunder", "Snow", "Fog", "Sandstorm", "Ash", "Spores", "Night", "Aurora"}),
    });

    add("demo.toggle_sprint", "Toggle Sprint", C::Movement, {choice("mode", "Mode", 1, {"Hold", "Toggle"})}, 'V');
    add("demo.toggle_sneak", "Toggle Sneak", C::Movement);
    add("demo.auto_walk", "Auto Walk", C::Movement);
    add("demo.snap_look", "Snap Look", C::Movement, {Setting{"angle", "Angle", "", 180, 90, 180, 90}});

    add("demo.auto_gg", "Auto GG", C::Player, {choice("message", "Message", 0, {"gg", "GG WP", "good game"})});
    add("demo.japanese", "自動装備", C::Player);
    add("demo.hit_delay", "Hit Delay Fix", C::Player);
    add("demo.inventory_hud", "Inventory HUD", C::Player, {Setting{"scale", "Scale", "", 1.0F, 0.5F, 2.0F, 0.05F}});

    add("demo.discord", "Discord Presence", C::Misc);
    add("demo.screenshot", "Screenshot Helper", C::Misc, {Setting{"copy", "Copy to Clipboard", "", true}});
    add("demo.fps_boost", "FPS Boost", C::Misc, {choice("mode", "Mode", 0, {"Low", "Balanced", "High"})});

    // A dedicated fixture matching the supplied Edge-lit reference. These modules
    // remain disabled in the normal demo and never perform game actions.
    add("edge.auto_crystal", "Auto Crystal", C::Player, {choice("mode", "Mode", 0, {"Smart", "Extended"})});
    add("edge.elytra_fly", "Elytra Fly", C::Movement, {choice("mode", "Mode", 0, {"Bounce", "Control"})});
    add("edge.velocity", "Velocity", C::Player, {choice("mode", "Mode", 0, {"Cancel", "Adaptive Extended"})});
    add("edge.no_fall", "No Fall", C::Player, {choice("mode", "Mode", 0, {"Packet", "Ground"})});
    add("edge.sprint", "Sprint", C::Movement, {choice("mode", "Mode", 0, {"Legit", "Omni"})});
    add("edge.nametags", "Nametags", C::Visual);
    add("edge.freecam", "Freecam", C::Visual);
    add("edge.burrow", "Burrow", C::Player);
    add("edge.timer", "Timer", C::Misc);
}

} // namespace medicon::preview
