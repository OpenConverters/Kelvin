// Timing and analog-IC selectors (owner-approved 2026-11-15, decisions 1-8). Fixtures are
// verbatim catalogue records (kelvin-public-data, 2026-11-15):
//  select_timing/timing_devices.ndjson: 1 ABM3BN-25.000MHZ-18-D1G-T (25 MHz, 10/15 ppm, 18 pF, drawn)
//   2 ABM3-25.000MHZ-B4Y-T (30 ppm tol) 3 XRCGE25M000FXA1AR0 (no stability, 6 pF)
//   4 ABC2-25.000MHZ-4-T (no drawing, case '11x5 4-SMD, No Lead') 5 ABM10A-25.000MHz-D2Y-T3 (8 pF)
//   6 ASA-25.000MHZ-L-T (XO, 3.3 V, all-inclusive 50 ppm) 7 ABM10A-16.000MHz-10-D2Y-T3
//   8 SiT3907 (programmable, 1-220 MHz, 10 ppm) 9 830010595 (no lifecycle status)
//  select_analog/analog_ics.ndjson: 1 OPA2993 2 OPA4328 (quad) 3 AP358 (no status) 4 ADS9326
//   5 ADS8661W 6 TLV2886 (GBW 5.4 MHz, supply >= 4.5 V) 7 MAX40242 (no slew rate)
//  select_status/: timing 1 ABM3BN (production) 2 ABM8-25.000MHZ-10-D1G-T. (nrnd)
//   3 FC5BAEBMM25.0 (nrnd) 4 830010595 (no status); analog 1 OPA2993 (production)
//   2 OPA2591 (preview) 3 AD712 (nrnd) 4 AP358 (no status)
//  select_status_rank/: timing 1 830059627 (no status, 38.4 MHz, 30 ppm, 2.0 x 1.6 mm)
//   2 ABM10AAIG-38.400MHz-10-2Z-T3 (production, 38.4 MHz, 50 ppm, 2.5 x 2.0 mm)
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

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
    FAIL("candidate " << mpn << " missing");
    static json none;
    return none;
}
bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
bool contains(const json& arr, const std::string& s) {
    return std::find(arr.begin(), arr.end(), json(s)) != arr.end();
}
bool note_has(const json& c, const std::string& needle) {
    for (const auto& n : c.at("notes"))
        if (n.get<std::string>().find(needle) != std::string::npos) return true;
    return false;
}
int64_t rej_of(const json& r, const char* key) {
    const json& rej = r.at("rejections");
    return rej.contains(key) ? rej.at(key).get<int64_t>() : 0;
}
const json opt = {{"includeEnvelope", false}};
}  // namespace

TEST_CASE("select timing: frequency identity, stated-fail rejects, stability before area",
          "[select][timing]") {
    api::Engine eng(dir("select_timing"), "", /*quiet=*/true);
    json req = {{"frequency", {{"nominal", 25e6}}}, {"maximumFrequencyDeviation", 0.0},
                {"maximumFrequencyTolerance", 20e-6}, {"maximumFrequencyStability", 50e-6}};
    json r = eng.select("timing", req, opt);
    auto m = mpns(r);
    // Three fully verified parts. The programmable SiT3907 (ordered at 25 MHz, 10 ppm) is an
    // exact match like the fixed parts (decision 7) and states the tightest stability; then
    // ABM3BN (15 ppm, 16 mm2) before ABM10A-25 (30 ppm, 5 mm2): the tighter stability is
    // decided before the smaller area (decision 1).
    REQUIRE(m.size() == 3);
    CHECK(m[0] == "SiT3907");
    CHECK(m[1] == "ABM3BN-25.000MHZ-18-D1G-T");
    CHECK(m[2] == "ABM10A-25.000MHz-D2Y-T3");
    CHECK_FALSE(has(m, "ABM3-25.000MHZ-B4Y-T"));
    CHECK_FALSE(has(m, "ABM10A-16.000MHz-10-D2Y-T3"));
    // stated 30/50/30 ppm tolerances; the XO's all-inclusive 50 ppm against 20 ppm (decision 5);
    // the status-less 830010595 is no longer dropped as discontinued, it fails its 30 ppm
    CHECK(rej_of(r, "tolerance_loose") == 5);
    CHECK(rej_of(r, "frequency_off") == 1);
    CHECK(rej_of(r, "discontinued") == 0);
    CHECK_FALSE(has(m, "ASA-25.000MHZ-L-T"));
    CHECK_FALSE(has(m, "830010595"));
    CHECK(cand(r, "ABM3BN-25.000MHZ-18-D1G-T").at("sortKey").at("unverifiedItems") == 0);
    // order: non-decreasing UNVERIFIED items
    int prev = -1;
    for (const auto& c : r.at("candidates")) {
        int u = c.at("sortKey").at("unverifiedItems").get<int>();
        CHECK(u >= prev);
        prev = u;
    }
}

