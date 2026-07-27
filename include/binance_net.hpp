#pragma once

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

// One-shot blocking HTTPS GET. Throws std::runtime_error on any non-200.
std::string https_get(const std::string& host, const std::string& target);

}  // namespace hft
