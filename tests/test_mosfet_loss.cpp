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
    // Qgs+Qgd as stated (owner 2026-11-14, decision 3): the basis says it is an upper bound.
    CHECK(b.switching.basis.find("the record states no Qgs2, so this is an upper bound") !=
          std::string::npos);
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

TEST_CASE("mosfet over-rating: Vds or Id > 3x the requirement sinks, noted, never excluded",
          "[mosfet_overrating]") {
    const std::vector<std::string> lines = {
        record("HV_HIGH_ID", 600, 77.5, 0.005, {{"totalGateCharge", 20e-9}}),   // both axes
        record("HIGH_ID", 60, 100, 0.008, {{"totalGateCharge", 20e-9}}),        // Id 50x
        record("CLOSE", 60, 5, 0.020, {{"totalGateCharge", 20e-9}}),            // 1.39x / 2.5x
    };
    auto s = shard_of(lines);
    MosfetConstraints c = loose();
    // Off (the Python-parity core): pure lowest Rds(on).
    json off = select_mosfet(s, c, MosfetTiebreaker::LowestRdsOn);
    CHECK(off.at("candidates")[0].at("mpn") == "HV_HIGH_ID");
    CHECK_FALSE(off.at("candidates")[0].contains("overRating"));
    // On: one step per over-rated axis, all three still offered.
    c.over_rating_tier = true;
    json on = select_mosfet(s, c, MosfetTiebreaker::LowestRdsOn);
    const json& k = on.at("candidates");
    REQUIRE(k.size() == 3);
    CHECK(k[0].at("mpn") == "CLOSE");
    CHECK(k[1].at("mpn") == "HIGH_ID");
    CHECK(k[2].at("mpn") == "HV_HIGH_ID");
    CHECK(k[0].at("overRating").at("axes").empty());
    CHECK(k[2].at("overRating").at("axes") == json::array({"vds", "id"}));
    const std::string note = k[2].at("overRating").at("notes")[0].get<std::string>();
    CHECK(note.find("drain-source voltage rating 600 V is 13.9x the requirement's 43.2 V") !=
          std::string::npos);
    CHECK(k[2].at("ratings").at("vds").get<double>() == 600);
}

TEST_CASE("mosfet over-rating: on for every Engine recommendation", "[mosfet_overrating]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(KELVIN_TEST_DIR) / "_mosfet_overrating_api";
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "mosfets.ndjson", std::ios::binary | std::ios::trunc);
        f << record("HV", 600, 77.5, 0.005, {{"totalGateCharge", 20e-9}}) << "\n"
          << record("CLOSE", 60, 5, 0.020, {{"totalGateCharge", 20e-9}}) << "\n";
    }
    api::Engine eng(dir.string(), "", /*quiet=*/true);
    const json req = {{"ratedDrainSourceVoltage", 43.2},
                      {"ratedContinuousDrainCurrent", 2},
                      {"maximumOnResistance", 1}};
    json r = eng.select("mosfet", req, {{"includeEnvelope", false}});
    CHECK(r.at("candidates")[0].at("mpn") == "CLOSE");
    CHECK(r.at("candidates")[1].at("overRating").at("axes").size() == 2);
    fs::remove_all(dir);
}

TEST_CASE("mosfet board area and price: resolved land, stated unit cost, no invented price",
          "[mosfet_area_price]") {
    namespace fs = std::filesystem;
    json a = json::parse(record("TO247_PRICED", 600, 77.5, 0.041, {{"totalGateCharge", 170e-9}}));
    json& mi = a["semiconductor"]["mosfet"];
    mi["manufacturerInfo"]["datasheetInfo"]["part"]["case"] = "TO247";
    mi["distributorsInfo"] = json::array(
        {{{"name", "Digi-Key"}, {"cost", {{"value", 0.0}, {"currency", "USD"}}}},  // not a price
         {{"name", "Infineon"}, {"moq", 1}, {"cost", {{"value", 2.45}, {"currency", "USD"}}}}});
    const std::string bare = record("NO_CASE", 60, 5, 0.020, {{"totalGateCharge", 20e-9}});
    const fs::path dir = fs::path(KELVIN_TEST_DIR) / "_mosfet_area_price";
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "mosfets.ndjson", std::ios::binary | std::ios::trunc);
        f << a.dump() << "\n" << bare << "\n";
    }
    api::Engine eng(dir.string(), "", /*quiet=*/true);
    const json req = {{"ratedDrainSourceVoltage", 43.2},
                      {"ratedContinuousDrainCurrent", 2},
                      {"maximumOnResistance", 1}};
    json r = eng.select("mosfet", req, {{"includeEnvelope", false}, {"priceQuantity", 1000}});
    json to247, nocase;
    for (const auto& c : r.at("candidates")) (c.at("mpn") == "NO_CASE" ? nocase : to247) = c;
    REQUIRE(to247.at("boardArea").at("mm2").is_number());
    CHECK(to247.at("boardArea").at("mm2").get<double>() > 50);
    CHECK(to247.at("boardArea").at("basis").get<std::string>().find("'TO247'") != std::string::npos);
    CHECK(nocase.at("boardArea").at("mm2").is_null());
    CHECK(nocase.at("boardArea").at("basis").get<std::string>().rfind("not stated", 0) == 0);
    const json& p = to247.at("price");
    CHECK(p.at("quantity").get<double>() == 1000);
    CHECK(p.at("atQuantity").is_null());  // the record has no price breaks
    REQUIRE(p.at("quotes").size() == 1);   // the 0.0 quote is not a price
    CHECK(p.at("quotes")[0].at("distributor") == "Infineon");
    CHECK(p.at("quotes")[0].at("unitCost").get<double>() == Approx(2.45));
    CHECK(nocase.at("price").at("quotes").empty());
    CHECK_THROWS_AS(eng.select("mosfet", req, {{"priceQuantity", "1000"}}), InvalidOptions);
    fs::remove_all(dir);
}

