// [select][mfr] Manufacturer policy for selection: diversity cap + allowlist.
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "MfrPolicy.hpp"

using namespace kelvin;

namespace {
struct R {
    std::string manufacturer;
};
std::vector<const R*> ptrs(const std::vector<R>& rows) {
    std::vector<const R*> v;
    for (const R& r : rows) v.push_back(&r);
    return v;
}
size_t count_mfr(const std::vector<const R*>& out, const std::string& m) {
    size_t n = 0;
    for (const R* r : out) n += (norm_mfr(r->manufacturer) == norm_mfr(m));
    return n;
}
}  // namespace

TEST_CASE("mfr policy: no-op returns the plain top-N in order", "[select][mfr]") {
    std::vector<R> rows = {{"A"}, {"A"}, {"B"}, {"C"}, {"D"}};
    MfrPolicy p;  // empty allowlist, max_frac = 1.0
    REQUIRE_FALSE(p.active());
    auto out = apply_mfr_policy(ptrs(rows), 3, p);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0]->manufacturer == "A");
    REQUIRE(out[1]->manufacturer == "A");
    REQUIRE(out[2]->manufacturer == "B");
}

TEST_CASE("mfr policy: allowlist keeps only named vendors, ranked order", "[select][mfr]") {
    std::vector<R> rows = {{"Eaton"}, {"Würth Elektronik"}, {"Murata"}, {"WURTH ELEKTRONIK"}, {"KEMET"}};
    MfrPolicy p;
    p.allowlist = {norm_mfr("würth")};  // substring, case-insensitive
    auto out = apply_mfr_policy(ptrs(rows), 10, p);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0]->manufacturer == "Würth Elektronik");
    REQUIRE(out[1]->manufacturer == "WURTH ELEKTRONIK");
}

TEST_CASE("mfr policy: diversity cap limits any vendor's share", "[select][mfr]") {
    // A dominates the ranking (4x); 8 other vendors follow. N=10, max_frac=0.2
    // -> cap = floor(0.2*10) = 2. Enough other vendors exist to fill without
    // relaxing, so A must be capped at 2.
    std::vector<R> rows = {{"A"}, {"A"}, {"A"}, {"A"}, {"B"}, {"C"}, {"D"},
                           {"E"}, {"F"}, {"G"}, {"H"}, {"I"}};
    MfrPolicy p;
    p.max_frac = 0.2;
    auto out = apply_mfr_policy(ptrs(rows), 10, p);
    REQUIRE(out.size() == 10);
    REQUIRE(count_mfr(out, "A") <= 2);
    REQUIRE(count_mfr(out, "A") >= 1);  // best vendor still represented
}

TEST_CASE("mfr policy: thin population fills to N rather than starve", "[select][mfr]") {
    // Only 3 vendors; cap alone would leave < N. The fill pass tops it up so the
    // caller still gets N candidates (diversity-preferring, never fewer).
    std::vector<R> rows;
    for (int i = 0; i < 8; ++i) rows.push_back({"A"});
    rows.push_back({"B"});
    rows.push_back({"C"});
    MfrPolicy p;
    p.max_frac = 0.2;  // cap = 2 over N=10
    auto out = apply_mfr_policy(ptrs(rows), 10, p);
    REQUIRE(out.size() == 10);  // all 10 returned (relaxed), not starved to ~6
}
