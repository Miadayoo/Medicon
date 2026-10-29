#pragma once
#include <string>
#include <string_view>

namespace medicon::custom_model {
// Native nametags can contain Bedrock formatting and a second title line.
inline std::string model_player_name(std::string_view tag) {
    std::string name;
    for (std::size_t i = 0; i < tag.size(); ++i) {
        const auto c = static_cast<unsigned char>(tag[i]);
        if (c == '\n' || c == '\r') break;
        if (c == 0xC2 && i + 2 < tag.size() && static_cast<unsigned char>(tag[i + 1]) == 0xA7) { i += 2; continue; }
        if (c < 32 || c == 127) return {};
        name += tag[i];
        if (name.size() > 128) return {};
    }
    const auto first = name.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return name.substr(first, name.find_last_not_of(' ') - first + 1);
}
}
