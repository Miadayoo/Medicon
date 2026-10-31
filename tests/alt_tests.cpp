#include <medicon/alt/Auth.hpp>
#include <medicon/alt/Session.hpp>

#include <nlohmann/json.hpp>

#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace medicon::alt;
using json = nlohmann::json;

int checks = 0;
void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

constexpr const char* code_page =
    R"({"device_code":"device-secret","user_code":"ABCD1234","verification_uri":"https://www.microsoft.com/link","expires_in":900,"interval":2})";
constexpr const char* token_page = R"({"access_token":"access-secret","refresh_token":"refresh-secret"})";
constexpr const char* user_page = R"({"Token":"user-token","DisplayClaims":{"xui":[{"uhs":"123"}]}})";
constexpr const char* xsts_page = R"({"Token":"xsts","DisplayClaims":{"xui":[{"uhs":"123","xid":"2535400000000000","gtg":"Mia Alt"}]}})";

// Replies are consumed in order; the sign-in never touches a real server.
class FakeHost final : public SignInHost {
public:
    std::deque<HttpReply> forms, jsons, profiles;
    std::vector<std::string> form_hosts, form_bodies, json_hosts, json_bodies;
    std::string authorized_body, authorization;
    std::vector<SignInStage> stages;
    std::vector<SignInProgress> progress;
    std::string opened, code, copied_code;
    std::string profile_hash, profile_token;
    int seconds_waited = 0;
    int cancel_after = -1;     // wait() fails after this many seconds
    int closes = 0;
    bool window_opens = true;
    bool clipboard_works = true;
    bool code_ready_before_browser = false;

    HttpReply post_form(std::string_view host, std::string_view, std::string_view body) override {
        form_hosts.emplace_back(host);
        form_bodies.emplace_back(body);
        return take(forms);
    }
    HttpReply post_json(std::string_view host, std::string_view, std::string_view body) override {
        json_hosts.emplace_back(host);
        json_bodies.emplace_back(body);
        return take(jsons);
    }
    HttpReply get_profile(std::string_view user_hash, std::string_view xsts_token) override {
        profile_hash = user_hash;
        profile_token = xsts_token;
        return take(profiles);
    }
    HttpReply post_authorized_json(std::string_view host, std::string_view path, std::string_view body,
                                   std::string_view auth) override {
        check(host == "authorization.franchise.minecraft-services.net" && path == "/api/v1.0/multiplayer/session/start",
              "server credentials reach only the Minecraft authorization service");
        authorized_body = body;
        authorization = auth;
        return take(jsons);
    }
    bool copy_code(std::string_view user_code) override {
        copied_code = user_code;
        return clipboard_works;
    }
    bool open_window(const std::string& url) override {
        code_ready_before_browser = !code.empty() && copied_code == code && stages.back() == SignInStage::Waiting;
        opened = url;
        return window_opens;
    }
    void close_window() override { ++closes; }
    bool wait(std::chrono::seconds duration) override {
        seconds_waited += static_cast<int>(duration.count());
        return cancel_after < 0 || seconds_waited < cancel_after;
    }
    void stage(SignInStage stage, std::string_view user_code, bool code_copied, bool window_opened) override {
        stages.push_back(stage);
        progress.push_back({stage, std::string(user_code), code_copied, window_opened});
        if (!user_code.empty()) code = user_code;
    }

private:
    static HttpReply take(std::deque<HttpReply>& replies) {
        if (replies.empty()) throw std::runtime_error("an unexpected request");
        auto reply = std::move(replies.front());
        replies.pop_front();
        return reply;
    }
};

