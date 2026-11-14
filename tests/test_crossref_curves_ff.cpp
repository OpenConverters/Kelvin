// Owner curve rule (2026-11-14, "make sure that Kelvin compares ALL existing graphs"): the
// MOSFET, diode and connector curves, on real TAS records (data 524a699) in
// tests/fixtures/curves_ff_*.ndjson, indexed by the real shard builder, read back through
// browse_rows (the row the web receives) and turned into the spec as web/src/crossref.js does.
#include <iostream>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "Browse.hpp"
#include "CrossRef.hpp"
#include "Index.hpp"

using nlohmann::json;

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
json nz(const json& v) { return v.is_number() && v.get<double>() > 0 ? v : json(nullptr); }
json raw(const json& v) { return v.is_null() ? json(nullptr) : v; }
std::string str_or_null(const json& r, const char* k) { return r.value(k, ""); }
json base(const json& r) {
    return json{{"mpn", r["mpn"]},
                {"length_m", nz(r["lengthM"])},
                {"width_m", nz(r["widthM"])},
                {"height_m", nz(r["heightM"])},
                {"case_code", r.value("caseCode", "")},
                {"mount", r.value("mount", "")},
                {"lead_pitch_m", r.contains("leadPitchM") ? nz(r["leadPitchM"]) : json(nullptr)},
                {"is_production", r["is_production"]},
                {"provenance_retracted", r.value("provenanceRetracted", false)}};
}
json opt_str(const json& r, const char* k) {
    const std::string s = r.value(k, "");
    return s.empty() ? json(nullptr) : json(s);
}
// web/src/crossref.js MOSFET spec, curves included (v22).
json mosfet_spec(const json& r) {
    json s = base(r);
    s["vds"] = nz(r["vds_rated"]);
    s["id"] = nz(r["id_continuous"]);
    s["rds_on"] = nz(r["rds_on"]);
    s["qg"] = nz(r["qg_total"]);
    s["coss"] = nz(r["coss"]);
    s["coss_vds"] = nz(r["coss_vds"]);
    s["vgs_threshold_max"] = nz(r["vgs_threshold_max"]);
    s["vgs_threshold_typ"] = nz(r["vgs_threshold_typ"]);
    s["rds_on_vgs"] = nz(r["rds_on_vgs"]);
    s["vgs_max"] = nz(r["vgs_max"]);
    s["technology"] = r.value("technology", "");
    s["qualification"] = r.value("qualification", "");
    s["channel"] = opt_str(r, "channel");
    s["die_configuration"] = opt_str(r, "die_configuration");
    s["internal_connection"] = opt_str(r, "internal_connection");
    for (const char* k : {"ciss_curve", "coss_curve", "crss_curve", "rds_on_id_curve",
                          "rds_on_tj_curve", "qg_curve", "id_ambient_curves"})
        s[k] = raw(r[k]);
    return s;
}
json raw_or_null(const json& r, const char* k) {
    return r.contains(k) && !r[k].is_null() ? r[k] : json(nullptr);
}
json str_or_null_json(const json& r, const char* k) {
    const std::string v = r.contains(k) && r[k].is_string() ? r[k].get<std::string>() : "";
    return v.empty() ? json(nullptr) : json(v);
}
// web/src/crossref.js diode spec, curves included (v22).
json diode_spec(const json& r) {
    json s = base(r);
    s["vrrm"] = nz(r["vrrm_rated"]);
    s["if_avg"] = nz(r["if_avg_rated"]);
    s["vf"] = nz(r["vf_typ"]);
    s["power_dissipation"] = nz(r["power_dissipation"]);
    s["qrr"] = raw_or_null(r, "qrr");
    s["trr"] = raw_or_null(r, "trr");
    s["technology"] = r.value("technology", "");
    s["die_configuration"] = str_or_null_json(r, "die_configuration");
    s["internal_connection"] = str_or_null_json(r, "internal_connection");
    s["number_of_elements"] = nz(r["number_of_elements"]);
    s["vz_min_V"] = nz(r["vz_min"]);
    s["vz_max_V"] = nz(r["vz_max"]);
    s["vz_tolerance_pct"] = r.contains("vz_tolerance") && r["vz_tolerance"].is_number()
                                ? json(r["vz_tolerance"].get<double>() * 100)
                                : json(nullptr);
    s["clamping_voltage"] = nz(r["clamping_voltage"]);
    s["clamping_current"] = nz(r["clamping_current"]);
    s["peak_pulse_power"] = nz(r["peak_pulse_power"]);
    s["peak_pulse_current"] = nz(r["peak_pulse_current"]);
    s["junction_capacitance"] = nz(r["junction_capacitance"]);
    s["junction_capacitance_vr"] = raw_or_null(r, "junction_capacitance_vr");
    s["vf_test_current"] = nz(r["vf_test_current"]);
    for (const char* k : {"vf_curve", "ir_curve", "cj_curve"}) s[k] = raw(r[k]);
    s["clamping_waveform"] = r.value("clamping_waveform", "");
    s["peak_pulse_power_waveform"] = r.value("peak_pulse_power_waveform", "");
    s["peak_pulse_current_waveform"] = r.value("peak_pulse_current_waveform", "");
    s["tj_max"] = raw_or_null(r, "tj_max");
    s["qualification"] = r.value("qualification", "");
    return s;
}
// web/src/crossref.js connector spec, the derating curve included (v22).
json connector_spec(const json& r) {
    json s = base(r);
    s["family"] = r.value("family", "");
    s["positions"] = nz(r["positions"]);
    s["polarity"] = str_or_null_json(r, "polarity");
    s["interface_standard"] = str_or_null_json(r, "interface_standard");
    s["pitch_mm"] = r.contains("pitch") && r["pitch"].is_number() && r["pitch"].get<double>() > 0
                        ? json(r["pitch"].get<double>() * 1e3)
                        : json(nullptr);
    s["rows"] = nz(r["rows"]);
    s["rated_current_A"] = nz(r["rated_current"]);
    s["rated_voltage_V"] = nz(r["rated_voltage"]);
    s["current_ambient_curve"] = raw(r["current_ambient_curve"]);
    s["temp_min_C"] = raw_or_null(r, "temp_min_c");
    s["temp_max_C"] = raw_or_null(r, "temp_max_c");
    s["contact_plating"] = str_or_null_json(r, "contact_plating");
    s["termination"] = str_or_null_json(r, "termination");
    s["mounting"] = str_or_null_json(r, "mount");
    s["orientation"] = str_or_null_json(r, "orientation");
    const std::string pl = r.value("pluggable", "");
    s["pluggable"] = pl == "true" ? json(true) : pl == "false" ? json(false) : json(nullptr);
    s["actuator_type"] = str_or_null_json(r, "actuator_type");
    s["shell_size"] = str_or_null_json(r, "shell_size");
    s["coding"] = str_or_null_json(r, "coding");
    s["coupling_type"] = str_or_null_json(r, "coupling_type");
    s["mating_cycles"] = nz(r["mating_cycles"]);
    return s;
}
std::string fixture(const char* f) {
    return std::string(KELVIN_TEST_DIR) + "/fixtures/curves_ff_" + f;
}
const json& row(const std::string& family, const std::string& mpn) {
    static const json mos = [] {
        auto sh = kelvin::build_mosfet_shard(fixture("mosfets.ndjson"));
        return kelvin::browse::browse_rows(sh, json{{"limit", 1000}})["rows"];
    }();
    static const json dio = [] {
        auto sh = kelvin::build_diode_shard(fixture("diodes.ndjson"));
        return kelvin::browse::browse_rows(sh, json{{"limit", 1000}})["rows"];
    }();
    static const json con = [] {
        auto sh = kelvin::build_connector_shard(fixture("connectors.ndjson"));
        return kelvin::browse::browse_rows(sh, json{{"limit", 1000}})["rows"];
    }();
    const json* rows = family == "mosfet" ? &mos : family == "diode" ? &dio : &con;
    for (const auto& r : *rows)
        if (r.value("mpn", "") == mpn) return r;
    throw std::runtime_error("fixture has no " + family + " record " + mpn);
}
json spec(const std::string& family, const std::string& mpn) {
    if (family == "diode") return diode_spec(row(family, mpn));
    if (family == "connector") return connector_spec(row(family, mpn));
    return mosfet_spec(row(family, mpn));
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
}  // namespace

