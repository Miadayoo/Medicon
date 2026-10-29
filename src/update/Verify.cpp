#include <medicon/update/Loader.hpp>
#include <medicon/update/ReleasePublicKey.hpp>

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif

// SHA-256 and RSA-PSS verification with CNG. The public key is embedded
// (ReleasePublicKey.hpp); the private half never leaves the release server.
namespace medicon::update {
namespace {

constexpr NTSTATUS k_success = 0;

std::string to_hex(const std::array<std::uint8_t, 32>& digest) {
    std::string hex(64, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        hex[i * 2] = "0123456789abcdef"[(digest[i] >> 4) & 0xF];
        hex[i * 2 + 1] = "0123456789abcdef"[digest[i] & 0xF];
    }
    return hex;
}

bool sha256(std::string_view data, std::array<std::uint8_t, 32>& out) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != k_success) return false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == k_success;
    while (ok && !data.empty()) {
        const auto count = static_cast<ULONG>(data.size() > 1048576 ? 1048576 : data.size());
        ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), count, 0) == k_success;
        data.remove_prefix(count);
    }
    if (ok) ok = BCryptFinishHash(hash, out.data(), static_cast<ULONG>(out.size()), 0) == k_success;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// Builds a BCRYPT_RSAPUBLIC_BLOB from the embedded big-endian modulus/exponent.
std::vector<std::uint8_t> public_key_blob() {
    const auto& modulus = release_key_modulus;
    const auto& exponent = release_key_exponent;
    std::vector<std::uint8_t> blob(sizeof(BCRYPT_RSAKEY_BLOB) + exponent.size() + modulus.size());
    auto* header = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(blob.data());
    header->Magic = BCRYPT_RSAPUBLIC_MAGIC;
    header->BitLength = static_cast<ULONG>(modulus.size() * 8);
    header->cbPublicExp = static_cast<ULONG>(exponent.size());
    header->cbModulus = static_cast<ULONG>(modulus.size());
    header->cbPrime1 = 0;
    header->cbPrime2 = 0;
    std::copy(exponent.begin(), exponent.end(), blob.begin() + sizeof(BCRYPT_RSAKEY_BLOB));
    std::copy(modulus.begin(), modulus.end(), blob.begin() + sizeof(BCRYPT_RSAKEY_BLOB) + exponent.size());
    return blob;
}

class CngVerifier final : public Verifier {
public:
    bool verify_manifest(std::string_view manifest, std::string_view signature) override {
        // No embedded key means no trust: refuse everything (docs/auto-update.md).
        if (release_key_modulus.empty() || release_key_exponent.empty() || signature.empty()) return false;
        std::array<std::uint8_t, 32> digest{};
        if (!sha256(manifest, digest)) return false;

        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0) != k_success) return false;
        BCRYPT_KEY_HANDLE key = nullptr;
        auto blob = public_key_blob();
        bool ok = false;
        if (BCryptImportKeyPair(alg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key, blob.data(),
                                static_cast<ULONG>(blob.size()), 0) == k_success) {
            BCRYPT_PSS_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM, 32};
            ok = BCryptVerifySignature(key, &padding, digest.data(), static_cast<ULONG>(digest.size()),
                                       reinterpret_cast<PUCHAR>(const_cast<char*>(signature.data())),
                                       static_cast<ULONG>(signature.size()), BCRYPT_PAD_PSS) == k_success;
            BCryptDestroyKey(key);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
        return ok;
    }

    std::string sha256_hex(std::string_view data) override {
        std::array<std::uint8_t, 32> digest{};
        return sha256(data, digest) ? to_hex(digest) : std::string{};
    }
};

} // namespace

std::unique_ptr<Verifier> make_cng_verifier() { return std::make_unique<CngVerifier>(); }

} // namespace medicon::update
