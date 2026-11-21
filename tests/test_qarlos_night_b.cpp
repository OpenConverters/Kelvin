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

namespace {
// The projection web/src/crossref.js passes since B7: the isolation rating with its duration,
// and the operating range for an isolator only.
json night_b_iso_spec(const std::string& mpn) {
    const auto r = night_b_row(mpn);
    json s = night_b_gain_spec(mpn);
    auto pos = [](double v) { return std::isnan(v) || !(v > 0) ? json(nullptr) : json(v); };
    auto num = [](double v) { return std::isnan(v) ? json(nullptr) : json(v); };
    const bool iso = r.device_type == "digitalIsolator";
    s["isolation_voltage_V"] = pos(r.isolation_voltage_rms);
    s["isolation_duration_s"] = pos(r.isolation_duration_s);
    s["temp_min_C"] = iso ? num(r.temp_min_c) : json(nullptr);
    s["temp_max_C"] = iso ? num(r.temp_max_c) : json(nullptr);
    return s;
}
}  // namespace

TEST_CASE("owner B7: a digital isolator's isolation voltage and operating range are compared",
          "[qarlos-night-b][qarlos-night-b-isoelec]") {
    // The rows carry the stated rating, its duration and the range.
    const auto h = night_b_row("DCL540H01");
    CHECK(h.isolation_voltage_rms == 5000.0);
    CHECK(h.isolation_duration_s == 60.0);
    CHECK(h.temp_min_c == -40.0);
    CHECK(h.temp_max_c == 110.0);
    CHECK(std::isnan(night_b_row("DCM340H01").temp_max_c));
    CHECK(night_b_row("DCL340H0B").isolation_voltage_rms == 3000.0);

    // A same-rated twin: both electricals verified, so the B1 cap no longer applies.
    json orig = night_b_iso_spec("DCL540H01");
    json twin = orig;
    twin["mpn"] = "DCL540H01-TWIN";  // a synthetic MPN in no TAS record
    twin["lifecycle_status"] = "production";
    const json ok = night_b_one(orig, twin);
    CHECK(night_b_verdict(ok, "isolation_voltage_V") == "pass");
    CHECK(night_b_verdict(ok, "temp_max_C") == "pass");
    CHECK(ok["grade"] == "drop_in");
    CHECK(!night_b_note(ok, "no electrical parameter could be compared"));

    // DCL540H01A (-40..125 degC) -> DCL540H01 (-40..110): the hot end is not covered.
    const json hot = night_b_one(night_b_iso_spec("DCL540H01A"), night_b_iso_spec("DCL540H01"));
    CHECK(night_b_verdict(hot, "temp_max_C") == "fail");
    CHECK(hot["grade"] == "major_review");

    // DCL540H01 (5000 Vrms) -> DCL340H0B (3000 Vrms, also 4 / 0): a lower isolation rating.
    const json low = night_b_one(orig, night_b_iso_spec("DCL340H0B"));
    CHECK(night_b_verdict(low, "isolation_voltage_V") == "fail");
    CHECK(low["grade"] == "major_review");
    CHECK(night_b_note(low, "lower withstand isolation voltage: the substitute is rated 3000 Vrms "
                            "against the original's 5000 Vrms"));

    // DCL540H01 -> DCM340H01 (no range on record): the range is unverified, the isolation
    // verified, so B1 does not cap.
    const json dcm = night_b_one(orig, night_b_iso_spec("DCM340H01"));
    CHECK(night_b_verdict(dcm, "temp_max_C") == "unverified");
    CHECK(night_b_verdict(dcm, "isolation_voltage_V") == "pass");
    CHECK(!night_b_note(dcm, "no electrical parameter could be compared"));
    CHECK(night_b_note(dcm, "the substitute's record states no maximum operating temperature"));
}

