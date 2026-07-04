#include "Views.hpp"

#include <regex>
#include <unordered_map>

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

// A dimensionWithTolerance-or-scalar field: read `.nominal` when it's an object, else the scalar.
// (Parity note: HS reads the literal `.nominal`/`.maximum` key here, NOT the PEAS resolver's
// nominal->mid->max chain. We reproduce HS exactly; a post-parity change could switch to the
// resolver — see KELVIN_PROPOSAL.md open question 5.)
std::optional<double> dim_key(const json& o, const char* key, const char* which) {
    const json* v = obj_get(o, key);
    if (!v) return std::nullopt;
    if (v->is_object()) {
        const json* n = obj_get(*v, which);
        if (n && n->is_number() && !n->is_boolean()) return n->get<double>();
        return std::nullopt;
    }
    if (v->is_number() && !v->is_boolean()) return v->get<double>();
    return std::nullopt;
}

bool pos(const std::optional<double>& x) { return x.has_value() && *x > 0.0; }

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

    auto vgs = dim_key(*elec, "gateThresholdVoltage", "maximum");
    double vgs_max = vgs.value_or(0.0);

    MosfetRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    r.vds_rated = *vds;
    r.id_continuous = *id;
    r.rds_on = *rds;
    r.qg_total = qg_total;
    r.coss = coss;
    r.vgs_threshold_max = vgs_max;
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

    auto cap_nom = dim_key(*elec, "capacitance", "nominal");
    auto v_rated = get_num(*elec, "ratedVoltage");
    if (!pos(cap_nom) || !pos(v_rated)) return std::nullopt;

    auto ripple_o = get_num(*elec, "rippleCurrent");
    double ripple = (ripple_o && *ripple_o >= 0) ? *ripple_o : 0.0;
    auto esr_o = get_num(*elec, "esr");
    double esr = (esr_o && *esr_o >= 0) ? *esr_o : 0.0;

    CapacitorRow r;
    r.mpn = *mpn;
    r.manufacturer = *manuf;
    r.capacitance = *cap_nom;
    r.v_rated = *v_rated;
    r.ripple_current_rms = ripple;
    r.esr = esr;
    // technology = part.family or part.subType or part.series
    std::string tech = get_str(*part, "family").value_or("");
    if (tech.empty()) tech = get_str(*part, "subType").value_or("");
    if (tech.empty()) tech = get_str(*part, "series").value_or("");
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

    auto r_nom = dim_key(*elec, "resistance", "nominal");
    if (!r_nom || *r_nom <= 0) return std::nullopt;

    auto tol_o = get_num(*elec, "tolerance");
    double tol = (tol_o && *tol_o > 0) ? *tol_o : 0.05;
    auto pw_o = get_num(*elec, "powerRating");
    double pw = (pw_o && *pw_o > 0) ? *pw_o : 0.0;

    ResistorRow r;
    r.mpn = *mpn;
    r.manufacturer = get_str(*mi, "name").value_or("");
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

}  // namespace kelvin
