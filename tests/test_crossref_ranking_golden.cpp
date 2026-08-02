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

TEST_CASE("golden: one missing axis on the original does not blank the footprint — ABT #516",
          "[crossref][golden]") {
    // KEMET C0805C103M5GECTU (2.0 mm long, no width on record) crossed against
    // Murata returned GRM3195C1H153JA01 and GCM3195C1H153JA16 — 3.2 x 1.6 mm 1206
    // bodies — at "footprint": null, penalty 3, direction "equivalent" and no note:
    // bit-for-bit what the true 2012M/0805 candidates scored, so nothing said the
    // land pattern differs, and the two 1206 parts ranked above two of the 0805
    // ones. dims_of() required BOTH land axes and dropped the box when either was
    // missing, so the axis the record DOES state — the one that settles this — was
    // never compared.
    json original = {{"mpn", "C0805C103M5GECTU"}, {"value_si", 1e-8},   {"voltage", 50.0},
                     {"technology", "ceramic-class-1"}, {"length_m", 0.002}};
    json cands = json::array({
        {{"mpn", "GRM3195C1H153JA01"}, {"value_si", 1e-8}, {"voltage", 50.0},
         {"technology", "ceramic-class-1"}, {"case_code", "3216M/1206"}, {"length_m", 0.0032},
         {"width_m", 0.0016}},
        {{"mpn", "GRM2195C1H153JA01"}, {"value_si", 1e-8}, {"voltage", 50.0},
         {"technology", "ceramic-class-1"}, {"case_code", "2012M/0805"}, {"length_m", 0.002},
         {"width_m", 0.00125}},
    });
    Options opt;
    opt.max_results = 50;
    auto r = cross_reference("capacitor", original, cands, opt);
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

    const json big = by_mpn("GRM3195C1H153JA01");
    const json same = by_mpn("GRM2195C1H153JA01");
    REQUIRE(!big.is_null());
    REQUIRE(!same.is_null());
    // The stated axis settles it: a 3.2 mm body is one case size up from a 2 mm land.
    CHECK(big["footprint"] == "one_size_larger");
    CHECK(notes_of(big).find("one case size larger") != std::string::npos);
    CHECK(notes_of(big).find("3.2 x 1.6 mm vs 2 mm long") != std::string::npos);
    // The 0805 part is NOT declared a fit — the original's width is still unstated —
    // but "cannot tell" outranks "established different land pattern", and both now
    // carry a footprint verdict where the pair used to be indistinguishable.
    CHECK(same["footprint"] == "unknown");
    CHECK(same["penalty"].get<double>() < big["penalty"].get<double>());
    auto o = order("capacitor", original, cands);
    REQUIRE(o.size() == 2);
    CHECK(o[0] == "GRM2195C1H153JA01");
    CHECK(o[1] == "GRM3195C1H153JA01");
}

