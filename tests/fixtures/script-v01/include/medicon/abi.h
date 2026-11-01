#ifndef MEDICON_SCRIPT_ABI_H
#define MEDICON_SCRIPT_ABI_H

/* C-only, fixed-width Windows x64 boundary. Ownership never crosses DLLs.
 * All host calls and plugin callbacks run under the host's frame lock.
 * Pointer arguments are borrowed for the duration of the call only. */
#include <stdint.h>
#include <stddef.h>

#define MEDICON_SCRIPT_ABI_VERSION 1u
#define MEDICON_SCRIPT_API_VERSION 100u
#define MEDICON_SCRIPT_MAGIC 0x4D435331u
#define MEDICON_FEATURE_CHAT 1ull
#define MEDICON_FEATURE_RENDER 2ull
#define MEDICON_FEATURE_PLAYER 4ull
#define MEDICON_EVENT_TICK 1u
#define MEDICON_EVENT_RENDER 2u
#define MEDICON_EVENT_KEY 3u

#if defined(_WIN32)
#define MEDICON_SCRIPT_EXPORT __declspec(dllexport)
#define MEDICON_SCRIPT_CALL __cdecl
#else
#define MEDICON_SCRIPT_EXPORT __attribute__((visibility("default")))
#define MEDICON_SCRIPT_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MediconString { const char* data; uint32_t size; } MediconString;
typedef struct MediconPoint { float x, y; } MediconPoint;
typedef struct MediconColor { float r, g, b, a; } MediconColor;
typedef struct MediconPlayerSnapshot {
    float x, y, z, velocity_x, velocity_y, velocity_z, health, yaw, pitch;
    uint32_t alive;
} MediconPlayerSnapshot;
enum MediconValueType { MEDICON_BOOL = 1, MEDICON_INT = 2, MEDICON_FLOAT = 3, MEDICON_COLOR = 4, MEDICON_STRING = 5 };
typedef struct MediconValue {
    uint32_t type;
    int32_t integer;
    float number;
    MediconColor color;
    MediconString text;
} MediconValue;
typedef struct MediconSettingDescriptor {
    MediconString id, label;
    MediconValue initial;
    float minimum, maximum, step;
} MediconSettingDescriptor;
typedef struct MediconTickEvent { float delta_seconds; } MediconTickEvent;
typedef struct MediconScriptKeyEvent { int32_t key; uint32_t down, repeat; } MediconScriptKeyEvent;

/* Exported data, read directly from the PE file BEFORE LoadLibrary.
 * No pointers, relocations, constructors, or compiler-specific objects. */
typedef struct MediconScriptManifest {
    uint32_t magic, size, abi_version, api_version;
    char id[64];
    char name[96];
    uint32_t category, reserved;
    uint64_t required_features;
} MediconScriptManifest;

typedef struct MediconHostApi {
    uint32_t size, abi_version;
    uint64_t features;
    void* context;
    void (MEDICON_SCRIPT_CALL *log)(void*, uint32_t severity, MediconString);
    int32_t (MEDICON_SCRIPT_CALL *chat)(void*, MediconString);
    int32_t (MEDICON_SCRIPT_CALL *define_setting)(void*, const MediconSettingDescriptor*);
    int32_t (MEDICON_SCRIPT_CALL *read_setting)(void*, MediconString, MediconValue*);
    int32_t (MEDICON_SCRIPT_CALL *setting_range)(void*, MediconString, float, float, float);
    int32_t (MEDICON_SCRIPT_CALL *local_player)(void*, MediconPlayerSnapshot*);
    MediconPoint (MEDICON_SCRIPT_CALL *render_size)(void*);
    void (MEDICON_SCRIPT_CALL *fill_circle)(void*, MediconPoint, float, MediconColor);
    void (MEDICON_SCRIPT_CALL *text)(void*, MediconPoint, MediconString, MediconColor, float);
} MediconHostApi;

typedef struct MediconPluginApi {
    uint32_t size, abi_version;
    void* (MEDICON_SCRIPT_CALL *create)(const MediconHostApi*);
    void (MEDICON_SCRIPT_CALL *destroy)(void*);
    int32_t (MEDICON_SCRIPT_CALL *enable)(void*);
    int32_t (MEDICON_SCRIPT_CALL *disable)(void*);
    int32_t (MEDICON_SCRIPT_CALL *tick)(void*, float);
    int32_t (MEDICON_SCRIPT_CALL *render)(void*);
    int32_t (MEDICON_SCRIPT_CALL *event)(void*, uint32_t, const void*);
} MediconPluginApi;
typedef int32_t (MEDICON_SCRIPT_CALL *MediconScriptEntry)(uint32_t host_abi, MediconPluginApi*);

#ifdef __cplusplus
}
static_assert(sizeof(MediconScriptManifest) == 192, "Unexpected script manifest layout");
#endif
#endif
