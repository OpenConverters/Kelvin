#include "Views.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

#include "CrossRefCurves.hpp"      // crossref::curve_at (unit check of unlabelled curves)
#include "CrossRefDimensions.hpp"  // crossref::normalize_mount (smd / leaded)
#include "DimensionJson.hpp"       // PEAS::resolve_dimensional_values (json overload)

namespace kelvin {
namespace {

// ---- small json readers mirroring Python's permissive dict access ----------
// nlohmann: a missing key or a null yields a null json; is_number() excludes booleans
// (Python's isinstance(x,(int,float)) would accept bool, but the numeric TAS fields never
// carry booleans, so this matches in practice).

const json* obj_get(const json& o, const char* key) {
    if (!o.is_object()) return nullptr;
    auto it = o.find(key);
    if (it == o.end() || it->is_null()) return nullptr;
    return &(*it);
}

std::optional<std::string> get_str(const json& o, const char* key) {
    const json* v = obj_get(o, key);
    if (v && v->is_string()) return v->get<std::string>();
    return std::nullopt;
}

std::optional<double> get_num(const json& o, const char* key) {
    const json* v = obj_get(o, key);
    if (v && v->is_number() && !v->is_boolean()) return v->get<double>();
    return std::nullopt;
}

bool get_bool(const json& o, const char* key, bool dflt) {
    const json* v = obj_get(o, key);
    if (v && v->is_boolean()) return v->get<bool>();
    return dflt;
}

// ---- measured curves (owner rule 2026-11-14: Kelvin compares ALL existing graphs) ----------
// SI scale of the unit a curve label states in brackets ("Gate charge [nC]" -> 1e-9). No label,
// or no bracket, is the schema's SI. A unit not in this table is a record Kelvin cannot read
// without guessing, so it throws with the part and the label.
double curve_label_scale(const std::string& label, const std::string& who) {
    const auto lb = label.rfind('['), rb = label.rfind(']');
    if (lb == std::string::npos || rb == std::string::npos || rb < lb) return 1.0;
    const std::string u = label.substr(lb + 1, rb - lb - 1);
    static const std::unordered_map<std::string, double> k = {
        {"V", 1.0},   {"A", 1.0},     {"F", 1.0},     {"C", 1.0},    {"ohm", 1.0},
        {"degC", 1.0}, {"nC", 1e-9},  {"pF", 1e-12},  {"nF", 1e-9},  {"mA", 1e-3},
        {"uA", 1e-6}, {"mV", 1e-3},   {"mohm", 1e-3}};
    const auto it = k.find(u);
    if (it == k.end())
        throw std::runtime_error(who + ": curve label '" + label + "' states a unit Kelvin "
                                 "does not read");
    return it->second;
}

// The samples of one curve, in x order, as a row carries them (CurvePts): a permutation of the
// stated samples. Two samples at one x (or one float apart) state two values there and which is
// the curve is not on record: the curve is NOT kept (empty), as read_curve_xy below does.
CurvePts curve_pts(std::vector<std::pair<double, double>> pts) {
    std::stable_sort(pts.begin(), pts.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    auto x = std::make_shared<std::vector<float>>();
    auto y = std::make_shared<std::vector<float>>();
    for (const auto& [xi, yi] : pts) {
        const float xf = static_cast<float>(xi);
        if (!x->empty() && !(xf > x->back())) return CurvePts{};
        x->push_back(xf);
        y->push_back(static_cast<float>(yi));
    }
    CurvePts out;
    if (x->size() < 2) return out;  // one sample is a rating, not a curve
    out.x = std::move(x);
    out.y = std::move(y);
    return out;
}
// A stated numeric condition onto the curve: temperature / dcBias / frequency into their own
// fields (the capacitor curves' shape), any other into the sorted named list.
void set_condition(CurvePts& c, const std::string& key, double v) {
    if (key == "temperature") c.temperature = v;
    else if (key == "dcBias") c.dc_bias = v;
    else if (key == "frequency") c.frequency = v;
    else {
        const auto it = std::lower_bound(c.cond_keys.begin(), c.cond_keys.end(), key);
        const auto i = it - c.cond_keys.begin();
        c.cond_keys.insert(it, key);
        c.cond_vals.insert(c.cond_vals.begin() + i, v);
    }
}

// Reads a datasheetInfo.curves entry {xData, yData, xLabel?, yLabel?, conditions?} into a
// CurvePts: SI (the labels' units applied), in x order, every numeric condition carried. A
// malformed curve is a record defect that would put a wrong number in front of an engineer:
// loud, with the part (the resistor derating precedent below). Two samples at one x, or fewer
// than two samples: not kept (curve_pts).
CurvePts read_xy_curve(const json& c, const std::string& who) {
    auto bad = [&](const char* why) { return std::runtime_error(who + " " + why); };
    const json* xs = obj_get(c, "xData");
    const json* ys = obj_get(c, "yData");
    if (!xs || !ys || !xs->is_array() || !ys->is_array())
        throw bad("must carry xData[] and yData[]");
    if (xs->size() != ys->size() || xs->empty())
        throw bad("xData[] and yData[] must be equal-length and non-empty");
    const double sx = curve_label_scale(get_str(c, "xLabel").value_or(""), who);
    const double sy = curve_label_scale(get_str(c, "yLabel").value_or(""), who);
    std::vector<std::pair<double, double>> pts;
    for (size_t i = 0; i < xs->size(); ++i) {
        const json &xv = (*xs)[i], &yv = (*ys)[i];
        if (!xv.is_number() || xv.is_boolean() || !yv.is_number() || yv.is_boolean())
            throw bad("holds a non-numeric point");
        const double x = xv.get<double>() * sx, y = yv.get<double>() * sy;
        if (!std::isfinite(x) || !std::isfinite(y)) throw bad("holds a non-finite point");
        pts.emplace_back(x, y);
    }
    CurvePts out = curve_pts(std::move(pts));
    if (out.empty()) return out;
    if (const json* cond = obj_get(c, "conditions"); cond && cond->is_object())
        for (auto it = cond->begin(); it != cond->end(); ++it)
            if (it->is_number() && !it->is_boolean()) set_condition(out, it.key(), it->get<double>());
    return out;
}

// datasheetInfo.curves.<name>, or an empty curve when the record states none.
CurvePts read_named_curve(const json& di, const char* name, const std::string& mpn) {
    const json* curves = obj_get(di, "curves");
    const json* c = curves ? obj_get(*curves, name) : nullptr;
    if (!c) return {};
    if (!c->is_object()) throw std::runtime_error(mpn + ": curves." + name + " is not an object");
    // The labels carry the units (PEAS curve: "label for the X axis, including unit"); the
    // schema does not say an unlabelled axis is SI. A curve missing either label has no scale
    // on record: not read, never assumed SI — marked, so the comparison says why it is
    // UNVERIFIED (6 records in data 524a699: IPB017N10N5 cossVsVds / rdsOnVsTj, STPS30L60CT,
    // C3D10065A and C4D10120A forwardVoltageVsCurrent / capacitanceVsVoltage).
    // Owner 2026-11-14: such a curve is read as SI when its record's own scalar lies on it
    // (settle_unit_unstated below, which every unlabelled curve goes through). Until then the
    // points are read as SI and the curve is marked.
    if (!get_str(*c, "xLabel") || !get_str(*c, "yLabel")) {
        CurvePts out = read_xy_curve(*c, mpn + ": curves." + name);
        out.unit_unstated = true;
        return out;
    }
    return read_xy_curve(*c, mpn + ": curves." + name);
}

// The record's own scalar a curve without axis units is checked against (owner 2026-11-14):
// `scalar` (SI, in the curve's y quantity) stated at `at` (SI, on the curve's x axis). Both
// must be stated; the curve is read at `at` with the comparison's own interpolation.
struct UnitCheck {
    std::optional<double> scalar, at;
    bool loglog = false;
    const char* what = "";    // "Coss"
    const char* y_unit = "";  // printed unit of the scalar, after y_scale
    double y_scale = 1.0;
    const char* x_unit = "";  // printed unit of `at`
    // How `at` was obtained when the record does not state it (owner 2026-11-14: a scalar
    // Rds(on) with no stated junction temperature is taken at 25 degC, as Vf is); "" = stated.
    const char* at_basis = "";
};
// An unlabelled curve (read_named_curve) is kept, read as SI, only when the record's own scalar
// lies on it within 2 % at the scalar's stated condition; it is then marked unit_inferred and
// its unit_note says which scalar confirmed it. Otherwise its points are dropped (stated, not
// readable: unit_unstated) and unit_note says why — no scalar to check, the scalar's condition
// outside the curve, or the scalar not on it. A labelled curve is left as it is.
void settle_unit_unstated(CurvePts& c, const UnitCheck* check) {
    if (!c.unit_unstated) return;
    auto drop = [&](std::string why) {
        CurvePts out;
        out.unit_unstated = true;
        out.unit_note = std::move(why);
        c = std::move(out);
    };
    if (c.empty()) return drop("");
    if (!check || !check->scalar || !check->at)
        return drop("no scalar of its record states a value on it to check the units against");
    char b[512];
    const double sc = *check->scalar, at = *check->at;
    std::optional<double> read;
    {
        std::vector<double> xs(c.x->begin(), c.x->end()), ys(c.y->begin(), c.y->end());
        const crossref::Curve cv(std::move(xs), std::move(ys));
        bool ok = true;
        if (check->loglog)
            for (size_t i = 0; i < cv.x.size(); ++i)
                if (!(cv.x[i] > 0) || !(cv.y[i] > 0)) ok = false;
        if (ok) read = crossref::curve_at(cv, at, check->loglog ? crossref::Interp::LogLog
                                                                : crossref::Interp::Linear);
    }
    if (!read) {
        std::snprintf(b, sizeof b,
                      "its record's %s of %.4g %s is stated at %g %s%s, which the curve does not "
                      "reach as SI, so the units cannot be checked",
                      check->what, sc * check->y_scale, check->y_unit, at, check->x_unit,
                      check->at_basis);
        return drop(b);
    }
    if (!(std::fabs(*read - sc) <= crossref::kCurveLevelFraction * std::fabs(sc))) {
        std::snprintf(b, sizeof b,
                      "its record's %s of %.4g %s at %g %s%s does not lie on it read as SI "
                      "(%.4g %s there), so the units are not confirmed",
                      check->what, sc * check->y_scale, check->y_unit, at, check->x_unit,
                      check->at_basis, *read * check->y_scale, check->y_unit);
        return drop(b);
    }
    std::snprintf(b, sizeof b,
                  "its axes state no units; read as SI because its record's own %s of %.4g %s "
                  "at %g %s%s lies on it (%.4g %s read there)",
                  check->what, sc * check->y_scale, check->y_unit, at, check->x_unit,
                  check->at_basis, *read * check->y_scale, check->y_unit);
    c.unit_unstated = false;
    c.unit_inferred = true;
    c.unit_note = b;
}

// A dimensionWithTolerance-or-scalar field, collapsed to a scalar with the CANONICAL resolver
// (PEAS::resolve_dimensional_values) — never a hand-read of `.nominal`/`.maximum` (house rule).
// Semantics: number -> number; object -> preferred bound with the resolver's fallback chain
// (NOMINAL: nominal -> mid(min,max) -> max -> min). A field that is absent OR present-but-
// unresolvable returns nullopt, so the row is counted unreadable_row (the extractor's sanctioned
// permissive skip — accounted, not a silent default). For rows carrying a nominal this equals the
// old `.nominal` read, so parity holds; it differs only for a dimWithTol lacking a nominal, where
// the resolver's min/max fallback is the intended behaviour.
std::optional<double> resolve_field(const json& o, const char* key,
                                    PEAS::DimensionalValues pref = PEAS::DimensionalValues::NOMINAL) {
    const json* v = obj_get(o, key);
    if (!v) return std::nullopt;
    try {
        return PEAS::resolve_dimensional_values(*v, pref);
    } catch (const std::exception&) {
        return std::nullopt;  // present-but-unresolvable -> unreadable row (HS None semantics)
    }
}

bool pos(const std::optional<double>& x) { return x.has_value() && *x > 0.0; }

// TAS stores a P-channel MOSFET's or a PNP transistor's ratings signed as the datasheet
// prints them (V_DS = -30 V, I_D = -5 A); older rows carry the bare magnitude. The selector
// compares magnitudes, so a negative rating is read as its magnitude when the part says it
// is of the negative polarity. A negative rating on a part that says it is N-channel / NPN,
// or that names no polarity at all, contradicts itself: it stays unreadable (counted), never
// silently flipped — guessing which half of the record is wrong is not the indexer's call.
std::optional<double> rating_magnitude(std::optional<double> v, bool negative_polarity) {
    if (!v) return std::nullopt;
    if (*v < 0.0) return negative_polarity ? std::optional<double>(-*v) : std::nullopt;
    return v;
}

// A PEAS `curve` ({xData, yData, xLabel?, yLabel?}) read at x by log-log interpolation
// between the two samples that bracket it. nullopt when the curve is malformed, a label
// names a different unit than the caller expects, or x lies outside the sampled range —
// a value outside what was measured is not on record.
std::optional<double> curve_at(const json* c, double x, const char* x_unit, const char* y_unit) {
    if (!c || !c->is_object()) return std::nullopt;
    if (auto xl = get_str(*c, "xLabel"); xl && xl->find(x_unit) == std::string::npos)
        return std::nullopt;
    if (auto yl = get_str(*c, "yLabel");
        yl && (yl->find(y_unit) == std::string::npos || yl->find(std::string("m") + y_unit) != std::string::npos))
        return std::nullopt;
    const json* xs = obj_get(*c, "xData");
    const json* ys = obj_get(*c, "yData");
    if (!xs || !ys || !xs->is_array() || !ys->is_array() || xs->size() != ys->size())
        return std::nullopt;
    bool have_lo = false, have_hi = false;
    double xlo = 0, ylo = 0, xhi = 0, yhi = 0;
    for (size_t i = 0; i < xs->size(); ++i) {
        const json& xv = (*xs)[i];
        const json& yv = (*ys)[i];
        if (!xv.is_number() || !yv.is_number()) continue;
        const double xi = xv.get<double>(), yi = yv.get<double>();
        if (!(xi > 0) || !(yi > 0)) continue;
        if (xi == x) return yi;
        if (xi < x && (!have_lo || xi > xlo)) { xlo = xi; ylo = yi; have_lo = true; }
        if (xi > x && (!have_hi || xi < xhi)) { xhi = xi; yhi = yi; have_hi = true; }
    }
    if (!have_lo || !have_hi) return std::nullopt;
    const double t = (std::log(x) - std::log(xlo)) / (std::log(xhi) - std::log(xlo));
    return std::exp(std::log(ylo) + t * (std::log(yhi) - std::log(ylo)));
}

// A PEAS `curve` ({xData, yData, xLabel?, yLabel?, conditions?}) read WHOLE onto a row
// (v22, owner 2026-11-14). The samples are put in x order (the order a record lists them in
// carries no meaning); nothing else is changed. The curve is NOT kept (empty, counted by the
// caller's statistics as "no curve") when it is not one: fewer than two samples, x and y of
// different lengths, a non-numeric or non-finite sample, a label naming a different unit
// than the caller's (any of `y_units`; a milli-prefixed one is a different unit), two
// samples at one x (which of the two is the curve is not on record), or — when
// `positive_y` — a sample <= 0 on a quantity read on log axes. Every x must be > 0 (all
// the capacitor curves are frequencies or voltages from 0: `nonneg_x` admits x = 0).
// `conditions` {temperature, dcBias, frequency} are kept when numeric.
CurvePts read_curve_xy(const json* c, const char* x_unit, std::initializer_list<const char*> y_units,
                       bool positive_y, bool nonneg_x = false, bool signed_x = false) {
    CurvePts out;
    if (!c || !c->is_object()) return out;
    if (auto xl = get_str(*c, "xLabel"); xl && xl->find(x_unit) == std::string::npos) return out;
    if (auto yl = get_str(*c, "yLabel")) {
        bool ok = false;
        for (const char* u : y_units)
            if (yl->find(u) != std::string::npos && yl->find(std::string("m") + u) == std::string::npos)
                ok = true;
        if (!ok) return out;
    }
    const json* xs = obj_get(*c, "xData");
    const json* ys = obj_get(*c, "yData");
    if (!xs || !ys || !xs->is_array() || !ys->is_array() || xs->size() != ys->size() ||
        xs->size() < 2)
        return out;
    std::vector<std::pair<double, double>> pts;
    for (size_t i = 0; i < xs->size(); ++i) {
        const json& xv = (*xs)[i];
        const json& yv = (*ys)[i];
        if (!xv.is_number() || !yv.is_number()) return out;
        const double xi = xv.get<double>(), yi = yv.get<double>();
        if (!std::isfinite(xi) || !std::isfinite(yi)) return out;
        // `signed_x`: an axis that legitimately crosses 0 (junction temperature, degC).
        if (!signed_x && (nonneg_x ? xi < 0 : !(xi > 0))) return out;
        if (positive_y && !(yi > 0)) return out;
        pts.emplace_back(xi, yi);
    }
    std::sort(pts.begin(), pts.end());
    auto x = std::make_shared<std::vector<float>>();
    auto y = std::make_shared<std::vector<float>>();
    for (const auto& [xi, yi] : pts) {
        const float xf = static_cast<float>(xi);
        if (!x->empty() && !(xf > x->back())) return out;  // duplicate x (or one float apart)
        x->push_back(xf);
        y->push_back(static_cast<float>(yi));
    }
    out.x = std::move(x);
    out.y = std::move(y);
    if (const json* cond = obj_get(*c, "conditions"); cond && cond->is_object()) {
        if (auto v = get_num(*cond, "temperature")) out.temperature = *v;
        if (auto v = get_num(*cond, "dcBias")) out.dc_bias = *v;
        if (auto v = get_num(*cond, "frequency")) out.frequency = *v;
    }
    return out;
}

// The leading run of a curve up to (not including) its first sample <= 0; empty when fewer
// than two samples remain. Used where a sign change ends the quantity's meaning (C(f) past
// self-resonance), so the rest of the curve is not read as that quantity.
CurvePts leading_positive(CurvePts c) {
    if (c.empty()) return c;
    size_t n = 0;
    while (n < c.y->size() && (*c.y)[n] > 0) ++n;
    if (n == c.y->size()) return c;
    if (n < 2) return CurvePts{};
    c.x = std::make_shared<std::vector<float>>(c.x->begin(), c.x->begin() + n);
    c.y = std::make_shared<std::vector<float>>(c.y->begin(), c.y->begin() + n);
    return c;
}

// CAS capacitanceBiasPoints ([{voltage, capacitance, temperature?, acVoltage?}]) as a curve:
// DC bias V -> effective capacitance F, absolute, as the records state it (34,110 records,
// none states a ratio). Put in voltage order; NOT kept when it is not one curve: fewer than
// two points, a non-numeric / negative voltage or a capacitance <= 0, two points at one
// voltage, or points at more than one temperature (one curve per temperature is not what a
// single row carries). The temperature, when stated, is the curve's condition.
CurvePts read_bias_curve(const json* b) {
    CurvePts out;
    if (!b || !b->is_array() || b->size() < 2) return out;
    std::vector<std::pair<double, double>> pts;
    std::optional<double> temp;
    bool first = true;
    for (const json& p : *b) {
        if (!p.is_object()) return out;
        const auto v = get_num(p, "voltage"), c = get_num(p, "capacitance");
        if (!v || !c || !std::isfinite(*v) || !std::isfinite(*c) || *v < 0 || !(*c > 0)) return out;
        const auto t = get_num(p, "temperature");
        if (first) temp = t;
        else if (t.has_value() != temp.has_value() || (t && *t != *temp)) return out;
        first = false;
        pts.emplace_back(*v, *c);
    }
    std::sort(pts.begin(), pts.end());
    auto x = std::make_shared<std::vector<float>>();
    auto y = std::make_shared<std::vector<float>>();
    for (const auto& [xi, yi] : pts) {
        const float xf = static_cast<float>(xi);
        if (!x->empty() && !(xf > x->back())) return out;
        x->push_back(xf);
        y->push_back(static_cast<float>(yi));
    }
    out.x = std::move(x);
    out.y = std::move(y);
    if (temp) out.temperature = *temp;
    return out;
}

// A ripple-current table (v22, owner 2026-11-14) — the record's own frequency or temperature
// multiplier / ampere table, kept only when it is evidently THIS rating's table, so the ranker
// can move the scalar rating to the other side's basis with I x y(target) / y(stated):
//   * the y label names a ripple current ("Ripple current multiplier", "Ripple current [A]"),
//     or the table is unlabelled and reads 1.0 (a multiplier) or the scalar itself (amperes)
//     at the rating's own stated basis, within 1 % (an unlabelled table that does neither is
//     not shown to belong to this rating); any other label ("Maximum continuous current") is
//     not a ripple table;
//   * the x label, when present, names the axis (`x_unit`) and, for temperature, not the CASE
//     temperature: a case-temperature curve is a different condition from the ambient scalar
//     rating (owner 2026-11-14);
//   * the table's own stated condition on the OTHER axis (a frequency table's temperature, a
//     temperature table's frequency) must be the scalar's; a condition stated by one side only
//     or not numeric is a different condition (owner 2026-11-14) and the table is not kept;
//   * every sample finite, y > 0 (a ratio of two readings), x strictly increasing.
// The y values are kept as stated (multiplier or amperes): the ranker only ever uses the
// ratio of two readings, which is the same either way. `basis` is the scalar's own stated
// point on this axis (NaN when not stated: an unlabelled table cannot then be anchored).
// `other_basis` is the scalar's stated value on the other axis (NaN = not stated);
// `other_key` the condition key ("temperature" / "frequency"); for a frequency condition the
// 100 Hz / 120 Hz sameness of the ripple basis (owner 2026-11-09) applies. `log_x`: frequency
// (read log-log, x > 0 required); temperature is read linearly.
// The CAS `factors` block (rippleCurrentFrequencyFactor* / rippleCurrentTemperatureFactor*)
// repeats the same tables on the records that carry it (Nichicon, Panasonic), so only the
// electrical.*Points tables are read.
CurvePts read_ripple_table(const json* c, const char* x_unit, double basis, double scalar,
                           const char* other_key, double other_basis, bool log_x) {
    CurvePts out;
    if (!c || !c->is_object()) return out;
    if (auto xl = get_str(*c, "xLabel")) {
        if (xl->find(x_unit) == std::string::npos) return out;
        std::string l = *xl;
        for (auto& ch : l) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (l.find("case") != std::string::npos) return out;
    }
    bool labelled = false;
    if (auto yl = get_str(*c, "yLabel")) {
        if (yl->rfind("Ripple current", 0) != 0) return out;
        labelled = true;
    }
    if (const json* cond = obj_get(*c, "conditions"); cond && cond->is_object()) {
        if (auto it = cond->find(other_key); it != cond->end() && !it->is_null()) {
            if (!it->is_number() || std::isnan(other_basis)) return out;
            const double v = it->get<double>();
            auto same = [](double a, double b) {
                return std::fabs(a - b) <= 1e-9 * std::max(std::fabs(a), std::fabs(b));
            };
            const bool mains = std::string(other_key) == "frequency" &&
                               (same(v, 100) || same(v, 120)) &&
                               (same(other_basis, 100) || same(other_basis, 120));
            if (!same(v, other_basis) && !mains) return out;
        }
    }
    const json* xs = obj_get(*c, "xData");
    const json* ys = obj_get(*c, "yData");
    if (!xs || !ys || !xs->is_array() || !ys->is_array() || xs->size() != ys->size() ||
        xs->size() < 2)
        return out;
    std::vector<std::pair<double, double>> pts;
    for (size_t i = 0; i < xs->size(); ++i) {
        const json& xv = (*xs)[i];
        const json& yv = (*ys)[i];
        if (!xv.is_number() || !yv.is_number()) return out;
        const double xi = xv.get<double>(), yi = yv.get<double>();
        if (!std::isfinite(xi) || !std::isfinite(yi) || !(yi > 0)) return out;
        if (log_x && !(xi > 0)) return out;
        pts.emplace_back(xi, yi);
    }
    std::sort(pts.begin(), pts.end());
    for (size_t i = 1; i < pts.size(); ++i)
        if (!(pts[i].first > pts[i - 1].first)) return out;
    if (!labelled) {
        if (std::isnan(basis)) return out;
        // the table's reading at the stated basis (interpolated, log-log for frequency)
        std::optional<double> at;
        for (size_t i = 1; i < pts.size() && !at; ++i) {
            const auto [x0, y0] = pts[i - 1];
            const auto [x1, y1] = pts[i];
            if (basis < x0 || basis > x1) continue;
            if (basis == x0) at = y0;
            else if (basis == x1) at = y1;
            else if (log_x)
                at = std::exp(std::log(y0) + (std::log(basis) - std::log(x0)) /
                                                 (std::log(x1) - std::log(x0)) *
                                                 (std::log(y1) - std::log(y0)));
            else at = y0 + (basis - x0) / (x1 - x0) * (y1 - y0);
        }
        if (!at) return out;
        const bool unit_multiplier = std::fabs(*at - 1.0) <= 0.01;
        const bool amperes = std::fabs(*at - scalar) <= 0.01 * scalar;
        if (!unit_multiplier && !amperes) return out;
    }
    auto x = std::make_shared<std::vector<float>>();
    auto y = std::make_shared<std::vector<float>>();
    for (const auto& [xi, yi] : pts) {
        const float xf = static_cast<float>(xi);
        if (!x->empty() && !(xf > x->back())) return CurvePts{};
        x->push_back(xf);
        y->push_back(static_cast<float>(yi));
    }
    out.x = std::move(x);
    out.y = std::move(y);
    return out;
}

// Physical body size + case code, shared by every family extractor: the
// cross-reference footprint check needs a real size, because a substitute that
// does not fit the board is not a substitute. Dimensions are
// dimensionWithTolerance in TAS, so they go through the resolver rather than
// being hand-read. Absent stays NaN — the ranker then falls back to resolving
// the case code, and reports "unknown" if that fails too, never assuming a fit.
// `lies_flat`: the body's axis is PARALLEL to the board (an axial or MELF resistor), so a
// stated diameter is its width and its height and the length stays the length. Without it a
// diameter is read as an upright can, which turned a 9.53 mm-long PTF65 into a 3.68 x 3.68 mm
// footprint standing 9.53 mm tall (ABT #2032).
// `upright_disc`: the body is a disc standing on its leads (see the diameter fold below).
void fill_dimensions(RowBase& r, const json& di, const json& part, bool lies_flat = false,
                     bool upright_disc = false) {
    // Mount type first: `thickness` below needs it, because the same T label names a
    // different axis on a chip than on a leaded body. Three schemas spell the SAME fact
    // three ways: `mechanical.assemblyType` (semiconductors, magnetics),
    // `mechanical.shape.assembly` (CAS capacitors — THT / SMT / Snap-In, stated on every
    // record) and `mechanical.mountingStyle` (CONAS connectors). Reading only the first
    // left the mount unknown for the whole capacitor and connector catalogues, so the
    // mount gate never fired there even though the record said outright which one the
    // part is.
    const json* mech = obj_get(di, "mechanical");
    if (mech) {
        if (auto a = get_str(*mech, "assemblyType")) r.mount = *a;
        else if (auto a = get_str(*mech, "mountingStyle")) r.mount = *a;
        else if (const json* shape = obj_get(*mech, "shape")) {
            if (auto a = get_str(*shape, "assembly")) r.mount = *a;
        }
        // Some records nest the drawing one level deeper under `dimensions`.
        const json* nested = obj_get(*mech, "dimensions");
        const json& src = nested ? *nested : *mech;
        if (auto v = resolve_field(src, "length"); pos(v)) r.length_m = *v;
        if (auto v = resolve_field(src, "width"); pos(v)) r.width_m = *v;
        if (auto v = resolve_field(src, "height"); pos(v)) r.height_m = *v;
        std::optional<double> dia = resolve_field(src, "diameter");
        // Lead pitch of a leaded body: CAS states it as mechanical.dimensions.pitch (only
        // in the nested drawing — a connector's mechanical.pitch is its CONTACT pitch, its
        // own axis). Else the record's own drilled two-hole landPattern states it as the
        // distance between the two hole centres. Neither -> NaN, never a typical value.
        if (nested)
            if (auto v = resolve_field(*nested, "pitch"); pos(v)) r.lead_pitch_m = *v;
        if (!present(r.lead_pitch_m))
            if (const json* lp = obj_get(*mech, "landPattern"))
                if (const json* pads = obj_get(*lp, "pads"); pads && pads->is_array() &&
                                                             pads->size() == 2) {
                    const json& a = (*pads)[0];
                    const json& b = (*pads)[1];
                    auto ax = get_num(a, "x"), ay = get_num(a, "y"), bx = get_num(b, "x"),
                         by = get_num(b, "y");
                    if (a.contains("drill") && b.contains("drill") && ax && ay && bx && by) {
                        const double d = std::hypot(*bx - *ax, *by - *ay);
                        if (d > 0) r.lead_pitch_m = d;
                    }
                }
        if (nested) {  // a nested block may still leave siblings on `mechanical`
            if (!present(r.length_m))
                if (auto v = resolve_field(*mech, "length"); pos(v)) r.length_m = *v;
            if (!present(r.width_m))
                if (auto v = resolve_field(*mech, "width"); pos(v)) r.width_m = *v;
            if (!present(r.height_m))
                if (auto v = resolve_field(*mech, "height"); pos(v)) r.height_m = *v;
            if (!pos(dia)) dia = resolve_field(*mech, "diameter");
        }
        // A cylindrical body (an electrolytic can, an axial part) states a DIAMETER
        // instead of a footprint: the board area it occupies is diameter x diameter,
        // and the one axis left over is the can's length — which records name
        // `height` (Würth) or `length` (Vishay) for the very same axis. Fold it the
        // way the "DxL" case-code resolver already does (LWH{d, d, len}) so an
        // explicit drawing and a case code yield the same box instead of two
        // incomparable ones. Dropping the diameter left every can in the catalogue
        // with no seat at all, and the footprint check with nothing to compare.
        //
        // An UPRIGHT DISC (`upright_disc`: shapeType names a disc) is the exception to the
        // can fold: it stands on its leads with its face vertical and the leads in the
        // face's plane, so the diameter runs across the leads AND upward, and the thickness
        // is the land's depth. Board footprint D x T, height D. Datasheets checked
        // 2026-11-09: Vishay D Series (doc 28549, rev. 08-Jan-2026) p.2 "Lead
        // configuration": D drawn in the lead plane, T in the side view; KEMET C900
        // (C1065_X1_440_VAC_Y2_300_VAC, 10/25/22) p.3 "Lead Configurations": "D Body
        // Diameter" across the leads, "T Body Thickness" side view; TDK CK45 (e413_ck45,
        // July 2011) p.3/3 taping drawing: D in the lead plane, T in the side view. The
        // can fold read KEMET C901U100DZNDAA7317 (D 7, T 5 mm) as a 7 x 7 mm land with no
        // height and lost T. Only when the record states no length or width of its own (a
        // disc that does would be a drawing this rule has not seen); a stated height (the
        // seated height) is kept; an unstated thickness leaves the depth unknown, never D.
        const bool disc_fold = upright_disc && pos(dia) && !lies_flat &&
                               !present(r.length_m) && !present(r.width_m);
        if (disc_fold) {
            std::optional<double> disc_thk = resolve_field(src, "thickness");
            if (nested && !pos(disc_thk)) disc_thk = resolve_field(*mech, "thickness");
            r.length_m = *dia;
            if (pos(disc_thk)) r.width_m = *disc_thk;
            if (!present(r.height_m)) r.height_m = *dia;
        } else if (pos(dia) && lies_flat) {
            if (!present(r.length_m) && present(r.height_m)) r.length_m = r.height_m;
            r.width_m = *dia;
            r.height_m = *dia;
        } else if (pos(dia)) {
            const double axial = present(r.height_m) ? r.height_m : r.length_m;
            r.length_m = *dia;
            r.width_m = *dia;
            r.height_m = axial;
        } else {
            // `thickness` is the body's T dimension, and WHICH axis that is depends on how
            // the part mounts, so the mount decides the slot:
            //   chip-style SMT (MLCC, chip film) — CAS spells it out: "the dimension
            //     perpendicular to the mounting plane", i.e. the HEIGHT of an L x W x T body;
            //   leaded box (R73 / F5A radial film, radial-moulded tantalum) — the drawing's
            //     H is the height above the board and T is the body DEPTH, the second
            //     FOOTPRINT axis.
            // Reading it into neither dropped the third dimension of 33k capacitors:
            // R73TN2150SE00K states 26.5 x 17 x 8.5 mm and reached the ranker as a 26.5 mm
            // box of unknown width, so no footprint or body-width gate could fire on it
            // (ABT #480). Only ever fills a slot the record left empty, and never on a
            // cylindrical body — there the diameter fold above already defines the box and a
            // stated T is not one of its axes. An unclassified mount fills nothing: which
            // axis T is would be a guess, and an honest "unknown" beats a plausible number.
            std::optional<double> thk = resolve_field(src, "thickness");
            if (nested && !pos(thk)) thk = resolve_field(*mech, "thickness");
            if (pos(thk)) {
                const std::string mount = crossref::normalize_mount(r.mount);
                if (mount == "smd") {
                    if (!present(r.height_m)) r.height_m = *thk;
                } else if (mount == "leaded") {
                    if (!present(r.width_m)) {
                        r.width_m = *thk;
                    } else if (present(r.length_m) && !present(r.height_m) && lies_flat) {
                        // An AXIAL box drawn L x W x T (KEMET SRA / TRA): L is the body
                        // axis, lead exit to lead exit, parallel to the board; W and T
                        // are its cross-section, W across and T upward — KEMET C1119
                        // S-Series (7/2/2024) p.2, "Dimensions", axial end view: W drawn
                        // horizontal, T vertical; and CAS's own definition of thickness
                        // as the dimension perpendicular to the mounting plane. So the
                        // land is L x W and the height T.
                        r.height_m = *thk;
                    } else if (present(r.length_m) && !present(r.height_m)) {
                        // A RADIAL box drawn L x W x T with no H (TDK FA/FG/FK radial
                        // MLCCs: 4,620 records; KEMET 28, Murata 12, survey 2026-11-09).
                        // T is the body depth, a footprint axis, on every vendor's
                        // drawing; L and W span the face the leads leave from, so one is
                        // the height above the board and the other runs across the
                        // leads. WHICH one is a vendor convention, and the vendors
                        // disagree (datasheets checked 2026-11-09):
                        //   TDK FG catalogue (leadmlcc_halogenfree_fg_en, 20181102):
                        //     the type code reads "LxWxT"; FG26 = 5.5x6.0x3.5, and the
                        //     FG26 drawing (p.14) puts 5.5 across the leads, 6.0 upward
                        //     (FG28 p.6, FG18 p.23 the same): L across, W = height;
                        //   Murata RDER72E105MUB1H03B data sheet p.1 "Appearance &
                        //     Shape": "L max." across the leads, "W max." upward: L
                        //     across, W = height;
                        //   KEMET C1119 S-Series p.2 and C1116 V-Series p.2 radial
                        //     drawings: W across the leads, L upward: L = height.
                        // The record states neither the convention nor the height, and
                        // CAS defines length / width only as "Length / B" and "Width /
                        // A", so the orientation cannot be read from the data. Choosing
                        // by manufacturer name would be a guess wearing a datasheet's
                        // clothes; instead the box stays unfilled and the row says why,
                        // and the cross-reference reports the footprint UNVERIFIED with
                        // a note (CrossRef.hpp). The fix belongs in TAS: state the height
                        // above the board as `height` (and the across-leads axis as
                        // `length`), as the KEMET L x T x H radial records already do.
                        // (Before: T dropped, FG26C0G2A223JNT06 boxed on its 5.5 x 6.0 mm
                        // face; then 00f06be's guess of KEMET's convention for every
                        // vendor, which turned TDK's and Murata's boxes on their side.)
                        r.length_m = kNaN();
                        r.width_m = kNaN();
                        r.box_axes_ambiguous = true;
                    }
                }
            }
        }
    }
    // Case code: `part.case` is the field the catalogue actually populates
    // (100% of sampled capacitors/mosfets/resistors/diodes); `mechanical.case`
    // and the caseCode/package spellings are accepted as fallbacks.
    //
    // A PLACEHOLDER is not a case code (Qarlos 2026-11-09): 13 MOSFET, 84 diode and 624 IGBT
    // records state case "Unknown" (Infineon BSC160N15NS5ATMA1 ...). Read as a code, two
    // such records named the same "case" and the same-case rule called the land kept. Rule:
    // a placeholder reads as not stated, and the next source is consulted.
    auto stated_case = [](const std::optional<std::string>& c) -> std::optional<std::string> {
        if (!c) return std::nullopt;
        std::string t;
        for (char ch : *c)
            if (!std::isspace(static_cast<unsigned char>(ch)))
                t += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (t.empty() || t == "unknown" || t == "n/a" || t == "-" || t == "tbd" ||
            t == "none")
            return std::nullopt;
        return c;
    };
    if (auto c = stated_case(get_str(part, "case"))) r.case_code = *c;
    else if (mech) { if (auto c = stated_case(get_str(*mech, "case"))) r.case_code = *c; }
    if (r.case_code.empty()) {
        if (auto c = stated_case(get_str(part, "caseCode"))) r.case_code = *c;
        else if (auto p = stated_case(get_str(part, "package"))) r.case_code = *p;
    }
}

}  // namespace

bool datasheet_unusable(const std::string& url) {
    if (url.empty()) return true;
    static const std::regex re_example(R"(^https?://(www\.)?example\.com)",
                                       std::regex::icase);
    static const std::regex re_vishay(R"(vishay\.com/(en/)?search)", std::regex::icase);
    static const std::regex re_dspdf(R"(datasheetpdf\.com)", std::regex::icase);
    return std::regex_search(url, re_example) || std::regex_search(url, re_vishay) ||
           std::regex_search(url, re_dspdf);
}

std::string normalize_ctas_topology(const std::string& name) {
    static const std::unordered_map<std::string, std::string> kMap = {
        {"buckConverter", "buck"},
        {"boostConverter", "boost"},
        {"buckBoostConverter", "buck_boost"},
        {"flybackConverter", "flyback"},
        {"forwardConverter", "forward"},
        {"llcResonantConverter", "llc"},
        {"powerFactorCorrection", "power_factor_correction"},
        {"sepicConverter", "sepic"},
        {"cukConverter", "cuk"},
        {"zetaConverter", "zeta"},
        {"pushPullConverter", "push_pull"},
        {"phaseShiftedFullBridge", "phase_shifted_full_bridge"},
        {"dualActiveBridge", "dual_active_bridge"},
        {"cllcResonantConverter", "cllc"},
        {"clllcResonantConverter", "clllc"},
        {"seriesResonantConverter", "series_resonant"},
        {"viennaRectifierConverter", "vienna"},
    };
    auto it = kMap.find(name);
    if (it != kMap.end()) return it->second;
    std::string s = name;
    if (s.size() >= 9 && s.compare(s.size() - 9, 9, "Converter") == 0)
        s = s.substr(0, s.size() - 9);
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        char ch = s[i];
        if (ch >= 'A' && ch <= 'Z') {
            if (i > 0) out.push_back('_');
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::optional<MosfetRow> extract_mosfet(const json& env) {
    const json* semi = obj_get(env, "semiconductor");
    if (!semi) return std::nullopt;
    const json* m = obj_get(*semi, "mosfet");
    if (!m) return std::nullopt;
    const json* mi = obj_get(*m, "manufacturerInfo");
    if (!mi) return std::nullopt;
    const json* di = obj_get(*mi, "datasheetInfo");
    if (!di) return std::nullopt;
    const json empty = json::object();
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;
    const json* elec = obj_get(*di, "electrical");
    // A multi-die package (part.dieConfiguration 'dual' / 'complementary') carries `dies`, one
    // full electrical block per die, INSTEAD of a package-level block (SAS mosfet.json). The
    // row holds one value per field, so it is read from the dies only when they state the
    // SAME thing: every die's electrical block and channel identical (owner 2026-11-13) —
    // then that die's values are the part's, per die, as for a dual diode. Dies that differ
    // (every complementary pair, asymmetric power blocks) state no single value for the
    // part's Vds / Id / Rds(on) and are left unread rather than given one die's numbers.
    std::optional<std::string> die_channel;
    if (!elec) {
        const json* dies = obj_get(*di, "dies");
        if (!dies || !dies->is_array() || dies->size() < 2) return std::nullopt;
        const json& first = (*dies)[0];
        const json* e0 = obj_get(first, "electrical");
        auto ch0 = get_str(first, "subType");
        if (!e0 || !ch0) return std::nullopt;
        for (const auto& d : *dies) {
            const json* e = obj_get(d, "electrical");
            auto ch = get_str(d, "subType");
            if (!e || !ch || *e != *e0 || *ch != *ch0) return std::nullopt;
        }
        elec = e0;
        die_channel = ch0;
    }

    auto mpn = get_str(*mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    auto manuf = get_str(*mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    const std::string channel =
        die_channel ? *die_channel : get_str(*part, "subType").value_or("");
    const bool p_channel = channel == "pChannel";
    auto vds = rating_magnitude(get_num(*elec, "drainSourceVoltage"), p_channel);
    auto id = rating_magnitude(get_num(*elec, "continuousDrainCurrent"), p_channel);
    auto rds = get_num(*elec, "onResistance");
    if (!pos(vds) || !pos(rds)) return std::nullopt;
    // Id is required, with ONE exception (owner 2026-11-13, V1): a multi-die record whose
    // dies (identical, see above) state no continuousDrainCurrent at all and that has no
    // package electrical block to state one either. 46 catalogue duals are that shape (TI
    // CSD885xx/CSD873xx power blocks, AOS AO48xx ...): read with Id UNSET (NaN, never 0),
    // which the ranker grades UNVERIFIED, the selector and browse filters exclude as not
    // stated. A stated non-positive Id, or a single-die record without one, stays unread.
    const bool id_unstated_on_dies =
        die_channel.has_value() && !elec->contains("continuousDrainCurrent");
    if (!pos(id) && !id_unstated_on_dies) return std::nullopt;

    // Absent stays ABSENT (NaN in the row), never 0: see MosfetRow::qg_total. The selector's
    // Qg constraint is vacuous for a row without one, and the evidence tier counts it as
    // incomplete, exactly as the 0 did — but the row no longer claims 0 C of gate charge.
    auto qg = get_num(*elec, "totalGateCharge");
    if (qg && *qg < 0.0) return std::nullopt;

    // Leave ABSENT (NaN) when the record does not state one. `value_or(0.0)` wrote 0 F
    // output capacitance — physically impossible, and the same bits a stated 0 would
    // produce — into every row whose record has no `outputCapacitance` key, so the row
    // reported coss: 0 where every other absent field reports null (ABT #484). `pos`
    // treats a stated 0 as not-stated too, which is what it is: no MOSFET has zero Coss.
    auto coss_o = get_num(*elec, "outputCapacitance");
    if (coss_o && *coss_o < 0.0) return std::nullopt;

    // A P-channel threshold is a negative interval (-2.5 … -1 V): its worst case, the largest
    // magnitude, is the MINIMUM bound, negated. A legacy positive record keeps the old read.
    //
    // Only a STATED bound is a maximum. The resolver's MAXIMUM preference falls back to the
    // nominal and then to the minimum when the record has no maximum, which is right for a
    // value but wrong for a guaranteed limit: 1,350 catalogue MOSFETs state only a nominal
    // threshold and 668 only a minimum, and each one reached the ranker as a worst-case
    // Vgs(th)max — BSC028N08NS5's typical 2.3 V was reported as its maximum and drove a
    // gate-threshold warn the record does not support (ABT #2045). With no stated bound the
    // field stays ABSENT (NaN): it used to be 0, and the row then reported a 0 V worst-case
    // threshold for every nominal-only record (SQJ409EP, BSC020N04LS, IGO60R040D1 ...).
    //
    // P-channel bounds are read as MAGNITUDES, whichever key holds them (Qarlos 2026-11-09,
    // seed 21936773). The catalogue's P-channel records state the threshold four ways
    // (survey of mosfets.ndjson): {minimum, maximum} both negative, the minimum always the
    // larger magnitude (481, signed order); a lone negative minimum (656, e.g. -4 V); a lone
    // negative MAXIMUM (147: onsemi FDMA86265P -4 V, FQD5P20TM -5 V, the vendor's "Max"
    // column); a lone nominal (142). The old read took only the minimum, so every lone-maximum
    // record lost its threshold and a -4 V part read 'unverified' against a -2 V logic-level
    // ISP14EP15LM. Rule: the worst case is the LARGEST magnitude among the stated negative
    // minimum/maximum. A lone negative maximum is that magnitude in either reading of the
    // record: in the vendor-column reading it is |Vth|max; in the signed reading the
    // magnitude is at least that, so a compare that calls it higher than the original's is
    // right both ways. One exception, where the record says outright that its bound is the
    // SMALL end: a stated nominal with a larger magnitude than every stated bound (Vishay
    // SUD08P06-155L-GE3: maximum -1, nominal -2). Then no worst case is stated: absent.
    std::optional<double> vgs, vgs_typ;
    if (const json* gtv = obj_get(*elec, "gateThresholdVoltage"); gtv && gtv->is_object()) {
        auto hi = get_num(*gtv, "maximum"), lo = get_num(*gtv, "minimum"),
             nom = get_num(*gtv, "nominal");
        if (p_channel && ((lo && *lo < 0.0) || (hi && *hi < 0.0))) {
            double worst = 0.0;
            if (lo && *lo < 0.0) worst = std::max(worst, -*lo);
            if (hi && *hi < 0.0) worst = std::max(worst, -*hi);
            if (!(nom && *nom < 0.0 && -*nom > worst)) vgs = worst;
        } else if (pos(hi)) {
            vgs = hi;
        }
        // The typical, as a magnitude (see MosfetRow::vgs_threshold_typ). A P-channel
        // nominal is negative; an N-channel one must be positive to be read.
        if (nom && *nom != 0.0 && (p_channel || *nom > 0.0)) vgs_typ = std::fabs(*nom);
    }

    MosfetRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    fill_dimensions(r, *di, *part);
    r.vds_rated = *vds;
    if (pos(id)) r.id_continuous = *id;
    r.rds_on = *rds;
    if (pos(qg)) r.qg_total = *qg;
    if (pos(coss_o)) r.coss = *coss_o;
    if (auto v = get_num(*elec, "capacitanceMeasurementVds"); pos(v)) r.coss_vds = *v;
    // Distributor unit costs (owner 2026-11-14): only a stated positive value with a currency.
    if (const json* dis = obj_get(*m, "distributorsInfo"); dis && dis->is_array()) {
        json quotes = json::array();
        for (const json& d : *dis) {
            if (!d.is_object()) continue;
            const json* cost = obj_get(d, "cost");
            if (!cost || !cost->is_object()) continue;
            auto v = get_num(*cost, "value");
            auto cur = get_str(*cost, "currency");
            if (!pos(v) || !cur || cur->empty()) continue;
            json q = {{"distributor", get_str(d, "name").value_or("")},
                      {"unitCost", *v},
                      {"currency", *cur}};
            if (auto moq = get_num(d, "moq"); pos(moq)) q["moq"] = *moq;
            quotes.push_back(std::move(q));
        }
        if (!quotes.empty()) r.price_quotes = quotes.dump();
    }
    // Operating-point loss inputs (owner 2026-11-14): stated values only, absent stays NaN.
    if (auto v = get_num(*elec, "gateSourceCharge"); pos(v)) r.qgs = *v;
    if (auto v = get_num(*elec, "gateDrainCharge"); pos(v)) r.qgd = *v;
    if (auto v = rating_magnitude(get_num(*elec, "totalGateChargeVgs"), p_channel); pos(v))
        r.qg_vgs = *v;
    // The Coss(Vds), Rds(on)(Tj) and gate-charge curves the loss terms read are the row's
    // measured curves below (read_named_curve: one set, with the conditions each states).
    if (vgs) r.vgs_threshold_max = *vgs;
    if (vgs_typ) r.vgs_threshold_typ = *vgs_typ;
    if (auto v = rating_magnitude(get_num(*elec, "onResistanceVgs"), p_channel); pos(v))
        r.rds_on_vgs = *v;
    if (auto v = rating_magnitude(get_num(*elec, "gateSourceVoltageMax"), p_channel); pos(v))
        r.vgs_max = *v;
    r.channel = channel;
    r.die_configuration = get_str(*part, "dieConfiguration").value_or("");
    r.internal_connection = get_str(*part, "internalConnection").value_or("");
    r.qualification = get_str(*part, "qualification").value_or("");
    r.technology = get_str(*part, "technology").value_or("");
    // Measured curves (owner rule 2026-11-14), as stated; compared by CrossRefCurveChecks.hpp.
    r.curves.mut().ciss_curve = read_named_curve(*di, "cissVsVds", *mpn);
    r.curves.mut().coss_curve = read_named_curve(*di, "cossVsVds", *mpn);
    r.curves.mut().crss_curve = read_named_curve(*di, "crssVsVds", *mpn);
    r.curves.mut().rds_on_id_curve = read_named_curve(*di, "rdsOnVsId", *mpn);
    r.curves.mut().rds_on_tj_curve = read_named_curve(*di, "rdsOnVsTj", *mpn);
    r.curves.mut().qg_curve = read_named_curve(*di, "gateCharge", *mpn);
    {
        // Unlabelled curves: checked against the record's own scalar at its stated condition.
        auto opt = [](double v) { return present(v) ? std::optional<double>(v) : std::nullopt; };
        auto elec_pos = [&](const char* k) -> std::optional<double> {
            auto v = get_num(*elec, k);
            return pos(v) ? v : std::nullopt;
        };
        const std::optional<double> cvds = opt(r.coss_vds);
        const UnitCheck ciss{elec_pos("inputCapacitance"), cvds, false, "Ciss", "pF", 1e12, "V"};
        const UnitCheck coss{opt(r.coss), cvds, false, "Coss", "pF", 1e12, "V"};
        const UnitCheck crss{elec_pos("reverseTransferCapacitance"), cvds, false, "Crss", "pF",
                             1e12, "V"};
        const UnitCheck rid{opt(r.rds_on), elec_pos("onResistanceId"), false, "Rds(on)", "mohm",
                            1e3, "A"};
        settle_unit_unstated(r.curves.mut().ciss_curve, &ciss);
        settle_unit_unstated(r.curves.mut().coss_curve, &coss);
        settle_unit_unstated(r.curves.mut().crss_curve, &crss);
        settle_unit_unstated(r.curves.mut().rds_on_id_curve, &rid);
        // Rds(on)(Tj): the scalar Rds(on) states no junction temperature (SAS has no field for
        // it); owner 2026-11-14: it is taken as 25 degC for this unit check, the convention Vf
        // follows, and the note says so. The gate-charge curve's x is the charge, no scalar
        // states a Vgs-to-charge point checked here: not checked.
        UnitCheck rtj{opt(r.rds_on), std::optional<double>(25.0), false, "Rds(on)", "mohm",
                      1e3, "degC"};
        rtj.at_basis = " (the record states no junction temperature for it: taken as 25 degC, "
                       "as for Vf)";
        settle_unit_unstated(r.curves.mut().rds_on_tj_curve, &rtj);
        settle_unit_unstated(r.curves.mut().qg_curve, nullptr);
    }
    // Ambient ratings (the package's, or the identical dies' this row is read from — the 43
    // AOS duals state their per-die Id only here, ABT #2388): one curve per rating and per
    // gate-source voltage its points state, x = ambient degC, y = |Id| (a P-channel record
    // states a negative current; the rating is its magnitude, as rating_magnitude reads Id).
    // Points stating no drain current (power only) are not Id points and are not part of it.
    // The conditions: timeBase and mounting (free text), duration and Vgs (numbers).
    if (const json* ars = obj_get(*elec, "ambientRatings"); ars && ars->is_array()) {
        for (const auto& a : *ars) {
            const json* pts = obj_get(a, "points");
            if (!pts || !pts->is_array()) continue;
            std::string text;
            if (auto tb = get_str(a, "timeBase")) text = *tb;
            if (auto mt = get_str(a, "mounting"))
                text += (text.empty() ? "" : "; ") + *mt;
            // group by the Vgs each point states (none = its own group)
            std::vector<std::pair<std::optional<double>, std::vector<std::pair<double, double>>>> g;
            for (const auto& p : *pts) {
                const auto t = get_num(p, "ambientTemperature");
                const auto id_a = get_num(p, "continuousDrainCurrent");
                if (!id_a) continue;
                if (!t) throw std::runtime_error(*mpn + ": an ambientRatings point states a drain "
                                                 "current at no ambient temperature");
                const auto vg = get_num(p, "gateSourceVoltage");
                auto it = std::find_if(g.begin(), g.end(), [&](const auto& e) { return e.first == vg; });
                if (it == g.end()) {
                    g.push_back({vg, {}});
                    it = g.end() - 1;
                }
                it->second.emplace_back(*t, std::fabs(*id_a));
            }
            for (auto& [vg, xy] : g) {
                std::stable_sort(xy.begin(), xy.end(),
                                 [](const auto& l, const auto& rr) { return l.first < rr.first; });
                CurvePts c = curve_pts(xy);
                if (c.empty()) continue;  // one ambient point, or two at one temperature
                if (auto d = get_num(a, "duration")) set_condition(c, "duration", *d);
                if (vg) set_condition(c, "gateSourceVoltage", *vg);
                c.cond_text = text;
                r.curves.mut().id_ambient_curves.push_back(std::move(c));
            }
        }
    }
    auto status = get_str(*mi, "status");
    r.is_production = status.has_value() && *status == "production";
    r.datasheet_usable = !datasheet_unusable(get_str(*mi, "datasheetUrl").value_or(""));
    const json* thermal = obj_get(*di, "thermal");
    if (thermal) {
        auto rja = get_num(*thermal, "thermalResistanceJunctionAmbient");
        if (rja && *rja > 0) r.rth_ja = *rja;
        auto rjc = get_num(*thermal, "thermalResistanceJunctionCase");
        if (rjc && *rjc > 0) r.rth_jc = *rjc;
        auto tj = get_num(*thermal, "junctionTemperatureMax");
        if (tj) r.tj_max = *tj;
    }
    return r;
}

std::optional<DiodeRow> extract_diode(const json& env) {
    const json* semi = obj_get(env, "semiconductor");
    if (!semi) return std::nullopt;
    const json* d = obj_get(*semi, "diode");
    if (!d) return std::nullopt;
    const json* mi = obj_get(*d, "manufacturerInfo");
    if (!mi) return std::nullopt;
    const json* di = obj_get(*mi, "datasheetInfo");
    if (!di) return std::nullopt;
    const json empty = json::object();
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;
    const json* elec = obj_get(*di, "electrical");
    // A multi-diode package whose diodes are described one by one carries `dies` (SAS
    // bcc7a93: one subType + electrical block per diode) INSTEAD of a package-level block.
    // Read exactly as extract_mosfet reads MOSFET dies (owner 2026-11-13): when every die
    // states the SAME diode — subType, electrical block and any die thermal block identical
    // — that die's values are the part's, per diode, as for a dual with one block; dies that
    // differ (a steering pair beside a different clamp) give the part no single V_F, V_RWM
    // or I_PP and the record is left unread rather than given one die's numbers.
    std::optional<std::string> die_subtype;
    std::optional<double> die_count;
    if (!elec) {
        const json* dies = obj_get(*di, "dies");
        if (!dies || !dies->is_array() || dies->size() < 2) return std::nullopt;
        const json& first = (*dies)[0];
        const json* e0 = obj_get(first, "electrical");
        auto st0 = get_str(first, "subType");
        if (!e0 || !st0) return std::nullopt;
        const json* t0 = obj_get(first, "thermal");
        for (const auto& d : *dies) {
            const json* e = obj_get(d, "electrical");
            auto st = get_str(d, "subType");
            const json* t = obj_get(d, "thermal");
            if (!e || !st || *e != *e0 || *st != *st0) return std::nullopt;
            if ((t == nullptr) != (t0 == nullptr) || (t && *t != *t0)) return std::nullopt;
        }
        elec = e0;
        die_subtype = st0;
        die_count = static_cast<double>(dies->size());
    }

    auto mpn = get_str(*mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    auto manuf = get_str(*mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    // Diode subtypes are characterised by DIFFERENT parameters, and demanding the
    // rectifier set from all of them silently emptied the catalogue of whole families:
    // 8,173 of 8,274 zeners, 2,025 of 2,025 TVS and 720 of 720 ESD parts were dropped
    // as "unreadable", so kelvin.openconverters.com offered no transient-suppression or
    // ESD-protection parts at all (ABT #423). Measured over TAS diodes.ndjson:
    //   zener    carries breakdownVoltage (its Vz); only 101/8,274 carry reverseVoltage
    //   tvs/esd  carry standoffVoltage; NEITHER carries forwardCurrent at all
    // So take each subtype's own reverse-direction rating, and require forward
    // parameters only where the subtype actually has them.
    const std::string sub = die_subtype ? *die_subtype : get_str(*part, "subType").value_or("");
    const bool rectifier_like = (sub != "zener" && sub != "tvs" && sub != "esd");

    const bool protection = (sub == "tvs" || sub == "esd");
    // A protection diode's working rating is its STANDOFF voltage, and only that. Reading
    // reverseVoltage first took the BREAKDOWN maximum some importers wrote there: P6KE180A
    // (standoff 180 V) reached the ranker as a 221 V part and passed a 210 V original's
    // check while already conducting at 200 V (ABT #2047). Every tvs/esd record states a
    // standoff; one that does not is unreadable, not read from another field.
    //
    // A zener's working rating is its Vz, the breakdownVoltage, and only that — the same
    // rule. resolve_field, NOT get_num: a zener's breakdownVoltage is a
    // dimensionWithTolerance ({minimum, nominal, maximum} — Vz is a graded range, so all
    // of them are objects), and get_num reads none of it. Reading reverseVoltage first
    // (72 of 13,881 zeners state one) took the reverse-LEAKAGE test voltage instead:
    // BZX55C3.3 (Vz 3.135-3.465 V) reached the ranker as a 4.95 V part. Now that the
    // ranker compares a zener's Vz to the original's window, that would reject the
    // genuine equivalent and admit a wrong one (Qarlos 2026-11-08). A zener stating no
    // breakdown voltage is unreadable, not read from another field.
    std::optional<double> vrrm = protection       ? resolve_field(*elec, "standoffVoltage")
                                 : sub == "zener" ? resolve_field(*elec, "breakdownVoltage")
                                                  : get_num(*elec, "reverseVoltage");
    auto if_avg = get_num(*elec, "forwardCurrent");
    auto vf = get_num(*elec, "forwardVoltage");
    if (!pos(vrrm)) return std::nullopt;
    if (rectifier_like && (!pos(if_avg) || !pos(vf))) return std::nullopt;

    // Leave both ABSENT (NaN) when the record does not state them. The `: 0.0` this
    // replaces wrote a zero for a MISSING key, so a record carrying no recovery data at
    // all — 15,165 of the 17,888 of them — reported an ideal diode, zero recovery charge
    // and zero recovery time, in the row the user is shown, while every other absent
    // field on that same row read null (ABT #489).
    //
    // NOT `pos`, which is what the capacitor ESR and the MOSFET Coss use: a stated 0 is
    // real here. 260 records state Qrr = 0 and 257 of them are sicSchottky (the other 3
    // schottky) — a majority-carrier device stores no minority charge, and the physics
    // validator flags a NON-zero Qrr on one as suspicious. So keep what the record says,
    // including a zero, and let only silence be silence. A negative is unreadable, not a
    // value, and stays absent.
    auto qrr_o = get_num(*elec, "reverseRecoveryCharge");
    auto trr_o = get_num(*elec, "reverseRecoveryTime");

    DiodeRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    fill_dimensions(r, *di, *part);
    r.vrrm_rated = *vrrm;
    // NaN, not 0: a zener publishes no average forward current, and "0 A" is a
    // plausible-looking number where NaN is unmistakably absent. Same convention the
    // thermal fields in this struct already use.
    r.if_avg_rated = pos(if_avg) ? *if_avg : kNaN();
    r.vf_typ = pos(vf) ? *vf : kNaN();
    if (qrr_o && *qrr_o >= 0) r.qrr = *qrr_o;
    if (trr_o && *trr_o >= 0) r.trr = *trr_o;
    // The GUARANTEED breakdown window, alongside the nominal resolve_field just took.
    // The nominal is what the part is marked; the window is what it does — an A-grade
    // zener and the B grade of the same nominal share vrrm_rated and are different parts
    // (ABT #488). Both bounds are required for a tolerance: half a band is not one.
    if (const json* bv = obj_get(*elec, "breakdownVoltage"); bv && bv->is_object()) {
        auto lo = get_num(*bv, "minimum"), hi = get_num(*bv, "maximum");
        if (pos(lo)) r.vz_min = *lo;
        if (pos(hi)) r.vz_max = *hi;
        // Half-width over midpoint, not over the nominal: 28 records state a band with no
        // nominal to divide by, 253 state a band the nominal does not sit in the middle
        // of, and the width of the window is what the two bounds themselves say. A record
        // whose bounds are inverted states no window we can read.
        if (pos(lo) && pos(hi) && *hi >= *lo) r.vz_tolerance = (*hi - *lo) / (*hi + *lo);
    }
    r.technology = sub;
    // The die configuration the record STATES, verbatim; absent stays "" (unknown to the
    // ranker), never filled from the part number or the package.
    if (auto v = get_str(*part, "dieConfiguration")) r.die_configuration = *v;
    if (auto v = get_str(*part, "internalConnection")) r.internal_connection = *v;
    // How many diodes (v22): the count the identical-diode block states, else the number of
    // dies read above. Only an integer >= 2 is a count (SAS minimum); anything else is
    // unreadable and stays NaN, never rounded into one.
    if (auto n = get_num(*elec, "numberOfElements"); n && *n >= 2 && *n == std::floor(*n))
        r.number_of_elements = *n;
    else if (die_count)
        r.number_of_elements = *die_count;
    // Continuous dissipation rating. Absent stays NaN; a 0 or negative is unreadable.
    if (auto v = get_num(*elec, "powerDissipation"); pos(v)) r.power_dissipation = *v;
    // The surge / clamp / capacitance specs, each with the impulse it was stated at
    // (ABT #2036, #2040, #2041, #2048-#2051). Absent stays NaN / "", never a default.
    if (auto v = get_num(*elec, "clampingVoltage"); pos(v)) r.clamping_voltage = *v;
    if (auto v = get_num(*elec, "clampingCurrent"); pos(v)) r.clamping_current = *v;
    if (auto v = get_num(*elec, "peakPulsePower"); pos(v)) r.peak_pulse_power = *v;
    if (auto v = get_num(*elec, "peakPulseCurrent"); pos(v)) r.peak_pulse_current = *v;
    if (auto v = get_num(*elec, "junctionCapacitance"); pos(v)) r.junction_capacitance = *v;
    // Its bias basis (v21): 0 V is a real basis, so not `pos`; a negative is unreadable.
    if (auto v = get_num(*elec, "junctionCapacitanceVr"); v && *v >= 0.0)
        r.junction_capacitance_vr = *v;
    // The Vf test current (v22): a current, so only a positive value is a basis.
    if (auto v = get_num(*elec, "forwardVoltageAt"); pos(v)) r.vf_test_current = *v;
    // Reverse leakage (owner 2026-11-14, operating-point loss): a stated positive current only.
    if (auto v = get_num(*elec, "reverseLeakageCurrent"); pos(v)) r.reverse_leakage = *v;
    // Distributor unit costs (owner 2026-11-14), read exactly as extract_mosfet reads them:
    // only a stated positive value with a currency.
    if (const json* dis = obj_get(*d, "distributorsInfo"); dis && dis->is_array()) {
        json quotes = json::array();
        for (const json& q0 : *dis) {
            if (!q0.is_object()) continue;
            const json* cost = obj_get(q0, "cost");
            if (!cost || !cost->is_object()) continue;
            auto v = get_num(*cost, "value");
            auto cur = get_str(*cost, "currency");
            if (!pos(v) || !cur || cur->empty()) continue;
            json q = {{"distributor", get_str(q0, "name").value_or("")},
                      {"unitCost", *v},
                      {"currency", *cur}};
            if (auto moq = get_num(q0, "moq"); pos(moq)) q["moq"] = *moq;
            quotes.push_back(std::move(q));
        }
        if (!quotes.empty()) r.price_quotes = quotes.dump();
    }
    // Measured curves (owner rule 2026-11-14), as stated; compared by CrossRefCurveChecks.hpp.
    r.curves.mut().vf_curve = read_named_curve(*di, "forwardVoltageVsCurrent", *mpn);
    r.curves.mut().ir_curve = read_named_curve(*di, "reverseCurrentVsVoltage", *mpn);
    r.curves.mut().cj_curve = read_named_curve(*di, "capacitanceVsVoltage", *mpn);
    {
        // Unlabelled curves: checked against the record's own scalar at its stated condition.
        auto opt = [](double v) { return present(v) ? std::optional<double>(v) : std::nullopt; };
        const UnitCheck vf{opt(r.vf_typ), opt(r.vf_test_current), true, "Vf", "V", 1.0, "A"};
        const UnitCheck cj{opt(r.junction_capacitance), opt(r.junction_capacitance_vr), false,
                           "junction capacitance", "pF", 1e12, "V"};
        settle_unit_unstated(r.curves.mut().vf_curve, &vf);
        // Ir(Vr): the record's reverse leakage states no reverse voltage on record — not checked.
        settle_unit_unstated(r.curves.mut().ir_curve, nullptr);
        settle_unit_unstated(r.curves.mut().cj_curve, &cj);
    }
    r.clamping_waveform = get_str(*elec, "clampingWaveform").value_or("");
    r.peak_pulse_power_waveform = get_str(*elec, "peakPulsePowerWaveform").value_or("");
    r.peak_pulse_current_waveform = get_str(*elec, "peakPulseCurrentWaveform").value_or("");
    // Verbatim; "" = not stated (the ranker reads that as an UNKNOWN grade, never commercial).
    r.qualification = get_str(*part, "qualification").value_or("");
    auto status = get_str(*mi, "status");
    r.is_production = status.has_value() && *status == "production";
    const json* thermal = obj_get(*di, "thermal");
    if (thermal) {
        auto rja = get_num(*thermal, "thermalResistanceJunctionAmbient");
        if (rja && *rja > 0) r.rth_ja = *rja;
        auto rjc = get_num(*thermal, "thermalResistanceJunctionCase");
        if (rjc && *rjc > 0) r.rth_jc = *rjc;
        auto tj = get_num(*thermal, "junctionTemperatureMax");
        if (tj) r.tj_max = *tj;
    }
    return r;
}

// Every record nests its manufacturerInfo one or two levels down ({"capacitor": {…}},
// {"semiconductor": {"diode": {…}}}); find it without knowing the family.
static const json* find_manufacturer_info(const json& o, int depth) {
    if (!o.is_object() || depth > 2) return nullptr;
    if (const json* mi = obj_get(o, "manufacturerInfo")) return mi;
    for (const auto& [k, v] : o.items())
        if (const json* mi = find_manufacturer_info(v, depth + 1)) return mi;
    return nullptr;
}

bool all_provenance_retracted(const json& env) {
    const json* mi = find_manufacturer_info(env, 0);
    const json* di = mi ? obj_get(*mi, "datasheetInfo") : nullptr;
    const json* pv = di ? obj_get(*di, "provenance") : nullptr;
    if (!pv || !pv->is_array() || pv->empty()) return false;
    for (const json& e : *pv) {
        const json* r = e.is_object() ? obj_get(e, "retracted") : nullptr;
        if (!r || !r->is_boolean() || !r->get<bool>()) return false;
    }
    return true;
}

std::string all_provenance_retraction_reason(const json& env) {
    if (!all_provenance_retracted(env)) return "";
    const json* mi = find_manufacturer_info(env, 0);
    const json* pv = obj_get(*obj_get(*mi, "datasheetInfo"), "provenance");
    std::vector<std::string> reasons;
    for (const json& e : *pv) {
        const std::string r = get_str(e, "retractionReason").value_or("");
        if (!r.empty() && std::find(reasons.begin(), reasons.end(), r) == reasons.end())
            reasons.push_back(r);
    }
    std::string out;
    for (const std::string& r : reasons) out += (out.empty() ? "" : "; ") + r;
    return out;
}

namespace {
// A provenance `fields` entry, in the record-relative form the walk below produces. The corpus
// writes the same field several ways: "electrical.forwardCurrent",
// "datasheetInfo.electrical.breakdownVoltage", "manufacturerInfo.datasheetInfo.electrical.x",
// and with an array index for an array-valued block ("electrical.0.inductance").
std::string normalize_provenance_path(std::string p) {
    for (const char* pre : {"manufacturerInfo.datasheetInfo.", "datasheetInfo."})
        if (p.rfind(pre, 0) == 0) p = p.substr(std::char_traits<char>::length(pre));
    std::string out, seg;
    auto flush = [&] {
        const bool index = !seg.empty() && std::all_of(seg.begin(), seg.end(), [](char c) {
            return std::isdigit(static_cast<unsigned char>(c)) != 0;
        });
        if (!seg.empty() && !index) out += (out.empty() ? "" : ".") + seg;
        seg.clear();
    };
    for (char c : p) {
        if (c == '.') flush();
        else seg += c;
    }
    flush();
    return out;
}
// `scope` ("" = the whole record) covers `path` when it is the path, a parent of it, or a part
// of it (a scope deeper than the leaf names part of that leaf's value).
bool scope_covers(const std::string& scope, const std::string& path) {
    if (scope.empty() || scope == path) return true;
    if (path.size() > scope.size())
        return path.compare(0, scope.size(), scope) == 0 && path[scope.size()] == '.';
    return scope.compare(0, path.size(), path) == 0 && scope[path.size()] == '.';
}
// Every stated leaf of datasheetInfo (provenance itself excluded), array indices dropped.
void stated_leaves(const json& v, const std::string& path, std::vector<std::string>& out) {
    if (v.is_null()) return;
    if (v.is_object()) {
        for (const auto& [k, sub] : v.items()) {
            if (path.empty() && k == "provenance") continue;
            stated_leaves(sub, path.empty() ? k : path + "." + k, out);
        }
        return;
    }
    if (v.is_array()) {
        bool scalar = true;
        for (const json& e : v)
            if (e.is_object() || e.is_array()) scalar = false;
        if (scalar) {
            if (!v.empty()) out.push_back(path);
            return;
        }
        for (const json& e : v) stated_leaves(e, path, out);
        return;
    }
    out.push_back(path);
}
}  // namespace

namespace {
bool field_retracted(const RowBase& r, const char* field) {
    return std::find(r.retracted_fields.begin(), r.retracted_fields.end(), field) !=
           r.retracted_fields.end();
}
}  // namespace

int unstate_retracted_ratings(DiodeRow& r) {
    // The field the rating was read from (extract_diode: one source per subtype, no fallback).
    const bool protection = r.technology == "tvs" || r.technology == "esd";
    const char* src = protection                ? "electrical.standoffVoltage"
                      : r.technology == "zener" ? "electrical.breakdownVoltage"
                                                : "electrical.reverseVoltage";
    int n = 0;
    if (field_retracted(r, src)) {
        r.vrrm_rated = kNaN();
        ++n;
    }
    // A zener's window is its rating too (the zener pool's range filter); a TVS's breakdown
    // window is a secondary axis and stays.
    if (r.technology == "zener" && field_retracted(r, "electrical.breakdownVoltage") &&
        (!std::isnan(r.vz_min) || !std::isnan(r.vz_max) || !std::isnan(r.vz_tolerance))) {
        r.vz_min = r.vz_max = r.vz_tolerance = kNaN();
        ++n;
    }
    return n;
}

int unstate_retracted_ratings(MosfetRow& r) {
    if (!field_retracted(r, "electrical.drainSourceVoltage")) return 0;
    r.vds_rated = kNaN();
    return 1;
}

int unstate_retracted_ratings(CapacitorRow& r) {
    int n = 0;
    if (!std::isnan(r.v_rated) && field_retracted(r, "electrical.ratedVoltage")) {
        r.v_rated = kNaN();
        ++n;
    }
    if (!std::isnan(r.v_rated_ac) && field_retracted(r, "electrical.voltageRatedAcMax")) {
        r.v_rated_ac = kNaN();
        ++n;
    }
    // The primary value (owner 2026-11-13, C), with the guaranteed band read from the same field.
    if (present(r.capacitance) && field_retracted(r, "electrical.capacitance")) {
        r.capacitance = r.capacitance_min = r.capacitance_max = kNaN();
        ++n;
    }
    return n;
}

int unstate_retracted_ratings(ResistorRow& r) {
    if (!present(r.resistance) || !field_retracted(r, "electrical.resistance")) return 0;
    r.resistance = kNaN();
    return 1;
}

int unstate_retracted_ratings(VaristorRow& r) {
    if (!present(r.varistor_voltage) || !field_retracted(r, "electrical.varistorVoltage")) return 0;
    r.varistor_voltage = kNaN();
    return 1;
}

int unstate_retracted_ratings(MagneticRow& r) {
    int n = 0;
    // electrical is an array; provenance paths drop the index (retracted_only_fields).
    if (present(r.inductance) && field_retracted(r, "electrical.inductance")) {
        r.inductance = r.inductance_tol_minus_pct = r.inductance_tol_plus_pct = kNaN();
        ++n;
    }
    // A chip bead's primary value is its impedance (primary_value_spec("chipBead")), read from
    // the measured curve: Z@100 MHz and the bracketed peak.
    if ((present(r.impedance_100mhz) || present(r.impedance_peak)) &&
        field_retracted(r, "electrical.impedancePoints")) {
        r.impedance_100mhz = r.impedance_peak = r.impedance_peak_freq = kNaN();
        ++n;
    }
    for (auto [path, c] : {std::pair{"electrical.resistancePoints", &r.curves.mut().r_curve},
                           std::pair{"electrical.reactancePoints", &r.curves.mut().x_curve},
                           std::pair{"electrical.impedancePoints", &r.curves.mut().z_curve}})
        if (!c->empty() && field_retracted(r, path)) {
            *c = CurvePts{};
            ++n;
        }
    // A CMC's DM readings come from differentialModeImpedancePoints, or from the
    // impedancePoints tagged "differential" when that field is absent: either retraction
    // unstates them.
    if ((present(r.dm_impedance_100mhz) || present(r.dm_impedance_peak)) &&
        (field_retracted(r, "electrical.impedancePoints") ||
         field_retracted(r, "electrical.differentialModeImpedancePoints"))) {
        r.dm_impedance_100mhz = r.dm_impedance_peak = r.dm_impedance_peak_freq = kNaN();
        ++n;
    }
    if (!r.curves.mut().dm_z_curve.empty() &&
        (field_retracted(r, "electrical.impedancePoints") ||
         field_retracted(r, "electrical.differentialModeImpedancePoints"))) {
        r.curves.mut().dm_z_curve = CurvePts{};
        ++n;
    }
    for (auto [path, c] :
         {std::pair{"electrical.commonModeInsertionLossPoints", &r.curves.mut().cm_il_curve},
          std::pair{"electrical.differentialModeInsertionLossPoints", &r.curves.mut().dm_il_curve}})
        if (!c->empty() && field_retracted(r, path)) {
            *c = CurvePts{};
            ++n;
        }
    return n;
}

int unstate_retracted_ratings(TimingRow& r) {
    int n = 0;
    if (present(r.frequency) && field_retracted(r, "electrical.frequency")) {
        r.frequency = kNaN();
        ++n;
    }
    // A programmable blank's frequency datum (owner 2026-11-13: refused without it).
    if (present(r.frequency_min) && field_retracted(r, "electrical.frequencyRange")) {
        r.frequency_min = r.frequency_max = kNaN();
        ++n;
    }
    return n;
}

int unstate_retracted_ratings(ConnectorRow& r) {
    if (!present(r.positions) || !field_retracted(r, "mechanical.positions")) return 0;
    r.positions = kNaN();
    return 1;
}

FieldRetraction retracted_only_fields(const json& env) {
    FieldRetraction out;
    const json* mi = find_manufacturer_info(env, 0);
    const json* di = mi ? obj_get(*mi, "datasheetInfo") : nullptr;
    const json* pv = di ? obj_get(*di, "provenance") : nullptr;
    if (!pv || !pv->is_array()) return out;
    struct Scope {
        std::string path;
        std::string reason;
    };
    std::vector<Scope> retracted;
    std::vector<std::string> live;
    bool any_live = false;
    for (const json& e : *pv) {
        if (!e.is_object()) continue;
        const json* r = obj_get(e, "retracted");
        const bool is_retracted = r && r->is_boolean() && r->get<bool>();
        const json* f = obj_get(e, "fields");
        const bool scoped = f && f->is_array() && !f->empty();
        if (!is_retracted) {
            any_live = true;
            if (!scoped) return out;  // a live entry for the whole record covers every field
            for (const json& p : *f)
                if (p.is_string()) live.push_back(normalize_provenance_path(p.get<std::string>()));
            continue;
        }
        const std::string reason = get_str(e, "retractionReason").value_or("");
        if (!scoped) {
            retracted.push_back({"", reason});
            continue;
        }
        for (const json& p : *f)
            if (p.is_string())
                retracted.push_back({normalize_provenance_path(p.get<std::string>()), reason});
    }
    if (retracted.empty() || !any_live) return out;  // nothing retracted, or all of it
    std::vector<std::string> leaves;
    stated_leaves(*di, "", leaves);
    std::vector<std::string> reasons;
    for (const std::string& leaf : leaves) {
        const Scope* by = nullptr;
        for (const Scope& s : retracted)
            if (scope_covers(s.path, leaf)) {
                by = &s;
                break;
            }
        if (!by) continue;
        bool live_cover = false;
        for (const std::string& s : live)
            if (scope_covers(s, leaf)) {
                live_cover = true;
                break;
            }
        if (live_cover) continue;
        const size_t dot = leaf.find('.');
        const size_t dot2 = dot == std::string::npos ? dot : leaf.find('.', dot + 1);
        const std::string field = dot2 == std::string::npos ? leaf : leaf.substr(0, dot2);
        if (std::find(out.fields.begin(), out.fields.end(), field) == out.fields.end())
            out.fields.push_back(field);
        if (!by->reason.empty() &&
            std::find(reasons.begin(), reasons.end(), by->reason) == reasons.end())
            reasons.push_back(by->reason);
    }
    std::sort(out.fields.begin(), out.fields.end());
    for (const std::string& r : reasons) out.reason += (out.reason.empty() ? "" : "; ") + r;
    return out;
}

std::optional<CapacitorRow> extract_capacitor(const json& env) {
    const json* cap = obj_get(env, "capacitor");
    if (!cap) return std::nullopt;
    const json* mi = obj_get(*cap, "manufacturerInfo");
    if (!mi) return std::nullopt;
    const json* di = obj_get(*mi, "datasheetInfo");
    if (!di) return std::nullopt;
    const json* elec = obj_get(*di, "electrical");
    if (!elec) return std::nullopt;
    const json empty = json::object();
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;

    auto mpn = get_str(*mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    auto manuf = get_str(*mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    auto cap_nom = resolve_field(*elec, "capacitance");
    // A line-safety (X/Y) or AC film capacitor may be rated for AC only: CAS accepts
    // voltageRatedAcMax in place of ratedVoltage. Such a part has no DC rating, so v_rated
    // stays ABSENT (NaN) and the DC selector refuses it under its own reason — but it is a
    // real part to browse and cross-reference, not an unreadable row.
    auto v_rated = get_num(*elec, "ratedVoltage");
    auto v_rated_ac = get_num(*elec, "voltageRatedAcMax");
    if (!pos(cap_nom) || !(pos(v_rated) || pos(v_rated_ac))) return std::nullopt;

    // Leave both ABSENT (NaN) when the record does not state them. `>= 0` used to accept
    // a stated 0 as real and substitute 0.0 for a missing one, which are the two ways to
    // land on the same impossible value from opposite causes. `pos` treats a stated 0 as
    // not-stated too, which is what it is: no capacitor has zero ESR.
    auto ripple_o = get_num(*elec, "rippleCurrent");
    auto esr_o = get_num(*elec, "esr");
    // ESL lives in the equivalent circuit, not the electrical block: CAS
    // models a capacitor as rs/cs/ls/riso and `ls` IS the series inductance.
    std::optional<double> esl_o;
    if (auto mp = di->find("modelParams"); mp != di->end() && mp->is_object())
        esl_o = get_num(*mp, "ls");

    CapacitorRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    // An AXIAL body lies on the board with its axis parallel to it: CAS states its body as
    // dimensions.length (the axis, lead exit to lead exit) and dimensions.diameter, so the
    // diameter is its width and its height and the length stays the length — the resistor rule
    // (ABT #2032). Read as an upright can, A50IQ4150AA60K (28 mm long, 10 mm diameter) became
    // a 10 x 10 mm footprint standing 28 mm tall and C114K222K1X5CA lost its 4.06 mm length
    // (Qarlos 2026-11-09, seeds 21413128 / 21622586). CAS has no axial flag; the record says
    // it in mechanical.shape.shapeType ("Axial", "Axial Leaded", "Axial Hermetic", "Axial
    // Cylindrical", "Axial-lead", ...), so a shapeType naming "axial" is the evidence. Any
    // other shape keeps the upright-can fold.
    // A shapeType naming "disc" ("Disc", "Radial Disc") is the same kind of evidence for an
    // upright disc: footprint diameter x thickness, height the diameter (fill_dimensions).
    bool axial = false, disc = false;
    if (const json* mech = obj_get(*di, "mechanical"))
        if (const json* shape = obj_get(*mech, "shape"))
            if (auto st = get_str(*shape, "shapeType")) {
                std::string l = *st;
                for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                axial = l.find("axial") != std::string::npos;
                disc = !axial && l.find("disc") != std::string::npos;
            }
    fill_dimensions(r, *di, *part, /*lies_flat=*/axial, /*upright_disc=*/disc);
    r.capacitance = *cap_nom;
    if (pos(v_rated)) r.v_rated = *v_rated;
    if (pos(v_rated_ac)) r.v_rated_ac = *v_rated_ac;
    if (pos(ripple_o)) r.ripple_current_rms = *ripple_o;
    if (pos(esr_o)) r.esr = *esr_o;
    if (pos(esl_o)) r.esl = *esl_o;
    // Technology is `part.technology` — a real comparable class
    // ("ceramic-class-2", "aluminum-electrolytic-wet", "film-polypropylene"),
    // present on every record. The family/subType/series chain below is only a
    // fallback: those hold a VENDOR SERIES ("GRM1885"), which is not a
    // technology, is never equal across manufacturers, and made both the
    // catalogue facet and the cross-reference same-technology filter useless.
    std::string tech = get_str(*part, "technology").value_or("");
    if (tech.empty()) tech = get_str(*part, "family").value_or("");
    if (tech.empty()) tech = get_str(*part, "subType").value_or("");
    if (tech.empty()) tech = get_str(*part, "series").value_or("");
    // Dielectric code (X7R / C0G / X5R …) drives the class-equivalence gate in
    // the cross-reference ranker: swapping C0G for X5R is a real regression.
    r.dielectric_code = get_str(*part, "dielectricCode").value_or("");
    // The series name, kept verbatim — it is where the line-safety approval lives
    // (X1/X2/X3, Y1/Y2/Y3/Y4) for a mains capacitor, and nothing else in the record
    // states one. WIMA writes it in both slots ("MKP-X1 R"), KEMET into `family`
    // ("R47 X1 440 VAC"), so family first with series as the fallback — the same
    // chain the resistor device class uses.
    r.family = get_str(*mi, "family").value_or("");
    if (r.family.empty()) r.family = get_str(*part, "series").value_or("");
    if (auto v = get_num(*elec, "esrFrequency"); pos(v)) r.esr_frequency = *v;
    // The frequency the scalar ripple rating is stated AT (v20). Only alongside a ripple
    // value: a frequency with nothing quoted at it is not a basis for anything. Never
    // inferred from the series, the technology or a "typical" 100/120 Hz.
    if (present(r.ripple_current_rms))
        if (auto v = get_num(*elec, "rippleCurrentFrequency"); pos(v))
            r.ripple_current_frequency = *v;
    // The temperature the scalar ripple rating is stated AT (v22), the same way: only
    // alongside a ripple value, never inferred from the temperature range or the series.
    if (present(r.ripple_current_rms))
        if (auto v = get_num(*elec, "rippleCurrentTemperature"); v && std::isfinite(*v))
            r.ripple_current_temperature = *v;
    // The dissipation factor tan(delta) and the frequency it is stated AT (v22, owner
    // 2026-11-14): the frequency only alongside a stated factor, never a "typical" 120 Hz.
    if (auto v = get_num(*elec, "dissipationFactor"); pos(v)) {
        r.dissipation_factor = *v;
        if (auto f = get_num(*elec, "dissipationFactorFrequency"); pos(f))
            r.dissipation_factor_frequency = *f;
    }
    // The IEC 60384-14 safety class the record states (v20), verbatim. Absent or null stays
    // "" (not stated); the explicit token "none" is a stated fact and is kept as such.
    if (const json* sc = obj_get(*part, "safetyClass"); sc && sc->is_object())
        r.safety_class = get_str(*sc, "class").value_or("");
    // The guaranteed capacitance band (v20), in farads as stated. Kept only when the record
    // states BOTH ends and they bracket the nominal with a non-zero width: a lone end, or a
    // "band" of zero width, is not a tolerance statement, and nothing is inferred from the
    // MPN tolerance letter or a typical band.
    if (const json* cj = obj_get(*elec, "capacitance"); cj && cj->is_object()) {
        auto lo = get_num(*cj, "minimum"), hi = get_num(*cj, "maximum");
        if (lo && hi && *lo > 0 && *lo <= r.capacitance && *hi >= r.capacitance && *hi > *lo) {
            r.capacitance_min = *lo;
            r.capacitance_max = *hi;
        }
    }
    // The leaded form (v20), from the shapeType words the vendors write ("Axial", "Axial
    // Leaded", "Axial Hermetic", "Radial", "Radial Boxed", "Radial Disc", "Radial Molded",
    // "Resin dipped & radial leaded type", ...). A shapeType naming neither ("Box", "SMD
    // Chip", "Single-ended") leaves it "" — never guessed from the series or the technology.
    if (const json* mech = obj_get(*di, "mechanical"))
        if (const json* shape = obj_get(*mech, "shape"))
            if (auto st = get_str(*shape, "shapeType")) {
                std::string l = *st;
                for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const bool ax = l.find("axial") != std::string::npos;
                const bool rad = l.find("radial") != std::string::npos;
                if (ax != rad) r.lead_form = ax ? "axial" : "radial";
            }
    // The qualified reliability level (v20, PEAS qualifiedReliability on part), verbatim.
    if (const json* qr = obj_get(*part, "qualifiedReliability"); qr && qr->is_object()) {
        r.qualified_spec = get_str(*qr, "specification").value_or("");
        if (!r.qualified_spec.empty()) {
            r.qualified_grade = get_str(*qr, "grade").value_or("");
            if (auto fr = get_num(*qr, "failureRate"); fr && *fr >= 0)
                r.qualified_failure_rate = *fr;
        }
    }
    // No scalar ESR, but a measured ESR-vs-frequency curve: read it AT 100 kHz, the
    // frequency the catalogue's scalar ESRs are overwhelmingly quoted at, and say so in
    // esr_frequency. TDK publishes the curve and no scalar, so every TDK candidate reached
    // the ranker with "no ESR" while its record carried 325 measured points (ABT #2031).
    // Only an INTERPOLATION between two samples that bracket 100 kHz is taken — never an
    // extrapolation — and only from a curve whose labels, when present, say Hz and ohm.
    // The whole curve is kept too (v22, owner 2026-11-14): the ranker compares two ESR curves
    // on their overlap and reads one at the other side's stated ESR frequency.
    r.curves.mut().esr_curve = read_curve_xy(obj_get(*elec, "esrPoints"), "[Hz]", {"Ohm", "\u03a9"},
                                /*positive_y=*/true);
    r.curves.mut().bias_curve = read_bias_curve(obj_get(*elec, "capacitanceBiasPoints"));
    r.curves.mut().z_curve = read_curve_xy(obj_get(*elec, "impedancePoints"), "[Hz]", {"Ohm", "\u03a9"},
                              /*positive_y=*/true);
    r.curves.mut().cf_curve = leading_positive(read_curve_xy(obj_get(*elec, "capacitanceFrequencyPoints"),
                                                "[Hz]", {"[F]"}, /*positive_y=*/false));
    r.curves.mut().esl_curve = read_curve_xy(obj_get(*elec, "inductanceFrequencyPoints"), "[Hz]", {"[H]"},
                                /*positive_y=*/true);
    // The ripple rating's own frequency and temperature tables (v22, owner 2026-11-14), kept
    // only alongside a ripple rating and only when evidently that rating's (read_ripple_table).
    // Frequency: electrical.rippleCurrentFrequencyPoints (multiplier), else
    // rippleCurrentPoints (amperes vs Hz).
    if (present(r.ripple_current_rms)) {
        r.curves.mut().ripple_f_curve = read_ripple_table(obj_get(*elec, "rippleCurrentFrequencyPoints"), "Hz",
                                             r.ripple_current_frequency, r.ripple_current_rms,
                                             "temperature", r.ripple_current_temperature,
                                             /*log_x=*/true);
        if (r.curves.mut().ripple_f_curve.empty())
            r.curves.mut().ripple_f_curve = read_ripple_table(obj_get(*elec, "rippleCurrentPoints"), "Hz",
                                                 r.ripple_current_frequency, r.ripple_current_rms,
                                                 "temperature", r.ripple_current_temperature,
                                                 /*log_x=*/true);
        r.curves.mut().ripple_t_curve = read_ripple_table(obj_get(*elec, "rippleCurrentTemperaturePoints"),
                                             "emperature", r.ripple_current_temperature,
                                             r.ripple_current_rms, "frequency",
                                             r.ripple_current_frequency, /*log_x=*/false);
    }
    // Distributor unit costs (owner 2026-11-14, capacitor selector): only a stated positive
    // value with a currency, as for MOSFETs.
    if (const json* dis = obj_get(*cap, "distributorsInfo"); dis && dis->is_array()) {
        json quotes = json::array();
        for (const json& d : *dis) {
            if (!d.is_object()) continue;
            const json* cost = obj_get(d, "cost");
            if (!cost || !cost->is_object()) continue;
            auto v = get_num(*cost, "value");
            auto cur = get_str(*cost, "currency");
            if (!pos(v) || !cur || cur->empty()) continue;
            json q = {{"distributor", get_str(d, "name").value_or("")},
                      {"unitCost", *v},
                      {"currency", *cur}};
            if (auto moq = get_num(d, "moq"); pos(moq)) q["moq"] = *moq;
            quotes.push_back(std::move(q));
        }
        if (!quotes.empty()) r.price_quotes = quotes.dump();
    }
    if (!present(r.esr)) {
        if (auto v = curve_at(obj_get(*elec, "esrPoints"), 1e5, "[Hz]", "Ohm"); v) {
            r.esr = *v;
            r.esr_frequency = 1e5;
            r.esr_from_curve = true;
        }
    }
    // Operating temperature range: a substitute must cover the original's, at
    // both ends. CAS carries it under two keys: its own `thermal.temperature` and the PEAS
    // datasheetInfoThermal `thermal.operatingTemperature`, both {minimum, maximum}. Of the
    // 604,716 published records (2026-11-08), 476,195 state a thermal block: 466,726 use
    // only `temperature`, 9,391 only `operatingTemperature`, 78 both (all 78 agree).
    // Reading only `temperature` dropped the -40..105 C range of EEEHC0J221XP, so 85 C
    // substitutes got no temperature verdict (Qarlos 2026-11-08). Each end is read from whichever key states it; when both
    // state an end and they DISAGREE, that end is left unknown — picking one would be a
    // silent choice between two contradicting datasheet claims.
    if (const json* th = obj_get(*di, "thermal")) {
        const json* a = obj_get(*th, "temperature");
        const json* b = obj_get(*th, "operatingTemperature");
        auto end = [&](const char* k) -> std::optional<double> {
            std::optional<double> va = a ? get_num(*a, k) : std::nullopt;
            std::optional<double> vb = b ? get_num(*b, k) : std::nullopt;
            if (va && vb) return (*va == *vb) ? va : std::nullopt;
            return va ? va : vb;
        };
        if (auto v = end("minimum")) r.temp_min_c = *v;
        if (auto v = end("maximum")) r.temp_max_c = *v;
    }
    r.technology = tech;
    auto status = get_str(*mi, "status");
    r.is_production = status.has_value() && *status == "production";
    auto rth = get_num(*elec, "thermalResistance");
    if (rth && *rth > 0) r.rth = *rth;
    return r;
}

std::optional<ResistorRow> extract_resistor(const json& env) {
    const json* res = obj_get(env, "resistor");
    if (!res) return std::nullopt;
    const json* mi = obj_get(*res, "manufacturerInfo");
    if (!mi) return std::nullopt;
    const json* di = obj_get(*mi, "datasheetInfo");
    if (!di) return std::nullopt;
    const json* elec = obj_get(*di, "electrical");
    // A network whose elements are described one by one carries `elements` (RAS 3ba21f6: one
    // electrical block per element) INSTEAD of a package-level block. Read as extract_diode
    // reads dies (owner 2026-11-13, AA2): when every element states the SAME electrical block
    // (and the same thermal block, if any) its values are the part's, per element; elements
    // that differ (a divider, a terminator) give the network no single resistance and the
    // record is left unread rather than given one element's value.
    std::optional<double> element_count;
    if (!elec) {
        const json* els = obj_get(*di, "elements");
        if (!els || !els->is_array() || els->size() < 2) return std::nullopt;
        const json& first = (*els)[0];
        const json* e0 = obj_get(first, "electrical");
        if (!e0) return std::nullopt;
        const json* t0 = obj_get(first, "thermal");
        for (const auto& el : *els) {
            const json* e = obj_get(el, "electrical");
            const json* t = obj_get(el, "thermal");
            if (!e || *e != *e0) return std::nullopt;
            if ((t == nullptr) != (t0 == nullptr) || (t && *t != *t0)) return std::nullopt;
        }
        elec = e0;
        element_count = static_cast<double>(els->size());
    }
    const json empty = json::object();
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;

    // mpn = reference or part.partNumber; manufacturer optional (defaults "")
    auto mpn = get_str(*mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    if (!mpn) return std::nullopt;

    // Zero is a RESISTANCE, not a missing one. A 0 ohm link is one of the most
    // common parts on a real board — Yageo's RC series alone ships them in
    // every case size — and rejecting it here deleted 107 of them from the
    // catalogue while their records sat in the source data, so a board using
    // one was told its part did not exist (ABT #1123).
    //
    // Absent is still rejected, by the !r_nom above: the field is either there
    // or the record is refused. So a stored row's resistance == 0 always means
    // a real zero and never "we did not know" — which matters, because the row
    // struct's own default is also 0 and callers cannot tell the two apart.
    auto r_nom = resolve_field(*elec, "resistance");
    if (!r_nom || *r_nom < 0) return std::nullopt;

    // Tolerance exactly as stated. Absent (or a non-positive number, which is no tolerance
    // band at all) stays NaN = not on record: the cross-reference grades it UNVERIFIED and
    // the selector cannot show it meets a tolerance limit. It used to become a "typical"
    // +/-5 %, which let an unstated part pass as a 5 % one (Qarlos 2026-11-08 follow-up).
    auto tol_o = get_num(*elec, "tolerance");
    double tol = (tol_o && *tol_o > 0) ? *tol_o : kNaN();
    auto pw_o = get_num(*elec, "powerRating");
    double pw = (pw_o && *pw_o > 0) ? *pw_o : 0.0;

    ResistorRow r;
    r.mpn = *mpn;
    r.manufacturer = get_str(*mi, "name").value_or("");
    // A resistor that states a diameter is an axial or MELF body, which lies on the board.
    fill_dimensions(r, *di, *part, /*lies_flat=*/true);
    r.resistance = *r_nom;
    r.tolerance = tol;
    r.power_rating = pw;
    // The device class travels with the row. Panasonic writes it into both slots
    // ("Chip Resistor Array"), YAGEO into `family` alone ("YC" / "AF_Array"), KOA and
    // Stackpole into `series` alone — so read family first and fall back to series.
    r.family = get_str(*mi, "family").value_or("");
    if (r.family.empty()) r.family = get_str(*part, "series").value_or("");
    auto status = get_str(*mi, "status");
    r.is_production = status.has_value() && *status == "production";
    // The temperature basis of the power rating (v18). A rating is the power the part carries
    // up to a stated ambient; above it the part derates along its curve. Both are carried
    // exactly as stated — absent stays NaN / empty, never a "typical" 70 degC knee.
    if (auto t = get_num(*elec, "powerRatingTemperature")) r.power_rating_temperature = *t;
    if (const json* factors = obj_get(*di, "factors")) {
        if (const json* pd = obj_get(*factors, "powerDerating")) {
            const json* ts = obj_get(*pd, "temperature");
            const json* as = obj_get(*pd, "amplitude");
            // A derating curve the ranker cannot read is a defect in the record, and reading
            // half of it would put a wrong power in front of an engineer. Loud, with the part.
            auto bad = [&](const char* why) {
                return std::runtime_error("resistor " + *mpn + ": factors.powerDerating " + why);
            };
            if (!ts || !as || !ts->is_array() || !as->is_array())
                throw bad("must carry temperature[] and amplitude[]");
            if (ts->size() != as->size() || ts->size() < 2)
                throw bad("temperature[] and amplitude[] must be equal-length with >= 2 points");
            for (size_t i = 0; i < ts->size(); ++i) {
                const json& tv = (*ts)[i];
                const json& av = (*as)[i];
                if (!tv.is_number() || tv.is_boolean() || !av.is_number() || av.is_boolean())
                    throw bad("holds a non-numeric point");
                const double ti = tv.get<double>(), ai = av.get<double>();
                if (!r.derating_temperature.empty() && !(ti > r.derating_temperature.back()))
                    throw bad("temperatures must strictly increase");
                if (!(ai >= 0.0 && ai <= 1.0)) throw bad("amplitude must lie in [0, 1]");
                r.derating_temperature.push_back(ti);
                r.derating_amplitude.push_back(ai);
            }
        }
    }
    // v19: the resistive element, its TCR, its operating range and its stated function — the
    // properties a parametric match (R, P, tolerance) cannot see and the cross-reference now
    // compares (Qarlos 2026-11-08). Survey of resistors.ndjson (578,915 records, 2026-11-08):
    // part.technology on every record, temperatureCoefficient on 520,842, thermal.
    // operatingTemperature on 465,147 (its only temperature key in RAS).
    r.technology = get_str(*part, "technology").value_or("");
    if (auto t = get_num(*elec, "temperatureCoefficient")) r.tcr = *t;
    if (const json* th = obj_get(*di, "thermal")) {
        if (const json* t = obj_get(*th, "operatingTemperature")) {
            // Straight from the object: a cold limit is negative and 0 degC is a real rating.
            if (auto v = get_num(*t, "minimum")) r.temp_min_c = *v;
            if (auto v = get_num(*t, "maximum")) r.temp_max_c = *v;
        }
    }
    std::vector<std::string> class_texts;
    for (auto s : {get_str(*mi, "family"), get_str(*part, "series"), get_str(*part, "description")})
        if (s) class_texts.push_back(*s);
    const ResistorFunction fn = resistor_function_class(r.technology, class_texts);
    r.current_sense = fn.current_sense;
    r.pulse_withstanding = fn.pulse_withstanding;
    // v21: the maximum continuous working voltage, as stated. Absent / non-positive stays NaN.
    if (auto v = get_num(*elec, "maxVoltage"); pos(v)) r.max_voltage = *v;
    // v22: the network structure the record states. Only an integer >= 2 is a count (RAS
    // minimum); anything else is unreadable and stays NaN, never rounded into one.
    if (auto n = get_num(*elec, "numberOfElements"); n && *n >= 2 && *n == std::floor(*n))
        r.number_of_elements = *n;
    else if (element_count)
        r.number_of_elements = *element_count;
    r.internal_connection = get_str(*part, "internalConnection").value_or("");
    return r;
}

// Survey of resistors.ndjson (2026-11-08) for the class text the vendors write: current
// sensing is stated as technology currentSenseShunt (10,016 records) or "Current Sensing
// Resistor(s)" in family/series/description (Panasonic ERJ-xBQ/ERJ-xDQ series, TE CGS RL73);
// pulse capability as "Anti-Surge ..." (Panasonic ERJ-P/ERJ-PA/ERJ-T, 13,063), "Anti-Pulse
// ..." (581) and "Anti Pulse Chip Resistors" (KOA SG73, 2,627 descriptions). Matching is on
// those phrases only, case- and hyphen-insensitive; a word like "surge" on its own is not
// taken as a claim.
ResistorFunction resistor_function_class(const std::string& technology,
                                         const std::vector<std::string>& class_texts) {
    ResistorFunction f;
    if (technology == "currentSenseShunt") f.current_sense = true;
    static const char* kSense[] = {"current sensing", "current sense"};
    static const char* kPulse[] = {"anti surge",      "antisurge",       "anti pulse",
                                   "antipulse",       "pulse withstand", "surge withstand",
                                   "pulse resistant", "surge resistant", "pulse proof"};
    for (const std::string& raw : class_texts) {
        std::string t;
        t.reserve(raw.size());
        for (unsigned char c : raw)
            t += (c == '-' || c == '_') ? ' ' : static_cast<char>(std::tolower(c));
        for (const char* k : kSense)
            if (t.find(k) != std::string::npos) f.current_sense = true;
        for (const char* k : kPulse)
            if (t.find(k) != std::string::npos) f.pulse_withstanding = true;
    }
    return f;
}

std::optional<ControllerRow> extract_controller(const json& env) {
    // ctrl = env["controller"] if it's an object, else env itself.
    const json* ctrl = obj_get(env, "controller");
    if (!ctrl) ctrl = &env;
    const json empty = json::object();
    const json* mi = obj_get(*ctrl, "manufacturerInfo");
    if (!mi) mi = &empty;
    const json* ds = obj_get(*mi, "datasheetInfo");
    if (!ds) ds = &empty;
    const json* fn = obj_get(*ds, "function");
    if (!fn) fn = &empty;
    const json* part = obj_get(*ds, "part");
    if (!part) part = &empty;

    auto mpn = get_str(*mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    auto manuf = get_str(*mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    ControllerRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    fill_dimensions(r, *ds, *part);
    r.category = get_str(*fn, "category").value_or("");
    const json* topos = obj_get(*fn, "intendedTopologies");
    if (topos && topos->is_array()) {
        for (const auto& t : *topos)
            if (t.is_string()) r.topologies.push_back(normalize_ctas_topology(t.get<std::string>()));
    }
    const json* el = obj_get(*ds, "electrical");
    if (el) {
        const json* vref_blk = obj_get(*el, "referenceVoltage");
        if (vref_blk) {
            auto vr = get_num(*vref_blk, "nominal");
            if (vr && *vr > 0) r.vref = *vr;
        }
    }
    r.integrated_fet = get_bool(*fn, "integratedFet", false);
    r.integrated_driver = (r.category == "gateDriver") || get_bool(*fn, "integratedDriver", false);
    return r;
}

// ---- Phase 5 extractors ----------------------------------------------------
namespace {
// Shared semiconductor-envelope descent to (electrical, part, manufacturerInfo) for igbt/bjt.
struct SemiRefs {
    const json* mi;
    const json* elec;
    const json* part;
};
std::optional<SemiRefs> semi_refs(const json& env, const char* kind, const json& empty) {
    const json* semi = obj_get(env, "semiconductor");
    if (!semi) return std::nullopt;
    const json* node = obj_get(*semi, kind);
    if (!node) return std::nullopt;
    const json* mi = obj_get(*node, "manufacturerInfo");
    if (!mi) return std::nullopt;
    const json* di = obj_get(*mi, "datasheetInfo");
    if (!di) return std::nullopt;
    const json* elec = obj_get(*di, "electrical");
    if (!elec) return std::nullopt;
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;
    return SemiRefs{mi, elec, part};
}
}  // namespace

std::optional<IgbtRow> extract_igbt(const json& env) {
    const json empty = json::object();
    auto refs = semi_refs(env, "igbt", empty);
    if (!refs) return std::nullopt;
    auto mpn = get_str(*refs->mi, "reference");
    if (!mpn) mpn = get_str(*refs->part, "partNumber");
    auto manuf = get_str(*refs->mi, "name");
    if (!mpn || !manuf) return std::nullopt;
    auto vces = get_num(*refs->elec, "collectorEmitterVoltage");
    auto ic = get_num(*refs->elec, "continuousCollectorCurrent");
    if (!pos(vces) || !pos(ic)) return std::nullopt;
    IgbtRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    const json* dinfo_dims = obj_get(*refs->mi, "datasheetInfo");
    if (dinfo_dims) fill_dimensions(r, *dinfo_dims, *refs->part);
    r.vces_rated = *vces;
    r.ic_continuous = *ic;
    r.vce_sat = get_num(*refs->elec, "collectorEmitterSaturation").value_or(0.0);
    r.technology = get_str(*refs->part, "technology").value_or("");
    auto st = get_str(*refs->mi, "status");
    r.is_production = st && *st == "production";
    const json* dinfo = obj_get(*refs->mi, "datasheetInfo");
    const json* thermal = dinfo ? obj_get(*dinfo, "thermal") : nullptr;
    if (thermal) {
        auto tj = get_num(*thermal, "junctionTemperatureMax");
        if (tj) r.tj_max = *tj;
        auto rjc = get_num(*thermal, "thermalResistanceJunctionCase");
        if (rjc && *rjc > 0) r.rth_jc = *rjc;
    }
    return r;
}

std::optional<BjtRow> extract_bjt(const json& env) {
    const json empty = json::object();
    auto refs = semi_refs(env, "bjt", empty);
    if (!refs) return std::nullopt;
    auto mpn = get_str(*refs->mi, "reference");
    if (!mpn) mpn = get_str(*refs->part, "partNumber");
    auto manuf = get_str(*refs->mi, "name");
    if (!mpn || !manuf) return std::nullopt;
    const std::string polarity = get_str(*refs->part, "subType").value_or("");
    const bool pnp = polarity == "pnp";
    auto vceo = rating_magnitude(get_num(*refs->elec, "collectorEmitterVoltage"), pnp);
    auto ic = rating_magnitude(get_num(*refs->elec, "collectorCurrent"), pnp);
    if (!pos(vceo) || !pos(ic)) return std::nullopt;
    BjtRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    const json* dinfo_dims = obj_get(*refs->mi, "datasheetInfo");
    if (dinfo_dims) fill_dimensions(r, *dinfo_dims, *refs->part);
    r.vceo_rated = *vceo;
    r.ic_continuous = *ic;
    // dcCurrentGain: {minimum,maximum} or scalar -> the guaranteed MINIMUM gain.
    r.hfe_min = resolve_field(*refs->elec, "dcCurrentGain", PEAS::DimensionalValues::MINIMUM)
                    .value_or(0.0);
    r.power_dissipation = get_num(*refs->elec, "powerDissipation").value_or(0.0);
    r.technology = get_str(*refs->part, "technology").value_or("");
    r.polarity = polarity;
    auto st = get_str(*refs->mi, "status");
    r.is_production = st && *st == "production";
    return r;
}

std::optional<VaristorRow> extract_varistor(const json& env) {
    const json empty = json::object();
    const json* v = obj_get(env, "varistor");
    if (!v) return std::nullopt;
    const json* mi = obj_get(*v, "manufacturerInfo");
    if (!mi) return std::nullopt;
    const json* di = obj_get(*mi, "datasheetInfo");
    if (!di) return std::nullopt;
    const json* elec = obj_get(*di, "electrical");
    if (!elec) return std::nullopt;
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;
    auto mpn = get_str(*mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    auto manuf = get_str(*mi, "name");
    if (!mpn || !manuf) return std::nullopt;
    auto vnom = resolve_field(*elec, "varistorVoltage");
    if (!pos(vnom)) return std::nullopt;
    VaristorRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    fill_dimensions(r, *di, *part);
    r.varistor_voltage = *vnom;
    r.clamping_voltage = get_num(*elec, "clampingVoltage").value_or(0.0);
    r.peak_surge_current = get_num(*elec, "peakSurgeCurrent").value_or(0.0);
    r.max_continuous_dc_voltage = get_num(*elec, "maxContinuousDcVoltage").value_or(0.0);
    r.capacitance = get_num(*elec, "capacitance").value_or(0.0);
    r.technology = get_str(*part, "technology").value_or("");
    auto st = get_str(*mi, "status");
    r.is_production = st && *st == "production";
    return r;
}

// ---- Magnetic extractor ----------------------------------------------------
namespace {
// A single representative saturation current (A) for one electrical entry:
//   1. saturationCurrentPeak scalar (the datasheet headline), else
//   2. the saturationCurrents [{percentInductanceDrop,current}] table — the current at the point
//      nearest the 20 % inductance-drop industry convention (falls back to the largest current when
//      no point states a basis). Returns NaN when neither is present.
double magnetic_saturation_current(const json& elec, double& percent_drop) {
    percent_drop = kNaN();
    const json* tbl = obj_get(elec, "saturationCurrents");
    auto peak = get_num(elec, "saturationCurrentPeak");
    if (peak && *peak > 0) {
        // The scalar's basis is the table entry that states the same current, if any —
        // a scalar with no matching entry is of unknown basis and stays NaN.
        if (tbl && tbl->is_array())
            for (const auto& p : *tbl) {
                if (!p.is_object()) continue;
                auto cur = get_num(p, "current");
                auto pd = get_num(p, "percentInductanceDrop");
                if (cur && pd && *pd >= 0 && std::fabs(*cur - *peak) <= 1e-9 * *peak) {
                    percent_drop = *pd;
                    break;
                }
            }
        return *peak;
    }
    if (tbl && tbl->is_array()) {
        const json* best = nullptr;
        double best_dist = 0.0, max_cur = kNaN();
        for (const auto& p : *tbl) {
            if (!p.is_object()) continue;
            auto cur = get_num(p, "current");
            if (!cur || *cur <= 0) continue;
            if (!present(max_cur) || *cur > max_cur) max_cur = *cur;
            auto pd = get_num(p, "percentInductanceDrop");
            if (pd) {
                double d = std::fabs(*pd - 20.0);
                if (!best || d < best_dist) { best = &p; best_dist = d; }
            }
        }
        if (best) {
            percent_drop = get_num(*best, "percentInductanceDrop").value_or(kNaN());
            return get_num(*best, "current").value_or(kNaN());
        }
        return max_cur;  // no stated basis -> the largest quoted current
    }
    return kNaN();
}

// The record's whole saturation table (MagneticRow.sat_table_*, v20): every saturationCurrents
// entry with a positive current, in record order, carrying its criterion and its temperature
// when stated (NaN otherwise); then the saturationCurrentPeak scalar as a criterion-unknown
// point when no entry states that same current (MAS: the scalar is the datasheet's single
// unqualified Isat). Nothing is interpolated, merged or dropped for being "redundant": the
// ranker decides what is comparable.
void magnetic_saturation_table(const json& elec, MagneticRow& r) {
    const json* tbl = obj_get(elec, "saturationCurrents");
    if (tbl && tbl->is_array())
        for (const auto& p : *tbl) {
            if (!p.is_object()) continue;
            auto cur = get_num(p, "current");
            if (!cur || !(*cur > 0)) continue;
            auto pd = get_num(p, "percentInductanceDrop");
            auto t = get_num(p, "temperature");
            r.sat_table_current.push_back(*cur);
            r.sat_table_percent_drop.push_back(pd && *pd >= 0 ? *pd : kNaN());
            r.sat_table_temperature.push_back(t ? *t : kNaN());
        }
    auto peak = get_num(elec, "saturationCurrentPeak");
    if (peak && *peak > 0) {
        bool listed = false;
        for (double c : r.sat_table_current)
            if (std::fabs(c - *peak) <= 1e-9 * *peak) listed = true;
        if (!listed) {
            r.sat_table_current.push_back(*peak);
            r.sat_table_percent_drop.push_back(kNaN());
            r.sat_table_temperature.push_back(kNaN());
        }
    }
}

// Saturation currents READ OFF the record's own inductance-vs-DC-bias curve (MAS
// electrical[].inductancePoints, "the inductance roll-off / saturation curve"), for a record
// that states no saturationCurrents table and no saturationCurrentPeak. Coilcraft's Power
// Inductor Finder curves, among others, reached the catalogue only this way (2,102 inductor
// records, survey of magnetics.ndjson 2026-11-09), and the ranker never saw them: 1812PS-153KRC,
// whose curve is already 38 % down at 0.98 A, was graded 'upgrade' for a 1.2 A original, and
// 1008PS-103KRC (30 % down at 1.06 A) 'equivalent' for a 7.5 A one (Qarlos 2026-11-09, seeds
// 21832044 / 21727315). Rule:
//   * the reference is the curve's OWN zero-bias point (MAS: the drop is measured "from the
//     small-signal value"); a curve with no point at 0 A states no reference and yields nothing;
//   * Isat at a criterion is the FIRST current at which the curve falls to (1 - drop) x L(0),
//     linearly interpolated between the two measured points that bracket the crossing — never
//     extrapolated past the last measured point, so a curve that never reaches a criterion
//     states no Isat at it;
//   * the criteria read are the ones vendors quote, 10 / 20 / 30 % (the span the ranker's isat
//     rules already use), each tagged with its percent drop so the ranker compares like for like;
//   * DECISION (owner to confirm): a curve given at several temperatures is read at ONE of
//     them, the stated temperature nearest 25 degC (the room-temperature basis datasheet Isat
//     figures are quoted at; the hotter one on a tie), never interpolated between temperatures;
//     an untemperatured curve is read only when no temperature is stated at all.
// Reading a stated curve is not a model: no saturation physics, no default, no typical value.
//
// Owner 2026-11-13, Z5: the picked curve itself is kept on the row (bias_curve_current /
// bias_curve_ratio, L / L(0)) whether or not the record also states a bare Isat, so the
// ranker can compare two curves like for like at the same drop. The table is derived from it
// only when the record states no Isat (sat_table_from_curve then marks the table as derived).
void store_curve(const std::vector<std::pair<double, double>>& c, CurvePts& out);  // below
// MAS ratedCurrentPoints as the shared curve storage (owner 2026-11-14, item 7): x = the
// temperature rise (K) each current is rated at, y = the current (A), temperature = the
// ambient (degC) when stated. A single rating is kept as a one-point set (11,202 of the 12,909
// records state one; the ranker reads it at its own rise only). Not carried — the scalar
// rated_current stands alone, as before — when the points cannot be read as one rating per
// rise under one ambient: a point without a positive current and rise, ambients that differ
// or are stated on some points only, two different currents at one rise (14 Panasonic
// ETQP3W / ETQP1W records, e.g. ETQP3W1R5WFN 6.6 A and 11 A both at 40 K), or a current that
// FALLS as the allowed rise grows (4 Coilcraft records, e.g. ME3215-333KLC 35 A at 20 K and
// 0.48 A at 40 K) — a self-heating rating cannot do that, so the record is wrong, and no
// guess is made at which point is.
void magnetic_rated_points(const json& elec, MagneticRow& r) {
    const json* rcp = obj_get(elec, "ratedCurrentPoints");
    if (!rcp || !rcp->is_array() || rcp->empty()) return;
    std::vector<std::pair<double, double>> pts;  // (rise, current)
    std::optional<double> ambient;
    bool any_ambient = false, any_unstated = false;
    for (const auto& p : *rcp) {
        if (!p.is_object()) return;
        const auto cur = get_num(p, "current"), rise = get_num(p, "temperatureRise");
        if (!cur || !rise || !(*cur > 0) || !(*rise > 0)) return;
        if (const auto t = get_num(p, "temperature")) {
            if (ambient && *ambient != *t) return;
            ambient = *t;
            any_ambient = true;
        } else {
            any_unstated = true;
        }
        pts.emplace_back(*rise, *cur);
    }
    if (any_ambient && any_unstated) return;
    std::sort(pts.begin(), pts.end());
    std::vector<float> x, y;
    for (const auto& [rise, cur] : pts) {
        const float fx = static_cast<float>(rise), fy = static_cast<float>(cur);
        if (!x.empty() && fx == x.back()) {
            if (fy != y.back()) return;  // two currents at one rise
            continue;
        }
        if (!y.empty() && fy < y.back()) return;  // falls as the rise grows
        x.push_back(fx);
        y.push_back(fy);
    }
    r.curves.mut().rated_points.x = std::make_shared<const std::vector<float>>(std::move(x));
    r.curves.mut().rated_points.y = std::make_shared<const std::vector<float>>(std::move(y));
    if (ambient) r.curves.mut().rated_points.temperature = *ambient;
}
void magnetic_saturation_from_bias_curve(const json& elec, MagneticRow& r) {
    const bool stated = !r.sat_table_current.empty();  // the record states its Isat outright
    const json* pts = obj_get(elec, "inductancePoints");
    if (!pts || !pts->is_array()) return;
    struct BiasPoint { double current, inductance; };
    struct Curve { bool has_temp; double temp; std::vector<BiasPoint> pts; };
    std::vector<Curve> curves;
    for (const auto& p : *pts) {
        if (!p.is_object()) continue;
        auto cur = get_num(p, "current");
        auto L = get_num(p, "inductance");
        if (!cur || !L || *cur < 0 || !(*L > 0)) continue;
        auto t = get_num(p, "temperature");
        Curve* c = nullptr;
        for (auto& k : curves)
            if (k.has_temp == t.has_value() && (!t || k.temp == *t)) c = &k;
        if (!c) {
            curves.push_back({t.has_value(), t ? *t : kNaN(), {}});
            c = &curves.back();
        }
        c->pts.push_back({*cur, *L});
    }
    const Curve* pick = nullptr;
    for (const auto& c : curves) {
        if (!c.has_temp) continue;
        if (!pick) { pick = &c; continue; }
        const double dc = std::fabs(c.temp - 25.0), dp = std::fabs(pick->temp - 25.0);
        if (dc < dp || (dc == dp && c.temp > pick->temp)) pick = &c;
    }
    if (!pick)
        for (const auto& c : curves)
            if (!c.has_temp) pick = &c;
    // Every stated curve, as it is (owner 2026-11-14): stated temperatures ascending, the
    // untemperatured set last; a curve stating two inductances at one current is not carried
    // (store_curve).
    std::vector<const Curve*> order;
    for (const auto& c : curves) order.push_back(&c);
    std::stable_sort(order.begin(), order.end(), [](const Curve* a, const Curve* b) {
        if (a->has_temp != b->has_temp) return a->has_temp;
        return a->has_temp && a->temp < b->temp;
    });
    for (const Curve* c : order) {
        std::vector<std::pair<double, double>> xy;
        for (const BiasPoint& p : c->pts) xy.emplace_back(p.current, p.inductance);
        std::sort(xy.begin(), xy.end());
        CurvePts cp;
        store_curve(xy, cp);
        if (cp.empty()) continue;
        if (c->has_temp) cp.temperature = c->temp;
        r.curves.mut().l_curves.push_back(std::move(cp));
    }
    if (!pick) return;
    std::vector<BiasPoint> curve = pick->pts;
    std::stable_sort(curve.begin(), curve.end(),
                     [](const BiasPoint& a, const BiasPoint& b) { return a.current < b.current; });
    if (curve.size() < 2 || curve.front().current != 0.0) return;  // no small-signal reference
    const double L0 = curve.front().inductance;
    if (pick->has_temp) r.bias_curve_temperature = pick->temp;
    for (const BiasPoint& p : curve) {
        r.bias_curve_current.push_back(p.current);
        r.bias_curve_ratio.push_back(p.inductance / L0);
    }
    if (stated) return;
    r.sat_table_from_curve = true;
    for (const double drop : {10.0, 20.0, 30.0}) {
        const double threshold = L0 * (1.0 - drop / 100.0);
        for (size_t k = 1; k < curve.size(); ++k) {
            const BiasPoint& a = curve[k - 1];
            const BiasPoint& b = curve[k];
            if (!(b.inductance <= threshold)) continue;
            if (!(a.inductance > threshold) || !(b.current > a.current)) break;  // undecidable
            const double frac = (a.inductance - threshold) / (a.inductance - b.inductance);
            r.sat_table_current.push_back(a.current + frac * (b.current - a.current));
            r.sat_table_percent_drop.push_back(drop);
            r.sat_table_temperature.push_back(pick->has_temp ? pick->temp : kNaN());
            break;
        }
    }
    // The headline scalar follows the table rule of magnetic_saturation_current: the point
    // nearest the 20 % convention.
    for (size_t i = 0; i < r.sat_table_current.size(); ++i)
        if (!present(r.saturation_current) ||
            std::fabs(r.sat_table_percent_drop[i] - 20.0) <
                std::fabs(r.saturation_percent_drop - 20.0)) {
            r.saturation_current = r.sat_table_current[i];
            r.saturation_percent_drop = r.sat_table_percent_drop[i];
        }
}
// Which points of an impedancePoints array make one curve. MAS tags a point with `winding`
// only where one array holds several curves: a common-mode choke's impedancePoints carry the
// common-mode AND the differential-mode curve in one array ("common" / "differential", 616
// records, survey of data-524a699), and the extractor used to read them as one curve — in 185
// records the largest sample, taken as the CM "peak", was a DM point (744290130: its DM
// resonance outranked its CM curve). Untagged points of a CMC are common-mode (MAS: CMC
// impedancePoints are "Common-mode impedance vs. frequency").
//   Untagged     — every point (a part with one curve); a record that tags its points with
//                  more than one winding states several curves and yields none here.
//   Common       — the CMC's "common" or untagged points.
//   Differential — the CMC's "differential" points.
enum class ImpedanceMode { Untagged, Common, Differential };
std::vector<std::pair<double, double>> impedance_curve_points(const json& pts,
                                                              ImpedanceMode mode) {
    std::vector<std::pair<double, double>> curve;  // (frequency, |Z|), sorted by frequency
    std::string first_tag;
    bool several_tags = false;
    for (const auto& pt : pts) {
        if (!pt.is_object()) continue;
        const auto w = get_str(pt, "winding");
        if (mode == ImpedanceMode::Untagged && w) {
            if (first_tag.empty()) first_tag = *w;
            else if (*w != first_tag) several_tags = true;
        }
        if (mode == ImpedanceMode::Common && w && *w != "common") continue;
        if (mode == ImpedanceMode::Differential && (!w || *w != "differential")) continue;
        auto f = get_num(pt, "frequency");
        const json* z = obj_get(pt, "impedance");
        if (!f || !z) continue;
        auto mag = get_num(*z, "magnitude");
        if (!mag || *mag <= 0 || *f <= 0) continue;
        curve.emplace_back(*f, *mag);
    }
    if (several_tags) return {};
    std::sort(curve.begin(), curve.end());
    return curve;
}

// A measured curve onto the row as two float vectors (MagneticRow, v22 curves). `c` is sorted
// by x. The row stores 4-byte floats, so x is compared AFTER that rounding: two samples that
// land on the same float x with the same y are one sample; with different y the record states
// two values at one x and the curve is not carried (which one is right is not on record).
// Fewer than two samples is not a curve. Nothing is resampled or smoothed.
void store_curve(const std::vector<std::pair<double, double>>& c, CurvePts& out) {
    std::vector<float> x, y;
    for (const auto& [cx, cy] : c) {
        const float fx = static_cast<float>(cx), fy = static_cast<float>(cy);
        if (!std::isfinite(fx) || !std::isfinite(fy)) return;
        if (!x.empty() && fx == x.back()) {
            if (fy != y.back()) return;
            continue;
        }
        x.push_back(fx);
        y.push_back(fy);
    }
    if (x.size() < 2) return;
    out.x = std::make_shared<const std::vector<float>>(std::move(x));
    out.y = std::make_shared<const std::vector<float>>(std::move(y));
}

// A frequency sweep of one scalar (resistancePoints {frequency, resistance}, reactancePoints
// {frequency, reactance}, insertion-loss points {frequency, insertionLoss}) as (x, y) sorted by
// frequency: points with a non-positive frequency or no value are not samples. A record whose
// points carry more than one `winding` tag states several curves in one array and yields none
// (none in data-524a699 does).
std::vector<std::pair<double, double>> sweep_points(const json& pts, const char* ykey) {
    std::vector<std::pair<double, double>> curve;
    if (!pts.is_array()) return curve;
    std::optional<std::string> first_tag;
    for (const auto& pt : pts) {
        if (!pt.is_object()) continue;
        if (const auto w = get_str(pt, "winding")) {
            if (!first_tag) first_tag = *w;
            else if (*w != *first_tag) return {};
        }
        const auto f = get_num(pt, "frequency");
        const auto y = get_num(pt, ykey);
        if (!f || !(*f > 0) || !y) continue;
        curve.emplace_back(*f, *y);
    }
    std::sort(curve.begin(), curve.end());
    return curve;
}

// The readings Kelvin ranks a |Z|(f) curve on: |Z| at the conventional 100 MHz test point
// and the bracketed peak with its frequency. A bead's published spec is |Z| at 100 MHz, and
// TDK's own engineer calls that spot value "irrelevant and misleading" on its own: parts with
// identical Z@100MHz routinely peak at very different heights and frequencies, which is what
// actually decides whether the bead suppresses the noise you have. Where the catalogue
// carries the measured curve we derive the peak and its frequency — neither is a field in any
// vendor's datasheet table. Outputs stay NaN when the curve does not state them.
void impedance_readings(const std::vector<std::pair<double, double>>& curve, double& at_100m_out,
                        double& peak_out, double& peak_freq_out) {
    // `closest` tracks a DISTANCE, and its best possible value is zero — so zero cannot also
    // mean "nothing seen yet". It did, and the collision cost exactly the parts this field
    // exists for: a bead sampled AT 100 MHz set closest = 0, which still read as unset, so the
    // next point overwrote it and the 5 MHz proximity test then failed.
    double at_100m = 0.0, closest = 0.0;
    bool have_closest = false;
    for (const auto& [f, mag] : curve) {
        const double d = std::fabs(f - 1e8);
        if (!have_closest || d < closest) {
            closest = d;
            at_100m = mag;
            have_closest = true;
        }
    }
    // A peak is only a peak when the curve is seen to fall on BOTH sides of it. The largest
    // sample of a curve that ends while still rising (or starts while falling) is just where
    // the measurement stopped: unbracketed maximum -> no peak (UNVERIFIED).
    if (!curve.empty()) {
        std::size_t first = 0, last = 0;
        for (std::size_t k = 1; k < curve.size(); ++k) {
            if (curve[k].second > curve[first].second) first = last = k;
            else if (curve[k].second == curve[first].second) last = k;
        }
        if (first > 0 && last + 1 < curve.size()) {
            peak_out = curve[first].second;
            peak_freq_out = curve[first].first;
        }
    }
    // Only claim a 100 MHz value when a sample is actually near it — extrapolating from a
    // distant point would invent a datum.
    if (have_closest && at_100m > 0 && closest <= 5e6) at_100m_out = at_100m;
}
}  // namespace

std::optional<MagneticRow> extract_magnetic(const json& env) {
    const json* mag = obj_get(env, "magnetic");
    if (!mag) return std::nullopt;
    const json* mi = obj_get(*mag, "manufacturerInfo");
    if (!mi) return std::nullopt;
    const json* di = obj_get(*mi, "datasheetInfo");
    if (!di) return std::nullopt;
    const json empty = json::object();
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;

    // Identity: reference (or part.partNumber) + manufacturer name. As with every family, a row
    // without an identity is unreadable (skipped, counted) — that is not a spec gate.
    auto mpn = get_str(*mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    auto manuf = get_str(*mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    MagneticRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    // MAS states the board attach as mechanical.mounting (the PEAS connectionType enum: smt /
    // tht / ...), a key fill_dimensions does not read: 9,083 magnetic records state it (survey
    // 2026-11-09), and a TDK B78108S1562K000 ('tht', axial) was graded minor_review for an SMT
    // original with no mount note (Qarlos 2026-11-09, seed 22146231). Set before
    // fill_dimensions, which needs the mount, and which an explicit assemblyType still
    // overrides (no record states both).
    if (const json* mech = obj_get(*di, "mechanical"))
        if (auto m = get_str(*mech, "mounting")) r.mount = *m;
    fill_dimensions(r, *di, *part);
    // Distributor unit costs (owner 2026-11-14): only a stated positive value with a currency,
    // the MOSFET extractor's rule.
    if (const json* dis = obj_get(*mag, "distributorsInfo"); dis && dis->is_array()) {
        json quotes = json::array();
        for (const json& d : *dis) {
            if (!d.is_object()) continue;
            const json* cost = obj_get(d, "cost");
            if (!cost || !cost->is_object()) continue;
            auto v = get_num(*cost, "value");
            auto cur = get_str(*cost, "currency");
            if (!v || !(*v > 0) || !cur || cur->empty()) continue;
            json q = {{"distributor", get_str(d, "name").value_or("")},
                      {"unitCost", *v},
                      {"currency", *cur}};
            if (auto moq = get_num(d, "moq"); moq && *moq > 0) q["moq"] = *moq;
            quotes.push_back(std::move(q));
        }
        if (!quotes.empty()) r.price_quotes = quotes.dump();
    }

    // electrical is an ARRAY; the first entry is the primary-winding projection we rank on.
    const json* elec_arr = obj_get(*di, "electrical");
    if (elec_arr && elec_arr->is_array() && !elec_arr->empty() && elec_arr->front().is_object()) {
        const json& elec = elec_arr->front();
        auto L = resolve_field(elec, "inductance");
        if (L && *L > 0) r.inductance = *L;
        // The stated inductance tolerance (R1, owner 2026-11-13): MAS carries it as the
        // inductance's minimum and maximum, not as a percentage (survey of magnetics.ndjson
        // 3d2be2e: 62,302 records state nominal+minimum+maximum, no record a tolerance key).
        // A band is two-sided: both bounds must be stated — the resolver falls back to the
        // nominal for a missing end, which would read as a 0 % side. A band that does not
        // bracket the nominal is not a tolerance and is not carried.
        if (present(r.inductance))
            if (const json* lj = obj_get(elec, "inductance");
                lj && lj->is_object() && lj->contains("minimum") && lj->contains("maximum")) {
                try {
                    const double lo = PEAS::resolve_dimensional_values(
                                     *lj, PEAS::DimensionalValues::MINIMUM),
                                 hi = PEAS::resolve_dimensional_values(
                                     *lj, PEAS::DimensionalValues::MAXIMUM);
                    if (lo > 0 && lo <= r.inductance && hi >= r.inductance && hi > lo) {
                        r.inductance_tol_minus_pct = (1.0 - lo / r.inductance) * 100.0;
                        r.inductance_tol_plus_pct = (hi / r.inductance - 1.0) * 100.0;
                    }
                } catch (const std::exception&) {
                    // a non-numeric bound: no band on record (unreadable, never guessed)
                }
            }
        r.saturation_current = magnetic_saturation_current(elec, r.saturation_percent_drop);
        magnetic_saturation_table(elec, r);
        magnetic_saturation_from_bias_curve(elec, r);
        const json* rc = obj_get(elec, "ratedCurrents");
        if (rc && rc->is_array()) {
            double best = kNaN();
            for (const auto& v : *rc)
                if (v.is_number() && !v.is_boolean()) {
                    double x = v.get<double>();
                    if (x > 0 && (!present(best) || x > best)) best = x;
                }
            r.rated_current = best;
        }
        // MAS ratedCurrentPoints: the rated current qualified by its temperature-rise
        // criterion, which the schema PREFERS over the bare ratedCurrents array and tells
        // vendors to state instead of it. 4,724 magnetic records state only this form (survey
        // 2026-11-09), so their rating was dropped: 0805CS-820XGLB (0.4 A at 15 K) left every
        // candidate's rated current unweighed (Qarlos 2026-11-09, seed 22041502). Read when the
        // bare array states nothing, with the same rule as that array: the largest stated
        // current. The temperature-rise basis is not yet carried (no row field), exactly as the
        // bare array carries none.
        if (!present(r.rated_current))
            if (const json* rcp = obj_get(elec, "ratedCurrentPoints"); rcp && rcp->is_array()) {
                double best = kNaN();
                for (const auto& p : *rcp) {
                    if (!p.is_object()) continue;
                    auto x = get_num(p, "current");
                    if (x && *x > 0 && (!present(best) || *x > best)) best = *x;
                }
                r.rated_current = best;
            }
        magnetic_rated_points(elec, r);
        // DCR: prefer the worst-case (maximum) bound; fall back to the plural dcResistances[0].
        auto dcr = resolve_field(elec, "dcResistance", PEAS::DimensionalValues::MAXIMUM);
        const json* dcr_src = dcr ? obj_get(elec, "dcResistance") : nullptr;
        if (!dcr) {
            const json* dcrs = obj_get(elec, "dcResistances");
            if (dcrs && dcrs->is_array() && !dcrs->empty()) {
                try {
                    dcr = PEAS::resolve_dimensional_values(dcrs->front(),
                                                           PEAS::DimensionalValues::MAXIMUM);
                    dcr_src = &dcrs->front();
                } catch (const std::exception&) {
                }
            }
        }
        if (dcr && *dcr > 0) {
            r.dcr = *dcr;
            // Which bound the resolver returned (owner 2026-11-14): the MAXIMUM when the
            // record states one, else its nominal (a bare number reads as a nominal).
            r.dcr_is_maximum = dcr_src && dcr_src->is_object() && dcr_src->contains("maximum");
        }
        auto srf = get_num(elec, "selfResonantFrequency");
        if (srf && *srf > 0) r.srf = *srf;
        // Impedance curve(s): |Z|(f) per mode (impedance_curve_points / impedance_readings,
        // above). A common-mode choke's impedancePoints carry BOTH modes in one array, each
        // point tagged by `winding` (616 records); the common-mode curve is its headline, the
        // differential-mode one is read separately, and the two are never merged.
        if (const json* pts = obj_get(elec, "impedancePoints");
            pts && pts->is_array() && !pts->empty()) {
            const bool cmc = get_str(elec, "subtype").value_or("") == "commonModeChoke";
            const auto cm = impedance_curve_points(*pts, cmc ? ImpedanceMode::Common
                                                             : ImpedanceMode::Untagged);
            impedance_readings(cm, r.impedance_100mhz, r.impedance_peak, r.impedance_peak_freq);
            store_curve(cm, r.curves.mut().z_curve);
            if (cmc) {
                // differentialModeImpedancePoints is the schema's own DM field; the tagged
                // points are read only when it is absent (every record that states both states
                // the same points, survey of data-524a699).
                const json* dmp = obj_get(elec, "differentialModeImpedancePoints");
                const auto dm =
                    dmp && dmp->is_array() && !dmp->empty()
                        ? impedance_curve_points(*dmp, ImpedanceMode::Untagged)
                        : impedance_curve_points(*pts, ImpedanceMode::Differential);
                impedance_readings(dm, r.dm_impedance_100mhz, r.dm_impedance_peak,
                                   r.dm_impedance_peak_freq);
                store_curve(dm, r.curves.mut().dm_z_curve);
            }
        }
        // R(f) and X(f) (v22 curves): the dissipative and the reactive part of a bead's
        // impedance, compared curve vs curve by the ranker.
        if (const json* rp = obj_get(elec, "resistancePoints"))
            store_curve(sweep_points(*rp, "resistance"), r.curves.mut().r_curve);
        if (const json* xp = obj_get(elec, "reactancePoints"))
            store_curve(sweep_points(*xp, "reactance"), r.curves.mut().x_curve);
        // A CMC's insertion loss per mode (dB, positive = attenuation, as all 310 / 264 records
        // state it), compared curve vs curve by the ranker.
        if (const json* p = obj_get(elec, "commonModeInsertionLossPoints"))
            store_curve(sweep_points(*p, "insertionLoss"), r.curves.mut().cm_il_curve);
        if (const json* p = obj_get(elec, "differentialModeInsertionLossPoints"))
            store_curve(sweep_points(*p, "insertionLoss"), r.curves.mut().dm_il_curve);
        // Transformer turns ratio (primary:secondary): electrical[0].turnsRatios[0] (a dimWithTol). Lets
        // select_magnetic rank a catalog transformer by how close its ratio is to the design's required
        // ratio — the meaningful match for flyback/isolated transformers (TAS has no per-winding structure).
        const json* trs = obj_get(elec, "turnsRatios");
        if (trs && trs->is_array() && !trs->empty()) {
            try {
                double tr = PEAS::resolve_dimensional_values(trs->front());
                if (tr > 0) r.turns_ratio = tr;
            } catch (const std::exception&) {
            }
            // The whole array (v22), all-or-nothing: one unreadable or non-positive entry
            // leaves the structure unknown rather than a shorter, wrong one.
            std::vector<double> all;
            bool ok = true;
            for (const auto& t : *trs) {
                try {
                    const double v = PEAS::resolve_dimensional_values(t);
                    if (!(v > 0)) { ok = false; break; }
                    all.push_back(v);
                } catch (const std::exception&) {
                    ok = false;
                    break;
                }
            }
            if (ok) r.turns_ratios = std::move(all);
        }
        r.device_type = get_str(elec, "subtype").value_or("");
        // A cable core's fit and pass count (v22, owner 2026-11-14), as stated; a value that is
        // not positive (a diameter) or negative (a pass count) is not a statement of either.
        if (r.device_type == "cableCore") {
            if (const auto d = get_num(elec, "maximumCableOuterDiameter"); d && *d > 0)
                r.cable_max_diameter = *d;
            if (const auto n = get_num(elec, "numberTurns"); n && *n > 0) r.number_turns = *n;
        }

        // Winding structure across EVERY wiring configuration (a transformer with an auxiliary winding
        // files both "all windings" and "auxiliary windings open"): the set of secondary-winding counts
        // the part can realize. Only stated structure counts — a multi-winding entry without turnsRatios
        // adds nothing.
        for (const auto& e : *elec_arr) {
            if (!e.is_object()) continue;
            // Every configuration's turns ratios (v22, owner 2026-11-13 W4), all-or-nothing per
            // configuration exactly as turns_ratios: an unreadable or non-positive entry leaves
            // that configuration out rather than a shorter, wrong array.
            if (const json* cr = obj_get(e, "turnsRatios"); cr && cr->is_array() && !cr->empty()) {
                std::vector<double> cfg;
                bool ok = true;
                for (const auto& t : *cr) {
                    try {
                        const double v = PEAS::resolve_dimensional_values(t);
                        if (!(v > 0)) { ok = false; break; }
                        cfg.push_back(v);
                    } catch (const std::exception&) {
                        ok = false;
                        break;
                    }
                }
                if (ok) r.turns_ratio_configs.push_back(std::move(cfg));
            }
            const std::string st = get_str(e, "subtype").value_or("");
            if (st == "inductor" || st == "chipBead") {
                r.secondary_counts |= 1u;
            } else if (st == "transformer" || st == "coupledInductor") {
                const json* ratios = obj_get(e, "turnsRatios");
                if (ratios && ratios->is_array() && !ratios->empty() && ratios->size() < 32)
                    r.secondary_counts |= (1u << ratios->size());
            }
        }
    }

    std::string fam = get_str(*mi, "family").value_or("");
    if (fam.empty()) fam = get_str(*part, "family").value_or("");
    r.family = fam;
    // Operating-temperature range (v20): PEAS datasheetInfoThermal thermal.operatingTemperature,
    // the only temperature key the magnetic catalogue uses (survey of magnetics.ndjson,
    // 2026-11-09: 80,794 of 97,191 records state it, 74,954 with a minimum). Straight from the
    // object: a cold limit is negative and 0 degC is a real rating.
    if (const json* th = obj_get(*di, "thermal")) {
        if (const json* t = obj_get(*th, "operatingTemperature")) {
            if (auto v = get_num(*t, "minimum")) r.temp_min_c = *v;
            if (auto v = get_num(*t, "maximum")) r.temp_max_c = *v;
        }
    }
    auto status = get_str(*mi, "status");
    r.is_production = status.has_value() && *status == "production";
    return r;
}

// ---- analog ICs / timing devices -------------------------------------------
// Both envelopes carry a VARIABLE subtype key (analog.<subtype> / timeBase.<subtype>), so the
// walk takes the first object member carrying a manufacturerInfo. Identity is required; every
// electrical datum is optional (NaN when absent): the selectors judge an absent one UNVERIFIED.
namespace {
struct SubtypeNode {
    std::string subtype;
    const json* mi = nullptr;  // manufacturerInfo
    const json* di = nullptr;  // datasheetInfo (may be null)
};

std::optional<SubtypeNode> subtype_node(const json& env, const char* root) {
    const json* node = obj_get(env, root);
    if (!node || !node->is_object()) return std::nullopt;
    for (auto it = node->begin(); it != node->end(); ++it) {
        if (!it->is_object()) continue;
        const json* mi = obj_get(*it, "manufacturerInfo");
        if (!mi) continue;
        SubtypeNode out;
        out.subtype = it.key();
        out.mi = mi;
        out.di = obj_get(*mi, "datasheetInfo");
        return out;
    }
    return std::nullopt;
}

void set_if_pos(double& dst, const std::optional<double>& v) {
    if (v && *v > 0) dst = *v;
}
}  // namespace

std::optional<AnalogRow> extract_analog(const json& env) {
    auto node = subtype_node(env, "analog");
    if (!node) return std::nullopt;
    const json empty = json::object();
    const json* di = node->di ? node->di : &empty;
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;

    auto mpn = get_str(*node->mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    auto manuf = get_str(*node->mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    AnalogRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    fill_dimensions(r, *di, *part);
    r.device_type = node->subtype;

    const json* elec = obj_get(*di, "electrical");
    if (elec && elec->is_object()) {
        set_if_pos(r.channels, get_num(*elec, "numberOfChannels"));
        set_if_pos(r.input_offset_voltage, get_num(*elec, "inputOffsetVoltage"));
        set_if_pos(r.input_bias_current, get_num(*elec, "inputBiasCurrent"));
        set_if_pos(r.gain_bandwidth, get_num(*elec, "gainBandwidthProduct"));
        set_if_pos(r.slew_rate, get_num(*elec, "slewRate"));
        set_if_pos(r.cmrr, get_num(*elec, "commonModeRejectionRatio"));
        set_if_pos(r.resolution, get_num(*elec, "resolution"));
        set_if_pos(r.sample_rate, get_num(*elec, "sampleRate"));
        set_if_pos(r.on_resistance, get_num(*elec, "onResistance"));
        const json* supply = obj_get(*elec, "supply");
        if (supply) {
            set_if_pos(r.vsupply_min, get_num(*supply, "minimumSupplyVoltage"));
            set_if_pos(r.vsupply_max, get_num(*supply, "maximumSupplyVoltage"));
        }
        r.architecture = get_str(*elec, "architecture").value_or("");
        r.input_stage = get_str(*elec, "inputStage").value_or("");
    }
    auto status = get_str(*node->mi, "status");
    r.is_production = status.has_value() && *status == "production";
    r.status = status.value_or("");  // "" = not stated (v24)
    return r;
}

std::optional<TimingRow> extract_timing(const json& env) {
    auto node = subtype_node(env, "timeBase");
    if (!node) return std::nullopt;
    const json empty = json::object();
    const json* di = node->di ? node->di : &empty;
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;

    auto mpn = get_str(*node->mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    auto manuf = get_str(*node->mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    TimingRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    fill_dimensions(r, *di, *part);
    r.device_type = node->subtype;

    const json* elec = obj_get(*di, "electrical");
    if (elec && elec->is_object()) {
        set_if_pos(r.frequency, get_num(*elec, "frequency"));
        set_if_pos(r.frequency_tolerance, get_num(*elec, "frequencyTolerance"));
        set_if_pos(r.frequency_stability, get_num(*elec, "frequencyStability"));
        set_if_pos(r.load_capacitance, get_num(*elec, "loadCapacitance"));
        set_if_pos(r.built_in_capacitance, get_num(*elec, "builtInCapacitance"));
        set_if_pos(r.esr, get_num(*elec, "equivalentSeriesResistance"));
        set_if_pos(r.rms_phase_jitter, get_num(*elec, "rmsPhaseJitter"));
        // v22: the programmable span. Both ends or neither (the schema requires both); a
        // range whose minimum exceeds its maximum states no span and is left unset.
        if (const json* fr = obj_get(*elec, "frequencyRange")) {
            const auto lo = get_num(*fr, "minimum"), hi = get_num(*fr, "maximum");
            if (lo && hi && *lo > 0 && *hi > 0 && *lo <= *hi) {
                r.frequency_min = *lo;
                r.frequency_max = *hi;
            }
        }
        const json* supply = obj_get(*elec, "supply");
        if (supply) {
            set_if_pos(r.vsupply_min, get_num(*supply, "minimumSupplyVoltage"));
            set_if_pos(r.vsupply_max, get_num(*supply, "maximumSupplyVoltage"));
            set_if_pos(r.current_consumption, get_num(*supply, "currentConsumption"));
        }
        r.technology = get_str(*elec, "technology").value_or("");
        r.output_type = get_str(*elec, "outputType").value_or("");
        r.mode = get_str(*elec, "mode").value_or("");
        r.enable_function = get_str(*elec, "enableFunction").value_or("");
    }
    // Read straight from the object, not through set_if_pos: a cold limit is negative.
    if (const json* th = obj_get(*di, "thermal")) {
        if (const json* t = obj_get(*th, "operatingTemperature")) {
            if (auto v = get_num(*t, "minimum")) r.temp_min_c = *v;
            if (auto v = get_num(*t, "maximum")) r.temp_max_c = *v;
        }
    }
    auto status = get_str(*node->mi, "status");
    r.is_production = status.has_value() && *status == "production";
    r.status = status.value_or("");  // "" = not stated (v24)
    return r;
}

std::optional<ConnectorRow> extract_connector(const json& env) {
    const json* con = obj_get(env, "connector");
    if (!con) return std::nullopt;
    const json* mi = obj_get(*con, "manufacturerInfo");
    if (!mi) return std::nullopt;
    const json empty = json::object();
    const json* di = obj_get(*mi, "datasheetInfo");
    if (!di) di = &empty;
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;

    auto mpn = get_str(*mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    auto manuf = get_str(*mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    ConnectorRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    fill_dimensions(r, *di, *part);

    const json* elec = obj_get(*di, "electrical");
    if (elec && elec->is_object()) {
        set_if_pos(r.rated_current, get_num(*elec, "ratedCurrentPerContact"));
        set_if_pos(r.rated_voltage, get_num(*elec, "ratedVoltage"));
    }
    // derating.currentVsAmbient (owner rule 2026-11-14): the per-contact current FRACTION
    // against ambient, {temperature[], amplitude[]}, and the contact temperature rise it was
    // set for (maxTemperatureRise) as its condition. Malformed -> loud, with the part.
    if (const json* der = obj_get(*di, "derating")) {
        if (const json* cva = obj_get(*der, "currentVsAmbient")) {
            json xy = {{"xData", cva->value("temperature", json(nullptr))},
                       {"yData", cva->value("amplitude", json(nullptr))}};
            if (auto rise = get_num(*der, "maxTemperatureRise"))
                xy["conditions"] = {{"maxTemperatureRise", *rise}};
            r.curves.mut().current_ambient_curve = read_xy_curve(xy, *mpn + ": derating.currentVsAmbient");
            if (!r.curves.mut().current_ambient_curve.empty())
                for (float a : *r.curves.mut().current_ambient_curve.y)
                    if (!(a >= 0.0f && a <= 1.0f))
                        throw std::runtime_error(*mpn + ": derating.currentVsAmbient amplitude must "
                                             "lie in [0, 1]");
        }
    }
    const json* mech = obj_get(*di, "mechanical");
    if (mech && mech->is_object()) {
        set_if_pos(r.positions, get_num(*mech, "positions"));
        set_if_pos(r.pitch, get_num(*mech, "pitch"));
        set_if_pos(r.rows, get_num(*mech, "rows"));
        set_if_pos(r.mating_cycles, get_num(*mech, "matingCycles"));
        // Mating-axis orientation (vertical / rightAngle / horizontal / mezzanine / edge /
        // cable). Empty when unstated: the ranker then reports it unverified, never a match.
        if (auto o = get_str(*mech, "orientation")) r.orientation = *o;
    }
    // Operating temperature range: a substitute must cover the original's, at both
    // ends. Read straight out of the object — NOT through set_if_pos, because a cold
    // limit is normally negative and 0 degC is a real rating, so "positive" is not a
    // presence test for a temperature.
    const json* envir = obj_get(*di, "environmental");
    if (envir && envir->is_object()) {
        if (const json* t = obj_get(*envir, "operatingTemperature")) {
            if (auto v = get_num(*t, "minimum")) r.temp_min_c = *v;
            if (auto v = get_num(*t, "maximum")) r.temp_max_c = *v;
        }
    }
    const json* fd = obj_get(*di, "familyDetails");
    if (fd && fd->is_object()) {
        r.family = get_str(*fd, "family").value_or("");
        // familyDetails is a discriminated union on family, and two of the axes below are
        // spelled per variant. Reading only one spelling of each is what made the family
        // caveat's promise — "compared where both records state one" — false about records
        // that DO state one, which is the whole of ABT #505:
        //   * the wire attach is 'termination' on wireToBoard/wireToWire/power and
        //     'clampType' on terminalBlock (CONAS utils.json says so itself: "same axis as
        //     familyWireToBoard.termination"), and the dropped half was the LARGER one —
        //     11,627 clampType rows against 9,908 termination ones — on the very family
        //     where the wire attach decides the swap;
        //   * the standardised interface is 'interfaceStandard' on dataInterface and
        //     'interface' on rf, so all 8,235 RF rows named their interface (SMA / BNC /
        //     U.FL / FAKRA …) and reached the ranker with the slot empty, which reported
        //     an SMA-against-BNC comparison "unverified" instead of a definite non-mate.
        // No record states both spellings of an axis: they belong to different variants.
        r.interface_standard = get_str(*fd, "interfaceStandard")
                                   .value_or(get_str(*fd, "interface").value_or(""));
        r.termination = get_str(*fd, "termination")
                            .value_or(get_str(*fd, "clampType").value_or(""));
        // terminalBlock's pluggable flag, tri-state: unstated stays "" (never "false").
        if (auto it = fd->find("pluggable"); it != fd->end() && it->is_boolean())
            r.pluggable = it->get<bool>() ? "true" : "false";
        // Variant identities (owner 2026-11-12): only the structured fields, each present on
        // exactly one familyDetails variant (fpcFfc / circular); unstated stays "".
        r.actuator_type = get_str(*fd, "actuatorType").value_or("");
        r.shell_size = get_str(*fd, "shellSize").value_or("");
        r.coding = get_str(*fd, "coding").value_or("");
        r.coupling_type = get_str(*fd, "couplingType").value_or("");
    }
    // The separable interface itself: only the mating-area material, which is what decides
    // whether two halves may be mated at all (gold onto tin frets and corrodes). The
    // underplating and the plating thicknesses are wear data, not compatibility data.
    if (const json* mat = obj_get(*di, "material")) {
        if (const json* plating = obj_get(*mat, "contactPlating"))
            r.contact_plating = get_str(*plating, "matingAreaMaterialRef").value_or("");
    }
    r.polarity = get_str(*part, "matingPolarity").value_or("");
    r.series = get_str(*part, "series").value_or("");
    auto status = get_str(*mi, "status");
    r.is_production = status.has_value() && *status == "production";
    return r;
}

}  // namespace kelvin
