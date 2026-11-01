#pragma once

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace medicon {

// Declaration order is the display order of category panels.
enum class Category { Visual, Movement, Player, Misc, Client, Scripts };
inline constexpr std::size_t category_count = 6;

struct Color {
    float r = 1.0F;
    float g = 1.0F;
    float b = 1.0F;
    float a = 1.0F;
    bool operator==(const Color&) const = default;
};

struct Choice {
    int index = 0;
    std::vector<std::string> options;
    bool operator==(const Choice&) const = default;
};

// IDs are stable persistence keys. Names and descriptions may change freely.
// Numeric bounds drive both the GUI controls and config validation.
struct Setting {
    using Value = std::variant<bool, int, float, Choice, Color, std::string>;

    std::string id;
    std::string name;
    std::string description;
    Value value;
    float minimum = 0.0F;
    float maximum = 1.0F;
    float step = 1.0F;
    bool visible = true;

    template <class T> T& get() { return std::get<T>(value); }
    template <class T> const T& get() const { return std::get<T>(value); }
};

const char* category_name(Category category) noexcept;
// Stable lower-case identifier used in persistence keys.
const char* category_id(Category category) noexcept;

} // namespace medicon
