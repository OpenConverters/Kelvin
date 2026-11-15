#include "CapacitorLoss.hpp"
#include "CrossRef.hpp"  // kGrossOverRatingFactor (one rule with the cross-reference)
#include "MfrPolicy.hpp"
#include "DiodeLoss.hpp"
#include "MagneticLoss.hpp"
#include "MosfetLoss.hpp"
#include "OpRanking.hpp"  // the operating-point ranking's UNVERIFIED key (all families)
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

// Board area of a MOSFET (owner 2026-11-14): the land the cross-reference resolves for it —
// the record's drawn L x W, else the case table's outline for its case code — in mm^2, with
// where it came from. Not resolvable -> mm2 null, "not stated".
json board_area_json(const MosfetRow& m) {
    json p = json::object();
    if (present(m.length_m)) p["length_m"] = m.length_m;
    if (present(m.width_m)) p["width_m"] = m.width_m;
    if (present(m.height_m)) p["height_m"] = m.height_m;
    if (!m.case_code.empty()) p["case_code"] = m.case_code;
    if (!m.mount.empty()) p["mount"] = m.mount;
    const auto d = crossref::dims_of(p, "mosfet");
    if (!d || !crossref::has_land(*d))
        return {{"mm2", nullptr},
                {"basis", "not stated: no drawn L x W and no resolvable case code" +
                              (m.case_code.empty() ? std::string() : " ('" + m.case_code + "')")}};
    return {{"mm2", *d->length * *d->width * 1e6},
            {"basis", d->land_from_table ? d->land_source
                                         : std::string("the record's drawn length x width")}};
}

// Price (owner 2026-11-14): the record's distributor unit costs. The schema holds no price
// breaks, so the price AT the asked quantity is not stated; the unit costs are shown as such.
json price_json(const MosfetRow& m, std::optional<double> quantity) {
    json p;
    p["quantity"] = quantity ? json(*quantity) : json(nullptr);
    p["atQuantity"] = nullptr;
    p["quotes"] = m.price_quotes.empty() ? json::array() : json::parse(m.price_quotes);
    p["basis"] = m.price_quotes.empty()
                     ? "not stated: the record carries no distributor cost"
                     : "unit cost as the distributor states it; the record carries no price "
                       "breaks, so the price at a given quantity is not stated";
    return p;
}

json loss_term_json(const LossTerm& t) {
    json j;
    j["watts"] = t.watts ? json(*t.watts) : json(nullptr);
    if (t.watts) j["basis"] = t.basis;
    else j["missing"] = t.missing;
    return j;
}

// One candidate's operating-point loss: every term (watts + basis, or null + what is
// missing), the total (null while any term is UNVERIFIED), and the notes.
json loss_json(const LossBreakdown& lb, const LossOperatingPoint& op) {
    json loss;
    loss["position"] = to_string(op.position);
    loss["dutyCycle"] = lb.duty;
    loss["dutyBasis"] = lb.duty_basis;
    loss["irms"] = lb.irms;
    loss["terms"] = {{"conduction", loss_term_json(lb.conduction)},
                     {"switching", loss_term_json(lb.switching)},
                     {"gateDrive", loss_term_json(lb.gate_drive)},
                     {"coss", loss_term_json(lb.coss)}};
    const auto tot = lb.total();
    loss["total"] = tot ? json(*tot) : json(nullptr);
    loss["computedSum"] = lb.computed_sum();
    loss["unverified"] = lb.unverified();
    loss["notes"] = lb.notes;
    return loss;
}

