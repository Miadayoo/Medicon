#pragma once
#include <medicon/custom_model/Animation.hpp>
namespace medicon::sdk::custom_model {
bool initialize_animation_reader(std::string& diagnostic);
// Call after native begin + one native seed vertex reserved these buffers.
// Layout and bounds writes come from 1459CA7A0's bounded body.
bool fill_animated_tessellator(void* tess,std::span<const medicon::custom_model::SkinOutput> skin,
    const medicon::custom_model::AnimationBatch& batch,bool hurt) noexcept;
// Caller has already validated a freshly obtained local actor in WorldScope.
// Read-only 1.26.52.3 EnTT components; no native pointer escapes the callback.
[[nodiscard]] medicon::custom_model::AnimationInput read_animation_input(void* actor,float alpha,float body_yaw) noexcept;
}
