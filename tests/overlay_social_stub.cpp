// Linked only into MediconOverlayTest. Keep the production overlay, native
// hooks, ImGui and graphics backends; replace networking/account state alone.
#include <medicon/social/Service.hpp>

namespace medicon::social {

struct SocialService::Impl { Channel channel; alt::Channel alts; };

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
alt::Channel& SocialService::alt_channel() noexcept { return impl_->alts; }
bool SocialService::alt_worker_finished() const noexcept { return true; }
void SocialService::set_alt_game_bridge(bool) {}
std::optional<std::string> SocialService::alt_native_authorization(std::string_view) { return std::nullopt; }
void SocialService::alt_native_auth_result(unsigned) {}
std::optional<std::string> SocialService::alt_connection_token(std::string_view) { return std::nullopt; }
void SocialService::alt_connection_used(std::string_view) {}
std::filesystem::path SocialService::default_directory() { return std::filesystem::temp_directory_path(); }

} // namespace medicon::social
