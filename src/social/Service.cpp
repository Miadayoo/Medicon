#include <medicon/social/Service.hpp>

#include "SocialApi.hpp"
#include "SocialNet.hpp"

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
    bool started = false;
    bool stopped = false;
};

SocialService::SocialService(Options options) : impl_(std::make_unique<Impl>()) {
    impl_->directory = std::move(options.directory);
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
}

void SocialService::request_stop() noexcept {
    impl_->net->request_stop();
    impl_->api->request_stop();
}

bool SocialService::stop() {
    if (!impl_->started || impl_->stopped) return true;
    impl_->stopped = true;
    impl_->api->request_stop();
    // Both run concurrently against one 2 s budget: the REST worker is usually idle.
    const bool net = impl_->net->stop(2s);
    const bool api = impl_->api->stop(100ms);
    if (!net || !api) impl_->core->report("Social: a network thread did not stop in time and was detached.");
    return net && api;
}

Channel& SocialService::channel() noexcept { return impl_->core->channel(); }

std::filesystem::path SocialService::default_directory() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &path)))
        throw std::runtime_error("Cannot locate %APPDATA% for Social settings.");
    std::filesystem::path directory = std::filesystem::path(path) / L"Medicon";
    CoTaskMemFree(path);
    return directory;
}

} // namespace medicon::social
