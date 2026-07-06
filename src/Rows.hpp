// Per-family extracted row views — the selector's working representation.
//
// Each struct is the C++ mirror of the corresponding Heaviside `from_envelope` typed view
// (heaviside/catalogue/selector.py). The field set is intentionally the exact subset the
// selector filters/ranks on, plus the identity (mpn/manufacturer) and the source locator
// (lineno + byte span into the NDJSON, used to fetch the full envelope for winners).
//
// A nullable numeric (e.g. rth_ja "None" in Python) is represented as NaN. Helpers below
// make the NaN<->present intent explicit so the selector code reads like the Python.
#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace kelvin {

enum class Family : uint32_t {
    Mosfet = 1,
    Diode = 2,
    Capacitor = 3,
    Resistor = 4,
    Controller = 5,
    // Phase 5 — new selectors (no HS reference; review-gated, physics-sensible bounds).
    Igbt = 6,
    Bjt = 7,
    Varistor = 8,
    // Magnetic (from-spec): unlike the parametric families this one RANKS the whole catalogue
    // and returns the top-N even when nothing meets the spec (no hard gate) — see select_magnetic.
    Magnetic = 9,
};

inline const char* family_name(Family f) {
    switch (f) {
        case Family::Mosfet: return "mosfet";
        case Family::Diode: return "diode";
        case Family::Capacitor: return "capacitor";
        case Family::Resistor: return "resistor";
        case Family::Controller: return "controller";
        case Family::Igbt: return "igbt";
        case Family::Bjt: return "bjt";
        case Family::Varistor: return "varistor";
        case Family::Magnetic: return "magnetic";
    }
    return "unknown";
}

inline const char* family_file(Family f) {
    switch (f) {
        case Family::Mosfet: return "mosfets.ndjson";
        case Family::Diode: return "diodes.ndjson";
        case Family::Capacitor: return "capacitors.ndjson";
        case Family::Resistor: return "resistors.ndjson";
        case Family::Controller: return "controllers.ndjson";
        case Family::Igbt: return "igbts.ndjson";
        case Family::Bjt: return "bjts.ndjson";
        case Family::Varistor: return "varistors.ndjson";
        case Family::Magnetic: return "magnetics.ndjson";
    }
    return "";
}

inline double kNaN() { return std::numeric_limits<double>::quiet_NaN(); }
inline bool present(double x) { return !std::isnan(x); }

// Common locator carried by every family row.
struct RowBase {
    uint32_t lineno = 0;      // 1-based source line (the determinism tie-break key)
    uint64_t src_offset = 0;  // byte offset of the raw line start in the NDJSON
    uint32_t src_length = 0;  // raw line length (bytes, excluding the newline)
    std::string mpn;
    std::string manufacturer;
};

struct MosfetRow : RowBase {
    double vds_rated = 0, id_continuous = 0, rds_on = 0, qg_total = 0, coss = 0;
    double vgs_threshold_max = 0;
    double rth_ja = kNaN(), rth_jc = kNaN(), tj_max = kNaN();
    std::string technology;   // Si / SiC / GaN (part.technology)
    bool is_production = false;
    bool datasheet_usable = false;  // !_datasheet_unusable(datasheetUrl), precomputed at index time
    // Python evidence tier: qg_total <= 0 OR datasheet unusable.
    bool evidence_incomplete() const { return qg_total <= 0.0 || !datasheet_usable; }
    bool no_thermal() const { return !present(rth_ja) || !present(tj_max); }
};

struct DiodeRow : RowBase {
    double vrrm_rated = 0, if_avg_rated = 0, vf_typ = 0, qrr = 0, trr = 0;
    double rth_ja = kNaN(), rth_jc = kNaN(), tj_max = kNaN();
    std::string technology;   // part.subType (schottky / fastRecovery / ...)
    bool is_production = false;
    bool no_thermal() const { return !present(rth_ja) || !present(tj_max); }
};

struct CapacitorRow : RowBase {
    double capacitance = 0, v_rated = 0, ripple_current_rms = 0, esr = 0;
    double rth = kNaN();
    std::string technology;   // part.family|subType|series
    bool is_production = false;
    bool no_thermal() const { return !present(rth); }
};

struct ResistorRow : RowBase {
    double resistance = 0, tolerance = 0, power_rating = 0;
    bool is_production = false;  // not filtered on today, carried for completeness/evidence
};

struct IgbtRow : RowBase {
    double vces_rated = 0, ic_continuous = 0, vce_sat = 0;
    double rth_jc = kNaN(), tj_max = kNaN();
    std::string technology;  // Si / SiC (part.technology)
    bool is_production = false;
    bool no_thermal() const { return !present(tj_max); }
};

struct BjtRow : RowBase {
    double vceo_rated = 0, ic_continuous = 0, hfe_min = 0, power_dissipation = 0;
    double tj_max = kNaN();
    std::string technology;
    bool is_production = false;
    bool no_thermal() const { return !present(tj_max); }
};

struct VaristorRow : RowBase {
    double varistor_voltage = 0, clamping_voltage = 0, peak_surge_current = 0;
    double max_continuous_dc_voltage = 0, capacitance = 0;
    std::string technology;
    bool is_production = false;
};

// A catalogue magnetic (inductor / transformer / coupled-inductor / choke / chip-bead). Every
// electrical datum is nullable (NaN when the datasheet omits it) — that is deliberate: the
// magnetic selector RANKS on what is present rather than gating on it, so a part with a null
// saturation current is deprioritised, never dropped. Fields are the primary-winding projection
// of manufacturerInfo.datasheetInfo.electrical[0] (the array's first entry).
struct MagneticRow : RowBase {
    double inductance = kNaN();           // H  — electrical[0].inductance (nominal)
    double saturation_current = kNaN();   // A  — saturationCurrentPeak, else the ~20%-drop table headline
    double rated_current = kNaN();        // A  — max(ratedCurrents[])
    double dcr = kNaN();                  // Ω  — dcResistance (worst-case / maximum), else dcResistances[0]
    double srf = kNaN();                  // Hz — selfResonantFrequency
    double turns_ratio = kNaN();          // —  electrical[0].turnsRatios[0] (transformers): primary:secondary
    std::string device_type;              // electrical[0].subtype (inductor/transformer/…): annotated, never gated
    std::string family;                   // manufacturerInfo.family or part.family (series, for context)
    bool is_production = false;
    bool has_inductance() const { return present(inductance); }
};

struct ControllerRow : RowBase {
    double vref = kNaN();  // referenceVoltage.nominal, NaN when absent
    std::string category;  // function.category
    std::vector<std::string> topologies;  // normalized HS short names
    bool integrated_fet = false;
    bool integrated_driver = false;
    bool has_vref() const { return present(vref); }
    // CTAS carries no Vin/fsw ranges -> permissive window (matches selector.py).
    double vin_min() const { return 0.0; }
    double vin_max() const { return 1e12; }
    double fsw_min_khz() const { return 0.0; }
    double fsw_max_khz() const { return 1e12; }
};

}  // namespace kelvin
