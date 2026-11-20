#include "Index.hpp"

#include <cstring>
#include <cstdio>
#ifdef _WIN32
#include <process.h>          // _getpid: MSVC has no <unistd.h>
#else
#include <unistd.h>
#endif
#include <algorithm>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "Hash.hpp"
#include "Views.hpp"

namespace kelvin {
namespace {

using json = nlohmann::json;

static_assert(sizeof(double) == 8, "double must be 8 bytes");

constexpr char kMagic[8] = {'K', 'E', 'L', 'V', 'I', 'D', 'X', '\1'};
// v2 added RowBase dimensions (length/width/height, case code, mount type) for
// the cross-reference footprint and mount-type checks.
// v3 added the substitution-condition fields: Rds(on) test Vgs, Vgs max and
// qualification on MOSFETs; ESR measurement frequency and operating temperature
// range on capacitors. These are the CONDITIONS under which a headline number
// was measured — without them, comparing the numbers manufactures a verdict.
// An older shard is rejected rather than read leniently: the field offsets
// moved, so a lenient read would silently mis-parse every row.
// v4 added chip-bead impedance-curve summaries (Z@100MHz, peak |Z| and its
// frequency) derived from the measured curve — parameters no vendor tabulates.
// v5 added extractor_hash: WHICH CODE produced the rows, not just which bytes
// they came from. See the comment on kExtractorHash below.
// v6 added the resistor family/series string — the device class, i.e. whether the
// part is a discrete two-terminal chip or a multi-element array/network. Without it
// the cross-reference compared body outlines only and graded discrete 1206 chips as
// drop-ins for a 4-element 3.2 x 1.6 mm array (ABT #481).
// v7 added the connector pitch — the only land-pattern datum a connector record carries.
// Without it the cross-reference had nothing to compare and graded a 5.00 mm-pitch part a
// drop_in for a 2.00 mm one, with the caveat asserting the catalogue had no pitch at all
// while every raw record held it (ABT #485).
// v8 added the connector operating-temperature range — the environmental limit 365,680 of
// the 391,073 records state. Without it the cross-reference had no temperature verdict at
// all and called a +105 degC part an "upgrade" over a +125 degC original (ABT #520).
// v9 added the three mating fields the connector family caveat asserted the catalogue did
// not hold: contact plating (97,144 records), termination (9,908) and mating cycles
// (56,573). The caveat was written to describe what the extractor read, not what the
// records say, so it told the engineer a checkable gate was uncheckable (ABT #487).
// v10 added the diode breakdown-voltage band (both bounds + the grade they define). The
// extractor collapsed breakdownVoltage to its nominal, so the window 3,728 zeners and
// 137 TVS guarantee was dropped and a nominal-only candidate scored "vrrm: pass" against
// a 1.11 % original with nothing said about the grade (ABT #488).
// v11 added the capacitor family/series string — the ONLY evidence a TAS capacitor
// record carries of an IEC 60384-14 line-safety approval (X1/X2/X3, Y1/Y2/Y3/Y4);
// the schema has no safety-class field at all (ABT #677) and the vendors write the
// class into the series name. Without it the cross-reference had only voltages to
// compare and graded six KEMET R46 (X2, 275 VAC) parts 'recommended' / 'drop_in' /
// 'upgrade' against a WIMA MKP-X1 R mains capacitor, voltage "pass", notes null —
// a safety-approval downgrade presented as a clean upgrade (ABT #557).
// v12 added the capacitor's equivalent series INDUCTANCE (CAS modelParams.ls).
// It is the one quantity a layout cannot supply and a package name cannot stand
// in for — inside a single 0402 the real spread is 120 to 1392 pH, so Faraday's
// "0402 -> 0.4 nH" table was not an approximation of it. 3,544 Wuerth parts
// carry a per-part figure as of TAS 3589b2e; before that 745 records shared one
// value and no ceramic had one at all (ABT #1122).
// v14 reads P-channel MOSFET and PNP ratings as magnitudes and records the polarity
// (MosfetRow.channel, BjtRow.polarity), and indexes AC-only capacitors with their AC
// rating (CapacitorRow.v_rated_ac; v_rated absent). Before it, 919 signed P-channel
// MOSFETs, 571 PNP transistors and 2,275 AC-only capacitors were unreadable rows, and
// the 493 P-channel MOSFETs stored with positive ratings were indexed as if N-channel
// (ABT #1749).
// v15 records the inductance-drop criterion of a magnetic's headline Isat
// (MagneticRow.saturation_percent_drop), so the ranker can refuse to compare a 30 %-drop
// rating against a 10 %-drop one as if they were the same number (ABT #2042).
// v16 carries a timing device's operating-temperature range (TimingRow.temp_min_c /
// temp_max_c), which the cross-reference compares (ABT #2033, #2046).
// v17 carries a tvs/esd diode's surge, clamp and capacitance specs with their stated
// waveforms (ABT #2036, #2040, #2041, #2048-#2051), and on EVERY family's row whether all of
// the record's provenance is retracted (RowBase.provenance_retracted, ABT #2035).
// v18 carries a diode's continuous power-dissipation rating (DiodeRow.power_dissipation),
// which the cross-reference compares, and reads a zener's vrrm_rated from its breakdown
// voltage (its Vz) only, never from reverseVoltage (Qarlos 2026-11-08).
// v18 carries a resistor's power-rating temperature and its stated derating curve
// (ResistorRow.power_rating_temperature / derating_temperature / derating_amplitude), so the
// cross-reference compares power at the original's rating temperature.
// v19 carries a connector's mating-axis orientation (ConnectorRow.orientation) and, on EVERY
// family's row, the lead pitch of a leaded part (RowBase.lead_pitch_m) — the land datum of a
// through-hole body, which the cross-reference footprint check compares (Qarlos 2026-11-08).
// v19 carries a diode's stated die configuration and internal connection
// (DiodeRow.die_configuration / internal_connection), the basis of its forward current: a
// dual package's forwardCurrent is per leg (Qarlos 2026-11-08).
// v19 carries a resistor's resistive-element technology, TCR, operating-temperature range
// and stated function (ResistorRow.technology / tcr / temp_min_c / temp_max_c /
// current_sense / pulse_withstanding), which the cross-reference compares (Qarlos
// 2026-11-08: a 175 C metal film took 155 C parts as pass, a current-sense chip took
// general-purpose thick film as drop_in, a 4x worse TCR went unmentioned).
// v20 carries a connector's row count (ConnectorRow.rows, mechanical.rows): positions / rows
// is the row length the cross-reference's pin-field check runs over (Qarlos 2026-11-08: a
// 24-way 5.00 mm strip passed against a 5.08 mm original, end pin 1.84 mm off its hole),
// and a terminal block's pluggable / fixed form (ConnectorRow.pluggable, tri-state).
// v20 carries a capacitor's stated safety class (CapacitorRow.safety_class, CAS
// part.safetyClass.class) and the frequency its ripple current is stated at
// (CapacitorRow.ripple_current_frequency), and a magnetic's operating-temperature range
// (MagneticRow.temp_min_c / temp_max_c), all of which the cross-reference compares (Qarlos
// 2026-11-09: an X2 original took non-safety parts, a 100 Hz ripple rating was failed against a
// 20 kHz one, a 200 degC inductor took 125 degC parts with no caveat).
// v20 also carries a capacitor's guaranteed capacitance band (CapacitorRow.capacitance_min /
// capacitance_max, farads), its leaded form (CapacitorRow.lead_form: axial / radial) and its
// qualified reliability level (CapacitorRow.qualified_spec / qualified_grade /
// qualified_failure_rate), all compared by the cross-reference (Qarlos 2026-11-09: a +/-5 %
// part took a +/-1 % original's grade, axial parts were offered for a radial original, a
// MIL-PRF-55681 grade S original took commercial parts with no caveat).
// v20 carries a timing device's control-pin function (TimingRow.enable_function, TDAS
// electrical.enableFunction), which the cross-reference compares as pinout identity (Qarlos
// 2026-11-09: an output-enable XO took standby parts as drop_in).
// v20 carries a magnetic's whole saturation table (MagneticRow.sat_table_current /
// sat_table_percent_drop / sat_table_temperature), not only the point nearest 20 %, so an
// unknown-criterion Isat is held against the substitute's 10-30 % curve (Qarlos 2026-11-08).
// v20 carries RowBase.box_axes_ambiguous: a leaded L x W x T body with no height whose record
// does not say which of L and W stands up from the board (vendors' drawings disagree), so
// the cross-reference reports its footprint UNVERIFIED instead of guessing (2026-11-09).
// v21 carries a resistor's maximum working voltage (ResistorRow.max_voltage, RAS
// electrical.maxVoltage) and the reverse voltage a diode's junction capacitance is stated at
// (DiodeRow.junction_capacitance_vr), both compared by the cross-reference (Qarlos r8/r10:
// an 800 V HV chip resistor took parts with no voltage check; a Schottky's 250 pF @ 1 V was
// passed against 170 pF @ 48 V).
// v21 also carries the drain-source voltage a MOSFET's capacitances are stated at
// (MosfetRow.coss_vds, SAS electrical.capacitanceMeasurementVds), the basis of its Coss
// (Qarlos r11: a 5 pF Coss read near 400 V was passed against 53 pF at 25 V).
// v21 also carries a ceramic resonator's built-in load capacitors (TimingRow.built_in_capacitance,
// TDAS electrical.builtInCapacitance), compared by the cross-reference (Qarlos r11/r13: a
// 15 pF resonator took 22 pF and capacitor-less parts with no verdict).
// v22 carries a timing device's programmable frequency range (TimingRow.frequency_min /
// frequency_max, TDAS electrical.frequencyRange, ab9f3c4): a programmable blank is admitted
// for a programmable original only when its range contains the original's frequency, or
// covers the original's range (owner 2026-11-13). A new TimingRow field changes what a shard
// row serialises, so every v21 shard must be rebuilt and both engines redeployed together.
// v22 (not yet deployed when added, so no bump) also carries a timing device's supply current
// (TimingRow.current_consumption, TDAS electrical.supply.currentConsumption; owner 2026-11-13),
// and a capacitor's ripple-rating temperature (CapacitorRow.ripple_current_temperature, CAS
// electrical.rippleCurrentTemperature; Qarlos r26), and (owner 2026-11-14, capacitor
// operating-point selector) a capacitor's distributor price quotes and its dissipation factor
// with the frequency it is stated at.
// v23 (owner 2026-11-15) moves every row's measured curves (CurvePts: a capacitor's ESR / |Z| /
// C(f) / ESL / DC-bias / ripple tables, a magnetic's L(I) / |Z| / R / X / CMC curves and rated
// points, a MOSFET's capacitance / Rds(on) / gate-charge / ambient-rating curves, a diode's
// Vf / Ir / Cj curves, a connector's derating) out of <family>.kidx into a separate curve shard
// <family>.curves.kidx, keyed by row index, which the browser fetches only when something reads
// curves. The base header gains the curve shard's build id (so the base build id covers it) and
// the curve shard's header names the base build id: a curve shard of another build is refused.
// The curve shard's own magic, so a base shard handed to the curve loader (or the reverse) is
// refused by its first 8 bytes rather than misparsed.
constexpr char kCurveMagic[8] = {'K', 'E', 'L', 'V', 'C', 'R', 'V', '\1'};
// v24 carries a BJT's transition frequency and saturation voltage (BjtRow.transition_frequency,
// vce_sat; tj_max is now filled), an IGBT's Vce(sat) test current (IgbtRow.vce_sat_ic), a
// varistor's AC working voltage and energy rating (VaristorRow.max_continuous_ac_voltage,
// energy_absorption) and the analog fields the cross-reference's kAnalog table already
// compared but no row carried (AnalogRow.offset_drift, quiescent_current, propagation_delay,
// rail_to_rail_input / _output, output_stage; owner 2026-11-15).
// v24 also carries the lifecycle status every record states (RowBase.status,
// manufacturerInfo.status; "" when none; owner 2026-11-15): under excludeDiscontinued every
// selector keeps a part with no stated status (one more UNVERIFIED item) and drops only a
// stated non-production one, which is_production cannot tell apart. Numbered 24: one above the v23
// curve-shard format; every v23 shard must be rebuilt and both engines redeployed together.
constexpr uint32_t kFormatVersion = 24;

// Identity of the code that produces rows, so a cached shard can be recognised as
// having been built by a DIFFERENT extractor than the one running now (ABT #426).
//
// Both cache paths keyed on the source bytes alone and said nothing about the code:
//   * load_or_build() returns a cached shard verbatim when the NDJSON has not changed,
//     so an extractor fix on an unchanged catalogue was a total no-op;
//   * build_generic()'s incremental tail build reuses prev's rows for the whole prefix
//     and parses only appended lines with the new extractor.
// Concretely: extract_diode was fixed to index zener/tvs/esd, the rebuild reported
// 'diode: rows=7148 unreadable=10918' byte-identical to before, and the buildId changed
// (it is recomputed), so nothing looked stale. Only deleting diode.kidx by hand revealed
// the real result, rows=18066 unreadable=0.
//
// The value is a hash of the extractor SOURCES (Views.cpp/Views.hpp/Rows.hpp), computed
// by CMake and re-computed whenever those files change. That is deliberate over the two
// obvious alternatives: a hand-bumped constant relies on remembering — the exact failure
// this is meant to end — and __DATE__/__TIME__ would make two builds of the same commit
// produce different bytes, which breaks the byte-for-byte deploy verification.
//
// There is NO default. A build that does not define it is a build whose cache would be
// silently dishonest again, so it fails to compile instead.
#ifndef KELVIN_EXTRACTOR_HASH
#error "KELVIN_EXTRACTOR_HASH is not defined -- configure via CMake (see CMakeLists.txt). \
Without it a cached shard cannot be told apart from one built by different extractor code."
#endif
constexpr uint64_t kExtractorHash = KELVIN_EXTRACTOR_HASH;

// ---- string pool -----------------------------------------------------------
class StringPool {
   public:
    StringPool() { intern(std::string()); }  // "" always at ref 0
    uint32_t intern(const std::string& s) {
        auto it = map_.find(s);
        if (it != map_.end()) return it->second;
        uint32_t ref = static_cast<uint32_t>(blob_.size());
        uint32_t len = static_cast<uint32_t>(s.size());
        blob_.append(reinterpret_cast<const char*>(&len), 4);
        blob_.append(s);
        map_.emplace(s, ref);
        return ref;
    }
    const std::string& blob() const { return blob_; }

