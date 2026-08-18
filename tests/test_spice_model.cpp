// ABT #806: a switching device as a SPICE model card, with the tier it earned.
// What is pinned here is mostly the REFUSALS — a model that quietly appears
// when the data does not support one is the failure mode this whole tier
// system exists to prevent.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "SpiceModel.hpp"

#include <fstream>
#include <sstream>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinRel;

static nlohmann::json first_mosfet_with(const std::string& part) {
    std::ifstream in(std::string(KELVIN_TEST_DIR) + "/fixtures/mosfets.ndjson");
    REQUIRE(in.good());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded()) continue;
        if (!j.contains("semiconductor")) continue;
        const auto& m = j["semiconductor"]["mosfet"];
        if (part.empty() ||
            m["manufacturerInfo"].value("reference", "") == part)
            return m;
    }
    return nlohmann::json();
}

TEST_CASE("a catalogue part becomes a tier-2 model that states its tier",
          "[spice][model]") {
    const nlohmann::json m = first_mosfet_with("");
    REQUIRE(!m.is_null());
    const auto r = Kelvin::spice::mosfet_model(m, "Q_HS");
    REQUIRE(r.tier == Kelvin::spice::Tier::Datasheet);

    // the card is runnable ngspice, and it is a VDMOS
    CHECK_THAT(r.card, ContainsSubstring(".model Q_HS VDMOS(nchan"));
    // and it never lets the reader forget what it is
    CHECK_THAT(r.card, ContainsSubstring("TIER 2"));
    CHECK_THAT(r.card, ContainsSubstring("NOT a vendor model"));
    CHECK_THAT(r.card, ContainsSubstring("Model conventions, none of them measured"));
    CHECK(r.assumptions.size() >= 3);

    // every parameter it stands on is echoed, so a reader can check the fit
    const auto& e = m["manufacturerInfo"]["datasheetInfo"]["electrical"];
    CHECK_THAT(r.used["onResistance"].get<double>(),
               WithinRel(e["onResistance"].get<double>(), 1e-12));
    CHECK_THAT(r.used["outputCapacitance"].get<double>(),
               WithinRel(e["outputCapacitance"].get<double>(), 1e-12));

    // The fit must REPRODUCE the datasheet's on-resistance at the datasheet's
    // own gate drive: channel (1/(Kp*Vov), the triode small-signal value) plus
    // the series drain resistance. Fitting Kp from the test current in
    // saturation instead gave a 360 mOhm channel for a 50 mOhm part — caught
    // here, which is why this assertion is written as the round trip and not
    // as an inequality.
    const double vth = r.used["gateThresholdVoltage"].get<double>();
    const double vgs = r.used["onResistanceVgs"].get<double>();
    const double rds = r.used["onResistance"].get<double>();
    auto param = [&](const std::string& key) {
        const size_t at = r.card.find(" " + key + "=");
        REQUIRE(at != std::string::npos);
        return std::stod(r.card.substr(at + key.size() + 2));
    };
    const double kp = param("Kp"), rd = param("Rd");
    const double r_chan = 1.0 / (kp * (vgs - vth));
    CHECK_THAT(r_chan + rd, WithinRel(rds, 1e-3));
}

TEST_CASE("a record without the fields refuses, and names them",
          "[spice][model]") {
    nlohmann::json m = first_mosfet_with("");
    REQUIRE(!m.is_null());
    auto& e = m["manufacturerInfo"]["datasheetInfo"]["electrical"];
    e.erase("outputCapacitance");
    e.erase("reverseTransferCapacitance");
    const auto r = Kelvin::spice::mosfet_model(m);
    CHECK(r.tier == Kelvin::spice::Tier::None);
    CHECK(r.card.empty());
    CHECK_THAT(r.why, ContainsSubstring("outputCapacitance"));
    CHECK_THAT(r.why, ContainsSubstring("reverseTransferCapacitance"));
    // and it says why a stand-in is not offered instead
    CHECK_THAT(r.why, ContainsSubstring("class default"));
}

TEST_CASE("an unphysical record is refused rather than fitted", "[spice][model]") {
    nlohmann::json m = first_mosfet_with("");
    auto& e = m["manufacturerInfo"]["datasheetInfo"]["electrical"];
    e["reverseTransferCapacitance"] = e["inputCapacitance"].get<double>() * 2.0;
    const auto r = Kelvin::spice::mosfet_model(m);
    CHECK(r.tier == Kelvin::spice::Tier::None);
    CHECK_THAT(r.why, ContainsSubstring("not physical"));
}

TEST_CASE("a bare record with no datasheet block refuses", "[spice][model]") {
    nlohmann::json m = {{"manufacturerInfo", {{"name", "ACME"}, {"reference", "X1"}}}};
    const auto r = Kelvin::spice::mosfet_model(m);
    CHECK(r.tier == Kelvin::spice::Tier::None);
    CHECK_THAT(r.why, ContainsSubstring("datasheetInfo.electrical"));
}

TEST_CASE("tier 1 is declared unavailable, never silently downgraded",
          "[spice][model]") {
    const auto r = Kelvin::spice::vendor_model_unavailable("BSC0902NS");
    CHECK(r.tier == Kelvin::spice::Tier::Vendor);
    CHECK(r.card.empty());              // no card: the point of the tier
    CHECK_THAT(r.why, ContainsSubstring("vendor SPICE model"));
}

TEST_CASE("the whole fixture catalogue is either modelled or refused, never half",
          "[spice][model]") {
    std::ifstream in(std::string(KELVIN_TEST_DIR) + "/fixtures/mosfets.ndjson");
    REQUIRE(in.good());
    std::string line;
    int modelled = 0, refused = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.contains("semiconductor")) continue;
        const auto r = Kelvin::spice::mosfet_model(j["semiconductor"]["mosfet"]);
        if (r.tier == Kelvin::spice::Tier::Datasheet) {
            ++modelled;
            CHECK(!r.card.empty());
            CHECK_THAT(r.card, ContainsSubstring("VDMOS"));
            CHECK(r.why.empty());
        } else {
            ++refused;
            CHECK(r.card.empty());      // a refusal never carries a card
            CHECK(!r.why.empty());      // and always says why
        }
    }
    CHECK(modelled > 0);
    INFO("modelled " << modelled << ", refused " << refused);
    CHECK(modelled + refused > 100);    // the fixture is a real catalogue
}
