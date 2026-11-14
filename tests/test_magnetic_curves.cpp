// Magnetic curves compared under the owner's curve rule (2026-11-14: "make sure that Kelvin
// compares ALL existing graphs"). Every record used here is a REAL TAS magnetics.ndjson line
// (data-524a699, distributorsInfo dropped) kept in tests/fixtures/magnetic_curves.ndjson, so
// each test runs the extractor on what the catalogue states, then the ranker on the spec
// block web/src/crossref.js builds from the browse row.
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

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
const char* const kCurveKeys[] = {"impedance_curve",         "resistance_curve",
                                  "reactance_curve",         "dm_impedance_curve",
                                  "cm_insertion_loss_curve", "dm_insertion_loss_curve"};
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
           {"temp_min_C", raw(r.temp_min_c)}, {"temp_max_C", raw(r.temp_max_c)},
           {"device_type", r.device_type},
           {"rated_current_points", row["rated_current_points"]}};
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
        s["inductance_curves"] = row["inductance_curves"];
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

// 5. A common-mode choke's other curves (owner 2026-11-14): differential-mode |Z|(f) (397
// records) and CM / DM insertion loss (310 / 264 records) were never compared. CM: higher is
// better. DM: LOWER is better (signal-line selection practice; the note says so and names the
// power-line exception). Each is graded only in the original's useful band, closed by its
// |Z| peak of the same mode; for a common-mode choke the band opens at 150 kHz (owner
// 2026-11-14, the bottom of the CISPR conducted range). Insertion loss PASSES within 1 dB of
// the original's (owner 2026-11-14), the CMRR spec's 6 dB band beyond it is the WARN band.
// Two Würth 47 uH THT power-line CMCs: the 2 A WE-CMB NiZn 744841247 (CM |Z| peaking near
// 29 MHz) and the 15 A WE-ExB 744844470.
TEST_CASE("curves: a CMC's DM impedance and CM / DM insertion loss are compared",
          "[magcurves][crossref][cmc]") {
    const auto o = row_of("744841247"), s = row_of("744844470");
    REQUIRE_FALSE(o.dm_z_curve.empty());
    REQUIRE_FALSE(s.cm_il_curve.empty());
    REQUIRE_FALSE(s.dm_il_curve.empty());
    const json c = rank("magnetic", spec_of(o), spec_of(s));
    INFO(c.dump());
    // CM |Z| (bead impedance bands) and CM insertion loss (the CMRR spec, higher is better):
    // at the original's 29 MHz CM peak the substitute blocks 13.7 dB against 36.6 dB.
    CHECK(verdict_of(c, "impedance_curve") == "fail");
    CHECK(verdict_of(c, "cm_insertion_loss_curve") == "fail");
    const std::string cm = note_with(c, "CM insertion loss(f) compared on both curves");
    CHECK(cm.find("useful band 150 kHz-29 MHz") != std::string::npos);
    CHECK(cm.find("worst at 29 MHz, substitute 13.7 dB vs original 36.6 dB") !=
          std::string::npos);
    // DM |Z|: the substitute's is lower everywhere in band (1.81 vs 3.32 ohm at 150 kHz): PASS.
    CHECK(verdict_of(c, "dm_impedance_curve") == "pass");
    const std::string dz = note_with(c, "DM |Z|(f) compared on both curves");
    CHECK(dz.find("to the original's differential-mode |Z| peak") != std::string::npos);
    CHECK(dz.find("lower DM impedance / insertion loss is ranked better") != std::string::npos);
    // DM insertion loss: the substitute's is higher, at most 0.18 dB (0.251 vs 0.0685 dB at
    // 238 kHz) — within the 1 dB PASS margin. (Before the owner's 1 dB margin this pair was a
    // WARN at 1 MHz, 0.13 vs 0.118 dB.)
    CHECK(verdict_of(c, "dm_insertion_loss_curve") == "pass");
    CHECK(note_with(c, "DM insertion loss(f) compared on both curves")
              .find("worst at 238 kHz, substitute 0.251 dB vs original 0.0685 dB") !=
          std::string::npos);
}

