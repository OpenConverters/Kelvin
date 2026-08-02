// Class-equivalence gates: capacitor construction family, dielectric envelope,
// semiconductor process, gate drive, operating temperature, measurement-basis
// awareness, and the industry-style match grade.
//
// Every case here encodes a documented real-world substitution failure, not a
// characterisation of current behaviour.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../src/CrossRef.hpp"
#include "../src/CrossRefDimensions.hpp"
#include "../src/CrossRefClasses.hpp"

using namespace kelvin::crossref;

// ── capacitor construction family ────────────────────────────────────────────

TEST_CASE("capacitor families are classified from the technology string", "[crossref][classes]") {
    CHECK(cap_family("ceramic-class-1") == CapFamily::CeramicClass1);
    CHECK(cap_family("ceramic-class-2") == CapFamily::CeramicClass2);
    CHECK(cap_family("aluminum-electrolytic-wet") == CapFamily::AluminiumWet);
    CHECK(cap_family("aluminum-electrolytic-polymer") == CapFamily::AluminiumPolymer);
    CHECK(cap_family("tantalum-mno2") == CapFamily::TantalumMnO2);
    CHECK(cap_family("tantalum-polymer") == CapFamily::TantalumPolymer);
    CHECK(cap_family("film-polypropylene") == CapFamily::Film);
    // ceramic of unstated class must NOT be guessed into a class
    CHECK(cap_family("ceramic") == CapFamily::Unknown);
    CHECK(cap_family("") == CapFamily::Unknown);
}

TEST_CASE("a ceramic is never a drop-in for a tantalum or an electrolytic",
          "[crossref][classes][rank]") {
    // Same capacitance, same voltage, completely different part.
    json original = {{"mpn", "TANT"}, {"value_si", 1e-5}, {"voltage", 25.0},
                     {"technology", "tantalum-mno2"}};
    json cands = json::array({
        {{"mpn", "CER"}, {"value_si", 1e-5}, {"voltage", 25.0}, {"technology", "ceramic-class-2"}}});
    auto r = cross_reference("capacitor", original, cands, Options{});
    CHECK(r["candidates"][0]["status"] == "no_substitute");
}

TEST_CASE("MnO2 and polymer tantalum are not interchangeable", "[crossref][classes][rank]") {
    // Different failure mode (ignition vs benign) and different derating
    // convention (50% vs 70-80%) — a swap either way moves the part into a role
    // it was not derated for.
    json original = {{"mpn", "MNO2"}, {"value_si", 1e-5}, {"voltage", 25.0},
                     {"technology", "tantalum-mno2"}};
    json cands = json::array({
        {{"mpn", "POLY"}, {"value_si", 1e-5}, {"voltage", 25.0},
         {"technology", "tantalum-polymer"}}});
    auto r = cross_reference("capacitor", original, cands, Options{});
    CHECK(r["candidates"][0]["status"] == "no_substitute");
    CHECK(std::string(r["candidates"][0]["notes"][0]).find("cathode") != std::string::npos);
}

TEST_CASE("same family passes the family gate", "[crossref][classes][rank]") {
    json original = {{"mpn", "A"}, {"value_si", 1e-7}, {"voltage", 50.0},
                     {"technology", "ceramic-class-2"}};
    json cands = json::array({
        {{"mpn", "B"}, {"value_si", 1e-7}, {"voltage", 50.0}, {"technology", "ceramic-class-2"}}});
    auto r = cross_reference("capacitor", original, cands, Options{});
    CHECK(r["candidates"][0]["status"] != "no_substitute");
}

// ── dielectric envelope (EIA RS-198) ─────────────────────────────────────────

TEST_CASE("dielectric codes decode to their published envelope", "[crossref][classes]") {
    auto x7r = dielectric_envelope("X7R");
    REQUIRE(x7r.has_value());
    CHECK(x7r->temp_min_c == -55);
    CHECK(x7r->temp_max_c == 125);
    CHECK(x7r->delta_c_pct == 15.0);
    auto x5r = dielectric_envelope("X5R");
    REQUIRE(x5r.has_value());
    CHECK(x5r->temp_max_c == 85);   // the whole point: X5R stops at 85 degC
    auto c0g = dielectric_envelope("C0G");
    REQUIRE(c0g.has_value());
    CHECK(c0g->class1);
    CHECK(dielectric_envelope("NP0")->class1);   // same material system as C0G
    CHECK_FALSE(dielectric_envelope("WAT").has_value());
}

