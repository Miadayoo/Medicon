#pragma once

#include <medicon/social/LicenseStore.hpp>

#include <cstdint>

namespace medicon::social {

struct AccountCredential {
    // Zero while registration/migration is pending. Persisted before any request.
    std::int64_t user_id = 0;
    std::string username;
    std::string secret;
    std::string legacy_key; // only until an existing license is migrated
};

// account.bin is bound to the current Windows user with DPAPI. A corrupt or
// inaccessible credential must be preserved, never silently replaced.
class AccountStore {
public:
    explicit AccountStore(std::filesystem::path file)
        : protected_(std::move(file), "Medicon.Social.Account.v1") {}
    std::optional<AccountCredential> load(std::string& error) const;
    bool save(const AccountCredential& account, std::string& error) const;
    bool clear(std::string& error) const { return protected_.clear(error); }
    static std::optional<std::string> generate_secret(std::string& error);

private:
    LicenseStore protected_;
};

} // namespace medicon::social