void test_full_sign_in() {
    FakeHost host;
    host.forms = {{200, code_page}, {400, R"({"error":"authorization_pending"})"}, {0, ""},
                  {400, R"({"error":"slow_down"})"}, {200, token_page}};
    host.jsons = {{200, user_page}, {200, xsts_page}};
    const auto result = sign_in(host, "client-id", 42);
    check(result.credentials.has_value() && result.error.empty(), "the whole flow succeeds");
    check(result.credentials->account == Account{"2535400000000000", "Mia Alt", 42}, "gamertag and XUID come from XSTS");
    check(result.credentials->refresh_token == "refresh-secret", "the refresh token is kept");
    check(host.opened == "https://www.microsoft.com/link", "the window uses the verification URI without an invented prefill parameter");
    check(host.code == "ABCD1234", "the code is reported to the screen for manual entry");
    check(host.code_ready_before_browser, "the code is published and copied before browser activation");
    check(host.progress[1].code_copied && !host.progress[1].window_opened && host.progress[2].window_opened,
          "clipboard and browser results reach the screen separately");
    check((host.stages == std::vector{SignInStage::Starting, SignInStage::Waiting, SignInStage::Waiting, SignInStage::Finishing}), "stages in order");
    check(host.form_hosts[0] == "login.live.com", "the Xbox-capable MSA endpoint gets the device request");
    check(host.form_bodies[0].find("client_id=client-id") != std::string::npos, "the configured app is used");
    check(host.form_bodies[0].find("scope=XboxLive.signin%20offline_access") != std::string::npos, "the scope is encoded");
    check(host.form_bodies[0].find("response_type=device_code") != std::string::npos, "the device response_type is set");
    check(host.form_bodies[1].find("device_code=device-secret") != std::string::npos, "the poll carries the device code");
    check(host.seconds_waited == 2 + 2 + 2 + 7, "polls keep the interval; slow_down adds 5 s");
    check(host.json_hosts == std::vector<std::string>({"user.auth.xboxlive.com", "xsts.auth.xboxlive.com"}), "Xbox Live in order");
    check(json::parse(host.json_bodies[0])["Properties"]["RpsTicket"] == "d=access-secret", "the user token uses the d= ticket");
    check(json::parse(host.json_bodies[1])["Properties"]["UserTokens"][0] == "user-token", "XSTS gets the user token");
    check(json::parse(host.json_bodies[1])["RelyingParty"] == "http://xboxlive.com",
          "XSTS uses the service RP accepted by the Xbox profile API");
    check(host.closes >= 1, "the window closes once the code is exchanged");
}