json loss_op_json(const LossOperatingPoint& op) {
    json o = {{"position", to_string(op.position)}, {"inputVoltage", op.vin},
              {"outputCurrent", op.iout}, {"switchingFrequency", op.fsw},
              {"dutyCycle", op.duty_value()}};
    if (op.vout) o["outputVoltage"] = *op.vout;
    if (op.ripple_pp) o["inductorRippleCurrent"] = *op.ripple_pp;
    if (op.vdrive) o["gateDriveVoltage"] = *op.vdrive;
    if (op.idrive) o["gateDriveCurrent"] = *op.idrive;
    if (op.tj) o["junctionTemperature"] = *op.tj;
    return o;
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
        if (c.channel && m.channel != *c.channel) { rej["channel_mismatch"]++; continue; }
        if (m.vds_rated < c.vds_min) { rej["vds_rated_low"]++; continue; }
        // An unstated Id (NaN: a dual whose dies state none) does not meet an Id floor — it
        // is not read as passing (NaN < x is false) nor as 0; it has its own bucket.
        if (!present(m.id_continuous)) { rej["id_continuous_unstated"]++; continue; }
        if (m.id_continuous < c.id_min) { rej["id_continuous_low"]++; continue; }
        if (m.rds_on > c.rds_on_max) { rej["rds_on_high"]++; continue; }
        // A row with no stated Qg (NaN) is not rejected by a Qg ceiling: the constraint is
        // vacuous for it, and the evidence tier below ranks it as incomplete.
        if (present(m.qg_total) && m.qg_total > c.qg_max) { rej["qg_total_high"]++; continue; }
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
    // LOWEST_OPERATING_POINT_LOSS (owner 2026-11-14): the caller's operating point, required.
    const bool op_loss_mode = tb == MosfetTiebreaker::LowestOperatingPointLoss;
    std::map<const MosfetRow*, LossBreakdown> op_loss;
    if (op_loss_mode) {
        if (!c.loss_op)
            throw InvalidOptions("lowest_operating_point_loss requires a lossOperatingPoint "
                                 "(position, inputVoltage, outputCurrent, switchingFrequency, "
                                 "dutyCycle or outputVoltage)");
        c.loss_op->validate();
        for (const MosfetRow* m : passing) op_loss.emplace(m, switch_loss(*m, *c.loss_op));
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
            // An absent Qg (NaN) must not reach the sort: NaN breaks the strict weak
            // ordering std::sort needs. It sorts LAST — an unknown gate charge is not the
            // lowest one, which is what the 0 it used to hold made it.
            case MosfetTiebreaker::LowestQg:
                return present(m->qg_total) ? m->qg_total
                                            : std::numeric_limits<double>::infinity();
            case MosfetTiebreaker::HighestVdsMargin: return -m->vds_rated / c.vds_min;
            case MosfetTiebreaker::HighestIdMargin: return -m->id_continuous / c.id_min;
            case MosfetTiebreaker::LowestTotalLoss: return total_loss(m);
            case MosfetTiebreaker::LowestOperatingPointLoss: return op_loss.at(m).computed_sum();
        }
        return 0;
    };
    // Operating-point loss: a candidate with an UNVERIFIED term ranks after every fully
    // computed one (owner 2026-11-14) — first key, ahead of the evidence tiers; among the
    // incomplete ones, fewer unverified terms first, then the sum of the terms that were
    // computed.
    // A candidate whose board area cannot be determined counts one more UNVERIFIED item (owner
    // 2026-11-14, every family: op_ranking).
    std::map<const MosfetRow*, op_ranking::SizeEvidence> op_size;
    if (op_loss_mode)
        for (const MosfetRow* m : passing) op_size.emplace(m, op_ranking::size_of(board_area_json(*m)));
    auto unverified_terms = [&](const MosfetRow* m) -> size_t {
        return op_loss_mode
                   ? op_ranking::unverified_items(op_loss.at(m).unverified().size(), op_size.at(m))
                   : 0;
    };
    // Gross over-rating (owner 2026-11-14): Vds or Id more than kGrossOverRatingFactor (3x)
    // the requirement. Each such axis is one step of a tier just above the metric, so an
    // over-rated part sinks below the parts of its evidence tier that are not; it is never
    // excluded. Same factor as the cross-reference's Y4 rule.
    auto over_rated_axes = [&](const MosfetRow* m) {
        std::vector<std::string> axes;
        if (m->vds_rated > crossref::kGrossOverRatingFactor * c.vds_min) axes.push_back("vds");
        if (present(m->id_continuous) && m->id_continuous > crossref::kGrossOverRatingFactor * c.id_min)
            axes.push_back("id");
        return axes;
    };
    // The tier ranks only where the ranking itself does not already weigh the rating (owner
    // 2026-11-14): under the operating-point loss the loss decides (over-rating is a flag and a
    // note), and under highest_vds_margin / highest_id_margin a higher rating is what was asked
    // for. Everywhere else it stays a tier.
    const bool over_rating_ranks = c.over_rating_tier && !op_loss_mode &&
                                   tb != MosfetTiebreaker::HighestVdsMargin &&
                                   tb != MosfetTiebreaker::HighestIdMargin;
    auto over_rated = [&](const MosfetRow* m) -> size_t {
        return over_rating_ranks ? over_rated_axes(m).size() : 0;
    };
    // Under the operating-point loss, total loss alone sets the order after the UNVERIFIED key
    // (owner 2026-11-14): the evidence / thermal tiers do not reorder it.
    auto evidence_tier = [&](const MosfetRow* m) -> int {
        return !op_loss_mode && m->evidence_incomplete() ? 1 : 0;
    };
    auto thermal_tier = [&](const MosfetRow* m) -> int {
        return !op_loss_mode && m->no_thermal() ? 1 : 0;
    };
    // tuple (evidence_incomplete, no_thermal, metric, lineno) ascending — mirrors Python min().
    std::sort(passing.begin(), passing.end(), [&](const MosfetRow* a, const MosfetRow* b) {
        auto ka = std::make_tuple(unverified_terms(a), evidence_tier(a), thermal_tier(a),
                                  over_rated(a), metric(a), a->lineno);
        auto kb = std::make_tuple(unverified_terms(b), evidence_tier(b), thermal_tier(b),
                                  over_rated(b), metric(b), b->lineno);
        return ka < kb;
    });

    json result;
    result["category"] = "mosfet";
    result["tiebreaker"] = to_string(tb);
    if (op_loss_mode) result["lossOperatingPoint"] = loss_op_json(*c.loss_op);
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
        cd["boardArea"] = board_area_json(*m);
        cd["price"] = price_json(*m, c.price_quantity);
        cd["ratings"] = {{"vds", m->vds_rated},
                         {"id", num_or_null(m->id_continuous)},
                         {"rdsOn", m->rds_on}};
        if (c.over_rating_tier) {
            json orj = {{"axes", json::array()}, {"notes", json::array()}};
            for (const std::string& ax : over_rated_axes(m)) {
                const bool v = ax == "vds";
                const double have = v ? m->vds_rated : m->id_continuous;
                const double need = v ? c.vds_min : c.id_min;
                char b[400];
                std::snprintf(b, sizeof b,
                              "%s %g %s is %.3gx the requirement's %g %s — more than %gx: a "
                              "bigger body, higher cost and more capacitance are likely; %s",
                              v ? "drain-source voltage rating" : "continuous drain current rating",
                              have, v ? "V" : "A", have / need, need, v ? "V" : "A",
                              crossref::kGrossOverRatingFactor,
                              over_rating_ranks
                                  ? "ranked after closer-rated parts"
                                  : (op_loss_mode ? "flagged only: the operating-point loss "
                                                    "alone sets the order"
                                                  : "flagged only: this ranking does not "
                                                    "demote it"));
                orj["axes"].push_back(ax);
                orj["notes"].push_back(b);
            }
            cd["overRating"] = std::move(orj);
        }
        // A dual / complementary package stays offered, LABELLED (owner 2026-11-13, V3): its
        // ratings are per die and its pinout is not a single die's. Only stated values are
        // emitted; a record that states no configuration carries neither key.
        if (!m->die_configuration.empty())
            cd["evidence"]["dieConfiguration"] = m->die_configuration;
        if (!m->internal_connection.empty())
            cd["evidence"]["internalConnection"] = m->internal_connection;
        if (loss_mode) {
            double p_cond = op_d * op_i * op_i * m->rds_on;
            double p_sw = m->qg_total > 0 ? 0.5 * op_v * op_i * m->qg_total * op_f / kIg : 0.0;
            double p_coss = m->coss > 0 ? 0.5 * m->coss * op_v * op_v * op_f : 0.0;
            cd["sortKey"] = {{"metric", "total_loss"}, {"pCond", p_cond}, {"pSw", p_sw},
                             {"pCoss", p_coss}, {"total", p_cond + p_sw + p_coss}};
        } else if (op_loss_mode) {
            cd["loss"] = loss_json(op_loss.at(m), *c.loss_op);
            const LossBreakdown& lb = op_loss.at(m);
            const op_ranking::SizeEvidence& sz = op_size.at(m);
            if (sz.undetermined) cd["loss"]["notes"].push_back(sz.note);
            cd["sortKey"] = {{"metric", to_string(tb)}, {"value", lb.computed_sum()}};
            cd["sortKey"].update(op_ranking::sort_key_fields(lb.unverified().size(), sz));
        } else {
            cd["sortKey"] = {{"metric", to_string(tb)}, {"value", metric(m)}};
        }
        cands.push_back(std::move(cd));
    }
    result["candidates"] = std::move(cands);
    return result;
}

namespace {

// Board area of a diode (owner 2026-11-14), resolved exactly as board_area_json(MosfetRow):
// the record's drawn L x W, else the case table's outline for its case code.
json board_area_json(const DiodeRow& d) {
    json p = json::object();
    if (present(d.length_m)) p["length_m"] = d.length_m;
    if (present(d.width_m)) p["width_m"] = d.width_m;
    if (present(d.height_m)) p["height_m"] = d.height_m;
    if (!d.case_code.empty()) p["case_code"] = d.case_code;
    if (!d.mount.empty()) p["mount"] = d.mount;
    const auto dm = crossref::dims_of(p, "diode");
    if (!dm || !crossref::has_land(*dm))
        return {{"mm2", nullptr},
                {"basis", "not stated: no drawn L x W and no resolvable case code" +
                              (d.case_code.empty() ? std::string() : " ('" + d.case_code + "')")}};
    return {{"mm2", *dm->length * *dm->width * 1e6},
            {"basis", dm->land_from_table ? dm->land_source
                                          : std::string("the record's drawn length x width")}};
}

// Price of a diode (owner 2026-11-14), as price_json(MosfetRow).
json price_json(const DiodeRow& d, std::optional<double> quantity) {
    json p;
    p["quantity"] = quantity ? json(*quantity) : json(nullptr);
    p["atQuantity"] = nullptr;
    p["quotes"] = d.price_quotes.empty() ? json::array() : json::parse(d.price_quotes);
    p["basis"] = d.price_quotes.empty()
                     ? "not stated: the record carries no distributor cost"
                     : "unit cost as the distributor states it; the record carries no price "
                       "breaks, so the price at a given quantity is not stated";
    return p;
}

// The curves a diode row carries for the loss terms (datasheetInfo.curves, SI).
DiodeCurves diode_curves_of(const DiodeRow& d) { return {&d.curves->vf_curve, &d.curves->cj_curve, &d.curves->ir_curve}; }

json diode_loss_json(const DiodeLossBreakdown& lb, const DiodeLossOperatingPoint& op) {
    json loss;
    loss["position"] = to_string(op.position);
    loss["dutyCycle"] = lb.duty;
    loss["dutyBasis"] = lb.duty_basis;
    loss["forwardCurrent"] = lb.i_on;
    loss["averageForwardCurrent"] = lb.i_avg;
    loss["terms"] = {{"conduction", loss_term_json(lb.conduction)},
                     {"switching", loss_term_json(lb.switching)},
                     {"leakage", loss_term_json(lb.leakage)}};
    const auto tot = lb.total();
    loss["total"] = tot ? json(*tot) : json(nullptr);
    loss["computedSum"] = lb.computed_sum();
    loss["unverified"] = lb.unverified();
    loss["notes"] = lb.notes;
    return loss;
}

json diode_loss_op_json(const DiodeLossOperatingPoint& op) {
    json o = {{"position", to_string(op.position)}, {"inputVoltage", op.vin},
              {"outputCurrent", op.iout}, {"switchingFrequency", op.fsw},
              {"dutyCycle", op.duty_value()}};
    if (op.vout) o["outputVoltage"] = *op.vout;
    if (op.ripple_pp) o["inductorRippleCurrent"] = *op.ripple_pp;
    if (op.tj) o["junctionTemperature"] = *op.tj;
    return o;
}

}  // namespace