TEST_CASE("X7R to X5R is caught as a temperature regression", "[crossref][classes]") {
    const std::string reg = dielectric_regression("X7R", "X5R");
    REQUIRE_FALSE(reg.empty());
    CHECK(reg.find("upper temperature") != std::string::npos);
}

TEST_CASE("X7R to X7S is caught as a tolerance regression", "[crossref][classes]") {
    // Same temperature range, but +/-22% instead of +/-15% — invisible to a
    // string comparison of the codes.
    const std::string reg = dielectric_regression("X7R", "X7S");
    REQUIRE_FALSE(reg.empty());
    CHECK(reg.find("capacitance change") != std::string::npos);
}

TEST_CASE("an improving dielectric swap is not flagged", "[crossref][classes]") {
    CHECK(dielectric_regression("X5R", "X7R").empty());   // wider temperature
    CHECK(dielectric_regression("X7S", "X7R").empty());   // tighter tolerance
    CHECK(dielectric_regression("X7R", "C0G").empty());   // class 1 is better
    // class 1 -> class 2 IS a regression (adds bias derating and ageing)
    CHECK_FALSE(dielectric_regression("C0G", "X7R").empty());
    // undecodable either side -> no claim
    CHECK(dielectric_regression("X7R", "").empty());
}

// ── semiconductor process ────────────────────────────────────────────────────

TEST_CASE("Si to SiC or GaN is flagged as a gate-drive change", "[crossref][classes]") {
    CHECK_FALSE(process_conflict("Si", "SiC").empty());
    CHECK_FALSE(process_conflict("Si", "GaN").empty());
    CHECK(process_conflict("Si", "Si").empty());
    CHECK(process_conflict("", "SiC").empty());  // unknown -> no claim
}

TEST_CASE("a SiC part offered for a Si original is not silently recommended",
          "[crossref][classes][rank]") {
    json original = {{"mpn", "SI"}, {"vds", 650.0}, {"id", 20.0}, {"rds_on", 0.1},
                     {"technology", "Si"}};
    json cands = json::array({
        {{"mpn", "SIC"}, {"vds", 650.0}, {"id", 20.0}, {"rds_on", 0.05}, {"technology", "SiC"}}});
    auto r = cross_reference("mosfet", original, cands, Options{});
    CHECK(r["candidates"][0]["status"] == "partial");
}

// ── MOSFET gate drive ────────────────────────────────────────────────────────

TEST_CASE("a standard-level FET for a logic-level original is flagged",
          "[crossref][classes][rank]") {
    // The classic failure: works on the bench at 10 V, never fully enhances
    // from the 3.3 V controller already on the board.
    json original = {{"mpn", "LOGIC"}, {"vds", 60.0}, {"id", 10.0}, {"rds_on", 0.01},
                     {"vgs_threshold_max", 1.5}};
    json cands = json::array({
        {{"mpn", "STD"}, {"vds", 60.0}, {"id", 10.0}, {"rds_on", 0.01},
         {"vgs_threshold_max", 4.0}}});
    auto r = cross_reference("mosfet", original, cands, Options{});
    CHECK(r["candidates"][0]["status"] == "partial");
    CHECK(std::string(r["candidates"][0]["notes"][0]).find("gate threshold") != std::string::npos);
}

TEST_CASE("Rds(on) quoted at a higher Vgs is flagged as not comparable",
          "[crossref][classes][rank]") {
    json original = {{"mpn", "A"}, {"vds", 30.0}, {"id", 10.0}, {"rds_on", 0.01},
                     {"rds_on_vgs", 4.5}};
    json cands = json::array({
        {{"mpn", "B"}, {"vds", 30.0}, {"id", 10.0}, {"rds_on", 0.01}, {"rds_on_vgs", 10.0}}});
    auto r = cross_reference("mosfet", original, cands, Options{});
    CHECK(r["candidates"][0]["status"] == "partial");
}

TEST_CASE("a lower Vgs(max) rating is surfaced", "[crossref][classes][rank]") {
    json original = {{"mpn", "A"}, {"vds", 30.0}, {"id", 10.0}, {"rds_on", 0.01}, {"vgs_max", 20.0}};
    json cands = json::array({
        {{"mpn", "B"}, {"vds", 30.0}, {"id", 10.0}, {"rds_on", 0.01}, {"vgs_max", 12.0}}});
    auto r = cross_reference("mosfet", original, cands, Options{});
    CHECK(r["candidates"][0]["status"] == "partial");
}

