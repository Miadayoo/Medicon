#include <imgui.h>

// Storage for GImGui (see medicon/gui/ImGuiConfig.hpp).
thread_local ImGuiContext* MediconImGuiContext = nullptr;
