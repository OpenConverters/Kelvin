// Unit tests for the shared curve helper (src/CrossRefCurves.hpp): validation, reading inside
// the measured range only, overlap, and the worst-point verdict over the overlap.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <string>

#include "../src/CrossRefCurves.hpp"

using namespace kelvin::crossref;
using Catch::Matchers::WithinRel;

TEST_CASE("Curve rejects malformed input instead of repairing it", "[curves]") {
    CHECK_THROWS_AS(Curve({1, 2, 3}, {1, 2}), std::invalid_argument);       // lengths
    CHECK_THROWS_AS(Curve({1}, {1}), std::invalid_argument);                // one sample
    CHECK_THROWS_AS(Curve({1, 3, 2}, {1, 2, 3}), std::invalid_argument);    // unsorted
    CHECK_THROWS_AS(Curve({1, 2, 2}, {1, 2, 3}), std::invalid_argument);    // duplicate x
    CHECK_THROWS_AS(Curve({1, NAN}, {1, 2}), std::invalid_argument);        // non-finite
    CHECK_NOTHROW(Curve({1, 2}, {5, -5}));
}

TEST_CASE("curve_at reads inside the measured range only", "[curves]") {
    const Curve c({10, 100, 1000}, {1, 0.1, 0.01});
    // Exactly at samples.
    CHECK(*curve_at(c, 10, Interp::LogLog) == 1.0);
    CHECK(*curve_at(c, 1000, Interp::Linear) == 0.01);
    // Log-log: a power law is reproduced exactly between samples.
    CHECK_THAT(*curve_at(c, std::sqrt(10.0 * 100.0), Interp::LogLog), WithinRel(std::sqrt(0.1), 1e-12));
    // Linear: the straight line between the bracketing samples.
    CHECK_THAT(*curve_at(c, 55, Interp::Linear), WithinRel(0.55, 1e-12));
    // Never extrapolated.
    CHECK_FALSE(curve_at(c, 9.999, Interp::LogLog));
    CHECK_FALSE(curve_at(c, 1000.001, Interp::Linear));
    CHECK_FALSE(curve_at(c, NAN, Interp::Linear));
    // Log-log over a non-positive sample is not readable: throws.
    const Curve z({0, 10}, {1, 2});
    CHECK_THROWS_AS(curve_at(z, 5, Interp::LogLog), std::invalid_argument);
    CHECK(*curve_at(z, 5, Interp::Linear) == 1.5);
}

TEST_CASE("overlap is the range both curves measured", "[curves]") {
    const Curve a({1, 10}, {1, 1}), b({5, 50}, {1, 1}), c({20, 30}, {1, 1}), d({10, 20}, {1, 1});
    auto ab = overlap(a, b);
    REQUIRE(ab);
    CHECK(ab->first == 5);
    CHECK(ab->second == 10);
    CHECK_FALSE(overlap(a, c));
    auto ad = overlap(a, d);  // touching at one point
    REQUIRE(ad);
    CHECK(ad->first == 10);
    CHECK(ad->second == 10);
}

TEST_CASE("compare_curves: worst point decides, judged with the scalar spec", "[curves]") {
    const ParamSpec esr{"esr", Dir::Lower, 1.5, std::nullopt, false, true, nullptr};
    const Curve orig({1e3, 1e4, 1e5, 1e6}, {1.0, 0.5, 0.2, 0.2});
    SECTION("equal or lower everywhere passes") {
        const Curve sub({1e3, 1e6}, {0.9, 0.05});  // log-log line stays below orig
        auto v = compare_curves(orig, sub, esr, Interp::LogLog);
        CHECK(std::string(v.verdict) == PASS);
        CHECK(v.points == 4);
        REQUIRE(v.range);
        CHECK(v.range->first == 1e3);
    }
    SECTION("one point inside the band warns, its x and values are reported") {
        // 0.26 at 1e5 vs 0.2: x1.3, inside the 1.5x band -> WARN at depth 0.6.
        const Curve sub({1e3, 1e4, 1e5, 1e6}, {1.0, 0.5, 0.26, 0.2});
        auto v = compare_curves(orig, sub, esr, Interp::LogLog);
        CHECK(std::string(v.verdict) == WARN);
        CHECK(v.worst_x == 1e5);
        CHECK(v.orig_y == 0.2);
        CHECK(v.sub_y == 0.26);
        REQUIRE(v.warn_depth);
        CHECK_THAT(*v.warn_depth, WithinRel(0.6, 1e-9));
    }
    SECTION("a FAIL anywhere beats a deeper WARN elsewhere") {
        const Curve sub({1e3, 1e4, 1e5, 1e6}, {1.45, 0.8, 0.2, 0.2});  // 1.45x warn, 1.6x fail
        auto v = compare_curves(orig, sub, esr, Interp::LogLog);
        CHECK(std::string(v.verdict) == FAIL);
        CHECK(v.worst_x == 1e4);
    }
    SECTION("only the overlap is compared, at both curves' sample x") {
        // Sub measured 3e4..3e6 only; its own 1e3-side is absent. Points: 3e4 (sub), 1e5, 1e6
        // (orig), 3e6 is outside orig -> excluded.
        const Curve sub({3e4, 3e6}, {0.2, 0.2});
        auto v = compare_curves(orig, sub, esr, Interp::LogLog);
        CHECK(v.points == 3);
        REQUIRE(v.range);
        CHECK(v.range->first == 3e4);
        CHECK(v.range->second == 1e6);
        // At 3e4 orig is ~0.2* (log-log between 0.5 and 0.2): sub 0.2 is lower -> pass.
        CHECK(std::string(v.verdict) == PASS);
    }
    SECTION("no overlap is UNVERIFIED") {
        const Curve sub({1e7, 1e8}, {0.01, 0.01});
        auto v = compare_curves(orig, sub, esr, Interp::LogLog);
        CHECK(std::string(v.verdict) == UNVERIFIED);
        CHECK(v.points == 0);
        CHECK_FALSE(v.range);
        CHECK(std::isnan(v.worst_x));
    }
    SECTION("Higher-is-better spec: the smallest sub/orig decides") {
        const ParamSpec cap{"capacitance", Dir::Higher, 0.8, std::nullopt, false, false, nullptr};
        const Curve o({0, 5, 10}, {10, 8, 5});
        const Curve s({0, 5, 10}, {10, 7, 4.5});  // 0.875 (warn) and 0.9 (warn): 0.875 worst
        auto v = compare_curves(o, s, cap, Interp::Linear);
        CHECK(std::string(v.verdict) == WARN);
        CHECK(v.worst_x == 5);
    }
    SECTION("a non-numeric spec is a programming error") {
        const ParamSpec cls{"dielectric", Dir::ClassMatch, 0, std::nullopt, false, false, nullptr};
        CHECK_THROWS_AS(compare_curves(orig, orig, cls, Interp::LogLog), std::invalid_argument);
    }
}
