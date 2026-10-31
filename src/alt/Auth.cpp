#include <medicon/alt/Auth.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <limits>

namespace medicon::alt {
namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

// Keep device authorization and token polling on the same consumer MSA endpoint.
constexpr std::string_view msa_host = "login.live.com";
constexpr std::string_view device_code_path = "/oauth20_connect.srf";
constexpr std::string_view token_path = "/oauth20_token.srf";
constexpr std::string_view scope = "XboxLive.signin offline_access";
constexpr std::string_view xbl_host = "user.auth.xboxlive.com";
constexpr std::string_view xsts_host = "xsts.auth.xboxlive.com";
constexpr int default_interval = 5;
constexpr int max_interval = 60;

SignInResult failed(std::string error, std::string diagnostic = {}) {
    SignInResult result;
    result.error = std::move(error);
    result.diagnostic = std::move(diagnostic);
    return result;
}

SignInResult cancelled() {
    SignInResult result;
    result.cancelled = true;
    return result;
}

std::optional<json> parse_object(std::string_view text) {
    auto document = json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return std::nullopt;
    return document;
}

std::string text_field(const json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

int int_field(const json& object, const char* key, int fallback) {
    const auto found = object.find(key);
    return found != object.end() && found->is_number_integer() ? found->get<int>() : fallback;
}

// Closes Microsoft's window on every way out of the sign-in.
struct WindowScope {
    SignInHost& host;
    ~WindowScope() { host.close_window(); }
};

std::optional<std::uint32_t> xbox_error_code(std::string_view body) {
    const auto document = parse_object(body);
    if (!document) return std::nullopt;
    const auto found = document->find("XErr");
    if (found == document->end()) return std::nullopt;
    std::uint64_t value = 0;
    if (found->is_number_unsigned()) value = found->get<std::uint64_t>();
    else if (found->is_number_integer()) {
        const auto number = found->get<std::int64_t>();
        if (number < 0) return std::nullopt;
        value = static_cast<std::uint64_t>(number);
    } else if (found->is_string()) {
        const auto& text = found->get_ref<const std::string&>();
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    } else return std::nullopt;
    if (value > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

SignInResult xbox_http_failure(const HttpReply& reply, std::string_view stage, std::string error) {
    std::string diagnostic(stage);
    diagnostic += reply.status == 0 ? ": no response" : ": HTTP " + std::to_string(reply.status);
    if (reply.status == 0) error = "network";
    const auto code = xbox_error_code(reply.body);
    if (!code) return failed(std::move(error), std::move(diagnostic));
    diagnostic += ", XErr " + std::to_string(*code);
    switch (*code) {
    case 2148916233: error = "no_xbox_profile"; break;
    case 2148916235: error = "region_blocked"; break;
    case 2148916236:
    case 2148916237:
    case 2148916238: error = "child_account"; break;
    default: break;
    }
    return failed(std::move(error), std::move(diagnostic));
}

bool decimal_id(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool header_value(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return c > 32 && c < 127; });
}

SignInResult xbox_profile(SignInHost& host, const std::string& access_token, const std::string& refresh_token,
                          std::int64_t now_ms) {
    const json user{{"RelyingParty", "http://auth.xboxlive.com"},
                    {"TokenType", "JWT"},
                    {"Properties",
                     {{"AuthMethod", "RPS"}, {"SiteName", xbl_host}, {"RpsTicket", "d=" + access_token}}}};
    const auto user_reply = host.post_json(xbl_host, "/user/authenticate", user.dump());
    if (user_reply.status != 200) return xbox_http_failure(user_reply, "Xbox user auth", "xbox_user_failed");
    const auto user_token = parse_object(user_reply.body);
    if (!user_token || text_field(*user_token, "Token").empty()) return failed("bad_response", "Xbox user auth: invalid response");

    // Request the service token accepted by profile.xboxlive.com. Do not depend
    // on the optional xid/gtg display claims to identify the linked account.
    const json authorize{{"RelyingParty", "http://xboxlive.com"},
                         {"TokenType", "JWT"},
                         {"Properties",
                          {{"SandboxId", "RETAIL"}, {"UserTokens", json::array({text_field(*user_token, "Token")})}}}};
    const auto xsts_reply = host.post_json(xsts_host, "/xsts/authorize", authorize.dump());
    if (xsts_reply.status != 200) return xbox_http_failure(xsts_reply, "Xbox token", "xsts_failed");
    const auto xsts = parse_object(xsts_reply.body);
    if (!xsts) return failed("bad_response", "Xbox token: invalid response");
    const auto claims = xsts->find("DisplayClaims");
    if (claims == xsts->end() || !claims->is_object()) return failed("bad_response", "Xbox token: missing display claims");
    const auto users = claims->find("xui");
    if (users == claims->end() || !users->is_array() || users->empty() || !users->front().is_object())
        return failed("bad_response", "Xbox token: missing user claims");
    Credentials credentials;
    credentials.account.xuid = text_field(users->front(), "xid");
    credentials.account.gamertag = text_field(users->front(), "gtg");
    credentials.account.added_at = now_ms;
    credentials.refresh_token = refresh_token;
    if (credentials.account.xuid.empty() || credentials.account.gamertag.empty()) {
        const auto hash = text_field(users->front(), "uhs");
        const auto token = text_field(*xsts, "Token");
        if (!header_value(hash) || hash.find(';') != std::string::npos || !header_value(token))
            return failed("bad_response", "Xbox token: missing or invalid profile credentials");
        const auto profile_reply = host.get_profile(hash, token);
        if (profile_reply.status != 200) return xbox_http_failure(profile_reply, "Xbox profile", "profile_failed");
        const auto profile = parse_object(profile_reply.body);
        if (!profile) return failed("bad_response", "Xbox profile: invalid response");
        const auto profiles = profile->find("profileUsers");
        if (profiles == profile->end() || !profiles->is_array() || profiles->size() != 1 || !profiles->front().is_object())
            return failed("profile_unavailable", "Xbox profile: missing account details");
        const auto& account = profiles->front();
        const auto xuid = text_field(account, "id");
        if (!credentials.account.xuid.empty() && credentials.account.xuid != xuid)
            return failed("bad_response", "Xbox profile: account identity mismatch");
        credentials.account.xuid = xuid;
        credentials.account.gamertag.clear();
        const auto settings = account.find("settings");
        if (settings != account.end() && settings->is_array()) {
            for (const auto& setting : *settings) {
                if (setting.is_object() && text_field(setting, "id") == "Gamertag") {
                    credentials.account.gamertag = text_field(setting, "value");
                    break;
                }
            }
        }
    }
    if (!decimal_id(credentials.account.xuid) || credentials.account.gamertag.empty())
        return failed("profile_unavailable", "Xbox profile: missing or invalid account details");
    SignInResult result;
    result.credentials = std::move(credentials);
    return result;
}

} // namespace

std::string form_encode(std::string_view text) {
    std::string encoded;
    encoded.reserve(text.size());
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded.push_back(raw);
        } else {
            std::array<char, 4> hex{};
            std::snprintf(hex.data(), hex.size(), "%%%02X", c);
            encoded.append(hex.data(), 3);
        }
    }
    return encoded;
}

