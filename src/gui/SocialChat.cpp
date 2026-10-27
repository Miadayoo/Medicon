#include <medicon/gui/SocialChat.hpp>

#include <medicon/gui/Draw.hpp>
#include <medicon/gui/Glass.hpp>
#include <medicon/gui/GuiMath.hpp>
#include <medicon/gui/Theme.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace medicon::gui {
namespace {

namespace style = theme::social;
namespace metrics = theme::social::metrics;
using social::ChatEntry;

enum : std::uint32_t {
    id_field = 900,
    id_send,
    id_jump,
    id_scroll,
    id_retry_first = 10000,
};

constexpr std::int64_t group_window_ms = 5 * 60 * 1000;

std::string clock_text(std::int64_t milliseconds) {
    const std::time_t seconds = static_cast<std::time_t>(milliseconds / 1000);
    std::tm local{};
    if (milliseconds <= 0 || localtime_s(&local, &seconds) != 0) return {};
    char text[8];
    std::snprintf(text, sizeof text, "%02d:%02d", local.tm_hour, local.tm_min);
    return text;
}

// Spec 2.1: the length is counted without surrounding whitespace (ASCII and the ideographic space).
std::string trimmed(std::string text) {
    const auto blank = [](std::string_view rest) {
        if (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t')) return std::size_t{1};
        if (rest.substr(0, 3) == "\xE3\x80\x80") return std::size_t{3};
        return std::size_t{0};
    };
    while (const auto n = blank(text)) text.erase(0, n);
    while (!text.empty()) {
        if (text.back() == ' ' || text.back() == '\t') text.pop_back();
        else if (text.size() >= 3 && text.compare(text.size() - 3, 3, "\xE3\x80\x80") == 0) text.resize(text.size() - 3);
        else break;
    }
    return text;
}

const char* failure_text(const std::string& error) {
    if (error == "offline") return "Not connected";
    if (error == "rate_limited") return "Sending too fast";
    if (error == "too_long") return "Too long";
    if (error == "not_friends") return "Not friends";
    return "Failed to send";
}

} // namespace

void SocialChatView::request_glyphs(SocialFonts& fonts) {
    // Kana and Japanese punctuation up front: typing them never waits for a new
    // atlas. Kanji are added as they appear.
    std::string kana;
    const auto append = [&](char32_t c) {
        char bytes[4];
        int n = 0;
        if (c < 0x800) {
            bytes[n++] = static_cast<char>(0xC0 | (c >> 6));
        } else {
            bytes[n++] = static_cast<char>(0xE0 | (c >> 12));
            bytes[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        }
        bytes[n++] = static_cast<char>(0x80 | (c & 0x3F));
        kana.append(bytes, static_cast<std::size_t>(n));
    };
    for (char32_t c = 0x3001; c <= 0x301F; ++c) append(c); // 、。「」 and friends
    for (char32_t c = 0x3041; c <= 0x3096; ++c) append(c); // hiragana
    for (char32_t c = 0x30A1; c <= 0x30FC; ++c) append(c); // katakana, ー
    for (char32_t c = 0xFF01; c <= 0xFF5E; ++c) append(c); // full-width ASCII
    fonts.request(SocialText::Body, kana);
    for (const char* text : {"Global Chat", "Online", "Offline"}) fonts.request(SocialText::Heading, text);
    for (const char* text : {"Send", "New messages \xE2\x86\x93"}) fonts.request(SocialText::Title, text);
    for (const char* text : {"Message", "No messages yet", "Loading\xE2\x80\xA6", "Say hi!", "Could not load"})
        fonts.request(SocialText::Body, text);
    for (const char* text : {"Read", "0123456789: left", "Failed to send \xC2\xB7 Click to retry", "Not connected",
                             "Sending too fast", "Too long", "Not friends", "Online", "Offline",
                             "One message every 0.3 seconds", "Messages are up to 300 characters",
                             "You can only message friends", "Could not send"})
        fonts.request(SocialText::Small, text);
}

void SocialChatView::reset() {
    input_.clear();
    offset_ = target_ = content_ = 0.0F;
    stick_ = true;
    unseen_ = 0;
    arrivals_ = 0;
    first_key_ = 0;
    marked_read_ = 0;
    error_.clear();
    layouts_.clear();
}

void SocialChatView::show_error(std::string text, double now) {
    error_ = std::move(text);
    error_until_ = now + 2.0;
}

std::int64_t SocialChatView::key(const ChatEntry& entry) noexcept {
    if (entry.message.id != 0) return entry.message.id;
    return -static_cast<std::int64_t>(std::hash<std::string>{}(entry.nonce) >> 2) - 1;
}

bool SocialChatView::starts_group(const std::vector<ChatEntry>& entries, std::size_t index) {
    if (index == 0) return true;
    const auto& previous = entries[index - 1].message;
    const auto& current = entries[index].message;
    if (previous.author.id != current.author.id) return true;
    // Pending messages have no server time yet: they join the group before them.
    if (current.created_at == 0 || previous.created_at == 0) return false;
    return current.created_at - previous.created_at > group_window_ms;
}

std::string SocialChatView::next_nonce() {
    return (dm() ? "d" : "g") + std::to_string(conversation_) + "-" + std::to_string(next_nonce_++);
}

const SocialChatView::Layout& SocialChatView::layout(ChatContext& context, const ChatEntry& entry, float width) {
    auto [found, inserted] = layouts_.try_emplace(key(entry));
    if (inserted) {
        auto& result = found->second;
        context.fonts.wrap(SocialText::Body, entry.message.body, width, result.lines);
        for (const auto& line : result.lines) {
            const auto text = std::string_view(entry.message.body).substr(line.begin, line.end - line.begin);
            result.widest = std::max(result.widest, context.fonts.measure(SocialText::Body, text));
        }
        result.time = clock_text(entry.message.created_at);
    }
    return found->second;
}

float SocialChatView::row_height(ChatContext& context, const std::vector<ChatEntry>& entries, std::size_t index,
                                 float width, bool read_mark) {
    const float s = context.scale;
    const auto& fonts = context.fonts;
    const auto& entry = entries[index];
    const bool group = starts_group(entries, index);
    const auto& body = layout(context, entry, width);
    const float lines = static_cast<float>(std::max<std::size_t>(1, body.lines.size())) * fonts.line_height(SocialText::Body);
    float height = (group ? metrics::chat_group_gap : metrics::chat_line_gap) * s;
    if (dm()) {
        height += lines + metrics::bubble_padding_y * s * 2.0F;
        if (read_mark) height += fonts.line_height(SocialText::Small);
    } else {
        if (group) height += fonts.line_height(SocialText::Title) + 1.0F * s;
        height += lines;
    }
    if (entry.status == ChatEntry::Status::Failed) height += fonts.line_height(SocialText::Small) + 2.0F * s;
    return height;
}

void SocialChatView::draw_header(ChatContext& context, const SocialRect& area) {
    auto* list = context.list;
    const auto& fonts = context.fonts;
    const float s = context.scale;
    const float y = area.min.y + (area.height() - fonts.line_height(SocialText::Heading)) * 0.5F;
    if (!dm()) {
        fonts.draw(list, SocialText::Heading, area.min.x, y, to_u32(style::heading), "Global Chat");
        return;
    }
    // Spec 4.6: the friend's name and whether they are online.
    const auto* other = context.store.find_friend(conversation_);
    std::string scratch;
    const auto name = fonts.ellipsize(SocialText::Heading, other ? std::string_view(other->user.name) : std::string_view("-"),
                                      area.width() * 0.6F, scratch);
    fonts.draw(list, SocialText::Heading, area.min.x, y, to_u32(style::heading), name);
    const bool online = other && other->online;
    const float x = area.min.x + fonts.measure(SocialText::Heading, name) + 14.0F * s;
    const float small = fonts.line_height(SocialText::Small);
    const float cy = y + fonts.line_height(SocialText::Heading) * 0.55F;
    const float dot = metrics::status_dot * s * 0.5F;
    list->AddCircleFilled({x + dot, cy}, dot, to_u32(online ? style::online : style::offline), 16);
    fonts.draw(list, SocialText::Small, x + dot * 2.0F + 6.0F * s, cy - small * 0.5F, to_u32(style::text_secondary),
               online ? "Online" : "Offline");
}

void SocialChatView::send(ChatContext& context) {
    const std::string body = trimmed(input_.utf8());
    if (body.empty() || !context.channel) return; // spec 4.6: an empty message does nothing
    const std::string nonce = next_nonce();
    context.store.add_pending(conversation_, nonce, body);
    context.channel->commands.push(social::SendChat{nonce, conversation_, body});
    input_.clear();
    stick_ = true;
}

void SocialChatView::retry(ChatContext& context, const ChatEntry& entry) {
    if (!context.channel) return;
    const std::string nonce = next_nonce();
    const std::string body = entry.message.body;
    if (context.store.retry(conversation_, entry.nonce, nonce)) {
        context.channel->commands.push(social::SendChat{nonce, conversation_, body});
        stick_ = true;
    }
}

void SocialChatView::draw_input(ChatContext& context, const SocialRect& area) {
    auto* list = context.list;
    const auto& fonts = context.fonts;
    const float s = context.scale;
    const float height = metrics::input_height * s;
    const float send_width = metrics::send_width * s;
    const float y = area.max.y - height - 6.0F * s;
    const SocialRect field{{area.min.x, y}, {area.max.x - send_width - 10.0F * s, y + height}};
    const SocialRect button{{field.max.x + 10.0F * s, y}, {area.max.x, y + height}};
    auto& input = context.input;
    if (input.click(id_field, field)) input_.set_focused(true);
    input_.draw(list, fonts, field, s, 1.0F, "Message", context.channel != nullptr, context.now, &context.shadows);
    const bool enabled = input_.length() > 0;
    send_motion_.step(input.hover(button), input.held(id_send), context.dt);
    draw_social_button(list, fonts, &context.shadows, button, "Send", SocialButton::Primary, send_motion_, enabled, s, 1.0F);
    if ((input.click(id_send, button) && enabled) || input_.consume_submit()) send(context);
    // Above the field: a short error (2 s), and the characters left past 250 (spec 4.8).
    const float small = fonts.line_height(SocialText::Small);
    const float above = y - small - 4.0F * s;
    if (!error_.empty() && context.now < error_until_)
        fonts.draw(list, SocialText::Small, field.min.x + 4.0F * s, above, to_u32(style::error), error_);
    if (input_.length() > 250) {
        char left[24];
        std::snprintf(left, sizeof left, "%d left", static_cast<int>(300 - input_.length()));
        fonts.draw(list, SocialText::Small, field.max.x - fonts.measure(SocialText::Small, left) - 4.0F * s, above,
                   to_u32(input_.length() >= 300 ? style::error : style::text_secondary), left);
    }
}

void SocialChatView::render(ChatContext& context, const SocialRect& area) {
    auto* list = context.list;
    auto& fonts = context.fonts;
    auto& store = context.store;
    auto& input = context.input;
    const float s = context.scale;
    auto& conversation = store.conversation(conversation_);
    const bool online = store.state() == social::ConnectionState::Online;
    if (online && context.channel && !conversation.loaded && !conversation.loading && conversation.error.empty()) {
        context.channel->api.push(social::FetchMessages{conversation_, std::nullopt, 50});
        conversation.loading = true;
    }

    const SocialRect header{area.min, {area.max.x, area.min.y + metrics::chat_header_height * s}};
    draw_header(context, header);
    const float input_row = metrics::chat_input_row * s;
    const SocialRect view{{area.min.x, header.max.y}, {area.max.x, area.max.y - input_row - fonts.line_height(SocialText::Small)}};
    const float scrollbar = metrics::scrollbar_width * s + 6.0F * s;
    const float width = view.width() - scrollbar;
    const float body_width = dm() ? width * metrics::bubble_max_ratio - metrics::bubble_padding_x * s * 2.0F : width;
    if (layout_width_ != body_width || layout_revision_ != fonts.revision()) {
        layouts_.clear();
        layout_width_ = body_width;
        layout_revision_ = fonts.revision();
    }

    const auto& entries = conversation.entries;
    // Spec 4.6: "Read" under the last of my messages that they have read.
    std::size_t read_index = entries.size();
    if (dm()) {
        const auto* other = store.find_friend(conversation_);
        const std::int64_t read_up_to = other ? other->their_read_up_to : 0;
        const auto me = store.me() ? store.me()->id : 0;
        for (std::size_t i = entries.size(); i-- > 0;) {
            const auto& message = entries[i].message;
            if (message.author.id == me && message.id != 0 && message.id <= read_up_to) {
                read_index = i;
                break;
            }
        }
    }
    heights_.resize(entries.size());
    content_ = 0.0F;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        heights_[i] = row_height(context, entries, i, body_width, i == read_index);
        content_ += heights_[i];
    }
    content_ += 8.0F * s;

    // History prepended above: keep what is on screen where it was (spec 4.6).
    if (first_key_ != 0 && !entries.empty() && key(entries.front()) != first_key_) {
        float added = 0.0F;
        for (std::size_t i = 0; i < entries.size() && key(entries[i]) != first_key_; ++i) added += heights_[i];
        if (added < content_) {
            offset_ += added;
            target_ += added;
        }
    }
    first_key_ = entries.empty() ? 0 : key(entries.front());
    // New messages follow only while the bottom is in view; otherwise they are counted.
    if (conversation.arrivals != arrivals_) {
        if (!stick_) unseen_ += static_cast<int>(conversation.arrivals - arrivals_);
        arrivals_ = conversation.arrivals;
    }
    const float limit = std::max(0.0F, content_ - view.height());
    const auto& pointer = input.pointer();
    if (pointer.valid && pointer.wheel != 0.0F && view.contains(pointer.position)) {
        target_ -= pointer.wheel * metrics::wheel_step * s;
        stick_ = false;
    }
    if (stick_) target_ = limit;
    target_ = std::clamp(target_, 0.0F, limit);
    offset_ = std::clamp(follow(offset_, target_, context.dt, style::timing::scroll_tau), 0.0F, limit);
    if (target_ >= limit - 2.0F * s) stick_ = true;
    if (stick_) unseen_ = 0;

    // The top was reached: load older messages (before = the oldest id).
    if (online && context.channel && conversation.loaded && conversation.has_more && !conversation.loading &&
        offset_ < metrics::load_more_distance * s) {
        const auto oldest = std::find_if(entries.begin(), entries.end(), [](const ChatEntry& e) { return e.message.id != 0; });
        if (oldest != entries.end()) {
            context.channel->api.push(social::FetchMessages{conversation_, oldest->message.id, 50});
            conversation.loading = true;
        }
    }

    list->PushClipRect(view.min, view.max, true);
    // A short conversation sits at the bottom, as chats do.
    float top = view.min.y - offset_ + std::max(0.0F, view.height() - content_);
    const auto me = store.me() ? store.me()->id : 0;
    const float body_line = fonts.line_height(SocialText::Body);
    const float small = fonts.line_height(SocialText::Small);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const float bottom = top + heights_[i];
        if (bottom < view.min.y || top > view.max.y) {
            top = bottom;
            continue;
        }
        const auto& entry = entries[i];
        const auto& message = entry.message;
        const bool group = starts_group(entries, i);
        const bool mine = message.author.id == me;
        const float alpha = entry.status == ChatEntry::Status::Pending ? 0.5F : 1.0F; // spec 4.6
        const auto& body = layout(context, entry, body_width);
        float y = top + (group ? metrics::chat_group_gap : metrics::chat_line_gap) * s;
        if (dm()) {
            const float pad_x = metrics::bubble_padding_x * s, pad_y = metrics::bubble_padding_y * s;
            const float bubble_w = body.widest + pad_x * 2.0F;
            const float bubble_h = static_cast<float>(std::max<std::size_t>(1, body.lines.size())) * body_line + pad_y * 2.0F;
            const float x = mine ? view.min.x + width - bubble_w : view.min.x;
            const ImVec2 min{x, y}, max{x + bubble_w, y + bubble_h};
            GlassPlate plate;
            plate.accent = false;
            plate.drop = true;
            plate.fill = mine ? style::bubble_mine : style::bubble_theirs;
            plate.hover = mine ? 0.7F : 0.0F; // mine reads as the more solid, blue one
            plate.alpha = alpha;
            draw_glass_plate(list, min, max, metrics::bubble_radius * s, plate);
            float line_top = y + pad_y;
            for (const auto& line : body.lines) {
                fonts.draw(list, SocialText::Body, x + pad_x, line_top, to_u32(style::text, alpha),
                           std::string_view(message.body).substr(line.begin, line.end - line.begin));
                line_top += body_line;
            }
            const std::string& time = body.time;
            const float time_w = fonts.measure(SocialText::Small, time);
            const float time_x = mine ? min.x - time_w - 6.0F * s : max.x + 6.0F * s;
            fonts.draw(list, SocialText::Small, time_x, max.y - small, to_u32(style::text_secondary, alpha), time);
            y = max.y;
            if (i == read_index) {
                fonts.draw(list, SocialText::Small, max.x - fonts.measure(SocialText::Small, "Read"), y,
                           to_u32(style::text_secondary), "Read");
                y += small;
            }
        } else {
            if (group) {
                std::string scratch;
                const auto name = fonts.ellipsize(SocialText::Title, message.author.name, width * 0.6F, scratch);
                fonts.draw(list, SocialText::Title, view.min.x, y, to_u32(mine ? style::my_name : style::heading, alpha), name);
                float after = view.min.x + fonts.measure(SocialText::Title, name) + 8.0F * s;
                if (message.author.rank == social::Rank::Owner) {
                    // Owners carry a small pink tag after their name.
                    const float tag_w = fonts.measure(SocialText::Small, "OWNER") + 10.0F * s;
                    const float tag_y = y + (fonts.line_height(SocialText::Title) - small) * 0.5F;
                    list->AddRectFilled({after - 2.0F * s, tag_y - 1.0F * s}, {after - 2.0F * s + tag_w, tag_y + small + 1.0F * s},
                                        to_u32(style::owner_tag, alpha), (small + 2.0F * s) * 0.5F);
                    fonts.draw(list, SocialText::Small, after + 3.0F * s, tag_y, to_u32(style::owner_text, alpha), "OWNER");
                    after += tag_w + 6.0F * s;
                }
                fonts.draw(list, SocialText::Small, after, y + fonts.line_height(SocialText::Title) - small - 1.0F * s,
                           to_u32(style::text_secondary, alpha), body.time);
                y += fonts.line_height(SocialText::Title) + 1.0F * s;
            }
            for (const auto& line : body.lines) {
                if (y + body_line >= view.min.y && y <= view.max.y)
                    fonts.draw(list, SocialText::Body, view.min.x, y, to_u32(style::text, alpha),
                               std::string_view(message.body).substr(line.begin, line.end - line.begin));
                y += body_line;
            }
            if (body.lines.empty()) y += body_line;
        }
        if (entry.status == ChatEntry::Status::Failed) {
            // Spec 4.6: failed sends say so and are sent again on a click.
            std::string label = std::string(failure_text(entry.error)) + " \xC2\xB7 Click to retry";
            const float w = fonts.measure(SocialText::Small, label);
            const float x = dm() && mine ? view.min.x + width - w : view.min.x;
            const SocialRect link{{x, y + 2.0F * s}, {x + w, y + 2.0F * s + small}};
            fonts.draw(list, SocialText::Small, link.min.x, link.min.y, to_u32(style::error), label);
            if (input.hover(link)) list->AddLine({link.min.x, link.max.y}, {link.max.x, link.max.y}, to_u32(style::error));
            if (input.click(id_retry_first + static_cast<std::uint32_t>(i), link)) retry(context, entry);
        }
        top = bottom;
    }
    list->PopClipRect();

