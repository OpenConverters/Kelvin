// Parity tests for the cross-reference scoring engine (CrossRefScore.hpp),
// mirroring Heaviside's tests/unit/test_scoring_engine.py. The load-bearing
// behaviours: the 330nH-for-1.5µH rejection, diminishing-returns over-
// dimensioning, gate dominance, and closest-value-wins.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "../src/CrossRefScore.hpp"

using namespace kelvin::crossref;
using Catch::Matchers::WithinAbs;

namespace {
ScoreResult mag(double sub, double orig = 1.5e-6) {
    return score_range(orig, sub, 0.90, 1.10, 0.80, 1.25);
}
ScoreResult isat(double sub, double orig = 3.25) {
    return score_directional(orig, sub, Mode::HigherBetter, 0.9, 0.8);
}
}  // namespace

TEST_CASE("RANGE: exact value passes, zero penalty", "[crossref][score]") {
    auto r = mag(1.5e-6);
    REQUIRE(r.verdict == PASS);
    REQUIRE_THAT(r.penalty, WithinAbs(0.0, 1e-9));
}

TEST_CASE("RANGE: 330nH-for-1.5uH is a hard FAIL", "[crossref][score]") {
    auto r = mag(330e-9);
    REQUIRE(r.verdict == FAIL);
    REQUIRE(r.ratio.has_value());
    REQUIRE_THAT(*r.ratio, WithinAbs(0.22, 0.01));
}

TEST_CASE("RANGE: in-accept-off-nominal warns, just-outside fails", "[crossref][score]") {
    REQUIRE(mag(1.2e-6).verdict == WARN);   // 0.8x, at accept floor
    REQUIRE(mag(1.1e-6).verdict == FAIL);   // 0.733x < 0.8x
    REQUIRE(mag(1.6e-6).verdict == PASS);   // within tight
}

TEST_CASE("RANGE: closest value has lowest penalty (monotone)", "[crossref][score]") {
    double a = mag(1.5e-6).penalty, b = mag(1.6e-6).penalty, c = mag(1.75e-6).penalty;
    REQUIRE(a <= b);
    REQUIRE(b <= c);
}

TEST_CASE("HIGHER_BETTER: meets passes, slight deficit warns, large deficit fails",
          "[crossref][score]") {
    REQUIRE(isat(3.3).verdict == PASS);
    REQUIRE(isat(3.1).verdict == WARN);   // 0.95x
    REQUIRE(isat(2.0).verdict == FAIL);   // 0.62x < 0.8x gate
}

TEST_CASE("Over-dimensioning: diminishing returns and cap", "[crossref][score]") {
    double p1 = isat(3.5).penalty, p2 = isat(6.5).penalty, p4 = isat(13.0).penalty;
    REQUIRE(p1 < p2);
    REQUIRE(p2 < p4);
    REQUIRE((p4 - p2) < (p2 - p1));  // concave: each doubling adds less
    // saturation: 8x and 80x are identical
    REQUIRE_THAT(isat(26.0).penalty, WithinAbs(isat(260.0).penalty, 1e-9));
}

TEST_CASE("over_dimensioning_penalty helper", "[crossref][score]") {
    REQUIRE(over_dimensioning_penalty(3.25, 3.25) == 0.0);
    REQUIRE(over_dimensioning_penalty(3.25, 3.0) == 0.0);
    REQUIRE(over_dimensioning_penalty(std::nullopt, 5.0) == 0.0);
    double p12 = over_dimensioning_penalty(3.25, 3.9);
    double p4 = over_dimensioning_penalty(3.25, 13.0);
    REQUIRE(p12 > 0.0);
    REQUIRE(p12 < p4);
}

TEST_CASE("primary-value dispatch", "[crossref][score]") {
    bool has = false;
    REQUIRE(score_primary_value("magnetic", 1.5e-6, 330e-9, has).verdict == FAIL);
    REQUIRE(has);
    score_primary_value("mosfet", 1.0, 1.0, has);
    REQUIRE_FALSE(has);  // no primary-value spec for mosfet
    REQUIRE(score_primary_value("resistor", 47000.0, 10000.0, has).verdict == FAIL);
    REQUIRE(score_primary_value("chipBead", 120.0, 60.0, has).verdict == FAIL);  // impedance halved
}

TEST_CASE("missing value is unverified, never a silent pass", "[crossref][score]") {
    bool has = false;
    REQUIRE(score_range(1.5e-6, std::nullopt, 0.9, 1.1, 0.8, 1.25).verdict == UNVERIFIED);
    REQUIRE(score_primary_value("magnetic", std::nullopt, 1.5e-6, has).verdict == UNVERIFIED);
}
