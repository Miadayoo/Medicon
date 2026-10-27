#include <medicon/gui/SocialFonts.hpp>

#include <medicon/gui/Fonts.hpp>
#include <medicon/gui/Log.hpp>
#include <medicon/gui/Textures.hpp>
#include <medicon_embedded_fonts.hpp>

#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace medicon::gui {
namespace {

constexpr std::array<float, social_text_count> reference_sizes{38.0F, 21.0F, 16.0F, 14.0F, 12.0F};
constexpr std::uint32_t no_break = 0xFFFFFFFFu;
constexpr char32_t ellipsis = 0x2026;
// Pending glyphs draw as this blank until the build that adds them lands.
constexpr char32_t ideographic_space = 0x3000;

std::size_t index(SocialText role) { return static_cast<std::size_t>(role); }

bool in_base(char32_t c) {
    return (c >= 0x20 && c <= 0x7E) || (c >= 0xA0 && c <= 0xFF) || c == ellipsis || c == ideographic_space;
}

template <class Function> void for_each_codepoint(std::string_view text, Function&& function) {
    const char* cursor = text.data();
    const char* end = cursor + text.size();
    while (cursor < end) {
        unsigned int c = static_cast<unsigned char>(*cursor);
        if (c < 0x80) ++cursor;
        else cursor += ImTextCharFromUtf8(&c, cursor, end);
        function(c);
    }
}

bool cjk(unsigned c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF);
}

// Kinsoku: what may not start a line, and what may not end one.
bool no_line_start(unsigned c) {
    constexpr std::u32string_view list = U"、。，．,.:;!?！？）)」』】〕〉》］｝ーぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ…‥・：；";
    return list.find(static_cast<char32_t>(c)) != std::u32string_view::npos;
}
bool no_line_end(unsigned c) {
    constexpr std::u32string_view list = U"（(「『【〔〈《［｛";
    return list.find(static_cast<char32_t>(c)) != std::u32string_view::npos;
}
bool breakable(unsigned before, unsigned after) {
    if (before == ' ') return true;
    if (no_line_start(after) || no_line_end(before)) return false;
    return cjk(before) || cjk(after);
}

std::vector<ImWchar> make_ranges(const std::set<char32_t>& glyphs) {
    std::set<char32_t> points(glyphs);
    for (char32_t c = 0x20; c <= 0x7E; ++c) points.insert(c);
    for (char32_t c = 0xA0; c <= 0xFF; ++c) points.insert(c);
    points.insert(ellipsis);
    points.insert(ideographic_space);
    std::vector<ImWchar> pairs;
    for (auto it = points.begin(); it != points.end();) {
        const char32_t first = *it;
        char32_t last = first;
        ++it;
        while (it != points.end() && *it == last + 1) last = *it++;
        pairs.push_back(static_cast<ImWchar>(first));
        pairs.push_back(static_cast<ImWchar>(last));
    }
    pairs.push_back(0);
    return pairs;
}

} // namespace

struct SocialFonts::Face {
    // Both embedded in the binary: nothing to free, nothing read from disk.
    std::span<const unsigned char> latin, japanese;
    FontFaceMetrics latin_metrics, japanese_metrics;
    std::string name;
};

struct SocialFonts::Build {
    std::shared_ptr<const Face> face; // the atlas reads glyph data from these bytes
    std::unique_ptr<ImFontAtlas> atlas;
    std::array<std::vector<ImWchar>, social_text_count> ranges; // referenced by the atlas sources
    std::array<ImFont*, social_text_count> fonts{};
    Glyphs glyphs;
    Sizes sizes{};
    unsigned char* pixels = nullptr; // owned by the atlas; kept for a later re-upload
    int width = 0, height = 0;
};

SocialFonts::SocialFonts() = default;
SocialFonts::~SocialFonts() = default;

std::shared_ptr<const SocialFonts::Face> SocialFonts::load_face() {
    // The Main Menu's look: DynaPuff for Latin text, kawai.otf for Japanese.
    auto face = std::make_shared<Face>();
    face->latin = std::span<const unsigned char>(embedded_fonts::dynapuff_medium, sizeof embedded_fonts::dynapuff_medium);
    face->latin_metrics = find_font_face(face->latin.data(), face->latin.size(), {}, {});
    face->japanese = bundled_japanese_font();
    face->japanese_metrics = find_font_face(face->japanese.data(), face->japanese.size(), {}, {});
    if (!face->latin_metrics.valid || !face->japanese_metrics.valid) return nullptr;
    face->name = "DynaPuff + kawai.otf";
    return face;
}

