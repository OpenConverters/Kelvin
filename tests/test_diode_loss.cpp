// [diode_loss] Operating-point loss ranking of a buck freewheel diode (owner 2026-11-14): every
// term from the record's datasheet values at the caller's operating point, each with its basis;
// a missing input makes its term UNVERIFIED (never a guessed number) and such a candidate ranks
// after the fully computed ones. Over-rating (Vrrm / If(avg) > 3x) is a flag under this ranking
// and a tier under lowest_vf / lowest_qrr.
//
// Real records from the frozen catalogue (TAS 524a699), tests/fixtures/diodes_op_loss_ranking.ndjson:
//   STPS8045       ST schottky 45 V / 80 A, Vf 0.4 V at 80 A, Ir 1.2 mA, Cj 3 nF at 10 V, Digi-Key 4.95 USD
//   ES3J           onsemi ultrafast 600 V / 3 A, Vf 1.7 V at 3 A, Qrr 20 nC, Ir 10 uA
//   S2T-CT         SMC fastRecovery 1300 V / 2 A, Vf 1.7 V at 2 A, trr 150 ns, no Qrr, Ir 5 uA
//   VS-40CPQ045    Vishay schottky dual 45 V / 20 A per leg, no Ir
//   STPS30L60CT    ST schottky dual 60 V / 15 A per leg, Vf 0.42 V at 15 A, Ir 0.3 mA
//   S2A            YAGEO 50 V / 2 A, Vf 1.15 V, no subtype
//   C3D10065A      Wolfspeed sicSchottky 650 V / 10 A, Ir 33 uA, stated Cj only
//   MSS1P6HM3_A/H  Vishay schottky 60 V / 1 A (below the 1.667 A floor: gated out)
//   STPS3L60       ST schottky 60 V / 3 A, Vf 0.55 V at 3 A, Ir 0.3 mA: not over-rated, thermal stated
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "DiodeLoss.hpp"
#include "Index.hpp"
#include "KelvinApi.hpp"
#include "Select.hpp"

using namespace kelvin;
using nlohmann::json;
using Catch::Approx;

namespace {

const std::string kFixture =
    std::string(KELVIN_TEST_DIR) + "/fixtures/diodes_op_loss_ranking.ndjson";

const Shard<DiodeRow>& fixture_shard() {
    static const Shard<DiodeRow> s = build_diode_shard(kFixture);
    return s;
}

const DiodeRow& row(const std::string& mpn) {
    for (const auto& r : fixture_shard().rows)
        if (r.mpn == mpn) return r;
    throw std::runtime_error("no row " + mpn);
}

// Su et al. buck at 36 V in: Vout 12 V (D = 1/3), Iout 2 A, fs 80 kHz.
DiodeLossOperatingPoint paper_point() {
    DiodeLossOperatingPoint op;
    op.vin = 36;
    op.iout = 2;
    op.fsw = 80e3;
    op.vout = 12;
    return op;
}

// Vrrm >= 1.2 x 36 V; If(avg) >= 1.25 x the 1.333 A average at 36 V.
DiodeConstraints su_constraints() {
    DiodeConstraints c;
    c.vrrm_min = 43.2;
    c.if_avg_min = 1.25 * 2.0 * (1 - 12.0 / 36);
    return c;
}

bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

std::vector<std::string> mpns(const json& r) {
    std::vector<std::string> v;
    for (const auto& c : r.at("candidates")) v.push_back(c.at("mpn").get<std::string>());
    return v;
}

const json& cand(const json& r, const std::string& mpn) {
    for (const auto& c : r.at("candidates"))
        if (c.at("mpn") == mpn) return c;
    throw std::runtime_error("no candidate " + mpn);
}

}  // namespace

TEST_CASE("diode loss: the record's leakage current and unit cost reach the row", "[diode_loss]") {
    CHECK(row("STPS8045").reverse_leakage == Approx(1.2e-3));
    CHECK(row("ES3J").reverse_leakage == Approx(1e-5));
    CHECK(std::isnan(row("VS-40CPQ045").reverse_leakage));  // not stated: NaN, never 0
    CHECK(json::parse(row("STPS8045").price_quotes) ==
          json::parse(R"([{"distributor":"Digi-Key","unitCost":4.95,"currency":"USD"}])"));
    CHECK(row("S2T-CT").price_quotes.empty());
}

