// [capacitor_loss] Operating-point ranking of buck capacitors (owner 2026-11-14): the ESR loss at
// fs, the effective capacitance at the DC voltage, the output ripple voltage and the ripple-current
// rating moved to fs / the ambient — each from the record's own values and curves, each with its
// basis, or UNVERIFIED with what is missing (never a guessed number).
//
// Real records from the frozen catalogue (TAS 524a699, tools/qarlos/bench/data-524a699), copied
// verbatim into tests/fixtures/curves_capacitors.ndjson (the curve fixtures) and
// tests/fixtures/capacitors_op_loss.ndjson (the ranking fixture).
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "CapacitorLoss.hpp"
#include "KelvinApi.hpp"
#include "Views.hpp"

using namespace kelvin;
using nlohmann::json;
using Catch::Approx;

namespace {

json fixture_line(const std::string& file, const std::string& mpn) {
    std::ifstream in(std::string(KELVIN_TEST_DIR) + "/fixtures/" + file);
    std::string line;
    while (std::getline(in, line)) {
        json j = json::parse(line);
        const json& mi = j["capacitor"]["manufacturerInfo"];
        if (mi.value("reference", "") == mpn ||
            mi["datasheetInfo"]["part"].value("partNumber", "") == mpn)
            return j;
    }
    FAIL("fixture " << mpn << " missing from " << file);
    return {};
}
CapacitorRow row(const std::string& mpn, const std::string& file = "curves_capacitors.ndjson") {
    auto r = extract_capacitor(fixture_line(file, mpn));
    REQUIRE(r.has_value());
    return *r;
}
bool has(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }
std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += x + " | ";
    return s;
}

// Su et al. buck: Vin 36 V, Vout 12 V, Iout 2 A, fs 80 kHz, L 180 uH -> dIpp = 24 * (1/3) /
// (180 uH * 80 kHz) = 0.5556 A.
constexpr double kDipp = 24.0 * (12.0 / 36.0) / (180e-6 * 80e3);
CapacitorOperatingPoint out_point() {
    CapacitorOperatingPoint op;
    op.position = CapacitorPosition::BuckOutput;
    op.vin = 36;
    op.vout = 12;
    op.iout = 2;
    op.fsw = 80e3;
    op.ripple_pp = kDipp;
    return op;
}
CapacitorOperatingPoint in_point() {
    CapacitorOperatingPoint op = out_point();
    op.position = CapacitorPosition::BuckInput;
    op.ripple_pp.reset();
    return op;
}

}  // namespace

TEST_CASE("capacitor loss: ripple current through the output and the input capacitor",
          "[capacitor_loss]") {
    const CapacitorRow x = row("380LQ102M200H452");
    auto o = capacitor_loss(x, out_point());
    CHECK(o.i_ripple == Approx(kDipp / std::sqrt(12.0)));
    CHECK(o.v_dc == 12);
    auto i = capacitor_loss(x, in_point());
    CHECK(i.i_ripple == Approx(2.0 * std::sqrt((1.0 / 3) * (2.0 / 3))));  // 0.9428 A
    CHECK(i.v_dc == 36);
    CHECK(i.has_ripple_voltage());  // the input model (owner 2026-11-14)
}

// TDK C1005X5R1V225K050BC (X5R 2.2 uF 35 V): ESR(f) and C(Vdc) curves -> both read off them.
TEST_CASE("capacitor loss: ESR and effective capacitance read off the record's curves",
          "[capacitor_loss]") {
    const CapacitorRow x = row("C1005X5R1V225K050BC");
    REQUIRE_FALSE(x.esr_curve.empty());
    REQUIRE_FALSE(x.bias_curve.empty());
    auto b = capacitor_loss(x, out_point());
    REQUIRE(b.esr_at_fs);
    REQUIRE(b.esr_loss.watts);
    CHECK(has(b.esr_loss.basis, "at fs 80000 Hz read off the record's ESR(f) curve"));
    CHECK(*b.esr_loss.watts == Approx(b.i_ripple * b.i_ripple * *b.esr_at_fs));
    REQUIRE(b.c_eff.value);
    CHECK(has(b.c_eff.basis, "at 12 V DC read off the record's DC-bias curve"));
    CHECK(*b.c_eff.value < 0.5 * x.capacitance);  // X5R at 12 V of 35 V: well below nominal
    REQUIRE(b.ripple_voltage.value);
    CHECK(*b.ripple_voltage.value ==
          Approx(kDipp * *b.esr_at_fs + kDipp / (8 * 80e3 * *b.c_eff.value)));
    // No ripple rating on an MLCC record: the check is UNVERIFIED, the loss is still complete.
    CHECK_FALSE(b.ripple_rating.value);
    CHECK(has(joined(b.ripple_rating.missing), "ripple-current rating not stated"));
    CHECK(b.unverified().empty());
}

