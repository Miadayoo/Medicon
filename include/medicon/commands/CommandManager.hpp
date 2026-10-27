#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace medicon {

class ModuleManager;

struct CommandResult {
    bool handled{};
    bool changed{};
    bool success{};
    std::string message;
};

// Run on the same thread that owns the ModuleManager. This class parses local
// commands and changes existing module keybinds; its caller owns persistence
// and displaying messages. Every leading-dot command is consumed locally.
class CommandManager {
public:
    explicit CommandManager(ModuleManager& modules);

    CommandResult execute(std::string_view input);
    static std::optional<int> parse_key(std::string_view input);
    static std::string key_label(int key);

private:
    ModuleManager& modules_;
};

} // namespace medicon
