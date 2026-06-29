#pragma once

// =============================================================================
// WebSocketServer.hpp — Minimal RFC 6455 WebSocket Broadcast Server
// =============================================================================
// Self-contained: SHA-1, Base64, HTTP upgrade handshake, frame encoding.
// Zero external dependencies — pure POSIX sockets + C++20.
//
// Designed for broadcast-only use: the engine pushes JSON snapshots in,
// every connected client receives them. Client → server frames are ignored.
//
// Usage:
//   WebSocketServer ws(9001);
//   ws.start();
//   ws.broadcast(json_string);   // thread-safe, called from any thread
//   ws.stop();
// =============================================================================

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mxray {

// =============================================================================
// Internal helpers — SHA-1 (FIPS 180-4) and Base64
// Required for the Sec-WebSocket-Accept key derivation (RFC 6455 §4.2.2)
// =============================================================================
namespace ws_detail {

inline void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301u;
    uint32_t h1 = 0xEFCDAB89u;
    uint32_t h2 = 0x98BADCFEu;
    uint32_t h3 = 0x10325476u;
    uint32_t h4 = 0xC3D2E1F0u;

    // Pad message to multiple of 64 bytes: append 0x80, zeros, 64-bit length
    size_t padded = ((len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> msg(padded, 0);
    std::memcpy(msg.data(), data, len);
    msg[len] = 0x80;
    uint64_t bits = static_cast<uint64_t>(len) * 8;
    for (int i = 0; i < 8; ++i)
        msg[padded - 8 + i] = static_cast<uint8_t>(bits >> (56 - i * 8));

    auto rol = [](uint32_t v, int s) -> uint32_t {
        return (v << s) | (v >> (32 - s));
    };

    for (size_t blk = 0; blk < padded; blk += 64) {
        uint32_t w[80];
        for (int j = 0; j < 16; ++j) {
            w[j] = (static_cast<uint32_t>(msg[blk + j*4    ]) << 24)
                 | (static_cast<uint32_t>(msg[blk + j*4 + 1]) << 16)
                 | (static_cast<uint32_t>(msg[blk + j*4 + 2]) <<  8)
                 |  static_cast<uint32_t>(msg[blk + j*4 + 3]);
        }
        for (int j = 16; j < 80; ++j)
            w[j] = rol(w[j-3] ^ w[j-8] ^ w[j-14] ^ w[j-16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int j = 0; j < 80; ++j) {
            uint32_t f, k;
            if      (j < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (j < 40) { f = b ^ c ^ d;           k = 0x6ED9EBA1u; }
            else if (j < 60) { f = (b&c)|(b&d)|(c&d);  k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;           k = 0xCA62C1D6u; }
            uint32_t tmp = rol(a, 5) + f + e + k + w[j];
            e = d; d = c; c = rol(b, 30); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    auto store32 = [&](int idx, uint32_t v) {
        out[idx*4    ] = static_cast<uint8_t>(v >> 24);
        out[idx*4 + 1] = static_cast<uint8_t>(v >> 16);
        out[idx*4 + 2] = static_cast<uint8_t>(v >>  8);
        out[idx*4 + 3] = static_cast<uint8_t>(v      );
    };
    store32(0,h0); store32(1,h1); store32(2,h2); store32(3,h3); store32(4,h4);
}

inline std::string base64_encode(const uint8_t* data, size_t len) {
    static constexpr char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b  = static_cast<uint32_t>(data[i]) << 16;
        if (i+1 < len) b |= static_cast<uint32_t>(data[i+1]) << 8;
        if (i+2 < len) b |= static_cast<uint32_t>(data[i+2]);
        out += tbl[(b >> 18) & 0x3F];
        out += tbl[(b >> 12) & 0x3F];
        out += (i+1 < len) ? tbl[(b >>  6) & 0x3F] : '=';
        out += (i+2 < len) ? tbl[(b      ) & 0x3F] : '=';
    }
    return out;
}

// Build a WebSocket text frame (server→client, no masking per RFC 6455 §6.1)
inline std::string text_frame(const std::string& payload) {
    std::string frame;
    frame.reserve(payload.size() + 10);
    frame += static_cast<char>(0x81); // FIN=1, opcode=0x1 (text)

    size_t len = payload.size();
    if (len < 126) {
        frame += static_cast<char>(len);
    } else if (len < 65536) {
        frame += static_cast<char>(126);
        frame += static_cast<char>((len >> 8) & 0xFF);
        frame += static_cast<char>( len       & 0xFF);
    } else {
        frame += static_cast<char>(127);
        for (int i = 7; i >= 0; --i)
            frame += static_cast<char>((len >> (i * 8)) & 0xFF);
    }
    frame += payload;
    return frame;
}

} // namespace ws_detail

// =============================================================================
// WebSocketServer
// =============================================================================
class WebSocketServer {
public:
    explicit WebSocketServer(uint16_t port = 9001)
        : port_(port), server_fd_(-1), running_(false) {}

    ~WebSocketServer() { stop(); }

    // Non-copyable / non-movable — owns OS handle + thread
    WebSocketServer(const WebSocketServer&)            = delete;
    WebSocketServer& operator=(const WebSocketServer&) = delete;

    // -------------------------------------------------------------------------
    // start() — bind, listen, launch event-loop thread
    // Returns true on success.
    // -------------------------------------------------------------------------
    bool start() {
        server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            std::cerr << "[WS] socket() failed\n";
            return false;
        }

        int opt = 1;
        ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        set_nonblocking(server_fd_);

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port_);

        if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::cerr << "[WS] bind() failed on port " << port_ << "\n";
            ::close(server_fd_);
            server_fd_ = -1;
            return false;
        }

        ::listen(server_fd_, 32);
        running_.store(true, std::memory_order_relaxed);
        loop_thread_ = std::thread([this] { event_loop(); });

        std::cout << "[WS] Listening on ws://0.0.0.0:" << port_ << "\n";
        return true;
    }

    // -------------------------------------------------------------------------
    // stop() — graceful shutdown, closes all clients, joins thread
    // -------------------------------------------------------------------------
    void stop() {
        running_.store(false, std::memory_order_relaxed);
        if (loop_thread_.joinable()) loop_thread_.join();
        if (server_fd_ >= 0) {
            ::close(server_fd_);
            server_fd_ = -1;
        }
    }

    // -------------------------------------------------------------------------
    // broadcast() — thread-safe, send JSON to every connected client
    // -------------------------------------------------------------------------
    void broadcast(const std::string& json) {
        if (json.empty()) return;
        const std::string frame = ws_detail::text_frame(json);

        std::lock_guard<std::mutex> lk(clients_mutex_);
        for (auto it = clients_.begin(); it != clients_.end(); ) {
            if (!it->handshake_done) { ++it; continue; }
            ssize_t sent = ::send(it->fd, frame.data(), frame.size(), MSG_NOSIGNAL);
            if (sent < 0) {
                std::cout << "[WS] Client " << it->fd << " disconnected (send error)\n";
                ::close(it->fd);
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
    }

    size_t client_count() const noexcept {
        std::lock_guard<std::mutex> lk(clients_mutex_);
        return clients_.size();
    }

    uint16_t port() const noexcept { return port_; }

private:
    // -------------------------------------------------------------------------
    // Per-client state
    // -------------------------------------------------------------------------
    struct Client {
        int         fd             = -1;
        bool        handshake_done = false;
        std::string recv_buf;

        explicit Client(int fd_) : fd(fd_) {}
    };

    // -------------------------------------------------------------------------
    // event_loop() — runs in loop_thread_
    // Uses select() to multiplex the server socket and all client sockets.
    // -------------------------------------------------------------------------
    void event_loop() {
        while (running_.load(std::memory_order_relaxed)) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(server_fd_, &rfds);
            int maxfd = server_fd_;

            {
                std::lock_guard<std::mutex> lk(clients_mutex_);
                for (const auto& c : clients_) {
                    FD_SET(c.fd, &rfds);
                    maxfd = std::max(maxfd, c.fd);
                }
            }

            timeval tv{0, 5000}; // 5 ms — keeps the shutdown check responsive
            int ready = ::select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
            if (ready <= 0) continue;

            // Accept new TCP connections
            if (FD_ISSET(server_fd_, &rfds)) {
                sockaddr_in cli{};
                socklen_t cli_len = sizeof(cli);
                int fd = ::accept(server_fd_,
                    reinterpret_cast<sockaddr*>(&cli), &cli_len);
                if (fd >= 0) {
                    set_nonblocking(fd);
                    std::lock_guard<std::mutex> lk(clients_mutex_);
                    clients_.emplace_back(fd);
                    std::cout << "[WS] TCP connection from "
                              << inet_ntoa(cli.sin_addr) << "\n";
                }
            }

            // Service existing clients
            std::lock_guard<std::mutex> lk(clients_mutex_);
            for (auto it = clients_.begin(); it != clients_.end(); ) {
                if (!FD_ISSET(it->fd, &rfds)) { ++it; continue; }

                char tmp[8192];
                ssize_t n = ::recv(it->fd, tmp, sizeof(tmp), 0);
                if (n <= 0) {
                    std::cout << "[WS] Client " << it->fd << " disconnected\n";
                    ::close(it->fd);
                    it = clients_.erase(it);
                    continue;
                }

                it->recv_buf.append(tmp, static_cast<size_t>(n));

                if (!it->handshake_done) {
                    // Wait for the full HTTP request (ends with \r\n\r\n)
                    if (it->recv_buf.find("\r\n\r\n") != std::string::npos) {
                        std::string response = build_handshake_response(it->recv_buf);
                        if (!response.empty()) {
                            ::send(it->fd, response.data(), response.size(), MSG_NOSIGNAL);
                            it->handshake_done = true;
                            it->recv_buf.clear();
                            std::cout << "[WS] Handshake complete — client "
                                      << it->fd << " is ready\n";
                        } else {
                            // Bad request — reject
                            const char* bad = "HTTP/1.1 400 Bad Request\r\n\r\n";
                            ::send(it->fd, bad, strlen(bad), MSG_NOSIGNAL);
                            ::close(it->fd);
                            it = clients_.erase(it);
                            continue;
                        }
                    }
                }
                // We intentionally ignore client→server frames (broadcast-only server).
                ++it;
            }
        }

        // Clean up all remaining clients on shutdown
        std::lock_guard<std::mutex> lk(clients_mutex_);
        for (auto& c : clients_) ::close(c.fd);
        clients_.clear();
    }

    // -------------------------------------------------------------------------
    // build_handshake_response() — RFC 6455 §4.2.2 upgrade response
    // -------------------------------------------------------------------------
    static std::string build_handshake_response(const std::string& request) {
        // Find Sec-WebSocket-Key header
        auto pos = request.find("Sec-WebSocket-Key:");
        if (pos == std::string::npos) return {};
        pos += 18;
        while (pos < request.size() && request[pos] == ' ') ++pos;
        auto end = request.find("\r\n", pos);
        if (end == std::string::npos) return {};

        const std::string key    = request.substr(pos, end - pos);
        // Strip any trailing whitespace/CR
        const std::string combined = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

        uint8_t hash[20];
        ws_detail::sha1(
            reinterpret_cast<const uint8_t*>(combined.data()),
            combined.size(), hash);
        const std::string accept = ws_detail::base64_encode(hash, 20);

        return "HTTP/1.1 101 Switching Protocols\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Accept: " + accept + "\r\n"
               "\r\n";
    }

    static void set_nonblocking(int fd) {
        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    // -------------------------------------------------------------------------
    // Members
    // -------------------------------------------------------------------------
    uint16_t              port_;
    int                   server_fd_;
    std::atomic<bool>     running_;
    std::thread           loop_thread_;
    mutable std::mutex    clients_mutex_;
    std::vector<Client>   clients_;
};

} // namespace mxray
