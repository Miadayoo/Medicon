#pragma once

#include "medicon/alt/Alt.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace medicon::alt {

struct HttpReply {
    int status = 0; // 0 when no response arrived
    std::string body;
};

// What sign_in() needs from the outside world; faked in tests.
class SignInHost {
public:
    virtual ~SignInHost() = default;
    virtual HttpReply post_form(std::string_view host, std::string_view path, std::string_view body) = 0;
    virtual HttpReply post_json(std::string_view host, std::string_view path, std::string_view body) = 0;
    virtual HttpReply post_authorized_json(std::string_view, std::string_view, std::string_view, std::string_view) { return {}; }
    // Uses an Xbox service XSTS token to read the signed-in user's profile.
    virtual HttpReply get_profile(std::string_view user_hash, std::string_view xsts_token) = 0;
    // Copies only the human-readable verification code, before opening the browser.
    virtual bool copy_code(std::string_view user_code) = 0;
    // Shows Microsoft's own sign-in page. False when nothing could be opened.
    virtual bool open_window(const std::string& url) = 0;
    virtual void close_window() = 0;
    // Sleeps; false when the sign-in was cancelled meanwhile.
    virtual bool wait(std::chrono::seconds duration) = 0;
    virtual void stage(SignInStage stage, std::string_view user_code, bool code_copied = false,
                       bool window_opened = true) = 0;
};

struct Credentials {
    Account account;
    std::string refresh_token;
};

struct SignInResult {
    std::optional<Credentials> credentials;
    bool cancelled = false;
    // not_configured, network, rejected, bad_response, window, declined, expired,
    // no_xbox_profile, profile_unavailable, child_account, region_blocked,
    // xbox_user_failed, xsts_failed, profile_failed
    std::string error;
    // Locally formatted stage / HTTP status / numeric XErr only. Never a body or token.
    std::string diagnostic = {};
};

// The OAuth device flow on Microsoft's consumer tenant, then Xbox Live for the
// gamertag and XUID. The password only ever reaches Microsoft's page: this
// process sees the code and the tokens it is exchanged for.
SignInResult sign_in(SignInHost& host, std::string_view client_id, std::int64_t now_ms);

// Validates verification_uri and the separately displayed user code. Returns
// the URI unchanged; do not invent an OTC prefill query. nullopt unless it is
// an https URL on a Microsoft sign-in host with a plain code: the result reaches
// a command line, so nothing a server sent may break out of its quotes.
std::optional<std::string> login_url(std::string_view verification_uri, std::string_view user_code);
std::string form_encode(std::string_view text);

// The document sealed into alts.bin.
std::string serialize_accounts(const std::vector<Credentials>& accounts);
std::optional<std::vector<Credentials>> parse_accounts(std::string_view text);
// Replaces the account with the same XUID, or adds it.
void upsert_account(std::vector<Credentials>& accounts, Credentials account);

} // namespace medicon::alt
