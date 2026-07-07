// Kelvin browse: deterministic filter / sort / facet / paginate over one family's shard rows —
// the catalogue-browsing surface for the Kelvin web frontend (kelvin.openconverters.com).
//
// Pure shard-in / json-out, no selection semantics: a browse filter is an explicit user filter
// (a part with the datum absent is excluded when you filter on that datum), unlike the magnetic
// selector's rank-not-gate. Facet counts and numeric ranges follow the standard faceted-search
// rule: each facet is counted over the rows matching every OTHER filter, so multi-selecting
// within one facet or widening a range never zeroes its own option list.
//
// Query (all keys optional):
//   { "filters": { "<numField>": {"min":x,"max":y}, "<strField>": ["v1","v2"],
//                  "<boolField>": true, "mpn": "substr" },
//     "sort": {"field": "<numField>|mpn|manufacturer|lineno", "dir": "asc"|"desc"},
//     "offset": 0, "limit": 50, "withFacets": false, "facetTop": 100 }
// Unknown filter/sort fields throw InvalidOptions (no silent skips).
//
// Result:
//   { "family", "rowCount", "total", "rows": [ {mpn, manufacturer, lineno, srcOffset,
//     srcLength, <fields...>} ], and with withFacets: "facets": { "<strField>":
//     {"values": [[value,count]...], "omitted": N}, ... }, "ranges": { "<numField>":
//     {"min", "max", "present"} } }
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "Constraints.hpp"  // InvalidOptions
#include "Index.hpp"

