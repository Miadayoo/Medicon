#include <medicon/gui/Log.hpp>

#include <Windows.h>

#include <string>
#include <utility>

namespace medicon::gui {
namespace {
std::function<void(std::string_view)>& sink() {
    static std::function<void(std::string_view)> instance;
    return instance;
}
} // namespace

void log_line(std::string_view message) {
    std::string line = "[Medicon GUI] ";
    line.append(message);
    line.push_back('\n');
    OutputDebugStringA(line.c_str());
    if (sink()) sink()(message);
}

void set_log_sink(std::function<void(std::string_view)> callback) { sink() = std::move(callback); }

} // namespace medicon::gui
