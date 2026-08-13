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

// ── Capacitor line-safety subclass (IEC 60384-14: X1/X2/X3, Y1/Y2/Y3/Y4) ────
// A capacitor across the mains is not judged by its numbers. It is judged by the
// SAFETY APPROVAL it carries, because the thing the approval buys is what happens
// when the part fails: an X capacitor sits line-to-line, where a shorted part is a
// fire, and a Y capacitor sits line-to-earth, where a shorted part puts mains on
// the chassis. IEC 60384-14 grades that by the impulse the part must survive —
// X1 2.5 < Up <= 4 kV, X2 Up <= 2.5 kV, X3 no impulse test; Y1 8 kV, Y2 5 kV,
// Y3 none, Y4 2.5 kV — and by the insulation duty each class is approved for.
//
// So X1 -> X2 is a downgrade in the one property the part exists for, and no
// margin anywhere else compensates: a WIMA MKP-X1 R (440 VAC, X1) came back with
// six KEMET R46 X2 candidates graded recommended / drop_in / UPGRADE, voltage
// "pass", notes null (ABT #557). The ranker compared a 560 V figure against 440 V
// and never saw the class at all.
//
// There is no safety-class field in the catalogue schema (filed as ABT #677), so
// what is read is the only evidence a TAS capacitor record carries: the series /
// family string, where the approval is written into the series NAME — "MKP-X1 R",
// "MKP-X2", "MKP-Y2", "R47 X1 440 VAC", "R53 X2 310 VAC". 4,227 of the 253,830
// capacitor records name a class this way, in 7 distinct strings, all of them
// unambiguous.
//
// What is NOT done here is a guess. A series string that names no class yields
// UNKNOWN — not "probably X2", not "not a safety part". KEMET's R46 is in fact an
// X2 line ("R46 275 VAC" plus an X2 datasheet URL), but the string the shard
// carries does not say so, and a ranker that inferred it would be asserting a
// safety approval it cannot show. Unknown is reported as unknown, and the caller
// treats it as unverified — never as a pass.
struct SafetyClass {
    // Rank within an axis, higher = the more onerous approval. Absent = the
    // record names no class on that axis, which is UNKNOWN, not "none".
    std::optional<int> x;  // X3=1, X2=2, X1=3   (line-to-line, across the mains)
    std::optional<int> y;  // Y4=1, Y3=2, Y2=3, Y1=4 (line-to-earth)
    bool any() const { return x.has_value() || y.has_value(); }
};

// The class name as the datasheet writes it, from a rank on one axis.
inline std::string safety_class_name(char axis, int rank) {
    const int digit = (axis == 'X') ? (4 - rank) : (5 - rank);
    return std::string(1, axis) + std::to_string(digit);
}

// Decode X1/X2/X3/Y1..Y4 out of a series / family string.
//
// Matched as a whole TOKEN, never as a substring: the string is split on every
// character that is not a letter or a digit, and a piece counts only when it is
// exactly the two characters. That boundary is what keeps the decode honest —
// substring matching would read Murata's "X2Y" integrated EMI filter as an X2
// approval, and would fire on any series code that happens to contain the pair.
inline SafetyClass safety_class(const std::string& series) {
    SafetyClass out;
    std::string tok;
    const auto take = [&] {
        if (tok.size() != 2) return;
        const char a = static_cast<char>(std::toupper(static_cast<unsigned char>(tok[0])));
        const char d = tok[1];
        if (d < '1' || d > '4') return;
        if (a == 'X' && d <= '3') {
            const int rank = 4 - (d - '0');
            if (!out.x || rank > *out.x) out.x = rank;
        } else if (a == 'Y') {
            const int rank = 5 - (d - '0');
            if (!out.y || rank > *out.y) out.y = rank;
        }
    };
    for (char c : series) {
        if (std::isalnum(static_cast<unsigned char>(c))) tok += c;
        else { take(); tok.clear(); }
    }
    take();
    return out;
}

// Why an X1 is not replaced by an X2. Stated as the approval change it is, with
// both class names, because "voltage: pass" is what the engineer would otherwise
// read off the row.
inline std::string safety_class_downgrade(char axis, int orig_rank, int sub_rank) {
    const std::string o = safety_class_name(axis, orig_rank), s = safety_class_name(axis, sub_rank);
    const std::string where = (axis == 'X')
                                  ? "line-to-line across the mains, where a shorted capacitor is "
                                    "a fire"
                                  : "line-to-earth, where a shorted capacitor puts mains on the "
                                    "chassis";
    return "line-safety approval downgrade " + o + " -> " + s +
           ": the original is an IEC 60384-14 class-" + o + " capacitor and the substitute's "
           "record names class " + s + ", a lower impulse-withstand grade for a part sitting " +
           where + ". This is a safety approval, not a margin — no rated-voltage headroom "
           "substitutes for it, and the swap is not certifiable where the original's class was "
           "the requirement";
}

