# Lesson 4 — Fill Simulation, Strategy, and What the Numbers Say

The part where the project stops being infrastructure and starts answering the
actual question: *does this make money?*

Files: [`include/fill_sim.hpp`](../include/fill_sim.hpp),
[`include/portfolio.hpp`](../include/portfolio.hpp),
[`include/strategy.hpp`](../include/strategy.hpp),
[`src/backtest_main.cpp`](../src/backtest_main.cpp)

---

## 1. The fill model is the backtest

Everything else can be perfect and a bad fill model still makes the result
fiction. The naive version — *"price touched my level, so I filled"* — is the
single biggest reason market-making backtests look profitable and then lose
money live.

In reality you fill only after **everyone ahead of you at that price** fills
first. So we track, per resting order, how much volume sits in front of it:

```cpp
place()     queue_ahead = size currently at that price   // everyone there is ahead
on_trade()  a trade at our price on our side eats the queue FIRST;
            only the leftover fills us
on_book()   the level shrank with no trade => cancellations
```

The test that captures the whole idea:

```cpp
sim.place(Side::Buy, 100.00, 1, book);        // 10 units already resting
sim.on_trade(trade_at("100.00", "4", true));  // price "touched" our level...
CHECK(fills.empty());                          // ...and we did NOT fill
CHECK(sim.bid().queue_ahead == 6);
```

### The cancellation rule

When a level shrinks with no trade to explain it, someone cancelled — but the
depth stream never says *whose* order it was. This is exactly the ambiguity that
made us record the trade stream in Lesson 3.

We assume cancels came from **behind** us, so our queue position does not
improve. We only advance when arithmetic forces it (`queue_ahead` can never
exceed the level's total size).

That makes the model **pessimistic**: in reality some cancels really are ahead
of you, so true fill rates are a little better than what we report. That is the
right direction to be wrong in. An optimistic backtest is a bill you pay later,
with real money.

### Requoting costs queue position

```cpp
sim.place(Side::Buy, 100.00, 1, book);   // queue_ahead falls to 1 after trades
sim.place(Side::Buy, 100.00, 1, book);   // same price: keeps our place
CHECK(sim.bid().queue_ahead == 1);

sim.place(Side::Buy,  99.90, 1, book);   // one tick away: back of a 25-unit queue
CHECK(sim.bid().queue_ahead == 25);
```

Queue position is the asset a market maker spends. A strategy that requotes on
every tick throws it away continuously and never reaches the front of anything.
That is why the backtester has a hysteresis threshold, and why this cost falls
out of the model naturally instead of being bolted on.

## 2. Markout: measuring adverse selection directly

Spread capture is easy to measure and easy to fool yourself with. **Markout** is
the number that matters: for each fill, compare the mid price one second later
against what we traded at, signed by direction.

- Bought at 100, mid is 100.05 later → **+** good fill
- Bought at 100, mid is 99.95 later → **−** we were picked off

```cpp
mo.on_fill(Fill{Side::Buy, 100.00, 2, t});
mo.on_mark(t + horizon, 99.50);
CHECK(mo.total() == -1.0);   // (99.50 - 100.00) * 2
```

A market maker with positive spread capture and strongly negative markout is not
profitable — it is a slow-motion loss with good bookkeeping. This is Lesson 0's
adverse selection, finally measurable.

## 3. The strategy

Avellaneda–Stoikov reduced to the two ideas that carry the weight, with
dimensionless parameters:

**Reservation price.** Don't quote around fair value — quote around fair value
shifted *against* your inventory:

```
r = fair − q · γ · σ²
```

Long inventory pushes `r` down, moving both quotes down, making us more likely
to sell and less likely to buy. We give up a little expected spread to pull back
toward flat.

**Spread widens with volatility.** A-S's optimal spread grows with `γσ²`.
Volatility is when you get run over, so that is when you demand compensation.

Plus two things A-S doesn't give you but reality demands: hard inventory limits
(stop quoting the side that makes it worse), and never crossing the book (a
maker that crosses becomes a taker — worse fee, no spread, none of the queue
dynamics the strategy is built on).

Setting `γ = 0` and `use_microprice = false` yields the naive symmetric market
maker, deliberately, so the two can be compared on identical bytes.

## 4. Results

60 seconds of live BTCUSDT, 574 depth events, 1646 trades. Quote size 0.002 BTC,
max position 0.010 BTC.

### Half-spread sweep, at Binance's 2 bp base maker fee

| half-spread | fills | gross P&L | fees | **net** | markout |
|---|---|---|---|---|---|
| 0.5 tick | 19 | −0.100 | 0.494 | **−0.593** | −0.83 bp |
| 1 tick | 19 | −0.099 | 0.494 | **−0.593** | −0.83 bp |
| 2 tick | 20 | −0.097 | 0.494 | **−0.591** | −0.82 bp |
| 4 tick | 15 | −0.097 | 0.390 | **−0.486** | −0.74 bp |

### Same strategy, varying only the fee

| maker fee | fees paid | **net P&L** | net in bp |
|---|---|---|---|
| 2.0 bp (base tier) | 0.494 | **−0.593** | −2.40 |
| 1.0 bp | 0.247 | **−0.346** | −1.40 |
| 0.0 bp | 0.000 | **−0.100** | −0.40 |
| −0.5 bp (rebate) | −0.123 | **+0.024** | **+0.10** |

## 5. What this actually means

**It loses money.** That is the correct result, it is what Lesson 0 predicted
from arithmetic alone, and it is worth more than a backtest that "worked".

Three findings, in order of importance:

**1. This is a fee-tier problem before it is a strategy problem.** The entire
P&L range from −2.40 bp to +0.10 bp is driven by the fee, not by anything the
strategy does. BTCUSDT's book sits at a one-tick spread — about 0.15 bp round
trip — while the base maker fee is 2 bp per side. **You cannot capture 0.15 bp
and pay 4 bp.** Real market makers on this venue operate at VIP tiers where the
maker fee is near zero or negative. Without that, no amount of cleverness fixes
the arithmetic.

**2. Gross P&L is negative before fees at all.** Even at zero fee we lose 0.40 bp,
and markout says why: −0.83 bp of adverse selection. The fills we get are
systematically the ones we don't want. Widening the spread reduces it (−0.83 →
−0.74 bp at 4 ticks) but also cuts fills, so it does not rescue the total.