// Owner 2026-11-14 (decision 2): under lowest_operating_point_loss the total loss alone sets the
// order after the UNVERIFIED key — over-rating is a flag and a note, and the evidence / thermal
// tiers do not reorder it; under highest_vds_margin / highest_id_margin the over-rating tier is
// off; every other ranking keeps it. Real records from the frozen catalogue (data-524a699):
//   CSD19537Q3       TI, 100 V / 12 A, 19 mohm — Id over-rated (6x)
//   IGLD60R190D1     Infineon GaN, 600 V / 5 A — Vds over-rated (13.9x)
//   NTMFD5C680NLT1G  onsemi dual, 60 V / 20 A — Id over-rated, no thermal data, datasheet unusable
//   AO4286           AOS, 100 V / 4 A, 68 mohm — not over-rated
TEST_CASE("mosfet over-rating: a flag under the loss and margin rankings, a tier elsewhere",
          "[mosfet_overrating]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(KELVIN_TEST_DIR) / "_mosfet_op_loss_ranking";
    fs::create_directories(dir);
    fs::copy_file(fs::path(KELVIN_TEST_DIR) / "fixtures" / "mosfets_op_loss_ranking.ndjson",
                  dir / "mosfets.ndjson", fs::copy_options::overwrite_existing);
    api::Engine eng(dir.string(), "", /*quiet=*/true);
    const json req = {{"ratedDrainSourceVoltage", 43.2},
                      {"ratedContinuousDrainCurrent", 2},
                      {"maximumOnResistance", 1}};
    auto mpns = [](const json& r) {
        std::vector<std::string> v;
        for (const auto& c : r.at("candidates")) v.push_back(c.at("mpn").get<std::string>());
        return v;
    };
    auto note_of = [](const json& r, const std::string& mpn) {
        for (const auto& c : r.at("candidates"))
            if (c.at("mpn") == mpn) return c.at("overRating").at("notes")[0].get<std::string>();
        throw std::runtime_error("no " + mpn);
    };

    // Operating-point loss (Su et al. point, 10 V / 1 A drive): loss order, over-rated or not.
    const json op = {{"position", "buckHighSide"}, {"inputVoltage", 36}, {"outputVoltage", 12},
                     {"outputCurrent", 2}, {"switchingFrequency", 80e3},
                     {"gateDriveVoltage", 10}, {"gateDriveCurrent", 1}};
    json r = eng.select("mosfet", req, {{"lossOperatingPoint", op}, {"includeEnvelope", false}});
    REQUIRE(r.at("candidates").size() == 4);
    const std::vector<std::string> by_loss = mpns(r);
    // Owner 2026-11-14 (size and voltage, a): a board area that cannot be determined counts one
    // more UNVERIFIED item — NTMFD5C680NLT1G ("SO-8FL Dual / DFN-8", no drawn size) ranks after
    // the three whose areas resolve. CSD19537Q3's "SON (3x3)" resolves since the stated-body
    // rule (owner 2026-11-14), so it takes the place its loss gives it.
    CHECK(by_loss == std::vector<std::string>{"CSD19537Q3", "AO4286", "IGLD60R190D1",
                                              "NTMFD5C680NLT1G"});
    double prev = 0;
    size_t prev_items = 0;
    for (const auto& c : r.at("candidates")) {
        const size_t items = c.at("sortKey").at("unverifiedItems").get<size_t>();
        if (items != prev_items) prev = 0;  // a new UNVERIFIED group: the loss restarts
        prev_items = items;
        if (c.at("loss").at("total").is_null()) continue;  // UNVERIFIED ranks after: not here
        CHECK(c.at("loss").at("total").get<double>() >= prev);
        prev = c.at("loss").at("total").get<double>();
    }
    const json* csd = nullptr;
    for (const auto& c : r.at("candidates"))
        if (c.at("mpn") == "CSD19537Q3") csd = &c;
    REQUIRE(csd);
    CHECK(csd->at("overRating").at("axes") == json::array({"id"}));
    // Decision 3: CSD19537Q3's switching term is Qgs+Qgd as stated, named an upper bound.
    CHECK(csd->at("loss").at("terms").at("switching").at("basis")
              .get<std::string>().find("the record states no Qgs2, so this is an upper bound") !=
          std::string::npos);
    CHECK(note_of(r, "CSD19537Q3").find("flagged only: the operating-point loss alone sets the order") !=
          std::string::npos);

    // lowest_rds_on keeps the tier: AO4286 (not over-rated) first, the incomplete record last.
    r = eng.select("mosfet", req, {{"tiebreaker", "lowest_rds_on"}, {"includeEnvelope", false}});
    CHECK(mpns(r) == std::vector<std::string>{"AO4286", "CSD19537Q3", "IGLD60R190D1",
                                              "NTMFD5C680NLT1G"});
    CHECK(note_of(r, "CSD19537Q3").find("ranked after closer-rated parts") != std::string::npos);

    // Vds / Id margin rankings: no tier — the highest rating is what was asked for.
    r = eng.select("mosfet", req, {{"tiebreaker", "highest_vds_margin"}, {"includeEnvelope", false}});
    CHECK(mpns(r).front() == "IGLD60R190D1");
    CHECK(note_of(r, "IGLD60R190D1").find("flagged only: this ranking does not demote it") !=
          std::string::npos);
    r = eng.select("mosfet", req, {{"tiebreaker", "highest_id_margin"}, {"includeEnvelope", false}});
    CHECK(mpns(r).front() == "CSD19537Q3");
    fs::remove_all(dir);
}

