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
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace kelvin::crossref {

// (length, width, height) in metres. EVERY axis is optional, because catalogue
// records are: a drawing may state a length and no width, and height is nullopt
// whenever the code fixes only a footprint (chip MLCC height varies with
// value/dielectric and is not encoded). An axis the record DOES state still
// settles what it settles — see compare_land.
struct Dims {
    std::optional<double> length;
    std::optional<double> width;
    std::optional<double> height;
    // The height is the TABLE's outline value for the code (chip EIA, JEDEC package, tantalum
    // letter), not one the code itself spells out ("8x8x11.7", "10x20", "3528-21"). A code
    // fixes a land; it does not fix how tall one vendor's body is (a "0603" chip resistor
    // ships 0.35 to 0.55 mm), so the ranker never judges clearance from such a height.
    bool height_from_table = false;
    // The same for the LAND axes: length/width are the table's outline for a named code, not
    // a body the record states. Package tables are typical outlines with their own axis
    // conventions (the TO-220 row is width x thickness x height), so a table box is never
    // weighed against a record's drawn body as if it were that part's body (Qarlos
    // 2026-11-09, seed 22041502). See CrossRef.hpp, the same-case rule.
    bool land_from_table = false;
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

// No "DDPAK" -> D2PAK entry: Infineon's D-DPAK / DDPAK is its double-DPAK, top-side-cooled
// outline, a different land from the D2PAK (TO-263), and the alias only waited for a record
// spelled without the hyphen to call the two one package (Qarlos 2026-11-09).
inline const std::map<std::string, std::string>& package_aliases() {
    static const std::map<std::string, std::string> t{
        {"SC-59", "SOT-23"},    {"TO-236", "SOT-23"},   {"SC-59A", "SOT-23"},
        {"SOT-25", "SOT-23-5"}, {"SC-74A", "SOT-23-5"}, {"SC-70", "SOT-323"},
        {"SC-70-3", "SOT-323"}, {"SC-70-5", "SOT-353"}, {"SC-88A", "SOT-353"},
        {"SC-70-6", "SOT-363"}, {"SC-88", "SOT-363"},   {"TO-243", "SOT-89"},
        {"SC-62", "SOT-89"},    {"TO-261", "SOT-223"},  {"SC-73", "SOT-223"},
        {"TO-252", "DPAK"},     {"TO-252AA", "DPAK"},   {"TO-263", "D2PAK"},
        {"TO-263AB", "D2PAK"},  {"SO-8", "SOIC-8"},
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

// The lookup key of a package name: norm_pkg, then a hyphen that only separates the
// letter prefix of a registered name from its digits is dropped ("SOD-323" and "SOD323",
// "TO-220" and "TO220", "SC-70-3" and "SC70-3" are one key). The catalogue writes these
// names both ways (1,119 diode records say "SOD323", 1,057 "SOT23", 173 MOSFETs "TO220")
// and the table held only the hyphenated spelling, so a same-package part resolved no
// dimensions at all and its footprint read 'unknown' with the unknown-dimension penalty
// (Qarlos 2026-11-09, seed 21517857: BZX384-A20 "SOD323" against GDZ20B-G "SOD-323").
// Only a LETTER-hyphen-DIGIT boundary is punctuation: "SOT-23-5" keeps its digit-digit
// hyphen, and "D-DPAK" (Infineon's double DPAK, not a D2PAK) keeps its letter-letter one.
inline std::string pkg_key(const std::string& s) {
    const std::string n = norm_pkg(s);
    std::string out;
    for (size_t i = 0; i < n.size(); ++i) {
        if (n[i] == '-' && i > 0 && i + 1 < n.size() &&
            std::isalpha(static_cast<unsigned char>(n[i - 1])) &&
            std::isdigit(static_cast<unsigned char>(n[i + 1])))
            continue;
        out += n[i];
    }
    return out;
}

inline std::optional<LWH> resolve_package(const std::string& code) {
    // Both tables re-keyed by pkg_key once, so the input and the table compare on the
    // same spelling.
    static const std::map<std::string, std::string> aliases = [] {
        std::map<std::string, std::string> t;
        for (const auto& [k, v] : package_aliases()) t[pkg_key(k)] = pkg_key(v);
        return t;
    }();
    static const std::map<std::string, LWH> table = [] {
        std::map<std::string, LWH> t;
        for (const auto& [k, v] : package_mm()) t[pkg_key(k)] = v;
        return t;
    }();
    std::string key = pkg_key(code);
    // A vendor-prepended NOISE word is tolerated ("PACKAGE-SOT-23" -> "SOT-23"), and only
    // that. The old rule accepted ANY prefix ending in a table name, so every Q-DPAK
    // (90 MOSFETs) resolved to the DPAK outline and the footprint note quoted DPAK's
    // 6.6 x 6.1 mm for a different, larger top-side-cooled package (Qarlos 2026-11-09, seed
    // 20994212, IPDQ65R040CFD7A); ThinDPAK, D-DPAK, DPAKDPAK, ITO-220 and TSOT-23-6 matched
    // the same way. A different prefix is a different outline, so it resolves nothing.
    for (const char* noise : {"PACKAGE-", "PKG-", "CASE-"}) {
        const std::string n = noise;
        if (key.size() > n.size() && key.compare(0, n.size(), n) == 0) {
            key = key.substr(n.size());
            break;
        }
    }
    auto alias = aliases.find(key);
    if (alias != aliases.end()) key = alias->second;
    auto it = table.find(key);
    if (it != table.end()) return it->second;
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

    auto from_table = [](const LWH& v) {
        Dims d = to_metres(v);
        d.height_from_table = d.height.has_value();
        d.land_from_table = true;
        return d;
    };

    // 1) Named packages (SOT/SOD/TO/DPAK/SOIC/...) and their aliases.
    if (auto pkg = resolve_package(raw)) return from_table(*pkg);

    // 2) Molded tantalum EIA letter (single A-D), capacitors only.
    if (cat == "capacitor") {
        auto it = tantalum_letter().find(upper(raw));
        if (it != tantalum_letter().end()) return from_table(it->second);
    }

    // 3) Aluminium electrolytic / any "DxL" can code -> diameter x diameter x length.
    //    A THREE-token "LxWxH" (an SMD vertical-chip can: "6.3x6.3x5.8", "8x8x11.7") is
    //    read first and as stated: the DxL pattern below matches its first two tokens and
    //    took the second WIDTH for the length, so a 5.8 mm tall can was compared as 6.3 mm
    //    and an 11.7 mm one as 8 mm, and the height verdicts were wrong both ways
    //    (ABT #2030).
    if (cat == "capacitor" || cat.empty()) {
        static const std::regex lwh(
            R"((?:^|[^0-9.])(\d{1,3}(?:\.\d{1,2})?)\s*[xX]\s*(\d{1,3}(?:\.\d{1,2})?)\s*[xX]\s*(\d{1,3}(?:\.\d{1,2})?)(?:[^0-9.]|$))");
        std::smatch m;
        if (std::regex_search(raw, m, lwh)) {
            const double l = std::stod(m[1].str()), w = std::stod(m[2].str()),
                         h = std::stod(m[3].str());
            if (l > 0 && w > 0 && h > 0 && l <= 100 && w <= 100 && h <= 100)
                return to_metres(LWH{l, w, h});
            return std::nullopt;  // three tokens that are not a body: no guess from two of them
        }
    }
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
            return from_table(v);
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
// penalised). Weights are Heaviside's: any candidate that fits outranks any
// candidate that doesn't, yet an oversize part stays finite-scored so it can
// still win when it is the only option.
//
// The two LAND axes decide the footprint and nothing else does. Height is a
// separate axis with a separate question (clearance under the enclosure, not
// which pads the part sits on) and is scored by height_fit / height_penalty
// below.
inline constexpr double kFitAreaWeight = 0.5;
inline constexpr double kOversizeBase = 10.0;
inline constexpr double kOversizeScale = 8.0;
inline constexpr double kUnknownDimPenalty = 2.0;
inline constexpr double kDimTolerance = 1.02;  // 2% slack for rounding/terminations
inline constexpr double kSlightlyOversizeOverflow = 0.65;  // ~one EIA size up
inline constexpr double kSlightlyOversizeBase = 1.0;
inline constexpr double kSlightlyOversizeScale = 2.5;
// "Case kept" band (strict_case, e.g. magnetics/chip beads). A substitute whose
// every LAND dimension is at least this fraction of the original's is the SAME
// footprint — a true drop-in that keeps the land pattern. Below it the pads no
// longer match, so it is a footprint CHANGE (FootprintTier::Smaller), not a fit.
inline constexpr double kUndersizeBand = 0.85;
inline constexpr double kUndersizeWeight = 2.0;  // ranks a same-size part above a smaller one

// True when BOTH land axes are on record: only then is the box a footprint that
// can be rotated and whose area is known.
inline bool has_land(const Dims& d) {
    return d.length && *d.length > 0 && d.width && *d.width > 0;
}

// True when at least ONE land axis is on record — the least a drawing can state
// and still say something about the land pattern. A drawing that states only a
// HEIGHT is a real drawing (its clearance is judged, see height_fit) but it is
// not a land: nothing about the pads can be compared to it, either way.
inline bool has_any_land(const Dims& d) {
    return (d.length && *d.length > 0) || (d.width && *d.width > 0);
}

// What two boxes can actually be compared on.
//
// When both records state both land axes the compare is orientation-agnostic —
// longest against longest — so a part that fits once rotated still fits. When
// either side states only ONE axis there is no orientation left to choose, so the
// axes are compared BY NAME: length against length, width against width. That is
// a comparison of the same physical axis, not a guess at the missing one.
//
// A partial compare is decisive in ONE direction only. A 3.2 mm body does not sit
// on a 2 mm land however the unstated axis went — turning it to make room puts the
// terminations across the pads, which is a different land pattern either way — and
// a body 40% short of the stated axis cannot reach the same pads. But two axes
// that match say nothing about the axis nobody stated. `fit_known` carries that
// asymmetry: false means "can refute a fit, cannot confirm one". Blanking the
// whole compare instead reported a 1206 body against an 0805 original as
// "footprint": null with no note (ABT #516).
struct LandCompare {
    bool comparable = false;  // the records share at least one land axis
    bool fit_known = false;   // ... and enough of them to CONFIRM a fit
    double overflow = 0.0;    // largest fraction the substitute exceeds by (0 = none)
    double lin = 1.0;         // smallest substitute/original axis ratio (undersize test)
    double area = 1.0;        // substitute/original land area; only meaningful if fit_known
};

inline LandCompare compare_land(const Dims& source, const Dims& candidate) {
    LandCompare r;
    if (has_land(source) && has_land(candidate)) {
        const double s_long = std::max(*source.length, *source.width);
        const double s_short = std::min(*source.length, *source.width);
        const double c_long = std::max(*candidate.length, *candidate.width);
        const double c_short = std::min(*candidate.length, *candidate.width);
        r.comparable = r.fit_known = true;
        r.overflow = std::max({c_long / s_long - 1.0, c_short / s_short - 1.0, 0.0});
        r.lin = std::min(c_long / s_long, c_short / s_short);
        r.area = (c_long * c_short) / (s_long * s_short);
        return r;
    }
    // Half-stated land: the like-named axes both records give, and nothing else.
    // `lin` stays capped at 1.0 so an axis that grew never reads as undersize.
    for (const std::optional<double> Dims::*axis : {&Dims::length, &Dims::width}) {
        const std::optional<double>& s = source.*axis;
        const std::optional<double>& c = candidate.*axis;
        if (!s || !c || *s <= 0 || *c <= 0) continue;
        const double ratio = *c / *s;
        r.comparable = true;
        r.overflow = std::max(r.overflow, ratio - 1.0);
        r.lin = std::min(r.lin, ratio);
    }
    return r;
}

// Categorise the fit of the LAND PATTERN. `strict_case` (magnetics/chip beads)
// additionally splits out a materially SMALLER body as its own tier — the pads
// won't match, so it is not a drop-in even though it physically fits inside the
// original's area. Height is NOT consulted: see height_fit.
enum class FootprintTier { Fits, Smaller, OneSizeLarger, Overflows, Unknown };

inline FootprintTier footprint_tier(const std::optional<Dims>& source,
                                    const std::optional<Dims>& candidate, bool strict_case = false) {
    if (!source || !candidate) return FootprintTier::Unknown;
    const LandCompare lc = compare_land(*source, *candidate);
    if (!lc.comparable) return FootprintTier::Unknown;
    if (lc.overflow > kDimTolerance - 1.0)
        return lc.overflow <= kSlightlyOversizeOverflow ? FootprintTier::OneSizeLarger
                                                        : FootprintTier::Overflows;
    if (strict_case && lc.lin < kUndersizeBand) return FootprintTier::Smaller;
    // The axes that matched cannot vouch for an axis nobody stated.
    return lc.fit_known ? FootprintTier::Fits : FootprintTier::Unknown;
}

inline double footprint_penalty(const std::optional<Dims>& source,
                                const std::optional<Dims>& candidate, bool strict_case = false) {
    if (!source) return 0.0;  // cannot enforce; surfaced separately as a diagnostic
    if (!candidate) return kUnknownDimPenalty;
    const LandCompare lc = compare_land(*source, *candidate);
    switch (footprint_tier(source, candidate, strict_case)) {
        case FootprintTier::Fits:
            // Historical right-sizing: a smaller part is mildly PREFERRED (it frees
            // board space). Kept for chip passives that sit on standard pads.
            if (!strict_case) return kFitAreaWeight * lc.area;
            [[fallthrough]];
        case FootprintTier::Smaller:
            // strict_case: the SAME footprint (case kept) is the true drop-in and must
            // rank first, so score 0 at equal size and PENALISE undersize in proportion
            // to how far under it is — a smaller body is a different land pattern, not a
            // free win.
            return kUndersizeWeight * std::max(0.0, 1.0 - lc.lin);
        case FootprintTier::OneSizeLarger:
            return kSlightlyOversizeBase + kSlightlyOversizeScale * lc.overflow;
        case FootprintTier::Overflows:
            return kOversizeBase + kOversizeScale * lc.overflow;
        case FootprintTier::Unknown:
            break;
    }
    return kUnknownDimPenalty;
}

inline const char* footprint_tier_name(FootprintTier t) {
    switch (t) {
        case FootprintTier::Fits: return "fits";
        case FootprintTier::Smaller: return "smaller";
        case FootprintTier::OneSizeLarger: return "one_size_larger";
        case FootprintTier::Overflows: return "overflows";
        case FootprintTier::Unknown: return "unknown";
    }
    return "unknown";
}

// ── Height / clearance: the OTHER axis, judged on its own ────────────────────
// Height used to ride in the same scalar as the two land dimensions, which made
// the tool say the opposite of the geometry (ABT #513): Würth 74439346100
// (6.36 x 6.56 x 6.0 mm) against Abracon ASPI-104S-100N-T (10.3 x 10.4 x 4.0 mm)
// came out "one_size_larger — about one case size larger, verify board fit". The
// height overshoot cleared `fits`, which skipped the undersize test that would
// have called the 39%-of-the-land body Smaller, and then supplied the only ratio
// the tier was named from. A part cannot be both 37% narrower and one size
// larger; and the real risk — +50% of height — was never mentioned.
//
// The two axes ask different questions: the land decides WHICH PADS the part sits
// on, the height decides whether it clears whatever is above the board. Neither
// answers for the other, so each is classified and reported separately.
enum class HeightFit { Fits, Taller, MuchTaller, Unknown };

// How much taller the substitute is, as a fraction of the original's height
// (0.5 = +50%). 0 when it is no taller; 0 when either side does not state one,
// which is Unknown, not "fits" — callers must test height_fit for that.
inline double height_overflow(const std::optional<Dims>& source,
                              const std::optional<Dims>& candidate) {
    if (!source || !candidate || !source->height || !candidate->height) return 0.0;
    if (*source->height <= 0 || *candidate->height <= 0) return 0.0;
    return std::max(*candidate->height / *source->height - 1.0, 0.0);
}

inline HeightFit height_fit(const std::optional<Dims>& source,
                            const std::optional<Dims>& candidate) {
    if (!source || !candidate || !source->height || !candidate->height) return HeightFit::Unknown;
    if (*source->height <= 0 || *candidate->height <= 0) return HeightFit::Unknown;
    if (*candidate->height <= *source->height * kDimTolerance) return HeightFit::Fits;
    return height_overflow(source, candidate) <= kSlightlyOversizeOverflow ? HeightFit::Taller
                                                                          : HeightFit::MuchTaller;
}

// Clearance penalty, on the same ladder the land oversize term uses so a taller
// part costs what it always cost — only now it ADDS to the land verdict instead
// of replacing it. A SHORTER part is not penalised: it keeps the pads and asks
// less of the enclosure.
inline double height_penalty(const std::optional<Dims>& source,
                             const std::optional<Dims>& candidate) {
    const HeightFit hf = height_fit(source, candidate);
    if (hf == HeightFit::Fits || hf == HeightFit::Unknown) return 0.0;
    const double overflow = height_overflow(source, candidate);
    return hf == HeightFit::Taller ? kSlightlyOversizeBase + kSlightlyOversizeScale * overflow
                                   : kOversizeBase + kOversizeScale * overflow;
}

inline const char* height_fit_name(HeightFit h) {
    switch (h) {
        case HeightFit::Fits: return "fits";
        case HeightFit::Taller: return "taller";
        case HeightFit::MuchTaller: return "much_taller";
        case HeightFit::Unknown: return "unknown";
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
        "SIP", "RADIAL", "AXIAL", "THRU-HOLE", "THROUGH", "THT", "SNAP-IN"};
    // "SCREW-TERMINAL" is not here: a screw-terminal can is its own mount class ("screw",
    // normalize_mount / mount_class), not a soldered leaded part.
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
// A connector's board attach (CONAS mechanical.mountingStyle) adds four classes of its own,
// each a different board process from SMD, soldered through-hole and one another: "pressfit"
// (a compliant pin pressed into a plated hole, no solder), "skedd" (a solderless direct
// plug-in into plated holes), "panel" (fastened to a panel, not to the board) and "cable"
// (a free-hanging cable-side part with no board attach at all). No PEAS assemblyType uses
// these words, so the other families classify exactly as before.
inline std::string normalize_mount(const std::string& assembly_type) {
    std::string a = assembly_type;
    for (auto& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (a == "smt" || a == "smd" || a == "surfacemount" || a == "surface_mount") return "smd";
    // "snap-in" is CAS shape.assembly's own enum value (THT / Screw Type / SMT / Snap-In /
    // Chassis Mount): a snap-in can's pins go through plated holes and are wave-soldered, so it
    // is a through-hole board process — the same class leaded_tokens() already gives the
    // "SNAP-IN" package string. Unclassified, a stated Snap-In mount was reported as "not
    // stated" (Qarlos 2026-11-09, 380LX151M450K022 / 475VXH330MEFCSN35X35).
    // "Screw Type" (CAS's enum value) is its own class, "screw": the part is fastened by
    // screw terminals — bolted to bus bars or tagged leads, the can clamped — which is neither
    // reflow on pads nor a soldered through-hole lead, so a screw-terminal part against a
    // radial-leaded or SMT one is an ESTABLISHED mount mismatch, not an unknown. KEMET
    // C315C300JDG5TA7303 (radial THT) was offered Vishay RA012020BB30036BF1 RF power tubes
    // (Screw Type), and Nichicon LNY2W472MSEH (Screw Type) radial cans, with the mount left
    // "not established" (Qarlos 2026-11-09, seeds 21098941 / 22146231). The CAS enum names
    // it outright, so nothing is inferred. "Chassis Mount" stays unclassified, like "chassis".
    if (a == "tht" || a == "th" || a == "throughhole" || a == "through_hole" || a == "radial" ||
        a == "axial" || a == "snap-in" || a == "snapin" || a == "snap_in")
        return "leaded";
    if (a == "screw type" || a == "screw-type" || a == "screw_type" || a == "screw" ||
        a == "screw terminal" || a == "screw-terminal")
        return "screw";
    if (a == "pressfit" || a == "press_fit" || a == "press-fit") return "pressfit";
    if (a == "skedd") return "skedd";
    if (a == "panel") return "panel";
    if (a == "cable") return "cable";
    return "";
}

// One part's mount class, "smd" / "leaded" / "" (unknown): its EXPLICIT assembly type when
// the record states one, else what its package/case string classifies as. Each side is
// classified on its own, so a record that states "tht" and one that states only a "1210"
// case code are still told apart (MF1/2CC3573F vs ERJS14F3573U, Qarlos 2026-11-08: the
// old pairwise rule fell back to the package strings of BOTH parts, found the axial part
// had none, and said nothing). A package string matching both token sets is unknown.
inline std::string mount_class(const std::string& mount, const std::string& package) {
    const std::string m = normalize_mount(mount);
    if (!m.empty()) return m;
    if (package.empty()) return "";
    if (dims_detail::upper(package).find("SCREW") != std::string::npos) return "screw";
    const bool smd = is_smd(package), leaded = is_leaded(package);
    if (smd == leaded) return "";
    return smd ? "smd" : "leaded";
}

// The shared mount comparison: both classes known and different -> incompatible (a board
// process change, not a substitution); either unknown -> not established. Order of
// evidence: both EXPLICIT assembly types; else both package strings (the pairwise rule this
// always had); else each side's own class (mount_class), which is what catches an explicit
// "tht" against a bare "1210".
enum class MountMatch { Same, Incompatible, Unknown };
inline MountMatch compare_mount(const std::string& o_mount, const std::string& s_mount,
                                const std::string& o_pkg, const std::string& s_pkg) {
    const std::string om = normalize_mount(o_mount), sm = normalize_mount(s_mount);
    if (!om.empty() && !sm.empty())
        return om == sm ? MountMatch::Same : MountMatch::Incompatible;
    const std::string op = mount_class("", o_pkg), sp = mount_class("", s_pkg);
    if (!op.empty() && !sp.empty())
        return op == sp ? MountMatch::Same : MountMatch::Incompatible;
    const std::string o = mount_class(o_mount, o_pkg), s = mount_class(s_mount, s_pkg);
    if (o.empty() || s.empty()) return MountMatch::Unknown;
    return o == s ? MountMatch::Same : MountMatch::Incompatible;
}

inline bool mount_incompatible(const std::string& o_mount, const std::string& s_mount,
                               const std::string& o_pkg, const std::string& s_pkg) {
    return compare_mount(o_mount, s_mount, o_pkg, s_pkg) == MountMatch::Incompatible;
}

// Normalise a case/package code for IDENTITY comparison ("case kept?"): keep only
// letters and digits, upper-cased. "QFN (8x8)" -> "QFN8X8", "SOT-23" -> "SOT23",
// "DO-214AB" -> "DO214AB". A conservative signal — genuinely different packages
// (different pinout/pads) compare unequal; the rare vendor alias (e.g. SMA vs
// DO-214AC) compares unequal too and is surfaced as "verify package", not hidden.
inline std::string normalize_case_code(const std::string& s) {
    std::string out;
    for (unsigned char c : s)
        if (std::isalnum(c)) out += static_cast<char>(std::toupper(c));
    return out;
}

// Diode package identity through an EXPLICIT alias table (Qarlos 2026-11-08). The common
// name and the JEDEC registration of one outline are the same land pattern, and the
// catalogue writes the pair every way — 'DO-214AB', 'SMC', 'SMC (DO-214AB)',
// 'DO-214AB (SMC)' (2,245 / 235 / 877 / 1,240 diode records) — so a 3KASMC36A came back
// "different package/case, different land pattern" against an SMLJ36CA it shares pads
// with. Each group below is one registered outline; a case string matches a group only
// when its normalised form EQUALS a member, or two different members written one after
// the other (the "NAME (REGISTRATION)" form, either order). No substring or fuzzy
// matching: 'SMC-2', 'SMB Flat', 'MELF (DO-213AB) glass' stay themselves and still
// compare unequal, as an unrecognised alias should ("verify package", never hidden).
// Only registrations that ARE the named outline are listed (JEDEC DO-214AA/AB/AC,
// DO-204AL/AC/AH, DO-213AB, DO-219AB/AC/AD, DO-220AA, DO-215AA/AB, DO-221AC/BC,
// TO-277A, TO-263AC, SOD-80).
// The shared matcher behind every package alias table: a group is one registered outline
// written as its normalised members; a case string matches a group only when its
// normalised form EQUALS a member, or two different members one after the other (the
// "NAME (REGISTRATION)" and "NAME / REGISTRATION" forms, either order). Anything else is
// returned as its plain normalised code. No substring, prefix or fuzzy match.
inline std::string canonical_case_in(const std::vector<std::vector<std::string>>& groups,
                                     const std::string& s) {
    const std::string n = normalize_case_code(s);
    for (const auto& g : groups) {
        for (const auto& a : g) {
            if (n == a) return g.front();
            for (const auto& b : g)
                if (&a != &b && n == a + b) return g.front();
        }
    }
    return n;
}

// MOSFET package identity through an EXPLICIT alias table (Qarlos 2026-11-09). One JEDEC
// outline under its industry name, its JEDEC registration, the 3-terminal suffix form and
// Infineon's PG- name is ONE land pattern, and the catalogue writes it every way: 'DPAK' 431,
// 'TO-252' 150, 'TO252 (DPAK)' 85, 'DPAK-3 / TO-252-3' 96, 'DPAK (TO-252)' 55 ... So TI's
// CSD20065D 'TO-252 (D-PAK)' against Infineon's IPD600N25N3 G 'DPAK' (seed 20300704), and
// Infineon's IPA90R500S7 'TO-220 FullPAK' against ST's STF8N90K5 'TO-220FP' (seed 21203670),
// came back "different package/case ... a substitute to re-lay-out". Each group is one
// outline; only names that ARE that outline are listed:
//   DPAK   = TO-252 / TO-252AA / DPAK-3 / TO-252-3 / PG-TO252-3 / D-PAK
//   D2PAK  = TO-263 / TO-263AB / D2PAK-3 / TO-263-3 / TO-263-2 / PG-TO263-3 (the 2-lead
//            and 3-lead D2PAK share the land: two leads plus the tab; the catalogue's own
//            'D2PAK-3 / TO-263-2' record states the equivalence)
//   IPAK   = TO-251 / TO-251AA;  I2PAK = TO-262 / TO-262-3
//   TO-220 = TO-220-3 / TO-220AB / PG-TO220-3
//   TO-220FP = TO-220 FullPAK = TO-220F = TO-220-3 FullPAK = TO-220FP-3 (the fully
//            isolated TO-220; Toshiba's TO-220SIS and ROHM's TO-220CFM are vendor outlines
//            of their own and are NOT folded in)
//   TO-247 = TO-247-3 / TO-247AC / TO-247AD / PG-TO247-3
//   SOT-23 = TO-236 / TO-236AB / SOT-23-3;  SOT-223 = TO-261 / TO-261AA / SOT-223-4 / TO-261-4
//   SO-8   = SOIC-8;  SC-70 = SOT-323;  TOLL = TO-LL / HSOF-8 / PG-HSOF-8
// Different outlines stay different: Q-DPAK, D2PAK-7 / D2PAK 7-pin / TO-263-7, TO-247-4,
// sTOLL, DPAK+, ThinDPAK, TO-220SIS each normalise to themselves.
inline std::string canonical_mosfet_case(const std::string& s) {
    static const std::vector<std::vector<std::string>> kGroups = {
        {"DPAK", "TO252", "TO252AA", "DPAK3", "TO2523", "PGTO2523"},
        {"D2PAK", "TO263", "TO263AB", "D2PAK3", "TO2633", "TO2632", "PGTO2633"},
        {"IPAK", "TO251", "TO251AA"},
        {"I2PAK", "TO262", "TO2623"},
        {"TO220", "TO2203", "TO220AB", "PGTO2203"},
        {"TO220FP", "TO220FULLPAK", "TO220F", "TO2203FULLPAK", "TO220FP3"},
        {"TO247", "TO2473", "TO247AC", "TO247AD", "PGTO2473"},
        {"SOT23", "TO236", "TO236AB", "SOT233"},
        {"SOT223", "TO261", "TO261AA", "SOT2234", "TO2614"},
        {"SO8", "SOIC8"},
        {"SC70", "SOT323"},
        {"TOLL", "TOLL8L", "HSOF8", "PGHSOF8"}};
    // A '+' names a different outline ("DPAK+", "D2PAK+"), but the alnum normaliser drops it,
    // which made DPAK+ compare equal to DPAK. Keep it: such a name matches no group and
    // compares equal only to the same '+' name.
    if (s.find('+') != std::string::npos) return normalize_case_code(s) + "+";
    return canonical_case_in(kGroups, s);
}

inline std::string canonical_diode_case(const std::string& s) {
    static const std::vector<std::vector<std::string>> kGroups = {
        {"DO214AC", "SMA"},      {"DO214AA", "SMB"},     {"DO214AB", "SMC"},
        {"DO204AL", "DO41"},     {"DO204AC", "DO15"},    {"DO204AH", "DO35"},
        {"DO213AB", "MELF"},     {"SOD80", "MINIMELF"},  {"DO219AB", "SMF"},
        {"DO219AC", "MICROSMF"}, {"DO219AD", "MICROSMP"}, {"DO220AA", "SMP"},
        {"DO215AA", "SMBG"},     {"DO215AB", "SMCG"},    {"DO221AC", "SLIMSMA"},
        {"DO221BC", "SMPA"},     {"TO277A", "SMPC"},     {"TO263AC", "SMPD"}};
    return canonical_case_in(kGroups, s);
}

// Chip-passive case identity with EXPLICITLY marked metric codes resolved to their imperial
// equivalent (Qarlos 2026-11-09, seed 21936773: CDR37BR331AJUMAC "0402" against Murata
// GRM155R71H331KA01 "1005M/0402" was called a different land pattern). The catalogue marks a
// metric code with an "M" suffix, alone ("1005M") or beside its imperial name ("1005M/0402",
// the Murata form: 0603M/0201 4,754 records, 1005M/0402 3,533, 1608M/0603 2,475, ...). Rule:
//   - "<metric>M/<imperial>" (either order) -> the imperial half, which the record states
//     outright; when the metric half is in the table below and maps to a DIFFERENT imperial
//     code the string contradicts itself and is left as written (never silently resolved);
//   - a lone "<metric>M" listed below -> its imperial equivalent;
//   - anything else, including every UNMARKED 4-digit code, is the plain normalised code: a
//     bare "0603" could be imperial 0603 or metric 0603 (imperial 0201), and which one it is
//     is never guessed.
// The table is the IEC 60062 / EIA-198 metric<->imperial chip-size pairs.
inline std::string canonical_chip_case(const std::string& s) {
    static const std::map<std::string, std::string> kMetricToImperial = {
        {"0201", "008004"}, {"0402", "01005"}, {"0603", "0201"}, {"1005", "0402"},
        {"1608", "0603"},   {"2012", "0805"},  {"3216", "1206"}, {"3225", "1210"},
        {"4516", "1806"},   {"4520", "1808"},  {"4532", "1812"}, {"5025", "2010"},
        {"5750", "2220"},   {"6332", "2512"},  {"2828", "1111"}, {"5728", "2211"},
        {"3212", "1205"},   {"0510", "0204"}};
    static const std::regex pair_mi(R"(^\s*(\d{4,5})M\s*/\s*(\d{4,6})\s*$)",
                                    std::regex::icase);
    static const std::regex pair_im(R"(^\s*(\d{4,6})\s*/\s*(\d{4,5})M\s*$)",
                                    std::regex::icase);
    static const std::regex lone_m(R"(^\s*(\d{4,5})M\s*$)", std::regex::icase);
    std::smatch m;
    std::string metric, imperial;
    if (std::regex_match(s, m, pair_mi)) {
        metric = m[1].str();
        imperial = m[2].str();
    } else if (std::regex_match(s, m, pair_im)) {
        imperial = m[1].str();
        metric = m[2].str();
    } else if (std::regex_match(s, m, lone_m)) {
        auto it = kMetricToImperial.find(m[1].str());
        return it != kMetricToImperial.end() ? it->second : normalize_case_code(s);
    } else {
        return normalize_case_code(s);
    }
    auto it = kMetricToImperial.find(metric);
    if (it != kMetricToImperial.end() && it->second != imperial) return normalize_case_code(s);
    return imperial;
}

// The identity a case-kept gate compares: the alias-resolved form for diodes and MOSFETs,
// the metric-resolved chip code for chip passives (capacitors, resistors), the plain
// normalised code everywhere else (an 'SMA' connector is an RF interface, not DO-214AC).
inline std::string case_identity(const std::string& category, const std::string& s) {
    if (category == "diode") return canonical_diode_case(s);
    if (category == "mosfet") return canonical_mosfet_case(s);
    if (category == "capacitor" || category == "resistor") return canonical_chip_case(s);
    return normalize_case_code(s);
}

// Wide-terminal ("reverse geometry", long-side termination) chip codes: the EIA imperial
// code read LLWW has its FIRST pair smaller than its second, because the terminations run
// along the long sides — 1225 is the 2512 outline turned 90 degrees, 0612 the 1206, 0508 the
// 0805, 1020 the 2010, 1218 the 1812, 0306 the 0603, 0406 the 0604. The body outline is the
// same box, so a rotation-agnostic size compare calls it a fit, but the pads are not: the
// land is two long strips instead of two short ones. The codes listed are the ones the
// resistor catalogue states (survey 2026-11-08: 1218 2,518; 0612 2,082; 1225 1,484; 1020
// 1,076; 0406 831; 0508 273; 0306 16). MELF size codes such as 0204/0207 are NOT chips and
// are deliberately absent.
inline bool wide_terminal_chip_code(const std::string& code) {
    static const char* kCodes[] = {"0306", "0406", "0508", "0612", "1020", "1218", "1225"};
    const std::string n = normalize_case_code(code);
    for (const char* c : kCodes)
        if (n == c) return true;
    return false;
}

// A standard EIA chip code both records state ("0603" and "0603") fixes ONE land pattern.
// The vendors' nominal drawings of that code still differ by a few percent (Vishay CRCW0603
// draws 1.55 x 0.85 mm, YAGEO AT0603 1.6 x 0.8 mm; the CRCW drawing's own tolerance runs to
// 1.65 mm), which the 2 % slack of the size compare read as "one case size larger" (Qarlos
// 2026-11-08). One EIA size step is 25 % or more in length, so a same-code body within this
// spread of the original's is the same land, not a larger one.
inline constexpr double kSameChipCodeBodySpread = 0.10;
inline bool same_standard_chip_code(const std::string& a, const std::string& b) {
    const std::string na = normalize_case_code(a), nb = normalize_case_code(b);
    if (na.empty() || na != nb) return false;
    return dims_detail::chip_imperial().count(na) > 0;
}

// Categories whose land pattern is the part's BODY, so an original with no body
// on record has no land pattern on record either. Connectors are the exception:
// their records carry no outline at all and their land IS the contact pitch,
// which is compared on its own axis and reported as the footprint verdict — for
// them "no dimensions" is not "no land datum" (ABT #485).
inline bool footprint_is_the_body(const std::string& category) {
    return category != "connector";
}

// Categories whose package strings vary too much by series for the generic mount
// rule to be safe (mirrors Heaviside's GFoot skip list).
inline bool mount_gate_applies(const std::string& category) {
    return !(category == "magnetic" || category == "chipBead" || category == "mosfet" ||
             category == "diode" || category == "igbt");
}

}  // namespace kelvin::crossref
