// Property / fuzz pass over REAL catalogue fixture rows, driven the same way the
// web UI drives the engine (browse row -> flat spec -> cross_reference).
//
// Every "are you happy?" probe this session found a defect the targeted unit
// tests passed straight through — the identity-key collision that disabled the
// MOSFET current check, a no-data bead ranking first, a connector rejected on a
// blank field. Those share a shape: they only surface on a real pool run end to
// end, not on a hand-built two-row case. This test asserts the invariants that
// must hold for ANY input, over hundreds of real rows per family, so that class
// of bug fails here instead of in production.
//
// Invariants (each is a real bug class):
//   * never throws on well-formed input (the identity guard is exercised
//     separately and deliberately)
//   * every penalty is finite and non-negative — no NaN leaking from a bad ratio
//   * an EXACT COPY of the original is always offered, is never rejected, reads
//     as "equivalent" (nothing is better or worse than itself), and never needs
//     a major review or redesign. It may be OUTRANKED by a genuinely better part
//     (a smaller footprint frees board space — the intended right-sizing
//     tie-breaker), so "ranks first" is NOT asserted; "is a clean match" is.
//   * the ranking is deterministic — same question, same answer
#include <cmath>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "../src/Browse.hpp"
#include "../src/CrossRef.hpp"
#include "../src/Index.hpp"

using namespace kelvin;
using namespace kelvin::crossref;
using nlohmann::json;

