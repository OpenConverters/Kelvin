// Physical-fit + MPN-decode tests: the case-code resolver, the footprint
// penalty/tier, mount-type compatibility, AEC-Q and rated-voltage decoding, the
// MLCC DC-bias model, and their integration into the ranker.
//
// The values asserted here are the published table entries and Heaviside's
// formula outputs, not this implementation's own output — a characterisation
// test that only pins current behaviour would not catch a porting error.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "../src/CrossRef.hpp"
#include "../src/CrossRefDecode.hpp"
#include "../src/CrossRefDimensions.hpp"

using namespace kelvin::crossref;
using Catch::Matchers::WithinRel;

// ── case-code resolution ─────────────────────────────────────────────────────

TEST_CASE("chip EIA codes resolve to the published footprint", "[crossref][dims]") {
    auto d = resolve_dimensions("0603", "capacitor");
    REQUIRE(d.has_value());
    CHECK_THAT(d->length, WithinRel(1.60e-3, 1e-9));
    CHECK_THAT(d->width, WithinRel(0.80e-3, 1e-9));
    // MLCC height varies with value/dielectric and is NOT encoded in the code.
    CHECK_FALSE(d->height.has_value());
}

TEST_CASE("a chip resistor keeps its standardised height", "[crossref][dims]") {
    auto d = resolve_dimensions("0603", "resistor");
    REQUIRE(d.has_value());
    REQUIRE(d->height.has_value());
    CHECK_THAT(*d->height, WithinRel(0.45e-3, 1e-9));
}

// The central category-aware rule: the same 4-digit string means different
// things per family. This is the gotcha the whole resolver exists for.
TEST_CASE("4020 is a chip footprint on a capacitor but a cube on an inductor",
          "[crossref][dims]") {
    auto chip = resolve_dimensions("2010", "capacitor");  // imperial EIA chip
    REQUIRE(chip.has_value());
    CHECK_THAT(chip->length, WithinRel(5.00e-3, 1e-9));
    CHECK_THAT(chip->width, WithinRel(2.50e-3, 1e-9));

    // "4020" is not a standard chip code, so on a magnetic it reads as a molded
    // power inductor: 4.0 x 4.0 mm square footprint x 2.0 mm high.
    auto ind = resolve_dimensions("4020", "magnetic");
    REQUIRE(ind.has_value());
    CHECK_THAT(ind->length, WithinRel(4.0e-3, 1e-9));
    CHECK_THAT(ind->width, WithinRel(4.0e-3, 1e-9));
    REQUIRE(ind->height.has_value());
    CHECK_THAT(*ind->height, WithinRel(2.0e-3, 1e-9));
}

TEST_CASE("molded tantalum letters and can DxL codes resolve", "[crossref][dims]") {
    auto b = resolve_dimensions("B", "capacitor");  // 3528-21
    REQUIRE(b.has_value());
    CHECK_THAT(b->length, WithinRel(3.50e-3, 1e-9));
    CHECK_THAT(b->width, WithinRel(2.80e-3, 1e-9));

    auto can = resolve_dimensions("10x20", "capacitor");  // diameter x height
    REQUIRE(can.has_value());
    CHECK_THAT(can->length, WithinRel(10e-3, 1e-9));
    REQUIRE(can->height.has_value());
    CHECK_THAT(*can->height, WithinRel(20e-3, 1e-9));
}

TEST_CASE("JEDEC packages and their aliases resolve", "[crossref][dims]") {
    auto sot = resolve_dimensions("SOT-23", "mosfet");
    REQUIRE(sot.has_value());
    CHECK_THAT(sot->length, WithinRel(2.90e-3, 1e-9));
    // alias: SC-59 is the same outline
    auto alias = resolve_dimensions("SC-59", "mosfet");
    REQUIRE(alias.has_value());
    CHECK_THAT(alias->length, WithinRel(sot->length, 1e-9));
    // separator normalisation
    auto loose = resolve_dimensions("sc 70", "diode");
    REQUIRE(loose.has_value());
    CHECK_THAT(loose->length, WithinRel(2.00e-3, 1e-9));
}

TEST_CASE("an unrecognised code resolves to nothing rather than a guess",
          "[crossref][dims]") {
    CHECK_FALSE(resolve_dimensions("WHO-KNOWS", "capacitor").has_value());
    CHECK_FALSE(resolve_dimensions("", "capacitor").has_value());
}

