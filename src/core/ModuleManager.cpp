#include "medicon/core/ModuleManager.hpp"

#include <cmath>

namespace medicon {

Module& ModuleManager::adopt(std::unique_ptr<Module> module) {
    if (!module || find(module->id())) throw std::invalid_argument("Null or duplicate runtime module");
    auto& result = *module;
    modules_.push_back(std::move(module));
    return result;
}

std::unique_ptr<Module> ModuleManager::extract(std::string_view id) {
    for (auto it = modules_.begin(); it != modules_.end(); ++it) {
        if ((*it)->id() != id) continue;
        (*it)->set_enabled(false);
        auto result = std::move(*it);
        modules_.erase(it);
        return result;
    }
    return {};
}

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
