// What the bot actually did, according to the exchange.
//
//   ./build/hftstatus              one snapshot
//   ./build/hftstatus --watch      refresh every 3 seconds
//   ./build/hftstatus --trades 50  show more fills
//
// The console log of hftlive shows what the bot INTENDED. This shows what
// happened. They differ every time an order is rejected, partially filled, or
// filled after we thought we had cancelled it -- so when the two disagree, this
// is the one that is right.

#include "exec_client.hpp"

#include <simdjson.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>

namespace {

using namespace hft;

std::string env_or_empty(const char* k) {
    const char* v = std::getenv(k);
    return v ? v : "";
}

void load_env_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return;
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        const auto start = s.find_first_not_of(" \t");
        if (start == std::string::npos || s[start] == '#') continue;
        const auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(start, eq - start);
        std::string val = s.substr(eq + 1);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        if (!key.empty()) ::setenv(key.c_str(), val.c_str(), 0);
    }
    std::fclose(f);
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string hhmmss(std::int64_t ms) {
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm           tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

void show(ExecClient& exec, int n_trades) {
    simdjson::dom::parser parser;

    std::printf("\033[2J\033[H");
    std::printf("=== %s on TESTNET ===\n\n", exec.spec().symbol.c_str());

    // ---- balance
    {
        const std::string      json = exec.balance(now_ms());
        simdjson::dom::element d;
        if (!parser.parse(json.data(), json.size()).get(d)) {
            simdjson::dom::array arr;
            if (!d.get(arr)) {
                for (auto e : arr) {
                    std::string_view asset, bal, upnl;
                    if (e["asset"].get(asset) || e["balance"].get(bal)) continue;
                    if (std::atof(std::string(bal).c_str()) == 0.0) continue;
                    (void)e["crossUnPnl"].get(upnl);
                    std::printf("  %-6s balance %-16s unrealised %s\n", std::string(asset).c_str(),
                                std::string(bal).c_str(), std::string(upnl).c_str());
                }
            }
        }
    }

    // ---- position
    {
        const std::string      json = exec.position_risk(now_ms());
        simdjson::dom::element d;
        if (!parser.parse(json.data(), json.size()).get(d)) {
            simdjson::dom::array arr;
            if (!d.get(arr)) {
                for (auto e : arr) {
                    std::string_view amt, entry, upnl;
                    if (e["positionAmt"].get(amt)) continue;
                    (void)e["entryPrice"].get(entry);
                    (void)e["unRealizedProfit"].get(upnl);
                    std::printf("\n  position    %-12s  entry %-12s  unrealised %s\n",
                                std::string(amt).c_str(), std::string(entry).c_str(),
                                std::string(upnl).c_str());
                }
            }
        }
    }

    // ---- open orders
    {
        const std::string json = exec.open_orders(now_ms());
        std::printf("\n  OPEN ORDERS\n");
        simdjson::dom::element d;
        simdjson::dom::array   arr;
        if (!parser.parse(json.data(), json.size()).get(d) && !d.get(arr)) {
            bool any = false;
            for (auto e : arr) {
                std::string_view side, price, qty, filled;
                (void)e["side"].get(side);
                (void)e["price"].get(price);
                (void)e["origQty"].get(qty);
                (void)e["executedQty"].get(filled);
                std::printf("    %-5s %12s  x %-10s  filled %s\n", std::string(side).c_str(),
                            std::string(price).c_str(), std::string(qty).c_str(),
                            std::string(filled).c_str());
                any = true;
            }
            if (!any) std::printf("    (none -- hftlive cancels everything on shutdown)\n");
        }
    }

    // ---- fills
    {
        const std::string json = exec.user_trades(n_trades, now_ms());
        std::printf("\n  FILLS (last %d)\n", n_trades);
        simdjson::dom::element d;
        simdjson::dom::array   arr;
        double                 pnl = 0, comm = 0, notional = 0;
        int                    maker = 0, total = 0;

        if (!parser.parse(json.data(), json.size()).get(d) && !d.get(arr)) {
            for (auto e : arr) {
                std::string_view side, price, qty, rp, cm;
                std::int64_t     t = 0;
                bool             is_maker = false;
                (void)e["side"].get(side);
                (void)e["price"].get(price);
                (void)e["qty"].get(qty);
                (void)e["realizedPnl"].get(rp);
                (void)e["commission"].get(cm);
                (void)e["time"].get(t);
                (void)e["maker"].get(is_maker);

                const double p = std::atof(std::string(price).c_str());
                const double q = std::atof(std::string(qty).c_str());
                pnl += std::atof(std::string(rp).c_str());
                comm += std::atof(std::string(cm).c_str());
                notional += p * q;
                total++;
                if (is_maker) maker++;

                std::printf("    %s  %-4s %-9s @ %-11s  %-6s  pnl %s\n", hhmmss(t).c_str(),
                            std::string(side).c_str(), std::string(qty).c_str(),
                            std::string(price).c_str(), is_maker ? "MAKER" : "taker",
                            std::string(rp).c_str());
            }
        }

        if (total > 0) {
            std::printf("\n  fills %d   maker %d/%d", total, maker, total);
            if (maker < total) std::printf("  *** %d TAKER FILLS ***", total - maker);
            std::printf("\n  notional    %.2f USDT\n", notional);
            std::printf("  realised    %+.6f USDT\n", pnl);
            std::printf("  commission  %.6f USDT", comm);
            if (notional > 0) std::printf("   (%.2f bp of notional)", comm / notional * 10000.0);
            std::printf("\n  NET         %+.6f USDT", pnl - comm);
            if (notional > 0) std::printf("   (%+.3f bp)", (pnl - comm) / notional * 10000.0);
            std::printf("\n");
        } else {
            std::printf("    (no fills yet)\n");
        }
    }
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    bool watch    = false;
    int  n_trades = 20;
    std::string symbol = "BTCUSDT";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--watch") watch = true;
        else if (a == "--trades" && i + 1 < argc) n_trades = std::atoi(argv[++i]);
        else if (a == "--symbol" && i + 1 < argc) symbol = argv[++i];
        else {
            std::fprintf(stderr, "usage: hftstatus [--watch] [--trades N] [--symbol BTCUSDT]\n");
            return 2;
        }
    }

    load_env_file(".env");
    Credentials creds{env_or_empty("BINANCE_API_KEY"), env_or_empty("BINANCE_API_SECRET")};
    if (creds.api_key.empty() || creds.secret.empty()) {
        std::fprintf(stderr, "no credentials: put BINANCE_API_KEY / BINANCE_API_SECRET in .env\n");
        return 2;
    }

    SymbolSpec spec;
    spec.symbol = symbol;
    ExecClient  exec("testnet.binancefuture.com", creds, spec);

    try {
        do {
            show(exec, n_trades);
            if (watch) std::this_thread::sleep_for(std::chrono::seconds(3));
        } while (watch);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nstatus failed: %s\n", e.what());
        return 1;
    }
    return 0;
}
