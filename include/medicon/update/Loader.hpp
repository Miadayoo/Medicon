#pragma once

#include <medicon/update/Update.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The update orchestration (docs/auto-update.md). It is written against the
// interfaces below, never Win32 directly, so run_update's whole decision
// sequence is unit tested with fakes. The Windows implementations live in
// Download.cpp, Verify.cpp, Storage.cpp and LoaderMain.cpp.
namespace medicon::update {

// Matches MediconUpdateState in HostApi.h.
enum class State : std::uint32_t { UpToDate = 0, Updated = 1, Offline = 2, Failed = 3, Fallback = 4 };

struct FetchResult {
    bool reached = false;       // false for a network/TLS/redirect failure: the server was not reached
    int status = 0;             // HTTP status when reached
    std::string body;
    std::string error;          // short, never a secret
};

// One GET to the embedded host over HTTPS. Redirects and non-HTTPS are refused
// by the implementation; `path` already begins with '/'.
class Fetcher {
public:
    virtual ~Fetcher() = default;
    [[nodiscard]] virtual FetchResult get(std::string_view path, std::uint64_t max_bytes) = 0;
};

class Verifier {
public:
    virtual ~Verifier() = default;
    // RSA-PSS/SHA-256 over the exact manifest bytes with the embedded public key.
    [[nodiscard]] virtual bool verify_manifest(std::string_view manifest, std::string_view signature) = 0;
    // Lowercase hex, or empty on failure.
    [[nodiscard]] virtual std::string sha256_hex(std::string_view data) = 0;
};

// One installed version on disk: the Core and the manifest/signature it came with.
struct InstalledCore {
    std::string version;
    std::string manifest;
    std::string signature;
};

class FileStore {
public:
    virtual ~FileStore() = default;
    [[nodiscard]] virtual std::vector<InstalledCore> installed() = 0;
    // Absolute path of an installed Core, for LoadLibraryExW.
    [[nodiscard]] virtual std::string core_path(const std::string& version) = 0;
    // Lowercase hex SHA-256 of the installed Core file, or empty when unreadable.
    [[nodiscard]] virtual std::string core_sha256(const std::string& version) = 0;
    // Write the Core, manifest and signature of `version` atomically: either the
    // whole version becomes visible or none of it does.
    [[nodiscard]] virtual bool install(const std::string& version, std::string_view dll, std::string_view manifest,
                                       std::string_view signature) = 0;
    virtual void remove(const std::string& version) = 0;
    [[nodiscard]] virtual LoaderState load_state() = 0;
    virtual void save_state(const LoaderState& state) = 0;
    virtual void log(std::string_view line) = 0;
};

// Loads a verified Core and runs the client's bootstrap. launch() returns true
// only once the Core reported it started (Medicon_Initialize returned 1).
class CoreLauncher {
public:
    virtual ~CoreLauncher() = default;
    [[nodiscard]] virtual bool launch(const std::string& version, const std::string& core_path, State state,
                                      const std::string& previous_version) = 0;
};

struct UpdateConfig {
    std::string allowed_host;   // "medicon.example.com": the only host ever fetched
    std::string game_version;   // the running Minecraft.Windows.exe FileVersion
};

// The live status the loader publishes for the Core's UI (MediconLoaderGetStatus).
struct Status {
    Phase phase = Phase::Idle;
    std::uint32_t progress_permille = 0;
    std::string current_version;
    std::string latest_version;
    std::string last_error;
};
using StatusSink = std::function<void(const Status&)>;

struct UpdateOutcome {
    State state = State::Failed;
    std::string started_version;   // empty when no Core could start
    std::string previous_version;  // what ran last, from state.json
};

// The whole flow: check the server, verify and install a newer Core when there
// is one, then start the newest Core that launches, falling back to older
// verified ones. Offline, it starts the newest verified local Core. Returns
// what happened; started_version is empty only when nothing could start.
UpdateOutcome run_update(Fetcher& fetcher, Verifier& verifier, FileStore& store, CoreLauncher& launcher,
                         const UpdateConfig& config, const StatusSink& status);

} // namespace medicon::update
