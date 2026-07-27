// Tests for the pieces that stand between a strategy and real money:
// request signing, decimal formatting, and the risk gate.
//
// None of these touch the network. All three are places where a bug is not a
// bad backtest -- it is a rejected order, a wrong price on the wire, or a
// liquidated account.

#include "price.hpp"
#include "risk.hpp"
#include "signer.hpp"

#include <cstdio>
#include <string>

using namespace hft;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(cond)) {                                                      \
            std::printf("  FAIL  %s:%d   %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

static void test_hmac_known_vector() {
    std::printf("test_hmac_known_vector\n");

    // The standard RFC test vector. Checking against a published value rather
    // than against ourselves is the point: a signature routine that is
    // self-consistently wrong still fails every request, with an error message
    // that blames the timestamp.
    CHECK(hmac_sha256_hex("key", "The quick brown fox jumps over the lazy dog") ==
          "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");

    CHECK(hmac_sha256_hex("k", "").size() == 64);  // always 32 bytes of hex
}

static void test_sign_query_shape() {
    std::printf("test_sign_query_shape\n");

    const std::string q = sign_query("symbol=BTCUSDT&side=BUY", "secret", 1700000000000LL);

    // The signature must be appended LAST and must cover everything before it.
    const auto sig_pos = q.find("&signature=");
    CHECK(sig_pos != std::string::npos);
    CHECK(q.find("timestamp=1700000000000") != std::string::npos);
    CHECK(q.find("recvWindow=") != std::string::npos);
    CHECK(q.substr(sig_pos + 11).size() == 64);

    // Signing exactly the transmitted prefix is the whole contract. Getting
    // this wrong yields Binance's -1022 with no clue which side is at fault.
    const std::string payload = q.substr(0, sig_pos);
    CHECK(q.substr(sig_pos + 11) == hmac_sha256_hex("secret", payload));

    // Timestamp is part of the signed payload, so it must change the signature.
    const std::string q2 = sign_query("symbol=BTCUSDT&side=BUY", "secret", 1700000000001LL);
    CHECK(q2 != q);
}

static void test_format_fixed() {
    std::printf("test_format_fixed\n");

    // What actually goes on the wire. Binance rejects orders whose precision
    // does not match the symbol's tickSize/stepSize.
    CHECK(format_fixed(*parse_fixed("65050.10"), 2) == "65050.10");
    CHECK(format_fixed(*parse_fixed("0.002"), 3) == "0.002");
    CHECK(format_fixed(*parse_fixed("1"), 0) == "1");
    CHECK(format_fixed(*parse_fixed("-0.5"), 1) == "-0.5");
    CHECK(format_fixed(*parse_fixed("100"), 2) == "100.00");

    // Round-trips exactly, because it never goes through a double.
    const auto px = *parse_fixed("64917.70");
    CHECK(*parse_fixed(format_fixed(px, 2)) == px);
}

static void test_risk_position_limit() {
    std::printf("test_risk_position_limit\n");

    RiskConfig c;
    c.max_position   = *parse_fixed("0.010");
    c.max_order_size = *parse_fixed("0.002");
    RiskGate g(c);

    // Room to buy.
    CHECK(g.check(Side::Buy, *parse_fixed("0.002"), *parse_fixed("0.006"), 1000, 1000) ==
          RiskVerdict::Allow);

    // The limit is checked against the position we would hold if this order
    // filled COMPLETELY. 0.009 + 0.002 = 0.011 > 0.010, so it is refused --
    // sizing against the CURRENT position is how you wake up past your limit.
    CHECK(g.check(Side::Buy, *parse_fixed("0.002"), *parse_fixed("0.009"), 1000, 1000) ==
          RiskVerdict::RejectPosition);

    // Selling from a long position is always fine: it reduces risk.
    CHECK(g.check(Side::Sell, *parse_fixed("0.002"), *parse_fixed("0.009"), 1000, 1000) ==
          RiskVerdict::Allow);

    // Symmetric on the short side.
    CHECK(g.check(Side::Sell, *parse_fixed("0.002"), -*parse_fixed("0.009"), 1000, 1000) ==
          RiskVerdict::RejectPosition);

    CHECK(g.check(Side::Buy, *parse_fixed("0.500"), 0, 1000, 1000) == RiskVerdict::RejectSize);
    CHECK(g.check(Side::Buy, 0, 0, 1000, 1000) == RiskVerdict::RejectSize);
}

static void test_risk_stale_book() {
    std::printf("test_risk_stale_book\n");

    RiskConfig c;
    c.max_position    = *parse_fixed("1");
    c.max_order_size  = *parse_fixed("1");
    c.max_book_age_ms = 2000;
    RiskGate g(c);

    CHECK(g.check(Side::Buy, *parse_fixed("1"), 0, 10000, 9000) == RiskVerdict::Allow);

    // Stale data is worse than no data, because it still looks actionable.
    CHECK(g.check(Side::Buy, *parse_fixed("1"), 0, 10000, 5000) == RiskVerdict::RejectStaleBook);
}

static void test_risk_rate_limit() {
    std::printf("test_risk_rate_limit\n");

    RiskConfig c;
    c.max_position     = *parse_fixed("1");
    c.max_order_size   = *parse_fixed("1");
    c.max_orders_per_s = 3;
    RiskGate g(c);

    for (int i = 0; i < 3; ++i) {
        CHECK(g.check(Side::Buy, *parse_fixed("1"), 0, 1000, 1000) == RiskVerdict::Allow);
        g.record_sent(1000);
    }
    // Exceeding the venue's rate limit gets the whole key throttled or banned,
    // which is a far worse outcome than skipping one requote.
    CHECK(g.check(Side::Buy, *parse_fixed("1"), 0, 1000, 1000) == RiskVerdict::RejectRate);

    // The window rolls.
    CHECK(g.check(Side::Buy, *parse_fixed("1"), 0, 2100, 2100) == RiskVerdict::Allow);
}

static void test_risk_kill_switch() {
    std::printf("test_risk_kill_switch\n");

    RiskConfig c;
    c.max_position   = *parse_fixed("1");
    c.max_order_size = *parse_fixed("1");
    c.max_drawdown   = 10.0;
    RiskGate g(c);

    g.on_equity(100.0);
    g.on_equity(95.0);
    CHECK(!g.halted());
    CHECK(g.check(Side::Buy, *parse_fixed("1"), 0, 1000, 1000) == RiskVerdict::Allow);

    g.on_equity(88.0);  // 12 below the peak of 100
    CHECK(g.halted());
    CHECK(g.check(Side::Buy, *parse_fixed("1"), 0, 1000, 1000) == RiskVerdict::RejectHalted);

    // The halt is PERMANENT. A strategy that has spent its loss budget does not
    // get to decide it feels better now; a human restarts it.
    g.on_equity(200.0);
    CHECK(g.halted());
    CHECK(g.check(Side::Buy, *parse_fixed("1"), 0, 1000, 1000) == RiskVerdict::RejectHalted);
}

int main() {
    test_hmac_known_vector();
    test_sign_query_shape();
    test_format_fixed();
    test_risk_position_limit();
    test_risk_stale_book();
    test_risk_rate_limit();
    test_risk_kill_switch();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
