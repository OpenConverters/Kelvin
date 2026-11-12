#include "Views.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>
#include <stdexcept>
#include <unordered_map>

#include "CrossRefDimensions.hpp"  // crossref::normalize_mount (smd / leaded)
#include "DimensionJson.hpp"       // PEAS::resolve_dimensional_values (json overload)

namespace kelvin {
namespace {

// ---- small json readers mirroring Python's permissive dict access ----------
// nlohmann: a missing key or a null yields a null json; is_number() excludes booleans
// (Python's isinstance(x,(int,float)) would accept bool, but the numeric TAS fields never
// carry booleans, so this matches in practice).

const json* obj_get(const json& o, const char* key) {
    if (!o.is_object()) return nullptr;
    auto it = o.find(key);
    if (it == o.end() || it->is_null()) return nullptr;
    return &(*it);
}

std::optional<std::string> get_str(const json& o, const char* key) {
    const json* v = obj_get(o, key);
    if (v && v->is_string()) return v->get<std::string>();
    return std::nullopt;
}

std::optional<double> get_num(const json& o, const char* key) {
    const json* v = obj_get(o, key);
    if (v && v->is_number() && !v->is_boolean()) return v->get<double>();
    return std::nullopt;
}

bool get_bool(const json& o, const char* key, bool dflt) {
    const json* v = obj_get(o, key);
    if (v && v->is_boolean()) return v->get<bool>();
    return dflt;
}

// A dimensionWithTolerance-or-scalar field, collapsed to a scalar with the CANONICAL resolver
// (PEAS::resolve_dimensional_values) — never a hand-read of `.nominal`/`.maximum` (house rule).
// Semantics: number -> number; object -> preferred bound with the resolver's fallback chain
// (NOMINAL: nominal -> mid(min,max) -> max -> min). A field that is absent OR present-but-
// unresolvable returns nullopt, so the row is counted unreadable_row (the extractor's sanctioned
// permissive skip — accounted, not a silent default). For rows carrying a nominal this equals the
// old `.nominal` read, so parity holds; it differs only for a dimWithTol lacking a nominal, where
// the resolver's min/max fallback is the intended behaviour.
std::optional<double> resolve_field(const json& o, const char* key,
                                    PEAS::DimensionalValues pref = PEAS::DimensionalValues::NOMINAL) {
    const json* v = obj_get(o, key);
    if (!v) return std::nullopt;
    try {
        return PEAS::resolve_dimensional_values(*v, pref);
    } catch (const std::exception&) {
        return std::nullopt;  // present-but-unresolvable -> unreadable row (HS None semantics)
    }
}

bool pos(const std::optional<double>& x) { return x.has_value() && *x > 0.0; }

// TAS stores a P-channel MOSFET's or a PNP transistor's ratings signed as the datasheet
// prints them (V_DS = -30 V, I_D = -5 A); older rows carry the bare magnitude. The selector
// compares magnitudes, so a negative rating is read as its magnitude when the part says it
// is of the negative polarity. A negative rating on a part that says it is N-channel / NPN,
// or that names no polarity at all, contradicts itself: it stays unreadable (counted), never
// silently flipped — guessing which half of the record is wrong is not the indexer's call.
std::optional<double> rating_magnitude(std::optional<double> v, bool negative_polarity) {
    if (!v) return std::nullopt;
    if (*v < 0.0) return negative_polarity ? std::optional<double>(-*v) : std::nullopt;
    return v;
}

// A PEAS `curve` ({xData, yData, xLabel?, yLabel?}) read at x by log-log interpolation
// between the two samples that bracket it. nullopt when the curve is malformed, a label
// names a different unit than the caller expects, or x lies outside the sampled range —
// a value outside what was measured is not on record.
std::optional<double> curve_at(const json* c, double x, const char* x_unit, const char* y_unit) {
    if (!c || !c->is_object()) return std::nullopt;
    if (auto xl = get_str(*c, "xLabel"); xl && xl->find(x_unit) == std::string::npos)
        return std::nullopt;
    if (auto yl = get_str(*c, "yLabel");
        yl && (yl->find(y_unit) == std::string::npos || yl->find(std::string("m") + y_unit) != std::string::npos))
        return std::nullopt;
    const json* xs = obj_get(*c, "xData");
    const json* ys = obj_get(*c, "yData");
    if (!xs || !ys || !xs->is_array() || !ys->is_array() || xs->size() != ys->size())
        return std::nullopt;
    bool have_lo = false, have_hi = false;
    double xlo = 0, ylo = 0, xhi = 0, yhi = 0;
    for (size_t i = 0; i < xs->size(); ++i) {
        const json& xv = (*xs)[i];
        const json& yv = (*ys)[i];
        if (!xv.is_number() || !yv.is_number()) continue;
        const double xi = xv.get<double>(), yi = yv.get<double>();
        if (!(xi > 0) || !(yi > 0)) continue;
        if (xi == x) return yi;
        if (xi < x && (!have_lo || xi > xlo)) { xlo = xi; ylo = yi; have_lo = true; }
        if (xi > x && (!have_hi || xi < xhi)) { xhi = xi; yhi = yi; have_hi = true; }
    }
    if (!have_lo || !have_hi) return std::nullopt;
    const double t = (std::log(x) - std::log(xlo)) / (std::log(xhi) - std::log(xlo));
    return std::exp(std::log(ylo) + t * (std::log(yhi) - std::log(ylo)));
}

// Physical body size + case code, shared by every family extractor: the
// cross-reference footprint check needs a real size, because a substitute that
// does not fit the board is not a substitute. Dimensions are
// dimensionWithTolerance in TAS, so they go through the resolver rather than
// being hand-read. Absent stays NaN — the ranker then falls back to resolving
// the case code, and reports "unknown" if that fails too, never assuming a fit.
// `lies_flat`: the body's axis is PARALLEL to the board (an axial or MELF resistor), so a
// stated diameter is its width and its height and the length stays the length. Without it a
// diameter is read as an upright can, which turned a 9.53 mm-long PTF65 into a 3.68 x 3.68 mm
// footprint standing 9.53 mm tall (ABT #2032).
// `upright_disc`: the body is a disc standing on its leads (see the diameter fold below).
void fill_dimensions(RowBase& r, const json& di, const json& part, bool lies_flat = false,
                     bool upright_disc = false) {
    // Mount type first: `thickness` below needs it, because the same T label names a
    // different axis on a chip than on a leaded body. Three schemas spell the SAME fact
    // three ways: `mechanical.assemblyType` (semiconductors, magnetics),
    // `mechanical.shape.assembly` (CAS capacitors — THT / SMT / Snap-In, stated on every
    // record) and `mechanical.mountingStyle` (CONAS connectors). Reading only the first
    // left the mount unknown for the whole capacitor and connector catalogues, so the
    // mount gate never fired there even though the record said outright which one the
    // part is.
    const json* mech = obj_get(di, "mechanical");
    if (mech) {
        if (auto a = get_str(*mech, "assemblyType")) r.mount = *a;
        else if (auto a = get_str(*mech, "mountingStyle")) r.mount = *a;
        else if (const json* shape = obj_get(*mech, "shape")) {
            if (auto a = get_str(*shape, "assembly")) r.mount = *a;
        }
        // Some records nest the drawing one level deeper under `dimensions`.
        const json* nested = obj_get(*mech, "dimensions");
        const json& src = nested ? *nested : *mech;
        if (auto v = resolve_field(src, "length"); pos(v)) r.length_m = *v;
        if (auto v = resolve_field(src, "width"); pos(v)) r.width_m = *v;
        if (auto v = resolve_field(src, "height"); pos(v)) r.height_m = *v;
        std::optional<double> dia = resolve_field(src, "diameter");
        // Lead pitch of a leaded body: CAS states it as mechanical.dimensions.pitch (only
        // in the nested drawing — a connector's mechanical.pitch is its CONTACT pitch, its
        // own axis). Else the record's own drilled two-hole landPattern states it as the
        // distance between the two hole centres. Neither -> NaN, never a typical value.
        if (nested)
            if (auto v = resolve_field(*nested, "pitch"); pos(v)) r.lead_pitch_m = *v;
        if (!present(r.lead_pitch_m))
            if (const json* lp = obj_get(*mech, "landPattern"))
                if (const json* pads = obj_get(*lp, "pads"); pads && pads->is_array() &&
                                                             pads->size() == 2) {
                    const json& a = (*pads)[0];
                    const json& b = (*pads)[1];
                    auto ax = get_num(a, "x"), ay = get_num(a, "y"), bx = get_num(b, "x"),
                         by = get_num(b, "y");
                    if (a.contains("drill") && b.contains("drill") && ax && ay && bx && by) {
                        const double d = std::hypot(*bx - *ax, *by - *ay);
                        if (d > 0) r.lead_pitch_m = d;
                    }
                }
        if (nested) {  // a nested block may still leave siblings on `mechanical`
            if (!present(r.length_m))
                if (auto v = resolve_field(*mech, "length"); pos(v)) r.length_m = *v;
            if (!present(r.width_m))
                if (auto v = resolve_field(*mech, "width"); pos(v)) r.width_m = *v;
            if (!present(r.height_m))
                if (auto v = resolve_field(*mech, "height"); pos(v)) r.height_m = *v;
            if (!pos(dia)) dia = resolve_field(*mech, "diameter");
        }
        // A cylindrical body (an electrolytic can, an axial part) states a DIAMETER
        // instead of a footprint: the board area it occupies is diameter x diameter,
        // and the one axis left over is the can's length — which records name
        // `height` (Würth) or `length` (Vishay) for the very same axis. Fold it the
        // way the "DxL" case-code resolver already does (LWH{d, d, len}) so an
        // explicit drawing and a case code yield the same box instead of two
        // incomparable ones. Dropping the diameter left every can in the catalogue
        // with no seat at all, and the footprint check with nothing to compare.
        //
        // An UPRIGHT DISC (`upright_disc`: shapeType names a disc) is the exception to the
        // can fold: it stands on its leads with its face vertical and the leads in the
        // face's plane, so the diameter runs across the leads AND upward, and the thickness
        // is the land's depth. Board footprint D x T, height D. Datasheets checked
        // 2026-11-09: Vishay D Series (doc 28549, rev. 08-Jan-2026) p.2 "Lead
        // configuration": D drawn in the lead plane, T in the side view; KEMET C900
        // (C1065_X1_440_VAC_Y2_300_VAC, 10/25/22) p.3 "Lead Configurations": "D Body
        // Diameter" across the leads, "T Body Thickness" side view; TDK CK45 (e413_ck45,
        // July 2011) p.3/3 taping drawing: D in the lead plane, T in the side view. The
        // can fold read KEMET C901U100DZNDAA7317 (D 7, T 5 mm) as a 7 x 7 mm land with no
        // height and lost T. Only when the record states no length or width of its own (a
        // disc that does would be a drawing this rule has not seen); a stated height (the
        // seated height) is kept; an unstated thickness leaves the depth unknown, never D.
        const bool disc_fold = upright_disc && pos(dia) && !lies_flat &&
                               !present(r.length_m) && !present(r.width_m);
        if (disc_fold) {
            std::optional<double> disc_thk = resolve_field(src, "thickness");
            if (nested && !pos(disc_thk)) disc_thk = resolve_field(*mech, "thickness");
            r.length_m = *dia;
            if (pos(disc_thk)) r.width_m = *disc_thk;
            if (!present(r.height_m)) r.height_m = *dia;
        } else if (pos(dia) && lies_flat) {
            if (!present(r.length_m) && present(r.height_m)) r.length_m = r.height_m;
            r.width_m = *dia;
            r.height_m = *dia;
        } else if (pos(dia)) {
            const double axial = present(r.height_m) ? r.height_m : r.length_m;
            r.length_m = *dia;
            r.width_m = *dia;
            r.height_m = axial;
        } else {
            // `thickness` is the body's T dimension, and WHICH axis that is depends on how
            // the part mounts, so the mount decides the slot:
            //   chip-style SMT (MLCC, chip film) — CAS spells it out: "the dimension
            //     perpendicular to the mounting plane", i.e. the HEIGHT of an L x W x T body;
            //   leaded box (R73 / F5A radial film, radial-moulded tantalum) — the drawing's
            //     H is the height above the board and T is the body DEPTH, the second
            //     FOOTPRINT axis.
            // Reading it into neither dropped the third dimension of 33k capacitors:
            // R73TN2150SE00K states 26.5 x 17 x 8.5 mm and reached the ranker as a 26.5 mm
            // box of unknown width, so no footprint or body-width gate could fire on it
            // (ABT #480). Only ever fills a slot the record left empty, and never on a
            // cylindrical body — there the diameter fold above already defines the box and a
            // stated T is not one of its axes. An unclassified mount fills nothing: which
            // axis T is would be a guess, and an honest "unknown" beats a plausible number.
            std::optional<double> thk = resolve_field(src, "thickness");
            if (nested && !pos(thk)) thk = resolve_field(*mech, "thickness");
            if (pos(thk)) {
                const std::string mount = crossref::normalize_mount(r.mount);
                if (mount == "smd") {
                    if (!present(r.height_m)) r.height_m = *thk;
                } else if (mount == "leaded") {
                    if (!present(r.width_m)) {
                        r.width_m = *thk;
                    } else if (present(r.length_m) && !present(r.height_m) && lies_flat) {
                        // An AXIAL box drawn L x W x T (KEMET SRA / TRA): L is the body
                        // axis, lead exit to lead exit, parallel to the board; W and T
                        // are its cross-section, W across and T upward — KEMET C1119
                        // S-Series (7/2/2024) p.2, "Dimensions", axial end view: W drawn
                        // horizontal, T vertical; and CAS's own definition of thickness
                        // as the dimension perpendicular to the mounting plane. So the
                        // land is L x W and the height T.
                        r.height_m = *thk;
                    } else if (present(r.length_m) && !present(r.height_m)) {
                        // A RADIAL box drawn L x W x T with no H (TDK FA/FG/FK radial
                        // MLCCs: 4,620 records; KEMET 28, Murata 12, survey 2026-11-09).
                        // T is the body depth, a footprint axis, on every vendor's
                        // drawing; L and W span the face the leads leave from, so one is
                        // the height above the board and the other runs across the
                        // leads. WHICH one is a vendor convention, and the vendors
                        // disagree (datasheets checked 2026-11-09):
                        //   TDK FG catalogue (leadmlcc_halogenfree_fg_en, 20181102):
                        //     the type code reads "LxWxT"; FG26 = 5.5x6.0x3.5, and the
                        //     FG26 drawing (p.14) puts 5.5 across the leads, 6.0 upward
                        //     (FG28 p.6, FG18 p.23 the same): L across, W = height;
                        //   Murata RDER72E105MUB1H03B data sheet p.1 "Appearance &
                        //     Shape": "L max." across the leads, "W max." upward: L
                        //     across, W = height;
                        //   KEMET C1119 S-Series p.2 and C1116 V-Series p.2 radial
                        //     drawings: W across the leads, L upward: L = height.
                        // The record states neither the convention nor the height, and
                        // CAS defines length / width only as "Length / B" and "Width /
                        // A", so the orientation cannot be read from the data. Choosing
                        // by manufacturer name would be a guess wearing a datasheet's
                        // clothes; instead the box stays unfilled and the row says why,
                        // and the cross-reference reports the footprint UNVERIFIED with
                        // a note (CrossRef.hpp). The fix belongs in TAS: state the height
                        // above the board as `height` (and the across-leads axis as
                        // `length`), as the KEMET L x T x H radial records already do.
                        // (Before: T dropped, FG26C0G2A223JNT06 boxed on its 5.5 x 6.0 mm
                        // face; then 00f06be's guess of KEMET's convention for every
                        // vendor, which turned TDK's and Murata's boxes on their side.)
                        r.length_m = kNaN();
                        r.width_m = kNaN();
                        r.box_axes_ambiguous = true;
                    }
                }
            }
        }
    }
    // Case code: `part.case` is the field the catalogue actually populates
    // (100% of sampled capacitors/mosfets/resistors/diodes); `mechanical.case`
    // and the caseCode/package spellings are accepted as fallbacks.
    //
    // A PLACEHOLDER is not a case code (Qarlos 2026-11-09): 13 MOSFET, 84 diode and 624 IGBT
    // records state case "Unknown" (Infineon BSC160N15NS5ATMA1 ...). Read as a code, two
    // such records named the same "case" and the same-case rule called the land kept. Rule:
    // a placeholder reads as not stated, and the next source is consulted.
    auto stated_case = [](const std::optional<std::string>& c) -> std::optional<std::string> {
        if (!c) return std::nullopt;
        std::string t;
        for (char ch : *c)
            if (!std::isspace(static_cast<unsigned char>(ch)))
                t += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (t.empty() || t == "unknown" || t == "n/a" || t == "-" || t == "tbd" ||
            t == "none")
            return std::nullopt;
        return c;
    };
    if (auto c = stated_case(get_str(part, "case"))) r.case_code = *c;
    else if (mech) { if (auto c = stated_case(get_str(*mech, "case"))) r.case_code = *c; }
    if (r.case_code.empty()) {
        if (auto c = stated_case(get_str(part, "caseCode"))) r.case_code = *c;
        else if (auto p = stated_case(get_str(part, "package"))) r.case_code = *p;
    }
}

}  // namespace

bool datasheet_unusable(const std::string& url) {
    if (url.empty()) return true;
    static const std::regex re_example(R"(^https?://(www\.)?example\.com)",
                                       std::regex::icase);
    static const std::regex re_vishay(R"(vishay\.com/(en/)?search)", std::regex::icase);
    static const std::regex re_dspdf(R"(datasheetpdf\.com)", std::regex::icase);
    return std::regex_search(url, re_example) || std::regex_search(url, re_vishay) ||
           std::regex_search(url, re_dspdf);
}

std::string normalize_ctas_topology(const std::string& name) {
    static const std::unordered_map<std::string, std::string> kMap = {
        {"buckConverter", "buck"},
        {"boostConverter", "boost"},
        {"buckBoostConverter", "buck_boost"},
        {"flybackConverter", "flyback"},
        {"forwardConverter", "forward"},
        {"llcResonantConverter", "llc"},
        {"powerFactorCorrection", "power_factor_correction"},
        {"sepicConverter", "sepic"},
        {"cukConverter", "cuk"},
        {"zetaConverter", "zeta"},
        {"pushPullConverter", "push_pull"},
        {"phaseShiftedFullBridge", "phase_shifted_full_bridge"},
        {"dualActiveBridge", "dual_active_bridge"},
        {"cllcResonantConverter", "cllc"},
        {"clllcResonantConverter", "clllc"},
        {"seriesResonantConverter", "series_resonant"},
        {"viennaRectifierConverter", "vienna"},
    };
    auto it = kMap.find(name);
    if (it != kMap.end()) return it->second;
    std::string s = name;
    if (s.size() >= 9 && s.compare(s.size() - 9, 9, "Converter") == 0)
        s = s.substr(0, s.size() - 9);
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        char ch = s[i];
        if (ch >= 'A' && ch <= 'Z') {
            if (i > 0) out.push_back('_');
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::optional<MosfetRow> extract_mosfet(const json& env) {
    const json* semi = obj_get(env, "semiconductor");
    if (!semi) return std::nullopt;
    const json* m = obj_get(*semi, "mosfet");
    if (!m) return std::nullopt;
    const json* mi = obj_get(*m, "manufacturerInfo");
    if (!mi) return std::nullopt;
    const json* di = obj_get(*mi, "datasheetInfo");
    if (!di) return std::nullopt;
    const json* elec = obj_get(*di, "electrical");
    if (!elec) return std::nullopt;
    const json empty = json::object();
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;

    auto mpn = get_str(*mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    auto manuf = get_str(*mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    const std::string channel = get_str(*part, "subType").value_or("");
    const bool p_channel = channel == "pChannel";
    auto vds = rating_magnitude(get_num(*elec, "drainSourceVoltage"), p_channel);
    auto id = rating_magnitude(get_num(*elec, "continuousDrainCurrent"), p_channel);
    auto rds = get_num(*elec, "onResistance");
    if (!pos(vds) || !pos(id) || !pos(rds)) return std::nullopt;

    // Absent stays ABSENT (NaN in the row), never 0: see MosfetRow::qg_total. The selector's
    // Qg constraint is vacuous for a row without one, and the evidence tier counts it as
    // incomplete, exactly as the 0 did — but the row no longer claims 0 C of gate charge.
    auto qg = get_num(*elec, "totalGateCharge");
    if (qg && *qg < 0.0) return std::nullopt;

    // Leave ABSENT (NaN) when the record does not state one. `value_or(0.0)` wrote 0 F
    // output capacitance — physically impossible, and the same bits a stated 0 would
    // produce — into every row whose record has no `outputCapacitance` key, so the row
    // reported coss: 0 where every other absent field reports null (ABT #484). `pos`
    // treats a stated 0 as not-stated too, which is what it is: no MOSFET has zero Coss.
    auto coss_o = get_num(*elec, "outputCapacitance");
    if (coss_o && *coss_o < 0.0) return std::nullopt;

    // A P-channel threshold is a negative interval (-2.5 … -1 V): its worst case, the largest
    // magnitude, is the MINIMUM bound, negated. A legacy positive record keeps the old read.
    //
    // Only a STATED bound is a maximum. The resolver's MAXIMUM preference falls back to the
    // nominal and then to the minimum when the record has no maximum, which is right for a
    // value but wrong for a guaranteed limit: 1,350 catalogue MOSFETs state only a nominal
    // threshold and 668 only a minimum, and each one reached the ranker as a worst-case
    // Vgs(th)max — BSC028N08NS5's typical 2.3 V was reported as its maximum and drove a
    // gate-threshold warn the record does not support (ABT #2045). With no stated bound the
    // field stays ABSENT (NaN): it used to be 0, and the row then reported a 0 V worst-case
    // threshold for every nominal-only record (SQJ409EP, BSC020N04LS, IGO60R040D1 ...).
    //
    // P-channel bounds are read as MAGNITUDES, whichever key holds them (Qarlos 2026-11-09,
    // seed 21936773). The catalogue's P-channel records state the threshold four ways
    // (survey of mosfets.ndjson): {minimum, maximum} both negative, the minimum always the
    // larger magnitude (481, signed order); a lone negative minimum (656, e.g. -4 V); a lone
    // negative MAXIMUM (147: onsemi FDMA86265P -4 V, FQD5P20TM -5 V, the vendor's "Max"
    // column); a lone nominal (142). The old read took only the minimum, so every lone-maximum
    // record lost its threshold and a -4 V part read 'unverified' against a -2 V logic-level
    // ISP14EP15LM. Rule: the worst case is the LARGEST magnitude among the stated negative
    // minimum/maximum. A lone negative maximum is that magnitude in either reading of the
    // record: in the vendor-column reading it is |Vth|max; in the signed reading the
    // magnitude is at least that, so a compare that calls it higher than the original's is
    // right both ways. One exception, where the record says outright that its bound is the
    // SMALL end: a stated nominal with a larger magnitude than every stated bound (Vishay
    // SUD08P06-155L-GE3: maximum -1, nominal -2). Then no worst case is stated: absent.
    std::optional<double> vgs, vgs_typ;
    if (const json* gtv = obj_get(*elec, "gateThresholdVoltage"); gtv && gtv->is_object()) {
        auto hi = get_num(*gtv, "maximum"), lo = get_num(*gtv, "minimum"),
             nom = get_num(*gtv, "nominal");
        if (p_channel && ((lo && *lo < 0.0) || (hi && *hi < 0.0))) {
            double worst = 0.0;
            if (lo && *lo < 0.0) worst = std::max(worst, -*lo);
            if (hi && *hi < 0.0) worst = std::max(worst, -*hi);
            if (!(nom && *nom < 0.0 && -*nom > worst)) vgs = worst;
        } else if (pos(hi)) {
            vgs = hi;
        }
        // The typical, as a magnitude (see MosfetRow::vgs_threshold_typ). A P-channel
        // nominal is negative; an N-channel one must be positive to be read.
        if (nom && *nom != 0.0 && (p_channel || *nom > 0.0)) vgs_typ = std::fabs(*nom);
    }

    MosfetRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    fill_dimensions(r, *di, *part);
    r.vds_rated = *vds;
    r.id_continuous = *id;
    r.rds_on = *rds;
    if (pos(qg)) r.qg_total = *qg;
    if (pos(coss_o)) r.coss = *coss_o;
    if (vgs) r.vgs_threshold_max = *vgs;
    if (vgs_typ) r.vgs_threshold_typ = *vgs_typ;
    if (auto v = rating_magnitude(get_num(*elec, "onResistanceVgs"), p_channel); pos(v))
        r.rds_on_vgs = *v;
    if (auto v = rating_magnitude(get_num(*elec, "gateSourceVoltageMax"), p_channel); pos(v))
        r.vgs_max = *v;
    r.channel = channel;
    r.qualification = get_str(*part, "qualification").value_or("");
    r.technology = get_str(*part, "technology").value_or("");
    auto status = get_str(*mi, "status");
    r.is_production = status.has_value() && *status == "production";
    r.datasheet_usable = !datasheet_unusable(get_str(*mi, "datasheetUrl").value_or(""));
    const json* thermal = obj_get(*di, "thermal");
    if (thermal) {
        auto rja = get_num(*thermal, "thermalResistanceJunctionAmbient");
        if (rja && *rja > 0) r.rth_ja = *rja;
        auto rjc = get_num(*thermal, "thermalResistanceJunctionCase");
        if (rjc && *rjc > 0) r.rth_jc = *rjc;
        auto tj = get_num(*thermal, "junctionTemperatureMax");
        if (tj) r.tj_max = *tj;
    }
    return r;
}

std::optional<DiodeRow> extract_diode(const json& env) {
    const json* semi = obj_get(env, "semiconductor");
    if (!semi) return std::nullopt;
    const json* d = obj_get(*semi, "diode");
    if (!d) return std::nullopt;
    const json* mi = obj_get(*d, "manufacturerInfo");
    if (!mi) return std::nullopt;
    const json* di = obj_get(*mi, "datasheetInfo");
    if (!di) return std::nullopt;
    const json* elec = obj_get(*di, "electrical");
    if (!elec) return std::nullopt;
    const json empty = json::object();
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;

    auto mpn = get_str(*mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    auto manuf = get_str(*mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    // Diode subtypes are characterised by DIFFERENT parameters, and demanding the
    // rectifier set from all of them silently emptied the catalogue of whole families:
    // 8,173 of 8,274 zeners, 2,025 of 2,025 TVS and 720 of 720 ESD parts were dropped
    // as "unreadable", so kelvin.openconverters.com offered no transient-suppression or
    // ESD-protection parts at all (ABT #423). Measured over TAS diodes.ndjson:
    //   zener    carries breakdownVoltage (its Vz); only 101/8,274 carry reverseVoltage
    //   tvs/esd  carry standoffVoltage; NEITHER carries forwardCurrent at all
    // So take each subtype's own reverse-direction rating, and require forward
    // parameters only where the subtype actually has them.
    const std::string sub = get_str(*part, "subType").value_or("");
    const bool rectifier_like = (sub != "zener" && sub != "tvs" && sub != "esd");

    const bool protection = (sub == "tvs" || sub == "esd");
    // A protection diode's working rating is its STANDOFF voltage, and only that. Reading
    // reverseVoltage first took the BREAKDOWN maximum some importers wrote there: P6KE180A
    // (standoff 180 V) reached the ranker as a 221 V part and passed a 210 V original's
    // check while already conducting at 200 V (ABT #2047). Every tvs/esd record states a
    // standoff; one that does not is unreadable, not read from another field.
    //
    // A zener's working rating is its Vz, the breakdownVoltage, and only that — the same
    // rule. resolve_field, NOT get_num: a zener's breakdownVoltage is a
    // dimensionWithTolerance ({minimum, nominal, maximum} — Vz is a graded range, so all
    // of them are objects), and get_num reads none of it. Reading reverseVoltage first
    // (72 of 13,881 zeners state one) took the reverse-LEAKAGE test voltage instead:
    // BZX55C3.3 (Vz 3.135-3.465 V) reached the ranker as a 4.95 V part. Now that the
    // ranker compares a zener's Vz to the original's window, that would reject the
    // genuine equivalent and admit a wrong one (Qarlos 2026-11-08). A zener stating no
    // breakdown voltage is unreadable, not read from another field.
    std::optional<double> vrrm = protection       ? resolve_field(*elec, "standoffVoltage")
                                 : sub == "zener" ? resolve_field(*elec, "breakdownVoltage")
                                                  : get_num(*elec, "reverseVoltage");
    auto if_avg = get_num(*elec, "forwardCurrent");
    auto vf = get_num(*elec, "forwardVoltage");
    if (!pos(vrrm)) return std::nullopt;
    if (rectifier_like && (!pos(if_avg) || !pos(vf))) return std::nullopt;

    // Leave both ABSENT (NaN) when the record does not state them. The `: 0.0` this
    // replaces wrote a zero for a MISSING key, so a record carrying no recovery data at
    // all — 15,165 of the 17,888 of them — reported an ideal diode, zero recovery charge
    // and zero recovery time, in the row the user is shown, while every other absent
    // field on that same row read null (ABT #489).
    //
    // NOT `pos`, which is what the capacitor ESR and the MOSFET Coss use: a stated 0 is
    // real here. 260 records state Qrr = 0 and 257 of them are sicSchottky (the other 3
    // schottky) — a majority-carrier device stores no minority charge, and the physics
    // validator flags a NON-zero Qrr on one as suspicious. So keep what the record says,
    // including a zero, and let only silence be silence. A negative is unreadable, not a
    // value, and stays absent.
    auto qrr_o = get_num(*elec, "reverseRecoveryCharge");
    auto trr_o = get_num(*elec, "reverseRecoveryTime");

    DiodeRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    fill_dimensions(r, *di, *part);
    r.vrrm_rated = *vrrm;
    // NaN, not 0: a zener publishes no average forward current, and "0 A" is a
    // plausible-looking number where NaN is unmistakably absent. Same convention the
    // thermal fields in this struct already use.
    r.if_avg_rated = pos(if_avg) ? *if_avg : kNaN();
    r.vf_typ = pos(vf) ? *vf : kNaN();
    if (qrr_o && *qrr_o >= 0) r.qrr = *qrr_o;
    if (trr_o && *trr_o >= 0) r.trr = *trr_o;
    // The GUARANTEED breakdown window, alongside the nominal resolve_field just took.
    // The nominal is what the part is marked; the window is what it does — an A-grade
    // zener and the B grade of the same nominal share vrrm_rated and are different parts
    // (ABT #488). Both bounds are required for a tolerance: half a band is not one.
    if (const json* bv = obj_get(*elec, "breakdownVoltage"); bv && bv->is_object()) {
        auto lo = get_num(*bv, "minimum"), hi = get_num(*bv, "maximum");
        if (pos(lo)) r.vz_min = *lo;
        if (pos(hi)) r.vz_max = *hi;
        // Half-width over midpoint, not over the nominal: 28 records state a band with no
        // nominal to divide by, 253 state a band the nominal does not sit in the middle
        // of, and the width of the window is what the two bounds themselves say. A record
        // whose bounds are inverted states no window we can read.
        if (pos(lo) && pos(hi) && *hi >= *lo) r.vz_tolerance = (*hi - *lo) / (*hi + *lo);
    }
    r.technology = get_str(*part, "subType").value_or("");
    // The die configuration the record STATES, verbatim; absent stays "" (unknown to the
    // ranker), never filled from the part number or the package.
    if (auto v = get_str(*part, "dieConfiguration")) r.die_configuration = *v;
    if (auto v = get_str(*part, "internalConnection")) r.internal_connection = *v;
    // Continuous dissipation rating. Absent stays NaN; a 0 or negative is unreadable.
    if (auto v = get_num(*elec, "powerDissipation"); pos(v)) r.power_dissipation = *v;
    // The surge / clamp / capacitance specs, each with the impulse it was stated at
    // (ABT #2036, #2040, #2041, #2048-#2051). Absent stays NaN / "", never a default.
    if (auto v = get_num(*elec, "clampingVoltage"); pos(v)) r.clamping_voltage = *v;
    if (auto v = get_num(*elec, "clampingCurrent"); pos(v)) r.clamping_current = *v;
    if (auto v = get_num(*elec, "peakPulsePower"); pos(v)) r.peak_pulse_power = *v;
    if (auto v = get_num(*elec, "peakPulseCurrent"); pos(v)) r.peak_pulse_current = *v;
    if (auto v = get_num(*elec, "junctionCapacitance"); pos(v)) r.junction_capacitance = *v;
    // Its bias basis (v21): 0 V is a real basis, so not `pos`; a negative is unreadable.
    if (auto v = get_num(*elec, "junctionCapacitanceVr"); v && *v >= 0.0)
        r.junction_capacitance_vr = *v;
    r.clamping_waveform = get_str(*elec, "clampingWaveform").value_or("");
    r.peak_pulse_power_waveform = get_str(*elec, "peakPulsePowerWaveform").value_or("");
    r.peak_pulse_current_waveform = get_str(*elec, "peakPulseCurrentWaveform").value_or("");
    // Verbatim; "" = not stated (the ranker reads that as an UNKNOWN grade, never commercial).
    r.qualification = get_str(*part, "qualification").value_or("");
    auto status = get_str(*mi, "status");
    r.is_production = status.has_value() && *status == "production";
    const json* thermal = obj_get(*di, "thermal");
    if (thermal) {
        auto rja = get_num(*thermal, "thermalResistanceJunctionAmbient");
        if (rja && *rja > 0) r.rth_ja = *rja;
        auto rjc = get_num(*thermal, "thermalResistanceJunctionCase");
        if (rjc && *rjc > 0) r.rth_jc = *rjc;
        auto tj = get_num(*thermal, "junctionTemperatureMax");
        if (tj) r.tj_max = *tj;
    }
    return r;
}

// Every record nests its manufacturerInfo one or two levels down ({"capacitor": {…}},
// {"semiconductor": {"diode": {…}}}); find it without knowing the family.
static const json* find_manufacturer_info(const json& o, int depth) {
    if (!o.is_object() || depth > 2) return nullptr;
    if (const json* mi = obj_get(o, "manufacturerInfo")) return mi;
    for (const auto& [k, v] : o.items())
        if (const json* mi = find_manufacturer_info(v, depth + 1)) return mi;
    return nullptr;
}

bool all_provenance_retracted(const json& env) {
    const json* mi = find_manufacturer_info(env, 0);
    const json* di = mi ? obj_get(*mi, "datasheetInfo") : nullptr;
    const json* pv = di ? obj_get(*di, "provenance") : nullptr;
    if (!pv || !pv->is_array() || pv->empty()) return false;
    for (const json& e : *pv) {
        const json* r = e.is_object() ? obj_get(e, "retracted") : nullptr;
        if (!r || !r->is_boolean() || !r->get<bool>()) return false;
    }
    return true;
}

std::optional<CapacitorRow> extract_capacitor(const json& env) {
    const json* cap = obj_get(env, "capacitor");
    if (!cap) return std::nullopt;
    const json* mi = obj_get(*cap, "manufacturerInfo");
    if (!mi) return std::nullopt;
    const json* di = obj_get(*mi, "datasheetInfo");
    if (!di) return std::nullopt;
    const json* elec = obj_get(*di, "electrical");
    if (!elec) return std::nullopt;
    const json empty = json::object();
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;

    auto mpn = get_str(*mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    auto manuf = get_str(*mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    auto cap_nom = resolve_field(*elec, "capacitance");
    // A line-safety (X/Y) or AC film capacitor may be rated for AC only: CAS accepts
    // voltageRatedAcMax in place of ratedVoltage. Such a part has no DC rating, so v_rated
    // stays ABSENT (NaN) and the DC selector refuses it under its own reason — but it is a
    // real part to browse and cross-reference, not an unreadable row.
    auto v_rated = get_num(*elec, "ratedVoltage");
    auto v_rated_ac = get_num(*elec, "voltageRatedAcMax");
    if (!pos(cap_nom) || !(pos(v_rated) || pos(v_rated_ac))) return std::nullopt;

    // Leave both ABSENT (NaN) when the record does not state them. `>= 0` used to accept
    // a stated 0 as real and substitute 0.0 for a missing one, which are the two ways to
    // land on the same impossible value from opposite causes. `pos` treats a stated 0 as
    // not-stated too, which is what it is: no capacitor has zero ESR.
    auto ripple_o = get_num(*elec, "rippleCurrent");
    auto esr_o = get_num(*elec, "esr");
    // ESL lives in the equivalent circuit, not the electrical block: CAS
    // models a capacitor as rs/cs/ls/riso and `ls` IS the series inductance.
    std::optional<double> esl_o;
    if (auto mp = di->find("modelParams"); mp != di->end() && mp->is_object())
        esl_o = get_num(*mp, "ls");

    CapacitorRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    // An AXIAL body lies on the board with its axis parallel to it: CAS states its body as
    // dimensions.length (the axis, lead exit to lead exit) and dimensions.diameter, so the
    // diameter is its width and its height and the length stays the length — the resistor rule
    // (ABT #2032). Read as an upright can, A50IQ4150AA60K (28 mm long, 10 mm diameter) became
    // a 10 x 10 mm footprint standing 28 mm tall and C114K222K1X5CA lost its 4.06 mm length
    // (Qarlos 2026-11-09, seeds 21413128 / 21622586). CAS has no axial flag; the record says
    // it in mechanical.shape.shapeType ("Axial", "Axial Leaded", "Axial Hermetic", "Axial
    // Cylindrical", "Axial-lead", ...), so a shapeType naming "axial" is the evidence. Any
    // other shape keeps the upright-can fold.
    // A shapeType naming "disc" ("Disc", "Radial Disc") is the same kind of evidence for an
    // upright disc: footprint diameter x thickness, height the diameter (fill_dimensions).
    bool axial = false, disc = false;
    if (const json* mech = obj_get(*di, "mechanical"))
        if (const json* shape = obj_get(*mech, "shape"))
            if (auto st = get_str(*shape, "shapeType")) {
                std::string l = *st;
                for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                axial = l.find("axial") != std::string::npos;
                disc = !axial && l.find("disc") != std::string::npos;
            }
    fill_dimensions(r, *di, *part, /*lies_flat=*/axial, /*upright_disc=*/disc);
    r.capacitance = *cap_nom;
    if (pos(v_rated)) r.v_rated = *v_rated;
    if (pos(v_rated_ac)) r.v_rated_ac = *v_rated_ac;
    if (pos(ripple_o)) r.ripple_current_rms = *ripple_o;
    if (pos(esr_o)) r.esr = *esr_o;
    if (pos(esl_o)) r.esl = *esl_o;
    // Technology is `part.technology` — a real comparable class
    // ("ceramic-class-2", "aluminum-electrolytic-wet", "film-polypropylene"),
    // present on every record. The family/subType/series chain below is only a
    // fallback: those hold a VENDOR SERIES ("GRM1885"), which is not a
    // technology, is never equal across manufacturers, and made both the
    // catalogue facet and the cross-reference same-technology filter useless.
    std::string tech = get_str(*part, "technology").value_or("");
    if (tech.empty()) tech = get_str(*part, "family").value_or("");
    if (tech.empty()) tech = get_str(*part, "subType").value_or("");
    if (tech.empty()) tech = get_str(*part, "series").value_or("");
    // Dielectric code (X7R / C0G / X5R …) drives the class-equivalence gate in
    // the cross-reference ranker: swapping C0G for X5R is a real regression.
    r.dielectric_code = get_str(*part, "dielectricCode").value_or("");
    // The series name, kept verbatim — it is where the line-safety approval lives
    // (X1/X2/X3, Y1/Y2/Y3/Y4) for a mains capacitor, and nothing else in the record
    // states one. WIMA writes it in both slots ("MKP-X1 R"), KEMET into `family`
    // ("R47 X1 440 VAC"), so family first with series as the fallback — the same
    // chain the resistor device class uses.
    r.family = get_str(*mi, "family").value_or("");
    if (r.family.empty()) r.family = get_str(*part, "series").value_or("");
    if (auto v = get_num(*elec, "esrFrequency"); pos(v)) r.esr_frequency = *v;
    // The frequency the scalar ripple rating is stated AT (v20). Only alongside a ripple
    // value: a frequency with nothing quoted at it is not a basis for anything. Never
    // inferred from the series, the technology or a "typical" 100/120 Hz.
    if (present(r.ripple_current_rms))
        if (auto v = get_num(*elec, "rippleCurrentFrequency"); pos(v))
            r.ripple_current_frequency = *v;
    // The IEC 60384-14 safety class the record states (v20), verbatim. Absent or null stays
    // "" (not stated); the explicit token "none" is a stated fact and is kept as such.
    if (const json* sc = obj_get(*part, "safetyClass"); sc && sc->is_object())
        r.safety_class = get_str(*sc, "class").value_or("");
    // The guaranteed capacitance band (v20), in farads as stated. Kept only when the record
    // states BOTH ends and they bracket the nominal with a non-zero width: a lone end, or a
    // "band" of zero width, is not a tolerance statement, and nothing is inferred from the
    // MPN tolerance letter or a typical band.
    if (const json* cj = obj_get(*elec, "capacitance"); cj && cj->is_object()) {
        auto lo = get_num(*cj, "minimum"), hi = get_num(*cj, "maximum");
        if (lo && hi && *lo > 0 && *lo <= r.capacitance && *hi >= r.capacitance && *hi > *lo) {
            r.capacitance_min = *lo;
            r.capacitance_max = *hi;
        }
    }
    // The leaded form (v20), from the shapeType words the vendors write ("Axial", "Axial
    // Leaded", "Axial Hermetic", "Radial", "Radial Boxed", "Radial Disc", "Radial Molded",
    // "Resin dipped & radial leaded type", ...). A shapeType naming neither ("Box", "SMD
    // Chip", "Single-ended") leaves it "" — never guessed from the series or the technology.
    if (const json* mech = obj_get(*di, "mechanical"))
        if (const json* shape = obj_get(*mech, "shape"))
            if (auto st = get_str(*shape, "shapeType")) {
                std::string l = *st;
                for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const bool ax = l.find("axial") != std::string::npos;
                const bool rad = l.find("radial") != std::string::npos;
                if (ax != rad) r.lead_form = ax ? "axial" : "radial";
            }
    // The qualified reliability level (v20, PEAS qualifiedReliability on part), verbatim.
    if (const json* qr = obj_get(*part, "qualifiedReliability"); qr && qr->is_object()) {
        r.qualified_spec = get_str(*qr, "specification").value_or("");
        if (!r.qualified_spec.empty()) {
            r.qualified_grade = get_str(*qr, "grade").value_or("");
            if (auto fr = get_num(*qr, "failureRate"); fr && *fr >= 0)
                r.qualified_failure_rate = *fr;
        }
    }
    // No scalar ESR, but a measured ESR-vs-frequency curve: read it AT 100 kHz, the
    // frequency the catalogue's scalar ESRs are overwhelmingly quoted at, and say so in
    // esr_frequency. TDK publishes the curve and no scalar, so every TDK candidate reached
    // the ranker with "no ESR" while its record carried 325 measured points (ABT #2031).
    // Only an INTERPOLATION between two samples that bracket 100 kHz is taken — never an
    // extrapolation — and only from a curve whose labels, when present, say Hz and ohm.
    if (!present(r.esr)) {
        if (auto v = curve_at(obj_get(*elec, "esrPoints"), 1e5, "[Hz]", "Ohm"); v) {
            r.esr = *v;
            r.esr_frequency = 1e5;
        }
    }
    // Operating temperature range: a substitute must cover the original's, at
    // both ends. CAS carries it under two keys: its own `thermal.temperature` and the PEAS
    // datasheetInfoThermal `thermal.operatingTemperature`, both {minimum, maximum}. Of the
    // 604,716 published records (2026-11-08), 476,195 state a thermal block: 466,726 use
    // only `temperature`, 9,391 only `operatingTemperature`, 78 both (all 78 agree).
    // Reading only `temperature` dropped the -40..105 C range of EEEHC0J221XP, so 85 C
    // substitutes got no temperature verdict (Qarlos 2026-11-08). Each end is read from whichever key states it; when both
    // state an end and they DISAGREE, that end is left unknown — picking one would be a
    // silent choice between two contradicting datasheet claims.
    if (const json* th = obj_get(*di, "thermal")) {
        const json* a = obj_get(*th, "temperature");
        const json* b = obj_get(*th, "operatingTemperature");
        auto end = [&](const char* k) -> std::optional<double> {
            std::optional<double> va = a ? get_num(*a, k) : std::nullopt;
            std::optional<double> vb = b ? get_num(*b, k) : std::nullopt;
            if (va && vb) return (*va == *vb) ? va : std::nullopt;
            return va ? va : vb;
        };
        if (auto v = end("minimum")) r.temp_min_c = *v;
        if (auto v = end("maximum")) r.temp_max_c = *v;
    }
    r.technology = tech;
    auto status = get_str(*mi, "status");
    r.is_production = status.has_value() && *status == "production";
    auto rth = get_num(*elec, "thermalResistance");
    if (rth && *rth > 0) r.rth = *rth;
    return r;
}

std::optional<ResistorRow> extract_resistor(const json& env) {
    const json* res = obj_get(env, "resistor");
    if (!res) return std::nullopt;
    const json* mi = obj_get(*res, "manufacturerInfo");
    if (!mi) return std::nullopt;
    const json* di = obj_get(*mi, "datasheetInfo");
    if (!di) return std::nullopt;
    const json* elec = obj_get(*di, "electrical");
    if (!elec) return std::nullopt;
    const json empty = json::object();
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;

    // mpn = reference or part.partNumber; manufacturer optional (defaults "")
    auto mpn = get_str(*mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    if (!mpn) return std::nullopt;

    // Zero is a RESISTANCE, not a missing one. A 0 ohm link is one of the most
    // common parts on a real board — Yageo's RC series alone ships them in
    // every case size — and rejecting it here deleted 107 of them from the
    // catalogue while their records sat in the source data, so a board using
    // one was told its part did not exist (ABT #1123).
    //
    // Absent is still rejected, by the !r_nom above: the field is either there
    // or the record is refused. So a stored row's resistance == 0 always means
    // a real zero and never "we did not know" — which matters, because the row
    // struct's own default is also 0 and callers cannot tell the two apart.
    auto r_nom = resolve_field(*elec, "resistance");
    if (!r_nom || *r_nom < 0) return std::nullopt;

    // Tolerance exactly as stated. Absent (or a non-positive number, which is no tolerance
    // band at all) stays NaN = not on record: the cross-reference grades it UNVERIFIED and
    // the selector cannot show it meets a tolerance limit. It used to become a "typical"
    // +/-5 %, which let an unstated part pass as a 5 % one (Qarlos 2026-11-08 follow-up).
    auto tol_o = get_num(*elec, "tolerance");
    double tol = (tol_o && *tol_o > 0) ? *tol_o : kNaN();
    auto pw_o = get_num(*elec, "powerRating");
    double pw = (pw_o && *pw_o > 0) ? *pw_o : 0.0;

    ResistorRow r;
    r.mpn = *mpn;
    r.manufacturer = get_str(*mi, "name").value_or("");
    // A resistor that states a diameter is an axial or MELF body, which lies on the board.
    fill_dimensions(r, *di, *part, /*lies_flat=*/true);
    r.resistance = *r_nom;
    r.tolerance = tol;
    r.power_rating = pw;
    // The device class travels with the row. Panasonic writes it into both slots
    // ("Chip Resistor Array"), YAGEO into `family` alone ("YC" / "AF_Array"), KOA and
    // Stackpole into `series` alone — so read family first and fall back to series.
    r.family = get_str(*mi, "family").value_or("");
    if (r.family.empty()) r.family = get_str(*part, "series").value_or("");
    auto status = get_str(*mi, "status");
    r.is_production = status.has_value() && *status == "production";
    // The temperature basis of the power rating (v18). A rating is the power the part carries
    // up to a stated ambient; above it the part derates along its curve. Both are carried
    // exactly as stated — absent stays NaN / empty, never a "typical" 70 degC knee.
    if (auto t = get_num(*elec, "powerRatingTemperature")) r.power_rating_temperature = *t;
    if (const json* factors = obj_get(*di, "factors")) {
        if (const json* pd = obj_get(*factors, "powerDerating")) {
            const json* ts = obj_get(*pd, "temperature");
            const json* as = obj_get(*pd, "amplitude");
            // A derating curve the ranker cannot read is a defect in the record, and reading
            // half of it would put a wrong power in front of an engineer. Loud, with the part.
            auto bad = [&](const char* why) {
                return std::runtime_error("resistor " + *mpn + ": factors.powerDerating " + why);
            };
            if (!ts || !as || !ts->is_array() || !as->is_array())
                throw bad("must carry temperature[] and amplitude[]");
            if (ts->size() != as->size() || ts->size() < 2)
                throw bad("temperature[] and amplitude[] must be equal-length with >= 2 points");
            for (size_t i = 0; i < ts->size(); ++i) {
                const json& tv = (*ts)[i];
                const json& av = (*as)[i];
                if (!tv.is_number() || tv.is_boolean() || !av.is_number() || av.is_boolean())
                    throw bad("holds a non-numeric point");
                const double ti = tv.get<double>(), ai = av.get<double>();
                if (!r.derating_temperature.empty() && !(ti > r.derating_temperature.back()))
                    throw bad("temperatures must strictly increase");
                if (!(ai >= 0.0 && ai <= 1.0)) throw bad("amplitude must lie in [0, 1]");
                r.derating_temperature.push_back(ti);
                r.derating_amplitude.push_back(ai);
            }
        }
    }
    // v19: the resistive element, its TCR, its operating range and its stated function — the
    // properties a parametric match (R, P, tolerance) cannot see and the cross-reference now
    // compares (Qarlos 2026-11-08). Survey of resistors.ndjson (578,915 records, 2026-11-08):
    // part.technology on every record, temperatureCoefficient on 520,842, thermal.
    // operatingTemperature on 465,147 (its only temperature key in RAS).
    r.technology = get_str(*part, "technology").value_or("");
    if (auto t = get_num(*elec, "temperatureCoefficient")) r.tcr = *t;
    if (const json* th = obj_get(*di, "thermal")) {
        if (const json* t = obj_get(*th, "operatingTemperature")) {
            // Straight from the object: a cold limit is negative and 0 degC is a real rating.
            if (auto v = get_num(*t, "minimum")) r.temp_min_c = *v;
            if (auto v = get_num(*t, "maximum")) r.temp_max_c = *v;
        }
    }
    std::vector<std::string> class_texts;
    for (auto s : {get_str(*mi, "family"), get_str(*part, "series"), get_str(*part, "description")})
        if (s) class_texts.push_back(*s);
    const ResistorFunction fn = resistor_function_class(r.technology, class_texts);
    r.current_sense = fn.current_sense;
    r.pulse_withstanding = fn.pulse_withstanding;
    // v21: the maximum continuous working voltage, as stated. Absent / non-positive stays NaN.
    if (auto v = get_num(*elec, "maxVoltage"); pos(v)) r.max_voltage = *v;
    return r;
}

// Survey of resistors.ndjson (2026-11-08) for the class text the vendors write: current
// sensing is stated as technology currentSenseShunt (10,016 records) or "Current Sensing
// Resistor(s)" in family/series/description (Panasonic ERJ-xBQ/ERJ-xDQ series, TE CGS RL73);
// pulse capability as "Anti-Surge ..." (Panasonic ERJ-P/ERJ-PA/ERJ-T, 13,063), "Anti-Pulse
// ..." (581) and "Anti Pulse Chip Resistors" (KOA SG73, 2,627 descriptions). Matching is on
// those phrases only, case- and hyphen-insensitive; a word like "surge" on its own is not
// taken as a claim.
ResistorFunction resistor_function_class(const std::string& technology,
                                         const std::vector<std::string>& class_texts) {
    ResistorFunction f;
    if (technology == "currentSenseShunt") f.current_sense = true;
    static const char* kSense[] = {"current sensing", "current sense"};
    static const char* kPulse[] = {"anti surge",      "antisurge",       "anti pulse",
                                   "antipulse",       "pulse withstand", "surge withstand",
                                   "pulse resistant", "surge resistant", "pulse proof"};
    for (const std::string& raw : class_texts) {
        std::string t;
        t.reserve(raw.size());
        for (unsigned char c : raw)
            t += (c == '-' || c == '_') ? ' ' : static_cast<char>(std::tolower(c));
        for (const char* k : kSense)
            if (t.find(k) != std::string::npos) f.current_sense = true;
        for (const char* k : kPulse)
            if (t.find(k) != std::string::npos) f.pulse_withstanding = true;
    }
    return f;
}

std::optional<ControllerRow> extract_controller(const json& env) {
    // ctrl = env["controller"] if it's an object, else env itself.
    const json* ctrl = obj_get(env, "controller");
    if (!ctrl) ctrl = &env;
    const json empty = json::object();
    const json* mi = obj_get(*ctrl, "manufacturerInfo");
    if (!mi) mi = &empty;
    const json* ds = obj_get(*mi, "datasheetInfo");
    if (!ds) ds = &empty;
    const json* fn = obj_get(*ds, "function");
    if (!fn) fn = &empty;
    const json* part = obj_get(*ds, "part");
    if (!part) part = &empty;

    auto mpn = get_str(*mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    auto manuf = get_str(*mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    ControllerRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    fill_dimensions(r, *ds, *part);
    r.category = get_str(*fn, "category").value_or("");
    const json* topos = obj_get(*fn, "intendedTopologies");
    if (topos && topos->is_array()) {
        for (const auto& t : *topos)
            if (t.is_string()) r.topologies.push_back(normalize_ctas_topology(t.get<std::string>()));
    }
    const json* el = obj_get(*ds, "electrical");
    if (el) {
        const json* vref_blk = obj_get(*el, "referenceVoltage");
        if (vref_blk) {
            auto vr = get_num(*vref_blk, "nominal");
            if (vr && *vr > 0) r.vref = *vr;
        }
    }
    r.integrated_fet = get_bool(*fn, "integratedFet", false);
    r.integrated_driver = (r.category == "gateDriver") || get_bool(*fn, "integratedDriver", false);
    return r;
}

// ---- Phase 5 extractors ----------------------------------------------------
namespace {
// Shared semiconductor-envelope descent to (electrical, part, manufacturerInfo) for igbt/bjt.
struct SemiRefs {
    const json* mi;
    const json* elec;
    const json* part;
};
std::optional<SemiRefs> semi_refs(const json& env, const char* kind, const json& empty) {
    const json* semi = obj_get(env, "semiconductor");
    if (!semi) return std::nullopt;
    const json* node = obj_get(*semi, kind);
    if (!node) return std::nullopt;
    const json* mi = obj_get(*node, "manufacturerInfo");
    if (!mi) return std::nullopt;
    const json* di = obj_get(*mi, "datasheetInfo");
    if (!di) return std::nullopt;
    const json* elec = obj_get(*di, "electrical");
    if (!elec) return std::nullopt;
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;
    return SemiRefs{mi, elec, part};
}
}  // namespace

std::optional<IgbtRow> extract_igbt(const json& env) {
    const json empty = json::object();
    auto refs = semi_refs(env, "igbt", empty);
    if (!refs) return std::nullopt;
    auto mpn = get_str(*refs->mi, "reference");
    if (!mpn) mpn = get_str(*refs->part, "partNumber");
    auto manuf = get_str(*refs->mi, "name");
    if (!mpn || !manuf) return std::nullopt;
    auto vces = get_num(*refs->elec, "collectorEmitterVoltage");
    auto ic = get_num(*refs->elec, "continuousCollectorCurrent");
    if (!pos(vces) || !pos(ic)) return std::nullopt;
    IgbtRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    const json* dinfo_dims = obj_get(*refs->mi, "datasheetInfo");
    if (dinfo_dims) fill_dimensions(r, *dinfo_dims, *refs->part);
    r.vces_rated = *vces;
    r.ic_continuous = *ic;
    r.vce_sat = get_num(*refs->elec, "collectorEmitterSaturation").value_or(0.0);
    r.technology = get_str(*refs->part, "technology").value_or("");
    auto st = get_str(*refs->mi, "status");
    r.is_production = st && *st == "production";
    const json* dinfo = obj_get(*refs->mi, "datasheetInfo");
    const json* thermal = dinfo ? obj_get(*dinfo, "thermal") : nullptr;
    if (thermal) {
        auto tj = get_num(*thermal, "junctionTemperatureMax");
        if (tj) r.tj_max = *tj;
        auto rjc = get_num(*thermal, "thermalResistanceJunctionCase");
        if (rjc && *rjc > 0) r.rth_jc = *rjc;
    }
    return r;
}

std::optional<BjtRow> extract_bjt(const json& env) {
    const json empty = json::object();
    auto refs = semi_refs(env, "bjt", empty);
    if (!refs) return std::nullopt;
    auto mpn = get_str(*refs->mi, "reference");
    if (!mpn) mpn = get_str(*refs->part, "partNumber");
    auto manuf = get_str(*refs->mi, "name");
    if (!mpn || !manuf) return std::nullopt;
    const std::string polarity = get_str(*refs->part, "subType").value_or("");
    const bool pnp = polarity == "pnp";
    auto vceo = rating_magnitude(get_num(*refs->elec, "collectorEmitterVoltage"), pnp);
    auto ic = rating_magnitude(get_num(*refs->elec, "collectorCurrent"), pnp);
    if (!pos(vceo) || !pos(ic)) return std::nullopt;
    BjtRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    const json* dinfo_dims = obj_get(*refs->mi, "datasheetInfo");
    if (dinfo_dims) fill_dimensions(r, *dinfo_dims, *refs->part);
    r.vceo_rated = *vceo;
    r.ic_continuous = *ic;
    // dcCurrentGain: {minimum,maximum} or scalar -> the guaranteed MINIMUM gain.
    r.hfe_min = resolve_field(*refs->elec, "dcCurrentGain", PEAS::DimensionalValues::MINIMUM)
                    .value_or(0.0);
    r.power_dissipation = get_num(*refs->elec, "powerDissipation").value_or(0.0);
    r.technology = get_str(*refs->part, "technology").value_or("");
    r.polarity = polarity;
    auto st = get_str(*refs->mi, "status");
    r.is_production = st && *st == "production";
    return r;
}

std::optional<VaristorRow> extract_varistor(const json& env) {
    const json empty = json::object();
    const json* v = obj_get(env, "varistor");
    if (!v) return std::nullopt;
    const json* mi = obj_get(*v, "manufacturerInfo");
    if (!mi) return std::nullopt;
    const json* di = obj_get(*mi, "datasheetInfo");
    if (!di) return std::nullopt;
    const json* elec = obj_get(*di, "electrical");
    if (!elec) return std::nullopt;
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;
    auto mpn = get_str(*mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    auto manuf = get_str(*mi, "name");
    if (!mpn || !manuf) return std::nullopt;
    auto vnom = resolve_field(*elec, "varistorVoltage");
    if (!pos(vnom)) return std::nullopt;
    VaristorRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    fill_dimensions(r, *di, *part);
    r.varistor_voltage = *vnom;
    r.clamping_voltage = get_num(*elec, "clampingVoltage").value_or(0.0);
    r.peak_surge_current = get_num(*elec, "peakSurgeCurrent").value_or(0.0);
    r.max_continuous_dc_voltage = get_num(*elec, "maxContinuousDcVoltage").value_or(0.0);
    r.capacitance = get_num(*elec, "capacitance").value_or(0.0);
    r.technology = get_str(*part, "technology").value_or("");
    auto st = get_str(*mi, "status");
    r.is_production = st && *st == "production";
    return r;
}

// ---- Magnetic extractor ----------------------------------------------------
namespace {
// A single representative saturation current (A) for one electrical entry:
//   1. saturationCurrentPeak scalar (the datasheet headline), else
//   2. the saturationCurrents [{percentInductanceDrop,current}] table — the current at the point
//      nearest the 20 % inductance-drop industry convention (falls back to the largest current when
//      no point states a basis). Returns NaN when neither is present.
double magnetic_saturation_current(const json& elec, double& percent_drop) {
    percent_drop = kNaN();
    const json* tbl = obj_get(elec, "saturationCurrents");
    auto peak = get_num(elec, "saturationCurrentPeak");
    if (peak && *peak > 0) {
        // The scalar's basis is the table entry that states the same current, if any —
        // a scalar with no matching entry is of unknown basis and stays NaN.
        if (tbl && tbl->is_array())
            for (const auto& p : *tbl) {
                if (!p.is_object()) continue;
                auto cur = get_num(p, "current");
                auto pd = get_num(p, "percentInductanceDrop");
                if (cur && pd && *pd >= 0 && std::fabs(*cur - *peak) <= 1e-9 * *peak) {
                    percent_drop = *pd;
                    break;
                }
            }
        return *peak;
    }
    if (tbl && tbl->is_array()) {
        const json* best = nullptr;
        double best_dist = 0.0, max_cur = kNaN();
        for (const auto& p : *tbl) {
            if (!p.is_object()) continue;
            auto cur = get_num(p, "current");
            if (!cur || *cur <= 0) continue;
            if (!present(max_cur) || *cur > max_cur) max_cur = *cur;
            auto pd = get_num(p, "percentInductanceDrop");
            if (pd) {
                double d = std::fabs(*pd - 20.0);
                if (!best || d < best_dist) { best = &p; best_dist = d; }
            }
        }
        if (best) {
            percent_drop = get_num(*best, "percentInductanceDrop").value_or(kNaN());
            return get_num(*best, "current").value_or(kNaN());
        }
        return max_cur;  // no stated basis -> the largest quoted current
    }
    return kNaN();
}

// The record's whole saturation table (MagneticRow.sat_table_*, v20): every saturationCurrents
// entry with a positive current, in record order, carrying its criterion and its temperature
// when stated (NaN otherwise); then the saturationCurrentPeak scalar as a criterion-unknown
// point when no entry states that same current (MAS: the scalar is the datasheet's single
// unqualified Isat). Nothing is interpolated, merged or dropped for being "redundant": the
// ranker decides what is comparable.
void magnetic_saturation_table(const json& elec, MagneticRow& r) {
    const json* tbl = obj_get(elec, "saturationCurrents");
    if (tbl && tbl->is_array())
        for (const auto& p : *tbl) {
            if (!p.is_object()) continue;
            auto cur = get_num(p, "current");
            if (!cur || !(*cur > 0)) continue;
            auto pd = get_num(p, "percentInductanceDrop");
            auto t = get_num(p, "temperature");
            r.sat_table_current.push_back(*cur);
            r.sat_table_percent_drop.push_back(pd && *pd >= 0 ? *pd : kNaN());
            r.sat_table_temperature.push_back(t ? *t : kNaN());
        }
    auto peak = get_num(elec, "saturationCurrentPeak");
    if (peak && *peak > 0) {
        bool listed = false;
        for (double c : r.sat_table_current)
            if (std::fabs(c - *peak) <= 1e-9 * *peak) listed = true;
        if (!listed) {
            r.sat_table_current.push_back(*peak);
            r.sat_table_percent_drop.push_back(kNaN());
            r.sat_table_temperature.push_back(kNaN());
        }
    }
}

// Saturation currents READ OFF the record's own inductance-vs-DC-bias curve (MAS
// electrical[].inductancePoints, "the inductance roll-off / saturation curve"), for a record
// that states no saturationCurrents table and no saturationCurrentPeak. Coilcraft's Power
// Inductor Finder curves, among others, reached the catalogue only this way (2,102 inductor
// records, survey of magnetics.ndjson 2026-11-09), and the ranker never saw them: 1812PS-153KRC,
// whose curve is already 38 % down at 0.98 A, was graded 'upgrade' for a 1.2 A original, and
// 1008PS-103KRC (30 % down at 1.06 A) 'equivalent' for a 7.5 A one (Qarlos 2026-11-09, seeds
// 21832044 / 21727315). Rule:
//   * the reference is the curve's OWN zero-bias point (MAS: the drop is measured "from the
//     small-signal value"); a curve with no point at 0 A states no reference and yields nothing;
//   * Isat at a criterion is the FIRST current at which the curve falls to (1 - drop) x L(0),
//     linearly interpolated between the two measured points that bracket the crossing — never
//     extrapolated past the last measured point, so a curve that never reaches a criterion
//     states no Isat at it;
//   * the criteria read are the ones vendors quote, 10 / 20 / 30 % (the span the ranker's isat
//     rules already use), each tagged with its percent drop so the ranker compares like for like;
//   * DECISION (owner to confirm): a curve given at several temperatures is read at ONE of
//     them, the stated temperature nearest 25 degC (the room-temperature basis datasheet Isat
//     figures are quoted at; the hotter one on a tie), never interpolated between temperatures;
//     an untemperatured curve is read only when no temperature is stated at all.
// Reading a stated curve is not a model: no saturation physics, no default, no typical value.
void magnetic_saturation_from_bias_curve(const json& elec, MagneticRow& r) {
    if (!r.sat_table_current.empty()) return;  // the record states its Isat outright
    const json* pts = obj_get(elec, "inductancePoints");
    if (!pts || !pts->is_array()) return;
    struct BiasPoint { double current, inductance; };
    struct Curve { bool has_temp; double temp; std::vector<BiasPoint> pts; };
    std::vector<Curve> curves;
    for (const auto& p : *pts) {
        if (!p.is_object()) continue;
        auto cur = get_num(p, "current");
        auto L = get_num(p, "inductance");
        if (!cur || !L || *cur < 0 || !(*L > 0)) continue;
        auto t = get_num(p, "temperature");
        Curve* c = nullptr;
        for (auto& k : curves)
            if (k.has_temp == t.has_value() && (!t || k.temp == *t)) c = &k;
        if (!c) {
            curves.push_back({t.has_value(), t ? *t : kNaN(), {}});
            c = &curves.back();
        }
        c->pts.push_back({*cur, *L});
    }
    const Curve* pick = nullptr;
    for (const auto& c : curves) {
        if (!c.has_temp) continue;
        if (!pick) { pick = &c; continue; }
        const double dc = std::fabs(c.temp - 25.0), dp = std::fabs(pick->temp - 25.0);
        if (dc < dp || (dc == dp && c.temp > pick->temp)) pick = &c;
    }
    if (!pick)
        for (const auto& c : curves)
            if (!c.has_temp) pick = &c;
    if (!pick) return;
    std::vector<BiasPoint> curve = pick->pts;
    std::stable_sort(curve.begin(), curve.end(),
                     [](const BiasPoint& a, const BiasPoint& b) { return a.current < b.current; });
    if (curve.size() < 2 || curve.front().current != 0.0) return;  // no small-signal reference
    const double L0 = curve.front().inductance;
    for (const double drop : {10.0, 20.0, 30.0}) {
        const double threshold = L0 * (1.0 - drop / 100.0);
        for (size_t k = 1; k < curve.size(); ++k) {
            const BiasPoint& a = curve[k - 1];
            const BiasPoint& b = curve[k];
            if (!(b.inductance <= threshold)) continue;
            if (!(a.inductance > threshold) || !(b.current > a.current)) break;  // undecidable
            const double frac = (a.inductance - threshold) / (a.inductance - b.inductance);
            r.sat_table_current.push_back(a.current + frac * (b.current - a.current));
            r.sat_table_percent_drop.push_back(drop);
            r.sat_table_temperature.push_back(pick->has_temp ? pick->temp : kNaN());
            break;
        }
    }
    // The headline scalar follows the table rule of magnetic_saturation_current: the point
    // nearest the 20 % convention.
    for (size_t i = 0; i < r.sat_table_current.size(); ++i)
        if (!present(r.saturation_current) ||
            std::fabs(r.sat_table_percent_drop[i] - 20.0) <
                std::fabs(r.saturation_percent_drop - 20.0)) {
            r.saturation_current = r.sat_table_current[i];
            r.saturation_percent_drop = r.sat_table_percent_drop[i];
        }
}
}  // namespace

std::optional<MagneticRow> extract_magnetic(const json& env) {
    const json* mag = obj_get(env, "magnetic");
    if (!mag) return std::nullopt;
    const json* mi = obj_get(*mag, "manufacturerInfo");
    if (!mi) return std::nullopt;
    const json* di = obj_get(*mi, "datasheetInfo");
    if (!di) return std::nullopt;
    const json empty = json::object();
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;

    // Identity: reference (or part.partNumber) + manufacturer name. As with every family, a row
    // without an identity is unreadable (skipped, counted) — that is not a spec gate.
    auto mpn = get_str(*mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    auto manuf = get_str(*mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    MagneticRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    // MAS states the board attach as mechanical.mounting (the PEAS connectionType enum: smt /
    // tht / ...), a key fill_dimensions does not read: 9,083 magnetic records state it (survey
    // 2026-11-09), and a TDK B78108S1562K000 ('tht', axial) was graded minor_review for an SMT
    // original with no mount note (Qarlos 2026-11-09, seed 22146231). Set before
    // fill_dimensions, which needs the mount, and which an explicit assemblyType still
    // overrides (no record states both).
    if (const json* mech = obj_get(*di, "mechanical"))
        if (auto m = get_str(*mech, "mounting")) r.mount = *m;
    fill_dimensions(r, *di, *part);

    // electrical is an ARRAY; the first entry is the primary-winding projection we rank on.
    const json* elec_arr = obj_get(*di, "electrical");
    if (elec_arr && elec_arr->is_array() && !elec_arr->empty() && elec_arr->front().is_object()) {
        const json& elec = elec_arr->front();
        auto L = resolve_field(elec, "inductance");
        if (L && *L > 0) r.inductance = *L;
        r.saturation_current = magnetic_saturation_current(elec, r.saturation_percent_drop);
        magnetic_saturation_table(elec, r);
        magnetic_saturation_from_bias_curve(elec, r);
        const json* rc = obj_get(elec, "ratedCurrents");
        if (rc && rc->is_array()) {
            double best = kNaN();
            for (const auto& v : *rc)
                if (v.is_number() && !v.is_boolean()) {
                    double x = v.get<double>();
                    if (x > 0 && (!present(best) || x > best)) best = x;
                }
            r.rated_current = best;
        }
        // MAS ratedCurrentPoints: the rated current qualified by its temperature-rise
        // criterion, which the schema PREFERS over the bare ratedCurrents array and tells
        // vendors to state instead of it. 4,724 magnetic records state only this form (survey
        // 2026-11-09), so their rating was dropped: 0805CS-820XGLB (0.4 A at 15 K) left every
        // candidate's rated current unweighed (Qarlos 2026-11-09, seed 22041502). Read when the
        // bare array states nothing, with the same rule as that array: the largest stated
        // current. The temperature-rise basis is not yet carried (no row field), exactly as the
        // bare array carries none.
        if (!present(r.rated_current))
            if (const json* rcp = obj_get(elec, "ratedCurrentPoints"); rcp && rcp->is_array()) {
                double best = kNaN();
                for (const auto& p : *rcp) {
                    if (!p.is_object()) continue;
                    auto x = get_num(p, "current");
                    if (x && *x > 0 && (!present(best) || *x > best)) best = *x;
                }
                r.rated_current = best;
            }
        // DCR: prefer the worst-case (maximum) bound; fall back to the plural dcResistances[0].
        auto dcr = resolve_field(elec, "dcResistance", PEAS::DimensionalValues::MAXIMUM);
        if (!dcr) {
            const json* dcrs = obj_get(elec, "dcResistances");
            if (dcrs && dcrs->is_array() && !dcrs->empty()) {
                try {
                    dcr = PEAS::resolve_dimensional_values(dcrs->front(),
                                                           PEAS::DimensionalValues::MAXIMUM);
                } catch (const std::exception&) {
                }
            }
        }
        if (dcr && *dcr > 0) r.dcr = *dcr;
        auto srf = get_num(elec, "selfResonantFrequency");
        if (srf && *srf > 0) r.srf = *srf;
        // Chip-bead impedance curve. A bead's published spec is |Z| at 100 MHz,
        // and TDK's own engineer calls that spot value "irrelevant and
        // misleading" on its own: parts with identical Z@100MHz routinely peak
        // at very different heights and frequencies, which is what actually
        // decides whether the bead suppresses the noise you have. Where the
        // catalogue carries the measured curve we derive the peak and its
        // frequency — neither is a field in any vendor's datasheet table.
        if (const json* pts = obj_get(elec, "impedancePoints");
            pts && pts->is_array() && !pts->empty()) {
            // `closest` tracks a DISTANCE, and its best possible value is
            // zero — so zero cannot also mean "nothing seen yet". It did, and
            // the collision cost exactly the parts this field exists for: a
            // bead sampled AT 100 MHz set closest = 0, which still read as
            // unset, so the next point overwrote it and the 5 MHz proximity
            // test then failed. Every bead measured at the conventional test
            // point — which is most of them — lost its headline figure.
            double best_z = 0.0, best_f = 0.0, at_100m = 0.0, closest = 0.0;
            bool have_closest = false;
            for (const auto& pt : *pts) {
                if (!pt.is_object()) continue;
                auto f = get_num(pt, "frequency");
                const json* z = obj_get(pt, "impedance");
                if (!f || !z) continue;
                auto mag = get_num(*z, "magnitude");
                if (!mag || *mag <= 0 || *f <= 0) continue;
                if (*mag > best_z) { best_z = *mag; best_f = *f; }
                // nearest sample to the conventional 100 MHz test point
                const double d = std::fabs(*f - 1e8);
                if (!have_closest || d < closest) {
                    closest = d; at_100m = *mag; have_closest = true;
                }
            }
            if (best_z > 0) { r.impedance_peak = best_z; r.impedance_peak_freq = best_f; }
            // Only claim a 100 MHz value when a sample is actually near it —
            // extrapolating from a distant point would invent a datum.
            if (have_closest && at_100m > 0 && closest <= 5e6)
                r.impedance_100mhz = at_100m;
        }
        // Transformer turns ratio (primary:secondary): electrical[0].turnsRatios[0] (a dimWithTol). Lets
        // select_magnetic rank a catalog transformer by how close its ratio is to the design's required
        // ratio — the meaningful match for flyback/isolated transformers (TAS has no per-winding structure).
        const json* trs = obj_get(elec, "turnsRatios");
        if (trs && trs->is_array() && !trs->empty()) {
            try {
                double tr = PEAS::resolve_dimensional_values(trs->front());
                if (tr > 0) r.turns_ratio = tr;
            } catch (const std::exception&) {
            }
        }
        r.device_type = get_str(elec, "subtype").value_or("");

        // Winding structure across EVERY wiring configuration (a transformer with an auxiliary winding
        // files both "all windings" and "auxiliary windings open"): the set of secondary-winding counts
        // the part can realize. Only stated structure counts — a multi-winding entry without turnsRatios
        // adds nothing.
        for (const auto& e : *elec_arr) {
            if (!e.is_object()) continue;
            const std::string st = get_str(e, "subtype").value_or("");
            if (st == "inductor" || st == "chipBead") {
                r.secondary_counts |= 1u;
            } else if (st == "transformer" || st == "coupledInductor") {
                const json* ratios = obj_get(e, "turnsRatios");
                if (ratios && ratios->is_array() && !ratios->empty() && ratios->size() < 32)
                    r.secondary_counts |= (1u << ratios->size());
            }
        }
    }

    std::string fam = get_str(*mi, "family").value_or("");
    if (fam.empty()) fam = get_str(*part, "family").value_or("");
    r.family = fam;
    // Operating-temperature range (v20): PEAS datasheetInfoThermal thermal.operatingTemperature,
    // the only temperature key the magnetic catalogue uses (survey of magnetics.ndjson,
    // 2026-11-09: 80,794 of 97,191 records state it, 74,954 with a minimum). Straight from the
    // object: a cold limit is negative and 0 degC is a real rating.
    if (const json* th = obj_get(*di, "thermal")) {
        if (const json* t = obj_get(*th, "operatingTemperature")) {
            if (auto v = get_num(*t, "minimum")) r.temp_min_c = *v;
            if (auto v = get_num(*t, "maximum")) r.temp_max_c = *v;
        }
    }
    auto status = get_str(*mi, "status");
    r.is_production = status.has_value() && *status == "production";
    return r;
}

// ---- browse-only families (analog ICs / timing devices) --------------------
// Both envelopes carry a VARIABLE subtype key (analog.<subtype> / timeBase.<subtype>), so the
// walk takes the first object member carrying a manufacturerInfo. Identity is required; every
// electrical datum is optional (NaN when absent) — these rows browse, they never gate.
namespace {
struct SubtypeNode {
    std::string subtype;
    const json* mi = nullptr;  // manufacturerInfo
    const json* di = nullptr;  // datasheetInfo (may be null)
};

std::optional<SubtypeNode> subtype_node(const json& env, const char* root) {
    const json* node = obj_get(env, root);
    if (!node || !node->is_object()) return std::nullopt;
    for (auto it = node->begin(); it != node->end(); ++it) {
        if (!it->is_object()) continue;
        const json* mi = obj_get(*it, "manufacturerInfo");
        if (!mi) continue;
        SubtypeNode out;
        out.subtype = it.key();
        out.mi = mi;
        out.di = obj_get(*mi, "datasheetInfo");
        return out;
    }
    return std::nullopt;
}

void set_if_pos(double& dst, const std::optional<double>& v) {
    if (v && *v > 0) dst = *v;
}
}  // namespace

std::optional<AnalogRow> extract_analog(const json& env) {
    auto node = subtype_node(env, "analog");
    if (!node) return std::nullopt;
    const json empty = json::object();
    const json* di = node->di ? node->di : &empty;
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;

    auto mpn = get_str(*node->mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    auto manuf = get_str(*node->mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    AnalogRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    fill_dimensions(r, *di, *part);
    r.device_type = node->subtype;

    const json* elec = obj_get(*di, "electrical");
    if (elec && elec->is_object()) {
        set_if_pos(r.channels, get_num(*elec, "numberOfChannels"));
        set_if_pos(r.input_offset_voltage, get_num(*elec, "inputOffsetVoltage"));
        set_if_pos(r.input_bias_current, get_num(*elec, "inputBiasCurrent"));
        set_if_pos(r.gain_bandwidth, get_num(*elec, "gainBandwidthProduct"));
        set_if_pos(r.slew_rate, get_num(*elec, "slewRate"));
        set_if_pos(r.cmrr, get_num(*elec, "commonModeRejectionRatio"));
        set_if_pos(r.resolution, get_num(*elec, "resolution"));
        set_if_pos(r.sample_rate, get_num(*elec, "sampleRate"));
        set_if_pos(r.on_resistance, get_num(*elec, "onResistance"));
        const json* supply = obj_get(*elec, "supply");
        if (supply) {
            set_if_pos(r.vsupply_min, get_num(*supply, "minimumSupplyVoltage"));
            set_if_pos(r.vsupply_max, get_num(*supply, "maximumSupplyVoltage"));
        }
        r.architecture = get_str(*elec, "architecture").value_or("");
        r.input_stage = get_str(*elec, "inputStage").value_or("");
    }
    auto status = get_str(*node->mi, "status");
    r.is_production = status.has_value() && *status == "production";
    return r;
}

std::optional<TimingRow> extract_timing(const json& env) {
    auto node = subtype_node(env, "timeBase");
    if (!node) return std::nullopt;
    const json empty = json::object();
    const json* di = node->di ? node->di : &empty;
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;

    auto mpn = get_str(*node->mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    auto manuf = get_str(*node->mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    TimingRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    fill_dimensions(r, *di, *part);
    r.device_type = node->subtype;

    const json* elec = obj_get(*di, "electrical");
    if (elec && elec->is_object()) {
        set_if_pos(r.frequency, get_num(*elec, "frequency"));
        set_if_pos(r.frequency_tolerance, get_num(*elec, "frequencyTolerance"));
        set_if_pos(r.frequency_stability, get_num(*elec, "frequencyStability"));
        set_if_pos(r.load_capacitance, get_num(*elec, "loadCapacitance"));
        set_if_pos(r.esr, get_num(*elec, "equivalentSeriesResistance"));
        set_if_pos(r.rms_phase_jitter, get_num(*elec, "rmsPhaseJitter"));
        const json* supply = obj_get(*elec, "supply");
        if (supply) {
            set_if_pos(r.vsupply_min, get_num(*supply, "minimumSupplyVoltage"));
            set_if_pos(r.vsupply_max, get_num(*supply, "maximumSupplyVoltage"));
        }
        r.technology = get_str(*elec, "technology").value_or("");
        r.output_type = get_str(*elec, "outputType").value_or("");
        r.mode = get_str(*elec, "mode").value_or("");
        r.enable_function = get_str(*elec, "enableFunction").value_or("");
    }
    // Read straight from the object, not through set_if_pos: a cold limit is negative.
    if (const json* th = obj_get(*di, "thermal")) {
        if (const json* t = obj_get(*th, "operatingTemperature")) {
            if (auto v = get_num(*t, "minimum")) r.temp_min_c = *v;
            if (auto v = get_num(*t, "maximum")) r.temp_max_c = *v;
        }
    }
    auto status = get_str(*node->mi, "status");
    r.is_production = status.has_value() && *status == "production";
    return r;
}

std::optional<ConnectorRow> extract_connector(const json& env) {
    const json* con = obj_get(env, "connector");
    if (!con) return std::nullopt;
    const json* mi = obj_get(*con, "manufacturerInfo");
    if (!mi) return std::nullopt;
    const json empty = json::object();
    const json* di = obj_get(*mi, "datasheetInfo");
    if (!di) di = &empty;
    const json* part = obj_get(*di, "part");
    if (!part) part = &empty;

    auto mpn = get_str(*mi, "reference");
    if (!mpn) mpn = get_str(*part, "partNumber");
    auto manuf = get_str(*mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    ConnectorRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    fill_dimensions(r, *di, *part);

    const json* elec = obj_get(*di, "electrical");
    if (elec && elec->is_object()) {
        set_if_pos(r.rated_current, get_num(*elec, "ratedCurrentPerContact"));
        set_if_pos(r.rated_voltage, get_num(*elec, "ratedVoltage"));
    }
    const json* mech = obj_get(*di, "mechanical");
    if (mech && mech->is_object()) {
        set_if_pos(r.positions, get_num(*mech, "positions"));
        set_if_pos(r.pitch, get_num(*mech, "pitch"));
        set_if_pos(r.rows, get_num(*mech, "rows"));
        set_if_pos(r.mating_cycles, get_num(*mech, "matingCycles"));
        // Mating-axis orientation (vertical / rightAngle / horizontal / mezzanine / edge /
        // cable). Empty when unstated: the ranker then reports it unverified, never a match.
        if (auto o = get_str(*mech, "orientation")) r.orientation = *o;
    }
    // Operating temperature range: a substitute must cover the original's, at both
    // ends. Read straight out of the object — NOT through set_if_pos, because a cold
    // limit is normally negative and 0 degC is a real rating, so "positive" is not a
    // presence test for a temperature.
    const json* envir = obj_get(*di, "environmental");
    if (envir && envir->is_object()) {
        if (const json* t = obj_get(*envir, "operatingTemperature")) {
            if (auto v = get_num(*t, "minimum")) r.temp_min_c = *v;
            if (auto v = get_num(*t, "maximum")) r.temp_max_c = *v;
        }
    }
    const json* fd = obj_get(*di, "familyDetails");
    if (fd && fd->is_object()) {
        r.family = get_str(*fd, "family").value_or("");
        // familyDetails is a discriminated union on family, and two of the axes below are
        // spelled per variant. Reading only one spelling of each is what made the family
        // caveat's promise — "compared where both records state one" — false about records
        // that DO state one, which is the whole of ABT #505:
        //   * the wire attach is 'termination' on wireToBoard/wireToWire/power and
        //     'clampType' on terminalBlock (CONAS utils.json says so itself: "same axis as
        //     familyWireToBoard.termination"), and the dropped half was the LARGER one —
        //     11,627 clampType rows against 9,908 termination ones — on the very family
        //     where the wire attach decides the swap;
        //   * the standardised interface is 'interfaceStandard' on dataInterface and
        //     'interface' on rf, so all 8,235 RF rows named their interface (SMA / BNC /
        //     U.FL / FAKRA …) and reached the ranker with the slot empty, which reported
        //     an SMA-against-BNC comparison "unverified" instead of a definite non-mate.
        // No record states both spellings of an axis: they belong to different variants.
        r.interface_standard = get_str(*fd, "interfaceStandard")
                                   .value_or(get_str(*fd, "interface").value_or(""));
        r.termination = get_str(*fd, "termination")
                            .value_or(get_str(*fd, "clampType").value_or(""));
        // terminalBlock's pluggable flag, tri-state: unstated stays "" (never "false").
        if (auto it = fd->find("pluggable"); it != fd->end() && it->is_boolean())
            r.pluggable = it->get<bool>() ? "true" : "false";
        // Variant identities (owner 2026-11-12): only the structured fields, each present on
        // exactly one familyDetails variant (fpcFfc / circular); unstated stays "".
        r.actuator_type = get_str(*fd, "actuatorType").value_or("");
        r.shell_size = get_str(*fd, "shellSize").value_or("");
        r.coding = get_str(*fd, "coding").value_or("");
        r.coupling_type = get_str(*fd, "couplingType").value_or("");
    }
    // The separable interface itself: only the mating-area material, which is what decides
    // whether two halves may be mated at all (gold onto tin frets and corrodes). The
    // underplating and the plating thicknesses are wear data, not compatibility data.
    if (const json* mat = obj_get(*di, "material")) {
        if (const json* plating = obj_get(*mat, "contactPlating"))
            r.contact_plating = get_str(*plating, "matingAreaMaterialRef").value_or("");
    }
    r.polarity = get_str(*part, "matingPolarity").value_or("");
    r.series = get_str(*part, "series").value_or("");
    auto status = get_str(*mi, "status");
    r.is_production = status.has_value() && *status == "production";
    return r;
}

}  // namespace kelvin
