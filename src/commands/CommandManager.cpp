#include <medicon/commands/CommandManager.hpp>

#include <medicon/core/ConfigStore.hpp>
#include <medicon/core/ModuleManager.hpp>
#include <medicon/modules/ClientModules.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <system_error>
#include <utility>
#include <vector>

namespace medicon {
namespace {

constexpr std::string_view usage =
    "Usage: .bind <module> [key], .bind list, .bind reset, or .unbind <module>. "
    "Quote module names containing spaces.";
constexpr std::string_view config_usage =
    "Usage: .config save [name] [overwrite], .config load <name>, "
    ".config delete <name>, or .config list. Short forms: .c s [name], .c l <name>. "
    "Without a name, save updates the current config.";
constexpr std::string_view help_usage = "Usage: .help [command].";

struct CommandInfo {
    std::string_view name;
    std::string_view alias;
    std::string_view description;
    std::string_view usage;
};

constexpr auto command_infos = std::to_array<CommandInfo>({
    {"bind", "", "Bind a key to a module.", usage},
    {"unbind", "", "Remove a module's key.", usage},
    {"config", "c", "Save and load named configs.", config_usage},
    {"help", "", "Show the available commands.", help_usage}
});

std::string command_label(const CommandInfo& command) {
    auto label = "." + std::string(command.name);
    if (!command.alias.empty()) label += " (." + std::string(command.alias) + ")";
    return label;
}

bool whitespace(char value) {
    return value == ' ' || value == '\t' || value == '\r' ||
           value == '\n' || value == '\v' || value == '\f';
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && whitespace(value.front())) value.remove_prefix(1);
    while (!value.empty() && whitespace(value.back())) value.remove_suffix(1);
    return value;
}

char lowercase(char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool equal_name(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (lowercase(left[index]) != lowercase(right[index])) return false;
    }
    return true;
}

struct Token {
    std::string text;
    bool quoted = false;
};

bool tokenize(std::string_view input, std::vector<Token>& tokens, std::string& error) {
    while (!(input = trim(input)).empty()) {
        Token token;
        if (input.front() == '"' || input.front() == '\'') {
            token.quoted = true;
            const char quote = input.front();
            input.remove_prefix(1);
            bool closed = false;
            while (!input.empty()) {
                char value = input.front();
                input.remove_prefix(1);
                if (value == quote) { closed = true; break; }
                if (value == '\\' && !input.empty() &&
                        (input.front() == quote || input.front() == '\\')) {
                    value = input.front();
                    input.remove_prefix(1);
                }
                token.text.push_back(value);
            }
            if (!closed) { error = "Unclosed quoted argument."; return false; }
            if (!input.empty() && !whitespace(input.front())) {
                error = "Separate quoted arguments with spaces.";
                return false;
            }
        } else {
            while (!input.empty() && !whitespace(input.front())) {
                token.text.push_back(input.front());
                input.remove_prefix(1);
            }
        }
        tokens.push_back(std::move(token));
    }
    return true;
}

CommandResult failure(std::string message) { return {true, false, false, std::move(message)}; }
CommandResult success(std::string message, bool changed = false) {
    return {true, changed, true, std::move(message)};
}

CommandResult not_found(std::string_view command) {
    return failure(std::string(command) + " was not found. Try using .help to see the available commands.");
}

CommandResult run_help(const std::vector<Token>& tokens) {
    if (tokens.size() > 2) return failure(std::string(help_usage));
    if (tokens.size() == 2) {
        std::string_view name = tokens[1].text;
        if (!name.empty() && name.front() == '.') name.remove_prefix(1);
        for (const auto& command : command_infos) {
            if (!equal_name(command.name, name) && (command.alias.empty() || !equal_name(command.alias, name))) continue;
            return success(command_label(command) + " - " + std::string(command.description) +
                "\n" + std::string(command.usage));
        }
        return not_found(tokens[1].text);
    }
    std::string message = "Commands:";
    for (const auto& command : command_infos)
        message += "\n" + command_label(command) + " - " + std::string(command.description);
    return success(message + "\nUse .help <command> for details.");
}

// Names become file names, so stay ASCII and avoid Windows device names.
bool valid_config_name(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (const char value : name) {
        const char letter = lowercase(value);
        if (!(letter >= 'a' && letter <= 'z') && !(value >= '0' && value <= '9') &&
                value != '-' && value != '_') return false;
    }
    for (const std::string_view device : {"con", "prn", "aux", "nul"})
        if (equal_name(name, device)) return false;
    const bool port = name.size() == 4 && (equal_name(name.substr(0, 3), "com") || equal_name(name.substr(0, 3), "lpt"));
    return !port || name[3] < '0' || name[3] > '9';
}

CommandResult list_configs(const std::filesystem::path& directory, bool show_usage) {
    std::vector<std::string> names;
    std::error_code error;
    const auto status = std::filesystem::status(directory, error);
    if (!std::filesystem::status_known(status)) return failure("Could not read configs: " + error.message());
    error.clear(); // A missing directory is an empty list.
    if (std::filesystem::is_directory(status)) {
        for (std::filesystem::directory_iterator entry(directory, error), end; !error && entry != end; entry.increment(error)) {
            if (entry->path().extension() != ".json") continue;
            const auto stem = entry->path().stem().u8string();
            std::string name(stem.begin(), stem.end());
            if (valid_config_name(name)) names.push_back(std::move(name));
        }
    }
    if (error) return failure("Could not read configs: " + error.message());
    std::sort(names.begin(), names.end());
    std::string message = "Configs:";
    if (names.empty()) message += " no configs are saved.";
    for (const auto& name : names) message += "\n" + name;
    if (show_usage) message += "\n" + std::string(config_usage);
    return success(std::move(message));
}

CommandResult run_config(ModuleManager& modules, const std::filesystem::path& directory,
                         std::string& current, const std::vector<Token>& tokens) {
    if (directory.empty()) return failure("Configs are not available in this host.");
    const auto action = [&](std::string_view name) {
        return tokens.size() > 1 && !tokens[1].quoted && equal_name(tokens[1].text, name);
    };
    if (tokens.size() == 1 || (tokens.size() == 2 && action("list")))
        return list_configs(directory, tokens.size() == 1);
    const bool save = action("save") || action("s");
    const bool load = action("load") || action("l");
    const bool remove = action("delete");
    const bool overwrite = save && tokens.size() == 4 && !tokens[3].quoted && equal_name(tokens[3].text, "overwrite");
    if ((!save && !load && !remove) || (!save && tokens.size() < 3) || tokens.size() > (overwrite ? 4U : 3U))
        return failure(std::string(config_usage));
    if (tokens.size() < 3 && current.empty())
        return failure("No config is loaded. Use .config save <name> or .config load <name>.");

    const std::string name = tokens.size() < 3 ? current : tokens[2].text;
    if (!valid_config_name(name))
        return failure("Invalid config name '" + name + "'. Use up to 64 letters, digits, '-' or '_'.");
    const ConfigStore store(directory / (name + ".json"));
    std::error_code file_error;
    const auto status = std::filesystem::status(store.path(), file_error);
    if (!std::filesystem::status_known(status))
        return failure("Could not read config '" + name + "': " + file_error.message());
    const bool exists = std::filesystem::is_regular_file(status);
    if (save) {
        if (exists && !overwrite && !equal_name(name, current))
            return failure("Config '" + name + "' already exists. Use .config save " + name + " overwrite to replace it.");
        std::string error;
        if (!store.save(modules, error)) return failure("Could not save config '" + name + "': " + error);
        current = name;
        return success("Saved config '" + name + "'.");
    }
    if (!exists) return failure("Config '" + name + "' does not exist. Use .config list to see saved configs.");
    if (remove) {
        std::filesystem::remove(store.path(), file_error);
        if (file_error) return failure("Could not delete config '" + name + "': " + file_error.message());
        if (equal_name(name, current)) current.clear();
        return success("Deleted config '" + name + "'.");
    }
    std::string error;
    if (!store.load(modules, error)) return failure("Could not load config '" + name + "': " + error);
    current = name;
    auto result = success("Loaded config '" + name + "'.", true);
    result.config_loaded = true;
    return result;
}

struct KeyName { std::string_view name; int code; };

// Platform-independent numeric Win32 virtual-key values, matching Module's
// stored keybind format. No keyboard layout or OS API is needed for parsing.
constexpr auto key_names = std::to_array<KeyName>({
    {"none", 0}, {"unbound", 0}, {"unbind", 0}, {"clear", 0}, {"off", 0},
    {"insert", 0x2D}, {"ins", 0x2D},
    {"shift", 0x10}, {"lshift", 0xA0}, {"leftshift", 0xA0},
    {"rshift", 0xA1}, {"rightshift", 0xA1},
    {"ctrl", 0x11}, {"control", 0x11},
    {"lctrl", 0xA2}, {"lcontrol", 0xA2}, {"leftctrl", 0xA2}, {"leftcontrol", 0xA2},
    {"rctrl", 0xA3}, {"rcontrol", 0xA3}, {"rightctrl", 0xA3}, {"rightcontrol", 0xA3},
    {"alt", 0x12}, {"lalt", 0xA4}, {"leftalt", 0xA4}, {"ralt", 0xA5}, {"rightalt", 0xA5},
    {"space", 0x20}, {"spacebar", 0x20}, {"tab", 0x09}, {"esc", 0x1B}, {"escape", 0x1B},
    {"enter", 0x0D}, {"return", 0x0D}, {"backspace", 0x08}, {"back", 0x08},
    {"mouse1", 0x01}, {"lmb", 0x01}, {"mouse2", 0x02}, {"rmb", 0x02},
    {"mouse3", 0x04}, {"mmb", 0x04}, {"mouse4", 0x05}, {"mb4", 0x05}, {"xbutton1", 0x05},
    {"mouse5", 0x06}, {"mb5", 0x06}, {"xbutton2", 0x06},
    {"left", 0x25}, {"up", 0x26}, {"right", 0x27}, {"down", 0x28},
    {"home", 0x24}, {"end", 0x23}, {"pageup", 0x21}, {"pgup", 0x21},
    {"pagedown", 0x22}, {"pgdn", 0x22}, {"delete", 0x2E}, {"del", 0x2E},
    {"capslock", 0x14}, {"numlock", 0x90}, {"scrolllock", 0x91},
    {"pause", 0x13}, {"print", 0x2C}, {"printscreen", 0x2C},
    {"lwin", 0x5B}, {"rwin", 0x5C}, {"menu", 0x5D}
});

} // namespace

