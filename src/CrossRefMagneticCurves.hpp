// Magnetic curves compared curve-vs-curve (owner rule 2026-11-14: "make sure that Kelvin
// compares ALL existing graphs"). The comparison itself is the shared helper
// (CrossRefCurves.hpp: read only inside both measured ranges, the scalar spec's bands at every
// point, the worst point decides); this header says, for each magnetic curve the catalogue
// carries, which spec-block key holds it, which scalar spec's bands judge it, and on which axes
// it is read, and writes the note.
//
// A curve travels in the spec block as {"x": [...], "y": [...]} (web/src/crossref.js copies
// it from the browse row, src/Browse.hpp extra_row_json), optionally with "mode" (a
// common-mode choke's impedance curve is its COMMON-MODE |Z|: "common"). A curve stated on one
// side only is not a comparison and yields nothing here (the scalar rows still stand); with a
// curve on both sides the result is PASS / WARN / FAIL at the worst point, or UNVERIFIED with a
// note when the curves do not overlap or were measured under different conditions (no cap).
// Program-only, no I/O.
#pragma once
#include <cmath>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "CrossRefCurves.hpp"
#include "CrossRefParams.hpp"

namespace kelvin::crossref::magcurve {

// How a curve is read between its samples (CrossRefCurves.hpp offers linear and log-log;
// semi-log-x is linear interpolation in log(x), done by handing the helper log(x)).
//   LogLog   — |Z|(f): positive on both axes, drawn log-log.
//   SemiLogX — frequency on a log axis, the quantity linear: R(f) and X(f) (R reaches 0 and X
//              changes sign, so they cannot be read on log axes) and insertion loss in dB.
//   Linear   — linear axes: L(I), I(dT).
enum class Axis { LogLog, SemiLogX, Linear };

// "1.23 MHz" / "45.6 ohm": engineering prefix, three significant digits.
inline std::string fmt(double v, const char* unit) {
    static const struct { double f; const char* p; } kP[] = {
        {1e9, "G"}, {1e6, "M"}, {1e3, "k"}, {1, ""}, {1e-3, "m"}, {1e-6, "u"}, {1e-9, "n"}};
    char b[64];
    const double a = std::fabs(v);
    const std::string u = unit;
    if (a == 0 || !std::isfinite(v) || u == "dB" || u == "K" || u == "degC") {
        std::snprintf(b, sizeof b, "%.3g %s", v, unit);
        return b;
    }
    for (const auto& p : kP)
        if (a >= p.f * 0.9995) {
            std::snprintf(b, sizeof b, "%.3g %s%s", v / p.f, p.p, unit);
            return b;
        }
    std::snprintf(b, sizeof b, "%.3g %s", v, unit);
    return b;
}

// The curve under `key` in a spec block (the shared spec shape, curve_from_spec: {x, y,
// temperature?, dc_bias?, frequency?}, plus "mode" on a common-mode choke's |Z|), nullopt when
// absent / null. A present but malformed curve THROWS (the browse row builds it; a bad one is a
// bug, not missing data).
inline std::optional<SpecCurve> read_curve(const nlohmann::json& s, const char* key,
                                           std::string* mode = nullptr) {
    if (!s.is_object()) return std::nullopt;
    auto c = curve_from_spec(s, key);
    if (c && mode) *mode = s.at(key).value("mode", std::string());
    return c;
}

struct Def {
    const char* key;    // spec-block key
    const char* name;   // the param row's name
    const char* label;  // for the note
    ParamSpec spec;
    const char* spec_name;  // the scalar spec whose bands judge it (named in the note)
    Axis axis;
    const char* xunit;
    const char* yunit;
    bool inductive_only = false;  // X(f): judged where both curves are inductive (X > 0)
};

// The scalar spec `key` of category `cat`'s PARAM_SPECS table; throws when absent (a curve's
// bands are borrowed from a spec that exists, never invented here).
inline const ParamSpec& scalar_spec(const std::string& cat, const std::string& key) {
    for (const ParamSpec& s : params_for(cat))
        if (s.key == key) return s;
    throw std::logic_error("magcurve: no scalar spec " + cat + "." + key);
}

// Differential-mode curves of a common-mode choke. The DM path is the path the wanted signal
// or supply current takes, so a DM impedance or insertion loss that RISES is the regression
// (a signal CMC's datasheet bounds its DM insertion loss from above). No scalar spec states a
// DM quantity:
//   * DM |Z|(f) takes the DCR spec's bands (Lower, 1.3x): the series impedance in the wanted
//     path, of which the DCR is the DC end.
//   * DM insertion loss takes the one dB spec Kelvin has, the op-amp CMRR's 6 dB additive
//     band, with the direction of a loss in the wanted path (Lower).
// Both directions are owner-to-confirm (a power-line CMC may use its leakage as a DM filter).
inline ParamSpec dm_impedance_spec() {
    ParamSpec p = scalar_spec("magnetic", "dcr");
    p.key = "dm_impedance_curve";
    p.exclude_missing_sub = false;
    return p;
}
inline ParamSpec dm_insertion_loss_spec() {
    ParamSpec p = scalar_spec("analog", "cmrr_db");
    p.key = "dm_insertion_loss_curve";
    p.dir = Dir::Lower;
    return p;
}
// The bead impedance spec (chipBead impedance_100mhz: Higher, PASS down to 0.85x, WARN to
// 0.8x), without its exclude-if-missing flag: a curve on one side only yields no row at all.
inline ParamSpec impedance_spec(const char* key) {
    ParamSpec p = scalar_spec("chipBead", "impedance_100mhz");
    p.key = key;
    p.exclude_missing_sub = false;
    return p;
}

inline const std::vector<Def>& defs() {
    static const std::vector<Def> d = {
        // |Z|(f): an inductor's, a bead's, a cable core's; a CMC's COMMON-MODE |Z| (mode
        // "common"). Bead impedance bands — the one impedance spec Kelvin has.
        {"impedance_curve", "impedance_curve", "|Z|(f)", impedance_spec("impedance_curve"),
         "the chip-bead impedance spec (PASS >= 0.85x, WARN >= 0.8x)", Axis::LogLog, "Hz",
         "ohm"},
        // R(f): the dissipative part of a bead's impedance — what actually turns the noise
        // into heat. Bead impedance bands; linear ohms over log frequency (R reaches 0 at low
        // frequency in 68 bead records, which a log axis cannot read).
        {"resistance_curve", "resistance_curve", "R(f)", impedance_spec("resistance_curve"),
         "the chip-bead impedance spec (PASS >= 0.85x, WARN >= 0.8x)", Axis::SemiLogX, "Hz",
         "ohm"},
        // X(f): the reactive part, inductive (X > 0) below the bead's resonance and capacitive
        // above it. The bead impedance bands are ratios, which a sign change makes meaningless,
        // so X is judged on the inductive band both curves state (each curve's leading run of
        // X > 0); the capacitive region is not judged (owner to decide how).
        {"reactance_curve", "reactance_curve", "X(f)", impedance_spec("reactance_curve"),
         "the chip-bead impedance spec (PASS >= 0.85x, WARN >= 0.8x)", Axis::SemiLogX, "Hz",
         "ohm", true},
    };
    return d;
}

struct Result {
    std::string name;
    const char* verdict = UNVERIFIED;
    std::optional<double> warn_depth;
    std::string note;
    Dir dir = Dir::Higher;
    std::optional<double> orig_y, sub_y;  // at the worst point (unset when UNVERIFIED)
};

// The leading inductive run of a reactance curve (X > 0 from its first sample on); nullopt
// when fewer than two samples are inductive.
inline std::optional<Curve> inductive_part(const Curve& c) {
    std::vector<double> x, y;
    for (std::size_t i = 0; i < c.x.size() && c.y[i] > 0; ++i) {
        x.push_back(c.x[i]);
        y.push_back(c.y[i]);
    }
    if (x.size() < 2) return std::nullopt;
    return Curve(std::move(x), std::move(y));
}

inline Curve log_x(const Curve& c) {
    std::vector<double> x;
    for (double v : c.x) {
        if (!(v > 0)) throw std::invalid_argument("magcurve: log axis over a non-positive x");
        x.push_back(std::log(v));
    }
    return Curve(std::move(x), c.y);
}

// One curve pair, judged under `d`.
inline Result compare_pair(const Def& d, const Curve& orig, const Curve& sub) {
    Result r;
    r.name = d.name;
    r.dir = d.spec.dir;
    const char* axis_word = d.axis == Axis::LogLog     ? "log-log"
                            : d.axis == Axis::SemiLogX ? "linear in log-frequency"
                                                       : "linear";
    auto range = [&](const Curve& c) { return fmt(c.x.front(), d.xunit) + "-" +
                                              fmt(c.x.back(), d.xunit); };
    std::optional<Curve> o = orig, s = sub;
    if (d.inductive_only) {
        o = inductive_part(orig);
        s = inductive_part(sub);
        if (!o || !s) {
            r.note = std::string(d.label) + ": " +
                     (!o ? "the original's" : "the substitute's") +
                     " curve is not inductive (X > 0) over two samples, so the inductive band "
                     "was not compared";
            return r;
        }
    }
    const bool semilog = d.axis == Axis::SemiLogX;
    const Curve oc = semilog ? log_x(*o) : *o, sc = semilog ? log_x(*s) : *s;
    const CurveVerdict v = compare_curves(oc, sc, d.spec,
                                          d.axis == Axis::LogLog ? Interp::LogLog : Interp::Linear);
    if (v.verdict == std::string(UNVERIFIED)) {
        r.note = std::string(d.label) + " not compared: the two curves do not overlap (original " +
                 range(*o) + ", substitute " + range(*s) + ") — nothing is extrapolated";
        return r;
    }
    const double wx = semilog ? std::exp(v.worst_x) : v.worst_x;
    const double lo = semilog ? std::exp(v.range->first) : v.range->first;
    const double hi = semilog ? std::exp(v.range->second) : v.range->second;
    r.verdict = v.verdict;
    r.warn_depth = v.warn_depth;
    r.orig_y = v.orig_y;
    r.sub_y = v.sub_y;
    const std::string what = v.verdict == std::string(PASS)   ? "level or ahead everywhere"
                             : v.verdict == std::string(WARN) ? "behind"
                                                              : "well behind";
    char pts[32];
    std::snprintf(pts, sizeof pts, "%zu", v.points);
    r.note = std::string(d.label) + " compared on both curves over " + fmt(lo, d.xunit) + "-" +
             fmt(hi, d.xunit) + " (" + pts + " points, " + axis_word + ", " + d.spec_name +
             (d.inductive_only ? ", inductive band X > 0 only" : "") + "): " + what +
             " — worst at " + fmt(wx, d.xunit) + ", substitute " + fmt(v.sub_y, d.yunit) +
             " vs original " + fmt(v.orig_y, d.yunit);
    return r;
}

// Every curve both records carry, compared (see the header comment).
inline std::vector<Result> compare_all(const nlohmann::json& orig, const nlohmann::json& sub) {
    std::vector<Result> out;
    for (const Def& d : defs()) {
        std::string om, sm;
        const auto o = read_curve(orig, d.key, &om), s = read_curve(sub, d.key, &sm);
        if (!o || !s) continue;
        if (om != sm) {
            Result r;
            r.name = d.name;
            r.dir = d.spec.dir;
            auto m = [](const std::string& x) {
                return x.empty() ? std::string("unstated mode") : x + "-mode";
            };
            r.note = std::string(d.label) + " not compared: the curves were measured in "
                     "different winding modes (original " + m(om) + ", substitute " + m(sm) +
                     ")";
            out.push_back(std::move(r));
            continue;
        }
        out.push_back(compare_pair(d, o->curve, s->curve));
    }
    return out;
}

}  // namespace kelvin::crossref::magcurve
