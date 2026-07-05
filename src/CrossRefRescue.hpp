// Operating-point magnetic rescue — the FAE-winning capability ported from
// Heaviside's crossref_pipeline.py (_operating_point_magnetic_rescue + friends).
//
// When an in-kind (BOM-value) inductor can't carry a converter's operating
// current, SIZE the inductor from the CIRCUIT: compute the ripple-required
// inductance, then pick a real target-manufacturer part that covers the ripple
// requirement AND the operating current with margin, right-sized by footprint.
// Program-only and network-free (the emitted-MPN datasheet check is a host
// callback in the consumer, not here). Mirrors the Python exactly so the shared
// golden corpus (tests/golden/crossref_parity.json) reproduces across languages.
#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace kelvin::crossref {

using json = nlohmann::json;

// ── Ripple-derived inductance sizing (stress.py required_inductance) ──────────
// Minimum main-inductor inductance (H) to hold the peak-to-peak ripple within
// the spec's currentRippleRatio. Only single-power-inductor topologies (buck,
// boost) are sized; flyback/cuk/sepic use a coupled inductor/transformer, so a
// 2-terminal power inductor is the wrong part type -> nullopt (no wrong-type
// rescue). nullopt also when a required field (fsw, ripple, V/I) is missing.
inline std::optional<double> required_inductance(const std::string& topology, const json& spec,
                                                 size_t op_index = 0) {
    auto num = [](const json& o, const char* k) -> std::optional<double> {
        auto it = o.find(k);
        if (it == o.end() || it->is_null() || !it->is_number()) return std::nullopt;
        return it->get<double>();
    };
    auto ops = spec.find("operatingPoints");
    if (ops == spec.end() || !ops->is_array() || ops->size() <= op_index) return std::nullopt;
    const json& op = (*ops)[op_index];
    auto fsw = num(op, "switchingFrequency");
    if (!fsw || *fsw <= 0) return std::nullopt;
    auto ripple = num(spec, "currentRippleRatio");
    if (!ripple || *ripple <= 0) return std::nullopt;

    auto vbound = [&](const char* which) -> std::optional<double> {
        auto iv = spec.find("inputVoltage");
        if (iv == spec.end() || !iv->is_object()) return std::nullopt;
        return num(*iv, which);
    };
    auto first_of = [&](const char* key) -> std::optional<double> {
        auto a = op.find(key);
        if (a == op.end() || !a->is_array() || a->empty() || !(*a)[0].is_number()) return std::nullopt;
        return (*a)[0].get<double>();
    };
    std::optional<double> vout = first_of("outputVoltages"), iout = first_of("outputCurrents");
    if (!vout || !iout || *iout <= 0) return std::nullopt;

    std::string topo = topology;
    std::transform(topo.begin(), topo.end(), topo.begin(), [](unsigned char c) { return std::tolower(c); });

    if (topo == "buck") {
        auto vmax = vbound("maximum");
        if (!vmax || *vout >= *vmax) return std::nullopt;
        double di_l = *ripple * *iout;
        return *vout * (*vmax - *vout) / (*vmax * *fsw * di_l);
    }
    if (topo == "boost") {
        auto vmin = vbound("minimum"), vmax = vbound("maximum");
        if (!vmin || !vmax || *vout <= *vmax) return std::nullopt;
        // L(Vin) proportional to Vin^2*(Vout-Vin); size at the worst Vin across
        // [Vin_min, Vin_max] (endpoints + interior max 2*Vout/3 when in range).
        double v_interior = 2.0 * *vout / 3.0;
        std::set<double> cands{*vmin, *vmax};
        if (*vmin <= v_interior && v_interior <= *vmax) cands.insert(v_interior);
        double worst = 0.0;
        for (double v : cands) worst = std::max(worst, v * v * (*vout - v));
        return worst / (*ripple * *iout * *vout * *vout * *fsw);
    }
    return std::nullopt;
}

