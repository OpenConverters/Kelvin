// Physical-fit engine for cross-referencing — ported from Heaviside's
// case_dimensions.py + crossref_pipeline.py (_footprint_penalty / _footprint_tier)
// and guardrails.py (_gfoot_footprint_compatibility mount-type rules).
//
// A substitute must fit the board space the original occupies, so the ranker
// needs real dimensions. Many catalogue records carry only a case-code string,
// so a code is resolved to (L, W, H) from published tables — IPC-7351 / EIA /
// JEDEC outlines and vendor mechanicals. Every number here comes from a
// published table; an unrecognised code yields nullopt (surface the gap, never
// guess a size).
//
// THE central rule (Heaviside's gotcha #4): the same 4-digit string means
// different things per component family — "4020" is 4.0x2.0 mm as a chip but
// 4.0x4.0x2.0 mm on a molded power inductor, and "DxL" on an electrolytic is
// diameter x height. Resolution is therefore CATEGORY-AWARE.
//
// Units: metres throughout, matching both the Python and the TAS mechanical
// fields (datasheetInfo.mechanical.length et al are metres).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace kelvin::crossref {

// (length, width, height) in metres; height nullopt when the code fixes only a
// footprint (chip MLCC height varies with value/dielectric and is not encoded).
struct Dims {
    double length = 0.0;
    double width = 0.0;
    std::optional<double> height;
};

