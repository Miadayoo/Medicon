#pragma once
#include <medicon/platform/ClientApi.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minecraft.Windows.exe starts automatically after LoadLibrary. Other DXGI hosts
// can opt in explicitly. Start returns immediately; inspect Status/LastError.
MEDICON_API int MediconStartOverlay(void);
// 0 stopped, 1 bootstrapping, 2 waiting for a game frame/queue, 3 drawing, 4 failed.
MEDICON_API uint32_t MediconOverlayStatus(void);
MEDICON_API uint32_t MediconOverlayLastError(char* buffer, uint32_t capacity);
// Call outside DllMain and outside a rendering/input callback.
// Hooks are disabled and input/GPU resources released. Direct automatic
// injection releases its module reference only after callbacks and hooks drain;
// explicit/API-hosted loads remain pinned for callback safety.
MEDICON_API int MediconStopOverlay(void);

#ifdef __cplusplus
}
#endif
