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