// 1. MOSFET C(Vds) curves. IPD130N10NF2S and IPD052N10NF2S / ISG0616N10NM5HSC (Infineon 100 V,
// all three curves at f 1 MHz, Vgs 0 V, 0..82 V) — the larger dies' capacitances are ~2.7x
// over the whole range; IPD95R2K0P7's are measured at 250 kHz (different conditions). And the
// Coss basis (r11 seed 24240811): AOD4N60 states 53 pF at 25 V, IPD95R2K0P7 5 pF at no Vds —
// its Coss(Vds) curve reads 72.65 pF at 25 V, so Coss is compared there (1.37x: warn).
TEST_CASE("curves ff 1: MOSFET Ciss/Coss/Crss(Vds) curves compared; a Coss curve resolves the "
          "Vds basis", "[crossref][curves][curves-ff]") {
    const json r = rank("mosfet", "IPD130N10NF2S",
                        {"IPD052N10NF2S", "ISG0616N10NM5HSC", "IPD95R2K0P7"});
    const json big = find(r, "IPD052N10NF2S");
    REQUIRE(!big.is_null());
    CHECK(verdict_of(big, "ciss_vs_vds") == "fail");
    CHECK(verdict_of(big, "coss_vs_vds") == "fail");
    CHECK(verdict_of(big, "crss_vs_vds") == "fail");
    CHECK(has_note(big, "output capacitance against Vds (Coss(Vds)): compared on both curves over "
                        "0 V to 82 V (30 points, linear interpolation, conditions f 1 MHz, Vgs 0 V); "
                        "worst at 82 V: substitute 412.7 pF against the original's 151.8 pF (fail"));
    CHECK(big["grade"] != "drop_in");
    const json other = find(r, "IPD95R2K0P7");
    REQUIRE(!other.is_null());
    CHECK(verdict_of(other, "coss_vs_vds") == "unverified");
    CHECK(has_note(other, "output capacitance against Vds (Coss(Vds)): measured under different "
                          "conditions (original: f 1 MHz, Vgs 0 V; substitute: f 250 kHz, Vgs 0 V)"));
    // The other way round the substitute's capacitances are lower everywhere: pass.
    const json small = find(rank("mosfet", "IPD052N10NF2S", {"IPD130N10NF2S"}), "IPD130N10NF2S");
    REQUIRE(!small.is_null());
    CHECK(verdict_of(small, "coss_vs_vds") == "pass");
    CHECK(verdict_of(small, "ciss_vs_vds") == "pass");
    CHECK(verdict_of(small, "crss_vs_vds") == "pass");

    // Coss basis resolved on the substitute's curve at the original's 25 V.
    const json res = find(rank("mosfet", "AOD4N60", {"IPD95R2K0P7"}), "IPD95R2K0P7");
    REQUIRE(!res.is_null());
    CHECK(verdict_of(res, "coss") == "warn");
    CHECK(verdict_of(res, "coss_basis") == "");
    CHECK(has_note(res, "output capacitance compared at the original's 25 V: the substitute's "
                        "Coss(Vds) curve reads 72.65 pF there (its record states 5 pF at no "
                        "stated Vds) against the original's 53 pF"));
}

