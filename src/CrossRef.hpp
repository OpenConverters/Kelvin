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
#include <map>
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

inline bool flag(const json& o, const char* k) {
    auto it = o.find(k);
    return it != o.end() && it->is_boolean() && it->get<bool>();
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

// The supply range an oscillator record states, for a note: "2.5 V" for a fixed-rail
// part, "1.71-3.63 V" for a wide-range one, and says so when one end is not stated
// rather than inventing it.
inline std::string supply_range(const json& p) {
    auto lo = num(p, "supply_min_V"), hi = num(p, "supply_max_V");
    if (lo && hi) return *lo == *hi ? volts(*lo) + " V" : volts(*lo) + "-" + volts(*hi) + " V";
    if (lo) return "at least " + volts(*lo) + " V (no maximum stated)";
    if (hi) return "at most " + volts(*hi) + " V (no minimum stated)";
    return "an unstated supply";
}

// The land a footprint note is talking about, longest axis first — the same way
// round the fit test compares them, so two parts read comparably. A record that
// states only one land axis says which axis it is and that the other is absent,
// rather than inventing it.
// The inductance-drop criteria a saturation-current record states, for a note.
inline std::string isat_basis(const std::vector<isat::Point>& pts) {
    std::string out;
    for (const auto& p : pts) {
        if (!p.percent_drop) continue;
        char buf[48];
        std::snprintf(buf, sizeof buf, "%g A at %g %% drop", p.current, *p.percent_drop);
        out += (out.empty() ? "" : ", ") + std::string(buf);
    }
    if (out.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%g A at no stated criterion",
                      pts.empty() ? 0.0 : pts.front().current);
        out = buf;
    }
    return out;
}

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
    // A connector is half of a mated pair: the other gender, another interface
    // standard or mounting style does not plug into what is on the board. Only a
    // MISMATCH rejects — a record that omits one is unverified, not refused
    // (none of these excludes a missing substitute). Pitch is deliberately NOT
    // here: a different pitch is reported as a land-pattern change (partial,
    // redesign), ranked below every part that keeps the pads.
    static const std::set<std::string> connector{"family", "positions", "polarity",
                                                 "interface_standard", "mounting"};
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
    if (cat == "mosfet") return key == "channel";
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
    // What the CIRCUIT needs, not what the original happened to have: the
    // minimum value of a higher-is-better parameter at the operating point
    // (a magnetic's saturation_current >= the peak current, a capacitor's voltage
    // >= V_peak x derating). Callers own the margins; the ranker only enforces
    // them. Below a requirement is a hard reject; a record that does not state
    // the parameter cannot be shown to meet it and is capped at partial.
    std::map<std::string, double> requirements;
    size_t max_results = 25;
};

// A capacitor above this multiple of the original's value is capped at partial.
inline constexpr double kCapDropInMaxRatio = 1.2;
// When the original states no temperature grade, a candidate below 125 C pays
// this much per degree short — a tie-breaker only (85 C X5R vs 125 C X7R costs
// 0.08, far under any real parameter term), so the more capable grade wins a tie.
inline constexpr double kUnknownTempGradePenaltyPerC = 0.002;

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
    // Every axis the drawing states is kept, land or not. A record that states a
    // HEIGHT and no land is not a record with no dimensions: throwing it away made
    // the ranker print "mechanical dimensions unavailable for the substitute" beside
    // the height it had just listed, and silently disabled the clearance axis that
    // judges that height — an Abracon ALFB-01 (5.0 mm tall) came back minor_review
    // against a 0.35 mm chip bead with no height verdict at all (ABT #499). What a
    // land-less drawing cannot do is answer a FOOTPRINT question; that is what
    // has_any_land guards, not what is kept here.
    const auto partial = [&]() -> std::optional<Dims> {
        if (drawn.length || drawn.width || drawn.height) return drawn;
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
        //
        // And a height the code resolver took from its outline TABLE is not the record's: it
        // is the table's typical body for the code. Judging clearance from it graded
        // CR0603-FX-1743ELF's height 'pass' against a 0.45 mm AC0603FR-07174KL when the
        // Bourns record states no height at all (Qarlos 2026-11-08). Only the record's own
        // drawing, or a code that spells its height out, answers the clearance question.
        if (!coded->height || coded->height_from_table) {
            coded->height = drawn.height;
            coded->height_from_table = false;
        }
        return coded;
    }
    return partial();
}

// ── resistor power at the original's rating temperature ─────────────────────
// A resistor's power rating is the power it carries up to a STATED ambient, and above that
// ambient it derates along its curve. RN50C5000FR36 is 0.05 W at 125 degC (flat to 125,
// zero at 175); CR0603-FX-5100ELF is 0.1 W at 70 degC and zero at 125 degC. Compared as
// 0.05 vs 0.1 the thick-film part "passed"; at the temperature the original is rated for it
// delivers nothing. So the comparison is made at the ORIGINAL's rating temperature, from
// what each record states — never from a typical curve:
//   * T_ref is the original's powerRatingTemperature, else the knee of its stated curve
//     (the end of the leading full-power run). Neither stated -> the ratings are compared
//     as stated, exactly as before (no basis exists for any candidate, so none is skewed).
//   * The original delivers its rating at T_ref (that is what the rating says).
//   * The substitute: from its stated curve, linearly interpolated at T_ref (the curves are
//     straight-line segments in the datasheets); 0 W above the curve's last temperature,
//     where the part is not rated to operate at all. With no curve, its full rating holds
//     when its own rating temperature is at or above T_ref (the definition of a rating
//     temperature). Anything else — no curve and a lower or unstated rating temperature,
//     or T_ref below the curve's first point — is NOT ON RECORD, and the verdict is
//     UNVERIFIED, never a pass on the nominal.
struct PowerAtTemperature {
    bool applies = false;          // the original states a temperature basis
    double t_ref = 0.0;            // degC
    std::optional<double> orig;    // W the original delivers at t_ref
    std::optional<double> subst;   // W the substitute delivers at t_ref; nullopt = not on record
    std::string how;               // how `subst` was established, or why it could not be
};

namespace power_detail {
// Reads a stated curve as two equal-length arrays. Absent/null -> empty. A malformed curve is
// a caller error (the extractor validates every record), so it throws.
inline void curve(const json& p, std::vector<double>& t, std::vector<double>& a) {
    t.clear();
    a.clear();
    const auto ti = p.find("derating_temperature"), ai = p.find("derating_amplitude");
    const bool t_has = ti != p.end() && !ti->is_null(), a_has = ai != p.end() && !ai->is_null();
    if (!t_has && !a_has) return;
    if (!t_has || !a_has || !ti->is_array() || !ai->is_array() || ti->size() != ai->size() ||
        ti->size() < 2)
        throw std::invalid_argument("cross_reference: '" + str(p, "mpn") +
                                    "' derating_temperature/derating_amplitude must be "
                                    "equal-length arrays of >= 2 points");
    for (size_t i = 0; i < ti->size(); ++i) {
        if (!(*ti)[i].is_number() || !(*ai)[i].is_number())
            throw std::invalid_argument("cross_reference: '" + str(p, "mpn") +
                                        "' derating curve holds a non-numeric point");
        t.push_back((*ti)[i].get<double>());
        a.push_back((*ai)[i].get<double>());
        if (i > 0 && !(t[i] > t[i - 1]))
            throw std::invalid_argument("cross_reference: '" + str(p, "mpn") +
                                        "' derating temperatures must strictly increase");
    }
}
// End of the leading full-power run: the temperature up to which the curve carries 100 %.
inline std::optional<double> knee(const std::vector<double>& t, const std::vector<double>& a) {
    std::optional<double> k;
    for (size_t i = 0; i < t.size() && a[i] >= 1.0; ++i) k = t[i];
    return k;
}
inline std::string celsius(double t) {
    char b[32];
    std::snprintf(b, sizeof b, "%g degC", t);
    return b;
}
inline std::string watts(double w) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3g W", w);
    return b;
}
}  // namespace power_detail

