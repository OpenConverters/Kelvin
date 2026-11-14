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
std::string fixture(const char* f) {
    return std::string(KELVIN_TEST_DIR) + "/fixtures/curves_ff_" + f;
}
const json& row(const std::string& family, const std::string& mpn) {
    static const json mos = [] {
        auto sh = kelvin::build_mosfet_shard(fixture("mosfets.ndjson"));
        return kelvin::browse::browse_rows(sh, json{{"limit", 1000}})["rows"];
    }();
    const json* rows = &mos;
    (void)family;
    for (const auto& r : *rows)
        if (r.value("mpn", "") == mpn) return r;
    throw std::runtime_error("fixture has no " + family + " record " + mpn);
}
json spec(const std::string& family, const std::string& mpn) {
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