std::optional<std::string> login_url(std::string_view uri, std::string_view code) {
    constexpr std::string_view scheme = "https://";
    if (!uri.starts_with(scheme) || code.empty()) return std::nullopt;
    for (const char raw : code)
        if (!std::isalnum(static_cast<unsigned char>(raw))) return std::nullopt;
    for (const char raw : uri) {
        const auto c = static_cast<unsigned char>(raw);
        if (!std::isalnum(c) && std::string_view("-._~/:").find(raw) == std::string_view::npos) return std::nullopt;
    }
    const auto rest = uri.substr(scheme.size());
    const auto host = rest.substr(0, rest.find('/'));
    constexpr std::array hosts{std::string_view{"microsoft.com"}, std::string_view{"www.microsoft.com"},
                               std::string_view{"login.live.com"}, std::string_view{"login.microsoftonline.com"}};
    if (std::find(hosts.begin(), hosts.end(), host) == hosts.end()) return std::nullopt;
    // Use verification_uri as returned. Inventing an OTC prefill URL can fail
    // with a first-party consent error before Microsoft's code-entry page opens.
    // The user enters the code displayed by SignInStage::Waiting instead.
    return std::string(uri);
}

SignInResult sign_in(SignInHost& host, std::string_view client_id, std::int64_t now_ms) {
    if (client_id.empty()) return failed("not_configured");
    host.stage(SignInStage::Starting, {});
    const std::string client = "client_id=" + form_encode(client_id);
    // login.live.com's device endpoint wants an explicit response_type.
    const auto code_reply = host.post_form(msa_host, device_code_path,
                                           client + "&scope=" + form_encode(scope) + "&response_type=device_code");
    if (code_reply.status == 0) return failed("network");
    if (code_reply.status != 200) return failed("rejected");
    const auto code = parse_object(code_reply.body);
    if (!code) return failed("bad_response");
    const std::string device_code = text_field(*code, "device_code");
    const std::string user_code = text_field(*code, "user_code");
    const auto url = login_url(text_field(*code, "verification_uri"), user_code);
    if (device_code.empty() || !url) return failed("bad_response");
    int interval = std::clamp(int_field(*code, "interval", default_interval), 1, max_interval);
    int remaining = int_field(*code, "expires_in", 900);

    const WindowScope window{host};
    const bool copied = host.copy_code(user_code);
    // Publish before browser activation: the game may stop presenting while unfocused.
    host.stage(SignInStage::Waiting, user_code, copied, false);
    const bool opened = host.open_window(*url);
    host.stage(SignInStage::Waiting, user_code, copied, opened);
    const std::string poll = "grant_type=" + form_encode("urn:ietf:params:oauth:grant-type:device_code") + "&" + client +
                             "&device_code=" + form_encode(device_code);
    std::string access_token, refresh_token;
    while (access_token.empty()) {
        // Browser launchers can exit after handing off to another process. Keep
        // the device code available until Cancel or expiry, also for phone sign-in.
        for (int i = 0; i < interval; ++i) {
            if (remaining-- <= 0) return failed("expired");
            if (!host.wait(1s)) return cancelled();
        }
        const auto reply = host.post_form(msa_host, token_path, poll);
        if (reply.status == 0) continue; // a dropped connection; the code is still good
        const auto document = parse_object(reply.body);
        if (!document) return failed("bad_response");
        if (reply.status == 200) {
            access_token = text_field(*document, "access_token");
            refresh_token = text_field(*document, "refresh_token");
            if (access_token.empty()) return failed("bad_response");
            break;
        }
        const std::string error = text_field(*document, "error");
        if (error == "authorization_pending") continue;
        if (error == "slow_down") {
            interval = std::min(interval + 5, max_interval);
            continue;
        }
        if (error == "authorization_declined") return failed("declined");
        if (error == "expired_token" || error == "bad_verification_code") return failed("expired");
        return failed("rejected");
    }
    host.close_window();
    host.stage(SignInStage::Finishing, {});
    return xbox_profile(host, access_token, refresh_token, now_ms);
}

