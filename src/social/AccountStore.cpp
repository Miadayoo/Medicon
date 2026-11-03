#include <medicon/social/AccountStore.hpp>
#include <medicon/social/Protocol.hpp>

#include <Windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>

namespace medicon::social {
namespace {

bool valid(const AccountCredential& account) {
    const bool secret = account.secret.size() == 64 && std::all_of(account.secret.begin(), account.secret.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    if (!secret || account.user_id < 0 || account.user_id > 9007199254740991LL) return false;
    if (!account.legacy_key.empty()) return account.user_id == 0 && account.legacy_key.size() <= 64;
    return valid_username(account.username);
}

} // namespace

std::optional<AccountCredential> AccountStore::load(std::string& error) const {
    const auto bytes = protected_.load(error);
    if (!bytes) return std::nullopt;
    try {
        const auto json = nlohmann::json::parse(*bytes);
        if (json.at("version") == 1 && json.at("userId").is_number_integer()) {
            AccountCredential account{json.at("userId").get<std::int64_t>(), json.at("username").get<std::string>(),
                                      json.at("secret").get<std::string>(), json.value("legacyKey", std::string{})};
            if (valid(account)) return account;
        }
    } catch (...) {
        // JSON exceptions may include plaintext credentials; never expose them.
    }
    error = "The saved account credential is invalid";
    return std::nullopt;
}

bool AccountStore::save(const AccountCredential& account, std::string& error) const {
    if (!valid(account)) {
        error = "The account credential is invalid";
        return false;
    }
    return protected_.save(nlohmann::json{{"version", 1}, {"userId", account.user_id}, {"username", account.username},
        {"secret", account.secret}, {"legacyKey", account.legacy_key}}.dump(), error);
}

std::optional<std::string> AccountStore::generate_secret(std::string& error) {
    error.clear();
    std::array<unsigned char, 32> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        error = "The account secret could not be generated";
        return std::nullopt;
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string secret;
    secret.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        secret.push_back(hex[byte >> 4]);
        secret.push_back(hex[byte & 15]);
    }
    SecureZeroMemory(bytes.data(), bytes.size());
    return secret;
}

} // namespace medicon::social
