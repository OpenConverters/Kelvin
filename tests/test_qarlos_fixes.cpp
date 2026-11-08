// Regression tests for the Qarlos cross-reference audit of 2026-10-24
// (ABT #2029-#2051). Each case reproduces the reported row in miniature.
#include <cmath>
#include <optional>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "CrossRef.hpp"
#include "Views.hpp"

using nlohmann::json;
using Catch::Approx;

namespace {
std::string verdict_of(const json& c, const std::string& name) {
    for (const auto& p : c.at("params"))
        if (p.value("name", "") == name) return p.value("verdict", "");
    return "";
}
bool has_note(const json& c, const std::string& needle) {
    if (!c.contains("notes")) return false;
    for (const auto& n : c["notes"])
        if (n.get<std::string>().find(needle) != std::string::npos) return true;
    return false;
}
}  // namespace

// ABT #2045: a nominal-only (or minimum-only) gate threshold is not a maximum.
TEST_CASE("qarlos #2045: vgs_threshold_max is read only from a stated maximum",
          "[qarlos][views]") {
    auto mos = [](json gtv) {
        return json{{"semiconductor", {{"mosfet", {{"manufacturerInfo",
            {{"name", "Infineon"}, {"datasheetInfo", {{"part", {{"partNumber", "BSC028N08NS5"},
                                                                 {"subType", "nChannel"}}},
                {"electrical", {{"drainSourceVoltage", 80.0}, {"continuousDrainCurrent", 100.0},
                                {"onResistance", 0.0028}, {"gateThresholdVoltage", gtv}}}}}}}}}}}};
    };
    auto nominal = kelvin::extract_mosfet(mos({{"nominal", 2.3}}));
    REQUIRE(nominal.has_value());
    CHECK(nominal->vgs_threshold_max == 0.0);  // unknown, not 2.3
    auto minimum = kelvin::extract_mosfet(mos({{"minimum", 1.2}}));
    REQUIRE(minimum.has_value());
    CHECK(minimum->vgs_threshold_max == 0.0);
    auto stated = kelvin::extract_mosfet(mos({{"minimum", 1.2}, {"nominal", 1.8}, {"maximum", 2.35}}));
    REQUIRE(stated.has_value());
    CHECK(stated->vgs_threshold_max == 2.35);
}

TEST_CASE("qarlos #2045: an Rds(on) with no stated Vgs against a stated one is flagged",
          "[qarlos][crossref]") {
    json orig = {{"mpn", "NVMFS4C306NT1G"}, {"vds", 30.0}, {"id", 100.0}, {"rds_on", 0.0034},
                 {"rds_on_vgs", 10.0}};
    json cand = {{"mpn", "BSC028N08NS5"}, {"vds", 80.0}, {"id", 100.0}, {"rds_on", 0.0028}};
    auto r = kelvin::crossref::cross_reference("mosfet", orig, json::array({cand}));
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "rds_on_basis") == "unverified");
    CHECK(has_note(c, "does not state the Vgs its Rds(on)"));
}

// ABT #2030: a three-token LxWxH case code reads its height from the third token.
TEST_CASE("qarlos #2030: LxWxH case codes give the third token as height",
          "[qarlos][dims]") {
    using kelvin::crossref::resolve_dimensions;
    auto a = resolve_dimensions("6.3x6.3x5.8", "capacitor");
    REQUIRE(a.has_value());
    CHECK(*a->length == Approx(6.3e-3));
    CHECK(*a->width == Approx(6.3e-3));
    CHECK(*a->height == Approx(5.8e-3));
    auto b = resolve_dimensions("8x8x11.7", "capacitor");
    REQUIRE(b.has_value());
    CHECK(*b->height == Approx(11.7e-3));
    // the two-token DxL can code still folds as before
    auto c = resolve_dimensions("6.3x11", "capacitor");
    REQUIRE(c.has_value());
    CHECK(*c->length == Approx(6.3e-3));
    CHECK(*c->height == Approx(11e-3));
}

