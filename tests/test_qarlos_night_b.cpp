// Qarlos rounds 32-33 (Kelvin 9416f0b, shards v25, TAS 33069cb): the analog cross-reference.
// The records in tests/fixtures/qarlos_night_b_analog.ndjson are the TAS 33069cb lines, verbatim.
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

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

