#include <medicon/gui/Fonts.hpp>
#include <medicon/gui/Log.hpp>
#include <medicon_embedded_fonts.hpp>
#include <medicon_embedded_splash.hpp>
#include <imgui_internal.h>

#include <Windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>

namespace medicon::gui {
namespace {

constexpr char32_t cjk_first = 0x2E80;
constexpr char32_t ellipsis = 0x2026;
// The dedicated splash face needs only the four title glyphs: メジコン.
constexpr ImWchar splash_ranges[]{0x3053, 0x3053, 0x3058, 0x3058, 0x3081, 0x3081, 0x3093, 0x3093, // めじこん
                                  0x30B3, 0x30B3, 0x30B8, 0x30B8, 0x30E1, 0x30E1, 0x30F3, 0x30F3, 0}; // メジコン
// The large menu face only needs "Medicon". A separate cache weight prevents
// these restricted ranges being reused for ordinary text at the same size.
constexpr ImWchar main_menu_logo_ranges[]{'M', 'M', 'c', 'e', 'i', 'i', 'n', 'o', 0};

template <class T> struct Com {
    T* pointer = nullptr;
    ~Com() { if (pointer) pointer->Release(); }
    T** put() { return &pointer; }
    T* operator->() const { return pointer; }
};

struct ComScope {
    // A host may already use another apartment model. RPC_E_CHANGED_MODE still
    // leaves COM available, but only a successful initialization needs balancing.
    bool owned = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    ~ComScope() { if (owned) CoUninitialize(); }
};

// A 1/64-pixel key retains fractional render sizes without float map keys.
int size_key(float px) { return static_cast<int>(std::lround(px * 64.0F)); }

std::uint16_t be16(const unsigned char* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
std::int16_t be16s(const unsigned char* p) { return static_cast<std::int16_t>(be16(p)); }
std::uint32_t be32(const unsigned char* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

struct Span {
    const unsigned char* data;
    std::size_t size;
    bool has(std::size_t offset, std::size_t length) const { return offset <= size && length <= size - offset; }
};

std::vector<std::uint32_t> face_offsets(Span file) {
    if (!file.has(0, 12)) return {};
    if (std::memcmp(file.data, "ttcf", 4) == 0) {
        const auto count = be32(file.data + 8);
        std::vector<std::uint32_t> offsets;
        for (std::uint32_t i = 0; i < count && file.has(12 + 4 * i, 4); ++i) offsets.push_back(be32(file.data + 12 + 4 * i));
        return offsets;
    }
    return {0};
}

bool find_table(Span file, std::uint32_t face, const char* tag, std::uint32_t& offset, std::uint32_t& length) {
    if (!file.has(face, 12)) return false;
    const auto tables = be16(file.data + face + 4);
    for (std::uint32_t i = 0; i < tables; ++i) {
        const std::size_t record = face + 12 + 16 * i;
        if (!file.has(record, 16)) return false;
        if (std::memcmp(file.data + record, tag, 4) == 0) {
            offset = be32(file.data + record + 8);
            length = be32(file.data + record + 12);
            return file.has(offset, length);
        }
    }
    return false;
}

std::string lower_ascii(std::string_view text) {
    std::string result(text);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return result;
}

// Returns the English (or first Windows) name string for a name ID, lower-cased ASCII.
std::string name_string(Span file, std::uint32_t face, int name_id) {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    if (!find_table(file, face, "name", offset, length) || length < 6) return {};
    const unsigned char* table = file.data + offset;
    const auto count = be16(table + 2);
    const auto strings = be16(table + 4);
    std::string fallback;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t record = 6 + 12 * static_cast<std::size_t>(i);
        if (record + 12 > length) break;
        const auto platform = be16(table + record);
        const auto encoding = be16(table + record + 2);
        const auto language = be16(table + record + 4);
        const auto id = be16(table + record + 6);
        const auto size = be16(table + record + 8);
        const auto start = be16(table + record + 10);
        if (id != name_id || platform != 3 || (encoding != 1 && encoding != 10)) continue;
        if (static_cast<std::size_t>(strings) + start + size > length) continue;
        std::string value;
        for (std::uint32_t c = 0; c + 1 < size; c += 2) {
            const auto unit = be16(table + strings + start + c);
            value.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
        }
        if (language == 0x0409) return lower_ascii(value);
        if (fallback.empty()) fallback = lower_ascii(value);
    }
    return fallback;
}

std::filesystem::path fonts_directory() {
    wchar_t buffer[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return L"C:\\Windows\\Fonts";
    return std::filesystem::path(buffer) / L"Fonts";
}

void append_utf8(std::set<char32_t>& output, std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        char32_t code = 0;
        std::size_t length = 1;
        if (c < 0x80) code = c;
        else if ((c >> 5) == 0x6) { code = c & 0x1F; length = 2; }
        else if ((c >> 4) == 0xE) { code = c & 0x0F; length = 3; }
        else if ((c >> 3) == 0x1E) { code = c & 0x07; length = 4; }
        else { ++i; continue; }
        if (i + length > text.size()) break;
        for (std::size_t k = 1; k < length; ++k) code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        // Base Latin-1 + ellipsis are always rasterised (see make_ranges), so
        // per-frame collection only tracks codepoints that can actually grow
        // the atlas. This keeps the hot set tiny (CJK/custom symbols).
        if (code >= 0x20 && code <= 0xFFFF && code != ellipsis &&
            !((code >= 0x20 && code <= 0x7E) || (code >= 0xA0 && code <= 0xFF)))
            output.insert(code);
        i += length;
    }
}

} // namespace

FontManager::~FontManager() {
    // A host-owned atlas may still reference face data and glyph ranges after the
    // GUI is destroyed (e.g. DLL shutdown while the game keeps its ImGui context),
    // so those allocations are only released when the GUI owns the atlas.
    if (!owns_atlas_) return;
    release_ranges();
    for (auto& [name, file] : files_) IM_FREE(file.data);
}

void FontManager::add_text(std::string_view utf8) { append_utf8(codepoints_, utf8); }

FontWeight FontManager::role_weight(FontRole role) {
    switch (role) {
    case FontRole::ArrayList:
    case FontRole::ArrayListMode: return FontWeight::DynaPuffEm;
    case FontRole::Splash:
    case FontRole::WatermarkJapanese: return FontWeight::Kawai;
    case FontRole::MainMenuLogo: return FontWeight::DynaPuffLogo;
    default: return FontWeight::DynaPuff;
    }
}

float FontManager::role_size(FontRole role) const {
    switch (role) {
    case FontRole::Header: return sizes_.header;
    case FontRole::Module: return sizes_.module;
    case FontRole::KeyHint: return sizes_.key_hint;
    case FontRole::Label: return sizes_.label;
    case FontRole::Value: return sizes_.value;
    case FontRole::ArrayList: return sizes_.arraylist;
    case FontRole::ArrayListMode: return sizes_.arraylist_mode;
    case FontRole::Splash: return sizes_.splash;
    case FontRole::MainMenuLogo: return sizes_.main_menu_logo;
    case FontRole::MainMenuButton: return sizes_.main_menu_button;
    case FontRole::Watermark: return sizes_.watermark;
    case FontRole::WatermarkSmall: return sizes_.watermark_small;
    case FontRole::WatermarkJapanese: return sizes_.watermark_japanese;
    case FontRole::BlockCount: return sizes_.block_count;
    }
    return sizes_.module;
}

const FontManager::FileData* FontManager::load_file(const std::string& name) {
    if (const auto found = files_.find(name); found != files_.end()) return found->second.data ? &found->second : nullptr;
    FileData file{};
    const unsigned char* embedded = nullptr;
    std::size_t embedded_size = 0;
    if (name == "dynapuff.medium.ttf") {
        embedded = embedded_fonts::dynapuff_medium;
        embedded_size = sizeof embedded_fonts::dynapuff_medium;
    } else if (name == "kawai.otf") {
        embedded = embedded_splash::kawai;
        embedded_size = sizeof embedded_splash::kawai;
    }
    if (embedded) {
        file.data = IM_ALLOC(embedded_size);
        file.size = static_cast<int>(embedded_size);
        std::memcpy(file.data, embedded, embedded_size);
        return &(files_[name] = file);
    }
    std::ifstream stream(fonts_directory() / name, std::ios::binary | std::ios::ate);
    if (stream) {
        const auto size = static_cast<std::streamoff>(stream.tellg());
        if (size > 0 && size < (64 << 20)) {
            file.data = IM_ALLOC(static_cast<std::size_t>(size));
            file.size = static_cast<int>(size);
            stream.seekg(0);
            if (!stream.read(static_cast<char*>(file.data), size)) {
                IM_FREE(file.data);
                file = {};
            }
        }
    }
    auto& stored = files_[name] = file;
    return stored.data ? &stored : nullptr;
}

FontManager::FaceInfo FontManager::find_face(const std::string& name, std::string_view family, std::string_view subfamily) {
    const auto* file = load_file(name);
    if (!file) return FaceInfo{};
    const auto metrics = find_font_face(static_cast<const unsigned char*>(file->data),
                                        static_cast<std::size_t>(file->size), family, subfamily);
    if (!metrics.valid) return FaceInfo{};
    return FaceInfo{name, metrics.index, metrics.ascender, metrics.descender, metrics.cap_height, metrics.units_per_em, true};
}

std::filesystem::path system_fonts_directory() { return fonts_directory(); }

std::span<const unsigned char> bundled_japanese_font() noexcept { return embedded_splash::kawai; }

FontFaceMetrics find_font_face(const unsigned char* data, std::size_t size, std::string_view family,
                               std::string_view subfamily) {
    FontFaceMetrics info{};
    const Span span{data, size};
    const auto offsets = face_offsets(span);
    const auto wanted_family = lower_ascii(family);
    const auto wanted_sub = lower_ascii(subfamily);
    for (std::size_t index = 0; index < offsets.size(); ++index) {
        const auto face = offsets[index];
        if (!family.empty()) {
            const bool typographic = name_string(span, face, 16) == wanted_family && name_string(span, face, 17) == wanted_sub;
            const bool legacy = name_string(span, face, 1) == wanted_family && name_string(span, face, 2) == wanted_sub;
            if (!typographic && !legacy) continue;
        }
        std::uint32_t head = 0, head_len = 0, hhea = 0, hhea_len = 0, os2 = 0, os2_len = 0;
        if (!find_table(span, face, "head", head, head_len) || head_len < 20) continue;
        if (!find_table(span, face, "hhea", hhea, hhea_len) || hhea_len < 8) continue;
        info.units_per_em = be16(span.data + head + 18);
        info.ascender = be16s(span.data + hhea + 4);
        info.descender = be16s(span.data + hhea + 6);
        info.cap_height = 0;
        if (find_table(span, face, "OS/2", os2, os2_len) && os2_len >= 90 && be16(span.data + os2) >= 2) {
            info.cap_height = be16s(span.data + os2 + 88);
        }
        info.index = static_cast<int>(index);
        info.valid = info.units_per_em > 0 && info.ascender > info.descender;
        if (info.valid) return info;
    }
    return FontFaceMetrics{};
}

void FontManager::resolve_faces() {
    if (faces_resolved_) return;
    faces_resolved_ = true;
    dynapuff_ = find_face("dynapuff.medium.ttf", {}, {});
    kawai_ = find_face("kawai.otf", {}, {});
    cjk_ = kawai_;
}

ImWchar* FontManager::make_ranges(const std::set<char32_t>& codepoints, bool cjk_only) {
    std::set<char32_t> points;
    if (!cjk_only) {
        for (char32_t c = 0x20; c <= 0x7E; ++c) points.insert(c);
        for (char32_t c = 0xA0; c <= 0xFF; ++c) points.insert(c);
        points.insert(ellipsis);
    }
    for (const auto c : codepoints) if (!cjk_only || c >= cjk_first) points.insert(c);
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
    auto* ranges = static_cast<ImWchar*>(IM_ALLOC(pairs.size() * sizeof(ImWchar)));
    std::memcpy(ranges, pairs.data(), pairs.size() * sizeof(ImWchar));
    ranges_.push_back(ranges);
    return ranges;
}

void FontManager::release_ranges() {
    for (auto* ranges : ranges_) IM_FREE(ranges);
    ranges_.clear();
}

void FontManager::load_splash_image() {
    if (splash_image_loaded_) return;
    splash_image_loaded_ = true;
    ComScope com;
    const bool decoded = [this]() {
        Com<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                    reinterpret_cast<void**>(factory.put())))) return false;
        Com<IWICStream> stream;
        if (FAILED(factory->CreateStream(stream.put()))) return false;
        if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(embedded_splash::mez),
                                                static_cast<DWORD>(sizeof embedded_splash::mez)))) return false;
        Com<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromStream(stream.pointer, nullptr, WICDecodeMetadataCacheOnLoad,
                                                    decoder.put()))) return false;
        Com<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, frame.put()))) return false;
        Com<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(converter.put()))) return false;
        if (FAILED(converter->Initialize(frame.pointer, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                          nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
        UINT width = 0, height = 0;
        if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0 || width > 8190 || height > 8190)
            return false;
        std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
        if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()))) return false;
        splash_pixels_ = std::move(pixels);
        splash_image_width_ = static_cast<int>(width);
        splash_image_height_ = static_cast<int>(height);
        return true;
    }();
    if (!decoded) log_line("Bundled mez.png could not be decoded; splash picture unavailable.");
}