// ── measurement basis ────────────────────────────────────────────────────────

TEST_CASE("ESR quoted at different frequencies is not compared as a bare number",
          "[crossref][classes][rank]") {
    // A 120 Hz figure and a 100 kHz figure differ severalfold on the same part.
    json original = {{"mpn", "A"}, {"value_si", 1e-4}, {"voltage", 25.0}, {"esr", 0.5},
                     {"esr_frequency", 120.0}, {"technology", "aluminum-electrolytic-wet"}};
    json cands = json::array({
        {{"mpn", "B"}, {"value_si", 1e-4}, {"voltage", 25.0}, {"esr", 0.03},
         {"esr_frequency", 100000.0}, {"technology", "aluminum-electrolytic-wet"}}});
    auto r = cross_reference("capacitor", original, cands, Options{});
    const json& c = r["candidates"][0];
    bool flagged = false;
    for (const auto& p : c["params"])
        if (p["name"] == "esr_basis") flagged = true;
    CHECK(flagged);
}

TEST_CASE("operating temperature range must be covered", "[crossref][classes][rank]") {
    json original = {{"mpn", "WIDE"}, {"value_si", 1e-7}, {"voltage", 50.0},
                     {"technology", "ceramic-class-2"}, {"temp_min_C", -55.0}};
    json cands = json::array({
        {{"mpn", "NARROW"}, {"value_si", 1e-7}, {"voltage", 50.0},
         {"technology", "ceramic-class-2"}, {"temp_min_C", -20.0}}});
    auto r = cross_reference("capacitor", original, cands, Options{});
    CHECK(r["candidates"][0]["status"] == "partial");
}

// ── grade + direction ────────────────────────────────────────────────────────

TEST_CASE("a clean same-size match grades drop_in", "[crossref][classes][rank]") {
    json original = {{"mpn", "A"}, {"value_si", 1e-7}, {"voltage", 50.0}, {"package", "0603"},
                     {"technology", "ceramic-class-2"}};
    json cands = json::array({
        {{"mpn", "B"}, {"value_si", 1e-7}, {"voltage", 50.0}, {"package", "0603"},
         {"technology", "ceramic-class-2"}}});
    auto r = cross_reference("capacitor", original, cands, Options{});
    CHECK(r["candidates"][0]["grade"] == "drop_in");
}

TEST_CASE("an oversize part grades redesign, not drop_in", "[crossref][classes][rank]") {
    json original = {{"mpn", "A"}, {"value_si", 1e-7}, {"voltage", 50.0}, {"package", "0402"},
                     {"technology", "ceramic-class-2"}};
    json cands = json::array({
        {{"mpn", "BIG"}, {"value_si", 1e-7}, {"voltage", 50.0}, {"package", "1206"},
         {"technology", "ceramic-class-2"}}});
    auto r = cross_reference("capacitor", original, cands, Options{});
    CHECK(r["candidates"][0]["grade"] == "redesign");
}

TEST_CASE("direction reports upgrade, downgrade and mixed honestly",
          "[crossref][classes][rank]") {
    json original = {{"mpn", "O"}, {"vds", 60.0}, {"id", 10.0}, {"rds_on", 0.010}};
    // strictly better: more voltage headroom, more current, lower Rds(on)
    json up = json::array({{{"mpn", "UP"}, {"vds", 100.0}, {"id", 20.0}, {"rds_on", 0.005}}});
    CHECK(cross_reference("mosfet", original, up, Options{})["candidates"][0]["direction"] ==
          "upgrade");
    // better on current, worse on Rds(on)
    json mix = json::array({{{"mpn", "MIX"}, {"vds", 60.0}, {"id", 20.0}, {"rds_on", 0.030}}});
    CHECK(cross_reference("mosfet", original, mix, Options{})["candidates"][0]["direction"] ==
          "mixed");
    // nothing clearly leads
    json same = json::array({{{"mpn", "SAME"}, {"vds", 60.0}, {"id", 10.0}, {"rds_on", 0.010}}});
    CHECK(cross_reference("mosfet", original, same, Options{})["candidates"][0]["direction"] ==
          "equivalent");
}

