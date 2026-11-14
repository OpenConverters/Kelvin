// Magnetic curves compared under the owner's curve rule (2026-11-14: "make sure that Kelvin
// compares ALL existing graphs"). Every record used here is a REAL TAS magnetics.ndjson line
// (data-524a699, distributorsInfo dropped) kept in tests/fixtures/magnetic_curves.ndjson, so
// each test runs the extractor on what the catalogue states, then the ranker on the spec
// block web/src/crossref.js builds from the browse row.
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "Browse.hpp"
#include "CrossRef.hpp"
#include "Views.hpp"

using Catch::Approx;
using nlohmann::json;

namespace {
// MAGNETIC_CURVE_KEYS in web/src/crossref.js.
const char* const kCurveKeys[] = {"impedance_curve", "resistance_curve", "reactance_curve"};
// The real record whose manufacturerInfo.reference is `mpn`; throws when the fixture lacks it.
json real_record(const std::string& mpn) {
    std::ifstream in(std::string(KELVIN_TEST_DIR) + "/fixtures/magnetic_curves.ndjson");
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        json j = json::parse(line);
        if (j["magnetic"]["manufacturerInfo"].value("reference", "") == mpn) return j;
    }
    throw std::runtime_error("magnetic_curves.ndjson has no record " + mpn);
}
kelvin::MagneticRow row_of(const std::string& mpn) {
    auto r = kelvin::extract_magnetic(real_record(mpn));
    if (!r) throw std::runtime_error("extract_magnetic refused " + mpn);
    return *r;
}
// The spec block web/src/crossref.js builds from the browse row: base() + the inductor model
// (XREF 'magnetic') or, for `bead`, the impedance model (BEAD_VARIANT), with magneticCurves(r).
json spec_of(const kelvin::MagneticRow& r, bool bead = false) {
    json row;
    kelvin::browse::extra_row_json(r, row);
    auto nz = [](double v) { return std::isnan(v) || !(v > 0) ? json(nullptr) : json(v); };
    auto raw = [](double v) { return std::isnan(v) ? json(nullptr) : json(v); };
    json s{{"mpn", r.mpn},
           {"length_m", nz(r.length_m)}, {"width_m", nz(r.width_m)},
           {"height_m", nz(r.height_m)}, {"case_code", r.case_code}, {"mount", r.mount},
           {"lead_pitch_m", nz(r.lead_pitch_m)}, {"is_production", r.is_production},
           {"provenance_retracted", r.provenance_retracted},
           {"rated_current", nz(r.rated_current)}, {"dcr", nz(r.dcr)}, {"srf", nz(r.srf)},
           {"temp_min_C", raw(r.temp_min_c)}, {"temp_max_C", raw(r.temp_max_c)}};
    if (bead) {
        s["impedance_100mhz"] = nz(r.impedance_100mhz);
        s["impedance_peak"] = nz(r.impedance_peak);
        s["impedance_peak_freq"] = nz(r.impedance_peak_freq);
    } else {
        s["value_si"] = nz(r.inductance);
        s["saturation_current"] = nz(r.saturation_current);
        s["saturation_points"] = row["saturation_points"];
        s["tolerance_minus_pct"] = raw(r.inductance_tol_minus_pct);
        s["tolerance_plus_pct"] = raw(r.inductance_tol_plus_pct);
        s["bias_curve"] = row["bias_curve"];
        s["saturation_from_curve"] = row["saturation_from_curve"];
    }
    for (const char* k : kCurveKeys) s[k] = row.contains(k) ? row[k] : json(nullptr);
    return s;
}
std::string verdict_of(const json& c, const std::string& name) {
    for (const auto& p : c.at("params"))
        if (p.value("name", "") == name) return p.value("verdict", "");
    return "";
}
std::string note_with(const json& c, const std::string& needle) {
    for (const auto& n : c.value("notes", json::array()))
        if (n.get<std::string>().find(needle) != std::string::npos) return n.get<std::string>();
    return "";
}
json rank(const char* cat, const json& orig, const json& cand) {
    kelvin::crossref::Options opt;
    auto r = kelvin::crossref::cross_reference(cat, orig, json::array({cand}), opt);
    return r["candidates"].at(0);
}
}  // namespace

// 1. A common-mode choke's impedancePoints hold its common-mode AND its differential-mode
// curve in one array, each point tagged by `winding`. The extractor ignored the tag and read
// one merged curve: for 744290130 (Würth WE-CMBNC) the largest sample of the merged array is
// the DM point at 1 GHz (214 ohm, the last sample, so no bracketed peak at all), and its real
// common-mode peak, 132.81 ohm at 870.96 MHz, was lost. 185 records had a DM point as the
// merged maximum (survey of data-524a699).
TEST_CASE("curves: a CMC's impedance readings are taken per winding mode",
          "[magcurves][views][cmc]") {
    const auto r = row_of("744290130");
    // common mode: bracketed peak at 870.96 MHz; the sample nearest 100 MHz is 95.499 MHz
    CHECK(r.impedance_peak == Approx(132.81));
    CHECK(r.impedance_peak_freq == Approx(870.96e6));
    CHECK(r.impedance_100mhz == Approx(52.61));
    // differential mode: 30.067 ohm at 95.499 MHz; its maximum is its last sample (1 GHz),
    // which is where the measurement stopped, not a peak
    CHECK(r.dm_impedance_100mhz == Approx(30.067));
    CHECK(std::isnan(r.dm_impedance_peak));
    CHECK(std::isnan(r.dm_impedance_peak_freq));
}

