// Cross-reference ranker — the deterministic "score substitutes for an original
// part" verb. Program-only and LLM-free: Kirchhoff and the Kelvin web frontend
// consume the ranked list directly; Heaviside runs its LLM chooser over the same
// list.
//
// Given an ORIGINAL part's spec block and a list of candidate spec blocks (both
// category-appropriate JSON, SI base units), returns a ranked, gated, honest
// result: each candidate carries a status (recommended/partial/no_substitute),
// per-parameter verdicts, a total penalty, and an original_verified flag.
//
// The scoring model is Heaviside's, reused rather than re-derived:
//   * primary value          CrossRefScore.hpp   (score_primary_value)
//   * per-parameter verdicts CrossRefParams.hpp  (params_for/compare_param — the
//                            PARAM_SPECS table, 9 categories, incl. class/exact
//                            gates and %-drop-normalized saturation current)
//   * physical fit           CrossRefDimensions.hpp (case-code resolution,
//                            3-axis orientation-agnostic footprint, mount type)
//   * MPN-derived gates      CrossRefDecode.hpp  (AEC-Q grade, rated voltage,
//                            MLCC DC-bias effective capacitance)
// Critical ratings the PARAM_SPECS table deliberately leaves to Heaviside's
// stress guardrails (Vds, Vrrm, rated voltage, Id, If) are gated here instead,
// since this ranker has no guardrail stage behind it.
//
// The honesty rule (the FAE finding): when the ORIGINAL's specs are unverified,
// a candidate can never be a clean 'recommended' — it is capped at 'partial' and
// flagged original_unverified. A candidate whose primary value is out of range,
// whose critical rating falls far below a KNOWN original, whose identity
// parameters differ, or whose mount type is incompatible, is rejected outright.
#pragma once
#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "CrossRefDecode.hpp"
#include "CrossRefDimensions.hpp"
#include "CrossRefParams.hpp"
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

// ── Critical ratings ─────────────────────────────────────────────────────────
// Directional ratings that PARAM_SPECS leaves out because Heaviside gates them
// in its stress guardrails (G7 voltage stress, G8 current stress). `hard` means
// a FAIL against a KNOWN original rejects the candidate: shipping a 40 V FET in
// place of a 100 V one is not a partial substitution, it is a field failure.
struct Rating {
    const char* key;
    Mode mode;
    double warn_factor;
    double gate_factor;
    bool hard;
};

inline std::vector<Rating> critical_ratings(const std::string& cat) {
    if (cat == "mosfet")
        return {{"vds", Mode::HigherBetter, 0.95, 0.9, true},
                {"id", Mode::HigherBetter, 0.9, 0.7, false}};
    if (cat == "diode")
        return {{"vrrm", Mode::HigherBetter, 0.95, 0.9, true},
                {"if_avg", Mode::HigherBetter, 0.9, 0.7, false}};
    if (cat == "capacitor") return {{"voltage", Mode::HigherBetter, 0.95, 0.9, true}};
    if (cat == "igbt")
        return {{"vces", Mode::HigherBetter, 0.95, 0.9, true},
                {"ic", Mode::HigherBetter, 0.9, 0.7, false}};
    return {};
}

// Parameters inside PARAM_SPECS whose FAIL is fatal rather than a demotion:
// a severe shortfall on a magnetic's current rating, or a mismatch on an
// IDENTITY parameter (a 6-position connector is not a 4-position one; a dual
// op-amp is not a quad). Everything else demotes to 'partial'.
inline bool is_hard_param(const std::string& cat, const std::string& key) {
    static const std::set<std::string> magnetic{"saturation_current", "rated_current"};
    static const std::set<std::string> connector{"family", "positions", "rated_current_A"};
    static const std::set<std::string> analog{"subtype", "channels"};
    static const std::set<std::string> timebase{"subtype", "technology", "frequency"};
    if (cat == "magnetic" || cat == "chipBead") return magnetic.count(key) > 0;
    if (cat == "connector") return connector.count(key) > 0;
    if (cat == "analog") return analog.count(key) > 0;
    if (cat == "timeBase") return timebase.count(key) > 0;
    return false;
}