std::string serialize_accounts(const std::vector<Credentials>& accounts) {
    json list = json::array();
    for (const auto& entry : accounts) {
        list.push_back({{"xuid", entry.account.xuid},
                        {"gamertag", entry.account.gamertag},
                        {"addedAt", entry.account.added_at},
                        {"refreshToken", entry.refresh_token}});
    }
    return json{{"version", 1}, {"accounts", std::move(list)}}.dump();
}

std::optional<std::vector<Credentials>> parse_accounts(std::string_view text) {
    const auto document = parse_object(text);
    if (!document) return std::nullopt;
    const auto list = document->find("accounts");
    if (list == document->end() || !list->is_array()) return std::nullopt;
    std::vector<Credentials> accounts;
    for (const auto& entry : *list) {
        if (!entry.is_object()) return std::nullopt;
        Credentials credentials;
        credentials.account.xuid = text_field(entry, "xuid");
        credentials.account.gamertag = text_field(entry, "gamertag");
        const auto added = entry.find("addedAt");
        if (added != entry.end() && added->is_number_integer()) credentials.account.added_at = added->get<std::int64_t>();
        credentials.refresh_token = text_field(entry, "refreshToken");
        if (credentials.account.xuid.empty()) return std::nullopt;
        accounts.push_back(std::move(credentials));
    }
    return accounts;
}

void upsert_account(std::vector<Credentials>& accounts, Credentials account) {
    const auto found = std::find_if(accounts.begin(), accounts.end(), [&](const Credentials& entry) {
        return entry.account.xuid == account.account.xuid;
    });
    if (found == accounts.end()) accounts.push_back(std::move(account));
    else *found = std::move(account);
}

} // namespace medicon::alt
