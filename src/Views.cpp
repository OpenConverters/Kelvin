#include "Views.hpp"

#include <regex>
#include <unordered_map>

#include "DimensionJson.hpp"  // PEAS::resolve_dimensional_values (json overload)

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

// Physical body size + case code, shared by every family extractor: the
// cross-reference footprint check needs a real size, because a substitute that
// does not fit the board is not a substitute. Dimensions are
// dimensionWithTolerance in TAS, so they go through the resolver rather than
// being hand-read. Absent stays NaN — the ranker then falls back to resolving
// the case code, and reports "unknown" if that fails too, never assuming a fit.
void fill_dimensions(RowBase& r, const json& di, const json& part) {
    if (const json* mech = obj_get(di, "mechanical")) {
        // Some records nest the drawing one level deeper under `dimensions`.
        const json* nested = obj_get(*mech, "dimensions");
        const json& src = nested ? *nested : *mech;
        if (auto v = resolve_field(src, "length"); pos(v)) r.length_m = *v;
        if (auto v = resolve_field(src, "width"); pos(v)) r.width_m = *v;
        if (auto v = resolve_field(src, "height"); pos(v)) r.height_m = *v;
        if (nested) {  // a nested block may still leave siblings on `mechanical`
            if (!present(r.length_m))
                if (auto v = resolve_field(*mech, "length"); pos(v)) r.length_m = *v;
            if (!present(r.width_m))
                if (auto v = resolve_field(*mech, "width"); pos(v)) r.width_m = *v;
            if (!present(r.height_m))
                if (auto v = resolve_field(*mech, "height"); pos(v)) r.height_m = *v;
        }
    }
    // Case code: `part.case` is the field the catalogue actually populates
    // (100% of sampled capacitors/mosfets/resistors/diodes); `mechanical.case`
    // and the caseCode/package spellings are accepted as fallbacks.
    const json* mech = obj_get(di, "mechanical");
    if (auto c = get_str(part, "case")) r.case_code = *c;
    else if (mech) { if (auto c = get_str(*mech, "case")) r.case_code = *c; }
    if (r.case_code.empty()) {
        if (auto c = get_str(part, "caseCode")) r.case_code = *c;
        else if (auto p = get_str(part, "package")) r.case_code = *p;
    }
    // Explicit mount type when the record states it (smt / tht / chassis).
    if (mech) { if (auto a = get_str(*mech, "assemblyType")) r.mount = *a; }
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
    auto manuf = get_str(*mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    auto vds = get_num(*elec, "drainSourceVoltage");
    auto id = get_num(*elec, "continuousDrainCurrent");
    auto rds = get_num(*elec, "onResistance");
    if (!pos(vds) || !pos(id) || !pos(rds)) return std::nullopt;

    auto qg = get_num(*elec, "totalGateCharge");
    double qg_total = qg.value_or(0.0);  // legacy rows: Qg constraint becomes vacuous
    if (qg_total < 0.0) return std::nullopt;

    auto coss_o = get_num(*elec, "outputCapacitance");
    double coss = coss_o.value_or(0.0);
    if (coss < 0.0) return std::nullopt;

    auto vgs = resolve_field(*elec, "gateThresholdVoltage", PEAS::DimensionalValues::MAXIMUM);
    double vgs_max = vgs.value_or(0.0);

    MosfetRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    fill_dimensions(r, *di, *part);
    r.vds_rated = *vds;
    r.id_continuous = *id;
    r.rds_on = *rds;
    r.qg_total = qg_total;
    r.coss = coss;
    r.vgs_threshold_max = vgs_max;
    if (auto v = get_num(*elec, "onResistanceVgs"); pos(v)) r.rds_on_vgs = *v;
    if (auto v = get_num(*elec, "gateSourceVoltageMax"); pos(v)) r.vgs_max = *v;
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
    auto manuf = get_str(*mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    auto vrrm = get_num(*elec, "reverseVoltage");
    auto if_avg = get_num(*elec, "forwardCurrent");
    auto vf = get_num(*elec, "forwardVoltage");
    if (!pos(vrrm) || !pos(if_avg) || !pos(vf)) return std::nullopt;

    auto qrr_o = get_num(*elec, "reverseRecoveryCharge");
    double qrr = (qrr_o && *qrr_o >= 0) ? *qrr_o : 0.0;
    auto trr_o = get_num(*elec, "reverseRecoveryTime");
    double trr = (trr_o && *trr_o >= 0) ? *trr_o : 0.0;

    DiodeRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    fill_dimensions(r, *di, *part);
    r.vrrm_rated = *vrrm;
    r.if_avg_rated = *if_avg;
    r.vf_typ = *vf;
    r.qrr = qrr;
    r.trr = trr;
    r.technology = get_str(*part, "subType").value_or("");
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
    auto manuf = get_str(*mi, "name");
    if (!mpn || !manuf) return std::nullopt;

    auto cap_nom = resolve_field(*elec, "capacitance");
    auto v_rated = get_num(*elec, "ratedVoltage");
    if (!pos(cap_nom) || !pos(v_rated)) return std::nullopt;

    auto ripple_o = get_num(*elec, "rippleCurrent");
    double ripple = (ripple_o && *ripple_o >= 0) ? *ripple_o : 0.0;
    auto esr_o = get_num(*elec, "esr");
    double esr = (esr_o && *esr_o >= 0) ? *esr_o : 0.0;

    CapacitorRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    fill_dimensions(r, *di, *part);
    r.capacitance = *cap_nom;
    r.v_rated = *v_rated;
    r.ripple_current_rms = ripple;
    r.esr = esr;
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
    if (auto v = get_num(*elec, "esrFrequency"); pos(v)) r.esr_frequency = *v;
    // Operating temperature range: a substitute must cover the original's, at
    // both ends. Present on essentially every capacitor record.
    if (const json* th = obj_get(*di, "thermal")) {
        if (const json* t = obj_get(*th, "temperature")) {
            if (auto v = get_num(*t, "minimum")) r.temp_min_c = *v;
            if (auto v = get_num(*t, "maximum")) r.temp_max_c = *v;
        }
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

    auto r_nom = resolve_field(*elec, "resistance");
    if (!r_nom || *r_nom <= 0) return std::nullopt;

    auto tol_o = get_num(*elec, "tolerance");
    double tol = (tol_o && *tol_o > 0) ? *tol_o : 0.05;
    auto pw_o = get_num(*elec, "powerRating");
    double pw = (pw_o && *pw_o > 0) ? *pw_o : 0.0;

    ResistorRow r;
    r.mpn = *mpn;
    r.manufacturer = get_str(*mi, "name").value_or("");
    fill_dimensions(r, *di, *part);
    r.resistance = *r_nom;
    r.tolerance = tol;
    r.power_rating = pw;
    auto status = get_str(*mi, "status");
    r.is_production = status.has_value() && *status == "production";
    return r;
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
    auto manuf = get_str(*refs->mi, "name");
    if (!mpn || !manuf) return std::nullopt;
    auto vceo = get_num(*refs->elec, "collectorEmitterVoltage");
    auto ic = get_num(*refs->elec, "collectorCurrent");
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
double magnetic_saturation_current(const json& elec) {
    auto peak = get_num(elec, "saturationCurrentPeak");
    if (peak && *peak > 0) return *peak;
    const json* tbl = obj_get(elec, "saturationCurrents");
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
        if (best) return get_num(*best, "current").value_or(kNaN());
        return max_cur;  // no stated basis -> the largest quoted current
    }
    return kNaN();
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
    fill_dimensions(r, *di, *part);

    // electrical is an ARRAY; the first entry is the primary-winding projection we rank on.
    const json* elec_arr = obj_get(*di, "electrical");
    if (elec_arr && elec_arr->is_array() && !elec_arr->empty() && elec_arr->front().is_object()) {
        const json& elec = elec_arr->front();
        auto L = resolve_field(elec, "inductance");
        if (L && *L > 0) r.inductance = *L;
        r.saturation_current = magnetic_saturation_current(elec);
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
    }

    std::string fam = get_str(*mi, "family").value_or("");
    if (fam.empty()) fam = get_str(*part, "family").value_or("");
    r.family = fam;
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
    if (mech && mech->is_object()) set_if_pos(r.positions, get_num(*mech, "positions"));
    const json* fd = obj_get(*di, "familyDetails");
    if (fd && fd->is_object()) {
        r.family = get_str(*fd, "family").value_or("");
        r.interface_standard = get_str(*fd, "interfaceStandard").value_or("");
    }
    r.polarity = get_str(*part, "matingPolarity").value_or("");
    r.series = get_str(*part, "series").value_or("");
    auto status = get_str(*mi, "status");
    r.is_production = status.has_value() && *status == "production";
    return r;
}

}  // namespace kelvin
