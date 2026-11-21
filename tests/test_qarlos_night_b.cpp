// Qarlos rounds 32-33 (Kelvin 9416f0b, shards v25, TAS 33069cb): the analog cross-reference.
// The records in tests/fixtures/qarlos_night_b_analog.ndjson are the TAS 33069cb lines, verbatim.
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "CrossRef.hpp"
#include "Views.hpp"

using nlohmann::json;

namespace {
kelvin::AnalogRow night_b_row(const std::string& mpn) {
    std::ifstream in(std::string(KELVIN_TEST_DIR) + "/fixtures/qarlos_night_b_analog.ndjson");
    std::string line;
    while (std::getline(in, line))
        if (line.find("\"reference\": \"" + mpn + "\"") != std::string::npos) {
            auto r = kelvin::extract_analog(json::parse(line));
            REQUIRE(r.has_value());
            return *r;
        }
    throw std::runtime_error("fixture has no analog record " + mpn);
}
}  // namespace

TEST_CASE("qarlos r32-33: an analog switch's channel count is its numberOfSwitches",
          "[qarlos-night-b][qarlos-night-b-channels]") {
    // MAX4742 states numberOfSwitches 2, TMUX9832 32, ADG1402 1, ADG788 / TMUX5411 4: before
    // the fix channels was NaN on every switch record.
    CHECK(night_b_row("MAX4742").channels == 2.0);
    CHECK(night_b_row("TMUX9832").channels == 32.0);
    CHECK(night_b_row("ADG1402").channels == 1.0);
    CHECK(night_b_row("ADG788").channels == 4.0);
    CHECK(night_b_row("TMUX5411").channels == 4.0);
    // A multiplexer keeps reading numberOfChannels (DG406: 16).
    CHECK(night_b_row("DG406").channels == 16.0);
}

namespace {
// The spec web/src/crossref.js projects from a browse row (the analog family's spec()): NaN and
// "" read as null, the record's status as lifecycle_status.
json night_b_spec(const kelvin::AnalogRow& r) {
    auto num = [](double v) { return std::isnan(v) ? json(nullptr) : json(v); };
    auto txt = [](const std::string& s) { return s.empty() ? json(nullptr) : json(s); };
    return {{"mpn", r.mpn},
            {"manufacturer", r.manufacturer},
            {"lifecycle_status", txt(r.status)},
            {"subtype", txt(r.device_type)},
            {"channels", num(r.channels)},
            {"configuration", txt(r.configuration)},
            {"supply_min_V", num(r.vsupply_min)},
            {"supply_max_V", num(r.vsupply_max)},
            {"on_resistance", num(r.on_resistance)}};
}
json night_b_spec(const std::string& mpn) { return night_b_spec(night_b_row(mpn)); }
json night_b_one(const json& orig, const json& cand) {
    const json r = kelvin::crossref::cross_reference("analog", orig, json::array({cand}),
                                                     kelvin::crossref::Options{});
    REQUIRE(r["candidates"].size() == 1);
    return r["candidates"][0];
}
bool night_b_note(const json& c, const std::string& s) {
    if (!c.contains("notes")) return false;
    for (const auto& n : c["notes"])
        if (n.get<std::string>().find(s) != std::string::npos) return true;
    return false;
}
std::string night_b_verdict(const json& c, const std::string& name) {
    for (const auto& p : c["params"])
        if (p["name"] == name) return p["verdict"];
    return "";
}
}  // namespace

TEST_CASE("qarlos r32-33: an analog switch / multiplexer configuration is an identity",
          "[qarlos-night-b][qarlos-night-b-config]") {
    // The rows carry the stated arrangement.
    CHECK(night_b_row("ADG1402").configuration == "SPST-NC");
    CHECK(night_b_row("TMUX5411").configuration == "SPST-NO");
    CHECK(night_b_row("ADG788").configuration == "SPDT");
    CHECK(night_b_row("LMH6586").configuration == "Crosspoint/exchange");
    CHECK(night_b_row("DG406").configuration == "16:1");

    // ADG788 (SPDT, 4) -> TMUX5411 (SPST-NO, 4): same count, different switch (r33 35970459).
    const json spdt = night_b_one(night_b_spec("ADG788"), night_b_spec("TMUX5411"));
    CHECK(spdt["status"] == "no_substitute");
    CHECK(night_b_note(spdt, "switch configuration differs: the original is SPDT, the "
                             "substitute SPST-NO"));
    // LMH6586 (16 x 16 crosspoint) -> DG406 (16:1 mux): same channel count (r33 35865730).
    const json xpt = night_b_one(night_b_spec("LMH6586"), night_b_spec("DG406"));
    CHECK(xpt["status"] == "no_substitute");
    CHECK(night_b_note(xpt, "multiplexer configuration differs: the original is "
                            "Crosspoint/exchange, the substitute 16:1"));
    // ADG1402 (SPST-NC, 1) -> TMUX9832 (SPST-NO, 32) and MAX4742 (SPST-NC, 2) -> TMUX9832
    // (r32 35342085, r33 35761001).
    CHECK(night_b_one(night_b_spec("ADG1402"), night_b_spec("TMUX9832"))["status"] ==
          "no_substitute");
    CHECK(night_b_one(night_b_spec("MAX4742"), night_b_spec("TMUX9832"))["status"] ==
          "no_substitute");
}

