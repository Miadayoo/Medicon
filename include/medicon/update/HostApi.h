#pragma once
#include <stdint.h>
#include <wchar.h>

// The C ABI between MediconLoader.dll and MediconCore.dll (docs/auto-update.md).
// Only plain C types cross it: the two DLLs may be built at different times.

#ifdef __cplusplus
extern "C" {
#endif

#define MEDICON_CORE_API_VERSION 1u

// What the loader did before starting this Core.
typedef enum MediconUpdateState {
    MEDICON_UPDATE_UP_TO_DATE = 0,    // the newest published Core was already installed
    MEDICON_UPDATE_UPDATED = 1,       // a newer Core was downloaded and verified this launch
    MEDICON_UPDATE_OFFLINE = 2,       // the server could not be reached; a verified local Core runs
    MEDICON_UPDATE_FAILED = 3,        // the update failed; an older verified Core runs
    MEDICON_UPDATE_FALLBACK = 4       // a newer Core failed to start; an older one runs
} MediconUpdateState;

typedef struct MediconHostAPI {
    uint32_t size;                    // sizeof(MediconHostAPI); newer fields go after the last one
    uint32_t api_version;             // MEDICON_CORE_API_VERSION
    const wchar_t* core_version;      // "1.0.5": the version of this Core, never NULL
    const wchar_t* previous_version;  // the version that ran last, or NULL (first run, or the same)
    const wchar_t* data_directory;    // %LOCALAPPDATA%\Medicon, never NULL
    uint32_t update_state;            // MediconUpdateState
    uint32_t reserved;
} MediconHostAPI;

// Exported by MediconCore.dll. Initialize returns 1 once the client's own
// bootstrap is running and 0 when nothing was started (the loader may then
// FreeLibrary and fall back). A second call returns 1 without starting again.
typedef uint32_t (*MediconGetApiVersionFn)(void);
typedef int (*MediconInitializeFn)(const MediconHostAPI* host);
typedef void (*MediconShutdownFn)(void);

// Exported by MediconLoader.dll for the Core's UI: a snapshot of the update.
typedef struct MediconLoaderStatus {
    uint32_t size;                    // set by the caller: sizeof(MediconLoaderStatus)
    uint32_t phase;                   // medicon::update::Phase
    uint32_t progress_permille;       // download progress, 0..1000
    wchar_t current_version[32];
    wchar_t latest_version[32];
    char last_error[256];             // UTF-8, never a secret
} MediconLoaderStatus;
typedef int (*MediconLoaderGetStatusFn)(MediconLoaderStatus* status);

#ifdef __cplusplus
}
#endif
