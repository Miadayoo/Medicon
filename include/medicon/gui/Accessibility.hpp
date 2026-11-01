#pragma once
#include <optional>

namespace medicon::gui {
struct AccessibilityPreferences {
    bool reduced_motion=false;
    bool reduced_transparency=false;
    bool high_contrast=false;
};
// Shared by native UI, existing Glass, and scripts on the render thread.
inline thread_local AccessibilityPreferences accessibility_preferences{};
void refresh_accessibility_preferences();
void override_accessibility_preferences(std::optional<AccessibilityPreferences> preferences);
}
