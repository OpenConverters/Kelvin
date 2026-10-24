// Regression tests for the Qarlos cross-reference audit of 2026-10-24
// (ABT #2029-#2051). Each case reproduces the reported row in miniature.
#include <cmath>
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