// 6. Owner 2026-11-14: a common-mode choke's useful band opens at 150 kHz, not 1 MHz. Würth
// WE-TFC 744862100 (10 mH, 0.45 A) peaks in common mode at 537 kHz: with the 1 MHz opening its
// band closed below its own start and no curve was graded at all. Against the real power-line
// CMC WE-CMB 744821110 (10 mH, 0.7 A, CM peak 331 kHz) the CM |Z| is now graded over
// 150-537 kHz, where the substitute holds 22.2 kohm against 61 kohm at the original's peak
// (0.36x): FAIL. The DM insertion loss shows the band beyond the 1 dB margin: 24.9 vs 20.6 dB
// at 3.32 MHz, 4.3 dB the wrong way, inside the 6 dB WARN band.
TEST_CASE("curves: a CMC's curves are graded from 150 kHz", "[magcurves][crossref][cmc][band]") {
    const auto o = row_of("744862100"), s = row_of("744821110");
    REQUIRE(o.device_type == "commonModeChoke");
    REQUIRE(o.impedance_peak_freq == Approx(537e3));
    const json c = rank("magnetic", spec_of(o), spec_of(s));
    INFO(c.dump());
    CHECK(verdict_of(c, "impedance_curve") == "fail");
    const std::string z = note_with(c, "|Z|(f) compared on both curves");
    CHECK(z.find("useful band 150 kHz-537 kHz, from 150 kHz (a common-mode choke") !=
          std::string::npos);
    CHECK(z.find("worst at 537 kHz, substitute 22.2 kohm vs original 61 kohm") !=
          std::string::npos);
    CHECK(verdict_of(c, "dm_insertion_loss_curve") == "warn");
    CHECK(note_with(c, "DM insertion loss(f) compared on both curves")
              .find("worst at 3.32 MHz, substitute 24.9 dB vs original 20.6 dB") !=
          std::string::npos);
}

// 7. L(I) curves (owner 2026-11-14: "compare ALL existing graphs"): every curve a record states,
// per temperature, compared with the other's at each temperature BOTH state, the worst
// deciding. Murata MDH6045C-1R5NA states -55 / 20 / 125 / 150 degC, TDK CLF7045NIT-1R5N-D ten
// temperatures from -40 to 150 degC: compared at 20, 125 and 150 degC only (-55 and -40 are
// not read against each other). The substitute holds more inductance at every common point.
TEST_CASE("curves: L(I) curves are compared at each temperature both records state",
          "[magcurves][crossref][lcurve]") {
    const auto o = row_of("MDH6045C-1R5NA"), s = row_of("CLF7045NIT-1R5N-D");
    REQUIRE(o.l_curves.size() == 4);
    REQUIRE(s.l_curves.size() == 10);
    const json c = rank("magnetic", spec_of(o), spec_of(s));
    INFO(c.dump());
    CHECK(verdict_of(c, "inductance_curve") == "pass");
    const std::string n = note_with(c, "L(I) compared on both curves");
    CHECK(n.find("at 20 degC, 125 degC, 150 degC (linear") != std::string::npos);
    CHECK(n.find("-40") == std::string::npos);
    CHECK(n.find("-55") == std::string::npos);
}

// 8. Owner 2026-11-14: 20 degC and 25 degC are the same (room) temperature for L(I) and Isat
// curves. Würth 74438366010 states its L(I) at 20 degC, Coilcraft XAL6030-102MEC at 25 degC:
// both the L(I) curves and the Isat read off them (at the original's stated criterion) are now
// compared, and the note says the two temperatures were taken as one.
TEST_CASE("curves: 20 and 25 degC L(I) curves are compared as room temperature",
          "[magcurves][crossref][lcurve][room]") {
    const auto o = row_of("74438366010"), s = row_of("XAL6030-102MEC");
    const json c = rank("magnetic", spec_of(o), spec_of(s));
    INFO(c.dump());
    CHECK(verdict_of(c, "inductance_curve") == "pass");
    CHECK(note_with(c, "L(I) compared on both curves")
              .find("at 20 degC against 25 degC (both room temperature)") != std::string::npos);
    CHECK(note_with(c, "saturation compared on both L(I) curves at the original's stated "
                       "criterion") != "");
    CHECK(note_with(c, "L(I) curves stated at different temperatures") == "");
}

// 9. A temperature stated by one side only is a different condition (owner 2026-11-14):
// Taiyo Yuden LAXHG6060YEL1R0NMR states its L(I) with no temperature, Coilcraft
// XAL6030-102MEC at 25 degC. Neither the L(I) curves nor the Isat on them are compared:
// UNVERIFIED with a note, the stated Isat figures compared instead.
TEST_CASE("curves: an L(I) temperature stated on one side only blocks the comparison",
          "[magcurves][crossref][lcurve]") {
    const auto o = row_of("LAXHG6060YEL1R0NMR"), s = row_of("XAL6030-102MEC");
    const json c = rank("magnetic", spec_of(o), spec_of(s));
    INFO(c.dump());
    CHECK(verdict_of(c, "inductance_curve") == "unverified");
    CHECK(note_with(c, "L(I) curves not compared: stated at different temperatures (original no "
                       "stated temperature; substitute 25 degC)") != "");
    CHECK(note_with(c, "L(I) curves stated at different temperatures (original no stated "
                       "temperature, substitute 25 degC) — Isat not read off the two curves") !=
          "");
}

