// Lifecycle status, ONE rule for every family (owner 2026-11-15): under excludeDiscontinued a
// STATED non-production manufacturerInfo.status drops the part; a record stating none is kept,
// "status" joins its unverified list with a 'status not stated' note, and it ranks after the
// parts that state one. Fixtures are verbatim catalogue records (kelvin-public-data, TAS 06fed09):
//  select_status_all/mosfets.ndjson: 1 AOGL68910 (no status, 100 V, 1.05 mOhm)
//   2 XK1R9F10QB (nrnd) 3 BSC012N10NS5 (production, 100 V, 1.2 mOhm) 4 IPI030N10N3 G (obsolete)
//  select_status_all/bjts.ndjson: 1 BD675 (no status, 45 V, hFE >= 750) 2 BC847BHZGT116 (nrnd)
//   3 BCW66KH (obsolete) 4 PMBT6429 (production, 45 V, hFE >= 500)
#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "KelvinApi.hpp"
#include "Select.hpp"

using namespace kelvin;
using json = nlohmann::json;

namespace {
std::string dir(const char* sub) { return std::string(KELVIN_TEST_DIR) + "/fixtures/" + sub; }
std::vector<std::string> mpns(const json& r) {
    std::vector<std::string> out;
    for (const auto& c : r.at("candidates")) out.push_back(c.at("mpn").get<std::string>());
    return out;
}
const json& cand(const json& r, const std::string& mpn) {
    for (const auto& c : r.at("candidates"))
        if (c.at("mpn") == mpn) return c;
    FAIL("candidate " << mpn << " not returned");
    static const json none;
    return none;
}
bool contains(const json& arr, const std::string& s) {
    return arr.is_array() && std::find(arr.begin(), arr.end(), json(s)) != arr.end();
}
bool note_has(const json& c, const std::string& s) {
    if (!c.contains("notes")) return false;
    for (const auto& n : c.at("notes"))
        if (n.get<std::string>().find(s) != std::string::npos) return true;
    return false;
}
uint64_t rej_of(const json& r, const char* k) { return r.at("rejections").value(k, uint64_t{0}); }
}  // namespace

TEST_CASE("select mosfet: no stated status is kept UNVERIFIED and ranks after a stated one",
          "[select][status][mosfet]") {
    api::Engine eng(dir("select_status_all"), "", true);
    const json req = {{"ratedDrainSourceVoltage", 100.0},
                      {"ratedContinuousDrainCurrent", 20.0},
                      {"maximumOnResistance", 0.005}};
    // excludeDiscontinued off: status is no requirement; AOGL68910 (lowest Rds(on), first in
    // the file) leads — the premise that makes the ranking below meaningful.
    json all = eng.select("mosfet", req,
                          {{"includeEnvelope", false}, {"excludeDiscontinued", false},
                           {"tiebreaker", "lowest_rds_on"}});
    REQUIRE(mpns(all).size() == 4);
    CHECK(mpns(all)[0] == "AOGL68910");
    CHECK_FALSE(cand(all, "AOGL68910").contains("unverified"));

    json r = eng.select("mosfet", req, {{"includeEnvelope", false}, {"tiebreaker", "lowest_rds_on"}});
    CHECK(mpns(r) == std::vector<std::string>{"BSC012N10NS5", "AOGL68910"});
    CHECK(rej_of(r, "discontinued") == 2);  // nrnd XK1R9F10QB, obsolete IPI030N10N3 G
    const json& ns = cand(r, "AOGL68910");
    CHECK(contains(ns.at("unverified"), "status"));
    CHECK(note_has(ns, "status not stated"));
    CHECK(ns.at("sortKey").at("statusUnverified") == true);
    CHECK_FALSE(cand(r, "BSC012N10NS5").contains("unverified"));
}

TEST_CASE("select bjt: no stated status is kept UNVERIFIED and ranks after a stated one",
          "[select][status][bjt]") {
    api::Engine eng(dir("select_status_all"), "", true);
    const json req = {{"ratedCollectorEmitterVoltage", 45.0}, {"ratedCollectorCurrent", 0.1}};
    json all = eng.select("bjt", req,
                          {{"includeEnvelope", false}, {"excludeDiscontinued", false},
                           {"tiebreaker", "highest_hfe"}});
    REQUIRE(mpns(all).size() == 4);
    CHECK(mpns(all)[0] == "BD675");  // hFE >= 750 beats 500 when status is no requirement

    json r = eng.select("bjt", req, {{"includeEnvelope", false}, {"tiebreaker", "highest_hfe"}});
    CHECK(mpns(r) == std::vector<std::string>{"PMBT6429", "BD675"});
    CHECK(rej_of(r, "discontinued") == 2);  // nrnd BC847BHZGT116, obsolete BCW66KH
    CHECK(contains(cand(r, "BD675").at("unverified"), "status"));
    CHECK(note_has(cand(r, "BD675"), "status not stated"));
}

