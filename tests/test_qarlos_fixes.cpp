// Regression tests for the Qarlos cross-reference audit of 2026-10-24
// (ABT #2029-#2051). Each case reproduces the reported row in miniature.
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "CrossRef.hpp"
#include "Select.hpp"
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
    // Unknown, not 2.3 — and ABSENT (NaN), not the 0 V the row used to report (Qarlos
    // 2026-11-08).
    CHECK(std::isnan(nominal->vgs_threshold_max));
    auto minimum = kelvin::extract_mosfet(mos({{"minimum", 1.2}}));
    REQUIRE(minimum.has_value());
    CHECK(std::isnan(minimum->vgs_threshold_max));
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

// ── Qarlos audit of 2026-11-08 ───────────────────────────────────────────────

namespace {
json mosfet_env(const char* mpn, const char* channel, json elec) {
    json part = {{"partNumber", mpn}, {"subType", channel}};
    return json{{"semiconductor", {{"mosfet", {{"manufacturerInfo",
        {{"name", "Vendor"}, {"reference", mpn}, {"status", "production"},
         {"datasheetInfo", {{"part", part}, {"electrical", elec}}}}}}}}}};
}
}  // namespace

// A threshold with no stated worst-case bound, and a record with no Qg, are ABSENT in the
// row (NaN -> browse null -> ranker UNVERIFIED), never 0. Seen on SQSA82CENW (nominal 5 V
// only), BSC020N04LS (nominal 2 V), IGO60R040D1 (GaN, nominal only) and others in 5 of 5
// runs: the row reported vgs_threshold_max = 0 V.
TEST_CASE("qarlos 2026-11-08: absent threshold and Qg are absent in the MOSFET row, not 0",
          "[qarlos][views]") {
    json base = {{"drainSourceVoltage", 100.0}, {"continuousDrainCurrent", 20.0},
                 {"onResistance", 0.01}};
    json nominal = base;
    nominal["gateThresholdVoltage"] = {{"nominal", 5.0}};
    auto r = kelvin::extract_mosfet(mosfet_env("SQSA82CENW", "nChannel", nominal));
    REQUIRE(r.has_value());
    CHECK(std::isnan(r->vgs_threshold_max));
    CHECK(std::isnan(r->qg_total));           // no totalGateCharge key
    CHECK(r->evidence_incomplete());          // an absent Qg is still incomplete evidence
    json max_only = base;
    max_only["gateThresholdVoltage"] = {{"maximum", 3.5}};
    max_only["totalGateCharge"] = 4.2e-8;
    auto m = kelvin::extract_mosfet(mosfet_env("X", "nChannel", max_only));
    REQUIRE(m.has_value());
    CHECK(m->vgs_threshold_max == 3.5);
    CHECK(m->qg_total == Approx(4.2e-8));
    // P-channel nominal-only: no stated bound either.
    json p_nom = base;
    p_nom["drainSourceVoltage"] = -40.0;
    p_nom["gateThresholdVoltage"] = {{"nominal", -2.0}};
    auto p = kelvin::extract_mosfet(mosfet_env("SQJ409EP", "pChannel", p_nom));
    REQUIRE(p.has_value());
    CHECK(std::isnan(p->vgs_threshold_max));
}

// DMP1011LFV (AEC-Q101) against SQ4153EY, whose TAS record states no qualification: the
// substitute's grade is UNKNOWN, not "commercial".
TEST_CASE("qarlos 2026-11-08: a MOSFET with no stated qualification is unknown-grade, not commercial",
          "[qarlos][crossref]") {
    json orig = {{"mpn", "DMP1011LFV"}, {"vds", 12.0}, {"id", 10.0}, {"rds_on", 0.01},
                 {"technology", "Si"}, {"qualification", "Automotive (AEC-Q101)"}};
    json silent = {{"mpn", "SQ4153EY"}, {"vds", 12.0}, {"id", 25.0}, {"rds_on", 0.008},
                   {"technology", "Si"}, {"qualification", ""}};
    json market = silent;
    market["mpn"] = "MARKET1";
    market["qualification"] = "Automotive";
    json stated = silent;
    stated["mpn"] = "INDUSTRIAL1";
    stated["qualification"] = "Industrial";
    auto r = kelvin::crossref::cross_reference("mosfet", orig,
                                               json::array({silent, market, stated}));
    const json* cs = nullptr; const json* cm = nullptr; const json* ci = nullptr;
    for (const auto& c : r["candidates"]) {
        if (c["mpn"] == "SQ4153EY") cs = &c;
        if (c["mpn"] == "MARKET1") cm = &c;
        if (c["mpn"] == "INDUSTRIAL1") ci = &c;
    }
    REQUIRE(cs); REQUIRE(cm); REQUIRE(ci);
    CHECK(verdict_of(*cs, "automotive_grade") == "unverified");
    CHECK_FALSE(has_note(*cs, "substitute is commercial"));
    CHECK(has_note(*cs, "states no qualification"));
    CHECK((*cs)["status"] == "partial");     // not a drop-in on the record's word
    // "Automotive" names a market, not AEC-Q: unknown too, never "commercial".
    CHECK(verdict_of(*cm, "automotive_grade") == "unverified");
    CHECK_FALSE(has_note(*cm, "substitute is commercial"));
    // A STATED non-automotive grade is still a fail.
    CHECK(verdict_of(*ci, "automotive_grade") == "fail");
    CHECK(has_note(*ci, "substitute is commercial"));
}

// ── Qarlos audit of 2026-11-08: magnetics + capacitors ───────────────────────

// PL8132NLT (iNRCORE) states only a rated current (2.7 A); VLCF4020T-270MR48 states only an
// Isat (0.48 A). Neither like-for-like compare had both operands, so both axes came back
// unverified and the row was graded minor_review (run2 seed 20263126). A substitute that
// saturates below the original's rated current is saturated at a current the original is
// specified to carry: that is a regression, not an unknown.
TEST_CASE("qarlos 2026-11-08: Isat below the original's rated current fails the magnetic",
          "[qarlos][crossref][isat]") {
    // spec blocks exactly as web/src/crossref.js builds them from the two shard rows
    json orig = {{"mpn", "PL8132NLT"}, {"value_si", 2.91e-5}, {"rated_current", 2.7}};
    json cand = {{"mpn", "VLCF4020T-270MR48"}, {"value_si", 2.7e-5},
                 {"saturation_current", 0.48}, {"dcr", 0.496}};
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    auto r = kelvin::crossref::cross_reference("magnetic", orig, json::array({cand}), opt);
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "saturation_current") == "fail");
    CHECK(c["grade"] == "major_review");
    CHECK(c["direction"] == "downgrade");
    CHECK(has_note(c, "saturates below the original's rated current"));
    CHECK_FALSE(has_note(c, "could not be weighed"));

    // just under the rating (inside the 0.9 tolerance) warns
    json near = cand;
    near["saturation_current"] = 2.6;
    auto rn = kelvin::crossref::cross_reference("magnetic", orig, json::array({near}), opt);
    CHECK(verdict_of(rn["candidates"][0], "saturation_current") == "warn");

    // at or above the rating proves nothing about the original's own Isat: still unverified,
    // still direction-blind (the one-way rule is not an equivalence)
    json above = cand;
    above["saturation_current"] = 3.0;
    auto ra = kelvin::crossref::cross_reference("magnetic", orig, json::array({above}), opt);
    CHECK(verdict_of(ra["candidates"][0], "saturation_current") == "unverified");
    CHECK(has_note(ra["candidates"][0], "could not be weighed"));

    // the substitute is read at its most lenient stated criterion: a 10 % figure below the
    // rating does not fail a part whose 30 % figure clears it
    json pts = cand;
    pts["saturation_current"] = 2.0;
    pts["saturation_points"] = json::array({{{"percent_drop", 10.0}, {"current", 2.0}},
                                            {{"percent_drop", 30.0}, {"current", 2.9}}});
    auto rp = kelvin::crossref::cross_reference("magnetic", orig, json::array({pts}), opt);
    CHECK(verdict_of(rp["candidates"][0], "saturation_current") == "unverified");
}

// Run5 seed 20300704: BWVS00606045150T00 states Isat 3.06 A with no drop criterion;
// MSS6132T-123MLC states 1.3 / 1.5 / 1.7 A at 10 / 20 / 30 %. The 1.9x criterion band
// excused the 1.5 A headline, so the part was warned and graded minor_review. Wherever in
// the 10-30 % span the original's criterion lies, the substitute delivers at most 1.7 A.
TEST_CASE("qarlos 2026-11-08 r3: an unknown-basis Isat is held against the substitute's whole curve",
          "[qarlos][crossref][isat]") {
    json orig = {{"mpn", "BWVS00606045150T00"}, {"value_si", 1.5e-5}, {"saturation_current", 3.06},
                 {"rated_current", 1.98}, {"dcr", 0.08}};
    json mss = {{"mpn", "MSS6132T-123MLC"}, {"value_si", 1.2e-5}, {"saturation_current", 1.5},
                {"saturation_points", json::array({{{"percent_drop", 10.0}, {"current", 1.3}},
                                                   {{"percent_drop", 20.0}, {"current", 1.5}},
                                                   {{"percent_drop", 30.0}, {"current", 1.7}}})},
                {"rated_current", 1.98}, {"dcr", 0.079}};
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    auto r = kelvin::crossref::cross_reference("magnetic", orig, json::array({mss}), opt);
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "saturation_current") == "fail");
    CHECK(c["grade"] == "no_substitute");  // Isat is a hard gate for magnetics

    // A substitute whose 30 % figure clears the original keeps the band: still uncertain.
    json close = mss;
    close["mpn"] = "CLOSE";
    close["saturation_points"][2]["current"] = 2.9;
    auto rc = kelvin::crossref::cross_reference("magnetic", orig, json::array({close}), opt);
    CHECK(verdict_of(rc["candidates"][0], "saturation_current") == "warn");

    // A curve that stops short of 30 % does not bound the original's criterion: band rules.
    json short_curve = mss;
    short_curve["mpn"] = "SHORT";
    short_curve["saturation_points"] = json::array({{{"percent_drop", 10.0}, {"current", 1.7}}});
    auto rs = kelvin::crossref::cross_reference("magnetic", orig, json::array({short_curve}), opt);
    CHECK(verdict_of(rs["candidates"][0], "saturation_current") == "warn");

    // Mirror: a based original (10 % figure 3.06 A) against an unknown-basis 1.5 A substitute.
    json based = orig;
    based["saturation_points"] = json::array({{{"percent_drop", 10.0}, {"current", 3.06}}});
    json bare = {{"mpn", "BARE"}, {"value_si", 1.5e-5}, {"saturation_current", 1.5},
                 {"rated_current", 1.98}, {"dcr", 0.08}};
    auto rm = kelvin::crossref::cross_reference("magnetic", based, json::array({bare}), opt);
    CHECK(verdict_of(rm["candidates"][0], "saturation_current") == "fail");
}

// EEEHC0J221XP (Panasonic) states its range as thermal.operatingTemperature, the PEAS key;
// the extractor read only CAS's thermal.temperature, so the -40..105 C range was dropped and
// 85 C substitutes got no temperature verdict (run3 seed 20264135).
TEST_CASE("qarlos 2026-11-08: capacitor temperature range is read from operatingTemperature too",
          "[qarlos][views][crossref]") {
    auto cap = [](json thermal) {
        return json{{"capacitor", {{"manufacturerInfo", {{"name", "Panasonic"},
            {"reference", "EEEHC0J221XP"}, {"status", "production"},
            {"datasheetInfo", {{"part", {{"technology", "aluminum-electrolytic-wet"}}},
                {"electrical", {{"capacitance", {{"nominal", 220e-6}}}, {"ratedVoltage", 6.3}}},
                {"thermal", thermal}}}}}}}};
    };
    auto pea = kelvin::extract_capacitor(
        cap({{"operatingTemperature", {{"minimum", -40.0}, {"maximum", 105.0}}}}));
    REQUIRE(pea.has_value());
    CHECK(pea->temp_min_c == -40.0);
    CHECK(pea->temp_max_c == 105.0);

    auto cas = kelvin::extract_capacitor(cap({{"temperature", {{"minimum", -55.0}, {"maximum", 85.0}}}}));
    REQUIRE(cas.has_value());
    CHECK(cas->temp_min_c == -55.0);
    CHECK(cas->temp_max_c == 85.0);

    // both keys stating the same range: read once
    auto both = kelvin::extract_capacitor(
        cap({{"temperature", {{"minimum", -55.0}, {"maximum", 105.0}}},
             {"operatingTemperature", {{"minimum", -55.0}, {"maximum", 105.0}}}}));
    REQUIRE(both.has_value());
    CHECK(both->temp_min_c == -55.0);
    CHECK(both->temp_max_c == 105.0);

    // contradicting ends are not silently resolved; a stated end on one key only is kept
    auto clash = kelvin::extract_capacitor(
        cap({{"temperature", {{"maximum", 85.0}}},
             {"operatingTemperature", {{"minimum", -40.0}, {"maximum", 105.0}}}}));
    REQUIRE(clash.has_value());
    CHECK(clash->temp_min_c == -40.0);
    CHECK(std::isnan(clash->temp_max_c));

    // and the range now gates: an 85 C substitute for the 105 C original fails temp_max_C
    json orig = {{"mpn", "EEEHC0J221XP"}, {"value_si", 220e-6}, {"voltage", 6.3},
                 {"technology", "aluminum-electrolytic-wet"},
                 {"temp_min_C", pea->temp_min_c}, {"temp_max_C", pea->temp_max_c}};
    json sub = {{"mpn", "MAL202124221E3"}, {"value_si", 220e-6}, {"voltage", 6.3},
                {"technology", "aluminum-electrolytic-wet"}, {"temp_min_C", -40.0},
                {"temp_max_C", 85.0}};
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    auto r = kelvin::crossref::cross_reference("capacitor", orig, json::array({sub}), opt);
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "temp_max_C") == "fail");
    CHECK(c["direction"] == "downgrade");
}

