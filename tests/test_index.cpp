// [index] Round-trip, counts, DataError, and incremental (tail) build equivalence.
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "Browse.hpp"
#include "Index.hpp"
#include "RecordFetcher.hpp"
#include "Views.hpp"

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
// ABT #557: the IEC 60384-14 line-safety approval of a mains capacitor (X1/X2/X3,
// Y1/Y2/Y3/Y4) exists NOWHERE in the catalogue as a field — the schema has no
// safety-class column at all (ABT #677). The vendors write it into the series name,
// and the extractor dropped that string, so the cross-reference had only voltages to
// compare and graded six KEMET R46 (X2) parts drop-in upgrades for a WIMA MKP-X1 R.
TEST_CASE("index: the capacitor series/family string reaches the shard row and round-trips",
          "[index][capacitor][abt557]") {
    auto cap_line = [](const std::string& mpn, const std::string& family,
                       const std::string& series) {
        return "{\"capacitor\":{\"manufacturerInfo\":{\"name\":\"ACME\",\"reference\":\"" + mpn +
               "\",\"status\":\"production\"" +
               (family.empty() ? "" : ",\"family\":\"" + family + "\"") +
               ",\"datasheetInfo\":{\"part\":{\"partNumber\":\"" + mpn +
               "\",\"technology\":\"film-polypropylene\"" +
               (series.empty() ? "" : ",\"series\":\"" + series + "\"") +
               "},\"electrical\":{\"capacitance\":{\"nominal\":3.3e-07},\"ratedVoltage\":440}}}}}";
    };
    std::string path = tmp_path("capacitor_family.ndjson");
    write_file(path, cap_line("KEMETX1", "R47 X1 440 VAC", "") + "\n" +   // family only
                         cap_line("WIMAX1", "", "MKP-X1 R") + "\n" +      // series fallback
                         cap_line("NOSERIES", "", "") + "\n");            // states neither
    auto shard = build_capacitor_shard(path);
    REQUIRE(shard.meta.row_count == 3);
    REQUIRE(shard.rows[0].family == "R47 X1 440 VAC");
    REQUIRE(shard.rows[1].family == "MKP-X1 R");
    REQUIRE(shard.rows[2].family.empty());  // absent stays absent, never invented

    auto back = deserialize_capacitor_shard(serialize_shard(shard));
    REQUIRE(back.rows[0].family == shard.rows[0].family);
    REQUIRE(back.rows[1].family == shard.rows[1].family);
    REQUIRE(back.rows[2].family.empty());

    // and it is visible to the caller that builds the cross-reference spec block —
    // which is the whole point: the ranker reads the class out of this string.
    nlohmann::json rows = browse::browse_rows(shard, nlohmann::json{{"limit", 10}}).at("rows");
    REQUIRE(rows[0].at("family").get<std::string>() == "R47 X1 440 VAC");
    REQUIRE(rows[1].at("family").get<std::string>() == "MKP-X1 R");
    fs::remove(path);
}

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