**3. The microprice signal is smaller than one tick, so quoting rounds it away.**
Naive-mid and microprice configurations produce nearly identical results. The
microprice edge is ~0.03 on a 0.10 tick — a real signal that cannot be expressed
in a *price* on this instrument. To use it you would have to express it in
**size**, in **whether to quote at all**, or trade a wider-spread instrument.
That is a genuine microstructure constraint, not a bug.

### What would come next, given more time

- **Quote sizing / quote suppression** driven by the microprice and trade-flow
  imbalance, since price cannot carry a sub-tick signal
- Test on a wider-spread symbol where a tick is worth more in bp
- A model of our own latency (we currently assume our quote is in the book
  instantly, which is optimistic in the one place the model is not conservative)
- Fee-tier sensitivity as an explicit input to strategy selection

## 6. Honest limitations

Stated plainly, because a backtest whose assumptions are hidden is worse than no
backtest:

- **No latency model.** Quotes are assumed to reach the book instantly. Real
  round trip is 1–50 ms, during which the book moves. This is optimistic.
- **We do not affect the market.** Our orders are simulated alongside the real
  book, not inside it. At 0.002 BTC on BTCUSDT that is a fair approximation;
  at size it would not be.
- **Cancel/replace is instant and free** beyond the queue-position cost.
- **Sample is short.** 60 seconds is enough to validate the machinery, not to
  draw a conclusion about a strategy. Longer recordings are the fix, and cost
  nothing but wall-clock time.

---

*Run it:*

```bash
./build/hftbacktest data/btcusdt.jsonl.gz --gamma 5 --half 0.5
./build/hftbacktest data/btcusdt.jsonl.gz --gamma 0 --microprice 0   # naive baseline
./build/hftbacktest data/btcusdt.jsonl.gz --fee 0 --csv pnl.csv      # zero-fee scenario
```
