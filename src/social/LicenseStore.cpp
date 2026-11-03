#include <medicon/social/LicenseStore.hpp>

#include "Files.hpp"

#include <Windows.h>
#include <dpapi.h>

#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace medicon::social {
namespace {

// Binds the blob to this purpose; another DPAPI user of the same account cannot
// decrypt it by accident.
constexpr std::uintmax_t max_file_bytes = 64 * 1024;

DATA_BLOB entropy(std::string_view purpose) {
    return DATA_BLOB{static_cast<DWORD>(purpose.size()),
                     reinterpret_cast<BYTE*>(const_cast<char*>(purpose.data()))};
}

struct LocalBlob {
    DATA_BLOB blob{};
    ~LocalBlob() {
        if (!blob.pbData) return;
        SecureZeroMemory(blob.pbData, blob.cbData);
        LocalFree(blob.pbData);
    }
};

std::string win32_message(const char* operation, DWORD code) {
    return std::string(operation) + " failed (Win32 error " + std::to_string(code) + ")";
}

} // namespace

std::optional<std::string> LicenseStore::load(std::string& error) const {
    error.clear();
    std::error_code ec;
    const bool exists = std::filesystem::exists(file_, ec);
    if (ec) {
        error = file_.filename().string() + " could not be inspected";
        return std::nullopt;
    }
    if (!exists) return std::nullopt;
    const auto size = std::filesystem::file_size(file_, ec);
    if (ec || size == 0 || size > max_file_bytes) {
        error = file_.filename().string() + " has an unexpected size";
        return std::nullopt;
    }
    std::ifstream stream(file_, std::ios::binary);
    std::vector<BYTE> sealed((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (!stream && !stream.eof()) {
        error = file_.filename().string() + " could not be read";
        return std::nullopt;
    }
    DATA_BLOB input{static_cast<DWORD>(sealed.size()), sealed.data()};
    DATA_BLOB salt = entropy(purpose_);
    LocalBlob output;
    if (!CryptUnprotectData(&input, nullptr, &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output.blob)) {
        // Typically copied from another Windows account. Keep the file; the user signs in again.
        error = win32_message("Decrypting the saved credential", GetLastError());
        return std::nullopt;
    }
    std::string key(reinterpret_cast<const char*>(output.blob.pbData), output.blob.cbData);
    if (key.empty()) {
        error = "The saved credential is empty";
        return std::nullopt;
    }
    return key;
}

bool LicenseStore::save(std::string_view key, std::string& error) const {
    error.clear();
    DATA_BLOB input{static_cast<DWORD>(key.size()), reinterpret_cast<BYTE*>(const_cast<char*>(key.data()))};
    DATA_BLOB salt = entropy(purpose_);
    LocalBlob output;
    // No CRYPTPROTECT_LOCAL_MACHINE: the CurrentUser scope (spec 4.2).
    if (!CryptProtectData(&input, L"Medicon Social credential", &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                          &output.blob)) {
        error = win32_message("Encrypting the credential", GetLastError());
        return false;
    }
    return detail::replace_file(
        file_, {reinterpret_cast<const char*>(output.blob.pbData), static_cast<std::size_t>(output.blob.cbData)}, error);
}

bool LicenseStore::clear(std::string& error) const {
    error.clear();
    std::error_code ec;
    std::filesystem::remove(file_, ec);
    if (ec) error = file_.filename().string() + " could not be deleted: " + ec.message();
    return !ec;
}

} // namespace medicon::social