// ── footprint penalty / tier ─────────────────────────────────────────────────

TEST_CASE("footprint fit is orientation-agnostic", "[crossref][dims]") {
    Dims source{3.20e-3, 1.60e-3, std::nullopt};  // 1206
    Dims rotated{1.60e-3, 3.20e-3, std::nullopt};  // same part, rotated
    CHECK(footprint_tier(source, rotated) == FootprintTier::Fits);
}

TEST_CASE("a smaller part fits and scores below any oversize part", "[crossref][dims]") {
    Dims source{3.20e-3, 1.60e-3, std::nullopt};   // 1206
    Dims smaller{1.60e-3, 0.80e-3, std::nullopt};  // 0603
    Dims bigger{5.00e-3, 2.50e-3, std::nullopt};   // 2010
    double p_small = footprint_penalty(source, smaller);
    double p_big = footprint_penalty(source, bigger);
    CHECK(footprint_tier(source, smaller) == FootprintTier::Fits);
    CHECK(p_small < kFitAreaWeight);
    CHECK(p_big > p_small);
    // any part that fits outranks any part that does not
    CHECK(p_small < kSlightlyOversizeBase);
}

TEST_CASE("strict_case: the same footprint is the true drop-in, a smaller body is a change",
          "[crossref][dims]") {
    // Magnetics / chip beads have custom land patterns: only a same-size part keeps
    // the pads. A materially smaller body must NOT rank as a free fit.
    Dims source{7.3e-3, 7.3e-3, 4.3e-3};
    Dims same{7.3e-3, 7.3e-3, 4.3e-3};   // case kept — the true drop-in
    Dims tiny{4.45e-3, 4.45e-3, 3.0e-3}; // ~40% smaller — pads won't match
    // same size scores 0 (best) and ranks first; the smaller body is penalised.
    CHECK(footprint_penalty(source, same, /*strict*/ true) == 0.0);
    CHECK(footprint_penalty(source, tiny, /*strict*/ true) > footprint_penalty(source, same, true));
    CHECK(footprint_tier(source, same, true) == FootprintTier::Fits);
    CHECK(footprint_tier(source, tiny, true) == FootprintTier::Smaller);
    // Non-strict (chip passives) keeps the historical right-sizing: smaller still fits.
    CHECK(footprint_tier(source, tiny) == FootprintTier::Fits);
}

TEST_CASE("one case size up is a partial, two sizes up is a respin", "[crossref][dims]") {
    Dims source{1.00e-3, 0.50e-3, std::nullopt};   // 0402
    Dims one_up{1.60e-3, 0.80e-3, std::nullopt};   // 0603: 0.6 linear overflow
    Dims way_up{3.20e-3, 1.60e-3, std::nullopt};   // 1206
    CHECK(footprint_tier(source, one_up) == FootprintTier::OneSizeLarger);
    CHECK(footprint_tier(source, way_up) == FootprintTier::Overflows);
    CHECK(footprint_penalty(source, way_up) >= kOversizeBase);
}

TEST_CASE("height is enforced when both sides know it", "[crossref][dims]") {
    Dims source{4.0e-3, 4.0e-3, 2.0e-3};
    Dims tall{4.0e-3, 4.0e-3, 6.0e-3};  // same footprint, three times the height
    CHECK(footprint_tier(source, tall) != FootprintTier::Fits);
}

TEST_CASE("unknown dimensions are penalised, not silently passed", "[crossref][dims]") {
    Dims source{3.20e-3, 1.60e-3, std::nullopt};
    CHECK(footprint_penalty(source, std::nullopt) == kUnknownDimPenalty);
    CHECK(footprint_tier(source, std::nullopt) == FootprintTier::Unknown);
    // An unknown SOURCE cannot be enforced at all — no penalty, surfaced elsewhere.
    CHECK(footprint_penalty(std::nullopt, source) == 0.0);
}

// ── mount type ───────────────────────────────────────────────────────────────