struct Options {
    double primary_weight = 4.0;    // value proximity dominates (_VALUE_MATCH_WEIGHT)
    double gate_weight = 1.0;
    double overdim_weight = 0.1;    // small tie-breaker toward right-sizing
    double footprint_weight = 1.0;  // Heaviside's penalties are already scaled
    bool original_verified = true;  // caller: was the original's spec block resolved?
    bool check_footprint = true;
    bool check_lifecycle = true;
    std::optional<double> operating_voltage;  // enables the MLCC DC-bias comparison
    size_t max_results = 25;
};

// Non-numeric verdict penalties. Sized to sit in the same 0-5 band as the
// numeric terms so no single categorical miss can outweigh the primary value.
inline constexpr double kVerdictWarnPenalty = 0.5;
inline constexpr double kVerdictFailPenalty = 3.0;

// Physical size of a part: an explicit mechanical drawing when the record has
// one, else the category-aware case-code resolution. Metres.
inline std::optional<Dims> dims_of(const json& p, const std::string& category) {
    auto l = num(p, "length_m"), w = num(p, "width_m"), h = num(p, "height_m");
    if (l && w && *l > 0 && *w > 0) {
        Dims d;
        d.length = *l;
        d.width = *w;
        if (h && *h > 0) d.height = *h;
        return d;
    }
    std::string code = str(p, "case_code");
    if (code.empty()) code = str(p, "package");
    return resolve_dimensions(code, category);
}

