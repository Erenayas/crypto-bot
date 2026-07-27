// Backtest a market-making strategy over a recording.
//
//   ./build/hftbacktest data/btcusdt.jsonl.gz
//   ./build/hftbacktest data/x.jsonl.gz --gamma 0 --microprice 0    naive baseline
//   ./build/hftbacktest data/x.jsonl.gz --csv out.csv               P&L curve

#include "fill_sim.hpp"
#include "portfolio.hpp"
#include "replay_engine.hpp"
#include "strategy.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace {

using namespace hft;

struct Config {
    std::string path;
    std::string csv;
    double      maker_fee     = 0.0002;  // 2 bp, Binance USD-M base tier
    double      tick          = 0.10;    // BTCUSDT
    double      size          = 0.002;   // per quote
    double      max_position  = 0.010;
    double      gamma         = 1.0;
    double      base_half     = 1.0;     // ticks
    double      vol_coeff     = 0.0;
    bool        microprice    = true;
    double      requote_ticks = 1.0;
    std::int64_t markout_ms   = 1000;
};

class Backtester {
public:
    explicit Backtester(const Config& c)
        : cfg_(c),
          markout_(c.markout_ms * 1'000'000),
          vol_(0.02),
          mm_(make_params(c)) {
        if (!c.csv.empty()) {
            csv_ = std::fopen(c.csv.c_str(), "w");
            if (csv_) std::fprintf(csv_, "ts_ns,mid,position,equity,fills\n");
        }
    }

    ~Backtester() {
        if (csv_) std::fclose(csv_);
    }

    void on_header(std::string_view meta) { meta_ = std::string(meta); }

    void on_snapshot(const DepthSnapshot& s, std::int64_t ns) {
        (void)sync_.on_snapshot(s);
        after_book(ns);
    }

    void on_depth(const DepthUpdate& e, std::int64_t ns) {
        (void)sync_.on_event(e);
        after_book(ns);
    }

    void on_trade(const Trade& t, std::int64_t ns) {
        if (!sync_.synced()) return;
        fills_.clear();
        sim_.on_trade(t, ns, fills_);
        for (const auto& f : fills_) {
            pf_.on_fill(f, cfg_.maker_fee);
            markout_.on_fill(f);
        }
    }

    const Portfolio&      portfolio() const { return pf_; }
    const MarkoutTracker& markout() const { return markout_; }
    const FillSimulator&  sim() const { return sim_; }
    const std::string&    meta() const { return meta_; }
    double                last_mid() const { return last_mid_; }
    std::uint64_t         quoting_updates() const { return quoting_; }
    std::uint64_t         book_updates() const { return updates_; }

private:
    static MMParams make_params(const Config& c) {
        MMParams p;
        p.tick            = *parse_fixed(std::to_string(c.tick));
        p.size            = *parse_fixed(std::to_string(c.size));
        p.max_position    = *parse_fixed(std::to_string(c.max_position));
        p.base_half_ticks = c.base_half;
        p.vol_coeff       = c.vol_coeff;
        p.gamma           = c.gamma;
        p.use_microprice  = c.microprice;
        return p;
    }

    void after_book(std::int64_t ns) {
        // A book we cannot verify is a book we must not quote on. Pull
        // everything and wait for the resync -- exactly what the live bot will
        // do, so the backtest keeps describing the real program.
        if (!sync_.synced()) {
            sim_.cancel_all();
            return;
        }

        const OrderBook& book = sync_.book();
        const auto       mid  = book.mid();
        if (!mid) return;

        ++updates_;
        last_mid_ = *mid;
        vol_.update(*mid);

        // Order matters: clamp queue positions against the new book BEFORE
        // deciding where to quote.
        sim_.on_book(book);
        markout_.on_mark(ns, *mid);

        if (!vol_.ready()) return;  // don't trade on an unwarmed volatility estimate
        ++quoting_;

        const Quote q         = mm_.quote(book, pf_.position, vol_.sigma());
        const Price threshold = static_cast<Price>(cfg_.requote_ticks * static_cast<double>(mm_.params().tick));

        requote(Side::Buy, q.bid, q.bid_px, threshold, book);
        requote(Side::Sell, q.ask, q.ask_px, threshold, book);

        if (csv_ && updates_ % 200 == 0) {
            std::fprintf(csv_, "%lld,%.2f,%.6f,%.6f,%llu\n", (long long)ns, *mid,
                         to_double(pf_.position), pf_.equity(*mid),
                         (unsigned long long)pf_.fills);
        }
    }

