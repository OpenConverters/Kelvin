// Cross-reference scoring — the deterministic utility-curve engine, ported
// parity-exact from Heaviside's pipeline/scoring.py. Pure math, no data deps, so
// it is header-only and usable identically from the native lib, PyKelvin, and
// the embind WASM build the Kirchhoff GUI loads.
//
// Four comparison modes mirror the engineering direction of every real
// parameter; each yields a CONTINUOUS penalty (0 = ideal) plus a discrete
// verdict (pass/warn/fail/unverified). Everything is computed in log-ratio
// space x = ln(s/o): unit-free, multiplicatively symmetric, naturally
// compressing large ratios. No fabrication — a missing value yields UNVERIFIED,
// never a silent pass.
#pragma once
#include <cmath>
#include <optional>
#include <string>

namespace kelvin::crossref {

enum class Mode { Exact, HigherBetter, LowerBetter, Range };

// Verdict strings match param_check.py / scoring.py exactly (shared vocabulary).
inline constexpr const char* PASS = "pass";
inline constexpr const char* WARN = "warn";
inline constexpr const char* FAIL = "fail";
inline constexpr const char* UNVERIFIED = "unverified";

// ── Curve tuning constants (identical to scoring.py) ────────────────────────
inline constexpr double kKOver = 0.6;   // over-dimensioning weight
inline const double kXOverCap = std::log(8.0);  // penalty freezes past an 8x surplus
inline constexpr double kEdgeEps = 1e-9;
inline constexpr double kKDef = 4.0;    // deficit weight
inline constexpr double kSDef = 3.0;    // deficit steepness
inline constexpr double kKProx = 2.0;   // range proximity weight

struct ScoreResult {
    double penalty = 0.0;          // 0 = ideal; larger = worse
    std::string verdict = PASS;    // pass / warn / fail / unverified
    std::optional<double> ratio;   // substitute / original when both known
};

// Concave, capped over-dimensioning penalty for a log-ratio surplus >= 0.
inline double over_penalty(double surplus) {
    return kKOver * std::sqrt(std::min(surplus, kXOverCap));
}

// Ranking tie-breaker: a rating that EXCEEDS its requirement costs a little,
// with diminishing returns; 0 at-or-under requirement or on missing input.
inline double over_dimensioning_penalty(std::optional<double> required,
                                        std::optional<double> actual, double weight = 1.0) {
    if (!required || !actual || *required <= 0 || *actual <= *required) return 0.0;
    return weight * over_penalty(std::log(*actual / *required));
}

// HIGHER_BETTER / LOWER_BETTER: surplus side passes (capped over-dim penalty),
// deficit side is a steep exponential, WARN then FAIL past the gate.
inline ScoreResult score_directional(std::optional<double> original,
                                     std::optional<double> substitute, Mode mode,
                                     double warn_factor, double gate_factor) {
    if (!original && !substitute) return {0.0, UNVERIFIED, std::nullopt};
    if (!substitute) return {0.0, UNVERIFIED, std::nullopt};
    if (!original) return {0.0, UNVERIFIED, std::nullopt};
    double o = *original, s = *substitute;
    if (o <= 0 || s <= 0) {
        bool ok = (mode == Mode::HigherBetter) ? (s >= o) : (s <= o);
        return {0.0, ok ? PASS : FAIL, std::nullopt};
    }
    double ratio = s / o;
    double x = std::log(ratio);
    double surplus = (mode == Mode::HigherBetter) ? x : -x;
    if (surplus >= 0) {
        return {over_penalty(surplus), PASS, ratio};
    }
    double deficit = -surplus;
    double penalty = kKDef * (std::exp(deficit * kSDef) - 1.0);
    double warn_edge = (warn_factor != 0.0) ? std::fabs(std::log(warn_factor)) : 0.0;
    double gate_edge = (gate_factor != 0.0) ? std::fabs(std::log(gate_factor)) : warn_edge;
    std::string verdict = (deficit <= gate_edge) ? WARN : FAIL;
    (void)warn_edge;  // warn/near-limit both render WARN; kept for parity clarity
    return {penalty, verdict, ratio};
}

// RANGE / proximity: closest-to-nominal wins; inside tight = PASS, inside accept
// but off nominal = WARN, outside accept = FAIL (the 330nH-for-1.5µH rejection).
inline ScoreResult score_range(std::optional<double> original, std::optional<double> substitute,
                               double tight_lo, double tight_hi, double accept_lo,
                               double accept_hi) {
    if (!original && !substitute) return {0.0, UNVERIFIED, std::nullopt};
    if (!substitute) return {0.0, UNVERIFIED, std::nullopt};
    if (!original) return {0.0, UNVERIFIED, std::nullopt};
    double o = *original, s = *substitute;
    if (o <= 0 || s <= 0) return {0.0, (s == o) ? PASS : FAIL, std::nullopt};
    double ratio = s / o;
    double x = std::log(ratio);
    double lo_t = std::log(tight_lo), hi_t = std::log(tight_hi);
    double lo_a = std::log(accept_lo), hi_a = std::log(accept_hi);
    double dist_tight = (x < lo_t) ? (lo_t - x) : (x > hi_t ? x - hi_t : 0.0);
    if ((lo_a - kEdgeEps) <= x && x <= (hi_a + kEdgeEps)) {
        double penalty = kKProx * dist_tight;
        return {penalty, (dist_tight <= kEdgeEps) ? PASS : WARN, ratio};
    }
    double dist_accept = (x < lo_a) ? (lo_a - x) : (x - hi_a);
    double penalty = kKProx * dist_tight + kKDef * (std::exp(dist_accept * kSDef) - 1.0);
    return {penalty, FAIL, ratio};
}

// ── Primary-value specification per category (mirrors PRIMARY_VALUE_SPECS) ───
struct PrimaryValueSpec {
    Mode mode;
    double tight_lo, tight_hi, accept_lo, accept_hi;  // RANGE windows
    double warn_factor, gate_factor;                  // HIGHER/LOWER thresholds
};

// Returns the spec + true, or {} + false when the category has no primary-value
// gate (mosfet/diode/connector/analog/timeBase match on other axes).
inline bool primary_value_spec(const std::string& category, PrimaryValueSpec& out) {
    if (category == "resistor") {
        out = {Mode::Range, 0.99, 1.01, 0.95, 1.05, 0.9, 0.8};
        return true;
    }
    if (category == "capacitor") {
        out = {Mode::Range, 0.90, 1.50, 0.80, 4.00, 0.9, 0.8};
        return true;
    }
    if (category == "magnetic") {
        out = {Mode::Range, 0.90, 1.10, 0.80, 1.25, 0.9, 0.8};
        return true;
    }
    if (category == "chipBead") {
        out = {Mode::HigherBetter, 1.0, 1.0, 1.0, 1.0, 0.8, 0.7};
        return true;
    }
    return false;
}

// Score the primary electrical value for a category (SI base units). has_spec is
// false when the category has no primary-value gate.
inline ScoreResult score_primary_value(const std::string& category, std::optional<double> original,
                                       std::optional<double> substitute, bool& has_spec) {
    PrimaryValueSpec spec{};
    has_spec = primary_value_spec(category, spec);
    if (!has_spec) return {0.0, UNVERIFIED, std::nullopt};
    if (spec.mode == Mode::Range) {
        return score_range(original, substitute, spec.tight_lo, spec.tight_hi, spec.accept_lo,
                           spec.accept_hi);
    }
    return score_directional(original, substitute, spec.mode, spec.warn_factor, spec.gate_factor);
}

}  // namespace kelvin::crossref