// ABT #505: familyDetails is a discriminated union on family, and two axes are spelled per
// variant — the wire attach is 'termination' on wireToBoard/wireToWire/power and 'clampType'
// on terminalBlock, the standardised interface is 'interfaceStandard' on dataInterface and
// 'interface' on rf. extract_connector read only one spelling of each, so 11,627 records
// stating a clamp type (more than the 9,908 stating a termination) and 8,235 stating an RF
// interface reached the ranker with those slots empty, while the family caveat told the
// engineer both ARE compared where the records state them. A 7.5 mm spring-cage WAGO block
// scored against screw-clamp parts with no wire-attach verdict at all.
TEST_CASE("index: connector familyDetails variants spell one axis two ways",
          "[index][connector][abt505]") {
    auto block_line = [](const std::string& mpn, const std::string& family,
                         const std::string& detail) {
        return "{\"connector\":{\"manufacturerInfo\":{\"name\":\"ACME\",\"reference\":\"" + mpn +
               "\",\"status\":\"production\",\"datasheetInfo\":{\"part\":{\"partNumber\":\"" + mpn +
               "\"},\"mechanical\":{\"positions\":9,\"pitch\":0.0075},\"familyDetails\":{\"family\":\"" +
               family + "\"" + detail + "}}}}}";
    };
    std::string path = tmp_path("connector_clamp.ndjson");
    write_file(path, block_line("CAGE", "terminalBlock", ",\"clampType\":\"springCage\"") + "\n" +
                         block_line("SCREW", "terminalBlock", ",\"clampType\":\"screw\"") + "\n" +
                         block_line("CRIMP", "wireToBoard", ",\"termination\":\"crimp\"") + "\n" +
                         block_line("MUTE", "terminalBlock", "") + "\n" +
                         block_line("SMA", "rf", ",\"interface\":\"SMA\"") + "\n" +
                         block_line("USBC", "dataInterface", ",\"interfaceStandard\":\"USB-C\"") + "\n");
    auto shard = build_connector_shard(path);
    REQUIRE(shard.meta.row_count == 6);
    REQUIRE(shard.rows[0].termination == "springCage");
    REQUIRE(shard.rows[1].termination == "screw");
    REQUIRE(shard.rows[2].termination == "crimp");  // the other spelling still reads
    REQUIRE(shard.rows[3].termination.empty());     // absent stays UNKNOWN, never a value
    REQUIRE(shard.rows[4].interface_standard == "SMA");
    REQUIRE(shard.rows[5].interface_standard == "USB-C");
    REQUIRE(shard.rows[0].interface_standard.empty());

    auto back = deserialize_connector_shard(serialize_shard(shard));
    REQUIRE(back.rows[0].termination == "springCage");
    REQUIRE(back.rows[3].termination.empty());
    REQUIRE(back.rows[4].interface_standard == "SMA");

    // and it is visible to the caller that builds the cross-reference spec block
    nlohmann::json rows =
        browse::browse_rows(shard, nlohmann::json{{"limit", 10}}).at("rows");
    REQUIRE(rows[0].at("termination").get<std::string>() == "springCage");
    REQUIRE(rows[1].at("termination").get<std::string>() == "screw");
    REQUIRE(rows[4].at("interface_standard").get<std::string>() == "SMA");
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

// ---------------------------------------------------------------------------
// The shard cache is a SHARED directory (KELVIN_INDEX_DIR, default ~/.kelvin/index),
// and everything below is about two users of it not corrupting each other. Both bugs
// surfaced the same way and from a long distance: a JSON parse error raised while
// fetching a selected part's full record, naming neither file nor offset, in whichever
// test happened to be running when another process was building.
// ---------------------------------------------------------------------------

TEST_CASE("index: a shard is installed atomically, never truncated in place", "[index]") {
    // Truncating the real file left a window in which any reader — another test run,
    // another session on the same box — saw a partial shard whose byte offsets then
    // addressed the NDJSON at nonsense positions.
    const std::string path = tmp_path("atomic.ndjson");
    const std::string shard = tmp_path("atomic.kidx");
    fs::remove(shard);
    write_file(path, mosfet_line("M1", 100, 10, 0.01, 20e-9) + "\n" +
                     mosfet_line("M2", 200, 20, 0.02, 40e-9) + "\n");
    Shard<MosfetRow> s = build_mosfet_shard(path);
    write_shard(shard, s);
    const auto first = fs::file_size(shard);
    REQUIRE(first > 0);

    // Rewrite it repeatedly; the installed file is complete and loadable after each,
    // and no .tmp is left lying around.
    for (int i = 0; i < 3; ++i) {
        write_shard(shard, s);
        REQUIRE(fs::file_size(shard) == first);
        Shard<MosfetRow> back = read_mosfet_shard(shard);
        CHECK(back.rows.size() == s.rows.size());
    }
    int strays = 0;
    for (const auto& e : fs::directory_iterator(fs::path(shard).parent_path()))
        if (e.path().filename().string().rfind("kelvin_test_atomic.kidx.tmp", 0) == 0) ++strays;
    CHECK(strays == 0);
    fs::remove(shard);
    fs::remove(path);
}

TEST_CASE("index: a record span that is not a record boundary says so", "[index]") {
    // A stale index used to reach json::parse with the middle of some other record and
    // throw "parse error at line 1, column 1 ... last read: 'c'", which names nothing.
    // Worse, bytes that happened to parse would have returned the WRONG part as the
    // right one.
    const std::string path = tmp_path("boundary.ndjson");
    const std::string a = mosfet_line("M1", 100, 10, 0.01, 20e-9);
    write_file(path, a + "\n" + mosfet_line("M2", 200, 20, 0.02, 40e-9) + "\n");

    FileRecordFetcher fetch(path);
    CHECK(fetch.fetch(0, static_cast<uint32_t>(a.size())).is_object());   // the real span works

    // …and a span starting 20 bytes into it is refused, by name.
    REQUIRE_THROWS_WITH(fetch.fetch(20, static_cast<uint32_t>(a.size() - 20)),
                        Catch::Matchers::ContainsSubstring("does not match its source") &&
                        Catch::Matchers::ContainsSubstring("byte 20"));
    fs::remove(path);
}

TEST_CASE("index: a source that changed size since the shard was built is refused at open",
          "[index]") {
    // The shard records how big its source was. A different size is proof — not suspicion —
    // that the offsets cannot address this file, and it is far better caught once at open
    // than as a mangled record later (ABT #1108).
    const std::string path = tmp_path("moved.ndjson");
    write_file(path, mosfet_line("M1", 100, 10, 0.01, 20e-9) + "\n");
    Shard<MosfetRow> s = build_mosfet_shard(path);
    const uint64_t built_against = s.meta.source_size;

    CHECK_NOTHROW(FileRecordFetcher(path, built_against));      // unchanged: fine

    write_file(path, mosfet_line("M1", 100, 10, 0.01, 20e-9) + "\n" +
                     mosfet_line("M2", 200, 20, 0.02, 40e-9) + "\n");
    REQUIRE_THROWS_WITH(FileRecordFetcher(path, built_against),
                        Catch::Matchers::ContainsSubstring("its index was built against"));
    fs::remove(path);
}

TEST_CASE("index: a source rewritten IN PLACE mid-read is refused", "[index]") {
    // The dangerous case: not a parse error but a SILENT one. A span whose bytes happen to
    // parse returns a different part as the right one. Seen for real — another session
    // rewrote a 632 MB catalogue while a 36-minute suite was reading it (ABT #1108).
    const std::string path = tmp_path("inplace.ndjson");
    const std::string a = mosfet_line("M1", 100, 10, 0.01, 20e-9);
    write_file(path, a + "\n" + mosfet_line("M2", 200, 20, 0.02, 40e-9) + "\n");

    FileRecordFetcher fetch(path);
    CHECK(fetch.fetch(0, static_cast<uint32_t>(a.size())).is_object());

    // rewrite the SAME inode with different content
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));   // mtime has 1 s resolution
    write_file(path, mosfet_line("Z9", 600, 60, 0.06, 90e-9) + "\n" + a + "\n");
    REQUIRE_THROWS_WITH(fetch.fetch(0, static_cast<uint32_t>(a.size())),
                        Catch::Matchers::ContainsSubstring("rewritten IN PLACE"));
    fs::remove(path);
}

