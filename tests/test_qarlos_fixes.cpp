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
