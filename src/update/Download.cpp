#include "../loader/Platform.hpp"

#include <Windows.h>
#include <winhttp.h>

#include <array>

#pragma comment(lib, "winhttp.lib")

namespace medicon::update {
namespace {

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size < 0 ? 0 : size), L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    explicit operator bool() const { return h != nullptr; }
};

class WinHttpFetcher final : public Fetcher {
public:
    WinHttpFetcher(std::string host, std::string agent) : host_(std::move(host)) {
        session_ = WinHttpOpen(widen(agent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
        if (session_) WinHttpSetTimeouts(session_, 15'000, 15'000, 20'000, 60'000);
    }
    ~WinHttpFetcher() override { if (session_) WinHttpCloseHandle(session_); }

    FetchResult get(std::string_view path, std::uint64_t max_bytes) override {
        FetchResult result;
        if (!session_) { result.error = "no_session"; return result; }
        Handle connect{WinHttpConnect(session_, widen(host_).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0)};
        if (!connect) { result.error = "connect"; return result; }
        // HTTPS only; the request fails rather than silently following a redirect
        // to another host or to plain HTTP.
        Handle request{WinHttpOpenRequest(connect.h, L"GET", widen(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
        if (!request) { result.error = "open"; return result; }
        DWORD redirect = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &redirect, sizeof(redirect));
        if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.h, nullptr)) {
            result.error = "send:" + std::to_string(GetLastError());
            return result;
        }
        result.reached = true;
        DWORD status = 0, size = sizeof(status);
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
            result.status = static_cast<int>(status);
        std::array<char, 16384> buffer{};
        while (true) {
            DWORD read = 0;
            if (!WinHttpReadData(request.h, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
                result.reached = false;
                result.error = "read:" + std::to_string(GetLastError());
                result.body.clear();
                return result;
            }
            if (read == 0) break;
            if (result.body.size() + read > max_bytes) {
                result.error = "too_large";
                result.body.clear();
                result.reached = true;
                result.status = 0;  // treated as an incomplete/invalid response
                return result;
            }
            result.body.append(buffer.data(), read);
        }
        return result;
    }

private:
    std::string host_;
    HINTERNET session_ = nullptr;
};

} // namespace

std::unique_ptr<Fetcher> make_winhttp_fetcher(std::string host, std::string agent) {
    return std::make_unique<WinHttpFetcher>(std::move(host), std::move(agent));
}

} // namespace medicon::update