TEST_CASE("diode loss: a PN diode with every value stated is fully computed", "[diode_loss]") {
    const DiodeLossBreakdown b = diode_loss(row("ES3J"), paper_point());
    REQUIRE(b.unverified().empty());
    CHECK(b.duty == Approx(1.0 / 3));
    CHECK(b.i_avg == Approx(4.0 / 3));
    // Vf 1.7 V (stated at 3 A, within 2x of the 2 A conducted) x If(avg) 1.333 A.
    CHECK(*b.conduction.watts == Approx(1.7 * 4.0 / 3));
    CHECK(has(b.conduction.basis, "stated Vf 1.7 V at If 3 A, temperature not stated"));
    CHECK_FALSE(has(b.conduction.basis, "more than 2x"));
    // Qrr 20 nC x 36 V x 80 kHz.
    CHECK(*b.switching.watts == Approx(20e-9 * 36 * 80e3));
    CHECK(has(b.switching.basis, "stated Qrr 20 nC (the record states none of its test conditions"));
    // Ir 10 uA x 36 V x D.
    CHECK(*b.leakage.watts == Approx(10e-6 * 36 / 3));
    CHECK(has(b.leakage.basis, "neither its reverse voltage nor its temperature"));
    CHECK(*b.total() == Approx(1.7 * 4.0 / 3 + 20e-9 * 36 * 80e3 + 10e-6 * 36 / 3));
    // A scalar Vf is the same at every current, so the ripple does not change the term.
    DiodeLossOperatingPoint op = paper_point();
    op.ripple_pp = 0.8;
    CHECK(*diode_loss(row("ES3J"), op).conduction.watts == Approx(*b.conduction.watts));
}

TEST_CASE("diode loss: a Schottky with no Cj(Vr) curve has no switching term", "[diode_loss]") {
    const DiodeLossBreakdown b = diode_loss(row("STPS8045"), paper_point());
    CHECK(b.unverified() == std::vector<std::string>{"switching"});
    REQUIRE(b.switching.missing.size() == 1);
    CHECK(has(b.switching.missing[0], "the record has no Cj(Vr) curve (SAS states no Qc)"));
    CHECK(has(b.switching.missing[0], "the single stated Cj 3000 pF at 10 V does not give the charge"));
    // Vf stated at 80 A, 40x the 2 A it conducts: computed, and the basis and a note say so.
    CHECK(*b.conduction.watts == Approx(0.4 * 4.0 / 3));
    CHECK(has(b.conduction.basis, "at If 80 A, more than 2x from the 2 A it conducts here"));
    REQUIRE(b.notes.size() == 1);
    CHECK(has(b.notes[0], "Vf is stated at 80 A, more than 2x from the 2 A"));
    CHECK(*b.leakage.watts == Approx(1.2e-3 * 36 / 3));
    CHECK_FALSE(b.total());
}

TEST_CASE("diode loss: missing Qrr, missing Ir, missing subtype are each UNVERIFIED",
          "[diode_loss]") {
    const DiodeLossBreakdown s2t = diode_loss(row("S2T-CT"), paper_point());
    CHECK(s2t.unverified() == std::vector<std::string>{"switching"});
    CHECK(s2t.switching.missing ==
          std::vector<std::string>{
              "Qrr not stated in the record (trr 150 ns is stated; Qrr is not derived from it)"});
    const DiodeLossBreakdown vs = diode_loss(row("VS-40CPQ045"), paper_point());
    CHECK(vs.unverified() == std::vector<std::string>{"switching", "leakage"});
    CHECK(vs.leakage.missing == std::vector<std::string>{"Ir not stated in the record"});
    bool per_diode = false;
    for (const auto& n : vs.notes)
        per_diode |= has(n, "per diode (dual): the loss is for one diode carrying the whole current");
    CHECK(per_diode);
    const DiodeLossBreakdown s2a = diode_loss(row("S2A"), paper_point());
    CHECK(s2a.unverified() == std::vector<std::string>{"switching", "leakage"});
    CHECK(has(s2a.switching.missing[0], "the record states no diode subtype"));
    CHECK(has(s2a.conduction.basis, "(test current not stated)"));
}