namespace {
json xo(const std::string& mpn, std::optional<double> vmin, std::optional<double> vmax) {
    // The spec web/src/crossref.js builds for a timing row (supply_min_V / supply_max_V
    // from vsupply_min / vsupply_max, null when unstated).
    json o = {{"mpn", mpn}, {"subtype", "oscillator"}, {"technology", "crystalOscillator"},
              {"frequency", 644531250.0}, {"output_type", "lvds"}};
    o["supply_min_V"] = vmin ? json(*vmin) : json(nullptr);
    o["supply_max_V"] = vmax ? json(*vmax) : json(nullptr);
    return o;
}
const json* cand_by_mpn(const json& r, const std::string& mpn) {
    for (const auto& c : r["candidates"])
        if (c.value("mpn", "") == mpn) return &c;
    return nullptr;
}
}  // namespace

// EG-2121CA 644.53125M-LGPAL3 (EPSON, fixed 2.5 V) took 3.3 V and 1.8 V XOs as
// minor_review / upgrade (seed 20264135). The substitute's supply range must COVER the
// original's, with no warn band; unknown is unverified.
TEST_CASE("qarlos 2026-11-08: oscillator supply range must cover the original's",
          "[qarlos][crossref]") {
    const json orig = xo("EG-2121CA 644.53125M-LGPAL3", 2.5, 2.5);
    json cands = json::array({xo("ASG2-D-V-A-644.53125MHZ", 3.3, 3.3), xo("XO-18", 1.8, 1.8),
                              xo("XO-27", 2.7, 2.7), xo("XO-24", 2.4, 2.4),
                              xo("XO-WIDE", 2.25, 2.75),
                              xo("XO-NOSUPPLY", std::nullopt, std::nullopt)});
    auto r = kelvin::crossref::cross_reference("timeBase", orig, cands);

    const json* c33 = cand_by_mpn(r, "ASG2-D-V-A-644.53125MHZ");
    REQUIRE(c33);
    CHECK(verdict_of(*c33, "supply_min_V") == "fail");
    CHECK((*c33)["grade"] == "major_review");
    CHECK(has_note(*c33, "the original runs from 2.5 V, the substitute from 3.3 V"));

    const json* c18 = cand_by_mpn(r, "XO-18");
    REQUIRE(c18);
    CHECK(verdict_of(*c18, "supply_max_V") == "fail");
    CHECK((*c18)["grade"] == "major_review");

    // Inside the analog family's old 0.3 V / 10 % warn bands: still a different rail.
    const json* c27 = cand_by_mpn(r, "XO-27");
    REQUIRE(c27);
    CHECK(verdict_of(*c27, "supply_min_V") == "fail");
    const json* c24 = cand_by_mpn(r, "XO-24");
    REQUIRE(c24);
    CHECK(verdict_of(*c24, "supply_max_V") == "fail");

    const json* wide = cand_by_mpn(r, "XO-WIDE");
    REQUIRE(wide);
    CHECK(verdict_of(*wide, "supply_min_V") == "pass");
    CHECK(verdict_of(*wide, "supply_max_V") == "pass");
    CHECK_FALSE(has_note(*wide, "supply range"));

    const json* none = cand_by_mpn(r, "XO-NOSUPPLY");
    REQUIRE(none);
    CHECK(verdict_of(*none, "supply_min_V") == "unverified");
    CHECK(none->value("status", "") != "no_substitute");
    CHECK(has_note(*none, "states no supply range"));

    // A passive crystal has no supply: the keys are not compared even if present.
    json xtal = {{"mpn", "XTAL"}, {"subtype", "oscillator"}, {"technology", "quartzCrystal"},
                 {"frequency", 25e6}, {"supply_min_V", 2.5}, {"supply_max_V", 2.5}};
    json xtal2 = xtal;
    xtal2["mpn"] = "XTAL2";
    xtal2["supply_min_V"] = 3.3;
    xtal2["supply_max_V"] = 3.3;
    auto rx = kelvin::crossref::cross_reference("timeBase", xtal, json::array({xtal2}));
    CHECK(verdict_of(rx["candidates"][0], "supply_min_V") == "");
    CHECK(verdict_of(rx["candidates"][0], "supply_max_V") == "");
}

// FTMH-108-02-F-DH (Samtec, SMT) took 1-6450840-0 (TE, press-fit) with no mount verdict
// (seed 20262117). The board attach is a hard identity key; unknown is unverified.
TEST_CASE("qarlos 2026-11-08: connector board mount is compared and a change rejects",
          "[qarlos][crossref]") {
    auto con = [](const std::string& mpn, const char* mounting) {
        json o = {{"mpn", mpn}, {"family", "boardToBoard"}, {"positions", 16.0},
                  {"rated_current_A", 3.0}};
        o["mounting"] = mounting ? json(mounting) : json(nullptr);
        return o;
    };
    const json orig = con("FTMH-108-02-F-DH", "smt");
    auto r = kelvin::crossref::cross_reference(
        "connector", orig,
        json::array({con("1-6450840-0", "pressFit"), con("SAME", "smt"), con("UNSTATED", nullptr)}));
    const json* pf = cand_by_mpn(r, "1-6450840-0");
    REQUIRE(pf);
    CHECK(verdict_of(*pf, "mounting") == "fail");
    CHECK((*pf)["status"] == "no_substitute");
    CHECK(has_note(*pf, "board mount differs: smt -> pressFit"));
    const json* same = cand_by_mpn(r, "SAME");
    REQUIRE(same);
    CHECK(verdict_of(*same, "mounting") == "pass");
    const json* un = cand_by_mpn(r, "UNSTATED");
    REQUIRE(un);
    CHECK(verdict_of(*un, "mounting") == "unverified");
    CHECK(un->value("status", "") != "no_substitute");
    // Unknown board attach against a stated one is not a drop-in (WT16PRD8CD, same seed).
    CHECK((*same)["grade"] == "drop_in");
    CHECK((*un)["grade"] == "minor_review");
    CHECK(has_note(*un, "states no board mount"));
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
    // A Vz a little BELOW the original's marking but inside its window is a match, not a
    // shortfall: vrrm is no HigherBetter rating on a zener (SZMMSZ5251ET1G, 21.0 V, warned).
    auto rl = kelvin::crossref::cross_reference(
        "diode", orig, json::array({zener("LOWER_INSIDE", 20.9, 20.7, 21.1, 0.5)}));
    const json li = rl["candidates"][0];
    CHECK(verdict_of(li, "vrrm") == "pass");
    CHECK(verdict_of(li, "zener_voltage") == "pass");
    CHECK(li["grade"] == "drop_in");
    CHECK(li["direction"] == "equivalent");

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

// ── Qarlos audit 2026-11-08 (resistor family) ────────────────────────────────

// Run3 seed 20264135 / run5 seed 20266153: a chip resistor whose record states no height
// (Bourns CR0603-FX-1743ELF: case code only) had its height graded 'pass' from the case-code
// table's typical 0.45 mm against an original that states 0.45 mm.
TEST_CASE("qarlos 2026-11-08: a case-code table height is not the record's height",
          "[qarlos][crossref][dims]") {
    json orig = {{"mpn", "RC0603FR-07174KL"}, {"value_si", 174000.0}, {"power_rating", 0.1},
                 {"tolerance_pct", 1.0}, {"case_code", "0603"}, {"length_m", 1.6e-3},
                 {"width_m", 0.8e-3}, {"height_m", 0.45e-3}};
    json table_only = {{"mpn", "CR0603-FX-1743ELF"}, {"value_si", 174000.0}, {"power_rating", 0.1},
                       {"tolerance_pct", 1.0}, {"case_code", "0603"}};
    // Land stated, no height (the Panasonic ERJ shape): the axis used to vanish.
    json land_only = {{"mpn", "ERJ3EKF1743V"}, {"value_si", 174000.0}, {"power_rating", 0.1},
                      {"tolerance_pct", 1.0}, {"case_code", "0603"}, {"length_m", 1.6e-3},
                      {"width_m", 0.8e-3}};
    json stated = {{"mpn", "STATED"}, {"value_si", 174000.0}, {"power_rating", 0.1},
                   {"tolerance_pct", 1.0}, {"case_code", "0603"}, {"length_m", 1.6e-3},
                   {"width_m", 0.8e-3}, {"height_m", 0.45e-3}};
    kelvin::crossref::Options opt;
    opt.max_results = 10;
    auto r = kelvin::crossref::cross_reference("resistor", orig,
                                               json::array({table_only, land_only, stated}), opt);
    auto by = [&](const std::string& m) {
        for (const auto& c : r["candidates"])
            if (c["mpn"] == m) return c;
        return json(nullptr);
    };
    for (const char* m : {"CR0603-FX-1743ELF", "ERJ3EKF1743V"}) {
        const json c = by(m);
        REQUIRE(!c.is_null());
        CHECK(verdict_of(c, "height") == "unverified");
        CHECK(c["height_fit"] == "unknown");
        CHECK(c["grade"] != "drop_in");
        CHECK(has_note(c, "states no height"));
    }
    // The footprint still follows from the case code (the code IS the land).
    CHECK(verdict_of(by("CR0603-FX-1743ELF"), "footprint") == "pass");
    // A record that states its height is judged and can still be a drop-in.
    CHECK(verdict_of(by("STATED"), "height") == "pass");
    CHECK(by("STATED")["grade"] == "drop_in");
}

// The same rule on another family through the shared code: a SOT-23 MOSFET's JEDEC table
// height is not a record height either.
TEST_CASE("qarlos 2026-11-08: a package table height is not judged on a mosfet",
          "[qarlos][crossref][dims]") {
    json orig = {{"mpn", "O"}, {"vds", 30.0}, {"id", 3.0}, {"rds_on", 0.05}, {"case_code", "SOT-23"},
                 {"height_m", 1.0e-3}};
    json cand = {{"mpn", "C"}, {"vds", 30.0}, {"id", 3.0}, {"rds_on", 0.05}, {"case_code", "SOT-23"}};
    auto r = kelvin::crossref::cross_reference("mosfet", orig, json::array({cand}));
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "height") == "unverified");
    CHECK(c["grade"] != "drop_in");
}

