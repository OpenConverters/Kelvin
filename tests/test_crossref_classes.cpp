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

// ── connector pitch: the land pattern, not a footnote (ABT #485) ─────────────

TEST_CASE("a connector pitch mismatch is a footprint failure, never a drop_in",
          "[crossref][classes][rank][abt485]") {
    // The reported case: Samtec TW-13-09-F-S-235-SM, 13 positions on a 2.00 mm
    // pitch. All three candidates matched family/positions/polarity and were
    // returned "recommended"/"drop_in" with footprint null, because the pitch
    // never reached the ranker. Pin 13 sits 12 x pitch from pin 1, so 2.54 mm
    // puts it 6.48 mm off its pad and 5.00 mm puts it 36 mm off.
    json original = {{"mpn", "TW-13-09-F-S-235-SM"}, {"family", "boardToBoard"},
                     {"positions", 13}, {"polarity", "male"}, {"pitch_mm", 2.0},
                     {"rated_current_A", 3.9}};
    json cands = json::array({
        {{"mpn", "SAME_PITCH"}, {"family", "boardToBoard"}, {"positions", 13},
         {"polarity", "male"}, {"pitch_mm", 2.0}, {"rated_current_A", 5.0}},
        {{"mpn", "P254"}, {"family", "boardToBoard"}, {"positions", 13},
         {"polarity", "male"}, {"pitch_mm", 2.54}, {"rated_current_A", 5.0}},
        {{"mpn", "P500"}, {"family", "boardToBoard"}, {"positions", 13},
         {"polarity", "male"}, {"pitch_mm", 5.0}, {"rated_current_A", 5.0}},
        {{"mpn", "NO_PITCH"}, {"family", "boardToBoard"}, {"positions", 13},
         {"polarity", "male"}, {"rated_current_A", 5.0}}});
    auto r = cross_reference("connector", original, cands, Options{});
    auto by_mpn = [&](const char* m) {
        auto it = std::find_if(r["candidates"].begin(), r["candidates"].end(),
                               [&](const json& c) { return c["mpn"] == m; });
        REQUIRE(it != r["candidates"].end());
        return *it;
    };
    auto verdict_of = [](const json& c, const char* name) {
        for (const auto& p : c["params"])
            if (p["name"] == name) return p["verdict"].get<std::string>();
        return std::string("<absent>");
    };

    const json same = by_mpn("SAME_PITCH");
    CHECK(verdict_of(same, "pitch_mm") == PASS);
    CHECK(same["grade"] == "drop_in");

    for (const char* m : {"P254", "P500"}) {
        const json c = by_mpn(m);
        INFO(m);
        CHECK(verdict_of(c, "pitch_mm") == FAIL);
        // the land pattern is REPORTED, not left null next to a known mismatch
        CHECK(c["footprint"] == "different_land_pattern");
        CHECK(verdict_of(c, "footprint") == FAIL);
        CHECK(c["grade"] != "drop_in");
        CHECK(c["status"] == "partial");
        CHECK(c["penalty"].get<double>() > same["penalty"].get<double>());
    }
    // the wrong-pitch parts must not outrank the one that keeps the land pattern
    CHECK(r["candidates"][0]["mpn"] == "SAME_PITCH");

    // A candidate whose record states no pitch is UNVERIFIED — an unknown, not a
    // mismatch. Kelvin does not invent the datum, and does not fail it either.
    const json unknown = by_mpn("NO_PITCH");
    CHECK(verdict_of(unknown, "pitch_mm") == UNVERIFIED);
    CHECK_FALSE(unknown.contains("footprint"));

    // and the note names the geometry, not just "pitch differs"
    const json p254 = by_mpn("P254");
    REQUIRE(p254.contains("notes"));
    std::string notes = p254["notes"].dump();
    CHECK(notes.find("2 -> 2.54 mm") != std::string::npos);
    CHECK(notes.find("6.48 mm off its pad") != std::string::npos);
}

// ── connector operating temperature: a hard environmental limit (ABT #520) ───

