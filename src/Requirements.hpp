// designRequirements (SAS/CAS/RAS/CTAS schema block) -> typed constraints.
// Faithful port of heaviside/catalogue/kirchhoff_fill.py `_*_constraints`, including its
// selection policies (HV -> total-loss op-point attach, resonant-cap ±15% band, cap 2x
// oversize cap, control-resistor exact-value handling is done at stamp time, not here).
#pragma once
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "Constraints.hpp"

namespace kelvin {

using json = nlohmann::json;

// Above this rated Vds, attach an operating point so the MOSFET is ranked by total loss
// (kirchhoff_fill._HV_SWITCHING_LOSS_VDS = 100 V).
inline constexpr double kHvSwitchingLossVds = 100.0;
inline constexpr double kCapOversizeMax = 2.0;    // kirchhoff_fill._CAP_OVERSIZE_MAX
inline constexpr double kResonantCapTol = 0.15;   // kirchhoff_fill._RESONANT_CAP_TOL

// op_fsw: the converter switching frequency (Hz), supplied by the caller (KH/HS). When >0 and
// the stage is high-voltage, the returned constraints carry the op-point and the caller must
// use LOWEST_TOTAL_LOSS (mirrors kirchhoff_fill.fill_kirchhoff_bom's tiebreaker choice).
MosfetConstraints mosfet_constraints(const json& req, std::optional<double> op_fsw);
DiodeConstraints diode_constraints(const json& req);
CapacitorConstraints capacitor_constraints(const json& req);
ResistorConstraints resistor_constraints(const json& req);
ControllerConstraints controller_constraints(const json& req, const std::string& topology,
                                             double vin_nom, double fsw_hz);
IgbtConstraints igbt_constraints(const json& req);
BjtConstraints bjt_constraints(const json& req);
VaristorConstraints varistor_constraints(const json& req);

// Connector requirements -> constraints. Keys: positions (exact contact count),
// minimumCurrentPerContact|ratedCurrentPerContact, minimumRatedVoltage|ratedVoltage,
// family, matingPolarity|polarity. At least one gating key is required (validate()).
ConnectorConstraints connector_constraints(const json& req);

// Magnetic design requirements -> targets. Target inductance is the MAS `magnetizingInductance`
// (falling back to `inductance` / `desiredInductance`), resolved from a dimensionWithTolerance.
// Operating-point currents are read best-effort (peak/rms) — all fields are optional and this
// never throws, because the magnetic path returns ranked matches even for a spec-less seed.
MagneticConstraints magnetic_constraints(const json& req);

}  // namespace kelvin
