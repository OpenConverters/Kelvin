// MPN decoding + MLCC DC-bias — ported from Heaviside's crossref_pipeline.py
// (_AUTOMOTIVE_MPN_PAT / _decode_cap_mpn / _is_automotive_grade) and
// param_check.py (effective_capacitance_at_bias / mlcc_bias_param).
//
// Why decode the MPN at all: these gates must fire even when the ORIGINAL is not
// in the catalogue. An engineer cross-referencing a part we have no record of
// still deserves to be told that the proposed substitute is a commercial part
// standing in for an automotive one, or is rated for less voltage.
//
// Everything here is conservative by construction: an unrecognised MPN decodes to
// nothing rather than to a guess, because a wrong decode would mis-gate a real
// part. Only well-known vendor conventions are listed.
#pragma once
#include <cmath>
#include <optional>
#include <regex>
#include <string>

namespace kelvin::crossref {

// ── AEC-Q automotive grade from the MPN ──────────────────────────────────────
// Lists each vendor's automotive convention (the grade of the PART), not a
// preferred vendor. Murata GRM is commercial; GCM/GCJ/GCG/GRT/GJ8/KRM/KCM are not.
inline bool is_automotive_grade(const std::string& mpn) {
    if (mpn.empty()) return false;
    static const std::regex pat(
        R"((^(GC[MJG]|GRT|GJ8|KRM|KCM)\d)|(AUTO$)|(^CGA\d)|(^AC\d)|(^AEC))",
        std::regex::icase);
    std::string m = mpn;
    // trim
    size_t b = m.find_first_not_of(" \t");
    if (b == std::string::npos) return false;
    m = m.substr(b, m.find_last_not_of(" \t") - b + 1);
    return std::regex_search(m, pat);
}

// ── Capacitor rated voltage / dielectric from the MPN ────────────────────────
struct CapDecode {
    std::optional<double> voltage;   // V
    std::string dielectric;          // "" when not literally present
};

inline std::optional<double> cap_voltage_code(const std::string& code) {
    static const std::map<std::string, double> t{
        {"0E", 2.5},  {"0G", 4.0},   {"0J", 6.3},   {"1A", 10.0},  {"1C", 16.0},
        {"1D", 20.0}, {"1E", 25.0},  {"1V", 35.0},  {"YA", 35.0},  {"1H", 50.0},
        {"2A", 100.0},{"2D", 200.0}, {"2E", 250.0}, {"2W", 450.0}, {"2J", 630.0},
        {"3A", 1000.0}, {"3D", 2000.0}};
    auto it = t.find(code);
    return it == t.end() ? std::nullopt : std::optional<double>(it->second);
}

// Best-effort decode of rated voltage (and dielectric when literal). Returns an
// empty result when nothing is confidently decodable — never guesses. Murata
// encodes the dielectric as a coded temperature characteristic rather than
// literally, so it is only read where it appears verbatim.
inline CapDecode decode_cap_mpn(const std::string& mpn) {
    CapDecode out;
    if (mpn.empty()) return out;
    std::string m;
    for (char c : mpn) m += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    static const char* literals[] = {"C0G", "NP0", "X8R", "X7T", "X7S",
                                     "X7R", "X6S", "X5R", "U2J", "Y5V", "Z5U"};
    for (const char* d : literals) {
        if (m.find(d) != std::string::npos) {
            std::string s(d);
            out.dielectric = (s == "C0G" || s == "NP0") ? "C0G/NP0" : s;
            break;
        }
    }
    // Murata/TDK 2-char voltage code immediately before the 3-digit cap code.
    static const std::regex vcode(R"(([0-9][A-Z])(\d{3})[A-Z])");
    std::string last;
    for (auto it = std::sregex_iterator(m.begin(), m.end(), vcode);
         it != std::sregex_iterator(); ++it) {
        last = (*it)[1].str();
    }
    if (!last.empty()) {
        if (auto v = cap_voltage_code(last)) { out.voltage = *v; return out; }
    }
    if (!out.dielectric.empty()) {
        // Venkel/generic: 3 digits right after an explicit dielectric literal is
        // an EIA voltage code (two significant figures x 10^multiplier).
        static const std::regex eia(R"((?:C0G|NP0|X[5678][RST]|Y5V|Z5U)(\d)(\d)(\d))");
        std::smatch em;
        if (std::regex_search(m, em, eia)) {
            int sig = std::stoi(em[1].str() + em[2].str());
            out.voltage = sig * std::pow(10.0, std::stoi(em[3].str()));
        }
    }
    return out;
}

// ── MLCC DC-bias effective capacitance ───────────────────────────────────────
// Class-2 ceramics lose capacitance under DC bias — a "10 uF" X5R can measure
// 3-4 uF at its operating voltage — so comparing nameplate values alone is
// misleading for a substitution. Model C(V) = C_nom / (1 + (V/vth)^k), fitting k
// from the two REAL anchors carried in the catalogue (no estimation):
//   * 50% loss at vthMLCC              -> C(vth) = C_nom/2  (implicit in the form)
//   * capacitanceSaturationMLCC        -> C(v_rated) = sat * C_nom
// Returns nullopt (-> "unverified", never an estimate) when an anchor is missing
// or out of range, and for class-1 (C0G/NP0) parts, which do not bias-derate.
inline std::optional<double> effective_capacitance_at_bias(std::optional<double> c_nom,
                                                           std::optional<double> v_rated,
                                                           std::optional<double> sat_ratio,
                                                           std::optional<double> vth,
                                                           double v_op) {
    if (!c_nom || *c_nom <= 0 || v_op < 0) return std::nullopt;
    if (!vth || *vth <= 0) return std::nullopt;
    if (!sat_ratio || *sat_ratio <= 0 || *sat_ratio >= 1) return std::nullopt;
    if (!v_rated || *v_rated <= 0 || *v_rated == *vth) return std::nullopt;
    double ratio = (1.0 - *sat_ratio) / *sat_ratio;  // = (v_rated/vth)^k
    if (ratio <= 0) return std::nullopt;
    double k = std::log(ratio) / std::log(*v_rated / *vth);
    if (k <= 0) return std::nullopt;
    return *c_nom / (1.0 + std::pow(v_op / *vth, k));
}

}  // namespace kelvin::crossref
