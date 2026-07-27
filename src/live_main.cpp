// Live market maker on Binance USD-M futures TESTNET.
//
//   export BINANCE_API_KEY=...  BINANCE_API_SECRET=...
//   ./build/hftlive                  dry run: quotes computed and logged, nothing sent
//   ./build/hftlive --live           actually place orders on TESTNET
//
// SAFETY DECISIONS, deliberate and not configurable:
//
//   * Orders go to TESTNET only. There is no mainnet code path. Trading real
//     money should be a change someone makes on purpose, in a diff, not a flag
//     they can typo.
//   * Dry run is the default. --live is required to send anything.
//   * Market data comes from the SAME venue we trade on. Quoting mainnet prices
//     into the testnet book would place orders at prices that make no sense
//     there -- the books are genuinely different.
//   * All orders are post-only (GTX). We are a maker or we are nothing.
//   * Every order passes the risk gate. Any exception cancels everything.

#include "binance_net.hpp"
#include "depth_sync.hpp"
#include "exec_client.hpp"
#include "json_decode.hpp"
#include "risk.hpp"
#include "strategy.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace hft;

constexpr char kWsHost[]   = "stream.binancefuture.com";
constexpr char kRestHost[] = "testnet.binancefuture.com";

std::atomic<bool> g_stop{false};
extern "C" void   on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

std::string env_or_empty(const char* k) {
    const char* v = std::getenv(k);
    return v ? v : "";
}

// Loads KEY=VALUE lines from a .env file into the process environment, without
// overwriting anything already set.
//
// Credentials live in a gitignored file rather than in shell history or a
// command line. `ps` shows every running process's argv to every user on the
// machine, and shell history files are backed up, synced and pasted into bug
// reports. A secret that was ever an argument is a leaked secret.
void load_env_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return;

    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();

        const auto hash = s.find_first_not_of(" \t");
        if (hash == std::string::npos || s[hash] == '#') continue;

        const auto eq = s.find('=');
        if (eq == std::string::npos) continue;

        std::string key = s.substr(hash, eq - hash);
        std::string val = s.substr(eq + 1);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') &&
            val.back() == val.front()) {
            val = val.substr(1, val.size() - 2);
        }
        if (!key.empty()) ::setenv(key.c_str(), val.c_str(), 0 /* don't overwrite */);
    }
    std::fclose(f);
}

