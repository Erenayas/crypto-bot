#pragma once

#include "depth_sync.hpp"
#include "price.hpp"
#include "trade.hpp"

#include <simdjson.h>

#include <optional>
#include <string_view>

namespace hft {

// ---------------------------------------------------------------------------
// Decoding from an ALREADY-PARSED document.
//
// Live data arrives as a bare JSON message; recorded data arrives wrapped in an
// envelope, so the payload is a nested element of a document someone else
// parsed. Both paths funnel into these two functions, so there is exactly one
// implementation of "what a depth event means" -- and replay can never decode
// differently from live.
// ---------------------------------------------------------------------------

// Levels arrive as arrays of [price, quantity] STRING pairs -- Binance sends
// them as strings precisely so clients don't lose precision to a JSON number.
// We go straight from the string to fixed point, never through a double.
inline bool decode_levels(simdjson::simdjson_result<simdjson::dom::element> node,
                          std::vector<Level>&                               out) {
    simdjson::dom::array levels;
    if (node.get(levels)) return false;

    out.clear();
    out.reserve(levels.size());
    for (auto entry : levels) {
        simdjson::dom::array pair;
        if (entry.get(pair)) return false;

        std::string_view px_s, qty_s;
        if (pair.at(0).get(px_s)) return false;
        if (pair.at(1).get(qty_s)) return false;

        const auto px  = parse_fixed(px_s);
        const auto qty = parse_fixed(qty_s);
        if (!px || !qty) return false;

        out.push_back(Level{*px, *qty});
    }
    return true;
}

// {"e":"depthUpdate","E":..,"T":..,"s":"BTCUSDT","U":..,"u":..,"pu":..,
//  "b":[["7403.89","0.002"]],"a":[["7405.96","3.340"]]}
//
// ALL-OR-NOTHING. A half-decoded event corrupts the book exactly like a dropped
// message does, except it is far harder to trace.
inline std::optional<DepthUpdate> decode_depth_event(simdjson::dom::element doc) {
    // The stream multiplexes event types; ignore anything that is not depth.
    std::string_view type;
    if (doc["e"].get(type) || type != "depthUpdate") return std::nullopt;

    DepthUpdate ev;
    if (doc["U"].get(ev.U)) return std::nullopt;
    if (doc["u"].get(ev.u)) return std::nullopt;
    if (doc["pu"].get(ev.pu)) return std::nullopt;

    if (!decode_levels(doc["b"], ev.bids)) return std::nullopt;
    if (!decode_levels(doc["a"], ev.asks)) return std::nullopt;
    return ev;
}

// {"lastUpdateId":1027024,"E":..,"T":..,"bids":[["4.00","431.00"]],"asks":[..]}
inline std::optional<DepthSnapshot> decode_depth_snapshot(simdjson::dom::element doc) {
    DepthSnapshot snap;
    if (doc["lastUpdateId"].get(snap.last_update_id)) return std::nullopt;
    if (!decode_levels(doc["bids"], snap.bids)) return std::nullopt;
    if (!decode_levels(doc["asks"], snap.asks)) return std::nullopt;
    return snap;
}

// @trade:    {"e":"trade","E":..,"T":..,"s":"BTCUSDT","t":7930321136,
//             "p":"64917.70","q":"0.004","X":"MARKET","m":false}
// @aggTrade: {"e":"aggTrade","E":..,"a":5933014,"s":"BTCUSDT","p":"65050.10",
//             "q":"0.031","f":100,"l":105,"T":..,"m":true}
//
// Both are accepted. They differ only in granularity and in which field carries
// the id, so one decoder covers both and recordings from either stream replay
// through identical code.
inline std::optional<Trade> decode_trade(simdjson::dom::element doc) {
    std::string_view type;
    if (doc["e"].get(type)) return std::nullopt;

    const bool is_agg = (type == "aggTrade");
    if (!is_agg && type != "trade") return std::nullopt;

    Trade            t;
    t.aggregated = is_agg;
    std::string_view px_s, qty_s;
    if (doc["p"].get(px_s)) return std::nullopt;
    if (doc["q"].get(qty_s)) return std::nullopt;
    if (doc[is_agg ? "a" : "t"].get(t.id)) return std::nullopt;
    if (doc["T"].get(t.exch_ms)) return std::nullopt;
    if (doc["m"].get(t.buyer_is_maker)) return std::nullopt;

    const auto px  = parse_fixed(px_s);
    const auto qty = parse_fixed(qty_s);
    if (!px || !qty) return std::nullopt;
    t.px  = *px;
    t.qty = *qty;
    return t;
}

// Combined streams (/stream?streams=a/b) wrap every message in an envelope:
//   {"stream":"btcusdt@aggTrade","data":{ ... }}
// Single streams (/ws/<name>) send the payload bare.
//
// Accepting both means a recording made with either endpoint replays through
// exactly the same code -- including the recordings we already made before the
// trade stream was added.
inline simdjson::dom::element unwrap_stream_payload(simdjson::dom::element msg) {
    simdjson::dom::element data;
    if (!msg["data"].get(data)) return data;
    return msg;
}

// ---------------------------------------------------------------------------
// Decoding from a raw JSON string (the live path).
//
// API choice: simdjson's DOM parser, not the faster On-Demand parser. DOM walks
// the whole document up front, which costs more, but the calling code reads like
// the JSON it parses. On-Demand is a Week 4 optimisation -- after we measure.
// ---------------------------------------------------------------------------
class MessageDecoder {
public:
    // Parses a raw WebSocket message and strips the combined-stream envelope.
    // Callers then dispatch on the payload's "e" field.
    bool parse_message(std::string_view json, simdjson::dom::element& payload) {
        simdjson::dom::element msg;
        if (!parse(json, msg)) return false;
        payload = unwrap_stream_payload(msg);
        return true;
    }

    std::optional<DepthUpdate> decode_event(std::string_view json) {
        simdjson::dom::element doc;
        if (!parse_message(json, doc)) return std::nullopt;
        return decode_depth_event(doc);
    }

    std::optional<DepthSnapshot> decode_snapshot(std::string_view json) {
        simdjson::dom::element doc;
        if (!parse(json, doc)) return std::nullopt;
        return decode_depth_snapshot(doc);
    }

private:
    bool parse(std::string_view json, simdjson::dom::element& out) {
        // simdjson reads past the end of the buffer by design -- its SIMD loads
        // are 64 bytes wide -- so the input must carry padding. Passing
        // (pointer, length) lets simdjson copy into a padded buffer that it owns
        // and REUSES across calls, so we get the padding without allocating per
        // message and without a dangling-view trap.
        return !parser_.parse(json.data(), json.size()).get(out);
    }

    simdjson::dom::parser parser_;  // reused across messages: it owns the scratch
                                    // buffers, so one per message would allocate
                                    // on every tick.
};

}  // namespace hft
