#include <medicon/scripts/ScriptRuntime.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace medicon::scripts {
namespace {
std::string_view str(MediconString s,size_t limit=16384) {
    if (s.size > limit || (s.size && !s.data)) throw std::invalid_argument("Invalid SDK string/length");
    return s.size ? std::string_view(s.data,s.size) : std::string_view{};
}
bool rectangle(const MediconRect& r) {
    for (float x : {r.x,r.y,r.width,r.height}) if (!std::isfinite(x) || std::abs(x)>65536) return false;
    return r.width>0 && r.height>0 && r.width<=16384 && r.height<=16384;
}
bool color(MediconColor c) { for (float v : {c.r,c.g,c.b,c.a}) if (!std::isfinite(v) || v<0 || v>1) return false; return true; }
}
double ScriptModuleAdapter::now() const {
    if (services_.clock) return services_.clock();
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void ScriptModuleAdapter::measured(std::chrono::steady_clock::time_point start) noexcept {
    const auto us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
    ++telemetry_.callbacks; telemetry_.total_us+=us; telemetry_.max_us=std::max(telemetry_.max_us,us);
}
void ScriptModuleAdapter::diagnostic(std::string code,std::string message,const MediconCallSite* site) {
    if (reported_.size()>=64 || !reported_.insert(code).second) return;
    ScriptDiagnostic d{source_,0,0,"warning",std::move(code),std::move(message),"Check feature::available and SDK/docs/19-existing-liquid-glass-bridge.md; the previous module remains usable."};
    if (site) { if (site->file.size) d.file=path_from_utf8(str(site->file)); d.line=static_cast<int>(std::min(site->line,static_cast<uint32_t>(INT32_MAX))); d.column=static_cast<int>(std::min(site->column,static_cast<uint32_t>(INT32_MAX))); }
    if (services_.log) services_.log(1,id()+" generation="+std::to_string(generation_)+" "+d.code+": "+d.message);
    diagnostics_.push_back(std::move(d));
}
bool ScriptModuleAdapter::command(std::string_view api) {
    if (++commands_<=2048) { ++telemetry_.draw_commands; return true; }
    ++telemetry_.rejected; diagnostic("COMMAND_QUOTA",std::string(api)+": callback exceeds 2048 commands"); return false;
}
bool ScriptModuleAdapter::glass_style(const MediconGlassStyle& s,const MediconCallSite* site) {
    constexpr uint32_t supported=MEDICON_GLASS_RADIUS|MEDICON_GLASS_REFRACTION|MEDICON_GLASS_OPACITY|MEDICON_GLASS_QUALITY;
    const bool ok=s.preset==0 && !(s.fields&~supported) && (!((s.fields&MEDICON_GLASS_QUALITY) && s.quality));
    if (!ok) diagnostic("GLASS_UNSUPPORTED","Glass: requested preset/style is not exposed by Medicon's existing optics; using the shared GlassPlate fallback",site);
    return ok;
}
void ScriptModuleAdapter::cleanup() noexcept {
    timers_.clear();
    try { if (services_.cleanup) services_.cleanup(id(),generation_); } catch (...) {}
}
void ScriptModuleAdapter::timers_tick() {
    const auto time=now(); std::vector<uint64_t> due;
    for (const auto& [id,timer] : timers_) if (timer->due<=time) due.push_back(id);
    for (auto id : due) {
        if (!enabled() || retired_) break;
        const auto found=timers_.find(id); if(found==timers_.end()) continue;
        auto timer=found->second; // pins the DLL-owned closure until the callback returns.
        if (!timer->interval) timers_.erase(found);
        if (!timer->callback(timer->data)) { failed("timer callback"); break; }
        if (timer->interval && timers_.contains(id)) timer->due=time+timer->interval;
    }
}
void ScriptModuleAdapter::lifecycle_events() {
    const bool present=services_.player && services_.player().has_value();
    const uint64_t world=present ? (services_.world_identity ? services_.world_identity() : 1) : 0;
    if (world_present_ && (!present || world!=previous_world_)) {
        const MediconWorldEvent e{previous_world_}; if (!plugin_.event(object_,MEDICON_EVENT_GAME_LEFT,&e)) { failed("GameLeft"); return; }
    }
    if (present && (!world_present_ || world!=previous_world_)) {
        const MediconWorldEvent e{world}; if (!plugin_.event(object_,MEDICON_EVENT_GAME_JOINED,&e)) { failed("GameJoined"); return; }
    }
    world_present_=present; previous_world_=world;
    MediconFrameInfo f{}; f.size=sizeof(f);
    if (!frame_info(this,&f)) return;
    if (!screen_seen_ || previous_screen_.width!=f.width || previous_screen_.height!=f.height || previous_screen_.scale!=f.scale) {
        previous_screen_={f.width/std::max(.01F,f.scale),f.height/std::max(.01F,f.scale),f.scale};
        // Compare physical dimensions separately to avoid scaling oscillation.
        const MediconScreenEvent e=previous_screen_;
        previous_screen_.width=f.width; previous_screen_.height=f.height; screen_seen_=true;
        if (!plugin_.event(object_,MEDICON_EVENT_SCREEN_RESIZED,&e)) failed("ScreenResized");
    }
}
void ScriptModuleAdapter::ui() noexcept {
    std::lock_guard lock(callbacks_); refresh_features();
    if (!enabled() || retired_ || !object_ || plugin_.size<sizeof(MediconPluginApi) || !plugin_.ui) return;
    MediconFrameInfo f{}; f.size=sizeof(f); frame_info(this,&f);
    if (!(f.flags&MEDICON_FRAME_UI_VISIBLE)) return;
    commands_=panels_=0; ui_active_=true;
    const auto start=std::chrono::steady_clock::now(); const auto ok=plugin_.ui(object_);
    try { if(services_.ui_finish) services_.ui_finish(); } catch (...) {}
    ui_active_=false; if (!ok) failed("on_ui"); measured(start);
}
int32_t MEDICON_SCRIPT_CALL ScriptModuleAdapter::frame_info(void* p,MediconFrameInfo* out) noexcept {
    try {
        auto& s=*static_cast<ScriptModuleAdapter*>(p); if(!out || out->size<sizeof(*out) || s.retired_) return 0;
        MediconFrameInfo f{}; f.size=sizeof(f); f.scale=1;
        if(s.services_.frame) f=s.services_.frame();
        else if(s.services_.render_size) { auto size=s.services_.render_size(); f.width=size.x; f.height=size.y; }
        f.size=sizeof(f); f.now_seconds=s.now(); f.delta_seconds=s.delta_; f.generation=s.generation_;
        if(!std::isfinite(f.scale) || f.scale<=0) f.scale=1;
        *out=f; return 1;
    } catch (...) { return 0; }
}
int32_t MEDICON_SCRIPT_CALL ScriptModuleAdapter::draw_shape(void* p,const MediconDrawCommand* d) noexcept {
    try {
        auto& s=*static_cast<ScriptModuleAdapter*>(p);
        if(!s.rendering_ || !d || d->size!=sizeof(*d) || !s.services_.shape || !s.command("draw_shape")) return MEDICON_RESULT_INVALID;
        if(!color(d->color) || !std::isfinite(d->radius) || d->radius<0 || d->radius>32768 || !std::isfinite(d->thickness) || d->thickness<=0 || d->thickness>256) return MEDICON_RESULT_INVALID;
        if(d->kind==3) {
            for(float x : {d->rect.x,d->rect.y,d->end.x,d->end.y}) if(!std::isfinite(x)||std::abs(x)>65536) return MEDICON_RESULT_INVALID;
        } else if((d->kind!=1 && d->kind!=2) || !rectangle(d->rect)) return MEDICON_RESULT_INVALID;
        return s.services_.shape(*d);
    } catch (...) { return MEDICON_RESULT_INVALID; }
}
namespace {
bool style_valid(const MediconGlassStyle& d) {
    if(d.size!=sizeof(d)||d.version!=1||d.preset>3||d.quality>5||d.reserved||d.fields&~1023u) return false;
    for(float x : {d.radius,d.blur,d.refraction,d.saturation,d.highlight,d.border,d.shadow,d.opacity}) if(!std::isfinite(x)||x<0||x>65536) return false;
    return d.refraction<=1 && d.opacity<=1 && color(d.tint);
}
}
int32_t MEDICON_SCRIPT_CALL ScriptModuleAdapter::glass(void* p,const MediconRect* r,const MediconGlassStyle* d,const MediconCallSite* site) noexcept {
    try {
        auto& s=*static_cast<ScriptModuleAdapter*>(p);
        if(!s.rendering_ || !r || !d || !rectangle(*r) || !style_valid(*d) || !s.services_.glass) { s.diagnostic("GLASS_INVALID","draw.glass: invalid values, unavailable bridge, or wrong callback phase",site); return MEDICON_RESULT_INVALID; }
        if(!s.command("glass") || ++s.panels_>20) { s.diagnostic("PANEL_QUOTA","Glass quota is 20 panels per module callback",site); ++s.telemetry_.rejected; return MEDICON_RESULT_QUOTA; }
        const bool supported=s.glass_style(*d,site); ++s.telemetry_.glass_panels;
        const auto result=s.services_.glass(s.id(),s.generation_,*r,*d);
        if(result==MEDICON_RESULT_FALLBACK && supported) s.diagnostic("GLASS_FALLBACK","Native glass is unavailable, disabled by shared accessibility policy, or the frame-wide pane budget is exhausted; using GlassPlate",site);
        return result;
    } catch (...) { return MEDICON_RESULT_INVALID; }
}
uint32_t MEDICON_SCRIPT_CALL ScriptModuleAdapter::ui_begin(void* p,MediconString id,const MediconRect* r,const MediconGlassStyle* d,const MediconCallSite* site) noexcept {
    try {
        auto& s=*static_cast<ScriptModuleAdapter*>(p);
        if(!s.ui_active_ || !r || !d || !rectangle(*r) || !style_valid(*d) || !valid_id(str(id)) || !s.services_.ui_begin || !s.command("glass_panel")) return 0;
        if(++s.panels_>20) { s.diagnostic("UI_PANEL_QUOTA","UI quota is 20 panels per module callback",site); return 0; }
        const bool supported=s.glass_style(*d,site); ++s.telemetry_.glass_panels;
        const auto token=s.services_.ui_begin(s.id(),s.generation_,str(id),*r,*d);
        if(!token) s.diagnostic("UI_PANEL_REJECTED","UI panel rejected: duplicate frame ID or 64 live host panel IDs. Reuse stable panel IDs; slots are released on disable/reload.",site);
        if(token&&supported&&s.services_.ui_status&&s.services_.ui_status()==MEDICON_RESULT_FALLBACK)
            s.diagnostic("UI_GLASS_FALLBACK","Native UI background is unavailable or covered by ClickGUI; using the shared readable GlassPlate. Move the panel away from native columns to expose optics.",site);
        return token;
    } catch (...) { return 0; }
}
void MEDICON_SCRIPT_CALL ScriptModuleAdapter::ui_end(void* p,uint32_t token) noexcept { try { auto& s=*static_cast<ScriptModuleAdapter*>(p); if(s.ui_active_&&s.services_.ui_end) s.services_.ui_end(token); } catch (...) {} }
int32_t MEDICON_SCRIPT_CALL ScriptModuleAdapter::ui_widget(void* p,uint32_t token,const MediconWidget* w) noexcept {
    try { auto& s=*static_cast<ScriptModuleAdapter*>(p);
        if(!s.ui_active_ || !w || w->size!=sizeof(*w) || !s.services_.ui_widget || !s.command("ui_widget") || w->kind<1 || w->kind>3) return 0;
        str(w->text); if(w->kind!=1&&!valid_id(str(w->id))) return 0;
        if(w->kind==3 && (!std::isfinite(w->value)||w->value<0||w->value>1)) return 0;
        return s.services_.ui_widget(token,*w);
    } catch (...) { return 0; }
}
int32_t MEDICON_SCRIPT_CALL ScriptModuleAdapter::theme_color(void* p,uint32_t token,MediconColor* out) noexcept {
    try { auto& s=*static_cast<ScriptModuleAdapter*>(p); if(out&&token<5&&s.services_.theme_color) { *out=s.services_.theme_color(token); return 1; } } catch(...) {} return 0;
}
int32_t MEDICON_SCRIPT_CALL ScriptModuleAdapter::input(void* p,int32_t key,MediconPoint* cursor) noexcept {
    try { auto& s=*static_cast<ScriptModuleAdapter*>(p); if(!s.ui_active_ || !(s.host_.features&MEDICON_FEATURE_INPUT) || (s.services_.input_focused&&!s.services_.input_focused())) return 0;
        // Cursor and held keys are provided by the GUI bridge, never polled from the OS by a script.
        if(cursor && s.services_.cursor) { *cursor=s.services_.cursor(); return cursor->x>=0 && cursor->y>=0; }
        if(key>=0 && key<256) return s.services_.keys[static_cast<size_t>(key)] ? 1 : 0;
    } catch(...) {} return 0;
}
int32_t MEDICON_SCRIPT_CALL ScriptModuleAdapter::notify(void* p,MediconString title,MediconString body,uint32_t level) noexcept {
    try { auto& s=*static_cast<ScriptModuleAdapter*>(p); return s.enabled()&&!s.retired_&&level<4&&s.services_.notify&&s.services_.notify(s.id(),s.generation_,str(title,128),str(body,1024),level); } catch(...) { return 0; }
}
int32_t MEDICON_SCRIPT_CALL ScriptModuleAdapter::play_sound(void* p,uint32_t kind) noexcept {
    try { auto& s=*static_cast<ScriptModuleAdapter*>(p); if(kind!=1) { s.diagnostic("SOUND_UNSUPPORTED","sound::play_ui: only Success maps to the existing notification chime"); return 0; }
        return s.enabled()&&!s.retired_&&s.services_.sound&&s.services_.sound(kind); } catch(...) { return 0; }
}
int32_t MEDICON_SCRIPT_CALL ScriptModuleAdapter::storage_get(void* p,MediconString key,char* out,uint32_t capacity) noexcept {
    try { auto& s=*static_cast<ScriptModuleAdapter*>(p); if(s.retired_||!s.services_.storage) return -1;
        const auto value=s.services_.storage->get(s.id(),str(key,128)); if(!value) return -1;
        if(out) { if(capacity<value->size()) return -2; std::memcpy(out,value->data(),value->size()); } return static_cast<int32_t>(value->size());
    } catch(...) { return -2; }
}
int32_t MEDICON_SCRIPT_CALL ScriptModuleAdapter::storage_set(void* p,MediconString key,MediconString value) noexcept {
    try { auto& s=*static_cast<ScriptModuleAdapter*>(p);
        if(s.constructing_||s.retired_||!s.services_.storage) return 0;
        const bool ok=s.services_.storage->set(s.id(),str(key,128),str(value,ScriptStorage::quota));
        if(!ok) s.diagnostic("STORAGE_QUOTA","storage::set rejected: invalid key/UTF-8 or module exceeds 256 KiB/1024 keys"); return ok;
    } catch(...) { return 0; }
}
uint64_t MEDICON_SCRIPT_CALL ScriptModuleAdapter::timer_add(void* p,double delay,double interval,MediconTimerCallback callback,MediconTimerDestroy destroy,void* data) noexcept {
    try { auto& s=*static_cast<ScriptModuleAdapter*>(p);
        if(!s.enabled()||s.retired_||!callback||!destroy||!std::isfinite(delay)||!std::isfinite(interval)||delay<0||interval<0||delay>604800||interval>604800||(interval>0&&interval<.001)||s.timers_.size()>=64) { s.diagnostic("TIMER_LIMIT","after/every: invalid interval or more than 64 timers"); return 0; }
        auto timer=std::make_shared<Timer>(); timer->due=s.now()+delay; timer->interval=interval; timer->callback=callback; timer->data=data;
        const auto id=s.next_timer_++; s.timers_.emplace(id,timer); timer->destroy=destroy; return id;
    } catch(...) { return 0; }
}
void MEDICON_SCRIPT_CALL ScriptModuleAdapter::timer_cancel(void* p,uint64_t id) noexcept { try { static_cast<ScriptModuleAdapter*>(p)->timers_.erase(id); } catch(...) {} }
}
