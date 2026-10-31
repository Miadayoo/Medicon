#include <medicon/commands/CommandManager.hpp>
#include <medicon/core/ConfigStore.hpp>
#include <medicon/core/ModuleManager.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace medicon;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class TestModule final : public Module {
public:
    TestModule(std::string id, std::string name, int key = 0)
        : Module(std::move(id), std::move(name), "Command test", Category::Misc, key) {}
};

void expect_success(const CommandResult& result, bool changed, const char* message) {
    check(result.handled && result.success && result.changed == changed && !result.message.empty(), message);
}

void expect_failure(const CommandResult& result, const char* message) {
    check(result.handled && !result.success && !result.changed && !result.message.empty(), message);
}

void test_keys() {
    constexpr std::array<std::pair<const char*, int>, 30> keys{{
        {"a", 'A'}, {"Z", 'Z'}, {"0", '0'}, {"9", '9'},
        {"F1", 0x70}, {"f12", 0x7B}, {"F24", 0x87},
        {"insert", 0x2D}, {"Ins", 0x2D}, {"rSHIFT", 0xA1}, {"LShift", 0xA0},
        {"right shift", 0xA1}, {"left_shift", 0xA0},
        {"CTRL", 0x11}, {"control", 0x11}, {"lctrl", 0xA2},
        {"LeftControl", 0xA2}, {"rcontrol", 0xA3}, {"right ctrl", 0xA3},
        {"Space", 0x20}, {"spacebar", 0x20}, {"Tab", 0x09}, {"Escape", 0x1B},
        {"Mouse4", 0x05}, {"mouse5", 0x06}, {"xbutton2", 0x06},
        {"None", 0}, {"unbound", 0}, {"clear", 0}, {"off", 0}
    }};
    for (const auto& [text, code] : keys) {
        const auto parsed = CommandManager::parse_key(text);
        check(parsed && *parsed == code, "Key name or alias parsed incorrectly");
        const auto round_trip = CommandManager::parse_key(CommandManager::key_label(code));
        check(round_trip && *round_trip == code, "Canonical key label cannot be parsed");
    }
    for (const auto* text : {"", " ", "F0", "F25", "F01", "F-1", "F 1", "Fn", "255", "0x2D",
                             "Ctrl+A", "mouse6", "SomeKey", "AA", "-1", "+1"})
        check(!CommandManager::parse_key(text), "Unsupported key accepted");
    check(CommandManager::parse_key("F8") == 0x77, "F8 must parse as a normal physical key");
    check(CommandManager::key_label(-1) == "Key -1", "Unknown key label must remain diagnostic");
}

void test_bind_and_query() {
    ModuleManager modules;
    auto& example = modules.add<TestModule>("example", "Example Module");
    auto& sprint = modules.add<TestModule>("sprint", "Sprint Assist");
    auto& gui = modules.add<TestModule>("clickgui", "ClickGUI", 0x2D);
    CommandManager commands(modules);

    for (const auto* text : {"", "hello world", "bind example K", "/help", "chat .bind example K"}) {
        const auto result = commands.execute(text);
        check(!result.handled && !result.changed && !result.success && result.message.empty(), "Ordinary chat was consumed");
    }
    expect_success(commands.execute(" \t.BiNd EXAMPLE k \r\n"), true, "Case-insensitive ID binding failed");
    check(example.keybind() == 'K' && !example.enabled(), "Binding changed something other than the keybind");
    expect_success(commands.execute(".bind example K"), false, "Unchanged bind should not request persistence");
    const auto query = commands.execute(".bind \"example module\"");
    expect_success(query, false, "Quoted display-name query failed");
    check(query.message.find("K") != std::string::npos, "Query omitted the current key");
    expect_success(commands.execute(".bind 'SPRINT ASSIST' \"Right Shift\""), true, "Quoted module/key binding failed");
    check(sprint.keybind() == 0xA1, "Right Shift virtual key was not stored");

    const auto list = commands.execute(".bind list");
    expect_success(list, false, "Bind list failed");
    check(list.message.find("Example Module [example]: K") != std::string::npos &&
          list.message.find("Sprint Assist [sprint]: RShift") != std::string::npos &&
          list.message.find("ClickGUI [clickgui]: Insert") != std::string::npos,
          "Bind list omitted keys or disambiguating module IDs");
    expect_success(commands.execute(".bind"), false, "Bare bind should list keys and usage");
    expect_success(commands.execute(".unbind example"), true, "Unbind failed");
    expect_success(commands.execute(".unbind example"), false, "Already-unbound command incorrectly reported a change");
    check(example.keybind() == 0, "Unbind did not store None");
    expect_success(commands.execute(".bind sprint none"), true, "None alias failed");
    check(sprint.keybind() == 0, "None did not remove the bind");
    expect_success(commands.execute(".bind example 0"), true, "Digit zero binding failed");
    check(example.keybind() == '0', "Digit zero was confused with None");
    check(gui.keybind() == 0x2D, "Other module commands changed the GUI bind");
}

