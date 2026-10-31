#include <medicon/alt/Session.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>

namespace medicon::alt {
namespace {
using json = nlohmann::json;
constexpr std::string_view services = "authorization.franchise.minecraft-services.net";

json object(std::string_view value) {
    if (value.size() > 131072) return json::object();
    auto result = json::parse(value, nullptr, false);
    return result.is_object() ? result : json::object();
}
std::string field(const json& value, const char* key) {
    const auto it = value.find(key);
    return it != value.end() && it->is_string() ? it->get<std::string>() : std::string{};
}
bool safe_header(std::string_view value) {
    return !value.empty() && value.size() < 65536 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= 32 && c < 127;
    });
}
std::optional<std::string> unbase64(std::string_view value) {
    if (value.empty() || value.size() > 65536) return std::nullopt;
    std::string out;
    unsigned bits = 0, count = 0;
    bool padding = false;
    for (const char c : value) {
        if (c == '=') { padding = true; continue; }
        if (padding) return std::nullopt;
        int n = -1;
        if (c >= 'A' && c <= 'Z') n = c - 'A';
        else if (c >= 'a' && c <= 'z') n = c - 'a' + 26;
        else if (c >= '0' && c <= '9') n = c - '0' + 52;
        else if (c == '+' || c == '-') n = 62;
        else if (c == '/' || c == '_') n = 63;
        if (n < 0) return std::nullopt;
        bits = (bits << 6) | static_cast<unsigned>(n);
        count += 6;
        if (count >= 8) { count -= 8; out.push_back(static_cast<char>((bits >> count) & 255)); }
    }
    return out;
}
std::string base64(std::string_view value) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned bits = 0, count = 0;
    for (unsigned char c : value) {
        bits = (bits << 8) | c;
        count += 8;
        while (count >= 6) { count -= 6; out.push_back(alphabet[(bits >> count) & 63]); }
    }
    if (count) out.push_back(alphabet[(bits << (6 - count)) & 63]);
    while (out.size() % 4) out.push_back('=');
    return out;
}
json claims(std::string_view token) {
    const auto first = token.find('.');
    const auto last = token.find('.', first == token.npos ? 0 : first + 1);
    if (first == token.npos || last == token.npos || token.find('.', last + 1) != token.npos || last + 1 >= token.size())
        return json::object();
    const auto decoded = unbase64(token.substr(first + 1, last - first - 1));
    return decoded ? object(*decoded) : json::object();
}
SessionResult failure(const HttpReply& reply, const char* stage) {
    return {{}, "session_failed", std::string(stage) + ": HTTP " + std::to_string(reply.status)};
}
} // namespace

SessionResult prepare_session(SignInHost& host, Credentials& account, std::string_view application_id,
                              std::string_view device_id, std::string_view game_version) {
    auto reply = host.post_form("login.live.com", "/oauth20_token.srf", "client_id=" + form_encode(application_id) +
        "&grant_type=refresh_token&refresh_token=" + form_encode(account.refresh_token) + "&scope=XboxLive.signin%20offline_access");
    if (reply.status != 200) return failure(reply, "Microsoft refresh");
    auto data = object(reply.body);
    const auto access = field(data, "access_token");
    if (access.empty()) return {{}, "bad_response", "Microsoft refresh: missing access token"};
    if (auto refresh = field(data, "refresh_token"); !refresh.empty()) account.refresh_token = std::move(refresh);

    reply = host.post_json("user.auth.xboxlive.com", "/user/authenticate", json{
        {"RelyingParty", "http://auth.xboxlive.com"}, {"TokenType", "JWT"}, {"Properties", {
            {"AuthMethod", "RPS"}, {"SiteName", "user.auth.xboxlive.com"}, {"RpsTicket", "d=" + access}}}}.dump());
    if (reply.status != 200) return failure(reply, "Xbox user auth");
    const auto user = field(object(reply.body), "Token");
    if (user.empty()) return {{}, "bad_response", "Xbox user auth: missing token"};
    reply = host.post_json("xsts.auth.xboxlive.com", "/xsts/authorize", json{
        {"RelyingParty", "https://b980a380.minecraft.playfabapi.com/"}, {"TokenType", "JWT"},
        {"Properties", {{"SandboxId", "RETAIL"}, {"UserTokens", json::array({user})}}}}.dump());
    if (reply.status != 200) return failure(reply, "Xbox PlayFab token");
    data = object(reply.body);
    const auto token = field(data, "Token");
    const auto display = data.value("DisplayClaims", json::object());
    const auto users = display.is_object() ? display.value("xui", json::array()) : json::array();
    const auto hash = users.is_array() && users.size() == 1 ? field(users[0], "uhs") : std::string{};
    if (!safe_header(hash) || hash.find(';') != hash.npos || !safe_header(token))
        return {{}, "bad_response", "Xbox PlayFab token: incomplete response"};
    reply = host.post_json("20ca2.playfabapi.com", "/Client/LoginWithXbox", json{
        {"TitleId", "20CA2"}, {"CreateAccount", true}, {"XboxToken", "XBL3.0 x=" + hash + ";" + token},
        {"InfoRequestParameters", {{"GetUserAccountInfo", true}, {"GetPlayerProfile", true}}}}.dump());
    if (reply.status != 200) return failure(reply, "PlayFab login");
    data = object(reply.body).value("data", json::object());
    if (!data.is_object()) return {{}, "bad_response", "PlayFab login: invalid response"};
    const auto ticket = field(data, "SessionTicket");
    // Verify the selected identity before issuing a server session. Never use
    // another account's token after an expired or mismatched credential.
    const auto info = data.value("InfoResultPayload", json::object());
    const auto user_info = info.is_object() ? info.value("UserInfo", json::object()) : json::object();
    const auto xbox = user_info.is_object() ? user_info.value("XboxInfo", json::object()) : json::object();
    // Some title profiles omit XboxInfo. The server token's required xid is
    // checked again against the selected account before it can be serialized.
    const auto playfab_xuid = xbox.is_object() ? field(xbox, "XboxUserId") : std::string{};
    if ((!playfab_xuid.empty() && playfab_xuid != account.account.xuid) || ticket.empty())
        return {{}, "bad_response", "PlayFab login: account identity mismatch"};
    reply = host.post_json(services, "/api/v1.0/session/start", json{
        {"device", {{"applicationType", "MinecraftPE"}, {"gameVersion", game_version}, {"id", device_id},
            {"memory", "8589934592"}, {"platform", "Windows10"}, {"playFabTitleId", "20CA2"},
            {"storePlatform", "uwp.store"}, {"type", "Windows10"}}},
        {"user", {{"token", ticket}, {"tokenType", "PlayFab"}}}}.dump());
    if (reply.status != 200) return failure(reply, "Minecraft session");
    data = object(reply.body).value("result", json::object());
    const auto auth = data.is_object() ? field(data, "authorizationHeader") : std::string{};
    if (!safe_header(auth)) return {{}, "bad_response", "Minecraft session: missing authorization"};
    return {auth, {}, {}};
}

