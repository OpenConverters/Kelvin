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
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "CrossRefClasses.hpp"  // dielectric code decoding
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
    // abs_tol specs only: the substitute still PASSES within this additive margin the wrong way
    // (an insertion-loss curve within 1 dB of the original's, owner 2026-11-14), the abs_tol
    // band beyond it is the WARN band. Either direction.
    std::optional<double> pass_abs = std::nullopt;
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
// EXACT_MATCH equivalence group for an oscillator's control pin (TDAS
// electrical.enableFunction). Owner decision 2026-11-09: "outputEnable" and "triState"
// are ONE pin function: both keep the oscillator running and put the output in high
// impedance when the pin is pulled low, so a board wired for one works with the other.
// "standby" (powers the device down; restart costs the start-up time) and "none" (no
// control pin) are not in the group, so each still rejects against every other value.
inline const ClassRank& enable_function_rank() {
    static const ClassRank R = {{"outputenable", 1}, {"tristate", 1}};
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
//
// `warn_depth` is set on a NUMERIC WARN only (compare_numeric): how far the substitute sits
// past the pass limit toward the fail limit, 0 at the pass limit and 1 at the fail limit
// (owner, 2026-11-13: a WARN's penalty grows with its deviation, see warn_penalty in
// CrossRef.hpp). Every other verdict, and a WARN that is not a position between two numeric
// limits, leaves it empty.
struct ParamOutcome {
    ParamOutcome(const char* v) : verdict(v) {}  // NOLINT(google-explicit-constructor)
    ParamOutcome(const char* v, bool missing) : verdict(v), missing_required_sub(missing) {}
    const char* verdict;
    bool missing_required_sub = false;
    std::optional<double> warn_depth = std::nullopt;
};

// Where `value` sits between a pass limit (0) and a fail limit (1), for a value already
// judged WARN (past the pass limit, not past the fail limit). Works for either direction:
// the fraction is (value - pass) / (fail - pass). Clamped to [0, 1] so the representation
// epsilon at either boundary cannot push it outside.
inline std::optional<double> warn_depth_between(double value, double pass_limit,
                                                double fail_limit) {
    const double span = fail_limit - pass_limit;
    if (span == 0.0) return std::nullopt;  // no band between the limits: nothing to scale on
    return std::clamp((value - pass_limit) / span, 0.0, 1.0);
}

// Two catalogue numbers that differ only in their last bits are the same stated value.
// The records carry decimal datasheet figures through binary doubles and unit scaling,
// so one vendor's 33 nC arrives as 3.3e-08 and another's as 3.3000000000000004e-08, and
// a 20 ppm stability as 1.9999999999999998e-05. A strict `sv <= ov` read the second Qg
// as HIGHER than the first and warned (SSM3J132TU vs SiA477EDJT, Qarlos 2026-11-09,
// seed 20889483). Relative 1e-9 is far below any datasheet resolution (a 1 ppm crystal
// tolerance is 1e-6 of the frequency) and far above double rounding (~1e-16), so it only
// ever merges representations of one number, never two real values.
constexpr double kRepresentationEpsilon = 1e-9;
inline bool same_stated_value(double a, double b) {
    if (a == b) return true;
    return std::abs(a - b) <= kRepresentationEpsilon * std::max(std::abs(a), std::abs(b));
}
// Room temperature (owner 2026-11-14): 20 degC and 25 degC are the same measurement
// condition, in every family — datasheets quote "room temperature" at either (Würth states
// 20 degC where Coilcraft states 25 degC for the same bench measurement). One definition for
// every caller: curve conditions (conditions_differ), a magnetic's L(I) / Isat curve
// temperatures and its rated-current ambient. Two stated temperatures name the same condition
// when they are the same stated value or both room temperature. Both values must be stated:
// a temperature stated on one side only is a different condition, and case vs ambient is the
// caller's to tell apart — the one exception is a magnetic's rated-current ambient, where an
// unstated ambient is read as room temperature (owner 2026-11-14,
// magcurve::rated_at_common_rise).
inline bool is_room_temperature(double t) {
    return same_stated_value(t, 20.0) || same_stated_value(t, 25.0);
}
inline bool same_room_temperature(double a, double b) {
    if (same_stated_value(a, b)) return true;
    return is_room_temperature(a) && is_room_temperature(b);
}

// Mirrors _compare_numeric: LOWER/HIGHER with abs_tol (additive) or tol_factor.
// Equal (within the representation epsilon above) always passes, in either direction.
inline ParamOutcome compare_numeric(const ParamSpec& spec, std::optional<double> o,
                                    std::optional<double> s) {
    if (!o && !s) return UNVERIFIED;
    if (!s) return {UNVERIFIED, spec.exclude_missing_sub};
    if (!o) return UNVERIFIED;
    double ov = *o, sv = *s;
    if (same_stated_value(sv, ov)) return PASS;
    if (spec.pass_abs && !spec.abs_tol)
        throw std::logic_error("compare_numeric: spec '" + spec.key +
                               "' has an additive pass margin but no additive band");
    if (spec.dir == Dir::Lower) {
        const double pass_ceiling = spec.pass_abs ? ov + *spec.pass_abs : ov;
        if (sv <= pass_ceiling) return PASS;
        double ceiling = spec.abs_tol ? ov + *spec.abs_tol : ov * spec.tol_factor;
        if (sv > ceiling) return FAIL;
        ParamOutcome warn(WARN);
        warn.warn_depth = warn_depth_between(sv, pass_ceiling, ceiling);
        return warn;
    }
    const double pass_floor =
        spec.pass_abs ? ov - *spec.pass_abs : ov * spec.pass_factor.value_or(1.0);
    if (sv >= pass_floor) return PASS;  // Higher
    double floor = spec.abs_tol ? ov - *spec.abs_tol : ov * spec.tol_factor;
    if (sv < floor) return FAIL;
    ParamOutcome warn(WARN);
    warn.warn_depth = warn_depth_between(sv, pass_floor, floor);
    return warn;
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

// ── Isat read on two L(I) curves (owner 2026-11-13, Z5) ──────────────────────────────────
// When BOTH records state an inductance-vs-bias curve (spec bias_curve: {current: [A],
// ratio: [L / L(0)]}, MagneticRow.bias_curve_*), Isat is compared on the curves, like for
// like, instead of on the bare saturationCurrentPeak figures, which are quoted at criteria
// that need not match: at the original's stated drop criterion(s) when its record states one
// (a saturation table it STATES, not one read off its curve), otherwise at 10, 20 and 30 %
// drop, and graded on the worst criterion. Each current is the FIRST point the curve falls to
// (1 - drop) x L(0), interpolated between the two measured points that bracket it — the
// rule magnetic_saturation_from_bias_curve uses — and never extrapolated: a drop the curve
// does not reach inside its measured range is UNVERIFIED for that criterion. Per criterion the
// like-for-like rule of compare(): substitute >= margin x original -> PASS, else FAIL.
// Worst: FAIL, then UNVERIFIED, then PASS.
struct BiasCurve {
    std::vector<double> current, ratio;
    std::optional<double> temperature;  // degC the curve is stated at (owner 2026-11-14)
};
inline std::optional<BiasCurve> bias_curve(const nlohmann::json& s) {
    if (!s.is_object()) return std::nullopt;
    auto it = s.find("bias_curve");
    if (it == s.end() || !it->is_object()) return std::nullopt;
    auto ci = it->find("current"), ri = it->find("ratio");
    if (ci == it->end() || ri == it->end() || !ci->is_array() || !ri->is_array() ||
        ci->size() != ri->size() || ci->size() < 2)
        return std::nullopt;
    BiasCurve c;
    for (size_t k = 0; k < ci->size(); ++k) {
        const auto& a = (*ci)[k];
        const auto& b = (*ri)[k];
        if (!a.is_number() || !b.is_number()) return std::nullopt;
        c.current.push_back(a.get<double>());
        c.ratio.push_back(b.get<double>());
    }
    if (c.current.front() != 0.0) return std::nullopt;  // no small-signal reference
    if (auto t = it->find("temperature"); t != it->end() && t->is_number())
        c.temperature = t->get<double>();
    return c;
}
inline std::optional<double> curve_current_at_drop(const BiasCurve& c, double drop_pct) {
    const double threshold = 1.0 - drop_pct / 100.0;
    for (size_t k = 1; k < c.current.size(); ++k) {
        if (!(c.ratio[k] <= threshold)) continue;
        if (!(c.ratio[k - 1] > threshold) || !(c.current[k] > c.current[k - 1]))
            return std::nullopt;  // undecidable
        const double frac = (c.ratio[k - 1] - threshold) / (c.ratio[k - 1] - c.ratio[k]);
        return c.current[k - 1] + frac * (c.current[k] - c.current[k - 1]);
    }
    return std::nullopt;  // not reached inside the measured range
}
struct CurveCriterion {
    double drop_pct;
    std::optional<double> orig_at, sub_at;
    const char* verdict;
};
struct CurveCompare {
    bool applies = false;
    bool stated_criterion = false;  // read at the original's stated criterion(s)
    std::vector<CurveCriterion> rows;
    const char* verdict = PASS;
    std::optional<size_t> decisive;  // the worst criterion with both currents read
    // Both records state a curve but at different temperatures (or only one states its
    // temperature): the curves are not read against each other (owner 2026-11-14) and this
    // says why; the stated Isat figures are compared instead.
    std::string not_applied_note;
};
inline CurveCompare compare_curves(const nlohmann::json& orig, const nlohmann::json& sub,
                                   double margin) {
    CurveCompare out;
    const auto oc = bias_curve(orig), sc = bias_curve(sub);
    if (!oc || !sc) return out;
    if (oc->temperature.has_value() != sc->temperature.has_value() ||
        (oc->temperature && !same_room_temperature(*oc->temperature, *sc->temperature))) {
        auto t = [](const std::optional<double>& v) {
            if (!v) return std::string("no stated temperature");
            char b[32];
            std::snprintf(b, sizeof b, "%g degC", *v);
            return std::string(b);
        };
        out.not_applied_note = "L(I) curves stated at different temperatures (original " +
                               t(oc->temperature) + ", substitute " + t(sc->temperature) +
                               ") — Isat not read off the two curves; the stated figures are "
                               "compared instead";
        return out;
    }
    out.applies = true;
    std::vector<double> drops;
    const bool from_curve = orig.is_object() && orig.value("saturation_from_curve", false);
    if (!from_curve)
        for (const auto& p : coerce_points(orig.value("saturation_points", nlohmann::json())))
            if (p.percent_drop && *p.percent_drop > 0 && *p.percent_drop < 100 &&
                std::find(drops.begin(), drops.end(), *p.percent_drop) == drops.end())
                drops.push_back(*p.percent_drop);
    out.stated_criterion = !drops.empty();
    if (drops.empty()) drops = {10.0, 20.0, 30.0};
    std::sort(drops.begin(), drops.end());
    auto rank = [](const char* v) {
        return std::string(v) == FAIL ? 2 : std::string(v) == UNVERIFIED ? 1 : 0;
    };
    double worst_ratio = 0;
    for (double d : drops) {
        CurveCriterion row{d, curve_current_at_drop(*oc, d), curve_current_at_drop(*sc, d),
                           UNVERIFIED};
        if (row.orig_at && row.sub_at)
            row.verdict = *row.sub_at >= margin * *row.orig_at ? PASS : FAIL;
        if (rank(row.verdict) > rank(out.verdict)) out.verdict = row.verdict;
        if (row.orig_at && row.sub_at) {
            const double ratio = *row.sub_at / *row.orig_at;
            if (!out.decisive || ratio < worst_ratio) {
                out.decisive = out.rows.size();
                worst_ratio = ratio;
            }
        }
        out.rows.push_back(row);
    }
    return out;
}
// ── Isat read on ONE L(I) curve against the other side's stated criterion ─────────────────
// The owner curve rule (2026-11-14) as the Coss / Vf / Cj basis checks apply it: a stated
// curve resolves a basis mismatch. Isat had only the two-curve case (Z5 above), so a
// substitute whose record carries an L(I) curve but quotes its Isat figure at no criterion was
// "not directly comparable" with an original rated 1.3 A at 35 % drop, and graded only WARN,
// although its own curve falls 35 % at 0.65 A (VLS3012CX-220M-1 / VLS4012CX-220M-1 for
// SCRH6D38-220, Qarlos r33 seed 35761001). Here exactly one record states a curve and the
// other states its Isat at one or more drop criteria: the curve is read at each stated
// criterion — the first point it falls to (1 - drop) x L(0), interpolated, never extrapolated
// (curve_current_at_drop) — and set against the stated current. Only a curve stated at room
// temperature is read, against stated points at room temperature or at none (a scalar that
// states no temperature is taken as 25 degC, owner 2026-11-14, as for Vf). Per criterion
// substitute >= margin x original -> PASS, else FAIL; a criterion the curve does not reach is
// UNVERIFIED; worst wins, as in compare_curves. No criterion read -> not applied (the stated
// figures are compared as before).
struct OneCurveCompare {
    bool applies = false;
    bool curve_on_substitute = false;
    std::vector<CurveCriterion> rows;
    const char* verdict = PASS;
    std::optional<size_t> decisive;
};
inline OneCurveCompare compare_one_curve(const nlohmann::json& orig, const nlohmann::json& sub,
                                         double margin) {
    OneCurveCompare out;
    const auto oc = bias_curve(orig), sc = bias_curve(sub);
    if (oc.has_value() == sc.has_value()) return out;  // none, or both (compare_curves)
    const BiasCurve& curve = oc ? *oc : *sc;
    const nlohmann::json& stated_side = oc ? sub : orig;
    if (!curve.temperature || !is_room_temperature(*curve.temperature)) return out;
    if (stated_side.is_object() && stated_side.value("saturation_from_curve", false)) return out;
    auto pts = stated_side.is_object() ? stated_side.find("saturation_points")
                                       : nlohmann::json::const_iterator{};
    if (!stated_side.is_object() || pts == stated_side.end() || !pts->is_array()) return out;
    std::vector<std::pair<double, double>> stated;  // (drop %, current A)
    for (const auto& p : *pts) {
        if (!p.is_object()) continue;
        const auto c = detail::jnum(p, "current"), d = detail::jnum(p, "percent_drop");
        if (!c || !(*c > 0) || !d || !(*d > 0) || !(*d < 100)) continue;
        if (const auto t = detail::jnum(p, "temperature"); t && !is_room_temperature(*t))
            continue;
        bool seen = false;
        for (const auto& e : stated) seen = seen || e.first == *d;
        if (!seen) stated.emplace_back(*d, *c);
    }
    if (stated.empty()) return out;
    std::sort(stated.begin(), stated.end());
    out.curve_on_substitute = sc.has_value();
    auto rank = [](const char* v) {
        return std::string(v) == FAIL ? 2 : std::string(v) == UNVERIFIED ? 1 : 0;
    };
    double worst_ratio = 0;
    bool any_read = false;
    for (const auto& [d, c] : stated) {
        const auto read = curve_current_at_drop(curve, d);
        CurveCriterion row{d, std::nullopt, std::nullopt, UNVERIFIED};
        row.orig_at = oc ? read : std::optional<double>(c);
        row.sub_at = sc ? read : std::optional<double>(c);
        if (row.orig_at && row.sub_at) {
            any_read = true;
            row.verdict = *row.sub_at >= margin * *row.orig_at ? PASS : FAIL;
            const double ratio = *row.sub_at / *row.orig_at;
            if (!out.decisive || ratio < worst_ratio) {
                out.decisive = out.rows.size();
                worst_ratio = ratio;
            }
        }
        if (rank(row.verdict) > rank(out.verdict)) out.verdict = row.verdict;
        out.rows.push_back(row);
    }
    out.applies = any_read;
    return out;
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
    // Both records state an L(I) curve: compared on the curves (owner 2026-11-13, Z5).
    if (const auto cc = isat::compare_curves(orig, sub, spec.tol_factor); cc.applies)
        return cc.verdict;
    // One record states a curve, the other a stated criterion: the curve is read there.
    if (const auto oc = isat::compare_one_curve(orig, sub, spec.tol_factor); oc.applies)
        return oc.verdict;
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

// dielectric_code: when both codes DECODE, the verdict is the decoded comparison — the class-1
// coefficient (class1_tc_regression) or the EIA RS-198 envelope (dielectric_regression), the
// same rules that write the note in CrossRef.hpp, so the verdict and the note cannot disagree.
// The substring rank map could not see N750 at all and left C0G -> N750 'unverified'
// (Qarlos 2026-11-09, seed 21517857), and it passed X7R -> X7S on an equal rank while the
// envelope note called it a regression. Undecodable codes keep the rank compare.
inline ParamOutcome compare_dielectric_code(const ParamSpec& spec, const std::string& o_raw,
                                            const std::string& s_raw) {
    const std::string o = dielectric_key(o_raw), s = dielectric_key(s_raw);
    if (!o.empty() && !s.empty()) {
        if (class1_tc(o) && class1_tc(s)) {
            const auto r = class1_tc_regression(o, s);
            if (!r) return UNVERIFIED;
            return r->empty() ? PASS : FAIL;
        }
        if (dielectric_envelope(o) && dielectric_envelope(s))
            return dielectric_regression(o, s).empty() ? PASS : FAIL;
    }
    return compare_class(spec, o_raw, s_raw);
}

inline ParamOutcome compare_param(const ParamSpec& spec, const nlohmann::json& orig,
                                  const nlohmann::json& sub) {
    if (spec.key == "saturation_current") return compare_saturation_current(spec, orig, sub);
    if (spec.key == "dielectric_code")
        return compare_dielectric_code(spec, detail::jstr(orig, spec.key),
                                       detail::jstr(sub, spec.key));
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
        // The maximum continuous DC / AC working voltages and the energy rating are hard
        // critical ratings (critical_ratings("varistor"), CrossRef.hpp; owner 2026-11-15), not
        // rows of this table.
        // Capacitance (owner 2026-11-15): the load the part puts on the line it protects.
        // Lower is better; the diode junction-capacitance band (up to 2x warns, beyond FAILs).
        // The records state no measurement frequency / bias for it, so it is compared as stated.
        {"capacitance", D::Lower, 2.0, std::nullopt, false, false, nullptr},
    };
    // An IGBT's ratings Vces / Ic are critical_ratings (CrossRef.hpp). The table grades the
    // conduction drop and the junction limit (owner 2026-11-15), on the diode's conventions:
    // Vce(sat) like the diode Vf (lower is better, up to 1.2x warns), Tj max like the diode
    // tj_max (the substitute must reach it, no band). Vce(sat) is compared when both records
    // state the same collector current (vce_sat_ic) or neither states one; otherwise
    // CrossRef.hpp reports it UNVERIFIED (owner 2026-11-15, the diode Vf rule).
    static const std::vector<ParamSpec> kIgbt = {
        {"vce_sat", D::Lower, 1.2, std::nullopt, false, false, nullptr},
        {"tj_max", D::Higher, 0.0, 0.0, false, false, nullptr},
    };
    // A BJT (owner 2026-11-15), on the IGBT / MOSFET pattern: Vceo / Ic are critical_ratings
    // (CrossRef.hpp); the polarity is an identity like the MOSFET channel (an NPN is never a
    // PNP's substitute); the guaranteed minimum gain warns down to 0.7x, fT too (the GBW
    // band), the power rating down to 0.9x; Vce(sat) and Tj max as for the IGBT (Vce(sat)
    // compared on a common collector current or when neither states one, CrossRef.hpp).
    static const std::vector<ParamSpec> kBjt = {
        {"polarity", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        {"hfe_min", D::Higher, 0.7, std::nullopt, false, false, nullptr},
        {"transition_frequency", D::Higher, 0.7, std::nullopt, false, false, nullptr},
        {"power_dissipation", D::Higher, 0.9, std::nullopt, false, false, nullptr},
        {"vce_sat", D::Lower, 1.2, std::nullopt, false, false, nullptr},
        {"tj_max", D::Higher, 0.0, 0.0, false, false, nullptr},
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
        // Maximum junction temperature (Qarlos 2026-11-09, seeds 20575296 / 20889483: a
        // 125 degC Bourns Schottky took a 175 / 150 degC Vishay / onsemi part clean, one even
        // 'upgrade'). The substitute must reach the original's Tj max with NO band (abs_tol
        // 0): the design's thermal margin was computed against it. Lower = FAIL, absent =
        // UNVERIFIED (CrossRef.hpp names both temperatures, or the gap, in a note).
        {"tj_max", D::Higher, 0.0, 0.0, false, false, nullptr},
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
        // Maximum continuous working voltage (Qarlos r10 seed 23612437 / r8 seed 22565147: an
        // 800 V KOA HV73V and a 500 V CMF60 took substitutes with no voltage verdict, the row
        // never carried the rating). The band of the resistor power rating and the connector
        // rated voltage: equal or higher passes, down to 90 % warns, lower FAILs. Absent ->
        // UNVERIFIED and noted (CrossRef.hpp), no cap.
        {"max_voltage_V", D::Higher, 0.9, std::nullopt, false, false, nullptr},
    };
    static const std::vector<ParamSpec> kMagnetic = {
        {"saturation_current", D::Higher, 0.9, std::nullopt, false, true, nullptr},
        {"dcr", D::Lower, 1.3, std::nullopt, false, false, nullptr},
        {"rated_current", D::Higher, 0.9, std::nullopt, false, false, nullptr},
        // Operating range (Qarlos 2026-11-09, seed 20680025: XT26008-T-K, rated -55..200 degC,
        // took 125 degC chip inductors with no caveat). The resistor rule: the substitute must
        // cover the original's range at both ends with NO warn band (abs_tol 0) — a lower
        // maximum or a warmer minimum is a FAIL. Absent -> UNVERIFIED and noted (a secondary
        // axis: no grade cap, owner 2026-11-09).
        {"temp_min_C", D::Lower, 0.0, 0.0, false, false, nullptr},
        {"temp_max_C", D::Higher, 0.0, 0.0, false, false, nullptr},
        // Self-resonant frequency (Qarlos 2026-11-09, seed 21098941: AIML-0402HP-R20K-T, SRF
        // 200 MHz, graded drop_in for a 900 MHz MHQ1005PR20GTD25 — the magnetic table had no
        // SRF entry, so the 4.5x drop was never compared). Above its SRF the part is a
        // capacitor, and well below it the inductance already departs from its nominal, so a
        // substitute resonating well below the original is a downgrade at the working
        // frequency. Band: the chip bead's existing SRF spec (below), the one SRF rule Kelvin
        // already had — at or above the original's passes, down to 0.8x warns, below 0.8x
        // FAILs. A higher SRF is never a regression.
        // Pass band (owner, 2026-11-12, Qarlos bench 20470567: 2.00 vs 2.01 MHz warned): down
        // to 0.95x of the original still PASSES — a few percent is datasheet rounding of a
        // typical figure, not a regression. 0.8x-0.95x warns, below 0.8x fails.
        // One side unstated -> UNVERIFIED, uncapped: most power-inductor records state none.
        {"srf", D::Higher, 0.8, std::nullopt, false, false, nullptr, 0.95},
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
        // Operating range: the magnetic table's rule (above), because a bead or a signal CMC
        // is a part of the same magnetic catalogue ranked by its impedance instead of its
        // inductance. MFBA3V1608K-471-R (-55..150 degC) took MMZ1608Y471BTA00 (to 125 degC)
        // as 'upgrade' with nothing said: the bead table carried no temperature at all
        // (Qarlos r9 seed 23088792). A lower maximum or a warmer minimum is a FAIL; absent
        // -> UNVERIFIED and noted (secondary axis: no grade cap, owner 2026-11-09).
        {"temp_min_C", D::Lower, 0.0, 0.0, false, false, nullptr},
        {"temp_max_C", D::Higher, 0.0, 0.0, false, false, nullptr},
    };
    static const std::vector<ParamSpec> kConnector = {
        {"family", D::ExactMatch, 0.0, std::nullopt, false, true, &connector_family_rank()},
        {"positions", D::ExactMatch, 0.0, std::nullopt, false, true, nullptr},
        {"polarity", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        {"pitch_mm", D::ExactMatch, 0.02, std::nullopt, false, false, nullptr},
        {"interface_standard", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        // Family-variant identities (owner 2026-11-12, "Identity when stated"): the
        // structured CONAS familyFpcFfc.actuatorType and familyCircular.couplingType /
        // .coding / .shellSize, judged exactly like interface_standard — a stated
        // difference rejects (is_hard_param), one side only is UNVERIFIED (noted in
        // CrossRef.hpp, no cap), neither side is not compared. Coupling is checked before
        // shell size: in the catalogue a threaded and a bayonet circular never share a
        // shell code, so with shell size first the coupling difference would never be named.
        {"actuator_type", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        {"coupling_type", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        {"coding", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        {"shell_size", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        // No "mounting" key: the board attach is decided by the shared mount gate
        // (compare_mount in CrossRefDimensions.hpp), the one mount comparison every
        // family goes through — a second copy here pushed two verdicts and two notes.
        {"rated_current_A", D::Higher, 0.9, std::nullopt, false, true, nullptr},
        {"rated_voltage_V", D::Higher, 0.9, std::nullopt, false, false, nullptr},
        // The original's operating range must be covered at both ends with NO band (owner
        // 2026-11-14; was a 15 degC WARN band): any shortfall is a FAIL, as in the other
        // families (Qarlos r28: ABM13N-48.0000MHZ-8-D1X-T5, -40 degC, took 830108049101,
        // -30 degC, as a WARN).
        {"temp_min_C", D::Lower, 0.0, 0.0, false, false, nullptr},
        {"temp_max_C", D::Higher, 0.0, 0.0, false, false, nullptr},
        // Identity, not equal-or-better: the finish on the separable surface is a
        // compatibility property of the PAIR, so a change is a change in both directions
        // — a tin substitute cannot be mated to the gold counterpart the original was
        // chosen for, and a gold one cannot be mated to a tin counterpart either.
        {"contact_plating", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        {"termination", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        {"mating_cycles", D::Higher, 0.5, std::nullopt, false, false, nullptr},
        {"contact_resistance", D::Lower, 2.0, std::nullopt, false, false, nullptr},
    };
    // No "package" row (owner 2026-11-15): the package fit is the shared footprint / mount
    // gate on the case code, as for every semiconductor; an exact string match failed
    // "SOIC" against "SOIC-8".
    static const std::vector<ParamSpec> kAnalog = {
        {"subtype", D::ExactMatch, 0.0, std::nullopt, false, true, nullptr},
        {"channels", D::ExactMatch, 0.0, std::nullopt, false, true, nullptr},
        // An ADC's conversion architecture (AAS adc electrical.architecture: SAR / deltaSigma /
        // pipeline / flash / dualSlope / folding) is an identity (Qarlos 2026-11-20): a 65 MSPS
        // pipeline AD9228 for a SAR MAX1305 has a different latency, input structure and
        // clocking, and a delta-sigma converter (AD7150, a capacitance-to-digital converter its
        // record files as an adc) is no drop-in for a pipeline one. A hard key: both stated and
        // different rejects; one side unstated is UNVERIFIED, noted and capped below drop_in
        // (CrossRef.hpp). Compared for ADC originals only — on the other analog subtypes the
        // field is stated for a few special cases (zeroDrift / currentFeedback op-amps) and its
        // absence there says nothing.
        {"architecture", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
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
        // A switch / multiplexer's on-resistance (owner 2026-11-15): lower is better, the
        // MOSFET Rds(on) band (up to 1.5x warns, beyond FAILs).
        {"on_resistance", D::Lower, 1.5, std::nullopt, false, false, nullptr},
    };
    static const std::vector<ParamSpec> kTimeBase = {
        {"subtype", D::ExactMatch, 0.0, std::nullopt, false, true, nullptr},
        {"technology", D::ExactMatch, 0.0, std::nullopt, false, true, nullptr},
        // The nominal frequency is the part's identity, not a value with a tolerance: a
        // 25.00125 MHz oscillator is a different part from a 25 MHz one, whatever its
        // stability. The old 1e-4 relative band (100 ppm) passed SG-8018CA 25.001250M
        // (+50 ppm) and 24.998700M (-52 ppm) for a 25 MHz ECS-3225SMVQ, and a 14.745 MHz
        // part (-40.7 ppm) for a 14.7456 MHz one, with no note (Qarlos 2026-11-09, seeds
        // 20889483 / 21622586). Equal within the representation epsilon only (1e-9 =
        // 0.001 ppm); a hard key, so a difference rejects, and CrossRef.hpp states both
        // frequencies and the offset in ppm.
        {"frequency", D::ExactMatch, kRepresentationEpsilon, std::nullopt, false, true, nullptr},
        {"load_capacitance_pF", D::ExactMatch, 0.05, std::nullopt, false, false, nullptr},
        {"output_type", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        {"mode", D::ExactMatch, 0.0, std::nullopt, false, false, nullptr},
        // The control pin's function (TDAS electrical.enableFunction: none / outputEnable /
        // standby / triState). Pin 1 of a packaged XO is wired by the board for ONE of
        // them: an output-enable pin keeps the oscillator running and gates the output, a
        // standby pin powers the whole device down (restart costs the start-up time),
        // "none" is no control pin. ISM92-3251BH-25.000 MHZ (outputEnable) took ASFL1
        // (standby) as drop_in (Qarlos 2026-11-09, seed 21832044). A hard identity key like
        // output_type: both stated and different rejects, with a note naming both; stated
        // on one side only is UNVERIFIED, noted, and caps the grade below drop_in
        // (CrossRef.hpp). outputEnable and triState are one function
        // (enable_function_rank): they pass, with no note.
        {"enable_function", D::ExactMatch, 0.0, std::nullopt, false, false,
         &enable_function_rank()},
        // Frequency tolerance and stability (+/- ppm) are the accuracy the design was built
        // around (a UART baud budget, an Ethernet +/-100 ppm link, an RTC's drift): a
        // substitute must be equal or tighter. Looser by any amount is a FAIL, not a
        // warning — the old 2x warn band let a 50 ppm part through for a 25 ppm one.
        // Unstated on the substitute is UNVERIFIED (compare_numeric). Equality is judged
        // within the representation epsilon (49.999999999999996 ppm is 50 ppm).
        {"tolerance_ppm", D::Lower, 1.0, std::nullopt, false, false, nullptr},
        {"stability_ppm", D::Lower, 1.0, std::nullopt, false, false, nullptr},
        {"aging_ppm_y", D::Lower, 2.0, std::nullopt, false, false, nullptr},
        {"esr", D::Lower, 1.5, std::nullopt, false, false, nullptr},
        // An active oscillator runs from the board's rail, and the board's rail is
        // whatever the ORIGINAL's stated supply range allows: Kelvin cannot see which
        // voltage inside it the design actually uses. So the substitute's range must
        // COVER the original's at both ends, with no tolerance band — a 3.3 V-only XO
        // does not start on a 2.5 V rail, and a 1.8 V-only one is overstressed on it.
        // The analog family's 0.3 V / 10 % warn bands were copied here and let a
        // 2.7 V-minimum part pass as a warning for a 2.5 V original. A shortfall is a
        // FAIL (a different rail is a board change), which CrossRef.hpp turns into a
        // rejection (owner 2026-11-13); an unstated range is UNVERIFIED.
        // Not applied to passive resonators (quartzCrystal / ceramicResonator), which
        // have no supply — CrossRef.hpp skips both keys for them.
        {"supply_min_V", D::Lower, 0.0, 0.0, false, false, nullptr},
        {"supply_max_V", D::Higher, 0.0, 0.0, false, false, nullptr},
        {"current_consumption", D::Lower, 3.0, std::nullopt, false, false, nullptr},
        // The original's operating range must be covered at both ends with NO band (owner
        // 2026-11-14; was a 15 degC WARN band): any shortfall is a FAIL, as in the other
        // families (Qarlos r28: ABM13N-48.0000MHZ-8-D1X-T5, -40 degC, took 830108049101,
        // -30 degC, as a WARN).
        {"temp_min_C", D::Lower, 0.0, 0.0, false, false, nullptr},
        {"temp_max_C", D::Higher, 0.0, 0.0, false, false, nullptr},
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
    if (category == "igbt") return kIgbt;
    if (category == "bjt") return kBjt;
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
        {"max_voltage_V", "maximum working voltage"},
        {"vz_tolerance_pct", "breakdown-voltage tolerance"},
        {"vce_sat", "collector-emitter saturation voltage"},
        {"hfe_min", "minimum DC current gain (hFE)"},
        {"transition_frequency", "transition frequency (fT)"},
        {"max_continuous_dc_voltage", "maximum continuous DC voltage"},
        {"max_continuous_ac_voltage", "maximum continuous AC voltage"},
        {"energy_absorption", "energy rating"},
        {"on_resistance", "on-resistance"},
        {"architecture", "conversion architecture"},
        {"bias_r1", "built-in base resistor R1"},
        {"bias_r2", "built-in base-emitter resistor R2"},
    };
    for (const auto& [k, label] : kLabels)
        if (k == key) return label;
    std::string out = key;
    std::replace(out.begin(), out.end(), '_', ' ');
    return out;
}

}  // namespace kelvin::crossref
