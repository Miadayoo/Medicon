#include "medicon/core/ConfigStore.hpp"
#include "medicon/core/EventBus.hpp"
#include "medicon/core/ModuleManager.hpp"
#include "medicon/modules/BuiltinModules.hpp"
#include "medicon/modules/ExampleModule.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace medicon;
using Json = nlohmann::json;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <class Function> void expect_invalid(Function&& function, const char* message) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(message);
}

class Probe final : public Module {
public:
    explicit Probe(std::string id = "probe", int key = 65)
        : Module(std::move(id), "Probe", "Lifecycle probe", Category::Misc, key),
          amount(add_setting({"amount", "Amount", "", 2, 0, 10, 1})) {}

    Setting& amount;
    int enables = 0;
    int disables = 0;
    int ticks = 0;
    int renders = 0;
    int amount_on_enable = 0;
    float last_delta = 0;

    Setting& expose_setting(Setting setting) { return add_setting(std::move(setting)); }

protected:
    void on_enable() noexcept override {
        ++enables;
        amount_on_enable = amount.get<int>();
    }
    void on_disable() noexcept override { ++disables; }
    void on_tick(float delta) noexcept override { ++ticks; last_delta = delta; }
    void on_render() noexcept override { ++renders; }
};

struct TestDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("medicon-core-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TestDirectory() { std::filesystem::create_directory(path); }
    ~TestDirectory() {
        std::error_code ignored;
        std::filesystem::remove(path / "config.json", ignored);
        std::filesystem::remove(path, ignored);
    }
};

void write_json(const std::filesystem::path& path, const Json& json) {
    std::ofstream stream(path);
    stream << json.dump(2);
    check(static_cast<bool>(stream), "Failed to write fixture config");
}

Json read_json(const std::filesystem::path& path) {
    std::ifstream stream(path);
    return Json::parse(stream);
}

void test_modules() {
    ModuleManager modules;
    auto& probe = modules.add<Probe>();
    check(modules.find("probe") == &probe && !modules.find("absent"), "Module lookup failed");
    expect_invalid([&] { modules.add<Probe>(); }, "Duplicate module IDs accepted");
    expect_invalid([&] { probe.set_keybind(256); }, "Invalid keybind accepted");
    expect_invalid([&] { probe.expose_setting({"amount", "Duplicate", "", false}); },
                   "Duplicate setting IDs accepted");
    expect_invalid([&] { probe.expose_setting({"bad", "Bad", "", 12, 0, 10, 1}); },
                   "Out-of-bounds setting default accepted");

    auto* stable_address = &probe.amount;
    for (int index = 0; index < 64; ++index) {
        probe.expose_setting({"extra" + std::to_string(index), "Extra", "", false});
    }
    check(&probe.settings().front() == stable_address, "Setting references are not stable");
    modules.tick(0.1F);
    modules.render();
    check(probe.ticks == 0 && probe.renders == 0, "Disabled modules received callbacks");

    modules.key_event(65, true);
    modules.key_event(65, true);
    modules.key_event(65, true, true);
    check(probe.enabled() && probe.enables == 1, "Repeated key press toggled a module");
    modules.key_event(65, false);
    modules.key_event(65, true);
    check(!probe.enabled() && probe.disables == 1, "Fresh key press did not toggle");
    modules.reset_keys();
    modules.key_event(65, true, true);
    check(!probe.enabled(), "Repeat flag ignored after focus reset");
    modules.key_event(65, false);
    modules.key_event(65, true);
    probe.set_enabled(true);
    modules.tick(0.25F);
    modules.tick(-1.0F);
    modules.tick(std::numeric_limits<float>::quiet_NaN());
    modules.render();
    check(probe.ticks == 1 && probe.last_delta == 0.25F && probe.renders == 1,
          "Enabled callback dispatch or invalid delta filtering failed");
    modules.shutdown();
    modules.shutdown();
    check(!probe.enabled() && probe.disables == 2, "Shutdown is not idempotent");
}

void test_config_round_trip() {
    TestDirectory directory;
    const auto path = directory.path / "config.json";
    ConfigStore store(path);
    ModuleManager source;
    auto& original = source.add<ExampleModule>();
    std::string error = "stale";
    check(store.load(source, error) && error.empty(), "Missing config must use defaults");

    original.show_label.get<bool>() = false;
    original.scale.get<float>() = 1.75F;
    original.sample_count.get<int>() = 17;
    original.alignment.get<Choice>().index = 2;
    original.tint.get<Color>() = Color{0.25F, 0.5F, 0.75F, 0.5F};
    original.label.get<std::string>() = "Medicon text";
    original.set_keybind(112);
    original.set_enabled(true);
    check(store.save(source, error) && error.empty(), "Config save failed");
    original.label.get<std::string>() = "Second save";
    check(store.save(source, error), "Atomic overwrite of existing config failed");

    ModuleManager target;
    auto& restored = target.add<ExampleModule>();
    check(store.load(target, error) && error.empty(), "Config load failed");
    check(restored.enabled() && restored.keybind() == 112, "Module state did not round trip");
    check(restored.settings().size() == original.settings().size(), "Setting count changed");
    for (std::size_t index = 0; index < original.settings().size(); ++index) {
        check(restored.settings()[index].value == original.settings()[index].value,
              "A typed setting failed to round trip");
    }

    const auto valid_config = read_json(path);
    original.scale.get<float>() = std::numeric_limits<float>::infinity();
    check(!store.save(source, error) && !error.empty(), "Non-finite value persisted");
    check(read_json(path) == valid_config, "Failed save damaged the previous config");
    check(std::distance(std::filesystem::directory_iterator(directory.path),
                        std::filesystem::directory_iterator{}) == 1,
          "A temporary config was left behind");
}