TEST_CASE("qarlos r32-33: the same configuration passes, a one-sided one is unverified",
          "[qarlos-night-b][qarlos-night-b-config]") {
    const json orig = night_b_spec("ADG788");
    json same = orig;
    same["mpn"] = "ADG788-TWIN";
    same["lifecycle_status"] = "production";  // a synthetic MPN in no TAS record
    const json ok = night_b_one(orig, same);
    CHECK(ok["status"] != "no_substitute");
    CHECK(night_b_verdict(ok, "configuration") == "pass");
    // The substitute's record states no configuration: UNVERIFIED with a note, never drop_in.
    json open = same;
    open["configuration"] = nullptr;
    const json u = night_b_one(orig, open);
    CHECK(u["status"] != "no_substitute");
    CHECK(night_b_verdict(u, "configuration") == "unverified");
    CHECK(u["grade"] != "drop_in");
    CHECK(night_b_note(u, "the original's switch configuration is SPDT; the substitute's record "
                          "states none"));
    // Not a switch or a multiplexer: the configuration is not compared.
    const json opa = {{"mpn", "OPA-A"}, {"subtype", "operationalAmplifier"}, {"channels", 1.0},
                      {"configuration", "SPDT"}};
    const json opb = {{"mpn", "OPA-B"}, {"lifecycle_status", "production"},
                      {"subtype", "operationalAmplifier"}, {"channels", 1.0}};
    CHECK(night_b_verdict(night_b_one(opa, opb), "configuration").empty());
}

namespace {
// Every analog field web/src/crossref.js projects (the analog family's spec()), for the owner's
// 2026-11-21 rules B1 / B2 / B4: NaN and "" read as null; the isolator counts are raw (0 stated).
json night_b_full_spec(const kelvin::AnalogRow& r) {
    auto num = [](double v) { return std::isnan(v) ? json(nullptr) : json(v); };
    auto pos = [](double v) { return std::isnan(v) || !(v > 0) ? json(nullptr) : json(v); };
    auto txt = [](const std::string& s) { return s.empty() ? json(nullptr) : json(s); };
    return {{"mpn", r.mpn},
            {"manufacturer", r.manufacturer},
            {"lifecycle_status", txt(r.status)},
            {"subtype", txt(r.device_type)},
            {"channels", pos(r.channels)},
            {"supply_min_V", pos(r.vsupply_min)},
            {"supply_max_V", pos(r.vsupply_max)},
            {"gbw", pos(r.gain_bandwidth)},
            {"slew_rate", pos(r.slew_rate)},
            {"input_offset_voltage", num(r.input_offset_voltage)},
            {"input_bias_current", num(r.input_bias_current)},
            {"cmrr_db", pos(r.cmrr)},
            {"resolution", pos(r.resolution)},
            {"sample_rate", pos(r.sample_rate)},
            {"offset_drift", pos(r.offset_drift)},
            {"quiescent_current", pos(r.quiescent_current)},
            {"propagation_delay", pos(r.propagation_delay)},
            {"on_resistance", pos(r.on_resistance)},
            {"rail_to_rail_input", txt(r.rail_to_rail_input)},
            {"rail_to_rail_output", txt(r.rail_to_rail_output)},
            {"output_stage", txt(r.output_stage)},
            {"architecture", txt(r.architecture)},
            {"configuration", txt(r.configuration)},
            {"forward_channels", num(r.forward_channels)},
            {"reverse_channels", num(r.reverse_channels)}};
}
json night_b_full(const std::string& mpn) { return night_b_full_spec(night_b_row(mpn)); }
}  // namespace

