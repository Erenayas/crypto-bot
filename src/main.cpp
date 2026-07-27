// Phase 1/2 deliverable: a live, verified L2 order book plus trade flow.
//
//   ./build/hftbot                    BTCUSDT from mainnet market data
//   ./build/hftbot --symbol ethusdt   another symbol
//   ./build/hftbot --record f.gz      also write a replayable recording
//   ./build/hftbot --testnet          testnet market data (see note below)

#include "binance_net.hpp"
#include "depth_sync.hpp"
#include "json_decode.hpp"
#include "recording.hpp"
#include "trade.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace hft;

// Market data requires NO API KEY, so by default we read the real book from
// mainnet. This is completely safe -- we are only listening; no order can be
// placed without signed, authenticated requests.
//
// It is also the correct choice: the testnet book is thin and synthetic, so
// spreads, depth and fill rates there bear no resemblance to reality. Every
// strategy number we produce from it in Week 3 would be meaningless. Testnet is
// where the ORDERS go in Week 4; mainnet is where the DATA comes from now.
constexpr char kWsHostLive[]   = "fstream.binance.com";
constexpr char kRestHostLive[] = "fapi.binance.com";
constexpr char kWsHostTest[]   = "stream.binancefuture.com";
constexpr char kRestHostTest[] = "testnet.binancefuture.com";

// Clean shutdown. A recorder that is SIGKILLed loses whatever sits in zlib's
// deflate buffer, so we catch the polite signals and close the file properly.
// std::atomic<bool> is lock-free on every platform we care about, which is the
// requirement for touching it from a signal handler.
std::atomic<bool> g_stop{false};

extern "C" void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

struct FeedStats {
    std::uint64_t        events    = 0;
    std::uint64_t        trades    = 0;
    std::uint64_t        undecoded = 0;
    Qty                  buy_vol   = 0;  // aggressive buying: lifted asks
    Qty                  sell_vol  = 0;  // aggressive selling: hit bids
    std::optional<Trade> last;
};