json select_diode(const Shard<DiodeRow>& shard, const DiodeConstraints& c, DiodeTiebreaker tb,
                  size_t max_candidates, RecordFetcher* fetcher, const MfrPolicy& mfr) {
    c.validate();
    std::map<std::string, uint64_t> rej;
    rej["unreadable_row"] = shard.meta.unreadable_row_count;
    std::vector<const DiodeRow*> passing;
    for (const auto& d : shard.rows) {
        if (c.exclude_discontinued && !d.is_production) { rej["discontinued"]++; continue; }
        // Negated form on purpose: zener/TVS/ESD rows carry NaN where the subtype
        // publishes no forward rating, and `NaN < min` is FALSE — the natural-looking
        // `if (x < min) reject` therefore lets an UNKNOWN value pass a numeric gate.
        // Caught by the parity golden when TVS parts became selectable for a 1.5 A
        // requirement they say nothing about. Absent must fail the bound, not skip it.
        if (!(d.vrrm_rated >= c.vrrm_min)) { rej["vrrm_low"]++; continue; }
        if (!(d.if_avg_rated >= c.if_avg_min)) { rej["if_avg_low"]++; continue; }
        if (c.qrr_max.has_value() && d.qrr > *c.qrr_max) { rej["qrr_high"]++; continue; }
        passing.push_back(&d);
    }
    if (passing.empty())
        throw NoCandidates("DiodeConstraints", emit_rejections(rej), shard.meta.source_line_count);

    // LOWEST_OPERATING_POINT_LOSS (owner 2026-11-14): the caller's operating point, required.
    const bool op_loss_mode = tb == DiodeTiebreaker::LowestOperatingPointLoss;
    std::map<const DiodeRow*, DiodeLossBreakdown> op_loss;
    if (op_loss_mode) {
        if (!c.loss_op)
            throw InvalidOptions("lowest_operating_point_loss requires a lossOperatingPoint "
                                 "(position, inputVoltage, outputCurrent, switchingFrequency, "
                                 "dutyCycle or outputVoltage)");
        c.loss_op->validate();
        for (const DiodeRow* d : passing)
            op_loss.emplace(d, diode_loss(*d, *c.loss_op, diode_curves_of(*d)));
    }

    // A NaN metric would make this comparator violate strict weak ordering, which is UB
    // in std::sort — not merely a wrong order. vf_typ is NaN on subtypes that publish no
    // forward voltage (a zener may pass both gates and still have none), so an unknown
    // metric sorts LAST rather than being fed to the comparator.
    auto worst = std::numeric_limits<double>::infinity();
    auto metric = [&](const DiodeRow* d) -> double {
        double m = 0;
        switch (tb) {
            case DiodeTiebreaker::LowestVf: m = d->vf_typ; break;
            case DiodeTiebreaker::LowestQrr: m = d->qrr; break;
            case DiodeTiebreaker::HighestVrrmMargin: m = -d->vrrm_rated / c.vrrm_min; break;
            case DiodeTiebreaker::HighestIfMargin: m = -d->if_avg_rated / c.if_avg_min; break;
            case DiodeTiebreaker::LowestOperatingPointLoss: m = op_loss.at(d).computed_sum(); break;
        }
        return std::isnan(m) ? worst : m;
    };
    // Operating-point loss: a candidate with an UNVERIFIED term ranks after every fully
    // computed one (owner 2026-11-14) — first key; among the incomplete ones, fewer
    // unverified terms first, then the sum of the terms that were computed.
    // A candidate whose board area cannot be determined counts one more UNVERIFIED item (owner
    // 2026-11-14, every family: op_ranking).
    std::map<const DiodeRow*, op_ranking::SizeEvidence> op_size;
    if (op_loss_mode)
        for (const DiodeRow* d : passing) op_size.emplace(d, op_ranking::size_of(board_area_json(*d)));
    auto unverified_terms = [&](const DiodeRow* d) -> size_t {
        return op_loss_mode
                   ? op_ranking::unverified_items(op_loss.at(d).unverified().size(), op_size.at(d))
                   : 0;
    };
    // Gross over-rating (owner 2026-11-14): Vrrm or If(avg) more than kGrossOverRatingFactor
    // (3x) the requirement. A tier just above the metric under lowest_vf / lowest_qrr; a flag
    // and a note only under the operating-point loss (the loss decides) and the margin
    // rankings (a higher rating is what they ask for). Never excluded.
    auto over_rated_axes = [&](const DiodeRow* d) {
        std::vector<std::string> axes;
        if (d->vrrm_rated > crossref::kGrossOverRatingFactor * c.vrrm_min) axes.push_back("vrrm");
        if (present(d->if_avg_rated) && d->if_avg_rated > crossref::kGrossOverRatingFactor * c.if_avg_min)
            axes.push_back("if_avg");
        return axes;
    };
    const bool over_rating_ranks = c.over_rating_tier && !op_loss_mode &&
                                   tb != DiodeTiebreaker::HighestVrrmMargin &&
                                   tb != DiodeTiebreaker::HighestIfMargin;
    auto over_rated = [&](const DiodeRow* d) -> size_t {
        return over_rating_ranks ? over_rated_axes(d).size() : 0;
    };
    // Under the operating-point loss, total loss alone sets the order after the UNVERIFIED key
    // (owner 2026-11-14): the thermal tier does not reorder it.
    auto thermal_tier = [&](const DiodeRow* d) -> int {
        return !op_loss_mode && d->no_thermal() ? 1 : 0;
    };
    std::sort(passing.begin(), passing.end(), [&](const DiodeRow* a, const DiodeRow* b) {
        return std::make_tuple(unverified_terms(a), thermal_tier(a), over_rated(a), metric(a),
                               a->lineno) <
               std::make_tuple(unverified_terms(b), thermal_tier(b), over_rated(b), metric(b),
                               b->lineno);
    });

    json result;
    result["category"] = "diode";
    result["tiebreaker"] = to_string(tb);
    if (op_loss_mode) result["lossOperatingPoint"] = diode_loss_op_json(*c.loss_op);
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
        cd["boardArea"] = board_area_json(*d);
        cd["price"] = price_json(*d, c.price_quantity);
        cd["ratings"] = {{"vrrm", d->vrrm_rated},
                         {"ifAvg", num_or_null(d->if_avg_rated)},
                         {"vf", num_or_null(d->vf_typ)}};
        if (!d->technology.empty()) cd["ratings"]["subType"] = d->technology;
        if (c.over_rating_tier) {
            json orj = {{"axes", json::array()}, {"notes", json::array()}};
            for (const std::string& ax : over_rated_axes(d)) {
                const bool v = ax == "vrrm";
                const double have = v ? d->vrrm_rated : d->if_avg_rated;
                const double need = v ? c.vrrm_min : c.if_avg_min;
                char b[400];
                std::snprintf(b, sizeof b,
                              "%s %g %s is %.3gx the requirement's %g %s — more than %gx: a "
                              "bigger body, higher cost and more capacitance are likely; %s",
                              v ? "repetitive reverse voltage rating" : "average forward current rating",
                              have, v ? "V" : "A", have / need, need, v ? "V" : "A",
                              crossref::kGrossOverRatingFactor,
                              over_rating_ranks
                                  ? "ranked after closer-rated parts"
                                  : (op_loss_mode ? "flagged only: the operating-point loss "
                                                    "alone sets the order"
                                                  : "flagged only: this ranking does not "
                                                    "demote it"));
                orj["axes"].push_back(ax);
                orj["notes"].push_back(b);
            }
            cd["overRating"] = std::move(orj);
        }
        if (op_loss_mode) {
            const DiodeLossBreakdown& lb = op_loss.at(d);
            cd["loss"] = diode_loss_json(lb, *c.loss_op);
            const op_ranking::SizeEvidence& sz = op_size.at(d);
            if (sz.undetermined) cd["loss"]["notes"].push_back(sz.note);
            cd["sortKey"] = {{"metric", to_string(tb)}, {"value", lb.computed_sum()}};
            cd["sortKey"].update(op_ranking::sort_key_fields(lb.unverified().size(), sz));
        } else {
            cd["sortKey"] = {{"metric", to_string(tb)}, {"value", metric(d)}};
        }
        cands.push_back(std::move(cd));
    }
    result["candidates"] = std::move(cands);
    return result;
}

