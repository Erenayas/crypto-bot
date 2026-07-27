#pragma once

#include "binance_net.hpp"
#include "exec_client.hpp"
#include "price.hpp"

#include <simdjson.h>

#include <optional>
#include <string>

namespace hft {

// Fetches tickSize, stepSize and precision for one symbol from
// /fapi/v1/exchangeInfo.
//
// These were hardcoded to BTCUSDT's values for the whole project, which was
// fine while BTCUSDT was the only symbol and wrong the moment it was not. The
// numbers vary enormously -- BTCUSDT's tick is 0.10 on a 64,000 price (0.0002%)
// while ADAUSDT's is 0.0001 on 0.158 (0.06%), a 400x difference in relative
// terms. Getting them from the venue is the only correct answer.
//
// Public endpoint: no API key required.
inline std::optional<SymbolSpec> fetch_symbol_spec(const std::string& rest_host,
                                                   const std::string& symbol) {
    const std::string json = https_get(rest_host, "/fapi/v1/exchangeInfo");

    simdjson::dom::parser  parser;
    simdjson::dom::element doc;
    if (parser.parse(json.data(), json.size()).get(doc)) return std::nullopt;

    simdjson::dom::array symbols;
    if (doc["symbols"].get(symbols)) return std::nullopt;

    for (auto e : symbols) {
        std::string_view name;
        if (e["symbol"].get(name) || name != symbol) continue;

        SymbolSpec spec;
        spec.symbol = symbol;

        std::int64_t pp = 0, qp = 0;
        if (e["pricePrecision"].get(pp) || e["quantityPrecision"].get(qp)) return std::nullopt;
        spec.price_dp = static_cast<int>(pp);
        spec.qty_dp   = static_cast<int>(qp);

        simdjson::dom::array filters;
        if (e["filters"].get(filters)) return std::nullopt;
        for (auto f : filters) {
            std::string_view type, tick;
            if (f["filterType"].get(type)) continue;
            if (type == "PRICE_FILTER" && !f["tickSize"].get(tick)) {
                if (const auto t = parse_fixed(tick)) spec.tick = *t;
            }
        }
        if (spec.tick <= 0) return std::nullopt;
        return spec;
    }
    return std::nullopt;
}

}  // namespace hft
