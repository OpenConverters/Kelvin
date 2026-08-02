// [index] Round-trip, counts, DataError, and incremental (tail) build equivalence.
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "Browse.hpp"
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

// ---- ABT #426 --------------------------------------------------------------
// A shard is keyed on the code that produced it, not only on the bytes it read. Both
// cache paths used to key on the source alone, so an extract_* change was invisible:
// an unchanged catalogue returned the cached shard verbatim, and an appended one reused
// every pre-existing row. The symptom was a real extractor fix landing as a no-op with
// an unchanged row count and a freshly recomputed buildId, hiding the staleness.
TEST_CASE("index: a shard from different extractor code is stale", "[index][abt426]") {
    std::string path = tmp_path("extractor_stale.ndjson");
    write_file(path, mosfet_line("A", 100, 10, 0.01, 2e-9) + "\n");
    auto shard = build_mosfet_shard(path);

    REQUIRE(shard.meta.extractor_hash == extractor_version());
    REQUIRE_FALSE(shard_is_stale(shard.meta, path));

    // Same bytes, shard built by a different build of the extractors.
    ShardMeta forged = shard.meta;
    forged.extractor_hash ^= 0x9e3779b97f4a7c15ULL;
    REQUIRE(shard_is_stale(forged, path));
    fs::remove(path);
}

TEST_CASE("index: incremental build refuses to reuse foreign extractor rows",
          "[index][abt426][incremental]") {
    std::string path = tmp_path("extractor_incr.ndjson");
    std::string base = mosfet_line("A", 100, 10, 0.01, 2e-9) + "\n" +
                       mosfet_line("B", 60, 5, 0.05, 1e-9) + "\n";
    write_file(path, base);
    auto prev = build_mosfet_shard(path);

    // Pretend prev came from an older extractor, then append — exactly the nightly's
    // shape after someone edits a view. The prefix must be RE-PARSED, not reused.
    prev.meta.extractor_hash ^= 0x9e3779b97f4a7c15ULL;
    prev.rows[0].mpn = "STALE-ROW-THAT-MUST-NOT-SURVIVE";

    write_file(path, base + mosfet_line("C", 200, 20, 0.005, 4e-9) + "\n");
    auto incremental = build_mosfet_shard(path, &prev);
    auto full = build_mosfet_shard(path);

    REQUIRE(incremental.rows[0].mpn == "A");  // re-parsed, not carried over from prev
    REQUIRE(incremental.meta.extractor_hash == extractor_version());
    REQUIRE(serialize_shard(incremental) == serialize_shard(full));
    fs::remove(path);
}

// ABT #485: connector.manufacturerInfo.datasheetInfo.mechanical.pitch is stated on 246k of
// the 392k catalogue connectors and was extracted by nothing, so the shard row — and every
// consumer of it — behaved as though the catalogue had no pitch at all. A connector record
// carries no body outline, so pitch IS its land pattern: dropping it left the cross-reference
// with nothing to compare and it graded 2.54 mm parts drop_in against a 2.00 mm original.
TEST_CASE("index: connector pitch reaches the shard row and survives a round-trip",
          "[index][connector][abt485]") {
    auto connector_line = [](const std::string& mpn, int positions, const std::string& pitch) {
        return "{\"connector\":{\"manufacturerInfo\":{\"name\":\"ACME\",\"reference\":\"" + mpn +
               "\",\"status\":\"production\",\"datasheetInfo\":{\"part\":{\"partNumber\":\"" + mpn +
               "\",\"matingPolarity\":\"male\"},\"mechanical\":{\"positions\":" +
               std::to_string(positions) + (pitch.empty() ? "" : ",\"pitch\":" + pitch) +
               "},\"familyDetails\":{\"family\":\"boardToBoard\"}}}}}";
    };
    std::string path = tmp_path("connector_pitch.ndjson");
    write_file(path, connector_line("P200", 13, "0.002") + "\n" +
                         connector_line("P254", 13, "0.00254") + "\n" +
                         connector_line("NOPITCH", 13, "") + "\n");
    auto shard = build_connector_shard(path);
    REQUIRE(shard.meta.row_count == 3);
    REQUIRE(shard.rows[0].pitch == 0.002);       // metres, as the catalogue states it
    REQUIRE(shard.rows[1].pitch == 0.00254);
    REQUIRE(std::isnan(shard.rows[2].pitch));    // absent stays UNKNOWN, never 0

    auto back = deserialize_connector_shard(serialize_shard(shard));
    REQUIRE(back.rows[0].pitch == shard.rows[0].pitch);
    REQUIRE(back.rows[1].pitch == shard.rows[1].pitch);
    REQUIRE(std::isnan(back.rows[2].pitch));

    // and it is visible to the caller that builds the cross-reference spec block
    nlohmann::json rows =
        browse::browse_rows(shard, nlohmann::json{{"limit", 10}}).at("rows");
    REQUIRE(rows[0].at("pitch").get<double>() == 0.002);
    REQUIRE(rows[2].at("pitch").is_null());
    fs::remove(path);
}

