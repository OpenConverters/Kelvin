// Operating-point loss and checks of a power inductor from its datasheet values (owner
// 2026-11-14), the magnetic twin of MosfetLoss.hpp.
//
// The operating point states the converter position (buckInductor) and its conduction mode
// (ccm / dcm / crm, required). Per mode:
//   inductance gate  ccm  nominal >= the required L (L_min)
//                    dcm  the guaranteed maximum nominal x (1 + tol+) <= 0.9 x L_crit, with
//                         L_crit = Vout*(1 - Vout/Vin_min)/(2*Iout*fs) the CCM/DCM boundary at
//                         the minimum Vin and full load: a 10 % margin so the mode does not
//                         flip at the boundary (owner 2026-11-14); no tolerance stated ->
//                         UNVERIFIED (a nominal above 0.9 x L_crit fails regardless). The
//                         requested L is no floor in dcm / crm: its distance goes in a note.
//                    crm  the tolerance band [nominal x (1 - tol-), nominal x (1 + tol+)] inside
//                         the operating point's window; no tolerance -> UNVERIFIED (a nominal
//                         outside the window fails regardless)
//   L used           the tolerance's MINIMUM L (nominal x (1 - tol-)), else the nominal with a
//                    note; read at Ipk off the record's L(I) curve when it states one at the
//                    operating ambient, else at room temperature (20 / 25 degC) or unstated
//                    (L(Ipk) = L_used x L/L(0) at Ipk, never extrapolated; a curve with no 0 A
//                    point states no L(0) and is not read)
//   waveform (buck)  ccm  D = Vout/Vin, dI = (Vin-Vout) D / (fs L), Ipk = Iout + dI/2,
//                         Irms = sqrt(Iout^2 + dI^2/12)
//                    dcm  D = sqrt(2 L fs Iout Vout / (Vin (Vin-Vout))), Ipk = (Vin-Vout) D/(L fs),
//                         D2 = Ipk L fs / Vout, Irms = Ipk sqrt((D + D2)/3)
//                    crm  Ipk = dI = 2 Iout, Irms = 2 Iout / sqrt(3), fs = (Vin-Vout) D/(2 Iout L)
//   over-size flag   ccm  nominal > 2 x L_min (flag + note, like over-rating)
// Terms:
//   DCR      P = Irms^2 * DCR (the record's stated maximum, else its nominal; the basis says which)
//   core/AC  UNVERIFIED: magnetics math lives in MKF only, so Kelvin computes no core-loss model;
//            no catalogue record states a loss at an operating point. Interim owner rule
//            (2026-11-14): this one term does not push a candidate behind the others.
// Checks (hard gates in the selector):
//   saturation  the current at a 20 % inductance drop. An L(I) curve decides directly:
//               L at Ipk >= 0.8 x L(0). Otherwise the record's stated Isat points: interpolated
//               between the points that bracket 20 %, never extrapolated; points only above 20 %
//               are an upper bound (can fail, cannot pass), points only below 20 % a lower bound
//               (can pass, cannot fail); else UNVERIFIED. Temperature: the points at the
//               operating ambient, else at room temperature (20 / 25 degC) or unstated, else
//               UNVERIFIED (never read at another temperature); a criterion stated twice at
//               one temperature keeps the lower current.
//   thermal     the rated current at a 40 K rise, from the record's rated-current points per
//               rise (MagneticRow::rated_points): interpolated, never extrapolated; only
//               lower-rise points -> a lower bound (can pass, cannot fail); only higher-rise
//               points -> an upper bound (fails when already below Irms, else UNVERIFIED). An
//               unstated ambient is room temperature (noted).
// Every datasheet value is the record's, every operating-point value the caller's.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "Constraints.hpp"
#include "CrossRefCurves.hpp"
#include "CrossRefParams.hpp"  // is_room_temperature, same_room_temperature
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
    std::optional<double> frequency;  // Hz, crm only: set by L
    std::string duty_basis, inductance_basis, ipk_basis, irms_basis, frequency_basis;
    LossTerm dcr, core;
    // The conduction mode's inductance gate (owner 2026-11-14). A failed gate ends the
    // evaluation: the waveform, terms and the other checks are then not computed.
    MagneticCheck inductance_gate, saturation, thermal;
    // ccm: nominal above 2x the required L (owner 2026-11-14): a flag and a note, not a gate.
    bool over_sized = false;
    std::string over_sized_note;
    std::vector<std::string> notes;
    // Names of the UNVERIFIED terms and checks, in order.
    std::vector<std::string> unverified() const {
        std::vector<std::string> u;
        if (inductance_gate.unverified()) u.push_back("inductance");
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

// One number into a format, at any length (a fixed buffer cut long notes short).
inline std::string fmt(const char* f, double a) {
    const int n = std::snprintf(nullptr, 0, f, a);
    if (n < 0) throw std::logic_error(std::string("fmt: bad format ") + f);
    std::string b(static_cast<std::size_t>(n) + 1, '\0');
    std::snprintf(b.data(), b.size(), f, a);
    b.resize(static_cast<std::size_t>(n));
    return b;
}

// 20 degC and 25 degC count as the same room temperature (owner 2026-11-14): the shared rule.
inline bool room_temperature(double t) { return crossref::is_room_temperature(t); }
// A stated temperature at the operating ambient (equal, or both room temperature).
inline bool at_ambient(double t, double ambient) {
    return present(t) && crossref::same_room_temperature(t, ambient);
}

}  // namespace magnetic_loss_detail

// ---- the position's current waveform per conduction mode ---------------------------------
// One function per position; a boost inductor or a flyback magnetizing inductance adds its
// own branch here and in dcm_maximum_inductance, and the gates and checks below stay as they
// are.
struct InductorWaveform {
    double duty = 0, ipk = 0, irms = 0, ripple_pp = 0;
    std::optional<double> frequency;  // crm: set by L
    // dcm: the L given lies above the CCM/DCM boundary at this Vin (D + D2 > 1), so the
    // discontinuous waveform does not hold at it.
    bool continuous = false;
    std::string duty_basis, ipk_basis, irms_basis, frequency_basis;
};

