#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace medicon {

// Register disabled hooks, then enable them together after validating every target.
// The host must stop dispatch and quiesce callbacks before clear()/destruction.
class HookManager final {
public:
    HookManager() = default;
    ~HookManager();
    HookManager(const HookManager&) = delete;
    HookManager& operator=(const HookManager&) = delete;
    bool create(std::string name, void* target, void* detour, void** original, std::string& error);
    bool enable_all(std::string& error);
    bool disable_all(std::string& error);
    bool clear(std::string& error);
    bool shutdown(std::string& error);
    [[nodiscard]] std::size_t size() const noexcept { return hooks_.size(); }
private:
    struct Hook { std::string name; void* target; bool enabled{}; };
    bool initialize(std::string& error);
    std::vector<Hook> hooks_;
    bool initialized_{};
};

} // namespace medicon
