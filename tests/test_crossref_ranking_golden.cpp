// Golden RANKING corpus for cross_reference() — the one verb that had no pinned
// reference output, so a refactor could silently reorder results and nothing
// would fail. Each case fixes a full multi-candidate ordering that is
// hand-verified as physically correct (not merely "what the engine does now"),
// so this is a correctness pin, not a characterisation snapshot.
//
// If a case here changes, the reviewer must confirm the NEW order is more
// correct than the old one and update the expectation with a reason — never
// blindly re-pin to green.
#include <algorithm>
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

TEST_CASE("golden: keeping the case is the true drop-in and ranks above a smaller body",
          "[crossref][golden]") {
    // For a magnetic, only a substitute that KEEPS the footprint (same body size)
    // is a true drop-in — it must rank first. An electrically identical part in a
    // materially smaller body has a different land pattern (pads won't match), so
    // it is a minor_review and ranks below the case-kept part, however tempting its
    // smaller size is.
    json original = {{"mpn", "O"}, {"value_si", 4.7e-6}, {"saturation_current", 9.0},
                     {"rated_current", 7.0}, {"dcr", 0.026},  {"length_m", 0.0073},
                     {"width_m", 0.0073},    {"height_m", 0.0043}};
    json cands = json::array({
        // Materially smaller body — fits inside the area but changes the pads.
        {{"mpn", "SMALLER"}, {"value_si", 4.7e-6}, {"saturation_current", 9.0},
         {"rated_current", 7.0}, {"dcr", 0.026}, {"length_m", 0.0044}, {"width_m", 0.0044},
         {"height_m", 0.003}},
        // Same body — the case kept, the honest drop-in.
        {{"mpn", "CASE_KEPT"}, {"value_si", 4.7e-6}, {"saturation_current", 9.0},
         {"rated_current", 7.0}, {"dcr", 0.026}, {"length_m", 0.0073}, {"width_m", 0.0073},
         {"height_m", 0.0043}},
    });
    auto o = order("magnetic", original, cands);
    REQUIRE(o.size() == 2);
    CHECK(o[0] == "CASE_KEPT");  // same footprint ranks first
    CHECK(o[1] == "SMALLER");
    CHECK(grade_of("magnetic", original, cands, "CASE_KEPT") == "drop_in");
    CHECK(grade_of("magnetic", original, cands, "SMALLER") == "minor_review");
}

TEST_CASE("golden: a different IC package is a substitute, not a drop-in",
          "[crossref][golden]") {
    // Catalogue audit finding: MOSFET/diode "drop_in"s across a changed package
    // (e.g. a diode DO-214AB -> SMA) — different pinout/pads, so not a drop-in even
    // when the exact body dimensions are not on record. A SAME-package part is the
    // true drop-in; a different package is a minor_review to re-lay-out.
    json original = {{"mpn", "MBRS340"}, {"vrrm", 40.0}, {"if_avg", 3.0},
                     {"technology", "schottky"}, {"case_code", "DO-214AB"}};
    json cands = json::array({
        // Different package — electrically strong, but the land pattern changes.
        {{"mpn", "B340A"}, {"vrrm", 40.0}, {"if_avg", 3.0}, {"technology", "schottky"},
         {"case_code", "SMA"}},
        // Same package (spelled with a dash) — the genuine drop-in.
        {{"mpn", "SAME_PKG"}, {"vrrm", 40.0}, {"if_avg", 3.0}, {"technology", "schottky"},
         {"case_code", "DO214AB"}},
    });
    auto o = order("diode", original, cands);
    REQUIRE(o.size() == 2);
    CHECK(o[0] == "SAME_PKG");  // case kept ranks first
    CHECK(grade_of("diode", original, cands, "SAME_PKG") == "drop_in");
    CHECK(grade_of("diode", original, cands, "B340A") == "minor_review");
}

