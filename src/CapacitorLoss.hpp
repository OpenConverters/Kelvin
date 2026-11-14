// Operating-point loss and checks of a buck capacitor from its datasheet values (owner
// 2026-11-14), the capacitor twin of MosfetLoss.hpp.
//
// Ripple current through the capacitor (ideal CCM buck, D = Vout/Vin):
//   output   I_ripple = dIpp / sqrt(12)              (the inductor's triangular ripple)
//   input    I_ripple = Iout * sqrt(D * (1 - D))     (the chopped input current, flat-top)
// Terms and checks:
//   ESR loss          P = I_ripple^2 * ESR(fs)  — ESR read off the record's ESR(f) curve at fs
//                     when it covers fs, else the scalar ESR with its stated frequency named
//                     (a scalar ESR with no stated frequency is ignored, owner 2026-11-14);
//                     a record with no ESR read: ESR = tan(delta) / (2*pi*f_df*C) at
//                     tan(delta)'s own frequency f_df, an UPPER BOUND at fs >= f_df (ESR falls
//                     with frequency; owner 2026-11-14) — fs below f_df or no f_df: UNVERIFIED
//   effective C       at the DC voltage across the part (Vout / Vin): read off the record's
//                     DC-bias curve when it covers that voltage; a class-II-type ceramic
//                     (X5R / X7R ...) without one is UNVERIFIED — never derated by a guessed
//                     factor; any other dielectric keeps its stated capacitance, said so
//   ripple voltage    output: dV = dIpp * ESR + dIpp / (8 * fs * Ceff);
//                     input (standard buck input model, owner 2026-11-14):
//                     dV = Iout * D * (1 - D) / (fs * Ceff) + Iout * ESR;
//                     over the caller's maxRippleVoltage -> flagged, ranked after the
//                     candidates that meet it
//   ripple rating     the record's ripple-current rating moved to fs and to the ambient on its
//                     OWN frequency / temperature tables (the cross-reference's conversion),
//                     only inside each table's range; a rating stated at or above the ambient
//                     with no temperature table is kept as a conservative lower bound, said so
//                     (owner 2026-11-14); compared with I_ripple — a rating below it is flagged
//                     and ranks after the candidates that carry it (owner 2026-11-14)
// Every datasheet value is the record's, every operating-point value the caller's; a missing one
// leaves its term or check UNVERIFIED (no number, `missing` says what is absent).
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "Constraints.hpp"
#include "CrossRefCurves.hpp"
#include "MosfetLoss.hpp"  // LossTerm
#include "Rows.hpp"

namespace kelvin {

// A computed check value: the number and its basis, or nullopt and what is missing.
struct CapacitorCheck {
    std::optional<double> value;
    std::string basis;
    std::vector<std::string> missing;
};

struct CapacitorLossBreakdown {
    CapacitorPosition position = CapacitorPosition::BuckOutput;
    double duty = 0, i_ripple = 0, v_dc = 0;
    std::string duty_basis, i_ripple_basis;
    LossTerm esr_loss;
    std::optional<double> esr_at_fs;  // the ESR the loss term used (nullopt = UNVERIFIED)
    bool esr_upper_bound = false;     // esr_at_fs derived from tan(delta): an upper bound at fs
    CapacitorCheck c_eff;             // effective capacitance at v_dc
    CapacitorCheck ripple_voltage;    // the output or input ripple voltage
    std::optional<double> max_ripple_v;
    CapacitorCheck ripple_rating;     // the ripple-current rating at fs (and the ambient)
    std::vector<std::string> notes;