namespace {

// Board area and volume of a capacitor (owner 2026-11-14): the land the cross-reference resolves
// (crossref::dims_of — the record's drawn L x W, else the case table's outline for its code), and
// L x W x H with the height the record draws or its code spells out (a table's typical height is
// not the part's: dims_of drops it). Capacitor height matters, so the volume is reported beside
// the area; either is null with "not stated" when it cannot be resolved.
std::pair<json, json> capacitor_size_json(const CapacitorRow& x) {
    json p = json::object();
    if (present(x.length_m)) p["length_m"] = x.length_m;
    if (present(x.width_m)) p["width_m"] = x.width_m;
    if (present(x.height_m)) p["height_m"] = x.height_m;
    if (!x.case_code.empty()) p["case_code"] = x.case_code;
    if (!x.mount.empty()) p["mount"] = x.mount;
    if (!x.lead_form.empty()) p["lead_form"] = x.lead_form;
    if (!x.family.empty()) p["family"] = x.family;
    if (!x.technology.empty()) p["technology"] = x.technology;
    const auto d = crossref::dims_of(p, "capacitor");
    if (!d || !crossref::has_land(*d)) {
        const std::string why = "not stated: no drawn L x W and no resolvable case code" +
                                (x.case_code.empty() ? std::string() : " ('" + x.case_code + "')");
        return {json{{"mm2", nullptr}, {"basis", why}}, json{{"mm3", nullptr}, {"basis", why}}};
    }
    const std::string land = d->land_from_table ? d->land_source
                                                : std::string("the record's drawn length x width");
    json area = {{"mm2", *d->length * *d->width * 1e6}, {"basis", land}};
    json vol;
    if (d->height && *d->height > 0 && !d->height_from_table)
    {
        char h[64];
        std::snprintf(h, sizeof h, " x the stated height %.4g mm", *d->height * 1e3);
        vol = {{"mm3", *d->length * *d->width * *d->height * 1e9}, {"basis", land + h}};
    }
    else
        vol = {{"mm3", nullptr}, {"basis", "not stated: the record states no height"}};
    return {area, vol};
}

json capacitor_price_json(const CapacitorRow& x, std::optional<double> quantity) {
    json p;
    p["quantity"] = quantity ? json(*quantity) : json(nullptr);
    p["atQuantity"] = nullptr;
    p["quotes"] = x.price_quotes.empty() ? json::array() : json::parse(x.price_quotes);
    p["basis"] = x.price_quotes.empty()
                     ? "not stated: the record carries no distributor cost"
                     : "unit cost as the distributor states it; the record carries no price "
                       "breaks, so the price at a given quantity is not stated";
    return p;
}

json capacitor_check_json(const CapacitorCheck& t, const char* unit_key) {
    json j;
    j[unit_key] = t.value ? json(*t.value) : json(nullptr);
    if (t.value) j["basis"] = t.basis;
    else j["missing"] = t.missing;
    return j;
}

json capacitor_loss_json(const CapacitorLossBreakdown& lb) {
    json loss;
    loss["position"] = to_string(lb.position);
    loss["dutyCycle"] = lb.duty;
    loss["dutyBasis"] = lb.duty_basis;
    loss["rippleCurrent"] = lb.i_ripple;
    loss["rippleCurrentBasis"] = lb.i_ripple_basis;
    loss["dcVoltage"] = lb.v_dc;
    json term = loss_term_json(lb.esr_loss);
    if (lb.esr_upper_bound) term["upperBound"] = true;  // ESR from tan(delta) (owner 2026-11-14)
    loss["terms"] = {{"esrLoss", term}};
    const auto tot = lb.total();
    loss["total"] = tot ? json(*tot) : json(nullptr);
    loss["computedSum"] = lb.computed_sum();
    loss["unverified"] = lb.unverified();
    json checks;
    checks["effectiveCapacitance"] = capacitor_check_json(lb.c_eff, "farads");
    if (lb.has_ripple_voltage()) {
        json rv = capacitor_check_json(lb.ripple_voltage, "volts");
        rv["limit"] = lb.max_ripple_v ? json(*lb.max_ripple_v) : json(nullptr);
        rv["overLimit"] = lb.over_ripple_voltage();
        checks["rippleVoltage"] = std::move(rv);
    }
    json rr = capacitor_check_json(lb.ripple_rating, "amperes");
    rr["needed"] = lb.i_ripple;
    const auto ok = lb.ripple_rating_ok();
    rr["meets"] = ok ? json(*ok) : json(nullptr);
    checks["rippleCurrentRating"] = std::move(rr);
    loss["checks"] = std::move(checks);
    loss["notes"] = lb.notes;
    return loss;
}

json capacitor_loss_op_json(const CapacitorOperatingPoint& op) {
    json o = {{"position", to_string(op.position)}, {"inputVoltage", op.vin},
              {"outputVoltage", op.vout}, {"outputCurrent", op.iout},
              {"switchingFrequency", op.fsw}, {"dutyCycle", op.duty()}};
    if (op.ripple_pp) o["inductorRippleCurrent"] = *op.ripple_pp;
    if (op.max_ripple_v) o["maxRippleVoltage"] = *op.max_ripple_v;
    if (op.ambient) o["ambientTemperature"] = *op.ambient;
    return o;
}

}  // namespace

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
        // An AC-only part (line-safety X/Y) has no DC rating: NaN would compare false and
        // wave it through, so it is refused under its own reason before the DC compare.
        if (!present(x.v_rated)) { rej["no_dc_rating"]++; continue; }
        if (x.v_rated < c.v_rated_min) { rej["v_rated_low"]++; continue; }
        // A capacitance resting only on retracted sources is not stated (NaN, Views.hpp
        // unstate_retracted_ratings): NaN would compare false and pass both bounds.
        if (!present(x.capacitance)) { rej["no_capacitance"]++; continue; }
        if (x.capacitance < c.capacitance_min) { rej["capacitance_low"]++; continue; }
        if (x.capacitance > c.capacitance_max) { rej["capacitance_high"]++; continue; }
        if (c.ripple_current_min.has_value()) {
            // An UNKNOWN ripple rating cannot satisfy a ripple requirement. Since absence
            // became NaN (ABT #455), `NaN < min` is false, so this test alone would have
            // waved every part with no published ripple current straight through as if it
            // met the requirement. Rejected under its own reason so the histogram says
            // "no published rating" rather than burying it in ripple_low.
            if (!present(x.ripple_current_rms)) { rej["ripple_unknown"]++; continue; }
            if (x.ripple_current_rms < *c.ripple_current_min) { rej["ripple_low"]++; continue; }
        }
        passing.push_back(&x);
    }
    if (passing.empty())
        throw NoCandidates("CapacitorConstraints", emit_rejections(rej),
                           shard.meta.source_line_count);

    // LOWEST_OPERATING_POINT_LOSS (owner 2026-11-14): the caller's operating point, required.
    const bool op_loss_mode = tb == CapacitorTiebreaker::LowestOperatingPointLoss;
    std::map<const CapacitorRow*, CapacitorLossBreakdown> op_loss;
    if (op_loss_mode) {
        if (!c.loss_op)
            throw InvalidOptions("lowest_operating_point_loss requires a lossOperatingPoint "
                                 "(position, inputVoltage, outputVoltage, outputCurrent, "
                                 "switchingFrequency; inductorRippleCurrent for buckOutput)");
        c.loss_op->validate();
        for (const CapacitorRow* x : passing) op_loss.emplace(x, capacitor_loss(*x, *c.loss_op));
    }

    double ripple_min_denom =
        (c.ripple_current_min && *c.ripple_current_min != 0.0) ? *c.ripple_current_min : 1.0;
    auto metric = [&](const CapacitorRow* x) -> double {
        switch (tb) {
            case CapacitorTiebreaker::LowestEsr: return x->esr;
            case CapacitorTiebreaker::HighestRippleHeadroom:
                return -x->ripple_current_rms / ripple_min_denom;
            case CapacitorTiebreaker::HighestVoltageMargin: return -x->v_rated / c.v_rated_min;
            case CapacitorTiebreaker::HighestCapacitance: return -x->capacitance;
            case CapacitorTiebreaker::LowestOperatingPointLoss: return op_loss.at(x).computed_sum();
        }
        return 0;
    };
    // A NaN metric (the tiebreaker's parameter is not published for this part) must not
    // reach the comparison: NaN compares false both ways, which breaks the strict weak
    // ordering std::sort requires and is undefined behaviour, not merely a wrong order.
    // Rank unknown LAST behind a flag, exactly as no_thermal already does — and never by
    // substituting a number for the absent one, which is the defect being fixed.
    auto unknown_metric = [&](const CapacitorRow* x) { return std::isnan(metric(x)) ? 1 : 0; };
    // Operating-point loss (owner 2026-11-14): a candidate with an UNVERIFIED item ranks after
    // every fully verified one — an UNVERIFIED term, or a board area or volume that cannot be
    // determined (one more item; op_ranking, every family); then one over the caller's
    // ripple-voltage limit OR under the ripple-current rating after the ones that pass both;
    // then a rated voltage over 3x the need (below); then the loss (the thermal tier does not
    // reorder it).
    std::map<const CapacitorRow*, op_ranking::SizeEvidence> op_size;
    if (op_loss_mode)
        for (const CapacitorRow* x : passing) {
            const auto [area, vol] = capacitor_size_json(*x);
            op_size.emplace(x, op_ranking::size_of(area, &vol));
        }
    auto unverified_terms = [&](const CapacitorRow* x) -> size_t {
        return op_loss_mode
                   ? op_ranking::unverified_items(op_loss.at(x).unverified().size(), op_size.at(x))
                   : 0;
    };
    auto fails_ripple = [&](const CapacitorRow* x) -> int {
        return op_loss_mode && op_loss.at(x).fails_ripple_checks() ? 1 : 0;
    };
    auto thermal_tier = [&](const CapacitorRow* x) -> int {
        return !op_loss_mode && x->no_thermal() ? 1 : 0;
    };
    // Gross over-rating (owner 2026-11-14): rated voltage more than kGrossOverRatingFactor (3x)
    // the requirement. Judged on the rated voltage only: the ripple-current axis was dropped
    // (owner 2026-11-14); a ripple rating BELOW the need is the ripple key above, not this.
    struct OverAxis {
        std::string axis;
        double have, need;
    };
    auto over_rated_axes = [&](const CapacitorRow* x) {
        std::vector<OverAxis> axes;
        if (x->v_rated > crossref::kGrossOverRatingFactor * c.v_rated_min)
            axes.push_back({"voltage", x->v_rated, c.v_rated_min});
        return axes;
    };
    // The tier ranks where the ranking does not already weigh the rating (owner 2026-11-14, as
    // for MOSFETs): under highest_voltage_margin / highest_ripple_headroom a higher rating is
    // what was asked for. Unlike MOSFETs and diodes it ALSO ranks under the operating-point
    // loss (owner 2026-11-14): a capacitor's size and cost scale with its rated voltage, so an
    // over-rated part ranks after the UNVERIFIED and ripple keys' passers, before the loss —
    // part of that ranking's definition, so on whether or not the caller asked for the tier.
    const bool over_rating_ranks =
        op_loss_mode || (c.over_rating_tier && tb != CapacitorTiebreaker::HighestVoltageMargin &&
                         tb != CapacitorTiebreaker::HighestRippleHeadroom);
    auto over_rated = [&](const CapacitorRow* x) -> size_t {
        return over_rating_ranks ? over_rated_axes(x).size() : 0;
    };
    std::sort(passing.begin(), passing.end(), [&](const CapacitorRow* a, const CapacitorRow* b) {
        return std::make_tuple(unverified_terms(a), fails_ripple(a), thermal_tier(a),
                               unknown_metric(a), over_rated(a), metric(a), a->lineno) <
               std::make_tuple(unverified_terms(b), fails_ripple(b), thermal_tier(b),
                               unknown_metric(b), over_rated(b), metric(b), b->lineno);
    });

    json result;
    result["category"] = "capacitor";
    result["tiebreaker"] = to_string(tb);
    if (op_loss_mode) result["lossOperatingPoint"] = capacitor_loss_op_json(*c.loss_op);
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
        auto [area, vol] = capacitor_size_json(*x);
        cd["boardArea"] = std::move(area);
        cd["volume"] = std::move(vol);
        cd["price"] = capacitor_price_json(*x, c.price_quantity);
        cd["ratings"] = {{"voltage", x->v_rated},
                         {"capacitance", x->capacitance},
                         {"technology", x->technology},
                         {"dielectric", x->dielectric_code},
                         {"esr", num_or_null(x->esr)},
                         {"esrFrequency", num_or_null(x->esr_frequency)},
                         {"rippleCurrent", num_or_null(x->ripple_current_rms)},
                         {"rippleCurrentFrequency", num_or_null(x->ripple_current_frequency)},
                         {"rippleCurrentTemperature", num_or_null(x->ripple_current_temperature)}};
        if (c.over_rating_tier || op_loss_mode) {
            json orj = {{"axes", json::array()}, {"notes", json::array()}};
            for (const OverAxis& ax : over_rated_axes(x)) {
                char b[512];
                std::snprintf(b, sizeof b,
                              "rated voltage %g V is %.3gx the requirement's %.4g V — more than "
                              "%gx: a bigger body and higher cost are likely; %s",
                              ax.have, ax.have / ax.need, ax.need,
                              crossref::kGrossOverRatingFactor,
                              over_rating_ranks
                                  ? (op_loss_mode ? "ranked after closer-rated parts, ahead of "
                                                    "the operating-point loss (a capacitor's "
                                                    "size and cost scale with its voltage)"
                                                  : "ranked after closer-rated parts")
                                  : "flagged only: this ranking does not demote it");
                orj["axes"].push_back(ax.axis);
                orj["notes"].push_back(b);
            }
            cd["overRating"] = std::move(orj);
        }
        if (op_loss_mode) {
            const CapacitorLossBreakdown& lb = op_loss.at(x);
            cd["loss"] = capacitor_loss_json(lb);
            const op_ranking::SizeEvidence& sz = op_size.at(x);
            if (sz.undetermined) cd["loss"]["notes"].push_back(sz.note);
            cd["sortKey"] = {{"metric", to_string(tb)}, {"value", lb.computed_sum()},
                             {"overRippleVoltage", lb.over_ripple_voltage()},
                             {"underRippleCurrentRating", lb.under_ripple_rating()},
                             {"overRatedVoltage", over_rated(x) > 0}};
            cd["sortKey"].update(op_ranking::sort_key_fields(lb.unverified().size(), sz));
        } else {
            cd["sortKey"] = {{"metric", to_string(tb)}, {"value", metric(x)}};
        }
        cands.push_back(std::move(cd));
    }
    result["candidates"] = std::move(cands);
    return result;
}

