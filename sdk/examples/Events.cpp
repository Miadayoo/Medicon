#include <medicon.hpp>
using namespace medicon;
class EventExample : public Module {
    void on_enable() override {
        listen<GameJoined>([](const GameJoined&){log::info("Joined a world");});
        listen<GameLeft>([](const GameLeft&){log::info("Left the world");});
        listen<ScreenResized>([](const ScreenResized& event){log::info("Scale: "+std::to_string(event.scale));});
        listen<KeyPressed>([](const KeyPressed& event){if(event.key==Key::F6&&!event.repeat) notification::show("F6","Event received");});
    }
};
MEDICON_MODULE(EventExample,"event_example","Event Example",Category::Utility);
