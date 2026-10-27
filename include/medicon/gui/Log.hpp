#pragma once

#include <functional>
#include <string_view>

namespace medicon::gui {

// Diagnostics go to the debugger output and to an optional host sink (the
// Preview prints them to stdout).
void log_line(std::string_view message);
void set_log_sink(std::function<void(std::string_view)> sink);

} // namespace medicon::gui