void test_configuration_and_first_request() {
    FakeHost none;
    check(sign_in(none, "", 0).error == "not_configured", "no client id: nothing is sent");
    check(none.form_hosts.empty() && none.opened.empty(), "and no window opens");

    FakeHost offline;
    offline.forms = {{0, ""}};
    check(sign_in(offline, "id", 0).error == "network", "no response");
    FakeHost refused;
    refused.forms = {{400, R"({"error":"invalid_client"})"}};
    check(sign_in(refused, "id", 0).error == "rejected", "Microsoft refuses the app");
    FakeHost blocked;
    blocked.forms = {{200, code_page}, {200, token_page}};
    blocked.jsons = {{200, user_page}, {200, xsts_page}};
    blocked.window_opens = false;
    check(sign_in(blocked, "id", 0).credentials.has_value(), "a browser launch failure keeps the code usable on another device");
    check(!blocked.progress[2].window_opened && blocked.progress[2].user_code == "ABCD1234",
          "a failed launch leaves the code and reopen action available");
    FakeHost clipboard_busy;
    clipboard_busy.forms = {{200, code_page}};
    clipboard_busy.clipboard_works = false;
    clipboard_busy.cancel_after = 1;
    check(sign_in(clipboard_busy, "id", 0).cancelled, "clipboard failure does not discard the sign-in session");
    check(!clipboard_busy.progress[2].code_copied && clipboard_busy.progress[2].user_code == "ABCD1234",
          "clipboard failure is reported truthfully alongside the visible code");
    FakeHost foreign;
    foreign.forms = {{200, R"({"device_code":"d","user_code":"AB","verification_uri":"https://evil.example/link"})"}};
    check(sign_in(foreign, "id", 0).error == "bad_response" && foreign.opened.empty(), "a foreign page is never opened");
}

void test_waiting_ends() {
    FakeHost cancelled;
    cancelled.forms = {{200, code_page}, {400, R"({"error":"authorization_pending"})"}};
    cancelled.cancel_after = 3;
    const auto first = sign_in(cancelled, "id", 0);
    check(first.cancelled && first.error.empty() && !first.credentials, "Cancel ends the wait");
    check(cancelled.closes == 1, "and closes the window");

    FakeHost declined;
    declined.forms = {{200, code_page}, {400, R"({"error":"authorization_declined"})"}};
    check(sign_in(declined, "id", 0).error == "declined", "the user declined");

    FakeHost expired;
    expired.forms = {{200, code_page}, {400, R"({"error":"expired_token"})"}};
    check(sign_in(expired, "id", 0).error == "expired", "the code expired on the server");

    FakeHost timed_out;
    timed_out.forms = {{200, R"({"device_code":"d","user_code":"AB","verification_uri":"https://www.microsoft.com/link","expires_in":3,"interval":2})"},
                       {400, R"({"error":"authorization_pending"})"}};
    check(sign_in(timed_out, "id", 0).error == "expired", "expires_in bounds the wait");
}

void test_xbox_failures() {
    const auto after_token = [](std::vector<HttpReply> replies) {
        FakeHost host;
        host.forms = {{200, code_page}, {200, token_page}};
        host.jsons.assign(replies.begin(), replies.end());
        return sign_in(host, "id", 0).error;
    };
    check(after_token({{401, ""}}) == "xbox_user_failed", "user.auth refuses");
    check(after_token({{0, ""}}) == "network", "user.auth unreachable");
    check(after_token({{200, "not json"}}) == "bad_response", "user.auth garbage");
    check(after_token({{200, user_page}, {401, R"({"XErr":2148916233})"}}) == "no_xbox_profile", "no Xbox profile");
    check(after_token({{200, user_page}, {401, R"({"XErr":2148916235})"}}) == "region_blocked", "blocked region");
    check(after_token({{200, user_page}, {401, R"({"XErr":2148916238})"}}) == "child_account", "child account");
    check(after_token({{200, user_page}, {401, "{}"}}) == "xsts_failed", "unknown refusal");
    check(after_token({{200, user_page}, {200, R"({"DisplayClaims":{"xui":[]}})"}}) == "bad_response", "no claims");
    check(after_token({{200, user_page}, {200, R"({"DisplayClaims":{"xui":[{"uhs":"1"}]}})"}}) == "bad_response",
          "a response without the XSTS token cannot be used to call the profile API");
}

void test_profile_lookup_and_diagnostics() {
    const auto attempt = [](HttpReply profile, std::string_view claims = R"({"uhs":"123"})") {
        FakeHost host;
        host.forms = {{200, code_page}, {200, token_page}};
        host.jsons = {{200, user_page}, {200, json{{"Token", "xsts-secret"},
            {"DisplayClaims", {{"xui", json::array({json::parse(claims)})}}}}.dump()}};
        host.profiles = {std::move(profile)};
        const auto result = sign_in(host, "id", 123);
        check(host.profile_hash == "123" && host.profile_token == "xsts-secret", "profile lookup uses the XSTS credentials");
        return result;
    };
    const auto linked = attempt({200, R"({"profileUsers":[{"id":"2535400000000000","settings":[{"id":"Gamertag","value":"Mia Alt"}]}]})"});
    check(linked.credentials && linked.credentials->account == Account{"2535400000000000", "Mia Alt", 123},
          "a token with only uhs still links the account using the profile API");
    check(linked.credentials->refresh_token == "refresh-secret", "profile lookup preserves the refresh token");
    const auto denied = attempt({403, R"({"Message":"sensitive-remote-message","Token":"sensitive-token"})"});
    check(denied.error == "profile_failed" && denied.diagnostic == "Xbox profile: HTTP 403", "profile rejection names the failing stage and HTTP status only");
    check(attempt({200, R"({"profileUsers":[]})"}).error == "profile_unavailable", "empty profile data is distinct from no Xbox account");
    check(attempt({200, "not json"}).diagnostic == "Xbox profile: invalid response", "invalid profile JSON is identified");
    check(attempt({200, R"({"profileUsers":[{"id":"2","settings":[{"id":"Gamertag","value":"Other Account"}]}]})"},
                  R"({"uhs":"123","xid":"1"})").diagnostic == "Xbox profile: account identity mismatch", "another account's profile is rejected");
    check(attempt({200, R"({"profileUsers":[{"id":"not-a-number","settings":[{"id":"Gamertag","value":"Name"}]}]})"}).error == "profile_unavailable", "malformed XUIDs are rejected");

    const auto rejected = [](int status, std::string body) {
        FakeHost host;
        host.forms = {{200, code_page}, {200, token_page}};
        host.jsons = {{200, user_page}, {status, std::move(body)}};
        return sign_in(host, "id", 0);
    };
    const auto observed = rejected(400, "");
    check(observed.error == "xsts_failed" && observed.diagnostic == "Xbox token: HTTP 400",
          "the observed XSTS 400 is not reported as a missing Xbox profile");
    const auto unknown = rejected(400, R"({"XErr":2148916227,"Message":"sensitive-remote-message"})");
    check(unknown.diagnostic == "Xbox token: HTTP 400, XErr 2148916227", "unknown Xbox errors retain the numeric code without remote messages");
    check(rejected(400, R"({"XErr":"2148916233"})").error == "no_xbox_profile", "known Xbox errors are read from string codes and non-401 responses");
    check(rejected(400, R"({"XErr":"token-secret\r\n"})").diagnostic == "Xbox token: HTTP 400", "non-numeric error text never enters diagnostics");
    check(rejected(400, R"({"XErr":18446744073709551615})").diagnostic == "Xbox token: HTTP 400", "oversized Xbox codes do not overflow");
}

void test_login_url() {
    check(login_url("https://www.microsoft.com/link", "ABC123") == "https://www.microsoft.com/link", "the usual page is unchanged: OTC prefill can cause first-party consent errors");
    check(login_url("https://login.microsoftonline.com/common/oauth2/deviceauth", "A") ==
              "https://login.microsoftonline.com/common/oauth2/deviceauth", "the tenant page is unchanged too");
    check(login_url("https://login.live.com/oauth20_remoteconnect.srf", "ABC123") ==
              "https://login.live.com/oauth20_remoteconnect.srf", "the consumer page is unchanged too");
    check(!login_url("http://www.microsoft.com/link", "A"), "https only");
    check(!login_url("https://microsoft.com.evil.example/link", "A"), "suffix tricks");
    check(!login_url("https://www.microsoft.com@evil.example/", "A"), "userinfo tricks");
    check(!login_url("https://www.microsoft.com/\" --gpu-launcher=calc", "A"), "nothing leaves the command line's quotes");
    check(!login_url("https://www.microsoft.com/link", "A\" --x"), "the code is checked too");
    check(!login_url("https://www.microsoft.com/link", ""), "no code");
    check(form_encode("a b+c/é") == "a%20b%2Bc%2F%C3%A9", "form encoding");
}

void test_account_file() {
    std::vector<Credentials> accounts;
    upsert_account(accounts, {{"1", "One", 5}, "r1"});
    upsert_account(accounts, {{"2", "Two", 6}, "r2"});
    upsert_account(accounts, {{"1", "One New", 7}, "r3"});
    check(accounts.size() == 2 && accounts[0].account.gamertag == "One New" && accounts[0].refresh_token == "r3",
          "signing in again replaces the account");
    const auto loaded = parse_accounts(serialize_accounts(accounts));
    check(loaded && loaded->size() == 2, "round trip");
    check((*loaded)[1].account == Account{"2", "Two", 6} && (*loaded)[1].refresh_token == "r2", "every field survives");
    check(!parse_accounts("not json") && !parse_accounts("{}") && !parse_accounts(R"({"accounts":[{}]})"),
          "a broken file is refused, not read as empty");
}

std::string encode64(std::string_view text) {
    constexpr char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    unsigned bits = 0, count = 0;
    std::string out;
    for (unsigned char c : text) {
        bits = (bits << 8) | c;
        count += 8;
        while (count >= 6) { count -= 6; out += chars[(bits >> count) & 63]; }
    }
    if (count) out += chars[(bits << (6 - count)) & 63];
    return out;
}
std::string test_jwt(std::string key, int expiry = 5000) {
    return "e30." + encode64(json{{"cpk", key}, {"exp", expiry}, {"xid", "2535400000000000"},
        {"iss", "https://authorization.franchise.minecraft-services.net/"},
        {"aud", "api://auth-minecraft-services/multiplayer"}}.dump()) + ".fixture-signature";
}

void test_server_session() {
    FakeHost host;
    host.forms = {{200, token_page}};
    host.jsons = {{200, user_page}, {200, xsts_page}, {200, R"({"data":{"SessionTicket":"pfb-secret",
        "InfoResultPayload":{"UserInfo":{"XboxInfo":{"XboxUserId":"2535400000000000"}}}}})"},
        {200, R"({"result":{"authorizationHeader":"MCToken session-secret"}})"}};
    Credentials credentials{{"2535400000000000", "Mia Alt", 0}, "old-refresh"};
    auto result = prepare_session(host, credentials, "our-app", "device-guid", "1.26.52");
    check(result.authorization == "MCToken session-secret" && result.error.empty(), "refresh through Xbox/PlayFab/Minecraft succeeds");
    check(credentials.refresh_token == "refresh-secret", "refresh-token rotation is retained");
    check(host.form_bodies[0].find("grant_type=refresh_token") != std::string::npos, "Use refreshes the stored credential");
    check(json::parse(host.json_bodies[1])["RelyingParty"] == "https://b980a380.minecraft.playfabapi.com/",
          "Minecraft gets a separate PlayFab RP token, not the profile token");
    check(json::parse(host.json_bodies[2])["XboxToken"] == "XBL3.0 x=123;xsts", "PlayFab receives the Xbox service token");
    check(json::parse(host.json_bodies[3])["user"]["token"] == "pfb-secret", "Minecraft session uses the PlayFab ticket");

    FakeHost mismatch;
    mismatch.forms = {{200, token_page}};
    mismatch.jsons = {{200, user_page}, {200, xsts_page}, {200, R"({"data":{"SessionTicket":"ticket",
        "InfoResultPayload":{"UserInfo":{"XboxInfo":{"XboxUserId":"999"}}}}})"}};
    check(prepare_session(mismatch, credentials, "app", "id", "1.26.52").authorization.empty(), "a mismatched PlayFab account cannot be activated");
    FakeHost failed;
    failed.forms = {{200, token_page}};
    failed.jsons = {{403, "server body containing a secret"}};
    result = prepare_session(failed, credentials, "app", "id", "1.26.52");
    check(result.authorization.empty() && result.diagnostic == "Xbox user auth: HTTP 403", "failures expose only stage and status");

    // The original private key stays in Minecraft. Request a newly signed
    // credential for exactly its public key, then validate the returned binding.
    const std::string key(160, 'A'), other_key(160, 'B');
    const auto native = test_jwt(key);
    FakeHost token_host;
    token_host.jsons = {{200, json{{"result", {{"signedToken", test_jwt(key)}}}}.dump()}};
    auto server = authorize_connection(token_host, "MCToken session-secret", native, 1000);
    check(!server.token.empty() && token_host.authorization == "MCToken session-secret", "the selected session issues the server credential");
    check(json::parse(token_host.authorized_body)["publicKey"] == key, "the native public key is preserved exactly");
    check(server_token_xuid(server.token) == credentials.account.xuid, "the server credential carries the linked identity");
    token_host.jsons = {{200, json{{"result", {{"signedToken", test_jwt(other_key)}}}}.dump()}};
    check(authorize_connection(token_host, "auth", native, 1000).token.empty(), "another signing key is rejected");
    token_host.jsons = {{200, json{{"signedToken", test_jwt(key, 1010)}}.dump()}};
    check(authorize_connection(token_host, "auth", native, 1000).token.empty(), "an expired or nearly expired credential is rejected");
    check(!server_public_key("invalid.jwt") && !server_public_key("e30.e30.x"), "missing native binding fails closed");
    check(native_multiplayer_url("https://authorization.franchise.minecraft-services.net/api/v1.0/multiplayer/session/start"),
          "native authorization reaches the exact multiplayer issuance endpoint");
    for (const std::string_view url : {"http://authorization.franchise.minecraft-services.net/api/v1.0/multiplayer/session/start",
        "https://authorization.franchise.minecraft-services.net.attacker.test/api/v1.0/multiplayer/session/start",
        "https://authorization.franchise.minecraft-services.net@attacker.test/api/v1.0/multiplayer/session/start",
        "https://authorization.franchise.minecraft-services.net/api/v1.0/session/start",
        "https://authorization.franchise.minecraft-services.net/api/v1.0/multiplayer/session/start?redirect=https://attacker.test",
        "https://server.example/v1/join/123"})
        check(!native_multiplayer_url(url), "service credentials never route to signaling, other endpoints, or lookalike hosts");
    check(valid_server_token(native, 1000) && !valid_server_token(native, 4990), "native token validation checks expiry before login");
}

} // namespace

int main() {
    try {
        test_full_sign_in();
        test_configuration_and_first_request();
        test_waiting_ends();
        test_xbox_failures();
        test_profile_lookup_and_diagnostics();
        test_login_url();
        test_account_file();
        test_server_session();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << checks << " checks passed\n";
    return 0;
}
