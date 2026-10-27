#include "WinHttp.hpp"

#include <array>
#include <vector>

namespace medicon::social::net {
namespace {

// REST and WebSocket messages are small; refuse anything larger instead of buffering it.
constexpr std::size_t max_body_bytes = 1 << 20;
// Clients ping every 30 s and the server answers at once, so 75 s without a
// single frame means the connection is gone even if TCP never reported it.
constexpr DWORD websocket_receive_timeout_ms = 75'000;

int status_code(HINTERNET request) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &size, WINHTTP_NO_HEADER_INDEX))
        return 0;
    return static_cast<int>(status);
}

bool read_body(HINTERNET request, std::string& body, DWORD& error) {
    std::array<char, 8192> buffer{};
    while (true) {
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            error = GetLastError();
            return false;
        }
        if (read == 0) return true;
        if (body.size() + read > max_body_bytes) {
            error = ERROR_INSUFFICIENT_BUFFER;
            return false;
        }
        body.append(buffer.data(), read);
    }
}

std::wstring headers_for(std::string_view bearer, bool json) {
    std::wstring headers = L"Accept: application/json\r\n";
    if (json) headers += L"Content-Type: application/json\r\n";
    if (!bearer.empty()) headers += L"Authorization: Bearer " + widen(bearer) + L"\r\n";
    return headers;
}

} // namespace

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

WebSocket::WebSocket(Handle connect, HINTERNET socket) noexcept : connect_(std::move(connect)), socket_(socket) {}

WebSocket::~WebSocket() { abort(); }

WebSocket::Received WebSocket::receive(std::string& message, std::uint16_t& close_code, DWORD& error) {
    message.clear();
    std::array<char, 16384> buffer{};
    bool binary = false;
    while (true) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        // Deliberately unlocked: this blocks for as long as the server is quiet.
        // abort() closes the handle from another thread to end the wait.
        const DWORD result = WinHttpWebSocketReceive(socket_, buffer.data(), static_cast<DWORD>(buffer.size()), &read, &type);
        if (result != NO_ERROR) {
            error = result;
            return Received::Failed;
        }
        switch (type) {
        case WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE:
        case WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE:
            if (message.size() + read > max_body_bytes) {
                error = ERROR_INSUFFICIENT_BUFFER;
                return Received::Failed;
            }
            message.append(buffer.data(), read);
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) return Received::Message;
            break;
        case WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE:
            binary = true;
            break;
        case WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE:
            // The protocol is JSON text only (spec 2.4); skip the whole binary message.
            binary = false;
            message.clear();
            break;
        case WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE: {
            USHORT status = 0;
            std::array<char, WINHTTP_WEB_SOCKET_MAX_CLOSE_REASON_LENGTH> reason{};
            DWORD reason_length = 0;
            std::lock_guard lock(mutex_);
            if (closed_ || WinHttpWebSocketQueryCloseStatus(socket_, &status, reason.data(),
                    static_cast<DWORD>(reason.size()), &reason_length) != NO_ERROR)
                status = 1005; // RFC 6455: no status received
            close_code = status;
            return Received::Closed;
        }
        default:
            break;
        }
        (void)binary;
    }
}

bool WebSocket::send_text(std::string_view text, DWORD& error) {
    std::lock_guard lock(mutex_);
    if (closed_) {
        error = ERROR_INVALID_HANDLE;
        return false;
    }
    error = WinHttpWebSocketSend(socket_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                 const_cast<char*>(text.data()), static_cast<DWORD>(text.size()));
    return error == NO_ERROR;
}

void WebSocket::shutdown(std::uint16_t code, std::string_view reason) noexcept {
    std::lock_guard lock(mutex_);
    if (closed_) return;
    WinHttpWebSocketShutdown(socket_, code, const_cast<char*>(reason.data()), static_cast<DWORD>(reason.size()));
}

void WebSocket::abort() noexcept {
    std::lock_guard lock(mutex_);
    if (closed_) return;
    closed_ = true;
    WinHttpCloseHandle(socket_);
}

