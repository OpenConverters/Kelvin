// [crossref][golden] Replay the Heaviside-generated crossref golden corpus
// (tests/golden/crossref_parity.json, produced by
// Heaviside/tests/evals/kelvin_parity/generate_golden.py from the live Python
// engine) and assert Kelvin's C++ reproduces Python's exact deterministic
// outputs. This is the parity gate for the Python->Kelvin crossref cutover: as
// each Python primitive is ported into Kelvin, its golden section flips green.
//
// COVERED today: score_primary_value, over_dimensioning_penalty.
// PORT TODO (add sections as the C++ lands): required_inductance,
// footprint_area_mm2, operating_point_rescue.
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <nlohmann/json.hpp>

#include "CrossRefRescue.hpp"
#include "CrossRefScore.hpp"

using nlohmann::json;
using Catch::Matchers::WithinRel;
using namespace kelvin::crossref;

namespace {
std::string golden_path() {
    return std::string(KELVIN_TEST_DIR) + "/golden/crossref_parity.json";
}
json load_golden() {
    std::ifstream f(golden_path());
    REQUIRE(f.good());
    json j;
    f >> j;
    return j;
}
std::optional<double> opt(const json& o, const char* k) {
    if (!o.contains(k) || o.at(k).is_null()) return std::nullopt;
    return o.at(k).get<double>();
}
}  // namespace

TEST_CASE("golden: score_primary_value matches Python", "[crossref][golden]") {
    const json g = load_golden();
    for (const auto& c : g.at("score_primary_value")) {
        const std::string cat = c.at("category").get<std::string>();
        bool has_spec = false;
        ScoreResult r = score_primary_value(cat, opt(c, "original"), opt(c, "substitute"), has_spec);
        const auto& exp = c.at("expect");
        INFO("category=" << cat << " orig=" << c.at("original") << " sub=" << c.at("substitute"));
        if (exp.is_null()) {
            // Python returned None (category has no primary-value spec).
            REQUIRE_FALSE(has_spec);
            continue;
        }
        REQUIRE(has_spec);
        REQUIRE(r.verdict == exp.at("verdict").get<std::string>());
        REQUIRE_THAT(r.penalty, WithinRel(exp.at("penalty").get<double>(), 1e-6));
    }
}

TEST_CASE("golden: over_dimensioning_penalty matches Python", "[crossref][golden]") {
    const json g = load_golden();
    for (const auto& c : g.at("over_dimensioning_penalty")) {
        double got = over_dimensioning_penalty(opt(c, "required"), opt(c, "actual"));
        INFO("required=" << c.at("required") << " actual=" << c.at("actual"));
        REQUIRE_THAT(got, WithinRel(c.at("expect").get<double>(), 1e-6));
    }
}

TEST_CASE("golden: required_inductance matches Python", "[crossref][golden]") {
    const json g = load_golden();
    for (const auto& c : g.at("required_inductance")) {
        auto got = required_inductance(c.at("topology").get<std::string>(), c.at("spec"));
        const auto& exp = c.at("expect");
        INFO("topology=" << c.at("topology"));
        if (exp.is_null()) {
            REQUIRE_FALSE(got.has_value());
        } else {
            REQUIRE(got.has_value());
            REQUIRE_THAT(*got, WithinRel(exp.get<double>(), 1e-6));
        }
    }
}

TEST_CASE("golden: footprint_area_mm2 matches Python", "[crossref][golden]") {
    const json g = load_golden();
    for (const auto& c : g.at("footprint_area_mm2")) {
        double got = footprint_area_mm2(c.at("summary"));
        const auto& exp = c.at("expect");
        INFO("summary=" << c.at("summary"));
        if (exp.is_string() && exp.get<std::string>() == "inf") {
            REQUIRE(std::isinf(got));
        } else {
            REQUIRE_THAT(got, WithinRel(exp.get<double>(), 1e-6));
        }
    }
}

TEST_CASE("golden: operating_point_magnetic_rescue matches Python", "[crossref][golden]") {
    const json g = load_golden();
    for (const auto& c : g.at("operating_point_rescue")) {
        const auto& st = c.at("stress");
        std::optional<double> i_rms;
        if (st.contains("i_rms") && !st.at("i_rms").is_null()) i_rms = st.at("i_rms").get<double>();
        auto r = operating_point_magnetic_rescue(st.at("l_required").get<double>(),
                                                 st.at("i_peak").get<double>(), i_rms, c.at("pool"));
        const auto& exp_mpn = c.at("expect").at("mpn");
        if (exp_mpn.is_null()) {
            REQUIRE_FALSE(r.has_value());
        } else {
            REQUIRE(r.has_value());
            REQUIRE(r->summary.at("mpn").get<std::string>() == exp_mpn.get<std::string>());
        }
    }
}
