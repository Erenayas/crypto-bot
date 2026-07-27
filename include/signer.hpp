#pragma once

#include <string>
#include <string_view>

namespace hft {

// HMAC-SHA256, lowercase hex. Binance signs every authenticated request with
// this over the raw query string.
std::string hmac_sha256_hex(std::string_view key, std::string_view data);

// Appends `timestamp`, signs the whole query string, and appends `signature`.
//
// The order matters and is easy to get wrong: the signature must cover exactly
// the bytes that get sent, in the same order. Building the query one way and
// sending it another produces a -1022 "Signature for this request is not valid"
// with no hint as to which of the two is wrong.
std::string sign_query(std::string_view query, std::string_view secret,
                       std::int64_t timestamp_ms, std::int64_t recv_window_ms = 5000);

}  // namespace hft