std::unique_ptr<SocialFonts::Build> SocialFonts::build(std::shared_ptr<const Face> face, Sizes sizes, Glyphs glyphs) {
    // Worker thread. GImGui is per thread (ImGuiConfig.hpp), so ImGui's allocator
    // never touches the render thread's context from here.
    auto result = std::make_unique<Build>();
    result->face = std::move(face);
    result->atlas = std::make_unique<ImFontAtlas>();
    auto& atlas = *result->atlas;
    // Text only, plus the white pixel and baked lines that shapes use in a Scope.
    atlas.Flags |= ImFontAtlasFlags_NoMouseCursors;
    const auto& face_data = *result->face;
    // Sizes are em px, as in CSS; ImGui sizes a face by ascent minus descent.
    const auto raster = [](float em, const FontFaceMetrics& metrics) {
        return em * static_cast<float>(metrics.ascender - metrics.descender) / static_cast<float>(metrics.units_per_em);
    };
    for (std::size_t i = 0; i < social_text_count; ++i) {
        result->ranges[i] = make_ranges(glyphs[i]);
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.FontNo = face_data.latin_metrics.index;
        config.OversampleH = sizes[i] >= 48.0F ? 1 : 2;
        config.OversampleV = 1;
        std::snprintf(config.Name, sizeof config.Name, "Social DynaPuff %.0fpx", sizes[i]);
        const float latin_px = raster(sizes[i], face_data.latin_metrics);
        ImFont* font = atlas.AddFontFromMemoryTTF(const_cast<unsigned char*>(face_data.latin.data()),
                                                  static_cast<int>(face_data.latin.size()), latin_px, &config,
                                                  result->ranges[i].data());
        if (!font) return nullptr;
        atlas.Sources.back().SizePixels = latin_px;
        // kawai.otf fills in what DynaPuff lacks (Japanese); DynaPuff's own glyphs win.
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.FontDataOwnedByAtlas = false;
        merge.FontNo = face_data.japanese_metrics.index;
        merge.OversampleH = config.OversampleH;
        merge.OversampleV = 1;
        const float japanese_px = raster(sizes[i], face_data.japanese_metrics);
        if (!atlas.AddFontFromMemoryTTF(const_cast<unsigned char*>(face_data.japanese.data()),
                                        static_cast<int>(face_data.japanese.size()), japanese_px, &merge,
                                        result->ranges[i].data()))
            return nullptr;
        atlas.Sources.back().SizePixels = japanese_px;
        font->FallbackChar = static_cast<ImWchar>(ideographic_space);
        result->fonts[i] = font;
    }
    if (!atlas.Build()) return nullptr;
    atlas.GetTexDataAsRGBA32(&result->pixels, &result->width, &result->height);
    if (!result->pixels) return nullptr;
    result->glyphs = std::move(glyphs);
    result->sizes = sizes;
    return result;
}

void SocialFonts::request(SocialText role, std::string_view utf8) {
    auto& glyphs = requested_[index(role)];
    for_each_codepoint(utf8, [&](unsigned int c) {
        if (c < 0x20 || c > 0xFFFF || in_base(static_cast<char32_t>(c))) return;
        if (glyphs.insert(static_cast<char32_t>(c)).second) dirty_ = true;
    });
}

void SocialFonts::forget_texture() noexcept {
    texture_ = 0;
    host_ = nullptr;
}

void SocialFonts::upload(TextureHost* host) {
    Build& source = pending_ ? *pending_ : *current_;
    const ImTextureID id = host->create(source.pixels, source.width, source.height);
    if (!id) return;
    if (texture_ && host_) host_->release(texture_);
    texture_ = id;
    host_ = host;
    source.atlas->SetTexID(id);
    if (pending_) {
        current_ = std::move(pending_);
        ++revision_;
    }
}

void SocialFonts::update(float display_height, TextureHost* host) {
    Sizes wanted{};
    for (std::size_t i = 0; i < social_text_count; ++i) {
        // Whole pixels keep the number of distinct builds small during a resize.
        wanted[i] = std::max(8.0F, std::round(reference_sizes[i] * display_height / 1080.0F));
    }
    if (wanted != sizes_) {
        sizes_ = wanted;
        dirty_ = true;
    }
    if (!face_) {
        if (face_failed_) return;
        if (!face_loading_.valid()) face_loading_ = std::async(std::launch::async, &SocialFonts::load_face);
        if (face_loading_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        face_ = face_loading_.get();
        if (!face_) {
            face_failed_ = true;
            log_line("Social text: the bundled fonts could not be read.");
            return;
        }
        log_line("Social text: " + face_->name);
    }
    if (building_.valid() && building_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        if (auto finished = building_.get()) pending_ = std::move(finished);
        else log_line("Social text: building the font atlas failed.");
    }
    if (host && (pending_ || (current_ && !texture_))) upload(host);
    if (dirty_ && !building_.valid()) {
        dirty_ = false;
        building_ = std::async(std::launch::async, &SocialFonts::build, face_, sizes_, requested_);
    }
}

bool SocialFonts::ready() const noexcept { return current_ && texture_ && current_->sizes == sizes_; }

bool SocialFonts::has(SocialText role, std::string_view utf8) const {
    if (!ready()) return false;
    const auto& glyphs = current_->glyphs[index(role)];
    bool all = true;
    for_each_codepoint(utf8, [&](unsigned int c) {
        if (c >= 0x20 && c <= 0xFFFF && !in_base(static_cast<char32_t>(c)) && !glyphs.contains(static_cast<char32_t>(c)))
            all = false;
    });
    return all;
}

ImFont* SocialFonts::font(SocialText role) const noexcept {
    return ready() ? current_->fonts[index(role)] : nullptr;
}

float SocialFonts::line_height(SocialText role) const noexcept {
    if (const auto* f = font(role)) return f->FontSize;
    return sizes_[index(role)] * 1.3F;
}

float SocialFonts::em(SocialText role) const noexcept { return sizes_[index(role)]; }

float SocialFonts::measure(SocialText role, std::string_view text) const {
    auto* f = font(role);
    if (!f || text.empty()) return 0.0F;
    return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0F, text.data(), text.data() + text.size()).x;
}

