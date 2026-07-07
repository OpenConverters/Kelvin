#include "MfrPolicy.hpp"
#include "Select.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace kelvin {
namespace {

// Distinct manufacturers across the candidate POOL (every gate-passing row, or every ranked row for the
// rank-not-gate families), sorted. Drives the GUI "restrict to one manufacturer" dropdown so it lists
// EVERY vendor that has a fitting part — not just the vendors of the top-N candidates shown. Computed
// over the full pool (before the diversity cap / allowlist), so it stays complete even under a restriction.
template <class Row>
json manufacturer_facet(const std::vector<const Row*>& pool) {
    std::set<std::string> mfrs;
    for (const Row* r : pool)
        if (r && !r->manufacturer.empty()) mfrs.insert(r->manufacturer);
    return json(std::vector<std::string>(mfrs.begin(), mfrs.end()));
}

// inf/NaN margin -> JSON null (Python float("inf") headrooms).
json num_or_null(double x) {
    if (std::isinf(x) || std::isnan(x)) return json(nullptr);
    return json(x);
}

json emit_rejections(const std::map<std::string, uint64_t>& rej) {
    json out = json::object();
    for (const auto& kv : rej)
        if (kv.second > 0) out[kv.first] = kv.second;
    return out;
}

template <class Row>
json base_candidate(const Row* r, RecordFetcher* fetcher) {
    json c;
    c["mpn"] = r->mpn;
    c["manufacturer"] = r->manufacturer;
    c["line"] = r->lineno;
    c["srcOffset"] = r->src_offset;
    c["srcLength"] = r->src_length;
    if (fetcher) c["envelope"] = fetcher->fetch(r->src_offset, r->src_length);
    return c;
}

}  // namespace

json select_mosfet(const Shard<MosfetRow>& shard, const MosfetConstraints& c, MosfetTiebreaker tb,
                   size_t max_candidates, RecordFetcher* fetcher, const MfrPolicy& mfr) {
    c.validate();
    std::map<std::string, uint64_t> rej;
    rej["unreadable_row"] = shard.meta.unreadable_row_count;

    std::vector<const MosfetRow*> passing;
    for (const auto& m : shard.rows) {
        if (c.exclude_discontinued && !m.is_production) { rej["discontinued"]++; continue; }
        if (!c.technology_allowed.empty() &&
            c.technology_allowed.find(m.technology) == c.technology_allowed.end()) {
            rej["technology"]++; continue;
        }
        if (m.vds_rated < c.vds_min) { rej["vds_rated_low"]++; continue; }
        if (m.id_continuous < c.id_min) { rej["id_continuous_low"]++; continue; }
        if (m.rds_on > c.rds_on_max) { rej["rds_on_high"]++; continue; }
        if (m.qg_total > c.qg_max) { rej["qg_total_high"]++; continue; }
        passing.push_back(&m);
    }
    if (passing.empty())
        throw NoCandidates("MosfetConstraints", emit_rejections(rej), shard.meta.source_line_count);

    // LOWEST_TOTAL_LOSS op-point validation (Python raises ValueError otherwise).
    bool loss_mode = tb == MosfetTiebreaker::LowestTotalLoss;
    double op_i = 0, op_v = 0, op_d = 0, op_f = 0;
    if (loss_mode) {
        auto ok = [](const std::optional<double>& x) { return x.has_value() && *x > 0; };
        if (!ok(c.op_i_rms) || !ok(c.op_vds) || !ok(c.op_duty) || !ok(c.op_fsw))
            throw InvalidOptions(
                "LOWEST_TOTAL_LOSS requires op_i_rms/op_vds/op_duty/op_fsw on MosfetConstraints");
        op_i = *c.op_i_rms; op_v = *c.op_vds; op_d = *c.op_duty; op_f = *c.op_fsw;
    }
    constexpr double kIg = 1.0;  // gate-drive current proxy
    auto total_loss = [&](const MosfetRow* m) {
        double p_cond = op_d * op_i * op_i * m->rds_on;
        double p_sw = m->qg_total > 0 ? 0.5 * op_v * op_i * m->qg_total * op_f / kIg : 0.0;
        double p_coss = m->coss > 0 ? 0.5 * m->coss * op_v * op_v * op_f : 0.0;
        return p_cond + p_sw + p_coss;
    };
    auto metric = [&](const MosfetRow* m) -> double {
        switch (tb) {
            case MosfetTiebreaker::LowestRdsOn: return m->rds_on;
            case MosfetTiebreaker::LowestQg: return m->qg_total;
            case MosfetTiebreaker::HighestVdsMargin: return -m->vds_rated / c.vds_min;
            case MosfetTiebreaker::HighestIdMargin: return -m->id_continuous / c.id_min;
            case MosfetTiebreaker::LowestTotalLoss: return total_loss(m);
        }
        return 0;
    };
    // tuple (evidence_incomplete, no_thermal, metric, lineno) ascending — mirrors Python min().
    std::sort(passing.begin(), passing.end(), [&](const MosfetRow* a, const MosfetRow* b) {
        auto ka = std::make_tuple(a->evidence_incomplete() ? 1 : 0, a->no_thermal() ? 1 : 0,
                                  metric(a), a->lineno);
        auto kb = std::make_tuple(b->evidence_incomplete() ? 1 : 0, b->no_thermal() ? 1 : 0,
                                  metric(b), b->lineno);
        return ka < kb;
    });

    json result;
    result["category"] = "mosfet";
    result["tiebreaker"] = to_string(tb);
    result["totalRowsConsidered"] = shard.meta.source_line_count;
    result["alternativesConsidered"] = passing.size();
    result["rejections"] = emit_rejections(rej);
    json cands = json::array();
    result["manufacturers"] = manufacturer_facet(passing);
    const auto chosen = apply_mfr_policy(passing, max_candidates, mfr);
    for (size_t i = 0; i < chosen.size(); ++i) {
        const MosfetRow* m = chosen[i];
        json cd = base_candidate(m, fetcher);
        cd["margins"] = {
            {"vds_margin", m->vds_rated / c.vds_min},
            {"id_margin", m->id_continuous / c.id_min},
            {"rds_on_headroom", c.rds_on_max / m->rds_on},
            {"qg_headroom", m->qg_total > 0 ? num_or_null(c.qg_max / m->qg_total) : json(nullptr)},
        };
        cd["evidence"] = {{"datasheetUsable", m->datasheet_usable},
                          {"thermalPresent", !m->no_thermal()},
                          {"qgPresent", m->qg_total > 0}};
        if (loss_mode) {
            double p_cond = op_d * op_i * op_i * m->rds_on;
            double p_sw = m->qg_total > 0 ? 0.5 * op_v * op_i * m->qg_total * op_f / kIg : 0.0;
            double p_coss = m->coss > 0 ? 0.5 * m->coss * op_v * op_v * op_f : 0.0;
            cd["sortKey"] = {{"metric", "total_loss"}, {"pCond", p_cond}, {"pSw", p_sw},
                             {"pCoss", p_coss}, {"total", p_cond + p_sw + p_coss}};
        } else {
            cd["sortKey"] = {{"metric", to_string(tb)}, {"value", metric(m)}};
        }
        cands.push_back(std::move(cd));
    }
    result["candidates"] = std::move(cands);
    return result;
}