// A controller's CATEGORY says what its loop regulates; `intendedTopologies` says only which
// switching stage it can drive. Reading the second as an answer to the first put an offline
// PFC/LED driver on the first line of a 48 -> 12 V buck BOM: NCL30000's datasheet does list
// buckConverter, so it matched on topology, ranked as a "real controller", and won — while
// nothing about a 48 V DC rail asks for power-factor correction. It is a category error, not a
// ranking preference, and it is the part an FAE notices first. (ABT #694.)
//
// A caller that names the category explicitly still gets exactly what it asked for; this
// governs only the inference made on the caller's behalf.
namespace {
bool regulates_output_rail(const std::string& category, const std::string& topo) {
    // Neither of these closes a loop around a converter output at all: a supervisor WATCHES a
    // rail and asserts reset, and a shunt regulator is a two-terminal reference (a TL431).
    // Both carry a reference voltage, which is what `has_vref` rewards, so they rank as
    // credible controllers on the strength of the very field that marks them as not one.
    if (category == "supervisor" || category == "shuntRegulator") return false;
    // Drives the secondary-side rectifier FET of a converter someone else is controlling —
    // an adjunct to a topology, never the loop that sets the output.
    if (category == "syncRectifierController") return false;
    // A PFC controller's loop shapes INPUT current to follow the line voltage; its "output"
    // is a bulk rail held loosely, if at all. That belongs to a design whose stage IS the
    // front end, so it is admitted for the PFC topologies and refused for the rest —
    // including the buck stage its own datasheet correctly says it can drive.
    if (category == "pfcController")
        return topo == "power_factor_correction" || topo == "vienna";
    return true;
}
}  // namespace

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
        // Before the topology list is consulted at all — a category that does not regulate
        // this design's output rail is not made suitable by listing its switching stage.
        const bool category_requested = c.category.has_value() && ctrl.category == *c.category;
        if (!category_requested && !regulates_output_rail(ctrl.category, topo)) {
            rej["category_does_not_regulate_this_rail"]++; continue;
        }
        // For a PFC FRONT END the category is the qualification and the listed topology is
        // just the switching stage that implements it — a boost-PFC controller (UCC3817,
        // L6562, NCP1654, FAN9611 …) records "boostConverter" because that is the stage it
        // drives, and boost IS how PFC is nearly always done. Filtering those on the literal
        // topology string left 9 of the 67 pfcControllers reachable by no design at all: the
        // topology filter kept them out of power_factor_correction and the category gate
        // above kept them out of buck/boost. Whichever way the catalogue spells the stage,
        // a part whose whole purpose is shaping line current belongs to a PFC design.
        const bool pfc_for_pfc =
            ctrl.category == "pfcController" &&
            (topo == "power_factor_correction" || topo == "vienna");
        bool has_topo = !ctrl.topologies.empty() && !pfc_for_pfc;
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
        // A row whose record states no tolerance (NaN) cannot be shown to meet the limit,
        // so it is rejected under its own reason, never passed as if it were tight.
        if (std::isnan(r.tolerance)) { rej["tolerance_unknown"]++; continue; }
        if (r.tolerance > c.max_tolerance) { rej["tolerance_loose"]++; continue; }
        // Not stated (retracted-only, NaN): a NaN deviation would compare false and pass.
        if (!present(r.resistance)) { rej["no_resistance"]++; continue; }
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
        if (c.polarity && b.polarity != *c.polarity) { rej["polarity_mismatch"]++; continue; }
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
// Winding structure: can any of the part's wiring configurations realize the circuit's winding count?
// Structure is not a degree of fit — a single-winding inductor cannot be a flyback transformer, and a
// part whose configurations all carry the wrong number of windings cannot be wired into the slot (the
// engine rejects it at bind time). Würth's own flyback selector treats turns ratios as its one
// strictlyRequired filter for the same reason. Still ranked, never dropped: a structural fail sorts
// behind every part that fits.
int winding_status(const std::optional<int>& want, uint32_t counts) {
    if (!want || *want < 0 || *want >= 32 || counts == 0) return -1;
    return (counts & (1u << *want)) ? 2 : 0;
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
    auto structure_fail = [&](const MagneticRow* m) {
        return winding_status(c.secondary_windings, m->secondary_counts) == 0 ? 1 : 0;
    };

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
    // Ascending tuple: (structural fit, fit penalty, production first, lower DCR, source line) —
    // deterministic.
    std::sort(all.begin(), all.end(), [&](const MagneticRow* a, const MagneticRow* b) {
        return std::make_tuple(structure_fail(a), penalty(a), a->is_production ? 0 : 1, dcr_key(a), a->lineno) <
               std::make_tuple(structure_fail(b), penalty(b), b->is_production ? 0 : 1, dcr_key(b), b->lineno);
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
                        {"secondaryWindings",
                         c.secondary_windings ? json(*c.secondary_windings) : json(nullptr)},
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
        int ws = winding_status(c.secondary_windings, m->secondary_counts);
        int overall = -1;  // worst of the evaluated dimensions; -1 => nothing evaluable
        for (int s : {is, ss, rs, trs, ws})
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
                                    {"turnsRatio", verdict_str(trs)},
                                    {"windingStructure", verdict_str(ws)}};
        json counts = json::array();
        for (int n = 0; n < 32; ++n)
            if (m->secondary_counts & (1u << n)) counts.push_back(n);
        cd["evidence"] = {{"deviceType", m->device_type},
                          {"family", m->family},
                          {"isProduction", m->is_production},
                          {"inductance", num_or_null(m->inductance)},
                          {"saturationCurrent", num_or_null(m->saturation_current)},
                          {"ratedCurrent", num_or_null(m->rated_current)},
                          {"turnsRatio", num_or_null(m->turns_ratio)},
                          {"secondaryWindingCounts", m->secondary_counts ? counts : json(nullptr)},
                          {"dcr", num_or_null(m->dcr)},
                          {"selfResonantFrequency", num_or_null(m->srf)}};
        cd["sortKey"] = {{"metric", "best_fit"}, {"penalty", penalty(m)}};
        cands.push_back(std::move(cd));
    }
    result["candidates"] = std::move(cands);
    return result;
}