bool FontManager::prepare_splash_rect(ImFontAtlas& atlas) {
    load_splash_image();
    if (splash_pixels_.empty()) return false;
    const int width = splash_image_width_ + 2;
    const int height = splash_image_height_ + 2;
    if (splash_atlas_ == &atlas && splash_rect_ >= 0 && splash_rect_ < atlas.CustomRects.Size) {
        const auto* rect = atlas.GetCustomRectByIndex(splash_rect_);
        if (rect->Width == width && rect->Height == height && !rect->Font) return false;
    }
    splash_atlas_ = &atlas;
    splash_rect_ = atlas.AddCustomRectRegular(width, height);
    splash_image_ = {};
    // ImGui's area heuristic can choose 512px even though the supplied picture
    // is wider. Ensure the rectangle fits while retaining a larger host width.
    int minimum_width = 1;
    while (minimum_width < width) minimum_width *= 2;
    atlas.TexDesiredWidth = std::max(atlas.TexDesiredWidth, minimum_width);
    return true;
}

void FontManager::fill_splash_rect(ImFontAtlas& atlas) {
    splash_image_ = {};
    if (splash_atlas_ != &atlas || splash_rect_ < 0 || splash_rect_ >= atlas.CustomRects.Size) return;
    const auto* rect = atlas.GetCustomRectByIndex(splash_rect_);
    if (!rect->IsPacked()) return;
    unsigned char* pixels = nullptr;
    int atlas_width = 0, atlas_height = 0;
    atlas.GetTexDataAsRGBA32(&pixels, &atlas_width, &atlas_height);
    if (!pixels || rect->X + rect->Width > atlas_width || rect->Y + rect->Height > atlas_height) return;
    // A transparent gutter prevents neighboring glyph pixels bleeding into the
    // picture when the backend samples its edges during scale animations.
    for (int y = 0; y < rect->Height; ++y) {
        const auto start = (static_cast<std::size_t>(rect->Y + y) * atlas_width + rect->X) * 4;
        std::memset(pixels + start, 0, static_cast<std::size_t>(rect->Width) * 4);
    }
    for (int y = 0; y < splash_image_height_; ++y) {
        const auto source = static_cast<std::size_t>(y) * splash_image_width_ * 4;
        const auto destination = (static_cast<std::size_t>(rect->Y + 1 + y) * atlas_width + rect->X + 1) * 4;
        std::memcpy(pixels + destination, splash_pixels_.data() + source, static_cast<std::size_t>(splash_image_width_) * 4);
    }
    atlas.TexPixelsUseColors = true;
    splash_image_.valid = true;
    splash_image_.uv_min = ImVec2(static_cast<float>(rect->X + 1) / atlas_width,
                                  static_cast<float>(rect->Y + 1) / atlas_height);
    splash_image_.uv_max = ImVec2(static_cast<float>(rect->X + 1 + splash_image_width_) / atlas_width,
                                  static_cast<float>(rect->Y + 1 + splash_image_height_) / atlas_height);
    splash_image_.size = ImVec2(static_cast<float>(splash_image_width_), static_cast<float>(splash_image_height_));
}

