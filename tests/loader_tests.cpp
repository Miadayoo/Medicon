#include <medicon/update/Loader.hpp>

#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace medicon::update;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr const char* host = "medicon.example.com";

std::string manifest_for(const std::string& version, const std::string& sha, const std::string& mcbe = "1.26.52") {
    return R"({"version":")" + version + R"(","channel":"stable","mcbe_version":")" + mcbe +
           R"(","loader_api":1,"download_url":"https://medicon.example.com/medicon/releases/)" + version +
           R"(/MediconCore.dll","sha256":")" + sha + R"(","size":8,"changelog":["+ x"]})";
}

// A signature is "sig:" + the manifest bytes; the fake verifier checks that.
struct FakeVerifier : Verifier {
    bool verify_manifest(std::string_view manifest, std::string_view signature) override {
        return signature == "sig:" + std::string(manifest);
    }
    std::string sha256_hex(std::string_view data) override {
        // 8-byte Core bodies in these tests: map the body to a stable 64-hex string.
        std::string hex(64, '0');
        for (std::size_t i = 0; i < data.size() && i < 32; ++i) {
            hex[i * 2] = "0123456789abcdef"[(data[i] >> 4) & 0xF];
            hex[i * 2 + 1] = "0123456789abcdef"[data[i] & 0xF];
        }
        return hex;
    }
};

struct Server {
    std::map<std::string, FetchResult> routes;
    bool reachable = true;
    void publish(FakeVerifier& v, const std::string& version, const std::string& body, const std::string& mcbe = "1.26.52") {
        const auto manifest = manifest_for(version, v.sha256_hex(body), mcbe);
        routes["/medicon/manifest.json"] = {true, 200, manifest, ""};
        routes["/medicon/manifest.sig"] = {true, 200, "sig:" + manifest, ""};
        routes["/medicon/releases/" + version + "/MediconCore.dll"] = {true, 200, body, ""};
    }
};

struct FakeFetcher : Fetcher {
    Server& server;
    int downloads = 0;
    explicit FakeFetcher(Server& s) : server(s) {}
    FetchResult get(std::string_view path, std::uint64_t) override {
        if (!server.reachable) return {false, 0, "", "offline"};
        const auto found = server.routes.find(std::string(path));
        if (std::string(path).find("/MediconCore.dll") != std::string::npos) ++downloads;
        if (found == server.routes.end()) return {true, 404, "", ""};
        return found->second;
    }
};

struct FakeStore : FileStore {
    FakeVerifier& verifier;
    std::map<std::string, std::string> cores;      // version -> body
    std::map<std::string, std::string> manifests;  // version -> manifest
    LoaderState state;
    std::vector<std::string> logs;
    int installs = 0;
    explicit FakeStore(FakeVerifier& v) : verifier(v) {}

    void seed(const std::string& version, const std::string& body, const std::string& mcbe = "1.26.52") {
        const auto manifest = manifest_for(version, verifier.sha256_hex(body), mcbe);
        cores[version] = body;
        manifests[version] = manifest;
    }
    std::vector<InstalledCore> installed() override {
        std::vector<InstalledCore> result;
        for (const auto& [version, manifest] : manifests) result.push_back({version, manifest, "sig:" + manifest});
        return result;
    }
    std::string core_path(const std::string& version) override { return "/cores/" + version + "/MediconCore.dll"; }
    std::string core_sha256(const std::string& version) override {
        const auto found = cores.find(version);
        return found == cores.end() ? std::string{} : verifier.sha256_hex(found->second);
    }
    bool install(const std::string& version, std::string_view dll, std::string_view manifest, std::string_view) override {
        ++installs;
        cores[version] = std::string(dll);
        manifests[version] = std::string(manifest);
        return true;
    }
    void remove(const std::string& version) override {
        cores.erase(version);
        manifests.erase(version);
    }
    LoaderState load_state() override { return state; }
    void save_state(const LoaderState& next) override { state = next; }
    void log(std::string_view line) override { logs.push_back(std::string(line)); }
};

