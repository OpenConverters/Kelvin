// Property / fuzz pass over REAL catalogue fixture rows, driven the same way the
// web UI drives the engine (browse row -> flat spec -> cross_reference).
//
// Every "are you happy?" probe this session found a defect the targeted unit
// tests passed straight through — the identity-key collision that disabled the
// MOSFET current check, a no-data bead ranking first, a connector rejected on a
// blank field. Those share a shape: they only surface on a real pool run end to
// end, not on a hand-built two-row case. This test asserts the invariants that
// must hold for ANY input, over hundreds of real rows per family, so that class
// of bug fails here instead of in production.
//
// Invariants (each is a real bug class):
//   * never throws on well-formed input (the identity guard is exercised
//     separately and deliberately)
//   * every penalty is finite and non-negative — no NaN leaking from a bad ratio
//   * an EXACT COPY of the original is always offered, is never rejected, reads
//     as "equivalent" (nothing is better or worse than itself), and never needs
//     a major review or redesign. It may be OUTRANKED by a genuinely better part
//     (a smaller footprint frees board space — the intended right-sizing
//     tie-breaker), so "ranks first" is NOT asserted; "is a clean match" is.
//   * the ranking is deterministic — same question, same answer
#include <cmath>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "../src/Browse.hpp"
#include "../src/CrossRef.hpp"
#include "../src/Index.hpp"

using namespace kelvin;
using namespace kelvin::crossref;
using nlohmann::json;

namespace {
std::string fixtures() { return std::string(KELVIN_TEST_DIR) + "/fixtures"; }

// Browse row -> flat spec, per family. This mirrors CrossRefView.vue's spec()
// builders: same field names the engine reads, `_key` carrying identity so the
// MOSFET-id-collision class of bug is exercised, not sidestepped.
json spec_for(const std::string& cat, const json& row) {
    auto num = [&](const char* k) -> json {
        auto it = row.find(k);
        if (it != row.end() && it->is_number() && it->get<double>() > 0) return *it;
        return json(nullptr);
    };
    auto s = [&](const char* k) -> std::string {
        auto it = row.find(k);
        return (it != row.end() && it->is_string()) ? it->get<std::string>() : "";
    };
    json o{{"_key", s("manufacturer") + "|" + s("mpn")}, {"mpn", s("mpn")}};
    // shared physical size + lifecycle, as base() does
    for (const char* k : {"lengthM", "widthM", "heightM"}) {
        auto it = row.find(k);
        if (it != row.end() && !it->is_null()) o[std::string(k) == "lengthM"  ? "length_m"
                                                : std::string(k) == "widthM" ? "width_m"
                                                                             : "height_m"] = *it;
    }
    if (!s("caseCode").empty()) o["case_code"] = s("caseCode");
    if (!s("mount").empty()) o["mount"] = s("mount");

    if (cat == "mosfet") {
        o["vds"] = num("vds_rated"); o["id"] = num("id_continuous");
        o["rds_on"] = num("rds_on"); o["qg"] = num("qg_total");
        o["vgs_threshold_max"] = num("vgs_threshold_max"); o["technology"] = s("technology");
    } else if (cat == "diode") {
        o["vrrm"] = num("vrrm_rated"); o["if_avg"] = num("if_avg_rated");
        o["vf"] = num("vf_typ"); o["technology"] = s("technology");
    } else if (cat == "capacitor") {
        o["value_si"] = num("capacitance"); o["voltage"] = num("v_rated");
        o["esr"] = num("esr"); o["technology"] = s("technology");
        o["dielectric_code"] = s("dielectric_code");
    } else if (cat == "resistor") {
        o["value_si"] = num("resistance"); o["power_rating"] = num("power_rating");
    } else if (cat == "magnetic") {
        o["value_si"] = num("inductance"); o["saturation_current"] = num("saturation_current");
        o["rated_current"] = num("rated_current"); o["dcr"] = num("dcr");
    } else if (cat == "connector") {
        o["family"] = s("family"); o["positions"] = num("positions");
        o["rated_current_A"] = num("rated_current"); o["rated_voltage_V"] = num("rated_voltage");
    } else if (cat == "analog") {
        o["subtype"] = s("device_type"); o["channels"] = num("channels");
        o["supply_max_V"] = num("vsupply_max"); o["gbw"] = num("gain_bandwidth");
        o["slew_rate"] = num("slew_rate");
    }
    return o;
}

template <class Row>
void fuzz_family(const std::string& cat, const Shard<Row>& shard) {
    json all = browse::browse_rows(shard, json{{"limit", 400}});
    const json& rows = all.at("rows");
    REQUIRE(rows.size() > 2);

    // a fixed, spread-out sample of originals (deterministic — no RNG in tests)
    for (size_t i = 0; i < rows.size(); i += std::max<size_t>(1, rows.size() / 40)) {
        json orig = spec_for(cat, rows[i]);

        // candidate pool: a slice of other rows, PLUS an exact copy of the original
        json pool = json::array();
        for (size_t j = 0; j < rows.size() && pool.size() < 30; ++j)
            if (j != i) pool.push_back(spec_for(cat, rows[j]));
        json self_copy = orig;
        self_copy["_key"] = "SELF_COPY";
        self_copy["mpn"] = orig.value("mpn", std::string());
        pool.push_back(self_copy);

        // max_results high enough that a perfect match is never truncated away.
        Options opt; opt.max_results = pool.size() + 1;
        json r1, r2;
        REQUIRE_NOTHROW(r1 = cross_reference(cat, orig, pool, opt));
        REQUIRE_NOTHROW(r2 = cross_reference(cat, orig, pool, opt));

        // determinism
        REQUIRE(r1.dump() == r2.dump());

        // every penalty finite and non-negative
        for (const auto& c : r1.at("candidates")) {
            double pen = c.value("penalty", -1.0);
            INFO(cat << " candidate " << c.value("mpn", std::string()) << " penalty " << pen);
            CHECK(std::isfinite(pen));
            CHECK(pen >= 0.0);
        }

        // an exact copy of the original is always a clean, equivalent match.
        const auto& cands = r1.at("candidates");
        auto self = std::find_if(cands.begin(), cands.end(), [](const json& c) {
            return c.value("_key", std::string()) == "SELF_COPY";
        });
        REQUIRE(self != cands.end());
        INFO(cat << " self-copy graded " << self->value("grade", std::string())
                 << " / " << self->value("direction", std::string()));
        CHECK(self->value("status", std::string()) != "no_substitute");
        CHECK(self->value("direction", std::string()) == "equivalent");
        CHECK(self->value("grade", std::string()) != "major_review");
        CHECK(self->value("grade", std::string()) != "redesign");
    }
}
}  // namespace

