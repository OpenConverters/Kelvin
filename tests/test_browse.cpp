// [browse] The catalogue-browsing surface (Browse.hpp): filter/sort/facet/paginate over shard
// rows. Covers the faceted-search "count over all other filters" rule, NaN-last sorting,
// pagination, the unknown-field guard, and the Engine-level unloaded-shard guard.
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "Browse.hpp"
#include "Index.hpp"
#include "KelvinApi.hpp"

using namespace kelvin;
using nlohmann::json;

namespace {
std::string fixtures_dir() { return std::string(KELVIN_TEST_DIR) + "/fixtures"; }
}  // namespace

TEST_CASE("browse: unfiltered page over the mosfet fixture", "[browse]") {
    auto shard = build_mosfet_shard(fixtures_dir() + "/mosfets.ndjson");
    json r = browse::browse_rows(shard, json{{"limit", 10}});
    REQUIRE(r.at("family") == "mosfet");
    REQUIRE(r.at("total").get<uint64_t>() == shard.meta.row_count);
    REQUIRE(r.at("rows").size() == 10);
    // Default order is lineno ascending (file order).
    REQUIRE(r.at("rows")[0].at("lineno").get<uint32_t>() <
            r.at("rows")[9].at("lineno").get<uint32_t>());
    // Every row carries identity + locator + the family fields.
    const json& row = r.at("rows")[0];
    REQUIRE(row.contains("mpn"));
    REQUIRE(row.contains("manufacturer"));
    REQUIRE(row.contains("srcOffset"));
    REQUIRE(row.contains("srcLength"));
    REQUIRE(row.contains("vds_rated"));
    REQUIRE(row.contains("technology"));
}

TEST_CASE("browse: numeric range filter excludes absent values", "[browse]") {
    auto shard = build_mosfet_shard(fixtures_dir() + "/mosfets.ndjson");
    json all = browse::browse_rows(shard, json::object());
    json hv = browse::browse_rows(
        shard, json{{"filters", {{"vds_rated", {{"min", 100.0}}}}}, {"limit", 1000}});
    REQUIRE(hv.at("total").get<uint64_t>() < all.at("total").get<uint64_t>());
    for (const auto& row : hv.at("rows")) REQUIRE(row.at("vds_rated").get<double>() >= 100.0);
}

TEST_CASE("browse: sort by numeric field, NaN last, desc", "[browse]") {
    auto shard = build_magnetic_shard(fixtures_dir() + "/magnetics.ndjson");
    json r = browse::browse_rows(
        shard, json{{"sort", {{"field", "inductance"}, {"dir", "desc"}}}, {"limit", 1000}});
    double prev = std::numeric_limits<double>::infinity();
    bool seen_null = false;
    for (const auto& row : r.at("rows")) {
        if (row.at("inductance").is_null()) {
            seen_null = true;
            continue;
        }
        REQUIRE_FALSE(seen_null);  // once nulls start, no numbers may follow
        double v = row.at("inductance").get<double>();
        REQUIRE(v <= prev);
        prev = v;
    }
}

TEST_CASE("browse: facets are counted over all other filters, not their own", "[browse]") {
    auto shard = build_mosfet_shard(fixtures_dir() + "/mosfets.ndjson");
    json unfiltered = browse::browse_rows(shard, json{{"withFacets", true}, {"limit", 0}});
    const json& mfr_facet = unfiltered.at("facets").at("manufacturer").at("values");
    REQUIRE(mfr_facet.size() >= 2);
    std::string top_mfr = mfr_facet[0][0].get<std::string>();

    // Filtering to one manufacturer must NOT collapse the manufacturer facet itself...
    json filtered = browse::browse_rows(
        shard, json{{"filters", {{"manufacturer", {top_mfr}}}}, {"withFacets", true}, {"limit", 0}});
    REQUIRE(filtered.at("facets").at("manufacturer").at("values").size() == mfr_facet.size());
    // ...while the result set is restricted to it.
    REQUIRE(filtered.at("total").get<uint64_t>() == mfr_facet[0][1].get<uint64_t>());
    // The technology facet IS restricted by the manufacturer filter.
    uint64_t tech_total = 0;
    for (const auto& v : filtered.at("facets").at("technology").at("values"))
        tech_total += v[1].get<uint64_t>();
    REQUIRE(tech_total == filtered.at("total").get<uint64_t>());
}

