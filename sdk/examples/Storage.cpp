#include <medicon.hpp>
using namespace medicon;
class StorageExample : public Module {
    int visits_=0;
    void on_enable() override {
        if(auto value=storage::get("visits")) { try { visits_=std::stoi(*value); } catch(...) { visits_=0; } }
        visits_=std::clamp(visits_,0,1000000)+1;
        storage::set("visits",std::to_string(visits_));
    }
    void on_render(RenderContext& draw) override { draw.text({24,230},"Enabled "+std::to_string(visits_)+" times"); }
};
MEDICON_MODULE(StorageExample,"storage_example","Storage Example",Category::Utility);
