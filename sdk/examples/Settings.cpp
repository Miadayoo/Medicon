#include <medicon.hpp>
using namespace medicon;

class SettingsExample : public Module {
    Setting<bool> greeting = setting<bool>("greeting", "Greeting", true);
    Setting<float> radius = setting<float>("radius", "Radius", 4).range(1, 20, .5F);
    Setting<int> count = setting<int>("count", "Count", 1).range(1, 10);
    Setting<Color> tint = setting<Color>("tint", "Tint", Color{1, .5F, .2F, 1});
    Setting<std::string> label = setting<std::string>("label", "Label", "Medicon");
    void on_enable() override {
        if (greeting.get()) chat::print(label.get());
        log::info("Radius: " + std::to_string(radius.get()) + "; count: " + std::to_string(count.get()));
    }
    void on_render(RenderContext& context) override {
        context.fill_circle(context.center(), radius.get(), tint.get());
    }
};
MEDICON_MODULE(SettingsExample, "settings_example", "Settings Example", Category::Utility);