// 2. MOSFET Rds(on) curves. BSC034N06NS (3.4 mohm class) and BSC066N06NS (6.6 mohm class),
// Infineon 60 V, both stating Rds(on)(Id) at Vgs 10 V / Tj 25 degC and Rds(on)(Tj) at Id 50 A /
// Vgs 10 V. The larger-Rds part as the substitute is ~1.9x over the whole range: beyond the
// Rds(on) spec's 1.5x -> fail on both curves; the other way round it passes.
TEST_CASE("curves ff 2: MOSFET Rds(on)(Id) and Rds(on)(Tj) curves compared",
          "[crossref][curves][curves-ff]") {
    const json worse = find(rank("mosfet", "BSC034N06NS", {"BSC066N06NS"}), "BSC066N06NS");
    REQUIRE(!worse.is_null());
    CHECK(verdict_of(worse, "rds_on_vs_id") == "fail");
    CHECK(verdict_of(worse, "rds_on_vs_tj") == "fail");
    CHECK(has_note(worse, "on-resistance against drain current (Rds(on)(Id)): compared on both "
                          "curves over 14 A to 250 A"));
    CHECK(has_note(worse, "on-resistance against junction temperature (Rds(on)(Tj)): compared on "
                          "both curves over -55 degC to 141.1 degC"));
    const json better = find(rank("mosfet", "BSC066N06NS", {"BSC034N06NS"}), "BSC034N06NS");
    REQUIRE(!better.is_null());
    CHECK(verdict_of(better, "rds_on_vs_id") == "pass");
    CHECK(verdict_of(better, "rds_on_vs_tj") == "pass");
}

