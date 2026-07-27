// Raw stream dumper. Connects to a WebSocket path and prints what arrives.
//
//   ./build/wsdump /ws/btcusdt@aggTrade
//   ./build/wsdump "/stream?streams=btcusdt@depth@100ms/btcusdt@aggTrade" 10
//
// Exists because "the subscription was accepted but no messages arrive" is a
// question you cannot answer from inside the bot. Being able to look at the
// wire directly turns a guessing game into a two-second check.

#include "binance_net.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: wsdump <path> [count] [host]\n");
        return 2;
    }
    const std::string path  = argv[1];
    const int         count = argc > 2 ? std::atoi(argv[2]) : 5;
    const std::string host  = argc > 3 ? argv[3] : "fstream.binance.com";

    try {
        hft::WebSocketClient ws;
        ws.connect(host, path);
        std::fprintf(stderr, "connected to wss://%s%s\n\n", host.c_str(), path.c_str());

        for (int i = 0; i < count; ++i) {
            const auto msg = ws.read();
            std::printf("[%d] %.*s\n", i, static_cast<int>(msg.size() > 300 ? 300 : msg.size()),
                        msg.data());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
