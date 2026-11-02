#pragma once

#include <cstdint>
#include <string>

namespace medicon::sdk::custom_model {

enum class ViewMode : unsigned { unknown, first_person, third_person, third_person_front };
[[nodiscard]] constexpr bool third_person(ViewMode view) noexcept {
    return view == ViewMode::third_person || view == ViewMode::third_person_front;
}
[[nodiscard]] const char* view_mode_name(ViewMode view) noexcept;

// Read-only native ABI boundary. Neither getter is called. The profile and
// every borrowed pointer are validated again in the current render callback.
struct ViewProfile { std::uintptr_t client_options_getter{}, option_getter{}; };
// Bootstrap only, after the P0/P1 executable gate, before render hooks enable.
bool initialize_view_reader(std::string& diagnostic);
[[nodiscard]] ViewMode read_client_view(void* client, ViewProfile profile) noexcept;
// Only after the existing 1.26.60.5 executable/profile gate has passed.
[[nodiscard]] ViewMode read_local_view(void* client) noexcept;

} // namespace medicon::sdk::custom_model