TEST_CASE("owner B7: B1 caps an isolator only when its electricals are unverified",
          "[qarlos-night-b][qarlos-night-b-isoelec]") {
    json orig = night_b_iso_spec("DCL540H01");
    json twin = orig;
    twin["mpn"] = "DCL540H01-TWIN";
    twin["lifecycle_status"] = "production";
    twin["isolation_voltage_V"] = nullptr;
    twin["temp_min_C"] = nullptr;
    twin["temp_max_C"] = nullptr;
    const json c = night_b_one(orig, twin);
    CHECK(night_b_verdict(c, "isolation_voltage_V") == "unverified");
    CHECK(c["grade"] == "minor_review");
    CHECK(night_b_note(c, "no electrical parameter could be compared"));
    // A rating tested for a different duration is not the same figure: UNVERIFIED, noted.
    json dur = orig;
    dur["mpn"] = "DCL540H01-1S";
    dur["lifecycle_status"] = "production";
    dur["isolation_duration_s"] = 1.0;
    const json d = night_b_one(orig, dur);
    CHECK(night_b_verdict(d, "isolation_voltage_V") == "unverified");
    CHECK(night_b_note(d, "withstand isolation voltages tested for different durations"));
    // Not an isolator original: the isolation row is not compared. Its operating range is
    // (owner 2026-11-21, B11 / T1: expectation rewritten from "no temp_max_C row" to the new
    // behaviour, a 85 degC substitute does not cover the original's 125 degC).
    const json opa = {{"mpn", "OPA-A"}, {"subtype", "operationalAmplifier"}, {"channels", 1.0},
                      {"temp_max_C", 125.0}, {"gbw", 1e6}};
    const json opb = {{"mpn", "OPA-B"}, {"lifecycle_status", "production"},
                      {"subtype", "operationalAmplifier"}, {"channels", 1.0},
                      {"temp_max_C", 85.0}, {"gbw", 1e6}};
    CHECK(night_b_verdict(night_b_one(opa, opb), "temp_max_C") == "fail");
}

TEST_CASE("owner B10: an isolator's isolation voltage stated by one record only caps",
          "[qarlos-night-b][qarlos-night-b-isoprimary]") {
    json orig = night_b_iso_spec("DCL540H01");
    json twin = orig;
    twin["mpn"] = "DCL540H01-NOISO";  // a synthetic MPN in no TAS record
    twin["lifecycle_status"] = "production";
    twin["isolation_voltage_V"] = nullptr;
    // The range is verified (B1 does not cap), so only the isolation primary axis caps.
    const json c = night_b_one(orig, twin);
    CHECK(night_b_verdict(c, "isolation_voltage_V") == "unverified");
    CHECK(night_b_verdict(c, "temp_max_C") == "pass");
    CHECK(!night_b_note(c, "no electrical parameter could be compared"));
    CHECK(c["grade"] == "minor_review");
    CHECK(night_b_note(c, "isolation voltage stated by one record only: the original's record "
                          "states 5000 Vrms, the substitute's none"));
    // The other way round (only the substitute states it): capped as well.
    const json r = night_b_one(twin, orig);
    CHECK(night_b_verdict(r, "isolation_voltage_V") == "unverified");
    CHECK(r["grade"] == "minor_review");
    // Not an isolator original: no isolation row, no cap from it.
    json opa = night_b_full("OP262");
    json opb = opa;
    opb["mpn"] = "OP262-TWIN";
    opb["lifecycle_status"] = "production";
    opb["isolation_voltage_V"] = 2500.0;
    CHECK(night_b_verdict(night_b_one(opa, opb), "isolation_voltage_V").empty());
}

namespace {
// The projection web/src/crossref.js passes since B11: the operating range for every analog
// subtype (raw: a cold limit is negative), on top of the full projection.
json night_b_temp_spec(const std::string& mpn) {
    const auto r = night_b_row(mpn);
    json s = night_b_full_spec(r);
    auto num = [](double v) { return std::isnan(v) ? json(nullptr) : json(v); };
    s["temp_min_C"] = num(r.temp_min_c);
    s["temp_max_C"] = num(r.temp_max_c);
    return s;
}
}  // namespace

TEST_CASE("owner B11: the operating range is compared for every analog subtype",
          "[qarlos-night-b][qarlos-night-b-temp]") {
    // OP262 (-55..125 degC) -> TS462CPT (-20..70): neither end covered.
    const json op = night_b_one(night_b_temp_spec("OP262"), night_b_temp_spec("TS462CPT"));
    CHECK(night_b_verdict(op, "temp_min_C") == "fail");
    CHECK(night_b_verdict(op, "temp_max_C") == "fail");
    CHECK(night_b_note(op, "rated only down to -20 degC vs the original's -55 degC"));
    CHECK(op["grade"] == "major_review");
    // A switch: ADG1402 (-40..125 degC) against a twin rated only to 85 degC (ADG788's and
    // MAX4742's hot end; neither is ADG1402's configuration): the hot end is not covered.
    json sw85 = night_b_temp_spec("ADG1402");
    sw85["mpn"] = "ADG1402-85C";  // a synthetic MPN in no TAS record
    sw85["lifecycle_status"] = "production";
    sw85["temp_max_C"] = 85.0;
    const json sw = night_b_one(night_b_temp_spec("ADG1402"), sw85);
    CHECK(night_b_verdict(sw, "temp_min_C") == "pass");
    CHECK(night_b_verdict(sw, "temp_max_C") == "fail");
    // A same-range twin passes both ends.
    json orig = night_b_temp_spec("OP262");
    json twin = orig;
    twin["mpn"] = "OP262-TWIN";  // a synthetic MPN in no TAS record
    twin["lifecycle_status"] = "production";
    const json ok = night_b_one(orig, twin);
    CHECK(night_b_verdict(ok, "temp_min_C") == "pass");
    CHECK(night_b_verdict(ok, "temp_max_C") == "pass");
    CHECK(ok["grade"] == "drop_in");
    // One-sided: UNVERIFIED with the operating-range note, no cap.
    twin["temp_min_C"] = nullptr;
    twin["temp_max_C"] = nullptr;
    const json one = night_b_one(orig, twin);
    CHECK(night_b_verdict(one, "temp_max_C") == "unverified");
    CHECK(night_b_note(one, "the substitute's record states no maximum operating temperature"));
    CHECK(one["grade"] == "drop_in");
}