// 3. MOSFET gate-charge curve, read as the charge to reach each Vgs and graded only ABOVE the
// higher of the two Miller plateaus (owner 2026-11-14: the region that sets the charge at the
// drive voltage; the plateaus are named in the note, not graded). BSC066N06NS (plateau 5.168 V,
// 7.22 to 9.597 nC; 17.6 nC to ~10 V) against BSC034N06NS (plateau 4.714 V, 12.26 to 16.8 nC;
// 33.9 nC): above 5.168 V the substitute needs up to 1.94x the charge (31.64 against 16.35 nC
// at 9.408 V) -> warn (the Qg spec's 2x). Before the plateau rule the worst point was ON the
// plateau (4.737 V: 17.95 against 6.084 nC, 2.95x -> fail). BSC012N06NS (116 nC) is ~6.7x
// above the plateau -> fail; the reverse of the first pair passes.
TEST_CASE("curves ff 3: MOSFET gate-charge curves compared as charge to reach Vgs",
          "[crossref][curves][curves-ff]") {
    const json r = rank("mosfet", "BSC066N06NS", {"BSC034N06NS", "BSC012N06NS"});
    const json more = find(r, "BSC034N06NS");
    REQUIRE(!more.is_null());
    CHECK(verdict_of(more, "qg_vs_vgs") == "warn");
    CHECK(has_note(more, "gate charge to reach Vgs (Qg(Vgs), from the gate-charge curve): "
                         "compared on both curves over 5.533 V to 9.988 V"));
    CHECK(has_note(more, "worst at 9.408 V: substitute 31.64 nC against the original's 16.35 nC"));
    CHECK(has_note(more, "(graded above 5.168 V only; Miller plateaus: original's at 5.168 V "
                         "(5.15 to 5.168 V) from 7.22 to 9.597 nC, substitute's at 4.714 V "
                         "(4.696 to 4.714 V) from 12.26 to 16.8 nC, not graded)"));
    const json big = find(r, "BSC012N06NS");
    REQUIRE(!big.is_null());
    CHECK(verdict_of(big, "qg_vs_vgs") == "fail");
    const json less = find(rank("mosfet", "BSC034N06NS", {"BSC066N06NS"}), "BSC066N06NS");
    REQUIRE(!less.is_null());
    CHECK(verdict_of(less, "qg_vs_vgs") == "pass");
}

TEST_CASE("curves ff 4: MOSFET Id against ambient temperature compared under common conditions",
          "[crossref][curves][curves-ff]") {
    const json r = rank("mosfet", "NX7002BKS", {"NX138BKS", "NX138BK"});
    const json lower = find(r, "NX138BKS");
    REQUIRE(!lower.is_null());
    CHECK(verdict_of(lower, "id_vs_ambient") == "warn");
    CHECK(verdict_of(lower, "id") == "pass");
    CHECK(has_note(lower, "drain current rating against ambient temperature (Id(Ta)): compared on "
                          "both curves over 25 degC to 100 degC"));
    const json higher = find(rank("mosfet", "NX138BKS", {"NX7002BKS"}), "NX7002BKS");
    REQUIRE(!higher.is_null());
    CHECK(verdict_of(higher, "id_vs_ambient") == "pass");
    const json other = find(r, "NX138BK");
    REQUIRE(!other.is_null());
    CHECK(verdict_of(other, "id_vs_ambient") == "unverified");
    CHECK(has_note(other, "drain current rating against ambient temperature (Id(Ta)): no rating "
                          "of the substitute is stated under the conditions of one of the "
                          "original's"));
    const json dual = find(rank("mosfet", "AO4832", {"AO4822"}), "AO4822");
    REQUIRE(!dual.is_null());
    CHECK(verdict_of(dual, "id_vs_ambient") == "warn");
    CHECK(verdict_of(dual, "id") != "warn");  // the scalar Id is not read off the curve
    CHECK(verdict_of(dual, "id") != "pass");
}