inline InductorWaveform buck_waveform(const InductorOperatingPoint& op, double L) {
    using magnetic_loss_detail::fmt;
    InductorWaveform w;
    const double vin = op.vin, vout = op.vout, iout = op.iout;
    switch (op.mode) {
        case ConductionMode::Ccm: {
            const double fs = *op.fsw;
            w.duty = vout / vin;
            w.duty_basis = "D = Vout/Vin = " + fmt("%g V", vout) + " / " + fmt("%g V", vin) +
                           " (ideal CCM buck)";
            w.ripple_pp = (vin - vout) * w.duty / (fs * L);
            w.ipk = iout + w.ripple_pp / 2.0;
            w.ipk_basis = "CCM: Ipk = Iout + dI/2, dI = (Vin-Vout)*D/(fs*L) = " +
                          fmt("%.4g A", w.ripple_pp);
            w.irms = std::sqrt(iout * iout + w.ripple_pp * w.ripple_pp / 12.0);
            w.irms_basis = "CCM: Irms = sqrt(Iout^2 + dI^2/12) = " + fmt("%.4g A", w.irms);
            return w;
        }
        case ConductionMode::Dcm: {
            const double fs = *op.fsw;
            w.duty = std::sqrt(2.0 * L * fs * iout * vout / (vin * (vin - vout)));
            w.duty_basis = "DCM: D = sqrt(2*L*fs*Iout*Vout / (Vin*(Vin-Vout))) = " +
                           fmt("%.4g", w.duty);
            w.ipk = (vin - vout) * w.duty / (L * fs);
            w.ripple_pp = w.ipk;
            const double d2 = w.ipk * L * fs / vout;
            w.continuous = w.duty + d2 > 1.0 + 1e-9;  // L above the boundary at this Vin
            w.ipk_basis = "DCM: Ipk = (Vin-Vout)*D/(L*fs) = " + fmt("%.4g A", w.ipk) +
                          fmt(" (the current returns to 0 A; D2 = Ipk*L*fs/Vout = %.4g)", d2);
            w.irms = w.ipk * std::sqrt((w.duty + d2) / 3.0);
            w.irms_basis = "DCM: Irms = Ipk*sqrt((D+D2)/3) = " + fmt("%.4g A", w.irms);
            return w;
        }
        case ConductionMode::Crm: {
            w.duty = vout / vin;
            w.duty_basis = "D = Vout/Vin = " + fmt("%g V", vout) + " / " + fmt("%g V", vin) +
                           " (CrM buck)";
            w.ipk = 2.0 * iout;
            w.ripple_pp = w.ipk;
            w.ipk_basis = "CrM: Ipk = dI = 2*Iout = " + fmt("%.4g A", w.ipk) +
                          " (independent of L; L sets the frequency)";
            w.irms = w.ipk / std::sqrt(3.0);
            w.irms_basis = "CrM: Irms = 2*Iout/sqrt(3) = " + fmt("%.4g A", w.irms);
            w.frequency = (vin - vout) * w.duty / (w.ipk * L);
            w.frequency_basis = "CrM: fs = (Vin-Vout)*D/(2*Iout*L) = " +
                                fmt("%.4g kHz", *w.frequency / 1e3) + " at L at Ipk";
            return w;
        }
    }
    throw std::logic_error("buck_waveform: unhandled conduction mode");
}

inline InductorWaveform inductor_waveform(const InductorOperatingPoint& op, double L) {
    switch (op.position) {
        case InductorPosition::BuckInductor: return buck_waveform(op, L);
    }
    throw InvalidOptions("inductor_waveform: unhandled inductor position");
}

// The DCM limit: the largest L that keeps the converter discontinuous over the stated range.
// l_crit: the boundary itself; l_max = kDcmBoundaryMargin x l_crit, the gate (owner 2026-11-14).
inline constexpr double kDcmBoundaryMargin = 0.9;
struct DcmBoundary {
    double l_crit = 0, l_max = 0;
    std::string basis;
};

inline DcmBoundary dcm_maximum_inductance(const InductorOperatingPoint& op) {
    using magnetic_loss_detail::fmt;
    switch (op.position) {
        case InductorPosition::BuckInductor: {
            // The buck's boundary L = Vout*(1 - Vout/Vin)/(2*fs*Iout) grows with Vin, so the
            // smallest is at the minimum Vin, at full load.
            const double vmin = *op.vin_min, fs = *op.fsw;
            DcmBoundary d;
            d.l_crit = op.vout * (1.0 - op.vout / vmin) / (2.0 * fs * op.iout);
            d.l_max = kDcmBoundaryMargin * d.l_crit;
            d.basis = "L_max = 0.9 x L_crit = " + fmt("%.4g uH", d.l_max * 1e6) +
                      ", L_crit = Vout*(1 - Vout/Vin_min)/(2*Iout*fs) = " +
                      fmt("%.4g uH", d.l_crit * 1e6) + " (the buck's CCM/DCM boundary at Vin_min " +
                      fmt("%g V", vmin) + fmt(" and full load %g A", op.iout) +
                      "; the 10 % margin keeps the mode from flipping at the boundary: UT Austin "
                      "EE462L buck notes, arXiv 1607.01669, TI LMZ13608 datasheet)";
            return d;
        }
    }
    throw InvalidOptions("dcm_maximum_inductance: unhandled inductor position");
}

