#include <medicon.hpp>
using namespace medicon;
class GlassFallback : public Module {
    void on_render(RenderContext& render) override {
        auto draw=render.logical();
        const float x=std::max(24.F,draw.viewport_size().width-600.F);
        // Intentionally unsupported: emits a source/line diagnostic and uses GlassPlate.
        draw.glass({x,330,300,70},Glass::frosted().blur(18).tint(Color::rgb(40,80,120)));
        draw.text({x+20,354},"Readable shared fallback",Color::white(),16);
    }
};
MEDICON_MODULE(GlassFallback,"glass_fallback","Glass Fallback",Category::Visual);
