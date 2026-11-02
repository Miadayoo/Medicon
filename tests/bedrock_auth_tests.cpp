#include <medicon/sdk/BedrockAuth.hpp>
#include <medicon/sdk/detail/AuthHttpApi.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using medicon::sdk::detail::read_account_identity;
using medicon::sdk::detail::xbox_identity_gamertag_offset;
using medicon::sdk::detail::xbox_identity_xuid_offset;

int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

// A fabricated Social::XboxLiveIdentity: a heap buffer with MSVC inline strings
// at the observed offsets. Read in-process, exactly like the chat SDK tests.
struct FakeIdentity {
    alignas(std::uintptr_t) std::array<std::byte, xbox_identity_gamertag_offset + 32> bytes{};
    // Heap backing for MSVC strings longer than the 15-byte inline buffer (a real
    // XUID is 16 digits), kept alive for the duration of the read.
    std::vector<std::unique_ptr<std::string>> heap;

    void set_string(std::size_t offset, std::string_view value) {
        std::memset(bytes.data() + offset, 0, 32);
        std::uint64_t size = value.size(), capacity = 15;
        if (value.size() <= 15) {
            std::memcpy(bytes.data() + offset, value.data(), value.size());
        } else {
            auto& backing = heap.emplace_back(std::make_unique<std::string>(value));
            const auto data = reinterpret_cast<std::uintptr_t>(backing->c_str());
            capacity = value.size();
            std::memcpy(bytes.data() + offset, &data, sizeof(data));
        }
        std::memcpy(bytes.data() + offset + 16, &size, sizeof(size));
        std::memcpy(bytes.data() + offset + 24, &capacity, sizeof(capacity));
    }
    const void* base() const { return bytes.data(); }
};

void reads_identity() {
    FakeIdentity identity;
    identity.set_string(xbox_identity_xuid_offset, "2535412345678901");
    identity.set_string(xbox_identity_gamertag_offset, "Mia Alt");
    const auto account = read_account_identity(identity.base());
    require(account.has_value(), "a valid identity was not read");
    require(account->xuid == "2535412345678901", "the XUID came from +0x30");
    require(account->gamertag == "Mia Alt", "the gamertag came from +0x1F0");
}

void rejects_malformed() {
    require(!read_account_identity(nullptr), "a null identity was accepted");

    FakeIdentity empty;
    empty.set_string(xbox_identity_xuid_offset, "2535412345678901");
    empty.set_string(xbox_identity_gamertag_offset, "");
    require(!read_account_identity(empty.base()), "an empty gamertag was accepted");

    FakeIdentity non_numeric;
    non_numeric.set_string(xbox_identity_xuid_offset, "not-a-xuid");
    non_numeric.set_string(xbox_identity_gamertag_offset, "Mia Alt");
    require(!read_account_identity(non_numeric.base()), "a non-decimal XUID was accepted");

    // An unaligned pointer must be refused before any read.
    FakeIdentity aligned;
    aligned.set_string(xbox_identity_xuid_offset, "1");
    aligned.set_string(xbox_identity_gamertag_offset, "g");
    const auto* unaligned = reinterpret_cast<const std::byte*>(aligned.base()) + 1;
    require(!read_account_identity(unaligned), "an unaligned identity was accepted");
}

void connection_auth_view() {
    medicon::sdk::detail::ConnectionAuth source;
    const std::uintptr_t native_key = 0x12345678;
    std::memcpy(source.prefix.data(), &native_key, sizeof(native_key));
    const std::uint32_t offline = 2;
    std::memcpy(source.prefix.data() + 8, &offline, sizeof(offline));
    source.token = "native-token";
    for (const std::string& token : {std::string{}, std::string{"short-token"}, std::string(4096, 'X')}) {
        medicon::sdk::detail::ConnectionAuthView view(source, token);
        const auto copied = medicon::sdk::detail::read_connection_auth(view.bytes.data());
        require(copied && copied->token == token, "the native MSVC reader sees the whole replacement token");
        require(std::memcmp(copied->prefix.data(), &native_key, sizeof(native_key)) == 0, "the native signing key pointer is preserved");
        std::uint32_t type = 99;
        std::memcpy(&type, copied->prefix.data() + 8, sizeof(type));
        require(type == 0, "the service token uses Full authentication");
        require(source.token == "native-token", "the original game-owned token remains unchanged");
    }
    require(!medicon::sdk::detail::read_connection_auth(nullptr), "invalid auth objects are refused");
}

void native_http_modules() {
    // Separate handle namespaces model two independent libHC runtimes. A
    // missing export in one must never be borrowed from the other runtime.
    bool plain_loaded = true, gdk_loaded = false;
    std::string_view plain_missing, gdk_missing;
    const auto resolve = [&] {
        return medicon::sdk::detail::resolve_auth_http_api(
            [&](std::wstring_view name) -> std::uintptr_t {
                if (name == L"libHttpClient.dll" && plain_loaded) return 0x1000;
                if (name == L"libHttpClient.GDK.dll" && gdk_loaded) return 0x2000;
                return 0;
            },
            [&](std::uintptr_t module, std::string_view name) -> std::uintptr_t {
                if (name == (module == 0x1000 ? plain_missing : gdk_missing)) return 0;
                constexpr std::string_view names[] = {"HCHttpCallPerformAsync", "HCHttpCallResponseGetStatusCode",
                    "HCHttpCallGetRequestUrl", "HCHttpCallRequestSetHeader"};
                for (std::uintptr_t i = 0; i < std::size(names); ++i)
                    if (name == names[i]) return module + i + 1;
                return 0;
            });
    };
    auto api = resolve();
    require(api && api.module == "libHttpClient.dll" && api.perform == 0x1001 && api.url == 0x1003,
        "1.26.60 works when only the non-GDK HTTP module is loaded");
    plain_loaded = false; gdk_loaded = true;
    api = resolve();
    require(api && api.module == "libHttpClient.GDK.dll" && api.header == 0x2004,
        "the earlier GDK module name remains supported");
    plain_loaded = true;
    require(resolve().perform == 0x1001, "prefer the HTTP module imported by 1.26.60 when both are loaded");
    plain_missing = "HCHttpCallRequestSetHeader";
    require(!resolve(), "an incomplete current runtime must not select an unrelated complete GDK runtime");
    gdk_missing = "HCHttpCallGetRequestUrl";
    require(!resolve(), "partial modules must not be mixed to produce a false ready state");
    plain_loaded = gdk_loaded = false;
    require(!resolve(), "an absent HTTP runtime must keep account switching unavailable");
}

} // namespace

int main() {
    try {
        reads_identity();
        rejects_malformed();
        connection_auth_view();
        native_http_modules();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << checks << " checks passed\n";
    return 0;
}