// 10. Rated current per temperature rise (MAS ratedCurrentPoints, owner 2026-11-14): kept on
// the row as the shared curve storage (x = rise K, y = current A, temperature = ambient),
// one point or more; not kept when the record cannot be read as one rating per rise —
// Panasonic ETQP3W1R5WFN states 6.6 A AND 11 A at 40 K, Coilcraft ME3215-333KLC 35 A at 20 K
// and 0.48 A at 40 K (a rating that falls as the allowed rise grows). The scalar stands.
TEST_CASE("curves: rated-current points are extracted per temperature rise",
          "[magcurves][views][rated]") {
    const auto x = row_of("XFL7015-102MEC");
    REQUIRE_FALSE(x.rated_points.empty());
    CHECK(*x.rated_points.x == std::vector<float>{20.f, 40.f});
    CHECK(*x.rated_points.y == std::vector<float>{7.5f, 10.5f});
    CHECK(std::isnan(x.rated_points.temperature));
    const auto one = row_of("CDMC60D28L150NP-1R0MC");
    REQUIRE(one.rated_points.x->size() == 1);
    CHECK(one.rated_points.temperature == 20.0);
    for (const char* bad : {"ETQP3W1R5WFN", "ME3215-333KLC"}) {
        const auto r = row_of(bad);
        INFO(bad);
        CHECK(r.rated_points.empty());
        CHECK(r.rated_current > 0);
    }
}

// 11. With rated-current points on both records the rating is compared at a COMMON rise, read
// between a record's two stated rises when needed (never extrapolated). TDK CLF6045T-1R0N is
// rated 4.5 A at 30 K; Coilcraft XFL7015-102MEC 7.5 A at 20 K and 10.5 A at 40 K, so at 30 K it
// carries 9 A (the largest-current scalar compared 10.5 A at 40 K instead).
TEST_CASE("curves: rated current is compared at a common temperature rise",
          "[magcurves][crossref][rated]") {
    const json c = rank("magnetic", spec_of(row_of("CLF6045T-1R0N")),
                        spec_of(row_of("XFL7015-102MEC")));
    INFO(c.dump());
    CHECK(verdict_of(c, "rated_current") == "pass");
    CHECK(note_with(c, "rated current compared at a common temperature rise: at 30 K the "
                       "substitute carries 9 A against the original's 4.5 A") != "");
}

// 12. No common rise: Coilcraft 1008CS-102XFLC is rated 0.37 A at 15 K, 1008PS-102KLB 1.4 A at
// 20 K and 2 A at 40 K. 15 K lies outside the substitute's stated rises, so nothing is
// comparable: UNVERIFIED + note (the largest-current scalar had passed it, 2 A vs 0.37 A).
TEST_CASE("curves: rated currents at different rises are not compared",
          "[magcurves][crossref][rated]") {
    const json c = rank("magnetic", spec_of(row_of("1008CS-102XFLC")),
                        spec_of(row_of("1008PS-102KLB")));
    INFO(c.dump());
    CHECK(verdict_of(c, "rated_current") == "unverified");
    CHECK(note_with(c, "rated current not compared: rated at different temperature rises "
                       "(original 370 mA at 15 K; substitute 1.4 A at 20 K, 2 A at 40 K)") != "");
}

// 13. Ambient (owner 2026-11-14): 20 and 25 degC are the same room temperature; an ambient
// stated by one side only is a different condition. Sumida CDMC60D28L150NP-1R0MC is rated
// 12.8 A at 40 K, 20 degC ambient: against Sumida CY0630AT150DS-1R0MC (13.5 A at 40 K, 25 degC)
// it is compared, against Coilcraft DO3316H-102MLD (10 A at 40 K, no stated ambient) it is not.
TEST_CASE("curves: rated current ambient — room temperature matches, one-sided does not",
          "[magcurves][crossref][rated][room]") {
    const auto o = spec_of(row_of("CDMC60D28L150NP-1R0MC"));
    const json room = rank("magnetic", o, spec_of(row_of("CY0630AT150DS-1R0MC")));
    INFO(room.dump());
    CHECK(verdict_of(room, "rated_current") == "pass");
    CHECK(note_with(room, "20 degC ambient against 25 degC ambient, both room temperature") !=
          "");
    const json one = rank("magnetic", o, spec_of(row_of("DO3316H-102MLD")));
    INFO(one.dump());
    CHECK(verdict_of(one, "rated_current") == "unverified");
    CHECK(note_with(one, "rated current not compared: rated under different ambient conditions "
                         "(original 20 degC ambient, substitute no stated ambient)") != "");
}

