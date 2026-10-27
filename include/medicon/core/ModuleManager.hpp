#pragma once

#include "medicon/core/Module.hpp"

#include <array>
#include <concepts>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace medicon {

class ModuleManager {
public:
    ~ModuleManager() { shutdown(); }
    ModuleManager() = default;
    ModuleManager(const ModuleManager&) = delete;
    ModuleManager& operator=(const ModuleManager&) = delete;

    // Add module types once at startup; keep the returned typed reference.
    template <class T, class... Args> requires std::derived_from<T, Module>
    T& add(Args&&... args) {
        auto module = std::make_unique<T>(std::forward<Args>(args)...);
        if (find(module->id())) {
            throw std::invalid_argument("Duplicate module ID: " + module->id());
        }
        auto& reference = *module;
        modules_.push_back(std::move(module));
        return reference;
    }

    std::vector<std::unique_ptr<Module>>& all() noexcept { return modules_; }
    const std::vector<std::unique_ptr<Module>>& all() const noexcept { return modules_; }
    Module* find(std::string_view id) noexcept;
    const Module* find(std::string_view id) const noexcept;

    // Win32 virtual-key codes; 0 means unbound. Dispatch only on a fresh press.
    void key_event(int key, bool down, bool repeat = false) noexcept;
    // Release remembered key state when focus changes or a GUI captures input.
    void reset_keys() noexcept { keys_down_.fill(false); }
    void tick(float delta_seconds) noexcept;
    void render() noexcept;
    void shutdown() noexcept;

private:
    std::vector<std::unique_ptr<Module>> modules_;
    std::array<bool, 256> keys_down_{};
};

} // namespace medicon
