// Faithful C++ ports of Heaviside's `<Family>.from_envelope` typed-view projections
// (heaviside/catalogue/selector.py). Each returns std::nullopt exactly when the Python
// returns None (an "unreadable_row"): a required field missing / non-numeric / non-positive.
//
// These are the SINGLE extraction point shared by the streaming path and the index builder,
// so the two agree by construction (the [equiv] test guards it).
#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Rows.hpp"

namespace kelvin {

using json = nlohmann::json;

// datasheetUrl "unusable" — empty OR matches one of the librarian's BAD_DATASHEET_URL_PATTERNS
// (heaviside/librarian/guards.py). Used only for the MOSFET evidence tier.
bool datasheet_unusable(const std::string& url);

// CTAS intendedTopologies -> HS short topology name (selector.py _normalize_ctas_topology).
std::string normalize_ctas_topology(const std::string& name);

// True when the record states provenance and EVERY entry is retracted (ABT #2035).
// Family-agnostic; the index builder stamps it on every row.
bool all_provenance_retracted(const json& env);

// The datasheetInfo fields of a record that rest ONLY on retracted provenance (owner
// 2026-11-13, S1). A field is covered by a provenance entry when the entry names no `fields`
// (it covers the whole record) or names the field, a parent of it or a part of it. A STATED
// field covered by a retracted entry and by no live entry is retracted-only. Reported as the
// field's first two path segments with array indices dropped ("electrical.reverseVoltage",
// "mechanical.case"), sorted; `reason` joins the distinct retractionReason labels of the
// retracted entries that cover them. Empty when the record has no retracted entry, when every
// entry is retracted (all_provenance_retracted covers that), or when a live entry names no
// fields (it covers every field).
struct FieldRetraction {
    std::vector<std::string> fields;
    std::string reason;
};
FieldRetraction retracted_only_fields(const json& env);

// The function a resistor record STATES for itself (ResistorRow.current_sense /
// pulse_withstanding). Read from the RAS technology enum and the record's own class text
// (manufacturerInfo.family, part.series, part.description) — never from the MPN.
struct ResistorFunction {
    bool current_sense = false;
    bool pulse_withstanding = false;
};
ResistorFunction resistor_function_class(const std::string& technology,
                                         const std::vector<std::string>& class_texts);

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