TEST_CASE("owner B4: a digital isolator's forward / reverse channels are identities",
          "[qarlos-night-b][qarlos-night-b-isolator]") {
    // The 48 isolator records state forwardChannels / reverseChannels and never
    // numberOfChannels: each count is kept as stated (0 included), the total is their sum.
    const auto d541 = night_b_row("DCL541H01");
    CHECK(d541.forward_channels == 3.0);
    CHECK(d541.reverse_channels == 1.0);
    CHECK(d541.channels == 4.0);
    const auto d320 = night_b_row("DCL320C00");
    CHECK(d320.forward_channels == 2.0);
    CHECK(d320.reverse_channels == 0.0);
    CHECK(d320.channels == 2.0);
    // Not an isolator: neither count is set.
    CHECK(std::isnan(night_b_row("OP262").forward_channels));
    CHECK(std::isnan(night_b_row("OP262").reverse_channels));

    // DCL541H01 (3 forward / 1 reverse) -> DCL542H01 (2 / 2): 4 channels each, a different
    // split, so a different pinout and direction: rejected, on the forward count first.
    const json x = night_b_one(night_b_full("DCL541H01"), night_b_full("DCL542H01"));
    CHECK(x["status"] == "no_substitute");
    CHECK(night_b_verdict(x, "forward_channels") == "fail");
    // DCL540H01 (4 / 0) -> DCL540L01 (4 / 0): both counts pass.
    const json same = night_b_one(night_b_full("DCL540H01"), night_b_full("DCL540L01"));
    CHECK(same["status"] != "no_substitute");
    CHECK(night_b_verdict(same, "forward_channels") == "pass");
    CHECK(night_b_verdict(same, "reverse_channels") == "pass");
    CHECK(night_b_verdict(same, "channels") == "pass");
}

TEST_CASE("owner B2: an analog supply range stated by one record only caps at minor_review",
          "[qarlos-night-b][qarlos-night-b-primary]") {
    // Qarlos r35 seed 36808291: LTC2946 (2.7-100 V) -> ADS1014L (no supply on record) was
    // drop_in. The resolution passes, so this is the supply rule, not B1.
    const json c = night_b_one(night_b_full("LTC2946"), night_b_full("ADS1014L"));
    CHECK(c["status"] != "no_substitute");
    CHECK(night_b_verdict(c, "resolution") == "pass");
    CHECK(night_b_verdict(c, "supply_max_V") == "unverified");
    CHECK(c["grade"] == "minor_review");
    CHECK(night_b_note(c, "supply range stated by one record only: the original's record states "
                          "2.7-100 V, the substitute's none"));
    // The other direction: the substitute states it, the original does not.
    const json r = night_b_one(night_b_full("ADS1014L"), night_b_full("LTC2946"));
    CHECK(r["grade"] == "minor_review");
    CHECK(night_b_note(r, "supply range stated by one record only: the original's record states "
                          "none, the substitute's 2.7-100 V"));
}

TEST_CASE("owner B2: an op-amp's rail-to-rail input stated by one record only caps",
          "[qarlos-night-b][qarlos-night-b-primary]") {
    // Qarlos r36 seed 37227207: TS462CPT (railToRailInput false) -> OP262 (not stated) was a
    // clean drop_in.
    const json c = night_b_one(night_b_full("TS462CPT"), night_b_full("OP262"));
    CHECK(c["status"] != "no_substitute");
    CHECK(night_b_verdict(c, "rail_to_rail_input") == "unverified");
    CHECK(c["grade"] != "drop_in");
    CHECK(night_b_note(c, "rail-to-rail input stated by one record only: the original's record "
                          "states no, the substitute's none"));
}

TEST_CASE("owner B2: a switch's on-resistance stated by one record only caps",
          "[qarlos-night-b][qarlos-night-b-primary]") {
    json orig = night_b_full("ADG788");
    orig["on_resistance"] = 2.5;  // synthetic pair, so the rule is the only difference
    json twin = orig;
    twin["mpn"] = "ADG788-TWIN";
    twin["lifecycle_status"] = "production";
    const json ok = night_b_one(orig, twin);
    CHECK(ok["grade"] == "drop_in");
    twin["on_resistance"] = nullptr;
    const json c = night_b_one(orig, twin);
    CHECK(night_b_verdict(c, "on_resistance") == "unverified");
    CHECK(c["grade"] == "minor_review");
    CHECK(night_b_note(c, "on-resistance stated by one record only: the original's record states "
                          "2.5 ohm, the substitute's none"));
}