TEST_CASE("golden: an established land-pattern change never outranks an unverified fit",
          "[crossref][golden]") {
    // Audit finding on Panasonic ERA2AEC2152X (0402, 21.5 kOhm, 0.25 %, 63 mW):
    // the Vishay SMM0102 MELF parts headed the list at penalty 0.726 while the
    // exact-value YAGEO 0402 thin films sat at 2.057, because a footprint the tool
    // itself annotates as "a substitute to re-lay-out, not a drop-in" cost one
    // categorical warn (0.5) and a footprint nobody could verify cost the
    // dimensional kUnknownDimPenalty (2.0) — two unrelated scales. The engineer
    // reading top-down was handed the re-layout part first (ABT #498).
    //
    // A known "no" must rank below a "cannot tell", so the exact-value part whose
    // record merely omits a width now leads, whatever its footprint verdict.
    json original = {{"mpn", "ERA2AEC2152X"}, {"value_si", 21500.0}, {"power_rating", 0.063},
                     {"tolerance_pct", 0.25},  {"case_code", "0402"},  {"length_m", 0.001},
                     {"width_m", 0.0005}};
    json cands = json::array({
        // 0102 MELF: a different land pattern AND +2.33 % on a 0.25 % part.
        {{"mpn", "SMM01020E2202BB300"}, {"value_si", 22000.0}, {"power_rating", 0.2},
         {"tolerance_pct", 0.1}, {"case_code", "0102"}},
        // Exact value, exact power, same stated length — but no width and no case
        // code on record, so its fit cannot be verified.
        {{"mpn", "AT0402BRD0721K5L"}, {"value_si", 21500.0}, {"power_rating", 0.063},
         {"tolerance_pct", 0.1}, {"length_m", 0.001}},
    });
    auto o = order("resistor", original, cands);
    REQUIRE(o.size() == 2);
    CHECK(o[0] == "AT0402BRD0721K5L");   // unverified fit, exact value
    CHECK(o[1] == "SMM01020E2202BB300"); // established re-layout, value off
}

TEST_CASE("golden: the footprint ladder is monotone — fits < unverified < established miss",
          "[crossref][golden]") {
    // The ordering rule behind ABT #498, stated directly: for one otherwise
    // identical part, a VERIFIED fit beats a footprint nobody could verify, which
    // beats every footprint the ranker has established to be a different land
    // pattern (another case code, a body that no longer covers the pads, one that
    // overhangs them). Each of those was scored on its own scale before, so the
    // rungs could and did cross.
    json original = {{"mpn", "O"},        {"value_si", 21500.0}, {"power_rating", 0.063},
                     {"tolerance_pct", 1.0}, {"case_code", "0402"}, {"length_m", 0.001},
                     {"width_m", 0.0005}};
    json cands = json::array({
        {{"mpn", "OVERHANGS"}, {"value_si", 21500.0}, {"power_rating", 0.063},
         {"tolerance_pct", 1.0}, {"length_m", 0.0032}, {"width_m", 0.0016}},
        {{"mpn", "DIFFERENT_CASE"}, {"value_si", 21500.0}, {"power_rating", 0.063},
         {"tolerance_pct", 1.0}, {"case_code", "0603"}},
        {{"mpn", "UNVERIFIED"}, {"value_si", 21500.0}, {"power_rating", 0.063},
         {"tolerance_pct", 1.0}},
        {{"mpn", "SMALLER_BODY"}, {"value_si", 21500.0}, {"power_rating", 0.063},
         {"tolerance_pct", 1.0}, {"length_m", 0.0006}, {"width_m", 0.0003}},
        {{"mpn", "FITS"}, {"value_si", 21500.0}, {"power_rating", 0.063},
         {"tolerance_pct", 1.0}, {"case_code", "0402"}, {"length_m", 0.001},
         {"width_m", 0.0005}},
    });
    auto o = order("resistor", original, cands);
    REQUIRE(o.size() == 5);
    CHECK(o[0] == "FITS");
    CHECK(o[1] == "UNVERIFIED");
    // The three established mismatches follow; a body that grossly overhangs the
    // pads stays the worst of them (the continuous term still separates them).
    CHECK(std::find(o.begin(), o.end(), "SMALLER_BODY") > o.begin() + 1);
    CHECK(std::find(o.begin(), o.end(), "DIFFERENT_CASE") > o.begin() + 1);
    CHECK(o[4] == "OVERHANGS");
}