struct FakeLauncher : CoreLauncher {
    std::vector<std::string> fail;        // versions whose Core refuses to start
    std::vector<std::string> unloadable;  // versions Windows cannot map on this PC
    std::vector<std::pair<std::string, State>> launched;
    LaunchResult launch(const std::string& version, const std::string&, State state, const std::string&,
                        std::string& error) override {
        launched.emplace_back(version, state);
        if (std::find(unloadable.begin(), unloadable.end(), version) != unloadable.end()) {
            error = "Windows error 126 loading the file";
            return LaunchResult::LoadFailed;
        }
        if (std::find(fail.begin(), fail.end(), version) != fail.end()) {
            error = "Medicon_Initialize failed";
            return LaunchResult::Rejected;
        }
        return LaunchResult::Started;
    }
};

UpdateConfig config() { return {host, "1.26.52.3"}; }

void test_first_run_downloads_and_runs() {
    FakeVerifier v;
    Server server;
    server.publish(v, "1.0.5", "CORE0005");
    FakeFetcher fetcher(server);
    FakeStore store(v);
    FakeLauncher launcher;
    const auto outcome = run_update(fetcher, v, store, launcher, config(), nullptr);
    check(outcome.state == State::Updated && outcome.started_version == "1.0.5", "first run installs and starts the latest");
    check(store.installs == 1 && launcher.launched.size() == 1 && launcher.launched[0].second == State::Updated,
          "the Core is told it was updated");
    check(store.state.last_run == "1.0.5" && store.state.good == std::vector<std::string>{"1.0.5"}, "state records the run");
}

void test_up_to_date_does_not_download() {
    FakeVerifier v;
    Server server;
    server.publish(v, "1.0.5", "CORE0005");
    FakeFetcher fetcher(server);
    FakeStore store(v);
    store.seed("1.0.5", "CORE0005");
    store.state.good = {"1.0.5"};
    store.state.last_run = "1.0.5";
    FakeLauncher launcher;
    const auto outcome = run_update(fetcher, v, store, launcher, config(), nullptr);
    check(outcome.state == State::UpToDate && fetcher.downloads == 0 && store.installs == 0,
          "the newest Core already present is not downloaded again");
    check(launcher.launched.size() == 1 && launcher.launched[0].second == State::UpToDate, "started up to date");
}

void test_offline_runs_verified_local() {
    FakeVerifier v;
    Server server;
    server.reachable = false;
    FakeFetcher fetcher(server);
    FakeStore store(v);
    store.seed("1.0.4", "CORE0004");
    store.state.good = {"1.0.4"};
    FakeLauncher launcher;
    const auto outcome = run_update(fetcher, v, store, launcher, config(), nullptr);
    check(outcome.state == State::Offline && outcome.started_version == "1.0.4", "offline starts the local Core");
    check(launcher.launched.size() == 1 && launcher.launched[0].second == State::Offline, "told it is offline");
}

void test_nested_revision_update_and_fallback() {
    FakeVerifier v;
    Server server;
    server.publish(v, "1.0.3", "CORE0003");
    FakeFetcher fetcher(server);
    FakeStore store(v);
    store.seed("1.0.27", "CORE0027");
    store.state.good = {"1.0.27"};
    store.state.last_run = "1.0.27";
    FakeLauncher launcher;
    const auto outcome = run_update(fetcher, v, store, launcher, config(), nullptr);
    check(outcome.state == State::Updated && outcome.started_version == "1.0.3" && fetcher.downloads == 1,
          "1.0.27 updates to 1.0.3 instead of continuing to run 1.0.27");
    check(launcher.launched.size() == 1 && launcher.launched.front().first == "1.0.3",
          "the downloaded nested successor is launched first");
    FakeFetcher again(server);
    FakeLauncher cached;
    check(run_update(again, v, store, cached, config(), nullptr).started_version == "1.0.3" && again.downloads == 0,
          "next startup reuses 1.0.3 instead of selecting 1.0.27");
    server.reachable = false;
    FakeFetcher offline(server);
    FakeLauncher offline_launcher;
    check(run_update(offline, v, store, offline_launcher, config(), nullptr).started_version == "1.0.3",
          "offline startup also prefers 1.0.3");
    FakeFetcher fallback_fetcher(server);
    FakeLauncher fallback_launcher;
    fallback_launcher.fail = {"1.0.3"};
    const auto fallback = run_update(fallback_fetcher, v, store, fallback_launcher, config(), nullptr);
    check(fallback.started_version == "1.0.27" && fallback_launcher.launched.size() == 2 &&
          fallback_launcher.launched[0].first == "1.0.3" && fallback_launcher.launched[1].first == "1.0.27",
          "failed 1.0.3 falls back to the last revision in the 1.0.2 line");
}