TEST_CASE("browse: mpn substring filter is case-insensitive", "[browse]") {
    auto shard = build_mosfet_shard(fixtures_dir() + "/mosfets.ndjson");
    json all = browse::browse_rows(shard, json{{"limit", 1}});
    std::string mpn = all.at("rows")[0].at("mpn").get<std::string>();
    std::string probe = mpn.substr(0, std::min<size_t>(4, mpn.size()));
    for (auto& c : probe) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    json r = browse::browse_rows(shard, json{{"filters", {{"mpn", probe}}}, {"limit", 1000}});
    REQUIRE(r.at("total").get<uint64_t>() >= 1);
}

TEST_CASE("browse: pagination is stable and non-overlapping", "[browse]") {
    auto shard = build_capacitor_shard(fixtures_dir() + "/capacitors.ndjson");
    json sort = {{"field", "capacitance"}, {"dir", "asc"}};
    json p1 = browse::browse_rows(shard, json{{"sort", sort}, {"offset", 0}, {"limit", 5}});
    json p2 = browse::browse_rows(shard, json{{"sort", sort}, {"offset", 5}, {"limit", 5}});
    REQUIRE(p1.at("rows").size() == 5);
    REQUIRE(p2.at("rows").size() == 5);
    for (const auto& a : p1.at("rows"))
        for (const auto& b : p2.at("rows")) REQUIRE(a.at("lineno") != b.at("lineno"));
}

TEST_CASE("browse: numeric ranges report min/max/present", "[browse]") {
    auto shard = build_magnetic_shard(fixtures_dir() + "/magnetics.ndjson");
    json r = browse::browse_rows(shard, json{{"withFacets", true}, {"limit", 0}});
    const json& ind = r.at("ranges").at("inductance");
    REQUIRE(ind.at("present").get<uint64_t>() > 0);
    REQUIRE(ind.at("min").get<double>() <= ind.at("max").get<double>());
}

TEST_CASE("browse: unknown filter or sort field throws InvalidOptions", "[browse]") {
    auto shard = build_mosfet_shard(fixtures_dir() + "/mosfets.ndjson");
    REQUIRE_THROWS_AS(
        browse::browse_rows(shard, json{{"filters", {{"nonsense", {{"min", 1.0}}}}}}),
        InvalidOptions);
    REQUIRE_THROWS_AS(browse::browse_rows(shard, json{{"sort", {{"field", "nonsense"}}}}),
                      InvalidOptions);
}

TEST_CASE("browse: controller topologies list facet + filter", "[browse]") {
    auto shard = build_controller_shard(fixtures_dir() + "/controllers.ndjson");
    json r = browse::browse_rows(shard, json{{"withFacets", true}, {"limit", 0}});
    const json& topo = r.at("facets").at("topologies").at("values");
    REQUIRE(topo.size() >= 1);
    std::string top_topo = topo[0][0].get<std::string>();
    json f = browse::browse_rows(
        shard, json{{"filters", {{"topologies", {top_topo}}}}, {"limit", 1000}});
    REQUIRE(f.at("total").get<uint64_t>() == topo[0][1].get<uint64_t>());
}

TEST_CASE("browse: Engine guards an unloaded family when there is no data dir", "[browse]") {
    api::Engine eng("", "", /*quiet=*/true);
    REQUIRE_THROWS_AS(eng.browse("mosfet", json::object()), InvalidOptions);
}

TEST_CASE("browse: Engine browses from a data dir", "[browse]") {
    api::Engine eng(fixtures_dir(), "", /*quiet=*/true);
    json r = eng.browse("diode", json{{"limit", 3}});
    REQUIRE(r.at("family") == "diode");
    REQUIRE(r.at("rows").size() == 3);
}

