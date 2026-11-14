// [mosfet_loss] Operating-point loss ranking (owner 2026-11-14): every term from the record's
// datasheet values at the caller's operating point, each with its basis; a missing input makes
// its term UNVERIFIED (never a guessed number) and such a candidate ranks after the fully
// computed ones.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "Index.hpp"
#include "KelvinApi.hpp"
#include "MosfetLoss.hpp"
#include "Select.hpp"

using namespace kelvin;
using nlohmann::json;
using Catch::Approx;

namespace {

json curve(const char* xl, const char* yl, std::vector<double> x, std::vector<double> y,
           json cond = json::object()) {
    json c = {{"xLabel", xl}, {"yLabel", yl}, {"xData", x}, {"yData", y}};
    if (!cond.empty()) c["conditions"] = cond;
    return c;
}

// A MOSFET record. `elec_extra` / `curves` are merged into the datasheetInfo blocks.
std::string record(const std::string& mpn, double vds, double id, double rds, json elec_extra,
                   json curves = json::object()) {
    json elec = {{"drainSourceVoltage", vds},
                 {"continuousDrainCurrent", id},
                 {"onResistance", rds},
                 {"gateThresholdVoltage", {{"maximum", 2.5}}}};
    for (auto& [k, v] : elec_extra.items()) elec[k] = v;
    json di = {{"part", {{"technology", "Si"}, {"subType", "nChannel"}}}, {"electrical", elec},
               {"thermal", {{"junctionTemperatureMax", 150}, {"thermalResistanceJunctionAmbient", 50}}}};
    if (!curves.empty()) di["curves"] = curves;
    json r = {{"semiconductor",
               {{"mosfet",
                 {{"manufacturerInfo",
                   {{"name", "ACME"},
                    {"reference", mpn},
                    {"status", "production"},
                    {"datasheetUrl", "https://acme.com/" + mpn + ".pdf"},
                    {"datasheetInfo", di}}}}}}}};
    return r.dump();
}

// FULL: every value the four terms need, with all three curves.
std::string full_part(const std::string& mpn = "FULL") {
    return record(mpn, 60, 20, 0.010,
                  {{"onResistanceVgs", 10}, {"totalGateCharge", 20e-9}, {"totalGateChargeVgs", 10},
                   {"gateSourceCharge", 4e-9}, {"gateDrainCharge", 3e-9},
                   {"outputCapacitance", 200e-12}, {"capacitanceMeasurementVds", 30}},
                  {{"cossVsVds", curve("Drain-source voltage [V]", "Output capacitance [F]",
                                       {10, 30, 60}, {400e-12, 200e-12, 100e-12},
                                       {{"gateSourceVoltage", 0}, {"frequency", 1e6}})},
                   {"rdsOnVsTj", curve("Junction temperature [degC]",
                                       "Drain-source on-state resistance [ohm]", {-40, 25, 150},
                                       {0.007, 0.010, 0.018},
                                       {{"gateSourceVoltage", 10}, {"drainCurrent", 10}})},
                   {"gateCharge", curve("Gate charge [nC]", "Gate-source voltage [V]",
                                        {0, 4, 7, 20}, {0, 4.5, 4.5, 10},
                                        {{"drainSourceVoltage", 30}, {"drainCurrent", 10}})}});
}

Shard<MosfetRow> shard_of(const std::vector<std::string>& lines) {
    std::string path = std::string(KELVIN_TEST_DIR) + "/_mosfet_loss_tmp.ndjson";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        for (auto& l : lines) f << l << "\n";
    }
    auto s = build_mosfet_shard(path);
    std::remove(path.c_str());
    return s;
}

LossOperatingPoint paper_point() {
    LossOperatingPoint op;
    op.vin = 36;
    op.iout = 2;
    op.fsw = 80e3;
    op.vout = 12;
    return op;
}

MosfetConstraints loose() {
    MosfetConstraints c;
    c.vds_min = 43.2;
    c.id_min = 2;
    c.rds_on_max = 1.0;
    c.qg_max = INFINITY;
    return c;
}

const MosfetRow& row(const Shard<MosfetRow>& s, const std::string& mpn) {
    for (const auto& r : s.rows)
        if (r.mpn == mpn) return r;
    throw std::runtime_error("no row " + mpn);
}

}  // namespace

