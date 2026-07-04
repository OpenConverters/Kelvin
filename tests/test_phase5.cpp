// [phase5] IGBT / BJT / varistor selectors (no HS reference — behaviour + real-fixture smoke).
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "Index.hpp"
#include "Requirements.hpp"
#include "Select.hpp"

using namespace kelvin;

namespace {
std::string fixtures_dir() { return std::string(KELVIN_TEST_DIR) + "/fixtures"; }

std::string num(double v) {
    std::ostringstream os;
    os.precision(17);
    os << v;
    return os.str();
}
std::string igbt(const std::string& mpn, double vces, double ic, double vsat) {
    return "{\"semiconductor\":{\"igbt\":{\"manufacturerInfo\":{\"name\":\"ACME\",\"reference\":\"" +
           mpn + "\",\"status\":\"production\",\"datasheetInfo\":{\"part\":{},\"electrical\":{"
           "\"collectorEmitterVoltage\":" + num(vces) + ",\"continuousCollectorCurrent\":" + num(ic) +
           ",\"collectorEmitterSaturation\":" + num(vsat) + "}}}}}}";
}
template <class BuildFn>
auto shard_from(const std::vector<std::string>& lines, BuildFn build) {
    std::string path = std::string(KELVIN_TEST_DIR) + "/_p5_tmp.ndjson";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        for (auto& l : lines) f << l << "\n";
    }
    auto s = build(path);
    std::remove(path.c_str());
    return s;
}
}  // namespace

TEST_CASE("phase5: igbt filters + lowest-Vce(sat) rank", "[phase5][igbt]") {
    auto shard = shard_from({igbt("HI_VSAT", 1200, 100, 2.5), igbt("LO_VSAT", 1200, 100, 1.4),
                             igbt("LOW_V", 300, 100, 1.0)},
                            [](const std::string& p) { return build_igbt_shard(p); });
    IgbtConstraints c;
    c.vces_min = 600;
    c.ic_min = 50;
    auto r = select_igbt(shard, c, IgbtTiebreaker::LowestVceSat);
    REQUIRE(r.at("candidates")[0].at("mpn") == "LO_VSAT");   // lowest Vce(sat) among 1200 V parts
    REQUIRE(r.at("rejections").at("vces_low") == 1);          // the 300 V part dropped
    // vce_sat_max filter
    c.vce_sat_max = 2.0;
    auto r2 = select_igbt(shard, c, IgbtTiebreaker::LowestVceSat);
    REQUIRE(r2.at("rejections").at("vce_sat_high") == 1);
}

TEST_CASE("phase5: igbt requirements mapping", "[phase5][igbt]") {
    json req = {{"ratedCollectorEmitterVoltage", 650.0}, {"ratedCollectorCurrent", 40.0},
                {"maximumSaturationVoltage", 2.0}};
    auto c = igbt_constraints(req);
    REQUIRE(c.vces_min == 650.0);
    REQUIRE(c.ic_min == 40.0);
    REQUIRE(c.vce_sat_max.value() == 2.0);
}

TEST_CASE("phase5: bjt selects over the real fixture (highest hFE)", "[phase5][bjt]") {
    auto shard = build_bjt_shard(fixtures_dir() + "/bjts.ndjson");
    REQUIRE(shard.meta.row_count > 0);
    BjtConstraints c;
    c.vceo_min = 20;
    c.ic_min = 0.1;
    auto r = select_bjt(shard, c, BjtTiebreaker::HighestHfe);
    REQUIRE_FALSE(r.at("candidates").empty());
    // candidates are sorted by descending guaranteed hFE.
    double h0 = r.at("candidates")[0].at("margins").at("hfe").get<double>();
    if (r.at("candidates").size() > 1) {
        double h1 = r.at("candidates")[1].at("margins").at("hfe").get<double>();
        REQUIRE(h0 >= h1);
    }
}

TEST_CASE("phase5: varistor selects over the real fixture (lowest clamping)", "[phase5][varistor]") {
    auto shard = build_varistor_shard(fixtures_dir() + "/varistors.ndjson");
    REQUIRE(shard.meta.row_count > 0);
    VaristorConstraints c;
    c.rated_continuous_voltage = 5.0;
    auto r = select_varistor(shard, c, VaristorTiebreaker::LowestClampingVoltage);
    REQUIRE_FALSE(r.at("candidates").empty());
    // Every candidate must actually withstand the rated continuous voltage.
    for (const auto& cand : r.at("candidates")) {
        double vc_margin = cand.at("margins").at("vc_margin").get<double>();
        REQUIRE(vc_margin >= 1.0);
    }
    // Unsatisfiable -> NoCandidates with a vc_low histogram.
    VaristorConstraints hi;
    hi.rated_continuous_voltage = 1e9;
    try {
        select_varistor(shard, hi, VaristorTiebreaker::LowestClampingVoltage);
        FAIL("expected NoCandidates");
    } catch (const NoCandidates& e) {
        REQUIRE(e.rejections.contains("vc_low"));
    }
}
