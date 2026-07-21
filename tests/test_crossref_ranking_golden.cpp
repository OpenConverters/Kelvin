// Golden RANKING corpus for cross_reference() — the one verb that had no pinned
// reference output, so a refactor could silently reorder results and nothing
// would fail. Each case fixes a full multi-candidate ordering that is
// hand-verified as physically correct (not merely "what the engine does now"),
// so this is a correctness pin, not a characterisation snapshot.
//
// If a case here changes, the reviewer must confirm the NEW order is more
// correct than the old one and update the expectation with a reason — never
// blindly re-pin to green.
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "../src/CrossRef.hpp"

using namespace kelvin::crossref;
using nlohmann::json;

namespace {
// Run a case and return the ordered list of candidate mpns.
std::vector<std::string> order(const std::string& cat, const json& orig, const json& cands) {
    Options opt;
    opt.max_results = 50;
    auto r = cross_reference(cat, orig, cands, opt);
    std::vector<std::string> out;
    for (const auto& c : r["candidates"]) out.push_back(c.value("mpn", std::string()));
    return out;
}
std::string grade_of(const std::string& cat, const json& orig, const json& cands,
                     const std::string& mpn) {
    Options opt;
    opt.max_results = 50;
    auto r = cross_reference(cat, orig, cands, opt);
    for (const auto& c : r["candidates"])
        if (c.value("mpn", std::string()) == mpn) return c.value("grade", std::string());
    return "<absent>";
}
}  // namespace

TEST_CASE("golden: MOSFET ranking is value-then-headroom, current shortfall sinks",
          "[crossref][golden]") {
    // 60 V / 100 A / 2 mOhm original. Expected order:
    //  1 EXACT      — same ratings, drop-in
    //  2 BETTER     — more voltage headroom + lower Rds(on), a right-sized upgrade
    //  3 BIG_VDS    — 650 V is gross over-dimensioning, right-sizing penalises it
    //  4 WEAK_I     — 30 A is a severe current shortfall against a known 100 A. Id
    //                is a DEMOTION not a hard gate (unlike Vds): the part carries
    //                less current, the engineer judges if their draw fits — so it
    //                grades major_review and sinks to the bottom on penalty,
    //                rather than being rejected outright.
    json original = {{"mpn", "O"}, {"vds", 60.0}, {"id", 100.0}, {"rds_on", 0.002}};
    json cands = json::array({
        {{"mpn", "BIG_VDS"}, {"vds", 650.0}, {"id", 100.0}, {"rds_on", 0.002}},
        {{"mpn", "WEAK_I"}, {"vds", 60.0}, {"id", 30.0}, {"rds_on", 0.002}},
        {{"mpn", "EXACT"}, {"vds", 60.0}, {"id", 100.0}, {"rds_on", 0.002}},
        {{"mpn", "BETTER"}, {"vds", 75.0}, {"id", 100.0}, {"rds_on", 0.0015}},
    });
    auto o = order("mosfet", original, cands);
    REQUIRE(o.size() == 4);
    CHECK(o[0] == "EXACT");
    CHECK(o[1] == "BETTER");
    CHECK(o[3] == "WEAK_I");  // a severe current shortfall sinks last
    CHECK(grade_of("mosfet", original, cands, "WEAK_I") == "major_review");
}

TEST_CASE("golden: capacitor dielectric + family ordering", "[crossref][golden]") {
    // 100 nF / 50 V X7R 0603 ceramic-class-2. Expected:
    //  1 X7R_SAME   — identical dielectric + size, drop-in
    //  2 X5R        — upper-temperature regression, offered but major review
    //  3 TANT       — different construction family, rejected, sinks
    json original = {{"mpn", "O"}, {"value_si", 1e-7}, {"voltage", 50.0}, {"package", "0603"},
                     {"technology", "ceramic-class-2"}, {"dielectric_code", "X7R"}};
    json cands = json::array({
        {{"mpn", "TANT"}, {"value_si", 1e-7}, {"voltage", 50.0},
         {"technology", "tantalum-mno2"}},
        {{"mpn", "X5R"}, {"value_si", 1e-7}, {"voltage", 50.0}, {"package", "0603"},
         {"technology", "ceramic-class-2"}, {"dielectric_code", "X5R"}},
        {{"mpn", "X7R_SAME"}, {"value_si", 1e-7}, {"voltage", 50.0}, {"package", "0603"},
         {"technology", "ceramic-class-2"}, {"dielectric_code", "X7R"}},
    });
    auto o = order("capacitor", original, cands);
    REQUIRE(o.size() == 3);
    CHECK(o[0] == "X7R_SAME");
    CHECK(o[2] == "TANT");
    CHECK(grade_of("capacitor", original, cands, "X7R_SAME") == "drop_in");
    CHECK(grade_of("capacitor", original, cands, "X5R") == "major_review");
    CHECK(grade_of("capacitor", original, cands, "TANT") == "no_substitute");
}

TEST_CASE("golden: diode voltage ordering", "[crossref][golden]") {
    // 100 V / 3 A schottky. A higher-voltage schottky is the upgrade; a
    // lower-voltage one falls below the hard Vrrm floor and is rejected.
    json original = {{"mpn", "O"}, {"vrrm", 100.0}, {"if_avg", 3.0}, {"technology", "schottky"}};
    json cands = json::array({
        {{"mpn", "UNDER_V"}, {"vrrm", 60.0}, {"if_avg", 3.0}, {"technology", "schottky"}},
        {{"mpn", "SAME"}, {"vrrm", 100.0}, {"if_avg", 3.0}, {"technology", "schottky"}},
        {{"mpn", "HIGHER_V"}, {"vrrm", 150.0}, {"if_avg", 3.0}, {"technology", "schottky"}},
    });
    auto o = order("diode", original, cands);
    REQUIRE(o.size() == 3);
    CHECK(o.back() == "UNDER_V");
    CHECK(grade_of("diode", original, cands, "UNDER_V") == "no_substitute");
}

