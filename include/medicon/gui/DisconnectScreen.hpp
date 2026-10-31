#pragma once
#include <medicon/gui/GuiContext.hpp>
#include <medicon/gui/SocialWidgets.hpp>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace medicon::gui {
enum class DisconnectAction { Reconnect, SwitchAccount, SwitchAccountAndReconnect };
class DisconnectScreen {
public:
    void set_state(bool visible,bool available,std::uint64_t generation,std::string error = {});
    bool visible() const noexcept {return visible_;}
    std::uint64_t generation() const noexcept {return generation_;}
    SocialRect bounds() const noexcept {return bounds_;}
    SocialRect button(DisconnectAction action) const noexcept {return buttons_[static_cast<unsigned>(action)];}
    std::optional<std::uint64_t> consume_reconnect() noexcept {return std::exchange(reconnect_,{});}
    void render(ImDrawList*,FontManager&,IShadowRenderer&,const FrameInfo&,const PointerInput&,bool alts_available);
    std::optional<DisconnectAction> consume_action() noexcept {return std::exchange(action_,{});}
    void reconnect_after_choice() noexcept {if(visible_ && available_) reconnect_=generation_;}
private:
    SocialInput input_;
    bool visible_=false,available_=false;
    std::uint64_t generation_=0;
    SocialRect bounds_{};
    std::array<SocialRect,3> buttons_{};
    std::array<SocialMotion,3> motion_{};
    std::optional<DisconnectAction> action_;
    std::optional<std::uint64_t> reconnect_;
    std::string error_;
};
}