// ── case-code unit ambiguity ─────────────────────────────────────────────────
// Imperial 0603 (1.6 x 0.8 mm) and metric 0603 (0.6 x 0.3 mm) are a 4x area
// difference. These pin the disambiguation rules against the forms the
// catalogue actually uses, so a future table edit cannot silently introduce the
// collision.
TEST_CASE("imperial and metric case codes cannot collide", "[crossref][dims][classes]") {
    // The metric table deliberately holds only codes that are NOT also valid
    // imperial codes, so a bare 4-digit code has exactly one reading.
    auto imperial_0603 = resolve_dimensions("0603", "capacitor");
    REQUIRE(imperial_0603.has_value());
    CHECK(*imperial_0603->length == Catch::Approx(1.60e-3));  // imperial, per distributor convention

    // "1608" is unambiguously metric — and resolves to the same physical size as
    // imperial 0603, which is the point: they are the same part.
    auto metric_1608 = resolve_dimensions("1608", "capacitor");
    REQUIRE(metric_1608.has_value());
    CHECK(*metric_1608->length == Catch::Approx(1.60e-3));
    CHECK(*metric_1608->width == Catch::Approx(0.80e-3));

    // The catalogue's explicit dual form must resolve, not fall through.
    auto dual = resolve_dimensions("1608M/0603", "capacitor");
    REQUIRE(dual.has_value());
    CHECK(*dual->length == Catch::Approx(1.60e-3));
    CHECK(*dual->width == Catch::Approx(0.80e-3));

    // A real electrolytic can code from the catalogue (diameter x height).
    auto can = resolve_dimensions("10x16", "capacitor");
    REQUIRE(can.has_value());
    CHECK(*can->length == Catch::Approx(10e-3));
    REQUIRE(can->height.has_value());
    CHECK(*can->height == Catch::Approx(16e-3));
}

// ── crystal load capacitance ─────────────────────────────────────────────────

TEST_CASE("crystal frequency pull is computed from the load mismatch", "[crossref][classes]") {
    // 18 pF board network, 12 pF crystal: tens of ppm, several times a typical
    // +/-20 ppm budget.
    auto ppm = crystal_pull_ppm(18e-12, 12e-12);
    REQUIRE(ppm.has_value());
    CHECK(std::abs(*ppm) > 20.0);
    // matching load -> no pull
    CHECK(std::abs(*crystal_pull_ppm(18e-12, 18e-12)) < 1e-9);
    // sign flips with the direction of the mismatch
    CHECK((*crystal_pull_ppm(12e-12, 18e-12)) * (*crystal_pull_ppm(18e-12, 12e-12)) < 0);
    CHECK_FALSE(crystal_pull_ppm(std::nullopt, 12e-12).has_value());
}

TEST_CASE("oscillators are not subject to the load-capacitance gate", "[crossref][classes]") {
    CHECK(is_passive_resonator("quartzCrystal", ""));
    CHECK(is_passive_resonator("", "crystal"));
    CHECK_FALSE(is_passive_resonator("crystalOscillator", "oscillator"));
    CHECK_FALSE(is_passive_resonator("tcxo", ""));
    CHECK_FALSE(is_passive_resonator("mems", "oscillator"));
    CHECK_FALSE(is_passive_resonator("ocxo", "oscillator"));

    // THE real-data shape: in TAS every timing part sits under the
    // `timeBase.oscillator` container, so a plain quartz crystal carries
    // device_type "oscillator". Technology must win, or the gate silently
    // disables itself on every crystal in the catalogue (it did).
    CHECK(is_passive_resonator("quartzCrystal", "oscillator"));
}

TEST_CASE("a crystal for a different load capacitance is rejected", "[crossref][classes][rank]") {
    json original = {{"mpn", "X18"}, {"technology", "quartzCrystal"}, {"device_type", "crystal"},
                     {"subtype", "crystal"}, {"frequency", 16e6}, {"load_capacitance", 18e-12}};
    json cands = json::array({
        {{"mpn", "X12"}, {"technology", "quartzCrystal"}, {"device_type", "crystal"},
         {"subtype", "crystal"}, {"frequency", 16e6}, {"load_capacitance", 12e-12}},
        {{"mpn", "X18B"}, {"technology", "quartzCrystal"}, {"device_type", "crystal"},
         {"subtype", "crystal"}, {"frequency", 16e6}, {"load_capacitance", 18e-12}}});
    auto r = cross_reference("timeBase", original, cands, Options{});
    // the matching-CL part wins; the mismatched one is not a substitute
    CHECK(r["candidates"][0]["mpn"] == "X18B");
    auto bad = std::find_if(r["candidates"].begin(), r["candidates"].end(),
                            [](const json& c) { return c["mpn"] == "X12"; });
    REQUIRE(bad != r["candidates"].end());
    CHECK((*bad)["status"] == "no_substitute");
    CHECK(std::string((*bad)["notes"][0]).find("ppm off frequency") != std::string::npos);
}