TEST_CASE("golden: ferrite bead curve ordering", "[crossref][golden]") {
    // 151 ohm @ 100 MHz, peak 939 ohm @ 1038 MHz. Expected:
    //  1 CURVE_MATCH — same headline AND same peak/band, drop-in
    //  2 WRONG_BAND  — same 100 MHz value but peaks far off, major review
    //  3 NO_CURVE    — no impedance data at all, sinks (cannot be verified)
    json original = {{"mpn", "O"}, {"impedance_100mhz", 151.0}, {"impedance_peak", 939.0},
                     {"impedance_peak_freq", 1038e6}, {"dcr", 0.325}, {"rated_current", 0.85}};
    json cands = json::array({
        {{"mpn", "NO_CURVE"}, {"dcr", 0.05}, {"rated_current", 5.0}},
        {{"mpn", "WRONG_BAND"}, {"impedance_100mhz", 151.0}, {"impedance_peak", 15000.0},
         {"impedance_peak_freq", 46e6}, {"dcr", 0.3}, {"rated_current", 0.9}},
        {{"mpn", "CURVE_MATCH"}, {"impedance_100mhz", 150.0}, {"impedance_peak", 950.0},
         {"impedance_peak_freq", 1000e6}, {"dcr", 0.3}, {"rated_current", 0.9}},
    });
    auto o = order("chipBead", original, cands);
    REQUIRE(o.size() == 3);
    CHECK(o[0] == "CURVE_MATCH");
    CHECK(o.back() == "NO_CURVE");
}

TEST_CASE("golden: crystal load-capacitance ordering", "[crossref][golden]") {
    // 18 pF board network. A matching-CL crystal is the drop-in; a mismatched CL
    // runs off frequency and is rejected however good it looks otherwise.
    json original = {{"mpn", "O"}, {"technology", "quartzCrystal"}, {"subtype", "crystal"},
                     {"frequency", 16e6}, {"load_capacitance", 18e-12}};
    json cands = json::array({
        {{"mpn", "WRONG_CL"}, {"technology", "quartzCrystal"}, {"subtype", "crystal"},
         {"frequency", 16e6}, {"load_capacitance", 12e-12}},
        {{"mpn", "MATCH_CL"}, {"technology", "quartzCrystal"}, {"subtype", "crystal"},
         {"frequency", 16e6}, {"load_capacitance", 18e-12}},
    });
    auto o = order("timeBase", original, cands);
    REQUIRE(o.size() == 2);
    CHECK(o[0] == "MATCH_CL");
    CHECK(grade_of("timeBase", original, cands, "WRONG_CL") == "no_substitute");
}

TEST_CASE("golden: a magnetic with no mechanical dimensions is excluded from cross-ref",
          "[crossref][golden]") {
    // Regression for a real report: crossing Würth 744777004 (7.3 x 7.3 x 4.3 mm,
    // explicit mechanical drawing) surfaced 7847709047 as a "drop_in" even though
    // that WE-PD part carries NO mechanical block — only an ambiguous case code
    // ("1210"), which the resolver was reading as a tiny EIA-1210 chip (3.2 x 2.5)
    // and thus "fitting". A magnetic whose footprint we cannot verify must NOT be
    // offered as a substitute: it is rejected (no_substitute) and flagged with
    // `missing_dimensions` so the gap can be backfilled, while an electrically
    // identical part WITH matching dimensions stays a clean drop-in.
    json original = {{"mpn", "744777004"}, {"value_si", 4.7e-6}, {"saturation_current", 9.0},
                     {"rated_current", 7.0}, {"dcr", 0.026},     {"length_m", 0.0073},
                     {"width_m", 0.0073},    {"height_m", 0.0043}};
    json cands = json::array({
        // Same electricals, but only a bare (ambiguous) case code — no L/W/H.
        {{"mpn", "7847709047"}, {"value_si", 4.7e-6}, {"saturation_current", 9.0},
         {"rated_current", 7.0}, {"dcr", 0.026}, {"case_code", "1210"}},
        // Same electricals WITH an explicit matching footprint — the honest drop-in.
        {{"mpn", "SAME_SIZE"}, {"value_si", 4.7e-6}, {"saturation_current", 9.0},
         {"rated_current", 7.0}, {"dcr", 0.026}, {"length_m", 0.0073}, {"width_m", 0.0073},
         {"height_m", 0.0043}},
    });
    // The dimensioned equal is a drop-in and ranks first; the un-dimensioned part
    // is excluded (no_substitute) and marked as a data gap, so it sinks.
    auto o = order("magnetic", original, cands);
    REQUIRE(o.size() == 2);
    CHECK(o[0] == "SAME_SIZE");
    CHECK(o[1] == "7847709047");  // excluded -> sinks to the bottom
    CHECK(grade_of("magnetic", original, cands, "SAME_SIZE") == "drop_in");
    CHECK(grade_of("magnetic", original, cands, "7847709047") == "no_substitute");

    // The excluded part is flagged as an incomplete record for backfill.
    Options opt;
    opt.max_results = 50;
    auto r = cross_reference("magnetic", original, cands, opt);
    bool found = false;
    for (const auto& c : r["candidates"])
        if (c.value("mpn", std::string()) == "7847709047") {
            found = true;
            CHECK(c.value("status", std::string()) == "no_substitute");
            CHECK(c.value("missing_dimensions", false) == true);
        }
    CHECK(found);
}