TEST_CASE("select timing: nearest frequency, then stability, then board area, then file order",
          "[select][timing]") {
    api::Engine eng(dir("select_timing"), "", true);
    // 20 MHz within 30 %: the programmable SiT3907 is ordered at 20 MHz (deviation 0); the
    // 16 MHz part (20 % off) is nearer than every 25 MHz part (25 %), whatever their stability
    json r = eng.select("timing", {{"frequency", 20e6}, {"maximumFrequencyDeviation", 0.3}}, opt);
    auto m = mpns(r);
    REQUIRE(m.size() >= 5);
    CHECK(m[0] == "SiT3907");
    CHECK(m[1] == "ABM10A-16.000MHz-10-D2Y-T3");
    CHECK(m[2] == "ABM3BN-25.000MHZ-18-D1G-T");  // 25 MHz, 15 ppm
    // 30 ppm both: ABM10A-25 (2.5 x 2.0 mm) before ABM3 (5.0 x 3.2 mm) although ABM3 comes
    // first in the file
    auto pos = [&](const std::string& s) { return std::find(m.begin(), m.end(), s) - m.begin(); };
    CHECK(pos("ABM10A-25.000MHz-D2Y-T3") == 3);
    CHECK(pos("ABM3-25.000MHZ-B4Y-T") == 4);
    // a part stating no stability (XRCGE) ranks after those stating one, at equal deviation
    CHECK(pos("ASA-25.000MHZ-L-T") < pos("XRCGE25M000FXA1AR0"));
    CHECK(r.at("order") == json({"unverifiedItems", "frequencyDeviation", "frequencyStability",
                                 "boardArea", "fileOrder"}));
}

TEST_CASE("select timing: load capacitance asks for a crystal, supply for an oscillator",
          "[select][timing]") {
    api::Engine eng(dir("select_timing"), "", true);
    json r = eng.select("timing",
                        {{"frequency", 25e6}, {"maximumFrequencyDeviation", 0.0},
                         {"loadCapacitance", 18e-12}},
                        opt);
    auto m = mpns(r);
    // decision 6: the XO and the programmable oscillator are rejected under their own bucket
    CHECK(rej_of(r, "oscillator_for_load_capacitance") == 2);
    CHECK_FALSE(has(m, "ASA-25.000MHZ-L-T"));
    CHECK_FALSE(has(m, "SiT3907"));
    // stated 8 pF, 6 pF, 16 pF
    CHECK(rej_of(r, "load_capacitance_mismatch") == 3);
    CHECK_FALSE(has(m, "ABM10A-25.000MHz-D2Y-T3"));
    CHECK(m.front() == "ABM3BN-25.000MHZ-18-D1G-T");
    // the undrawn ABC2 passes CL but its size is undetermined: one UNVERIFIED item
    const json& abc2 = cand(r, "ABC2-25.000MHZ-4-T");
    CHECK(abc2.at("boardArea").at("mm2").is_null());
    CHECK(abc2.at("sortKey").at("sizeUnverified") == true);

    // a supply voltage asks for an oscillator: every passive crystal is rejected
    json r2 = eng.select("timing",
                         {{"frequency", 25e6}, {"maximumFrequencyDeviation", 0.0},
                          {"supplyVoltage", 2.5}},
                         opt);
    CHECK(rej_of(r2, "passive_for_supply_voltage") == 7);
    // the 3.3 V-only XO states a range that excludes 2.5 V; the 1.8-3.3 V SiT3907 covers it
    CHECK(rej_of(r2, "supply_out_of_range") == 1);
    CHECK(mpns(r2) == std::vector<std::string>{"SiT3907"});

    // footprint 3.0 x 2.0 mm: the 5.0 x 3.2 mm ABM3BN is too large
    json r3 = eng.select("timing",
                         {{"frequency", 25e6}, {"maximumFrequencyDeviation", 0.0},
                          {"maximumLength", 3.0e-3}, {"maximumWidth", 2.0e-3}},
                         opt);
    CHECK_FALSE(has(mpns(r3), "ABM3BN-25.000MHZ-18-D1G-T"));
    CHECK(has(mpns(r3), "ASA-25.000MHZ-L-T"));
}