TEST_CASE("mosfet loss: every term read off the record's curves at the operating point",
          "[mosfet_loss]") {
    auto s = shard_of({full_part()});
    const MosfetRow& m = row(s, "FULL");
    LossOperatingPoint op = paper_point();
    op.ripple_pp = 0.6;
    op.vdrive = 8;
    op.idrive = 1.5;
    op.tj = 100;
    LossBreakdown b = switch_loss(m, op);
    REQUIRE(b.unverified().empty());
    CHECK(b.duty == Approx(1.0 / 3));
    // Irms = sqrt(D (Iout^2 + dI^2/12)); Rds(on) at 100 degC off the curve = 14.8 mohm.
    const double irms2 = (4.0 + 0.36 / 12) / 3;
    CHECK(*b.conduction.watts == Approx(irms2 * 0.0148).epsilon(1e-5));
    CHECK(b.conduction.basis.find("at Tj 100 degC read off the record's Rds(on)(Tj) curve") !=
          std::string::npos);
    // Vin Iout (Qgs + Qgd) / Ig fs.
    CHECK(*b.switching.watts == Approx(36.0 * 2 * 7e-9 / 1.5 * 80e3).epsilon(1e-6));
    // Qg at 8 V: first crossing above the 4.5 V plateau, 7 + 3.5/5.5*13 nC.
    const double qg8 = (7 + 3.5 / 5.5 * 13) * 1e-9;
    CHECK(*b.gate_drive.watts == Approx(qg8 * 8 * 80e3).epsilon(1e-5));
    CHECK(b.gate_drive.basis.find("gate-charge curve (curve at Vds 30 V)") != std::string::npos);
    // Coss at 36 V on the log-log curve (C ~ 1/V between 30 and 60 V) = 166.7 pF.
    CHECK(*b.coss.watts == Approx(0.5 * (200e-12 * 30 / 36) * 36 * 36 * 80e3).epsilon(1e-5));
    CHECK(b.coss.basis.find("Coss(Vds) curve (log-log)") != std::string::npos);
    // 8 V drive under the 10 V the Rds(on) is stated at: said, not hidden.
    REQUIRE(b.notes.size() == 1);
    CHECK(b.notes[0].find("conduction term is understated") != std::string::npos);
    CHECK(b.total().has_value());
}

TEST_CASE("mosfet loss: no curve -> the stated value with its basis named, never rescaled",
          "[mosfet_loss]") {
    auto s = shard_of({record("STATED", 60, 20, 0.010,
                              {{"onResistanceVgs", 10}, {"totalGateCharge", 20e-9},
                               {"gateSourceCharge", 4e-9}, {"gateDrainCharge", 3e-9},
                               {"outputCapacitance", 200e-12}, {"capacitanceMeasurementVds", 30}})});
    LossOperatingPoint op = paper_point();
    op.vdrive = 10;
    op.idrive = 1;
    op.tj = 100;  // stated, but the record has no Rds(on)(Tj) curve: 25 degC value, named
    LossBreakdown b = switch_loss(row(s, "STATED"), op);
    REQUIRE(b.unverified().empty());
    CHECK(*b.conduction.watts == Approx(4.0 / 3 * 0.010));
    CHECK(b.conduction.basis.find("25 degC") != std::string::npos);
    CHECK(b.conduction.basis.find("no Rds(on)(Tj) curve") != std::string::npos);
    CHECK(*b.coss.watts == Approx(0.5 * 200e-12 * 36 * 36 * 80e3));
    CHECK(b.coss.basis.find("at Vds 30 V, not at Vin 36 V") != std::string::npos);
    CHECK(*b.gate_drive.watts == Approx(20e-9 * 10 * 80e3));
    CHECK(b.gate_drive.basis.find("Vgs not stated") != std::string::npos);
}

TEST_CASE("mosfet loss: no gate drive given -> switching and gate terms UNVERIFIED",
          "[mosfet_loss]") {
    auto s = shard_of({full_part()});
    LossBreakdown b = switch_loss(row(s, "FULL"), paper_point());
    CHECK_FALSE(b.switching.watts.has_value());
    CHECK(b.switching.missing == std::vector<std::string>{"gate drive current not given"});
    CHECK_FALSE(b.gate_drive.watts.has_value());
    CHECK(b.gate_drive.missing == std::vector<std::string>{"gate drive voltage not given"});
    CHECK(b.unverified() == std::vector<std::string>{"switching", "gateDrive"});
    CHECK_FALSE(b.total().has_value());
    // Conduction and Coss are still computed.
    CHECK(b.conduction.watts.has_value());
    CHECK(b.coss.watts.has_value());
}