CommandManager::CommandManager(ModuleManager& modules, std::filesystem::path config_directory)
    : modules_(modules), config_directory_(std::move(config_directory)) {}

std::optional<int> CommandManager::parse_key(std::string_view input) {
    input = trim(input);
    if (input.empty()) return std::nullopt;
    if (input.size() == 1) {
        const char value = lowercase(input.front());
        if (value >= 'a' && value <= 'z') return static_cast<int>(value - ('a' - 'A'));
        if (value >= '0' && value <= '9') return static_cast<int>(value);
    }
    if (input.size() >= 2 && input.size() <= 3 && lowercase(input.front()) == 'f' && input[1] != '0') {
        int number = 0;
        const auto [end, error] = std::from_chars(input.data() + 1, input.data() + input.size(), number);
        if (error == std::errc{} && end == input.data() + input.size() && number >= 1 && number <= 24)
            return 0x70 + number - 1;
    }
    std::string name;
    for (const char value : input) {
        if (whitespace(value) || value == '_' || value == '-') continue;
        name.push_back(lowercase(value));
    }
    for (const auto& key : key_names) if (key.name == name) return key.code;
    return std::nullopt;
}

std::string CommandManager::key_label(int key) {
    if (key == 0) return "None";
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::string(1, static_cast<char>(key));
    if (key >= 0x70 && key <= 0x87) return "F" + std::to_string(key - 0x70 + 1);
    switch (key) {
    case 0x01: return "LMB";
    case 0x02: return "RMB";
    case 0x04: return "MMB";
    case 0x05: return "Mouse4";
    case 0x06: return "Mouse5";
    case 0x08: return "Backspace";
    case 0x09: return "Tab";
    case 0x0D: return "Enter";
    case 0x10: return "Shift";
    case 0x11: return "Ctrl";
    case 0x12: return "Alt";
    case 0x13: return "Pause";
    case 0x14: return "Caps Lock";
    case 0x1B: return "Esc";
    case 0x20: return "Space";
    case 0x21: return "Page Up";
    case 0x22: return "Page Down";
    case 0x23: return "End";
    case 0x24: return "Home";
    case 0x25: return "Left";
    case 0x26: return "Up";
    case 0x27: return "Right";
    case 0x28: return "Down";
    case 0x2C: return "Print";
    case 0x2D: return "Insert";
    case 0x2E: return "Delete";
    case 0x5B: return "LWin";
    case 0x5C: return "RWin";
    case 0x5D: return "Menu";
    case 0x90: return "Num Lock";
    case 0x91: return "Scroll Lock";
    case 0xA0: return "LShift";
    case 0xA1: return "RShift";
    case 0xA2: return "LCtrl";
    case 0xA3: return "RCtrl";
    case 0xA4: return "LAlt";
    case 0xA5: return "RAlt";
    default: return "Key " + std::to_string(key);
    }
}