// Display only. BTCUSDT quotes to 2 decimals and sizes to 3; Week 4 will read
// the real tickSize/stepSize per symbol from /fapi/v1/exchangeInfo.
void print_book(const DepthSync& sync, const std::string& symbol, int levels,
                const FeedStats& st) {
    const auto& book = sync.book();

    std::printf("\033[2J\033[H");  // clear screen, home cursor
    std::printf("%s   %s   events %llu   trades %llu   resyncs %llu   undecoded %llu\n\n",
                symbol.c_str(), sync.synced() ? "SYNCED" : "SYNCING",
                static_cast<unsigned long long>(st.events),
                static_cast<unsigned long long>(st.trades),
                static_cast<unsigned long long>(sync.resync_count()),
                static_cast<unsigned long long>(st.undecoded));

    // Asks are printed worst-to-best so the book reads top-down like an
    // exchange UI, with the spread in the middle.
    const auto n_asks = std::min<std::size_t>(static_cast<std::size_t>(levels), book.ask_levels());
    const std::vector<std::pair<Price, Qty>> asks(
        book.asks().begin(), std::next(book.asks().begin(), static_cast<long>(n_asks)));
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
        std::printf("        ASK  %12.2f   x %10.3f\n", to_double(it->first), to_double(it->second));
    }

    if (const auto mid = book.mid(); mid) {
        const auto micro  = book.microprice();
        const auto spread = book.spread();
        std::printf("  ----  spread %.2f   mid %.4f   micro %.4f   skew %+.4f\n",
                    to_double(*spread), *mid, *micro, *micro - *mid);
    } else {
        std::printf("  ----  (no book yet)\n");
    }

    int shown = 0;
    for (auto it = book.bids().begin(); it != book.bids().end() && shown < levels; ++it, ++shown) {
        std::printf("        BID  %12.2f   x %10.3f\n", to_double(it->first), to_double(it->second));
    }

    // Trade flow imbalance: of the size that CROSSED the spread, how much was
    // buying? This is a second, independent alpha signal -- the book tells you
    // what people are willing to do, trades tell you what they actually did.
    const double total = static_cast<double>(st.buy_vol + st.sell_vol);
    if (total > 0) {
        const double ofi = static_cast<double>(st.buy_vol - st.sell_vol) / total;
        std::printf("\n  flow  buy %.3f / sell %.3f   imbalance %+.3f\n",
                    to_double(st.buy_vol), to_double(st.sell_vol), ofi);
    }
    if (st.last) {
        std::printf("  last  %.2f x %.3f   aggressor %s\n", to_double(st.last->px),
                    to_double(st.last->qty), st.last->hit_bid() ? "SELL" : "BUY");
    }

    if (book.crossed()) {
        std::printf("\n  *** BOOK IS CROSSED -- reconstruction is wrong ***\n");
    }
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    std::string symbol = "btcusdt";
    std::string record_path;
    bool        testnet = false;
    int         levels  = 5;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--symbol" && i + 1 < argc) symbol = argv[++i];
        else if (arg == "--levels" && i + 1 < argc) levels = std::atoi(argv[++i]);
        else if (arg == "--record" && i + 1 < argc) record_path = argv[++i];
        else if (arg == "--testnet") testnet = true;
        else {
            std::fprintf(stderr,
                         "usage: hftbot [--symbol btcusdt] [--levels 5] [--testnet]\n"
                         "              [--record data/btcusdt.jsonl.gz]\n");
            return 2;
        }
    }

    const std::string ws_host   = testnet ? kWsHostTest : kWsHostLive;
    const std::string rest_host = testnet ? kRestHostTest : kRestHostLive;

    // ONE combined stream, not two connections.
    //
    // Two sockets would give two INDEPENDENT orderings, and we would have no
    // reliable way to know whether a trade happened before or after a given book
    // update. The fill model depends entirely on that ordering: "was my level
    // consumed by this trade, or had it already been cancelled?" is unanswerable
    // if the two feeds can arrive out of order relative to each other.
    const std::string stream =
        "/stream?streams=" + symbol + "@depth@100ms/" + symbol + "@trade";
    const std::string snap_path = "/fapi/v1/depth?symbol=" + to_upper(symbol) + "&limit=1000";

    DepthSync      sync;
    MessageDecoder decoder;
    FeedStats      st;

    std::optional<Recorder> recorder;
    if (!record_path.empty()) {
        const std::string meta = "{\"format\":" + std::to_string(kRecordingFormat) +
                                 ",\"symbol\":\"" + to_upper(symbol) + "\",\"stream\":\"" + stream +
                                 "\",\"ws_host\":\"" + ws_host + "\",\"rest_host\":\"" + rest_host +
                                 "\"}";
        recorder.emplace(record_path, meta);
        std::fprintf(stderr, "recording to %s\n", record_path.c_str());
    }

    // Fetch a snapshot and drive the sync state machine until it takes. On a
    // fast stream the first snapshot can already be outrun by buffered events;
    // that is normal, and the fix is simply a newer snapshot.
    auto resync = [&] {
        for (int attempt = 0; attempt < 8; ++attempt) {
            const std::string body = https_get(rest_host, snap_path);
            if (recorder) recorder->write(RecordKind::Snapshot, body, now_ns());

            const auto snap = decoder.decode_snapshot(body);
            if (!snap) throw std::runtime_error("could not decode depth snapshot");
            if (sync.on_snapshot(*snap) == SyncAction::None) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        throw std::runtime_error("could not synchronise book after 8 attempts");
    };

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGALRM, on_signal);

    auto last_draw = std::chrono::steady_clock::now();

    while (!g_stop) {
        WebSocketClient ws;
        try {
            // ORDER MATTERS: the stream is opened BEFORE the snapshot is
            // requested. Events published while the REST call is in flight then
            // sit in the kernel's receive buffer and are read afterwards, so
            // nothing between the snapshot and our first read is lost.
            ws.connect(ws_host, stream);
            sync.reset();
            resync();

            while (!g_stop) {
                const auto msg = ws.read();

                // Record BEFORE decoding, and record everything -- including
                // messages we cannot decode. The recording is ground truth; a
                // decoder bug must be fixable after the fact, not baked in.
                if (recorder) recorder->write(RecordKind::WsMessage, msg, now_ns());

                simdjson::dom::element payload;
                if (!decoder.parse_message(msg, payload)) {
                    ++st.undecoded;
                    continue;
                }

                std::string_view type;
                if (payload["e"].get(type)) {
                    ++st.undecoded;
                    continue;
                }

                if (type == "depthUpdate") {
                    ++st.events;
                    const auto ev = decode_depth_event(payload);
                    if (!ev) {
                        ++st.undecoded;
                        continue;
                    }
                    if (sync.on_event(*ev) == SyncAction::RequestSnapshot) resync();
                } else if (type == "trade" || type == "aggTrade") {
                    ++st.trades;
                    const auto tr = decode_trade(payload);
                    if (!tr) {
                        ++st.undecoded;
                        continue;
                    }
                    (tr->hit_bid() ? st.sell_vol : st.buy_vol) += tr->qty;
                    st.last = *tr;
                }

                const auto now = std::chrono::steady_clock::now();
                if (now - last_draw > std::chrono::milliseconds(250)) {
                    last_draw = now;
                    print_book(sync, to_upper(symbol), levels, st);
                    // Bound how much of the recording a kill -9 can cost us.
                    if (recorder) recorder->flush();
                }
            }
        } catch (const std::exception& e) {
            // A signal interrupts the blocking read, which surfaces here as a
            // network error. Check the flag before assuming the network broke.
            if (g_stop) break;
            std::fprintf(stderr, "\n[net] %s -- reconnecting in 1s\n", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    if (recorder) {
        recorder->flush();
        std::fprintf(stderr, "\nstopped: %llu depth events + %llu trades recorded to %s\n",
                     static_cast<unsigned long long>(st.events),
                     static_cast<unsigned long long>(st.trades), record_path.c_str());
    }
    return 0;  // Recorder's destructor closes the gzip stream cleanly.
}