inline PowerAtTemperature power_at_rating_temperature(const json& original, const json& cand) {
    using namespace power_detail;
    PowerAtTemperature r;
    const auto o_p = num(original, "power_rating"), s_p = num(cand, "power_rating");
    std::vector<double> ot, oa, st, sa;
    curve(original, ot, oa);
    curve(cand, st, sa);
    std::optional<double> t_ref = num(original, "power_rating_temperature");
    if (!t_ref && !ot.empty()) t_ref = knee(ot, oa);
    if (!t_ref || !o_p || !s_p) return r;  // no basis, or a rating absent: plain compare
    r.applies = true;
    r.t_ref = *t_ref;
    r.orig = *o_p;
    if (!st.empty()) {
        if (r.t_ref < st.front()) {
            r.how = "the substitute's derating curve starts at " + celsius(st.front()) +
                    ", above the original's rating temperature of " + celsius(r.t_ref);
            return r;
        }
        if (r.t_ref > st.back()) {
            r.subst = 0.0;
            r.how = "the substitute's derating curve ends at " + celsius(st.back()) +
                    " — it is not rated to operate at the original's " + celsius(r.t_ref);
            return r;
        }
        for (size_t i = 0; i + 1 < st.size(); ++i) {
            if (r.t_ref >= st[i] && r.t_ref <= st[i + 1]) {
                const double f = (r.t_ref - st[i]) / (st[i + 1] - st[i]);
                r.subst = *s_p * (sa[i] + f * (sa[i + 1] - sa[i]));
                break;
            }
        }
        r.how = "from the substitute's derating curve";
        return r;
    }
    if (const auto s_t = num(cand, "power_rating_temperature")) {
        if (*s_t >= r.t_ref) {
            r.subst = *s_p;
            r.how = "the substitute's rating holds to " + celsius(*s_t);
        } else {
            r.how = "the substitute is rated at " + celsius(*s_t) +
                    " and its record states no derating curve above that";
        }
        return r;
    }
    r.how = "the substitute's record states neither the temperature its rating applies at "
            "nor a derating curve";
    return r;
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
    // A resistor's power at the original's rating temperature is not on the substitute's
    // record (see power_at_rating_temperature): UNVERIFIED, and the grade cannot be drop_in.
    bool power_basis_unverified = false;
    // SMD vs through-hole could not be established for this pair (compare_mount Unknown).
    bool mount_unverified = false;
    // Direction bookkeeping: on each directional parameter we could compare,
    // did the substitute come out strictly ahead of the original, or behind?
    int better = 0, worse = 0;
    // A direction computed on the axes that happened to be present is not a verdict when
    // the axis the match TURNS ON was not among them. The case is specific: the SUBSTITUTE
    // states a hard-gate parameter and the ORIGINAL's record does not. The mirror image —
    // original states it, substitute silent — already rejects above, and when NEITHER states
    // it the axis is missing identically for every candidate, so it cannot skew one against
    // another. Only here did the ranker hold a real number it had no basis to judge, and
    // "upgrade" would be a claim about a comparison that never happened.
    bool direction_blind = false;
    std::vector<std::string> blind_axes;
    // Isat on both sides but at criteria that cannot be brought together (ABT #2042): the
    // same blindness, with its own note (written where it is detected).
    bool isat_basis_blind = false;
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
        // Inside the accepted window the primary score is flat, so a 20 uH part
        // tied an exact 22 uH one and the footprint decided. For RANKING the
        // value always pulls, if gently (the shared primitive stays unchanged).
        if (o_val && s_val && *o_val > 0 && *s_val > 0)
            penalty += opt.primary_weight * kPrimaryProximity * std::fabs(std::log(*s_val / *o_val));
        // The capacitor window allows up to 1.5x on purpose (more bulk C is
        // usually fine), but "recommended" means a drop-in, and a +42 % value is
        // one only where the capacitance is bulk or decoupling — never in a
        // timing, filter or compensation role, which the ranker cannot see.
        if (cat == "capacitor" && pv.verdict == PASS && o_val && s_val && *o_val > 0 &&
            *s_val / *o_val > kCapDropInMaxRatio) {
            demote();
            notes.push_back(signed_pct((*s_val / *o_val - 1.0) * 100.0) +
                            " more capacitance than the original — fine for bulk or decoupling, "
                            "not where the value sets a timing, filter or compensation point");
        }
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
        std::string conflict = cap_family_conflict(cap_family(str(original, "technology")),
                                                   cap_family(str(cand, "technology")));
        // A family of unstated sub-type ("ceramic" with no class) still has a
        // construction: ceramic against film or electrolytic is a different part
        // whatever the class. Compare the coarse groups when the fine families
        // could not be told apart.
        if (conflict.empty()) {
            const std::string og = cap_group(str(original, "technology")),
                              sg = cap_group(str(cand, "technology"));
            if (!og.empty() && !sg.empty() && og != sg)
                conflict = og + " replaced by " + sg + ": a different capacitor construction";
        }
        if (!conflict.empty()) {
            params.push_back({{"name", "family"}, {"verdict", FAIL}});
            notes.push_back(conflict);
            return reject("different capacitor construction family");
        }
    }
    // ── line-safety approval class (IEC 60384-14 X1/X2/X3, Y1/Y2/Y3/Y4) ──────
    // A capacitor across the mains is chosen for its APPROVAL, and the approval is
    // not a number the ranker can trade against another number: X1 -> X2 halves the
    // impulse the part must survive in the place where a shorted capacitor starts a
    // fire. So a declared downgrade is a rejection, not a penalty.
    //
    // The gate is self-selecting on the ORIGINAL: it engages only when the original's
    // own series names a class, which is the only case where a safety approval is a
    // stated requirement. An ordinary 100 nF X7R chip never reaches it.
    //
    // The third outcome is the honest one and the common one. The catalogue has no
    // safety-class field (ABT #677) and the class is readable only from a series
    // name, so a substitute whose series says nothing cannot be checked. That is
    // UNVERIFIED — nothing was compared — and it carries the disqualifying
    // consequence rather than a verdict: demoted out of 'recommended', the FAIL's
    // penalty, the grade capped at major_review, and a note saying which approval
    // went unchecked. Silence used to read as agreement: the six KEMET R46 offered
    // for a WIMA MKP-X1 R were 'recommended' / 'drop_in' / 'upgrade' with voltage
    // "pass" and notes null (ABT #557).
    if (cat == "capacitor") {
        const std::string o_series = str(original, "family");
        const SafetyClass o_class = safety_class(o_series);
        if (o_class.any()) {
            const std::string s_series = str(cand, "family");
            const SafetyClass s_class = safety_class(s_series);
            struct AxisPair {
                char axis;
                std::optional<int> o, s;
            };
            const AxisPair axes[] = {{'X', o_class.x, s_class.x}, {'Y', o_class.y, s_class.y}};
            bool downgraded = false, unverified = false;
            for (const AxisPair& a : axes) {
                if (!a.o) continue;  // the original claims nothing on this axis
                if (!a.s) {
                    unverified = true;
                    notes.push_back(safety_class_unverified(a.axis, *a.o, s_series));
                } else if (*a.s < *a.o) {
                    downgraded = true;
                    notes.push_back(safety_class_downgrade(a.axis, *a.o, *a.s));
                }
            }
            params.push_back({{"name", "safety_class"},
                              {"verdict", downgraded ? FAIL : (unverified ? UNVERIFIED : PASS)}});
            if (downgraded) return reject("line-safety approval class downgrade");
            if (unverified) {
                demote();
                penalty += opt.gate_weight * kVerdictFailPenalty;
                missing_required_data = true;
            }
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
    // An active oscillator's supply range, captured from the PARAM_SPECS verdicts below so
    // the note can state both ranges in volts: a bare "supply_min_V: fail" does not say that
    // a 3.3 V-only part was offered for a 2.5 V rail. It was never compared at all (the
    // spec builder did not pass the range), so a 3.3 V and a 1.8 V XO were graded
    // minor_review / upgrade for a 2.5 V EPSON original.
    bool timebase_supply_fail = false;
    bool timebase_supply_unverified = false;
    bool connector_mount_unverified = false;
    // A passive resonator (quartz crystal, ceramic resonator) has no supply: it is driven
    // by the MCU's oscillator stage. The supply keys mean nothing there and are not
    // compared even if a record happened to carry one.
    const bool timebase_passive =
        cat == "timeBase" && (str(original, "technology") == "quartzCrystal" ||
                              str(original, "technology") == "ceramicResonator");
    // A resistor's power is compared at the original's rating temperature (see
    // power_at_rating_temperature). Computed once, consumed by the power_rating spec below.
    const PowerAtTemperature power_t =
        cat == "resistor" ? power_at_rating_temperature(original, cand) : PowerAtTemperature{};
    // ── PARAM_SPECS verdicts (the shared Heaviside table) ────────────────────
    for (const ParamSpec& spec : params_for(cat)) {
        const bool supply_key = spec.key == "supply_min_V" || spec.key == "supply_max_V";
        if (cat == "timeBase" && supply_key && timebase_passive) continue;
        bool has_data = detail::present(original, spec.key) || detail::present(cand, spec.key);
        if (spec.key == "saturation_current")
            has_data = has_data || detail::present(original, "saturation_points") ||
                       detail::present(cand, "saturation_points");
        if (!has_data) continue;
        const bool power_at_t = cat == "resistor" && spec.key == "power_rating" && power_t.applies;
        const ParamOutcome outcome =
            !power_at_t     ? compare_param(spec, original, cand)
            : power_t.subst ? compare_numeric(spec, power_t.orig, power_t.subst)
                            : ParamOutcome(UNVERIFIED);
        const std::string verdict = outcome.verdict;
        params.push_back({{"name", spec.key}, {"verdict", verdict}});
        if (cat == "connector" && verdict == FAIL) {
            if (spec.key == "pitch_mm") connector_pitch_conflict = true;
            if (spec.key == "contact_plating") connector_plating_conflict = true;
            if (spec.key == "termination") connector_termination_conflict = true;
            // The board attach (smt / tht / pressFit / skedd / panel / cable) is a
            // different board process, not a parameter: a press-fit part needs a press
            // and plated holes sized for it, an SMT one a reflow pass on pads. A hard
            // identity key, so this FAIL rejects. Say which pair decided it: the generic
            // SMD-vs-leaded gate cannot classify press-fit, panel or cable, and an SMT
            // original used to take a press-fit substitute with no mount verdict at all.
            if (spec.key == "mounting")
                notes.push_back("board mount differs: " + str(original, "mounting") + " -> " +
                                str(cand, "mounting") +
                                " — a different board-attach process, not a drop-in");
        }
        // The original states its board attach and the substitute's record does not:
        // whether it goes on the same board process is unknown, so the swap cannot be
        // called a drop-in. Capped like an unverified footprint (minor_review), with a
        // note — WT16PRD8CD (no mountingStyle, no pitch) came back drop_in for an SMT
        // Samtec original.
        if (cat == "connector" && spec.key == "mounting" && verdict == UNVERIFIED &&
            detail::present(original, spec.key)) {
            connector_mount_unverified = true;
            notes.push_back("the original is " + str(original, "mounting") +
                            "-mounted; the substitute's record states no board mount — "
                            "confirm it uses the same board-attach process");
        }
        if (cat == "timeBase" && supply_key) {
            if (verdict == FAIL) timebase_supply_fail = true;
            if (verdict == UNVERIFIED && detail::present(original, spec.key))
                timebase_supply_unverified = true;
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
        if (power_at_t) {
            using power_detail::celsius;
            using power_detail::watts;
            const double s_nominal = *s;
            // Penalty and direction are about the power at T_ref too, not the nominal.
            o = power_t.orig;
            s = power_t.subst;
            if (!power_t.subst) {
                // Not on record is not a pass: the original's rating is stated at a
                // temperature, and nothing says what the substitute delivers there. Capped
                // the way an unverified footprint is (power_basis_unverified below).
                demote();
                power_basis_unverified = true;
                notes.push_back("power rating not comparable at the original's rating "
                                "temperature: the original is rated " + watts(*power_t.orig) +
                                " at " + celsius(power_t.t_ref) + ", and " + power_t.how +
                                " — confirm the substitute's derating from its datasheet");
            } else if (*power_t.subst != s_nominal) {
                notes.push_back("power compared at the original's rating temperature (" +
                                celsius(power_t.t_ref) + "): the substitute's " + watts(s_nominal) +
                                " rating delivers " + watts(*power_t.subst) + " there (" +
                                power_t.how + ") against the original's " +
                                watts(*power_t.orig));
            }
        }
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
        if (numeric) {
            // Isat is compared at a common inductance-drop criterion, so its direction is too:
            // the raw scalars may be quoted at 10 % and 30 % drop and say nothing about which
            // part saturates first. With no common criterion there is no direction to count,
            // and the note names both bases (ABT #2042).
            std::optional<double> od = o, sd = s;
            bool basis_mismatch = false;
            if (spec.key == "saturation_current") {
                const auto op = isat::resolve_points(original), sp = isat::resolve_points(cand);
                if (!op.empty() && !sp.empty() && (isat::has_basis(op) || isat::has_basis(sp))) {
                    if (auto t = isat::common_percent_drop(op, sp)) {
                        od = isat::isat_at_percent_drop(op, *t);
                        sd = isat::isat_at_percent_drop(sp, *t);
                    } else {
                        basis_mismatch = true;
                        notes.push_back("saturation current quoted at different criteria (" +
                                        isat_basis(op) + " on the original, " + isat_basis(sp) +
                                        " on the substitute) — not directly comparable; confirm "
                                        "the substitute's Isat at the original's criterion");
                    }
                }
            }
            // A WARN or FAIL on a directional parameter IS a regression on it, whatever the
            // ratio: a 34.9 A Isat against 36 A warned, sat inside the +/-5 % neutral band
            // and the row came back 'upgrade' beside its own warn (ABT #2039).
            if (basis_mismatch) {
                // The hard axis could not be weighed, exactly the direction_blind situation:
                // a reassuring direction would be a claim about a comparison not made.
                if (is_hard_param(cat, spec.key)) isat_basis_blind = true;
            } else if (verdict == WARN || verdict == FAIL) {
                ++worse;
            } else {
                note_direction(spec.dir, od, sd);
            }
        }
        // The original states only a rated current and the substitute saturates below it
        // (compare_saturation_current). That axis WAS weighed — and came out behind — so it
        // is not reported as blind; say which two figures were set against each other.
        const bool isat_vs_rating = spec.key == "saturation_current" &&
                                    (verdict == WARN || verdict == FAIL) &&
                                    isat_below_original_rating(original, cand);
        if (isat_vs_rating)
            notes.push_back("the original's record states no saturation current, but the "
                            "substitute saturates below the original's rated current — it "
                            "cannot carry the current the original is rated for without "
                            "losing inductance");
        if (numeric && is_hard_param(cat, spec.key) && !o && s && !isat_vs_rating) {
            direction_blind = true;
            blind_axes.push_back(param_label(spec.key));
        }
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

    if (timebase_supply_fail)
        notes.push_back("supply range does not cover the original's: the original runs from " +
                        supply_range(original) + ", the substitute from " + supply_range(cand) +
                        " — it will not run correctly from the original's rail");
    else if (timebase_supply_unverified)
        notes.push_back("the original runs from " + supply_range(original) +
                        "; the substitute's record states no supply range, so the rail could "
                        "not be checked — confirm it from the datasheet");

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

    // ── transient protection (tvs / esd originals) ───────────────────────────
    // A protection diode is chosen for the surge it absorbs, the level it clamps to and the
    // capacitance it hangs on the line — none of which Vrrm (its standoff) says. Nothing of
    // it was compared: a 400 W SMAJ came back 'upgrade' for a 15 kW 15KP, a 90 pF part
    // 'recommended' for a 1.2 pF one, a unidirectional part for a bidirectional one
    // (ABT #2036, #2038, #2040, #2041, #2048-#2051). Rules, Alf 2026-10-25:
    //  * Ppk / Ipp / Vc are compared only between ratings stated at the SAME impulse; a
    //    different or unstated waveform leaves the parameter UNVERIFIED with a note, and for
    //    the surge ratings caps the grade as missing required data (not a rejection).
    //  * a breakdown window reaching above the original's raises the protection level: WARN.
    //  * uni-directional for a bi-directional original (decoded from the classic series
    //    order code) FAILS; an undecodable polarity is UNVERIFIED.
    const std::string o_tech = str(original, "technology");
    const bool protection = cat == "diode" && (o_tech == "tvs" || o_tech == "esd");
    bool surge_blind = false;
    if (protection) {
        auto fmt = [](double v, const char* unit) {
            char b[48];
            std::snprintf(b, sizeof b, "%g %s", v, unit);
            return std::string(b);
        };
        auto at_wf = [](const std::string& wf) {
            return wf.empty() ? std::string("at no stated waveform") : "at " + wf + " us";
        };
        // One waveform-bound rating. `cap` = its unknowns cap the grade (the surge ratings).
        auto waveform_rated = [&](const char* key, const char* wf_key, Dir dir, double tol,
                                  const char* label, const char* unit, bool cap,
                                  const char* consequence) {
            const auto o = num(original, key), s = num(cand, key);
            if (!o && !s) return;
            const std::string ow = str(original, wf_key), sw = str(cand, wf_key);
            if (!o) {  // the original states nothing to hold the substitute to
                params.push_back({{"name", key}, {"verdict", UNVERIFIED}});
                return;
            }
            auto unverified = [&](const std::string& why) {
                params.push_back({{"name", key}, {"verdict", UNVERIFIED}});
                notes.push_back(why);
                if (cap) {
                    demote();
                    penalty += opt.gate_weight * kVerdictFailPenalty;
                    missing_required_data = true;
                    // The rating the protection match turns on went unweighed, so a
                    // reassuring direction would be a claim about a comparison not made.
                    surge_blind = true;
                }
            };
            if (!s) {
                unverified(std::string("substitute's record carries no ") + label +
                           " — the original states " + fmt(*o, unit) + " " + at_wf(ow) +
                           "; confirm it from the datasheet before this swap");
                return;
            }
            if (ow.empty() || sw.empty() || ow != sw) {
                unverified(std::string(label) + " stated at different or unstated impulses (" +
                           fmt(*o, unit) + " " + at_wf(ow) + " on the original, " +
                           fmt(*s, unit) + " " + at_wf(sw) +
                           " on the substitute) — not comparable; confirm the substitute's " +
                           label + " at the original's waveform");
                return;
            }
            const ParamSpec spec{key, dir, tol, std::nullopt, false, false, nullptr};
            const char* v = compare_numeric(spec, o, s).verdict;
            params.push_back({{"name", key}, {"verdict", v}});
            if (v == FAIL || v == WARN) {
                demote();
                penalty += opt.gate_weight * (v == FAIL ? kVerdictFailPenalty : kVerdictWarnPenalty);
                ++worse;
                notes.push_back(std::string(label) + " " + fmt(*s, unit) + " against the original's " +
                                fmt(*o, unit) + " " + at_wf(ow) + " (" +
                                signed_pct(100.0 * (*s / *o - 1.0)) + ") — " + consequence);
            } else {
                note_direction(dir, o, s);
            }
        };
        waveform_rated("peak_pulse_power", "peak_pulse_power_waveform", Dir::Higher, 0.9,
                       "peak pulse power", "W", true,
                       "the substitute absorbs less surge energy than the original is rated for");
        waveform_rated("peak_pulse_current", "peak_pulse_current_waveform", Dir::Higher, 0.9,
                       "peak pulse current", "A", true,
                       "the substitute survives less surge current than the original is rated for");
        waveform_rated("clamping_voltage", "clamping_waveform", Dir::Lower, 1.05,
                       "clamping voltage", "V", false,
                       "the protected circuit sees a higher voltage during a surge; re-check its "
                       "rating");

        // Junction capacitance loads the line being protected (a high-speed I/O is what an
        // ESD part guards). Stated without an impulse, so compared directly.
        if (const auto oc = num(original, "junction_capacitance"),
            sc = num(cand, "junction_capacitance");
            oc || sc) {
            const ParamSpec spec{"junction_capacitance", Dir::Lower, 2.0, std::nullopt, false,
                                 false, nullptr};
            const char* v = compare_numeric(spec, oc, sc).verdict;
            params.push_back({{"name", "junction_capacitance"}, {"verdict", v}});
            if (v == FAIL || v == WARN) {
                demote();
                penalty += opt.gate_weight * (v == FAIL ? kVerdictFailPenalty : kVerdictWarnPenalty);
                ++worse;
                notes.push_back("junction capacitance " + fmt(*sc * 1e12, "pF") +
                                " against the original's " + fmt(*oc * 1e12, "pF") +
                                " — more load on the protected line; check its signal integrity");
            } else if (oc && !sc) {
                notes.push_back("substitute's record carries no junction capacitance — the "
                                "original states " + fmt(*oc * 1e12, "pF") +
                                "; confirm the load on the protected line from the datasheet");
            } else {
                note_direction(Dir::Lower, oc, sc);
            }
        }

        // Breakdown window: where the part begins to conduct. Upper ends when both state
        // one, else lower ends.
        {
            const auto olo = num(original, "vz_min_V"), ohi = num(original, "vz_max_V");
            const auto slo = num(cand, "vz_min_V"), shi = num(cand, "vz_max_V");
            std::optional<double> oe, se;
            if (ohi && shi) oe = ohi, se = shi;
            else if (olo && slo) oe = olo, se = slo;
            if (oe && se) {
                const bool above = *se > *oe * (1.0 + 1e-3);
                params.push_back({{"name", "breakdown_window"}, {"verdict", above ? WARN : PASS}});
                if (above) {
                    demote();
                    penalty += opt.gate_weight * kVerdictWarnPenalty;
                    ++worse;
                    auto band = [&](std::optional<double> lo, std::optional<double> hi) {
                        return (lo ? volts(*lo) : std::string("?")) + "-" +
                               (hi ? volts(*hi) : std::string("?")) + " V";
                    };
                    notes.push_back("breakdown window " + band(slo, shi) +
                                    " reaches above the original's " + band(olo, ohi) +
                                    " — the substitute starts to conduct later, so the protection "
                                    "level rises; re-check what the protected circuit tolerates");
                }
            } else if (olo || ohi || slo || shi) {  // one side states none: not comparable
                params.push_back({{"name", "breakdown_window"}, {"verdict", UNVERIFIED}});
            }
        }

        // Standoff band, two-sided (Alf 2026-10-25, ABT #2034): the standoff is the rail the
        // part guards, so one more than 10 % above the original's clamps too late for it —
        // a 12 V part on a 5.5 V line. WARN, not FAIL.
        if (const auto ov = num(original, "vrrm"), sv = num(cand, "vrrm");
            ov && sv && *sv > *ov * 1.10) {
            params.push_back({{"name", "standoff_band"}, {"verdict", WARN}});
            demote();
            penalty += opt.gate_weight * kVerdictWarnPenalty;
            ++worse;
            notes.push_back("standoff " + volts(*sv) + " V against the original's " + volts(*ov) +
                            " V (" + signed_pct(100.0 * (*sv / *ov - 1.0)) +
                            ") — a protection part for a higher rail; it clamps later than the "
                            "protected circuit expects");
        }
        // Standoff BELOW the original's (Qarlos 2026-11-08): the original's standoff is the
        // circuit's working voltage, and a part whose standoff is lower is outside its
        // rated off-state there. The vrrm rating below only WARNED inside its 0.9-0.95
        // band, so a 66 V 15KPA066C came back minor_review for a 70 V 15KP70. Rule
        // (conservative, for the owner to confirm):
        //  * substitute's breakdown MINIMUM at or below the original's standoff: it may be
        //    in breakdown — conducting — at the circuit's working voltage. REJECTED.
        //  * otherwise a standoff below the original's: FAIL (caps at major_review); its
        //    leakage is unspecified at the working voltage.
        if (const auto ov = num(original, "vrrm"), sv = num(cand, "vrrm");
            ov && sv && *sv < *ov * (1.0 - 1e-3)) {
            const auto s_vbr_min = num(cand, "vz_min_V");
            params.push_back({{"name", "standoff_band"}, {"verdict", FAIL}});
            if (s_vbr_min && *s_vbr_min <= *ov) {
                notes.push_back("standoff " + volts(*sv) + " V and breakdown minimum " +
                                volts(*s_vbr_min) + " V against the original's " + volts(*ov) +
                                " V standoff — the substitute can conduct at the circuit's "
                                "working voltage");
                return reject("breakdown minimum at or below the original's working voltage");
            }
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            ++worse;
            notes.push_back("standoff " + volts(*sv) + " V against the original's " + volts(*ov) +
                            " V (" + signed_pct(100.0 * (*sv / *ov - 1.0)) +
                            ") — below the circuit's working voltage; its leakage there is not "
                            "specified");
        }

        // Polarity.
        const TvsPolarity op = tvs_polarity_from_mpn(str(original, "mpn")),
                          sp = tvs_polarity_from_mpn(str(cand, "mpn"));
        if (op != TvsPolarity::Unknown || sp != TvsPolarity::Unknown) {
            if (op == TvsPolarity::Bi && sp == TvsPolarity::Uni) {
                params.push_back({{"name", "polarity"}, {"verdict", FAIL}});
                demote();
                penalty += opt.gate_weight * kVerdictFailPenalty;
                ++worse;
                notes.push_back("unidirectional substitute for a bidirectional original (decoded "
                                "from the part numbers) — it does not clamp a negative excursion, "
                                "it conducts it as a forward diode");
            } else if (op == TvsPolarity::Uni && sp == TvsPolarity::Bi) {
                // The mirror image is no silent pass either (Qarlos 2026-11-08). A
                // unidirectional original clamps a NEGATIVE excursion as a forward diode at
                // about -1 V; a bidirectional part lets it swing to its reverse breakdown,
                // tens of volts for a 58 V standoff, and no longer conducts the reverse-
                // polarity fault a uni part is often placed to blow a fuse with. A
                // different clamping characteristic: FAIL, capping at major_review, the
                // same weight as the opposite case (conservative; for the owner to confirm).
                params.push_back({{"name", "polarity"}, {"verdict", FAIL}});
                demote();
                penalty += opt.gate_weight * kVerdictFailPenalty;
                ++worse;
                notes.push_back("bidirectional substitute for a unidirectional original (decoded "
                                "from the part numbers) — a negative excursion is no longer "
                                "clamped near -1 V by forward conduction but only at the "
                                "substitute's reverse breakdown; confirm what the protected "
                                "circuit tolerates below ground");
            } else if (op == TvsPolarity::Unknown || sp == TvsPolarity::Unknown) {
                params.push_back({{"name", "polarity"}, {"verdict", UNVERIFIED}});
                if (op == TvsPolarity::Bi)
                    notes.push_back("the original is bidirectional (decoded from its part number); "
                                    "the substitute's polarity cannot be read from its record or "
                                    "part number — confirm it clamps both polarities");
            } else {
                params.push_back({{"name", "polarity"}, {"verdict", PASS}});
            }
        }
    }

    // ── zener voltage (zener originals) ──────────────────────────────────────
    // Vz IS the zener. Vrrm above only compares the marking as a HigherBetter rating, so a
    // 24 V zener came back drop_in / upgrade for a 22 V (21.2 V) one and 11-20 V zeners
    // drop_in / recommended for a 9.1 V one (Qarlos 2026-11-08). Rule (conservative, for
    // the owner to confirm):
    //  * the original's window is its stated breakdown band [vz_min, vz_max]; with no band
    //    on record, its marked/resolved Vz (vrrm) is the only datum and the window is that
    //    point +/- kZenerPointTol (1 %, well inside the ~8-10 % E24 step, so it separates
    //    neighbouring voltages and absorbs a band midpoint against a marked nominal).
    //  * the substitute's Vz (its nominal, else its band midpoint — what vrrm carries)
    //    outside that window: a different zener voltage, REJECTED.
    //  * inside, and the substitute's guaranteed band also inside: PASS.
    //  * inside, but its guaranteed band reaches outside the original's: FAIL (caps at
    //    major_review) — the part is not guaranteed to regulate where the circuit was
    //    designed to.
    //  * inside, substitute states no band against an original that does: UNVERIFIED. Not a
    //    demotion: the ABT #488 decision treats an absent band as unknown, not a regression,
    //    and its note (above) already names the window going unchecked.
    //  * no Vz on either side: UNVERIFIED, and with no Vz on the substitute the grade is
    //    capped as missing required data.
    // web/src/crossref.js pre-filters the pool at exactly these edges (kZenerEdgeTol, kZenerPointTol).
    const bool zener = cat == "diode" && o_tech == "zener";
    if (zener) {
        constexpr double kZenerEdgeTol = 1e-3;   // rounding slack on a stated band edge
        constexpr double kZenerPointTol = 0.01;  // window of a band-less original
        const auto oc = num(original, "vrrm"), olo = num(original, "vz_min_V"),
                   ohi = num(original, "vz_max_V");
        const auto sc = num(cand, "vrrm"), slo = num(cand, "vz_min_V"),
                   shi = num(cand, "vz_max_V");
        const bool o_band = olo && ohi && *ohi >= *olo;
        const bool s_band = slo && shi && *shi >= *slo;
        std::optional<double> wlo, whi;
        if (o_band) {
            wlo = *olo * (1.0 - kZenerEdgeTol);
            whi = *ohi * (1.0 + kZenerEdgeTol);
        } else if (oc) {
            wlo = *oc * (1.0 - kZenerPointTol);
            whi = *oc * (1.0 + kZenerPointTol);
        }
        std::optional<double> s_center = sc;
        if (!s_center && s_band) s_center = 0.5 * (*slo + *shi);
        auto band = [](double lo, double hi) { return volts(lo) + "-" + volts(hi) + " V"; };
        const std::string o_window = o_band ? band(*olo, *ohi)
                                     : oc   ? volts(*oc) + " V (no band on record)"
                                            : std::string();
        if (!wlo) {
            params.push_back({{"name", "zener_voltage"}, {"verdict", UNVERIFIED}});
        } else if (!s_center) {
            params.push_back({{"name", "zener_voltage"}, {"verdict", UNVERIFIED}});
            notes.push_back("substitute's record states no zener voltage — the original's is " +
                            o_window + "; confirm it from the datasheet before this swap");
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            missing_required_data = true;
        } else if (*s_center < *wlo || *s_center > *whi) {
            params.push_back({{"name", "zener_voltage"}, {"verdict", FAIL}});
            notes.push_back("zener voltage " + volts(*s_center) + " V is outside the original's " +
                            o_window + " — a different zener voltage, not a substitute");
            return reject("a different zener voltage");
        } else if (o_band && s_band && (*slo < *wlo || *shi > *whi)) {
            params.push_back({{"name", "zener_voltage"}, {"verdict", FAIL}});
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            ++worse;
            notes.push_back("the substitute guarantees Vz " + band(*slo, *shi) +
                            ", reaching outside the original's " + o_window +
                            " — not guaranteed to regulate where the circuit was designed to");
        } else if (o_band && !s_band) {
            params.push_back({{"name", "zener_voltage"}, {"verdict", UNVERIFIED}});
        } else {
            params.push_back({{"name", "zener_voltage"}, {"verdict", PASS}});
        }
    }

    // ── forward-current basis (multi-die diode packages) ─────────────────────
    // SAS diode.json: for a package declared part.dieConfiguration 'dual', every electrical
    // value is PER DIODE (per leg), never the package total; a record that declares no
    // configuration is, by the schema, one die. A rating is only comparable on the same
    // basis, and the record is the only admissible witness of the basis — never the MPN.
    // Rule (Qarlos 2026-11-08, run 4 seed 20292785: MBRF1560CT, 15 A whole-device, graded
    // if_avg 'pass' against VFT2060C's 10 A per leg):
    //  * both sides state their configuration, or neither does: compared as stated (both
    //    per diode by the schema's definition);
    //  * one side states a MULTI-die package and the other states none: the other side's
    //    figure cannot be shown to be per leg — catalogue imports routinely omit the field on
    //    dual parts and copy the vendor table's whole-device current — so if_avg is
    //    UNVERIFIED, the grade is capped as missing required data (the current rating went
    //    unweighed, exactly as an unstated surge waveform caps a TVS) and the direction is
    //    unknown — UNLESS the verdict is the same under every reading of the unstated basis
    //    (see `settled` below), in which case it is compared as stated. Decision for the
    //    owner to confirm: the cap is major_review, the same as a rating the substitute does
    //    not state at all.
    bool current_basis_blind = false;
    std::string current_basis_note;
    if (cat == "diode") {
        const std::string odc = str(original, "die_configuration"),
                          sdc = str(cand, "die_configuration");
        auto multi = [](const std::string& c) { return !c.empty() && c != "single"; };
        const auto oi = num(original, "if_avg"), si = num(cand, "if_avg");
        const bool cand_basis_unknown = multi(odc) && sdc.empty();
        const bool orig_basis_unknown = multi(sdc) && odc.empty();
        // An unstated basis only ever moves a figure DOWN when it is brought to per leg (a
        // whole-device figure divides among the legs), so the stated number is the most
        // generous per-leg reading of it. A verdict that holds under every reading is not
        // unknown and is kept: a substitute whose figure FAILS even read as per leg (1 A
        // against 10 A per leg) fails, and one that PASSES against the original's figure
        // read as per leg (the strictest requirement it can mean) passes.
        std::optional<ScoreResult> best;
        if (oi && si && (cand_basis_unknown || orig_basis_unknown))
            for (const auto& r : critical_ratings(cat))
                if (std::string(r.key) == "if_avg")
                    best = score_directional(oi, si, r.mode, r.warn_factor, r.gate_factor);
        const bool settled = best && ((cand_basis_unknown && best->verdict == FAIL) ||
                                      (orig_basis_unknown && best->verdict == PASS));
        if (best && !settled) {
            auto amps = [](double v) {
                char b[32];
                std::snprintf(b, sizeof b, "%g A", v);
                return std::string(b);
            };
            auto side = [&](const std::string& who, const std::string& dc, double a) {
                return dc.empty() ? who + "'s " + amps(a) + " (its record states no die "
                                                            "configuration, so per leg or "
                                                            "whole device is unknown)"
                                  : who + "'s " + amps(a) + " per diode (a '" + dc +
                                        "' package)";
            };
            current_basis_note =
                "forward current not comparable: the " + side("original", odc, *oi) +
                " against the " + side("substitute", sdc, *si) +
                " — confirm the per-leg I_F(AV) of both from their datasheets before this swap";
        }
    }

    // ── critical ratings (Vds / Vrrm / rated voltage / Id / If) ──────────────
    for (const auto& r : critical_ratings(cat)) {
        auto o = num(original, r.key), s = num(cand, r.key);
        if (!o && !s) continue;
        if (!current_basis_note.empty() && std::string(r.key) == "if_avg") {
            params.push_back({{"name", r.key}, {"verdict", UNVERIFIED}});
            notes.push_back(current_basis_note);
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            missing_required_data = true;
            current_basis_blind = true;
            continue;
        }
        if (zener && std::string(r.key) == "vrrm") {
            // A zener's vrrm is its Vz, and a Vz is matched, not "more is better": as a
            // HigherBetter rating any Vz a hair below the original's (21.0 V inside a
            // 20.64-21.71 V window) WARNED. The zener rule above has already rejected every
            // Vz outside the original's window, so a stated pair here is a marking inside
            // it. No over-dimensioning penalty and no direction either way.
            params.push_back({{"name", r.key}, {"verdict", (o && s) ? PASS : UNVERIFIED}});
            if (r.hard && !o && s) {
                direction_blind = true;
                blind_axes.push_back(param_label(r.key));
            }
            continue;
        }
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
        if (v.verdict == WARN || v.verdict == FAIL)
            ++worse;  // a shortfall verdict is a regression, inside the neutral band or not
        else if (!(protection && std::string(r.key) == "vrrm"))
            // A protection diode's standoff is the rail it guards, not a margin: a 12 V
            // standoff part on a 5.5 V line clamps later, which is no upgrade (ABT #2034).
            note_direction(r.mode == Mode::HigherBetter ? Dir::Higher : Dir::Lower, o, s);
        if (r.hard && !o && s) {
            direction_blind = true;
            blind_axes.push_back(param_label(r.key));
        }
    }

    // ── temperature grade tie-break when the original states none ───────────
    if (cat == "capacitor" && !num(original, "temp_max_C")) {
        if (const auto t = num(cand, "temp_max_C"); t && *t < 125.0)
            penalty += kUnknownTempGradePenaltyPerC * (125.0 - *t);
    }

    // ── operating-point requirements (the circuit, not the original) ────────
    for (const auto& [key, need] : opt.requirements) {
        const auto s = num(cand, key.c_str());
        const std::string label = param_label(key);
        char buf[160];
        if (!s) {
            params.push_back({{"name", "required_" + key}, {"verdict", WARN}});
            std::snprintf(buf, sizeof buf, "%s not stated — cannot confirm it meets the circuit's %g",
                          label.c_str(), need);
            notes.emplace_back(buf);
            missing_required_data = true;
            demote();
            penalty += opt.gate_weight * kVerdictWarnPenalty;
            continue;
        }
        if (*s < need) {
            params.push_back({{"name", "required_" + key}, {"verdict", FAIL}});
            std::snprintf(buf, sizeof buf, "%s %g is below the circuit's requirement of %g",
                          label.c_str(), *s, need);
            notes.emplace_back(buf);
            return reject("does not meet the circuit's operating-point requirement");
        }
        params.push_back({{"name", "required_" + key}, {"verdict", PASS}});
        penalty += opt.overdim_weight * over_dimensioning_penalty(need, s, 1.0);
    }

    // ── physical fit ─────────────────────────────────────────────────────────
    if (opt.check_footprint) {
        const std::string o_pkg = str(original, "package").empty() ? str(original, "case_code")
                                                                   : str(original, "package");
        const std::string s_pkg =
            str(cand, "package").empty() ? str(cand, "case_code") : str(cand, "package");
        // Mount type: an SMD part cannot stand in for a leaded one. Skipped for
        // categories whose package strings vary too much by series to classify.
        const std::string o_mt = str(original, "mount"), s_mt = str(cand, "mount");
        const MountMatch mount_match = mount_gate_applies(cat)
                                           ? compare_mount(o_mt, s_mt, o_pkg, s_pkg)
                                           : MountMatch::Same;
        // Name what each side's class was read from — its EXPLICIT assembly type when the
        // record states one, its package string otherwise (mount_class's order). Always
        // quoting the packages printed a half-empty "mount type incompatible:  -> 8x8x10"
        // for the many records that state a mount type but carry no package string.
        auto mount_src = [](const std::string& mt, const std::string& pkg) {
            return !normalize_mount(mt).empty() ? mt : pkg;
        };
        if (mount_match == MountMatch::Incompatible) {
            params.push_back({{"name", "mounting"}, {"verdict", FAIL}});
            notes.push_back("mount type incompatible: " + mount_src(o_mt, o_pkg) + " -> " +
                            mount_src(s_mt, s_pkg));
            return reject("mount type incompatible (SMD vs leaded)");
        }
        const std::string o_cls = mount_class(o_mt, o_pkg), s_cls = mount_class(s_mt, s_pkg);
        if (mount_match == MountMatch::Unknown && (!o_cls.empty() || !s_cls.empty())) {
            // One record states its mount and the other does not: not on record is not
            // compatible — SMD vs through-hole is a different board process. Capped like
            // an unverified footprint. (Neither stating one leaves the axis silent, the
            // same known-vs-absent rule the height axis follows.)
            params.push_back({{"name", "mounting"}, {"verdict", UNVERIFIED}});
            mount_unverified = true;
            auto said = [](const std::string& cls) {
                return cls.empty() ? std::string("not stated") : cls;
            };
            notes.push_back("mount type not established (original: " + said(o_cls) +
                            ", substitute: " + said(s_cls) +
                            ") — confirm SMD vs through-hole from the datasheets");
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
                case_identity(cat, oc) != case_identity(cat, sc)) {
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
        // A LAND to fit to is what this branch needs, not merely a drawing. An
        // original whose record states only a height has no pads to compare
        // against, so it belongs to the "original states no package dimensions"
        // branch below — which is where it went before a land-less drawing could
        // reach here at all.
        if (o_dims && has_any_land(*o_dims) && !out.contains("footprint")) {
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
                // Three different silences, and the note must name the right one.
                // "mechanical dimensions unavailable" is true only of a record that
                // states NONE; printing it beside a row that lists a height (or a
                // length) overstates the gap and buries the fact that the stated
                // axis matches (ABT #499). What is missing is named instead.
                if (!s_dims || (!has_any_land(*s_dims) && !s_dims->height)) {
                    notes.push_back(
                        "mechanical dimensions unavailable for the substitute — footprint fit "
                        "could not be verified; confirm it fits the original's land pattern");
                } else if (has_any_land(*s_dims)) {
                    notes.push_back("only part of the land is on record (" + land_mm(*s_dims) +
                                    " vs " + land_mm(*o_dims) +
                                    ") — what is stated does not rule the fit out, but does not "
                                    "confirm it either; verify the land pattern");
                } else {
                    // A drawing with a height and no land. It says something real —
                    // the clearance axis above judges it — but nothing at all about
                    // the pads, so the footprint stays unverified and the note says
                    // which of the two it is talking about.
                    notes.push_back("the substitute's record states its height (" +
                                    mm(*s_dims->height) +
                                    " mm) but neither land dimension, so its fit to the "
                                    "original's " +
                                    land_mm(*o_dims) +
                                    " land could not be verified; confirm the land pattern");
                }
            }
        } else if ((!o_dims || !has_any_land(*o_dims)) && footprint_is_the_body(cat) &&
                   !out.contains("footprint")) {
            // The same rung, seen from the ORIGINAL's side. Every branch above needs a
            // footprint for the ORIGINAL; with none, the package axis vanished from the
            // verdict table entirely and "drop_in" — the grade that asserts the part
            // solders into the existing land pattern — was awarded with zero mechanical
            // information about the original. Murata LQP02HQ30NHZ2E, a 01005 (0402
            // metric, 0.4 x 0.2 mm) chip, came back drop_in / penalty 0 / notes null
            // against a Pulse BSCH0010050533NJCP whose 1005-metric body is stated only
            // in its free-text description — indistinguishable from the two candidates
            // that ARE the original's size (ABT #545). The axis is always reported now:
            // silence is what let the top grade through, and reporting UNVERIFIED is
            // what the tool already does for an electrical parameter nobody can check.
            const std::string oc = str(original, "case_code"), sc = str(cand, "case_code");
            // The case-kept gate above only publishes its NEGATIVE. Its positive is a
            // verdict too, and for the families whose case code IS the land pattern it
            // is the one way the fit can be established with no drawing on either side:
            // two records naming the same package name the same pads. Magnetics and
            // chip beads are excluded here exactly as they are there — a magnetic
            // "1210" is an EIA chip or a molded power inductor, so equal codes are not
            // an equal land.
            if (cat != "magnetic" && cat != "chipBead" && !oc.empty() && !sc.empty() &&
                case_identity(cat, oc) == case_identity(cat, sc)) {
                out["footprint"] = "fits";
                params.push_back({{"name", "footprint"}, {"verdict", PASS}});
            } else {
                out["footprint"] = "unknown";
                params.push_back({{"name", "footprint"}, {"verdict", UNVERIFIED}});
                // Name what the substitute IS, so an engineer can see the two case
                // sizes for themselves even though the ranker cannot compare them.
                // Only where the substitute states a package at all: where neither
                // record states one the UNVERIFIED verdict is the whole story, and the
                // gap belongs to the catalogue, not to this candidate.
                std::string what = s_pkg;
                if (s_dims && has_land(*s_dims)) what += (what.empty() ? "" : ", ") + land_mm(*s_dims);
                if (!what.empty())
                    notes.push_back(
                        "the original's record states no land dimensions, so its land pattern "
                        "could not be established — this substitute is " + what +
                        "; footprint fit is UNVERIFIED, confirm the pads before substituting");
            }
        }
        // ── height / clearance, its own axis ─────────────────────────────────
        // Not part of the land pattern, so it is judged and reported separately
        // and runs whatever the footprint verdict was — a part can need a new land
        // pattern AND more headroom, and an engineer has to be told both. Only a
        // TALLER substitute is a risk; a shorter one keeps the pads and asks less
        // of the enclosure.
        HeightFit hfit = height_fit(o_dims, s_dims);
        // A magnetic is verified ONLY from its drawing (see dims_of), and its height is the
        // axis that varies most across one footprint: a 2.5 x 2.0 mm body ships 1.0 to 1.2 mm
        // tall. The original stating a height and the substitute's record stating none left
        // the axis silent, and VLS252012HBX-100M-1 came back drop_in against a 1.0 mm
        // LQH2HPH100MGR (ABT #2029). The clearance is UNVERIFIED and the grade is capped the
        // way an unverified footprint caps it.
        //
        // The same silence is no different on any other family: a chip resistor, a capacitor
        // or a transistor whose original states a height and whose substitute states none
        // has an unverified clearance too, and its axis must say so rather than vanish (or,
        // before the table height was dropped in dims_of, read 'pass' from a typical value).
        if (hfit == HeightFit::Unknown && o_dims &&
            o_dims->height && (!s_dims || !s_dims->height) && !out.contains("height_fit")) {
            out["height_fit"] = "unknown";
            params.push_back({{"name", "height"}, {"verdict", UNVERIFIED}});
            demote();
            notes.push_back("the substitute's record states no height (the original is " +
                            mm(*o_dims->height) +
                            " mm tall), so vertical clearance could not be verified; confirm it "
                            "from the datasheet");
        }
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
        // The candidate's Rds(on) with no stated test Vgs, against an original whose is
        // stated: the two numbers may not describe the same drive, so the rds_on verdict
        // above is a comparison of unknown basis. Say so instead of letting the "pass"
        // stand alone — BSC028N08NS5's 2.8 mOhm (no Vgs on record) passed against a
        // 3.4 mOhm-at-10 V original with nothing beside it (ABT #2045).
        if (o_rv && !s_rv && num(cand, "rds_on")) {
            params.push_back({{"name", "rds_on_basis"}, {"verdict", UNVERIFIED}});
            notes.push_back("the substitute's record does not state the Vgs its Rds(on) is "
                            "specified at (the original's is at " + volts(*o_rv) +
                            " V), so the Rds(on) comparison may not hold at the original's "
                            "drive — confirm it from the datasheet");
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
        // Records state CL in farads (load_capacitance) or in pF — the parameter
        // table's own key (load_capacitance_pF); read either, or the gate never
        // sees a value from the latter and waves every CL through.
        auto load_cl = [](const json& j) -> std::optional<double> {
            if (auto f = num(j, "load_capacitance")) return f;
            if (auto pf = num(j, "load_capacitance_pF")) return *pf * 1e-12;
            return std::nullopt;
        };
        auto o_cl = load_cl(original), s_cl = load_cl(cand);
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
    //
    // Three answers, not two. A family whose spec CARRIES a `qualification` key (MOSFETs)
    // is one whose records can state the grade, so a record that states none is UNKNOWN —
    // not commercial. Reading silence as "commercial" FAILED Vishay's SQ4153EY (an SQ-line
    // part whose TAS record has no part.qualification, like 464 of the 465 Vishay SQ*
    // MOSFETs) against the AEC-Q101 DMP1011LFV as "substitute is commercial", a claim no
    // record makes. Kelvin does not decode the grade from a vendor prefix here: "SQ means
    // automotive" is a guess, and the gap belongs to the catalogue. Likewise a label that
    // says "Automotive" without naming AEC-Q states a market, not a qualification, so it is
    // unknown too. "-" is a placeholder, i.e. silence. Families without the field keep the
    // MPN decode as their only evidence (Murata GRM is a stated commercial series there).
    enum class Grade { Aec, NotAec, Unknown };
    auto grade_of = [](const json& p) {
        const std::string q = lower_copy(str(p, "qualification"));
        if (!q.empty() && q != "-") {
            if (q.find("aec") != std::string::npos) return Grade::Aec;
            if (q.find("automotive") != std::string::npos &&
                q.find("non-automotive") == std::string::npos)
                return Grade::Unknown;
            return Grade::NotAec;
        }
        if (is_automotive_grade(str(p, "mpn"))) return Grade::Aec;
        return p.contains("qualification") ? Grade::Unknown : Grade::NotAec;
    };
    if (grade_of(original) == Grade::Aec) {
        const Grade g = grade_of(cand);
        if (g == Grade::NotAec) {
            params.push_back({{"name", "automotive_grade"}, {"verdict", FAIL}});
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            notes.push_back("original is an automotive (AEC-Q) grade part; substitute is commercial");
        } else if (g == Grade::Unknown) {
            // Not a drop-in on the record's word either: an AEC-Q original cannot be
            // replaced by a part whose qualification nobody has established, so the grade
            // is capped the way an unverified footprint caps it, at no extra penalty.
            params.push_back({{"name", "automotive_grade"}, {"verdict", UNVERIFIED}});
            demote();
            const std::string q = str(cand, "qualification");
            notes.push_back(
                "original is an automotive (AEC-Q) grade part; the substitute's record " +
                (q.empty() || q == "-" ? std::string("states no qualification")
                                       : "says \"" + q + "\" but names no AEC-Q qualification") +
                ", so its grade is UNVERIFIED — confirm AEC-Q qualification from the datasheet");
        }
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

    // Say which axis went unweighed, or "unknown" reads as a shrug rather than a specific
    // gap the reader can close from a datasheet.
    if (direction_blind) {
        std::string axes;
        for (size_t i = 0; i < blind_axes.size(); ++i)
            axes += (i ? ", " : "") + blind_axes[i];
        notes.push_back("the original's record states no " + axes +
                        ", so the substitute's could not be weighed against it — better or "
                        "worse on that axis is unknown, whatever the other parameters say");
    }

    // ── retracted sources (ABT #2035) ────────────────────────────────────────
    // Every source the substitute's record cites has been retracted (VLIN1626-02G: a
    // category browse page naming no order code). What the row says may be right, but
    // nothing verifies it, so no verdict drawn from it may read 'pass' or 'fits': each
    // becomes UNVERIFIED, the grade is capped as missing required data and the direction
    // is unknown. Regressions stay as they are — data that already disqualifies the part
    // does not become better for being unsourced.
    const bool cand_retracted = flag(cand, "provenance_retracted");
    if (cand_retracted) {
        for (auto& p : params)
            if (p.value("verdict", "") == PASS) p["verdict"] = UNVERIFIED;
        if (out.value("footprint", "") == "fits") out["footprint"] = "unknown";
        if (out.value("height_fit", "") == "fits") out["height_fit"] = "unknown";
        demote();
        penalty += opt.gate_weight * kVerdictFailPenalty;
        missing_required_data = true;
        notes.push_back("every source cited by the substitute's record is retracted — none of "
                        "its data is verified; confirm the part from its datasheet before "
                        "relying on any verdict here");
    }

    // Honesty: an unverified original can never be a clean 'recommended'. An original
    // whose every source is retracted is unverified the same way (ABT #2035).
    if (!opt.original_verified || flag(original, "provenance_retracted")) {
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
    // An unverified clearance caps the grade exactly as an unverified footprint does.
    if (out.value("height_fit", "") == "unknown") footprint_unverified = true;
    // So does an unknown board attach against a connector original that states one.
    if (connector_mount_unverified) footprint_unverified = true;
    // drop_in asserts no rating regressed; a power nobody can state at the original's rating
    // temperature has not been shown not to, so it takes the same minor_review cap.
    out["grade"] = grade_for(status, tier, any_warn, any_fail,
                             footprint_unverified || power_basis_unverified ||
                                 mount_unverified,
                             missing_required_data, hfit);

    // Direction: on the directional parameters we could actually compare, did
    // the substitute come out ahead or behind? Mirrors the industry's upgrade /
    // downgrade suffix (SiliconExpert A/U vs A/D), but computed from the
    // measured ratios rather than asserted. "equivalent" when neither side
    // clearly leads — including when there was nothing comparable to judge on,
    // which is honest rather than flattering.
    const std::string dir = (worse > 0 && better == 0)   ? "downgrade"
                            : (better > 0 && worse == 0) ? "upgrade"
                            : (better > 0 && worse > 0)  ? "mixed"
                                                         : "equivalent";
    // With a hard axis unweighed (see direction_blind), the two REASSURING readings are the
    // ones that could be wrong: "upgrade" and "equivalent" both assert that nothing is behind,
    // on evidence that never covered the parameter the match turns on. A measured regression
    // stands — blindness elsewhere cannot make a regression we did see disappear — and so does
    // "mixed", which already carries one.
    out["direction"] = (cand_retracted || ((direction_blind || isat_basis_blind || surge_blind ||
                                            current_basis_blind) &&
                                           (dir == "upgrade" || dir == "equivalent")))
                           ? "unknown"
                           : dir;
    return out;
}

// Rank a candidate list for an original. Returns {category, original_verified,
// candidates:[...]} sorted best-first (lowest penalty); no_substitute rows sink.
//: Categories the ranker has rules for. Any other would be compared on nothing
//: and every candidate would come back "recommended" — a 300 V varistor for a
//: 30 V one did — so an unknown category is refused, not ranked.
inline bool known_category(const std::string& c) {
    static const std::set<std::string> k{"resistor", "capacitor", "magnetic", "chipBead",
                                         "mosfet", "diode", "igbt", "connector", "analog",
                                         "timeBase", "varistor"};
    return k.count(c) > 0;
}

inline json cross_reference(const std::string& category, const json& original,
                            const json& candidates, const Options& opt = {}) {
    if (!known_category(category))
        throw std::invalid_argument("cross_reference: no ranking rules for category '" +
                                    category + "'");
    std::vector<json> scored;
    scored.reserve(candidates.size());
    for (const auto& c : candidates) scored.push_back(score_candidate(category, original, c, opt));
    std::stable_sort(scored.begin(), scored.end(), [](const json& a, const json& b) {
        return a.value("penalty", 1e18) < b.value("penalty", 1e18);
    });
    json cands = json::array();
    for (size_t i = 0; i < scored.size() && i < opt.max_results; ++i) cands.push_back(scored[i]);
    return {{"category", category},
            {"original_verified",
             opt.original_verified && !flag(original, "provenance_retracted")},
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
    if (options.contains("requirements")) {
        const auto& req = options["requirements"];
        if (!req.is_object())
            throw std::invalid_argument("cross_reference: 'requirements' must be an object "
                                        "{parameter: minimum}");
        for (const auto& [key, v] : req.items()) {
            if (!v.is_number())
                throw std::invalid_argument("cross_reference: requirement '" + key +
                                            "' must be a number");
            opt.requirements[key] = v.get<double>();
        }
    }
    return cross_reference(category, original, candidates, opt);
}

}  // namespace kelvin::crossref
