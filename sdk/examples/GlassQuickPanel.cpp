#include <medicon.hpp>
using namespace medicon;
class GlassQuickPanel : public Module {
    float progress_=0;
    void on_ui(UIContext& ui) override {
        // Only interactive while ClickGUI owns the cursor. Do not retain panel.
        const float x=std::max(24.F,ui.viewport_size().width-600.F);
        auto panel=ui.glass_panel("quick",{x,150,300,160},Glass::regular().radius(20));
        panel.label("Quick controls");
        if(panel.button("advance","Advance")) progress_=std::min(1.F,progress_+.1F);
        panel.progress("progress",progress_);
    }
};
MEDICON_MODULE(GlassQuickPanel,"glass_quick_panel","Glass Quick Panel",Category::Visual);