   private:
    std::string blob_;
    std::unordered_map<std::string, uint32_t> map_;
};

std::string pool_read(const char* base, size_t size, uint32_t ref) {
    if (ref + 4 > size) throw std::runtime_error("kelvin: corrupt shard (string ref OOB)");
    uint32_t len;
    std::memcpy(&len, base + ref, 4);
    if (ref + 4 + len > size) throw std::runtime_error("kelvin: corrupt shard (string len OOB)");
    return std::string(base + ref + 4, len);
}

// ---- archives --------------------------------------------------------------
struct WriteAr {
    std::string* buf;
    StringPool* pool;
    static constexpr bool writing = true;
    void u32(uint32_t& v) { buf->append(reinterpret_cast<const char*>(&v), 4); }
    void u64(uint64_t& v) { buf->append(reinterpret_cast<const char*>(&v), 8); }
    void dbl(double& v) { buf->append(reinterpret_cast<const char*>(&v), 8); }
    void str(std::string& s) {
        uint32_t ref = pool->intern(s);
        u32(ref);
    }
    void boolean(bool& b) {
        uint32_t v = b ? 1u : 0u;
        u32(v);
    }
    void strvec(std::vector<std::string>& v) {
        uint32_t n = static_cast<uint32_t>(v.size());
        u32(n);
        for (auto& s : v) str(s);
    }
    void dblvec(std::vector<double>& v) {  // v18
        uint32_t n = static_cast<uint32_t>(v.size());
        u32(n);
        for (auto& d : v) dbl(d);
    }
    void dblvecvec(std::vector<std::vector<double>>& v) {  // v22
        uint32_t n = static_cast<uint32_t>(v.size());
        u32(n);
        for (auto& x : v) dblvec(x);
    }
    // A float vector as a pooled blob (v22, curves): identical vectors are stored once.
    void fltvec(std::shared_ptr<const std::vector<float>>& v) {
        std::string blob;
        if (v && !v->empty())
            blob.assign(reinterpret_cast<const char*>(v->data()), v->size() * sizeof(float));
        str(blob);
    }
    void curve(CurvePts& c) {  // v22 (owner 2026-11-14)
        fltvec(c.x);
        fltvec(c.y);
        dbl(c.temperature);
        dbl(c.dc_bias);
        dbl(c.frequency);
        strvec(c.cond_keys);  // further conditions (MOSFET / diode / connector curves)
        dblvec(c.cond_vals);
        str(c.cond_text);
        boolean(c.unit_unstated);
        boolean(c.unit_inferred);
        str(c.unit_note);
    }
};

struct ReadAr {
    const char* rows_base;
    size_t rows_size;
    size_t pos = 0;
    const char* pool_base;
    size_t pool_size;
    static constexpr bool writing = false;
    void need(size_t n) {
        if (pos + n > rows_size) throw std::runtime_error("kelvin: corrupt shard (row data OOB)");
    }
    void u32(uint32_t& v) {
        need(4);
        std::memcpy(&v, rows_base + pos, 4);
        pos += 4;
    }
    void u64(uint64_t& v) {
        need(8);
        std::memcpy(&v, rows_base + pos, 8);
        pos += 8;
    }
    void dbl(double& v) {
        need(8);
        std::memcpy(&v, rows_base + pos, 8);
        pos += 8;
    }
    void str(std::string& s) {
        uint32_t ref;
        u32(ref);
        s = pool_read(pool_base, pool_size, ref);
    }
    void boolean(bool& b) {
        uint32_t v;
        u32(v);
        b = v != 0;
    }
    void strvec(std::vector<std::string>& v) {
        uint32_t n;
        u32(n);
        v.resize(n);
        for (auto& s : v) str(s);
    }
    void dblvec(std::vector<double>& v) {  // v18
        uint32_t n;
        u32(n);
        need(static_cast<size_t>(n) * 8);
        v.resize(n);
        for (auto& d : v) dbl(d);
    }
    void dblvecvec(std::vector<std::vector<double>>& v) {  // v22
        uint32_t n;
        u32(n);
        need(static_cast<size_t>(n) * 4);  // each inner array carries at least its count
        v.resize(n);
        for (auto& x : v) dblvec(x);
    }
    // Pooled float vectors decoded once per pool ref and shared between rows.
    std::unordered_map<uint32_t, std::shared_ptr<const std::vector<float>>> flt_cache{};
    void fltvec(std::shared_ptr<const std::vector<float>>& v) {
        uint32_t ref;
        u32(ref);
        if (ref == 0) {  // "" (ref 0): no curve
            v.reset();
            return;
        }
        if (auto it = flt_cache.find(ref); it != flt_cache.end()) {
            v = it->second;
            return;
        }
        const std::string blob = pool_read(pool_base, pool_size, ref);
        if (blob.size() % sizeof(float) != 0)
            throw std::runtime_error("kelvin: corrupt shard (float vector size)");
        auto out = std::make_shared<std::vector<float>>(blob.size() / sizeof(float));
        std::memcpy(out->data(), blob.data(), blob.size());
        v = out;
        flt_cache.emplace(ref, v);
    }
    void curve(CurvePts& c) {  // v22 (owner 2026-11-14)
        fltvec(c.x);
        fltvec(c.y);
        dbl(c.temperature);
        dbl(c.dc_bias);
        dbl(c.frequency);
        strvec(c.cond_keys);  // further conditions (MOSFET / diode / connector curves)
        dblvec(c.cond_vals);
        str(c.cond_text);
        boolean(c.unit_unstated);
        boolean(c.unit_inferred);
        str(c.unit_note);
    }
};

// A list of curves (v22): a MOSFET's ambient ratings (one per mounting / time base / Vgs), a
// magnetic's L(I) curves (one per stated temperature).
template <class Ar>
void curves(Ar& ar, std::vector<CurvePts>& v) {
    uint32_t n = static_cast<uint32_t>(v.size());
    ar.u32(n);
    if constexpr (!Ar::writing) {
        ar.need(static_cast<size_t>(n) * 4);  // each curve carries at least one pool ref
        v.resize(n);
    }
    for (auto& c : v) ar.curve(c);
}

template <class Ar>
void io_base(Ar& ar, RowBase& r) {
    ar.u32(r.lineno);
    ar.u64(r.src_offset);
    ar.u32(r.src_length);
    ar.str(r.mpn);
    ar.str(r.manufacturer);
    ar.dbl(r.length_m);
    ar.dbl(r.width_m);
    ar.dbl(r.height_m);
    ar.str(r.case_code);
    ar.str(r.mount);
    ar.boolean(r.provenance_retracted);  // v17
    ar.dbl(r.lead_pitch_m);              // v19
    ar.boolean(r.box_axes_ambiguous);    // v20
    ar.strvec(r.retracted_fields);       // v22 (owner 2026-11-13, S1)
    ar.str(r.retraction_reason);         // v22
    ar.str(r.status);                    // v24 (owner 2026-11-15, every family)
}

template <class Ar>
void row_io(Ar& ar, MosfetRow& r) {
    io_base(ar, r);
    ar.dbl(r.vds_rated);
    ar.dbl(r.id_continuous);
    ar.dbl(r.rds_on);
    ar.dbl(r.qg_total);
    ar.dbl(r.coss);
    ar.dbl(r.vgs_threshold_max);
    ar.dbl(r.rds_on_vgs);
    ar.dbl(r.vgs_max);
    ar.str(r.qualification);
    ar.dbl(r.rth_ja);
    ar.dbl(r.rth_jc);
    ar.dbl(r.tj_max);
    ar.str(r.technology);
    ar.str(r.channel);      // v14
    ar.dbl(r.vgs_threshold_typ);  // v20
    ar.dbl(r.coss_vds);           // v21
    ar.str(r.die_configuration);  // v22
    ar.str(r.internal_connection);  // v22 (owner 2026-11-13, V2)
    ar.dbl(r.qgs);                  // v22 (owner 2026-11-14, operating-point loss)
    ar.dbl(r.qgd);
    ar.dbl(r.qg_vgs);
    ar.str(r.price_quotes);       // v22 (owner 2026-11-14, price)
    ar.boolean(r.is_production);
    ar.boolean(r.datasheet_usable);
}

template <class Ar>
void row_io(Ar& ar, DiodeRow& r) {
    io_base(ar, r);
    ar.dbl(r.vrrm_rated);
    ar.dbl(r.if_avg_rated);
    ar.dbl(r.vf_typ);
    ar.dbl(r.qrr);
    ar.dbl(r.trr);
    ar.dbl(r.vz_min);
    ar.dbl(r.vz_max);
    ar.dbl(r.vz_tolerance);
    ar.dbl(r.power_dissipation);       // v18
    ar.dbl(r.rth_ja);
    ar.dbl(r.rth_jc);
    ar.dbl(r.tj_max);
    ar.str(r.technology);
    ar.str(r.die_configuration);       // v19
    ar.str(r.internal_connection);     // v19
    ar.dbl(r.clamping_voltage);        // v17
    ar.dbl(r.clamping_current);        // v17
    ar.dbl(r.peak_pulse_power);        // v17
    ar.dbl(r.peak_pulse_current);      // v17
    ar.dbl(r.junction_capacitance);    // v17
    ar.str(r.clamping_waveform);       // v17
    ar.str(r.peak_pulse_power_waveform);    // v17
    ar.str(r.peak_pulse_current_waveform);  // v17
    ar.str(r.qualification);           // v20
    ar.dbl(r.junction_capacitance_vr); // v21
    ar.dbl(r.number_of_elements);      // v22 (SAS bcc7a93)
    ar.dbl(r.vf_test_current);         // v22
    ar.dbl(r.reverse_leakage);         // v22 (owner 2026-11-14, operating-point loss)
    ar.str(r.price_quotes);            // v22 (owner 2026-11-14, price)
    ar.boolean(r.is_production);
}

template <class Ar>
void row_io(Ar& ar, CapacitorRow& r) {
    io_base(ar, r);
    ar.dbl(r.capacitance);
    ar.dbl(r.v_rated);
    ar.dbl(r.ripple_current_rms);
    ar.dbl(r.esr);
    ar.dbl(r.rth);
    ar.str(r.technology);
    ar.str(r.dielectric_code);
    ar.str(r.family);
    ar.dbl(r.esr_frequency);
    ar.dbl(r.esl);          // v12
    ar.dbl(r.temp_min_c);
    ar.dbl(r.temp_max_c);
    ar.dbl(r.v_rated_ac);   // v14
    ar.str(r.safety_class);              // v20
    ar.dbl(r.ripple_current_frequency);  // v20
    ar.dbl(r.capacitance_min);           // v20
    ar.dbl(r.capacitance_max);           // v20
    ar.str(r.lead_form);                 // v20
    ar.str(r.qualified_spec);            // v20
    ar.str(r.qualified_grade);           // v20
    ar.dbl(r.qualified_failure_rate);    // v20
    ar.boolean(r.is_production);
    ar.dbl(r.ripple_current_temperature);  // v22 (Qarlos r26)
    ar.boolean(r.esr_from_curve);          // v22 (owner 2026-11-14, useful band)
    ar.str(r.price_quotes);                // v22 (owner 2026-11-14, capacitor price)
    ar.dbl(r.dissipation_factor);          // v22 (owner 2026-11-14, ESR from tan delta)
    ar.dbl(r.dissipation_factor_frequency);
}

template <class Ar>
void row_io(Ar& ar, ResistorRow& r) {
    io_base(ar, r);
    ar.dbl(r.resistance);
    ar.dbl(r.tolerance);
    ar.dbl(r.power_rating);
    ar.str(r.family);
    ar.boolean(r.is_production);
    ar.dbl(r.power_rating_temperature);  // v18
    ar.dblvec(r.derating_temperature);   // v18
    ar.dblvec(r.derating_amplitude);     // v18
    ar.str(r.technology);                // v19
    ar.dbl(r.tcr);                       // v19
    ar.dbl(r.temp_min_c);                // v19
    ar.dbl(r.temp_max_c);                // v19
    ar.boolean(r.current_sense);         // v19
    ar.boolean(r.pulse_withstanding);    // v19
    ar.dbl(r.max_voltage);               // v21
    ar.dbl(r.number_of_elements);        // v22 (RAS 3ba21f6)
    ar.str(r.internal_connection);       // v22 (RAS 3ba21f6)
}

template <class Ar>
void row_io(Ar& ar, IgbtRow& r) {
    io_base(ar, r);
    ar.dbl(r.vces_rated);
    ar.dbl(r.ic_continuous);
    ar.dbl(r.vce_sat);
    ar.dbl(r.rth_jc);
    ar.dbl(r.tj_max);
    ar.str(r.technology);
    ar.dbl(r.vce_sat_ic);  // v24
    ar.boolean(r.is_production);
}

template <class Ar>
void row_io(Ar& ar, BjtRow& r) {
    io_base(ar, r);
    ar.dbl(r.vceo_rated);
    ar.dbl(r.ic_continuous);
    ar.dbl(r.hfe_min);
    ar.dbl(r.power_dissipation);
    ar.dbl(r.tj_max);
    ar.str(r.technology);
    ar.str(r.polarity);     // v14
    ar.dbl(r.transition_frequency);  // v24
    ar.dbl(r.vce_sat);               // v24
    ar.dbl(r.vce_sat_ic);            // v24
    ar.dbl(r.hfe_ic);                // v24
    ar.boolean(r.is_production);
}

template <class Ar>
void row_io(Ar& ar, VaristorRow& r) {
    io_base(ar, r);
    ar.dbl(r.varistor_voltage);
    ar.dbl(r.clamping_voltage);
    ar.dbl(r.peak_surge_current);
    ar.dbl(r.max_continuous_dc_voltage);
    ar.dbl(r.capacitance);
    ar.str(r.technology);
    ar.dbl(r.max_continuous_ac_voltage);  // v24
    ar.dbl(r.energy_absorption);          // v24
    ar.boolean(r.is_production);
}

template <class Ar>
void row_io(Ar& ar, MagneticRow& r) {
    io_base(ar, r);
    ar.dbl(r.inductance);
    ar.dbl(r.saturation_current);
    ar.dbl(r.saturation_percent_drop);  // v15
    ar.dbl(r.rated_current);
    ar.dbl(r.dcr);
    ar.dbl(r.srf);
    ar.dbl(r.turns_ratio);
    ar.u32(r.secondary_counts);
    ar.dbl(r.impedance_100mhz);
    ar.dbl(r.impedance_peak);
    ar.dbl(r.impedance_peak_freq);
    ar.str(r.device_type);
    ar.str(r.family);
    ar.dbl(r.temp_min_c);  // v20
    ar.dbl(r.temp_max_c);  // v20
    ar.dblvec(r.sat_table_current);       // v20
    ar.dblvec(r.sat_table_percent_drop);  // v20
    ar.dblvec(r.sat_table_temperature);   // v20
    ar.dblvec(r.turns_ratios);             // v22
    ar.dblvecvec(r.turns_ratio_configs);   // v22 (W4)
    ar.dbl(r.inductance_tol_minus_pct);    // v22
    ar.dbl(r.inductance_tol_plus_pct);     // v22
    ar.dblvec(r.bias_curve_current);       // v22 (Z5)
    ar.dblvec(r.bias_curve_ratio);         // v22 (Z5)
    ar.dbl(r.bias_curve_temperature);      // v22 (curves)
    ar.boolean(r.sat_table_from_curve);    // v22 (Z5)
    ar.boolean(r.dcr_is_maximum);          // v22 (owner 2026-11-14, operating-point loss)
    ar.str(r.price_quotes);                // v22 (owner 2026-11-14, price)
    ar.dbl(r.dm_impedance_100mhz);         // v22 (curves, CMC DM mode)
    ar.dbl(r.dm_impedance_peak);           // v22
    ar.dbl(r.dm_impedance_peak_freq);      // v22
    ar.dbl(r.cable_max_diameter);          // v22 (cable cores, owner 2026-11-14)
    ar.dbl(r.number_turns);                // v22
    ar.boolean(r.is_production);
}

template <class Ar>
void row_io(Ar& ar, AnalogRow& r) {
    io_base(ar, r);
    ar.dbl(r.channels);
    ar.dbl(r.input_offset_voltage);
    ar.dbl(r.input_bias_current);
    ar.dbl(r.gain_bandwidth);
    ar.dbl(r.slew_rate);
    ar.dbl(r.cmrr);
    ar.dbl(r.vsupply_min);
    ar.dbl(r.vsupply_max);
    ar.dbl(r.resolution);
    ar.dbl(r.sample_rate);
    ar.dbl(r.on_resistance);
    ar.str(r.device_type);
    ar.str(r.architecture);
    ar.str(r.input_stage);
    ar.dbl(r.offset_drift);          // v24
    ar.dbl(r.quiescent_current);     // v24
    ar.dbl(r.propagation_delay);     // v24
    ar.str(r.rail_to_rail_input);    // v24
    ar.str(r.rail_to_rail_output);   // v24
    ar.str(r.output_stage);          // v24
    ar.boolean(r.is_production);
}

template <class Ar>
void row_io(Ar& ar, TimingRow& r) {
    io_base(ar, r);
    ar.dbl(r.frequency);
    ar.dbl(r.frequency_tolerance);
    ar.dbl(r.frequency_stability);
    ar.dbl(r.load_capacitance);
    ar.dbl(r.esr);
    ar.dbl(r.rms_phase_jitter);
    ar.dbl(r.vsupply_min);
    ar.dbl(r.vsupply_max);
    ar.str(r.device_type);
    ar.str(r.technology);
    ar.str(r.output_type);
    ar.str(r.mode);
    ar.dbl(r.temp_min_c);   // v16
    ar.dbl(r.temp_max_c);   // v16
    ar.str(r.enable_function);  // v20
    ar.dbl(r.built_in_capacitance);  // v21
    ar.dbl(r.frequency_min);  // v22
    ar.dbl(r.frequency_max);  // v22
    ar.dbl(r.current_consumption);  // v22 (owner 2026-11-13, S3)
    ar.boolean(r.is_production);
}

template <class Ar>
void row_io(Ar& ar, ConnectorRow& r) {
    io_base(ar, r);
    ar.dbl(r.rated_current);
    ar.dbl(r.rated_voltage);
    ar.dbl(r.positions);
    ar.dbl(r.pitch);
    ar.dbl(r.temp_min_c);
    ar.dbl(r.temp_max_c);
    ar.dbl(r.mating_cycles);
    ar.str(r.family);
    ar.str(r.interface_standard);
    ar.str(r.polarity);
    ar.str(r.series);
    ar.str(r.contact_plating);
    ar.str(r.termination);
    ar.str(r.orientation);  // v19
    ar.dbl(r.rows);         // v20
    ar.str(r.pluggable);    // v20
    ar.str(r.actuator_type);  // v21
    ar.str(r.shell_size);     // v21
    ar.str(r.coding);         // v21
    ar.str(r.coupling_type);  // v21
    ar.boolean(r.is_production);
}

template <class Ar>
void row_io(Ar& ar, ControllerRow& r) {
    io_base(ar, r);
    ar.dbl(r.vref);
    ar.str(r.category);
    ar.strvec(r.topologies);
    ar.boolean(r.integrated_fet);
    ar.boolean(r.integrated_driver);
}

// ---- curve sets (v23: the curve shard's records) ---------------------------
template <class Ar>
void curves_io(Ar& ar, MosfetCurveSet& c) {
    ar.curve(c.ciss_curve);       // v22 (owner curve rule 2026-11-14)
    ar.curve(c.coss_curve);       // v22 (also read by the operating-point loss, MosfetLoss.hpp)
    ar.curve(c.crss_curve);       // v22
    ar.curve(c.rds_on_id_curve);  // v22
    ar.curve(c.rds_on_tj_curve);  // v22 (also read by the operating-point loss)
    ar.curve(c.qg_curve);         // v22 (also read by the operating-point loss)
    curves(ar, c.id_ambient_curves);  // v22
}
template <class Ar>
void curves_io(Ar& ar, DiodeCurveSet& c) {
    ar.curve(c.vf_curve);  // v22 (owner curve rule 2026-11-14)
    ar.curve(c.ir_curve);
    ar.curve(c.cj_curve);
}
template <class Ar>
void curves_io(Ar& ar, CapacitorCurveSet& c) {
    ar.curve(c.esr_curve);  // v22 (owner 2026-11-14, curves)
    ar.curve(c.z_curve);
    ar.curve(c.cf_curve);
    ar.curve(c.esl_curve);
    ar.curve(c.bias_curve);
    ar.curve(c.ripple_f_curve);  // v22 (owner 2026-11-14, ripple tables)
    ar.curve(c.ripple_t_curve);
}
template <class Ar>
void curves_io(Ar& ar, MagneticCurveSet& c) {
    curves(ar, c.l_curves);    // v22 (curves)
    ar.curve(c.rated_points);  // v22 (curves, rated current per rise)
    ar.curve(c.z_curve);       // v22 curves
    ar.curve(c.r_curve);
    ar.curve(c.x_curve);
    ar.curve(c.dm_z_curve);    // v22 curves (CMC)
    ar.curve(c.cm_il_curve);
    ar.curve(c.dm_il_curve);
}
template <class Ar>
void curves_io(Ar& ar, ConnectorCurveSet& c) {
    ar.curve(c.current_ambient_curve);  // v22 (owner curve rule 2026-11-14)
}

// ---- header (fixed 104 bytes, all little-endian) ---------------------------
#pragma pack(push, 1)
struct Header {
    char magic[8];
    uint32_t format_version;
    uint32_t family_id;
    uint64_t source_size;
    uint64_t content_hash;
    uint64_t extractor_hash;  // v5: identity of the code that produced the rows
    uint64_t row_count;
    uint64_t unreadable_row_count;
    uint64_t source_line_count;
    uint64_t physical_line_count;
    uint64_t build_id;
    uint64_t pool_size;
    uint64_t rows_size;
    uint64_t curves_build_id;  // v23: build id of the curve shard paired with this base shard
    uint32_t has_curve_shard;  // v23: 1 when the family has a curve shard
    uint32_t reserved;         // v23: 0
};
// The curve shard's header (v23). base_build_id ties it to ONE base shard; curves_build_id is
// the hash of its own body, which the base header carries too.
struct CurveHeader {
    char magic[8];
    uint32_t format_version;
    uint32_t family_id;
    uint64_t base_build_id;
    uint64_t curves_build_id;
    uint64_t row_count;     // rows of the base shard
    uint64_t record_count;  // rows that state curves (one record each)
    uint64_t pool_size;
    uint64_t body_size;
};
#pragma pack(pop)

// ---- generic serialize -----------------------------------------------------
template <class Row>
void serialize_body(const Shard<Row>& shard, std::string& pool_blob, std::string& rows_buf) {
    StringPool pool;
    WriteAr war{&rows_buf, &pool};
    for (const auto& row : shard.rows) {
        Row copy = row;  // io takes non-const refs; a copy keeps this pure
        row_io(war, copy);
    }
    pool_blob = pool.blob();
}

// The curve shard body: one record per row that states curves, in row order:
// u32 row index, u32 lineno (a second check on the pairing), the family's curve set.
template <class Row>
void serialize_curve_body(const Shard<Row>& shard, std::string& pool_blob, std::string& body,
                          uint64_t& records) {
    StringPool pool;
    WriteAr war{&body, &pool};
    records = 0;
    for (size_t i = 0; i < shard.rows.size(); ++i) {
        const auto* set = shard.rows[i].curves.stated();  // throws when the row is detached
        if (!set) continue;
        uint32_t idx = static_cast<uint32_t>(i);
        uint32_t lineno = shard.rows[i].lineno;
        war.u32(idx);
        war.u32(lineno);
        auto copy = *set;
        curves_io(war, copy);
        ++records;
    }
    pool_blob = pool.blob();
}

template <class Row>
bool rows_detached(const Shard<Row>& shard) {
    if constexpr (row_has_curves<Row>) {
        bool any = false, all = true;
        for (const auto& r : shard.rows) {
            any = any || r.curves.detached();
            all = all && r.curves.detached();
        }
        if (any && !all)
            throw std::runtime_error(std::string("kelvin: ") + family_name(shard.meta.family) +
                                     " shard mixes rows with and without loaded curves");
        return any;
    }
    return false;
}

// The base shard's curve-shard linkage: the curve body's hash, from the rows' own curves, or
// for a base-only shard (curves not attached) the one its header carried.
template <class Row>
uint64_t curves_build_id_of(const Shard<Row>& shard) {
    if constexpr (!row_has_curves<Row>) {
        return 0;  // not a hash input: has_curve_shard is 0 and the base build id skips it
    } else {
        if (rows_detached(shard)) {
            if (!shard.meta.has_curve_shard)
                throw std::runtime_error("kelvin: base-only shard carries no curve-shard linkage");
            return shard.meta.curves_build_id;
        }
        std::string pool_blob, body;
        uint64_t records = 0;
        serialize_curve_body(shard, pool_blob, body, records);
        return fnv1a64(body.data(), body.size(), fnv1a64(pool_blob.data(), pool_blob.size()));
    }
}

// The base build id hashes the base body and, for a family with curves, the curve shard's
// build id: a change to any curve changes the base shard's id (and URL) too.
uint64_t base_build_id(const char* pool, size_t pool_size, const char* rows, size_t rows_size,
                       bool has_curves, uint64_t curves_build_id) {
    uint64_t seed = fnv1a64(pool, pool_size);
    if (has_curves) seed = fnv1a64(&curves_build_id, sizeof curves_build_id, seed);
    return fnv1a64(rows, rows_size, seed);
}
uint64_t base_build_id(const std::string& pool_blob, const std::string& rows_buf,
                       bool has_curves, uint64_t curves_build_id) {
    return base_build_id(pool_blob.data(), pool_blob.size(), rows_buf.data(), rows_buf.size(),
                         has_curves, curves_build_id);
}

template <class Row>
void compute_build_ids(Shard<Row>& shard) {
    std::string pool_blob, rows_buf;
    serialize_body(shard, pool_blob, rows_buf);
    shard.meta.has_curve_shard = row_has_curves<Row>;
    shard.meta.curves_build_id = curves_build_id_of(shard);
    shard.meta.build_id =
        base_build_id(pool_blob, rows_buf, row_has_curves<Row>, shard.meta.curves_build_id);
}

template <class Row>
std::string serialize_impl(const Shard<Row>& shard) {
    std::string pool_blob, rows_buf;
    serialize_body(shard, pool_blob, rows_buf);
    const uint64_t curves_id = curves_build_id_of(shard);
    const uint64_t build_id = base_build_id(pool_blob, rows_buf, row_has_curves<Row>, curves_id);

    Header h{};
    std::memcpy(h.magic, kMagic, 8);
    h.format_version = kFormatVersion;
    h.family_id = static_cast<uint32_t>(shard.meta.family);
    h.source_size = shard.meta.source_size;
    h.content_hash = shard.meta.content_hash;
    h.extractor_hash = shard.meta.extractor_hash;
    h.row_count = shard.meta.row_count;
    h.unreadable_row_count = shard.meta.unreadable_row_count;
    h.source_line_count = shard.meta.source_line_count;
    h.physical_line_count = shard.meta.physical_line_count;
    h.build_id = build_id;
    h.pool_size = pool_blob.size();
    h.rows_size = rows_buf.size();
    h.curves_build_id = curves_id;
    h.has_curve_shard = row_has_curves<Row> ? 1u : 0u;
    h.reserved = 0;

    std::string out;
    out.reserve(sizeof(Header) + pool_blob.size() + rows_buf.size());
    out.append(reinterpret_cast<const char*>(&h), sizeof(Header));
    out.append(pool_blob);
    out.append(rows_buf);
    return out;
}

template <class Row>
std::string serialize_curve_impl(const Shard<Row>& shard) {
    if (rows_detached(shard))
        throw CurvesNotLoaded(std::string("kelvin: cannot write the ") +
                              family_name(shard.meta.family) +
                              " curve shard: the rows' curves are not loaded");
    std::string pool_blob, body;
    uint64_t records = 0;
    serialize_curve_body(shard, pool_blob, body, records);
    std::string base_pool, base_rows;
    serialize_body(shard, base_pool, base_rows);
    const uint64_t curves_id =
        fnv1a64(body.data(), body.size(), fnv1a64(pool_blob.data(), pool_blob.size()));

    CurveHeader h{};
    std::memcpy(h.magic, kCurveMagic, 8);
    h.format_version = kFormatVersion;
    h.family_id = static_cast<uint32_t>(shard.meta.family);
    h.base_build_id = base_build_id(base_pool, base_rows, true, curves_id);
    h.curves_build_id = curves_id;
    h.row_count = shard.rows.size();
    h.record_count = records;
    h.pool_size = pool_blob.size();
    h.body_size = body.size();

    std::string out;
    out.reserve(sizeof(CurveHeader) + pool_blob.size() + body.size());
    out.append(reinterpret_cast<const char*>(&h), sizeof(CurveHeader));
    out.append(pool_blob);
    out.append(body);
    return out;
}

// ---- generic deserialize ---------------------------------------------------
// The base shard only. A family with curves comes back with every row's curves DETACHED:
// reading one throws CurvesNotLoaded until attach_curve_impl() pairs the curve shard.
template <class Row>
Shard<Row> deserialize_impl(const std::string& bytes, Family expected) {
    if (bytes.size() < sizeof(Header))
        throw std::runtime_error("kelvin: shard too small for header");
    if (std::memcmp(bytes.data(), kCurveMagic, 8) == 0)
        throw std::runtime_error(std::string("kelvin: this is a curve shard, not the ") +
                                 family_name(expected) + " base shard");
    Header h;
    std::memcpy(&h, bytes.data(), sizeof(Header));
    if (std::memcmp(h.magic, kMagic, 8) != 0)
        throw std::runtime_error("kelvin: bad shard magic");
    if (h.format_version != kFormatVersion)
        throw std::runtime_error("kelvin: unsupported shard format version " +
                                 std::to_string(h.format_version));
    if (h.family_id != static_cast<uint32_t>(expected))
        throw std::runtime_error("kelvin: shard family mismatch");
    if (sizeof(Header) + h.pool_size + h.rows_size > bytes.size())
        throw std::runtime_error("kelvin: shard truncated");
    if (h.has_curve_shard != (row_has_curves<Row> ? 1u : 0u))
        throw std::runtime_error(std::string("kelvin: ") + family_name(expected) +
                                 " shard header disagrees on whether the family has a curve shard");

    const char* pool_base = bytes.data() + sizeof(Header);
    const char* rows_base = pool_base + h.pool_size;

    // Verify build_id (content integrity + determinism guard).
    const uint64_t expect_build =
        base_build_id(pool_base, static_cast<size_t>(h.pool_size), rows_base,
                      static_cast<size_t>(h.rows_size), h.has_curve_shard != 0, h.curves_build_id);
    if (expect_build != h.build_id)
        throw std::runtime_error("kelvin: shard build_id mismatch (corrupt or tampered)");

    Shard<Row> shard;
    shard.meta.family = expected;
    shard.meta.source_size = h.source_size;
    shard.meta.content_hash = h.content_hash;
    shard.meta.extractor_hash = h.extractor_hash;
    shard.meta.row_count = h.row_count;
    shard.meta.unreadable_row_count = h.unreadable_row_count;
    shard.meta.source_line_count = h.source_line_count;
    shard.meta.physical_line_count = h.physical_line_count;
    shard.meta.build_id = h.build_id;
    shard.meta.curves_build_id = h.curves_build_id;
    shard.meta.has_curve_shard = h.has_curve_shard != 0;

    ReadAr rar{rows_base, static_cast<size_t>(h.rows_size), 0, pool_base,
               static_cast<size_t>(h.pool_size)};
    shard.rows.resize(h.row_count);
    for (uint64_t i = 0; i < h.row_count; ++i) {
        row_io(rar, shard.rows[i]);
        if constexpr (row_has_curves<Row>) shard.rows[i].curves.detach();
    }
    return shard;
}

// Pair a curve shard with a loaded base shard. Every check is a throw: a curve shard of any
// other build, family or format is refused, never half-applied (records are parsed into a
// side vector first and attached only when the whole shard has been read).
template <class Row>
void attach_curve_impl(Shard<Row>& shard, const std::string& bytes) {
    static_assert(row_has_curves<Row>, "family has no curve shard");
    const std::string fam = family_name(shard.meta.family);
    if (bytes.size() < sizeof(CurveHeader))
        throw std::runtime_error("kelvin: " + fam + " curve shard too small for its header");
    CurveHeader h;
    std::memcpy(&h, bytes.data(), sizeof(CurveHeader));
    if (std::memcmp(h.magic, kCurveMagic, 8) != 0)
        throw std::runtime_error("kelvin: " + fam + " curve shard has a bad magic (not a curve shard)");
    if (h.format_version != kFormatVersion)
        throw std::runtime_error("kelvin: " + fam + " curve shard is format " +
                                 std::to_string(h.format_version) + ", this engine reads " +
                                 std::to_string(kFormatVersion));
    if (h.family_id != static_cast<uint32_t>(shard.meta.family))
        throw std::runtime_error("kelvin: curve shard is for another family, not " + fam);
    if (!shard.meta.has_curve_shard)
        throw std::runtime_error("kelvin: the " + fam + " base shard names no curve shard");
    if (h.base_build_id != shard.meta.build_id || h.curves_build_id != shard.meta.curves_build_id)
        throw std::runtime_error(
            "kelvin: " + fam + " curve shard does not belong to the loaded base shard (curve "
            "shard pairs with base build " + std::to_string(h.base_build_id) + ", curves " +
            std::to_string(h.curves_build_id) + "; loaded base is build " +
            std::to_string(shard.meta.build_id) + ", curves " +
            std::to_string(shard.meta.curves_build_id) + ") — deploy the two from one build");
    if (h.row_count != shard.rows.size())
        throw std::runtime_error("kelvin: " + fam + " curve shard row count mismatch");
    if (sizeof(CurveHeader) + h.pool_size + h.body_size > bytes.size())
        throw std::runtime_error("kelvin: " + fam + " curve shard truncated");
    const char* pool_base = bytes.data() + sizeof(CurveHeader);
    const char* body_base = pool_base + h.pool_size;
    if (fnv1a64(body_base, h.body_size, fnv1a64(pool_base, h.pool_size)) != h.curves_build_id)
        throw std::runtime_error("kelvin: " + fam + " curve shard content does not match its "
                                 "build id (corrupt or tampered)");

    using Set = std::decay_t<decltype(*shard.rows[0].curves)>;
    ReadAr rar{body_base, static_cast<size_t>(h.body_size), 0, pool_base,
               static_cast<size_t>(h.pool_size)};
    std::vector<std::pair<uint32_t, std::shared_ptr<Set>>> recs;
    recs.reserve(h.record_count);
    int64_t prev = -1;
    for (uint64_t k = 0; k < h.record_count; ++k) {
        uint32_t idx = 0, lineno = 0;
        rar.u32(idx);
        rar.u32(lineno);
        if (idx >= shard.rows.size() || static_cast<int64_t>(idx) <= prev)
            throw std::runtime_error("kelvin: " + fam + " curve shard record index out of order");
        if (shard.rows[idx].lineno != lineno)
            throw std::runtime_error("kelvin: " + fam + " curve shard record for row " +
                                     std::to_string(idx) + " names line " + std::to_string(lineno) +
                                     ", the base row is line " +
                                     std::to_string(shard.rows[idx].lineno));
        prev = idx;
        auto set = std::make_shared<Set>();
        curves_io(rar, *set);
        recs.emplace_back(idx, std::move(set));
    }
    if (rar.pos != h.body_size)
        throw std::runtime_error("kelvin: " + fam + " curve shard has trailing bytes");
    for (auto& r : shard.rows) r.curves.attach(nullptr);
    for (auto& [idx, set] : recs) shard.rows[idx].curves.attach(std::move(set));
}

// ---- file helpers ----------------------------------------------------------
std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw DataError(path, 0, "TAS catalogue file does not exist");
    f.seekg(0, std::ios::end);
    std::streamoff sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::string buf;
    buf.resize(static_cast<size_t>(sz));
    if (sz > 0) f.read(&buf[0], sz);
    return buf;
}

// Trim ASCII whitespace (matches Python str.strip for the whitespace TAS lines carry).
void trim(const char*& s, size_t& n) {
    while (n > 0 && (s[0] == ' ' || s[0] == '\t' || s[0] == '\r' || s[0] == '\n' || s[0] == '\v' ||
                     s[0] == '\f')) {
        ++s;
        --n;
    }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n' ||
                     s[n - 1] == '\v' || s[n - 1] == '\f')) {
        --n;
    }
}