void SocialFonts::draw(ImDrawList* list, SocialText role, float x, float top, ImU32 color, std::string_view text) const {
    auto* f = font(role);
    if (!f || !list || text.empty()) return;
    list->AddText(f, f->FontSize, {std::round(x), std::round(top)}, color, text.data(), text.data() + text.size());
}

std::string_view SocialFonts::ellipsize(SocialText role, std::string_view text, float max_width, std::string& scratch) const {
    auto* f = font(role);
    if (!f || measure(role, text) <= max_width) return text;
    constexpr char dots[] = "\xE2\x80\xA6"; // U+2026
    const float dots_width = f->GetCharAdvance(static_cast<ImWchar>(ellipsis));
    if (!(dots_width <= max_width)) return {};
    float width = 0.0F;
    std::size_t keep = 0;
    const char* cursor = text.data();
    const char* end = cursor + text.size();
    while (cursor < end) {
        unsigned int c = static_cast<unsigned char>(*cursor);
        const char* next = c < 0x80 ? cursor + 1 : cursor + ImTextCharFromUtf8(&c, cursor, end);
        width += f->GetCharAdvance(static_cast<ImWchar>(c));
        if (width + dots_width > max_width) break;
        cursor = next;
        keep = static_cast<std::size_t>(cursor - text.data());
    }
    scratch.assign(text.data(), keep);
    scratch.append(dots);
    return scratch;
}

void SocialFonts::wrap(SocialText role, std::string_view text, float width, std::vector<Line>& lines) const {
    lines.clear();
    const auto total = static_cast<std::uint32_t>(text.size());
    auto* f = font(role);
    if (!f) {
        lines.push_back({0, total});
        return;
    }
    const auto measure_range = [&](std::uint32_t begin, std::uint32_t end) {
        return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0F, text.data() + begin, text.data() + end).x;
    };
    const auto push = [&](std::uint32_t begin, std::uint32_t end) {
        while (end > begin && text[end - 1] == ' ') --end;
        lines.push_back({begin, end});
    };
    std::uint32_t line_begin = 0, break_at = no_break, i = 0;
    float line_width = 0.0F;
    unsigned int previous = 0;
    while (i < total) {
        unsigned int c = static_cast<unsigned char>(text[i]);
        std::uint32_t next = i + 1;
        if (c >= 0x80) next = i + static_cast<std::uint32_t>(ImTextCharFromUtf8(&c, text.data() + i, text.data() + total));
        if (c == '\n') {
            push(line_begin, i);
            line_begin = next;
            line_width = 0.0F;
            break_at = no_break;
            previous = 0;
            i = next;
            continue;
        }
        if (c == '\r') {
            i = next;
            continue;
        }
        if (i > line_begin && breakable(previous, c)) break_at = i;
        const float advance = f->GetCharAdvance(static_cast<ImWchar>(c));
        if (line_width + advance > width && i > line_begin) {
            // Break at the last opportunity; a word longer than the line is cut here.
            const std::uint32_t end = break_at != no_break ? break_at : i;
            push(line_begin, end);
            line_begin = end;
            while (line_begin < i && text[line_begin] == ' ') ++line_begin;
            line_width = measure_range(line_begin, i);
            break_at = no_break;
        }
        line_width += advance;
        previous = c;
        i = next;
    }
    push(line_begin, total);
}

SocialFonts::Scope::Scope(const SocialFonts& fonts, ImDrawList* list) {
    if (!list || !fonts.ready()) return;
    list_ = list;
    list->PushTextureID(fonts.texture_);
    auto& shared = *list->_Data;
    white_ = shared.TexUvWhitePixel;
    lines_ = shared.TexUvLines;
    shared.TexUvWhitePixel = fonts.current_->atlas->TexUvWhitePixel;
    shared.TexUvLines = fonts.current_->atlas->TexUvLines;
}

SocialFonts::Scope::~Scope() {
    if (!list_) return;
    list_->_Data->TexUvWhitePixel = white_;
    list_->_Data->TexUvLines = lines_;
    list_->PopTextureID();
}

} // namespace medicon::gui
