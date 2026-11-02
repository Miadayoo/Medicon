#include "AltService.hpp"
#include "Clipboard.hpp"

#include "../social/Files.hpp"
#include "../social/WinHttp.hpp"

#include <medicon/alt/Auth.hpp>
#include <medicon/alt/Session.hpp>

#include <Windows.h>
#include <dpapi.h>
#include <objbase.h>

#include <atomic>
#include <algorithm>
#include <cwctype>
#include <mutex>
#include <vector>

namespace medicon::alt {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// Binds the blob to this purpose, like the Social license.
constexpr char entropy_text[] = "Medicon.Alts.v1";
constexpr wchar_t profile_prefix[] = L"Medicon-Login-";

DATA_BLOB entropy() {
    return DATA_BLOB{static_cast<DWORD>(sizeof(entropy_text) - 1),
                     reinterpret_cast<BYTE*>(const_cast<char*>(entropy_text))};
}

struct LocalBlob {
    DATA_BLOB blob{};
    ~LocalBlob() {
        if (!blob.pbData) return;
        SecureZeroMemory(blob.pbData, blob.cbData);
        LocalFree(blob.pbData);
    }
};

std::wstring edge_path() {
    wchar_t path[MAX_PATH]{};
    DWORD size = sizeof(path);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe", nullptr,
                     RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS)
        return path;
    return {};
}

std::wstring quoted(const std::wstring& text) { return L"\"" + text + L"\""; }

// Each launch gets both an InPrivate window and a newly created user-data
// directory. It never falls back to a browser that could reuse existing cookies.
class LoginWindow {
public:
    ~LoginWindow() { close(); }

    bool open(const std::string& url) {
        close();
        const std::wstring wide_url = social::net::widen(url);
        const std::wstring edge = edge_path();
        return !edge.empty() && open_edge(edge, wide_url);
    }

    void close() {
        if (!process_) return;
        EnumWindows(&close_owned, static_cast<LPARAM>(GetProcessId(process_)));
        if (WaitForSingleObject(process_, 3000) != WAIT_OBJECT_0) TerminateProcess(process_, 0);
        CloseHandle(process_);
        process_ = nullptr;
        // Only remove the fresh directory this instance created. Another login
        // may still be using a different Medicon-Login-* profile.
        for (int attempt = 0; attempt < 10 && !profile_.empty(); ++attempt) {
            if (!profile_.is_absolute() || profile_.parent_path() != profile_root_ ||
                !profile_.filename().wstring().starts_with(profile_prefix)) break;
            const DWORD attributes = GetFileAttributesW(profile_.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) break;
            std::error_code ec;
            std::filesystem::remove_all(profile_, ec);
            if (!ec) profile_.clear();
            else Sleep(200);
        }
    }

private:
    static BOOL CALLBACK close_owned(HWND window, LPARAM process_id) {
        DWORD owner = 0;
        GetWindowThreadProcessId(window, &owner);
        if (owner == static_cast<DWORD>(process_id)) PostMessageW(window, WM_CLOSE, 0, 0);
        return TRUE;
    }

    bool open_edge(const std::wstring& edge, const std::wstring& url) {
        std::error_code ec;
        const auto temp = std::filesystem::temp_directory_path(ec);
        if (ec) return false;
        const auto root = std::filesystem::weakly_canonical(temp, ec);
        if (ec || !root.is_absolute()) return false;
        GUID id{};
        wchar_t unique[40]{};
        if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, unique, 40)) return false;
        const auto profile = root / (profile_prefix + std::to_wstring(GetCurrentProcessId()) + L"-" + unique);
        // Refuse an existing directory instead of ever reusing a cookie store.
        if (!std::filesystem::create_directory(profile, ec) || ec) return false;
        // login_url() allows nothing in the URL that could leave these quotes.
        std::wstring command = quoted(edge) + L" --inprivate --new-window " + quoted(url) + L" --user-data-dir=" + quoted(profile.wstring()) +
                               L" --no-first-run --no-default-browser-check --window-size=520,700";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(edge.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
            std::filesystem::remove(profile, ec);
            return false;
        }
        CloseHandle(process.hThread);
        process_ = process.hProcess;
        profile_ = profile;
        profile_root_ = root;
        return true;
    }

    HANDLE process_ = nullptr;
    std::filesystem::path profile_;
    std::filesystem::path profile_root_;
};

social::Endpoint endpoint(std::string_view host) { return social::Endpoint{true, std::string(host), 443, ""}; }

HttpReply reply_of(const social::net::HttpResponse& response) { return HttpReply{response.status, response.body}; }

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

struct AltService::State {
    explicit State(Options options)
        : file(options.directory / L"alts.bin"), log(std::move(options.log)), http(options.client_version),
          application_id(std::move(options.application_id)), make_sign_in_host(std::move(options.make_sign_in_host)) {}