// ABT #2031: a capacitor with an ESR curve and no scalar reads the curve at 100 kHz.
TEST_CASE("qarlos #2031: ESR is interpolated from esrPoints at 100 kHz", "[qarlos][views]") {
    auto cap = [](json esr_points) {
        json elec = {{"capacitance", {{"nominal", 2.2e-8}}}, {"ratedVoltage", 100.0}};
        if (!esr_points.is_null()) elec["esrPoints"] = esr_points;
        return json{{"capacitor", {{"manufacturerInfo", {{"name", "TDK"}, {"reference", "CGA4J2X7R2A223K125AE"},
            {"datasheetInfo", {{"part", {{"technology", "ceramic-class-2"}}}, {"electrical", elec}}}}}}}};
    };
    json curve = {{"xLabel", "Frequency [Hz]"}, {"yLabel", "ESR [Ohm]"},
                  {"xData", {1e3, 99983.69962, 104693.003, 1e6}},
                  {"yData", {48.0, 0.614709973, 0.592105985, 0.1}}};
    auto r = kelvin::extract_capacitor(cap(curve));
    REQUIRE(r.has_value());
    REQUIRE(kelvin::present(r->esr));
    CHECK(r->esr == Approx(0.6146).epsilon(1e-3));
    CHECK(r->esr_frequency == 1e5);
    // a curve that does not reach 100 kHz is not extrapolated
    json short_curve = {{"xData", {1e3, 1e4}}, {"yData", {48.0, 5.0}}};
    auto s = kelvin::extract_capacitor(cap(short_curve));
    REQUIRE(s.has_value());
    CHECK_FALSE(kelvin::present(s->esr));
    CHECK_FALSE(kelvin::present(s->esr_frequency));
}

// ABT #2032: an axial resistor lies flat — length is the length, diameter is W and H.
TEST_CASE("qarlos #2032: an axial resistor's diameter is its width and height", "[qarlos][views]") {
    json env = {{"resistor", {{"manufacturerInfo", {{"name", "Vishay"}, {"reference", "PTF6595K300BZBF"},
        {"datasheetInfo", {{"part", json::object()},
                           {"electrical", {{"resistance", 95300.0}, {"powerRating", 0.5}}},
                           {"mechanical", {{"length", 9.53e-3}, {"diameter", 3.68e-3}}}}}}}}}};
    auto r = kelvin::extract_resistor(env);
    REQUIRE(r.has_value());
    CHECK(r->length_m == Approx(9.53e-3));
    CHECK(r->width_m == Approx(3.68e-3));
    CHECK(r->height_m == Approx(3.68e-3));
}

// ABT #2039: a WARN on a directional parameter counts as a regression for direction.
TEST_CASE("qarlos #2039: a warned Isat makes the direction mixed, not upgrade",
          "[qarlos][crossref]") {
    json orig = {{"mpn", "AMELH6030S-R38MT"}, {"value_si", 0.38e-6}, {"saturation_current", 36.0},
                 {"rated_current", 29.0}, {"dcr", 0.0012}};
    json cand = {{"mpn", "SPM10054T-R47M-HZ"}, {"value_si", 0.38e-6}, {"saturation_current", 34.9},
                 {"rated_current", 36.0}, {"dcr", 0.0008}};
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    auto r = kelvin::crossref::cross_reference("magnetic", orig, json::array({cand}), opt);
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "saturation_current") == "warn");
    CHECK(c["direction"] == "mixed");
}

// ABT #2042: Isat at 30 % drop is not compared against Isat at 10 % drop as if equal.
TEST_CASE("qarlos #2042: Isat across different drop criteria warns and names both",
          "[qarlos][crossref][isat]") {
    json orig = {{"mpn", "744774218"}, {"value_si", 150e-6}, {"saturation_current", 0.5},
                 {"saturation_points", json::array({{{"percent_drop", 10.0}, {"current", 0.5}}})},
                 {"rated_current", 0.42}};
    json cand = {{"mpn", "VLS5045EX-151M"}, {"value_si", 150e-6}, {"saturation_current", 0.56},
                 {"saturation_points", json::array({{{"percent_drop", 30.0}, {"current", 0.56}}})},
                 {"rated_current", 0.42}};
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    auto r = kelvin::crossref::cross_reference("magnetic", orig, json::array({cand}), opt);
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "saturation_current") == "warn");
    CHECK(has_note(c, "different criteria"));
    CHECK(c["direction"] == "unknown");
    // the extractor records the criterion of the headline point
    json env = {{"magnetic", {{"manufacturerInfo", {{"name", "TDK"}, {"reference", "VLS5045EX-151M"},
        {"datasheetInfo", {{"electrical", json::array({{{"inductance", {{"nominal", 150e-6}}},
            {"saturationCurrentPeak", 0.56},
            {"saturationCurrents", json::array({{{"percentInductanceDrop", 30.0}, {"current", 0.56}}})}}})}}}}}}}};
    auto row = kelvin::extract_magnetic(env);
    REQUIRE(row.has_value());
    CHECK(row->saturation_percent_drop == 30.0);
}