TEST_CASE("golden: an original with no land pattern still reports the package axis — ABT #545",
          "[crossref][golden]") {
    // Pulse BSCH0010050533NJCP (a 1005-metric, 1.0 x 0.5 mm RF chip inductor whose
    // size is stated only in the free-text description, so the shard row carries no
    // case code and no drawing) crossed against Murata returned SIX candidates that
    // were bit-for-bit identical in the verdict table: grade "drop_in", penalty 0,
    // footprint null, notes null, and no footprint entry in params at all. Two of
    // them are 01005 (0402 metric) chips — 0.4 x 0.2 mm, a body shorter than the
    // pad-to-pad gap of the original's land — and two are 0302 (0804 metric). Every
    // branch of the footprint block needed a footprint for the ORIGINAL, so with none
    // the axis simply vanished and "drop_in", the grade that asserts the part solders
    // into the existing land pattern, was awarded with no mechanical information about
    // the original whatsoever.
    json original = {{"mpn", "BSCH0010050533NJCP"}, {"value_si", 3.3e-8}, {"dcr", 0.58}};
    json cands = json::array({
        // Same size as the original — but nothing on record says so.
        {{"mpn", "LQG15HS33NH02D"}, {"value_si", 3.3e-8}, {"dcr", 0.58},
         {"case_code", "0402 (1005 Metric)"}, {"length_m", 0.001}, {"width_m", 0.0005}},
        // Two case sizes down: 0.4 x 0.2 mm, stated as a case code only.
        {{"mpn", "LQP02HQ30NHZ2E"}, {"value_si", 3.0e-8}, {"dcr", 0.58},
         {"case_code", "01005 (0402 Metric)"}},
        // Neither side states a package: the gap is the catalogue's, not the part's.
        {{"mpn", "NO_PACKAGE"}, {"value_si", 3.3e-8}, {"dcr", 0.58}},
    });
    Options opt;
    opt.max_results = 50;
    auto r = cross_reference("magnetic", original, cands, opt);
    auto by_mpn = [&](const std::string& m) {
        for (const auto& c : r["candidates"])
            if (c.value("mpn", std::string()) == m) return c;
        return json(nullptr);
    };
    auto verdict_of = [](const json& c, const char* name) {
        for (const auto& p : c["params"])
            if (p["name"] == name) return p["verdict"].get<std::string>();
        return std::string("<absent>");
    };
    auto notes_of = [&](const json& c) {
        std::string all;
        for (const auto& n : c.value("notes", json::array())) all += n.get<std::string>() + " | ";
        return all;
    };

    // Every candidate carries the axis, and none of them is a drop-in: the claim
    // "fits the original's land pattern" cannot be made about a land nobody stated.
    for (const char* mpn : {"LQG15HS33NH02D", "LQP02HQ30NHZ2E", "NO_PACKAGE"}) {
        const json c = by_mpn(mpn);
        REQUIRE(!c.is_null());
        CHECK(verdict_of(c, "footprint") == UNVERIFIED);
        CHECK(c["footprint"] == "unknown");
        CHECK(c["grade"] != "drop_in");
    }

    // The note names what the substitute IS, so the two case sizes are readable even
    // though the ranker cannot compare them.
    CHECK(notes_of(by_mpn("LQP02HQ30NHZ2E")).find("01005 (0402 Metric)") != std::string::npos);
    CHECK(notes_of(by_mpn("LQG15HS33NH02D")).find("1 x 0.5 mm") != std::string::npos);
    // Where NEITHER record states a package there is nothing to name: the UNVERIFIED
    // verdict is the whole story and a sentence blaming the candidate would be wrong.
    CHECK_FALSE(by_mpn("NO_PACKAGE").contains("notes"));

    // The axis is reported, not scored: with no original to compare against, the
    // footprint term is the same for every candidate and must not reorder them.
    CHECK(by_mpn("LQG15HS33NH02D")["penalty"].get<double>() ==
          by_mpn("NO_PACKAGE")["penalty"].get<double>());
}

TEST_CASE("golden: with no drawing on either side, the kept case IS the land pattern — ABT #545",
          "[crossref][golden]") {
    // The other half of the same gap. For the families whose case code is the
    // footprint, two records naming the same package name the same pads — that is
    // the one way the fit can be established with no dimensions on either side, and
    // it is what keeps a genuine same-package substitute a drop-in. Magnetics and
    // chip beads are excluded, exactly as they are from the case-kept gate: a
    // magnetic "1210" is an EIA chip or a molded power inductor, so equal codes there
    // are not an equal land.
    json original = {{"mpn", "MBRS340"}, {"vrrm", 40.0}, {"if_avg", 3.0},
                     {"technology", "schottky"}, {"case_code", "DO-214AB"}};
    json cands = json::array({
        {{"mpn", "SAME_PKG"}, {"vrrm", 40.0}, {"if_avg", 3.0}, {"technology", "schottky"},
         {"case_code", "DO214AB"}},
        {{"mpn", "NO_PKG"}, {"vrrm", 40.0}, {"if_avg", 3.0}, {"technology", "schottky"}},
    });
    Options opt;
    opt.max_results = 50;
    auto r = cross_reference("diode", original, cands, opt);
    auto by_mpn = [&](const std::string& m) {
        for (const auto& c : r["candidates"])
            if (c.value("mpn", std::string()) == m) return c;
        return json(nullptr);
    };
    const json same = by_mpn("SAME_PKG");
    REQUIRE(!same.is_null());
    CHECK(same["footprint"] == "fits");
    CHECK(same["grade"] == "drop_in");
    // The candidate that names no package at all cannot claim the same.
    const json bare = by_mpn("NO_PKG");
    REQUIRE(!bare.is_null());
    CHECK(bare["footprint"] == "unknown");
    CHECK(bare["grade"] == "minor_review");

    // The magnetic exclusion holds: identical codes, still unverified.
    json m_orig = {{"mpn", "O"}, {"value_si", 4.7e-6}, {"case_code", "1210"}};
    json m_cands = json::array({{{"mpn", "SAME_CODE"}, {"value_si", 4.7e-6}, {"case_code", "1210"}}});
    auto rm = cross_reference("magnetic", m_orig, m_cands, opt);
    CHECK(rm["candidates"][0]["footprint"] == "unknown");
    CHECK(rm["candidates"][0]["grade"] != "drop_in");
}