// Murata ZRB157R6YA225ME11 (X5R): a DC-bias curve at 25 degC but no ESR at all -> the ESR loss
// and (with a limit) the ripple voltage are UNVERIFIED; Ceff still read off the curve.
TEST_CASE("capacitor loss: no ESR on record is UNVERIFIED, never estimated", "[capacitor_loss]") {
    const CapacitorRow x = row("ZRB157R6YA225ME11");
    CapacitorOperatingPoint op = out_point();
    op.max_ripple_v = 0.05;
    auto b = capacitor_loss(x, op);
    CHECK_FALSE(b.esr_loss.watts);
    CHECK(has(joined(b.esr_loss.missing), "ESR not stated in the record"));
    REQUIRE(b.c_eff.value);
    CHECK(has(b.c_eff.basis, "curve at 25 degC"));
    CHECK_FALSE(b.ripple_voltage.value);
    CHECK(b.unverified() == std::vector<std::string>{"esrLoss", "rippleVoltage"});
    CHECK_FALSE(b.total());
}

// A class-II MLCC whose record has no DC-bias curve: Ceff is UNVERIFIED, not derated by a factor.
// KEMET C1206C105K4RACTU (ceramic-class-2, 1 uF 16 V; the record states no dielectric code), the
// frozen catalogue line kept verbatim in capacitors_op_loss.ndjson.
TEST_CASE("capacitor loss: class-II ceramic without a DC-bias curve leaves Ceff UNVERIFIED",
          "[capacitor_loss]") {
    const CapacitorRow x = row("C1206C105K4RACTU", "capacitors_op_loss.ndjson");
    REQUIRE(x.bias_curve.empty());
    auto b = capacitor_loss(x, out_point());
    CHECK_FALSE(b.c_eff.value);
    CHECK(has(joined(b.c_eff.missing), "has no DC-bias curve"));
    CHECK(has(joined(b.c_eff.missing), "not derated by a guessed factor"));
    CHECK_FALSE(b.ripple_voltage.value);
}

// Vishay MAL209332102E3 (Al 1000 uF 200 V): scalar ESR 0.18 ohm stated at 120 Hz; ripple 2.35 A
// at 120 Hz with a frequency table that stops at 10 kHz -> no conversion to 80 kHz.
TEST_CASE("capacitor loss: scalar ESR names its frequency; a ripple table is not extrapolated",
          "[capacitor_loss]") {
    const CapacitorRow x = row("MAL209332102E3");
    auto b = capacitor_loss(x, out_point());
    REQUIRE(b.esr_loss.watts);
    CHECK(*b.esr_at_fs == Approx(0.18));
    CHECK(has(b.esr_loss.basis, "stated ESR 180 mohm at 120 Hz, not at fs 80000 Hz"));
    REQUIRE(b.c_eff.value);
    CHECK(*b.c_eff.value == Approx(1e-3));
    CHECK(has(b.c_eff.basis, "not a class-II ceramic"));
    CHECK_FALSE(b.ripple_rating.value);
    CHECK(has(joined(b.ripple_rating.missing), "(60 to 10000 Hz) does not cover both points"));
}

// Nichicon UPW1V102MHD: 1.95 A at 100 kHz / 105 degC, frequency table flat 1.0 from 10 kHz to
// 100 kHz, temperature table 60..105 degC (1.6 at 60) -> at 80 kHz and 60 degC: 1.95 * 1.6.
// Panasonic EEUEB1V102: 0.9 A at 85 degC, temperature table 40..85 -> a 25 degC ambient is
// outside it: UNVERIFIED, not extrapolated.
TEST_CASE("capacitor loss: ripple rating moved to fs and the ambient on the record's tables",
          "[capacitor_loss]") {
    CapacitorOperatingPoint op = in_point();
    op.ambient = 60;
    auto b = capacitor_loss(row("UPW1V102MHD"), op);
    REQUIRE(b.ripple_rating.value);
    CHECK(*b.ripple_rating.value == Approx(1.95 * 1.6));
    CHECK(has(b.ripple_rating.basis, "x1.6 on the record's ripple-current temperature table"));
    CHECK(*b.ripple_rating_ok());
    op.ambient = 25;
    auto e = capacitor_loss(row("EEUEB1V102"), op);
    CHECK_FALSE(e.ripple_rating.value);
    CHECK(has(joined(e.ripple_rating.missing), "(40 to 85 degC) does not cover both points"));
    // No ambient given: the rating stays at its stated temperature, said so.
    op.ambient.reset();
    auto n = capacitor_loss(row("EEUEB1V102"), op);
    REQUIRE(n.ripple_rating.value);
    CHECK(*n.ripple_rating.value == Approx(0.9));
    CHECK(has(n.ripple_rating.basis, "at its stated 85 degC (no ambient given)"));
}

