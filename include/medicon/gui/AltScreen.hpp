#pragma once

#include "medicon/gui/GuiContext.hpp"
#include "medicon/gui/GuiMath.hpp"
#include "medicon/gui/SocialFonts.hpp"
#include "medicon/gui/SocialWidgets.hpp"

#include <medicon/alt/Alt.hpp>

#include <cstdint>
#include <optional>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

namespace medicon::gui {

class IShadowRenderer;
class SocialBackground;

// The AltManager, opened from the Main Menu: the Social panel with its sidebar on
// the right, the saved accounts in the middle and the Microsoft button beside
// them. Render thread only. Holds no tokens: the service keeps those.
class AltScreen {
public:
    struct Context {
        ImDrawList* list = nullptr;
        SocialFonts& fonts;
        SocialBackground& background;
        IShadowRenderer& shadows;
        alt::Channel* channel = nullptr;
        FrameInfo frame{};
        PointerInput pointer{};
        // As SocialScreen::Context::backdrop_time.
        double backdrop_time = 0.0;
    };

    void open();
    void close() noexcept;
    bool opened() const noexcept { return open_; }
    // Open, or still fading out.
    bool visible() const noexcept { return open_ || animation_.p > 0.0F; }
    // Fully open: nothing under the backdrop needs drawing.
    bool opaque() const noexcept { return open_ && animation_.p >= 1.0F; }
    void key_event(int virtual_key, bool down);
    void on_event(const alt::Event& event, SocialFonts& fonts);
    void render(Context& context);

    // Fixed strings at the sizes they are drawn in.
    static void request_glyphs(SocialFonts& fonts);

    // As last drawn, for Preview and tests.
    const std::vector<alt::Account>& accounts() const noexcept { return accounts_; }
    bool signing_in() const noexcept { return stage_.has_value(); }
    const std::string& error() const noexcept { return error_; }
    const std::string& diagnostic() const noexcept { return diagnostic_; }
    const std::string& user_code() const noexcept { return user_code_; }
    bool code_copied() const noexcept { return code_copied_; }
    bool window_opened() const noexcept { return window_opened_; }
    SocialRect code_rect() const noexcept { return code_rect_; }
    SocialRect copy_code_rect() const noexcept { return copy_code_rect_; }
    SocialRect open_sign_in_rect() const noexcept { return open_sign_in_rect_; }
    SocialRect microsoft_rect() const noexcept { return microsoft_rect_; }
    SocialRect cancel_rect() const noexcept { return cancel_rect_; }
    SocialRect remove_rect(std::string_view xuid) const noexcept;
    SocialRect use_rect(std::string_view xuid) const noexcept;
    const std::string& active_xuid() const noexcept { return session_.active_xuid; }
    std::optional<alt::UseFinished> consume_use_finished() noexcept { return std::exchange(use_finished_, {}); }
    SocialRect confirm_ok_rect() const noexcept { return confirm_ok_rect_; }
    bool confirming() const noexcept { return !confirm_remove_.empty(); }

private:
    struct RowButton {
        std::string xuid;
        SocialRect rect;
    };

    void draw_list(Context& context, const SocialRect& area, float scale);
    void draw_sign_in(Context& context, const SocialRect& area, float scale);
    void draw_sidebar(Context& context, const SocialRect& area, float scale);
    void draw_confirm(Context& context, const SocialRect& panel, float scale);
    const SocialMotion& motion(std::uint32_t id, const SocialRect& rect, float dt);

    bool open_ = false;
    Reversible animation_;
    SocialInput input_;
    SocialScroll scroll_;
    std::unordered_map<std::uint32_t, SocialMotion> motion_;
    std::vector<alt::Account> accounts_;
    alt::SessionChanged session_;
    std::uint64_t next_use_id_ = 0, pending_use_id_ = 0;
    std::optional<alt::UseFinished> use_finished_;
    bool load_failed_ = false;
    std::optional<alt::SignInStage> stage_;
    std::string user_code_;
    bool code_copied_ = false;
    bool copy_pending_ = false;
    bool window_opened_ = false;
    std::string error_;  // a code from sign_in()
    std::string diagnostic_; // safe stage and HTTP/XErr details
    std::string added_;  // the gamertag just added
    std::string confirm_remove_; // XUID waiting for "Remove"
    SocialRect microsoft_rect_{};
    SocialRect code_rect_{}, copy_code_rect_{}, open_sign_in_rect_{};
    SocialRect cancel_rect_{};
    SocialRect confirm_ok_rect_{};
    std::vector<RowButton> remove_rects_;
    std::vector<RowButton> use_rects_;
    std::vector<SocialFonts::Line> wrapped_;
};

} // namespace medicon::gui
