// ============================================================================
// http.cppm — Minimal HTTP types & TCP server (Ch 1, 9, 14)
//
// Ch 1:  "Resources extend beyond memory: sockets, FDs, metrics."
// Ch 9:  "Make interfaces narrow — accept only what you need."
// Ch 14: "std::jthread + std::stop_token for cooperative cancellation."
//
// NOTE: This is a teaching-grade HTTP implementation. Production code
//       would use a battle-tested library (Boost.Beast, etc.).
//
// C++23 features used:
//   • C++20 modules (Ch 11)          — named module with POSIX in GMF
//   • std::string_view              — non-owning header/body access
//   • std::format                   — response formatting
//   • std::optional                 — missing headers
//   • std::jthread / std::stop_token — cooperative shutdown
//   • RAII                          — socket lifetime management
//   • enum class                    — HTTP method as closed set
//   • [[nodiscard]]                 — prevent silent drops
// ============================================================================
module;

// Global module fragment: standard + POSIX headers
// (POSIX headers must appear here, not in the module purview)
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

export module webapi.http;

export namespace webapi::http {

#ifdef _WIN32
using socket_handle = SOCKET;
using socket_length = int;
constexpr socket_handle invalid_socket_handle = INVALID_SOCKET;

[[nodiscard]] inline bool initialize_socket_runtime() noexcept {
    static const bool initialized = [] {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
}

[[nodiscard]] inline int read_from_socket(socket_handle socket_fd, char* buffer, int size) noexcept {
    return ::recv(socket_fd, buffer, size, 0);
}

[[nodiscard]] inline int write_to_socket(socket_handle socket_fd, const char* buffer, int size) noexcept {
    return ::send(socket_fd, buffer, size, 0);
}

inline void close_socket(socket_handle socket_fd) noexcept {
    ::closesocket(socket_fd);
}

[[nodiscard]] inline std::string last_socket_error() {
    return std::format("WSA error {}", ::WSAGetLastError());
}
#else
using socket_handle = int;
using socket_length = socklen_t;
constexpr socket_handle invalid_socket_handle = -1;

[[nodiscard]] inline bool initialize_socket_runtime() noexcept {
    return true;
}

[[nodiscard]] inline int read_from_socket(socket_handle socket_fd, char* buffer, int size) noexcept {
    return ::read(socket_fd, buffer, static_cast<std::size_t>(size));
}

[[nodiscard]] inline int write_to_socket(socket_handle socket_fd, const char* buffer, int size) noexcept {
    return ::write(socket_fd, buffer, static_cast<std::size_t>(size));
}

inline void close_socket(socket_handle socket_fd) noexcept {
    ::close(socket_fd);
}

[[nodiscard]] inline std::string last_socket_error() {
    return std::strerror(errno);
}
#endif

// ── HTTP Method (closed set — Ch 3) ─────────────────────────────────────────
enum class Method : std::uint8_t {
    GET, POST, PUT, PATCH, DELETE_, UNKNOWN,
};

[[nodiscard]] constexpr Method parse_method(std::string_view sv) noexcept {
    if (sv == "GET")    return Method::GET;
    if (sv == "POST")   return Method::POST;
    if (sv == "PUT")    return Method::PUT;
    if (sv == "PATCH")  return Method::PATCH;
    if (sv == "DELETE") return Method::DELETE_;
    return Method::UNKNOWN;
}

[[nodiscard]] constexpr std::string_view method_to_string(Method m) noexcept {
    switch (m) {
        case Method::GET:     return "GET";
        case Method::POST:    return "POST";
        case Method::PUT:     return "PUT";
        case Method::PATCH:   return "PATCH";
        case Method::DELETE_: return "DELETE";
        case Method::UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

// ── HTTP Request ────────────────────────────────────────────────────────────
struct Request {
    Method      method{Method::UNKNOWN};
    std::string path;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    // Ch 5: use std::optional for "absence is not an error"
    [[nodiscard]] std::optional<std::string_view>
    header(std::string_view name) const {
        for (const auto& [k, v] : headers) {
            if (k == name) return v;
        }
        return std::nullopt;
    }

    // Extract a path parameter: e.g., "/tasks/42" with pattern "/tasks/"
    [[nodiscard]] std::optional<std::string_view>
    path_param_after(std::string_view prefix) const {
        if (!path.starts_with(prefix)) return std::nullopt;
        auto rest = std::string_view{path}.substr(prefix.size());
        if (rest.empty()) return std::nullopt;
        return rest;
    }
};

// ── HTTP Response ───────────────────────────────────────────────────────────
struct Response {
    int         status{200};
    std::string body;
    std::string content_type = "application/json";

    [[nodiscard]] std::string serialize() const {
        return std::format(
            "HTTP/1.1 {} {}\r\n"
            "Content-Type: {}\r\n"
            "Content-Length: {}\r\n"
            "Connection: close\r\n"
            "\r\n"
            "{}",
            status, status_text(), content_type, body.size(), body
        );
    }

    // Helper factories (Ch 4: keep construction sites readable) ──────────────
    [[nodiscard]] static Response ok(std::string body) {
        return {.status = 200, .body = std::move(body)};
    }

    [[nodiscard]] static Response created(std::string body) {
        return {.status = 201, .body = std::move(body)};
    }

    [[nodiscard]] static Response no_content() {
        return {.status = 204, .body = {}};
    }

    [[nodiscard]] static Response error(int status, std::string body) {
        return {.status = status, .body = std::move(body)};
    }

private:
    [[nodiscard]] std::string_view status_text() const noexcept {
        switch (status) {
            case 200: return "OK";
            case 201: return "Created";
            case 204: return "No Content";
            case 400: return "Bad Request";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 409: return "Conflict";
            case 500: return "Internal Server Error";
            default:  return "Unknown";
        }
    }
};

// ── Request parser (minimal — not production-grade) ─────────────────────────
[[nodiscard]] inline std::optional<Request>
parse_request(std::string_view raw) {
    // Find end of request line
    auto line_end = raw.find("\r\n");
    if (line_end == std::string_view::npos) return std::nullopt;

    auto request_line = raw.substr(0, line_end);

    // Parse method
    auto sp1 = request_line.find(' ');
    if (sp1 == std::string_view::npos) return std::nullopt;

    auto method_sv = request_line.substr(0, sp1);
    auto method = parse_method(method_sv);

    // Parse path
    auto sp2 = request_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return std::nullopt;

    auto path = std::string(request_line.substr(sp1 + 1, sp2 - sp1 - 1));

    // Parse headers
    std::vector<std::pair<std::string, std::string>> headers;
    auto pos = line_end + 2;
    while (pos < raw.size()) {
        auto next_end = raw.find("\r\n", pos);
        if (next_end == std::string_view::npos || next_end == pos) break;

        auto header_line = raw.substr(pos, next_end - pos);
        auto colon = header_line.find(':');
        if (colon != std::string_view::npos) {
            auto name = header_line.substr(0, colon);
            auto value = header_line.substr(colon + 1);
            // trim leading space
            if (!value.empty() && value[0] == ' ') value = value.substr(1);
            headers.emplace_back(std::string(name), std::string(value));
        }
        pos = next_end + 2;
    }

    // Find body (after blank line)
    auto body_start = raw.find("\r\n\r\n");
    std::string body;
    if (body_start != std::string_view::npos) {
        body = std::string(raw.substr(body_start + 4));
    }

    return Request{
        .method  = method,
        .path    = std::move(path),
        .body    = std::move(body),
        .headers = std::move(headers),
    };
}

// ── RAII Socket wrapper (Ch 1: "make resource lifetime visible in type") ────
class Socket {
public:
    Socket() = default;
    explicit Socket(socket_handle fd) noexcept : fd_{fd} {}

    // Move-only — unique ownership (Ch 1)
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept : fd_{std::exchange(other.fd_, invalid_socket_handle)} {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, invalid_socket_handle);
        }
        return *this;
    }

    ~Socket() { close(); }

    [[nodiscard]] socket_handle fd() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ != invalid_socket_handle; }
    explicit operator bool() const noexcept { return valid(); }

    void close() noexcept {
        if (valid()) {
            close_socket(fd_);
            fd_ = invalid_socket_handle;
        }
    }

private:
    socket_handle fd_{invalid_socket_handle};
};

// ── Handler type ────────────────────────────────────────────────────────────
using Handler = std::function<Response(const Request&)>;

// ── TCP Server with graceful shutdown (Ch 14: jthread + stop_token) ─────────
class Server {
public:
    explicit Server(std::uint16_t port, Handler handler)
        : port_{port}
        , handler_{std::move(handler)}
    {}