TEST_CASE("crystal mode and oscillator output type never cross", "[crossref][classes][rank]") {
    // Fundamental vs 3rd overtone: an overtone circuit's tank is inductive at
    // the fundamental, so the loop cannot close — it will not start, or runs
    // near a third of the marked frequency.
    json xtal = {{"mpn", "F"}, {"technology", "quartzCrystal"}, {"subtype", "crystal"},
                 {"frequency", 27e6}, {"load_capacitance", 18e-12}, {"mode", "fundamental"}};
    json overtone = json::array({
        {{"mpn", "OT"}, {"technology", "quartzCrystal"}, {"subtype", "crystal"},
         {"frequency", 27e6}, {"load_capacitance", 18e-12}, {"mode", "thirdOvertone"}}});
    CHECK(cross_reference("timeBase", xtal, overtone, Options{})["candidates"][0]["status"] ==
          "no_substitute");

    // LVDS and HCSL are different termination networks, not a parameter.
    json xo = {{"mpn", "A"}, {"technology", "crystalOscillator"}, {"subtype", "oscillator"},
               {"frequency", 25e6}, {"output_type", "LVDS"}};
    json other = json::array({
        {{"mpn", "B"}, {"technology", "crystalOscillator"}, {"subtype", "oscillator"},
         {"frequency", 25e6}, {"output_type", "HCSL"}}});
    CHECK(cross_reference("timeBase", xo, other, Options{})["candidates"][0]["status"] ==
          "no_substitute");

    // Same output type still crosses.
    json same = json::array({
        {{"mpn", "C"}, {"technology", "crystalOscillator"}, {"subtype", "oscillator"},
         {"frequency", 25e6}, {"output_type", "LVDS"}}});
    CHECK(cross_reference("timeBase", xo, same, Options{})["candidates"][0]["status"] !=
          "no_substitute");
}

// ── chip bead impedance curve ────────────────────────────────────────────────
// A bead's published spec is |Z| at 100 MHz. TDK's own engineer: that spot value
// "is irrelevant and misleading" alone — his two 120 Ohm beads peak at ~150 Ohm
// @400 MHz and ~700 Ohm @700 MHz respectively, and swapping one for the other
// turned 474 mV of undershoot into 750 mV, worse than fitting no bead at all.
TEST_CASE("beads with equal Z@100MHz but different curves are separated",
          "[crossref][classes][rank]") {
    json original = {{"mpn", "B120"}, {"impedance_100mhz", 120.0},
                     {"impedance_peak", 700.0}, {"impedance_peak_freq", 700e6},
                     {"dcr", 0.1}, {"rated_current", 2.0}};
    json cands = json::array({
        // same headline impedance, same DCR, same current — different curve
        {{"mpn", "SAME_CURVE"}, {"impedance_100mhz", 120.0}, {"impedance_peak", 690.0},
         {"impedance_peak_freq", 690e6}, {"dcr", 0.1}, {"rated_current", 2.0}},
        {{"mpn", "WRONG_BAND"}, {"impedance_100mhz", 120.0}, {"impedance_peak", 150.0},
         {"impedance_peak_freq", 400e6}, {"dcr", 0.1}, {"rated_current", 2.0}}});
    auto r = cross_reference("chipBead", original, cands, Options{});
    // the curve-matched part must win
    CHECK(r["candidates"][0]["mpn"] == "SAME_CURVE");
    auto wrong = std::find_if(r["candidates"].begin(), r["candidates"].end(),
                              [](const json& c) { return c["mpn"] == "WRONG_BAND"; });
    REQUIRE(wrong != r["candidates"].end());
    // it is offered, but never as a clean drop-in — it peaks in a different band
    CHECK((*wrong)["grade"] != "drop_in");
}