    Channel channel;
    std::filesystem::path file;
    std::function<void(std::string_view)> log;
    social::net::HttpClient http;
    std::string application_id;
    std::function<std::unique_ptr<SignInHost>(Channel&)> make_sign_in_host;
    std::vector<Credentials> accounts;
    std::mutex session_mutex;
    SessionChanged session;
    std::string session_authorization;
    std::string issued_token;
    std::uint64_t session_generation = 0;
    bool readable = true; // false: alts.bin is kept as it is, never overwritten
    std::atomic_bool stopping{false};
    std::atomic_bool finished{false};

    void report(std::string_view line) const {
        if (log) log(line);
    }

    void publish() {
        AccountsChanged changed;
        changed.load_failed = !readable;
        for (const auto& entry : accounts) changed.accounts.push_back(entry.account);
        channel.events.push(std::move(changed));
    }

    void load() {
        std::error_code ec;
        if (!std::filesystem::exists(file, ec)) return;
        std::string sealed = social::detail::read_file(file);
        DATA_BLOB input{static_cast<DWORD>(sealed.size()), reinterpret_cast<BYTE*>(sealed.data())};
        DATA_BLOB salt = entropy();
        LocalBlob output;
        if (sealed.empty() || !CryptUnprotectData(&input, nullptr, &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output.blob)) {
            // Typically copied from another Windows account.
            readable = false;
            report("AltManager: alts.bin could not be decrypted.");
            return;
        }
        auto parsed = parse_accounts({reinterpret_cast<const char*>(output.blob.pbData), output.blob.cbData});
        if (!parsed) {
            readable = false;
            report("AltManager: alts.bin is not valid.");
            return;
        }
        accounts = std::move(*parsed);
    }

    bool save() {
        if (!readable) return false;
        std::string plain = serialize_accounts(accounts);
        DATA_BLOB input{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(plain.data())};
        DATA_BLOB salt = entropy();
        LocalBlob output;
        const bool sealed = CryptProtectData(&input, L"Medicon alts", &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                                             &output.blob) != FALSE;
        SecureZeroMemory(plain.data(), plain.size());
        if (!sealed) {
            report("AltManager: the accounts could not be encrypted.");
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        std::string error;
        if (!social::detail::replace_file(
                file, {reinterpret_cast<const char*>(output.blob.pbData), static_cast<std::size_t>(output.blob.cbData)}, error)) {
            report("AltManager: " + error);
            return false;
        }
        return true;
    }

    void remove(const std::string& xuid) {
        {
            std::lock_guard lock(session_mutex);
            // Keep the in-use credential until the user selects another one.
            // Removing it from storage does not change an existing connection.
            if (session.selected_xuid == xuid) {
                session.error = "account_in_use";
                session.diagnostic.clear();
                channel.events.push(session);
                return;
            }
        }
        std::erase_if(accounts, [&](const Credentials& entry) { return entry.account.xuid == xuid; });
        if (!save()) report("AltManager: removing an account was not saved.");
        publish();
    }

    void run_sign_in();
    void use(const UseAccount& command);
    void run() {
        load();
        publish();
        { std::lock_guard lock(session_mutex); channel.events.push(session); }
        while (const auto command = channel.commands.pop()) {
            if (std::holds_alternative<StartSignIn>(*command)) run_sign_in();
            else if (const auto* remove_account = std::get_if<RemoveAccount>(&*command)) remove(remove_account->xuid);
            else if (const auto* use_account = std::get_if<UseAccount>(&*command)) use(*use_account);
        }
        finished.store(true, std::memory_order_release);
    }
};

namespace {

template <class State>
class Host final : public SignInHost {
public:
    explicit Host(State& state) : state_(state) {}