    // Non-copyable, non-movable (owns a running thread)
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Start accepting connections (Ch 14: cooperative cancellation)
    void run(std::stop_token stop_token) {
        auto server_sock = create_server_socket();
        if (!server_sock) {
            std::cerr << "Failed to create server socket\n";
            return;
        }

        std::cout << std::format("Listening on port {}\n", port_);

        while (!stop_token.stop_requested()) {
            // Use select() with timeout to allow periodic stop checks
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(server_sock.fd(), &readfds);

            struct timeval tv{.tv_sec = 1, .tv_usec = 0};
            int ready = ::select(
#ifdef _WIN32
                0,
#else
                server_sock.fd() + 1,
#endif
                &readfds, nullptr, nullptr, &tv);

            if (ready <= 0) continue;  // timeout or error — check stop again

            struct sockaddr_in client_addr{};
            socket_length addr_len = sizeof(client_addr);
            Socket client{::accept(server_sock.fd(),
                          reinterpret_cast<struct sockaddr*>(&client_addr),
                          &addr_len)};

            if (!client) continue;

            handle_connection(std::move(client));
        }

        std::cout << "Server shutting down gracefully\n";
    }

    // Run the server on a jthread, blocking until the stop predicate fires.
    // Ch 14: jthread + stop_token for cooperative cancellation.
    //
    // The jthread lives inside this method so that all stop_source/stop_token
    // instantiation happens within the module — avoiding a known libc++ 20
    // linker issue with __atomic_unique_lock visibility in consumer TUs.
    void run_until(const std::atomic<bool>& should_stop) {
        std::jthread server_thread{[this](std::stop_token st) {
            run(st);
        }};

        // Poll the external stop flag and forward to the jthread's stop_source
        while (!should_stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        server_thread.request_stop();
        // jthread auto-joins on destruction
    }

private:
    std::uint16_t port_;
    Handler       handler_;

    [[nodiscard]] Socket create_server_socket() const {
        if (!initialize_socket_runtime()) {
            return {};
        }

        Socket sock{::socket(AF_INET, SOCK_STREAM, 0)};
        if (!sock) return {};

        int opt = 1;
        if (::setsockopt(sock.fd(), SOL_SOCKET, SO_REUSEADDR,
#ifdef _WIN32
                         reinterpret_cast<const char*>(&opt),
#else
                         &opt,
#endif
                         sizeof(opt)) < 0) {
            return {};
        }

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (::bind(sock.fd(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::cerr << std::format("bind failed: {}\n", last_socket_error());
            return {};
        }

        if (::listen(sock.fd(), 16) < 0) {
            return {};
        }

        return sock;
    }

    void handle_connection(Socket client) const {
        // Read request (simplified: single read, small requests only)
        std::array<char, 8192> buf{};
        auto n = read_from_socket(client.fd(), buf.data(), static_cast<int>(buf.size() - 1));
        if (n <= 0) return;

        auto raw = std::string_view{buf.data(), static_cast<std::size_t>(n)};
        auto req = parse_request(raw);

        Response resp;
        if (req) {
            resp = handler_(*req);
        } else {
            resp = Response::error(400, R"({"error":"malformed request"})");
        }

        auto data = resp.serialize();
        (void)write_to_socket(client.fd(), data.data(), static_cast<int>(data.size()));
        // Socket closes automatically via RAII
    }
};

}  // namespace webapi::http
