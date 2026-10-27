#pragma once

#include "medicon/gui/SocialFonts.hpp"
#include "medicon/gui/SocialWidgets.hpp"

#include <medicon/social/Store.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace medicon::gui {

class IShadowRenderer;

// What a chat view draws with, from SocialScreen::render.
struct ChatContext {
    ImDrawList* list = nullptr;
    SocialFonts& fonts;
    IShadowRenderer& shadows;
    social::SocialStore& store;
    social::Channel* channel = nullptr;
    SocialInput& input;
    float dt = 0.0F;
    float scale = 1.0F;
    double now = 0.0; // ImGui time: caret blink, banners
};

// Global (spec 4.6) or one DM: the message list, its input and the send button.
// ChatList is shared: Global shows a name and time per group, a DM shows
// bubbles (mine right, blue) and "Read" under my last message they read.
// Render thread only.
class SocialChatView {
public:
    explicit SocialChatView(social::ConversationId conversation) : conversation_(conversation) {}

    void render(ChatContext& context, const SocialRect& area);
    SocialTextInput& field() noexcept { return input_; }
    // A short error above the input for 2 s (spec 4.6), e.g. rate_limited.
    void show_error(std::string text, double now);
    // Forget what was drawn (the account changed).
    void reset();

    static void request_glyphs(SocialFonts& fonts);

private:
    // Wrapped body lines of one message (cached; grouping is decided per frame).
    struct Layout {
        std::vector<SocialFonts::Line> lines;
        float widest = 0.0F; // the longest line, for a DM bubble
        std::string time;    // HH:MM, formatted once
    };

    void draw_header(ChatContext& context, const SocialRect& area);
    void draw_input(ChatContext& context, const SocialRect& area);
    void send(ChatContext& context);
    void retry(ChatContext& context, const social::ChatEntry& entry);
    const Layout& layout(ChatContext& context, const social::ChatEntry& entry, float width);
    // The message id, or for my unanswered ones a negative hash of the nonce:
    // no allocation per entry and frame.
    static std::int64_t key(const social::ChatEntry& entry) noexcept;
    // Starts a new group: another author, or more than 5 minutes later (spec 4.6).
    static bool starts_group(const std::vector<social::ChatEntry>& entries, std::size_t index);
    float row_height(ChatContext& context, const std::vector<social::ChatEntry>& entries, std::size_t index, float width,
                     bool read_mark);
    std::string next_nonce();

    social::ConversationId conversation_;
    bool dm() const noexcept { return conversation_ != social::global_conversation; }

    SocialTextInput input_{300};
    // Scroll: distance from the top of the content; the view sticks to the bottom
    // until the user scrolls up.
    float offset_ = 0.0F;
    float target_ = 0.0F;
    float content_ = 0.0F;
    bool stick_ = true;
    int unseen_ = 0;
    std::uint32_t arrivals_ = 0;
    std::int64_t first_key_ = 0; // the oldest entry drawn last frame, to keep the view still when history is prepended
    std::int64_t marked_read_ = 0;
    std::string error_;
    double error_until_ = 0.0;
    // Wrapped lines per message, until the width or the atlas changes.
    float layout_width_ = -1.0F;
    std::uint32_t layout_revision_ = 0;
    std::unordered_map<std::int64_t, Layout> layouts_;
    std::uint64_t next_nonce_ = 1;
    std::vector<float> heights_; // per entry, this frame
    SocialMotion send_motion_, jump_motion_;
};

} // namespace medicon::gui
