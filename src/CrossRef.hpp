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
    // A connector is half of a mated pair: the other gender or another interface
    // standard does not plug into what is on the board. Only a MISMATCH rejects — a
    // record that omits one is unverified, not refused (none of these excludes a
    // missing substitute). Pitch is deliberately NOT here: a different pitch is
    // reported as a land-pattern change (partial, redesign), ranked below every part
    // that keeps the pads. The board attach (smt / tht / pressFit ...) and the
    // mating-axis orientation are identities too, but each is decided by its own
    // gate (the shared mount gate, the connector orientation block), not here.
    static const std::set<std::string> connector{"family", "positions", "polarity",
                                                 "interface_standard"};
    static const std::set<std::string> analog{"subtype", "channels"};
    // mode: a 3rd-overtone crystal cannot cross with a fundamental one in either
    // direction — an overtone circuit's LC tank is inductive at the fundamental,
    // so the loop cannot close there; drop an overtone part into a fundamental
    // circuit and it either fails to start or runs near a third of the marking.
    // output_type: LVDS / HCSL / LVPECL / CMOS are different termination
    // networks, not a parameter — swapping one for another is a board respin.
    // Both are self-selecting: mode is absent on oscillators and output_type on
    // bare crystals, and an absent parameter is simply not compared.
    // enable_function: pin 1's function (output enable / standby / none) is wired by the
    // board, so a different one is a different pinout (CrossRefParams.hpp kTimeBase).
    static const std::set<std::string> timebase{"subtype", "technology", "frequency", "mode",
                                                "output_type", "enable_function"};
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
//
// Above the knee (Qarlos 2026-11-08, seed 20276947): RN55E90R9BRE6 is 0.125 W at 70 degC and
// still 0.1 W at 125 degC (curve 70/125/175 -> 100/80/0 %); MFP-25BRD52-91R is 0.25 W at 70
// degC but derates to zero at 155 degC, so at 125 degC it carries 0.088 W. Compared at T_ref
// alone it read "pass / upgrade". The rule is: the substitute must deliver at least the
// original's power at EVERY temperature where both records state a curve — the overlap of the
// two curves' spans. Both curves are straight-line segments, so the worst ratio s(T)/o(T) is
// found at a breakpoint of one curve or the other; every breakpoint in the overlap is checked
// (points where the original delivers nothing are skipped: nothing is required there). The
// verdict, penalty and direction are taken at the WORST such temperature. Outside the overlap
// nothing is compared from the curves (the operating-range rule answers that).
struct PowerAtTemperature {
    bool applies = false;          // the original states a temperature basis
    double t_ref = 0.0;            // degC, where the comparison is made (see `at_knee`)
    std::optional<double> orig;    // W the original delivers at t_ref
    std::optional<double> subst;   // W the substitute delivers at t_ref; nullopt = not on record
    std::string how;               // how `subst` was established, or why it could not be
    // True when t_ref is the original's rating temperature; false when the curve scan found a
    // worse temperature above it and t_ref was moved there.
    bool at_rating_temperature = true;
    double rating_temperature = 0.0;  // degC, the original's rating temperature
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
// Amplitude of a stated curve at temperature x, linearly interpolated. The caller keeps x
// inside [t.front(), t.back()].
inline double amplitude_at(const std::vector<double>& t, const std::vector<double>& a, double x) {
    for (size_t i = 0; i + 1 < t.size(); ++i) {
        if (x >= t[i] && x <= t[i + 1]) {
            const double f = (x - t[i]) / (t[i + 1] - t[i]);
            return a[i] + f * (a[i + 1] - a[i]);
        }
    }
    throw std::logic_error("power_detail::amplitude_at: temperature outside the curve");
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
    r.rating_temperature = *t_ref;
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
        r.subst = *s_p * amplitude_at(st, sa, r.t_ref);
        r.how = "from the substitute's derating curve";
        // Above the knee: both curves stated -> every breakpoint of either curve inside
        // their common span (see the struct comment). The worst ratio wins, and only a
        // strictly worse one moves the comparison off the rating temperature.
        if (!ot.empty() && *r.orig > 0.0) {
            const double lo = std::max(ot.front(), st.front());
            const double hi = std::min(ot.back(), st.back());
            if (lo <= hi) {
                std::vector<double> probe{lo, hi};
                for (double x : ot)
                    if (x >= lo && x <= hi) probe.push_back(x);
                for (double x : st)
                    if (x >= lo && x <= hi) probe.push_back(x);
                double worst_ratio = *r.subst / *r.orig;
                for (double x : probe) {
                    const double o_w = *o_p * amplitude_at(ot, oa, x);
                    if (!(o_w > 0.0)) continue;  // the original carries nothing here
                    const double s_w = *s_p * amplitude_at(st, sa, x);
                    if (s_w / o_w < worst_ratio - 1e-12) {
                        worst_ratio = s_w / o_w;
                        r.t_ref = x;
                        r.orig = o_w;
                        r.subst = s_w;
                        r.at_rating_temperature = false;
                    }
                }
                if (!r.at_rating_temperature) r.how = "from both records' derating curves";
            }
        }
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

// ── connector pitch: does the pin field line up? ─────────────────────────────
// A connector record carries no body outline, so its contact pitch IS its land pattern:
// contact k of a row sits (k-1) x pitch from contact 1. Two pitches that differ by a few
// hundredths of a millimetre (5.00 / 5.08, 2.50 / 2.54) are a fit on a two-hole pattern
// and a misfit on a long row, because the difference ADDS UP along the row: a 24-way
// 5.00 mm strip in a 5.08 mm board puts its end pin 23 x 0.08 = 1.84 mm off its hole. A
// relative tolerance on the pitch alone (the old 2 % ExactMatch band) passed that pair.
//
// Rule (owner decision to confirm: the 0.1 mm limit is the one the owner approved for a
// two-hole 2.50/2.54 and 5.00/5.08 pattern, applied here to the FARTHEST pin):
//   offset = (positions_per_row - 1) x |pitch_original - pitch_substitute|
//   offset <= 0.1 mm  -> pass (the pin field lines up);
//   offset >  0.1 mm  -> fail: a different land pattern, reported exactly like any other
//                        pitch mismatch (footprint different_land_pattern, partial), with
//                        a note giving both pitches, the per-row count and the end-pin offset.
// positions_per_row = ceil(positions / rows), taken from each record that states both
// (mechanical.positions and mechanical.rows); when both records state it, the LONGER row
// decides (that row's end pin is the one that has to reach its hole). When neither record
// states rows, the row length is not known and is only BOUNDED, never assumed:
//   * from below by 2 — a stated contact-to-contact pitch means at least two contacts sit
//     that far apart, so a difference over 0.1 mm already misplaces the first neighbour
//     -> fail (certain whatever the row length);
//   * from above by the stated position count (a row cannot hold more contacts than the
//     part has), so an offset within 0.1 mm even over the whole count -> pass;
//   * in between, the arrangement decides and nobody stated it -> unverified with a note.
// With no position count on either side, a non-zero difference within 0.1 mm is
// unverified: two positions is NOT assumed as the row length.
// Only decides when BOTH pitches are stated; otherwise the PARAM_SPECS table's own
// missing-value handling applies.
// Row count: when BOTH records state mechanical.rows and they differ (1 row against 2), the
// pins are arranged differently and cannot meet the same holes whatever the pitch — a
// different pin field, reported like a pitch mismatch (pitch FAIL when both pitches are
// stated, footprint different_land_pattern in every case), with a note naming both row
// counts. A row count stated on one side only, or on neither, gives no verdict on its own.
struct ConnectorPitchFit {
    const char* verdict = nullptr;  // PASS / FAIL / UNVERIFIED; nullptr = not decided here
    double per_row = 0.0;           // contacts per row the verdict was taken over (0 = unknown)
    double offset_mm = 0.0;         // end-pin offset over that row
    bool rows_differ = false;       // both records state rows, and they differ
    double o_rows = 0.0, s_rows = 0.0;
    std::string note;
};
constexpr double kConnectorPinFieldLimitMm = 0.1;
inline ConnectorPitchFit connector_pitch_fit(const json& original, const json& cand) {
    ConnectorPitchFit f;
    const auto o_r = num(original, "rows"), s_r = num(cand, "rows");
    if (o_r && s_r && *o_r != *s_r) {
        f.rows_differ = true;
        f.o_rows = *o_r;
        f.s_rows = *s_r;
    }
    const auto o_p = num(original, "pitch_mm"), s_p = num(cand, "pitch_mm");
    if (!o_p || !s_p) return f;
    if (f.rows_differ) {
        f.verdict = FAIL;
        return f;
    }
    const double d = std::fabs(*o_p - *s_p);
    // Floating-point equality only: any real catalogue difference is >= 0.01 mm.
    if (d <= 1e-9 * std::max(*o_p, *s_p)) {
        f.verdict = PASS;
        return f;
    }
    double per_row = 0.0;
    for (const json* rec : {&original, &cand}) {
        const auto n = num(*rec, "positions"), rows = num(*rec, "rows");
        if (n && rows && *rows >= 1.0) per_row = std::max(per_row, std::ceil(*n / *rows));
    }
    const std::string pitches =
        "pitch " + mm(*o_p * 1e-3) + " -> " + mm(*s_p * 1e-3) + " mm";
    if (per_row > 0.0) {
        f.per_row = per_row;
        f.offset_mm = (per_row - 1.0) * d;
        f.verdict = f.offset_mm <= kConnectorPinFieldLimitMm ? PASS : FAIL;
        return f;
    }
    if (d > kConnectorPinFieldLimitMm) {
        // The adjacent contact already misses; the row length does not change the verdict.
        f.verdict = FAIL;
        return f;
    }
    double bound = 0.0;
    for (const json* rec : {&original, &cand})
        if (const auto n = num(*rec, "positions")) bound = std::max(bound, *n);
    if (bound >= 1.0 && (bound - 1.0) * d <= kConnectorPinFieldLimitMm) {
        f.verdict = PASS;
        return f;
    }
    f.verdict = UNVERIFIED;
    if (bound >= 1.0)
        f.note = pitches + ": neither record states how the " +
                 std::to_string(static_cast<long>(bound)) +
                 " positions are split into rows, and in a single row the end pin would sit " +
                 mm((bound - 1.0) * d * 1e-3) +
                 " mm off its hole (over 0.1 mm) — confirm the pin field lines up";
    else
        f.note = pitches + ": neither record states a position count, so the end-pin offset "
                 "cannot be worked out — confirm the pin field lines up";
    return f;
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
    //
    // v20 (Qarlos 2026-11-09, seed 20784754): CAS now has the field — part.safetyClass.class —
    // and the spec carries it as `safety_class`. Vishay's F339MX252231KIM2T0 states x2 there,
    // 310 VAC, and no series name, so this gate never engaged and non-safety DC film parts came
    // back 'recommended'. Each side's class is read from the stated field when the record has
    // one, else from its series name as before. Rule (owner decision to confirm):
    //   * the original states no class (or states "none")  -> nothing is compared;
    //   * same or stricter class on the original's axis     -> pass (X1 covers X2, Y1 covers
    //     Y2; X never covers Y and Y is not read as covering X);
    //   * a lower class on that axis                        -> FAIL, rejected (as before);
    //   * the substitute's record STATES its class and it holds nothing on that axis (a stated
    //     Y2 or "none" against an X2 original)              -> FAIL, rejected like an identity
    //     mismatch: the record says what the part is certified as;
    //   * the substitute states no class anywhere           -> UNVERIFIED with the FAIL's
    //     consequence (demoted, FAIL penalty, capped at major_review), as before. Rejecting
    //     this case outright is an open decision for the owner: it would overturn the ABT #557
    //     rule that an unstated class is an absence of evidence, not a mismatch.
    // The AC rating is part of the approval (IEC 60384-14 grants a class AT a rated AC
    // voltage), so for a class-stating original it is compared too: a substitute AC rating
    // below the original's is a FAIL and rejects; an unstated one is UNVERIFIED with the same
    // consequence as an unverified class. Equal or higher passes. The DC "voltage" row above
    // is not that rating and is never read as it.
    if (cat == "capacitor") {
        const std::string o_series = str(original, "family");
        const std::string o_token = str(original, "safety_class");
        const SafetyClass o_class =
            o_token.empty() ? safety_class(o_series) : safety_class_from_token(o_token);
        if (o_class.any()) {
            const std::string s_series = str(cand, "family");
            const std::string s_token = str(cand, "safety_class");
            const bool s_stated = !s_token.empty();
            const SafetyClass s_class =
                s_stated ? safety_class_from_token(s_token) : safety_class(s_series);
            struct AxisPair {
                char axis;
                std::optional<int> o, s;
            };
            const AxisPair axes[] = {{'X', o_class.x, s_class.x}, {'Y', o_class.y, s_class.y}};
            bool downgraded = false, unverified = false;
            for (const AxisPair& a : axes) {
                if (!a.o) continue;  // the original claims nothing on this axis
                if (!a.s && s_stated) {
                    downgraded = true;
                    notes.push_back(safety_class_not_held(a.axis, *a.o, s_token));
                } else if (!a.s) {
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
            const auto o_ac = num(original, "voltage_ac"), s_ac = num(cand, "voltage_ac");
            bool ac_unverified = false;
            if (o_ac) {
                char buf[64];
                if (s_ac && *s_ac < *o_ac) {
                    params.push_back({{"name", "voltage_ac"}, {"verdict", FAIL}});
                    std::snprintf(buf, sizeof buf, "%g V AC vs the original's %g V AC", *s_ac,
                                  *o_ac);
                    notes.push_back(std::string("AC rating below the original's: ") + buf +
                                    " — the safety class is granted at its rated AC voltage, "
                                    "so the substitute is not approved for the original's mains");
                    return reject("AC voltage rating below a safety-class original's");
                }
                if (s_ac) {
                    params.push_back({{"name", "voltage_ac"}, {"verdict", PASS}});
                } else {
                    params.push_back({{"name", "voltage_ac"}, {"verdict", UNVERIFIED}});
                    ac_unverified = true;
                    std::snprintf(buf, sizeof buf, "%g V AC", *o_ac);
                    notes.push_back(std::string("the original is approved at ") + buf +
                                    "; the substitute's record states no AC rating, so its mains "
                                    "rating could not be compared (its DC rating is not that "
                                    "rating) — confirm it on the datasheet");
                }
            }
            if (unverified || ac_unverified) {
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
    // ── resistive element and stated function (Qarlos 2026-11-08) ────────────
    // R, P and tolerance are the same columns on a thin film, a thick film, a metal film and
    // a wirewound, and on a current-sense shunt or an anti-surge chip and the general-purpose
    // chip beside it — so silence about the class read as agreement: ERJ6DQJ1R5V (current
    // sensing) took SR0805JR-471R5L as drop_in, SG731JTTD4R7M (anti pulse) took AA0603 chips
    // as drop_in. Only what the RECORDS state is used (RAS part.technology; the function the
    // extractor read from technology/family/series/description), never the MPN.
    //   * Technology class differs (both stated): WARN + note — a different TCR, noise,
    //     stability and pulse behaviour, never a drop-in; not counted as a direction (thick ->
    //     thin is better on most axes and worse on pulse load, so neither word is honest).
    //     Two exceptions, both because the enum value does not name an element: melf is a
    //     package, so it is not compared; and currentSenseShunt against a record whose
    //     function is ALSO current sensing (ERJ6DQ "Current Sensing Resistors Thick Film
    //     Type" against a thickFilm record whose series says current sensing) is the same
    //     class written two ways, so the function check below decides it alone.
    //   * The original states a function the substitute's record does not: FAIL + note,
    //     like the anti-sulfuration class above (major_review). A sense resistor's value is a
    //     measurement and a surge resistor's rating is its reason to exist; a substitute that
    //     does not claim either has not been shown to do the job, and a general-purpose chip
    //     under the original's surge fails open.
    if (cat == "resistor") {
        const std::string o_tech = str(original, "technology"), s_tech = str(cand, "technology");
        const bool both_sense = flag(original, "current_sense") && flag(cand, "current_sense");
        const bool comparable = !o_tech.empty() && !s_tech.empty() && o_tech != "melf" &&
                                s_tech != "melf" &&
                                !(both_sense && (o_tech == "currentSenseShunt" ||
                                                 s_tech == "currentSenseShunt"));
        if (comparable && o_tech != s_tech) {
            params.push_back({{"name", "technology"}, {"verdict", WARN}});
            demote();
            penalty += opt.gate_weight * kVerdictWarnPenalty;
            notes.push_back("resistor technology differs: " + o_tech + " -> " + s_tech +
                            " — a different construction class, with different TCR, noise, "
                            "long-term stability and pulse behaviour; not a drop-in where the "
                            "original's construction was chosen for them");
        }
        struct Function {
            const char* key;
            const char* what;
        };
        for (const Function& f : {Function{"current_sense", "a current-sense resistor"},
                                  Function{"pulse_withstanding", "a pulse/surge-withstanding "
                                                                 "(anti-surge) resistor"}}) {
            if (!flag(original, f.key)) continue;
            const bool kept = flag(cand, f.key);
            params.push_back({{"name", f.key}, {"verdict", kept ? PASS : FAIL}});
            if (kept) continue;
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            ++worse;  // a lost function is a regression, whatever the numbers say
            notes.push_back(std::string("the original is ") + f.what +
                            " by its record; the substitute's record states no such class (" +
                            (s_tech.empty() ? std::string("technology unstated") : s_tech) +
                            ") — confirm it from the datasheet before using it in that role");
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
    // Whether the pin field lines up (connector_pitch_fit above): it decides the pitch_mm
    // verdict whenever both records state a pitch. An alignment nobody can establish is
    // an unverified land pattern and caps the grade like one.
    const ConnectorPitchFit connector_pitch =
        cat == "connector" ? connector_pitch_fit(original, cand) : ConnectorPitchFit{};
    bool connector_pitch_unverified = false;
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
    // A timing part's control-pin function, or its frequency tolerance / stability, stated
    // by the original and not by the substitute (or the pin function by the substitute
    // only): UNVERIFIED, noted, and the grade capped below drop_in — drop_in asserts the
    // same pinout and no accuracy regression, which nobody has shown (Qarlos 2026-11-09).
    bool timebase_unverified = false;
    // A connector orientation that could not be established (see the orientation block
    // after the PARAM_SPECS loop): not a drop-in.
    bool connector_orientation_unverified = false;
    // Pluggable vs fixed terminal block stated on one side only (see the pluggable block).
    bool connector_pluggable_unverified = false;
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
    // ── capacitor ripple-current basis ──────────────────────────────────────
    // A ripple rating is stated AT a frequency, and the same can is rated very differently at
    // 100 Hz and at 20 kHz: 380LX151M450K022 is 1.2 A at 120 Hz and 1.7 A at 20 kHz. Its
    // 1.7 A @ 20 kHz was failed against a candidate's 1.36 A @ 100 Hz with no caveat (Qarlos
    // 2026-11-09, seed 20365838). Rule — compare like with like, the treatment the resistor
    // power basis gets (power_at_rating_temperature):
    //   * both records state the frequency and it is the same -> compared as stated;
    //   * both state it and it DIFFERS -> UNVERIFIED, with a note giving both values and both
    //     frequencies — with ONE exception, the owner's rule (2026-11-09): 100 Hz and 120 Hz
    //     are the same basis, the low-frequency ripple rating quoted at twice the 50 Hz or the
    //     60 Hz mains frequency (the region a datasheet names, not a property of the part), so
    //     a 100 Hz and a 120 Hz rating are compared directly. Rubycon VXH 1.63 A @ 120 Hz
    //     against Vishay 1.75 A @ 100 Hz was left unverified (Qarlos round 4). Every other
    //     difference, however small, stays UNVERIFIED;
    //   * the original states it, the substitute does not -> UNVERIFIED, noted;
    //   * the original states none -> compared as stated, exactly as before (no basis exists
    //     for any candidate, so none is skewed against another).
    // An UNVERIFIED basis is demoted and capped at minor_review (drop_in asserts no rating
    // regressed, which nobody has shown) and counts in neither direction. No conversion is
    // attempted: never a "typical" frequency multiplier, and not the record's own multiplier
    // table either (a candidate's table rarely reaches the original's frequency — 198 PHR-SI's
    // stops at 10 kHz — and an extrapolated multiplier is a guess).
    bool ripple_basis_unverified = false;
    if (cat == "capacitor") {
        const auto o_ir = num(original, "ripple_current"), s_ir = num(cand, "ripple_current");
        const auto o_f = num(original, "ripple_current_frequency"),
                   s_f = num(cand, "ripple_current_frequency");
        if (o_ir && s_ir && o_f) {
            char buf[256];
            auto same_f = [](double a, double b) {
                return std::fabs(a - b) <= 1e-9 * std::max(a, b);
            };
            auto mains_ripple_basis = [&](double f) { return same_f(f, 100.0) || same_f(f, 120.0); };
            const bool same_basis =
                s_f && (same_f(*o_f, *s_f) || (mains_ripple_basis(*o_f) && mains_ripple_basis(*s_f)));
            if (s_f && !same_basis) {
                ripple_basis_unverified = true;
                std::snprintf(buf, sizeof buf,
                              "ripple current quoted at different frequencies (original %g A at "
                              "%g Hz, substitute %g A at %g Hz)",
                              *o_ir, *o_f, *s_ir, *s_f);
                notes.push_back(std::string(buf) +
                                " — not directly comparable; confirm the substitute's rating at "
                                "the original's frequency from its datasheet");
            } else if (!s_f) {
                ripple_basis_unverified = true;
                std::snprintf(buf, sizeof buf,
                              "the original's ripple current is %g A at %g Hz; the substitute's "
                              "record states %g A at no frequency",
                              *o_ir, *o_f, *s_ir);
                notes.push_back(std::string(buf) +
                                " — not directly comparable; confirm its rating at the "
                                "original's frequency from the datasheet");
            }
            if (ripple_basis_unverified) demote();
        }
    }
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
        // With no basis for the substitute's power at T_ref, a nominal rating below the
        // original's is a FAIL, not merely unverified: whatever its derating, the substitute
        // delivers at most its nominal there, and that is already less than the original's.
        const bool power_nominal_short =
            power_at_t && !power_t.subst && *detail::jnum(cand, spec.key) < *power_t.orig;
        const bool pin_field = spec.key == "pitch_mm" && connector_pitch.verdict != nullptr;
        const bool ripple_blind =
            cat == "capacitor" && spec.key == "ripple_current" && ripple_basis_unverified;
        const ParamOutcome outcome =
            pin_field             ? ParamOutcome(connector_pitch.verdict)
            : ripple_blind        ? ParamOutcome(UNVERIFIED)
            : !power_at_t         ? compare_param(spec, original, cand)
            : power_t.subst       ? compare_numeric(spec, power_t.orig, power_t.subst)
            : power_nominal_short ? ParamOutcome(FAIL)
                                  : ParamOutcome(UNVERIFIED);
        const std::string verdict = outcome.verdict;
        params.push_back({{"name", spec.key}, {"verdict", verdict}});
        if (pin_field && verdict == UNVERIFIED) {
            connector_pitch_unverified = true;
            notes.push_back(connector_pitch.note);
        }
        if (cat == "connector" && verdict == FAIL) {
            if (spec.key == "pitch_mm") connector_pitch_conflict = true;
            if (spec.key == "contact_plating") connector_plating_conflict = true;
            if (spec.key == "termination") connector_termination_conflict = true;
        }
        if (cat == "timeBase" && supply_key) {
            if (verdict == FAIL) timebase_supply_fail = true;
            if (verdict == UNVERIFIED && detail::present(original, spec.key))
                timebase_supply_unverified = true;
        }
        if (cat == "timeBase" && spec.key == "frequency" && verdict == FAIL) {
            // Say how far off it is: the reject reason alone ("an identity parameter
            // differs") does not tell the engineer that 25.00125 MHz was offered for 25 MHz.
            const auto o_f = detail::jnum(original, "frequency"), s_f = detail::jnum(cand, "frequency");
            if (o_f && s_f && *o_f > 0) {
                char buf[256];
                std::snprintf(buf, sizeof buf,
                              "nominal frequency differs: the original is %.10g Hz, the "
                              "substitute %.10g Hz (%+.1f ppm)",
                              *o_f, *s_f, (*s_f / *o_f - 1.0) * 1e6);
                notes.push_back(std::string(buf) +
                                " — a timing part's frequency is its identity; a different "
                                "nominal is not a substitute, whatever its stability");
            }
        }
        if (cat == "timeBase" && spec.key == "enable_function") {
            const std::string o_en = str(original, "enable_function"),
                              s_en = str(cand, "enable_function");
            if (verdict == FAIL) {
                notes.push_back("control-pin function differs: the original's is '" + o_en +
                                "', the substitute's '" + s_en +
                                "' — pin 1 is wired for the original's function, so this is a "
                                "different pinout, not a drop-in");
            } else if (verdict == UNVERIFIED && (!o_en.empty() || !s_en.empty())) {
                timebase_unverified = true;
                notes.push_back(
                    !o_en.empty()
                        ? "the original's control pin is '" + o_en +
                              "'; the substitute's record states no control-pin function — "
                              "confirm its pin 1 from the datasheet"
                        : "the substitute's control pin is '" + s_en +
                              "'; the original's record states no control-pin function — "
                              "confirm both pin 1 functions match from the datasheets");
            }
        }
        if (cat == "timeBase" && (spec.key == "tolerance_ppm" || spec.key == "stability_ppm")) {
            const char* what = spec.key == "tolerance_ppm" ? "frequency tolerance"
                                                           : "frequency stability";
            const auto o_p = detail::jnum(original, spec.key), s_p = detail::jnum(cand, spec.key);
            char buf[256];
            if (verdict == FAIL && o_p && s_p) {
                std::snprintf(buf, sizeof buf,
                              "looser %s: the substitute is +/-%g ppm against the original's "
                              "+/-%g ppm",
                              what, *s_p, *o_p);
                notes.push_back(std::string(buf) +
                                " — the design's timing budget was built on the original's");
            } else if (verdict == UNVERIFIED && o_p && !s_p) {
                timebase_unverified = true;
                std::snprintf(buf, sizeof buf,
                              "the original's %s is +/-%g ppm; the substitute's record states "
                              "none",
                              what, *o_p);
                notes.push_back(std::string(buf) + " — confirm it from the datasheet");
            }
        }
        if (cat == "diode" && spec.key == "vz_tolerance_pct") {
            if (verdict == WARN || verdict == FAIL) diode_vz_band_regression = true;
            // Only when the ORIGINAL states a window: that is the case where a real
            // guarantee is on the table and the substitute's record answers nothing.
            if (verdict == UNVERIFIED && detail::present(original, spec.key))
                diode_vz_band_unverified = true;
        }

        const bool numeric = (spec.dir == Dir::Lower || spec.dir == Dir::Higher);
        // Two ratings on different bases have no direction between them (ripple basis above).
        auto o = numeric && !ripple_blind ? detail::jnum(original, spec.key) : std::nullopt;
        auto s = numeric && !ripple_blind ? detail::jnum(cand, spec.key) : std::nullopt;
        if (power_at_t) {
            using power_detail::celsius;
            using power_detail::watts;
            const double s_nominal = *s;
            // Penalty and direction are about the power at T_ref too, not the nominal.
            o = power_t.orig;
            s = power_t.subst;
            if (power_nominal_short) {
                // The nominal is the most the substitute can deliver at T_ref, so the
                // downgrade is counted on it (the FAIL above does the demotion).
                s = s_nominal;
                notes.push_back("power rating below the original's: the substitute is rated " +
                                watts(s_nominal) + " against the original's " +
                                watts(*power_t.orig) + " at " + celsius(power_t.t_ref) +
                                ", and " + power_t.how);
            } else if (!power_t.subst) {
                // Not on record is not a pass: the original's rating is stated at a
                // temperature, and nothing says what the substitute delivers there. Capped
                // the way an unverified footprint is (power_basis_unverified below).
                demote();
                power_basis_unverified = true;
                notes.push_back("power rating not comparable at the original's rating "
                                "temperature: the original is rated " + watts(*power_t.orig) +
                                " at " + celsius(power_t.t_ref) + ", and " + power_t.how +
                                " — confirm the substitute's derating from its datasheet");
            } else if (!power_t.at_rating_temperature) {
                // The derating curves cross above the knee: name the temperature, both
                // powers there, and the rating temperature they were level or ahead at.
                notes.push_back("power compared along both derating curves: at " +
                                celsius(power_t.t_ref) + " the substitute delivers " +
                                watts(*power_t.subst) + " against the original's " +
                                watts(*power_t.orig) + " (rated " + watts(*num(original, "power_rating")) +
                                " at " + celsius(power_t.rating_temperature) +
                                ") — it derates more steeply above the original's knee");
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

    // ── connector mating-axis orientation (CONAS mechanical.orientation) ─────
    // vertical / rightAngle / horizontal / mezzanine / edge / cable is the direction the
    // counterpart (cable, FPC, daughter board) arrives from: a vertical FPC connector
    // for a right-angle one is a different mechanical design — the flex leaves the board
    // the other way — not a substitution (F32Q-1A7H1-11024 vs 2-2492142-4, Qarlos
    // 2026-11-08 seed 20269028). Rule:
    //   both stated and equal                -> pass;
    //   both stated, different               -> fail, rejected like any connector identity;
    //   horizontal against rightAngle        -> unverified with a note (DECISION for the
    //     owner: both describe a mating axis parallel to the board and vendors use the two
    //     words for the same body, but the schema keeps them apart, so neither "same" nor
    //     "different" can be asserted from the record);
    //   stated on one side only              -> unverified with a note, capped like an
    //     unverified footprint (never a drop-in on an orientation nobody checked);
    //   stated on neither side               -> not compared (silent, as every axis is).
    if (cat == "connector") {
        const std::string oo = str(original, "orientation"), so = str(cand, "orientation");
        if (!oo.empty() || !so.empty()) {
            const auto said = [](const std::string& v) {
                return v.empty() ? std::string("not stated") : v;
            };
            const bool parallel_pair = (oo == "horizontal" && so == "rightAngle") ||
                                       (oo == "rightAngle" && so == "horizontal");
            if (!oo.empty() && !so.empty() && oo == so) {
                params.push_back({{"name", "orientation"}, {"verdict", PASS}});
            } else if (!oo.empty() && !so.empty() && !parallel_pair) {
                params.push_back({{"name", "orientation"}, {"verdict", FAIL}});
                notes.push_back("orientation differs: " + oo + " -> " + so +
                                " — the counterpart mates along a different axis; a different "
                                "mechanical design, not a drop-in");
                return reject("connector orientation differs (a different mating axis)");
            } else {
                params.push_back({{"name", "orientation"}, {"verdict", UNVERIFIED}});
                connector_orientation_unverified = true;
                if (parallel_pair)
                    notes.push_back("orientation: the original is " + oo +
                                    " and the substitute " + so +
                                    " — vendors use both words for a mating axis parallel to the "
                                    "board; confirm from the drawings that the counterpart "
                                    "arrives the same way");
                else
                    notes.push_back("orientation not established (original: " + said(oo) +
                                    ", substitute: " + said(so) +
                                    ") — confirm vertical / right-angle from the datasheets");
            }
        }
        // ── pluggable vs fixed terminal block (CONAS familyTerminalBlock.pluggable) ──
        // A pluggable terminal-block plug is the loose half that mates onto a board header;
        // a fixed PCB terminal block is soldered to the board and takes the wire itself. One
        // is not a drop-in for the other: Molex's 395337424 Eurostyle plug took WAGO 739-124
        // fixed strips (Qarlos 2026-11-08). Tri-state, never defaulted:
        //   both stated, different -> reject (a plug is not a fixed block);
        //   both stated, same      -> pass;
        //   stated on one side     -> unverified with a note, capped like an unverified footprint;
        //   stated on neither      -> not compared (silent).
        const auto pluggable_of = [](const json& rec) -> std::optional<bool> {
            auto it = rec.find("pluggable");
            if (it == rec.end() || it->is_null()) return std::nullopt;
            if (!it->is_boolean())
                throw std::invalid_argument("cross_reference: connector 'pluggable' must be "
                                            "true, false or null");
            return it->get<bool>();
        };
        const auto op = pluggable_of(original), sp = pluggable_of(cand);
        if (op || sp) {
            const auto said = [](const std::optional<bool>& v) {
                return !v ? std::string("not stated")
                          : *v ? std::string("pluggable") : std::string("fixed");
            };
            if (op && sp && *op == *sp) {
                params.push_back({{"name", "pluggable"}, {"verdict", PASS}});
            } else if (op && sp) {
                params.push_back({{"name", "pluggable"}, {"verdict", FAIL}});
                notes.push_back("terminal block form differs: " + said(op) + " -> " + said(sp) +
                                " — a pluggable plug mates onto a header, a fixed block is "
                                "soldered to the board; not a drop-in");
                return reject("pluggable vs fixed terminal block (a different form factor)");
            } else {
                params.push_back({{"name", "pluggable"}, {"verdict", UNVERIFIED}});
                connector_pluggable_unverified = true;
                notes.push_back("pluggable vs fixed not established (original: " + said(op) +
                                ", substitute: " + said(sp) +
                                ") — confirm both are the same form (pluggable plug or fixed "
                                "PCB block) from the datasheets");
            }
        }
    }
    // ── resistor TCR and operating range: say it, and do not call an unknown a drop-in ──
    // TNPW0805680RDETA (25 ppm/K thin film) took ERJS06D6800V (100 ppm/K) with nothing said
    // (Qarlos 2026-11-08): the TCR verdict alone does not tell the engineer how far apart the
    // two are. And an operating range the original states but the substitute's record does
    // not is UNVERIFIED — drop_in asserts no rating regressed, which nobody has shown, so it
    // takes the same minor_review cap as an unverified footprint.
    // The same operating-range rule holds for a magnetic (Qarlos 2026-11-09, seed 20680025):
    // its PARAM_SPECS carry temp_min_C / temp_max_C with no warn band, and an end the
    // original states but the substitute's record does not takes the same cap here.
    // Set by an operating-range end or (resistor) a TCR the original states and the
    // substitute's record does not: both are temperature behaviour nobody has shown kept.
    bool temp_unverified = false;
    if (cat == "resistor" || cat == "magnetic") {
        auto verdict_of = [&](const char* name) -> std::string {
            for (const auto& p : params)
                if (p.value("name", "") == name) return p.value("verdict", "");
            return "";
        };
        const std::string tv = verdict_of("tcr");
        const auto o_tcr = num(original, "tcr"), s_tcr = num(cand, "tcr");
        if (cat == "resistor" && (tv == WARN || tv == FAIL) && o_tcr && s_tcr) {
            char b[160];
            std::snprintf(b, sizeof b,
                          "TCR +/-%g ppm/K vs the original's +/-%g ppm/K (%.3gx worse)",
                          std::fabs(*s_tcr), std::fabs(*o_tcr),
                          std::fabs(*o_tcr) > 0 ? std::fabs(*s_tcr) / std::fabs(*o_tcr) : 0.0);
            notes.push_back(std::string(b) +
                            " — the resistance drifts that much more over temperature; verify "
                            "anywhere the value sets a ratio, a reference or a gain");
        }
        // A TCR the original states and the substitute's record does not is UNVERIFIED, noted,
        // and capped at minor_review — the operating-range rule below, for the same reason:
        // drop_in asserts nothing regressed, and nobody has shown the drift did not.
        // CRCW080530R0JNTA (200 ppm/K) took three YAGEO AC0805 parts, whose records state no
        // TCR, as drop_in with no note (Qarlos 2026-11-09, seed 22146231). When NEITHER record
        // states one, the gap is the same for every candidate and nothing is said.
        if (cat == "resistor" && tv == UNVERIFIED && o_tcr && !s_tcr) {
            temp_unverified = true;
            char b[96];
            std::snprintf(b, sizeof b, "the original states a TCR of +/-%g ppm/K",
                          std::fabs(*o_tcr));
            notes.push_back(std::string(b) +
                            "; the substitute's record states none — its drift over temperature "
                            "is unknown; confirm it from the datasheet");
        }
        for (const char* k : {"temp_min_C", "temp_max_C"}) {
            if (verdict_of(k) == UNVERIFIED && num(original, k) && !num(cand, k)) {
                temp_unverified = true;
                notes.push_back(std::string("the original states its operating range to ") +
                                degc(*num(original, k)) +
                                " degC; the substitute's record states no " +
                                (std::string(k) == "temp_min_C" ? "minimum" : "maximum") +
                                " operating temperature — confirm it from the datasheet");
            }
        }
        if (temp_unverified) demote();
    }
    // A magnetic's SRF short of the original's (srf spec, CrossRefParams.hpp): name both
    // frequencies, so the engineer can set the gap against the circuit's working frequency.
    if (cat == "magnetic") {
        for (const auto& p : params) {
            if (p.value("name", "") != "srf") continue;
            const std::string v = p.value("verdict", "");
            const auto o_srf = num(original, "srf"), s_srf = num(cand, "srf");
            if ((v == WARN || v == FAIL) && o_srf && s_srf) {
                char b[200];
                std::snprintf(b, sizeof b,
                              "self-resonant frequency %g MHz vs the original's %g MHz (%.3gx "
                              "lower)",
                              *s_srf / 1e6, *o_srf / 1e6, *o_srf / *s_srf);
                notes.push_back(std::string(b) +
                                " — it stops behaving as the same inductor at a lower "
                                "frequency; verify against the circuit's working frequency");
            }
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

    if (timebase_unverified) demote();
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

    // ── diode package topology (die configuration / internal connection) ─────
    // How many die a package holds and how they are bonded inside it fix its pinout and the
    // circuit it forms: a common-cathode centre-tap rectifier is not a common-anode one, a
    // series pair (BAV99) is neither, and a single diode is not a dual. Rule (coordinator,
    // Qarlos 2026-11-08 round 2), applied to each field separately, from the record only —
    // never inferred from the MPN or the package:
    //  * both sides state it and the values differ: FAIL, the candidate is rejected, and the
    //    note names both — the topology is not interchangeable;
    //  * one side states it and the other does not: UNVERIFIED with a note, and the grade is
    //    capped below drop_in (minor_review). It adds no further penalty or cap where the
    //    forward-current basis rule above has already capped the row for the same silence;
    //  * neither side states it: silent (the overwhelming majority of the catalogue).
    // Decision for the owner to confirm: a stated 'single' against an unstated configuration
    // is UNVERIFIED too, although the schema reads an absent dieConfiguration as one die.
    bool topology_unverified = false;
    if (cat == "diode") {
        struct Field {
            const char* key;
            const char* label;
        };
        for (const Field f : {Field{"die_configuration", "die configuration"},
                              Field{"internal_connection", "internal connection"}}) {
            const std::string o = str(original, f.key), s = str(cand, f.key);
            if (o.empty() && s.empty()) continue;
            if (!o.empty() && !s.empty()) {
                if (o == s) {
                    params.push_back({{"name", f.key}, {"verdict", PASS}});
                    continue;
                }
                params.push_back({{"name", f.key}, {"verdict", FAIL}});
                notes.push_back(std::string(f.label) + " differs: the original is '" + o +
                                "', the substitute '" + s +
                                "' — a different package topology and pinout, not a substitute");
                return reject("a different diode package topology");
            }
            params.push_back({{"name", f.key}, {"verdict", UNVERIFIED}});
            notes.push_back(o.empty()
                                ? "the substitute's record states " + std::string(f.label) + " '" +
                                      s + "' and the original's states none — confirm the "
                                      "original's package topology and pinout match"
                                : "the original's record states " + std::string(f.label) + " '" +
                                      o + "' and the substitute's states none — confirm the "
                                      "substitute's package topology and pinout from its "
                                      "datasheet");
            if (!current_basis_blind && !topology_unverified) {
                demote();
                penalty += opt.gate_weight * kVerdictWarnPenalty;
            }
            topology_unverified = true;
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

    // ── mount type ───────────────────────────────────────────────────────────
    const std::string o_pkg = str(original, "package").empty() ? str(original, "case_code")
                                                               : str(original, "package");
    const std::string s_pkg =
        str(cand, "package").empty() ? str(cand, "case_code") : str(cand, "package");
    // Mount type: an SMD part cannot stand in for a leaded one. Skipped for
    // categories whose package strings vary too much by series to classify.
    // This is the ONE mount comparison for every family, connectors included. A
    // connector's board attach is CONAS mechanical.mountingStyle (smt / tht / pressFit /
    // skedd / panel / cable), which its spec carries under `mounting`; every other
    // family's explicit assembly type travels as `mount`. compare_mount classifies the
    // connector classes too (normalize_mount), so a press-fit part against an SMT one is
    // Incompatible here rather than in a parallel connector-only key that pushed a second
    // 'mounting' verdict and a second note for the same pair.
    const bool connector = cat == "connector";
    const char* mount_key = connector ? "mounting" : "mount";
    const std::string o_mt = str(original, mount_key), s_mt = str(cand, mount_key);
    // Name what each side's class was read from — its EXPLICIT assembly type when the
    // record states one, its package string otherwise (mount_class's order). Always
    // quoting the packages printed a half-empty "mount type incompatible:  -> 8x8x10"
    // for the many records that state a mount type but carry no package string.
    auto mount_src = [](const std::string& mt, const std::string& pkg) {
        return !normalize_mount(mt).empty() ? mt : pkg;
    };
    // For every other family the mount is part of the physical fit and goes with
    // check_footprint. A connector's board attach is an IDENTITY of the part (owner's rule,
    // 2026-11-09): it is compared even when the caller switches the footprint check off.
    if (connector || opt.check_footprint) {
        const MountMatch mount_match = mount_gate_applies(cat)
                                           ? compare_mount(o_mt, s_mt, o_pkg, s_pkg)
                                           : MountMatch::Same;
        if (mount_match == MountMatch::Incompatible) {
            params.push_back({{"name", "mounting"}, {"verdict", FAIL}});
            if (connector) {
                // A press-fit part needs a press and plated holes sized for it, an SMT one a
                // reflow pass on pads: a different board process, not a parameter.
                notes.push_back("board mount differs: " + mount_src(o_mt, o_pkg) + " -> " +
                                mount_src(s_mt, s_pkg) +
                                " — a different board-attach process, not a drop-in");
                return reject("board mount differs (a different board-attach process)");
            }
            notes.push_back("mount type incompatible: " + mount_src(o_mt, o_pkg) + " -> " +
                            mount_src(s_mt, s_pkg));
            return reject("mount type incompatible (a different board-mount process)");
        }
        const std::string o_cls = mount_class(o_mt, o_pkg), s_cls = mount_class(s_mt, s_pkg);
        // A connector's board attach is one of its identity rows in the verdict table, so a
        // match is reported, not left blank (the other families keep a silent match).
        if (connector && mount_match == MountMatch::Same && !o_cls.empty() && !s_cls.empty())
            params.push_back({{"name", "mounting"}, {"verdict", PASS}});
        if (connector && mount_match == MountMatch::Unknown &&
            (!o_cls.empty() || !s_cls.empty())) {
            // One record states its board attach and the other does not: whether it goes on
            // the same board process is unknown, so the swap cannot be called a drop-in —
            // WT16PRD8CD (no mountingStyle, no pitch) came back drop_in for an SMT Samtec
            // original. Capped like an unverified footprint (minor_review).
            params.push_back({{"name", "mounting"}, {"verdict", UNVERIFIED}});
            mount_unverified = true;
            notes.push_back(!o_cls.empty()
                                ? "the original is " + mount_src(o_mt, o_pkg) +
                                      "-mounted; the substitute's record states no board mount — "
                                      "confirm it uses the same board-attach process"
                                : "the original's record states no board mount; the substitute "
                                  "is " + mount_src(s_mt, s_pkg) + "-mounted — confirm it uses the same "
                                  "board-attach process");
        } else if (mount_match == MountMatch::Unknown && (!o_cls.empty() || !s_cls.empty())) {
            // One record states its mount and the other does not: not on record is not
            // compatible — SMD vs through-hole is a different board process. Capped like
            // an unverified footprint. (Neither stating one leaves the axis silent, the
            // same known-vs-absent rule the height axis follows.)
            params.push_back({{"name", "mounting"}, {"verdict", UNVERIFIED}});
            mount_unverified = true;
            // Report what the record SAYS: a stated mount Kelvin does not classify (CAS
            // "Screw Type", "Chassis Mount") is quoted as stated, never called "not stated" —
            // the note contradicted the record when Snap-In was unclassified (Qarlos 2026-11-09).
            auto said = [](const std::string& cls, const std::string& mt) {
                if (!cls.empty()) return cls;
                if (!mt.empty()) return "\"" + mt + "\" (not classifiable as SMD or through-hole)";
                return std::string("not stated");
            };
            notes.push_back("mount type not established (original: " + said(o_cls, o_mt) +
                            ", substitute: " + said(s_cls, s_mt) +
                            ") — confirm SMD vs through-hole from the datasheets");
        }
    }

    // ── physical fit ─────────────────────────────────────────────────────────
    if (opt.check_footprint) {
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
        // A different row count is the same failure (connector_pitch_fit): reported here even
        // when a record states no pitch, since the arrangement alone settles it.
        if (connector_pitch.rows_differ && !out.contains("footprint")) {
            out["footprint"] = "different_land_pattern";
            params.push_back({{"name", "footprint"}, {"verdict", FAIL}});
            penalty += opt.footprint_weight * kVerdictFailPenalty;
            demote();
            notes.push_back("contact rows differ: " +
                            std::to_string(static_cast<long>(connector_pitch.o_rows)) + " -> " +
                            std::to_string(static_cast<long>(connector_pitch.s_rows)) +
                            " rows — a different pin field, a different land pattern, not a "
                            "drop-in");
        }
        if (connector_pitch_conflict && o_pitch && s_pitch && !out.contains("footprint")) {
            out["footprint"] = "different_land_pattern";
            params.push_back({{"name", "footprint"}, {"verdict", FAIL}});
            penalty += opt.footprint_weight * kVerdictFailPenalty;
            std::string note = "pitch differs: " + mm(*o_pitch * 1e-3) + " -> " +
                               mm(*s_pitch * 1e-3) + " mm (" +
                               signed_pct(100.0 * (*s_pitch / *o_pitch - 1.0)) + ")";
            // The row the verdict was taken over (connector_pitch_fit): the per-row count
            // when a record states the rows, else the stated position count.
            std::optional<double> n;
            if (connector_pitch.per_row > 0.0) n = connector_pitch.per_row;
            else n = num(original, "positions");
            // Without a stated row count the verdict rests on the adjacent contact alone
            // (connector_pitch_fit); the end-pin figure is then for a single row, said so.
            if (n && *n >= 2.0)
                note += (connector_pitch.per_row > 0.0 ? "; over " : "; in a single row of ") +
                        std::to_string(static_cast<long>(*n)) +
                        (connector_pitch.per_row > 0.0 ? " positions per row" : " positions") +
                        " the last contact sits " +
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
        // ── lead pitch: the land of a LEADED body ───────────────────────────
        // A through-hole part lands on its holes, and where they sit is the lead pitch,
        // not the body outline: a radial film capacitor's PCM 15 IS its land pattern, its
        // 18 x 7 mm box only the keep-out. The body compare below therefore cannot call a
        // leaded part's land kept on its own — it graded R60II34705030K (no pitch on
        // record) 'fits' against MKP4D034704D00MSSD (PCM 15, two-hole land) on the box
        // size alone (Qarlos 2026-11-08, seed 20276947). Rule, for every family except
        // connectors (whose pitch is their own axis above), whenever either side is leaded:
        //   both lead pitches stated, more than kLeadPitchTolM apart -> a different land
        //     pattern (FAIL, a re-layout), exactly like the connector pitch;
        //   both stated and equal                                -> the holes are kept;
        //     the body compare still decides clearance and keep-out;
        //   either one unstated -> the land is established only by two equal case codes
        //     (a registered outline such as DO-41 or TO-220 names its hole pattern); with
        //     no such case match a body 'fits' becomes UNVERIFIED, never a pass.
        // DECISIONS for the owner: kLeadPitchTolM = 0.1 mm treats 2.50 / 2.54 and
        // 5.00 / 5.08 mm as one hole pattern (the usual metric/inch cross-use); and an
        // axial part with no pitch on record is held to the same rule, although its lead
        // bend is chosen by the layout rather than by the part.
        constexpr double kLeadPitchTolM = 0.1e-3;
        const std::string o_mcls = mount_class(o_mt, o_pkg), s_mcls = mount_class(s_mt, s_pkg);
        const bool leaded_pair = cat != "connector" && (o_mcls == "leaded" || s_mcls == "leaded");
        const auto o_lp = num(original, "lead_pitch_m"), s_lp = num(cand, "lead_pitch_m");
        bool lead_land_unverified = false;
        if (leaded_pair && !out.contains("footprint")) {
            if (o_lp && s_lp) {
                if (std::fabs(*o_lp - *s_lp) > kLeadPitchTolM) {
                    out["footprint"] = "different_land_pattern";
                    params.push_back({{"name", "footprint"}, {"verdict", FAIL}});
                    demote();
                    penalty += opt.footprint_weight * kVerdictFailPenalty;
                    notes.push_back("lead pitch differs: " + mm(*o_lp) + " -> " + mm(*s_lp) +
                                    " mm — the holes are in different places; a different land "
                                    "pattern, not a drop-in");
                }
            } else {
                const std::string oc = str(original, "case_code"), sc = str(cand, "case_code");
                const bool same_case = cat != "magnetic" && cat != "chipBead" && !oc.empty() &&
                                       !sc.empty() &&
                                       case_identity(cat, oc) == case_identity(cat, sc);
                lead_land_unverified = !same_case;
            }
        }
        // A wide-terminal chip (1225, 0612, ...) against a record that states no case code:
        // the outline compare below would turn the 3.2 x 6.4 mm 1225 and call it a fit for a
        // 6.6 x 3.2 mm drawing, but its pads run along the long sides and nothing in the
        // other record says its land does (M55342H09B10D0PWS vs AC1225FK-0710RL, Qarlos
        // 2026-11-08). When BOTH state codes and they differ, the case gate above has
        // already ruled. So the fit is UNVERIFIED — capped like any unverified land.
        if (cat == "resistor" && !out.contains("footprint")) {
            const std::string oc = str(original, "case_code"), sc = str(cand, "case_code");
            const bool ow = wide_terminal_chip_code(oc), sw = wide_terminal_chip_code(sc);
            if (ow != sw && (ow ? sc : oc).empty()) {
                out["footprint"] = "unknown";
                params.push_back({{"name", "footprint"}, {"verdict", UNVERIFIED}});
                demote();
                penalty += opt.footprint_weight * kUnknownDimPenalty;
                // The note claims only what the records establish (Qarlos 2026-11-09:
                // RWR80S28R0FSRSL and RLR32C2701GRRSL are axial bodies 10-14 mm long, and
                // ERJB2CJR024V's LVR candidates state no dimensions at all, yet each note
                // said the outlines "match when turned"). Rule:
                //   * both records state both land axes and the orientation-agnostic compare
                //     finds the same box (no axis over by more than the 2 % slack, none short
                //     of kUndersizeBand) -> the outlines match only when turned;
                //   * both state both axes and the boxes differ -> say so, with both boxes;
                //   * either record lacks a complete land outline -> say which one states no
                //     dimensions; nothing about the outline is claimed.
                // The verdict is UNVERIFIED in every case, as before.
                const std::string wide_side = sw ? "substitute" : "original";
                const std::string plain_side = sw ? "original" : "substitute";
                const auto& wide_dims = sw ? s_dims : o_dims;
                const auto& plain_dims = sw ? o_dims : s_dims;
                auto box = [](const Dims& d) { return mm(*d.length) + " x " + mm(*d.width) + " mm"; };
                std::string outline;
                if (plain_dims && has_land(*plain_dims) && wide_dims && has_land(*wide_dims)) {
                    const LandCompare lc = compare_land(*plain_dims, *wide_dims);
                    const bool same_box =
                        lc.overflow <= kDimTolerance - 1.0 && lc.lin >= kUndersizeBand;
                    outline = same_box
                                  ? "the outlines match only when turned, the pads do not"
                                  : "the outlines differ as well (" + plain_side + " " +
                                        box(*plain_dims) + ", " + wide_side + " " +
                                        box(*wide_dims) + ")";
                } else {
                    const bool plain_has = plain_dims && has_land(*plain_dims);
                    const bool wide_has = wide_dims && has_land(*wide_dims);
                    outline = std::string("the ") +
                              (!plain_has && !wide_has ? "records state"
                               : !plain_has            ? plain_side + "'s record states"
                                                       : wide_side + "'s record states") +
                              " no complete outline dimensions, so the outlines could not be "
                              "compared either";
                }
                notes.push_back("the " + wide_side + " is a wide-terminal " + (sw ? sc : oc) +
                                " chip (terminations along the long sides) and the " +
                                plain_side + "'s record states no case code — " + outline +
                                "; verify the land pattern");
            }
        }
        // ── same stated case = same land (MOSFETs, diodes) ──────────────────
        // A MOSFET or diode case code names a registered outline (TO-220, DPAK, SOD-323,
        // DO-214AB ...), and two records naming the same one name the same pads. The body
        // compare below must not overrule that with a box neither record states: NTP18G120
        // (TO-220, drawn 15.7 x 10.2 mm) took IPP200N15N3 (TO-220, no mechanical block) as
        // "smaller body ... not a drop-in" because the candidate's box was the package
        // TABLE's 10.16 x 4.57 mm TO-220 row, whose axes are width x thickness (Qarlos
        // 2026-11-09, seed 22041502). The same gap left a SOD323 zener 'unknown' against a
        // SOD-323 one (seed 21517857). Rule: both records state case codes with the same
        // case_identity, and at least one side's land is NOT a drawing of its own (a table
        // outline, or none at all) -> the land is kept: footprint 'fits', PASS. When BOTH
        // records draw their own bodies, those drawings are compared as before.
        if ((cat == "mosfet" || cat == "diode") && !out.contains("footprint")) {
            const std::string oc = str(original, "case_code"), sc = str(cand, "case_code");
            auto drawn_land = [](const std::optional<Dims>& d) {
                return d && has_land(*d) && !d->land_from_table;
            };
            if (!oc.empty() && !sc.empty() && case_identity(cat, oc) == case_identity(cat, sc) &&
                !(drawn_land(o_dims) && drawn_land(s_dims))) {
                out["footprint"] = "fits";
                params.push_back({{"name", "footprint"}, {"verdict", PASS}});
            }
        }
        FootprintTier tier = footprint_tier(o_dims, s_dims, strict_case);
        // Same standard chip code on both records: a drawing a few percent off the original's
        // is the same land, not a larger case (see kSameChipCodeBodySpread).
        bool same_code_spread = false;
        if (cat == "resistor" && tier == FootprintTier::OneSizeLarger && o_dims && s_dims &&
            same_standard_chip_code(str(original, "case_code"), str(cand, "case_code")) &&
            compare_land(*o_dims, *s_dims).overflow <= kSameChipCodeBodySpread) {
            tier = FootprintTier::Fits;
            same_code_spread = true;
        }
        // "One case size larger" describes a body that grew. One that overhangs the original
        // on one axis but is materially SHORTER on the other (below kUndersizeBand, the same
        // "materially smaller" line footprint_tier draws) did not grow: it is a different land.
        // RNC55K1472FSB14 (6.35 x 2.39 mm axial) took ERJS14F1472U (3.2 x 2.5 mm chip) as
        // "about one case size larger" (Qarlos 2026-11-08 seed 20276947). The overhang still
        // costs what it did — same penalty rung, same WARN, same minor_review cap — only the
        // tier name and the note change.
        const bool crossed_land = tier == FootprintTier::OneSizeLarger && o_dims && s_dims &&
                                  compare_land(*o_dims, *s_dims).lin < kUndersizeBand;
        // A LAND to fit to is what this branch needs, not merely a drawing. An
        // original whose record states only a height has no pads to compare
        // against, so it belongs to the "original states no package dimensions"
        // branch below — which is where it went before a land-less drawing could
        // reach here at all.
        if (o_dims && has_any_land(*o_dims) && !out.contains("footprint")) {
            // A same-code spread scores as the fit it is: the strict_case Fits term (0 at the
            // same size, rising only for an undersize body), not the one-size-larger rung.
            double fit_penalty =
                same_code_spread
                    ? kUndersizeWeight * std::max(0.0, 1.0 - compare_land(*o_dims, *s_dims).lin)
                    : footprint_penalty(o_dims, s_dims, strict_case);
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
            // "different_case" is the existing name for a land the pads cannot match (the
            // case-code gate above); the grade reads it as a WARN, exactly as one_size_larger.
            out["footprint"] = crossed_land ? "different_case" : footprint_tier_name(tier);
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
            if (crossed_land) {
                demote();
                notes.push_back("different land, not a larger case (" + land_mm(*s_dims) +
                                " vs " + land_mm(*o_dims) +
                                ") — longer on one axis and much shorter on the other, so the "
                                "pads do not match; it needs a new land pattern, verify the pads "
                                "before substituting");
            } else if (tier == FootprintTier::OneSizeLarger) {
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
        // The lead-pitch rule's last case (see above): a leaded body that 'fits' with the
        // hole pattern on record for neither or only one side has not had its land
        // compared at all — UNVERIFIED, capped like any unverified footprint.
        if (lead_land_unverified && out.value("footprint", "") == "fits") {
            out["footprint"] = "unknown";
            for (auto& p : params)
                if (p.value("name", "") == "footprint") p["verdict"] = UNVERIFIED;
            demote();
            penalty += opt.footprint_weight * kUnknownDimPenalty;
            auto pitch_said = [](const std::optional<double>& lp) {
                return lp ? mm(*lp) + " mm" : std::string("not stated");
            };
            notes.push_back("the body fits, but the lead pitch — the land pattern of a leaded "
                            "part — is not on record for both (original: " + pitch_said(o_lp) +
                            ", substitute: " + pitch_said(s_lp) +
                            "); footprint fit is UNVERIFIED, confirm the hole pattern from the "
                            "datasheet");
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
    // A regression here is a regression on the dielectric axis, so it counts against the
    // direction exactly as a WARN/FAIL on a numeric axis does (ABT #2039): a C0G original
    // with an N750 substitute came back an 'upgrade' on its higher voltage (seed 21517857).
    bool dielectric_unverified = false;
    if (cat == "capacitor") {
        const std::string reg = dielectric_regression(str(original, "dielectric_code"),
                                                      str(cand, "dielectric_code"));
        if (!reg.empty()) {
            params.push_back({{"name", "dielectric_envelope"}, {"verdict", FAIL}});
            ++worse;
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            notes.push_back(reg);
        } else {
            // Two DIFFERENT stated codes whose equivalence nothing above could decide (an
            // undecodable code such as SL, BX or a vendor's own, or two class-1 codes of one
            // coefficient where one states no tolerance): never silent, never a drop-in. The
            // dielectric_code verdict is UNVERIFIED; say so and cap the grade like any
            // unverified fit (Qarlos 2026-11-09: a C0G original's dielectric axis must not be
            // passed over without a word).
            const std::string ok = dielectric_key(str(original, "dielectric_code")),
                              sk = dielectric_key(str(cand, "dielectric_code"));
            bool unverified = false;
            for (const auto& p : params)
                if (p.value("name", "") == "dielectric_code" &&
                    p.value("verdict", "") == std::string(UNVERIFIED))
                    unverified = true;
            if (!ok.empty() && !sk.empty() && ok != sk && unverified) {
                dielectric_unverified = true;
                demote();
                notes.push_back("dielectric codes differ (" + ok + " -> " + sk +
                                ") and their temperature characteristics could not be compared "
                                "— confirm the substitute's dielectric from its datasheet");
            }
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
        // ── |Vgs(th)| max: the original states one, the substitute does not ──────────
        // (Qarlos 2026-11-09, seed 21098941: logic-level NVMFS5C670NLET1G, Vth max 2 V, took
        // BSC060N10NS6 / BSC046N08NS5 whose records state only a TYPICAL of 3.5 / 3.8 V, with
        // vgs_threshold_max 'unverified' and no caveat.) Rule:
        //   * the substitute's stated TYPICAL is above the original's guaranteed maximum ->
        //     its maximum is higher still (max >= typ), an ESTABLISHED downgrade for the
        //     original's gate drive: the verdict becomes WARN, or FAIL beyond the table's 2x
        //     band, exactly what the stated maximum would have earned at that value at least;
        //   * otherwise -> UNVERIFIED, demoted out of 'recommended', with a note naming the
        //     original's maximum and what the substitute's record states.
        // The typical is never read AS the maximum: it only bounds it from below.
        if (o_th && !s_th) {
            const auto s_typ = num(cand, "vgs_threshold_typ");
            for (auto& p : params) {
                if (p.value("name", "") != "vgs_threshold_max") continue;
                if (s_typ && *s_typ > *o_th + 1e-9) {
                    const bool fail = *s_typ > *o_th * 2.0;
                    p["verdict"] = fail ? FAIL : WARN;
                    demote();
                    penalty += opt.gate_weight * (fail ? kVerdictFailPenalty : kVerdictWarnPenalty);
                    ++worse;
                    notes.push_back("higher gate threshold than the original: the substitute's "
                                    "TYPICAL Vgs(th) is " + volts(*s_typ) +
                                    " V, already above the original's guaranteed maximum of " +
                                    volts(*o_th) +
                                    " V, so its maximum is higher still — a downgrade for the "
                                    "original's gate drive (a logic-level drive may not enhance "
                                    "it fully); check the drive voltage against the substitute's "
                                    "datasheet");
                } else {
                    p["verdict"] = UNVERIFIED;
                    demote();
                    notes.push_back(
                        "the original's gate threshold is at most " + volts(*o_th) +
                        " V; the substitute's record states no maximum" +
                        (s_typ ? " (typical " + volts(*s_typ) + " V)" : std::string()) +
                        ", so Vgs(th) is UNVERIFIED — confirm the substitute enhances at the "
                        "original's gate drive from its datasheet");
                }
            }
        }
        // A higher maximum inside the 1.25x band the gate_drive check below leaves alone is
        // still a downgrade for a logic-level drive (less overdrive at the same Vgs). The
        // table verdict already WARNs it; it was silent.
        if (o_th && s_th && *s_th > *o_th + 1e-9 && *s_th <= *o_th * 1.25)
            notes.push_back("higher gate threshold than the original (Vgs(th) max " +
                            volts(*s_th) + " V vs " + volts(*o_th) +
                            " V) — less gate overdrive at the original's drive voltage, a "
                            "downgrade for a logic-level drive; check enhancement and switching "
                            "at that drive");
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
    // A bare "Automotive" label on the ORIGINAL (Qarlos 2026-11-09, seed 20470567). The label
    // reads UNKNOWN for a SUBSTITUTE (it does not establish AEC-Q), and the same reading on
    // the original's side made the whole gate skip: Infineon's IPW65R048CFDA, qualification
    // "Automotive", was replaced by Vishay SiHG61N65EF (no qualification on record) graded
    // 'recommended' with no note. The original's label is still a statement that the part
    // was built for automotive service, and the design it sits in may depend on that, so the
    // gate engages for it too. Rule (DECISION for the owner to confirm):
    //   * substitute names AEC-Q, or carries the same bare automotive label -> nothing
    //     flagged (it claims at least what the original claims);
    //   * substitute states a non-automotive grade (Industrial, Standard, ...) -> FAIL, as for
    //     an AEC-Q original;
    //   * substitute states nothing -> UNVERIFIED, demoted to partial, with a note.
    auto automotive_label = [&](const json& p) {
        const std::string q = lower_copy(str(p, "qualification"));
        return q.find("automotive") != std::string::npos &&
               q.find("non-automotive") == std::string::npos &&
               q.find("aec") == std::string::npos;
    };
    const Grade o_grade = grade_of(original);
    const bool o_label_only = o_grade != Grade::Aec && automotive_label(original);
    if (o_label_only) {
        const Grade g = grade_of(cand);
        const std::string oq = str(original, "qualification");
        const std::string q = str(cand, "qualification");
        if (g == Grade::NotAec) {
            params.push_back({{"name", "automotive_grade"}, {"verdict", FAIL}});
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            notes.push_back("original is labelled \"" + oq + "\" (an automotive part); the "
                            "substitute's record states \"" + q +
                            "\" — not an automotive part");
        } else if (g == Grade::Unknown && !automotive_label(cand)) {
            params.push_back({{"name", "automotive_grade"}, {"verdict", UNVERIFIED}});
            demote();
            notes.push_back("original is labelled \"" + oq +
                            "\" (an automotive part); the substitute's record states no "
                            "qualification, so its automotive grade is UNVERIFIED — confirm "
                            "AEC-Q qualification from the datasheet");
        }
    }
    if (o_grade == Grade::Aec) {
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
    // So does a connector orientation that could not be established.
    if (connector_orientation_unverified) footprint_unverified = true;
    // And a connector pin field whose alignment could not be established.
    if (connector_pitch_unverified) footprint_unverified = true;
    // And a pluggable / fixed form stated on one side only.
    if (connector_pluggable_unverified) footprint_unverified = true;
    // drop_in asserts no rating regressed; a power nobody can state at the original's rating
    // temperature has not been shown not to, so it takes the same minor_review cap.
    out["grade"] = grade_for(status, tier, any_warn, any_fail,
                             footprint_unverified || power_basis_unverified ||
                                 ripple_basis_unverified ||
                                 topology_unverified ||
                                 mount_unverified || temp_unverified || timebase_unverified ||
                                 dielectric_unverified,
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