TEST_CASE("owner B1: an analog candidate with no electrical verified caps at minor_review",
          "[qarlos-night-b][qarlos-night-b-allunv]") {
    // Two op-amps that share only identities: the original states its GBW, the substitute its
    // slew rate. Both rows UNVERIFIED and neither a B2 primary axis.
    const json o = {{"mpn", "OPA-O"}, {"subtype", "operationalAmplifier"}, {"channels", 1.0},
                    {"lifecycle_status", "production"}, {"gbw", 1e6}};
    const json s = {{"mpn", "OPA-S"}, {"subtype", "operationalAmplifier"}, {"channels", 1.0},
                    {"lifecycle_status", "production"}, {"slew_rate", 1e6}};
    const json c = night_b_one(o, s);
    CHECK(c["status"] != "no_substitute");
    CHECK(c["grade"] == "minor_review");
    CHECK(night_b_note(c, "no electrical parameter could be compared"));
    // One established electrical lifts the cap.
    json s2 = s;
    s2["gbw"] = 1e6;
    const json ok = night_b_one(o, s2);
    CHECK(ok["grade"] == "drop_in");
    CHECK(!night_b_note(ok, "no electrical parameter could be compared"));
    // Neither record states any electrical beyond the identities: capped too.
    json bare_o = o, bare_s = s;
    bare_o.erase("gbw");
    bare_s.erase("slew_rate");
    const json b = night_b_one(bare_o, bare_s);
    CHECK(b["grade"] == "minor_review");
    CHECK(night_b_note(b, "neither record states an electrical parameter beyond the identities"));
}

namespace {
// The full projection plus the gain range web/src/crossref.js passes since B5 (gain_min /
// gain_max, NaN -> null).
json night_b_gain_spec(const std::string& mpn) {
    const auto r = night_b_row(mpn);
    json s = night_b_full_spec(r);
    auto pos = [](double v) { return std::isnan(v) || !(v > 0) ? json(nullptr) : json(v); };
    s["gain_min"] = pos(r.minimum_gain);
    s["gain_max"] = pos(r.maximum_gain);
    return s;
}
}  // namespace

TEST_CASE("owner B5: an instrumentation amplifier's gain range must be covered",
          "[qarlos-night-b][qarlos-night-b-gain]") {
    // The rows carry the stated range (AAS minimumGain / maximumGain).
    CHECK(night_b_row("INA126").minimum_gain == 5.0);
    CHECK(night_b_row("INA126").maximum_gain == 10000.0);
    CHECK(night_b_row("AD627").maximum_gain == 1000.0);
    CHECK(night_b_row("LT1101").minimum_gain == 10.0);
    CHECK(std::isnan(night_b_row("OP262").minimum_gain));

    // INA126 (5-10000 V/V) -> AD627 (5-1000): the maximum falls short, a FAIL.
    const json ad = night_b_one(night_b_gain_spec("INA126"), night_b_gain_spec("AD627"));
    CHECK(night_b_verdict(ad, "gain_min") == "pass");
    CHECK(night_b_verdict(ad, "gain_max") == "fail");
    CHECK(ad["grade"] == "major_review");
    CHECK(night_b_note(ad, "gain range not covered: the original is specified over 5-10000 V/V, "
                           "the substitute over 5-1000 V/V"));
    // INA126 -> LT1101 (10-100): short at both ends.
    const json lt = night_b_one(night_b_gain_spec("INA126"), night_b_gain_spec("LT1101"));
    CHECK(night_b_verdict(lt, "gain_min") == "fail");
    CHECK(night_b_verdict(lt, "gain_max") == "fail");
    CHECK(lt["grade"] == "major_review");
    // The other way round AD627 (5-1000) -> INA126 (5-10000) covers it: both rows pass.
    const json up = night_b_one(night_b_gain_spec("AD627"), night_b_gain_spec("INA126"));
    CHECK(night_b_verdict(up, "gain_min") == "pass");
    CHECK(night_b_verdict(up, "gain_max") == "pass");
    CHECK(!night_b_note(up, "gain range not covered"));
}

TEST_CASE("owner B5: a gain range stated by one record only caps at minor_review",
          "[qarlos-night-b][qarlos-night-b-gain]") {
    json orig = night_b_gain_spec("INA126");
    json twin = orig;
    twin["mpn"] = "INA126-TWIN";  // a synthetic MPN in no TAS record
    twin["lifecycle_status"] = "production";
    const json ok = night_b_one(orig, twin);
    CHECK(ok["grade"] == "drop_in");
    twin["gain_min"] = nullptr;
    twin["gain_max"] = nullptr;
    const json c = night_b_one(orig, twin);
    CHECK(night_b_verdict(c, "gain_max") == "unverified");
    CHECK(c["grade"] == "minor_review");
    CHECK(night_b_note(c, "gain range stated by one record only: the original's record states "
                          "5-10000 V/V, the substitute's none"));
}
