// Deterministic replay of a recording.
//
//   ./build/hftreplay data/btcusdt.jsonl.gz
//   ./build/hftreplay data/btcusdt.jsonl.gz --twice     prove determinism
//
// The whole point: the same bytes, through the same DepthSync, produce the same
// book -- every time, as fast as the disk allows, with no exchange involved.

#include "replay_engine.hpp"

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

struct Collector {
    DepthSync   sync;
    std::string meta;
    Qty         buy_vol  = 0;
    Qty         sell_vol = 0;

    void on_header(std::string_view m) { meta = std::string(m); }
    void on_snapshot(const DepthSnapshot& s, std::int64_t) { (void)sync.on_snapshot(s); }
    void on_depth(const DepthUpdate& e, std::int64_t) { (void)sync.on_event(e); }
    void on_trade(const Trade& t, std::int64_t) {
        (t.hit_bid() ? sell_vol : buy_vol) += t.qty;
    }
};

struct Result {
    ReplayStats   st;
    std::uint64_t checksum = 0;
    std::uint64_t resyncs  = 0;
    bool          crossed  = false;
    double        secs     = 0.0;
    Qty           buy_vol = 0, sell_vol = 0;
    std::string   meta;
    std::optional<Level> best_bid, best_ask;
    std::size_t          bid_levels = 0, ask_levels = 0;
};

Result run(const std::string& path) {
    Collector  c;
    const auto t0 = std::chrono::steady_clock::now();
    Result     r;
    r.st   = replay_file(path, c);
    r.secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    r.checksum   = book_checksum(c.sync.book());
    r.resyncs    = c.sync.resync_count();
    r.crossed    = c.sync.book().crossed();
    r.meta       = c.meta;
    r.buy_vol    = c.buy_vol;
    r.sell_vol   = c.sell_vol;
    r.best_bid   = c.sync.book().best_bid();
    r.best_ask   = c.sync.book().best_ask();
    r.bid_levels = c.sync.book().bid_levels();
    r.ask_levels = c.sync.book().ask_levels();
    return r;
}

void print(const Result& r, const std::string& path) {
    const double span = static_cast<double>(r.st.last_ns - r.st.first_ns) / 1e9;

    std::printf("replay %s\n", path.c_str());
    std::printf("  meta         %s\n", r.meta.c_str());
    std::printf("  records      %llu  (snapshots %llu, depth %llu, trades %llu)\n",
                (unsigned long long)r.st.records, (unsigned long long)r.st.snapshots,
                (unsigned long long)r.st.depth, (unsigned long long)r.st.trades);
    std::printf("  undecodable  %llu\n", (unsigned long long)r.st.undecodable);
    std::printf("  resyncs      %llu\n", (unsigned long long)r.resyncs);
    if (r.st.truncated)
        std::printf("  truncated    yes (recorder was killed; earlier data is valid)\n");
    std::printf("  wall span    %.1f s\n", span);
    std::printf("  replay time  %.3f s   (%.0fx realtime)\n", r.secs,
                r.secs > 0 ? span / r.secs : 0.0);

    const double total = static_cast<double>(r.buy_vol + r.sell_vol);
    if (total > 0) {
        std::printf("  trade flow   buy %.3f / sell %.3f   imbalance %+.3f\n",
                    to_double(r.buy_vol), to_double(r.sell_vol),
                    static_cast<double>(r.buy_vol - r.sell_vol) / total);
    }

    if (r.best_bid && r.best_ask) {
        std::printf("  final book   bid %.2f / ask %.2f   levels %zu/%zu\n",
                    to_double(r.best_bid->px), to_double(r.best_ask->px), r.bid_levels,
                    r.ask_levels);
    } else {
        std::printf("  final book   (empty -- ended out of sync)\n");
    }
    std::printf("  crossed      %s\n", r.crossed ? "YES  *** CORRUPT ***" : "no");
    std::printf("  checksum     0x%016llx\n", (unsigned long long)r.checksum);
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
        const Result a = run(path);
        print(a, path);

        if (twice) {
            const Result b    = run(path);
            const bool   same = a.checksum == b.checksum && a.resyncs == b.resyncs &&
                              a.st.depth == b.st.depth && a.st.trades == b.st.trades;
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
