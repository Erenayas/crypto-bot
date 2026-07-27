// Phase 1 deliverable: a live, verified L2 order book.
//
//   ./build/hftbot                    BTCUSDT from mainnet market data
//   ./build/hftbot --symbol ethusdt   another symbol
//   ./build/hftbot --testnet          testnet market data (see note below)

#include "binance_net.hpp"
#include "depth_sync.hpp"
#include "json_decode.hpp"
#include "recording.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <optional>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iterator>
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
constexpr char kWsHostLive[]  = "fstream.binance.com";
constexpr char kRestHostLive[] = "fapi.binance.com";
constexpr char kWsHostTest[]  = "stream.binancefuture.com";
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

// Display only. BTCUSDT quotes to 2 decimals and sizes to 3; Week 4 will read
// the real tickSize/stepSize per symbol from /fapi/v1/exchangeInfo.
void print_book(const DepthSync& sync, const std::string& symbol, int levels,
                std::uint64_t events, std::uint64_t undecoded) {
    const auto& book = sync.book();

    std::printf("\033[2J\033[H");  // clear screen, home cursor
    std::printf("%s   %s   events %llu   resyncs %llu   undecoded %llu\n\n",
                symbol.c_str(), sync.synced() ? "SYNCED" : "SYNCING",
                static_cast<unsigned long long>(events),
                static_cast<unsigned long long>(sync.resync_count()),
                static_cast<unsigned long long>(undecoded));

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

    if (book.crossed()) {
        std::printf("\n  *** BOOK IS CROSSED -- reconstruction is wrong ***\n");
    }
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    std::string symbol      = "btcusdt";
    std::string record_path;
    bool        testnet     = false;
    int         levels      = 5;

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
    const std::string stream    = "/ws/" + symbol + "@depth@100ms";
    const std::string snap_path = "/fapi/v1/depth?symbol=" + to_upper(symbol) + "&limit=1000";

    DepthSync    sync;
    DepthDecoder decoder;
    std::uint64_t events = 0, undecoded = 0;

    std::optional<Recorder> recorder;
    if (!record_path.empty()) {
        const std::string meta = "{\"format\":1,\"symbol\":\"" + to_upper(symbol) +
                                 "\",\"stream\":\"" + stream + "\",\"ws_host\":\"" + ws_host +
                                 "\",\"rest_host\":\"" + rest_host + "\"}";
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
                ++events;

                // Record BEFORE decoding, and record everything -- including
                // messages we cannot decode. The recording is ground truth; a
                // decoder bug must be fixable after the fact, not baked in.
                if (recorder) recorder->write(RecordKind::Event, msg, now_ns());

                const auto ev = decoder.decode_event(msg);
                if (!ev) {
                    ++undecoded;  // not a depth event, or malformed. Never guess.
                    continue;
                }

                if (sync.on_event(*ev) == SyncAction::RequestSnapshot) {
                    resync();
                }

                const auto now = std::chrono::steady_clock::now();
                if (now - last_draw > std::chrono::milliseconds(250)) {
                    last_draw = now;
                    print_book(sync, to_upper(symbol), levels, events, undecoded);
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
        std::fprintf(stderr, "\nstopped: %llu events recorded to %s\n",
                     static_cast<unsigned long long>(events), record_path.c_str());
    }
    return 0;  // Recorder's destructor closes the gzip stream cleanly.
}