// Cornell Dubilier 380LQ102M200H452: ESR curve 120 Hz..20 kHz (80 kHz outside it) -> the scalar
// 0.09 ohm at 20 kHz, named. dV = 0.5556 * 0.09 + 0.5556 / (8 * 80e3 * 1 mF) = 50.87 mV.
TEST_CASE("capacitor loss: output ripple voltage and its limit", "[capacitor_loss]") {
    const CapacitorRow x = row("380LQ102M200H452");
    CapacitorOperatingPoint op = out_point();
    op.max_ripple_v = 0.05;
    auto b = capacitor_loss(x, op);
    CHECK(has(b.esr_loss.basis, "fs 80000 Hz is outside the record's ESR(f) curve (120 to 20000 Hz)"));
    REQUIRE(b.ripple_voltage.value);
    CHECK(*b.ripple_voltage.value == Approx(kDipp * 0.09 + kDipp / (8 * 80e3 * 1e-3)));
    CHECK(b.over_ripple_voltage());
    CHECK(b.unverified().empty());
    op.max_ripple_v = 0.06;
    CHECK_FALSE(capacitor_loss(x, op).over_ripple_voltage());
}

TEST_CASE("capacitor loss: operating-point options are validated, nothing defaulted",
          "[capacitor_loss]") {
    CapacitorOperatingPoint op = out_point();
    op.ripple_pp.reset();
    CHECK_THROWS_AS(op.validate(), InvalidOptions);  // buckOutput needs the inductor ripple
    op = in_point();
    op.max_ripple_v = 0.1;
    CHECK_NOTHROW(op.validate());  // the input has a ripple-voltage model (owner 2026-11-14)
    op = out_point();
    op.vout = 40;
    CHECK_THROWS_AS(op.validate(), InvalidOptions);
    CHECK_THROWS_AS(capacitor_position_from_string("boostOutput"), InvalidOptions);
}

// Owner 2026-11-14: a record stating only tan(delta) gets ESR = tan(delta) / (2*pi*f_df*C) at
// tan(delta)'s own frequency, used as an UPPER BOUND at fs. Chemi-Con EKXJ201ELL151MJ50S (the
// paper's output cap, 150 uF 200 V) states tan(delta) 0.2 at 120 Hz and no ESR.
TEST_CASE("capacitor loss: ESR from the dissipation factor is an upper bound at fs",
          "[capacitor_loss]") {
    const CapacitorRow x = row("EKXJ201ELL151MJ50S", "capacitors_op_loss.ndjson");
    REQUIRE_FALSE(present(x.esr));
    REQUIRE(x.dissipation_factor == Approx(0.2));
    REQUIRE(x.dissipation_factor_frequency == Approx(120));
    auto b = capacitor_loss(x, out_point());
    const double esr = 0.2 / (2 * 3.14159265358979323846 * 120 * 150e-6);  // 1.768 ohm
    REQUIRE(b.esr_at_fs);
    CHECK(*b.esr_at_fs == Approx(esr));
    CHECK(b.esr_upper_bound);
    REQUIRE(b.esr_loss.watts);
    CHECK(*b.esr_loss.watts == Approx(b.i_ripple * b.i_ripple * esr));
    CHECK(has(b.esr_loss.basis, "at tan(delta)'s stated 120 Hz (the record states no ESR), an "
                                "upper bound at fs 80000 Hz since ESR falls with frequency"));
    REQUIRE(b.ripple_voltage.value);
    CHECK(has(b.ripple_voltage.basis, "an upper bound: the ESR is"));
    CHECK(b.unverified().empty());
    // fs below tan(delta)'s frequency: the derived ESR is no upper bound there -> UNVERIFIED.
    // KEMET C1206C105K4RACTU states tan(delta) 0.035 at 1 kHz; at a 500 Hz fs nothing is derived.
    CapacitorOperatingPoint low = out_point();
    low.fsw = 500;
    auto k = capacitor_loss(row("C1206C105K4RACTU", "capacitors_op_loss.ndjson"), low);
    CHECK_FALSE(k.esr_loss.watts);
    CHECK(has(joined(k.esr_loss.missing), "tan(delta) 0.035 at 1000 Hz, above fs 500 Hz"));
    // ... at 80 kHz it is: 0.035 / (2*pi * 1 kHz * 1 uF).
    auto k80 = capacitor_loss(row("C1206C105K4RACTU", "capacitors_op_loss.ndjson"), out_point());
    REQUIRE(k80.esr_at_fs);
    CHECK(*k80.esr_at_fs == Approx(0.035 / (2 * 3.14159265358979323846 * 1e3 * 1e-6)));
    // tan(delta) with no stated frequency: UNVERIFIED. KEMET C0603X5R1H105K states 0.03 only.
    auto n = capacitor_loss(row("C0603X5R1H105K", "capacitors_op_loss.ndjson"), out_point());
    CHECK_FALSE(n.esr_loss.watts);
    CHECK(has(joined(n.esr_loss.missing), "tan(delta) 0.03 without its frequency"));
}

