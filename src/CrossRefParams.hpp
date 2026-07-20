// Secondary-parameter gate engine — ported from Heaviside param_check.py.
// evaluate_params(category, original, substitute) runs each per-category
// ParamSpec (ESR/ripple/dielectric/Rds(on)/Isat/DCR/family/pitch/frequency/…)
// and returns a per-parameter verdict (pass/warn/fail/unverified). The verdict
// semantics mirror the Python comparators exactly so the shared golden corpus
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

using ClassRank = std::vector<std::pair<std::string, int>>;

struct ParamSpec {
    std::string key;
    Dir dir;
    double tol_factor;
    std::optional<double> abs_tol;     // additive band (temps, dB) — overrides tol_factor
    bool magnitude = false;            // compare |value| (TCR, offset, bias)
    bool exclude_missing_sub = false;  // missing substitute value -> FAIL (not UNVERIFIED)
    const ClassRank* rank = nullptr;   // class-equivalence map (dielectric / family / y-n)
};

// ── Class-rank maps (mirror param_check.py) ──────────────────────────────────
// _DIELECTRIC_RANK — insertion order preserved (CLASS_MATCH: first SUBSTRING wins).
inline const ClassRank& dielectric_rank() {
    static const ClassRank R = {
        {"c0g", 5}, {"np0", 5}, {"cog", 5}, {"x8r", 4}, {"x8l", 4}, {"x7r", 3}, {"x7s", 3},
        {"x7t", 3}, {"x6s", 2}, {"x6t", 2}, {"x5r", 1}, {"y5v", 0}, {"z5u", 0},
        {"ceramicclass1", 5}, {"ceramicclass2", 2}, {"ceramicclass3", 0}};
    return R;
}
inline const ClassRank& connector_family_rank() {  // EXACT_MATCH equivalence group
    static const ClassRank R = {{"pinheadersocket", 1}, {"boardtoboard", 1}};
    return R;
}
inline const ClassRank& yes_no_rank() {  // CLASS_MATCH (rail-to-rail)
    static const ClassRank R = {{"yes", 1}, {"no", 0}};
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
// CLASS_MATCH: first rank key that is a SUBSTRING of the normalized value.
inline std::optional<int> rank_substring(const std::string& normv, const ClassRank& r) {
    for (const auto& [k, v] : r)
        if (normv.find(k) != std::string::npos) return v;
    return std::nullopt;
}
// EXACT_MATCH equivalence: exact-key lookup, default = the string itself. Returns
// a comparable group id ("#N" for a mapped rank, else the raw normalized string).
inline std::string exact_group(const std::string& normv, const ClassRank& r) {
    for (const auto& [k, v] : r)
        if (k == normv) return "#" + std::to_string(v);
    return normv;
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

// Mirrors _compare_class: categorical equal-or-better via a SUBSTRING rank map.
inline const char* compare_class(const ParamSpec& spec, const std::string& o_raw,
                                 const std::string& s_raw) {
    const ClassRank& r = spec.rank ? *spec.rank : dielectric_rank();
    std::string on = norm_class(o_raw), sn = norm_class(s_raw);
    auto o_rank = rank_substring(on, r), s_rank = rank_substring(sn, r);
    if (!o_rank && !s_rank) {
        if (!on.empty() && !sn.empty() && on != sn) return WARN;
        if (!on.empty() && !sn.empty()) return PASS;
        return UNVERIFIED;
    }
    if (!s_rank) return UNVERIFIED;
    if (!o_rank) return UNVERIFIED;
    return (*s_rank >= *o_rank) ? PASS : FAIL;
}

// Mirrors _compare_exact: identity — equivalence groups, numeric tol, or string.
inline const char* compare_exact(const ParamSpec& spec, const nlohmann::json& orig,
                                 const nlohmann::json& sub) {
    bool o_pres = detail::present(orig, spec.key), s_pres = detail::present(sub, spec.key);
    if (!o_pres && !s_pres) return UNVERIFIED;
    if (!s_pres) return spec.exclude_missing_sub ? FAIL : UNVERIFIED;
    if (!o_pres) return UNVERIFIED;
    if (spec.rank) {
        std::string og = exact_group(norm_class(detail::jstr(orig, spec.key)), *spec.rank);
        std::string sg = exact_group(norm_class(detail::jstr(sub, spec.key)), *spec.rank);
        return og == sg ? PASS : FAIL;
    }
    auto of = detail::jnum(orig, spec.key), sf = detail::jnum(sub, spec.key);
    if (of && sf) {
        if (*of == *sf) return PASS;
        double denom = std::max(std::abs(*of), std::abs(*sf));
        if (denom > 0 && std::abs(*of - *sf) / denom <= spec.tol_factor) return PASS;
        return FAIL;
    }
    std::string on = norm_class(detail::jstr(orig, spec.key)), sn = norm_class(detail::jstr(sub, spec.key));
    if (!on.empty() && on == sn) return PASS;
    return FAIL;
}

// ── Saturation-current %-drop normalization (manufacturer-agnostic) ──────────
// A magnetic's I_sat is quoted at a roll-off criterion — the % the inductance has
// dropped from its small-signal value (|ΔL/L|). Vendors pick different criteria
// (Würth @10 %/@30 %, Coilcraft/Vishay @20 %), so comparing raw scalars manufactures
// a FALSE shortfall. This mirrors Heaviside's heaviside/pipeline/isat_normalize.py
// (proven, 13 tests): normalize BOTH sides to a common criterion before comparing.
// It reads ONLY the stated per-point %-drop of each datapoint, never the vendor.
namespace isat {
constexpr double kCanonicalPercentDrop = 20.0;  // most common industry convention
constexpr double kBasisUncertaintyBand = 1.9;   // ratio a criterion diff could explain

struct Point {
    std::optional<double> percent_drop;  // |ΔL/L| %, or nullopt if basis unknown
    double current;                      // amperes
};

// Parse a saturation_points array ([{percent_drop, current, temperature?}]),
// dropping malformed entries; sorted with known-basis points first (mirrors
// _coerce_points).
inline std::vector<Point> coerce_points(const nlohmann::json& pts) {
    std::vector<Point> out;
    if (!pts.is_array()) return out;
    for (const auto& p : pts) {
        if (!p.is_object()) continue;
        auto ci = p.find("current");
        if (ci == p.end() || ci->is_null() || ci->is_boolean() || !ci->is_number()) continue;
        double cur = ci->get<double>();
        if (!(cur > 0)) continue;
        std::optional<double> pd;
        auto pi = p.find("percent_drop");
        if (pi != p.end() && !pi->is_null() && !pi->is_boolean() && pi->is_number()) {
            double v = pi->get<double>();
            if (v >= 0) pd = v;
        }
        out.push_back({pd, cur});
    }
    std::sort(out.begin(), out.end(), [](const Point& a, const Point& b) {
        bool an = !a.percent_drop.has_value(), bn = !b.percent_drop.has_value();
        if (an != bn) return an < bn;  // known (false) sorts before unknown (true)
        return a.percent_drop.value_or(0.0) < b.percent_drop.value_or(0.0);
    });
    return out;
}

// Best saturation representation for one candidate summary (mirrors
// resolve_saturation_points priority): explicit saturation_points table, else
// the legacy saturation_current scalar as one basis-unknown point. (The
// inductancePoints-derived path runs upstream in Heaviside, which fills
// saturation_points — Kelvin reads the already-resolved list.)
inline std::vector<Point> resolve_points(const nlohmann::json& summary) {
    auto it = summary.find("saturation_points");
    if (it != summary.end() && it->is_array() && !it->empty()) {
        auto pts = coerce_points(*it);
        if (!pts.empty()) return pts;
    }
    auto sc = detail::jnum(summary, "saturation_current");
    if (sc && *sc > 0) return {{std::nullopt, *sc}};
    return {};
}

inline bool has_basis(const std::vector<Point>& pts) {
    for (const auto& p : pts)
        if (p.percent_drop) return true;
    return false;
}

// I_sat interpolated to target_pct inductance drop; nullopt when no based points.
// Clamps beyond the measured range rather than extrapolating a divergent slope.
inline std::optional<double> isat_at_percent_drop(const std::vector<Point>& all, double target) {
    std::vector<const Point*> pts;
    for (const auto& p : all)
        if (p.percent_drop) pts.push_back(&p);
    if (pts.empty()) return std::nullopt;
    for (const auto* p : pts)
        if (std::abs(*p->percent_drop - target) < 1e-9) return p->current;
    const Point* below = nullptr;  // greatest pd below target
    const Point* above = nullptr;  // smallest pd above target
    for (const auto* p : pts) {
        if (*p->percent_drop < target && (!below || *p->percent_drop > *below->percent_drop))
            below = p;
        if (*p->percent_drop > target && (!above || *p->percent_drop < *above->percent_drop))
            above = p;
    }
    if (below && above) {
        double frac = (target - *below->percent_drop) / (*above->percent_drop - *below->percent_drop);
        return below->current + frac * (above->current - below->current);
    }
    return below ? below->current : above->current;
}

// The single most representative I_sat when bases can't be matched: a based point
// nearest the canonical criterion, else the lone scalar (mirrors _headline).
inline std::optional<double> headline(const std::vector<Point>& pts) {
    if (pts.empty()) return std::nullopt;
    const Point* best = nullptr;
    for (const auto& p : pts) {
        if (!p.percent_drop) continue;
        if (!best || std::abs(*p.percent_drop - kCanonicalPercentDrop) <
                         std::abs(*best->percent_drop - kCanonicalPercentDrop))
            best = &p;
    }
    return best ? best->current : pts.front().current;
}

enum class Verdict { Adequate, Shortfall, Uncertain };

// Mirrors compare_isat: both based -> normalize + compare; else headline compare
// that only calls SHORTFALL when the gap is too large for any basis diff.
inline Verdict compare(const std::vector<Point>& op, const std::vector<Point>& sp, double margin) {
    auto o_at = isat_at_percent_drop(op, kCanonicalPercentDrop);
    auto s_at = isat_at_percent_drop(sp, kCanonicalPercentDrop);
    if (o_at && s_at) return (*s_at >= margin * *o_at) ? Verdict::Adequate : Verdict::Shortfall;
    auto o_raw = headline(op), s_raw = headline(sp);
    if (!o_raw || !s_raw) return Verdict::Uncertain;
    double ratio = *s_raw / *o_raw;
    if (ratio * kBasisUncertaintyBand < margin) return Verdict::Shortfall;
    return Verdict::Uncertain;
}
}  // namespace isat

// saturation_current gate: %-drop-normalized when either side states a real
// inductance-drop basis; otherwise the raw HIGHER_BETTER numeric compare (legacy
// basis-unknown scalars — preserves existing pass/warn/fail granularity).
inline const char* compare_saturation_current(const ParamSpec& spec, const nlohmann::json& orig,
                                              const nlohmann::json& sub) {
    std::vector<isat::Point> op = isat::resolve_points(orig), sp = isat::resolve_points(sub);
    bool o_has = !op.empty(), s_has = !sp.empty();
    if (!o_has && !s_has) return UNVERIFIED;
    if (!s_has) return spec.exclude_missing_sub ? FAIL : UNVERIFIED;
    if (!o_has) return UNVERIFIED;
    if (!isat::has_basis(op) && !isat::has_basis(sp))
        return compare_numeric(spec, detail::jnum(orig, spec.key), detail::jnum(sub, spec.key));
    switch (isat::compare(op, sp, spec.tol_factor)) {
        case isat::Verdict::Adequate:  return PASS;
        case isat::Verdict::Shortfall: return FAIL;
        case isat::Verdict::Uncertain: return WARN;
    }
    return WARN;  // unreachable
}

inline const char* compare_param(const ParamSpec& spec, const nlohmann::json& orig,
                                 const nlohmann::json& sub) {
    if (spec.key == "saturation_current") return compare_saturation_current(spec, orig, sub);
    if (spec.dir == Dir::ClassMatch)
        return compare_class(spec, detail::jstr(orig, spec.key), detail::jstr(sub, spec.key));
    if (spec.dir == Dir::ExactMatch) return compare_exact(spec, orig, sub);
    auto o = detail::jnum(orig, spec.key), s = detail::jnum(sub, spec.key);
    if (spec.magnitude) {
        if (o) o = std::abs(*o);
        if (s) s = std::abs(*s);
    }
    return compare_numeric(spec, o, s);
}

// PARAM_SPECS — the per-category tables (param_check.py).
inline const std::vector<ParamSpec>& params_for(const std::string& category) {
    using D = Dir;
    static const std::vector<ParamSpec> kEmpty;
    static const std::vector<ParamSpec> kCapacitor = {
        {"esr", D::Lower, 1.5, std::nullopt, false, true, nullptr},
        {"ripple_current", D::Higher, 0.9, std::nullopt, false, true, nullptr},
        {"technology", D::ClassMatch, 0.0, std::nullopt, false, false, &dielectric_rank()},
        {"dielectric_code", D::ClassMatch, 0.0, std::nullopt, false, false, &dielectric_rank()},
        {"temp_max_C", D::Higher, 0.0, 5.0, false, false, nullptr},
        {"tolerance_pct", D::Lower, 2.0, std::nullopt, false, false, nullptr},
    };
    static const std::vector<ParamSpec> kMosfet = {
        {"rds_on", D::Lower, 1.5, std::nullopt, false, false, nullptr},
        {"qg", D::Lower, 2.0, std::nullopt, false, false, nullptr},
        {"coss", D::Lower, 2.0, std::nullopt, false, false, nullptr},
        {"vgs_threshold_max", D::Lower, 2.0, std::nullopt, false, false, nullptr},
    };
    static const std::vector<ParamSpec> kDiode = {
        {"vf", D::Lower, 1.2, std::nullopt, false, false, nullptr},
        {"qrr", D::Lower, 2.0, std::nullopt, false, false, nullptr},
        {"trr", D::Lower, 2.0, std::nullopt, false, false, nullptr},
    };
    static const std::vector<ParamSpec> kResistor = {
        {"power_rating", D::Higher, 0.9, std::nullopt, false, false, nullptr},
        {"tolerance_pct", D::Lower, 2.0, std::nullopt, false, false, nullptr},
        {"tcr", D::Lower, 2.0, std::nullopt, true, false, nullptr},
    };
    static const std::vector<ParamSpec> kMagnetic = {
        {"saturation_current", D::Higher, 0.9, std::nullopt, false, true, nullptr},
        {"dcr", D::Lower, 1.3, std::nullopt, false, false, nullptr},
        {"rated_current", D::Higher, 0.9, std::nullopt, false, false, nullptr},
    };
    static const std::vector<ParamSpec> kChipBead = {
        // exclude_missing_sub: impedance IS what a bead is. A candidate with no
        // impedance data cannot be verified as a substitute for one, so its
        // absence FAILs rather than passing silently — otherwise a part with no
        // curve at all accrues no penalty and outranks genuinely curve-matched
        // parts, which is exactly the "missing data ranks best" pathology.
        {"impedance_100mhz", D::Higher, 0.8, std::nullopt, false, true, nullptr},
        // Peak |Z| and — critically — the frequency it occurs at. Two beads with
        // the same Z@100MHz can peak several-fold apart in height and in
        // frequency, which is what decides whether the part suppresses the noise
        // actually present. The peak FREQUENCY is a shape parameter, so a shift
        // in either direction matters: ExactMatch with a 30% band, wide enough
        // to tolerate curve-sampling differences and narrow enough to separate
        // parts that work in genuinely different bands.
        // Also exclude-if-missing: peak |Z| and its frequency are the curve. A
        // candidate carrying neither the 100 MHz value NOR a curve takes both
        // failures and therefore sorts below one that carries the curve and
        // matches it — otherwise the part we know least about wins on having
        // accrued the fewest penalties.
        {"impedance_peak", D::Higher, 0.8, std::nullopt, false, true, nullptr},
        {"impedance_peak_freq", D::ExactMatch, 0.3, std::nullopt, false, false, nullptr},
        {"srf", D::Higher, 0.8, std::nullopt, false, false, nullptr},
        {"dcr", D::Lower, 1.3, std::nullopt, false, false, nullptr},
        {"rated_current", D::Higher, 0.9, std::nullopt, false, false, nullptr},
    };
    static const std::vector<ParamSpec> kConnector = {
        {"family", D::ExactMatch, 0.0, std::nullopt, false, true, &connector_family_rank()},
        {"positions", D::ExactMatch, 0.0, std::nullopt, false, true, nullptr},
        {"polarity", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        {"pitch_mm", D::ExactMatch, 0.02, std::nullopt, false, false, nullptr},
        {"interface_standard", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        {"mounting", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        {"rated_current_A", D::Higher, 0.9, std::nullopt, false, true, nullptr},
        {"rated_voltage_V", D::Higher, 0.9, std::nullopt, false, false, nullptr},
        {"temp_min_C", D::Lower, 0.0, 15.0, false, false, nullptr},
        {"temp_max_C", D::Higher, 0.0, 15.0, false, false, nullptr},
        {"mating_cycles", D::Higher, 0.5, std::nullopt, false, false, nullptr},
        {"contact_resistance", D::Lower, 2.0, std::nullopt, false, false, nullptr},
    };
    static const std::vector<ParamSpec> kAnalog = {
        {"subtype", D::ExactMatch, 0.0, std::nullopt, false, true, nullptr},
        {"channels", D::ExactMatch, 0.0, std::nullopt, false, true, nullptr},
        {"package", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        {"supply_min_V", D::Lower, 0.0, 0.3, false, false, nullptr},
        {"supply_max_V", D::Higher, 0.9, std::nullopt, false, false, nullptr},
        {"gbw", D::Higher, 0.7, std::nullopt, false, false, nullptr},
        {"slew_rate", D::Higher, 0.7, std::nullopt, false, false, nullptr},
        {"input_offset_voltage", D::Lower, 2.0, std::nullopt, true, false, nullptr},
        {"offset_drift", D::Lower, 2.0, std::nullopt, true, false, nullptr},
        {"input_bias_current", D::Lower, 10.0, std::nullopt, true, false, nullptr},
        {"cmrr_db", D::Higher, 0.0, 6.0, false, false, nullptr},
        {"quiescent_current", D::Lower, 3.0, std::nullopt, false, false, nullptr},
        {"rail_to_rail_input", D::ClassMatch, 0.0, std::nullopt, false, false, &yes_no_rank()},
        {"rail_to_rail_output", D::ClassMatch, 0.0, std::nullopt, false, false, &yes_no_rank()},
        {"propagation_delay", D::Lower, 2.0, std::nullopt, false, false, nullptr},
        {"output_stage", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        {"resolution", D::Higher, 1.0, std::nullopt, false, false, nullptr},
        {"sample_rate", D::Higher, 0.9, std::nullopt, false, false, nullptr},
    };
    static const std::vector<ParamSpec> kTimeBase = {
        {"subtype", D::ExactMatch, 0.0, std::nullopt, false, true, nullptr},
        {"technology", D::ExactMatch, 0.0, std::nullopt, false, true, nullptr},
        {"frequency", D::ExactMatch, 1e-4, std::nullopt, false, true, nullptr},
        {"load_capacitance_pF", D::ExactMatch, 0.05, std::nullopt, false, false, nullptr},
        {"output_type", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        {"mode", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        {"tolerance_ppm", D::Lower, 2.0, std::nullopt, false, false, nullptr},
        {"stability_ppm", D::Lower, 2.0, std::nullopt, false, false, nullptr},
        {"aging_ppm_y", D::Lower, 2.0, std::nullopt, false, false, nullptr},
        {"esr", D::Lower, 1.5, std::nullopt, false, false, nullptr},
        {"supply_min_V", D::Lower, 0.0, 0.3, false, false, nullptr},
        {"supply_max_V", D::Higher, 0.9, std::nullopt, false, false, nullptr},
        {"current_consumption", D::Lower, 3.0, std::nullopt, false, false, nullptr},
        {"temp_min_C", D::Lower, 0.0, 15.0, false, false, nullptr},
        {"temp_max_C", D::Higher, 0.0, 15.0, false, false, nullptr},
        {"package", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
    };
    if (category == "capacitor") return kCapacitor;
    if (category == "mosfet") return kMosfet;
    if (category == "diode") return kDiode;
    if (category == "resistor") return kResistor;
    if (category == "magnetic") return kMagnetic;
    if (category == "chipBead") return kChipBead;
    if (category == "connector") return kConnector;
    if (category == "analog") return kAnalog;
    if (category == "timeBase") return kTimeBase;
    return kEmpty;
}

// One {name, verdict} per parameter that has data on at least one side.
inline std::vector<std::pair<std::string, std::string>> evaluate_params(
    const std::string& category, const nlohmann::json& orig, const nlohmann::json& sub) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const ParamSpec& spec : params_for(category)) {
        bool has_data = detail::present(orig, spec.key) || detail::present(sub, spec.key);
        if (spec.key == "saturation_current")  // a multi-point table counts as data too
            has_data = has_data || detail::present(orig, "saturation_points") ||
                       detail::present(sub, "saturation_points");
        if (!has_data) continue;
        out.emplace_back(spec.key, compare_param(spec, orig, sub));
    }
    return out;
}

}  // namespace kelvin::crossref