bool FontManager::prepare(ImFontAtlas& atlas, const FontSizes& sizes) {
    sizes_ = sizes;
    if (atlas.Locked) return false;
    resolve_faces();
    if (!dynapuff_.valid) {
        if (description_.empty()) {
            description_ = "Bundled DynaPuff unavailable; using the host's default ImGui font.";
            log_line(description_);
        }
        return false;
    }
    const bool needs_cjk = !codepoints_.empty() && *codepoints_.rbegin() >= cjk_first;
    if (needs_cjk && !cjk_.valid) {
        // Fallback order Yu Gothic UI -> Meiryo UI, matched by family/subfamily
        // name because TTC face indices differ between Windows builds.
        for (const char* file : {"YuGothM.ttc", "YuGothR.ttc"}) {
            if (!cjk_.valid) cjk_ = find_face(file, "Yu Gothic UI", "Regular");
        }
        if (!cjk_.valid) cjk_ = find_face("meiryo.ttc", "Meiryo UI", "Regular");
    }

    const std::array<std::pair<FontWeight, float>, 14> required{{
        {role_weight(FontRole::Header), sizes.header},
        {role_weight(FontRole::Module), sizes.module},
        {role_weight(FontRole::KeyHint), sizes.key_hint},
        {role_weight(FontRole::Label), sizes.label},
        {role_weight(FontRole::Value), sizes.value},
        {role_weight(FontRole::ArrayList), sizes.arraylist},
        {role_weight(FontRole::ArrayListMode), sizes.arraylist_mode},
        {role_weight(FontRole::Splash), sizes.splash},
        {role_weight(FontRole::MainMenuLogo), sizes.main_menu_logo},
        {role_weight(FontRole::MainMenuButton), sizes.main_menu_button},
        {role_weight(FontRole::Watermark), sizes.watermark},
        {role_weight(FontRole::WatermarkSmall), sizes.watermark_small},
        {role_weight(FontRole::WatermarkJapanese), sizes.watermark_japanese},
        {role_weight(FontRole::BlockCount), sizes.block_count},
    }};
    const bool glyphs_changed = codepoints_.size() != built_codepoints_;
    bool missing = false;
    for (const auto& [weight, px] : required) {
        if (!std::isfinite(px) || px <= 0.0F) continue;
        if (!built_.contains({static_cast<int>(weight), size_key(px)})) missing = true;
    }
    const bool image_added = prepare_splash_rect(atlas);
    if (!glyphs_changed && !missing && !image_added && atlas.IsBuilt()) return false;

    if (owns_atlas_) {
        atlas.Clear();
        splash_rect_ = -1;
        splash_image_ = {};
        prepare_splash_rect(atlas);
        release_ranges();
        atlas.AddFontDefault();
        built_.clear();
    } else if (glyphs_changed) {
        // Faces with the old glyph set stay in the host atlas (cannot be removed
        // in ImGui 1.91); they are simply no longer referenced.
        built_.clear();
    }

    ImWchar* ranges = make_ranges(codepoints_, false);
    ImWchar* cjk_ranges = needs_cjk && cjk_.valid ? make_ranges(codepoints_, true) : nullptr;
    for (const auto& [weight, px] : required) {
        if (!std::isfinite(px) || px <= 0.0F) continue;
        const std::pair<int, int> key{static_cast<int>(weight), size_key(px)};
        if (built_.contains(key)) continue;
        const FaceInfo& face = weight == FontWeight::Kawai ? kawai_ : dynapuff_;
        const auto* data = load_file(face.file);
        if (!data) continue;
        const float requested_px = static_cast<float>(key.second) / 64.0F;
        const bool em_size = weight == FontWeight::DynaPuffEm;
        // CSS font-size is the em, whereas ImGui/STB's positive SizePixels is
        // ascent minus descent. Convert once before rasterising, never at draw.
        const float raster_px = em_size ? requested_px * static_cast<float>(face.ascender - face.descender) /
            static_cast<float>(face.units_per_em) : requested_px;
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.FontNo = face.index;
        // Oversampling only helps small UI sizes. Logo and splash bitmaps are already
        // larger than a pixel, and a 2x atlas of them dominates the font texture.
        config.OversampleH = raster_px >= 48.0F ? 1 : 2;
        config.OversampleV = 1;
        std::snprintf(config.Name, sizeof config.Name, "%s %.2fpx", face.file.c_str(), requested_px);
        const ImWchar* face_ranges = weight == FontWeight::Kawai ? splash_ranges :
                                     weight == FontWeight::DynaPuffLogo ? main_menu_logo_ranges : ranges;
        ImFont* font = atlas.AddFontFromMemoryTTF(data->data, data->size, raster_px, &config, face_ranges);
        if (!font) continue;
        // Pinned ImGui 1.91 rounds AddFont's size for widget layout. Our own
        // layout supports fractional sizes, so keep the exact rasterisation.
        atlas.Sources.back().SizePixels = raster_px;
        if (cjk_ranges && weight != FontWeight::Kawai && weight != FontWeight::DynaPuffLogo) {
            if (const auto* cjk_data = load_file(cjk_.file)) {
                // Match em sizes so CJK glyphs line up with DynaPuff.
                const float em = raster_px * static_cast<float>(face.units_per_em) /
                                 static_cast<float>(face.ascender - face.descender);
                const float cjk_px = em * static_cast<float>(cjk_.ascender - cjk_.descender) /
                                     static_cast<float>(cjk_.units_per_em);
                ImFontConfig merge;
                merge.MergeMode = true;
                merge.FontDataOwnedByAtlas = false;
                merge.FontNo = cjk_.index;
                merge.OversampleH = cjk_px >= 48.0F ? 1 : 2;
                merge.OversampleV = 1;
                atlas.AddFontFromMemoryTTF(cjk_data->data, cjk_data->size, cjk_px, &merge, cjk_ranges);
                atlas.Sources.back().SizePixels = cjk_px;
            }
        }
        const float scale = raster_px / static_cast<float>(face.ascender - face.descender);
        built_[key] = Built{font, 0.0F, face.cap_height > 0 ? static_cast<float>(face.cap_height) * scale : 0.0F};
    }
    if (!atlas.Build()) {
        log_line("Font atlas build failed; using the default font.");
        built_.clear();
        return true;
    }
    fill_splash_rect(atlas);
    for (auto& [key, built] : built_) {
        // ImGui places glyphs at round(Ascent) below the text origin.
        built.baseline = std::round(built.font->Ascent);
        if (built.cap <= 0.0F) {
            const ImFontGlyph* glyph = built.font->FindGlyph('H');
            built.cap = glyph ? glyph->Y1 - glyph->Y0 : built.font->FontSize * 0.7F;
        }
    }
    built_codepoints_ = codepoints_.size();
    measure_cache_.clear();

    std::string description = "English=embedded DynaPuff Medium, Splash=embedded kawai.otf";
    if (needs_cjk) {
        description += cjk_.valid ? ", CJK=" + cjk_.file + "#" + std::to_string(cjk_.index) : ", CJK=unavailable";
    }
    if (description != description_) {
        description_ = description;
        log_line("Fonts: " + description_);
    }
    return true;
}