// ABT #520: connector.manufacturerInfo.datasheetInfo.environmental.operatingTemperature is
// stated on 365,680 of the 391,073 catalogue connectors and reached nothing — extract_connector
// never opened the environmental block. Both ends were lost, so a +105 degC substitute for a
// +125 degC original carried no temperature verdict at all and was reported an "upgrade".
TEST_CASE("index: connector operating temperature reaches the shard row and round-trips",
          "[index][connector][abt520]") {
    auto connector_line = [](const std::string& mpn, const std::string& temps) {
        return "{\"connector\":{\"manufacturerInfo\":{\"name\":\"ACME\",\"reference\":\"" + mpn +
               "\",\"status\":\"production\",\"datasheetInfo\":{\"part\":{\"partNumber\":\"" + mpn +
               "\",\"matingPolarity\":\"male\"},\"mechanical\":{\"positions\":25}," +
               (temps.empty() ? "" : "\"environmental\":{\"operatingTemperature\":" + temps + "},") +
               "\"familyDetails\":{\"family\":\"dataInterface\"}}}}}";
    };
    std::string path = tmp_path("connector_temp.ndjson");
    write_file(path, connector_line("HOT", "{\"minimum\":-55,\"maximum\":125}") + "\n" +
                         connector_line("COLD_ZERO", "{\"minimum\":0,\"maximum\":105}") + "\n" +
                         connector_line("NOTEMP", "") + "\n");
    auto shard = build_connector_shard(path);
    REQUIRE(shard.meta.row_count == 3);
    REQUIRE(shard.rows[0].temp_min_c == -55);   // negative is a value, not absence
    REQUIRE(shard.rows[0].temp_max_c == 125);
    REQUIRE(shard.rows[1].temp_min_c == 0);     // and so is 0 degC
    REQUIRE(shard.rows[1].temp_max_c == 105);
    REQUIRE(std::isnan(shard.rows[2].temp_min_c));  // absent stays UNKNOWN, never 0
    REQUIRE(std::isnan(shard.rows[2].temp_max_c));

    auto back = deserialize_connector_shard(serialize_shard(shard));
    REQUIRE(back.rows[0].temp_min_c == -55);
    REQUIRE(back.rows[0].temp_max_c == 125);
    REQUIRE(back.rows[1].temp_min_c == 0);
    REQUIRE(std::isnan(back.rows[2].temp_max_c));

    // and it is visible to the caller that builds the cross-reference spec block
    nlohmann::json rows =
        browse::browse_rows(shard, nlohmann::json{{"limit", 10}}).at("rows");
    REQUIRE(rows[0].at("temp_min_c").get<double>() == -55);
    REQUIRE(rows[0].at("temp_max_c").get<double>() == 125);
    REQUIRE(rows[1].at("temp_min_c").get<double>() == 0);
    REQUIRE(rows[2].at("temp_max_c").is_null());
    fs::remove(path);
}

// ABT #487: the connector family caveat told the engineer the catalogue carries no plating,
// termination or mating-cycle data, so full mating compatibility could not be checked. The
// records carry all three — 97,144 / 9,908 / 56,573 of the 391,073 — and extract_connector
// opened neither the material block nor familyDetails.termination nor mechanical.matingCycles,
// which is what the caveat was really describing.
TEST_CASE("index: connector mating fields reach the shard row and round-trip",
          "[index][connector][abt487]") {
    auto connector_line = [](const std::string& mpn, const std::string& plating,
                             const std::string& termination, const std::string& cycles) {
        return "{\"connector\":{\"manufacturerInfo\":{\"name\":\"ACME\",\"reference\":\"" + mpn +
               "\",\"status\":\"production\",\"datasheetInfo\":{\"part\":{\"partNumber\":\"" + mpn +
               "\",\"matingPolarity\":\"male\"},\"mechanical\":{\"positions\":13" +
               (cycles.empty() ? "" : ",\"matingCycles\":" + cycles) + "}," +
               (plating.empty() ? "" : "\"material\":{\"contactPlating\":{\"matingAreaMaterialRef\":\"" +
                                           plating + "\",\"matingAreaThickness\":7.62e-07}},") +
               "\"familyDetails\":{\"family\":\"boardToBoard\"" +
               (termination.empty() ? "" : ",\"termination\":\"" + termination + "\"") + "}}}}}";
    };
    std::string path = tmp_path("connector_mating.ndjson");
    write_file(path, connector_line("GOLD", "au-gold", "crimp", "500") + "\n" +
                         connector_line("TIN", "sn-tin", "idc", "30") + "\n" +
                         connector_line("BARE", "", "", "") + "\n");
    auto shard = build_connector_shard(path);
    REQUIRE(shard.meta.row_count == 3);
    // the MATERIAL, not the thickness: gold over 0.25 um and over 0.76 um are one interface
    REQUIRE(shard.rows[0].contact_plating == "au-gold");
    REQUIRE(shard.rows[0].termination == "crimp");
    REQUIRE(shard.rows[0].mating_cycles == 500);
    REQUIRE(shard.rows[1].contact_plating == "sn-tin");
    REQUIRE(shard.rows[1].termination == "idc");
    REQUIRE(shard.rows[1].mating_cycles == 30);
    REQUIRE(shard.rows[2].contact_plating.empty());  // absent stays UNKNOWN, never a value
    REQUIRE(shard.rows[2].termination.empty());
    REQUIRE(std::isnan(shard.rows[2].mating_cycles));

    auto back = deserialize_connector_shard(serialize_shard(shard));
    REQUIRE(back.rows[0].contact_plating == "au-gold");
    REQUIRE(back.rows[0].termination == "crimp");
    REQUIRE(back.rows[0].mating_cycles == 500);
    REQUIRE(back.rows[2].contact_plating.empty());
    REQUIRE(std::isnan(back.rows[2].mating_cycles));

    // and it is visible to the caller that builds the cross-reference spec block
    nlohmann::json rows =
        browse::browse_rows(shard, nlohmann::json{{"limit", 10}}).at("rows");
    REQUIRE(rows[0].at("contact_plating").get<std::string>() == "au-gold");
    REQUIRE(rows[0].at("termination").get<std::string>() == "crimp");
    REQUIRE(rows[0].at("mating_cycles").get<double>() == 500);
    REQUIRE(rows[2].at("mating_cycles").is_null());
    fs::remove(path);
}