TEST_CASE("a bead with no impedance data never outranks a curve-matched one",
          "[crossref][classes][rank]") {
    // Impedance is what a bead IS. A candidate carrying none cannot be verified
    // as a substitute, and must not accrue zero penalty and win by default.
    json original = {{"mpn", "ORIG"}, {"impedance_100mhz", 151.0}, {"impedance_peak", 939.0},
                     {"impedance_peak_freq", 1038e6}, {"dcr", 0.325}, {"rated_current", 0.85}};
    json cands = json::array({
        {{"mpn", "NO_DATA"}, {"dcr", 0.009}, {"rated_current", 6.0}},
        {{"mpn", "CURVE_MATCH"}, {"impedance_100mhz", 150.0}, {"impedance_peak", 950.0},
         {"impedance_peak_freq", 1000e6}, {"dcr", 0.17}, {"rated_current", 0.9}}});
    auto r = cross_reference("chipBead", original, cands, Options{});
    CHECK(r["candidates"][0]["mpn"] == "CURVE_MATCH");
    auto blank = std::find_if(r["candidates"].begin(), r["candidates"].end(),
                              [](const json& c) { return c["mpn"] == "NO_DATA"; });
    REQUIRE(blank != r["candidates"].end());
    CHECK((*blank)["grade"] != "drop_in");
    // and it must sort strictly WORSE, not merely grade worse — a part we know
    // nothing about cannot win by having accrued the fewest penalties
    CHECK((*blank)["penalty"].get<double>() > r["candidates"][0]["penalty"].get<double>());
}

TEST_CASE("a partial-curve candidate still beats a no-data one", "[crossref][classes][rank]") {
    // The realistic catalogue case: the original has Z@100MHz, most candidates
    // only have the curve. Having SOME verifiable impedance must beat having none.
    json original = {{"mpn", "ORIG"}, {"impedance_100mhz", 151.0}, {"impedance_peak", 939.0},
                     {"impedance_peak_freq", 1038e6}, {"dcr", 0.325}, {"rated_current", 0.85}};
    json cands = json::array({
        {{"mpn", "NO_DATA"}, {"dcr", 0.009}, {"rated_current", 6.0}},
        {{"mpn", "CURVE_ONLY"}, {"impedance_peak", 950.0}, {"impedance_peak_freq", 1000e6},
         {"dcr", 0.17}, {"rated_current", 0.9}}});
    auto r = cross_reference("chipBead", original, cands, Options{});
    CHECK(r["candidates"][0]["mpn"] == "CURVE_ONLY");
}

// ── connector cross-reference realism ────────────────────────────────────────

TEST_CASE("a blank optional identity field is unknown, not a mismatch",
          "[crossref][classes][rank]") {
    // present() counts "" as present, so two connectors with no
    // interface_standard used to FAIL that exact-match spuriously. An empty
    // string on both sides is "not specified", i.e. unverified.
    json original = {{"mpn", "A"}, {"family", "terminalBlock"}, {"positions", 9},
                     {"interface_standard", ""}, {"rated_current_A", 18.0}};
    json cands = json::array({
        {{"mpn", "B"}, {"family", "terminalBlock"}, {"positions", 9},
         {"interface_standard", ""}, {"rated_current_A", 18.0}}});
    auto r = cross_reference("connector", original, cands, Options{});
    const json& c = r["candidates"][0];
    for (const auto& p : c["params"])
        if (p["name"] == "interface_standard") CHECK(p["verdict"] == UNVERIFIED);
    CHECK(c["status"] != "no_substitute");
}

TEST_CASE("a connector current shortfall demotes, it does not reject",
          "[crossref][classes][rank]") {
    // Same family, same position count, slightly lower current: a real
    // alternative the engineer may accept, not a no_substitute. A DIFFERENT
    // position count is still a hard reject.
    json original = {{"mpn", "O"}, {"family", "terminalBlock"}, {"positions", 9},
                     {"rated_current_A", 18.0}};
    json cands = json::array({
        {{"mpn", "LOWER_I"}, {"family", "terminalBlock"}, {"positions", 9},
         {"rated_current_A", 15.0}},
        {{"mpn", "WRONG_POS"}, {"family", "terminalBlock"}, {"positions", 6},
         {"rated_current_A", 18.0}}});
    auto r = cross_reference("connector", original, cands, Options{});
    auto lower = std::find_if(r["candidates"].begin(), r["candidates"].end(),
                              [](const json& c) { return c["mpn"] == "LOWER_I"; });
    auto wrong = std::find_if(r["candidates"].begin(), r["candidates"].end(),
                              [](const json& c) { return c["mpn"] == "WRONG_POS"; });
    REQUIRE(lower != r["candidates"].end());
    REQUIRE(wrong != r["candidates"].end());
    CHECK((*lower)["status"] == "partial");        // offered, flagged
    CHECK((*wrong)["status"] == "no_substitute");  // different part
}

