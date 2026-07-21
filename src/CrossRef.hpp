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
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "CrossRefClasses.hpp"
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
    static const std::set<std::string> connector{"family", "positions"};
    static const std::set<std::string> analog{"subtype", "channels"};
    // mode: a 3rd-overtone crystal cannot cross with a fundamental one in either
    // direction — an overtone circuit's LC tank is inductive at the fundamental,
    // so the loop cannot close there; drop an overtone part into a fundamental
    // circuit and it either fails to start or runs near a third of the marking.
    // output_type: LVDS / HCSL / LVPECL / CMOS are different termination
    // networks, not a parameter — swapping one for another is a board respin.
    // Both are self-selecting: mode is absent on oscillators and output_type on
    // bare crystals, and an absent parameter is simply not compared.
    static const std::set<std::string> timebase{"subtype", "technology", "frequency", "mode",
                                                "output_type"};
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

// ── Match grade ──────────────────────────────────────────────────────────────
// The industry cross-reference graders (SiliconExpert A / A-U / A-D / B / C / D
// / SF, Z2Data Drop-In A / B / C) all express two things our status alone does
// not: whether the swap needs a BOARD change, and whether the substitute is
// better or worse than the original. Both are what an engineer actually asks.
//
// We express the same two axes, but from published rules rather than an
// undisclosed algorithm:
//   drop_in        — fits the original's footprint, no parameter regressions
//   minor_review   — fits, but with warnings worth a look
//   major_review   — fits, but a parameter regressed materially
//   redesign       — does not fit, or mount/family/process differs
//   no_substitute  — a hard gate failed
// direction: upgrade / equivalent / downgrade, from the per-parameter verdicts.
//
// `footprint_unverified` is set when the ORIGINAL has a known footprint but the
// substitute's could not be established (no mechanical drawing). "drop_in" asserts
// the part fits the original's land pattern — a claim we cannot make without the
// substitute's dimensions — so an unverified footprint caps the grade at
// minor_review however clean the electricals are.
inline const char* grade_for(const std::string& status, FootprintTier fit, bool any_warn,
                             bool any_fail, bool footprint_unverified = false) {
    if (status == "no_substitute") return "no_substitute";
    if (fit == FootprintTier::Overflows) return "redesign";
    if (any_fail) return "major_review";
    if (any_warn || fit == FootprintTier::OneSizeLarger || footprint_unverified)
        return "minor_review";
    return "drop_in";
}

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
    // Magnetics and chip beads: a bare case code is NOT a reliable footprint. A
    // 4-digit magnetic code like "1210" is ambiguous between an EIA chip
    // (3.2 x 2.5 mm) and a molded power inductor (~12 x 10 mm) — resolving it
    // either way fabricates a footprint the datasheet never gave us, and would let
    // a physically larger part read as a drop-in. For these families the footprint
    // is verified ONLY from an explicit mechanical drawing; absent that it stays
    // unknown, so the part cannot be graded a drop-in. (Same reason these
    // categories are excluded from the mount gate — their package strings vary too
    // much by series to classify.)
    if (category == "magnetic" || category == "chipBead") return std::nullopt;
    std::string code = str(p, "case_code");
    if (code.empty()) code = str(p, "package");
    return resolve_dimensions(code, category);
}

