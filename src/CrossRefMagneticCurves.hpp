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
#include <algorithm>
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
    // Impedance-type curves (owner 2026-11-14) are judged only in the original's useful band;
    // `peak_key` names the original's curve whose impedance peak closes it.
    const char* peak_key = "impedance_curve";
};

// The scalar spec `key` of category `cat`'s PARAM_SPECS table; throws when absent (a curve's
// bands are borrowed from a spec that exists, never invented here).
inline const ParamSpec& scalar_spec(const std::string& cat, const std::string& key) {
    for (const ParamSpec& s : params_for(cat))
        if (s.key == key) return s;
    throw std::logic_error("magcurve: no scalar spec " + cat + "." + key);
}

// The curves of a common-mode choke beyond its CM |Z|. Owner 2026-11-14: "CMC differential-mode
// impedance and insertion loss: lower is better (signal-line selection practice), with a
// note" — the DM path carries the wanted signal or supply current, so a DM impedance or
// insertion loss that RISES is the regression (a signal CMC's datasheet bounds its DM
// insertion loss from above); a power-line CMC that relies on its leakage as a DM filter is
// the exception the note points at. CM impedance and CM insertion loss: higher is better (the
// noise the choke is there to block). No scalar spec states a DM quantity or an insertion
// loss; the closest are named:
//   * DM |Z|(f) takes the DCR spec's bands (Lower, PASS <= 1x, WARN to 1.3x): the series
//     impedance in the wanted path, of which the DCR is the DC end.
//   * insertion loss takes the one dB spec Kelvin has, the op-amp CMRR's 6 dB additive band:
//     CM as it is (Higher, a rejection), DM with the direction of a loss in the wanted path
//     (Lower).
inline ParamSpec dm_impedance_spec() {
    ParamSpec p = scalar_spec("magnetic", "dcr");
    p.key = "dm_impedance_curve";
    p.exclude_missing_sub = false;
    return p;
}
// Owner 2026-11-14: an insertion-loss curve PASSES within 1 dB of the original's (the wrong
// way), then the CMRR spec's 6 dB band is the WARN band, then FAIL.
inline constexpr double kInsertionLossPassDb = 1.0;
inline ParamSpec dm_insertion_loss_spec() {
    ParamSpec p = scalar_spec("analog", "cmrr_db");
    p.key = "dm_insertion_loss_curve";
    p.dir = Dir::Lower;
    p.pass_abs = kInsertionLossPassDb;
    return p;
}
inline ParamSpec cm_insertion_loss_spec() {
    ParamSpec p = scalar_spec("analog", "cmrr_db");
    p.key = "cm_insertion_loss_curve";
    p.pass_abs = kInsertionLossPassDb;
    return p;
}
// Said with every DM curve verdict (owner 2026-11-14).
inline constexpr const char* kDmLowerNote =
    "; lower DM impedance / insertion loss is ranked better (signal-line selection practice: "
    "the DM path carries the wanted signal) — a power-line choke relying on its DM leakage as "
    "a filter needs the opposite, check the application";
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
        // A CMC's differential-mode |Z|(f), its band closed by its own DM resonance.
        {"dm_impedance_curve", "dm_impedance_curve", "DM |Z|(f)", dm_impedance_spec(),
         "the DCR spec (lower is better: PASS <= 1x, WARN <= 1.3x)", Axis::LogLog, "Hz", "ohm",
         false, "dm_impedance_curve"},
        // Insertion loss in dB (50 ohm), linear dB over log frequency; each mode's band closed
        // by the original's |Z| peak of that mode.
        {"cm_insertion_loss_curve", "cm_insertion_loss_curve", "CM insertion loss(f)",
         cm_insertion_loss_spec(),
         "PASS within 1 dB (owner 2026-11-14), then the CMRR spec's 6 dB band as WARN, higher is "
         "better",
         Axis::SemiLogX, "Hz", "dB", false, "impedance_curve"},
        {"dm_insertion_loss_curve", "dm_insertion_loss_curve", "DM insertion loss(f)",
         dm_insertion_loss_spec(),
         "PASS within 1 dB (owner 2026-11-14), then the CMRR spec's 6 dB band as WARN, lower is "
         "better",
         Axis::SemiLogX, "Hz", "dB", false, "dm_impedance_curve"},
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