// Owner 2026-11-14: a ripple rating stated at or above the ambient, with no temperature table,
// is used as a conservative lower bound, with a note. EKXJ201ELL151MJ50S: 0.72 A at 120 Hz and
// 105 degC, a frequency table (120 Hz..100 kHz) and no temperature table.
TEST_CASE("capacitor loss: a rating stated above the ambient with no table is a lower bound",
          "[capacitor_loss]") {
    const CapacitorRow x = row("EKXJ201ELL151MJ50S", "capacitors_op_loss.ndjson");
    REQUIRE(x.ripple_t_curve.empty());
    CapacitorOperatingPoint op = in_point();
    op.ambient = 40;
    auto b = capacitor_loss(x, op);
    REQUIRE(b.ripple_rating.value);
    // Moved to 80 kHz on its frequency table (2.05 at 10 kHz, 2.25 at 100 kHz, log-log).
    const double k = std::exp(std::log(2.05) + (std::log(2.25) - std::log(2.05)) *
                                                   (std::log(80e3) - std::log(1e4)) /
                                                   (std::log(1e5) - std::log(1e4)));
    CHECK(*b.ripple_rating.value == Approx(0.72 * k));
    CHECK(has(b.ripple_rating.basis, "taken as at least the rating stated at 105 degC (no "
                                     "temperature table: a conservative lower bound)"));
    CHECK(has(joined(b.notes), "used as a conservative lower bound"));
    // An ambient above the stated temperature is not covered by that bound: UNVERIFIED.
    op.ambient = 110;
    auto h = capacitor_loss(x, op);
    CHECK_FALSE(h.ripple_rating.value);
    CHECK(has(joined(h.ripple_rating.missing), "the record has no ripple-current temperature table"));
}

// Owner 2026-11-14 (coordinator's model): the input capacitor's ripple voltage is the standard
// buck input model dV_in = Iout*D*(1-D)/(fs*Ceff) + Iout*ESR. 380LQ102M200H452: 1 mF, 0.09 ohm.
TEST_CASE("capacitor loss: input ripple voltage by the standard buck input model",
          "[capacitor_loss]") {
    CapacitorOperatingPoint op = in_point();
    op.max_ripple_v = 0.18;
    auto b = capacitor_loss(row("380LQ102M200H452"), op);
    REQUIRE(b.ripple_voltage.value);
    const double d = 12.0 / 36;
    CHECK(*b.ripple_voltage.value == Approx(2 * d * (1 - d) / (80e3 * 1e-3) + 2 * 0.09));
    CHECK(has(b.ripple_voltage.basis, "dV_in = Iout*D*(1-D)/(fs*Ceff) + Iout*ESR"));
    CHECK(b.over_ripple_voltage());  // 5.56 mV (Ceff) + 180 mV (ESR) = 185.6 mV > 180 mV
    op.max_ripple_v = 0.19;
    CHECK_FALSE(capacitor_loss(row("380LQ102M200H452"), op).over_ripple_voltage());
}

