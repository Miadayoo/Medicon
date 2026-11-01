#include <medicon.hpp>
using namespace medicon;

class CustomHUD : public Module {
    Setting<float> radius = setting<float>("radius", "Radius", 4).range(1, 20, .5F);
    void on_render(RenderContext& context) override {
        context.fill_circle(context.center(), radius.get(), Color{0, 1, .8F, 1});
        if (const auto player = game::local_player())
            context.text({12, 64}, "Health: " + std::to_string(player->health));
    }
};
MEDICON_MODULE(CustomHUD, "custom_hud", "Custom HUD", Category::Visual);