    HttpReply post_form(std::string_view host, std::string_view path, std::string_view body) override {
        auto reply = reply_of(state_.http.upload(endpoint(host), path, "application/x-www-form-urlencoded", body.data(), body.size(), {}));
        if (reply.status != 400) report_http(host, path, reply.status);
        return reply;
    }
    HttpReply post_json(std::string_view host, std::string_view path, std::string_view body) override {
        auto reply = reply_of(state_.http.request(endpoint(host), L"POST", path, body, {},
            host.ends_with(".xboxlive.com") ? "x-xbl-contract-version: 1\r\n" : ""));
        report_http(host, path, reply.status);
        return reply;
    }
    HttpReply post_authorized_json(std::string_view host, std::string_view path, std::string_view body,
                                   std::string_view authorization) override {
        auto reply = reply_of(state_.http.request(endpoint(host), L"POST", path, body, {},
            "Authorization: " + std::string(authorization) + "\r\n"));
        report_http(host, path, reply.status);
        return reply;
    }
    HttpReply get_profile(std::string_view user_hash, std::string_view xsts_token) override {
        constexpr std::string_view host = "profile.xboxlive.com";
        constexpr std::string_view path = "/users/me/profile/settings?settings=Gamertag";
        const std::string headers = "Authorization: XBL3.0 x=" + std::string(user_hash) + ";" + std::string(xsts_token) +
                                    "\r\nx-xbl-contract-version: 2\r\nContent-Type: application/json\r\n";
        auto reply = reply_of(state_.http.request(endpoint(host), L"GET", path, {}, {}, headers));
        report_http(host, path, reply.status);
        return reply;
    }
    bool copy_code(std::string_view user_code) override {
        user_code_ = user_code;
        code_copied_ = detail::copy_sign_in_code(user_code_);
        return code_copied_;
    }
    bool open_window(const std::string& url) override {
        url_ = url;
        window_opened_ = window_.open(url_);
        return window_opened_;
    }
    void close_window() override { window_.close(); }
    bool wait(std::chrono::seconds duration) override {
        const auto deadline = Clock::now() + duration;
        while (!state_.stopping.load(std::memory_order_acquire)) {
            const auto command = state_.channel.commands.pop(deadline);
            if (!command) return !state_.stopping.load(std::memory_order_acquire);
            if (std::holds_alternative<CancelSignIn>(*command)) return false;
            if (std::holds_alternative<CopySignInCode>(*command)) {
                copy_code(user_code_);
                stage(SignInStage::Waiting, user_code_, code_copied_, window_opened_);
            }
            if (std::holds_alternative<OpenSignInPage>(*command)) {
                open_window(url_);
                stage(SignInStage::Waiting, user_code_, code_copied_, window_opened_);
            }
            if (const auto* remove_account = std::get_if<RemoveAccount>(&*command)) state_.remove(remove_account->xuid);
        }
        return false;
    }
    void stage(SignInStage stage, std::string_view user_code, bool code_copied, bool window_opened) override {
        state_.channel.events.push(SignInProgress{stage, std::string(user_code), code_copied, window_opened});
    }

private:
    void report_http(std::string_view host, std::string_view path, int status) {
        // These paths are fixed endpoints; never include request/response bodies or codes.
        state_.report("AltManager: " + std::string(host) + std::string(path) + " HTTP " + std::to_string(status));
    }
    State& state_;
    LoginWindow window_;
    std::string user_code_, url_;
    bool code_copied_ = false;
    bool window_opened_ = false;
};

} // namespace

void AltService::State::use(const UseAccount& command) {
    const auto& xuid = command.xuid;
    {
        std::lock_guard lock(session_mutex);
        session.error.clear();
        session.diagnostic.clear();
        if (!session.available) {
            session.error = "switch_unavailable";
            channel.events.push(session);
            channel.events.push(UseFinished{xuid, false, command.request_id});
            return;
        }
        session.busy = true;
        channel.events.push(session);
    }
    SessionResult result;
    const auto found = std::find_if(accounts.begin(), accounts.end(), [&](const auto& value) { return value.account.xuid == xuid; });
    try {
        if (found == accounts.end()) result.error = "account_missing";
        else {
            GUID guid{};
            wchar_t text[40]{};
            if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, text, 40)) result.error = "session_failed";
            else {
                const std::wstring wide(text);
                const std::string device(wide.begin() + 1, wide.end() - 1);
                auto host = make_sign_in_host ? make_sign_in_host(channel) : std::make_unique<Host<State>>(*this);
                result = prepare_session(*host, *found, application_id, device, "1.26.60");
                // Persist refresh-token rotation even if a later Xbox/MC stage fails.
                if (!save()) report("AltManager: refreshed credential could not be saved.");
            }
        }
    } catch (...) { result = {{}, "bad_response", "Account switch: invalid response"}; }
    std::lock_guard lock(session_mutex);
    session.busy = false;
    const bool success = !result.authorization.empty() && !stopping.load();
    if (success) {
        ++session_generation;
        session.selected_xuid = xuid;
        session_authorization = std::move(result.authorization);
        issued_token.clear();
        report("AltManager: selected account authenticated for Minecraft; reconnect to apply.");
    } else {
        session.error = result.error;
        session.diagnostic = result.diagnostic;
        report("AltManager: account switch failed: " + result.error + "; " + result.diagnostic);
    }
    channel.events.push(session);
    channel.events.push(UseFinished{xuid, success, command.request_id});
    report("AltManager: account switch completion published.");
}

