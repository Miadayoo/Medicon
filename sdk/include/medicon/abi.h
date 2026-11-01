#ifndef MEDICON_SCRIPT_ABI_H
#define MEDICON_SCRIPT_ABI_H

/* C-only, fixed-width Windows x64 boundary. Ownership never crosses DLLs.
 * All host calls and plugin callbacks run under the host's frame lock.
 * Pointer arguments are borrowed for the duration of the call only. */
#include <stdint.h>
#include <stddef.h>

#define MEDICON_SCRIPT_ABI_VERSION 1u
#define MEDICON_SCRIPT_API_VERSION 400u
#define MEDICON_SCRIPT_LEGACY_API_VERSION 100u
#define MEDICON_HOST_V1_SIZE 96u
#define MEDICON_PLUGIN_V1_SIZE 64u
#define MEDICON_SCRIPT_MAGIC 0x4D435331u
#define MEDICON_FEATURE_CHAT 1ull
#define MEDICON_FEATURE_RENDER 2ull
#define MEDICON_FEATURE_PLAYER 4ull
#define MEDICON_FEATURE_GLASS (1ull << 3)
#define MEDICON_FEATURE_REFRACTION (1ull << 4)
#define MEDICON_FEATURE_UI (1ull << 5)
#define MEDICON_FEATURE_ANIMATION (1ull << 6)
#define MEDICON_FEATURE_NOTIFICATIONS (1ull << 7)
#define MEDICON_FEATURE_STORAGE (1ull << 8)
#define MEDICON_FEATURE_TIMERS (1ull << 9)
#define MEDICON_FEATURE_THEME (1ull << 10)
#define MEDICON_FEATURE_INPUT (1ull << 11)
#define MEDICON_FEATURE_SOUND (1ull << 12)
#define MEDICON_FEATURE_GLASS_RADIUS (1ull << 13)
#define MEDICON_FEATURE_GLASS_OPACITY (1ull << 14)
#define MEDICON_EVENT_TICK 1u
#define MEDICON_EVENT_RENDER 2u
#define MEDICON_EVENT_KEY 3u
#define MEDICON_EVENT_GAME_JOINED 4u
#define MEDICON_EVENT_GAME_LEFT 5u
#define MEDICON_EVENT_SCREEN_RESIZED 6u
#define MEDICON_FRAME_UI_VISIBLE 1u
#define MEDICON_FRAME_REDUCED_MOTION 2u
#define MEDICON_FRAME_REDUCED_TRANSPARENCY 4u
#define MEDICON_FRAME_HIGH_CONTRAST 8u
#define MEDICON_GLASS_RADIUS 1u
#define MEDICON_GLASS_BLUR 2u
#define MEDICON_GLASS_REFRACTION 4u
#define MEDICON_GLASS_TINT 8u
#define MEDICON_GLASS_SATURATION 16u
#define MEDICON_GLASS_HIGHLIGHT 32u
#define MEDICON_GLASS_BORDER 64u
#define MEDICON_GLASS_SHADOW 128u
#define MEDICON_GLASS_OPACITY 256u
#define MEDICON_GLASS_QUALITY 512u
#define MEDICON_RESULT_OK 1
#define MEDICON_RESULT_FALLBACK 2
#define MEDICON_RESULT_UNSUPPORTED -1
#define MEDICON_RESULT_INVALID -2
#define MEDICON_RESULT_QUOTA -3

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
typedef struct MediconRect { float x, y, width, height; } MediconRect;
typedef struct MediconCallSite { MediconString file; uint32_t line, column; } MediconCallSite;
typedef struct MediconFrameInfo {
    uint32_t size, flags;
    float width, height, scale, delta_seconds;
    double now_seconds;
    uint64_t generation;
} MediconFrameInfo;
typedef struct MediconGlassStyle {
    uint32_t size, version, preset, fields;
    float radius, blur, refraction, saturation, highlight, border, shadow, opacity;
    MediconColor tint;
    uint32_t quality, reserved;
} MediconGlassStyle;
typedef struct MediconDrawCommand {
    uint32_t size, kind; /* 1 fill rounded rect, 2 stroke, 3 line */
    MediconRect rect;
    MediconPoint end;
    MediconColor color;
    float radius, thickness;
} MediconDrawCommand;
typedef struct MediconWidget {
    uint32_t size, kind; /* 1 label, 2 button, 3 progress */
    MediconString id, text;
    float value;
} MediconWidget;
typedef struct MediconWorldEvent { uint64_t world_identity; } MediconWorldEvent;
typedef struct MediconScreenEvent { float width, height, scale; } MediconScreenEvent;
typedef int32_t (MEDICON_SCRIPT_CALL *MediconTimerCallback)(void*);
typedef void (MEDICON_SCRIPT_CALL *MediconTimerDestroy)(void*);

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
    /* ABI 1 append-only extension. Never read past size. Old DLLs see size=96. */
    int32_t (MEDICON_SCRIPT_CALL *frame_info)(void*, MediconFrameInfo*);
    int32_t (MEDICON_SCRIPT_CALL *draw_shape)(void*, const MediconDrawCommand*);
    int32_t (MEDICON_SCRIPT_CALL *glass)(void*, const MediconRect*, const MediconGlassStyle*, const MediconCallSite*);
    uint32_t (MEDICON_SCRIPT_CALL *ui_begin)(void*, MediconString, const MediconRect*, const MediconGlassStyle*, const MediconCallSite*);
    void (MEDICON_SCRIPT_CALL *ui_end)(void*, uint32_t);
    int32_t (MEDICON_SCRIPT_CALL *ui_widget)(void*, uint32_t, const MediconWidget*);
    int32_t (MEDICON_SCRIPT_CALL *theme_color)(void*, uint32_t, MediconColor*);
    int32_t (MEDICON_SCRIPT_CALL *input)(void*, int32_t key, MediconPoint* cursor);
    int32_t (MEDICON_SCRIPT_CALL *notify)(void*, MediconString, MediconString, uint32_t);
    int32_t (MEDICON_SCRIPT_CALL *play_sound)(void*, uint32_t);
    int32_t (MEDICON_SCRIPT_CALL *storage_get)(void*, MediconString, char*, uint32_t);
    int32_t (MEDICON_SCRIPT_CALL *storage_set)(void*, MediconString, MediconString);
    uint64_t (MEDICON_SCRIPT_CALL *timer_add)(void*, double, double, MediconTimerCallback, MediconTimerDestroy, void*);
    void (MEDICON_SCRIPT_CALL *timer_cancel)(void*, uint64_t);
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
    int32_t (MEDICON_SCRIPT_CALL *ui)(void*);
} MediconPluginApi;
typedef int32_t (MEDICON_SCRIPT_CALL *MediconScriptEntry)(uint32_t host_abi, MediconPluginApi*);

#ifdef __cplusplus
}
static_assert(sizeof(MediconScriptManifest) == 192, "Unexpected script manifest layout");
static_assert(offsetof(MediconHostApi, frame_info) == MEDICON_HOST_V1_SIZE, "Host v1 prefix changed");
static_assert(offsetof(MediconPluginApi, ui) == MEDICON_PLUGIN_V1_SIZE, "Plugin v1 prefix changed");
#endif
#endif