// Owner 2026-11-14: a converted ripple-current rating below the ripple the part would carry is
// flagged, noted, and (in the selector) ranks after those that carry it. At the Su et al. input
// with fs 100 kHz (I_ripple 0.943 A): Taiyo Yuden RCHTQ560M4EFH0002X (56 uF 63 V hybrid) is
// rated 0.71 A at 100 kHz / 150 degC -> short; KEMET A767KN476M1HLAE029 (47 uF 50 V polymer)
// 3.3 A at 100 kHz -> carries it.
TEST_CASE("capacitor loss: a ripple-current rating below the ripple is a shortfall",
          "[capacitor_loss]") {
    CapacitorOperatingPoint op = in_point();
    op.fsw = 100e3;
    auto b = capacitor_loss(row("RCHTQ560M4EFH0002X", "capacitors_op_loss.ndjson"), op);
    REQUIRE(b.ripple_rating.value);
    CHECK(*b.ripple_rating.value == Approx(0.71));
    CHECK(b.i_ripple == Approx(2.0 * std::sqrt(2.0 / 9)));
    CHECK(b.under_ripple_rating());
    CHECK(b.fails_ripple_checks());
    CHECK(has(joined(b.notes), "is below the 0.9428 A ripple current at the operating point: "
                               "ranked after the candidates that carry it"));
    auto ok = capacitor_loss(row("A767KN476M1HLAE029", "capacitors_op_loss.ndjson"), op);
    REQUIRE(ok.ripple_rating.value);
    CHECK_FALSE(ok.under_ripple_rating());
    CHECK_FALSE(ok.fails_ripple_checks());
}

namespace {
std::vector<std::string> mpns_of(const json& r) {
    std::vector<std::string> v;
    for (const auto& c : r.at("candidates")) v.push_back(c.at("mpn").get<std::string>());
    return v;
}
const json& cand_of(const json& r, const std::string& mpn) {
    for (const auto& c : r.at("candidates"))
        if (c.at("mpn") == mpn) return c;
    throw std::runtime_error("no candidate " + mpn);
}
}  // namespace