void test_bad_signature_is_not_run() {
    FakeVerifier v;
    Server server;
    server.publish(v, "1.0.6", "CORE0006");
    server.routes["/medicon/manifest.sig"].body = "sig:tampered";  // no longer matches the manifest
    FakeFetcher fetcher(server);
    FakeStore store(v);
    store.seed("1.0.4", "CORE0004");
    store.state.good = {"1.0.4"};
    FakeLauncher launcher;
    const auto outcome = run_update(fetcher, v, store, launcher, config(), nullptr);
    check(fetcher.downloads == 0 && store.installs == 0, "a bad signature stops the download");
    check(outcome.started_version == "1.0.4", "the verified local Core still runs");
}

void test_sha_mismatch_is_discarded() {
    FakeVerifier v;
    Server server;
    const auto manifest = manifest_for("1.0.6", std::string(64, 'f'));  // a digest the body will not match
    server.routes["/medicon/manifest.json"] = {true, 200, manifest, ""};
    server.routes["/medicon/manifest.sig"] = {true, 200, "sig:" + manifest, ""};
    server.routes["/medicon/releases/1.0.6/MediconCore.dll"] = {true, 200, "CORE0006", ""};
    FakeFetcher fetcher(server);
    FakeStore store(v);
    store.seed("1.0.4", "CORE0004");
    store.state.good = {"1.0.4"};
    FakeLauncher launcher;
    const auto outcome = run_update(fetcher, v, store, launcher, config(), nullptr);
    check(store.installs == 0 && outcome.started_version == "1.0.4", "a hash mismatch is not installed");
}

void test_interrupted_download_keeps_old() {
    FakeVerifier v;
    Server server;
    const auto manifest = manifest_for("1.0.6", v.sha256_hex("CORE0006"));
    server.routes["/medicon/manifest.json"] = {true, 200, manifest, ""};
    server.routes["/medicon/manifest.sig"] = {true, 200, "sig:" + manifest, ""};
    server.routes["/medicon/releases/1.0.6/MediconCore.dll"] = {true, 200, "CORE", ""};  // short of size 8
    FakeFetcher fetcher(server);
    FakeStore store(v);
    store.seed("1.0.5", "CORE0005");
    store.state.good = {"1.0.5"};
    FakeLauncher launcher;
    const auto outcome = run_update(fetcher, v, store, launcher, config(), nullptr);
    check(store.installs == 0 && outcome.started_version == "1.0.5", "a truncated download is rejected");
}

void test_launch_failure_falls_back() {
    FakeVerifier v;
    Server server;
    server.publish(v, "1.0.7", "CORE0007");
    FakeFetcher fetcher(server);
    FakeStore store(v);
    store.seed("1.0.6", "CORE0006");
    store.state.good = {"1.0.6"};
    store.state.last_run = "1.0.6";
    FakeLauncher launcher;
    launcher.fail = {"1.0.7"};  // the freshly downloaded Core will not start
    const auto outcome = run_update(fetcher, v, store, launcher, config(), nullptr);
    check(store.installs == 1, "the new Core is still downloaded");
    check(launcher.launched.size() == 2 && launcher.launched[0].first == "1.0.7" && launcher.launched[1].first == "1.0.6",
          "it tries the new Core, then the old one");
    check(outcome.state == State::Fallback && outcome.started_version == "1.0.6", "fallback to the old Core");
    check(std::find(store.state.bad.begin(), store.state.bad.end(), "1.0.7") != store.state.bad.end(),
          "the broken Core is remembered as bad");
    FakeFetcher again(server);
    FakeLauncher relaunch;
    relaunch.fail = {"1.0.7"};
    run_update(again, v, store, relaunch, config(), nullptr);
    check(again.downloads == 0 && relaunch.launched.size() == 1 && relaunch.launched[0].first == "1.0.6",
          "a Core known to be bad is not downloaded or tried again");
}