// 5. Diode Vf(If) curves (log-log, judged with the Vf spec: lower is better, 1.2x), and the Vf
// basis resolved on a 25 degC curve. Vishay 60 V Schottkys, every curve at 25 degC unless named:
//  * VT30L60C-M3/4W -> V30D60CLHM3_A/I: up to 1.10x at 40.9 A -> warn; the reverse passes.
//  * V30D60CLHM3_A/I (0.61 V at 15 A) -> V40PW60C-M3/I (0.68 V at 20 A): its curve reads
//    0.5565 V at 15 A -> the scalar Vf passes on the original's current (no vf_basis row).
//  * -> V40DM60C-M3/I (0.65 V at 20 A, its curve at 125 degC): the curves are UNVERIFIED
//    (different temperature); the basis is settled on the ORIGINAL's 25 degC curve, 0.5791 V at
//    20 A, against 0.65 V (1.12x) -> warn.
//  * -> STPS30L60CT: its curve carries no axis labels; its own Vf (0.42 V at 15 A) lies on it,
//    so it is read as SI (owner 2026-11-14) — but it states no temperature where the
//    original's states 25 degC: different conditions -> UNVERIFIED.
TEST_CASE("curves ff 5: diode Vf(If) curves compared; a 25 degC curve resolves the Vf basis",
          "[crossref][curves][curves-ff]") {
    const json up = find(rank("diode", "VT30L60C-M3/4W", {"V30D60CLHM3_A/I"}), "V30D60CLHM3_A/I");
    REQUIRE(!up.is_null());
    CHECK(verdict_of(up, "vf_vs_if") == "warn");
    CHECK(has_note(up, "forward voltage against forward current (Vf(If)): compared on both curves "
                       "over 0.1028 A to 51.16 A"));
    const json r = rank("diode", "V30D60CLHM3_A/I",
                        {"VT30L60C-M3/4W", "V40PW60C-M3/I", "V40DM60C-M3/I", "STPS30L60CT"});
    const json down = find(r, "VT30L60C-M3/4W");
    REQUIRE(!down.is_null());
    CHECK(verdict_of(down, "vf_vs_if") == "pass");
    const json pw = find(r, "V40PW60C-M3/I");
    REQUIRE(!pw.is_null());
    CHECK(verdict_of(pw, "vf") == "pass");
    CHECK(verdict_of(pw, "vf_basis") == "");
    CHECK(has_note(pw, "forward voltage compared at the original's 15 A: the substitute's 25 degC "
                       "Vf(If) curve reads 0.5565 V there"));
    const json dm = find(r, "V40DM60C-M3/I");
    REQUIRE(!dm.is_null());
    CHECK(verdict_of(dm, "vf_vs_if") == "unverified");
    CHECK(verdict_of(dm, "vf") == "warn");
    CHECK(verdict_of(dm, "vf_basis") == "");
    CHECK(has_note(dm, "forward voltage compared at the substitute's 20 A: the original's 25 degC "
                       "Vf(If) curve reads 0.5791 V there"));
    const json st = find(r, "STPS30L60CT");
    REQUIRE(!st.is_null());
    CHECK(verdict_of(st, "vf_vs_if") == "unverified");
    CHECK(has_note(st, "forward voltage against forward current (Vf(If)): measured under "
                       "different conditions (original: T 25 degC; substitute: none stated)"));
    CHECK(has_note(st, "read as SI because its record's own Vf of 0.42 V at 15 A lies on it"));
}

