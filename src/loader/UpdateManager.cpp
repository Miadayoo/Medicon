#include <medicon/update/Loader.hpp>

#include <algorithm>

namespace medicon::update {
namespace {

// A locally installed Core is trusted again only after its own manifest's
// signature verifies and the Core file still hashes to that manifest. Nothing
// is trusted from the file system alone.
std::optional<Manifest> verify_local(Verifier& verifier, FileStore& store, const InstalledCore& core,
                                     std::string_view host) {
    if (!verifier.verify_manifest(core.manifest, core.signature)) return std::nullopt;
    std::string error;
    auto manifest = parse_manifest(core.manifest, host, error);
    if (!manifest || manifest->version_text != core.version) return std::nullopt;
    if (!same_digest(manifest->sha256, store.core_sha256(core.version))) return std::nullopt;
    return manifest;
}

void publish(const StatusSink& status, Status& state, Phase phase) {
    state.phase = phase;
    if (status) status(state);
}

} // namespace

UpdateOutcome run_update(Fetcher& fetcher, Verifier& verifier, FileStore& store, CoreLauncher& launcher,
                         const UpdateConfig& config, const StatusSink& status) {
    Status live;
    LoaderState persisted = store.load_state();
    live.current_version = persisted.last_run;
    UpdateOutcome outcome;
    outcome.previous_version = persisted.last_run;
    // Older loaders also blamed a release when Windows could not map its file
    // (a non-ASCII profile path, an antivirus lock); retry what they marked.
    if (persisted.bad_rule != bad_rule_revision) {
        if (!persisted.bad.empty()) store.log("retrying Cores an older loader marked as failed");
        persisted.bad.clear();
        persisted.bad_rule = bad_rule_revision;
    }
    // Why nothing newer is running, kept for the player when nothing starts.
    std::string problem;

    // Verify every installed Core up front, so an interrupted or tampered file
    // is never a candidate, online or off.
    std::vector<std::string> verified;
    for (const auto& core : store.installed()) {
        if (verify_local(verifier, store, core, config.allowed_host)) verified.push_back(core.version);
        else store.log("ignoring unverifiable local Core " + core.version);
    }

    bool downloaded = false;     // a newer Core was installed this run
    bool reached_server = false;

    publish(status, live, Phase::Checking);
    const auto manifest_fetch = fetcher.get("/medicon/manifest.json", max_manifest_bytes);
    reached_server = manifest_fetch.reached;
    if (manifest_fetch.reached && manifest_fetch.status == 200) {
        const auto signature = fetcher.get("/medicon/manifest.sig", max_signature_bytes);
        std::string error;
        std::optional<Manifest> manifest;
        if (signature.reached && signature.status == 200 &&
            verifier.verify_manifest(manifest_fetch.body, signature.body))
            manifest = parse_manifest(manifest_fetch.body, config.allowed_host, error);
        else
            error = "manifest_signature";

        if (!manifest) {
            store.log("update check rejected: " + (error.empty() ? manifest_fetch.error : error));
            problem = "The release on the update server could not be verified (" +
                      (error.empty() ? manifest_fetch.error : error) + ").";
        } else if (!mcbe_compatible(manifest->mcbe_version, config.game_version)) {
            store.log("latest Core " + manifest->version_text + " targets MCBE " + manifest->mcbe_version +
                      ", not " + config.game_version);
            problem = "Medicon " + manifest->version_text + " supports Minecraft " + manifest->mcbe_version +
                      ", but this game is " + (config.game_version.empty() ? "an unknown version" : config.game_version) + ".";
        } else {
            live.latest_version = manifest->version_text;
            const bool have_it = std::find(verified.begin(), verified.end(), manifest->version_text) != verified.end();
            const bool is_bad = std::find(persisted.bad.begin(), persisted.bad.end(), manifest->version_text) !=
                                persisted.bad.end();
            if (have_it || is_bad) {
                // Already installed, or known not to start: never fetch it again.
                store.log("latest Core " + manifest->version_text + (is_bad ? " previously failed to start" : " already installed"));
                if (is_bad) problem = "Medicon " + manifest->version_text + " failed to start before.";
            } else {
                publish(status, live, Phase::Downloading);
                const auto core = fetcher.get("/medicon/releases/" + manifest->version_text + "/" +
                                                  std::string(core_file_name),
                                              manifest->size);
                if (!core.reached) {
                    reached_server = false;
                    store.log("download failed: " + core.error);
                    problem = "Downloading Medicon " + manifest->version_text + " failed (" + core.error + ").";
                } else if (core.status != 200 || core.body.size() != manifest->size) {
                    store.log("download incomplete for " + manifest->version_text);
                    problem = "The download of Medicon " + manifest->version_text + " was incomplete.";
                } else {
                    publish(status, live, Phase::Verifying);
                    if (!same_digest(manifest->sha256, verifier.sha256_hex(core.body))) {
                        store.log("sha256 mismatch for " + manifest->version_text + "; discarded");
                        problem = "The download of Medicon " + manifest->version_text + " did not match its release.";
                    } else if (!store.install(manifest->version_text, core.body, manifest_fetch.body, signature.body)) {
                        store.log("could not install " + manifest->version_text);
                        problem = "Medicon " + manifest->version_text + " could not be saved to the Medicon data folder.";
                    } else {
                        store.log("installed Core " + manifest->version_text);
                        verified.push_back(manifest->version_text);
                        downloaded = true;
                        publish(status, live, Phase::Ready);
                    }
                }
            }
        }
    } else if (manifest_fetch.reached) {
        store.log("update check: server returned HTTP " + std::to_string(manifest_fetch.status));
        problem = "The update server returned HTTP " + std::to_string(manifest_fetch.status) + ".";
    } else {
        store.log("update check: server unreachable (" + manifest_fetch.error + ")");
        problem = "The update server " + config.allowed_host + " could not be reached (" + manifest_fetch.error + ").";
    }

    // Start the newest verified Core that launches, oldest-known-bad excluded,
    // falling back through the rest. The newest is the one just downloaded when
    // there was one.
    const auto order = start_order(verified, persisted);
    if (order.empty()) {
        publish(status, live, Phase::Failed);
        live.last_error = reached_server ? "no_verified_core" : "offline_no_core";
        if (status) status(live);
        store.log("no verified Core could be started");
        outcome.state = State::Failed;
        outcome.failure = problem.empty() ? "No usable Medicon version is installed." : problem;
        return outcome;
    }

    const std::string newest = order.front();
    // The state each candidate would start in: the Core sees it at init time, so
    // it is decided before the call, not after.
    const auto state_for = [&](const std::string& version) {
        if (!reached_server) return State::Offline;
        if (version != newest) return State::Fallback;
        return downloaded ? State::Updated : State::UpToDate;
    };
    publish(status, live, Phase::Loading);
    for (const auto& version : order) {
        const State intended = state_for(version);
        std::string error;
        const auto result = launcher.launch(version, store.core_path(version), intended, persisted.last_run, error);
        if (result == LaunchResult::Started) {
            store.log("started Core " + version);
            mark_good(persisted, version);
            persisted.last_run = version;
            store.save_state(persisted);

            live.current_version = version;
            outcome.state = intended;
            outcome.started_version = version;

            publish(status, live, intended == State::Offline ? Phase::Offline
                                  : intended == State::Updated ? Phase::Running
                                                               : Phase::UpToDate);
            // Keep the newest installed, the one now running and two good ones.
            std::vector<std::string> installed;
            for (const auto& core : store.installed()) installed.push_back(core.version);
            for (const auto& stale : prune(installed, persisted, version)) {
                store.remove(stale);
                store.log("removed old Core " + stale);
            }
            return outcome;
        }
        if (result == LaunchResult::Rejected) {
            store.log("Core " + version + " failed to start (" + error + "); trying an older version");
            mark_bad(persisted, version);
        } else {
            // The verified file itself is fine; this PC could not map it. Not
            // the release's fault, so the next launch tries it again.
            store.log("Core " + version + " could not be loaded (" + error + "); trying an older version");
        }
        problem = "Medicon " + version + " could not be started (" + error + ").";
        persisted.last_run.clear();
        store.save_state(persisted);
    }

    publish(status, live, Phase::Failed);
    live.last_error = "core_launch_failed";
    if (status) status(live);
    outcome.state = State::Failed;
    outcome.failure = problem;
    return outcome;
}

} // namespace medicon::update
