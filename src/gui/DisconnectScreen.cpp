#include <medicon/gui/DisconnectScreen.hpp>
#include <medicon/gui/Draw.hpp>
#include <medicon/gui/Glass.hpp>
#include <medicon/gui/Theme.hpp>
#include <algorithm>
#include <cmath>

namespace medicon::gui {
void DisconnectScreen::set_state(bool visible,bool available,std::uint64_t generation,std::string error) {
    if (visible_!=visible || generation_!=generation) {
        input_.reset();action_.reset();reconnect_.reset();bounds_={};buttons_={};motion_={};
    }
    visible_=visible;available_=available;generation_=generation;error_=std::move(error);
}
void DisconnectScreen::render(ImDrawList* list,FontManager& fonts,IShadowRenderer& shadows,const FrameInfo& frame,const PointerInput& pointer,bool alts) {
    if (!visible_) return;
    namespace style=theme::social;
    namespace metrics=theme::social::metrics;
    constexpr std::array labels{"Reconnect","Switch Account","Switch Account and Reconnect"};
    const float s=std::clamp(std::min(frame.client_w/1100.0F,frame.client_h/650.0F),0.65F,1.25F);
    const float pad=16*s,gap=7*s,height=38*s;
    const float width=std::min(frame.client_w-pad*2,std::max(280*s,fonts.measure(FontRole::Module,labels[2])+28*s));
    const float bottom=frame.client_h-pad;
    const float top=std::max(pad,bottom-height*3-gap*2);
    bounds_={{pad,top},{pad+width,bottom}};
    input_.begin(pointer);
    for (unsigned i=0;i<labels.size();++i) {
        const SocialRect rect{{pad,top+i*(height+gap)},{pad+width,top+i*(height+gap)+height}};
        buttons_[i]=rect;
        const bool enabled=i==1?alts:(available_ && (i==0 || alts));
        const bool hover=enabled && input_.hover(rect);
        const bool clicked=input_.click(i+1,rect);
        auto& motion=motion_[i];
        motion.step(hover,enabled && input_.held(i+1),frame.dt);
        const float fade=enabled?1.0F:0.7F;
        const ImVec2 center{(rect.min.x+rect.max.x)*0.5F,(rect.min.y+rect.max.y)*0.5F};
        {
            const ScaleAround press(list,center,1.0F-(1.0F-metrics::button_press_scale)*motion.press);
            GlassPlate plate;
            plate.fill=style::plate;
            plate.hover=motion.hover;
            plate.press=motion.press;
            plate.alpha=fade;
            plate.accent_inset=height*metrics::accent_inset;
            plate.glow=enabled?&shadows:nullptr;
            plate.glow_sigma=social_button_glow_sigma(s);
            // The shared HUD milk keeps the same white glass readable over
            // vanilla's dark disconnect backdrop, without a dark custom fill.
            plate.fill_opacity=theme::watermark::fill_opacity+
                (theme::glass::fill_pressed-theme::watermark::fill_opacity)*std::max(motion.hover,motion.press);
            draw_glass_plate(list,rect.min,rect.max,metrics::button_radius*s,plate);
            const float accent=metrics::button_radius*s*0.6F;
            const float text_width=fonts.measure(FontRole::Module,labels[i]);
            fonts.draw(list,FontRole::Module,rect.min.x+accent+(rect.width()-accent-text_width)*0.5F,
                fonts.centered_baseline(FontRole::Module,rect.min.y,height),to_u32(style::heading,fade),labels[i]);
        }
        if (clicked && enabled && !action_) {
            const auto action=static_cast<DisconnectAction>(i);
            if (action==DisconnectAction::Reconnect) reconnect_=generation_;
            else action_=action;
        }
    }
    input_.end();
    if (!error_.empty() || !available_) {
        const auto text=error_.empty()?"No saved server. Join from Play.":error_;
        std::string scratch;
        fonts.draw(list,FontRole::KeyHint,pad,top-10*s,to_u32(style::error),
            fonts.ellipsize(FontRole::KeyHint,text,width,scratch));
    }
}
}