TEST_CASE("SMD and leaded are mutually incompatible", "[crossref][dims]") {
    CHECK(mount_type_incompatible("0805", "TO-220"));
    CHECK(mount_type_incompatible("RADIAL", "SOIC-8"));
    CHECK_FALSE(mount_type_incompatible("0805", "0603"));
    CHECK_FALSE(mount_type_incompatible("TO-220", "TO-247"));
    // an unreadable package string never triggers a rejection
    CHECK_FALSE(mount_type_incompatible("", "TO-220"));
    CHECK_FALSE(mount_type_incompatible("MYSTERY", "TO-220"));
    // the gate is skipped for the categories whose packages vary too much
    CHECK_FALSE(mount_gate_applies("magnetic"));
    CHECK_FALSE(mount_gate_applies("mosfet"));
    CHECK(mount_gate_applies("capacitor"));
}

// ── MPN decoding ─────────────────────────────────────────────────────────────

TEST_CASE("automotive grade is read from vendor MPN conventions", "[crossref][decode]") {
    CHECK(is_automotive_grade("GCM188R71H104KA57D"));   // Murata automotive
    CHECK(is_automotive_grade("CGA4J2X7R1H104K125AA")); // TDK automotive
    CHECK(is_automotive_grade("C0805X7R1H104K-AUTO"));
    CHECK_FALSE(is_automotive_grade("GRM188R71H104KA93D"));  // GRM = commercial
    CHECK_FALSE(is_automotive_grade(""));
}

TEST_CASE("capacitor rated voltage decodes from the MPN", "[crossref][decode]") {
    auto d = decode_cap_mpn("GRM188R71H104KA93D");  // 1H = 50 V
    REQUIRE(d.voltage.has_value());
    CHECK_THAT(*d.voltage, WithinRel(50.0, 1e-9));

    auto lit = decode_cap_mpn("C0805X7R500104K");  // literal dielectric
    CHECK(lit.dielectric == "X7R");

    // Nothing decodable -> nothing claimed.
    auto none = decode_cap_mpn("SOME-PART-9000");
    CHECK_FALSE(none.voltage.has_value());
    CHECK(none.dielectric.empty());
}

// ── MLCC DC bias ─────────────────────────────────────────────────────────────

TEST_CASE("class-2 MLCC loses capacitance under DC bias", "[crossref][decode]") {
    // C_nom 10uF, 25V rated, 30% remaining at rated, 50% loss at 8V.
    auto c = effective_capacitance_at_bias(10e-6, 25.0, 0.30, 8.0, 8.0);
    REQUIRE(c.has_value());
    // at vth the model is exactly half the nameplate, by construction
    CHECK_THAT(*c, WithinRel(5e-6, 1e-9));
    // more bias -> less capacitance
    auto more = effective_capacitance_at_bias(10e-6, 25.0, 0.30, 8.0, 16.0);
    REQUIRE(more.has_value());
    CHECK(*more < *c);
}

TEST_CASE("DC bias returns nothing when an anchor is missing", "[crossref][decode]") {
    CHECK_FALSE(effective_capacitance_at_bias(10e-6, 25.0, std::nullopt, 8.0, 8.0).has_value());
    CHECK_FALSE(effective_capacitance_at_bias(10e-6, 25.0, 0.30, std::nullopt, 8.0).has_value());
    // class-1 style (no derating anchors) -> unverified, never an estimate
    CHECK_FALSE(effective_capacitance_at_bias(10e-6, std::nullopt, 0.30, 8.0, 8.0).has_value());
}

// ── ranker integration ───────────────────────────────────────────────────────

TEST_CASE("a mount-type mismatch rejects the candidate", "[crossref][rank][dims]") {
    json original = {{"mpn", "O"}, {"value_si", 1e-7}, {"voltage", 50.0}, {"package", "0805"}};
    json cands = json::array({
        {{"mpn", "THT"}, {"value_si", 1e-7}, {"voltage", 50.0}, {"package", "RADIAL"}}});
    auto r = cross_reference("capacitor", original, cands, Options{});
    CHECK(r["candidates"][0]["status"] == "no_substitute");
}

TEST_CASE("an oversize part is demoted to partial, not dropped", "[crossref][rank][dims]") {
    json original = {{"mpn", "O"}, {"value_si", 1e-7}, {"voltage", 50.0}, {"package", "0402"}};
    json cands = json::array({
        {{"mpn", "BIG"}, {"value_si", 1e-7}, {"voltage", 50.0}, {"package", "1206"}}});
    auto r = cross_reference("capacitor", original, cands, Options{});
    // still offered — it is a real part that works electrically
    CHECK(r["candidates"][0]["status"] == "partial");
    CHECK(r["candidates"][0]["footprint"] == "overflows");
}

