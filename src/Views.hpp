// Faithful C++ ports of Heaviside's `<Family>.from_envelope` typed-view projections
// (heaviside/catalogue/selector.py). Each returns std::nullopt exactly when the Python
// returns None (an "unreadable_row"): a required field missing / non-numeric / non-positive.
//
// These are the SINGLE extraction point shared by the streaming path and the index builder,
// so the two agree by construction (the [equiv] test guards it).
#pragma once
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "Rows.hpp"

namespace kelvin {

using json = nlohmann::json;

// datasheetUrl "unusable" — empty OR matches one of the librarian's BAD_DATASHEET_URL_PATTERNS
// (heaviside/librarian/guards.py). Used only for the MOSFET evidence tier.
bool datasheet_unusable(const std::string& url);

// CTAS intendedTopologies -> HS short topology name (selector.py _normalize_ctas_topology).
std::string normalize_ctas_topology(const std::string& name);

// Extractors. Locator fields (lineno/src_offset/src_length) are left default; the caller sets them.
std::optional<MosfetRow> extract_mosfet(const json& env);
std::optional<DiodeRow> extract_diode(const json& env);
std::optional<CapacitorRow> extract_capacitor(const json& env);
std::optional<ResistorRow> extract_resistor(const json& env);
std::optional<ControllerRow> extract_controller(const json& env);
std::optional<IgbtRow> extract_igbt(const json& env);
std::optional<BjtRow> extract_bjt(const json& env);
std::optional<VaristorRow> extract_varistor(const json& env);
std::optional<MagneticRow> extract_magnetic(const json& env);
std::optional<AnalogRow> extract_analog(const json& env);
std::optional<TimingRow> extract_timing(const json& env);
std::optional<ConnectorRow> extract_connector(const json& env);

}  // namespace kelvin
