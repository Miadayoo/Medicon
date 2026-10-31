#pragma once

#include <medicon/social/Config.hpp>

#include <Windows.h>
#include <winhttp.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace medicon::social::net {

class Handle {
public:
    Handle() = default;
    explicit Handle(HINTERNET handle) noexcept : handle_(handle) {}
    ~Handle() { reset(); }
    Handle(Handle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    HINTERNET get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }
    void reset() noexcept {
        if (handle_) WinHttpCloseHandle(std::exchange(handle_, nullptr));
    }

private:
    HINTERNET handle_ = nullptr;
};

std::wstring widen(std::string_view utf8);

struct HttpResponse {
    int status = 0;  // 0 when no response arrived; `error` then holds the WinHTTP code
    DWORD error = 0;
    std::string body;
};

// A WebSocket after a completed upgrade. receive() runs on one thread while
// send_text(), shutdown() and abort() may run on another (spec 4.3).
class WebSocket {
public:
    enum class Received { Message, Closed, Failed };

    WebSocket(Handle connect, HINTERNET socket) noexcept;
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    // Blocks until a whole text message, the server's close frame, or an error.
    // Binary messages are skipped; close_code is set for Closed only.
    Received receive(std::string& message, std::uint16_t& close_code, DWORD& error);
    bool send_text(std::string_view text, DWORD& error);
    // Sends our close frame. The server answers with its own, which ends receive().
    void shutdown(std::uint16_t code, std::string_view reason) noexcept;
    // Closes the handle, cancelling a receive() that is still blocked.
    void abort() noexcept;

private:
    Handle connect_;
    HINTERNET socket_;   // never reassigned; closed once under mutex_
    std::mutex mutex_;
    bool closed_ = false;
};

class HttpClient {
public:
    explicit HttpClient(std::string_view client_version);
    bool valid() const noexcept { return static_cast<bool>(session_); }

    // path includes the query. A non-empty json_body is sent as application/json.
    HttpResponse request(const Endpoint& endpoint, const wchar_t* method, std::string_view path,
                         std::string_view json_body, std::string_view bearer, std::string_view extra_headers = {}) const;

    // A binary POST (the Core DLL release upload) with an explicit content type.
    HttpResponse upload(const Endpoint& endpoint, std::string_view path, std::string_view content_type,
                        const void* body, std::size_t size, std::string_view bearer) const;

    struct Upgrade {
        std::unique_ptr<WebSocket> socket;
        int status = 0;  // 101 on success, or the HTTP status the server answered with
        DWORD error = 0;
    };
    Upgrade open_websocket(const Endpoint& endpoint, std::string_view path, std::string_view bearer) const;

private:
    Handle session_;
};

} // namespace medicon::social::net
