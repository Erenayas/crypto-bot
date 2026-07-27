// Deterministic replay of a recording.
//
//   ./build/hftreplay data/btcusdt.jsonl.gz
//   ./build/hftreplay data/btcusdt.jsonl.gz --twice     prove determinism
//
// The whole point: the same bytes, through the same DepthSync, produce the same
// book -- every time, as fast as the disk allows, with no exchange involved.

#include "depth_sync.hpp"
#include "gzfile.hpp"
#include "json_decode.hpp"
#include "recording.hpp"
#include "trade.hpp"

#include <simdjson.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>

namespace {

using namespace hft;

// A cheap fingerprint of the entire book. Two replays of one recording must
// produce the same number -- that is what "deterministic" means, and it is
// worth being able to check mechanically rather than by eye.
std::uint64_t book_checksum(const OrderBook& b) {
    std::uint64_t h   = 1469598103934665603ULL;  // FNV-1a offset basis
    const auto    mix = [&h](std::int64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= static_cast<std::uint64_t>((v >> (i * 8)) & 0xff);
            h *= 1099511628211ULL;  // FNV prime
        }
    };
    for (const auto& [px, qty] : b.bids()) { mix(px); mix(qty); }
    for (const auto& [px, qty] : b.asks()) { mix(px); mix(qty); }
    return h;
}

struct Stats {
    std::uint64_t records     = 0;
    std::uint64_t events      = 0;
    std::uint64_t trades      = 0;
    std::uint64_t snapshots   = 0;
    std::uint64_t undecodable = 0;
    std::uint64_t resyncs     = 0;
    Qty           buy_vol     = 0;
    Qty           sell_vol    = 0;
    std::int64_t  first_ns    = 0;
    std::int64_t  last_ns     = 0;
    std::uint64_t checksum    = 0;
    std::string   meta;
    bool          crossed     = false;
    bool          truncated   = false;
    double        replay_secs = 0.0;
    std::optional<Level> best_bid, best_ask;
    std::size_t          bid_levels = 0, ask_levels = 0;
};

Stats replay(const std::string& path) {
    Stats                 st;
    GzLineReader          reader(path);
    simdjson::dom::parser parser;
    DepthSync             sync;

    const auto t0 = std::chrono::steady_clock::now();

    // Dispatch on the payload's own "e" field. Shared by the format-2 path
    // below and, in spirit, by the live loop in main.cpp -- one definition of
    // what a message means, or replay stops describing the live program.
    const auto handle_payload = [&](simdjson::dom::element data) {
        std::string_view type;
        if (data["e"].get(type)) {
            ++st.undecodable;
            return;
        }
        if (type == "depthUpdate") {
            ++st.events;
            const auto ev = decode_depth_event(data);
            if (!ev) {
                ++st.undecodable;
                return;
            }
            (void)sync.on_event(*ev);
        } else if (type == "trade" || type == "aggTrade") {
            ++st.trades;
            const auto tr = decode_trade(data);
            if (!tr) {
                ++st.undecodable;
                return;
            }
            (tr->hit_bid() ? st.sell_vol : st.buy_vol) += tr->qty;
        }
    };

    std::string_view line;
    while (reader.next(line)) {
        if (line.empty()) continue;
        ++st.records;

        simdjson::dom::element env;
        if (parser.parse(line.data(), line.size()).get(env)) {
            ++st.undecodable;  // truncated final line of a killed recording, usually
            continue;
        }

        std::int64_t     t = 0;
        std::string_view kind;
        if (env["t"].get(t) || env["k"].get(kind)) {
            ++st.undecodable;
            continue;
        }
        if (st.first_ns == 0) st.first_ns = t;
        st.last_ns = t;

        simdjson::dom::element payload;
        if (env["d"].get(payload)) {
            ++st.undecodable;
            continue;
        }

        if (kind == "h") {
            st.meta = simdjson::to_string(payload);
        } else if (kind == "s") {
            ++st.snapshots;
            const auto snap = decode_depth_snapshot(payload);
            if (!snap) {
                ++st.undecodable;
                continue;
            }
            // A resync request here is moot: the recording already contains
            // whatever snapshot came next, so we simply keep reading.
            (void)sync.on_snapshot(*snap);
        } else if (kind == "e") {
            // Format 1: the record kind said "depth event" and meant it.
            ++st.events;
            const auto ev = decode_depth_event(payload);
            if (!ev) {
                ++st.undecodable;
                continue;
            }
            (void)sync.on_event(*ev);
        } else if (kind == "w") {
            // Format 2: any WebSocket message. Strip the combined-stream
            // envelope, then let the payload say what it is.
            handle_payload(unwrap_stream_payload(payload));
        }
    }

    st.replay_secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    st.resyncs     = sync.resync_count();
    st.checksum    = book_checksum(sync.book());
    st.crossed     = sync.book().crossed();
    st.truncated   = reader.truncated();
    st.best_bid    = sync.book().best_bid();
    st.best_ask    = sync.book().best_ask();
    st.bid_levels  = sync.book().bid_levels();
    st.ask_levels  = sync.book().ask_levels();
    return st;
}