// ---- saturation: the current at a 20 % inductance drop from the stated Isat points --------
// ipk_lower_bound: the Ipk handed in is only a lower bound of the true one.
inline MagneticCheck saturation_from_points(const MagneticRow& m, double ipk, bool ipk_lower_bound,
                                            std::optional<double> ambient) {
    using magnetic_loss_detail::fmt;
    using magnetic_loss_detail::room_temperature;
    MagneticCheck s;
    struct P { double i, drop, t; };
    std::vector<P> all;
    for (std::size_t k = 0; k < m.sat_table_current.size(); ++k)
        all.push_back({m.sat_table_current[k],
                       k < m.sat_table_percent_drop.size() ? m.sat_table_percent_drop[k] : kNaN(),
                       k < m.sat_table_temperature.size() ? m.sat_table_temperature[k] : kNaN()});
    std::vector<P> crit;  // points that state their inductance-drop criterion
    for (const P& p : all)
        if (present(p.drop) && present(p.i) && p.i > 0) crit.push_back(p);
    if (crit.empty()) {
        if (all.empty())
            s.missing.push_back("no saturation current and no L(I) curve in the record");
        else
            s.missing.push_back(fmt("Isat %.4g A is stated without its inductance-drop "
                                    "criterion, so the current at a 20 %% drop is not known",
                                    all.front().i));
        return s;
    }
    // One temperature (owner 2026-11-14): the operating ambient when the record states points
    // there, else room temperature (20 / 25 degC) or unstated; never another temperature.
    auto at = [&](auto pred) {
        std::vector<P> v;
        for (const P& p : crit)
            if (pred(p.t)) v.push_back(p);
        return v;
    };
    std::vector<P> pts;
    std::string temp_basis;
    if (ambient && !room_temperature(*ambient)) {
        pts = at([&](double t) { return magnetic_loss_detail::at_ambient(t, *ambient); });
        if (!pts.empty()) temp_basis = fmt(" at %g degC (the operating ambient)", *ambient);
    }
    if (pts.empty()) {
        pts = at([&](double t) { return !present(t) || room_temperature(t); });
        if (!pts.empty()) {
            const bool stated = std::any_of(pts.begin(), pts.end(),
                                            [](const P& p) { return present(p.t); });
            temp_basis = stated ? " at room temperature" : " (temperature not stated)";
            if (ambient && !room_temperature(*ambient))
                temp_basis += fmt(", not at the operating ambient %g degC (not rescaled)",
                                  *ambient);
        }
    }
    if (pts.empty()) {
        std::string temps;
        std::vector<double> seen;
        for (const P& p : crit)
            if (std::find(seen.begin(), seen.end(), p.t) == seen.end()) {
                seen.push_back(p.t);
                temps += (temps.empty() ? "" : ", ") + fmt("%g degC", p.t);
            }
        s.missing.push_back("the record states Isat only at " + temps +
                            ": not at the operating ambient" +
                            (ambient ? fmt(" %g degC", *ambient) : std::string(" (not stated)")) +
                            " nor at room temperature, and it is not read at another temperature");
        return s;
    }
    // One current per criterion: a criterion stated twice at one temperature keeps the lower.
    std::sort(pts.begin(), pts.end(), [](const P& a, const P& b) {
        return std::make_pair(a.drop, a.i) < std::make_pair(b.drop, b.i);
    });
    std::string dup_note;
    {
        std::vector<P> u;
        for (const P& p : pts)
            if (u.empty() || u.back().drop != p.drop) u.push_back(p);
            else dup_note = fmt("; the record states two currents at %g %% drop, the lower is used",
                                p.drop);
        pts = std::move(u);
    }
    enum class Kind { Exact, Lower, Upper } kind;
    double i20 = 0;
    std::string i20_basis;
    const P* below = nullptr;
    const P* above = nullptr;
    for (const P& p : pts) {
        if (std::fabs(p.drop - 20.0) < 1e-9) { below = above = &p; break; }
        if (p.drop < 20.0) below = &p;  // ascending: the last one below is the nearest
        else if (!above) above = &p;
    }
    if (below && below == above) {
        kind = Kind::Exact;
        i20 = below->i;
        i20_basis = fmt("Isat %.4g A at 20 %% inductance drop", i20);
    } else if (below && above) {
        kind = Kind::Exact;
        i20 = below->i + (20.0 - below->drop) / (above->drop - below->drop) * (above->i - below->i);
        i20_basis = fmt("Isat at 20 %% drop %.4g A", i20) +
                    fmt(", interpolated (linear in drop) between %.4g A", below->i) +
                    fmt(" at %g %%", below->drop) + fmt(" and %.4g A", above->i) +
                    fmt(" at %g %%", above->drop);
    } else if (below) {
        kind = Kind::Lower;
        i20 = below->i;
        i20_basis = fmt("Isat %.4g A", i20) + fmt(" at %g %% drop", below->drop) +
                    " (the record states no point at or above 20 %: a lower bound of the "
                    "20 % current, not extrapolated)";
    } else {
        kind = Kind::Upper;
        i20 = above->i;
        i20_basis = fmt("Isat %.4g A", i20) + fmt(" at %g %% drop", above->drop) +
                    " (the record states no point at or below 20 %: an upper bound of the "
                    "20 % current, not extrapolated)";
    }
    i20_basis += temp_basis + dup_note;
    if (m.sat_table_from_curve) i20_basis += " (read off the record's L(I) curve; no Isat stated)";
    const std::string ipk_s = fmt("Ipk %.4g A", ipk) + (ipk_lower_bound ? " (a lower bound)" : "");
    const bool can_fail = kind != Kind::Lower;
    const bool can_pass = kind != Kind::Upper && !ipk_lower_bound;
    if (can_fail && i20 <= ipk) {
        s.status = MagneticCheck::Status::Fail;
        s.basis = i20_basis + " does not exceed " + ipk_s;
    } else if (can_pass && i20 > ipk) {
        s.status = MagneticCheck::Status::Pass;
        s.basis = i20_basis + " exceeds " + ipk_s;
    } else {
        s.missing.push_back(i20_basis + " against " + ipk_s +
                            ": the 20 % current is not known to exceed Ipk");
    }
    return s;
}

