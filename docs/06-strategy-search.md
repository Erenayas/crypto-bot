# Lesson 6 — Searching for a Strategy That Beats the Fee

Lessons 4 and 5 established that the market maker loses, and that nearly all of
the loss is the maker fee. This lesson is the search that followed, framed as:
*take the fee as fixed and find something that clears it.*

Every number here is measured, and most of the ideas failed.

Tool: [`tools/screen.py`](../tools/screen.py)

---

## 1. The problem restated in one line

BTCUSDT's spread is **0.02 bp**. The round-trip maker fee is **4 bp**.

The fee is **200× the spread**. No amount of strategy work fixes a 200× gap;
this is arithmetic, not tuning. Every measurement in this project has been a
different view of that sentence.

## 2. Can a cheaper fee tier close it?

| tier | maker fee | net (backtest) |
|---|---|---|
| base | 2.00 bp | −2.318 bp |
| + BNB discount (10% on futures) | 1.80 bp | −2.117 bp |
| VIP 1 | 1.60 bp | −1.916 bp |
| VIP 1 + BNB | 1.44 bp | −1.756 bp |
| VIP 9 (needs ~$2B monthly volume) | 0.00 bp | **−0.311 bp** |

The BNB discount is real and free — hold BNB, pay fees with it, 10% off futures.
It buys **0.20 bp**, about a tenth of the gap.

And the row that ends the discussion: **at a zero fee the backtest still loses
0.311 bp.** On mainnet data, the strategy's gross edge is negative. No fee tier
reachable by anyone, including VIP 9, makes this instrument work.

### A caveat that cuts the other way

Our *live* sessions on testnet showed gross P&L of **+1.02 bp** across 110 fills,
which would imply break-even at a 1.02 bp fee. That contradicts the backtest.

Believe the backtest, for the reason recorded back in
[Lesson 2 §5](02-transport.md): **testnet's book is thin and synthetic.** No
professional firm is competing with our quotes there, so queues are short and
fills are easy. The backtest runs on *mainnet* data, where the competition is
real. The favourable live numbers are testnet's gift, not ours.

This is exactly the trap the project warned about in week one and then walked
toward in week four.

## 3. Is there a better instrument?

Screening all 109 liquid USDT perps for spread against the fee:

| symbol | spread | net edge | trades/sec | verdict |
|---|---|---|---|---|
| OPUSDT | 10.97 bp | +6.97 bp | 1.1 | wide but nobody trades |
| ADAUSDT | 6.32 bp | +2.32 bp | 3.3 | wide but nobody trades |
| NEARUSDT | 5.63 bp | +1.63 bp | 3.0 | wide but nobody trades |
| BNBUSDT | 0.18 bp | −3.82 bp | 7.0 | tight |
| SOLUSDT | 1.33 bp | −2.67 bp | 13.5 | tight |
| BTCUSDT | 0.02 bp | −3.98 bp | 32.0 | tight |
| ETHUSDT | 0.05 bp | −3.95 bp | 60.2 | tight |

13 of 46 established symbols have a spread wider than the fee. **Every one of
them has almost no trade flow.** A first attempt on ADAUSDT produced **8 fills
in 12 minutes** — nothing to draw a conclusion from.

So the screen became `(spread − fee) × trade count`, which prices both halves.
The result is a clean market-structure fact:

> **Where there is volume, competition has compressed the spread below retail
> fees. Where the spread is wide, there is no volume.** The two never coincide
> among established names.

They do coincide on new listings — one showed a 6.41 bp spread at 228 trades/sec
— but a market maker holding inventory in a freshly listed token does not get to
argue with a 30% announcement gap. Out of scope by choice, not by ignorance.

### And the wide spread is not free money anyway

Testing on that high-frequency wide-spread listing, with a real sample of
**10,596 fills**, markout came in at **−1.5 bp**. The spread is wide *because*
adverse selection is severe there. It is a risk premium, and the measurement
says the premium does not cover the risk. Widening our own quotes improved
things monotonically (−3.56 bp at a 1.25 bp edge, −2.22 bp at 8 bp) but never
turned positive.

