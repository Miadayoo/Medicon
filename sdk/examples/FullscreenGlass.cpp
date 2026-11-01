#include <medicon.hpp>
using namespace medicon;
class FullscreenGlass : public Module {
    void on_render(RenderContext& render) override {
        auto draw=render.logical(); const auto size=draw.viewport_size();
        draw.glass({0,0,size.width,size.height},Glass::regular().radius(24));
        draw.text({30,30},"Full viewport / same shared capture",Color::white(),22);
    }
};
MEDICON_MODULE(FullscreenGlass,"fullscreen_glass","Fullscreen Glass",Category::Visual);
