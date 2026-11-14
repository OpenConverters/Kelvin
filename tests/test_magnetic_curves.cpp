// Magnetic curves compared under the owner's curve rule (2026-11-14: "make sure that Kelvin
// compares ALL existing graphs"). Every record used here is a REAL TAS magnetics.ndjson line
// (data-524a699, distributorsInfo dropped) kept in tests/fixtures/magnetic_curves.ndjson, so
// each test runs the extractor on what the catalogue states, then the ranker on the spec
// block web/src/crossref.js builds from the browse row.
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "Views.hpp"

using Catch::Approx;
using nlohmann::json;

namespace {
// The real record whose manufacturerInfo.reference is `mpn`; throws when the fixture lacks it.
json real_record(const std::string& mpn) {
    std::ifstream in(std::string(KELVIN_TEST_DIR) + "/fixtures/magnetic_curves.ndjson");
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        json j = json::parse(line);
        if (j["magnetic"]["manufacturerInfo"].value("reference", "") == mpn) return j;
    }
    throw std::runtime_error("magnetic_curves.ndjson has no record " + mpn);
}
kelvin::MagneticRow row_of(const std::string& mpn) {
    auto r = kelvin::extract_magnetic(real_record(mpn));
    if (!r) throw std::runtime_error("extract_magnetic refused " + mpn);
    return *r;
}
}  // namespace

// 1. A common-mode choke's impedancePoints hold its common-mode AND its differential-mode
// curve in one array, each point tagged by `winding`. The extractor ignored the tag and read
// one merged curve: for 744290130 (Würth WE-CMBNC) the largest sample of the merged array is
// the DM point at 1 GHz (214 ohm, the last sample, so no bracketed peak at all), and its real
// common-mode peak, 132.81 ohm at 870.96 MHz, was lost. 185 records had a DM point as the
// merged maximum (survey of data-524a699).
TEST_CASE("curves: a CMC's impedance readings are taken per winding mode",
          "[magcurves][views][cmc]") {
    const auto r = row_of("744290130");
    // common mode: bracketed peak at 870.96 MHz; the sample nearest 100 MHz is 95.499 MHz
    CHECK(r.impedance_peak == Approx(132.81));
    CHECK(r.impedance_peak_freq == Approx(870.96e6));
    CHECK(r.impedance_100mhz == Approx(52.61));
    // differential mode: 30.067 ohm at 95.499 MHz; its maximum is its last sample (1 GHz),
    // which is where the measurement stopped, not a peak
    CHECK(r.dm_impedance_100mhz == Approx(30.067));
    CHECK(std::isnan(r.dm_impedance_peak));
    CHECK(std::isnan(r.dm_impedance_peak_freq));
}
