// The MOSFET, diode and connector curves under the owner's curve rule (2026-11-14: "make sure
// that Kelvin compares ALL existing graphs"). Every comparison goes through the shared helper
// (CrossRefCurves.hpp: compare_curves / curve_at); this header only knows, per curve, which
// view field holds it, which scalar spec's bands judge it, how it is interpolated and how its
// values and conditions are printed — plus the reads that resolve a scalar basis mismatch
// (Coss at the other side's Vds, Vf at its test current, Cj at its bias).
//
// The rule, as implemented here:
//   * a curve is compared only when BOTH records state it; one side alone is nothing to
//     compare against and adds no row (the scalars keep their own rows and notes);
//   * curves are the shared spec shape (CurvePts on the row -> curve_from_spec, SpecCurve);
//   * conditions (conditions_differ): every numeric condition and the free-text one (mounting
//     / time base) must be stated by both with the same value; any difference, or a condition
//     only one side states (owner 2026-11-14), is "different conditions" -> UNVERIFIED + note
//     naming both sets, no cap;
//   * a log-log quantity holding a non-positive sample -> UNVERIFIED + note, no cap;
//   * no overlap -> UNVERIFIED + note with both ranges, no cap;
//   * otherwise the worst point of compare_curves decides, and the note names the x where it
//     is worst, both values, the interpolation and the spec whose bands judged it.
// Program-only, no I/O.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "CrossRefCurves.hpp"
#include "CrossRefParams.hpp"
#include "CrossRefScore.hpp"