// Parse the NDJSON region [begin,end) of `content`, calling extract() per non-blank line.
// start_physical_line: the 1-based physical line number of the first byte at `begin`.
template <class Row, class Extract>
void parse_region(const std::string& path, const std::string& content, size_t begin, size_t end,
                  uint32_t start_physical_line, Extract&& extract, Shard<Row>& out) {
    size_t i = begin;
    uint32_t phys = start_physical_line;
    while (i < end) {
        size_t nl = content.find('\n', i);
        size_t line_end = (nl == std::string::npos || nl > end) ? end : nl;
        const char* raw = content.data() + i;
        size_t raw_len = line_end - i;
        const char* s = raw;
        size_t n = raw_len;
        trim(s, n);
        if (n != 0) {
            out.meta.source_line_count += 1;
            json env;
            try {
                env = json::parse(s, s + n);
            } catch (const json::parse_error& e) {
                throw DataError(path, phys, std::string("JSON decode error: ") + e.what());
            }
            if (!env.is_object())
                throw DataError(path, phys, "top-level value is not an object");
            auto row = extract(env);
            if (row) {
                row->lineno = phys;
                row->src_offset = static_cast<uint64_t>(i);
                row->src_length = static_cast<uint32_t>(raw_len);
                row->provenance_retracted = all_provenance_retracted(env);
                FieldRetraction fr = retracted_only_fields(env);
                row->retracted_fields = std::move(fr.fields);
                row->retraction_reason = std::move(fr.reason);
                // Owner 2026-11-13, Z1: a retracted-only primary rating is not stated.
                unstate_retracted_ratings(*row);
                // A wholly retracted record carries its reason too (Qarlos r26 31990757).
                if (row->provenance_retracted)
                    row->retraction_reason = all_provenance_retraction_reason(env);
                out.rows.push_back(std::move(*row));
                out.meta.row_count += 1;
            } else {
                out.meta.unreadable_row_count += 1;
            }
        }
        if (nl == std::string::npos || nl >= end) break;
        i = nl + 1;
        phys += 1;
    }
}

