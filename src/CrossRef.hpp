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
#include <cmath>
#include <cstdio>
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

// A percentage as an engineer writes it in a note: two decimals at most, no
// trailing zeros ("0.93 %", "5 %"). The other numeric notes here round to whole
// units, which would print a 0.25 % tolerance as "0 %".
inline std::string plain_pct(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s + " %";
}
// The same, sign always shown — a shift has a direction and it matters.
inline std::string signed_pct(double v) { return (v >= 0 ? "+" : "") + plain_pct(v); }

// A length as a mechanical drawing states it: metres in, millimetres out, two
// decimals at most, no trailing zeros ("6.36", "4"). Bare, so a note can write
// the unit once for a pair ("10.4 x 10.3 mm").
inline std::string mm(double metres) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", metres * 1000.0);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

// A temperature as a datasheet states it, sign always shown ("+125", "-55"): the
// sign is half the meaning of an operating limit. Bare, so a note can write the
// unit once for a pair ("+105 vs +125 degC").
inline std::string degc(double c) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", c);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return (c > 0 ? "+" : "") + s;
}

// A voltage as a datasheet states it ("3.56", "15.75"): three decimals at most, no
// trailing zeros. Bare, so a note can write the unit once for a band.
inline std::string volts(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

// The breakdown window a diode record GUARANTEES, written as a zener datasheet
// writes it: "3.56-3.64 V (a 1.11 % grade)". Both halves earn their place — the
// volts are what the circuit is checked against, the percentage is the grade the
// part is ordered by.
inline std::string vz_band(const json& p) {
    auto lo = num(p, "vz_min_V"), hi = num(p, "vz_max_V"), tol = num(p, "vz_tolerance_pct");
    if (!lo || !hi) return tol ? "a " + plain_pct(*tol) + " grade" : "";
    std::string s = volts(*lo) + "-" + volts(*hi) + " V";
    if (tol) s += " (a " + plain_pct(*tol) + " grade)";
    return s;
}

// The land a footprint note is talking about, longest axis first — the same way
// round the fit test compares them, so two parts read comparably. A record that
// states only one land axis says which axis it is and that the other is absent,
// rather than inventing it.
inline std::string land_mm(const Dims& d) {
    if (has_land(d))
        return mm(std::max(*d.length, *d.width)) + " x " + mm(std::min(*d.length, *d.width)) +
               " mm";
    if (d.length) return mm(*d.length) + " mm long, width not stated";
    if (d.width) return mm(*d.width) + " mm wide, length not stated";
    return "no land dimensions on record";
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

// The footprint term is a LADDER whose rungs must be monotone in how much board
// work the verdict implies, and the UNVERIFIED rung (kUnknownDimPenalty) is its
// hinge: the substitute might still drop in, the record just does not say. A
// footprint the ranker has ESTABLISHED to be a different land pattern — another
// case code, a body that no longer covers the pads, one that overhangs them — is
// a strictly WORSE answer than "cannot tell", because the answer is known and it
// is "no". So an established mismatch costs the unverified fit plus a warn step,
// and never less.
//
// Left unfloored, the categorical warn (0.5) and the dimensional scale (2.0 for
// an unverified fit) were two unrelated scales: a 0402 -> 0102 MELF the tool
// itself annotates as "a substitute to re-lay-out, not a drop-in" scored 0.5,
// while the exact-value, exact-power 0402 thin films whose records merely omit a
// width scored 2.0, and the re-layout parts headed the list (ABT #498).
inline constexpr double kEstablishedFootprintMismatchPenalty =
    kUnknownDimPenalty + kVerdictWarnPenalty;

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
//
// `missing_required_data` is the electrical twin of that: the substitute's record
// carries no value at all for a parameter whose absence is disqualifying (ESR on
// a capacitor, Isat on a magnetic). Nothing FAILED — nothing was compared — but a
// part nobody can check on the spec that defines it is not a drop-in either, so
// it takes the same major_review cap a real regression would.
inline const char* grade_for(const std::string& status, FootprintTier fit, bool any_warn,
                             bool any_fail, bool footprint_unverified = false,
                             bool missing_required_data = false,
                             HeightFit height = HeightFit::Unknown) {
    if (status == "no_substitute") return "no_substitute";
    // Either axis can put the part outside the original's board space: the land
    // overhanging the pads, or the body no longer clearing what is above it.
    if (fit == FootprintTier::Overflows || height == HeightFit::MuchTaller) return "redesign";
    if (any_fail || missing_required_data) return "major_review";
    // A materially smaller body (strict_case families) is a footprint CHANGE — the
    // pads won't match — so it is a review, not a true drop-in.
    if (any_warn || fit == FootprintTier::OneSizeLarger || fit == FootprintTier::Smaller ||
        footprint_unverified)
        return "minor_review";
    return "drop_in";
}

// Physical size of a part: an explicit mechanical drawing when the record has
// one, else the category-aware case-code resolution. Metres.
//
// A drawing that states only ONE land axis is kept, not thrown away: the axis it
// does state settles what it settles (see compare_land). Requiring both blanked
// the footprint verdict entirely for the ~8.6k catalogue rows that state one —
// 3.2 x 1.6 mm 1206 bodies were offered against a 2.0 mm 0805 original with
// "footprint": null, no note, and the same penalty as the true 0805 parts (ABT
// #516). A case code that resolves to a COMPLETE outline still wins over a
// half-stated drawing; the partial drawing is the fallback, never the loser.
inline std::optional<Dims> dims_of(const json& p, const std::string& category) {
    auto l = num(p, "length_m"), w = num(p, "width_m"), h = num(p, "height_m");
    Dims drawn;
    if (l && *l > 0) drawn.length = *l;
    if (w && *w > 0) drawn.width = *w;
    if (h && *h > 0) drawn.height = *h;
    if (has_land(drawn)) return drawn;
    const auto partial = [&]() -> std::optional<Dims> {
        if (drawn.length || drawn.width) return drawn;
        return std::nullopt;
    };
    // Magnetics and chip beads: a bare case code is NOT a reliable footprint. A
    // 4-digit magnetic code like "1210" is ambiguous between an EIA chip
    // (3.2 x 2.5 mm) and a molded power inductor (~12 x 10 mm) — resolving it
    // either way fabricates a footprint the datasheet never gave us, and would let
    // a physically larger part read as a drop-in. For these families the footprint
    // is verified ONLY from an explicit mechanical drawing; absent that it stays
    // unknown, so the part cannot be graded a drop-in. (Same reason these
    // categories are excluded from the mount gate — their package strings vary too
    // much by series to classify.)
    if (category == "magnetic" || category == "chipBead") return partial();
    std::string code = str(p, "case_code");
    if (code.empty()) code = str(p, "package");
    if (auto coded = resolve_dimensions(code, category)) {
        // The drawing's own height still stands where the table fixes no height —
        // a chip's height is not encoded in its case code.
        if (!coded->height) coded->height = drawn.height;
        return coded;
    }
    return partial();
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
    // A parameter whose absence disqualifies was absent on the substitute — an
    // unknown, not a regression, so it caps the grade rather than reading as one.
    bool missing_required_data = false;
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
    const auto o_val = num(original, "value_si"), s_val = num(cand, "value_si");
    const auto o_tol = num(original, "tolerance_pct"), s_tol = num(cand, "tolerance_pct");
    bool has_pv = false;
    // The original's tolerance bounds the pass window where the tolerance IS the
    // value's guarantee (resistors): a 0.25 % part cannot have a 0.93 % shift
    // called a drop-in on the strength of a catalogue-wide +/-1 % band.
    auto pv = score_primary_value(cat, o_val, s_val, has_pv, o_tol);
    if (has_pv) {
        params.push_back({{"name", "value"}, {"verdict", pv.verdict}});
        if (pv.verdict == FAIL) return reject("primary value out of range");
        penalty += opt.primary_weight * pv.penalty;
        if (pv.verdict == WARN) {
            demote();
            // Say WHAT moved and by how much. A bare "value: warn" left the row
            // silent about the one parameter the part exists for, so a shifted
            // value read exactly like an exact-value match (ABT #497).
            if (o_val && s_val && *o_val > 0) {
                const double shift = (*s_val / *o_val - 1.0) * 100.0;
                std::string note = signed_pct(shift) + " shift from the original's nominal value";
                if (o_tol && *o_tol > 0 && std::fabs(shift) > *o_tol) {
                    note += "; the original is a " + plain_pct(*o_tol) +
                            " part, so this sits outside the band it guarantees";
                    // The stronger claim — that no unit of either part can meet the
                    // other's spec — is about the GUARANTEED bands, so it is tested
                    // on the bands themselves rather than on the nominal shift.
                    if (s_tol && *s_tol >= 0 &&
                        (*s_val * (1.0 + *s_tol / 100.0) < *o_val * (1.0 - *o_tol / 100.0) ||
                         *s_val * (1.0 - *s_tol / 100.0) > *o_val * (1.0 + *o_tol / 100.0)))
                        note += " and the two guaranteed windows do not overlap";
                    note += " — not a drop-in anywhere the value sets a ratio or a reference";
                }
                notes.push_back(note);
            }
        }
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
    // A chip resistor ARRAY is not a discrete resistor: same body outline, a
    // different land pattern, and a rating that is per element rather than per
    // package. Surfaced rather than rejected — N discretes DO replace one array
    // once the board changes — so it lands as a FAIL on the device class, which
    // caps the grade at major_review and sinks the candidate below any genuine
    // array-for-array match.
    bool resistor_class_conflict = false;
    if (cat == "resistor") {
        const std::string conflict = resistor_network_conflict(
            declares_resistor_network(str(original, "family"), str(original, "mpn")),
            declares_resistor_network(str(cand, "family"), str(cand, "mpn")));
        if (!conflict.empty()) {
            params.push_back({{"name", "configuration"}, {"verdict", FAIL}});
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            notes.push_back(conflict);
            resistor_class_conflict = true;
        }
    }
    // Anti-sulfuration is a CONSTRUCTION class, not a parameter. An ERJ-S and the
    // ordinary ERJ beside it share every catalogue column and both read
    // "thickFilm", so silence about it read as agreement and a plain chip came
    // back drop_in / recommended for an anti-sulfurated original (ABT #518). The
    // part still FITS — this is a reliability class, not a land pattern — so it
    // stays out of the footprint conflict above and caps the grade on its own.
    if (cat == "resistor" &&
        sulfur_resistant_by_design(str(original, "family"), str(original, "mpn"))) {
        const bool declared = declares_sulfur_resistance(str(cand, "family"), str(cand, "mpn"));
        params.push_back({{"name", "sulfur_resistance"}, {"verdict", declared ? PASS : FAIL}});
        if (!declared) {
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            notes.push_back(sulfur_resistance_conflict(str(cand, "family")));
        }
    }
    // A single-phase bridge rectifier is not a discrete diode: four dies on four
    // terminals (AC, AC, +, -), a Vf quoted per element against a path that runs
    // through two of them, and an If(AV) that is the bridge's OUTPUT current. It
    // takes the same treatment as the resistor array above — a FAIL on the device
    // class, which caps the grade at major_review — rather than a rejection, because
    // four discretes DO replace a bridge once the board changes (ABT #521).
    bool diode_class_conflict = false;
    if (cat == "diode") {
        const std::string conflict =
            diode_configuration_conflict(str(original, "case_code"), str(original, "mpn"),
                                         str(cand, "case_code"), str(cand, "mpn"));
        if (!conflict.empty()) {
            params.push_back({{"name", "configuration"}, {"verdict", FAIL}});
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            notes.push_back(conflict);
            diode_class_conflict = true;
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

    // A connector's PITCH is its land pattern — the record carries no body outline,
    // so nothing else settles the fit. Captured from the PARAM_SPECS verdict below
    // (one tolerance, stated once) and reported as the footprint verdict, because a
    // "footprint": null next to a known pitch mismatch reads as "we could not check
    // the fit" when it has just been checked and failed (ABT #485).
    bool connector_pitch_conflict = false;
    // The other two halves of "does it mate": the finish on the separable surface and the
    // way the wire attaches. Both are stated by the records and both were declared absent
    // from the catalogue by the family caveat, so neither was ever compared (ABT #487).
    // Captured the same way, for the same reason — a bare "contact_plating: fail" does not
    // tell the engineer that gold is about to be mated to tin.
    bool connector_plating_conflict = false;
    bool connector_termination_conflict = false;
    // A zener's breakdown WINDOW, captured from the PARAM_SPECS verdict below for the
    // same reason: a bare "vz_tolerance_pct: unverified" beside "vrrm: pass" leaves the
    // engineer with no idea that the original guarantees 3.56-3.64 V and the substitute's
    // record guarantees nothing at all (ABT #488).
    bool diode_vz_band_regression = false;
    bool diode_vz_band_unverified = false;
    // ── PARAM_SPECS verdicts (the shared Heaviside table) ────────────────────
    for (const ParamSpec& spec : params_for(cat)) {
        bool has_data = detail::present(original, spec.key) || detail::present(cand, spec.key);
        if (spec.key == "saturation_current")
            has_data = has_data || detail::present(original, "saturation_points") ||
                       detail::present(cand, "saturation_points");
        if (!has_data) continue;
        const ParamOutcome outcome = compare_param(spec, original, cand);
        const std::string verdict = outcome.verdict;
        params.push_back({{"name", spec.key}, {"verdict", verdict}});
        if (cat == "connector" && verdict == FAIL) {
            if (spec.key == "pitch_mm") connector_pitch_conflict = true;
            if (spec.key == "contact_plating") connector_plating_conflict = true;
            if (spec.key == "termination") connector_termination_conflict = true;
        }
        if (cat == "diode" && spec.key == "vz_tolerance_pct") {
            if (verdict == WARN || verdict == FAIL) diode_vz_band_regression = true;
            // Only when the ORIGINAL states a window: that is the case where a real
            // guarantee is on the table and the substitute's record answers nothing.
            if (verdict == UNVERIFIED && detail::present(original, spec.key))
                diode_vz_band_unverified = true;
        }

        const bool numeric = (spec.dir == Dir::Lower || spec.dir == Dir::Higher);
        auto o = numeric ? detail::jnum(original, spec.key) : std::nullopt;
        auto s = numeric ? detail::jnum(cand, spec.key) : std::nullopt;
        const bool hard_gate =
            is_hard_param(cat, spec.key) && (o || detail::present(original, spec.key));

        if (outcome.missing_required_sub) {
            // The substitute's record carries no value for this parameter at all,
            // so nothing was compared: the verdict stays UNVERIFIED rather than
            // claiming a regression that had no substitute-side operand. The
            // consequence is still the FAIL's — same penalty, same demotion, and
            // a grade capped at major_review — so a part we cannot check never
            // outranks one we could. Say so in a note, or the engineer reads an
            // unexplained downgrade (ABT #496).
            notes.push_back("substitute's record carries no " + param_label(spec.key) +
                            " — the original states one, so this comparison could not be made; "
                            "confirm it from the datasheet before this swap");
            if (hard_gate) return reject("a parameter the match turns on is absent from the "
                                         "substitute's record — cannot be verified");
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            missing_required_data = true;
        } else if (verdict == FAIL) {
            if (hard_gate)
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

    // Say WHAT changed about the mating interface, not just that something did. The
    // counterpart is already on the board and is not being replaced, so the substitute
    // meets it on the substitute's finish, and the pairing is what decides.
    if (connector_plating_conflict)
        notes.push_back("contact plating differs: " + str(original, "contact_plating") + " -> " +
                        str(cand, "contact_plating") +
                        " — the substitute meets your existing counterpart on a different mating "
                        "surface; dissimilar finishes fret and corrode at a separable joint");
    if (connector_termination_conflict)
        notes.push_back("wire termination differs: " + str(original, "termination") + " -> " +
                        str(cand, "termination") +
                        " — a different wire-attach method, not a drop-in for a built harness");

    // Say WHAT the two parts guarantee, in volts. The Vrrm verdict above compares the
    // MARKED voltage, which two grades of the same zener share; the window is what the
    // reference or the clamp was designed around, so a change in it — or an unknown —
    // has to be stated rather than left implied by a "pass" on the marking.
    if (diode_vz_band_unverified)
        notes.push_back("the original guarantees " + vz_band(original) +
                        "; the substitute's record states no breakdown-voltage band, so the "
                        "grades could not be compared — confirm the substitute's from its "
                        "datasheet before using it as a reference or a clamp");
    else if (diode_vz_band_regression)
        notes.push_back("looser breakdown grade: the substitute guarantees " + vz_band(cand) +
                        " against the original's " + vz_band(original) +
                        " — confirm the reference or clamp level tolerates the wider window");

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
            // Name the pair that actually decided it — the EXPLICIT assembly types
            // when both records state one, the package strings otherwise (the same
            // order mount_incompatible uses). Always quoting the packages printed a
            // half-empty "mount type incompatible:  -> 8x8x10" for the many records
            // that state a mount type but carry no package string.
            const std::string o_mt = str(original, "mount"), s_mt = str(cand, "mount");
            const bool by_mount =
                !normalize_mount(o_mt).empty() && !normalize_mount(s_mt).empty();
            notes.push_back("mount type incompatible: " + (by_mount ? o_mt : o_pkg) + " -> " +
                            (by_mount ? s_mt : s_pkg));
            return reject("mount type incompatible (SMD vs leaded)");
        }
        auto o_dims = dims_of(original, cat), s_dims = dims_of(cand, cat);
        // A part with NO dimensions must not be offered as a substitute for an
        // original whose footprint we DO know: its fit can never be verified. For
        // families where the footprint is the mechanical fit and the case code is
        // not a reliable size (magnetics, chip beads), such a substitute is
        // EXCLUDED — rejected and flagged as a data gap — rather than guessed at.
        // The real remedy is to backfill its dimensions (datasheet Seeker); until
        // then it is not a substitute anyone can trust. (Other families resolve
        // dimensions reliably from their EIA/JEDEC case code, so this does not touch
        // them. And when the ORIGINAL itself has no footprint there is nothing to
        // fit to, so a dimensionless-vs-dimensionless compare — e.g. a part against
        // itself — is left to the electrical verdicts, never rejected here. The same
        // goes for an original that states only one land axis: one axis is not a
        // footprint to fit to, so it grades the substitute rather than excluding it.)
        // Both sides are tested on a COMPLETE land, which is what "has a footprint"
        // meant before a half-stated drawing could reach here at all: for a custom
        // land pattern one axis verifies nothing, so this stays exactly as strict.
        if ((cat == "magnetic" || cat == "chipBead") && o_dims && has_land(*o_dims) &&
            (!s_dims || !has_land(*s_dims))) {
            out["missing_dimensions"] = true;  // partial/incomplete catalogue record
            params.push_back({{"name", "footprint"}, {"verdict", UNVERIFIED}});
            notes.push_back(
                "no complete land pattern on record — footprint fit cannot be verified; excluded "
                "from cross-reference until its dimensions are added (catalogue data gap)");
            return reject("no dimensional data — cannot verify footprint fit");
        }
        // strict_case: a true drop-in KEEPS the footprint. Only a same-size
        // substitute (the case kept) scores 0 on footprint and ranks first; a
        // materially smaller body is a different land pattern (different pinout/pads
        // for an IC package, different pad geometry for a chip) — a footprint change
        // (FootprintTier::Smaller) that must be reviewed, never a clean drop-in.
        // Applied to every family (a catalogue audit found ~3k "drop_in"s across a
        // changed package — e.g. a MOSFET QFN 8x8 -> QFN 6x5). Chip passives lose
        // the old "smaller frees board space" right-sizing by deliberate choice.
        const bool strict_case = true;
        // A device-class conflict decides the footprint, and the body outline must not
        // be allowed to overrule it: an EXB-V8V array and a discrete 1206 share a
        // 3.2 x 1.6 mm outline, so the size compare below would report "fits" for a
        // part that lands on two pads out of eight. Claiming the slot first is what
        // stops that — every compare below is guarded on it still being free. A
        // four-terminal bridge module is the same story one rung coarser: the
        // case-code gate below would have called SMC -> SIP-4 an ordinary
        // "different_case" WARN, when the pads cannot be made to match at all.
        if (resistor_class_conflict || diode_class_conflict) {
            out["footprint"] = "different_land_pattern";
            params.push_back({{"name", "footprint"}, {"verdict", FAIL}});
            penalty += opt.footprint_weight * kVerdictFailPenalty;
        }
        // The connector equivalent, and for the same reason: the pitch decides the land
        // pattern and there is no body outline to weigh against it. Say WHERE the last
        // contact ends up — "pitch differs" is abstract, "pin 13 sits 6.5 mm off its pad"
        // is the reason the board does not accept the part.
        auto o_pitch = num(original, "pitch_mm"), s_pitch = num(cand, "pitch_mm");
        if (connector_pitch_conflict && o_pitch && s_pitch && !out.contains("footprint")) {
            out["footprint"] = "different_land_pattern";
            params.push_back({{"name", "footprint"}, {"verdict", FAIL}});
            penalty += opt.footprint_weight * kVerdictFailPenalty;
            std::string note = "pitch differs: " + mm(*o_pitch * 1e-3) + " -> " +
                               mm(*s_pitch * 1e-3) + " mm (" +
                               signed_pct(100.0 * (*s_pitch / *o_pitch - 1.0)) + ")";
            auto n = num(original, "positions");
            if (n && *n >= 2.0)
                note += "; over " + std::to_string(static_cast<long>(*n)) +
                        " positions the last contact sits " +
                        mm(std::fabs(*s_pitch - *o_pitch) * (*n - 1.0) * 1e-3) +
                        " mm off its pad";
            notes.push_back(note + " — a different land pattern, not a drop-in");
        }
        // "Case kept" gate for the families whose CASE CODE is the footprint (chip
        // passives + IC packages, i.e. everything except magnetics/chip beads whose
        // codes are unreliable). A true drop-in keeps the package; a DIFFERENT case
        // is a different land pattern (pinout/pads), so it is a substitute to review,
        // not a drop-in — and this is caught even when neither part's exact body
        // dimensions resolve (DO-214AB, SOD-128, QFN NxN … are often not in the
        // dimension tables). Magnetics/chip beads fall through to the SIZE check.
        if (cat != "magnetic" && cat != "chipBead" && !out.contains("footprint")) {
            const std::string oc = str(original, "case_code"), sc = str(cand, "case_code");
            if (!oc.empty() && !sc.empty() &&
                normalize_case_code(oc) != normalize_case_code(sc)) {
                out["footprint"] = "different_case";
                params.push_back({{"name", "footprint"}, {"verdict", WARN}});
                demote();
                // An ESTABLISHED land-pattern change, so it sits above the
                // unverified rung of the footprint ladder — never below it.
                penalty += opt.footprint_weight * kEstablishedFootprintMismatchPenalty;
                notes.push_back("different package/case (" + oc + " -> " + sc +
                                ") — different land pattern; a substitute to re-lay-out, not a "
                                "drop-in (verify pads/pinout)");
            }
        }
        FootprintTier tier = footprint_tier(o_dims, s_dims, strict_case);
        if (o_dims && !out.contains("footprint")) {
            double fit_penalty = footprint_penalty(o_dims, s_dims, strict_case);
            // Same ladder rule as the case-code gate above: a body the compare has
            // ESTABLISHED not to match the pads (smaller than them, or overhanging
            // them) is a worse answer than one nobody could verify, so it is
            // floored onto the rung above UNVERIFIED. The continuous term stands
            // wherever it is already higher — a 3x-oversize part must still cost
            // more than a 1.1x one — and below the rung the parts tie on footprint
            // and are ordered by their electricals, which is what separates two
            // substitutes that both need a re-layout anyway.
            if (tier != FootprintTier::Fits && tier != FootprintTier::Unknown)
                fit_penalty = std::max(fit_penalty, kEstablishedFootprintMismatchPenalty);
            penalty += opt.footprint_weight * fit_penalty;
            out["footprint"] = footprint_tier_name(tier);
            const char* verdict = tier == FootprintTier::Fits            ? PASS
                                  : tier == FootprintTier::Smaller       ? WARN
                                  : tier == FootprintTier::OneSizeLarger ? WARN
                                  : tier == FootprintTier::Overflows     ? FAIL
                                                                         : UNVERIFIED;
            params.push_back({{"name", "footprint"}, {"verdict", verdict}});
            // Heaviside demotes rather than rejects an oversize part: it is a
            // real part that works electrically and needs a board-space check.
            // The engineer decides whether the board has room.
            // Each note states the land it is talking about. Four geometrically
            // different outcomes used to carry the same sentence, so an engineer
            // could not tell a part that overhangs the pads from one that no longer
            // covers them (ABT #513).
            if (tier == FootprintTier::OneSizeLarger) {
                demote();
                notes.push_back("about one case size larger (" + land_mm(*s_dims) + " vs " +
                                land_mm(*o_dims) + ") — verify board fit");
            } else if (tier == FootprintTier::Smaller) {
                demote();
                notes.push_back("smaller body than the original (" + land_mm(*s_dims) + " vs " +
                                land_mm(*o_dims) +
                                ") — the land pattern differs, so this is not a drop-in; it needs "
                                "a new land pattern, verify the pads before substituting");
            } else if (tier == FootprintTier::Overflows) {
                demote();
                notes.push_back("larger than the original's footprint (" + land_mm(*s_dims) +
                                " vs " + land_mm(*o_dims) + ") — board respin likely");
            } else if (tier == FootprintTier::Unknown) {
                // Non-magnetic family whose original has a footprint but whose
                // substitute's case code did not resolve to one: the fit is
                // UNVERIFIED (magnetics/chip beads are already excluded above). It
                // must not read as a drop-in, so the grade is capped below. The same
                // rung takes a half-stated land on either side: the axes that COULD
                // be compared did not rule the fit out, which is not the same as
                // ruling it in.
                demote();
                notes.push_back(
                    !s_dims ? "mechanical dimensions unavailable for the substitute — footprint "
                              "fit could not be verified; confirm it fits the original's land "
                              "pattern"
                            : "only part of the land is on record (" + land_mm(*s_dims) + " vs " +
                                  land_mm(*o_dims) +
                                  ") — what is stated does not rule the fit out, but does not "
                                  "confirm it either; verify the land pattern");
            }
        }
        // ── height / clearance, its own axis ─────────────────────────────────
        // Not part of the land pattern, so it is judged and reported separately
        // and runs whatever the footprint verdict was — a part can need a new land
        // pattern AND more headroom, and an engineer has to be told both. Only a
        // TALLER substitute is a risk; a shorter one keeps the pads and asks less
        // of the enclosure.
        HeightFit hfit = height_fit(o_dims, s_dims);
        if (hfit != HeightFit::Unknown) {
            out["height_fit"] = height_fit_name(hfit);
            params.push_back({{"name", "height"},
                              {"verdict", hfit == HeightFit::Fits         ? PASS
                                          : hfit == HeightFit::Taller     ? WARN
                                                                          : FAIL}});
            if (hfit != HeightFit::Fits) {
                // Same ladder floor the footprint term takes: an ESTABLISHED
                // clearance overshoot is a worse answer than one nobody could
                // verify, never a cheaper one.
                penalty += opt.footprint_weight *
                           std::max(height_penalty(o_dims, s_dims),
                                    kEstablishedFootprintMismatchPenalty);
                demote();
                const std::string delta = mm(*o_dims->height) + " -> " + mm(*s_dims->height) +
                                          " mm (" +
                                          signed_pct(100.0 * height_overflow(o_dims, s_dims)) + ")";
                notes.push_back(hfit == HeightFit::Taller
                                    ? "taller than the original: " + delta +
                                          " — verify vertical clearance"
                                    : "much taller than the original: " + delta +
                                          " — will not fit the original's height envelope");
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
    // A substitute must cover the original's whole rated range, at both ends, and
    // the note names both temperatures: "does not reach the original's maximum" is
    // true of 1 degC and of 40 degC, and the engineer has to size the gap against
    // the enclosure ambient, not against the adjective.
    {
        // temp_min_C / temp_max_C are also PARAM_SPECS entries for some categories
        // (connector, timeBase). One physical fact gets one verdict and one penalty,
        // so where that table already ruled, this block adds the note only.
        auto already_judged = [&](const char* name) {
            for (const auto& p : params)
                if (p.value("name", "") == name) return true;
            return false;
        };
        auto o_lo = num(original, "temp_min_C"), s_lo = num(cand, "temp_min_C");
        auto o_hi = num(original, "temp_max_C"), s_hi = num(cand, "temp_max_C");
        if (o_lo && s_lo && *s_lo > *o_lo + 1e-9) {
            if (!already_judged("temp_min_C")) {
                params.push_back({{"name", "temp_min_C"}, {"verdict", FAIL}});
                demote();
                penalty += opt.gate_weight * kVerdictFailPenalty;
            }
            notes.push_back("rated only down to " + degc(*s_lo) + " degC vs the original's " +
                            degc(*o_lo) + " degC — does not cover the original's cold end");
        }
        if (o_hi && s_hi && *s_hi < *o_hi - 1e-9) {
            notes.push_back("rated only to " + degc(*s_hi) + " degC vs the original's " +
                            degc(*o_hi) + " degC — verify the enclosure ambient");
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
    // delivering that resistance in the original's circuit. The converse is just
    // as much of a fact: where the candidate's Rds(on) IS specified at a Vgs the
    // original's drive reaches, that part enhances there by its vendor's own
    // guarantee, and the verdict has to say so.
    if (cat == "mosfet") {
        auto o_th = num(original, "vgs_threshold_max"), s_th = num(cand, "vgs_threshold_max");
        auto o_rv = num(original, "rds_on_vgs"), s_rv = num(cand, "rds_on_vgs");
        if (o_th && s_th && *s_th > *o_th * 1.25) {
            // "the existing drive voltage" is the Vgs the ORIGINAL's Rds(on) is specified
            // at — the only statement of the drive level either record makes. Where the
            // candidate's own Rds(on) is a guaranteed value at a Vgs that drive reaches,
            // and its threshold sits below it, full enhancement there is guaranteed by the
            // candidate's datasheet: what is left is TIMING, not enhancement. Failing that
            // part contradicted its own record — onsemi's FCB290N80 states 0.29 ohm at
            // Vgs = 10 V with Vth(max) = 4.5 V and was graded a hard fail against an
            // original whose Rds(on) is likewise specified at 10 V — and contradicted this
            // same block's vgs_threshold_max verdict, which had already said warn on the
            // very same quantity (ABT #502). A fail is what the evidence supports only when
            // the candidate's record does not tie its Rds(on) to a drive the original's
            // reaches, or when its threshold climbs into that drive.
            const bool enhances_at_drive =
                o_rv && s_rv && *s_rv <= *o_rv + 1e-9 && *s_th < *o_rv;
            params.push_back(
                {{"name", "gate_drive"}, {"verdict", enhances_at_drive ? WARN : FAIL}});
            demote();
            penalty += opt.gate_weight *
                       (enhances_at_drive ? kVerdictWarnPenalty : kVerdictFailPenalty);
            std::string note = "higher gate threshold than the original (" + volts(*s_th) +
                               " V vs " + volts(*o_th) + " V) — ";
            if (enhances_at_drive) {
                note += "its Rds(on) is a guaranteed value at Vgs = " + volts(*s_rv) +
                        " V, which the original's own " + volts(*o_rv) +
                        " V drive reaches, so it does fully enhance there; expect slower "
                        "turn-on and a Miller plateau sitting higher — check the gate-drive "
                        "timing and the switching loss it buys";
            } else if (o_rv && *s_th >= *o_rv) {
                note += "its threshold reaches the " + volts(*o_rv) +
                        " V drive the original's Rds(on) is specified at — it may not "
                        "enhance at all";
            } else if (!s_rv) {
                note += "its record does not state the Vgs its Rds(on) is specified at, so "
                        "full enhancement at the existing drive is not established";
            } else if (o_rv) {
                note += "its Rds(on) is specified at Vgs = " + volts(*s_rv) + " V, above the " +
                        volts(*o_rv) +
                        " V the original's is — it may not fully enhance at the existing "
                        "drive voltage";
            } else {
                note += "the original's record does not state the Vgs its Rds(on) is "
                        "specified at, so the drive level in the existing circuit is "
                        "unknown — it may not fully enhance at it";
            }
            notes.push_back(note);
        }
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
               : f == "smaller"         ? FootprintTier::Smaller
               : f == "one_size_larger" ? FootprintTier::OneSizeLarger
               : f == "overflows"       ? FootprintTier::Overflows
                                        : FootprintTier::Unknown;
        // Only a literal "unknown" (dims/case-code missing) is UNVERIFIED and blocks
        // a drop-in via grade_for; "different_case" already carries a WARN param
        // (any_warn -> minor_review), so it must not also read as unverified.
        footprint_unverified = (f == "unknown");
    }
    // The clearance axis, carried alongside the land one: a body that no longer
    // clears the enclosure is outside the original's board space just as surely as
    // one that overhangs the pads.
    HeightFit hfit = HeightFit::Unknown;
    if (out.contains("height_fit")) {
        const std::string h = out["height_fit"];
        hfit = h == "fits"          ? HeightFit::Fits
               : h == "taller"      ? HeightFit::Taller
               : h == "much_taller" ? HeightFit::MuchTaller
                                    : HeightFit::Unknown;
    }
    out["grade"] = grade_for(status, tier, any_warn, any_fail, footprint_unverified,
                             missing_required_data, hfit);

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