// Owner 2026-11-14: under lowest_operating_point_loss the order is the UNVERIFIED count (loss
// terms + one for a board area or volume that cannot be determined), the ripple checks, then a
// rated voltage over 3x the need (capacitor size and cost scale with voltage), then the loss;
// lowest_esr keeps the over-rating tier. Over-rating is judged on the rated voltage only (the
// ripple-current axis was dropped, owner 2026-11-14). Real records from the frozen catalogue
// (data-524a699), verbatim in capacitors_op_loss.ndjson. The Su et al. buck output (150 uF,
// 14.4 V, ripple >= 1.25x):
//   EFDKS55K187F064DH / EFDKA45J167F044DH  Eaton film 180 / 160 uF, 550 / 450 V (Kelvin's old pick):
//                                          ESR with no frequency, no size stated
//   EKXJ201ELL151MJ50S                     Chemi-Con 150 uF 200 V (the paper's pick; tan(delta) only)
//   GWC1V151MCO1GS, HHXD160ARA181MF80G,    35 V / 16 V / 35 V polymer and electrolytic parts,
//   EXV227M035A9PAA                        not voltage over-rated
//   860040878017                           Wurth 220 uF 100 V electrolytic, 33.7 mohm with no
//                                          frequency (6.9x V)
TEST_CASE("capacitor over-rating: a tier under the operating-point loss and under lowest_esr",
          "[capacitor_overrating]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(KELVIN_TEST_DIR) / "_capacitor_op_loss_out";
    fs::create_directories(dir);
    fs::copy_file(fs::path(KELVIN_TEST_DIR) / "fixtures" / "capacitors_op_loss.ndjson",
                  dir / "capacitors.ndjson", fs::copy_options::overwrite_existing);
    api::Engine eng(dir.string(), "", /*quiet=*/true);
    const double i_out = kDipp / std::sqrt(12.0);
    const json req = {{"capacitance", 150e-6}, {"ratedVoltage", 14.4},
                      {"minimumRippleCurrent", 1.25 * i_out}};
    const json op = {{"position", "buckOutput"}, {"inputVoltage", 36}, {"outputVoltage", 12},
                     {"outputCurrent", 2}, {"switchingFrequency", 80e3},
                     {"inductorRippleCurrent", kDipp}};
    json r = eng.select("capacitor", req, {{"lossOperatingPoint", op}, {"includeEnvelope", false}});
    CHECK(r.at("tiebreaker") == "lowest_operating_point_loss");
    CHECK(mpns_of(r) == std::vector<std::string>{"GWC1V151MCO1GS", "HHXD160ARA181MF80G",
                                                 "EXV227M035A9PAA", "EKXJ201ELL151MJ50S",
                                                 "860040878017", "EFDKS55K187F064DH",
                                                 "EFDKA45J167F044DH"});
    // The key is non-decreasing: (UNVERIFIED items, ripple checks failed, over-rated voltage, loss).
    auto key = [](const json& c) {
        const json& k = c.at("sortKey");
        return std::make_tuple(k.at("unverifiedItems").get<size_t>(),
                               k.at("overRippleVoltage").get<bool>() ||
                                   k.at("underRippleCurrentRating").get<bool>(),
                               k.at("overRatedVoltage").get<bool>(), k.at("value").get<double>());
    };
    for (size_t i = 1; i < r.at("candidates").size(); ++i)
        CHECK_FALSE(key(r.at("candidates")[i]) < key(r.at("candidates")[i - 1]));
    const json& efdk = cand_of(r, "EFDKS55K187F064DH");
    CHECK(efdk.at("overRating").at("axes") == json::array({"voltage"}));
    CHECK(has(efdk.at("overRating").at("notes").at(0).get<std::string>(),
              "rated voltage 550 V is 38.2x the requirement's 14.4 V"));
    CHECK(has(efdk.at("overRating").at("notes").at(0).get<std::string>(),
              "ranked after closer-rated parts, ahead of the operating-point loss"));
    CHECK(efdk.at("boardArea").at("mm2").is_null());  // the record states no dimensions
    CHECK(efdk.at("volume").at("mm3").is_null());
    CHECK(efdk.at("sortKey").at("sizeUnverified") == true);
    CHECK(efdk.at("sortKey").at("unverifiedItems") == 2);  // ESR (no frequency) + size
    const json& ekxj = cand_of(r, "EKXJ201ELL151MJ50S");
    CHECK(ekxj.at("loss").at("terms").at("esrLoss").at("upperBound") == true);

    // lowest_esr keeps the over-rating tier, below the legacy thermal tier (as for MOSFETs): the
    // films state a thermal resistance and stay first; among the rest the 100 V part (voltage
    // axis) sinks below the 0.12 ohm EXV227M035A9PAA (none); EKXJ (no ESR) stays last.
    r = eng.select("capacitor", req, {{"tiebreaker", "lowest_esr"}, {"includeEnvelope", false}});
    CHECK(mpns_of(r) == std::vector<std::string>{"EFDKS55K187F064DH", "EFDKA45J167F044DH",
                                                 "GWC1V151MCO1GS", "HHXD160ARA181MF80G",
                                                 "EXV227M035A9PAA", "860040878017",
                                                 "EKXJ201ELL151MJ50S"});
    // Voltage only (owner 2026-11-14: the ripple-current axis was dropped): 860040878017's
    // stated 2.22 A rating, over 3x the 0.2 A need, is not an axis; GWC1V151MCO1GS has none.
    CHECK(cand_of(r, "860040878017").at("overRating").at("axes") == json::array({"voltage"}));
    CHECK(cand_of(r, "GWC1V151MCO1GS").at("overRating").at("axes").empty());
    CHECK(has(cand_of(r, "860040878017").at("overRating").at("notes").at(0).get<std::string>(),
              "ranked after closer-rated parts"));
    fs::remove_all(dir);
}

