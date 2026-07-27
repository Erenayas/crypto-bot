#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace hft {

// Prices and quantities are FIXED-POINT INTEGERS, scaled by kScale.
// They are never doubles. See docs/01-order-book.md for the reasoning.
using Price = std::int64_t;
using Qty   = std::int64_t;

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

}  // namespace hft