## 4. Different economics: the grid

A market maker earns the **spread**. A grid earns **volatility** — it hangs
orders at fixed prices and collects one grid step per completed round trip. A
20 bp step pays 20 bp and costs 4 bp, and it does not care that the spread is
0.02 bp.

That reframing matters for instrument choice: **screening a grid by spread picks
the wrong instrument.** The right metric is how far price moves per hour.

| symbol | spread | hourly range | suggested step | volume |
|---|---|---|---|---|
| NEARUSDT | 5.64 bp | 98 bp | 24 bp | 36M |
| ADAUSDT | 6.33 bp | 90 bp | 22 bp | 115M |
| LINKUSDT | 1.17 bp | 87 bp | 22 bp | 34M |
| SUIUSDT | 1.43 bp | 83 bp | 21 bp | 41M |
| **ETHUSDT** | 0.05 bp | 76 bp | 19 bp | **1838M** |
| **SOLUSDT** | 1.33 bp | 67 bp | 17 bp | **319M** |
| **BTCUSDT** | 0.02 bp | 47 bp | 12 bp | **2017M** |
| TRXUSDT | 0.30 bp | 17 bp | 4 bp | 30M |

Note how the ordering changes. ETHUSDT and SOLUSDT are hopeless for a market
maker and perfectly reasonable for a grid: deep liquidity *and* enough movement.
TRXUSDT is the opposite — it barely moves, so a grid there would capture 4 bp
per round trip against a 4 bp fee.

### What we measured, and why it does not settle anything

On 15 minutes of BTCUSDT:

| | fills | gross | net |
|---|---|---|---|
| market maker | 241 | −0.31 bp | −2.32 bp |
| grid 5 bp × 5 | 184 | −1.64 bp | −3.65 bp |
| grid 20 bp × 5 | 6 | −54.54 bp | −56.56 bp |

Worse — but **the test is unfair to the grid**, and saying so is not special
pleading. Those fifteen minutes are a 0.6% downtrend, which is a grid's worst
case *by construction*: it buys every level down, accumulates, and never gets
the reversal that closes a round trip. All six fills at the 20 bp step were
buys. BTC's entire 15-minute range is about 47 bp, so a 20 bp grid can cross
three levels at most.

Judging a grid on a trending 15-minute sample is the same error as judging the
market maker on ADAUSDT's 8 fills — an error this project has now made twice and
caught twice.

**Status: open.** A grid needs hours of data spanning both ranging and trending
regimes. Recording that is cheap; concluding without it is not.

## 5. What the search actually established

1. **BTCUSDT passive market making is dead at any reachable fee tier**, and it is
   dead on gross margin, not just after fees.
2. **No established symbol has both a wide spread and real flow.** That is market
   structure, not bad luck, and it is worth understanding rather than fighting.
3. **Wide spreads are risk premia, not arbitrage.** Where we found one with real
   flow, markout was −1.5 bp.
4. **The grid changes which question to ask** — volatility, not spread — and
   moves the majors from hopeless to plausible. Untested at the sample size that
   would settle it.
5. **Our live results are optimistic because they are on testnet.** The backtest,
   on mainnet data, is the one to believe. Catching this required remembering a
   warning written in week one.

## 6. Ideas measured and rejected along the way

Recorded because the reasoning is reusable, and because a list of things that
did not work is more useful than a list of things that did:

| idea | result | why |
|---|---|---|
| Never close at a loss | −18.9 bp, 9 fills | converts losses into inventory ([L4 §7](04-fills-and-strategy.md)) |
| Average down into a loser | −4.65 bp at 5×, risk grew 5× | buys most when the market disagrees most |
| Quote more slowly | worse at every interval | stale quotes are adversely selected quotes |
| Minimum edge floor | **−2.898 → −2.318 bp** | the one that worked; markout −0.838 → −0.070 |

---

*Run the screens yourself:*

```bash
./tools/screen.py spread    # market-making view
./tools/screen.py vol       # grid view
```
