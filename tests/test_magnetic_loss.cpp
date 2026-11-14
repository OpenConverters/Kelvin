// [magnetic_loss] Operating-point ranking of power inductors (owner 2026-11-14): ripple from
// the candidate's own L (read at Ipk off its L(I) curve), saturation and rated current as hard
// gates, the DCR loss ordering, core loss UNVERIFIED. Every record is a REAL catalogue line
// (tests/fixtures/magnetics_op_loss.ndjson, copied verbatim from the frozen TAS data 524a699).
//
// Operating point throughout: the Su et al. buck at its 36 V corner — Vin 36 V, Vout 12 V,
// Iout 2 A, fs 80 kHz, so D = 1/3 and (Vin-Vout)*D/fs = 1e-4 V*s.
#include <cmath>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "Index.hpp"
#include "KelvinApi.hpp"
#include "MagneticLoss.hpp"
#include "Select.hpp"

using namespace kelvin;
using nlohmann::json;
using Catch::Approx;

namespace {

const Shard<MagneticRow>& fixture_shard() {
    static const Shard<MagneticRow> s =
        build_magnetic_shard(std::string(KELVIN_TEST_DIR) + "/fixtures/magnetics_op_loss.ndjson");
    return s;
}

const MagneticRow& row(const std::string& mpn) {
    for (const auto& r : fixture_shard().rows)
        if (r.mpn == mpn) return r;
    throw std::runtime_error("fixture row missing: " + mpn);
}

InductorOperatingPoint su36() {
    InductorOperatingPoint op;
    op.vin = 36;
    op.vout = 12;
    op.iout = 2;
    op.fsw = 80e3;
    return op;
}

MagneticConstraints su_constraints() {
    MagneticConstraints c;
    c.target_inductance = 166.7e-6;
    c.loss_op = su36();
    return c;
}

std::vector<std::string> mpns(const json& r) {
    std::vector<std::string> v;
    for (const auto& c : r.at("candidates")) v.push_back(c.at("mpn").get<std::string>());
    return v;
}

}  // namespace

TEST_CASE("magnetic loss: no L(I) curve -> nominal L, DCR loss on the stated maximum",
          "[magnetic_loss]") {
    // 760800301 (Wuerth WE-TORPFC 180 uH, the paper's pick): DCR max 20 mohm, Isat 43 A @30 %,
    // rated 48 A, no L(I) curve.
    const auto b = magnetic_loss(row("760800301"), su36());
    CHECK(b.duty == Approx(1.0 / 3));
    CHECK(b.inductance == Approx(180e-6));
    CHECK(b.ripple_pp == Approx(1e-4 / 180e-6));
    CHECK(b.ipk == Approx(2 + 0.5 * 1e-4 / 180e-6));
    const double dI = 1e-4 / 180e-6;
    const double irms = std::sqrt(4 + dI * dI / 12);
    CHECK(b.irms == Approx(irms));
    REQUIRE(b.dcr.watts);
    CHECK(*b.dcr.watts == Approx(irms * irms * 0.020));
    CHECK(b.dcr.basis.find("the record's stated maximum") != std::string::npos);
    CHECK(b.inductance_basis.find("states no L(I) curve") != std::string::npos);
    CHECK_FALSE(b.core.watts);
    CHECK(b.saturation.str() == std::string("pass"));
    CHECK(b.saturation.basis.find("at 30 % inductance drop") != std::string::npos);
    CHECK(b.thermal.str() == std::string("pass"));
    CHECK_FALSE(b.total());  // core loss UNVERIFIED -> no total
    CHECK(b.rank_unverified() == 0);
}

TEST_CASE("magnetic loss: L at Ipk read off the record's L(I) curve, solved with the ripple",
          "[magnetic_loss]") {
    // 74437529203221 (Wuerth, 220 uH): L(I) at 20 degC, 0.98455 x L(0) at 2.2 A and
    // 0.97091 x at 2.82 A. At this point Ipk ~ 2.231 A, inside that segment.
    const MagneticRow m = row("74437529203221");  // a copy: the fixture shard is static
    REQUIRE(m.bias_curve_current.size() == 18);
    const auto b = magnetic_loss(m, su36());
    const double r22 = 0.0002166 / 0.00022, r282 = 0.0002136 / 0.00022;
    const double ratio = r22 + (b.ipk - 2.2) / (2.82 - 2.2) * (r282 - r22);
    CHECK(b.ipk > 2.2);
    CHECK(b.ipk < 2.82);
    CHECK(b.inductance == Approx(220e-6 * ratio).epsilon(1e-9));
    // Self-consistent: the Ipk the L was read at is the Ipk that L produces.
    CHECK(b.ipk == Approx(2 + 0.5 * 1e-4 / b.inductance).epsilon(1e-9));
    CHECK(b.inductance < 220e-6);
    CHECK(b.inductance_basis.find("read off the record's L(I) curve") != std::string::npos);
    REQUIRE(b.dcr.watts);
    CHECK(*b.dcr.watts == Approx(b.irms * b.irms * 0.0401));
}

