// Secondary-parameter gate engine — ported from Heaviside param_check.py.
// evaluate_params(category, original, substitute) runs each per-category
// ParamSpec (ESR/ripple/dielectric/Rds(on)/Qg/Vf/Isat/DCR/…) and returns a
// per-parameter verdict (pass/warn/fail/unverified). The verdict semantics
// mirror the Python comparators exactly so the shared golden corpus
// (tests/golden/crossref_parity.json, "evaluate_params" section) reproduces
// across languages. Program-only, no I/O.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "CrossRefScore.hpp"  // PASS/WARN/FAIL/UNVERIFIED

namespace kelvin::crossref {

enum class Dir { Lower, Higher, ClassMatch, ExactMatch };

struct ParamSpec {
    std::string key;
    Dir dir;
    double tol_factor;
    std::optional<double> abs_tol;      // additive band (temps, dB) — overrides tol_factor
    bool magnitude = false;             // compare |value| (TCR, offset)
    bool exclude_missing_sub = false;   // missing substitute value -> FAIL (not UNVERIFIED)
    bool dielectric = false;            // class_rank = _DIELECTRIC_RANK
};

// _DIELECTRIC_RANK — insertion order preserved; first substring match wins.
inline const std::vector<std::pair<std::string, int>>& dielectric_rank() {
    static const std::vector<std::pair<std::string, int>> R = {
        {"c0g", 5}, {"np0", 5}, {"cog", 5}, {"x8r", 4}, {"x8l", 4}, {"x7r", 3}, {"x7s", 3},
        {"x7t", 3}, {"x6s", 2}, {"x6t", 2}, {"x5r", 1}, {"y5v", 0}, {"z5u", 0},
        {"ceramicclass1", 5}, {"ceramicclass2", 2}, {"ceramicclass3", 0}};
    return R;
}

inline std::string norm_class(const std::string& v) {
    std::string out;
    for (char c : v) {
        if (c == '-' || c == ' ') continue;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

inline std::optional<int> class_rank_of(const std::string& normv) {
    for (const auto& [k, val] : dielectric_rank())
        if (normv.find(k) != std::string::npos) return val;
    return std::nullopt;
}

namespace detail {
inline bool present(const nlohmann::json& p, const std::string& k) {
    auto it = p.find(k);
    return it != p.end() && !it->is_null();
}
inline std::optional<double> jnum(const nlohmann::json& p, const std::string& k) {
    auto it = p.find(k);
    if (it == p.end() || it->is_null() || it->is_boolean() || !it->is_number()) return std::nullopt;
    return it->get<double>();
}
inline std::string jstr(const nlohmann::json& p, const std::string& k) {
    auto it = p.find(k);
    if (it == p.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}
}  // namespace detail

// Mirrors _compare_numeric: LOWER/HIGHER with abs_tol (additive) or tol_factor.
inline const char* compare_numeric(const ParamSpec& spec, std::optional<double> o,
                                   std::optional<double> s) {
    if (!o && !s) return UNVERIFIED;
    if (!s) return spec.exclude_missing_sub ? FAIL : UNVERIFIED;
    if (!o) return UNVERIFIED;
    double ov = *o, sv = *s;
    if (spec.dir == Dir::Lower) {
        if (sv <= ov) return PASS;
        double ceiling = spec.abs_tol ? ov + *spec.abs_tol : ov * spec.tol_factor;
        return sv <= ceiling ? WARN : FAIL;
    }
    if (sv >= ov) return PASS;  // Higher
    double floor = spec.abs_tol ? ov - *spec.abs_tol : ov * spec.tol_factor;
    return sv >= floor ? WARN : FAIL;
}

// Mirrors _compare_class: categorical equal-or-better via the dielectric rank.
inline const char* compare_class(const std::string& o_raw, const std::string& s_raw) {
    std::string on = norm_class(o_raw), sn = norm_class(s_raw);
    auto o_rank = class_rank_of(on), s_rank = class_rank_of(sn);
    if (!o_rank && !s_rank) {
        if (!on.empty() && !sn.empty() && on != sn) return WARN;
        if (!on.empty() && !sn.empty()) return PASS;
        return UNVERIFIED;
    }
    if (!s_rank) return UNVERIFIED;
    if (!o_rank) return UNVERIFIED;
    return (*s_rank >= *o_rank) ? PASS : FAIL;
}

inline const char* compare_param(const ParamSpec& spec, const nlohmann::json& orig,
                                 const nlohmann::json& sub) {
    if (spec.dir == Dir::ClassMatch)
        return compare_class(detail::jstr(orig, spec.key), detail::jstr(sub, spec.key));
    // numeric (Lower/Higher); ExactMatch falls through to a strict numeric/string
    // path handled by the connector/analog specs (not in this table yet).
    auto o = detail::jnum(orig, spec.key), s = detail::jnum(sub, spec.key);
    if (spec.magnitude) {
        if (o) o = std::abs(*o);
        if (s) s = std::abs(*s);
    }
    return compare_numeric(spec, o, s);
}

// PARAM_SPECS for the numeric/class categories (connector/analog/timeBase — the
// EXACT_MATCH identity families — land with the mating-check port).
inline const std::vector<ParamSpec>& params_for(const std::string& category) {
    static const std::vector<ParamSpec> kEmpty;
    static const std::vector<ParamSpec> kCapacitor = {
        {"esr", Dir::Lower, 1.5, std::nullopt, false, true, false},
        {"ripple_current", Dir::Higher, 0.9, std::nullopt, false, true, false},
        {"technology", Dir::ClassMatch, 0.0, std::nullopt, false, false, true},
        {"dielectric_code", Dir::ClassMatch, 0.0, std::nullopt, false, false, true},
        {"temp_max_C", Dir::Higher, 0.0, 5.0, false, false, false},
        {"tolerance_pct", Dir::Lower, 2.0, std::nullopt, false, false, false},
    };
    static const std::vector<ParamSpec> kMosfet = {
        {"rds_on", Dir::Lower, 1.5, std::nullopt, false, false, false},
        {"qg", Dir::Lower, 2.0, std::nullopt, false, false, false},
        {"coss", Dir::Lower, 2.0, std::nullopt, false, false, false},
        {"vgs_threshold_max", Dir::Lower, 2.0, std::nullopt, false, false, false},
    };
    static const std::vector<ParamSpec> kDiode = {
        {"vf", Dir::Lower, 1.2, std::nullopt, false, false, false},
        {"qrr", Dir::Lower, 2.0, std::nullopt, false, false, false},
        {"trr", Dir::Lower, 2.0, std::nullopt, false, false, false},
    };
    static const std::vector<ParamSpec> kResistor = {
        {"power_rating", Dir::Higher, 0.9, std::nullopt, false, false, false},
        {"tolerance_pct", Dir::Lower, 2.0, std::nullopt, false, false, false},
        {"tcr", Dir::Lower, 2.0, std::nullopt, true, false, false},
    };
    static const std::vector<ParamSpec> kMagnetic = {
        {"saturation_current", Dir::Higher, 0.9, std::nullopt, false, true, false},
        {"dcr", Dir::Lower, 1.3, std::nullopt, false, false, false},
        {"rated_current", Dir::Higher, 0.9, std::nullopt, false, false, false},
    };
    static const std::vector<ParamSpec> kChipBead = {
        {"impedance_100mhz", Dir::Higher, 0.8, std::nullopt, false, false, false},
        {"srf", Dir::Higher, 0.8, std::nullopt, false, false, false},
        {"dcr", Dir::Lower, 1.3, std::nullopt, false, false, false},
        {"rated_current", Dir::Higher, 0.9, std::nullopt, false, false, false},
    };
    if (category == "capacitor") return kCapacitor;
    if (category == "mosfet") return kMosfet;
    if (category == "diode") return kDiode;
    if (category == "resistor") return kResistor;
    if (category == "magnetic") return kMagnetic;
    if (category == "chipBead") return kChipBead;
    return kEmpty;
}

// One {name, verdict} per parameter that has data on at least one side.
inline std::vector<std::pair<std::string, std::string>> evaluate_params(
    const std::string& category, const nlohmann::json& orig, const nlohmann::json& sub) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const ParamSpec& spec : params_for(category)) {
        if (!detail::present(orig, spec.key) && !detail::present(sub, spec.key)) continue;
        out.emplace_back(spec.key, compare_param(spec, orig, sub));
    }
    return out;
}

}  // namespace kelvin::crossref