FontFace FontManager::face(FontRole role) const {
    const float px = role_size(role);
    const auto found = built_.find({static_cast<int>(role_weight(role)), size_key(px)});
    if (found != built_.end()) return FontFace{found->second.font, found->second.font->FontSize, found->second.baseline, found->second.cap};

    // Fallback: the host's default font scaled to the requested size.
    FontFace fallback{};
    auto& io = ImGui::GetIO();
    fallback.font = io.FontDefault ? io.FontDefault : (io.Fonts->Fonts.Size > 0 ? io.Fonts->Fonts[0] : nullptr);
    fallback.size = px;
    if (fallback.font && fallback.font->FontSize > 0.0F) {
        const float scale = px / fallback.font->FontSize;
        if (const ImFontGlyph* glyph = fallback.font->FindGlyph('H')) {
            fallback.baseline = std::round(glyph->Y1 * scale);
            fallback.cap_height = (glyph->Y1 - glyph->Y0) * scale;
        } else {
            fallback.baseline = std::round(fallback.font->Ascent * scale);
            fallback.cap_height = px * 0.7F;
        }
    }
    return fallback;
}

static std::uint64_t hash_text(std::string_view text) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash ^ (static_cast<std::uint64_t>(text.size()) * 0x9E3779B97F4A7C15ull);
}