// ABT #2029: a known original height against an unknown candidate height is not a drop-in.
TEST_CASE("qarlos #2029: unknown candidate height caps a magnetic at minor_review",
          "[qarlos][crossref][dims]") {
    json orig = {{"mpn", "LQH2HPH100MGR"}, {"value_si", 10e-6}, {"saturation_current", 0.6},
                 {"rated_current", 0.5}, {"length_m", 2.5e-3}, {"width_m", 2.0e-3},
                 {"height_m", 1.0e-3}};
    json cand = {{"mpn", "VLS252012HBX-100M-1"}, {"value_si", 10e-6}, {"saturation_current", 0.7},
                 {"rated_current", 0.6}, {"length_m", 2.5e-3}, {"width_m", 2.0e-3}};
    auto r = kelvin::crossref::cross_reference("magnetic", orig, json::array({cand}));
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "height") == "unverified");
    CHECK(c["grade"] == "minor_review");
    CHECK(has_note(c, "states no height"));
}

// ABT #2033 / #2046: a timing device's operating-temperature range is extracted and compared.
TEST_CASE("qarlos #2033/#2046: timing temperature range is extracted and gates",
          "[qarlos][views][crossref]") {
    json env = {{"timeBase", {{"oscillator", {{"manufacturerInfo", {{"name", "TXC CORPORATION"},
        {"reference", "7V-25.000MAKV-T"},
        {"datasheetInfo", {{"electrical", {{"frequency", 25e6}, {"technology", "quartzCrystal"}}},
                           {"thermal", {{"operatingTemperature", {{"minimum", -40.0}, {"maximum", 125.0}}}}}}}}}}}}}};
    auto row = kelvin::extract_timing(env);
    REQUIRE(row.has_value());
    CHECK(row->temp_min_c == -40.0);
    CHECK(row->temp_max_c == 125.0);

    json orig = {{"mpn", "7V-25.000MAKV-T"}, {"subtype", "oscillator"}, {"technology", "quartzCrystal"},
                 {"frequency", 25e6}, {"temp_min_C", -40.0}, {"temp_max_C", 125.0}};
    json narrow = {{"mpn", "FA-238 25.0000MD-AJ0"}, {"subtype", "oscillator"},
                   {"technology", "quartzCrystal"}, {"frequency", 25e6},
                   {"temp_min_C", -20.0}, {"temp_max_C", 70.0}};
    auto r = kelvin::crossref::cross_reference("timeBase", orig, json::array({narrow}));
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "temp_max_C") == "fail");
    CHECK(c["grade"] == "major_review");
    CHECK(c["direction"] == "downgrade");
    CHECK(has_note(c, "rated only to +70"));
}

// ── TVS / ESD (ABT #2034, #2036, #2038, #2040, #2041, #2047-#2051) ───────────
namespace {
json tvs_env(const std::string& mpn, json elec, json extra_di = json::object()) {
    json di = {{"part", {{"partNumber", mpn}, {"subType", "tvs"}}}, {"electrical", elec}};
    for (auto& [k, v] : extra_di.items()) di[k] = v;
    return {{"semiconductor", {{"diode", {{"manufacturerInfo",
        {{"name", "Littelfuse"}, {"reference", mpn}, {"datasheetInfo", di}}}}}}}};
}
json tvs(const std::string& mpn, double standoff) {
    return {{"mpn", mpn}, {"technology", "tvs"}, {"vrrm", standoff}};
}
}  // namespace

