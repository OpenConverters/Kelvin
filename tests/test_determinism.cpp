// [determinism] Bit-identical rebuilds (no timestamps in the format) and lineno tie-breaking.
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "Index.hpp"
#include "Select.hpp"

using namespace kelvin;
namespace fs = std::filesystem;

namespace {
std::string cap_line(const std::string& mpn, double cap, double v, double esr,
                     const std::string& status = "production") {
    return "{\"capacitor\":{\"manufacturerInfo\":{\"name\":\"ACME\",\"reference\":\"" + mpn +
           "\",\"status\":\"" + status +
           "\",\"datasheetInfo\":{\"part\":{\"family\":\"ceramic\"},\"electrical\":{"
           "\"capacitance\":{\"nominal\":" +
           std::to_string(cap) + "},\"ratedVoltage\":" + std::to_string(v) +
           ",\"esr\":" + std::to_string(esr) + "}}}}}";
}
std::string tmp_path(const std::string& n) {
    return (fs::temp_directory_path() / ("kelvin_det_" + n)).string();
}
void write_file(const std::string& p, const std::string& c) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << c;
}
}  // namespace

TEST_CASE("determinism: two rebuilds produce byte-identical shards", "[determinism]") {
    std::string path = tmp_path("id.ndjson");
    write_file(path, cap_line("A", 1e-6, 50, 0.1) + "\n" + cap_line("B", 2e-6, 25, 0.05) + "\n");
    auto s1 = build_capacitor_shard(path);
    auto s2 = build_capacitor_shard(path);
    REQUIRE(serialize_shard(s1) == serialize_shard(s2));
    REQUIRE(s1.meta.build_id == s2.meta.build_id);
    fs::remove(path);
}

TEST_CASE("determinism: equal sort keys break by source line order", "[determinism]") {
    // Two caps with identical ESR (=0) and identical thermal status; LOWEST_ESR ties on the
    // metric, so the earlier file line must win (mirrors Python min() first-in-order).
    std::string path = tmp_path("tie.ndjson");
    write_file(path, cap_line("FIRST", 1e-6, 50, 0.0) + "\n" + cap_line("SECOND", 1e-6, 50, 0.0) +
                         "\n");
    auto shard = build_capacitor_shard(path);
    CapacitorConstraints c;
    c.capacitance_min = 1e-7;
    c.capacitance_max = 1e-4;
    c.v_rated_min = 10;
    auto r = select_capacitor(shard, c, CapacitorTiebreaker::LowestEsr);
    REQUIRE(r.at("candidates")[0].at("mpn") == "FIRST");
    REQUIRE(r.at("candidates")[1].at("mpn") == "SECOND");
    fs::remove(path);
}