json select_diode(const Shard<DiodeRow>& shard, const DiodeConstraints& c, DiodeTiebreaker tb,
                  size_t max_candidates, RecordFetcher* fetcher, const MfrPolicy& mfr) {
    c.validate();
    std::map<std::string, uint64_t> rej;
    rej["unreadable_row"] = shard.meta.unreadable_row_count;
    std::vector<const DiodeRow*> passing;
    for (const auto& d : shard.rows) {
        if (c.exclude_discontinued && !d.is_production) { rej["discontinued"]++; continue; }
        if (d.vrrm_rated < c.vrrm_min) { rej["vrrm_low"]++; continue; }
        if (d.if_avg_rated < c.if_avg_min) { rej["if_avg_low"]++; continue; }
        if (c.qrr_max.has_value() && d.qrr > *c.qrr_max) { rej["qrr_high"]++; continue; }
        passing.push_back(&d);
    }
    if (passing.empty())
        throw NoCandidates("DiodeConstraints", emit_rejections(rej), shard.meta.source_line_count);

    auto metric = [&](const DiodeRow* d) -> double {
        switch (tb) {
            case DiodeTiebreaker::LowestVf: return d->vf_typ;
            case DiodeTiebreaker::LowestQrr: return d->qrr;
            case DiodeTiebreaker::HighestVrrmMargin: return -d->vrrm_rated / c.vrrm_min;
            case DiodeTiebreaker::HighestIfMargin: return -d->if_avg_rated / c.if_avg_min;
        }
        return 0;
    };
    std::sort(passing.begin(), passing.end(), [&](const DiodeRow* a, const DiodeRow* b) {
        return std::make_tuple(a->no_thermal() ? 1 : 0, metric(a), a->lineno) <
               std::make_tuple(b->no_thermal() ? 1 : 0, metric(b), b->lineno);
    });

    json result;
    result["category"] = "diode";
    result["tiebreaker"] = to_string(tb);
    result["totalRowsConsidered"] = shard.meta.source_line_count;
    result["alternativesConsidered"] = passing.size();
    result["rejections"] = emit_rejections(rej);
    json cands = json::array();
    result["manufacturers"] = manufacturer_facet(passing);
    const auto chosen = apply_mfr_policy(passing, max_candidates, mfr);
    bool qrr_active = c.qrr_max.has_value() && *c.qrr_max != 0.0;
    for (size_t i = 0; i < chosen.size(); ++i) {
        const DiodeRow* d = chosen[i];
        json cd = base_candidate(d, fetcher);
        cd["margins"] = {
            {"vrrm_margin", d->vrrm_rated / c.vrrm_min},
            {"if_avg_margin", d->if_avg_rated / c.if_avg_min},
            {"qrr_headroom",
             (qrr_active && d->qrr > 0) ? num_or_null(*c.qrr_max / d->qrr) : json(nullptr)},
        };
        cd["evidence"] = {{"thermalPresent", !d->no_thermal()}};
        cd["sortKey"] = {{"metric", to_string(tb)}, {"value", metric(d)}};
        cands.push_back(std::move(cd));
    }
    result["candidates"] = std::move(cands);
    return result;
}

