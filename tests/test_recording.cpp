// Round-trip tests for the recording format. Writes a real gzip file to the
// system temp directory, reads it back, and checks the bytes survived.

#include "gzfile.hpp"
#include "json_decode.hpp"
#include "recording.hpp"

#include <simdjson.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace hft;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            std::printf("  FAIL  %s:%d   %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

namespace {

constexpr char kEventJson[] =
    R"({"e":"depthUpdate","E":1,"T":1,"s":"TESTUSDT","U":10,"u":12,"pu":9,)"
    R"("b":[["100.00","1.5"],["99.99","2.0"]],"a":[["100.01","3.0"]]})";

// Deliberately larger than GzLineReader's 64 KiB chunk, so the buffer-growth
// path is exercised. Real depth snapshots at limit=1000 land in this range, so
// this is not a synthetic edge case -- it is the normal case for snapshots.
std::string make_big_snapshot(int levels) {
    std::string s = R"({"lastUpdateId":7,"bids":[)";
    for (int i = 0; i < levels; ++i) {
        if (i) s += ',';
        s += "[\"" + std::to_string(1000 + i) + ".50\",\"1.250\"]";
    }
    s += R"(],"asks":[["9999.00","1.0"]]})";
    return s;
}

struct Parsed {
    std::int64_t t = 0;
    std::string  kind;
    std::string  payload;
};

std::vector<Parsed> read_all(const std::string& path) {
    std::vector<Parsed>   out;
    GzLineReader          reader(path);
    simdjson::dom::parser parser;

    std::string_view line;
    while (reader.next(line)) {
        simdjson::dom::element env;
        if (parser.parse(line.data(), line.size()).get(env)) continue;

        Parsed p;
        std::string_view k;
        if (env["t"].get(p.t) || env["k"].get(k)) continue;
        p.kind    = std::string(k);
        p.payload = simdjson::to_string(env["d"]);
        out.push_back(std::move(p));
    }
    return out;
}

}  // namespace

int main() {
    const auto path =
        (std::filesystem::temp_directory_path() / "hft_test_recording.jsonl.gz").string();
    std::filesystem::remove(path);

    const std::string big = make_big_snapshot(4000);
    CHECK(big.size() > (1u << 16));  // must exceed the reader's chunk size

    std::printf("test_recording_roundtrip\n");
    {
        Recorder rec(path, R"({"format":1,"symbol":"TESTUSDT"})");
        rec.write(RecordKind::Event, kEventJson, 1'700'000'000'000'000'001LL);
        rec.write(RecordKind::Snapshot, big, 1'700'000'000'000'000'002LL);
    }  // destructor closes the file

    const auto records = read_all(path);
    CHECK(records.size() == 3);  // header + event + snapshot
    if (records.size() != 3) {
        std::printf("\n%d checks, %d failures\n", g_checks, g_failures + 1);
        return 1;
    }

    CHECK(records[0].kind == "h");
    CHECK(records[1].kind == "e");
    CHECK(records[2].kind == "s");

    // Timestamps survive at full nanosecond precision. Round-tripping these
    // through a double would silently lose the low digits -- 1.7e18 needs 61
    // bits of mantissa and a double has 53.
    CHECK(records[1].t == 1'700'000'000'000'000'001LL);
    CHECK(records[2].t == 1'700'000'000'000'000'002LL);

    // The payload must still decode to exactly what the live path would produce.
    std::printf("test_recorded_payload_decodes\n");
    {
        simdjson::dom::parser  parser;
        simdjson::dom::element doc;
        CHECK(!parser.parse(records[1].payload.data(), records[1].payload.size()).get(doc));

        const auto ev = decode_depth_event(doc);
        CHECK(ev.has_value());
        if (ev) {
            CHECK(ev->U == 10);
            CHECK(ev->u == 12);
            CHECK(ev->pu == 9);
            CHECK(ev->bids.size() == 2);
            CHECK(ev->asks.size() == 1);
            CHECK(ev->bids[0].px == *parse_fixed("100.00"));
            CHECK(ev->bids[0].qty == *parse_fixed("1.5"));
        }
    }

    // A line far larger than the reader's chunk must come back whole.
    std::printf("test_large_line_roundtrip\n");
    {
        simdjson::dom::parser  parser;
        simdjson::dom::element doc;
        CHECK(!parser.parse(records[2].payload.data(), records[2].payload.size()).get(doc));

        const auto snap = decode_depth_snapshot(doc);
        CHECK(snap.has_value());
        if (snap) {
            CHECK(snap->last_update_id == 7);
            CHECK(snap->bids.size() == 4000);
            CHECK(snap->asks.size() == 1);
            CHECK(snap->bids[0].px == *parse_fixed("1000.50"));
        }
    }

    std::filesystem::remove(path);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
