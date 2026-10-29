// Linked only into MediconOverlayTest. Keep the production overlay, native
// hooks, ImGui and graphics backends; replace networking/account state alone.
#include <medicon/social/Service.hpp>

namespace medicon::social {

struct SocialService::Impl { Channel channel; };

SocialService::SocialService(Options) : impl_(std::make_unique<Impl>()) {}
SocialService::~SocialService() = default;
void SocialService::start() {
    SettingsLoaded settings;
    settings.music_enabled = false;
    impl_->channel.events.push(settings);
    impl_->channel.events.push(StateChanged{ConnectionState::Reconnecting, {}});
}
void SocialService::request_stop() noexcept {}
bool SocialService::stop() { return true; }
Channel& SocialService::channel() noexcept { return impl_->channel; }
std::filesystem::path SocialService::default_directory() { return std::filesystem::temp_directory_path(); }

} // namespace medicon::social
