// [select] Targeted selector behaviours the golden can't easily pin: the Coss total-loss
// steer (abt #64), the MOSFET evidence tier, MLCC ESR=0-is-best, the diode no-evidence-tier
// asymmetry, and rejection-histogram order.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "Index.hpp"
#include "Select.hpp"

using namespace kelvin;

namespace {
// Full-precision double formatter (std::to_string would flatten 875e-12 to "0.000000").
std::string num(double v) {
    std::ostringstream os;
    os.precision(17);
    os << v;
    return os.str();
}

// Rich mosfet line with coss + optional qg/datasheet.
std::string mosfet(const std::string& mpn, double vds, double id, double rds, double qg, double coss,
                   const std::string& url = "https://acme.com/x.pdf") {
    std::string qg_field = qg >= 0 ? (",\"totalGateCharge\":" + num(qg)) : "";
    return "{\"semiconductor\":{\"mosfet\":{\"manufacturerInfo\":{\"name\":\"ACME\",\"reference\":\"" +
           mpn + "\",\"status\":\"production\",\"datasheetUrl\":\"" + url +
           "\",\"datasheetInfo\":{\"part\":{\"technology\":\"Si\"},\"electrical\":{"
           "\"drainSourceVoltage\":" +
           num(vds) + ",\"continuousDrainCurrent\":" + num(id) + ",\"onResistance\":" + num(rds) +
           ",\"outputCapacitance\":" + num(coss) + qg_field + "}}}}}}";
}

Shard<MosfetRow> shard_from(const std::vector<std::string>& lines) {
    // Build a shard from in-memory lines by writing a temp file (reuses the real parser).
    std::string path = std::string(KELVIN_TEST_DIR) + "/_select_tmp.ndjson";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        for (auto& l : lines) f << l << "\n";
    }
    auto s = build_mosfet_shard(path);
    std::remove(path.c_str());
    return s;
}
}  // namespace

TEST_CASE("select: Coss total-loss steers away from the high-Coss part (abt #64)", "[select][mosfet]") {
    // At 400 V / low current, the low-Rds part has huge Coss; total-loss must prefer the low-Coss
    // part even though LOWEST_RDS_ON would take the other.
    auto shard = shard_from({
        mosfet("LOW_RDS_HIGH_COSS", 650, 20, 0.00625, 50e-9, 875e-12),
        mosfet("HIGH_RDS_LOW_COSS", 650, 10, 0.070, 30e-9, 90e-12),
    });
    MosfetConstraints c;
    c.vds_min = 400;
    c.id_min = 1;
    c.rds_on_max = 1.0;
    c.qg_max = INFINITY;
    // LOWEST_RDS_ON -> the 6.25 mΩ part.
    auto by_rds = select_mosfet(shard, c, MosfetTiebreaker::LowestRdsOn);
    REQUIRE(by_rds.at("candidates")[0].at("mpn") == "LOW_RDS_HIGH_COSS");
    // LOWEST_TOTAL_LOSS at 400 V / 1 A -> the low-Coss part.
    c.op_i_rms = 1;
    c.op_vds = 400;
    c.op_duty = 0.5;
    c.op_fsw = 100000;
    auto by_loss = select_mosfet(shard, c, MosfetTiebreaker::LowestTotalLoss);
    REQUIRE(by_loss.at("candidates")[0].at("mpn") == "HIGH_RDS_LOW_COSS");
    // The winner's sortKey exposes the loss breakdown.
    REQUIRE(by_loss.at("candidates")[0].at("sortKey").at("pCoss").get<double>() > 0);
}

TEST_CASE("select: evidence tier deprioritises a no-Qg part", "[select][mosfet]") {
    // The lower-Rds part has no Qg (evidence-incomplete) -> the documented part wins LOWEST_RDS_ON.
    auto shard = shard_from({
        mosfet("NO_QG_LOW_RDS", 100, 10, 0.005, -1 /*absent*/, 100e-12),
        mosfet("HAS_QG_HIGH_RDS", 100, 10, 0.010, 5e-9, 100e-12),
    });
    MosfetConstraints c;
    c.vds_min = 60;
    c.id_min = 5;
    c.rds_on_max = 1.0;
    c.qg_max = INFINITY;
    auto r = select_mosfet(shard, c, MosfetTiebreaker::LowestRdsOn);
    REQUIRE(r.at("candidates")[0].at("mpn") == "HAS_QG_HIGH_RDS");
}

TEST_CASE("select: bad datasheet URL makes a part evidence-incomplete", "[select][mosfet]") {
    auto shard = shard_from({
        mosfet("DEAD_SHEET_LOW_RDS", 100, 10, 0.005, 5e-9, 100e-12,
               "https://datasheetpdf.com/foo"),
        mosfet("GOOD_SHEET_HIGH_RDS", 100, 10, 0.010, 5e-9, 100e-12, "https://acme.com/y.pdf"),
    });
    MosfetConstraints c;
    c.vds_min = 60;
    c.id_min = 5;
    c.rds_on_max = 1.0;
    c.qg_max = INFINITY;
    auto r = select_mosfet(shard, c, MosfetTiebreaker::LowestRdsOn);
    REQUIRE(r.at("candidates")[0].at("mpn") == "GOOD_SHEET_HIGH_RDS");
}

