// Operating-point loss and checks of a power inductor from its datasheet values (owner
// 2026-11-14), the magnetic twin of MosfetLoss.hpp.
//
// Buck output inductor, ideal CCM:
//   duty     D   = Vout / Vin
//   ripple   dI  = (Vin - Vout) * D / (fs * L)      L = the candidate's OWN inductance, read at
//                                                   Ipk off its L(I) curve when it states one
//   peak     Ipk = Iout + dI / 2
//   rms      Irms = sqrt(Iout^2 + dI^2 / 12)
// Terms:
//   DCR      P = Irms^2 * DCR (the record's stated maximum, else its nominal; the basis says which)
//   core/AC  UNVERIFIED: magnetics math lives in MKF only, so Kelvin computes no core-loss model;
//            no catalogue record states a loss at an operating point. Interim owner rule
//            (2026-11-14): this one term does not push a candidate behind the others.
// Checks (hard gates in the selector):
//   saturation  the stated Isat must exceed Ipk; no Isat and no L(I) curve -> UNVERIFIED
//   thermal     Irms must not exceed the rated (temperature-rise) current; none -> UNVERIFIED
// Every datasheet value is the record's, every operating-point value the caller's. A curve is
// read only inside its measured range (never extrapolated).
#pragma once
#include <cmath>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "Constraints.hpp"
#include "CrossRefCurves.hpp"
#include "MosfetLoss.hpp"  // LossTerm
#include "Rows.hpp"