TEST_CASE("a connector rated 20 degC cooler than the original is not an upgrade",
          "[crossref][classes][rank][abt520]") {
    // The reported case: Molex MM-214-025-161-00WE, -55/+125 degC, 3 A per contact.
    // All six Amphenol candidates are -55/+105 degC and 5 A, and every one came back
    // direction "upgrade" with no temperature entry in the verdict table — the label
    // rested on the current alone while the only other comparable limit moved the
    // wrong way by 20 degC.
    json original = {{"mpn", "MM-214-025-161-00WE"}, {"family", "dataInterface"},
                     {"positions", 25},              {"polarity", "male"},
                     {"rated_current_A", 3.0},       {"rated_voltage_V", 600.0},
                     {"temp_min_C", -55.0},          {"temp_max_C", 125.0}};
    json cands = json::array({
        {{"mpn", "SAME_RANGE"}, {"family", "dataInterface"}, {"positions", 25},
         {"polarity", "male"}, {"rated_current_A", 5.0}, {"rated_voltage_V", 600.0},
         {"temp_min_C", -55.0}, {"temp_max_C", 125.0}},
        {{"mpn", "DERATED_105"}, {"family", "dataInterface"}, {"positions", 25},
         {"polarity", "male"}, {"rated_current_A", 5.0}, {"rated_voltage_V", 600.0},
         {"temp_min_C", -55.0}, {"temp_max_C", 105.0}},
        {{"mpn", "COLD_SHORT"}, {"family", "dataInterface"}, {"positions", 25},
         {"polarity", "male"}, {"rated_current_A", 5.0}, {"rated_voltage_V", 600.0},
         {"temp_min_C", -25.0}, {"temp_max_C", 125.0}},
        {{"mpn", "NO_TEMP"}, {"family", "dataInterface"}, {"positions", 25},
         {"polarity", "male"}, {"rated_current_A", 5.0}, {"rated_voltage_V", 600.0}}});
    auto r = cross_reference("connector", original, cands, Options{});
    auto by_mpn = [&](const char* m) {
        auto it = std::find_if(r["candidates"].begin(), r["candidates"].end(),
                               [&](const json& c) { return c["mpn"] == m; });
        REQUIRE(it != r["candidates"].end());
        return *it;
    };
    auto verdict_of = [](const json& c, const char* name) {
        for (const auto& p : c["params"])
            if (p["name"] == name) return p["verdict"].get<std::string>();
        return std::string("<absent>");
    };
    auto count_of = [](const json& c, const char* name) {
        int n = 0;
        for (const auto& p : c["params"])
            if (p["name"] == name) ++n;
        return n;
    };

    // Same range, more current: an upgrade, and it stays one.
    const json same = by_mpn("SAME_RANGE");
    CHECK(verdict_of(same, "temp_max_C") == PASS);
    CHECK(same["direction"] == "upgrade");
    CHECK(same["grade"] == "drop_in");

    // 20 degC of lost headroom: judged, penalised, explained, and NOT an upgrade.
    const json hot = by_mpn("DERATED_105");
    CHECK(verdict_of(hot, "temp_max_C") == FAIL);
    CHECK(hot["direction"] != "upgrade");
    CHECK(hot["grade"] != "drop_in");
    CHECK(hot["status"] == "partial");
    CHECK(hot["penalty"].get<double>() > same["penalty"].get<double>());
    REQUIRE(hot.contains("notes"));
    // the note sizes the gap — "does not reach the maximum" is true of 1 degC too
    CHECK(hot["notes"].dump().find("+105 degC vs the original's +125 degC") !=
          std::string::npos);
    // and it must not outrank the part that keeps the whole range
    CHECK(r["candidates"][0]["mpn"] == "SAME_RANGE");

    // The cold end is a limit too, and it is reported once: the category's own
    // PARAM_SPECS rules it, so the shared temperature block adds the note only.
    const json cold = by_mpn("COLD_SHORT");
    CHECK(verdict_of(cold, "temp_min_C") == FAIL);
    CHECK(count_of(cold, "temp_min_C") == 1);
    CHECK(cold["notes"].dump().find("-25 degC vs the original's -55 degC") !=
          std::string::npos);

    // A record that states no temperature is UNVERIFIED — an unknown, not a
    // regression. Kelvin does not invent the rating, and does not fail it either.
    const json unknown = by_mpn("NO_TEMP");
    CHECK(verdict_of(unknown, "temp_max_C") == UNVERIFIED);
    CHECK(verdict_of(unknown, "temp_min_C") == UNVERIFIED);
}

// ── connector mating interface: plating, termination, cycles (ABT #487) ──────

