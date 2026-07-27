# Lesson 0 — Market Microstructure

> Read this before writing a single line of trading code. Every design decision
> later in the project traces back to something on this page.

---

## 1. The Limit Order Book (LOB)

An exchange is not a place where "the price" lives. It is a **queue of unfilled
orders**, sorted by price. That structure is the *limit order book*.

```
        BIDS (buyers)              ASKS (sellers)
   size      price            price      size
   ------------------        ------------------
    12.4    60,199.9   <-BB   60,200.1    3.1   <- BA
     8.0    60,199.8          60,200.2   15.7
    31.2    60,199.7          60,200.3    9.4
```

Vocabulary you must be fluent in:

| Term | Meaning |
|---|---|
| **Best bid (BB)** | Highest price someone will *buy* at |
| **Best ask (BA)** | Lowest price someone will *sell* at |
| **BBO** | The pair (best bid, best ask) — "top of book" |
| **Spread** | `BA - BB`. Above: `0.2`. Never negative on a sane venue |
| **Mid price** | `(BB + BA) / 2` = `60,200.0` |
| **Depth** | Resting size at each level |
| **Level** | One price rung of the book |

The book is not a snapshot you fetch — it is a **living object you reconstruct
and maintain** from a stream of updates. Getting that reconstruction exactly
right is Phase 1 of this project, and it is where most amateur bots are
silently, invisibly broken.

---

## 2. Two kinds of orders, two kinds of trader

**Limit order** — "buy 1 BTC at 60,199.9 or better." It *rests* in the book
until someone trades against it. You are **passive**. You add liquidity. You are
a **maker**.

**Market order** — "buy 1 BTC right now at whatever the best price is." It
crosses the spread and consumes resting orders. You are **aggressive**. You
remove liquidity. You are a **taker**.

Every trade has exactly one maker and one taker.

### Why exchanges pay you to be a maker

Liquidity is the product an exchange sells. So the fee schedule is asymmetric.
Binance Futures, base tier (approximate — verify current numbers):

- **Maker fee: 0.020%** (2 basis points)
- **Taker fee: 0.050%** (5 basis points)

A *basis point* (bp) = 0.01%. Learn to think in bps; the numbers in this
business are small and percentages get confusing fast.

At higher volume tiers or with BNB discounts, the maker fee can approach zero
or go **negative** (a rebate — they pay *you*). Some venues have negative maker
fees at every tier. This is the structural foundation of market making.

---

## 3. What a market maker actually does

You quote **both sides simultaneously**:

```
   post BUY  1.0 @ 60,199.9      (your bid)
   post SELL 1.0 @ 60,200.1      (your ask)
```

If both fill, you bought at 60,199.9 and sold at 60,200.1. You captured the
spread: **0.2 dollars, about 0.33 bps**, minus 2×0.020% in maker fees.

Wait — do that arithmetic. Spread captured is ~0.33 bps. Fees are ~4 bps
round trip. **You just lost money.**

This is the single most important thing on this page. At a 0.2 spread on a
60,000 asset, a naive spread-capture market maker is *guaranteed* to lose. The
edge has to come from somewhere:

- A **wider spread** than the market's (but then you rarely get filled)
- **Fee rebates** or a volume tier where maker fees are ~0
- **Predicting short-term direction** so your fills are better than random
- **Inventory management** so you exit positions at good prices

You will build a naive version first *specifically so you can watch it lose
money* and understand which of these levers matters.

---

## 4. Your two enemies

### Enemy #1: Adverse selection (toxic flow)

This is the deep one. Think carefully.

Your orders rest passively. You do not choose when you trade — **someone else
chooses to trade against you.** So ask: who trades against a resting order, and
when?

Suppose you're quoting bid 60,199.9 / ask 60,200.1, and a large buyer arrives
who knows (or has correctly guessed) that BTC is about to jump to 60,250. They
lift your ask. You are now **short 1 BTC at 60,200.1** and the price
immediately runs to 60,250.

You "captured the spread" and lost $50.

The fills you receive are **systematically the ones you least want**. When price
is about to rise, you get hit on your ask. When it's about to fall, you get hit
on your bid. Your counterparties are, on average, better informed about the
next 500ms than you are. This is *adverse selection*, and the flow that causes
it is called *toxic*.

**This — not latency — is what kills crypto market makers.** Almost everything
sophisticated we build is a defense against it:

- Quoting around a smarter fair-value estimate (§6) instead of the mid
- Widening your spread when volatility or order flow imbalance rises
- Pulling quotes entirely when you detect toxicity
- Cancelling fast when the book moves against you

### Enemy #2: Inventory risk