float FontManager::measure(FontRole role, std::string_view text) {
    if (text.empty()) return 0.0F;
    const FontFace f = face(role);
    if (!f.font) return 0.0F;
    // Font pointer, raster size and a stack hash. The previous string key allocated
    // on every label, including cache hits, while the GUI was open.
    const MeasureKey key{f.font, size_key(f.size), hash_text(text)};
    if (const auto found = measure_cache_.find(key); found != measure_cache_.end()) return found->second;
    const float width = f.font->CalcTextSizeA(f.size, FLT_MAX, 0.0F, text.data(), text.data() + text.size()).x;
    if (measure_cache_.size() > 8192) measure_cache_.clear();
    measure_cache_.emplace(key, width);
    return width;
}

void FontManager::draw(ImDrawList* list, FontRole role, float x, float baseline_y, ImU32 color, std::string_view text,
                       bool snap_to_pixels) const {
    if (text.empty() || !list) return;
    const FontFace f = face(role);
    if (!f.font) return;
    const ImVec2 origin(snap_to_pixels ? std::round(x) : x,
                        (snap_to_pixels ? std::round(baseline_y) : baseline_y) - f.baseline);
    const int first_vertex = list->VtxBuffer.Size;
    list->AddText(f.font, f.size, origin, color,
                  text.data(), text.data() + text.size());
    if (!snap_to_pixels) {
        // ImFont::RenderText truncates the origin even when AddText receives
        // floats. Restore the residual after glyph generation for smooth motion.
        const ImVec2 residual(origin.x - std::trunc(origin.x), origin.y - std::trunc(origin.y));
        for (int i = first_vertex; i < list->VtxBuffer.Size; ++i) {
            list->VtxBuffer[i].pos.x += residual.x;
            list->VtxBuffer[i].pos.y += residual.y;
        }
    }
}