// Run2 seed 20263126: RN50C5000FR36 is 0.05 W at 125 degC (flat to 125, zero at 175);
// CR0603-FX-5100ELF is 0.1 W, flat to 70 degC and zero at 125 degC. Power is compared at the
// original's rating temperature, where the thick-film part delivers nothing.
TEST_CASE("qarlos 2026-11-08: resistor power is compared at the original's rating temperature",
          "[qarlos][crossref]") {
    json orig = {{"mpn", "RN50C5000FR36"}, {"value_si", 500.0}, {"power_rating", 0.05},
                 {"tolerance_pct", 1.0}, {"power_rating_temperature", 125.0},
                 {"derating_temperature", {-65.0, 125.0, 175.0}},
                 {"derating_amplitude", {1.0, 1.0, 0.0}}};
    auto cand = [](const char* mpn, json extra) {
        json c = {{"mpn", mpn}, {"value_si", 510.0}, {"power_rating", 0.1}, {"tolerance_pct", 1.0}};
        c.update(extra);
        return c;
    };
    json cands = json::array({
        cand("CR0603-FX-5100ELF", {{"derating_temperature", {-55.0, 70.0, 125.0}},
                                   {"derating_amplitude", {1.0, 1.0, 0.0}}}),
        // 0.1 W flat to 70, zero at 155: at 125 degC it delivers 0.1 * 30/85 = 0.0353 W.
        cand("CURVE_155", {{"power_rating_temperature", 70.0},
                           {"derating_temperature", {-55.0, 70.0, 155.0}},
                           {"derating_amplitude", {1.0, 1.0, 0.0}}}),
        // Its rating holds to 125 degC by its own statement: full 0.1 W there.
        cand("RATED_125", {{"power_rating_temperature", 125.0}}),
        // Rated at 70 degC with no curve: nothing says what it delivers at 125.
        cand("RATED_70_NO_CURVE", {{"power_rating_temperature", 70.0}}),
        // Nothing about temperature at all.
        cand("NO_BASIS", json::object()),
        // Curve reaches 175 degC: at 125 degC 0.1 * (1 - 55/105) = 0.0476 W -> warn (>= 0.9x).
        cand("CURVE_175", {{"derating_temperature", {-55.0, 70.0, 175.0}},
                           {"derating_amplitude", {1.0, 1.0, 0.0}}}),
    });
    kelvin::crossref::Options opt;
    opt.max_results = 10;
    auto r = kelvin::crossref::cross_reference("resistor", orig, cands, opt);
    auto by = [&](const std::string& m) {
        for (const auto& c : r["candidates"])
            if (c["mpn"] == m) return c;
        return json(nullptr);
    };
    const json z = by("CR0603-FX-5100ELF");
    REQUIRE(!z.is_null());
    CHECK(verdict_of(z, "power_rating") == "fail");
    CHECK(has_note(z, "ends at 125 degC") == false);  // 125 is ON the curve: interpolated, 0 W
    CHECK(has_note(z, "delivers 0 W"));
    CHECK(z["grade"] == "major_review");

    CHECK(verdict_of(by("CURVE_155"), "power_rating") == "fail");
    CHECK(verdict_of(by("CURVE_175"), "power_rating") == "warn");
    CHECK(verdict_of(by("RATED_125"), "power_rating") == "pass");
    for (const char* m : {"RATED_70_NO_CURVE", "NO_BASIS"}) {
        const json c = by(m);
        REQUIRE(!c.is_null());
        CHECK(verdict_of(c, "power_rating") == "unverified");
        CHECK(c["grade"] != "drop_in");
        CHECK(has_note(c, "not comparable at the original's rating temperature"));
    }

    // Beyond the substitute's curve: not rated to operate there at all.
    json hot = orig;
    hot["power_rating_temperature"] = 150.0;
    auto r2 = kelvin::crossref::cross_reference("resistor", hot, json::array({cands[0]}), opt);
    CHECK(verdict_of(r2["candidates"][0], "power_rating") == "fail");
    CHECK(has_note(r2["candidates"][0], "ends at 125 degC"));

    // No basis for the substitute, and a nominal below the original's: at most 0.04 W at
    // 125 degC whatever its derating, so it is a FAIL and the downgrade counts.
    for (json low : {cand("NO_BASIS_LOW", json::object()),
                     cand("RATED_70_LOW", {{"power_rating_temperature", 70.0}})}) {
        low["power_rating"] = 0.04;
        auto rl = kelvin::crossref::cross_reference("resistor", orig, json::array({low}), opt);
        const json c = rl["candidates"][0];
        CHECK(verdict_of(c, "power_rating") == "fail");
        CHECK(c["grade"] == "major_review");
        CHECK(c["direction"] != "equivalent");
        CHECK(c["direction"] != "upgrade");
        CHECK(has_note(c, "power rating below the original's"));
    }

    // An original with no temperature basis keeps the plain nominal compare.
    json plain = {{"mpn", "P"}, {"value_si", 500.0}, {"power_rating", 0.05}, {"tolerance_pct", 1.0}};
    auto r3 = kelvin::crossref::cross_reference("resistor", plain, json::array({cands[4]}), opt);
    CHECK(verdict_of(r3["candidates"][0], "power_rating") == "pass");
}

TEST_CASE("qarlos 2026-11-08: the resistor extractor carries the power-rating basis",
          "[qarlos][views]") {
    auto env = [](json elec, json factors) {
        json di = {{"part", json::object()}, {"electrical", elec}};
        if (!factors.is_null()) di["factors"] = factors;
        return json{{"resistor", {{"manufacturerInfo",
            {{"name", "Vishay"}, {"reference", "RN50C5000FR36"}, {"datasheetInfo", di}}}}}};
    };
    auto r = kelvin::extract_resistor(env(
        {{"resistance", {{"nominal", 500.0}}}, {"powerRating", 0.05}, {"powerRatingTemperature", 125}},
        {{"powerDerating", {{"temperature", {-65, 125, 175}}, {"amplitude", {1, 1, 0}}}}}));
    REQUIRE(r.has_value());
    CHECK(r->power_rating_temperature == Approx(125.0));
    REQUIRE(r->derating_temperature.size() == 3);
    CHECK(r->derating_temperature[2] == Approx(175.0));
    CHECK(r->derating_amplitude[2] == Approx(0.0));

    auto bare = kelvin::extract_resistor(env({{"resistance", 500.0}, {"powerRating", 0.05}}, nullptr));
    REQUIRE(bare.has_value());
    CHECK(std::isnan(bare->power_rating_temperature));
    CHECK(bare->derating_temperature.empty());

    // A curve the ranker cannot read is loud, never half-read.
    CHECK_THROWS(kelvin::extract_resistor(env({{"resistance", 500.0}, {"powerRating", 0.05}},
        {{"powerDerating", {{"temperature", {125, 70}}, {"amplitude", {1, 0}}}}})));
}

// Seed 20264135 (full family set): MF1/2CC3573F is an axial through-hole resistor
// (assemblyType "tht", no case code); ERJS14F3573U is a 1210 chip with no assemblyType.
// The old pairwise rule fell back to BOTH package strings, found none on the axial part
// and graded the pair with no mount caveat.
TEST_CASE("qarlos 2026-11-08: an explicit tht against a bare chip case code is a mount FAIL",
          "[qarlos][crossref][dims]") {
    using kelvin::crossref::compare_mount;
    using kelvin::crossref::MountMatch;
    CHECK(compare_mount("tht", "", "", "1210") == MountMatch::Incompatible);
    CHECK(compare_mount("tht", "", "", "") == MountMatch::Unknown);
    CHECK(compare_mount("smt", "", "", "0603") == MountMatch::Same);

    json orig = {{"mpn", "MF1/2CC3573F"}, {"value_si", 357000.0}, {"power_rating", 0.5},
                 {"tolerance_pct", 1.0}, {"mount", "tht"}, {"length_m", 9e-3},
                 {"width_m", 3.5e-3}, {"height_m", 3.5e-3}};
    json chip = {{"mpn", "ERJS14F3573U"}, {"value_si", 357000.0}, {"power_rating", 0.5},
                 {"tolerance_pct", 1.0}, {"case_code", "1210"}, {"length_m", 3.2e-3},
                 {"width_m", 2.5e-3}};
    json silent = {{"mpn", "NO_MOUNT"}, {"value_si", 357000.0}, {"power_rating", 0.5},
                   {"tolerance_pct", 1.0}, {"length_m", 9e-3}, {"width_m", 3.5e-3},
                   {"height_m", 3.5e-3}};
    json axial = silent;
    axial["mpn"] = "AXIAL";
    axial["mount"] = "axial";
    kelvin::crossref::Options opt;
    opt.max_results = 10;
    auto r = kelvin::crossref::cross_reference("resistor", orig,
                                               json::array({chip, silent, axial}), opt);
    auto by = [&](const std::string& m) {
        for (const auto& c : r["candidates"])
            if (c["mpn"] == m) return c;
        return json(nullptr);
    };
    // The chip is never offered as a substitute for the axial part (rejected, or dropped).
    const json c = by("ERJS14F3573U");
    CHECK((c.is_null() || c["status"] == "no_substitute"));
    // A substitute that states no mount against a stated one: UNVERIFIED, capped.
    const json s = by("NO_MOUNT");
    REQUIRE(!s.is_null());
    CHECK(verdict_of(s, "mounting") == "unverified");
    CHECK(s["grade"] != "drop_in");
    CHECK(has_note(s, "mount type not established"));
    // Same mount class: no mount verdict, no cap from it.
    REQUIRE(!by("AXIAL").is_null());
    CHECK(verdict_of(by("AXIAL"), "mounting") == "");
}

// ── Qarlos audit of 2026-11-08, round 2: connectors and capacitors ─────────
namespace {
size_t count_params(const json& c, const std::string& name) {
    size_t n = 0;
    for (const auto& p : c.at("params"))
        if (p.value("name", "") == name) ++n;
    return n;
}
size_t count_notes(const json& c, const std::string& needle) {
    size_t n = 0;
    if (!c.contains("notes")) return 0;
    for (const auto& s : c["notes"])
        if (s.get<std::string>().find(needle) != std::string::npos) ++n;
    return n;
}
json fpc(const std::string& mpn, const char* orientation) {
    // The shape web/src/crossref.js sends: base()'s `mount` AND the connector's `mounting`.
    json o = {{"mpn", mpn},      {"family", "fpcFfc"},     {"positions", 24.0},
              {"pitch_mm", 0.5}, {"rated_current_A", 0.5}, {"mount", "smt"},
              {"mounting", "smt"}};
    o["orientation"] = orientation ? json(orientation) : json(nullptr);
    return o;
}
}  // namespace

// F32Q-1A7H1-11024 (Amphenol, right-angle ZIF FPC) took 2-2492142-4 and other VERTICAL FPC
// connectors with no orientation verdict or note (seed 20269028).
TEST_CASE("qarlos 2026-11-08: connector orientation is compared; a change rejects",
          "[qarlos][crossref]") {
    const json orig = fpc("F32Q-1A7H1-11024", "rightAngle");
    kelvin::crossref::Options opt;
    opt.max_results = 10;
    auto r = kelvin::crossref::cross_reference(
        "connector", orig,
        json::array({fpc("VERT", "vertical"), fpc("RA", "rightAngle"),
                     fpc("2-2492142-4", nullptr), fpc("HORIZ", "horizontal")}),
        opt);
    const json* vert = cand_by_mpn(r, "VERT");
    REQUIRE(vert);
    CHECK(verdict_of(*vert, "orientation") == "fail");
    CHECK((*vert)["status"] == "no_substitute");
    CHECK(has_note(*vert, "orientation differs: rightAngle -> vertical"));

    const json* ra = cand_by_mpn(r, "RA");
    REQUIRE(ra);
    CHECK(verdict_of(*ra, "orientation") == "pass");
    CHECK((*ra)["grade"] == "drop_in");

    // The real candidate states no orientation in its record: unverified, noted, capped.
    const json* un = cand_by_mpn(r, "2-2492142-4");
    REQUIRE(un);
    CHECK(verdict_of(*un, "orientation") == "unverified");
    CHECK(un->value("status", "") != "no_substitute");
    CHECK((*un)["grade"] != "drop_in");
    CHECK(has_note(*un,
                   "orientation not established (original: rightAngle, substitute: not stated)"));

    // horizontal vs rightAngle: not asserted either way (decision recorded in CrossRef.hpp).
    const json* hz = cand_by_mpn(r, "HORIZ");
    REQUIRE(hz);
    CHECK(verdict_of(*hz, "orientation") == "unverified");
    CHECK((*hz)["grade"] != "drop_in");
    CHECK(has_note(*hz, "confirm from the drawings"));

    // Neither record states one: the axis stays silent.
    auto r2 = kelvin::crossref::cross_reference("connector", fpc("A", nullptr),
                                                json::array({fpc("B", nullptr)}));
    CHECK(verdict_of(r2["candidates"][0], "orientation") == "");
}