// Score one candidate against the original. Returns a per-candidate JSON verdict.
inline json score_candidate(const std::string& cat, const json& original, const json& cand,
                            const Options& opt) {
    json out;
    out["mpn"] = str(cand, "mpn");
    double penalty = 0.0;
    std::string status = "recommended";
    json params = json::array();
    std::vector<std::string> notes;

    auto reject = [&](const char* reason) {
        out["status"] = "no_substitute";
        out["reason"] = reason;
        out["penalty"] = 1e9;
        out["params"] = params;
        if (!notes.empty()) out["notes"] = notes;
        return out;
    };
    auto demote = [&] {
        if (status == "recommended") status = "partial";
    };

    // ── primary value ────────────────────────────────────────────────────────
    bool has_pv = false;
    auto pv = score_primary_value(cat, num(original, "value_si"), num(cand, "value_si"), has_pv);
    if (has_pv) {
        params.push_back({{"name", "value"}, {"verdict", pv.verdict}});
        if (pv.verdict == FAIL) return reject("primary value out of range");
        penalty += opt.primary_weight * pv.penalty;
        if (pv.verdict == WARN) demote();
    }

    // ── PARAM_SPECS verdicts (the shared Heaviside table) ────────────────────
    for (const ParamSpec& spec : params_for(cat)) {
        bool has_data = detail::present(original, spec.key) || detail::present(cand, spec.key);
        if (spec.key == "saturation_current")
            has_data = has_data || detail::present(original, "saturation_points") ||
                       detail::present(cand, "saturation_points");
        if (!has_data) continue;
        const std::string verdict = compare_param(spec, original, cand);
        params.push_back({{"name", spec.key}, {"verdict", verdict}});

        const bool numeric = (spec.dir == Dir::Lower || spec.dir == Dir::Higher);
        auto o = numeric ? detail::jnum(original, spec.key) : std::nullopt;
        auto s = numeric ? detail::jnum(cand, spec.key) : std::nullopt;

        if (verdict == FAIL) {
            if (is_hard_param(cat, spec.key) && (o || detail::present(original, spec.key)))
                return reject(spec.dir == Dir::ExactMatch
                                  ? "an identity parameter differs from the original"
                                  : "a critical rating falls far below the original");
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
        } else if (verdict == WARN) {
            demote();
            penalty += opt.gate_weight * kVerdictWarnPenalty;
        } else if (verdict == PASS && numeric && o && s) {
            // Right-sizing tie-breaker: a rating that vastly exceeds the
            // original's costs a little, with diminishing returns.
            penalty += opt.overdim_weight * (spec.dir == Dir::Higher
                                                 ? over_dimensioning_penalty(o, s, 1.0)
                                                 : over_dimensioning_penalty(s, o, 1.0));
        }
    }

    // ── critical ratings (Vds / Vrrm / rated voltage / Id / If) ──────────────
    for (const auto& r : critical_ratings(cat)) {
        auto o = num(original, r.key), s = num(cand, r.key);
        if (!o && !s) continue;
        auto v = score_directional(o, s, r.mode, r.warn_factor, r.gate_factor);
        params.push_back({{"name", r.key}, {"verdict", v.verdict}});
        if (v.verdict == FAIL) {
            if (r.hard && o) return reject("a critical rating falls far below the original");
            demote();
            penalty += opt.gate_weight * v.penalty;
        } else if (v.verdict == WARN) {
            demote();
            penalty += opt.gate_weight * v.penalty;
        } else {
            penalty += opt.overdim_weight * over_dimensioning_penalty(o, s, 1.0);
        }
    }

    // ── physical fit ─────────────────────────────────────────────────────────
    if (opt.check_footprint) {
        const std::string o_pkg = str(original, "package").empty() ? str(original, "case_code")
                                                                   : str(original, "package");
        const std::string s_pkg =
            str(cand, "package").empty() ? str(cand, "case_code") : str(cand, "package");
        // Mount type: an SMD part cannot stand in for a leaded one. Skipped for
        // categories whose package strings vary too much by series to classify.
        if (mount_gate_applies(cat) &&
            mount_incompatible(str(original, "mount"), str(cand, "mount"), o_pkg, s_pkg)) {
            params.push_back({{"name", "mounting"}, {"verdict", FAIL}});
            notes.push_back("mount type incompatible: " + o_pkg + " -> " + s_pkg);
            return reject("mount type incompatible (SMD vs leaded)");
        }
        auto o_dims = dims_of(original, cat), s_dims = dims_of(cand, cat);
        FootprintTier tier = footprint_tier(o_dims, s_dims);
        if (o_dims) {
            penalty += opt.footprint_weight * footprint_penalty(o_dims, s_dims);
            out["footprint"] = footprint_tier_name(tier);
            const char* verdict = tier == FootprintTier::Fits            ? PASS
                                  : tier == FootprintTier::OneSizeLarger ? WARN
                                  : tier == FootprintTier::Overflows     ? FAIL
                                                                         : UNVERIFIED;
            params.push_back({{"name", "footprint"}, {"verdict", verdict}});
            // Heaviside demotes rather than rejects an oversize part: it is a
            // real part that works electrically and needs a board-space check.
            // The engineer decides whether the board has room.
            if (tier == FootprintTier::OneSizeLarger) {
                demote();
                notes.push_back("about one case size larger — verify board fit");
            } else if (tier == FootprintTier::Overflows) {
                demote();
                notes.push_back("larger than the original's footprint — board respin likely");
            }
        }
    }

    // ── automotive grade (AEC-Q), decoded from the MPN ───────────────────────
    // Fires even when the original is not in the catalogue, which is exactly
    // when an engineer most needs to be told.
    if (is_automotive_grade(str(original, "mpn")) && !is_automotive_grade(str(cand, "mpn"))) {
        params.push_back({{"name", "automotive_grade"}, {"verdict", FAIL}});
        demote();
        penalty += opt.gate_weight * kVerdictFailPenalty;
        notes.push_back("original is an automotive (AEC-Q) grade part; substitute is commercial");
    }

    // ── rated voltage decoded from the MPN, when the record lacks it ─────────
    if (cat == "capacitor" && !num(original, "voltage") && !num(cand, "voltage")) {
        auto od = decode_cap_mpn(str(original, "mpn"));
        auto sd = decode_cap_mpn(str(cand, "mpn"));
        if (od.voltage && sd.voltage) {
            const char* verdict = *sd.voltage >= *od.voltage ? PASS : FAIL;
            params.push_back({{"name", "voltage_from_mpn"}, {"verdict", verdict}});
            if (verdict == FAIL) {
                notes.push_back("substitute's MPN-decoded rated voltage is below the original's");
                return reject("rated voltage (decoded from MPN) below the original");
            }
        }
    }

    // ── MLCC DC-bias: compare EFFECTIVE capacitance at the operating point ───
    if (cat == "capacitor" && opt.operating_voltage && *opt.operating_voltage > 0) {
        auto oc = effective_capacitance_at_bias(
            num(original, "value_si"), num(original, "voltage"),
            num(original, "capacitance_saturation_mlcc"), num(original, "vth_mlcc"),
            *opt.operating_voltage);
        auto sc = effective_capacitance_at_bias(
            num(cand, "value_si"), num(cand, "voltage"),
            num(cand, "capacitance_saturation_mlcc"), num(cand, "vth_mlcc"),
            *opt.operating_voltage);
        if (oc || sc) {
            auto v = score_directional(oc, sc, Mode::HigherBetter, 0.9, 0.9);
            params.push_back({{"name", "c_bias"}, {"verdict", v.verdict}});
            if (v.verdict == FAIL || v.verdict == WARN) {
                demote();
                penalty += opt.gate_weight * v.penalty;
                notes.push_back("effective capacitance under DC bias falls short of the original");
            }
        }
    }

    // ── lifecycle ────────────────────────────────────────────────────────────
    // Heaviside never consults this; Kelvin's catalogue carries it, so a
    // substitute that is not in production is surfaced rather than silently
    // recommended.
    if (opt.check_lifecycle) {
        auto it = cand.find("is_production");
        if (it != cand.end() && it->is_boolean() && !it->get<bool>()) {
            params.push_back({{"name", "lifecycle"}, {"verdict", WARN}});
            demote();
            penalty += opt.gate_weight * kVerdictWarnPenalty;
            notes.push_back("substitute is not marked production status");
        }
    }

    // Honesty: an unverified original can never be a clean 'recommended'.
    if (!opt.original_verified) {
        if (status == "recommended") status = "partial";
        out["original_unverified"] = true;
    }

    out["status"] = status;
    out["penalty"] = penalty;
    out["params"] = params;
    if (!notes.empty()) out["notes"] = notes;
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

// JSON-options wrapper shared by both bindings (PyKelvin + embind WASM).
inline json cross_reference_json(const std::string& category, const json& original,
                                 const json& candidates, const json& options) {
    Options opt;
    if (options.contains("original_verified") && options["original_verified"].is_boolean())
        opt.original_verified = options["original_verified"].get<bool>();
    if (options.contains("max_results") && options["max_results"].is_number())
        opt.max_results = options["max_results"].get<size_t>();
    if (options.contains("primary_weight") && options["primary_weight"].is_number())
        opt.primary_weight = options["primary_weight"].get<double>();
    if (options.contains("check_footprint") && options["check_footprint"].is_boolean())
        opt.check_footprint = options["check_footprint"].get<bool>();
    if (options.contains("check_lifecycle") && options["check_lifecycle"].is_boolean())
        opt.check_lifecycle = options["check_lifecycle"].get<bool>();
    if (options.contains("operating_voltage") && options["operating_voltage"].is_number())
        opt.operating_voltage = options["operating_voltage"].get<double>();
    return cross_reference(category, original, candidates, opt);
}

}  // namespace kelvin::crossref
