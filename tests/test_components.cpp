// [components] TAS-document walk: select_components picks per seed and defers the right ones;
// bind_part stamps a chosen envelope so the component reads as bound.
#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "Constraints.hpp"
#include "KelvinApi.hpp"

using namespace kelvin;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {
std::string fixtures_dir() { return std::string(KELVIN_TEST_DIR) + "/fixtures"; }

// A minimal TAS with: a real mosfet seed, a body-diode (deferred), a numerical aid (deferred),
// a magnetic (deferred), and a resistor seed.
json make_tas() {
    return json::parse(R"({
      "inputs": {"designRequirements": {"inputVoltage": 100.0, "switchingFrequency": 100000.0}},
      "topology": {"stages": [{"name": "power", "circuit": {"components": [
        {"name": "Q1", "data": {"semiconductor": {"mosfet": {}},
          "inputs": {"designRequirements": {"ratedDrainSourceVoltage": 60.0,
            "ratedContinuousDrainCurrent": 5.0, "maximumOnResistance": 0.1}}}},
        {"name": "D1", "data": {"semiconductor": {"diode": {}},
          "inputs": {"designRequirements": {"role": "bodyDiode"}}}},
        {"name": "Csn1", "data": {"capacitor": {},
          "inputs": {"designRequirements": {"capacitance": {"nominal": 2.2e-9}, "ratedVoltage": 100.0}}}},
        {"name": "T1", "data": {"magnetic": {}, "inputs": {"designRequirements": {}}}},
        {"name": "Rb", "data": {"resistor": {},
          "inputs": {"designRequirements": {"resistance": {"nominal": 10000.0}, "tolerance": 0.05}}}}
      ]}}]}
    })");
}
}  // namespace

TEST_CASE("components: select_components fills seeds and defers the rest", "[components]") {
    api::Engine eng(fixtures_dir(), "", /*quiet=*/true);
    json tas = make_tas();
    json res = api::select_components(eng, tas, json::object());
    const json& comps = res.at("components");

    auto by_ref = [&](const std::string& ref) -> json {
        for (const auto& c : comps)
            if (c.at("ref") == ref) return c;
        FAIL("no component " + ref);
        return {};
    };

    // Q1: a real mosfet is sourced.
    json q1 = by_ref("Q1");
    REQUIRE(q1.at("filled").get<bool>() == true);
    REQUIRE(q1.contains("mpn"));
    // D1: body diode -> deferred.
    REQUIRE(by_ref("D1").at("filled").get<bool>() == false);
    REQUIRE(by_ref("D1").at("deferred").get<std::string>().find("body diode") != std::string::npos);
    // Csn1: numerical aid -> deferred (never sourced).
    REQUIRE(by_ref("Csn1").at("filled").get<bool>() == false);
    REQUIRE(by_ref("Csn1").at("deferred").get<std::string>().find("numerical") != std::string::npos);
    // T1: magnetic -> deferred to MKF.
    REQUIRE(by_ref("T1").at("deferred").get<std::string>().find("MKF") != std::string::npos);
    // Rb: resistor is sourced.
    REQUIRE(by_ref("Rb").at("filled").get<bool>() == true);
}

TEST_CASE("components: bind_part stamps the envelope and it reads as bound", "[components]") {
    api::Engine eng(fixtures_dir(), "", /*quiet=*/true);
    json tas = make_tas();
    json res = api::select_components(eng, tas, json::object());
    json q1;
    for (const auto& c : res.at("components"))
        if (c.at("ref") == "Q1") q1 = c;
    json envelope = q1.at("selection").at("candidates")[0].at("envelope");

    json bound = api::bind_part(tas, "Q1", envelope);
    // The Q1 semiconductor slot now carries a real manufacturerInfo.
    const json& q1data =
        bound.at("topology").at("stages")[0].at("circuit").at("components")[0].at("data");
    REQUIRE(q1data.at("semiconductor").at("mosfet").contains("manufacturerInfo"));

    // A second select_components now treats Q1 as already bound.
    json res2 = api::select_components(eng, bound, json::object());
    for (const auto& c : res2.at("components"))
        if (c.at("ref") == "Q1") {
            REQUIRE(c.at("filled").get<bool>() == false);
            REQUIRE(c.at("deferred").get<std::string>() == "already bound");
        }

    // bind_part on an unknown ref throws.
    REQUIRE_THROWS_AS(api::bind_part(tas, "NOPE", envelope), InvalidOptions);
}