// Why an X1 original and a substitute whose record names no class are not
// comparable. The claim made is exactly the true one — the check could not be
// run — and never that the substitute is or is not a safety part.
inline std::string safety_class_unverified(char axis, int orig_rank,
                                           const std::string& substitute_series) {
    const std::string o = safety_class_name(axis, orig_rank);
    const std::string names = substitute_series.empty()
                                  ? "this substitute's record names no series at all"
                                  : "this substitute's record names series \"" +
                                        substitute_series + "\"";
    return "line-safety class UNVERIFIED: the original is an IEC 60384-14 class-" + o +
           " mains capacitor and " + names +
           " and states no safety class, so the approval could not be compared. Kelvin's "
           "catalogue carries no safety-class field (ABT #677) — the class is only ever read "
           "from the series name — so this is an absence of evidence, not evidence the "
           "substitute is unapproved. The rated-voltage row above compares catalogue voltages "
           "and is NOT the AC mains rating this class is granted against. Confirm the "
           "substitute's X/Y approval on its datasheet before this swap";
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

// ── Resistor sulfuration resistance: a construction class, not a parameter ──
// A standard thick-film chip resistor's inner electrode is silver. In a
// sulfur-bearing atmosphere — automotive, HVAC, rubber-sealed enclosures — that
// silver corrodes to Ag2S, the termination loses contact with the resistive
// element, and the part fails OPEN. No catalogue column predicts it, which is
// exactly why vendors sell a sulfur-resistant series ALONGSIDE the electrically
// identical standard one: Panasonic's ERJ-S beside ERJ, YAGEO's AF beside RC.
//
// So the two records agree on every number — 63.4 ohm, 1 %, 1 W, 2512,
// 100 ppm/degC — and both read technology "thickFilm". No parametric verdict can
// ever separate them, and offered against Panasonic ERJS1TF63R4U the ordinary
// Bourns CR2512-FX-63R4ELF came back drop_in / recommended, all four verdicts
// "pass" and notes null (ABT #518).
//
// The only evidence a RAS resistor record carries is manufacturerInfo.family /
// part.series — the schema has no termination-metallisation field — so that
// string is what is read, in two forms, both counted over the whole resistor
// catalogue (149,255 records):
//   * self-describing: "Anti-Sulfurated Thick Film Chip Resistors" and its
//     Anti-Surge / Precision / Array / Wide-Terminal variants (32,656 records);
//   * a vendor series designator whose OWN linked datasheet titles the line
//     anti-sulfurated (4,428 records), listed below.
// Two different questions are asked of that string, and the asymmetry is the
// point:
//   by design — was the ORIGINAL specified for sulfur resistance? Only a series
//               that EXISTS for it counts, or every part that merely lists the
//               property among its features would drag the catalogue through
//               this gate;
//   declares  — does the SUBSTITUTE state sulfur resistance at all? Here a
//               feature bullet is enough: a part that claims the property claims
//               it, whatever its series was named for.
// A record matching neither is NOT asserted to be sulfur-vulnerable. It states
// nothing, and that is what the note says.

// A vendor series designator, matched the way a global part number is built: the
// code, then the size digits ("AF2512…"). The family string must agree or be
// absent — never name a DIFFERENT line — which is what stops a two-letter code
// from firing on another vendor's part. Verified across all 149,255 resistor
// records: every code below is carried by exactly one manufacturer.
inline bool named_series(const std::string& family, const std::string& mpn, const char* code) {
    const size_t n = std::char_traits<char>::length(code);
    if (mpn.size() <= n || mpn.compare(0, n, code) != 0 ||
        !std::isdigit(static_cast<unsigned char>(mpn[n])))
        return false;
    if (family.empty()) return true;  // 68 rows carry the part number and nothing else
    if (family.size() < n || family.compare(0, n, code) != 0) return false;
    // "AF" and "AF_Array" are the same line; "PS" and "PSP" are not.
    return family.size() == n || !std::isalpha(static_cast<unsigned char>(family[n]));
}

// True when the series EXISTS for sulfur resistance — the question asked of the
// ORIGINAL, because that is what makes the attribute a specified requirement
// rather than a bonus the datasheet happens to mention.
inline bool sulfur_resistant_by_design(const std::string& family, const std::string& mpn) {
    const std::string f = lower_copy(family);
    // Panasonic says it in English, in both family and part.series.
    if (contains(f, "sulfur") || contains(f, "sulphur")) return true;
    const std::string m = lower_copy(mpn);
    // YAGEO AA / AF (and AF_Array) / AH / AS / RP — each titled "ANTI-SULFURATED
    // …" on the datasheet the catalogue record itself links (PYU-AA_51,
    // PYU-AF_51, PYU-AH_51, PYU-AS, PYU-RP_51).
    for (const char* code : {"aa", "af", "ah", "as", "rp"})
        if (named_series(f, m, code)) return true;
    // Würth WRIS-RSKS, "Thick Film - AntiSulfur General Purpose", sulfur
    // resistance tested to ASTM-B-809. Its part numbers are bare order codes, so
    // the family string is the only handle there is.
    return f == "wris-rsks";
}

// True when the record states sulfur resistance at all — the question asked of
// the SUBSTITUTE. Every series above, plus the lines whose datasheets carry the
// property without being named for it.
inline bool declares_sulfur_resistance(const std::string& family, const std::string& mpn) {
    if (sulfur_resistant_by_design(family, mpn)) return true;
    const std::string f = lower_copy(family), m = lower_copy(mpn);
    // YAGEO thin films AT / NT / VT and current sensors PA / PE / PS / PK
    // ("Superior resistance against sulfur", "Resistance against sulfur-
    // containing atmosphere"), and the MELF bodies — Vishay SMM0102, YAGEO
    // Vitrohm ZCM — whose "intrinsic sulfur resistance" is having no silver
    // inner electrode to lose in the first place.
    for (const char* code : {"at", "nt", "vt", "pa", "pe", "ps", "pk", "smm", "zcm"})
        if (named_series(f, m, code)) return true;
    return false;
}

// Why an anti-sulfurated original and a substitute that states nothing are not
// the same part, phrased from what the substitute's record actually says. Asked
// one way only: a sulfur-resistant substitute for an ordinary original is a
// strictly added property and costs the engineer nothing, so it is not a caveat.
inline std::string sulfur_resistance_conflict(const std::string& substitute_family) {
    const std::string names = substitute_family.empty()
                                  ? "this substitute's record names no series at all"
                                  : "this substitute's record names series \"" +
                                        substitute_family + "\"";
    return "the original is an anti-sulfurated series — a sulfur-resistant inner electrode, "
           "the property it exists for and one no catalogue number shows — and " +
           names +
           " and states no sulfur resistance: a standard thick-film chip's silver inner "
           "electrode corrodes to Ag2S in a sulfur-bearing atmosphere (automotive, HVAC, "
           "rubber-sealed enclosures) and the resistor fails OPEN. Electrically identical is "
           "not equivalent here — confirm the substitute's construction against its datasheet "
           "before using it where the original's anti-sulfuration was the reason for it";
}

// ── Diode device configuration: two-terminal discrete vs bridge module ───────
// The ranker models a catalogue "diode" as a two-terminal device: one Vrrm, one
// Vf, one If(AV), two pads. A single-phase BRIDGE RECTIFIER is a different
// device — four dies on four terminals (AC, AC, +, -) in one body — and its
// catalogue columns do not mean the same thing. Vf is quoted PER ELEMENT while
// the current path runs through TWO elements in series, so the real in-circuit
// drop is about twice the number being compared; If(AV) is the bridge's DC
// OUTPUT current, about twice what any one element carries. Offered against a
// two-terminal original the whole verdict table compares the wrong quantities,
// and no land pattern makes a four-terminal part sit on two pads (ABT #521).
//
// The only device-class evidence a TAS diode record carries is its PACKAGE — the
// schema has no terminal- or element-count field — so that string is what is
// read. It comes in two forms, both counted over the whole diode catalogue
// (134 of 18,065 records):
//   * an outline that exists only as a single-phase bridge — GBPC (51), GBU (21),
//     GBJ (18), KBPC (12), KBP (5), MBS (5): 112 records;
//   * a generic package whose NAME states four leads — DIP-4 (11), SOIC-4 (5),
//     TSSOP-4 (3), SIP-4 (2), SOIC4 W (1): 22 records, every one of them a bridge
//     (Diodes DF/DB and MB*S/MB*M, onsemi MB1S, MDB*S, GBU8KS, DFB25100).
// A package that states neither declares nothing, and is NOT asserted to be a
// discrete: the gate below fires only when exactly one side declares a module.
// It also judges FOUR leads only — the dual/array outlines (SO-8, SOT-363-6,
// three-lead TO-220 common-cathode pairs) are multi-die parts too, but their
// case strings do not state a terminal count, so they are a separate catalogue
// gap rather than something to guess at here.

// The leading package token of a case string, lowercased:
// "GBPC4 28.75x28.75x11.10" -> "gbpc4", "SMC (DO-214AB)" -> "smc",
// "SOIC4 W" -> "soic4", "SC-88-6 / SC-70-6" -> "sc-88-6".
inline std::string case_token(const std::string& case_code) {
    std::string t;
    for (char ch : lower_copy(case_code)) {
        if (ch == ' ' || ch == '(' || ch == '/' || ch == ',') break;
        t += ch;
    }
    return t;
}

// True when the package IS a single-phase bridge outline. The trailing digits of
// "GBPC4" are the current rating of the outline, not a lead count, so they are
// stripped before the family is matched.
inline bool bridge_outline(const std::string& case_code) {
    std::string t = case_token(case_code);
    while (!t.empty() && std::isdigit(static_cast<unsigned char>(t.back()))) t.pop_back();
    static const char* kBridgeOutlines[] = {"gbpc", "gbu", "gbj", "kbpc", "kbp", "mbs"};
    for (const char* o : kBridgeOutlines)
        if (t == o) return true;
    return false;
}

// The lead count a GENERIC package name states, when it states one. Only the
// families whose trailing number IS the lead count are read this way: on a chip
// or JEDEC diode outline that number is a size or a registration number
// ("SOD-123", "SOT-23", "DO-214AB"), and reading it as terminals would turn most
// of the catalogue into a module.
inline std::optional<int> declared_lead_count(const std::string& case_code) {
    const std::string t = case_token(case_code);
    static const char* kLeadCountFamilies[] = {"dip",   "sip",  "soic", "ssop",
                                               "tssop", "msop", "sop"};
    for (const char* fam : kLeadCountFamilies) {
        const std::string f(fam);
        if (t.size() <= f.size() || t.compare(0, f.size(), f) != 0) continue;
        std::string rest = t.substr(f.size());
        if (rest[0] == '-') rest.erase(0, 1);
        if (rest.empty() || rest.size() > 3) continue;
        bool digits = true;
        for (char ch : rest)
            if (!std::isdigit(static_cast<unsigned char>(ch))) digits = false;
        if (!digits) continue;
        return std::stoi(rest);
    }
    return std::nullopt;
}

// True when the record's package declares a device that is not a two-terminal
// diode: a bridge outline, or a generic package stating the four terminals a
// single-phase bridge needs.
inline bool declares_diode_module(const std::string& case_code) {
    if (bridge_outline(case_code)) return true;
    const auto leads = declared_lead_count(case_code);
    return leads.has_value() && *leads == 4;
}

// Which four-terminal device it is — consulted ONLY once the package has already
// declared a module, so it can sharpen the wording and can never widen the gate.
// The series letters are required to be followed by a digit, which is what
// separates the bridge lines (GBU8K, MB6S, DF02M, DB107, MDB10S) from every other
// part whose MPN happens to start with the same letters.
inline bool names_bridge_series(const std::string& mpn) {
    const std::string m = lower_copy(mpn);
    static const char* kBridgeSeries[] = {"gbpc", "gbu", "gbj", "kbpc", "kbp", "mdb", "mb", "df",
                                          "db"};
    for (const char* s : kBridgeSeries) {
        const std::string p(s);
        if (m.size() > p.size() && m.compare(0, p.size(), p) == 0 &&
            std::isdigit(static_cast<unsigned char>(m[p.size()])))
            return true;
    }
    return false;
}

// Why a module <-> discrete swap is not a substitution, phrased for the direction
// it actually happened in. Empty when the two sides agree, or when neither
// package declares a module.
inline std::string diode_configuration_conflict(const std::string& original_case,
                                                const std::string& original_mpn,
                                                const std::string& substitute_case,
                                                const std::string& substitute_mpn) {
    const bool original_is_module = declares_diode_module(original_case);
    const bool substitute_is_module = declares_diode_module(substitute_case);
    if (original_is_module == substitute_is_module) return "";
    const std::string& mod_case = original_is_module ? original_case : substitute_case;
    const std::string& mod_mpn = original_is_module ? original_mpn : substitute_mpn;
    const bool bridge = bridge_outline(mod_case) || names_bridge_series(mod_mpn);
    const std::string what =
        bridge ? "a single-phase BRIDGE RECTIFIER module: four dies on four terminals "
                 "(AC, AC, +, -)"
               : "a multi-terminal module: its package (" + mod_case +
                     ") states four leads, where a two-terminal diode has two";
    const std::string ratings =
        bridge ? "its Vf is quoted PER ELEMENT and the current runs through TWO elements in "
                 "series, so the real in-circuit drop is about twice the figure compared here, "
                 "and its If(AV) is the bridge's DC OUTPUT current, about twice what any one "
                 "element carries — neither is a per-diode rating comparable to the other side's"
               : "its Vf and If(AV) are the MODULE's ratings, not the per-diode ratings this "
                 "table has put beside them";
    if (substitute_is_module)
        return "this is " + what +
               ", and the original is a two-terminal diode: no land pattern makes it sit on the "
               "original's two pads, and " +
               ratings;
    return "the original is " + what +
           ", and this is a two-terminal diode: it replaces ONE of the module's elements on a "
           "different land pattern — a bridge takes four discretes plus a new land pattern to "
           "replace — and " +
           ratings;
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
