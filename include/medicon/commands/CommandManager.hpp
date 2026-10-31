#pragma once

#include <filesystem>
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
    bool config_loaded{}; // A named config replaced the live module state.
};

// Run on the same thread that owns the ModuleManager. This class parses local
// commands, changes existing module keybinds and reads/writes named configs in
// config_directory (empty disables .config); its caller owns persisting the
// active config and displaying messages. Every leading-dot command is consumed
// locally.
class CommandManager {
public:
    explicit CommandManager(ModuleManager& modules, std::filesystem::path config_directory = {});

    CommandResult execute(std::string_view input);
    static std::optional<int> parse_key(std::string_view input);
    static std::string key_label(int key);

private:
    ModuleManager& modules_;
    std::filesystem::path config_directory_;
    std::string current_config_; // Last named config saved or loaded this session.
};

} // namespace medicon
