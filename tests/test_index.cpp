// [index] Round-trip, counts, DataError, and incremental (tail) build equivalence.
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "Index.hpp"

using namespace kelvin;
namespace fs = std::filesystem;

namespace {
std::string mosfet_line(const std::string& mpn, double vds, double id, double rds, double qg,
                        const std::string& tech = "Si", const std::string& status = "production") {
    return "{\"semiconductor\":{\"mosfet\":{\"manufacturerInfo\":{\"name\":\"ACME\",\"reference\":\"" +
           mpn + "\",\"status\":\"" + status +
           "\",\"datasheetUrl\":\"https://acme.com/x.pdf\",\"datasheetInfo\":{\"part\":{"
           "\"technology\":\"" +
           tech + "\"},\"electrical\":{\"drainSourceVoltage\":" + std::to_string(vds) +
           ",\"continuousDrainCurrent\":" + std::to_string(id) +
           ",\"onResistance\":" + std::to_string(rds) +
           ",\"totalGateCharge\":" + std::to_string(qg) + "}}}}}}";
}

std::string tmp_path(const std::string& name) {
    return (fs::temp_directory_path() / ("kelvin_test_" + name)).string();
}

void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << content;
}
}  // namespace

TEST_CASE("index: build counts + round-trip", "[index]") {
    std::string path = tmp_path("counts.ndjson");
    std::string content = mosfet_line("A", 100, 10, 0.01, 2e-9) + "\n" +
                          "\n" +  // blank line: skipped, NOT counted
                          mosfet_line("B", 60, 5, 0.05, 1e-9) + "\n" +
                          "{\"semiconductor\":{\"mosfet\":{}}}\n" +  // unreadable (missing fields)
                          mosfet_line("C", 200, 20, 0.005, 4e-9) + "\n";
    write_file(path, content);

    auto shard = build_mosfet_shard(path);
    REQUIRE(shard.meta.row_count == 3);
    REQUIRE(shard.meta.unreadable_row_count == 1);
    REQUIRE(shard.meta.source_line_count == 4);  // == row + unreadable (blank excluded)
    REQUIRE(shard.meta.row_count + shard.meta.unreadable_row_count == shard.meta.source_line_count);

    // Line numbers are the physical (blank-inclusive) line: A=1, B=3, C=5.
    REQUIRE(shard.rows[0].mpn == "A");
    REQUIRE(shard.rows[0].lineno == 1);
    REQUIRE(shard.rows[1].mpn == "B");
    REQUIRE(shard.rows[1].lineno == 3);
    REQUIRE(shard.rows[2].mpn == "C");
    REQUIRE(shard.rows[2].lineno == 5);

    // serialize -> deserialize preserves everything.
    std::string bytes = serialize_shard(shard);
    auto back = deserialize_mosfet_shard(bytes);
    REQUIRE(back.meta.row_count == shard.meta.row_count);
    REQUIRE(back.meta.build_id == shard.meta.build_id);
    REQUIRE(back.rows.size() == shard.rows.size());
    for (size_t i = 0; i < back.rows.size(); ++i) {
        REQUIRE(back.rows[i].mpn == shard.rows[i].mpn);
        REQUIRE(back.rows[i].vds_rated == shard.rows[i].vds_rated);
        REQUIRE(back.rows[i].src_offset == shard.rows[i].src_offset);
        REQUIRE(back.rows[i].src_length == shard.rows[i].src_length);
    }
    fs::remove(path);
}

TEST_CASE("index: envelope fetch by byte span round-trips the source line", "[index]") {
    std::string path = tmp_path("fetch.ndjson");
    write_file(path, mosfet_line("A", 100, 10, 0.01, 2e-9) + "\n" +
                         mosfet_line("B", 60, 5, 0.05, 1e-9) + "\n");
    auto shard = build_mosfet_shard(path);
    for (const auto& r : shard.rows) {
        std::ifstream f(path, std::ios::binary);
        f.seekg(static_cast<std::streamoff>(r.src_offset));
        std::string buf(r.src_length, '\0');
        f.read(&buf[0], r.src_length);
        auto j = nlohmann::json::parse(buf);
        REQUIRE(j.at("semiconductor").at("mosfet").at("manufacturerInfo").at("reference") == r.mpn);
    }
    fs::remove(path);
}

TEST_CASE("index: structurally invalid JSON throws DataError at the right line", "[index]") {
    std::string path = tmp_path("bad.ndjson");
    write_file(path, mosfet_line("A", 100, 10, 0.01, 2e-9) + "\n" + "{not json}\n");
    try {
        build_mosfet_shard(path);
        FAIL("expected DataError");
    } catch (const DataError& e) {
        REQUIRE(e.lineno == 2);
    }
    fs::remove(path);
}

TEST_CASE("index: non-object top-level line throws DataError", "[index]") {
    std::string path = tmp_path("nonobj.ndjson");
    write_file(path, std::string("[1,2,3]\n"));
    REQUIRE_THROWS_AS(build_mosfet_shard(path), DataError);
    fs::remove(path);
}

TEST_CASE("index: incremental tail build == full rebuild (byte-identical)", "[index][incremental]") {
    std::string path = tmp_path("incr.ndjson");
    std::string base = mosfet_line("A", 100, 10, 0.01, 2e-9) + "\n" +
                       mosfet_line("B", 60, 5, 0.05, 1e-9) + "\n";
    write_file(path, base);
    auto prev = build_mosfet_shard(path);

    // Append two records (the nightly's append-only promote).
    std::string appended = base + mosfet_line("C", 200, 20, 0.005, 4e-9) + "\n" +
                           mosfet_line("D", 30, 3, 0.1, 0.5e-9) + "\n";
    write_file(path, appended);

    auto incremental = build_mosfet_shard(path, &prev);
    auto full = build_mosfet_shard(path);  // no prev -> full rebuild

    REQUIRE(incremental.meta.row_count == full.meta.row_count);
    REQUIRE(incremental.meta.source_line_count == full.meta.source_line_count);
    REQUIRE(incremental.meta.build_id == full.meta.build_id);
    REQUIRE(serialize_shard(incremental) == serialize_shard(full));  // byte-identical
    // The appended rows kept correct physical line numbers (C=3, D=4).
    REQUIRE(incremental.rows[2].mpn == "C");
    REQUIRE(incremental.rows[2].lineno == 3);
    REQUIRE(incremental.rows[3].mpn == "D");
    REQUIRE(incremental.rows[3].lineno == 4);
    fs::remove(path);
}

TEST_CASE("index: staleness detects append", "[index]") {
    std::string path = tmp_path("stale.ndjson");
    write_file(path, mosfet_line("A", 100, 10, 0.01, 2e-9) + "\n");
    auto shard = build_mosfet_shard(path);
    REQUIRE_FALSE(shard_is_stale(shard.meta, path));
    write_file(path, mosfet_line("A", 100, 10, 0.01, 2e-9) + "\n" +
                         mosfet_line("B", 60, 5, 0.05, 1e-9) + "\n");
    REQUIRE(shard_is_stale(shard.meta, path));
    fs::remove(path);
}
