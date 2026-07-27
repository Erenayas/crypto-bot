// A live web dashboard for the bot, served on localhost.
//
//   ./build/hftdash                 http://127.0.0.1:8080
//   ./build/hftdash --port 9000 --initial 5000
//
// Why a local server rather than a static page: the data lives behind an API
// key, and every number here comes from the EXCHANGE on each request, not from
// the bot's own bookkeeping. When the bot's log and this page disagree, this
// page is right -- that is the whole point of reconciling against the venue.
//
// Bound to 127.0.0.1 only. This process holds credentials; it has no business
// listening on a public interface.

#include "binance_net.hpp"
#include "exec_client.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <simdjson.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace {

using namespace hft;
namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp       = net::ip::tcp;

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
        while (!key.empty() && key.back() == ' ') key.pop_back();
        if (!key.empty()) ::setenv(key.c_str(), s.substr(eq + 1).c_str(), 0);
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

std::string fmt(double v, int dp, bool sign = false) {
    char buf[64];
    std::snprintf(buf, sizeof buf, sign ? "%+.*f" : "%.*f", dp, v);
    return buf;
}

struct FillRow {
    std::string  time, side, qty, price, pnl;
    bool         maker = false;
};

struct Snapshot {
    double  usdt = 0, unrealised = 0, position = 0, entry = 0;
    double  realised = 0, commission = 0, notional = 0;
    int     fills = 0, maker = 0, open_orders = 0;
    std::vector<FillRow>    recent;
    std::vector<std::string> open_rows;
    std::string             error;
};

Snapshot gather(ExecClient& exec, int n_trades) {
    Snapshot              s;
    simdjson::dom::parser parser;

    try {
        simdjson::dom::element d;
        simdjson::dom::array   arr;

        // balance
        std::string json = exec.balance(now_ms());
        if (!parser.parse(json.data(), json.size()).get(d) && !d.get(arr)) {
            for (auto e : arr) {
                std::string_view asset, bal, upnl;
                if (e["asset"].get(asset) || e["balance"].get(bal)) continue;
                if (asset != "USDT") continue;
                s.usdt = std::atof(std::string(bal).c_str());
                if (!e["crossUnPnl"].get(upnl)) s.unrealised = std::atof(std::string(upnl).c_str());
            }
        }

        // position
        json = exec.position_risk(now_ms());
        if (!parser.parse(json.data(), json.size()).get(d) && !d.get(arr)) {
            for (auto e : arr) {
                std::string_view amt, entry;
                if (e["positionAmt"].get(amt)) continue;
                s.position = std::atof(std::string(amt).c_str());
                if (!e["entryPrice"].get(entry)) s.entry = std::atof(std::string(entry).c_str());
            }
        }

        // open orders
        json = exec.open_orders(now_ms());
        if (!parser.parse(json.data(), json.size()).get(d) && !d.get(arr)) {
            for (auto e : arr) {
                std::string_view side, price, qty, filled;
                (void)e["side"].get(side);
                (void)e["price"].get(price);
                (void)e["origQty"].get(qty);
                (void)e["executedQty"].get(filled);
                s.open_rows.push_back("<tr><td class=\"" +
                                      std::string(side == "BUY" ? "buy" : "sell") + "\">" +
                                      std::string(side) + "</td><td>" + std::string(price) +
                                      "</td><td>" + std::string(qty) + "</td><td>" +
                                      std::string(filled) + "</td></tr>");
                ++s.open_orders;
            }
        }

        // fills
        json = exec.user_trades(n_trades, now_ms());
        if (!parser.parse(json.data(), json.size()).get(d) && !d.get(arr)) {
            for (auto e : arr) {
                std::string_view side, price, qty, rp, cm;
                std::int64_t     t        = 0;
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
                s.realised += std::atof(std::string(rp).c_str());
                s.commission += std::atof(std::string(cm).c_str());
                s.notional += p * q;
                ++s.fills;
                if (is_maker) ++s.maker;

                s.recent.push_back(FillRow{hhmmss(t), std::string(side), std::string(qty),
                                           std::string(price), std::string(rp), is_maker});
            }
        }
    } catch (const std::exception& e) {
        s.error = e.what();
    }
    return s;
}

