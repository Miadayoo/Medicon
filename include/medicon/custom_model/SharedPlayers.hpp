#pragma once
#include <medicon/custom_model/BundledModel.hpp>
#include <medicon/sdk/Gameplay.hpp>
#include <medicon/social/Events.hpp>
#include <span>

namespace medicon::custom_model {
struct SharedModelBinding {
    std::uintptr_t actor{}; // Identity comparison only; native draw supplies the live actor.
    std::uint64_t identity{},world{};
    unsigned model{};
};
struct SharedPlayerSelection {
    social::SyncModels sync;
    std::vector<SharedModelBinding> bindings;
};
// Copied game/social snapshots only. Never reads an actor pointer or changes the world.
inline SharedPlayerSelection select_shared_players(const sdk::GameplayFrame& frame,BuiltinModel selected,
                                                   std::span<const social::PlayerModel> advertised,
                                                   bool publish_self=true,bool receive_players=true) {
    SharedPlayerSelection result;
    if (!frame.ready || !frame.world_identity) return result;
    const auto identity=[](const sdk::ActorSnapshot& actor) {
        return social::ModelIdentity{std::to_string(actor.unique_id),actor.model_name};
    };
    const auto local=identity(frame.local);
    if (publish_self && social::valid_model_identity(local) && selected<BuiltinModel::count)
        result.sync.self=social::PlayerModel{local,selected==BuiltinModel::chocolat?"chocolat":"neon",0};
    if (!receive_players) return result;
    const auto distance=[&](const sdk::ActorSnapshot* actor) {
        const auto dx=actor->position.x-frame.local.position.x,dz=actor->position.z-frame.local.position.z;
        return dx*dx+dz*dz;
    };
    std::vector<const sdk::ActorSnapshot*> nearby;
    for (const auto& actor:frame.actors)
        if (actor.player && !actor.local && actor.alive && !actor.model_name.empty() && distance(&actor)<=64*64)
            nearby.push_back(&actor);
    std::sort(nearby.begin(),nearby.end(),[&](const auto* a,const auto* b){return distance(a)<distance(b);});
    for (const auto* actor:nearby) {
        if (result.sync.watching.size()>=social::max_model_peers) break;
        const auto player=identity(*actor);
        if (!social::valid_model_identity(player)) continue;
        result.sync.watching.push_back(player);
        const social::PlayerModel* match{};
        bool ambiguous{};
        for (const auto& peer:advertised) if (peer.player==player && social::valid_model_id(peer.model)) {
            if (match) ambiguous=true;
            match=&peer;
        }
        if (match && !ambiguous) result.bindings.push_back({actor->id,static_cast<std::uint64_t>(actor->unique_id),frame.world_identity,
            match->model=="chocolat"?1U:0U});
    }
    return result;
}
}
