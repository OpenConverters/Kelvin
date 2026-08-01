// Component-class equivalence: the rules that decide whether two parts are even
// the same KIND of thing, and what a dielectric code actually promises.
//
// These exist because a parametric match is not a substitution. A 10 uF ceramic
// and a 10 uF tantalum share every catalogue column and are not interchangeable:
// different failure mode, different ESR, different derating, different bias
// behaviour. Industry cross-reference graders (SiliconExpert's A/B/C/D/SF,
// Z2Data's Drop-In A/B/C) gate their top tiers on package+pinout for exactly
// this reason — form-fit-function, not spec similarity.
//
// Everything here is a published standard (EIA RS-198 dielectric codes, IEC
// 60384 family structure) or a documented failure mode, never a guess. A code
// this file cannot decode yields "unknown", and the caller treats unknown as
// unverified rather than as a pass.
#pragma once
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace kelvin::crossref {

// ── Capacitor construction family ────────────────────────────────────────────
// Distinct dielectric systems with distinct failure modes and derating rules.
// A swap ACROSS families is never like-for-like even at matching C/V.
enum class CapFamily {
    Unknown,
    CeramicClass1,   // C0G/NP0 — no bias derating, no ageing, low loss
    CeramicClass2,   // X7R/X5R/… — bias derating, ageing, piezo noise
    AluminiumWet,    // electrolyte evaporation wear-out, 10 degC life rule
    AluminiumPolymer,// ESR-rise wear-out, much lower ESR, no electrolyte loss
    AluminiumHybrid,
    TantalumMnO2,    // ignition failure mode -> 50% voltage derating convention
    TantalumPolymer, // benign resistive failure -> 70-80% derating
    Film,            // self-healing (metallized) vs not (film/foil)
    Supercapacitor,
    Mica,
};

inline std::string lower_copy(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// Classify from the catalogue's `part.technology` string (the field that carries
// "ceramic-class-2", "aluminum-electrolytic-wet", "tantalum-mno2", …).
inline CapFamily cap_family(const std::string& technology) {
    const std::string t = lower_copy(technology);
    if (t.empty()) return CapFamily::Unknown;
    if (contains(t, "ceramic")) {
        if (contains(t, "class-1") || contains(t, "class1")) return CapFamily::CeramicClass1;
        if (contains(t, "class-2") || contains(t, "class2")) return CapFamily::CeramicClass2;
        return CapFamily::Unknown;  // ceramic of unstated class — do not guess
    }
    if (contains(t, "tantalum")) {
        if (contains(t, "polymer")) return CapFamily::TantalumPolymer;
        if (contains(t, "mno2") || contains(t, "manganese")) return CapFamily::TantalumMnO2;
        return CapFamily::Unknown;
    }
    if (contains(t, "aluminum") || contains(t, "aluminium")) {
        if (contains(t, "polymer")) return CapFamily::AluminiumPolymer;
        if (contains(t, "hybrid")) return CapFamily::AluminiumHybrid;
        if (contains(t, "wet") || contains(t, "electrolytic")) return CapFamily::AluminiumWet;
        return CapFamily::Unknown;
    }
    if (contains(t, "film") || contains(t, "polypropylene") || contains(t, "polyester") ||
        contains(t, "mkp") || contains(t, "mkt"))
        return CapFamily::Film;
    if (contains(t, "supercap") || contains(t, "edlc")) return CapFamily::Supercapacitor;
    if (contains(t, "mica")) return CapFamily::Mica;
    return CapFamily::Unknown;
}

inline const char* cap_family_name(CapFamily f) {
    switch (f) {
        case CapFamily::CeramicClass1: return "ceramic class 1";
        case CapFamily::CeramicClass2: return "ceramic class 2";
        case CapFamily::AluminiumWet: return "aluminium electrolytic (wet)";
        case CapFamily::AluminiumPolymer: return "aluminium polymer";
        case CapFamily::AluminiumHybrid: return "aluminium hybrid";
        case CapFamily::TantalumMnO2: return "tantalum (MnO2)";
        case CapFamily::TantalumPolymer: return "tantalum (polymer)";
        case CapFamily::Film: return "film";
        case CapFamily::Supercapacitor: return "supercapacitor";
        case CapFamily::Mica: return "mica";
        case CapFamily::Unknown: return "unknown";
    }
    return "unknown";
}

// Why a given cross-family swap is unsafe — surfaced to the user rather than
// just refused, because the reason is the useful part.
inline std::string cap_family_conflict(CapFamily o, CapFamily s) {
    if (o == CapFamily::Unknown || s == CapFamily::Unknown || o == s) return "";
    auto is_tant = [](CapFamily f) {
        return f == CapFamily::TantalumMnO2 || f == CapFamily::TantalumPolymer;
    };
    // The two cathode systems fail differently: MnO2 can ignite under surge and
    // carries a 50%-derating convention; polymer fails benignly and derates to
    // 70-80%. Swapping either way moves a part into a role it was not derated for.
    if (is_tant(o) && is_tant(s))
        return "tantalum cathode system differs (MnO2 vs polymer): different failure mode and "
               "voltage-derating convention";
    if (o == CapFamily::CeramicClass1 && s == CapFamily::CeramicClass2)
        return "class-1 ceramic replaced by class-2: adds DC-bias derating, ageing and "
               "piezoelectric noise the original does not have";
    if (o == CapFamily::CeramicClass2 && s == CapFamily::CeramicClass1)
        return "class-2 ceramic replaced by class-1: stable, but capacitance per volume differs "
               "sharply — confirm the value is genuinely available in this size";
    if (o == CapFamily::AluminiumWet && s == CapFamily::AluminiumPolymer)
        return "wet electrolytic replaced by polymer: far lower ESR removes loop damping, and "
               "wear-out changes from capacitance fade to ESR rise";
    if (o == CapFamily::AluminiumPolymer && s == CapFamily::AluminiumWet)
        return "polymer replaced by wet electrolytic: much higher ESR, likely to miss the ripple "
               "requirement";
    return std::string("different capacitor family (") + cap_family_name(o) + " -> " +
           cap_family_name(s) + "): different failure mode and derating rules";
}

// ── Dielectric code envelope (EIA RS-198) ────────────────────────────────────
// A class-2 code is three symbols: low temperature, high temperature, and the
// maximum capacitance change over that range. Comparing the CODES as strings
// misses that X7R and X5R differ only in upper temperature, and that X7R and X7S
// differ only in tolerance — both are real regressions that a string equality
// check calls "different" without saying how, and a rank table calls "worse"
// without saying on which axis.
struct DielectricEnvelope {
    double temp_min_c = 0;
    double temp_max_c = 0;
    double delta_c_pct = 0;  // maximum |dC/C| over the range, percent
    bool class1 = false;     // C0G/NP0: ppm-stable, no bias derating, no ageing
};

inline std::optional<DielectricEnvelope> dielectric_envelope(const std::string& code_raw) {
    std::string c;
    for (char ch : code_raw)
        if (!std::isspace(static_cast<unsigned char>(ch)))
            c += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (c.empty()) return std::nullopt;

    // Class 1: C0G / NP0 are the same material system (±30 ppm/degC).
    if (c == "C0G" || c == "NP0" || c == "NPO") return DielectricEnvelope{-55, 125, 0.3, true};
    if (c == "U2J") return DielectricEnvelope{-55, 125, 0.75, true};
    if (c == "CH" || c == "CG") return DielectricEnvelope{-55, 125, 0.6, true};

    // Class 2: <low temp><high temp><max change>.
    static const struct { char k; double v; } kLow[] = {
        {'X', -55}, {'Y', -30}, {'Z', 10}};
    static const struct { char k; double v; } kHigh[] = {
        {'4', 65}, {'5', 85}, {'6', 105}, {'7', 125}, {'8', 150}, {'9', 200}};
    static const struct { char k; double v; } kDelta[] = {
        {'A', 1.0},  {'B', 1.5},  {'C', 2.2},  {'D', 3.3},  {'E', 4.7}, {'F', 7.5},
        {'P', 10.0}, {'R', 15.0}, {'S', 22.0}, {'T', 33.0}, {'U', 56.0}, {'V', 82.0}};
    if (c.size() != 3) return std::nullopt;
    DielectricEnvelope e;
    bool lo = false, hi = false, d = false;
    for (const auto& x : kLow)
        if (x.k == c[0]) { e.temp_min_c = x.v; lo = true; }
    for (const auto& x : kHigh)
        if (x.k == c[1]) { e.temp_max_c = x.v; hi = true; }
    for (const auto& x : kDelta)
        if (x.k == c[2]) { e.delta_c_pct = x.v; d = true; }
    if (!lo || !hi || !d) return std::nullopt;
    return e;
}

// Compare two dielectric codes on all three axes. Returns an empty string when
// the substitute's envelope covers the original's; otherwise the specific
// regression, naming the axis. Empty also when either code is undecodable — the
// caller reports that as unverified, never as a pass.
inline std::string dielectric_regression(const std::string& original, const std::string& substitute) {
    auto o = dielectric_envelope(original);
    auto s = dielectric_envelope(substitute);
    if (!o || !s) return "";
    std::vector<std::string> issues;
    if (s->temp_max_c < o->temp_max_c)
        issues.push_back("upper temperature " + std::to_string(static_cast<int>(s->temp_max_c)) +
                         " degC vs " + std::to_string(static_cast<int>(o->temp_max_c)));
    if (s->temp_min_c > o->temp_min_c)
        issues.push_back("lower temperature " + std::to_string(static_cast<int>(s->temp_min_c)) +
                         " degC vs " + std::to_string(static_cast<int>(o->temp_min_c)));
    if (s->delta_c_pct > o->delta_c_pct)
        issues.push_back("capacitance change +/-" + std::to_string(static_cast<int>(s->delta_c_pct)) +
                         "% vs +/-" + std::to_string(static_cast<int>(o->delta_c_pct)) + "%");
    if (!o->class1 && s->class1) return "";  // class 1 for class 2 is an improvement
    if (o->class1 && !s->class1)
        issues.push_back("class-1 dielectric replaced by class-2 (adds bias derating and ageing)");
    if (issues.empty()) return "";
    std::string out = "dielectric regression: " + issues[0];
    for (size_t i = 1; i < issues.size(); ++i) out += "; " + issues[i];
    return out;
}

// ── Resistor device class: discrete chip vs multi-element array/network ──────
// A catalogue "resistor" is normally a two-terminal discrete. A chip resistor
// ARRAY (network) is a different device: N isolated elements in one body, 2N
// terminals along the long edges, and a power rating quoted PER ELEMENT. Its
// body OUTLINE is an ordinary chip size — a Panasonic EXB-V8V is 3.2 x 1.6 mm,
// exactly a 1206 — so a footprint check made on the outline alone reports "fits"
// for a discrete 1206 whose two end terminations land on none of the array's
// eight pads: three of the four nets are left open. Replacing one array takes a
// discrete PER ELEMENT and a new land pattern (ABT #481).
//
// The only device-class evidence a RAS resistor record carries is
// manufacturerInfo.family / part.series — the schema has no element- or
// terminal-count field — so that string is what is read. It comes in two forms:
//   * self-describing: "Chip Resistor Array", "Chip Resistor Networks",
//     "Anti-Sulfurated Chip Resistor Array", "AF_Array" (2,724 records);
//   * a vendor series designator that names an array line without saying so in
//     English (1,183 records), listed below — each confirmed against the
//     datasheet the catalogue record ITSELF links, and each verified to be
//     carried by every record of that line and by no other part.
// A record matching neither is NOT asserted to be discrete: it carries no
// declaration, and the gate below only fires when exactly one side declares a
// network. An undeclared array offered against a discrete original therefore
// still slips through — that is a catalogue data gap, not something to guess at.
inline bool declares_resistor_network(const std::string& family, const std::string& mpn) {
    const std::string f = lower_copy(family);
    if (contains(f, "array") || contains(f, "network")) return true;
    const std::string m = lower_copy(mpn);
    auto starts = [&m](const char* p) { return m.rfind(p, 0) == 0; };
    // YAGEO YC / TC — "Chip Resistor Arrays", datasheet PYU-YC_TC_GROUP_51_ROHS_L.
    // Family designator and MPN prefix agree on all 1,172 records and no other
    // part carries either, so both are required rather than either alone.
    if ((f == "yc" && starts("yc")) || (f == "tc" && starts("tc"))) return true;
    // Bourns CAT / CAY — "Chip Resistor Arrays", datasheet CATCAY.pdf. These
    // records carry no family and no series at all, so the MPN is the only handle.
    if ((starts("cat") || starts("cay")) && m.size() > 3 &&
        std::isdigit(static_cast<unsigned char>(m[3])))
        return true;
    // Panasonic EXB — the whole EXB line is chip resistor arrays/networks
    // (datasheets AOC0000C12 / C14 / C20). 2,605 of the 2,606 EXB records say so
    // in their family string; this catches the one that does not.
    if (starts("exb")) return true;
    return false;
}

// Why an array <-> discrete swap is not a substitution, phrased for the direction
// it actually happened in. Empty when the two sides agree, or when neither side
// declares a class.
inline std::string resistor_network_conflict(bool original_is_network, bool substitute_is_network) {
    if (original_is_network == substitute_is_network) return "";
    if (original_is_network)
        return "the original is a multi-element chip resistor array/network and this is a "
               "discrete two-terminal resistor: the body outline may match, but the land "
               "pattern does not — one discrete covers two of the array's pads and leaves the "
               "other elements' nets open. It takes one discrete PER ELEMENT plus a new PCB "
               "land pattern to replace the array, and the array's power rating is per "
               "element, not per package";
    return "this is a multi-element chip resistor array/network and the original is a discrete "
           "two-terminal resistor: it carries several isolated elements on a land pattern the "
           "original's two pads cannot connect";
}

// ── Quartz crystal load capacitance ──────────────────────────────────────────
// A crystal does not have a frequency on its own: it has a frequency AT a
// specified load capacitance. The board's load network (two capacitors plus
// stray) is designed for that CL, so dropping in a crystal specified for a
// different CL leaves the same load network pulling it to the wrong frequency.
// Nothing about the part looks wrong — the marking still says 16 MHz.
//
// For parallel resonance the pull is
//     f_L = f_s * (1 + C1 / (2 * (C0 + C_L)))
// so swapping C_L(orig) for C_L(sub) shifts the frequency by
//     df/f ~= (C1/2) * (1/(C0 + CL_orig) - 1/(C0 + CL_sub))
// Typical AT-cut values (C1 ~ 5 fF motional, C0 ~ 3 pF shunt) give tens of ppm
// for a few pF of mismatch — usually several times a typical +/-20 ppm budget.
// The estimate below uses those typical values and is reported as an ESTIMATE,
// because the real C1/C0 are per-part and most catalogue records omit them.
inline constexpr double kTypicalMotionalC = 5e-15;  // C1, farads
inline constexpr double kTypicalShuntC = 3e-12;     // C0, farads

// Frequency pull in ppm from using cl_sub where the board expects cl_orig.
// Both in farads. Returns nullopt when either is missing or non-physical.
inline std::optional<double> crystal_pull_ppm(std::optional<double> cl_orig,
                                              std::optional<double> cl_sub) {
    if (!cl_orig || !cl_sub || *cl_orig <= 0 || *cl_sub <= 0) return std::nullopt;
    const double a = kTypicalShuntC + *cl_orig, b = kTypicalShuntC + *cl_sub;
    if (a <= 0 || b <= 0) return std::nullopt;
    return (kTypicalMotionalC / 2.0) * (1.0 / a - 1.0 / b) * 1e6;
}

// True when the record is a passive resonator, whose frequency depends on the
// board's load network. An oscillator (XO/TCXO/VCXO/MEMS) contains its own
// circuit and has no load-capacitance dependency, so the gate must not fire.
//
// `technology` is the authoritative discriminator and device_type is only a
// fallback: in TAS every timing part sits under the `timeBase.oscillator`
// container, so device_type reads "oscillator" even for a plain quartz crystal.
// Treating that as disqualifying silently disabled this gate on all 518 crystals
// in the catalogue. The data confirms the rule cleanly — load capacitance is
// present on 99% of technology=quartzCrystal parts and on 0% of every active
// type (crystalOscillator, mems, tcxo, ocxo, vcxo, programmable).
inline bool is_passive_resonator(const std::string& technology, const std::string& device_type) {
    const std::string t = lower_copy(technology);
    if (!t.empty()) {
        if (contains(t, "oscillator") || contains(t, "tcxo") || contains(t, "vcxo") ||
            contains(t, "ocxo") || contains(t, "mems") || contains(t, "programmable"))
            return false;
        return contains(t, "crystal") || contains(t, "quartz") || contains(t, "resonator");
    }
    const std::string d = lower_copy(device_type);
    return contains(d, "crystal") || contains(d, "resonator");
}

// ── Semiconductor process technology ─────────────────────────────────────────
// Si, SiC and GaN are not drop-in for one another even at matching Vds/Id: gate
// drive differs (GaN needs a tightly controlled, lower Vgs and has no body
// diode in the usual sense; SiC typically wants a higher, often negative-off
// drive). A swap across processes is a gate-driver redesign, not a substitution.
inline std::string process_technology(const std::string& raw) {
    const std::string t = lower_copy(raw);
    if (contains(t, "gan")) return "GaN";
    if (contains(t, "sic") || contains(t, "silicon carbide")) return "SiC";
    if (contains(t, "si") || contains(t, "silicon")) return "Si";
    return "";
}

inline std::string process_conflict(const std::string& original, const std::string& substitute) {
    const std::string o = process_technology(original), s = process_technology(substitute);
    if (o.empty() || s.empty() || o == s) return "";
    return o + " device replaced by " + s +
           ": different gate-drive requirements — not a drop-in without driver changes";
}

}  // namespace kelvin::crossref