std::string render(const Snapshot& s, double initial, const std::string& symbol) {
    const double net    = s.realised - s.commission;
    const double total  = net + s.unrealised;
    const double roi     = initial > 0 ? total / initial * 100.0 : 0.0;
    const double net_bp  = s.notional > 0 ? net / s.notional * 10000.0 : 0.0;
    const double fee_bp  = s.notional > 0 ? s.commission / s.notional * 10000.0 : 0.0;
    const auto   cls     = [](double v) { return v > 0 ? "pos" : (v < 0 ? "neg" : ""); };

    std::string h;
    h += R"(<!doctype html><html><head><meta charset="utf-8">
<meta http-equiv="refresh" content="5">
<title>)" + symbol + R"( — bot</title><style>
:root{--bg:#0e1116;--card:#161b22;--line:#272d36;--fg:#e6edf3;--dim:#8b949e;
--pos:#3fb950;--neg:#f85149;--acc:#d29922}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);
font:14px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;padding:24px}
h1{font-size:16px;font-weight:600;margin:0 0 4px}
.sub{color:var(--dim);font-size:12px;margin-bottom:20px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin-bottom:20px}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:14px}
.label{color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.06em}
.val{font-size:22px;font-weight:600;margin-top:6px;font-variant-numeric:tabular-nums}
.note{color:var(--dim);font-size:11px;margin-top:4px}
.pos{color:var(--pos)}.neg{color:var(--neg)}
table{width:100%;border-collapse:collapse;font-size:13px}
th{text-align:left;color:var(--dim);font-weight:500;font-size:11px;text-transform:uppercase;
padding:6px 8px;border-bottom:1px solid var(--line)}
td{padding:5px 8px;border-bottom:1px solid var(--line);font-variant-numeric:tabular-nums}
.buy{color:var(--pos)}.sell{color:var(--neg)}
.sec{margin-top:22px}.sec h2{font-size:12px;color:var(--dim);text-transform:uppercase;
letter-spacing:.06em;margin:0 0 8px;font-weight:500}
.wrap{background:var(--card);border:1px solid var(--line);border-radius:8px;overflow-x:auto}
.tag{font-size:10px;padding:1px 5px;border-radius:3px;background:#21262d;color:var(--dim)}
.err{background:#3d1418;border:1px solid var(--neg);padding:10px;border-radius:6px;margin-bottom:16px}
</style></head><body>)";

    h += "<h1>" + symbol + " market maker <span class=\"tag\">TESTNET</span></h1>";
    h += "<div class=\"sub\">live from the exchange &middot; refreshes every 5s &middot; last " +
         std::to_string(s.fills) + " fills</div>";

    if (!s.error.empty()) h += "<div class=\"err\">exchange error: " + s.error + "</div>";

    h += "<div class=\"grid\">";
    h += "<div class=\"card\"><div class=\"label\">Total P&amp;L</div><div class=\"val " +
         std::string(cls(total)) + "\">" + fmt(total, 4, true) +
         "</div><div class=\"note\">realised + unrealised, USDT</div></div>";
    h += "<div class=\"card\"><div class=\"label\">ROI</div><div class=\"val " +
         std::string(cls(roi)) + "\">" + fmt(roi, 4, true) +
         "%</div><div class=\"note\">on " + fmt(initial, 0) + " USDT</div></div>";
    h += "<div class=\"card\"><div class=\"label\">Fills</div><div class=\"val\">" +
         std::to_string(s.fills) + "</div><div class=\"note\">" + std::to_string(s.maker) +
         " maker" + (s.maker < s.fills ? " &middot; <span class=\"neg\">" +
                                             std::to_string(s.fills - s.maker) + " taker</span>"
                                       : "") +
         "</div></div>";
    h += "<div class=\"card\"><div class=\"label\">Position</div><div class=\"val " +
         std::string(cls(s.position)) + "\">" + fmt(s.position, 4, true) +
         "</div><div class=\"note\">entry " + fmt(s.entry, 2) + "</div></div>";
    h += "<div class=\"card\"><div class=\"label\">Realised</div><div class=\"val " +
         std::string(cls(s.realised)) + "\">" + fmt(s.realised, 4, true) +
         "</div><div class=\"note\">before fees</div></div>";
    h += "<div class=\"card\"><div class=\"label\">Commission</div><div class=\"val neg\">-" +
         fmt(s.commission, 4) + "</div><div class=\"note\">" + fmt(fee_bp, 2) +
         " bp of notional</div></div>";
    h += "<div class=\"card\"><div class=\"label\">Net after fees</div><div class=\"val " +
         std::string(cls(net)) + "\">" + fmt(net, 4, true) + "</div><div class=\"note\">" +
         fmt(net_bp, 3, true) + " bp of volume</div></div>";
    h += "<div class=\"card\"><div class=\"label\">Volume</div><div class=\"val\">" +
         fmt(s.notional, 0) + "</div><div class=\"note\">USDT notional traded</div></div>";
    h += "</div>";

    h += "<div class=\"sec\"><h2>Open orders (" + std::to_string(s.open_orders) + ")</h2><div "
         "class=\"wrap\"><table><tr><th>Side</th><th>Price</th><th>Qty</th><th>Filled</th></tr>";
    if (s.open_rows.empty()) {
        h += "<tr><td colspan=\"4\" style=\"color:var(--dim)\">none &mdash; hftlive cancels "
             "everything on shutdown</td></tr>";
    }
    for (const auto& r : s.open_rows) h += r;
    h += "</table></div></div>";

    h += "<div class=\"sec\"><h2>Recent fills</h2><div class=\"wrap\"><table>"
         "<tr><th>Time</th><th>Side</th><th>Qty</th><th>Price</th><th>Role</th>"
         "<th>Realised</th></tr>";
    for (auto it = s.recent.rbegin(); it != s.recent.rend(); ++it) {
        const double p = std::atof(it->pnl.c_str());
        h += "<tr><td>" + it->time + "</td><td class=\"" +
             (it->side == "BUY" ? "buy" : "sell") + "\">" + it->side + "</td><td>" + it->qty +
             "</td><td>" + it->price + "</td><td>" +
             (it->maker ? "<span class=\"tag\">maker</span>"
                        : "<span class=\"neg\">TAKER</span>") +
             "</td><td class=\"" + cls(p) + "\">" + (p == 0 ? "&mdash;" : fmt(p, 6, true)) +
             "</td></tr>";
    }
    if (s.recent.empty())
        h += "<tr><td colspan=\"6\" style=\"color:var(--dim)\">no fills yet</td></tr>";
    h += "</table></div></div>";

    h += "<div class=\"sub\" style=\"margin-top:20px\">Every number is queried from the exchange "
         "on each refresh, not from the bot's own bookkeeping. When the two disagree, this page "
         "is the one that is right.</div>";
    h += "</body></html>";
    return h;
}

}  // namespace