namespace dims_detail {

inline std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

inline std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Uppercase and collapse whitespace/underscores to hyphens, keeping meaningful
// hyphens (SOT-23) — mirrors Python _norm_pkg.
inline std::string norm_pkg(const std::string& s) {
    static const std::regex sep(R"([\s_]+)");
    return std::regex_replace(upper(trim(s)), sep, "-");
}

struct LWH {
    double l, w, h;  // mm; h < 0 means "not fixed by this code"
};

// Chip passives: EIA imperial code -> L, W, H (mm). H applies to thick-film chip
// RESISTORS (near-fixed); for MLCCs it is dropped by the caller.
inline const std::map<std::string, LWH>& chip_imperial() {
    static const std::map<std::string, LWH> t{
        {"01005", {0.40, 0.20, 0.13}}, {"0201", {0.60, 0.30, 0.23}},
        {"0402", {1.00, 0.50, 0.35}},  {"0603", {1.60, 0.80, 0.45}},
        {"0805", {2.00, 1.25, 0.50}},  {"1206", {3.20, 1.60, 0.55}},
        {"1210", {3.20, 2.50, 0.55}},  {"1808", {4.50, 2.00, -1}},
        {"1812", {4.50, 3.20, 0.60}},  {"2010", {5.00, 2.50, 0.55}},
        {"2220", {5.70, 5.00, 0.60}},  {"2225", {5.70, 6.35, 0.60}},
        {"2512", {6.30, 3.20, 0.55}},
    };
    return t;
}

// Metric (IEC) chip codes that are NOT also a standard imperial code, so seeing
// one unambiguously means metric.
inline const std::map<std::string, LWH>& chip_metric() {
    static const std::map<std::string, LWH> t{
        {"1005", {1.00, 0.50, -1}}, {"1608", {1.60, 0.80, -1}}, {"2012", {2.00, 1.25, -1}},
        {"3216", {3.20, 1.60, -1}}, {"3225", {3.20, 2.50, -1}}, {"4520", {4.50, 2.00, -1}},
        {"4532", {4.50, 3.20, -1}}, {"5025", {5.00, 2.50, -1}}, {"5750", {5.70, 5.00, -1}},
        {"6332", {6.30, 3.20, -1}},
    };
    return t;
}

// Molded tantalum EIA letter. A-D agree across KEMET/AVX/Vishay; later letters
// disagree by vendor and are omitted rather than guessed.
inline const std::map<std::string, LWH>& tantalum_letter() {
    static const std::map<std::string, LWH> t{
        {"A", {3.20, 1.60, 1.80}}, {"B", {3.50, 2.80, 1.90}},
        {"C", {6.00, 3.20, 2.50}}, {"D", {7.30, 4.30, 2.80}},
    };
    return t;
}

inline const std::map<std::string, LWH>& tantalum_metric() {
    static const std::map<std::string, LWH> t{
        {"1608", {1.60, 0.80, -1}}, {"2012", {2.00, 1.25, -1}}, {"3216", {3.20, 1.60, -1}},
        {"3528", {3.50, 2.80, -1}}, {"6032", {6.00, 3.20, -1}}, {"7343", {7.30, 4.30, -1}},
    };
    return t;
}

// Discrete semiconductor + IC packages: canonical name -> body L, W, H (mm).
inline const std::map<std::string, LWH>& package_mm() {
    static const std::map<std::string, LWH> t{
        {"SOT-23", {2.90, 1.30, 1.10}},   {"SOT-23-5", {2.90, 1.60, 1.10}},
        {"SOT-23-6", {2.90, 1.60, 1.10}}, {"SOT-323", {2.00, 1.25, 0.95}},
        {"SOT-353", {2.00, 1.25, 0.95}},  {"SOT-363", {2.00, 1.25, 0.95}},
        {"SOT-563", {1.60, 1.20, 0.60}},  {"SOT-666", {1.60, 1.20, 0.55}},
        {"SOT-89", {4.50, 2.50, 1.50}},   {"SOT-223", {6.50, 3.50, 1.80}},
        {"SOD-123", {2.70, 1.60, 1.10}},  {"SOD-323", {1.70, 1.25, 0.95}},
        {"SOD-523", {1.20, 0.80, 0.60}},  {"SOD-882", {1.00, 0.60, 0.48}},
        {"SOD-128", {3.80, 2.60, 1.00}},  {"DPAK", {6.10, 6.60, 2.30}},
        {"D2PAK", {10.00, 9.00, 4.50}},   {"TO-220", {10.16, 4.57, 15.70}},
        {"TO-247", {15.90, 5.30, 20.00}}, {"TO-92", {4.50, 3.80, 5.00}},
        {"POWERPAK-1212-8", {3.30, 3.30, 1.07}}, {"POWERPAK-SO-8", {6.15, 5.15, 1.00}},
        {"SOIC-8", {4.90, 3.90, 1.75}},   {"SOIC-14", {8.65, 3.90, 1.75}},
        {"SOIC-16", {9.90, 3.90, 1.75}},  {"TSSOP-8", {3.00, 4.40, 1.20}},
        {"TSSOP-14", {5.00, 4.40, 1.20}}, {"TSSOP-16", {5.00, 4.40, 1.20}},
        {"MSOP-8", {3.00, 3.00, 1.10}},   {"MSOP-10", {3.00, 3.00, 1.10}},
    };
    return t;
}

inline const std::map<std::string, std::string>& package_aliases() {
    static const std::map<std::string, std::string> t{
        {"SC-59", "SOT-23"},    {"TO-236", "SOT-23"},   {"SC-59A", "SOT-23"},
        {"SOT-25", "SOT-23-5"}, {"SC-74A", "SOT-23-5"}, {"SC-70", "SOT-323"},
        {"SC-70-3", "SOT-323"}, {"SC-70-5", "SOT-353"}, {"SC-88A", "SOT-353"},
        {"SC-70-6", "SOT-363"}, {"SC-88", "SOT-363"},   {"TO-243", "SOT-89"},
        {"SC-62", "SOT-89"},    {"TO-261", "SOT-223"},  {"SC-73", "SOT-223"},
        {"TO-252", "DPAK"},     {"TO-252AA", "DPAK"},   {"TO-263", "D2PAK"},
        {"TO-263AB", "D2PAK"},  {"DDPAK", "D2PAK"},     {"SO-8", "SOIC-8"},
        {"SOIC8", "SOIC-8"},    {"SO8", "SOIC-8"},      {"SO-14", "SOIC-14"},
        {"SO-16", "SOIC-16"},
    };
    return t;
}

inline Dims to_metres(const LWH& v) {
    Dims d;
    d.length = v.l / 1000.0;
    d.width = v.w / 1000.0;
    if (v.h > 0) d.height = v.h / 1000.0;
    return d;
}

inline std::optional<LWH> resolve_package(const std::string& code) {
    std::string key = norm_pkg(code);
    auto alias = package_aliases().find(key);
    if (alias != package_aliases().end()) key = alias->second;
    auto it = package_mm().find(key);
    if (it != package_mm().end()) return it->second;
    // tolerate a vendor-prepended prefix ("PACKAGE-SOT-23" -> "SOT-23")
    for (const auto& [canon, v] : package_mm()) {
        if (key.size() > canon.size() &&
            key.compare(key.size() - canon.size(), canon.size(), canon) == 0) {
            return v;
        }
    }
    return std::nullopt;
}

}  // namespace dims_detail