uint32_t count_physical_lines(const std::string& content, size_t upto) {
    // Physical line count of content[0,upto): number of '\n' up to upto, +1 if the last byte
    // isn't a trailing newline (mirrors Python enumerate over the file's physical lines).
    uint32_t lines = 0;
    size_t i = 0;
    while (i < upto) {
        size_t nl = content.find('\n', i);
        if (nl == std::string::npos || nl >= upto) {
            lines += 1;  // final line without newline
            break;
        }
        lines += 1;
        i = nl + 1;
    }
    return lines;
}

template <class Row, class Extract>
Shard<Row> build_generic(Family fam, const std::string& path, Extract&& extract,
                         const Shard<Row>* prev) {
    std::string content = read_file(path);
    Shard<Row> shard;
    shard.meta.family = fam;
    shard.meta.source_size = content.size();
    shard.meta.content_hash = fnv1a64(content.data(), content.size());
    shard.meta.extractor_hash = kExtractorHash;

    // Incremental tail build: new file is an append-only extension of prev's source AND
    // prev's rows were produced by the extractor running now. Without the second
    // condition the reused prefix carries the OLD extractor's output forever (ABT #426).
    if (prev != nullptr && prev->meta.extractor_hash == kExtractorHash &&
        content.size() > prev->meta.source_size &&
        fnv1a64(content.data(), prev->meta.source_size) == prev->meta.content_hash) {
        // The reused rows must carry their curves: a base-only prev would write a curve shard
        // missing every prefix row's curves (v23).
        if (rows_detached(*prev))
            throw CurvesNotLoaded(std::string("kelvin: incremental ") + family_name(fam) +
                                  " build needs the previous shard's curves attached");
        shard.rows = prev->rows;  // reuse prefix rows verbatim
        shard.meta.row_count = prev->meta.row_count;
        shard.meta.unreadable_row_count = prev->meta.unreadable_row_count;
        shard.meta.source_line_count = prev->meta.source_line_count;
        // prev ended exactly at a line boundary (append-only) -> continue numbering after it.
        uint32_t start_line = static_cast<uint32_t>(prev->meta.physical_line_count) + 1;
        parse_region(path, content, prev->meta.source_size, content.size(), start_line,
                     std::forward<Extract>(extract), shard);
        shard.meta.physical_line_count = count_physical_lines(content, content.size());
        compute_build_ids(shard);
        return shard;
    }

    parse_region(path, content, 0, content.size(), 1, std::forward<Extract>(extract), shard);
    shard.meta.physical_line_count = count_physical_lines(content, content.size());
    compute_build_ids(shard);
    return shard;
}

}  // namespace