namespace {
std::string fixtures() { return std::string(KELVIN_TEST_DIR) + "/fixtures"; }

// Browse row -> flat spec, per family. This mirrors CrossRefView.vue's spec()
// builders: same field names the engine reads, `_key` carrying identity so the
// MOSFET-id-collision class of bug is exercised, not sidestepped.
json spec_for(const std::string& cat, const json& row) {
    auto num = [&](const char* k) -> json {
        auto it = row.find(k);
        if (it != row.end() && it->is_number() && it->get<double>() > 0) return *it;
        return json(nullptr);
    };
    auto s = [&](const char* k) -> std::string {
        auto it = row.find(k);
        return (it != row.end() && it->is_string()) ? it->get<std::string>() : "";
    };
    json o{{"_key", s("manufacturer") + "|" + s("mpn")}, {"mpn", s("mpn")}};
    // shared physical size + lifecycle, as base() does
    for (const char* k : {"lengthM", "widthM", "heightM"}) {
        auto it = row.find(k);
        if (it != row.end() && !it->is_null()) o[std::string(k) == "lengthM"  ? "length_m"
                                                : std::string(k) == "widthM" ? "width_m"
                                                                             : "height_m"] = *it;
    }
    if (!s("caseCode").empty()) o["case_code"] = s("caseCode");
    if (!s("mount").empty()) o["mount"] = s("mount");

    if (cat == "mosfet") {
        o["vds"] = num("vds_rated"); o["id"] = num("id_continuous");
        o["rds_on"] = num("rds_on"); o["qg"] = num("qg_total");
        o["vgs_threshold_max"] = num("vgs_threshold_max"); o["technology"] = s("technology");
    } else if (cat == "diode") {
        o["vrrm"] = num("vrrm_rated"); o["if_avg"] = num("if_avg_rated");
        o["vf"] = num("vf_typ"); o["technology"] = s("technology");
    } else if (cat == "capacitor") {
        o["value_si"] = num("capacitance"); o["voltage"] = num("v_rated");
        o["esr"] = num("esr"); o["technology"] = s("technology");
        o["dielectric_code"] = s("dielectric_code");
    } else if (cat == "resistor") {
        o["value_si"] = num("resistance"); o["power_rating"] = num("power_rating");
    } else if (cat == "magnetic") {
        o["value_si"] = num("inductance"); o["saturation_current"] = num("saturation_current");
        o["rated_current"] = num("rated_current"); o["dcr"] = num("dcr");
    } else if (cat == "connector") {
        o["family"] = s("family"); o["positions"] = num("positions");
        o["rated_current_A"] = num("rated_current"); o["rated_voltage_V"] = num("rated_voltage");
    } else if (cat == "analog") {
        o["subtype"] = s("device_type"); o["channels"] = num("channels");
        o["supply_max_V"] = num("vsupply_max"); o["gbw"] = num("gain_bandwidth");
        o["slew_rate"] = num("slew_rate");
    } else if (cat == "igbt") {
        // web/src/crossref.js XREF igbt (owner 2026-11-15)
        o["vces"] = num("vces_rated"); o["ic"] = num("ic_continuous");
        o["vce_sat"] = num("vce_sat"); o["vce_sat_ic"] = num("vce_sat_ic");
        o["tj_max"] = num("tj_max"); o["technology"] = s("technology");
    } else if (cat == "bjt") {
        // web/src/crossref.js XREF bjt (owner 2026-11-15)
        o["vceo"] = num("vceo_rated"); o["ic"] = num("ic_continuous");
        o["hfe_min"] = num("hfe_min"); o["transition_frequency"] = num("transition_frequency");
        o["power_dissipation"] = num("power_dissipation");
        o["vce_sat"] = num("vce_sat"); o["vce_sat_ic"] = num("vce_sat_ic");
        o["tj_max"] = num("tj_max");
        if (!s("polarity").empty()) o["polarity"] = s("polarity");
        o["technology"] = s("technology");
    } else if (cat == "varistor") {
        // web/src/crossref.js XREF varistor (owner 2026-11-15)
        o["value_si"] = num("varistor_voltage"); o["clamping_voltage"] = num("clamping_voltage");
        o["peak_surge_current"] = num("peak_surge_current");
        o["max_continuous_dc_voltage"] = num("max_continuous_dc_voltage");
        o["capacitance"] = num("capacitance"); o["technology"] = s("technology");
        o["max_continuous_ac_voltage"] = num("max_continuous_ac_voltage");
        o["energy_absorption"] = num("energy_absorption");
    }
    return o;
}

template <class Row>
void fuzz_family(const std::string& cat, const Shard<Row>& shard) {
    json all = browse::browse_rows(shard, json{{"limit", 400}, {"curves", true}});
    const json& rows = all.at("rows");
    REQUIRE(rows.size() > 2);

    // a fixed, spread-out sample of originals (deterministic — no RNG in tests)
    for (size_t i = 0; i < rows.size(); i += std::max<size_t>(1, rows.size() / 40)) {
        json orig = spec_for(cat, rows[i]);

        // candidate pool: a slice of other rows, PLUS an exact copy of the original
        json pool = json::array();
        for (size_t j = 0; j < rows.size() && pool.size() < 30; ++j)
            if (j != i) pool.push_back(spec_for(cat, rows[j]));
        json self_copy = orig;
        self_copy["_key"] = "SELF_COPY";
        self_copy["mpn"] = orig.value("mpn", std::string());
        pool.push_back(self_copy);

        // max_results high enough that a perfect match is never truncated away.
        Options opt; opt.max_results = pool.size() + 1;
        json r1, r2;
        REQUIRE_NOTHROW(r1 = cross_reference(cat, orig, pool, opt));
        REQUIRE_NOTHROW(r2 = cross_reference(cat, orig, pool, opt));

        // determinism
        REQUIRE(r1.dump() == r2.dump());

        // every penalty finite and non-negative
        for (const auto& c : r1.at("candidates")) {
            double pen = c.value("penalty", -1.0);
            INFO(cat << " candidate " << c.value("mpn", std::string()) << " penalty " << pen);
            CHECK(std::isfinite(pen));
            CHECK(pen >= 0.0);
        }

        // an exact copy of the original is always a clean, equivalent match.
        const auto& cands = r1.at("candidates");
        auto self = std::find_if(cands.begin(), cands.end(), [](const json& c) {
            return c.value("_key", std::string()) == "SELF_COPY";
        });
        REQUIRE(self != cands.end());
        INFO(cat << " self-copy graded " << self->value("grade", std::string())
                 << " / " << self->value("direction", std::string()));
        CHECK(self->value("status", std::string()) != "no_substitute");
        // A magnetic whose record states no saturation current reads 'unknown', not
        // 'equivalent': Isat stated by neither record (the copy is the original) leaves the axis
        // the match turns on uncompared (owner 2026-11-13, N1c, approved this exception).
        const bool isat_absent_both =
            cat == "magnetic" && !kelvin::crossref::detail::present(orig, "saturation_current") &&
            !kelvin::crossref::detail::present(orig, "saturation_points");
        if (isat_absent_both)
            CHECK(self->value("direction", std::string()) == "unknown");
        else
            CHECK(self->value("direction", std::string()) == "equivalent");
        CHECK(self->value("grade", std::string()) != "major_review");
        CHECK(self->value("grade", std::string()) != "redesign");
    }
}
}  // namespace