// ---- Magnetic: lowest_operating_point_loss (owner 2026-11-14) ------------------------------
namespace {

json mag_check_json(const MagneticCheck& k) {
    json j = {{"status", k.str()}};
    if (k.unverified()) j["missing"] = k.missing;
    else j["basis"] = k.basis;
    return j;
}

json mag_loss_json(const MagneticLossBreakdown& lb, const InductorOperatingPoint& op) {
    json loss;
    loss["position"] = to_string(op.position);
    loss["conductionMode"] = to_string(op.mode);
    loss["dutyCycle"] = lb.duty;
    loss["dutyBasis"] = lb.duty_basis;
    loss["inductanceAtPeak"] = lb.inductance;
    loss["inductanceBasis"] = lb.inductance_basis;
    loss["ripplePeakToPeak"] = lb.ripple_pp;
    loss["peakCurrent"] = lb.ipk;
    loss["peakCurrentBasis"] = lb.ipk_basis;
    loss["rmsCurrent"] = lb.irms;
    loss["rmsCurrentBasis"] = lb.irms_basis;
    if (lb.frequency) {
        loss["switchingFrequency"] = *lb.frequency;
        loss["switchingFrequencyBasis"] = lb.frequency_basis;
    }
    loss["terms"] = {{"dcr", loss_term_json(lb.dcr)}, {"coreLoss", loss_term_json(lb.core)}};
    loss["checks"] = {{"inductance", mag_check_json(lb.inductance_gate)},
                      {"saturation", mag_check_json(lb.saturation)},
                      {"thermal", mag_check_json(lb.thermal)}};
    const auto tot = lb.total();
    loss["total"] = tot ? json(*tot) : json(nullptr);
    loss["computedSum"] = lb.computed_sum();
    loss["unverified"] = lb.unverified();
    loss["notes"] = lb.notes;
    return loss;
}

json mag_loss_op_json(const InductorOperatingPoint& op) {
    json o = {{"position", to_string(op.position)}, {"conductionMode", to_string(op.mode)},
              {"inputVoltage", op.vin}, {"outputVoltage", op.vout}, {"outputCurrent", op.iout}};
    if (op.fsw) o["switchingFrequency"] = *op.fsw;
    if (op.vin_min) o["minimumInputVoltage"] = *op.vin_min;
    if (op.window_min_l) o["minimumInductance"] = *op.window_min_l;
    if (op.window_max_l) o["maximumInductance"] = *op.window_max_l;
    if (op.mode == ConductionMode::Dcm) {
        const DcmBoundary d = dcm_maximum_inductance(op);
        o["dcmMaximumInductance"] = d.l_max;
        o["dcmMaximumInductanceBasis"] = d.basis;
    }
    if (op.ambient) o["ambientTemperature"] = *op.ambient;
    return o;
}

// Board area of a magnetic: its record's DRAWN length x width only (dims_of never sizes a
// magnetic from a case code), mm^2.
json mag_board_area_json(const MagneticRow& m) {
    json p = json::object();
    if (present(m.length_m)) p["length_m"] = m.length_m;
    if (present(m.width_m)) p["width_m"] = m.width_m;
    if (present(m.height_m)) p["height_m"] = m.height_m;
    if (!m.case_code.empty()) p["case_code"] = m.case_code;
    if (!m.mount.empty()) p["mount"] = m.mount;
    const auto d = crossref::dims_of(p, "magnetic");
    if (!d || !crossref::has_land(*d))
        return {{"mm2", nullptr},
                {"basis", "not stated: the record draws no length x width (a magnetic is never "
                          "sized from its case code)"}};
    return {{"mm2", *d->length * *d->width * 1e6},
            {"basis", "the record's drawn length x width"}};
}

// Volume of a magnetic (owner 2026-11-14: equal billing with the board area): its record's
// DRAWN length x width x height, the bounding box, mm^3.
json mag_volume_json(const MagneticRow& m) {
    std::vector<std::string> miss;
    if (!present(m.length_m)) miss.push_back("length");
    if (!present(m.width_m)) miss.push_back("width");
    if (!present(m.height_m)) miss.push_back("height");
    if (!miss.empty()) {
        std::string w;
        for (const auto& x : miss) w += (w.empty() ? "" : ", ") + x;
        return {{"mm3", nullptr},
                {"basis", "not stated: the record draws no " + w +
                              " (a magnetic is never sized from its case code)"}};
    }
    return {{"mm3", m.length_m * m.width_m * m.height_m * 1e9},
            {"basis", "the record's drawn length x width x height (bounding box)"}};
}

json mag_price_json(const MagneticRow& m, std::optional<double> quantity) {
    json p;
    p["quantity"] = quantity ? json(*quantity) : json(nullptr);
    p["atQuantity"] = nullptr;
    p["quotes"] = m.price_quotes.empty() ? json::array() : json::parse(m.price_quotes);
    p["basis"] = m.price_quotes.empty()
                     ? "not stated: the record carries no distributor cost"
                     : "unit cost as the distributor states it; the record carries no price "
                       "breaks, so the price at a given quantity is not stated";
    return p;
}

}  // namespace

