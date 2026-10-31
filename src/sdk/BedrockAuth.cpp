#include <medicon/sdk/BedrockAuth.hpp>

#include <medicon/memory/module_view.hpp>
#include <medicon/memory/signature_catalog.hpp>
#include <medicon/memory/signature_registry.hpp>
#include <medicon/sdk/BedrockChat.hpp>

#include <Windows.h>

#include <array>
#include <cstring>
#include <limits>

namespace medicon::sdk {
namespace {

bool copy_bytes(std::uintptr_t address, void* destination, std::size_t length) noexcept {
    if (!address || !destination || !length ||
        length - 1 > std::numeric_limits<std::uintptr_t>::max() - address) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), destination, length, &copied) !=
               FALSE &&
           copied == length;
}

} // namespace

namespace detail {

std::optional<ConnectionAuth> read_connection_auth(const void* auth) noexcept {
    try {
        ConnectionAuth value;
        const auto address = reinterpret_cast<std::uintptr_t>(auth);
        if (!copy_bytes(address, value.prefix.data(), value.prefix.size())) return std::nullopt;
        std::uint32_t type = 0;
        std::memcpy(&type, value.prefix.data() + 8, sizeof(type));
        if (type != 0 && type != 2) return std::nullopt;
        // Chat's string reader intentionally caps messages at 1 KiB. Signed
        // server credentials are larger, so use a separate bounded reader.
        std::array<std::byte, 32> raw{};
        if (!copy_bytes(address + 16, raw.data(), raw.size())) return std::nullopt;
        std::uint64_t size = 0, capacity = 0;
        std::memcpy(&size, raw.data() + 16, sizeof(size));
        std::memcpy(&capacity, raw.data() + 24, sizeof(capacity));
        if (size > 65536 || size > capacity || capacity > 262144) return std::nullopt;
        if (capacity < 16) {
            if (capacity != 15 || size > 15 || raw[size] != std::byte{}) return std::nullopt;
            value.token.assign(reinterpret_cast<const char*>(raw.data()), static_cast<std::size_t>(size));
        } else {
            std::uintptr_t data = 0;
            std::memcpy(&data, raw.data(), sizeof(data));
            value.token.resize(static_cast<std::size_t>(size) + 1);
            if (!copy_bytes(data, value.token.data(), value.token.size()) || value.token.back() != '\0') return std::nullopt;
            value.token.pop_back();
        }
        return value;
    } catch (...) { return std::nullopt; }
}

ConnectionAuthView::ConnectionAuthView(const ConnectionAuth& original, const std::string& token) noexcept {
    std::memcpy(bytes.data(), original.prefix.data(), original.prefix.size());
    // A service-issued multiplayer token always uses Full authentication,
    // including when the native account was in offline/self-signed mode.
    const std::uint32_t full_authentication = 0;
    std::memcpy(bytes.data() + 8, &full_authentication, sizeof(full_authentication));
    const std::uint64_t size = token.size();
    const std::uint64_t capacity = size <= 15 ? 15 : size;
    if (size <= 15) std::memcpy(bytes.data() + 16, token.data(), token.size());
    else {
        const auto data = reinterpret_cast<std::uintptr_t>(token.c_str());
        std::memcpy(bytes.data() + 16, &data, sizeof(data));
    }
    std::memcpy(bytes.data() + 32, &size, sizeof(size));
    std::memcpy(bytes.data() + 40, &capacity, sizeof(capacity));
}

std::optional<AccountIdentity> read_account_identity(const void* xbox_identity) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(xbox_identity);
    if (!base || base % alignof(std::uintptr_t)) return std::nullopt;
    // The game stores both as MSVC release std::string; read_msvc_string copies
    // the bytes without ever treating the foreign object as one of ours.
    auto xuid = read_msvc_string(reinterpret_cast<const void*>(base + xbox_identity_xuid_offset));
    auto gamertag = read_msvc_string(reinterpret_cast<const void*>(base + xbox_identity_gamertag_offset));
    if (!xuid || !gamertag || xuid->empty() || gamertag->empty()) return std::nullopt;
    // The XUID is decimal; reject anything else so a wrong pointer cannot pass.
    for (const char c : *xuid)
        if (c < '0' || c > '9') return std::nullopt;
    return AccountIdentity{std::move(*xuid), std::move(*gamertag)};
}

} // namespace detail

BedrockAuth::State BedrockAuth::locate(std::string& diagnostic) {
    diagnostic.clear();
    if (state_ != State::waiting || scanned_) {
        diagnostic = terminal_diagnostic_;
        return state_;
    }
    scanned_ = true;
    const auto unsupported = [&](std::string message) {
        state_ = State::unsupported;
        terminal_diagnostic_ = std::move(message);
        diagnostic = terminal_diagnostic_;
        return state_;
    };
    auto module = memory::current_process_module(&diagnostic);
    if (!module) return unsupported(diagnostic);

    memory::SignatureRegistry signatures;
    signatures.scan(memory::medicon_auth_signatures(), module->executable_regions, module->image);
    const auto resolve = [&](std::string_view name) -> std::uintptr_t {
        const auto address = signatures.address(name);
        return address && module->image.contains(*address) ? *address : 0;
    };
    connection_request_create_ = resolve("ConnectionRequest_create");
    connection_auth_info_to_json_ = resolve("ConnectionAuthInfo_toJson");
    user_impl_create_ = resolve("Social_UserImpl_create");
    get_global_token_ = resolve("PlayerAuthentication_getGlobalToken");
    start_multiplayer_session_ = resolve("AuthServiceApi_startMultiplayerSession");

    // UserImpl::create anchors the version: without it the layout offsets this
    // SDK reads are not the ones that were analysed, so stay unsupported.
    if (!user_impl_create_)
        return unsupported("Social::UserImpl::create signature did not resolve; auth SDK is 1.26.52.03 only.");
    state_ = State::ready;
    return state_;
}

std::uintptr_t BedrockAuth::login_serialization_target(std::string& diagnostic) const {
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 pe{};
    if (!copy_bytes(module, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        dos.e_lfanew > 1048576 || !copy_bytes(module + dos.e_lfanew, &pe, sizeof(pe)) ||
        pe.Signature != IMAGE_NT_SIGNATURE || pe.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        pe.FileHeader.TimeDateStamp != 0x6ab54e37 || pe.OptionalHeader.SizeOfImage != 0x12c02000 ||
        connection_auth_info_to_json_ != module + 0x46add0) {
        diagnostic = "Server login bridge requires the reviewed Minecraft 1.26.52.03 executable.";
        return 0;
    }
    diagnostic.clear();
    return connection_auth_info_to_json_;
}

} // namespace medicon::sdk
