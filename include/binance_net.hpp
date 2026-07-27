#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace hft {

// Transport layer. Deliberately tiny, and deliberately hiding Boost.
//
// Boost.Beast's headers are enormous -- including them here would pull them into
// every translation unit that touches market data and wreck compile times. The
// pimpl idiom keeps all of Boost inside binance_net.cpp. Everyone else sees
// std::string.
//
// Both clients are SYNCHRONOUS and BLOCKING. That is a real choice, not
// laziness -- see docs/02-transport.md for why it is correct here, and what we
// would change if the network stopped being our bottleneck.

// A blocking TLS WebSocket client.
class WebSocketClient {
public:
    WebSocketClient();
    ~WebSocketClient();
    WebSocketClient(const WebSocketClient&)            = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    // Throws on failure. `target` is e.g. "/ws/btcusdt@depth@100ms".
    void connect(const std::string& host, const std::string& target);

    // Blocks until one complete message arrives. The returned view is valid
    // only until the next read() -- copy it if you need to keep it.
    std::string_view read();

    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A persistent HTTPS connection.
//
// Opening a fresh TCP + TLS connection per order costs ~390ms round trip to
// Binance; reusing one costs ~280ms. On a book that moves a tick in that window,
// the difference decides whether a post-only order is still passive when it
// arrives -- measured, not guessed (docs/05).
//
// Reconnects transparently when the peer closes the connection, which servers
// do routinely on idle keep-alives.
class HttpsSession {
public:
    explicit HttpsSession(std::string host);
    ~HttpsSession();
    HttpsSession(const HttpsSession&)            = delete;
    HttpsSession& operator=(const HttpsSession&) = delete;

    // Throws std::runtime_error on any non-200, with the exchange's error body.
    std::string request(const std::string& method, const std::string& target,
                        const std::string& api_key = "");

    std::uint64_t reconnects() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One-shot blocking HTTPS request. Throws std::runtime_error on any non-200,
// with the exchange's error body in the message -- Binance explains rejections
// there and swallowing it turns a five-second fix into an afternoon.
//
// `api_key`, when non-empty, is sent as X-MBX-APIKEY. Authenticated endpoints
// need it in addition to the signature on the query string.
std::string https_request(const std::string& method, const std::string& host,
                          const std::string& target, const std::string& api_key = "");

inline std::string https_get(const std::string& host, const std::string& target) {
    return https_request("GET", host, target);
}

}  // namespace hft
