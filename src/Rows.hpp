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
    // Browse-only families (no requirements emitter → no selector yet; the Kelvin site
    // catalogues them and select() refuses them loudly).
    Analog = 10,
    Timing = 11,
    Connector = 12,
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
        case Family::Analog: return "analog";
        case Family::Timing: return "timing";
        case Family::Connector: return "connector";
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
        case Family::Analog: return "analog_ics.ndjson";
        case Family::Timing: return "timing_devices.ndjson";
        case Family::Connector: return "connectors.ndjson";
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
    // Physical body size, metres, from datasheetInfo.mechanical. NaN when the
    // record carries no drawing — the cross-reference footprint check then falls
    // back to resolving `case_code`, and reports "unknown" if that fails too
    // rather than assuming the part fits.
    double length_m = kNaN();
    double width_m = kNaN();
    double height_m = kNaN();
    std::string case_code;  // part.case / mechanical.case — the fallback size source
    // mechanical.assemblyType (smt / tht / chassis): an EXPLICIT mount type, far
    // more reliable than inferring it from a package string, which is all
    // Heaviside had. Empty when the record does not state it.
    std::string mount;
};

struct MosfetRow : RowBase {
    double vds_rated = 0, id_continuous = 0, rds_on = 0, qg_total = 0, coss = 0;
    double vgs_threshold_max = 0;
    // Rds(on) is only meaningful at the Vgs it was measured at: a logic-level
    // part specified at 4.5 V and a standard part at 10 V are not comparable as
    // bare scalars. Distributor tables that omit this (Mouser) make the two
    // indistinguishable, which is a documented substitution hazard.
    double rds_on_vgs = kNaN();
    double vgs_max = kNaN();       // absolute gate-source rating
    std::string qualification;     // AEC-Q101 etc., when the datasheet states it
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
    std::string technology;      // part.technology (ceramic-class-2, film-…)
    std::string dielectric_code; // part.dielectricCode (X7R / C0G / X5R …)
    // ESR without its measurement frequency is not comparable — a 120 Hz figure
    // and a 100 kHz figure differ severalfold on the same part.
    double esr_frequency = kNaN();
    double temp_min_c = kNaN();
    double temp_max_c = kNaN();
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

// A catalogue analog IC (op-amp / comparator / ADC / DAC / switch / mux / ...). Browse-only:
// every datum is nullable and nothing gates — the row is the parametric-search projection of
// analog.<subtype>.manufacturerInfo.datasheetInfo.electrical.
struct AnalogRow : RowBase {
    double channels = kNaN();             // numberOfChannels
    double input_offset_voltage = kNaN(); // V
    double input_bias_current = kNaN();   // A
    double gain_bandwidth = kNaN();       // Hz — gainBandwidthProduct
    double slew_rate = kNaN();            // V/s
    double cmrr = kNaN();                 // dB
    double vsupply_min = kNaN();          // V — supply.minimumSupplyVoltage
    double vsupply_max = kNaN();          // V — supply.maximumSupplyVoltage
    double resolution = kNaN();           // bits (ADC/DAC)
    double sample_rate = kNaN();          // S/s (ADC)
    double on_resistance = kNaN();        // Ω (switch/mux)
    std::string device_type;              // the analog.<subtype> key (operationalAmplifier, adc, …)
    std::string architecture;
    std::string input_stage;
    bool is_production = false;
};

// A catalogue timing device (quartz crystal / MEMS / XO / TCXO / ...). Browse-only projection
// of timeBase.<subtype>.manufacturerInfo.datasheetInfo.electrical.
struct TimingRow : RowBase {
    double frequency = kNaN();            // Hz
    double frequency_tolerance = kNaN();  // fraction (±)
    double frequency_stability = kNaN();  // fraction (±)
    double load_capacitance = kNaN();     // F
    double esr = kNaN();                  // Ω — equivalentSeriesResistance
    double rms_phase_jitter = kNaN();     // s
    double vsupply_min = kNaN();          // V — supply.minimumSupplyVoltage
    double vsupply_max = kNaN();          // V
    std::string device_type;              // the timeBase.<subtype> key (oscillator / timer)
    std::string technology;               // quartzCrystal / mems / crystalOscillator / tcxo / …
    std::string output_type;
    std::string mode;
    bool is_production = false;
};

// A catalogue connector. Browse-only projection of connector.manufacturerInfo.datasheetInfo
// (electrical.ratedCurrentPerContact/ratedVoltage, mechanical.positions, familyDetails, part).
struct ConnectorRow : RowBase {
    double rated_current = kNaN();  // A per contact
    double rated_voltage = kNaN();  // V
    double positions = kNaN();      // contact count
    std::string family;             // familyDetails.family (boardToBoard / terminalBlock / …)
    std::string interface_standard; // familyDetails.interfaceStandard (when standardised)
    std::string polarity;           // part.matingPolarity (male / female / hermaphroditic)
    std::string series;             // part.series
    bool is_production = false;
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