// ---- thermal: the rated current at a 40 K rise -----------------------------------------------
// rated_points: x = temperature rise (K), y = rated current (A), temperature = the ambient the
// ratings are stated at (NaN = not stated). why_empty: what to report when there are none.
inline MagneticCheck thermal_check(const CurvePts& rated_points, double irms,
                                   std::optional<double> ambient, const std::string& why_empty) {
    using magnetic_loss_detail::fmt;
    using magnetic_loss_detail::room_temperature;
    MagneticCheck t;
    if (rated_points.empty() || !rated_points.y || rated_points.y->size() != rated_points.x->size()) {
        t.missing.push_back(why_empty);
        return t;
    }
    struct P { double rise, i; };
    std::vector<P> pts;
    for (std::size_t k = 0; k < rated_points.x->size(); ++k)
        pts.push_back({(*rated_points.x)[k], (*rated_points.y)[k]});
    std::sort(pts.begin(), pts.end(), [](const P& a, const P& b) { return a.rise < b.rise; });
    std::string amb;
    if (present(rated_points.temperature))
        amb = fmt(" at %g degC ambient", rated_points.temperature);
    else
        amb = " (ambient not stated: read as room temperature)";
    bool same_ambient = true;
    if (ambient) {
        const double rt = rated_points.temperature;
        same_ambient = present(rt) ? magnetic_loss_detail::at_ambient(rt, *ambient)
                                   : room_temperature(*ambient);
    }
    if (!same_ambient)
        amb += fmt(", not at the operating ambient %g degC (not rescaled)", *ambient);
    const P* below = nullptr;
    const P* above = nullptr;
    for (const P& p : pts) {
        if (std::fabs(p.rise - 40.0) < 1e-9) { below = above = &p; break; }
        if (p.rise < 40.0) below = &p;
        else if (!above) above = &p;
    }
    double i40 = 0;
    bool lower_bound = false;
    // Only higher-rise points: the current at 40 K is at most the lowest-rise one stated (an
    // upper bound, owner 2026-11-14): it fails a part already below Irms, never passes one.
    std::string basis;
    if (below && below == above) {
        i40 = below->i;
        basis = fmt("rated current %.4g A at a 40 K rise", i40);
    } else if (below && above) {
        i40 = below->i + (40.0 - below->rise) / (above->rise - below->rise) * (above->i - below->i);
        basis = fmt("rated current at a 40 K rise %.4g A", i40) +
                fmt(", interpolated (linear in rise) between %.4g A", below->i) +
                fmt(" at %g K", below->rise) + fmt(" and %.4g A", above->i) +
                fmt(" at %g K", above->rise);
    } else if (below) {
        i40 = below->i;
        lower_bound = true;
        basis = fmt("rated current %.4g A", i40) + fmt(" at a %g K rise", below->rise) +
                " (no 40 K point: a lower bound of the 40 K current, not extrapolated)";
    } else {
        const std::string ub = fmt("rated current %.4g A", above->i) +
                               fmt(" at a %g K rise", above->rise) + amb +
                               " (the record rates its current only above a 40 K rise: an upper "
                               "bound of the 40 K current, not extrapolated)";
        if (above->i < irms) {
            t.status = MagneticCheck::Status::Fail;
            t.basis = fmt("Irms %.4g A exceeds the ", irms) + ub;
        } else {
            t.missing.push_back(fmt("Irms %.4g A is within the ", irms) + ub +
                                ": the 40 K current is not known to cover it");
        }
        return t;
    }
    basis += amb;
    if (irms <= i40) {
        t.status = MagneticCheck::Status::Pass;
        t.basis = fmt("Irms %.4g A is within the ", irms) + basis;
    } else if (lower_bound) {
        t.missing.push_back(fmt("Irms %.4g A exceeds the ", irms) + basis +
                            ": the 40 K current is not known");
    } else {
        t.status = MagneticCheck::Status::Fail;
        t.basis = fmt("Irms %.4g A exceeds the ", irms) + basis;
    }
    return t;
}

// The record's rated currents per temperature rise (MagneticRow::rated_points, Views.cpp
// magnetic_rated_points). Empty: the record states no rated current, or none readable as one
// rating per rise under one ambient -> no thermal verdict.
inline MagneticCheck thermal_check_of(const MagneticRow& m, double irms,
                                      std::optional<double> ambient) {
    using magnetic_loss_detail::fmt;
    const std::string why =
        present(m.rated_current)
            ? fmt("the rated current %.4g A", m.rated_current) +
                  " (the largest the record states) is not stated with its temperature rise "
                  "(or its rated-current points are not readable as one rating per rise), so "
                  "the current at a 40 K rise is not known"
            : std::string("no rated current stated in the record");
    return thermal_check(m.rated_points, irms, ambient, why);
}