TEST_CASE("golden: the footprint note names what is missing, not 'no dimensions' — ABT #499",
          "[crossref][golden]") {
    // Panasonic ERA2AEC2152X (1.0 x 0.5 mm) returned YAGEO AT0402BRD0721K5L with
    // "mechanical dimensions unavailable for the substitute" printed beside the row
    // that listed lengthM 0.001 and heightM 0.0003 — both faithfully carried from the
    // raw record. One canned sentence covered three different silences, so it
    // overstated the gap on every record that states SOME of its outline and buried
    // the useful fact that the stated length equals the original's. Worse, a record
    // stating only a HEIGHT had its drawing thrown away entirely by dims_of, which
    // also disabled the clearance axis that judges that very height.
    json original = {{"mpn", "ERA2AEC2152X"}, {"value_si", 21500.0},   {"tolerance_pct", 0.25},
                     {"power_rating", 0.063}, {"case_code", "0402"},   {"length_m", 0.001},
                     {"width_m", 0.0005},     {"height_m", 0.00035}};
    json cands = json::array({
        // The ticket's part: length and height stated, width and case code absent.
        {{"mpn", "AT0402BRD0721K5L"}, {"value_si", 21500.0}, {"tolerance_pct", 0.1},
         {"power_rating", 0.063}, {"length_m", 0.001}, {"height_m", 0.0003}},
        // A height and no land at all — 1,167 catalogue rows look like this.
        {{"mpn", "HEIGHT_ONLY"}, {"value_si", 21500.0}, {"tolerance_pct", 0.1},
         {"power_rating", 0.063}, {"height_m", 0.0009}},
        // Nothing on record: the canned sentence is TRUE here and must stay.
        {{"mpn", "NO_DIMS"}, {"value_si", 21500.0}, {"tolerance_pct", 0.1},
         {"power_rating", 0.063}},
    });
    Options opt;
    opt.max_results = 50;
    auto r = cross_reference("resistor", original, cands, opt);
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
    auto verdict_of = [](const json& c, const char* name) {
        for (const auto& p : c["params"])
            if (p["name"] == name) return p["verdict"].get<std::string>();
        return std::string("<absent>");
    };

    // The stated length is compared and quoted; the missing axis is what the note is
    // about. The footprint verdict itself is unchanged — width is genuinely absent,
    // so the fit is still UNVERIFIED.
    const json partial = by_mpn("AT0402BRD0721K5L");
    REQUIRE(!partial.is_null());
    CHECK(partial["footprint"] == "unknown");
    CHECK(verdict_of(partial, "footprint") == UNVERIFIED);
    CHECK(notes_of(partial).find("mechanical dimensions unavailable") == std::string::npos);
    CHECK(notes_of(partial).find("1 mm long, width not stated vs 1 x 0.5 mm") != std::string::npos);

    // A height and no land says nothing about the pads — but it is not "no
    // dimensions", and the clearance axis it DOES settle is now judged.
    const json h_only = by_mpn("HEIGHT_ONLY");
    REQUIRE(!h_only.is_null());
    CHECK(h_only["footprint"] == "unknown");
    CHECK(notes_of(h_only).find("mechanical dimensions unavailable") == std::string::npos);
    CHECK(notes_of(h_only).find("states its height (0.9 mm) but neither land dimension") !=
          std::string::npos);
    CHECK(h_only["height_fit"] == "much_taller");
    CHECK(verdict_of(h_only, "height") == FAIL);

    // ... and a record that really states nothing still gets the sentence that is
    // true of it, so the fix narrows the claim rather than deleting it.
    const json bare = by_mpn("NO_DIMS");
    REQUIRE(!bare.is_null());
    CHECK(bare["footprint"] == "unknown");
    CHECK(notes_of(bare).find("mechanical dimensions unavailable") != std::string::npos);
    CHECK(verdict_of(bare, "height") == "<absent>");
}