// 2. An inductor's |Z|(f) curve (8,062 Murata / Würth / TDK records) was extracted into
// Z@100M / peak readings the inductor model never compared. Now the curves are compared
// point by point over the band both measured, log-log, with the chip-bead impedance spec's
// bands (the one impedance spec Kelvin has: PASS >= 0.85x, WARN >= 0.8x).
TEST_CASE("curves: an inductor's |Z|(f) curve is compared with the other's",
          "[magcurves][crossref][impedance]") {
    const auto o = row_of("LQG15HN10NJ02"), s = row_of("LQW15AN10NJ00");
    REQUIRE(o.z_curve.x->size() == 300);
    REQUIRE(s.z_curve.x->size() == 300);
    const json c = rank("magnetic", spec_of(o), spec_of(s));
    INFO(c.dump());
    // Both 10 nH 0402 parts are swept 10 MHz-3 GHz; the wirewound LQW15AN sits at 217 ohm at
    // 3 GHz against the multilayer LQG15HN's 379 ohm (0.57x): FAIL, demoted, named.
    CHECK(verdict_of(c, "impedance_curve") == "fail");
    const std::string note = note_with(c, "|Z|(f) compared on both curves");
    CHECK(note.find("over 10 MHz-3 GHz") != std::string::npos);
    CHECK(note.find("worst at 3 GHz, substitute 217 ohm vs original 379 ohm") !=
          std::string::npos);
    CHECK(c["status"] == "partial");
}

// 3. A bead's resistancePoints / reactancePoints (2,691 records carry both) were never
// compared. R(f) — the dissipative part that suppresses the noise — and X(f) on the band both
// curves are inductive are now compared with the bead impedance spec's bands, linear ohms over
// log frequency.
TEST_CASE("curves: a bead's R(f) and X(f) are compared with the other's",
          "[magcurves][crossref][bead]") {
    const auto o = row_of("KMZ1608YHR301BTD25"), s = row_of("MMZ1608B301CTAH0");
    REQUIRE(o.r_curve.x->size() >= 2);
    REQUIRE(s.x_curve.x->size() >= 2);
    const json c = rank("chipBead", spec_of(o, true), spec_of(s, true));
    INFO(c.dump());
    // Two TDK 0603 300 ohm @ 100 MHz beads, both swept 1 MHz-3 GHz with a SIGNED reactance
    // (X < 0 above resonance). The original KMZ1608YHR301B is a sharp high-speed bead (R
    // peaking near 477 ohm around 300 MHz); the broadband MMZ1608B301C is resistive early and
    // flat, so at 345 MHz it dissipates 321 ohm against 477 ohm (0.67x): R(f) FAIL.
    CHECK(verdict_of(c, "resistance_curve") == "fail");
    const std::string rn = note_with(c, "R(f) compared on both curves");
    CHECK(rn.find("worst at 345 MHz, substitute 321 ohm vs original 477 ohm") !=
          std::string::npos);
    // X(f) is judged on the band both are inductive (the substitute turns capacitive first,
    // at ~222 MHz), where its reactance has fallen to 1.26 ohm against the original's 97.5.
    CHECK(verdict_of(c, "reactance_curve") == "fail");
    const std::string xn = note_with(c, "X(f) compared on both curves");
    CHECK(xn.find("over 1 MHz-222 MHz") != std::string::npos);
    CHECK(xn.find("inductive band X > 0 only") != std::string::npos);
    CHECK(c["status"] == "partial");
}

// 4. Owner 2026-11-14: impedance-type curves are judged only in the useful band — from the
// original's rating frequency (none is stated on a magnetic record: 1 MHz) up to its
// self-resonance / impedance peak. Two Murata 4.7 uH 1210 wirewound inductors, neither
// stating an SRF, both swept to GHz: LQW32FT4R7M0H peaks at 258 MHz; above its resonance it
// is a capacitor, and there LQW32FT4R7M8H's |Z| is 0.43x (1.35 GHz). Those points are shown
// in the note but not graded; below the peak the substitute stays above 0.85x (PASS).
TEST_CASE("curves: |Z|(f) is graded only up to the original's impedance peak",
          "[magcurves][crossref][impedance][band]") {
    const auto o = row_of("LQW32FT4R7M0H"), s = row_of("LQW32FT4R7M8H");
    REQUIRE(std::isnan(o.srf));
    const json c = rank("magnetic", spec_of(o), spec_of(s));
    INFO(c.dump());
    CHECK(verdict_of(c, "impedance_curve") == "pass");
    const std::string note = note_with(c, "|Z|(f) compared on both curves");
    CHECK(note.find("useful band 1 MHz-258 MHz") != std::string::npos);
    CHECK(note.find("to the original's |Z| peak") != std::string::npos);
}