TEST_CASE("qarlos 2026-11-08: the connector extractor carries mechanical.orientation",
          "[qarlos][views]") {
    auto env = [](json mech) {
        return json{{"connector",
                     {{"manufacturerInfo",
                       {{"name", "Amphenol CS"},
                        {"reference", "F32Q-1A7H1-11024"},
                        {"status", "production"},
                        {"datasheetInfo",
                         {{"part", {{"partNumber", "F32Q-1A7H1-11024"}}},
                          {"mechanical", mech}}}}}}}};
    };
    auto a = kelvin::extract_connector(env({{"positions", 24},
                                            {"pitch", 0.0005},
                                            {"orientation", "rightAngle"},
                                            {"mountingStyle", "smt"}}));
    REQUIRE(a.has_value());
    CHECK(a->orientation == "rightAngle");
    CHECK(a->mount == "smt");
    auto b = kelvin::extract_connector(env({{"positions", 24}, {"pitch", 0.0005}}));
    REQUIRE(b.has_value());
    CHECK(b->orientation.empty());
    // The contact pitch is the connector's own axis, never a lead pitch.
    CHECK(std::isnan(b->lead_pitch_m));
}

// 395337424 (Molex Eurostyle, 24 positions, 1 row, 5.08 mm) took 739-124 (WAGO, 24 positions,
// 5.00 mm) with pitch "pass", no note and no penalty (seed 20784754): the old 2 % relative
// band let 0.08 mm through, and over 23 pitches the end pin is 1.84 mm off its hole. The pin
// field must line up: (positions_per_row - 1) x |dpitch| <= 0.1 mm.
TEST_CASE("qarlos 2026-11-08: a connector pitch difference is judged at the end pin",
          "[qarlos][crossref]") {
    auto tb = [](const std::string& mpn, double pitch_mm, json positions, json rows) {
        // The shape web/src/crossref.js sends for a terminal block.
        json o = {{"mpn", mpn},           {"family", "terminalBlock"}, {"pitch_mm", pitch_mm},
                  {"rated_current_A", 18.0}, {"mount", nullptr},       {"mounting", nullptr}};
        o["positions"] = positions;
        o["rows"] = rows;
        return o;
    };
    const json orig = tb("395337424", 5.08, 24.0, 1.0);
    kelvin::crossref::Options opt;
    opt.max_results = 10;
    auto r = kelvin::crossref::cross_reference(
        "connector", orig,
        json::array({tb("739-124", 5.00, 24.0, nullptr), tb("SAME", 5.08, 24.0, nullptr)}), opt);

    const json* wago = cand_by_mpn(r, "739-124");
    REQUIRE(wago);
    CHECK(verdict_of(*wago, "pitch_mm") == "fail");
    CHECK((*wago)["footprint"] == "different_land_pattern");
    CHECK((*wago)["status"] == "partial");
    CHECK((*wago)["grade"] != "drop_in");
    CHECK(has_note(*wago, "pitch differs: 5.08 -> 5 mm"));
    CHECK(has_note(*wago, "over 24 positions per row the last contact sits 1.84 mm off its pad"));
    const json* same = cand_by_mpn(r, "SAME");
    REQUIRE(same);
    CHECK(verdict_of(*same, "pitch_mm") == "pass");
    CHECK(r["candidates"][0]["mpn"] == "SAME");

    // The same 0.08 mm on a 2-way part: 1 x 0.08 mm, inside 0.1 mm — the pin field lines up.
    auto r2 = kelvin::crossref::cross_reference(
        "connector", tb("O2", 5.08, 2.0, 1.0), json::array({tb("C2", 5.00, 2.0, nullptr)}), opt);
    CHECK(verdict_of(r2["candidates"][0], "pitch_mm") == "pass");

    // 24 positions in 2 rows of 12: 11 x 0.08 = 0.88 mm, still a different land pattern.
    auto r3 = kelvin::crossref::cross_reference(
        "connector", tb("O3", 5.08, 24.0, 2.0), json::array({tb("C3", 5.00, 24.0, 2.0)}), opt);
    CHECK(verdict_of(r3["candidates"][0], "pitch_mm") == "fail");
    CHECK(has_note(r3["candidates"][0], "over 12 positions per row the last contact sits 0.88 mm"));

    // No row count on either record: 24 positions may be one row (1.84 mm) or several; the
    // arrangement decides and nobody stated it -> unverified, noted, capped. Not a pass.
    auto r4 = kelvin::crossref::cross_reference(
        "connector", tb("O4", 5.08, 24.0, nullptr), json::array({tb("C4", 5.00, 24.0, nullptr)}),
        opt);
    const json& c4 = r4["candidates"][0];
    CHECK(verdict_of(c4, "pitch_mm") == "unverified");
    CHECK(c4["grade"] != "drop_in");
    CHECK(has_note(c4, "neither record states how the 24 positions are split into rows"));

    // No position count at all: a non-zero difference is unverified — two positions is not
    // assumed.
    auto r5 = kelvin::crossref::cross_reference(
        "connector", tb("O5", 5.08, nullptr, nullptr),
        json::array({tb("C5", 5.00, nullptr, nullptr)}), opt);
    const json& c5 = r5["candidates"][0];
    CHECK(verdict_of(c5, "pitch_mm") == "unverified");
    CHECK(c5["grade"] != "drop_in");
    CHECK(has_note(c5, "neither record states a position count"));

    // The extractor carries mechanical.rows into the row (v20).
    auto env = json{{"connector",
                     {{"manufacturerInfo",
                       {{"name", "Molex"},
                        {"reference", "395337424"},
                        {"status", "production"},
                        {"datasheetInfo",
                         {{"part", {{"partNumber", "395337424"}}},
                          {"mechanical", {{"positions", 24}, {"rows", 1}, {"pitch", 0.00508}}},
                          {"familyDetails", {{"family", "terminalBlock"}}}}}}}}}};
    auto row = kelvin::extract_connector(env);
    REQUIRE(row.has_value());
    CHECK(row->rows == Approx(1.0));
}

// Both records state mechanical.rows and they differ: a different pin field, whatever the
// pitch. One side only, or neither: rows alone give no verdict.
TEST_CASE("qarlos 2026-11-08: a connector with a different row count is a different pin field",
          "[qarlos][crossref]") {
    auto hdr = [](const std::string& mpn, json pitch_mm, json rows) {
        json o = {{"mpn", mpn},      {"family", "pinHeaderSocket"}, {"positions", 24.0},
                  {"rated_current_A", 3.0}, {"mount", nullptr},     {"mounting", nullptr}};
        o["pitch_mm"] = pitch_mm;
        o["rows"] = rows;
        return o;
    };
    kelvin::crossref::Options opt;
    opt.max_results = 10;
    auto r = kelvin::crossref::cross_reference(
        "connector", hdr("ORIG", 2.54, 1.0),
        json::array({hdr("TWO_ROWS", 2.54, 2.0), hdr("TWO_ROWS_NO_PITCH", nullptr, 2.0),
                     hdr("ONE_ROW", 2.54, 1.0), hdr("ROWS_UNSTATED", 2.54, nullptr)}),
        opt);
    const json* two = cand_by_mpn(r, "TWO_ROWS");
    REQUIRE(two);
    CHECK(verdict_of(*two, "pitch_mm") == "fail");
    CHECK((*two)["footprint"] == "different_land_pattern");
    CHECK((*two)["status"] == "partial");
    CHECK(has_note(*two, "contact rows differ: 1 -> 2 rows"));
    // The arrangement alone settles it, even with no pitch on the substitute's record.
    const json* np = cand_by_mpn(r, "TWO_ROWS_NO_PITCH");
    REQUIRE(np);
    CHECK((*np)["footprint"] == "different_land_pattern");
    CHECK((*np)["status"] == "partial");
    CHECK(has_note(*np, "contact rows differ: 1 -> 2 rows"));
    const json* one = cand_by_mpn(r, "ONE_ROW");
    REQUIRE(one);
    CHECK(verdict_of(*one, "pitch_mm") == "pass");
    CHECK_FALSE(one->contains("footprint"));
    const json* un = cand_by_mpn(r, "ROWS_UNSTATED");
    REQUIRE(un);
    CHECK(verdict_of(*un, "pitch_mm") == "pass");
    CHECK_FALSE(un->contains("footprint"));
    CHECK(count_notes(*un, "contact rows differ") == 0);
}

// 395337424 (Molex Eurostyle pluggable plug) took WAGO 739-124 fixed PCB strips with no
// form-factor note (seed 20784754). CONAS familyTerminalBlock.pluggable is the structured
// field; it is tri-state and an unstated one is never read as "fixed".
TEST_CASE("qarlos 2026-11-08: a pluggable terminal-block plug is not a fixed block",
          "[qarlos][crossref]") {
    auto tb = [](const std::string& mpn, json pluggable) {
        json o = {{"mpn", mpn},        {"family", "terminalBlock"}, {"positions", 24.0},
                  {"pitch_mm", 5.08},  {"rows", 1.0},               {"rated_current_A", 18.0},
                  {"mount", nullptr},  {"mounting", nullptr}};
        o["pluggable"] = pluggable;
        return o;
    };
    kelvin::crossref::Options opt;
    opt.max_results = 10;
    auto r = kelvin::crossref::cross_reference(
        "connector", tb("PLUG", true),
        json::array({tb("FIXED", false), tb("PLUG2", true), tb("UNSTATED", nullptr)}), opt);
    const json* fixed = cand_by_mpn(r, "FIXED");
    REQUIRE(fixed);
    CHECK(verdict_of(*fixed, "pluggable") == "fail");
    CHECK((*fixed)["status"] == "no_substitute");
    CHECK(has_note(*fixed, "terminal block form differs: pluggable -> fixed"));
    const json* plug2 = cand_by_mpn(r, "PLUG2");
    REQUIRE(plug2);
    CHECK(verdict_of(*plug2, "pluggable") == "pass");
    CHECK((*plug2)["grade"] == "drop_in");
    const json* un = cand_by_mpn(r, "UNSTATED");
    REQUIRE(un);
    CHECK(verdict_of(*un, "pluggable") == "unverified");
    CHECK(un->value("status", "") != "no_substitute");
    CHECK((*un)["grade"] != "drop_in");
    CHECK(has_note(*un, "pluggable vs fixed not established (original: pluggable, substitute: "
                        "not stated)"));
    // Neither record states it: silent.
    auto r2 = kelvin::crossref::cross_reference("connector", tb("A", nullptr),
                                                json::array({tb("B", nullptr)}), opt);
    CHECK(verdict_of(r2["candidates"][0], "pluggable") == "");

    // The extractor: tri-state, unstated stays empty (never "false").
    auto env = [](json fd) {
        return json{{"connector",
                     {{"manufacturerInfo",
                       {{"name", "Würth Elektronik"},
                        {"reference", "X"},
                        {"status", "production"},
                        {"datasheetInfo",
                         {{"part", {{"partNumber", "X"}}},
                          {"mechanical", {{"positions", 4}, {"pitch", 0.00508}}},
                          {"familyDetails", fd}}}}}}}};
    };
    auto t = kelvin::extract_connector(env({{"family", "terminalBlock"}, {"pluggable", true}}));
    auto f = kelvin::extract_connector(env({{"family", "terminalBlock"}, {"pluggable", false}}));
    auto n = kelvin::extract_connector(env({{"family", "terminalBlock"}}));
    REQUIRE((t && f && n));
    CHECK(t->pluggable == "true");
    CHECK(f->pluggable == "false");
    CHECK(n->pluggable.empty());
}

// Round-1 merge left connectors with TWO mount checks (a PARAM_SPECS 'mounting' key and the
// shared compare_mount gate): a connector whose substitute states no board attach came back
// with two 'mounting' verdicts and two notes. One gate now decides it.
TEST_CASE("qarlos 2026-11-08: a connector gets exactly one mount verdict and one note",
          "[qarlos][crossref]") {
    json orig = fpc("ORIG", "rightAngle");
    json none = fpc("NONE", "rightAngle");
    none.erase("mount");
    none["mounting"] = nullptr;
    json pf = fpc("PF", "rightAngle");
    pf["mount"] = "pressFit";
    pf["mounting"] = "pressFit";
    json tht = fpc("THT", "rightAngle");
    tht["mount"] = "tht";
    tht["mounting"] = "tht";
    kelvin::crossref::Options opt;
    opt.max_results = 10;
    auto r = kelvin::crossref::cross_reference(
        "connector", orig, json::array({none, pf, tht, fpc("SAME", "rightAngle")}), opt);
    const json* n = cand_by_mpn(r, "NONE");
    REQUIRE(n);
    CHECK(count_params(*n, "mounting") == 1);
    CHECK(verdict_of(*n, "mounting") == "unverified");
    CHECK(count_notes(*n, "mount") == 1);
    CHECK((*n)["grade"] == "minor_review");
    for (const char* m : {"PF", "THT"}) {
        const json* c = cand_by_mpn(r, m);
        REQUIRE(c);
        CHECK(count_params(*c, "mounting") == 1);
        CHECK(verdict_of(*c, "mounting") == "fail");
        CHECK((*c)["status"] == "no_substitute");
        CHECK(count_notes(*c, "mount") == 1);
        CHECK(has_note(*c, "board mount differs: smt -> "));
    }
    const json* same = cand_by_mpn(r, "SAME");
    REQUIRE(same);
    CHECK(count_params(*same, "mounting") == 1);
    CHECK(verdict_of(*same, "mounting") == "pass");
    // The connector board-attach classes are classes of the shared comparison itself.
    using kelvin::crossref::compare_mount;
    using kelvin::crossref::MountMatch;
    CHECK(compare_mount("smt", "pressFit", "", "") == MountMatch::Incompatible);
    CHECK(compare_mount("tht", "pressFit", "", "") == MountMatch::Incompatible);
    CHECK(compare_mount("panel", "cable", "", "") == MountMatch::Incompatible);
    CHECK(compare_mount("pressFit", "pressFit", "", "") == MountMatch::Same);
    CHECK(compare_mount("skedd", "", "", "") == MountMatch::Unknown);
}

