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
    CHECK(imperial_0603->length == Catch::Approx(1.60e-3));  // imperial, per distributor convention

    // "1608" is unambiguously metric — and resolves to the same physical size as
    // imperial 0603, which is the point: they are the same part.
    auto metric_1608 = resolve_dimensions("1608", "capacitor");
    REQUIRE(metric_1608.has_value());
    CHECK(metric_1608->length == Catch::Approx(1.60e-3));
    CHECK(metric_1608->width == Catch::Approx(0.80e-3));

    // The catalogue's explicit dual form must resolve, not fall through.
    auto dual = resolve_dimensions("1608M/0603", "capacitor");
    REQUIRE(dual.has_value());
    CHECK(dual->length == Catch::Approx(1.60e-3));
    CHECK(dual->width == Catch::Approx(0.80e-3));

    // A real electrolytic can code from the catalogue (diameter x height).
    auto can = resolve_dimensions("10x16", "capacitor");
    REQUIRE(can.has_value());
    CHECK(can->length == Catch::Approx(10e-3));
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