// ── Footprint metric with bad-data plausibility guard (_footprint_area_mm2) ───
// A flat SMD power inductor is wider than it is tall; a thin tall cylinder is a
// leaded choke. A power inductor's footprint also scales with its current, so a
// tiny area on a many-amp part is wrong dimension data. Returns +inf (sorts
// last) for unknown / implausibly small / tall-leaded / current-inconsistent
// records so the right-sizing sort can't be fooled by bad data.
inline constexpr double kMaxHeightToFootprint = 1.5;
inline constexpr double kMinPlausibleFootprintMm2 = 4.0;
inline constexpr double kMinFootprintMm2PerAmp = 3.0;

inline double footprint_area_mm2(const json& summary) {
    auto num = [](const json& o, const char* k) -> std::optional<double> {
        auto it = o.find(k);
        if (it == o.end() || it->is_null() || !it->is_number()) return std::nullopt;
        return it->get<double>();
    };
    json dims;
    if (auto it = summary.find("dimensions_mm"); it != summary.end() && it->is_object())
        dims = *it;
    else if (auto it2 = summary.find("dimensions"); it2 != summary.end() && it2->is_object())
        dims = *it2;
    else
        return INFINITY;
    auto length = num(dims, "length"), width = num(dims, "width"), height = num(dims, "height");
    if (!length || !width || *length <= 0 || *width <= 0) return INFINITY;
    double area = *length * *width;
    if (area < kMinPlausibleFootprintMm2) return INFINITY;
    if (height && *height > kMaxHeightToFootprint * std::max(*length, *width)) return INFINITY;
    auto current = num(summary, "saturation_current");
    if (!current) current = num(summary, "rated_current");
    if (current && *current > 0 && area < kMinFootprintMm2PerAmp * *current) return INFINITY;
    return area;
}

// ── Operating-point magnetic rescue selection (_operating_point_magnetic_rescue)
// Given the circuit's ripple-required inductance + operating currents and a
// candidate POOL (array of summary dicts — the caller supplies the target-
// manufacturer magnetics; catalogue access and the network MPN-existence check
// live in the consumer, not here), return the right-sized part: covers the
// ripple L (l_req <= L <= 4*l_req) AND the operating peak/RMS with margin
// (Isat >= 1.15*peak, IR >= 1.25*rms), then two-tier sorted — L close to the
// requirement first, then smallest footprint, then closest L, then least Isat
// over-margin. Returns nullopt when nothing qualifies.
inline constexpr double kLOversizeCap = 4.0;
inline constexpr double kIsatMargin = 1.15;
inline constexpr double kIrMargin = 1.25;

struct RescueResult {
    json summary;
    double inductance;
};

inline std::optional<RescueResult> operating_point_magnetic_rescue(
    double l_required, double i_peak, std::optional<double> i_rms, const json& candidates) {
    auto num = [](const json& o, const char* k) -> std::optional<double> {
        auto it = o.find(k);
        if (it == o.end() || it->is_null() || !it->is_number()) return std::nullopt;
        return it->get<double>();
    };
    if (!(l_required > 0) || !(i_peak > 0)) return std::nullopt;
    // key = (l_tier, footprint_area, L/l_req, isat); sort ascending, first wins.
    using Key = std::tuple<int, double, double, double>;
    std::vector<std::pair<Key, const json*>> ranked;
    if (!candidates.is_array()) return std::nullopt;
    for (const json& s : candidates) {
        auto L = num(s, "inductance");
        if (!L) L = num(s, "value_si");
        auto isat = num(s, "saturation_current");
        if (!L || *L < l_required || *L > kLOversizeCap * l_required) continue;
        if (!isat || *isat < kIsatMargin * i_peak) continue;
        if (i_rms && *i_rms > 0) {
            auto irms = num(s, "rated_current");
            if (!irms || *irms < kIrMargin * *i_rms) continue;
        }
        int l_tier = (*L <= 1.5 * l_required) ? 0 : 1;
        ranked.emplace_back(Key{l_tier, footprint_area_mm2(s), *L / l_required, *isat}, &s);
    }
    if (ranked.empty()) return std::nullopt;
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    const json& best = *ranked.front().second;
    double L = num(best, "inductance").value_or(num(best, "value_si").value_or(0.0));
    return RescueResult{best, L};
}

}  // namespace kelvin::crossref
