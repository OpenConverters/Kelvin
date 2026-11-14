// Curve-vs-curve comparison, shared by every family (owner rule 2026-11-14: "make sure that
// Kelvin compares ALL existing graphs"). One implementation of the one rule:
//
//   * a curve is a set of measured (x, y) samples, x strictly increasing;
//   * it is read only INSIDE its measured range — linear interpolation, or log-log where the
//     quantity is plotted on logarithmic axes — never extrapolated (a value outside what was
//     measured is not on record: nullopt);
//   * two curves are compared at the x points inside BOTH ranges (the union of both curves'
//     own sample x within the overlap), each point judged by compare_numeric with that
//     quantity's scalar ParamSpec (same bands, same direction);
//   * the worst point decides the verdict and is reported (x, both y, warn depth);
//   * no overlap -> UNVERIFIED (the caller adds the note; no cap).
//
// Different measurement conditions (temperature, bias, winding mode, mounting) are the
// caller's to detect before calling: this header compares numbers, it does not know what a
// curve was measured under. Program-only, no I/O.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "CrossRefParams.hpp"  // ParamSpec, compare_numeric
#include "CrossRefScore.hpp"   // PASS / WARN / FAIL / UNVERIFIED

namespace kelvin::crossref {

enum class Interp { Linear, LogLog };

// A measured curve. The constructor validates and THROWS on malformed input — no silent
// repair: sizes must match, at least two samples, every value finite, x strictly
// increasing (sorted, no duplicates). A caller holding unsorted record data sorts it (a
// permutation, not a change of data) and decides itself what a duplicate x means.
struct Curve {
    std::vector<double> x, y;
    Curve(std::vector<double> xs, std::vector<double> ys) : x(std::move(xs)), y(std::move(ys)) {
        if (x.size() != y.size())
            throw std::invalid_argument("Curve: x and y differ in length (" +
                                        std::to_string(x.size()) + " vs " +
                                        std::to_string(y.size()) + ")");
        if (x.size() < 2)
            throw std::invalid_argument("Curve: a curve needs at least two samples, got " +
                                        std::to_string(x.size()));
        for (std::size_t i = 0; i < x.size(); ++i) {
            if (!std::isfinite(x[i]) || !std::isfinite(y[i]))
                throw std::invalid_argument("Curve: non-finite sample at index " +
                                            std::to_string(i));
            if (i > 0 && !(x[i] > x[i - 1]))
                throw std::invalid_argument("Curve: x not strictly increasing at index " +
                                            std::to_string(i));
        }
    }
};

// y at x, interpolated between the two samples that bracket it; exactly the sample at a
// sampled x; nullopt outside [x.front(), x.back()]. LogLog throws when the bracketing
// samples or x are not positive (the curve cannot be read on log axes).
inline std::optional<double> curve_at(const Curve& c, double x, Interp interp) {
    if (!std::isfinite(x) || x < c.x.front() || x > c.x.back()) return std::nullopt;
    std::size_t hi = 1;
    while (hi < c.x.size() && c.x[hi] < x) ++hi;
    if (c.x[hi] == x) return c.y[hi];
    const std::size_t lo = hi - 1;
    if (c.x[lo] == x) return c.y[lo];
    const double x0 = c.x[lo], x1 = c.x[hi], y0 = c.y[lo], y1 = c.y[hi];
    if (interp == Interp::Linear) return y0 + (x - x0) / (x1 - x0) * (y1 - y0);
    if (!(x0 > 0) || !(y0 > 0) || !(y1 > 0))
        throw std::invalid_argument("curve_at: log-log read over a non-positive sample");
    const double t = (std::log(x) - std::log(x0)) / (std::log(x1) - std::log(x0));
    return std::exp(std::log(y0) + t * (std::log(y1) - std::log(y0)));
}

// The x range both curves measured, or nullopt when they do not meet.
inline std::optional<std::pair<double, double>> overlap(const Curve& a, const Curve& b) {
    const double lo = std::max(a.x.front(), b.x.front());
    const double hi = std::min(a.x.back(), b.x.back());
    if (lo > hi) return std::nullopt;
    return std::make_pair(lo, hi);
}

struct CurveVerdict {
    const char* verdict = UNVERIFIED;  // PASS / WARN / FAIL / UNVERIFIED (no overlap)
    // The deciding point (unset when UNVERIFIED): the worst verdict, and among points of that
    // verdict the one furthest the wrong way (Lower: largest sub/orig; Higher: smallest).
    double worst_x = std::nan("");
    double orig_y = std::nan("");
    double sub_y = std::nan("");
    std::optional<double> warn_depth;  // compare_numeric's, at the worst point, on a WARN
    std::optional<std::pair<double, double>> range;  // the overlap compared
    std::size_t points = 0;                            // sample points judged
};

// orig vs sub on their overlap, each point judged with `spec` (Dir::Lower or Dir::Higher;
// anything else throws — a curve is a numeric quantity).
inline CurveVerdict compare_curves(const Curve& orig, const Curve& sub, const ParamSpec& spec,
                                   Interp interp) {
    if (spec.dir != Dir::Lower && spec.dir != Dir::Higher)
        throw std::invalid_argument("compare_curves: spec '" + spec.key +
                                    "' is not a numeric (Lower/Higher) spec");
    CurveVerdict out;
    out.range = overlap(orig, sub);
    if (!out.range) return out;
    const auto [lo, hi] = *out.range;
    std::vector<double> xs;
    for (const Curve* c : {&orig, &sub})
        for (double x : c->x)
            if (x >= lo && x <= hi) xs.push_back(x);
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    auto rank = [](const char* v) {
        const std::string s = v;
        return s == FAIL ? 2 : s == WARN ? 1 : 0;
    };
    // How far the wrong way a point sits, comparable within one verdict: the ratio (or, for an
    // additive abs_tol spec, the difference) in the spec's bad direction.
    auto badness = [&](double o, double s) {
        if (spec.abs_tol) return spec.dir == Dir::Lower ? s - o : o - s;
        if (spec.dir == Dir::Lower) return o != 0 ? s / o : (s > 0 ? HUGE_VAL : 0.0);
        return s != 0 ? o / s : (o > 0 ? HUGE_VAL : 0.0);
    };
    int best_rank = -1;
    double best_bad = -HUGE_VAL;
    for (double x : xs) {
        const auto o = curve_at(orig, x, interp), s = curve_at(sub, x, interp);
        if (!o || !s) continue;  // cannot happen inside the overlap; kept total, never guessed
        const ParamOutcome r = compare_numeric(spec, o, s);
        ++out.points;
        const int rk = rank(r.verdict);
        const double bad = badness(*o, *s);
        if (rk > best_rank || (rk == best_rank && bad > best_bad)) {
            best_rank = rk;
            best_bad = bad;
            out.verdict = r.verdict;
            out.worst_x = x;
            out.orig_y = *o;
            out.sub_y = *s;
            out.warn_depth = r.warn_depth;
        }
    }
    return out;
}

}  // namespace kelvin::crossref
