#pragma once
#include <cstdint>

namespace medicon::sdk::detail {

// Native retry's third argument is NonOwnerPointer<SceneStack>, distinct from
// its first argument, DisconnectionRequestHandler*. Both are 24-byte owners at
// the native ABI, so checking just size/validity cannot distinguish them.
struct SceneStackOwner { std::uintptr_t validity{}, control{}, object{}; };
static_assert(sizeof(SceneStackOwner)==24);
struct ReconnectCalls {
    using GetSceneStack = void*(*)(void*,SceneStackOwner*);
    using ReleaseSceneStack = void(*)(SceneStackOwner*);
    using Retry = void(*)(void*,void*,SceneStackOwner*);
    GetSceneStack get_scene_stack{};
    ReleaseSceneStack release_scene_stack{};
    Retry retry{};
};
bool dispatch_reconnect(const ReconnectCalls&,void* client,void* handler,void* factory,
                        std::uintptr_t stack,std::uintptr_t stack_table,std::uintptr_t push_scene);
}