json select_capacitor(const Shard<CapacitorRow>& shard, const CapacitorConstraints& c,
                      CapacitorTiebreaker tb, size_t max_candidates, RecordFetcher* fetcher, const MfrPolicy& mfr) {
    c.validate();
    std::map<std::string, uint64_t> rej;
    rej["unreadable_row"] = shard.meta.unreadable_row_count;
    std::vector<const CapacitorRow*> passing;
    for (const auto& x : shard.rows) {
        if (c.exclude_discontinued && !x.is_production) { rej["discontinued"]++; continue; }
        if (!c.technology_allowed.empty() &&
            c.technology_allowed.find(x.technology) == c.technology_allowed.end()) {
            rej["technology"]++; continue;
        }
        if (x.v_rated < c.v_rated_min) { rej["v_rated_low"]++; continue; }
        if (x.capacitance < c.capacitance_min) { rej["capacitance_low"]++; continue; }
        if (x.capacitance > c.capacitance_max) { rej["capacitance_high"]++; continue; }
        if (c.ripple_current_min.has_value() && x.ripple_current_rms < *c.ripple_current_min) {
            rej["ripple_low"]++; continue;
        }
        passing.push_back(&x);
    }
    if (passing.empty())
        throw NoCandidates("CapacitorConstraints", emit_rejections(rej),
                           shard.meta.source_line_count);

    double ripple_min_denom =
        (c.ripple_current_min && *c.ripple_current_min != 0.0) ? *c.ripple_current_min : 1.0;
    auto metric = [&](const CapacitorRow* x) -> double {
        switch (tb) {
            case CapacitorTiebreaker::LowestEsr: return x->esr;
            case CapacitorTiebreaker::HighestRippleHeadroom:
                return -x->ripple_current_rms / ripple_min_denom;
            case CapacitorTiebreaker::HighestVoltageMargin: return -x->v_rated / c.v_rated_min;
            case CapacitorTiebreaker::HighestCapacitance: return -x->capacitance;
        }
        return 0;
    };
    std::sort(passing.begin(), passing.end(), [&](const CapacitorRow* a, const CapacitorRow* b) {
        return std::make_tuple(a->no_thermal() ? 1 : 0, metric(a), a->lineno) <
               std::make_tuple(b->no_thermal() ? 1 : 0, metric(b), b->lineno);
    });

    json result;
    result["category"] = "capacitor";
    result["tiebreaker"] = to_string(tb);
    result["totalRowsConsidered"] = shard.meta.source_line_count;
    result["alternativesConsidered"] = passing.size();
    result["rejections"] = emit_rejections(rej);
    json cands = json::array();
    result["manufacturers"] = manufacturer_facet(passing);
    const auto chosen = apply_mfr_policy(passing, max_candidates, mfr);
    bool ripple_active = c.ripple_current_min.has_value() && *c.ripple_current_min > 0;
    for (size_t i = 0; i < chosen.size(); ++i) {
        const CapacitorRow* x = chosen[i];
        json cd = base_candidate(x, fetcher);
        cd["margins"] = {
            {"v_margin", x->v_rated / c.v_rated_min},
            {"capacitance_ratio", x->capacitance / c.capacitance_min},
            {"ripple_headroom",
             ripple_active ? num_or_null(x->ripple_current_rms / *c.ripple_current_min)
                           : json(nullptr)},
        };
        cd["evidence"] = {{"thermalPresent", !x->no_thermal()}};
        cd["sortKey"] = {{"metric", to_string(tb)}, {"value", metric(x)}};
        cands.push_back(std::move(cd));
    }
    result["candidates"] = std::move(cands);
    return result;
}