TEST_CASE("a connector that changes the mating interface is judged, not caveated away",
          "[crossref][classes][rank][abt487]") {
    // The reported case: the family caveat read "the catalogue carries no pitch, plating
    // or termination data", so none of it was compared and every candidate came back
    // drop_in. The records state all of it. A gold-plated original meets its counterpart
    // on gold; a tin-plated substitute meets that same counterpart on tin, which is the
    // one pairing connector practice rules out.
    json original = {{"mpn", "TW-13-09-F-S-235-SM"}, {"family", "boardToBoard"},
                     {"positions", 13},              {"polarity", "male"},
                     {"pitch_mm", 2.0},              {"rated_current_A", 3.9},
                     {"contact_plating", "au-gold"}, {"termination", "crimp"},
                     {"mating_cycles", 500.0}};
    json cands = json::array({
        {{"mpn", "SAME_IFACE"}, {"family", "boardToBoard"}, {"positions", 13},
         {"polarity", "male"}, {"pitch_mm", 2.0}, {"rated_current_A", 5.0},
         {"contact_plating", "au-gold"}, {"termination", "crimp"}, {"mating_cycles", 500.0}},
        {{"mpn", "TIN"}, {"family", "boardToBoard"}, {"positions", 13},
         {"polarity", "male"}, {"pitch_mm", 2.0}, {"rated_current_A", 5.0},
         {"contact_plating", "sn-tin"}, {"termination", "crimp"}, {"mating_cycles", 500.0}},
        {{"mpn", "IDC"}, {"family", "boardToBoard"}, {"positions", 13},
         {"polarity", "male"}, {"pitch_mm", 2.0}, {"rated_current_A", 5.0},
         {"contact_plating", "au-gold"}, {"termination", "idc"}, {"mating_cycles", 500.0}},
        {{"mpn", "WORN"}, {"family", "boardToBoard"}, {"positions", 13},
         {"polarity", "male"}, {"pitch_mm", 2.0}, {"rated_current_A", 5.0},
         {"contact_plating", "au-gold"}, {"termination", "crimp"}, {"mating_cycles", 30.0}},
        {{"mpn", "NO_IFACE"}, {"family", "boardToBoard"}, {"positions", 13},
         {"polarity", "male"}, {"pitch_mm", 2.0}, {"rated_current_A", 5.0}}});
    auto r = cross_reference("connector", original, cands, Options{});
    auto by_mpn = [&](const char* m) {
        auto it = std::find_if(r["candidates"].begin(), r["candidates"].end(),
                               [&](const json& c) { return c["mpn"] == m; });
        REQUIRE(it != r["candidates"].end());
        return *it;
    };
    auto verdict_of = [](const json& c, const char* name) {
        for (const auto& p : c["params"])
            if (p["name"] == name) return p["verdict"].get<std::string>();
        return std::string("<absent>");
    };

    // The like-for-like part keeps the interface and stays a drop-in.
    const json same = by_mpn("SAME_IFACE");
    CHECK(verdict_of(same, "contact_plating") == PASS);
    CHECK(verdict_of(same, "termination") == PASS);
    CHECK(verdict_of(same, "mating_cycles") == PASS);
    CHECK(same["grade"] == "drop_in");

    // Gold -> tin: judged and named, not left to a caveat that says it cannot be checked.
    const json tin = by_mpn("TIN");
    CHECK(verdict_of(tin, "contact_plating") == FAIL);
    CHECK(tin["grade"] != "drop_in");
    CHECK(tin["penalty"].get<double>() > same["penalty"].get<double>());
    REQUIRE(tin.contains("notes"));
    CHECK(tin["notes"].dump().find("au-gold -> sn-tin") != std::string::npos);

    // Crimp -> IDC is a different wire-attach method, not a finish.
    const json idc = by_mpn("IDC");
    CHECK(verdict_of(idc, "termination") == FAIL);
    CHECK(idc["grade"] != "drop_in");
    CHECK(idc["notes"].dump().find("crimp -> idc") != std::string::npos);

    // 500 mating cycles down to 30 is a durability regression, and it is now visible.
    const json worn = by_mpn("WORN");
    CHECK(verdict_of(worn, "mating_cycles") == FAIL);
    CHECK(worn["grade"] != "drop_in");

    // A record that states none of it is UNVERIFIED — an unknown, not a mismatch. The
    // spec block must send null rather than "", or every such part would FAIL here.
    const json unknown = by_mpn("NO_IFACE");
    CHECK(verdict_of(unknown, "contact_plating") == UNVERIFIED);
    CHECK(verdict_of(unknown, "termination") == UNVERIFIED);
    CHECK(verdict_of(unknown, "mating_cycles") == UNVERIFIED);

    // and none of the changed-interface parts may outrank the one that keeps it
    CHECK(r["candidates"][0]["mpn"] == "SAME_IFACE");
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

// ── resistor sulfuration resistance (ABT #518) ───────────────────────────────
// Panasonic's ERJ-S exists as a separate series from the electrically identical
// ERJ because its inner electrode survives a sulfur-bearing atmosphere. Every
// number on the two records matches and both read "thickFilm", so the family /
// series string is the only thing that can tell them apart — and it produced no
// verdict, no note and no caveat.

TEST_CASE("a resistor's sulfuration class is read from what its record declares",
          "[crossref][classes][abt518]") {
    // Self-describing family strings — the form all 32,656 Panasonic records use.
    CHECK(sulfur_resistant_by_design("Anti-Sulfurated Thick Film Chip Resistors", "ERJS1TF63R4U"));
    CHECK(sulfur_resistant_by_design("Anti-Sulfurated Thick Film Chip Resistors/ Anti-Surge Type",
                                     "ERJU02F1001X"));
    CHECK(sulfur_resistant_by_design("Anti-Sulfurated Chip Resistor Array", "EXBN8V510JX"));
    CHECK(sulfur_resistant_by_design("Anti Sulfurated High Power Wide Terminal Chip Resistors",
                                     "ERJC1CF1R00U"));
    // Vendor series whose OWN linked datasheet titles the line anti-sulfurated:
    // YAGEO AA / AF / AH / AS / RP, Würth WRIS-RSKS (ASTM-B-809).
    CHECK(sulfur_resistant_by_design("AF", "AF2512FR-0763R4L"));
    CHECK(sulfur_resistant_by_design("AF_Array", "AF164-JR-0751RL"));
    CHECK(sulfur_resistant_by_design("AA0402DR-0", "AA0402DR-0710KL"));
    CHECK(sulfur_resistant_by_design("AH", "AH0603FR-071KL"));
    CHECK(sulfur_resistant_by_design("AS", "AS1206FR-0715RL"));
    CHECK(sulfur_resistant_by_design("RP", "RP0603DRD0710KL"));
    CHECK(sulfur_resistant_by_design("WRIS-RSKS", "560112110002"));
    // The general-purpose lines must NOT trip it — 112k records ride on this.
    CHECK_FALSE(sulfur_resistant_by_design("CR", "CR2512-FX-63R4ELF"));
    CHECK_FALSE(sulfur_resistant_by_design("RC", "RC2512FK-0763R4L"));
    CHECK_FALSE(sulfur_resistant_by_design("AC", "AC2512FK-0763R4L"));
    CHECK_FALSE(sulfur_resistant_by_design("AWW", "YagAWW62R251218635"));
    CHECK_FALSE(sulfur_resistant_by_design("Thick Film Chip Resistors", "ERJ1GJJ392C"));
    CHECK_FALSE(sulfur_resistant_by_design("", ""));
    // The series code needs the family AND the part number to agree, so neither a
    // stray family code nor an unrelated part number can fire it alone, and the
    // digit that follows the code is what separates "AS1206…" from "ASSY-…".
    CHECK_FALSE(sulfur_resistant_by_design("AF", "RC2512FK-0763R4L"));
    CHECK_FALSE(sulfur_resistant_by_design("CR", "AF2512FR-0763R4L"));
    CHECK_FALSE(sulfur_resistant_by_design("AS", "ASSY-1206-100R"));

    // A line named for something else whose datasheet still states the property
    // SATISFIES a sulfur-resistant original without being one of the series that
    // triggers the question: YAGEO's thin films and current sensors, and the MELF
    // bodies with no silver inner electrode to lose.
    CHECK(declares_sulfur_resistance("AT", "AT0603DRD0710KL"));
    CHECK(declares_sulfur_resistance("PE Wide Terminal", "PE2512FKE070R01L"));
    CHECK(declares_sulfur_resistance("SMM0102", "SMM01020C4709FB300"));
    CHECK(declares_sulfur_resistance("ZCM", "ZCM204FKE07-10RAA"));
    CHECK_FALSE(sulfur_resistant_by_design("AT", "AT0603DRD0710KL"));
    // "PS" and "PSP" are different lines; the code must end at a non-letter.
    CHECK_FALSE(declares_sulfur_resistance("PSP", "PSP1206-100R"));
    CHECK_FALSE(declares_sulfur_resistance("CR", "CR2512-FX-63R4ELF"));
}

TEST_CASE("a standard thick-film chip is not a drop-in for an anti-sulfurated original",
          "[crossref][classes][abt518]") {
    // The ticket's case, as the ranker sees it: 63.4 ohm, 1 %, 1 W, 2512 on both
    // sides, both thick film. Nothing numeric can separate them, which is exactly
    // why the construction class has to be the thing that decides it.
    json original = {{"mpn", "ERJS1TF63R4U"},
                     {"family", "Anti-Sulfurated Thick Film Chip Resistors"},
                     {"value_si", 63.4},
                     {"power_rating", 1.0},
                     {"tolerance_pct", 1.0},
                     {"case_code", "2512"},
                     {"length_m", 0.0064},
                     {"width_m", 0.0032}};
    json cands = json::array({
        {{"mpn", "CR2512-FX-63R4ELF"}, {"family", "CR"}, {"value_si", 63.4},
         {"power_rating", 1.0}, {"tolerance_pct", 1.0}, {"case_code", "2512"},
         {"length_m", 0.0064}, {"width_m", 0.0032}},
        // A record that names no series at all: the class is not knowable from it,
        // and the note has to say that rather than name a line it does not have.
        {{"mpn", "RC2512FK-0763R4L"}, {"family", ""}, {"value_si", 63.4},
         {"power_rating", 1.0}, {"tolerance_pct", 1.0}, {"case_code", "2512"},
         {"length_m", 0.0064}, {"width_m", 0.0032}},
        // An anti-sulfurated line: the class agrees, so this one ranks normally.
        {{"mpn", "AF2512FR-0763R4L"}, {"family", "AF"}, {"value_si", 63.4},
         {"power_rating", 1.0}, {"tolerance_pct", 1.0}, {"case_code", "2512"},
         {"length_m", 0.0064}, {"width_m", 0.0032}}});
    auto r = cross_reference("resistor", original, cands, Options{});
    auto find = [&](const char* mpn) {
        for (const auto& c : r["candidates"])
            if (c["mpn"] == mpn) return c;
        FAIL("candidate " << mpn << " missing from the result");
        return json{};
    };
    auto verdict = [](const json& c, const char* name) {
        for (const auto& p : c["params"])
            if (p["name"] == name) return p["verdict"].get<std::string>();
        return std::string("<absent>");
    };

    const json plain = find("CR2512-FX-63R4ELF");
    CHECK(plain["status"] == "partial");
    CHECK(plain["grade"] == "major_review");  // was drop_in / recommended
    CHECK(verdict(plain, "sulfur_resistance") == "fail");
    // The part still FITS — this is a reliability class, not a land pattern.
    CHECK(plain["footprint"] == "fits");
    CHECK(verdict(plain, "footprint") == "pass");
    // And it says WHY, rather than leaving notes null as it did.
    REQUIRE(plain.contains("notes"));
    const std::string note = plain["notes"][0];
    CHECK(note.find("anti-sulfurated") != std::string::npos);
    CHECK(note.find("\"CR\"") != std::string::npos);
    CHECK(note.find("fails OPEN") != std::string::npos);

    // Silence about the series reads as silence, not as a different series.
    const json silent = find("RC2512FK-0763R4L");
    CHECK(silent["grade"] == "major_review");
    CHECK(verdict(silent, "sulfur_resistance") == "fail");
    REQUIRE(silent.contains("notes"));
    CHECK(std::string(silent["notes"][0]).find("names no series at all") != std::string::npos);

    const json anti_sulfur = find("AF2512FR-0763R4L");
    CHECK(anti_sulfur["status"] == "recommended");
    CHECK(anti_sulfur["grade"] == "drop_in");
    CHECK(verdict(anti_sulfur, "sulfur_resistance") == "pass");
    CHECK_FALSE(anti_sulfur.contains("notes"));
    // Like-for-like must OUTRANK the caveated parts, not merely differ from them.
    CHECK(anti_sulfur["penalty"].get<double>() < plain["penalty"].get<double>());
}

TEST_CASE("an anti-sulfurated substitute for an ordinary original is not a caveat",
          "[crossref][classes][abt518]") {
    // Asked one way only. Sulfur resistance is a property ADDED to the original's
    // spec, and an engineer replacing an ordinary chip with an ERJ-S loses
    // nothing — flagging it would be noise on 112k records.
    json original = {{"mpn", "CR2512-FX-63R4ELF"}, {"family", "CR"},   {"value_si", 63.4},
                     {"power_rating", 1.0},        {"tolerance_pct", 1.0}, {"case_code", "2512"},
                     {"length_m", 0.0064},         {"width_m", 0.0032}};
    json cands = json::array({
        {{"mpn", "ERJS1TF63R4U"}, {"family", "Anti-Sulfurated Thick Film Chip Resistors"},
         {"value_si", 63.4}, {"power_rating", 1.0}, {"tolerance_pct", 1.0},
         {"case_code", "2512"}, {"length_m", 0.0064}, {"width_m", 0.0032}}});
    auto r = cross_reference("resistor", original, cands, Options{});
    const json& c = r["candidates"][0];
    CHECK(c["status"] == "recommended");
    CHECK(c["grade"] == "drop_in");
    CHECK_FALSE(c.contains("notes"));
    for (const auto& p : c["params"]) CHECK(p["name"] != "sulfur_resistance");
}

// ── diode device configuration: bridge module vs discrete (ABT #521) ─────────
// GBU8KS is a 4-die single-phase bridge on four terminals (AC, AC, +, -). Offered
// against Vishay's S8J — a two-terminal SMC rectifier — it was graded
// minor_review, the only candidate in a 307-part pool above major_review, on a
// single note that reduced the difference to a land-pattern change. Its 1.0 V Vf
// is per element (two in series: ~2.0 V in circuit, not the +18 % the vf=warn
// implied) and its 8 A If(AV) is bridge OUTPUT current, ~4 A per element, against
// S8J's 8 A per-diode rating that if_avg=pass equated it to.

TEST_CASE("a diode's device configuration is read from what its package declares",
          "[crossref][classes][abt521]") {
    // Outlines that exist only as single-phase bridges (112 catalogue records).
    CHECK(declares_diode_module("GBU"));
    CHECK(declares_diode_module("GBJ"));
    CHECK(declares_diode_module("KBPC"));
    CHECK(declares_diode_module("KBP"));
    CHECK(declares_diode_module("MBS"));
    // The trailing digits of GBPC4 are the outline's current rating, not leads.
    CHECK(declares_diode_module("GBPC4 28.75x28.75x11.10"));
    // Generic packages whose NAME states the four terminals (22 records).
    CHECK(declares_diode_module("SIP-4"));
    CHECK(declares_diode_module("DIP-4"));
    CHECK(declares_diode_module("SOIC-4"));
    CHECK(declares_diode_module("SOIC4 W"));
    CHECK(declares_diode_module("TSSOP-4"));
    // Two-terminal outlines must NOT trip it — 17,931 records ride on this, and
    // the trailing number on a chip/JEDEC code is a size or a registration
    // number, never a terminal count.
    CHECK_FALSE(declares_diode_module("SMC (DO-214AB)"));
    CHECK_FALSE(declares_diode_module("SOD-123"));
    CHECK_FALSE(declares_diode_module("SOT-23 (TO-236) 2.90x1.30x1.00, 1.90P"));
    CHECK_FALSE(declares_diode_module("DO-201AD"));
    CHECK_FALSE(declares_diode_module("TO-220AB"));
    CHECK_FALSE(declares_diode_module("SMA-2"));
    CHECK_FALSE(declares_diode_module(""));
    // FOUR leads only. SO-8 duals and SOT-363-6 arrays are multi-die parts too,
    // but nothing in their case string states a terminal count, so this gate does
    // not judge them rather than guessing (see CrossRefClasses.hpp).
    CHECK_FALSE(declares_diode_module("SOIC-8"));
    CHECK_FALSE(declares_diode_module("TSSOP6"));
    CHECK_FALSE(declares_diode_module("SC-88-6 / SC-70-6 / SOT-363-6"));

    // The MPN only ever sharpens the WORDING once the package has spoken, so it
    // is checked on the shape that separates a bridge line from a part that
    // merely starts with the same letters.
    CHECK(names_bridge_series("GBU8KS"));
    CHECK(names_bridge_series("MB6S"));
    CHECK(names_bridge_series("MDB10S"));
    CHECK(names_bridge_series("DF02M"));
    CHECK(names_bridge_series("DB107"));
    CHECK_FALSE(names_bridge_series("MBRS340"));   // schottky, not a bridge line
    CHECK_FALSE(names_bridge_series("DFB25100"));  // no digit after "DF"
    CHECK_FALSE(names_bridge_series(""));
}

TEST_CASE("a bridge rectifier module is not a substitute for a two-terminal diode",
          "[crossref][classes][abt521]") {
    // The ticket's case as the ranker sees it: S8J, and the two candidates that
    // headed its list. Every electrical column of GBU8KS reads BETTER than the
    // discrete's, which is exactly why the device class has to decide it.
    json original = {{"mpn", "S8J"},   {"vrrm", 600.0},          {"if_avg", 8.0},
                     {"vf", 0.85},     {"technology", "rectifier"},
                     {"case_code", "SMC (DO-214AB)"}};
    json cands = json::array({
        {{"mpn", "GBU8KS"}, {"vrrm", 800.0}, {"if_avg", 8.0}, {"vf", 1.0},
         {"technology", "rectifier"}, {"case_code", "SIP-4"}},
        // A genuine two-terminal discrete whose only deficit is a higher Vf.
        {{"mpn", "IDV08E65D2"}, {"vrrm", 650.0}, {"if_avg", 8.0}, {"vf", 1.6},
         {"technology", "rectifier"}},
    });
    auto r = cross_reference("diode", original, cands, Options{});
    auto find = [&](const char* mpn) {
        for (const auto& c : r["candidates"])
            if (c["mpn"] == mpn) return c;
        FAIL("candidate " << mpn << " missing from the result");
        return json{};
    };

    const json bridge = find("GBU8KS");
    CHECK(bridge["status"] == "partial");
    CHECK(bridge["grade"] == "major_review");  // never minor_review
    // "different_case" said the pads move; they cannot be made to match at all.
    CHECK(bridge["footprint"] == "different_land_pattern");
    bool fp_fail = false, class_fail = false;
    for (const auto& p : bridge["params"]) {
        if (p["name"] == "footprint") fp_fail = p["verdict"] == "fail";
        if (p["name"] == "configuration") class_fail = p["verdict"] == "fail";
    }
    CHECK(fp_fail);
    CHECK(class_fail);
    // And the note says what it is and why the table's own numbers are not
    // comparable, instead of "verify pads/pinout".
    REQUIRE(bridge.contains("notes"));
    const std::string note = bridge["notes"][0];
    CHECK(note.find("BRIDGE RECTIFIER") != std::string::npos);
    CHECK(note.find("AC, AC, +, -") != std::string::npos);
    CHECK(note.find("PER ELEMENT") != std::string::npos);
    CHECK(note.find("OUTPUT current") != std::string::npos);

    // The whole point of the ticket: the bridge must not head the list over a
    // discrete whose only deficit is a real, comparable Vf regression.
    const json discrete = find("IDV08E65D2");
    CHECK(bridge["penalty"].get<double>() > discrete["penalty"].get<double>());
    CHECK(r["candidates"][0]["mpn"] == "IDV08E65D2");
}

TEST_CASE("a discrete offered for a bridge original is flagged the other way round",
          "[crossref][classes][abt521]") {
    // The mirror case: one discrete replaces ONE of the bridge's four elements,
    // and the original's ratings are per element / bridge output.
    json original = {{"mpn", "GBU8K"}, {"vrrm", 800.0}, {"if_avg", 8.0}, {"vf", 1.0},
                     {"technology", "rectifier"}, {"case_code", "GBU"}};
    json cands = json::array({{{"mpn", "S8K"},
                               {"vrrm", 800.0},
                               {"if_avg", 8.0},
                               {"vf", 0.85},
                               {"technology", "rectifier"},
                               {"case_code", "SMC (DO-214AB)"}}});
    auto r = cross_reference("diode", original, cands, Options{});
    const json& c = r["candidates"][0];
    CHECK(c["status"] == "partial");
    CHECK(c["grade"] == "major_review");
    CHECK(c["footprint"] == "different_land_pattern");
    REQUIRE(c.contains("notes"));
    const std::string note = c["notes"][0];
    CHECK(note.find("the original is a single-phase BRIDGE RECTIFIER") != std::string::npos);
    CHECK(note.find("four discretes") != std::string::npos);
}

TEST_CASE("a package that states four leads but no bridge series says only what it knows",
          "[crossref][classes][abt521]") {
    // DFB25100 (onsemi, SIP-4) declares four leads; its MPN matches no bridge
    // series in the table, so the note claims the terminals and not the topology.
    json original = {{"mpn", "S8J"}, {"vrrm", 600.0}, {"if_avg", 8.0}, {"vf", 0.85},
                     {"technology", "rectifier"}, {"case_code", "SMC (DO-214AB)"}};
    json cands = json::array({{{"mpn", "DFB25100"},
                               {"vrrm", 1000.0},
                               {"if_avg", 25.0},
                               {"vf", 1.1},
                               {"technology", "rectifier"},
                               {"case_code", "SIP-4"}}});
    auto r = cross_reference("diode", original, cands, Options{});
    const json& c = r["candidates"][0];
    CHECK(c["grade"] == "major_review");
    CHECK(c["footprint"] == "different_land_pattern");
    const std::string note = c["notes"][0];
    CHECK(note.find("states four leads") != std::string::npos);
    CHECK(note.find("BRIDGE") == std::string::npos);  // not asserted, not known
}

TEST_CASE("the four-lead package survives extraction into the browse row",
          "[crossref][classes][browse][abt521]") {
    // The class evidence was already in the row — this pins that, so a later
    // extractor change cannot quietly take the gate's only input away. Real
    // catalogue records, copied verbatim.
    auto shard = kelvin::build_diode_shard(std::string(KELVIN_TEST_DIR) +
                                           "/fixtures/diodes_bridges.ndjson");
    json all = kelvin::browse::browse_rows(shard, json{{"limit", 100}});
    std::map<std::string, json> by_mpn;
    for (const auto& row : all.at("rows")) by_mpn[row.at("mpn").get<std::string>()] = row;
    REQUIRE(by_mpn.size() == 8);

    CHECK(by_mpn.at("GBU8KS").at("caseCode") == "SIP-4");
    CHECK(by_mpn.at("GBU8K").at("caseCode") == "GBU");
    CHECK(by_mpn.at("MB6S").at("caseCode") == "SOIC-4");
    CHECK(by_mpn.at("DF02M").at("caseCode") == "DIP-4");
    CHECK(by_mpn.at("MDB6S").at("caseCode") == "TSSOP-4");
    CHECK(by_mpn.at("S8J").at("caseCode") == "SMC (DO-214AB)");

    // End to end on the real rows: the two-terminal original, every bridge in the
    // fixture as a candidate, and the one genuine discrete.
    // Exactly what crossref.js's diode `spec` builds, including its `caseCode ?? ''`
    // — browse omits the key on a record that states no case.
    auto spec = [](const json& row) {
        return json{{"mpn", row.at("mpn")},
                    {"vrrm", row.at("vrrm_rated")},
                    {"if_avg", row.at("if_avg_rated")},
                    {"vf", row.at("vf_typ")},
                    {"technology", row.at("technology")},
                    {"case_code", row.value("caseCode", "")}};
    };
    json cands = json::array();
    for (const char* m : {"GBU8KS", "GBU8K", "MB6S", "DF02M", "MDB6S", "IDV08E65D2"})
        cands.push_back(spec(by_mpn.at(m)));
    auto r = cross_reference("diode", spec(by_mpn.at("S8J")), cands, Options{});
    for (const auto& c : r["candidates"]) {
        const std::string mpn = c["mpn"];
        if (mpn == "IDV08E65D2") continue;
        INFO("candidate " << mpn);
        CHECK(c["grade"] != "drop_in");
        CHECK(c["grade"] != "minor_review");
    }
}

// ── zener breakdown grade: the window, not the marking (ABT #488) ────────────
// The reported case: Nexperia BZX84-A3V6-Q, the '-A' grade of a 3.6 V zener, whose
// record guarantees 3.56-3.64 V (1.11 %). Every candidate offered against it — all
// nominal-only records — came back {"name":"vrrm","verdict":"pass"} with no caveat
// and one note, about the package. Vrrm compares the MARKED voltage, which the
// standard '-B' grade (3.42-3.78 V, 5 %) shares, so silence there read as agreement
// on the one spec a reference or a clamp is designed around. The tool's own
// if_avg=unverified on identically-absent candidate data is the honest treatment it
// withheld here.

TEST_CASE("a zener's guaranteed window is compared, not just its marked voltage",
          "[crossref][classes][rank][abt488]") {
    json original = {{"mpn", "BZX84-A3V6-Q"}, {"technology", "zener"}, {"case_code", "SOT23"},
                     {"vrrm", 3.6},           {"if_avg", 0.2},        {"vz_min_V", 3.56},
                     {"vz_max_V", 3.64},      {"vz_tolerance_pct", 100.0 * 0.08 / 7.2}};
    json cands = json::array({
        {{"mpn", "A_GRADE"}, {"technology", "zener"}, {"case_code", "SOT23"}, {"vrrm", 3.6},
         {"if_avg", 0.2}, {"vz_min_V", 3.56}, {"vz_max_V", 3.64},
         {"vz_tolerance_pct", 100.0 * 0.08 / 7.2}},
        {{"mpn", "B_GRADE"}, {"technology", "zener"}, {"case_code", "SOT23"}, {"vrrm", 3.6},
         {"if_avg", 0.2}, {"vz_min_V", 3.42}, {"vz_max_V", 3.78}, {"vz_tolerance_pct", 5.0}},
        {{"mpn", "NO_BAND"}, {"technology", "zener"}, {"case_code", "SOT23"}, {"vrrm", 3.6},
         {"if_avg", 0.2}}});
    auto r = cross_reference("diode", original, cands, Options{});
    auto by_mpn = [&](const char* m) {
        auto it = std::find_if(r["candidates"].begin(), r["candidates"].end(),
                               [&](const json& c) { return c["mpn"] == m; });
        REQUIRE(it != r["candidates"].end());
        return *it;
    };
    auto verdict_of = [](const json& c, const char* name) {
        for (const auto& p : c["params"])
            if (p["name"] == name) return p["verdict"].get<std::string>();
        return std::string("<absent>");
    };

    // Same grade: judged, passed, and said nothing about — a match needs no note.
    const json same = by_mpn("A_GRADE");
    CHECK(verdict_of(same, "vz_tolerance_pct") == PASS);
    CHECK(same["grade"] == "drop_in");
    CHECK_FALSE(same.contains("notes"));

    // The '-B' grade: same marking, 4.5x the window. A regression, sized in volts.
    const json loose = by_mpn("B_GRADE");
    CHECK(verdict_of(loose, "vz_tolerance_pct") == FAIL);
    CHECK(verdict_of(loose, "vrrm") == PASS);  // the marking still agrees, and still says so
    CHECK(loose["status"] == "partial");
    CHECK(loose["grade"] == "major_review");
    CHECK(loose["penalty"].get<double>() > same["penalty"].get<double>());
    REQUIRE(loose.contains("notes"));
    CHECK(loose["notes"].dump().find("3.42-3.78 V (a 5 % grade)") != std::string::npos);
    CHECK(loose["notes"].dump().find("3.56-3.64 V (a 1.11 % grade)") != std::string::npos);

    // A record that states no window: UNVERIFIED, exactly as the same absence is
    // reported for if_avg — never a pass on the nominal — and the note says which
    // window is going unchecked, in volts, rather than leaving it to be inferred.
    const json unknown = by_mpn("NO_BAND");
    CHECK(verdict_of(unknown, "vz_tolerance_pct") == UNVERIFIED);
    REQUIRE(unknown.contains("notes"));
    CHECK(unknown["notes"].dump().find("the original guarantees 3.56-3.64 V (a 1.11 % grade)") !=
          std::string::npos);
    CHECK(unknown["notes"].dump().find("no breakdown-voltage band") != std::string::npos);
    // and the unknown is not a regression: nothing was compared, so nothing failed
    CHECK(unknown["status"] == "recommended");

    // The other way round — the original's own window is not on record — reports the
    // same unverified verdict WITHOUT the note: there is no guarantee being dropped.
    json bare_original = {{"mpn", "NOM_ONLY"}, {"technology", "zener"}, {"case_code", "SOT23"},
                          {"vrrm", 3.6},       {"if_avg", 0.2}};
    auto r2 = cross_reference("diode", bare_original, json::array({cands[0]}), Options{});
    const json tighter = r2["candidates"][0];
    CHECK(verdict_of(tighter, "vz_tolerance_pct") == UNVERIFIED);
    CHECK_FALSE(tighter.contains("notes"));
}