    // Hysteresis. Without it the strategy cancels and replaces on every tick,
    // and since a replace goes to the BACK of the new queue, it would never
    // reach the front of anything and would essentially never fill. Requoting
    // is not free -- queue position is the asset being spent.
    void requote(Side side, bool want, Price px, Price threshold, const OrderBook& book) {
        const auto& o = side == Side::Buy ? sim_.bid() : sim_.ask();
        if (!want) {
            sim_.cancel(side);
            return;
        }
        if (o.active && std::llabs(o.px - px) < threshold) return;  // close enough; stay put
        sim_.place(side, px, mm_.params().size, book);
    }

    Config             cfg_;
    DepthSync          sync_;
    FillSimulator      sim_;
    Portfolio          pf_;
    MarkoutTracker     markout_;
    VolEstimator       vol_;
    MarketMaker        mm_;
    std::vector<Fill>  fills_;
    std::string        meta_;
    double             last_mid_ = 0.0;
    std::uint64_t      updates_  = 0;
    std::uint64_t      quoting_  = 0;
    std::FILE*         csv_      = nullptr;
};

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: hftbacktest <recording.jsonl.gz> [options]\n"
                     "  --gamma F         inventory risk aversion (0 = no skew)\n"
                     "  --microprice 0|1  fair value source\n"
                     "  --half F          base half-spread in ticks\n"
                     "  --vol F           volatility coefficient\n"
                     "  --size F          quote size\n"
                     "  --max-pos F       hard inventory limit\n"
                     "  --fee F           maker fee rate (0.0002 = 2bp)\n"
                     "  --requote F       requote threshold in ticks\n"
                     "  --csv PATH        write the P&L curve\n");
        return 2;
    }
    cfg.path = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const auto        next = [&] { return std::atof(argv[++i]); };
        if (a == "--gamma") cfg.gamma = next();
        else if (a == "--microprice") cfg.microprice = next() != 0.0;
        else if (a == "--half") cfg.base_half = next();
        else if (a == "--vol") cfg.vol_coeff = next();
        else if (a == "--size") cfg.size = next();
        else if (a == "--max-pos") cfg.max_position = next();
        else if (a == "--fee") cfg.maker_fee = next();
        else if (a == "--requote") cfg.requote_ticks = next();
        else if (a == "--markout-ms") cfg.markout_ms = static_cast<std::int64_t>(next());
        else if (a == "--csv" && i + 1 < argc) cfg.csv = argv[++i];
        else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return 2;
        }
    }

    try {
        Backtester bt(cfg);
        const auto t0 = std::chrono::steady_clock::now();
        const auto st = replay_file(cfg.path, bt);
        const auto secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        const auto&  pf   = bt.portfolio();
        const double mid  = bt.last_mid();
        const double net  = pf.equity(mid);
        const double span = static_cast<double>(st.last_ns - st.first_ns) / 1e9;
        const double vol_notional = to_double(pf.volume) * mid;

        std::printf("backtest %s\n", cfg.path.c_str());
        std::printf("  meta          %s\n", bt.meta().c_str());
        std::printf("  strategy      gamma %.2f   fair %s   half %.1f tick   vol %.2f\n",
                    cfg.gamma, cfg.microprice ? "microprice" : "mid", cfg.base_half,
                    cfg.vol_coeff);
        std::printf("  data          %.1f s   depth %llu   trades %llu   replayed in %.2f s\n",
                    span, (unsigned long long)st.depth, (unsigned long long)st.trades, secs);
        std::printf("\n");
        std::printf("  quotes placed %llu\n", (unsigned long long)bt.sim().placements());
        std::printf("  fills         %llu\n", (unsigned long long)pf.fills);
        std::printf("  volume        %.4f  (notional %.0f)\n", to_double(pf.volume), vol_notional);
        std::printf("  final pos     %+.4f   (max long %+.4f / max short %+.4f)\n",
                    to_double(pf.position), to_double(pf.max_long), to_double(pf.max_short));
        std::printf("\n");
        std::printf("  gross P&L     %+.4f\n", net + pf.fees_paid);
        std::printf("  fees paid     %-.4f\n", pf.fees_paid);
        std::printf("  NET P&L       %+.4f", net);
        if (vol_notional > 0) std::printf("   (%+.3f bp of volume)", net / vol_notional * 10000.0);
        std::printf("\n\n");
        std::printf("  markout %lldms  %+.4f   (%+.3f bp over %llu fills)\n",
                    (long long)cfg.markout_ms, bt.markout().total(), bt.markout().bps(),
                    (unsigned long long)bt.markout().count());
        if (!cfg.csv.empty()) std::printf("\n  P&L curve -> %s\n", cfg.csv.c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "backtest failed: %s\n", e.what());
        return 1;
    }
    return 0;
}
