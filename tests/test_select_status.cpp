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