TEST_CASE("select timing: an oscillator's all-inclusive stability is judged as its tolerance",
          "[select][timing]") {
    api::Engine eng(dir("select_timing"), "", true);
    json r = eng.select("timing",
                        {{"frequency", 25e6}, {"maximumFrequencyDeviation", 0.0},
                         {"maximumFrequencyTolerance", 50e-6}},
                        opt);
    const json& xo = cand(r, "ASA-25.000MHZ-L-T");
    CHECK_FALSE(contains(xo.at("unverified"), "frequencyTolerance"));
    bool judged = false;
    for (const auto& k : xo.at("checks"))
        if (k.at("key") == "frequencyTolerance") {
            judged = true;
            CHECK(k.at("verdict") == "pass");
            CHECK(k.at("basis") == "frequencyStability (all-inclusive)");
            CHECK(k.at("stated").get<double>() == Catch::Approx(50e-6));
        }
    CHECK(judged);
    CHECK(note_has(xo, "all-inclusive stability"));
    // a crystal keeps its own tolerance figure (no basis)
    for (const auto& k : cand(r, "ABM3BN-25.000MHZ-18-D1G-T").at("checks"))
        if (k.at("key") == "frequencyTolerance") CHECK_FALSE(k.contains("basis"));
}

TEST_CASE("select timing: a programmable blank matches inside its range, rejects outside",
          "[select][timing]") {
    api::Engine eng(dir("select_timing"), "", true);
    json r = eng.select("timing", {{"frequency", 25e6}, {"maximumFrequencyDeviation", 0.0}}, opt);
    const json& sit = cand(r, "SiT3907");
    CHECK(contains(sit.at("flags"), "programmable"));
    CHECK(note_has(sit, "must be ordered or programmed for"));
    CHECK(sit.at("frequencyDeviation") == 0.0);
    CHECK(sit.at("sortKey").at("frequencyDeviation") == 0.0);
    CHECK(rej_of(r, "frequency_not_stated") == 0);
    // ranked like a fixed part (deviation 0, then its stated 10 ppm): the flag is not a key
    CHECK(sit.at("sortKey").at("unverifiedItems") == 0);
    CHECK(mpns(r).front() == "SiT3907");
    CHECK_FALSE(contains(cand(r, "ABM3BN-25.000MHZ-18-D1G-T").at("flags"), "programmable"));
    // 250 MHz lies outside 1-220 MHz: rejected, while the fixed parts within 100 % still match
    json r2 = eng.select("timing", {{"frequency", 250e6}, {"maximumFrequencyDeviation", 1.0}}, opt);
    CHECK_FALSE(has(mpns(r2), "SiT3907"));
    CHECK(rej_of(r2, "frequency_outside_programmable_range") == 1);
}

TEST_CASE("select timing / analog: no stated status is kept UNVERIFIED, a stated one drops",
          "[select][timing][analog]") {
    api::Engine eng(dir("select_status"), "", true);
    json r = eng.select("timing", {{"frequency", 25e6}, {"maximumFrequencyDeviation", 0.0}}, opt);
    auto m = mpns(r);
    CHECK(m == std::vector<std::string>{"ABM3BN-25.000MHZ-18-D1G-T", "830010595"});
    CHECK(rej_of(r, "discontinued") == 2);  // the two stated nrnd parts
    const json& ns = cand(r, "830010595");
    CHECK(contains(ns.at("unverified"), "status"));
    CHECK(note_has(ns, "status not stated"));
    CHECK(ns.at("sortKey").at("unverifiedItems") == 1);  // drawn, so the status is its one item
    CHECK(cand(r, "ABM3BN-25.000MHZ-18-D1G-T").at("sortKey").at("unverifiedItems") == 0);
    // excludeDiscontinued off: status is not a requirement, so it is no UNVERIFIED item
    json all = eng.select("timing", {{"frequency", 25e6}, {"maximumFrequencyDeviation", 0.0}},
                          json{{"includeEnvelope", false}, {"excludeDiscontinued", false}});
    CHECK(mpns(all).size() == 4);
    CHECK_FALSE(contains(cand(all, "830010595").at("unverified"), "status"));

    json a = eng.select("analog", {{"deviceType", "operationalAmplifier"}}, opt);
    CHECK(mpns(a) == std::vector<std::string>{"OPA2993", "AP358"});
    CHECK(rej_of(a, "discontinued") == 2);  // preview OPA2591, nrnd AD712
    CHECK(contains(cand(a, "AP358").at("unverified"), "status"));
}