// ── resistor device class: array/network vs discrete (ABT #481) ──────────────
// A Panasonic EXB-V8V is four isolated 51 ohm elements on eight terminals in a
// 3.2 x 1.6 mm body — the same OUTLINE as a discrete 1206. Judging fit on the
// outline alone returned three discrete chips as drop_in / footprint "fits" with
// no caveat, when one of them lands on two of the array's eight pads and leaves
// the other three nets open.

#include "../src/Browse.hpp"
#include "../src/Index.hpp"

TEST_CASE("a resistor's device class is read from what its record declares",
          "[crossref][classes][abt481]") {
    // Self-describing family strings, the form 2,724 catalogue records use.
    CHECK(declares_resistor_network("Chip Resistor Array", "EXBV8V510JV"));
    CHECK(declares_resistor_network("Chip Resistor Networks", "EXBE10C103J"));
    CHECK(declares_resistor_network("Anti-Sulfurated Chip Resistor Array", "EXBN8V510JX"));
    CHECK(declares_resistor_network("AF_Array", "AF164-JR-0751RL"));
    // Vendor series designators that name an array line without saying so:
    // YAGEO YC/TC (datasheet PYU-YC_TC_GROUP), Bourns CAT/CAY (CATCAY.pdf),
    // Panasonic EXB (AOC0000C12/C14/C20 — the backstop for the one EXB record
    // whose family string is empty).
    CHECK(declares_resistor_network("YC", "YC164-JR-0751RL"));
    CHECK(declares_resistor_network("TC", "TC122-JR-0739RL"));
    CHECK(declares_resistor_network("", "CAY16-180J4LF"));
    CHECK(declares_resistor_network("", "EXB-E10C103J"));
    // Plain discretes must NOT trip it — 145k records ride on this.
    CHECK_FALSE(declares_resistor_network("CR", "CR1206-JW-510ELF"));
    CHECK_FALSE(declares_resistor_network("RC", "RC1206JR-0751RL"));
    CHECK_FALSE(declares_resistor_network("RMCF0603", "RMCF0603JT33R0"));
    CHECK_FALSE(declares_resistor_network("RK73B1JTTD", "RK73B1JTTD330J"));
    CHECK_FALSE(declares_resistor_network("", ""));
    // The YC/TC rule needs the family AND the MPN to agree, so neither a stray
    // family letter pair nor an unrelated MPN prefix can fire it alone.
    CHECK_FALSE(declares_resistor_network("YC", "RC1206JR-0751RL"));
    CHECK_FALSE(declares_resistor_network("RC", "YC164-JR-0751RL"));
    // Bourns' rule keys on the digit that follows the series letters, so a part
    // named "CATHODE…" or "CAY" alone does not read as an array.
    CHECK_FALSE(declares_resistor_network("", "CAYX-180J4LF"));
}

TEST_CASE("a discrete chip is not a drop-in for a resistor array", "[crossref][classes][abt481]") {
    // The ticket's case, as the ranker sees it. Same value, same tolerance, the
    // discrete's power rating comfortably above the array's PER-ELEMENT 0.063 W,
    // and a body outline that matches to the micron — every electrical column
    // agrees, which is exactly why the device class has to be the thing that
    // decides it.
    json original = {{"mpn", "EXBV8V510JV"}, {"family", "Chip Resistor Array"},
                     {"value_si", 51.0},     {"power_rating", 0.063},
                     {"tolerance_pct", 5.0}, {"length_m", 0.0032},
                     {"width_m", 0.0016}};
    json cands = json::array({
        {{"mpn", "CR1206-JW-510ELF"}, {"family", "CR"}, {"value_si", 51.0},
         {"power_rating", 0.25}, {"tolerance_pct", 5.0}, {"case_code", "1206"}},
        // Another array: the class agrees, so this one is free to rank normally.
        {{"mpn", "YC164-JR-0751RL"}, {"family", "YC"}, {"value_si", 51.0},
         {"power_rating", 0.063}, {"tolerance_pct", 5.0}, {"length_m", 0.0032},
         {"width_m", 0.0016}}});
    auto r = cross_reference("resistor", original, cands, Options{});
    auto find = [&](const char* mpn) {
        for (const auto& c : r["candidates"])
            if (c["mpn"] == mpn) return c;
        FAIL("candidate " << mpn << " missing from the result");
        return json{};
    };

    const json discrete = find("CR1206-JW-510ELF");
    CHECK(discrete["status"] == "partial");
    CHECK(discrete["grade"] == "major_review");  // never drop_in
    // The outline matches, so the SIZE compare would have said "fits". The land
    // pattern is what actually differs, and that is what the verdict must name.
    CHECK(discrete["footprint"] == "different_land_pattern");
    bool fp_fail = false, class_fail = false;
    for (const auto& p : discrete["params"]) {
        if (p["name"] == "footprint") fp_fail = p["verdict"] == "fail";
        if (p["name"] == "configuration") class_fail = p["verdict"] == "fail";
    }
    CHECK(fp_fail);
    CHECK(class_fail);
    // And it says WHY, rather than leaving tool_notes null as it did.
    REQUIRE(discrete.contains("notes"));
    const std::string note = discrete["notes"][0];
    CHECK(note.find("array") != std::string::npos);
    CHECK(note.find("land pattern") != std::string::npos);
    CHECK(note.find("PER ELEMENT") != std::string::npos);

    const json array_sub = find("YC164-JR-0751RL");
    CHECK(array_sub["status"] == "recommended");
    CHECK(array_sub["grade"] == "drop_in");
    // Array-for-array must outrank array-for-discrete, not merely differ from it.
    CHECK(array_sub["penalty"].get<double>() < discrete["penalty"].get<double>());
}

