#include <medicon.hpp>
using namespace medicon;
using namespace std::chrono_literals;
class MotionExample : public Module {
    Tween<Vec2> position_{{24,270}};
    bool right_=false;
    void on_enable() override {
        every(2s,[this]{right_=!right_; position_.to({right_?240.F:24.F,270},500ms,Easing::InOutCubic);});
    }
    void on_render(RenderContext& render) override {
        auto draw=render.logical(); const auto p=position_.value();
        draw.fill_rounded_rect({p.x,p.y,120,36},12,theme::color(theme::Token::Surface));
        draw.text({p.x+12,p.y+10},"Motion",theme::color(theme::Token::TextPrimary),14);
    }
};
MEDICON_MODULE(MotionExample,"motion_example","Motion Example",Category::Visual);