    if (entries.empty()) {
        const char* text = conversation.loading ? "Loading\xE2\x80\xA6"
                         : !conversation.error.empty() || (!conversation.loaded && online) ? "Could not load"
                         : dm() ? "Say hi!" : "No messages yet";
        const float w = fonts.measure(SocialText::Body, text);
        fonts.draw(list, SocialText::Body, view.min.x + (width - w) * 0.5F, view.min.y + (view.height() - body_line) * 0.5F,
                   to_u32(style::text_secondary), text);
    }
    // The scroll thumb.
    if (limit > 0.5F) {
        const float thumb_h = std::max(24.0F * s, view.height() * view.height() / content_);
        const float thumb_y = view.min.y + offset_ / limit * (view.height() - thumb_h);
        const float bar = metrics::scrollbar_width * s;
        list->AddRectFilled({view.max.x - bar - 2.0F * s, thumb_y}, {view.max.x - 2.0F * s, thumb_y + thumb_h},
                            to_u32(style::scrollbar), bar * 0.5F);
    }
    // Spec 4.6: while reading older messages, new ones bring up a button instead of scrolling.
    if (unseen_ > 0) {
        const char* label = "New messages \xE2\x86\x93";
        const float w = fonts.measure(SocialText::Title, label) + 36.0F * s;
        const float h = 30.0F * s;
        const SocialRect pill{{view.min.x + (width - w) * 0.5F, view.max.y - h - 8.0F * s},
                              {view.min.x + (width + w) * 0.5F, view.max.y - 8.0F * s}};
        jump_motion_.step(input.hover(pill), input.held(id_jump), context.dt);
        GlassPlate plate;
        plate.hover = jump_motion_.hover;
        plate.press = jump_motion_.press;
        plate.accent = false;
        plate.fill = style::plate;
        plate.glow = &context.shadows;
        plate.glow_sigma = social_button_glow_sigma(s);
        draw_glass_plate(list, pill.min, pill.max, h * 0.5F, plate);
        fonts.draw(list, SocialText::Title, pill.min.x + 18.0F * s, pill.min.y + (h - fonts.line_height(SocialText::Title)) * 0.5F,
                   to_u32(style::heading), label);
        if (input.click(id_jump, pill)) {
            stick_ = true;
            unseen_ = 0;
        }
    }

    // A DM on screen at its bottom is read (spec 4.6): dm.read up to their newest message.
    if (dm() && context.channel && stick_) {
        std::int64_t newest = 0;
        for (const auto& entry : entries)
            if (entry.message.author.id == conversation_ && entry.message.id > newest) newest = entry.message.id;
        if (newest > marked_read_) {
            marked_read_ = newest;
            context.channel->commands.push(social::MarkRead{conversation_, newest});
            store.clear_unread(conversation_);
        }
    }

    draw_input(context, {{area.min.x, area.max.y - input_row}, area.max});
}

} // namespace medicon::gui