// MKP4D034704D00MSSD (WIMA, PCM 15, two-hole land) took R60II34705030K (KEMET R60, no lead
// pitch, no land pattern on record) graded footprint 'pass' / 'fits' on the box size alone
// (seed 20276947). A leaded body's land is its lead pitch.
TEST_CASE("qarlos 2026-11-08: a leaded part's footprint is not a pass without its lead pitch",
          "[qarlos][crossref]") {
    auto film = [](const std::string& mpn, std::optional<double> pitch, double w) {
        json c = {{"mpn", mpn},        {"value_si", 4.7e-7}, {"voltage", 250.0},
                  {"mount", "THT"},    {"length_m", 18e-3},  {"width_m", w},
                  {"height_m", 12e-3}, {"technology", "film-polypropylene"}};
        c["lead_pitch_m"] = pitch ? json(*pitch) : json(nullptr);
        return c;
    };
    const json orig = film("MKP4D034704D00MSSD", 15e-3, 7e-3);
    kelvin::crossref::Options opt;
    opt.max_results = 10;
    auto r = kelvin::crossref::cross_reference(
        "capacitor", orig,
        json::array({film("R60II34705030K", std::nullopt, 6e-3), film("PCM15", 15e-3, 6e-3),
                     film("PCM10", 10e-3, 6e-3)}),
        opt);
    const json* un = cand_by_mpn(r, "R60II34705030K");
    REQUIRE(un);
    CHECK(verdict_of(*un, "footprint") == "unverified");
    CHECK((*un)["footprint"] == "unknown");
    CHECK((*un)["grade"] != "drop_in");
    CHECK(has_note(*un, "lead pitch"));
    CHECK(has_note(*un, "substitute: not stated"));

    const json* same = cand_by_mpn(r, "PCM15");
    REQUIRE(same);
    CHECK(verdict_of(*same, "footprint") == "pass");

    const json* other = cand_by_mpn(r, "PCM10");
    REQUIRE(other);
    CHECK(verdict_of(*other, "footprint") == "fail");
    CHECK((*other)["footprint"] == "different_land_pattern");
    CHECK(has_note(*other, "lead pitch differs"));

    // Two equal case codes name the hole pattern: established without a pitch.
    json a = film("A", std::nullopt, 7e-3), b = film("B", std::nullopt, 7e-3);
    a["case_code"] = "PCM 15";
    b["case_code"] = "PCM 15";
    auto rc = kelvin::crossref::cross_reference("capacitor", a, json::array({b}));
    CHECK(verdict_of(rc["candidates"][0], "footprint") == "pass");

    // An SMD pair is untouched: its land is its body.
    json s1 = film("S1", std::nullopt, 7e-3), s2 = film("S2", std::nullopt, 7e-3);
    s1["mount"] = "SMT";
    s2["mount"] = "SMT";
    auto rs = kelvin::crossref::cross_reference("capacitor", s1, json::array({s2}));
    CHECK(verdict_of(rs["candidates"][0], "footprint") == "pass");
}

TEST_CASE("qarlos 2026-11-08: the lead pitch is read from dimensions.pitch or a two-hole land",
          "[qarlos][views]") {
    auto cap = [](json mech) {
        return json{
            {"capacitor",
             {{"manufacturerInfo",
               {{"name", "WIMA"},
                {"reference", "MKP4D034704D00MSSD"},
                {"status", "production"},
                {"datasheetInfo",
                 {{"part",
                   {{"partNumber", "MKP4D034704D00MSSD"}, {"technology", "film-polypropylene"}}},
                  {"electrical",
                   {{"capacitance", {{"nominal", 4.7e-7}}}, {"ratedVoltage", 100.0}}},
                  {"mechanical", mech}}}}}}}};
    };
    const json shape = {{"assembly", "THT"}};
    auto a = kelvin::extract_capacitor(
        cap({{"shape", shape},
             {"dimensions", {{"length", {{"nominal", 0.018}}}, {"pitch", {{"nominal", 0.015}}}}}}));
    REQUIRE(a.has_value());
    CHECK(a->lead_pitch_m == Approx(0.015));
    const json drilled = json::array({{{"id", "1"}, {"x", -0.0075}, {"y", 0.0}, {"drill", 0.0011}},
                                      {{"id", "2"}, {"x", 0.0075}, {"y", 0.0}, {"drill", 0.0011}}});
    auto b = kelvin::extract_capacitor(cap({{"shape", shape},
                                            {"dimensions", {{"length", {{"nominal", 0.018}}}}},
                                            {"landPattern", {{"pads", drilled}}}}));
    REQUIRE(b.has_value());
    CHECK(b->lead_pitch_m == Approx(0.015));
    // Undrilled (SMD) pads are not a lead pitch; nothing stated stays NaN.
    const json flat = json::array({{{"x", -0.0075}, {"y", 0.0}}, {{"x", 0.0075}, {"y", 0.0}}});
    auto c = kelvin::extract_capacitor(cap({{"shape", shape},
                                            {"dimensions", {{"length", {{"nominal", 0.018}}}}},
                                            {"landPattern", {{"pads", flat}}}}));
    REQUIRE(c.has_value());
    CHECK(std::isnan(c->lead_pitch_m));
}


// Owner's rule (2026-11-09): a connector's board attach is an identity of the part and is
// compared even when the caller switches the footprint check off — one verdict, one note.
TEST_CASE("qarlos 2026-11-08: connector mount is compared with check_footprint off",
          "[qarlos][crossref]") {
    json orig = fpc("SMT", "rightAngle");
    json pf = fpc("PF", "rightAngle");
    pf["mount"] = "pressFit";
    pf["mounting"] = "pressFit";
    kelvin::crossref::Options opt;
    opt.max_results = 10;
    opt.check_footprint = false;
    auto r = kelvin::crossref::cross_reference("connector", orig,
                                               json::array({pf, fpc("SAME", "rightAngle")}), opt);
    const json* c = cand_by_mpn(r, "PF");
    REQUIRE(c);
    CHECK(count_params(*c, "mounting") == 1);
    CHECK(verdict_of(*c, "mounting") == "fail");
    CHECK((*c)["status"] == "no_substitute");
    CHECK(count_notes(*c, "mount") == 1);
    CHECK(has_note(*c, "board mount differs: smt -> pressFit"));
    const json* same = cand_by_mpn(r, "SAME");
    REQUIRE(same);
    CHECK(count_params(*same, "mounting") == 1);
    CHECK(verdict_of(*same, "mounting") == "pass");
    // Other families keep the mount inside the footprint gate: off means not compared.
    json rorig = {{"mpn", "O"}, {"value_si", 1e-7}, {"voltage", 50.0}, {"mount", "THT"}};
    json rsmd = {{"mpn", "S"}, {"value_si", 1e-7}, {"voltage", 50.0}, {"mount", "SMT"}};
    auto rc = kelvin::crossref::cross_reference("capacitor", rorig, json::array({rsmd}), opt);
    CHECK(verdict_of(rc["candidates"][0], "mounting") == "");
}

// ── Qarlos audit of 2026-11-08 round 2: dual-diode current basis ─────────────
namespace {
json rect(const std::string& mpn, double if_avg, const std::string& dc = "",
          const std::string& ic = "") {
    json d = {{"mpn", mpn}, {"technology", "schottky"}, {"case_code", "ITO-220AB"},
              {"vrrm", 60.0}, {"if_avg", if_avg}, {"vf", 0.65}};
    d["die_configuration"] = dc.empty() ? json() : json(dc);
    d["internal_connection"] = ic.empty() ? json() : json(ic);
    return d;
}
}  // namespace

// Run 4 seed 20292785: VFT2060C-M3/4W (dual common-cathode, forwardCurrent 10 A per leg)
// against MBRF1560CT (record states no die configuration; 15 A is the vendor table's
// whole-device figure). if_avg was graded 'pass' on per-leg vs whole-device numbers.
TEST_CASE("qarlos 2026-11-08: a dual diode's per-leg current is compared like with like",
          "[qarlos][crossref][views]") {
    // Extraction carries the stated basis verbatim; absent stays empty.
    json env = {{"semiconductor", {{"diode", {{"manufacturerInfo",
        {{"name", "Vishay"}, {"reference", "VFT2060C-M3/4W"}, {"datasheetInfo",
            {{"part", {{"partNumber", "VFT2060C-M3/4W"}, {"subType", "schottky"},
                       {"dieConfiguration", "dual"}, {"internalConnection", "commonCathode"}}},
             {"electrical", {{"reverseVoltage", 60.0}, {"forwardCurrent", 10.0},
                             {"forwardVoltage", 0.65}}}}}}}}}}}};
    auto row = kelvin::extract_diode(env);
    REQUIRE(row.has_value());
    CHECK(row->die_configuration == "dual");
    CHECK(row->internal_connection == "commonCathode");
    CHECK(row->if_avg_rated == 10.0);
    json& part = env["semiconductor"]["diode"]["manufacturerInfo"]["datasheetInfo"]["part"];
    part.erase("dieConfiguration");
    part.erase("internalConnection");
    auto bare = kelvin::extract_diode(env);
    REQUIRE(bare.has_value());
    CHECK(bare->die_configuration.empty());
    CHECK(bare->internal_connection.empty());

    const json orig = rect("VFT2060C-M3/4W", 10.0, "dual", "commonCathode");
    auto r = kelvin::crossref::cross_reference(
        "diode", orig,
        json::array({rect("MBRF1560CT", 15.0),
                     rect("DUAL_PER_LEG_10", 10.0, "dual", "commonCathode"),
                     rect("DUAL_PER_LEG_5", 5.0, "dual", "commonCathode")}));
    // Unknown basis on the substitute: never a pass, the grade is capped, no reassuring
    // direction, and the note says why.
    const json mb = find_mpn(r, "MBRF1560CT");
    CHECK(verdict_of(mb, "if_avg") == "unverified");
    CHECK(mb["grade"] == "major_review");
    CHECK(mb["direction"] == "unknown");
    CHECK(mb["status"] != "no_substitute");
    CHECK(has_note(mb, "forward current not comparable"));
    CHECK(has_note(mb, "states no die configuration"));
    // Both per leg: compared as usual.
    CHECK(verdict_of(find_mpn(r, "DUAL_PER_LEG_10"), "if_avg") == "pass");
    CHECK(verdict_of(find_mpn(r, "DUAL_PER_LEG_5"), "if_avg") == "fail");
    // Settled under every reading (same seed, PMEG6010CEJ,115): 1 A is below 10 A per leg
    // even read as per leg, so it FAILS rather than hiding behind 'unverified'.
    auto r1 = kelvin::crossref::cross_reference("diode", orig,
                                                json::array({rect("PMEG6010CEJ,115", 1.0)}));
    CHECK(verdict_of(r1["candidates"][0], "if_avg") == "fail");
    CHECK_FALSE(has_note(r1["candidates"][0], "forward current not comparable"));

    // The mirror image: an original stating no configuration against a stated dual.
    auto r2 = kelvin::crossref::cross_reference(
        "diode", rect("UNSTATED", 10.0),
        json::array({rect("DUAL_15", 15.0, "dual", "commonCathode"),
                     rect("DUAL_8", 8.0, "dual", "commonCathode"), rect("PLAIN_8", 8.0)}));
    // 15 A per leg meets even the strictest reading of the original's 10 A: a pass.
    CHECK(verdict_of(find_mpn(r2, "DUAL_15"), "if_avg") == "pass");
    // 8 A per leg against 10 A that may be per leg or the whole device: unknown.
    CHECK(verdict_of(find_mpn(r2, "DUAL_8"), "if_avg") == "unverified");
    CHECK(find_mpn(r2, "DUAL_8")["grade"] == "major_review");
    CHECK(has_note(find_mpn(r2, "DUAL_8"), "the original's 10 A (its record states no die"));
    // Neither side multi-die: the schema's one-die reading, compared as stated.
    CHECK(verdict_of(find_mpn(r2, "PLAIN_8"), "if_avg") == "warn");
}