int main(int argc, char** argv) {
    unsigned short port     = 8080;
    int            n_trades = 50;
    double         initial  = 5000.0;
    std::string    symbol   = "BTCUSDT";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = static_cast<unsigned short>(std::atoi(argv[++i]));
        else if (a == "--trades" && i + 1 < argc) n_trades = std::atoi(argv[++i]);
        else if (a == "--initial" && i + 1 < argc) initial = std::atof(argv[++i]);
        else if (a == "--symbol" && i + 1 < argc) symbol = argv[++i];
        else {
            std::fprintf(stderr,
                         "usage: hftdash [--port 8080] [--trades 50] [--initial 5000]\n"
                         "               [--symbol BTCUSDT]\n");
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
        net::io_context ioc;
        // 127.0.0.1 only. This process holds API credentials; it has no business
        // listening on a public interface.
        tcp::acceptor acceptor{ioc, tcp::endpoint{net::ip::make_address("127.0.0.1"), port}};
        std::printf("dashboard: http://127.0.0.1:%u   (Ctrl-C to stop)\n", port);
        std::fflush(stdout);

        for (;;) {
            tcp::socket socket{ioc};
            acceptor.accept(socket);
            try {
                beast::flat_buffer               buffer;
                http::request<http::string_body> req;
                http::read(socket, buffer, req);

                if (req.target() == "/favicon.ico") {
                    http::response<http::empty_body> res{http::status::no_content, req.version()};
                    res.prepare_payload();
                    http::write(socket, res);
                } else {
                    const std::string body = render(gather(exec, n_trades), initial, symbol);
                    http::response<http::string_body> res{http::status::ok, req.version()};
                    res.set(http::field::content_type, "text/html; charset=utf-8");
                    res.body() = body;
                    res.prepare_payload();
                    http::write(socket, res);
                }
                beast::error_code ec;
                socket.shutdown(tcp::socket::shutdown_send, ec);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[dash] request failed: %s\n", e.what());
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dashboard failed: %s\n", e.what());
        return 1;
    }
}
