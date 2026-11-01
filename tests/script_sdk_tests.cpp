#include <medicon.hpp>
#include <iostream>
#include <map>
#include <array>
using namespace medicon;

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Host {
    std::map<std::string, MediconValue> settings;
    std::string last_chat, last_log;
    int circles = 0, texts = 0;
    double time=0;
    uint32_t flags=0;
};
int ticks = 0, keys = 0, renders = 0;
class Probe final : public Module {
    Setting<float> radius = setting<float>("radius", "Radius", 4).range(1, 20, .5F);
    Setting<bool> active = setting<bool>("active", "Active", true);
    void on_enable() override {
        chat::print("Hello SDK");
        check(active.get(), "Typed bool setting");
        check(radius.get() == 7, "Host setting is read dynamically");
        check(features::is_available("render") && !features::is_available("missing"), "Feature query");
        check(!game::local_player(), "Missing player is optional");
        listen<TickEvent>([](const TickEvent& e) { check(e.delta_seconds == .05F, "Tick payload"); ++ticks; });
        listen<KeyEvent>([](const KeyEvent& e) { check(e.key == 65, "Key payload"); ++keys; });
        listen<RenderEvent>([](const RenderEvent&) { ++renders; });
    }
    void on_render(RenderContext& context) override {
        check(context.center().x == 960 && context.center().y == 540, "Render center");
        context.fill_circle(context.center(), radius.get()); context.text({10, 10}, "Test");
    }
};
}
MEDICON_MODULE(Probe, "sdk_probe", "SDK Probe", Category::Utility);

int main() {
    try {
        Host data;
        MediconHostApi host{}; host.size = sizeof(host); host.abi_version = sdk::abi_version;
        host.features = MEDICON_FEATURE_RENDER; host.context = &data;
        host.log = [](void* p, uint32_t, MediconString s) { static_cast<Host*>(p)->last_log.assign(s.data, s.size); };
        host.chat = [](void* p, MediconString s) -> int32_t { static_cast<Host*>(p)->last_chat.assign(s.data, s.size); return 1; };
        host.define_setting = [](void* p, const MediconSettingDescriptor* d) -> int32_t {
            return static_cast<Host*>(p)->settings.emplace(std::string(d->id.data, d->id.size), d->initial).second;
        };
        host.setting_range = [](void*, MediconString, float a, float b, float step) -> int32_t { return a <= b && step > 0; };
        host.read_setting = [](void* p, MediconString id, MediconValue* out) -> int32_t {
            *out = static_cast<Host*>(p)->settings.at(std::string(id.data, id.size)); return 1;
        };
        host.local_player = [](void*, MediconPlayerSnapshot*) -> int32_t { return 0; };
        host.render_size = [](void*) { return MediconPoint{1920,1080}; };
        host.fill_circle = [](void* p, MediconPoint, float r, MediconColor) { check(r == 7, "Circle radius"); ++static_cast<Host*>(p)->circles; };
        host.text = [](void* p, MediconPoint, MediconString, MediconColor, float) { ++static_cast<Host*>(p)->texts; };
        MediconPluginApi plugin{}; plugin.size = sizeof(plugin);
        check(!medicon_script_entry(999, &plugin), "ABI mismatch refused");
        check(medicon_script_entry(sdk::abi_version, &plugin) != 0, "C ABI entry");
        check(plugin.create(nullptr) == nullptr, "Invalid host table fails closed");
        auto* object = plugin.create(&host); check(object != nullptr, "Create module with private overrides");
        data.settings["radius"].number = 7;
        check(plugin.enable(object) && data.last_chat == "Hello SDK", "Enable calls host");
        check(plugin.tick(object, .05F) && ticks == 1, "Scoped tick listener");
        const MediconScriptKeyEvent key{65, 1, 0};
        check(plugin.event(object, MEDICON_EVENT_KEY, &key) && keys == 1, "C event bridge");
        check(plugin.render(object) && renders == 1 && data.circles == 1 && data.texts == 1, "Render facade");
        check(plugin.disable(object), "Disable module");
        check(plugin.tick(object, .05F) && ticks == 1, "Listeners automatically disconnected");
        check(plugin.enable(object) && plugin.tick(object, .05F) && ticks == 2, "Re-enable registers listeners once");
        plugin.disable(object); plugin.destroy(object);
        host.features|=MEDICON_FEATURE_ANIMATION;
        host.frame_info=[](void* p,MediconFrameInfo* f)->int32_t {auto& h=*static_cast<Host*>(p);f->now_seconds=h.time;f->flags=h.flags;f->width=1920;f->height=1080;f->scale=2;return 1;};
        {
            detail::HostScope scope(&host);
            Tween<float> tween{0};tween.to(10.F,std::chrono::seconds(1),Easing::Linear);data.time=.5;
            check(std::abs(tween.value()-5)<.001F,"Tween follows host monotonic clock");
            tween.to(20.F,std::chrono::seconds(1),Easing::Linear);check(tween.value()==5,"Interrupted tween is continuous");
            data.time=1;check(std::abs(tween.value()-12.5F)<.001F,"Interrupted tween destination");
            Tween<Vec2> point{{0,10}};point.to({10,20},std::chrono::seconds(1),Easing::Linear);data.time=1.5;
            check(point.value().x==5&&point.value().y==15,"Vector interpolation");
            Tween<Color> color{Color::transparent()};color.to(Color::white(),std::chrono::seconds(1),Easing::Linear);data.time=2;
            check(color.value().a==.5F,"Color interpolation");
            data.flags=MEDICON_FRAME_REDUCED_MOTION;check(tween.value()==20&&point.value().x==10,"Reduced motion snaps to target");
            tween.to(3.F,std::chrono::seconds(1));check(tween.value()==3,"Reduced motion applies to new transitions");data.flags=0;
            check(sdk::api_version()==400&&sdk::abi_version()==1,"Callable version API preserves constant compatibility");
            auto style=Glass::regular().radius(24).refraction(.2F).descriptor();
            check(style.radius==24&&style.refraction==.2F&&style.fields==(MEDICON_GLASS_RADIUS|MEDICON_GLASS_REFRACTION),"Glass descriptor is a pure copied value");
            host.size=MEDICON_HOST_V1_SIZE;
            check(!detail::extended()&&input::cursor_position().x==-1&&!storage::get("key"),"Legacy host table never reads appended functions");
            host.size=sizeof(host);
        }
        alignas(MediconPluginApi) std::array<unsigned char,MEDICON_PLUGIN_V1_SIZE+8> prefix{};
        std::fill(prefix.begin()+MEDICON_PLUGIN_V1_SIZE,prefix.end(),0xAD);
        auto* old_table=reinterpret_cast<MediconPluginApi*>(prefix.data());old_table->size=MEDICON_PLUGIN_V1_SIZE;
        check(medicon_script_entry(1,old_table)&&old_table->size==MEDICON_PLUGIN_V1_SIZE,"New entry negotiates old prefix");
        check(std::all_of(prefix.begin()+MEDICON_PLUGIN_V1_SIZE,prefix.end(),[](auto b){return b==0xAD;}),"Entry preserves memory past caller table capacity");
        check(std::string(medicon_script_manifest.id) == "sdk_probe", "Exported metadata");
        std::cout << "Script SDK mock host tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