TEST_CASE("browse: analog ICs — subtype facet + supply-range filter", "[browse][analog]") {
    auto shard = build_analog_shard(fixtures_dir() + "/analog_ics.ndjson");
    REQUIRE(shard.meta.row_count > 0);
    json r = browse::browse_rows(shard, json{{"withFacets", true}, {"limit", 5}});
    REQUIRE(r.at("family") == "analog");
    REQUIRE(r.at("facets").contains("device_type"));
    REQUIRE(r.at("facets").at("device_type").at("values").size() >= 1);
    const json& row = r.at("rows")[0];
    REQUIRE(row.contains("gain_bandwidth"));
    REQUIRE(row.contains("vsupply_max"));
    // numeric filter excludes rows lacking the datum
    json f = browse::browse_rows(
        shard, json{{"filters", {{"gain_bandwidth", {{"min", 1e6}}}}}, {"limit", 1000}});
    for (const auto& x : f.at("rows")) REQUIRE(x.at("gain_bandwidth").get<double>() >= 1e6);
}

TEST_CASE("browse: timing devices — technology facet + frequency sort", "[browse][timing]") {
    auto shard = build_timing_shard(fixtures_dir() + "/timing_devices.ndjson");
    REQUIRE(shard.meta.row_count > 0);
    json r = browse::browse_rows(
        shard, json{{"sort", {{"field", "frequency"}, {"dir", "asc"}}}, {"withFacets", true},
                    {"limit", 1000}});
    REQUIRE(r.at("family") == "timing");
    REQUIRE(r.at("facets").at("technology").at("values").size() >= 1);
    double prev = 0;
    for (const auto& row : r.at("rows")) {
        if (row.at("frequency").is_null()) continue;
        double v = row.at("frequency").get<double>();
        REQUIRE(v >= prev);
        prev = v;
    }
}

TEST_CASE("browse: round-trip serialize/deserialize for the new families", "[browse][index]") {
    auto a = build_analog_shard(fixtures_dir() + "/analog_ics.ndjson");
    auto a2 = deserialize_analog_shard(serialize_shard(a));
    REQUIRE(a2.rows.size() == a.rows.size());
    REQUIRE(a2.rows.front().mpn == a.rows.front().mpn);
    auto t = build_timing_shard(fixtures_dir() + "/timing_devices.ndjson");
    auto t2 = deserialize_timing_shard(serialize_shard(t));
    REQUIRE(t2.rows.size() == t.rows.size());
    REQUIRE(t2.meta.build_id == t.meta.build_id);
}

TEST_CASE("browse: connectors — family/polarity facets + positions filter", "[browse][connector]") {
    auto shard = build_connector_shard(fixtures_dir() + "/connectors.ndjson");
    REQUIRE(shard.meta.row_count > 0);
    json r = browse::browse_rows(shard, json{{"withFacets", true}, {"limit", 5}});
    REQUIRE(r.at("family") == "connector");
    REQUIRE(r.at("facets").at("polarity").at("values").size() >= 1);
    REQUIRE(r.at("facets").contains("series"));
    json f = browse::browse_rows(
        shard, json{{"filters", {{"positions", {{"min", 10.0}}}}}, {"limit", 1000}});
    for (const auto& x : f.at("rows")) REQUIRE(x.at("positions").get<double>() >= 10.0);
    // round-trip
    auto c2 = deserialize_connector_shard(serialize_shard(shard));
    REQUIRE(c2.meta.build_id == shard.meta.build_id);
    REQUIRE(c2.rows.size() == shard.rows.size());
}

