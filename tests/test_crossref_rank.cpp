// Tests for the cross-reference ranker (CrossRef.hpp) — the deterministic
// substitute selection KH consumes and HS runs its LLM over. Encodes the
// FAE-review findings as executable invariants.
#include <catch2/catch_test_macros.hpp>

#include "../src/CrossRef.hpp"

using namespace kelvin::crossref;

namespace {
json mag(const std::string& mpn, double L, double isat, double irms = 5.0, double dcr = 0.02) {
    return {{"mpn", mpn}, {"value_si", L}, {"saturation_current", isat},
            {"rated_current", irms}, {"dcr", dcr}};
}
}  // namespace

TEST_CASE("330nH is rejected for a 1.5uH original", "[crossref][rank]") {
    json original = mag("ORIG", 1.5e-6, 3.25);
    json cands = json::array({mag("W_330n", 330e-9, 12.4)});
    Options opt; opt.original_verified = true;
    auto r = cross_reference("magnetic", original, cands, opt);
    REQUIRE(r["candidates"][0]["status"] == "no_substitute");
}

TEST_CASE("severe current shortfall against a known original is rejected", "[crossref][rank]") {
    // original 25.5A Isat, candidate 2.1A (< 70%) -> no_substitute
    json original = mag("ORIG", 1.5e-6, 25.5, 21.0);
    json cands = json::array({mag("under", 1.5e-6, 2.1, 2.0)});
    Options opt; opt.original_verified = true;
    auto r = cross_reference("magnetic", original, cands, opt);
    REQUIRE(r["candidates"][0]["status"] == "no_substitute");
}

TEST_CASE("good in-kind match is recommended", "[crossref][rank]") {
    json original = mag("ORIG", 1.5e-6, 3.25, 3.25, 0.075);
    json cands = json::array({mag("good", 1.5e-6, 4.8, 8.6, 0.019)});
    Options opt; opt.original_verified = true;
    auto r = cross_reference("magnetic", original, cands, opt);
    REQUIRE(r["candidates"][0]["status"] == "recommended");
}

TEST_CASE("unverified original caps a good match at partial", "[crossref][rank]") {
    json original = mag("ORIG", 1.5e-6, 3.25);
    json cands = json::array({mag("good", 1.5e-6, 4.8, 8.6, 0.019)});
    Options opt; opt.original_verified = false;  // original not resolved
    auto r = cross_reference("magnetic", original, cands, opt);
    REQUIRE(r["candidates"][0]["status"] == "partial");
    REQUIRE(r["candidates"][0]["original_unverified"] == true);
}

TEST_CASE("ranking: right-sized outranks grossly-oversized, wrong-value sinks", "[crossref][rank]") {
    json original = mag("ORIG", 1.5e-6, 3.25);
    json cands = json::array({
        mag("oversize", 1.5e-6, 40.0),   // meets, hugely over-dimensioned
        mag("rightsize", 1.5e-6, 3.6),   // meets, right-sized
        mag("wrongval", 330e-9, 12.0),   // wrong value -> no_substitute
    });
    Options opt; opt.original_verified = true;
    auto r = cross_reference("magnetic", original, cands, opt);
    REQUIRE(r["candidates"][0]["mpn"] == "rightsize");
    REQUIRE(r["candidates"].back()["mpn"] == "wrongval");
    REQUIRE(r["candidates"].back()["status"] == "no_substitute");
}

TEST_CASE("capacitor max-temp downgrade demotes to partial", "[crossref][rank]") {
    json original = {{"mpn", "O"}, {"value_si", 1e-7}, {"voltage", 16.0}, {"temp_max_C", 125.0}};
    json cands = json::array({
        {{"mpn", "X5R"}, {"value_si", 1e-7}, {"voltage", 25.0}, {"temp_max_C", 85.0}}});
    Options opt; opt.original_verified = true;
    auto r = cross_reference("capacitor", original, cands, opt);
    REQUIRE(r["candidates"][0]["status"] == "partial");
}