// Score one candidate against the original. Returns a per-candidate JSON verdict.
inline json score_candidate(const std::string& cat, const json& original, const json& cand,
                            const Options& opt) {
    json out;
    out["mpn"] = str(cand, "mpn");
    // Optional caller-supplied identity, echoed verbatim. Two vendors can ship
    // the same MPN string, so a caller that needs to map a result back to its
    // own row passes a key rather than overloading `mpn` — `mpn` must stay the
    // REAL part number, because the AEC-Q and rated-voltage gates decode it.
    //
    // The field is `_key`, deliberately underscore-prefixed: it previously used
    // `id`, which COLLIDES with the MOSFET drain-current parameter of the same
    // name. The identity string overwrote the current, num() then read a string
    // as absent, and the drain-current comparison silently disabled itself — a
    // 31 A part ranked top against a 200 A original. A datasheet parameter will
    // never be named with a leading underscore, so the namespace is safe.
    if (!str(cand, "_key").empty()) out["_key"] = str(cand, "_key");
    // Guard the class of bug rather than just this instance: a caller that puts
    // a STRING where a physical parameter is expected has masked that parameter,
    // and num() would read it as simply absent — the comparison then disables
    // itself in silence. Say so instead.
    for (const auto& r : critical_ratings(cat)) {
        auto it = cand.find(r.key);
        if (it != cand.end() && it->is_string())
            throw std::invalid_argument(std::string("cross_reference: '") + r.key +
                                        "' is a numeric parameter for category '" + cat +
                                        "' but was given a string — a caller key must not "
                                        "reuse a parameter name (use `_key`)");
    }
    double penalty = 0.0;
    std::string status = "recommended";
    json params = json::array();
    std::vector<std::string> notes;
    // Direction bookkeeping: on each directional parameter we could compare,
    // did the substitute come out strictly ahead of the original, or behind?
    int better = 0, worse = 0;
    auto note_direction = [&](Dir dir, std::optional<double> o, std::optional<double> s) {
        if (!o || !s || *o <= 0 || *s <= 0) return;
        const double ratio = *s / *o;
        if (ratio > 1.05) (dir == Dir::Higher ? better : worse)++;
        else if (ratio < 0.95) (dir == Dir::Higher ? worse : better)++;
    };

    auto reject = [&](const char* reason) {
        out["status"] = "no_substitute";
        out["reason"] = reason;
        out["penalty"] = 1e9;
        out["params"] = params;
        if (!notes.empty()) out["notes"] = notes;
        // A rejected candidate still carries grade/direction so every row in the
        // result has the same shape — a consumer should never have to special-case
        // the rejects to render a table.
        out["grade"] = "no_substitute";
        out["direction"] = "downgrade";
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

    // ── construction family (hard) ───────────────────────────────────────────
    // A parametric match across families is not a substitution: a 10 uF ceramic
    // and a 10 uF tantalum share every catalogue column and have different
    // failure modes, ESR, derating and bias behaviour.
    if (cat == "capacitor") {
        const std::string conflict = cap_family_conflict(cap_family(str(original, "technology")),
                                                         cap_family(str(cand, "technology")));
        if (!conflict.empty()) {
            params.push_back({{"name", "family"}, {"verdict", FAIL}});
            notes.push_back(conflict);
            return reject("different capacitor construction family");
        }
    }
    // Si / SiC / GaN differ in gate-drive requirements — a driver redesign, not
    // a drop-in, so it is surfaced loudly rather than scored away.
    if (cat == "mosfet" || cat == "igbt" || cat == "diode") {
        const std::string conflict =
            process_conflict(str(original, "technology"), str(cand, "technology"));
        if (!conflict.empty()) {
            params.push_back({{"name", "process"}, {"verdict", FAIL}});
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            notes.push_back(conflict);
        }
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
        if (numeric) note_direction(spec.dir, o, s);
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
        note_direction(r.mode == Mode::HigherBetter ? Dir::Higher : Dir::Lower, o, s);
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
            } else if (tier == FootprintTier::Unknown) {
                // We know the original's footprint but could not establish the
                // substitute's (no mechanical drawing; its case code is not a
                // reliable footprint for this family). The fit is UNVERIFIED, so
                // this must not read as a drop-in — the grade is capped below.
                demote();
                notes.push_back(
                    "mechanical dimensions unavailable for the substitute — footprint fit "
                    "could not be verified; confirm it fits the original's land pattern");
            }
        }
    }

    // ── dielectric envelope (EIA RS-198), not a string compare ──────────────
    // X7R -> X5R keeps the code "shape" but drops the upper temperature 125 ->
    // 85 degC; X7R -> X7S keeps the temperature and widens tolerance 15 -> 22%.
    // Both are real regressions that comparing code strings cannot articulate.
    if (cat == "capacitor") {
        const std::string reg = dielectric_regression(str(original, "dielectric_code"),
                                                      str(cand, "dielectric_code"));
        if (!reg.empty()) {
            params.push_back({{"name", "dielectric_envelope"}, {"verdict", FAIL}});
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            notes.push_back(reg);
        }
    }

    // ── operating temperature range ─────────────────────────────────────────
    // A substitute must cover the original's whole rated range, at both ends.
    {
        auto o_lo = num(original, "temp_min_C"), s_lo = num(cand, "temp_min_C");
        auto o_hi = num(original, "temp_max_C"), s_hi = num(cand, "temp_max_C");
        if (o_lo && s_lo && *s_lo > *o_lo + 1e-9) {
            params.push_back({{"name", "temp_min_C"}, {"verdict", FAIL}});
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            notes.push_back("does not reach the original's minimum operating temperature");
        }
        if (o_hi && s_hi && *s_hi < *o_hi - 1e-9) {
            // temp_max_C may also be covered by PARAM_SPECS for some categories;
            // the note is what carries the meaning either way.
            notes.push_back("does not reach the original's maximum operating temperature");
        }
    }

    // ── ESR / ripple measurement conditions ─────────────────────────────────
    // ESR is strongly frequency-dependent and a ripple rating is meaningless
    // without its frequency: a 120 Hz rating and a 100 kHz rating are not
    // interconvertible. Comparing them as bare numbers manufactures a verdict.
    // Where both sides state a frequency and they disagree by more than a
    // decade, the comparison is reported UNVERIFIED rather than passed.
    if (cat == "capacitor") {
        auto o_f = num(original, "esr_frequency"), s_f = num(cand, "esr_frequency");
        if (o_f && s_f && *o_f > 0 && *s_f > 0) {
            double ratio = *o_f > *s_f ? *o_f / *s_f : *s_f / *o_f;
            if (ratio > 10.0) {
                params.push_back({{"name", "esr_basis"}, {"verdict", UNVERIFIED}});
                notes.push_back("ESR quoted at different frequencies (" +
                                std::to_string(static_cast<long>(*o_f)) + " Hz vs " +
                                std::to_string(static_cast<long>(*s_f)) +
                                " Hz) — not directly comparable");
            }
        }
    }

    // ── MOSFET gate-drive compatibility ─────────────────────────────────────
    // The classic swap failure: a logic-level part replaced by a standard-level
    // one still switches on the bench at 10 V and never fully enhances from a
    // 3.3 V controller. Rds(on) is only meaningful at its stated Vgs, so a part
    // whose Rds(on) is specified at a higher Vgs than the original's is not
    // delivering that resistance in the original's circuit.
    if (cat == "mosfet") {
        auto o_th = num(original, "vgs_threshold_max"), s_th = num(cand, "vgs_threshold_max");
        if (o_th && s_th && *s_th > *o_th * 1.25) {
            params.push_back({{"name", "gate_drive"}, {"verdict", FAIL}});
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            notes.push_back("higher gate threshold than the original — may not fully enhance at "
                            "the existing drive voltage");
        }
        auto o_rv = num(original, "rds_on_vgs"), s_rv = num(cand, "rds_on_vgs");
        if (o_rv && s_rv && *s_rv > *o_rv + 1e-9) {
            params.push_back({{"name", "rds_on_basis"}, {"verdict", WARN}});
            demote();
            penalty += opt.gate_weight * kVerdictWarnPenalty;
            notes.push_back("Rds(on) is specified at a higher Vgs (" +
                            std::to_string(static_cast<int>(*s_rv)) + " V vs " +
                            std::to_string(static_cast<int>(*o_rv)) +
                            " V) — it will be higher at the original's drive level");
        }
        // Vgs(max) is the gate's absolute rating: a lower one can be exceeded by
        // the existing driver and destroy the part.
        auto o_gm = num(original, "vgs_max"), s_gm = num(cand, "vgs_max");
        if (o_gm && s_gm && *s_gm < *o_gm - 1e-9) {
            params.push_back({{"name", "vgs_max"}, {"verdict", WARN}});
            demote();
            penalty += opt.gate_weight * kVerdictWarnPenalty;
            notes.push_back("lower maximum gate-source voltage than the original — check the "
                            "existing gate drive cannot exceed it");
        }
    }

    // ── crystal load capacitance ────────────────────────────────────────────
    // A crystal is specified AT a load capacitance; the board's load network is
    // built for that value. A substitute specified for a different CL runs off
    // frequency in the existing circuit while looking entirely correct. Applies
    // to passive resonators only — an oscillator carries its own circuit.
    if (cat == "timeBase" &&
        is_passive_resonator(str(original, "technology"), str(original, "device_type")) &&
        is_passive_resonator(str(cand, "technology"), str(cand, "device_type"))) {
        auto o_cl = num(original, "load_capacitance"), s_cl = num(cand, "load_capacitance");
        if (auto ppm = crystal_pull_ppm(o_cl, s_cl)) {
            const double mag = std::abs(*ppm);
            if (mag > 5.0) {  // below this the pull is lost in the part's own tolerance
                params.push_back({{"name", "load_capacitance"}, {"verdict", FAIL}});
                notes.push_back(
                    "specified for a different load capacitance (" +
                    std::to_string(static_cast<int>(std::llround(*s_cl * 1e12))) + " pF vs " +
                    std::to_string(static_cast<int>(std::llround(*o_cl * 1e12))) +
                    " pF): in the original's load network it would run roughly " +
                    std::to_string(static_cast<int>(std::llround(mag))) +
                    " ppm off frequency (estimate, typical C0/C1)");
                return reject("crystal specified for a different load capacitance");
            }
        }
    }

    // ── automotive grade (AEC-Q), decoded from the MPN ───────────────────────
    // Fires even when the original is not in the catalogue, which is exactly
    // when an engineer most needs to be told.
    // The catalogue's structured `qualification` field is authoritative where it
    // exists; the MPN decode is the fallback for parts we hold no record of.
    auto qualified = [](const json& p) {
        const std::string q = lower_copy(str(p, "qualification"));
        if (!q.empty()) return q.find("aec") != std::string::npos;
        return is_automotive_grade(str(p, "mpn"));
    };
    if (qualified(original) && !qualified(cand)) {
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

    // ── grade + direction ────────────────────────────────────────────────────
    bool any_warn = false, any_fail = false;
    for (const auto& p : params) {
        const std::string v = p.value("verdict", "");
        if (v == WARN) any_warn = true;
        if (v == FAIL) any_fail = true;
    }
    FootprintTier tier = FootprintTier::Unknown;
    bool footprint_unverified = false;
    if (out.contains("footprint")) {
        const std::string f = out["footprint"];
        tier = f == "fits"              ? FootprintTier::Fits
               : f == "one_size_larger" ? FootprintTier::OneSizeLarger
               : f == "overflows"       ? FootprintTier::Overflows
                                        : FootprintTier::Unknown;
        // out["footprint"] is only recorded when the ORIGINAL has a footprint, so
        // an "unknown" tier here means the substitute's could not be verified —
        // which must block a drop-in claim (see grade_for).
        footprint_unverified = (tier == FootprintTier::Unknown);
    }
    out["grade"] = grade_for(status, tier, any_warn, any_fail, footprint_unverified);

    // Direction: on the directional parameters we could actually compare, did
    // the substitute come out ahead or behind? Mirrors the industry's upgrade /
    // downgrade suffix (SiliconExpert A/U vs A/D), but computed from the
    // measured ratios rather than asserted. "equivalent" when neither side
    // clearly leads — including when there was nothing comparable to judge on,
    // which is honest rather than flattering.
    out["direction"] = (worse > 0 && better == 0)   ? "downgrade"
                       : (better > 0 && worse == 0) ? "upgrade"
                       : (better > 0 && worse > 0)  ? "mixed"
                                                    : "equivalent";
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