TEST_CASE("browse: histogram — log buckets count every present value once", "[browse][histogram]") {
    auto shard = build_magnetic_shard(fixtures_dir() + "/magnetics.ndjson");
    json r = browse::browse_rows(
        shard, json{{"histogram", {{"field", "inductance"}, {"buckets", 12}, {"log", true}}},
                    {"limit", 0}});
    const json& h = r.at("histogram");
    REQUIRE(h.at("field") == "inductance");
    REQUIRE(h.at("log") == true);
    uint64_t total = 0;
    for (const auto& c : h.at("counts")) total += c.get<uint64_t>();
    REQUIRE(total == h.at("present").get<uint64_t>());
    REQUIRE(h.at("present").get<uint64_t>() + h.at("absent").get<uint64_t>() ==
            r.at("total").get<uint64_t>());
    REQUIRE(h.at("edges").size() == h.at("counts").size() + 1);
    // edges strictly increasing, spanning the data
    double prev = 0;
    for (const auto& e : h.at("edges")) {
        REQUIRE(e.get<double>() > prev);
        prev = e.get<double>();
    }
    // histogram respects filters: a narrow window shrinks the present count
    json f = browse::browse_rows(
        shard, json{{"filters", {{"inductance", {{"min", 1e-6}, {"max", 1e-3}}}}},
                    {"histogram", {{"field", "inductance"}, {"buckets", 6}, {"log", true}}},
                    {"limit", 0}});
    REQUIRE(f.at("histogram").at("present").get<uint64_t>() <
            h.at("present").get<uint64_t>());
    REQUIRE_THROWS_AS(
        browse::browse_rows(shard, json{{"histogram", {{"field", "nonsense"}}}}),
        InvalidOptions);
}

TEST_CASE("browse-only families: select refuses loudly", "[browse][select]") {
    api::Engine eng(fixtures_dir(), "", /*quiet=*/true);
    REQUIRE_THROWS_AS(eng.select("analog", json::object(), json::object()), InvalidOptions);
    REQUIRE_THROWS_AS(eng.select("timing", json::object(), json::object()), InvalidOptions);
}

TEST_CASE("select connector: gates + margin ranking + rejection buckets", "[select][connector]") {
    api::Engine eng(fixtures_dir(), "", /*quiet=*/true);
    // an empty requirement is a caller error, not an everything-matches query
    REQUIRE_THROWS_AS(eng.select("connector", json::object(), json::object()), InvalidOptions);

    // discover a real (family, positions) pair from the fixture via browse
    json b = eng.browse("connector", json{{"filters", {{"positions", {{"min", 1.0}}}}},
                                          {"limit", 1}});
    REQUIRE(b.at("rows").size() == 1);
    const json& probe = b.at("rows")[0];
    double positions = probe.at("positions").get<double>();
    std::string family = probe.at("family").get<std::string>();

    json r = eng.select("connector",
                        json{{"positions", positions}, {"family", family}},
                        json{{"maxCandidates", 10}});
    REQUIRE(r.at("category") == "connector");
    REQUIRE(r.at("tiebreaker") == "highest_current_margin");
    REQUIRE(r.at("candidates").size() >= 1);
    for (const auto& cand : r.at("candidates")) {
        REQUIRE(cand.at("evidence").at("positions").get<double>() == positions);
        REQUIRE(cand.at("evidence").at("family") == family);
    }
    // ranked by rated current descending (highest margin first) among documented parts
    double prev = std::numeric_limits<double>::infinity();
    for (const auto& cand : r.at("candidates")) {
        const json& rc = cand.at("evidence").at("ratedCurrentPerContact");
        if (rc.is_null()) break;  // undocumented tier sorts after every documented part
        REQUIRE(rc.get<double>() <= prev);
        prev = rc.get<double>();
    }

    // a current gate produces ratio margins >= 1 on every candidate
    json r2 = eng.select("connector", json{{"minimumCurrentPerContact", 0.5}, {"family", family}},
                         json{{"maxCandidates", 5}});
    for (const auto& cand : r2.at("candidates"))
        REQUIRE(cand.at("margins").at("current_margin").get<double>() >= 1.0);

    // an impossible gate yields NoCandidates JSON with the offending bucket via the string facade
    std::string s = api::select_string(fixtures_dir(), "", "connector",
                                       R"({"minimumCurrentPerContact": 1e9})", "{}");
    json err = json::parse(s);
    REQUIRE(err.at("error") == "NoCandidates");
    REQUIRE(err.at("rejections").contains("current_low"));
}
