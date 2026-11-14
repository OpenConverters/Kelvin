// Operating-point loss of a diode from its datasheet values (owner 2026-11-14). The diode
// counterpart of MosfetLoss.hpp: same LossTerm, same rules.
//
// Buck FREEWHEEL (catch) diode, CCM. The switch duty is D; the diode carries the inductor
// current for 1-D of the period:
//   conduction  P = Vf(If) * If * (1-D)       If = the current it conducts, Iout (flat top),
//                                              or the ramp Iout +/- dIpp/2 on the Vf(If) curve
//   switching   PN / fast recovery:  P = Qrr * Vin * fs
//               Schottky / SiC:      P = 1/2 * Qc * Vin * fs,  Qc = integral of Cj(Vr), 0..Vin
//   leakage     P = Ir(Vin) * Vin * D
// Every datasheet value is the record's; every operating-point value the caller's. A term
// whose input is missing is UNVERIFIED (no watts, `missing` names what is absent) — never a
// guessed number. Each computed term names its basis: which value, read where, at what
// condition, and a condition the record does not state is said to be not stated.
//
// Curves (Vf(If), Cj(Vr), Ir(Vr)) are read inside their measured range only (curve_at);
// the row's curve fields are passed in as DiodeCurves so this header does not depend on how
// a row carries them.
#pragma once
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "Constraints.hpp"
#include "CrossRefCurves.hpp"
#include "MosfetLoss.hpp"  // LossTerm, mosfet_loss_detail::fmt / to_curve
#include "Rows.hpp"