// ---- concrete wrappers -----------------------------------------------------
Shard<MosfetRow> build_mosfet_shard(const std::string& p, const Shard<MosfetRow>* prev) {
    return build_generic<MosfetRow>(Family::Mosfet, p, extract_mosfet, prev);
}
Shard<DiodeRow> build_diode_shard(const std::string& p, const Shard<DiodeRow>* prev) {
    return build_generic<DiodeRow>(Family::Diode, p, extract_diode, prev);
}
Shard<CapacitorRow> build_capacitor_shard(const std::string& p, const Shard<CapacitorRow>* prev) {
    return build_generic<CapacitorRow>(Family::Capacitor, p, extract_capacitor, prev);
}
Shard<ResistorRow> build_resistor_shard(const std::string& p, const Shard<ResistorRow>* prev) {
    return build_generic<ResistorRow>(Family::Resistor, p, extract_resistor, prev);
}
Shard<ControllerRow> build_controller_shard(const std::string& p, const Shard<ControllerRow>* prev) {
    return build_generic<ControllerRow>(Family::Controller, p, extract_controller, prev);
}
Shard<IgbtRow> build_igbt_shard(const std::string& p, const Shard<IgbtRow>* prev) {
    return build_generic<IgbtRow>(Family::Igbt, p, extract_igbt, prev);
}
Shard<BjtRow> build_bjt_shard(const std::string& p, const Shard<BjtRow>* prev) {
    return build_generic<BjtRow>(Family::Bjt, p, extract_bjt, prev);
}
Shard<VaristorRow> build_varistor_shard(const std::string& p, const Shard<VaristorRow>* prev) {
    return build_generic<VaristorRow>(Family::Varistor, p, extract_varistor, prev);
}
Shard<MagneticRow> build_magnetic_shard(const std::string& p, const Shard<MagneticRow>* prev) {
    return build_generic<MagneticRow>(Family::Magnetic, p, extract_magnetic, prev);
}
Shard<AnalogRow> build_analog_shard(const std::string& p, const Shard<AnalogRow>* prev) {
    return build_generic<AnalogRow>(Family::Analog, p, extract_analog, prev);
}
Shard<TimingRow> build_timing_shard(const std::string& p, const Shard<TimingRow>* prev) {
    return build_generic<TimingRow>(Family::Timing, p, extract_timing, prev);
}
Shard<ConnectorRow> build_connector_shard(const std::string& p, const Shard<ConnectorRow>* prev) {
    return build_generic<ConnectorRow>(Family::Connector, p, extract_connector, prev);
}