json select_controller(const Shard<ControllerRow>& shard, const ControllerConstraints& c,
                       size_t max_candidates, RecordFetcher* fetcher, const MfrPolicy& mfr) {
    c.validate();
    std::string topo = c.topology;
    std::transform(topo.begin(), topo.end(), topo.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::map<std::string, uint64_t> rej;
    rej["unreadable_row"] = shard.meta.unreadable_row_count;
    std::vector<const ControllerRow*> passing;
    for (const auto& ctrl : shard.rows) {
        bool has_topo = !ctrl.topologies.empty();
        bool topo_listed =
            std::find(ctrl.topologies.begin(), ctrl.topologies.end(), topo) != ctrl.topologies.end();
        bool has_any = std::find(ctrl.topologies.begin(), ctrl.topologies.end(), "any") !=
                       ctrl.topologies.end();
        bool has_all = std::find(ctrl.topologies.begin(), ctrl.topologies.end(), "all") !=
                       ctrl.topologies.end();
        if (has_topo && !topo_listed && !has_any && !has_all) { rej["topology"]++; continue; }
        // vin/fsw windows are permissive (0..1e12) — never reject (kept for structural parity).
        if (!(ctrl.vin_min() <= c.vin_nom && c.vin_nom <= ctrl.vin_max())) {
            rej["vin_out_of_range"]++; continue;
        }
        if (!(ctrl.fsw_min_khz() <= c.fsw_khz && c.fsw_khz <= ctrl.fsw_max_khz())) {
            rej["fsw_out_of_range"]++; continue;
        }
        if (c.integrated_fet.has_value() && ctrl.integrated_fet != *c.integrated_fet) {
            rej["integrated_fet_mismatch"]++; continue;
        }
        if (c.category.has_value() && ctrl.category != *c.category) {
            rej["category_mismatch"]++; continue;
        }
        passing.push_back(&ctrl);
    }
    if (passing.empty())
        throw NoCandidates("ControllerConstraints", emit_rejections(rej),
                           shard.meta.source_line_count);

    // Python: max(passing, key=(real_ctrl, has_vref, topo_specific)); first-maximal wins ties.
    // -> sort DESCENDING by the tuple, ties broken by ASCENDING lineno.
    auto rank = [&](const ControllerRow* x) {
        int real_ctrl = x->category == "gateDriver" ? 0 : 1;
        int has_vref = x->has_vref() ? 1 : 0;
        int topo_specific =
            std::find(x->topologies.begin(), x->topologies.end(), topo) != x->topologies.end() ? 1
                                                                                                : 0;
        return std::make_tuple(real_ctrl, has_vref, topo_specific);
    };
    std::sort(passing.begin(), passing.end(), [&](const ControllerRow* a, const ControllerRow* b) {
        auto ra = rank(a), rb = rank(b);
        if (ra != rb) return ra > rb;       // higher tuple first
        return a->lineno < b->lineno;       // tie: earlier file order (Python first-maximal)
    });

    json result;
    result["category"] = "controller";
    result["tiebreaker"] = "controller_rank";
    result["totalRowsConsidered"] = shard.meta.source_line_count;
    result["alternativesConsidered"] = passing.size();
    result["rejections"] = emit_rejections(rej);
    json cands = json::array();
    result["manufacturers"] = manufacturer_facet(passing);
    const auto chosen = apply_mfr_policy(passing, max_candidates, mfr);
    for (size_t i = 0; i < chosen.size(); ++i) {
        const ControllerRow* x = chosen[i];
        json cd = base_candidate(x, fetcher);
        cd["margins"] = json::object();
        auto r = rank(x);
        cd["sortKey"] = {{"realController", std::get<0>(r)},
                         {"hasVref", std::get<1>(r)},
                         {"topologySpecific", std::get<2>(r)}};
        cd["evidence"] = {{"vrefPresent", x->has_vref()},
                          {"category", x->category},
                          {"integratedDriver", x->integrated_driver}};
        cands.push_back(std::move(cd));
    }
    result["candidates"] = std::move(cands);
    return result;
}

json select_resistor(const Shard<ResistorRow>& shard, const ResistorConstraints& c,
                     size_t max_candidates, RecordFetcher* fetcher, const MfrPolicy& mfr) {
    c.validate();
    std::map<std::string, uint64_t> rej;
    rej["unreadable_row"] = shard.meta.unreadable_row_count;
    std::vector<const ResistorRow*> passing;
    for (const auto& r : shard.rows) {
        if (r.tolerance > c.max_tolerance) { rej["tolerance_loose"]++; continue; }
        double dev = std::fabs(r.resistance - c.target_ohms) / c.target_ohms;
        if (dev > c.max_value_deviation) { rej["value_far"]++; continue; }
        passing.push_back(&r);
    }
    if (passing.empty())
        throw NoCandidates("ResistorConstraints", emit_rejections(rej),
                           shard.meta.source_line_count);

    auto dev_of = [&](const ResistorRow* r) {
        return std::fabs(r->resistance - c.target_ohms) / c.target_ohms;
    };
    // Python keeps the best by (tolerance, dev), first wins ties -> ascending (tolerance,dev,lineno).
    std::sort(passing.begin(), passing.end(), [&](const ResistorRow* a, const ResistorRow* b) {
        return std::make_tuple(a->tolerance, dev_of(a), a->lineno) <
               std::make_tuple(b->tolerance, dev_of(b), b->lineno);
    });

    json result;
    result["category"] = "resistor";
    result["tiebreaker"] = "nearest_value";
    result["totalRowsConsidered"] = shard.meta.source_line_count;
    result["alternativesConsidered"] = passing.size();  // == Python `considered`
    result["rejections"] = emit_rejections(rej);
    json cands = json::array();
    result["manufacturers"] = manufacturer_facet(passing);
    const auto chosen = apply_mfr_policy(passing, max_candidates, mfr);
    for (size_t i = 0; i < chosen.size(); ++i) {
        const ResistorRow* r = chosen[i];
        json cd = base_candidate(r, fetcher);
        cd["margins"] = json::object();
        cd["deviation"] = (r->resistance - c.target_ohms) / c.target_ohms;  // signed
        cd["sortKey"] = {{"tolerance", r->tolerance}, {"absDeviation", dev_of(r)}};
        cands.push_back(std::move(cd));
    }
    result["candidates"] = std::move(cands);
    return result;
}

// ---- Phase 5 selectors -----------------------------------------------------
json select_igbt(const Shard<IgbtRow>& shard, const IgbtConstraints& c, IgbtTiebreaker tb,
                 size_t max_candidates, RecordFetcher* fetcher, const MfrPolicy& mfr) {
    c.validate();
    std::map<std::string, uint64_t> rej;
    rej["unreadable_row"] = shard.meta.unreadable_row_count;
    std::vector<const IgbtRow*> passing;
    for (const auto& g : shard.rows) {
        if (c.exclude_discontinued && !g.is_production) { rej["discontinued"]++; continue; }
        if (g.vces_rated < c.vces_min) { rej["vces_low"]++; continue; }
        if (g.ic_continuous < c.ic_min) { rej["ic_low"]++; continue; }
        if (c.vce_sat_max.has_value() && g.vce_sat > *c.vce_sat_max) { rej["vce_sat_high"]++; continue; }
        passing.push_back(&g);
    }
    if (passing.empty())
        throw NoCandidates("IgbtConstraints", emit_rejections(rej), shard.meta.source_line_count);
    auto metric = [&](const IgbtRow* g) -> double {
        switch (tb) {
            case IgbtTiebreaker::LowestVceSat: return g->vce_sat;
            case IgbtTiebreaker::HighestVcesMargin: return -g->vces_rated / c.vces_min;
            case IgbtTiebreaker::HighestIcMargin: return -g->ic_continuous / c.ic_min;
        }
        return 0;
    };
    std::sort(passing.begin(), passing.end(), [&](const IgbtRow* a, const IgbtRow* b) {
        return std::make_tuple(a->no_thermal() ? 1 : 0, metric(a), a->lineno) <
               std::make_tuple(b->no_thermal() ? 1 : 0, metric(b), b->lineno);
    });
    json result;
    result["category"] = "igbt";
    result["tiebreaker"] = to_string(tb);
    result["totalRowsConsidered"] = shard.meta.source_line_count;
    result["alternativesConsidered"] = passing.size();
    result["rejections"] = emit_rejections(rej);
    json cands = json::array();
    result["manufacturers"] = manufacturer_facet(passing);
    const auto chosen = apply_mfr_policy(passing, max_candidates, mfr);
    for (size_t i = 0; i < chosen.size(); ++i) {
        const IgbtRow* g = chosen[i];
        json cd = base_candidate(g, fetcher);
        cd["margins"] = {{"vces_margin", g->vces_rated / c.vces_min},
                         {"ic_margin", g->ic_continuous / c.ic_min},
                         {"vce_sat_headroom",
                          (c.vce_sat_max && g->vce_sat > 0) ? num_or_null(*c.vce_sat_max / g->vce_sat)
                                                            : json(nullptr)}};
        cd["evidence"] = {{"thermalPresent", !g->no_thermal()}};
        cd["sortKey"] = {{"metric", to_string(tb)}, {"value", metric(g)}};
        cands.push_back(std::move(cd));
    }
    result["candidates"] = std::move(cands);
    return result;
}

json select_bjt(const Shard<BjtRow>& shard, const BjtConstraints& c, BjtTiebreaker tb,
                size_t max_candidates, RecordFetcher* fetcher, const MfrPolicy& mfr) {
    c.validate();
    std::map<std::string, uint64_t> rej;
    rej["unreadable_row"] = shard.meta.unreadable_row_count;
    std::vector<const BjtRow*> passing;
    for (const auto& b : shard.rows) {
        if (c.exclude_discontinued && !b.is_production) { rej["discontinued"]++; continue; }
        if (b.vceo_rated < c.vceo_min) { rej["vceo_low"]++; continue; }
        if (b.ic_continuous < c.ic_min) { rej["ic_low"]++; continue; }
        if (c.hfe_min.has_value() && b.hfe_min < *c.hfe_min) { rej["hfe_low"]++; continue; }
        passing.push_back(&b);
    }
    if (passing.empty())
        throw NoCandidates("BjtConstraints", emit_rejections(rej), shard.meta.source_line_count);
    auto metric = [&](const BjtRow* b) -> double {
        switch (tb) {
            case BjtTiebreaker::HighestHfe: return -b->hfe_min;
            case BjtTiebreaker::HighestVceoMargin: return -b->vceo_rated / c.vceo_min;
            case BjtTiebreaker::HighestIcMargin: return -b->ic_continuous / c.ic_min;
        }
        return 0;
    };
    std::sort(passing.begin(), passing.end(), [&](const BjtRow* a, const BjtRow* b) {
        return std::make_tuple(a->no_thermal() ? 1 : 0, metric(a), a->lineno) <
               std::make_tuple(b->no_thermal() ? 1 : 0, metric(b), b->lineno);
    });
    json result;
    result["category"] = "bjt";
    result["tiebreaker"] = to_string(tb);
    result["totalRowsConsidered"] = shard.meta.source_line_count;
    result["alternativesConsidered"] = passing.size();
    result["rejections"] = emit_rejections(rej);
    json cands = json::array();
    result["manufacturers"] = manufacturer_facet(passing);
    const auto chosen = apply_mfr_policy(passing, max_candidates, mfr);
    for (size_t i = 0; i < chosen.size(); ++i) {
        const BjtRow* b = chosen[i];
        json cd = base_candidate(b, fetcher);
        cd["margins"] = {{"vceo_margin", b->vceo_rated / c.vceo_min},
                         {"ic_margin", b->ic_continuous / c.ic_min},
                         {"hfe", b->hfe_min}};
        cd["sortKey"] = {{"metric", to_string(tb)}, {"value", metric(b)}};
        cands.push_back(std::move(cd));
    }
    result["candidates"] = std::move(cands);
    return result;
}

json select_varistor(const Shard<VaristorRow>& shard, const VaristorConstraints& c,
                     VaristorTiebreaker tb, size_t max_candidates, RecordFetcher* fetcher, const MfrPolicy& mfr) {
    c.validate();
    std::map<std::string, uint64_t> rej;
    rej["unreadable_row"] = shard.meta.unreadable_row_count;
    std::vector<const VaristorRow*> passing;
    for (const auto& v : shard.rows) {
        if (c.exclude_discontinued && !v.is_production) { rej["discontinued"]++; continue; }
        if (v.max_continuous_dc_voltage < c.rated_continuous_voltage) { rej["vc_low"]++; continue; }
        if (c.max_clamping_voltage.has_value() && v.clamping_voltage > *c.max_clamping_voltage) {
            rej["clamping_high"]++; continue;
        }
        if (c.min_peak_surge_current.has_value() && v.peak_surge_current < *c.min_peak_surge_current) {
            rej["surge_low"]++; continue;
        }
        if (c.max_capacitance.has_value() && v.capacitance > *c.max_capacitance) {
            rej["capacitance_high"]++; continue;
        }
        passing.push_back(&v);
    }
    if (passing.empty())
        throw NoCandidates("VaristorConstraints", emit_rejections(rej),
                           shard.meta.source_line_count);
    // Parts missing the ranked datum are deprioritised (clamping/capacitance 0 = unknown, not best).
    auto tier_metric = [&](const VaristorRow* v) -> std::tuple<int, double> {
        switch (tb) {
            case VaristorTiebreaker::LowestClampingVoltage:
                return {v->clamping_voltage <= 0 ? 1 : 0, v->clamping_voltage};
            case VaristorTiebreaker::HighestSurge:
                return {v->peak_surge_current <= 0 ? 1 : 0, -v->peak_surge_current};
            case VaristorTiebreaker::LowestCapacitance:
                return {v->capacitance <= 0 ? 1 : 0, v->capacitance};
        }
        return {0, 0};
    };
    std::sort(passing.begin(), passing.end(), [&](const VaristorRow* a, const VaristorRow* b) {
        auto ka = tier_metric(a);
        auto kb = tier_metric(b);
        return std::make_tuple(std::get<0>(ka), std::get<1>(ka), a->lineno) <
               std::make_tuple(std::get<0>(kb), std::get<1>(kb), b->lineno);
    });
    json result;
    result["category"] = "varistor";
    result["tiebreaker"] = to_string(tb);
    result["totalRowsConsidered"] = shard.meta.source_line_count;
    result["alternativesConsidered"] = passing.size();
    result["rejections"] = emit_rejections(rej);
    json cands = json::array();
    result["manufacturers"] = manufacturer_facet(passing);
    const auto chosen = apply_mfr_policy(passing, max_candidates, mfr);
    for (size_t i = 0; i < chosen.size(); ++i) {
        const VaristorRow* v = chosen[i];
        json cd = base_candidate(v, fetcher);
        cd["margins"] = {
            {"vc_margin", v->max_continuous_dc_voltage / c.rated_continuous_voltage},
            {"clamping_headroom",
             (c.max_clamping_voltage && v->clamping_voltage > 0)
                 ? num_or_null(*c.max_clamping_voltage / v->clamping_voltage)
                 : json(nullptr)},
            {"surge_headroom", (c.min_peak_surge_current && *c.min_peak_surge_current > 0)
                                   ? num_or_null(v->peak_surge_current / *c.min_peak_surge_current)
                                   : json(nullptr)}};
        cd["sortKey"] = {{"clampingVoltage", v->clamping_voltage},
                         {"peakSurgeCurrent", v->peak_surge_current}};
        cands.push_back(std::move(cd));
    }
    result["candidates"] = std::move(cands);
    return result;
}

// ---- Magnetic (from-spec, rank-not-gate) -----------------------------------
namespace {
// Scoring weights — inductance closeness dominates; a sat-current / rated-current shortfall only
// breaks near-ties. All terms are natural-log distances (dimensionless, scale-free), so a 2x
// mismatch costs the same whether the target is 1 uH or 1 mH.
constexpr double kIndW = 1.0;    // weight: |ln(L/target)| closeness
constexpr double kSatW = 0.5;    // weight: peak-current saturation shortfall
constexpr double kRatW = 0.25;   // weight: rms-current rated shortfall
constexpr double kTurnsW = 1.0;  // weight: |ln(turnsRatio/target)| closeness (transformer selection)
// Penalty a candidate pays for lacking the datum a set target would score against (ranked below
// parts that have the datum but roughly this far off — never dropped).
const double kMissingInd = std::log(3.0);   // ~1.10  (≈ a 3x inductance mismatch)
const double kMissingCur = std::log(2.0);   // ~0.69  (≈ a 2x current shortfall)

// Per-dimension verdict: 2=pass, 1=marginal, 0=fail, -1=not-applicable (target or datum absent).
int inductance_status(bool have, double L, double target) {
    if (!have || !present(L) || L <= 0) return -1;
    double r = L / target;
    if (r >= 1.0 / 1.3 && r <= 1.3) return 2;   // within ±30 %
    if (r >= 0.5 && r <= 2.0) return 1;          // within a factor of 2
    return 0;
}
int headroom_status(bool have, double val, double ref) {
    if (!have || !present(val) || val <= 0) return -1;
    double h = val / ref;
    if (h >= 1.0) return 2;
    if (h >= 0.8) return 1;
    return 0;
}
const char* verdict_str(int v) {
    switch (v) {
        case 2: return "pass";
        case 1: return "marginal";
        case 0: return "fail";
    }
    return "unknown";
}
}  // namespace

json select_connector(const Shard<ConnectorRow>& shard, const ConnectorConstraints& c,
                      ConnectorTiebreaker tb, size_t max_candidates, RecordFetcher* fetcher,
                      const MfrPolicy& mfr) {
    c.validate();
    std::map<std::string, uint64_t> rej;
    rej["unreadable_row"] = shard.meta.unreadable_row_count;
    std::vector<const ConnectorRow*> passing;
    for (const auto& v : shard.rows) {
        if (c.exclude_discontinued && !v.is_production) { rej["discontinued"]++; continue; }
        if (c.family && v.family != *c.family) { rej["family_mismatch"]++; continue; }
        if (c.polarity && v.polarity != *c.polarity) { rej["polarity_mismatch"]++; continue; }
        // exact contact count: a 10-position part is no substitute for a 26-position need
        if (c.positions) {
            if (!present(v.positions)) { rej["positions_undocumented"]++; continue; }
            if (v.positions != *c.positions) { rej["positions_mismatch"]++; continue; }
        }
        // gated ratings: an undocumented rating is rejected (counted), never assumed adequate
        if (c.current_min) {
            if (!present(v.rated_current)) { rej["current_undocumented"]++; continue; }
            if (v.rated_current < *c.current_min) { rej["current_low"]++; continue; }
        }
        if (c.voltage_min) {
            if (!present(v.rated_voltage)) { rej["voltage_undocumented"]++; continue; }
            if (v.rated_voltage < *c.voltage_min) { rej["voltage_low"]++; continue; }
        }
        passing.push_back(&v);
    }
    if (passing.empty())
        throw NoCandidates("ConnectorConstraints", emit_rejections(rej),
                           shard.meta.source_line_count);
    // Rank by rating headroom; a part missing the ranked datum is deprioritised, not dropped
    // (it already passed every gate the caller asked for).
    auto tier_metric = [&](const ConnectorRow* v) -> std::tuple<int, double> {
        switch (tb) {
            case ConnectorTiebreaker::HighestCurrentMargin:
                return {present(v->rated_current) ? 0 : 1,
                        present(v->rated_current) ? -v->rated_current : 0.0};
            case ConnectorTiebreaker::HighestVoltageMargin:
                return {present(v->rated_voltage) ? 0 : 1,
                        present(v->rated_voltage) ? -v->rated_voltage : 0.0};
        }
        return {0, 0};
    };
    std::sort(passing.begin(), passing.end(), [&](const ConnectorRow* a, const ConnectorRow* b) {
        auto ka = tier_metric(a);
        auto kb = tier_metric(b);
        return std::make_tuple(std::get<0>(ka), std::get<1>(ka), a->lineno) <
               std::make_tuple(std::get<0>(kb), std::get<1>(kb), b->lineno);
    });
    json result;
    result["category"] = "connector";
    result["tiebreaker"] = to_string(tb);
    result["totalRowsConsidered"] = shard.meta.source_line_count;
    result["alternativesConsidered"] = passing.size();
    result["rejections"] = emit_rejections(rej);
    result["manufacturers"] = manufacturer_facet(passing);
    json cands = json::array();
    const auto chosen = apply_mfr_policy(passing, max_candidates, mfr);
    for (const ConnectorRow* v : chosen) {
        json cd = base_candidate(v, fetcher);
        cd["margins"] = {
            {"current_margin", (c.current_min && present(v->rated_current))
                                   ? num_or_null(v->rated_current / *c.current_min)
                                   : json(nullptr)},
            {"voltage_margin", (c.voltage_min && present(v->rated_voltage))
                                   ? num_or_null(v->rated_voltage / *c.voltage_min)
                                   : json(nullptr)},
        };
        cd["evidence"] = {
            {"family", v->family},
            {"interfaceStandard", v->interface_standard},
            {"series", v->series},
            {"polarity", v->polarity},
            {"positions", present(v->positions) ? json(v->positions) : json(nullptr)},
            {"ratedCurrentPerContact",
             present(v->rated_current) ? json(v->rated_current) : json(nullptr)},
            {"ratedVoltage", present(v->rated_voltage) ? json(v->rated_voltage) : json(nullptr)},
            {"isProduction", v->is_production},
        };
        cands.push_back(std::move(cd));
    }
    result["candidates"] = std::move(cands);
    return result;
}

json select_magnetic(const Shard<MagneticRow>& shard, const MagneticConstraints& c,
                     size_t max_candidates, RecordFetcher* fetcher, const MfrPolicy& mfr) {
    c.validate();  // never throws — kept for structural parity with the other selectors
    std::map<std::string, uint64_t> rej;
    rej["unreadable_row"] = shard.meta.unreadable_row_count;

    // RANK-NOT-GATE: every readable row is a candidate. We never drop a part for missing a spec —
    // each instead carries per-dimension margins + a pass/marginal/fail verdict so the UI can show
    // even imperfect matches. (There are no rejection buckets beyond the index-time unreadable count.)
    std::vector<const MagneticRow*> all;
    all.reserve(shard.rows.size());
    for (const auto& m : shard.rows) all.push_back(&m);
    if (all.empty())
        throw NoCandidates("MagneticConstraints", emit_rejections(rej), shard.meta.source_line_count);

    const bool have_L = c.target_inductance && *c.target_inductance > 0;
    const bool have_ipk = c.peak_current && *c.peak_current > 0;
    const bool have_irms = c.rms_current && *c.rms_current > 0;
    const bool have_TR = c.target_turns_ratio && *c.target_turns_ratio > 0;

    auto penalty = [&](const MagneticRow* m) -> double {
        double p = 0.0;
        if (have_L) {
            if (present(m->inductance) && m->inductance > 0)
                p += kIndW * std::fabs(std::log(m->inductance / *c.target_inductance));
            else
                p += kIndW * kMissingInd;
        }
        if (have_ipk) {
            if (present(m->saturation_current) && m->saturation_current > 0)
                p += kSatW * std::max(0.0, std::log(*c.peak_current / m->saturation_current));
            else
                p += kSatW * kMissingCur;
        }
        if (have_irms) {
            if (present(m->rated_current) && m->rated_current > 0)
                p += kRatW * std::max(0.0, std::log(*c.rms_current / m->rated_current));
            else
                p += kRatW * kMissingCur;
        }
        if (have_TR) {
            if (present(m->turns_ratio) && m->turns_ratio > 0)
                p += kTurnsW * std::fabs(std::log(m->turns_ratio / *c.target_turns_ratio));
            else
                p += kTurnsW * kMissingInd;  // no ratio datum → sink but never drop
        }
        return p;
    };
    // Unknown DCR sorts last among otherwise-equal parts (a real low-DCR part is preferred).
    auto dcr_key = [](const MagneticRow* m) {
        return present(m->dcr) ? m->dcr : std::numeric_limits<double>::infinity();
    };
    // Ascending tuple: (fit penalty, production first, lower DCR, source line) — deterministic.
    std::sort(all.begin(), all.end(), [&](const MagneticRow* a, const MagneticRow* b) {
        return std::make_tuple(penalty(a), a->is_production ? 0 : 1, dcr_key(a), a->lineno) <
               std::make_tuple(penalty(b), b->is_production ? 0 : 1, dcr_key(b), b->lineno);
    });

    json result;
    result["category"] = "magnetic";
    result["tiebreaker"] = "best_fit";
    result["totalRowsConsidered"] = shard.meta.source_line_count;
    result["alternativesConsidered"] = all.size();
    result["rejections"] = emit_rejections(rej);
    result["target"] = {{"inductance", have_L ? json(*c.target_inductance) : json(nullptr)},
                        {"peakCurrent", have_ipk ? json(*c.peak_current) : json(nullptr)},
                        {"rmsCurrent", have_irms ? json(*c.rms_current) : json(nullptr)},
                        {"turnsRatio", have_TR ? json(*c.target_turns_ratio) : json(nullptr)},
                        {"kind", c.kind}};

    json cands = json::array();
    result["manufacturers"] = manufacturer_facet(all);
    const auto chosen = apply_mfr_policy(all, max_candidates, mfr);
    for (const MagneticRow* m : chosen) {
        json cd = base_candidate(m, fetcher);
        int is = inductance_status(have_L, m->inductance, have_L ? *c.target_inductance : 1.0);
        int ss = headroom_status(have_ipk, m->saturation_current, have_ipk ? *c.peak_current : 1.0);
        int rs = headroom_status(have_irms, m->rated_current, have_irms ? *c.rms_current : 1.0);
        // Turns-ratio closeness reuses the inductance ±30 % / factor-2 bands (same "match a value" shape).
        int trs = inductance_status(have_TR, m->turns_ratio, have_TR ? *c.target_turns_ratio : 1.0);
        int overall = -1;  // worst of the evaluated dimensions; -1 => nothing evaluable
        for (int s : {is, ss, rs, trs})
            if (s >= 0) overall = (overall < 0) ? s : std::min(overall, s);

        cd["margins"] = {
            {"inductance_ratio",
             (have_L && present(m->inductance)) ? num_or_null(m->inductance / *c.target_inductance)
                                                : json(nullptr)},
            {"inductance_log_distance",
             (have_L && present(m->inductance) && m->inductance > 0)
                 ? num_or_null(std::fabs(std::log(m->inductance / *c.target_inductance)))
                 : json(nullptr)},
            {"saturation_headroom",
             (have_ipk && present(m->saturation_current))
                 ? num_or_null(m->saturation_current / *c.peak_current)
                 : json(nullptr)},
            {"rated_headroom",
             (have_irms && present(m->rated_current))
                 ? num_or_null(m->rated_current / *c.rms_current)
                 : json(nullptr)},
            {"turns_ratio_ratio",
             (have_TR && present(m->turns_ratio) && m->turns_ratio > 0)
                 ? num_or_null(m->turns_ratio / *c.target_turns_ratio)
                 : json(nullptr)},
        };
        cd["verdict"] = verdict_str(overall);
        cd["verdictByDimension"] = {{"inductance", verdict_str(is)},
                                    {"saturationCurrent", verdict_str(ss)},
                                    {"ratedCurrent", verdict_str(rs)},
                                    {"turnsRatio", verdict_str(trs)}};
        cd["evidence"] = {{"deviceType", m->device_type},
                          {"family", m->family},
                          {"isProduction", m->is_production},
                          {"inductance", num_or_null(m->inductance)},
                          {"saturationCurrent", num_or_null(m->saturation_current)},
                          {"ratedCurrent", num_or_null(m->rated_current)},
                          {"turnsRatio", num_or_null(m->turns_ratio)},
                          {"dcr", num_or_null(m->dcr)},
                          {"selfResonantFrequency", num_or_null(m->srf)}};
        cd["sortKey"] = {{"metric", "best_fit"}, {"penalty", penalty(m)}};
        cands.push_back(std::move(cd));
    }
    result["candidates"] = std::move(cands);
    return result;
}

}  // namespace kelvin
