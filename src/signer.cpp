#include "signer.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <cstdint>
#include <stdexcept>

namespace hft {

std::string hmac_sha256_hex(std::string_view key, std::string_view data) {
    unsigned char  out[EVP_MAX_MD_SIZE];
    unsigned int   len = 0;
    const unsigned char* r =
        HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(data.data()), data.size(), out, &len);
    if (!r) throw std::runtime_error("HMAC-SHA256 failed");

    static constexpr char kHex[] = "0123456789abcdef";
    std::string           hex;
    hex.resize(static_cast<std::size_t>(len) * 2);
    for (unsigned i = 0; i < len; ++i) {
        hex[2 * i]     = kHex[out[i] >> 4];
        hex[2 * i + 1] = kHex[out[i] & 0x0f];
    }
    return hex;
}

std::string sign_query(std::string_view query, std::string_view secret,
                       std::int64_t timestamp_ms, std::int64_t recv_window_ms) {
    std::string q(query);
    if (!q.empty()) q += '&';
    q += "recvWindow=" + std::to_string(recv_window_ms);
    q += "&timestamp=" + std::to_string(timestamp_ms);

    // Sign exactly what we are about to send, after every other parameter is in
    // place. Signing a different string than we transmit is the classic -1022.
    q += "&signature=" + hmac_sha256_hex(secret, q);
    return q;
}

}  // namespace hft