// Round-2 follow-up: a dual common-cathode rectifier is not a common-anode one, a series
// pair or a single diode. A stated mismatch rejects; a one-sided statement is unverified
// and keeps the row off drop_in; silence on both sides stays silent.
TEST_CASE("qarlos 2026-11-08: diode package topology must match where both records state it",
          "[qarlos][crossref]") {
    const json orig = rect("VFT2060C-M3/4W", 10.0, "dual", "commonCathode");
    auto r = kelvin::crossref::cross_reference(
        "diode", orig,
        json::array({rect("SAME_TOPO", 10.0, "dual", "commonCathode"),
                     rect("COMMON_ANODE", 10.0, "dual", "commonAnode"),
                     rect("SERIES_PAIR", 10.0, "dual", "series"),
                     rect("SINGLE_DIE", 10.0, "single"),
                     rect("NO_CONNECTION", 10.0, "dual")}));
    const json same = find_mpn(r, "SAME_TOPO");
    CHECK(verdict_of(same, "die_configuration") == "pass");
    CHECK(verdict_of(same, "internal_connection") == "pass");
    CHECK(same["grade"] == "drop_in");

    const json ca = find_mpn(r, "COMMON_ANODE");
    CHECK(ca["status"] == "no_substitute");
    CHECK(verdict_of(ca, "internal_connection") == "fail");
    CHECK(has_note(ca, "the original is 'commonCathode', the substitute 'commonAnode'"));
    CHECK(find_mpn(r, "SERIES_PAIR")["status"] == "no_substitute");

    const json single = find_mpn(r, "SINGLE_DIE");
    CHECK(single["status"] == "no_substitute");
    CHECK(verdict_of(single, "die_configuration") == "fail");
    CHECK(has_note(single, "the original is 'dual', the substitute 'single'"));

    // Configuration matches, connection stated on one side only: unverified, below drop_in,
    // but not a regression.
    const json nc = find_mpn(r, "NO_CONNECTION");
    CHECK(nc["status"] != "no_substitute");
    CHECK(verdict_of(nc, "internal_connection") == "unverified");
    CHECK(nc["grade"] == "minor_review");
    CHECK(has_note(nc, "internal connection 'commonCathode' and the substitute's states none"));

    // Neither side states either field: no topology verdict at all, drop_in stays reachable.
    auto r2 = kelvin::crossref::cross_reference("diode", rect("PLAIN_A", 10.0),
                                                json::array({rect("PLAIN_B", 10.0)}));
    const json pb = r2["candidates"][0];
    CHECK(verdict_of(pb, "die_configuration") == "");
    CHECK(verdict_of(pb, "internal_connection") == "");
    CHECK(pb["grade"] == "drop_in");
}
// ── Resistors, Qarlos audit of 2026-11-08 (round 2; seeds 20269028, 20276947, 20284866,
// 20292785, 20300704). Each spec below is what web/src/crossref.js builds from the v19 row of
// the named TAS record (values copied from resistors.ndjson).
namespace {
json res_spec(const std::string& mpn, double r_ohm, double p_w, double tol_pct, json extra) {
    json o = {{"mpn", mpn}, {"value_si", r_ohm}, {"power_rating", p_w}, {"tolerance_pct", tol_pct}};
    o.update(extra);
    return o;
}
json res_by(const json& r, const std::string& mpn) {
    for (const auto& c : r["candidates"])
        if (c["mpn"] == mpn) return c;
    return json(nullptr);
}
kelvin::crossref::Options res_opt() {
    kelvin::crossref::Options opt;
    opt.max_results = 10;
    opt.check_footprint = false;  // the footprint cases below turn it back on
    return opt;
}
}  // namespace

TEST_CASE("qarlos 2026-11-08 r2: the resistor extractor carries technology, TCR, range, function",
          "[qarlos][views]") {
    auto env = [](json part, json mi_extra, json thermal) {
        json di = {{"part", part},
                   {"electrical", {{"resistance", {{"nominal", 4.7}}}, {"tolerance", 0.2},
                                   {"powerRating", 0.1}, {"temperatureCoefficient", 400}}}};
        if (!thermal.is_null()) di["thermal"] = thermal;
        json mi = {{"name", "KOA Speer"}, {"reference", "SG731JTTD4R7M"}, {"datasheetInfo", di}};
        mi.update(mi_extra);
        return json{{"resistor", {{"manufacturerInfo", mi}}}};
    };
    // SG731JTTD4R7M: the anti-pulse class is stated only in part.description.
    auto sg = kelvin::extract_resistor(
        env({{"partNumber", "SG731JTTD4R7M"}, {"series", "SG731JTTD"}, {"technology", "thickFilm"},
             {"case", "0603"}, {"description", "Anti Pulse Chip Resistors"}},
            json::object(), {{"operatingTemperature", {{"minimum", -55}, {"maximum", 155}}}}));
    REQUIRE(sg.has_value());
    CHECK(sg->technology == "thickFilm");
    CHECK(sg->tcr == Approx(400.0));
    CHECK(sg->temp_min_c == Approx(-55.0));
    CHECK(sg->temp_max_c == Approx(155.0));
    CHECK(sg->pulse_withstanding);
    CHECK_FALSE(sg->current_sense);
    // ERJ6DQJ1R5V: current sensing from the technology enum.
    auto erj = kelvin::extract_resistor(
        env({{"partNumber", "ERJ6DQJ1R5V"}, {"technology", "currentSenseShunt"}},
            {{"family", "Current Sensing Resistors Thick Film Type"}}, nullptr));
    REQUIRE(erj.has_value());
    CHECK(erj->current_sense);
    CHECK(std::isnan(erj->temp_max_c));
    // Panasonic ERJ-P: "Anti-Surge" in the family string, hyphenated.
    CHECK(kelvin::resistor_function_class("thickFilm",
                                          {"Anti-Surge High Power Thick Film Chip Resistors"})
              .pulse_withstanding);
    // A general-purpose chip states neither; the MPN is never read.
    auto aa = kelvin::resistor_function_class("thickFilm", {"AA"});
    CHECK_FALSE(aa.pulse_withstanding);
    CHECK_FALSE(aa.current_sense);
}

// Run2 RNC55K1472FSB14 (-65..175 degC) took MF0204FTE52-14K7 (-55..155 degC) as a clean
// pass; run5 M55342H12B20E0PT5 (150 degC) took RN73H1JTTD2002B05 (125 degC) as minor_review.
TEST_CASE("qarlos 2026-11-08 r2: a resistor's operating range must cover the original's",
          "[qarlos][crossref]") {
    json orig = res_spec("RNC55K1472FSB14", 14700.0, 0.125, 1.0,
                         {{"technology", "metalFilm"}, {"temp_min_C", -65.0}, {"temp_max_C", 175.0}});
    json mf0 = res_spec("MF0204FTE52-14K7", 14700.0, 0.4, 1.0,
                        {{"technology", "metalFilm"}, {"temp_min_C", -65.0}, {"temp_max_C", 155.0}});
    json silent = res_spec("NO_RANGE", 14700.0, 0.4, 1.0, {{"technology", "metalFilm"}});
    json same = res_spec("SAME_RANGE", 14700.0, 0.125, 1.0,
                         {{"technology", "metalFilm"}, {"temp_min_C", -65.0}, {"temp_max_C", 175.0}});
    auto r = kelvin::crossref::cross_reference("resistor", orig, json::array({mf0, silent, same}),
                                               res_opt());
    const json m = res_by(r, "MF0204FTE52-14K7");
    REQUIRE(!m.is_null());
    CHECK(verdict_of(m, "temp_max_C") == "fail");
    CHECK(m["grade"] == "major_review");
    CHECK(m["direction"] != "upgrade");
    CHECK(has_note(m, "rated only to +155"));
    // Unknown is UNVERIFIED, and not a drop-in.
    const json s = res_by(r, "NO_RANGE");
    REQUIRE(!s.is_null());
    CHECK(verdict_of(s, "temp_max_C") == "unverified");
    CHECK(s["grade"] == "minor_review");
    CHECK(has_note(s, "states no maximum operating temperature"));
    // The same range: no cap from it.
    CHECK(res_by(r, "SAME_RANGE")["grade"] == "drop_in");
    // A warmer minimum is the same rule at the other end.
    json warm = mf0;
    warm["mpn"] = "WARM_MIN";
    warm["temp_min_C"] = -55.0;
    warm["temp_max_C"] = 175.0;
    auto r2 = kelvin::crossref::cross_reference("resistor", orig, json::array({warm}), res_opt());
    CHECK(verdict_of(r2["candidates"][0], "temp_min_C") == "fail");
}

// Run2 RN55E90R9BRE6: 0.125 W at 70 degC, curve 70/125/175 -> 100/80/0 %. MFP-25BRD52-91R
// (0.25 W, 70/155 -> 100/0 %) and RT0805BRD0790R9L (0.125 W, same curve) were graded
// power_rating pass / upgrade / equivalent: at 125 degC they carry 0.088 W and 0.044 W
// against the original's 0.1 W.
TEST_CASE("qarlos 2026-11-08 r2: resistor power must hold along the original's derating curve",
          "[qarlos][crossref]") {
    json orig = res_spec("RN55E90R9BRE6", 90.9, 0.125, 0.1,
                         {{"power_rating_temperature", 70.0},
                          {"derating_temperature", {-65.0, 70.0, 125.0, 175.0}},
                          {"derating_amplitude", {1.0, 1.0, 0.8, 0.0}}});
    auto curve155 = [](const char* mpn, double p) {
        return res_spec(mpn, 90.9, p, 0.1,
                        {{"derating_temperature", {-55.0, 70.0, 155.0}},
                         {"derating_amplitude", {1.0, 1.0, 0.0}}});
    };
    // Same curve shape as the original, twice the power: ahead everywhere.
    json strong = res_spec("STRONG", 90.9, 0.25, 0.1,
                           {{"derating_temperature", {-65.0, 70.0, 125.0, 175.0}},
                            {"derating_amplitude", {1.0, 1.0, 0.8, 0.0}}});
    auto r = kelvin::crossref::cross_reference(
        "resistor", orig,
        json::array({curve155("MFP-25BRD52-91R", 0.25), curve155("RT0805BRD0790R9L", 0.125), strong}),
        res_opt());
    for (const char* m : {"MFP-25BRD52-91R", "RT0805BRD0790R9L"}) {
        const json c = res_by(r, m);
        REQUIRE(!c.is_null());
        CHECK(verdict_of(c, "power_rating") == "fail");
        CHECK(c["direction"] != "upgrade");
        CHECK(c["direction"] != "equivalent");
        CHECK(c["grade"] == "major_review");
        CHECK(has_note(c, "power compared along both derating curves"));
    }
    CHECK(verdict_of(res_by(r, "STRONG"), "power_rating") == "pass");
    CHECK(res_by(r, "STRONG")["direction"] == "upgrade");
}