CommandResult CommandManager::execute(std::string_view input) {
    input = trim(input);
    if (input.empty() || input.front() != '.') return {};
    std::vector<Token> tokens;
    std::string error;
    if (!tokenize(input, tokens, error)) return failure(std::move(error));
    if (equal_name(tokens.front().text, ".help")) return run_help(tokens);
    if (equal_name(tokens.front().text, ".config") || equal_name(tokens.front().text, ".c"))
        return run_config(modules_, config_directory_, current_config_, tokens);
    const bool bind = equal_name(tokens.front().text, ".bind");
    const bool unbind = equal_name(tokens.front().text, ".unbind");
    if (!bind && !unbind) return not_found(tokens.front().text);

    const bool list = bind && (tokens.size() == 1 ||
        (tokens.size() == 2 && !tokens[1].quoted && equal_name(tokens[1].text, "list")));
    if (list) {
        std::string message = "Binds:";
        std::size_t count = 0;
        for (const auto& module : modules_.all()) {
            if (module->keybind() == 0) continue;
            message += "\n" + module->name() + " [" + module->id() + "]: " + key_label(module->keybind());
            ++count;
        }
        if (count == 0) message += " no modules are currently bound.";
        if (tokens.size() == 1) message += "\n" + std::string(usage);
        return success(std::move(message));
    }
    if (bind && tokens.size() == 2 && !tokens[1].quoted && equal_name(tokens[1].text, "reset")) {
        bool changed = false;
        bool has_gui = false;
        for (const auto& module : modules_.all()) {
            const bool gui = equal_name(module->id(), ClickGuiModule::module_id);
            has_gui |= gui;
            const int key = gui ? ClickGuiModule::default_key : 0;
            changed |= module->keybind() != key;
            module->set_keybind(key);
        }
        if (changed) modules_.reset_keys();
        return success(has_gui ? "All binds reset. ClickGUI stays reachable on Insert." : "All binds cleared.", changed);
    }
    if ((unbind && tokens.size() != 2) || (bind && (tokens.size() < 2 || tokens.size() > 3)))
        return failure(std::string(usage));

    Module* selected = nullptr;
    for (const auto& module : modules_.all()) {
        if (!equal_name(module->id(), tokens[1].text) && !equal_name(module->name(), tokens[1].text)) continue;
        if (selected) return failure("Ambiguous module name '" + tokens[1].text + "'; use a unique module ID.");
        selected = module.get();
    }
    if (!selected) return failure("Unknown module '" + tokens[1].text + "'.");
    if (bind && tokens.size() == 2)
        return success(selected->name() + " is bound to " + key_label(selected->keybind()) + ".");

    const auto requested = unbind ? std::optional<int>(0) : parse_key(tokens[2].text);
    if (!requested) return failure("Unknown key '" + tokens[2].text + "'. Use a letter, digit, F1-F24, named key, Mouse4/Mouse5, or none.");
    const bool gui_fallback = *requested == 0 && equal_name(selected->id(), ClickGuiModule::module_id);
    const int key = gui_fallback ? ClickGuiModule::default_key : *requested;
    const bool changed = selected->keybind() != key;
    if (changed) {
        selected->set_keybind(key);
        modules_.reset_keys();
    }
    if (gui_fallback) return success("ClickGUI is bound to Insert so the GUI stays reachable.", changed);
    if (key == 0) return success(selected->name() + (changed ? " is now unbound." : " is already unbound."), changed);
    return success(selected->name() + (changed ? " is now bound to " : " is already bound to ") + key_label(key) + ".", changed);
}

} // namespace medicon
