#include <medicon/gui/SocialWidgets.hpp>

#include <medicon/gui/Draw.hpp>
#include <medicon/gui/Glass.hpp>
#include <medicon/gui/GuiMath.hpp>
#include <medicon/gui/Shadow.hpp>
#include <medicon/gui/Theme.hpp>

#include <Windows.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>

namespace medicon::gui {
namespace {

namespace style = theme::social;
namespace metrics = theme::social::metrics;

bool high_surrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool low_surrogate(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

std::string narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(size, 0)), '\0');
    if (size > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

// Another program can hold the clipboard open for a moment; retry briefly.
bool open_clipboard() {
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (OpenClipboard(nullptr)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// Clipboard calls can block on another program, so they run off the render thread.
std::wstring read_clipboard() {
    std::wstring text;
    if (!open_clipboard()) return text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* chars = static_cast<const wchar_t*>(GlobalLock(data))) {
            text = chars;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return text;
}

void write_clipboard(std::wstring text) {
    if (!open_clipboard()) return;
    EmptyClipboard();
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* target = GlobalLock(memory)) {
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

} // namespace

// --- SocialInput ----------------------------------------------------------------

void SocialInput::reset() noexcept {
    armed_ = false;
    claimed_ = false;
    active_ = 0;
}

void SocialInput::begin(const PointerInput& pointer) noexcept {
    pointer_ = pointer;
    claimed_ = false;
    const auto& left = pointer.buttons[0];
    if (!pointer.valid) {
        armed_ = false;
        active_ = 0;
    } else if (!armed_ && !left.down && !left.pressed && !left.released) {
        armed_ = true;
    }
}

void SocialInput::end() noexcept {
    if (!pointer_.buttons[0].down) active_ = 0;
}

bool SocialInput::click(std::uint32_t id, const SocialRect& rect) noexcept {
    if (!armed_ || blocked_) return false;
    const auto& left = pointer_.buttons[0];
    const bool inside = rect.contains(pointer_.position);
    if (left.pressed && inside) {
        active_ = id;
        claimed_ = true;
    }
    if (left.released && active_ == id) {
        active_ = 0;
        return inside;
    }
    return false;
}

bool SocialInput::drag(std::uint32_t id, const SocialRect& rect) noexcept {
    if (!armed_ || blocked_) return false;
    const auto& left = pointer_.buttons[0];
    if (left.pressed && rect.contains(pointer_.position)) {
        active_ = id;
        claimed_ = true;
    }
    return active_ == id && (left.down || left.pressed);
}

// --- SocialScroll -----------------------------------------------------------------

float SocialScroll::begin(ImDrawList* list, const SocialRect& view, float content_height, const PointerInput& pointer,
                          float dt, float scale) {
    view_ = view;
    content_ = content_height;
    const float limit = max_offset();
    if (pointer.valid && pointer.wheel != 0.0F && view.contains(pointer.position))
        target_ -= pointer.wheel * metrics::wheel_step * scale;
    target_ = std::clamp(target_, 0.0F, limit);
    offset_ = std::clamp(follow(offset_, target_, dt, style::timing::scroll_tau), 0.0F, limit);
    list->PushClipRect(view.min, view.max, true);
    return view.min.y - offset_;
}

void SocialScroll::end(ImDrawList* list, SocialInput& input, std::uint32_t id, float scale, float alpha) {
    list->PopClipRect();
    const float height = view_.height();
    const float limit = max_offset();
    if (limit <= 0.5F) return;
    const float thumb_height = std::max(24.0F * scale, height * height / content_);
    const float range = height - thumb_height;
    const float thumb_y = view_.min.y + offset_ / limit * range;
    const float width = metrics::scrollbar_width * scale;
    const SocialRect thumb{{view_.max.x - width - 2.0F * scale, thumb_y}, {view_.max.x - 2.0F * scale, thumb_y + thumb_height}};
    // A wider grab area than the thin bar that is drawn.
    const SocialRect grab{{thumb.min.x - 8.0F * scale, thumb.min.y}, {view_.max.x, thumb.max.y}};
    const auto& pointer = input.pointer();
    if (pointer.buttons[0].pressed && grab.contains(pointer.position)) grab_ = pointer.position.y - thumb_y;
    if (input.drag(id, grab) && range > 0.0F) {
        const float t = std::clamp((pointer.position.y - grab_ - view_.min.y) / range, 0.0F, 1.0F);
        target_ = offset_ = t * limit;
    }
    list->AddRectFilled(thumb.min, thumb.max, to_u32(style::scrollbar, alpha), width * 0.5F);
}

// --- SocialTextInput ---------------------------------------------------------------

namespace {

char32_t code_point_at(std::wstring_view text, std::size_t index, std::size_t& units) noexcept {
    const wchar_t c = text[index];
    if (high_surrogate(c) && index + 1 < text.size() && low_surrogate(text[index + 1])) {
        units = 2;
        return 0x10000 + ((static_cast<char32_t>(c) - 0xD800) << 10) + (static_cast<char32_t>(text[index + 1]) - 0xDC00);
    }
    units = 1;
    return static_cast<char32_t>(c);
}

// Code points that belong to the character before them: combining marks
// (including the kana voicing marks), variation selectors, skin tones, emoji
// tags and the zero-width joiner.
bool extends_cluster(char32_t c) noexcept {
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) ||
           (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F) || c == 0x3099 || c == 0x309A ||
           (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF) || (c >= 0x1F3FB && c <= 0x1F3FF) ||
           (c >= 0xE0020 && c <= 0xE007F) || c == 0x200D;
}

bool regional_indicator(char32_t c) noexcept { return c >= 0x1F1E6 && c <= 0x1F1FF; }

double qpc_seconds() noexcept {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / frequency;
}

double (*clock_source)() = nullptr;

} // namespace

std::size_t social_cluster_end(std::wstring_view text, std::size_t index) noexcept {
    if (index >= text.size()) return text.size();
    std::size_t units = 0;
    const char32_t first = code_point_at(text, index, units);
    std::size_t end = index + units;
    // A flag is a pair of regional indicators.
    if (regional_indicator(first) && end < text.size() && regional_indicator(code_point_at(text, end, units))) end += units;
    bool joined = false;
    while (end < text.size()) {
        const char32_t c = code_point_at(text, end, units);
        if (!joined && !extends_cluster(c)) break;
        joined = !joined && c == 0x200D; // the code point after a joiner is part of the cluster
        end += units;
    }
    return end;
}

SocialTextInput::~SocialTextInput() = default;

double SocialTextInput::clock() noexcept { return clock_source ? clock_source() : qpc_seconds(); }

void SocialTextInput::set_clock(double (*source)()) noexcept { clock_source = source; }

SocialTextInput::Pose SocialTextInput::pose(double elapsed, float font_px) noexcept {
    // Sink for the first third, then rise and take on the normal colour; no overshoot.
    constexpr float sink = 0.33F;
    const float depth = std::clamp(font_px * 0.12F, 1.5F, 3.0F);
    const float p = static_cast<float>(std::clamp(elapsed / animation_seconds, 0.0, 1.0));
    if (p >= 1.0F) return {0.0F, 1.0F};
    if (p < sink) {
        const float t = 1.0F - p / sink;
        return {depth * (1.0F - t * t), 0.0F};
    }
    const float rise = ease_out_cubic((p - sink) / (1.0F - sink));
    return {depth * (1.0F - rise), rise};
}

void SocialTextInput::set_focused(bool focused) noexcept {
    if (focused_ == focused) return;
    focused_ = focused;
    focused_at_ = ImGui::GetCurrentContext() ? ImGui::GetTime() : -1.0;
    all_selected_ = false;
    if (focused) caret_ = text_.size();
}

void SocialTextInput::clear() {
    text_.clear();
    spawn_.clear();
    animation_end_ = 0.0;
    caret_ = 0;
    all_selected_ = false;
}

std::string SocialTextInput::utf8() const { return narrow(text_); }

std::size_t SocialTextInput::length() const noexcept {
    std::size_t count = 0;
    for (const wchar_t c : text_) if (!low_surrogate(c)) ++count;
    return count;
}

std::size_t SocialTextInput::previous(std::size_t index) const noexcept {
    for (std::size_t at = 0; at < index;) {
        const std::size_t end = social_cluster_end(text_, at);
        if (end >= index) return at;
        at = end;
    }
    return 0;
}

std::size_t SocialTextInput::next(std::size_t index) const noexcept { return social_cluster_end(text_, index); }

void SocialTextInput::edited() noexcept {
    edited_at_ = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
    ++revision_;
}

void SocialTextInput::erase(std::size_t begin, std::size_t end) {
    text_.erase(begin, end - begin);
    spawn_.erase(spawn_.begin() + static_cast<std::ptrdiff_t>(begin), spawn_.begin() + static_cast<std::ptrdiff_t>(end));
}

void SocialTextInput::erase_selection() {
    if (!all_selected_) return;
    clear();
}

void SocialTextInput::insert(std::wstring_view text, Source source) {
    erase_selection();
    // Count code points so a limit like spec 2.5's 300 matches the server.
    std::size_t room = max_code_points_ > length() ? max_code_points_ - length() : 0;
    std::size_t take = 0;
    while (take < text.size() && room > 0) {
        const bool pair = high_surrogate(text[take]) && take + 1 < text.size() && low_surrogate(text[take + 1]);
        take += pair ? 2 : 1;
        --room;
    }
    const std::size_t begin = caret_, end = caret_ + take;
    text_.insert(begin, text.substr(0, take));
    spawn_.insert(spawn_.begin() + static_cast<std::ptrdiff_t>(begin), take, 0.0);
    caret_ = end;
    if (take > 0 && animate_) {
        const double now = clock();
        // Clusters the insertion touched; a mark typed after a letter animates
        // the letter it joins. Everything else keeps its own start time.
        std::size_t moving = 0;
        for (std::size_t at = 0; at < text_.size(); at = social_cluster_end(text_, at)) {
            const std::size_t cluster_end = social_cluster_end(text_, at);
            if ((cluster_end <= begin || at >= end) && spawn_[at] > 0.0 && now < spawn_[at] + animation_seconds) ++moving;
        }
        std::size_t order = 0;
        for (std::size_t at = 0; at < text_.size();) {
            const std::size_t cluster_end = social_cluster_end(text_, at);
            if (cluster_end > begin && at < end) {
                double start = 0.0; // 0 = shown settled at once
                if (source == Source::Typed) start = now;
                else if (order < paste_animated) start = now + paste_stagger * static_cast<double>(order);
                ++order;
                if (start > 0.0 && moving < max_animating) ++moving;
                else start = 0.0;
                std::fill(spawn_.begin() + static_cast<std::ptrdiff_t>(at), spawn_.begin() + static_cast<std::ptrdiff_t>(cluster_end), start);
                if (start > 0.0) animation_end_ = std::max(animation_end_, start + animation_seconds);
            }
            at = cluster_end;
        }
    }
    // A base character inserted before existing marks would leave the caret inside a cluster.
    if (caret_ > 0 && caret_ < text_.size()) caret_ = std::max(caret_, next(previous(caret_)));
    edited();
}

bool SocialTextInput::key(int virtual_key, bool ctrl) {
    if (!focused_) return false;
    switch (virtual_key) {
    case VK_BACK:
        if (all_selected_) {
            erase_selection();
        } else if (caret_ > 0) {
            const auto start = previous(caret_);
            erase(start, caret_);
            caret_ = start;
        }
        edited();
        return true;
    case VK_DELETE:
        if (all_selected_) erase_selection();
        else if (caret_ < text_.size()) erase(caret_, next(caret_));
        edited();
        return true;
    case VK_LEFT:
        caret_ = all_selected_ ? 0 : previous(caret_);
        all_selected_ = false;
        edited();
        return true;
    case VK_RIGHT:
        caret_ = all_selected_ ? text_.size() : next(caret_);
        all_selected_ = false;
        edited();
        return true;
    case VK_HOME:
        caret_ = 0;
        all_selected_ = false;
        edited();
        return true;
    case VK_END:
        caret_ = text_.size();
        all_selected_ = false;
        edited();
        return true;
    case VK_RETURN:
        if (multiline_) insert(L"\n", Source::Typed);
        else submit_ = true;
        return true;
    case VK_UP:
    case VK_DOWN: {
        if (!multiline_) return false;
        // Same column (in UTF-16 units) on the line above or below, as far as it goes.
        const std::size_t start = line_start(caret_);
        const std::size_t column = caret_ - start;
        if (virtual_key == VK_UP) {
            if (start == 0) caret_ = 0;
            else {
                const std::size_t above = line_start(start - 1);
                caret_ = std::min(above + column, start - 1);
            }
        } else {
            const std::size_t end = line_end(caret_);
            if (end >= text_.size()) caret_ = text_.size();
            else caret_ = std::min(end + 1 + column, line_end(end + 1));
        }
        // Never inside a surrogate pair or a combining sequence.
        if (caret_ > 0 && caret_ < text_.size()) caret_ = std::max(caret_, next(previous(caret_)));
        all_selected_ = false;
        edited();
        return true;
    }
    default:
        break;
    }
    if (!ctrl) return false;
    switch (virtual_key) {
    case 'A':
        all_selected_ = !text_.empty();
        return true;
    case 'C':
    case 'X':
        if (all_selected_) {
            if (copy_.valid()) {
                if (copy_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return true;
                copy_.get();
            }
            copy_ = std::async(std::launch::async, write_clipboard, text_);
            if (virtual_key == 'X') {
                erase_selection();
                edited();
            }
        }
        return true;
    case 'V':
        if (!paste_.valid()) paste_ = std::async(std::launch::async, read_clipboard);
        return true;
    default:
        return false;
    }
}

void SocialTextInput::type(const ImVector<ImWchar>& characters) {
    if (!focused_ || characters.empty()) return;
    std::wstring typed;
    for (const ImWchar c : characters) {
        // Control characters (Ctrl+letter, Backspace, Tab) are handled as keys.
        if (c < 0x20 || c == 0x7F) continue;
        typed.push_back(static_cast<wchar_t>(c));
    }
    if (!typed.empty()) insert(typed, Source::Typed);
}

void SocialTextInput::type(std::wstring_view text) {
    if (!focused_ || text.empty()) return;
    std::wstring typed;
    for (const wchar_t c : text)
        if (c >= 0x20 && c != 0x7F) typed.push_back(c);
    if (!typed.empty()) insert(typed, Source::Typed);
}

void SocialTextInput::set_composition(std::wstring text, int cursor) {
    if (text != composition_) edited();
    composition_ = std::move(text);
    composition_cursor_ = std::clamp(cursor, 0, static_cast<int>(composition_.size()));
}

void SocialTextInput::update() {
    if (!paste_.valid() || paste_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const std::wstring text = paste_.get();
    if (focused_) paste(text);
}

void SocialTextInput::paste(std::wstring_view text) {
    std::wstring line;
    if (multiline_) {
        // Line breaks kept (CRLF and CR become LF), tabs dropped.
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == L'\r') {
                line.push_back(L'\n');
                if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            } else if (text[i] != L'\t') {
                line.push_back(text[i]);
            }
        }
    } else {
        // Single line: keep what comes before the first line break, without tabs.
        line.assign(text.substr(0, std::min(text.size(), text.find_first_of(L"\r\n"))));
        std::erase(line, L'\t');
    }
    insert(line, Source::Pasted);
}

void SocialTextInput::set_text(std::wstring_view text) {
    clear();
    text_.assign(text);
    spawn_.assign(text_.size(), 0.0);
    caret_ = text_.size();
    edited();
}

std::size_t SocialTextInput::line_start(std::size_t index) const noexcept {
    const auto found = index == 0 ? std::wstring::npos : text_.rfind(L'\n', index - 1);
    return found == std::wstring::npos ? 0 : found + 1;
}

std::size_t SocialTextInput::line_end(std::size_t index) const noexcept {
    const auto found = text_.find(L'\n', index);
    return found == std::wstring::npos ? text_.size() : found;
}

bool SocialTextInput::consume_submit() noexcept { return std::exchange(submit_, false); }

void SocialTextInput::draw(ImDrawList* list, const SocialFonts& fonts, const SocialRect& rect, float scale, float alpha,
                           std::string_view placeholder, bool enabled, double now, IShadowRenderer* glow) const {
    const float fade = alpha * (enabled ? 1.0F : 0.6F);
    const float radius = metrics::input_radius * scale;
    if (multiline_) {
        draw_multiline(list, fonts, rect, scale, fade, placeholder, enabled, now);
        return;
    }
    // Focus fades a soft blue glow in (and out) over the focus transition.
    const float since = focused_at_ < 0.0 ? 1.0F
                                          : static_cast<float>(std::clamp((now - focused_at_) / style::timing::focus_glow, 0.0, 1.0));
    const float focus = ease_out_cubic(focused_ ? since : 1.0F - since);
    if (glow && focus > 0.0F) {
        glow->draw(list, rect.min, rect.max, CornerRadii::all(radius),
                   ShadowParams{style::input_glow_opacity, social_button_glow_sigma(scale), 0.0F, 0.0F, to_u32(style::glow)},
                   fade * focus);
    }
    list->AddRectFilled(rect.min, rect.max, to_u32(style::input_fill, fade), radius);
    list->AddRect(rect.min, rect.max, to_u32(lerp(style::input_border, style::input_focus, focus), fade), radius, 0,
                  std::max(1.0F, scale * (1.0F + 0.5F * focus)));
    const float padding = metrics::input_padding * scale;
    const float line = fonts.line_height(SocialText::Body);
    const float top = rect.min.y + (rect.height() - line) * 0.5F;
    // The vertical padding (at least the 3 px dip) keeps a sinking character inside the clip.
    list->PushClipRect({rect.min.x + padding * 0.5F, rect.min.y}, {rect.max.x - padding * 0.5F, rect.max.y}, true);
    float caret_x = rect.min.x + padding;
    if (!composition_.empty() && !masked_) {
        // IME: the unconfirmed text sits at the caret, underlined, and never animates.
        const std::wstring_view all(text_);
        const std::string before = narrow(all.substr(0, caret_));
        const std::string composing = narrow(composition_);
        const std::string inside = narrow(std::wstring_view(composition_).substr(0, static_cast<std::size_t>(composition_cursor_)));
        const std::string after = narrow(all.substr(caret_));
        const float start = fonts.measure(SocialText::Body, before);
        const float width = fonts.measure(SocialText::Body, composing);
        const float cursor = start + fonts.measure(SocialText::Body, inside);
        const float shift = std::max(0.0F, cursor - (rect.width() - padding * 2.0F));
        const float x = rect.min.x + padding - shift;
        fonts.draw(list, SocialText::Body, x, top, to_u32(style::text, fade), before + composing + after);
        const float underline = std::max(1.0F, scale);
        list->AddRectFilled({x + start, top + line - underline * 2.0F}, {x + start + width, top + line - underline},
                            to_u32(style::input_focus, fade));
        caret_x = x + cursor;
    } else if (text_.empty()) {
        if (!focused_) fonts.draw(list, SocialText::Body, rect.min.x + padding, top, to_u32(style::placeholder, fade), placeholder);
    } else {
        // The masked field holds the license key. Never expose the key in the UI.
        std::string shown, before;
        if (masked_) {
            for (std::size_t at = 0; at < text_.size(); at = social_cluster_end(text_, at)) {
                shown.push_back('*');
                if (at < caret_) before.push_back('*');
            }
        } else {
            shown = narrow(text_);
            before = narrow(std::wstring_view(text_).substr(0, caret_));
        }
        const float before_caret = fonts.measure(SocialText::Body, before);
        // Keep the caret inside the box: a longer text slides left.
        const float shift = std::max(0.0F, before_caret - (rect.width() - padding * 2.0F));
        const float x = rect.min.x + padding - shift;
        if (all_selected_) {
            list->AddRectFilled({x, top}, {x + fonts.measure(SocialText::Body, shown), top + line},
                                to_u32(style::selection, fade));
        }
        const double time = clock();
        if (animate_ && animating(time)) draw_animated(list, fonts, x, top, fade, time, 0, text_.size());
        else fonts.draw(list, SocialText::Body, x, top, to_u32(style::text, fade), shown);
        // The caret sits at the final layout position and never follows a dip.
        caret_x = x + before_caret;
    }
    caret_position_ = {caret_x, top};
    caret_height_ = line;
    const float blink = style::timing::caret_blink;
    if (focused_ && enabled && std::fmod(std::max(0.0, now - edited_at_), static_cast<double>(blink) * 2.0) < blink) {
        const float width = std::max(1.0F, 1.5F * scale);
        list->AddRectFilled({std::round(caret_x), top + line * 0.12F}, {std::round(caret_x) + width, top + line * 0.88F},
                            to_u32(style::text, fade));
    }
    list->PopClipRect();
}

void SocialTextInput::draw_multiline(ImDrawList* list, const SocialFonts& fonts, const SocialRect& rect, float scale,
                                     float fade, std::string_view placeholder, bool enabled, double now) const {
    const float radius = metrics::input_radius * scale;
    const float focus = focused_ ? 1.0F : 0.0F;
    list->AddRectFilled(rect.min, rect.max, to_u32(style::input_fill, fade), radius);
    list->AddRect(rect.min, rect.max, to_u32(lerp(style::input_border, style::input_focus, focus), fade), radius, 0,
                  std::max(1.0F, scale * (1.0F + 0.5F * focus)));
    const float padding = metrics::input_padding * scale;
    const float line = fonts.line_height(SocialText::Body);
    const float x = rect.min.x + padding;
    const float view_top = rect.min.y + padding * 0.75F;
    const float view_height = rect.height() - padding * 1.5F;
    // The caret's line stays in view.
    const auto caret_line = static_cast<float>(std::count(text_.begin(), text_.begin() + static_cast<std::ptrdiff_t>(caret_), L'\n'));
    scroll_ = std::clamp(scroll_, std::max(0.0F, (caret_line + 1.0F) * line - view_height), caret_line * line);
    list->PushClipRect({rect.min.x + padding * 0.5F, rect.min.y + 2.0F}, {rect.max.x - padding * 0.5F, rect.max.y - 2.0F}, true);
    if (text_.empty() && composition_.empty()) {
        if (!focused_) fonts.draw(list, SocialText::Body, x, view_top, to_u32(style::placeholder, fade), placeholder);
    }
    const double time = clock();
    const bool moving = animate_ && animating(time);
    float caret_x = x, caret_y = view_top - scroll_ + caret_line * line;
    float top = view_top - scroll_;
    for (std::size_t start = 0;; top += line) {
        const std::size_t end = line_end(start);
        const bool has_caret = caret_ >= start && caret_ <= end;
        if (top + line >= rect.min.y && top <= rect.max.y) {
            if (all_selected_ && end > start) {
                list->AddRectFilled({x, top}, {x + fonts.measure(SocialText::Body, narrow(std::wstring_view(text_).substr(start, end - start))), top + line},
                                    to_u32(style::selection, fade));
            }
            if (has_caret && !composition_.empty()) {
                // IME: the unconfirmed text sits at the caret, underlined.
                const std::string before = narrow(std::wstring_view(text_).substr(start, caret_ - start));
                const std::string composing = narrow(composition_);
                const std::string inside = narrow(std::wstring_view(composition_).substr(0, static_cast<std::size_t>(composition_cursor_)));
                const float from = fonts.measure(SocialText::Body, before);
                fonts.draw(list, SocialText::Body, x, top, to_u32(style::text, fade),
                           before + composing + narrow(std::wstring_view(text_).substr(caret_, end - caret_)));
                const float underline = std::max(1.0F, scale);
                list->AddRectFilled({x + from, top + line - underline * 2.0F},
                                    {x + from + fonts.measure(SocialText::Body, composing), top + line - underline},
                                    to_u32(style::input_focus, fade));
                caret_x = x + from + fonts.measure(SocialText::Body, inside);
            } else {
                if (moving) draw_animated(list, fonts, x, top, fade, time, start, end);
                else fonts.draw(list, SocialText::Body, x, top, to_u32(style::text, fade),
                                narrow(std::wstring_view(text_).substr(start, end - start)));
                if (has_caret) caret_x = x + fonts.measure(SocialText::Body, narrow(std::wstring_view(text_).substr(start, caret_ - start)));
            }
        }
        if (end >= text_.size()) break;
        start = end + 1;
    }
    caret_position_ = {caret_x, caret_y};
    caret_height_ = line;
    const float blink = style::timing::caret_blink;
    if (focused_ && enabled && std::fmod(std::max(0.0, now - edited_at_), static_cast<double>(blink) * 2.0) < blink) {
        const float width = std::max(1.0F, 1.5F * scale);
        list->AddRectFilled({std::round(caret_x), caret_y + line * 0.12F}, {std::round(caret_x) + width, caret_y + line * 0.88F},
                            to_u32(style::text, fade));
    }
    list->PopClipRect();
}

void SocialTextInput::draw_animated(ImDrawList* list, const SocialFonts& fonts, float x, float top, float fade,
                                    double now, std::size_t begin, std::size_t end_index) const {
    // Settled clusters are drawn in runs and moving ones one by one. Every piece
    // is then shifted to where the single-string draw puts that glyph (which
    // snaps only the start of the line), so nothing but the moving characters
    // differs from the plain path, and nothing at all once they settle.
    const float origin = std::round(x);
    const auto place = [&](int first_vertex, float pen, float dip) {
        const float dx = origin + (pen - x) - std::round(pen);
        for (int i = first_vertex; i < list->VtxBuffer.Size; ++i) {
            list->VtxBuffer[i].pos.x += dx;
            list->VtxBuffer[i].pos.y += dip;
        }
    };
    const ImU32 settled = to_u32(style::text, fade);
    const Color grey = lerp(style::text, style::typing_grey, style::typing_grey_mix);
    const float font_px = fonts.em(SocialText::Body);
    std::string run;
    float run_pen = x;
    const auto flush = [&] {
        if (run.empty()) return;
        const int first = list->VtxBuffer.Size;
        fonts.draw(list, SocialText::Body, run_pen, top, settled, run);
        place(first, run_pen, 0.0F);
        run.clear();
    };
    float pen = x;
    char piece[64];
    for (std::size_t at = begin; at < end_index;) {
        const std::size_t end = std::min(social_cluster_end(text_, at), end_index);
        std::string_view glyphs = "*";
        if (!masked_) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, text_.data() + at, static_cast<int>(end - at), piece,
                                                  static_cast<int>(sizeof piece), nullptr, nullptr);
            glyphs = std::string_view(piece, static_cast<std::size_t>(std::max(bytes, 0)));
        }
        const double spawn = spawn_[at];
        if (spawn > 0.0 && now < spawn + animation_seconds) {
            flush();
            // A staggered paste character waits in its start colour.
            const Pose now_pose = pose(std::max(0.0, now - spawn), font_px);
            const Color color = lerp(grey, style::text, now_pose.blend);
            const float opacity = style::typing_start_opacity + (1.0F - style::typing_start_opacity) * now_pose.blend;
            const int first = list->VtxBuffer.Size;
            fonts.draw(list, SocialText::Body, pen, top, to_u32(color, fade * opacity), glyphs);
            place(first, pen, now_pose.dip);
        } else {
            if (run.empty()) run_pen = pen;
            run.append(glyphs);
        }
        pen += fonts.measure(SocialText::Body, glyphs);
        at = end;
    }
    flush();
}

// --- Drawing ------------------------------------------------------------------------

void SocialMotion::step(bool hovered, bool held, float dt) noexcept {
    hover = std::clamp(follow(hover, hovered ? 1.0F : 0.0F, dt, style::timing::hover_tau), 0.0F, 1.0F);
    press = std::clamp(follow(press, held ? 1.0F : 0.0F, dt, style::timing::press_tau), 0.0F, 1.0F);
}

float social_button_glow_sigma(float scale) noexcept { return metrics::button_glow * 0.5F * scale; }

void draw_social_button(ImDrawList* list, const SocialFonts& fonts, IShadowRenderer* glow, const SocialRect& rect,
                        std::string_view label, SocialButton kind, const SocialMotion& motion, bool enabled,
                        float scale, float alpha) {
    const float fade = alpha * (enabled ? 1.0F : 0.7F);
    const float hover = enabled ? motion.hover : 0.0F;
    const ImVec2 center{(rect.min.x + rect.max.x) * 0.5F, (rect.min.y + rect.max.y) * 0.5F};
    // Pressed: 0.98, eased with the press.
    const ScaleAround press(list, center, 1.0F - (1.0F - metrics::button_press_scale) * (enabled ? motion.press : 0.0F));
    GlassPlate plate;
    plate.hover = hover;
    plate.press = enabled ? motion.press : 0.0F;
    plate.fill = style::plate;
    plate.alpha = fade;
    plate.glow = enabled ? glow : nullptr;
    plate.glow_sigma = social_button_glow_sigma(scale);
    plate.accent_inset = rect.height() * metrics::accent_inset;
    draw_glass_plate(list, rect.min, rect.max, metrics::button_radius * scale, plate);
    const Color& ink = kind == SocialButton::Danger ? style::error : style::heading;
    const float width = fonts.measure(SocialText::Title, label);
    const float line = fonts.line_height(SocialText::Title);
    // The accent line sits on the left; the label is centred in what remains.
    const float accent = metrics::button_radius * scale * 0.6F;
    fonts.draw(list, SocialText::Title, rect.min.x + accent + (rect.width() - accent - width) * 0.5F,
               rect.min.y + (rect.height() - line) * 0.5F, to_u32(ink, fade), label);
}

void draw_social_toggle(ImDrawList* list, const SocialRect& rect, bool on, float hover, float alpha) {
    const float radius = rect.height() * 0.5F;
    const Color& track = on ? style::toggle_on : style::toggle_off;
    list->AddRectFilled(rect.min, rect.max, to_u32(track, alpha * (0.88F + 0.12F * hover)), radius);
    list->AddRect(rect.min, rect.max, to_u32(theme::white, alpha * 0.7F), radius);
    const float knob = radius - std::max(2.0F, rect.height() * 0.12F);
    const float x = on ? rect.max.x - radius : rect.min.x + radius;
    list->AddCircleFilled({x, rect.min.y + radius + std::max(1.0F, knob * 0.15F)}, knob, to_u32(style::knob_drop, alpha), 24);
    list->AddCircleFilled({x, rect.min.y + radius}, knob, to_u32(style::knob, alpha), 24);
}

void draw_social_slider(ImDrawList* list, const SocialRect& rect, float value, bool hovered, float scale, float alpha) {
    const float y = (rect.min.y + rect.max.y) * 0.5F;
    const float half = std::max(1.0F, 2.5F * scale);
    const float x = rect.min.x + rect.width() * std::clamp(value, 0.0F, 1.0F);
    list->AddRectFilled({rect.min.x, y - half}, {rect.max.x, y + half}, to_u32(style::toggle_off, alpha), half);
    list->AddRectFilled({rect.min.x, y - half}, {x, y + half}, to_u32(style::toggle_on, alpha), half);
    const float knob = (hovered ? 9.0F : 8.0F) * scale;
    list->AddCircleFilled({x, y + std::max(1.0F, 1.2F * scale)}, knob, to_u32(style::knob_drop, alpha), 24);
    list->AddCircleFilled({x, y}, knob, to_u32(style::knob, alpha), 24);
    list->AddCircle({x, y}, knob, to_u32(style::toggle_on, alpha), 24, std::max(1.0F, 1.5F * scale));
}

void draw_social_gear(ImDrawList* list, ImVec2 center, float radius, ImU32 color) {
    constexpr int teeth = 8;
    constexpr float tau = 6.28318530718F;
    for (int i = 0; i < teeth; ++i) {
        const float angle = tau * static_cast<float>(i) / teeth;
        const ImVec2 along{std::cos(angle), std::sin(angle)};
        const ImVec2 across{-along.y, along.x};
        const float half = radius * 0.17F;
        const float inner = radius * 0.62F;
        const auto point = [&](float distance, float side) {
            return ImVec2{center.x + along.x * distance + across.x * side, center.y + along.y * distance + across.y * side};
        };
        list->AddQuadFilled(point(inner, -half), point(radius, -half * 0.8F), point(radius, half * 0.8F), point(inner, half),
                            color);
    }
    list->AddCircle(center, radius * 0.52F, color, 24, radius * 0.30F);
}

} // namespace medicon::gui
