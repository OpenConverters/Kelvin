// Typed selection constraints — C++ mirror of the selector.py *Constraints dataclasses.
// Built either from a schema designRequirements block (Requirements.cpp, the port of
// kirchhoff_fill._*_constraints) or directly (the parity harness feeds these verbatim).
#pragma once
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

namespace kelvin {

// Thrown for invalid options/constraints (maps Python ValueError). Distinct from NoCandidates.
struct InvalidOptions : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class MosfetTiebreaker {
    LowestRdsOn,
    LowestQg,
    HighestVdsMargin,
    HighestIdMargin,
    LowestTotalLoss,
};
enum class DiodeTiebreaker {
    LowestVf,
    LowestQrr,
    HighestVrrmMargin,
    HighestIfMargin,
};
enum class CapacitorTiebreaker {
    LowestEsr,
    HighestRippleHeadroom,
    HighestVoltageMargin,
    HighestCapacitance,
};

const char* to_string(MosfetTiebreaker t);
const char* to_string(DiodeTiebreaker t);
const char* to_string(CapacitorTiebreaker t);
MosfetTiebreaker mosfet_tiebreaker_from_string(const std::string& s);
DiodeTiebreaker diode_tiebreaker_from_string(const std::string& s);
CapacitorTiebreaker capacitor_tiebreaker_from_string(const std::string& s);

struct MosfetConstraints {
    double vds_min = 0, id_min = 0, rds_on_max = 0, qg_max = 0;
    std::set<std::string> technology_allowed{"Si", "SiC", "GaN"};
    bool exclude_discontinued = true;
    std::optional<double> op_i_rms, op_vds, op_duty, op_fsw;
    void validate() const {
        auto pos = [](double v, const char* n) {
            if (!(v > 0)) throw InvalidOptions(std::string("MosfetConstraints.") + n +
                                               " must be a positive number");
        };
        pos(vds_min, "vds_min");
        pos(id_min, "id_min");
        pos(rds_on_max, "rds_on_max");
        pos(qg_max, "qg_max");
        if (technology_allowed.empty())
            throw InvalidOptions("MosfetConstraints.technology_allowed cannot be empty");
    }
};

struct DiodeConstraints {
    double vrrm_min = 0, if_avg_min = 0;
    std::optional<double> qrr_max;  // absent = no Qrr filter
    bool exclude_discontinued = true;
    void validate() const {
        if (!(vrrm_min > 0)) throw InvalidOptions("DiodeConstraints.vrrm_min must be positive");
        if (!(if_avg_min > 0)) throw InvalidOptions("DiodeConstraints.if_avg_min must be positive");
        if (qrr_max && *qrr_max < 0)
            throw InvalidOptions("DiodeConstraints.qrr_max must be non-negative");
    }
};

struct CapacitorConstraints {
    double capacitance_min = 0, capacitance_max = 0, v_rated_min = 0;
    std::optional<double> ripple_current_min;  // absent = skip filter (MLCC)
    std::set<std::string> technology_allowed;  // empty = any
    bool exclude_discontinued = true;
    void validate() const {
        if (!(capacitance_min > 0))
            throw InvalidOptions("CapacitorConstraints.capacitance_min must be positive");
        if (!(capacitance_max > 0))
            throw InvalidOptions("CapacitorConstraints.capacitance_max must be positive");
        if (!(v_rated_min > 0))
            throw InvalidOptions("CapacitorConstraints.v_rated_min must be positive");
        if (ripple_current_min && *ripple_current_min < 0)
            throw InvalidOptions("CapacitorConstraints.ripple_current_min must be non-negative");
        if (capacitance_min > capacitance_max)
            throw InvalidOptions("CapacitorConstraints.capacitance_min > capacitance_max");
    }
};

struct ControllerConstraints {
    std::string topology;
    double vin_nom = 0, fsw_khz = 0;
    std::optional<bool> integrated_fet;  // absent = don't care
    std::optional<std::string> category;  // absent = any
    void validate() const {}  // Python ControllerConstraints has no __post_init__
};

struct ResistorConstraints {
    double target_ohms = 0;
    double max_tolerance = 0.01;
    double max_value_deviation = 0.05;
    void validate() const {
        if (!(target_ohms > 0))
            throw InvalidOptions("ResistorConstraints.target_ohms must be positive");
    }
};

// ---- Phase 5 (no HS reference; physics-sensible bounds) ---------------------
enum class IgbtTiebreaker { LowestVceSat, HighestVcesMargin, HighestIcMargin };
enum class BjtTiebreaker { HighestHfe, HighestVceoMargin, HighestIcMargin };
enum class VaristorTiebreaker { LowestClampingVoltage, HighestSurge, LowestCapacitance };

const char* to_string(IgbtTiebreaker t);
const char* to_string(BjtTiebreaker t);
const char* to_string(VaristorTiebreaker t);
IgbtTiebreaker igbt_tiebreaker_from_string(const std::string& s);
BjtTiebreaker bjt_tiebreaker_from_string(const std::string& s);
VaristorTiebreaker varistor_tiebreaker_from_string(const std::string& s);

struct IgbtConstraints {
    double vces_min = 0, ic_min = 0;
    std::optional<double> vce_sat_max;  // maximumSaturationVoltage
    bool exclude_discontinued = true;
    void validate() const {
        if (!(vces_min > 0)) throw InvalidOptions("IgbtConstraints.vces_min must be positive");
        if (!(ic_min > 0)) throw InvalidOptions("IgbtConstraints.ic_min must be positive");
    }
};

struct BjtConstraints {
    double vceo_min = 0, ic_min = 0;
    std::optional<double> hfe_min;  // minimumDcCurrentGain
    bool exclude_discontinued = true;
    void validate() const {
        if (!(vceo_min > 0)) throw InvalidOptions("BjtConstraints.vceo_min must be positive");
        if (!(ic_min > 0)) throw InvalidOptions("BjtConstraints.ic_min must be positive");
    }
};

struct VaristorConstraints {
    double rated_continuous_voltage = 0;  // part maxContinuousDcVoltage must be >= this
    std::optional<double> max_clamping_voltage, min_peak_surge_current, max_capacitance;
    bool exclude_discontinued = true;
    void validate() const {
        if (!(rated_continuous_voltage > 0))
            throw InvalidOptions("VaristorConstraints.rated_continuous_voltage must be positive");
    }
};

// Magnetic constraints — deliberately ALL-OPTIONAL and never-throwing. The magnetic selector
// ranks the whole catalogue toward these targets and returns the top-N even if none satisfy them
// (the "magnetic-first fallback" the caller needs), so there is nothing to hard-validate: a
// missing target simply makes that scoring term neutral. None of these fields gate a candidate.
struct MagneticConstraints {
    std::optional<double> target_inductance;  // H — magnetizingInductance / inductance / desiredInductance
    std::optional<double> peak_current;       // A — operating-point peak (saturation-current headroom)
    std::optional<double> rms_current;        // A — operating-point rms (rated-current headroom)
    std::optional<double> target_turns_ratio; // — designRequirements.turnsRatios[0] (transformers)
    std::string kind;                          // "inductor"/"transformer" hint — annotation only, never gates
    void validate() const {}                   // never throws — imperfect matches are still returned
};

}  // namespace kelvin
