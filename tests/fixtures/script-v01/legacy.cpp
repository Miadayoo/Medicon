#include <medicon.hpp>
using namespace medicon;
class Legacy : public Module {
    Setting<int> amount=setting<int>("amount","Amount",3).range(1,8);
    void on_enable() override { chat::print("legacy:"+std::to_string(amount.get())); }
    void on_render(RenderContext& draw) override { draw.fill_circle(draw.center(),4); }
};
MEDICON_MODULE(Legacy,"legacy_v01","Frozen v0.1",Category::Utility);