namespace kelvin {

// A pass / fail check with its basis, or UNVERIFIED with what is missing.
struct MagneticCheck {
    enum class Status { Pass, Fail, Unverified };
    Status status = Status::Unverified;
    std::string basis;                 // what the verdict rests on (pass / fail)
    std::vector<std::string> missing;  // what is absent (unverified)
    bool unverified() const { return status == Status::Unverified; }
    bool failed() const { return status == Status::Fail; }
    const char* str() const {
        switch (status) {
            case Status::Pass: return "pass";
            case Status::Fail: return "fail";
            case Status::Unverified: break;
        }
        return "unverified";
    }
};

struct MagneticLossBreakdown {
    double duty = 0, inductance = 0, ripple_pp = 0, ipk = 0, irms = 0;
    std::string duty_basis, inductance_basis, ipk_basis, irms_basis;
    LossTerm dcr, core;
    MagneticCheck saturation, thermal;
    std::vector<std::string> notes;
    // Names of the UNVERIFIED terms and checks, in order.
    std::vector<std::string> unverified() const {
        std::vector<std::string> u;
        if (!dcr.watts) u.push_back("dcr");
        if (!core.watts) u.push_back("coreLoss");
        if (saturation.unverified()) u.push_back("saturation");
        if (thermal.unverified()) u.push_back("thermal");
        return u;
    }
    // The UNVERIFIED count the ranking keys on: every term and check except the core loss
    // (interim owner rule 2026-11-14 — no record states one, so it would order nothing).
    std::size_t rank_unverified() const {
        std::size_t n = 0;
        for (const auto& u : unverified())
            if (u != "coreLoss") ++n;
        return n;
    }
    double computed_sum() const {
        double s = 0;
        for (const LossTerm* t : {&dcr, &core})
            if (t->watts) s += *t->watts;
        return s;
    }
    // The total only when every loss term is computed.
    std::optional<double> total() const {
        if (!dcr.watts || !core.watts) return std::nullopt;
        return computed_sum();
    }
};

namespace magnetic_loss_detail {

inline std::string fmt(const char* f, double a) {
    char b[96];
    std::snprintf(b, sizeof b, f, a);
    return b;
}

}  // namespace magnetic_loss_detail

inline MagneticLossBreakdown buck_inductor_loss(const MagneticRow& m,
                                                const InductorOperatingPoint& op) {
    using magnetic_loss_detail::fmt;
    if (!present(m.inductance) || !(m.inductance > 0))
        throw std::invalid_argument("buck_inductor_loss: the record states no inductance");
    MagneticLossBreakdown b;
    const double vin = op.vin, vout = op.vout, iout = op.iout, fs = op.fsw;
    b.duty = op.duty();
    b.duty_basis = "D = Vout/Vin = " + fmt("%g V", vout) + " / " + fmt("%g V", vin) +
                   " (ideal CCM buck)";
    const double volt_sec = (vin - vout) * b.duty / fs;  // V*s: dI = volt_sec / L
    const double L0 = m.inductance;
    const double ipk_nominal = iout + volt_sec / (2.0 * L0);

    // ---- L at Ipk off the record's L(I) curve ---------------------------------------------
    // The row keeps the curve as L/L(0) against current; L(Ipk) = nominal * ratio(Ipk). Ipk
    // depends on L(Ipk), so solve h(I) = Iout + volt_sec / (2 * L0 * ratio(I)) - I = 0 by
    // bisection on [Iout, curve end]: h(Iout) > 0 always; h(end) > 0 means the operating
    // point lies beyond the measured curve (not extrapolated).
    enum class Read { NoCurve, Unreadable, InRange, Beyond } read = Read::NoCurve;
    double curve_end = 0, ratio_at = 1;
    std::string unreadable_why;
    std::optional<crossref::Curve> curve;
    if (m.bias_curve_current.size() >= 2 &&
        m.bias_curve_current.size() == m.bias_curve_ratio.size()) {
        // A record whose curve repeats a current (two readings at one bias) cannot be read at
        // a point without choosing between them: it is not read, and the basis says why.
        try {
            curve.emplace(m.bias_curve_current, m.bias_curve_ratio);
        } catch (const std::invalid_argument& e) {
            read = Read::Unreadable;
            unreadable_why = e.what();
        }
    }
    if (curve) {
        const crossref::Curve& c = *curve;
        curve_end = c.x.back();
        auto h = [&](double i) -> std::optional<double> {
            const auto r = crossref::curve_at(c, i, crossref::Interp::Linear);
            if (!r || !(*r > 0)) return std::nullopt;
            return iout + volt_sec / (2.0 * L0 * *r) - i;
        };
        const auto h_end = h(curve_end);
        if (iout > curve_end || !h_end || *h_end > 0) {
            read = Read::Beyond;
        } else {
            double lo = iout, hi = curve_end;
            for (int k = 0; k < 200 && hi - lo > 1e-12 * curve_end; ++k) {
                const double mid = 0.5 * (lo + hi);
                const auto hm = h(mid);
                if (!hm) { read = Read::Beyond; break; }  // a non-positive sample: no reading
                (*hm > 0 ? lo : hi) = mid;
            }
            if (read != Read::Beyond) {
                read = Read::InRange;
                b.ipk = hi;
                ratio_at = *crossref::curve_at(c, b.ipk, crossref::Interp::Linear);
            }
        }
    }
    if (read == Read::InRange) {
        b.inductance = L0 * ratio_at;
        b.inductance_basis =
            "L " + fmt("%.4g uH", b.inductance * 1e6) + fmt(" at Ipk %.4g A", b.ipk) +
            ": the nominal " + fmt("%.4g uH", L0 * 1e6) + fmt(" x L/L(0) %.4g", ratio_at) +
            " read off the record's L(I) curve (linear; Ipk and the L it is read at solved "
            "together)";
    } else {
        b.inductance = L0;
        b.ipk = ipk_nominal;
        b.inductance_basis = "the record's nominal L " + fmt("%.4g uH", L0 * 1e6);
        if (read == Read::NoCurve)
            b.inductance_basis += " (the record states no L(I) curve: any roll-off below Isat "
                                  "is not in the ripple)";
        else if (read == Read::Unreadable)
            b.inductance_basis += " (the record's L(I) curve is not read: " + unreadable_why +
                                  "; any roll-off below Isat is not in the ripple)";
        else
            b.inductance_basis += "; Ipk lies beyond the record's L(I) curve (measured to " +
                                  fmt("%.4g A", curve_end) +
                                  "), so L at Ipk is not read and not extrapolated: the ripple, "
                                  "Ipk and Irms are lower bounds";
    }
    b.ripple_pp = volt_sec / b.inductance;
    b.ipk_basis = "Ipk = Iout + dI/2, dI = (Vin-Vout)*D/(fs*L) = " + fmt("%.4g A", b.ripple_pp) +
                  " with " + b.inductance_basis;
    b.irms = std::sqrt(iout * iout + b.ripple_pp * b.ripple_pp / 12.0);
    b.irms_basis = "Irms = sqrt(Iout^2 + dI^2/12) = " + fmt("%.4g A", b.irms);

    // ---- DCR loss: Irms^2 * DCR ----------------------------------------------------------
    if (present(m.dcr) && m.dcr > 0) {
        b.dcr.watts = b.irms * b.irms * m.dcr;
        b.dcr.basis = "Irms^2 x DCR " + fmt("%.4g mohm", m.dcr * 1e3) +
                      (m.dcr_is_maximum ? " (the record's stated maximum)"
                                        : " (the record's nominal / typical: it states no "
                                          "maximum)") +
                      ", at the record's measurement temperature (not rescaled); " +
                      b.irms_basis;
    } else {
        b.dcr.missing.push_back("DCR not stated in the record");
    }

    // ---- core / AC loss -------------------------------------------------------------------
    b.core.missing.push_back(
        "core / AC loss: the record states no loss at this operating point, and Kelvin computes "
        "no magnetics model (core loss belongs to MKF); not counted in the ranking (owner "
        "2026-11-14, interim)");

    // ---- saturation: Isat must exceed Ipk -------------------------------------------------
    {
        MagneticCheck& s = b.saturation;
        // The Ipk the gate compares against: the solved one, or (beyond the curve) the larger
        // of the nominal-L Ipk and the curve's end — both lower bounds of the true one.
        const double ipk_gate =
            read == Read::Beyond ? std::max(ipk_nominal, curve_end) : b.ipk;
        if (present(m.saturation_current) && m.saturation_current > 0) {
            const double isat = m.saturation_current;
            std::string isat_basis =
                "Isat " + fmt("%.4g A", isat) +
                (present(m.saturation_percent_drop)
                     ? fmt(" at %g %% inductance drop", m.saturation_percent_drop)
                     : std::string(" (drop criterion not stated)")) +
                (m.sat_table_from_curve ? " read off the record's L(I) curve (no Isat stated)"
                                        : std::string());
            if (isat <= ipk_gate) {
                s.status = MagneticCheck::Status::Fail;
                s.basis = isat_basis + " does not exceed Ipk " + fmt("%.4g A", ipk_gate) +
                          (read == Read::Beyond ? " (a lower bound)" : "");
            } else if (read == Read::Beyond) {
                s.status = MagneticCheck::Status::Unverified;
                s.missing.push_back(isat_basis + "; Ipk lies beyond the record's L(I) curve "
                                    "(measured to " + fmt("%.4g A", curve_end) +
                                    "), so it is not known to be below Isat");
            } else {
                s.status = MagneticCheck::Status::Pass;
                s.basis = isat_basis + " exceeds Ipk " + fmt("%.4g A", b.ipk);
            }
        } else if (read == Read::InRange) {
            s.status = MagneticCheck::Status::Pass;
            s.basis = "no Isat stated; the record's L(I) curve does not fall 10 % within its "
                      "range (to " + fmt("%.4g A", curve_end) + fmt("), and at Ipk %.4g A", b.ipk) +
                      fmt(" L is %.3g %% of L(0)", ratio_at * 100);
        } else if (read == Read::Beyond) {
            s.status = MagneticCheck::Status::Unverified;
            s.missing.push_back("no Isat stated, and Ipk lies beyond the record's L(I) curve "
                                "(measured to " + fmt("%.4g A", curve_end) + ")");
        } else if (read == Read::Unreadable) {
            s.status = MagneticCheck::Status::Unverified;
            s.missing.push_back("no Isat stated, and the record's L(I) curve is not readable (" +
                                unreadable_why + ")");
        } else {
            s.status = MagneticCheck::Status::Unverified;
            s.missing.push_back("no saturation current and no L(I) curve in the record");
        }
    }

    // ---- thermal: Irms against the rated (temperature-rise) current ------------------------
    {
        MagneticCheck& t = b.thermal;
        if (present(m.rated_current) && m.rated_current > 0) {
            const std::string rated =
                "rated current " + fmt("%.4g A", m.rated_current) +
                " (the largest the record states; its temperature-rise criterion and ambient "
                "are not carried by the index)";
            if (b.irms > m.rated_current) {
                t.status = MagneticCheck::Status::Fail;
                t.basis = fmt("Irms %.4g A exceeds the ", b.irms) + rated;
            } else {
                t.status = MagneticCheck::Status::Pass;
                t.basis = fmt("Irms %.4g A is within the ", b.irms) + rated;
            }
        } else {
            t.status = MagneticCheck::Status::Unverified;
            t.missing.push_back("no rated current stated in the record");
        }
    }
    if (op.ambient)
        b.notes.push_back(fmt("ambient %g degC: the L(I) curve, Isat, DCR and rated current are "
                              "read at the record's own condition, not rescaled to it",
                              *op.ambient));
    return b;
}

// The breakdown at the operating point's position.
inline MagneticLossBreakdown magnetic_loss(const MagneticRow& m,
                                           const InductorOperatingPoint& op) {
    switch (op.position) {
        case InductorPosition::BuckInductor: return buck_inductor_loss(m, op);
    }
    throw InvalidOptions("magnetic_loss: unhandled inductor position");
}

}  // namespace kelvin
