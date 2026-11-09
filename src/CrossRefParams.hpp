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
    bool exclude_missing_sub = false;  // missing substitute value disqualifies (ParamOutcome)
    const ClassRank* rank = nullptr;   // class-equivalence map (dielectric / family / y-n)
    // HIGHER only: the substitute still PASSES down to original x pass_factor.
    // For a parameter whose spread is wider than the comparison (bead impedance
    // is a +/-25 % figure, measured differently on each side), a few percent
    // short is bookkeeping, not a regression.
    std::optional<double> pass_factor = std::nullopt;
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
// CLASS_MATCH over a JSON boolean: detail::jstr dumps true/false, so the rank
// map is keyed on those words. Used for AEC-Q200 qualification, where an
// equal-or-better substitute is one that is at least as qualified.
inline const ClassRank& automotive_rank() {
    static const ClassRank R = {{"true", 1}, {"false", 0}};
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

// The outcome of one parameter comparison. `verdict` is the reported vocabulary
// (pass/warn/fail/unverified) and converts implicitly from a bare verdict, so the
// comparators below read as they always did.
//
// `missing_required_sub` carries the one thing a verdict must NOT say: the
// original states the parameter, the substitute's record does not carry it at
// all, and the spec is one whose absence is disqualifying (exclude_missing_sub).
// That is not a FAIL — a FAIL asserts a comparison, and this comparison had no
// substitute-side operand — so it is reported UNVERIFIED, exactly as the same
// known-vs-absent situation is reported on every other spec. The disqualifying
// consequence (penalty, demotion, grade cap, and a note naming what must be
// confirmed) is applied from this flag instead, in CrossRef.hpp.
struct ParamOutcome {
    ParamOutcome(const char* v) : verdict(v) {}  // NOLINT(google-explicit-constructor)
    ParamOutcome(const char* v, bool missing) : verdict(v), missing_required_sub(missing) {}
    const char* verdict;
    bool missing_required_sub = false;
};

// Mirrors _compare_numeric: LOWER/HIGHER with abs_tol (additive) or tol_factor.
inline ParamOutcome compare_numeric(const ParamSpec& spec, std::optional<double> o,
                                    std::optional<double> s) {
    if (!o && !s) return UNVERIFIED;
    if (!s) return {UNVERIFIED, spec.exclude_missing_sub};
    if (!o) return UNVERIFIED;
    double ov = *o, sv = *s;
    if (spec.dir == Dir::Lower) {
        if (sv <= ov) return PASS;
        double ceiling = spec.abs_tol ? ov + *spec.abs_tol : ov * spec.tol_factor;
        return sv <= ceiling ? WARN : FAIL;
    }
    if (sv >= ov * spec.pass_factor.value_or(1.0)) return PASS;  // Higher
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
inline ParamOutcome compare_exact(const ParamSpec& spec, const nlohmann::json& orig,
                                  const nlohmann::json& sub) {
    bool o_pres = detail::present(orig, spec.key), s_pres = detail::present(sub, spec.key);
    if (!o_pres && !s_pres) return UNVERIFIED;
    if (!s_pres) return {UNVERIFIED, spec.exclude_missing_sub};
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
    // Neither side states a value (both absent or empty strings): unknown, not a
    // mismatch. present() counts "" as present, so without this a blank optional
    // identity field (a connector with no interface_standard) FAILs spuriously.
    if (on.empty() && sn.empty()) return UNVERIFIED;
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
// The span of drop criteria vendors quote Isat at (Würth 10 % / 30 %, Coilcraft/Vishay 20 %).
constexpr double kStrictestQuotedPercentDrop = 10.0;
constexpr double kLoosestQuotedPercentDrop = 30.0;

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

// I_sat interpolated to target_pct inductance drop; nullopt when no based points or when
// the target lies OUTSIDE the stated criteria. It used to clamp to the nearest stated point,
// which read a lone 10 %-drop rating as the part's 20 %-drop rating and compared it as such
// (ABT #2042): a rating at a criterion the record does not reach is not on record.
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
    return std::nullopt;
}

// A criterion BOTH sides can be read at: the canonical 20 % first, else any criterion one
// side states that the other states or brackets. nullopt when no such criterion exists —
// the two ratings are then of different (or unknown) basis and not directly comparable.
inline std::optional<double> common_percent_drop(const std::vector<Point>& op,
                                                 const std::vector<Point>& sp) {
    if (isat_at_percent_drop(op, kCanonicalPercentDrop) &&
        isat_at_percent_drop(sp, kCanonicalPercentDrop))
        return kCanonicalPercentDrop;
    std::vector<double> targets;
    for (const auto* side : {&op, &sp})
        for (const auto& p : *side)
            if (p.percent_drop) targets.push_back(*p.percent_drop);
    std::sort(targets.begin(), targets.end(), [](double a, double b) {
        return std::abs(a - kCanonicalPercentDrop) < std::abs(b - kCanonicalPercentDrop);
    });
    for (double t : targets)
        if (isat_at_percent_drop(op, t) && isat_at_percent_drop(sp, t)) return t;
    return std::nullopt;
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
    if (auto t = common_percent_drop(op, sp)) {
        const double o_at = *isat_at_percent_drop(op, *t), s_at = *isat_at_percent_drop(sp, *t);
        return (s_at >= margin * o_at) ? Verdict::Adequate : Verdict::Shortfall;
    }
    // One side's rating has no stated criterion, the other states a curve spanning the
    // criteria vendors quote (10 % to 30 % drop, see above). Isat rises with the drop
    // criterion, so wherever inside that span the unknown criterion lies, the based side's
    // Isat there is bracketed by its own stated points. DECISION (owner to confirm): an
    // unknown-basis rating is taken to sit inside the 10-30 % span; a criterion looser than
    // 30 % or stricter than 10 % is not assumed. Then:
    //  - unknown original vs based substitute: the substitute delivers at most its 30 % (or
    //    looser) figure; below margin x the original's figure is a shortfall
    //    (BWVS00606045150T00 3.06 A vs MSS6132T-123MLC 1.3/1.5/1.7 A at 10/20/30 % was
    //    warned through the 1.9x band, Qarlos 2026-11-08 seed 20300704);
    //  - based original vs unknown substitute: the original delivers at least its 10 % (or
    //    stricter) figure; a substitute rating below margin x that is a shortfall.
    // Everything else keeps the band below.
    auto highest_at_or_beyond = [](const std::vector<Point>& pts, double pct) {
        std::optional<double> best;
        for (const auto& p : pts)
            if (p.percent_drop && *p.percent_drop >= pct && (!best || p.current > *best))
                best = p.current;
        return best;
    };
    auto lowest_at_or_below = [](const std::vector<Point>& pts, double pct) {
        std::optional<double> best;
        for (const auto& p : pts)
            if (p.percent_drop && *p.percent_drop <= pct && (!best || p.current < *best))
                best = p.current;
        return best;
    };
    if (!has_basis(op) && has_basis(sp)) {
        const auto s_loosest = highest_at_or_beyond(sp, kLoosestQuotedPercentDrop);
        const auto o_raw = headline(op);
        if (s_loosest && o_raw && *s_loosest < margin * *o_raw) return Verdict::Shortfall;
    }
    if (has_basis(op) && !has_basis(sp)) {
        const auto o_strictest = lowest_at_or_below(op, kStrictestQuotedPercentDrop);
        const auto s_raw = headline(sp);
        if (o_strictest && s_raw && *s_raw < margin * *o_strictest) return Verdict::Shortfall;
    }
    auto o_raw = headline(op), s_raw = headline(sp);
    if (!o_raw || !s_raw) return Verdict::Uncertain;
    double ratio = *s_raw / *o_raw;
    if (ratio * kBasisUncertaintyBand < margin) return Verdict::Shortfall;
    return Verdict::Uncertain;
}
}  // namespace isat

// The original states no saturation current, only its rated (thermal) current. The two
// are different quantities and are NOT compared as equals: nothing here says what the
// original's Isat is. But they are both datasheet limits in amperes, and one ordering is
// still decidable. A circuit that runs the original at its rating draws at least that
// current at its peak (peak >= RMS), so a substitute that saturates BELOW the original's
// rated current is saturated at a current the original is specified to carry
// (PL8132NLT rated 2.7 A vs VLCF4020T-270MR48 saturating at 0.48 A was graded
// minor_review, Qarlos 2026-11-08). The reverse inference does not hold — an Isat at or
// above the rated current says nothing about the original's own Isat — so that stays
// UNVERIFIED, and so does the mirror case (substitute rated current vs original Isat:
// RMS may sit far below peak). The substitute is read at the MOST lenient criterion it
// states (its highest stated Isat), so a FAIL never rests on a strict 10 %-drop figure.
// Same tolerance as a like-for-like Isat compare: below rated x tol_factor fails, the band
// up to the rating warns. A comparison of two stated values; no saturation physics.
inline std::optional<double> isat_highest_stated(const std::vector<isat::Point>& pts) {
    std::optional<double> best;
    for (const auto& p : pts)
        if (!best || p.current > *best) best = p.current;
    return best;
}
inline ParamOutcome isat_against_original_rating(const ParamSpec& spec, const nlohmann::json& orig,
                                                 const std::vector<isat::Point>& sp) {
    const auto o_rated = detail::jnum(orig, "rated_current");
    const auto s_isat = isat_highest_stated(sp);
    if (!o_rated || !(*o_rated > 0) || !s_isat) return UNVERIFIED;
    if (*s_isat >= *o_rated) return UNVERIFIED;
    return (*s_isat >= spec.tol_factor * *o_rated) ? WARN : FAIL;
}
// True when the saturation_current verdict for this pair came from the rule above
// (original has no Isat, substitute's Isat sits below the original's rated current).
inline bool isat_below_original_rating(const nlohmann::json& orig, const nlohmann::json& sub) {
    if (!isat::resolve_points(orig).empty()) return false;
    const auto o_rated = detail::jnum(orig, "rated_current");
    const auto s_isat = isat_highest_stated(isat::resolve_points(sub));
    return o_rated && *o_rated > 0 && s_isat && *s_isat < *o_rated;
}

// saturation_current gate: %-drop-normalized when either side states a real
// inductance-drop basis; otherwise the raw HIGHER_BETTER numeric compare (legacy
// basis-unknown scalars — preserves existing pass/warn/fail granularity).
inline ParamOutcome compare_saturation_current(const ParamSpec& spec, const nlohmann::json& orig,
                                               const nlohmann::json& sub) {
    std::vector<isat::Point> op = isat::resolve_points(orig), sp = isat::resolve_points(sub);
    bool o_has = !op.empty(), s_has = !sp.empty();
    if (!o_has && !s_has) return UNVERIFIED;
    if (!s_has) return {UNVERIFIED, spec.exclude_missing_sub};
    if (!o_has) return isat_against_original_rating(spec, orig, sp);
    if (!isat::has_basis(op) && !isat::has_basis(sp))
        return compare_numeric(spec, detail::jnum(orig, spec.key), detail::jnum(sub, spec.key));
    switch (isat::compare(op, sp, spec.tol_factor)) {
        case isat::Verdict::Adequate:  return PASS;
        case isat::Verdict::Shortfall: return FAIL;
        case isat::Verdict::Uncertain: return WARN;
    }
    return WARN;  // unreachable
}

inline ParamOutcome compare_param(const ParamSpec& spec, const nlohmann::json& orig,
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
    // A varistor is matched on its varistor voltage (the primary value); what it
    // lets through at a surge must not rise, and the surge it survives must not fall.
    static const std::vector<ParamSpec> kVaristor = {
        {"clamping_voltage", D::Lower, 1.05, std::nullopt, false, true, nullptr},
        {"peak_surge_current", D::Higher, 0.9, std::nullopt, false, true, nullptr},
    };
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
        // A P-channel part is not a substitute for an N-channel one, whatever its ratings
        // (the ratings are magnitudes for both). Identity, so a mismatch rejects.
        {"channel", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
    };
    static const std::vector<ParamSpec> kDiode = {
        {"vf", D::Lower, 1.2, std::nullopt, false, false, nullptr},
        {"qrr", D::Lower, 2.0, std::nullopt, false, false, nullptr},
        {"trr", D::Lower, 2.0, std::nullopt, false, false, nullptr},
        // The breakdown-voltage grade: how wide a window the part guarantees around
        // its marked voltage. Same treatment as the resistor and capacitor tolerances
        // — tighter or equal passes, up to 2x warns, looser than that is a regression
        // — because for a zener the window is what the circuit is designed around, and
        // the nominal compare (Vrrm) cannot see it. Not exclude_missing_sub: a
        // candidate whose record omits the band is UNKNOWN, and the honest report of
        // an unknown is UNVERIFIED, not a penalty for the vendor's silence (ABT #488).
        {"vz_tolerance_pct", D::Lower, 2.0, std::nullopt, false, false, nullptr},
        // Continuous power-dissipation rating (Qarlos 2026-11-08: a 0.3 W zener came back
        // drop_in for a 0.5 W one). Equal-or-higher passes, down to 90 % warns, lower
        // fails — the resistor power-rating rule. An absent value is UNVERIFIED.
        {"power_dissipation", D::Higher, 0.9, std::nullopt, false, false, nullptr},
    };
    static const std::vector<ParamSpec> kResistor = {
        {"power_rating", D::Higher, 0.9, std::nullopt, false, false, nullptr},
        {"tolerance_pct", D::Lower, 2.0, std::nullopt, false, false, nullptr},
        {"tcr", D::Lower, 2.0, std::nullopt, true, false, nullptr},
        // Operating range (Qarlos 2026-11-08: RNC55K1472FSB14, rated to 175 degC, took
        // 155 degC metal films as a clean pass). The substitute must cover the original's
        // range at both ends with NO warn band (abs_tol 0): a resistor that stops at 155 degC
        // is not rated at 160 degC however close the numbers look. Absent -> UNVERIFIED.
        {"temp_min_C", D::Lower, 0.0, 0.0, false, false, nullptr},
        {"temp_max_C", D::Higher, 0.0, 0.0, false, false, nullptr},
    };
    static const std::vector<ParamSpec> kMagnetic = {
        {"saturation_current", D::Higher, 0.9, std::nullopt, false, true, nullptr},
        {"dcr", D::Lower, 1.3, std::nullopt, false, false, nullptr},
        {"rated_current", D::Higher, 0.9, std::nullopt, false, false, nullptr},
    };
    static const std::vector<ParamSpec> kChipBead = {
        // exclude_missing_sub: impedance IS what a bead is. A candidate with no
        // impedance data cannot be verified as a substitute for one, so its
        // absence is disqualifying rather than passing silently — otherwise a
        // part with no curve at all accrues no penalty and outranks genuinely
        // curve-matched parts, which is exactly the "missing data ranks best"
        // pathology.
        // pass_factor 0.85 matches the bead's primary-value rule (within 15 % IS
        // the same bead); below that it warns, below 80 % it fails.
        {"impedance_100mhz", D::Higher, 0.8, std::nullopt, false, true, nullptr, 0.85},
        // Peak |Z| and — critically — the frequency it occurs at. Two beads with
        // the same Z@100MHz can peak several-fold apart in height and in
        // frequency, which is what decides whether the part suppresses the noise
        // actually present. The peak FREQUENCY is a shape parameter, so a shift
        // in either direction matters: ExactMatch with a 30% band, wide enough
        // to tolerate curve-sampling differences and narrow enough to separate
        // parts that work in genuinely different bands.
        // Also exclude-if-missing: peak |Z| and its frequency are the curve. A
        // candidate carrying neither the 100 MHz value NOR a curve takes both
        // penalties and therefore sorts below one that carries the curve and
        // matches it — otherwise the part we know least about wins on having
        // accrued the fewest penalties.
        {"impedance_peak", D::Higher, 0.8, std::nullopt, false, true, nullptr},
        {"impedance_peak_freq", D::ExactMatch, 0.3, std::nullopt, false, false, nullptr},
        {"srf", D::Higher, 0.8, std::nullopt, false, false, nullptr},
        {"dcr", D::Lower, 1.3, std::nullopt, false, false, nullptr},
        {"rated_current", D::Higher, 0.9, std::nullopt, false, false, nullptr},
        // Qualification grade. Two beads can agree on every parameter above and
        // still not be interchangeable: an AEC-Q200 part and a general-grade one
        // differ in qualification, not in physics, and the grade is often the
        // whole reason a specific part is on the BOM. A downgrade FAILs; an
        // unknown grade on the substitute is UNVERIFIED, which the caller treats
        // as demoting rather than passing (ABT #884).
        {"automotive", D::ClassMatch, 0.0, std::nullopt, false, false, &automotive_rank()},
    };
    static const std::vector<ParamSpec> kConnector = {
        {"family", D::ExactMatch, 0.0, std::nullopt, false, true, &connector_family_rank()},
        {"positions", D::ExactMatch, 0.0, std::nullopt, false, true, nullptr},
        {"polarity", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        {"pitch_mm", D::ExactMatch, 0.02, std::nullopt, false, false, nullptr},
        {"interface_standard", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        // No "mounting" key: the board attach is decided by the shared mount gate
        // (compare_mount in CrossRefDimensions.hpp), the one mount comparison every
        // family goes through — a second copy here pushed two verdicts and two notes.
        {"rated_current_A", D::Higher, 0.9, std::nullopt, false, true, nullptr},
        {"rated_voltage_V", D::Higher, 0.9, std::nullopt, false, false, nullptr},
        {"temp_min_C", D::Lower, 0.0, 15.0, false, false, nullptr},
        {"temp_max_C", D::Higher, 0.0, 15.0, false, false, nullptr},
        // Identity, not equal-or-better: the finish on the separable surface is a
        // compatibility property of the PAIR, so a change is a change in both directions
        // — a tin substitute cannot be mated to the gold counterpart the original was
        // chosen for, and a gold one cannot be mated to a tin counterpart either.
        {"contact_plating", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        {"termination", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
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
        // An active oscillator runs from the board's rail, and the board's rail is
        // whatever the ORIGINAL's stated supply range allows: Kelvin cannot see which
        // voltage inside it the design actually uses. So the substitute's range must
        // COVER the original's at both ends, with no tolerance band — a 3.3 V-only XO
        // does not start on a 2.5 V rail, and a 1.8 V-only one is overstressed on it.
        // The analog family's 0.3 V / 10 % warn bands were copied here and let a
        // 2.7 V-minimum part pass as a warning for a 2.5 V original. A shortfall is a
        // FAIL (a different rail is a board change); an unstated range is UNVERIFIED.
        // Not applied to passive resonators (quartzCrystal / ceramicResonator), which
        // have no supply — CrossRef.hpp skips both keys for them.
        {"supply_min_V", D::Lower, 0.0, 0.0, false, false, nullptr},
        {"supply_max_V", D::Higher, 0.0, 0.0, false, false, nullptr},
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
    if (category == "varistor") return kVaristor;
    return kEmpty;
}

// One report per parameter that has data on at least one side.
//
// `missing_required_sub` rides along rather than being folded into the verdict for the
// reason ParamOutcome gives: the verdict vocabulary must not claim a comparison that had
// no substitute-side operand. But a caller that reports verdicts and NOTHING else has no
// way to see the disqualification either, and UNVERIFIED on its own reads as benign — so
// the flag has to reach whoever is deciding, not just whoever is printing.
struct ParamReport {
    std::string key;
    std::string verdict;
    bool missing_required_sub = false;
};

inline std::vector<ParamReport> evaluate_params(const std::string& category,
                                                const nlohmann::json& orig,
                                                const nlohmann::json& sub) {
    std::vector<ParamReport> out;
    for (const ParamSpec& spec : params_for(category)) {
        bool has_data = detail::present(orig, spec.key) || detail::present(sub, spec.key);
        if (spec.key == "saturation_current")  // a multi-point table counts as data too
            has_data = has_data || detail::present(orig, "saturation_points") ||
                       detail::present(sub, "saturation_points");
        if (!has_data) continue;
        const ParamOutcome outcome = compare_param(spec, orig, sub);
        out.push_back({spec.key, outcome.verdict, outcome.missing_required_sub});
    }
    return out;
}

// How a spec key is spelled in a note an engineer reads. Only the keys whose
// bare form reads badly are listed; anything else is the key with its
// underscores opened out (dcr, srf, positions, …).
inline std::string param_label(const std::string& key) {
    static const std::vector<std::pair<std::string, std::string>> kLabels = {
        {"esr", "ESR"},
        {"ripple_current", "RMS ripple current rating"},
        {"saturation_current", "saturation current"},
        {"impedance_100mhz", "impedance at 100 MHz"},
        {"impedance_peak", "peak impedance"},
        {"impedance_peak_freq", "peak impedance frequency"},
        {"positions", "position count"},
        {"channels", "channel count"},
        {"rated_current_A", "rated current"},
        {"rated_voltage_V", "rated voltage"},
        {"vz_tolerance_pct", "breakdown-voltage tolerance"},
    };
    for (const auto& [k, label] : kLabels)
        if (k == key) return label;
    std::string out = key;
    std::replace(out.begin(), out.end(), '_', ' ');
    return out;
}

}  // namespace kelvin::crossref