// Magnetic best_fit (owner 2026-11-15): the same gate and note, and the lifecycle order
// production < status not stated < stated non-production right after the structural fit.
// select_status_magnetic/magnetics.ndjson (TAS 6bcb7bb, verbatim): 1 SRP1265A-220M (Bourns,
// 22 uH, no status) 2 744043100 (Wuerth, 10 uH, production) 3 NR4018T220M (Taiyo Yuden, 22 uH,
// obsolete). Against 22 uH the two 22 uH parts fit exactly and the production part does not.
TEST_CASE("select magnetic best_fit: production, then status not stated, then a stated "
          "non-production status", "[select][status][magnetic]") {
    api::Engine eng(dir("select_status_magnetic"), "", true);
    const json req = {{"magnetizingInductance", 22e-6}};
    json all = eng.select("magnetic", req,
                          {{"includeEnvelope", false}, {"excludeDiscontinued", false}});
    CHECK(all.at("tiebreaker") == "best_fit");
    CHECK(mpns(all) == std::vector<std::string>{"744043100", "SRP1265A-220M", "NR4018T220M"});
    CHECK(rej_of(all, "discontinued") == 0);
    CHECK_FALSE(cand(all, "SRP1265A-220M").contains("unverified"));  // status is no requirement

    json r = eng.select("magnetic", req, {{"includeEnvelope", false}});
    CHECK(mpns(r) == std::vector<std::string>{"744043100", "SRP1265A-220M"});
    CHECK(rej_of(r, "discontinued") == 1);  // obsolete NR4018T220M
    const json& ns = cand(r, "SRP1265A-220M");
    CHECK(contains(ns.at("unverified"), "status"));
    CHECK(note_has(ns, "status not stated"));
    CHECK(ns.at("sortKey").at("statusUnverified") == true);
    CHECK_FALSE(cand(r, "744043100").contains("unverified"));
}

// ---- Unstated BJT / varistor values are NaN, never 0 (owner 2026-11-15) ---------------------
// unstated_nan/ (kelvin-public-data, TAS 06fed09): bjts 1 PBSS4130PANP (no dcCurrentGain, no
// powerDissipation) 2 PMBT6429 (hFE >= 500, Pd stated); varistors 1 V130LA10CP (no
// maxContinuousDcVoltage, no capacitance) 2 B72520T0300K062 (no capacitance) 3
// AVR-M0603C120MTAAB (every value stated).

TEST_CASE("browse bjt / varistor: an unstated value is null, not 0", "[browse][nan][bjt][varistor]") {
    api::Engine eng(dir("unstated_nan"), "", true);
    json b = eng.browse("bjt", json{{"limit", 10}});
    REQUIRE(b.at("rows").size() == 2);
    for (const auto& row : b.at("rows")) {
        if (row.at("mpn") == "PBSS4130PANP") {
            CHECK(row.at("hfe_min").is_null());
            CHECK(row.at("power_dissipation").is_null());
        } else {
            CHECK(row.at("hfe_min").get<double>() == 500.0);
            CHECK(row.at("power_dissipation").is_number());
        }
    }
    json v = eng.browse("varistor", json{{"limit", 10}});
    REQUIRE(v.at("rows").size() == 3);
    for (const auto& row : v.at("rows")) {
        if (row.at("mpn") == "V130LA10CP") {
            CHECK(row.at("max_continuous_dc_voltage").is_null());
            CHECK(row.at("capacitance").is_null());
            CHECK(row.at("clamping_voltage").is_number());
        } else if (row.at("mpn") == "B72520T0300K062") {
            CHECK(row.at("capacitance").is_null());
            CHECK(row.at("max_continuous_dc_voltage").get<double>() == 38.0);
        } else {
            CHECK(row.at("capacitance").is_number());
        }
    }
    // A numeric filter never matches an unstated value (it used to match 0 in [0, x]).
    json f = eng.browse("varistor",
                        json{{"filters", {{"capacitance", {{"min", 0.0}, {"max", 1.0}}}}}, {"limit", 10}});
    CHECK(f.at("rows").size() == 1);
}

TEST_CASE("select bjt / varistor: an unstated gated value is its own rejection, never 0",
          "[select][nan][bjt][varistor]") {
    api::Engine eng(dir("unstated_nan"), "", true);
    const json opt = {{"includeEnvelope", false}};
    json b = eng.select("bjt", {{"ratedCollectorEmitterVoltage", 20.0}, {"ratedCollectorCurrent", 0.1}},
                        {{"includeEnvelope", false}, {"tiebreaker", "highest_hfe"}});
    CHECK(mpns(b) == std::vector<std::string>{"PMBT6429", "PBSS4130PANP"});  // unstated hFE last
    CHECK(cand(b, "PBSS4130PANP").at("margins").at("hfe").is_null());
    json bh = eng.select("bjt", {{"ratedCollectorEmitterVoltage", 20.0}, {"ratedCollectorCurrent", 0.1},
                                 {"minimumDcCurrentGain", 100.0}}, opt);
    CHECK(mpns(bh) == std::vector<std::string>{"PMBT6429"});
    CHECK(rej_of(bh, "hfe_unstated") == 1);

    json v = eng.select("varistor", {{"ratedContinuousVoltage", 5.0}},
                        {{"includeEnvelope", false}, {"tiebreaker", "lowest_capacitance"}});
    CHECK(mpns(v) == std::vector<std::string>{"AVR-M0603C120MTAAB", "B72520T0300K062"});
    CHECK(rej_of(v, "vc_unstated") == 1);  // V130LA10CP states no Vdc
    CHECK(rej_of(v, "vc_low") == 0);
}