TEST_CASE("fuzz: mosfet ranker holds its invariants over real rows", "[crossref][fuzz]") {
    fuzz_family("mosfet", build_mosfet_shard(fixtures() + "/mosfets.ndjson"));
}
TEST_CASE("fuzz: diode ranker holds its invariants over real rows", "[crossref][fuzz]") {
    fuzz_family("diode", build_diode_shard(fixtures() + "/diodes.ndjson"));
}
TEST_CASE("fuzz: capacitor ranker holds its invariants over real rows", "[crossref][fuzz]") {
    fuzz_family("capacitor", build_capacitor_shard(fixtures() + "/capacitors.ndjson"));
}
TEST_CASE("fuzz: resistor ranker holds its invariants over real rows", "[crossref][fuzz]") {
    fuzz_family("resistor", build_resistor_shard(fixtures() + "/resistors.ndjson"));
}
TEST_CASE("fuzz: magnetic ranker holds its invariants over real rows", "[crossref][fuzz]") {
    fuzz_family("magnetic", build_magnetic_shard(fixtures() + "/magnetics.ndjson"));
}
TEST_CASE("fuzz: connector ranker holds its invariants over real rows", "[crossref][fuzz]") {
    fuzz_family("connector", build_connector_shard(fixtures() + "/connectors.ndjson"));
}
TEST_CASE("fuzz: analog ranker holds its invariants over real rows", "[crossref][fuzz]") {
    fuzz_family("analog", build_analog_shard(fixtures() + "/analog_ics.ndjson"));
}