// 6. Diode Ir(Vr) curves (judged with the quiescent-current spec, borrowed: lower is better, 3x)
// and Cj(Vr) curves (the junction-capacitance spec, 2x), both linear in Vr.
//  * SS10P4CHM3_A/H -> SS8P3CLHM3_A/H (25 degC both): up to 1.76x -> warn; -> SS12P3LHM3_A/H:
//    5.5x at 30 V -> fail; -> SS6P4CHM3_A/H (its curve at 125 degC) -> UNVERIFIED.
//  * VS-3C40CP07L-M3 -> VS-3C20ET07T-M3: the same Cj(Vr) curve digitised twice, never above the
//    original's -> pass. The other way round the substitute is 1001 pF against 1000 pF at
//    0.31 V: within 2 % of the original's, level -> pass (owner 2026-11-14; it was a warn).
//  * Cj basis: STPS30L60CT (1.5 nF at 5 V) -> VS-3C20ET07T-M3 (240 pF at no stated bias): its
//    Cj(Vr) curve reads 537.9 pF at 5 V -> pass. C3D10065A (280 pF at 0 V) -> VS-3C20ET07T-M3:
//    its curve starts at 0.1 V, so 0 V is outside it -> never extrapolated, the basis stays
//    UNVERIFIED.
TEST_CASE("curves ff 6: diode Ir(Vr) and Cj(Vr) curves compared; a Cj curve resolves the bias",
          "[crossref][curves][curves-ff]") {
    const json r = rank("diode", "SS10P4CHM3_A/H",
                        {"SS8P3CLHM3_A/H", "SS12P3LHM3_A/H", "SS6P4CHM3_A/H"});
    const json a = find(r, "SS8P3CLHM3_A/H");
    REQUIRE(!a.is_null());
    CHECK(verdict_of(a, "ir_vs_vr") == "warn");
    CHECK(has_note(a, "reverse current against reverse voltage (Ir(Vr)): compared on both curves "
                      "over 4.013 V to 30.01 V"));
    const json b = find(r, "SS12P3LHM3_A/H");
    REQUIRE(!b.is_null());
    CHECK(verdict_of(b, "ir_vs_vr") == "fail");
    const json hot = find(r, "SS6P4CHM3_A/H");
    REQUIRE(!hot.is_null());
    CHECK(verdict_of(hot, "ir_vs_vr") == "unverified");
    CHECK(has_note(hot, "measured under different conditions (original: T 25 degC; substitute: "
                        "T 125 degC)"));
    const json cj = find(rank("diode", "VS-3C40CP07L-M3", {"VS-3C20ET07T-M3"}), "VS-3C20ET07T-M3");
    REQUIRE(!cj.is_null());
    CHECK(verdict_of(cj, "cj_vs_vr") == "pass");
    const json cj2 = find(rank("diode", "VS-3C20ET07T-M3", {"VS-3C40CP07L-M3"}), "VS-3C40CP07L-M3");
    REQUIRE(!cj2.is_null());
    CHECK(verdict_of(cj2, "cj_vs_vr") == "pass");
    CHECK(has_note(cj2, "closest to the original's limit at 0.3093 V: substitute 1001 pF against "
                        "the original's 1000 pF (pass"));
    const json bias = find(rank("diode", "STPS30L60CT", {"VS-3C20ET07T-M3"}), "VS-3C20ET07T-M3");
    REQUIRE(!bias.is_null());
    CHECK(verdict_of(bias, "junction_capacitance") == "pass");
    CHECK(verdict_of(bias, "junction_capacitance_basis") == "");
    CHECK(has_note(bias, "junction capacitance compared at the original's 5 V: the substitute's "
                         "Cj(Vr) curve reads 537.9 pF there"));
    const json zero = find(rank("diode", "C3D10065A", {"VS-3C20ET07T-M3"}), "VS-3C20ET07T-M3");
    REQUIRE(!zero.is_null());
    CHECK(verdict_of(zero, "junction_capacitance_basis") == "unverified");
}

// 7. Connector per-contact current derating (derating.currentVsAmbient, a FRACTION of the rated
// current): each side's curve scaled by its own rated current, judged with the rated-current
// spec (higher is better, WARN down to 0.9x). TE Economy Power 2.5 PCB headers, 4 positions,
// all on the same derating shape at a 30 K contact rise: right-angle 1744426-4 (4.5 A) ->
// 1744446-4 (4.2 A) 0.93x -> warn; vertical 1744439-4 (4.2 A) -> 1744418-4 (3.0 A) 0.71x ->
// fail; the reverse passes.
TEST_CASE("curves ff 7: connector current derating curves compared in amperes",
          "[crossref][curves][curves-ff]") {
    const json w = find(rank("connector", "1744426-4", {"1744446-4"}), "1744446-4");
    REQUIRE(!w.is_null());
    CHECK(verdict_of(w, "current_vs_ambient") == "warn");
    CHECK(has_note(w, "per-contact current rating against ambient temperature (derating curve x "
                      "rated current): compared on both curves over 0 degC to 106.5 degC"));
    const json f = find(rank("connector", "1744439-4", {"1744418-4"}), "1744418-4");
    REQUIRE(!f.is_null());
    CHECK(verdict_of(f, "current_vs_ambient") == "fail");
    const json p = find(rank("connector", "1744418-4", {"1744439-4"}), "1744439-4");
    REQUIRE(!p.is_null());
    CHECK(verdict_of(p, "current_vs_ambient") == "pass");
}