TEST_CASE("fuzz: mosfet ranker holds its invariants over real rows", "[crossref][fuzz]") {
    fuzz_family("mosfet", build_mosfet_shard(fixtures() + "/mosfets.ndjson"));
}
TEST_CASE("fuzz: diode ranker holds its invariants over real rows", "[crossref][fuzz]") {
    fuzz_family("diode", build_diode_shard(fixtures() + "/diodes.ndjson"));
}
TEST_CASE("fuzz: capacitor ranker holds its invariants over real rows", "[crossref][fuzz]") {
    fuzz_family("capacitor", build_capacitor_shard(fixtures() + "/capacitors.ndjson"));
}
TEST_CASE("fuzz: resistor ranker holds its invariants over real rows", "[crossref][fuzz]") {
    fuzz_family("resistor", build_resistor_shard(fixtures() + "/resistors.ndjson"));
}
TEST_CASE("fuzz: magnetic ranker holds its invariants over real rows", "[crossref][fuzz]") {
    fuzz_family("magnetic", build_magnetic_shard(fixtures() + "/magnetics.ndjson"));
}
TEST_CASE("fuzz: connector ranker holds its invariants over real rows", "[crossref][fuzz]") {
    fuzz_family("connector", build_connector_shard(fixtures() + "/connectors.ndjson"));
}
TEST_CASE("fuzz: analog ranker holds its invariants over real rows", "[crossref][fuzz]") {
    fuzz_family("analog", build_analog_shard(fixtures() + "/analog_ics.ndjson"));
}
TEST_CASE("fuzz: igbt ranker holds its invariants over real rows", "[crossref][fuzz]") {
    fuzz_family("igbt", build_igbt_shard(fixtures() + "/igbts.ndjson"));
}
TEST_CASE("fuzz: bjt ranker holds its invariants over real rows", "[crossref][fuzz]") {
    fuzz_family("bjt", build_bjt_shard(fixtures() + "/bjts.ndjson"));
}
TEST_CASE("fuzz: varistor ranker holds its invariants over real rows", "[crossref][fuzz]") {
    fuzz_family("varistor", build_varistor_shard(fixtures() + "/varistors.ndjson"));
}

// ── IGBT / BJT / varistor guards on real records (owner 2026-11-15) ──────────────────────
// Each case takes real TAS rows through the same browse row -> spec path as the fuzz pass
// and asserts the one rule it guards, so reverting that rule fails here.
namespace {
template <class Row>
json spec_by_mpn(const std::string& cat, const Shard<Row>& shard, const std::string& mpn) {
    json all = browse::browse_rows(shard, json{{"limit", 1000}, {"curves", true}});
    for (const auto& r : all.at("rows"))
        if (r.value("mpn", std::string()) == mpn) return spec_for(cat, r);
    FAIL("fixture row " << mpn << " not found");
    return json();
}
const json& cand_by_mpn(const json& result, const std::string& mpn) {
    for (const auto& c : result.at("candidates"))
        if (c.value("mpn", std::string()) == mpn) return c;
    FAIL("candidate " << mpn << " missing from the result");
    static json none;
    return none;
}
std::string param_verdict(const json& c, const std::string& name) {
    for (const auto& p : c.at("params"))
        if (p.value("name", std::string()) == name) return p.value("verdict", std::string());
    return "";
}
}  // namespace

