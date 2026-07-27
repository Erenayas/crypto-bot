#include "binance_net.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <stdexcept>

namespace hft {
namespace {

namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
namespace ssl       = net::ssl;
using tcp           = net::ip::tcp;

// Certificate verification is ON. It would "work" without it, and you would
// have no idea who you were sending orders to.
ssl::context make_ssl_context() {
    ssl::context ctx{ssl::context::tlsv12_client};
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(ssl::verify_peer);
    return ctx;
}

// Server Name Indication. Binance shares an IP across many hostnames, so
// without SNI the TLS handshake gets the wrong certificate and fails.
void set_sni(SSL* handle, const std::string& host) {
    if (!SSL_set_tlsext_host_name(handle, host.c_str())) {
        throw std::runtime_error("failed to set TLS SNI hostname for " + host);
    }
}

}  // namespace

// ------------------------------------------------------------ WebSocketClient

struct WebSocketClient::Impl {
    net::io_context                             ioc;
    ssl::context                                ctx = make_ssl_context();
    websocket::stream<ssl::stream<tcp::socket>> ws{ioc, ctx};
    beast::flat_buffer                          buffer;
};

WebSocketClient::WebSocketClient() : impl_(std::make_unique<Impl>()) {}
WebSocketClient::~WebSocketClient() { close(); }

void WebSocketClient::connect(const std::string& host, const std::string& target) {
    auto& d = *impl_;

    tcp::resolver resolver{d.ioc};
    const auto    endpoints = resolver.resolve(host, "443");

    // .next_layer() peels one layer off: ws -> ssl::stream -> tcp::socket.
    net::connect(d.ws.next_layer().next_layer(), endpoints);

    set_sni(d.ws.next_layer().native_handle(), host);
    d.ws.next_layer().handshake(ssl::stream_base::client);  // TLS handshake

    d.ws.set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(http::field::user_agent, "hftbot/0.1");
    }));

    // Binance sends JSON text frames. Beast answers protocol-level pings
    // automatically, so the connection stays alive without us doing anything.
    d.ws.handshake(host, target);  // WebSocket upgrade
}

std::string_view WebSocketClient::read() {
    auto& d = *impl_;
    d.buffer.clear();
    d.ws.read(d.buffer);
    const auto data = d.buffer.data();
    return std::string_view(static_cast<const char*>(data.data()), data.size());
}

void WebSocketClient::close() noexcept {
    try {
        if (impl_ && impl_->ws.is_open()) {
            impl_->ws.close(websocket::close_code::normal);
        }
    } catch (...) {
        // Closing a socket that the peer already dropped throws. Nothing useful
        // to do about it, and a destructor must never propagate.
    }
}

// ------------------------------------------------------------------ https_get

std::string https_request(const std::string& method, const std::string& host,
                          const std::string& target, const std::string& api_key) {
    net::io_context ioc;
    ssl::context    ctx = make_ssl_context();

    ssl::stream<tcp::socket> stream{ioc, ctx};
    tcp::resolver            resolver{ioc};
    net::connect(stream.next_layer(), resolver.resolve(host, "443"));

    set_sni(stream.native_handle(), host);
    stream.handshake(ssl::stream_base::client);

    http::verb verb = http::verb::get;
    if (method == "POST") verb = http::verb::post;
    else if (method == "DELETE") verb = http::verb::delete_;
    else if (method == "PUT") verb = http::verb::put;

    http::request<http::string_body> req{verb, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "hftbot/0.1");
    if (!api_key.empty()) req.set("X-MBX-APIKEY", api_key);
    // Binance reads signed parameters from the query string even on POST, so
    // the body stays empty. Content-Length must still be set or some proxies
    // hold the connection open waiting for a body that never comes.
    req.prepare_payload();
    http::write(stream, req);

    beast::flat_buffer                buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    // Most servers just drop the connection rather than completing a clean TLS
    // shutdown, which surfaces here as an error. It is expected; ignore it.
    beast::error_code ec;
    stream.shutdown(ec);

    if (res.result() != http::status::ok) {
        throw std::runtime_error("HTTP " + std::to_string(res.result_int()) + " from " +
                                 method + " " + host + target + ": " + res.body());
    }
    return res.body();
}

}  // namespace hft
