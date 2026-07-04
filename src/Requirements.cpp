#include "Requirements.hpp"

#include <cmath>

#include "DimensionJson.hpp"  // PEAS::resolve_dimensional_values

namespace kelvin {
namespace {

// req[key] must be a number; throw with the field name (no silent fallback — mirrors the
// Python float(req["..."]) KeyError/TypeError surfacing as a loud failure).
double req_num(const json& req, const char* key) {
    if (!req.is_object() || !req.contains(key) || !req.at(key).is_number())
        throw InvalidOptions(std::string("designRequirements missing numeric '") + key + "'");
    return req.at(key).get<double>();
}

// A required dimensionWithTolerance input field, collapsed with the canonical resolver (never a
// hand-read of `.nominal`). Throws if the field is absent or unresolvable (no silent fallback).
double req_dim(const json& req, const char* key) {
    if (!req.is_object() || !req.contains(key))
        throw InvalidOptions(std::string("designRequirements missing '") + key + "'");
    try {
        return PEAS::resolve_dimensional_values(req.at(key));
    } catch (const std::exception& e) {
        throw InvalidOptions(std::string("designRequirements '") + key + "' unresolvable: " +
                             e.what());
    }
}

std::optional<double> opt_num(const json& req, const char* key) {
    if (req.is_object() && req.contains(key) && req.at(key).is_number())
        return req.at(key).get<double>();
    return std::nullopt;
}

std::string opt_str(const json& req, const char* key) {
    if (req.is_object() && req.contains(key) && req.at(key).is_string())
        return req.at(key).get<std::string>();
    return std::string();
}

}  // namespace

MosfetConstraints mosfet_constraints(const json& req, std::optional<double> op_fsw) {
    double vds_min = req_num(req, "ratedDrainSourceVoltage");
    double id_min = req_num(req, "ratedContinuousDrainCurrent");
    double rds_on_max = req_num(req, "maximumOnResistance");
    MosfetConstraints c;
    c.vds_min = vds_min;
    c.id_min = id_min;
    c.rds_on_max = rds_on_max;
    c.qg_max = INFINITY;  // Kirchhoff emits no gate-charge limit (Python math.inf)
    if (op_fsw && *op_fsw > 0 && vds_min > kHvSwitchingLossVds) {
        c.op_i_rms = id_min;
        c.op_vds = vds_min;
        c.op_duty = 0.5;
        c.op_fsw = *op_fsw;
    }
    return c;
}

DiodeConstraints diode_constraints(const json& req) {
    DiodeConstraints c;
    c.vrrm_min = req_num(req, "ratedReverseVoltage");
    c.if_avg_min = req_num(req, "ratedForwardCurrent");
    return c;
}

CapacitorConstraints capacitor_constraints(const json& req) {
    double cnom = req_dim(req, "capacitance");
    auto ripple = opt_num(req, "minimumRippleCurrent");
    double v_rated = req_num(req, "ratedVoltage");
    CapacitorConstraints c;
    c.v_rated_min = v_rated;
    c.ripple_current_min = ripple;
    if (opt_str(req, "role") == "resonant") {
        c.capacitance_min = cnom * (1.0 - kResonantCapTol);
        c.capacitance_max = cnom * (1.0 + kResonantCapTol);
    } else {
        c.capacitance_min = cnom;
        c.capacitance_max = cnom * kCapOversizeMax;
    }
    return c;
}

ResistorConstraints resistor_constraints(const json& req) {
    ResistorConstraints c;
    c.target_ohms = req_dim(req, "resistance");
    auto tol = opt_num(req, "tolerance");
    c.max_tolerance = tol.value_or(0.05);
    c.max_value_deviation = 0.2;  // kirchhoff_fill uses ±20% for the power-path match
    return c;
}

ControllerConstraints controller_constraints(const json& req, const std::string& topology,
                                             double vin_nom, double fsw_hz) {
    ControllerConstraints c;
    c.topology = topology;
    c.vin_nom = vin_nom;
    c.fsw_khz = fsw_hz / 1000.0;
    c.integrated_fet = std::nullopt;  // kirchhoff_fill passes integrated_fet=None
    std::string cat = opt_str(req, "category");
    if (!cat.empty()) c.category = cat;
    return c;
}

// ---- tiebreaker <-> string -------------------------------------------------
const char* to_string(MosfetTiebreaker t) {
    switch (t) {
        case MosfetTiebreaker::LowestRdsOn: return "lowest_rds_on";
        case MosfetTiebreaker::LowestQg: return "lowest_qg";
        case MosfetTiebreaker::HighestVdsMargin: return "highest_vds_margin";
        case MosfetTiebreaker::HighestIdMargin: return "highest_id_margin";
        case MosfetTiebreaker::LowestTotalLoss: return "lowest_total_loss";
    }
    return "";
}
const char* to_string(DiodeTiebreaker t) {
    switch (t) {
        case DiodeTiebreaker::LowestVf: return "lowest_vf";
        case DiodeTiebreaker::LowestQrr: return "lowest_qrr";
        case DiodeTiebreaker::HighestVrrmMargin: return "highest_vrrm_margin";
        case DiodeTiebreaker::HighestIfMargin: return "highest_if_margin";
    }
    return "";
}
const char* to_string(CapacitorTiebreaker t) {
    switch (t) {
        case CapacitorTiebreaker::LowestEsr: return "lowest_esr";
        case CapacitorTiebreaker::HighestRippleHeadroom: return "highest_ripple_headroom";
        case CapacitorTiebreaker::HighestVoltageMargin: return "highest_voltage_margin";
        case CapacitorTiebreaker::HighestCapacitance: return "highest_capacitance";
    }
    return "";
}
MosfetTiebreaker mosfet_tiebreaker_from_string(const std::string& s) {
    if (s == "lowest_rds_on") return MosfetTiebreaker::LowestRdsOn;
    if (s == "lowest_qg") return MosfetTiebreaker::LowestQg;
    if (s == "highest_vds_margin") return MosfetTiebreaker::HighestVdsMargin;
    if (s == "highest_id_margin") return MosfetTiebreaker::HighestIdMargin;
    if (s == "lowest_total_loss") return MosfetTiebreaker::LowestTotalLoss;
    throw InvalidOptions("unknown mosfet tiebreaker: " + s);
}
DiodeTiebreaker diode_tiebreaker_from_string(const std::string& s) {
    if (s == "lowest_vf") return DiodeTiebreaker::LowestVf;
    if (s == "lowest_qrr") return DiodeTiebreaker::LowestQrr;
    if (s == "highest_vrrm_margin") return DiodeTiebreaker::HighestVrrmMargin;
    if (s == "highest_if_margin") return DiodeTiebreaker::HighestIfMargin;
    throw InvalidOptions("unknown diode tiebreaker: " + s);
}
CapacitorTiebreaker capacitor_tiebreaker_from_string(const std::string& s) {
    if (s == "lowest_esr") return CapacitorTiebreaker::LowestEsr;
    if (s == "highest_ripple_headroom") return CapacitorTiebreaker::HighestRippleHeadroom;
    if (s == "highest_voltage_margin") return CapacitorTiebreaker::HighestVoltageMargin;
    if (s == "highest_capacitance") return CapacitorTiebreaker::HighestCapacitance;
    throw InvalidOptions("unknown capacitor tiebreaker: " + s);
}

}  // namespace kelvin