TEST_CASE("magnetic loss: an operating point beyond the measured L(I) curve is not extrapolated",
          "[magnetic_loss]") {
    // Same record, Iout 10 A: the curve ends at 9.88 A. Ipk lies beyond it, so L at Ipk is not
    // read; Isat 4.1 A is below even the lower bound of Ipk -> saturation FAIL.
    auto op = su36();
    op.iout = 10;
    const auto b = magnetic_loss(row("74437529203221"), op);
    CHECK(b.inductance == Approx(220e-6));
    CHECK(b.inductance_basis.find("not read and not extrapolated") != std::string::npos);
    CHECK(b.saturation.failed());
}

TEST_CASE("magnetic loss: saturation and rated current checks", "[magnetic_loss]") {
    // 744115 (Wuerth 200 uH): Isat 0.9 A @10 % < Ipk ~2.25 A -> FAIL.
    CHECK(magnetic_loss(row("744115"), su36()).saturation.failed());
    // 760804110 (Wuerth 250 uH): Isat 5.6 A passes, rated 1.1 A < Irms ~2.0 A -> FAIL.
    const auto t = magnetic_loss(row("760804110"), su36());
    CHECK(t.saturation.str() == std::string("pass"));
    CHECK(t.thermal.failed());
    // 1130-181K-RC (Bourns 180 uH): no Isat, no L(I) curve -> saturation UNVERIFIED.
    const auto u = magnetic_loss(row("1130-181K-RC"), su36());
    CHECK(u.saturation.unverified());
    CHECK(u.rank_unverified() == 1);
    // PE-51516NL (Pulse 188 uH): DCR stated as a nominal only -> the basis says so.
    const auto n = magnetic_loss(row("PE-51516NL"), su36());
    REQUIRE(n.dcr.watts);
    CHECK(n.dcr.basis.find("nominal / typical: it states no maximum") != std::string::npos);
}

TEST_CASE("magnetic op-loss selector: power inductors only, hard gates counted, DCR loss orders, "
          "unverified after",
          "[magnetic_loss]") {
    const json r = select_magnetic_op_loss(fixture_shard(), su_constraints(), 20);
    CHECK(r.at("tiebreaker") == "lowest_operating_point_loss");
    const json& rej = r.at("rejections");
    CHECK(rej.value("subtype_transformer", 0) == 1);               // 750811351
    CHECK(rej.value("inductance_below_required", 0) == 1);         // 750315320 (160 uH)
    CHECK(rej.value("saturation_current_below_peak", 0) == 1);     // 744115
    CHECK(rej.value("rated_current_below_rms", 0) == 1);           // 760804110
    // Fully verified ones by DCR loss (20 mohm, 40.1 mohm, 98 mohm), then the two whose
    // saturation is UNVERIFIED (5.5 mohm, 48 mohm), however low their DCR loss.
    CHECK(mpns(r) == std::vector<std::string>{"760800301", "74437529203221", "MSS1812T-224",
                                              "1130-181K-RC", "PE-51516NL"});
    const json& top = r.at("candidates").at(0);
    CHECK(top.at("loss").at("checks").at("saturation").at("status") == "pass");
    CHECK(top.at("loss").at("terms").at("coreLoss").at("watts").is_null());
    CHECK(top.at("loss").at("total").is_null());
    // Over-rating is a flag: 43 A Isat and 48 A rated against ~2.3 A / ~2.0 A.
    CHECK(top.at("overRating").at("axes") == json::array({"isat", "ratedCurrent"}));
    CHECK(top.contains("boardArea"));
    CHECK(top.contains("price"));
}

TEST_CASE("magnetic op-loss: the API reads lossOperatingPoint strictly", "[magnetic_loss]") {
    MagneticConstraints c = su_constraints();
    c.loss_op.reset();
    CHECK_THROWS_AS(select_magnetic_op_loss(fixture_shard(), c, 5), InvalidOptions);
    c = su_constraints();
    c.target_inductance.reset();
    CHECK_THROWS_AS(select_magnetic_op_loss(fixture_shard(), c, 5), InvalidOptions);
    c = su_constraints();
    c.loss_op->vout = 40;  // not a buck
    CHECK_THROWS_AS(select_magnetic_op_loss(fixture_shard(), c, 5), InvalidOptions);
    CHECK_THROWS_AS(inductor_position_from_string("boostInductor"), InvalidOptions);
    CHECK(magnetic_tiebreaker_from_string("best_fit") == MagneticTiebreaker::BestFit);
}
