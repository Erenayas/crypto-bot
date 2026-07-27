#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hft {

// Prices and quantities are FIXED-POINT INTEGERS, scaled by kScale.
// They are never doubles. See docs/01-order-book.md for the reasoning.
using Price = std::int64_t;
using Qty   = std::int64_t;

enum class Side { Buy, Sell };

inline constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

inline constexpr std::int64_t kScale       = 100'000'000;  // 1e8
inline constexpr int          kScaleDigits = 8;

// Guard against absurd input from the wire overflowing int64.
inline constexpr int kMaxWholeDigits = 12;

// Parses a decimal string such as "60199.90" into fixed point.
//
// Returns nullopt on malformed input. We never silently coerce garbage from the
// network into a number -- a misparsed price is worse than no price, because it
// looks valid all the way down to the order we send.
constexpr std::optional<std::int64_t> parse_fixed(std::string_view s) noexcept {
    if (s.empty()) return std::nullopt;

    std::size_t i   = 0;
    bool        neg = false;
    if (s[0] == '-') { neg = true; i = 1; }
    else if (s[0] == '+') { i = 1; }

    std::int64_t whole        = 0;
    int          whole_digits = 0;
    for (; i < s.size() && s[i] != '.'; ++i) {
        if (s[i] < '0' || s[i] > '9') return std::nullopt;
        if (++whole_digits > kMaxWholeDigits) return std::nullopt;
        whole = whole * 10 + (s[i] - '0');
    }
    if (whole_digits == 0) return std::nullopt;

    std::int64_t frac        = 0;
    int          frac_digits = 0;
    if (i < s.size() && s[i] == '.') {
        ++i;
        if (i == s.size()) return std::nullopt;  // trailing '.' is malformed
        for (; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') return std::nullopt;
            // Truncate precision beyond our scale rather than overflowing.
            if (frac_digits < kScaleDigits) {
                frac = frac * 10 + (s[i] - '0');
                ++frac_digits;
            }
        }
    }
    for (; frac_digits < kScaleDigits; ++frac_digits) frac *= 10;

    const std::int64_t v = whole * kScale + frac;
    return neg ? -v : v;
}

// Only for display and for DERIVED STATISTICS (mid, microprice, P&L estimates).
// Never round-trip an exact value through double.
inline constexpr double to_double(std::int64_t fixed) noexcept {
    return static_cast<double>(fixed) / static_cast<double>(kScale);
}

// Fixed point back to a decimal string, with exactly `decimals` places.
//
// Used for the price and quantity we send to the exchange, which is why it goes
// through integer arithmetic rather than snprintf("%.2f", to_double(v)): a
// value that survived the whole pipeline exactly should not acquire a rounding
// error in the last step before the wire. Binance rejects orders whose
// precision does not match the symbol's tickSize/stepSize.
inline std::string format_fixed(std::int64_t v, int decimals) {
    const bool          neg = v < 0;
    const std::uint64_t a   = neg ? static_cast<std::uint64_t>(-v) : static_cast<std::uint64_t>(v);

    std::string s;
    if (neg) s += '-';
    s += std::to_string(a / static_cast<std::uint64_t>(kScale));

    if (decimals > 0) {
        char          buf[kScaleDigits];
        std::uint64_t frac = a % static_cast<std::uint64_t>(kScale);
        for (int i = kScaleDigits - 1; i >= 0; --i) {
            buf[i] = static_cast<char>('0' + frac % 10);
            frac /= 10;
        }
        s += '.';
        s.append(buf, static_cast<std::size_t>(decimals < kScaleDigits ? decimals : kScaleDigits));
    }
    return s;
}

}  // namespace hft