TEST_CASE("diode loss: a candidate with an UNVERIFIED term ranks after every computed one",
          "[diode_loss]") {
    DiodeConstraints c = su_constraints();
    c.loss_op = paper_point();
    json r = select_diode(fixture_shard(), c, DiodeTiebreaker::LowestOperatingPointLoss);
    const std::vector<std::string> order = mpns(r);
    // MSS1P6HM3_A/H (1 A) is gated out; ES3J is the only one with all three terms.
    REQUIRE(order.size() == 8);
    CHECK(order[0] == "ES3J");
    CHECK(cand(r, "ES3J").at("loss").at("total").get<double>() > 2.0);
    // STPS8045's computed sum is far lower, but its switching term is UNVERIFIED.
    CHECK(cand(r, "STPS8045").at("loss").at("computedSum").get<double>() <
          cand(r, "ES3J").at("loss").at("total").get<double>());
    CHECK(cand(r, "STPS8045").at("loss").at("total").is_null());
    // UNVERIFIED-count key first, then the computed sum, ascending.
    size_t prev_u = 0;
    double prev_sum = 0;
    for (const auto& k : r.at("candidates")) {
        const size_t u = k.at("loss").at("unverified").size();
        const double sum = k.at("loss").at("computedSum").get<double>();
        CHECK(u >= prev_u);
        if (u == prev_u) CHECK(sum >= prev_sum);
        prev_u = u;
        prev_sum = sum;
    }
    CHECK(r.at("lossOperatingPoint").at("position") == "buckFreewheel");
    CHECK(r.at("lossOperatingPoint").at("dutyCycle").get<double>() == Approx(1.0 / 3));
    c.loss_op.reset();
    CHECK_THROWS_AS(select_diode(fixture_shard(), c, DiodeTiebreaker::LowestOperatingPointLoss),
                    InvalidOptions);
}

TEST_CASE("diode over-rating: a flag under the loss and margin rankings, a tier under lowest_vf",
          "[diode_overrating]") {
    DiodeConstraints c = su_constraints();
    // Off (the Python-parity core): pure lowest Vf, STPS8045 (0.4 V) first, no overRating key.
    json off = select_diode(fixture_shard(), c, DiodeTiebreaker::LowestVf);
    CHECK(off.at("candidates")[0].at("mpn") == "STPS8045");
    CHECK_FALSE(off.at("candidates")[0].contains("overRating"));
    c.over_rating_tier = true;
    json on = select_diode(fixture_shard(), c, DiodeTiebreaker::LowestVf);
    const json& st = cand(on, "STPS8045");
    CHECK(st.at("overRating").at("axes") == json::array({"if_avg"}));
    CHECK(has(st.at("overRating").at("notes")[0].get<std::string>(),
              "average forward current rating 80 A is 48x the requirement's 1.66667 A"));
    CHECK(has(st.at("overRating").at("notes")[0].get<std::string>(),
              "ranked after closer-rated parts"));
    // Within a thermal tier, a part with fewer over-rated axes comes first.
    for (size_t i = 1; i < on.at("candidates").size(); ++i) {
        const json& a = on.at("candidates")[i - 1];
        const json& b = on.at("candidates")[i];
        if (a.at("evidence").at("thermalPresent") == b.at("evidence").at("thermalPresent"))
            CHECK(a.at("overRating").at("axes").size() <= b.at("overRating").at("axes").size());
    }
    // STPS3L60 (60 V / 3 A, Vf 0.55 V) is the closest-rated part with thermal data: first.
    CHECK(on.at("candidates")[0].at("mpn") == "STPS3L60");
    CHECK(on.at("candidates")[0].at("overRating").at("axes").empty());
    // Under the operating-point loss: flagged, the loss alone orders.
    c.loss_op = paper_point();
    json op = select_diode(fixture_shard(), c, DiodeTiebreaker::LowestOperatingPointLoss);
    CHECK(op.at("candidates")[0].at("mpn") == "ES3J");
    CHECK(op.at("candidates")[0].at("overRating").at("axes") == json::array({"vrrm"}));
    CHECK(has(cand(op, "STPS8045").at("overRating").at("notes")[0].get<std::string>(),
              "flagged only: the operating-point loss alone sets the order"));
    // Under a margin ranking: flagged, not demoted.
    json m = select_diode(fixture_shard(), c, DiodeTiebreaker::HighestIfMargin);
    CHECK(m.at("candidates")[0].at("mpn") == "STPS8045");
    CHECK(has(m.at("candidates")[0].at("overRating").at("notes")[0].get<std::string>(),
              "flagged only: this ranking does not demote it"));
}