// 8. Curves without axis units (no xLabel / yLabel; owner 2026-11-14): read as SI when the
// record's own scalar lies on the curve within 2 % at the scalar's stated condition, with a note;
// otherwise not read (UNVERIFIED + note). Real records (data 524a699):
//  * STPS30L60CT (Vf 0.42 V at 15 A, Cj 1.5 nF at 5 V) and C3D10065A (Vf 1.5 V at 10 A, Cj
//    280 pF at 0 V): both scalars lie on their curves -> read. Vf(If) 0.5 to 20 A: C3D10065A
//    1.65 V at 15 A against 0.42 V -> fail; and the Cj basis (5 V against 0 V) is settled on
//    C3D10065A's curve: 215 pF at 5 V against 1.5 nF -> pass.
//  * C3D10065A -> C4D10120A (Cj 500 pF at 0 V, on its curve): Cj(Vr) 500 against 280 pF at
//    0 V -> warn.
//  * IPB017N10N5: Coss 1810 pF at 50 V, its unlabelled Coss(Vds) curve reads 1500 pF there
//    (17 % off) -> not confirmed, not read; its unlabelled Rds(on)(Tj) has no scalar to check
//    (the scalar Rds(on) states no junction temperature) -> not read.
TEST_CASE("curves ff 8: unlabelled curves read as SI when the record's own scalar lies on them",
          "[crossref][curves][curves-ff]") {
    const json c3 = find(rank("diode", "STPS30L60CT", {"C3D10065A"}), "C3D10065A");
    REQUIRE(!c3.is_null());
    CHECK(verdict_of(c3, "vf_vs_if") == "fail");
    CHECK(has_note(c3, "forward voltage against forward current (Vf(If)): compared on both curves "
                       "over 0.5 A to 20 A (7 points, log-log interpolation, conditions none "
                       "stated); worst at 15 A: substitute 1.65 V against the original's 0.42 V"));
    CHECK(has_note(c3, "the original's curve: its axes state no units; read as SI because its "
                       "record's own Vf of 0.42 V at 15 A lies on it (0.42 V read there) (units "
                       "inferred from the record's scalar)"));
    CHECK(verdict_of(c3, "junction_capacitance") == "pass");
    CHECK(verdict_of(c3, "junction_capacitance_basis") == "");
    CHECK(has_note(c3, "junction capacitance compared at the original's 5 V: the substitute's "
                       "Cj(Vr) curve reads 215 pF there"));
    CHECK(has_note(c3, "read as SI because its record's own junction capacitance of 280 pF at "
                       "0 V lies on it"));
    const json c4 = find(rank("diode", "C3D10065A", {"C4D10120A"}), "C4D10120A");
    REQUIRE(!c4.is_null());
    CHECK(verdict_of(c4, "cj_vs_vr") == "warn");
    CHECK(has_note(c4, "worst at 0 V: substitute 500 pF against the original's 280 pF"));

    const json& ipb = row("mosfet", "IPB017N10N5");
    CHECK(ipb["coss_curve"].value("unit_unstated", false));
    CHECK(ipb["rds_on_tj_curve"].value("unit_unstated", false));
    CHECK(ipb["rds_on_tj_curve"].value("unit_note", "").find("no scalar of its record") !=
          std::string::npos);
    const json m = find(rank("mosfet", "IPD052N10NF2S", {"IPB017N10N5"}), "IPB017N10N5");
    REQUIRE(!m.is_null());
    CHECK(verdict_of(m, "coss_vs_vds") == "unverified");
    CHECK(has_note(m, "the substitute's curve: its record's Coss of 1810 pF at 50 V does not lie "
                      "on it read as SI (1500 pF there), so the units are not confirmed"));
}