// The useful band of an impedance-type curve (owner 2026-11-14): "from the original's stated
// rating frequency (or 1 MHz if none) up to the original's self-resonance / impedance peak;
// points outside are shown in the note range but not graded". No magnetic record states a
// rating frequency (data-524a699 carries none: the only frequency fields are the curve samples,
// selfResonantFrequency and cutOffFrequency), so the band opens at 1 MHz (150 kHz for a common-mode choke, owner 2026-11-14). It closes at:
//   1. the original's stated SRF (the spec's srf; not for a CMC's differential-mode curves,
//      whose resonance the CMC's one SRF does not describe), else
//   2. the original's impedance peak, read off its |Z|(f) of the same mode — or, for a bead
//      that states R(f) and X(f) instead, off |R + jX| at the frequencies both state:
//        a bracketed maximum (the curve falls on both sides) -> that frequency;
//        a maximum at the last sample (still rising)          -> the end of the sweep, the
//                                                                resonance lies beyond it;
//        a maximum at the first sample (falling throughout)   -> the first sample: the
//                                                                resonance lies below the sweep
//                                                                and nothing is in band.
// nullopt (with `why` saying so) when the original carries nothing that locates the peak.
struct Band {
    double lo = 1e6, hi = 0;
    std::string lo_why = "1 MHz (no rating frequency stated)";
    std::string hi_why;
};
// Owner 2026-11-14: a common-mode choke's band opens at 150 kHz, the bottom of the CISPR
// conducted-emission range it is there to filter; beads and inductors keep 1 MHz.
inline constexpr double kCmcBandLo = 150e3;
inline bool is_cmc(const nlohmann::json& s) {
    if (!s.is_object()) return false;
    const auto t = s.find("device_type");
    return t != s.end() && t->is_string() && t->get<std::string>() == "commonModeChoke";
}
inline std::optional<Curve> impedance_magnitude(const nlohmann::json& s, const char* peak_key) {
    if (const auto z = curve_from_spec(s, peak_key)) return z->curve;
    if (std::string(peak_key) != "impedance_curve") return std::nullopt;
    const auto r = curve_from_spec(s, "resistance_curve"), x = curve_from_spec(s, "reactance_curve");
    if (!r || !x) return std::nullopt;
    std::vector<double> f, m;
    for (std::size_t i = 0, k = 0; i < r->curve.x.size(); ++i) {
        while (k < x->curve.x.size() && x->curve.x[k] < r->curve.x[i]) ++k;
        if (k < x->curve.x.size() && x->curve.x[k] == r->curve.x[i]) {
            f.push_back(r->curve.x[i]);
            m.push_back(std::hypot(r->curve.y[i], x->curve.y[k]));
        }
    }
    if (f.size() < 2) return std::nullopt;
    return Curve(std::move(f), std::move(m));
}
inline std::optional<Band> useful_band(const nlohmann::json& orig, const Def& d,
                                       std::string& why_none) {
    Band b;
    if (is_cmc(orig)) {
        b.lo = kCmcBandLo;
        b.lo_why = "150 kHz (a common-mode choke: the bottom of the CISPR conducted range)";
    }
    const bool dm = std::string(d.peak_key) != "impedance_curve";
    if (!dm && orig.is_object()) {
        const auto it = orig.find("srf");
        if (it != orig.end() && it->is_number() && it->get<double>() > 0) {
            b.hi = it->get<double>();
            b.hi_why = "the original's stated SRF";
            return b;
        }
    }
    const auto z = impedance_magnitude(orig, d.peak_key);
    if (!z) {
        why_none = "the original states no SRF and no impedance curve to locate its peak, so "
                   "its useful band is unknown";
        return std::nullopt;
    }
    const std::string src = std::string(d.peak_key) == "impedance_curve"
                                ? (curve_from_spec(orig, d.peak_key) ? "|Z|" : "|R + jX|")
                                : "differential-mode |Z|";
    std::size_t first = 0, last = 0;
    for (std::size_t k = 1; k < z->y.size(); ++k) {
        if (z->y[k] > z->y[first]) first = last = k;
        else if (z->y[k] == z->y[first]) last = k;
    }
    if (first > 0 && last + 1 < z->y.size()) {
        b.hi = z->x[first];
        b.hi_why = "the original's " + src + " peak";
    } else if (first > 0) {
        b.hi = z->x.back();
        b.hi_why = "the end of the original's " + src + " sweep (still rising: its resonance "
                   "lies beyond it)";
    } else {
        b.hi = z->x.front();
        b.hi_why = "the start of the original's " + src + " sweep (falling throughout: its "
                   "resonance lies below it)";
    }
    return b;
}

