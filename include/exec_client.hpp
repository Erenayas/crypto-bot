#pragma once

#include "binance_net.hpp"
#include "price.hpp"
#include "signer.hpp"

#include <cstdint>
#include <string>

namespace hft {

struct Credentials {
    std::string api_key;
    std::string secret;
};

// Precision comes from /fapi/v1/exchangeInfo per symbol. Sending more decimals
// than the symbol allows is rejected outright, which is the first wall everyone
// hits on their first live order.
struct SymbolSpec {
    std::string symbol    = "BTCUSDT";
    int         price_dp  = 2;  // tickSize 0.10 -> 2 decimals accepted
    int         qty_dp    = 3;  // stepSize 0.001
    Price       tick      = 10000000;  // 0.10 in fixed point
};

// Signed order operations against Binance USD-M futures.
//
// Synchronous and one-connection-per-request, matching the rest of the project:
// correctness first, and the network dominates the latency budget anyway. A
// production system would hold a warm keep-alive connection.
class ExecClient {
public:
    ExecClient(std::string host, Credentials creds, SymbolSpec spec)
        : host_(std::move(host)), creds_(std::move(creds)), spec_(std::move(spec)) {}

    // POST-ONLY limit order.
    //
    // timeInForce=GTX ("Good Till Crossing") tells the exchange to REJECT the
    // order outright if it would execute immediately. That is exactly what a
    // market maker wants: crossing the spread would make us a taker -- higher
    // fee, no spread captured, and none of the queue dynamics the strategy is
    // built on. Better a rejected order than an accidental taker fill.
    std::string place_post_only(Side side, Price px, Qty qty, std::int64_t now_ms) {
        std::string q = "symbol=" + spec_.symbol +
                        "&side=" + (side == Side::Buy ? "BUY" : "SELL") +
                        "&type=LIMIT&timeInForce=GTX" +
                        "&price=" + format_fixed(px, spec_.price_dp) +
                        "&quantity=" + format_fixed(qty, spec_.qty_dp);
        return send("POST", "/fapi/v1/order", q, now_ms);
    }

    std::string cancel(std::int64_t order_id, std::int64_t now_ms) {
        return send("DELETE", "/fapi/v1/order",
                    "symbol=" + spec_.symbol + "&orderId=" + std::to_string(order_id), now_ms);
    }

    // The panic button. Called on shutdown, on a risk halt, and on any
    // exception we do not understand. Leaving orders resting in a market you
    // have stopped watching is how a bad day becomes a very bad day.
    std::string cancel_all(std::int64_t now_ms) {
        return send("DELETE", "/fapi/v1/allOpenOrders", "symbol=" + spec_.symbol, now_ms);
    }

    std::string open_orders(std::int64_t now_ms) {
        return send("GET", "/fapi/v1/openOrders", "symbol=" + spec_.symbol, now_ms);
    }

    // Ground truth for our position. We track it locally from fills too, but
    // the exchange is authoritative and the two must be reconciled -- a local
    // position that has silently drifted from the real one is how risk limits
    // stop meaning anything.
    std::string position_risk(std::int64_t now_ms) {
        return send("GET", "/fapi/v2/positionRisk", "symbol=" + spec_.symbol, now_ms);
    }

    const SymbolSpec& spec() const { return spec_; }

private:
    std::string send(const char* method, const char* path, const std::string& query,
                     std::int64_t now_ms) {
        const std::string signed_q = sign_query(query, creds_.secret, now_ms);
        return https_request(method, host_, std::string(path) + "?" + signed_q, creds_.api_key);
    }

    std::string host_;
    Credentials creds_;
    SymbolSpec  spec_;
};

}  // namespace hft
