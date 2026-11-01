#include <medicon.hpp>
using namespace medicon;
class GlassHUD : public Module {
    Setting<float> strength=setting<float>("strength","Refraction",.75F).range(0,1,.05F);
    void on_render(RenderContext& render) override {
        auto draw=render.logical();
        const float x=std::max(24.F,draw.viewport_size().width-600.F);
        draw.glass({x,24,300,86},Glass::regular().radius(24).refraction(strength.get()));
        draw.text({x+20,48},"Medicon Glass",Color::white(),20);
        draw.text({x+20,76},"Shared native optics",Color::white(),13);
    }
};
MEDICON_MODULE(GlassHUD,"glass_hud","Glass HUD",Category::Visual);