You want to be **flat** — long and short in balance, earning spread, exposed to
no direction. But fills don't arrive in balanced pairs. After a run of one-sided
flow, you might be long 5 BTC.

You are now a directional trader who never chose to be one. If BTC drops 1%,
you lose $3,000 — which is thousands of spread captures' worth of profit,
erased by one move.

So a real market maker **skews its quotes based on inventory**. Long 5 BTC?
Lower *both* your bid and your ask, making you more likely to sell and less
likely to buy, pulling you back toward flat. You accept slightly worse expected
spread in exchange for less directional risk.

That trade-off — spread capture vs. inventory risk — has a formal optimal
solution: the **Avellaneda–Stoikov model**, which we build in Week 3.

---

## 5. Queue position, and why latency *actually* matters

Within a single price level, orders fill **FIFO** — first in, first out.

```
   60,199.9  [ A: 5.0 ][ B: 2.0 ][ YOU: 1.0 ][ D: 8.0 ]
                                   ^ 7.0 BTC must trade before you fill
```

Two consequences, and the second is subtle:

1. **Front of queue fills more often.** Obvious.
2. **Front of queue fills *better*.** Also less obvious and more important. The
   early part of a sweep is uninformed flow. The trades that reach deep into
   the queue are the ones where the price is genuinely moving — i.e. the toxic
   ones. **Being late in the queue means being adversely selected more often.**

So when a new price level forms, everyone races to be first. *That* is where
latency earns its keep — not in thinking faster, but in claiming queue position
and in cancelling before you get run over.

And now the reality check for crypto: your round trip to Binance is roughly
**1–50 ms** over the public internet, maybe **0.5–5 ms** colocated in the right
AWS region. Compare to equities HFT at sub-microsecond. You are not going to
win a pure speed race, and you should not design as though you could.

**Optimize your model first. Optimize your microseconds last.** We do latency
work in Week 4 — after measuring, never before.

---

## 6. Fair value: mid price is naive, use the microprice

If you quote symmetrically around the mid, you are assuming the next tick is
equally likely up or down. Look at this book:

```
    BIDS                    ASKS
    100.0  @ 99.99          2.0  @ 100.01
```

100 units want to buy, 2 units want to sell. The ask will almost certainly get
consumed first. "Fair value" is clearly **not** 100.00 — it's much closer to
100.01.

The **microprice** captures this by weighting each price by the size on the
*opposite* side:

```
              bid_px * ask_size  +  ask_px * bid_size
microprice =  ------------------------------------------
                      bid_size + ask_size
```

The crossed weighting is the whole trick, and it is worth pausing on. A large
`bid_size` puts weight on `ask_px`, pulling fair value **up** — because heavy
buying interest predicts an upward move. Plug the numbers in yourself:

```
(99.99 × 2.0 + 100.01 × 100.0) / 102.0 = 100.0096
```

Versus a mid of 100.00. Quoting around 100.0096 instead of 100.00 is a small
shift that meaningfully reduces how often you get picked off.

This is your first *alpha signal* — a genuine, if weak, predictor of short-term
price direction. Order flow imbalance is the most reliable signal in
high-frequency trading, and the microprice is its simplest expression.

---

## 7. Perpetual futures specifics (we're on Binance Futures)

A **perpetual future** ("perp") is a derivative that tracks spot but never
expires. Four things you must know:

- **Funding rate** — every 8 hours, longs pay shorts or vice versa, to tether
  the perp price to spot. If you're carrying inventory across a funding
  timestamp, this is real P&L. It can also be an edge if you lean the right way.
- **Mark price** — a smoothed index price used for liquidations, *not* the last
  traded price. Liquidations trigger off mark, not off the book.
- **Leverage and liquidation** — position sizing errors don't just lose money
  here, they can wipe the account. Hard position limits are a Week 4 deliverable
  and are not optional.
- **Contract size / tick size / lot size** — every symbol has a minimum price
  increment and quantity increment. Orders violating them are rejected. You
  will hit this on day one of live testing.

---

## 8. Check your understanding

Answer these in your own words before we move to Phase 1. If any is fuzzy, say
so and we'll go back over it.

1. You quote both sides and both fill. Why might you still have lost money?
2. Why are the fills you receive worse, on average, than a random trade at the
   same price?
3. You are long 8 BTC and want to get flat. Which way do you move your quotes,
   and why do you move *both* rather than just one?
4. Bid 50.0 @ 99.99, ask 10.0 @ 100.01. Compute the mid and the microprice.
   Which is the better estimate of fair value, and why?
5. Why does being 400th in a queue expose you to *more* adverse selection than
   being 2nd — beyond simply filling less often?

---

*Next: [Lesson 1 — Reconstructing the Order Book](01-order-book.md)*
