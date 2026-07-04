#include "Select.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>

namespace kelvin {
namespace {

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
                   size_t max_candidates, RecordFetcher* fetcher) {
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
    size_t n = std::min(max_candidates, passing.size());
    for (size_t i = 0; i < n; ++i) {
        const MosfetRow* m = passing[i];
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
                  size_t max_candidates, RecordFetcher* fetcher) {
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
    size_t n = std::min(max_candidates, passing.size());
    bool qrr_active = c.qrr_max.has_value() && *c.qrr_max != 0.0;
    for (size_t i = 0; i < n; ++i) {
        const DiodeRow* d = passing[i];
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
                      CapacitorTiebreaker tb, size_t max_candidates, RecordFetcher* fetcher) {
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
    size_t n = std::min(max_candidates, passing.size());
    bool ripple_active = c.ripple_current_min.has_value() && *c.ripple_current_min > 0;
    for (size_t i = 0; i < n; ++i) {
        const CapacitorRow* x = passing[i];
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
                       size_t max_candidates, RecordFetcher* fetcher) {
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
    size_t n = std::min(max_candidates, passing.size());
    for (size_t i = 0; i < n; ++i) {
        const ControllerRow* x = passing[i];
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
                     size_t max_candidates, RecordFetcher* fetcher) {
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
    size_t n = std::min(max_candidates, passing.size());
    for (size_t i = 0; i < n; ++i) {
        const ResistorRow* r = passing[i];
        json cd = base_candidate(r, fetcher);
        cd["margins"] = json::object();
        cd["deviation"] = (r->resistance - c.target_ohms) / c.target_ohms;  // signed
        cd["sortKey"] = {{"tolerance", r->tolerance}, {"absDeviation", dev_of(r)}};
        cands.push_back(std::move(cd));
    }
    result["candidates"] = std::move(cands);
    return result;
}

}  // namespace kelvin