TEST_CASE("varistor: a substitute rated below the original's Vdc max is refused",
          "[crossref][5fam][varistor]") {
    // TDK B72500E0140S260 (Vdc max 18 V) and B72500E1140S260 (16 V): same 24.5 V varistor
    // voltage, clamping voltage, surge current, Vac max and 0603 case. 16 V < 18 V: the hard
    // working-voltage gate (factor 1.0) rejects it; the original's own copy passes.
    auto shard = build_varistor_shard(fixtures() + "/varistors.ndjson");
    json orig = spec_by_mpn("varistor", shard, "B72500E0140S260");
    json lower = spec_by_mpn("varistor", shard, "B72500E1140S260");
    REQUIRE(orig.at("max_continuous_dc_voltage") == 18.0);
    REQUIRE(lower.at("max_continuous_dc_voltage") == 16.0);
    json self = orig;
    self["_key"] = "SELF_COPY";
    Options opt; opt.max_results = 5;
    json r = cross_reference("varistor", orig, json::array({lower, self}), opt);
    const json& c = cand_by_mpn(r, "B72500E1140S260");
    INFO(c.dump());
    CHECK(c.value("status", std::string()) == "no_substitute");
    CHECK(param_verdict(c, "max_continuous_dc_voltage") == "fail");
}

TEST_CASE("igbt: Vce(sat) is compared only on a common collector current",
          "[crossref][5fam][igbt]") {
    // Real TAS 06fed09 rows, all 650 V / 100 A TO247. Original AOK50B65M2: 1.72 V at 50 A.
    //   AOK50B65H1       1.90 V at 50 A  -> same basis, compared (1.10x: within the 1.2x band)
    //   FGH4L50T65MQDC50 1.45 V at 50 A  -> same basis, compared (lower: pass)
    //   STGW100H65FB2-4  1.55 V at 100 A -> different current: UNVERIFIED + vce_sat_basis
    //   IKZA100N65EH7    1.40 V, no Ic   -> only one side states it: UNVERIFIED + vce_sat_basis
    auto shard = build_igbt_shard(fixtures() + "/igbts_vce_sat_basis.ndjson");
    json orig = spec_by_mpn("igbt", shard, "AOK50B65M2");
    REQUIRE(orig.at("vce_sat_ic") == 50.0);
    json pool = json::array();
    for (const char* m : {"AOK50B65H1", "FGH4L50T65MQDC50", "STGW100H65FB2-4", "IKZA100N65EH7XKSA1"})
        pool.push_back(spec_by_mpn("igbt", shard, m));
    Options opt; opt.max_results = 10;
    json r = cross_reference("igbt", orig, pool, opt);
    {
        const json& c = cand_by_mpn(r, "AOK50B65H1");
        INFO(c.dump());
        CHECK(param_verdict(c, "vce_sat") == "warn");
        CHECK(param_verdict(c, "vce_sat_basis") == "");
    }
    {
        const json& c = cand_by_mpn(r, "FGH4L50T65MQDC50");
        INFO(c.dump());
        CHECK(param_verdict(c, "vce_sat") == "pass");
        CHECK(param_verdict(c, "vce_sat_basis") == "");
    }
    for (const char* m : {"STGW100H65FB2-4", "IKZA100N65EH7XKSA1"}) {
        const json& c = cand_by_mpn(r, m);
        INFO(c.dump());
        CHECK(param_verdict(c, "vce_sat") == "unverified");
        CHECK(param_verdict(c, "vce_sat_basis") == "unverified");
    }
}