    // Both positions carry a ripple-voltage model since the owner's 2026-11-14 input model.
    bool has_ripple_voltage() const { return true; }
    // Names of the UNVERIFIED terms the ranking counts: the ESR loss, and the ripple voltage
    // when the caller set a limit on it (a candidate nobody can check against the limit is not
    // one that meets it).
    std::vector<std::string> unverified() const {
        std::vector<std::string> u;
        if (!esr_loss.watts) u.push_back("esrLoss");
        if (max_ripple_v && !ripple_voltage.value) u.push_back("rippleVoltage");
        return u;
    }
    double computed_sum() const { return esr_loss.watts ? *esr_loss.watts : 0.0; }
    std::optional<double> total() const {
        if (!unverified().empty()) return std::nullopt;
        return computed_sum();
    }
    // Over the caller's ripple-voltage limit (computed and above it).
    bool over_ripple_voltage() const {
        return max_ripple_v && ripple_voltage.value && *ripple_voltage.value > *max_ripple_v;
    }
    // nullopt = not checked (rating UNVERIFIED); else whether the rating covers I_ripple.
    std::optional<bool> ripple_rating_ok() const {
        if (!ripple_rating.value) return std::nullopt;
        return *ripple_rating.value >= i_ripple;
    }
    // The converted ripple-current rating is below the ripple it would carry (owner
    // 2026-11-14: ranks after those that carry it, with the over-limit ripple voltage).
    bool under_ripple_rating() const {
        const auto ok = ripple_rating_ok();
        return ok && !*ok;
    }
    // The operating-point ranking's second key: over the ripple-voltage limit OR under the
    // ripple-current rating.
    bool fails_ripple_checks() const { return over_ripple_voltage() || under_ripple_rating(); }
};

namespace capacitor_loss_detail {

constexpr double kPi = 3.14159265358979323846;

inline std::string fmt(const char* f, double a) {
    char b[96];
    std::snprintf(b, sizeof b, f, a);
    return b;
}

inline std::optional<crossref::Curve> to_curve(const CurvePts& c) {
    if (c.empty() || !c.y || c.y->size() != c.x->size() || c.x->size() < 2) return std::nullopt;
    std::vector<double> xs(c.x->begin(), c.x->end()), ys(c.y->begin(), c.y->end());
    return crossref::Curve(std::move(xs), std::move(ys));
}

inline bool same(double a, double b) {
    return std::fabs(a - b) <= 1e-9 * std::max({std::fabs(a), std::fabs(b), 1.0});
}
// 20 degC and 25 degC count as the same condition (owner 2026-11-14).
inline bool same_temp(double a, double b) {
    auto room = [](double t) { return same(t, 20.0) || same(t, 25.0); };
    return same(a, b) || (room(a) && room(b));
}

// A class-II-type ceramic (X5R / X7R / Y5V ...), whose capacitance falls with DC bias: the
// record's technology says ceramic class 2 or 3, or its dielectric code is an EIA class-II
// code (letter X/Y/Z, digit, letter).
inline bool class_ii_type(const CapacitorRow& r) {
    if (r.technology == "ceramic-class-2" || r.technology == "ceramic-class-3") return true;
    const std::string& d = r.dielectric_code;
    return d.size() == 3 && (d[0] == 'X' || d[0] == 'Y' || d[0] == 'Z') &&
           std::isdigit(static_cast<unsigned char>(d[1])) &&
           std::isupper(static_cast<unsigned char>(d[2]));
}

inline std::string curve_temp(const CurvePts& c) {
    return std::isnan(c.temperature) ? std::string(", curve temperature not stated")
                                     : fmt(", curve at %g degC", c.temperature);
}

}  // namespace capacitor_loss_detail

inline CapacitorLossBreakdown capacitor_loss(const CapacitorRow& x, const CapacitorOperatingPoint& op) {
    using namespace capacitor_loss_detail;
    CapacitorLossBreakdown b;
    b.position = op.position;
    const double fs = op.fsw;
    b.duty = op.duty();
    b.duty_basis = "D = Vout/Vin = " + fmt("%g V", op.vout) + " / " + fmt("%g V", op.vin) +
                   " (ideal CCM buck)";
    b.max_ripple_v = op.max_ripple_v;
    if (op.position == CapacitorPosition::BuckOutput) {
        b.i_ripple = *op.ripple_pp / std::sqrt(12.0);
        b.i_ripple_basis = "I_ripple = dIpp/sqrt(12) with dIpp " + fmt("%g A", *op.ripple_pp) +
                           " (the inductor's triangular ripple)";
        b.v_dc = op.vout;
    } else {
        b.i_ripple = op.iout * std::sqrt(b.duty * (1.0 - b.duty));
        b.i_ripple_basis = "I_ripple = Iout*sqrt(D*(1-D)) with Iout " + fmt("%g A", op.iout) +
                           fmt(", D %.4g", b.duty) + " (flat-top input current)";
        b.v_dc = op.vin;
    }

    // ---- ESR at fs and the ESR loss --------------------------------------------------
    {
        std::string basis, curve_miss;
        if (!x.esr_curve.empty()) {
            std::optional<double> e;
            if (auto c = to_curve(x.esr_curve)) e = crossref::curve_at(*c, fs, crossref::Interp::LogLog);
            if (e && *e > 0) {
                b.esr_at_fs = *e;
                basis = "ESR " + fmt("%.4g mohm", *e * 1e3) + fmt(" at fs %g Hz", fs) +
                        " read off the record's ESR(f) curve (log-log" + curve_temp(x.esr_curve) + ")";
            } else {
                curve_miss = fmt("; fs %g Hz is outside the record's ESR(f) curve", fs) +
                             fmt(" (%g", (*x.esr_curve.x).front()) +
                             fmt(" to %g Hz), not extrapolated", (*x.esr_curve.x).back());
            }
        }
        // A stated ESR with no stated frequency is UNVERIFIED (owner 2026-11-14): the frequency is
        // a condition of the value, and ESR moves with it by orders of magnitude.
        std::string nofreq_miss;
        if (!b.esr_at_fs && present(x.esr) && !x.esr_from_curve && !present(x.esr_frequency))
            nofreq_miss = "; the record states ESR " + fmt("%.4g mohm", x.esr * 1e3) +
                          " without its frequency (a condition of the value), so it is not "
                          "read at fs";
        if (!b.esr_at_fs && present(x.esr) && nofreq_miss.empty()) {
            b.esr_at_fs = x.esr;
            if (x.esr_from_curve)
                basis = "ESR " + fmt("%.4g mohm", x.esr * 1e3) +
                        " read off the record's ESR curve at 100 kHz (the record states no scalar "
                        "ESR)" + (same(1e5, fs) ? std::string() : fmt(", not at fs %g Hz", fs));
            else
                basis = "stated ESR " + fmt("%.4g mohm", x.esr * 1e3) +
                        fmt(" at %g Hz", x.esr_frequency) +
                        (same(x.esr_frequency, fs) ? std::string() : fmt(", not at fs %g Hz", fs));
            basis += curve_miss;
        }
        // No ESR read (owner 2026-11-14): ESR = tan(delta) / (2*pi*f_df*C) at tan(delta)'s own
        // frequency, an UPPER BOUND at fs (ESR falls with frequency) — which holds only for fs
        // at or above f_df; below it, or with no f_df, nothing is derived. An ESR stated without
        // its frequency is ignored (owner 2026-11-14): the record is treated as one stating no
        // ESR, so its tan(delta) applies the same way.
        std::string df_miss;
        if (!b.esr_at_fs && present(x.dissipation_factor)) {
            const double df = x.dissipation_factor, fdf = x.dissipation_factor_frequency;
            if (std::isnan(fdf)) {
                df_miss = fmt("; the record states tan(delta) %g", df) +
                          " without its frequency, so no ESR can be derived from it";
            } else if (!(x.capacitance > 0)) {
                df_miss = fmt("; the record states tan(delta) %g", df) +
                          " but no capacitance to derive an ESR from it";
            } else if (fs < fdf && !same(fs, fdf)) {
                df_miss = fmt("; the record states tan(delta) %g", df) + fmt(" at %g Hz,", fdf) +
                          fmt(" above fs %g Hz: the ESR derived from it is not an upper bound at fs", fs);
            } else {
                const double e = df / (2.0 * kPi * fdf * x.capacitance);
                b.esr_at_fs = e;
                b.esr_upper_bound = true;
                basis = "ESR <= " + fmt("%.4g mohm", e * 1e3) + fmt(" = tan(delta) %g", df) +
                        fmt(" / (2*pi * %g Hz", fdf) + fmt(" * %.4g uF)", x.capacitance * 1e6) +
                        fmt(" at tan(delta)'s stated %g Hz", fdf) +
                        (nofreq_miss.empty()
                             ? std::string(" (the record states no ESR)")
                             : " (the record's ESR " + fmt("%.4g mohm", x.esr * 1e3) +
                                   " states no frequency and is ignored)") +
                        ", an upper bound at " + fmt("fs %g Hz", fs) +
                        " since ESR falls with frequency";
                b.notes.push_back("ESR derived from tan(delta) at " + fmt("%g Hz", fdf) +
                                  ": the ESR loss and the ripple voltage are upper bounds");
            }
        }
        if (!b.esr_at_fs) {
            b.esr_loss.missing.push_back(
                (nofreq_miss.empty() ? "ESR not stated in the record"
                                     : "ESR at fs not stated in the record" + nofreq_miss) +
                curve_miss + df_miss);
        } else {
            b.esr_loss.watts = b.i_ripple * b.i_ripple * *b.esr_at_fs;
            b.esr_loss.basis = basis + "; " + b.i_ripple_basis + fmt(" = %.4g A", b.i_ripple);
        }
    }

    // ---- effective capacitance at the DC voltage -------------------------------------
    {
        CapacitorCheck& t = b.c_eff;
        std::string curve_miss;
        if (!x.bias_curve.empty()) {
            std::optional<double> c_at;
            if (auto c = to_curve(x.bias_curve))
                c_at = crossref::curve_at(*c, b.v_dc, crossref::Interp::Linear);
            if (c_at && *c_at > 0) {
                t.value = *c_at;
                t.basis = "Ceff " + fmt("%.4g uF", *c_at * 1e6) + fmt(" at %g V DC", b.v_dc) +
                          " read off the record's DC-bias curve (linear" + curve_temp(x.bias_curve) +
                          fmt("; nominal %.4g uF)", x.capacitance * 1e6);
            } else {
                curve_miss = fmt("; %g V DC is outside the record's DC-bias curve", b.v_dc) +
                             fmt(" (%g", (*x.bias_curve.x).front()) +
                             fmt(" to %g V), not extrapolated", (*x.bias_curve.x).back());
            }
        }
        if (!t.value) {
            if (class_ii_type(x)) {
                t.missing.push_back(
                    "effective capacitance at " + fmt("%g V DC", b.v_dc) + " not stated: a " +
                    (x.dielectric_code.empty() ? x.technology : x.dielectric_code) +
                    " ceramic loses capacitance with DC bias and the record " +
                    (x.bias_curve.empty() ? std::string("has no DC-bias curve")
                                          : std::string("DC-bias curve does not reach it")) +
                    curve_miss + " (not derated by a guessed factor)");
            } else {
                t.value = x.capacitance;
                t.basis = "stated capacitance " + fmt("%.4g uF", x.capacitance * 1e6) + " (" +
                          (x.technology.empty() ? std::string("technology not stated")
                                                : x.technology) +
                          ": not a class-II ceramic, no DC-bias derating applied)" + curve_miss;
            }
        }
    }

    // ---- ripple voltage (output: the inductor ripple; input: the standard buck input model) --
    if (b.has_ripple_voltage()) {
        CapacitorCheck& t = b.ripple_voltage;
        if (!b.esr_at_fs) t.missing.push_back("ESR at fs not known (see esrLoss)");
        if (!b.c_eff.value) t.missing.push_back("effective capacitance not known (see effectiveCapacitance)");
        const bool out = op.position == CapacitorPosition::BuckOutput;
        if (t.missing.empty()) {
            double v_esr, v_c;
            std::string formula;
            if (out) {
                const double dipp = *op.ripple_pp;
                v_esr = dipp * *b.esr_at_fs;
                v_c = dipp / (8.0 * fs * *b.c_eff.value);
                formula = "dV = dIpp*ESR + dIpp/(8*fs*Ceff) = ";
            } else {
                v_esr = op.iout * *b.esr_at_fs;
                v_c = op.iout * b.duty * (1.0 - b.duty) / (fs * *b.c_eff.value);
                formula = "dV_in = Iout*D*(1-D)/(fs*Ceff) + Iout*ESR (standard buck input model) = ";
            }
            t.value = v_esr + v_c;
            t.basis = formula + fmt("%.4g mV", v_esr * 1e3) + " (ESR) + " +
                      fmt("%.4g mV", v_c * 1e3) + " (Ceff)" +
                      (b.esr_upper_bound ? std::string("; an upper bound: the ESR is (see esrLoss)")
                                         : std::string()) +
                      (op.max_ripple_v ? fmt("; limit %.4g mV", *op.max_ripple_v * 1e3) : std::string());
        }
        if (b.over_ripple_voltage())
            b.notes.push_back(std::string(out ? "output" : "input") + " ripple " +
                              fmt("%.4g mV", *t.value * 1e3) + " is over the " +
                              fmt("%.4g mV", *op.max_ripple_v * 1e3) +
                              " limit: ranked after the candidates that meet it");
    }

    // ---- ripple-current rating at fs (and the ambient) --------------------------------
    {
        CapacitorCheck& t = b.ripple_rating;
        if (!present(x.ripple_current_rms)) {
            t.missing.push_back("ripple-current rating not stated in the record");
        } else {
            const double i0 = x.ripple_current_rms;
            double factor = 1.0;
            std::string how;
            // Frequency axis: the rating moved to fs on the record's own frequency table.
            const double f0 = x.ripple_current_frequency;
            if (std::isnan(f0)) {
                t.missing.push_back("the record states no frequency for its " + fmt("%g A", i0) +
                                    " ripple rating: it cannot be moved to fs");
            } else if (!same(f0, fs)) {
                std::optional<double> a, z;
                if (auto c = to_curve(x.ripple_f_curve)) {
                    a = crossref::curve_at(*c, fs, crossref::Interp::LogLog);
                    z = crossref::curve_at(*c, f0, crossref::Interp::LogLog);
                }
                if (a && z) {
                    factor *= *a / *z;
                    how += fmt("; x%.4g on the record's ripple-current frequency table", *a / *z) +
                           fmt(" (%g Hz", f0) + fmt(" -> %g Hz, log-log)", fs);
                } else {
                    t.missing.push_back(
                        fmt("ripple rating stated at %g Hz", f0) + "; " +
                        (x.ripple_f_curve.empty()
                             ? std::string("the record has no ripple-current frequency table")
                             : fmt("the record's ripple-current frequency table (%g", (*x.ripple_f_curve.x).front()) +
                                   fmt(" to %g Hz) does not cover both points", (*x.ripple_f_curve.x).back())) +
                        fmt(" to move it to fs %g Hz (not extrapolated)", fs));
                }
            }
            // Temperature axis: only when the caller states the ambient.
            const double t0 = x.ripple_current_temperature;
            std::string temp_basis;
            if (!op.ambient) {
                temp_basis = std::isnan(t0) ? std::string(" (rating temperature not stated; no ambient given)")
                                            : fmt(" at its stated %g degC (no ambient given)", t0);
            } else if (std::isnan(t0)) {
                t.missing.push_back("the record states no temperature for its ripple rating: it "
                                    "cannot be moved to the " + fmt("%g degC", *op.ambient) + " ambient");
            } else if (same_temp(t0, *op.ambient)) {
                temp_basis = fmt(" at %g degC", t0);
            } else if (x.ripple_t_curve.empty() && t0 > *op.ambient) {
                // Stated at a temperature above the ambient, no table (owner 2026-11-14): the
                // stated value is a conservative lower bound of the rating at the ambient.
                temp_basis = fmt(" at the %g degC ambient", *op.ambient) +
                             fmt(", taken as at least the rating stated at %g degC", t0) +
                             " (no temperature table: a conservative lower bound)";
                b.notes.push_back("ripple-current rating stated at " + fmt("%g degC", t0) +
                                  fmt(", above the %g degC ambient", *op.ambient) +
                                  ", with no temperature table: used as a conservative lower bound");
            } else {
                std::optional<double> a, z;
                if (auto c = to_curve(x.ripple_t_curve)) {
                    a = crossref::curve_at(*c, *op.ambient, crossref::Interp::Linear);
                    z = crossref::curve_at(*c, t0, crossref::Interp::Linear);
                }
                if (a && z) {
                    factor *= *a / *z;
                    how += fmt("; x%.4g on the record's ripple-current temperature table", *a / *z) +
                           fmt(" (%g degC", t0) + fmt(" -> %g degC ambient, linear)", *op.ambient);
                    temp_basis = fmt(" at %g degC ambient", *op.ambient);
                } else {
                    t.missing.push_back(
                        fmt("ripple rating stated at %g degC", t0) + "; " +
                        (x.ripple_t_curve.empty()
                             ? std::string("the record has no ripple-current temperature table")
                             : fmt("the record's ripple-current temperature table (%g", (*x.ripple_t_curve.x).front()) +
                                   fmt(" to %g degC) does not cover both points", (*x.ripple_t_curve.x).back())) +
                        fmt(" to move it to the %g degC ambient (not extrapolated)", *op.ambient));
                }
            }
            if (t.missing.empty()) {
                t.value = i0 * factor;
                t.basis = "ripple rating " + fmt("%.4g A", *t.value) + fmt(" at fs %g Hz", fs) +
                          temp_basis + fmt(" (stated %g A", i0) +
                          (std::isnan(f0) ? std::string() : fmt(" at %g Hz", f0)) +
                          (std::isnan(t0) ? std::string() : fmt(", %g degC", t0)) + ")" + how +
                          fmt("; needed %.4g A", b.i_ripple);
                if (*t.value < b.i_ripple)
                    b.notes.push_back("ripple-current rating " + fmt("%.4g A", *t.value) +
                                      " is below the " + fmt("%.4g A", b.i_ripple) +
                                      " ripple current at the operating point: ranked after "
                                      "the candidates that carry it");
            }
        }
    }
    return b;
}

}  // namespace kelvin