// #2047: a TVS's working rating is its standoff, never the reverseVoltage some importers
// filled with the breakdown maximum. The surge specs are extracted with their waveforms.
TEST_CASE("qarlos #2047: tvs vrrm is the standoff; surge specs carry their waveform",
          "[qarlos][views]") {
    auto row = kelvin::extract_diode(tvs_env("P6KE180A",
        {{"standoffVoltage", 180.0}, {"reverseVoltage", 221.0},
         {"peakPulsePower", 600.0}, {"peakPulsePowerWaveform", "10/1000"},
         {"peakPulseCurrent", 2.1}, {"clampingVoltage", 290.0}, {"clampingCurrent", 2.1},
         {"clampingWaveform", "10/1000"}, {"junctionCapacitance", 5e-11}}));
    REQUIRE(row.has_value());
    CHECK(row->vrrm_rated == 180.0);
    CHECK(row->peak_pulse_power == 600.0);
    CHECK(row->peak_pulse_power_waveform == "10/1000");
    CHECK(row->peak_pulse_current == 2.1);
    CHECK(row->peak_pulse_current_waveform.empty());
    CHECK(row->clamping_voltage == 290.0);
    CHECK(row->junction_capacitance == 5e-11);
    CHECK_FALSE(row->provenance_retracted);
}

// #2036 / #2041 / #2048: surge ratings are compared at equal waveforms and a shortfall
// is a regression; an unstated or different waveform is unverified and caps the grade.
TEST_CASE("qarlos #2036/#2041/#2048: peak pulse power compared at equal waveforms only",
          "[qarlos][crossref]") {
    json orig = tvs("15KP30A", 30.0);
    orig.update({{"peak_pulse_power", 15000.0}, {"peak_pulse_power_waveform", "10/1000"},
                 {"peak_pulse_current", 296.0}, {"peak_pulse_current_waveform", "10/1000"}});
    json weak = tvs("SMAJ30A", 30.0);
    weak.update({{"peak_pulse_power", 400.0}, {"peak_pulse_power_waveform", "10/1000"},
                 {"peak_pulse_current", 8.3}, {"peak_pulse_current_waveform", "10/1000"}});
    json other_wf = tvs("SMBJ30A", 30.0);
    other_wf.update({{"peak_pulse_power", 20000.0}, {"peak_pulse_power_waveform", "8/20"}});
    auto r = kelvin::crossref::cross_reference("diode", orig, json::array({weak, other_wf}));
    const json* w = nullptr; const json* o = nullptr;
    for (const auto& c : r["candidates"]) (c["mpn"] == "SMAJ30A" ? w : o) = &c;
    REQUIRE(w); REQUIRE(o);
    CHECK(verdict_of(*w, "peak_pulse_power") == "fail");
    CHECK(verdict_of(*w, "peak_pulse_current") == "fail");
    CHECK((*w)["grade"] == "major_review");
    CHECK((*w)["direction"] == "downgrade");
    CHECK((*w)["status"] != "no_substitute");  // a demotion, not a hard reject
    CHECK(has_note(*w, "peak pulse power 400 W against the original's 15000 W at 10/1000 us"));
    CHECK(verdict_of(*o, "peak_pulse_power") == "unverified");
    CHECK(verdict_of(*o, "peak_pulse_current") == "unverified");
    CHECK((*o)["grade"] == "major_review");
    CHECK((*o)["direction"] == "unknown");  // the surge axis went unweighed
    CHECK(has_note(*o, "different or unstated impulses"));
}

// #2034: a higher standoff is not an upgrade; a breakdown window above the original's warns.
TEST_CASE("qarlos #2034: higher standoff is no upgrade; breakdown window above warns",
          "[qarlos][crossref]") {
    json orig = tvs("SMBJ5.0A", 5.0);
    orig.update({{"vz_min_V", 6.4}, {"vz_max_V", 7.0}});
    json high = tvs("SMBJ12A", 12.0);
    high.update({{"vz_min_V", 13.3}, {"vz_max_V", 14.7}});
    auto r = kelvin::crossref::cross_reference("diode", orig, json::array({high}));
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "vrrm") == "pass");
    CHECK(verdict_of(c, "breakdown_window") == "warn");
    CHECK(c["direction"] == "downgrade");
    CHECK(c["grade"] != "drop_in");
    CHECK(has_note(c, "protection level rises"));
    // Without the breakdown data the standoff alone may not read as an upgrade either:
    // within the 10 % band it is equivalent, beyond it a WARN capped at minor_review.
    auto r2 = kelvin::crossref::cross_reference("diode", tvs("SMBJ5.0A", 5.0),
                                                json::array({tvs("SMBJ5.0B", 5.4)}));
    CHECK(r2["candidates"][0]["direction"] == "equivalent");
    CHECK(verdict_of(r2["candidates"][0], "standoff_band") == "");
}