void test_load_failure_is_retried() {
    FakeVerifier v;
    Server server;
    server.publish(v, "1.0.7", "CORE0007");
    FakeFetcher fetcher(server);
    FakeStore store(v);
    FakeLauncher launcher;
    launcher.unloadable = {"1.0.7"};  // e.g. the old non-UTF-8 path or an antivirus lock
    const auto outcome = run_update(fetcher, v, store, launcher, config(), nullptr);
    check(outcome.started_version.empty() && store.state.bad.empty(), "a file Windows cannot load is not blamed on the release");
    check(outcome.failure.find("1.0.7") != std::string::npos && outcome.failure.find("126") != std::string::npos,
          "the player is told which Core failed and why");
    FakeFetcher again(server);
    FakeLauncher relaunch;
    const auto retry = run_update(again, v, store, relaunch, config(), nullptr);
    check(again.downloads == 0 && retry.started_version == "1.0.7", "the installed Core is tried again next launch");
}

void test_old_bad_marks_are_retried() {
    FakeVerifier v;
    Server server;
    server.publish(v, "1.0.2", "CORE0002");
    FakeFetcher fetcher(server);
    FakeStore store(v);
    store.seed("1.0.2", "CORE0002");
    store.state.bad = {"1.0.2"};  // recorded by a loader that blamed load failures on the release
    FakeLauncher launcher;
    const auto outcome = run_update(fetcher, v, store, launcher, config(), nullptr);
    check(outcome.started_version == "1.0.2" && store.state.bad.empty() && store.state.bad_rule == bad_rule_revision,
          "bad marks from an older rule are dropped");

    store.state.bad = {"1.0.2"};  // marked under the current rule: stays bad
    FakeFetcher again(server);
    FakeLauncher relaunch;
    const auto skipped = run_update(again, v, store, relaunch, config(), nullptr);
    check(relaunch.launched.empty() && skipped.started_version.empty() &&
          skipped.failure.find("failed to start before") != std::string::npos, "current bad marks are kept");
}

void test_failure_names_the_game_version() {
    FakeVerifier v;
    Server server;
    server.publish(v, "1.0.5", "CORE0005");
    FakeFetcher fetcher(server);
    FakeStore store(v);
    FakeLauncher launcher;
    const auto outcome = run_update(fetcher, v, store, launcher, {host, "1.26.60.1"}, nullptr);
    check(outcome.started_version.empty() && outcome.failure.find("1.26.52") != std::string::npos &&
              outcome.failure.find("1.26.60.1") != std::string::npos,
          "a fresh install on another Minecraft version says which versions differ");

    server.reachable = false;
    FakeFetcher offline(server);
    const auto unreachable = run_update(offline, v, store, launcher, config(), nullptr);
    check(unreachable.failure.find("could not be reached") != std::string::npos, "an unreachable server is explained");
}

void test_unverifiable_local_is_ignored() {
    FakeVerifier v;
    Server server;
    server.reachable = false;
    FakeFetcher fetcher(server);
    FakeStore store(v);
    store.seed("1.0.4", "CORE0004");
    store.cores["1.0.4"] = "TAMPERED";  // the file no longer matches its manifest hash
    store.state.good = {"1.0.4"};
    FakeLauncher launcher;
    const auto outcome = run_update(fetcher, v, store, launcher, config(), nullptr);
    check(launcher.launched.empty() && outcome.started_version.empty() && outcome.state == State::Failed,
          "a tampered local Core is never started");
}

void test_wrong_game_version_is_skipped() {
    FakeVerifier v;
    Server server;
    server.publish(v, "2.0.0", "CORE2000", "1.27.0");  // for a different MCBE build
    FakeFetcher fetcher(server);
    FakeStore store(v);
    store.seed("1.0.5", "CORE0005");
    store.state.good = {"1.0.5"};
    FakeLauncher launcher;
    const auto outcome = run_update(fetcher, v, store, launcher, config(), nullptr);
    check(fetcher.downloads == 0 && outcome.started_version == "1.0.5", "a Core for another MCBE version is not taken");
}

} // namespace

int main() {
    try {
        test_first_run_downloads_and_runs();
        test_up_to_date_does_not_download();
        test_offline_runs_verified_local();
        test_nested_revision_update_and_fallback();
        test_bad_signature_is_not_run();
        test_sha_mismatch_is_discarded();
        test_interrupted_download_keeps_old();
        test_launch_failure_falls_back();
        test_load_failure_is_retried();
        test_old_bad_marks_are_retried();
        test_failure_names_the_game_version();
        test_unverifiable_local_is_ignored();
        test_wrong_game_version_is_skipped();
        std::cout << "Loader tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Loader test failed: " << error.what() << '\n';
        return 1;
    }
}