// ---- the L(I) curve the operating point reads (owner 2026-11-14) ----------------------------
// MagneticRow::l_curves holds every stated L(I) curve (henries vs amperes, one per temperature).
// The one read: at the operating ambient, else at room temperature (20 / 25 degC) or unstated;
// one such curve. Its L(0) is its 0 A point; a curve with no 0 A point is read with the record's
// nominal L as L(0) (datasheets define the Isat drop from the nominal), with a note saying so,
// and is not read when the record states no nominal. With no ambient given, two room-temperature
// curves (stated 20 / 25 degC) are both read and the one with the lower L at Ipk is used (the
// breakdown solves each; owner 2026-11-14). Anything else is not read, and `why` says so: two
// candidate curves otherwise (not chosen between), only other temperatures, a curve with no
// L(0).
struct LCurve {
    std::optional<crossref::Curve> ratio;  // current (A) -> L / L(0); always set once picked
    double temperature;     // as stated (NaN: unstated)
    std::string temp_basis;  // its temperature, plus where its L(0) comes from when not stated
    std::string l0_note;     // non-empty: L(0) is the record's nominal (no 0 A point)
};
struct LCurvePick {
    std::vector<LCurve> curves;  // one, or the room-temperature pair (lowest_at_ipk)
    bool lowest_at_ipk = false;  // read every curve, use the one with the lower L at Ipk
    bool stated = false;         // the record states some L(I) curve
    std::string why;             // not read: why
};
inline LCurvePick pick_l_curve(const MagneticRow& m, std::optional<double> ambient) {
    using magnetic_loss_detail::fmt;
    using magnetic_loss_detail::room_temperature;
    LCurvePick k;
    auto temp_s = [](double t) {
        return present(t) ? fmt("%g degC", t) : std::string("no stated temperature");
    };
    if (m.l_curves.empty()) {
        // The extractor keeps the picked curve as it is stated (bias_curve_*); l_curves drops a
        // curve stating two inductances at one current (Views.cpp store_curve).
        if (m.bias_curve_current.size() >= 2) {
            k.stated = true;
            k.why = "its L(I) curve at " + temp_s(m.bias_curve_temperature) +
                    " states two inductances at one current, so it cannot be read at a point";
        }
        return k;
    }
    k.stated = true;
    // The curve the extractor picked (bias_curve_*, nearest 25 degC) but l_curves dropped: it
    // states two inductances at one current. It still counts where its temperature qualifies.
    const double bt = m.bias_curve_temperature;
    const bool dropped =
        m.bias_curve_current.size() >= 2 &&
        std::none_of(m.l_curves.begin(), m.l_curves.end(), [&](const CurvePts& c) {
            return present(bt) ? (present(c.temperature) && c.temperature == bt)
                               : !present(c.temperature);
        });
    const std::string dropped_why =
        "its L(I) curve at " + temp_s(bt) +
        " states two inductances at one current, so it cannot be read at a point";
    std::vector<const CurvePts*> pick;
    if (ambient && !room_temperature(*ambient)) {
        if (dropped && magnetic_loss_detail::at_ambient(bt, *ambient)) {
            k.why = dropped_why;
            return k;
        }
        for (const CurvePts& c : m.l_curves)
            if (magnetic_loss_detail::at_ambient(c.temperature, *ambient)) pick.push_back(&c);
    }
    const bool at_amb = !pick.empty();
    if (!at_amb) {
        if (dropped && (!present(bt) || room_temperature(bt))) {
            k.why = dropped_why;
            return k;
        }
        for (const CurvePts& c : m.l_curves)
            if (!present(c.temperature) || room_temperature(c.temperature)) pick.push_back(&c);
    }
    std::string all;
    for (const CurvePts& c : m.l_curves) all += (all.empty() ? "" : ", ") + temp_s(c.temperature);
    if (pick.empty()) {
        k.why = "its L(I) curves are stated only at " + all + ": not at the operating ambient" +
                (ambient ? fmt(" %g degC", *ambient) : std::string(" (not stated)")) +
                " nor at room temperature, and a curve is not read at another temperature";
        return k;
    }
    std::string qualify;
    for (const CurvePts* c : pick) qualify += (qualify.empty() ? "" : ", ") + temp_s(c->temperature);
    const std::string n_qualify =
        "it states " + std::to_string(pick.size()) + " L(I) curves that qualify (" + qualify + ")";
    if (pick.size() > 1) {
        // Owner 2026-11-14: with no ambient given, room-temperature curves stated at 20 and
        // 25 degC are all read; the breakdown uses the one with the lower L at Ipk. An
        // unstated temperature among them, or a given ambient, is not that case.
        const bool room_pair =
            !ambient && std::all_of(pick.begin(), pick.end(), [](const CurvePts* c) {
                return present(c->temperature);
            });
        if (!room_pair) {
            k.why = n_qualify + ", and which one applies is not stated";
            return k;
        }
        k.lowest_at_ipk = true;
    }
    // Each picked curve as L / L(0) against current.
    for (const CurvePts* cp : pick) {
        const CurvePts& c = *cp;
        LCurve lc{std::nullopt, c.temperature, {}, {}};
        double l0 = 0;
        std::vector<double> x, y;
        if (c.x->front() == 0.0f && c.y->front() > 0.0f) {
            l0 = c.y->front();
        } else if (c.x->front() > 0.0f && c.y->front() > 0.0f) {
            // No 0 A point (owner 2026-11-14): the record's nominal L is L(0) — datasheets
            // define the Isat drop from the nominal. No nominal: L(0) is not known.
            if (!present(m.inductance) || !(m.inductance > 0)) {
                k.why = "its L(I) curve at " + temp_s(c.temperature) +
                        fmt(" starts at %.4g A, not at 0 A, and the record states no nominal L, ",
                            c.x->front()) +
                        "so L(0) is not known and L/L(0) is not either";
                k.curves.clear();
                return k;
            }
            l0 = m.inductance;
            x.push_back(0.0);
            y.push_back(1.0);
            lc.l0_note = "the L(I) curve at " + temp_s(c.temperature) +
                         fmt(" starts at %.4g A, not at 0 A: L(0) is the record's nominal ",
                             c.x->front()) +
                         fmt("%.4g uH", l0 * 1e6) +
                         " (datasheets define the Isat drop from the nominal; owner 2026-11-14)";
        } else {
            k.why = "its L(I) curve at " + temp_s(c.temperature) +
                    fmt(" starts at %.4g A", c.x->front()) +
                    fmt(" with L %.4g uH", c.y->front() * 1e6) +
                    ", so L(0) is not known and L/L(0) is not either";
            k.curves.clear();
            return k;
        }
        for (std::size_t i = 0; i < c.x->size(); ++i) {
            x.push_back((*c.x)[i]);
            y.push_back((*c.y)[i] / l0);
        }
        try {
            lc.ratio.emplace(std::move(x), std::move(y));
        } catch (const std::invalid_argument& e) {
            k.why = "its L(I) curve at " + temp_s(c.temperature) + " is not readable: " + e.what();
            k.curves.clear();
            return k;
        }
        if (at_amb)
            lc.temp_basis = fmt(" at %g degC (the operating ambient)", c.temperature);
        else
            lc.temp_basis =
                (present(c.temperature) ? fmt(" at %g degC (room temperature)", c.temperature)
                                        : std::string(" (temperature not stated: read as room "
                                                      "temperature)")) +
                (ambient && !room_temperature(*ambient)
                     ? fmt(", not at the operating ambient %g degC (not rescaled)", *ambient)
                     : std::string());
        if (!lc.l0_note.empty())
            lc.temp_basis += fmt(", L(0) = the record's nominal %.4g uH (the curve has no 0 A point)",
                                 l0 * 1e6);
        k.curves.push_back(std::move(lc));
    }
    if (k.lowest_at_ipk) k.why = n_qualify;  // the breakdown completes it when it cannot choose
    return k;
}

