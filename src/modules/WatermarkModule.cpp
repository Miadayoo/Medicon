#include "medicon/modules/WatermarkModule.hpp"

namespace medicon {

WatermarkModule::WatermarkModule()
    : Module(module_id, "Watermark", "Medicon tag at the top left while playing.", Category::Client),
      mode(add_setting({"mode", "Mode", "medicon is the glass tag; めじこん is the glowing title of the inject animation.",
                        Choice{0, {"medicon", "めじこん"}}})),
      show_version(add_setting({"version", "Show Version", "Client version after the name.", true})),
      show_fps(add_setting({"fps", "Show FPS", "Frames per second, updated twice a second.", false})),
      show_ping(add_setting({"ping", "Show Ping", "Server latency, when the client can read it.", false})),
      shadow(add_setting({"shadow", "Shadow", "Soft blue shadow under the plate.", true})),
      glow(add_setting({"glow", "Glow", "Soft light around the plate and its accent line, or around the Japanese letters.", true})),
      scale(add_setting({"scale", "Scale", "Multiplier on the HUD scale.", 1.0F, 0.75F, 1.5F, 0.05F})),
      opacity(add_setting({"opacity", "Opacity", "", 1.0F, 0.5F, 1.0F, 0.05F})) {
    set_show_in_arraylist(false);
    set_enabled(true);
}

bool WatermarkModule::setting_visible(const Setting& setting) const noexcept {
    if (!setting.visible) return false;
    return &setting != &shadow || !japanese();
}

bool WatermarkModule::japanese() const noexcept {
    const auto* selection = std::get_if<Choice>(&mode.value);
    return selection && selection->index == 1;
}

} // namespace medicon