// ABT #488: electrical.breakdownVoltage is a dimensionWithTolerance and extract_diode kept
// only the resolved nominal, so the WINDOW 3,728 of the 8,274 catalogue zeners (and 137 TVS)
// guarantee was dropped between the record and the shard row. The A grade and the B grade of
// the same marked voltage became the same row, and the cross-reference had nothing to compare.
TEST_CASE("index: diode breakdown-voltage band reaches the shard row and round-trips",
          "[index][diode][abt488]") {
    auto zener_line = [](const std::string& mpn, const std::string& breakdown) {
        return "{\"semiconductor\":{\"diode\":{\"manufacturerInfo\":{\"name\":\"ACME\","
               "\"reference\":\"" +
               mpn + "\",\"status\":\"production\",\"datasheetInfo\":{\"part\":{\"partNumber\":\"" +
               mpn + "\",\"subType\":\"zener\",\"case\":\"SOT23\"},\"electrical\":{" +
               "\"breakdownVoltage\":" + breakdown + "}}}}}}";
    };
    std::string path = tmp_path("diode_vz.ndjson");
    write_file(path, zener_line("A_GRADE", "{\"nominal\":3.6,\"minimum\":3.56,\"maximum\":3.64}") +
                         "\n" +
                         zener_line("B_GRADE", "{\"nominal\":3.6,\"minimum\":3.42,\"maximum\":3.78}") +
                         "\n" + zener_line("NOM_ONLY", "{\"nominal\":3.6}") + "\n" +
                         zener_line("MIN_ONLY", "{\"nominal\":3.6,\"minimum\":3.56}") + "\n");
    auto shard = build_diode_shard(path);
    REQUIRE(shard.meta.row_count == 4);
    // the nominal still resolves as before — the band is carried BESIDE it, not instead
    REQUIRE(shard.rows[0].vrrm_rated == 3.6);
    REQUIRE(shard.rows[0].vz_min == 3.56);
    REQUIRE(shard.rows[0].vz_max == 3.64);
    REQUIRE(shard.rows[0].vz_tolerance == Catch::Approx(0.0111111).epsilon(1e-4));  // +/-1.11 %
    REQUIRE(shard.rows[1].vz_tolerance == Catch::Approx(0.05).epsilon(1e-6));       // +/-5 %
    // A record that states only the nominal states no window: unknown, never 0 %.
    REQUIRE(std::isnan(shard.rows[2].vz_min));
    REQUIRE(std::isnan(shard.rows[2].vz_max));
    REQUIRE(std::isnan(shard.rows[2].vz_tolerance));
    // and half a band is not a band, though the bound it does state is kept
    REQUIRE(shard.rows[3].vz_min == 3.56);
    REQUIRE(std::isnan(shard.rows[3].vz_max));
    REQUIRE(std::isnan(shard.rows[3].vz_tolerance));

    auto back = deserialize_diode_shard(serialize_shard(shard));
    REQUIRE(back.rows[0].vz_min == 3.56);
    REQUIRE(back.rows[0].vz_max == 3.64);
    REQUIRE(back.rows[0].vz_tolerance == shard.rows[0].vz_tolerance);
    REQUIRE(std::isnan(back.rows[2].vz_tolerance));

    // and it is visible to the caller that builds the cross-reference spec block
    nlohmann::json rows =
        browse::browse_rows(shard, nlohmann::json{{"limit", 10}}).at("rows");
    REQUIRE(rows[0].at("vz_min").get<double>() == 3.56);
    REQUIRE(rows[0].at("vz_max").get<double>() == 3.64);
    REQUIRE(rows[0].at("vz_tolerance").get<double>() == Catch::Approx(0.0111111).epsilon(1e-4));
    REQUIRE(rows[2].at("vz_tolerance").is_null());
    fs::remove(path);
}
