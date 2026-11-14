// [magnetic_loss] Operating-point ranking of power inductors (owner 2026-11-14): ripple from
// the candidate's own L (read at Ipk off its L(I) curve), saturation and rated current as hard
// gates, the DCR loss ordering, core loss UNVERIFIED. Every record is a REAL catalogue line
// (tests/fixtures/magnetics_op_loss.ndjson, copied verbatim from the frozen TAS data 524a699).
//
// Operating point throughout: the Su et al. buck at its 36 V corner — Vin 36 V, Vout 12 V,
// Iout 2 A, fs 80 kHz, so D = 1/3 and (Vin-Vout)*D/fs = 1e-4 V*s.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
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
    op.mode = ConductionMode::Ccm;
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
    const auto b = magnetic_loss(row("760800301"), su36(), 166.7e-6);
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
    // Owner 2026-11-14 (decision 3): its only Isat point is at 30 % drop, an upper bound of
    // the 20 % current, which cannot pass the gate (was: pass on 43 A @30 %).
    CHECK(b.saturation.unverified());
    REQUIRE(!b.saturation.missing.empty());  // .front() below
    CHECK(b.saturation.missing.front().find("43 A at 30 % drop") != std::string::npos);
    // Decision 4: its rated current carries no temperature rise in this index (was: pass).
    CHECK(b.thermal.unverified());
    CHECK_FALSE(b.total());  // core loss UNVERIFIED -> no total
    CHECK(b.rank_unverified() == 2);  // was 0
}

TEST_CASE("magnetic loss: L at Ipk read off the record's L(I) curve, solved with the ripple",
          "[magnetic_loss]") {
    // 74437529203221 (Wuerth, 220 uH): L(I) at 20 degC, 0.98455 x L(0) at 2.2 A and
    // 0.97091 x at 2.82 A. At this point Ipk ~ 2.29 A, inside that segment (owner 2026-11-14,
    // decision 2: the ripple uses the tolerance's minimum L, 176 uH; was 220 uH, Ipk ~ 2.231 A).
    const MagneticRow m = row("74437529203221");  // a copy: the fixture shard is static
    REQUIRE(m.bias_curve_current.size() == 18);
    const auto b = magnetic_loss(m, su36(), 166.7e-6);
    // The curve is read from the shared curve storage (MagneticRow::l_curves, float samples;
    // owner 2026-11-14 wiring), so the expected ratio is formed from the stored samples.
    const double r22 = double(0.0002166f) / double(0.00022f),
                 r282 = double(0.0002136f) / double(0.00022f);
    const double ratio = r22 + (b.ipk - 2.2) / (2.82 - 2.2) * (r282 - r22);
    CHECK(b.ipk > 2.2);
    CHECK(b.ipk < 2.82);
    CHECK(b.inductance == Approx(176e-6 * ratio).epsilon(1e-9));
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
    const auto b = magnetic_loss(row("74437529203221"), op, 166.7e-6);
    CHECK(b.inductance == Approx(176e-6));  // the tolerance's minimum (decision 2; was 220 uH)
    CHECK(b.inductance_basis.find("not read and not extrapolated") != std::string::npos);
    CHECK(b.saturation.failed());
}

TEST_CASE("magnetic loss: an L(I) curve that repeats a current is not read, and does not throw",
          "[magnetic_loss]") {
    // 784771331 (Wuerth 330 uH): its 20 degC L(I) curve states the SAME reading twice (1.23 A,
    // 141.6 uH). The demo on the frozen catalogue threw "Curve: x not strictly increasing" out of
    // the whole selection before this was handled. Read from MagneticRow::l_curves (owner
    // 2026-11-14 wiring) the shared curve storage keeps one copy of an identical repeat, so the
    // curve IS read now (was: not read, Isat UNVERIFIED): it falls to 74 % of L(0) at 0.932 A
    // and Ipk lies beyond its end (1.36 A) -> saturation FAIL. A curve stating two DIFFERENT
    // inductances at one current is the "not read" case now (1229AS-H-2R0M, below).
    const auto b = magnetic_loss(row("784771331"), su36(), 166.7e-6);
    CHECK(b.inductance == Approx(330e-6));
    CHECK(b.inductance_basis.find("Ipk lies beyond the record's L(I) curve at 20 degC") !=
          std::string::npos);
    CHECK(b.saturation.failed());
    CHECK(b.saturation.basis.find("falls to 74.3 % of L(0) at 0.932 A") != std::string::npos);
}