// Resolve a case/package code to physical dimensions (metres), category-aware.
// nullopt when the code is not recognised — never a guess.
inline std::optional<Dims> resolve_dimensions(const std::string& case_code,
                                              const std::string& category) {
    using namespace dims_detail;
    std::string raw = trim(case_code);
    if (raw.empty()) return std::nullopt;
    std::string cat = category;
    for (auto& c : cat) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // 1) Named packages (SOT/SOD/TO/DPAK/SOIC/...) and their aliases.
    if (auto pkg = resolve_package(raw)) return to_metres(*pkg);

    // 2) Molded tantalum EIA letter (single A-D), capacitors only.
    if (cat == "capacitor") {
        auto it = tantalum_letter().find(upper(raw));
        if (it != tantalum_letter().end()) return to_metres(it->second);
    }

    // 3) Aluminium electrolytic / any "DxL" can code -> diameter x diameter x length.
    {
        static const std::regex dxl(R"((\d{1,2}(?:\.\d)?)\s*[xX]\s*(\d{1,3}(?:\.\d)?))");
        std::smatch m;
        if (std::regex_search(raw, m, dxl) && (cat == "capacitor" || cat.empty())) {
            double d = std::stod(m[1].str()), ln = std::stod(m[2].str());
            // Guard against a "4x4" that is really a footprint: a can's length is
            // comparable to or greater than its diameter.
            if (d <= 25 && ln <= 60 && ln >= d * 0.4) return to_metres(LWH{d, d, ln});
        }
    }

    // 4) Four-digit numeric codes — meaning depends on family.
    static const std::regex four(R"((?:^|[^0-9])(\d{4})(?:[^0-9]|$))");
    std::smatch fm;
    if (std::regex_search(raw, fm, four)) {
        const std::string code = fm[1].str();
        // 4a) Chip passives (and chip inductors): imperial EIA first.
        auto imp = chip_imperial().find(code);
        if (imp != chip_imperial().end()) {
            LWH v = imp->second;
            // MLCC height varies with value/dielectric -> drop it; resistors keep it.
            if (cat != "resistor") v.h = -1;
            return to_metres(v);
        }
        // 4b) Molded power inductor: NNMM = square footprint (NN) x height (MM),
        //     both in tenths of a mm. "4020" -> 4.0 x 4.0 x 2.0 mm.
        if (cat == "magnetic" || cat == "inductor" || cat == "chipbead") {
            double side = std::stod(code.substr(0, 2)) / 10.0;
            double hgt = std::stod(code.substr(2, 2)) / 10.0;
            if (side >= 0.5 && side <= 30 && hgt >= 0.3 && hgt <= 25)
                return to_metres(LWH{side, side, hgt});
        }
        // 4c) Metric chip code.
        auto met = chip_metric().find(code);
        if (met != chip_metric().end()) return to_metres(met->second);
        // 4d) Tantalum metric footprint, with optional "-NN" height suffix.
        if (cat == "capacitor") {
            auto tan = tantalum_metric().find(code);
            if (tan != tantalum_metric().end()) {
                LWH v = tan->second;
                static const std::regex tant(R"((\d{4})-(\d{2}))");
                std::smatch tm;
                if (std::regex_search(raw, tm, tant)) v.h = std::stod(tm[2].str()) / 10.0;
                return to_metres(v);
            }
        }
    }
    return std::nullopt;
}

// ── Footprint-fit penalty (Heaviside _footprint_penalty) ─────────────────────
// Orientation-agnostic on the footprint (a rotated part that still fits is not
// penalised) and 3-axis when both heights are known. Weights are Heaviside's:
// any candidate that fits outranks any candidate that doesn't, yet an oversize
// part stays finite-scored so it can still win when it is the only option.
inline constexpr double kFitAreaWeight = 0.5;
inline constexpr double kOversizeBase = 10.0;
inline constexpr double kOversizeScale = 8.0;
inline constexpr double kUnknownDimPenalty = 2.0;
inline constexpr double kDimTolerance = 1.02;  // 2% slack for rounding/terminations
inline constexpr double kSlightlyOversizeOverflow = 0.65;  // ~one EIA size up
inline constexpr double kSlightlyOversizeBase = 1.0;
inline constexpr double kSlightlyOversizeScale = 2.5;

inline double footprint_penalty(const std::optional<Dims>& source,
                                const std::optional<Dims>& candidate) {
    if (!source) return 0.0;  // cannot enforce; surfaced separately as a diagnostic
    if (!candidate) return kUnknownDimPenalty;
    double s_long = std::max(source->length, source->width);
    double s_short = std::min(source->length, source->width);
    double c_long = std::max(candidate->length, candidate->width);
    double c_short = std::min(candidate->length, candidate->width);
    if (s_long <= 0 || s_short <= 0 || c_long <= 0 || c_short <= 0) return kUnknownDimPenalty;

    bool fits = c_long <= s_long * kDimTolerance && c_short <= s_short * kDimTolerance;
    if (source->height && candidate->height)
        fits = fits && *candidate->height <= *source->height * kDimTolerance;
    if (fits) return kFitAreaWeight * ((c_long * c_short) / (s_long * s_short));

    double overflow = std::max(c_long / s_long, c_short / s_short);
    if (source->height && candidate->height && *source->height > 0)
        overflow = std::max(overflow, *candidate->height / *source->height);
    overflow = std::max(overflow - 1.0, 0.0);
    if (overflow <= kSlightlyOversizeOverflow)
        return kSlightlyOversizeBase + kSlightlyOversizeScale * overflow;
    return kOversizeBase + kOversizeScale * overflow;
}

