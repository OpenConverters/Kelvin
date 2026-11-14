// Capacitor curves compared curve-vs-curve (owner rule 2026-11-14: "make sure that Kelvin
// compares ALL existing graphs"). Real records from the frozen bench data (TAS 524a699,
// tools/qarlos/bench/data-524a699/capacitors.ndjson), copied verbatim into
// tests/fixtures/curves_capacitors.ndjson; each is extracted, turned into the ranker spec the
// way web/src/crossref.js does, and ranked.
#include <cmath>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "Browse.hpp"
#include "CrossRef.hpp"
#include "Views.hpp"

using nlohmann::json;

namespace {
json fixture(const std::string& mpn) {
    std::ifstream in(std::string(KELVIN_TEST_DIR) + "/fixtures/curves_capacitors.ndjson");
    std::string line;
    while (std::getline(in, line)) {
        json j = json::parse(line);
        if (j["capacitor"]["manufacturerInfo"]["reference"] == mpn) return j;
    }
    FAIL("fixture " << mpn << " missing");
    return {};
}
kelvin::CapacitorRow row(const std::string& mpn) {
    auto r = kelvin::extract_capacitor(fixture(mpn));
    REQUIRE(r.has_value());
    return *r;
}
// web/src/crossref.js capacitor spec (base() + spec()), including the v22 curve fields.
json spec(const kelvin::CapacitorRow& r) {
    auto nz = [](double v) { return std::isnan(v) || !(v > 0) ? json(nullptr) : json(v); };
    auto raw = [](double v) { return std::isnan(v) ? json(nullptr) : json(v); };
    auto s = [](const std::string& v) { return v.empty() ? json(nullptr) : json(v); };
    json o{{"mpn", r.mpn}, {"length_m", nz(r.length_m)}, {"width_m", nz(r.width_m)},
           {"height_m", nz(r.height_m)}, {"case_code", r.case_code}, {"mount", r.mount},
           {"lead_pitch_m", nz(r.lead_pitch_m)},
           {"value_si", nz(r.capacitance)}, {"voltage", nz(r.v_rated)}, {"esr", nz(r.esr)},
           {"ripple_current", nz(r.ripple_current_rms)}, {"technology", r.technology},
           {"dielectric_code", r.dielectric_code}, {"family", r.family},
           {"esr_frequency", nz(r.esr_frequency)}, {"temp_min_C", raw(r.temp_min_c)},
           {"temp_max_C", raw(r.temp_max_c)},
           {"ripple_current_frequency", nz(r.ripple_current_frequency)},
           {"ripple_current_temperature", raw(r.ripple_current_temperature)},
           {"capacitance_min", nz(r.capacitance_min)}, {"capacitance_max", nz(r.capacitance_max)},
           {"lead_form", s(r.lead_form)}, {"qualified_spec", s(r.qualified_spec)},
           {"qualified_grade", s(r.qualified_grade)},
           {"qualified_failure_rate", raw(r.qualified_failure_rate)}};
    json extra;
    kelvin::browse::extra_row_json(r, extra);  // the curves, exactly as the browse row carries them
    for (auto& [k, v] : extra.items()) o[k] = v;
    return o;
}
json rank_one(const std::string& orig, const std::string& sub) {
    auto r = kelvin::crossref::cross_reference("capacitor", spec(row(orig)),
                                               json::array({spec(row(sub))}));
    REQUIRE(r["candidates"].size() == 1);
    return r["candidates"][0];
}
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
std::string notes_of(const json& c) { return c.contains("notes") ? c["notes"].dump(1) : ""; }
}  // namespace

TEST_CASE("ESR curves: both records carry one -> compared on the overlap",
          "[curves][capacitor][esr-curve]") {
    // Samsung CL05A225KP5NSNC (0.0346 ohm at 100 kHz, curve 100 Hz-6 GHz) vs TDK
    // C1005X5R1A225K050BC (no scalar, curve 1 kHz-3 GHz).
    const auto o = row("CL05A225KP5NSNC");
    const auto s = row("C1005X5R1A225K050BC");
    REQUIRE_FALSE(o.esr_curve.empty());
    REQUIRE_FALSE(s.esr_curve.empty());
    const json c = rank_one("CL05A225KP5NSNC", "C1005X5R1A225K050BC");
    INFO(notes_of(c));
    // The overlap is the TDK range; the worst point is at 1.1 kHz, where the substitute's
    // 3.17 ohm is x1.24 the original's 2.55 ohm: WARN (ESR spec, PASS <= 1x, FAIL > 1.5x).
    CHECK(has_note(c, "ESR compared on both curves from 1002.73 to 3e+09 Hz (388 points, "
                      "log-log): worst at 1099.4 Hz, substitute 3.168 ohm vs original 2.546 ohm"));
    CHECK(verdict_of(c, "esr") == "warn");
    CHECK(c["grade"] != "drop_in");
}

TEST_CASE("ESR basis: the substitute's curve is read at the original's frequency",
          "[curves][capacitor][esr-curve]") {
    // Vishay MAL209332102E3: 0.18 ohm at 120 Hz, no curve. Cornell Dubilier 380LQ102M200H452:
    // 0.09 ohm at 20 kHz, curve 120 Hz (0.199 ohm) - 20 kHz (0.09 ohm). Different bases: the
    // substitute's own curve gives its ESR at 120 Hz, 0.199 ohm, x1.11 -> WARN (ESR band 1.5x).
    const json c = rank_one("MAL209332102E3", "380LQ102M200H452");
    INFO(notes_of(c));
    CHECK(verdict_of(c, "esr") == "warn");
    CHECK(verdict_of(c, "esr_basis") == "");
    CHECK(has_note(c, "ESR compared at the original's 120 Hz: the substitute's curve reads "
                      "0.199 ohm there (its record quotes 0.09 ohm at 20000 Hz) against the "
                      "original's 0.18 ohm"));
}

TEST_CASE("DC-bias curves: effective capacitance compared on the common bias range",
          "[curves][capacitor][bias-curve]") {
    // Murata ZRB157R6YA225ME11 (2.2 uF 35 V X5R 0402; 201 points 0-35 V at 25 degC) vs TDK
    // C1005X5R1V225K050BC (2.2 uF 35 V X5R 0402; 14 points 0-35 V, no temperature stated):
    // the same nominal, rating, dielectric and case, so it ranked a clean drop_in. Under bias
    // the TDK part keeps far less capacitance (0.54 uF at 5 V against Murata's ~1.2 uF).
    const auto o = row("ZRB157R6YA225ME11");
    const auto s = row("C1005X5R1V225K050BC");
    REQUIRE_FALSE(o.bias_curve.empty());
    REQUIRE_FALSE(s.bias_curve.empty());
    CHECK(o.bias_curve.temperature == 25.0);
    CHECK(std::isnan(s.bias_curve.temperature));
    const json c = rank_one("ZRB157R6YA225ME11", "C1005X5R1V225K050BC");
    INFO(notes_of(c));
    CHECK(verdict_of(c, "capacitance_bias") == "fail");
    CHECK(has_note(c, "effective capacitance under DC bias compared on both curves over DC "
                      "bias 0 to 35 V"));
    CHECK(c["grade"] != "drop_in");
    // The same part against itself: every point equal -> pass.
    const json same = rank_one("ZRB157R6YA225ME11", "ZRB157R6YA225ME11");
    CHECK(verdict_of(same, "capacitance_bias") == "pass");
}