TEST_CASE("mosfet loss: a candidate with an UNVERIFIED term ranks after every computed one",
          "[mosfet_loss]") {
    // NO_QGD has a far lower Rds(on) and the same Coss, so its computed sum is lower — but its
    // switching term cannot be computed (no Qgs / Qgd), so it ranks after FULL.
    auto s = shard_of({record("NO_QGD", 60, 20, 0.001,
                              {{"totalGateCharge", 20e-9}, {"outputCapacitance", 50e-12},
                               {"capacitanceMeasurementVds", 36}}),
                       full_part()});
    MosfetConstraints c = loose();
    LossOperatingPoint op = paper_point();
    op.vdrive = 10;
    op.idrive = 1;
    c.loss_op = op;
    json r = select_mosfet(s, c, MosfetTiebreaker::LowestOperatingPointLoss);
    const json& cands = r.at("candidates");
    REQUIRE(cands.size() == 2);
    CHECK(cands[0].at("mpn") == "FULL");
    CHECK(cands[1].at("mpn") == "NO_QGD");
    CHECK(cands[1].at("loss").at("computedSum").get<double>() <
          cands[0].at("loss").at("computedSum").get<double>());
    CHECK(cands[1].at("loss").at("total").is_null());
    CHECK(cands[1].at("loss").at("unverified") == json::array({"switching"}));
    CHECK(cands[1].at("loss").at("terms").at("switching").at("missing") ==
          json::array({"Qgs not stated in the record", "Qgd not stated in the record"}));
    CHECK(cands[0].at("loss").at("total").get<double>() > 0);
    CHECK(r.at("lossOperatingPoint").at("dutyCycle").get<double>() == Approx(1.0 / 3));
    // The tiebreaker without an operating point is refused, not run on invented values.
    c.loss_op.reset();
    CHECK_THROWS_AS(select_mosfet(s, c, MosfetTiebreaker::LowestOperatingPointLoss),
                    InvalidOptions);
}

TEST_CASE("mosfet loss: the API reads lossOperatingPoint strictly", "[mosfet_loss]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(KELVIN_TEST_DIR) / "_mosfet_loss_api";
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "mosfets.ndjson", std::ios::binary | std::ios::trunc);
        f << full_part() << "\n";
    }
    api::Engine eng(dir.string(), "", /*quiet=*/true);
    const json req = {{"ratedDrainSourceVoltage", 43.2},
                      {"ratedContinuousDrainCurrent", 2},
                      {"maximumOnResistance", 1}};
    json op = {{"position", "buckHighSide"}, {"inputVoltage", 36}, {"outputCurrent", 2},
               {"switchingFrequency", 80e3}, {"outputVoltage", 12}};
    // With an operating point the loss ranking is the default.
    json r = eng.select("mosfet", req, {{"lossOperatingPoint", op}, {"includeEnvelope", false}});
    CHECK(r.at("tiebreaker") == "lowest_operating_point_loss");
    CHECK(r.at("candidates")[0].at("loss").at("unverified") ==
          json::array({"switching", "gateDrive"}));
    // A number given as a string is refused, not read as "not given".
    json bad = op;
    bad["gateDriveVoltage"] = "10";
    CHECK_THROWS_AS(eng.select("mosfet", req, {{"lossOperatingPoint", bad}}), InvalidOptions);
    // The low side is not modelled yet: refused by name.
    bad = op;
    bad["position"] = "buckLowSide";
    CHECK_THROWS_AS(eng.select("mosfet", req, {{"lossOperatingPoint", bad}}), InvalidOptions);
    // Neither duty nor Vout: refused.
    bad = op;
    bad.erase("outputVoltage");
    CHECK_THROWS_AS(eng.select("mosfet", req, {{"lossOperatingPoint", bad}}), InvalidOptions);
    CHECK_THROWS_AS(
        eng.select("mosfet", req, {{"tiebreaker", "lowest_operating_point_loss"}}),
        InvalidOptions);
    fs::remove_all(dir);
}
