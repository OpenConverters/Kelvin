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
