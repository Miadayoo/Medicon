#pragma once
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

#if defined(_WIN32) && defined(MEDICON_EXPORTS)
#define MEDICON_API __declspec(dllexport)
#elif defined(_WIN32)
#define MEDICON_API __declspec(dllimport)
#else
#define MEDICON_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

MEDICON_API uint32_t MediconApiVersion(void);
MEDICON_API uint32_t MediconImGuiVersion(void);
typedef void* (*MediconAllocFn)(size_t size, void* user_data);
typedef void (*MediconFreeFn)(void* pointer, void* user_data);
// Pass the host's ImGui::GetAllocatorFunctions results before the first frame.
// This keeps ImGui allocations compatible across static CRT/DLL boundaries.
MEDICON_API int MediconSetImGuiAllocators(MediconAllocFn allocate, MediconFreeFn release, void* user_data);
// Initialize explicitly after LoadLibrary has returned, outside the loader lock.
// Passing NULL selects %LOCALAPPDATA%/Medicon/config.json.
MEDICON_API int MediconInitialize(const wchar_t* config_path);
// Host owns ImGui 1.91.9b context, renderer, NewFrame/Render and input backend.
// Call between ImGui::NewFrame() and ImGui::Render() on a single UI thread.
MEDICON_API int MediconFrame(void* imgui_context, float delta_seconds);
// Call after the platform backend's NewFrame (DisplaySize set) and before
// ImGui::NewFrame(). Adds the GUI fonts (Segoe UI and bundled Geist at the current
// requested pixel sizes) to the host font atlas. Returns 0 on error, 1 when nothing changed,
// 2 when the atlas was rebuilt and the host must re-create its font texture
// (e.g. ImGui_ImplDX11_InvalidateDeviceObjects + CreateDeviceObjects) before
// ImGui::NewFrame(). This is required to prepare the native-size Edge-lit fonts.
MEDICON_API int MediconPrepareFrame(void* imgui_context);
// D3D11 hosts: after MediconFrame and before rendering the host ImGui draw data,
// draw Edge-lit onto the completed scene. The objects are ID3D11Device*,
// ID3D11DeviceContext*, ID3D11Texture2D*, ID3D11RenderTargetView* respectively.
// The call preserves host pipeline state and does not retain the back buffer.
// Automatic Minecraft overlays and MediconPreview already perform this pass.
MEDICON_API int MediconRenderEdgeLitD3D11(void* imgui_context, void* device,
    void* immediate_context, void* back_buffer, void* render_target);
// Call Frame, KeyEvent and Shutdown on the same UI thread; lifecycle hooks run
// synchronously. pressed/repeat: 0 or 1. Mouse buttons 4/5 may be sent as
// VK_XBUTTON1/VK_XBUTTON2 so they can be used as module keybinds.
// Returns 1 when the GUI owns the key (it is open, or Esc closed it): do not
// forward it to the game.
MEDICON_API int MediconKeyEvent(int key, int pressed, int repeat);
// Execute .bind/.unbind on the host UI thread; 1 on success, 0 on error.
// Changed binds persist immediately. reply receives a bounded UTF-8 diagnostic.
MEDICON_API int MediconCommand(const char* input, char* reply, uint32_t capacity);
// Call on focus loss to clear hotkey edge state.
MEDICON_API int MediconResetKeys(void);
// Stop callbacks, wait for them to finish, call Shutdown, then FreeLibrary.
MEDICON_API int MediconShutdown(void);
// Copies UTF-8 diagnostics; required size includes NUL. buffer may be NULL.
MEDICON_API uint32_t MediconLastError(char* buffer, uint32_t capacity);

#ifdef __cplusplus
}
#endif

