// Cross-reference ranker — the deterministic "score substitutes for an original
// part" verb. Program-only and LLM-free: Kirchhoff and the Kelvin web frontend
// consume the ranked list directly; Heaviside runs its LLM chooser over the same
// list.
//
// Given an ORIGINAL part's spec block and a list of candidate spec blocks (both
// category-appropriate JSON, SI base units), returns a ranked, gated, honest
// result: each candidate carries a status (recommended/partial/no_substitute),
// per-parameter verdicts, a total penalty, and an original_verified flag.
//
// The scoring model is Heaviside's, reused rather than re-derived:
//   * primary value          CrossRefScore.hpp   (score_primary_value)
//   * per-parameter verdicts CrossRefParams.hpp  (params_for/compare_param — the
//                            PARAM_SPECS table, 9 categories, incl. class/exact
//                            gates and %-drop-normalized saturation current)
//   * physical fit           CrossRefDimensions.hpp (case-code resolution,
//                            3-axis orientation-agnostic footprint, mount type)
//   * MPN-derived gates      CrossRefDecode.hpp  (AEC-Q grade, rated voltage,
//                            MLCC DC-bias effective capacitance)
// Critical ratings the PARAM_SPECS table deliberately leaves to Heaviside's
// stress guardrails (Vds, Vrrm, rated voltage, Id, If) are gated here instead,
// since this ranker has no guardrail stage behind it.
//
// The honesty rule (the FAE finding): when the ORIGINAL's specs are unverified,
// a candidate can never be a clean 'recommended' — it is capped at 'partial' and
// flagged original_unverified. A candidate whose primary value is out of range,
// whose critical rating falls far below a KNOWN original, whose identity
// parameters differ, or whose mount type is incompatible, is rejected outright.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "CrossRefClasses.hpp"
#include "CrossRefCurveChecks.hpp"
#include "CrossRefCurves.hpp"
#include "CrossRefDecode.hpp"
#include "CrossRefDimensions.hpp"
#include "CrossRefMagneticCurves.hpp"  // magnetic curves (owner 2026-11-14)
#include "CrossRefParams.hpp"
#include "CrossRefScore.hpp"

namespace kelvin::crossref {

using json = nlohmann::json;

inline std::optional<double> num(const json& o, const char* k) {
    auto it = o.find(k);
    if (it == o.end() || it->is_null()) return std::nullopt;
    if (it->is_number()) return it->get<double>();
    return std::nullopt;
}

inline bool flag(const json& o, const char* k) {
    auto it = o.find(k);
    return it != o.end() && it->is_boolean() && it->get<bool>();
}
inline std::string str(const json& o, const char* k) {
    auto it = o.find(k);
    if (it == o.end() || it->is_null() || !it->is_string()) return "";
    return it->get<std::string>();
}

// A percentage as an engineer writes it in a note: two decimals at most, no
// trailing zeros ("0.93 %", "5 %"). The other numeric notes here round to whole
// units, which would print a 0.25 % tolerance as "0 %".
inline std::string plain_pct(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s + " %";
}
// The same, sign always shown — a shift has a direction and it matters.
inline std::string signed_pct(double v) { return (v >= 0 ? "+" : "") + plain_pct(v); }

//: Transformer turns ratio (owner 2026-11-13, R2): equal within print rounding passes, up to
//: 15 % off warns ("turns ratio can be +-15%, but with care"), beyond that fails.
inline constexpr double kTurnsRatioPassRel = 0.01;
inline constexpr double kTurnsRatioWarnRel = 0.15;
// A cable core whose opening is more than this many times the original's fits the original's
// cable loosely (owner 2026-11-14): a PASS with a note.
inline constexpr double kCableLooseFitRatio = 1.5;
// A capacitor's ESL curve is graded from this multiple of the original's self-resonance up
// (owner 2026-11-14): X/omega = L(1 - 1/k^2), within about 11 % of L from k = 3.
inline constexpr double kEslBandOfResonance = 3.0;

// A resistance as an engineer writes it: SI prefix, up to six significant digits,
// no trailing zeros ("14.7 kΩ", "15 kΩ", "0.5 Ω").
inline std::string ohms(double v) {
    static const struct { double scale; const char* unit; } k[] = {
        {1e9, " GΩ"}, {1e6, " MΩ"}, {1e3, " kΩ"}, {1.0, " Ω"}, {1e-3, " mΩ"}, {1e-6, " µΩ"}};
    const double a = std::fabs(v);
    for (const auto& p : k)
        if (a >= p.scale || p.scale == 1e-6) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.6g", v / p.scale);
            return std::string(buf) + p.unit;
        }
    throw std::logic_error("ohms: unreachable");
}

// Representation epsilon for "the same nominal resistance": two records stating
// 14.7 kΩ as 14700 and 1.47e4 must compare equal, and nothing else may.
constexpr double kResistanceIdentityRelEps = 1e-9;

// A length as a mechanical drawing states it: metres in, millimetres out, two
// decimals at most, no trailing zeros ("6.36", "4"). Bare, so a note can write
// the unit once for a pair ("10.4 x 10.3 mm").
inline std::string mm(double metres) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", metres * 1000.0);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

// A temperature as a datasheet states it, sign always shown ("+125", "-55"): the
// sign is half the meaning of an operating limit. Bare, so a note can write the
// unit once for a pair ("+105 vs +125 degC").
inline std::string degc(double c) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", c);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return (c > 0 ? "+" : "") + s;
}

// A voltage as a datasheet states it ("3.56", "15.75"): three decimals at most, no
// trailing zeros. Bare, so a note can write the unit once for a band.
inline std::string volts(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

// The breakdown window a diode record GUARANTEES, written as a zener datasheet
// writes it: "3.56-3.64 V (a 1.11 % grade)". Both halves earn their place — the
// volts are what the circuit is checked against, the percentage is the grade the
// part is ordered by.
inline std::string vz_band(const json& p) {
    auto lo = num(p, "vz_min_V"), hi = num(p, "vz_max_V"), tol = num(p, "vz_tolerance_pct");
    if (!lo || !hi) return tol ? "a " + plain_pct(*tol) + " grade" : "";
    std::string s = volts(*lo) + "-" + volts(*hi) + " V";
    if (tol) s += " (a " + plain_pct(*tol) + " grade)";
    return s;
}

// The supply range an oscillator record states, for a note: "2.5 V" for a fixed-rail
// part, "1.71-3.63 V" for a wide-range one, and says so when one end is not stated
// rather than inventing it.
inline std::string supply_range(const json& p) {
    auto lo = num(p, "supply_min_V"), hi = num(p, "supply_max_V");
    if (lo && hi) return *lo == *hi ? volts(*lo) + " V" : volts(*lo) + "-" + volts(*hi) + " V";
    if (lo) return "at least " + volts(*lo) + " V (no maximum stated)";
    if (hi) return "at most " + volts(*hi) + " V (no minimum stated)";
    return "an unstated supply";
}

// The land a footprint note is talking about, longest axis first — the same way
// round the fit test compares them, so two parts read comparably. A record that
// states only one land axis says which axis it is and that the other is absent,
// rather than inventing it.
// The inductance-drop criteria a saturation-current record states, for a note.
inline std::string isat_basis(const std::vector<isat::Point>& pts) {
    std::string out;
    for (const auto& p : pts) {
        if (!p.percent_drop) continue;
        char buf[48];
        std::snprintf(buf, sizeof buf, "%g A at %g %% drop", p.current, *p.percent_drop);
        out += (out.empty() ? "" : ", ") + std::string(buf);
    }
    if (out.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%g A at no stated criterion",
                      pts.empty() ? 0.0 : pts.front().current);
        out = buf;
    }
    return out;
}

inline std::string land_mm(const Dims& d) {
    if (has_land(d))
        return mm(std::max(*d.length, *d.width)) + " x " + mm(std::min(*d.length, *d.width)) +
               " mm";
    if (d.length) return mm(*d.length) + " mm long, width not stated";
    if (d.width) return mm(*d.width) + " mm wide, length not stated";
    return "no land dimensions on record";
}

// ── Which datasheet fields a comparison rests on (owner 2026-11-13, S1) ─────
// A row's `retracted_fields` lists the datasheetInfo fields of its record that rest only on
// retracted provenance (two path segments, Views.hpp retracted_only_fields). A comparison
// drawn from one of them is UNVERIFIED. This names, per category and comparison (params[] name),
// the fields the compared value is read from by the extractor (src/Views.cpp). A comparison
// not listed here is not attributed to any field, so a retraction never touches it.
inline std::vector<std::string> comparison_source_fields(const std::string& cat,
                                                         const std::string& name,
                                                         const json& row) {
    using V = std::vector<std::string>;
    // Body size and case code: the footprint and clearance axes (fill_dimensions).
    static const V kBody = {"mechanical.length",  "mechanical.width",     "mechanical.height",
                            "mechanical.diameter", "mechanical.thickness", "mechanical.dimensions",
                            "mechanical.landPattern", "mechanical.case",   "part.case",
                            "part.package"};
    static const V kHeight = {"mechanical.height", "mechanical.thickness", "mechanical.dimensions"};
    static const V kMount = {"mechanical.assemblyType", "mechanical.mountingStyle",
                             "mechanical.shape", "mechanical.mounting"};
    static const V kOpTemp = {"thermal.operatingTemperature"};
    if (name == "footprint") return kBody;
    if (name == "height") return kHeight;
    if (name == "mounting") return kMount;
    if (cat == "diode") {
        if (name == "vrrm") {
            const std::string t = str(row, "technology");
            if (t == "tvs" || t == "esd") return {"electrical.standoffVoltage"};
            if (t == "zener") return {"electrical.breakdownVoltage"};
            return {"electrical.reverseVoltage"};
        }
        if (name == "if_avg") return {"electrical.forwardCurrent"};
        if (name == "vf") return {"electrical.forwardVoltage"};
        if (name == "qrr") return {"electrical.reverseRecoveryCharge"};
        if (name == "trr") return {"electrical.reverseRecoveryTime"};
        if (name == "zener_voltage" || name == "breakdown_window" || name == "vz_tolerance_pct")
            return {"electrical.breakdownVoltage"};
        if (name == "standoff_band") return {"electrical.standoffVoltage"};
        if (name == "power_dissipation") return {"electrical.powerDissipation"};
        if (name == "tj_max") return {"thermal.junctionTemperatureMax"};
        if (name == "junction_capacitance") return {"electrical.junctionCapacitance"};
        // The measured curves (owner curve rule 2026-11-14), each read from its own curve.
        if (name == "vf_vs_if") return {"curves.forwardVoltageVsCurrent"};
        if (name == "ir_vs_vr") return {"curves.reverseCurrentVsVoltage"};
        if (name == "cj_vs_vr") return {"curves.capacitanceVsVoltage"};
        if (name == "junction_capacitance_basis")
            return {"electrical.junctionCapacitance", "electrical.junctionCapacitanceVr"};
        if (name == "clamping_voltage") return {"electrical.clampingVoltage"};
        if (name == "peak_pulse_power") return {"electrical.peakPulsePower"};
        if (name == "peak_pulse_current") return {"electrical.peakPulseCurrent"};
        if (name == "configuration") return {"part.dieConfiguration", "part.internalConnection"};
        if (name == "technology") return {"part.subType"};
        if (name == "automotive_grade") return {"part.qualification"};
        if (name == "temp_min_C" || name == "temp_max_C") return kOpTemp;
        return {};
    }
    if (cat == "mosfet") {
        if (name == "vds") return {"electrical.drainSourceVoltage"};
        if (name == "id") return {"electrical.continuousDrainCurrent"};
        if (name == "rds_on") return {"electrical.onResistance"};
        // The measured curves (owner curve rule 2026-11-14), each read from its own curve.
        if (name == "ciss_vs_vds") return {"curves.cissVsVds"};
        if (name == "coss_vs_vds") return {"curves.cossVsVds"};
        if (name == "crss_vs_vds") return {"curves.crssVsVds"};
        if (name == "rds_on_vs_id") return {"curves.rdsOnVsId"};
        if (name == "rds_on_vs_tj") return {"curves.rdsOnVsTj"};
        if (name == "qg_vs_vgs") return {"curves.gateCharge"};
        if (name == "id_vs_ambient") return {"electrical.ambientRatings"};
        if (name == "rds_on_basis") return {"electrical.onResistance", "electrical.onResistanceVgs"};
        if (name == "qg") return {"electrical.totalGateCharge"};
        if (name == "coss") return {"electrical.outputCapacitance"};
        if (name == "coss_basis")
            return {"electrical.outputCapacitance", "electrical.capacitanceMeasurementVds"};
        if (name == "vgs_threshold_max") return {"electrical.gateThresholdVoltage"};
        if (name == "vgs_max") return {"electrical.gateSourceVoltageMax"};
        if (name == "channel") return {"part.subType"};
        if (name == "technology") return {"part.technology"};
        if (name == "automotive_grade") return {"part.qualification"};
        return {};
    }
    if (cat == "capacitor") {
        if (name == "value") return {"electrical.capacitance"};
        if (name == "capacitance_tolerance") return {"electrical.capacitance"};
        if (name == "voltage") return {"electrical.ratedVoltage"};
        if (name == "voltage_ac") return {"electrical.voltageRatedAcMax"};
        if (name == "esr") return {"electrical.esr", "electrical.esrPoints"};
        if (name == "esr_basis") return {"electrical.esr", "electrical.esrFrequency"};
        if (name == "ripple_current")
            return {"electrical.rippleCurrent", "electrical.rippleCurrentFrequency",
                    "electrical.rippleCurrentTemperature"};
        if (name == "technology") return {"part.technology", "part.subType"};
        if (name == "dielectric_code") return {"part.dielectricCode"};
        if (name == "safety_class") return {"part.safetyClass"};
        if (name == "qualified_reliability") return {"part.qualifiedReliability"};
        if (name == "lead_form") return {"mechanical.shape"};
        if (name == "temp_max_C") return {"thermal.operatingTemperature", "thermal.temperature"};
        return {};
    }
    if (cat == "magnetic" || cat == "chipBead") {
        if (name == "value") return {"electrical.inductance"};
        if (name == "saturation_current")
            return {"electrical.saturationCurrents", "electrical.saturationCurrentPeak"};
        if (name == "rated_current") return {"electrical.ratedCurrents", "electrical.ratedCurrentPoints"};
        if (name == "dcr") return {"electrical.dcResistance", "electrical.dcResistances"};
        if (name == "srf") return {"electrical.selfResonantFrequency"};
        if (name == "impedance_100mhz" || name == "impedance_peak" || name == "impedance_peak_freq")
            return {"electrical.impedancePoints"};
        if (name == "temp_min_C" || name == "temp_max_C") return kOpTemp;
        return {};
    }
    if (cat == "connector") {
        if (name == "rated_current_A") return {"electrical.ratedCurrentPerContact"};
        // The derating curve is a fraction, compared in amperes (x the rated current).
        if (name == "current_vs_ambient")
            return {"derating.currentVsAmbient", "electrical.ratedCurrentPerContact"};
        if (name == "rated_voltage_V") return {"electrical.ratedVoltage"};
        if (name == "positions") return {"mechanical.positions"};
        if (name == "pitch_mm") return {"mechanical.pitch"};
        if (name == "rows") return {"mechanical.rows"};
        if (name == "mating_cycles") return {"mechanical.matingCycles"};
        if (name == "orientation") return {"mechanical.orientation"};
        if (name == "polarity") return {"part.matingPolarity"};
        if (name == "contact_plating") return {"material.contactPlating"};
        if (name == "family" || name == "interface_standard" || name == "termination" ||
            name == "actuator_type" || name == "coupling_type" || name == "coding" ||
            name == "shell_size")
            return {"familyDetails." + std::string(name == "interface_standard" ? "interfaceStandard"
                                                   : name == "actuator_type"    ? "actuatorType"
                                                   : name == "coupling_type"    ? "couplingType"
                                                   : name == "shell_size"       ? "shellSize"
                                                                                : name)};
        if (name == "temp_min_C" || name == "temp_max_C") return {"environmental.operatingTemperature"};
        return {};
    }
    if (cat == "timeBase") {
        if (name == "frequency") return {"electrical.frequency"};
        if (name == "tolerance_ppm") return {"electrical.frequencyTolerance"};
        if (name == "stability_ppm") return {"electrical.frequencyStability"};
        if (name == "load_capacitance" || name == "load_capacitance_pF")
            return {"electrical.loadCapacitance"};
        if (name == "built_in_capacitance") return {"electrical.builtInCapacitance"};
        if (name == "esr") return {"electrical.equivalentSeriesResistance"};
        if (name == "supply_min_V" || name == "supply_max_V" || name == "current_consumption")
            return {"electrical.supply"};
        if (name == "technology") return {"electrical.technology"};
        if (name == "output_type") return {"electrical.outputType"};
        if (name == "mode") return {"electrical.mode"};
        if (name == "enable_function") return {"electrical.enableFunction"};
        if (name == "temp_min_C" || name == "temp_max_C") return kOpTemp;
        return {};
    }
    return {};
}

// ── Critical ratings ─────────────────────────────────────────────────────────
// Directional ratings that PARAM_SPECS leaves out because Heaviside gates them
// in its stress guardrails (G7 voltage stress, G8 current stress). `hard` means
// a FAIL against a KNOWN original rejects the candidate: shipping a 40 V FET in
// place of a 100 V one is not a partial substitution, it is a field failure.
struct Rating {
    const char* key;
    Mode mode;
    double warn_factor;
    double gate_factor;
    bool hard;
};

inline std::vector<Rating> critical_ratings(const std::string& cat) {
    if (cat == "mosfet")
        return {{"vds", Mode::HigherBetter, 0.95, 0.9, true},
                {"id", Mode::HigherBetter, 0.9, 0.7, false}};
    if (cat == "diode")
        return {{"vrrm", Mode::HigherBetter, 0.95, 0.9, true},
                {"if_avg", Mode::HigherBetter, 0.9, 0.7, false}};
    if (cat == "capacitor") return {{"voltage", Mode::HigherBetter, 0.95, 0.9, true}};
    if (cat == "igbt")
        return {{"vces", Mode::HigherBetter, 0.95, 0.9, true},
                {"ic", Mode::HigherBetter, 0.9, 0.7, false}};
    return {};
}

// Parameters inside PARAM_SPECS whose FAIL is fatal rather than a demotion:
// a severe shortfall on a magnetic's current rating, or a mismatch on an
// IDENTITY parameter (a 6-position connector is not a 4-position one; a dual
// op-amp is not a quad). Everything else demotes to 'partial'.
// Primary RATINGS for the original-only rule (owner 2026-11-13, Y2): a rating the ORIGINAL's
// record does not state while the substitute's does caps every such candidate at
// minor_review, with a note — nothing holds the substitute to the original's requirement on
// the axis the match turns on, so "drop_in" would be a claim about a comparison never made.
// The axes are the primary ratings of the grade table in score_candidate; identities (connector
// gender / positions, analog channels, timing frequency, ...) are not ratings and keep their
// own rules, as do the current ratings (Id / If(AV) / Ic) that Y1 already caps from either side.
inline bool is_primary_rating(const std::string& cat, const std::string& key) {
    static const std::map<std::string, std::set<std::string>> k = {
        {"mosfet", {"vds", "rds_on"}},
        {"diode", {"vrrm", "power_dissipation", "peak_pulse_power", "peak_pulse_current"}},
        {"igbt", {"vces"}},
        {"capacitor", {"voltage", "ripple_current"}},
        {"magnetic", {"saturation_current", "rated_current"}},
        {"chipBead", {"impedance_100mhz", "impedance_peak", "rated_current"}},
        {"connector", {"rated_current_A", "rated_voltage_V"}},
        {"varistor", {"clamping_voltage", "peak_surge_current"}},
        {"resistor", {"power_rating"}}};
    const auto it = k.find(cat);
    return it != k.end() && it->second.count(key);
}
// Both-unknown primary cap (owner decision 2026-11-14; both_unstated_primary in
// score_candidate): a primary axis stated by NEITHER record caps the grade at minor_review —
// but only where that axis is a real datasheet rating for the ORIGINAL's technology. A rating
// the technology does not publish (ripple current on an MLCC, If(AV) on a TVS) is missing from
// every record of that kind, so its absence says nothing about the pair. The one table:
//
//   category   axis (key)                       technologies it applies to
//   ---------  -------------------------------  -------------------------------------------
//   capacitor  ripple current (ripple_current)  aluminium electrolytic (wet, hybrid, polymer)
//                                               and tantalum polymer; NOT ceramic (MLCC),
//                                               film, tantalum MnO2, others
//   diode      If(AV) (if_avg)                  rectifier, schottky, sicSchottky,
//                                               fastRecovery, ultrafast; NOT tvs/esd/zener/
//                                               switching/pin
//   diode      Ppk, Ipp (peak_pulse_power,      tvs, esd
//              peak_pulse_current)
//   diode      Pd (power_dissipation)           zener, tvs, esd, switching, pin; NOT the
//                                               rectifiers (rectifier, schottky, sicSchottky,
//                                               fastRecovery, ultrafast)
//   magnetic   Isat, rated current              inductor, coupledInductor (device_type);
//              (saturation_current,             NOT transformer, commonModeChoke, cableCore
//              rated_current)
//   mosfet     Vds, Id, Rds(on)                 every MOSFET
//   connector  rated current (rated_current_A)  every connector; NOT rated voltage
//
// Every other primary axis (capacitor rated voltage, diode Vrrm, IGBT, chip bead, varistor,
// resistor power, connector rated voltage) has no both-unknown cap. An original whose
// technology is not stated gives no cap on a technology-scoped axis: whether the rating exists
// for it is unknown, and no technology is assumed.
inline bool both_unknown_cap_applies(const std::string& cat, const std::string& key,
                                     const std::string& original_technology) {
    const std::string& t = original_technology;
    if (cat == "mosfet") return key == "vds" || key == "id" || key == "rds_on";
    if (cat == "connector") return key == "rated_current_A";
    if (cat == "magnetic")
        return (key == "saturation_current" || key == "rated_current") &&
               (t == "inductor" || t == "coupledInductor");
    if (cat == "capacitor") {
        if (key != "ripple_current") return false;
        const CapFamily f = cap_family(t);
        return f == CapFamily::AluminiumWet || f == CapFamily::AluminiumHybrid ||
               f == CapFamily::AluminiumPolymer || f == CapFamily::TantalumPolymer;
    }
    if (cat == "diode") {
        static const std::set<std::string> rectifiers{"rectifier", "schottky", "sicSchottky",
                                                      "fastRecovery", "ultrafast"};
        static const std::set<std::string> pd{"zener", "tvs", "esd", "switching", "pin"};
        if (key == "if_avg") return rectifiers.count(t) > 0;
        if (key == "peak_pulse_power" || key == "peak_pulse_current")
            return t == "tvs" || t == "esd";
        if (key == "power_dissipation") return pd.count(t) > 0;
        return false;
    }
    return false;
}
inline std::string primary_rating_label(const std::string& key) {
    static const std::map<std::string, std::string> k = {
        {"vds", "drain-source voltage rating (Vds)"},
        {"rds_on", "on-resistance (Rds(on))"},
        {"vrrm", "reverse voltage rating"},
        {"power_dissipation", "power dissipation rating"},
        {"peak_pulse_power", "peak pulse power"},
        {"peak_pulse_current", "peak pulse current"},
        {"vces", "collector-emitter voltage rating (Vces)"},
        {"voltage", "rated voltage"},
        {"rated_current", "rated current"},
        {"clamping_voltage", "clamping voltage"},
        {"peak_surge_current", "peak surge current"},
        {"power_rating", "power rating"}};
    const auto it = k.find(key);
    return it != k.end() ? it->second : param_label(key);
}

// Whether a spec states ANY physical datum: a case / package string, a body or lead-pitch
// dimension, or a mount (connectors: `mounting`). The no-physical-data rule
// (kNoPhysicalDataPenalty) reads it.
inline bool states_physical_data(const json& p) {
    for (const char* k : {"case_code", "package", "mount", "mounting"})
        if (!str(p, k).empty()) return true;
    for (const char* k : {"length_m", "width_m", "height_m", "lead_pitch_m"})
        if (num(p, k)) return true;
    return false;
}

inline bool is_hard_param(const std::string& cat, const std::string& key) {
    static const std::set<std::string> magnetic{"saturation_current", "rated_current"};
    // A connector is half of a mated pair: the other gender or another interface
    // standard does not plug into what is on the board. Only a MISMATCH rejects — a
    // record that omits one is unverified, not refused (none of these excludes a
    // missing substitute). Pitch is deliberately NOT here: a different pitch is
    // reported as a land-pattern change (partial, redesign), ranked below every part
    // that keeps the pads. The board attach (smt / tht / pressFit ...) and the
    // mating-axis orientation are identities too, but each is decided by its own
    // gate (the shared mount gate, the connector orientation block), not here.
    // termination (familyDetails.termination: crimp / idc / poke-in / solderCup / screw ...)
    // is the wire-side attach — an IDC socket for a crimp housing changes the harness, not a
    // parameter of the part — so a STATED difference rejects like the other identities
    // (Qarlos 2026-11-09, HIF3B-10D-2.54R(63)). One side only: unverified and capped below
    // drop_in (the one-sided identity block after the PARAM_SPECS loop).
    // The family-variant identities (familyFpcFfc.actuatorType, familyCircular.couplingType /
    // .coding / .shellSize) follow interface_standard exactly (owner 2026-11-12, "Identity
    // when stated"): a stated difference rejects; one side only is unverified with a note.
    static const std::set<std::string> connector{
        "family",        "positions",     "polarity", "interface_standard", "termination",
        "actuator_type", "coupling_type", "coding",   "shell_size"};
    static const std::set<std::string> analog{"subtype", "channels"};
    // mode: a 3rd-overtone crystal cannot cross with a fundamental one in either
    // direction — an overtone circuit's LC tank is inductive at the fundamental,
    // so the loop cannot close there; drop an overtone part into a fundamental
    // circuit and it either fails to start or runs near a third of the marking.
    // output_type: LVDS / HCSL / LVPECL / CMOS are different termination
    // networks, not a parameter — swapping one for another is a board respin.
    // Both are self-selecting: mode is absent on oscillators and output_type on
    // bare crystals, and an absent parameter is simply not compared.
    // enable_function: pin 1's function (output enable / standby / none) is wired by the
    // board, so a different one is a different pinout (CrossRefParams.hpp kTimeBase).
    // technology: an identity except the two owner-approved crossings (MEMS <-> quartz XO,
    // TCXO for a quartz XO; timing_technology_cross), which score_candidate turns into a WARN
    // before this gate is consulted.
    static const std::set<std::string> timebase{"subtype", "technology", "frequency", "mode",
                                                "output_type", "enable_function"};
    if (cat == "magnetic" || cat == "chipBead") return magnetic.count(key) > 0;
    if (cat == "mosfet") return key == "channel";
    if (cat == "connector") return connector.count(key) > 0;
    if (cat == "analog") return analog.count(key) > 0;
    if (cat == "timeBase") return timebase.count(key) > 0;
    return false;
}

// The family-variant connector identities (owner 2026-11-12): the label a note uses,
// naming the CONAS field it was read from. nullptr for any other key.
inline const char* connector_variant_axis_label(const std::string& key) {
    if (key == "actuator_type") return "FPC actuator type (familyFpcFfc.actuatorType)";
    if (key == "coupling_type") return "circular coupling type (familyCircular.couplingType)";
    if (key == "coding") return "circular coding (familyCircular.coding)";
    if (key == "shell_size") return "circular shell size (familyCircular.shellSize)";
    return nullptr;
}

struct Options {
    double primary_weight = 4.0;    // value proximity dominates (_VALUE_MATCH_WEIGHT)
    double gate_weight = 1.0;
    double overdim_weight = 0.1;    // small tie-breaker toward right-sizing
    double footprint_weight = 1.0;  // Heaviside's penalties are already scaled
    bool original_verified = true;  // caller: was the original's spec block resolved?
    bool check_footprint = true;
    bool check_lifecycle = true;
    std::optional<double> operating_voltage;  // enables the MLCC DC-bias comparison
    // What the CIRCUIT needs, not what the original happened to have: the
    // minimum value of a higher-is-better parameter at the operating point
    // (a magnetic's saturation_current >= the peak current, a capacitor's voltage
    // >= V_peak x derating). Callers own the margins; the ranker only enforces
    // them. Below a requirement is a hard reject; a record that does not state
    // the parameter cannot be shown to meet it and is capped at partial.
    std::map<std::string, double> requirements;
    size_t max_results = 25;
};

// A capacitor above this multiple of the original's value is capped at partial.
inline constexpr double kCapDropInMaxRatio = 1.2;
// When the original states no temperature grade, a candidate below 125 C pays
// this much per degree short — a tie-breaker only (85 C X5R vs 125 C X7R costs
// 0.08, far under any real parameter term), so the more capable grade wins a tie.
inline constexpr double kUnknownTempGradePenaltyPerC = 0.002;

// Non-numeric verdict penalties. Sized to sit in the same 0-5 band as the
// numeric terms so no single categorical miss can outweigh the primary value.
inline constexpr double kVerdictWarnPenalty = 0.5;
inline constexpr double kVerdictFailPenalty = 3.0;
// Gross over-rating (owner 2026-11-13, Y4; resistor power rating added by the owner
// 2026-11-14): a substitute whose primary voltage, current or (resistor) power rating is
// more than kGrossOverRatingFactor times the original's costs
// kGrossOverRatingPenalty per such axis, plus a note naming it — a 200 V / 3 A rectifier for
// a 70 V / 70 mA signal Schottky is "better" on paper and very likely a bigger body, more
// cost and more capacitance. A fixed step, well below a WARN (0.5), so it only orders among
// otherwise-equal candidates; grade and direction are unchanged.
inline constexpr double kGrossOverRatingFactor = 3.0;
// A candidate that states NO physical data at all — no case / package string, no body or lead
// dimension, no mount — against an original that states at least one (owner 2026-11-14;
// states_physical_data): capped at minor_review, a note, and this small step on top of the
// unknown-footprint penalty, so parts of a known size rank first. Below a WARN: it orders, it
// does not judge (Qarlos r31 seed 34608982: Vishay D2TO020C1R910FTE3, whose record states no
// case at all, against a Bourns 0201 chip).
inline constexpr double kNoPhysicalDataPenalty = 0.25;
static_assert(kNoPhysicalDataPenalty < kVerdictWarnPenalty,
              "the no-physical-data step must stay below a WARN");
inline constexpr double kGrossOverRatingPenalty = 0.15;
static_assert(kGrossOverRatingPenalty < kVerdictWarnPenalty / 2,
              "Y4: the over-rating step must stay well below a WARN");
// A NUMERIC WARN's penalty scales with its deviation (owner, 2026-11-13). Every WARN used to
// cost a flat kVerdictWarnPenalty, so a Qg 3 % over the original's and one 93 % over it (both
// inside the 2x WARN band; STL13NM60N -> SiHH11N60EF / SiHF12N60E) cost the same, and so did
// a TVS standoff +17 % and +40 % (1.5SMC100CA -> 5.0SMDJ100CA / 5.0SMDJ120CA): the ranking
// could not tell a near-miss from a near-FAIL. The rule, linear in the compared value:
//
//     penalty = W + d * k * (F - W),   d = (value - pass limit) / (fail limit - pass limit)
//
// with W = kVerdictWarnPenalty, F = kVerdictFailPenalty, d in [0, 1] and
// k = kWarnDepthFailFraction. So a WARN at the pass boundary costs W (0.5) as before, one at
// the fail boundary costs W + 0.95 (F - W) = 2.875 — just under a FAIL's 3.0, so the FAIL
// stays the worse answer — and the penalty is monotonic in between. The GRADE is untouched:
// a WARN is still a WARN. A WARN that is not a position between two numeric limits (a class
// difference, an unstated basis, a numeric axis with no fail limit) keeps the flat W.
inline constexpr double kWarnDepthFailFraction = 0.95;
inline double warn_penalty(std::optional<double> depth) {
    if (!depth) return kVerdictWarnPenalty;
    const double d = std::clamp(*depth, 0.0, 1.0);
    return kVerdictWarnPenalty + d * kWarnDepthFailFraction * (kVerdictFailPenalty - kVerdictWarnPenalty);
}
// plain_pct prints a percentage to 0.01 %: the resolution a breakdown-grade note shows.
inline constexpr double kVzGradePrintResolutionPct = 0.01;

// The footprint term is a LADDER whose rungs must be monotone in how much board
// work the verdict implies, and the UNVERIFIED rung (kUnknownDimPenalty) is its
// hinge: the substitute might still drop in, the record just does not say. A
// footprint the ranker has ESTABLISHED to be a different land pattern — another
// case code, a body that no longer covers the pads, one that overhangs them — is
// a strictly WORSE answer than "cannot tell", because the answer is known and it
// is "no". So an established mismatch costs the unverified fit plus a warn step,
// and never less.
//
// Left unfloored, the categorical warn (0.5) and the dimensional scale (2.0 for
// an unverified fit) were two unrelated scales: a 0402 -> 0102 MELF the tool
// itself annotates as "a substitute to re-lay-out, not a drop-in" scored 0.5,
// while the exact-value, exact-power 0402 thin films whose records merely omit a
// width scored 2.0, and the re-layout parts headed the list (ABT #498).
//
// Owner 2026-11-13 (Y3) moved the hinge: the unverified rung now costs exactly the
// established-mismatch rung (2.5, kUnknownDimPenalty in CrossRefDimensions.hpp), so an
// unknown footprint never outranks a known different case on the footprint term; the ladder
// is fits < unverified == established different case <= the continuous overhang / undersize
// terms (floored at this value).
inline constexpr double kEstablishedFootprintMismatchPenalty = kUnknownDimPenalty;
static_assert(kUnknownDimPenalty >= kEstablishedFootprintMismatchPenalty,
              "Y3: an unknown footprint must cost at least a known different case");

// ── Match grade ──────────────────────────────────────────────────────────────
// The industry cross-reference graders (SiliconExpert A / A-U / A-D / B / C / D
// / SF, Z2Data Drop-In A / B / C) all express two things our status alone does
// not: whether the swap needs a BOARD change, and whether the substitute is
// better or worse than the original. Both are what an engineer actually asks.
//
// We express the same two axes, but from published rules rather than an
// undisclosed algorithm:
//   drop_in        — fits the original's footprint, no parameter regressions
//   minor_review   — fits, but with warnings worth a look
//   major_review   — fits, but a parameter regressed materially
//   redesign       — does not fit, or mount/family/process differs
//   no_substitute  — a hard gate failed
// direction: upgrade / equivalent / downgrade, from the per-parameter verdicts.
//
// `footprint_unverified` is set when the ORIGINAL has a known footprint but the
// substitute's could not be established (no mechanical drawing). "drop_in" asserts
// the part fits the original's land pattern — a claim we cannot make without the
// substitute's dimensions — so an unverified footprint caps the grade at
// minor_review however clean the electricals are.
//
// `missing_required_data` is the electrical twin of that: the substitute's record
// carries no value at all for a PRIMARY rating whose absence is disqualifying (ripple
// current on a capacitor, Isat on a magnetic; owner's primary/secondary rule of
// 2026-11-09, table at the grade site in score_candidate). Nothing FAILED — nothing
// was compared — but a part nobody can check on the spec that defines it is not a drop-in either,
// so it takes the same major_review cap a real regression would.
inline const char* grade_for(const std::string& status, FootprintTier fit, bool any_warn,
                             bool any_fail, bool footprint_unverified = false,
                             bool missing_required_data = false,
                             HeightFit height = HeightFit::Unknown) {
    if (status == "no_substitute") return "no_substitute";
    // Either axis can put the part outside the original's board space: the land
    // overhanging the pads, or the body no longer clearing what is above it.
    if (fit == FootprintTier::Overflows || height == HeightFit::MuchTaller) return "redesign";
    if (any_fail || missing_required_data) return "major_review";
    // A materially smaller body (strict_case families) is a footprint CHANGE — the
    // pads won't match — so it is a review, not a true drop-in.
    if (any_warn || fit == FootprintTier::OneSizeLarger || fit == FootprintTier::Smaller ||
        footprint_unverified)
        return "minor_review";
    return "drop_in";
}

// Physical size of a part: an explicit mechanical drawing when the record has
// one, else the category-aware case-code resolution. Metres.
//
// A drawing that states only ONE land axis is kept, not thrown away: the axis it
// does state settles what it settles (see compare_land). Requiring both blanked
// the footprint verdict entirely for the ~8.6k catalogue rows that state one —
// 3.2 x 1.6 mm 1206 bodies were offered against a 2.0 mm 0805 original with
// "footprint": null, no note, and the same penalty as the true 0805 parts (ABT
// #516). A case code that resolves to a COMPLETE outline still wins over a
// half-stated drawing; the partial drawing is the fallback, never the loser.
inline std::optional<Dims> dims_of(const json& p, const std::string& category) {
    auto l = num(p, "length_m"), w = num(p, "width_m"), h = num(p, "height_m");
    Dims drawn;
    if (l && *l > 0) drawn.length = *l;
    if (w && *w > 0) drawn.width = *w;
    if (h && *h > 0) drawn.height = *h;
    if (has_land(drawn)) return drawn;
    // Every axis the drawing states is kept, land or not. A record that states a
    // HEIGHT and no land is not a record with no dimensions: throwing it away made
    // the ranker print "mechanical dimensions unavailable for the substitute" beside
    // the height it had just listed, and silently disabled the clearance axis that
    // judges that height — an Abracon ALFB-01 (5.0 mm tall) came back minor_review
    // against a 0.35 mm chip bead with no height verdict at all (ABT #499). What a
    // land-less drawing cannot do is answer a FOOTPRINT question; that is what
    // has_any_land guards, not what is kept here.
    const auto partial = [&]() -> std::optional<Dims> {
        if (drawn.length || drawn.width || drawn.height) return drawn;
        return std::nullopt;
    };
    // Magnetics and chip beads: a bare case code is NOT a reliable footprint. A
    // 4-digit magnetic code like "1210" is ambiguous between an EIA chip
    // (3.2 x 2.5 mm) and a molded power inductor (~12 x 10 mm) — resolving it
    // either way fabricates a footprint the datasheet never gave us, and would let
    // a physically larger part read as a drop-in. For these families the footprint
    // is verified ONLY from an explicit mechanical drawing; absent that it stays
    // unknown, so the part cannot be graded a drop-in. (Same reason these
    // categories are excluded from the mount gate — their package strings vary too
    // much by series to classify.)
    if (category == "magnetic" || category == "chipBead") return partial();
    std::string code = str(p, "case_code");
    if (code.empty()) code = str(p, "package");
    // A leaded capacitor (through-hole mount or an axial / radial form on record) is never
    // sized from a chip case table (resolve_dimensions, leaded_body).
    const bool leaded_body = category == "capacitor" && (normalize_mount(str(p, "mount")) == "leaded" ||
                                                         !str(p, "lead_form").empty());
    // A single-letter capacitor case is a tantalum case letter, and a letter means a size
    // only within its series (owner 2026-11-13): CWR06's "A" is 2.54 x 1.27 mm, the EIA "A"
    // 3.2 x 1.6. Resolved through the series' own table, or not at all — then the record's
    // drawing stands, or the footprint is unverified. Never the EIA table by default.
    const bool letter_code = category == "capacitor" && !leaded_body && is_case_letter(code);
    if (auto coded = letter_code ? resolve_tantalum_letter(code, str(p, "family"))
                                 : resolve_dimensions(code, category, leaded_body)) {
        // The drawing's own height still stands where the table fixes no height —
        // a chip's height is not encoded in its case code.
        //
        // And a height the code resolver took from its outline TABLE is not the record's: it
        // is the table's typical body for the code. Judging clearance from it graded
        // CR0603-FX-1743ELF's height 'pass' against a 0.45 mm AC0603FR-07174KL when the
        // Bourns record states no height at all (Qarlos 2026-11-08). Only the record's own
        // drawing, or a code that spells its height out, answers the clearance question.
        if (!coded->height || coded->height_from_table) {
            coded->height = drawn.height;
            coded->height_from_table = false;
        }
        return coded;
    }
    return partial();
}

// One capacitor's side of the case gate (capacitor_case_identities): its case string, series,
// technology, whether its letter may be read as a chip case (never for a leaded body: the
// leaded_body rule dims_of applies), and the body its record DRAWS — length_m / width_m as
// stated, never a table outline resolved from the code.
inline CapacitorCaseSide capacitor_case_side(const json& p) {
    CapacitorCaseSide x;
    x.code = str(p, "case_code");
    x.series = str(p, "family");
    x.technology = str(p, "technology");
    x.letter_ok = !(normalize_mount(str(p, "mount")) == "leaded" || !str(p, "lead_form").empty());
    const auto l = num(p, "length_m"), w = num(p, "width_m");
    if (l && *l > 0 && w && *w > 0) {
        Dims d;
        d.length = *l;
        d.width = *w;
        x.drawn = d;
    }
    return x;
}

// The two case identities the case-kept gates compare. For capacitors they are
// series-aware: an EIA-letter series' case letter is its EIA size code (owner 2026-11-13,
// capacitor_case_identities); a leaded capacitor's letter is never a chip case (the same
// leaded_body rule dims_of applies). Every other family: case_identity on both codes.
inline std::pair<std::string, std::string> case_identities_of(const std::string& cat,
                                                              const json& original,
                                                              const json& cand) {
    const std::string oc = str(original, "case_code"), sc = str(cand, "case_code");
    if (cat != "capacitor") return {case_identity(cat, oc), case_identity(cat, sc)};
    return capacitor_case_identities(capacitor_case_side(original), capacitor_case_side(cand));
}

// ── programmable oscillator frequency range (owner 2026-11-13, K2) ───────────
// A factory-programmable blank (TDAS technology "programmable", e.g. SiTime SiT3901) is sold
// for a SPAN of frequencies, stated as electrical.frequencyRange {minimum, maximum} (TDAS
// ab9f3c4); `frequency` is then absent, or the one frequency it was ordered at. The span is
// read only between two programmable parts: the technology is an identity
// (timing_technology_cross lists no crossing for "programmable"), so a programmable part
// never stands in for, or is replaced by, a fixed XO / MEMS / TCXO. Rules:
//   * the original states a frequency -> a substitute that states a frequency must equal it
//     (the ordinary exact identity, not handled here); a BLANK substitute (range, no
//     frequency) is admitted when the original's frequency lies inside its range — PASS,
//     with a note to order it programmed to that frequency — and rejected otherwise;
//   * the original states only a range -> the substitute's range must cover it, at both
//     ends; a substitute stating a frequency (an ordered part) cannot cover a range, and is
//     rejected;
//   * anything else (no range on the side that needs one) is the ordinary frequency compare.
// An original with neither a frequency nor a range is refused before ranking
// (web/src/crossref.js timingNeedsFrequency), worded as every missing-primary refusal.
// Range ends are compared within the representation epsilon (same_stated_value).
struct ProgrammableFrequencyFit {
    bool applies = false;  // the frequency verdict below replaces the ordinary compare
    bool pass = false;
    // A blank substitute admitted for an original stating one fixed frequency: it needs an
    // ordering step (programmed to that frequency), so the grade is capped at minor_review.
    bool blank_for_fixed = false;
    // The rejection is for an ABSENT datum (the substitute states neither a frequency nor a
    // range), not a compared mismatch: the reject reads direction 'unknown'.
    bool unverifiable = false;
    std::string note;
    const char* reject_reason = nullptr;
};

inline ProgrammableFrequencyFit programmable_frequency_fit(const json& original,
                                                           const json& cand) {
    ProgrammableFrequencyFit out;
    if (str(original, "technology") != "programmable" ||
        str(cand, "technology") != "programmable")
        return out;
    const auto o_f = num(original, "frequency"), s_f = num(cand, "frequency");
    const auto o_lo = num(original, "frequency_range_min"),
               o_hi = num(original, "frequency_range_max");
    const auto s_lo = num(cand, "frequency_range_min"), s_hi = num(cand, "frequency_range_max");
    const bool o_range = o_lo && o_hi, s_blank = !s_f && s_lo && s_hi;
    auto at_or_above = [](double v, double floor) { return v >= floor || same_stated_value(v, floor); };
    auto hz = [](double v) {
        char b[64];
        std::snprintf(b, sizeof b, "%.10g Hz", v);
        return std::string(b);
    };
    auto span = [&](double lo, double hi) { return hz(lo) + " to " + hz(hi); };
    if (o_f) {
        if (!s_blank) return out;  // a stated frequency: the ordinary exact identity
        out.applies = true;
        out.pass = at_or_above(*o_f, *s_lo) && at_or_above(*s_hi, *o_f);
        out.blank_for_fixed = out.pass;
        if (out.pass) {
            out.note = "programmable blank: the original's " + hz(*o_f) +
                       " lies inside the substitute's programmable range (" + span(*s_lo, *s_hi) +
                       ") — order it programmed to " + hz(*o_f);
        } else {
            out.note = "the original's " + hz(*o_f) +
                       " lies outside the substitute's programmable range (" + span(*s_lo, *s_hi) +
                       ") — it cannot be programmed to the original's frequency";
            out.reject_reason = "the original's frequency lies outside the substitute's "
                                "programmable range";
        }
        return out;
    }
    if (!o_range) return out;
    out.applies = true;
    if (s_blank) {
        out.pass = at_or_above(*o_lo, *s_lo) && at_or_above(*s_hi, *o_hi);
        if (out.pass) {
            out.note = "programmable range covers the original's: the substitute spans " +
                       span(*s_lo, *s_hi) + ", the original " + span(*o_lo, *o_hi);
        } else {
            out.note = "programmable range does not cover the original's: the substitute spans " +
                       span(*s_lo, *s_hi) + ", the original " + span(*o_lo, *o_hi) +
                       " — it cannot be programmed to every frequency the original can";
            out.reject_reason = "the substitute's programmable range does not cover the original's";
        }
        return out;
    }
    out.pass = false;
    out.note = s_f ? "the original is a programmable blank spanning " + span(*o_lo, *o_hi) +
                         "; the substitute is ordered at one frequency (" + hz(*s_f) +
                         ") — it does not cover the original's range"
                   : "the original is a programmable blank spanning " + span(*o_lo, *o_hi) +
                         "; the substitute's record states neither a frequency nor a "
                         "programmable range — its span cannot be checked";
    out.unverifiable = !s_f;
    out.reject_reason = s_f ? "a fixed-frequency part does not cover the original's "
                              "programmable range"
                            : "a parameter the match turns on is absent from the substitute's "
                              "record — cannot be verified";
    return out;
}

// ── resistor working voltage: like with like ─────────────────────────────────
// RAS electrical.maxVoltage is written two ways. Most records state the series LIMITING
// element voltage (RNMF14FTC12R4: 200 V); others state the rated continuous working voltage,
// which the datasheets define as sqrt(P x R) or that limit, whichever is less (YAGEO
// MFR-25FBF52-12R4: 1.76 V = sqrt(0.25 W x 12.4 ohm), its provenance says so). Compared as
// stated, the 1.76 V was FAILed against the 200 V, for two parts that carry the same voltage
// (Qarlos bench 20269028). The voltage a resistor can take continuously IS
// min(maxVoltage, sqrt(P x R)), and that reading is the same for both conventions — so when
// BOTH records state the voltage, the power rating and the resistance, the two are compared
// on it. Anything less stated on either side: compared exactly as stated, as before.
struct WorkingVoltage {
    bool applies = false;
    double orig = 0.0, subst = 0.0;     // V, min(maxVoltage, sqrt(P x R)) of each record
    bool orig_limited = false, subst_limited = false;  // sqrt(P x R) below the stated voltage
};
inline WorkingVoltage resistor_working_voltage(const json& original, const json& cand) {
    WorkingVoltage w;
    const auto ov = detail::jnum(original, "max_voltage_V"), sv = detail::jnum(cand, "max_voltage_V");
    const auto op = detail::jnum(original, "power_rating"), sp = detail::jnum(cand, "power_rating");
    const auto orr = detail::jnum(original, "value_si"), sr = detail::jnum(cand, "value_si");
    if (!ov || !sv || !op || !sp || !orr || !sr) return w;
    if (!(*ov > 0 && *sv > 0 && *op > 0 && *sp > 0 && *orr > 0 && *sr > 0)) return w;
    const double o_pr = std::sqrt(*op * *orr), s_pr = std::sqrt(*sp * *sr);
    w.applies = true;
    w.orig = std::min(*ov, o_pr);
    w.subst = std::min(*sv, s_pr);
    w.orig_limited = o_pr < *ov;
    w.subst_limited = s_pr < *sv;
    return w;
}

// ── resistor power at the original's rating temperature ─────────────────────
// A resistor's power rating is the power it carries up to a STATED ambient, and above that
// ambient it derates along its curve. RN50C5000FR36 is 0.05 W at 125 degC (flat to 125,
// zero at 175); CR0603-FX-5100ELF is 0.1 W at 70 degC and zero at 125 degC. Compared as
// 0.05 vs 0.1 the thick-film part "passed"; at the temperature the original is rated for it
// delivers nothing. So the comparison is made at the ORIGINAL's rating temperature, from
// what each record states — never from a typical curve:
//   * T_ref is the original's powerRatingTemperature, else the knee of its stated curve
//     (the end of the leading full-power run). Neither stated -> the ratings are compared
//     as stated, exactly as before (no basis exists for any candidate, so none is skewed).
//   * The original delivers its rating at T_ref (that is what the rating says).
//   * The substitute: from its stated curve, linearly interpolated at T_ref (the curves are
//     straight-line segments in the datasheets); 0 W above the curve's last temperature,
//     where the part is not rated to operate at all. With no curve, its full rating holds
//     when its own rating temperature is at or above T_ref (the definition of a rating
//     temperature). Anything else — no curve and a lower or unstated rating temperature,
//     or T_ref below the curve's first point — is NOT ON RECORD, and the verdict is
//     UNVERIFIED, never a pass on the nominal.
//
// Above the knee (Qarlos 2026-11-08, seed 20276947): RN55E90R9BRE6 is 0.125 W at 70 degC and
// still 0.1 W at 125 degC (curve 70/125/175 -> 100/80/0 %); MFP-25BRD52-91R is 0.25 W at 70
// degC but derates to zero at 155 degC, so at 125 degC it carries 0.088 W. Compared at T_ref
// alone it read "pass / upgrade". The rule is: the substitute must deliver at least the
// original's power at EVERY temperature where both records state a curve — the overlap of the
// two curves' spans. Both curves are straight-line segments, so the worst ratio s(T)/o(T) is
// found at a breakpoint of one curve or the other; every breakpoint in the overlap is checked
// (points where the original delivers nothing are skipped: nothing is required there). The
// verdict, penalty and direction are taken at the WORST such temperature. Outside the overlap
// nothing is compared from the curves (the operating-range rule answers that).
struct PowerAtTemperature {
    bool applies = false;          // the original states a temperature basis
    double t_ref = 0.0;            // degC, where the comparison is made (see `at_knee`)
    std::optional<double> orig;    // W the original delivers at t_ref
    std::optional<double> subst;   // W the substitute delivers at t_ref; nullopt = not on record
    std::string how;               // how `subst` was established, or why it could not be
    // True when t_ref is the original's rating temperature; false when the curve scan found a
    // worse temperature above it and t_ref was moved there.
    bool at_rating_temperature = true;
    double rating_temperature = 0.0;  // degC, the original's rating temperature
};

namespace power_detail {
// Reads a stated curve as two equal-length arrays. Absent/null -> empty. A malformed curve is
// a caller error (the extractor validates every record), so it throws.
inline void curve(const json& p, std::vector<double>& t, std::vector<double>& a) {
    t.clear();
    a.clear();
    const auto ti = p.find("derating_temperature"), ai = p.find("derating_amplitude");
    const bool t_has = ti != p.end() && !ti->is_null(), a_has = ai != p.end() && !ai->is_null();
    if (!t_has && !a_has) return;
    if (!t_has || !a_has || !ti->is_array() || !ai->is_array() || ti->size() != ai->size() ||
        ti->size() < 2)
        throw std::invalid_argument("cross_reference: '" + str(p, "mpn") +
                                    "' derating_temperature/derating_amplitude must be "
                                    "equal-length arrays of >= 2 points");
    for (size_t i = 0; i < ti->size(); ++i) {
        if (!(*ti)[i].is_number() || !(*ai)[i].is_number())
            throw std::invalid_argument("cross_reference: '" + str(p, "mpn") +
                                        "' derating curve holds a non-numeric point");
        t.push_back((*ti)[i].get<double>());
        a.push_back((*ai)[i].get<double>());
        if (i > 0 && !(t[i] > t[i - 1]))
            throw std::invalid_argument("cross_reference: '" + str(p, "mpn") +
                                        "' derating temperatures must strictly increase");
    }
}
// End of the leading full-power run: the temperature up to which the curve carries 100 %.
inline std::optional<double> knee(const std::vector<double>& t, const std::vector<double>& a) {
    std::optional<double> k;
    for (size_t i = 0; i < t.size() && a[i] >= 1.0; ++i) k = t[i];
    return k;
}
inline std::string celsius(double t) {
    char b[32];
    std::snprintf(b, sizeof b, "%g degC", t);
    return b;
}
inline std::string watts(double w) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3g W", w);
    return b;
}
// Amplitude of a stated curve at temperature x, linearly interpolated. The caller keeps x
// inside [t.front(), t.back()].
inline double amplitude_at(const std::vector<double>& t, const std::vector<double>& a, double x) {
    for (size_t i = 0; i + 1 < t.size(); ++i) {
        if (x >= t[i] && x <= t[i + 1]) {
            const double f = (x - t[i]) / (t[i + 1] - t[i]);
            return a[i] + f * (a[i + 1] - a[i]);
        }
    }
    throw std::logic_error("power_detail::amplitude_at: temperature outside the curve");
}
}  // namespace power_detail

inline PowerAtTemperature power_at_rating_temperature(const json& original, const json& cand) {
    using namespace power_detail;
    PowerAtTemperature r;
    const auto o_p = num(original, "power_rating"), s_p = num(cand, "power_rating");
    std::vector<double> ot, oa, st, sa;
    curve(original, ot, oa);
    curve(cand, st, sa);
    std::optional<double> t_ref = num(original, "power_rating_temperature");
    if (!t_ref && !ot.empty()) t_ref = knee(ot, oa);
    if (!t_ref || !o_p || !s_p) return r;  // no basis, or a rating absent: plain compare
    r.applies = true;
    r.t_ref = *t_ref;
    r.rating_temperature = *t_ref;
    r.orig = *o_p;
    if (!st.empty()) {
        if (r.t_ref < st.front()) {
            r.how = "the substitute's derating curve starts at " + celsius(st.front()) +
                    ", above the original's rating temperature of " + celsius(r.t_ref);
            return r;
        }
        if (r.t_ref > st.back()) {
            r.subst = 0.0;
            r.how = "the substitute's derating curve ends at " + celsius(st.back()) +
                    " — it is not rated to operate at the original's " + celsius(r.t_ref);
            return r;
        }
        r.subst = *s_p * amplitude_at(st, sa, r.t_ref);
        r.how = "from the substitute's derating curve";
        // Above the knee: both curves stated -> every breakpoint of either curve inside
        // their common span (see the struct comment). The worst ratio wins, and only a
        // strictly worse one moves the comparison off the rating temperature.
        if (!ot.empty() && *r.orig > 0.0) {
            const double lo = std::max(ot.front(), st.front());
            const double hi = std::min(ot.back(), st.back());
            if (lo <= hi) {
                std::vector<double> probe{lo, hi};
                for (double x : ot)
                    if (x >= lo && x <= hi) probe.push_back(x);
                for (double x : st)
                    if (x >= lo && x <= hi) probe.push_back(x);
                double worst_ratio = *r.subst / *r.orig;
                for (double x : probe) {
                    const double o_w = *o_p * amplitude_at(ot, oa, x);
                    if (!(o_w > 0.0)) continue;  // the original carries nothing here
                    const double s_w = *s_p * amplitude_at(st, sa, x);
                    if (s_w / o_w < worst_ratio - 1e-12) {
                        worst_ratio = s_w / o_w;
                        r.t_ref = x;
                        r.orig = o_w;
                        r.subst = s_w;
                        r.at_rating_temperature = false;
                    }
                }
                if (!r.at_rating_temperature) r.how = "from both records' derating curves";
            }
        }
        return r;
    }
    if (const auto s_t = num(cand, "power_rating_temperature")) {
        if (*s_t >= r.t_ref) {
            r.subst = *s_p;
            r.how = "the substitute's rating holds to " + celsius(*s_t);
        } else {
            r.how = "the substitute is rated at " + celsius(*s_t) +
                    " and its record states no derating curve above that";
        }
        return r;
    }
    r.how = "the substitute's record states neither the temperature its rating applies at "
            "nor a derating curve";
    return r;
}

// ── connector pitch: does the pin field line up? ─────────────────────────────
// A connector record carries no body outline, so its contact pitch IS its land pattern:
// contact k of a row sits (k-1) x pitch from contact 1. Two pitches that differ by a few
// hundredths of a millimetre (5.00 / 5.08, 2.50 / 2.54) are a fit on a two-hole pattern
// and a misfit on a long row, because the difference ADDS UP along the row: a 24-way
// 5.00 mm strip in a 5.08 mm board puts its end pin 23 x 0.08 = 1.84 mm off its hole. A
// relative tolerance on the pitch alone (the old 2 % ExactMatch band) passed that pair.
//
// Rule (owner decision to confirm: the 0.1 mm limit is the one the owner approved for a
// two-hole 2.50/2.54 and 5.00/5.08 pattern, applied here to the FARTHEST pin):
//   offset = (positions_per_row - 1) x |pitch_original - pitch_substitute|
//   offset <= 0.1 mm  -> pass (the pin field lines up);
//   offset >  0.1 mm  -> fail: a different land pattern, reported exactly like any other
//                        pitch mismatch (footprint different_land_pattern, partial), with
//                        a note giving both pitches, the per-row count and the end-pin offset.
// positions_per_row = ceil(positions / rows), taken from each record that states both
// (mechanical.positions and mechanical.rows); when both records state it, the LONGER row
// decides (that row's end pin is the one that has to reach its hole). When neither record
// states rows, the row length is not known and is only BOUNDED, never assumed:
//   * from below by 2 — a stated contact-to-contact pitch means at least two contacts sit
//     that far apart, so a difference over 0.1 mm already misplaces the first neighbour
//     -> fail (certain whatever the row length);
//   * from above by the stated position count (a row cannot hold more contacts than the
//     part has), so an offset within 0.1 mm even over the whole count -> pass;
//   * in between, the arrangement decides and nobody stated it -> unverified with a note.
// With no position count on either side, a non-zero difference within 0.1 mm is
// unverified: two positions is NOT assumed as the row length.
// Only decides when BOTH pitches are stated; otherwise the PARAM_SPECS table's own
// missing-value handling applies.
// Row count: when BOTH records state mechanical.rows and they differ (1 row against 2), the
// pins are arranged differently and cannot meet the same holes whatever the pitch — a
// different pin field, reported like a pitch mismatch (pitch FAIL when both pitches are
// stated, footprint different_land_pattern in every case), with a note naming both row
// counts. A row count stated on one side only, or on neither, gives no verdict on its own.
struct ConnectorPitchFit {
    const char* verdict = nullptr;  // PASS / FAIL / UNVERIFIED; nullptr = not decided here
    double per_row = 0.0;           // contacts per row the verdict was taken over (0 = unknown)
    double offset_mm = 0.0;         // end-pin offset over that row
    bool rows_differ = false;       // both records state rows, and they differ
    double o_rows = 0.0, s_rows = 0.0;
    std::string note;
};
constexpr double kConnectorPinFieldLimitMm = 0.1;
inline ConnectorPitchFit connector_pitch_fit(const json& original, const json& cand) {
    ConnectorPitchFit f;
    const auto o_r = num(original, "rows"), s_r = num(cand, "rows");
    if (o_r && s_r && *o_r != *s_r) {
        f.rows_differ = true;
        f.o_rows = *o_r;
        f.s_rows = *s_r;
    }
    const auto o_p = num(original, "pitch_mm"), s_p = num(cand, "pitch_mm");
    if (!o_p || !s_p) return f;
    if (f.rows_differ) {
        f.verdict = FAIL;
        return f;
    }
    const double d = std::fabs(*o_p - *s_p);
    // Floating-point equality only: any real catalogue difference is >= 0.01 mm.
    if (d <= 1e-9 * std::max(*o_p, *s_p)) {
        f.verdict = PASS;
        return f;
    }
    double per_row = 0.0;
    for (const json* rec : {&original, &cand}) {
        const auto n = num(*rec, "positions"), rows = num(*rec, "rows");
        if (n && rows && *rows >= 1.0) per_row = std::max(per_row, std::ceil(*n / *rows));
    }
    // The lower bound of 2 above holds here too: a stated contact-to-contact pitch means two
    // contacts sit that far apart, so a row is never read as one contact long. Amphenol
    // 10167520-121253CLF states 6 positions in 6 rows; the per-row count came out 1, the
    // end-pin offset 0, and its 3.0 mm pitch was graded 'pass' against TE 3-641148-5's
    // 3.96 mm (Qarlos r17 seed 27487410).
    if (per_row > 0.0) per_row = std::max(per_row, 2.0);
    const std::string pitches =
        "pitch " + mm(*o_p * 1e-3) + " -> " + mm(*s_p * 1e-3) + " mm";
    if (per_row > 0.0) {
        f.per_row = per_row;
        f.offset_mm = (per_row - 1.0) * d;
        f.verdict = f.offset_mm <= kConnectorPinFieldLimitMm ? PASS : FAIL;
        return f;
    }
    if (d > kConnectorPinFieldLimitMm) {
        // The adjacent contact already misses; the row length does not change the verdict.
        f.verdict = FAIL;
        return f;
    }
    double bound = 0.0;
    for (const json* rec : {&original, &cand})
        if (const auto n = num(*rec, "positions")) bound = std::max(bound, *n);
    if (bound >= 1.0 && (bound - 1.0) * d <= kConnectorPinFieldLimitMm) {
        f.verdict = PASS;
        return f;
    }
    f.verdict = UNVERIFIED;
    if (bound >= 1.0)
        f.note = pitches + ": neither record states how the " +
                 std::to_string(static_cast<long>(bound)) +
                 " positions are split into rows, and in a single row the end pin would sit " +
                 mm((bound - 1.0) * d * 1e-3) +
                 " mm off its hole (over 0.1 mm) — confirm the pin field lines up";
    else
        f.note = pitches + ": neither record states a position count, so the end-pin offset "
                 "cannot be worked out — confirm the pin field lines up";
    return f;
}

// Score one candidate against the original. Returns a per-candidate JSON verdict.
inline json score_candidate(const std::string& cat, const json& original, const json& cand,
                            const Options& opt) {
    json out;
    out["mpn"] = str(cand, "mpn");
    // Optional caller-supplied identity, echoed verbatim. Two vendors can ship
    // the same MPN string, so a caller that needs to map a result back to its
    // own row passes a key rather than overloading `mpn` — `mpn` must stay the
    // REAL part number, because the AEC-Q and rated-voltage gates decode it.
    //
    // The field is `_key`, deliberately underscore-prefixed: it previously used
    // `id`, which COLLIDES with the MOSFET drain-current parameter of the same
    // name. The identity string overwrote the current, num() then read a string
    // as absent, and the drain-current comparison silently disabled itself — a
    // 31 A part ranked top against a 200 A original. A datasheet parameter will
    // never be named with a leading underscore, so the namespace is safe.
    if (!str(cand, "_key").empty()) out["_key"] = str(cand, "_key");
    // Guard the class of bug rather than just this instance: a caller that puts
    // a STRING where a physical parameter is expected has masked that parameter,
    // and num() would read it as simply absent — the comparison then disables
    // itself in silence. Say so instead.
    for (const auto& r : critical_ratings(cat)) {
        auto it = cand.find(r.key);
        if (it != cand.end() && it->is_string())
            throw std::invalid_argument(std::string("cross_reference: '") + r.key +
                                        "' is a numeric parameter for category '" + cat +
                                        "' but was given a string — a caller key must not "
                                        "reuse a parameter name (use `_key`)");
    }
    double penalty = 0.0;
    std::string status = "recommended";
    json params = json::array();
    std::vector<std::string> notes;
    // A parameter whose absence disqualifies was absent on the substitute — an
    // unknown, not a regression, so it caps the grade rather than reading as one.
    bool missing_required_data = false;
    // A resistor's power at the original's rating temperature is not on the substitute's
    // record (see power_at_rating_temperature): UNVERIFIED, and the grade cannot be drop_in.
    bool power_basis_unverified = false;
    // SMD vs through-hole could not be established for this pair (compare_mount Unknown).
    bool mount_unverified = false;
    // An original's AC rating the substitute does not state (owner 2026-11-14): capped
    // minor_review like a missing primary rating.
    bool ac_rating_unverified = false;
    // A primary current rating (MOSFET Id, diode If(AV), IGBT Ic) stated by one record only
    // (owner 2026-11-13, Y1): UNVERIFIED with a note, capped at minor_review.
    bool current_rating_one_sided = false;
    // A cable core's cable diameter or pass count the substitute does not state while the
    // original does (owner 2026-11-14): UNVERIFIED, capped at minor_review.
    bool cable_core_unverified = false;
    // Primary ratings the ORIGINAL does not state and the substitute does (owner 2026-11-13,
    // Y2; is_primary_rating): each candidate is capped at minor_review with a note.
    std::vector<std::string> orig_unstated_primary;
    auto orig_unstated = [&](const std::string& key) {
        const std::string l = primary_rating_label(key);
        if (std::find(orig_unstated_primary.begin(), orig_unstated_primary.end(), l) ==
            orig_unstated_primary.end())
            orig_unstated_primary.push_back(l);
    };
    // Primary ratings stated by NEITHER record (owner decision 2026-11-14): each candidate is
    // capped at minor_review with a note naming the axes. This reverses, for primary axes
    // only, the 2026-11-12 rule "missing data on both sides is not capped": a drop_in claims
    // the axis the match turns on was compared, and here it never was. Scoped (owner
    // 2026-11-14) to the axes that are a real datasheet rating for the original's technology:
    // both_unknown_cap_applies holds the one table. Secondary axes stay uncapped, and the
    // footprint keeps its own owner rules (N1b: neither side states a land -> uncapped).
    std::vector<std::string> both_unstated_primary;
    auto both_unstated = [&](const std::string& key) {
        static const std::map<std::string, std::string> kCurrent = {
            {"id", "continuous drain current (Id)"},
            {"if_avg", "average forward current (If(AV))"}};
        const std::string tech =
            cat == "magnetic" ? str(original, "device_type") : str(original, "technology");
        if (!both_unknown_cap_applies(cat, key, tech)) return;
        const auto c = kCurrent.find(key);
        const std::string l = c != kCurrent.end() ? c->second : primary_rating_label(key);
        if (std::find(both_unstated_primary.begin(), both_unstated_primary.end(), l) ==
            both_unstated_primary.end())
            both_unstated_primary.push_back(l);
    };
    // Y4: one note and one small penalty per grossly over-rated primary V / A rating.
    auto gross_over_rating = [&](const std::string& key, std::optional<double> o,
                                 std::optional<double> s) {
        if (!o || !s || !(*o > 0) || !(*s > kGrossOverRatingFactor * *o)) return;
        static const std::map<std::string, std::pair<const char*, const char*>> kWhat = {
            {"vds", {"drain-source voltage rating", "V"}},
            {"id", {"continuous drain current rating", "A"}},
            {"vrrm", {"reverse voltage rating", "V"}},
            {"if_avg", {"average forward current rating", "A"}},
            {"voltage", {"rated voltage", "V"}},
            {"vces", {"collector-emitter voltage rating", "V"}},
            {"ic", {"continuous collector current rating", "A"}},
            {"saturation_current", {"saturation current", "A"}},
            {"rated_current", {"rated current", "A"}},
            {"rated_current_A", {"rated current", "A"}},
            {"rated_voltage_V", {"rated voltage", "V"}},
            // A resistor's power rating (owner 2026-11-14): a 35 W power resistor for a 50 mW
            // chip is the same over-rating — a bigger body and more cost (no capacitance claim).
            {"power_rating", {"power rating", "W"}}};
        const auto it = kWhat.find(key);
        if (it == kWhat.end()) return;
        penalty += opt.gate_weight * kGrossOverRatingPenalty;
        const char* likely = key == "power_rating"
                                 ? "a bigger body and higher cost are likely"
                                 : "a bigger body, higher cost and more capacitance are likely";
        char b[320];
        std::snprintf(b, sizeof b,
                      "%s %g %s is %.3gx the original's %g %s — far above what the original is "
                      "rated for (more than %gx): %s; ranked after closer-rated parts",
                      it->second.first, *s, it->second.second, *s / *o, *o, it->second.second,
                      kGrossOverRatingFactor, likely);
        notes.push_back(b);
    };
    // Direction bookkeeping: on each directional parameter we could compare,
    // did the substitute come out strictly ahead of the original, or behind?
    int better = 0, worse = 0;
    // A direction computed on the axes that happened to be present is not a verdict when
    // the axis the match TURNS ON was not among them. The case is specific: the SUBSTITUTE
    // states a hard-gate parameter and the ORIGINAL's record does not. The mirror image —
    // original states it, substitute silent — already rejects above, and when NEITHER states
    // it the axis is missing identically for every candidate, so it cannot skew one against
    // another. Only here did the ranker hold a real number it had no basis to judge, and
    // "upgrade" would be a claim about a comparison that never happened.
    bool direction_blind = false;
    std::vector<std::string> blind_axes;
    // Isat on both sides but at criteria that cannot be brought together (ABT #2042): the
    // same blindness, with its own note (written where it is detected).
    bool isat_basis_blind = false;
    // Magnetics: a key electrical axis the ORIGINAL states and the substitute's record does
    // not (rated current, DCR, SRF; Isat in that position already rejects). The axis stays
    // UNVERIFIED, and a direction drawn from the remaining axes would claim the substitute
    // is not behind on it: LQW04AN47NH20D came back 'upgrade' for CWW0402-47RGS on its rated
    // current alone, with the original's 0.83 ohm DCR and 2.1 GHz SRF unanswered (Qarlos
    // 2026-11-09, seed 20784754). Chip beads and signal CMCs (impedance model) take the same
    // rule. Rule: any such axis turns 'upgrade' / 'equivalent' into
    // 'unknown', exactly as direction_blind does; a measured regression still stands.
    std::vector<std::string> magnetic_unweighed;
    // Timing: the frequency tolerance or stability the ORIGINAL states and the substitute's
    // record does not. The grade is untouched (owner: stability / tolerance unstated is a
    // secondary axis, note only), but the direction follows the magnetic rule above: an
    // 'upgrade' drawn from the supply range alone claimed the TCXO was not behind on the
    // accuracy it exists for (AVTX-13-E-40.000MHZ-E05, +/-0.5 ppm -> ECS-TXO-25CSMV-400-AN-TR,
    // no stability on record; Qarlos r20 28744158). 'upgrade' / 'equivalent' -> 'unknown'.
    bool timing_accuracy_unweighed = false;
    // A magnetic's rated current stated by the original and absent from the substitute's
    // record (owner, 2026-11-13; precedent: capacitor ESR absent): the FAIL-sized penalty so
    // a part that states its rating ranks first, a note, the direction 'unknown' (as for the
    // other unweighed axes above), and NO demotion and NO cap. 744766115 (0.6 A) took
    // AMT45S-150-2P-TL000, whose record states no rated current, ahead of B82432T1153K000
    // (0.902 A stated; Qarlos r14 25916475). Magnetics only: chip beads share the axis in the
    // code (same PARAM_SPECS row and hard-param set) but the owner's decision names
    // magnetics, so a bead keeps the unweighed treatment.
    bool magnetic_rated_absent = false;
    // A magnetic's saturation current stated by NEITHER record (see the PARAM_SPECS loop): the
    // axis the match turns on was never compared, so 'upgrade' / 'equivalent' would be a claim
    // about a comparison not made — the direction is 'unknown' (owner, 2026-11-13).
    bool isat_unstated_both = false;
    auto note_direction = [&](Dir dir, std::optional<double> o, std::optional<double> s) {
        if (!o || !s || *o <= 0 || *s <= 0) return;
        const double ratio = *s / *o;
        if (ratio > 1.05) (dir == Dir::Higher ? better : worse)++;
        else if (ratio < 0.95) (dir == Dir::Higher ? worse : better)++;
    };

    // `unverifiable`: the candidate is rejected because a datum the match needs is ABSENT
    // (no land pattern on record, a hard-gate parameter missing), not because a compared
    // parameter came out worse. Its direction is then 'unknown': 'downgrade' claimed a
    // regression nobody measured (Qarlos r17 27173223: AISC-0402HP-21NJ-T -> AC2218SQ-23NG,
    // rejected for no dimensions, read 'downgrade' though its every compared parameter was
    // equal or better).
    auto reject = [&](const char* reason, bool unverifiable = false) {
        out["status"] = "no_substitute";
        out["reason"] = reason;
        out["penalty"] = 1e9;
        out["params"] = params;
        if (!notes.empty()) out["notes"] = notes;
        // A rejected candidate still carries grade/direction so every row in the
        // result has the same shape — a consumer should never have to special-case
        // the rejects to render a table.
        out["grade"] = "no_substitute";
        out["direction"] = unverifiable ? "unknown" : "downgrade";
        return out;
    };
    auto demote = [&] {
        if (status == "recommended") status = "partial";
    };

    // ── primary value ────────────────────────────────────────────────────────
    const auto o_val = num(original, "value_si"), s_val = num(cand, "value_si");
    const auto o_tol = num(original, "tolerance_pct"), s_tol = num(cand, "tolerance_pct");
    bool has_pv = false;
    // The original's tolerance bounds the pass window where the tolerance IS the
    // value's guarantee (resistors): a 0.25 % part cannot have a 0.93 % shift
    // called a drop-in on the strength of a catalogue-wide +/-1 % band.
    // Magnetics (owner 2026-11-13, R1): the original's STATED inductance band — the record's
    // inductance minimum / maximum, as percent below / above the nominal, possibly asymmetric —
    // bounds the PASS window the way a resistor's tolerance does. No band stated: the
    // category's +/-15 % window stands.
    const auto o_band_minus = num(original, "tolerance_minus_pct"),
               o_band_plus = num(original, "tolerance_plus_pct");
    const bool magnetic_band =
        cat == "magnetic" && tolerance_band_stated(o_band_minus, o_band_plus);
    // Capacitors (owner 2026-11-13, Z4): the original's STATED capacitance band (the row's
    // capacitance minimum / maximum in farads, as percent below / above its nominal) bounds the
    // PASS window the same way. Narrowed here, not in primary_value_spec("capacitor"), whose
    // 0.90-1.50 window the golden table pins; it only ever narrows, so a -20/+80 % part keeps
    // the 1.50 top. No band stated: the 0.90-1.50 window stands.
    std::optional<double> cap_band_minus, cap_band_plus;
    if (cat == "capacitor" && o_val && *o_val > 0) {
        const auto lo = num(original, "capacitance_min"), hi = num(original, "capacitance_max");
        if (lo && hi) {
            cap_band_minus = (1.0 - *lo / *o_val) * 100.0;
            cap_band_plus = (*hi / *o_val - 1.0) * 100.0;
        }
    }
    const bool capacitor_band =
        cat == "capacitor" && tolerance_band_stated(cap_band_minus, cap_band_plus);
    auto pv = magnetic_band
                  ? score_primary_value(cat, o_val, s_val, has_pv, o_band_minus, o_band_plus)
                  : score_primary_value(cat, o_val, s_val, has_pv, o_tol);
    if (capacitor_band) {
        PrimaryValueSpec cs{};
        has_pv = primary_value_spec(cat, cs);
        cs.tight_lo = std::max(cs.tight_lo, 1.0 - *cap_band_minus / 100.0);
        cs.tight_hi = std::min(cs.tight_hi, 1.0 + *cap_band_plus / 100.0);
        pv = score_range(o_val, s_val, cs.tight_lo, cs.tight_hi, cs.accept_lo, cs.accept_hi);
    }
    // A capacitance outside the original's stated band (inside the accept window): WARN with a
    // note, ranked after the in-tolerance candidates (the "value_differs" tier), as for an
    // inductance.
    bool capacitance_off_band = false;
    if (capacitor_band && has_pv && pv.verdict != FAIL && o_val && s_val && *s_val > 0 &&
        outside_band(*s_val / *o_val, *cap_band_minus, *cap_band_plus)) {
        capacitance_off_band = true;
        pv.verdict = WARN;
    }
    // An inductance outside the original's stated band (but inside the accept window) is a
    // different value: WARN (score_range already said so), ranked after every in-band and
    // exact-value candidate — the resistor near-value sort tier ("value_differs"). Grade and
    // direction are left to the WARN itself, as for an off-nominal zener.
    bool inductance_off_band = false;
    if (magnetic_band && has_pv && pv.verdict != FAIL && o_val && s_val && *o_val > 0 &&
        *s_val > 0 && outside_band(*s_val / *o_val, *o_band_minus, *o_band_plus)) {
        inductance_off_band = true;
        pv.verdict = WARN;
    }
    // Resistors: a different NOMINAL resistance is a different part, whatever the
    // tolerance bands say. The ±5 % pool window keeps near values on offer as
    // alternatives, but every one is a WARN on value, capped at major_review, never
    // read as upgrade/equivalent, and ranked after every exact-value candidate
    // (cross_reference sorts on "value_differs" before penalty — a tier, not a
    // penalty constant another term could outweigh). The shared primitive
    // primary_value_spec("resistor") is untouched: this is a Kelvin ranking rule.
    bool value_differs = false;
    if (has_pv && cat == "resistor" && pv.verdict != FAIL && o_val && s_val && *o_val > 0 &&
        std::fabs(*s_val / *o_val - 1.0) > kResistanceIdentityRelEps) {
        value_differs = true;
        pv.verdict = WARN;
    }
    if (has_pv) {
        params.push_back({{"name", "value"}, {"verdict", pv.verdict}});
        if (pv.verdict == FAIL) return reject("primary value out of range");
        penalty += opt.primary_weight * pv.penalty;
        // Inside the accepted window the primary score is flat, so a 20 uH part
        // tied an exact 22 uH one and the footprint decided. For RANKING the
        // value always pulls, if gently (the shared primitive stays unchanged).
        if (o_val && s_val && *o_val > 0 && *s_val > 0)
            penalty += opt.primary_weight * kPrimaryProximity * std::fabs(std::log(*s_val / *o_val));
        // The capacitor window allows up to 1.5x on purpose (more bulk C is
        // usually fine), but "recommended" means a drop-in, and a +42 % value is
        // one only where the capacitance is bulk or decoupling — never in a
        // timing, filter or compensation role, which the ranker cannot see.
        if (cat == "capacitor" && pv.verdict == PASS && o_val && s_val && *o_val > 0 &&
            *s_val / *o_val > kCapDropInMaxRatio) {
            demote();
            notes.push_back(signed_pct((*s_val / *o_val - 1.0) * 100.0) +
                            " more capacitance than the original — fine for bulk or decoupling, "
                            "not where the value sets a timing, filter or compensation point");
        }
        if (pv.verdict == WARN) {
            demote();
            // Say WHAT moved and by how much. A bare "value: warn" left the row
            // silent about the one parameter the part exists for, so a shifted
            // value read exactly like an exact-value match (ABT #497).
            if (o_val && s_val && *o_val > 0) {
                const double shift = (*s_val / *o_val - 1.0) * 100.0;
                std::string note = signed_pct(shift) + " shift from the original's nominal value";
                if (o_tol && *o_tol > 0 && std::fabs(shift) > *o_tol) {
                    note += "; the original is a " + plain_pct(*o_tol) +
                            " part, so this sits outside the band it guarantees";
                    // The stronger claim — that no unit of either part can meet the
                    // other's spec — is about the GUARANTEED bands, so it is tested
                    // on the bands themselves rather than on the nominal shift.
                    if (s_tol && *s_tol >= 0 &&
                        (*s_val * (1.0 + *s_tol / 100.0) < *o_val * (1.0 - *o_tol / 100.0) ||
                         *s_val * (1.0 - *s_tol / 100.0) > *o_val * (1.0 + *o_tol / 100.0)))
                        note += " and the two guaranteed windows do not overlap";
                    note += " — not a drop-in anywhere the value sets a ratio or a reference";
                }
                notes.push_back(note);
            }
            if (inductance_off_band) {
                const double m = *o_band_minus, pl = *o_band_plus;
                const std::string band =
                    std::fabs(m - pl) < 0.005 ? "+/-" + plain_pct(m)
                                              : "+" + plain_pct(pl) + " / -" + plain_pct(m);
                char buf[160];
                std::snprintf(buf, sizeof buf, "inductance %.4g H against the original's %.4g H",
                              *s_val, *o_val);
                notes.push_back(std::string(buf) + " (" +
                                signed_pct((*s_val / *o_val - 1.0) * 100.0) +
                                "): outside the original's stated " + band +
                                " tolerance — a different inductance value, ranked after the "
                                "in-tolerance candidates; check the circuit tolerates it");
            }
            if (capacitance_off_band) {
                const double m = *cap_band_minus, pl = *cap_band_plus;
                const std::string band =
                    std::fabs(m - pl) < 0.005 ? "+/-" + plain_pct(m)
                                              : "+" + plain_pct(pl) + " / -" + plain_pct(m);
                char buf[160];
                std::snprintf(buf, sizeof buf, "capacitance %.4g F against the original's %.4g F",
                              *s_val, *o_val);
                notes.push_back(std::string(buf) + " (" +
                                signed_pct((*s_val / *o_val - 1.0) * 100.0) +
                                "): outside the original's stated " + band +
                                " tolerance — a different capacitance value, ranked after the "
                                "in-tolerance candidates; check the circuit tolerates it");
            }
            if (value_differs)
                notes.push_back("nominal resistance differs: " + ohms(*o_val) + " -> " +
                                ohms(*s_val) + " (" + signed_pct((*s_val / *o_val - 1.0) * 100.0) +
                                ") — a different value, an alternative only");
        }
    }

    // ── construction family (hard) ───────────────────────────────────────────
    // A parametric match across families is not a substitution: a 10 uF ceramic
    // and a 10 uF tantalum share every catalogue column and have different
    // failure modes, ESR, derating and bias behaviour.
    if (cat == "capacitor") {
        std::string conflict = cap_family_conflict(cap_family(str(original, "technology")),
                                                   cap_family(str(cand, "technology")));
        // A family of unstated sub-type ("ceramic" with no class) still has a
        // construction: ceramic against film or electrolytic is a different part
        // whatever the class. Compare the coarse groups when the fine families
        // could not be told apart.
        if (conflict.empty()) {
            const std::string og = cap_group(str(original, "technology")),
                              sg = cap_group(str(cand, "technology"));
            if (!og.empty() && !sg.empty() && og != sg)
                conflict = og + " replaced by " + sg + ": a different capacitor construction";
        }
        if (!conflict.empty()) {
            params.push_back({{"name", "family"}, {"verdict", FAIL}});
            notes.push_back(conflict);
            return reject("different capacitor construction family");
        }
    }
    // ── line-safety approval class (IEC 60384-14 X1/X2/X3, Y1/Y2/Y3/Y4) ──────
    // A capacitor across the mains is chosen for its APPROVAL, and the approval is
    // not a number the ranker can trade against another number: X1 -> X2 halves the
    // impulse the part must survive in the place where a shorted capacitor starts a
    // fire. So a declared downgrade is a rejection, not a penalty.
    //
    // The gate is self-selecting on the ORIGINAL: it engages only when the original's
    // own series names a class, which is the only case where a safety approval is a
    // stated requirement. An ordinary 100 nF X7R chip never reaches it.
    //
    // The third outcome is the honest one and the common one. The catalogue has no
    // safety-class field (ABT #677) and the class is readable only from a series
    // name, so a substitute whose series says nothing cannot be checked. That is
    // UNVERIFIED — nothing was compared — and it carries the disqualifying
    // consequence rather than a verdict: demoted out of 'recommended', the FAIL's
    // penalty, the grade capped at major_review, and a note saying which approval
    // went unchecked. Silence used to read as agreement: the six KEMET R46 offered
    // for a WIMA MKP-X1 R were 'recommended' / 'drop_in' / 'upgrade' with voltage
    // "pass" and notes null (ABT #557).
    //
    // v20 (Qarlos 2026-11-09, seed 20784754): CAS now has the field — part.safetyClass.class —
    // and the spec carries it as `safety_class`. Vishay's F339MX252231KIM2T0 states x2 there,
    // 310 VAC, and no series name, so this gate never engaged and non-safety DC film parts came
    // back 'recommended'. Each side's class is read from the stated field when the record has
    // one, else from its series name as before. Rule (owner decision to confirm):
    //   * the original states no class (or states "none")  -> nothing is compared;
    //   * same or stricter class on the original's axis     -> pass (X1 covers X2, Y1 covers
    //     Y2; X never covers Y and Y is not read as covering X);
    //   * a lower class on that axis                        -> FAIL, rejected (as before);
    //   * the substitute's record STATES its class and it holds nothing on that axis (a stated
    //     Y2 or "none" against an X2 original)              -> FAIL, rejected like an identity
    //     mismatch: the record says what the part is certified as;
    //   * the substitute states no class anywhere           -> UNVERIFIED with the FAIL's
    //     consequence (demoted, FAIL penalty, capped at major_review), as before. Rejecting
    //     this case outright is an open decision for the owner: it would overturn the ABT #557
    //     rule that an unstated class is an absence of evidence, not a mismatch.
    // The AC rating is part of the approval (IEC 60384-14 grants a class AT a rated AC
    // voltage), so for a class-stating original it is compared too: a substitute AC rating
    // below the original's is a FAIL and rejects; an unstated one is UNVERIFIED with the same
    // consequence as an unverified class. Equal or higher passes. The DC "voltage" row above
    // is not that rating and is never read as it.
    if (cat == "capacitor") {
        const std::string o_series = str(original, "family");
        const std::string o_token = str(original, "safety_class");
        const SafetyClass o_class =
            o_token.empty() ? safety_class(o_series) : safety_class_from_token(o_token);
        if (o_class.any()) {
            const std::string s_series = str(cand, "family");
            const std::string s_token = str(cand, "safety_class");
            const bool s_stated = !s_token.empty();
            const SafetyClass s_class =
                s_stated ? safety_class_from_token(s_token) : safety_class(s_series);
            struct AxisPair {
                char axis;
                std::optional<int> o, s;
            };
            const AxisPair axes[] = {{'X', o_class.x, s_class.x}, {'Y', o_class.y, s_class.y}};
            bool downgraded = false, unverified = false;
            for (const AxisPair& a : axes) {
                if (!a.o) continue;  // the original claims nothing on this axis
                if (!a.s && s_stated) {
                    downgraded = true;
                    notes.push_back(safety_class_not_held(a.axis, *a.o, s_token));
                } else if (!a.s) {
                    unverified = true;
                    notes.push_back(safety_class_unverified(a.axis, *a.o, s_series));
                } else if (*a.s < *a.o) {
                    downgraded = true;
                    notes.push_back(safety_class_downgrade(a.axis, *a.o, *a.s));
                }
            }
            params.push_back({{"name", "safety_class"},
                              {"verdict", downgraded ? FAIL : (unverified ? UNVERIFIED : PASS)}});
            if (downgraded) return reject("line-safety approval class downgrade");
            const auto o_ac = num(original, "voltage_ac"), s_ac = num(cand, "voltage_ac");
            bool ac_unverified = false;
            if (o_ac) {
                char buf[64];
                if (s_ac && *s_ac < *o_ac) {
                    params.push_back({{"name", "voltage_ac"}, {"verdict", FAIL}});
                    std::snprintf(buf, sizeof buf, "%g V AC vs the original's %g V AC", *s_ac,
                                  *o_ac);
                    notes.push_back(std::string("AC rating below the original's: ") + buf +
                                    " — the safety class is granted at its rated AC voltage, "
                                    "so the substitute is not approved for the original's mains");
                    return reject("AC voltage rating below a safety-class original's");
                }
                if (s_ac) {
                    params.push_back({{"name", "voltage_ac"}, {"verdict", PASS}});
                } else {
                    params.push_back({{"name", "voltage_ac"}, {"verdict", UNVERIFIED}});
                    ac_unverified = true;
                    std::snprintf(buf, sizeof buf, "%g V AC", *o_ac);
                    notes.push_back(std::string("the original is approved at ") + buf +
                                    "; the substitute's record states no AC rating, so its mains "
                                    "rating could not be compared (its DC rating is not that "
                                    "rating) — confirm it on the datasheet");
                }
            }
            if (unverified || ac_unverified) {
                demote();
                penalty += opt.gate_weight * kVerdictFailPenalty;
                missing_required_data = true;
            }
        } else if (const auto o_ac = num(original, "voltage_ac")) {
            // An original with no safety class that still states an AC rating (a film
            // capacitor rated 500 V DC / 250 V AC): the AC rating is a rating of its own, and
            // the DC one is not it. Qarlos r28 seed 33352234: Vishay MKP1847H61525JP4 (250 V
            // AC) took WIMA DC-LINK parts whose records state no AC rating, with no AC verdict
            // or note. Rule (owner 2026-11-14):
            //   * both stated -> compared like the rated voltage (the capacitor "voltage"
            //     critical rating: HigherBetter, WARN below 0.95x, FAIL below 0.9x rejects);
            //   * the substitute states none -> UNVERIFIED with a note, demoted and capped at
            //     minor_review like a missing primary rating (owner 2026-11-14), no penalty;
            //   * the original states none -> nothing is compared.
            const auto s_ac = num(cand, "voltage_ac");
            char buf[160];
            if (!s_ac) {
                params.push_back({{"name", "voltage_ac"}, {"verdict", UNVERIFIED}});
                ac_rating_unverified = true;
                demote();
                std::snprintf(buf, sizeof buf, "%g V AC", *o_ac);
                notes.push_back(std::string("the original is rated ") + buf +
                                "; the substitute's record states no AC rating, so its AC "
                                "rating could not be compared (its DC rating is not that "
                                "rating) — confirm it on the datasheet before an AC duty");
            } else {
                const auto v =
                    score_directional(o_ac, s_ac, Mode::HigherBetter, 0.95, 0.9);
                params.push_back({{"name", "voltage_ac"}, {"verdict", v.verdict}});
                if (v.verdict == FAIL || v.verdict == WARN) {
                    std::snprintf(buf, sizeof buf, "%g V AC against the original's %g V AC",
                                  *s_ac, *o_ac);
                    notes.push_back(std::string("AC rating below the original's: ") + buf +
                                    " — less margin to the circuit's AC voltage; confirm the "
                                    "application's worst-case AC voltage is covered");
                    if (v.verdict == FAIL)
                        return reject("AC voltage rating falls far below the original's");
                    demote();
                    penalty += opt.gate_weight * v.penalty;
                }
            }
        }
    }
    // A chip resistor ARRAY is not a discrete resistor: same body outline, a
    // different land pattern, and a rating that is per element rather than per
    // package. Surfaced rather than rejected — N discretes DO replace one array
    // once the board changes — so it lands as a FAIL on the device class, which
    // caps the grade at major_review and sinks the candidate below any genuine
    // array-for-array match.
    bool resistor_class_conflict = false;
    bool resistor_connection_unverified = false;
    bool resistor_count_one_sided = false;  // owner 2026-11-14: one-sided count caps minor_review
    if (cat == "resistor") {
        // Network structure (owner 2026-11-13, AA2; RAS 3ba21f6 electrical.numberOfElements /
        // elements[] / part.internalConnection). A STATED element count declares a network
        // and wins over the count a "0402x4" case string names; a record stating both, and
        // disagreeing, is a data defect: it is reported and its count is not chosen.
        struct NetCount {
            std::optional<double> n;  // the count, when the record settles one
            std::string defect;       // non-empty: the record contradicts itself
            bool stated = false;      // electrical.numberOfElements / elements[] on record
        };
        auto count_of = [](const json& p) {
            NetCount c;
            const auto stated = num(p, "number_of_elements");
            const auto from_case = case_resistor_element_count(str(p, "case_code"));
            c.stated = stated.has_value();
            if (stated && from_case && *stated != static_cast<double>(*from_case)) {
                char b[160];
                std::snprintf(b, sizeof b, "states %g elements but its case '%s' names %d",
                              *stated, str(p, "case_code").c_str(), *from_case);
                c.defect = b;
            } else if (stated) {
                c.n = stated;
            } else if (from_case) {
                c.n = static_cast<double>(*from_case);
            }
            return c;
        };
        const NetCount oc = count_of(original), sc = count_of(cand);
        const std::string conflict = resistor_network_conflict(
            oc.stated || declares_resistor_network(str(original, "family"), str(original, "mpn"),
                                                   str(original, "case_code")),
            sc.stated || declares_resistor_network(str(cand, "family"), str(cand, "mpn"),
                                                   str(cand, "case_code")));
        if (!conflict.empty()) {
            params.push_back({{"name", "configuration"}, {"verdict", FAIL}});
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            notes.push_back(conflict);
            resistor_class_conflict = true;
        } else {
            // Both sides are networks (or neither declares one). Element count is an
            // identity: different -> rejected; one side unknown -> UNVERIFIED capped at
            // minor_review (owner 2026-11-14); a record whose stated count contradicts its own
            // case is a data defect and keeps the network cap (major_review).
            auto n_str = [](double v) {
                char b[32];
                std::snprintf(b, sizeof b, "%g", v);
                return std::string(b);
            };
            auto count_unverified = [&](const std::string& note, bool defect) {
                params.push_back({{"name", "number_of_elements"}, {"verdict", UNVERIFIED}});
                notes.push_back(note);
                demote();
                penalty += opt.gate_weight * kVerdictFailPenalty;
                if (defect)
                    missing_required_data = true;
                else
                    resistor_count_one_sided = true;
            };
            if (!oc.defect.empty() || !sc.defect.empty()) {
                std::string note = "data defect: ";
                if (!oc.defect.empty()) note += "the original's record " + oc.defect;
                if (!oc.defect.empty() && !sc.defect.empty()) note += "; ";
                if (!sc.defect.empty()) note += "the substitute's record " + sc.defect;
                count_unverified(note + " — the element count is not chosen between them; "
                                        "confirm it on the datasheet",
                                 true);
            } else if (oc.n && sc.n) {
                if (*oc.n == *sc.n) {
                    params.push_back({{"name", "number_of_elements"}, {"verdict", PASS}});
                } else {
                    params.push_back({{"name", "number_of_elements"}, {"verdict", FAIL}});
                    notes.push_back("number of resistor elements differs: the original holds " +
                                    n_str(*oc.n) + ", the substitute " + n_str(*sc.n) +
                                    " — a different land pattern and circuit, not a substitute");
                    return reject("a different number of resistor elements");
                }
            } else if (oc.n || sc.n) {
                count_unverified(
                    oc.n ? "the original's record states " + n_str(*oc.n) +
                               " resistor elements and the substitute's states no count — "
                               "confirm the substitute's element count and pinout from its "
                               "datasheet"
                         : "the substitute's record states " + n_str(*sc.n) +
                               " resistor elements and the original's states no count — "
                               "confirm the original's element count and pinout match",
                    false);
            }
            // Internal connection: the MOSFET / diode internal_connection rule, field for
            // field — both stated and different -> rejected; one-sided -> UNVERIFIED, capped
            // below drop_in (minor_review); neither -> silent.
            const std::string o = str(original, "internal_connection"),
                              s = str(cand, "internal_connection");
            if (!o.empty() && !s.empty()) {
                if (o == s) {
                    params.push_back({{"name", "internal_connection"}, {"verdict", PASS}});
                } else {
                    params.push_back({{"name", "internal_connection"}, {"verdict", FAIL}});
                    notes.push_back("internal connection differs: the original is '" + o +
                                    "', the substitute '" + s +
                                    "' — a different network circuit and pinout, not a "
                                    "substitute");
                    return reject("a different resistor network internal connection");
                }
            } else if (!o.empty() || !s.empty()) {
                params.push_back({{"name", "internal_connection"}, {"verdict", UNVERIFIED}});
                notes.push_back(o.empty()
                                    ? "the substitute's record states internal connection '" + s +
                                          "' and the original's states none — confirm the "
                                          "original's network circuit and pinout match"
                                    : "the original's record states internal connection '" + o +
                                          "' and the substitute's states none — confirm the "
                                          "substitute's network circuit and pinout from its "
                                          "datasheet");
                demote();
                penalty += opt.gate_weight * kVerdictWarnPenalty;
                resistor_connection_unverified = true;
            }
        }
    }
    // Anti-sulfuration is a CONSTRUCTION class, not a parameter. An ERJ-S and the
    // ordinary ERJ beside it share every catalogue column and both read
    // "thickFilm", so silence about it read as agreement and a plain chip came
    // back drop_in / recommended for an anti-sulfurated original (ABT #518). The
    // part still FITS — this is a reliability class, not a land pattern — so it
    // stays out of the footprint conflict above and caps the grade on its own.
    if (cat == "resistor" &&
        sulfur_resistant_by_design(str(original, "family"), str(original, "mpn"))) {
        const bool declared = declares_sulfur_resistance(str(cand, "family"), str(cand, "mpn"));
        params.push_back({{"name", "sulfur_resistance"}, {"verdict", declared ? PASS : FAIL}});
        if (!declared) {
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            notes.push_back(sulfur_resistance_conflict(str(cand, "family")));
        }
    }
    // ── resistive element and stated function (Qarlos 2026-11-08) ────────────
    // R, P and tolerance are the same columns on a thin film, a thick film, a metal film and
    // a wirewound, and on a current-sense shunt or an anti-surge chip and the general-purpose
    // chip beside it — so silence about the class read as agreement: ERJ6DQJ1R5V (current
    // sensing) took SR0805JR-471R5L as drop_in, SG731JTTD4R7M (anti pulse) took AA0603 chips
    // as drop_in. Only what the RECORDS state is used (RAS part.technology; the function the
    // extractor read from technology/family/series/description), never the MPN.
    //   * Technology class differs (both stated): WARN + note — a different TCR, noise,
    //     stability and pulse behaviour, never a drop-in; not counted as a direction (thick ->
    //     thin is better on most axes and worse on pulse load, so neither word is honest).
    //     Two exceptions, both because the enum value does not name an element: melf is a
    //     package, so it is not compared; and currentSenseShunt against a record whose
    //     function is ALSO current sensing (ERJ6DQ "Current Sensing Resistors Thick Film
    //     Type" against a thickFilm record whose series says current sensing) is the same
    //     class written two ways, so the function check below decides it alone.
    //   * The original states a function the substitute's record does not: FAIL + note,
    //     like the anti-sulfuration class above (major_review). A sense resistor's value is a
    //     measurement and a surge resistor's rating is its reason to exist; a substitute that
    //     does not claim either has not been shown to do the job, and a general-purpose chip
    //     under the original's surge fails open.
    if (cat == "resistor") {
        const std::string o_tech = str(original, "technology"), s_tech = str(cand, "technology");
        const bool both_sense = flag(original, "current_sense") && flag(cand, "current_sense");
        const bool comparable = !o_tech.empty() && !s_tech.empty() && o_tech != "melf" &&
                                s_tech != "melf" &&
                                !(both_sense && (o_tech == "currentSenseShunt" ||
                                                 s_tech == "currentSenseShunt"));
        if (comparable && o_tech != s_tech) {
            params.push_back({{"name", "technology"}, {"verdict", WARN}});
            demote();
            penalty += opt.gate_weight * kVerdictWarnPenalty;
            notes.push_back("resistor technology differs: " + o_tech + " -> " + s_tech +
                            " — a different construction class, with different TCR, noise, "
                            "long-term stability and pulse behaviour; not a drop-in where the "
                            "original's construction was chosen for them");
        }
        struct Function {
            const char* key;
            const char* what;
        };
        for (const Function& f : {Function{"current_sense", "a current-sense resistor"},
                                  Function{"pulse_withstanding", "a pulse/surge-withstanding "
                                                                 "(anti-surge) resistor"}}) {
            if (!flag(original, f.key)) continue;
            const bool kept = flag(cand, f.key);
            params.push_back({{"name", f.key}, {"verdict", kept ? PASS : FAIL}});
            if (kept) continue;
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            ++worse;  // a lost function is a regression, whatever the numbers say
            notes.push_back(std::string("the original is ") + f.what +
                            " by its record; the substitute's record states no such class (" +
                            (s_tech.empty() ? std::string("technology unstated") : s_tech) +
                            ") — confirm it from the datasheet before using it in that role");
        }
    }
    // A single-phase bridge rectifier is not a discrete diode: four dies on four
    // terminals (AC, AC, +, -), a Vf quoted per element against a path that runs
    // through two of them, and an If(AV) that is the bridge's OUTPUT current. It
    // takes the same treatment as the resistor array above — a FAIL on the device
    // class, which caps the grade at major_review — rather than a rejection, because
    // four discretes DO replace a bridge once the board changes (ABT #521).
    bool diode_class_conflict = false;
    if (cat == "diode") {
        const std::string conflict =
            diode_configuration_conflict(str(original, "case_code"), str(original, "mpn"),
                                         str(cand, "case_code"), str(cand, "mpn"));
        if (!conflict.empty()) {
            params.push_back({{"name", "configuration"}, {"verdict", FAIL}});
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            notes.push_back(conflict);
            diode_class_conflict = true;
        }
    }
    // Si / SiC / GaN differ in gate-drive requirements — a driver redesign, not
    // a drop-in, so it is surfaced loudly rather than scored away.
    if (cat == "mosfet" || cat == "igbt" || cat == "diode") {
        const std::string conflict =
            process_conflict(str(original, "technology"), str(cand, "technology"));
        if (!conflict.empty()) {
            params.push_back({{"name", "process"}, {"verdict", FAIL}});
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            notes.push_back(conflict);
        }
    }

    // A connector's PITCH is its land pattern — the record carries no body outline,
    // so nothing else settles the fit. Captured from the PARAM_SPECS verdict below
    // (one tolerance, stated once) and reported as the footprint verdict, because a
    // "footprint": null next to a known pitch mismatch reads as "we could not check
    // the fit" when it has just been checked and failed (ABT #485).
    bool connector_pitch_conflict = false;
    // Whether the pin field lines up (connector_pitch_fit above): it decides the pitch_mm
    // verdict whenever both records state a pitch. An alignment nobody can establish is
    // an unverified land pattern and caps the grade like one.
    const ConnectorPitchFit connector_pitch =
        cat == "connector" ? connector_pitch_fit(original, cand) : ConnectorPitchFit{};
    bool connector_pitch_unverified = false;
    // The other two halves of "does it mate": the finish on the separable surface and the
    // way the wire attaches. Both are stated by the records and both were declared absent
    // from the catalogue by the family caveat, so neither was ever compared (ABT #487).
    // Captured the same way, for the same reason — a bare "contact_plating: fail" does not
    // tell the engineer that gold is about to be mated to tin.
    bool connector_plating_conflict = false;
    // A connector identity (gender, wire termination, contact pitch, row count) stated by
    // one record and not the other (the one-sided identity block after the PARAM_SPECS
    // loop): unverified, and never a drop-in.
    bool connector_identity_unverified = false;
    // Every connector identity axis that exactly ONE record states (owner, 2026-11-12): the
    // capped one-sided identities above, the board mount (mount block), and the family-variant
    // identities (actuator type, shell size, coding, coupling type). Each keeps its own
    // single-gap treatment; THREE OR MORE together cap the grade at major_review (grade site).
    std::vector<std::string> connector_identity_gaps;
    // A zener's breakdown WINDOW, captured from the PARAM_SPECS verdict below for the
    // same reason: a bare "vz_tolerance_pct: unverified" beside "vrrm: pass" leaves the
    // engineer with no idea that the original guarantees 3.56-3.64 V and the substitute's
    // record guarantees nothing at all (ABT #488).
    bool diode_vz_band_regression = false;
    bool diode_vz_band_unverified = false;
    // An active oscillator's supply range, captured from the PARAM_SPECS verdicts below so
    // the note can state both ranges in volts: a bare "supply_min_V: fail" does not say that
    // a 3.3 V-only part was offered for a 2.5 V rail. It was never compared at all (the
    // spec builder did not pass the range), so a 3.3 V and a 1.8 V XO were graded
    // minor_review / upgrade for a 2.5 V EPSON original. (A range that does not cover the
    // original's is rejected inside the loop, owner 2026-11-13; this flag is the one-sided
    // unstated case.)
    bool timebase_supply_unverified = false;
    // The original states its supply current and the substitute does not (secondary axis:
    // note only, no cap).
    bool timebase_current_unverified = false;
    // A timing part's control-pin function stated on one side only: UNVERIFIED, noted, and
    // the grade capped below drop_in — the pin function is the part's pinout, a primary axis,
    // and drop_in asserts the same pinout (Qarlos 2026-11-09). A frequency tolerance /
    // stability the original states and the substitute does not is UNVERIFIED and noted
    // too, but it is a secondary axis: no demotion, no cap (owner, 2026-11-09).
    bool timebase_pin_unverified = false;
    // The footprint is UNVERIFIED because NEITHER record states a land: both name only the
    // same generic package string (generic_package_names_no_land) and no dimensions, or
    // neither states any land dimension nor any package / case code at all. Reported, not
    // capped (owner, 2026-11-12: missing data on both sides is not capped).
    bool footprint_unstated_both = false;
    // A connector orientation that could not be established (see the orientation block
    // after the PARAM_SPECS loop): not a drop-in.
    bool connector_orientation_unverified = false;
    // Pluggable vs fixed terminal block stated on one side only (see the pluggable block).
    bool connector_pluggable_unverified = false;
    // A passive resonator (quartz crystal, ceramic resonator) has no supply: it is driven
    // by the MCU's oscillator stage. The supply keys mean nothing there and are not
    // compared even if a record happened to carry one.
    // The technology pair, for the two owner-approved crossings (timing_technology_cross):
    // a MEMS <-> quartz XO pair is capped at major_review after the grade is computed.
    const TimingTechCross timebase_tech_cross =
        cat == "timeBase" ? timing_technology_cross(str(original, "technology"),
                                                    str(cand, "technology"))
                          : TimingTechCross::Same;
    bool timebase_tech_major = false;
    const ProgrammableFrequencyFit prog_freq =
        cat == "timeBase" ? programmable_frequency_fit(original, cand) : ProgrammableFrequencyFit{};
    // Set when prog_freq admits a blank for a fixed-frequency original (M0): caps minor_review.
    bool timebase_prog_order = false;
    const bool timebase_passive =
        cat == "timeBase" && (str(original, "technology") == "quartzCrystal" ||
                              str(original, "technology") == "ceramicResonator");
    // A resistor's power is compared at the original's rating temperature (see
    // power_at_rating_temperature). Computed once, consumed by the power_rating spec below.
    const PowerAtTemperature power_t =
        cat == "resistor" ? power_at_rating_temperature(original, cand) : PowerAtTemperature{};
    // A resistor's working voltage is compared as min(maxVoltage, sqrt(P x R)) on both sides
    // when both records state all three (resistor_working_voltage).
    const WorkingVoltage work_v =
        cat == "resistor" ? resistor_working_voltage(original, cand) : WorkingVoltage{};
    // ── capacitor ripple-current basis ──────────────────────────────────────
    // A ripple rating is stated AT a frequency, and the same can is rated very differently at
    // 100 Hz and at 20 kHz: 380LX151M450K022 is 1.2 A at 120 Hz and 1.7 A at 20 kHz. Its
    // 1.7 A @ 20 kHz was failed against a candidate's 1.36 A @ 100 Hz with no caveat (Qarlos
    // 2026-11-09, seed 20365838). Rule — compare like with like, the treatment the resistor
    // power basis gets (power_at_rating_temperature):
    //   * both records state the frequency and it is the same -> compared as stated;
    //   * both state it and it DIFFERS -> UNVERIFIED, with a note giving both values and both
    //     frequencies — with ONE exception, the owner's rule (2026-11-09): 100 Hz and 120 Hz
    //     are the same basis, the low-frequency ripple rating quoted at twice the 50 Hz or the
    //     60 Hz mains frequency (the region a datasheet names, not a property of the part), so
    //     a 100 Hz and a 120 Hz rating are compared directly. Rubycon VXH 1.63 A @ 120 Hz
    //     against Vishay 1.75 A @ 100 Hz was left unverified (Qarlos round 4). Every other
    //     difference, however small, stays UNVERIFIED;
    //   * the original states it, the substitute does not -> UNVERIFIED, noted;
    //   * the original states none -> compared as stated, exactly as before (no basis exists
    //     for any candidate, so none is skewed against another).
    // An UNVERIFIED ripple basis is demoted and capped at minor_review (drop_in asserts no
    // current rating regressed, which nobody has shown) and counts in neither direction.
    // No conversion is attempted: never a "typical" frequency multiplier, and not the record's own
    // multiplier table either (a candidate's table rarely reaches the original's frequency — 198
    // PHR-SI's stops at 10 kHz — and an extrapolated multiplier is a guess).
    //
    // ESR is stated AT a frequency in exactly the same way, and gets exactly the same rule
    // through the same check (Qarlos 2026-11-09, seed 20365838): 380LX151M450K022 states
    // 0.44 ohm at 20 kHz, MAL219857151E3 0.637 ohm at 100 Hz, and the ESR verdict read
    // 'warn' and counted as a downgrade although the original's own curve gives 1.1 ohm at
    // 120 Hz. An ESR basis that is not common is UNVERIFIED (the esr_basis row), the ESR
    // verdict with it and it counts in neither direction; ESR is a secondary axis, so it no
    // longer demotes or caps the grade (owner, 2026-11-09). The decade-wide tolerance the
    // old ESR check allowed (any two frequencies within 10x compared as bare numbers) is gone: 100
    // Hz and 120 Hz are the only two different frequencies treated as one basis, for ESR as for
    // ripple.
    // `primary`: ripple current is a current rating (primary — demoted and capped); ESR is a
    // secondary electrical, so an unverified ESR basis keeps its verdict, note and lack of
    // direction but neither demotes nor caps (owner, 2026-11-09; table at the grade site).
    auto basis_unverified = [&](const char* value_key, const char* freq_key, const char* what,
                                const char* unit, bool primary) {
        const auto o_v = num(original, value_key), s_v = num(cand, value_key);
        const auto o_f = num(original, freq_key), s_f = num(cand, freq_key);
        if (!o_v || !s_v || !o_f) return false;
        char buf[320];
        auto same_f = [](double a, double b) { return std::fabs(a - b) <= 1e-9 * std::max(a, b); };
        auto mains_basis = [&](double f) { return same_f(f, 100.0) || same_f(f, 120.0); };
        const bool same_basis =
            s_f && (same_f(*o_f, *s_f) || (mains_basis(*o_f) && mains_basis(*s_f)));
        if (same_basis) return false;
        if (s_f)
            std::snprintf(buf, sizeof buf,
                          "%s quoted at different frequencies (original %g %s at %g Hz, "
                          "substitute %g %s at %g Hz) — not directly comparable; confirm the "
                          "substitute's %s at the original's frequency from its datasheet",
                          what, *o_v, unit, *o_f, *s_v, unit, *s_f, what);
        else
            std::snprintf(buf, sizeof buf,
                          "the original's %s is %g %s at %g Hz; the substitute's record states "
                          "%g %s at no frequency — not directly comparable; confirm its %s at "
                          "the original's frequency from the datasheet",
                          what, *o_v, unit, *o_f, *s_v, unit, what);
        notes.push_back(buf);
        if (primary) demote();
        return true;
    };
    // ── ripple rating moved to a common basis on a record's own table (owner 2026-11-14) ──
    // When the two ripple ratings are stated at different frequencies and/or temperatures, a
    // record's own ripple-current table (ripple_f_curve: frequency, read log-log; ripple_t_curve:
    // temperature, read linearly — both as Views.cpp read_ripple_table keeps them: labelled
    // ripple-current tables, or unlabelled ones anchored at the rating's own basis, no
    // case-temperature table, no table stated under another condition) moves a rating to the
    // other side's basis: I x y(target) / y(stated), only inside the table's measured range,
    // never extrapolated. Per differing axis, the substitute is moved to the original's basis
    // first; when its table does not cover both points, the original is moved to the
    // substitute's. The two axes are independent factors (each table is stated at the rating's
    // own basis on the other axis). Every differing axis must be resolved; otherwise nothing is
    // converted and the basis rules below (ripple_basis_unverified /
    // ripple_temp_basis_unverified) apply exactly as before. A side stating no basis on a
    // differing axis cannot be moved (there is no point to read its table from).
    struct RippleConv {
        double o = 0, s = 0;
        std::string note;
    };
    const std::optional<RippleConv> ripple_conv = [&]() -> std::optional<RippleConv> {
        if (cat != "capacitor") return std::nullopt;
        const auto o_v = num(original, "ripple_current"), s_v = num(cand, "ripple_current");
        if (!o_v || !s_v) return std::nullopt;
        const auto o_f = num(original, "ripple_current_frequency"),
                   s_f = num(cand, "ripple_current_frequency");
        const auto o_t = num(original, "ripple_current_temperature"),
                   s_t = num(cand, "ripple_current_temperature");
        auto same = [](double a, double b) {
            return std::fabs(a - b) <= 1e-9 * std::max({std::fabs(a), std::fabs(b), 1.0});
        };
        auto mains = [&](double f) { return same(f, 100.0) || same(f, 120.0); };
        const bool f_differs = o_f && !(s_f && (same(*o_f, *s_f) || (mains(*o_f) && mains(*s_f))));
        const bool t_differs = o_t && !(s_t && same(*o_t, *s_t));
        if (!f_differs && !t_differs) return std::nullopt;
        RippleConv out;
        double mo = 1.0, ms = 1.0;
        std::string how, basis;
        auto fmt = [](double v, const char* unit) {
            char b[48];
            std::snprintf(b, sizeof b, "%g %s", v, unit);
            return std::string(b);
        };
        // One axis; false when it cannot be resolved.
        auto axis = [&](bool differs, const std::optional<double>& ob,
                        const std::optional<double>& sb, const char* key, Interp interp,
                        const char* table, const char* unit) -> bool {
            if (!differs) return true;
            if (!sb) return false;
            char b[256];
            if (const auto st = curve_from_spec(cand, key)) {
                const auto a = curve_at(st->curve, *ob, interp), z = curve_at(st->curve, *sb, interp);
                if (a && z) {
                    ms *= *a / *z;
                    std::snprintf(b, sizeof b, "%sthe substitute's own %s table x%.4g (%s -> %s)",
                                  how.empty() ? "" : "; ", table, *a / *z, fmt(*sb, unit).c_str(),
                                  fmt(*ob, unit).c_str());
                    how += b;
                    basis += (basis.empty() ? "" : ", ") + fmt(*ob, unit);
                    return true;
                }
            }
            if (const auto ot = curve_from_spec(original, key)) {
                const auto a = curve_at(ot->curve, *sb, interp), z = curve_at(ot->curve, *ob, interp);
                if (a && z) {
                    mo *= *a / *z;
                    std::snprintf(b, sizeof b, "%sthe original's own %s table x%.4g (%s -> %s)",
                                  how.empty() ? "" : "; ", table, *a / *z, fmt(*ob, unit).c_str(),
                                  fmt(*sb, unit).c_str());
                    how += b;
                    basis += (basis.empty() ? "" : ", ") + fmt(*sb, unit);
                    return true;
                }
            }
            return false;
        };
        if (!axis(f_differs, o_f, s_f, "ripple_f_curve", Interp::LogLog,
                  "ripple-current frequency", "Hz"))
            return std::nullopt;
        if (!axis(t_differs, o_t, s_t, "ripple_t_curve", Interp::Linear,
                  "ripple-current temperature", "degC"))
            return std::nullopt;
        out.o = *o_v * mo;
        out.s = *s_v * ms;
        char buf[640];
        std::snprintf(buf, sizeof buf,
                      "ripple current compared on a common basis (%s): substitute %.4g A "
                      "(stated %g A) vs original %.4g A (stated %g A), moved by %s "
                      "(frequency log-log, temperature linear, inside each table's range)",
                      basis.c_str(), out.s, *s_v, out.o, *o_v, how.c_str());
        out.note = buf;
        return out;
    }();
    const bool ripple_basis_unverified =
        cat == "capacitor" && !ripple_conv &&
        basis_unverified("ripple_current", "ripple_current_frequency", "ripple current", "A",
                         /*primary=*/true);
    // The ripple rating's TEMPERATURE basis (v22, Qarlos r26 32304944): an 85 degC rating
    // and a 105 degC rating of the same can are not the same number — 600MXH270MEFCSN35X50
    // (1.67 A @ 105 degC) passed against B43541A8277M060 (2.25 A @ 85 degC). The Cj / Coss
    // basis rule: both state a ripple value and the original states its temperature -> a
    // substitute at the same temperature is compared as stated; one at a different
    // temperature or at none is UNVERIFIED, noted and counts in neither direction. Owner
    // 2026-11-13, Z3: it is also demoted and capped at minor_review, as the ripple FREQUENCY
    // basis rule above is (it was note-only, the Cj / Coss treatment). The original stating no
    // temperature -> compared as stated. No conversion between temperatures is attempted.
    const bool ripple_temp_basis_unverified = [&] {
        if (cat != "capacitor" || ripple_conv) return false;  // resolved on a table (above)
        const auto o_v = num(original, "ripple_current"), s_v = num(cand, "ripple_current");
        const auto o_t = num(original, "ripple_current_temperature"),
                   s_t = num(cand, "ripple_current_temperature");
        if (!o_v || !s_v || !o_t) return false;
        if (s_t && std::fabs(*o_t - *s_t) <= 1e-9 * std::max(std::fabs(*o_t), 1.0)) return false;
        char buf[320];
        if (s_t)
            std::snprintf(buf, sizeof buf,
                          "ripple current quoted at different temperatures (original %g A at "
                          "%s degC, substitute %g A at %s degC) — not directly comparable; "
                          "confirm the substitute's ripple current at the original's "
                          "temperature from its datasheet",
                          *o_v, degc(*o_t).c_str(), *s_v, degc(*s_t).c_str());
        else
            std::snprintf(buf, sizeof buf,
                          "the original's ripple current is %g A at %s degC; the substitute's "
                          "record states %g A at no temperature — not directly comparable; "
                          "confirm its ripple current at the original's temperature from the "
                          "datasheet",
                          *o_v, degc(*o_t).c_str(), *s_v);
        notes.push_back(buf);
        demote();
        return true;
    }();
    // ── capacitor curves (owner 2026-11-14: "compare ALL existing graphs") ──────
    // A curve's verdict replaces the scalar comparison of its quantity (curve_override, read
    // in the parameter loop below): the outcome, and the two values it was decided on (they
    // carry the direction and the over-dimensioning tie-breaker like stated scalars do).
    struct CurveOverride {
        ParamOutcome outcome{UNVERIFIED};
        std::optional<double> o, s;
    };
    std::map<std::string, CurveOverride> curve_override;
    auto spec_for = [&](const char* key) -> const ParamSpec& {
        for (const ParamSpec& p : params_for(cat))
            if (p.key == key) return p;
        throw std::logic_error(std::string("no ParamSpec '") + key + "' for " + cat);
    };
    // The ripple ratings moved to a common basis on a record's own table (ripple_conv above):
    // compared there with the ripple spec, the converted values carrying the direction.
    if (ripple_conv) {
        notes.push_back(ripple_conv->note);
        curve_override["ripple_current"] = {
            compare_numeric(spec_for("ripple_current"), ripple_conv->o, ripple_conv->s),
            ripple_conv->o, ripple_conv->s};
    }
    // A magnetic's rated current at a common temperature rise (owner 2026-11-14,
    // magcurve::rated_at_common_rise): with rated-current points on both records it replaces
    // the largest-current scalar comparison, the currents at the deciding rise carrying the
    // direction; different ambients or no common rise -> UNVERIFIED + note (no cap).
    if (cat == "magnetic" || cat == "chipBead") {
        const auto rb =
            magcurve::rated_at_common_rise(original, cand, spec_for("rated_current"));
        if (rb.applies) {
            if (!rb.note.empty()) notes.push_back(rb.note);
            curve_override["rated_current"] = {rb.outcome, rb.orig_at, rb.sub_at};
        }
    }
    auto hz = [](double f) {
        char b[32];
        std::snprintf(b, sizeof b, "%g Hz", f);
        return std::string(b);
    };
    // ── the useful bands of a capacitor's impedance-type curves (owner 2026-11-14) ─────
    // |Z|(f) and ESR(f) are judged only BELOW the original's self-resonance: from its stated
    // rating frequency (its ESR test frequency, esrFrequency, when the record states one;
    // 1 MHz when it does not — an ESR Kelvin itself read off the curve at 100 kHz,
    // esr_from_curve, is not a stated frequency) up to the original's self-resonance: the
    // minimum of its |Z| curve. When that minimum is the curve's last sample, |Z| is still
    // falling where the measurement stops and the resonance lies above it, so the band ends at
    // the curve's end (all that is known to be below resonance). No |Z| curve on the original
    // -> its self-resonance is unknown: the ESR curve is then graded from the band start up to
    // the end of the common range, with a note saying so (owner decision 2026-11-14, 1).
    // ESL(f) is extracted from the INDUCTIVE region, so it gets its own band: from 3x the
    // original's self-resonance upward to the end of the common range (owner decisions
    // 2026-11-14, 2, and 2026-11-14 follow-up: at k = f/f_res the reactance over omega reads
    // L(1 - 1/k^2), so right above resonance the extracted ESL is far below the true L; from
    // k = 3 it is within about 11 %). Self-resonance unknown (no |Z| curve, or |Z| still falling at the end
    // of its curve so the resonance lies somewhere above the measured range) -> no ESL band,
    // ESL is not graded (UNVERIFIED + note). (The catalogue states no SRF field for
    // capacitors, and every record with a C(f) curve also carries |Z|, so |Z| is the source.)
    // Points outside a band are not graded; the note names the band.
    struct CurveBand {
        std::optional<std::pair<double, double>> band;
        std::string why;  // the band in words, or why there is none
    };
    struct UsefulBands {
        CurveBand below;  // |Z| and ESR: rating frequency .. self-resonance
        CurveBand esl;    // ESL: self-resonance .. end of the common range
    };
    const UsefulBands useful_band = [&]() -> UsefulBands {
        UsefulBands u;
        if (cat != "capacitor") return u;
        char buf[320];
        const auto o_f = num(original, "esr_frequency");
        const auto derived = original.find("esr_from_curve");
        const bool from_curve =
            derived != original.end() && derived->is_boolean() && derived->get<bool>();
        const bool stated_f = o_f && !from_curve;
        const double lo = stated_f ? *o_f : 1e6;
        const std::string lo_what = stated_f ? "the original's rating (ESR test) frequency " + hz(lo)
                                             : std::string("1 MHz (the original states no rating frequency)");
        const auto oz = curve_from_spec(original, "impedance_curve");
        if (!oz) {
            // Open-topped: compare_curves clips the band to the two curves' common range.
            u.below.band = std::make_pair(lo, HUGE_VAL);
            u.below.why = "useful band from " + lo_what +
                          " up to the end of the common range; the original carries no |Z| "
                          "curve, so its self-resonance is unknown and the band's top is not "
                          "on record; points below the band are not graded";
            u.esl.why = "the original carries no |Z| curve, so its self-resonance (where the "
                        "inductive region the ESL is extracted from begins) is unknown";
            return u;
        }
        const auto& zy = oz->curve.y;
        const std::size_t imin =
            static_cast<std::size_t>(std::min_element(zy.begin(), zy.end()) - zy.begin());
        const double hi = oz->curve.x[imin];
        const bool at_end = imin + 1 == zy.size();
        const std::string hi_what =
            at_end ? "the end of the original's |Z| curve at " + hz(hi) +
                         " (|Z| still falling there; its self-resonance lies above)"
                   : "the original's self-resonance (|Z| minimum) at " + hz(hi);
        if (at_end) {
            u.esl.why = "the original's |Z| is still falling at the end of its curve at " +
                        hz(hi) +
                        ", so its self-resonance (where the inductive region the ESL is "
                        "extracted from begins) lies above the measured range and is unknown";
        } else {
            u.esl.band = std::make_pair(kEslBandOfResonance * hi, HUGE_VAL);
            u.esl.why = "ESL band from 3x the original's self-resonance (|Z| minimum at " +
                        hz(hi) + "), " + hz(kEslBandOfResonance * hi) +
                        ", up to the end of the common range (ESL is extracted from the "
                        "inductive region; just above resonance X/w reads L(1 - 1/k^2), within "
                        "about 11 % from 3x); points below it are not graded";
        }
        if (lo > hi) {
            std::snprintf(buf, sizeof buf,
                          "no useful band: %s lies below %s",
                          hi_what.c_str(), lo_what.c_str());
            u.below.why = buf;
            return u;
        }
        u.below.band = std::make_pair(lo, hi);
        u.below.why = "useful band from " + lo_what + " to " + hi_what +
                      "; points outside it are not graded";
        return u;
    }();
    // ESR (a): both records carry an ESR-vs-frequency curve -> compared on the overlap of the
    // two measured ranges inside the original's useful band, log-log (ESR curves are plotted on
    // log axes), with the ESR spec's bands at each point; the worst point decides. Owner
    // 2026-11-14: the curve verdict does NOT replace the scalar ESR verdict — it is its own
    // row (esr_curve), booked like every curve (a WARN demotes and costs its depth-scaled
    // warn, a FAIL demotes and costs the FAIL penalty), and the scalar ESR row is judged on
    // its own; both penalise. Different stated conditions, no band or no overlap -> an
    // UNVERIFIED esr_curve row and a note (no penalty, no cap).
    // ESR (b), the scalar: when the two scalars are quoted at different frequencies, a curve
    // resolves the basis: the substitute's ESR is read from its curve at the original's
    // frequency (or the original's from its curve at the substitute's), inside the measured
    // range only.
    if (cat == "capacitor") {
        const auto oc = curve_from_spec(original, "esr_curve"),
                   sc = curve_from_spec(cand, "esr_curve");
        const ParamSpec& esr_spec = spec_for("esr");
        if (oc && sc) {
            const std::string cond = conditions_differ(*oc, *sc);
            auto row = [&](const char* v) { params.push_back({{"name", "esr_curve"}, {"verdict", v}}); };
            if (!cond.empty()) row(UNVERIFIED);
            else if (!useful_band.below.band) row(UNVERIFIED);
            if (!cond.empty()) {
                notes.push_back("ESR curves measured under different conditions (" + cond +
                                ") — the curves were not compared");
            } else if (!useful_band.below.band) {
                notes.push_back("ESR curves not compared: " + useful_band.below.why);
            } else {
                const CurveVerdict v = compare_curves(oc->curve, sc->curve, esr_spec,
                                                      Interp::LogLog, useful_band.below.band);
                row(v.verdict);
                if (v.verdict == std::string(UNVERIFIED)) {
                    notes.push_back("ESR curves do not meet inside the useful band (original " +
                                    hz(oc->curve.x.front()) + " to " + hz(oc->curve.x.back()) +
                                    ", substitute " + hz(sc->curve.x.front()) + " to " +
                                    hz(sc->curve.x.back()) + "; " + useful_band.below.why +
                                    ") — the curves were not compared");
                } else {
                    char buf[640];
                    std::snprintf(buf, sizeof buf,
                                  "ESR compared on both curves from %g to %g Hz (%zu points, "
                                  "log-log): %s at %g Hz, substitute %.4g ohm vs original "
                                  "%.4g ohm (%s)",
                                  v.range->first, v.range->second, v.points,
                                  v.verdict == std::string(PASS) ? "passes everywhere, tightest" : "worst",
                                  v.worst_x, v.sub_y, v.orig_y, useful_band.below.why.c_str());
                    notes.push_back(buf);
                    if (v.verdict == std::string(FAIL)) {
                        demote();
                        penalty += opt.gate_weight * kVerdictFailPenalty;
                        ++worse;
                    } else if (v.verdict == std::string(WARN)) {
                        demote();
                        penalty += opt.gate_weight * warn_penalty(v.warn_depth);
                        ++worse;
                    } else {
                        note_direction(esr_spec.dir, v.orig_y, v.sub_y);
                    }
                }
            }
        }
        const auto o_v = num(original, "esr"), s_v = num(cand, "esr");
        const auto o_f = num(original, "esr_frequency"), s_f = num(cand, "esr_frequency");
        auto same_f = [](double a, double b) { return std::fabs(a - b) <= 1e-9 * std::max(a, b); };
        auto mains = [&](double f) { return same_f(f, 100.0) || same_f(f, 120.0); };
        const bool basis_differs =
            o_v && s_v && o_f && !(s_f && (same_f(*o_f, *s_f) || (mains(*o_f) && mains(*s_f))));
        if (basis_differs) {
            char buf[320];
            const auto s_at = sc ? curve_at(sc->curve, *o_f, Interp::LogLog) : std::nullopt;
            const auto o_at =
                !s_at && oc && s_f ? curve_at(oc->curve, *s_f, Interp::LogLog) : std::nullopt;
            if (s_at) {
                std::snprintf(buf, sizeof buf,
                              "ESR compared at the original's %g Hz: the substitute's curve "
                              "reads %.4g ohm there (its record quotes %g ohm at %s) against "
                              "the original's %g ohm",
                              *o_f, *s_at, *s_v, s_f ? hz(*s_f).c_str() : "no frequency", *o_v);
                notes.push_back(buf);
                curve_override["esr"] = {compare_numeric(esr_spec, o_v, s_at), o_v, s_at};
            } else if (o_at) {
                std::snprintf(buf, sizeof buf,
                              "ESR compared at the substitute's %g Hz: the original's curve "
                              "reads %.4g ohm there (its record quotes %g ohm at %g Hz) "
                              "against the substitute's %g ohm",
                              *s_f, *o_at, *o_v, *o_f, *s_v);
                notes.push_back(buf);
                curve_override["esr"] = {compare_numeric(esr_spec, o_at, s_v), o_at, s_v};
            }
        }
    }
    const bool esr_basis_unverified =
        cat == "capacitor" && !curve_override.count("esr") &&
        basis_unverified("esr", "esr_frequency", "ESR", "ohm", /*primary=*/false);
    if (esr_basis_unverified) params.push_back({{"name", "esr_basis"}, {"verdict", UNVERIFIED}});

    // ── MOSFET Coss basis (the Vds the capacitances are stated at) ───────────
    // Coss falls by orders of magnitude with Vds, so two Coss figures compare only at a common
    // drain-source voltage — the diode Cj bias rule, applied to Vds (Qarlos r11 seed 24240811:
    // IPD95R2K0P7's 5 pF, stated at no Vds and close to its own curve near 400 V, passed
    // against AOD4N60's 53 pF at 25 V). Rule:
    //  * both state Coss and the original states its Vds: a substitute stating the same Vds is
    //    compared as stated; one stating a different Vds or none -> UNVERIFIED (coss and a
    //    coss_basis row), a note giving both values and voltages, no direction, no penalty.
    //    Coss is a secondary electrical, so no demotion and no cap (owner's secondary-axis
    //    rule, 2026-11-09);
    //  * the original states no Vds -> compared as stated, exactly as before (no basis exists
    //    for any candidate, so none is skewed against another).
    // A stated Coss(Vds) curve resolves the mismatch (owner curve rule 2026-11-14): the
    // substitute's curve read at the original's Vds, else the original's curve read at the
    // substitute's stated Vds — inside the curve's measured range only, never extrapolated,
    // linear in Vds (curve_checks). The scalar comparison is then made on that common basis
    // (the Coss spec, through curve_override["coss"]) and the note names the reading. With no curve
    // covering the other side's Vds, the rule above stands.
    bool coss_basis_unverified = false;
    if (cat == "mosfet") {
        const auto oc = num(original, "coss"), sc = num(cand, "coss");
        const auto ov = num(original, "coss_vds"), sv = num(cand, "coss_vds");
        if (oc && sc && ov &&
            !(sv && std::fabs(*ov - *sv) <= 1e-9 * std::max(std::fabs(*ov), std::fabs(*sv)))) {
            using curve_checks::read_at;
            const auto s_curve = curve_checks::view_curve(cand, "coss_curve");
            const auto o_curve = curve_checks::view_curve(original, "coss_curve");
            const auto s_at = s_curve ? read_at(*s_curve, *ov, Interp::Linear) : std::nullopt;
            const auto o_at =
                !s_at && sv && o_curve ? read_at(*o_curve, *sv, Interp::Linear) : std::nullopt;
            char rb[400], sv_txt[48] = "no stated Vds";
            if (sv) std::snprintf(sv_txt, sizeof sv_txt, "%g V", *sv);
            if (s_at) {
                curve_override["coss"] = {compare_numeric(spec_for("coss"), oc, s_at), oc, s_at};
                std::snprintf(rb, sizeof rb,
                              "output capacitance compared at the original's %g V: the "
                              "substitute's Coss(Vds) curve reads %.4g pF there (its record "
                              "states %g pF at %s) against the original's %g pF",
                              *ov, *s_at * 1e12, *sc * 1e12, sv_txt, *oc * 1e12);
                notes.push_back(rb + curve_checks::inferred_units(cand, "coss_curve", "substitute's"));
            } else if (o_at) {
                curve_override["coss"] = {compare_numeric(spec_for("coss"), o_at, sc), o_at, sc};
                std::snprintf(rb, sizeof rb,
                              "output capacitance compared at the substitute's %g V: the "
                              "original's Coss(Vds) curve reads %.4g pF there (its record "
                              "states %g pF at %g V) against the substitute's %g pF",
                              *sv, *o_at * 1e12, *oc * 1e12, *ov, *sc * 1e12);
                notes.push_back(rb + curve_checks::inferred_units(original, "coss_curve", "original's"));
            }
        }
        if (oc && sc && ov && !curve_override.count("coss") &&
            !(sv && std::fabs(*ov - *sv) <= 1e-9 * std::max(std::fabs(*ov), std::fabs(*sv)))) {
            coss_basis_unverified = true;
            params.push_back({{"name", "coss_basis"}, {"verdict", UNVERIFIED}});
            char buf[400];
            if (sv)
                std::snprintf(buf, sizeof buf,
                              "output capacitance quoted at different drain-source voltages "
                              "(original %g pF at %g V, substitute %g pF at %g V) — not "
                              "directly comparable, Coss falls steeply with Vds; confirm the "
                              "substitute's Coss at the original's Vds from its datasheet",
                              *oc * 1e12, *ov, *sc * 1e12, *sv);
            else
                std::snprintf(buf, sizeof buf,
                              "the original's output capacitance is %g pF at %g V; the "
                              "substitute's record states %g pF at no drain-source voltage — "
                              "not directly comparable, Coss falls steeply with Vds; confirm "
                              "its Coss at the original's Vds from the datasheet",
                              *oc * 1e12, *ov, *sc * 1e12);
            notes.push_back(buf);
        }
    }

    // ── diode Vf basis (the forward current the forward voltage is stated at) ─
    // Owner 2026-11-13 (Y5): Vf rises with forward current, so two forward voltages compare only
    // at a common test current — the Cj / Coss basis rule, applied to the current:
    //  * both state Vf and the original states its test current: a substitute stating the same
    //    current is compared as stated; one stating a different current or none -> UNVERIFIED
    //    (vf and a vf_basis row), a note giving both values and currents, no direction, no
    //    penalty, no demotion and no cap (Vf is a secondary electrical);
    //  * the original states no test current -> compared as stated, exactly as before.
    // A stated Vf(If) curve resolves the mismatch (owner curve rule 2026-11-14): the substitute's
    // curve read at the original's test current, else the original's curve read at the
    // substitute's stated current — inside the curve's measured range only, never extrapolated,
    // log-log in If (curve_checks). Only a curve stated at room temperature (25 degC, or 20 degC:
    // the same condition, owner 2026-11-14, is_room_temperature) is read: a scalar Vf states
    // no temperature on record, and the owner's decision (2026-11-14) takes it as 25 degC; a
    // curve at another temperature, or at none stated, is not on the scalar's basis. The scalar
    // comparison is then made on that common basis (the Vf spec, through curve_override["vf"])
    // and the note names the reading.
    bool vf_basis_unverified = false;
    if (cat == "diode") {
        const auto ov = num(original, "vf"), sv = num(cand, "vf");
        const auto oi = num(original, "vf_test_current"), si = num(cand, "vf_test_current");
        const bool vf_basis_differs =
            ov && sv && oi &&
            !(si && std::fabs(*oi - *si) <= 1e-9 * std::max(std::fabs(*oi), std::fabs(*si)));
        if (vf_basis_differs) {
            using curve_checks::read_at;
            auto at_25 = [](const json& v) -> std::optional<SpecCurve> {
                auto c = curve_checks::view_curve(v, "vf_curve");
                if (!c || !c->temperature || !is_room_temperature(*c->temperature))
                    return std::nullopt;
                return c;
            };
            const auto s_curve = at_25(cand), o_curve = at_25(original);
            const auto s_at = s_curve ? read_at(*s_curve, *oi, Interp::LogLog) : std::nullopt;
            const auto o_at =
                !s_at && si && o_curve ? read_at(*o_curve, *si, Interp::LogLog) : std::nullopt;
            char rb[560], si_txt[48] = "no stated current";
            if (si) std::snprintf(si_txt, sizeof si_txt, "%g A", *si);
            if (s_at) {
                curve_override["vf"] = {compare_numeric(spec_for("vf"), ov, s_at), ov, s_at};
                std::snprintf(rb, sizeof rb,
                              "forward voltage compared at the original's %g A: the "
                              "substitute's %g degC Vf(If) curve reads %.4g V there (its record "
                              "states %g V at %s) against the original's %g V (the scalar Vf "
                              "states no temperature and is taken as 25 degC; 20 and 25 degC "
                              "are both room temperature)",
                              *oi, *s_curve->temperature, *s_at, *sv, si_txt, *ov);
                notes.push_back(rb + curve_checks::inferred_units(cand, "vf_curve", "substitute's"));
            } else if (o_at) {
                curve_override["vf"] = {compare_numeric(spec_for("vf"), o_at, sv), o_at, sv};
                std::snprintf(rb, sizeof rb,
                              "forward voltage compared at the substitute's %g A: the "
                              "original's %g degC Vf(If) curve reads %.4g V there (its record "
                              "states %g V at %g A) against the substitute's %g V (the scalar "
                              "Vf states no temperature and is taken as 25 degC; 20 and 25 degC "
                              "are both room temperature)",
                              *si, *o_curve->temperature, *o_at, *ov, *oi, *sv);
                notes.push_back(rb + curve_checks::inferred_units(original, "vf_curve", "original's"));
            }
        }
        if (vf_basis_differs && !curve_override.count("vf")) {
            vf_basis_unverified = true;
            params.push_back({{"name", "vf_basis"}, {"verdict", UNVERIFIED}});
            char buf[400];
            if (si)
                std::snprintf(buf, sizeof buf,
                              "forward voltage quoted at different forward currents (original "
                              "%g V at %g A, substitute %g V at %g A) — not directly comparable, "
                              "Vf rises with current; confirm the substitute's Vf at the "
                              "original's current from its datasheet",
                              *ov, *oi, *sv, *si);
            else
                std::snprintf(buf, sizeof buf,
                              "the original's forward voltage is %g V at %g A; the substitute's "
                              "record states %g V at no forward current — not directly "
                              "comparable, Vf rises with current; confirm its Vf at the "
                              "original's current from the datasheet",
                              *ov, *oi, *sv);
            notes.push_back(buf);
        }
    }

    // ── capacitance tolerance (the band each record guarantees) ─────────────
    // A capacitor's tolerance is the band the circuit was designed around, exactly as a
    // resistor's is: a +/-5 % C0G is not a drop-in for a +/-1 % one in a filter or timing
    // network. Kelvin never compared it — the capacitor spec carried no tolerance — so
    // CDR31BP470BFYSAB (47 pF +/-1 %) took +/-5 % GRM21A5 parts at the same grade and
    // penalty as its +/-1 % equivalent, and MCARQ167SCG0R3ATRA1J (0.3 pF +/-0.05 pF) took a
    // +/-0.1 pF GQM part as drop_in (Qarlos 2026-11-09, seeds 20365838 / 21727315).
    // Rule (DECISION for the owner to confirm): each record's band is its stated minimum and
    // maximum, in farads, as deviations below and above its own nominal (asymmetric bands
    // such as -20/+80 % stay asymmetric):
    //   * equal nominals -> the deviations are compared in FARADS, the unit the records state,
    //     so an absolute small-part tolerance (+/-0.05 pF) is never turned into a percentage;
    //   * different nominals (the value window allows them) -> each deviation is taken as a
    //     fraction of that record's OWN stated nominal; nothing is assumed about either part;
    //   * the substitute's band wider on either side -> FAIL, a note quoting both bands — the
    //     timing-tolerance rule (no warn band), since a looser guarantee is a regression
    //     however small; tighter or equal -> PASS;
    //   * the original states a band and the substitute does not -> UNVERIFIED and noted; a
    //     secondary axis, so no demotion and no grade cap (owner, 2026-11-09);
    //   * the original states none -> not compared (no basis exists for any candidate).
    // One fact, one verdict: where this ran, the shared table's tolerance_pct row is skipped.
    bool cap_tol_judged = false;
    if (cat == "capacitor") {
        const auto o_n = num(original, "value_si"), s_n = num(cand, "value_si");
        const auto o_lo = num(original, "capacitance_min"), o_hi = num(original, "capacitance_max");
        const auto s_lo = num(cand, "capacitance_min"), s_hi = num(cand, "capacitance_max");
        auto farads = [](double f) {
            const char* u = "F";
            double v = f;
            if (f < 1e-9) { v = f * 1e12; u = "pF"; }
            else if (f < 1e-6) { v = f * 1e9; u = "nF"; }
            else if (f < 1e-3) { v = f * 1e6; u = "uF"; }
            else { v = f * 1e3; u = "mF"; }
            char b[48];
            std::snprintf(b, sizeof b, "%.4g %s", v, u);
            return std::string(b);
        };
        auto band = [&](double n, double lo, double hi) {
            const double dn = n - lo, up = hi - n;
            const double rel = 1e-9 * n;
            std::string s = std::fabs(dn - up) <= rel
                                ? "+/-" + farads(up)
                                : "-" + farads(dn) + "/+" + farads(up);
            s += " (" + (std::fabs(dn - up) <= rel ? "+/-" + plain_pct(100.0 * up / n)
                                                     : "-" + plain_pct(100.0 * dn / n) + "/+" +
                                                           plain_pct(100.0 * up / n)) + ")";
            return s;
        };
        if (o_n && *o_n > 0 && o_lo && o_hi) {
            cap_tol_judged = true;
            if (s_n && *s_n > 0 && s_lo && s_hi) {
                const bool same_nominal = std::fabs(*o_n - *s_n) <= 1e-9 * std::max(*o_n, *s_n);
                // Deviations below / above nominal, in farads (equal nominals) or as a
                // fraction of each record's own nominal (different nominals).
                const double o_scale = same_nominal ? 1.0 : *o_n, s_scale = same_nominal ? 1.0 : *s_n;
                const double o_dn = (*o_n - *o_lo) / o_scale, o_up = (*o_hi - *o_n) / o_scale;
                const double s_dn = (*s_n - *s_lo) / s_scale, s_up = (*s_hi - *s_n) / s_scale;
                // 1e-6 relative slack: the records' farads are decimal renderings (4.465e-11
                // stored as 4.4649999999999996e-11), not a tolerance on the guarantee.
                auto wider = [](double s, double o) { return s > o * (1.0 + 1e-6) + 1e-30; };
                const bool looser = wider(s_dn, o_dn) || wider(s_up, o_up);
                params.push_back({{"name", "capacitance_tolerance"}, {"verdict", looser ? FAIL : PASS}});
                if (looser) {
                    demote();
                    ++worse;
                    penalty += opt.gate_weight * kVerdictFailPenalty;
                    notes.push_back("looser capacitance tolerance: the substitute guarantees " +
                                    band(*s_n, *s_lo, *s_hi) + " against the original's " +
                                    band(*o_n, *o_lo, *o_hi) +
                                    " — not a drop-in wherever the value sets a filter, timing "
                                    "or matching point");
                }
            } else {
                params.push_back({{"name", "capacitance_tolerance"}, {"verdict", UNVERIFIED}});
                notes.push_back("the original guarantees " + band(*o_n, *o_lo, *o_hi) +
                                "; the substitute's record states no capacitance band — confirm "
                                "its tolerance from the datasheet");
            }
        }
    }
    // ── PARAM_SPECS verdicts (the shared Heaviside table) ────────────────────
    for (const ParamSpec& spec : params_for(cat)) {
        // The supply current (current_consumption, v22) is a supply parameter like the range:
        // a passive crystal or resonator has none, so it is skipped for them too.
        const bool supply_key = spec.key == "supply_min_V" || spec.key == "supply_max_V" ||
                                spec.key == "current_consumption";
        if (cat == "timeBase" && supply_key && timebase_passive) continue;
        // A programmable original or blank substitute: the frequency verdict is the range
        // check (programmable_frequency_fit), which also runs when neither record states a
        // single frequency.
        if (cat == "timeBase" && spec.key == "frequency" && prog_freq.applies) {
            params.push_back({{"name", "frequency"}, {"verdict", prog_freq.pass ? PASS : FAIL}});
            notes.push_back(prog_freq.note);
            if (!prog_freq.pass) return reject(prog_freq.reject_reason, prog_freq.unverifiable);
            // A blank admitted for an original that states one fixed frequency is not a plain
            // drop-in: it must be ORDERED programmed to that frequency (owner, 2026-11-13, M0).
            // Capped at minor_review and demoted; the 'order it programmed to X Hz' note above
            // says what the step is. A blank covering a programmable original's range is not
            // capped here — that original was itself a blank.
            if (prog_freq.blank_for_fixed) {
                timebase_prog_order = true;
                demote();
            }
            continue;
        }
        if (cat == "capacitor" && spec.key == "tolerance_pct" && cap_tol_judged) continue;
        bool has_data = detail::present(original, spec.key) || detail::present(cand, spec.key);
        const auto curve_ov = curve_override.find(spec.key);
        if (curve_ov != curve_override.end()) has_data = true;
        if (spec.key == "saturation_current")
            has_data = has_data || detail::present(original, "saturation_points") ||
                       detail::present(cand, "saturation_points");
        // A magnetic's saturation current stated on NEITHER record (Qarlos bench 20269028
        // IHSM5832ER151L -> B82477G2154M000, r16 26859036 LSQCA201616T680M -> LQH2MCN680K02):
        // the axis was skipped without a row, so a part came back drop_in / 'upgrade' with
        // nothing saying Isat was never compared. Missing on both sides is not penalised, and
        // since 2026-11-14 (owner) it caps at minor_review like every primary axis stated by
        // neither record (both_unstated_primary); before that it was not capped (owner 2026-11-12: identical for every candidate, it cannot skew one against
        // another), but the gap is shown as an UNVERIFIED row with a note (owner, 2026-11-13;
        // test_crossref_ranking_golden ABT #545 was narrowed to "no footprint/package note"
        // with the owner's approval). The direction becomes 'unknown' for 'upgrade' and
        // 'equivalent' (owner 2026-11-13; see out["direction"] below).
        if (!has_data) both_unstated(spec.key);  // owner 2026-11-14
        if (!has_data && cat == "magnetic" && spec.key == "saturation_current") {
            params.push_back({{"name", spec.key}, {"verdict", UNVERIFIED}});
            isat_unstated_both = true;
            notes.push_back("saturation current stated by neither record — Isat was not "
                            "compared, so whether the substitute saturates before the original "
                            "is unknown; confirm both from the datasheets");
            continue;
        }
        if (!has_data) continue;
        const bool power_at_t = cat == "resistor" && spec.key == "power_rating" && power_t.applies;
        // With no basis for the substitute's power at T_ref, a nominal rating below the
        // original's is a FAIL, not merely unverified: whatever its derating, the substitute
        // delivers at most its nominal there, and that is already less than the original's.
        const bool power_nominal_short =
            power_at_t && !power_t.subst && *detail::jnum(cand, spec.key) < *power_t.orig;
        const bool pin_field = spec.key == "pitch_mm" && connector_pitch.verdict != nullptr;
        const bool ripple_blind =
            cat == "capacitor" && ((spec.key == "ripple_current" &&
                                    (ripple_basis_unverified || ripple_temp_basis_unverified)) ||
                                   (spec.key == "esr" && esr_basis_unverified));
        const bool coss_blind = (cat == "mosfet" && spec.key == "coss" && coss_basis_unverified) ||
                                (cat == "diode" && spec.key == "vf" && vf_basis_unverified);
        const bool work_v_basis = spec.key == "max_voltage_V" && work_v.applies;
        // A diode's breakdown grade is DERIVED, (Vz max - Vz min) / (Vz max + Vz min), from two
        // window figures the datasheet states to 0.1 V. Two 5 % TVS grades therefore come out
        // 4.99419 % (43 V class, 40.9-45.2 V) and 4.99469 % (47 V class, 44.7-49.4 V), and the
        // strict compare called the second "looser" and warned while its own note printed
        // "4.99 %" for both (Qarlos r9 seed 23088792, P6KE43CA-B -> P6KE47CA-E3/54). A
        // difference below the note's 0.01 % print resolution is rounding in the derivation,
        // not a wider guarantee: the verdict may not claim what its note cannot show.
        const bool vz_grade_same = [&] {
            if (cat != "diode" || spec.key != "vz_tolerance_pct") return false;
            const auto o = detail::jnum(original, spec.key), s = detail::jnum(cand, spec.key);
            return o && s && std::fabs(*s - *o) < kVzGradePrintResolutionPct / 2.0;
        }();
        const ParamOutcome outcome =
            pin_field             ? ParamOutcome(connector_pitch.verdict)
            : curve_ov != curve_override.end() ? curve_ov->second.outcome
            : vz_grade_same       ? ParamOutcome(PASS)
            : ripple_blind        ? ParamOutcome(UNVERIFIED)
            : coss_blind          ? ParamOutcome(UNVERIFIED)
            : work_v_basis        ? compare_numeric(spec, work_v.orig, work_v.subst)
            : !power_at_t         ? compare_param(spec, original, cand)
            : power_t.subst       ? compare_numeric(spec, power_t.orig, power_t.subst)
            : power_nominal_short ? ParamOutcome(FAIL)
                                  : ParamOutcome(UNVERIFIED);
        // A technology difference the owner allows (timing_technology_cross): an established
        // difference, so a WARN — not the identity FAIL that would reject it below.
        const bool timebase_tech_allowed =
            cat == "timeBase" && spec.key == "technology" &&
            (timebase_tech_cross == TimingTechCross::MemsQuartz ||
             timebase_tech_cross == TimingTechCross::TcxoUpgrade);
        const std::string verdict = timebase_tech_allowed && outcome.verdict == std::string(FAIL)
                                        ? std::string(WARN)
                                        : outcome.verdict;
        params.push_back({{"name", spec.key}, {"verdict", verdict}});
        if (timebase_tech_allowed && verdict == WARN) {
            const std::string o_t = str(original, "technology"), s_t = str(cand, "technology");
            auto name = [](const std::string& t) {
                return t == "mems" ? std::string("a MEMS oscillator")
                       : t == "tcxo" ? std::string("a TCXO")
                                     : std::string("a quartz crystal oscillator (XO)");
            };
            if (timebase_tech_cross == TimingTechCross::MemsQuartz) {
                // Owner 2026-11-13: allowed, at most major_review. The WARN below costs a warn
                // step; the rest of a FAIL's penalty is added here so a cross-technology part
                // ranks with the regressions, below every same-technology one.
                timebase_tech_major = true;
                penalty += opt.gate_weight * (kVerdictFailPenalty - kVerdictWarnPenalty);
                notes.push_back("technology differs: the original is " + name(o_t) +
                                ", the substitute " + name(s_t) +
                                " — jitter / phase noise, shock and vibration tolerance, aging and "
                                "start-up behaviour differ between MEMS and quartz; verify it "
                                "against the application");
            } else {
                // Owner 2026-11-13: a TCXO for a plain XO is an upgrade, at most minor_review
                // (the WARN). The technology step itself is the improvement, so it counts toward
                // the direction; stability keeps its own comparison and a regression there
                // still turns the direction to 'mixed'.
                ++better;
                // No claim about the supply current: "at a higher supply current and cost" was
                // printed for every TCXO, including ASDAIG-48.000MHZ-X-K-T (13 mA) ->
                // 7N48071001 (10 mA) (Qarlos r22 30000906). Neither current nor cost was on the
                // timing row then, so the note asks for the check instead of asserting its
                // outcome. Since v22 the row carries the supply current (owner 2026-11-13, S3):
                // when both records state it, the current_consumption row compares it and the
                // note says no more; when either does not, the note still asks for the check.
                const bool current_compared = num(original, "current_consumption").has_value() &&
                                              num(cand, "current_consumption").has_value();
                notes.push_back("technology differs: " + name(s_t) + " offered for " + name(o_t) +
                                (current_compared
                                     ? std::string(" — a temperature-compensated upgrade; its "
                                                   "supply current is compared on its own row")
                                     : std::string(" — a temperature-compensated upgrade; its "
                                                   "supply current is not compared here, so "
                                                   "confirm the rail's current budget")));
            }
        }
        if (pin_field && verdict == UNVERIFIED) {
            connector_pitch_unverified = true;
            notes.push_back(connector_pitch.note);
        }
        if (cat == "connector" && verdict == FAIL) {
            if (spec.key == "pitch_mm") connector_pitch_conflict = true;
            if (spec.key == "contact_plating") connector_plating_conflict = true;
            // Gender and wire termination are identities that reject (is_hard_param), so
            // their note is written HERE, before the reject below returns — a bare "an
            // identity parameter differs" does not say a plug was offered for a socket.
            if (spec.key == "termination")
                notes.push_back("wire termination differs: " + str(original, "termination") +
                                " -> " + str(cand, "termination") +
                                " — a different wire-attach method, not a drop-in for a built "
                                "harness");
            // The family-variant identities reject like interface_standard (is_hard_param);
            // their note names the CONAS field and both stated values (owner 2026-11-12).
            if (const char* what = connector_variant_axis_label(spec.key))
                notes.push_back(std::string(what) + " differs: " +
                                str(original, spec.key.c_str()) + " -> " +
                                str(cand, spec.key.c_str()) +
                                " — the counterpart the original mates with does not fit, "
                                "lock or key into it; not a substitute");
            if (spec.key == "polarity")
                notes.push_back("mating gender differs: " + str(original, "polarity") + " -> " +
                                str(cand, "polarity") +
                                " — it does not mate with the counterpart the original mates "
                                "with");
        }
        if (cat == "timeBase" && supply_key) {
            // A supply range that does not cover the original's is REJECTED, a hard key like
            // output_type (owner 2026-11-13): the part will not run, or is overstressed, on
            // the rail the board provides — a 3.3 V-only ECX-P35BN offered for a 2.5-3.3 V
            // AK2F3PBQ1 came back major_review (Qarlos r16 26859036). The note names both
            // ranges so the rejection says why. One side unstated stays UNVERIFIED (below).
            if (verdict == FAIL) {
                notes.push_back("supply range does not cover the original's: the original runs "
                                "from " + supply_range(original) + ", the substitute from " +
                                supply_range(cand) +
                                " — it will not run correctly from the original's rail");
                return reject("supply range does not cover the original's rail");
            }
            // The supply current shares the supply_key gate but is not the range: a
            // substitute that states its 1.8 V rail but no current was told "states no supply
            // range" (Qarlos r26 32095486, ASDMB-50.000MHZ-C-40-T -> 831056223). Each gap
            // gets the note for the field that is actually missing.
            if (verdict == UNVERIFIED && detail::present(original, spec.key)) {
                if (spec.key == "current_consumption") timebase_current_unverified = true;
                else timebase_supply_unverified = true;
            }
        }
        if (cat == "timeBase" && spec.key == "frequency" && verdict == FAIL) {
            // Say how far off it is: the reject reason alone ("an identity parameter
            // differs") does not tell the engineer that 25.00125 MHz was offered for 25 MHz.
            const auto o_f = detail::jnum(original, "frequency"), s_f = detail::jnum(cand, "frequency");
            if (o_f && s_f && *o_f > 0) {
                char buf[256];
                std::snprintf(buf, sizeof buf,
                              "nominal frequency differs: the original is %.10g Hz, the "
                              "substitute %.10g Hz (%+.1f ppm)",
                              *o_f, *s_f, (*s_f / *o_f - 1.0) * 1e6);
                notes.push_back(std::string(buf) +
                                " — a timing part's frequency is its identity; a different "
                                "nominal is not a substitute, whatever its stability");
            }
        }
        if (cat == "timeBase" && spec.key == "enable_function") {
            const std::string o_en = str(original, "enable_function"),
                              s_en = str(cand, "enable_function");
            if (verdict == FAIL) {
                notes.push_back("control-pin function differs: the original's is '" + o_en +
                                "', the substitute's '" + s_en +
                                "' — pin 1 is wired for the original's function, so this is a "
                                "different pinout, not a drop-in");
            } else if (verdict == UNVERIFIED && (!o_en.empty() || !s_en.empty())) {
                timebase_pin_unverified = true;
                notes.push_back(
                    !o_en.empty()
                        ? "the original's control pin is '" + o_en +
                              "'; the substitute's record states no control-pin function — "
                              "confirm its pin 1 from the datasheet"
                        : "the substitute's control pin is '" + s_en +
                              "'; the original's record states no control-pin function — "
                              "confirm both pin 1 functions match from the datasheets");
            }
        }
        if (cat == "timeBase" && (spec.key == "tolerance_ppm" || spec.key == "stability_ppm")) {
            const char* what = spec.key == "tolerance_ppm" ? "frequency tolerance"
                                                           : "frequency stability";
            const auto o_p = detail::jnum(original, spec.key), s_p = detail::jnum(cand, spec.key);
            char buf[256];
            if (verdict == FAIL && o_p && s_p) {
                std::snprintf(buf, sizeof buf,
                              "looser %s: the substitute is +/-%g ppm against the original's "
                              "+/-%g ppm",
                              what, *s_p, *o_p);
                notes.push_back(std::string(buf) +
                                " — the design's timing budget was built on the original's");
            } else if (verdict == UNVERIFIED && o_p && !s_p) {
                timing_accuracy_unweighed = true;
                std::snprintf(buf, sizeof buf,
                              "the original's %s is +/-%g ppm; the substitute's record states "
                              "none",
                              what, *o_p);
                notes.push_back(std::string(buf) + " — confirm it from the datasheet");
                // Owner 2026-11-13 (S2): a TCXO exists for its frequency stability, so for a
                // TCXO original that states it, stability is a PRIMARY axis: a TCXO substitute
                // that does not state it is capped at major_review (missing_required_data),
                // not note-only. AVTX-13-E-40.000MHZ-E05 (+/-0.5 ppm) took
                // ECS-TXO-25CSMV-400-AN-TR (no stability on record) at minor_review. Only
                // the TCXO class: the code groups no other compensated technology with it (an
                // OCXO is its own identity, timing_technology_cross), and plain XOs keep the
                // secondary-axis rule.
                if (spec.key == "stability_ppm" && str(original, "technology") == "tcxo" &&
                    str(cand, "technology") == "tcxo") {
                    demote();
                    penalty += opt.gate_weight * kVerdictFailPenalty;
                    missing_required_data = true;
                }
            }
        }
        // A crystal's ESR (its maximum motional resistance) is what the original's oscillator
        // stage must overcome to start: a higher one eats the negative-resistance margin the
        // board was designed with. The WARN / FAIL verdict was printed with no note, so the
        // engineer saw a demotion and no reason (Qarlos bench 20284866,
        // ABMM-14.7456MHZ-D2X-T 50 ohm -> ECS-147.4-18-23A-JGN-TR 80 ohm).
        if (cat == "timeBase" && spec.key == "esr" && (verdict == WARN || verdict == FAIL)) {
            const auto o_r = detail::jnum(original, "esr"), s_r = detail::jnum(cand, "esr");
            if (o_r && s_r) {
                char buf[256];
                std::snprintf(buf, sizeof buf,
                              "higher ESR: the substitute's maximum is %g ohm against the "
                              "original's %g ohm",
                              *s_r, *o_r);
                notes.push_back(std::string(buf) +
                                " — less start-up (negative-resistance) margin in the original's "
                                "oscillator circuit; confirm the margin with the substitute");
            }
        }
        if (cat == "diode" && spec.key == "vz_tolerance_pct") {
            if (verdict == WARN || verdict == FAIL) diode_vz_band_regression = true;
            // Only when the ORIGINAL states a window: that is the case where a real
            // guarantee is on the table and the substitute's record answers nothing.
            if (verdict == UNVERIFIED && detail::present(original, spec.key))
                diode_vz_band_unverified = true;
        }

        const bool numeric = (spec.dir == Dir::Lower || spec.dir == Dir::Higher);
        if ((cat == "magnetic" || cat == "chipBead") && numeric && verdict == UNVERIFIED &&
            (spec.key == "rated_current" || spec.key == "dcr" || spec.key == "srf") &&
            detail::present(original, spec.key) && !detail::present(cand, spec.key)) {
            if (cat == "magnetic" && spec.key == "rated_current") {
                // See magnetic_rated_absent: penalty + note, no demotion, no cap.
                magnetic_rated_absent = true;
                penalty += opt.gate_weight * kVerdictFailPenalty;
                const auto o_ir = detail::jnum(original, spec.key);
                char buf[96] = "";
                if (o_ir) std::snprintf(buf, sizeof buf, " (%g A)", *o_ir);
                notes.push_back("substitute's record carries no rated current — the original "
                                "states one" + std::string(buf) +
                                ", so this comparison could not be made and whether the "
                                "substitute is ahead or behind is unknown; ranked below "
                                "candidates that state it — confirm it from the datasheet "
                                "before this swap");
            } else {
                magnetic_unweighed.push_back(param_label(spec.key));
            }
        }
        // Two ratings on different bases have no direction between them (ripple basis above).
        auto o = numeric && !ripple_blind && !coss_blind ? detail::jnum(original, spec.key)
                                                         : std::nullopt;
        auto s = numeric && !ripple_blind && !coss_blind ? detail::jnum(cand, spec.key)
                                                         : std::nullopt;
        if (curve_ov != curve_override.end()) {  // decided on a curve: its two values
            o = curve_ov->second.o;
            s = curve_ov->second.s;
        }
        if (power_at_t) {
            using power_detail::celsius;
            using power_detail::watts;
            const double s_nominal = *s;
            // Penalty and direction are about the power at T_ref too, not the nominal.
            o = power_t.orig;
            s = power_t.subst;
            if (power_nominal_short) {
                // The nominal is the most the substitute can deliver at T_ref, so the
                // downgrade is counted on it (the FAIL above does the demotion).
                s = s_nominal;
                notes.push_back("power rating below the original's: the substitute is rated " +
                                watts(s_nominal) + " against the original's " +
                                watts(*power_t.orig) + " at " + celsius(power_t.t_ref) +
                                ", and " + power_t.how);
            } else if (!power_t.subst) {
                // Not on record is not a pass: the original's rating is stated at a
                // temperature, and nothing says what the substitute delivers there. Capped
                // the way an unverified footprint is (power_basis_unverified below).
                demote();
                power_basis_unverified = true;
                notes.push_back("power rating not comparable at the original's rating "
                                "temperature: the original is rated " + watts(*power_t.orig) +
                                " at " + celsius(power_t.t_ref) + ", and " + power_t.how +
                                " — confirm the substitute's derating from its datasheet");
            } else if (!power_t.at_rating_temperature) {
                // The derating curves cross above the knee: name the temperature, both
                // powers there, and the rating temperature they were level or ahead at.
                notes.push_back("power compared along both derating curves: at " +
                                celsius(power_t.t_ref) + " the substitute delivers " +
                                watts(*power_t.subst) + " against the original's " +
                                watts(*power_t.orig) + " (rated " + watts(*num(original, "power_rating")) +
                                " at " + celsius(power_t.rating_temperature) +
                                ") — it derates more steeply above the original's knee");
            } else if (*power_t.subst != s_nominal) {
                notes.push_back("power compared at the original's rating temperature (" +
                                celsius(power_t.t_ref) + "): the substitute's " + watts(s_nominal) +
                                " rating delivers " + watts(*power_t.subst) + " there (" +
                                power_t.how + ") against the original's " +
                                watts(*power_t.orig));
            }
        }
        const bool hard_gate =
            is_hard_param(cat, spec.key) && (o || detail::present(original, spec.key));

        if (outcome.missing_required_sub) {
            // The substitute's record carries no value for this parameter at all,
            // so nothing was compared: the verdict stays UNVERIFIED rather than
            // claiming a regression that had no substitute-side operand. The
            // consequence is still the FAIL's — same penalty, same demotion, and
            // a grade capped at major_review — so a part we cannot check never
            // outranks one we could. Say so in a note, or the engineer reads an
            // unexplained downgrade (ABT #496).
            notes.push_back("substitute's record carries no " + param_label(spec.key) +
                            " — the original states one, so this comparison could not be made; "
                            "confirm it from the datasheet before this swap");
            if (hard_gate) return reject("a parameter the match turns on is absent from the "
                                         "substitute's record — cannot be verified", true);
            penalty += opt.gate_weight * kVerdictFailPenalty;
            // Only a PRIMARY rating's absence is missing required data (owner, 2026-11-09).
            // The one secondary axis the shared table marks exclude_missing_sub is a
            // capacitor's ESR: it keeps the FAIL penalty (ranking still prefers a part that
            // states it) and the note, but neither demotes nor caps the grade.
            const bool secondary_axis = cat == "capacitor" && spec.key == "esr";
            if (!secondary_axis) {
                demote();
                missing_required_data = true;
            }
        } else if (verdict == FAIL) {
            if (hard_gate)
                return reject(spec.dir == Dir::ExactMatch
                                  ? "an identity parameter differs from the original"
                                  : "a critical rating falls far below the original");
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
        } else if (verdict == WARN) {
            demote();
            // A WARN the comparison produced carries its depth; one rewritten to WARN above
            // (an allowed timing-technology cross) or taken from the connector pitch rule has
            // none and costs the flat warn.
            penalty += opt.gate_weight *
                       warn_penalty(verdict == outcome.verdict ? outcome.warn_depth : std::nullopt);
        } else if (verdict == PASS && numeric && o && s) {
            // Right-sizing tie-breaker: a rating that vastly exceeds the
            // original's costs a little, with diminishing returns.
            penalty += opt.overdim_weight * (spec.dir == Dir::Higher
                                                 ? over_dimensioning_penalty(o, s, 1.0)
                                                 : over_dimensioning_penalty(s, o, 1.0));
            if (spec.dir == Dir::Higher &&
                (spec.key == "saturation_current" || spec.key == "rated_current" ||
                 spec.key == "rated_current_A" || spec.key == "rated_voltage_V"))
                gross_over_rating(spec.key, o, s);
            // Resistor power (owner 2026-11-14): the NOMINAL ratings, whatever basis (rating
            // temperature, derating curve) the PASS was decided on.
            if (cat == "resistor" && spec.key == "power_rating")
                gross_over_rating(spec.key, num(original, "power_rating"),
                                  num(cand, "power_rating"));
        }
        if (numeric) {
            // Isat is compared at a common inductance-drop criterion, so its direction is too:
            // the raw scalars may be quoted at 10 % and 30 % drop and say nothing about which
            // part saturates first. With no common criterion there is no direction to count,
            // and the note names both bases (ABT #2042).
            std::optional<double> od = o, sd = s;
            bool basis_mismatch = false;
            const auto curves = spec.key == "saturation_current"
                                    ? isat::compare_curves(original, cand, spec.tol_factor)
                                    : isat::CurveCompare{};
            if (!curves.not_applied_note.empty()) notes.push_back(curves.not_applied_note);
            if (curves.applies) {
                // Owner 2026-11-13, Z5: the two L(I) curves decided (compare_saturation_current).
                // Direction from the worst criterion read on both; none read -> blind.
                char buf[96];
                std::string list;
                for (const auto& row : curves.rows) {
                    auto amps = [](const std::optional<double>& a) {
                        char b[32];
                        if (!a) return std::string("not reached");
                        std::snprintf(b, sizeof b, "%.3g A", *a);
                        return std::string(b);
                    };
                    std::snprintf(buf, sizeof buf, "%g %% drop ", row.drop_pct);
                    list += (list.empty() ? "" : "; ") + std::string(buf) + amps(row.sub_at) +
                            " vs " + amps(row.orig_at);
                }
                notes.push_back(std::string("saturation compared on both L(I) curves at ") +
                                (curves.stated_criterion ? "the original's stated criterion"
                                                         : "10, 20 and 30 % inductance drop") +
                                " (substitute vs original: " + list +
                                ") — a drop outside a curve's measured range is not "
                                "extrapolated and reads unverified");
                if (curves.decisive && verdict != UNVERIFIED) {
                    od = curves.rows[*curves.decisive].orig_at;
                    sd = curves.rows[*curves.decisive].sub_at;
                } else {
                    basis_mismatch = true;
                }
            } else if (spec.key == "saturation_current") {
                const auto op = isat::resolve_points(original), sp = isat::resolve_points(cand);
                if (!op.empty() && !sp.empty() && (isat::has_basis(op) || isat::has_basis(sp))) {
                    if (auto t = isat::common_percent_drop(op, sp)) {
                        od = isat::isat_at_percent_drop(op, *t);
                        sd = isat::isat_at_percent_drop(sp, *t);
                    } else {
                        basis_mismatch = true;
                        notes.push_back("saturation current quoted at different criteria (" +
                                        isat_basis(op) + " on the original, " + isat_basis(sp) +
                                        " on the substitute) — not directly comparable; confirm "
                                        "the substitute's Isat at the original's criterion");
                    }
                }
            }
            // A WARN or FAIL on a directional parameter IS a regression on it, whatever the
            // ratio: a 34.9 A Isat against 36 A warned, sat inside the +/-5 % neutral band
            // and the row came back 'upgrade' beside its own warn (ABT #2039).
            if (basis_mismatch) {
                // The hard axis could not be weighed, exactly the direction_blind situation:
                // a reassuring direction would be a claim about a comparison not made.
                if (is_hard_param(cat, spec.key)) isat_basis_blind = true;
            } else if (verdict == WARN || verdict == FAIL) {
                ++worse;
            } else {
                note_direction(spec.dir, od, sd);
            }
        }
        // The original states only a rated current and the substitute saturates below it
        // (compare_saturation_current). That axis WAS weighed — and came out behind — so it
        // is not reported as blind; say which two figures were set against each other.
        const bool isat_vs_rating = spec.key == "saturation_current" &&
                                    (verdict == WARN || verdict == FAIL) &&
                                    isat_below_original_rating(original, cand);
        if (isat_vs_rating)
            notes.push_back("the original's record states no saturation current, but the "
                            "substitute saturates below the original's rated current — it "
                            "cannot carry the current the original is rated for without "
                            "losing inductance");
        if (numeric && is_hard_param(cat, spec.key) && !o && s && !isat_vs_rating) {
            direction_blind = true;
            blind_axes.push_back(param_label(spec.key));
        }
        if (numeric && is_primary_rating(cat, spec.key) && !detail::jnum(original, spec.key) &&
            detail::jnum(cand, spec.key))
            orig_unstated(spec.key);
    }

    // ── magnetic curves, curve vs curve (owner 2026-11-14) ───────────────────
    // Every curve BOTH records carry (CrossRefMagneticCurves.hpp): judged at every point
    // inside both measured ranges with a scalar spec's bands, the worst point deciding. A WARN
    // or FAIL is a measured regression (demoted and penalised like the scalar row it extends);
    // UNVERIFIED (no overlap, different measurement conditions) is a note only, no cap.
    if (cat == "magnetic" || cat == "chipBead") {
        for (const magcurve::Result& r : magcurve::compare_all(original, cand)) {
            params.push_back({{"name", r.name}, {"verdict", r.verdict}});
            notes.push_back(r.note);
            const std::string v = r.verdict;
            if (v == FAIL) {
                demote();
                penalty += opt.gate_weight * kVerdictFailPenalty;
                ++worse;
            } else if (v == WARN) {
                demote();
                penalty += opt.gate_weight * warn_penalty(r.warn_depth);
                ++worse;
            } else if (v == PASS) {
                note_direction(r.dir, r.orig_y, r.sub_y);
            }
        }
    }

    // ── capacitor curves with no scalar of their own (owner 2026-11-14) ─────────
    // Each is compared on the overlap of the two measured ranges with the bands and direction of
    // the closest scalar spec (named in its note), the worst point deciding. Booked exactly like
    // a table parameter: a WARN demotes and costs its depth-scaled warn, a FAIL demotes and
    // costs the FAIL penalty (any_warn / any_fail then set the grade), both count as a
    // regression. Both records stating the curve under different conditions, or no overlap ->
    // UNVERIFIED with a note: no penalty, no cap, no direction. A curve on one side only is not
    // a comparison and adds nothing.
    auto curve_param = [&](const char* key, const char* name, const char* what, const char* unit,
                           const char* x_what, const char* x_unit, const ParamSpec& spec,
                           const char* spec_named, Interp interp,
                           const CurveBand* banded = nullptr) {
        const auto oc = curve_from_spec(original, key), sc = curve_from_spec(cand, key);
        if (!oc || !sc) return;
        char buf[800];
        const std::string cond = conditions_differ(*oc, *sc);
        if (!cond.empty()) {
            params.push_back({{"name", name}, {"verdict", UNVERIFIED}});
            notes.push_back(std::string(what) + " curves measured under different conditions (" +
                            cond + ") — not compared");
            return;
        }
        // `banded`: an impedance-type curve, graded only inside that band of the original's.
        if (banded && !banded->band) {
            params.push_back({{"name", name}, {"verdict", UNVERIFIED}});
            notes.push_back(std::string(what) + " curves not compared: " + banded->why);
            return;
        }
        const CurveVerdict v = compare_curves(oc->curve, sc->curve, spec, interp,
                                              banded ? banded->band : std::nullopt);
        params.push_back({{"name", name}, {"verdict", v.verdict}});
        if (v.verdict == std::string(UNVERIFIED)) {
            std::snprintf(buf, sizeof buf,
                          "%s curves do not %s (original %g to %g %s, substitute %g to %g "
                          "%s%s%s) — not compared",
                          what, banded ? "meet inside the useful band" : "overlap",
                          oc->curve.x.front(), oc->curve.x.back(), x_unit,
                          sc->curve.x.front(), sc->curve.x.back(), x_unit, banded ? "; " : "",
                          banded ? banded->why.c_str() : "");
            notes.push_back(buf);
            return;
        }
        const bool ok = v.verdict == std::string(PASS);
        std::snprintf(buf, sizeof buf,
                      "%s compared on both curves over %s %g to %g %s (%zu points, %s, bands of "
                      "the %s): %s at %g %s, substitute %.4g %s vs original %.4g %s%s%s%s",
                      what, x_what, v.range->first, v.range->second, x_unit, v.points,
                      interp == Interp::LogLog ? "log-log" : "linear", spec_named,
                      ok ? "passes everywhere, tightest" : "worst", v.worst_x, x_unit, v.sub_y, unit, v.orig_y,
                      unit, banded ? " (" : "", banded ? banded->why.c_str() : "",
                      banded ? ")" : "");
        notes.push_back(buf);
        if (v.verdict == std::string(FAIL)) {
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            ++worse;
        } else if (v.verdict == std::string(WARN)) {
            demote();
            penalty += opt.gate_weight * warn_penalty(v.warn_depth);
            ++worse;
        } else {
            note_direction(spec.dir, v.orig_y, v.sub_y);
        }
    };
    if (cat == "capacitor") {
        // (b) DC bias (owner 2026-11-14, "compare when both state curves"): effective
        // capacitance at each common bias voltage, absolute farads as the records state them,
        // linear in V (the bias axis is linear and starts at 0 V). Bands from the capacitance
        // spec's lower side (primary_value_spec("capacitor"): PASS down to 0.90x, accept down
        // to 0.80x): PASS >= 0.90x the original's C_eff, WARN down to 0.80x, FAIL below. More
        // capacitance is never a regression here; the nominal window still bounds the top.
        static const ParamSpec kBiasSpec{"capacitance_bias", Dir::Higher, 0.80, std::nullopt,
                                         false, false, nullptr, 0.90};
        curve_param("capacitance_bias_curve", "capacitance_bias",
                    "effective capacitance under DC bias", "F", "DC bias", "V", kBiasSpec,
                    "capacitance spec", Interp::Linear);
        // (c) The frequency curves, log-log (all three are plotted on log axes), each with the
        // closest scalar spec, named in its note:
        //  |Z|(f): the ESR spec (lower is better, PASS <= 1x, WARN to 1.5x, FAIL beyond) — the
        //          impedance a decoupling or filter capacitor presents is what it is used for;
        //  C(f):   the capacitance spec's lower bands, as for DC bias (read only below the first
        //          non-positive sample, see Views.cpp leading_positive);
        //  ESL(f): the ESR spec's bands (lower is better), the only lower-is-better impedance
        //          spec the capacitor table has; ESL itself has no scalar spec.
        static const ParamSpec kImpedanceSpec{"impedance", Dir::Lower, 1.5, std::nullopt,
                                              false, false, nullptr};
        static const ParamSpec kCapFreqSpec{"capacitance_frequency", Dir::Higher, 0.80,
                                            std::nullopt, false, false, nullptr, 0.90};
        static const ParamSpec kEslSpec{"esl", Dir::Lower, 1.5, std::nullopt, false, false,
                                        nullptr};
        // |Z| is graded only below the original's self-resonance, ESL only above it
        // (useful_band above);
        // C(f) is read only below self-resonance already (leading_positive), DC bias unchanged.
        curve_param("impedance_curve", "impedance_curve", "|Z|", "ohm", "frequency", "Hz",
                    kImpedanceSpec, "ESR spec", Interp::LogLog, &useful_band.below);
        curve_param("capacitance_frequency_curve", "capacitance_frequency_curve",
                    "capacitance vs frequency", "F", "frequency", "Hz", kCapFreqSpec,
                    "capacitance spec", Interp::LogLog);
        curve_param("esl_curve", "esl_curve", "ESL", "H", "frequency", "Hz", kEslSpec,
                    "ESR spec", Interp::LogLog, &useful_band.esl);
    }

    // ── transformer turns ratio (owner 2026-11-13, R2; W4) ───────────────────
    // MagneticRow carried the ratio but the ranker never compared it, so a 1:1.2 part and a
    // 1:2 part were the same candidate. Owner: "turns ratio can be +-15%, but with care".
    //   both stated, every ratio within 1 % (print rounding)      -> PASS
    //   worst ratio off by more than 1 %, up to 15 %              -> WARN + note
    //   worst ratio off by more than 15 %                         -> rejected (W4), reason given
    //   different number of ratios (a different winding structure) -> rejected (W4), reason given
    //   stated on one side only                                   -> UNVERIFIED + note, no cap
    // Each array is one electrical[] wiring configuration's turnsRatios in record order
    // (primary:secondary for each secondary), compared element-wise; the deviation is
    // |sub/orig - 1| per element. A record may list several configurations (Würth's "all
    // windings" and "auxiliary windings open"): every configuration of the original is set
    // against every configuration of the substitute and the best pair decides (owner
    // 2026-11-13, W4) — the pair of equal size with the smallest worst deviation. A row without
    // turns_ratio_configs (an older caller) is read as its one turns_ratios configuration.
    if (cat == "magnetic") {
        using Cfg = std::vector<double>;
        auto one = [](const json& a) {
            Cfg v;
            if (!a.is_array()) return v;
            for (const auto& x : a) {
                if (!x.is_number() || !(x.get<double>() > 0)) return Cfg{};  // all-or-nothing
                v.push_back(x.get<double>());
            }
            return v;
        };
        auto configs = [&](const json& p) {
            std::vector<Cfg> out;
            if (!p.is_object()) return out;
            if (auto it = p.find("turns_ratio_configs"); it != p.end() && it->is_array())
                for (const auto& c : *it)
                    if (Cfg v = one(c); !v.empty()) out.push_back(std::move(v));
            if (out.empty())
                if (auto it = p.find("turns_ratios"); it != p.end())
                    if (Cfg v = one(*it); !v.empty()) out.push_back(std::move(v));
            return out;
        };
        const std::vector<Cfg> o_cf = configs(original), s_cf = configs(cand);
        auto list = [](const Cfg& v) {
            std::string out;
            char buf[32];
            for (size_t i = 0; i < v.size(); ++i) {
                std::snprintf(buf, sizeof buf, "%.4g", v[i]);
                out += (i ? ", " : "") + std::string(buf);
            }
            return v.size() > 1 ? "[" + out + "]" : out;
        };
        auto lists = [&](const std::vector<Cfg>& cf) {
            std::string out;
            for (size_t i = 0; i < cf.size(); ++i) out += (i ? " or " : "") + list(cf[i]);
            return out;
        };
        auto which = [](size_t i, size_t n) {
            return "wiring configuration " + std::to_string(i + 1) + " of " + std::to_string(n);
        };
        if (!o_cf.empty() && !s_cf.empty()) {
            // Best pair: equal size, smallest worst deviation.
            bool any_pair = false;
            double best = 0.0;
            size_t bo = 0, bs = 0, bw = 0;
            for (size_t i = 0; i < o_cf.size(); ++i)
                for (size_t j = 0; j < s_cf.size(); ++j) {
                    if (o_cf[i].size() != s_cf[j].size()) continue;
                    double worst = 0.0;
                    size_t wi = 0;
                    for (size_t k = 0; k < o_cf[i].size(); ++k) {
                        const double d = std::fabs(s_cf[j][k] / o_cf[i][k] - 1.0);
                        if (d > worst) { worst = d; wi = k; }
                    }
                    if (!any_pair || worst < best) {
                        any_pair = true;
                        best = worst;
                        bo = i;
                        bs = j;
                        bw = wi;
                    }
                }
            if (!any_pair) {
                params.push_back({{"name", "turns_ratio"}, {"verdict", FAIL}});
                notes.push_back("winding structure differs: the original states turns ratio(s) " +
                                lists(o_cf) + ", the substitute " + lists(s_cf) +
                                " — a different number of secondaries in every listed wiring "
                                "configuration, not a substitute");
                return reject("transformer winding structure differs (a different number of "
                              "turns ratios)");
            }
            const Cfg& o_tr = o_cf[bo];
            const Cfg& s_tr = s_cf[bs];
            // Which configurations the verdict was taken on, when either record lists several.
            std::string on;
            if (o_cf.size() > 1 || s_cf.size() > 1) {
                if (o_cf.size() > 1) on += "the original's " + which(bo, o_cf.size());
                if (s_cf.size() > 1)
                    on += (on.empty() ? "" : " against ") + std::string("the substitute's ") +
                          which(bs, s_cf.size());
            }
            const char* v = PASS;
            if (best > kTurnsRatioPassRel + kEdgeEps) {
                v = best > kTurnsRatioWarnRel + kEdgeEps ? FAIL : WARN;
                char buf[200];
                std::snprintf(buf, sizeof buf, "turns ratio %.4g vs the original's %.4g",
                              s_tr[bw], o_tr[bw]);
                std::string note = std::string(buf);
                if (o_tr.size() > 1)
                    note += " (secondary " + std::to_string(bw + 1) + " of " +
                            std::to_string(o_tr.size()) + ")";
                note += " (" + signed_pct((s_tr[bw] / o_tr[bw] - 1.0) * 100.0) + ")";
                if (!on.empty()) note += ", closest pair: " + on;
                note += v == WARN ? " — check the circuit tolerates it"
                                  : " — beyond the 15 % a transformer swap can absorb; a "
                                    "different ratio, not a substitute without a redesign";
                notes.push_back(note);
            } else if (!on.empty() && (bo != 0 || bs != 0)) {
                notes.push_back("turns ratio " + list(s_tr) + " matches the original's " +
                                list(o_tr) + " on " + on +
                                " — confirm the circuit wires it that way");
            }
            params.push_back({{"name", "turns_ratio"}, {"verdict", v}});
            if (v == FAIL)
                return reject("transformer turns ratio more than 15 % from the original's");
            if (v == WARN) {
                demote();
                penalty += opt.gate_weight * kVerdictWarnPenalty;
                ++worse;
            }
        } else if (!o_cf.empty() || !s_cf.empty()) {
            // Secondary-axis missing data: note only, no cap, no penalty (owner policy).
            params.push_back({{"name", "turns_ratio"}, {"verdict", UNVERIFIED}});
            notes.push_back(!o_cf.empty()
                                ? "the original's turns ratio is " + lists(o_cf) +
                                      "; the substitute's record states none — confirm it "
                                      "from the datasheet"
                                : "the substitute's turns ratio is " + lists(s_cf) +
                                      "; the original's record states none — confirm both "
                                      "ratios match from the datasheets");
        }
    }

    // ── cable cores: cable fit and pass count (owner 2026-11-14) ─────────────
    // A cable core (MAS subtype cableCore) is chosen for the cable it must pass and is rated
    // per pass count; ranked by the impedance model (chipBead), two of its axes are its own:
    //   cable_max_diameter (maximumCableOuterDiameter, m): the substitute must accept at least
    //     the original's largest cable — smaller -> REJECTED (no substitute: the cable does not
    //     pass through, owner 2026-11-14); equal or larger -> PASS, and more than
    //     1.5x the original's opening -> PASS with a note that the cable fits loosely;
    //   number_turns (numberTurns, the cable passes its |Z| is stated for): must match -> FAIL.
    // Not stated by the substitute while the original states it -> UNVERIFIED + note, capped
    // at minor_review. Stated by the substitute only -> UNVERIFIED + note, no cap (the owner's
    // rule covers the substitute-missing case only). Stated by neither -> nothing (no cap).
    if ((cat == "chipBead" || cat == "magnetic") && str(original, "device_type") == "cableCore") {
        auto mm = [](double m) {
            char b[32];
            std::snprintf(b, sizeof b, "%.3g mm", m * 1e3);
            return std::string(b);
        };
        auto passes = [](double n) {
            char b[32];
            std::snprintf(b, sizeof b, "%g", n);
            return std::string(b) + (n == 1.0 ? " pass" : " passes");
        };
        auto one_sided = [&](const char* key, const char* what, const std::optional<double>& o,
                             const std::string& shown) {
            params.push_back({{"name", key}, {"verdict", UNVERIFIED}});
            if (o) {
                cable_core_unverified = true;
                demote();
                notes.push_back(std::string("the original's ") + what + " is " + shown +
                                "; the substitute's record states none — confirm it from the "
                                "datasheet before this swap");
            } else {
                notes.push_back(std::string("the substitute's ") + what + " is " + shown +
                                "; the original's record states none — confirm the original's "
                                "from its datasheet");
            }
        };
        const auto od = num(original, "cable_max_diameter_m"), sd = num(cand, "cable_max_diameter_m");
        if (od && sd) {
            const char* v = PASS;
            if (*sd < *od && !same_stated_value(*sd, *od)) {
                notes.push_back("cable opening " + mm(*sd) + " against the original's " + mm(*od) +
                                " — the original's largest cable does not pass through the "
                                "substitute");
                params.push_back({{"name", "cable_max_diameter_m"}, {"verdict", FAIL}});
                return reject("cable core opening smaller than the original's: the original's "
                              "cable does not pass through");
            } else if (*sd > kCableLooseFitRatio * *od) {
                notes.push_back("cable opening " + mm(*sd) + " against the original's " + mm(*od) +
                                " — more than 1.5x larger, so the original's cable fits loosely "
                                "(less of the core's impedance couples to a thin cable; check "
                                "the fit)");
            }
            params.push_back({{"name", "cable_max_diameter_m"}, {"verdict", v}});
        } else if (od || sd) {
            one_sided("cable_max_diameter_m", "maximum cable outer diameter", od, mm(od ? *od : *sd));
        }
        const auto on = num(original, "number_turns"), sn = num(cand, "number_turns");
        if (on && sn) {
            const bool same = same_stated_value(*on, *sn);
            params.push_back({{"name", "number_turns"}, {"verdict", same ? PASS : FAIL}});
            if (!same) {
                notes.push_back("impedance stated for " + passes(*sn) + " of the cable against "
                                "the original's " + passes(*on) +
                                " — a different configuration, its |Z| is not the original's");
                demote();
                penalty += opt.gate_weight * kVerdictFailPenalty;
                ++worse;
            }
        } else if (on || sn) {
            one_sided("number_turns", "number of cable passes", on, passes(on ? *on : *sn));
        }
    }

    // ── connector identities stated on one side only ─────────────────────────
    // Gender (part.matingPolarity), wire termination (familyDetails.termination / clampType),
    // contact pitch (mechanical.pitch, the land pattern) and row count (mechanical.rows) are
    // identities of a connector, judged by the rule the orientation and pluggable axes use
    // (Qarlos 2026-11-09):
    //   both stated, different -> reject (gender, termination: is_hard_param) or a different
    //                             land pattern (pitch, rows: connector_pitch_fit);
    //   stated on ONE side     -> unverified with a note naming both sides, capped below
    //                             drop_in like an unverified footprint — drop_in asserts the
    //                             part mates, attaches and lands like the original, and nobody
    //                             has shown that. HIF3B-10D-2.54R(63) (IDC) took card-edge
    //                             parts with no termination as drop_in; 713492023 (no pitch on
    //                             record) took a TE header stating 2.54 mm as a clean drop_in
    //                             while better-documented parts were capped; 54202-G0804-B02LF
    //                             (2 rows) never compared rows against candidates stating none;
    //   stated on neither side -> silent (nothing to compare, the same for every candidate).
    // The PARAM_SPECS loop above has already pushed the gender / termination / pitch verdict
    // (UNVERIFIED for one side); this block adds the note, the cap, and the rows verdict.
    // A pitch that only appears in a description ("6.35mm Pitch") is NOT read here: Kelvin
    // compares structured fields, and the description gap is the catalogue's (TAS #2337).
    if (cat == "connector") {
        const auto stated = [](const json& rec, const char* k) {
            auto it = rec.find(k);
            if (it == rec.end() || it->is_null()) return false;
            if (it->is_string()) return !it->get<std::string>().empty();
            return true;
        };
        const auto said = [&](const json& rec, const char* k) -> std::string {
            if (!stated(rec, k)) return "not stated";
            const json& v = rec.at(k);
            if (v.is_string()) return v.get<std::string>();
            if (!v.is_number())
                throw std::invalid_argument(std::string("cross_reference: connector '") + k +
                                            "' must be a string or a number");
            const double x = v.get<double>();
            if (std::string(k) == "pitch_mm") return mm(x * 1e-3) + " mm";
            return std::to_string(static_cast<long>(std::lround(x)));
        };
        struct Axis { const char* key; const char* what; const char* confirm; };
        static const Axis kAxes[] = {
            {"polarity", "mating gender", "confirm it mates with the original's counterpart"},
            {"termination", "wire termination", "confirm the same wire-attach method"},
            {"pitch_mm", "contact pitch", "confirm the pin field lines up"},
            {"rows", "contact rows", "confirm the pin field lines up"},
        };
        for (const Axis& a : kAxes) {
            const bool os = stated(original, a.key), ss = stated(cand, a.key);
            if (os == ss) continue;
            // rows is not a PARAM_SPECS key, so its verdict is pushed here.
            if (std::string(a.key) == "rows")
                params.push_back({{"name", "rows"}, {"verdict", UNVERIFIED}});
            connector_identity_unverified = true;
            // Only an axis the ORIGINAL states and the substitute does not is a gap toward the
            // several-gaps cap (owner 2026-11-13, W1): stating extra data never hurts a candidate.
            if (os) connector_identity_gaps.push_back(a.what);
            notes.push_back(std::string(a.what) + " not established (original: " +
                            said(original, a.key) + ", substitute: " + said(cand, a.key) +
                            ") — " + a.confirm + " from the datasheets");
        }
        // Position count stated by the SUBSTITUTE only (Qarlos r15 26440120: 05FHJ-SM1-TB, whose
        // record states no positions, took the 20-position Molex 2250190200 as a clean drop_in,
        // 'equivalent', no note). Positions is an identity (is_hard_param): stated on both
        // sides and different it rejects, and a substitute that omits it while the original
        // states it is rejected in the PARAM_SPECS loop (missing required data on a hard
        // axis), so only this direction reaches here. It takes the one-sided identity
        // treatment of gender / termination / pitch / rows: the UNVERIFIED verdict the loop
        // pushed, a note naming both sides, and the cap below drop_in. It does NOT count toward
        // the several-gaps major_review cap below: only axes the original states count (owner
        // 2026-11-13 W1, reversing the earlier I4 choice that counted it).
        if (!stated(original, "positions") && stated(cand, "positions")) {
            connector_identity_unverified = true;
            notes.push_back("contact positions not established (original: " +
                            said(original, "positions") + ", substitute: " +
                            said(cand, "positions") +
                            ") — confirm the position count from the datasheets");
        }
        // Family-variant identities stated on one side only (owner 2026-11-12, "Identity when
        // stated"): the PARAM_SPECS loop has pushed UNVERIFIED; this adds the note naming the
        // field and the stated value. No cap and no demotion: that is interface_standard's
        // one-sided treatment (UNVERIFIED verdict, no note, no penalty, no cap), mirrored
        // exactly except for the note the owner asked for.
        for (const char* k : {"actuator_type", "coupling_type", "coding", "shell_size"}) {
            const bool os = stated(original, k), ss = stated(cand, k);
            if (os == ss) continue;
            // Counted toward the several-gaps cap (grade site) although a single one
            // stays uncapped: these are identities when stated (owner, 2026-11-12), and
            // DECISION for the owner: whether they should count is recorded at the cap.
            // Only when the original states it (owner 2026-11-13, W1).
            if (os) connector_identity_gaps.push_back(connector_variant_axis_label(k));
            notes.push_back(std::string(connector_variant_axis_label(k)) +
                            " not established (original: " + said(original, k) +
                            ", substitute: " + said(cand, k) +
                            ") — confirm it from the datasheets");
        }
    }

    // ── connector mating-axis orientation (CONAS mechanical.orientation) ─────
    // vertical / rightAngle / horizontal / mezzanine / edge / cable is the direction the
    // counterpart (cable, FPC, daughter board) arrives from: a vertical FPC connector
    // for a right-angle one is a different mechanical design — the flex leaves the board
    // the other way — not a substitution (F32Q-1A7H1-11024 vs 2-2492142-4, Qarlos
    // 2026-11-08 seed 20269028). Rule:
    //   both stated and equal                -> pass;
    //   both stated, different               -> fail, rejected like any connector identity;
    //   horizontal against rightAngle        -> unverified with a note (DECISION for the
    //     owner: both describe a mating axis parallel to the board and vendors use the two
    //     words for the same body, but the schema keeps them apart, so neither "same" nor
    //     "different" can be asserted from the record);
    //   stated on one side only              -> unverified with a note, capped like an
    //     unverified footprint (never a drop-in on an orientation nobody checked);
    //   stated on neither side               -> not compared (silent, as every axis is).
    if (cat == "connector") {
        const std::string oo = str(original, "orientation"), so = str(cand, "orientation");
        if (!oo.empty() || !so.empty()) {
            const auto said = [](const std::string& v) {
                return v.empty() ? std::string("not stated") : v;
            };
            const bool parallel_pair = (oo == "horizontal" && so == "rightAngle") ||
                                       (oo == "rightAngle" && so == "horizontal");
            if (!oo.empty() && !so.empty() && oo == so) {
                params.push_back({{"name", "orientation"}, {"verdict", PASS}});
            } else if (!oo.empty() && !so.empty() && !parallel_pair) {
                params.push_back({{"name", "orientation"}, {"verdict", FAIL}});
                notes.push_back("orientation differs: " + oo + " -> " + so +
                                " — the counterpart mates along a different axis; a different "
                                "mechanical design, not a drop-in");
                return reject("connector orientation differs (a different mating axis)");
            } else {
                params.push_back({{"name", "orientation"}, {"verdict", UNVERIFIED}});
                connector_orientation_unverified = true;
                if (parallel_pair)
                    notes.push_back("orientation: the original is " + oo +
                                    " and the substitute " + so +
                                    " — vendors use both words for a mating axis parallel to the "
                                    "board; confirm from the drawings that the counterpart "
                                    "arrives the same way");
                else {
                    // One-sided orientation is one of the identity gaps counted towards the
                    // 3+ cap (owner decision 2026-11-13); the horizontal/rightAngle pair is
                    // stated on both sides and is not a gap. Only the original's stated
                    // orientation is a gap (owner 2026-11-13, W1).
                    if (!oo.empty()) connector_identity_gaps.push_back("orientation");
                    notes.push_back("orientation not established (original: " + said(oo) +
                                    ", substitute: " + said(so) +
                                    ") — confirm vertical / right-angle from the datasheets");
                }
            }
        }
        // ── pluggable vs fixed terminal block (CONAS familyTerminalBlock.pluggable) ──
        // A pluggable terminal-block plug is the loose half that mates onto a board header;
        // a fixed PCB terminal block is soldered to the board and takes the wire itself. One
        // is not a drop-in for the other: Molex's 395337424 Eurostyle plug took WAGO 739-124
        // fixed strips (Qarlos 2026-11-08). Tri-state, never defaulted:
        //   both stated, different -> reject (a plug is not a fixed block);
        //   both stated, same      -> pass;
        //   stated on one side     -> unverified with a note, capped like an unverified footprint;
        //   stated on neither      -> not compared (silent).
        const auto pluggable_of = [](const json& rec) -> std::optional<bool> {
            auto it = rec.find("pluggable");
            if (it == rec.end() || it->is_null()) return std::nullopt;
            if (!it->is_boolean())
                throw std::invalid_argument("cross_reference: connector 'pluggable' must be "
                                            "true, false or null");
            return it->get<bool>();
        };
        const auto op = pluggable_of(original), sp = pluggable_of(cand);
        if (op || sp) {
            const auto said = [](const std::optional<bool>& v) {
                return !v ? std::string("not stated")
                          : *v ? std::string("pluggable") : std::string("fixed");
            };
            if (op && sp && *op == *sp) {
                params.push_back({{"name", "pluggable"}, {"verdict", PASS}});
            } else if (op && sp) {
                params.push_back({{"name", "pluggable"}, {"verdict", FAIL}});
                notes.push_back("terminal block form differs: " + said(op) + " -> " + said(sp) +
                                " — a pluggable plug mates onto a header, a fixed block is "
                                "soldered to the board; not a drop-in");
                return reject("pluggable vs fixed terminal block (a different form factor)");
            } else {
                params.push_back({{"name", "pluggable"}, {"verdict", UNVERIFIED}});
                connector_pluggable_unverified = true;
                notes.push_back("pluggable vs fixed not established (original: " + said(op) +
                                ", substitute: " + said(sp) +
                                ") — confirm both are the same form (pluggable plug or fixed "
                                "PCB block) from the datasheets");
            }
        }
    }
    // ── resistor TCR and operating range: say it, and say what is unknown ──
    // TNPW0805680RDETA (25 ppm/K thin film) took ERJS06D6800V (100 ppm/K) with nothing said
    // (Qarlos 2026-11-08): the TCR verdict alone does not tell the engineer how far apart the
    // two are. And an operating range the original states but the substitute's record does
    // not is UNVERIFIED, with a note naming the end that went unchecked.
    // The same operating-range rule holds for a magnetic (Qarlos 2026-11-09, seed 20680025):
    // its PARAM_SPECS carry temp_min_C / temp_max_C with no warn band.
    // Operating range, TCR and diode Tj max are SECONDARY axes (owner, 2026-11-09): stated on
    // the original only, they are UNVERIFIED and noted but neither demote nor cap the grade
    // (they used to cap at minor_review; see the table at the grade site). A stated shortfall
    // is still a FAIL from the table loop.
    if (cat == "resistor" || cat == "magnetic" || cat == "chipBead" || cat == "diode") {
        auto verdict_of = [&](const char* name) -> std::string {
            for (const auto& p : params)
                if (p.value("name", "") == name) return p.value("verdict", "");
            return "";
        };
        const std::string tv = verdict_of("tcr");
        const auto o_tcr = num(original, "tcr"), s_tcr = num(cand, "tcr");
        if (cat == "resistor" && (tv == WARN || tv == FAIL) && o_tcr && s_tcr) {
            char b[160];
            std::snprintf(b, sizeof b,
                          "TCR +/-%g ppm/K vs the original's +/-%g ppm/K (%.3gx worse)",
                          std::fabs(*s_tcr), std::fabs(*o_tcr),
                          std::fabs(*o_tcr) > 0 ? std::fabs(*s_tcr) / std::fabs(*o_tcr) : 0.0);
            notes.push_back(std::string(b) +
                            " — the resistance drifts that much more over temperature; verify "
                            "anywhere the value sets a ratio, a reference or a gain");
        }
        // A TCR the original states and the substitute's record does not is UNVERIFIED and
        // noted — the operating-range rule below (secondary: no demotion, no cap).
        // CRCW080530R0JNTA (200 ppm/K) took three YAGEO AC0805 parts, whose records state no
        // TCR, as drop_in with no note (Qarlos 2026-11-09, seed 22146231). When NEITHER record
        // states one, the gap is the same for every candidate and nothing is said.
        if (cat == "resistor" && tv == UNVERIFIED && o_tcr && !s_tcr) {
            char b[96];
            std::snprintf(b, sizeof b, "the original states a TCR of +/-%g ppm/K",
                          std::fabs(*o_tcr));
            notes.push_back(std::string(b) +
                            "; the substitute's record states none — its drift over temperature "
                            "is unknown; confirm it from the datasheet");
        }
        // A resistor's maximum working voltage (max_voltage_V in kResistor, the power-rating
        // band): a WARN / FAIL is already counted worse by the table loop; name both voltages,
        // because the verdict alone does not say how much headroom went. One the original
        // states and the substitute's record does not is UNVERIFIED and noted, no demotion and
        // no cap (Qarlos r10 seed 23612437, r8 seed 22565147).
        if (cat == "resistor") {
            const auto o_v = num(original, "max_voltage_V"), s_v = num(cand, "max_voltage_V");
            const std::string v = verdict_of("max_voltage_V");
            // On the working-voltage basis (resistor_working_voltage) the note says so, with
            // the voltages the verdict was taken on.
            auto basis = [&] {
                return "the working voltage, min(maximum voltage, sqrt(P x R)), is " +
                       volts(work_v.subst) + " V against the original's " +
                       volts(work_v.orig) + " V";
            };
            if ((v == WARN || v == FAIL) && o_v && s_v) {
                notes.push_back("lower maximum working voltage: the substitute is rated " +
                                volts(*s_v) + " V against the original's " + volts(*o_v) +
                                " V" + (work_v.applies ? "; " + basis() : std::string()) +
                                " — check the voltage the resistor actually sees in the "
                                "design");
            } else if (v == PASS && work_v.applies && o_v && s_v && *s_v < *o_v) {
                notes.push_back("maximum voltage stated as " + volts(*s_v) +
                                " V against the original's " + volts(*o_v) + " V, but at " +
                                "these resistances and power ratings " + basis() +
                                " — the power rating, not the stated voltage, is the limit");
            } else if (v == UNVERIFIED && o_v && !s_v) {
                notes.push_back("the original's maximum working voltage is " + volts(*o_v) +
                                " V; the substitute's record states none — confirm it from the "
                                "datasheet");
            }
        }
        for (const char* k : {"temp_min_C", "temp_max_C"}) {
            if (verdict_of(k) == UNVERIFIED && num(original, k) && !num(cand, k)) {
                notes.push_back(std::string("the original states its operating range to ") +
                                degc(*num(original, k)) +
                                " degC; the substitute's record states no " +
                                (std::string(k) == "temp_min_C" ? "minimum" : "maximum") +
                                " operating temperature — confirm it from the datasheet");
            }
        }
        // A diode's maximum junction temperature (tj_max in kDiode, no band): a lower one is
        // already a FAIL counted worse by the table loop; name both temperatures, because the
        // verdict alone does not say how much thermal margin went. One the original states
        // and the substitute's record does not is UNVERIFIED and noted like the operating
        // range above, with no demotion and no cap (Qarlos 2026-11-09, seeds 20575296 /
        // 20889483; secondary since the owner's rule of the same day).
        if (cat == "diode") {
            const auto o_tj = num(original, "tj_max"), s_tj = num(cand, "tj_max");
            const std::string v = verdict_of("tj_max");
            if (v == FAIL && o_tj && s_tj) {
                notes.push_back("lower maximum junction temperature: the substitute is rated to " +
                                degc(*s_tj) + " degC against the original's " + degc(*o_tj) +
                                " degC — less thermal margin at the original's operating point; "
                                "re-check the junction temperature in the design");
            } else if (v == UNVERIFIED && o_tj && !s_tj) {
                notes.push_back("the original's maximum junction temperature is " +
                                degc(*o_tj) +
                                " degC; the substitute's record states none — confirm it from "
                                "the datasheet");
            }
        }
    }
    // A magnetic's SRF short of the original's (srf spec, CrossRefParams.hpp): name both
    // frequencies, so the engineer can set the gap against the circuit's working frequency.
    if (cat == "magnetic") {
        for (const auto& p : params) {
            if (p.value("name", "") != "srf") continue;
            const std::string v = p.value("verdict", "");
            const auto o_srf = num(original, "srf"), s_srf = num(cand, "srf");
            if ((v == WARN || v == FAIL) && o_srf && s_srf) {
                // The size of the gap as a ratio only where a ratio reads (2x and more): a
                // 2.00 vs 2.01 MHz gap printed "(1x lower)" through %.3g (Qarlos bench seed
                // 20470567, IHLP8787MZER101M5A -> SRP2313AA-101M). Below 2x it is a percentage.
                const double ratio = *o_srf / *s_srf;
                char gap[48];
                if (ratio >= 2.0)
                    std::snprintf(gap, sizeof gap, "%.3gx lower", ratio);
                else
                    std::snprintf(gap, sizeof gap, "%s", signed_pct(100.0 * (*s_srf / *o_srf - 1.0)).c_str());
                char b[200];
                std::snprintf(b, sizeof b, "self-resonant frequency %g MHz vs the original's %g MHz (%s)",
                              *s_srf / 1e6, *o_srf / 1e6, gap);
                notes.push_back(std::string(b) +
                                " — it stops behaving as the same inductor at a lower "
                                "frequency; verify against the circuit's working frequency");
            }
        }
    }

    // Say WHAT changed about the mating interface, not just that something did. The
    // counterpart is already on the board and is not being replaced, so the substitute
    // meets it on the substitute's finish, and the pairing is what decides.
    if (connector_plating_conflict)
        notes.push_back("contact plating differs: " + str(original, "contact_plating") + " -> " +
                        str(cand, "contact_plating") +
                        " — the substitute meets your existing counterpart on a different mating "
                        "surface; dissimilar finishes fret and corrode at a separable joint");

    if (timebase_pin_unverified) demote();
    if (timebase_supply_unverified)
        notes.push_back("the original runs from " + supply_range(original) +
                        "; the substitute's record states no supply range, so the rail could "
                        "not be checked — confirm it from the datasheet");
    if (timebase_current_unverified) {
        // %g, not volts()'s three decimals: a 32 kHz oscillator draws microamps (1.5 uA would
        // print as "0.002 mA").
        char cur[32];
        std::snprintf(cur, sizeof cur, "%g mA", *num(original, "current_consumption") * 1000.0);
        notes.push_back("the original draws " + std::string(cur) +
                        " from its supply; the substitute's record states no supply current, so "
                        "the rail's current budget could not be checked — confirm it from the "
                        "datasheet");
    }

    // Say WHAT the two parts guarantee, in volts. The Vrrm verdict above compares the
    // MARKED voltage, which two grades of the same zener share; the window is what the
    // reference or the clamp was designed around, so a change in it — or an unknown —
    // has to be stated rather than left implied by a "pass" on the marking.
    if (diode_vz_band_unverified)
        notes.push_back("the original guarantees " + vz_band(original) +
                        "; the substitute's record states no breakdown-voltage band, so the "
                        "grades could not be compared — confirm the substitute's from its "
                        "datasheet before using it as a reference or a clamp");
    else if (diode_vz_band_regression)
        notes.push_back("looser breakdown grade: the substitute guarantees " + vz_band(cand) +
                        " against the original's " + vz_band(original) +
                        " — confirm the reference or clamp level tolerates the wider window");

    // ── transient protection (tvs / esd originals) ───────────────────────────
    // A protection diode is chosen for the surge it absorbs, the level it clamps to and the
    // capacitance it hangs on the line — none of which Vrrm (its standoff) says. Nothing of
    // it was compared: a 400 W SMAJ came back 'upgrade' for a 15 kW 15KP, a 90 pF part
    // 'recommended' for a 1.2 pF one, a unidirectional part for a bidirectional one
    // (ABT #2036, #2038, #2040, #2041, #2048-#2051). Rules, Alf 2026-10-25:
    //  * Ppk / Ipp / Vc are compared only between ratings stated at the SAME impulse; a
    //    different or unstated waveform leaves the parameter UNVERIFIED with a note, and for
    //    the surge ratings caps the grade as missing required data (not a rejection).
    //  * a breakdown window reaching above the original's raises the protection level: WARN.
    //  * uni-directional for a bi-directional original (decoded from the classic series
    //    order code) FAILS; an undecodable polarity is UNVERIFIED.
    const std::string o_tech = str(original, "technology");
    const bool protection = cat == "diode" && (o_tech == "tvs" || o_tech == "esd");
    bool surge_blind = false;
    if (protection) {
        auto fmt = [](double v, const char* unit) {
            char b[48];
            std::snprintf(b, sizeof b, "%g %s", v, unit);
            return std::string(b);
        };
        auto at_wf = [](const std::string& wf) {
            return wf.empty() ? std::string("at no stated waveform") : "at " + wf + " us";
        };
        // One waveform-bound rating. `cap` = its unknowns cap the grade (the surge ratings).
        auto waveform_rated = [&](const char* key, const char* wf_key, Dir dir, double tol,
                                  const char* label, const char* unit, bool cap,
                                  const char* consequence) {
            const auto o = num(original, key), s = num(cand, key);
            if (!o && !s) {
                both_unstated(key);  // owner 2026-11-14
                return;
            }
            const std::string ow = str(original, wf_key), sw = str(cand, wf_key);
            if (!o) {  // the original states nothing to hold the substitute to
                params.push_back({{"name", key}, {"verdict", UNVERIFIED}});
                if (is_primary_rating(cat, key)) orig_unstated(key);  // Y2
                return;
            }
            auto unverified = [&](const std::string& why) {
                params.push_back({{"name", key}, {"verdict", UNVERIFIED}});
                notes.push_back(why);
                if (cap) {
                    demote();
                    penalty += opt.gate_weight * kVerdictFailPenalty;
                    missing_required_data = true;
                    // The rating the protection match turns on went unweighed, so a
                    // reassuring direction would be a claim about a comparison not made.
                    surge_blind = true;
                }
            };
            if (!s) {
                unverified(std::string("substitute's record carries no ") + label +
                           " — the original states " + fmt(*o, unit) + " " + at_wf(ow) +
                           "; confirm it from the datasheet before this swap");
                return;
            }
            if (ow.empty() || sw.empty() || ow != sw) {
                unverified(std::string(label) + " stated at different or unstated impulses (" +
                           fmt(*o, unit) + " " + at_wf(ow) + " on the original, " +
                           fmt(*s, unit) + " " + at_wf(sw) +
                           " on the substitute) — not comparable; confirm the substitute's " +
                           label + " at the original's waveform");
                return;
            }
            // Clamping voltage is a point on the V-I curve: the same waveform at a different
            // current is a different point (Qarlos 2026-11-09, seed 20784754: TPC20AHM3_A/H
            // clamps 27.7 V at 54.2 A, 15KPA018-SD-Q 28.4 V at 527.8 A, and the ranker warned
            // "higher clamp voltage" across a 10x current gap). Rule (DECISION for the owner:
            // the 1.1 band): both records state their test current and the larger is more
            // than 1.1x the smaller -> UNVERIFIED, with both (Vc, Ipp) pairs named. No scaling
            // to a common current — the dynamic resistance is not on record.
            if (std::string(key) == "clamping_voltage") {
                constexpr double kClampCurrentBand = 1.1;
                const auto oi = num(original, "clamping_current"),
                           si = num(cand, "clamping_current");
                if (oi && si && std::max(*oi, *si) > kClampCurrentBand * std::min(*oi, *si)) {
                    unverified("clamping voltage stated at different test currents (" +
                               fmt(*o, unit) + " at " + fmt(*oi, "A") + " on the original, " +
                               fmt(*s, unit) + " at " + fmt(*si, "A") +
                               " on the substitute) — not comparable; confirm the substitute's "
                               "clamping voltage at the original's current");
                    return;
                }
            }
            const ParamSpec spec{key, dir, tol, std::nullopt, false, false, nullptr};
            const ParamOutcome cmp = compare_numeric(spec, o, s);
            const char* v = cmp.verdict;
            params.push_back({{"name", key}, {"verdict", v}});
            if (v == FAIL || v == WARN) {
                demote();
                penalty += opt.gate_weight *
                           (v == FAIL ? kVerdictFailPenalty : warn_penalty(cmp.warn_depth));
                ++worse;
                notes.push_back(std::string(label) + " " + fmt(*s, unit) + " against the original's " +
                                fmt(*o, unit) + " " + at_wf(ow) + " (" +
                                signed_pct(100.0 * (*s / *o - 1.0)) + ") — " + consequence);
            } else {
                note_direction(dir, o, s);
            }
        };
        waveform_rated("peak_pulse_power", "peak_pulse_power_waveform", Dir::Higher, 0.9,
                       "peak pulse power", "W", true,
                       "the substitute absorbs less surge energy than the original is rated for");
        waveform_rated("peak_pulse_current", "peak_pulse_current_waveform", Dir::Higher, 0.9,
                       "peak pulse current", "A", true,
                       "the substitute survives less surge current than the original is rated for");
        waveform_rated("clamping_voltage", "clamping_waveform", Dir::Lower, 1.05,
                       "clamping voltage", "V", false,
                       "the protected circuit sees a higher voltage during a surge; re-check its "
                       "rating");

        // Breakdown window: where the part begins to conduct. Upper ends when both state
        // one, else lower ends.
        // The original's UPPER end with none on the substitute (Qarlos bench 20575296:
        // 5.0SMDJ100A-QH 111-123 V against 15KP100A, 111 V minimum and no maximum): the
        // lower ends alone cannot show that the substitute's window stays under the
        // original's top, so equal lower ends are no PASS — the axis is UNVERIFIED, noted.
        // A substitute minimum already above the original's would still be the WARN below
        // (that much is established whatever its maximum).
        {
            const auto olo = num(original, "vz_min_V"), ohi = num(original, "vz_max_V");
            const auto slo = num(cand, "vz_min_V"), shi = num(cand, "vz_max_V");
            std::optional<double> oe, se;
            if (ohi && shi) oe = ohi, se = shi;
            else if (olo && slo) oe = olo, se = slo;
            const bool upper_unstated_sub = ohi && !shi;
            if (oe && se && upper_unstated_sub && !(*se > *oe * (1.0 + 1e-3))) {
                params.push_back({{"name", "breakdown_window"}, {"verdict", UNVERIFIED}});
                notes.push_back("the original's breakdown window tops out at " + volts(*ohi) +
                                " V; the substitute's record states only its minimum (" +
                                volts(*slo) +
                                " V), so whether its window reaches above the original's is "
                                "unknown — confirm its maximum breakdown voltage from the "
                                "datasheet");
            } else if (oe && se) {
                const bool above = *se > *oe * (1.0 + 1e-3);
                params.push_back({{"name", "breakdown_window"}, {"verdict", above ? WARN : PASS}});
                if (above) {
                    demote();
                    penalty += opt.gate_weight * kVerdictWarnPenalty;
                    ++worse;
                    auto band = [&](std::optional<double> lo, std::optional<double> hi) {
                        return (lo ? volts(*lo) : std::string("?")) + "-" +
                               (hi ? volts(*hi) : std::string("?")) + " V";
                    };
                    notes.push_back("breakdown window " + band(slo, shi) +
                                    " reaches above the original's " + band(olo, ohi) +
                                    " — the substitute starts to conduct later, so the protection "
                                    "level rises; re-check what the protected circuit tolerates");
                }
            } else if (olo || ohi || slo || shi) {  // one side states none: not comparable
                params.push_back({{"name", "breakdown_window"}, {"verdict", UNVERIFIED}});
            }
        }

        // Standoff band, two-sided (Alf 2026-10-25, ABT #2034): the standoff is the rail the
        // part guards, so one more than 10 % above the original's clamps too late for it —
        // a 12 V part on a 5.5 V line. WARN; for a TVS original, above kStandoffFailRatio a
        // FAIL (caps at major_review), owner 2026-11-13: a 13.6 V-standoff 1.5KE16CA for a
        // 6.5 V SA6.5CA (2.09x) came back a WARN like a 1.2x part (Qarlos bench 20575296).
        // Extended to ESD originals (owner, 2026-11-13): the same physics — a 12 V-standoff
        // part does not clamp a 5.5 V line.
        constexpr double kStandoffWarnRatio = 1.10;
        constexpr double kStandoffFailRatio = 1.50;
        if (const auto ov = num(original, "vrrm"), sv = num(cand, "vrrm");
            ov && sv && *sv > *ov * kStandoffWarnRatio) {
            const bool far_above = (o_tech == "tvs" || o_tech == "esd") && *sv > *ov * kStandoffFailRatio;
            params.push_back({{"name", "standoff_band"}, {"verdict", far_above ? FAIL : WARN}});
            demote();
            // The WARN band runs from 1.10x to the 1.50x FAIL limit for a TVS / ESD original,
            // so its penalty scales across it (warn_penalty). Any other original has no fail
            // limit on this axis, so its WARN keeps the flat warn.
            const bool has_fail_limit = o_tech == "tvs" || o_tech == "esd";
            penalty += opt.gate_weight *
                       (far_above ? kVerdictFailPenalty
                                  : warn_penalty(has_fail_limit
                                                     ? warn_depth_between(*sv / *ov,
                                                                          kStandoffWarnRatio,
                                                                          kStandoffFailRatio)
                                                     : std::nullopt));
            ++worse;
            notes.push_back("standoff " + volts(*sv) + " V against the original's " + volts(*ov) +
                            " V (" + signed_pct(100.0 * (*sv / *ov - 1.0)) +
                            ") — a protection part for a higher rail; it clamps later than the "
                            "protected circuit expects" +
                            (far_above ? std::string("; more than 1.5x the original's standoff, "
                                                     "so its clamping voltage is far above what "
                                                     "the protected circuit was designed to see")
                                       : std::string()));
        }
        // Standoff BELOW the original's (Qarlos 2026-11-08): the original's standoff is the
        // circuit's working voltage, and a part whose standoff is lower is outside its
        // rated off-state there. The vrrm rating below only WARNED inside its 0.9-0.95
        // band, so a 66 V 15KPA066C came back minor_review for a 70 V 15KP70. Rule
        // (conservative, for the owner to confirm):
        //  * substitute's breakdown MINIMUM at or below the original's standoff: it may be
        //    in breakdown — conducting — at the circuit's working voltage. REJECTED.
        //  * otherwise a standoff below the original's: FAIL (caps at major_review); its
        //    leakage is unspecified at the working voltage.
        if (const auto ov = num(original, "vrrm"), sv = num(cand, "vrrm");
            ov && sv && *sv < *ov * (1.0 - 1e-3)) {
            const auto s_vbr_min = num(cand, "vz_min_V");
            params.push_back({{"name", "standoff_band"}, {"verdict", FAIL}});
            if (s_vbr_min && *s_vbr_min <= *ov) {
                notes.push_back("standoff " + volts(*sv) + " V and breakdown minimum " +
                                volts(*s_vbr_min) + " V against the original's " + volts(*ov) +
                                " V standoff — the substitute can conduct at the circuit's "
                                "working voltage");
                return reject("breakdown minimum at or below the original's working voltage");
            }
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            ++worse;
            notes.push_back("standoff " + volts(*sv) + " V against the original's " + volts(*ov) +
                            " V (" + signed_pct(100.0 * (*sv / *ov - 1.0)) +
                            ") — below the circuit's working voltage; its leakage there is not "
                            "specified");
        }

        // Polarity. The verdict is always reported for a protection original (Qarlos r13,
        // seeds 25078643 / 25183372): SMB10J30CAH and SMCG75CA-HRA are bidirectional, but
        // their series are not among the decoded ones, so SMBJ30A came back with a bare
        // UNVERIFIED polarity and no word, and SMCG78A-M3/57T (both sides undecodable) with no
        // polarity row at all. An unknown polarity on either side is UNVERIFIED with a note
        // saying which side could not be read; like the other one-sided unknowns of a
        // secondary nature it neither demotes nor caps (no cap existed here before either).
        const TvsPolarity op = tvs_polarity_from_mpn(str(original, "mpn")),
                          sp = tvs_polarity_from_mpn(str(cand, "mpn"));
        {
            if (op == TvsPolarity::Bi && sp == TvsPolarity::Uni) {
                params.push_back({{"name", "polarity"}, {"verdict", FAIL}});
                demote();
                penalty += opt.gate_weight * kVerdictFailPenalty;
                ++worse;
                notes.push_back("unidirectional substitute for a bidirectional original (decoded "
                                "from the part numbers) — it does not clamp a negative excursion, "
                                "it conducts it as a forward diode");
            } else if (op == TvsPolarity::Uni && sp == TvsPolarity::Bi) {
                // The mirror image is no silent pass either (Qarlos 2026-11-08). A
                // unidirectional original clamps a NEGATIVE excursion as a forward diode at
                // about -1 V; a bidirectional part lets it swing to its reverse breakdown,
                // tens of volts for a 58 V standoff, and no longer conducts the reverse-
                // polarity fault a uni part is often placed to blow a fuse with. A
                // different clamping characteristic: FAIL, capping at major_review, the
                // same weight as the opposite case (conservative; for the owner to confirm).
                params.push_back({{"name", "polarity"}, {"verdict", FAIL}});
                demote();
                penalty += opt.gate_weight * kVerdictFailPenalty;
                ++worse;
                notes.push_back("bidirectional substitute for a unidirectional original (decoded "
                                "from the part numbers) — a negative excursion is no longer "
                                "clamped near -1 V by forward conduction but only at the "
                                "substitute's reverse breakdown; confirm what the protected "
                                "circuit tolerates below ground");
            } else if (op == TvsPolarity::Unknown || sp == TvsPolarity::Unknown) {
                params.push_back({{"name", "polarity"}, {"verdict", UNVERIFIED}});
                auto pol = [](TvsPolarity t) {
                    return std::string(t == TvsPolarity::Bi ? "bidirectional" : "unidirectional");
                };
                if (op == TvsPolarity::Bi)
                    notes.push_back("the original is bidirectional (decoded from its part number); "
                                    "the substitute's polarity cannot be read from its record or "
                                    "part number — confirm it clamps both polarities");
                else if (op == TvsPolarity::Uni)
                    notes.push_back("the original is unidirectional (decoded from its part "
                                    "number); the substitute's polarity cannot be read from its "
                                    "record or part number — confirm it is unidirectional too "
                                    "(a bidirectional part no longer clamps a negative "
                                    "excursion near -1 V)");
                else if (sp != TvsPolarity::Unknown)
                    notes.push_back("the substitute is " + pol(sp) +
                                    " (decoded from its part number); the original's polarity "
                                    "cannot be read from its record or part number — confirm "
                                    "from the original's datasheet that it is " + pol(sp) +
                                    " too before this swap");
                else
                    notes.push_back("TVS polarity (unidirectional / bidirectional) is stated by "
                                    "neither record and decodes from neither part number — "
                                    "confirm from both datasheets that the substitute clamps the "
                                    "same polarities as the original");
            } else {
                params.push_back({{"name", "polarity"}, {"verdict", PASS}});
            }
        }
    }

    // ── junction capacitance, every diode whose original states one ─────────
    // It loads the line a protection part guards, and it is the switching/RF behaviour of a
    // small-signal or Schottky part. It was compared only for TVS / ESD originals, so BAS70JW
    // (2 pF small-signal Schottky) took SB3100 (a 3 A rectifier whose record states no Cj,
    // hundreds of pF on its datasheet) and SBL10100A as minor_review (Qarlos 2026-11-09, seed
    // 21203670). Rules (OWNER DECIDED for the absent case):
    //  * both state Cj: equal or lower PASS, up to 2x WARN, beyond FAIL (counted worse);
    //  * the original states one and the substitute's record does not: UNVERIFIED, noted,
    //    with the missing-required-data consequence (FAIL penalty, capped at major_review) —
    //    a part whose capacitance may be 100x the original's is never a minor review. This
    //    is the one secondary electrical that keeps a cap: the owner kept it explicitly on
    //    2026-11-12 (BAS70JW must not get 3 A / 10 A rectifiers as usable substitutes);
    //  * only the substitute states one: UNVERIFIED, nothing to hold it to.
    // Cj is stated AT a reverse bias (junction_capacitance_vr) and falls steeply with it, so
    // two values compare only on a common bias — the ESR / ripple frequency-basis rule
    // (basis_unverified above), applied to the bias (Qarlos r8 seed 22774605: PMEG6020EPAF,
    // 250 pF at 1 V, passed against MBRS2560T3G's 170 pF at 48 V):
    //  * both state the bias and it is the same -> compared as stated;
    //  * both state it and it differs, or the original states it and the substitute does
    //    not -> UNVERIFIED (junction_capacitance and a junction_capacitance_basis row), a
    //    note giving both values and biases, no direction, no penalty; Cj is a secondary
    //    electrical, so no demotion and no cap (owner's secondary-axis rule, 2026-11-09);
    //  * the original states no bias -> compared as stated, exactly as before (no basis
    //    exists for any candidate, so none is skewed against another).
    // No conversion between biases is attempted: never a C(V) model with an assumed
    // built-in potential and grading coefficient.
    if (cat == "diode") {
        const auto oc = num(original, "junction_capacitance"),
                   sc = num(cand, "junction_capacitance");
        const auto ovr = num(original, "junction_capacitance_vr"),
                   svr = num(cand, "junction_capacitance_vr");
        auto pf = [](double f) {
            char b[48];
            std::snprintf(b, sizeof b, "%g pF", f * 1e12);
            return std::string(b);
        };
        const std::string load = protection ? "more load on the protected line; check its "
                                              "signal integrity"
                                            : "more capacitance across the junction; check "
                                              "switching speed and signal loading in the "
                                              "original's circuit";
        auto bias = [](double v) {
            char b[32];
            std::snprintf(b, sizeof b, "%g V", v);
            return std::string(b);
        };
        const bool cj_basis_differs =
            oc && sc && ovr &&
            !(svr && std::fabs(*ovr - *svr) <= 1e-9 * std::max(std::fabs(*ovr), std::fabs(*svr)));
        // A different bias does not always leave the comparison open. Cj only FALLS as the
        // reverse bias rises (a depletion capacitance), and every stated or unstated bias is a
        // reverse bias >= 0 V. So (Qarlos bench 20300704: CDSOD323-T12C-DSL, 3 pF at 0 V,
        // against SD12T1G's 150 pF at no stated bias, called "not directly comparable"):
        //  * the substitute's value was taken at a bias at or ABOVE the original's — always so
        //    when the original's is 0 V — and already exceeds the original's: at the
        //    original's bias it is at least that large, so it is worse by at least that ratio.
        //    Judged as stated (the both-state rule: up to 2x WARN, beyond FAIL); a ratio of
        //    2x or less is only a lower bound, so the verdict is WARN with a note saying so.
        //  * the substitute's value was taken at a bias at or BELOW the original's (both
        //    stated) and is already no larger: at the original's bias it is smaller still —
        //    PASS.
        // Anything else stays UNVERIFIED as below. No C(V) model is used: only the sign of
        // the slope.
        const bool sub_bias_at_or_above =
            ovr && (*ovr <= 0.0 || (svr && *svr >= *ovr));
        const bool sub_bias_at_or_below = ovr && svr && *svr <= *ovr;
        const bool cj_bound_worse = cj_basis_differs && sub_bias_at_or_above && *sc > *oc;
        const bool cj_bound_pass = cj_basis_differs && sub_bias_at_or_below && *sc <= *oc;
        auto at_bias = [&](const std::optional<double>& v) {
            return v ? " at " + bias(*v) : std::string(" at no stated reverse voltage");
        };
        // A stated Cj(Vr) curve resolves the different bias first (owner curve rule
        // 2026-11-14): the substitute's curve read at the original's bias, else the original's
        // read at the substitute's stated bias — inside the measured range only, never
        // extrapolated, linear in Vr (curve_checks). The capacitances are then compared on that
        // common bias with the junction-capacitance spec; the bound rules below are only for
        // a pair no curve settles.
        std::optional<double> cj_s_at, cj_o_at;
        if (cj_basis_differs) {
            using curve_checks::read_at;
            if (const auto c = curve_checks::view_curve(cand, "cj_curve"))
                cj_s_at = read_at(*c, *ovr, Interp::Linear);
            if (!cj_s_at && svr)
                if (const auto c = curve_checks::view_curve(original, "cj_curve"))
                    cj_o_at = read_at(*c, *svr, Interp::Linear);
        }
        if (cj_s_at || cj_o_at) {
            const ParamSpec spec{"junction_capacitance", Dir::Lower, 2.0, std::nullopt, false,
                                 false, nullptr};
            const double o_c = cj_s_at ? *oc : *cj_o_at, s_c = cj_s_at ? *cj_s_at : *sc;
            const ParamOutcome cmp = compare_numeric(spec, o_c, s_c);
            const char* v = cmp.verdict;
            params.push_back({{"name", "junction_capacitance"}, {"verdict", v}});
            char rb[400];
            if (cj_s_at)
                std::snprintf(rb, sizeof rb,
                              "junction capacitance compared at the original's %s: the "
                              "substitute's Cj(Vr) curve reads %.4g pF there (its record states "
                              "%s%s) against the original's %s",
                              bias(*ovr).c_str(), *cj_s_at * 1e12, pf(*sc).c_str(),
                              at_bias(svr).c_str(), pf(*oc).c_str());
            else
                std::snprintf(rb, sizeof rb,
                              "junction capacitance compared at the substitute's %s: the "
                              "original's Cj(Vr) curve reads %.4g pF there (its record states "
                              "%s at %s) against the substitute's %s",
                              bias(*svr).c_str(), *cj_o_at * 1e12, pf(*oc).c_str(),
                              bias(*ovr).c_str(), pf(*sc).c_str());
            notes.push_back(rb + (cj_s_at ? curve_checks::inferred_units(cand, "cj_curve", "substitute's")
                                          : curve_checks::inferred_units(original, "cj_curve",
                                                                         "original's")));
            if (v == FAIL || v == WARN) {
                demote();
                penalty += opt.gate_weight *
                           (v == FAIL ? kVerdictFailPenalty : warn_penalty(cmp.warn_depth));
                ++worse;
                notes.push_back("junction capacitance " + pf(s_c) + " against the original's " +
                                pf(o_c) + " on a common bias — " + load);
            } else {
                note_direction(Dir::Lower, o_c, s_c);
            }
        } else if (cj_bound_worse) {
            const ParamSpec spec{"junction_capacitance", Dir::Lower, 2.0, std::nullopt, false,
                                 false, nullptr};
            const ParamOutcome cmp = compare_numeric(spec, oc, sc);
            const char* v = cmp.verdict;
            params.push_back({{"name", "junction_capacitance"}, {"verdict", v}});
            demote();
            penalty += opt.gate_weight *
                       (v == FAIL ? kVerdictFailPenalty : warn_penalty(cmp.warn_depth));
            ++worse;
            notes.push_back("junction capacitance " + pf(*sc) + at_bias(svr) +
                            " against the original's " + pf(*oc) + " at " + bias(*ovr) +
                            " — Cj only falls as reverse bias rises, so at the original's bias "
                            "the substitute's is at least " + pf(*sc) +
                            (v == FAIL ? std::string("")
                                       : std::string(" (a lower bound: it may be more than 2x)")) +
                            "; " + load);
        } else if (cj_bound_pass) {
            params.push_back({{"name", "junction_capacitance"}, {"verdict", PASS}});
            note_direction(Dir::Lower, oc, sc);
        } else if (cj_basis_differs) {
            params.push_back({{"name", "junction_capacitance"}, {"verdict", UNVERIFIED}});
            params.push_back({{"name", "junction_capacitance_basis"}, {"verdict", UNVERIFIED}});
            notes.push_back(
                svr ? "junction capacitance quoted at different reverse voltages (original " +
                          pf(*oc) + " at " + bias(*ovr) + ", substitute " + pf(*sc) + " at " +
                          bias(*svr) +
                          ") — not directly comparable, Cj falls with reverse bias; confirm the "
                          "substitute's Cj at the original's bias from its datasheet"
                    : "the original's junction capacitance is " + pf(*oc) + " at " + bias(*ovr) +
                          "; the substitute's record states " + pf(*sc) +
                          " at no reverse voltage — not directly comparable; confirm its Cj at "
                          "the original's bias from the datasheet");
        } else if (oc && sc) {
            const ParamSpec spec{"junction_capacitance", Dir::Lower, 2.0, std::nullopt, false,
                                 false, nullptr};
            const ParamOutcome cmp = compare_numeric(spec, oc, sc);
            const char* v = cmp.verdict;
            params.push_back({{"name", "junction_capacitance"}, {"verdict", v}});
            if (v == FAIL || v == WARN) {
                demote();
                penalty += opt.gate_weight *
                           (v == FAIL ? kVerdictFailPenalty : warn_penalty(cmp.warn_depth));
                ++worse;
                notes.push_back("junction capacitance " + pf(*sc) + " against the original's " +
                                pf(*oc) + " — " + load);
            } else {
                note_direction(Dir::Lower, oc, sc);
            }
        } else if (oc) {
            params.push_back({{"name", "junction_capacitance"}, {"verdict", UNVERIFIED}});
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            missing_required_data = true;
            notes.push_back("substitute's record carries no junction capacitance — the "
                            "original states " + pf(*oc) +
                            "; confirm it from the datasheet before this swap");
        } else if (sc) {
            params.push_back({{"name", "junction_capacitance"}, {"verdict", UNVERIFIED}});
        }
    }

    // ── measured curves (owner rule 2026-11-14: Kelvin compares ALL existing graphs) ──
    // Every curve both records state, judged point by point with its quantity's scalar spec on
    // the overlap (curve_checks::run, CrossRefCurveChecks.hpp). A WARN or FAIL is a regression
    // on that axis exactly as the scalar's would be: demoted, penalised (the depth-scaled warn),
    // counted worse. UNVERIFIED (different conditions, no overlap, an unreadable curve) adds the
    // row and its note only — no penalty, no cap. Every row carries its note: the x where the
    // curves are worst (or closest to the limit) and both values there.
    for (const auto& cr : curve_checks::run(cat, original, cand)) {
        const char* v = cr.result.verdict;
        params.push_back({{"name", cr.row}, {"verdict", v}});
        notes.push_back(cr.result.note);
        if (v == std::string(FAIL) || v == std::string(WARN)) {
            demote();
            penalty += opt.gate_weight * (v == std::string(FAIL) ? kVerdictFailPenalty
                                                                 : warn_penalty(cr.result.warn_depth));
            ++worse;
        }
    }

    // ── zener voltage (zener originals) ──────────────────────────────────────
    // Vz IS the zener. Vrrm above only compares the marking as a HigherBetter rating, so a
    // 24 V zener came back drop_in / upgrade for a 22 V (21.2 V) one and 11-20 V zeners
    // drop_in / recommended for a 9.1 V one (Qarlos 2026-11-08). Rule (conservative, for
    // the owner to confirm):
    //  * the original's window is its stated breakdown band [vz_min, vz_max]; with no band
    //    on record, its marked/resolved Vz (vrrm) is the only datum and the window is that
    //    point +/- kZenerPointTol (1 %, well inside the ~8-10 % E24 step, so it separates
    //    neighbouring voltages and absorbs a band midpoint against a marked nominal).
    //  * the substitute's Vz (its nominal, else its band midpoint — what vrrm carries)
    //    outside that window: a different zener voltage, REJECTED.
    //  * inside, and the substitute's guaranteed band also inside: PASS.
    //  * inside, but its guaranteed band reaches outside the original's: FAIL (caps at
    //    major_review) — the part is not guaranteed to regulate where the circuit was
    //    designed to.
    //  * inside, substitute states no band against an original that does: UNVERIFIED. Not a
    //    demotion: the ABT #488 decision treats an absent band as unknown, not a regression,
    //    and its note (above) already names the window going unchecked.
    //  * no Vz on either side: UNVERIFIED, and with no Vz on the substitute the grade is
    //    capped as missing required data.
    // web/src/crossref.js pre-filters the pool at exactly these edges (kZenerEdgeTol, kZenerPointTol).
    const bool zener = cat == "diode" && o_tech == "zener";
    bool zener_off_nominal = false;
    if (zener) {
        constexpr double kZenerEdgeTol = 1e-3;   // rounding slack on a stated band edge
        constexpr double kZenerPointTol = 0.01;  // window of a band-less original
        const auto oc = num(original, "vrrm"), olo = num(original, "vz_min_V"),
                   ohi = num(original, "vz_max_V");
        const auto sc = num(cand, "vrrm"), slo = num(cand, "vz_min_V"),
                   shi = num(cand, "vz_max_V");
        const bool o_band = olo && ohi && *ohi >= *olo;
        const bool s_band = slo && shi && *shi >= *slo;
        std::optional<double> wlo, whi;
        if (o_band) {
            wlo = *olo * (1.0 - kZenerEdgeTol);
            whi = *ohi * (1.0 + kZenerEdgeTol);
        } else if (oc) {
            wlo = *oc * (1.0 - kZenerPointTol);
            whi = *oc * (1.0 + kZenerPointTol);
        }
        std::optional<double> s_center = sc;
        if (!s_center && s_band) s_center = 0.5 * (*slo + *shi);
        auto band = [](double lo, double hi) { return volts(lo) + "-" + volts(hi) + " V"; };
        const std::string o_window = o_band ? band(*olo, *ohi)
                                     : oc   ? volts(*oc) + " V (no band on record)"
                                            : std::string();
        if (!wlo) {
            params.push_back({{"name", "zener_voltage"}, {"verdict", UNVERIFIED}});
        } else if (!s_center) {
            params.push_back({{"name", "zener_voltage"}, {"verdict", UNVERIFIED}});
            notes.push_back("substitute's record states no zener voltage — the original's is " +
                            o_window + "; confirm it from the datasheet before this swap");
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            missing_required_data = true;
        } else if (*s_center < *wlo || *s_center > *whi) {
            params.push_back({{"name", "zener_voltage"}, {"verdict", FAIL}});
            notes.push_back("zener voltage " + volts(*s_center) + " V is outside the original's " +
                            o_window + " — a different zener voltage, not a substitute");
            return reject("a different zener voltage");
        } else if (o_band && s_band && (*slo < *wlo || *shi > *whi)) {
            params.push_back({{"name", "zener_voltage"}, {"verdict", FAIL}});
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            ++worse;
            notes.push_back("the substitute guarantees Vz " + band(*slo, *shi) +
                            ", reaching outside the original's " + o_window +
                            " — not guaranteed to regulate where the circuit was designed to");
        } else if (o_band && !s_band) {
            params.push_back({{"name", "zener_voltage"}, {"verdict", UNVERIFIED}});
        } else {
            params.push_back({{"name", "zener_voltage"}, {"verdict", PASS}});
        }
        // A substitute whose Vz lies inside the original's window but away from its nominal
        // is a different marked voltage, and the verdicts above say nothing about it: a 6.0 V
        // 1N5233B sat at the top edge of BZV55-C5V6's 5.2-6.0 V window, 7 % above its 5.6 V,
        // with no word (Qarlos bench 20680025). Note only — the verdict and grade are those
        // decided above; an offset within kZenerPointTol is the same marking.
        if (wlo && s_center && oc && *oc > 0.0 &&
            std::fabs(*s_center / *oc - 1.0) > kZenerPointTol) {
            // Owner 2026-11-13: such a part also RANKS after every exact-nominal zener — the
            // resistor near-value tier (value_differs sort key in cross_reference), not a
            // penalty another term could outweigh. Its grade and direction stay those
            // decided above (zener_off_nominal does not take the resistor's grade cap).
            zener_off_nominal = true;
            notes.push_back("the substitute's Vz " + volts(*s_center) + " V is " +
                            signed_pct(100.0 * (*s_center / *oc - 1.0)) +
                            " from the original's " + volts(*oc) + " V, inside its " + o_window +
                            " window but a different nominal — check the circuit tolerates "
                            "the shift");
        }
    }

    // ── forward-current basis (multi-die diode packages) ─────────────────────
    // SAS diode.json: for a package declared part.dieConfiguration 'dual', every electrical
    // value is PER DIODE (per leg), never the package total; a record that declares no
    // configuration is, by the schema, one die. A rating is only comparable on the same
    // basis, and the record is the only admissible witness of the basis — never the MPN.
    // Rule (Qarlos 2026-11-08, run 4 seed 20292785: MBRF1560CT, 15 A whole-device, graded
    // if_avg 'pass' against VFT2060C's 10 A per leg):
    //  * both sides state their configuration, or neither does: compared as stated (both
    //    per diode by the schema's definition);
    //  * one side states a MULTI-die package and the other states none: the other side's
    //    figure cannot be shown to be per leg — catalogue imports routinely omit the field on
    //    dual parts and copy the vendor table's whole-device current — so if_avg is
    //    UNVERIFIED, the grade is capped as missing required data (the current rating went
    //    unweighed, exactly as an unstated surge waveform caps a TVS) and the direction is
    //    unknown — UNLESS the verdict is the same under every reading of the unstated basis
    //    (see `settled` below), in which case it is compared as stated. Decision for the
    //    owner to confirm: the cap is major_review, the same as a rating the substitute does
    //    not state at all.
    bool current_basis_blind = false;
    std::string current_basis_note;
    if (cat == "diode") {
        const std::string odc = str(original, "die_configuration"),
                          sdc = str(cand, "die_configuration");
        auto multi = [](const std::string& c) { return !c.empty() && c != "single"; };
        const auto oi = num(original, "if_avg"), si = num(cand, "if_avg");
        const bool cand_basis_unknown = multi(odc) && sdc.empty();
        const bool orig_basis_unknown = multi(sdc) && odc.empty();
        // An unstated basis only ever moves a figure DOWN when it is brought to per leg (a
        // whole-device figure divides among the legs), so the stated number is the most
        // generous per-leg reading of it. A verdict that holds under every reading is not
        // unknown and is kept: a substitute whose figure FAILS even read as per leg (1 A
        // against 10 A per leg) fails, and one that PASSES against the original's figure
        // read as per leg (the strictest requirement it can mean) passes.
        std::optional<ScoreResult> best;
        if (oi && si && (cand_basis_unknown || orig_basis_unknown))
            for (const auto& r : critical_ratings(cat))
                if (std::string(r.key) == "if_avg")
                    best = score_directional(oi, si, r.mode, r.warn_factor, r.gate_factor);
        const bool settled = best && ((cand_basis_unknown && best->verdict == FAIL) ||
                                      (orig_basis_unknown && best->verdict == PASS));
        if (best && !settled) {
            auto amps = [](double v) {
                char b[32];
                std::snprintf(b, sizeof b, "%g A", v);
                return std::string(b);
            };
            auto side = [&](const std::string& who, const std::string& dc, double a) {
                return dc.empty() ? who + "'s " + amps(a) + " (its record states no die "
                                                            "configuration, so per leg or "
                                                            "whole device is unknown)"
                                  : who + "'s " + amps(a) + " per diode (a '" + dc +
                                        "' package)";
            };
            current_basis_note =
                "forward current not comparable: the " + side("original", odc, *oi) +
                " against the " + side("substitute", sdc, *si) +
                " — confirm the per-leg I_F(AV) of both from their datasheets before this swap";
        }
    }

    // ── critical ratings (Vds / Vrrm / rated voltage / Id / If) ──────────────
    for (const auto& r : critical_ratings(cat)) {
        auto o = num(original, r.key), s = num(cand, r.key);
        if (!o && !s) {
            both_unstated(r.key);  // owner 2026-11-14
            continue;
        }
        if (!o && s && is_primary_rating(cat, r.key)) orig_unstated(r.key);
        if (!current_basis_note.empty() && std::string(r.key) == "if_avg") {
            params.push_back({{"name", r.key}, {"verdict", UNVERIFIED}});
            notes.push_back(current_basis_note);
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            missing_required_data = true;
            current_basis_blind = true;
            continue;
        }
        if (zener && std::string(r.key) == "vrrm") {
            // A zener's vrrm is its Vz, and a Vz is matched, not "more is better": as a
            // HigherBetter rating any Vz a hair below the original's (21.0 V inside a
            // 20.64-21.71 V window) WARNED. The zener rule above has already rejected every
            // Vz outside the original's window, so a stated pair here is a marking inside
            // it. No over-dimensioning penalty and no direction either way.
            params.push_back({{"name", r.key}, {"verdict", (o && s) ? PASS : UNVERIFIED}});
            if (r.hard && !o && s) {
                direction_blind = true;
                blind_axes.push_back(param_label(r.key));
            }
            continue;
        }
        auto v = score_directional(o, s, r.mode, r.warn_factor, r.gate_factor);
        params.push_back({{"name", r.key}, {"verdict", v.verdict}});
        // A primary CURRENT rating stated by one record only (owner 2026-11-13, Y1). The
        // voltage ratings are hard (a substitute missing one rejects; an original missing one
        // is direction_blind below); the current ratings were silently UNVERIFIED, so a dual
        // whose dies state no Id (CSD88584Q5DC) came back drop_in either way round. Now:
        // UNVERIFIED, demoted, capped at minor_review (primary_unverified), and a note naming
        // the side that states it. No penalty and no direction change.
        if ((r.key == std::string("id") || r.key == std::string("if_avg") ||
             r.key == std::string("ic")) &&
            (o.has_value() != s.has_value())) {
            static const std::map<std::string, const char*> kCur = {
                {"id", "continuous drain current (Id)"},
                {"if_avg", "average forward current (If(AV))"},
                {"ic", "continuous collector current (Ic)"}};
            const char* what = kCur.at(r.key);
            char b[96];
            std::snprintf(b, sizeof b, "%g A", o ? *o : *s);
            current_rating_one_sided = true;
            demote();
            notes.push_back(o ? std::string("the original states a ") + what + " of " + b +
                                    "; the substitute's record states none — whether it "
                                    "carries the original's current is unknown; confirm its "
                                    "rating from the datasheet before this swap"
                              : std::string("the substitute states a ") + what + " of " + b +
                                    "; the original's record states none, so it could not be "
                                    "weighed against the original's requirement — confirm the "
                                    "original's rating from its datasheet before this swap");
        }
        // A shortfall that is not a rejection must say so (Qarlos r13 seed 25078643: the
        // 600 V SiHL033N60SF for the 650 V FCH023N65S3L4 carried a 'warn' Vds row and no
        // word about it, so it read as a mild review). The verdict, penalty and grade are
        // unchanged; this is the note. A protection diode's standoff (its vrrm) is skipped:
        // the standoff-band rule above already notes every standoff below the original's.
        if ((v.verdict == WARN || (v.verdict == FAIL && !(r.hard && o))) && o && s &&
            !(protection && std::string(r.key) == "vrrm")) {
            static const std::map<std::string, std::pair<const char*, const char*>> kWhat = {
                {"vds", {"drain-source voltage rating", "V"}},
                {"id", {"continuous drain current rating", "A"}},
                {"vrrm", {"reverse voltage rating", "V"}},
                {"if_avg", {"average forward current rating", "A"}},
                {"voltage", {"rated voltage", "V"}},
                {"vces", {"collector-emitter voltage rating", "V"}},
                {"ic", {"continuous collector current rating", "A"}}};
            const auto it = kWhat.find(r.key);
            const char* what = it != kWhat.end() ? it->second.first : r.key;
            const char* unit = it != kWhat.end() ? it->second.second : "";
            char b[320];
            std::snprintf(b, sizeof b, "%s %g %s against the original's %g %s (%s)", what, *s,
                          unit, *o, unit, signed_pct(100.0 * (*s / *o - 1.0)).c_str());
            const bool voltage = std::string(unit) == "V";
            notes.push_back(std::string(b) +
                            (voltage ? " — below the original's rating: less margin to the "
                                       "circuit's peak voltage and its transients; confirm the "
                                       "application's worst-case voltage is covered"
                                     : " — below the original's rating: less margin at the "
                                       "circuit's current; confirm the application's "
                                       "worst-case current and thermal margin are covered"));
        }
        if (v.verdict == FAIL) {
            if (r.hard && o) return reject("a critical rating falls far below the original");
            demote();
            penalty += opt.gate_weight * v.penalty;
        } else if (v.verdict == WARN) {
            demote();
            penalty += opt.gate_weight * v.penalty;
        } else {
            penalty += opt.overdim_weight * over_dimensioning_penalty(o, s, 1.0);
            // A protection diode's standoff has its own above-band rule (standoff_band).
            if (v.verdict == PASS && r.mode == Mode::HigherBetter &&
                !(protection && std::string(r.key) == "vrrm"))
                gross_over_rating(r.key, o, s);
        }
        if (v.verdict == WARN || v.verdict == FAIL)
            ++worse;  // a shortfall verdict is a regression, inside the neutral band or not
        else if (!(protection && std::string(r.key) == "vrrm"))
            // A protection diode's standoff is the rail it guards, not a margin: a 12 V
            // standoff part on a 5.5 V line clamps later, which is no upgrade (ABT #2034).
            note_direction(r.mode == Mode::HigherBetter ? Dir::Higher : Dir::Lower, o, s);
        if (r.hard && !o && s) {
            direction_blind = true;
            blind_axes.push_back(param_label(r.key));
        }
    }

    // ── diode package topology (die configuration / internal connection) ─────
    // How many die a package holds and how they are bonded inside it fix its pinout and the
    // circuit it forms: a common-cathode centre-tap rectifier is not a common-anode one, a
    // series pair (BAV99) is neither, and a single diode is not a dual. Rule (coordinator,
    // Qarlos 2026-11-08 round 2), applied to each field separately, from the record only —
    // never inferred from the MPN or the package:
    //  * both sides state it and the values differ: FAIL, the candidate is rejected, and the
    //    note names both — the topology is not interchangeable;
    //  * one side states it and the other does not: UNVERIFIED with a note, and the grade is
    //    capped below drop_in (minor_review). It adds no further penalty or cap where the
    //    forward-current basis rule above has already capped the row for the same silence;
    //  * neither side states it: silent (the overwhelming majority of the catalogue).
    // Decision for the owner to confirm: a stated 'single' against an unstated configuration
    // is UNVERIFIED too, although the schema reads an absent dieConfiguration as one die.
    //
    // MOSFETs (owner 2026-11-13): die configuration (single / dual / complementary) is graded
    // by the same rule. A dual's ratings are per die, a complementary part holds an n- and a
    // p-channel die, and either one has a different pinout from a single die — so a
    // single-die substitute for a dual original is not a drop_in. Single-die MOSFET records
    // state no configuration, so that case lands in the one-sided branch: UNVERIFIED, capped
    // at minor_review.
    bool topology_unverified = false;
    if (cat == "diode" || cat == "mosfet") {
        struct Field {
            const char* key;
            const char* label;
        };
        std::vector<Field> fields{{"die_configuration", "die configuration"}};
        // MOSFET internal connection (halfBridge / independent / commonDrain), owner
        // 2026-11-13 V2: the same rule as the diode one, field for field.
        fields.push_back({"internal_connection", "internal connection"});
        for (const Field f : fields) {
            const std::string o = str(original, f.key), s = str(cand, f.key);
            if (o.empty() && s.empty()) continue;
            if (!o.empty() && !s.empty()) {
                if (o == s) {
                    params.push_back({{"name", f.key}, {"verdict", PASS}});
                    continue;
                }
                params.push_back({{"name", f.key}, {"verdict", FAIL}});
                notes.push_back(std::string(f.label) + " differs: the original is '" + o +
                                "', the substitute '" + s +
                                "' — a different package topology and pinout, not a substitute");
                return reject(cat == "diode" ? "a different diode package topology"
                              : std::string(f.key) == "internal_connection"
                                  ? "a different MOSFET internal connection"
                                  : "a different MOSFET die configuration");
            }
            params.push_back({{"name", f.key}, {"verdict", UNVERIFIED}});
            notes.push_back(o.empty()
                                ? "the substitute's record states " + std::string(f.label) + " '" +
                                      s + "' and the original's states none — confirm the "
                                      "original's package topology and pinout match"
                                : "the original's record states " + std::string(f.label) + " '" +
                                      o + "' and the substitute's states none — confirm the "
                                      "substitute's package topology and pinout from its "
                                      "datasheet");
            if (!current_basis_blind && !topology_unverified) {
                demote();
                penalty += opt.gate_weight * kVerdictWarnPenalty;
            }
            topology_unverified = true;
        }
    }
    // Diode count (owner 2026-11-13, AA1; SAS bcc7a93 electrical.numberOfElements /
    // dies[]): how many diodes the package holds is an identity like the die configuration —
    // an 8-line ESD array is not a 4-line one, whatever their per-line ratings. Graded only
    // when at least one record STATES a count; a stated 'dual' without one is 2 and a stated
    // 'single' is 1, because SAS defines them so (an absent configuration is not read as
    // one). Same rule as above: both known and different -> rejected; one side known ->
    // UNVERIFIED, capped as the die configuration is (no second penalty where that row, or
    // the forward-current basis, already capped for the same silence).
    if (cat == "diode" && (num(original, "number_of_elements") || num(cand, "number_of_elements"))) {
        auto count_of = [](const json& p) -> std::optional<double> {
            if (auto n = num(p, "number_of_elements")) return n;
            const std::string dc = str(p, "die_configuration");
            if (dc == "dual") return 2.0;
            if (dc == "single") return 1.0;
            return std::nullopt;
        };
        const auto o = count_of(original), s = count_of(cand);
        auto n_str = [](double v) {
            char b[32];
            std::snprintf(b, sizeof b, "%g", v);
            return std::string(b);
        };
        if (o && s) {
            if (*o == *s) {
                params.push_back({{"name", "number_of_elements"}, {"verdict", PASS}});
            } else {
                params.push_back({{"name", "number_of_elements"}, {"verdict", FAIL}});
                notes.push_back("number of diodes differs: the original holds " + n_str(*o) +
                                ", the substitute " + n_str(*s) +
                                " — a different package and pinout, not a substitute");
                return reject("a different number of diodes in the package");
            }
        } else {
            params.push_back({{"name", "number_of_elements"}, {"verdict", UNVERIFIED}});
            notes.push_back(o ? "the original's record states " + n_str(*o) +
                                    " diodes in the package and the substitute's states no "
                                    "count — confirm the substitute's diode count and pinout "
                                    "from its datasheet"
                              : "the substitute's record states " + n_str(*s) +
                                    " diodes in the package and the original's states no "
                                    "count — confirm the original's diode count and pinout match");
            if (!current_basis_blind && !topology_unverified) {
                demote();
                penalty += opt.gate_weight * kVerdictWarnPenalty;
            }
            topology_unverified = true;
        }
    }

    // ── temperature grade tie-break when the original states none ───────────
    if (cat == "capacitor" && !num(original, "temp_max_C")) {
        if (const auto t = num(cand, "temp_max_C"); t && *t < 125.0)
            penalty += kUnknownTempGradePenaltyPerC * (125.0 - *t);
    }

    // ── operating-point requirements (the circuit, not the original) ────────
    for (const auto& [key, need] : opt.requirements) {
        const auto s = num(cand, key.c_str());
        const std::string label = param_label(key);
        char buf[160];
        if (!s) {
            params.push_back({{"name", "required_" + key}, {"verdict", WARN}});
            std::snprintf(buf, sizeof buf, "%s not stated — cannot confirm it meets the circuit's %g",
                          label.c_str(), need);
            notes.emplace_back(buf);
            missing_required_data = true;
            demote();
            penalty += opt.gate_weight * kVerdictWarnPenalty;
            continue;
        }
        if (*s < need) {
            params.push_back({{"name", "required_" + key}, {"verdict", FAIL}});
            std::snprintf(buf, sizeof buf, "%s %g is below the circuit's requirement of %g",
                          label.c_str(), *s, need);
            notes.emplace_back(buf);
            return reject("does not meet the circuit's operating-point requirement");
        }
        params.push_back({{"name", "required_" + key}, {"verdict", PASS}});
        penalty += opt.overdim_weight * over_dimensioning_penalty(need, s, 1.0);
    }

    // ── mount type ───────────────────────────────────────────────────────────
    const std::string o_pkg = str(original, "package").empty() ? str(original, "case_code")
                                                               : str(original, "package");
    const std::string s_pkg =
        str(cand, "package").empty() ? str(cand, "case_code") : str(cand, "package");
    // Mount type: an SMD part cannot stand in for a leaded one. Skipped for
    // categories whose package strings vary too much by series to classify.
    // This is the ONE mount comparison for every family, connectors included. A
    // connector's board attach is CONAS mechanical.mountingStyle (smt / tht / pressFit /
    // skedd / panel / cable), which its spec carries under `mounting`; every other
    // family's explicit assembly type travels as `mount`. compare_mount classifies the
    // connector classes too (normalize_mount), so a press-fit part against an SMT one is
    // Incompatible here rather than in a parallel connector-only key that pushed a second
    // 'mounting' verdict and a second note for the same pair.
    const bool connector = cat == "connector";
    const char* mount_key = connector ? "mounting" : "mount";
    const std::string o_mt = str(original, mount_key), s_mt = str(cand, mount_key);
    // Magnetics and chip beads are off the generic gate because their case strings ("No
    // Tabs", "With Tabs", "Nonstandard", "2.5x2.0") do not classify. That skip also threw away
    // the mount the record STATES (MAS mechanical.mounting / assemblyType): a THT axial TDK
    // B78108S1562K000 came back minor_review for an SMT M83446/10-95FJ with no mount note
    // (Qarlos 2026-11-09, seed 22146231). Rule: for these two categories the gate runs on the
    // EXPLICIT mounts alone — package strings are never classified — with the shared outcome:
    // both stated and different rejects, one stated and the other not caps the grade.
    const bool explicit_mount_only = cat == "magnetic" || cat == "chipBead";
    const std::string o_mpkg = explicit_mount_only ? std::string() : o_pkg;
    const std::string s_mpkg = explicit_mount_only ? std::string() : s_pkg;
    // Name what each side's class was read from — its EXPLICIT assembly type when the
    // record states one, its package string otherwise (mount_class's order). Always
    // quoting the packages printed a half-empty "mount type incompatible:  -> 8x8x10"
    // for the many records that state a mount type but carry no package string.
    auto mount_src = [](const std::string& mt, const std::string& pkg) {
        return !normalize_mount(mt).empty() ? mt : pkg;
    };
    // For every other family the mount is part of the physical fit and goes with
    // check_footprint. A connector's board attach is an IDENTITY of the part (owner's rule,
    // 2026-11-09): it is compared even when the caller switches the footprint check off.
    if (connector || opt.check_footprint) {
        const MountMatch mount_match = (mount_gate_applies(cat) || explicit_mount_only)
                                           ? compare_mount(o_mt, s_mt, o_mpkg, s_mpkg)
                                           : MountMatch::Same;
        if (mount_match == MountMatch::Incompatible) {
            params.push_back({{"name", "mounting"}, {"verdict", FAIL}});
            if (connector) {
                // A press-fit part needs a press and plated holes sized for it, an SMT one a
                // reflow pass on pads: a different board process, not a parameter.
                notes.push_back("board mount differs: " + mount_src(o_mt, o_pkg) + " -> " +
                                mount_src(s_mt, s_pkg) +
                                " — a different board-attach process, not a drop-in");
                return reject("board mount differs (a different board-attach process)");
            }
            notes.push_back("mount type incompatible: " + mount_src(o_mt, o_mpkg) + " -> " +
                            mount_src(s_mt, s_mpkg));
            return reject("mount type incompatible (a different board-mount process)");
        }
        const std::string o_cls = mount_class(o_mt, o_mpkg), s_cls = mount_class(s_mt, s_mpkg);
        // A connector's board attach is one of its identity rows in the verdict table, so a
        // match is reported, not left blank (the other families keep a silent match).
        if (connector && mount_match == MountMatch::Same && !o_cls.empty() && !s_cls.empty())
            params.push_back({{"name", "mounting"}, {"verdict", PASS}});
        if (connector && mount_match == MountMatch::Unknown &&
            (!o_cls.empty() || !s_cls.empty())) {
            // One record states its board attach and the other does not: whether it goes on
            // the same board process is unknown, so the swap cannot be called a drop-in —
            // WT16PRD8CD (no mountingStyle, no pitch) came back drop_in for an SMT Samtec
            // original. Capped like an unverified footprint (minor_review).
            params.push_back({{"name", "mounting"}, {"verdict", UNVERIFIED}});
            mount_unverified = true;
            // A gap toward the several-gaps cap only when the original states the board
            // mount (owner 2026-11-13, W1).
            if (!o_cls.empty()) connector_identity_gaps.push_back("board mount");
            notes.push_back(!o_cls.empty()
                                ? "the original is " + mount_src(o_mt, o_pkg) +
                                      "-mounted; the substitute's record states no board mount — "
                                      "confirm it uses the same board-attach process"
                                : "the original's record states no board mount; the substitute "
                                  "is " + mount_src(s_mt, s_pkg) + "-mounted — confirm it uses the same "
                                  "board-attach process");
        } else if (mount_match == MountMatch::Unknown && (!o_cls.empty() || !s_cls.empty())) {
            // One record states its mount and the other does not: not on record is not
            // compatible — SMD vs through-hole is a different board process. Capped like
            // an unverified footprint. (Neither stating one leaves the axis silent, the
            // same known-vs-absent rule the height axis follows.)
            params.push_back({{"name", "mounting"}, {"verdict", UNVERIFIED}});
            mount_unverified = true;
            // Report what the record SAYS: a stated mount Kelvin does not classify (CAS
            // "Screw Type", "Chassis Mount") is quoted as stated, never called "not stated" —
            // the note contradicted the record when Snap-In was unclassified (Qarlos 2026-11-09).
            auto said = [](const std::string& cls, const std::string& mt) {
                if (!cls.empty()) return cls;
                if (!mt.empty()) return "\"" + mt + "\" (not classifiable as SMD or through-hole)";
                return std::string("not stated");
            };
            notes.push_back("mount type not established (original: " + said(o_cls, o_mt) +
                            ", substitute: " + said(s_cls, s_mt) +
                            ") — confirm SMD vs through-hole from the datasheets");
        }
    }

    // ── leaded form: axial vs radial (capacitors) ───────────────────────────
    // A leaded body's form is its land pattern: an axial part lies along the board with a
    // lead out of each end (holes a body length plus bends apart), a radial one stands with
    // both leads out of one face (holes at its lead spacing). Neither fits the other's holes,
    // whatever the two bodies measure. KEMET C322C270JAG5TA (radial, 5.08 mm) took axial
    // A330J15C0GL5TAA parts with no form-factor note, and the axial M39003/01-2751/PR took
    // radial molded T378 tantalums graded "one case size larger" (Qarlos 2026-11-09, seeds
    // 21517857 / 20994212). Rule (land identity, the connector-orientation treatment):
    //   * both records state a form and it differs -> FAIL with a note (a different land);
    //   * one states a form and the other does not, and the other is not an established SMD
    //     part (that mismatch is the mount gate's) -> UNVERIFIED, noted, capped below drop_in;
    //   * neither states one -> silent (identical for every candidate).
    bool lead_form_unverified = false;
    if (cat == "capacitor" && opt.check_footprint) {
        const std::string o_lf = str(original, "lead_form"), s_lf = str(cand, "lead_form");
        if (!o_lf.empty() && !s_lf.empty()) {
            if (o_lf != s_lf) {
                params.push_back({{"name", "lead_form"}, {"verdict", FAIL}});
                demote();
                penalty += opt.footprint_weight * kEstablishedFootprintMismatchPenalty;
                notes.push_back("leaded form differs: the original is " + o_lf +
                                "-leaded, the substitute " + s_lf +
                                "-leaded — a different land pattern (hole positions), not a "
                                "drop-in");
            } else {
                params.push_back({{"name", "lead_form"}, {"verdict", PASS}});
            }
        } else if (!o_lf.empty() || !s_lf.empty()) {
            const bool other_smd = mount_class(o_lf.empty() ? o_mt : s_mt,
                                               o_lf.empty() ? o_pkg : s_pkg) == "smd";
            if (!other_smd) {
                lead_form_unverified = true;
                params.push_back({{"name", "lead_form"}, {"verdict", UNVERIFIED}});
                demote();
                notes.push_back(!o_lf.empty()
                                    ? "the original is " + o_lf +
                                          "-leaded; the substitute's record states no axial / "
                                          "radial form — confirm its lead form from the datasheet"
                                    : "the substitute is " + s_lf +
                                          "-leaded; the original's record states no axial / "
                                          "radial form — confirm both lead forms match");
            }
        }
    }

    // ── physical fit ─────────────────────────────────────────────────────────
    if (opt.check_footprint) {
        auto o_dims = dims_of(original, cat), s_dims = dims_of(cand, cat);
        // The case identities every case-kept gate below compares (series-aware for capacitors).
        auto case_ids = case_identities_of(cat, original, cand);
        // Capacitor case codes from different systems that resolve to the same body (owner
        // 2026-11-13, W2): when both bodies resolve (drawn, or from the series' own table) and
        // agree within kSameChipCodeBodySpread on both axes, orientation-agnostic, the case is
        // the same and the footprint is decided on the body. CWR06FC475KC/HR ("D", the CWR06
        // table's 3.81 x 2.54 mm) against CWR09FC475KR ("1510", drawn 3.81 x 2.54 mm) was a
        // "different_case" WARN naming two identical bodies. Chip bodies only: a leaded body's
        // land is its lead pitch, never its outline (capacitor_case_side letter_ok).
        if (cat == "capacitor" && case_ids.first != case_ids.second && o_dims && s_dims &&
            has_land(*o_dims) && has_land(*s_dims) && capacitor_case_side(original).letter_ok &&
            capacitor_case_side(cand).letter_ok) {
            const LandCompare lc = compare_land(*o_dims, *s_dims);
            if (lc.overflow <= kSameChipCodeBodySpread && lc.lin >= 1.0 - kSameChipCodeBodySpread)
                case_ids.second = case_ids.first;
        }
        // A part with NO dimensions must not be offered as a substitute for an
        // original whose footprint we DO know: its fit can never be verified. For
        // families where the footprint is the mechanical fit and the case code is
        // not a reliable size (magnetics, chip beads), such a substitute is
        // EXCLUDED — rejected and flagged as a data gap — rather than guessed at.
        // The real remedy is to backfill its dimensions (datasheet Seeker); until
        // then it is not a substitute anyone can trust. (Other families resolve
        // dimensions reliably from their EIA/JEDEC case code, so this does not touch
        // them. And when the ORIGINAL itself has no footprint there is nothing to
        // fit to, so a dimensionless-vs-dimensionless compare — e.g. a part against
        // itself — is left to the electrical verdicts, never rejected here. The same
        // goes for an original that states only one land axis: one axis is not a
        // footprint to fit to, so it grades the substitute rather than excluding it.)
        // Both sides are tested on a COMPLETE land, which is what "has a footprint"
        // meant before a half-stated drawing could reach here at all: for a custom
        // land pattern one axis verifies nothing, so this stays exactly as strict.
        if ((cat == "magnetic" || cat == "chipBead") && o_dims && has_land(*o_dims) &&
            (!s_dims || !has_land(*s_dims))) {
            out["missing_dimensions"] = true;  // partial/incomplete catalogue record
            params.push_back({{"name", "footprint"}, {"verdict", UNVERIFIED}});
            notes.push_back(
                "no complete land pattern on record — footprint fit cannot be verified; excluded "
                "from cross-reference until its dimensions are added (catalogue data gap)");
            return reject("no dimensional data — cannot verify footprint fit", true);
        }
        // strict_case: a true drop-in KEEPS the footprint. Only a same-size
        // substitute (the case kept) scores 0 on footprint and ranks first; a
        // materially smaller body is a different land pattern (different pinout/pads
        // for an IC package, different pad geometry for a chip) — a footprint change
        // (FootprintTier::Smaller) that must be reviewed, never a clean drop-in.
        // Applied to every family (a catalogue audit found ~3k "drop_in"s across a
        // changed package — e.g. a MOSFET QFN 8x8 -> QFN 6x5). Chip passives lose
        // the old "smaller frees board space" right-sizing by deliberate choice.
        const bool strict_case = true;
        // A device-class conflict decides the footprint, and the body outline must not
        // be allowed to overrule it: an EXB-V8V array and a discrete 1206 share a
        // 3.2 x 1.6 mm outline, so the size compare below would report "fits" for a
        // part that lands on two pads out of eight. Claiming the slot first is what
        // stops that — every compare below is guarded on it still being free. A
        // four-terminal bridge module is the same story one rung coarser: the
        // case-code gate below would have called SMC -> SIP-4 an ordinary
        // "different_case" WARN, when the pads cannot be made to match at all.
        if (resistor_class_conflict || diode_class_conflict) {
            out["footprint"] = "different_land_pattern";
            params.push_back({{"name", "footprint"}, {"verdict", FAIL}});
            penalty += opt.footprint_weight * kVerdictFailPenalty;
        }
        // The connector equivalent, and for the same reason: the pitch decides the land
        // pattern and there is no body outline to weigh against it. Say WHERE the last
        // contact ends up — "pitch differs" is abstract, "pin 13 sits 6.5 mm off its pad"
        // is the reason the board does not accept the part.
        auto o_pitch = num(original, "pitch_mm"), s_pitch = num(cand, "pitch_mm");
        // A different row count is the same failure (connector_pitch_fit): reported here even
        // when a record states no pitch, since the arrangement alone settles it.
        if (connector_pitch.rows_differ && !out.contains("footprint")) {
            out["footprint"] = "different_land_pattern";
            params.push_back({{"name", "footprint"}, {"verdict", FAIL}});
            penalty += opt.footprint_weight * kVerdictFailPenalty;
            demote();
            notes.push_back("contact rows differ: " +
                            std::to_string(static_cast<long>(connector_pitch.o_rows)) + " -> " +
                            std::to_string(static_cast<long>(connector_pitch.s_rows)) +
                            " rows — a different pin field, a different land pattern, not a "
                            "drop-in");
        }
        if (connector_pitch_conflict && o_pitch && s_pitch && !out.contains("footprint")) {
            out["footprint"] = "different_land_pattern";
            params.push_back({{"name", "footprint"}, {"verdict", FAIL}});
            penalty += opt.footprint_weight * kVerdictFailPenalty;
            std::string note = "pitch differs: " + mm(*o_pitch * 1e-3) + " -> " +
                               mm(*s_pitch * 1e-3) + " mm (" +
                               signed_pct(100.0 * (*s_pitch / *o_pitch - 1.0)) + ")";
            // The row the verdict was taken over (connector_pitch_fit): the per-row count
            // when a record states the rows, else the stated position count.
            std::optional<double> n;
            if (connector_pitch.per_row > 0.0) n = connector_pitch.per_row;
            else n = num(original, "positions");
            // Without a stated row count the verdict rests on the adjacent contact alone
            // (connector_pitch_fit); the end-pin figure is then for a single row, said so.
            if (n && *n >= 2.0)
                note += (connector_pitch.per_row > 0.0 ? "; over " : "; in a single row of ") +
                        std::to_string(static_cast<long>(*n)) +
                        (connector_pitch.per_row > 0.0 ? " positions per row" : " positions") +
                        " the last contact sits " +
                        mm(std::fabs(*s_pitch - *o_pitch) * (*n - 1.0) * 1e-3) +
                        " mm off its pad";
            notes.push_back(note + " — a different land pattern, not a drop-in");
        }
        // "Case kept" gate for the families whose CASE CODE is the footprint (chip
        // passives + IC packages, i.e. everything except magnetics/chip beads whose
        // codes are unreliable). A true drop-in keeps the package; a DIFFERENT case
        // is a different land pattern (pinout/pads), so it is a substitute to review,
        // not a drop-in — and this is caught even when neither part's exact body
        // dimensions resolve (DO-214AB, SOD-128, QFN NxN … are often not in the
        // dimension tables). Magnetics/chip beads fall through to the SIZE check.
        // Magnetics and chip beads take part only when BOTH case codes decode as chip size
        // codes and no reading of the two can be the same size (magnetic_case_codes_differ):
        // a stated "0402" against "0302 (0804 Metric)" is a different land however either is
        // read, and LQW04AN47NH20D came back 'upgrade' for CWW0402-47RGS with no package note
        // (Qarlos 2026-11-09, seed 20784754). Their undecodable strings still fall through to
        // the SIZE check.
        // A LEADED pair whose records both state the lead pitch, and the same one: the holes
        // are the land (see "lead pitch" below), so the case STRINGS do not decide it. WIMA
        // FKP2C016801D00HO00 ("PCM 5", 7.2 x 4.5 x 6 mm, 5 mm pitch) took KEMET
        // PFR5682G63J11L4Bulk ("7.2x6x4.5mm", the same box, 5 mm pitch) as "different
        // package/case — different land pattern" (Qarlos bench seed 20575296). The body
        // compare below still decides clearance and keep-out; a different lead pitch is
        // still a different land, and the lead-pitch rule below (FAIL, "lead pitch differs")
        // says so — the case-string gate must not pre-empt it with its WARN when both
        // pitches are on record.
        constexpr double kLeadPitchTolM = 0.1e-3;
        const auto o_lp = num(original, "lead_pitch_m"), s_lp = num(cand, "lead_pitch_m");
        const std::string o_mcls = mount_class(o_mt, o_pkg), s_mcls = mount_class(s_mt, s_pkg);
        const bool leaded_pair = cat != "connector" && (o_mcls == "leaded" || s_mcls == "leaded");
        const bool pitches_decide = leaded_pair && o_lp && s_lp;
        if (!out.contains("footprint") && !pitches_decide) {
            const std::string oc = str(original, "case_code"), sc = str(cand, "case_code");
            const bool magnetic_case = cat == "magnetic" || cat == "chipBead";
            // A generic style string ("4-SMD, No Lead", "SMD", "Radial") names a pad count,
            // not a land, so it can no more establish a DIFFERENT land than the same one
            // (generic_package_names_no_land): ECS-TXO-2016-33-120-TR "4-SMD, No Lead" took
            // ATX-H13-E-12.000MHZ-F25 "2016 4-SMD" as "a different land pattern" (Qarlos r24
            // seed 31048196). The fit is left to the body compare / UNVERIFIED below.
            const bool generic_side =
                generic_package_names_no_land(oc) || generic_package_names_no_land(sc);
            // Two timing case strings naming the same standard body in different notation
            // (timing_case_body_code: "2520 4-SMD" / "IQXO-951 2520", r24 seed 30943467) have
            // not been shown to differ; whether the pads match is left to the body compare /
            // UNVERIFIED below, while a 3225 or a 2016 against a 2520 stays a different case.
            std::optional<std::string> o_body, s_body;
            if (cat == "timeBase") {
                o_body = timing_case_body_code(oc, num(original, "length_m"),
                                               num(original, "width_m"));
                s_body = timing_case_body_code(sc, num(cand, "length_m"), num(cand, "width_m"));
            }
            // A record whose case string is a vendor package code naming no body ("CFPS-39")
            // but which STATES a length and width agreeing with the body the other side's
            // case names is the same body too (Qarlos r31: ASEM1-14.31818MHZ-LC-T "3225 4-SMD"
            // against IQD 831025493 "CFPS-39", stated 3.2 x 2.5 mm, was called a different land
            // pattern with "3.2 x 2.5 mm against 3.2 x 2.5 mm" in the same note).
            std::string dims_named_side;  // which side's body comes from its stated dims
            auto dims_name_body = [&](const std::string& body, const std::string& case_str,
                                      const json& p) {
                return timing_case_body_tokens(case_str).empty() &&
                       timing_body_agrees(body, num(p, "length_m"), num(p, "width_m"));
            };
            if (cat == "timeBase" && o_body && !s_body && dims_name_body(*o_body, sc, cand)) {
                s_body = o_body;
                dims_named_side = "substitute";
            } else if (cat == "timeBase" && s_body && !o_body &&
                       dims_name_body(*s_body, oc, original)) {
                o_body = s_body;
                dims_named_side = "original";
            }
            const bool same_timing_body = o_body && s_body && *o_body == *s_body;
            if (magnetic_case ? magnetic_case_codes_differ(oc, sc)
                              : (!oc.empty() && !sc.empty() &&
                                 case_ids.first != case_ids.second && !generic_side &&
                                 !same_timing_body)) {
                out["footprint"] = "different_case";
                params.push_back({{"name", "footprint"}, {"verdict", WARN}});
                demote();
                // An ESTABLISHED land-pattern change, so it sits above the
                // unverified rung of the footprint ladder — never below it.
                penalty += opt.footprint_weight * kEstablishedFootprintMismatchPenalty;
                // Name the two bodies when both resolve, so the note carries the reason and
                // not just two strings: CWR06DC155JCA's case "A" is CWR06's 2.54 x 1.27 mm,
                // not the EIA 3216 (owner 2026-11-13, r16 26859036).
                std::string bodies;
                if (o_dims && s_dims && has_land(*o_dims) && has_land(*s_dims)) {
                    bodies = " — " + land_mm(*o_dims) + " against " + land_mm(*s_dims);
                    // Say where a size the record does not state comes from (Qarlos bench
                    // 20680025: CWR06FH335KCB's 5.08 x 1.27 mm is its series' case C).
                    std::vector<std::string> src;
                    if (o_dims->land_from_table && !o_dims->land_source.empty())
                        src.push_back("the original's from " + o_dims->land_source);
                    if (s_dims->land_from_table && !s_dims->land_source.empty())
                        src.push_back("the substitute's from " + s_dims->land_source);
                    if (!src.empty())
                        bodies += " (" + src[0] + (src.size() > 1 ? "; " + src[1] : "") +
                                  " — a table size, not one the record states)";
                }
                notes.push_back("different package/case (" + oc + " -> " + sc + ")" + bodies +
                                " — different land pattern; a substitute to re-lay-out, not a "
                                "drop-in (verify pads/pinout)");
            } else if (!magnetic_case && same_timing_body && case_ids.first != case_ids.second) {
                if (dims_named_side.empty())
                    notes.push_back("both records name the " + *o_body + " body ('" + oc +
                                    "' / '" + sc + "') in different notation — the same body "
                                    "size; the pad layout is not established by the case "
                                    "strings");
                else
                    notes.push_back(
                        "the " + std::string(dims_named_side == "substitute" ? "original" : "substitute") +
                        "'s case '" + (dims_named_side == "substitute" ? oc : sc) + "' names the " +
                        *o_body + " body and the " + dims_named_side + "'s record states that body (" +
                        (dims_named_side == "substitute" ? sc : oc) +
                        ") — the same body size; the pad layout is not established by the case "
                        "strings");
            }
        }
        // ── lead pitch: the land of a LEADED body ───────────────────────────
        // A through-hole part lands on its holes, and where they sit is the lead pitch,
        // not the body outline: a radial film capacitor's PCM 15 IS its land pattern, its
        // 18 x 7 mm box only the keep-out. The body compare below therefore cannot call a
        // leaded part's land kept on its own — it graded R60II34705030K (no pitch on
        // record) 'fits' against MKP4D034704D00MSSD (PCM 15, two-hole land) on the box
        // size alone (Qarlos 2026-11-08, seed 20276947). Rule, for every family except
        // connectors (whose pitch is their own axis above), whenever either side is leaded:
        //   both lead pitches stated, more than kLeadPitchTolM apart -> a different land
        //     pattern (FAIL, a re-layout), exactly like the connector pitch;
        //   both stated and equal                                -> the holes are kept;
        //     the body compare still decides clearance and keep-out;
        //   either one unstated -> the land is established only by two equal case codes
        //     (a registered outline such as DO-41 or TO-220 names its hole pattern); with
        //     no such case match a body 'fits' becomes UNVERIFIED, never a pass.
        // DECISIONS for the owner: kLeadPitchTolM = 0.1 mm treats 2.50 / 2.54 and
        // 5.00 / 5.08 mm as one hole pattern (the usual metric/inch cross-use); and an
        // axial part with no pitch on record is held to the same rule, although its lead
        // bend is chosen by the layout rather than by the part.
        bool lead_land_unverified = false;
        if (leaded_pair && !out.contains("footprint")) {
            if (o_lp && s_lp) {
                if (std::fabs(*o_lp - *s_lp) > kLeadPitchTolM) {
                    out["footprint"] = "different_land_pattern";
                    params.push_back({{"name", "footprint"}, {"verdict", FAIL}});
                    demote();
                    penalty += opt.footprint_weight * kVerdictFailPenalty;
                    notes.push_back("lead pitch differs: " + mm(*o_lp) + " -> " + mm(*s_lp) +
                                    " mm — the holes are in different places; a different land "
                                    "pattern, not a drop-in");
                }
            } else {
                const std::string oc = str(original, "case_code"), sc = str(cand, "case_code");
                // A generic style string ("Radial", "Cylindrical Can, Radial") names no hole
                // pattern, so two records stating it have not shown the same land
                // (generic_package_names_no_land, Qarlos r12/r13).
                const bool same_case = cat != "magnetic" && cat != "chipBead" && !oc.empty() &&
                                       !sc.empty() &&
                                       case_ids.first == case_ids.second &&
                                       !generic_package_names_no_land(oc);
                lead_land_unverified = !same_case;
            }
        }
        // A wide-terminal chip (1225, 0612, ...) against a record that states no case code:
        // the outline compare below would turn the 3.2 x 6.4 mm 1225 and call it a fit for a
        // 6.6 x 3.2 mm drawing, but its pads run along the long sides and nothing in the
        // other record says its land does (M55342H09B10D0PWS vs AC1225FK-0710RL, Qarlos
        // 2026-11-08). When BOTH state codes and they differ, the case gate above has
        // already ruled. So the fit is UNVERIFIED — capped like any unverified land.
        if (cat == "resistor" && !out.contains("footprint")) {
            const std::string oc = str(original, "case_code"), sc = str(cand, "case_code");
            const bool ow = wide_terminal_chip_code(oc), sw = wide_terminal_chip_code(sc);
            if (ow != sw && (ow ? sc : oc).empty()) {
                out["footprint"] = "unknown";
                params.push_back({{"name", "footprint"}, {"verdict", UNVERIFIED}});
                demote();
                penalty += opt.footprint_weight * kUnknownDimPenalty;
                // The note claims only what the records establish (Qarlos 2026-11-09:
                // RWR80S28R0FSRSL and RLR32C2701GRRSL are axial bodies 10-14 mm long, and
                // ERJB2CJR024V's LVR candidates state no dimensions at all, yet each note
                // said the outlines "match when turned"). Rule:
                //   * both records state both land axes and the orientation-agnostic compare
                //     finds the same box (no axis over by more than the 2 % slack, none short
                //     of kUndersizeBand) -> the outlines match only when turned;
                //   * both state both axes and the boxes differ -> say so, with both boxes;
                //   * either record lacks a complete land outline -> say which one states no
                //     dimensions; nothing about the outline is claimed.
                // The verdict is UNVERIFIED in every case, as before.
                const std::string wide_side = sw ? "substitute" : "original";
                const std::string plain_side = sw ? "original" : "substitute";
                const auto& wide_dims = sw ? s_dims : o_dims;
                const auto& plain_dims = sw ? o_dims : s_dims;
                auto box = [](const Dims& d) { return mm(*d.length) + " x " + mm(*d.width) + " mm"; };
                std::string outline;
                if (plain_dims && has_land(*plain_dims) && wide_dims && has_land(*wide_dims)) {
                    const LandCompare lc = compare_land(*plain_dims, *wide_dims);
                    const bool same_box =
                        lc.overflow <= kDimTolerance - 1.0 && lc.lin >= kUndersizeBand;
                    outline = same_box
                                  ? "the outlines match only when turned, the pads do not"
                                  : "the outlines differ as well (" + plain_side + " " +
                                        box(*plain_dims) + ", " + wide_side + " " +
                                        box(*wide_dims) + ")";
                } else {
                    const bool plain_has = plain_dims && has_land(*plain_dims);
                    const bool wide_has = wide_dims && has_land(*wide_dims);
                    outline = std::string("the ") +
                              (!plain_has && !wide_has ? "records state"
                               : !plain_has            ? plain_side + "'s record states"
                                                       : wide_side + "'s record states") +
                              " no complete outline dimensions, so the outlines could not be "
                              "compared either";
                }
                notes.push_back("the " + wide_side + " is a wide-terminal " + (sw ? sc : oc) +
                                " chip (terminations along the long sides) and the " +
                                plain_side + "'s record states no case code — " + outline +
                                "; verify the land pattern");
            }
        }
        // A leaded L x W x T body with no height whose record does not say which of L and W
        // stands up from the board (RowBase.box_axes_ambiguous): TDK and Murata draw L across
        // the leads and W upward, KEMET the reverse (Views.cpp fill_dimensions cites the
        // drawings). The row carries no box, so no size compare can rule; the footprint is
        // UNVERIFIED, capped like any unverified land, with a note naming whose box it is.
        // An established mismatch decided above (a different lead pitch, a different case,
        // a class conflict) still stands: it does not depend on the body's orientation.
        {
            const bool o_amb = flag(original, "box_axes_ambiguous"),
                       s_amb = flag(cand, "box_axes_ambiguous");
            if ((o_amb || s_amb) && !out.contains("footprint")) {
                out["footprint"] = "unknown";
                params.push_back({{"name", "footprint"}, {"verdict", UNVERIFIED}});
                demote();
                penalty += opt.footprint_weight * kUnknownDimPenalty;
                const std::string who =
                    o_amb && s_amb ? "both records draw their bodies as L x W x T with no "
                                     "height, and neither says"
                    : o_amb        ? "the original's record draws its body as L x W x T with "
                                     "no height, and does not say"
                                   : "the substitute's record draws its body as L x W x T with "
                                     "no height, and does not say";
                notes.push_back(who +
                                " which of L and W is the height above the board (vendors' "
                                "drawings disagree), "
                                "so the body's footprint and height could not be established; "
                                "footprint fit is UNVERIFIED, confirm the land and clearance "
                                "from the datasheet drawings");
            }
        }
        // ── same stated case = same land (MOSFETs, diodes) ──────────────────
        // A MOSFET or diode case code names a registered outline (TO-220, DPAK, SOD-323,
        // DO-214AB ...), and two records naming the same one name the same pads. The body
        // compare below must not overrule that with a box neither record states: NTP18G120
        // (TO-220, drawn 15.7 x 10.2 mm) took IPP200N15N3 (TO-220, no mechanical block) as
        // "smaller body ... not a drop-in" because the candidate's box was the package
        // TABLE's 10.16 x 4.57 mm TO-220 row, whose axes are width x thickness (Qarlos
        // 2026-11-09, seed 22041502). The same gap left a SOD323 zener 'unknown' against a
        // SOD-323 one (seed 21517857). Rule: both records state case codes with the same
        // case_identity, and at least one side's land is NOT a drawing of its own (a table
        // outline, or none at all) -> the land is kept: footprint 'fits', PASS. When BOTH
        // records draw their own bodies, those drawings are compared as before.
        if ((cat == "mosfet" || cat == "diode") && !out.contains("footprint")) {
            const std::string oc = str(original, "case_code"), sc = str(cand, "case_code");
            auto drawn_land = [](const std::optional<Dims>& d) {
                return d && has_land(*d) && !d->land_from_table;
            };
            if (!oc.empty() && !sc.empty() && case_ids.first == case_ids.second &&
                !(drawn_land(o_dims) && drawn_land(s_dims))) {
                out["footprint"] = "fits";
                params.push_back({{"name", "footprint"}, {"verdict", PASS}});
            }
        }
        // ── magnetic dual chip code against the substitute's drawn body (owner, 2026-11-13) ──
        // An original whose record draws no land but states a dual chip code ("3131 (8080
        // Metric)") had its size thrown away: LQH88PN220M38L (8.0 x 8.0 mm) ranked the 42 x
        // 33 mm AGP4233-223ME first as an unverified footprint (Qarlos bench 20276947), and
        // LQP02HV18NJ02L (0.4 x 0.2 mm) took 1.0 x 0.5 mm MHQ1005P18NGT000 the same way (bench
        // 20784754). The code's metric half is decoded (magnetic_dual_code_body) and set
        // against the substitute's drawn body, sorted axes:
        //   more than kMagneticCodeBodyBand larger or smaller on either axis -> an ESTABLISHED
        //     size difference: the decoded body stands in for the original's land and the
        //     ordinary size compare below grades it (one size larger / smaller / crossed land
        //     / overflow — the treatment of every other family's established mismatch);
        //   inside the band -> UNVERIFIED with a note: a decoded code is not a drawn land, so
        //     it never reads as 'fits' (capped like any one-sided unknown footprint).
        // Only the original's code is decoded: a magnetic substitute with no complete drawing
        // against an original that has one was already rejected above (missing_dimensions).
        if (cat == "magnetic" && !out.contains("footprint") &&
            (!o_dims || !has_any_land(*o_dims)) && s_dims && has_land(*s_dims)) {
            const std::string oc = str(original, "case_code");
            if (const auto coded = magnetic_dual_code_body(oc)) {
                const double s_long = std::max(*s_dims->length, *s_dims->width);
                const double s_short = std::min(*s_dims->length, *s_dims->width);
                const double r_long = s_long / *coded->length, r_short = s_short / *coded->width;
                const bool outside = r_long > kMagneticCodeBodyBand ||
                                     r_short > kMagneticCodeBodyBand ||
                                     r_long < 1.0 / kMagneticCodeBodyBand ||
                                     r_short < 1.0 / kMagneticCodeBodyBand;
                const std::string what = "the original's record draws no body; its chip code '" +
                                         oc + "' decodes (metric half) to " + land_mm(*coded) +
                                         " and the substitute's drawn body is " + land_mm(*s_dims);
                if (outside) {
                    Dims od = *coded;
                    if (o_dims) od.height = o_dims->height;
                    o_dims = od;
                    notes.push_back(what + " — more than 1.5x apart on at least one axis, an "
                                           "established size difference");
                } else {
                    out["footprint"] = "unknown";
                    params.push_back({{"name", "footprint"}, {"verdict", UNVERIFIED}});
                    demote();
                    penalty += opt.footprint_weight * kUnknownDimPenalty;
                    notes.push_back(what + " — within 1.5x on both axes, but a decoded code is "
                                           "not a drawn land; footprint fit is UNVERIFIED, "
                                           "confirm the pads before substituting");
                }
            }
        }
        FootprintTier tier = footprint_tier(o_dims, s_dims, strict_case);
        // Same standard chip code on both records: a drawing a few percent off the original's
        // is the same land, not a larger case (see kSameChipCodeBodySpread).
        bool same_code_spread = false;
        if (cat == "resistor" && tier == FootprintTier::OneSizeLarger && o_dims && s_dims &&
            same_standard_chip_code(str(original, "case_code"), str(cand, "case_code")) &&
            compare_land(*o_dims, *s_dims).overflow <= kSameChipCodeBodySpread) {
            tier = FootprintTier::Fits;
            same_code_spread = true;
        }
        // "One case size larger" describes a body that grew. One that overhangs the original
        // on one axis but is materially SHORTER on the other (below kUndersizeBand, the same
        // "materially smaller" line footprint_tier draws) did not grow: it is a different land.
        // RNC55K1472FSB14 (6.35 x 2.39 mm axial) took ERJS14F1472U (3.2 x 2.5 mm chip) as
        // "about one case size larger" (Qarlos 2026-11-08 seed 20276947). The overhang still
        // costs what it did — same penalty rung, same WARN, same minor_review cap — only the
        // tier name and the note change.
        const bool crossed_land = tier == FootprintTier::OneSizeLarger && o_dims && s_dims &&
                                  compare_land(*o_dims, *s_dims).lin < kUndersizeBand;
        // A LAND to fit to is what this branch needs, not merely a drawing. An
        // original whose record states only a height has no pads to compare
        // against, so it belongs to the "original states no package dimensions"
        // branch below — which is where it went before a land-less drawing could
        // reach here at all.
        if (o_dims && has_any_land(*o_dims) && !out.contains("footprint")) {
            // A same-code spread scores as the fit it is: the strict_case Fits term (0 at the
            // same size, rising only for an undersize body), not the one-size-larger rung.
            double fit_penalty =
                same_code_spread
                    ? kUndersizeWeight * std::max(0.0, 1.0 - compare_land(*o_dims, *s_dims).lin)
                    : footprint_penalty(o_dims, s_dims, strict_case);
            // Same ladder rule as the case-code gate above: a body the compare has
            // ESTABLISHED not to match the pads (smaller than them, or overhanging
            // them) is a worse answer than one nobody could verify, so it is
            // floored onto the rung above UNVERIFIED. The continuous term stands
            // wherever it is already higher — a 3x-oversize part must still cost
            // more than a 1.1x one — and below the rung the parts tie on footprint
            // and are ordered by their electricals, which is what separates two
            // substitutes that both need a re-layout anyway.
            if (tier != FootprintTier::Fits && tier != FootprintTier::Unknown)
                fit_penalty = std::max(fit_penalty, kEstablishedFootprintMismatchPenalty);
            penalty += opt.footprint_weight * fit_penalty;
            // "different_case" is the existing name for a land the pads cannot match (the
            // case-code gate above); the grade reads it as a WARN, exactly as one_size_larger.
            out["footprint"] = crossed_land ? "different_case" : footprint_tier_name(tier);
            const char* verdict = tier == FootprintTier::Fits            ? PASS
                                  : tier == FootprintTier::Smaller       ? WARN
                                  : tier == FootprintTier::OneSizeLarger ? WARN
                                  : tier == FootprintTier::Overflows     ? FAIL
                                                                         : UNVERIFIED;
            params.push_back({{"name", "footprint"}, {"verdict", verdict}});
            // Heaviside demotes rather than rejects an oversize part: it is a
            // real part that works electrically and needs a board-space check.
            // The engineer decides whether the board has room.
            // Each note states the land it is talking about. Four geometrically
            // different outcomes used to carry the same sentence, so an engineer
            // could not tell a part that overhangs the pads from one that no longer
            // covers them (ABT #513).
            if (crossed_land) {
                demote();
                notes.push_back("different land, not a larger case (" + land_mm(*s_dims) +
                                " vs " + land_mm(*o_dims) +
                                ") — longer on one axis and much shorter on the other, so the "
                                "pads do not match; it needs a new land pattern, verify the pads "
                                "before substituting");
            } else if (tier == FootprintTier::OneSizeLarger) {
                demote();
                notes.push_back("about one case size larger (" + land_mm(*s_dims) + " vs " +
                                land_mm(*o_dims) + ") — verify board fit");
            } else if (tier == FootprintTier::Smaller) {
                demote();
                notes.push_back("smaller body than the original (" + land_mm(*s_dims) + " vs " +
                                land_mm(*o_dims) +
                                ") — the land pattern differs, so this is not a drop-in; it needs "
                                "a new land pattern, verify the pads before substituting");
            } else if (tier == FootprintTier::Overflows) {
                demote();
                notes.push_back("larger than the original's footprint (" + land_mm(*s_dims) +
                                " vs " + land_mm(*o_dims) + ") — board respin likely");
            } else if (tier == FootprintTier::Unknown) {
                // Non-magnetic family whose original has a footprint but whose
                // substitute's case code did not resolve to one: the fit is
                // UNVERIFIED (magnetics/chip beads are already excluded above). It
                // must not read as a drop-in, so the grade is capped below. The same
                // rung takes a half-stated land on either side: the axes that COULD
                // be compared did not rule the fit out, which is not the same as
                // ruling it in.
                demote();
                // Three different silences, and the note must name the right one.
                // "mechanical dimensions unavailable" is true only of a record that
                // states NONE; printing it beside a row that lists a height (or a
                // length) overstates the gap and buries the fact that the stated
                // axis matches (ABT #499). What is missing is named instead.
                if (!s_dims || (!has_any_land(*s_dims) && !s_dims->height)) {
                    notes.push_back(
                        "mechanical dimensions unavailable for the substitute — footprint fit "
                        "could not be verified; confirm it fits the original's land pattern");
                } else if (has_any_land(*s_dims)) {
                    notes.push_back("only part of the land is on record (" + land_mm(*s_dims) +
                                    " vs " + land_mm(*o_dims) +
                                    ") — what is stated does not rule the fit out, but does not "
                                    "confirm it either; verify the land pattern");
                } else {
                    // A drawing with a height and no land. It says something real —
                    // the clearance axis above judges it — but nothing at all about
                    // the pads, so the footprint stays unverified and the note says
                    // which of the two it is talking about.
                    notes.push_back("the substitute's record states its height (" +
                                    mm(*s_dims->height) +
                                    " mm) but neither land dimension, so its fit to the "
                                    "original's " +
                                    land_mm(*o_dims) +
                                    " land could not be verified; confirm the land pattern");
                }
            }
        } else if ((!o_dims || !has_any_land(*o_dims)) && footprint_is_the_body(cat) &&
                   !out.contains("footprint")) {
            // The same rung, seen from the ORIGINAL's side. Every branch above needs a
            // footprint for the ORIGINAL; with none, the package axis vanished from the
            // verdict table entirely and "drop_in" — the grade that asserts the part
            // solders into the existing land pattern — was awarded with zero mechanical
            // information about the original. Murata LQP02HQ30NHZ2E, a 01005 (0402
            // metric, 0.4 x 0.2 mm) chip, came back drop_in / penalty 0 / notes null
            // against a Pulse BSCH0010050533NJCP whose 1005-metric body is stated only
            // in its free-text description — indistinguishable from the two candidates
            // that ARE the original's size (ABT #545). The axis is always reported now:
            // silence is what let the top grade through, and reporting UNVERIFIED is
            // what the tool already does for an electrical parameter nobody can check.
            const std::string oc = str(original, "case_code"), sc = str(cand, "case_code");
            // The case-kept gate above only publishes its NEGATIVE. Its positive is a
            // verdict too, and for the families whose case code IS the land pattern it
            // is the one way the fit can be established with no drawing on either side:
            // two records naming the same package name the same pads. Magnetics and
            // chip beads are excluded here exactly as they are there — a magnetic
            // "1210" is an EIA chip or a molded power inductor, so equal codes are not
            // an equal land.
            const bool same_code = cat != "magnetic" && cat != "chipBead" && !oc.empty() &&
                                   !sc.empty() && case_ids.first == case_ids.second;
            // ... unless the shared code is a generic style string ("4-SMD, No Lead", "SMD")
            // that names a pad count, not a size: it establishes nothing, so the fit is
            // UNVERIFIED, never 'fits' (generic_package_names_no_land; Qarlos r12/r13,
            // 7M-26.000MAHE-T -> FA-20H 26.0000MF10Z-W and three more timing pairs). When the
            // substitute states no land either, NEITHER record states the datum: the verdict
            // and the note stand, but the grade is not capped for it (owner, 2026-11-12:
            // missing data on BOTH sides is not capped). A substitute that does state a land
            // falls through to the one-sided branch below, which keeps its cap.
            const bool generic_code = same_code && generic_package_names_no_land(oc);
            if (same_code && !generic_code) {
                out["footprint"] = "fits";
                params.push_back({{"name", "footprint"}, {"verdict", PASS}});
            } else if (generic_code && (!s_dims || !has_any_land(*s_dims))) {
                out["footprint"] = "unknown";
                params.push_back({{"name", "footprint"}, {"verdict", UNVERIFIED}});
                // The ladder's UNVERIFIED rung (see kEstablishedFootprintMismatchPenalty):
                // an unknown fit costs what an unknown fit costs everywhere else.
                penalty += opt.footprint_weight * kUnknownDimPenalty;
                footprint_unstated_both = true;
                notes.push_back("both records state only the generic package '" + oc +
                                "', which names a pad count, not a body size, and neither "
                                "states its dimensions — footprint fit is UNVERIFIED, confirm "
                                "the land pattern from both datasheets");
            } else {
                out["footprint"] = "unknown";
                params.push_back({{"name", "footprint"}, {"verdict", UNVERIFIED}});
                // The ladder's UNVERIFIED rung, as in every other unknown-footprint branch
                // (owner, 2026-11-12). This branch used to add nothing, so a candidate whose
                // package was unknown outranked one whose package was KNOWN to differ (which
                // pays unknown + a warn): an obsolete 160 V-class TVS stating no case ranked
                // first for a 120 V P4SMA120CA-Q, ahead of the exact-class 1KSMB120A in a
                // stated DO-214AA (Qarlos bench 20284866; also r11 24136082, TSM120N06LCS).
                penalty += opt.footprint_weight * kUnknownDimPenalty;
                // NEITHER record states anything about the land: no land dimension on either
                // side (the original's is absent to reach this branch) and no package / case
                // code on either. The verdict and the penalty stand, but the grade is not
                // capped for it — missing data on BOTH sides is not capped (owner,
                // 2026-11-12), exactly as the generic-package branch above. A substitute
                // that states a land or a package, or an original that states a package,
                // is one-sided and keeps the cap.
                //
                // When the ORIGINAL states no land, no candidate is capped for footprint (owner,
                // 2026-11-13, N1b): with nothing to fit against, a candidate that states its
                // dimensions has not been shown to fit any less than one that states none, yet
                // it kept the minor_review cap while the dimensionless one did not
                // (BWLS0006040426NJ00: XFL3012-102MEC capped, SRN3015C-1R0M uncapped). Every
                // candidate still pays kUnknownDimPenalty above. "States no land" is no land
                // dimension (true to reach this branch) and no package / case code. (A magnetic
                // original stating only a case code is left capped: whether that code counts as
                // "no land" is an open question for the owner.) This branch is shared by every
                // family whose footprint is the body, so the rule applies to all of them alike.
                const bool original_states_no_land = o_pkg.empty();
                if (original_states_no_land) footprint_unstated_both = true;
                // Name what the substitute IS, so an engineer can see the two case
                // sizes for themselves even though the ranker cannot compare them.
                // Only where the substitute states a package at all: where neither
                // record states one the UNVERIFIED verdict is the whole story, and the
                // gap belongs to the catalogue, not to this candidate.
                std::string what = s_pkg;
                if (s_dims && has_land(*s_dims)) what += (what.empty() ? "" : ", ") + land_mm(*s_dims);
                if (!what.empty())
                    notes.push_back(
                        "the original's record states no land dimensions, so its land pattern "
                        "could not be established — this substitute is " + what +
                        "; footprint fit is UNVERIFIED, confirm the pads before substituting");
            }
        }
        // The lead-pitch rule's last case (see above): a leaded body that 'fits' with the
        // hole pattern on record for neither or only one side has not had its land
        // compared at all — UNVERIFIED, capped like any unverified footprint.
        if (lead_land_unverified && out.value("footprint", "") == "fits") {
            out["footprint"] = "unknown";
            for (auto& p : params)
                if (p.value("name", "") == "footprint") p["verdict"] = UNVERIFIED;
            demote();
            penalty += opt.footprint_weight * kUnknownDimPenalty;
            auto pitch_said = [](const std::optional<double>& lp) {
                return lp ? mm(*lp) + " mm" : std::string("not stated");
            };
            notes.push_back("the body fits, but the lead pitch — the land pattern of a leaded "
                            "part — is not on record for both (original: " + pitch_said(o_lp) +
                            ", substitute: " + pitch_said(s_lp) +
                            "); footprint fit is UNVERIFIED, confirm the hole pattern from the "
                            "datasheet");
        }
        // ── height / clearance, its own axis ─────────────────────────────────
        // Not part of the land pattern, so it is judged and reported separately
        // and runs whatever the footprint verdict was — a part can need a new land
        // pattern AND more headroom, and an engineer has to be told both. Only a
        // TALLER substitute is a risk; a shorter one keeps the pads and asks less
        // of the enclosure.
        // ── two tantalum cases on one land, at different heights (owner 2026-11-13, L2) ──
        // An EIA-series D (7343-31) and E (7343-43), a V (7343-20), or a "7343-43" string all
        // name the 7343 land (capacitor_case_identities), so they pass the case gate; what
        // they differ in is the HEIGHT, compared here on its own axis. A side whose record
        // draws no height takes the height its case string designates (tantalum_case_height:
        // a code that spells its height, never a table value); the ordinary clearance rule
        // below then grades it — taller WARN, much taller FAIL, shorter pass. Only for two
        // DIFFERENT case strings that the gate found to be one land.
        if (cat == "capacitor" && case_ids.first == case_ids.second &&
            normalize_case_code(str(original, "case_code")) !=
                normalize_case_code(str(cand, "case_code"))) {
            const auto oh = tantalum_case_height(capacitor_case_side(original));
            const auto sh = tantalum_case_height(capacitor_case_side(cand));
            std::vector<std::string> read;
            auto fill = [&](std::optional<Dims>& d, const std::optional<CaseHeight>& h,
                            const char* who) {
                if (!h || (d && d->height)) return;
                if (!d) d = Dims{};
                d->height = h->height_m;
                d->height_from_table = false;
                read.push_back(std::string(who) + " " + h->designation + " (" +
                               mm(h->height_m) + " mm)");
            };
            fill(o_dims, oh, "the original's");
            fill(s_dims, sh, "the substitute's");
            if (!read.empty() && o_dims && o_dims->height && s_dims && s_dims->height) {
                std::string what;
                for (size_t i = 0; i < read.size(); ++i) what += (i ? ", " : "") + read[i];
                notes.push_back("same land (" + str(original, "case_code") + " -> " +
                                str(cand, "case_code") +
                                "); the case heights differ by designation, read as " + what +
                                " — height compared on its own axis");
            }
        }
        HeightFit hfit = height_fit(o_dims, s_dims);
        // A magnetic is verified ONLY from its drawing (see dims_of), and its height is the
        // axis that varies most across one footprint: a 2.5 x 2.0 mm body ships 1.0 to 1.2 mm
        // tall. The original stating a height and the substitute's record stating none left
        // the axis silent, and VLS252012HBX-100M-1 came back drop_in against a 1.0 mm
        // LQH2HPH100MGR (ABT #2029). The clearance is UNVERIFIED and noted. Height alone is a
        // SECONDARY axis (owner, 2026-11-09): the land is what makes a drop-in, so an unstated
        // height neither demotes nor caps the grade any more; an ESTABLISHED taller body still
        // warns / redesigns below.
        //
        // The same silence is no different on any other family: a chip resistor, a capacitor
        // or a transistor whose original states a height and whose substitute states none
        // has an unverified clearance too, and its axis must say so rather than vanish (or,
        // before the table height was dropped in dims_of, read 'pass' from a typical value).
        if (hfit == HeightFit::Unknown && o_dims &&
            o_dims->height && (!s_dims || !s_dims->height) && !out.contains("height_fit")) {
            out["height_fit"] = "unknown";
            params.push_back({{"name", "height"}, {"verdict", UNVERIFIED}});
            notes.push_back("the substitute's record states no height (the original is " +
                            mm(*o_dims->height) +
                            " mm tall), so vertical clearance could not be verified; confirm it "
                            "from the datasheet");
        }
        if (hfit != HeightFit::Unknown) {
            out["height_fit"] = height_fit_name(hfit);
            params.push_back({{"name", "height"},
                              {"verdict", hfit == HeightFit::Fits         ? PASS
                                          : hfit == HeightFit::Taller     ? WARN
                                                                          : FAIL}});
            if (hfit != HeightFit::Fits) {
                // Same ladder floor the footprint term takes: an ESTABLISHED
                // clearance overshoot is a worse answer than one nobody could
                // verify, never a cheaper one.
                penalty += opt.footprint_weight *
                           std::max(height_penalty(o_dims, s_dims),
                                    kEstablishedFootprintMismatchPenalty);
                demote();
                const std::string delta = mm(*o_dims->height) + " -> " + mm(*s_dims->height) +
                                          " mm (" +
                                          signed_pct(100.0 * height_overflow(o_dims, s_dims)) + ")";
                notes.push_back(hfit == HeightFit::Taller
                                    ? "taller than the original: " + delta +
                                          " — verify vertical clearance"
                                    : "much taller than the original: " + delta +
                                          " — will not fit the original's height envelope");
            }
        }
    }

    // ── dielectric envelope (EIA RS-198), not a string compare ──────────────
    // X7R -> X5R keeps the code "shape" but drops the upper temperature 125 ->
    // 85 degC; X7R -> X7S keeps the temperature and widens tolerance 15 -> 22%.
    // Both are real regressions that comparing code strings cannot articulate.
    // A regression here is a regression on the dielectric axis, so it counts against the
    // direction exactly as a WARN/FAIL on a numeric axis does (ABT #2039): a C0G original
    // with an N750 substitute came back an 'upgrade' on its higher voltage (seed 21517857).
    if (cat == "capacitor") {
        const std::string reg = dielectric_regression(str(original, "dielectric_code"),
                                                      str(cand, "dielectric_code"));
        if (!reg.empty()) {
            params.push_back({{"name", "dielectric_envelope"}, {"verdict", FAIL}});
            ++worse;
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            notes.push_back(reg);
        } else {
            // Two DIFFERENT stated codes whose equivalence nothing above could decide (an
            // undecodable code such as SL, BX or a vendor's own, or two class-1 codes of one
            // coefficient where one states no tolerance): never silent, never a drop-in. The
            // dielectric_code verdict is UNVERIFIED; say so (Qarlos 2026-11-09: a C0G
            // original's dielectric axis must not be passed over without a word). A secondary
            // axis (owner, 2026-11-09): noted, but no demotion and no grade cap.
            const std::string ok = dielectric_key(str(original, "dielectric_code")),
                              sk = dielectric_key(str(cand, "dielectric_code"));
            bool unverified = false;
            for (const auto& p : params)
                if (p.value("name", "") == "dielectric_code" &&
                    p.value("verdict", "") == std::string(UNVERIFIED))
                    unverified = true;
            if (!ok.empty() && !sk.empty() && ok != sk && unverified) {
                notes.push_back("dielectric codes differ (" + ok + " -> " + sk +
                                ") and their temperature characteristics could not be compared "
                                "— confirm the substitute's dielectric from its datasheet");
            }
        }
    }

    // ── operating temperature range ─────────────────────────────────────────
    // A substitute must cover the original's whole rated range, at both ends, and
    // the note names both temperatures: "does not reach the original's maximum" is
    // true of 1 degC and of 40 degC, and the engineer has to size the gap against
    // the enclosure ambient, not against the adjective.
    {
        // temp_min_C / temp_max_C are also PARAM_SPECS entries for some categories
        // (connector, timeBase). One physical fact gets one verdict and one penalty,
        // so where that table already ruled, this block adds the note only.
        auto already_judged = [&](const char* name) {
            for (const auto& p : params)
                if (p.value("name", "") == name) return true;
            return false;
        };
        auto o_lo = num(original, "temp_min_C"), s_lo = num(cand, "temp_min_C");
        auto o_hi = num(original, "temp_max_C"), s_hi = num(cand, "temp_max_C");
        if (o_lo && s_lo && *s_lo > *o_lo + 1e-9) {
            if (!already_judged("temp_min_C")) {
                params.push_back({{"name", "temp_min_C"}, {"verdict", FAIL}});
                demote();
                penalty += opt.gate_weight * kVerdictFailPenalty;
            }
            notes.push_back("rated only down to " + degc(*s_lo) + " degC vs the original's " +
                            degc(*o_lo) + " degC — does not cover the original's cold end");
        }
        if (o_hi && s_hi && *s_hi < *o_hi - 1e-9) {
            notes.push_back("rated only to " + degc(*s_hi) + " degC vs the original's " +
                            degc(*o_hi) + " degC — verify the enclosure ambient");
        }
    }

    // ── MOSFET gate-drive compatibility ─────────────────────────────────────
    // The classic swap failure: a logic-level part replaced by a standard-level
    // one still switches on the bench at 10 V and never fully enhances from a
    // 3.3 V controller. Rds(on) is only meaningful at its stated Vgs, so a part
    // whose Rds(on) is specified at a higher Vgs than the original's is not
    // delivering that resistance in the original's circuit. The converse is just
    // as much of a fact: where the candidate's Rds(on) IS specified at a Vgs the
    // original's drive reaches, that part enhances there by its vendor's own
    // guarantee, and the verdict has to say so.
    if (cat == "mosfet") {
        auto o_th = num(original, "vgs_threshold_max"), s_th = num(cand, "vgs_threshold_max");
        auto o_rv = num(original, "rds_on_vgs"), s_rv = num(cand, "rds_on_vgs");
        // ── |Vgs(th)| max: the original states one, the substitute does not ──────────
        // (Qarlos 2026-11-09, seed 21098941: logic-level NVMFS5C670NLET1G, Vth max 2 V, took
        // BSC060N10NS6 / BSC046N08NS5 whose records state only a TYPICAL of 3.5 / 3.8 V, with
        // vgs_threshold_max 'unverified' and no caveat.) Rule:
        //   * the substitute's stated TYPICAL is above the original's guaranteed maximum ->
        //     its maximum is higher still (max >= typ), an ESTABLISHED downgrade for the
        //     original's gate drive: the verdict becomes WARN, or FAIL beyond the table's 2x
        //     band, exactly what the stated maximum would have earned at that value at least;
        //   * otherwise -> UNVERIFIED, with a note naming the original's maximum and what the
        //     substitute's record states. A secondary electrical (owner, 2026-11-09): no
        //     demotion, no grade cap.
        // The typical is never read AS the maximum: it only bounds it from below.
        if (o_th && !s_th) {
            const auto s_typ = num(cand, "vgs_threshold_typ");
            for (auto& p : params) {
                if (p.value("name", "") != "vgs_threshold_max") continue;
                if (s_typ && *s_typ > *o_th + 1e-9) {
                    const bool fail = *s_typ > *o_th * 2.0;
                    p["verdict"] = fail ? FAIL : WARN;
                    demote();
                    // WARN band: above the original's maximum up to the 2x FAIL limit.
                    penalty += opt.gate_weight *
                               (fail ? kVerdictFailPenalty
                                     : warn_penalty(warn_depth_between(*s_typ, *o_th,
                                                                       *o_th * 2.0)));
                    ++worse;
                    notes.push_back("higher gate threshold than the original: the substitute's "
                                    "TYPICAL Vgs(th) is " + volts(*s_typ) +
                                    " V, already above the original's guaranteed maximum of " +
                                    volts(*o_th) +
                                    " V, so its maximum is higher still — a downgrade for the "
                                    "original's gate drive (a logic-level drive may not enhance "
                                    "it fully); check the drive voltage against the substitute's "
                                    "datasheet");
                } else {
                    p["verdict"] = UNVERIFIED;
                    notes.push_back(
                        "the original's gate threshold is at most " + volts(*o_th) +
                        " V; the substitute's record states no maximum" +
                        (s_typ ? " (typical " + volts(*s_typ) + " V)" : std::string()) +
                        ", so Vgs(th) is UNVERIFIED — confirm the substitute enhances at the "
                        "original's gate drive from its datasheet");
                }
            }
        }
        // A higher maximum inside the 1.25x band the gate_drive check below leaves alone is
        // still a downgrade for a logic-level drive (less overdrive at the same Vgs). The
        // table verdict already WARNs it; it was silent.
        if (o_th && s_th && *s_th > *o_th + 1e-9 && *s_th <= *o_th * 1.25)
            notes.push_back("higher gate threshold than the original (Vgs(th) max " +
                            volts(*s_th) + " V vs " + volts(*o_th) +
                            " V) — less gate overdrive at the original's drive voltage, a "
                            "downgrade for a logic-level drive; check enhancement and switching "
                            "at that drive");
        if (o_th && s_th && *s_th > *o_th * 1.25) {
            // "the existing drive voltage" is the Vgs the ORIGINAL's Rds(on) is specified
            // at — the only statement of the drive level either record makes. Where the
            // candidate's own Rds(on) is a guaranteed value at a Vgs that drive reaches,
            // and its threshold sits below it, full enhancement there is guaranteed by the
            // candidate's datasheet: what is left is TIMING, not enhancement. Failing that
            // part contradicted its own record — onsemi's FCB290N80 states 0.29 ohm at
            // Vgs = 10 V with Vth(max) = 4.5 V and was graded a hard fail against an
            // original whose Rds(on) is likewise specified at 10 V — and contradicted this
            // same block's vgs_threshold_max verdict, which had already said warn on the
            // very same quantity (ABT #502). A fail is what the evidence supports only when
            // the candidate's record does not tie its Rds(on) to a drive the original's
            // reaches, or when its threshold climbs into that drive.
            const bool enhances_at_drive =
                o_rv && s_rv && *s_rv <= *o_rv + 1e-9 && *s_th < *o_rv;
            params.push_back(
                {{"name", "gate_drive"}, {"verdict", enhances_at_drive ? WARN : FAIL}});
            demote();
            penalty += opt.gate_weight *
                       (enhances_at_drive ? kVerdictWarnPenalty : kVerdictFailPenalty);
            std::string note = "higher gate threshold than the original (" + volts(*s_th) +
                               " V vs " + volts(*o_th) + " V) — ";
            if (enhances_at_drive) {
                note += "its Rds(on) is a guaranteed value at Vgs = " + volts(*s_rv) +
                        " V, which the original's own " + volts(*o_rv) +
                        " V drive reaches, so it does fully enhance there; expect slower "
                        "turn-on and a Miller plateau sitting higher — check the gate-drive "
                        "timing and the switching loss it buys";
            } else if (o_rv && *s_th >= *o_rv) {
                note += "its threshold reaches the " + volts(*o_rv) +
                        " V drive the original's Rds(on) is specified at — it may not "
                        "enhance at all";
            } else if (!s_rv) {
                note += "its record does not state the Vgs its Rds(on) is specified at, so "
                        "full enhancement at the existing drive is not established";
            } else if (o_rv) {
                note += "its Rds(on) is specified at Vgs = " + volts(*s_rv) + " V, above the " +
                        volts(*o_rv) +
                        " V the original's is — it may not fully enhance at the existing "
                        "drive voltage";
            } else {
                note += "the original's record does not state the Vgs its Rds(on) is "
                        "specified at, so the drive level in the existing circuit is "
                        "unknown — it may not fully enhance at it";
            }
            notes.push_back(note);
        }
        if (o_rv && s_rv && *s_rv > *o_rv + 1e-9) {
            params.push_back({{"name", "rds_on_basis"}, {"verdict", WARN}});
            demote();
            penalty += opt.gate_weight * kVerdictWarnPenalty;
            // volts(), not an int cast: a 4.5 V basis printed as "4 V" (Qarlos r10 seed
            // 23717166, CSD17304Q3 -> BSC059N04LS6, and two more pairs).
            notes.push_back("Rds(on) is specified at a higher Vgs (" +
                            volts(*s_rv) + " V vs " + volts(*o_rv) +
                            " V) — it will be higher at the original's drive level");
        }
        // The candidate's Rds(on) with no stated test Vgs, against an original whose is
        // stated: the two numbers may not describe the same drive, so the rds_on verdict
        // above is a comparison of unknown basis. Say so instead of letting the "pass"
        // stand alone — BSC028N08NS5's 2.8 mOhm (no Vgs on record) passed against a
        // 3.4 mOhm-at-10 V original with nothing beside it (ABT #2045).
        if (o_rv && !s_rv && num(cand, "rds_on")) {
            params.push_back({{"name", "rds_on_basis"}, {"verdict", UNVERIFIED}});
            notes.push_back("the substitute's record does not state the Vgs its Rds(on) is "
                            "specified at (the original's is at " + volts(*o_rv) +
                            " V), so the Rds(on) comparison may not hold at the original's "
                            "drive — confirm it from the datasheet");
        }
        // Vgs(max) is the gate's absolute rating: a lower one can be exceeded by
        // the existing driver and destroy the part.
        // A lower rating is a measured regression and counts toward the direction: SiSS60DN
        // (16 V) came back 'upgrade' for IRFR8314 (20 V) beside this very WARN (Qarlos
        // bench 20276947). An original's rating the substitute's record does not state is a
        // secondary axis stated on one side (owner, 2026-11-09): UNVERIFIED and noted, no
        // demotion, no cap — TK290P60Y (30 V) -> IPD60R280P7S showed no Vgs(max) verdict at
        // all (bench 20575296).
        auto o_gm = num(original, "vgs_max"), s_gm = num(cand, "vgs_max");
        if (o_gm && s_gm && *s_gm < *o_gm - 1e-9) {
            params.push_back({{"name", "vgs_max"}, {"verdict", WARN}});
            demote();
            penalty += opt.gate_weight * kVerdictWarnPenalty;
            ++worse;
            notes.push_back("lower maximum gate-source voltage than the original (" +
                            volts(*s_gm) + " V vs " + volts(*o_gm) +
                            " V) — check the existing gate drive cannot exceed it");
        } else if (o_gm && !s_gm) {
            params.push_back({{"name", "vgs_max"}, {"verdict", UNVERIFIED}});
            notes.push_back("the original's maximum gate-source voltage is " + volts(*o_gm) +
                            " V; the substitute's record states none, so it could not be "
                            "checked against the existing gate drive — confirm it from the "
                            "datasheet");
        }
    }

    // ── crystal load capacitance ────────────────────────────────────────────
    // A crystal is specified AT a load capacitance; the board's load network is
    // built for that value. A substitute specified for a different CL runs off
    // frequency in the existing circuit while looking entirely correct. Applies
    // to passive resonators only — an oscillator carries its own circuit.
    if (cat == "timeBase" &&
        is_passive_resonator(str(original, "technology"), str(original, "device_type")) &&
        is_passive_resonator(str(cand, "technology"), str(cand, "device_type"))) {
        // Records state CL in farads (load_capacitance) or in pF — the parameter
        // table's own key (load_capacitance_pF); read either, or the gate never
        // sees a value from the latter and waves every CL through.
        auto load_cl = [](const json& j) -> std::optional<double> {
            if (auto f = num(j, "load_capacitance")) return f;
            if (auto pf = num(j, "load_capacitance_pF")) return *pf * 1e-12;
            return std::nullopt;
        };
        auto o_cl = load_cl(original), s_cl = load_cl(cand);
        if (auto ppm = crystal_pull_ppm(o_cl, s_cl)) {
            const double mag = std::abs(*ppm);
            if (mag > 5.0) {  // below this the pull is lost in the part's own tolerance
                // The stated CL, not rounded to a whole pF: 8.5 pF printed as "9 pF vs 9 pF"
                // beside a FAIL (Qarlos r10 seed 23507708, 7V-25.000MAAB-T -> 830051068).
                auto pf = [](double farads) {
                    char b[32];
                    std::snprintf(b, sizeof b, "%g", farads * 1e12);
                    return std::string(b);
                };
                params.push_back({{"name", "load_capacitance"}, {"verdict", FAIL}});
                notes.push_back(
                    "specified for a different load capacitance (" +
                    pf(*s_cl) + " pF vs " + pf(*o_cl) +
                    " pF): in the original's load network it would run roughly " +
                    std::to_string(static_cast<int>(std::llround(mag))) +
                    " ppm off frequency (estimate, typical C0/C1)");
                return reject("crystal specified for a different load capacitance");
            }
        }
    }

    // ── ceramic resonator built-in load capacitors ──────────────────────────
    // A three-terminal ceramic resonator carries its load capacitors inside
    // (TDAS electrical.builtInCapacitance): the board has no load network of its own, so
    // these capacitors ARE the load the resonator oscillates into — the same fact the crystal
    // CL gate above guards, and judged the same way. Not extracted before, so CSTNE16M0VH3C
    // (15 pF) took 22 pF and capacitor-less parts with no verdict at all (Qarlos r11 24031353,
    // r13 25288101, bench 20575296). Rule, between two ceramic resonators:
    //   both stated, different -> FAIL, rejected like a crystal with a different CL, with a
    //     note giving both values. No ppm estimate: the crystal pull formula's typical C0/C1
    //     are quartz figures and say nothing about a ceramic resonator;
    //   both stated, equal     -> PASS;
    //   one side only          -> UNVERIFIED, with a note naming the stated value. TDAS says
    //     an absent value means "no built-in capacitors", but Kelvin does not read silence as
    //     a value: the note says what the record does and does not state. No cap (the CL axis
    //     is not one of the owner's primary axes);
    //   neither                -> silent.
    if (cat == "timeBase" && str(original, "technology") == "ceramicResonator" &&
        str(cand, "technology") == "ceramicResonator") {
        const auto o_b = num(original, "built_in_capacitance"),
                   s_b = num(cand, "built_in_capacitance");
        auto pf = [](double farads) {
            char b[32];
            std::snprintf(b, sizeof b, "%g", farads * 1e12);
            return std::string(b);
        };
        if (o_b && s_b) {
            // 1e-6 relative: the records' farads are decimal renderings, not a tolerance.
            if (std::fabs(*s_b - *o_b) > 1e-6 * std::max(*o_b, *s_b)) {
                params.push_back({{"name", "built_in_capacitance"}, {"verdict", FAIL}});
                notes.push_back("different built-in load capacitors (" + pf(*s_b) + " pF vs " +
                                pf(*o_b) +
                                " pF): the resonator oscillates into its own capacitors, so it "
                                "would run under a different load in the original's circuit");
                return reject("ceramic resonator with different built-in load capacitors");
            }
            params.push_back({{"name", "built_in_capacitance"}, {"verdict", PASS}});
        } else if (o_b || s_b) {
            params.push_back({{"name", "built_in_capacitance"}, {"verdict", UNVERIFIED}});
            notes.push_back(o_b ? "the original has " + pf(*o_b) +
                                      " pF built-in load capacitors; the substitute's record "
                                      "states none — confirm from its datasheet whether it has "
                                      "built-in capacitors (a two-terminal part needs an "
                                      "external load network)"
                                : "the substitute has " + pf(*s_b) +
                                      " pF built-in load capacitors; the original's record "
                                      "states none — confirm both parts' load capacitors and "
                                      "terminal count from the datasheets");
        }
    }

    // ── automotive grade (AEC-Q), decoded from the MPN ───────────────────────
    // Fires even when the original is not in the catalogue, which is exactly
    // when an engineer most needs to be told.
    // The catalogue's structured `qualification` field is authoritative where it
    // exists; the MPN decode is the fallback for parts we hold no record of.
    //
    // Three answers, not two. A family whose spec CARRIES a `qualification` key (MOSFETs)
    // is one whose records can state the grade, so a record that states none is UNKNOWN —
    // not commercial. Reading silence as "commercial" FAILED Vishay's SQ4153EY (an SQ-line
    // part whose TAS record has no part.qualification, like 464 of the 465 Vishay SQ*
    // MOSFETs) against the AEC-Q101 DMP1011LFV as "substitute is commercial", a claim no
    // record makes. Kelvin does not decode the grade from a vendor prefix here: "SQ means
    // automotive" is a guess, and the gap belongs to the catalogue. Likewise a label that
    // says "Automotive" without naming AEC-Q states a market, not a qualification, so it is
    // unknown too. "-" is a placeholder, i.e. silence. Families without the field keep the
    // MPN decode as their only evidence (Murata GRM is a stated commercial series there).
    enum class Grade { Aec, NotAec, Unknown };
    auto grade_of = [](const json& p) {
        const std::string q = lower_copy(str(p, "qualification"));
        if (!q.empty() && q != "-") {
            if (q.find("aec") != std::string::npos) return Grade::Aec;
            if (q.find("automotive") != std::string::npos &&
                q.find("non-automotive") == std::string::npos)
                return Grade::Unknown;
            return Grade::NotAec;
        }
        if (is_automotive_grade(str(p, "mpn"))) return Grade::Aec;
        return p.contains("qualification") ? Grade::Unknown : Grade::NotAec;
    };
    // A bare "Automotive" label on the ORIGINAL (Qarlos 2026-11-09, seed 20470567). The label
    // reads UNKNOWN for a SUBSTITUTE (it does not establish AEC-Q), and the same reading on
    // the original's side made the whole gate skip: Infineon's IPW65R048CFDA, qualification
    // "Automotive", was replaced by Vishay SiHG61N65EF (no qualification on record) graded
    // 'recommended' with no note. The original's label is still a statement that the part
    // was built for automotive service, and the design it sits in may depend on that, so the
    // gate engages for it too. Rule (DECISION for the owner to confirm):
    //   * substitute names AEC-Q, or carries the same bare automotive label -> nothing
    //     flagged (it claims at least what the original claims);
    //   * substitute states a non-automotive grade (Industrial, Standard, ...) -> FAIL, as for
    //     an AEC-Q original;
    //   * substitute states nothing -> UNVERIFIED, demoted to partial, with a note.
    auto automotive_label = [&](const json& p) {
        const std::string q = lower_copy(str(p, "qualification"));
        return q.find("automotive") != std::string::npos &&
               q.find("non-automotive") == std::string::npos &&
               q.find("aec") == std::string::npos;
    };
    const Grade o_grade = grade_of(original);
    const bool o_label_only = o_grade != Grade::Aec && automotive_label(original);
    if (o_label_only) {
        const Grade g = grade_of(cand);
        const std::string oq = str(original, "qualification");
        const std::string q = str(cand, "qualification");
        if (g == Grade::NotAec) {
            params.push_back({{"name", "automotive_grade"}, {"verdict", FAIL}});
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            notes.push_back("original is labelled \"" + oq + "\" (an automotive part); the "
                            "substitute's record states \"" + q +
                            "\" — not an automotive part");
        } else if (g == Grade::Unknown && !automotive_label(cand)) {
            params.push_back({{"name", "automotive_grade"}, {"verdict", UNVERIFIED}});
            demote();
            notes.push_back("original is labelled \"" + oq +
                            "\" (an automotive part); the substitute's record states no "
                            "qualification, so its automotive grade is UNVERIFIED — confirm "
                            "AEC-Q qualification from the datasheet");
        }
    }
    if (o_grade == Grade::Aec) {
        const Grade g = grade_of(cand);
        if (g == Grade::NotAec) {
            params.push_back({{"name", "automotive_grade"}, {"verdict", FAIL}});
            demote();
            penalty += opt.gate_weight * kVerdictFailPenalty;
            notes.push_back("original is an automotive (AEC-Q) grade part; substitute is commercial");
        } else if (g == Grade::Unknown) {
            // Not a drop-in on the record's word either: an AEC-Q original cannot be
            // replaced by a part whose qualification nobody has established, so the grade
            // is capped the way an unverified footprint caps it, at no extra penalty.
            params.push_back({{"name", "automotive_grade"}, {"verdict", UNVERIFIED}});
            demote();
            const std::string q = str(cand, "qualification");
            notes.push_back(
                "original is an automotive (AEC-Q) grade part; the substitute's record " +
                (q.empty() || q == "-" ? std::string("states no qualification")
                                       : "says \"" + q + "\" but names no AEC-Q qualification") +
                ", so its grade is UNVERIFIED — confirm AEC-Q qualification from the datasheet");
        }
    }

    // ── qualified reliability (MIL / established-reliability level) ──────────
    // A part qualified to a failure-rate level under a named specification (PEAS
    // qualifiedReliability: MIL-PRF-55681 grade S, MIL-PRF-39003 grade R at 0.001 %/1000 h)
    // is bought for that certification; a commercial part of the same ratings does not carry
    // it. CDR31BP470BFYSAB (MIL-PRF-55681, grade S) took Murata GRM commercial parts with no
    // reliability caveat (Qarlos 2026-11-09, seed 20365838). Treated like the AEC-Q grade, the
    // record's word only (nothing decoded from an MPN or a series). Rule:
    //   * the original states no qualification -> nothing to hold, not compared;
    //   * same specification and same grade (or the original states no grade) -> PASS;
    //   * same specification, both state a failure rate -> the substitute's at or below the
    //     original's PASSes, above it FAILs with both rates quoted;
    //   * anything else — the substitute states no qualification, another specification, or
    //     another grade with no rates to compare (the same letter means different rates under
    //     different specifications, and the order of letters is the specification's) ->
    //     UNVERIFIED, noted, and the grade capped below drop_in.
    bool qualification_unverified = false;
    if (cat == "capacitor") {
        const std::string o_q = str(original, "qualified_spec"), s_q = str(cand, "qualified_spec");
        if (!o_q.empty()) {
            const std::string o_g = str(original, "qualified_grade"), s_g = str(cand, "qualified_grade");
            const auto o_fr = num(original, "qualified_failure_rate"),
                       s_fr = num(cand, "qualified_failure_rate");
            auto level = [](const std::string& q, const std::string& g) {
                return q + (g.empty() ? std::string() : " grade " + g);
            };
            auto fit = [](double per_s) {  // FIT = failures per 1e9 h
                char b[48];
                std::snprintf(b, sizeof b, "%.4g FIT", per_s * 3600.0 * 1e9);
                return std::string(b);
            };
            const std::string o_level = level(o_q, o_g);
            if (s_q == o_q && (o_g.empty() || s_g == o_g)) {
                params.push_back({{"name", "qualified_reliability"}, {"verdict", PASS}});
            } else if (s_q == o_q && o_fr && s_fr) {
                const bool worse_rate = *s_fr > *o_fr * (1.0 + 1e-6);
                params.push_back(
                    {{"name", "qualified_reliability"}, {"verdict", worse_rate ? FAIL : PASS}});
                if (worse_rate) {
                    demote();
                    ++worse;
                    penalty += opt.gate_weight * kVerdictFailPenalty;
                    notes.push_back("higher qualified failure rate: the substitute is " +
                                    level(s_q, s_g) + " (" + fit(*s_fr) + ") against the original's " +
                                    o_level + " (" + fit(*o_fr) + ")");
                }
            } else {
                qualification_unverified = true;
                params.push_back({{"name", "qualified_reliability"}, {"verdict", UNVERIFIED}});
                demote();
                notes.push_back(
                    "the original is qualified to " + o_level + "; " +
                    (s_q.empty() ? std::string("the substitute's record states no qualified "
                                               "reliability level")
                                 : "the substitute's record states " + level(s_q, s_g) +
                                       ", which cannot be ranked against it from the records") +
                    " — confirm the program accepts it before substituting");
            }
        }
    }

    // ── rated voltage decoded from the MPN, when the record lacks it ─────────
    if (cat == "capacitor" && !num(original, "voltage") && !num(cand, "voltage")) {
        auto od = decode_cap_mpn(str(original, "mpn"));
        auto sd = decode_cap_mpn(str(cand, "mpn"));
        if (od.voltage && sd.voltage) {
            const char* verdict = *sd.voltage >= *od.voltage ? PASS : FAIL;
            params.push_back({{"name", "voltage_from_mpn"}, {"verdict", verdict}});
            if (verdict == FAIL) {
                notes.push_back("substitute's MPN-decoded rated voltage is below the original's");
                return reject("rated voltage (decoded from MPN) below the original");
            }
        }
    }

    // ── MLCC DC-bias: compare EFFECTIVE capacitance at the operating point ───
    if (cat == "capacitor" && opt.operating_voltage && *opt.operating_voltage > 0) {
        auto oc = effective_capacitance_at_bias(
            num(original, "value_si"), num(original, "voltage"),
            num(original, "capacitance_saturation_mlcc"), num(original, "vth_mlcc"),
            *opt.operating_voltage);
        auto sc = effective_capacitance_at_bias(
            num(cand, "value_si"), num(cand, "voltage"),
            num(cand, "capacitance_saturation_mlcc"), num(cand, "vth_mlcc"),
            *opt.operating_voltage);
        if (oc || sc) {
            auto v = score_directional(oc, sc, Mode::HigherBetter, 0.9, 0.9);
            params.push_back({{"name", "c_bias"}, {"verdict", v.verdict}});
            if (v.verdict == FAIL || v.verdict == WARN) {
                demote();
                penalty += opt.gate_weight * v.penalty;
                notes.push_back("effective capacitance under DC bias falls short of the original");
            }
        }
    }

    // ── lifecycle ────────────────────────────────────────────────────────────
    // Heaviside never consults this; Kelvin's catalogue carries it, so a
    // substitute that is not in production is surfaced rather than silently
    // recommended.
    if (opt.check_lifecycle) {
        auto it = cand.find("is_production");
        if (it != cand.end() && it->is_boolean() && !it->get<bool>()) {
            params.push_back({{"name", "lifecycle"}, {"verdict", WARN}});
            demote();
            penalty += opt.gate_weight * kVerdictWarnPenalty;
            notes.push_back("substitute is not marked production status");
        }
    }

    // Say which axis went unweighed, or "unknown" reads as a shrug rather than a specific
    // gap the reader can close from a datasheet.
    if (direction_blind) {
        std::string axes;
        for (size_t i = 0; i < blind_axes.size(); ++i)
            axes += (i ? ", " : "") + blind_axes[i];
        notes.push_back("the original's record states no " + axes +
                        ", so the substitute's could not be weighed against it — better or "
                        "worse on that axis is unknown, whatever the other parameters say");
    }
    if (!magnetic_unweighed.empty()) {
        std::string axes;
        for (size_t i = 0; i < magnetic_unweighed.size(); ++i)
            axes += (i ? ", " : "") + magnetic_unweighed[i];
        notes.push_back("the original states its " + axes + "; the substitute's record does "
                        "not, so whether it is ahead or behind there is unknown — confirm it "
                        "from the datasheet before calling this swap an upgrade");
    }

    // ── retracted sources (ABT #2035) ────────────────────────────────────────
    // Every source the substitute's record cites has been retracted (VLIN1626-02G: a
    // category browse page naming no order code). What the row says may be right, but
    // nothing verifies it, so no verdict drawn from it may read 'pass' or 'fits': each
    // becomes UNVERIFIED, the grade is capped as missing required data and the direction
    // is unknown. Regressions stay as they are — data that already disqualifies the part
    // does not become better for being unsourced.
    const bool cand_retracted = flag(cand, "provenance_retracted");
    if (cand_retracted) {
        for (auto& p : params)
            if (p.value("verdict", "") == PASS) p["verdict"] = UNVERIFIED;
        if (out.value("footprint", "") == "fits") out["footprint"] = "unknown";
        if (out.value("height_fit", "") == "fits") out["height_fit"] = "unknown";
        demote();
        penalty += opt.gate_weight * kVerdictFailPenalty;
        missing_required_data = true;
        notes.push_back("every source cited by the substitute's record is retracted — none of "
                        "its data is verified; confirm the part from its datasheet before "
                        "relying on any verdict here");
    }
    // The ORIGINAL's whole-record retraction was stated only as the original_unverified flag
    // and the site's banner: a consumer of the candidate (the MCP tool, the harness) got no
    // caveat and no reason (Qarlos r26 31990757, Vishay RGF1K). Named here, with the reason
    // the record gives. Owner 2026-11-13, Z2: the same treatment as a per-field retraction —
    // no PASS can be verified against the original's data, so each reads UNVERIFIED (a
    // 'fits' reads 'unknown') and the direction cannot read 'upgrade' or 'equivalent' (it was
    // PASS / 'upgrade' before: Vishay RGF1K -> 1N4006RLG). Regressions stay as they are. The
    // status stays capped as below; no grade cap beyond what UNVERIFIED implies.
    const bool orig_retracted_blind = flag(original, "provenance_retracted");
    if (orig_retracted_blind) {
        for (auto& p : params)
            if (p.value("verdict", "") == PASS) p["verdict"] = UNVERIFIED;
        if (out.value("footprint", "") == "fits") out["footprint"] = "unknown";
        if (out.value("height_fit", "") == "fits") out["height_fit"] = "unknown";
        const std::string why = str(original, "retraction_reason");
        notes.push_back("every source cited by the original's record is retracted" +
                        (why.empty() ? std::string() : " (" + why + ")") +
                        " — the original's data is unverified, so no verdict against it can be "
                        "relied on; confirm the original from its datasheet");
    }

    // ── fields resting only on retracted sources (owner 2026-11-13, S1) ────────
    // A record can carry a retracted entry for the whole record beside live entries scoped to
    // a few fields (VS-6CWH02FN-M3: a withdrawn category-page citation, and a live product page
    // for electrical.forwardCurrent only). Every other field then rests on the withdrawn source
    // alone. A comparison drawn from such a field, on either record, is read as UNVERIFIED (a
    // PASS cannot be verified by it) and a note names the retraction; a field with a live
    // source grades normally. Regressions stay as they are, as in the whole-record rule above.
    // The direction cannot then read 'upgrade' or 'equivalent' on unverified evidence.
    bool field_retraction_blind = false;
    if (!cand_retracted) {
        auto in_list = [](const json& row, const std::vector<std::string>& fields) {
            auto it = row.find("retracted_fields");
            if (it == row.end() || !it->is_array()) return false;
            for (const json& f : *it)
                if (f.is_string() &&
                    std::find(fields.begin(), fields.end(), f.get<std::string>()) != fields.end())
                    return true;
            return false;
        };
        const bool orig_whole = flag(original, "provenance_retracted");
        std::vector<std::string> cand_axes, orig_axes;
        auto add = [](std::vector<std::string>& v, const std::string& label) {
            if (std::find(v.begin(), v.end(), label) == v.end()) v.push_back(label);
        };
        for (auto& p : params) {
            if (p.value("verdict", "") != PASS) continue;
            const std::string name = p.value("name", "");
            const bool s = in_list(cand, comparison_source_fields(cat, name, cand));
            const bool o = !orig_whole && in_list(original, comparison_source_fields(cat, name, original));
            if (!s && !o) continue;
            p["verdict"] = UNVERIFIED;
            if (s) add(cand_axes, param_label(name));
            if (o) add(orig_axes, param_label(name));
        }
        for (const char* axis : {"footprint", "height_fit"}) {
            if (out.value(axis, "") != "fits") continue;
            const std::string name = std::string(axis) == "footprint" ? "footprint" : "height";
            const bool s = in_list(cand, comparison_source_fields(cat, name, cand));
            const bool o = !orig_whole && in_list(original, comparison_source_fields(cat, name, original));
            if (!s && !o) continue;
            out[axis] = "unknown";
            if (s) add(cand_axes, name == "footprint" ? "footprint" : "height");
            if (o) add(orig_axes, name == "footprint" ? "footprint" : "height");
        }
        auto join = [](const std::vector<std::string>& v) {
            std::string s;
            for (size_t i = 0; i < v.size(); ++i)
                s += (i == 0 ? "" : i + 1 == v.size() ? " and " : ", ") + v[i];
            return s;
        };
        auto because = [&](const json& row) {
            const std::string r = str(row, "retraction_reason");
            return r.empty() ? std::string() : " (" + r + ")";
        };
        if (!cand_axes.empty())
            notes.push_back("the substitute's " + join(cand_axes) +
                            " rest only on a source its record has retracted" + because(cand) +
                            " — no live source verifies them, so they read unverified; confirm "
                            "them from the datasheet");
        if (!orig_axes.empty())
            notes.push_back("the original's " + join(orig_axes) +
                            " rest only on a source its record has retracted" + because(original) +
                            " — the comparison against them is unverified; confirm them from the "
                            "datasheet");
        field_retraction_blind = !cand_axes.empty() || !orig_axes.empty();
    }

    // Honesty: an unverified original can never be a clean 'recommended'. An original
    // whose every source is retracted is unverified the same way (ABT #2035).
    if (!opt.original_verified || flag(original, "provenance_retracted")) {
        if (status == "recommended") status = "partial";
        out["original_unverified"] = true;
    }

    // ── several connector identities unverified at once (owner, 2026-11-12) ──
    // One identity stated by one record only is a gap an engineer closes from one datasheet
    // (minor_review, or no cap for a family-variant identity). Three or more together mean
    // the substitute's record establishes almost nothing of what makes it THIS connector:
    // D38999/20MG11BN (female, panel-mount, shell 21, coding N, threaded) took
    // YDIV46E21-11PNC009, whose record states none of the five, as minor_review (Qarlos bench
    // 20365838; it is a wire-to-wire plug). Counted: axes the ORIGINAL states and the
    // substitute's record does not (owner 2026-11-13, W1: an axis stated only by the
    // substitute is not a gap; stating extra data never hurts a candidate) — gender,
    // wire termination, contact pitch, contact rows, board mount, orientation (owner,
    // 2026-11-13; TW-03-05-F-Q-365-090 -> WT12PD9), and the family-variant identities. Contact
    // positions never counts: an original that states them against a silent substitute is
    // rejected (hard identity), so only the substitute-only direction reaches here. DECISION for the owner: the variant
    // identities (actuator type, shell size, coding, coupling type) count although a single one
    // is uncapped (interface_standard's treatment, which is NOT counted); pluggable is a capped
    // one-sided connector axis but not an identity in the owner's list, and is not counted.
    constexpr size_t kConnectorIdentityGapsMajor = 3;
    const bool connector_identity_gaps_major =
        connector_identity_gaps.size() >= kConnectorIdentityGapsMajor;
    if (connector_identity_gaps_major) {
        demote();
        std::string names;
        for (size_t i = 0; i < connector_identity_gaps.size(); ++i)
            names += (i ? ", " : "") + connector_identity_gaps[i];
        notes.push_back(std::to_string(connector_identity_gaps.size()) +
                        " connector identity axes are unverified (stated on one side only: " +
                        names +
                        ") — the substitute's record does not establish that it is the same "
                        "connector; confirm each from the datasheets");
    }

    if (!orig_unstated_primary.empty()) {
        std::string axes;
        for (size_t i = 0; i < orig_unstated_primary.size(); ++i)
            axes += (i ? ", " : "") + orig_unstated_primary[i];
        demote();
        notes.push_back("the original's record states no " + axes +
                        "; the substitute's does, but nothing holds it to the original's "
                        "requirement there — capped below drop_in; confirm the original's "
                        "rating from its datasheet before this swap");
    }
    // No physical data on the candidate at all while the original states some (owner
    // 2026-11-14; kNoPhysicalDataPenalty): capped, a small extra penalty, and a note.
    const bool cand_no_physical_data = (opt.check_footprint || cat == "connector") &&
                                       states_physical_data(original) &&
                                       !states_physical_data(cand);
    if (cand_no_physical_data) {
        demote();
        penalty += opt.footprint_weight * kNoPhysicalDataPenalty;
        // When the mount gate has already reported the missing mount (mount_unverified), this
        // note leaves the mount out: one mount note per candidate (owner 2026-11-14; the
        // connector guard "exactly one mount verdict and one note").
        notes.push_back(mount_unverified
                            ? "the substitute's record states no physical data at all (no case "
                              "or package, no dimensions; its board attach is noted separately) "
                              "— its size is unknown, so it ranks after parts of a known size; "
                              "confirm the package from its datasheet before this swap"
                            : "the substitute's record states no physical data at all (no case or "
                              "package, no dimensions, no mount) — its size and how it mounts are "
                              "unknown, so it ranks after parts of a known size; confirm the "
                              "package from its datasheet before this swap");
    }
    if (!both_unstated_primary.empty()) {
        std::string axes;
        for (size_t i = 0; i < both_unstated_primary.size(); ++i)
            axes += (i ? ", " : "") + both_unstated_primary[i];
        demote();
        notes.push_back("neither record states the " + axes +
                        " — a primary rating the match turns on was not compared, so the grade "
                        "is capped below drop_in; confirm it from both datasheets before this "
                        "swap");
    }
    out["status"] = status;
    out["penalty"] = penalty;
    out["params"] = params;
    if (!notes.empty()) out["notes"] = notes;

    // ── grade + direction ────────────────────────────────────────────────────
    bool any_warn = false, any_fail = false;
    for (const auto& p : params) {
        const std::string v = p.value("verdict", "");
        if (v == WARN) any_warn = true;
        if (v == FAIL) any_fail = true;
    }
    FootprintTier tier = FootprintTier::Unknown;
    bool footprint_unverified = false;
    if (out.contains("footprint")) {
        const std::string f = out["footprint"];
        tier = f == "fits"              ? FootprintTier::Fits
               : f == "smaller"         ? FootprintTier::Smaller
               : f == "one_size_larger" ? FootprintTier::OneSizeLarger
               : f == "overflows"       ? FootprintTier::Overflows
                                        : FootprintTier::Unknown;
        // Only a literal "unknown" (dims/case-code missing) is UNVERIFIED and blocks
        // a drop-in via grade_for; "different_case" already carries a WARN param
        // (any_warn -> minor_review), so it must not also read as unverified.
        footprint_unverified = (f == "unknown") && !footprint_unstated_both;
    }
    // The clearance axis, carried alongside the land one: a body that no longer
    // clears the enclosure is outside the original's board space just as surely as
    // one that overhangs the pads.
    HeightFit hfit = HeightFit::Unknown;
    if (out.contains("height_fit")) {
        const std::string h = out["height_fit"];
        hfit = h == "fits"          ? HeightFit::Fits
               : h == "taller"      ? HeightFit::Taller
               : h == "much_taller" ? HeightFit::MuchTaller
                                    : HeightFit::Unknown;
    }
    // ── which one-sided UNVERIFIED axes cap the grade (owner's rule, 2026-11-09) ──────────
    // A benchmark showed drop-ins falling from 28 % to 12 %, mostly because a substitute that
    // does not state a SECONDARY axis was capped at minor_review. The rule now:
    //   * PRIMARY axis stated by one side only (or on an incomparable basis) -> UNVERIFIED,
    //     noted, demoted, and capped (minor_review below; major_review where the absence is
    //     missing_required_data);
    //   * SECONDARY axis stated by one side only (or on an incomparable basis) -> UNVERIFIED
    //     and noted, any direction effect kept and any penalty kept, but NO demotion and NO
    //     cap, so drop_in stays reachable;
    //   * an ESTABLISHED mismatch (both sides state the axis: WARN / FAIL) is unchanged.
    //
    //   axis (flag / site)                                   class      unverified ->
    //   ---------------------------------------------------  ---------  -------------------
    //   land / footprint unknown (footprint_unverified)       primary    cap minor_review
    //   land / footprint unknown, NEITHER record states a
    //     land or a package (footprint_unstated_both; also
    //     both naming one generic package string; or the
    //     ORIGINAL states no land and no package,
    //     owner 2026-11-13 N1b)                               both-unst. UNVERIFIED + penalty,
    //                                                                    no cap
    //   lead form axial/radial (lead_form_unverified)         primary    cap minor_review
    //   SMD / THT / board mount (mount_unverified)            primary    cap minor_review
    //   connector pitch alignment (connector_pitch_unverif.)  primary    cap minor_review
    //   connector gender/termination/pitch/rows (identity)    primary    cap minor_review
    //   connector variant identity (actuator type, shell
    //     size, coding, coupling type), one alone             primary    note only, no cap
    //   connector identities stated by the ORIGINAL only, 3
    //     or more of gender, termination, pitch, rows, board
    //     mount, orientation and the variant identities
    //     (connector_identity_gaps; owner 2026-11-13 W1)    primary    cap major_review
    //   connector orientation (connector_orientation_unv.)    primary    cap minor_review
    //   connector pluggable vs fixed (pluggable_unverified)   primary    cap minor_review
    //   cable-core cable diameter / pass count not stated by
    //     the substitute (owner 2026-11-14; cable_core_unv.)  primary    cap minor_review
    //   diode / MOSFET die configuration, internal connection primary  cap minor_review
    //   oscillator control-pin function (timebase_pin_unv.)   primary    cap minor_review
    //   resistor power at rating temp (power_basis_unverif.)  primary    cap minor_review
    //   capacitor ripple-current basis (ripple_basis_unv.)    primary    cap minor_review
    //   capacitor ripple TEMPERATURE basis differs / unstated
    //     (ripple_temp_basis_unverified; owner 2026-11-13 Z3) primary    cap minor_review
    //   MIL qualified reliability (qualification_unverified)  primary    cap minor_review
    //   safety class / AC rating, Isat, ripple, bead |Z|,
    //     connector family/positions/rated current, analog
    //     subtype/channels, timebase subtype/tech/frequency,
    //     varistor Vclamp/Isurge, TVS Ppk/Ipp, diode If basis,
    //     zener Vz (missing_required_data)                    primary    cap major_review
    //   AEC-Q grade (automotive_grade)                        primary    demoted, no cap*
    //   magnetic rated current absent from the substitute
    //     (owner 2026-11-13; magnetic_rated_absent)           primary    note + FAIL penalty,
    //                                                                    direction 'unknown',
    //                                                                    no cap
    //   magnetic Isat stated by NEITHER record (owner
    //     2026-11-13)                                         both-unst. UNVERIFIED row + note,
    //                                                                    no penalty; capped
    //                                                                    minor_review since
    //                                                                    2026-11-14 (below)
    //   Id / If(AV) / Ic stated by one record only (owner
    //     2026-11-13, Y1; current_rating_one_sided)           primary    note, cap minor_review
    //   chip-bead rated current, connector rated voltage,
    //     diode power dissipation                             primary    no cap*
    //   a primary RATING stated by the substitute and NOT by
    //     the original (owner 2026-11-13, Y2; axes listed in
    //     is_primary_rating; orig_unstated_primary)            primary    note, cap minor_review
    //   a candidate stating no physical data at all (no case /
    //     package, dimension or mount) while the original states
    //     some (owner 2026-11-14; kNoPhysicalDataPenalty)    primary    note, small penalty,
    //                                                                    cap minor_review
    //   a primary RATING real for the original's technology
    //     (both_unknown_cap_applies) stated by NEITHER record
    //     (owner 2026-11-14; both_unstated_primary)           primary    note, cap minor_review
    //   height-only clearance (height_fit unknown)            secondary  note only
    //   operating-temperature range (resistor, magnetic)      secondary  note only
    //   resistor TCR                                          secondary  note only
    //   diode Tj max                                          secondary  note only
    //   diode junction capacitance absent (owner kept cap)   missing_required -> major_review
    //   diode Cj bias basis differs / unstated (cj basis)     secondary  note only
    //   resistor maximum working voltage                      secondary  note only
    //   capacitor ESR absent (was missing_required)           secondary  note + FAIL penalty
    //   capacitor ESR basis (esr_basis_unverified)            secondary  note only
    //   capacitance tolerance band                            secondary  note only
    //   capacitor dielectric codes not comparable             secondary  note only
    //   timing frequency tolerance / stability unstated       secondary  note only,
    //     on the substitute (original states it)                         direction 'unknown'
    //   timing technology MEMS <-> quartz XO (established,
    //     owner 2026-11-13; timebase_tech_major)              crossing   WARN, cap major_review
    //   timing technology TCXO for a quartz XO (upgrade)      crossing   WARN -> minor_review
    //   programmable blank for a fixed-frequency original
    //     (owner 2026-11-13, M0; timebase_prog_order)          ordering   cap minor_review + note
    //   MOSFET Vgs(th) max, Coss, Qg and other secondary
    //     electricals (table verdicts)                        secondary  note only
    //   MOSFET Coss Vds basis differs / unstated (coss basis) secondary  note only
    //   magnetic / bead SRF, DCR one-sided                    secondary  direction 'unknown'
    // Identity rows of a connector, the land, and the ratings in the owner's list are
    // primary; every other axis is secondary.
    // * primary, but never had a one-sided cap: "primary axes keep their caps" keeps what
    //   exists and adds none (left for the owner to decide).
    if (connector_orientation_unverified) footprint_unverified = true;
    if (connector_pitch_unverified) footprint_unverified = true;
    if (connector_pluggable_unverified) footprint_unverified = true;
    if (connector_identity_unverified) footprint_unverified = true;
    const bool primary_unverified = footprint_unverified || power_basis_unverified ||
                                    ripple_basis_unverified || ripple_temp_basis_unverified ||
                                    lead_form_unverified ||
                                    qualification_unverified || topology_unverified ||
                                    resistor_connection_unverified || resistor_count_one_sided ||
                                    mount_unverified || timebase_pin_unverified ||
                                    timebase_prog_order || current_rating_one_sided ||
                                    cable_core_unverified ||
                                    !orig_unstated_primary.empty() ||
                                    !both_unstated_primary.empty() || cand_no_physical_data ||
                                    ac_rating_unverified;
    out["grade"] = grade_for(status, tier, any_warn, any_fail, primary_unverified,
                             missing_required_data || connector_identity_gaps_major ||
                                 timebase_tech_major,
                             hfit);

    // Direction: on the directional parameters we could actually compare, did
    // the substitute come out ahead or behind? Mirrors the industry's upgrade /
    // downgrade suffix (SiliconExpert A/U vs A/D), but computed from the
    // measured ratios rather than asserted. "equivalent" when neither side
    // clearly leads — including when there was nothing comparable to judge on,
    // which is honest rather than flattering.
    const std::string dir = (worse > 0 && better == 0)   ? "downgrade"
                            : (better > 0 && worse == 0) ? "upgrade"
                            : (better > 0 && worse > 0)  ? "mixed"
                                                         : "equivalent";
    // With a hard axis unweighed (see direction_blind), the two REASSURING readings are the
    // ones that could be wrong: "upgrade" and "equivalent" both assert that nothing is behind,
    // on evidence that never covered the parameter the match turns on. A measured regression
    // stands — blindness elsewhere cannot make a regression we did see disappear — and so does
    // "mixed", which already carries one.
    out["direction"] = (cand_retracted || ((direction_blind || field_retraction_blind || orig_retracted_blind || isat_basis_blind || surge_blind ||
                                            current_basis_blind || !magnetic_unweighed.empty() ||
                                            magnetic_rated_absent || timing_accuracy_unweighed) &&
                                           (dir == "upgrade" || dir == "equivalent")) ||
                            // Isat stated by neither record (owner, 2026-11-13): 'upgrade' and
                            // 'equivalent' would both claim the comparison the match turns on, so
                            // both read 'unknown' (87ab9dd did 'upgrade'; 'equivalent' followed
                            // with the owner's yes, N1c, and the fuzz self-copy invariant accepts
                            // 'unknown' for exactly this case).
                            (isat_unstated_both && (dir == "upgrade" || dir == "equivalent")))
                           ? "unknown"
                           : dir;
    // A different nominal resistance (see value_differs above): an alternative only. Its
    // grade is capped at major_review (a worse grade stands), and it never reads as
    // "upgrade" or "equivalent" — the value it exists for is not the original's, so it is
    // behind the exact-value parts whatever its other ratings. cross_reference ranks it
    // after every exact-value candidate on this flag.
    if (value_differs) {
        out["value_differs"] = true;
        const std::string g = out["grade"];
        if (g == "drop_in" || g == "minor_review") out["grade"] = "major_review";
        const std::string d = out["direction"];
        if (d == "upgrade" || d == "equivalent") out["direction"] = "downgrade";
    }
    // A zener off the original's nominal (see zener_off_nominal): the same sort tier, and
    // nothing else — grade and direction unchanged (owner 2026-11-13).
    if (zener_off_nominal) out["value_differs"] = true;
    // An inductance outside the original's stated tolerance (see inductance_off_band): the
    // same sort tier; grade and direction stay the WARN's (owner 2026-11-13, R1).
    if (inductance_off_band) out["value_differs"] = true;
    if (capacitance_off_band) out["value_differs"] = true;  // owner 2026-11-13, Z4
    return out;
}

// Rank a candidate list for an original. Returns {category, original_verified,
// candidates:[...]} sorted best-first (lowest penalty); no_substitute rows sink.
//: Categories the ranker has rules for. Any other would be compared on nothing
//: and every candidate would come back "recommended" — a 300 V varistor for a
//: 30 V one did — so an unknown category is refused, not ranked.
inline bool known_category(const std::string& c) {
    static const std::set<std::string> k{"resistor", "capacitor", "magnetic", "chipBead",
                                         "mosfet", "diode", "igbt", "connector", "analog",
                                         "timeBase", "varistor"};
    return k.count(c) > 0;
}

inline json cross_reference(const std::string& category, const json& original,
                            const json& candidates, const Options& opt = {}) {
    if (!known_category(category))
        throw std::invalid_argument("cross_reference: no ranking rules for category '" +
                                    category + "'");
    std::vector<json> scored;
    scored.reserve(candidates.size());
    for (const auto& c : candidates) scored.push_back(score_candidate(category, original, c, opt));
    // Order: viable candidates before rejects, then exact-value candidates before
    // different-value alternatives (resistors and off-nominal zeners, "value_differs"),
    // then penalty. The
    // value tier is a sort key, not a penalty, so no combination of other penalties
    // can lift a different-value part above an exact-value one.
    std::stable_sort(scored.begin(), scored.end(), [](const json& a, const json& b) {
        const bool ra = a.value("status", "") == "no_substitute",
                   rb = b.value("status", "") == "no_substitute";
        if (ra != rb) return rb;
        const bool va = a.value("value_differs", false), vb = b.value("value_differs", false);
        if (va != vb) return vb;
        return a.value("penalty", 1e18) < b.value("penalty", 1e18);
    });
    json cands = json::array();
    for (size_t i = 0; i < scored.size() && i < opt.max_results; ++i) cands.push_back(scored[i]);
    return {{"category", category},
            {"original_verified",
             opt.original_verified && !flag(original, "provenance_retracted")},
            {"candidates", cands}};
}

// JSON-options wrapper shared by both bindings (PyKelvin + embind WASM).
inline json cross_reference_json(const std::string& category, const json& original,
                                 const json& candidates, const json& options) {
    Options opt;
    if (options.contains("original_verified") && options["original_verified"].is_boolean())
        opt.original_verified = options["original_verified"].get<bool>();
    if (options.contains("max_results") && options["max_results"].is_number())
        opt.max_results = options["max_results"].get<size_t>();
    if (options.contains("primary_weight") && options["primary_weight"].is_number())
        opt.primary_weight = options["primary_weight"].get<double>();
    if (options.contains("check_footprint") && options["check_footprint"].is_boolean())
        opt.check_footprint = options["check_footprint"].get<bool>();
    if (options.contains("check_lifecycle") && options["check_lifecycle"].is_boolean())
        opt.check_lifecycle = options["check_lifecycle"].get<bool>();
    if (options.contains("operating_voltage") && options["operating_voltage"].is_number())
        opt.operating_voltage = options["operating_voltage"].get<double>();
    if (options.contains("requirements")) {
        const auto& req = options["requirements"];
        if (!req.is_object())
            throw std::invalid_argument("cross_reference: 'requirements' must be an object "
                                        "{parameter: minimum}");
        for (const auto& [key, v] : req.items()) {
            if (!v.is_number())
                throw std::invalid_argument("cross_reference: requirement '" + key +
                                            "' must be a number");
            opt.requirements[key] = v.get<double>();
        }
    }
    return cross_reference(category, original, candidates, opt);
}

}  // namespace kelvin::crossref