void test_config_validation() {
    TestDirectory directory;
    const auto path = directory.path / "config.json";
    ConfigStore store(path);
    ModuleManager modules;
    auto& probe = modules.add<Probe>();
    auto& sample = modules.add<ExampleModule>();
    std::string error;

    const Json valid{{"version", 1}, {"modules", {
        {"probe", {{"enabled", true}, {"keybind", 66}, {"settings", {{"amount", 7}}}}},
        {"example", {{"settings", Json::object()}}}
    }}};

    const auto reject_setting = [&](const char* id, Json bad_value) {
        auto config = valid;
        config["modules"]["example"]["settings"][id] = std::move(bad_value);
        write_json(path, config);
        check(!store.load(modules, error) && !error.empty(), "Invalid setting value accepted");
        check(!probe.enabled() && probe.keybind() == 65 && probe.amount.get<int>() == 2 && probe.enables == 0,
              "Invalid config partially applied before validation completed");
    };
    reject_setting("show_label", 1);
    reject_setting("scale", 8.0);
    reject_setting("scale", "1.0");
    reject_setting("sample_count", 2.5);
    reject_setting("sample_count", 33);
    reject_setting("alignment", -1);
    reject_setting("alignment", 3);
    reject_setting("tint", Json::array({0.1, 0.2, 0.3}));
    reject_setting("tint", Json::array({0.1, 0.2, 1.01, 1.0}));
    reject_setting("label", false);

    for (const auto& bad_key : {Json(-1), Json(256), Json(2.5), Json("65"),
                               Json(std::numeric_limits<unsigned long long>::max())}) {
        auto config = valid;
        config["modules"]["probe"]["keybind"] = bad_key;
        write_json(path, config);
        check(!store.load(modules, error) && !probe.enabled(), "Invalid keybind accepted");
    }
    auto bad_version = valid;
    bad_version["version"] = 2;
    write_json(path, bad_version);
    check(!store.load(modules, error), "Unknown config version accepted");
    {
        std::ofstream stream(path);
        stream << "{broken";
    }
    check(!store.load(modules, error) && !error.empty(), "Malformed JSON accepted");

    auto future = valid;
    future["modules"]["future_module"] = {{"settings", {{"unused", true}}}};
    future["modules"]["example"]["settings"]["future_setting"] = true;
    write_json(path, future);
    check(store.load(modules, error) && error.empty(), "Unknown IDs should be ignored");
    check(probe.enabled() && probe.keybind() == 66 && probe.amount.get<int>() == 7,
          "Valid module config did not apply");
    check(probe.amount_on_enable == 7 && probe.enables == 1,
          "Enable hook ran before new settings were applied");
    check(sample.alignment.get<Choice>().options.size() == 3, "Choice metadata was changed by config");
}

void test_events() {
    struct Event { int value = 0; };
    struct Other { int value = 0; };
    EventBus bus;
    EventBus::Subscription later;
    EventBus::Subscription added;
    int first_calls = 0;
    int later_calls = 0;
    int added_calls = 0;
    auto first = bus.subscribe<Event>([&](Event& event) {
        ++first_calls;
        ++event.value;
        later.reset();
        if (first_calls == 1) {
            added = bus.subscribe<Event>([&](Event& nested) { ++added_calls; nested.value += 10; });
        }
    });
    later = bus.subscribe<Event>([&](Event&) { ++later_calls; });
    auto other = bus.subscribe<Other>([](Other& event) { ++event.value; });
    Event event;
    bus.publish(event);
    check(first_calls == 1 && later_calls == 0 && added_calls == 0 && event.value == 1,
          "Event mutation, unsubscribe, or dispatch snapshot failed");
    bus.publish(event);
    check(first_calls == 2 && added_calls == 1 && event.value == 12,
          "Subscription added during dispatch was not retained");
    first.reset();
    added.reset();
    bus.publish(event);
    check(event.value == 12, "Disconnected listener was called");
    Other separate;
    bus.publish(separate);
    check(separate.value == 1, "Typed event isolation failed");
    {
        auto transient = bus.subscribe<Event>([](Event& current) { current.value += 100; });
    }
    bus.publish(event);
    check(event.value == 12, "RAII unsubscribe failed");

    EventBus nested_bus;
    EventBus::Subscription self;
    int nested_calls = 0;
    self = nested_bus.subscribe<Event>([&](Event& current) {
        ++nested_calls;
        self.reset();
        nested_bus.publish(current);
    });
    nested_bus.publish(event);
    check(nested_calls == 1, "Self-disconnect during nested dispatch failed");

    EventBus::Subscription orphan;
    {
        EventBus temporary;
        orphan = temporary.subscribe<Event>([](Event&) {});
    }
    orphan.reset();
}

} // namespace

int main() {
    try {
        test_modules();
        test_config_round_trip();
        test_config_validation();
        test_events();
        std::cout << "Core tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Core test failure: " << exception.what() << '\n';
        return 1;
    }
}