// Owner 2026-11-14: under the operating-point ranking the order is the UNVERIFIED count, then
// (over the ripple-voltage limit OR under the ripple-current rating), then the loss; such a part
// stays listed, with a note. The Su et al. input (30 uF, 43.2 V, Iout 2 A, 36 V -> 12 V) at
// fs 100 kHz, where the 100 kHz ratings these records state compare directly, with a 145 mV
// input ripple limit and no minimum ripple-current filter:
//   EFDKA60J356E164DH   Eaton film 35 uF 600 V, 6 mohm at no stated frequency: UNVERIFIED
//   HHXC630ARA560MJA0G  Chemi-Con hybrid 56 uF 63 V, 30 mohm    26.7 mW, 139 mV, 1.4 A rating
//   RCHTQ560M4EFH0002X  Taiyo Yuden hybrid 56 uF, 28 mohm       24.9 mW, but rated 0.71 A < 0.943 A
//   A767KN476M1HLAE029  KEMET polymer 47 uF 50 V, 29 mohm       25.8 mW, but 153 mV > 145 mV
//   C44AHFP5300ZC0J     KEMET film 30 uF 600 V (paper's pick)   no ESR on record: UNVERIFIED
TEST_CASE("capacitor op loss: ripple-voltage limit and ripple-current shortfall rank after",
          "[capacitor_overrating]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(KELVIN_TEST_DIR) / "_capacitor_op_loss_in";
    fs::create_directories(dir);
    fs::copy_file(fs::path(KELVIN_TEST_DIR) / "fixtures" / "capacitors_op_loss.ndjson",
                  dir / "capacitors.ndjson", fs::copy_options::overwrite_existing);
    api::Engine eng(dir.string(), "", /*quiet=*/true);
    const json req = {{"capacitance", 30e-6}, {"ratedVoltage", 43.2}};
    const json op = {{"position", "buckInput"}, {"inputVoltage", 36}, {"outputVoltage", 12},
                     {"outputCurrent", 2}, {"switchingFrequency", 100e3},
                     {"maxRippleVoltage", 0.145}};
    json r = eng.select("capacitor", req, {{"lossOperatingPoint", op}, {"includeEnvelope", false}});
    // EFDKA60J356E164DH's 6 mohm ESR states no frequency (UNVERIFIED, owner 2026-11-14) and it
    // states no size: last, after C44AHFP5300ZC0J.
    CHECK(mpns_of(r) == std::vector<std::string>{"HHXC630ARA560MJA0G", "RCHTQ560M4EFH0002X",
                                                 "A767KN476M1HLAE029", "C44AHFP5300ZC0J",
                                                 "EFDKA60J356E164DH"});
    const json& rch = cand_of(r, "RCHTQ560M4EFH0002X");
    CHECK(rch.at("sortKey").at("underRippleCurrentRating") == true);
    CHECK(rch.at("loss").at("checks").at("rippleCurrentRating").at("meets") == false);
    CHECK(has(rch.at("loss").at("notes").dump(), "ranked after the candidates that carry it"));
    const json& a767 = cand_of(r, "A767KN476M1HLAE029");
    CHECK(a767.at("sortKey").at("overRippleVoltage") == true);
    CHECK(a767.at("loss").at("checks").at("rippleVoltage").at("overLimit") == true);
    CHECK(has(a767.at("loss").at("checks").at("rippleVoltage").at("basis").get<std::string>(),
              "dV_in = Iout*D*(1-D)/(fs*Ceff) + Iout*ESR"));
    CHECK(cand_of(r, "C44AHFP5300ZC0J").at("loss").at("unverified") == json::array({"esrLoss", "rippleVoltage"}));
    // Both demoted parts have a lower loss than HHXC630ARA560MJA0G: only the ripple key moves them.
    CHECK(rch.at("loss").at("total").get<double>() <
          cand_of(r, "HHXC630ARA560MJA0G").at("loss").at("total").get<double>());
    CHECK(a767.at("loss").at("total").get<double>() <
          cand_of(r, "HHXC630ARA560MJA0G").at("loss").at("total").get<double>());
    fs::remove_all(dir);
}

// Owner 2026-11-14 (size and voltage, b): for capacitors a rated voltage over 3x the need is a
// ranking tier even under the operating-point loss — after the UNVERIFIED and ripple keys, before
// the loss. A 24 V -> 5 V buck output (150 uF, 6 V, Iout 2 A, fs 80 kHz, L 180 uH) over the real
// records of capacitors_op_loss.ndjson: GWC1V151MCO1GS (35 V, 5.8x) has the lower ESR loss, but
// HHXD160ARA181MF80G (16 V, 2.7x) ranks first; both are fully verified and pass the ripple checks.
TEST_CASE("capacitor op loss: a rated voltage over 3x the need ranks after, before the loss",
          "[capacitor_overrating][cap_voltage_tier]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(KELVIN_TEST_DIR) / "_capacitor_voltage_tier";
    fs::create_directories(dir);
    fs::copy_file(fs::path(KELVIN_TEST_DIR) / "fixtures" / "capacitors_op_loss.ndjson",
                  dir / "capacitors.ndjson", fs::copy_options::overwrite_existing);
    api::Engine eng(dir.string(), "", /*quiet=*/true);
    const double dipp = (24.0 - 5.0) * (5.0 / 24.0) / (180e-6 * 80e3);
    const json req = {{"capacitance", 150e-6}, {"ratedVoltage", 6}};
    const json op = {{"position", "buckOutput"}, {"inputVoltage", 24}, {"outputVoltage", 5},
                     {"outputCurrent", 2}, {"switchingFrequency", 80e3},
                     {"inductorRippleCurrent", dipp}};
    json r = eng.select("capacitor", req, {{"lossOperatingPoint", op}, {"includeEnvelope", false}});
    const auto order = mpns_of(r);
    REQUIRE(order.size() >= 2);
    CHECK(order[0] == "HHXD160ARA181MF80G");
    CHECK(order[1] == "GWC1V151MCO1GS");
    const json& hhxd = cand_of(r, "HHXD160ARA181MF80G");
    const json& gwc = cand_of(r, "GWC1V151MCO1GS");
    CHECK(gwc.at("loss").at("total").get<double>() < hhxd.at("loss").at("total").get<double>());
    CHECK(gwc.at("sortKey").at("unverifiedItems") == 0);
    CHECK(hhxd.at("sortKey").at("unverifiedItems") == 0);
    CHECK(gwc.at("sortKey").at("overRatedVoltage") == true);
    CHECK(hhxd.at("sortKey").at("overRatedVoltage") == false);
    CHECK(gwc.at("overRating").at("axes") == json::array({"voltage"}));
    CHECK(has(gwc.at("overRating").at("notes").at(0).get<std::string>(),
              "rated voltage 35 V is 5.83x the requirement's 6 V"));
    fs::remove_all(dir);
}