TEST_CASE("a fitting part outranks an oversize one", "[crossref][rank][dims]") {
    json original = {{"mpn", "O"}, {"value_si", 1e-7}, {"voltage", 50.0}, {"package", "0805"}};
    json cands = json::array({
        {{"mpn", "BIG"}, {"value_si", 1e-7}, {"voltage", 50.0}, {"package", "1210"}},
        {{"mpn", "FITS"}, {"value_si", 1e-7}, {"voltage", 50.0}, {"package", "0603"}}});
    auto r = cross_reference("capacitor", original, cands, Options{});
    CHECK(r["candidates"][0]["mpn"] == "FITS");
}

TEST_CASE("an automotive original is not silently replaced by a commercial part",
          "[crossref][rank][decode]") {
    json original = {{"mpn", "GCM188R71H104KA57D"}, {"value_si", 1e-7}, {"voltage", 50.0}};
    json cands = json::array({
        {{"mpn", "GRM188R71H104KA93D"}, {"value_si", 1e-7}, {"voltage", 50.0}}});
    auto r = cross_reference("capacitor", original, cands, Options{});
    CHECK(r["candidates"][0]["status"] == "partial");
}

TEST_CASE("a non-production substitute is surfaced", "[crossref][rank]") {
    json original = {{"mpn", "O"}, {"value_si", 1e-7}, {"voltage", 50.0}};
    json cands = json::array({
        {{"mpn", "EOL"}, {"value_si", 1e-7}, {"voltage", 50.0}, {"is_production", false}}});
    auto r = cross_reference("capacitor", original, cands, Options{});
    CHECK(r["candidates"][0]["status"] == "partial");
}

TEST_CASE("a dielectric class regression is caught", "[crossref][rank]") {
    // C0G -> X5R is a real regression: far worse temperature and bias behaviour.
    json original = {{"mpn", "O"}, {"value_si", 1e-9}, {"voltage", 50.0}, {"technology", "C0G"}};
    json cands = json::array({
        {{"mpn", "X5R"}, {"value_si", 1e-9}, {"voltage", 50.0}, {"technology", "X5R"}}});
    auto r = cross_reference("capacitor", original, cands, Options{});
    CHECK(r["candidates"][0]["status"] != "recommended");
}

TEST_CASE("connectors and analog parts now rank (previously uncovered)", "[crossref][rank]") {
    json original = {{"mpn", "O"}, {"family", "terminalBlock"}, {"positions", 6},
                     {"rated_current_A", 10.0}};
    json cands = json::array({
        {{"mpn", "SAME"}, {"family", "terminalBlock"}, {"positions", 6}, {"rated_current_A", 12.0}},
        {{"mpn", "WRONGPOS"}, {"family", "terminalBlock"}, {"positions", 4},
         {"rated_current_A", 12.0}}});
    auto r = cross_reference("connector", original, cands, Options{});
    CHECK(r["candidates"][0]["mpn"] == "SAME");
    // a different position count is a different part, not a partial substitute
    auto wrong = std::find_if(r["candidates"].begin(), r["candidates"].end(),
                              [](const json& c) { return c["mpn"] == "WRONGPOS"; });
    REQUIRE(wrong != r["candidates"].end());
    CHECK((*wrong)["status"] == "no_substitute");
}

// ── shard round-trip ─────────────────────────────────────────────────────────
// Dimensions are only useful if they survive indexing. This guards the v2 shard
// format: a field-order slip would silently mis-parse every row.
#include "../src/Browse.hpp"
#include "../src/Index.hpp"

TEST_CASE("dimensions survive the shard round-trip and reach browse rows",
          "[crossref][dims][browse]") {
    auto shard = kelvin::build_mosfet_shard(std::string(KELVIN_TEST_DIR) + "/fixtures/mosfets.ndjson");
    json r = kelvin::browse::browse_rows(shard, json{{"limit", 200}});
    REQUIRE(r.at("rows").size() > 0);

    size_t with_dims = 0;
    for (const auto& row : r.at("rows")) {
        REQUIRE(row.contains("lengthM"));  // key always present, null when unknown
        REQUIRE(row.contains("widthM"));
        if (!row.at("lengthM").is_null() && !row.at("widthM").is_null()) {
            ++with_dims;
            // a real body size, in metres — not zero, not millimetres
            double l = row.at("lengthM").get<double>();
            CHECK(l > 0.0);
            CHECK(l < 0.5);  // half a metre would mean the units are wrong
        }
    }
    // The fixture carries mechanical drawings; if none survived, indexing dropped them.
    CHECK(with_dims > 0);
}