TEST_CASE("index: a capacitor's ESL survives the shard round trip (ABT #1122)",
          "[index]") {
    // The one quantity a layout cannot supply and a package name cannot stand in
    // for: inside a single 0402 the real spread is 120 to 1392 pH, against
    // Faraday's flat 0.4 nH for every one of them. It lives in the equivalent
    // circuit (CAS modelParams.ls), not the electrical block.
    const std::string path = tmp_path("esl.ndjson");
    const std::string with_ls =
        R"({"capacitor":{"manufacturerInfo":{"name":"Wurth Elektronik","reference":"885012206077",)"
        R"("status":"production","datasheetUrl":"https://we-online.com/x.pdf","datasheetInfo":{)"
        R"("part":{"partNumber":"885012206077","technology":"ceramic-class-2","case":"0402"},)"
        R"("electrical":{"capacitance":1e-7,"ratedVoltage":50.0},)"
        R"("modelParams":{"rs":0.0444,"cs":1e-7,"ls":7.99e-10,"riso":5e8}}}}})";
    const std::string without_ls =
        R"({"capacitor":{"manufacturerInfo":{"name":"ACME","reference":"NOMODEL1",)"
        R"("status":"production","datasheetUrl":"https://acme.com/x.pdf","datasheetInfo":{)"
        R"("part":{"partNumber":"NOMODEL1","technology":"ceramic-class-2","case":"0402"},)"
        R"("electrical":{"capacitance":1e-7,"ratedVoltage":50.0}}}}})";
    write_file(path, with_ls + "\n" + without_ls + "\n");

    Shard<CapacitorRow> s = build_capacitor_shard(path);
    REQUIRE(s.rows.size() == 2);
    const CapacitorRow* we = nullptr;
    const CapacitorRow* plain = nullptr;
    for (const auto& r : s.rows) {
        if (r.mpn == "885012206077") we = &r;
        if (r.mpn == "NOMODEL1") plain = &r;
    }
    REQUIRE(we);
    REQUIRE(plain);
    CHECK_THAT(we->esl, Catch::Matchers::WithinRel(7.99e-10, 1e-9));
    // absent stays ABSENT, never 0 — a 0 H ESL is a perfect capacitor, and the
    // package estimate downstream must still be able to tell it had no data
    CHECK_FALSE(std::isfinite(plain->esl));

    // …and it survives serialisation, which is what the format bump is for
    const std::string shard = tmp_path("esl.kidx");
    write_shard(shard, s);
    Shard<CapacitorRow> back = read_capacitor_shard(shard);
    REQUIRE(back.rows.size() == 2);
    for (const auto& r : back.rows) {
        if (r.mpn == "885012206077") CHECK_THAT(r.esl, Catch::Matchers::WithinRel(7.99e-10, 1e-9));
        if (r.mpn == "NOMODEL1") CHECK_FALSE(std::isfinite(r.esl));
    }
    fs::remove(path);
    fs::remove(shard);
}

