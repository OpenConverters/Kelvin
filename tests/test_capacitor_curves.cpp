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
        const json& mi = j["capacitor"]["manufacturerInfo"];
        if (mi.value("reference", "") == mpn ||
            mi["datasheetInfo"]["part"].value("partNumber", "") == mpn)
            return j;
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

TEST_CASE("ESR curves: an original with no |Z| curve has no useful band -> not graded",
          "[curves][capacitor][esr-curve]") {
    // Samsung CL05A225KP5NSNC (0.0346 ohm at 100 kHz, curve 100 Hz-6 GHz) vs TDK
    // C1005X5R1A225K050BC (no scalar, curve 1 kHz-3 GHz). Owner 2026-11-14: impedance-type
    // curves (ESR included) are judged only in the useful band, from the original's rating
    // frequency up to ITS self-resonance; the Samsung record carries no |Z| curve, so its
    // self-resonance is not on record and the curves are not graded.
    const auto o = row("CL05A225KP5NSNC");
    const auto s = row("C1005X5R1A225K050BC");
    REQUIRE_FALSE(o.esr_curve.empty());
    REQUIRE_FALSE(s.esr_curve.empty());
    const json c = rank_one("CL05A225KP5NSNC", "C1005X5R1A225K050BC");
    INFO(notes_of(c));
    CHECK(has_note(c, "ESR curves not compared: the original carries no |Z| curve, so its "
                      "self-resonance (the top of the useful band) is not on record"));
    // The scalars are then compared as stated: 0.0346 ohm at 100 kHz against the TDK curve's
    // own 100 kHz reading (a common basis) -> pass.
    CHECK(verdict_of(c, "esr") == "pass");
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
    // Owner 2026-11-14: a condition stated by ONE side only is a DIFFERENT condition. Murata's
    // curve is at 25 degC, TDK's at no stated temperature -> not compared, UNVERIFIED + note.
    const json c = rank_one("ZRB157R6YA225ME11", "C1005X5R1V225K050BC");
    INFO(notes_of(c));
    CHECK(verdict_of(c, "capacitance_bias") == "unverified");
    CHECK(has_note(c, "effective capacitance under DC bias curves measured under different "
                      "conditions (temperature 25 degC vs unstated) — not compared"));
    // The same part against itself: every point equal -> pass.
    const json same = rank_one("ZRB157R6YA225ME11", "ZRB157R6YA225ME11");
    CHECK(verdict_of(same, "capacitance_bias") == "pass");
}