TEST_CASE("an explicit assembly type beats package-string inference", "[crossref][dims]") {
    // Both records state their mount type -> use it, ignore the package strings.
    CHECK(mount_incompatible("smt", "tht", "", ""));
    CHECK_FALSE(mount_incompatible("smt", "smt", "", ""));
    // "chassis" does not classify as either -> fall back to the package strings.
    CHECK_FALSE(mount_incompatible("chassis", "smt", "0805", "0603"));
    CHECK(mount_incompatible("chassis", "smt", "0805", "TO-220"));
    // No explicit types at all -> package inference, as before.
    CHECK(mount_incompatible("", "", "0805", "TO-220"));
    CHECK(normalize_mount("SMT") == "smd");
    CHECK(normalize_mount("tht") == "leaded");
    CHECK(normalize_mount("chassis").empty());
}

TEST_CASE("caller identity travels in `_key`, leaving `mpn` decodable", "[crossref][rank]") {
    // Two vendors can ship the same MPN string, so callers pass their own `id`.
    // It must be echoed back UNCHANGED, and must not displace `mpn` — the AEC-Q
    // and rated-voltage gates decode `mpn`, and a composite key decodes to
    // nothing, silently disabling both gates.
    json original = {{"mpn", "GCM188R71H104KA57D"}, {"_key", "Murata|GCM188R71H104KA57D"},
                     {"value_si", 1e-7}, {"voltage", 50.0}};
    json cands = json::array({
        {{"mpn", "GRM188R71H104KA93D"}, {"_key", "Murata|GRM188R71H104KA93D"},
         {"value_si", 1e-7}, {"voltage", 50.0}}});
    auto r = cross_reference("capacitor", original, cands, Options{});
    const json& c = r["candidates"][0];
    CHECK(c["_key"] == "Murata|GRM188R71H104KA93D");
    CHECK(c["mpn"] == "GRM188R71H104KA93D");
    // the automotive gate still fires, which it cannot do on a composite key
    CHECK(c["status"] == "partial");
}

TEST_CASE("a caller identity key never masks a physical parameter", "[crossref][rank]") {
    // The identity field used to be `id`, which is ALSO the MOSFET drain-current
    // parameter. The key overwrote the current, num() read the string as absent,
    // and the drain-current comparison silently disabled itself in production:
    // a 31 A part ranked top against a 200 A original. This asserts the current
    // is still compared while an identity key is present.
    json original = {{"mpn", "ORIG"}, {"_key", "TI|ORIG"}, {"vds", 60.0}, {"id", 200.0},
                     {"rds_on", 0.00065}};
    json cands = json::array({
        {{"mpn", "WEAK"}, {"_key", "Infineon|WEAK"}, {"vds", 650.0}, {"id", 31.0},
         {"rds_on", 0.099}},
        {{"mpn", "STRONG"}, {"_key", "TI|STRONG"}, {"vds", 100.0}, {"id", 200.0},
         {"rds_on", 0.0009}}});
    auto r = cross_reference("mosfet", original, cands, Options{});
    CHECK(r["candidates"][0]["mpn"] == "STRONG");
    auto weak = std::find_if(r["candidates"].begin(), r["candidates"].end(),
                             [](const json& c) { return c["mpn"] == "WEAK"; });
    REQUIRE(weak != r["candidates"].end());
    // the drain-current shortfall must actually register
    bool id_failed = false;
    for (const auto& p : (*weak)["params"])
        if (p["name"] == "id" && p["verdict"] == FAIL) id_failed = true;
    CHECK(id_failed);
    CHECK((*weak)["penalty"].get<double>() > 100.0);
}

TEST_CASE("a string where a numeric parameter belongs is rejected loudly",
          "[crossref][rank]") {
    // The silent-masking failure mode: `id` is the MOSFET drain current, so a
    // caller reusing it as a row key removed the check without any signal.
    json original = {{"mpn", "O"}, {"vds", 60.0}, {"id", 200.0}};
    json bad = json::array({{{"mpn", "C"}, {"vds", 60.0}, {"id", "Vendor|C"}}});
    CHECK_THROWS_AS(cross_reference("mosfet", original, bad, Options{}), std::invalid_argument);
}