std::string serialize_shard(const Shard<MosfetRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<DiodeRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<CapacitorRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<ResistorRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<ControllerRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<IgbtRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<BjtRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<VaristorRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<MagneticRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<AnalogRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<TimingRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<ConnectorRow>& s) { return serialize_impl(s); }

namespace {
void write_bytes(const std::string& path, const std::string& bytes) {
    // Write beside the target, then RENAME. Truncating the real file in place made
    // every reader of a shared cache directory a hazard to every writer: a second
    // process (another test run, another session on the same box) that opened the
    // shard mid-write got a truncated one, whose byte offsets then addressed the
    // NDJSON at nonsense positions. The failure surfaced far away, as a JSON parse
    // error from the middle of a record, in whichever test happened to be running —
    // reproducible only in the sense that something always broke, never the same
    // thing twice. rename() is atomic on POSIX: a reader sees the whole old file or
    // the whole new one.
    //
    // The temp name carries the pid so two writers cannot collide on it either, and
    // it sits in the SAME directory so the rename cannot cross a filesystem.
#ifdef _WIN32
    const std::string tmp = path + ".tmp." + std::to_string(::_getpid());
#else
    const std::string tmp = path + ".tmp." + std::to_string(::getpid());
#endif
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("kelvin: cannot open shard for writing: " + tmp);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!f) { std::remove(tmp.c_str()); throw std::runtime_error(
            "kelvin: short write building shard: " + tmp); }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        throw std::runtime_error("kelvin: cannot install shard at " + path);
    }
}
}  // namespace

