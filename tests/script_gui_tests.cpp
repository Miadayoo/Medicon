#include <medicon/gui/Accessibility.hpp>
#include <medicon/gui/Overlay.hpp>
#include <medicon/gui/LiquidGlass.hpp>
#include <medicon/gui/ScriptGlassBridge.hpp>
#include <medicon/core/ModuleManager.hpp>
#include <medicon/scripts/ScriptRuntime.hpp>
#include <iostream>
#include <cstring>

namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
MediconString text(const char* s){return {s,static_cast<uint32_t>(strlen(s))};}
void tests(){
    using namespace medicon;using namespace medicon::gui;
    {scripts::HostServices no_context;attach_script_glass_bridge(no_context,nullptr,nullptr);
        check(no_context.frame().flags==0&&!(no_context.capabilities()&MEDICON_FEATURE_GLASS),"Host polling before ImGui creation is safe");}
    auto* context=ImGui::CreateContext();
    {
        auto& io=ImGui::GetIO();io.DisplaySize={1920,1080};io.DeltaTime=1.F/60;io.IniFilename=nullptr;
        ModuleManager modules;Overlay overlay(modules);scripts::HostServices host;
        overlay.gui().set_open(false);
        override_accessibility_preferences(AccessibilityPreferences{false,false,false});
        attach_script_glass_bridge(host,&overlay.fonts(),&overlay);
        const auto begin=[&]{overlay.prepare_frame();unsigned char* bytes;int w,h;io.Fonts->GetTexDataAsRGBA32(&bytes,&w,&h);ImGui::NewFrame();};
        const auto end=[&]{host.ui_finish();ImGui::Render();};
        MediconGlassStyle style{};style.size=sizeof(style);style.version=1;style.refraction=style.opacity=style.saturation=1;style.tint={1,1,1,1};style.radius=20;style.fields=MEDICON_GLASS_RADIUS;
        begin();check(!(host.frame().flags&MEDICON_FRAME_UI_VISIBLE),"Script UI hidden while ClickGUI closed");
        check(host.ui_begin("test",1,"panel",{10,400,300,180},style)==0,"Host refuses UI when cursor is not owned");end();
        overlay.gui().set_open(true);
        uint32_t stale=0;ImVec2 button{};int clicks=0;
        const auto panel=[&]{
            ImGui::SetNextWindowFocus();
            auto token=host.ui_begin("test",1,"panel",{10,400,300,180},style);
            check(token!=0,"Existing ImGui panel created");
            if(stale)check(host.ui_widget(stale,{sizeof(MediconWidget),1,{},text("stale"),0})==0,"Previous-frame token rejected");
            const MediconWidget label{sizeof(MediconWidget),1,{},text("Readable fallback"),0};host.ui_widget(token,label);
            const MediconWidget widget{sizeof(MediconWidget),2,text("button"),text("Activate"),0};
            clicks+=host.ui_widget(token,widget);const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();button={(a.x+b.x)*.5F,(a.y+b.y)*.5F};
            check(!overlay.allows_game_movement(),"Focused panel blocks GUI game movement");
            const MediconWidget progress{sizeof(MediconWidget),3,text("progress"),{},.5F};host.ui_widget(token,progress);
            stale=token;
        };
        begin();panel();end();
        io.AddMousePosEvent(button.x,button.y);begin();panel();end();
        io.AddMouseButtonEvent(0,true);begin();panel();end();
        begin();panel();end();
        io.AddMouseButtonEvent(0,false);begin();panel();end();
        begin();panel();end();check(clicks==1,"Existing button fires exactly once for press/hold/release");
        begin();
        LiquidGlassFrame pane;pane.owner="owner";pane.generation=1;pane.size={100,60};pane.center={50,50};
        check(publish_liquid_glass(pane),"Native publisher accepts owner-tagged pane");pane.generation=2;publish_liquid_glass(pane);
        remove_liquid_glass("owner",1);check(current_liquid_glass_frames().size()==1&&current_liquid_glass()->generation==2,"Cleanup removes only the retired generation");
        for(int i=0;i<100;++i)publish_liquid_glass(pane);check(current_liquid_glass_frames().size()==63,"Shared frame quota reserves native slot");
        pane.owner.clear();check(publish_liquid_glass(pane)&&current_liquid_glass_frames().size()==64,"Native HUD retains reserved slot");end();
        begin();check(current_liquid_glass_frames().empty(),"Panes never persist into next frame");
        override_accessibility_preferences(AccessibilityPreferences{true,true,true});
        const auto f=host.frame();check((f.flags&MEDICON_FRAME_REDUCED_MOTION)&&(f.flags&MEDICON_FRAME_REDUCED_TRANSPARENCY)&&(f.flags&MEDICON_FRAME_HIGH_CONTRAST),"Shared accessibility preferences exposed");
        const auto vertices=ImGui::GetBackgroundDrawList()->VtxBuffer.Size;
        check(host.glass("test",1,{10,10,200,80},style)==MEDICON_RESULT_FALLBACK,"Unavailable optics use shared plate");
        check(ImGui::GetBackgroundDrawList()->VtxBuffer.Size>vertices,"Fallback draws a readable surface");
        check(!(host.capabilities()&MEDICON_FEATURE_GLASS),"Capabilities do not claim unavailable native effect");
        Toast one;one.owner="test";one.generation=1;one.title="Owned";
        Toast two=one;two.generation=2;overlay.social().toasts().push(one);overlay.social().toasts().push(two);
        host.cleanup("test",1);check(overlay.social().toasts().visible().size()+overlay.social().toasts().queued()==1,"Cleanup removes only the matching toast generation");end();
        begin();
        for(int i=0;i<64;++i) {
            const auto token=host.ui_begin("pool",3,std::to_string(i),{1250,500,200,100},style);
            check(token!=0,"Bounded window pool accepts its live slots");host.ui_end(token);
        }
        check(!host.ui_begin("pool",3,"overflow",{1250,500,200,100},style),"Window state pool is bounded across panel IDs");end();
        host.cleanup("pool",3);begin();
        const auto replacement=host.ui_begin("replacement",4,"new",{1250,500,200,100},style);
        check(replacement!=0,"Reload releases and reuses window state slots");host.ui_end(replacement);end();
        io.AddFocusEvent(false);begin();check(!(host.frame().flags&MEDICON_FRAME_UI_VISIBLE),"Focus loss suspends script UI");end();
        override_accessibility_preferences(AccessibilityPreferences{});
    }
    forget_liquid_glass(context);ImGui::DestroyContext(context);
}
}
int main(){try{tests();std::cout<<"Script GUI focus, single activation, frame tokens, fallback and cleanup passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