json select_magnetic_op_loss(const Shard<MagneticRow>& shard, const MagneticConstraints& c,
                             size_t max_candidates, RecordFetcher* fetcher, const MfrPolicy& mfr) {
    if (!c.loss_op)
        throw InvalidOptions("lowest_operating_point_loss requires a lossOperatingPoint "
                             "(position, inputVoltage, outputVoltage, outputCurrent, "
                             "switchingFrequency)");
    c.loss_op->validate();
    const InductorOperatingPoint& op = *c.loss_op;
    // ccm gates on the required (minimum) inductance; dcm and crm gate on the operating
    // point's own limits and do not read it.
    std::optional<double> L_req;
    if (op.mode == ConductionMode::Ccm) {
        if (!c.target_inductance || !(*c.target_inductance > 0))
            throw InvalidOptions("lowest_operating_point_loss in conductionMode ccm requires the "
                                 "required inductance (designRequirements.magnetizingInductance)");
        L_req = c.target_inductance;
    }
    // The rejection count of each mode's inductance gate.
    const char* gate_rej = op.mode == ConductionMode::Ccm   ? "inductance_below_required"
                           : op.mode == ConductionMode::Dcm ? "inductance_above_dcm_maximum"
                                                            : "inductance_outside_crm_window";

    std::map<std::string, uint64_t> rej;
    rej["unreadable_row"] = shard.meta.unreadable_row_count;
    std::vector<const MagneticRow*> passing;
    std::map<const MagneticRow*, MagneticLossBreakdown> loss;
    for (const auto& m : shard.rows) {
        if (c.exclude_discontinued && !m.is_production) { rej["discontinued"]++; continue; }
        // Power inductors only: a transformer, coupled inductor, common-mode choke, bead or
        // cable core is not a buck inductor, whatever its inductance.
        if (m.device_type != "inductor") {
            rej["subtype_" + (m.device_type.empty() ? std::string("unstated") : m.device_type)]++;
            continue;
        }
        // A part whose every stated wiring configuration has secondaries is not a
        // single-winding inductor (0 = the record states no structure: not a mismatch).
        if (m.secondary_counts != 0 && !(m.secondary_counts & 1u)) {
            rej["no_single_winding_configuration"]++;
            continue;
        }
        if (!present(m.inductance) || !(m.inductance > 0)) { rej["inductance_unstated"]++; continue; }
        // ccm gates on the required L; dcm / crm only note the distance from it.
        MagneticLossBreakdown lb = magnetic_loss(m, op, L_req ? L_req : c.target_inductance);
        if (lb.inductance_gate.failed()) { rej[gate_rej]++; continue; }
        if (lb.saturation.failed()) { rej["saturation_current_below_peak"]++; continue; }
        if (lb.thermal.failed()) { rej["rated_current_below_rms"]++; continue; }
        loss.emplace(&m, std::move(lb));
        passing.push_back(&m);
    }
    if (passing.empty())
        throw NoCandidates("MagneticConstraints", emit_rejections(rej),
                           shard.meta.source_line_count);

    // Total loss alone orders, after the UNVERIFIED key (owner 2026-11-14). The core-loss term
    // is UNVERIFIED for every record and is left out of both keys (interim owner rule). A
    // candidate whose board area or volume cannot be determined counts one more UNVERIFIED
    // item (owner 2026-11-14, every family: op_ranking, read from the JSON the candidate shows).
    std::map<const MagneticRow*, op_ranking::SizeEvidence> op_size;
    std::map<const MagneticRow*, std::size_t> unv;
    for (const MagneticRow* m : passing) {
        const json area = mag_board_area_json(*m), vol = mag_volume_json(*m);
        op_size.emplace(m, op_ranking::size_of(area, &vol));
        unv[m] = op_ranking::unverified_items(loss.at(m).rank_unverified(), op_size.at(m));
    }
    std::sort(passing.begin(), passing.end(), [&](const MagneticRow* a, const MagneticRow* b) {
        return std::make_tuple(unv.at(a), loss.at(a).computed_sum(), a->lineno) <
               std::make_tuple(unv.at(b), loss.at(b).computed_sum(), b->lineno);
    });

    json result;
    result["category"] = "magnetic";
    result["tiebreaker"] = to_string(MagneticTiebreaker::LowestOperatingPointLoss);
    result["lossOperatingPoint"] = mag_loss_op_json(op);
    result["target"] = {{"inductance", L_req ? json(*L_req) : json(nullptr)}};
    if (op.mode != ConductionMode::Ccm && c.target_inductance)
        result["notes"] = json::array(
            {std::string("the required inductance stated in the requirements is not a gate in "
                         "conductionMode ") +
             to_string(op.mode) + ": the operating point's own limits are"});
    result["totalRowsConsidered"] = shard.meta.source_line_count;
    result["alternativesConsidered"] = passing.size();
    result["rejections"] = emit_rejections(rej);
    result["manufacturers"] = manufacturer_facet(passing);
    json cands = json::array();
    const auto chosen = apply_mfr_policy(passing, max_candidates, mfr);
    for (const MagneticRow* m : chosen) {
        const MagneticLossBreakdown& lb = loss.at(m);
        json cd = base_candidate(m, fetcher);
        cd["loss"] = mag_loss_json(lb, op);
        const op_ranking::SizeEvidence& sz = op_size.at(m);
        if (sz.undetermined) cd["loss"]["notes"].push_back(sz.note);
        cd["ratings"] = {{"inductance", m->inductance},
                         {"inductanceTolerance",
                          present(m->inductance_tol_minus_pct)
                              ? json{{"minusPercent", m->inductance_tol_minus_pct},
                                     {"plusPercent", m->inductance_tol_plus_pct}}
                              : json(nullptr)},
                         {"saturationCurrent", num_or_null(m->saturation_current)},
                         {"saturationPercentDrop", num_or_null(m->saturation_percent_drop)},
                         {"ratedCurrent", num_or_null(m->rated_current)},
                         {"dcr", num_or_null(m->dcr)},
                         {"dcrIsMaximum", present(m->dcr) ? json(m->dcr_is_maximum) : json(nullptr)}};
        // Over-rating (flag + note only under this ranking, owner 2026-11-14): Isat against
        // Ipk, the rated current against Irms.
        json orj = {{"axes", json::array()}, {"notes", json::array()}};
        auto flag = [&](const char* axis, const char* what, double have, double need,
                        const char* need_name) {
            if (!(present(have) && have > crossref::kGrossOverRatingFactor * need)) return;
            char b[400];
            std::snprintf(b, sizeof b,
                          "%s %.4g A is %.3gx the operating point's %s %.4g A — more than %gx: "
                          "a bigger body and higher cost are likely; flagged only: the "
                          "operating-point loss alone sets the order",
                          what, have, have / need, need_name, need,
                          crossref::kGrossOverRatingFactor);
            orj["axes"].push_back(axis);
            orj["notes"].push_back(b);
        };
        flag("isat", "saturation current", m->saturation_current, lb.ipk, "Ipk");
        flag("ratedCurrent", "rated current", m->rated_current, lb.irms, "Irms");
        if (lb.over_sized) {
            orj["axes"].push_back("inductance");
            orj["notes"].push_back(lb.over_sized_note);
        }
        cd["overRating"] = std::move(orj);
        cd["boardArea"] = mag_board_area_json(*m);
        cd["volume"] = mag_volume_json(*m);
        cd["price"] = mag_price_json(*m, c.price_quantity);
        cd["evidence"] = {{"deviceType", m->device_type},
                          {"family", m->family},
                          {"isProduction", m->is_production},
                          {"hasBiasCurve", !m->bias_curve_current.empty()}};
        // unverifiedTerms counts the UNVERIFIED checks and loss terms the ranking reads
        // (rank_unverified: core loss left out); unverifiedItems adds the size item.
        cd["sortKey"] = {{"metric", "lowest_operating_point_loss"}, {"value", lb.computed_sum()}};
        cd["sortKey"].update(op_ranking::sort_key_fields(lb.rank_unverified(), sz));
        cands.push_back(std::move(cd));
    }
    result["candidates"] = std::move(cands);
    return result;
}

}  // namespace kelvin