TEST_CASE("diode board area and price: resolved land, stated unit cost, no invented price",
          "[diode_area_price]") {
    DiodeConstraints c = su_constraints();
    c.price_quantity = 1000;
    json r = select_diode(fixture_shard(), c, DiodeTiebreaker::LowestVf);
    const json& st = cand(r, "STPS8045");
    // The record draws 10.3 x 5.6 mm.
    CHECK(st.at("boardArea").at("mm2").get<double>() == Approx(10.3 * 5.6));
    CHECK(st.at("boardArea").at("basis") == "the record's drawn length x width");
    const json& p = st.at("price");
    CHECK(p.at("quantity").get<double>() == 1000);
    CHECK(p.at("atQuantity").is_null());
    REQUIRE(p.at("quotes").size() == 1);
    CHECK(p.at("quotes")[0].at("unitCost").get<double>() == Approx(4.95));
    CHECK(cand(r, "S2T-CT").at("price").at("quotes").empty());
    CHECK(st.at("ratings").at("vrrm").get<double>() == 45);
    CHECK(st.at("ratings").at("ifAvg").get<double>() == 80);
    CHECK(st.at("ratings").at("subType") == "schottky");
}

TEST_CASE("diode loss: the API reads lossOperatingPoint strictly", "[diode_loss]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(KELVIN_TEST_DIR) / "_diode_loss_api";
    fs::create_directories(dir);
    fs::copy_file(kFixture, dir / "diodes.ndjson", fs::copy_options::overwrite_existing);
    api::Engine eng(dir.string(), "", /*quiet=*/true);
    const json req = {{"ratedReverseVoltage", 43.2}, {"ratedForwardCurrent", 1.25 * 4.0 / 3}};
    const json op = {{"position", "buckFreewheel"}, {"inputVoltage", 36}, {"outputVoltage", 12},
                     {"outputCurrent", 2}, {"switchingFrequency", 80e3}};
    // With an operating point the loss ranking is the default, and over-rating is flagged.
    json r = eng.select("diode", req, {{"lossOperatingPoint", op}, {"includeEnvelope", false}});
    CHECK(r.at("tiebreaker") == "lowest_operating_point_loss");
    CHECK(r.at("candidates")[0].at("mpn") == "ES3J");
    CHECK(r.at("candidates")[0].at("overRating").at("axes") == json::array({"vrrm"}));
    // The Engine ranks lowest_vf with the over-rating tier on: STPS8045 is no longer first.
    json vf = eng.select("diode", req, {{"includeEnvelope", false}});
    CHECK(vf.at("tiebreaker") == "lowest_vf");
    CHECK(vf.at("candidates")[0].at("mpn") == "STPS3L60");
    json bad = op;
    bad["gateDriveVoltage"] = 10;  // another family's key: refused, not ignored
    CHECK_THROWS_AS(eng.select("diode", req, {{"lossOperatingPoint", bad}}), InvalidOptions);
    bad = op;
    bad["inputVoltage"] = "36";  // a string is not a number
    CHECK_THROWS_AS(eng.select("diode", req, {{"lossOperatingPoint", bad}}), InvalidOptions);
    for (const char* p : {"buckHighSide", "boostOutput", "synchronousRectifier"}) {
        bad = op;
        bad["position"] = p;  // refused by name
        CHECK_THROWS_AS(eng.select("diode", req, {{"lossOperatingPoint", bad}}), InvalidOptions);
    }
    bad = op;
    bad.erase("position");
    CHECK_THROWS_AS(eng.select("diode", req, {{"lossOperatingPoint", bad}}), InvalidOptions);
    bad = op;
    bad["inductorRippleCurrent"] = 4.5;  // > 2 x Iout: DCM, not modelled
    CHECK_THROWS_AS(eng.select("diode", req, {{"lossOperatingPoint", bad}}), InvalidOptions);
    CHECK_THROWS_AS(eng.select("diode", req, {{"tiebreaker", "lowest_operating_point_loss"}}),
                    InvalidOptions);
    fs::remove_all(dir);
}
