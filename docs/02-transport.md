# Lesson 2 — Transport: WebSocket, TLS and JSON

Phase 1 is now live. This lesson covers how the bytes get from Binance into the
`DepthSync` we built in Lesson 1.

Files: [`include/binance_net.hpp`](../include/binance_net.hpp),
[`src/binance_net.cpp`](../src/binance_net.cpp),
[`include/json_decode.hpp`](../include/json_decode.hpp),
[`src/main.cpp`](../src/main.cpp)

---

## 1. The layering, and why it is worth the extra target

```
  hft_core   pure logic: book, sync, fixed point.   ZERO dependencies.
     ^
  hft_net    transport: Boost.Beast + OpenSSL.      Boost hidden behind pimpl.
     ^
  hftbot     wiring: decode -> sync -> display.
```

Two rules produce this shape:

**Rule 1 — the correctness-critical code depends on nothing.** `DepthSync`
knows nothing about sockets, JSON or threads, which is exactly why 60 test
assertions can verify it with no network. Untestable trading logic is how people
lose money.

**Rule 2 — Boost never escapes `binance_net.cpp`.** Beast's headers are
enormous; including them from a widely-included header would wreck compile times
across the project. The [pimpl idiom](../include/binance_net.hpp) puts every
Boost type inside one `.cpp`. Callers see `std::string`.

## 2. Synchronous blocking I/O is correct here

`WebSocketClient::read()` blocks. One thread, no async, no callbacks. For an
"HFT" project this looks wrong. It isn't, and the reason is worth understanding.

The subtle problem it seems to create: we must open the stream *before* the REST
snapshot (or events published during the snapshot are lost), but a single thread
can't read the socket and wait on an HTTP response at once.

**The kernel does the buffering for us.** Once the WebSocket is connected, the
OS receives and buffers incoming frames whether or not we call `read()`. So:

```cpp
ws.connect(...);   // stream open; kernel starts collecting frames
sync.reset();
resync();          // blocking REST call -- frames pile up in the socket buffer
while (true) {
    ws.read();     // we now drain them, oldest first, none lost
}
```

Events published during the REST call arrive with `u < lastUpdateId` and
`DepthSync` drops them as stale — until the one that straddles `lastUpdateId`
bridges us to live. The ordering guarantee comes free from TCP.

Where this would break: if the REST call took long enough to overflow the socket
receive buffer (a few hundred KB), we'd lose frames. At `@depth@100ms` on one
symbol that is not remotely close. Adding symbols or slowing the REST endpoint
changes that, and the fix is a dedicated reader thread feeding a queue — Week 4,
when we have measurements instead of guesses.

`DepthSync`'s internal buffer still earns its place: on a mid-stream resync it
holds the event that triggered the gap so it can bridge the next snapshot.

## 3. TLS details that bite

**SNI is mandatory.** Binance serves many hostnames from shared IPs. Without
`SSL_set_tlsext_host_name`, the server can't tell which certificate to present
and the handshake fails with an error that looks nothing like its cause.

**Certificate verification stays on.** `set_verify_mode(ssl::verify_peer)` plus
`set_default_verify_paths()`. Disabling it makes connection errors disappear,
which is why people disable it — and then they have no idea who they are sending
orders to.

**TLS shutdown errors on `https_get` are expected.** Most servers drop the
connection instead of completing a clean bidirectional shutdown. We collect the
error code and ignore it deliberately.

**Ping/pong is handled for us.** Binance pings every few minutes and disconnects
clients that don't pong. Beast answers protocol-level pings automatically.

## 4. JSON: strings, not numbers

Binance sends prices and sizes as JSON **strings**: `["65050.10","12.976"]`.
That is not sloppiness — it is so clients don't lose precision decoding them as
IEEE-754 doubles. We honour that by going straight from `string_view` to fixed
point via `parse_fixed`, never through a float. See Lesson 1 §1.

**Why the DOM parser.** simdjson has a faster On-Demand API, but DOM's calling
code reads like the JSON it parses. Correctness first; On-Demand is a Week 4
change, after measurement.

**Why `padded_string`.** simdjson reads past the end of the buffer by design —
its SIMD loads are 64 bytes wide. The input must carry padding, and it must stay
alive as long as the DOM that references it. Hence `buf_` is a member, not a
temporary. Getting this wrong is a use-after-free that usually "works" in
testing.

**All-or-nothing decoding.** `decode_event` returns `std::optional` and bails on
the first malformed field. A partially-decoded event corrupts the book exactly
like a dropped message — but is far harder to trace. The `undecoded` counter on
the display exists so silent decode failures can't hide.

## 5. Mainnet for data, testnet for orders

The default is **mainnet market data**, and that is deliberate:

- Market data needs **no API key**. We are only listening; no order can be
  placed without a signed, authenticated request. It is completely safe.
- The **testnet book is thin and synthetic**. Its spreads, depth and fill rates
  bear no resemblance to reality, so every strategy number we compute from it in
  Week 3 would be meaningless.

So: real data now, testnet orders in Week 4. `--testnet` switches the feed if
you want to see the difference — it is instructive to watch how much emptier
that book is.

## 6. Reconnection

The main loop wraps everything in `try`/`catch`, and on any network exception
reconnects after a second. Critically, it calls `sync.reset()` and re-snapshots:
a new connection tells us nothing about the old one's sequence numbers, so
carrying `last_u_` across a reconnect would silently accept a broken chain.

**The default on any transport failure is to distrust the book**, not to hope.

## 7. What the display shows

```
BTCUSDT   SYNCED   events 96   resyncs 0   undecoded 0

        ASK      65050.20   x      3.813
  ----  spread 0.10   mid 65050.1500   micro 65050.1773   skew +0.0273
        BID      65050.10   x     12.976
```

`skew` is `microprice - mid`, and it is Lesson 0's theory in live data: 12.98 on
the bid against 3.81 on the ask is a 3:1 imbalance, so fair value sits 2.7 cents
*above* the mid. A naive bot quoting symmetrically around the mid would be
systematically picked off on the upside here.

`resyncs` and `undecoded` are health metrics, on screen by default. If either
climbs, something is wrong with the feed — and you want to learn that from a
counter, not from the P&L.

---

*Next: Lesson 3 — Recording and deterministic replay*