// One curve pair, judged under `d` inside `band` (nullopt: the whole overlap).
inline Result compare_pair(const Def& d, const Curve& orig, const Curve& sub,
                           const std::optional<Band>& band = std::nullopt) {
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
    const auto ov = overlap(*o, *s);
    if (!ov) {
        r.note = std::string(d.label) + " not compared: the two curves do not overlap (original " +
                 range(*o) + ", substitute " + range(*s) + ") — nothing is extrapolated";
        return r;
    }
    const bool semilog = d.axis == Axis::SemiLogX;
    const Interp interp = d.axis == Axis::LogLog ? Interp::LogLog : Interp::Linear;
    const Curve oc0 = semilog ? log_x(*o) : *o, sc0 = semilog ? log_x(*s) : *s;
    const Curve* oc = &oc0;
    const Curve* sc = &sc0;
    std::string band_txt;
    std::optional<std::pair<double, double>> graded;  // the band, on the helper's x axis
    if (band) {
        band_txt = "; graded only in the useful band " + fmt(band->lo, d.xunit) + "-" +
                   fmt(band->hi, d.xunit) + ", from " + band->lo_why + " to " + band->hi_why +
                   ", of the measured overlap " + fmt(ov->first, d.xunit) + "-" +
                   fmt(ov->second, d.xunit);
        const double lo = semilog ? std::log(band->lo) : band->lo;
        const double hi = semilog ? std::log(band->hi) : band->hi;
        const auto ovc = overlap(*oc, *sc);
        // Graded only when the band and the measured overlap share a stretch (two points at
        // least); a band closed below its own opening (resonance under 1 MHz) holds nothing.
        if (!(hi > lo) || !ovc || !(std::min(ovc->second, hi) > std::max(ovc->first, lo))) {
            r.note = std::string(d.label) + " not graded: the measured overlap " +
                     fmt(ov->first, d.xunit) + "-" + fmt(ov->second, d.xunit) +
                     " holds no two points inside the useful band " + fmt(band->lo, d.xunit) +
                     "-" + fmt(band->hi, d.xunit) + " (from " + band->lo_why + " to " +
                     band->hi_why + ")";
            return r;
        }
        graded = std::make_pair(lo, hi);
    }
    const CurveVerdict v = compare_curves(*oc, *sc, d.spec, interp, graded);
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
    const std::string what = v.verdict == std::string(PASS)   ? "inside the PASS band everywhere"
                             : v.verdict == std::string(WARN) ? "behind"
                                                              : "well behind";
    char pts[32];
    std::snprintf(pts, sizeof pts, "%zu", v.points);
    r.note = std::string(d.label) + " compared on both curves over " + fmt(lo, d.xunit) + "-" +
             fmt(hi, d.xunit) + " (" + pts + " points, " + axis_word + ", " + d.spec_name +
             (d.inductive_only ? ", inductive band X > 0 only" : "") + band_txt + "): " + what +
             " — worst at " + fmt(wx, d.xunit) + ", substitute " + fmt(v.sub_y, d.yunit) +
             " vs original " + fmt(v.orig_y, d.yunit) +
             (std::string(d.peak_key) == "dm_impedance_curve" ? kDmLowerNote : "");
    return r;
}