namespace kelvin {

struct DiodeLossBreakdown {
    double duty = 0;      // the switch duty D (the diode conducts for 1-D)
    double i_on = 0;      // the current the diode conducts (Iout)
    double i_avg = 0;     // its period average, Iout * (1-D)
    std::string duty_basis;
    LossTerm conduction, switching, leakage;
    std::vector<std::string> notes;
    std::vector<std::string> unverified() const {
        std::vector<std::string> u;
        if (!conduction.watts) u.push_back("conduction");
        if (!switching.watts) u.push_back("switching");
        if (!leakage.watts) u.push_back("leakage");
        return u;
    }
    double computed_sum() const {
        double s = 0;
        for (const LossTerm* t : {&conduction, &switching, &leakage})
            if (t->watts) s += *t->watts;
        return s;
    }
    std::optional<double> total() const {
        if (!unverified().empty()) return std::nullopt;
        return computed_sum();
    }
};

// The record's curves the terms can read; a null pointer or an empty curve = none on record.
// x / y in SI: Vf(If) x = If [A] y = Vf [V]; Cj(Vr) x = Vr [V] y = Cj [F]; Ir(Vr) x = Vr [V]
// y = Ir [A].
struct DiodeCurves {
    const CurvePts* vf_if = nullptr;
    const CurvePts* cj_vr = nullptr;
    const CurvePts* ir_vr = nullptr;
};

namespace diode_loss_detail {

using mosfet_loss_detail::fmt;
using mosfet_loss_detail::to_curve;

inline bool usable(const CurvePts* c) { return c && !c->empty(); }

// The curve's stated temperature against the operating Tj, for the basis: "at 25 degC",
// "at 125 degC, not at Tj 100 degC", "temperature not stated". 20 and 25 degC count as the
// same (owner 2026-11-14).
inline std::string curve_temperature(const CurvePts& c, const std::optional<double>& tj) {
    std::string s;
    if (!present(c.temperature)) {
        s = "curve temperature not stated";
    } else {
        s = fmt("curve at %g degC", c.temperature);
        if (tj && !crossref::same_stated_value(c.temperature, *tj) &&
            !crossref::same_room_temperature(c.temperature, *tj))
            s += fmt(", not at the operating Tj %g degC", *tj);
    }
    if (c.unit_inferred) s += "; units inferred: " + c.unit_note;
    return s;
}

// Why a curve the record states was not read: it gives no units its own scalar confirms
// (Views.cpp settle_unit_unstated keeps its points out). "" when there is no such curve.
inline std::string unread_curve(const CurvePts* c, const char* name) {
    if (!c || !c->unit_unstated) return "";
    return std::string("; the record's ") + name + " curve states no units" +
           (c->unit_note.empty() ? std::string() : " (" + c->unit_note + ")") + ", not read";
}

inline std::string range_text(const CurvePts& c, const char* unit) {
    return fmt("(%g", (*c.x).front()) + fmt(" to %g ", (*c.x).back()) + unit + ")";
}

// The charge integral of Cj from 0 to v on the curve (trapezoid between samples, the end
// read linearly). nullopt unless the curve's measured range covers [0, v]: the charge below
// its first sample is not on record and is not extrapolated.
inline std::optional<double> charge_0_to(const crossref::Curve& c, double v) {
    if (c.x.front() > 0 || c.x.back() < v) return std::nullopt;
    double q = 0;
    for (std::size_t i = 0; i + 1 < c.x.size() && c.x[i] < v; ++i) {
        const double x0 = c.x[i];
        const double x1 = std::min(c.x[i + 1], v);
        const auto y1 = crossref::curve_at(c, x1, crossref::Interp::Linear);
        q += 0.5 * (c.y[i] + *y1) * (x1 - x0);
    }
    return q;
}

}  // namespace diode_loss_detail

inline DiodeLossBreakdown buck_freewheel_loss(const DiodeRow& d, const DiodeLossOperatingPoint& op,
                                              const DiodeCurves& cv = {}) {
    using namespace diode_loss_detail;
    DiodeLossBreakdown b;
    const double vin = op.vin, iout = op.iout, fs = op.fsw;
    b.duty = op.duty_value();
    b.duty_basis = op.duty ? "duty cycle as given"
                           : "D = Vout/Vin = " + fmt("%g", *op.vout) + " V / " + fmt("%g V", vin) +
                                 " (ideal CCM buck)";
    b.i_on = iout;
    b.i_avg = iout * (1 - b.duty);
    const std::string avg_text = "If(avg) = Iout*(1-D) = " + fmt("%.4g A", b.i_avg);

    if (d.die_configuration == "dual" || d.die_configuration == "array" ||
        (present(d.number_of_elements) && d.number_of_elements >= 2))
        b.notes.push_back("the record's values are per diode (" +
                          (d.die_configuration.empty() ? fmt("%g elements", d.number_of_elements)
                                                       : d.die_configuration) +
                          "): the loss is for one diode carrying the whole current");

    // ---- conduction: Vf(If) * If * (1-D) -----------------------------------------------
    {
        LossTerm& t = b.conduction;
        std::string curve_miss = unread_curve(cv.vf_if, "Vf(If)");
        if (usable(cv.vf_if)) {
            const CurvePts& cp = *cv.vf_if;
            if (auto c = to_curve(cp)) {
                const std::string cond = curve_temperature(cp, op.tj);
                if (op.ripple_pp && *op.ripple_pp > 0) {
                    // The current ramps linearly over the off time, so the average of
                    // Vf(i)*i over the ramp, times 1-D, is the conduction loss.
                    const double lo = iout - *op.ripple_pp / 2, hi = iout + *op.ripple_pp / 2;
                    constexpr int kN = 64;
                    double acc = 0;
                    bool inside = true;
                    for (int k = 0; k < kN && inside; ++k) {
                        const double i = lo + (hi - lo) * (k + 0.5) / kN;
                        const auto v = i > 0 ? crossref::curve_at(*c, i, crossref::Interp::LogLog)
                                             : std::nullopt;
                        if (!v) inside = false;
                        else acc += *v * i;
                    }
                    // Both ends of the ramp must lie on the curve too.
                    if (inside && lo > 0 && crossref::curve_at(*c, lo, crossref::Interp::LogLog) &&
                        crossref::curve_at(*c, hi, crossref::Interp::LogLog)) {
                        t.watts = acc / kN * (1 - b.duty);
                        t.basis = "Vf(If)*If averaged over the freewheel ramp " +
                                  fmt("%.4g", lo) + fmt(" to %.4g A", hi) +
                                  " read off the record's Vf(If) curve (log-log, " + cond +
                                  "), times (1-D) = " + fmt("%.4g", 1 - b.duty) + "; " + avg_text;
                    } else {
                        curve_miss = "; the ramp " + fmt("%.4g", lo) + fmt(" to %.4g A", hi) +
                                     " leaves the record's Vf(If) curve " + range_text(cp, "A") +
                                     ", not extrapolated";
                    }
                } else {
                    const auto v = crossref::curve_at(*c, iout, crossref::Interp::LogLog);
                    if (v && *v > 0) {
                        t.watts = *v * b.i_avg;
                        t.basis = "Vf " + fmt("%.4g V", *v) + fmt(" at If %g A", iout) +
                                  " (the current it conducts, flat top: no inductor ripple given)"
                                  " read off the record's Vf(If) curve (log-log, " +
                                  cond + ") x " + avg_text;
                    } else {
                        curve_miss = fmt("; If %g A is outside the record's Vf(If) curve ", iout) +
                                     range_text(cp, "A") + ", not extrapolated";
                    }
                }
            }
        }
        if (!t.watts) {
            if (!present(d.vf_typ)) {
                t.missing.push_back("Vf not stated in the record" + curve_miss);
            } else {
                std::string at;
                if (present(d.vf_test_current)) {
                    at = fmt(" at If %g A", d.vf_test_current);
                    const double r = d.vf_test_current / iout;
                    if (r > 2.0 || r < 0.5) {
                        at += fmt(", more than 2x from the %g A it conducts here: Vf at the "
                                  "operating current is not on record",
                                  iout);
                        b.notes.push_back("Vf is stated at " + fmt("%g A", d.vf_test_current) +
                                          ", more than 2x from the " + fmt("%g A", iout) +
                                          " the diode conducts: the conduction term rests on a "
                                          "Vf at another current");
                    }
                } else {
                    at = " (test current not stated)";
                }
                t.watts = d.vf_typ * b.i_avg;
                t.basis = "stated Vf " + fmt("%.4g V", d.vf_typ) + at +
                          ", temperature not stated in the record" + curve_miss + "; x " + avg_text;
            }
        }
    }

    // ---- switching ---------------------------------------------------------------------
    {
        LossTerm& t = b.switching;
        const std::string& sub = d.technology;
        const bool capacitive = sub == "schottky" || sub == "sicSchottky";
        const bool recovery = sub == "rectifier" || sub == "fastRecovery" || sub == "ultrafast" ||
                              sub == "switching" || sub == "pin";
        if (capacitive) {
            // 1/2 * Qc * Vin * fs, Qc from the Cj(Vr) curve over 0..Vin. SAS carries no stated
            // Qc; a single Cj at one bias does not give the charge, and a stated Qrr is not
            // read as Qc.
            std::optional<double> qc;
            std::string why;
            if (usable(cv.cj_vr)) {
                const CurvePts& cp = *cv.cj_vr;
                if (auto c = to_curve(cp)) {
                    qc = charge_0_to(*c, vin);
                    if (qc && *qc > 0) {
                        t.watts = 0.5 * *qc * vin * fs;
                        t.basis = "1/2*Qc*Vin*fs with Qc " + fmt("%.4g nC", *qc * 1e9) +
                                  fmt(" = integral of Cj(Vr) from 0 to %g V", vin) +
                                  " on the record's Cj(Vr) curve (trapezoid, " +
                                  curve_temperature(cp, op.tj) + ")";
                    } else {
                        why = "the record's Cj(Vr) curve " + range_text(cp, "V") +
                              fmt(" does not cover 0 to %g V: the charge outside it is not on "
                                  "record, not extrapolated",
                                  vin);
                    }
                }
            }
            if (!t.watts) {
                if (why.empty() && cv.cj_vr && cv.cj_vr->unit_unstated)
                    why = "Qc not on record (SAS states no Qc)" + unread_curve(cv.cj_vr, "Cj(Vr)");
                if (why.empty()) why = "Qc not on record: the record has no Cj(Vr) curve (SAS states no Qc)";
                if (present(d.junction_capacitance))
                    why += "; the single stated Cj " + fmt("%.4g pF", d.junction_capacitance * 1e12) +
                           (present(d.junction_capacitance_vr)
                                ? fmt(" at %g V", d.junction_capacitance_vr)
                                : std::string()) +
                           " does not give the charge over 0 to Vin";
                if (present(d.qrr) && d.qrr > 0)
                    why += "; the stated Qrr " + fmt("%.4g nC", d.qrr * 1e9) +
                           " is not read as a capacitive charge";
                t.missing.push_back(why);
            }
        } else if (recovery) {
            if (!present(d.qrr)) {
                t.missing.push_back(
                    "Qrr not stated in the record" +
                    (present(d.trr) ? fmt(" (trr %.4g ns is stated; Qrr is not derived from it)",
                                          d.trr * 1e9)
                                    : std::string()));
            } else {
                t.watts = d.qrr * vin * fs;
                t.basis = "Qrr*Vin*fs with stated Qrr " + fmt("%.4g nC", d.qrr * 1e9) +
                          " (the record states none of its test conditions: If, di/dt, Vr, Tj); "
                          "the recovery loss the diode causes, most of it dissipated in the "
                          "high-side switch";
            }
        } else {
            t.missing.push_back(sub.empty() ? std::string("the record states no diode subtype, so "
                                                          "neither the recovery-charge nor the "
                                                          "capacitive model applies")
                                            : "subtype '" + sub +
                                                  "' has no freewheel switching model");
        }
    }

    // ---- leakage: Ir(Vin) * Vin * D ----------------------------------------------------
    {
        LossTerm& t = b.leakage;
        std::string curve_miss = unread_curve(cv.ir_vr, "Ir(Vr)");
        if (usable(cv.ir_vr)) {
            const CurvePts& cp = *cv.ir_vr;
            if (auto c = to_curve(cp)) {
                const auto ir = crossref::curve_at(*c, vin, crossref::Interp::LogLog);
                if (ir && *ir > 0) {
                    t.watts = *ir * vin * b.duty;
                    t.basis = "Ir " + fmt("%.4g uA", *ir * 1e6) + fmt(" at Vr %g V", vin) +
                              " read off the record's Ir(Vr) curve (log-log, " +
                              curve_temperature(cp, op.tj) + ") x Vin x D";
                } else {
                    curve_miss = fmt("; Vin %g V is outside the record's Ir(Vr) curve ", vin) +
                                 range_text(cp, "V") + ", not extrapolated";
                }
            }
        }
        if (!t.watts) {
            if (!present(d.reverse_leakage)) {
                t.missing.push_back("Ir not stated in the record" + curve_miss);
            } else {
                t.watts = d.reverse_leakage * vin * b.duty;
                t.basis = "stated Ir " + fmt("%.4g uA", d.reverse_leakage * 1e6) +
                          " (the record states neither its reverse voltage nor its temperature; "
                          "leakage rises steeply with Tj)" +
                          curve_miss + " x Vin x D";
            }
        }
    }
    return b;
}

// The breakdown at the operating point's diode position.
inline DiodeLossBreakdown diode_loss(const DiodeRow& d, const DiodeLossOperatingPoint& op,
                                     const DiodeCurves& cv = {}) {
    switch (op.position) {
        case DiodePosition::BuckFreewheel: return buck_freewheel_loss(d, op, cv);
    }
    throw InvalidOptions("diode_loss: unhandled diode position");
}

}  // namespace kelvin
