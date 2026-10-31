#include <medicon/social/Service.hpp>

#include "SocialApi.hpp"
#include "SocialNet.hpp"
#include "../alt/AltService.hpp"

#include <Windows.h>
#include <ShlObj.h>

#include <chrono>
#include <stdexcept>

namespace medicon::social {

using namespace std::chrono_literals;

struct SocialService::Impl {
    std::filesystem::path directory;
    std::shared_ptr<detail::Core> core;
    std::shared_ptr<detail::SocialNet> net;
    std::shared_ptr<detail::SocialApi> api;
    std::unique_ptr<alt::AltService> alts;
    bool started = false;
    bool stopped = false;
};

SocialService::SocialService(Options options) : impl_(std::make_unique<Impl>()) {
    impl_->directory = std::move(options.directory);
    impl_->alts = std::make_unique<alt::AltService>(alt::AltService::Options{impl_->directory, options.client_version, options.log});
    impl_->core = std::make_shared<detail::Core>(std::move(options.client_version), std::move(options.log));
    impl_->net = std::make_shared<detail::SocialNet>(impl_->core, impl_->directory);
    impl_->api = std::make_shared<detail::SocialApi>(impl_->core);
}

SocialService::~SocialService() { stop(); }

void SocialService::start() {
    if (impl_->started) return;
    impl_->started = true;
    if (!impl_->core->http().valid()) impl_->core->report("Social: WinHTTP could not be initialized.");
    impl_->net->start();
    impl_->api->start();
    impl_->alts->start();
}

void SocialService::request_stop() noexcept {
    impl_->net->request_stop();
    impl_->api->request_stop();
    impl_->alts->request_stop();
}

bool SocialService::alt_worker_finished() const noexcept { return !impl_->started || impl_->alts->finished(); }

bool SocialService::stop() {
    if (!impl_->started || impl_->stopped) return true;
    impl_->stopped = true;
    impl_->api->request_stop();
    impl_->alts->request_stop();
    // All run concurrently against one 2 s budget: the REST and alt workers are usually idle.
    const bool net = impl_->net->stop(2s);
    const bool api = impl_->api->stop(100ms);
    const bool alts = impl_->alts->stop(1s);
    if (!net || !api || !alts) impl_->core->report("Social: a network thread did not stop in time and was detached.");
    return net && api && alts;
}

Channel& SocialService::channel() noexcept { return impl_->core->channel(); }

alt::Channel& SocialService::alt_channel() noexcept { return impl_->alts->channel(); }

void SocialService::set_alt_game_bridge(bool available) { impl_->alts->set_game_bridge(available); }
std::optional<std::string> SocialService::alt_native_authorization(std::string_view url) { return impl_->alts->native_authorization(url); }
void SocialService::alt_native_auth_result(unsigned status) { impl_->alts->native_auth_result(status); }
std::optional<std::string> SocialService::alt_connection_token(std::string_view token) { return impl_->alts->connection_token(token); }
void SocialService::alt_connection_used(std::string_view token) { impl_->alts->connection_used(token); }

std::filesystem::path SocialService::default_directory() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &path)))
        throw std::runtime_error("Cannot locate %APPDATA% for Social settings.");
    std::filesystem::path directory = std::filesystem::path(path) / L"Medicon";
    CoTaskMemFree(path);
    return directory;
}

} // namespace medicon::social