namespace {
// v23: a family with curves writes its curve shard beside the base (<base>.curves.kidx) —
// curves first, so a reader that finds the new base finds its curves too.
template <class Row>
void write_impl(const std::string& p, const Shard<Row>& s) {
    if constexpr (row_has_curves<Row>) write_bytes(curve_shard_path(p), serialize_curve_impl(s));
    write_bytes(p, serialize_impl(s));
}
// Native readers load the curve shard eagerly beside the base (v23). A missing or mismatched
// curve shard throws: a base whose curves cannot be paired is not a usable shard here.
template <class Row>
Shard<Row> read_impl(const std::string& p, Shard<Row> base) {
    if constexpr (row_has_curves<Row>) {
        const std::string cp = curve_shard_path(p);
        std::ifstream probe(cp, std::ios::binary);
        if (!probe)
            throw std::runtime_error("kelvin: " + std::string(family_name(base.meta.family)) +
                                     " curve shard missing beside its base shard: " + cp);
        probe.close();
        attach_curve_impl(base, read_file(cp));
    }
    return base;
}
}  // namespace

std::string curve_shard_path(const std::string& base_path) {
    const std::string ext = ".kidx";
    if (base_path.size() >= ext.size() &&
        base_path.compare(base_path.size() - ext.size(), ext.size(), ext) == 0)
        return base_path.substr(0, base_path.size() - ext.size()) + ".curves.kidx";
    return base_path + ".curves";
}

