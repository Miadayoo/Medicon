#include <medicon/gui/KeyNames.hpp>

#include <array>
#include <string>

namespace medicon::gui {
namespace {
std::string make_key_name(int key) {
    if (key == 0) return "None";
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::string(1, static_cast<char>(key));
    if (key >= 0x70 && key <= 0x87) return "F" + std::to_string(key - 0x70 + 1);
    if (key >= 0x60 && key <= 0x69) return "Num " + std::to_string(key - 0x60);
    switch (key) {
    case 0x01: return "LMB";
    case 0x02: return "RMB";
    case 0x04: return "MMB";
    case 0x05: return "Mouse4";
    case 0x06: return "Mouse5";
    case 0x08: return "Backspace";
    case 0x09: return "Tab";
    case 0x0D: return "Enter";
    case 0x10: return "Shift";
    case 0x11: return "Ctrl";
    case 0x12: return "Alt";
    case 0x13: return "Pause";
    case 0x14: return "Caps Lock";
    case 0x1B: return "Esc";
    case 0x20: return "Space";
    case 0x21: return "Page Up";
    case 0x22: return "Page Down";
    case 0x23: return "End";
    case 0x24: return "Home";
    case 0x25: return "Left";
    case 0x26: return "Up";
    case 0x27: return "Right";
    case 0x28: return "Down";
    case 0x2C: return "Print";
    case 0x2D: return "Insert";
    case 0x2E: return "Delete";
    case 0x5B: return "LWin";
    case 0x5C: return "RWin";
    case 0x5D: return "Menu";
    case 0x6A: return "Num *";
    case 0x6B: return "Num +";
    case 0x6D: return "Num -";
    case 0x6E: return "Num .";
    case 0x6F: return "Num /";
    case 0x90: return "Num Lock";
    case 0x91: return "Scroll Lock";
    case 0xA0: return "LShift";
    case 0xA1: return "RShift";
    case 0xA2: return "LCtrl";
    case 0xA3: return "RCtrl";
    case 0xA4: return "LAlt";
    case 0xA5: return "RAlt";
    case 0xBA: return ";";
    case 0xBB: return "=";
    case 0xBC: return ",";
    case 0xBD: return "-";
    case 0xBE: return ".";
    case 0xBF: return "/";
    case 0xC0: return "`";
    case 0xDB: return "[";
    case 0xDC: return "\\";
    case 0xDD: return "]";
    case 0xDE: return "'";
    default: return "Key " + std::to_string(key);
    }
}
} // namespace

std::string_view key_name(int key) {
    static const auto names = [] {
        std::array<std::string, 256> table;
        for (int index = 0; index < 256; ++index) table[static_cast<std::size_t>(index)] = make_key_name(index);
        return table;
    }();
    if (key < 0 || key > 255) return "None";
    return names[static_cast<std::size_t>(key)];
}

} // namespace medicon::gui