TEST_CASE("magnetic loss: saturation and rated current checks", "[magnetic_loss]") {
    // 744115 (Wuerth 200 uH): Isat 0.9 A @10 % < Ipk ~2.25 A: a lower bound of the 20 %
    // current cannot fail it -> UNVERIFIED (decision 3; was FAIL).
    CHECK(magnetic_loss(row("744115"), su36(), 166.7e-6).saturation.unverified());
    // 760804110 (Wuerth 250 uH): Isat 5.6 A @20 % passes; rated 1.1 A < Irms ~2.0 A, but its
    // rise is not carried in this index -> UNVERIFIED (decision 4; was FAIL).
    const auto t = magnetic_loss(row("760804110"), su36(), 166.7e-6);
    CHECK(t.saturation.str() == std::string("pass"));
    CHECK(t.thermal.unverified());
    // 1130-181K-RC (Bourns 180 uH): no Isat, no L(I) curve -> saturation UNVERIFIED.
    const auto u = magnetic_loss(row("1130-181K-RC"), su36(), 166.7e-6);
    CHECK(u.saturation.unverified());
    CHECK(u.rank_unverified() == 2);  // saturation and thermal (was 1)
    // PE-51516NL (Pulse 188 uH): DCR stated as a nominal only -> the basis says so.
    const auto n = magnetic_loss(row("PE-51516NL"), su36(), 166.7e-6);
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
    // Owner 2026-11-14 (decisions 3, 4): 744115 states only a 10 % Isat (a lower bound:
    // UNVERIFIED, no longer rejected), and no rated current here carries its rise (760804110
    // UNVERIFIED, no longer rejected; was 1). 784771331 is rejected by its L(I) curve (read
    // from l_curves since the curves wiring; was 0 after decision 3, 2 before it).
    CHECK(rej.value("saturation_current_below_peak", 0) == 1);
    CHECK(rej.value("rated_current_below_rms", 0) == 0);
    // Thermal is UNVERIFIED for all. One UNVERIFIED check, by DCR loss: 74437529203221,
    // MSS1812T-224, 760804110 (saturation passes); then two, by DCR loss.
    // (was: 760800301, 74437529203221, MSS1812T-224, 1130-181K-RC, PE-51516NL)
    CHECK(mpns(r) == std::vector<std::string>{"74437529203221", "MSS1812T-224", "760804110",
                                              "1130-181K-RC", "760800301", "PE-51516NL",
                                              "744115"});
    const json& top = r.at("candidates").at(0);
    CHECK(top.at("loss").at("checks").at("saturation").at("status") == "pass");
    CHECK(top.at("loss").at("terms").at("coreLoss").at("watts").is_null());
    CHECK(top.at("loss").at("total").is_null());
    // Over-rating is a flag: 760800301's 43 A Isat and 48 A rated against ~2.3 A / ~2.0 A
    // (it was the top candidate; it ranks fifth now).
    const json& paper = r.at("candidates").at(4);
    REQUIRE(paper.at("mpn") == "760800301");
    CHECK(paper.at("overRating").at("axes") == json::array({"isat", "ratedCurrent"}));
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

// ---- Owner decisions 2026-11-14: conduction mode, the 20 % saturation rule, the 40 K thermal
// rule, over-size flag and volume. Every record is a REAL catalogue line
// (tests/fixtures/magnetics_op_modes.ndjson, copied verbatim from the frozen TAS data 524a699).
namespace {

const Shard<MagneticRow>& modes_shard() {
    static const Shard<MagneticRow> s =
        build_magnetic_shard(std::string(KELVIN_TEST_DIR) + "/fixtures/magnetics_op_modes.ndjson");
    return s;
}

const MagneticRow& mrow(const std::string& mpn) {
    for (const auto& r : modes_shard().rows)
        if (r.mpn == mpn) return r;
    throw std::runtime_error("fixture row missing: " + mpn);
}

// The record's own ratedCurrentPoints, read from its fixture line (the index does not carry
// them with their rise yet): x = rise, y = current, temperature = the ambient it states.
CurvePts rated_points_of(const std::string& mpn) {
    std::ifstream f(std::string(KELVIN_TEST_DIR) + "/fixtures/magnetics_op_modes.ndjson");
    std::string line;
    while (std::getline(f, line)) {
        const json r = json::parse(line);
        const json& mi = r.at("magnetic").at("manufacturerInfo");
        if (mi.at("reference") != mpn) continue;
        const json& pts = mi.at("datasheetInfo").at("electrical").at(0).at("ratedCurrentPoints");
        auto x = std::make_shared<std::vector<float>>();
        auto y = std::make_shared<std::vector<float>>();
        CurvePts c;
        for (const auto& p : pts) {
            x->push_back(p.at("temperatureRise").get<float>());
            y->push_back(p.at("current").get<float>());
            if (p.contains("temperature")) c.temperature = p.at("temperature").get<double>();
        }
        c.x = x;
        c.y = y;
        return c;
    }
    throw std::runtime_error("fixture record missing: " + mpn);
}

InductorOperatingPoint su_dcm() {  // Su et al. buck run discontinuous, Vin 24-36 V
    InductorOperatingPoint op;
    op.mode = ConductionMode::Dcm;
    op.vin = 36;
    op.vin_min = 24;
    op.vout = 12;
    op.iout = 2;
    op.fsw = 80e3;
    return op;
}

InductorOperatingPoint su_crm() {  // CrM window 100-150 uH
    InductorOperatingPoint op;
    op.mode = ConductionMode::Crm;
    op.vin = 36;
    op.vout = 12;
    op.iout = 2;
    op.window_min_l = 100e-6;
    op.window_max_l = 150e-6;
    return op;
}

bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

}  // namespace

TEST_CASE("magnetic modes: CCM ripple and Ipk use the tolerance's minimum L", "[magnetic_loss]") {
    // 74437529203221: 220 uH, 176-264 uH stated -> 176 uH x L/L(0) at Ipk.
    const auto b = magnetic_loss(mrow("74437529203221"), su36(), 166.7e-6);
    CHECK(b.inductance_gate.str() == std::string("pass"));
    // The curve is read from the shared curve storage (MagneticRow::l_curves, float samples;
    // owner 2026-11-14 wiring), so the expected ratio is formed from the stored samples.
    const double r22 = double(0.0002166f) / double(0.00022f),
                 r282 = double(0.0002136f) / double(0.00022f);
    const double ratio = r22 + (b.ipk - 2.2) / (2.82 - 2.2) * (r282 - r22);
    CHECK(b.inductance == Approx(176e-6 * ratio).epsilon(1e-9));
    CHECK(b.ipk == Approx(2 + 0.5 * 1e-4 / b.inductance).epsilon(1e-9));
    CHECK(has(b.inductance_basis, "the tolerance's minimum L 176 uH"));
    // 760800301 states no tolerance: the nominal, with a note.
    const auto n = magnetic_loss(mrow("760800301"), su36(), 166.7e-6);
    CHECK(n.inductance == Approx(180e-6));
    CHECK(has(n.inductance_basis, "the record states no tolerance"));
    REQUIRE(n.notes.size() >= 1);
    CHECK(has(n.notes.front(), "no inductance tolerance"));
    CHECK(n.rank_unverified() == 2);  // saturation (30 % point only) and thermal; not "inductance"
    // A stated ambient is noted in full (a fixed 96-byte format buffer cut it short).
    auto hot = su36();
    hot.ambient = 85;
    const auto a = magnetic_loss(mrow("760800301"), hot, 166.7e-6);
    CHECK(has(a.notes.back(), "read at the record's own condition, not rescaled to it"));
}

TEST_CASE("magnetic modes: CCM over-size above 2x the required L is a flag, not a gate",
          "[magnetic_loss]") {
    // 760895731: 475 uH = 2.85x 166.7 uH.
    const auto b = magnetic_loss(mrow("760895731"), su36(), 166.7e-6);
    CHECK(b.inductance_gate.str() == std::string("pass"));
    CHECK(b.over_sized);
    CHECK(has(b.over_sized_note, "2.85x"));
    // 760800301: 180 uH = 1.08x -> not flagged.
    CHECK_FALSE(magnetic_loss(mrow("760800301"), su36(), 166.7e-6).over_sized);
    MagneticConstraints c;
    c.target_inductance = 166.7e-6;
    c.loss_op = su36();
    const json r = select_magnetic_op_loss(modes_shard(), c, 30);
    bool seen = false;
    for (const auto& cd : r.at("candidates"))
        if (cd.at("mpn") == "760895731") {
            seen = true;
            const auto& axes = cd.at("overRating").at("axes");
            CHECK(std::find(axes.begin(), axes.end(), "inductance") != axes.end());
        }
    CHECK(seen);
}

TEST_CASE("magnetic modes: DCM guaranteed maximum against the boundary at min Vin",
          "[magnetic_loss]") {
    // L_crit = 12 * (1 - 12/24) / (2 * 80 kHz * 2 A) = 18.75 uH; the gate is 0.9 x L_crit =
    // 16.875 uH (owner 2026-11-14: a 10 % margin; was L_crit itself).
    const auto d = dcm_maximum_inductance(su_dcm());
    CHECK(d.l_crit == Approx(18.75e-6));
    CHECK(d.l_max == Approx(16.875e-6));  // was 18.75e-6
    CHECK(has(d.basis, "Vout*(1 - Vout/Vin_min)/(2*Iout*fs)"));  // was "(2*fs*Iout)"
    CHECK(has(d.basis, "0.9 x L_crit"));
    // 74457115: 15 uH, 12-18 uH -> max 18 uH > 16.875 uH: FAIL under the margin (was: pass
    // against 18.75 uH, with its DCM waveform checked here; the DCM pass path and waveform are
    // now checked on 74457112, "magnetic curves: DCM ..." below).
    const auto p = magnetic_loss(mrow("74457115"), su_dcm(), std::nullopt);
    CHECK(p.inductance_gate.failed());
    CHECK(has(p.inductance_gate.basis, "guaranteed maximum 18 uH"));
    // 74457118: 18 uH nominal already above 16.875 uH: FAIL regardless of its tolerance (was:
    // nominal inside 18.75 uH, guaranteed maximum 21.6 uH above it).
    const auto f = magnetic_loss(mrow("74457118"), su_dcm(), std::nullopt);
    CHECK(f.inductance_gate.failed());
    CHECK(has(f.inductance_gate.basis, "nominal 18 uH is above L_max = 0.9 x L_crit"));
    // 7447704150: 15 uH, no tolerance stated -> UNVERIFIED (ranks after), not rejected.
    const auto u = magnetic_loss(mrow("7447704150"), su_dcm(), std::nullopt);
    CHECK(u.inductance_gate.unverified());
    CHECK(u.unverified().front() == "inductance");
    // 7447714220: 22 uH nominal above the boundary: FAIL without a tolerance.
    CHECK(magnetic_loss(mrow("7447714220"), su_dcm(), std::nullopt).inductance_gate.failed());
    MagneticConstraints c;
    c.loss_op = su_dcm();
    const json r = select_magnetic_op_loss(modes_shard(), c, 30);
    CHECK(r.at("rejections").value("inductance_above_dcm_maximum", 0) >= 2);
    CHECK(r.at("lossOperatingPoint").at("dcmMaximumInductance").get<double>() ==
          Approx(16.875e-6));  // was 18.75e-6
    const auto ms = mpns(r);
    const auto pos = [&](const std::string& m) {
        return std::find(ms.begin(), ms.end(), m) - ms.begin();
    };
    CHECK(pos("74457118") == static_cast<long>(ms.size()));
    // 74457115 is rejected under the margin now (was: ranked before 7447704150, the unverified
    // one); the pass-before-unverified order is checked on 74457112 below.
    CHECK(pos("74457115") == static_cast<long>(ms.size()));
    CHECK(pos("7447704150") < static_cast<long>(ms.size()));
}

TEST_CASE("magnetic modes: CrM tolerance band inside the window", "[magnetic_loss]") {
    // 1140-121K-RC: 120 uH, 108-132 uH inside 100-150 uH: pass.
    const auto p = magnetic_loss(mrow("1140-121K-RC"), su_crm(), std::nullopt);
    CHECK(p.inductance_gate.str() == std::string("pass"));
    CHECK(p.ipk == Approx(4.0));  // 2 x Iout, whatever L
    CHECK(p.irms == Approx(4.0 / std::sqrt(3.0)));
    REQUIRE(p.frequency);
    CHECK(*p.frequency == Approx(24.0 / 3 / (4.0 * 108e-6)));  // at the minimum L
    // 74457212: 120 uH, 96-144 uH: the band leaves the window below: FAIL.
    const auto f = magnetic_loss(mrow("74457212"), su_crm(), std::nullopt);
    CHECK(f.inductance_gate.failed());
    // 744771212: 120 uH, no tolerance: UNVERIFIED.
    CHECK(magnetic_loss(mrow("744771212"), su_crm(), std::nullopt).inductance_gate.unverified());
    // 760800301: 180 uH outside the window: FAIL.
    CHECK(magnetic_loss(mrow("760800301"), su_crm(), std::nullopt).inductance_gate.failed());
    MagneticConstraints c;
    c.loss_op = su_crm();
    const json r = select_magnetic_op_loss(modes_shard(), c, 30);
    CHECK(r.at("rejections").value("inductance_outside_crm_window", 0) >= 2);
}

TEST_CASE("magnetic modes: saturation at a 20 % drop from the stated Isat points",
          "[magnetic_loss]") {
    // 7447720901 (900 uH, no L(I) curve): 0.52 A at 10 %, 0.65 A at 30 % -> 0.585 A at 20 %.
    auto op = su36();
    op.iout = 0.5;  // Ipk = 0.5 + 0.5 * 1e-4 / 900e-6 = 0.5556 A
    const auto p = magnetic_loss(mrow("7447720901"), op, 166.7e-6);
    CHECK(p.saturation.str() == std::string("pass"));
    CHECK(has(p.saturation.basis, "Isat at 20 % drop 0.585 A, interpolated"));
    op.iout = 0.55;  // Ipk 0.6056 A: above 0.585 A, below the 30 % point
    CHECK(magnetic_loss(mrow("7447720901"), op, 166.7e-6).saturation.failed());
    // MSD1048-224KED: only 0.9 A at 30 % (an upper bound): below Ipk ~2.2 A -> FAIL.
    const auto f = magnetic_loss(mrow("MSD1048-224KED"), su36(), 166.7e-6);
    CHECK(f.saturation.failed());
    CHECK(has(f.saturation.basis, "an upper bound of the 20 % current"));
    // 760800301: only 43 A at 30 %: an upper bound cannot pass -> UNVERIFIED.
    const auto u = magnetic_loss(mrow("760800301"), su36(), 166.7e-6);
    CHECK(u.saturation.unverified());
}

TEST_CASE("magnetic modes: an L(I) curve decides saturation directly", "[magnetic_loss]") {
    // 74437529203221 at Iout 4.6 A: Ipk ~4.95 A lies above the 4.7 A interpolated from its
    // stated points (4.1 A at 10 %, 5.3 A at 30 %), but its curve still reads L above 80 % of
    // L(0) there: the curve decides -> pass.
    auto op = su36();
    op.iout = 4.6;
    const auto b = magnetic_loss(mrow("74437529203221"), op, 166.7e-6);
    CHECK(b.ipk > 4.7);
    CHECK(b.saturation.str() == std::string("pass"));
    CHECK(has(b.saturation.basis, "the record's L(I) curve"));
    // 744771220 (220 uH): its curve falls below 80 % before the Su Ipk -> FAIL.
    const auto f = magnetic_loss(mrow("744771220"), su36(), 166.7e-6);
    CHECK(f.saturation.failed());
    CHECK(has(f.saturation.basis, "L(I) curve"));
}

TEST_CASE("magnetic modes: thermal at a 40 K rise from the rated-current points",
          "[magnetic_loss]") {
    const std::string none = "none";
    // XAL5030-122MEC: 7.9 A at 20 K, 10.4 A at 40 K, ambient not stated.
    const CurvePts xal = rated_points_of("XAL5030-122MEC");
    const auto p = thermal_check(xal, 10.0, std::nullopt, none);
    CHECK(p.str() == std::string("pass"));
    CHECK(has(p.basis, "10.4 A at a 40 K rise"));
    CHECK(has(p.basis, "ambient not stated: read as room temperature"));
    CHECK(thermal_check(xal, 10.5, std::nullopt, none).failed());
    // CW100505-10NJ: only 0.48 A at 15 K -> a lower bound: passes below it, never fails.
    const CurvePts cw = rated_points_of("CW100505-10NJ");
    CHECK(thermal_check(cw, 0.4, std::nullopt, none).str() == std::string("pass"));
    CHECK(thermal_check(cw, 0.5, std::nullopt, none).unverified());
    // 7443630070: only 54.7 A at 50 K -> UNVERIFIED.
    CHECK(thermal_check(rated_points_of("7443630070"), 1.0, std::nullopt, none).unverified());
    // CDMC60D28L150NP-R10MC: 28.5 A at 40 K, 20 degC ambient; an 85 degC operating point is
    // stated, not applied.
    const auto h = thermal_check(rated_points_of("CDMC60D28L150NP-R10MC"), 20.0, 85.0, none);
    CHECK(h.str() == std::string("pass"));
    CHECK(has(h.basis, "at 20 degC ambient, not at the operating ambient 85 degC"));
    // LPS4018-333MRC: 25 degC counts as room temperature beside a 20 degC operating point.
    const auto l = thermal_check(rated_points_of("LPS4018-333MRC"), 0.7, 20.0, none);
    CHECK(l.str() == std::string("pass"));
    CHECK_FALSE(has(l.basis, "not at the operating ambient"));
    // The index carries the rated currents per rise (MagneticRow::rated_points, owner
    // 2026-11-14 wiring; was: no rise carried, UNVERIFIED for every record): XAL5030-122MEC
    // (1.2 uH) at the Su point carries a ripple of ~100 A, far above its 10.4 A at 40 K.
    const auto t = magnetic_loss(mrow("XAL5030-122MEC"), su36(), 1e-9).thermal;
    CHECK(t.failed());  // was unverified
    CHECK(has(t.basis, "exceeds the rated current 10.4 A at a 40 K rise"));
}

TEST_CASE("magnetic modes: volume beside board area", "[magnetic_loss]") {
    MagneticConstraints c;
    c.target_inductance = 166.7e-6;
    c.loss_op = su36();
    const json r = select_magnetic_op_loss(modes_shard(), c, 30);
    for (const auto& cd : r.at("candidates")) {
        REQUIRE(cd.contains("volume"));
        const auto& v = cd.at("volume");
        if (!v.at("mm3").is_null()) {
            const auto& m = mrow(cd.at("mpn").get<std::string>());
            CHECK(v.at("mm3").get<double>() == Approx(m.length_m * m.width_m * m.height_m * 1e9));
        }
    }
}

TEST_CASE("magnetic modes: the API requires conductionMode and its own inputs",
          "[magnetic_loss]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(KELVIN_TEST_DIR) / "_magnetic_modes_api";
    fs::create_directories(dir);
    fs::copy_file(fs::path(KELVIN_TEST_DIR) / "fixtures/magnetics_op_modes.ndjson",
                  dir / "magnetics.ndjson", fs::copy_options::overwrite_existing);
    api::Engine eng(dir.string(), "", /*quiet=*/true);
    const json req = {{"magnetizingInductance", {{"nominal", 166.7e-6}}}};
    json op = {{"position", "buckInductor"}, {"conductionMode", "ccm"}, {"inputVoltage", 36},
               {"outputVoltage", 12}, {"outputCurrent", 2}, {"switchingFrequency", 80e3}};
    const json r = eng.select("magnetic", req, {{"lossOperatingPoint", op}, {"includeEnvelope", false}});
    CHECK(r.at("lossOperatingPoint").at("conductionMode") == "ccm");
    json bad = op;
    bad.erase("conductionMode");  // nothing defaults
    CHECK_THROWS_AS(eng.select("magnetic", req, {{"lossOperatingPoint", bad}}), InvalidOptions);
    bad["conductionMode"] = "bcm";
    CHECK_THROWS_AS(eng.select("magnetic", req, {{"lossOperatingPoint", bad}}), InvalidOptions);
    bad = op;
    bad["conductionMode"] = "dcm";  // no minimumInputVoltage
    CHECK_THROWS_AS(eng.select("magnetic", req, {{"lossOperatingPoint", bad}}), InvalidOptions);
    bad = op;
    bad["conductionMode"] = "crm";  // fs is not an input, and no window
    CHECK_THROWS_AS(eng.select("magnetic", req, {{"lossOperatingPoint", bad}}), InvalidOptions);
    json crm = {{"position", "buckInductor"}, {"conductionMode", "crm"}, {"inputVoltage", 36},
                {"outputVoltage", 12}, {"outputCurrent", 2}, {"minimumInductance", 100e-6},
                {"maximumInductance", 150e-6}};
    const json rc = eng.select("magnetic", req, {{"lossOperatingPoint", crm}, {"includeEnvelope", false}});
    CHECK(rc.at("lossOperatingPoint").at("conductionMode") == "crm");
    CHECK(rc.at("target").at("inductance").is_null());
    fs::remove_all(dir);
}

// ---- Owner decisions 2026-11-14, round 3: the L(I) curves and rated-current points of the
// index wired in (MagneticRow::l_curves, rated_points), the DCM margin, the bounds and the size
// rule. Every record is a REAL catalogue line (tests/fixtures/magnetics_op_curves.ndjson,
// copied verbatim from the frozen TAS data 524a699).
namespace {

const Shard<MagneticRow>& curves_shard() {
    static const Shard<MagneticRow> s = build_magnetic_shard(
        std::string(KELVIN_TEST_DIR) + "/fixtures/magnetics_op_curves.ndjson");
    return s;
}

const MagneticRow& crow(const std::string& mpn) {
    for (const auto& r : curves_shard().rows)
        if (r.mpn == mpn) return r;
    throw std::runtime_error("fixture row missing: " + mpn);
}

long pos_in(const json& r, const std::string& mpn) {
    const auto ms = mpns(r);
    return std::find(ms.begin(), ms.end(), mpn) - ms.begin();
}

}  // namespace

TEST_CASE("magnetic curves: thermal FAIL from the record's rated-current points",
          "[magnetic_loss][magnetic_curves_op]") {
    // MSS1583-224KED (Coilcraft 220 uH): 1.3 A at 20 K, 1.8 A at 40 K; Irms ~2.0 A at the Su
    // point -> FAIL. DRQ127-221-R (Eaton 221.5 uH): 1.29 A at 40 K -> FAIL. Both are counted
    // as rejections of the selector (the FAIL path is reachable from catalogue rows again).
    const auto t = magnetic_loss(crow("MSS1583-224KED"), su36(), 166.7e-6);
    CHECK(t.saturation.str() != std::string("fail"));
    CHECK(t.thermal.failed());
    CHECK(has(t.thermal.basis, "rated current 1.8 A at a 40 K rise"));
    CHECK(has(t.thermal.basis, "ambient not stated: read as room temperature"));
    CHECK(magnetic_loss(crow("DRQ127-221-R"), su36(), 166.7e-6).thermal.failed());
    const json r = select_magnetic_op_loss(curves_shard(), su_constraints(), 30);
    CHECK(r.at("rejections").value("rated_current_below_rms", 0) == 2);
    CHECK(pos_in(r, "MSS1583-224KED") == static_cast<long>(mpns(r).size()));
    CHECK(pos_in(r, "DRQ127-221-R") == static_cast<long>(mpns(r).size()));
}

TEST_CASE("magnetic curves: a rated current only above a 40 K rise is an upper bound",
          "[magnetic_loss][magnetic_curves_op]") {
    // 1110-221K (Bourns 220 uH): only 1.6 A at 50 K. The 40 K current is at most 1.6 A, below
    // the Su Irms ~2.0 A -> FAIL (owner 2026-11-14: an upper bound fails a part already below
    // Irms). Against 1.0 A it cannot pass -> UNVERIFIED.
    const auto t = magnetic_loss(crow("1110-221K"), su36(), 166.7e-6).thermal;
    CHECK(t.failed());
    CHECK(has(t.basis, "rated current 1.6 A at a 50 K rise"));
    CHECK(has(t.basis, "an upper bound of the 40 K current"));
    const auto u = thermal_check(crow("1110-221K").rated_points, 1.0, std::nullopt, "none");
    CHECK(u.unverified());
}

TEST_CASE("magnetic curves: DCM pass under the 10 % margin, waveform, requested L as a note",
          "[magnetic_loss][magnetic_curves_op]") {
    // 74457112 (Wuerth 12 uH, 9.6-14.4 uH): 14.4 uH <= 0.9 x 18.75 uH = 16.875 uH -> pass.
    const auto p = magnetic_loss(crow("74457112"), su_dcm(), 15e-6);
    CHECK(p.inductance_gate.str() == std::string("pass"));
    CHECK(has(p.inductance_gate.basis, "guaranteed maximum 14.4 uH"));
    CHECK(has(p.inductance_gate.basis, "0.9 x L_crit"));
    // Ipk at the minimum L 9.6 uH: D = sqrt(2 L fs Iout Vout / (Vin (Vin-Vout))).
    const double L = 9.6e-6, D = std::sqrt(2 * L * 80e3 * 2 * 12 / (36.0 * 24));
    CHECK(p.inductance == Approx(L));
    CHECK(p.duty == Approx(D));
    CHECK(p.ipk == Approx(24 * D / (L * 80e3)));
    const double d2 = p.ipk * L * 80e3 / 12;
    CHECK(p.irms == Approx(p.ipk * std::sqrt((D + d2) / 3)));
    // The requested 15 uH is no floor in DCM: its distance is a note.
    CHECK(std::any_of(p.notes.begin(), p.notes.end(), [](const std::string& n) {
        return has(n, "DCM: nominal 12 uH is -20 % from the requested 15 uH (not a gate");
    }));
    // 7447704150 (15 uH, no tolerance): UNVERIFIED, ranks after the passing 74457112.
    MagneticConstraints c;
    c.target_inductance = 15e-6;
    c.loss_op = su_dcm();
    const json r = select_magnetic_op_loss(curves_shard(), c, 30);
    CHECK(pos_in(r, "74457112") < pos_in(r, "7447704150"));
    CHECK(pos_in(r, "7447704150") < static_cast<long>(mpns(r).size()));
}

TEST_CASE("magnetic curves: the L(I) curve read is the one at the ambient, else room temperature",
          "[magnetic_loss][magnetic_curves_op]") {
    // LQM2HPZR56ME0 (Murata 0.56 uH) states curves at -55, -30, 20, 25, 85 and 125 degC.
    InductorOperatingPoint op;
    op.mode = ConductionMode::Ccm;
    op.vin = 5;
    op.vout = 1.8;
    op.iout = 0.5;
    op.fsw = 2e6;
    // No ambient: two room-temperature curves (20 and 25 degC) qualify; neither is chosen.
    const auto n = magnetic_loss(crow("LQM2HPZR56ME0"), op, 1e-9);
    CHECK(has(n.inductance_basis, "it states 2 L(I) curves that qualify (20 degC, 25 degC)"));
    // At 85 degC: the 85 degC curve.
    op.ambient = 85;
    const auto a = magnetic_loss(crow("LQM2HPZR56ME0"), op, 1e-9);
    CHECK(has(a.inductance_basis, "read off the record's L(I) curve at 85 degC (the operating ambient)"));
    // VLS5045EX-1R0N (TDK 1 uH): its only curve is at -20 degC -> not read; its Isat decides.
    op.ambient.reset();
    const auto v = magnetic_loss(crow("VLS5045EX-1R0N"), op, 1e-9);
    CHECK(has(v.inductance_basis, "its L(I) curves are stated only at -20 degC"));
    // 7443310470 (Wuerth 4.7 uH): its 20 degC curve starts above 0 A -> no L(0), not read.
    const auto z = magnetic_loss(crow("7443310470"), op, 1e-9);
    CHECK(has(z.inductance_basis, "not at 0 A, so L(0) is not stated"));
    // 1229AS-H-2R0M (Coilcraft 2 uH): its 20 degC curve states two inductances at one current.
    const auto d = magnetic_loss(crow("1229AS-H-2R0M"), op, 1e-9);
    CHECK(has(d.inductance_basis, "its L(I) curve at 20 degC states two inductances at one current"));
    // LQH2HPN3R3MDR (Murata 3.3 uH): its 20 degC curve states two inductances at one current
    // (not carried) while its 105 degC curve is carried: the 20 degC one is named, not the
    // 105 degC one offered as the only temperature.
    const auto q = magnetic_loss(crow("LQH2HPN3R3MDR"), op, 1e-9);
    CHECK(has(q.inductance_basis, "its L(I) curve at 20 degC states two inductances at one current"));
}

TEST_CASE("magnetic curves: Isat at the operating ambient, else room temperature",
          "[magnetic_loss][magnetic_curves_op]") {
    // FP1005R4-R12-R (Eaton 0.12 uH): 60 A at 20 % (25 degC), 48 A at 20 % (125 degC). At
    // Vin 5 V, Vout 1 V, fs 1 MHz, Iout 50 A: Ipk = 50 + 0.5 x 0.8 / (1 MHz x 0.12 uH) = 53.3 A.
    InductorOperatingPoint op;
    op.mode = ConductionMode::Ccm;
    op.vin = 5;
    op.vout = 1;
    op.iout = 50;
    op.fsw = 1e6;
    const auto room = magnetic_loss(crow("FP1005R4-R12-R"), op, 1e-9);
    CHECK(room.ipk == Approx(50 + 0.5 * 0.8 / (1e6 * 0.12e-6)));
    CHECK(room.saturation.str() == std::string("pass"));
    CHECK(has(room.saturation.basis, "Isat 60 A at 20 % inductance drop at room temperature"));
    op.ambient = 125;
    const auto hot = magnetic_loss(crow("FP1005R4-R12-R"), op, 1e-9);
    CHECK(hot.saturation.failed());
    CHECK(has(hot.saturation.basis, "Isat 48 A at 20 % inductance drop at 125 degC (the operating ambient)"));
    op.ambient = 85;  // no points at 85 degC: room temperature, stated as such
    const auto mid = magnetic_loss(crow("FP1005R4-R12-R"), op, 1e-9);
    CHECK(mid.saturation.str() == std::string("pass"));
    CHECK(has(mid.saturation.basis, "not at the operating ambient 85 degC"));
}

TEST_CASE("magnetic curves: a size that cannot be determined counts one more UNVERIFIED",
          "[magnetic_loss][magnetic_curves_op]") {
    // MBH6045C-471MB=P3 (Murata 470 uH) draws no length x width x height: with the same two
    // UNVERIFIED checks as 760800301 (saturation, thermal) and a far lower DCR loss, it ranks
    // AFTER 760800301 under the size rule (owner 2026-11-14).
    const json r = select_magnetic_op_loss(curves_shard(), su_constraints(), 30);
    const json* mbh = nullptr;
    for (const auto& cd : r.at("candidates"))
        if (cd.at("mpn") == "MBH6045C-471MB=P3") mbh = &cd;
    REQUIRE(mbh);
    CHECK(mbh->at("sortKey").at("sizeUnverified") == true);
    CHECK(mbh->at("sortKey").at("unverifiedItems") == 3);
    CHECK(mbh->at("boardArea").at("mm2").is_null());
    CHECK(pos_in(r, "760800301") < pos_in(r, "MBH6045C-471MB=P3"));
}