// Run1 ERJ6DQJ1R5V (currentSenseShunt) took SR0805JR-471R5L (thickFilm) as drop_in; run3
// SG731JTTD4R7M (anti pulse) took AA0603JR-074R7L as drop_in; run4 M55342H09B10D0PWS
// (thinFilm) took ERJA1AF100U (currentSenseShunt) as equivalent with no caveat.
TEST_CASE("qarlos 2026-11-08 r2: a resistor technology or function change is not a drop-in",
          "[qarlos][crossref]") {
    json erj = res_spec("ERJ6DQJ1R5V", 1.5, 0.5, 5.0,
                        {{"technology", "currentSenseShunt"}, {"current_sense", true},
                         {"tcr", 100.0}});
    json sr = res_spec("SR0805JR-471R5L", 1.5, 0.5, 5.0,
                       {{"technology", "thickFilm"}, {"current_sense", false}});
    // A thick-film record whose series says current sensing: the same class written two ways.
    json rl73 = res_spec("RL73_SENSE", 1.5, 0.5, 5.0,
                         {{"technology", "thickFilm"}, {"current_sense", true}, {"tcr", 100.0}});
    auto r = kelvin::crossref::cross_reference("resistor", erj, json::array({sr, rl73}), res_opt());
    const json s = res_by(r, "SR0805JR-471R5L");
    REQUIRE(!s.is_null());
    CHECK(verdict_of(s, "current_sense") == "fail");
    CHECK(s["grade"] == "major_review");
    CHECK(s["direction"] == "downgrade");
    CHECK(has_note(s, "current-sense resistor by its record"));
    const json k = res_by(r, "RL73_SENSE");
    CHECK(verdict_of(k, "current_sense") == "pass");
    CHECK(verdict_of(k, "technology") == "");
    CHECK(k["grade"] == "drop_in");

    json sg = res_spec("SG731JTTD4R7M", 4.7, 0.1, 20.0,
                       {{"technology", "thickFilm"}, {"pulse_withstanding", true}});
    json aa = res_spec("AA0603JR-074R7L", 4.7, 0.1, 5.0, {{"technology", "thickFilm"}});
    auto r2 = kelvin::crossref::cross_reference("resistor", sg, json::array({aa}), res_opt());
    const json a = r2["candidates"][0];
    CHECK(verdict_of(a, "pulse_withstanding") == "fail");
    CHECK(a["grade"] == "major_review");
    CHECK(has_note(a, "pulse/surge-withstanding"));

    json m55 = res_spec("M55342H09B10D0PWS", 10.0, 1.0, 1.0, {{"technology", "thinFilm"}});
    json erja = res_spec("ERJA1AF100U", 10.0, 1.33, 1.0,
                         {{"technology", "currentSenseShunt"}, {"current_sense", true}});
    auto r3 = kelvin::crossref::cross_reference("resistor", m55, json::array({erja}), res_opt());
    const json e = r3["candidates"][0];
    CHECK(verdict_of(e, "technology") == "warn");
    CHECK(e["grade"] != "drop_in");
    CHECK(has_note(e, "thinFilm -> currentSenseShunt"));
}

// Run3 TNPW0805680RDETA (thin film, 25 ppm/K) took ERJS06D6800V (thick film, 100 ppm/K)
// with nothing said about either.
TEST_CASE("qarlos 2026-11-08 r2: a worse resistor TCR is flagged with both values",
          "[qarlos][crossref]") {
    json tnpw = res_spec("TNPW0805680RDETA", 680.0, 0.125, 0.5,
                         {{"technology", "thinFilm"}, {"tcr", 25.0}});
    json erjs = res_spec("ERJS06D6800V", 680.0, 0.125, 0.5,
                         {{"technology", "thickFilm"}, {"tcr", 100.0}});
    auto r = kelvin::crossref::cross_reference("resistor", tnpw, json::array({erjs}), res_opt());
    const json c = r["candidates"][0];
    const std::string tv = verdict_of(c, "tcr");
    CHECK((tv == "warn" || tv == "fail"));
    CHECK(has_note(c, "TCR +/-100 ppm/K vs the original's +/-25 ppm/K"));
    CHECK(verdict_of(c, "technology") == "warn");
    CHECK(c["grade"] != "drop_in");
}

// Run4 M55342H09B10D0PWS (drawn 6.58 x 3.15 mm, no case code) took AC1225FK-0710RL (1225,
// 3.2 x 6.4 mm, terminations on the long sides) as footprint "fits".
TEST_CASE("qarlos 2026-11-08 r2: a wide-terminal 1225 does not fit a conventional land",
          "[qarlos][crossref][dims]") {
    using kelvin::crossref::wide_terminal_chip_code;
    CHECK(wide_terminal_chip_code("1225"));
    CHECK(wide_terminal_chip_code("0612"));
    CHECK_FALSE(wide_terminal_chip_code("2512"));
    CHECK_FALSE(wide_terminal_chip_code("0204"));  // MELF size code, not a chip
    json m55 = res_spec("M55342H09B10D0PWS", 10.0, 1.0, 1.0,
                        {{"length_m", 6.5786e-3}, {"width_m", 3.1496e-3}});
    json ac = res_spec("AC1225FK-0710RL", 10.0, 2.0, 1.0,
                       {{"case_code", "1225"}, {"length_m", 3.2e-3}, {"width_m", 6.4e-3},
                        {"height_m", 0.55e-3}});
    kelvin::crossref::Options opt;
    opt.max_results = 10;
    auto r = kelvin::crossref::cross_reference("resistor", m55, json::array({ac}), opt);
    const json c = r["candidates"][0];
    CHECK(c["footprint"] == "unknown");
    CHECK(verdict_of(c, "footprint") == "unverified");
    CHECK(c["grade"] != "drop_in");
    CHECK(has_note(c, "wide-terminal 1225"));
    // 1225 against 1225 is the same land.
    json ac2 = ac;
    ac2["mpn"] = "ERJA1AF100U";
    auto r2 = kelvin::crossref::cross_reference("resistor", ac, json::array({ac2}), opt);
    CHECK(r2["candidates"][0]["footprint"] == "fits");
}

// Run4 CRCW060333K2DHECP (0603, drawn 1.55 x 0.85 mm) took AT0603DRD0733K2L (0603, drawn
// 1.6 x 0.8 mm) flagged "about one case size larger".
TEST_CASE("qarlos 2026-11-08 r2: two 0603 drawings a few percent apart are the same land",
          "[qarlos][crossref][dims]") {
    json crcw = res_spec("CRCW060333K2DHECP", 33200.0, 0.1, 0.5,
                         {{"case_code", "0603"}, {"length_m", 1.55e-3}, {"width_m", 0.85e-3},
                          {"height_m", 0.45e-3}});
    json at = res_spec("AT0603DRD0733K2L", 33200.0, 0.1, 0.5,
                       {{"case_code", "0603"}, {"length_m", 1.6e-3}, {"width_m", 0.8e-3},
                        {"height_m", 0.45e-3}});
    // A record that writes 0603 but draws a body 20 % longer is still flagged.
    json odd = at;
    odd["mpn"] = "ODD_0603";
    odd["length_m"] = 1.86e-3;
    kelvin::crossref::Options opt;
    opt.max_results = 10;
    auto r = kelvin::crossref::cross_reference("resistor", crcw, json::array({at, odd}), opt);
    const json a = res_by(r, "AT0603DRD0733K2L");
    REQUIRE(!a.is_null());
    CHECK(a["footprint"] == "fits");
    CHECK(verdict_of(a, "footprint") == "pass");
    CHECK_FALSE(has_note(a, "one case size larger"));
    CHECK(res_by(r, "ODD_0603")["footprint"] == "one_size_larger");
}

// Run2 seed 20276947: RNC55K1472FSB14 (6.35 x 2.39 mm) took ERJS14F1472U (3.2 x 2.5 mm) and
// ERJS12F1472U (4.5 x 3.2 mm) as "about one case size larger". Half as long and 5 % wider
// is a different land, not a larger case; ERJS1TF1472U (6.4 x 3.2 mm) really is larger.
TEST_CASE("qarlos 2026-11-08 r3: a body longer on one axis and shorter on the other is a different land",
          "[qarlos][crossref][dims]") {
    json rnc = res_spec("RNC55K1472FSB14", 14700.0, 0.1, 1.0,
                        {{"length_m", 6.35e-3}, {"width_m", 2.39e-3}});
    json s14 = res_spec("ERJS14F1472U", 14700.0, 0.5, 1.0,
                        {{"length_m", 3.2e-3}, {"width_m", 2.5e-3}});
    json s12 = res_spec("ERJS12F1472U", 14700.0, 0.75, 1.0,
                        {{"length_m", 4.5e-3}, {"width_m", 3.2e-3}});
    json s1t = res_spec("ERJS1TF1472U", 14700.0, 1.0, 1.0,
                        {{"length_m", 6.4e-3}, {"width_m", 3.2e-3}});
    kelvin::crossref::Options opt;
    opt.max_results = 10;
    auto r = kelvin::crossref::cross_reference("resistor", rnc, json::array({s14, s12, s1t}), opt);
    for (const char* m : {"ERJS14F1472U", "ERJS12F1472U"}) {
        const json c = res_by(r, m);
        REQUIRE(!c.is_null());
        CHECK(c["footprint"] == "different_case");
        CHECK(verdict_of(c, "footprint") == "warn");
        CHECK(c["grade"] == "minor_review");
        CHECK_FALSE(has_note(c, "one case size larger"));
        CHECK(has_note(c, "different land, not a larger case"));
    }
    CHECK(has_note(res_by(r, "ERJS14F1472U"), "3.2 x 2.5 mm vs 6.35 x 2.39 mm"));
    const json t = res_by(r, "ERJS1TF1472U");
    CHECK(t["footprint"] == "one_size_larger");
    CHECK(has_note(t, "about one case size larger"));

    // Chip against chip with crossed axes, on a capacitor (no resistor same-code rule):
    // a 2.0 x 2.0 mm body for a 3.2 x 1.6 mm one is 25 % wider and 37 % shorter.
    json cap_o = {{"mpn", "CAP_O"}, {"value_si", 1e-6}, {"voltage", 50.0},
                  {"length_m", 3.2e-3}, {"width_m", 1.6e-3}};
    json cap_s = {{"mpn", "CAP_S"}, {"value_si", 1e-6}, {"voltage", 50.0},
                  {"length_m", 2.0e-3}, {"width_m", 2.0e-3}};
    auto rc = kelvin::crossref::cross_reference("capacitor", cap_o, json::array({cap_s}), opt);
    const json cs = rc["candidates"][0];
    CHECK(cs["footprint"] == "different_case");
    CHECK(has_note(cs, "different land, not a larger case (2 x 2 mm vs 3.2 x 1.6 mm)"));
}

// The resistor extractor used to put a "typical" +/-5 % on every record whose
// electrical.tolerance was absent or 0, so an unstated part was compared — and selected — as a
// 5 % one. Absent stays absent: NaN in the row, null tolerance_pct in the spec (crossref.js),
// UNVERIFIED in the ranker, and rejected as tolerance_unknown by the selector.
TEST_CASE("qarlos 2026-11-08 r2: an unstated resistor tolerance stays unverified",
          "[qarlos][views][crossref]") {
    auto env = [](json elec) {
        json di = {{"part", {{"partNumber", "NO_TOL"}, {"technology", "thickFilm"}}},
                   {"electrical", elec}};
        return json{{"resistor",
                     {{"manufacturerInfo",
                       {{"name", "X"}, {"reference", "NO_TOL"}, {"datasheetInfo", di}}}}}};
    };
    auto absent = kelvin::extract_resistor(env({{"resistance", {{"nominal", 100.0}}}}));
    REQUIRE(absent.has_value());
    CHECK(std::isnan(absent->tolerance));
    auto zero = kelvin::extract_resistor(
        env({{"resistance", {{"nominal", 100.0}}}, {"tolerance", 0.0}}));
    REQUIRE(zero.has_value());
    CHECK(std::isnan(zero->tolerance));
    auto stated = kelvin::extract_resistor(
        env({{"resistance", {{"nominal", 100.0}}}, {"tolerance", 0.01}}));
    REQUIRE(stated.has_value());
    CHECK(stated->tolerance == Approx(0.01));

    // The ranker: a candidate with no tolerance on record is UNVERIFIED on tolerance.
    json orig = res_spec("ORIG", 100.0, 0.1, 1.0, json::object());
    json cand = {{"mpn", "NO_TOL"}, {"value_si", 100.0}, {"power_rating", 0.1},
                 {"tolerance_pct", nullptr}};
    auto r = kelvin::crossref::cross_reference("resistor", orig, json::array({cand}), res_opt());
    REQUIRE(r["candidates"].size() == 1);
    CHECK(verdict_of(r["candidates"][0], "tolerance_pct") == "unverified");

    // The selector: the unstated row is not passed as a tight one.
    kelvin::Shard<kelvin::ResistorRow> shard;
    shard.rows.push_back(*absent);
    shard.meta.source_line_count = 1;
    kelvin::ResistorConstraints rc;
    rc.target_ohms = 100.0;
    rc.max_tolerance = 0.05;
    try {
        kelvin::select_resistor(shard, rc);
        FAIL("an unstated tolerance passed a 5 % limit");
    } catch (const kelvin::NoCandidates& e) {
        CHECK(e.rejections.value("tolerance_unknown", 0) == 1);
    }
}


// ── Qarlos 2026-11-09 (seeds 20365838, 20575296, 20680025, 20784754) ─────────