std::optional<std::string> server_public_key(std::string_view token) {
    const auto data = claims(token);
    const auto found = data.find("cpk");
    if (found == data.end()) return std::nullopt;
    if (found->is_string()) {
        const auto raw = unbase64(found->get_ref<const std::string&>());
        if (raw && raw->size() >= 64 && raw->size() <= 512) return base64(*raw);
    } else if (found->is_object() && field(*found, "kty") == "EC" && field(*found, "crv") == "P-384") {
        const auto x = unbase64(field(*found, "x")), y = unbase64(field(*found, "y"));
        if (!x || !y || x->size() != 48 || y->size() != 48) return std::nullopt;
        constexpr unsigned char prefix[] = {0x30,0x76,0x30,0x10,0x06,0x07,0x2a,0x86,0x48,0xce,0x3d,0x02,0x01,
            0x06,0x05,0x2b,0x81,0x04,0x00,0x22,0x03,0x62,0x00,0x04};
        return base64(std::string(reinterpret_cast<const char*>(prefix), sizeof(prefix)) + *x + *y);
    }
    return std::nullopt;
}

std::string server_token_xuid(std::string_view token) {
    const auto data = claims(token);
    auto xuid = field(data, "xuid");
    if (xuid.empty()) xuid = field(data, "xid");
    if (xuid.empty()) {
        const auto extra = data.value("extraData", json::object());
        if (extra.is_object()) xuid = field(extra, "XUID");
    }
    if (!std::all_of(xuid.begin(), xuid.end(), [](char c) { return c >= '0' && c <= '9'; })) return {};
    return xuid;
}

ServerTokenResult authorize_connection(SignInHost& host, std::string_view authorization,
                                      std::string_view native_token, std::int64_t now_seconds) {
    const auto key = server_public_key(native_token);
    if (!key) return {{}, "connection_failed", "Server login: native public key unavailable"};
    const auto reply = host.post_authorized_json(services, "/api/v1.0/multiplayer/session/start",
                                                json{{"publicKey", *key}}.dump(), authorization);
    if (reply.status != 200) return {{}, "connection_failed", "Minecraft multiplayer: HTTP " + std::to_string(reply.status)};
    const auto data = object(reply.body);
    const auto result = data.value("result", json::object());
    auto token = result.is_object() ? field(result, "signedToken") : std::string{};
    if (token.empty()) token = field(data, "signedToken");
    const auto returned_key = server_public_key(token);
    if (!returned_key || *returned_key != *key || !valid_server_token(token, now_seconds))
        return {{}, "connection_failed", "Minecraft multiplayer: invalid key binding or expiry"};
    return {std::move(token), {}, {}};
}

bool valid_server_token(std::string_view token, std::int64_t now_seconds) {
    const auto decoded = claims(token);
    const auto expiry = decoded.find("exp");
    const auto not_before = decoded.find("nbf");
    return server_public_key(token).has_value() && !server_token_xuid(token).empty() &&
        expiry != decoded.end() && expiry->is_number_integer() && expiry->get<std::int64_t>() > now_seconds + 30 &&
        (not_before == decoded.end() || (not_before->is_number_integer() && not_before->get<std::int64_t>() <= now_seconds + 5)) &&
        field(decoded, "iss") == "https://authorization.franchise.minecraft-services.net/" &&
        field(decoded, "aud") == "api://auth-minecraft-services/multiplayer";
}

bool native_multiplayer_url(std::string_view url) noexcept {
    return url == "https://authorization.franchise.minecraft-services.net/api/v1.0/multiplayer/session/start";
}
} // namespace medicon::alt