void test_errors_are_atomic() {
    ModuleManager modules;
    auto& example = modules.add<TestModule>("example", "Example Module", 'Q');
    CommandManager commands(modules);
    for (const auto* text : {".bind missing K", ".bind example fakekey", ".bind example F25",
                             ".bind example Ctrl+A", ".bind example K extra", ".unbind example K",
                             ".unbind", ".bind \"Example Module K", ".bind \"Example Module\"K",
                             ".bind \"\" K", ".bind example \"\"", ".unknown example K", ".", "..bind example K"}) {
        expect_failure(commands.execute(text), "Malformed or unsupported command was accepted");
        check(example.keybind() == 'Q', "Rejected command modified the existing bind");
    }
    auto& same_name = modules.add<TestModule>("second", "Example Module", 'W');
    expect_failure(commands.execute(".bind \"example module\" E"), "Ambiguous display name was accepted");
    check(example.keybind() == 'Q' && same_name.keybind() == 'W', "Ambiguous command partially modified modules");
    expect_success(commands.execute(".bind second E"), true, "Unique ID could not resolve a duplicate display name");
    modules.add<TestModule>("third", "EXAMPLE", 'T');
    expect_failure(commands.execute(".bind example R"), "ID/display-name ambiguity was accepted");
    check(example.keybind() == 'Q', "Ambiguous ID/name collision modified the bind");
}

void test_gui_fallback_and_reset() {
    ModuleManager modules;
    auto& example = modules.add<TestModule>("example", "Example", 'K');
    auto& gui = modules.add<TestModule>("clickgui", "ClickGUI", 'P');
    auto& reserved = modules.add<TestModule>("reset", "Reset", 'R');
    auto& quoted = modules.add<TestModule>("quoted", "A \"Quoted\" Module");
    CommandManager commands(modules);
    const auto fallback = commands.execute(".bind clickgui none");
    expect_success(fallback, true, "ClickGUI None did not restore Insert");
    check(gui.keybind() == 0x2D && fallback.message.find("reachable") != std::string::npos,
          "ClickGUI fallback did not explain how to reopen it");
    expect_success(commands.execute(".unbind clickgui"), false, "Already-safe ClickGUI fallback should not change state");
    expect_success(commands.execute(".bind clickgui F9"), true, "Custom ClickGUI key failed");
    expect_success(commands.execute(".unbind clickgui"), true, "Unbind ClickGUI did not restore Insert");
    check(gui.keybind() == 0x2D, "Unbind made ClickGUI unreachable");
    expect_success(commands.execute(".bind \"reset\""), false, "Quoted reserved word should query a module");
    check(reserved.keybind() == 'R' && example.keybind() == 'K', "Quoted reset unexpectedly cleared binds");
    expect_success(commands.execute(".bind \"A \\\"Quoted\\\" Module\" F3"), true, "Escaped quotes in a display name failed");
    check(quoted.keybind() == 0x72, "Quoted display-name match stored the wrong key");
    gui.set_keybind('G');
    expect_success(commands.execute(".bind RESET"), true, "Global bind reset failed");
    check(example.keybind() == 0 && reserved.keybind() == 0 && quoted.keybind() == 0 && gui.keybind() == 0x2D,
          "Reset did not clear ordinary binds and retain GUI accessibility");
    expect_success(commands.execute(".bind reset"), false, "Idempotent reset should not request persistence");
}

