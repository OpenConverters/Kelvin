// v23 curve shards (owner 2026-11-15): a family's measured curves live in <family>.curves.kidx,
// apart from its base shard <family>.kidx. These tests load real catalogue fixtures both ways —
// built in memory (curves inline in the rows, the native path) and split into base bytes plus
// curve bytes (the browser path) — and check that:
//   a. a base shard alone browses, its rows say "curvesOmitted", and anything that needs curves
//      (browse with curves: true, an operating-point loss select) THROWS rather than reading an
//      unloaded curve as a record that states none;
//   b. base + curve shard browse exactly as the in-memory build;
//   c. a curve shard that is not this base's pair (another build, a tampered byte, a base shard
//      passed as curves) is refused;
//   d. selects ranked by operating-point loss (capacitor, MOSFET) and a capacitor
//      cross-reference give identical JSON inline and split.
// Fixtures (verbatim catalogue records): capacitors_op_loss.ndjson, mosfets_op_loss_ranking.ndjson,
// curves_capacitors.ndjson.
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "Browse.hpp"
#include "CrossRef.hpp"
#include "Index.hpp"
#include "KelvinApi.hpp"

using nlohmann::json;
using namespace kelvin;
namespace fs = std::filesystem;

namespace {
std::string fixture(const std::string& f) { return std::string(KELVIN_TEST_DIR) + "/fixtures/" + f; }

std::string slurp(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A data dir holding one family's fixture under the name the engine reads.
struct DataDir {
    fs::path dir;
    DataDir(const std::string& tag, const std::string& fixture_name, const std::string& as) {
        dir = fs::path(KELVIN_TEST_DIR) / ("_curve_shards_" + tag);
        fs::create_directories(dir);
        fs::copy_file(fixture(fixture_name), dir / as, fs::copy_options::overwrite_existing);
    }
    ~DataDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// The capacitor buck-output point of test_capacitor_loss.cpp ("a tier under the operating-point
// loss"), and the MOSFET buck high-side point of test_mosfet_loss.cpp (Su et al.).
constexpr double kDipp = 24.0 * (12.0 / 36.0) / (180e-6 * 80e3);
json cap_req() {
    return {{"capacitance", 150e-6}, {"ratedVoltage", 14.4},
            {"minimumRippleCurrent", 1.25 * kDipp / std::sqrt(12.0)}};
}
json cap_opts() {
    return {{"lossOperatingPoint",
             {{"position", "buckOutput"}, {"inputVoltage", 36}, {"outputVoltage", 12},
              {"outputCurrent", 2}, {"switchingFrequency", 80e3}, {"inductorRippleCurrent", kDipp}}},
            {"includeEnvelope", false}};
}
json mos_req() {
    return {{"ratedDrainSourceVoltage", 43.2}, {"ratedContinuousDrainCurrent", 2},
            {"maximumOnResistance", 1}};
}
json mos_opts() {
    return {{"lossOperatingPoint",
             {{"position", "buckHighSide"}, {"inputVoltage", 36}, {"outputVoltage", 12},
              {"outputCurrent", 2}, {"switchingFrequency", 80e3}, {"gateDriveVoltage", 10},
              {"gateDriveCurrent", 1}}},
            {"includeEnvelope", false}};
}

// web/src/crossref.js capacitor spec (as tests/test_capacitor_curves.cpp builds it), with the
// curves taken exactly as the browse row carries them.
json cap_spec(const CapacitorRow& r) {
    auto nz = [](double v) { return std::isnan(v) || !(v > 0) ? json(nullptr) : json(v); };
    auto raw = [](double v) { return std::isnan(v) ? json(nullptr) : json(v); };
    auto s = [](const std::string& v) { return v.empty() ? json(nullptr) : json(v); };
    json o{{"mpn", r.mpn}, {"length_m", nz(r.length_m)}, {"width_m", nz(r.width_m)},
           {"height_m", nz(r.height_m)}, {"case_code", r.case_code}, {"mount", r.mount},
           {"lead_pitch_m", nz(r.lead_pitch_m)},
           {"lifecycle_status", s(r.status)},
           {"value_si", nz(r.capacitance)}, {"voltage", nz(r.v_rated)}, {"esr", nz(r.esr)},
           {"ripple_current", nz(r.ripple_current_rms)}, {"technology", r.technology},
           {"dielectric_code", r.dielectric_code}, {"family", r.family},
           {"esr_frequency", nz(r.esr_frequency)}, {"temp_min_C", raw(r.temp_min_c)},
           {"temp_max_C", raw(r.temp_max_c)},
           {"ripple_current_frequency", nz(r.ripple_current_frequency)},
           {"ripple_current_temperature", raw(r.ripple_current_temperature)},
           {"capacitance_min", nz(r.capacitance_min)}, {"capacitance_max", nz(r.capacitance_max)},
           {"lead_form", s(r.lead_form)}, {"qualified_spec", s(r.qualified_spec)},
           {"qualified_grade", s(r.qualified_grade)},
           {"qualified_failure_rate", raw(r.qualified_failure_rate)}};
    json extra;
    browse::extra_row_json(r, extra);
    for (auto& [k, v] : extra.items()) o[k] = v;
    return o;
}

json cross_ref_all(const Shard<CapacitorRow>& sh, const std::string& orig_mpn) {
    const CapacitorRow* orig = nullptr;
    json cands = json::array();
    for (const auto& r : sh.rows) {
        if (r.mpn == orig_mpn) orig = &r;
        else cands.push_back(cap_spec(r));
    }
    REQUIRE(orig != nullptr);
    crossref::Options opt;
    opt.max_results = 50;
    return crossref::cross_reference("capacitor", cap_spec(*orig), cands, opt);
}

// A copy of `fixture_name` with its first occurrence of `from` replaced by `to`; returns its path.
std::string modified_copy(const std::string& fixture_name, const std::string& from,
                          const std::string& to, const fs::path& out) {
    std::string s = slurp(fixture(fixture_name));
    const auto at = s.find(from);
    REQUIRE(at != std::string::npos);
    s.replace(at, from.size(), to);
    std::ofstream(out, std::ios::binary) << s;
    return out.string();
}
}  // namespace

TEST_CASE("curve shards a: a base shard alone browses but refuses every curve read",
          "[curve_shards]") {
    const auto built = build_capacitor_shard(fixture("capacitors_op_loss.ndjson"));
    REQUIRE(built.rows.size() > 0);
    api::Engine eng("", "", /*quiet=*/true);
    eng.load_shard_bytes("capacitor", serialize_shard(built));

    const json page = eng.browse("capacitor", json{{"limit", 100}});
    REQUIRE(page.at("rows").size() == built.rows.size());
    for (const auto& r : page.at("rows")) {
        CHECK(r.value("curvesOmitted", false));
        CHECK_FALSE(r.contains("esr_curve"));
        CHECK_FALSE(r.contains("impedance_curve"));
    }
    // Asking for curves without the curve shard: thrown, never an empty curve.
    CHECK_THROWS_AS(eng.browse("capacitor", json{{"limit", 100}, {"curves", true}}),
                    CurvesNotLoaded);
    // The operating-point loss reads the candidates' curves (ESR(f) ...): thrown too.
    CHECK_THROWS_AS(eng.select("capacitor", cap_req(), cap_opts()), CurvesNotLoaded);

    // The same for a MOSFET base shard (Coss(Vds), Rds(on)(Tj), Qg curves).
    const auto mos = build_mosfet_shard(fixture("mosfets_op_loss_ranking.ndjson"));
    eng.load_shard_bytes("mosfet", serialize_shard(mos));
    CHECK_THROWS_AS(eng.select("mosfet", mos_req(), mos_opts()), CurvesNotLoaded);

    // A deserialized base shard's rows are detached: a direct curve read throws.
    const auto base = deserialize_capacitor_shard(serialize_shard(built));
    CHECK(base.rows[0].curves.detached());
    CHECK_THROWS_AS(base.rows[0].curves->esr_curve, CurvesNotLoaded);
    CHECK_THROWS_AS(serialize_curve_shard(base), CurvesNotLoaded);
}

TEST_CASE("curve shards b: base + curve shard browse exactly as the in-memory build",
          "[curve_shards]") {
    for (const std::string f : {"capacitors_op_loss.ndjson", "curves_capacitors.ndjson"}) {
        INFO(f);
        DataDir dd("b", f, "capacitors.ndjson");
        api::Engine inline_eng(dd.dir.string(), "", /*quiet=*/true);
        const auto built = build_capacitor_shard((dd.dir / "capacitors.ndjson").string());

        api::Engine split("", "", /*quiet=*/true);
        split.load_shard_bytes("capacitor", serialize_shard(built));
        const ShardMeta m = split.load_curve_shard_bytes("capacitor", serialize_curve_shard(built));
        CHECK(m.build_id == built.meta.build_id);
        CHECK(m.curves_build_id == built.meta.curves_build_id);

        const json q{{"limit", 1000}, {"curves", true}};
        const json a = inline_eng.browse("capacitor", q);
        const json b = split.browse("capacitor", q);
        REQUIRE(a.at("rows").size() == built.rows.size());
        CHECK(a.dump() == b.dump());
        // The curves are really there (not an all-null page that would compare equal anyway).
        size_t with_curves = 0;
        for (const auto& r : b.at("rows"))
            for (const char* k : {"esr_curve", "impedance_curve", "capacitance_frequency_curve",
                                  "esl_curve", "capacitance_bias_curve", "ripple_f_curve",
                                  "ripple_t_curve"})
                with_curves += r.at(k).is_null() ? 0 : 1;
        CHECK(with_curves > 0);
        // And a plain browse is the same in both (curves omitted on both).
        CHECK(inline_eng.browse("capacitor", json{{"limit", 1000}}).dump() ==
              split.browse("capacitor", json{{"limit", 1000}}).dump());
    }
}

TEST_CASE("curve shards c: a curve shard that is not this base's pair is refused",
          "[curve_shards]") {
    const auto built = build_capacitor_shard(fixture("capacitors_op_loss.ndjson"));
    const std::string base = serialize_shard(built);
    const std::string curves = serialize_curve_shard(built);

    // Another build: the same fixture with one whitespace byte added inside its first record
    // (that row's byte span and every later row's offset change, in the base shard only; every
    // curve and line number is the same, so only the base-build pairing tells the two apart).
    const fs::path tmp = fs::path(KELVIN_TEST_DIR) / "_curve_shards_c.ndjson";
    const auto other = build_capacitor_shard(
        modified_copy("capacitors_op_loss.ndjson", "\"partNumber\"", "\"partNumber\" ", tmp));
    fs::remove(tmp);
    REQUIRE(other.rows.size() == built.rows.size());
    REQUIRE(other.meta.build_id != built.meta.build_id);
    REQUIRE(other.meta.curves_build_id == built.meta.curves_build_id);  // the premise
    {
        auto sh = deserialize_capacitor_shard(base);
        CHECK_THROWS(attach_curve_shard(sh, serialize_curve_shard(other)));
        CHECK(sh.rows[0].curves.detached());  // a refused shard changes nothing
        api::Engine eng("", "", /*quiet=*/true);
        eng.load_shard_bytes("capacitor", base);
        CHECK_THROWS(eng.load_curve_shard_bytes("capacitor", serialize_curve_shard(other)));
        CHECK_THROWS_AS(eng.browse("capacitor", json{{"limit", 5}, {"curves", true}}),
                        CurvesNotLoaded);
    }
    // A tampered byte in the curve body.
    {
        std::string bad = curves;
        bad[bad.size() - 3] ^= 0x01;
        auto sh = deserialize_capacitor_shard(base);
        CHECK_THROWS(attach_curve_shard(sh, bad));
        CHECK(sh.rows[0].curves.detached());
    }
    // A base shard handed over as curves, and a curve shard handed over as a base.
    {
        auto sh = deserialize_capacitor_shard(base);
        CHECK_THROWS(attach_curve_shard(sh, base));
        CHECK_THROWS(deserialize_capacitor_shard(curves));
    }
    // Another family's curve shard.
    {
        const auto mos = build_mosfet_shard(fixture("mosfets_op_loss_ranking.ndjson"));
        auto sh = deserialize_capacitor_shard(base);
        CHECK_THROWS(attach_curve_shard(sh, serialize_curve_shard(mos)));
    }
    // The real pair attaches.
    {
        auto sh = deserialize_capacitor_shard(base);
        attach_curve_shard(sh, curves);
        CHECK_FALSE(sh.rows[0].curves.detached());
    }
}

TEST_CASE("curve shards d: operating-point selects and a cross-reference are identical inline "
          "and split", "[curve_shards]") {
    // Capacitor operating-point loss.
    {
        DataDir dd("d_cap", "capacitors_op_loss.ndjson", "capacitors.ndjson");
        api::Engine inline_eng(dd.dir.string(), "", /*quiet=*/true);
        const auto built = build_capacitor_shard((dd.dir / "capacitors.ndjson").string());
        api::Engine split("", "", /*quiet=*/true);
        split.load_shard_bytes("capacitor", serialize_shard(built));
        split.load_curve_shard_bytes("capacitor", serialize_curve_shard(built));
        const json a = inline_eng.select("capacitor", cap_req(), cap_opts());
        const json b = split.select("capacitor", cap_req(), cap_opts());
        CHECK(a.at("tiebreaker") == "lowest_operating_point_loss");
        REQUIRE(a.at("candidates").size() == 7);
        CHECK(a.dump() == b.dump());
    }
    // MOSFET operating-point loss.
    {
        DataDir dd("d_mos", "mosfets_op_loss_ranking.ndjson", "mosfets.ndjson");
        api::Engine inline_eng(dd.dir.string(), "", /*quiet=*/true);
        const auto built = build_mosfet_shard((dd.dir / "mosfets.ndjson").string());
        api::Engine split("", "", /*quiet=*/true);
        split.load_shard_bytes("mosfet", serialize_shard(built));
        split.load_curve_shard_bytes("mosfet", serialize_curve_shard(built));
        const json a = inline_eng.select("mosfet", mos_req(), mos_opts());
        const json b = split.select("mosfet", mos_req(), mos_opts());
        REQUIRE(a.at("candidates").size() == 4);
        CHECK(a.dump() == b.dump());
    }
    // Capacitor cross-reference over the curve fixture: every other record as a candidate.
    {
        const auto built = build_capacitor_shard(fixture("curves_capacitors.ndjson"));
        auto split = deserialize_capacitor_shard(serialize_shard(built));
        attach_curve_shard(split, serialize_curve_shard(built));
        const json a = cross_ref_all(built, "CL05A225KP5NSNC");
        const json b = cross_ref_all(split, "CL05A225KP5NSNC");
        REQUIRE(a.at("candidates").size() > 0);
        CHECK(a.dump().find("esr_curve") != std::string::npos);  // curve verdicts are in it
        CHECK(a.dump() == b.dump());
    }
}

// e. The cross-reference with the candidates' curves attached from the curve shard (owner
// 2026-11-20): the caller builds each spec off a browse row without curves (every curve key
// null), names its row by lineno, and the engine fills the curves in — the specs, and so the
// ranking, are then exactly those built from rows carrying their curves.
TEST_CASE("curve shards e: candidates' curves attached from the curve shard rank identically",
          "[curve_shards]") {
    const auto built = build_capacitor_shard(fixture("curves_capacitors.ndjson"));
    api::Engine eng("", "", /*quiet=*/true);
    eng.load_shard_bytes("capacitor", serialize_shard(built));
    // A curve-less spec: what web/src/crossref.js builds off a row browsed without curves.
    auto bare = [](const CapacitorRow& r) {
        json o = cap_spec(r);
        json curves;
        browse::curve_row_json(r, curves);
        for (auto it = curves.begin(); it != curves.end(); ++it) o[it.key()] = nullptr;
        return o;
    };
    const CapacitorRow* orig = nullptr;
    json full = json::array(), cands = json::array(), linenos = json::array();
    for (const auto& r : built.rows) {
        if (r.mpn == "CL05A225KP5NSNC") { orig = &r; continue; }
        full.push_back(cap_spec(r));
        cands.push_back(bare(r));
        linenos.push_back(r.lineno);
    }
    REQUIRE(orig != nullptr);
    REQUIRE(cands.size() > 1);

    // Without the curve shard the attach throws: an unloaded curve is not a stated-none one.
    {
        json c = cands;
        CHECK_THROWS_AS(eng.attach_candidate_curves("capacitor", c, linenos), CurvesNotLoaded);
        CHECK_THROWS_AS(eng.row_curves("capacitor", linenos[0]), CurvesNotLoaded);
    }
    eng.load_curve_shard_bytes("capacitor", serialize_curve_shard(built));

    json attached = cands;
    eng.attach_candidate_curves("capacitor", attached, linenos);
    CHECK(attached.dump() == full.dump());
    crossref::Options opt;
    opt.max_results = 50;
    const json a = crossref::cross_reference("capacitor", cap_spec(*orig), full, opt);
    const json b = crossref::cross_reference("capacitor", cap_spec(*orig), attached, opt);
    REQUIRE(a.at("candidates").size() > 0);
    CHECK(a.dump().find("esr_curve") != std::string::npos);
    CHECK(a.dump() == b.dump());

    // row_curves: the curves a browse with curves: true puts on the row.
    const json page = eng.browse("capacitor", {{"limit", 1000}, {"curves", true}});
    const json& r0 = page.at("rows").at(0);
    const json rc = eng.row_curves("capacitor", r0.at("lineno"));
    REQUIRE(rc.contains("esr_curve"));
    for (auto it = rc.begin(); it != rc.end(); ++it) CHECK(r0.at(it.key()) == it.value());

    // A key the projection leaves out stays out; one it already filled is refused.
    {
        json c = cands;
        c[0].erase("esl_curve");
        eng.attach_candidate_curves("capacitor", c, linenos);
        CHECK_FALSE(c[0].contains("esl_curve"));
    }
    {
        json c = cands;
        c[0]["esr_curve"] = json::object();
        CHECK_THROWS_AS(eng.attach_candidate_curves("capacitor", c, linenos), InvalidOptions);
    }
    // A lineno naming no row, or another row than the candidate's, is refused.
    {
        json c = cands, ln = linenos;
        ln[0] = 999999;
        CHECK_THROWS_AS(eng.attach_candidate_curves("capacitor", c, ln), InvalidOptions);
        ln = linenos;
        std::swap(ln[0], ln[1]);
        CHECK_THROWS_AS(eng.attach_candidate_curves("capacitor", c, ln), InvalidOptions);
        ln.erase(ln.size() - 1);
        CHECK_THROWS_AS(eng.attach_candidate_curves("capacitor", c, ln), InvalidOptions);
    }
}
