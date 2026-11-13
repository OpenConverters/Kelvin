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
// For such a record, the distinct retractionReason labels of its entries joined with "; "
// (in record order); "" when not every entry is retracted or none states a reason. Qarlos r26
// 31990757: the Vishay RGF1K's reason reached neither the ranker spec nor any note.
std::string all_provenance_retraction_reason(const json& env);

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

// A PRIMARY rating that rests only on retracted sources is NOT STATED (owner 2026-11-13, Z1):
// the value is removed from the row (NaN, the absent convention), so every pool filter, the
// browse view and the ranker see the record exactly as one that states no such rating — a
// hard floor on it (web/src/crossref.js hardMin / the zener range filter) leaves the record out
// of the pool. The ratings: diode working voltage (vrrm_rated from reverseVoltage, the TVS/ESD
// standoffVoltage or the zener breakdownVoltage, with the zener window vz_min/vz_max), MOSFET
// vds_rated (drainSourceVoltage), capacitor v_rated (ratedVoltage) and v_rated_ac
// (voltageRatedAcMax). Reads row.retracted_fields (retracted_only_fields above), so it applies
// to the per-field rule only; a wholly retracted record keeps its values (its own rule, CrossRef.hpp).
//
// The same holds for each family's PRIMARY VALUE (owner 2026-11-13, C): a value resting only on
// retracted sources is not stated, so an original is refused by the missing-primary rules and a
// candidate is left out by every filter on it, exactly as a record that states none. The values:
// capacitor capacitance (electrical.capacitance, with the band read from it), resistor
// resistance (electrical.resistance), varistor voltage (electrical.varistorVoltage), magnetic
// inductance (electrical[].inductance, with its tolerance band) and chip-bead impedance
// (electrical[].impedancePoints: Z@100 MHz and the peak), timing frequency
// (electrical.frequency) and the programmable span (electrical.frequencyRange), and connector
// positions (mechanical.positions).
// Returns how many ratings / values were removed.
template <class Row>
int unstate_retracted_ratings(Row&) { return 0; }
int unstate_retracted_ratings(DiodeRow& r);
int unstate_retracted_ratings(MosfetRow& r);
int unstate_retracted_ratings(CapacitorRow& r);
int unstate_retracted_ratings(ResistorRow& r);
int unstate_retracted_ratings(VaristorRow& r);
int unstate_retracted_ratings(MagneticRow& r);
int unstate_retracted_ratings(TimingRow& r);
int unstate_retracted_ratings(ConnectorRow& r);

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