std::string_view FontManager::ellipsize(FontRole role, std::string_view text, float max_width, std::string& scratch) {
    if (measure(role, text) <= max_width) return text;
    constexpr char dots[] = "\xE2\x80\xA6"; // U+2026
    const FontFace f = face(role);
    if (!f.font) return {};
    const float scale = f.size / f.font->FontSize;
    const float dots_width = f.font->GetCharAdvance(static_cast<ImWchar>(ellipsis)) * scale;
    if (!(dots_width <= max_width)) return {};

    // Match CalcTextSizeA's advances and UTF-8 decoder in one forward pass.
    // The previous suffix search allocated and measured every shortened copy,
    // making a heavily clipped label quadratic and flooding measure_cache_.
    // Keep the longest fitting boundary, including after newline resets.
    float line_width = 0.0F, previous_lines_width = 0.0F;
    std::size_t keep = 0;
    const char* cursor = text.data();
    const char* end = cursor + text.size();
    while (cursor < end) {
        unsigned int code = static_cast<unsigned char>(*cursor);
        if (code < 0x80) ++cursor;
        else cursor += ImTextCharFromUtf8(&code, cursor, end);
        if (code == '\n') {
            previous_lines_width = std::max(previous_lines_width, line_width);
            line_width = 0.0F;
        } else if (code != '\r') {
            const float advance = code < static_cast<unsigned>(f.font->IndexAdvanceX.Size)
                ? f.font->IndexAdvanceX[static_cast<int>(code)] : f.font->FallbackAdvanceX;
            line_width += advance * scale;
        }
        if (std::max(previous_lines_width, line_width + dots_width) <= max_width)
            keep = static_cast<std::size_t>(cursor - text.data());
        if (line_width > max_width || previous_lines_width > max_width) break;
    }
    scratch.assign(text.data(), keep);
    scratch.append(dots);
    return scratch;
}

float FontManager::centered_baseline(FontRole role, float top, float height, bool snap_to_pixels) const {
    const float baseline = top + (height + face(role).cap_height) * 0.5F;
    return snap_to_pixels ? std::round(baseline) : baseline;
}

} // namespace medicon::gui
