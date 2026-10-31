#pragma once

#include "medicon/social/Events.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace medicon::alt {

// The Microsoft app alts sign in through, embedded at build time
// (-DMEDICON_MSA_CLIENT_ID=...). Empty: sign-in reports "not_configured".
inline constexpr std::string_view client_id = MEDICON_MSA_CLIENT_ID;

// What the screen shows of an account. Its tokens stay with the service.
struct Account {
    std::string xuid;
    std::string gamertag;
    std::int64_t added_at = 0; // Unix milliseconds
    bool operator==(const Account&) const = default;
};

enum class SignInStage { Starting, Waiting, Finishing };

// Service -> render thread.
struct AccountsChanged {
    std::vector<Account> accounts;
    bool load_failed = false; // alts.bin exists but could not be read
};
struct SignInProgress {
    SignInStage stage = SignInStage::Starting;
    std::string user_code; // shown while waiting, for entry on Microsoft's verification page
    bool code_copied = false;
    bool window_opened = true;
};
struct SignInFinished {
    bool ok = false;
    bool cancelled = false;
    std::string error; // a code from sign_in(); never holds a token
    std::string gamertag;
    std::string diagnostic = {}; // safe stage / HTTP / XErr details; never a token
};
struct SessionChanged {
    std::string selected_xuid;
    std::string active_xuid; // identity in the last serialized server login
    bool busy = false;
    bool available = false;
    std::string error;
    std::string diagnostic;
};
struct UseFinished { std::string xuid; bool ok = false; std::uint64_t request_id = 0; };
using Event = std::variant<AccountsChanged, SignInProgress, SignInFinished, SessionChanged, UseFinished>;

// Render thread -> service.
struct StartSignIn {};
struct CancelSignIn {};
struct CopySignInCode {};
struct OpenSignInPage {};
struct RemoveAccount {
    std::string xuid;
};
struct UseAccount { std::string xuid; std::uint64_t request_id = 0; };
using Command = std::variant<StartSignIn, CancelSignIn, CopySignInCode, OpenSignInPage, RemoveAccount, UseAccount>;

// The only surface shared between the alt service and the render thread.
struct Channel {
    social::Inbox<Event> events;
    social::Outbox<Command> commands;
};

} // namespace medicon::alt