// ── L(I): inductance vs DC bias, per temperature (owner 2026-11-14) ─────────────────────
// MAS inductancePoints state L(I) optionally per temperature; the spec block carries every
// curve the record states as "inductance_curves" (the shared curve shape, a list: x = A,
// y = H, temperature when stated). Two records' curves are compared at each temperature BOTH
// state — 20 and 25 degC count as the same room temperature (owner 2026-11-14,
// same_room_temperature) when a record has no curve at the other's exact temperature; an
// unstated temperature matches only an unstated one (owner 2026-11-14: a condition stated by
// one side only is a different condition). Read directly in henries on the current
// range both measured, linear axes, never extrapolated; a curve with no 0 A point is compared
// like any other (no L(0) is needed to compare two L(I) curves). Bands: the inductance spec's
// lower side (primary_value_spec "magnetic", Higher — more inductance held under bias is
// never a regression): PASS down to the original's stated tolerance floor (owner 2026-11-13
// R1) or 0.85x when it states none, WARN down to 0.8x, FAIL below. The worst point over every
// common temperature decides. No common temperature -> UNVERIFIED + note, no cap.
inline ParamSpec inductance_curve_spec(const nlohmann::json& orig) {
    PrimaryValueSpec pv{};
    if (!primary_value_spec("magnetic", pv))
        throw std::logic_error("magcurve: no magnetic primary value spec");
    double pass = pv.tight_lo;
    if (orig.is_object()) {
        const auto t = orig.find("tolerance_minus_pct");
        if (t != orig.end() && t->is_number() && t->get<double>() >= 0)
            pass = std::max(pv.accept_lo, 1.0 - t->get<double>() / 100.0);
    }
    return ParamSpec{"inductance_curve", Dir::Higher, pv.accept_lo, std::nullopt, false, false,
                     nullptr, pass};
}
inline std::string temp_word(const std::optional<double>& t) {
    if (!t) return "no stated temperature";
    char b[32];
    std::snprintf(b, sizeof b, "%g degC", *t);
    return b;
}
inline std::optional<Result> compare_inductance_curves(const nlohmann::json& orig,
                                                       const nlohmann::json& sub) {
    if (!orig.is_object() || !sub.is_object()) return std::nullopt;
    const auto oc = curves_from_spec(orig, "inductance_curves");
    const auto sc = curves_from_spec(sub, "inductance_curves");
    if (oc.empty() || sc.empty()) return std::nullopt;
    const ParamSpec spec = inductance_curve_spec(orig);
    Result r;
    r.name = "inductance_curve";
    r.dir = Dir::Higher;
    auto temps = [](const std::vector<SpecCurve>& v) {
        std::string s;
        for (const auto& c : v) s += (s.empty() ? "" : ", ") + temp_word(c.temperature);
        return s;
    };
    auto rank = [](const char* v) {
        const std::string s = v;
        return s == FAIL ? 3 : s == WARN ? 2 : 1;
    };
    int best = 0;
    double best_ratio = HUGE_VAL;
    CurveVerdict best_v;
    std::string compared;
    std::size_t pairs = 0;
    // The substitute curves an original curve is read against: those at the SAME stated
    // temperature, or — only when there is none — those at the other room temperature
    // (20 / 25 degC, owner 2026-11-14), so a record stating both 20 and 25 degC curves is
    // never read across them.
    auto exact = [](const SpecCurve& o, const SpecCurve& s) {
        if (o.temperature.has_value() != s.temperature.has_value()) return false;
        return !o.temperature || same_stated_value(*o.temperature, *s.temperature);
    };
    auto room = [](const SpecCurve& o, const SpecCurve& s) {
        return o.temperature && s.temperature &&
               same_room_temperature(*o.temperature, *s.temperature);
    };
    auto pair_word = [](const SpecCurve& o, const SpecCurve& s) {
        if (!o.temperature || same_stated_value(*o.temperature, *s.temperature))
            return temp_word(o.temperature);
        return temp_word(o.temperature) + " against " + temp_word(s.temperature) +
               " (both room temperature)";
    };
    std::string best_word;
    for (const auto& o : oc) {
        bool any_exact = false;
        for (const auto& s : sc) any_exact = any_exact || exact(o, s);
        for (const auto& s : sc) {
            if (!(any_exact ? exact(o, s) : room(o, s))) continue;
            ++pairs;
            const CurveVerdict v = compare_curves(o.curve, s.curve, spec, Interp::Linear);
            if (v.verdict == std::string(UNVERIFIED)) continue;
            compared += (compared.empty() ? "" : ", ") + pair_word(o, s);
            const double ratio = v.orig_y > 0 ? v.sub_y / v.orig_y : HUGE_VAL;
            const int rk = rank(v.verdict);
            if (rk > best || (rk == best && ratio < best_ratio)) {
                best = rk;
                best_ratio = ratio;
                best_word = pair_word(o, s);
                best_v = v;
            }
        }
    }
    if (pairs == 0) {
        r.note = "L(I) curves not compared: stated at different temperatures (original " +
                 temps(oc) + "; substitute " + temps(sc) + ") — a curve is not read across "
                 "temperatures";
        return r;
    }
    if (best == 0) {
        r.note = "L(I) curves not compared: at the common temperature(s) the two curves cover "
                 "no common current range — nothing is extrapolated";
        return r;
    }
    r.verdict = best_v.verdict;
    r.warn_depth = best_v.warn_depth;
    r.orig_y = best_v.orig_y;
    r.sub_y = best_v.sub_y;
    char band[96];
    std::snprintf(band, sizeof band, "PASS >= %.3gx, WARN >= %.3gx", *spec.pass_factor,
                  spec.tol_factor);
    const std::string what = r.verdict == std::string(PASS)   ? "inside the PASS band everywhere"
                             : r.verdict == std::string(WARN) ? "holds less inductance"
                                                              : "holds far less inductance";
    r.note = "L(I) compared on both curves at " + compared + " (linear, " +
             fmt(best_v.range->first, "A") + "-" + fmt(best_v.range->second, "A") +
             " at the deciding temperature, the inductance spec's lower side: " + band + "): " +
             what + " — worst at " + fmt(best_v.worst_x, "A") + ", " + best_word +
             ", substitute " + fmt(best_v.sub_y, "H") + " vs original " +
             fmt(best_v.orig_y, "H");
    return r;
}

