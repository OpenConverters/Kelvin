// Regression tests for the Qarlos cross-reference audit of 2026-10-24
// (ABT #2029-#2051). Each case reproduces the reported row in miniature.
#include <cmath>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "Browse.hpp"
#include "Index.hpp"
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

// ABT #2029: a known original height against an unknown candidate height is reported
// UNVERIFIED with a note. Height is a secondary axis (owner, 2026-11-09): no cap any more.
TEST_CASE("qarlos #2029: unknown candidate height on a magnetic is unverified and noted",
          "[qarlos][crossref][dims]") {
    json orig = {{"mpn", "LQH2HPH100MGR"}, {"value_si", 10e-6}, {"saturation_current", 0.6},
                 {"rated_current", 0.5}, {"length_m", 2.5e-3}, {"width_m", 2.0e-3},
                 {"height_m", 1.0e-3}};
    json cand = {{"mpn", "VLS252012HBX-100M-1"}, {"value_si", 10e-6}, {"saturation_current", 0.7},
                 {"rated_current", 0.6}, {"length_m", 2.5e-3}, {"width_m", 2.0e-3}};
    auto r = kelvin::crossref::cross_reference("magnetic", orig, json::array({cand}));
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "height") == "unverified");
    CHECK(c["grade"] == "drop_in");  // owner 2026-11-09: height is secondary, no cap
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
    // Owner decision 2026-11-13: a supply range that does not cover the original's is
    // REJECTED (was capped at major_review). Fixture unchanged; only the expected grade.
    CHECK((*c33)["grade"] == "no_substitute");
    CHECK(has_note(*c33, "the original runs from 2.5 V, the substitute from 3.3 V"));

    const json* c18 = cand_by_mpn(r, "XO-18");
    REQUIRE(c18);
    CHECK(verdict_of(*c18, "supply_max_V") == "fail");
    CHECK((*c18)["grade"] == "no_substitute");  // owner 2026-11-13, as above

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
        CHECK(c["grade"] == "drop_in");  // owner 2026-11-09: height is secondary
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
    CHECK(c["grade"] == "drop_in");  // owner 2026-11-09: height is secondary
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
// pitch. One side only: rows UNVERIFIED with a note, capped below drop_in (Qarlos
// 2026-11-09, 54202-G0804-B02LF; the same rule as orientation and pluggable). Neither: silent.
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
    // Added 2026-11-09: rows stated by the original only is unverified, noted, and capped.
    CHECK(verdict_of(*un, "rows") == "unverified");
    CHECK(has_note(*un, "contact rows not established (original: 1, substitute: not stated)"));
    CHECK((*un)["grade"] == "minor_review");
    // The like-for-like part reports no rows note and stays a drop-in.
    CHECK(count_notes(*one, "contact rows") == 0);
    CHECK((*one)["grade"] == "drop_in");
    // Neither record states rows: silent, and nothing caps the grade.
    auto r2 = kelvin::crossref::cross_reference("connector", hdr("ORIG_NO_ROWS", 2.54, nullptr),
                                                json::array({hdr("CAND_NO_ROWS", 2.54, nullptr)}),
                                                opt);
    const json* neither = cand_by_mpn(r2, "CAND_NO_ROWS");
    REQUIRE(neither);
    CHECK(verdict_of(*neither, "rows") == "");
    CHECK(count_notes(*neither, "contact rows") == 0);
    CHECK((*neither)["grade"] == "drop_in");
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
    // Unknown is UNVERIFIED and noted (secondary since 2026-11-09: no cap).
    const json s = res_by(r, "NO_RANGE");
    REQUIRE(!s.is_null());
    CHECK(verdict_of(s, "temp_max_C") == "unverified");
    CHECK(s["grade"] == "drop_in");  // owner 2026-11-09: operating range is secondary
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
    // Screw Type is classified since the round-5..7 fix (its own class, "screw"; see the
    // screw-terminal test below). Chassis Mount is the CAS value that stays unclassified.
    CHECK(normalize_mount("Screw Type") == "screw");
    CHECK(normalize_mount("Chassis Mount").empty());
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
    // A stated mount Kelvin cannot classify is quoted, not called absent. This used "Screw
    // Type", which is now classified (a screw-terminal original against a THT can is an
    // established mismatch, asserted here); the unclassifiable case is CAS "Chassis Mount".
    json screw = orig;
    screw["mount"] = "Screw Type";
    auto r2 = kelvin::crossref::cross_reference("capacitor", screw, json::array({tht}));
    CHECK(r2["candidates"][0]["status"] == "no_substitute");
    CHECK(has_note(r2["candidates"][0], "mount type incompatible: Screw Type -> THT"));
    json chassis = orig;
    chassis["mount"] = "Chassis Mount";
    auto r3 = kelvin::crossref::cross_reference("capacitor", chassis, json::array({tht}));
    CHECK(has_note(r3["candidates"][0], "original: \"Chassis Mount\""));
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
            CHECK(c["grade"] == "drop_in");  // owner 2026-11-09: range is secondary
            CHECK(has_note(c, "states no maximum operating temperature"));
        }
    }
    CHECK(seen == 3);
    // a one-degree shortfall is still a FAIL: no warn band
    auto r2 = kelvin::crossref::cross_reference(
        "magnetic", orig, json::array({ind("ONE_SHORT", -55.0, 199.0)}), opt);
    CHECK(verdict_of(r2["candidates"][0], "temp_max_C") == "fail");
}

// ── Resistors, Qarlos audit rounds 5-7 (2026-11-09). Every record below is the REAL TAS record
// from resistors.ndjson (provenance dropped), run through extract_resistor and turned into the
// ranker spec exactly as web/src/crossref.js builds it (base() + the resistor spec()).
namespace {
json res_record_spec(const char* raw) {
    const auto row = kelvin::extract_resistor(json::parse(raw));
    REQUIRE(row.has_value());
    auto num_or_null = [](double v) { return std::isnan(v) ? json(nullptr) : json(v); };
    auto pos_or_null = [](double v) { return (!std::isnan(v) && v > 0) ? json(v) : json(nullptr); };
    json s = {{"mpn", row->mpn},
              {"length_m", pos_or_null(row->length_m)},
              {"width_m", pos_or_null(row->width_m)},
              {"height_m", pos_or_null(row->height_m)},
              {"case_code", row->case_code},
              {"mount", row->mount},
              {"lead_pitch_m", pos_or_null(row->lead_pitch_m)},
              {"is_production", row->is_production},
              {"provenance_retracted", row->provenance_retracted},
              {"value_si", pos_or_null(row->resistance)},
              {"power_rating", pos_or_null(row->power_rating)},
              {"tolerance_pct", (!std::isnan(row->tolerance) && row->tolerance > 0)
                                    ? json(row->tolerance * 100)
                                    : json(nullptr)},
              {"family", row->family},
              {"power_rating_temperature", num_or_null(row->power_rating_temperature)},
              {"technology", row->technology},
              {"tcr", num_or_null(row->tcr)},
              {"temp_min_C", num_or_null(row->temp_min_c)},
              {"temp_max_C", num_or_null(row->temp_max_c)},
              {"current_sense", row->current_sense},
              {"pulse_withstanding", row->pulse_withstanding},
              {"max_voltage_V", pos_or_null(row->max_voltage)}};
    s["derating_temperature"] =
        row->derating_temperature.empty() ? json(nullptr) : json(row->derating_temperature);
    s["derating_amplitude"] =
        row->derating_amplitude.empty() ? json(nullptr) : json(row->derating_amplitude);
    return s;
}
json res_find(const json& r, const std::string& mpn) {
    for (const auto& c : r["candidates"])
        if (c["mpn"] == mpn) return c;
    return json(nullptr);
}

// Originals.
const char* kRWR80S28R0FSRSL = R"J({"resistor":{"manufacturerInfo":{"name":"Vishay","reference":"RWR80S28R0FSRSL","datasheetInfo":{"part":{"partNumber":"RWR80S28R0FSRSL","technology":"wirewound","series":"RWR80"},"electrical":{"resistance":{"nominal":28.0},"tolerance":0.01,"powerRating":2,"temperatureCoefficient":20,"powerRatingTemperature":25},"thermal":{"operatingTemperature":{"minimum":-55,"maximum":250}},"factors":{"powerDerating":{"temperature":[-60,25,250],"amplitude":[1,1,0]}},"mechanical":{"length":{"nominal":0.01031,"minimum":0.009523,"maximum":0.011097},"diameter":{"nominal":0.00239,"minimum":0.001603,"maximum":0.003177}}}}}})J";
const char* kRLR32C2701GRRSL = R"J({"resistor":{"manufacturerInfo":{"name":"Vishay","reference":"RLR32C2701GRRSL","datasheetInfo":{"part":{"partNumber":"RLR32C2701GRRSL","technology":"metalFilm","series":"RLR32"},"electrical":{"resistance":{"nominal":2700.0},"tolerance":0.02,"powerRating":1.0,"temperatureCoefficient":100,"powerRatingTemperature":70,"maxVoltage":500},"thermal":{"operatingTemperature":{"minimum":-65,"maximum":150}},"factors":{"powerDerating":{"temperature":[-65,70,150],"amplitude":[1,1,0]}},"mechanical":{"length":{"nominal":0.01427,"minimum":0.01348,"maximum":0.01506},"diameter":{"nominal":0.00483,"minimum":0.00445,"maximum":0.00521}}}}}})J";
const char* kERJB2CJR024V = R"J({"resistor":{"manufacturerInfo":{"name":"Panasonic","reference":"ERJB2CJR024V","status":"production","family":"High Power Current Sensing Resistors, Wide Terminal Type","datasheetInfo":{"part":{"partNumber":"ERJB2CJR024V","technology":"currentSenseShunt","series":"High Power Current Sensing Resistors, Wide Terminal Type","case":"0612"},"electrical":{"resistance":{"nominal":0.024},"tolerance":0.05,"powerRating":1.5},"mechanical":{"length":{"nominal":0.0016},"width":{"nominal":0.0032},"landPattern":{"pattern":"singleRow","originDatum":"padCentroid","pads":[{"id":"1","x":-0.00075,"y":0.0,"shape":"rectangle","width":0.001,"height":0.0034},{"id":"2","x":0.00075,"y":0.0,"shape":"rectangle","width":0.001,"height":0.0034}]}}}}}})J";
const char* kCMF5586R600DHEA = R"J({"resistor":{"manufacturerInfo":{"name":"Vishay","reference":"CMF5586R600DHEA","datasheetInfo":{"part":{"partNumber":"CMF5586R600DHEA","technology":"metalFilm","series":"CMF55"},"electrical":{"resistance":{"nominal":86.6},"tolerance":0.005,"powerRating":0.6,"temperatureCoefficient":50,"powerRatingTemperature":70,"maxVoltage":350},"thermal":{"operatingTemperature":{"minimum":-55,"maximum":155}},"factors":{"powerDerating":{"temperature":[-55,70,155],"amplitude":[1,1,0]}},"mechanical":{"length":{"maximum":0.0065},"diameter":{"maximum":0.0025}}}}}})J";
const char* kRK73H2AATTE3324F = R"J({"resistor":{"manufacturerInfo":{"name":"KOA Speer","reference":"RK73H2AATTE3324F","datasheetInfo":{"part":{"partNumber":"RK73H2AATTE3324F","series":"RK73H2AATTE","technology":"thickFilm","case":"0805","description":"Flat Chip Resistors(Precision Grade)"},"electrical":{"resistance":{"nominal":3320000},"tolerance":0.01,"temperatureCoefficient":400,"powerRating":0.25,"powerRatingTemperature":70,"maxVoltage":150,"maxOverloadVoltage":200},"thermal":{"operatingTemperature":{"minimum":-55,"maximum":155}},"mechanical":{"length":{"nominal":0.002,"minimum":0.0018,"maximum":0.0022},"width":{"nominal":0.00125,"minimum":0.00115,"maximum":0.00135},"height":{"nominal":0.00055,"minimum":0.00045,"maximum":0.00065},"assemblyType":"smt"}}}}})J";
const char* kSR732ATTDR412F = R"J({"resistor":{"manufacturerInfo":{"name":"KOA Speer","reference":"SR732ATTDR412F","datasheetInfo":{"part":{"partNumber":"SR732ATTDR412F","series":"SR732ATTD","technology":"thickFilm","case":"0805","description":"Low Resistance Flat Chip Resistors"},"electrical":{"resistance":{"nominal":0.412},"tolerance":0.01,"temperatureCoefficient":100,"powerRating":0.5,"powerRatingTemperature":70},"thermal":{"operatingTemperature":{"minimum":-55,"maximum":150}},"mechanical":{"length":{"nominal":0.002,"minimum":0.0018,"maximum":0.0022},"width":{"nominal":0.00125,"minimum":0.00115,"maximum":0.00135},"height":{"nominal":0.0005,"minimum":0.0004,"maximum":0.0006},"assemblyType":"smt"}}}}})J";
const char* kMFR25FTE52422K = R"J({"resistor":{"manufacturerInfo":{"name":"YAGEO","reference":"MFR-25FTE52-422K","datasheetInfo":{"part":{"partNumber":"MFR-25FTE52-422K","technology":"thinFilm","series":"MFR"},"electrical":{"resistance":{"nominal":422000.0},"tolerance":0.01,"powerRating":0.25,"powerRatingTemperature":70,"temperatureCoefficient":50.0},"thermal":{"operatingTemperature":{"minimum":-55.0,"maximum":155.0}},"factors":{"powerDerating":{"temperature":[-55,70,155],"amplitude":[1,1,0]}}}}}})J";
const char* kCRCW080530R0JNTA = R"J({"resistor":{"manufacturerInfo":{"name":"Vishay","reference":"CRCW080530R0JNTA","datasheetInfo":{"part":{"partNumber":"CRCW080530R0JNTA","technology":"thickFilm","series":"CRCW0805","case":"0805"},"electrical":{"resistance":{"nominal":30.0},"tolerance":0.05,"powerRating":0.125,"temperatureCoefficient":200,"powerRatingTemperature":70,"maxVoltage":150},"thermal":{"operatingTemperature":{"minimum":-55,"maximum":155}},"factors":{"powerDerating":{"temperature":[-55,70,155],"amplitude":[1,1,0]}},"mechanical":{"length":{"nominal":0.002,"minimum":0.0019,"maximum":0.0022},"width":{"nominal":0.00125,"minimum":0.0011,"maximum":0.0014},"height":{"nominal":0.0005,"minimum":0.0004,"maximum":0.0006}}}}}})J";

// Candidates.
const char* kAC0612FR0727RL = R"J({"resistor":{"manufacturerInfo":{"name":"YAGEO","reference":"AC0612FR-0727RL","status":"production","family":"AC Wide Terminal","datasheetInfo":{"part":{"partNumber":"AC0612FR-0727RL","technology":"thickFilm","case":"0612"},"electrical":{"resistance":{"nominal":27.0},"tolerance":0.01,"powerRating":1.0},"mechanical":{"length":{"nominal":0.0016},"height":{"nominal":0.00055},"width":{"nominal":0.0032,"minimum":0.003,"maximum":0.0034}},"thermal":{"operatingTemperature":{"minimum":-55.0,"maximum":155.0}}}}}})J";
const char* kERJA1AF272U = R"J({"resistor":{"manufacturerInfo":{"name":"Panasonic","reference":"ERJA1AF272U","status":"production","family":"High Power Current Sensing Resistors, Wide Terminal Type","datasheetInfo":{"part":{"partNumber":"ERJA1AF272U","technology":"currentSenseShunt","series":"High Power Current Sensing Resistors, Wide Terminal Type","case":"1225"},"electrical":{"resistance":{"nominal":2700.0},"tolerance":0.01,"powerRating":1.33,"temperatureCoefficient":100.0},"mechanical":{"length":{"nominal":0.0032},"width":{"nominal":0.0064}}}}}})J";
const char* kERJB2AF272V = R"J({"resistor":{"manufacturerInfo":{"name":"Panasonic","reference":"ERJB2AF272V","status":"production","family":"High Power Current Sensing Resistors, Wide Terminal Type","datasheetInfo":{"part":{"partNumber":"ERJB2AF272V","technology":"currentSenseShunt","series":"High Power Current Sensing Resistors, Wide Terminal Type","case":"0612"},"electrical":{"resistance":{"nominal":2700.0},"tolerance":0.01,"powerRating":0.75,"temperatureCoefficient":100.0},"mechanical":{"length":{"nominal":0.0016},"width":{"nominal":0.0032}}}}}})J";
const char* kLVR03R0250FE70 = R"J({"resistor":{"manufacturerInfo":{"name":"Vishay","reference":"LVR03R0250FE70","datasheetInfo":{"part":{"partNumber":"LVR03R0250FE70","technology":"currentSenseShunt","series":"LVR03"},"electrical":{"resistance":{"nominal":0.025},"tolerance":0.01,"powerRating":3.0,"powerRatingTemperature":25}}}}})J";
const char* k4_1879645_6 = R"J({"resistor":{"manufacturerInfo":{"name":"TE Connectivity","reference":"4-1879645-6","datasheetInfo":{"part":{"partNumber":"4-1879645-6","description":"86.6 ohm, Thin Film, Precision Resistor, .5 %, .25 W, ±50 ppm/°C, Axial-Leaded, Copper Termination, 7.2 x 2.5 mm, Bandoliered, Holsworthy Holco","technology":"thinFilm","series":"Holsworthy"},"electrical":{"resistance":{"nominal":86.6},"tolerance":0.005,"powerRating":0.25,"temperatureCoefficient":50.0}}}}})J";
const char* kMF1_2CC86R6D = R"J({"resistor":{"manufacturerInfo":{"name":"KOA Speer","reference":"MF1/2CC86R6D","datasheetInfo":{"part":{"partNumber":"MF1/2CC86R6D","series":"MF1/2","technology":"metalFilm","description":"Coat-Insulated Fixed Metal Film Resistors"},"electrical":{"resistance":{"nominal":86.6},"tolerance":0.005,"powerRating":0.5,"powerRatingTemperature":70,"maxVoltage":350,"maxOverloadVoltage":700,"temperatureCoefficient":50},"thermal":{"operatingTemperature":{"minimum":-55,"maximum":155}},"mechanical":{"length":{"nominal":0.009,"minimum":0.008,"maximum":0.01},"diameter":{"nominal":0.0035,"minimum":0.0031,"maximum":0.0039},"assemblyType":"tht"}}}}})J";
const char* k1623220_1 = R"J({"resistor":{"manufacturerInfo":{"name":"TE Connectivity","reference":"1623220-1","datasheetInfo":{"part":{"partNumber":"1623220-1","description":"3.3M ohm, Thick Film, General Purpose Resistor, 1 %, 0805, .125 W, ±100 ppm/°C, Solder, 2 Terminations, .55 mm [.022 in] Height, NEOHM CRG","technology":"thickFilm","series":"Neohm","case":"0805"},"electrical":{"resistance":{"nominal":3300000.0},"tolerance":0.01,"powerRating":0.125,"temperatureCoefficient":100.0}}}}})J";
const char* kERJU6QFR43V = R"J({"resistor":{"manufacturerInfo":{"name":"Panasonic","reference":"ERJU6QFR43V","family":"Anti-Sulfurated Thick Film Chip Resistors","datasheetInfo":{"part":{"partNumber":"ERJU6QFR43V","technology":"thickFilm","series":"Anti-Sulfurated Thick Film Chip Resistors","case":"0805"},"electrical":{"resistance":{"nominal":0.43},"tolerance":0.01,"powerRating":0.25},"mechanical":{"length":{"nominal":0.002},"width":{"nominal":0.00125}}}}}})J";
const char* kERJ6BQFR43V = R"J({"resistor":{"manufacturerInfo":{"name":"Panasonic","reference":"ERJ6BQFR43V","family":"Current Sensing Resistors Thick Film Type","datasheetInfo":{"part":{"partNumber":"ERJ6BQFR43V","technology":"currentSenseShunt","series":"Current Sensing Resistors Thick Film Type","case":"0805"},"electrical":{"resistance":{"nominal":0.43},"tolerance":0.01,"powerRating":0.33},"mechanical":{"length":{"nominal":0.002},"width":{"nominal":0.00125}}}}}})J";
const char* kERA6AED4223V = R"J({"resistor":{"manufacturerInfo":{"name":"Panasonic","reference":"ERA6AED4223V","family":"Thin film chip resistors (High reliability type)","datasheetInfo":{"part":{"partNumber":"ERA6AED4223V","technology":"thinFilm","series":"Thin film chip resistors (High reliability type)","case":"0805"},"electrical":{"resistance":{"nominal":422000.0},"tolerance":0.005,"powerRating":0.125,"temperatureCoefficient":25.0},"mechanical":{"length":{"nominal":0.002},"width":{"nominal":0.00125}}}}}})J";
const char* kAC0805JR0730RL = R"J({"resistor":{"manufacturerInfo":{"name":"YAGEO","reference":"AC0805JR-0730RL","status":"production","family":"AC","datasheetInfo":{"part":{"partNumber":"AC0805JR-0730RL","technology":"thickFilm","case":"0805"},"electrical":{"resistance":{"nominal":30.0},"tolerance":0.05,"powerRating":0.125},"mechanical":{"length":{"nominal":0.002},"height":{"nominal":0.0005},"width":{"nominal":0.00125,"minimum":0.00115,"maximum":0.00135}},"thermal":{"operatingTemperature":{"minimum":-55.0,"maximum":155.0}},"factors":{"powerDerating":{"temperature":[-55,70,155],"amplitude":[1,1,0]}}}}}})J";
}  // namespace

// r5.2 RWR80S28R0FSRSL -> AC0612FR-0727RL, r5.3 RLR32C2701GRRSL -> ERJA1AF272U, r6.5
// ERJB2CJR024V -> LVR03R0250FE70: the wide-terminal note said the outlines "match when
// turned" for a 10 mm axial body, a 14 mm axial body and a part with no dimensions at all.
TEST_CASE("qarlos 2026-11-09 r5-7: the wide-terminal note claims only the outline it compared",
          "[qarlos][crossref][dims]") {
    kelvin::crossref::Options opt;
    opt.max_results = 10;
    auto one = [&](const char* o, const char* c) {
        auto r = kelvin::crossref::cross_reference("resistor", res_record_spec(o),
                                                   json::array({res_record_spec(c)}), opt);
        REQUIRE(r["candidates"].size() == 1);
        return r["candidates"][0];
    };
    struct Wide { const char* orig; const char* cand; const char* wide; const char* plain; };
    for (const Wide& w : {Wide{kRWR80S28R0FSRSL, kAC0612FR0727RL, "substitute 1.6 x 3.2 mm",
                               "original 10.31 x 2.39 mm"},
                          Wide{kRLR32C2701GRRSL, kERJA1AF272U, "substitute 3.2 x 6.4 mm",
                               "original 14.27 x 4.83 mm"}}) {
        INFO(w.wide);
        const json k = one(w.orig, w.cand);
        CHECK(verdict_of(k, "footprint") == "unverified");
        CHECK(has_note(k, "wide-terminal"));
        CHECK_FALSE(has_note(k, "match only when turned"));
        CHECK(has_note(k, "the outlines differ as well"));
        CHECK(has_note(k, w.wide));
        CHECK(has_note(k, w.plain));
        CHECK(k["grade"] != "drop_in");
    }
    // The LVR candidate states neither a case code nor any dimension.
    const json lvr = one(kERJB2CJR024V, kLVR03R0250FE70);
    CHECK(verdict_of(lvr, "footprint") == "unverified");
    CHECK(lvr["grade"] != "drop_in");
    CHECK(has_note(lvr, "wide-terminal 0612"));
    CHECK_FALSE(has_note(lvr, "match only when turned"));
    CHECK(has_note(lvr, "the substitute's record states no complete outline dimensions"));
    // The established case: M55342H09B10D0PWS's 6.58 x 3.15 mm drawing is the 1225 box turned.
    json m55 = {{"mpn", "M55342H09B10D0PWS"}, {"value_si", 2700.0}, {"power_rating", 1.0},
                {"tolerance_pct", 1.0}, {"length_m", 6.5786e-3}, {"width_m", 3.1496e-3}};
    auto r = kelvin::crossref::cross_reference("resistor", m55,
                                               json::array({res_record_spec(kERJA1AF272U)}), opt);
    REQUIRE(r["candidates"].size() == 1);
    CHECK(has_note(r["candidates"][0], "the outlines match only when turned"));
}

// r5.3, r5.4, r7.1, r7.2, r7.4: a lower nominal power against an original whose rating is
// stated at a temperature. e537384 made the no-basis shortfall a FAIL; these are the audited
// pairs from their real records, plus a substitute WITH a stated basis (KOA MF1/2CC86R6D,
// 0.5 W at 70 degC) against CMF55's 0.6 W at 70 degC.
TEST_CASE("qarlos 2026-11-09 r5-7: a lower resistor power is never graded upgrade or equivalent",
          "[qarlos][crossref]") {
    auto opt = res_opt();
    struct Pair { const char* orig; const char* cand; const char* mpn; };
    for (const Pair& p : {Pair{kRLR32C2701GRRSL, kERJB2AF272V, "ERJB2AF272V"},
                          Pair{kCMF5586R600DHEA, k4_1879645_6, "4-1879645-6"},
                          Pair{kCMF5586R600DHEA, kMF1_2CC86R6D, "MF1/2CC86R6D"},
                          Pair{kRK73H2AATTE3324F, k1623220_1, "1623220-1"},
                          Pair{kSR732ATTDR412F, kERJU6QFR43V, "ERJU6QFR43V"},
                          Pair{kSR732ATTDR412F, kERJ6BQFR43V, "ERJ6BQFR43V"},
                          Pair{kMFR25FTE52422K, kERA6AED4223V, "ERA6AED4223V"}}) {
        INFO(p.mpn);
        auto r = kelvin::crossref::cross_reference("resistor", res_record_spec(p.orig),
                                                   json::array({res_record_spec(p.cand)}), opt);
        const json c = res_find(r, p.mpn);
        REQUIRE(!c.is_null());
        CHECK(verdict_of(c, "power_rating") == "fail");
        CHECK(c["direction"] != "upgrade");
        CHECK(c["direction"] != "equivalent");
        CHECK(c["grade"] != "drop_in");
        CHECK(c["grade"] != "minor_review");
    }
}

// r7.3 CRCW080530R0JNTA (200 ppm/K) took three YAGEO AC0805 parts, whose records state no TCR,
// as drop_in with no note.
TEST_CASE("qarlos 2026-11-09 r5-7: a TCR the substitute does not state is unverified and noted",
          "[qarlos][crossref]") {
    auto opt = res_opt();
    auto r = kelvin::crossref::cross_reference("resistor", res_record_spec(kCRCW080530R0JNTA),
                                               json::array({res_record_spec(kAC0805JR0730RL)}),
                                               opt);
    const json c = res_find(r, "AC0805JR-0730RL");
    REQUIRE(!c.is_null());
    CHECK(verdict_of(c, "tcr") == "unverified");
    CHECK(c["grade"] == "drop_in");  // owner 2026-11-09: TCR is secondary, no cap
    CHECK(has_note(c, "the original states a TCR of +/-200 ppm/K"));
    CHECK(has_note(c, "substitute's record states none"));
    // Neither side states a TCR: the gap is common to every candidate, nothing is said.
    json o = res_record_spec(kCRCW080530R0JNTA);
    o["tcr"] = nullptr;
    auto r2 = kelvin::crossref::cross_reference("resistor", o,
                                                json::array({res_record_spec(kAC0805JR0730RL)}),
                                                opt);
    REQUIRE(r2["candidates"].size() == 1);
    CHECK_FALSE(has_note(r2["candidates"][0], "states a TCR"));
    CHECK(r2["candidates"][0]["grade"] == "drop_in");
}

// ── Qarlos 2026-11-09 rounds 5-6: timing frequency identity, stability, control pin; MOSFET Qg ──
namespace {
// The spec web/src/crossref.js builds for a timing row, from the REAL TAS record fields
// (frequency, frequencyStability x 1e6, supply, operating range, outputType, enableFunction).
json osc(const std::string& mpn, double f, double stability_frac, double vmin, double vmax,
         double tmin, double tmax, const char* enable) {
    json o = {{"mpn", mpn}, {"subtype", "oscillator"}, {"technology", "crystalOscillator"},
              {"device_type", "oscillator"}, {"frequency", f},
              {"stability_ppm", stability_frac * 1e6}, {"output_type", "cmos"},
              {"supply_min_V", vmin}, {"supply_max_V", vmax},
              {"temp_min_C", tmin}, {"temp_max_C", tmax}};
    o["enable_function"] = enable ? json(enable) : json(nullptr);
    return o;
}
json run_timing(const json& orig, const json& cands) {
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    return kelvin::crossref::cross_reference("timeBase", orig, cands, opt);
}
}  // namespace

// ECS-3225SMVQ-250-DS-TR (25 MHz, 20 ppm) took SG-8018CA 25.001250M-TJHSA0 (+50 ppm) as a
// frequency 'pass' (seed 20889483); ASSVP-14.7456MHZ-R-C16 took SG-8018CA 14.7450M-TJHPA0
// (-40.7 ppm) the same way (seed 21622586). The 1e-4 relative band was 100 ppm. A different
// nominal frequency rejects, with both frequencies and the offset in ppm.
TEST_CASE("qarlos 2026-11-09: an oscillator's nominal frequency is identity, not a band",
          "[qarlos][crossref]") {
    const json ecs = osc("ECS-3225SMVQ-250-DS-TR", 25000000.0, 1.9999999999999998e-05, 1.62,
                         3.63, -40.0, 125.0, nullptr);
    auto r = run_timing(ecs, json::array({
        osc("SG-8018CA 25.001250M-TJHSA0", 25001250.0, 4.9999999999999996e-05, 1.62, 3.63,
            -40.0, 105.0, nullptr),
        osc("SAME-25M-20PPM", 25000000.0, 2e-05, 1.62, 3.63, -40.0, 125.0, nullptr)}));
    const json* off = cand_by_mpn(r, "SG-8018CA 25.001250M-TJHSA0");
    REQUIRE(off);
    CHECK(verdict_of(*off, "frequency") == "fail");
    CHECK((*off)["status"] == "no_substitute");
    CHECK(has_note(*off, "the original is 25000000 Hz, the substitute 25001250 Hz (+50.0 ppm)"));
    // Equal nominal (and 2e-05 vs 1.9999999999999998e-05 is the same 20 ppm): a pass,
    // and the stability compare is equal, not looser.
    const json* same = cand_by_mpn(r, "SAME-25M-20PPM");
    REQUIRE(same);
    CHECK(verdict_of(*same, "frequency") == "pass");
    CHECK(verdict_of(*same, "stability_ppm") == "pass");
    CHECK((*same)["grade"] == "drop_in");

    const json assvp = osc("ASSVP-14.7456MHZ-R-C16", 14745600.0, 2.4999999999999998e-05, 3.3,
                           3.3, -40.0, 85.0, "standby");
    auto r2 = run_timing(assvp, json::array({osc("SG-8018CA 14.7450M-TJHPA0", 14745000.0,
                                                 4.9999999999999996e-05, 1.62, 3.63, -40.0,
                                                 105.0, nullptr)}));
    const json& c2 = r2["candidates"][0];
    CHECK(c2["status"] == "no_substitute");
    CHECK(has_note(c2, "(-40.7 ppm)"));
}

// Stability (+/- ppm) is a separate axis from the nominal: equal or tighter passes, looser
// by any amount FAILs (the old 2x band warned a 50 ppm part for a 25 ppm one), unstated on
// the substitute is UNVERIFIED and noted (secondary since 2026-11-09: no cap).
TEST_CASE("qarlos 2026-11-09: a looser oscillator stability fails; unstated is unverified",
          "[qarlos][crossref]") {
    const json orig = osc("ASSVP-14.7456MHZ-R-C16", 14745600.0, 2.4999999999999998e-05, 3.3,
                          3.3, -40.0, 85.0, "standby");
    json nostab = osc("NOSTAB", 14745600.0, 0.0, 3.3, 3.3, -40.0, 85.0, "standby");
    nostab["stability_ppm"] = nullptr;
    auto r = run_timing(orig, json::array({
        osc("LOOSER-50", 14745600.0, 4.9999999999999996e-05, 3.3, 3.3, -40.0, 85.0, "standby"),
        osc("TIGHTER-20", 14745600.0, 2e-05, 3.3, 3.3, -40.0, 85.0, "standby"), nostab}));
    const json* loose = cand_by_mpn(r, "LOOSER-50");
    REQUIRE(loose);
    CHECK(verdict_of(*loose, "stability_ppm") == "fail");
    CHECK((*loose)["grade"] == "major_review");
    CHECK(has_note(*loose, "looser frequency stability: the substitute is +/-50 ppm against "
                           "the original's +/-25 ppm"));
    const json* tight = cand_by_mpn(r, "TIGHTER-20");
    REQUIRE(tight);
    CHECK(verdict_of(*tight, "stability_ppm") == "pass");
    const json* none = cand_by_mpn(r, "NOSTAB");
    REQUIRE(none);
    CHECK(verdict_of(*none, "stability_ppm") == "unverified");
    CHECK((*none)["grade"] == "drop_in");  // owner 2026-11-09: stability is secondary
    CHECK(has_note(*none, "the original's frequency stability is +/-25 ppm; the substitute's "
                          "record states none"));
}

// ISM92-3251BH-25.000 MHZ (ILSI, enableFunction outputEnable) took ASFL1-25.000MHZ-LC-T
// (standby) and every other standby part as drop_in with no note (seed 21832044). The pin-1
// function is pinout identity: both stated and different rejects; one side only is
// UNVERIFIED and capped.
TEST_CASE("qarlos 2026-11-09: an oscillator's control-pin function is pinout identity",
          "[qarlos][views][crossref]") {
    json env = {{"timeBase", {{"oscillator", {{"manufacturerInfo", {{"name", "ILSI"},
        {"reference", "ISM92-3251BH-25.000 MHZ"}, {"status", "production"},
        {"datasheetInfo", {{"part", {{"partNumber", "ISM92-3251BH-25.000 MHZ"},
                                     {"package", "5032 4-SMD"}}},
            {"electrical", {{"technology", "crystalOscillator"}, {"frequency", 25000000.0},
                            {"frequencyStability", 4.9999999999999996e-05},
                            {"outputType", "cmos"},
                            {"supply", {{"minimumSupplyVoltage", 3.3},
                                        {"maximumSupplyVoltage", 3.3},
                                        {"currentConsumption", 0.04}}},
                            {"enableFunction", "outputEnable"}}},
            {"thermal", {{"operatingTemperature", {{"minimum", -40.0},
                                                    {"maximum", 85.0}}}}}}}}}}}}}};
    auto row = kelvin::extract_timing(env);
    REQUIRE(row.has_value());
    CHECK(row->enable_function == "outputEnable");

    const json ism = osc("ISM92-3251BH-25.000 MHZ", 25000000.0, 4.9999999999999996e-05, 3.3,
                         3.3, -40.0, 85.0, "outputEnable");
    auto r = run_timing(ism, json::array({
        osc("ASFL1-25.000MHZ-LC-T", 25000000.0, 4.9999999999999996e-05, 3.3, 3.3, -40.0, 85.0,
            "standby"),
        osc("NO-PIN-FN", 25000000.0, 4.9999999999999996e-05, 3.3, 3.3, -40.0, 85.0, nullptr),
        osc("OE-TOO", 25000000.0, 4.9999999999999996e-05, 3.3, 3.3, -40.0, 85.0,
            "outputEnable")}));
    const json* sb = cand_by_mpn(r, "ASFL1-25.000MHZ-LC-T");
    REQUIRE(sb);
    CHECK(verdict_of(*sb, "enable_function") == "fail");
    CHECK((*sb)["status"] == "no_substitute");
    CHECK(has_note(*sb, "control-pin function differs: the original's is 'outputEnable', the "
                        "substitute's 'standby'"));
    const json* unk = cand_by_mpn(r, "NO-PIN-FN");
    REQUIRE(unk);
    CHECK(verdict_of(*unk, "enable_function") == "unverified");
    CHECK((*unk)["status"] != "no_substitute");
    CHECK((*unk)["grade"] != "drop_in");
    CHECK(has_note(*unk, "the original's control pin is 'outputEnable'"));
    const json* oe = cand_by_mpn(r, "OE-TOO");
    REQUIRE(oe);
    CHECK(verdict_of(*oe, "enable_function") == "pass");
    CHECK((*oe)["grade"] == "drop_in");
    // Stated on the substitute only: unverified the other way round, also capped.
    auto r2 = run_timing(osc("ORIG-NOFN", 25000000.0, 5e-05, 3.3, 3.3, -40.0, 85.0, nullptr),
                         json::array({osc("SUB-SB", 25000000.0, 5e-05, 3.3, 3.3, -40.0, 85.0,
                                          "standby")}));
    CHECK(verdict_of(r2["candidates"][0], "enable_function") == "unverified");
    CHECK(r2["candidates"][0]["grade"] != "drop_in");
    CHECK(has_note(r2["candidates"][0], "the substitute's control pin is 'standby'"));
}

// Owner decision 2026-11-09: output enable and tri-state are ONE pin function (both gate
// the output to high impedance with the oscillator running), so OE against triState passes
// with no note; standby is a different function and still rejects with its note. The
// original is the real ISM92-3251BH-25.000 MHZ record (outputEnable). No TAS timing record
// states "triState" today (2,484 outputEnable, 7,247 standby, 1,061 none), so the triState
// side is the same part with only its control-pin function changed.
TEST_CASE("qarlos 2026-11-09: output enable and tri-state are one control-pin function",
          "[qarlos][crossref]") {
    const json ism = osc("ISM92-3251BH-25.000 MHZ", 25000000.0, 4.9999999999999996e-05, 3.3,
                         3.3, -40.0, 85.0, "outputEnable");
    auto r = run_timing(ism, json::array({
        osc("TRISTATE-SAME", 25000000.0, 4.9999999999999996e-05, 3.3, 3.3, -40.0, 85.0,
            "triState"),
        osc("ASFL1-25.000MHZ-LC-T", 25000000.0, 4.9999999999999996e-05, 3.3, 3.3, -40.0, 85.0,
            "standby")}));
    const json* ts = cand_by_mpn(r, "TRISTATE-SAME");
    REQUIRE(ts);
    CHECK(verdict_of(*ts, "enable_function") == "pass");
    CHECK((*ts)["grade"] == "drop_in");
    CHECK_FALSE(has_note(*ts, "control-pin function differs"));
    CHECK_FALSE(has_note(*ts, "control pin is"));
    const json* sb = cand_by_mpn(r, "ASFL1-25.000MHZ-LC-T");
    REQUIRE(sb);
    CHECK(verdict_of(*sb, "enable_function") == "fail");
    CHECK((*sb)["status"] == "no_substitute");
    CHECK(has_note(*sb, "control-pin function differs: the original's is 'outputEnable', the "
                        "substitute's 'standby'"));
    // The same equivalence the other way round: a triState original takes an OE part.
    auto r2 = run_timing(osc("TRISTATE-ORIG", 25000000.0, 5e-05, 3.3, 3.3, -40.0, 85.0,
                             "triState"),
                         json::array({osc("OE-SUB", 25000000.0, 5e-05, 3.3, 3.3, -40.0, 85.0,
                                          "outputEnable"),
                                      osc("NONE-SUB", 25000000.0, 5e-05, 3.3, 3.3, -40.0, 85.0,
                                          "none")}));
    const json* oe = cand_by_mpn(r2, "OE-SUB");
    REQUIRE(oe);
    CHECK(verdict_of(*oe, "enable_function") == "pass");
    const json* none = cand_by_mpn(r2, "NONE-SUB");
    REQUIRE(none);
    CHECK(verdict_of(*none, "enable_function") == "fail");
    CHECK(has_note(*none, "the original's is 'triState', the substitute's 'none'"));
}

// SSM3J132TU (Toshiba, 33 nC) took SiA477EDJT (Vishay, 3.3000000000000004e-08 in its record)
// with Qg 'warn' (seed 20889483): the strict `sv <= ov` read the last-bit difference as a
// higher Qg. Equal is a pass; lower is better; higher still warns / fails.
TEST_CASE("qarlos 2026-11-09: an equal MOSFET Qg is a pass, not a warn", "[qarlos][crossref]") {
    auto orig_row = kelvin::extract_mosfet(mosfet_env("SSM3J132TU", "pChannel",
        {{"drainSourceVoltage", -12.0}, {"gateSourceVoltageMax", 6.0},
         {"continuousDrainCurrent", -5.4}, {"powerDissipation", 0.5},
         {"inputCapacitance", 2.7e-09}, {"totalGateCharge", 3.3e-08}, {"onResistance", 0.017},
         {"onResistanceVgs", 4.5},
         {"gateThresholdVoltage", {{"minimum", -1.0}, {"maximum", -0.3}}}}));
    auto sub_row = kelvin::extract_mosfet(mosfet_env("SiA477EDJT", "pChannel",
        {{"drainSourceVoltage", -12.0}, {"continuousDrainCurrent", -12},
         {"gateSourceVoltageMax", 8}, {"powerDissipation", 19},
         {"outputCapacitance", 7.25e-10}, {"totalGateCharge", 3.3000000000000004e-08},
         {"onResistance", 0.013000000000000001},
         {"gateThresholdVoltage", {{"minimum", -1.0}}}}));
    REQUIRE(orig_row.has_value());
    REQUIRE(sub_row.has_value());
    auto spec = [](const kelvin::MosfetRow& m) {
        // The fields web/src/crossref.js passes for the Qg compare (qg = qg_total).
        return json{{"mpn", m.mpn}, {"vds", m.vds_rated}, {"id", m.id_continuous},
                    {"rds_on", m.rds_on}, {"qg", m.qg_total}, {"channel", m.channel}};
    };
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    auto r = kelvin::crossref::cross_reference("mosfet", spec(*orig_row),
                                               json::array({spec(*sub_row)}), opt);
    CHECK(verdict_of(r["candidates"][0], "qg") == "pass");
    // Direction is unchanged: a real increase still warns, a decrease passes.
    json higher = spec(*sub_row);
    higher["mpn"] = "HIGHER";
    higher["qg"] = 4.0e-08;
    json lower = spec(*sub_row);
    lower["mpn"] = "LOWER";
    lower["qg"] = 2.0e-08;
    auto r2 = kelvin::crossref::cross_reference("mosfet", spec(*orig_row),
                                                json::array({higher, lower}), opt);
    const json* hi = cand_by_mpn(r2, "HIGHER");
    const json* lo = cand_by_mpn(r2, "LOWER");
    REQUIRE(hi);
    REQUIRE(lo);
    CHECK(verdict_of(*hi, "qg") == "warn");
    CHECK(verdict_of(*lo, "qg") == "pass");
}

// ── Qarlos rounds 5-7 (2026-11-09), magnetic + capacitor ─────────────────────
namespace {
json mag_env(const char* mfr, const char* mpn, json elec) {
    return json{{"magnetic", {{"manufacturerInfo", {{"name", mfr}, {"reference", mpn},
        {"datasheetInfo", {{"electrical", json::array({elec})}}}}}}}};
}
json cap_env(const char* mfr, const char* mpn, json part, json mech, json elec) {
    part["partNumber"] = mpn;
    return json{{"capacitor", {{"manufacturerInfo", {{"name", mfr}, {"reference", mpn},
        {"datasheetInfo", {{"part", part}, {"mechanical", mech}, {"electrical", elec}}}}}}}};
}
// The magnetic spec block exactly as web/src/crossref.js builds it from the browse row.
json mag_spec(const kelvin::MagneticRow& r) {
    json row;
    kelvin::browse::extra_row_json(r, row);
    auto nz = [](double v) { return std::isnan(v) || !(v > 0) ? json(nullptr) : json(v); };
    return json{{"mpn", r.mpn}, {"value_si", nz(r.inductance)},
                {"saturation_current", nz(r.saturation_current)},
                {"saturation_points", row["saturation_points"]},
                {"rated_current", nz(r.rated_current)}, {"dcr", nz(r.dcr)}, {"srf", nz(r.srf)}};
}
}  // namespace

// Owner decision A: the v20 row carries the whole saturationCurrents table, and the real pair
// BWVS00606045150T00 (Pulse, 3.06 A, no criterion) vs MSS6132T-123MLC (Coilcraft 1.3 / 1.5 /
// 1.7 A at 10 / 20 / 30 %, TAS record values) is decided by isat::compare as a reject.
TEST_CASE("qarlos r5-7: the magnetic row carries the whole Isat table; the real pair rejects",
          "[qarlos][views][crossref][isat]") {
    auto mss = kelvin::extract_magnetic(mag_env("Coilcraft", "MSS6132T-123MLC",
        {{"subtype", "inductor"}, {"inductance", {{"nominal", 1.2e-05}}},
         {"dcResistance", {{"nominal", 0.079}}},
         {"saturationCurrents", json::array({{{"percentInductanceDrop", 10.0}, {"current", 1.3}},
                                             {{"percentInductanceDrop", 20.0}, {"current", 1.5}},
                                             {{"percentInductanceDrop", 30.0}, {"current", 1.7}}})}}));
    auto bwvs = kelvin::extract_magnetic(mag_env("Pulse Electronics", "BWVS00606045150T00",
        {{"subtype", "inductor"},
         {"inductance", {{"nominal", 1.5e-05}, {"minimum", 1.05e-05}, {"maximum", 1.95e-05}}},
         {"dcResistance", {{"nominal", 0.08}}}, {"ratedCurrents", {1.98}},
         {"saturationCurrentPeak", 3.06}}));
    REQUIRE(mss.has_value());
    REQUIRE(bwvs.has_value());
    REQUIRE(mss->sat_table_current.size() == 3);
    CHECK(mss->sat_table_current[2] == 1.7);
    CHECK(mss->sat_table_percent_drop[2] == 30.0);
    CHECK(std::isnan(mss->sat_table_temperature[0]));
    REQUIRE(bwvs->sat_table_current.size() == 1);  // the unqualified scalar, criterion unknown
    CHECK(std::isnan(bwvs->sat_table_percent_drop[0]));

    const json o = mag_spec(*bwvs), c = mag_spec(*mss);
    CHECK(c["saturation_points"].size() == 3);
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    auto r = kelvin::crossref::cross_reference("magnetic", o, json::array({c}), opt);
    CHECK(verdict_of(r["candidates"][0], "saturation_current") == "fail");
    CHECK(r["candidates"][0]["status"] == "no_substitute");
}

// Finding r5.3 (seed 21098941): AIML-0402HP-R20K-T (SRF 200 MHz) graded drop_in for TDK
// MHQ1005PR20GTD25 (SRF 900 MHz): the magnetic table had no SRF entry and crossref.js passed none.
TEST_CASE("qarlos r5-7: a magnetic SRF well below the original's fails",
          "[qarlos][views][crossref]") {
    auto mhq = kelvin::extract_magnetic(mag_env("TDK", "MHQ1005PR20GTD25",
        {{"subtype", "inductor"},
         {"inductance", {{"nominal", 2e-07}, {"minimum", 1.96e-07}, {"maximum", 2.04e-07}}},
         {"dcResistance", {{"maximum", 6.2}, {"nominal", 5.06}}},
         {"selfResonantFrequency", 900000000.0}, {"ratedCurrents", {0.08}}}));
    auto aiml = kelvin::extract_magnetic(mag_env("Abracon", "AIML-0402HP-R20K-T",
        {{"subtype", "inductor"}, {"inductance", {{"nominal", 2e-07}}},
         {"dcResistance", {{"maximum", 0.58}}}, {"ratedCurrents", {0.4}},
         {"saturationCurrentPeak", 0.48}, {"selfResonantFrequency", 200000000.0}}));
    REQUIRE(mhq.has_value());
    REQUIRE(aiml.has_value());
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    auto r = kelvin::crossref::cross_reference("magnetic", mag_spec(*mhq),
                                               json::array({mag_spec(*aiml)}), opt);
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "srf") == "fail");
    CHECK(c["grade"] != "drop_in");
    CHECK(c["direction"] != "upgrade");
    CHECK(has_note(c, "self-resonant frequency 200 MHz vs the original's 900 MHz"));
    // inside the 0.8x band warns; at or above the original passes
    json near = mag_spec(*aiml);
    near["srf"] = 8.0e8;
    auto rn = kelvin::crossref::cross_reference("magnetic", mag_spec(*mhq), json::array({near}), opt);
    CHECK(verdict_of(rn["candidates"][0], "srf") == "warn");
    near["srf"] = 1.0e9;
    auto rp = kelvin::crossref::cross_reference("magnetic", mag_spec(*mhq), json::array({near}), opt);
    CHECK(verdict_of(rp["candidates"][0], "srf") == "pass");
}

// Owner decision B: 100 Hz and 120 Hz ripple ratings are one basis. Rubycon
// 475VXH330MEFCSN35X35 (1.63 A @ 120 Hz) vs Vishay MAL226939331E3 (1.75 A @ 100 Hz), TAS values.
TEST_CASE("qarlos r5-7: a 100 Hz and a 120 Hz ripple rating are compared directly",
          "[qarlos][crossref]") {
    auto cap = [](const char* mpn, double v, double ir, double f) {
        return json{{"mpn", mpn}, {"value_si", 3.3e-4}, {"voltage", v},
                    {"technology", "aluminum-electrolytic-wet"}, {"ripple_current", ir},
                    {"ripple_current_frequency", f}};
    };
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    auto r = kelvin::crossref::cross_reference(
        "capacitor", cap("475VXH330MEFCSN35X35", 475.0, 1.63, 120.0),
        json::array({cap("MAL226939331E3", 500.0, 1.75, 100.0)}), opt);
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "ripple_current") == "pass");
    CHECK_FALSE(has_note(c, "different frequencies"));
    // reversed: a 120 Hz substitute below a 100 Hz original is a real shortfall
    auto r2 = kelvin::crossref::cross_reference(
        "capacitor", cap("MAL226939331E3", 500.0, 1.75, 100.0),
        json::array({cap("LOW", 500.0, 1.2, 120.0)}), opt);
    CHECK(verdict_of(r2["candidates"][0], "ripple_current") == "fail");
    // every other difference stays unverified
    auto r3 = kelvin::crossref::cross_reference(
        "capacitor", cap("475VXH330MEFCSN35X35", 475.0, 1.63, 120.0),
        json::array({cap("KHZ", 500.0, 1.75, 1000.0)}), opt);
    CHECK(verdict_of(r3["candidates"][0], "ripple_current") == "unverified");
}

// Findings r6.1 / r6.3: an axial body's length is its length and its diameter its width and
// height (A50IQ4150AA60K, C114K222K1X5CA; TAS values).
TEST_CASE("qarlos r5-7: an axial capacitor lies flat", "[qarlos][views]") {
    auto a50 = kelvin::extract_capacitor(cap_env("KEMET", "A50IQ4150AA60K",
        {{"technology", "film-polypropylene"}},
        {{"shape", {{"assembly", "THT"}, {"shapeType", "Axial"}}},
         {"dimensions", {{"length", {{"nominal", 0.028}}}, {"diameter", {{"nominal", 0.01}}}}}},
        {{"capacitance", {{"nominal", 1.5e-06}}}, {"ratedVoltage", 250.0}}));
    REQUIRE(a50.has_value());
    CHECK(a50->length_m == Approx(0.028));
    CHECK(a50->width_m == Approx(0.01));
    CHECK(a50->height_m == Approx(0.01));
    auto c114 = kelvin::extract_capacitor(cap_env("KEMET", "C114K222K1X5CA",
        {{"technology", "ceramic-class-2"}},
        {{"shape", {{"assembly", "THT"}, {"shapeType", "Axial"}}},
         {"dimensions", {{"length", {{"nominal", 0.00406}}}, {"diameter", {{"nominal", 0.00229}}}}}},
        {{"capacitance", {{"nominal", 2.2e-09}}}, {"ratedVoltage", 100.0}}));
    REQUIRE(c114.has_value());
    CHECK(c114->length_m == Approx(0.00406));
    CHECK(c114->height_m == Approx(0.00229));
    // an upright radial can keeps the can fold (diameter x diameter, standing its length)
    auto can = kelvin::extract_capacitor(cap_env("Rubycon", "475VXH330MEFCSN35X35",
        {{"technology", "aluminum-electrolytic-wet"}},
        {{"shape", {{"assembly", "Snap-In"}, {"shapeType", "Radial Cylindrical"}}},
         {"dimensions", {{"length", {{"nominal", 0.04}}}, {"diameter", {{"nominal", 0.035}}}}}},
        {{"capacitance", {{"nominal", 3.3e-4}}}, {"ratedVoltage", 475.0}}));
    REQUIRE(can.has_value());
    CHECK(can->height_m == Approx(0.04));
    CHECK(can->length_m == Approx(0.035));
}

// Finding r6.2 (seed 21517857): KEMET C322C270JAG5TA (C0G) took Vishay WKO330KCPQE0KR (N750)
// as a minor_review upgrade with the dielectric verdict 'unverified'.
TEST_CASE("qarlos r5-7: a different class-1 temperature coefficient fails",
          "[qarlos][crossref]") {
    auto cap = [](const char* mpn, const char* code, double v) {
        return json{{"mpn", mpn}, {"value_si", 2.7e-11}, {"voltage", v},
                    {"technology", "ceramic-class-1"}, {"dielectric_code", code}};
    };
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    auto run = [&](const char* o, const char* s) {
        return kelvin::crossref::cross_reference("capacitor", cap("ORIG", o, 250.0),
                                                 json::array({cap("SUB", s, 440.0)}), opt)
            ["candidates"][0];
    };
    {
        json c = run("C0G", "N750");
        CHECK(verdict_of(c, "dielectric_code") == "fail");
        CHECK(c["grade"] == "major_review");
        CHECK(c["direction"] != "upgrade");
        CHECK(has_note(c, "class-1 temperature coefficient differs: N750 (-750 ppm/K) for C0G"));
    }
    CHECK(verdict_of(run("C0G", "P100"), "dielectric_code") == "fail");
    CHECK(verdict_of(run("C0G", "U2J"), "dielectric_code") == "fail");
    CHECK(verdict_of(run("C0G", "C0H"), "dielectric_code") == "fail");  // wider tolerance
    CHECK(verdict_of(run("C0G", "NP0"), "dielectric_code") == "pass");
    CHECK(verdict_of(run("C0G", "BP"), "dielectric_code") == "pass");   // MIL 0 +/-30
    CHECK(verdict_of(run("C0H", "C0G"), "dielectric_code") == "pass");  // tighter
    CHECK(verdict_of(run("X7R", "Y5V"), "dielectric_code") == "fail");
    // same coefficient, a tolerance one code does not state: undecided and noted (secondary,
    // no cap since 2026-11-09)
    {
        json c = run("U2J", "N750");
        CHECK(verdict_of(c, "dielectric_code") == "unverified");
        CHECK(c["grade"] == "drop_in");  // owner 2026-11-09: dielectric is secondary
        CHECK(has_note(c, "dielectric codes differ (U2J -> N750)"));
    }
    // an undecodable code against a stated one is never silent
    CHECK(has_note(run("C0G", "SL"), "dielectric codes differ (C0G -> SL)"));
}

// Finding r7.1 (seed 21936773): CDR37BR331AJUMAC "0402" against Murata "1005M/0402" was a
// "different package/case".
TEST_CASE("qarlos r5-7: an explicitly metric chip code is its imperial equivalent",
          "[qarlos][crossref][dims]") {
    using kelvin::crossref::case_identity;
    CHECK(case_identity("capacitor", "1005M/0402") == case_identity("capacitor", "0402"));
    CHECK(case_identity("capacitor", "1005M") == "0402");
    CHECK(case_identity("capacitor", "0603M/0201") == "0201");
    CHECK(case_identity("resistor", "1608M/0603") == "0603");
    CHECK(case_identity("capacitor", "1005") != "0402");             // unmarked: never guessed
    CHECK(case_identity("capacitor", "0603") == "0603");             // bare 0603 stays itself
    CHECK(case_identity("capacitor", "1005M/0603") == "1005M0603");  // contradicts itself
    auto cap = [](const char* mpn, const char* cs, double l, double w) {
        return json{{"mpn", mpn}, {"value_si", 3.3e-10}, {"voltage", 50.0},
                    {"technology", "ceramic-class-2"}, {"dielectric_code", "X7R"},
                    {"case_code", cs}, {"mount", "SMT"}, {"length_m", l}, {"width_m", w}};
    };
    auto r = kelvin::crossref::cross_reference(
        "capacitor", cap("CDR37BR331AJUMAC", "0402", 0.00102, 0.00051),
        json::array({cap("GRM155R71H331KA01", "1005M/0402", 0.001, 0.0005)}));
    const json& c = r["candidates"][0];
    CHECK_FALSE(has_note(c, "different package/case"));
    CHECK(c.value("footprint", "") != "different_case");
}

// Finding r7.1 (seed 21936773): FG26C0G2A223JNT06 states L 5.5, W 6.0, T 3.5 mm (THT disc);
// T was dropped and the L x W face became the land, while L x T x H candidates were boxed on
// their real land.
// Owner decision 2026-11-09 (verify the convention before it counts): 00f06be then read every
// such box with KEMET's convention (W across the leads, L upward: KEMET C1119 S-Series p.2,
// C1116 V-Series p.2). TDK's FG catalogue (leadmlcc_halogenfree_fg_en 20181102) draws the
// FG26 (p.14) with L = 5.5 across the leads and W = 6.0 upward, and Murata's RDE sheet does
// the same, so that read turned TDK's and Murata's boxes on their side. The record does not
// say which convention it uses, so the box is AMBIGUOUS: no land, no height, flagged, and the
// cross-reference reports the footprint UNVERIFIED (next test). Same fixture as before; the
// assertions changed from 00f06be's guessed orientation to "ambiguous".
TEST_CASE("qarlos r5-7: a leaded L x W x T box with no height keeps its thickness",
          "[qarlos][views]") {
    auto fg = kelvin::extract_capacitor(cap_env("TDK", "FG26C0G2A223JNT06",
        {{"series", "FG"}, {"technology", "ceramic-class-1"}, {"case", ""}, {"dielectricCode", "C0G"}},
        {{"dimensions", {{"length", {{"maximum", 0.0055}}}, {"pitch", {{"nominal", 0.005}}},
                         {"thickness", {{"maximum", 0.0035}}}, {"width", {{"maximum", 0.006}}}}},
         {"shape", {{"assembly", "THT"}, {"shapeType", "Disc"}}}},
        {{"capacitance", {{"nominal", 2.2e-08}}}, {"ratedVoltage", 100.0}}));
    REQUIRE(fg.has_value());
    CHECK(fg->box_axes_ambiguous);
    CHECK(std::isnan(fg->length_m));
    CHECK(std::isnan(fg->width_m));
    CHECK(std::isnan(fg->height_m));
    CHECK(fg->lead_pitch_m == Approx(0.005));
    // KEMET C322C270JAG5TA states L x T x H: unchanged (land L x T, height H)
    auto c322 = kelvin::extract_capacitor(cap_env("KEMET", "C322C270JAG5TA",
        {{"technology", "ceramic-class-1"}, {"case", "C322"}, {"dielectricCode", "C0G"}},
        {{"shape", {{"assembly", "THT"}, {"shapeType", "Radial"}}},
         {"dimensions", {{"length", {{"maximum", 0.00508}}}, {"thickness", {{"maximum", 0.00318}}},
                         {"height", {{"maximum", 0.0066}}}, {"pitch", {{"nominal", 0.00508}}}}}},
        {{"capacitance", {{"nominal", 2.7e-11}}}, {"ratedVoltage", 250.0}}));
    REQUIRE(c322.has_value());
    CHECK(c322->length_m == Approx(0.00508));
    CHECK(c322->width_m == Approx(0.00318));
    CHECK(c322->height_m == Approx(0.0066));
    CHECK_FALSE(c322->box_axes_ambiguous);
}

// Owner decision 2026-11-09: the L x W x T radial box is ambiguous for every vendor, whatever
// its own convention, because the record does not state it. Real TAS records:
//   Murata RDER72E105MUB1H03B (L 7.7, W 12.5, T 4.0 mm; the Murata sheet's drawing has L
//     across the leads, W upward) and KEMET VRR07L103KGS (L 7.62, W 7.62, T 3.81 mm; KEMET
//     C1116 p.2 has W across, L upward) are both flagged and carry no box;
//   KEMET SRA25B104KGS, an AXIAL box (shapeType "Axial Rectangular"; KEMET C1119 p.2 axial
//     view: L along the axis, W across, T upward) is NOT ambiguous: land L x W, height T.
// The ranker then reports an ambiguous side's footprint UNVERIFIED with its note, and an
// established lead-pitch mismatch still rules first.
TEST_CASE("qarlos 2026-11-09: an L x W x T radial box with no height is ambiguous; footprint "
          "unverified",
          "[qarlos][views][crossref][dims]") {
    auto rde = kelvin::extract_capacitor(cap_env("Murata", "RDER72E105MUB1H03B",
        {{"series", "RDE"}, {"technology", "ceramic-class-2"}, {"dielectricCode", "X7R"}},
        {{"shape", {{"assembly", "THT"}, {"shapeType", "Radial Leaded"}}},
         {"dimensions", {{"length", {{"maximum", 0.0077}}}, {"width", {{"maximum", 0.0125}}},
                         {"thickness", {{"maximum", 0.004}}}, {"pitch", {{"nominal", 0.005}}},
                         {"pinDiameter", {{"nominal", 0.0005}}}}}},
        {{"capacitance", {{"nominal", 1e-06}}}, {"ratedVoltage", 250.0}}));
    REQUIRE(rde.has_value());
    CHECK(rde->box_axes_ambiguous);
    CHECK(std::isnan(rde->length_m));
    CHECK(std::isnan(rde->height_m));
    auto vrr = kelvin::extract_capacitor(cap_env("KEMET", "VRR07L103KGS",
        {{"series", "VRR Indust X7R HVHT200C"}, {"technology", "ceramic-class-2"},
         {"dielectricCode", "X7R"}},
        {{"shape", {{"assembly", "THT"}, {"shapeType", "Radial"}}},
         {"dimensions", {{"length", {{"maximum", 0.00762}}}, {"thickness", {{"maximum", 0.00381}}},
                         {"width", {{"maximum", 0.00762}}},
                         {"pitch", {{"nominal", 0.00508}, {"minimum", 0.00432}, {"maximum", 0.00584}}},
                         {"pinDiameter", {{"nominal", 0.000635}}}, {"pinLength", {{"nominal", 0.03175}}}}}},
        {{"capacitance", {{"nominal", 1e-08}}}, {"ratedVoltage", 500.0}}));
    REQUIRE(vrr.has_value());
    CHECK(vrr->box_axes_ambiguous);
    CHECK(std::isnan(vrr->width_m));
    auto sra = kelvin::extract_capacitor(cap_env("KEMET", "SRA25B104KGS",
        {{"series", "SRA Comm X7R"}, {"technology", "ceramic-class-2"}, {"dielectricCode", "X7R"}},
        {{"shape", {{"assembly", "THT"}, {"shapeType", "Axial Rectangular"}}},
         {"dimensions", {{"length", {{"maximum", 0.00686}}}, {"thickness", {{"maximum", 0.00254}}},
                         {"width", {{"maximum", 0.00254}}}, {"pinDiameter", {{"nominal", 0.000508}}},
                         {"pinLength", {{"nominal", 0.03175}}}}}},
        {{"capacitance", {{"nominal", 1e-07}}}, {"ratedVoltage", 50.0}}));
    REQUIRE(sra.has_value());
    CHECK_FALSE(sra->box_axes_ambiguous);
    CHECK(sra->length_m == Approx(0.00686));
    CHECK(sra->width_m == Approx(0.00254));
    CHECK(sra->height_m == Approx(0.00254));

    // The ranker: the spec web/src/crossref.js base() builds from the browse row
    // (boxAxesAmbiguous -> box_axes_ambiguous).
    auto spec = [](const kelvin::CapacitorRow& r, double c, double v) {
        auto nz = [](double x) { return std::isnan(x) || !(x > 0) ? json(nullptr) : json(x); };
        return json{{"mpn", r.mpn}, {"value_si", c}, {"voltage", v},
                    {"technology", "ceramic-class-2"}, {"dielectric_code", "X7R"},
                    {"mount", r.mount}, {"case_code", r.case_code},
                    {"length_m", nz(r.length_m)}, {"width_m", nz(r.width_m)},
                    {"height_m", nz(r.height_m)}, {"lead_pitch_m", nz(r.lead_pitch_m)},
                    {"box_axes_ambiguous", r.box_axes_ambiguous}};
    };
    // A same-pitch radial box stated L x T x H (unambiguous) for the Murata original.
    json boxed = {{"mpn", "BOXED-5MM"}, {"value_si", 1e-06}, {"voltage", 250.0},
                  {"technology", "ceramic-class-2"}, {"dielectric_code", "X7R"},
                  {"mount", "THT"}, {"length_m", 0.0077}, {"width_m", 0.004},
                  {"height_m", 0.0125}, {"lead_pitch_m", 0.005}};
    json wide = boxed;
    wide["mpn"] = "BOXED-10MM";
    wide["lead_pitch_m"] = 0.01;
    auto r = kelvin::crossref::cross_reference("capacitor", spec(*rde, 1e-06, 250.0),
                                               json::array({boxed, wide}));
    const json* same = cand_by_mpn(r, "BOXED-5MM");
    REQUIRE(same);
    CHECK((*same)["footprint"] == "unknown");
    CHECK(verdict_of(*same, "footprint") == "unverified");
    CHECK((*same)["grade"] != "drop_in");
    CHECK(has_note(*same, "the original's record draws its body as L x W x T with no height"));
    const json* far = cand_by_mpn(r, "BOXED-10MM");
    REQUIRE(far);
    CHECK((*far)["footprint"] == "different_land_pattern");
    CHECK(has_note(*far, "lead pitch differs"));
    // The other way round: the ambiguous KEMET part offered for an unambiguous original.
    json orig = boxed;
    orig["mpn"] = "ORIG-L-T-H";
    orig["value_si"] = 1e-08;
    orig["voltage"] = 500.0;
    orig["lead_pitch_m"] = 0.00508;
    auto r2 = kelvin::crossref::cross_reference("capacitor", orig,
                                                json::array({spec(*vrr, 1e-08, 500.0)}));
    const json& c2 = r2["candidates"][0];
    CHECK(c2["footprint"] == "unknown");
    CHECK(has_note(c2, "the substitute's record draws its body as L x W x T with no height"));
}

// Upright disc capacitors that state only a diameter and a thickness (KEMET 1,577, TDK 871,
// Vishay 60 records) were folded like a can: a diameter x diameter land with no height, T lost.
// A disc stands on its leads in its own plane: footprint D x T, height D. Drawings: Vishay D
// Series (doc 28549) p.2 "Lead configuration"; KEMET C900 (C1065) p.3 "Lead Configurations";
// TDK CK45 (e413_ck45) p.3/3. Real TAS records.
TEST_CASE("qarlos 2026-11-09: an upright disc's footprint is diameter x thickness, its height "
          "the diameter",
          "[qarlos][views]") {
    auto d101 = kelvin::extract_capacitor(cap_env("Vishay", "D101K20Y5PL63J5R",
        {{"series", "D Series"}, {"technology", "ceramic-class-2"}, {"dielectricCode", "Y5P"}},
        {{"dimensions", {{"diameter", {{"maximum", 0.005}}}, {"thickness", {{"maximum", 0.0035}}},
                         {"pitch", {{"nominal", 0.005}}}, {"pinDiameter", {{"nominal", 0.0006}}}}},
         {"shape", {{"assembly", "THT"}, {"shapeType", "Disc"}}}},
        {{"capacitance", {{"nominal", 1e-10}}}, {"ratedVoltage", 500.0}}));
    REQUIRE(d101.has_value());
    CHECK(d101->length_m == Approx(0.005));
    CHECK(d101->width_m == Approx(0.0035));
    CHECK(d101->height_m == Approx(0.005));
    CHECK_FALSE(d101->box_axes_ambiguous);
    auto c901 = kelvin::extract_capacitor(cap_env("KEMET", "C901U100DZNDAA7317",
        {{"series", "C900AC SFTY X1-440 Y2-300"}, {"technology", "ceramic-class-1"},
         {"case", "7x5mm"}, {"dielectricCode", "C0G"}},
        {{"shape", {{"assembly", "THT"}, {"shapeType", "Radial Disc"}}},
         {"dimensions", {{"thickness", {{"maximum", 0.005}}}, {"diameter", {{"maximum", 0.007}}},
                         {"pitch", {{"nominal", 0.01}, {"minimum", 0.009}, {"maximum", 0.011}}},
                         {"pinDiameter", {{"nominal", 0.0005}}}, {"pinLength", {{"nominal", 0.02}}}}}},
        {{"capacitance", {{"nominal", 1e-11}}}, {"voltageRatedAcMax", 440.0}}));
    REQUIRE(c901.has_value());
    CHECK(c901->length_m == Approx(0.007));
    CHECK(c901->width_m == Approx(0.005));
    CHECK(c901->height_m == Approx(0.007));
    auto ck45 = kelvin::extract_capacitor(cap_env("TDK", "CK45-B3AD101KYGNA",
        {{"series", "CK"}, {"technology", "ceramic-class-2"}, {"case", ""}, {"dielectricCode", "B"}},
        {{"dimensions", {{"diameter", {{"maximum", 0.0055}}}, {"pitch", {{"nominal", 0.005}}},
                         {"thickness", {{"maximum", 0.005}}}}},
         {"shape", {{"assembly", "THT"}, {"shapeType", "Disc"}}}},
        {{"capacitance", {{"nominal", 1e-10}}}, {"ratedVoltage", 1000.0}}));
    REQUIRE(ck45.has_value());
    CHECK(ck45->length_m == Approx(0.0055));
    CHECK(ck45->width_m == Approx(0.005));
    CHECK(ck45->height_m == Approx(0.0055));
}

// Findings r5.3 / r7.3 (seeds 21098941 / 22146231): a Screw Type part (CAS shape.assembly)
// against a radial-leaded one is an established mount mismatch, and the note names the stated
// mount, never "not stated".
TEST_CASE("qarlos r5-7: screw-terminal against radial-leaded is a mount mismatch",
          "[qarlos][crossref][dims]") {
    auto cap = [](const char* mpn, const char* mount) {
        return json{{"mpn", mpn}, {"value_si", 3e-11}, {"voltage", 2000.0},
                    {"technology", "ceramic-class-1"}, {"mount", mount}};
    };
    auto r = kelvin::crossref::cross_reference("capacitor", cap("C315C300JDG5TA7303", "THT"),
                                               json::array({cap("RA012020BB30036BF1", "Screw Type")}));
    const json& c = r["candidates"][0];
    CHECK(c["status"] == "no_substitute");
    CHECK(verdict_of(c, "mounting") == "fail");
    CHECK(has_note(c, "mount type incompatible: THT -> Screw Type"));
    CHECK_FALSE(has_note(c, "not stated"));
    // LNY2W472MSEH (Screw Type) against a screw-terminal can: same class
    auto r2 = kelvin::crossref::cross_reference("capacitor", cap("LNY2W472MSEH", "Screw Type"),
                                                json::array({cap("SCREWCAN", "Screw Type")}));
    CHECK(verdict_of(r2["candidates"][0], "mounting") != "fail");
    // a record with no mount but a "Screw-Terminal" package string is a screw part, not leaded
    CHECK(kelvin::crossref::mount_class("", "Screw-Terminal") == "screw");
}


// ── Qarlos v20 re-test 2026-11-09 (connector identities stated on one side) ──
// Spec blocks exactly as web/src/crossref.js builds them from the shard rows of the named
// TAS records (connectors.ndjson): every unstated optional identity is null, never "".
namespace {
json conn_spec(const std::string& mpn, json fields) {
    json o = {{"mpn", mpn},           {"length_m", nullptr},       {"width_m", nullptr},
              {"height_m", nullptr},  {"case_code", ""},           {"mount", ""},
              {"lead_pitch_m", nullptr}, {"family", ""},           {"positions", nullptr},
              {"polarity", nullptr},  {"interface_standard", nullptr}, {"pitch_mm", nullptr},
              {"rows", nullptr},      {"rated_current_A", nullptr}, {"rated_voltage_V", nullptr},
              {"temp_min_C", nullptr}, {"temp_max_C", nullptr},    {"contact_plating", nullptr},
              {"termination", nullptr}, {"mounting", nullptr},     {"orientation", nullptr},
              {"pluggable", nullptr}, {"mating_cycles", nullptr}};
    o.update(fields);
    if (o["mounting"].is_string()) o["mount"] = o["mounting"];
    return o;
}
json conn_one(const json& orig, const json& cand) {
    kelvin::crossref::Options opt;
    opt.max_results = 10;
    auto r = kelvin::crossref::cross_reference("connector", orig, json::array({cand}), opt);
    REQUIRE(r["candidates"].size() == 1);
    return r["candidates"][0];
}
// HIF3B-10D-2.54R(63) (Hirose): an IDC socket, termination stated.
json kHIF3B() {
    return conn_spec("HIF3B-10D-2.54R(63)",
                     {{"family", "wireToBoard"}, {"positions", 10.0}, {"polarity", "female"},
                      {"pitch_mm", 2.54}, {"rows", 2.0}, {"rated_current_A", 1.0},
                      {"rated_voltage_V", 200.0}, {"temp_min_C", -55.0}, {"temp_max_C", 85.0},
                      {"termination", "idc"}, {"mating_cycles", 500.0}});
}
// 66925-005LF (Amphenol CS): the same arrangement, no termination on record.
json k66925() {
    return conn_spec("66925-005LF",
                     {{"family", "wireToBoard"}, {"positions", 10.0}, {"polarity", "female"},
                      {"pitch_mm", 2.54}, {"rows", 2.0}, {"rated_current_A", 3.0}});
}
}  // namespace

// Seed 20575296: HIF3B-10D-2.54R(63) (IDC) took 66925-005LF, which states no termination,
// as a clean drop_in with termination 'unverified'. An unverified termination never
// coexists with drop_in; a stated different one rejects.
TEST_CASE("qarlos v20: a connector termination stated on one side caps below drop_in",
          "[qarlos][crossref]") {
    const json c = conn_one(kHIF3B(), k66925());
    CHECK(verdict_of(c, "termination") == "unverified");
    CHECK(has_note(c, "wire termination not established (original: idc, substitute: not stated)"));
    CHECK(c["grade"] == "minor_review");
    // Stated on both sides and different: rejected, and the note says what changed.
    json crimp = k66925();
    crimp["termination"] = "crimp";
    const json x = conn_one(kHIF3B(), crimp);
    CHECK(x["status"] == "no_substitute");
    CHECK(verdict_of(x, "termination") == "fail");
    CHECK(has_note(x, "wire termination differs: idc -> crimp"));
    // Same termination on both: passes and stays a drop-in.
    json idc = k66925();
    idc["termination"] = "idc";
    const json same = conn_one(kHIF3B(), idc);
    CHECK(verdict_of(same, "termination") == "pass");
    CHECK(same["grade"] == "drop_in");
}

// Seed 20365838: D38999/20MG11BN (female, panel-mount receptacle) took YDIV46E21-11SNC009,
// a record stating neither gender nor mount ('Wire-to-Wire ... Plug' only in its description).
TEST_CASE("qarlos v20: a connector gender stated on one side is noted and capped",
          "[qarlos][crossref]") {
    const json orig = conn_spec("D38999/20MG11BN",
                                {{"family", "circular"}, {"positions", 11.0},
                                 {"polarity", "female"}, {"rated_current_A", 23.0},
                                 {"rated_voltage_V", 600.0}, {"mounting", "panel"}});
    const json cand = conn_spec("YDIV46E21-11SNC009",
                                {{"family", "circular"}, {"positions", 11.0},
                                 {"rated_current_A", 23.0}, {"temp_min_C", -65.0},
                                 {"temp_max_C", 200.0}});
    const json c = conn_one(orig, cand);
    CHECK(verdict_of(c, "polarity") == "unverified");
    CHECK(has_note(c, "mating gender not established (original: female, substitute: not stated)"));
    CHECK(verdict_of(c, "mounting") == "unverified");
    CHECK(has_note(c, "the original is panel-mounted"));
    CHECK(c["grade"] == "minor_review");
    // Gender stated and different: rejected with a note naming both.
    json male = cand;
    male["polarity"] = "male";
    const json m = conn_one(orig, male);
    CHECK(m["status"] == "no_substitute");
    CHECK(has_note(m, "mating gender differs: female -> male"));
    // A board-mounted part for the panel-mount original: a different attach, rejected.
    json board = cand;
    board["mounting"] = "tht";
    board["mount"] = "tht";
    const json b = conn_one(orig, board);
    CHECK(b["status"] == "no_substitute");
    CHECK(verdict_of(b, "mounting") == "fail");
}

// Seed 21413128: 713492023 (Molex C-Grid, no pitch / orientation / mount on record —
// 'Vertical, Surface Mount' is description text only) took the right-angle TE header
// 7-102570-2 (pitch 2.54 mm stated, no orientation / mount) as a clean drop_in, while the
// Sullins parts that state an orientation and mount were capped. Both sides are silent on
// orientation and mount, so those stay silent; the pitch is stated by the substitute only.
TEST_CASE("qarlos v20: a contact pitch stated on one side is noted and capped",
          "[qarlos][crossref]") {
    const json orig = conn_spec("713492023",
                                {{"family", "pinHeaderSocket"}, {"positions", 50.0},
                                 {"polarity", "male"}, {"rated_current_A", 3.0},
                                 {"rated_voltage_V", 250.0}, {"temp_min_C", -40.0},
                                 {"temp_max_C", 105.0}});
    const json cand = conn_spec("7-102570-2",
                                {{"family", "pinHeaderSocket"}, {"positions", 50.0},
                                 {"polarity", "male"}, {"pitch_mm", 2.54},
                                 {"rated_current_A", 3.0}, {"temp_min_C", -65.0},
                                 {"temp_max_C", 105.0}});
    const json c = conn_one(orig, cand);
    CHECK(verdict_of(c, "pitch_mm") == "unverified");
    CHECK(has_note(c, "contact pitch not established (original: not stated, substitute: 2.54 mm)"));
    CHECK(verdict_of(c, "orientation") == "");  // neither states it: silent
    CHECK(c["grade"] == "minor_review");
}

// Seeds 21622586 / 21727315 / 21832044: a pitch that differs on the RECORDS fails; a pitch
// that appears only in a candidate's description ('6.35mm Pitch') is not on record, so the
// pitch is unverified — now with a note and the drop_in cap (the description is TAS #2337).
TEST_CASE("qarlos v20: a stated pitch difference fails, a description-only pitch is unverified",
          "[qarlos][crossref]") {
    // 734-232/105-604 (WAGO MCS MINI): 3.81 mm, 2 positions.
    const json orig = conn_spec("734-232/105-604",
                                {{"family", "pinHeaderSocket"}, {"positions", 2.0},
                                 {"pitch_mm", 3.81}, {"polarity", "male"},
                                 {"rated_current_A", 10.0}});
    // 367590021 (Molex MLX): its record states no pitch.
    const json mlx = conn_spec("367590021",
                               {{"family", "pinHeaderSocket"}, {"positions", 2.0},
                                {"polarity", "male"}, {"rated_current_A", 12.0},
                                {"rated_voltage_V", 600.0}, {"temp_min_C", -55.0},
                                {"temp_max_C", 105.0}});
    const json c = conn_one(orig, mlx);
    CHECK(verdict_of(c, "pitch_mm") == "unverified");
    CHECK(has_note(c, "contact pitch not established (original: 3.81 mm, substitute: not stated)"));
    CHECK(c["grade"] != "drop_in");
    // Had the record stated 6.35 mm: over 0.1 mm on two contacts, a different land pattern.
    json stated = mlx;
    stated["pitch_mm"] = 6.35;
    const json s = conn_one(orig, stated);
    CHECK(verdict_of(s, "pitch_mm") == "fail");
    CHECK(s["footprint"] == "different_land_pattern");
    CHECK(s["grade"] != "drop_in");
    CHECK(s["grade"] != "minor_review");
}

// Seed 22355689: 54202-G0804-B02LF (2 rows) against 87220-7 (rows not on record).
TEST_CASE("qarlos v20: a connector row count stated on one side is compared",
          "[qarlos][crossref]") {
    const json orig = conn_spec("54202-G0804-B02LF",
                                {{"family", "boardToBoard"}, {"positions", 7.0},
                                 {"polarity", "male"}, {"pitch_mm", 2.54}, {"rows", 2.0},
                                 {"rated_current_A", 3.0}, {"rated_voltage_V", 1500.0},
                                 {"temp_min_C", -65.0}, {"temp_max_C", 125.0},
                                 {"mounting", "smt"}, {"orientation", "vertical"}});
    const json cand = conn_spec("87220-7",
                                {{"family", "boardToBoard"}, {"positions", 7.0},
                                 {"polarity", "male"}, {"pitch_mm", 2.54},
                                 {"rated_current_A", 3.0}, {"temp_min_C", -55.0},
                                 {"temp_max_C", 125.0}});
    const json c = conn_one(orig, cand);
    CHECK(verdict_of(c, "rows") == "unverified");
    CHECK(has_note(c, "contact rows not established (original: 2, substitute: not stated)"));
    CHECK(c["grade"] != "drop_in");
}

// ── Qarlos v20 re-test (2026-11-09), capacitor ───────────────────────────────
namespace {
// The capacitor spec block as web/src/crossref.js builds it from the row (base() + spec()).
json cap_spec(const kelvin::CapacitorRow& r) {
    auto nz = [](double v) { return std::isnan(v) || !(v > 0) ? json(nullptr) : json(v); };
    auto raw = [](double v) { return std::isnan(v) ? json(nullptr) : json(v); };
    auto s = [](const std::string& v) { return v.empty() ? json(nullptr) : json(v); };
    return json{{"mpn", r.mpn}, {"length_m", nz(r.length_m)}, {"width_m", nz(r.width_m)},
                {"height_m", nz(r.height_m)}, {"case_code", r.case_code}, {"mount", r.mount},
                {"lead_pitch_m", nz(r.lead_pitch_m)},
                {"value_si", nz(r.capacitance)}, {"voltage", nz(r.v_rated)}, {"esr", nz(r.esr)},
                {"ripple_current", nz(r.ripple_current_rms)}, {"technology", r.technology},
                {"dielectric_code", r.dielectric_code}, {"family", r.family},
                {"esr_frequency", nz(r.esr_frequency)}, {"temp_min_C", raw(r.temp_min_c)},
                {"temp_max_C", raw(r.temp_max_c)},
                {"ripple_current_frequency", nz(r.ripple_current_frequency)},
                {"capacitance_min", nz(r.capacitance_min)}, {"capacitance_max", nz(r.capacitance_max)},
                {"lead_form", s(r.lead_form)}, {"qualified_spec", s(r.qualified_spec)},
                {"qualified_grade", s(r.qualified_grade)},
                {"qualified_failure_rate", raw(r.qualified_failure_rate)}};
}
kelvin::CapacitorRow cap_row(json env) {
    auto r = kelvin::extract_capacitor(env);
    REQUIRE(r.has_value());
    return *r;
}
json c0g_part(const char* series, const char* dielectric = "C0G") {
    return json{{"series", series}, {"technology", "ceramic-class-1"}, {"case", "0805"},
                {"dielectricCode", dielectric}};
}
json cband(double nom, double lo, double hi) {
    return json{{"nominal", nom}, {"minimum", lo}, {"maximum", hi}};
}
json find_cand(const json& res, const char* mpn) {
    for (const auto& c : res["candidates"])
        if (c["mpn"] == mpn) return c;
    FAIL("candidate " << mpn << " missing");
    return json{};
}
}  // namespace

// Finding 1: a looser capacitance tolerance is a downgrade (TAS record values).
// CDR31BP470BFYSAB 47 pF +/-1 % vs GRM21A5C2E470JW01 +/-5 % and ...GW01 +/-2 %;
// MCARQ167SCG0R3ATRA1J 0.3 pF +/-0.05 pF vs GQM1875C2ER30BB12D +/-0.1 pF.
TEST_CASE("qarlos v20: a looser capacitance tolerance FAILs with both bands quoted",
          "[qarlos][crossref][views]") {
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    auto cdr = cap_row(cap_env("Vishay", "CDR31BP470BFYSAB", c0g_part("CDR31", "BP"), json::object(),
        {{"capacitance", cband(4.7e-11, 4.653e-11, 4.747e-11)}, {"ratedVoltage", 100.0}}));
    CHECK(cdr.capacitance_min == 4.653e-11);
    CHECK(cdr.capacitance_max == 4.747e-11);
    auto jw = cap_row(cap_env("Murata", "GRM21A5C2E470JW01", c0g_part("GRM21A5"), json::object(),
        {{"capacitance", cband(4.7e-11, 4.4649999999999996e-11, 4.935e-11)}, {"ratedVoltage", 250.0}}));
    auto gw = cap_row(cap_env("Murata", "GRM21A5C2E470GW01", c0g_part("GRM21A5"), json::object(),
        {{"capacitance", cband(4.7e-11, 4.606e-11, 4.794e-11)}, {"ratedVoltage", 250.0}}));
    auto same = cdr;  // the original's own band
    same.mpn = "SAME";
    auto r = kelvin::crossref::cross_reference(
        "capacitor", cap_spec(cdr), json::array({cap_spec(jw), cap_spec(gw), cap_spec(same)}), opt);
    const json cjw = find_cand(r, "GRM21A5C2E470JW01"), cgw = find_cand(r, "GRM21A5C2E470GW01"),
               csame = find_cand(r, "SAME");
    CHECK(verdict_of(cjw, "capacitance_tolerance") == "fail");
    CHECK(verdict_of(cgw, "capacitance_tolerance") == "fail");
    CHECK(verdict_of(csame, "capacitance_tolerance") == "pass");
    CHECK(cjw["grade"] != "drop_in");
    CHECK(has_note(cjw, "looser capacitance tolerance: the substitute guarantees +/-2.35 pF (+/-5 %) "
                        "against the original's +/-0.47 pF (+/-1 %)"));
    CHECK(cjw["penalty"].get<double>() > csame["penalty"].get<double>());
    // the reverse (a tighter substitute) passes
    auto rev = kelvin::crossref::cross_reference("capacitor", cap_spec(jw),
                                                 json::array({cap_spec(cdr)}), opt);
    CHECK(verdict_of(rev["candidates"][0], "capacitance_tolerance") == "pass");

    // an absolute small-part tolerance, compared in farads
    json rf = {{"technology", "ceramic-class-1"}, {"case", "0603"}, {"dielectricCode", "C0G"}};
    auto mca = cap_row(cap_env("Taiyo Yuden", "MCARQ167SCG0R3ATRA1J", rf, json::object(),
        {{"capacitance", cband(3e-13, 2.5e-13, 3.4999999999999997e-13)}, {"ratedVoltage", 250.0}}));
    auto gqm = cap_row(cap_env("Murata", "GQM1875C2ER30BB12D", rf, json::object(),
        {{"capacitance", cband(3e-13, 2e-13, 4e-13)}, {"ratedVoltage", 250.0}}));
    auto r2 = kelvin::crossref::cross_reference("capacitor", cap_spec(mca),
                                                json::array({cap_spec(gqm)}), opt);
    const json& c2 = r2["candidates"][0];
    CHECK(verdict_of(c2, "capacitance_tolerance") == "fail");
    CHECK(c2["grade"] != "drop_in");
    CHECK(has_note(c2, "+/-0.1 pF"));
    CHECK(has_note(c2, "+/-0.05 pF"));
    // a candidate with no band against a banded original: unverified (secondary, no cap
    // since 2026-11-09)
    auto bare = gqm;
    bare.mpn = "BARE";
    bare.capacitance_min = bare.capacitance_max = kelvin::kNaN();
    auto r3 = kelvin::crossref::cross_reference("capacitor", cap_spec(mca),
                                                json::array({cap_spec(bare)}), opt);
    CHECK(verdict_of(r3["candidates"][0], "capacitance_tolerance") == "unverified");
    CHECK(r3["candidates"][0]["grade"] == "drop_in");  // owner 2026-11-09: secondary
    // a lone end is not a band
    auto lone = cap_row(cap_env("X", "LONE", rf, json::object(),
        {{"capacitance", {{"nominal", 3e-13}, {"maximum", 4e-13}}}, {"ratedVoltage", 50.0}}));
    CHECK(std::isnan(lone.capacitance_min));
    CHECK(std::isnan(lone.capacitance_max));
}

// Finding 2: ESR at different frequencies is not compared (TAS record values).
// 380LX151M450K022 0.44 ohm @ 20 kHz vs MAL219857151E3 0.637 ohm @ 100 Hz.
TEST_CASE("qarlos v20: ESR is compared only on a common frequency basis", "[qarlos][crossref]") {
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    json al = {{"technology", "aluminum-electrolytic-wet"}};
    auto cde = cap_row(cap_env("Cornell Dubilier", "380LX151M450K022", al, json::object(),
        {{"capacitance", cband(1.5e-4, 1.2e-4, 1.8e-4)}, {"ratedVoltage", 450.0}, {"esr", 0.44},
         {"esrFrequency", 20000}, {"rippleCurrent", 1.7}, {"rippleCurrentFrequency", 20000}}));
    auto mal = cap_row(cap_env("Vishay", "MAL219857151E3", al, json::object(),
        {{"capacitance", {{"nominal", 1.5e-4}}}, {"ratedVoltage", 450.0}, {"esr", 0.637},
         {"esrFrequency", 100}, {"rippleCurrent", 1.36}, {"rippleCurrentFrequency", 100}}));
    auto r = kelvin::crossref::cross_reference("capacitor", cap_spec(cde),
                                               json::array({cap_spec(mal)}), opt);
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "esr") == "unverified");
    CHECK(verdict_of(c, "esr_basis") == "unverified");
    CHECK(c["direction"] != "downgrade");
    CHECK(c["grade"] != "drop_in");
    CHECK(has_note(c, "ESR quoted at different frequencies (original 0.44 ohm at 20000 Hz, "
                      "substitute 0.637 ohm at 100 Hz)"));
    // 100 Hz and 120 Hz are one basis: compared directly
    auto at120 = cde;
    at120.esr_frequency = 120;
    at120.esr = 0.5;
    auto r2 = kelvin::crossref::cross_reference("capacitor", cap_spec(at120),
                                                json::array({cap_spec(mal)}), opt);
    CHECK(verdict_of(r2["candidates"][0], "esr") == "warn");
    CHECK(verdict_of(r2["candidates"][0], "esr_basis") == "");
    // within a decade used to be compared as bare numbers; it is not a common basis
    auto at1k = cde;
    at1k.esr_frequency = 1000;
    auto r3 = kelvin::crossref::cross_reference("capacitor", cap_spec(at1k),
                                                json::array({cap_spec(mal)}), opt);
    CHECK(verdict_of(r3["candidates"][0], "esr") == "unverified");
}

// Finding 3: X8L (+15/-40 %) for X8R (+/-15 %) is a regression (TAS record values).
TEST_CASE("qarlos v20: the class-2 compare decodes all three symbols, X8L for X8R fails",
          "[qarlos][crossref][classes]") {
    auto x8l = kelvin::crossref::dielectric_envelope("X8L");
    REQUIRE(x8l.has_value());
    CHECK(x8l->delta_up_pct == 15.0);
    CHECK(x8l->delta_down_pct == 40.0);
    const std::string reg = kelvin::crossref::dielectric_regression("X8R", "X8L");
    CHECK(reg.find("capacitance change +15/-40% vs +/-15%") != std::string::npos);
    CHECK(kelvin::crossref::dielectric_regression("X8L", "X8R").empty());
    // the asymmetric letters compare side by side: Y5U (+22/-56) for Y5V (+22/-82) is no worse
    CHECK(kelvin::crossref::dielectric_regression("Y5V", "Y5U").empty());
    CHECK_FALSE(kelvin::crossref::dielectric_regression("Y5U", "Y5V").empty());
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    auto kem = cap_row(cap_env("KEMET", "C0603C392G4HAC7867",
        {{"series", "SMD Comm X8R HT150C"}, {"case", "0603"}, {"technology", "ceramic-class-2"},
         {"dielectricCode", "X8R"}},
        json::object(), {{"capacitance", cband(3.9e-9, 3.822e-9, 3.978e-9)}, {"ratedVoltage", 16.0}}));
    auto gcj = cap_row(cap_env("Murata", "GCJ188L81H392KA01",
        {{"series", "GCJ188L"}, {"case", "1608M/0603"}, {"technology", "ceramic-class-2"},
         {"dielectricCode", "X8L"}},
        json::object(), {{"capacitance", cband(3.9e-9, 3.51e-9, 4.290000000000001e-9)}, {"ratedVoltage", 50.0}}));
    auto r = kelvin::crossref::cross_reference("capacitor", cap_spec(kem),
                                               json::array({cap_spec(gcj)}), opt);
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "dielectric_code") == "fail");
    CHECK(verdict_of(c, "dielectric_envelope") == "fail");
    CHECK(c["grade"] != "drop_in");
    CHECK(has_note(c, "+15/-40%"));
}

// Finding 4: a MIL-PRF-55681 grade S original against parts stating no qualification.
TEST_CASE("qarlos v20: a qualified-reliability original caps a substitute stating none",
          "[qarlos][crossref][views]") {
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    json cdr_part = c0g_part("CDR31", "BP");
    cdr_part["qualifiedReliability"] = {{"specification", "MIL-PRF-55681"}, {"grade", "S"}};
    auto cdr = cap_row(cap_env("Vishay", "CDR31BP470BFYSAB", cdr_part, json::object(),
        {{"capacitance", cband(4.7e-11, 4.653e-11, 4.747e-11)}, {"ratedVoltage", 100.0}}));
    CHECK(cdr.qualified_spec == "MIL-PRF-55681");
    CHECK(cdr.qualified_grade == "S");
    CHECK(std::isnan(cdr.qualified_failure_rate));
    // a commercial part with the original's own band, so only the qualification differs
    auto grm = cap_row(cap_env("Murata", "GRM21A5C2E470GW01", c0g_part("GRM21A5"), json::object(),
        {{"capacitance", cband(4.7e-11, 4.653e-11, 4.747e-11)}, {"ratedVoltage", 250.0}}));
    auto r = kelvin::crossref::cross_reference("capacitor", cap_spec(cdr),
                                               json::array({cap_spec(grm)}), opt);
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "qualified_reliability") == "unverified");
    CHECK(c["grade"] == "minor_review");
    CHECK(has_note(c, "the original is qualified to MIL-PRF-55681 grade S; the substitute's record "
                      "states no qualified reliability level"));
    // same specification: the stated failure rates decide (M39003/01 grade R vs grade M)
    auto mil = [](const char* mpn, const char* g, double fr) {
        json p = {{"series", "M39003/01"}, {"technology", "tantalum-mno2"}, {"case", "B"},
                  {"qualifiedReliability", {{"specification", "MIL-PRF-39003"}, {"grade", g},
                                            {"failureRate", fr}, {"distribution", "exponential"}}}};
        return cap_row(cap_env("KEMET", mpn, p, json::object(),
            {{"capacitance", cband(1e-6, 9.5e-7, 1.05e-6)}, {"ratedVoltage", 35.0}}));
    };
    auto r2 = kelvin::crossref::cross_reference(
        "capacitor", cap_spec(mil("M39003/01-R", "R", 2.7778e-11)),
        json::array({cap_spec(mil("M39003/01-M", "M", 2.7778e-09)),
                     cap_spec(mil("M39003/01-R2", "R", 2.7778e-11))}),
        opt);
    CHECK(verdict_of(find_cand(r2, "M39003/01-M"), "qualified_reliability") == "fail");
    CHECK(has_note(find_cand(r2, "M39003/01-M"), "higher qualified failure rate"));
    CHECK(verdict_of(find_cand(r2, "M39003/01-R2"), "qualified_reliability") == "pass");
    // a better rate on the substitute passes
    auto r3 = kelvin::crossref::cross_reference(
        "capacitor", cap_spec(mil("M39003/01-M", "M", 2.7778e-09)),
        json::array({cap_spec(mil("M39003/01-R", "R", 2.7778e-11))}), opt);
    CHECK(verdict_of(r3["candidates"][0], "qualified_reliability") == "pass");
}

// Finding 5: axial vs radial is a different land pattern; a leaded body is never sized from a
// chip case table (TAS record values).
TEST_CASE("qarlos v20: axial vs radial is land identity; no chip table on a leaded body",
          "[qarlos][crossref][views][dimensions]") {
    kelvin::crossref::Options opt;
    auto c322 = cap_row(cap_env("KEMET", "C322C270JAG5TA",
        {{"series", "GoldMax 300 Comm C0G"}, {"case", "C322"}, {"technology", "ceramic-class-1"},
         {"dielectricCode", "C0G"}},
        {{"shape", {{"assembly", "THT"}, {"shapeType", "Radial"}}},
         {"dimensions", {{"length", {{"maximum", 0.00508}}}, {"thickness", {{"maximum", 0.00318}}},
                         {"height", {{"maximum", 0.0066}}},
                         {"pitch", {{"nominal", 0.00508}, {"minimum", 0.0043}, {"maximum", 0.00586}}}}}},
        {{"capacitance", cband(2.7e-11, 2.565e-11, 2.835e-11)}, {"ratedVoltage", 250.0}}));
    CHECK(c322.lead_form == "radial");
    auto a330 = cap_row(cap_env("KEMET", "A330J15C0GL5TAA",
        {{"technology", "ceramic-class-1"}, {"dielectricCode", "C0G"}},
        {{"shape", {{"assembly", "THT"}, {"shapeType", "Axial Leaded"}}}},
        {{"capacitance", cband(3.3e-11, 3.135e-11, 3.4650000000000004e-11)}, {"ratedVoltage", 500.0}}));
    CHECK(a330.lead_form == "axial");
    auto r = kelvin::crossref::cross_reference("capacitor", cap_spec(c322),
                                               json::array({cap_spec(a330)}), opt);
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "lead_form") == "fail");
    CHECK(c["grade"] != "drop_in");
    CHECK(has_note(c, "leaded form differs: the original is radial-leaded, the substitute "
                      "axial-leaded"));
    // one side states no form and the other is not SMD: unverified and capped
    auto noform = a330;
    noform.mpn = "NOFORM";
    noform.lead_form = "";
    auto ru = kelvin::crossref::cross_reference("capacitor", cap_spec(c322),
                                                json::array({cap_spec(noform)}), opt);
    CHECK(verdict_of(ru["candidates"][0], "lead_form") == "unverified");
    CHECK(ru["candidates"][0]["grade"] != "drop_in");

    // M39003/01-2751/PR: axial, case "B", no drawing — not an EIA 3528 tantalum chip.
    auto m39 = cap_row(cap_env("Vishay", "M39003/01-2751/PR",
        {{"technology", "tantalum-mno2"}, {"series", "M39003/01"}, {"case", "B"},
         {"qualifiedReliability", {{"specification", "MIL-PRF-39003"}, {"grade", "R"},
                                   {"failureRate", 2.7778e-11}, {"distribution", "exponential"}}}},
        {{"shape", {{"assembly", "THT"}, {"shapeType", "Axial"}}}},
        {{"capacitance", cband(2.2e-5, 1.98e-5, 2.42e-5)}, {"ratedVoltage", 15.0}}));
    auto t378 = cap_row(cap_env("KEMET", "T378E226K015AS", {{"technology", "tantalum-mno2"}},
        {{"shape", {{"assembly", "THT"}, {"shapeType", "Radial Molded"}}},
         {"dimensions", {{"height", {{"nominal", 0.00787}}}, {"thickness", {{"nominal", 0.0033}}}}}},
        {{"capacitance", {{"nominal", 2.2e-5}}}, {"ratedVoltage", 15.0}}));
    CHECK_FALSE(kelvin::crossref::dims_of(cap_spec(m39), "capacitor").has_value());
    CHECK_FALSE(kelvin::crossref::resolve_dimensions("B", "capacitor", true).has_value());
    CHECK(kelvin::crossref::resolve_dimensions("B", "capacitor").has_value());  // an SMD chip
    auto r2 = kelvin::crossref::cross_reference("capacitor", cap_spec(m39),
                                                json::array({cap_spec(t378)}), opt);
    const json& c2 = r2["candidates"][0];
    CHECK(verdict_of(c2, "lead_form") == "fail");
    CHECK(c2.value("footprint", "") != "one_size_larger");
}

// ── Qarlos v20 re-test (2026-11-09), magnetic ────────────────────────────────
// Every record below is the REAL TAS magnetics.ndjson line (distributorsInfo dropped), so the
// tests run the extractor on what the catalogue states, then the ranker on the spec block
// web/src/crossref.js builds from the browse row (base() + the magnetic spec).
namespace {
json real_mag(const char* text) { return json::parse(text); }
json mag_spec_xref(const kelvin::MagneticRow& r) {
    json row;
    kelvin::browse::extra_row_json(r, row);
    auto nz = [](double v) { return std::isnan(v) || !(v > 0) ? json(nullptr) : json(v); };
    auto raw = [](double v) { return std::isnan(v) ? json(nullptr) : json(v); };
    return json{{"mpn", r.mpn},
                {"length_m", nz(r.length_m)}, {"width_m", nz(r.width_m)},
                {"height_m", nz(r.height_m)}, {"case_code", r.case_code}, {"mount", r.mount},
                {"lead_pitch_m", nz(r.lead_pitch_m)}, {"is_production", r.is_production},
                {"provenance_retracted", r.provenance_retracted},
                {"value_si", nz(r.inductance)},
                {"saturation_current", nz(r.saturation_current)},
                {"saturation_points", row["saturation_points"]},
                {"rated_current", nz(r.rated_current)}, {"dcr", nz(r.dcr)}, {"srf", nz(r.srf)},
                {"temp_min_C", raw(r.temp_min_c)}, {"temp_max_C", raw(r.temp_max_c)}};
}
const char* kRec1812PS = R"KREC({"magnetic":{"manufacturerInfo":{"name":"Coilcraft","reference":"1812PS-153KRC","status":"production","datasheetUrl":"https://www.coilcraft.com/en-us/files/datasheet/1812PS","datasheetInfo":{"part":{"description":"RF Inductors - SMD Power inductor"},"electrical":[{"subtype":"inductor","inductance":{"nominal":1.5e-05,"minimum":1.2e-05,"maximum":1.8e-05},"dcResistance":{"nominal":0.26},"inductancePoints":[{"current":0.0,"inductance":1.4999999999999999e-05,"temperature":25.0},{"current":0.005,"inductance":1.4984853584651633e-05,"temperature":25.0},{"current":0.009,"inductance":1.4984853584651633e-05,"temperature":25.0},{"current":0.018,"inductance":1.4989902389767756e-05,"temperature":25.0},{"current":0.026,"inductance":1.4984853584651633e-05,"temperature":25.0},{"current":0.036,"inductance":1.4984853584651633e-05,"temperature":25.0},{"current":0.046,"inductance":1.4984853584651633e-05,"temperature":25.0},{"current":0.054,"inductance":1.497980477953551e-05,"temperature":25.0},{"current":0.064,"inductance":1.497980477953551e-05,"temperature":25.0},{"current":0.074,"inductance":1.497980477953551e-05,"temperature":25.0},{"current":0.082,"inductance":1.4974755974419387e-05,"temperature":25.0},{"current":0.092,"inductance":1.4969707169303264e-05,"temperature":25.0},{"current":0.1,"inductance":1.4959609559071018e-05,"temperature":25.0},{"current":0.125,"inductance":1.4964658364187143e-05,"temperature":25.0},{"current":0.15,"inductance":1.4929316728374285e-05,"temperature":25.0},{"current":0.17,"inductance":1.4909121507909795e-05,"temperature":25.0},{"current":0.195,"inductance":1.4878828677213059e-05,"temperature":25.0},{"current":0.22,"inductance":1.4828340626051834e-05,"temperature":25.0},{"current":0.24,"inductance":1.4782901380006732e-05,"temperature":25.0},{"current":0.28,"inductance":1.476270615954224e-05,"temperature":25.0},{"current":0.32,"inductance":1.4767754964658363e-05,"temperature":25.0},{"current":0.37,"inductance":1.46415348367553e-05,"temperature":25.0},{"current":0.42,"inductance":1.450016829350387e-05,"temperature":25.0},{"current":0.46,"inductance":1.4363850555368563e-05,"temperature":25.0},{"current":0.52,"inductance":1.4111410299562435e-05,"temperature":25.0},{"current":0.62,"inductance":1.3530797711208349e-05,"temperature":25.0},{"current":0.7,"inductance":1.2909794681925279e-05,"temperature":25.0},{"current":0.8,"inductance":1.1874789633120162e-05,"temperature":25.0},{"current":0.88,"inductance":1.0804442948502186e-05,"temperature":25.0},{"current":0.98,"inductance":9.27364523729384e-06,"temperature":25.0}]}],"provenance":[{"source":"scrape","sourceName":"Coilcraft parametric search","sourceUrl":"https://www.coilcraft.com/en-us/files/datasheet/1812PS","retrievedDate":"2026-06-22"},{"source":"manufacturerParametric","sourceName":"Coilcraft parametric search","retrievedDate":"2026-07-21","fields":["mechanical.height","mechanical.length","mechanical.width"]},{"source":"manufacturerParametric","sourceName":"Coilcraft Power Inductor Finder (L vs DC bias)","sourceUrl":"https://www.coilcraft.com/en-us/tools/power-inductor-finder/","retrievedDate":"2026-11-03","fields":["electrical.inductancePoints"],"verification":"valuesReadFromSource","verificationDate":"2026-11-03","verificationMethod":"Typical inductance normalised to nominal vs DC bias current, 25 degC ambient; series data for the tool part 1812PS-153"}],"mechanical":{"length":{"nominal":0.00587},"width":{"nominal":0.00498},"height":{"nominal":0.00381}}}}}})KREC";
const char* kRecSRN3012 = R"KREC({"magnetic":{"manufacturerInfo":{"name":"Bourns","reference":"SRN3012BTA-150M","status":"production","datasheetUrl":"https://www.bourns.com/docs/Product-Datasheets/SRN3012BTA.pdf","datasheetInfo":{"part":{"caseCode":"1212 (3030 Metric)","shielded":true},"electrical":[{"subtype":"inductor","inductance":{"nominal":1.4999999999999999e-05,"minimum":1.2e-05,"maximum":1.7999999999999997e-05},"dcResistance":{"maximum":0.4},"ratedCurrents":[1.2]}],"mechanical":{"mounting":"smt"},"thermal":{"operatingTemperature":{"minimum":-55.0,"maximum":125.0}},"provenance":[{"source":"manufacturerParametric","sourceName":"Bourns parametric search","sourceUrl":"https://www.bourns.com/docs/Product-Datasheets/SRN3012BTA.pdf","retrievedDate":"2026-08-01","verification":"partNamed","verificationDate":"2026-10-09"}]}}}})KREC";
const char* kRec1008PS = R"KREC({"magnetic":{"manufacturerInfo":{"name":"Coilcraft","reference":"1008PS-103KRC","status":"production","datasheetInfo":{"part":{"description":"RF Inductors - SMD Power inductor"},"electrical":[{"subtype":"inductor","inductance":{"nominal":1e-05,"minimum":8.000000000000001e-06,"maximum":1.2e-05},"inductancePoints":[{"current":0.0,"inductance":9.999999999999999e-06,"temperature":25.0},{"current":0.1,"inductance":1.003068282399931e-05,"temperature":25.0},{"current":0.2,"inductance":1.0017526843279625e-05,"temperature":25.0},{"current":0.3,"inductance":9.940483487222473e-06,"temperature":25.0},{"current":0.4,"inductance":9.814981940112673e-06,"temperature":25.0},{"current":0.5,"inductance":9.634328825552582e-06,"temperature":25.0},{"current":0.6,"inductance":9.3835397379159e-06,"temperature":25.0},{"current":0.7,"inductance":9.0334940026381e-06,"temperature":25.0},{"current":0.8,"inductance":8.570575081053759e-06,"temperature":25.0},{"current":0.9,"inductance":8.005638136564797e-06,"temperature":25.0},{"current":1.0,"inductance":7.346214820202418e-06,"temperature":25.0},{"current":1.1,"inductance":6.751506305551103e-06,"temperature":25.0},{"current":1.2,"inductance":5.9089318162206145e-06,"temperature":25.0},{"current":1.3,"inductance":5.046679569521321e-06,"temperature":25.0},{"current":1.4,"inductance":4.05448193394889e-06,"temperature":25.0},{"current":1.5,"inductance":2.8102082126258946e-06,"temperature":25.0}]}],"provenance":[{"source":"scrape","sourceName":"Coilcraft parametric search","retrievedDate":"2026-06-22"},{"source":"manufacturerParametric","sourceName":"Coilcraft parametric search","retrievedDate":"2026-07-21","fields":["mechanical.height","mechanical.length","mechanical.width"]},{"source":"manufacturerParametric","sourceName":"Coilcraft product page (series parts table)","sourceUrl":"https://www.coilcraft.com/en-us/products/power/shielded-inductors/high-srf-power-chip/ps-fs/1008ps/","retrievedDate":"2026-10-18","fields":["electrical.inductance"],"verification":"partNamed","verificationDate":"2026-10-18"},{"source":"manufacturerParametric","sourceName":"Coilcraft Power Inductor Finder (L vs DC bias)","sourceUrl":"https://www.coilcraft.com/en-us/tools/power-inductor-finder/","retrievedDate":"2026-11-03","fields":["electrical.inductancePoints"],"verification":"valuesReadFromSource","verificationDate":"2026-11-03","verificationMethod":"Typical inductance normalised to nominal vs DC bias current, 25 degC ambient; series data for the tool part 1008PS-103"}],"mechanical":{"length":{"nominal":0.00381},"width":{"nominal":0.00378},"height":{"nominal":0.00274}}}}}})KREC";
const char* kRecPA4342 = R"KREC({"magnetic":{"manufacturerInfo":{"name":"Pulse Electronics","reference":"PA4342.103NLT","status":"production","datasheetUrl":"https://yageogroup.com/content/datasheet/asset/file/DATASHEET_P769","family":"PIMC","datasheetInfo":{"part":{"partNumber":"PA4342.103NLT"},"electrical":[{"subtype":"inductor","inductance":{"nominal":1e-05},"dcResistance":{"nominal":0.03},"ratedCurrents":[7.5]}],"mechanical":{"length":{"nominal":0.0115},"height":{"nominal":0.004},"width":{"nominal":0.0103}},"thermal":{"operatingTemperature":{"minimum":-55.0,"maximum":125.0}},"provenance":[{"source":"scrape","sourceName":"YAGEO Group product catalogue","sourceUrl":"https://yageogroup.com/content/datasheet/asset/file/DATASHEET_P769","retrievedDate":"2026-06-22","verification":"partNamed","verificationDate":"2026-10-09"},{"source":"scrape","sourceName":"YAGEO Group product catalogue","retrievedDate":"2026-07-21","fields":["mechanical.height","mechanical.width"]}]}}}})KREC";
const char* kRec0805CS = R"KREC({"magnetic":{"manufacturerInfo":{"name":"Coilcraft","reference":"0805CS-820XGLB","family":"0805CS","datasheetUrl":"https://www.coilcraft.com/en-us/products/rf/ceramic-core-chip-inductors/0805-(2012)/0805cs/0805cs-820/","datasheetInfo":{"part":{"partNumber":"0805CS-820XGLB","family":"0805CS"},"electrical":[{"subtype":"inductor","inductance":{"nominal":8.2e-08,"minimum":8.036e-08,"maximum":8.364e-08},"dcResistance":{"maximum":0.42},"selfResonantFrequency":1330000000.0,"ratedCurrentPoints":[{"current":0.4,"temperatureRise":15.0}]}],"provenance":[{"source":"manufacturerParametric","sourceName":"Coilcraft product page","sourceUrl":"https://www.coilcraft.com/en-us/products/rf/ceramic-core-chip-inductors/0805-(2012)/0805cs/0805cs-820/","retrievedDate":"2026-10-18","fields":["part.partNumber","electrical.0.inductance","electrical.0.dcResistance","electrical.0.selfResonantFrequency","electrical.0.ratedCurrentPoints"],"verification":"seriesConfirmed","verificationDate":"2026-10-18"}]}}}})KREC";
const char* kRecM83446 = R"KREC({"magnetic":{"manufacturerInfo":{"name":"Vanguard Electronics","reference":"M83446/10-95FJ","status":"production","series":"33000","family":"33000","datasheetInfo":{"part":{"description":"33000 Fixed Chip Inductor (QPL)","family":"33000","caseCode":"No Tabs"},"electrical":[{"subtype":"inductor","inductance":{"nominal":5.6e-06,"minimum":5.32e-06,"maximum":5.88e-06},"dcResistance":{"maximum":1.8},"ratedCurrents":[0.16],"selfResonantFrequency":40000000.0}],"mechanical":{"assemblyType":"SMT"},"thermal":{"operatingTemperature":{"minimum":-55.0,"maximum":125.0}},"provenance":[{"source":"scrape","sourceName":"Vanguard Electronics product catalogue","retrievedDate":"2026-06-22"}]}}}})KREC";
const char* kRecB78108 = R"KREC({"magnetic":{"manufacturerInfo":{"name":"TDK","reference":"B78108S1562K000","status":"production","datasheetUrl":"https://www.tdk-electronics.tdk.com/inf/30/db/ind_2008/b78108_148s.pdf","datasheetInfo":{"part":{"caseCode":"Axial"},"electrical":[{"subtype":"inductor","inductance":{"nominal":5.6e-06,"minimum":4.48e-06,"maximum":6.719999999999999e-06},"ratedCurrents":[0.78]}],"mechanical":{"mounting":"tht"},"provenance":[{"source":"manufacturerDatabase","sourceName":"TDK product database","sourceUrl":"https://www.tdk-electronics.tdk.com/inf/30/db/ind_2008/b78108_148s.pdf","retrievedDate":"2026-08-01","verification":"partNamed","verificationDate":"2026-10-09"},{"source":"manufacturerDatabase","sourceName":"TDK product center (part search listing)","sourceUrl":"https://product.tdk.com/en/search/inductor/inductor/lead/info?part_no=B78108S1562K000","retrievedDate":"2026-10-17","fields":["electrical.ratedCurrents"],"verification":"valuesReadFromSource","verificationDate":"2026-10-17"}]}}}})KREC";
const char* kRecCWW0402 = R"KREC({"magnetic":{"manufacturerInfo":{"name":"Vanguard Electronics","reference":"CWW0402-47RGS","status":"production","series":"CWW0402","family":"CWW0402","datasheetInfo":{"part":{"description":"CWW0402 Fixed Chip Inductor (COTS+)","family":"CWW0402","caseCode":"0402"},"electrical":[{"subtype":"inductor","inductance":{"nominal":4.700000000000001e-08,"minimum":4.6060000000000003e-08,"maximum":4.794000000000001e-08},"dcResistance":{"maximum":0.83},"ratedCurrents":[0.112],"selfResonantFrequency":2100000000.0}],"mechanical":{"length":{"nominal":0.0011988799999999998},"assemblyType":"SMT"},"thermal":{"operatingTemperature":{"minimum":-55.0,"maximum":125.0}},"provenance":[{"source":"scrape","sourceName":"Vanguard Electronics product catalogue","retrievedDate":"2026-06-22"},{"source":"manufacturerParametric","sourceName":"Vanguard Electronics product page","sourceUrl":"https://www.ve1.com/products/rf-microwave/fixed-chip-inductor/cww0402/cww0402-47rgs/","retrievedDate":"2026-10-09","verification":"partNamed","verificationDate":"2026-10-09"}]}}}})KREC";
const char* kRecLQW04 = R"KREC({"magnetic":{"manufacturerInfo":{"name":"Murata","reference":"LQW04AN47NH20D","status":"production","datasheetUrl":"https://pim.murata.com/en-global/pim/details/?partNum=LQW04AN47NH20D","datasheetInfo":{"part":{"caseCode":"0302 (0804 Metric)","shielded":true},"electrical":[{"subtype":"inductor","inductance":{"nominal":4.7000000000000004e-08,"minimum":3.7600000000000006e-08,"maximum":5.64e-08},"ratedCurrents":[0.14]}],"mechanical":{"mounting":"smt"},"thermal":{"operatingTemperature":{"minimum":-55.0,"maximum":125.0}},"provenance":[{"source":"manufacturerParametric","sourceName":"Murata product catalogue","sourceUrl":"https://pimapi.murata.com/public/api/pim/v1/products/search/cross-categories?partNum=LQW04AN47NH20D&languageRegion=en-global","retrievedDate":"2026-07-31"},{"source":"manufacturerParametric","sourceName":"Murata product catalogue (PIM API, values also printed on the productdetail page)","sourceUrl":"https://www.murata.com/en-us/products/productdetail?partno=LQW04AN47NH20D","retrievedDate":"2026-10-17","fields":["thermal.operatingTemperature","electrical.inductance","electrical.ratedCurrents","part.partNumber"],"verification":"valuesReadFromSource","verificationDate":"2026-10-17"}]}}}})KREC";
}  // namespace

// Findings v20 #1 / #2 (seeds 21832044, 21727315): a Coilcraft record whose ONLY saturation data
// is its L-vs-DC-bias curve (MAS inductancePoints) reached the ranker with no Isat at all, so
// 1812PS-153KRC (38 % down at 0.98 A) was 'upgrade' for the 1.2 A SRN3012BTA-150M and
// 1008PS-103KRC (30 % down at 1.06 A) 'equivalent' for the 7.5 A PA4342.103NLT. The extractor
// now reads Isat at 10/20/30 % off the record's own curve, interpolating only between measured
// points.
TEST_CASE("qarlos v20: Isat is read off the record's own L-vs-bias curve; the real pairs fail",
          "[qarlos][views][crossref][isat]") {
    auto ps1812 = kelvin::extract_magnetic(real_mag(kRec1812PS));
    auto srn = kelvin::extract_magnetic(real_mag(kRecSRN3012));
    auto ps1008 = kelvin::extract_magnetic(real_mag(kRec1008PS));
    auto pa = kelvin::extract_magnetic(real_mag(kRecPA4342));
    REQUIRE(ps1812.has_value());
    REQUIRE(srn.has_value());
    REQUIRE(ps1008.has_value());
    REQUIRE(pa.has_value());

    // 1812PS-153KRC: L(0) = 15 uH. 10 % (13.5 uH) is crossed between 0.62 A (13.53) and
    // 0.70 A (12.91), 20 % (12 uH) between 0.70 and 0.80 A (11.87), 30 % (10.5 uH) between
    // 0.88 A (10.80) and 0.98 A (9.27), the last measured point.
    REQUIRE(ps1812->sat_table_current.size() == 3);
    CHECK(ps1812->sat_table_percent_drop[0] == 10.0);
    CHECK(ps1812->sat_table_percent_drop[1] == 20.0);
    CHECK(ps1812->sat_table_percent_drop[2] == 30.0);
    CHECK(ps1812->sat_table_current[0] == Approx(0.6239).margin(5e-4));
    CHECK(ps1812->sat_table_current[1] == Approx(0.7881).margin(5e-4));
    CHECK(ps1812->sat_table_current[2] == Approx(0.8995).margin(5e-4));
    CHECK(ps1812->sat_table_temperature[2] == 25.0);
    CHECK(ps1812->saturation_current == Approx(0.7881).margin(5e-4));  // the 20 % headline
    CHECK(ps1812->saturation_percent_drop == 20.0);
    // 1008PS-103KRC: L(0) = 10 uH; 30 % (7 uH) between 1.0 A (7.35) and 1.1 A (6.75).
    REQUIRE(ps1008->sat_table_current.size() == 3);
    CHECK(ps1008->sat_table_current[2] == Approx(1.0582).margin(5e-4));
    // The originals state no curve and no Isat: nothing is derived for them.
    CHECK(srn->sat_table_current.empty());
    CHECK(pa->sat_table_current.empty());

    kelvin::crossref::Options opt;  // footprint check ON, as the web runs it
    auto r1 = kelvin::crossref::cross_reference("magnetic", mag_spec_xref(*srn),
                                                json::array({mag_spec_xref(*ps1812)}), opt);
    const json& c1 = r1["candidates"][0];
    CHECK(verdict_of(c1, "saturation_current") == "fail");
    CHECK(c1["direction"] != "upgrade");
    CHECK(c1["direction"] != "equivalent");
    CHECK(c1["grade"] != "drop_in");
    CHECK(has_note(c1, "saturates below the original's rated current"));

    auto r2 = kelvin::crossref::cross_reference("magnetic", mag_spec_xref(*pa),
                                                json::array({mag_spec_xref(*ps1008)}), opt);
    const json& c2 = r2["candidates"][0];
    CHECK(verdict_of(c2, "saturation_current") == "fail");
    CHECK(c2["direction"] != "upgrade");
    CHECK(c2["direction"] != "equivalent");
    CHECK(has_note(c2, "saturates below the original's rated current"));
}

TEST_CASE("qarlos v20: a bias curve is never extrapolated and needs its zero-bias point",
          "[qarlos][views][isat]") {
    auto curve = [](json pts) {
        return kelvin::extract_magnetic(mag_env("Coilcraft", "X",
            {{"subtype", "inductor"}, {"inductance", {{"nominal", 1e-05}}},
             {"inductancePoints", pts}}));
    };
    // Reaches 10 % and 20 % but stops at 25 % down: no 30 % point is invented.
    auto shallow = curve(json::array({{{"current", 0.0}, {"inductance", 1e-05}},
                                      {{"current", 1.0}, {"inductance", 9e-06}},
                                      {{"current", 2.0}, {"inductance", 7.5e-06}}}));
    REQUIRE(shallow.has_value());
    REQUIRE(shallow->sat_table_current.size() == 2);
    CHECK(shallow->sat_table_current[0] == Approx(1.0));
    CHECK(shallow->sat_table_current[1] == Approx(1.0 + 1.0 / 1.5));
    CHECK(std::isnan(shallow->sat_table_temperature[0]));
    // No point at 0 A: the small-signal reference is not on record, nothing is derived.
    auto noref = curve(json::array({{{"current", 0.1}, {"inductance", 1e-05}},
                                    {{"current", 2.0}, {"inductance", 5e-06}}}));
    REQUIRE(noref.has_value());
    CHECK(noref->sat_table_current.empty());
    CHECK(std::isnan(noref->saturation_current));
    // Several temperatures: the one nearest 25 degC is read, never a blend.
    auto multi = curve(json::array({{{"current", 0.0}, {"inductance", 1e-05}, {"temperature", 20}},
                                    {{"current", 1.0}, {"inductance", 8e-06}, {"temperature", 20}},
                                    {{"current", 0.0}, {"inductance", 1e-05}, {"temperature", 85}},
                                    {{"current", 1.0}, {"inductance", 6e-06}, {"temperature", 85}}}));
    REQUIRE(multi.has_value());
    REQUIRE(multi->sat_table_current.size() == 2);  // 10 % and 20 % on the 20 degC curve
    CHECK(multi->sat_table_temperature[0] == 20.0);
    CHECK(multi->sat_table_current[1] == Approx(1.0));
    // A stated Isat wins: the curve is not read when the record states saturationCurrents.
    auto stated = kelvin::extract_magnetic(mag_env("Coilcraft", "Y",
        {{"subtype", "inductor"}, {"inductance", {{"nominal", 1e-05}}},
         {"saturationCurrents", json::array({{{"percentInductanceDrop", 30.0}, {"current", 3.0}}})},
         {"inductancePoints", json::array({{{"current", 0.0}, {"inductance", 1e-05}},
                                           {{"current", 1.0}, {"inductance", 5e-06}}})}}));
    REQUIRE(stated.has_value());
    REQUIRE(stated->sat_table_current.size() == 1);
    CHECK(stated->sat_table_current[0] == 3.0);
}

// Finding v20 #3 (seed 22041502): 0805CS-820XGLB states its rating only as MAS
// ratedCurrentPoints (0.4 A at a 15 K rise), the schema's preferred form, and the extractor read
// only ratedCurrents: the rating was dropped and every candidate's came back unweighed.
TEST_CASE("qarlos v20: a rated current stated as ratedCurrentPoints is extracted and weighed",
          "[qarlos][views][crossref]") {
    auto cs = kelvin::extract_magnetic(real_mag(kRec0805CS));
    REQUIRE(cs.has_value());
    CHECK(cs->rated_current == 0.4);
    json lower = mag_spec_xref(*cs);
    lower["mpn"] = "LOWER";
    lower["rated_current"] = 0.25;
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    auto r = kelvin::crossref::cross_reference("magnetic", mag_spec_xref(*cs),
                                               json::array({lower}), opt);
    CHECK(verdict_of(r["candidates"][0], "rated_current") == "fail");
}

// Finding v20 #4 (seed 22146231): the TDK B78108S1562K000 record states mechanical.mounting
// 'tht' (MAS), the M83446/10-95FJ original assemblyType 'SMT'. The extractor never read MAS
// `mounting`, and the ranker skipped the mount gate for magnetics outright: minor_review, no note.
TEST_CASE("qarlos v20: a magnetic's stated mount is extracted and an SMT/THT swap rejects",
          "[qarlos][views][crossref]") {
    auto m83 = kelvin::extract_magnetic(real_mag(kRecM83446));
    auto tdk = kelvin::extract_magnetic(real_mag(kRecB78108));
    REQUIRE(m83.has_value());
    REQUIRE(tdk.has_value());
    CHECK(m83->mount == "SMT");
    CHECK(tdk->mount == "tht");
    kelvin::crossref::Options opt;
    auto r = kelvin::crossref::cross_reference("magnetic", mag_spec_xref(*m83),
                                               json::array({mag_spec_xref(*tdk)}), opt);
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "mounting") == "fail");
    CHECK(c["status"] == "no_substitute");
    CHECK(has_note(c, "mount type incompatible: SMT -> tht"));
    // The case strings ("No Tabs", "Axial") are still never classified for a magnetic: with no
    // stated mount on either side the axis stays silent.
    json o = mag_spec_xref(*m83), s = mag_spec_xref(*tdk);
    o["mount"] = "";
    s["mount"] = "";
    auto r2 = kelvin::crossref::cross_reference("magnetic", o, json::array({s}), opt);
    CHECK(verdict_of(r2["candidates"][0], "mounting") == "");
}

// Finding v20 #5 (seed 20784754): LQW04AN47NH20D ("0302 (0804 Metric)") graded 'upgrade' for
// CWW0402-47RGS ("0402"): the case gate skipped magnetics, and the direction came from the rated
// current alone with the original's DCR and SRF unanswered by the substitute's record.
TEST_CASE("qarlos v20: magnetic chip case codes that differ in every reading are a different case",
          "[qarlos][crossref][dimensions]") {
    using kelvin::crossref::magnetic_case_codes_differ;
    CHECK(magnetic_case_codes_differ("0402", "0302 (0804 Metric)"));
    CHECK(magnetic_case_codes_differ("0603", "0805"));
    CHECK(magnetic_case_codes_differ("1212 (3030 Metric)", "1616 (4040 Metric)"));
    // A reading in common, or nothing decodable, establishes nothing.
    CHECK_FALSE(magnetic_case_codes_differ("0402", "1005"));  // metric 1005 is imperial 0402
    CHECK_FALSE(magnetic_case_codes_differ("1210", "1210 (3225 Metric)"));
    CHECK_FALSE(magnetic_case_codes_differ("No Tabs", "0402"));
    CHECK_FALSE(magnetic_case_codes_differ("1008", "0805"));  // 1008 is in no table: not decoded
    CHECK_FALSE(magnetic_case_codes_differ("0402", "0805A"));
    CHECK_FALSE(magnetic_case_codes_differ("0805 (1608 Metric)", "0805"));  // inconsistent pair
}

TEST_CASE("qarlos v20: the real LQW04 / CWW0402 pair is a different case and never an upgrade",
          "[qarlos][views][crossref]") {
    auto cww = kelvin::extract_magnetic(real_mag(kRecCWW0402));
    auto lqw = kelvin::extract_magnetic(real_mag(kRecLQW04));
    REQUIRE(cww.has_value());
    REQUIRE(lqw.has_value());
    kelvin::crossref::Options opt;
    auto r = kelvin::crossref::cross_reference("magnetic", mag_spec_xref(*cww),
                                               json::array({mag_spec_xref(*lqw)}), opt);
    const json& c = r["candidates"][0];
    CHECK(c["footprint"] == "different_case");
    CHECK(verdict_of(c, "footprint") == "warn");
    CHECK(has_note(c, "different package/case (0402 -> 0302 (0804 Metric))"));
    CHECK(c["grade"] != "drop_in");
    CHECK(c["direction"] == "unknown");
    CHECK(has_note(c, "the original states its dcr, srf; the substitute's record does not"));
    // The direction rule on its own (footprint off, so the case gate cannot be what decides):
    // the rated current alone is ahead, the DCR and SRF are unanswered -> not an upgrade.
    kelvin::crossref::Options nofp;
    nofp.check_footprint = false;
    auto r2 = kelvin::crossref::cross_reference("magnetic", mag_spec_xref(*cww),
                                                json::array({mag_spec_xref(*lqw)}), nofp);
    CHECK(r2["candidates"][0]["direction"] == "unknown");
    // ... and once the substitute answers both, the same rated current IS an upgrade.
    json answered = mag_spec_xref(*lqw);
    answered["dcr"] = 0.5;
    answered["srf"] = 2.5e9;
    auto r3 = kelvin::crossref::cross_reference("magnetic", mag_spec_xref(*cww),
                                                json::array({answered}), nofp);
    CHECK(r3["candidates"][0]["direction"] == "upgrade");
}


// The same direction rule on the impedance model: the signal CMC ACM-21-161M-T (now ranked on
// its |Z|, seed 20889483) took ACM2012-201-2P as 'upgrade' with its 0.33 A rating unanswered.
TEST_CASE("qarlos v20: a bead/CMC rating the substitute does not state blocks 'upgrade'",
          "[qarlos][crossref]") {
    json o = {{"mpn", "ACM-21-161M-T"}, {"impedance_100mhz", 160.0}, {"impedance_peak", 160.0},
              {"impedance_peak_freq", 1e8}, {"dcr", 0.35}, {"rated_current", 0.33}};
    json s = {{"mpn", "ACM2012-201-2P"}, {"impedance_100mhz", 200.0}, {"impedance_peak", 200.0},
              {"impedance_peak_freq", 1e8}, {"dcr", 0.3}, {"rated_current", nullptr}};
    kelvin::crossref::Options opt;
    opt.check_footprint = false;
    auto r = kelvin::crossref::cross_reference("chipBead", o, json::array({s}), opt);
    CHECK(r["candidates"][0]["direction"] == "unknown");
    CHECK(has_note(r["candidates"][0], "the original states its rated current"));
    s["rated_current"] = 0.4;
    auto r2 = kelvin::crossref::cross_reference("chipBead", o, json::array({s}), opt);
    CHECK(r2["candidates"][0]["direction"] == "upgrade");
}

// ── Qarlos 2026-11-09 re-test (v20), MOSFET + diode ──────────────────────────
// Every case below runs on the REAL TAS records (verbatim lines of mosfets.ndjson /
// diodes.ndjson in tests/fixtures/qarlos_2026-11-09_*.ndjson), indexed by the real shard
// builder, read back through browse_rows (the row the web receives) and turned into the
// spec exactly as web/src/crossref.js builds it.
namespace {
json nz_(const json& v) {
    return v.is_number() && v.get<double>() > 0 ? v : json(nullptr);
}
json raw_(const json& v) { return v.is_null() ? json(nullptr) : v; }
json base_spec(const json& r) {
    return json{{"mpn", r["mpn"]},
                {"length_m", nz_(r["lengthM"])},
                {"width_m", nz_(r["widthM"])},
                {"height_m", nz_(r["heightM"])},
                {"case_code", r.value("caseCode", "")},
                {"mount", r.value("mount", "")},
                {"lead_pitch_m", r.contains("leadPitchM") ? nz_(r["leadPitchM"]) : json(nullptr)},
                {"is_production", r["is_production"]},
                {"provenance_retracted", r.value("provenanceRetracted", false)}};
}
// XREF mosfet spec (web/src/crossref.js).
json mosfet_spec(const json& r) {
    json s = base_spec(r);
    s["vds"] = nz_(r["vds_rated"]);
    s["id"] = nz_(r["id_continuous"]);
    s["rds_on"] = nz_(r["rds_on"]);
    s["qg"] = nz_(r["qg_total"]);
    s["coss"] = nz_(r["coss"]);
    s["vgs_threshold_max"] = nz_(r["vgs_threshold_max"]);
    s["vgs_threshold_typ"] = nz_(r["vgs_threshold_typ"]);
    s["rds_on_vgs"] = nz_(r["rds_on_vgs"]);
    s["vgs_max"] = nz_(r["vgs_max"]);
    s["technology"] = r.value("technology", "");
    s["qualification"] = r.value("qualification", "");
    const std::string ch = r.value("channel", "");
    s["channel"] = ch.empty() ? json(nullptr) : json(ch);
    return s;
}
// XREF diode spec (web/src/crossref.js).
json diode_spec(const json& r) {
    json s = base_spec(r);
    s["vrrm"] = nz_(r["vrrm_rated"]);
    s["if_avg"] = nz_(r["if_avg_rated"]);
    s["vf"] = nz_(r["vf_typ"]);
    s["power_dissipation"] = nz_(r["power_dissipation"]);
    s["qrr"] = raw_(r["qrr"]);
    s["trr"] = raw_(r["trr"]);
    s["technology"] = r.value("technology", "");
    const std::string dc = r.value("die_configuration", ""), ic = r.value("internal_connection", "");
    s["die_configuration"] = dc.empty() ? json(nullptr) : json(dc);
    s["internal_connection"] = ic.empty() ? json(nullptr) : json(ic);
    s["vz_min_V"] = nz_(r["vz_min"]);
    s["vz_max_V"] = nz_(r["vz_max"]);
    s["vz_tolerance_pct"] =
        r["vz_tolerance"].is_number() ? json(r["vz_tolerance"].get<double>() * 100) : json(nullptr);
    for (const char* k : {"clamping_voltage", "clamping_current", "peak_pulse_power",
                          "peak_pulse_current", "junction_capacitance"})
        s[k] = nz_(r[k]);
    for (const char* k : {"clamping_waveform", "peak_pulse_power_waveform",
                          "peak_pulse_current_waveform", "qualification"})
        s[k] = r.value(k, "");
    s["tj_max"] = raw_(r["tj_max"]);
    s["junction_capacitance_vr"] = raw_(r["junction_capacitance_vr"]);
    return s;
}
std::string fixture_(const char* f) {
    return std::string(KELVIN_TEST_DIR) + "/fixtures/qarlos_2026-11-09_" + f;
}
// mpn -> browse row, over the real records.
const json& md_row(const std::string& family, const std::string& mpn) {
    static const json mos = [] {
        auto sh = kelvin::build_mosfet_shard(fixture_("mosfets.ndjson"));
        return kelvin::browse::browse_rows(sh, json{{"limit", 1000}})["rows"];
    }();
    static const json dio = [] {
        auto sh = kelvin::build_diode_shard(fixture_("diodes.ndjson"));
        return kelvin::browse::browse_rows(sh, json{{"limit", 1000}})["rows"];
    }();
    for (const auto& r : family == "mosfet" ? mos : dio)
        if (r.value("mpn", "") == mpn) return r;
    throw std::runtime_error("fixture has no " + family + " record " + mpn);
}
json md_spec(const std::string& family, const std::string& mpn) {
    const json& r = md_row(family, mpn);
    return family == "mosfet" ? mosfet_spec(r) : diode_spec(r);
}
// Rank `cands` for `orig` exactly as the site does (footprint check on).
json md_rank(const std::string& family, const std::string& orig,
             const std::vector<std::string>& cands) {
    json c = json::array();
    for (const auto& m : cands) c.push_back(md_spec(family, m));
    return kelvin::crossref::cross_reference(family, md_spec(family, orig), c);
}
}  // namespace

// Item 1 (seeds 20300704, 21203670): one JEDEC outline under two names is one land.
TEST_CASE("qarlos 2026-11-09 md1: TO-252 (D-PAK) is DPAK, TO-220 FullPAK is TO-220FP",
          "[qarlos][crossref][dims]") {
    using kelvin::crossref::case_identity;
    CHECK(case_identity("mosfet", "TO-252 (D-PAK)") == case_identity("mosfet", "DPAK"));
    CHECK(case_identity("mosfet", "TO-220 FullPAK") == case_identity("mosfet", "TO-220FP"));
    // ... and different outlines stay different.
    CHECK(case_identity("mosfet", "TO-220 FullPAK") != case_identity("mosfet", "TO-220"));
    CHECK(case_identity("mosfet", "Q-DPAK") != case_identity("mosfet", "DPAK"));
    for (auto [o, c] : {std::pair{"CSD20065D", "IPD600N25N3 G"},
                        std::pair{"IPA90R500S7", "STF8N90K5"}}) {
        INFO(o << " -> " << c);
        auto r = md_rank("mosfet", o, {c});
        const json& cand = r["candidates"][0];
        CHECK(cand["footprint"] != "different_case");
        CHECK(verdict_of(cand, "footprint") != "warn");
        CHECK_FALSE(has_note(cand, "different package/case"));
    }
}

// Item 2 (seed 20994212): Q-DPAK is not DPAK; its footprint note may not quote DPAK's box.
TEST_CASE("qarlos 2026-11-09 md2: a Q-DPAK resolves no DPAK outline",
          "[qarlos][crossref][dims]") {
    using kelvin::crossref::resolve_dimensions;
    CHECK_FALSE(resolve_dimensions("Q-DPAK", "mosfet").has_value());
    CHECK(resolve_dimensions("DPAK", "mosfet").has_value());
    CHECK(md_row("mosfet", "IPDQ65R040CFD7A")["lengthM"].is_null());
    auto r = md_rank("mosfet", "SiHG055N65E", {"IPDQ65R040CFD7A"});
    const json& c = r["candidates"][0];
    CHECK_FALSE(has_note(c, "6.6"));
    CHECK_FALSE(has_note(c, "6.1 mm"));
}

// Item 3 (seed 22041502): same stated TO-220 on both, the candidate's only "box" a package-table
// outline -> the land is kept, footprint fits.
TEST_CASE("qarlos 2026-11-09 md3: same case code with a table outline keeps the land",
          "[qarlos][crossref][dims]") {
    auto r = md_rank("mosfet", "NTP18G120", {"IPP200N15N3"});
    const json& c = r["candidates"][0];
    CHECK(c["footprint"] == "fits");
    CHECK(verdict_of(c, "footprint") == "pass");
    CHECK_FALSE(has_note(c, "smaller body"));
}

// Item 4 (seed 20470567): a bare "Automotive" original engages the automotive gate.
TEST_CASE("qarlos 2026-11-09 md4: an 'Automotive' original holds the substitute to it",
          "[qarlos][crossref]") {
    // IPW65R048CFDA "Automotive"; SiHG61N65EF states nothing, IPDQ65R040CFD7A the same
    // "Automotive" label. The non-automotive case is SiHG61N65EF's real record with the
    // label Infineon's IPD600N25N3 G states ("Industrial"): that part itself is a 250 V
    // one and is rejected on Vds before the grade gate is reached.
    json industrial = md_spec("mosfet", "SiHG61N65EF");
    industrial["mpn"] = "SiHG61N65EF-INDUSTRIAL";
    industrial["qualification"] = md_row("mosfet", "IPD600N25N3 G")["qualification"];
    REQUIRE(industrial["qualification"] == "Industrial");
    auto r = kelvin::crossref::cross_reference(
        "mosfet", md_spec("mosfet", "IPW65R048CFDA"),
        json::array({md_spec("mosfet", "SiHG61N65EF"), industrial,
                     md_spec("mosfet", "IPDQ65R040CFD7A")}));
    const json* none = cand_by_mpn(r, "SiHG61N65EF");
    const json* ind = cand_by_mpn(r, "SiHG61N65EF-INDUSTRIAL");
    const json* same = cand_by_mpn(r, "IPDQ65R040CFD7A");
    REQUIRE(none);
    REQUIRE(ind);
    REQUIRE(same);
    CHECK(verdict_of(*none, "automotive_grade") == "unverified");
    CHECK((*none)["status"] != "recommended");
    CHECK(has_note(*none, "states no qualification"));
    CHECK(verdict_of(*ind, "automotive_grade") == "fail");
    CHECK((*same)["status"] != "no_substitute");  // scored through the grade gate
    CHECK(verdict_of(*same, "automotive_grade") == "");
    CHECK_FALSE(has_note(*same, "automotive"));
}

// Item 5 (seed 21098941): logic-level original (Vth max 2 V), substitute states only a 3.5 V
// typical -> its maximum is above 2 V: WARN with the note, not a silent 'unverified'.
TEST_CASE("qarlos 2026-11-09 md5: a typical Vth above the original's maximum warns",
          "[qarlos][crossref][views]") {
    CHECK(md_row("mosfet", "BSC060N10NS6")["vgs_threshold_max"].is_null());
    CHECK(md_row("mosfet", "BSC060N10NS6")["vgs_threshold_typ"] == 3.5);
    auto r = md_rank("mosfet", "NVMFS5C670NLET1G", {"BSC060N10NS6"});
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "vgs_threshold_max") == "warn");
    CHECK(has_note(c, "TYPICAL Vgs(th) is 3.5 V"));
    CHECK(c["status"] != "recommended");
}

// Item 6 (seed 21936773): a P-channel record whose only bound is a negative MAXIMUM (onsemi's
// "Max" column, FDMA86265P -4 V) keeps its |Vth| worst case.
TEST_CASE("qarlos 2026-11-09 md6: a lone negative P-channel maximum is the |Vth| worst case",
          "[qarlos][crossref][views]") {
    CHECK(md_row("mosfet", "FDMA86265P")["vgs_threshold_max"] == 4.0);
    CHECK(md_row("mosfet", "ISP14EP15LM")["vgs_threshold_max"] == 2.0);
    auto r = md_rank("mosfet", "ISP14EP15LM", {"FDMA86265P"});
    CHECK(verdict_of(r["candidates"][0], "vgs_threshold_max") == "warn");
}

// Item 7 (seeds 20575296, 20889483): a lower diode Tj max is a FAIL with both temperatures;
// one the substitute does not state is unverified and noted.
TEST_CASE("qarlos 2026-11-09 md7: a diode's lower Tj max fails, an unstated one is unverified",
          "[qarlos][crossref]") {
    for (auto [o, c, ot, st] : {std::tuple{"VSSAF3M6-M3/H", "CD214A-B360R", "175", "125"},
                                std::tuple{"SS16FSH", "CD-HD006", "150", "125"}}) {
        INFO(o << " -> " << c);
        auto r = md_rank("diode", o, {c});
        const json& cand = r["candidates"][0];
        CHECK(verdict_of(cand, "tj_max") == "fail");
        CHECK(cand["direction"] != "upgrade");
        CHECK(cand["direction"] != "equivalent");
        CHECK(cand["grade"] == "major_review");
        CHECK(has_note(cand, std::string("rated to +") + st + " degC against the original's +" +
                                 ot + " degC"));
    }
    // BAS70JW's record states no Tj max.
    auto r = md_rank("diode", "VSSAF3M6-M3/H", {"BAS70JW"});
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "tj_max") == "unverified");
    CHECK(has_note(c, "maximum junction temperature is +175 degC; the substitute's record states none"));
}

// Item 8 (seed 20784754): Vc 27.7 V at 54.2 A against 28.4 V at 527.8 A is two points of
// two V-I curves, not a comparison.
TEST_CASE("qarlos 2026-11-09 md8: clamping voltages at different test currents are unverified",
          "[qarlos][crossref]") {
    auto r = md_rank("diode", "TPC20AHM3_A/H", {"15KPA018-SD-Q"});
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "clamping_voltage") == "unverified");
    CHECK(has_note(c, "27.7 V at 54.2 A on the original, 28.4 V at 527.8 A on the substitute"));
    CHECK_FALSE(has_note(c, "higher voltage during a surge"));
}

// Item 9 (seed 21517857): "SOD323" and "SOD-323" are one package-table key.
TEST_CASE("qarlos 2026-11-09 md9: SOD323 resolves the SOD-323 outline",
          "[qarlos][crossref][dims]") {
    using kelvin::crossref::resolve_dimensions;
    auto a = resolve_dimensions("SOD323", "diode"), b = resolve_dimensions("SOD-323", "diode");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(*a->length == *b->length);
    CHECK(*a->width == *b->width);
    // A digit-digit hyphen is part of the name: SOT-23-5 is not SOT-235.
    CHECK(kelvin::crossref::dims_detail::pkg_key("SOT-23-5") == "SOT23-5");
    auto r = md_rank("diode", "GDZ20B-G", {"BZX384-A20"});
    CHECK(r["candidates"][0]["footprint"] == "fits");
}

// Item 10 (seed 21203670): a 2 pF small-signal Schottky original holds every diode substitute
// to its junction capacitance; one the substitute does not state caps at major_review (the
// owner kept this cap on 2026-11-12 when other secondary axes became note-only).
TEST_CASE("qarlos 2026-11-09 md10: an unstated junction capacitance caps at major_review",
          "[qarlos][crossref]") {
    auto r = md_rank("diode", "BAS70JW", {"SB3100", "SBL10100A"});
    REQUIRE(r["candidates"].size() == 2);
    for (const auto& c : r["candidates"]) {
        INFO(c["mpn"]);
        CHECK(verdict_of(c, "junction_capacitance") == "unverified");
        CHECK(c["grade"] == "major_review");
        CHECK(has_note(c, "no junction capacitance — the original states 2 pF"));
    }
    // Both stated: CD214A-B360R (135 pF) against BAS70JW (2 pF) is far beyond 2x.
    auto r2 = md_rank("diode", "BAS70JW", {"CD214A-B360R"});
    CHECK(verdict_of(r2["candidates"][0], "junction_capacitance") == "fail");
}

// Item 11 (seed 21308399): the diode row carries part.qualification, and an AEC-Q101 original
// holds the substitute to it.
TEST_CASE("qarlos 2026-11-09 md11: a diode's AEC-Q101 qualification engages the gate",
          "[qarlos][crossref][views]") {
    CHECK(md_row("diode", "RB168VYM150FHTR")["qualification"] == "Automotive (AEC-Q101)");
    CHECK(md_row("diode", "SS1H15LS")["qualification"] == "");
    auto r = md_rank("diode", "RB168VYM150FHTR", {"SS1H15LS"});
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "automotive_grade") == "unverified");
    CHECK(c["status"] != "recommended");
}

// Side fixes: a placeholder case string is not a case code; Infineon's D-DPAK is not D2PAK.
TEST_CASE("qarlos 2026-11-09 md-side: 'Unknown' is no case, D-DPAK is no D2PAK",
          "[qarlos][views][dims]") {
    CHECK_FALSE(md_row("mosfet", "BSC160N15NS5ATMA1").contains("caseCode"));
    using kelvin::crossref::case_identity;
    using kelvin::crossref::resolve_dimensions;
    CHECK_FALSE(resolve_dimensions("D-DPAK", "mosfet").has_value());
    CHECK_FALSE(resolve_dimensions("DDPAK", "mosfet").has_value());
    CHECK(case_identity("mosfet", "D-DPAK") != case_identity("mosfet", "D2PAK"));
}

// Retest seed 20276947: for RNC55K1472FSB14 (Vishay RNC55, 14.7 kOhm 1 %), ERJB2AF153V (15 kOhm,
// +2.04 %, inside the +/-5 % pool window) ranked FIRST, ahead of the exact-value ERJS parts: its
// penalty (2.66) was below theirs (3.0). Owner's rule: a different nominal resistance stays on
// offer as an alternative, but WARN on value, capped at major_review, never upgrade/equivalent,
// and ranked after every exact-value candidate. Records verbatim from TAS resistors.ndjson.
namespace {
const char* kRNC55K1472FSB14 = R"J({"resistor": {"manufacturerInfo": {"name": "Vishay", "reference": "RNC55K1472FSB14", "datasheetUrl": "https://www.vishay.com/docs/31025/erc.pdf", "datasheetInfo": {"part": {"partNumber": "RNC55K1472FSB14", "technology": "metalFilm", "series": "RNC55"}, "electrical": {"resistance": {"nominal": 14700.0}, "tolerance": 0.01, "powerRating": 0.125, "temperatureCoefficient": 100, "powerRatingTemperature": 70, "maxVoltage": 200}, "thermal": {"operatingTemperature": {"minimum": -65, "maximum": 175}}, "provenance": [{"source": "manufacturerDatasheet", "sourceName": "Vishay Dale ERC datasheet", "sourceUrl": "https://www.vishay.com/docs/31025/erc.pdf", "retrievedDate": "2026-10-09", "fields": ["electrical.resistance", "electrical.tolerance", "electrical.temperatureCoefficient", "electrical.powerRating", "electrical.maxVoltage", "thermal.operatingTemperature"], "verification": "seriesConfirmed", "verificationDate": "2026-10-09"}, {"source": "manufacturerDatabase", "sourceName": "Vishay product part list", "sourceUrl": "https://www.vishay.com/en/product/31025/tab/designtools-ppg/", "retrievedDate": "2026-10-18", "fields": ["part.partNumber"], "verification": "partNamed", "verificationDate": "2026-10-18"}, {"source": "manufacturerDatasheet", "sourceName": "Vishay ERC (Military RNC/RNR) datasheet, document 31025, revision 04-Sep-2025", "sourceUrl": "https://www.vishay.com/docs/31025/erc.pdf", "retrievedDate": "2026-10-23", "fields": ["factors.powerDerating"], "verification": "seriesConfirmed", "verificationDate": "2026-10-23"}, {"source": "manufacturerDatasheet", "sourceName": "Vishay ERC datasheet", "sourceUrl": "https://www.vishay.com/docs/31025/erc.pdf", "retrievedDate": "2026-10-23", "verification": "seriesConfirmed", "verificationDate": "2026-10-23", "fields": ["mechanical.length", "mechanical.diameter"]}], "factors": {"powerDerating": {"temperature": [-65, 70, 125, 175], "amplitude": [1, 1, 0.8, 0]}}, "mechanical": {"length": {"nominal": 0.00635, "minimum": 0.00518, "maximum": 0.00714}, "diameter": {"nominal": 0.00239, "minimum": 0.00209, "maximum": 0.00269}}}}}})J";
const char* kERJB2AF153V = R"J({"resistor":{"manufacturerInfo":{"name":"Panasonic","reference":"ERJB2AF153V","status":"production","datasheetUrl":"https://industrial.panasonic.com/cdbs/www-data/pdf/RDN0000/AOA0000C325.pdf","family":"High Power Current Sensing Resistors, Wide Terminal Type","datasheetInfo":{"part":{"partNumber":"ERJB2AF153V","technology":"currentSenseShunt","series":"High Power Current Sensing Resistors, Wide Terminal Type","case":"0612"},"electrical":{"resistance":{"nominal":15000.0},"tolerance":0.01,"powerRating":0.75,"temperatureCoefficient":100.0},"mechanical":{"length":{"nominal":0.0016},"width":{"nominal":0.0032},"landPattern":{"pattern":"singleRow","originDatum":"padCentroid","pads":[{"id":"1","x":-0.00075,"y":0.0,"shape":"rectangle","width":0.001,"height":0.0034},{"id":"2","x":0.00075,"y":0.0,"shape":"rectangle","width":0.001,"height":0.0034}]}},"provenance":[{"source":"scrape","sourceName":"Panasonic Industrial parametric search","sourceUrl":"https://industrial.panasonic.com/cdbs/www-data/pdf/RDN0000/AOA0000C325.pdf","retrievedDate":"2026-08-01","verification":"seriesConfirmed","verificationDate":"2026-10-10"},{"source":"derived","sourceName":"IPC-7351B","retrievedDate":"2026-08-02","fields":["mechanical.landPattern"],"derivation":"IPC-7351B nominal land pattern for the case code"},{"source":"manufacturerParametric","sourceName":"Panasonic Industry model listing","sourceUrl":"https://industrial.panasonic.com/ww/products/pt/current-sensing-chip-resistors/models/ERJB2AF153V","retrievedDate":"2026-10-09","fields":["electrical.resistance","electrical.tolerance","electrical.powerRating","electrical.temperatureCoefficient","part.case"],"verification":"valuesReadFromSource","verificationDate":"2026-10-18"}]}}}})J";
const char* kERJS06F1472V = R"J({"resistor":{"manufacturerInfo":{"name":"Panasonic","reference":"ERJS06F1472V","status":"production","datasheetUrl":"https://industrial.panasonic.com/cdbs/www-data/pdf/RDP0000/AOA0000C334.pdf","family":"Anti-Sulfurated Thick Film Chip Resistors","datasheetInfo":{"part":{"partNumber":"ERJS06F1472V","technology":"thickFilm","series":"Anti-Sulfurated Thick Film Chip Resistors","case":"0805"},"electrical":{"resistance":{"nominal":14700.0},"tolerance":0.01,"powerRating":0.125,"temperatureCoefficient":100.0},"mechanical":{"length":{"nominal":0.002},"width":{"nominal":0.00125},"landPattern":{"pattern":"singleRow","originDatum":"padCentroid","pads":[{"id":"1","x":-0.00095,"y":0.0,"shape":"rectangle","width":0.001,"height":0.00145},{"id":"2","x":0.00095,"y":0.0,"shape":"rectangle","width":0.001,"height":0.00145}]}},"provenance":[{"source":"scrape","sourceName":"Panasonic Industrial parametric search","sourceUrl":"https://industrial.panasonic.com/cdbs/www-data/pdf/RDP0000/AOA0000C334.pdf","retrievedDate":"2026-08-01","verification":"seriesConfirmed","verificationDate":"2026-10-10"},{"source":"derived","sourceName":"IPC-7351B","retrievedDate":"2026-08-02","fields":["mechanical.landPattern"],"derivation":"IPC-7351B nominal land pattern for the case code"},{"source":"manufacturerParametric","sourceName":"Panasonic Industry model listing","sourceUrl":"https://industrial.panasonic.com/ww/products/pt/anti-sulfurated-chip-resistors/models/ERJS06F1472V","retrievedDate":"2026-10-09","fields":["electrical.resistance","electrical.tolerance","electrical.powerRating","electrical.temperatureCoefficient","part.case"],"verification":"valuesReadFromSource","verificationDate":"2026-10-18"}]}}}})J";
}  // namespace

TEST_CASE("qarlos 2026-11-09 retest: a different resistor value ranks after every exact-value part",
          "[qarlos][crossref][rank]") {
    kelvin::crossref::Options opt;
    opt.max_results = 10;
    // The near value is listed first so the order is the ranker's, not the input's.
    auto r = kelvin::crossref::cross_reference(
        "resistor", res_record_spec(kRNC55K1472FSB14),
        json::array({res_record_spec(kERJB2AF153V), res_record_spec(kERJS06F1472V)}), opt);
    REQUIRE(r["candidates"].size() == 2);
    CHECK(r["candidates"][0]["mpn"] == "ERJS06F1472V");
    CHECK(r["candidates"][1]["mpn"] == "ERJB2AF153V");

    const json exact = res_find(r, "ERJS06F1472V");
    const json near = res_find(r, "ERJB2AF153V");
    REQUIRE(!exact.is_null());
    REQUIRE(!near.is_null());
    CHECK(verdict_of(exact, "value") == "pass");
    CHECK_FALSE(has_note(exact, "nominal resistance differs"));

    CHECK(near["status"] != "no_substitute");  // still offered, as an alternative
    CHECK(verdict_of(near, "value") == "warn");
    CHECK(near["grade"] == "major_review");
    CHECK(near["direction"] != "upgrade");
    CHECK(near["direction"] != "equivalent");
    CHECK(has_note(near, "nominal resistance differs: 14.7 kΩ -> 15 kΩ (+2.04 %) — a different "
                         "value, an alternative only"));
}


// ── Owner's primary/secondary rule (2026-11-09) ─────────────────────────────
// A benchmark showed drop-ins falling from 28 % to 12 %, mostly because a substitute that does
// not state a SECONDARY axis was capped at minor_review. Owner's decision: a one-sided UNVERIFIED
// on a secondary axis keeps its verdict and note but no longer caps the grade; primary axes
// (value, voltage/current/power ratings, pinout/identity, footprint/land/mount, safety class,
// qualification) keep their caps. The three records below are REAL TAS records
// (resistors.ndjson, verbatim).
namespace {
// YAGEO 10 kohm 1 % 0.4 W thin-film MELF/axial: operating range -55..155 degC stated.
const char* kMF0204FTE52_10K = R"J({"resistor":{"manufacturerInfo":{"name":"YAGEO","reference":"MF0204FTE52-10K","status":"production","datasheetUrl":"https://yageogroup.com/content/datasheet/asset/file/YAGEO-MF0_DATASHEET","family":"MF0","datasheetInfo":{"part":{"partNumber":"MF0204FTE52-10K","technology":"thinFilm"},"electrical":{"resistance":{"nominal":10000.0},"tolerance":0.01,"powerRating":0.4},"thermal":{"operatingTemperature":{"minimum":-55.0,"maximum":155.0}},"provenance":[{"source":"scrape","sourceName":"YAGEO Group product catalogue","sourceUrl":"https://yageogroup.com/content/datasheet/asset/file/YAGEO-MF0_DATASHEET","retrievedDate":"2026-08-01","verification":"seriesConfirmed","verificationDate":"2026-10-10"},{"source":"manufacturerParametric","sourceName":"YAGEO Group parametric catalogue","sourceUrl":"https://yageogroup.com/search/unique-parts/MF0204FTE52-10K","retrievedDate":"2026-10-18","fields":["electrical.resistance","electrical.tolerance","electrical.powerRating","thermal.operatingTemperature"],"verification":"valuesReadFromSource","verificationDate":"2026-10-18"},{"source":"manufacturerDatasheet","sourceName":"YAGEO MF0 series datasheet, V.4, Jan. 13, 2026","sourceUrl":"https://yageogroup.com/content/datasheet/asset/file/YAGEO-MF0_DATASHEET","retrievedDate":"2026-10-23","fields":["factors.powerDerating"],"verification":"seriesConfirmed","verificationDate":"2026-10-23"}],"factors":{"powerDerating":{"temperature":[-55,70,155],"amplitude":[1,1,0]}}}}}})J";
// YAGEO 10 kohm 1 % 0.4 W thin-film, the same derating curve: no operating range on record.
const char* kMMF204FRE10K = R"J({"resistor":{"manufacturerInfo":{"name":"YAGEO","reference":"MMF204FRE10K","status":"production","datasheetUrl":"https://yageogroup.com/content/datasheet/asset/file/YAGEO-MMF_DATASHEET","family":"MMF","datasheetInfo":{"part":{"partNumber":"MMF204FRE10K","technology":"thinFilm"},"electrical":{"resistance":{"nominal":10000.0},"tolerance":0.01,"powerRating":0.4},"provenance":[{"source":"scrape","sourceName":"YAGEO Group product catalogue","sourceUrl":"https://yageogroup.com/content/datasheet/asset/file/YAGEO-MMF_DATASHEET","retrievedDate":"2026-08-01","verification":"seriesConfirmed","verificationDate":"2026-10-10"},{"source":"manufacturerParametric","sourceName":"YAGEO Group parametric catalogue","sourceUrl":"https://yageogroup.com/search/unique-parts/MMF204FRE10K","retrievedDate":"2026-10-18","fields":["electrical.resistance","electrical.tolerance","electrical.powerRating"],"verification":"valuesReadFromSource","verificationDate":"2026-10-18"},{"source":"manufacturerDatasheet","sourceName":"YAGEO MMF series datasheet, V.5, Nov. 13, 2024","sourceUrl":"https://yageogroup.com/content/datasheet/asset/file/YAGEO-MMF_DATASHEET","retrievedDate":"2026-11-04","fields":["factors.powerDerating"],"verification":"seriesConfirmed","verificationMethod":"Maximum dissipation in percent of rated power versus ambient temperature, series datasheet figure: 100 % from -55 C to the 70 C rated-power temperature, falling linearly to 0 % at 155 C.","verificationDate":"2026-11-04"}],"factors":{"powerDerating":{"temperature":[-55,70,155],"amplitude":[1,1,0]}}}}}})J";
// Panasonic 30 ohm 5 % 0.125 W 0805: no rating temperature or derating on record, against
// CRCW080530R0JNTA's 0.125 W at 70 degC.
const char* kERJS06J300V = R"J({"resistor":{"manufacturerInfo":{"name":"Panasonic","reference":"ERJS06J300V","status":"production","datasheetUrl":"https://industrial.panasonic.com/cdbs/www-data/pdf/RDP0000/AOA0000C334.pdf","family":"Anti-Sulfurated Thick Film Chip Resistors","datasheetInfo":{"part":{"partNumber":"ERJS06J300V","technology":"thickFilm","series":"Anti-Sulfurated Thick Film Chip Resistors","case":"0805"},"electrical":{"resistance":{"nominal":30.0},"tolerance":0.05,"powerRating":0.125,"temperatureCoefficient":200.0},"mechanical":{"length":{"nominal":0.002},"width":{"nominal":0.00125},"landPattern":{"pattern":"singleRow","originDatum":"padCentroid","pads":[{"id":"1","x":-0.00095,"y":0.0,"shape":"rectangle","width":0.001,"height":0.00145},{"id":"2","x":0.00095,"y":0.0,"shape":"rectangle","width":0.001,"height":0.00145}]}},"provenance":[{"source":"scrape","sourceName":"Panasonic Industrial parametric search","sourceUrl":"https://industrial.panasonic.com/cdbs/www-data/pdf/RDP0000/AOA0000C334.pdf","retrievedDate":"2026-08-01","verification":"seriesConfirmed","verificationDate":"2026-10-10"},{"source":"derived","sourceName":"IPC-7351B","retrievedDate":"2026-08-02","fields":["mechanical.landPattern"],"derivation":"IPC-7351B nominal land pattern for the case code"},{"source":"manufacturerParametric","sourceName":"Panasonic Industry model listing","sourceUrl":"https://industrial.panasonic.com/ww/products/pt/anti-sulfurated-chip-resistors/models/ERJS06J300V","retrievedDate":"2026-10-09","fields":["electrical.resistance","electrical.tolerance","electrical.powerRating","electrical.temperatureCoefficient","part.case"],"verification":"valuesReadFromSource","verificationDate":"2026-10-18"}]}}}})J";
}  // namespace

TEST_CASE("owner 2026-11-09: a substitute missing only a secondary axis can be a drop_in",
          "[qarlos][crossref][grade]") {
    auto r = kelvin::crossref::cross_reference("resistor", res_record_spec(kMF0204FTE52_10K),
                                               json::array({res_record_spec(kMMF204FRE10K)}),
                                               res_opt());
    const json c = res_find(r, "MMF204FRE10K");
    REQUIRE(!c.is_null());
    // The secondary axis is still reported as unknown, with its notes ...
    CHECK(verdict_of(c, "temp_min_C") == "unverified");
    CHECK(verdict_of(c, "temp_max_C") == "unverified");
    CHECK(has_note(c, "states no minimum operating temperature"));
    CHECK(has_note(c, "states no maximum operating temperature"));
    // ... the primary ratings are established (power along both derating curves) ...
    CHECK(verdict_of(c, "power_rating") == "pass");
    // ... and the unknown no longer caps the grade or demotes the row.
    CHECK(c["grade"] == "drop_in");
    CHECK(c["status"] == "recommended");
}

TEST_CASE("owner 2026-11-09: a substitute missing a primary axis is still capped",
          "[qarlos][crossref][grade]") {
    // Power at the original's rating temperature (a power rating) not on the substitute's
    // record: capped at minor_review, with its note.
    auto r = kelvin::crossref::cross_reference("resistor", res_record_spec(kCRCW080530R0JNTA),
                                               json::array({res_record_spec(kERJS06J300V)}),
                                               res_opt());
    const json c = res_find(r, "ERJS06J300V");
    REQUIRE(!c.is_null());
    CHECK(verdict_of(c, "power_rating") == "unverified");
    CHECK(has_note(c, "power rating not comparable at the original's rating temperature"));
    CHECK(c["grade"] == "minor_review");
    CHECK(c["status"] == "partial");

    // Connector contact pitch (the land pattern) stated by the original only.
    json orig = {{"mpn", "O"}, {"family", "boardToBoard"}, {"positions", 10.0},
                 {"pitch_mm", 2.54}, {"rated_current_A", 3.0}};
    json cand = {{"mpn", "NO_PITCH"}, {"family", "boardToBoard"}, {"positions", 10.0},
                 {"rated_current_A", 3.0}};
    auto rc = kelvin::crossref::cross_reference("connector", orig, json::array({cand}));
    const json& cc = rc["candidates"][0];
    CHECK(verdict_of(cc, "pitch_mm") == "unverified");
    CHECK(has_note(cc, "contact pitch not established"));
    CHECK(cc["grade"] == "minor_review");
}

// ── Qarlos r8-r10, group B (ranking / notes) ─────────────────────────────────────────────
// Specs below carry the real catalogue values of each reported pair (frozen TAS 2d91b9c).

TEST_CASE("qarlos r9 23088792: two 5 % TVS grades that print the same are not 'looser'",
          "[qarlos][crossref][diode]") {
    // P6KE43CA-B (Littelfuse) and P6KE47CA-E3/54 (Vishay): both 5 % grades, derived from 0.1 V
    // window figures as 4.99419 % and 4.99469 %.
    auto tvs = [](const char* mpn, double lo, double hi, double vrrm, double vc, double ic,
                  double ipp) {
        return json{{"mpn", mpn}, {"technology", "tvs"}, {"vrrm", vrrm},
                    {"vz_min_V", lo}, {"vz_max_V", hi},
                    {"vz_tolerance_pct", 100.0 * (hi - lo) / (hi + lo)},
                    {"clamping_voltage", vc}, {"clamping_current", ic},
                    {"clamping_waveform", "10/1000"}, {"peak_pulse_power", 600.0},
                    {"peak_pulse_power_waveform", "10/1000"}, {"peak_pulse_current", ipp},
                    {"peak_pulse_current_waveform", "10/1000"}, {"tj_max", 175.0},
                    {"case_code", "DO-15"}, {"is_production", true}};
    };
    const json orig = tvs("P6KE43CA-B", 40.9, 45.2, 36.8, 59.3, 10.3, 10.3);
    const json same_grade = tvs("P6KE47CA-E3/54", 44.7, 49.4, 40.2, 64.8, 9.3, 9.3);
    // A genuine 10 % grade of the same class still reads looser.
    const json wider = tvs("P6KE43C", 38.7, 47.3, 34.8, 61.9, 9.7, 9.7);
    auto r = kelvin::crossref::cross_reference("diode", orig, json::array({same_grade, wider}));
    json same, loose;
    for (const auto& c : r["candidates"]) {
        if (c["mpn"] == "P6KE47CA-E3/54") same = c;
        if (c["mpn"] == "P6KE43C") loose = c;
    }
    REQUIRE(!same.is_null());
    REQUIRE(!loose.is_null());
    CHECK(verdict_of(same, "vz_tolerance_pct") == "pass");
    CHECK_FALSE(has_note(same, "looser breakdown grade"));
    CHECK(verdict_of(loose, "vz_tolerance_pct") != "pass");
    CHECK(has_note(loose, "looser breakdown grade"));
}

TEST_CASE("qarlos bench 20470567: an SRF a fraction lower is stated as a percentage",
          "[qarlos][crossref][magnetic]") {
    // IHLP8787MZER101M5A (SRF 2.01 MHz) -> SRP2313AA-101M (2 MHz).
    const json orig = {{"mpn", "IHLP8787MZER101M5A"}, {"value_si", 1e-4}, {"dcr", 0.0394},
                       {"rated_current", 9.1}, {"srf", 2.01e6}};
    json near = {{"mpn", "SRP2313AA-101M"}, {"value_si", 1e-4}, {"dcr", 0.004},
                 {"rated_current", 11.0}, {"srf", 2.0e6}};
    json far = near;
    far["mpn"] = "FAR";
    far["srf"] = 2.01e6 / 4.5;
    // Owner, 2026-11-12: down to 0.95x of the original's SRF passes, so the real 0.5 % pair
    // above no longer warns. The percentage wording is exercised by a candidate inside the
    // warn band (0.9x, owner-approved addition to this test).
    json mid = near;
    mid["mpn"] = "MID";
    mid["srf"] = 1.809e6;
    auto r = kelvin::crossref::cross_reference("magnetic", orig, json::array({near, far, mid}));
    json n, f, m;
    for (const auto& c : r["candidates"]) {
        if (c["mpn"] == "SRP2313AA-101M") n = c;
        if (c["mpn"] == "FAR") f = c;
        if (c["mpn"] == "MID") m = c;
    }
    REQUIRE(!n.is_null());
    REQUIRE(!f.is_null());
    REQUIRE(!m.is_null());
    CHECK(verdict_of(n, "srf") == "pass");
    CHECK_FALSE(has_note(n, "self-resonant frequency"));
    CHECK(verdict_of(m, "srf") == "warn");
    CHECK(has_note(m, "self-resonant frequency 1.809 MHz vs the original's 2.01 MHz (-10 %)"));
    CHECK_FALSE(has_note(m, "1x lower"));
    CHECK(has_note(f, "(4.5x lower)"));
}

TEST_CASE("qarlos r10 23717166: the Rds(on) basis note keeps a 4.5 V drive",
          "[qarlos][crossref][mosfet]") {
    // CSD17304Q3 (8.8 mOhm at 4.5 V) -> BSC059N04LS6 (at 10 V).
    const json orig = {{"mpn", "CSD17304Q3"}, {"vds", 30.0}, {"id", 16.0}, {"rds_on", 0.0088},
                       {"rds_on_vgs", 4.5}, {"channel", "nChannel"}};
    const json cand = {{"mpn", "BSC059N04LS6"}, {"vds", 40.0}, {"id", 50.0}, {"rds_on", 0.0059},
                       {"rds_on_vgs", 10.0}, {"channel", "nChannel"}};
    auto r = kelvin::crossref::cross_reference("mosfet", orig, json::array({cand}));
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "rds_on_basis") == "warn");
    CHECK(has_note(c, "Rds(on) is specified at a higher Vgs (10 V vs 4.5 V)"));
}

TEST_CASE("qarlos r10 23507708: a crystal CL note states 8.5 pF, not a rounded 9 pF",
          "[qarlos][crossref][timing]") {
    // 7V-25.000MAAB-T (CL 8.5 pF) -> 830051068 (CL 9 pF).
    const json orig = {{"mpn", "7V-25.000MAAB-T"}, {"subtype", "oscillator"},
                       {"technology", "quartzCrystal"}, {"device_type", "oscillator"},
                       {"frequency", 25e6}, {"load_capacitance", 8.5e-12},
                       {"load_capacitance_pF", 8.5}, {"mode", "fundamental"}};
    json cand = orig;
    cand["mpn"] = "830051068";
    cand["load_capacitance"] = 9e-12;
    cand["load_capacitance_pF"] = 9.0;
    auto r = kelvin::crossref::cross_reference("timeBase", orig, json::array({cand}));
    const json& c = r["candidates"][0];
    CHECK(c["status"] == "no_substitute");
    CHECK(has_note(c, "specified for a different load capacitance (9 pF vs 8.5 pF)"));
}

TEST_CASE("qarlos r9 23088792: a chip bead's operating range is compared",
          "[qarlos][crossref][magnetic]") {
    // MFBA3V1608K-471-R (-55..150 degC) -> MMZ1608Y471BTA00 (-55..125 degC).
    const json orig = {{"mpn", "MFBA3V1608K-471-R"}, {"impedance_100mhz", 470.0},
                       {"impedance_peak", 470.0}, {"impedance_peak_freq", 1e8}, {"dcr", 0.4},
                       {"rated_current", 0.35}, {"temp_min_C", -55.0}, {"temp_max_C", 150.0}};
    json cooler = {{"mpn", "MMZ1608Y471BTA00"}, {"impedance_100mhz", 470.0},
                   {"impedance_peak", 470.0}, {"impedance_peak_freq", 1e8}, {"dcr", 0.35},
                   {"rated_current", 0.5}, {"temp_min_C", -55.0}, {"temp_max_C", 125.0}};
    json silent = cooler;
    silent["mpn"] = "NO_RANGE";
    silent.erase("temp_min_C");
    silent.erase("temp_max_C");
    auto r = kelvin::crossref::cross_reference("chipBead", orig, json::array({cooler, silent}));
    json c, s;
    for (const auto& x : r["candidates"]) {
        if (x["mpn"] == "MMZ1608Y471BTA00") c = x;
        if (x["mpn"] == "NO_RANGE") s = x;
    }
    REQUIRE(!c.is_null());
    REQUIRE(!s.is_null());
    CHECK(verdict_of(c, "temp_max_C") == "fail");
    CHECK(has_note(c, "rated only to +125 degC vs the original's +150 degC"));
    CHECK(c["direction"] != "upgrade");
    CHECK(c["grade"] == "major_review");
    // Unstated on the substitute: UNVERIFIED and noted, no cap (secondary axis).
    CHECK(verdict_of(s, "temp_max_C") == "unverified");
    CHECK(has_note(s, "states no maximum operating temperature"));
}

// ── Qarlos r8-r10, group A: fields the originals lost on the way to the ranker ──────────────
// REAL TAS records (verbatim lines of the frozen bench resistors.ndjson / diodes.ndjson, TAS
// 2d91b9c) in tests/fixtures/qarlos_r8r10_*.ndjson.
namespace {
std::string r8r10_line(const char* file, const std::string& mpn) {
    std::ifstream in(std::string(KELVIN_TEST_DIR) + "/fixtures/qarlos_r8r10_" + file);
    std::string line;
    while (std::getline(in, line))
        if (line.find("\"reference\": \"" + mpn + "\"") != std::string::npos ||
            line.find("\"reference\":\"" + mpn + "\"") != std::string::npos)
            return line;
    throw std::runtime_error(std::string("fixture has no record ") + mpn);
}
json r8r10_res(const std::string& mpn) {
    return res_record_spec(r8r10_line("resistors.ndjson", mpn).c_str());
}
const json& r8r10_diode_row(const std::string& mpn) {
    static const json rows = [] {
        auto sh = kelvin::build_diode_shard(std::string(KELVIN_TEST_DIR) +
                                            "/fixtures/qarlos_r8r10_diodes.ndjson");
        return kelvin::browse::browse_rows(sh, json{{"limit", 100}})["rows"];
    }();
    for (const auto& r : rows)
        if (r.value("mpn", "") == mpn) return r;
    throw std::runtime_error("fixture has no diode " + mpn);
}
json r8r10_diode_rank(const std::string& orig, const std::vector<std::string>& cands) {
    json c = json::array();
    for (const auto& m : cands) c.push_back(diode_spec(r8r10_diode_row(m)));
    return kelvin::crossref::cross_reference("diode", diode_spec(r8r10_diode_row(orig)), c);
}
}  // namespace

// A2 (r10 seed 23612437) / A3 (r8 seed 22565147): RAS electrical.maxVoltage never reached the
// resistor row, so an 800 V KOA HV73V high-voltage chip took substitutes with no voltage verdict.
TEST_CASE("qarlos r8r10 A2/A3: a resistor's maximum working voltage is carried and compared",
          "[qarlos][crossref][views]") {
    CHECK(r8r10_res("HV73V2BTTD3483F")["max_voltage_V"] == 800.0);
    CHECK(r8r10_res("CMF604M7000JLEA")["max_voltage_V"] == 500.0);
    CHECK(r8r10_res("ERJUP8F3483V")["max_voltage_V"].is_null());

    auto r = kelvin::crossref::cross_reference(
        "resistor", r8r10_res("HV73V2BTTD3483F"),
        json::array({r8r10_res("RK73H2BTTD3483F"), r8r10_res("ERJUP8F3483V")}), res_opt());
    // 200 V against 800 V: a FAIL, counted against the part, with both voltages named.
    const json lower = res_find(r, "RK73H2BTTD3483F");
    REQUIRE(!lower.is_null());
    CHECK(verdict_of(lower, "max_voltage_V") == "fail");
    CHECK(has_note(lower, "lower maximum working voltage: the substitute is rated 200 V against "
                          "the original's 800 V"));
    CHECK(lower["grade"] != "drop_in");
    CHECK(lower["direction"] != "upgrade");
    // Not stated by the substitute: UNVERIFIED and noted.
    const json unknown = res_find(r, "ERJUP8F3483V");
    REQUIRE(!unknown.is_null());
    CHECK(verdict_of(unknown, "max_voltage_V") == "unverified");
    CHECK(has_note(unknown, "the original's maximum working voltage is 800 V; the substitute's "
                            "record states none"));
}

// A4 (r8 seed 22774605): PMEG6020EPAF states 250 pF at 1 V, MBRS2560T3G 170 pF at 48 V; the
// junction capacitance was graded 'pass' on those two numbers. Cj falls with reverse bias, so
// they compare only on a common bias (the ESR/ripple frequency-basis rule).
TEST_CASE("qarlos r8r10 A4: junction capacitances on different reverse biases are not compared",
          "[qarlos][crossref][views]") {
    CHECK(r8r10_diode_row("PMEG6020EPAF")["junction_capacitance_vr"] == 1.0);
    CHECK(r8r10_diode_row("MBRS2560T3G")["junction_capacitance_vr"] == 48.0);
    CHECK(r8r10_diode_row("CD214A-B360R")["junction_capacitance_vr"].is_null());

    auto r = r8r10_diode_rank("PMEG6020EPAF", {"MBRS2560T3G", "PMEG6020ELRX", "CD214A-B360R"});
    auto find = [&](const std::string& m) {
        for (const auto& c : r["candidates"])
            if (c["mpn"] == m) return c;
        return json(nullptr);
    };
    const json other_bias = find("MBRS2560T3G");
    REQUIRE(!other_bias.is_null());
    CHECK(verdict_of(other_bias, "junction_capacitance") == "unverified");
    CHECK(verdict_of(other_bias, "junction_capacitance_basis") == "unverified");
    CHECK(has_note(other_bias, "junction capacitance quoted at different reverse voltages "
                               "(original 250 pF at 1 V, substitute 170 pF at 48 V)"));
    // Same 1 V basis: compared as stated (110 pF <= 250 pF).
    const json same_bias = find("PMEG6020ELRX");
    REQUIRE(!same_bias.is_null());
    CHECK(verdict_of(same_bias, "junction_capacitance") == "pass");
    // A Cj at no stated bias against an original that states one: not comparable either.
    const json no_bias = find("CD214A-B360R");
    REQUIRE(!no_bias.is_null());
    CHECK(verdict_of(no_bias, "junction_capacitance") == "unverified");
    CHECK(has_note(no_bias, "the original's junction capacitance is 250 pF at 1 V; the "
                            "substitute's record states 135 pF at no reverse voltage"));
}


// ── Connector family-variant identities from real records (owner 2026-11-12) ──────────────
// "Identity when stated": familyFpcFfc.actuatorType and familyCircular.shellSize / .coding /
// .couplingType are read from the structured CONAS fields only (never descriptions or MPNs)
// and judged like interface_standard: both stated and different -> reject; one side only ->
// UNVERIFIED with a note naming the field and the stated value (no cap: interface_standard's
// one-sided treatment); neither -> no verdict row, no note. Every record below is a verbatim
// line of tools/qarlos/bench/data-0d5241d/connectors.ndjson (TAS 0d5241d), run through the
// real extractor and the spec web/src/crossref.js builds from the shard row.
namespace variant_ids {
// connectors.ndjson line 403646: 512810894
const char* k512810894 = R"KREC({"connector": {"manufacturerInfo": {"name": "Molex", "reference": "512810894", "status": "obsolete", "description": "0.50mm Pitch Easy-On FFC/FPC Connector, Surface Mount, Right-Angle, Non-ZIF, Dual Contact Style, 1.20mm Mated Height, 8 Circuits, Gold (Au) Plating", "family": "Easy-On", "datasheetInfo": {"part": {"partNumber": "512810894", "description": "0.50mm Pitch Easy-On FFC/FPC Connector, Surface Mount, Right-Angle, Non-ZIF, Dual Contact Style, 1.20mm Mated Height, 8 Circuits, Gold (Au) Plating", "series": "Easy-On"}, "electrical": {"ratedCurrentPerContact": 0.5, "ratedVoltage": 50.0}, "mechanical": {"positions": 8, "pitch": 0.0005, "orientation": "rightAngle", "matingCycles": 20}, "familyDetails": {"family": "fpcFfc", "actuatorType": "nonZif"}, "environmental": {"operatingTemperature": {"minimum": -20.0, "maximum": 85.0}}, "provenance": [{"source": "manufacturerParametric", "sourceName": "Molex product catalogue", "sourceUrl": "https://www.molex.com/en-us/products/part-detail/512810894", "retrievedDate": "2026-10-17", "fields": ["part.description", "part.series", "electrical.ratedCurrentPerContact", "electrical.ratedVoltage", "mechanical.positions", "mechanical.pitch", "mechanical.orientation", "mechanical.matingCycles", "familyDetails.family", "familyDetails.actuatorType", "environmental.operatingTemperature"], "verification": "valuesReadFromSource", "verificationDate": "2026-10-17"}]}}}})KREC";
// connectors.ndjson line 403516: 545500833
const char* k545500833 = R"KREC({"connector": {"manufacturerInfo": {"name": "Molex", "reference": "545500833", "status": "obsolete", "description": "0.50mm Pitch Easy-On FFC/FPC Connector, 1.20mm Height, Right-Angle, Surface Mount, ZIF, Top Contact Style, 8 Circuits", "family": "Easy-On", "datasheetInfo": {"part": {"partNumber": "545500833", "description": "0.50mm Pitch Easy-On FFC/FPC Connector, 1.20mm Height, Right-Angle, Surface Mount, ZIF, Top Contact Style, 8 Circuits", "series": "Easy-On"}, "electrical": {"ratedCurrentPerContact": 0.5, "ratedVoltage": 50.0}, "mechanical": {"positions": 8, "pitch": 0.0005, "orientation": "rightAngle", "matingCycles": 20}, "familyDetails": {"family": "fpcFfc", "actuatorType": "slide"}, "environmental": {"operatingTemperature": {"minimum": -40.0, "maximum": 85.0}}, "provenance": [{"source": "manufacturerParametric", "sourceName": "Molex product catalogue", "sourceUrl": "https://www.molex.com/en-us/products/part-detail/545500833", "retrievedDate": "2026-10-17", "fields": ["part.description", "part.series", "electrical.ratedCurrentPerContact", "electrical.ratedVoltage", "mechanical.positions", "mechanical.pitch", "mechanical.orientation", "mechanical.matingCycles", "familyDetails.family", "familyDetails.actuatorType", "environmental.operatingTemperature"], "verification": "valuesReadFromSource", "verificationDate": "2026-10-17"}]}}}})KREC";
// connectors.ndjson line 412054: 512960833
const char* k512960833 = R"KREC({"connector": {"manufacturerInfo": {"name": "Molex", "reference": "512960833", "status": "obsolete", "description": "0.50mm Pitch Easy-On FFC/FPC Connector, 1.30mm Height, Right-Angle, Surface Mount, ZIF, Bottom Contact Style, 8 Circuits", "family": "Easy-On", "datasheetInfo": {"part": {"partNumber": "512960833", "description": "0.50mm Pitch Easy-On FFC/FPC Connector, 1.30mm Height, Right-Angle, Surface Mount, ZIF, Bottom Contact Style, 8 Circuits", "series": "Easy-On"}, "electrical": {"ratedCurrentPerContact": 0.5, "ratedVoltage": 50.0}, "mechanical": {"positions": 8, "pitch": 0.0005, "orientation": "rightAngle", "matingCycles": 20}, "familyDetails": {"family": "fpcFfc"}, "environmental": {"operatingTemperature": {"minimum": -40.0, "maximum": 85.0}}, "provenance": [{"source": "manufacturerParametric", "sourceName": "Molex product catalogue", "sourceUrl": "https://www.molex.com/en-us/products/part-detail/512960833", "retrievedDate": "2026-10-17", "fields": ["part.description", "part.series", "electrical.ratedCurrentPerContact", "electrical.ratedVoltage", "mechanical.positions", "mechanical.pitch", "mechanical.orientation", "mechanical.matingCycles", "familyDetails.family", "environmental.operatingTemperature"], "verification": "valuesReadFromSource", "verificationDate": "2026-10-17"}]}}}})KREC";
// connectors.ndjson line 96908: CAT-M5833-M2934
const char* kCAT_M5833_M2934 = R"KREC({"connector":{"manufacturerInfo":{"name":"TE Connectivity","reference":"CAT-M5833-M2934","status":"production","datasheetInfo":{"part":{"partNumber":"CAT-M5833-M2934","series":null},"electrical":{"ratedCurrentPerContact":1.0},"mechanical":{"positions":4,"pitch":0.00127},"familyDetails":{"family":"fpcFfc"},"environmental":{"operatingTemperature":{"minimum":-40.0,"maximum":105.0}},"provenance":[{"source":"manufacturerParametric","sourceName":"TE Connectivity product catalogue","verification":"valuesReadFromSource","verificationDate":"2026-09-07","retrievedDate":"2026-06-24"}]}}}})KREC";
// connectors.ndjson line 108113: CAT-M5833-C8684
const char* kCAT_M5833_C8684 = R"KREC({"connector":{"manufacturerInfo":{"name":"TE Connectivity","reference":"CAT-M5833-C8684","status":"production","datasheetInfo":{"part":{"partNumber":"CAT-M5833-C8684","series":null},"electrical":{"ratedCurrentPerContact":1.5},"mechanical":{"positions":4,"pitch":0.00127},"familyDetails":{"family":"fpcFfc"},"environmental":{"operatingTemperature":{"minimum":-40.0,"maximum":105.0}},"provenance":[{"source":"manufacturerParametric","sourceName":"TE Connectivity product catalogue","verification":"valuesReadFromSource","verificationDate":"2026-09-07","retrievedDate":"2026-06-24"}]}}}})KREC";
// connectors.ndjson line 380265: 1924950000
const char* k1924950000 = R"KREC({"connector": {"manufacturerInfo": {"name": "Weidmüller", "reference": "1924950000", "status": "production", "orderCode": "1924950000", "datasheetUrl": "https://eshop.weidmueller.com/en/p/1924950000", "datasheetInfo": {"part": {"partNumber": "1924950000", "description": "Field attachable connector, M12"}, "electrical": {"ratedCurrentPerContact": 1.5, "ratedVoltage": 30.0}, "mechanical": {"positions": 12, "weight": 0.019, "matingCycles": 100}, "environmental": {"ipRating": "IP67", "rohsCompliant": true}, "familyDetails": {"family": "circular", "shellSize": "M12"}, "provenance": [{"source": "manufacturerParametric", "sourceName": "Weidmüller product catalogue", "sourceUrl": "https://eshop.weidmueller.com/en/p/1924950000", "retrievedDate": "2026-07-31", "verification": "partNamed", "verificationDate": "2026-10-10"}, {"source": "manufacturerParametric", "sourceName": "Weidmüller product catalogue", "sourceUrl": "https://eshop.weidmueller.com/en/p/1924950000", "retrievedDate": "2026-09-24", "fields": ["manufacturerInfo.reference"], "verification": "partNamed", "verificationDate": "2026-10-10"}, {"source": "manufacturerParametric", "sourceName": "Weidmüller eShop product catalogue listing", "sourceUrl": "https://eshop.weidmueller.com/en/p/1924950000", "retrievedDate": "2026-10-22", "fields": ["part.partNumber", "part.description"], "verification": "partNamed", "verificationDate": "2026-10-22"}]}, "family": "Circular Connectors"}}})KREC";
// connectors.ndjson line 380338: 1224150000
const char* k1224150000 = R"KREC({"connector": {"manufacturerInfo": {"name": "Weidmüller", "reference": "1224150000", "status": "production", "orderCode": "1224150000", "datasheetUrl": "https://eshop.weidmueller.com/en/p/1224150000", "datasheetInfo": {"part": {"partNumber": "1224150000", "description": "Sensor/Actuator plug-in connector"}, "electrical": {"ratedCurrentPerContact": 8.0, "ratedVoltage": 200.0}, "mechanical": {"positions": 12, "weight": 0.01, "matingCycles": 1000}, "environmental": {"rohsCompliant": true}, "familyDetails": {"family": "circular", "shellSize": "M23"}, "provenance": [{"source": "manufacturerParametric", "sourceName": "Weidmüller product catalogue", "sourceUrl": "https://eshop.weidmueller.com/en/p/1224150000", "retrievedDate": "2026-07-31", "verification": "partNamed", "verificationDate": "2026-10-10"}, {"source": "manufacturerParametric", "sourceName": "Weidmüller product catalogue", "sourceUrl": "https://eshop.weidmueller.com/en/p/1224150000", "retrievedDate": "2026-09-24", "fields": ["manufacturerInfo.reference"], "verification": "partNamed", "verificationDate": "2026-10-10"}]}, "family": "Circular Connectors"}}})KREC";
// connectors.ndjson line 380333: 1224000000
const char* k1224000000 = R"KREC({"connector": {"manufacturerInfo": {"name": "Weidmüller", "reference": "1224000000", "status": "production", "orderCode": "1224000000", "datasheetUrl": "https://eshop.weidmueller.com/en/p/1224000000", "datasheetInfo": {"part": {"partNumber": "1224000000", "description": "Sensor/Actuator plug-in connector"}, "electrical": {"ratedCurrentPerContact": 20.0, "ratedVoltage": 300.0}, "mechanical": {"positions": 6, "weight": 0.00704, "matingCycles": 1000}, "environmental": {"rohsCompliant": true}, "familyDetails": {"family": "circular", "shellSize": "M23"}, "provenance": [{"source": "manufacturerParametric", "sourceName": "Weidmüller product catalogue", "sourceUrl": "https://eshop.weidmueller.com/en/p/1224000000", "retrievedDate": "2026-07-31", "verification": "partNamed", "verificationDate": "2026-10-10"}, {"source": "manufacturerParametric", "sourceName": "Weidmüller product catalogue", "sourceUrl": "https://eshop.weidmueller.com/en/p/1224000000", "retrievedDate": "2026-09-24", "fields": ["manufacturerInfo.reference"], "verification": "partNamed", "verificationDate": "2026-10-10"}]}, "family": "Circular Connectors"}}})KREC";
// connectors.ndjson line 136453: DIV40E1706PA-023
const char* kDIV40E1706PA_023 = R"KREC({"connector":{"manufacturerInfo":{"name":"TE Connectivity","reference":"DIV40E1706PA-023","status":"production","datasheetInfo":{"part":{"partNumber":"DIV40E1706PA-023","description":"Wire-to-Panel, Circular Connector Receptacle, 6 Position, Shell Size D (15), .06 mm [.1 in] Centerline, Sealable, Wire & Cable, Power & Signal","series":null},"electrical":{"ratedCurrentPerContact":23.0},"mechanical":{"positions":6},"familyDetails":{"family":"circular"},"environmental":{"operatingTemperature":{"minimum":-65.0,"maximum":200.0}},"provenance":[{"source":"manufacturerParametric","sourceName":"TE Connectivity product catalogue","verification":"valuesReadFromSource","verificationDate":"2026-09-07","retrievedDate":"2026-06-24"}]},"description":"Wire-to-Panel, Circular Connector Receptacle, 6 Position, Shell Size D (15), .06 mm [.1 in] Centerline, Sealable, Wire & Cable, Power & Signal"}}})KREC";
// connectors.ndjson line 223386: CDM520-04A-01FRA-3M-67
const char* kCDM520_04A = R"KREC({"connector": {"manufacturerInfo": {"name": "CUI Devices", "reference": "CDM520-04A-01FRA-3M-67", "status": "production", "datasheetInfo": {"part": {"partNumber": "CDM520-04A-01FRA-3M-67", "description": "Round"}, "electrical": {"ratedCurrentPerContact": 1.0, "ratedVoltage": 60.0}, "mechanical": {"positions": 4, "mountingStyle": "cable"}, "provenance": [{"source": "manufacturerParametric", "verification": "inferredNotVerified", "verificationDate": "2026-09-06", "sourceName": "Same Sky product catalogue", "sourceUrl": "https://www.sameskydevices.com/catalog/interconnect", "retrievedDate": "2026-07-31"}, {"source": "manufacturerParametric", "sourceName": "Same Sky interconnect catalogue", "sourceUrl": "https://www.sameskydevices.com/product/interconnect/cable-assemblies/circular-cable-assemblies/cdm520-04a-01fra-3m-67", "retrievedDate": "2026-10-18", "fields": ["part.partNumber", "electrical.ratedCurrentPerContact", "electrical.ratedVoltage"], "verification": "partNamed", "verificationDate": "2026-10-18"}], "environmental": {"operatingTemperature": {"minimum": -25.0, "maximum": 80.0}}, "familyDetails": {"family": "circular", "coding": "A", "shielded": false}}, "description": "Round", "datasheetUrl": "https://www.sameskydevices.com/product/resource/cdm520.pdf"}}})KREC";
// connectors.ndjson line 223388: CDM519-04A-01FRA-2M-67
const char* kCDM519_04A = R"KREC({"connector": {"manufacturerInfo": {"name": "CUI Devices", "reference": "CDM519-04A-01FRA-2M-67", "status": "production", "datasheetInfo": {"part": {"partNumber": "CDM519-04A-01FRA-2M-67", "description": "Round", "series": "CDM519"}, "electrical": {"ratedCurrentPerContact": 1.0, "ratedVoltage": 60.0}, "mechanical": {"positions": 4, "mountingStyle": "cable"}, "provenance": [{"source": "manufacturerParametric", "verification": "inferredNotVerified", "verificationDate": "2026-09-06", "sourceName": "Same Sky product catalogue", "sourceUrl": "https://www.sameskydevices.com/catalog/interconnect", "retrievedDate": "2026-07-31"}, {"source": "manufacturerParametric", "sourceName": "Same Sky interconnect catalogue", "sourceUrl": "https://www.sameskydevices.com/product/interconnect/cable-assemblies/circular-cable-assemblies/cdm519-04a-01fra-2m-67", "retrievedDate": "2026-10-18", "fields": ["part.partNumber", "electrical.ratedCurrentPerContact", "electrical.ratedVoltage"], "verification": "partNamed", "verificationDate": "2026-10-18"}], "environmental": {"operatingTemperature": {"minimum": -25.0, "maximum": 80.0}}, "familyDetails": {"family": "circular", "coding": "A", "shielded": false}}, "description": "Round", "datasheetUrl": "https://www.sameskydevices.com/product/resource/cdm519.pdf"}}})KREC";
// connectors.ndjson line 223437: CDM818-05B-01FRA-2M-67
const char* kCDM818_05B = R"KREC({"connector": {"manufacturerInfo": {"name": "CUI Devices", "reference": "CDM818-05B-01FRA-2M-67", "status": "production", "datasheetInfo": {"part": {"partNumber": "CDM818-05B-01FRA-2M-67", "description": "Round", "series": "CDM818"}, "electrical": {"ratedCurrentPerContact": 3.0, "ratedVoltage": 30.0}, "mechanical": {"positions": 5, "mountingStyle": "cable"}, "provenance": [{"source": "manufacturerParametric", "verification": "inferredNotVerified", "verificationDate": "2026-09-06", "sourceName": "Same Sky product catalogue", "sourceUrl": "https://www.sameskydevices.com/catalog/interconnect", "retrievedDate": "2026-07-31"}, {"source": "manufacturerParametric", "sourceName": "Same Sky interconnect catalogue", "sourceUrl": "https://www.sameskydevices.com/product/interconnect/cable-assemblies/circular-cable-assemblies/cdm818-05b-01fra-2m-67", "retrievedDate": "2026-10-18", "fields": ["part.partNumber", "electrical.ratedCurrentPerContact", "electrical.ratedVoltage"], "verification": "partNamed", "verificationDate": "2026-10-18"}], "environmental": {"operatingTemperature": {"minimum": -25.0, "maximum": 80.0}}, "familyDetails": {"family": "circular", "coding": "B", "shielded": false}}, "description": "Round", "datasheetUrl": "https://www.sameskydevices.com/product/resource/cdm818.pdf"}}})KREC";
// connectors.ndjson line 223499: CDM1237-05A-01FRA-3M-67
const char* kCDM1237_05A = R"KREC({"connector": {"manufacturerInfo": {"name": "CUI Devices", "reference": "CDM1237-05A-01FRA-3M-67", "status": "production", "datasheetInfo": {"part": {"partNumber": "CDM1237-05A-01FRA-3M-67", "description": "Round", "series": "CDM1237"}, "electrical": {"ratedCurrentPerContact": 4.0, "ratedVoltage": 60.0}, "mechanical": {"positions": 5, "mountingStyle": "cable"}, "provenance": [{"source": "manufacturerParametric", "verification": "inferredNotVerified", "verificationDate": "2026-09-06", "sourceName": "Same Sky product catalogue", "sourceUrl": "https://www.sameskydevices.com/catalog/interconnect", "retrievedDate": "2026-07-31"}, {"source": "manufacturerParametric", "sourceName": "Same Sky interconnect catalogue", "sourceUrl": "https://www.sameskydevices.com/product/interconnect/cable-assemblies/circular-cable-assemblies/cdm1237-05a-01fra-3m-67", "retrievedDate": "2026-10-18", "fields": ["part.partNumber", "electrical.ratedCurrentPerContact", "electrical.ratedVoltage"], "verification": "partNamed", "verificationDate": "2026-10-18"}], "environmental": {"operatingTemperature": {"minimum": -25.0, "maximum": 80.0}}, "familyDetails": {"family": "circular", "coding": "A", "shielded": false}}, "description": "Round", "datasheetUrl": "https://www.sameskydevices.com/product/resource/cdm1237.pdf"}}})KREC";
// connectors.ndjson line 224473: CDM802-08A-FP-F010-67
const char* kCDM802_08A = R"KREC({"connector": {"manufacturerInfo": {"name": "CUI Devices", "reference": "CDM802-08A-FP-F010-67", "status": "production", "datasheetInfo": {"part": {"partNumber": "CDM802-08A-FP-F010-67", "description": "Jack, Female", "series": "CDM802", "matingPolarity": "female"}, "electrical": {"ratedCurrentPerContact": 3.0, "ratedVoltage": 60.0}, "mechanical": {"positions": 3, "mountingStyle": "panel"}, "provenance": [{"source": "manufacturerParametric", "verification": "inferredNotVerified", "verificationDate": "2026-09-06", "sourceName": "Same Sky product catalogue", "sourceUrl": "https://www.sameskydevices.com/catalog/interconnect", "retrievedDate": "2026-07-31"}, {"source": "manufacturerParametric", "sourceName": "Same Sky interconnect catalogue", "sourceUrl": "https://www.sameskydevices.com/product/interconnect/connectors/circular-connectors/cdm802-08a-fp-f010-67", "retrievedDate": "2026-10-18", "fields": ["part.partNumber", "electrical.ratedCurrentPerContact", "electrical.ratedVoltage", "mechanical.positions"], "verification": "partNamed", "verificationDate": "2026-10-18"}], "environmental": {"operatingTemperature": {"minimum": -25.0, "maximum": 80.0}}, "familyDetails": {"family": "circular", "coding": "A", "shielded": false}}, "description": "Jack, Female", "datasheetUrl": "https://www.sameskydevices.com/product/resource/cdm802.pdf"}}})KREC";
// connectors.ndjson line 106571: ZPF000000000005657
const char* kZPF5657 = R"KREC({"connector": {"manufacturerInfo": {"name": "TE Connectivity", "reference": "ZPF000000000005657", "status": "production", "datasheetInfo": {"part": {"partNumber": "ZPF000000000005657", "matingPolarity": "female", "description": "Circular Connector Plug, Shell Size 12, Sealable, Signal, Panel Mount, Bronze, Code N, 3 Signal Positions, 14 – 12 AWG, 1.91 – 3.18 mm² Wire", "series": "DEUTSCH"}, "electrical": {"ratedCurrentPerContact": 23.0}, "mechanical": {"positions": 3, "mountingStyle": "panel"}, "familyDetails": {"family": "circular"}, "environmental": {"operatingTemperature": {"minimum": -40.0, "maximum": 100.0}}, "provenance": [{"source": "manufacturerParametric", "sourceName": "TE Connectivity product catalogue", "sourceUrl": "https://www.te.com/en/product-ZPF000000000005657.html", "retrievedDate": "2026-09-07", "verification": "valuesReadFromSource", "verificationDate": "2026-09-07", "fields": ["mechanical.positions", "mechanical.mountingStyle"]}, {"source": "manufacturerParametric", "sourceName": "TE Connectivity product catalogue", "verification": "valuesReadFromSource", "verificationDate": "2026-09-07", "retrievedDate": "2026-06-24"}]}, "family": "DEUTSCH", "description": "Circular Connector Plug, Shell Size 12, Sealable, Signal, Panel Mount, Bronze, Code N, 3 Signal Positions, 14 – 12 AWG, 1.91 – 3.18 mm² Wire"}}})KREC";
// connectors.ndjson line 380347: 1224570000
const char* k1224570000 = R"KREC({"connector": {"manufacturerInfo": {"name": "Weidmüller", "reference": "1224570000", "status": "production", "orderCode": "1224570000", "datasheetUrl": "https://eshop.weidmueller.com/en/p/1224570000", "datasheetInfo": {"part": {"partNumber": "1224570000", "description": "Sensor/Actuator plug-in connector"}, "electrical": {"ratedCurrentPerContact": 8.0, "ratedVoltage": 200.0}, "mechanical": {"positions": 12, "weight": 0.01, "matingCycles": 1000}, "environmental": {"rohsCompliant": true}, "familyDetails": {"family": "circular", "shellSize": "M23"}, "provenance": [{"source": "manufacturerParametric", "sourceName": "Weidmüller product catalogue", "sourceUrl": "https://eshop.weidmueller.com/en/p/1224570000", "retrievedDate": "2026-07-31", "verification": "partNamed", "verificationDate": "2026-10-10"}, {"source": "manufacturerParametric", "sourceName": "Weidmüller product catalogue", "sourceUrl": "https://eshop.weidmueller.com/en/p/1224570000", "retrievedDate": "2026-09-24", "fields": ["manufacturerInfo.reference"], "verification": "partNamed", "verificationDate": "2026-10-10"}]}, "family": "Circular Connectors"}}})KREC";
// connectors.ndjson line 395327: D38999/24FA98AN
const char* kD38999_24FA98AN = R"KREC({"connector": {"manufacturerInfo": {"name": "Amphenol Aerospace", "reference": "D38999/24FA98AN", "family": "MIL-DTL-38999 Series III", "description": "MIL-DTL-38999 Series III jam nut receptacle, shell size 9, insert arrangement 9-98, 3 contacts, pin insert, supplied less contacts", "datasheetUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "datasheetInfo": {"part": {"partNumber": "D38999/24FA98AN", "series": "MIL-DTL-38999 Series III", "matingPolarity": "male", "description": "MIL-DTL-38999 Series III jam nut receptacle, shell size 9, insert arrangement 9-98, 3 contacts, pin insert, supplied less contacts"}, "electrical": {"ratedVoltage": 600.0, "dielectricWithstandingVoltage": 1800.0, "ratedCurrentPerContact": 7.5}, "mechanical": {"positions": 3, "mountingStyle": "panel"}, "familyDetails": {"family": "circular", "shellSize": "9", "coding": "N", "couplingType": "threaded"}, "provenance": [{"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["part.matingPolarity", "mechanical.positions", "mechanical.mountingStyle", "familyDetails.shellSize", "familyDetails.coding"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "Insert Availability chart and How to Order (Military) key decoded for this order code"}, {"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["electrical.ratedVoltage", "electrical.dielectricWithstandingVoltage"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "Service rating I: suggested operating voltage 600 VAC rms, test voltage 1800 VRMS at sea level"}, {"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["electrical.ratedCurrentPerContact"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "Contact rating test current for contact sizes 20"}, {"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["familyDetails.couplingType"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "Triple Start Threads"}]}}}})KREC";
// connectors.ndjson line 400553: SJT07RT-8-98P
const char* kSJT07RT_8_98P = R"KREC({"connector": {"manufacturerInfo": {"name": "Amphenol Aerospace", "reference": "SJT07RT-8-98P", "family": "Amphenol SJT", "description": "Amphenol SJT jam nut receptacle, shell size 8, insert arrangement 8-98, 3 contacts, pin insert", "datasheetUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "datasheetInfo": {"part": {"partNumber": "SJT07RT-8-98P", "series": "Amphenol SJT", "matingPolarity": "male", "description": "Amphenol SJT jam nut receptacle, shell size 8, insert arrangement 8-98, 3 contacts, pin insert"}, "electrical": {"ratedVoltage": 600.0, "dielectricWithstandingVoltage": 1800.0, "ratedCurrentPerContact": 7.5}, "mechanical": {"positions": 3, "mountingStyle": "panel"}, "familyDetails": {"family": "circular", "shellSize": "8", "coding": "N", "couplingType": "bayonet"}, "provenance": [{"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["part.matingPolarity", "mechanical.positions", "mechanical.mountingStyle", "familyDetails.shellSize", "familyDetails.coding"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "Insert Availability chart and How to Order (Military) key decoded for this order code"}, {"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["electrical.ratedVoltage", "electrical.dielectricWithstandingVoltage"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "Service rating I: suggested operating voltage 600 VAC rms, test voltage 1800 VRMS at sea level"}, {"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["electrical.ratedCurrentPerContact"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "Contact rating test current for contact sizes 20"}, {"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["familyDetails.couplingType"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "bayonet-lock coupling"}]}}}})KREC";
// connectors.ndjson line 400178: SJT06RT-12-22S
const char* kSJT06RT_12_22S = R"KREC({"connector": {"manufacturerInfo": {"name": "Amphenol Aerospace", "reference": "SJT06RT-12-22S", "family": "Amphenol SJT", "description": "Amphenol SJT straight plug, shell size 12, insert arrangement 12-22, 22 contacts, socket insert", "datasheetUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "datasheetInfo": {"part": {"partNumber": "SJT06RT-12-22S", "series": "Amphenol SJT", "matingPolarity": "female", "description": "Amphenol SJT straight plug, shell size 12, insert arrangement 12-22, 22 contacts, socket insert"}, "electrical": {"ratedVoltage": 400.0, "dielectricWithstandingVoltage": 1300.0, "ratedCurrentPerContact": 3.0}, "mechanical": {"positions": 22, "mountingStyle": "cable"}, "familyDetails": {"family": "circular", "shellSize": "12", "coding": "N", "couplingType": "bayonet"}, "provenance": [{"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["part.matingPolarity", "mechanical.positions", "mechanical.mountingStyle", "familyDetails.shellSize", "familyDetails.coding"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "Insert Availability chart and How to Order (Military) key decoded for this order code"}, {"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["electrical.ratedVoltage", "electrical.dielectricWithstandingVoltage"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "Service rating M: suggested operating voltage 400 VAC rms, test voltage 1300 VRMS at sea level"}, {"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["electrical.ratedCurrentPerContact"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "Contact rating test current for contact sizes 22M"}, {"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["familyDetails.couplingType"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "bayonet-lock coupling"}]}}}})KREC";
// connectors.ndjson line 399671: MS27484T12B22B
const char* kMS27484T12B22B = R"KREC({"connector": {"manufacturerInfo": {"name": "Amphenol Aerospace", "reference": "MS27484T12B22B", "family": "MIL-DTL-38999 Series II", "description": "MIL-DTL-38999 Series II crimp straight plug with grounding fingers, shell size 12, insert arrangement 12-22, 22 contacts, socket insert, supplied less contacts", "datasheetUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "datasheetInfo": {"part": {"partNumber": "MS27484T12B22B", "series": "MIL-DTL-38999 Series II", "matingPolarity": "female", "description": "MIL-DTL-38999 Series II crimp straight plug with grounding fingers, shell size 12, insert arrangement 12-22, 22 contacts, socket insert, supplied less contacts"}, "electrical": {"ratedVoltage": 400.0, "dielectricWithstandingVoltage": 1300.0, "ratedCurrentPerContact": 3.0}, "mechanical": {"positions": 22, "mountingStyle": "cable"}, "familyDetails": {"family": "circular", "shellSize": "12", "coding": "N"}, "provenance": [{"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["part.matingPolarity", "mechanical.positions", "mechanical.mountingStyle", "familyDetails.shellSize", "familyDetails.coding"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "Insert Availability chart and How to Order (Military) key decoded for this order code"}, {"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["electrical.ratedVoltage", "electrical.dielectricWithstandingVoltage"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "Service rating M: suggested operating voltage 400 VAC rms, test voltage 1300 VRMS at sea level"}, {"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["electrical.ratedCurrentPerContact"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "Contact rating test current for contact sizes 22M"}]}}}})KREC";

// The connector spec exactly as web/src/crossref.js builds it from a shard row: every
// unstated optional string is null, never "".
json nul_or(const std::string& s) { return s.empty() ? json(nullptr) : json(s); }
json nz(double v) { return std::isfinite(v) && v > 0 ? json(v) : json(nullptr); }
json fin(double v) { return std::isfinite(v) ? json(v) : json(nullptr); }
json spec_of(const char* line) {
    auto row = kelvin::extract_connector(json::parse(line));
    REQUIRE(row.has_value());
    const kelvin::ConnectorRow& r = *row;
    return json{{"mpn", r.mpn},
                {"length_m", nz(r.length_m)},
                {"width_m", nz(r.width_m)},
                {"height_m", nz(r.height_m)},
                {"case_code", r.case_code},
                {"mount", r.mount},
                {"lead_pitch_m", nz(r.lead_pitch_m)},
                {"is_production", r.is_production},
                {"provenance_retracted", r.provenance_retracted},
                {"box_axes_ambiguous", r.box_axes_ambiguous},
                {"family", r.family},
                {"positions", nz(r.positions)},
                {"polarity", nul_or(r.polarity)},
                {"interface_standard", nul_or(r.interface_standard)},
                {"pitch_mm", std::isfinite(r.pitch) && r.pitch > 0 ? json(r.pitch * 1e3)
                                                                   : json(nullptr)},
                {"rows", nz(r.rows)},
                {"rated_current_A", nz(r.rated_current)},
                {"rated_voltage_V", nz(r.rated_voltage)},
                {"temp_min_C", fin(r.temp_min_c)},
                {"temp_max_C", fin(r.temp_max_c)},
                {"contact_plating", nul_or(r.contact_plating)},
                {"termination", nul_or(r.termination)},
                {"mounting", nul_or(r.mount)},
                {"orientation", nul_or(r.orientation)},
                {"pluggable", r.pluggable == "true"    ? json(true)
                              : r.pluggable == "false" ? json(false)
                                                       : json(nullptr)},
                {"actuator_type", nul_or(r.actuator_type)},
                {"shell_size", nul_or(r.shell_size)},
                {"coding", nul_or(r.coding)},
                {"coupling_type", nul_or(r.coupling_type)},
                {"mating_cycles", nz(r.mating_cycles)}};
}
json rank_one(const char* orig, const char* cand) {
    kelvin::crossref::Options opt;
    opt.max_results = 10;
    auto r = kelvin::crossref::cross_reference("connector", spec_of(orig),
                                               json::array({spec_of(cand)}), opt);
    REQUIRE(r["candidates"].size() == 1);
    return r["candidates"][0];
}
size_t notes_with(const json& c, const std::string& needle) {
    size_t n = 0;
    if (c.contains("notes"))
        for (const auto& s : c["notes"])
            if (s.get<std::string>().find(needle) != std::string::npos) ++n;
    return n;
}
}  // namespace variant_ids

TEST_CASE("connector identity 2026-11-12: the extractor reads the variant fields as stated",
          "[qarlos][views][connector-identity]") {
    using namespace variant_ids;
    auto a = kelvin::extract_connector(json::parse(k545500833));
    auto b = kelvin::extract_connector(json::parse(k512960833));
    auto c = kelvin::extract_connector(json::parse(kD38999_24FA98AN));
    auto d = kelvin::extract_connector(json::parse(k1224150000));
    REQUIRE((a && b && c && d));
    CHECK(a->actuator_type == "slide");
    CHECK(b->actuator_type.empty());
    CHECK(c->shell_size == "9");
    CHECK(c->coding == "N");
    CHECK(c->coupling_type == "threaded");
    CHECK(d->shell_size == "M23");
    CHECK(d->coding.empty());
    CHECK(d->coupling_type.empty());
}

TEST_CASE("connector identity 2026-11-12: FPC actuator type (familyFpcFfc.actuatorType)",
          "[qarlos][crossref][connector-identity]") {
    using namespace variant_ids;
    // 512810894 (Molex Easy-On, nonZif) -> 545500833 (Molex, slide): both stated, different.
    const json diff = rank_one(k512810894, k545500833);
    CHECK(verdict_of(diff, "actuator_type") == "fail");
    CHECK(diff["status"] == "no_substitute");
    CHECK(has_note(diff, "FPC actuator type (familyFpcFfc.actuatorType) differs: nonZif -> slide"));
    // 545500833 (slide) -> 512960833 (no actuatorType on record): one side only.
    const json one = rank_one(k545500833, k512960833);
    CHECK(verdict_of(one, "actuator_type") == "unverified");
    CHECK(one["status"] != "no_substitute");
    // Partial only because the substitute is not marked production (lifecycle), not for
    // this axis: a one-sided variant identity does not cap (interface_standard's treatment).
    CHECK(has_note(one, "substitute is not marked production status"));
    CHECK(has_note(one, "FPC actuator type (familyFpcFfc.actuatorType) not established "
                        "(original: slide, substitute: not stated)"));
    // CAT-M5833-M2934 -> CAT-M5833-C8684 (TE): neither states it.
    const json none = rank_one(kCAT_M5833_M2934, kCAT_M5833_C8684);
    CHECK(verdict_of(none, "actuator_type") == "");
    CHECK(notes_with(none, "actuatorType") == 0);
}

TEST_CASE("connector identity 2026-11-12: circular shell size (familyCircular.shellSize)",
          "[qarlos][crossref][connector-identity]") {
    using namespace variant_ids;
    // 1924950000 (Weidmüller, M12) -> 1224150000 (Weidmüller, M23), both 12-way.
    const json diff = rank_one(k1924950000, k1224150000);
    CHECK(verdict_of(diff, "shell_size") == "fail");
    CHECK(diff["status"] == "no_substitute");
    CHECK(has_note(diff, "circular shell size (familyCircular.shellSize) differs: M12 -> M23"));
    // 1224000000 (Weidmüller, M23) -> DIV40E1706PA-023 (TE, no shellSize): one side only.
    const json one = rank_one(k1224000000, kDIV40E1706PA_023);
    CHECK(verdict_of(one, "shell_size") == "unverified");
    CHECK(one["status"] != "no_substitute");
    // No cap: the one-sided variant identity leaves drop_in reachable, exactly as a
    // one-sided interface_standard does (below).
    CHECK(one["grade"] == "drop_in");
    CHECK(has_note(one, "circular shell size (familyCircular.shellSize) not established "
                        "(original: M23, substitute: not stated)"));
    // The treatment mirrored: the same pair with interface_standard stated on the original
    // only (the field added to the spec here, not taken from the record) is also UNVERIFIED,
    // carries no note and keeps drop_in.
    {
        json o = spec_of(k1224000000), c = spec_of(kDIV40E1706PA_023);
        o["shell_size"] = nullptr;
        o["interface_standard"] = "M23";
        kelvin::crossref::Options opt;
        auto r = kelvin::crossref::cross_reference("connector", o, json::array({c}), opt);
        const json& m = r["candidates"][0];
        CHECK(verdict_of(m, "interface_standard") == "unverified");
        CHECK(m["grade"] == "drop_in");
        CHECK(notes_with(m, "M23") == 0);
    }
    // CDM520-04A-01FRA-3M-67 -> CDM519-04A-01FRA-2M-67 (CUI, coding A, no shell size on
    // either record).
    const json none = rank_one(kCDM520_04A, kCDM519_04A);
    CHECK(verdict_of(none, "shell_size") == "");
    CHECK(notes_with(none, "shellSize") == 0);
}

TEST_CASE("connector identity 2026-11-12: circular coding (familyCircular.coding)",
          "[qarlos][crossref][connector-identity]") {
    using namespace variant_ids;
    // CDM818-05B-01FRA-2M-67 (CUI, B-coded) -> CDM1237-05A-01FRA-3M-67 (CUI, A-coded).
    const json diff = rank_one(kCDM818_05B, kCDM1237_05A);
    CHECK(verdict_of(diff, "coding") == "fail");
    CHECK(diff["status"] == "no_substitute");
    CHECK(has_note(diff, "circular coding (familyCircular.coding) differs: B -> A"));
    // CDM802-08A-FP-F010-67 (CUI, A) -> ZPF000000000005657 (TE, no coding): one side only.
    const json one = rank_one(kCDM802_08A, kZPF5657);
    CHECK(verdict_of(one, "coding") == "unverified");
    CHECK(one["status"] != "no_substitute");
    // No cap: the one-sided variant identity leaves drop_in reachable, exactly as a
    // one-sided interface_standard does (below).
    CHECK(one["grade"] == "drop_in");
    CHECK(has_note(one, "circular coding (familyCircular.coding) not established "
                        "(original: A, substitute: not stated)"));
    // 1224150000 -> 1224570000 (Weidmüller M23, no coding on either).
    const json none = rank_one(k1224150000, k1224570000);
    CHECK(verdict_of(none, "coding") == "");
    CHECK(notes_with(none, "coding)") == 0);
}

TEST_CASE("connector identity 2026-11-12: circular coupling (familyCircular.couplingType)",
          "[qarlos][crossref][connector-identity]") {
    using namespace variant_ids;
    // D38999/24FA98AN (threaded, shell 9) -> SJT07RT-8-98P (bayonet, shell 8). No catalogue
    // pair differs in coupling alone (a threaded and a bayonet part never share a shell
    // code), so the shell differs too; coupling is checked first and is the reason named.
    const json diff = rank_one(kD38999_24FA98AN, kSJT07RT_8_98P);
    CHECK(verdict_of(diff, "coupling_type") == "fail");
    CHECK(diff["status"] == "no_substitute");
    CHECK(has_note(diff, "circular coupling type (familyCircular.couplingType) differs: "
                         "threaded -> bayonet"));
    // SJT06RT-12-22S (bayonet, 12, N) -> MS27484T12B22B (12, N, no couplingType).
    const json one = rank_one(kSJT06RT_12_22S, kMS27484T12B22B);
    CHECK(verdict_of(one, "coupling_type") == "unverified");
    CHECK(verdict_of(one, "shell_size") == "pass");
    CHECK(verdict_of(one, "coding") == "pass");
    CHECK(one["status"] != "no_substitute");
    // Partial only because the substitute is not marked production (lifecycle), not for
    // this axis: a one-sided variant identity does not cap (interface_standard's treatment).
    CHECK(has_note(one, "substitute is not marked production status"));
    CHECK(has_note(one, "circular coupling type (familyCircular.couplingType) not established "
                        "(original: bayonet, substitute: not stated)"));
    // 1224150000 -> 1224570000: neither states a coupling.
    const json none = rank_one(k1224150000, k1224570000);
    CHECK(verdict_of(none, "coupling_type") == "");
    CHECK(notes_with(none, "couplingType") == 0);
}

// ── Several one-sided connector identities: major_review (owner 2026-11-12) ────────────────
// One identity stated by one record only keeps its single-gap treatment (minor_review for
// gender / termination / pitch / rows / board mount, note only for a family-variant identity).
// THREE OR MORE such axes on one candidate cap the grade at major_review, with a note naming
// them. Verbatim lines of tools/qarlos/bench/data-0d5241d/connectors.ndjson (TAS 0d5241d).
namespace identity_gaps {
// connectors.ndjson line 394489: D38999/20MG11BN
const char* kD38999_20MG11BN = R"KREC({"connector": {"manufacturerInfo": {"name": "Amphenol Aerospace", "reference": "D38999/20MG11BN", "family": "MIL-DTL-38999 Series III", "description": "MIL-DTL-38999 Series III wall mounting receptacle, shell size 21, insert arrangement 21-11, 11 contacts, socket insert, supplied less contacts", "datasheetUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "datasheetInfo": {"part": {"partNumber": "D38999/20MG11BN", "series": "MIL-DTL-38999 Series III", "matingPolarity": "female", "description": "MIL-DTL-38999 Series III wall mounting receptacle, shell size 21, insert arrangement 21-11, 11 contacts, socket insert, supplied less contacts"}, "electrical": {"ratedVoltage": 600.0, "dielectricWithstandingVoltage": 1800.0, "ratedCurrentPerContact": 23.0}, "mechanical": {"positions": 11, "mountingStyle": "panel"}, "familyDetails": {"family": "circular", "shellSize": "21", "coding": "N", "couplingType": "threaded"}, "provenance": [{"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["part.matingPolarity", "mechanical.positions", "mechanical.mountingStyle", "familyDetails.shellSize", "familyDetails.coding"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "Insert Availability chart and How to Order (Military) key decoded for this order code"}, {"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["electrical.ratedVoltage", "electrical.dielectricWithstandingVoltage"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "Service rating I: suggested operating voltage 600 VAC rms, test voltage 1800 VRMS at sea level"}, {"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["electrical.ratedCurrentPerContact"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "Contact rating test current for contact sizes 12"}, {"source": "manufacturerDatasheet", "sourceName": "Amphenol Aerospace MIL-DTL-38999 catalogue", "sourceUrl": "https://www.amphenol-aerospace.com/resources/literature/view/38999-catalog", "retrievedDate": "2026-10-10", "fields": ["familyDetails.couplingType"], "verification": "seriesConfirmed", "verificationDate": "2026-10-10", "verificationMethod": "Triple Start Threads"}]}}}})KREC";
// connectors.ndjson line 136513: YDIV46E21-11PNC009
const char* kYDIV46E21_11PNC009 = R"KREC({"connector":{"manufacturerInfo":{"name":"TE Connectivity","reference":"YDIV46E21-11PNC009","status":"production","datasheetInfo":{"part":{"partNumber":"YDIV46E21-11PNC009","description":"Wire-to-Wire, Circular Connector Plug, 11 Position, Shell Size G (21), .06 mm [.1 in] Centerline, Sealable, Wire & Cable, DEUTSCH 38999 Series IV","series":"DEUTSCH"},"electrical":{"ratedCurrentPerContact":23.0},"mechanical":{"positions":11},"familyDetails":{"family":"circular"},"environmental":{"operatingTemperature":{"minimum":-65.0,"maximum":200.0}},"provenance":[{"source":"manufacturerParametric","sourceName":"TE Connectivity product catalogue","verification":"valuesReadFromSource","verificationDate":"2026-09-07","retrievedDate":"2026-06-24"}]},"family":"DEUTSCH","description":"Wire-to-Wire, Circular Connector Plug, 11 Position, Shell Size G (21), .06 mm [.1 in] Centerline, Sealable, Wire & Cable, DEUTSCH 38999 Series IV"}}})KREC";
// connectors.ndjson line 224128: CPM1203-05A-02-FRA-M9-67
const char* kCPM1203_05A = R"KREC({"connector": {"manufacturerInfo": {"name": "CUI Devices", "reference": "CPM1203-05A-02-FRA-M9-67", "status": "production", "datasheetInfo": {"part": {"partNumber": "CPM1203-05A-02-FRA-M9-67", "description": "Plug, Female", "series": "CPM1203", "matingPolarity": "male"}, "electrical": {"ratedCurrentPerContact": 4.0, "ratedVoltage": 60.0}, "mechanical": {"positions": 5, "mountingStyle": "cable"}, "provenance": [{"source": "manufacturerParametric", "verification": "inferredNotVerified", "verificationDate": "2026-09-06", "sourceName": "Same Sky product catalogue", "sourceUrl": "https://www.sameskydevices.com/catalog/interconnect", "retrievedDate": "2026-07-31"}, {"source": "manufacturerParametric", "sourceName": "Same Sky interconnect catalogue", "sourceUrl": "https://www.sameskydevices.com/product/interconnect/connectors/circular-connectors/cpm1203-05a-02-fra-m9-67", "retrievedDate": "2026-10-18", "fields": ["part.partNumber", "electrical.ratedCurrentPerContact", "electrical.ratedVoltage", "mechanical.positions"], "verification": "partNamed", "verificationDate": "2026-10-18"}], "environmental": {"operatingTemperature": {"minimum": -20.0, "maximum": 80.0}}, "familyDetails": {"family": "circular", "shellSize": "M12", "coding": "A", "shielded": true}}, "description": "Plug, Female", "datasheetUrl": "https://www.sameskydevices.com/product/resource/cpm1203.pdf"}}})KREC";
}  // namespace identity_gaps

// Qarlos bench 20365838: D38999/20MG11BN (female, panel, shell 21, coding N, threaded) took
// YDIV46E21-11PNC009 (a wire-to-wire plug whose record states none of the five) as
// minor_review.
TEST_CASE("connector identity 2026-11-12: three or more one-sided identities cap major_review",
          "[qarlos][crossref][connector-identity][identity-gaps]") {
    using namespace variant_ids;
    using namespace identity_gaps;
    const json c = rank_one(kD38999_20MG11BN, kYDIV46E21_11PNC009);
    CHECK(c["status"] != "no_substitute");
    CHECK(verdict_of(c, "polarity") == "unverified");
    CHECK(verdict_of(c, "mounting") == "unverified");
    CHECK(verdict_of(c, "shell_size") == "unverified");
    CHECK(verdict_of(c, "coding") == "unverified");
    CHECK(verdict_of(c, "coupling_type") == "unverified");
    CHECK(c["grade"] == "major_review");
    CHECK(has_note(c, "5 connector identity axes are unverified (stated on one side only: "
                      "mating gender, circular coupling type (familyCircular.couplingType), "
                      "circular coding (familyCircular.coding), circular shell size "
                      "(familyCircular.shellSize), board mount)"));
    // Each single-axis note is still there.
    CHECK(has_note(c, "mating gender not established (original: female, substitute: not "
                      "stated)"));
    CHECK(has_note(c, "the original is panel-mounted"));
}

// Boundary: two one-sided identities keep the single-gap grade. CPM1203-05A-02-FRA-M9-67 (CUI,
// male, cable, M12, A-coded) -> CDM1237-05A-01FRA-3M-67 (CUI, cable, A-coded, no gender and no
// shell size on record): gender (capped) + shell size (a variant identity) = 2.
TEST_CASE("connector identity 2026-11-12: two one-sided identities stay minor_review",
          "[qarlos][crossref][connector-identity][identity-gaps]") {
    using namespace variant_ids;
    using namespace identity_gaps;
    const json c = rank_one(kCPM1203_05A, kCDM1237_05A);
    CHECK(c["status"] != "no_substitute");
    CHECK(verdict_of(c, "polarity") == "unverified");
    CHECK(verdict_of(c, "shell_size") == "unverified");
    CHECK(verdict_of(c, "coding") == "pass");
    CHECK(verdict_of(c, "mounting") == "pass");
    CHECK(c["grade"] == "minor_review");
    CHECK(notes_with(c, "connector identity axes are unverified") == 0);
}

// ── Qarlos r11-r13, group D (diode + MOSFET) ─────────────────────────────────────────────
// REAL TAS records: every line of tests/fixtures/qarlos_r11r13_d_{diodes,mosfets}.ndjson is a
// verbatim line of tools/qarlos/bench/data-0d5241d (TAS 0d5241d), run through the real
// extractor and browse row, then into the spec web/src/crossref.js builds from that row.
namespace r11r13_d {
const json& row(const std::string& family, const std::string& mpn) {
    static const json mos = [] {
        auto sh = kelvin::build_mosfet_shard(std::string(KELVIN_TEST_DIR) +
                                             "/fixtures/qarlos_r11r13_d_mosfets.ndjson");
        return kelvin::browse::browse_rows(sh, json{{"limit", 100}})["rows"];
    }();
    static const json dio = [] {
        auto sh = kelvin::build_diode_shard(std::string(KELVIN_TEST_DIR) +
                                            "/fixtures/qarlos_r11r13_d_diodes.ndjson");
        return kelvin::browse::browse_rows(sh, json{{"limit", 100}})["rows"];
    }();
    for (const auto& r : family == "mosfet" ? mos : dio)
        if (r.value("mpn", "") == mpn) return r;
    throw std::runtime_error("fixture has no " + family + " record " + mpn);
}
// web/src/crossref.js's MOSFET spec, including coss_vds (v21).
json spec(const std::string& family, const std::string& mpn) {
    const json& r = row(family, mpn);
    if (family != "mosfet") return diode_spec(r);
    json s = mosfet_spec(r);
    s["coss_vds"] = nz_(r["coss_vds"]);
    return s;
}
json rank(const std::string& family, const std::string& orig,
          const std::vector<std::string>& cands) {
    json c = json::array();
    for (const auto& m : cands) c.push_back(spec(family, m));
    kelvin::crossref::Options opt;
    opt.max_results = 20;
    return kelvin::crossref::cross_reference(family, spec(family, orig), c, opt);
}
json find(const json& r, const std::string& mpn) {
    for (const auto& c : r["candidates"])
        if (c["mpn"] == mpn) return c;
    return json(nullptr);
}
}  // namespace r11r13_d

// D1 (r13 seeds 25078643 / 25183372): SMB10J30CAH and SMCG75CA-HRA are bidirectional, but
// neither series is decoded, so SMBJ30A came back with a bare 'unverified' polarity and no
// note, and SMCG78A-M3/57T with no polarity row at all. Every protection pair now reports the
// polarity, and an unknown side is UNVERIFIED with a note naming which side could not be read.
TEST_CASE("qarlos r11r13 D1: an unknown TVS polarity is always reported, with a note",
          "[qarlos][crossref][r11r13-d]") {
    using namespace r11r13_d;
    // Original undecodable, substitute decoded.
    const json r = rank("diode", "SMB10J30CAH", {"SMBJ30A", "SMBJ30CA"});
    const json uni = find(r, "SMBJ30A"), bi = find(r, "SMBJ30CA");
    REQUIRE(!uni.is_null());
    REQUIRE(!bi.is_null());
    CHECK(verdict_of(uni, "polarity") == "unverified");
    CHECK(has_note(uni, "the substitute is unidirectional (decoded from its part number); the "
                        "original's polarity cannot be read from its record or part number"));
    CHECK(verdict_of(bi, "polarity") == "unverified");
    CHECK(has_note(bi, "the substitute is bidirectional (decoded from its part number)"));
    // Neither side decodable: a row and a note, where there was nothing.
    const json r2 = rank("diode", "SMCG75CA-HRA", {"SMCG78A-M3/57T"});
    const json both = find(r2, "SMCG78A-M3/57T");
    REQUIRE(!both.is_null());
    CHECK(verdict_of(both, "polarity") == "unverified");
    CHECK(has_note(both, "TVS polarity (unidirectional / bidirectional) is stated by neither "
                         "record and decodes from neither part number"));
    // No cap: the grade stays what the other axes make it (lifecycle + surge warns).
    CHECK(both["grade"] == "minor_review");
    // Original decoded unidirectional, substitute undecodable.
    const json r3 = rank("diode", "SMBJ30A", {"SMB10J30CAH"});
    const json u = find(r3, "SMB10J30CAH");
    REQUIRE(!u.is_null());
    CHECK(verdict_of(u, "polarity") == "unverified");
    CHECK(has_note(u, "the original is unidirectional (decoded from its part number)"));
    // Decoded on both sides: unchanged — a uni part for a bi original FAILS.
    const json r4 = rank("diode", "SMBJ30CA", {"SMBJ30A"});
    const json f = find(r4, "SMBJ30A");
    REQUIRE(!f.is_null());
    CHECK(verdict_of(f, "polarity") == "fail");
}

// D2 (r12 seed 24659727): TS10P06G-K is a 10 A single-phase bridge in Taiwan Semiconductor's
// TS-6P outline. TS-6P was not a known bridge outline, so single diodes (10A07 in R-6, S10CK
// in SMC) came back minor_review upgrades with only a package note.
TEST_CASE("qarlos r11r13 D2: TS-6P and the other catalogue bridge outlines declare a module",
          "[qarlos][crossref][r11r13-d]") {
    using namespace r11r13_d;
    using kelvin::crossref::bridge_outline;
    for (const char* c : {"TS-6P", "TS-6PL", "TS4K", "D3K", "DBL", "DBLS", "KBU", "KBL", "KBJL",
                          "KBPF", "GBL", "ABS", "TBS", "YBS", "GBPC-M", "GBPC-W", "GBPC40-M",
                          "GBPC4 28.75x28.75x11.10", "GBU", "MBS"})
        CHECK(bridge_outline(c));
    for (const char* c : {"R-6", "SMC (DO-214AB)", "DO-214AA (SMB)", "DO-201AD", "TO-220AC 2L",
                          "SOD-123", "TS", ""})
        CHECK_FALSE(bridge_outline(c));

    const json r = rank("diode", "TS10P06G-K", {"10A07", "S10CK"});
    for (const char* m : {"10A07", "S10CK"}) {
        const json c = find(r, m);
        REQUIRE(!c.is_null());
        CHECK(verdict_of(c, "configuration") == "fail");
        CHECK(c["footprint"] == "different_land_pattern");
        CHECK(c["grade"] == "major_review");
        CHECK(has_note(c, "the original is a single-phase BRIDGE RECTIFIER module"));
    }
    // Bridge for bridge (same TS-6P outline): no configuration conflict.
    const json same = find(rank("diode", "TS10P05G-K", {"TS10P06G-K"}), "TS10P06G-K");
    REQUIRE(!same.is_null());
    CHECK(verdict_of(same, "configuration") == "");
}

// D3 (r11 seed 24240811): AOD4N60 states Coss 53 pF at 25 V (capacitanceMeasurementVds);
// IPD95R2K0P7 states 5 pF at no Vds (its own curve gives that near 400 V), and Coss was graded
// 'pass'. The diode Cj bias rule, applied to Vds: no common basis -> UNVERIFIED + note, no cap.
TEST_CASE("qarlos r11r13 D3: MOSFET Coss on different drain-source voltages is not compared",
          "[qarlos][crossref][views][r11r13-d]") {
    using namespace r11r13_d;
    CHECK(row("mosfet", "AOD4N60")["coss_vds"] == 25.0);
    CHECK(row("mosfet", "XP60CM600EH")["coss_vds"] == 100.0);
    CHECK(row("mosfet", "IPD95R2K0P7")["coss_vds"].is_null());

    const json r = rank("mosfet", "AOD4N60", {"IPD95R2K0P7", "AOD7N60", "XP60CM600EH"});
    const json none = find(r, "IPD95R2K0P7");
    REQUIRE(!none.is_null());
    CHECK(verdict_of(none, "coss") == "unverified");
    CHECK(verdict_of(none, "coss_basis") == "unverified");
    CHECK(has_note(none, "the original's output capacitance is 53 pF at 25 V; the substitute's "
                         "record states 5 pF at no drain-source voltage"));
    // Secondary axis: no demotion, no cap.
    CHECK(none["grade"] == "drop_in");
    const json other = find(r, "XP60CM600EH");
    REQUIRE(!other.is_null());
    CHECK(verdict_of(other, "coss") == "unverified");
    CHECK(has_note(other, "output capacitance quoted at different drain-source voltages "
                          "(original 53 pF at 25 V, substitute 20 pF at 100 V)"));
    // Same 25 V basis: compared as stated (88 pF against 53 pF, inside 2x).
    const json same = find(r, "AOD7N60");
    REQUIRE(!same.is_null());
    CHECK(verdict_of(same, "coss") == "warn");
    CHECK(verdict_of(same, "coss_basis") == "");
}

// D4 (r13 seed 25078643): FCH023N65S3L4 'TO-247-4' against IPZ65R019C7 'TO247 4-pin'
// (package PG-TO247-4) — the same four-lead TO-247 — was called a different package.
TEST_CASE("qarlos r11r13 D4: TO-247-4 / TO247 4-pin / PG-TO247-4 are one MOSFET outline",
          "[qarlos][crossref][r11r13-d]") {
    using namespace r11r13_d;
    using kelvin::crossref::case_identity;
    const std::string four = case_identity("mosfet", "TO-247-4");
    for (const char* c : {"TO247 4-pin", "PG-TO247-4", "TO247-4", "TO-247-4L", "TO247-4L"})
        CHECK(case_identity("mosfet", c) == four);
    for (const char* c : {"TO-247", "TO247", "PG-TO247-3", "TO247 4-pin (asymmetric leads)",
                          "TO-247-4L(X)", "HiP247-4", "TO-247-4 HC 8.8mm"})
        CHECK(case_identity("mosfet", c) != four);

    const json r = rank("mosfet", "FCH023N65S3L4", {"IPZ65R019C7", "IPW65R019C7"});
    const json z = find(r, "IPZ65R019C7"), w = find(r, "IPW65R019C7");
    REQUIRE(!z.is_null());
    REQUIRE(!w.is_null());
    CHECK(z["footprint"] == "fits");
    CHECK(verdict_of(z, "footprint") == "pass");
    // The 3-lead TO-247 stays a different package.
    CHECK(w["footprint"] == "different_case");
}

// D5 (r13 seed 25078643): the 600 V SiHL033N60SF for the 650 V FCH023N65S3L4 carried a 'warn'
// Vds row and no note. A critical-rating shortfall that is not a rejection now says so.
TEST_CASE("qarlos r11r13 D5: a critical-rating shortfall carries a note",
          "[qarlos][crossref][r11r13-d]") {
    using namespace r11r13_d;
    const json c = find(rank("mosfet", "FCH023N65S3L4", {"SiHL033N60SF"}), "SiHL033N60SF");
    REQUIRE(!c.is_null());
    CHECK(verdict_of(c, "vds") == "warn");
    CHECK(has_note(c, "drain-source voltage rating 600 V against the original's 650 V (-7.69 %) "
                      "— below the original's rating"));
    // Verdict and grade unchanged by the note.
    CHECK(c["grade"] == "minor_review");
    // A protection diode's standoff is noted by the standoff-band rule, not twice.
    const json t = find(rank("diode", "SMBJ30CA", {"SMBJ30A"}), "SMBJ30A");
    REQUIRE(!t.is_null());
    CHECK(!has_note(t, "reverse voltage rating"));
}
// ── Qarlos r11-r13 group E (timing + capacitor) ─────────────────────────────

namespace group_e {
// The flat spec web/src/crossref.js builds for a timing row (base() + the timing spec).
json timing_spec(const std::string& mpn, const std::string& technology, double frequency,
                 const std::string& case_code) {
    return {{"mpn", mpn},           {"subtype", "oscillator"}, {"device_type", "oscillator"},
            {"technology", technology}, {"frequency", frequency}, {"case_code", case_code},
            {"mount", ""},          {"is_production", true}};
}
}  // namespace group_e

TEST_CASE("qarlos r12/r13 group E: a generic package string names no land",
          "[qarlos][crossref][timing][footprint]") {
    using kelvin::crossref::generic_package_names_no_land;
    // The style descriptors the TDAS catalogue states with no size (survey of TAS 0d5241d).
    CHECK(generic_package_names_no_land("4-SMD, No Lead"));
    CHECK(generic_package_names_no_land("2-SMD, No Lead"));
    CHECK(generic_package_names_no_land("SMD"));
    CHECK(generic_package_names_no_land("3-SMD, Non-Standard"));
    CHECK(generic_package_names_no_land("4-SMD, Flat Leads"));
    CHECK(generic_package_names_no_land("Cylindrical Can, Radial"));
    CHECK(generic_package_names_no_land("2 Lead Plastic"));
    // A size, a registered outline or a vendor package code makes the string specific.
    CHECK_FALSE(generic_package_names_no_land("3225 4-SMD"));
    CHECK_FALSE(generic_package_names_no_land("14x9 4-SMD"));
    CHECK_FALSE(generic_package_names_no_land("HC-49/US"));
    CHECK_FALSE(generic_package_names_no_land("IQXC-42"));
    CHECK_FALSE(generic_package_names_no_land("14-DIP, 4 Leads (Full Size, Metal Can)"));
    CHECK_FALSE(generic_package_names_no_land("0603"));
    CHECK_FALSE(generic_package_names_no_land(""));
}

TEST_CASE("qarlos r12 24659727: '4-SMD, No Lead' on both records is not a footprint fit",
          "[qarlos][crossref][timing][footprint]") {
    using group_e::timing_spec;
    // 7M-26.000MAHE-T (TXC) -> FA-20H 26.0000MF10Z-W (EPSON): both TDAS records state the
    // package "4-SMD, No Lead" and no mechanical block (TAS 0d5241d timing_devices.ndjson).
    json orig = timing_spec("7M-26.000MAHE-T", "quartzCrystal", 26e6, "4-SMD, No Lead");
    orig["load_capacitance"] = 12e-12;
    orig["load_capacitance_pF"] = 12.0;
    orig["esr"] = 60.0;
    orig["mode"] = "fundamental";
    orig["tolerance_ppm"] = 30.0;
    orig["stability_ppm"] = 30.0;
    json cand = orig;
    cand["mpn"] = "FA-20H 26.0000MF10Z-W";
    cand["tolerance_ppm"] = 10.0;
    cand["stability_ppm"] = 10.0;
    auto r = kelvin::crossref::cross_reference("timeBase", orig, json::array({cand}));
    const json& c = r["candidates"][0];
    CHECK(c["footprint"] == "unknown");
    CHECK(verdict_of(c, "footprint") == "unverified");
    CHECK(has_note(c, "both records state only the generic package '4-SMD, No Lead'"));
    // Neither record states the datum: noted, not capped (owner 2026-11-12).
    CHECK(c["grade"] == "drop_in");

    // A size-bearing code still names the land: "3225 4-SMD" on both is a fit.
    json o2 = orig, c2 = cand;
    o2["case_code"] = "3225 4-SMD";
    c2["case_code"] = "3225 4-SMD";
    auto r2 = kelvin::crossref::cross_reference("timeBase", o2, json::array({c2}));
    CHECK(r2["candidates"][0]["footprint"] == "fits");
    CHECK(verdict_of(r2["candidates"][0], "footprint") == "pass");

    // The substitute states a land while the original names only the generic string: the
    // one-sided rung, which keeps its cap.
    json c3 = cand;
    c3["length_m"] = 2.0e-3;
    c3["width_m"] = 1.6e-3;
    auto r3 = kelvin::crossref::cross_reference("timeBase", orig, json::array({c3}));
    CHECK(r3["candidates"][0]["footprint"] == "unknown");
    CHECK(r3["candidates"][0]["grade"] == "minor_review");
}

TEST_CASE("qarlos bench 20284866: a crystal's higher ESR carries a note",
          "[qarlos][crossref][timing]") {
    using group_e::timing_spec;
    // ABMM-14.7456MHZ-D2X-T (50 ohm) -> ECS-147.4-18-23A-JGN-TR (80 ohm), same CL 18 pF.
    json orig = timing_spec("ABMM-14.7456MHZ-D2X-T", "quartzCrystal", 14745600.0, "");
    orig["load_capacitance"] = 18e-12;
    orig["load_capacitance_pF"] = 18.0;
    orig["esr"] = 50.0;
    orig["mode"] = "fundamental";
    json cand = orig;
    cand["mpn"] = "ECS-147.4-18-23A-JGN-TR";
    cand["esr"] = 80.0;
    auto r = kelvin::crossref::cross_reference("timeBase", orig, json::array({cand}));
    const json& c = r["candidates"][0];
    CHECK(verdict_of(c, "esr") == "fail");
    CHECK(has_note(c, "higher ESR: the substitute's maximum is 80 ohm against the original's "
                      "50 ohm"));
    // A lower ESR passes with no ESR note.
    json better = orig;
    better["mpn"] = "LOW-ESR";
    better["esr"] = 40.0;
    auto rb = kelvin::crossref::cross_reference("timeBase", orig, json::array({better}));
    CHECK(verdict_of(rb["candidates"][0], "esr") == "pass");
    CHECK_FALSE(has_note(rb["candidates"][0], "higher ESR"));
}

TEST_CASE("qarlos r11/r13 group E: a ceramic resonator's built-in capacitance is extracted",
          "[qarlos][views][timing]") {
    // Verbatim electrical block of CSTNE16M0VH3C000R0 (TAS 0d5241d timing_devices.ndjson).
    const json env = {{"timeBase", {{"oscillator", {{"manufacturerInfo",
        {{"name", "Murata"}, {"reference", "CSTNE16M0VH3C000R0"},
         {"datasheetInfo",
          {{"part", {{"partNumber", "CSTNE16M0VH3C000R0"}, {"series", "CSTNE16M0VH3C000R0"},
                     {"case", "SMD"}}},
           {"electrical", {{"technology", "ceramicResonator"}, {"outputType", "none"},
                           {"frequency", 16000000.0}, {"frequencyTolerance", 0.0007},
                           {"frequencyStability", 0.0013}, {"resonantImpedance", 40.0},
                           {"builtInCapacitance", 1.5e-11}}}}}}}}}}}};
    auto row = kelvin::extract_timing(env);
    REQUIRE(row.has_value());
    CHECK(row->built_in_capacitance == Approx(1.5e-11));
    // Still not a crystal CL: the two are different quantities.
    CHECK(std::isnan(row->load_capacitance));
}

TEST_CASE("qarlos r11/r13 group E: ceramic resonator built-in capacitors are compared",
          "[qarlos][crossref][timing]") {
    using group_e::timing_spec;
    // CSTNE16M0VH3C000R0 (15 pF built in) against AWSCR-16.00CV-T (22 pF built in) and
    // AWSZT-16.00CV-T (no builtInCapacitance on record), TAS 0d5241d.
    json orig = timing_spec("CSTNE16M0VH3C000R0", "ceramicResonator", 16e6, "SMD");
    orig["built_in_capacitance"] = 15e-12;
    json c22 = timing_spec("AWSCR-16.00CV-T", "ceramicResonator", 16e6, "");
    c22["built_in_capacitance"] = 22e-12;
    json none = timing_spec("AWSZT-16.00CV-T", "ceramicResonator", 16e6, "");
    json same = timing_spec("CSTNE16M0V53C000R0", "ceramicResonator", 16e6, "");
    same["built_in_capacitance"] = 15e-12;
    auto r = kelvin::crossref::cross_reference("timeBase", orig, json::array({c22, none, same}));
    json a, b, s;
    for (const auto& c : r["candidates"]) {
        if (c["mpn"] == "AWSCR-16.00CV-T") a = c;
        if (c["mpn"] == "AWSZT-16.00CV-T") b = c;
        if (c["mpn"] == "CSTNE16M0V53C000R0") s = c;
    }
    REQUIRE(!a.is_null());
    REQUIRE(!b.is_null());
    REQUIRE(!s.is_null());
    CHECK(a["status"] == "no_substitute");
    CHECK(verdict_of(a, "built_in_capacitance") == "fail");
    CHECK(has_note(a, "different built-in load capacitors (22 pF vs 15 pF)"));
    CHECK_FALSE(has_note(a, "ppm off frequency"));
    CHECK(b["status"] != "no_substitute");
    CHECK(verdict_of(b, "built_in_capacitance") == "unverified");
    CHECK(has_note(b, "the original has 15 pF built-in load capacitors; the substitute's "
                      "record states none"));
    CHECK(verdict_of(s, "built_in_capacitance") == "pass");
    CHECK_FALSE(has_note(s, "built-in"));
}

TEST_CASE("an unknown footprint costs the ladder's UNVERIFIED rung when the original has no land",
          "[qarlos][crossref][footprint]") {
    // Owner, 2026-11-12 (Qarlos bench 20284866, P4SMA120CA-Q -> 1KSMBJ160A; r11 24136082,
    // TSM120N06LCS -> BSC160N15NS5ATMA1): with no land on the original, a substitute stating
    // NO package paid nothing for its unknown fit, while one whose package was KNOWN to
    // differ paid unknown + a warn — so knowing less ranked better. Unknown now costs
    // kUnknownDimPenalty here as it does in every other unknown-footprint branch.
    const json orig = {{"mpn", "ORIG"}, {"vds", 60.0}, {"id", 10.0}, {"rds_on", 0.012},
                       {"channel", "nChannel"}};  // no package, no land
    json unknown = {{"mpn", "NOCASE"}, {"vds", 60.0}, {"id", 10.0}, {"rds_on", 0.012},
                    {"channel", "nChannel"}};
    auto r = kelvin::crossref::cross_reference("mosfet", orig, json::array({unknown}));
    REQUIRE(r["candidates"].size() == 1);
    const json& c = r["candidates"][0];
    CHECK(c["footprint"] == "unknown");
    CHECK(verdict_of(c, "footprint") == "unverified");
    CHECK(c["penalty"].get<double>() >= kelvin::crossref::kUnknownDimPenalty - 1e-9);
}

// ── Footprint stated by NEITHER record: UNVERIFIED, penalised, not capped (owner 2026-11-12) ──
// Missing data on BOTH sides is not capped. With no land dimension and no package / case code on
// either record, the footprint verdict stays UNVERIFIED and pays kUnknownDimPenalty, but the
// grade is not held below drop_in for it (the generic-package case already worked this way). A
// record on ONE side stating a land or a package keeps the cap. Verbatim lines of
// tools/qarlos/bench/data-0d5241d/magnetics.ndjson (TAS 0d5241d), through the real extractor.
namespace footprint_both_unstated {
// magnetics.ndjson line 78585: ELC09D331DFN
const char* kELC09D331DFN = R"KREC({"magnetic":{"manufacturerInfo":{"name":"Panasonic","reference":"ELC09D331DFN","status":"obsolete","datasheetUrl":"https://industrial.panasonic.com/cdbs/www-data/pdf/AGB0000/AGB0000C1.pdf","family":"General type 09D, 11D, 12D, 16B, 18B(Ferrite core type)","datasheetInfo":{"part":{"partNumber":"ELC09D331DFN","description":"General type 09D, 11D, 12D, 16B, 18B(Ferrite core type)"},"electrical":[{"subtype":"inductor","inductance":{"nominal":0.00033},"dcResistance":{"nominal":0.65},"ratedCurrents":[0.49]}],"provenance":[{"source":"scrape","sourceName":"Panasonic Industrial parametric search","sourceUrl":"https://industrial.panasonic.com/cdbs/www-data/pdf/AGB0000/AGB0000C1.pdf","retrievedDate":"2026-08-01","verification":"seriesConfirmed","verificationDate":"2026-10-09"}]}}}})KREC";
// magnetics.ndjson line 5253: SRN6045TA-331K
const char* kSRN6045TA_331K = R"KREC({"magnetic":{"manufacturerInfo":{"name":"Bourns","reference":"SRN6045TA-331K","status":"production","datasheetUrl":"https://www.mouser.com/datasheet/3/40/1/srn6045ta.pdf","datasheetInfo":{"part":{"description":"Power Inductors - SMD 330uH, 20% Semi-Shielded Power Inductor","shielded":true},"electrical":[{"subtype":"inductor","inductance":{"nominal":0.00033},"dcResistance":{"maximum":0.6},"saturationCurrentPeak":0.6}],"provenance":[{"source":"distributor","sourceName":"datasheet via Mouser listing","sourceUrl":"https://www.mouser.com/datasheet/3/40/1/srn6045ta.pdf"}]}},"distributorsInfo":[{"name":"Mouser","reference":"652-SRN6045TA-331K","link":"https://www.mouser.es/ProductDetail/Bourns/SRN6045TA-331K?qs=5aG0NVq1C4wpF%2F6FOZjLGg%3D%3D","cost":{"value":0.45,"currency":"USD"},"quantity":3546,"updatedAt":"2026-04-08"},{"name":"Digi-Key","link":"https://www.digikey.com/en/products/detail/bourns-inc/SRN6045TA-331K/16360230","reference":null}]}})KREC";
// magnetics.ndjson line 4666: DO5022P-334MLD
const char* kDO5022P_334MLD = R"KREC({"magnetic":{"manufacturerInfo":{"name":"Coilcraft","reference":"DO5022P-334MLD","status":"production","datasheetUrl":"https://www.mouser.com/datasheet/3/995/1/do5022p.pdf","datasheetInfo":{"part":{"description":"Power Inductors - SMD 330uH Unshld 20% 1A 560mOhms"},"electrical":[{"subtype":"inductor","inductance":{"nominal":0.00033},"dcResistance":{"nominal":0.56},"saturationCurrentPeak":1.0,"inductancePoints":[{"current":0.0,"inductance":0.00033,"temperature":25.0},{"current":0.1,"inductance":0.000329973800808441,"temperature":25.0},{"current":0.2,"inductance":0.0003297581870362143,"temperature":25.0},{"current":0.3,"inductance":0.0003295097964030583,"temperature":25.0},{"current":0.4,"inductance":0.0003294336515186573,"temperature":25.0},{"current":0.5,"inductance":0.0003290044307207895,"temperature":25.0},{"current":0.6,"inductance":0.00032846751452272985,"temperature":25.0},{"current":0.7,"inductance":0.00032781465296628564,"temperature":25.0},{"current":0.8,"inductance":0.00032703625826220454,"temperature":25.0},{"current":0.9,"inductance":0.0003260975468029716,"temperature":25.0},{"current":1.0,"inductance":0.0003249489073535091,"temperature":25.0},{"current":1.1,"inductance":0.00032378287610028926,"temperature":25.0},{"current":1.2,"inductance":0.0003219814865803041,"temperature":25.0},{"current":1.3,"inductance":0.0003197413999591209,"temperature":25.0},{"current":1.4,"inductance":0.0003166384123053427,"temperature":25.0},{"current":1.5,"inductance":0.00031224452916623713,"temperature":25.0},{"current":1.6,"inductance":0.00030562683635049074,"temperature":25.0},{"current":1.7,"inductance":0.00029569756572179454,"temperature":25.0},{"current":1.8,"inductance":0.000281053221014082,"temperature":25.0},{"current":1.9,"inductance":0.0002611161936679368,"temperature":25.0},{"current":2.0,"inductance":0.0002331693488039749,"temperature":25.0},{"current":2.1,"inductance":0.00019378170718729418,"temperature":25.0},{"current":2.2,"inductance":0.00014585413249595022,"temperature":25.0},{"current":2.3,"inductance":0.00011077965821119112,"temperature":25.0},{"current":2.4,"inductance":9.254152022878252e-05,"temperature":25.0},{"current":2.5,"inductance":8.320320302430892e-05,"temperature":25.0}]}],"provenance":[{"source":"distributor","sourceName":"datasheet via Mouser listing","sourceUrl":"https://www.mouser.com/datasheet/3/995/1/do5022p.pdf"},{"source":"manufacturerParametric","sourceName":"Coilcraft parametric search","retrievedDate":"2026-07-21","fields":["mechanical.height","mechanical.length","mechanical.width"]},{"source":"manufacturerParametric","sourceName":"Coilcraft Power Inductor Finder (L vs DC bias)","sourceUrl":"https://www.coilcraft.com/en-us/products/power/unshielded-inductors/ferrite-drum-surface-mount/do/do5022p/do5022p-334/","retrievedDate":"2026-11-03","fields":["electrical.inductancePoints"],"verification":"valuesReadFromSource","verificationDate":"2026-11-03","verificationMethod":"Typical inductance normalised to nominal vs DC bias current, 25 degC ambient; series data for the tool part DO5022P-334"}],"mechanical":{"length":{"nominal":0.01854},"width":{"nominal":0.01524},"height":{"nominal":0.00711}}}},"distributorsInfo":[{"name":"Mouser","reference":"994-DO5022P-334MLD","link":"https://www.mouser.es/ProductDetail/Coilcraft/DO5022P-334MLD?qs=zCSbvcPd3pb4sas6YUSpOQ%3D%3D","cost":{"value":2.8,"currency":"USD"},"quantity":937,"updatedAt":"2026-04-08"}]}})KREC";
json spec(const char* line) {
    auto r = kelvin::extract_magnetic(real_mag(line));
    REQUIRE(r.has_value());
    return mag_spec_xref(*r);
}
json one(const json& orig, const json& cand) {
    kelvin::crossref::Options opt;
    auto r = kelvin::crossref::cross_reference("magnetic", orig, json::array({cand}), opt);
    REQUIRE(r["candidates"].size() == 1);
    return r["candidates"][0];
}
}  // namespace footprint_both_unstated

// Qarlos bench 20575296: ELC09D331DFN (Panasonic) -> SRN6045TA-331K (Bourns). Neither record
// states a land or a package; the substitute was capped at minor_review for a gap that is the
// catalogue's, identical for every candidate.
TEST_CASE("footprint stated by neither record: UNVERIFIED and penalised, not capped",
          "[qarlos][crossref][footprint][footprint-both-unstated]") {
    using namespace footprint_both_unstated;
    const json o = spec(kELC09D331DFN), s = spec(kSRN6045TA_331K);
    REQUIRE(o["case_code"] == "");
    REQUIRE(s["case_code"] == "");
    REQUIRE(o["length_m"].is_null());
    REQUIRE(s["length_m"].is_null());
    const json c = one(o, s);
    CHECK(c["footprint"] == "unknown");
    CHECK(verdict_of(c, "footprint") == "unverified");
    CHECK(c["penalty"].get<double>() >= kelvin::crossref::kUnknownDimPenalty - 1e-9);
    CHECK(c["grade"] == "drop_in");
}

// Boundary: ONE record states the land or the package, so the gap is one-sided and keeps its
// minor_review cap.
TEST_CASE("footprint stated by one record only keeps its cap",
          "[qarlos][crossref][footprint][footprint-both-unstated]") {
    using namespace footprint_both_unstated;
    const json o = spec(kELC09D331DFN);
    // DO5022P-334MLD (Coilcraft) states an 18.54 x 15.24 mm body; the original states nothing.
    const json d = spec(kDO5022P_334MLD);
    REQUIRE(d["length_m"].is_number());
    const json c = one(o, d);
    CHECK(c["footprint"] == "unknown");
    CHECK(verdict_of(c, "footprint") == "unverified");
    CHECK(c["grade"] == "minor_review");
    // The pair above with a package stated on the original only (the field set here, not
    // taken from the record): one-sided again, capped.
    json oc = o;
    oc["case_code"] = "1210";
    const json p = one(oc, spec(kSRN6045TA_331K));
    CHECK(p["footprint"] == "unknown");
    CHECK(p["grade"] == "minor_review");
}

// ── Owner decisions 2026-11-13: timing technology crossings ──────────────────
// Every record below is the REAL TAS timing_devices.ndjson line (TAS 9cb6cf8, distributorsInfo
// dropped), run through the extractor, then projected to the spec web/src/crossref.js builds
// for a timing row (base() + the timing spec).
namespace timing_tech {
// timing_devices.ndjson line 7619: ASDM1-10.000MHZ-LC-T
const char* kASDM1_10_000MHZ_LC_T = R"KREC({"timeBase":{"oscillator":{"manufacturerInfo":{"name":"Abracon","reference":"ASDM1-10.000MHZ-LC-T","status":"production","series":"ASDM","datasheetInfo":{"part":{"partNumber":"ASDM1-10.000MHZ-LC-T","series":"ASDM","package":"2520 4-SMD","case":"2520 4-SMD"},"electrical":{"technology":"mems","frequency":10000000.0,"frequencyStability":4.9999999999999996e-05,"outputType":"cmos","supply":{"minimumSupplyVoltage":3.3,"maximumSupplyVoltage":3.3,"currentConsumption":0.015},"enableFunction":"standby"},"provenance":[{"source":"manufacturerParametric","sourceName":"Abracon parametric search","sourceUrl":"https://abracon.com/Oscillators/ASDM.pdf","retrievedDate":"2026-07-03","fields":["electrical.technology","electrical.frequency","electrical.frequencyStability","electrical.outputType","electrical.supply","electrical.enableFunction"],"verification":"seriesConfirmed","verificationDate":"2026-10-09"}],"thermal":{"operatingTemperature":{"minimum":-40.0,"maximum":85.0}}},"datasheetUrl":"https://abracon.com/Oscillators/ASDM.pdf"}}}})KREC";
// timing_devices.ndjson line 10892: ISM95-3251AH-10.000MHZ
const char* kISM95_3251AH_10_000MHZ = R"KREC({"timeBase":{"oscillator":{"manufacturerInfo":{"name":"ILSI","reference":"ISM95-3251AH-10.000MHZ","status":"production","series":"ISM95","datasheetInfo":{"part":{"partNumber":"ISM95-3251AH-10.000MHZ","series":"ISM95","package":"2520 4-SMD","case":"2520 4-SMD"},"electrical":{"technology":"crystalOscillator","frequency":10000000.0,"frequencyStability":2.4999999999999998e-05,"outputType":"cmos","supply":{"minimumSupplyVoltage":3.3,"maximumSupplyVoltage":3.3,"currentConsumption":0.02},"enableFunction":"standby"},"provenance":[{"source":"manufacturerParametric","sourceName":"Abracon parametric search","sourceUrl":"https://abracon.com/datasheets/ILSI/ISM95.pdf","retrievedDate":"2026-07-03","fields":["electrical.technology","electrical.frequency","electrical.frequencyStability","electrical.outputType","electrical.supply","electrical.enableFunction"],"verification":"seriesConfirmed","verificationDate":"2026-10-09"}],"thermal":{"operatingTemperature":{"minimum":-40.0,"maximum":85.0}}},"datasheetUrl":"https://abracon.com/datasheets/ILSI/ISM95.pdf"}}}})KREC";
// timing_devices.ndjson line 10528: EB13E2C2H-48.000M TR
const char* kEB13E2C2H_48_000M_TR = R"KREC({"timeBase":{"oscillator":{"manufacturerInfo":{"name":"Ecliptek","reference":"EB13E2C2H-48.000M TR","status":"production","series":"EB13E2","datasheetInfo":{"part":{"partNumber":"EB13E2C2H-48.000M TR","series":"EB13E2","package":"5032 4-SMD","case":"5032 4-SMD"},"electrical":{"technology":"crystalOscillator","frequency":48000000.0,"frequencyStability":9.999999999999999e-05,"outputType":"cmos","supply":{"minimumSupplyVoltage":3.3,"maximumSupplyVoltage":3.3,"currentConsumption":0.006},"enableFunction":"standby"},"provenance":[{"source":"manufacturerParametric","sourceName":"Abracon parametric search","sourceUrl":"https://abracon.com/datasheets/Ecliptek/EB13E2.pdf","retrievedDate":"2026-07-03","fields":["electrical.technology","electrical.frequency","electrical.frequencyStability","electrical.outputType","electrical.supply","electrical.enableFunction"],"verification":"seriesConfirmed","verificationDate":"2026-10-09"}],"thermal":{"operatingTemperature":{"minimum":-20.0,"maximum":70.0}}},"datasheetUrl":"https://abracon.com/datasheets/Ecliptek/EB13E2.pdf"}}}})KREC";
// timing_devices.ndjson line 8692: ASFLMB-48.000MHZ-LC-T
const char* kASFLMB_48_000MHZ_LC_T = R"KREC({"timeBase":{"oscillator":{"manufacturerInfo":{"name":"Abracon","reference":"ASFLMB-48.000MHZ-LC-T","status":"production","series":"ASFLMB","datasheetInfo":{"part":{"partNumber":"ASFLMB-48.000MHZ-LC-T","series":"ASFLMB","package":"5032 4-SMD","case":"5032 4-SMD"},"electrical":{"technology":"mems","frequency":48000000.0,"frequencyStability":4.9999999999999996e-05,"outputType":"cmos","supply":{"minimumSupplyVoltage":1.8,"maximumSupplyVoltage":3.3,"currentConsumption":0.015},"enableFunction":"standby"},"provenance":[{"source":"manufacturerParametric","sourceName":"Abracon parametric search","sourceUrl":"https://abracon.com/Oscillators/ASFLMB.pdf","retrievedDate":"2026-07-03","fields":["electrical.technology","electrical.frequency","electrical.frequencyStability","electrical.outputType","electrical.supply","electrical.enableFunction"],"verification":"seriesConfirmed","verificationDate":"2026-10-09"}],"thermal":{"operatingTemperature":{"minimum":-40.0,"maximum":85.0}}},"datasheetUrl":"https://abracon.com/Oscillators/ASFLMB.pdf"}}}})KREC";
// timing_devices.ndjson line 10952: MSH302548AH-50.000MHZ-T3
const char* kMSH302548AH_50_000MHZ_T3 = R"KREC({"timeBase":{"oscillator":{"manufacturerInfo":{"name":"MMD","reference":"MSH302548AH-50.000MHZ-T3","status":"production","series":"MSH","datasheetInfo":{"part":{"partNumber":"MSH302548AH-50.000MHZ-T3","series":"MSH","package":"3225 4-SMD","case":"3225 4-SMD"},"electrical":{"technology":"crystalOscillator","frequency":50000000.0,"frequencyStability":2.4999999999999998e-05,"outputType":"cmos","supply":{"minimumSupplyVoltage":3.3,"maximumSupplyVoltage":3.3,"currentConsumption":0.035},"enableFunction":"outputEnable"},"provenance":[{"source":"manufacturerParametric","sourceName":"Abracon parametric search","sourceUrl":"https://abracon.com/datasheets/MMD/MSH.pdf","retrievedDate":"2026-07-03","fields":["electrical.technology","electrical.frequency","electrical.frequencyStability","electrical.outputType","electrical.supply","electrical.enableFunction"],"verification":"seriesConfirmed","verificationDate":"2026-10-09"}],"thermal":{"operatingTemperature":{"minimum":-40.0,"maximum":85.0}}},"datasheetUrl":"https://abracon.com/datasheets/MMD/MSH.pdf"}}}})KREC";
// timing_devices.ndjson line 9592: ATX-H11-E-50.000MHZ-F25
const char* kATX_H11_E_50_000MHZ_F25 = R"KREC({"timeBase":{"oscillator":{"manufacturerInfo":{"name":"Abracon","reference":"ATX-H11-E-50.000MHZ-F25","status":"production","series":"ATX-H11","datasheetInfo":{"part":{"partNumber":"ATX-H11-E-50.000MHZ-F25","series":"ATX-H11","package":"3225 4-SMD","case":"3225 4-SMD"},"electrical":{"technology":"tcxo","frequency":50000000.0,"frequencyStability":2.4999999999999998e-06,"outputType":"cmos","supply":{"minimumSupplyVoltage":3.3,"maximumSupplyVoltage":3.3,"currentConsumption":0.007},"enableFunction":"outputEnable"},"provenance":[{"source":"manufacturerParametric","sourceName":"Abracon parametric search","sourceUrl":"https://abracon.com/datasheets/ATX-H11.pdf","retrievedDate":"2026-07-03","fields":["electrical.technology","electrical.frequency","electrical.frequencyStability","electrical.outputType","electrical.supply","electrical.enableFunction"],"verification":"seriesConfirmed","verificationDate":"2026-10-09"}],"thermal":{"operatingTemperature":{"minimum":-40.0,"maximum":85.0}}},"datasheetUrl":"https://abracon.com/datasheets/ATX-H11.pdf"}}}})KREC";
// timing_devices.ndjson line 6081: ABLNO-V-155.520MHZ-T2
const char* kABLNO_V_155_520MHZ_T2 = R"KREC({"timeBase":{"oscillator":{"manufacturerInfo":{"name":"Abracon","reference":"ABLNO-V-155.520MHZ-T2","status":"production","series":"ABLNO-V","datasheetInfo":{"part":{"partNumber":"ABLNO-V-155.520MHZ-T2","series":"ABLNO-V","package":"14x9 4-SMD","case":"14x9 4-SMD"},"electrical":{"technology":"vcxo","frequency":155520000.0,"frequencyStability":1.8e-05,"outputType":"cmos","supply":{"minimumSupplyVoltage":3.3,"maximumSupplyVoltage":3.3,"currentConsumption":0.04},"pullRange":9.999999999999999e-06,"rmsPhaseJitter":1e-13,"enableFunction":"none"},"provenance":[{"source":"manufacturerParametric","sourceName":"Abracon parametric search","sourceUrl":"https://abracon.com/Precisiontiming/ABLNO.pdf","retrievedDate":"2026-07-03","fields":["electrical.technology","electrical.frequency","electrical.frequencyStability","electrical.outputType","electrical.supply","electrical.pullRange","electrical.rmsPhaseJitter","electrical.enableFunction"],"verification":"citationOnly","verificationDate":"2026-10-09"}],"thermal":{"operatingTemperature":{"minimum":-40.0,"maximum":85.0}}},"datasheetUrl":"https://abracon.com/Precisiontiming/ABLNO.pdf"}}}})KREC";
// timing_devices.ndjson line 6046: ABLNO-155.520MHZ
const char* kABLNO_155_520MHZ = R"KREC({"timeBase":{"oscillator":{"manufacturerInfo":{"name":"Abracon","reference":"ABLNO-155.520MHZ","status":"production","series":"ABLNO","datasheetInfo":{"part":{"partNumber":"ABLNO-155.520MHZ","series":"ABLNO","package":"14x9 4-SMD","case":"14x9 4-SMD"},"electrical":{"technology":"crystalOscillator","frequency":155520000.0,"frequencyStability":1.8e-05,"outputType":"cmos","supply":{"minimumSupplyVoltage":3.3,"maximumSupplyVoltage":3.3,"currentConsumption":0.04},"enableFunction":"none"},"provenance":[{"source":"manufacturerParametric","sourceName":"Abracon parametric search","sourceUrl":"https://abracon.com/Precisiontiming/ABLNO.pdf","retrievedDate":"2026-07-03","fields":["electrical.technology","electrical.frequency","electrical.frequencyStability","electrical.outputType","electrical.supply","electrical.enableFunction"],"verification":"seriesConfirmed","verificationDate":"2026-10-09"}],"thermal":{"operatingTemperature":{"minimum":-40.0,"maximum":85.0}}},"datasheetUrl":"https://abracon.com/Precisiontiming/ABLNO.pdf"}}}})KREC";
// timing_devices.ndjson line 9054: AST3TQ-10.000MHZ-1-T2
const char* kAST3TQ_10_000MHZ_1_T2 = R"KREC({"timeBase":{"oscillator":{"manufacturerInfo":{"name":"Abracon","reference":"AST3TQ-10.000MHZ-1-T2","status":"production","series":"AST3TQ","datasheetInfo":{"part":{"partNumber":"AST3TQ-10.000MHZ-1-T2","series":"AST3TQ","package":"7050 4-SMD","case":"7050 4-SMD"},"electrical":{"technology":"tcxo","frequency":10000000.0,"frequencyStability":1e-07,"outputType":"cmos","supply":{"minimumSupplyVoltage":3.3,"maximumSupplyVoltage":3.3,"currentConsumption":0.0085},"rmsPhaseJitter":1.3000000000000001e-12,"enableFunction":"none"},"provenance":[{"source":"manufacturerParametric","sourceName":"Abracon parametric search","sourceUrl":"https://abracon.com/Oscillators/AST3TQ.pdf","retrievedDate":"2026-07-03","fields":["electrical.technology","electrical.frequency","electrical.frequencyStability","electrical.outputType","electrical.supply","electrical.rmsPhaseJitter","electrical.enableFunction"],"verification":"seriesConfirmed","verificationDate":"2026-10-09"}],"thermal":{"operatingTemperature":{"minimum":-40.0,"maximum":85.0}}},"datasheetUrl":"https://abracon.com/Oscillators/AST3TQ.pdf"}}}})KREC";
// timing_devices.ndjson line 587: NE555
const char* kNE555 = R"KREC({"timeBase":{"timer":{"manufacturerInfo":{"name":"Texas Instruments","reference":"NE555","status":"production","datasheetUrl":"https://www.ti.com/lit/gpn/NE555","datasheetInfo":{"part":{"partNumber":"NE555","package":"PDIP"},"electrical":{"technology":"bipolar555","numberOfChannels":1,"supply":{"minimumSupplyVoltage":4.5,"maximumSupplyVoltage":16.0,"currentConsumption":0.002}},"provenance":[{"source":"manufacturerParametric","sourceName":"Texas Instruments parametric search","sourceUrl":"https://www.ti.com/product/NE555","retrievedDate":"2026-07-03","fields":["part.partNumber","electrical.technology","electrical.numberOfChannels","electrical.supply"],"verification":"partNamed","verificationDate":"2026-10-09"}],"thermal":{"operatingTemperature":{"minimum":0.0,"maximum":70.0}}}}}}})KREC";
// timing_devices.ndjson line 588: SA555
const char* kSA555 = R"KREC({"timeBase":{"timer":{"manufacturerInfo":{"name":"Texas Instruments","reference":"SA555","status":"production","datasheetUrl":"https://www.ti.com/lit/gpn/SA555","datasheetInfo":{"part":{"partNumber":"SA555","package":"PDIP"},"electrical":{"technology":"bipolar555","numberOfChannels":1,"supply":{"minimumSupplyVoltage":4.5,"maximumSupplyVoltage":16.0,"currentConsumption":0.002}},"provenance":[{"source":"manufacturerParametric","sourceName":"Texas Instruments parametric search","sourceUrl":"https://www.ti.com/product/SA555","retrievedDate":"2026-07-03","fields":["part.partNumber","electrical.technology","electrical.numberOfChannels","electrical.supply"],"verification":"partNamed","verificationDate":"2026-10-09"}],"thermal":{"operatingTemperature":{"minimum":-40.0,"maximum":85.0}}}}}}})KREC";
// timing_devices.ndjson line 590: TLC555
const char* kTLC555 = R"KREC({"timeBase":{"timer":{"manufacturerInfo":{"name":"Texas Instruments","reference":"TLC555","status":"production","datasheetUrl":"https://www.ti.com/lit/gpn/TLC555","datasheetInfo":{"part":{"partNumber":"TLC555","package":"PDIP"},"electrical":{"technology":"cmos555","numberOfChannels":1,"supply":{"minimumSupplyVoltage":2.0,"maximumSupplyVoltage":15.0,"currentConsumption":0.00018}},"provenance":[{"source":"manufacturerParametric","sourceName":"Texas Instruments parametric search","sourceUrl":"https://www.ti.com/product/TLC555","retrievedDate":"2026-07-03","fields":["part.partNumber","electrical.technology","electrical.numberOfChannels","electrical.supply"],"verification":"partNamed","verificationDate":"2026-10-09"}],"thermal":{"operatingTemperature":{"minimum":-40.0,"maximum":125.0}}}}}}})KREC";

json spec(const char* line) {
    auto r = kelvin::extract_timing(json::parse(line));
    REQUIRE(r.has_value());
    auto nz = [](double v) { return std::isnan(v) || !(v > 0) ? json(nullptr) : json(v); };
    auto raw = [](double v) { return std::isnan(v) ? json(nullptr) : json(v); };
    auto str_or_null = [](const std::string& s) { return s.empty() ? json(nullptr) : json(s); };
    const json cl = nz(r->load_capacitance);
    return json{{"mpn", r->mpn},
                {"length_m", nz(r->length_m)}, {"width_m", nz(r->width_m)},
                {"height_m", nz(r->height_m)}, {"case_code", r->case_code}, {"mount", r->mount},
                {"lead_pitch_m", nz(r->lead_pitch_m)}, {"is_production", r->is_production},
                {"provenance_retracted", r->provenance_retracted},
                {"box_axes_ambiguous", r->box_axes_ambiguous},
                {"subtype", r->device_type}, {"technology", r->technology},
                {"device_type", r->device_type}, {"frequency", nz(r->frequency)},
                {"load_capacitance", cl},
                {"load_capacitance_pF", cl.is_null() ? json(nullptr) : json(r->load_capacitance * 1e12)},
                {"built_in_capacitance", nz(r->built_in_capacitance)}, {"esr", nz(r->esr)},
                {"output_type", str_or_null(r->output_type)}, {"mode", str_or_null(r->mode)},
                {"enable_function", str_or_null(r->enable_function)},
                {"supply_min_V", nz(r->vsupply_min)}, {"supply_max_V", nz(r->vsupply_max)},
                {"tolerance_ppm", std::isnan(r->frequency_tolerance) ? json(nullptr)
                                                                    : json(r->frequency_tolerance * 1e6)},
                {"stability_ppm", std::isnan(r->frequency_stability) ? json(nullptr)
                                                                    : json(r->frequency_stability * 1e6)},
                {"temp_min_C", raw(r->temp_min_c)}, {"temp_max_C", raw(r->temp_max_c)}};
}
json one(const char* orig, const char* cand) {
    auto r = kelvin::crossref::cross_reference("timeBase", spec(orig), json::array({spec(cand)}));
    REQUIRE(r["candidates"].size() == 1);
    return r["candidates"][0];
}
}  // namespace timing_tech

TEST_CASE("timing technology: MEMS and quartz XO cross at major_review, both directions",
          "[qarlos][crossref][timing][timing-tech]") {
    using namespace timing_tech;
    // ASDM1-10.000MHZ-LC-T (Abracon, MEMS, 10 MHz, CMOS, standby, 2520) -> ISM95-3251AH-10.000MHZ
    // (ILSI, quartz XO, 10 MHz, CMOS, standby, 2520). It was rejected as "an identity parameter
    // differs"; every other axis of the pair passes.
    const json c = one(kASDM1_10_000MHZ_LC_T, kISM95_3251AH_10_000MHZ);
    CHECK(c["status"] != "no_substitute");
    CHECK(verdict_of(c, "technology") == "warn");
    CHECK(c["grade"] == "major_review");
    CHECK(has_note(c, "technology differs: the original is a MEMS oscillator, the substitute a "
                      "quartz crystal oscillator (XO)"));
    CHECK(has_note(c, "jitter / phase noise, shock and vibration tolerance, aging and start-up "
                      "behaviour differ"));
    for (const auto& p : c["params"])
        if (p["name"] != "technology") CHECK(p["verdict"] != "fail");
    // The reverse direction: EB13E2C2H-48.000M TR (Ecliptek, quartz XO, 48 MHz, 5032) ->
    // ASFLMB-48.000MHZ-LC-T (Abracon, MEMS, 48 MHz, 5032).
    const json r = one(kEB13E2C2H_48_000M_TR, kASFLMB_48_000MHZ_LC_T);
    CHECK(r["status"] != "no_substitute");
    CHECK(verdict_of(r, "technology") == "warn");
    CHECK(r["grade"] == "major_review");
    CHECK(has_note(r, "technology differs: the original is a quartz crystal oscillator (XO), the "
                      "substitute a MEMS oscillator"));
}

TEST_CASE("timing technology: a TCXO is a minor_review upgrade for a quartz XO, one way only",
          "[qarlos][crossref][timing][timing-tech]") {
    using namespace timing_tech;
    // MSH302548AH-50.000MHZ-T3 (MMD, quartz XO, 50 MHz, +/-25 ppm, 3225) ->
    // ATX-H11-E-50.000MHZ-F25 (Abracon, TCXO, 50 MHz, +/-2.5 ppm, 3225).
    const json c = one(kMSH302548AH_50_000MHZ_T3, kATX_H11_E_50_000MHZ_F25);
    CHECK(c["status"] != "no_substitute");
    CHECK(verdict_of(c, "technology") == "warn");
    CHECK(c["grade"] == "minor_review");
    CHECK(c["direction"] == "upgrade");
    CHECK(verdict_of(c, "stability_ppm") == "pass");  // stability keeps its own comparison
    CHECK(has_note(c, "technology differs: a TCXO offered for a quartz crystal oscillator (XO)"));
    CHECK(has_note(c, "higher supply current and cost"));
    // The other way round stays excluded: the XO for the TCXO original.
    const json x = one(kATX_H11_E_50_000MHZ_F25, kMSH302548AH_50_000MHZ_T3);
    CHECK(x["status"] == "no_substitute");
    CHECK(verdict_of(x, "technology") == "fail");
    // A TCXO for a MEMS original is not covered by either decision: strict identity.
    // ASDM1-10.000MHZ-LC-T (MEMS, 10 MHz) -> AST3TQ-10.000MHZ-1-T2 (Abracon, TCXO, 10 MHz).
    const json m = one(kASDM1_10_000MHZ_LC_T, kAST3TQ_10_000MHZ_1_T2);
    CHECK(m["status"] == "no_substitute");
    CHECK(verdict_of(m, "technology") == "fail");
}

TEST_CASE("timing technology: every other pair stays a strict identity",
          "[qarlos][crossref][timing][timing-tech]") {
    using namespace timing_tech;
    // ABLNO-V-155.520MHZ-T2 (VCXO) -> ABLNO-155.520MHZ (fixed quartz XO), both Abracon,
    // 155.52 MHz, CMOS, 14x9: a VCXO's control voltage pin has no counterpart on a fixed XO.
    const json v = one(kABLNO_V_155_520MHZ_T2, kABLNO_155_520MHZ);
    CHECK(v["status"] == "no_substitute");
    CHECK(verdict_of(v, "technology") == "fail");
    CHECK_FALSE(has_note(v, "technology differs"));
    const json v2 = one(kABLNO_155_520MHZ, kABLNO_V_155_520MHZ_T2);
    CHECK(v2["status"] == "no_substitute");
}

TEST_CASE("timing technology: 555 timers are unchanged", "[qarlos][crossref][timing][timing-tech]") {
    using namespace timing_tech;
    // NE555 -> SA555 (both bipolar555, Texas Instruments): same technology, no frequency on
    // either record, compared as before; NE555 -> TLC555 (cmos555) stays rejected.
    const json n = spec(kNE555);
    REQUIRE(n["frequency"].is_null());
    REQUIRE(n["device_type"] == "timer");
    const json s = one(kNE555, kSA555);
    CHECK(s["status"] != "no_substitute");
    CHECK(verdict_of(s, "technology") == "pass");
    CHECK_FALSE(has_note(s, "technology differs"));
    const json c = one(kNE555, kTLC555);
    CHECK(c["status"] == "no_substitute");
    CHECK(verdict_of(c, "technology") == "fail");
}

// ── Qarlos r14-r16, group G (diode, MOSFET package aliases and gate rating) ──────────────
// r14 seed 25707017: onsemi's 'SMA-2' (1SMA5940BT3G) against 'DO-214AC (SMA)' (1SMA5940H) and
// r15 seed 26544849: onsemi's 'SOT-23 (TO-236) 2.90x1.30x1.00, 1.90P' (SZMMBZ5228BLT1G) against
// 'SOT-23' (BZX84-A3V9) were graded different_case. The onsemi datasheets of the SMA-2 / SMB-2
// parts print "SMA CASE 403D" / "SMB CASE 403A".
TEST_CASE("qarlos r14-r16: onsemi SMA-2 / SMB-2 and the body-suffixed SOT-23 name their outline",
          "[qarlos][crossref][dims][g-r14r16]") {
    using kelvin::crossref::canonical_diode_case;
    CHECK(canonical_diode_case("SMA-2") == canonical_diode_case("DO-214AC (SMA)"));
    CHECK(canonical_diode_case("SMB-2") == canonical_diode_case("SMB (DO-214AA)"));
    CHECK(canonical_diode_case("SOT-23 (TO-236) 2.90x1.30x1.00, 1.90P") ==
          canonical_diode_case("SOT-23"));
    CHECK(canonical_diode_case("SOT23") == canonical_diode_case("TO-236AB"));
    // Not folded in: onsemi's SMC-2 (no datasheet read), a suffix whose body is not the
    // registered outline's, a name outside every group, and the flat-lead SMA.
    CHECK(canonical_diode_case("SMC-2") != canonical_diode_case("SMC"));
    CHECK(canonical_diode_case("SOT-23 (TO-236) 2.90x1.60x1.00, 1.90P") !=
          canonical_diode_case("SOT-23"));
    CHECK(canonical_diode_case("SC-59-3 2.90x1.50x1.15, 1.90P") != canonical_diode_case("SOT-23"));
    CHECK(canonical_diode_case("SMA-FL") != canonical_diode_case("SMA"));

    // The two reported pairs, as their shard rows state them.
    json sma = zener("1SMA5940H", 43.0, 40.85, 45.15, 1.5);
    sma["case_code"] = "DO-214AC (SMA)";
    json sma2 = zener("1SMA5940BT3G", 43.0, std::nullopt, std::nullopt, 1.5);
    sma2["case_code"] = "SMA-2";
    const json a =
        kelvin::crossref::cross_reference("diode", sma, json::array({sma2}))["candidates"][0];
    CHECK(a["footprint"] == "fits");
    CHECK_FALSE(has_note(a, "different package/case"));

    json sot = zener("SZMMBZ5228BLT1G", 3.9, std::nullopt, std::nullopt, 0.225);
    sot["case_code"] = "SOT-23 (TO-236) 2.90x1.30x1.00, 1.90P";
    json bzx = zener("BZX84-A3V9", 3.9, 3.86, 3.94, 0.25);
    bzx["case_code"] = "SOT-23";
    const json b =
        kelvin::crossref::cross_reference("diode", sot, json::array({bzx}))["candidates"][0];
    CHECK(b["footprint"] == "fits");
    CHECK_FALSE(has_note(b, "different package/case"));
}

// Bench 20680025: BZV55-C5V6 (5.6 V, 5.2-6.0 V) -> 1N5233B (6.0 V, no band) was graded like the
// 5.6 V parts with its 7 % offset unmentioned. The note names it; the verdict is unchanged.
TEST_CASE("qarlos r14-r16: a zener inside the window but off the nominal is noted",
          "[qarlos][crossref][g-r14r16]") {
    json orig = zener("BZV55-C5V6", 5.6, 5.2, 6.0, 0.5);
    auto r = kelvin::crossref::cross_reference(
        "diode", orig,
        json::array({zener("1N5233B", 6.0, std::nullopt, std::nullopt, 0.5),
                     zener("1N5232B", 5.6, std::nullopt, std::nullopt, 0.5)}));
    const json off = find_mpn(r, "1N5233B");
    CHECK(verdict_of(off, "zener_voltage") == "unverified");
    CHECK(has_note(off, "the substitute's Vz 6 V is +7.14 % from the original's 5.6 V"));
    const json same = find_mpn(r, "1N5232B");
    CHECK_FALSE(has_note(same, "the substitute's Vz"));
    CHECK(off["grade"] == same["grade"]);
}

// Bench 20276947: SiSS60DN (Vgs max 16 V) was 'upgrade' for IRFR8314 (20 V) beside a vgs_max
// WARN; bench 20575296: IPD60R280P7S (no Vgs max on record) had no verdict for TK290P60Y's 30 V.
TEST_CASE("qarlos r14-r16: MOSFET Vgs(max) lower is a regression, unstated is UNVERIFIED",
          "[qarlos][crossref][g-r14r16]") {
    json irfr = {{"mpn", "IRFR8314"}, {"vds", 30.0}, {"id", 179.0}, {"rds_on", 0.0022},
                 {"rds_on_vgs", 10.0}, {"qg", 3.6e-8}, {"vgs_max", 20.0},
                 {"channel", "nChannel"}, {"technology", "Si"}};
    json siss = {{"mpn", "SiSS60DN"}, {"vds", 30.0}, {"id", 181.8}, {"rds_on", 0.00131},
                 {"qg", 2.59e-8}, {"coss", 1.785e-9}, {"vgs_threshold_max", 2.5},
                 {"vgs_max", 16.0}, {"channel", "nChannel"}, {"technology", "Si"}};
    const json c =
        kelvin::crossref::cross_reference("mosfet", irfr, json::array({siss}))["candidates"][0];
    CHECK(verdict_of(c, "vgs_max") == "warn");
    CHECK(c["direction"] != "upgrade");
    CHECK(has_note(c, "(16 V vs 20 V)"));

    json tk = {{"mpn", "TK290P60Y"}, {"vds", 600.0}, {"id", 11.5}, {"rds_on", 0.29},
               {"rds_on_vgs", 10.0}, {"qg", 2.5e-8}, {"vgs_threshold_max", 4.0},
               {"vgs_max", 30.0}, {"channel", "nChannel"}, {"technology", "Si"},
               {"case_code", "DPAK"}};
    json ipd = {{"mpn", "IPD60R280P7S"}, {"vds", 600.0}, {"id", 12.0}, {"rds_on", 0.28},
                {"rds_on_vgs", 10.0}, {"qg", 1.8e-8}, {"vgs_threshold_max", 4.0},
                {"channel", "nChannel"}, {"technology", "Si"}, {"case_code", "DPAK"}};
    const json u =
        kelvin::crossref::cross_reference("mosfet", tk, json::array({ipd}))["candidates"][0];
    CHECK(verdict_of(u, "vgs_max") == "unverified");
    CHECK(has_note(u, "maximum gate-source voltage is 30 V; the substitute's record states none"));
    // A secondary axis stated on one side: noted, never capped (owner, 2026-11-09).
    CHECK(u["grade"] == "drop_in");
}


// ── Qarlos r14-r16 group H (2026-11-13) ──────────────────────────────────────
// Records are the REAL lines of tools/qarlos/bench/data-9cb6cf8 (TAS 9cb6cf8), distributorsInfo
// dropped, run through the real extractor and the spec block web/src/crossref.js builds.
namespace isat_both_unstated {
// magnetics.ndjson line 81403: IHSM5832ER151L
const char* kIHSM5832ER151L = R"KREC({"magnetic":{"manufacturerInfo":{"name":"Vishay","reference":"IHSM5832ER151L","status":"production","family":"IHSM-5832","datasheetUrl":"https://www.vishay.com/docs/34020/ihsm-5832.pdf","datasheetInfo":{"part":{"partNumber":"IHSM5832ER151L","family":"IHSM-5832","description":"Power Inductors"},"electrical":[{"subtype":"inductor","inductance":{"nominal":0.00015},"dcResistance":{"maximum":0.609},"ratedCurrents":[0.82]}],"thermal":{"operatingTemperature":{"minimum":-55.0,"maximum":125.0}},"mechanical":{"mounting":"smt"},"provenance":[{"source":"manufacturerDatasheet","sourceName":"Vishay datasheet","sourceUrl":"https://www.vishay.com/docs/34020/ihsm-5832.pdf","retrievedDate":"2026-09-11","verification":"valuesReadFromSource","verificationDate":"2026-09-11","fields":["manufacturerInfo.reference","datasheetInfo.part.partNumber","datasheetInfo.electrical.0.inductance","datasheetInfo.electrical.0.dcResistance","datasheetInfo.electrical.0.ratedCurrents"]}]}}}})KREC";
// magnetics.ndjson line 21860: B82477G2154M000
const char* kB82477G2154M000 = R"KREC({"magnetic":{"manufacturerInfo":{"name":"TDK","reference":"B82477G2154M000","status":"production","family":"B82477G2","datasheetUrl":"https://www.tdk.com/en/search/compass/part_no/B82477G2154M000","datasheetInfo":{"part":{"description":"B82477G2 B82477G2154M000 150.0μH","caseCode":"","windingStyle":"wireWound"},"electrical":[{"subtype":"inductor","inductance":{"nominal":0.00015,"minimum":0.00011999999999999999,"maximum":0.00017999999999999998},"dcResistance":{"maximum":0.23},"ratedCurrents":[1.0]}],"mechanical":{"weight":{"nominal":0.0036},"height":{"nominal":0.006},"length":{"nominal":0.0123},"width":{"nominal":0.0123}},"thermal":{"operatingTemperature":{"minimum":-55.0,"maximum":125.0}},"provenance":[{"source":"manufacturerDatabase","sourceName":"TDK product database","sourceUrl":"https://www.tdk.com/en/search/compass/part_no/B82477G2154M000","retrievedDate":"2026-06-22"},{"source":"manufacturerDatabase","sourceName":"TDK product database","retrievedDate":"2026-07-21","fields":["mechanical.height","mechanical.length","mechanical.width"]},{"source":"manufacturerDatabase","sourceName":"TDK Meister database","retrievedDate":"2026-09-24","verification":"valuesReadFromSource","verificationDate":"2026-09-24","fields":["part.windingStyle"],"verificationMethod":"TDK Meister classifies this part's product class as wire-wound construction."},{"source":"manufacturerDatabase","sourceName":"TDK product center (part search listing)","sourceUrl":"https://product.tdk.com/en/search/inductor/inductor/smd/info?part_no=B82477G2154M000","retrievedDate":"2026-10-17","fields":["electrical.inductance","electrical.dcResistance","electrical.ratedCurrents","thermal.operatingTemperature","mechanical.length","mechanical.width"],"verification":"valuesReadFromSource","verificationDate":"2026-10-17"}]}}}})KREC";
// magnetics.ndjson line 31936: LSQCA201616T680M
const char* kLSQCA201616T680M = R"KREC({"magnetic":{"manufacturerInfo":{"name":"Taiyo Yuden","reference":"LSQCA201616T680M","status":"production","family":"Wire-wound Ferrite Inductors LSQB/LSQC/LSQE series for General Purpose","datasheetUrl":"https://ds.yuden.co.jp/TYCOMPAS/or/detail?pn=LSQCA201616T680M","datasheetInfo":{"part":{"description":"Wire-wound Ferrite Inductors LSQB/LSQC/LSQE series for General Purpose LSQCA201616T680M 68.0μH","caseCode":"0806","material":"Ferrite","windingStyle":"wireWound","shielded":false},"electrical":[{"subtype":"inductor","inductance":{"nominal":6.8e-05,"minimum":5.44e-05,"maximum":8.159999999999999e-05},"dcResistance":{"maximum":9.1,"nominal":7.0},"ratedCurrents":[0.095]}],"mechanical":{"length":{"nominal":0.002},"width":{"nominal":0.0016},"height":{"nominal":0.0018}},"thermal":{"operatingTemperature":{"minimum":-40.0,"maximum":105.0}},"provenance":[{"source":"manufacturerParametric","sourceName":"Taiyo Yuden TY-COMPAS","sourceUrl":"https://ds.yuden.co.jp/TYCOMPAS/or/detail?pn=LSQCA201616T680M","retrievedDate":"2026-07-31","verification":"partNamed","verificationDate":"2026-10-09"}]}}}})KREC";
// magnetics.ndjson line 23081: LQH2MCN680K02
const char* kLQH2MCN680K02 = R"KREC({"magnetic":{"manufacturerInfo":{"name":"Murata","reference":"LQH2MCN680K02","status":"production","datasheetUrl":"https://pim.murata.com/en-global/pim/details/?partNum=LQH2MCN680K02","family":"LQH2MCN","description":"General","datasheetInfo":{"part":{"partNumber":"LQH2MCN680K02","family":"LQH2MCN","description":"General"},"electrical":[{"subtype":"inductor","inductance":{"nominal":6.8e-05},"dcResistance":{"maximum":6.6,"nominal":6.6},"ratedCurrents":[0.1],"inductancePoints":[{"current":0.0,"inductance":6.44452e-05,"temperature":-40.0},{"current":0.01,"inductance":6.58003e-05,"temperature":-40.0},{"current":0.02,"inductance":6.61596e-05,"temperature":-40.0},{"current":0.03,"inductance":6.61472e-05,"temperature":-40.0},{"current":0.04,"inductance":6.60037e-05,"temperature":-40.0},{"current":0.05,"inductance":6.5789e-05,"temperature":-40.0},{"current":0.06,"inductance":6.54503e-05,"temperature":-40.0},{"current":0.07,"inductance":6.50437e-05,"temperature":-40.0},{"current":0.08,"inductance":6.45555e-05,"temperature":-40.0},{"current":0.09,"inductance":6.40031e-05,"temperature":-40.0},{"current":0.1,"inductance":6.33262e-05,"temperature":-40.0},{"current":0.0,"inductance":6.78896e-05,"temperature":20.0},{"current":0.01,"inductance":6.79949e-05,"temperature":20.0},{"current":0.02,"inductance":6.80107e-05,"temperature":20.0},{"current":0.03,"inductance":6.79553e-05,"temperature":20.0},{"current":0.04,"inductance":6.78525e-05,"temperature":20.0},{"current":0.05,"inductance":6.77309e-05,"temperature":20.0},{"current":0.06,"inductance":6.74924e-05,"temperature":20.0},{"current":0.07,"inductance":6.71718e-05,"temperature":20.0},{"current":0.08,"inductance":6.66966e-05,"temperature":20.0},{"current":0.09,"inductance":6.6031e-05,"temperature":20.0},{"current":0.1,"inductance":6.49599e-05,"temperature":20.0},{"current":0.0,"inductance":7.16968e-05,"temperature":85.0},{"current":0.01,"inductance":7.17586e-05,"temperature":85.0},{"current":0.02,"inductance":7.17701e-05,"temperature":85.0},{"current":0.03,"inductance":7.17209e-05,"temperature":85.0},{"current":0.04,"inductance":7.14592e-05,"temperature":85.0},{"current":0.05,"inductance":7.10108e-05,"temperature":85.0},{"current":0.06,"inductance":7.02835e-05,"temperature":85.0},{"current":0.07,"inductance":6.92204e-05,"temperature":85.0},{"current":0.08,"inductance":6.73426e-05,"temperature":85.0},{"current":0.09,"inductance":6.43549e-05,"temperature":85.0},{"current":0.1,"inductance":5.81183e-05,"temperature":85.0}]}],"thermal":{"operatingTemperature":{"minimum":-40,"maximum":125}},"mechanical":{"length":{"nominal":0.002},"width":{"nominal":0.0016},"height":{"nominal":0.00095}},"provenance":[{"source":"manufacturerParametric","sourceName":"Murata product catalogue","sourceUrl":"https://pimapi.murata.com/public/api/pim/v1/products/search/cross-categories?partNum=LQH2MCN680K02&languageRegion=en-global","retrievedDate":"2026-07-31"},{"source":"manufacturerParametric","sourceName":"Murata product catalogue (PIM)","sourceUrl":"https://www.murata.com/en-us/products/productdetail?partno=LQH2MCN680K02","retrievedDate":"2026-10-09","fields":["electrical.inductance","electrical.dcResistance","electrical.ratedCurrents","mechanical.length","mechanical.width","mechanical.height","thermal.operatingTemperature"],"verification":"partNamed","verificationDate":"2026-10-31"}]}}}})KREC";
json spec(const char* line) {
    auto r = kelvin::extract_magnetic(real_mag(line));
    REQUIRE(r.has_value());
    return mag_spec_xref(*r);
}
json one(const json& orig, const json& cand) {
    kelvin::crossref::Options opt;
    auto r = kelvin::crossref::cross_reference("magnetic", orig, json::array({cand}), opt);
    REQUIRE(r["candidates"].size() == 1);
    return r["candidates"][0];
}
int rows_named(const json& c, const std::string& name) {
    int n = 0;
    for (const auto& p : c.at("params"))
        if (p.value("name", "") == name) ++n;
    return n;
}
}  // namespace isat_both_unstated

// Qarlos bench 20269028 (IHSM5832ER151L -> B82477G2154M000) and r16 26859036
// (LSQCA201616T680M -> LQH2MCN680K02): neither record states a saturation current, and the
// verdict table carried no Isat row at all, so the candidate read drop_in / 'upgrade' with
// nothing showing that Isat was never compared. Both-missing is not capped (owner 2026-11-12),
// but the axis is reported UNVERIFIED.
TEST_CASE("qarlos r14-r16 H2: Isat stated on neither record is an UNVERIFIED row, uncapped",
          "[qarlos][crossref][isat][isat-both-unstated]") {
    using namespace isat_both_unstated;
    struct Pair { const char* o; const char* s; const char* grade; };
    for (const Pair& p : {Pair{kIHSM5832ER151L, kB82477G2154M000, "minor_review"},
                          Pair{kLSQCA201616T680M, kLQH2MCN680K02, "drop_in"}}) {
        const json o = spec(p.o), s = spec(p.s);
        // The premise: no Isat in any form on either side.
        REQUIRE(o["saturation_current"].is_null());
        REQUIRE(s["saturation_current"].is_null());
        REQUIRE(o["saturation_points"].is_null());
        REQUIRE(s["saturation_points"].is_null());
        const json c = one(o, s);
        CHECK(verdict_of(c, "saturation_current") == "unverified");
        CHECK(rows_named(c, "saturation_current") == 1);
        // Not capped: the grade is what the other axes give (B82477G2154M000's footprint is
        // unverified against an original with no land, hence minor_review).
        CHECK(c["grade"] == p.grade);
    }
    // Boundary: Isat stated on one side goes through the normal compare, one row only.
    json o = spec(kLSQCA201616T680M);
    o["saturation_current"] = 0.2;
    json s = spec(kLQH2MCN680K02);
    s["saturation_current"] = 0.25;
    const json c = one(o, s);
    CHECK(verdict_of(c, "saturation_current") == "pass");
    CHECK(rows_named(c, "saturation_current") == 1);
}

// Qarlos r15 26440120: 05FHJ-SM1-TB (JST FHJ FPC; its record states no position count) took
// the 20-position Molex 2250190200 as a clean drop_in, 'equivalent', penalty 0, positions
// 'unverified' and no note. Positions is a connector identity; stated by one record only it
// takes the one-sided identity treatment (note + cap below drop_in). Spec blocks exactly as
// web/src/crossref.js builds them from the shard rows.
TEST_CASE("qarlos r14-r16 H3: connector positions stated by the substitute only caps drop_in",
          "[qarlos][crossref][connector][positions-one-sided]") {
    const json orig = conn_spec("05FHJ-SM1-TB",
                                {{"family", "fpcFfc"}, {"pitch_mm", 0.5}, {"rated_current_A", 0.5},
                                 {"rated_voltage_V", 50.0}, {"temp_min_C", -25.0},
                                 {"temp_max_C", 85.0}, {"mounting", "smt"},
                                 {"orientation", "rightAngle"}});
    const json cand = conn_spec("2250190200",
                                {{"family", "fpcFfc"}, {"positions", 20.0}, {"pitch_mm", 0.5},
                                 {"rated_current_A", 0.5}, {"rated_voltage_V", 50.0},
                                 {"temp_min_C", -40.0}, {"temp_max_C", 85.0},
                                 {"contact_plating", "au-gold"}, {"mounting", "smt"},
                                 {"orientation", "rightAngle"}, {"mating_cycles", 20.0}});
    REQUIRE(orig["positions"].is_null());
    const json c = conn_one(orig, cand);
    CHECK(verdict_of(c, "positions") == "unverified");
    CHECK(c["grade"] == "minor_review");
    CHECK(has_note(c, "contact positions not established (original: not stated, substitute: 20)"));
    // Boundary: the same pair with the position count on both sides and equal is a drop_in.
    json o20 = orig;
    o20["positions"] = 20.0;
    const json same = conn_one(o20, cand);
    CHECK(verdict_of(same, "positions") == "pass");
    CHECK(same["grade"] == "drop_in");
    CHECK_FALSE(has_note(same, "contact positions not established"));
    // Stated on both sides and different still rejects (unchanged).
    json o5 = orig;
    o5["positions"] = 5.0;
    CHECK(conn_one(o5, cand)["status"] == "no_substitute");
}

// ── Qarlos r14-r16 owner decisions 2026-11-13, agent J (diode, timing, capacitor) ──
// Ranker specs exactly as web/src/crossref.js builds them from the shard rows of the frozen
// bench data (TAS 9cb6cf8; <file>.ndjson line N), captured with famFor(...).spec(row).
namespace dec_1113_j {
// diodes.ndjson line 8468: BZV55-C5V6
const char* kBZV55_C5V6 = R"KSPEC({"mpn":"BZV55-C5V6","length_m":null,"width_m":null,"height_m":null,"case_code":"LLDS; MiniMelf","mount":"","lead_pitch_m":null,"is_production":true,"provenance_retracted":false,"box_axes_ambiguous":false,"vrrm":5.6,"if_avg":0.25,"vf":null,"power_dissipation":0.5,"qrr":null,"trr":null,"technology":"zener","die_configuration":null,"internal_connection":null,"vz_min_V":5.2,"vz_max_V":6,"vz_tolerance_pct":7.1428571428571415,"clamping_voltage":null,"clamping_current":null,"peak_pulse_power":null,"peak_pulse_current":null,"junction_capacitance":null,"junction_capacitance_vr":null,"clamping_waveform":"","peak_pulse_power_waveform":"","peak_pulse_current_waveform":"","tj_max":null,"qualification":""})KSPEC";
// diodes.ndjson line 5958: 1N5233B
const char* k1N5233B = R"KSPEC({"mpn":"1N5233B","length_m":null,"width_m":null,"height_m":null,"case_code":"Axial Lead","mount":"","lead_pitch_m":0.01016,"is_production":true,"provenance_retracted":false,"box_axes_ambiguous":false,"vrrm":6,"if_avg":null,"vf":null,"power_dissipation":0.5,"qrr":null,"trr":null,"technology":"zener","die_configuration":null,"internal_connection":null,"vz_min_V":null,"vz_max_V":null,"vz_tolerance_pct":null,"clamping_voltage":null,"clamping_current":null,"peak_pulse_power":null,"peak_pulse_current":null,"junction_capacitance":null,"junction_capacitance_vr":null,"clamping_waveform":"","peak_pulse_power_waveform":"","peak_pulse_current_waveform":"","tj_max":null,"qualification":""})KSPEC";
// diodes.ndjson line 5953: 1N5232B
const char* k1N5232B = R"KSPEC({"mpn":"1N5232B","length_m":null,"width_m":null,"height_m":null,"case_code":"Axial Lead","mount":"","lead_pitch_m":0.01016,"is_production":true,"provenance_retracted":false,"box_axes_ambiguous":false,"vrrm":5.6,"if_avg":null,"vf":null,"power_dissipation":0.5,"qrr":null,"trr":null,"technology":"zener","die_configuration":null,"internal_connection":null,"vz_min_V":null,"vz_max_V":null,"vz_tolerance_pct":null,"clamping_voltage":null,"clamping_current":null,"peak_pulse_power":null,"peak_pulse_current":null,"junction_capacitance":null,"junction_capacitance_vr":null,"clamping_waveform":"","peak_pulse_power_waveform":"","peak_pulse_current_waveform":"","tj_max":null,"qualification":""})KSPEC";
// diodes.ndjson line 28299: SA6.5CA
const char* kSA6_5CA = R"KSPEC({"mpn":"SA6.5CA","length_m":null,"width_m":null,"height_m":null,"case_code":"DO-15","mount":"","lead_pitch_m":null,"is_production":true,"provenance_retracted":false,"box_axes_ambiguous":false,"vrrm":6.5,"if_avg":null,"vf":null,"power_dissipation":null,"qrr":null,"trr":null,"technology":"tvs","die_configuration":null,"internal_connection":null,"vz_min_V":7.22,"vz_max_V":7.98,"vz_tolerance_pct":5.000000000000004,"clamping_voltage":11.2,"clamping_current":45.5,"peak_pulse_power":500,"peak_pulse_current":45.5,"junction_capacitance":null,"junction_capacitance_vr":null,"clamping_waveform":"10/1000","peak_pulse_power_waveform":"10/1000","peak_pulse_current_waveform":"10/1000","tj_max":175,"qualification":""})KSPEC";
// diodes.ndjson line 16850: 1.5KE16CA-E3/54
const char* k1_5KE16CA_E3_54 = R"KSPEC({"mpn":"1.5KE16CA-E3/54","length_m":null,"width_m":null,"height_m":null,"case_code":"1.5KE","mount":"","lead_pitch_m":null,"is_production":false,"provenance_retracted":false,"box_axes_ambiguous":false,"vrrm":13.6,"if_avg":null,"vf":null,"power_dissipation":null,"qrr":null,"trr":null,"technology":"tvs","die_configuration":null,"internal_connection":null,"vz_min_V":15.2,"vz_max_V":16.8,"vz_tolerance_pct":5.000000000000004,"clamping_voltage":22.5,"clamping_current":66.7,"peak_pulse_power":1500,"peak_pulse_current":66.7,"junction_capacitance":null,"junction_capacitance_vr":null,"clamping_waveform":"10/1000","peak_pulse_power_waveform":"10/1000","peak_pulse_current_waveform":"10/1000","tj_max":175,"qualification":""})KSPEC";
// diodes.ndjson line 16838: 1.5KE10CA-E3/54
const char* k1_5KE10CA_E3_54 = R"KSPEC({"mpn":"1.5KE10CA-E3/54","length_m":null,"width_m":null,"height_m":null,"case_code":"1.5KE","mount":"","lead_pitch_m":null,"is_production":false,"provenance_retracted":false,"box_axes_ambiguous":false,"vrrm":8.55,"if_avg":null,"vf":null,"power_dissipation":null,"qrr":null,"trr":null,"technology":"tvs","die_configuration":null,"internal_connection":null,"vz_min_V":9.5,"vz_max_V":10.5,"vz_tolerance_pct":5,"clamping_voltage":14.5,"clamping_current":103,"peak_pulse_power":1500,"peak_pulse_current":103,"junction_capacitance":null,"junction_capacitance_vr":null,"clamping_waveform":"10/1000","peak_pulse_power_waveform":"10/1000","peak_pulse_current_waveform":"10/1000","tj_max":175,"qualification":""})KSPEC";
// timing_devices.ndjson line 6507: AK2F3PBQ1-312.5000
const char* kAK2F3PBQ1_312_5000 = R"KSPEC({"mpn":"AK2F3PBQ1-312.5000","length_m":null,"width_m":null,"height_m":null,"case_code":"2520 6-SMD","mount":"","lead_pitch_m":null,"is_production":true,"provenance_retracted":false,"box_axes_ambiguous":false,"subtype":"oscillator","technology":"crystalOscillator","device_type":"oscillator","frequency":312500000,"load_capacitance":null,"load_capacitance_pF":null,"built_in_capacitance":null,"esr":null,"output_type":"lvpecl","mode":null,"enable_function":"standby","supply_min_V":2.5,"supply_max_V":2.5,"tolerance_ppm":null,"stability_ppm":20,"temp_min_C":-40,"temp_max_C":85})KSPEC";
// timing_devices.ndjson line 12579: ECX-P35BN-312.500
const char* kECX_P35BN_312_500 = R"KSPEC({"mpn":"ECX-P35BN-312.500","length_m":null,"width_m":null,"height_m":null,"case_code":"6-SMD, No Lead","mount":"","lead_pitch_m":null,"is_production":false,"provenance_retracted":false,"box_axes_ambiguous":false,"subtype":"oscillator","technology":"crystalOscillator","device_type":"oscillator","frequency":312500000,"load_capacitance":null,"load_capacitance_pF":null,"built_in_capacitance":null,"esr":null,"output_type":"lvpecl","mode":null,"enable_function":null,"supply_min_V":3.3,"supply_max_V":3.3,"tolerance_ppm":null,"stability_ppm":49.99999999999999,"temp_min_C":-40,"temp_max_C":85})KSPEC";
// timing_devices.ndjson line 12553: ECX-P27BN-312.500
const char* kECX_P27BN_312_500 = R"KSPEC({"mpn":"ECX-P27BN-312.500","length_m":null,"width_m":null,"height_m":null,"case_code":"6-SMD, No Lead","mount":"","lead_pitch_m":null,"is_production":false,"provenance_retracted":false,"box_axes_ambiguous":false,"subtype":"oscillator","technology":"crystalOscillator","device_type":"oscillator","frequency":312500000,"load_capacitance":null,"load_capacitance_pF":null,"built_in_capacitance":null,"esr":null,"output_type":"lvpecl","mode":null,"enable_function":null,"supply_min_V":2.5,"supply_max_V":2.5,"tolerance_ppm":null,"stability_ppm":49.99999999999999,"temp_min_C":-40,"temp_max_C":85})KSPEC";
// capacitors.ndjson line 476527: CWR06DC155JCA
const char* kCWR06DC155JCA = R"KSPEC({"mpn":"CWR06DC155JCA","length_m":null,"width_m":null,"height_m":null,"case_code":"A","mount":"SMT","lead_pitch_m":null,"is_production":false,"provenance_retracted":false,"box_axes_ambiguous":false,"value_si":1.5e-06,"voltage":6,"esr":8,"ripple_current":null,"technology":"tantalum-mno2","dielectric_code":"","family":"CWR06","voltage_ac":null,"esr_frequency":100000,"temp_min_C":-55,"temp_max_C":125,"ripple_current_frequency":null,"safety_class":null,"capacitance_min":1.425e-06,"capacitance_max":1.575e-06,"lead_form":null,"qualified_spec":null,"qualified_grade":null,"qualified_failure_rate":null})KSPEC";
// capacitors.ndjson line 8327: T499A225K010ATE8K0
const char* kT499A225K010ATE8K0 = R"KSPEC({"mpn":"T499A225K010ATE8K0","length_m":null,"width_m":null,"height_m":null,"case_code":"1206","mount":"SMT","lead_pitch_m":null,"is_production":true,"provenance_retracted":false,"box_axes_ambiguous":false,"value_si":2.2e-06,"voltage":10,"esr":4.738103,"ripple_current":null,"technology":"tantalum-mno2","dielectric_code":"","family":"","voltage_ac":null,"esr_frequency":100000,"temp_min_C":-55,"temp_max_C":125,"ripple_current_frequency":null,"safety_class":null,"capacitance_min":null,"capacitance_max":null,"lead_form":null,"qualified_spec":null,"qualified_grade":null,"qualified_failure_rate":null})KSPEC";
// capacitors.ndjson line 431833: 293D105X9016A2TE3
const char* k293D105X9016A2TE3 = R"KSPEC({"mpn":"293D105X9016A2TE3","length_m":null,"width_m":null,"height_m":null,"case_code":"A","mount":"SMT","lead_pitch_m":null,"is_production":false,"provenance_retracted":false,"box_axes_ambiguous":false,"value_si":1e-06,"voltage":16,"esr":9.3,"ripple_current":0.09,"technology":"tantalum-mno2","dielectric_code":"","family":"293D","voltage_ac":null,"esr_frequency":100000,"temp_min_C":-55,"temp_max_C":125,"ripple_current_frequency":100000,"safety_class":null,"capacitance_min":9e-07,"capacitance_max":1.1e-06,"lead_form":null,"qualified_spec":null,"qualified_grade":null,"qualified_failure_rate":null})KSPEC";
// capacitors.ndjson line 439622: T95A685M016EBBL
const char* kT95A685M016EBBL = R"KSPEC({"mpn":"T95A685M016EBBL","length_m":null,"width_m":null,"height_m":null,"case_code":"A","mount":"SMT","lead_pitch_m":null,"is_production":false,"provenance_retracted":false,"box_axes_ambiguous":false,"value_si":6.8e-06,"voltage":16,"esr":1.4,"ripple_current":null,"technology":"tantalum-mno2","dielectric_code":"","family":"T95","voltage_ac":null,"esr_frequency":100000,"temp_min_C":-55,"temp_max_C":125,"ripple_current_frequency":null,"safety_class":null,"capacitance_min":5.44e-06,"capacitance_max":8.16e-06,"lead_form":null,"qualified_spec":null,"qualified_grade":null,"qualified_failure_rate":null})KSPEC";
// capacitors.ndjson line 433729: 293D685X9016A2TE3
const char* k293D685X9016A2TE3 = R"KSPEC({"mpn":"293D685X9016A2TE3","length_m":null,"width_m":null,"height_m":null,"case_code":"A","mount":"SMT","lead_pitch_m":null,"is_production":false,"provenance_retracted":false,"box_axes_ambiguous":false,"value_si":6.8e-06,"voltage":16,"esr":4.2,"ripple_current":0.13,"technology":"tantalum-mno2","dielectric_code":"","family":"293D","voltage_ac":null,"esr_frequency":100000,"temp_min_C":-55,"temp_max_C":125,"ripple_current_frequency":100000,"safety_class":null,"capacitance_min":6.12e-06,"capacitance_max":7.48e-06,"lead_form":null,"qualified_spec":null,"qualified_grade":null,"qualified_failure_rate":null})KSPEC";
json sp(const char* s) { return json::parse(s); }
json rank(const char* cat, const json& orig, const json& cands) {
    return kelvin::crossref::cross_reference(cat, orig, cands);
}
json by_mpn(const json& r, const std::string& mpn) {
    for (const auto& c : r.at("candidates"))
        if (c.value("mpn", "") == mpn) return c;
    FAIL("candidate " << mpn << " not in the ranking");
    return json();
}
int rank_of(const json& r, const std::string& mpn) {
    int i = 0;
    for (const auto& c : r.at("candidates")) {
        if (c.value("mpn", "") == mpn) return i;
        ++i;
    }
    return -1;
}
}  // namespace dec_1113_j

// J1 — Qarlos bench 20680025: 1N5233B (Vz 6.0 V) sat inside BZV55-C5V6's 5.2-6.0 V window, 7 %
// above its 5.6 V nominal, and ranked level with the exact 5.6 V 1N5232B. Owner 2026-11-13: a
// zener more than 1 % off the original's nominal sorts after every exact-nominal zener (the
// resistor near-value tier, value_differs); its grade is unchanged.
TEST_CASE("qarlos r14-r16 J1: an off-nominal zener ranks after every exact-nominal one",
          "[qarlos][crossref][zener][dec-1113-j]") {
    using namespace dec_1113_j;
    const json orig = sp(kBZV55_C5V6);
    // The off-nominal part FIRST in the pool: only the tier can put it behind.
    const json r = rank("diode", orig, json::array({sp(k1N5233B), sp(k1N5232B)}));
    const json off = by_mpn(r, "1N5233B"), exact = by_mpn(r, "1N5232B");
    CHECK(rank_of(r, "1N5232B") < rank_of(r, "1N5233B"));
    CHECK(off.value("value_differs", false));
    CHECK_FALSE(exact.value("value_differs", false));
    // Grade and direction unchanged: the same as the exact part's (both minor_review /
    // equivalent on this pair, the footprint WARN), not the resistor cap.
    CHECK(off["grade"] == "minor_review");
    CHECK(off["direction"] == exact["direction"]);
    CHECK(off["grade"] == exact["grade"]);
    CHECK(has_note(off, "a different nominal"));

    // Boundary: 1 % off the nominal is the same marking (no tier); 1.1 % is not.
    json at1 = sp(k1N5232B), past1 = sp(k1N5232B);
    at1["mpn"] = "VZ+1.0%";
    at1["vrrm"] = 5.6 * 1.0099;
    past1["mpn"] = "VZ+1.1%";
    past1["vrrm"] = 5.6 * 1.011;
    const json rb = rank("diode", orig, json::array({past1, at1}));
    CHECK_FALSE(by_mpn(rb, "VZ+1.0%").value("value_differs", false));
    CHECK(by_mpn(rb, "VZ+1.1%").value("value_differs", false));
    CHECK(rank_of(rb, "VZ+1.0%") < rank_of(rb, "VZ+1.1%"));
    CHECK(by_mpn(rb, "VZ+1.1%")["grade"] == by_mpn(rb, "VZ+1.0%")["grade"]);
}

// J2 — Qarlos bench 20575296: 1.5KE16CA-E3/54 (13.6 V standoff) for SA6.5CA (6.5 V, 2.09x)
// came back a WARN / minor_review like a 1.2x part. Owner 2026-11-13: a TVS standoff more than
// 1.5x the original's is a FAIL (major_review) with a note; 1.1x-1.5x stays WARN.
TEST_CASE("qarlos r14-r16 J2: a TVS standoff above 1.5x the original's is a FAIL",
          "[qarlos][crossref][tvs][dec-1113-j]") {
    using namespace dec_1113_j;
    const json orig = sp(kSA6_5CA);
    const json r = rank("diode", orig, json::array({sp(k1_5KE16CA_E3_54), sp(k1_5KE10CA_E3_54)}));
    const json far = by_mpn(r, "1.5KE16CA-E3/54"), near = by_mpn(r, "1.5KE10CA-E3/54");
    CHECK(verdict_of(far, "standoff_band") == "fail");
    CHECK(far["grade"] == "major_review");
    CHECK(far["status"] != "no_substitute");
    CHECK(has_note(far, "standoff 13.6 V against the original's 6.5 V"));
    CHECK(has_note(far, "more than 1.5x the original's standoff"));
    // 8.55 V is 1.32x: still the WARN.
    CHECK(verdict_of(near, "standoff_band") == "warn");
    CHECK_FALSE(has_note(near, "more than 1.5x"));
    CHECK(rank_of(r, "1.5KE10CA-E3/54") < rank_of(r, "1.5KE16CA-E3/54"));

    // Boundary at 1.4x / 1.6x on the same real candidate.
    json x14 = sp(k1_5KE10CA_E3_54), x16 = sp(k1_5KE10CA_E3_54);
    x14["mpn"] = "SO-1.4x";
    x14["vrrm"] = 6.5 * 1.4;
    x16["mpn"] = "SO-1.6x";
    x16["vrrm"] = 6.5 * 1.6;
    const json rb = rank("diode", orig, json::array({x14, x16}));
    CHECK(verdict_of(by_mpn(rb, "SO-1.4x"), "standoff_band") == "warn");
    CHECK(verdict_of(by_mpn(rb, "SO-1.6x"), "standoff_band") == "fail");
    CHECK(by_mpn(rb, "SO-1.6x")["grade"] == "major_review");
    CHECK(by_mpn(rb, "SO-1.4x")["grade"] != "major_review");
}

// J3 — Qarlos r16 26859036: ECX-P35BN-312.500 (3.3 V only) for AK2F3PBQ1-312.5000 (2.5 V)
// was major_review. Owner 2026-11-13: a supply range that does not cover the original's is
// REJECTED, a hard key like output_type, with the reason visible.
TEST_CASE("qarlos r14-r16 J3: an oscillator whose supply does not cover the original's is rejected",
          "[qarlos][crossref][timing][dec-1113-j]") {
    using namespace dec_1113_j;
    const json orig = sp(kAK2F3PBQ1_312_5000);
    const json r = rank("timeBase", orig,
                        json::array({sp(kECX_P35BN_312_500), sp(kECX_P27BN_312_500)}));
    const json p35 = by_mpn(r, "ECX-P35BN-312.500"), p27 = by_mpn(r, "ECX-P27BN-312.500");
    CHECK(p35["status"] == "no_substitute");
    CHECK(p35["reason"] == "supply range does not cover the original's rail");
    CHECK(verdict_of(p35, "supply_min_V") == "fail");
    CHECK(has_note(p35, "the original runs from 2.5 V, the substitute from 3.3 V"));
    // The covering part is still ranked.
    CHECK(p27["status"] != "no_substitute");
    CHECK(verdict_of(p27, "supply_min_V") == "pass");
    CHECK(verdict_of(p27, "supply_max_V") == "pass");

    // Boundary: the same candidate with its supply unstated is UNVERIFIED, not rejected.
    json unstated = sp(kECX_P35BN_312_500);
    unstated["mpn"] = "NO-SUPPLY";
    unstated["supply_min_V"] = nullptr;
    unstated["supply_max_V"] = nullptr;
    // A range that covers 2.5 V on both ends passes.
    json wide = sp(kECX_P35BN_312_500);
    wide["mpn"] = "WIDE";
    wide["supply_min_V"] = 1.71;
    wide["supply_max_V"] = 3.63;
    // MEMS crossing: covering -> still the crossing's major_review; not covering -> rejected.
    json mems_ok = wide, mems_short = sp(kECX_P35BN_312_500);
    mems_ok["mpn"] = "MEMS-WIDE";
    mems_ok["technology"] = "mems";
    mems_short["mpn"] = "MEMS-3V3";
    mems_short["technology"] = "mems";
    const json rb = rank("timeBase", orig, json::array({unstated, wide, mems_ok, mems_short}));
    const json u = by_mpn(rb, "NO-SUPPLY");
    CHECK(u["status"] != "no_substitute");
    CHECK(verdict_of(u, "supply_min_V") == "unverified");
    CHECK(has_note(u, "states no supply range"));
    CHECK(by_mpn(rb, "WIDE")["status"] != "no_substitute");
    CHECK(verdict_of(by_mpn(rb, "WIDE"), "supply_min_V") == "pass");
    CHECK(by_mpn(rb, "MEMS-WIDE")["status"] != "no_substitute");
    CHECK(by_mpn(rb, "MEMS-WIDE")["grade"] == "major_review");
    CHECK(by_mpn(rb, "MEMS-3V3")["status"] == "no_substitute");
    CHECK(by_mpn(rb, "MEMS-3V3")["reason"] == "supply range does not cover the original's rail");
}

// J4 — Qarlos r16 26859036: CWR06DC155JCA (case "A") was sized as the EIA 3216 tantalum and
// came out different_case against T499A225K010ATE8K0 ("1206" = 3216) only because the two
// strings differ. CWR06 (MIL-PRF-55365/4) draws its own case table: its A is 2.54 x 1.27 mm.
// Owner 2026-11-13: the letter -> EIA mapping applies only to series known to use EIA letters;
// other series use stated dimensions, or are UNVERIFIED. The CWR06 / 194D table and the EIA
// series list come from the Vishay datasheets (CrossRefDimensions.hpp).
TEST_CASE("qarlos r14-r16 J4: a tantalum case letter is sized by its series, never EIA by default",
          "[qarlos][crossref][tantalum][dec-1113-j]") {
    using namespace dec_1113_j;
    using kelvin::crossref::resolve_tantalum_letter;
    // The series tables.
    auto a_eia = resolve_tantalum_letter("A", "293D");
    REQUIRE(a_eia);
    CHECK(*a_eia->length == Approx(3.2e-3));
    CHECK(*a_eia->width == Approx(1.6e-3));
    auto a_cwr = resolve_tantalum_letter("A", "CWR06");
    REQUIRE(a_cwr);
    CHECK(*a_cwr->length == Approx(2.54e-3));
    CHECK(*a_cwr->width == Approx(1.27e-3));
    auto e_cwr = resolve_tantalum_letter("E", "194D");
    REQUIRE(e_cwr);
    CHECK(*e_cwr->length == Approx(5.08e-3));
    CHECK(*e_cwr->width == Approx(2.54e-3));
    CHECK_FALSE(resolve_tantalum_letter("A", "T95").has_value());  // its own A: 3.7 x 1.8
    CHECK_FALSE(resolve_tantalum_letter("A", "").has_value());     // no series named
    CHECK_FALSE(resolve_tantalum_letter("E", "293D").has_value()); // not in the A-D table

    // The real pair: still different_case, now for the bodies.
    const json cwr = sp(kCWR06DC155JCA);
    const json r = rank("capacitor", cwr, json::array({sp(kT499A225K010ATE8K0)}));
    const json t499 = by_mpn(r, "T499A225K010ATE8K0");
    CHECK(t499["footprint"] == "different_case");
    CHECK(has_note(t499, "different package/case (A -> 1206) — 2.54 x 1.27 mm against 3.2 x 1.6 mm"));

    // Same letter string, different series: CWR06 "A" against 293D "A" is not the same land.
    const json r2 = rank("capacitor", cwr, json::array({sp(k293D105X9016A2TE3)}));
    CHECK(by_mpn(r2, "293D105X9016A2TE3")["footprint"] != "fits");

    // Boundary: a series outside the checked list (T95, whose A is 3.7 x 1.8 mm) with no drawing
    // is UNVERIFIED against a 293D "A" of the same 6.8 uF / 16 V — not 'fits' on the shared
    // letter.
    const json eia = sp(k293D685X9016A2TE3);
    const json r3 = rank("capacitor", eia, json::array({sp(kT95A685M016EBBL)}));
    const json t95 = by_mpn(r3, "T95A685M016EBBL");
    CHECK(verdict_of(t95, "footprint") == "unverified");
    CHECK(t95["footprint"] != "fits");
    // And an EIA-series letter still resolves as before: 293D "A" against itself fits.
    json twin = eia;
    twin["mpn"] = "293D-TWIN";
    CHECK(by_mpn(rank("capacitor", eia, json::array({twin})), "293D-TWIN")["footprint"] == "fits");
}