// ---- the breakdown ---------------------------------------------------------------------------
// l_required: the required (minimum) inductance; the gate in ccm. In dcm / crm it is no floor
// (owner 2026-11-14): its distance from the nominal goes in a note.
inline MagneticLossBreakdown inductor_loss(const MagneticRow& m, const InductorOperatingPoint& op,
                                           std::optional<double> l_required) {
    using magnetic_loss_detail::fmt;
    if (!present(m.inductance) || !(m.inductance > 0))
        throw std::invalid_argument("inductor_loss: the record states no inductance");
    MagneticLossBreakdown b;
    const double L0 = m.inductance;
    const bool has_tol = present(m.inductance_tol_minus_pct) && present(m.inductance_tol_plus_pct);
    const double l_lo = has_tol ? L0 * (1.0 - m.inductance_tol_minus_pct / 100.0) : L0;
    const double l_hi = has_tol ? L0 * (1.0 + m.inductance_tol_plus_pct / 100.0) : L0;
    const std::string nominal_s = "nominal " + fmt("%.4g uH", L0 * 1e6);
    const std::string band_s =
        has_tol ? fmt("%.4g", l_lo * 1e6) + fmt("-%.4g uH", l_hi * 1e6) +
                      fmt(" (-%.3g %%", m.inductance_tol_minus_pct) +
                      fmt(" / +%.3g %%)", m.inductance_tol_plus_pct)
                : std::string();

    // ---- the mode's inductance gate ------------------------------------------------------
    MagneticCheck& g = b.inductance_gate;
    switch (op.mode) {
        case ConductionMode::Ccm: {
            if (!l_required || !(*l_required > 0))
                throw InvalidOptions("conductionMode ccm requires the required inductance "
                                     "(designRequirements.magnetizingInductance)");
            if (L0 >= *l_required) {
                g.status = MagneticCheck::Status::Pass;
                g.basis = "CCM: " + nominal_s + " >= the required " +
                          fmt("%.4g uH", *l_required * 1e6);
            } else {
                g.status = MagneticCheck::Status::Fail;
                g.basis = "CCM: " + nominal_s + " is below the required " +
                          fmt("%.4g uH", *l_required * 1e6);
            }
            if (L0 > 2.0 * *l_required) {
                b.over_sized = true;
                // The ripple at the nominal against the ripple at the required L: dI ~ 1/L.
                b.over_sized_note =
                    "over-sized: " + nominal_s + fmt(" is %.3gx", L0 / *l_required) +
                    " the required " + fmt("%.4g uH", *l_required * 1e6) +
                    " (above 2x: the ripple is under half the design's) — a bigger body and "
                    "higher cost are likely; flagged only: the operating-point loss alone sets "
                    "the order";
            }
            break;
        }
        case ConductionMode::Dcm: {
            const DcmBoundary d = dcm_maximum_inductance(op);
            if (L0 > d.l_max) {
                g.status = MagneticCheck::Status::Fail;
                g.basis = "DCM: " + nominal_s + " is above " + d.basis;
            } else if (!has_tol) {
                g.missing.push_back("DCM: the record states no inductance tolerance, so its "
                                    "guaranteed maximum is not known (" + nominal_s +
                                    " is within " + d.basis + ")");
            } else if (l_hi <= d.l_max) {
                g.status = MagneticCheck::Status::Pass;
                g.basis = "DCM: the guaranteed maximum " + fmt("%.4g uH", l_hi * 1e6) +
                          fmt(" (nominal x (1 + %.3g %%))", m.inductance_tol_plus_pct) +
                          " is within " + d.basis;
            } else {
                g.status = MagneticCheck::Status::Fail;
                g.basis = "DCM: the guaranteed maximum " + fmt("%.4g uH", l_hi * 1e6) +
                          fmt(" (nominal x (1 + %.3g %%))", m.inductance_tol_plus_pct) +
                          " is above " + d.basis;
            }
            break;
        }
        case ConductionMode::Crm: {
            const double wlo = *op.window_min_l, whi = *op.window_max_l;
            const std::string win = "the CrM window " + fmt("%.4g", wlo * 1e6) +
                                    fmt("-%.4g uH", whi * 1e6);
            if (L0 < wlo || L0 > whi) {
                g.status = MagneticCheck::Status::Fail;
                g.basis = "CrM: " + nominal_s + " lies outside " + win;
            } else if (!has_tol) {
                g.missing.push_back("CrM: the record states no inductance tolerance, so its band "
                                    "is not known to lie inside " + win + " (" + nominal_s +
                                    " does)");
            } else if (l_lo >= wlo && l_hi <= whi) {
                g.status = MagneticCheck::Status::Pass;
                g.basis = "CrM: the tolerance band " + band_s + " lies inside " + win;
            } else {
                g.status = MagneticCheck::Status::Fail;
                g.basis = "CrM: the tolerance band " + band_s + " does not lie inside " + win;
            }
            break;
        }
    }
    if (op.mode != ConductionMode::Ccm && l_required && *l_required > 0)
        b.notes.push_back(std::string(op.mode == ConductionMode::Dcm ? "DCM" : "CrM") + ": " +
                          nominal_s + fmt(" is %+.3g %%", (L0 / *l_required - 1.0) * 100) +
                          " from the requested " + fmt("%.4g uH", *l_required * 1e6) +
                          " (not a gate in this mode: the operating point's own limits are)");
    if (g.failed()) {
        const std::string why = "not evaluated: the inductance gate failed";
        b.dcr.missing.push_back(why);
        b.core.missing.push_back(why);
        b.saturation.missing.push_back(why);
        b.thermal.missing.push_back(why);
        return b;
    }

    // ---- the L the waveform uses: the tolerance's minimum ------------------------------------
    const double l_base = l_lo;
    const std::string base_s =
        has_tol ? "the tolerance's minimum L " + fmt("%.4g uH", l_lo * 1e6) + " (" + nominal_s +
                      fmt(" - %.3g %%)", m.inductance_tol_minus_pct)
                : "the " + nominal_s + " (the record states no tolerance)";
    if (!has_tol)
        b.notes.push_back("the record states no inductance tolerance: the ripple and Ipk use the "
                          "nominal L");

    // ---- L at Ipk off the record's L(I) curve ------------------------------------------------
    // The row keeps the curve as L/L(0) against current; L(Ipk) = l_base * ratio(Ipk). Ipk
    // depends on L(Ipk), so solve h(I) = Ipk(l_base * ratio(I)) - I = 0 by bisection on the
    // curve's range: h(start) > 0 always; h(end) > 0 means the operating point lies beyond
    // the measured curve (not extrapolated).
    enum class Read { NoCurve, Unreadable, InRange, Beyond };
    struct CurveRead {
        Read read = Read::NoCurve;
        double curve_end = 0, ratio_at = 1;
        std::string unreadable_why;
    };
    const InductorWaveform w_base = inductor_waveform(op, l_base);
    // Ipk and the L it is read at, solved together on one curve.
    auto solve = [&](const crossref::Curve& c) {
        CurveRead r;
        r.curve_end = c.x.back();
        auto h = [&](double i) -> std::optional<double> {
            const auto q = crossref::curve_at(c, i, crossref::Interp::Linear);
            if (!q || !(*q > 0)) return std::nullopt;
            return inductor_waveform(op, l_base * *q).ipk - i;
        };
        const auto h_start = h(c.x.front());
        const auto h_end = h(r.curve_end);
        if (!h_start || !(*h_start > 0)) {
            r.read = Read::Unreadable;
            r.unreadable_why = "the curve does not start below Ipk";
        } else if (!h_end || *h_end > 0) {
            r.read = Read::Beyond;
        } else {
            double lo = c.x.front(), hi = r.curve_end;
            r.read = Read::InRange;
            for (int k = 0; k < 200 && hi - lo > 1e-12 * r.curve_end; ++k) {
                const double mid = 0.5 * (lo + hi);
                const auto hm = h(mid);
                if (!hm) { r.read = Read::Beyond; break; }  // a non-positive sample: no reading
                (*hm > 0 ? lo : hi) = mid;
            }
            if (r.read == Read::InRange)
                r.ratio_at = *crossref::curve_at(c, hi, crossref::Interp::Linear);
        }
        return r;
    };
    LCurvePick pick = pick_l_curve(m, op.ambient);
    CurveRead cr;
    const LCurve* used = nullptr;  // the curve read
    std::string pair_basis;        // the room-temperature pair: which one is used, and why
    if (pick.curves.empty()) {
        if (pick.stated) {
            cr.read = Read::Unreadable;
            cr.unreadable_why = pick.why;
        }
    } else if (!pick.lowest_at_ipk) {
        used = &pick.curves.front();
        cr = solve(*used->ratio);
    } else {
        // Two room-temperature curves, no ambient (owner 2026-11-14): read both, use the one
        // with the lower L at Ipk. Each must read L at Ipk inside its measured range, or the
        // lower one is not known.
        std::vector<CurveRead> reads;
        std::string not_read, each;
        for (const LCurve& lc : pick.curves) {
            reads.push_back(solve(*lc.ratio));
            const CurveRead& r = reads.back();
            const std::string t = fmt("%g degC", lc.temperature);
            if (r.read == Read::InRange)
                each += (each.empty() ? "" : ", ") + t +
                        fmt(": %.4g uH", l_base * r.ratio_at * 1e6);
            else if (not_read.empty())
                not_read = r.read == Read::Beyond
                               ? "Ipk lies beyond its " + t + " curve" +
                                     fmt(" (measured to %.4g A)", r.curve_end)
                               : "its " + t + " curve is not read (" + r.unreadable_why + ")";
        }
        if (!not_read.empty()) {
            cr.read = Read::Unreadable;
            cr.unreadable_why = pick.why + " and no ambient is given; " + not_read +
                                ", so the one with the lower L at Ipk is not known";
        } else {
            std::size_t best = 0;
            for (std::size_t i = 1; i < reads.size(); ++i)
                if (reads[i].ratio_at < reads[best].ratio_at) best = i;
            used = &pick.curves[best];
            cr = reads[best];
            pair_basis = "; " + pick.why + " and no ambient is given: all read (L at Ipk " + each +
                         "), the lower used (owner 2026-11-14)";
        }
    }
    const Read read = cr.read;
    const double curve_end = cr.curve_end, ratio_at = cr.ratio_at;
    const std::string& unreadable_why = cr.unreadable_why;
    const std::string curve_basis = used ? used->temp_basis : std::string();
    if (used && !used->l0_note.empty() && (read == Read::InRange || read == Read::Beyond))
        b.notes.push_back(used->l0_note);
    InductorWaveform w = read == Read::InRange ? inductor_waveform(op, l_base * ratio_at) : w_base;
    if (read == Read::InRange) {
        b.inductance = l_base * ratio_at;
        b.inductance_basis = "L " + fmt("%.4g uH", b.inductance * 1e6) +
                             fmt(" at Ipk %.4g A: ", w.ipk) + base_s +
                             fmt(" x L/L(0) %.4g", ratio_at) +
                             " read off the record's L(I) curve" + curve_basis +
                             " (linear; Ipk and the L it is read at solved together)" + pair_basis;
    } else {
        b.inductance = l_base;
        b.inductance_basis = base_s;
        if (read == Read::NoCurve)
            b.inductance_basis += " (the record states no L(I) curve: any roll-off below Isat "
                                  "is not in the waveform)";
        else if (read == Read::Unreadable)
            b.inductance_basis += " (the record's L(I) curve is not read: " + unreadable_why +
                                  "; any roll-off below Isat is not in the waveform)";
        else
            b.inductance_basis += "; Ipk lies beyond the record's L(I) curve" + curve_basis +
                                  " (measured to " +
                                  fmt("%.4g A", curve_end) +
                                  "), so L at Ipk is not read and not extrapolated: the ripple, "
                                  "Ipk and Irms are lower bounds";
    }
    if (w.continuous) {
        // Only an L(I) curve reading above L(0) can get here (the gate holds the nominal at or
        // below the boundary at the minimum Vin).
        g.status = MagneticCheck::Status::Fail;
        g.basis = "DCM: L at Ipk " + fmt("%.4g uH", b.inductance * 1e6) + " (" +
                  b.inductance_basis + ") lies above the CCM/DCM boundary at Vin " +
                  fmt("%g V", op.vin) + ": the converter is not discontinuous with it";
        const std::string why = "not evaluated: the inductance gate failed";
        b.dcr.missing.push_back(why);
        b.core.missing.push_back(why);
        b.saturation.missing.push_back(why);
        b.thermal.missing.push_back(why);
        return b;
    }
    b.duty = w.duty;
    b.duty_basis = w.duty_basis;
    b.ripple_pp = w.ripple_pp;
    b.ipk = w.ipk;
    b.irms = w.irms;
    b.ipk_basis = w.ipk_basis + ", with " + b.inductance_basis;
    b.irms_basis = w.irms_basis;
    b.frequency = w.frequency;
    b.frequency_basis = w.frequency_basis;

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

    // ---- saturation: the current at a 20 % drop must exceed Ipk ----------------------------
    {
        MagneticCheck& s = b.saturation;
        if (read == Read::InRange) {
            // The curve decides directly: L at Ipk >= 0.8 x L(0).
            const std::string r = "the record's L(I) curve" + curve_basis +
                                  fmt(": at Ipk %.4g A", b.ipk) +
                                  fmt(" L is %.3g %% of L(0)", ratio_at * 100);
            if (ratio_at >= 0.8) {
                s.status = MagneticCheck::Status::Pass;
                s.basis = r + " (at least 80 %: within the 20 % drop criterion)";
            } else {
                s.status = MagneticCheck::Status::Fail;
                s.basis = r + " (below 80 %: beyond the 20 % drop criterion)";
            }
        } else {
            // Beyond the curve: the Ipk the gate compares against is a lower bound (the larger
            // of the no-roll-off Ipk and the curve's end). A curve that already falls below
            // 80 % within its range fails it.
            bool decided = false;
            if (read == Read::Beyond) {
                const crossref::Curve& c = *used->ratio;
                for (std::size_t k = 0; k < c.x.size(); ++k)
                    if (c.y[k] < 0.8) {
                        s.status = MagneticCheck::Status::Fail;
                        s.basis = "the record's L(I) curve" + curve_basis +
                                  fmt(" falls to %.3g %% of L(0)", c.y[k] * 100) +
                                  fmt(" at %.4g A, and Ipk lies beyond the curve", c.x[k]) +
                                  fmt(" (measured to %.4g A)", curve_end);
                        decided = true;
                        break;
                    }
            }
            if (!decided) {
                const bool lb = read == Read::Beyond;
                const double ipk_gate = lb ? std::max(w_base.ipk, curve_end) : b.ipk;
                s = saturation_from_points(m, ipk_gate, lb, op.ambient);
                if (read == Read::Unreadable)
                    for (auto& x : s.missing)
                        x += " (the record's L(I) curve is not readable: " + unreadable_why + ")";
            }
        }
    }

    // ---- thermal: Irms against the rated current at a 40 K rise ------------------------------
    b.thermal = thermal_check_of(m, b.irms, op.ambient);
    if (op.ambient)
        b.notes.push_back(fmt("ambient %g degC: the DCR is read at the record's own condition, not "
                              "rescaled to it (the L(I) curve, Isat and rated current state their "
                              "temperature in their basis)",
                              *op.ambient));
    return b;
}

// The breakdown at the operating point's position and mode.
inline MagneticLossBreakdown magnetic_loss(const MagneticRow& m, const InductorOperatingPoint& op,
                                           std::optional<double> l_required) {
    switch (op.position) {
        case InductorPosition::BuckInductor: return inductor_loss(m, op, l_required);
    }
    throw InvalidOptions("magnetic_loss: unhandled inductor position");
}

}  // namespace kelvin