void test_dispatch_and_persistence() {
    ModuleManager modules;
    auto& example = modules.add<TestModule>("example", "Example");
    auto& gui = modules.add<TestModule>("clickgui", "ClickGUI", 0x2D);
    CommandManager commands(modules);
    modules.key_event('E', true); // Remembered while it was not a module bind.
    expect_success(commands.execute(".bind example E"), true, "Binding command failed");
    modules.key_event('E', true);
    check(example.enabled(), "Changed bind left stale pressed-key state behind");
    modules.key_event('E', false);
    modules.key_event('E', true);
    check(!example.enabled(), "The module did not toggle from its new bind");
    expect_success(commands.execute(".bind example F8"), true, "F8 must remain available as a regular module bind");
    check(example.keybind() == 0x77, "F8 bind did not store the physical virtual-key code");
    modules.key_event(0x77, true);
    check(example.enabled(), "F8 was intercepted instead of dispatching the bound module");
    modules.key_event(0x77, false);
    expect_success(commands.execute(".bind clickgui mouse5"), true, "GUI mouse bind failed");

    const auto path = std::filesystem::temp_directory_path() /
        ("medicon-command-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    struct RemoveFile {
        std::filesystem::path path;
        ~RemoveFile() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{path};
    ConfigStore config(path);
    std::string error;
    check(config.save(modules, error), "Command-modified binds could not be saved");
    ModuleManager restored;
    auto& restored_example = restored.add<TestModule>("example", "Example");
    auto& restored_gui = restored.add<TestModule>("clickgui", "ClickGUI", 0x2D);
    check(config.load(restored, error), "Command-modified binds could not be restored");
    check(restored_example.keybind() == example.keybind() && restored_gui.keybind() == gui.keybind(),
          "Command binds did not round-trip through the existing config schema");
}

void test_help_and_not_found() {
    ModuleManager modules;
    CommandManager commands(modules);
    const auto help = commands.execute(".HELP");
    expect_success(help, false, "Help failed");
    for (const auto* name : {".bind", ".unbind", ".config", ".help"})
        check(help.message.find(name) != std::string::npos, "Help omitted a command");
    for (const auto* text : {".help config", ".help .config"}) {
        const auto detail = commands.execute(text);
        expect_success(detail, false, "Command help failed");
        check(detail.message.find(".config save") != std::string::npos, "Command help omitted usage");
    }
    expect_failure(commands.execute(".help bind extra"), "Extra help argument was accepted");
    const auto missing = commands.execute(".fly on");
    expect_failure(missing, "Unknown command was accepted");
    check(missing.message == ".fly was not found. Try using .help to see the available commands.",
          "Unknown command message changed");
    check(commands.execute(".help fly").message ==
          "fly was not found. Try using .help to see the available commands.",
          "Unknown help topic message changed");
}

void test_config() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("medicon-command-configs-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct RemoveDirectory {
        std::filesystem::path path;
        ~RemoveDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{directory};

    ModuleManager modules;
    auto& example = modules.add<TestModule>("example", "Example", 'K');
    CommandManager commands(modules, directory);

    expect_failure(CommandManager(modules).execute(".config list"), "Config command needs a directory");
    const auto empty = commands.execute(".config");
    expect_success(empty, false, "Listing a missing directory failed");
    check(empty.message.find("no configs") != std::string::npos, "Empty list was not reported");

    expect_failure(commands.execute(".c s"), "Bare save needs a current config");
    expect_success(commands.execute(".config save pvp"), false, "Save failed");
    check(std::filesystem::exists(directory / "pvp.json"), "Named config was not written");
    expect_success(commands.execute(".config save legit"), false, "Second save failed");
    example.set_keybind('L');
    expect_failure(commands.execute(".config save pvp"), "Save replaced another existing config");
    expect_failure(commands.execute(".config load missing"), "Missing config was loaded");
    check(example.keybind() == 'L', "Rejected load changed the module");
    const auto loaded = commands.execute(".CONFIG LOAD pvp");
    expect_success(loaded, true, "Load failed");
    check(loaded.config_loaded && example.keybind() == 'K', "Load did not restore the saved bind");

    example.set_keybind('M');
    expect_success(commands.execute(".config save pvp"), false, "Current config should save without overwrite");
    expect_success(commands.execute(".config save legit overwrite"), false, "Overwrite failed");
    const auto list = commands.execute(".config list");
    expect_success(list, false, "List failed");
    check(list.message == "Configs:\nlegit\npvp", "List was not sorted by name");

    { std::ofstream broken(directory / "broken.json"); broken << "{"; }
    expect_failure(commands.execute(".config load broken"), "Malformed config was loaded");
    check(example.keybind() == 'M', "Malformed config changed the module");

    expect_success(commands.execute(".config delete legit"), false, "Delete failed");
    expect_failure(commands.execute(".config delete legit"), "Deleted config still exists");
    check(!std::filesystem::exists(directory / "legit.json"), "Delete left the file");

    for (const auto* text : {".config save", ".config load", ".config delete", ".config rename pvp",
                             ".config \"list\"", ".config save pvp force", ".config load pvp overwrite",
                             ".config load ../pvp", ".config save a/b", ".config save a.b", ".config save \"\"",
                             ".config save con", ".config save NUL", ".config save com1",
                             ".config save \"two words\""})
        expect_failure(commands.execute(text), "Malformed config command was accepted");
    expect_success(commands.execute(".config save combo"), false, "Device-like prefix must remain usable");

    example.set_keybind('H');
    expect_success(commands.execute(".c s hive"), false, "Short save failed");
    example.set_keybind('J');
    const auto short_load = commands.execute(".C L hive");
    expect_success(short_load, true, "Short load failed");
    check(short_load.config_loaded && example.keybind() == 'H', "Short load did not restore the saved bind");
    example.set_keybind('N');
    const auto current_save = commands.execute(".c s");
    expect_success(current_save, false, "Bare save failed");
    check(current_save.message.find("hive") != std::string::npos, "Bare save did not target the loaded config");
    expect_success(commands.execute(".config load combo"), true, "Load before bare save failed");
    check(example.keybind() != 'N', "Bare save target test needs distinct state");
    expect_success(commands.execute(".config load hive"), true, "Reload of bare-saved config failed");
    check(example.keybind() == 'N', "Bare save did not write the current state");
    expect_failure(commands.execute(".c l"), "Short load needs a name");
    expect_success(commands.execute(".config delete hive"), false, "Delete of current config failed");
    expect_failure(commands.execute(".c s"), "Bare save targeted a deleted config");

    const auto alias_help = commands.execute(".help c");
    expect_success(alias_help, false, "Help for the short form failed");
    check(alias_help.message.find(".c s") != std::string::npos, "Help omitted the short forms");
}
} // namespace

int main() {
    try {
        test_keys();
        test_bind_and_query();
        test_errors_are_atomic();
        test_gui_fallback_and_reset();
        test_dispatch_and_persistence();
        test_help_and_not_found();
        test_config();
        std::cout << "Command tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Command test failure: " << error.what() << '\n';
        return 1;
    }
}