HttpClient::HttpClient(std::string_view client_version) {
    const std::wstring agent = L"Medicon-Social/" + widen(client_version);
    // Automatic proxy uses the user's system proxy settings when present.
    session_ = Handle(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0));
    // Certificate validation is WinHTTP's default and is never relaxed (spec 4.10).
    if (session_) WinHttpSetTimeouts(session_.get(), 10'000, 10'000, 10'000, 15'000);
}

HttpResponse HttpClient::request(const Endpoint& endpoint, const wchar_t* method, std::string_view path,
                                 std::string_view json_body, std::string_view bearer) const {
    HttpResponse response;
    if (!session_) {
        response.error = ERROR_INVALID_HANDLE;
        return response;
    }
    Handle connect(WinHttpConnect(session_.get(), widen(endpoint.host).c_str(), endpoint.port, 0));
    if (!connect) {
        response.error = GetLastError();
        return response;
    }
    const std::wstring target = widen(endpoint.base_path) + widen(path);
    Handle request(WinHttpOpenRequest(connect.get(), method, target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, endpoint.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        response.error = GetLastError();
        return response;
    }
    const std::wstring headers = headers_for(bearer, !json_body.empty());
    void* body = json_body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(json_body.data());
    const auto length = static_cast<DWORD>(json_body.size());
    if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1), body, length, length, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        response.error = GetLastError();
        return response;
    }
    response.status = status_code(request.get());
    if (!read_body(request.get(), response.body, response.error)) response.body.clear();
    return response;
}

HttpResponse HttpClient::upload(const Endpoint& endpoint, std::string_view path, std::string_view content_type,
                               const void* body, std::size_t size, std::string_view bearer) const {
    HttpResponse response;
    if (!session_) {
        response.error = ERROR_INVALID_HANDLE;
        return response;
    }
    Handle connect(WinHttpConnect(session_.get(), widen(endpoint.host).c_str(), endpoint.port, 0));
    if (!connect) {
        response.error = GetLastError();
        return response;
    }
    const std::wstring target = widen(endpoint.base_path) + widen(path);
    Handle request(WinHttpOpenRequest(connect.get(), L"POST", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, endpoint.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        response.error = GetLastError();
        return response;
    }
    std::wstring headers = L"Content-Type: " + widen(content_type) + L"\r\n";
    if (!bearer.empty()) headers += L"Authorization: Bearer " + widen(bearer) + L"\r\n";
    // A multi-megabyte upload needs longer than the default send timeout.
    WinHttpSetTimeouts(request.get(), 15000, 15000, 60000, 120000);
    if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1), const_cast<void*>(body),
                            static_cast<DWORD>(size), static_cast<DWORD>(size), 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        response.error = GetLastError();
        return response;
    }
    response.status = status_code(request.get());
    if (!read_body(request.get(), response.body, response.error)) response.body.clear();
    return response;
}

HttpClient::Upgrade HttpClient::open_websocket(const Endpoint& endpoint, std::string_view path,
                                               std::string_view bearer) const {
    Upgrade upgrade;
    if (!session_) {
        upgrade.error = ERROR_INVALID_HANDLE;
        return upgrade;
    }
    Handle connect(WinHttpConnect(session_.get(), widen(endpoint.host).c_str(), endpoint.port, 0));
    if (!connect) {
        upgrade.error = GetLastError();
        return upgrade;
    }
    const std::wstring target = widen(endpoint.base_path) + widen(path);
    Handle request(WinHttpOpenRequest(connect.get(), L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, endpoint.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request || !WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        upgrade.error = GetLastError();
        return upgrade;
    }
    DWORD timeout = websocket_receive_timeout_ms;
    WinHttpSetOption(request.get(), WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    const std::wstring headers = headers_for(bearer, false);
    if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        upgrade.error = GetLastError();
        return upgrade;
    }
    upgrade.status = status_code(request.get());
    if (upgrade.status != HTTP_STATUS_SWITCH_PROTOCOLS) return upgrade;
    HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request.get(), 0);
    if (!socket) {
        upgrade.error = GetLastError();
        upgrade.status = 0;
        return upgrade;
    }
    // The request handle is no longer needed; the connect handle lives as long as the socket.
    upgrade.socket = std::make_unique<WebSocket>(std::move(connect), socket);
    return upgrade;
}

} // namespace medicon::social::net