namespace kelvin::crossref::curve_checks {

using json = nlohmann::json;

// A curve the record states but whose axes carry no units (the row's {"unit_unstated": true},
// Browse curve_json): stated, never read.
inline bool unit_unstated(const json& view, const char* key) {
    const auto it = view.find(key);
    return it != view.end() && it->is_object() && it->value("unit_unstated", false);
}
// view[key] as one curve (the shared spec shape, curve_from_spec); nullopt when absent / null
// or stated without units (unit_unstated).
inline std::optional<SpecCurve> view_curve(const json& view, const char* key) {
    if (unit_unstated(view, key)) return std::nullopt;
    return curve_from_spec(view, key);
}
// view[key] as a list of curves (ambient ratings); empty when absent / null.
inline std::vector<SpecCurve> view_curves(const json& view, const char* key) {
    std::vector<SpecCurve> out;
    const auto it = view.find(key);
    if (it == view.end() || it->is_null()) return out;
    if (!it->is_array())
        throw std::invalid_argument(std::string("'") + view.value("mpn", "") + "' " + key +
                                    " must be an array of curves");
    for (const auto& c : *it) {
        const json one{{"c", c}};
        out.push_back(*curve_from_spec(one, "c"));
    }
    return out;
}

// The gate-charge curve is stated as Vgs against Qg; the Qg spec judges CHARGE, at the gate
// voltages both curves reach. Read as "the charge at which the gate first reaches Vgs": the
// samples in Qg order, keeping each one whose Vgs exceeds every earlier one (the Miller plateau
// and digitisation wobble do not raise Vgs, so they are not points of this function; nothing is
// invented — every kept point is a stated sample, x and y swapped). nullopt when fewer than two
// such points remain (the caller reports that curve UNVERIFIED with a note).
inline std::optional<SpecCurve> charge_to_reach_vgs(const SpecCurve& vgs_of_qg) {
    std::vector<double> x, y;
    double max_v = -HUGE_VAL;
    for (size_t i = 0; i < vgs_of_qg.curve.x.size(); ++i) {
        if (!(vgs_of_qg.curve.y[i] > max_v)) continue;
        max_v = vgs_of_qg.curve.y[i];
        x.push_back(vgs_of_qg.curve.y[i]);
        y.push_back(vgs_of_qg.curve.x[i]);
    }
    if (x.size() < 2) return std::nullopt;
    SpecCurve out = vgs_of_qg;
    out.curve = Curve(std::move(x), std::move(y));
    return out;
}

// Why the curve cannot be read under `interp`, or nullopt when it can (log-log needs every
// sample positive).
inline std::optional<std::string> unreadable(const SpecCurve& c, Interp interp) {
    if (interp == Interp::LogLog)
        for (size_t i = 0; i < c.curve.x.size(); ++i)
            if (!(c.curve.x[i] > 0) || !(c.curve.y[i] > 0))
                return std::string("it holds a zero or negative sample and is read on "
                                   "logarithmic axes");
    return std::nullopt;
}

inline std::string conditions_text(const SpecCurve& c) {
    static const std::vector<std::pair<std::string, std::pair<const char*, const char*>>> k = {
        {"gateSourceVoltage", {"Vgs", "V"}},  {"drainSourceVoltage", {"Vds", "V"}},
        {"drainCurrent", {"Id", "A"}},        {"junctionTemperature", {"Tj", "degC"}},
        {"duration", {"duration", "s"}},      {"maxTemperatureRise", {"temperature rise", "K"}}};
    std::string s;
    auto add = [&](const std::string& t) { s += (s.empty() ? "" : ", ") + t; };
    char b[96];
    if (c.temperature) {
        std::snprintf(b, sizeof b, "T %g degC", *c.temperature);
        add(b);
    }
    if (c.dc_bias) {
        std::snprintf(b, sizeof b, "DC bias %g V", *c.dc_bias);
        add(b);
    }
    if (c.frequency) {
        const double v = *c.frequency;
        if (v >= 1e6) std::snprintf(b, sizeof b, "f %g MHz", v / 1e6);
        else if (v >= 1e3) std::snprintf(b, sizeof b, "f %g kHz", v / 1e3);
        else std::snprintf(b, sizeof b, "f %g Hz", v);
        add(b);
    }
    for (const auto& [key, v] : c.conditions) {
        const auto it = std::find_if(k.begin(), k.end(), [&](const auto& e) { return e.first == key; });
        if (it != k.end())
            std::snprintf(b, sizeof b, "%s %g %s", it->second.first, v, it->second.second);
        else
            std::snprintf(b, sizeof b, "%s %g", key.c_str(), v);
        add(b);
    }
    if (!c.condition_text.empty()) add("'" + c.condition_text + "'");
    return s.empty() ? std::string("none stated") : s;
}

// How one curve type is compared and printed.
struct Quantity {
    const char* row;       // params[] row name
    const char* what;      // "output capacitance against Vds (Coss(Vds))"
    const char* x_unit;    // printed unit of x, after x_scale
    double x_scale;
    const char* y_unit;    // printed unit of y, after y_scale
    double y_scale;
    ParamSpec spec;        // the scalar spec whose bands and direction judge each point
    const char* band_from; // "the Coss spec" — named in the note
    Interp interp;
};

inline std::string fmt(double v, double scale, const char* unit) {
    char b[48];
    std::snprintf(b, sizeof b, "%.4g %s", v * scale, unit);
    return b;
}

struct Result {
    const char* verdict = UNVERIFIED;
    std::optional<double> warn_depth;
    std::string note;
    bool compared = false;  // a verdict from values (PASS / WARN / FAIL)
    double orig_y = std::nan(""), sub_y = std::nan("");
};

// One original curve against one substitute curve.
inline Result compare_pair(const SpecCurve& o, const SpecCurve& s, const Quantity& q) {
    Result r;
    const std::string head = std::string(q.what) + ": ";
    for (const auto& [c, side] : {std::pair<const SpecCurve*, const char*>{&o, "original's"},
                                  std::pair<const SpecCurve*, const char*>{&s, "substitute's"}})
        if (auto why = unreadable(*c, q.interp)) {
            r.note = head + "the " + side + " curve cannot be read (" + *why +
                     ") — not compared; confirm from the datasheet";
            return r;
        }
    if (const std::string diff = conditions_differ(o, s); !diff.empty()) {
        r.note = head + "measured under different conditions (original: " + conditions_text(o) +
                 "; substitute: " + conditions_text(s) +
                 ") — not compared; confirm from the datasheets at the application's conditions";
        return r;
    }
    const char* how = q.interp == Interp::LogLog ? "log-log interpolation" : "linear interpolation";
    auto range = [&](const SpecCurve& c) {
        return fmt(c.curve.x.front(), q.x_scale, q.x_unit) + " to " +
               fmt(c.curve.x.back(), q.x_scale, q.x_unit);
    };
    const CurveVerdict cv = compare_curves(o.curve, s.curve, q.spec, q.interp);
    if (!cv.range || cv.points == 0) {
        r.note = head + "the measured ranges do not overlap (original " + range(o) +
                 ", substitute " + range(s) + ") — not compared, never extrapolated";
        return r;
    }
    r.verdict = cv.verdict;
    r.warn_depth = cv.warn_depth;
    r.compared = true;
    r.orig_y = cv.orig_y;
    r.sub_y = cv.sub_y;
    const std::string v = cv.verdict;
    char pts[32];
    std::snprintf(pts, sizeof pts, "%zu point%s", cv.points, cv.points == 1 ? "" : "s");
    r.note = head + "compared on both curves over " + fmt(cv.range->first, q.x_scale, q.x_unit) +
             " to " + fmt(cv.range->second, q.x_scale, q.x_unit) + " (" + pts + ", " + how +
             ", conditions " + conditions_text(o) + "); " +
             (v == PASS ? "closest to the original's limit" : "worst") + " at " +
             fmt(cv.worst_x, q.x_scale, q.x_unit) + ": substitute " +
             fmt(cv.sub_y, q.y_scale, q.y_unit) + " against the original's " +
             fmt(cv.orig_y, q.y_scale, q.y_unit) + " (" + v + ", judged with " + q.band_from +
             "'s bands)";
    return r;
}

inline int rank(const char* v) {
    const std::string s = v;
    return s == FAIL ? 3 : s == WARN ? 2 : s == PASS ? 1 : 0;
}

// Sets of curves (ambient ratings: one per mounting / time base / Vgs): every original curve
// against every substitute curve measured under the same conditions; the worst compared pair
// decides. No pair under common conditions -> UNVERIFIED, with the conditions of both listed.
inline Result compare_sets(const std::vector<SpecCurve>& os, const std::vector<SpecCurve>& ss,
                           const Quantity& q) {
    Result best;
    bool any_same = false;
    for (const auto& o : os)
        for (const auto& s : ss) {
            if (!conditions_differ(o, s).empty()) continue;
            Result r = compare_pair(o, s, q);
            if (!any_same || (r.compared && (!best.compared || rank(r.verdict) > rank(best.verdict))))
                best = r;
            any_same = true;
        }
    if (any_same) return best;
    auto list = [](const std::vector<SpecCurve>& v) {
        std::string s;
        for (const auto& c : v) s += (s.empty() ? "" : " | ") + conditions_text(c);
        return s;
    };
    best.note = std::string(q.what) + ": no rating of the substitute is stated under the "
                "conditions of one of the original's (original: " + list(os) +
                "; substitute: " + list(ss) + ") — not compared; confirm from the datasheets";
    return best;
}

// The curve's value at x — inside its measured range only, and only for a curve readable under
// `interp`; nullopt otherwise. Used to put a scalar on the other side's basis.
inline std::optional<double> read_at(const SpecCurve& c, double x, Interp interp) {
    if (unreadable(c, interp)) return std::nullopt;
    return curve_at(c.curve, x, interp);
}
inline std::optional<double> condition(const SpecCurve& c, const char* key) {
    for (const auto& [k, v] : c.conditions)
        if (k == key) return v;
    return std::nullopt;
}

// ── the quantities ───────────────────────────────────────────────────────────
// Bands: each curve is judged with its own quantity's scalar spec (CrossRefParams.hpp
// params_for) where one exists; Ciss / Crss have none and take the Coss spec's (the closest:
// the same device's capacitance, measured the same way); the reverse current has none and takes
// the quiescent-current spec's (the closest: an unwanted current drawn, lower is better); the
// ambient drain current takes the Id rating's bands (critical_ratings: WARN down to 0.7x).
// Interpolation: linear where the axis the data is stated on is linear (Vds from 0 V, Id,
// temperature, Vgs, reverse voltage — the Ir(Vr) and Cj(Vr) samples run linearly in Vr, and a
// Cj(Vr) curve may start at 0 V, which no logarithmic axis holds); log-log for Vf(If), whose
// current samples are spaced in decades.
inline const std::vector<std::pair<const char*, Quantity>>& mosfet_quantities() {
    static const std::vector<std::pair<const char*, Quantity>> q = {
        {"ciss_curve",
         {"ciss_vs_vds", "input capacitance against Vds (Ciss(Vds))", "V", 1.0, "pF", 1e12,
          {"ciss", Dir::Lower, 2.0, std::nullopt, false, false, nullptr}, "the Coss spec",
          Interp::Linear}},
        {"coss_curve",
         {"coss_vs_vds", "output capacitance against Vds (Coss(Vds))", "V", 1.0, "pF", 1e12,
          {"coss", Dir::Lower, 2.0, std::nullopt, false, false, nullptr}, "the Coss spec",
          Interp::Linear}},
        {"crss_curve",
         {"crss_vs_vds", "reverse transfer capacitance against Vds (Crss(Vds))", "V", 1.0, "pF",
          1e12, {"crss", Dir::Lower, 2.0, std::nullopt, false, false, nullptr}, "the Coss spec",
          Interp::Linear}},
        {"rds_on_id_curve",
         {"rds_on_vs_id", "on-resistance against drain current (Rds(on)(Id))", "A", 1.0,
          "mohm", 1e3, {"rds_on", Dir::Lower, 1.5, std::nullopt, false, false, nullptr},
          "the Rds(on) spec", Interp::Linear}},
        {"rds_on_tj_curve",
         {"rds_on_vs_tj", "on-resistance against junction temperature (Rds(on)(Tj))", "degC",
          1.0, "mohm", 1e3, {"rds_on", Dir::Lower, 1.5, std::nullopt, false, false, nullptr},
          "the Rds(on) spec", Interp::Linear}},
    };
    return q;
}

// The gate-charge curve, read as the charge to reach each Vgs (charge_to_reach_vgs) and judged
// with the Qg spec's bands at the gate voltages both curves reach.
inline const Quantity& gate_charge_quantity() {
    static const Quantity q{"qg_vs_vgs",
                            "gate charge to reach Vgs (Qg(Vgs), from the gate-charge curve)",
                            "V", 1.0, "nC", 1e9,
                            {"qg", Dir::Lower, 2.0, std::nullopt, false, false, nullptr},
                            "the Qg spec", Interp::Linear};
    return q;
}
// The ambient drain-current ratings (a set per record: one per mounting / time base / Vgs),
// judged with the Id rating's bands (critical_ratings: higher is better, WARN down to 0.7x).
// Compared only curve against curve: a rating read off them never stands in for the scalar Id.
inline const Quantity& id_ambient_quantity() {
    static const Quantity q{"id_vs_ambient",
                            "drain current rating against ambient temperature (Id(Ta))", "degC",
                            1.0, "A", 1.0,
                            {"id", Dir::Higher, 0.7, std::nullopt, false, false, nullptr},
                            "the Id rating", Interp::Linear};
    return q;
}
inline const std::vector<std::pair<const char*, Quantity>>& diode_quantities() {
    static const std::vector<std::pair<const char*, Quantity>> q = {
        {"vf_curve",
         {"vf_vs_if", "forward voltage against forward current (Vf(If))", "A", 1.0, "V", 1.0,
          {"vf", Dir::Lower, 1.2, std::nullopt, false, false, nullptr}, "the Vf spec",
          Interp::LogLog}},
        // No reverse-current spec exists: the quiescent-current spec's bands are borrowed (the
        // closest: an unwanted current drawn, lower is better, 3x).
        {"ir_curve",
         {"ir_vs_vr", "reverse current against reverse voltage (Ir(Vr))", "V", 1.0, "uA", 1e6,
          {"quiescent_current", Dir::Lower, 3.0, std::nullopt, false, false, nullptr},
          "the quiescent-current spec (borrowed: no reverse-current spec exists)",
          Interp::Linear}},
        {"cj_curve",
         {"cj_vs_vr", "junction capacitance against reverse voltage (Cj(Vr))", "V", 1.0, "pF",
          1e12, {"junction_capacitance", Dir::Lower, 2.0, std::nullopt, false, false, nullptr},
          "the junction-capacitance spec", Interp::Linear}},
    };
    return q;
}
// A connector's derating curve is a FRACTION of the per-contact rated current; in amperes it is
// that fraction times the record's own rated current, judged with the rated-current spec.
inline const Quantity& connector_current_quantity() {
    static const Quantity q{
        "current_vs_ambient",
        "per-contact current rating against ambient temperature (derating curve x rated current)",
        "degC", 1.0, "A", 1.0,
        {"rated_current_A", Dir::Higher, 0.9, std::nullopt, false, false, nullptr},
        "the rated-current spec", Interp::Linear};
    return q;
}

// Why a curve both records state cannot be compared before any value is read, or nullopt:
// one stated without axis units (unit_unstated) has no scale on record.
inline std::optional<Result> unscaled(const json& orig, const json& cand, const char* key,
                                      const Quantity& q) {
    const bool o = unit_unstated(orig, key), s = unit_unstated(cand, key);
    if (!o && !s) return std::nullopt;
    Result r;
    r.note = std::string(q.what) + ": the " +
             (o && s ? "original's and the substitute's curves state"
                     : o ? "original's curve states" : "substitute's curve states") +
             " no axis units (no xLabel / yLabel on record), so the scale is not known — not "
             "compared; confirm from the datasheet";
    return r;
}
inline bool states(const json& view, const char* key) {
    const auto it = view.find(key);
    return it != view.end() && !it->is_null();
}

// Every curve comparison for one candidate, in a fixed order: (row, result). Only curves BOTH
// records state produce a row.
struct CurveRow {
    std::string row;
    Result result;
};
inline std::vector<CurveRow> run(const std::string& cat, const json& orig, const json& cand) {
    std::vector<CurveRow> out;
    auto one = [&](const char* key, const Quantity& q) {
        if (!states(orig, key) || !states(cand, key)) return;
        if (auto u = unscaled(orig, cand, key, q)) {
            out.push_back({q.row, *u});
            return;
        }
        out.push_back({q.row, compare_pair(*view_curve(orig, key), *view_curve(cand, key), q)});
    };
    if (cat == "mosfet") {
        for (const auto& [key, q] : mosfet_quantities()) one(key, q);
        const Quantity& qg = gate_charge_quantity();
        if (states(orig, "qg_curve") && states(cand, "qg_curve")) {
            if (auto u = unscaled(orig, cand, "qg_curve", qg)) {
                out.push_back({qg.row, *u});
            } else {
                const auto o = charge_to_reach_vgs(*view_curve(orig, "qg_curve"));
                const auto s = charge_to_reach_vgs(*view_curve(cand, "qg_curve"));
                if (o && s) {
                    out.push_back({qg.row, compare_pair(*o, *s, qg)});
                } else {
                    Result r;
                    r.note = std::string(qg.what) + ": the " +
                             (!o && !s ? "original's and the substitute's gate-charge curves do"
                                       : !o ? "original's gate-charge curve does"
                                            : "substitute's gate-charge curve does") +
                             " not rise through two gate voltages, so no charge-to-reach-Vgs "
                             "curve can be read off it — not compared; confirm from the datasheet";
                    out.push_back({qg.row, r});
                }
            }
        }
        const auto os = view_curves(orig, "id_ambient_curves");
        const auto ss = view_curves(cand, "id_ambient_curves");
        if (!os.empty() && !ss.empty())
            out.push_back({id_ambient_quantity().row, compare_sets(os, ss, id_ambient_quantity())});
    } else if (cat == "diode") {
        for (const auto& [key, q] : diode_quantities()) one(key, q);
    } else if (cat == "connector") {
        const Quantity& q = connector_current_quantity();
        const char* key = "current_ambient_curve";
        if (states(orig, key) && states(cand, key)) {
            auto rated = [](const json& v) -> std::optional<double> {
                const auto it = v.find("rated_current_A");
                if (it == v.end() || !it->is_number() || !(it->get<double>() > 0))
                    return std::nullopt;
                return it->get<double>();
            };
            const auto oa = rated(orig), sa = rated(cand);
            if (!oa || !sa) {
                Result r;
                r.note = std::string(q.what) + ": the " +
                         (!oa && !sa ? "original and the substitute state"
                                     : !oa ? "original states" : "substitute states") +
                         " no per-contact rated current, so the derating fraction has no "
                         "amperes to scale — not compared; confirm from the datasheet";
                out.push_back({q.row, r});
            } else {
                auto in_amps = [](SpecCurve c, double rated) {
                    std::vector<double> y = c.curve.y;
                    for (double& v : y) v *= rated;
                    c.curve = Curve(c.curve.x, std::move(y));
                    return c;
                };
                out.push_back({q.row, compare_pair(in_amps(*view_curve(orig, key), *oa),
                                                   in_amps(*view_curve(cand, key), *sa), q)});
            }
        }
    }
    return out;
}

}  // namespace kelvin::crossref::curve_checks
