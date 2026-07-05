// Cross-reference ranker — the deterministic "score substitutes for an original
// part" verb. Ported from Heaviside's crossref param-check gates + _rank_candidates
// core, built on CrossRefScore.hpp. Program-only and LLM-free: Kirchhoff consumes
// the ranked list directly; Heaviside runs its LLM chooser over the same list.
//
// Given an ORIGINAL part's spec block and a list of candidate spec blocks (both
// category-appropriate JSON, SI base units), returns a ranked, gated, honest
// result: each candidate carries a status (recommended/partial/no_substitute),
// per-parameter verdicts, a total penalty, and an original_verified flag.
//
// The honesty rule (the FAE finding): when the ORIGINAL's non-value specs are
// unknown, a candidate can never be a clean 'recommended' — it is capped at
// 'partial' and flagged original_unverified. A candidate whose primary value is
// out of range, or whose saturation/rated current is far below a KNOWN original,
// is rejected (no_substitute).
#pragma once
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "CrossRefScore.hpp"

namespace kelvin::crossref {

using json = nlohmann::json;

inline std::optional<double> num(const json& o, const char* k) {
    auto it = o.find(k);
    if (it == o.end() || it->is_null()) return std::nullopt;
    if (it->is_number()) return it->get<double>();
    return std::nullopt;
}

inline std::string str(const json& o, const char* k) {
    auto it = o.find(k);
    if (it == o.end() || it->is_null() || !it->is_string()) return "";
    return it->get<std::string>();
}

// One secondary parameter gate: numeric direction + the deficit thresholds.
struct Gate {
    const char* key;
    Mode mode;
    double warn_factor;
    double gate_factor;
    bool hard;  // a FAIL rejects the candidate outright (severe current, voltage floor)
};

// Judge-critical secondary gates per category (subset of param_check.py
// PARAM_SPECS — the ones that govern field failures). Full list grows over time.
inline std::vector<Gate> gates_for(const std::string& cat) {
    if (cat == "magnetic" || cat == "chipBead") {
        return {
            {"saturation_current", Mode::HigherBetter, 0.9, 0.7, true},  // severe shortfall = reject
            {"rated_current", Mode::HigherBetter, 0.9, 0.7, true},
            {"dcr", Mode::LowerBetter, 1.3, 2.0, false},
        };
    }
    if (cat == "capacitor") {
        return {
            {"voltage", Mode::HigherBetter, 0.95, 0.9, true},
            {"temp_max_C", Mode::HigherBetter, 0.95, 0.9, false},
            {"esr", Mode::LowerBetter, 1.5, 3.0, false},
            {"ripple_current", Mode::HigherBetter, 0.9, 0.7, false},
            {"tolerance_pct", Mode::LowerBetter, 2.0, 4.0, false},
        };
    }
    if (cat == "resistor") {
        return {
            {"power_rating", Mode::HigherBetter, 0.9, 0.7, false},
            {"tolerance_pct", Mode::LowerBetter, 2.0, 4.0, false},
            {"tcr", Mode::LowerBetter, 2.0, 4.0, false},
        };
    }
    if (cat == "mosfet") {
        return {
            {"vds", Mode::HigherBetter, 0.95, 0.9, true},
            {"id", Mode::HigherBetter, 0.9, 0.7, false},
            {"rds_on", Mode::LowerBetter, 1.5, 2.5, false},
            {"qg", Mode::LowerBetter, 2.0, 4.0, false},
        };
    }
    if (cat == "diode") {
        return {
            {"vrrm", Mode::HigherBetter, 0.95, 0.9, true},
            {"if_avg", Mode::HigherBetter, 0.9, 0.7, false},
            {"vf", Mode::LowerBetter, 1.2, 2.0, false},
            {"qrr", Mode::LowerBetter, 2.0, 4.0, false},
        };
    }
    return {};
}

struct Options {
    double primary_weight = 4.0;   // value proximity dominates (mirrors _VALUE_MATCH_WEIGHT)
    double gate_weight = 1.0;
    double overdim_weight = 0.1;   // small tie-breaker toward right-sizing
    bool original_verified = true; // caller: was the original's spec block resolved?
    size_t max_results = 25;
};

// Score one candidate against the original. Returns a per-candidate JSON verdict.
inline json score_candidate(const std::string& cat, const json& original, const json& cand,
                            const Options& opt) {
    json out;
    out["mpn"] = str(cand, "mpn");
    double penalty = 0.0;
    std::string status = "recommended";
    json params = json::array();

    // Primary value gate.
    bool has_pv = false;
    auto pv = score_primary_value(cat, num(original, "value_si"), num(cand, "value_si"), has_pv);
    if (has_pv) {
        params.push_back({{"name", "value"}, {"verdict", pv.verdict}});
        if (pv.verdict == FAIL) {
            out["status"] = "no_substitute";
            out["reason"] = "primary value out of range";
            out["penalty"] = 1e9;
            out["params"] = params;
            return out;
        }
        penalty += opt.primary_weight * pv.penalty;
        if (pv.verdict == WARN && status == "recommended") status = "partial";
    }

    // Secondary directional gates.
    bool any_hard_fail = false;
    for (const auto& g : gates_for(cat)) {
        auto o = num(original, g.key), s = num(cand, g.key);
        if (!o && !s) continue;
        auto r = score_directional(o, s, g.mode == Mode::LowerBetter ? Mode::LowerBetter
                                                                     : Mode::HigherBetter,
                                   g.warn_factor, g.gate_factor);
        params.push_back({{"name", g.key}, {"verdict", r.verdict}});
        if (r.verdict == FAIL) {
            if (g.hard && o) {  // hard gate fails only against a KNOWN original datum
                any_hard_fail = true;
            } else if (status == "recommended") {
                status = "partial";
            }
            penalty += opt.gate_weight * r.penalty;
        } else if (r.verdict == WARN && status == "recommended") {
            status = "partial";
            penalty += opt.gate_weight * r.penalty;
        } else {
            // PASS: small over-dimensioning tie-breaker toward right-sizing.
            penalty += opt.overdim_weight * over_dimensioning_penalty(o, s, 1.0);
        }
    }
    if (any_hard_fail) {
        out["status"] = "no_substitute";
        out["reason"] = "a critical rating falls far below the original";
        out["penalty"] = 1e9;
        out["params"] = params;
        return out;
    }

    // Honesty: an unverified original can never be a clean 'recommended'.
    if (!opt.original_verified) {
        if (status == "recommended") status = "partial";
        out["original_unverified"] = true;
    }

    out["status"] = status;
    out["penalty"] = penalty;
    out["params"] = params;
    return out;
}

// Rank a candidate list for an original. Returns {category, original_verified,
// candidates:[...]} sorted best-first (lowest penalty); no_substitute rows sink.
inline json cross_reference(const std::string& category, const json& original,
                            const json& candidates, const Options& opt = {}) {
    std::vector<json> scored;
    scored.reserve(candidates.size());
    for (const auto& c : candidates) scored.push_back(score_candidate(category, original, c, opt));
    std::stable_sort(scored.begin(), scored.end(), [](const json& a, const json& b) {
        return a.value("penalty", 1e18) < b.value("penalty", 1e18);
    });
    json cands = json::array();
    for (size_t i = 0; i < scored.size() && i < opt.max_results; ++i) cands.push_back(scored[i]);
    return {{"category", category},
            {"original_verified", opt.original_verified},
            {"candidates", cands}};
}

}  // namespace kelvin::crossref
