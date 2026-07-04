// [requirements] designRequirements -> constraints mapping (port of kirchhoff_fill._*_constraints):
// HV op-point attach, resonant-cap ±15% band vs 2x oversize, control-resistor deviation window.
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "Requirements.hpp"

using namespace kelvin;
using nlohmann::json;

TEST_CASE("requirements: mosfet HV stage attaches a total-loss operating point", "[requirements]") {
    json req = {{"ratedDrainSourceVoltage", 650.0},
                {"ratedContinuousDrainCurrent", 10.0},
                {"maximumOnResistance", 0.1}};
    // op_fsw present + Vds > 100 -> op point attached (proxies: op_i_rms=id, op_vds=vds, duty=0.5).
    auto c = mosfet_constraints(req, 100000.0);
    REQUIRE(c.op_fsw.has_value());
    REQUIRE(*c.op_i_rms == 10.0);
    REQUIRE(*c.op_vds == 650.0);
    REQUIRE(*c.op_duty == 0.5);
    REQUIRE(std::isinf(c.qg_max));
}

TEST_CASE("requirements: low-voltage mosfet gets no op point", "[requirements]") {
    json req = {{"ratedDrainSourceVoltage", 40.0},
                {"ratedContinuousDrainCurrent", 10.0},
                {"maximumOnResistance", 0.01}};
    auto c = mosfet_constraints(req, 100000.0);  // Vds < 100 -> no op point despite fsw
    REQUIRE_FALSE(c.op_fsw.has_value());
}

TEST_CASE("requirements: resonant cap gets a tight +-15% band; others 2x oversize", "[requirements]") {
    json resonant = {{"capacitance", {{"nominal", 6.8e-9}}}, {"ratedVoltage", 630.0},
                     {"role", "resonant"}};
    auto rc = capacitor_constraints(resonant);
    REQUIRE(rc.capacitance_min == Catch::Approx(6.8e-9 * 0.85));
    REQUIRE(rc.capacitance_max == Catch::Approx(6.8e-9 * 1.15));

    json filter = {{"capacitance", {{"nominal", 100e-6}}}, {"ratedVoltage", 63.0},
                   {"role", "outputFilter"}};
    auto fc = capacitor_constraints(filter);
    REQUIRE(fc.capacitance_min == Catch::Approx(100e-6));
    REQUIRE(fc.capacitance_max == Catch::Approx(200e-6));
}

TEST_CASE("requirements: resistor maps nominal + tolerance, 20% deviation window", "[requirements]") {
    json req = {{"resistance", {{"nominal", 10000.0}}}, {"tolerance", 0.01}};
    auto c = resistor_constraints(req);
    REQUIRE(c.target_ohms == 10000.0);
    REQUIRE(c.max_tolerance == 0.01);
    REQUIRE(c.max_value_deviation == 0.2);
}

TEST_CASE("requirements: missing required field throws (no silent fallback)", "[requirements]") {
    json req = {{"ratedDrainSourceVoltage", 100.0}};  // missing id + rds
    REQUIRE_THROWS_AS(mosfet_constraints(req, std::nullopt), InvalidOptions);
}