TEST_CASE("an array offered for a discrete original is flagged the other way round",
          "[crossref][classes][abt481]") {
    // The mirror case: dropping a four-element network onto a two-pad land
    // pattern connects one element and shorts nothing to the rest.
    json original = {{"mpn", "RC1206JR-0751RL"}, {"family", "RC"}, {"value_si", 51.0},
                     {"power_rating", 0.25}, {"tolerance_pct", 5.0}, {"case_code", "1206"}};
    json cands = json::array({
        {{"mpn", "EXBV8V510JV"}, {"family", "Chip Resistor Array"}, {"value_si", 51.0},
         {"power_rating", 0.063}, {"tolerance_pct", 5.0}, {"length_m", 0.0032},
         {"width_m", 0.0016}}});
    auto r = cross_reference("resistor", original, cands, Options{});
    const json& c = r["candidates"][0];
    CHECK(c["status"] == "partial");
    CHECK(c["grade"] == "major_review");
    REQUIRE(c.contains("notes"));
    CHECK(std::string(c["notes"][0]).find("array") != std::string::npos);
}

TEST_CASE("the device class survives extraction into the browse row",
          "[crossref][classes][browse][abt481]") {
    // The loss was in EXTRACTION: the raw record declares the class twice
    // (manufacturerInfo.family and part.series) and neither reached the row, so
    // the ranker could not gate on a field it never received. Real catalogue
    // records, copied verbatim.
    auto shard = kelvin::build_resistor_shard(std::string(KELVIN_TEST_DIR) +
                                              "/fixtures/resistors_arrays.ndjson");
    json all = kelvin::browse::browse_rows(shard, json{{"limit", 100}});
    std::map<std::string, json> by_mpn;
    for (const auto& row : all.at("rows")) by_mpn[row.at("mpn").get<std::string>()] = row;
    REQUIRE(by_mpn.size() == 8);

    CHECK(by_mpn.at("EXBV8V510JV").at("family") == "Chip Resistor Array");
    CHECK(by_mpn.at("EXBE10C103J").at("family") == "Chip Resistor Networks");
    CHECK(by_mpn.at("YC164-JR-0751RL").at("family") == "YC");
    CHECK(by_mpn.at("AF164-JR-0751RL").at("family") == "AF_Array");
    CHECK(by_mpn.at("CR1206-JW-510ELF").at("family") == "CR");
    // No family on the record: `part.series` is read next, and where the record
    // states neither the row carries "" — an honest blank, not a guess.
    CHECK(by_mpn.at("CAY16-180J4LF").at("family") == "");
    CHECK(by_mpn.at("EXB-E10C103J").at("family") == "");

    // End to end on the real rows: the array original, the discrete substitute.
    auto spec = [](const json& row) {
        return json{{"mpn", row.at("mpn")},
                    {"family", row.at("family")},
                    {"value_si", row.at("resistance")},
                    {"power_rating", row.at("power_rating")},
                    {"length_m", row.at("lengthM")},
                    {"width_m", row.at("widthM")}};
    };
    auto r = cross_reference("resistor", spec(by_mpn.at("EXBV8V510JV")),
                             json::array({spec(by_mpn.at("CR1206-JW-510ELF"))}), Options{});
    CHECK(r["candidates"][0]["grade"] == "major_review");
    CHECK(r["candidates"][0]["footprint"] == "different_land_pattern");
}