namespace kelvin {
namespace browse {

using json = nlohmann::json;

// ---- per-family field tables (pointer-to-member, so one engine walks every family) ----------
template <class Row>
struct FieldTable {
    std::vector<std::pair<const char*, double Row::*>> nums;
    std::vector<std::pair<const char*, std::string Row::*>> strs;   // facetable
    std::vector<std::pair<const char*, bool Row::*>> bools;
    std::vector<std::pair<const char*, std::vector<std::string> Row::*>> lists;  // facetable
};

template <class Row>
const FieldTable<Row>& fields();

template <>
inline const FieldTable<MosfetRow>& fields<MosfetRow>() {
    static const FieldTable<MosfetRow> t{
        {{"vds_rated", &MosfetRow::vds_rated},
         {"id_continuous", &MosfetRow::id_continuous},
         {"rds_on", &MosfetRow::rds_on},
         {"qg_total", &MosfetRow::qg_total},
         {"coss", &MosfetRow::coss},
         {"vgs_threshold_max", &MosfetRow::vgs_threshold_max},
         {"rth_ja", &MosfetRow::rth_ja},
         {"rth_jc", &MosfetRow::rth_jc},
         {"tj_max", &MosfetRow::tj_max}},
        {{"technology", &MosfetRow::technology}},
        {{"is_production", &MosfetRow::is_production},
         {"datasheet_usable", &MosfetRow::datasheet_usable}},
        {}};
    return t;
}
template <>
inline const FieldTable<DiodeRow>& fields<DiodeRow>() {
    static const FieldTable<DiodeRow> t{
        {{"vrrm_rated", &DiodeRow::vrrm_rated},
         {"if_avg_rated", &DiodeRow::if_avg_rated},
         {"vf_typ", &DiodeRow::vf_typ},
         {"qrr", &DiodeRow::qrr},
         {"trr", &DiodeRow::trr},
         {"rth_ja", &DiodeRow::rth_ja},
         {"rth_jc", &DiodeRow::rth_jc},
         {"tj_max", &DiodeRow::tj_max}},
        {{"technology", &DiodeRow::technology}},
        {{"is_production", &DiodeRow::is_production}},
        {}};
    return t;
}
template <>
inline const FieldTable<CapacitorRow>& fields<CapacitorRow>() {
    static const FieldTable<CapacitorRow> t{
        {{"capacitance", &CapacitorRow::capacitance},
         {"v_rated", &CapacitorRow::v_rated},
         {"ripple_current_rms", &CapacitorRow::ripple_current_rms},
         {"esr", &CapacitorRow::esr},
         {"rth", &CapacitorRow::rth}},
        {{"technology", &CapacitorRow::technology}},
        {{"is_production", &CapacitorRow::is_production}},
        {}};
    return t;
}
template <>
inline const FieldTable<ResistorRow>& fields<ResistorRow>() {
    static const FieldTable<ResistorRow> t{
        {{"resistance", &ResistorRow::resistance},
         {"tolerance", &ResistorRow::tolerance},
         {"power_rating", &ResistorRow::power_rating}},
        {},
        {{"is_production", &ResistorRow::is_production}},
        {}};
    return t;
}
template <>
inline const FieldTable<ControllerRow>& fields<ControllerRow>() {
    static const FieldTable<ControllerRow> t{
        {{"vref", &ControllerRow::vref}},
        {{"category", &ControllerRow::category}},
        {{"integrated_fet", &ControllerRow::integrated_fet},
         {"integrated_driver", &ControllerRow::integrated_driver}},
        {{"topologies", &ControllerRow::topologies}}};
    return t;
}
template <>
inline const FieldTable<IgbtRow>& fields<IgbtRow>() {
    static const FieldTable<IgbtRow> t{
        {{"vces_rated", &IgbtRow::vces_rated},
         {"ic_continuous", &IgbtRow::ic_continuous},
         {"vce_sat", &IgbtRow::vce_sat},
         {"rth_jc", &IgbtRow::rth_jc},
         {"tj_max", &IgbtRow::tj_max}},
        {{"technology", &IgbtRow::technology}},
        {{"is_production", &IgbtRow::is_production}},
        {}};
    return t;
}
template <>
inline const FieldTable<BjtRow>& fields<BjtRow>() {
    static const FieldTable<BjtRow> t{
        {{"vceo_rated", &BjtRow::vceo_rated},
         {"ic_continuous", &BjtRow::ic_continuous},
         {"hfe_min", &BjtRow::hfe_min},
         {"power_dissipation", &BjtRow::power_dissipation},
         {"tj_max", &BjtRow::tj_max}},
        {{"technology", &BjtRow::technology}},
        {{"is_production", &BjtRow::is_production}},
        {}};
    return t;
}
template <>
inline const FieldTable<VaristorRow>& fields<VaristorRow>() {
    static const FieldTable<VaristorRow> t{
        {{"varistor_voltage", &VaristorRow::varistor_voltage},
         {"clamping_voltage", &VaristorRow::clamping_voltage},
         {"peak_surge_current", &VaristorRow::peak_surge_current},
         {"max_continuous_dc_voltage", &VaristorRow::max_continuous_dc_voltage},
         {"capacitance", &VaristorRow::capacitance}},
        {{"technology", &VaristorRow::technology}},
        {{"is_production", &VaristorRow::is_production}},
        {}};
    return t;
}
template <>
inline const FieldTable<MagneticRow>& fields<MagneticRow>() {
    static const FieldTable<MagneticRow> t{
        {{"inductance", &MagneticRow::inductance},
         {"saturation_current", &MagneticRow::saturation_current},
         {"rated_current", &MagneticRow::rated_current},
         {"dcr", &MagneticRow::dcr},
         {"srf", &MagneticRow::srf},
         {"turns_ratio", &MagneticRow::turns_ratio}},
        {{"device_type", &MagneticRow::device_type}, {"family", &MagneticRow::family}},
        {{"is_production", &MagneticRow::is_production}},
        {}};
    return t;
}

// ---- parsed query -----------------------------------------------------------------------------
namespace detail {

struct NumRange {
    double min = -std::numeric_limits<double>::infinity();
    double max = std::numeric_limits<double>::infinity();
};

inline std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

template <class Row>
struct Query {
    std::map<size_t, NumRange> num_filters;                    // index into table.nums
    std::map<size_t, std::vector<std::string>> str_filters;    // index into table.strs
    std::map<size_t, bool> bool_filters;                       // index into table.bools
    std::map<size_t, std::vector<std::string>> list_filters;   // index into table.lists
    std::vector<std::string> manufacturers;                    // exact values, empty = all
    std::string mpn_substr;                                    // lowercased, empty = none
    int sort_num = -1;                                         // index into table.nums, or:
    enum class SortKey { Lineno, Mpn, Manufacturer, Num } sort_key = SortKey::Lineno;
    bool sort_desc = false;
    size_t offset = 0;
    size_t limit = 50;
    bool with_facets = false;
    size_t facet_top = 100;
};

template <class Row>
Query<Row> parse_query(const json& q) {
    const FieldTable<Row>& t = fields<Row>();
    Query<Row> out;
    if (!q.is_object() && !q.is_null())
        throw InvalidOptions("browse: query must be an object");
    if (q.is_null()) return out;

    auto num_index = [&](const std::string& name) -> int {
        for (size_t i = 0; i < t.nums.size(); ++i)
            if (name == t.nums[i].first) return static_cast<int>(i);
        return -1;
    };
    auto str_index = [&](const std::string& name) -> int {
        for (size_t i = 0; i < t.strs.size(); ++i)
            if (name == t.strs[i].first) return static_cast<int>(i);
        return -1;
    };
    auto bool_index = [&](const std::string& name) -> int {
        for (size_t i = 0; i < t.bools.size(); ++i)
            if (name == t.bools[i].first) return static_cast<int>(i);
        return -1;
    };
    auto list_index = [&](const std::string& name) -> int {
        for (size_t i = 0; i < t.lists.size(); ++i)
            if (name == t.lists[i].first) return static_cast<int>(i);
        return -1;
    };
    auto str_values = [](const json& v, const std::string& key) {
        if (!v.is_array()) throw InvalidOptions("browse: filter '" + key + "' must be an array");
        std::vector<std::string> vals;
        for (const auto& e : v) {
            if (!e.is_string())
                throw InvalidOptions("browse: filter '" + key + "' values must be strings");
            vals.push_back(e.get<std::string>());
        }
        return vals;
    };

    if (q.contains("filters")) {
        const json& f = q.at("filters");
        if (!f.is_object()) throw InvalidOptions("browse: filters must be an object");
        for (auto it = f.begin(); it != f.end(); ++it) {
            const std::string& key = it.key();
            const json& v = it.value();
            if (key == "mpn") {
                if (!v.is_string()) throw InvalidOptions("browse: filter 'mpn' must be a string");
                out.mpn_substr = lower(v.get<std::string>());
            } else if (key == "manufacturer") {
                out.manufacturers = str_values(v, key);
            } else if (int i = num_index(key); i >= 0) {
                if (!v.is_object())
                    throw InvalidOptions("browse: filter '" + key + "' must be {min,max}");
                NumRange r;
                if (v.contains("min")) {
                    if (!v.at("min").is_number())
                        throw InvalidOptions("browse: filter '" + key + "'.min must be a number");
                    r.min = v.at("min").get<double>();
                }
                if (v.contains("max")) {
                    if (!v.at("max").is_number())
                        throw InvalidOptions("browse: filter '" + key + "'.max must be a number");
                    r.max = v.at("max").get<double>();
                }
                out.num_filters[static_cast<size_t>(i)] = r;
            } else if (int i2 = str_index(key); i2 >= 0) {
                out.str_filters[static_cast<size_t>(i2)] = str_values(v, key);
            } else if (int i3 = bool_index(key); i3 >= 0) {
                if (!v.is_boolean())
                    throw InvalidOptions("browse: filter '" + key + "' must be a boolean");
                out.bool_filters[static_cast<size_t>(i3)] = v.get<bool>();
            } else if (int i4 = list_index(key); i4 >= 0) {
                out.list_filters[static_cast<size_t>(i4)] = str_values(v, key);
            } else {
                throw InvalidOptions("browse: unknown filter field '" + key + "'");
            }
        }
    }
    if (q.contains("sort")) {
        const json& s = q.at("sort");
        if (!s.is_object() || !s.contains("field") || !s.at("field").is_string())
            throw InvalidOptions("browse: sort must be {field, dir}");
        std::string fname = s.at("field").get<std::string>();
        if (s.contains("dir")) {
            std::string dir = s.at("dir").is_string() ? s.at("dir").get<std::string>() : "";
            if (dir != "asc" && dir != "desc")
                throw InvalidOptions("browse: sort.dir must be 'asc' or 'desc'");
            out.sort_desc = dir == "desc";
        }
        using SK = typename Query<Row>::SortKey;
        if (fname == "lineno") out.sort_key = SK::Lineno;
        else if (fname == "mpn") out.sort_key = SK::Mpn;
        else if (fname == "manufacturer") out.sort_key = SK::Manufacturer;
        else if (int i = num_index(fname); i >= 0) { out.sort_key = SK::Num; out.sort_num = i; }
        else throw InvalidOptions("browse: unknown sort field '" + fname + "'");
    }
    if (q.contains("offset")) {
        if (!q.at("offset").is_number_integer() || q.at("offset").get<int64_t>() < 0)
            throw InvalidOptions("browse: offset must be a non-negative integer");
        out.offset = q.at("offset").get<size_t>();
    }
    if (q.contains("limit")) {
        if (!q.at("limit").is_number_integer() || q.at("limit").get<int64_t>() < 0)
            throw InvalidOptions("browse: limit must be a non-negative integer");
        out.limit = std::min<size_t>(q.at("limit").get<size_t>(), 1000);
    }
    if (q.contains("withFacets")) {
        if (!q.at("withFacets").is_boolean())
            throw InvalidOptions("browse: withFacets must be a boolean");
        out.with_facets = q.at("withFacets").get<bool>();
    }
    if (q.contains("facetTop")) {
        if (!q.at("facetTop").is_number_integer() || q.at("facetTop").get<int64_t>() < 1)
            throw InvalidOptions("browse: facetTop must be a positive integer");
        out.facet_top = q.at("facetTop").get<size_t>();
    }
    return out;
}

// Skip codes for the counted-over-all-other-filters facet rule. kSkipNone applies every filter.
enum : int { kSkipNone = -1 };
struct Skip {
    // exactly one of these is >= 0 (or none for full matching); kind: 0 num, 1 str, 2 list, 3 mfr
    int kind = -1;
    size_t index = 0;
};

template <class Row>
bool matches(const Row& row, const FieldTable<Row>& t, const Query<Row>& q, const Skip& skip) {
    if (!q.mpn_substr.empty()) {
        if (lower(row.mpn).find(q.mpn_substr) == std::string::npos) return false;
    }
    if (!(skip.kind == 3) && !q.manufacturers.empty()) {
        bool any = false;
        for (const auto& m : q.manufacturers)
            if (row.manufacturer == m) { any = true; break; }
        if (!any) return false;
    }
    for (const auto& [i, r] : q.num_filters) {
        if (skip.kind == 0 && skip.index == i) continue;
        double v = row.*(t.nums[i].second);
        if (std::isnan(v) || v < r.min || v > r.max) return false;
    }
    for (const auto& [i, vals] : q.str_filters) {
        if (skip.kind == 1 && skip.index == i) continue;
        const std::string& v = row.*(t.strs[i].second);
        bool any = false;
        for (const auto& s : vals)
            if (v == s) { any = true; break; }
        if (!any) return false;
    }
    for (const auto& [i, want] : q.bool_filters) {
        if ((row.*(t.bools[i].second)) != want) return false;
    }
    for (const auto& [i, vals] : q.list_filters) {
        if (skip.kind == 2 && skip.index == i) continue;
        const std::vector<std::string>& have = row.*(t.lists[i].second);
        bool any = false;
        for (const auto& s : vals)
            if (std::find(have.begin(), have.end(), s) != have.end()) { any = true; break; }
        if (!any) return false;
    }
    return true;
}

// value->count, emitted as [[value,count]...] sorted count desc then value asc, capped at top
// with the number of omitted DISTINCT values reported (never silently truncated).
inline json facet_json(const std::map<std::string, uint64_t>& counts, size_t top) {
    std::vector<std::pair<std::string, uint64_t>> v(counts.begin(), counts.end());
    std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    json values = json::array();
    size_t n = std::min(v.size(), top);
    for (size_t i = 0; i < n; ++i) values.push_back(json::array({v[i].first, v[i].second}));
    return json{{"values", values}, {"omitted", v.size() - n}};
}

}  // namespace detail

// ---- the browse engine ------------------------------------------------------------------------
template <class Row>
json browse_rows(const Shard<Row>& shard, const json& query) {
    const FieldTable<Row>& t = fields<Row>();
    detail::Query<Row> q = detail::parse_query<Row>(query);

    std::vector<size_t> matched;
    matched.reserve(shard.rows.size() / 4 + 16);
    for (size_t i = 0; i < shard.rows.size(); ++i)
        if (detail::matches(shard.rows[i], t, q, detail::Skip{})) matched.push_back(i);

    using SK = typename detail::Query<Row>::SortKey;
    if (q.sort_key != SK::Lineno || q.sort_desc) {
        const auto& rows = shard.rows;
        std::stable_sort(matched.begin(), matched.end(), [&](size_t a, size_t b) {
            const Row& ra = rows[a];
            const Row& rb = rows[b];
            int cmp = 0;
            if (q.sort_key == SK::Num) {
                double va = ra.*(t.nums[q.sort_num].second);
                double vb = rb.*(t.nums[q.sort_num].second);
                bool na = std::isnan(va), nb = std::isnan(vb);
                if (na != nb) return nb;  // NaN sorts last regardless of direction
                if (!na) cmp = va < vb ? -1 : (va > vb ? 1 : 0);
            } else if (q.sort_key == SK::Mpn) {
                cmp = ra.mpn.compare(rb.mpn) < 0 ? -1 : (ra.mpn == rb.mpn ? 0 : 1);
            } else if (q.sort_key == SK::Manufacturer) {
                cmp = ra.manufacturer.compare(rb.manufacturer) < 0
                          ? -1
                          : (ra.manufacturer == rb.manufacturer ? 0 : 1);
            } else {  // Lineno desc
                cmp = ra.lineno < rb.lineno ? -1 : (ra.lineno == rb.lineno ? 0 : 1);
            }
            if (q.sort_desc) cmp = -cmp;
            if (cmp != 0) return cmp < 0;
            return ra.lineno < rb.lineno;
        });
    }

    json rows = json::array();
    for (size_t k = q.offset; k < matched.size() && rows.size() < q.limit; ++k) {
        const Row& r = shard.rows[matched[k]];
        json o{{"mpn", r.mpn},
               {"manufacturer", r.manufacturer},
               {"lineno", r.lineno},
               {"srcOffset", r.src_offset},
               {"srcLength", r.src_length}};
        for (const auto& [name, mem] : t.nums) {
            double v = r.*mem;
            if (std::isnan(v)) o[name] = nullptr;
            else o[name] = v;
        }
        for (const auto& [name, mem] : t.strs) o[name] = r.*mem;
        for (const auto& [name, mem] : t.bools) o[name] = r.*mem;
        for (const auto& [name, mem] : t.lists) o[name] = r.*mem;
        rows.push_back(std::move(o));
    }

    json out{{"family", family_name(shard.meta.family)},
             {"rowCount", shard.meta.row_count},
             {"total", matched.size()},
             {"rows", std::move(rows)}};

    if (q.with_facets) {
        json facets = json::object();
        {  // manufacturer facet (counted over all other filters)
            std::map<std::string, uint64_t> counts;
            for (const auto& r : shard.rows)
                if (detail::matches(r, t, q, detail::Skip{3, 0})) ++counts[r.manufacturer];
            facets["manufacturer"] = detail::facet_json(counts, q.facet_top);
        }
        for (size_t i = 0; i < t.strs.size(); ++i) {
            std::map<std::string, uint64_t> counts;
            for (const auto& r : shard.rows)
                if (detail::matches(r, t, q, detail::Skip{1, i})) ++counts[r.*(t.strs[i].second)];
            facets[t.strs[i].first] = detail::facet_json(counts, q.facet_top);
        }
        for (size_t i = 0; i < t.lists.size(); ++i) {
            std::map<std::string, uint64_t> counts;
            for (const auto& r : shard.rows)
                if (detail::matches(r, t, q, detail::Skip{2, i}))
                    for (const auto& v : r.*(t.lists[i].second)) ++counts[v];
            facets[t.lists[i].first] = detail::facet_json(counts, q.facet_top);
        }
        out["facets"] = std::move(facets);

        json ranges = json::object();
        for (size_t i = 0; i < t.nums.size(); ++i) {
            double lo = std::numeric_limits<double>::infinity();
            double hi = -std::numeric_limits<double>::infinity();
            uint64_t present = 0;
            for (const auto& r : shard.rows) {
                if (!detail::matches(r, t, q, detail::Skip{0, i})) continue;
                double v = r.*(t.nums[i].second);
                if (std::isnan(v)) continue;
                ++present;
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            json entry{{"present", present}};
            if (present > 0) {
                entry["min"] = lo;
                entry["max"] = hi;
            }
            ranges[t.nums[i].first] = std::move(entry);
        }
        out["ranges"] = std::move(ranges);
    }
    return out;
}

}  // namespace browse
}  // namespace kelvin