// Owner 2026-11-14: a stated ESR with no stated frequency leaves the ESR loss UNVERIFIED (the
// frequency is a condition of the value). Wurth 860040878017 (data-524a699) states ESR
// 33.7 mohm and no esrFrequency; GWC1V151MCO1GS states 18 mohm at 100 kHz and is read.
TEST_CASE("capacitor loss: an ESR with no stated frequency is UNVERIFIED", "[capacitor_loss]") {
    const CapacitorOperatingPoint op = out_point();
    auto b = capacitor_loss(row("860040878017", "capacitors_op_loss.ndjson"), op);
    CHECK_FALSE(b.esr_loss.watts);
    CHECK_FALSE(b.esr_at_fs);
    CHECK(b.unverified() == std::vector<std::string>{"esrLoss"});
    REQUIRE(b.esr_loss.missing.size() == 1);
    CHECK(has(b.esr_loss.missing[0], "the record states ESR 33.75 mohm without its frequency (a "
                                     "condition of the value), so it is not read at fs"));
    CHECK_FALSE(b.ripple_voltage.value);  // the ripple voltage needs the ESR too
    auto ok = capacitor_loss(row("GWC1V151MCO1GS", "capacitors_op_loss.ndjson"), op);
    REQUIRE(ok.esr_loss.watts);
    CHECK(has(ok.esr_loss.basis, "stated ESR 18 mohm at 100000 Hz"));
}

// Owner 2026-11-14: an ESR stated without its frequency is ignored; when the record states
// tan(delta) with its frequency, the ESR comes from tan(delta) as an upper bound, like a record
// stating no ESR, and the term is computed (labelled <=). Cornell Dubilier 730P106X9100
// (data-524a699, tests/fixtures/capacitors_esr_nofreq.ndjson): 10 uF film, ESR 12 mohm with no
// frequency, tan(delta) 0.001 at 1 kHz -> ESR <= 0.001 / (2*pi * 1 kHz * 10 uF) = 15.92 mohm.
// With neither (860040878017: ESR, no frequency, no tan(delta)) it stays UNVERIFIED (above).
TEST_CASE("capacitor loss: an ESR with no frequency is ignored; tan(delta) bounds it",
          "[capacitor_loss]") {
    const CapacitorRow x = row("730P106X9100", "capacitors_esr_nofreq.ndjson");
    REQUIRE(x.esr == Approx(0.012));
    REQUIRE(std::isnan(x.esr_frequency));
    auto b = capacitor_loss(x, out_point());
    const double esr = 0.001 / (2 * 3.14159265358979323846 * 1e3 * 10e-6);
    REQUIRE(b.esr_at_fs);
    CHECK(*b.esr_at_fs == Approx(esr));
    CHECK(b.esr_upper_bound);
    REQUIRE(b.esr_loss.watts);
    CHECK(*b.esr_loss.watts == Approx(b.i_ripple * b.i_ripple * esr));
    CHECK(has(b.esr_loss.basis, "ESR <= 15.92 mohm = tan(delta) 0.001 / (2*pi * 1000 Hz * 10 uF)"));
    CHECK(has(b.esr_loss.basis, "(the record's ESR 12 mohm states no frequency and is ignored), "
                                "an upper bound at fs 80000 Hz"));
    CHECK(b.unverified().empty());
}