TEST_CASE("Frequency curves: |Z|, C(f) and ESL compared on the common band",
          "[curves][capacitor][freq-curves]") {
    // KEMET C0603C104K3RECTU (100 nF 25 V 0603) vs C0603C104M4RAC (100 nF 16 V 0603): both
    // carry |Z|, C(f) and ESL curves (KEMET is the only maker with ESL curves, 16,583 records).
    const auto o = row("C0603C104K3RECTU");
    const auto s = row("C0603C104M4RAC");
    REQUIRE_FALSE(o.z_curve.empty());
    REQUIRE_FALSE(o.cf_curve.empty());
    REQUIRE_FALSE(o.esl_curve.empty());
    REQUIRE_FALSE(s.esl_curve.empty());
    const json c = rank_one("C0603C104K3RECTU", "C0603C104M4RAC");
    INFO(notes_of(c));
    // Owner 2026-11-14: |Z|, ESL and ESR curves are graded only in the useful band, from the
    // original's ESR test frequency (100 kHz, stated) to its self-resonance (|Z| minimum,
    // 31.6 MHz). |Z|: the substitute's 0.0535 ohm at 31.6 MHz is x2.1 the original's 0.0251
    // -> FAIL.
    CHECK(verdict_of(c, "impedance_curve") == "fail");
    CHECK(has_note(c, "|Z| compared on both curves over frequency 100000 to 3.16228e+07 Hz (26 "
                      "points, log-log, bands of the ESR spec): worst at 3.16228e+07 Hz, "
                      "substitute 0.05346 ohm vs original 0.02514 ohm (useful band from the "
                      "original's rating (ESR test) frequency 100000 Hz to the original's "
                      "self-resonance (|Z| minimum) at 3.16228e+07 Hz; points outside it are "
                      "not graded)"));
    // ESR curves, the same band: the substitute is lower everywhere in it.
    CHECK(verdict_of(c, "esr") == "pass");
    CHECK(has_note(c, "ESR compared on both curves from 100000 to 3.16228e+07 Hz (28 points, "
                      "log-log): passes everywhere"));
    // C(f) up to the original's self-resonance (21.9 MHz on the substitute): equal or more.
    CHECK(verdict_of(c, "capacitance_frequency_curve") == "pass");
    CHECK(has_note(c, "capacitance vs frequency compared on both curves over frequency 100 to "
                      "2.18776e+07 Hz (55 points, log-log, bands of the capacitance spec): "
                      "passes everywhere, tightest at 125.893 Hz"));
    // ESL: the original's ESL curve starts at 33.1 MHz, above its 31.6 MHz self-resonance, so
    // no ESL point lies in the useful band -> UNVERIFIED, not graded.
    CHECK(verdict_of(c, "esl_curve") == "unverified");
    CHECK(has_note(c, "ESL curves do not meet inside the useful band (original 3.31131e+07 to "
                      "1e+09 Hz, substitute 2.29087e+07 to 1e+09 Hz; useful band from the "
                      "original's rating (ESR test) frequency 100000 Hz to the original's "
                      "self-resonance (|Z| minimum) at 3.16228e+07 Hz; points outside it are "
                      "not graded) — not compared"));
    // (The 16 V substitute is rejected for the 25 V original's voltage anyway; the reverse
    // direction is the one a designer could take.) The other way round: |Z| still fails where
    // the two curves cross near the 16 V part's resonance (0.0616 vs 0.0265 ohm at 20 MHz);
    // ESL is not graded (its 25.1 MHz self-resonance lies below both ESL curves' start).
    const json r = rank_one("C0603C104M4RAC", "C0603C104K3RECTU");
    INFO(notes_of(r));
    CHECK(verdict_of(r, "impedance_curve") == "fail");
    CHECK(verdict_of(r, "esl_curve") == "unverified");
}

TEST_CASE("|Z| curves: a condition stated on one side only is a different condition",
          "[curves][capacitor][freq-curves]") {
    // Murata ZRB157R6YA225ME11 states its |Z| at 25 degC, 0 V bias; TDK C1005X5R1V225K050BC
    // states no condition. Owner 2026-11-14: one-sided = different -> UNVERIFIED + note.
    const json c = rank_one("ZRB157R6YA225ME11", "C1005X5R1V225K050BC");
    INFO(notes_of(c));
    CHECK(verdict_of(c, "impedance_curve") == "unverified");
    CHECK(has_note(c, "|Z| curves measured under different conditions (temperature 25 degC vs "
                      "unstated, DC bias 0 V vs unstated) — not compared"));
}

TEST_CASE("|Z| useful band: an ESR Kelvin read off the curve is no rating frequency -> 1 MHz",
          "[curves][capacitor][freq-curves][useful-band]") {
    // TDK C1005X5R1V225K050BC states no ESR: Kelvin reads its ESR off the ESR curve at 100 kHz
    // (esr_from_curve). That 100 kHz is not a frequency the record states, so the useful band
    // of its curves starts at 1 MHz (owner 2026-11-14), up to its |Z| minimum.
    const auto o = row("C1005X5R1V225K050BC");
    REQUIRE(o.esr_from_curve);
    REQUIRE_FALSE(o.z_curve.empty());
    const json c = rank_one("C1005X5R1V225K050BC", "C1005X5R1A225K050BC");
    INFO(notes_of(c));
    CHECK(has_note(c, "useful band from 1 MHz (the original states no rating frequency) to "));
    CHECK(has_note(c, "|Z| compared on both curves over frequency 1e+06 to "));
}
