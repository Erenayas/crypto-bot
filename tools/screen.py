#!/usr/bin/env python3
"""Screen Binance USD-M perps for whichever strategy you are considering.

    ./tools/screen.py spread      market-making view: is the spread wider than the fee?
    ./tools/screen.py vol         grid view: does price move enough per hour?
    ./tools/screen.py both

The two views disagree, and that disagreement is the point. A market maker earns
the SPREAD, so it needs an instrument where the spread exceeds the round-trip
fee. A grid earns VOLATILITY, so it does not care about the spread at all -- it
needs price to oscillate across its levels often enough.

Screening a grid by spread, or a market maker by volatility, picks the wrong
instrument with great confidence.
"""

import json
import math
import sys
import urllib.request

FAPI = "https://fapi.binance.com"

# Established names only. New listings can show enormous headline volume and
# a wide spread, and then gap 30% on one announcement -- a market maker holding
# inventory there does not get to argue with it.
MAJORS = """BTC ETH SOL XRP BNB DOGE ADA AVAX LINK DOT TRX LTC BCH XLM HBAR
SUI TON UNI NEAR APT ICP ETC FIL ARB OP ATOM INJ VET ALGO AAVE POL STX IMX
SEI TIA GRT LDO MKR CRV SAND MANA AXS EOS XTZ THETA RUNE ENA ONDO JUP""".split()

ROUND_TRIP_FEE_BP = 4.0  # 2 bp maker per side, Binance USD-M base tier


def get(path):
    with urllib.request.urlopen(FAPI + path, timeout=30) as r:
        return json.load(r)


def universe():
    books = {b["symbol"]: b for b in get("/fapi/v1/ticker/bookTicker")}
    stats = {t["symbol"]: t for t in get("/fapi/v1/ticker/24hr")}
    out = []
    for m in MAJORS:
        s = m + "USDT"
        b, t = books.get(s), stats.get(s)
        if not b or not t:
            continue
        bid, ask = float(b["bidPrice"]), float(b["askPrice"])
        if bid <= 0 or ask <= bid:
            continue
        mid = (bid + ask) / 2
        out.append({
            "symbol": s,
            "spread_bp": (ask - bid) / mid * 10000,
            "trades": int(t["count"]),
            "volume": float(t["quoteVolume"]),
        })
    return out


def spread_view(rows):
    print("\nMARKET MAKING:  is the spread wider than the round-trip fee?")
    print("=" * 78)
    print(f"{'symbol':<11}{'spread':>9}{'net edge':>11}{'trades/s':>11}{'volume':>13}{'score':>10}")
    print("-" * 78)
    for r in rows:
        r["edge_bp"] = r["spread_bp"] - ROUND_TRIP_FEE_BP
        r["score"] = r["edge_bp"] * r["trades"] / 1e6
    for r in sorted(rows, key=lambda x: -x["score"]):
        flag = "  viable" if r["edge_bp"] > 0 else ""
        print(f"{r['symbol']:<11}{r['spread_bp']:>8.2f}b{r['edge_bp']:>+10.2f}b"
              f"{r['trades']/86400:>11.1f}{r['volume']/1e6:>12.0f}M{r['score']:>10.1f}{flag}")
    print("-" * 78)
    print("score = net edge x daily trades. Both halves matter: a wide spread")
    print("nobody trades is worth as little as a tight one everybody does.")


def vol_view(rows):
    print("\nGRID:  does price move enough per hour to cross levels?")
    print("=" * 78)
    print(f"{'symbol':<11}{'hourly range':>15}{'hourly sd':>12}{'suggested step':>16}{'volume':>13}")
    print("-" * 78)
    for r in rows:
        k = get(f"/fapi/v1/klines?symbol={r['symbol']}&interval=1h&limit=24")
        r["range_bp"] = sum((float(c[2]) - float(c[3])) / float(c[4]) * 10000 for c in k) / len(k)
        rets = [math.log(float(k[i][4]) / float(k[i - 1][4])) for i in range(1, len(k))]
        mu = sum(rets) / len(rets)
        r["sd_bp"] = math.sqrt(sum((x - mu) ** 2 for x in rets) / len(rets)) * 10000
        r["step_bp"] = r["range_bp"] / 4  # aim for ~4 level crossings an hour

    for r in sorted(rows, key=lambda x: -x["range_bp"]):
        # A step must be several times the fee or the fee eats the capture.
        verdict = ("  ok" if r["step_bp"] > 3 * ROUND_TRIP_FEE_BP else
                   "  marginal" if r["step_bp"] > 1.5 * ROUND_TRIP_FEE_BP else "  too quiet")
        print(f"{r['symbol']:<11}{r['range_bp']:>14.0f}b{r['sd_bp']:>11.0f}b"
              f"{r['step_bp']:>15.0f}b{r['volume']/1e6:>12.0f}M{verdict}")
    print("-" * 78)
    print(f"step = hourly range / 4. Each completed round trip earns one step and")
    print(f"pays {ROUND_TRIP_FEE_BP:.0f} bp, so a step under ~12 bp hands most of the capture back.")


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "both"
    rows = universe()
    if what in ("spread", "both"):
        spread_view(rows)
    if what in ("vol", "both"):
        vol_view(rows)