TEST_CASE("qarlos #2034: standoff more than 10 % above the original's warns",
          "[qarlos][crossref]") {
    json orig = {{"mpn", "SP0518CAAT"}, {"technology", "esd"}, {"vrrm", 5.5}, {"case_code", "SOD-323"}};
    json cand = {{"mpn", "SD12T1G"}, {"technology", "esd"}, {"vrrm", 12.0}, {"case_code", "SOD-323"}};
    auto r = kelvin::crossref::cross_reference("diode", orig, json::array({cand}));
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "standoff_band") == "warn");
    CHECK(verdict_of(c, "vrrm") == "pass");
    CHECK(c["grade"] == "minor_review");
    CHECK(c["status"] != "no_substitute");
    CHECK(has_note(c, "standoff 12 V against the original's 5.5 V"));
}

// #2040 / #2049: clamping voltage compared at equal waveforms; #2050: Cj compared.
TEST_CASE("qarlos #2040/#2049/#2050: clamping voltage and junction capacitance are compared",
          "[qarlos][crossref]") {
    json orig = tvs("P4KE300A", 256.0);
    orig.update({{"clamping_voltage", 414.0}, {"clamping_waveform", "10/1000"},
                 {"junction_capacitance", 1.2e-12}});
    json cand = tvs("SMAJ300A", 300.0);
    cand.update({{"clamping_voltage", 602.0}, {"clamping_waveform", "10/1000"},
                 {"junction_capacitance", 9e-11}});
    auto r = kelvin::crossref::cross_reference("diode", orig, json::array({cand}));
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "clamping_voltage") == "fail");
    CHECK(verdict_of(c, "junction_capacitance") == "fail");
    CHECK(c["grade"] == "major_review");
    CHECK(c["direction"] == "downgrade");
    CHECK(has_note(c, "clamping voltage 602 V"));
    // An unstated clamping waveform leaves it unverified, without capping the grade.
    json nowf = tvs("SMAJ300A", 300.0);
    nowf["clamping_voltage"] = 300.0;
    auto r2 = kelvin::crossref::cross_reference("diode", orig, json::array({nowf}));
    CHECK(verdict_of(r2["candidates"][0], "clamping_voltage") == "unverified");
}

// #2038 / #2049: polarity decoded from the classic series order code.
TEST_CASE("qarlos #2038/#2049: unidirectional for bidirectional fails; unknown is unverified",
          "[qarlos][crossref]") {
    using kelvin::crossref::TvsPolarity;
    using kelvin::crossref::tvs_polarity_from_mpn;
    CHECK(tvs_polarity_from_mpn("15KP30CA") == TvsPolarity::Bi);
    CHECK(tvs_polarity_from_mpn("3KP210C") == TvsPolarity::Bi);
    CHECK(tvs_polarity_from_mpn("1.5KE30CA") == TvsPolarity::Bi);
    CHECK(tvs_polarity_from_mpn("SMBJ5.0CA-13-F") == TvsPolarity::Bi);
    CHECK(tvs_polarity_from_mpn("30KPA39A-HRA") == TvsPolarity::Uni);
    CHECK(tvs_polarity_from_mpn("SMAJ26A") == TvsPolarity::Uni);
    CHECK(tvs_polarity_from_mpn("SMBJ15-E3/52") == TvsPolarity::Uni);
    CHECK(tvs_polarity_from_mpn("PTVS3-380C-TH") == TvsPolarity::Unknown);
    CHECK(tvs_polarity_from_mpn("SM5S40ATHE3/I") == TvsPolarity::Unknown);

    json orig = tvs("15KP30CA", 30.0);
    auto r = kelvin::crossref::cross_reference(
        "diode", orig, json::array({tvs("SMAJ30A", 30.0), tvs("PTVS3-380C", 30.0),
                                    tvs("SMCJ30CA", 30.0)}));
    for (const auto& c : r["candidates"]) {
        if (c["mpn"] == "SMAJ30A") {
            CHECK(verdict_of(c, "polarity") == "fail");
            CHECK(c["grade"] == "major_review");
            CHECK(has_note(c, "unidirectional substitute for a bidirectional original"));
        } else if (c["mpn"] == "PTVS3-380C") {
            CHECK(verdict_of(c, "polarity") == "unverified");
            CHECK(has_note(c, "confirm it clamps both polarities"));
        } else {
            CHECK(verdict_of(c, "polarity") == "pass");
        }
    }
}