// ---------------------------------------------------------------------------
// A 0 ohm link is a resistor with a resistance of zero, not a resistor whose
// resistance is unknown. Kelvin used to reject both together and deleted 107
// real parts from the catalogue — Yageo's RC series ships them in every case
// size — so a board using one was told its part did not exist (ABT #1123).
// ---------------------------------------------------------------------------
TEST_CASE("resistor view: zero ohms is a value, absent is not", "[index][views]") {
    auto env = [](const nlohmann::json& electrical) {
        return nlohmann::json{{"resistor", {{"manufacturerInfo",
            {{"name", "YAGEO"}, {"reference", "RC0402FR-070RL"},
             {"status", "production"},
             {"datasheetInfo", {{"part", {{"partNumber", "RC0402FR-070RL"}}},
                                {"electrical", electrical}}}}}}}};
    };

    // the link itself: kept, and its zero survives into the row
    auto zero = kelvin::extract_resistor(
        env({{"resistance", {{"nominal", 0.0}}}, {"tolerance", 0.01}}));
    REQUIRE(zero.has_value());
    CHECK(zero->resistance == 0.0);
    CHECK(zero->mpn == "RC0402FR-070RL");

    // no resistance at all: still refused, because a row's own default is 0
    // and nothing downstream could tell that apart from the link above
    CHECK_FALSE(kelvin::extract_resistor(env({{"tolerance", 0.01}})).has_value());

    // and a negative one is not a resistor
    CHECK_FALSE(kelvin::extract_resistor(
        env({{"resistance", {{"nominal", -5.0}}}})).has_value());

    // an ordinary part is unaffected
    auto hundred = kelvin::extract_resistor(
        env({{"resistance", {{"nominal", 100.0}}}, {"tolerance", 0.01}}));
    REQUIRE(hundred.has_value());
    CHECK(hundred->resistance == 100.0);
}

// ---------------------------------------------------------------------------
// A distance's best possible value is zero, so zero cannot also mean "nothing
// seen yet". It did, and the collision cost exactly the parts the field exists
// for: a bead sampled AT 100 MHz — the conventional test point, so most of
// them — set closest = 0, which still read as unset, so the next sample
// overwrote it and the proximity test then rejected the lot.
// ---------------------------------------------------------------------------
TEST_CASE("magnetic view: a bead measured at 100 MHz keeps its 100 MHz figure",
          "[index][views]") {
    auto bead = [](std::vector<std::pair<double, double>> pts) {
        nlohmann::json arr = nlohmann::json::array();
        for (auto [f, z] : pts)
            arr.push_back({{"frequency", f}, {"impedance", {{"magnitude", z}}}});
        return nlohmann::json{{"magnetic", {{"manufacturerInfo",
            {{"name", "Wurth Elektronik"}, {"reference", "742792625"},
             {"status", "production"},
             {"datasheetInfo",
              {{"part", {{"partNumber", "742792625"}}},
               {"electrical", nlohmann::json::array({
                   {{"subtype", "chipBead"}, {"impedancePoints", arr}}})}}}}}}}};
    };

    // the real shape of a Wurth WE-CBF record: 120 ohm at 100 MHz, 180 at 400
    auto exact = kelvin::extract_magnetic(bead({{1e8, 120.0}, {4e8, 180.0}}));
    REQUIRE(exact.has_value());
    CHECK(exact->impedance_100mhz == 120.0);   // the headline, not the peak
    CHECK(exact->impedance_peak == 180.0);
    CHECK(exact->impedance_peak_freq == 4e8);
    CHECK(exact->device_type == "chipBead");

    // order must not matter either — the far sample first was the case that
    // happened to work before, and it still has to
    auto rev = kelvin::extract_magnetic(bead({{4e8, 180.0}, {1e8, 120.0}}));
    REQUIRE(rev.has_value());
    CHECK(rev->impedance_100mhz == 120.0);

    // near enough counts: 98 MHz is within the 5 MHz window
    auto near = kelvin::extract_magnetic(bead({{9.8e7, 110.0}, {4e8, 180.0}}));
    REQUIRE(near.has_value());
    CHECK(near->impedance_100mhz == 110.0);

    // and a curve that never goes near 100 MHz must NOT invent one
    auto far = kelvin::extract_magnetic(bead({{1e6, 5.0}, {1e9, 300.0}}));
    REQUIRE(far.has_value());
    CHECK_FALSE(kelvin::present(far->impedance_100mhz));
    CHECK(far->impedance_peak == 300.0);
}