TEST_CASE("select: rejection histogram omits zero-count keys and totals correctly", "[select][mosfet]") {
    auto shard = shard_from({
        mosfet("A", 100, 10, 0.005, 5e-9, 100e-12),
        mosfet("B", 30, 10, 0.005, 5e-9, 100e-12),  // vds too low for a 60 V need
    });
    MosfetConstraints c;
    c.vds_min = 60;
    c.id_min = 5;
    c.rds_on_max = 1.0;
    c.qg_max = INFINITY;
    auto r = select_mosfet(shard, c, MosfetTiebreaker::LowestRdsOn);
    REQUIRE(r.at("candidates")[0].at("mpn") == "A");
    REQUIRE(r.at("rejections").at("vds_rated_low") == 1);
    REQUIRE_FALSE(r.at("rejections").contains("discontinued"));  // zero -> omitted
    REQUIRE(r.at("totalRowsConsidered") == 2);
}

// ── controller category gate (ABT #694) ──────────────────────────────────────
namespace {
// A controller line: category + the topologies its datasheet says it can drive.
std::string controller(const std::string& mpn, const std::string& category,
                       const std::vector<std::string>& topos, double vref = 1.2) {
    std::string t;
    for (size_t i = 0; i < topos.size(); ++i) t += (i ? ",\"" : "\"") + topos[i] + "\"";
    return "{\"controller\":{\"manufacturerInfo\":{\"name\":\"ACME\",\"reference\":\"" + mpn +
           "\",\"status\":\"production\",\"datasheetInfo\":{\"function\":{\"category\":\"" +
           category + "\",\"intendedTopologies\":[" + t +
           "]},\"part\":{\"deviceType\":\"controller\",\"partNumber\":\"" + mpn +
           "\"},\"electrical\":{\"referenceVoltage\":{\"nominal\":" + num(vref) + "}}}}}}";
}

Shard<ControllerRow> ctrl_shard_from(const std::vector<std::string>& lines) {
    std::string path = std::string(KELVIN_TEST_DIR) + "/_select_ctrl_tmp.ndjson";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        for (auto& l : lines) f << l << "\n";
    }
    auto s = build_controller_shard(path);
    std::remove(path.c_str());
    return s;
}

ControllerConstraints ctrl_for(const std::string& topo) {
    ControllerConstraints c;
    c.topology = topo;
    c.vin_nom = 48.0;
    c.fsw_khz = 400.0;
    return c;
}
}  // namespace

TEST_CASE("select: a PFC controller is not a buck controller because it can drive a buck stage",
          "[select][controller]") {
    // The shape of the real defect: NCL30000 is an offline PFC/LED driver whose datasheet
    // legitimately lists buckConverter, so it matched a 48 -> 12 V buck on topology, ranked as
    // a "real controller" (not a gate driver) with a reference voltage, and took the first line
    // of the BOM. Its loop shapes input current against the line; nothing about a 48 V DC rail
    // asks for that. (ABT #694.)
    auto shard = ctrl_shard_from({
        controller("PFC_LED_DRIVER", "pfcController",
                   {"powerFactorCorrection", "buckConverter"}),
        controller("PLAIN_BUCK", "pwmController", {"buckConverter"}),
    });
    auto r = select_controller(shard, ctrl_for("buck"));
    REQUIRE(r.at("candidates")[0].at("mpn") == "PLAIN_BUCK");
    REQUIRE(r.at("rejections").at("category_does_not_regulate_this_rail") == 1);

    // ...but for a PFC front end it is exactly the right part, and still wins there.
    auto pfc = select_controller(shard, ctrl_for("power_factor_correction"));
    REQUIRE(pfc.at("candidates")[0].at("mpn") == "PFC_LED_DRIVER");
    REQUIRE_FALSE(pfc.at("rejections").contains("category_does_not_regulate_this_rail"));
}

TEST_CASE("select: parts that regulate no rail at all never rank as controllers",
          "[select][controller]") {
    // A supervisor WATCHES a rail and a shunt regulator is a two-terminal reference. Both carry
    // a reference voltage — the very field `has_vref` rewards — and neither lists a topology, so
    // the topology filter (which only rejects rows that DO list topologies) let them through and
    // the rank tuple then scored them as credible controllers.
    auto shard = ctrl_shard_from({
        controller("SUPERVISOR", "supervisor", {}),
        controller("TL431ish", "shuntRegulator", {}),
        controller("SR_CONTROLLER", "syncRectifierController", {}),
        controller("REAL_ONE", "pwmController", {"buckConverter"}),
    });
    auto r = select_controller(shard, ctrl_for("buck"));
    REQUIRE(r.at("candidates")[0].at("mpn") == "REAL_ONE");
    REQUIRE(r.at("candidates").size() == 1);
    REQUIRE(r.at("rejections").at("category_does_not_regulate_this_rail") == 3);
}

TEST_CASE("select: naming the category explicitly overrides the inference", "[select][controller]") {
    // The gate governs what is offered on the caller's behalf, not what it asked for by name:
    // someone who wants a sync-rectifier controller is not second-guessed.
    auto shard = ctrl_shard_from({
        controller("SR_CONTROLLER", "syncRectifierController", {}),
        controller("REAL_ONE", "pwmController", {"buckConverter"}),
    });
    auto c = ctrl_for("buck");
    c.category = "syncRectifierController";
    auto r = select_controller(shard, c);
    REQUIRE(r.at("candidates")[0].at("mpn") == "SR_CONTROLLER");
}
