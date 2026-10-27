#pragma once

// Included by imgui.h through IMGUI_USER_CONFIG in every target that builds or
// uses Medicon's ImGui. One current context per thread: the Social font atlas is
// built on a worker thread, and ImGui's allocator records statistics into the
// current context, which must never be the render thread's context there.
struct ImGuiContext;
extern thread_local ImGuiContext* MediconImGuiContext;
#define GImGui MediconImGuiContext
