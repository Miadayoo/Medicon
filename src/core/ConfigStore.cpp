#include "medicon/core/ConfigStore.hpp"
#include "medicon/core/ModuleManager.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace medicon {
namespace {

using Json = nlohmann::json;

[[noreturn]] void invalid(const std::string& location, const char* reason) {
    throw std::runtime_error(location + ": " + reason);
}

int read_integer(const Json& value, const std::string& location, int minimum, int maximum) {
    if (!value.is_number_integer()) invalid(location, "expected an integer");
    const auto number = value.get<double>();
    if (number < minimum || number > maximum) invalid(location, "integer outside allowed bounds");
    return value.get<int>();
}

float read_float(const Json& value, const std::string& location, float minimum, float maximum) {
    if (!value.is_number()) invalid(location, "expected a number");
    const auto number = value.get<double>();
    if (!std::isfinite(number) || number < minimum || number > maximum) {
        invalid(location, "number outside allowed bounds");
    }
    return static_cast<float>(number);
}

Setting::Value read_value(const Setting& setting, const Json& value, const std::string& location) {
    return std::visit([&](const auto& current) -> Setting::Value {
        using T = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<T, bool>) {
            if (!value.is_boolean()) invalid(location, "expected a boolean");
            return value.get<bool>();
        } else if constexpr (std::is_same_v<T, int>) {
            const auto number = read_integer(value, location, std::numeric_limits<int>::min(),
                                            std::numeric_limits<int>::max());
            if (static_cast<double>(number) < setting.minimum ||
                static_cast<double>(number) > setting.maximum) {
                invalid(location, "integer outside setting bounds");
            }
            return number;
        } else if constexpr (std::is_same_v<T, float>) {
            return read_float(value, location, setting.minimum, setting.maximum);
        } else if constexpr (std::is_same_v<T, Choice>) {
            if (current.options.empty()) invalid(location, "choice has no options");
            Choice choice = current;
            // Killaura's former boolean Rotate setting maps false -> None and
            // true -> Simple when upgrading an existing configuration.
            if (setting.id == "rotate" && value.is_boolean()) {
                choice.index = value.get<bool>() ? 1 : 0;
                return choice;
            }
            choice.index = read_integer(value, location, 0, static_cast<int>(choice.options.size() - 1));
            return choice;
        } else if constexpr (std::is_same_v<T, Color>) {
            if (!value.is_array() || value.size() != 4) invalid(location, "expected an RGBA array of four numbers");
            return Color{read_float(value[0], location, 0, 1), read_float(value[1], location, 0, 1),
                         read_float(value[2], location, 0, 1), read_float(value[3], location, 0, 1)};
        } else {
            if (!value.is_string()) invalid(location, "expected a string");
            return value.get<std::string>();
        }
    }, setting.value);
}

Json write_value(const Setting& setting) {
    auto value = std::visit([](const auto& current) -> Json {
        using T = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<T, Choice>) return current.index;
        else if constexpr (std::is_same_v<T, Color>) return Json::array({current.r, current.g, current.b, current.a});
        else return current;
    }, setting.value);
    // Public settings are convenient to edit; never persist invalid mutations.
    (void)read_value(setting, value, setting.id);
    return value;
}

struct PendingModule {
    Module* module;
    std::optional<bool> enabled;
    std::optional<int> keybind;
    std::vector<std::pair<Setting*, Setting::Value>> settings;
};

struct TemporaryFile {
    std::filesystem::path path;
    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
};

void atomic_replace(const std::filesystem::path& source, const std::filesystem::path& destination) {
#ifdef _WIN32
    // WRITE_THROUGH waits for the disk and hitches the frame that saved a click.
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Replace config");
    }
#else
    std::filesystem::rename(source, destination);
#endif
}

} // namespace