// #2035: a record whose every provenance entry is retracted verifies nothing.
TEST_CASE("qarlos #2035: all-retracted provenance is flagged and verifies nothing",
          "[qarlos][views][crossref]") {
    json retracted = json::array({{{"source", "manufacturerParametric"}, {"retracted", true}}});
    json mixed = json::array({{{"source", "a"}, {"retracted", true}}, {{"source", "b"}}});
    CHECK(kelvin::all_provenance_retracted(
        tvs_env("VLIN1626-02G", {{"standoffVoltage", 16.0}}, {{"provenance", retracted}})));
    CHECK_FALSE(kelvin::all_provenance_retracted(
        tvs_env("X", {{"standoffVoltage", 16.0}}, {{"provenance", mixed}})));
    CHECK_FALSE(kelvin::all_provenance_retracted(tvs_env("X", {{"standoffVoltage", 16.0}})));
    CHECK(kelvin::all_provenance_retracted(json{{"capacitor", {{"manufacturerInfo",
        {{"datasheetInfo", {{"provenance", retracted}}}}}}}}));

    json orig = {{"mpn", "SP0518CAAT"}, {"technology", "esd"}, {"vrrm", 5.5}, {"case_code", "SOD-323"}};
    json cand = {{"mpn", "VLIN1626-02G"}, {"technology", "esd"}, {"vrrm", 5.5},
                 {"case_code", "SOD-323"}, {"provenance_retracted", true}};
    auto r = kelvin::crossref::cross_reference("diode", orig, json::array({cand}));
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "vrrm") == "unverified");
    CHECK(c["grade"] == "major_review");
    CHECK(c["status"] == "partial");
    CHECK(c["direction"] == "unknown");
    CHECK(has_note(c, "retracted"));
    // As the original: the result is not a verified one.
    json o2 = cand; json c2 = orig;
    auto r2 = kelvin::crossref::cross_reference("diode", o2, json::array({c2}));
    CHECK(r2["original_verified"] == false);
    CHECK(r2["candidates"][0]["status"] == "partial");
}

// ── Qarlos audit of 2026-11-08: diodes ──────────────────────────────────────
namespace {
json find_mpn(const json& r, const std::string& mpn) {
    for (const auto& c : r.at("candidates"))
        if (c.value("mpn", "") == mpn) return c;
    FAIL("candidate " << mpn << " missing from the result");
    return json();
}
json zener(const std::string& mpn, std::optional<double> vz, std::optional<double> lo = std::nullopt,
           std::optional<double> hi = std::nullopt, std::optional<double> pd = std::nullopt) {
    json z = {{"mpn", mpn}, {"technology", "zener"}, {"case_code", "SOD-323"}};
    if (vz) z["vrrm"] = *vz;
    if (lo) z["vz_min_V"] = *lo;
    if (hi) z["vz_max_V"] = *hi;
    if (pd) z["power_dissipation"] = *pd;
    return z;
}
}  // namespace

