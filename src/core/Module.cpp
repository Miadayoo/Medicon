#include "medicon/core/Module.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace medicon {

const char* category_name(Category category) noexcept {
    switch (category) {
    case Category::Visual: return "Visual";
    case Category::Movement: return "Movement";
    case Category::Player: return "Player";
    case Category::Misc: return "Misc";
    case Category::Client: return "Client";
    }
    return "Misc";
}

const char* category_id(Category category) noexcept {
    switch (category) {
    case Category::Visual: return "visual";
    case Category::Movement: return "movement";
    case Category::Player: return "player";
    case Category::Misc: return "misc";
    case Category::Client: return "client";
    }
    return "misc";
}

std::string_view Module::suffix() const {
    for (const auto& setting : settings_) {
        if (!setting.visible || (setting.name != "Mode" && setting.id != "mode")) continue;
        if (const auto* choice = std::get_if<Choice>(&setting.value)) {
            if (choice->index >= 0 && static_cast<std::size_t>(choice->index) < choice->options.size()) {
                return choice->options[static_cast<std::size_t>(choice->index)];
            }
        }
    }
    return {};
}

Module::Module(std::string id, std::string name, std::string description, Category category,
               int key)
    : id_(std::move(id)), name_(std::move(name)), description_(std::move(description)),
      category_(category) {
    if (id_.empty()) throw std::invalid_argument("Module ID cannot be empty");
    set_keybind(key);
}

void Module::set_keybind(int key) {
    if (key < 0 || key > 255) throw std::invalid_argument("Keybind must be in [0, 255]");
    keybind_ = key;
}

void Module::set_enabled(bool enabled) noexcept {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (enabled_) on_enable();
    else on_disable();
}

Setting& Module::add_setting(Setting setting) {
    if (setting.id.empty()) throw std::invalid_argument("Setting ID cannot be empty");
    if (std::ranges::any_of(settings_, [&](const Setting& existing) {
            return existing.id == setting.id;
        })) {
        throw std::invalid_argument("Duplicate setting ID: " + setting.id);
    }
    if (std::holds_alternative<int>(setting.value) || std::holds_alternative<float>(setting.value)) {
        if (!std::isfinite(setting.minimum) || !std::isfinite(setting.maximum) ||
            !std::isfinite(setting.step) || setting.minimum > setting.maximum || setting.step <= 0) {
            throw std::invalid_argument("Invalid bounds for setting: " + setting.id);
        }
        const auto value = std::holds_alternative<int>(setting.value)
            ? static_cast<double>(setting.get<int>()) : static_cast<double>(setting.get<float>());
        if (!std::isfinite(value) || value < setting.minimum || value > setting.maximum) {
            throw std::invalid_argument("Default outside bounds for setting: " + setting.id);
        }
    }
    if (const auto* choice = std::get_if<Choice>(&setting.value)) {
        if (choice->index < 0 || static_cast<std::size_t>(choice->index) >= choice->options.size()) {
            throw std::invalid_argument("Invalid choice default for setting: " + setting.id);
        }
    }
    if (const auto* color = std::get_if<Color>(&setting.value)) {
        for (const float channel : {color->r, color->g, color->b, color->a}) {
            if (!std::isfinite(channel) || channel < 0.0F || channel > 1.0F) {
                throw std::invalid_argument("Invalid color default for setting: " + setting.id);
            }
        }
    }
    settings_.push_back(std::move(setting));
    return settings_.back();
}

} // namespace medicon