TEST_CASE("owner B11: a verified operating range does not lift B1 outside the isolators",
          "[qarlos-night-b][qarlos-night-b-temp]") {
    const json o = {{"mpn", "OPA-A"}, {"subtype", "operationalAmplifier"}, {"channels", 1.0},
                    {"temp_min_C", -40.0}, {"temp_max_C", 125.0}, {"gbw", 1e6}};
    const json s = {{"mpn", "OPA-B"}, {"lifecycle_status", "production"},
                    {"subtype", "operationalAmplifier"}, {"channels", 1.0},
                    {"temp_min_C", -40.0}, {"temp_max_C", 125.0}, {"slew_rate", 1e6}};
    const json c = night_b_one(o, s);
    CHECK(night_b_verdict(c, "temp_max_C") == "pass");
    CHECK(c["grade"] == "minor_review");
    CHECK(night_b_note(c, "no electrical parameter could be compared"));
}

namespace {
// The projection web/src/crossref.js passes since B12: the difference amplifier's fixed gain.
json night_b_dgain_spec(const std::string& mpn) {
    const auto r = night_b_row(mpn);
    json s = night_b_full_spec(r);
    s["fixed_gain"] = std::isnan(r.fixed_gain) ? json(nullptr) : json(r.fixed_gain);
    return s;
}
}  // namespace

TEST_CASE("owner B12: a difference amplifier's fixed gain is an exact identity",
          "[qarlos-night-b][qarlos-night-b-dgain]") {
    // The rows carry AAS differenceAmplifier electrical.gain, for that subtype only.
    CHECK(night_b_row("AD8276").fixed_gain == 1.0);
    CHECK(night_b_row("AD8275").fixed_gain == 0.2);
    CHECK(std::isnan(night_b_row("INA126").fixed_gain));
    // AD8276 (G = 1) -> AD8275 (G = 0.2): a different gain rejects, with a note naming both.
    const json r = night_b_one(night_b_dgain_spec("AD8276"), night_b_dgain_spec("AD8275"));
    CHECK(r["status"] == "no_substitute");
    CHECK(night_b_note(r, "fixed gain differs: the original is G = 1 V/V, the substitute "
                          "G = 0.2 V/V"));
    // AD8276 -> AD8479 (both G = 1): the gain passes.
    const json same = night_b_one(night_b_dgain_spec("AD8276"), night_b_dgain_spec("AD8479"));
    CHECK(night_b_verdict(same, "fixed_gain") == "pass");
}

TEST_CASE("owner B12: a fixed gain stated by one record only caps at minor_review",
          "[qarlos-night-b][qarlos-night-b-dgain]") {
    json orig = night_b_dgain_spec("AD8276");
    json twin = orig;
    twin["mpn"] = "AD8276-TWIN";  // a synthetic MPN in no TAS record
    twin["lifecycle_status"] = "production";
    const json ok = night_b_one(orig, twin);
    CHECK(night_b_verdict(ok, "fixed_gain") == "pass");
    CHECK(ok["grade"] == "drop_in");
    twin["fixed_gain"] = nullptr;
    const json c = night_b_one(orig, twin);
    CHECK(night_b_verdict(c, "fixed_gain") == "unverified");
    CHECK(c["status"] != "no_substitute");
    CHECK(c["grade"] == "minor_review");
    CHECK(night_b_note(c, "fixed gain stated by one record only: the original's record states "
                          "G = 1 V/V, the substitute's none"));
    // Not a difference-amplifier original: no fixed-gain row.
    json ina = night_b_gain_spec("INA126");
    json ina2 = ina;
    ina2["mpn"] = "INA126-G";
    ina2["lifecycle_status"] = "production";
    ina2["fixed_gain"] = 5.0;
    CHECK(night_b_verdict(night_b_one(ina, ina2), "fixed_gain").empty());
}