TEST_CASE("golden: land and height are two verdicts, not one — ABT #513",
          "[crossref][golden]") {
    // Abracon ASPI-104S-100N-T (10.3 x 10.4 x 4.0 mm) crossed against four Würth
    // inductors returned ONE footprint class and ONE note — "one_size_larger,
    // about one case size larger — verify board fit" — for four geometrically
    // different parts, because height rode in the same scalar as the two land
    // axes. 74439346100 is 6.36 x 6.56 mm: 39% of the original's land and 50%
    // taller, i.e. the OPPOSITE of one case size larger on the axis that governs
    // the pads, while the real risk (the height) went unmentioned.
    //
    // Each part must now read as the geometry it has, and a part needing a new
    // land pattern AND more headroom must say both.
    json original = {{"mpn", "ASPI-104S-100N-T"}, {"value_si", 1e-5},
                     {"saturation_current", 4.4}, {"rated_current", 4.4},
                     {"dcr", 0.035},              {"length_m", 0.0103},
                     {"width_m", 0.0104},         {"height_m", 0.004}};
    json cands = json::array({
        // 39% of the land, +50% height — a new land pattern plus a clearance check.
        {{"mpn", "74439346100"}, {"value_si", 1e-5}, {"saturation_current", 5.05},
         {"rated_current", 5.0}, {"dcr", 0.02915}, {"length_m", 0.00636},
         {"width_m", 0.00656}, {"height_m", 0.006}},
        // Essentially the original's land, 1 mm taller — the pads keep, the height
        // does not.
        {{"mpn", "7847714100"}, {"value_si", 1e-5}, {"saturation_current", 4.4},
         {"rated_current", 4.4}, {"dcr", 0.035}, {"length_m", 0.01}, {"width_m", 0.01},
         {"height_m", 0.005}},
        // Larger on all three axes — the only one "one case size larger" describes.
        {{"mpn", "784771100"}, {"value_si", 1e-5}, {"saturation_current", 4.4},
         {"rated_current", 4.4}, {"dcr", 0.035}, {"length_m", 0.012}, {"width_m", 0.012},
         {"height_m", 0.006}},
        // The case kept in every axis — the true drop-in.
        {{"mpn", "CASE_KEPT"}, {"value_si", 1e-5}, {"saturation_current", 4.4},
         {"rated_current", 4.4}, {"dcr", 0.035}, {"length_m", 0.0103}, {"width_m", 0.0104},
         {"height_m", 0.004}},
    });
    Options opt;
    opt.max_results = 50;
    auto r = cross_reference("magnetic", original, cands, opt);
    auto by_mpn = [&](const std::string& m) {
        for (const auto& c : r["candidates"])
            if (c.value("mpn", std::string()) == m) return c;
        return json(nullptr);
    };
    auto notes_of = [&](const json& c) {
        std::string all;
        for (const auto& n : c.value("notes", json::array())) all += n.get<std::string>() + " | ";
        return all;
    };

    const json small_tall = by_mpn("74439346100");
    REQUIRE(!small_tall.is_null());
    CHECK(small_tall["footprint"] == "smaller");       // NOT one_size_larger
    CHECK(small_tall["height_fit"] == "taller");
    CHECK(notes_of(small_tall).find("smaller body") != std::string::npos);
    CHECK(notes_of(small_tall).find("new land pattern") != std::string::npos);
    // The +50% height is named with its numbers, the risk the old note omitted.
    CHECK(notes_of(small_tall).find("4 -> 6 mm (+50 %)") != std::string::npos);

    const json same_land = by_mpn("7847714100");
    CHECK(same_land["footprint"] == "fits");           // the pads keep
    CHECK(same_land["height_fit"] == "taller");
    CHECK(notes_of(same_land).find("4 -> 5 mm (+25 %)") != std::string::npos);
    CHECK(notes_of(same_land).find("one case size larger") == std::string::npos);

    const json bigger = by_mpn("784771100");
    CHECK(bigger["footprint"] == "one_size_larger");   // the one it really describes
    CHECK(bigger["height_fit"] == "taller");

    // Four parts, four distinct messages — the collapse is what the ticket reported.
    CHECK(notes_of(small_tall) != notes_of(same_land));
    CHECK(notes_of(small_tall) != notes_of(bigger));
    CHECK(notes_of(same_land) != notes_of(bigger));

    // The part that keeps both the land AND the height is the only drop-in, and
    // leads: every other candidate owes the board some work.
    auto o = order("magnetic", original, cands);
    REQUIRE(o.size() == 4);
    CHECK(o[0] == "CASE_KEPT");
    CHECK(grade_of("magnetic", original, cands, "CASE_KEPT") == "drop_in");
    CHECK(grade_of("magnetic", original, cands, "74439346100") == "minor_review");
}