// The exchange's own clock. Binance rejects requests whose timestamp is outside
// recvWindow, and a drifting local clock produces an error that reads like a
// signature problem.
std::int64_t server_time() {
    const std::string      json = https_get(kRestHost, "/fapi/v1/time");
    simdjson::dom::parser  p;
    simdjson::dom::element d;
    if (p.parse(json.data(), json.size()).get(d)) return 0;
    std::int64_t t = 0;
    if (d["serverTime"].get(t)) return 0;
    return t;
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// One resting order per side, mirroring what the fill simulator models.
struct LiveOrder {
    std::int64_t id     = 0;
    Price        px     = 0;
    bool         active = false;
};

class LiveTrader {
public:
    LiveTrader(ExecClient& exec, MMParams mm, RiskConfig risk, bool dry)
        : exec_(exec), mm_(mm), risk_(risk), dry_(dry) {}

    void set_position(Qty p) { position_ = p; }
    void set_avg_entry(double e) { avg_entry_ = e; }
    Qty  position() const { return position_; }

    void on_book(const OrderBook& book, double sigma, std::int64_t book_ms) {
        const Quote q = mm_.quote(book, position_, sigma, avg_entry_);
        reconcile(Side::Buy, q.bid, q.bid_px, book_ms);
        reconcile(Side::Sell, q.ask, q.ask_px, book_ms);
    }

    // Cancel everything, then close any open position. Orders first: a resting
    // quote that fills while the market order is in flight would leave us with
    // a new position immediately after flattening.
    void flatten_all() {
        flatten_quotes();
        if (dry_ || position_ == 0) return;

        const Side side = position_ > 0 ? Side::Sell : Side::Buy;
        const Qty  qty  = position_ > 0 ? position_ : -position_;

        // A position smaller than the symbol's lot step rounds to "0.000" on
        // the wire and comes back as -4003. There is nothing to close and no
        // way to express it, so treat it as flat rather than reporting a
        // failure that needs manual attention -- crying wolf makes the real
        // "POSITION STILL OPEN" warning worthless.
        if (format_fixed(qty, exec_.spec().qty_dp) == format_fixed(0, exec_.spec().qty_dp)) {
            std::fprintf(stderr, "[exec] residual %s below lot step, treating as flat\n",
                         format_fixed(qty, 8).c_str());
            position_ = 0;
            return;
        }
        try {
            std::fprintf(stderr, "[exec] flattening %s %s (market, reduce-only)\n",
                         position_ > 0 ? "LONG" : "SHORT",
                         format_fixed(qty, exec_.spec().qty_dp).c_str());
            (void)exec_.close_position(side, qty, now_ms());
            position_ = 0;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[exec] FLATTEN FAILED: %s\n", e.what());
            std::fprintf(stderr, "[exec] *** POSITION STILL OPEN -- close it manually ***\n");
        }
    }

    void flatten_quotes() {
        if (!dry_) {
            try {
                exec_.cancel_all(now_ms());
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[exec] cancel_all failed: %s\n", e.what());
            }
        }
        bid_ = LiveOrder{};
        ask_ = LiveOrder{};
    }

    RiskGate&     risk() { return risk_; }
    std::uint64_t sent() const { return sent_; }
    std::uint64_t rejected() const { return rejected_; }

private:
    void reconcile(Side side, bool want, Price px, std::int64_t book_ms) {
        LiveOrder& o = side == Side::Buy ? bid_ : ask_;

        if (!want) {
            if (o.active) cancel(side, o);
            return;
        }
        // Hysteresis: moving a quote means losing queue position, so we only
        // move when the target has drifted at least a tick. Same reasoning as
        // the backtester, and the reason it is not a per-tick loop.
        if (o.active && std::llabs(o.px - px) < mm_.params().tick) return;

        const std::int64_t t = now_ms();
        // Check against the WORST CASE position, not the last polled one.
        //
        // Without a user-data stream we only learn our position from a poll
        // every few seconds, and fills happen in between. A live run breached a
        // 0.0040 limit by 2.2x for exactly this reason: the gate was reasoning
        // about a number that was already stale.
        //
        // So we assume any order still resting has already filled. That is
        // pessimistic and will occasionally block a legal quote -- which is the
        // correct direction to be wrong in for a risk check.
        const RiskVerdict v =
            risk_.check(side, mm_.params().size, worst_case_position(), t, book_ms);
        if (v != RiskVerdict::Allow) {
            ++rejected_;
            log_reject(side, v);
            if (o.active) cancel(side, o);
            return;
        }

        if (o.active) cancel(side, o);

        if (dry_) {
            std::printf("  DRY  %s %s x %s\n", side == Side::Buy ? "BID" : "ASK",
                        format_fixed(px, exec_.spec().price_dp).c_str(),
                        format_fixed(mm_.params().size, exec_.spec().qty_dp).c_str());
            o.px     = px;
            o.active = true;
            return;
        }

        try {
            const std::string res = exec_.place_post_only(side, px, mm_.params().size, t);
            risk_.record_sent(t);
            ++sent_;
            o.id     = parse_order_id(res);
            o.px     = px;
            o.active = true;
        } catch (const std::exception& e) {
            // A GTX rejection is NORMAL: the book moved and our "passive" price
            // would now cross. That is the protection working, not a failure.
            // A GTX rejection is the protection working: the book moved and
            // our passive price would now cross. Counted, not logged -- at a
            // 40% reject rate the log would be nothing else.
            ++rejected_;
            const std::string msg = e.what();
            if (msg.find("-5022") == std::string::npos) {
                std::fprintf(stderr, "[exec] place %s rejected: %s\n",
                             side == Side::Buy ? "BID" : "ASK", msg.c_str());
            }
            o.active = false;
        }
    }

    // Position we would hold if every resting order filled right now.
    Qty worst_case_position() const {
        // Take whichever direction is closer to a limit, so neither side can
        // sneak past while we reason about the other.
        const Qty long_case  = position_ + (bid_.active ? mm_.params().size : 0);
        const Qty short_case = position_ - (ask_.active ? mm_.params().size : 0);
        return std::llabs(long_case) >= std::llabs(short_case) ? long_case : short_case;
    }

    void cancel(Side side, LiveOrder& o) {
        if (!dry_ && o.id != 0) {
            try {
                exec_.cancel(o.id, now_ms());
            } catch (const std::exception& e) {
                // -2011 "Unknown order sent" means the order already filled or
                // was already gone. That is the NORMAL outcome of racing a
                // cancel against a fill, not a fault, so it is not worth a line
                // of log. Anything else is.
                const std::string msg = e.what();
                if (msg.find("-2011") == std::string::npos) {
                    std::fprintf(stderr, "[exec] cancel %s: %s\n",
                                 side == Side::Buy ? "BID" : "ASK", msg.c_str());
                }
            }
        }
        o = LiveOrder{};
    }

    void log_reject(Side side, RiskVerdict v) {
        std::fprintf(stderr, "[risk] %s blocked: %s\n", side == Side::Buy ? "BID" : "ASK",
                     to_string(v));
    }

    static std::int64_t parse_order_id(const std::string& json) {
        simdjson::dom::parser  p;
        simdjson::dom::element d;
        if (p.parse(json.data(), json.size()).get(d)) return 0;
        std::int64_t id = 0;
        if (d["orderId"].get(id)) return 0;
        return id;
    }

    ExecClient&   exec_;
    MarketMaker   mm_;
    RiskGate      risk_;
    bool          dry_;
    LiveOrder     bid_, ask_;
    Qty           position_ = 0;
    double        avg_entry_ = 0.0;
    std::uint64_t sent_     = 0;
    std::uint64_t rejected_ = 0;
};

// The exchange is authoritative about our position. We poll it rather than
// inferring from fills, because without a user-data stream we do not see fills
// at all -- and a local position that has silently drifted from the real one
// makes every risk limit meaningless.
Qty poll_position(ExecClient& exec) {
    const std::string      json = exec.position_risk(now_ms());
    simdjson::dom::parser  p;
    simdjson::dom::element d;
    if (p.parse(json.data(), json.size()).get(d)) return 0;

    simdjson::dom::array arr;
    if (d.get(arr)) return 0;
    for (auto e : arr) {
        std::string_view amt;
        if (!e["positionAmt"].get(amt)) {
            if (const auto v = parse_fixed(amt)) return *v;
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string symbol = "btcusdt";
    bool        live   = false;
    bool        check  = false;
    bool        flatten_only     = false;
    bool        flatten_on_exit  = false;
    MMParams    mm;
    RiskConfig  rk;

    // Defaults chosen by backtest sweep, not by taste (docs/04 section 4):
    // tight inventory limit plus strong skew was both the best-performing and
    // the least risky configuration -- inventory never accumulated at all.
    //
    // max_position is deliberately only 2x the quote size. Inventory you never
    // accumulate costs nothing to unwind, and unwinding is expensive: closing
    // 0.160 BTC cost 10.6 USDT in taker fees and slippage (docs/05 section 9).
    mm.tick            = *parse_fixed("0.10");
    mm.size            = *parse_fixed("0.002");
    mm.max_position    = *parse_fixed("0.004");
    mm.fee_bp          = 2.0;
    mm.base_half_ticks = 0.5;
    mm.gamma           = 50.0;
    mm.use_microprice  = true;
    // Refuse to quote closer than 1.25bp to fair value. Below that the fills we
    // get are dominated by traders who know something; above it we barely fill
    // at all. Swept in docs/04 section 8 -- this is the measured optimum, not a
    // guess, and it is the single largest strategy improvement in the project.
    mm.min_edge_bp     = 1.25;

    rk.max_position   = mm.max_position;
    rk.max_order_size = mm.size;
    rk.max_drawdown   = 50.0;  // USDT

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--symbol" && i + 1 < argc) symbol = argv[++i];
        else if (a == "--live") live = true;
        else if (a == "--check") check = true;
        else if (a == "--flatten") flatten_only = true;
        else if (a == "--flatten-on-exit") flatten_on_exit = true;
        else if (a == "--size" && i + 1 < argc) {
            mm.size = *parse_fixed(argv[++i]);
            rk.max_order_size = mm.size;
        } else if (a == "--max-pos" && i + 1 < argc) {
            mm.max_position = *parse_fixed(argv[++i]);
            rk.max_position = mm.max_position;
        } else if (a == "--gamma" && i + 1 < argc) mm.gamma = std::atof(argv[++i]);
        else if (a == "--half" && i + 1 < argc) mm.base_half_ticks = std::atof(argv[++i]);
        else if (a == "--min-edge-bp" && i + 1 < argc) mm.min_edge_bp = std::atof(argv[++i]);
        else if (a == "--max-drawdown" && i + 1 < argc) rk.max_drawdown = std::atof(argv[++i]);
        else {
            std::fprintf(stderr,
                         "usage: hftlive [--symbol btcusdt] [--live] [--check] [--size F]\n"
                         "               [--flatten] [--flatten-on-exit] [--min-edge-bp F]\n"
                         "               [--max-pos F] [--gamma F] [--half F]\n"
                         "               [--max-drawdown F]\n");
            return 2;
        }
    }

    load_env_file(".env");
    Credentials creds{env_or_empty("BINANCE_API_KEY"), env_or_empty("BINANCE_API_SECRET")};
    if ((live || check || flatten_only) && (creds.api_key.empty() || creds.secret.empty())) {
        std::fprintf(stderr,
                     "--live needs BINANCE_API_KEY and BINANCE_API_SECRET.\n"
                     "Put them in a .env file in the project root (it is gitignored):\n"
                     "  BINANCE_API_KEY=...\n"
                     "  BINANCE_API_SECRET=...\n"
                     "Testnet keys: https://testnet.binancefuture.com\n");
        return 2;
    }

    SymbolSpec spec;
    spec.symbol = symbol;
    std::transform(spec.symbol.begin(), spec.symbol.end(), spec.symbol.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    spec.tick = mm.tick;

    ExecClient exec(kRestHost, creds, spec);

    // Credential check: signed but READ-ONLY. Verifies the key, the secret, the
    // signing routine and the clock, without a single order reaching the book.
    // Never debug authentication by placing orders -- a failed order tells you
    // nothing about which of the four was wrong.
    if (check) {
        try {
            std::fprintf(stderr, "checking credentials against %s ...\n", kRestHost);
            const std::int64_t drift = now_ms() - server_time();
            std::fprintf(stderr, "  clock drift    %+lld ms%s\n", (long long)drift,
                         (drift > 1000 || drift < -1000) ? "   *** too large, fix NTP ***" : "  ok");

            const std::string pos = exec.position_risk(now_ms());
            std::fprintf(stderr, "  signed request OK\n");
            std::fprintf(stderr, "  position       %s\n",
                         format_fixed(poll_position(exec), spec.qty_dp).c_str());

            const std::string open = exec.open_orders(now_ms());
            std::fprintf(stderr, "  open orders    %s\n", open == "[]" ? "none" : open.c_str());
            (void)pos;
            std::fprintf(stderr, "\ncredentials work. safe to run --live\n");
            return 0;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "\nFAILED: %s\n", e.what());
            return 1;
        }
    }

    LiveTrader trader(exec, mm, rk, !(live || flatten_only));

    // Cancel everything and close the position, then exit. The panic button as
    // a standalone command, for when the bot is already gone and something is
    // still open.
    if (flatten_only) {
        try {
            trader.set_position(poll_position(exec));
            std::fprintf(stderr, "position before: %s\n",
                         format_fixed(trader.position(), spec.qty_dp).c_str());
            trader.flatten_all();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::fprintf(stderr, "position after:  %s\n",
                         format_fixed(poll_position(exec), spec.qty_dp).c_str());
            return 0;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "flatten failed: %s\n", e.what());
            return 1;
        }
    }

    std::fprintf(stderr, "%s %s on TESTNET   size %s   max pos %s   gamma %.1f\n",
                 spec.symbol.c_str(), live ? "LIVE (orders will be placed)" : "DRY RUN",
                 format_fixed(mm.size, spec.qty_dp).c_str(),
                 format_fixed(mm.max_position, spec.qty_dp).c_str(), mm.gamma);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGALRM, on_signal);

    const std::string stream    = "/stream?streams=" + symbol + "@depth@100ms/" + symbol + "@trade";
    const std::string snap_path = "/fapi/v1/depth?symbol=" + spec.symbol + "&limit=1000";

    DepthSync      sync;
    MessageDecoder decoder;
    VolEstimator   vol(0.02);

    auto last_poll  = std::chrono::steady_clock::now();
    auto last_print = last_poll;

    while (!g_stop) {
        WebSocketClient ws;
        try {
            ws.connect(kWsHost, stream);
            sync.reset();
            for (int attempt = 0;; ++attempt) {
                const auto snap = decoder.decode_snapshot(https_get(kRestHost, snap_path));
                if (!snap) throw std::runtime_error("cannot decode snapshot");
                if (sync.on_snapshot(*snap) == SyncAction::None) break;
                if (attempt >= 7) throw std::runtime_error("cannot synchronise book");
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (live) trader.set_position(poll_position(exec));

            while (!g_stop) {
                const auto msg = ws.read();

                simdjson::dom::element payload;
                if (!decoder.parse_message(msg, payload)) continue;
                std::string_view type;
                if (payload["e"].get(type)) continue;

                if (type == "depthUpdate") {
                    const auto ev = decode_depth_event(payload);
                    if (!ev) continue;
                    if (sync.on_event(*ev) == SyncAction::RequestSnapshot) {
                        // A book we cannot verify is a book we must not quote on.
                        trader.flatten_quotes();
                        break;  // reconnect path re-snapshots
                    }
                }
                if (!sync.synced()) continue;

                const auto mid = sync.book().mid();
                if (!mid) continue;
                vol.update(*mid);
                if (!vol.ready()) continue;

                trader.on_book(sync.book(), vol.sigma(), now_ms());

                const auto now = std::chrono::steady_clock::now();
                // Poll often. Every second between polls is a second the risk
                // gate spends reasoning about a stale position. The real fix is
                // the user-data stream (listenKey), which delivers fills as they
                // happen; this is the pragmatic version of it.
                if (live && now - last_poll > std::chrono::seconds(1)) {
                    last_poll = now;
                    trader.set_position(poll_position(exec));
                }
                if (now - last_print > std::chrono::seconds(2)) {
                    last_print = now;
                    std::printf("mid %.2f  pos %s  sent %llu  rejected %llu  risk-blocks %llu%s\n",
                                *mid, format_fixed(trader.position(), spec.qty_dp).c_str(),
                                (unsigned long long)trader.sent(),
                                (unsigned long long)trader.rejected(),
                                (unsigned long long)trader.risk().rejections(),
                                trader.risk().halted() ? "  *** HALTED ***" : "");
                    std::fflush(stdout);
                }
            }
        } catch (const std::exception& e) {
            if (g_stop) break;
            std::fprintf(stderr, "\n[net] %s -- flattening and reconnecting\n", e.what());
            trader.flatten_quotes();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    // Never leave orders resting in a market you have stopped watching.
    //
    // Closing the POSITION is opt-in, because it is a genuinely different
    // decision: it costs a taker fee and the spread, and a bot stopping is not
    // always a reason to exit a position. Whichever you choose, choose it --
    // the failure mode is leaving one open by accident.
    if (flatten_on_exit) {
        std::fprintf(stderr, "\nshutting down: cancelling orders and flattening position\n");
        trader.set_position(poll_position(exec));
        trader.flatten_all();
    } else {
        std::fprintf(stderr, "\nshutting down: cancelling all orders\n");
        trader.flatten_quotes();
        if (live) {
            const Qty p = poll_position(exec);
            if (p != 0) {
                std::fprintf(stderr,
                             "NOTE: position %s left OPEN. Use --flatten to close it, or\n"
                             "      --flatten-on-exit next time.\n",
                             format_fixed(p, spec.qty_dp).c_str());
            }
        }
    }
    return 0;
}
