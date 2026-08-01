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

// ABT #496: a WIMA DC-LINK part whose record carries no ESR at all was reported
// "esr": "fail" with no note — a verdict asserting a comparison that had no
// substitute-side operand, and the sole stated reason for a major_review. An
// absent value is an unknown, not a regression: it must read as unverified (as
// the same known-vs-absent situation does on temp_max_C), still cost the part
// the drop-in and the ranking place, and say what has to be confirmed.
TEST_CASE("a substitute with no ESR reads unverified, not failed", "[crossref][rank][abt496]") {
    json original = {{"mpn", "C4AKJBW5350A3PJ"}, {"value_si", 3.5e-5}, {"voltage", 700.0},
                     {"esr", 0.0039194366}, {"esr_frequency", 100000.0},
                     {"technology", "film-polypropylene"}, {"mount", "THT"},
                     {"length_m", 0.042}, {"width_m", 0.033}, {"height_m", 0.048},
                     {"temp_min_C", -55.0}, {"temp_max_C", 135.0}};
    json blind = original;  // identical part, minus the ESR its record never states
    blind["mpn"] = "DCP4L053507ID2JSC9";
    blind["esr"] = nullptr;
    blind["esr_frequency"] = nullptr;
    json measured = blind;
    measured["mpn"] = "MEASURED";
    measured["esr"] = 0.0035;
    measured["esr_frequency"] = 100000.0;

    auto r = cross_reference("capacitor", original, json::array({blind, measured}), Options{});
    auto find = [&](const char* mpn) {
        for (const auto& c : r["candidates"])
            if (c["mpn"] == mpn) return c;
        FAIL("candidate " << mpn << " missing from the result");
        return json{};
    };
    const json no_esr = find("DCP4L053507ID2JSC9");
    std::string esr_verdict;
    bool any_fail = false;
    for (const auto& p : no_esr["params"]) {
        if (p["name"] == "esr") esr_verdict = p["verdict"];
        if (p["verdict"] == "fail") any_fail = true;
    }
    CHECK(esr_verdict == "unverified");
    CHECK_FALSE(any_fail);  // nothing was compared, so nothing failed
    // The consequence stays: not a drop-in, and below the part we could check.
    CHECK(no_esr["grade"] == "major_review");
    CHECK(no_esr["status"] == "partial");
    CHECK(no_esr["penalty"].get<double>() > find("MEASURED")["penalty"].get<double>());
    // And it says why, rather than leaving tool_notes null as it did.
    REQUIRE(no_esr.contains("notes"));
    const std::string note = no_esr["notes"][0];
    CHECK(note.find("ESR") != std::string::npos);
    CHECK(note.find("datasheet") != std::string::npos);
}

TEST_CASE("a hard gate the substitute has no data for is rejected as unverified, not as a "
          "shortfall", "[crossref][rank][abt496]") {
    // Isat is a hard gate for magnetics, so a candidate that states none is still
    // rejected — but the reason must not claim its current rating fell short.
    json original = mag("ORIG", 1.5e-6, 3.25);
    json blind = {{"mpn", "NO_ISAT"}, {"value_si", 1.5e-6}, {"rated_current", 5.0},
                  {"dcr", 0.02}};
    auto r = cross_reference("magnetic", original, json::array({blind}), Options{});
    const json& c = r["candidates"][0];
    REQUIRE(c["status"] == "no_substitute");
    CHECK(std::string(c["reason"]).find("falls far below") == std::string::npos);
    for (const auto& p : c["params"])
        if (p["name"] == "saturation_current") CHECK(p["verdict"] == "unverified");
    REQUIRE(c.contains("notes"));
    CHECK(std::string(c["notes"][0]).find("saturation current") != std::string::npos);
}

TEST_CASE("a shifted resistor value cannot tie its exact-value siblings, and says why",
          "[crossref][rank][abt497]") {
    // ABT #497: ERA2AEC2152X, 21.5 kOhm +/-0.25 %, 0402. The 21.3 kOhm YAGEO part
    // scored value=pass at exactly the penalty of the 21.5 kOhm siblings, with no
    // note about the resistance at all — the wrong-value part and the exact-value
    // ones were indistinguishable in the output.
    json original = {{"mpn", "ERA2AEC2152X"}, {"value_si", 21500.0}, {"power_rating", 0.063},
                     {"tolerance_pct", 0.25}, {"case_code", "0402"}};
    json cands = json::array({
        {{"mpn", "RT0402BRD0721K3L"}, {"value_si", 21300.0}, {"power_rating", 0.063},
         {"tolerance_pct", 0.1}, {"case_code", "0402"}},
        {{"mpn", "RT0402BRD0721K5L"}, {"value_si", 21500.0}, {"power_rating", 0.063},
         {"tolerance_pct", 0.1}, {"case_code", "0402"}},
    });
    auto r = cross_reference("resistor", original, cands, Options{});
    auto find = [&](const char* mpn) {
        for (const auto& c : r["candidates"])
            if (c["mpn"] == mpn) return c;
        FAIL("candidate " << mpn << " missing from the result");
        return json{};
    };
    const json shifted = find("RT0402BRD0721K3L"), exact = find("RT0402BRD0721K5L");

    std::string shifted_value, exact_value;
    for (const auto& p : shifted["params"])
        if (p["name"] == "value") shifted_value = p["verdict"];
    for (const auto& p : exact["params"])
        if (p["name"] == "value") exact_value = p["verdict"];
    CHECK(shifted_value == "warn");
    CHECK(exact_value == "pass");
    // Strictly worse than the exact-value sibling — it must not tie it.
    CHECK(shifted["penalty"].get<double>() > exact["penalty"].get<double>());
    CHECK_FALSE(exact.contains("notes"));

    REQUIRE(shifted.contains("notes"));
    const std::string note = shifted["notes"][0];
    INFO("note: " << note);
    CHECK(note.find("-0.93 %") != std::string::npos);
    CHECK(note.find("0.25 % part") != std::string::npos);
    CHECK(note.find("do not overlap") != std::string::npos);
}
