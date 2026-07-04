// [parity] Replay the Heaviside-generated golden over the committed fixtures and assert Kelvin's
// selector reproduces the Python selector's decision exactly: candidates[0].mpn == chosen,
// rejection histograms equal key-by-key, margins equal, errors map 1:1. This is THE parity gate.
#include <cmath>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "Index.hpp"
#include "Requirements.hpp"
#include "Select.hpp"

using namespace kelvin;
using nlohmann::json;

namespace {

std::string fixtures_dir() { return std::string(KELVIN_TEST_DIR) + "/fixtures"; }
std::string golden_dir() { return std::string(KELVIN_TEST_DIR) + "/golden"; }

json load_json(const std::string& path) {
    std::ifstream f(path);
    REQUIRE(f.good());
    json j;
    f >> j;
    return j;
}

double opt_inf(const json& c, const char* key) {  // null -> +inf (qg_max)
    if (c.at(key).is_null()) return INFINITY;
    return c.at(key).get<double>();
}
std::optional<double> opt_num(const json& c, const char* key) {
    if (!c.contains(key) || c.at(key).is_null()) return std::nullopt;
    return c.at(key).get<double>();
}

// margins/deviation compare: both sides compute identical double expressions, so equality is
// expected; a 1e-9 relative slack absorbs only trivial reassociation.
bool dbl_eq(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    if (std::isinf(a) || std::isinf(b)) return a == b;
    double diff = std::fabs(a - b);
    return diff <= 1e-9 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

// Compare a candidate margins object to the golden margins (null == inf/absent).
void check_margins(const json& got, const json& want, const std::string& tag) {
    for (auto it = want.begin(); it != want.end(); ++it) {
        INFO(tag << " margin " << it.key());
        REQUIRE(got.contains(it.key()));
        const json& gv = got.at(it.key());
        const json& wv = it.value();
        if (wv.is_null()) {
            REQUIRE(gv.is_null());
        } else {
            REQUIRE_FALSE(gv.is_null());
            REQUIRE(dbl_eq(gv.get<double>(), wv.get<double>()));
        }
    }
}

void check_rejections(const json& got, const json& want, const std::string& tag) {
    INFO(tag << " rejection histogram");
    // Every golden key present with equal count; and no extra non-zero keys in `got`.
    for (auto it = want.begin(); it != want.end(); ++it) {
        INFO("key " << it.key());
        REQUIRE(got.contains(it.key()));
        REQUIRE(got.at(it.key()).get<uint64_t>() == it.value().get<uint64_t>());
    }
    for (auto it = got.begin(); it != got.end(); ++it) {
        INFO("extra key " << it.key());
        REQUIRE(want.contains(it.key()));
    }
}

MosfetConstraints parse_mosfet(const json& c) {
    MosfetConstraints m;
    m.vds_min = c.at("vds_min").get<double>();
    m.id_min = c.at("id_min").get<double>();
    m.rds_on_max = c.at("rds_on_max").get<double>();
    m.qg_max = opt_inf(c, "qg_max");
    m.technology_allowed.clear();
    for (const auto& t : c.at("technology_allowed")) m.technology_allowed.insert(t.get<std::string>());
    m.exclude_discontinued = c.at("exclude_discontinued").get<bool>();
    m.op_i_rms = opt_num(c, "op_i_rms");
    m.op_vds = opt_num(c, "op_vds");
    m.op_duty = opt_num(c, "op_duty");
    m.op_fsw = opt_num(c, "op_fsw");
    return m;
}

}  // namespace

TEST_CASE("parity: mosfet", "[parity][mosfet]") {
    auto golden = load_json(golden_dir() + "/kelvin_golden_mosfet.json");
    auto shard = build_mosfet_shard(fixtures_dir() + "/mosfets.ndjson");
    const auto& cases = golden.at("cases");
    for (size_t i = 0; i < cases.size(); ++i) {
        const json& cs = cases[i];
        INFO("mosfet case " << i);
        MosfetConstraints c = parse_mosfet(cs.at("constraints"));
        MosfetTiebreaker tb = mosfet_tiebreaker_from_string(cs.at("tiebreaker").get<std::string>());
        const json& expect = cs.at("expect");
        if (expect.contains("chosen")) {
            json r = select_mosfet(shard, c, tb);
            REQUIRE_FALSE(r.at("candidates").empty());
            REQUIRE(r.at("candidates")[0].at("mpn").get<std::string>() ==
                    expect.at("chosen").get<std::string>());
            REQUIRE(r.at("alternativesConsidered").get<uint64_t>() ==
                    expect.at("alternatives").get<uint64_t>());
            check_margins(r.at("candidates")[0].at("margins"), expect.at("margins"),
                          "mosfet " + std::to_string(i));
        } else if (expect.at("error") == "SelectionError") {
            try {
                select_mosfet(shard, c, tb);
                FAIL("expected NoCandidates");
            } catch (const NoCandidates& e) {
                check_rejections(e.rejections, expect.at("rejections"), "mosfet " + std::to_string(i));
                REQUIRE(e.total_rows_considered == expect.at("total").get<uint64_t>());
            }
        } else if (expect.at("error") == "ValueError") {
            REQUIRE_THROWS_AS(select_mosfet(shard, c, tb), InvalidOptions);
        }
    }
}

TEST_CASE("parity: diode", "[parity][diode]") {
    auto golden = load_json(golden_dir() + "/kelvin_golden_diode.json");
    auto shard = build_diode_shard(fixtures_dir() + "/diodes.ndjson");
    const auto& cases = golden.at("cases");
    for (size_t i = 0; i < cases.size(); ++i) {
        const json& cs = cases[i];
        INFO("diode case " << i);
        const json& c = cs.at("constraints");
        DiodeConstraints dc;
        dc.vrrm_min = c.at("vrrm_min").get<double>();
        dc.if_avg_min = c.at("if_avg_min").get<double>();
        dc.qrr_max = opt_num(c, "qrr_max");
        dc.exclude_discontinued = c.at("exclude_discontinued").get<bool>();
        DiodeTiebreaker tb = diode_tiebreaker_from_string(cs.at("tiebreaker").get<std::string>());
        const json& expect = cs.at("expect");
        if (expect.contains("chosen")) {
            json r = select_diode(shard, dc, tb);
            REQUIRE(r.at("candidates")[0].at("mpn").get<std::string>() ==
                    expect.at("chosen").get<std::string>());
            REQUIRE(r.at("alternativesConsidered").get<uint64_t>() ==
                    expect.at("alternatives").get<uint64_t>());
            check_margins(r.at("candidates")[0].at("margins"), expect.at("margins"),
                          "diode " + std::to_string(i));
        } else {
            try {
                select_diode(shard, dc, tb);
                FAIL("expected NoCandidates");
            } catch (const NoCandidates& e) {
                check_rejections(e.rejections, expect.at("rejections"), "diode " + std::to_string(i));
                REQUIRE(e.total_rows_considered == expect.at("total").get<uint64_t>());
            }
        }
    }
}

TEST_CASE("parity: capacitor", "[parity][capacitor]") {
    auto golden = load_json(golden_dir() + "/kelvin_golden_capacitor.json");
    auto shard = build_capacitor_shard(fixtures_dir() + "/capacitors.ndjson");
    const auto& cases = golden.at("cases");
    for (size_t i = 0; i < cases.size(); ++i) {
        const json& cs = cases[i];
        INFO("capacitor case " << i);
        const json& c = cs.at("constraints");
        CapacitorConstraints cc;
        cc.capacitance_min = c.at("capacitance_min").get<double>();
        cc.capacitance_max = c.at("capacitance_max").get<double>();
        cc.v_rated_min = c.at("v_rated_min").get<double>();
        cc.ripple_current_min = opt_num(c, "ripple_current_min");
        for (const auto& t : c.at("technology_allowed")) cc.technology_allowed.insert(t.get<std::string>());
        cc.exclude_discontinued = c.at("exclude_discontinued").get<bool>();
        CapacitorTiebreaker tb =
            capacitor_tiebreaker_from_string(cs.at("tiebreaker").get<std::string>());
        const json& expect = cs.at("expect");
        if (expect.contains("chosen")) {
            json r = select_capacitor(shard, cc, tb);
            REQUIRE(r.at("candidates")[0].at("mpn").get<std::string>() ==
                    expect.at("chosen").get<std::string>());
            REQUIRE(r.at("alternativesConsidered").get<uint64_t>() ==
                    expect.at("alternatives").get<uint64_t>());
            check_margins(r.at("candidates")[0].at("margins"), expect.at("margins"),
                          "capacitor " + std::to_string(i));
        } else {
            try {
                select_capacitor(shard, cc, tb);
                FAIL("expected NoCandidates");
            } catch (const NoCandidates& e) {
                check_rejections(e.rejections, expect.at("rejections"),
                                 "capacitor " + std::to_string(i));
                REQUIRE(e.total_rows_considered == expect.at("total").get<uint64_t>());
            }
        }
    }
}

TEST_CASE("parity: resistor", "[parity][resistor]") {
    auto golden = load_json(golden_dir() + "/kelvin_golden_resistor.json");
    auto shard = build_resistor_shard(fixtures_dir() + "/resistors.ndjson");
    const auto& cases = golden.at("cases");
    for (size_t i = 0; i < cases.size(); ++i) {
        const json& cs = cases[i];
        INFO("resistor case " << i);
        const json& c = cs.at("constraints");
        ResistorConstraints rc;
        rc.target_ohms = c.at("target_ohms").get<double>();
        rc.max_tolerance = c.at("max_tolerance").get<double>();
        rc.max_value_deviation = c.at("max_value_deviation").get<double>();
        const json& expect = cs.at("expect");
        if (expect.contains("chosen")) {
            json r = select_resistor(shard, rc);
            REQUIRE(r.at("candidates")[0].at("mpn").get<std::string>() ==
                    expect.at("chosen").get<std::string>());
            REQUIRE(r.at("alternativesConsidered").get<uint64_t>() ==
                    expect.at("alternatives").get<uint64_t>());
            REQUIRE(dbl_eq(r.at("candidates")[0].at("deviation").get<double>(),
                           expect.at("deviation").get<double>()));
        } else {
            try {
                select_resistor(shard, rc);
                FAIL("expected NoCandidates");
            } catch (const NoCandidates& e) {
                check_rejections(e.rejections, expect.at("rejections"),
                                 "resistor " + std::to_string(i));
                REQUIRE(e.total_rows_considered == expect.at("total").get<uint64_t>());
            }
        }
    }
}

TEST_CASE("parity: controller", "[parity][controller]") {
    auto golden = load_json(golden_dir() + "/kelvin_golden_controller.json");
    auto shard = build_controller_shard(fixtures_dir() + "/controllers.ndjson");
    const auto& cases = golden.at("cases");
    for (size_t i = 0; i < cases.size(); ++i) {
        const json& cs = cases[i];
        INFO("controller case " << i);
        const json& c = cs.at("constraints");
        ControllerConstraints cc;
        cc.topology = c.at("topology").get<std::string>();
        cc.vin_nom = c.at("vin_nom").get<double>();
        cc.fsw_khz = c.at("fsw_khz").get<double>();
        if (!c.at("integrated_fet").is_null()) cc.integrated_fet = c.at("integrated_fet").get<bool>();
        if (!c.at("category").is_null()) cc.category = c.at("category").get<std::string>();
        const json& expect = cs.at("expect");
        if (expect.contains("chosen")) {
            json r = select_controller(shard, cc);
            REQUIRE(r.at("candidates")[0].at("mpn").get<std::string>() ==
                    expect.at("chosen").get<std::string>());
            REQUIRE(r.at("alternativesConsidered").get<uint64_t>() ==
                    expect.at("alternatives").get<uint64_t>());
        } else {
            try {
                select_controller(shard, cc);
                FAIL("expected NoCandidates");
            } catch (const NoCandidates& e) {
                check_rejections(e.rejections, expect.at("rejections"),
                                 "controller " + std::to_string(i));
                REQUIRE(e.total_rows_considered == expect.at("total").get<uint64_t>());
            }
        }
    }
}
