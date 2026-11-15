// Operating-point loss of a MOSFET switch from its datasheet values (owner 2026-11-14).
//
// Method per TI SLVAEQ9, Fairchild AN-6005 and the ROHM buck-efficiency application note,
// for the hard-switched buck HIGH side:
//   conduction  P = Irms^2 * Rds(on)            Irms = sqrt(D * (Iout^2 + dIpp^2 / 12))
//   switching   P = Vin * Iout * (Qgs + Qgd) / Ig * fs
//   gate drive  P = Qg(Vdrive) * Vdrive * fs
//   Coss        P = 1/2 * Coss(Vin) * Vin^2 * fs
// Every datasheet value is the record's; every operating-point value the caller's. A term
// whose input is missing is UNVERIFIED (no watts, `missing` names what is absent) — never a
// guessed number. Each computed term names its basis (which value, read where, at what
// condition), so a reader can see e.g. that an Rds(on) is the 25 degC headline and not the
// value at the stated Tj.
//
// The low side / synchronous rectifier is a further SwitchPosition with its own terms here;
// the selector does not change.
#pragma once
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "Constraints.hpp"
#include "CrossRefCurves.hpp"
#include "Rows.hpp"

namespace kelvin {

struct LossTerm {
    std::optional<double> watts;       // nullopt = UNVERIFIED
    std::string basis;                 // what the number rests on (empty when unverified)
    std::vector<std::string> missing;  // what is absent (empty when computed)
};

struct LossBreakdown {
    double duty = 0, irms = 0;
    std::string duty_basis, irms_basis;
    LossTerm conduction, switching, gate_drive, coss;
    std::vector<std::string> notes;
    // Names of the UNVERIFIED terms, in term order.
    std::vector<std::string> unverified() const {
        std::vector<std::string> u;
        if (!conduction.watts) u.push_back("conduction");
        if (!switching.watts) u.push_back("switching");
        if (!gate_drive.watts) u.push_back("gateDrive");
        if (!coss.watts) u.push_back("coss");
        return u;
    }
    // Sum of the computed terms (the total when nothing is unverified).
    double computed_sum() const {
        double s = 0;
        for (const LossTerm* t : {&conduction, &switching, &gate_drive, &coss})
            if (t->watts) s += *t->watts;
        return s;
    }
    std::optional<double> total() const {
        if (!unverified().empty()) return std::nullopt;
        return computed_sum();
    }
};

namespace mosfet_loss_detail {

inline std::string fmt(const char* f, double a) {
    char b[96];
    std::snprintf(b, sizeof b, f, a);
    return b;
}

inline std::optional<crossref::Curve> to_curve(const CurvePts& c) {
    if (c.empty() || !c.y || c.y->size() != c.x->size()) return std::nullopt;
    std::vector<double> xs(c.x->begin(), c.x->end()), ys(c.y->begin(), c.y->end());
    return crossref::Curve(std::move(xs), std::move(ys));
}

// The magnitude of a condition the record states for a curve (`key`, e.g. gateSourceVoltage),
// NaN when it states none. A P-channel record states its gate / drain voltages negative; a
// negative value on an N-channel record is not a magnitude of anything and is not read (the
// rule the row applies to the scalar ratings, Views.cpp rating_magnitude).
inline double condition_magnitude(const MosfetRow& m, const CurvePts& c, const char* key) {
    for (std::size_t i = 0; i < c.cond_keys.size(); ++i) {
        if (c.cond_keys[i] != key) continue;
        const double v = c.cond_vals[i];
        if (v < 0) return m.channel == "pChannel" ? -v : kNaN();
        return v > 0 ? v : kNaN();
    }
    return kNaN();
}

// Whether every sample is positive: a curve read on log axes (Coss(Vds)) holding a 0 V or a
// non-positive sample cannot be read log-log, and is then not read at all.
inline bool loglog_readable(const crossref::Curve& c) {
    for (std::size_t i = 0; i < c.x.size(); ++i)
        if (!(c.x[i] > 0) || !(c.y[i] > 0)) return false;
    return true;
}

// The charge at which the gate-charge curve (x = Qg [C], y = Vgs [V]) FIRST reaches `v`:
// the charge the driver has delivered when the gate arrives at its drive voltage. Linear
// between the bracketing samples; nullopt when the curve never reaches `v` (no extrapolation).
inline std::optional<double> charge_at_vgs(const crossref::Curve& q, double v) {
    for (std::size_t i = 0; i + 1 < q.x.size(); ++i) {
        const double y0 = q.y[i], y1 = q.y[i + 1];
        if (y0 == v) return q.x[i];
        if ((y0 < v && v <= y1) || (y0 > v && v >= y1))
            return q.x[i] + (v - y0) / (y1 - y0) * (q.x[i + 1] - q.x[i]);
    }
    return std::nullopt;
}

}  // namespace mosfet_loss_detail

inline LossBreakdown buck_high_side_loss(const MosfetRow& m, const LossOperatingPoint& op) {
    using namespace mosfet_loss_detail;
    LossBreakdown b;
    const double vin = op.vin, iout = op.iout, fs = op.fsw;
    b.duty = op.duty_value();
    b.duty_basis = op.duty ? "duty cycle as given"
                           : "D = Vout/Vin = " + fmt("%g", *op.vout) + " V / " + fmt("%g V", vin) +
                                 " (ideal CCM buck)";
    if (op.ripple_pp) {
        b.irms = std::sqrt(b.duty * (iout * iout + *op.ripple_pp * *op.ripple_pp / 12.0));
        b.irms_basis = "Irms = sqrt(D*(Iout^2 + dIpp^2/12)) with dIpp " + fmt("%g A", *op.ripple_pp);
    } else {
        b.irms = std::sqrt(b.duty) * iout;
        b.irms_basis = "Irms = sqrt(D)*Iout (no inductor ripple given: flat-top current)";
    }

    // ---- conduction: Irms^2 * Rds(on) -------------------------------------------------
    {
        double rds = m.rds_on;
        std::string basis;
        const std::string at_vgs =
            present(m.rds_on_vgs) ? fmt(" at Vgs %g V", m.rds_on_vgs) : std::string(", Vgs not stated");
        const std::string stated = "stated Rds(on) " + fmt("%.4g mohm", m.rds_on * 1e3) + at_vgs +
                                   " (the record gives no Tj: datasheet headline, 25 degC)";
        const double tj_curve_vgs = condition_magnitude(m, m.curves->rds_on_tj_curve, "gateSourceVoltage");
        if (op.tj && !m.curves->rds_on_tj_curve.empty()) {
            std::optional<double> r;
            if (auto c = to_curve(m.curves->rds_on_tj_curve))
                r = crossref::curve_at(*c, *op.tj, crossref::Interp::Linear);
            if (r && *r > 0) {
                rds = *r;
                basis = "Rds(on) " + fmt("%.4g mohm", rds * 1e3) + fmt(" at Tj %g degC", *op.tj) +
                        " read off the record's Rds(on)(Tj) curve (linear)" +
                        (present(tj_curve_vgs) ? fmt(", curve at Vgs %g V", tj_curve_vgs)
                                                        : std::string(", curve Vgs not stated"));
            } else {
                basis = stated + "; Tj " + fmt("%g degC", *op.tj) + " is outside the record's " +
                        fmt("Rds(on)(Tj) curve (%g", (*m.curves->rds_on_tj_curve.x).front()) +
                        fmt(" to %g degC), not extrapolated", (*m.curves->rds_on_tj_curve.x).back());
            }
        } else if (op.tj) {
            basis = stated + "; the record has no Rds(on)(Tj) curve to read it at Tj " +
                    fmt("%g degC", *op.tj);
        } else {
            basis = stated;
        }
        b.conduction.watts = b.irms * b.irms * rds;
        b.conduction.basis = basis + "; " + b.irms_basis + fmt(" = %.4g A", b.irms);
        if (op.vdrive && present(m.rds_on_vgs) && *op.vdrive < m.rds_on_vgs)
            b.notes.push_back("Rds(on) is stated at Vgs " + fmt("%g V", m.rds_on_vgs) +
                              ", above the " + fmt("%g V", *op.vdrive) +
                              " gate drive: the conduction term is understated");
    }

    // ---- switching: Vin * Iout * (Qgs + Qgd) / Ig * fs ----------------------------------
    {
        LossTerm& t = b.switching;
        if (!op.idrive) t.missing.push_back("gate drive current not given");
        if (!present(m.qgs)) t.missing.push_back("Qgs not stated in the record");
        if (!present(m.qgd)) t.missing.push_back("Qgd not stated in the record");
        if (t.missing.empty()) {
            t.watts = vin * iout * (m.qgs + m.qgd) / *op.idrive * fs;
            t.basis = "Vin*Iout*(Qgs+Qgd)/Ig*fs with Qgs " + fmt("%.4g nC", m.qgs * 1e9) +
                      " (the whole stated gate-source charge: the record states no Qgs2, so "
                      "this is an upper bound), Qgd " + fmt("%.4g nC", m.qgd * 1e9) + ", Ig " +
                      fmt("%g A", *op.idrive);
        }
    }

    // ---- gate drive: Qg(Vdrive) * Vdrive * fs ------------------------------------------
    {
        LossTerm& t = b.gate_drive;
        if (!op.vdrive) {
            t.missing.push_back("gate drive voltage not given");
        } else {
            std::optional<double> qg;
            std::string basis;
            std::string curve_miss;
            const double qg_curve_vds = condition_magnitude(m, m.curves->qg_curve, "drainSourceVoltage");
            if (!m.curves->qg_curve.empty()) {
                if (auto c = to_curve(m.curves->qg_curve)) qg = charge_at_vgs(*c, *op.vdrive);
                if (qg && *qg > 0) {
                    basis = "Qg " + fmt("%.4g nC", *qg * 1e9) + fmt(" at Vgs %g V", *op.vdrive) +
                            " read off the record's gate-charge curve" +
                            (present(qg_curve_vds) ? fmt(" (curve at Vds %g V)", qg_curve_vds)
                                                     : std::string(" (curve Vds not stated)"));
                } else {
                    qg.reset();
                    curve_miss = fmt("; the gate-charge curve does not reach Vgs %g V", *op.vdrive);
                }
            }
            if (!qg && present(m.qg_total)) {
                qg = m.qg_total;
                basis = "stated Qg " + fmt("%.4g nC", m.qg_total * 1e9) +
                        (present(m.qg_vgs) ? fmt(" at Vgs %g V", m.qg_vgs)
                                           : std::string(" (Vgs not stated)")) +
                        curve_miss;
                if (present(m.qg_vgs) && m.qg_vgs != *op.vdrive)
                    b.notes.push_back("stated Qg is at Vgs " + fmt("%g V", m.qg_vgs) +
                                      ", not at the " + fmt("%g V", *op.vdrive) + " gate drive");
            }
            if (!qg) {
                t.missing.push_back("Qg not stated in the record");
            } else {
                t.watts = *qg * *op.vdrive * fs;
                t.basis = basis;
            }
        }
    }

    // ---- Coss: 1/2 * Coss(Vin) * Vin^2 * fs ---------------------------------------------
    {
        LossTerm& t = b.coss;
        std::optional<double> c_at;
        std::string basis;
        std::string curve_miss;
        if (!m.curves->coss_curve.empty()) {
            const auto c = to_curve(m.curves->coss_curve);
            const bool readable = c && loglog_readable(*c);
            if (readable) c_at = crossref::curve_at(*c, vin, crossref::Interp::LogLog);
            if (!readable) {
                curve_miss = "; the record's Coss(Vds) curve holds a zero or negative sample and "
                             "is not read on log axes";
            } else if (c_at && *c_at > 0) {
                basis = "Coss " + fmt("%.4g pF", *c_at * 1e12) + fmt(" at Vds %g V", vin) +
                        " read off the record's Coss(Vds) curve (log-log)";
            } else {
                c_at.reset();
                curve_miss = fmt("; Vin %g V is outside the record's Coss(Vds) curve", vin) +
                             fmt(" (%g", (*m.curves->coss_curve.x).front()) +
                             fmt(" to %g V), not extrapolated", (*m.curves->coss_curve.x).back());
            }
        }
        if (!c_at && present(m.coss)) {
            c_at = m.coss;
            basis = "stated Coss " + fmt("%.4g pF", m.coss * 1e12) +
                    (present(m.coss_vds) ? fmt(" at Vds %g V", m.coss_vds) +
                                               (m.coss_vds != vin ? fmt(", not at Vin %g V", vin)
                                                                  : std::string())
                                         : std::string(" (Vds not stated)")) +
                    curve_miss;
        }
        if (!c_at) {
            t.missing.push_back("Coss not stated in the record");
        } else {
            t.watts = 0.5 * *c_at * vin * vin * fs;
            t.basis = basis;
        }
    }
    return b;
}

// The breakdown at the operating point's switch position.
inline LossBreakdown switch_loss(const MosfetRow& m, const LossOperatingPoint& op) {
    switch (op.position) {
        case SwitchPosition::BuckHighSide: return buck_high_side_loss(m, op);
    }
    throw InvalidOptions("switch_loss: unhandled switch position");
}

}  // namespace kelvin