// Run 3 seed 20264135 / run 4 seed 20265144: Vz was never compared, so a 24 V zener was
// drop_in/upgrade for TFZVTR22B (20.64-21.71 V) and 11-20 V zeners drop_in for a 9.1 V one.
TEST_CASE("qarlos 2026-11-08: a zener's Vz must lie inside the original's window",
          "[qarlos][crossref]") {
    // TFZVTR22B states only the band; vrrm is its midpoint.
    const json orig = zener("TFZVTR22B", 21.175, 20.64, 21.71, 0.5);
    auto r = kelvin::crossref::cross_reference(
        "diode", orig,
        json::array({zener("MM3Z24VT1G", 24.0, std::nullopt, std::nullopt, 0.5),
                     zener("SZMM3Z22VT1G", 22.0, std::nullopt, std::nullopt, 0.5),
                     zener("INSIDE", 21.2, 20.8, 21.6, 0.5),
                     zener("REACHES_OUT", 21.2, 20.1, 22.3, 0.5),
                     zener("NOMINAL_ONLY", 21.2, std::nullopt, std::nullopt, 0.5)}));
    for (const char* m : {"MM3Z24VT1G", "SZMM3Z22VT1G"}) {
        const json c = find_mpn(r, m);
        CHECK(c["status"] == "no_substitute");
        CHECK(verdict_of(c, "zener_voltage") == "fail");
        CHECK(has_note(c, "outside the original's 20.64-21.71 V"));
    }
    const json in = find_mpn(r, "INSIDE");
    CHECK(verdict_of(in, "zener_voltage") == "pass");
    CHECK(in["grade"] == "drop_in");
    CHECK(in["direction"] != "upgrade");
    const json out = find_mpn(r, "REACHES_OUT");
    CHECK(verdict_of(out, "zener_voltage") == "fail");
    CHECK(out["grade"] == "major_review");
    CHECK(has_note(out, "reaching outside the original's"));
    // an absent band is unknown, not a pass (and, per ABT #488, not a regression)
    CHECK(verdict_of(find_mpn(r, "NOMINAL_ONLY"), "zener_voltage") == "unverified");

    // BZT52-C9V1-Q: 9.1 V, 8.5-9.6 V.
    const json o2 = zener("BZT52-C9V1-Q", 9.1, 8.5, 9.6);
    auto r2 = kelvin::crossref::cross_reference(
        "diode", o2, json::array({zener("SZMMSZ11T1G", 11.0), zener("MMSZ20T1G", 20.0),
                                  zener("MMSZ9V1T1G", 9.1)}));
    CHECK(find_mpn(r2, "SZMMSZ11T1G")["status"] == "no_substitute");
    CHECK(find_mpn(r2, "MMSZ20T1G")["status"] == "no_substitute");
    CHECK(find_mpn(r2, "MMSZ9V1T1G")["status"] != "no_substitute");

    // A band-less original: the window is its Vz +/- 1 %.
    auto r3 = kelvin::crossref::cross_reference(
        "diode", zener("NOM", 9.1),
        json::array({zener("SAME", 9.1, 8.5, 9.6), zener("NEXT", 10.0, 9.4, 10.6)}));
    CHECK(verdict_of(find_mpn(r3, "SAME"), "zener_voltage") == "pass");
    CHECK(find_mpn(r3, "NEXT")["status"] == "no_substitute");

    // A substitute with no Vz at all is never passed nor left uncapped.
    auto r4 = kelvin::crossref::cross_reference("diode", o2,
                                                json::array({zener("NOVZ", std::nullopt)}));
    const json nv = r4["candidates"][0];
    CHECK(verdict_of(nv, "zener_voltage") != "pass");
    CHECK(nv["grade"] != "drop_in");
    CHECK(nv["grade"] != "minor_review");
}

// Run 3 seed 20264135: SZMM3Z22VT1G (0.3 W) graded drop_in for TFZVTR22B (0.5 W).
TEST_CASE("qarlos 2026-11-08: a lower diode power dissipation is a downgrade",
          "[qarlos][crossref][views]") {
    json env = {{"semiconductor", {{"diode", {{"manufacturerInfo",
        {{"name", "ROHM"}, {"reference", "TFZVTR22B"}, {"datasheetInfo",
            {{"part", {{"partNumber", "TFZVTR22B"}, {"subType", "zener"}}},
             {"electrical", {{"breakdownVoltage", {{"minimum", 20.64}, {"maximum", 21.71}}},
                             {"reverseVoltage", 17.0}, {"powerDissipation", 0.5}}}}}}}}}}}};
    auto row = kelvin::extract_diode(env);
    REQUIRE(row.has_value());
    CHECK(row->power_dissipation == 0.5);
    // a zener's vrrm is its Vz (the band midpoint here), never the leakage-test reverseVoltage
    CHECK(row->vrrm_rated == Approx(21.175));

    const json orig = zener("TFZVTR22B", 21.175, 20.64, 21.71, 0.5);
    auto r = kelvin::crossref::cross_reference(
        "diode", orig, json::array({zener("LOW_PD", 21.2, 20.8, 21.6, 0.3),
                                    zener("SAME_PD", 21.2, 20.8, 21.6, 0.5)}));
    const json low = find_mpn(r, "LOW_PD");
    CHECK(verdict_of(low, "power_dissipation") == "fail");
    CHECK(low["grade"] == "major_review");
    CHECK(low["direction"] == "downgrade");
    CHECK(verdict_of(find_mpn(r, "SAME_PD"), "power_dissipation") == "pass");
}

