#include "medicon/core/ModuleManager.hpp"

#include <cmath>

namespace medicon {

Module* ModuleManager::find(std::string_view id) noexcept {
    if (id == "custommodelprobe") id = "custommodel"; // Legacy command/config name.
    for (const auto& module : modules_) if (module->id() == id) return module.get();
    return nullptr;
}

const Module* ModuleManager::find(std::string_view id) const noexcept {
    if (id == "custommodelprobe") id = "custommodel";
    for (const auto& module : modules_) if (module->id() == id) return module.get();
    return nullptr;
}

void ModuleManager::key_event(int key, bool down, bool repeat) noexcept {
    if (key <= 0 || key > 255) return;
    auto& held = keys_down_[static_cast<std::size_t>(key)];
    const bool fresh_press = down && !held && !repeat;
    held = down;
    if (!fresh_press) return;
    for (const auto& module : modules_) {
        if (module->keybind() == key) module->toggle();
    }
}

void ModuleManager::tick(float delta_seconds) noexcept {
    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0F) return;
    for (const auto& module : modules_) {
        if (module->enabled()) module->on_tick(delta_seconds);
    }
}

void ModuleManager::render() noexcept {
    for (const auto& module : modules_) if (module->enabled()) module->on_render();
}

void ModuleManager::shutdown() noexcept {
    for (auto iterator = modules_.rbegin(); iterator != modules_.rend(); ++iterator) {
        (*iterator)->set_enabled(false);
    }
    reset_keys();
}

} // namespace medicon
