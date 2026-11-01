#include <medicon/gui/ScriptGlassBridge.hpp>
#include <medicon/scripts/ScriptRuntime.hpp>
#include <medicon/gui/Accessibility.hpp>
#include <medicon/gui/Glass.hpp>
#include <medicon/gui/LiquidGlass.hpp>
#include <medicon/gui/Overlay.hpp>
#include <medicon/gui/Theme.hpp>
#include <algorithm>
#include <memory>
#include <set>
#include <vector>

namespace medicon::gui {
namespace {
ImU32 packed(MediconColor c) { return ImGui::ColorConvertFloat4ToU32({c.r,c.g,c.b,c.a}); }
std::string_view string(MediconString s) { return {s.data?s.data:"",s.size}; }
bool supported(const MediconGlassStyle& s) {
    constexpr auto fields=MEDICON_GLASS_RADIUS|MEDICON_GLASS_REFRACTION|MEDICON_GLASS_OPACITY|MEDICON_GLASS_QUALITY;
    return s.preset==0&&!(s.fields&~fields)&&(!(s.fields&MEDICON_GLASS_QUALITY)||s.quality==0);
}
struct Bridge {
    FontManager* fonts=nullptr; Overlay* overlay=nullptr;
    struct Scope { uint32_t token; bool visible; };
    std::vector<Scope> stack;
    struct Allocation { std::string key,owner; uint64_t generation=0; };
    // Reuse a fixed ImGui window pool; generation-specific window names would
    // otherwise leave an unbounded ImGui window cache across hot reloads.
    std::array<Allocation,64> allocations{};
    std::set<std::string> ids;
    uint32_t serial=0;
    int frame=-1;
    bool focused=false;
    int32_t ui_result=MEDICON_RESULT_OK;
    float scale() const { return overlay?overlay->frame().ui_scale:1.F; }
    bool visible() const {
        return ImGui::GetCurrentContext()&&overlay&&overlay->gui().open()&&!overlay->main_menu().active()&&!overlay->disconnect_screen().visible()&&
            !overlay->social().screen_visible()&&!overlay->social().screen_open()&&!overlay->splash_active()&&!ImGui::GetIO().AppFocusLost;
    }
    int32_t glass(std::string_view owner,uint64_t generation,MediconRect r,const MediconGlassStyle& style,ImDrawList* list,bool covered=false) {
        const auto viewport=ImGui::GetIO().DisplaySize;
        if(r.x>=viewport.x||r.y>=viewport.y||r.x+r.width<=0||r.y+r.height<=0) return MEDICON_RESULT_OK;
        if(!covered&&supported(style)&&liquid_glass_ready()) {
            LiquidGlassFrame pane;
            pane.center={r.x+r.width*.5F,r.y+r.height*.5F}; pane.size={r.width,r.height}; pane.scale=scale();
            pane.radius=(style.fields&MEDICON_GLASS_RADIUS)?style.radius:-1.F;
            pane.lens=style.refraction; pane.opacity=style.opacity; pane.owner=owner; pane.generation=generation;
            if(publish_liquid_glass(pane)) return MEDICON_RESULT_OK;
            // The frame-wide native budget is shared with built-in widgets.
        }
        GlassPlate plate; plate.fill=theme::panel_background; plate.fill_opacity=1; plate.alpha=style.opacity;
        list->PushClipRect({0,0},viewport,true);
        draw_glass_plate(list,{r.x,r.y},{r.x+r.width,r.y+r.height},
            std::clamp((style.fields&MEDICON_GLASS_RADIUS)?style.radius:24.F*scale(),0.F,std::min(r.width,r.height)*.5F),plate);
        list->PopClipRect();
        return MEDICON_RESULT_FALLBACK;
    }
    uint32_t begin(std::string_view owner,uint64_t generation,std::string_view id,MediconRect r,MediconGlassStyle style) {
        if(!visible()) return 0;
        ImGui::GetIO().ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
        if(frame!=ImGui::GetFrameCount()) { ids.clear(); frame=ImGui::GetFrameCount(); }
        const std::string key=std::string(owner)+":"+std::to_string(generation)+":"+std::string(id);
        auto slot=std::find_if(allocations.begin(),allocations.end(),[&](const auto& value){return value.key==key;});
        bool fresh=false;
        if(slot==allocations.end()) {
            slot=std::find_if(allocations.begin(),allocations.end(),[](const auto& value){return value.key.empty();});
            if(slot==allocations.end()) return 0;
            *slot={key,std::string(owner),generation};fresh=true;
        }
        const std::string name="Script / "+std::string(owner)+"###medicon-script-slot-"+std::to_string(slot-allocations.begin());
        if(!ids.insert(name).second) return 0;
        const float s=scale(); r={r.x*s,r.y*s,r.width*s,r.height*s}; style.radius*=s;
        ImGui::SetNextWindowPos({r.x,r.y},ImGuiCond_Always);
        ImGui::SetNextWindowSize({r.width,r.height},ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{16*s,14*s});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,style.radius);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{4*s,3*s});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{8*s,4*s});
        ImGui::PushStyleColor(ImGuiCol_Text,packed({theme::primary_text.r,theme::primary_text.g,theme::primary_text.b,1}));
        const bool shown=ImGui::Begin(name.c_str(),nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove|
            ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoCollapse);
        ImGui::PushID(key.c_str());
        if(fresh) ImGui::SetScrollY(0);
        if(!++serial) ++serial;
        stack.push_back({serial,shown});
        if(fonts) ImGui::PushFont(fonts->face(FontRole::Label).font); else ImGui::PushFont(ImGui::GetFont());
        if(shown) {
            // Existing optics run before ImGui and cannot see an opaque ClickGUI.
            // Reuse its shared readable plate when the native background would be hidden.
            bool covered=false;
            if(overlay) for(const auto& panel:overlay->gui().panel_rects())
                if(r.x<panel.x+panel.w&&r.x+r.width>panel.x&&r.y<panel.y+panel.h&&r.y+r.height>panel.y) {covered=true;break;}
            ui_result=glass(owner,generation,r,style,ImGui::GetWindowDrawList(),covered);
            focused=focused||ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            if(overlay&&(focused||ImGui::IsWindowHovered())) overlay->set_script_ui_focused(true);
        }
        return serial;
    }
    void end(uint32_t token) {
        if(stack.empty()||stack.back().token!=token) return;
        ImGui::PopFont(); ImGui::PopID(); ImGui::End(); ImGui::PopStyleColor(); ImGui::PopStyleVar(4); stack.pop_back();
    }
    void finish() { while(!stack.empty()) end(stack.back().token); focused=false; }
    int32_t widget(uint32_t token,const MediconWidget& w) {
        if(stack.empty()||stack.back().token!=token||!stack.back().visible) return 0;
        const auto text=string(w.text),id=string(w.id);
        if(fonts&&!text.empty()) fonts->add_text(text);
        if(w.kind==1) { ImGui::TextUnformatted(text.data(),text.data()+text.size()); return 1; }
        ImGui::PushID(id.data(),id.data()+id.size());
        bool result=false;
        if(w.kind==2) result=ImGui::Button(std::string(text).c_str());
        else if(w.kind==3) { ImGui::ProgressBar(w.value,{-1,0}); result=true; }
        ImGui::PopID(); return result?1:0;
    }
};
}
void attach_script_glass_bridge(scripts::HostServices& host,FontManager* fonts,Overlay* overlay) {
    auto state=std::make_shared<Bridge>(); state->fonts=fonts; state->overlay=overlay;
    host.frame=[state] {
        MediconFrameInfo f{}; f.size=sizeof(f);
        const auto size=ImGui::GetCurrentContext()?ImGui::GetIO().DisplaySize:ImVec2{state->overlay?state->overlay->frame().client_w:0.F,state->overlay?state->overlay->frame().client_h:0.F};
        f.width=size.x; f.height=size.y; f.scale=state->scale();
        const auto prefs=accessibility_preferences;
        if(state->visible()) f.flags|=MEDICON_FRAME_UI_VISIBLE;
        if(prefs.reduced_motion) f.flags|=MEDICON_FRAME_REDUCED_MOTION;
        if(prefs.reduced_transparency) f.flags|=MEDICON_FRAME_REDUCED_TRANSPARENCY;
        if(prefs.high_contrast) f.flags|=MEDICON_FRAME_HIGH_CONTRAST;
        return f;
    };
    host.capabilities=[state] {
        uint64_t flags=MEDICON_FEATURE_RENDER|MEDICON_FEATURE_THEME|MEDICON_FEATURE_ANIMATION|MEDICON_FEATURE_STORAGE|MEDICON_FEATURE_TIMERS;
        if(state->overlay) flags|=MEDICON_FEATURE_UI|MEDICON_FEATURE_INPUT|MEDICON_FEATURE_NOTIFICATIONS|MEDICON_FEATURE_SOUND;
        if(liquid_glass_ready()) flags|=MEDICON_FEATURE_GLASS|MEDICON_FEATURE_REFRACTION|MEDICON_FEATURE_GLASS_RADIUS|MEDICON_FEATURE_GLASS_OPACITY;
        return flags;
    };
    host.cursor=[state] { if(!state->visible())return MediconPoint{-1,-1};const auto pos=ImGui::GetIO().MousePos; return state->focused&&ImGui::IsMousePosValid(&pos)?MediconPoint{pos.x/state->scale(),pos.y/state->scale()}:MediconPoint{-1,-1}; };
    host.input_focused=[state]{return state->visible()&&state->focused;};
    host.shape=[](const MediconDrawCommand& d) {
        auto* list=ImGui::GetBackgroundDrawList(); list->PushClipRect({0,0},ImGui::GetIO().DisplaySize,true);
        const ImVec2 a{d.rect.x,d.rect.y},b{d.rect.x+d.rect.width,d.rect.y+d.rect.height};
        if(d.kind==1) list->AddRectFilled(a,b,packed(d.color),d.radius);
        else if(d.kind==2) list->AddRect(a,b,packed(d.color),d.radius,0,d.thickness);
        else if(d.kind==3) list->AddLine(a,{d.end.x,d.end.y},packed(d.color),d.thickness);
        list->PopClipRect(); return MEDICON_RESULT_OK;
    };
    host.glass=[state](auto owner,auto gen,const auto& r,const auto& style){return state->glass(owner,gen,r,style,ImGui::GetBackgroundDrawList());};
    host.ui_begin=[state](auto owner,auto gen,auto id,const auto& r,const auto& style){return state->begin(owner,gen,id,r,style);};
    host.ui_end=[state](auto token){state->end(token);};
    host.ui_status=[state]{return state->ui_result;};
    host.ui_finish=[state]{state->finish();};
    host.ui_widget=[state](auto token,const auto& w){return state->widget(token,w);};
    host.theme_color=[](uint32_t token) {
        const Color values[]{theme::primary_text,theme::secondary_text,theme::accent,theme::panel_background,theme::border};
        auto c=values[std::min(token,4u)]; return MediconColor{c.r,c.g,c.b,c.a};
    };
    host.notify=[state](auto owner,auto gen,auto title,auto body,uint32_t) {
        return state->overlay&&state->fonts&&state->overlay->social().script_notification(owner,gen,title,body,*state->fonts,state->overlay->in_menu());
    };
    host.sound=[state](uint32_t){return state->overlay&&state->overlay->social().script_sound(state->overlay->in_menu());};
    host.cleanup=[state](auto owner,auto gen) {
        state->finish(); remove_liquid_glass(owner,gen);
        for(auto& slot:state->allocations)if(slot.owner==owner&&slot.generation==gen)slot={};
        if(state->overlay) { state->overlay->social().toasts().remove_owner(owner,gen); state->overlay->set_script_ui_focused(false); }
    };
    host.features|=host.capabilities();
}
}
