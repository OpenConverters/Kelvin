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

}  // namespace kelvin::crossref