void AltService::State::run_sign_in() {
    report("AltManager: sign-in started.");
    auto host = make_sign_in_host ? make_sign_in_host(channel) : std::make_unique<Host<State>>(*this);
    auto result = sign_in(*host, application_id, now_ms());
    SignInFinished finished_event;
    finished_event.cancelled = result.cancelled;
    finished_event.error = std::move(result.error);
    finished_event.diagnostic = std::move(result.diagnostic);
    if (result.credentials) {
        finished_event.gamertag = result.credentials->account.gamertag;
        upsert_account(accounts, std::move(*result.credentials));
        finished_event.ok = save();
        if (!finished_event.ok) finished_event.error = "save_failed";
        publish();
        if (finished_event.ok) report("AltManager: account added and saved.");
    } else if (!result.cancelled) {
        report("AltManager: sign-in failed: " + finished_event.error);
        if (!finished_event.diagnostic.empty()) report("AltManager: " + finished_event.diagnostic);
    } else {
        report("AltManager: sign-in cancelled before an account was added.");
    }
    channel.events.push(std::move(finished_event));
}

AltService::AltService(Options options) : state_(std::make_shared<State>(std::move(options))) {}

AltService::~AltService() { stop(100ms); }

void AltService::start() {
    if (thread_.joinable()) return;
    thread_ = std::thread([state = state_] { state->run(); });
}

void AltService::request_stop() noexcept {
    state_->stopping.store(true, std::memory_order_release);
    state_->channel.commands.close();
}

bool AltService::finished() const noexcept {
    return !thread_.joinable() || state_->finished.load(std::memory_order_acquire);
}

bool AltService::stop(std::chrono::milliseconds timeout) {
    request_stop();
    if (!thread_.joinable()) return true;
    const auto deadline = Clock::now() + timeout;
    while (!state_->finished.load(std::memory_order_acquire) && Clock::now() < deadline) std::this_thread::sleep_for(10ms);
    if (state_->finished.load(std::memory_order_acquire)) {
        thread_.join();
        return true;
    }
    // A network call outlasted the budget. The worker holds its own reference to
    // State, so it ends safely once that call returns.
    thread_.detach();
    return false;
}

Channel& AltService::channel() noexcept { return state_->channel; }

void AltService::set_game_bridge(bool available) {
    std::lock_guard lock(state_->session_mutex);
    state_->session.available = available;
    state_->channel.events.push(state_->session);
}

std::optional<std::string> AltService::native_authorization(std::string_view url) {
    if (!native_multiplayer_url(url)) return std::nullopt;
    std::lock_guard lock(state_->session_mutex);
    if (state_->session.busy) return std::string{};
    if (state_->session.selected_xuid.empty()) return std::nullopt;
    if (state_->stopping.load()) return std::string{};
    return state_->session_authorization;
}

void AltService::native_auth_result(unsigned status) {
    std::lock_guard lock(state_->session_mutex);
    if (state_->session.selected_xuid.empty()) return;
    state_->report("AltManager: native multiplayer token HTTP " + std::to_string(status));
    if (status >= 400 || status == 0) {
        state_->session.error = "connection_failed";
        state_->session.diagnostic = "Native multiplayer token: HTTP " + std::to_string(status);
        state_->channel.events.push(state_->session);
    }
}

std::optional<std::string> AltService::connection_token(std::string_view native_token) {
    std::lock_guard lock(state_->session_mutex);
    if (state_->session.busy) {
        state_->session.error = "session_busy";
        state_->session.diagnostic.clear();
        state_->channel.events.push(state_->session);
        return std::string{};
    }
    if (state_->session.selected_xuid.empty()) return std::nullopt;
    if (state_->stopping.load()) return std::string{};
    // A late replacement would leave NetherNet's earlier identity assertion
    // bound to a different account/token. Only accept the game's own response
    // after routing its original request through the selected service session.
    if (server_token_xuid(native_token) == state_->session.selected_xuid && valid_server_token(native_token, now_ms() / 1000)) {
        state_->issued_token = native_token;
        return std::nullopt;
    }
    state_->session.error = "native_session_stale";
    state_->session.diagnostic = "Native multiplayer identity does not match the selected account. Return to the title screen and reconnect.";
    state_->report("AltManager: blocked stale native multiplayer identity; no late token replacement.");
    state_->channel.events.push(state_->session);
    return std::string{};
}

void AltService::connection_used(std::string_view token) {
    std::lock_guard lock(state_->session_mutex);
    const auto xuid = !state_->issued_token.empty() && token == state_->issued_token
        ? state_->session.selected_xuid : server_token_xuid(token);
    if (xuid.empty() || xuid == state_->session.active_xuid) return;
    state_->session.active_xuid = xuid;
    state_->session.error.clear();
    state_->session.diagnostic.clear();
    state_->report("AltManager: server login serialized with the authenticated account.");
    state_->channel.events.push(state_->session);
}

} // namespace medicon::alt
