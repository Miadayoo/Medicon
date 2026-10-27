#include <medicon/gui/Fonts.hpp>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace medicon::gui;
int checks = 0;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

struct ImGuiScope {
    ImGuiScope() { ImGui::CreateContext(); ImGui::GetIO().IniFilename = nullptr; }
    ~ImGuiScope() { ImGui::DestroyContext(); }
};

float width(FontFace face, std::string_view text) {
    return face.font->CalcTextSizeA(face.size, std::numeric_limits<float>::max(), 0,
                                   text.data(), text.data() + text.size()).x;
}

// Slow reference: render every candidate and choose the longest UTF-8 prefix
// that fits, independently of the production cutoff calculation.
std::string reference(FontFace face, std::string_view text, float available) {
    if (width(face, text) <= available) return std::string(text);
    constexpr char dots[] = "\xE2\x80\xA6";
    for (std::size_t end = text.size(); end > 0; --end) {
        if (end != text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) continue;
        std::string candidate(text.substr(0, end));
        candidate += dots;
        if (width(face, candidate) <= available) return candidate;
    }
    return width(face, dots) <= available ? dots : "";
}

void test_ellipsize() {
    ImGuiScope context;
    FontManager fonts;
    fonts.set_owns_atlas(true);
    const std::vector<std::string> labels{
        "", "Medicon", "Environment Changer", "123.456 [RSHIFT]",
        "\xE8\x87\xAA\xE5\x8B\x95\xE8\xA3\x85\xE5\x82\x99 Extended",
        "\xCE\xA9\xCE\xA9 supplementary \xF0\x9F\x98\x80 glyph",
        "MMMM\ni\nA long final line", "i\nMMMM\nA long final line", "abc\r\ndef\r final line",
        std::string(2048, 'W') + " ending",
    };
    for (const auto& label : labels) fonts.add_text(label);
    FontSizes sizes;
    sizes.splash = 0;
    sizes.arraylist = 17.375F;
    check(fonts.prepare(*ImGui::GetIO().Fonts, sizes), "font atlas prepares");

    for (const auto role : {FontRole::Label, FontRole::ArrayList}) {
        const auto face = fonts.face(role);
        for (const auto& label : labels) {
            std::vector<float> widths{-1, 0, 0.001F, 10, 20, 55, 97, width(face, label),
                                      std::numeric_limits<float>::infinity(),
                                      std::numeric_limits<float>::quiet_NaN()};
            // Exact glyph boundaries and their adjacent floats detect changes
            // to ImGui's accumulation order and <= edge behavior.
            for (std::size_t end = 0; end <= std::min<std::size_t>(label.size(), 48); ++end) {
                if (end != label.size() && (static_cast<unsigned char>(label[end]) & 0xC0) == 0x80) continue;
                const float boundary = width(face, label.substr(0, end) + "\xE2\x80\xA6");
                widths.push_back(boundary);
                widths.push_back(std::nextafter(boundary, 0.0F));
                widths.push_back(std::nextafter(boundary, std::numeric_limits<float>::infinity()));
            }
            for (const float available : widths) {
                std::string scratch;
                const auto actual = fonts.ellipsize(role, label, available, scratch);
                check(actual == reference(face, label, available), "clipped text matches rendered longest fitting prefix");
                if (!actual.empty()) check(width(face, actual) <= available, "clipped text stays inside its field");
            }
        }
    }
}
} // namespace

int main() {
    try {
        test_ellipsize();
        std::cout << "PASS ellipsize, " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "FAIL ellipsize: " << error.what() << '\n';
        return 1;
    }
}
