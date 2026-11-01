#include <medicon.hpp>
using namespace medicon;
using namespace std::chrono_literals;
class Extended : public Module {
    Setting<int> mode=setting<int>("mode","Mode",0).range(0,2);
    void on_enable() override {
        log::info("stored:"+storage::get("value").value_or("empty"));
        storage::set("value","persisted");
        after(100ms,[this]{log::info("once");if(mode.get()==1)throw std::runtime_error("timer failure");});
        every(200ms,[]{log::info("repeat");});
        const auto cancel=after(50ms,[]{log::info("cancelled-timer-fired");}); cancel_timer(cancel);
        if(every(0ms,[]{})==0) log::info("zero-repeat-rejected");
        listen<GameJoined>([](const GameJoined& e){log::info("join:"+std::to_string(e.world_identity));});
        listen<GameLeft>([](const GameLeft& e){log::info("leave:"+std::to_string(e.world_identity));});
        listen<ScreenResized>([](const ScreenResized& e){log::info("resize:"+std::to_string(static_cast<int>(e.width)));});
        listen<KeyPressed>([](const KeyPressed& e){if(e.key==Key::F6)log::info("pressed");});
        notification::show("Test","Existing toast"); sound::play_ui(sound::Builtin::Success);
    }
    void on_disable() override { storage::set("disabled","yes"); }
    void on_render(RenderContext& render) override {
        auto draw=render.logical();
        draw.glass({10,20,100,60},Glass::regular().radius(18));
        draw.glass({10,90,100,60},Glass::regular().blur(8));
        draw.line({0,0},{100,100},Color::white(),2);
        if(mode.get()==2)for(int i=0;i<30;++i)draw.glass({10,10,20,20});
    }
    void on_ui(UIContext& ui) override {
        auto panel=ui.glass_panel("test",{10,200,200,120}); panel.label("Test");
        if(panel.button("button","Activate"))log::info("button"); panel.progress("progress",.5F);
        if(input::key_down(Key::F6))log::info("held");
    }
};
MEDICON_MODULE(Extended,"extended_fixture","Extended Fixture",Category::Visual);