void print(const Stats& st, const std::string& path) {
    const double span = static_cast<double>(st.last_ns - st.first_ns) / 1e9;

    std::printf("replay %s\n", path.c_str());
    std::printf("  meta         %s\n", st.meta.c_str());
    std::printf("  records      %llu  (snapshots %llu, depth %llu, trades %llu)\n",
                (unsigned long long)st.records, (unsigned long long)st.snapshots,
                (unsigned long long)st.events, (unsigned long long)st.trades);
    std::printf("  undecodable  %llu\n", (unsigned long long)st.undecodable);
    std::printf("  resyncs      %llu\n", (unsigned long long)st.resyncs);
    if (st.truncated) std::printf("  truncated    yes (recorder was killed; data up to that point is valid)\n");
    std::printf("  wall span    %.1f s\n", span);
    std::printf("  replay time  %.3f s   (%.0fx realtime)\n", st.replay_secs,
                st.replay_secs > 0 ? span / st.replay_secs : 0.0);

    const double total = static_cast<double>(st.buy_vol + st.sell_vol);
    if (total > 0) {
        std::printf("  trade flow   buy %.3f / sell %.3f   imbalance %+.3f\n",
                    to_double(st.buy_vol), to_double(st.sell_vol),
                    static_cast<double>(st.buy_vol - st.sell_vol) / total);
    }

    if (st.best_bid && st.best_ask) {
        std::printf("  final book   bid %.2f / ask %.2f   levels %zu/%zu\n",
                    to_double(st.best_bid->px), to_double(st.best_ask->px), st.bid_levels,
                    st.ask_levels);
    } else {
        std::printf("  final book   (empty -- ended out of sync)\n");
    }
    std::printf("  crossed      %s\n", st.crossed ? "YES  *** CORRUPT ***" : "no");
    std::printf("  checksum     0x%016llx\n", (unsigned long long)st.checksum);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: hftreplay <recording.jsonl.gz> [--twice]\n");
        return 2;
    }
    const std::string path  = argv[1];
    const bool        twice = (argc > 2 && std::string(argv[2]) == "--twice");

    try {
        const Stats a = replay(path);
        print(a, path);

        if (twice) {
            const Stats b    = replay(path);
            const bool  same = a.checksum == b.checksum && a.resyncs == b.resyncs &&
                              a.events == b.events && a.trades == b.trades;
            std::printf("\n  determinism  %s  (second pass checksum 0x%016llx)\n",
                        same ? "PASS" : "FAIL", (unsigned long long)b.checksum);
            return same ? 0 : 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "replay failed: %s\n", e.what());
        return 1;
    }
    return 0;
}
