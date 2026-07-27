#pragma once

#include "gzfile.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace hft {

// A recording is gzipped JSONL. One line per record:
//
//   {"t":1753632000123456789,"k":"e","d":{ ...verbatim exchange payload... }}
//
//   t = OUR receive time, nanoseconds since the Unix epoch
//   k = record kind
//   d = the exchange's bytes, spliced in untouched
//
// The payload is stored VERBATIM and never re-serialised. That is the single
// most important property of this format: the recording is ground truth. If our
// decoder turns out to have a bug, we fix the decoder and re-run the same file.
// Had we recorded decoded structs instead, the bug would be baked into every
// recording we ever made, permanently.
enum class RecordKind : char {
    Header    = 'h',  // metadata: symbol, streams, hosts
    Snapshot  = 's',  // REST depth snapshot
    Event     = 'e',  // format 1: a depth event, and nothing else
    WsMessage = 'w',  // format 2: any WebSocket message; dispatch on payload "e"
};

// Why format 2 exists: once we subscribe to more than one stream, the record
// kind can no longer name the content -- the message itself does, via its "e"
// field. So 'w' means "whatever the exchange sent us over the socket".
//
// 'e' is kept because recordings made with format 1 must keep replaying. A file
// format that invalidates yesterday's data every time you learn something is
// not a file format worth having.
inline constexpr int kRecordingFormat = 2;

// Wall-clock time, for correlating with the exchange's own E/T timestamps.
//
// Note this is system_clock, NOT steady_clock. They are not interchangeable:
// system_clock is wall time (can jump when NTP corrects it) and is the only one
// comparable to a timestamp from another machine; steady_clock never jumps and
// is the only one valid for measuring durations. Using the wrong one is a
// classic source of impossible-looking latency numbers.
inline std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

class Recorder {
public:
    Recorder(const std::string& path, std::string_view meta_json) : gz_(path) {
        // Every recording is self-describing. Six months from now you will not
        // remember which symbol data/2026-07-27.jsonl.gz holds.
        write(RecordKind::Header, meta_json, now_ns());
    }

    void write(RecordKind kind, std::string_view payload, std::int64_t recv_ns) {
        // to_chars, not to_string: this runs on the live data path and
        // std::to_string allocates. line_ is a reused member for the same
        // reason -- after the first few messages, recording stops allocating.
        char       ts[24];
        const auto conv = std::to_chars(ts, ts + sizeof ts, recv_ns);

        line_.clear();
        line_ += "{\"t\":";
        line_.append(ts, static_cast<std::size_t>(conv.ptr - ts));
        line_ += ",\"k\":\"";
        line_ += static_cast<char>(kind);
        line_ += "\",\"d\":";
        line_ += payload;
        line_ += '}';

        gz_.write_line(line_);
    }

    void flush() { gz_.flush(); }

private:
    GzWriter    gz_;
    std::string line_;
};

}  // namespace hft