TEST_CASE("select timing: a part with no stated status ranks after a stated production one",
          "[select][timing]") {
    // 830059627 (no status, 30 ppm stability, 2.0 x 1.6 mm, first in the file) would win on
    // stability, board area and file order; its missing status is one more UNVERIFIED item
    // (decision 8), so the production ABM10AAIG (50 ppm, 2.5 x 2.0 mm) ranks first.
    api::Engine eng(dir("select_status_rank"), "", true);
    json r = eng.select("timing", {{"frequency", 38.4e6}, {"maximumFrequencyDeviation", 0.0}},
                        opt);
    CHECK(mpns(r) == std::vector<std::string>{"ABM10AAIG-38.400MHz-10-2Z-T3", "830059627"});
    CHECK(cand(r, "830059627").at("sortKey").at("unverifiedItems") == 1);
    CHECK(cand(r, "ABM10AAIG-38.400MHz-10-2Z-T3").at("sortKey").at("unverifiedItems") == 0);
    // excludeDiscontinued off: status is no requirement, so 830059627 wins on stability
    json all = eng.select("timing", {{"frequency", 38.4e6}, {"maximumFrequencyDeviation", 0.0}},
                          json{{"includeEnvelope", false}, {"excludeDiscontinued", false}});
    CHECK(mpns(all) == std::vector<std::string>{"830059627", "ABM10AAIG-38.400MHz-10-2Z-T3"});
}

TEST_CASE("select timing: missing or malformed inputs throw", "[select][timing]") {
    api::Engine eng(dir("select_timing"), "", true);
    CHECK_THROWS_AS(eng.select("timing", json::object(), json::object()), InvalidOptions);
    CHECK_THROWS_AS(eng.select("timing", {{"frequency", 25e6}}, json::object()), InvalidOptions);
    CHECK_THROWS_AS(eng.select("timing", {{"maximumFrequencyDeviation", 0.0}}, json::object()),
                    InvalidOptions);
    CHECK_THROWS_AS(eng.select("timing",
                               {{"frequency", 25e6}, {"maximumFrequencyDeviation", 0.0},
                                {"maximumFrequencyStability", "20ppm"}},
                               json::object()),
                    InvalidOptions);
    CHECK_THROWS_AS(eng.select("timing",
                               {{"frequency", 25e6}, {"maximumFrequencyDeviation", 0.0},
                                {"maximumLength", 3e-3}},
                               json::object()),
                    InvalidOptions);
}

TEST_CASE("select analog: device type gates, per-type checks, unverified ranks after",
          "[select][analog]") {
    api::Engine eng(dir("select_analog"), "", true);
    json req = {{"deviceType", "operationalAmplifier"}, {"numberOfChannels", 2},
                {"supplyVoltage", 5.0}, {"minimumGainBandwidthProduct", 10e6},
                {"minimumSlewRate", 5e6}};
    json r = eng.select("analog", req, opt);
    auto m = mpns(r);
    REQUIRE(m.size() == 3);
    CHECK(m[0] == "OPA2993");   // passes every check
    CHECK(m[1] == "MAX40242");  // slew rate not stated: UNVERIFIED, ranked after
    CHECK(m[2] == "AP358");     // GBW, slew rate and status not stated
    CHECK(cand(r, "MAX40242").at("unverified").at(0) == "slewRate");
    CHECK(contains(cand(r, "AP358").at("unverified"), "status"));
    CHECK(rej_of(r, "gain_bandwidth_low") == 1);  // TLV2886
    CHECK(rej_of(r, "channels_mismatch") == 1);   // OPA4328 (exact count, decision 4)
    CHECK(rej_of(r, "discontinued") == 0);        // AP358 states no status: kept (decision 8)
    CHECK(rej_of(r, "device_type_mismatch") == 2);
    // supply 3.3 V: TLV2886 states >= 4.5 V
    json r2 = eng.select("analog", {{"deviceType", "operationalAmplifier"}, {"supplyVoltage", 3.3}},
                         opt);
    CHECK_FALSE(has(mpns(r2), "TLV2886"));
    // ADC resolution
    json r3 = eng.select("analog", {{"deviceType", "adc"}, {"minimumResolution", 14}}, opt);
    CHECK(mpns(r3) == std::vector<std::string>{"ADS9326"});
}