// Owner 2026-11-14 (size and voltage, a): under the operating-point ranking a candidate whose
// board area cannot be determined counts one more UNVERIFIED item (op_ranking, every family).
// CSD19537Q3's "SON (3x3)" resolves since the stated-body rule, so the guard uses a real record
// whose case still resolves to no land (owner 2026-11-14, approved fixture change):
// mosfets_op_size.ndjson is mosfets_op_loss_ranking.ndjson plus NTMFD016N06CT1G (data-524a699,
// verbatim), case "SO-8FL Dual / DFN-8" (an onsemi name, no registered outline), no drawn size.
// Every loss term is computed and its loss (0.062 W) is below AO4286's (SO8, drawn 4.9 x 3.9 mm)
// and IGLD60R190D1's (drawn 13 x 7.6 mm); only the size item ranks it after both.
TEST_CASE("mosfet op loss: a board area that cannot be determined is one more UNVERIFIED item",
          "[mosfet_loss][op_size]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(KELVIN_TEST_DIR) / "_mosfet_op_size";
    fs::create_directories(dir);
    fs::copy_file(fs::path(KELVIN_TEST_DIR) / "fixtures" / "mosfets_op_size.ndjson",
                  dir / "mosfets.ndjson", fs::copy_options::overwrite_existing);
    api::Engine eng(dir.string(), "", /*quiet=*/true);
    const json req = {{"ratedDrainSourceVoltage", 43.2},
                      {"ratedContinuousDrainCurrent", 2},
                      {"maximumOnResistance", 1}};
    const json op = {{"position", "buckHighSide"}, {"inputVoltage", 36}, {"outputVoltage", 12},
                     {"outputCurrent", 2}, {"switchingFrequency", 80e3},
                     {"gateDriveVoltage", 10}, {"gateDriveCurrent", 1}};
    json r = eng.select("mosfet", req, {{"lossOperatingPoint", op}, {"includeEnvelope", false}});
    auto index_of = [&](const std::string& mpn) -> size_t {
        for (size_t k = 0; k < r.at("candidates").size(); ++k)
            if (r.at("candidates")[k].at("mpn") == mpn) return k;
        throw std::runtime_error("no " + mpn);
    };
    const json& nt = r.at("candidates")[index_of("NTMFD016N06CT1G")];
    const json& ao = r.at("candidates")[index_of("AO4286")];
    const json& ig = r.at("candidates")[index_of("IGLD60R190D1")];
    CHECK(nt.at("boardArea").at("mm2").is_null());
    CHECK(nt.at("loss").at("unverified").empty());  // every loss term computed
    CHECK(nt.at("sortKey").at("sizeUnverified") == true);
    CHECK(nt.at("sortKey").at("unverifiedItems") == 1);
    CHECK(nt.at("loss").at("notes").dump().find(
              "size UNVERIFIED (board area not stated: no drawn L x W and no resolvable case code "
              "('SO-8FL Dual / DFN-8')): counts one more UNVERIFIED item in the operating-point "
              "ranking") != std::string::npos);
    CHECK(ao.at("sortKey").at("sizeUnverified") == false);
    CHECK(ao.at("sortKey").at("unverifiedItems") == 0);
    CHECK(nt.at("loss").at("total").get<double>() < ao.at("loss").at("total").get<double>());
    CHECK(nt.at("loss").at("total").get<double>() < ig.at("loss").at("total").get<double>());
    CHECK(index_of("NTMFD016N06CT1G") > index_of("AO4286"));
    CHECK(index_of("NTMFD016N06CT1G") > index_of("IGLD60R190D1"));
    // A legacy ranking has no such key.
    json legacy = eng.select("mosfet", req, {{"tiebreaker", "lowest_rds_on"}, {"includeEnvelope", false}});
    CHECK_FALSE(legacy.at("candidates")[0].at("sortKey").contains("unverifiedItems"));
    fs::remove_all(dir);
}