bool ConfigStore::load(ModuleManager& modules, std::string& error) const {
    error.clear();
    try {
        if (!std::filesystem::exists(path_)) return true;
        if (std::filesystem::file_size(path_) > 4 * 1024 * 1024) {
            throw std::runtime_error("Config exceeds the 4 MiB limit");
        }
        std::ifstream stream(path_, std::ios::binary);
        if (!stream) throw std::runtime_error("Cannot open config for reading");
        const Json root = Json::parse(stream);
        if (!root.is_object()) invalid("config", "expected an object");
        if (!root.contains("version")) invalid("version", "missing schema version");
        (void)read_integer(root.at("version"), "version", 1, 1);
        if (!root.contains("modules") || !root.at("modules").is_object()) {
            invalid("modules", "expected an object");
        }

        std::vector<PendingModule> pending;
        const auto& entries = root.at("modules");
        for (const auto& module : modules.all()) {
            auto found = entries.find(module->id());
            // Preserve existing CustomModelProbe settings on the rename. A
            // new-name entry wins if both are present; saves use custommodel.
            if (found == entries.end() && module->id() == "custommodel")
                found = entries.find("custommodelprobe");
            if (found == entries.end()) continue;
            const auto location = "modules." + module->id();
            if (!found->is_object()) invalid(location, "expected an object");
            PendingModule update{module.get(), {}, {}, {}};
            if (const auto enabled = found->find("enabled"); enabled != found->end()) {
                if (!enabled->is_boolean()) invalid(location + ".enabled", "expected a boolean");
                update.enabled = enabled->get<bool>();
            }
            if (const auto keybind = found->find("keybind"); keybind != found->end()) {
                update.keybind = read_integer(*keybind, location + ".keybind", 0, 255);
            }
            if (const auto settings = found->find("settings"); settings != found->end()) {
                if (!settings->is_object()) invalid(location + ".settings", "expected an object");
                const bool legacy_regen_mode = module->id() == "regen" &&
                    settings->contains("mode") && !settings->contains("switch");
                for (auto& setting : module->settings()) {
                    auto value = settings->find(setting.id);
                    if (legacy_regen_mode && setting.id == "switch") value = settings->find("mode");
                    if (value == settings->end()) continue;
                    update.settings.emplace_back(&setting, read_value(setting, *value,
                        location + ".settings." + setting.id));
                }
            }
            pending.push_back(std::move(update));
        }

        // Validate the complete document before changing live state. Set every
        // value before invoking hooks so callbacks see the complete new config.
        for (auto& update : pending) {
            for (auto& [setting, value] : update.settings) setting->value.swap(value);
            if (update.keybind) update.module->set_keybind(*update.keybind);
        }
        for (auto& update : pending) {
            if (update.enabled) update.module->set_enabled(*update.enabled);
        }
        modules.reset_keys();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool ConfigStore::save(const ModuleManager& modules, std::string& error) const {
    error.clear();
    try {
        Json root{{"version", 1}, {"modules", Json::object()}};
        for (const auto& module : modules.all()) {
            Json settings = Json::object();
            for (const auto& setting : module->settings()) settings[setting.id] = write_value(setting);
            root["modules"][module->id()] = Json{{"enabled", module->enabled()},
                {"keybind", module->keybind()}, {"settings", std::move(settings)}};
        }

        if (const auto directory = path_.parent_path(); !directory.empty()) {
            std::filesystem::create_directories(directory);
        }
        static std::atomic<unsigned long long> sequence{0};
        auto temporary_path = path_;
        temporary_path += ".tmp." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
            + "." + std::to_string(sequence.fetch_add(1));
        TemporaryFile temporary{std::move(temporary_path)};
        std::ofstream stream(temporary.path, std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("Cannot open temporary config for writing");
        stream << root.dump(2) << '\n';
        stream.flush();
        if (!stream) throw std::runtime_error("Cannot write config");
        stream.close();
        if (!stream) throw std::runtime_error("Cannot close config");
        atomic_replace(temporary.path, path_);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

} // namespace medicon