// Categorise the fit. Derived from footprint_penalty so thresholds live in one place.
enum class FootprintTier { Fits, OneSizeLarger, Overflows, Unknown };

inline FootprintTier footprint_tier(const std::optional<Dims>& source,
                                    const std::optional<Dims>& candidate) {
    if (!source || !candidate) return FootprintTier::Unknown;
    double pen = footprint_penalty(source, candidate);
    if (pen >= kOversizeBase) return FootprintTier::Overflows;
    if (pen >= kSlightlyOversizeBase) return FootprintTier::OneSizeLarger;
    return FootprintTier::Fits;
}

inline const char* footprint_tier_name(FootprintTier t) {
    switch (t) {
        case FootprintTier::Fits: return "fits";
        case FootprintTier::OneSizeLarger: return "one_size_larger";
        case FootprintTier::Overflows: return "overflows";
        case FootprintTier::Unknown: return "unknown";
    }
    return "unknown";
}

// ── Mount-type compatibility (Heaviside guardrails GFoot) ────────────────────
// An SMD part cannot stand in for a leaded one (or vice versa) — it is a board
// respin, not a substitution. Heaviside applies this hard reject; it skips
// inductors/transformers/MOSFETs/diodes, whose package strings vary so much by
// series that generic rules produce too many false positives.
inline const std::vector<std::string>& smd_tokens() {
    static const std::vector<std::string> t{
        "0201", "0402", "0603", "0805", "1206", "1210", "1812", "2010", "2512", "2920",
        "DFN",  "SON",  "TDFN", "CSP",  "SOT",  "SOIC", "SOP",  "DPAK", "D2PAK",
        "QFN",  "BGA",  "LGA",  "SMA",  "SMB",  "SMC"};
    return t;
}

inline const std::vector<std::string>& leaded_tokens() {
    static const std::vector<std::string> t{
        "DIP", "PDIP", "TO-220", "TO-247", "TO-218", "TO-3P", "TO-126", "TO-92",
        "SIP", "RADIAL", "AXIAL", "THRU-HOLE", "THROUGH", "THT", "SNAP-IN",
        "SCREW-TERMINAL"};
    return t;
}

inline bool is_smd(const std::string& package) {
    const std::string p = dims_detail::upper(package);
    for (const auto& tok : smd_tokens())
        if (p.find(tok) != std::string::npos) return true;
    return false;
}

inline bool is_leaded(const std::string& package) {
    const std::string p = dims_detail::upper(package);
    for (const auto& tok : leaded_tokens())
        if (p.find(tok) != std::string::npos) return true;
    return false;
}

// True when the two package strings are opposite mount types (a hard reject).
// Both must classify — an unreadable string never triggers a rejection.
inline bool mount_type_incompatible(const std::string& original, const std::string& substitute) {
    if (original.empty() || substitute.empty()) return false;
    return (is_smd(original) && is_leaded(substitute)) ||
           (is_leaded(original) && is_smd(substitute));
}

// Normalise an explicit mechanical.assemblyType to "smd" / "leaded" / "".
// The catalogue states this directly for many parts, which beats inferring it
// from a package string — the only signal Heaviside had. "chassis" is left
// unclassified rather than forced into one of the two buckets.
inline std::string normalize_mount(const std::string& assembly_type) {
    std::string a = assembly_type;
    for (auto& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (a == "smt" || a == "smd" || a == "surfacemount" || a == "surface_mount") return "smd";
    if (a == "tht" || a == "th" || a == "throughhole" || a == "through_hole" || a == "radial" ||
        a == "axial")
        return "leaded";
    return "";
}

// Mount-type incompatibility using the EXPLICIT assembly types when both records
// state one, falling back to the package strings otherwise.
inline bool mount_incompatible(const std::string& o_mount, const std::string& s_mount,
                               const std::string& o_pkg, const std::string& s_pkg) {
    const std::string om = normalize_mount(o_mount), sm = normalize_mount(s_mount);
    if (!om.empty() && !sm.empty()) return om != sm;
    return mount_type_incompatible(o_pkg, s_pkg);
}

// Categories whose package strings vary too much by series for the generic mount
// rule to be safe (mirrors Heaviside's GFoot skip list).
inline bool mount_gate_applies(const std::string& category) {
    return !(category == "magnetic" || category == "chipBead" || category == "mosfet" ||
             category == "diode" || category == "igbt");
}

}  // namespace kelvin::crossref