// 380LX151M450K022 (1.7 A @ 20 kHz) against MAL219857151E3 (1.36 A @ 100 Hz): the ripple
// rating was failed across two frequencies with no caveat. Different stated frequencies, or a
// stated original frequency against an unstated one, are UNVERIFIED, noted, capped at
// minor_review and directionless; same frequency or an unstated original compare as before.
TEST_CASE("qarlos 2026-11-09: ripple current is compared only at a common frequency",
          "[qarlos][views][crossref]") {
    auto env = [](json elec) {
        elec["capacitance"] = {{"nominal", 150e-6}};
        elec["ratedVoltage"] = 450.0;
        json di = {{"part", {{"partNumber", "380LX151M450K022"},
                             {"technology", "aluminum-electrolytic-wet"}}},
                   {"electrical", elec}};
        return json{{"capacitor", {{"manufacturerInfo", {{"name", "Cornell Dubilier"},
                                                         {"reference", "380LX151M450K022"},
                                                         {"datasheetInfo", di}}}}}};
    };
    auto row = kelvin::extract_capacitor(env({{"rippleCurrent", 1.7},
                                              {"rippleCurrentFrequency", 20000.0}}));
    REQUIRE(row.has_value());
    CHECK(row->ripple_current_frequency == 20000.0);
    // a frequency with no ripple value quoted at it is not carried
    auto bare = kelvin::extract_capacitor(env({{"rippleCurrentFrequency", 120.0}}));
    REQUIRE(bare.has_value());
    CHECK(std::isnan(bare->ripple_current_frequency));

    auto cap = [](const char* mpn, double ir, std::optional<double> f) {
        json j = {{"mpn", mpn}, {"value_si", 150e-6}, {"voltage", 450.0},
                  {"technology", "aluminum-electrolytic-wet"}, {"esr", 0.44},
                  {"ripple_current", ir}};
        j["ripple_current_frequency"] = f ? json(*f) : json(nullptr);
        return j;
    };
    const json orig = cap("380LX151M450K022", 1.7, 20000.0);
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    // different frequencies: unverified, both values and frequencies named
    {
        auto r = kelvin::crossref::cross_reference(
            "capacitor", orig, json::array({cap("MAL219857151E3", 1.36, 100.0)}), opt);
        const json& c = r["candidates"][0];
        CHECK(verdict_of(c, "ripple_current") == "unverified");
        CHECK(has_note(c, "original 1.7 A at 20000 Hz, substitute 1.36 A at 100 Hz"));
        CHECK(c["status"] == "partial");
        CHECK(c["grade"] == "minor_review");
        CHECK(c["direction"] != "downgrade");
    }
    // the substitute states no frequency: unverified too
    {
        auto r = kelvin::crossref::cross_reference(
            "capacitor", orig, json::array({cap("NOFREQ", 1.36, std::nullopt)}), opt);
        const json& c = r["candidates"][0];
        CHECK(verdict_of(c, "ripple_current") == "unverified");
        CHECK(has_note(c, "states 1.36 A at no frequency"));
        CHECK(c["grade"] == "minor_review");
    }
    // same frequency: compared as stated, a real shortfall still fails
    {
        auto r = kelvin::crossref::cross_reference(
            "capacitor", orig, json::array({cap("SAMEF", 1.36, 20000.0)}), opt);
        CHECK(verdict_of(r["candidates"][0], "ripple_current") == "fail");
    }
    // the original states no frequency: compared as stated, as before
    {
        auto r = kelvin::crossref::cross_reference(
            "capacitor", cap("ORIG_NOF", 1.7, std::nullopt),
            json::array({cap("SUB", 1.36, 100.0)}), opt);
        CHECK(verdict_of(r["candidates"][0], "ripple_current") == "fail");
    }
}

// 380LX151M450K022 / 475VXH330MEFCSN35X35 state shape.assembly "Snap-In" (a CAS enum value);
// Kelvin did not classify it and the note called the original's mount "not stated". Snap-In
// is a through-hole board process; a stated but unclassifiable mount is quoted as stated.
TEST_CASE("qarlos 2026-11-09: a stated Snap-In mount is classified and never 'not stated'",
          "[qarlos][crossref][dims]") {
    using kelvin::crossref::normalize_mount;
    CHECK(normalize_mount("Snap-In") == "leaded");
    CHECK(normalize_mount("Screw Type").empty());
    json orig = {{"mpn", "380LX151M450K022"}, {"value_si", 150e-6}, {"voltage", 450.0},
                 {"technology", "aluminum-electrolytic-wet"}, {"mount", "Snap-In"}};
    json tht = {{"mpn", "THT_CAN"}, {"value_si", 150e-6}, {"voltage", 450.0},
                {"technology", "aluminum-electrolytic-wet"}, {"mount", "THT"}};
    json smt = tht;
    smt["mpn"] = "SMT_CAN";
    smt["mount"] = "SMT";
    json unstated = tht;
    unstated["mpn"] = "NO_MOUNT";
    unstated["mount"] = "";
    auto r = kelvin::crossref::cross_reference("capacitor", orig,
                                               json::array({tht, smt, unstated}));
    int seen = 0;
    for (const auto& c : r["candidates"]) {
        CHECK_FALSE(has_note(c, "original: not stated"));
        if (c["mpn"] == "SMT_CAN") {
            ++seen;
            CHECK(c["status"] == "no_substitute");
            CHECK(has_note(c, "mount type incompatible: Snap-In -> SMT"));
        }
        if (c["mpn"] == "THT_CAN") {
            ++seen;
            CHECK(verdict_of(c, "mounting") != "fail");
        }
        if (c["mpn"] == "NO_MOUNT") {
            ++seen;
            CHECK(has_note(c, "original: leaded"));
        }
    }
    CHECK(seen == 3);
    // a stated mount Kelvin cannot classify is quoted, not called absent
    json screw = orig;
    screw["mount"] = "Screw Type";
    auto r2 = kelvin::crossref::cross_reference("capacitor", screw, json::array({tht}));
    CHECK(has_note(r2["candidates"][0], "original: \"Screw Type\""));
}

// F339MX252231KIM2T0 states part.safetyClass x2 at 310 VAC and no series name, so the
// series-name gate never engaged and non-safety DC film parts came back 'recommended'.
TEST_CASE("qarlos 2026-11-09: a stated safety class and AC rating are extracted and compared",
          "[qarlos][views][crossref]") {
    json elec = {{"capacitance", {{"nominal", 2.2e-6}}}, {"ratedVoltage", 310.0},
                 {"voltageRatedAcMax", 310.0}};
    json env = {{"capacitor", {{"manufacturerInfo", {{"name", "Vishay"},
        {"reference", "F339MX252231KIM2T0"},
        {"datasheetInfo", {{"part", {{"partNumber", "F339MX252231KIM2T0"},
                                     {"technology", "film-polypropylene"},
                                     {"safetyClass", {{"class", "x2"}}}}},
                           {"electrical", elec}}}}}}}};
    auto row = kelvin::extract_capacitor(env);
    REQUIRE(row.has_value());
    CHECK(row->safety_class == "x2");
    CHECK(row->v_rated_ac == 310.0);

    auto cap = [](const char* mpn, json cls, json vac) {
        return json{{"mpn", mpn}, {"value_si", 2.2e-6}, {"voltage", 400.0},
                    {"technology", "film-polypropylene"}, {"family", ""},
                    {"safety_class", cls}, {"voltage_ac", vac}};
    };
    json orig = cap("F339MX252231KIM2T0", "x2", 310.0);
    orig["voltage"] = 310.0;
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    auto run = [&](const json& c) {
        return kelvin::crossref::cross_reference("capacitor", orig, json::array({c}), opt)
            ["candidates"][0];
    };
    // R75MR422050H4J: a DC film part with no class and no AC rating is never recommended
    {
        json c = run(cap("R75MR422050H4J", nullptr, nullptr));
        CHECK(c["status"] != "recommended");
        CHECK(verdict_of(c, "safety_class") == "unverified");
        CHECK(verdict_of(c, "voltage_ac") == "unverified");
        CHECK(c["grade"] == "major_review");
    }
    // X1 covers X2
    CHECK(run(cap("X1PART", "x1", 310.0))["status"] == "recommended");
    CHECK(verdict_of(run(cap("X1Y2PART", "x1y2", 310.0)), "safety_class") == "pass");
    // a stated class on the other axis, or a stated "none", is a mismatch and rejects
    {
        json c = run(cap("Y2PART", "y2", 310.0));
        CHECK(c["status"] == "no_substitute");
        CHECK(verdict_of(c, "safety_class") == "fail");
        CHECK(has_note(c, "holds no X-class approval"));
        json n = run(cap("NONE", "none", 310.0));
        CHECK(n["status"] == "no_substitute");
        CHECK(has_note(n, "NO safety-class approval"));
    }
    // a lower AC rating is a FAIL and rejects
    {
        json c = run(cap("X2_275", "x2", 275.0));
        CHECK(c["status"] == "no_substitute");
        CHECK(verdict_of(c, "voltage_ac") == "fail");
        CHECK(has_note(c, "275 V AC vs the original's 310 V AC"));
    }
    // Y1 covers Y2; an original with no class compares nothing
    {
        json y2 = cap("Y2ORIG", "y2", 300.0);
        auto r = kelvin::crossref::cross_reference(
            "capacitor", y2, json::array({cap("Y1PART", "y1", 300.0)}), opt);
        CHECK(verdict_of(r["candidates"][0], "safety_class") == "pass");
        json plain = cap("PLAIN", nullptr, nullptr);
        auto r2 = kelvin::crossref::cross_reference(
            "capacitor", plain, json::array({cap("NONE", "none", nullptr)}), opt);
        CHECK(verdict_of(r2["candidates"][0], "safety_class").empty());
        CHECK(verdict_of(r2["candidates"][0], "voltage_ac").empty());
    }
    // a token outside the CAS enum is malformed data, not "no class"
    CHECK_THROWS_AS(run(cap("BAD", "x9", 310.0)), std::invalid_argument);
}

// XT26008-T-K (-55..200 degC) took 125 degC chip inductors with no caveat: the magnetic row
// carried no temperature range. Extracted now, and compared with the resistor rule.
TEST_CASE("qarlos 2026-11-09: a magnetic's operating range is extracted and must be covered",
          "[qarlos][views][crossref]") {
    json env = {{"magnetic", {{"manufacturerInfo", {{"name", "Vanguard Electronics"},
        {"reference", "XT26008-T-K"},
        {"datasheetInfo", {{"electrical", json::array({{{"subtype", "inductor"},
                                                        {"inductance", {{"nominal", 4.7e-8}}},
                                                        {"ratedCurrents", {0.55}}}})},
                           {"thermal", {{"operatingTemperature",
                                         {{"minimum", -55.0}, {"maximum", 200.0}}}}}}}}}}}};
    auto row = kelvin::extract_magnetic(env);
    REQUIRE(row.has_value());
    CHECK(row->temp_min_c == -55.0);
    CHECK(row->temp_max_c == 200.0);

    auto ind = [](const char* mpn, json lo, json hi) {
        return json{{"mpn", mpn}, {"value_si", 4.7e-8}, {"rated_current", 0.55},
                    {"saturation_current", 0.7}, {"dcr", 0.08}, {"temp_min_C", lo},
                    {"temp_max_C", hi}};
    };
    json orig = ind("XT26008-T-K", -55.0, 200.0);
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    auto r = kelvin::crossref::cross_reference(
        "magnetic", orig,
        json::array({ind("LQW18AS47NG00", -40.0, 125.0), ind("COVERS", -55.0, 200.0),
                     ind("NO_RANGE", nullptr, nullptr)}),
        opt);
    int seen = 0;
    for (const auto& c : r["candidates"]) {
        if (c["mpn"] == "LQW18AS47NG00") {
            ++seen;
            CHECK(verdict_of(c, "temp_max_C") == "fail");
            CHECK(verdict_of(c, "temp_min_C") == "fail");
            CHECK(c["grade"] == "major_review");
            CHECK(has_note(c, "rated only to +125"));
        }
        if (c["mpn"] == "COVERS") {
            ++seen;
            CHECK(verdict_of(c, "temp_max_C") == "pass");
            CHECK(c["grade"] == "drop_in");
        }
        if (c["mpn"] == "NO_RANGE") {
            ++seen;
            CHECK(verdict_of(c, "temp_max_C") == "unverified");
            CHECK(c["grade"] == "minor_review");
            CHECK(has_note(c, "states no maximum operating temperature"));
        }
    }
    CHECK(seen == 3);
    // a one-degree shortfall is still a FAIL: no warn band
    auto r2 = kelvin::crossref::cross_reference(
        "magnetic", orig, json::array({ind("ONE_SHORT", -55.0, 199.0)}), opt);
    CHECK(verdict_of(r2["candidates"][0], "temp_max_C") == "fail");
}
