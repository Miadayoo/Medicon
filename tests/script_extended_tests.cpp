#include <medicon/scripts/ScriptManager.hpp>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <fstream>
#include <iostream>
#include <algorithm>

namespace {
using namespace medicon::scripts;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
BuildResult fixture(const fs::path& binary,std::string key) {
    BuildResult b; b.success=true; b.binary=binary; b.binary_hash=sha256_file(binary); b.manifest=inspect_plugin(binary);
    b.source.key=std::move(key); b.source.entry=binary; b.source.fingerprint=sha256(b.source.key); return b;
}
void storage_tests(const fs::path& root) {
    std::vector<std::string> reports;
    const auto report=[&](std::string_view s){reports.emplace_back(s);};
    {
        ScriptStorage store(root,report); store.start(); store.preload("one");store.preload("two");
        check(store.set("one","label","日本語"),"Unicode value accepted");
        check(!store.get("two","label"),"Storage isolated by module ID");
        check(!store.set("one","../escape","bad")&&!store.set("one","a/b","bad"),"Path keys rejected");
        check(!store.set("one","huge",std::string(ScriptStorage::quota,'x')),"Serialized quota enforced");
        check(!store.set("one","invalid",std::string(1,static_cast<char>(0xff))),"Invalid UTF-8 rejected");
        store.flush(); check(store.set("one","label","second"),"Updated cache");store.flush();
        check(fs::exists(root/(sha256("one")+".json.bak")),"Previous atomic version retained");
        store.stop();
    }
    {
        ScriptStorage store(root,report);store.start();store.preload("one");
        check(store.get("one","label")=="second","Restart loads persisted module storage");store.stop();
    }
    const auto path=root/(sha256("one")+".json"); {std::ofstream f(path);f<<"{ corrupt";}
    {ScriptStorage store(root,report);store.start();store.preload("one");check(!store.get("one","label"),"Corrupt storage starts with empty cache");store.stop();}
    check(fs::exists(fs::path(path.wstring()+L".corrupt"))&&!reports.empty(),"Corrupt source preserved and diagnosed");
    check(fs::file_size(path)==9,"Corrupt file not silently overwritten");
}
void runtime_tests(const fs::path& root) {
    fs::create_directories(root);ScriptTrustManager trust(root/"trust.json"); HostServices host;
    std::vector<std::string> logs; int cleaned=0,native=0,fallback=0,shapes=0,widgets=0,finished=0; bool joined=false,visible=false,focus=true;
    uint64_t world=17; double time=0;float width=1920;
    host.features=MEDICON_FEATURE_RENDER|MEDICON_FEATURE_ANIMATION|MEDICON_FEATURE_TIMERS|MEDICON_FEATURE_STORAGE|
        MEDICON_FEATURE_UI|MEDICON_FEATURE_INPUT|MEDICON_FEATURE_PLAYER;
    host.log=[&](uint32_t,std::string_view s){logs.emplace_back(s);};
    host.storage=std::make_shared<ScriptStorage>(root/"Storage",[](auto){});host.storage->start();host.storage->preload("extended_fixture");
    host.clock=[&]{return time;};host.render_size=[&]{return MediconPoint{width,1080};};
    host.frame=[&]{MediconFrameInfo f{};f.size=sizeof(f);f.width=width;f.height=1080;f.scale=2;f.flags=visible?MEDICON_FRAME_UI_VISIBLE:0;return f;};
    host.player=[&]()->std::optional<MediconPlayerSnapshot>{if(joined)return MediconPlayerSnapshot{};return {};};host.world_identity=[&]{return world;};
    host.input_focused=[&]{return focus;};
    host.glass=[&](auto owner,auto gen,const auto& r,const auto& s){check(owner=="extended_fixture"&&gen>0,"Owner and generation on every pane");check(r.x==20&&r.y>=20,"Logical render coordinates converted to physical pixels");
        if(s.fields&MEDICON_GLASS_BLUR){++fallback;return MEDICON_RESULT_FALLBACK;}++native;return MEDICON_RESULT_OK;};
    host.shape=[&](const auto& d){check(d.end.x==200&&d.thickness==4,"Primitive scaling");++shapes;return MEDICON_RESULT_OK;};
    host.ui_begin=[](auto,auto,auto,const auto&,const auto&){return 7u;};host.ui_end=[](auto token){check(token==7,"RAII panel end");};
    host.ui_widget=[&](auto token,const auto&){check(token==7,"Opaque panel token");++widgets;return 1;};host.ui_finish=[&]{++finished;};
    host.cleanup=[&](auto,uint64_t){++cleaned;};host.notify=[](auto,auto,auto,auto,uint32_t){return true;};host.sound=[](auto){return true;};
    medicon::ModuleManager modules;
    auto b=fixture(MEDICON_EXTENDED_FIXTURE,"extended");trust.approve(b);
    auto value=std::make_unique<ScriptModuleAdapter>(b,ScriptPluginLoader::load(b,trust,host.features),host);auto* module=value.get();modules.adopt(std::move(value));
    const auto count=[&](std::string_view needle){return std::ranges::count_if(logs,[&](const auto& s){return s.ends_with(needle);});};
    module->set_enabled(true);check(count("stored:empty")==1,"Storage available before enable");
    modules.tick(.05F);check(count("resize:960")==1,"Initial logical screen snapshot");
    time=.1;modules.tick(.05F);check(count("once")==1&&count("cancelled-timer-fired")==0,"One-shot and cancelled timers");
    time=.2;modules.tick(.05F);check(count("repeat")==1,"Repeating timer");
    time=5;modules.tick(.05F);check(count("repeat")==2,"Missed intervals coalesced after long pause");
    check(count("zero-repeat-rejected")==1,"Zero repeat does not become a one-shot");
    joined=true;modules.tick(.05F);check(count("join:17")==1,"GameJoined copied snapshot");
    world=18;modules.tick(.05F);check(count("leave:17")==1&&count("join:18")==1,"World switch emits leave then join");
    joined=false;modules.tick(.05F);check(count("leave:18")==1,"GameLeft snapshot");
    width=2560;modules.tick(.05F);check(count("resize:1280")==1,"Resize event units");
    modules.tick(.05F);check(count("resize:1280")==1,"No duplicate resized event");
    module->key_event(117,true,false);module->key_event(117,false,false);check(count("pressed")==1,"KeyPressed excludes releases");
    modules.render();check(native==1&&fallback==1&&shapes==1,"Native and unsupported fallback share host API");
    check(module->runtime_diagnostics().size()==1&&module->runtime_diagnostics()[0].code=="GLASS_UNSUPPORTED"&&module->runtime_diagnostics()[0].line>0,"Unsupported style diagnoses exact source line");
    module->ui();check(widgets==0,"No UI callbacks without host visibility");
    visible=true;host.keys[117]=true;module->ui();check(widgets==3&&finished==1&&count("button")==1&&count("held")==1,"Real DLL UI invokes host widgets and scoped input");
    focus=false;module->ui();check(count("held")==1,"No held keys without script panel focus");
    module->settings()[0].value=2;modules.render();check(native==20,"Per-module 20-panel quota");
    check(module->telemetry().rejected>0,"Quota rejection counted in telemetry");
    module->set_enabled(false);check(cleaned==1&&host.storage->get("extended_fixture","disabled")=="yes","Disable cleans owned resources and allows final persistence");
    time=99;modules.tick(.05F);module->ui();check(count("repeat")==2,"Disable cancels timers");
    module->settings()[0].value=1;module->set_enabled(true);time=99.2;modules.tick(.05F);
    check(!module->enabled()&&!module->healthy()&&cleaned>=2,"Timer exception disables and cleans only its module");
    module->drain();host.storage->stop();

    auto old=fixture(MEDICON_LEGACY_FIXTURE,"old");check(old.manifest.api_version==100,"Fixture compiled with frozen v0.1 headers");trust.approve(old);
    std::string chat;int circles=0;host.features=MEDICON_FEATURE_CHAT|MEDICON_FEATURE_RENDER;host.chat=[&](auto s){chat=s;return true;};host.circle=[&](auto,float,auto){++circles;};
    medicon::ModuleManager legacy_modules;
    auto legacy=std::make_unique<ScriptModuleAdapter>(old,ScriptPluginLoader::load(old,trust,host.features),host);auto* old_module=legacy.get();legacy_modules.adopt(std::move(legacy));
    old_module->set_enabled(true);legacy_modules.render();check(old_module->healthy()&&chat=="legacy:3"&&circles==1,"Frozen v0.1 DLL runs unchanged through prefix-sized ABI tables");old_module->drain();
}
}
int main(){
    const auto root=fs::temp_directory_path()/("medicon-extended-tests-"+std::to_string(GetCurrentProcessId()));
    try{fs::create_directories(root);storage_tests(root/"storage");runtime_tests(root/"runtime");
        if(ScriptPaths::contains(fs::temp_directory_path(),root))fs::remove_all(root);
        std::cout<<"Extended services and frozen binary compatibility passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<" (artifacts: "<<utf8(root)<<")\n";return 1;}
}
