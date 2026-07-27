#pragma once

#include "depth_sync.hpp"
#include "gzfile.hpp"
#include "json_decode.hpp"
#include "recording.hpp"
#include "trade.hpp"

#include <simdjson.h>

#include <cstdint>
#include <string>

namespace hft {

struct ReplayStats {
    std::uint64_t records     = 0;
    std::uint64_t depth       = 0;
    std::uint64_t trades      = 0;
    std::uint64_t snapshots   = 0;
    std::uint64_t undecodable = 0;
    std::int64_t  first_ns    = 0;
    std::int64_t  last_ns     = 0;
    bool          truncated   = false;
};

// Reads a recording and pushes decoded events at a handler, in file order.
//
// Templated rather than std::function-based so the calls inline and the hot
// loop does not allocate. The handler must provide:
//
//   void on_header  (std::string_view meta_json)
//   void on_snapshot(const DepthSnapshot&, std::int64_t recv_ns)
//   void on_depth   (const DepthUpdate&,   std::int64_t recv_ns)
//   void on_trade   (const Trade&,         std::int64_t recv_ns)
//
// Everything downstream -- the stats tool, the backtester -- shares this one
// loop. If replay ever decoded differently from live, a backtest would be
// measuring a program that does not exist.
template <class H>
ReplayStats replay_file(const std::string& path, H& h) {
    ReplayStats           st;
    GzLineReader          reader(path);
    simdjson::dom::parser parser;

    const auto dispatch = [&](simdjson::dom::element data, std::int64_t ns) {
        std::string_view type;
        if (data["e"].get(type)) {
            ++st.undecodable;
            return;
        }
        if (type == "depthUpdate") {
            const auto ev = decode_depth_event(data);
            if (!ev) {
                ++st.undecodable;
                return;
            }
            ++st.depth;
            h.on_depth(*ev, ns);
        } else if (type == "trade" || type == "aggTrade") {
            const auto tr = decode_trade(data);
            if (!tr) {
                ++st.undecodable;
                return;
            }
            ++st.trades;
            h.on_trade(*tr, ns);
        }
    };

    std::string_view line;
    while (reader.next(line)) {
        if (line.empty()) continue;
        ++st.records;

        simdjson::dom::element env;
        if (parser.parse(line.data(), line.size()).get(env)) {
            ++st.undecodable;  // usually the truncated last line of a killed run
            continue;
        }

        std::int64_t     ns = 0;
        std::string_view kind;
        if (env["t"].get(ns) || env["k"].get(kind)) {
            ++st.undecodable;
            continue;
        }
        if (st.first_ns == 0) st.first_ns = ns;
        st.last_ns = ns;

        simdjson::dom::element payload;
        if (env["d"].get(payload)) {
            ++st.undecodable;
            continue;
        }

        if (kind == "h") {
            h.on_header(simdjson::to_string(payload));
        } else if (kind == "s") {
            const auto snap = decode_depth_snapshot(payload);
            if (!snap) {
                ++st.undecodable;
                continue;
            }
            ++st.snapshots;
            h.on_snapshot(*snap, ns);
        } else if (kind == "e") {
            // Format 1: the record kind said "depth event" and meant it.
            const auto ev = decode_depth_event(payload);
            if (!ev) {
                ++st.undecodable;
                continue;
            }
            ++st.depth;
            h.on_depth(*ev, ns);
        } else if (kind == "w") {
            // Format 2: any WebSocket message; the payload says what it is.
            dispatch(unwrap_stream_payload(payload), ns);
        }
    }

    st.truncated = reader.truncated();
    return st;
}

}  // namespace hft