TEST_CASE("bjt: an NPN is never a PNP's substitute", "[crossref][5fam][bjt]") {
    // onsemi MJ15003G (NPN) and its complement MJ15004G (PNP): identical 140 V / 20 A / 250 W
    // ratings, gain and fT in the same TO-204 case. Only the polarity differs, and it rejects.
    auto shard = build_bjt_shard(fixtures() + "/bjts.ndjson");
    json npn = spec_by_mpn("bjt", shard, "MJ15003G");
    json pnp = spec_by_mpn("bjt", shard, "MJ15004G");
    REQUIRE(npn.at("polarity") == "npn");
    REQUIRE(pnp.at("polarity") == "pnp");
    json self = npn;
    self["_key"] = "SELF_COPY";
    Options opt; opt.max_results = 5;
    json r = cross_reference("bjt", npn, json::array({pnp, self}), opt);
    const json& c = cand_by_mpn(r, "MJ15004G");
    INFO(c.dump());
    CHECK(c.value("status", std::string()) == "no_substitute");
    CHECK(param_verdict(c, "polarity") == "fail");
}

TEST_CASE("bjt: package aliases with identical pads are one land pattern (Qarlos 2026-11-15)",
          "[crossref][5fam][bjt][case-alias]") {
    // Real TAS rows (tests/fixtures/qarlos_1115_bjt_case_aliases.ndjson, verbatim):
    //   TIP42C-S (Bourns, 'TO-220')  -> TIP42CG (onsemi, 'TO-220-3'): the 3-lead TO-220 under
    //     its pin-count suffix, one land pattern;
    //   BC817-Q (Nexperia, 'SOT23')  -> MMBT100 (onsemi, 'SOT-23 (TO-236) 2.90x1.30x1.00,
    //     1.90P'): SOT-23 is JEDEC TO-236AB, the same pads;
    //   negative: TIP42CG ('TO-220-3') -> MJF127G ('TO-220-3 FullPak'): the fully isolated
    //     TO-220 is a different outline and must stay a different case.
    auto shard = build_bjt_shard(fixtures() + "/qarlos_1115_bjt_case_aliases.ndjson");
    auto footprint = [&](const char* o, const char* s) {
        json orig = spec_by_mpn("bjt", shard, o);
        json sub = spec_by_mpn("bjt", shard, s);
        Options opt; opt.max_results = 5;
        json r = cross_reference("bjt", orig, json::array({sub}), opt);
        const json& c = cand_by_mpn(r, s);
        INFO(o << " -> " << s << ": " << c.dump());
        return c.value("footprint", std::string());
    };
    CHECK(footprint("TIP42C-S", "TIP42CG") != "different_case");
    CHECK(footprint("BC817-Q", "MMBT100") != "different_case");
    CHECK(footprint("TIP42CG", "MJF127G") == "different_case");
}

TEST_CASE("a hard rating the substitute does not state caps the grade below drop_in",
          "[crossref][5fam][varistor][hard-unverified]") {
    // Qarlos 2026-11-15: YAGEO 471KD20-P7.5 (Vdc max 385 V, 350 J) -> Littelfuse V300LA20CP,
    // whose record states no Vdc max and no energy rating, came back drop_in with no note.
    // Real TAS rows, verbatim (tests/fixtures/qarlos_1115_varistor_unverified_hard.ndjson).
    auto shard = build_varistor_shard(fixtures() + "/qarlos_1115_varistor_unverified_hard.ndjson");
    json orig = spec_by_mpn("varistor", shard, "471KD20-P7.5");
    json sub = spec_by_mpn("varistor", shard, "V300LA20CP");
    REQUIRE(orig.at("max_continuous_dc_voltage") == 385.0);
    REQUIRE(sub.at("max_continuous_dc_voltage").is_null());
    Options opt; opt.max_results = 5;
    json r = cross_reference("varistor", orig, json::array({sub}), opt);
    const json& c = cand_by_mpn(r, "V300LA20CP");
    INFO(c.dump());
    CHECK(param_verdict(c, "max_continuous_dc_voltage") == "unverified");
    CHECK(c.value("grade", std::string()) == "minor_review");
    bool named = false;
    for (const auto& n : c.value("notes", json::array()))
        if (n.get<std::string>().find("maximum continuous DC voltage") != std::string::npos)
            named = true;
    CHECK(named);
}