// Run 5 seed 20266153: SMCJ58CA (bidirectional) graded polarity 'pass' for SMBJ58A-Q.
TEST_CASE("qarlos 2026-11-08: bidirectional for unidirectional TVS fails",
          "[qarlos][crossref]") {
    auto r = kelvin::crossref::cross_reference(
        "diode", tvs("SMBJ58A-Q", 58.0),
        json::array({tvs("SMCJ58CA-M3/57T", 58.0), tvs("SMCJ58A", 58.0)}));
    const json bi = find_mpn(r, "SMCJ58CA-M3/57T");
    CHECK(verdict_of(bi, "polarity") == "fail");
    CHECK(bi["grade"] == "major_review");
    CHECK(has_note(bi, "bidirectional substitute for a unidirectional original"));
    CHECK(verdict_of(find_mpn(r, "SMCJ58A"), "polarity") == "pass");
}

// Run 4 seed 20265144: 15KPA066C-SD-Q (66 V standoff, Vbr min 66.4 V) minor_review for
// 15KP70 (70 V standoff).
TEST_CASE("qarlos 2026-11-08: a TVS standoff below the original's is no minor_review",
          "[qarlos][crossref]") {
    json orig = tvs("15KP70", 70.0);
    orig["vz_min_V"] = 77.8;
    json conducts = tvs("15KPA066C-SD-Q", 66.0);
    conducts.update({{"vz_min_V", 66.4}, {"vz_max_V", 80.0}});
    json lower = tvs("LOWER_STANDOFF", 66.0);
    lower.update({{"vz_min_V", 73.3}});
    auto r = kelvin::crossref::cross_reference("diode", orig, json::array({conducts, lower}));
    const json c = find_mpn(r, "15KPA066C-SD-Q");
    CHECK(c["status"] == "no_substitute");
    CHECK(has_note(c, "can conduct at the circuit's working voltage"));
    const json l = find_mpn(r, "LOWER_STANDOFF");
    CHECK(verdict_of(l, "standoff_band") == "fail");
    CHECK(l["grade"] == "major_review");
    CHECK(l["direction"] == "downgrade");
}

// Run 3 seed 20264135: 'DO-214AB' vs 'SMC (DO-214AB)' read as a different land pattern.
TEST_CASE("qarlos 2026-11-08: diode package aliases name the same outline",
          "[qarlos][crossref][dims]") {
    using kelvin::crossref::canonical_diode_case;
    CHECK(canonical_diode_case("SMC (DO-214AB)") == canonical_diode_case("DO-214AB"));
    CHECK(canonical_diode_case("DO-214AB (SMC)") == canonical_diode_case("SMC"));
    CHECK(canonical_diode_case("SMB (DO-214AA)") == canonical_diode_case("DO-214AA"));
    CHECK(canonical_diode_case("SMA") == canonical_diode_case("DO-214AC (SMA)"));
    CHECK(canonical_diode_case("DO-41 (DO-204AL)") == canonical_diode_case("DO-204AL"));
    CHECK(canonical_diode_case("MiniMELF (SOD-80)") == canonical_diode_case("SOD-80"));
    // different outlines stay different; unlisted variants are not guessed at
    CHECK(canonical_diode_case("SMC") != canonical_diode_case("SMB"));
    CHECK(canonical_diode_case("SMC-2") != canonical_diode_case("SMC"));
    CHECK(canonical_diode_case("SMB Flat") != canonical_diode_case("SMB"));
    CHECK(canonical_diode_case("SMC (DO-214AA)") != canonical_diode_case("SMC"));

    json orig = tvs("SMLJ36CA-QH", 36.0);
    orig["case_code"] = "DO-214AB";
    json same = tvs("3KASMC36AHE3_B/H", 36.0);
    same["case_code"] = "SMC (DO-214AB)";
    json other = tvs("SMBJ36CA", 36.0);
    other["case_code"] = "SMB (DO-214AA)";
    auto r = kelvin::crossref::cross_reference("diode", orig, json::array({same, other}));
    const json s = find_mpn(r, "3KASMC36AHE3_B/H");
    CHECK(s.value("footprint", "") != "different_case");
    CHECK_FALSE(has_note(s, "different package/case"));
    CHECK(find_mpn(r, "SMBJ36CA").value("footprint", "") == "different_case");
}