// ── rated current at a common temperature rise (owner 2026-11-14, item 7) ────────────────
// MAS ratedCurrentPoints qualify each rated current by the self-heating criterion it was rated
// at (temperatureRise, K) and optionally the ambient (temperature, degC); the spec block
// carries them as "rated_current_points" in the shared curve shape {x: [K], y: [A],
// temperature?} — possibly ONE point (a single rating at its rise). The scalar rated_current
// is the LARGEST stated current, so a 20 K and a 40 K rating were compared as equals. With
// points on both sides the rating is compared at a COMMON rise instead: every rise either
// record states that lies inside both records' stated rises, each side read there exactly or
// linearly between its two bracketing rises (never extrapolated), judged with the
// rated_current spec; the worst rise decides. Ambient: 20 and 25 degC are the same room
// temperature (owner 2026-11-14, same_room_temperature); an ambient stated by one side only,
// or two different ones, is a different condition -> UNVERIFIED + note, no cap; so is a pair
// with no common rise.
struct RatedPoints {
    std::vector<double> rise, current;
    std::optional<double> ambient;
};
inline std::optional<RatedPoints> read_rated_points(const nlohmann::json& s) {
    if (!s.is_object()) return std::nullopt;
    const auto it = s.find("rated_current_points");
    if (it == s.end() || it->is_null()) return std::nullopt;
    if (!it->is_object() || !it->contains("x") || !it->contains("y"))
        throw std::invalid_argument("rated_current_points: not {x, y}");
    RatedPoints p;
    p.rise = it->at("x").get<std::vector<double>>();
    p.current = it->at("y").get<std::vector<double>>();
    if (p.rise.empty() || p.rise.size() != p.current.size())
        throw std::invalid_argument("rated_current_points: x and y empty or of different length");
    for (std::size_t i = 1; i < p.rise.size(); ++i)
        if (!(p.rise[i] > p.rise[i - 1]))
            throw std::invalid_argument("rated_current_points: rises not strictly increasing");
    if (const auto t = it->find("temperature"); t != it->end() && !t->is_null())
        p.ambient = t->get<double>();
    return p;
}
// The current at `rise`: the stated one, or linear between the two bracketing rises; nullopt
// outside the stated rises (never extrapolated).
inline std::optional<double> rated_at(const RatedPoints& p, double rise) {
    for (std::size_t i = 0; i < p.rise.size(); ++i) {
        if (same_stated_value(p.rise[i], rise)) return p.current[i];
        if (i > 0 && p.rise[i - 1] < rise && rise < p.rise[i])
            return p.current[i - 1] + (rise - p.rise[i - 1]) / (p.rise[i] - p.rise[i - 1]) *
                                          (p.current[i] - p.current[i - 1]);
    }
    return std::nullopt;
}
struct RatedBasis {
    bool applies = false;  // both records state rated-current points
    std::optional<double> orig_at, sub_at;  // at the deciding common rise
    ParamOutcome outcome{UNVERIFIED};
    std::string note;  // empty when the comparison needs no explanation
};
inline RatedBasis rated_at_common_rise(const nlohmann::json& orig, const nlohmann::json& sub,
                                       const ParamSpec& spec) {
    RatedBasis b;
    const auto o = read_rated_points(orig), s = read_rated_points(sub);
    if (!o || !s) return b;
    b.applies = true;
    auto rises = [](const RatedPoints& p) {
        std::string out;
        for (std::size_t i = 0; i < p.rise.size(); ++i)
            out += (out.empty() ? "" : ", ") + fmt(p.current[i], "A") + " at " +
                   fmt(p.rise[i], "K");
        return out;
    };
    auto amb = [](const std::optional<double>& t) {
        return t ? fmt(*t, "degC") + " ambient" : std::string("no stated ambient");
    };
    if (o->ambient.has_value() != s->ambient.has_value() ||
        (o->ambient && !same_room_temperature(*o->ambient, *s->ambient))) {
        b.note = "rated current not compared: rated under different ambient conditions "
                 "(original " + amb(o->ambient) + ", substitute " + amb(s->ambient) +
                 ") — confirm the substitute's rating at the original's ambient from its "
                 "datasheet";
        return b;
    }
    std::vector<double> common;
    for (const RatedPoints* p : {&*o, &*s})
        for (double r : p->rise)
            if (rated_at(*o, r) && rated_at(*s, r)) common.push_back(r);
    if (common.empty()) {
        b.note = "rated current not compared: rated at different temperature rises (original " +
                 rises(*o) + "; substitute " + rises(*s) +
                 ") — a self-heating rating is only comparable at the same rise, and nothing is "
                 "extrapolated";
        return b;
    }
    auto rank = [](const char* v) {
        const std::string x = v;
        return x == FAIL ? 2 : x == WARN ? 1 : 0;
    };
    int best = -1;
    double best_ratio = HUGE_VAL, best_rise = 0;
    for (double r : common) {
        const double oa = *rated_at(*o, r), sa = *rated_at(*s, r);
        const ParamOutcome out = compare_numeric(spec, oa, sa);
        const double ratio = oa > 0 ? sa / oa : HUGE_VAL;
        if (rank(out.verdict) > best || (rank(out.verdict) == best && ratio < best_ratio)) {
            best = rank(out.verdict);
            best_ratio = ratio;
            best_rise = r;
            b.outcome = out;
            b.orig_at = oa;
            b.sub_at = sa;
        }
    }
    // Said only when it changes what is compared: several rises, different ones, or a
    // room-temperature pair (20 against 25 degC).
    const bool room_pair = o->ambient && !same_stated_value(*o->ambient, *s->ambient);
    if (o->rise.size() > 1 || s->rise.size() > 1 ||
        !same_stated_value(o->rise.front(), s->rise.front()) || room_pair)
        b.note = "rated current compared at a common temperature rise: at " +
                 fmt(best_rise, "K") + " the substitute carries " + fmt(*b.sub_at, "A") +
                 " against the original's " + fmt(*b.orig_at, "A") + " (original " +
                 rises(*o) + "; substitute " + rises(*s) +
                 (room_pair ? "; " + amb(o->ambient) + " against " + amb(s->ambient) +
                                  ", both room temperature"
                            : std::string()) +
                 ")";
    return b;
}

