#include <medicon.hpp>
using namespace medicon;
using namespace std::chrono_literals;
class AnimatedToast : public Module {
    Tween<float> opacity_{0};
    void on_enable() override {
        opacity_.to(1.F,260ms,Easing::OutCubic);
        after(500ms,[]{notification::show("Script ready","Uses Medicon's existing notification queue");});
    }
    void on_render(RenderContext& render) override {
        auto draw=render.logical();
        draw.glass({24,130,300,64},Glass::regular().radius(20).opacity(opacity_.value()));
        draw.text({44,152},"Motion follows system preference",{1,1,1,opacity_.value()},13);
    }
};
MEDICON_MODULE(AnimatedToast,"animated_toast","Animated Toast",Category::Visual);