TEST_CASE("select analog: the type's key figure orders, a part not stating it sorts after",
          "[select][analog]") {
    api::Engine eng(dir("select_analog"), "", true);
    // op-amps: lowest |Vos| first (TLV2886 15 uV, OPA4328 50 uV, MAX40242 0.6 mV, OPA2993 1 mV;
    // file order would be OPA2993, OPA4328, TLV2886, MAX40242). AP358 states no Vos: last.
    json r = eng.select("analog", {{"deviceType", "operationalAmplifier"}},
                        json{{"includeEnvelope", false}, {"excludeDiscontinued", false}});
    CHECK(mpns(r) ==
          std::vector<std::string>{"TLV2886", "OPA4328", "MAX40242", "OPA2993", "AP358"});
    CHECK(r.at("order").at(1) == "lowest inputOffsetVoltage");
    CHECK(cand(r, "AP358").at("sortKey").at("keyFigure").at("value").is_null());
    // ADC: highest sample rate
    json a = eng.select("analog", {{"deviceType", "adc"}}, opt);
    CHECK(mpns(a) == std::vector<std::string>{"ADS9326", "ADS8661W"});
    CHECK(a.at("order").at(1) == "highest sampleRate");
}

TEST_CASE("select analog: over-rating is flagged on the card and does not change the order",
          "[select][analog]") {
    api::Engine eng(dir("select_analog"), "", true);
    json r = eng.select("analog",
                        {{"deviceType", "operationalAmplifier"}, {"minimumGainBandwidthProduct", 6e6}},
                        json{{"includeEnvelope", false}, {"excludeDiscontinued", false}});
    auto m = mpns(r);
    // OPA4328 (40 MHz, 6.7x) and OPA2993 (20 MHz, 3.3x) are over-rated; MAX40242 (10 MHz,
    // 1.7x) is not. The order is still lowest Vos: OPA4328, MAX40242, OPA2993.
    REQUIRE(m.size() >= 3);
    CHECK(m[0] == "OPA4328");
    CHECK(m[1] == "MAX40242");
    CHECK(m[2] == "OPA2993");
    CHECK(contains(cand(r, "OPA4328").at("flags"), "overRated"));
    CHECK(contains(cand(r, "OPA2993").at("flags"), "overRated"));
    CHECK_FALSE(contains(cand(r, "MAX40242").at("flags"), "overRated"));
    const json& o = cand(r, "OPA2993").at("overRated");
    REQUIRE(o.size() == 1);
    CHECK(o.at(0).at("key") == "gainBandwidthProduct");
    CHECK(o.at(0).at("ratio").get<double>() == Catch::Approx(20.0 / 6.0));
    CHECK(note_has(cand(r, "OPA2993"), "over-rated"));
    // the over-rated flag carries no ranking weight: the sort key has no over-rating field
    CHECK_FALSE(cand(r, "OPA4328").at("sortKey").contains("overRated"));
}

TEST_CASE("select analog: missing type, inapplicable key and no-selector type throw",
          "[select][analog]") {
    api::Engine eng(dir("select_analog"), "", true);
    CHECK_THROWS_AS(eng.select("analog", json::object(), json::object()), InvalidOptions);
    CHECK_THROWS_AS(eng.select("analog", {{"deviceType", "adc"}, {"minimumGainBandwidthProduct", 1e6}},
                               json::object()),
                    InvalidOptions);
    CHECK_THROWS_AS(eng.select("analog", {{"deviceType", "digitalIsolator"}}, json::object()),
                    InvalidOptions);
    CHECK_THROWS_AS(eng.select("analog", {{"deviceType", "nonsense"}}, json::object()),
                    InvalidOptions);
}