// Every curve both records carry, compared (see the header comment).
inline std::vector<Result> compare_all(const nlohmann::json& orig, const nlohmann::json& sub) {
    std::vector<Result> out;
    for (const Def& d : defs()) {
        std::string om, sm;
        const auto o = read_curve(orig, d.key, &om), s = read_curve(sub, d.key, &sm);
        if (!o || !s) continue;
        Result r;
        r.name = d.name;
        r.dir = d.spec.dir;
        if (om != sm) {
            auto m = [](const std::string& x) {
                return x.empty() ? std::string("unstated mode") : x + "-mode";
            };
            r.note = std::string(d.label) + " not compared: the curves were measured in "
                     "different winding modes (original " + m(om) + ", substitute " + m(sm) +
                     ")";
            out.push_back(std::move(r));
            continue;
        }
        std::optional<Band> band;
        if (d.peak_key) {
            std::string why;
            band = useful_band(orig, d, why);
            if (!band) {
                r.note = std::string(d.label) + " not graded: " + why;
                out.push_back(std::move(r));
                continue;
            }
        }
        out.push_back(compare_pair(d, o->curve, s->curve, band));
    }
    if (auto l = compare_inductance_curves(orig, sub)) out.push_back(std::move(*l));
    return out;
}

}  // namespace kelvin::crossref::magcurve