#define KELVIN_CURVE_FAMILY(Row)                                                   \
    std::string serialize_curve_shard(const Shard<Row>& s) { return serialize_curve_impl(s); } \
    void attach_curve_shard(Shard<Row>& s, const std::string& b) { attach_curve_impl(s, b); }
KELVIN_CURVE_FAMILY(MosfetRow)
KELVIN_CURVE_FAMILY(DiodeRow)
KELVIN_CURVE_FAMILY(CapacitorRow)
KELVIN_CURVE_FAMILY(MagneticRow)
KELVIN_CURVE_FAMILY(ConnectorRow)
#undef KELVIN_CURVE_FAMILY

void write_shard(const std::string& p, const Shard<MosfetRow>& s) { write_impl(p, s); }
void write_shard(const std::string& p, const Shard<DiodeRow>& s) { write_impl(p, s); }
void write_shard(const std::string& p, const Shard<CapacitorRow>& s) { write_impl(p, s); }
void write_shard(const std::string& p, const Shard<ResistorRow>& s) { write_impl(p, s); }
void write_shard(const std::string& p, const Shard<ControllerRow>& s) { write_impl(p, s); }
void write_shard(const std::string& p, const Shard<IgbtRow>& s) { write_impl(p, s); }
void write_shard(const std::string& p, const Shard<BjtRow>& s) { write_impl(p, s); }
void write_shard(const std::string& p, const Shard<VaristorRow>& s) { write_impl(p, s); }
void write_shard(const std::string& p, const Shard<MagneticRow>& s) { write_impl(p, s); }
void write_shard(const std::string& p, const Shard<AnalogRow>& s) { write_impl(p, s); }
void write_shard(const std::string& p, const Shard<TimingRow>& s) { write_impl(p, s); }
void write_shard(const std::string& p, const Shard<ConnectorRow>& s) { write_impl(p, s); }

Shard<MosfetRow> deserialize_mosfet_shard(const std::string& b) { return deserialize_impl<MosfetRow>(b, Family::Mosfet); }
Shard<DiodeRow> deserialize_diode_shard(const std::string& b) { return deserialize_impl<DiodeRow>(b, Family::Diode); }
Shard<CapacitorRow> deserialize_capacitor_shard(const std::string& b) { return deserialize_impl<CapacitorRow>(b, Family::Capacitor); }
Shard<ResistorRow> deserialize_resistor_shard(const std::string& b) { return deserialize_impl<ResistorRow>(b, Family::Resistor); }
Shard<ControllerRow> deserialize_controller_shard(const std::string& b) { return deserialize_impl<ControllerRow>(b, Family::Controller); }
Shard<IgbtRow> deserialize_igbt_shard(const std::string& b) { return deserialize_impl<IgbtRow>(b, Family::Igbt); }
Shard<BjtRow> deserialize_bjt_shard(const std::string& b) { return deserialize_impl<BjtRow>(b, Family::Bjt); }
Shard<VaristorRow> deserialize_varistor_shard(const std::string& b) { return deserialize_impl<VaristorRow>(b, Family::Varistor); }
Shard<MagneticRow> deserialize_magnetic_shard(const std::string& b) { return deserialize_impl<MagneticRow>(b, Family::Magnetic); }
Shard<AnalogRow> deserialize_analog_shard(const std::string& b) { return deserialize_impl<AnalogRow>(b, Family::Analog); }
Shard<TimingRow> deserialize_timing_shard(const std::string& b) { return deserialize_impl<TimingRow>(b, Family::Timing); }
Shard<ConnectorRow> deserialize_connector_shard(const std::string& b) { return deserialize_impl<ConnectorRow>(b, Family::Connector); }

Shard<MosfetRow> read_mosfet_shard(const std::string& p) { return read_impl<MosfetRow>(p, deserialize_mosfet_shard(read_file(p))); }
Shard<DiodeRow> read_diode_shard(const std::string& p) { return read_impl<DiodeRow>(p, deserialize_diode_shard(read_file(p))); }
Shard<CapacitorRow> read_capacitor_shard(const std::string& p) { return read_impl<CapacitorRow>(p, deserialize_capacitor_shard(read_file(p))); }
Shard<ResistorRow> read_resistor_shard(const std::string& p) { return read_impl<ResistorRow>(p, deserialize_resistor_shard(read_file(p))); }
Shard<ControllerRow> read_controller_shard(const std::string& p) { return read_impl<ControllerRow>(p, deserialize_controller_shard(read_file(p))); }
Shard<IgbtRow> read_igbt_shard(const std::string& p) { return read_impl<IgbtRow>(p, deserialize_igbt_shard(read_file(p))); }
Shard<BjtRow> read_bjt_shard(const std::string& p) { return read_impl<BjtRow>(p, deserialize_bjt_shard(read_file(p))); }
Shard<VaristorRow> read_varistor_shard(const std::string& p) { return read_impl<VaristorRow>(p, deserialize_varistor_shard(read_file(p))); }
Shard<MagneticRow> read_magnetic_shard(const std::string& p) { return read_impl<MagneticRow>(p, deserialize_magnetic_shard(read_file(p))); }
Shard<AnalogRow> read_analog_shard(const std::string& p) { return read_impl<AnalogRow>(p, deserialize_analog_shard(read_file(p))); }
Shard<TimingRow> read_timing_shard(const std::string& p) { return read_impl<TimingRow>(p, deserialize_timing_shard(read_file(p))); }
Shard<ConnectorRow> read_connector_shard(const std::string& p) { return read_impl<ConnectorRow>(p, deserialize_connector_shard(read_file(p))); }

uint64_t extractor_version() { return kExtractorHash; }

std::optional<std::string> shard_stale_reason(const ShardMeta& meta,
                                              const std::string& ndjson_path) {
    // A shard built by different extractor code is stale even when every source byte is
    // identical — this is the path that made a working extractor fix look like a no-op,
    // because load_or_build() returns the cached shard verbatim when this says "fresh".
    if (meta.extractor_hash != kExtractorHash)
        return "it was built by different extractor code (shard " +
               std::to_string(meta.extractor_hash) + ", this binary " +
               std::to_string(kExtractorHash) + ")";
    std::ifstream f(ndjson_path, std::ios::binary | std::ios::ate);
    if (!f) return "its source " + ndjson_path + " cannot be opened";
    uint64_t size = static_cast<uint64_t>(f.tellg());
    if (size != meta.source_size)  // append/edit always changes size
        return "its source " + ndjson_path + " is " + std::to_string(size) +
               " bytes but the shard was built from " + std::to_string(meta.source_size);
    // Same size: confirm with a content hash (guards an in-place edit that preserved size).
    // STREAMED in fixed chunks, never read whole. The capacitor catalogue is 2.5 GB, and
    // reading it into one buffer to hash it put a 2.5 GB allocation on every cold load of the
    // family: under prod's 3 GB service cap that buffer went to swap on a spinning disk, and a
    // cold describe_family(capacitor) took 247-275 s with a perfectly valid cached shard
    // (2026-11-20). FNV-1a chains through its seed, so the chunked hash is bit-identical to
    // the one-shot hash the shard stores.
    f.seekg(0);
    constexpr uint64_t kChunk = uint64_t{8} << 20;
    std::string buf(static_cast<size_t>(std::min(size, kChunk)), '\0');
    uint64_t h = kFnvOffset;
    uint64_t done = 0;
    while (done < size) {
        const size_t n = static_cast<size_t>(std::min(size - done, kChunk));
        if (!f.read(&buf[0], static_cast<std::streamsize>(n)))
            return "its source " + ndjson_path + " could not be read to the end (" +
                   std::to_string(done) + " of " + std::to_string(size) + " bytes)";
        h = fnv1a64(buf.data(), n, h);
        done += n;
    }
    if (h != meta.content_hash)
        return "its source " + ndjson_path +
               " has the size the shard was built from but different content";
    return std::nullopt;
}

bool shard_is_stale(const ShardMeta& meta, const std::string& ndjson_path) {
    return shard_stale_reason(meta, ndjson_path).has_value();
}

uint32_t shard_format_version() { return kFormatVersion; }

}  // namespace kelvin
