#include "MfrPolicy.hpp"
#include "KelvinApi.hpp"

#include <sys/stat.h>

#include <functional>
#include <initializer_list>
#include <iostream>

#include "Browse.hpp"
#include "Constraints.hpp"
#include "Requirements.hpp"
#include "Select.hpp"
#include "Views.hpp"

namespace kelvin {
namespace api {
namespace {

bool file_exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && (st.st_mode & S_IFREG);
}

std::optional<double> opt_num(const json& o, const char* key) {
    if (o.is_object() && o.contains(key) && o.at(key).is_number()) return o.at(key).get<double>();
    return std::nullopt;
}
bool opt_bool(const json& o, const char* key, bool dflt) {
    if (o.is_object() && o.contains(key) && o.at(key).is_boolean()) return o.at(key).get<bool>();
    return dflt;
}
std::optional<std::string> opt_str(const json& o, const char* key) {
    if (o.is_object() && o.contains(key) && o.at(key).is_string())
        return o.at(key).get<std::string>();
    return std::nullopt;
}
size_t opt_size(const json& o, const char* key, size_t dflt) {
    if (o.is_object() && o.contains(key) && o.at(key).is_number_unsigned())
        return o.at(key).get<size_t>();
    if (o.is_object() && o.contains(key) && o.at(key).is_number_integer()) {
        auto v = o.at(key).get<long long>();
        if (v > 0) return static_cast<size_t>(v);
    }
    return dflt;
}

// switching frequency (Hz) from options: opFsw | switchingFrequency | operatingPoint.fsw
std::optional<double> op_fsw_of(const json& options) {
    if (auto v = opt_num(options, "opFsw")) return v;
    if (auto v = opt_num(options, "switchingFrequency")) return v;
    if (options.is_object() && options.contains("operatingPoint"))
        if (auto v = opt_num(options.at("operatingPoint"), "fsw")) return v;
    return std::nullopt;
}

}  // namespace

Family family_from_string(const std::string& s) {
    if (s == "mosfet" || s == "mosfets") return Family::Mosfet;
    if (s == "diode" || s == "diodes") return Family::Diode;
    if (s == "capacitor" || s == "capacitors") return Family::Capacitor;
    if (s == "resistor" || s == "resistors") return Family::Resistor;
    if (s == "controller" || s == "controllers") return Family::Controller;
    if (s == "igbt" || s == "igbts") return Family::Igbt;
    if (s == "bjt" || s == "bjts") return Family::Bjt;
    if (s == "varistor" || s == "varistors") return Family::Varistor;
    if (s == "magnetic" || s == "magnetics") return Family::Magnetic;
    if (s == "analog" || s == "analog_ics" || s == "analog_ic") return Family::Analog;
    if (s == "timing" || s == "timing_devices" || s == "timing_device" || s == "timebase")
        return Family::Timing;
    if (s == "connector" || s == "connectors") return Family::Connector;
    throw InvalidOptions("unknown category: " + s);
}

Engine::Engine(std::string data_dir, std::string cache_dir, bool quiet)
    : data_dir_(std::move(data_dir)), cache_dir_(std::move(cache_dir)), quiet_(quiet) {}

std::string Engine::ndjson_path(Family f) const {
    return data_dir_ + "/" + family_file(f);
}
std::string Engine::shard_path(Family f) const {
    return cache_dir_ + "/" + std::string(family_name(f)) + ".kidx";
}

// Generic load-or-build for one family. Persists to cache_dir_ when set; rebuilds (incremental
// when the file is an append-only extension) when the shard is stale.
namespace {
template <class Row, class Build, class Read, class Write>
Shard<Row> load_or_build(const std::string& ndjson, const std::string& shard,
                         bool has_cache, bool quiet, const char* fam_name, Build build, Read read,
                         Write write) {
    if (!has_cache) return build(ndjson, nullptr);  // in-memory, no persistence

    std::optional<Shard<Row>> prev;
    if (file_exists(shard)) {
        try {
            prev = read(shard);
        } catch (const std::exception&) {
            prev.reset();  // corrupt/old-format shard -> rebuild from scratch
        }
    }
    if (prev && !shard_is_stale(prev->meta, ndjson)) return std::move(*prev);

    if (!quiet)
        std::cerr << "[kelvin] (re)building " << fam_name << " index from " << ndjson << std::endl;
    Shard<Row> s = build(ndjson, prev ? &*prev : nullptr);
    write(shard, s);
    return s;
}
}  // namespace

const Shard<MosfetRow>& Engine::mosfet_shard() {
    if (!mosfet_)
        mosfet_ = load_or_build<MosfetRow>(
            ndjson_path(Family::Mosfet), shard_path(Family::Mosfet), !cache_dir_.empty(), quiet_,
            "mosfet",
            [](const std::string& p, const Shard<MosfetRow>* pv) { return build_mosfet_shard(p, pv); },
            [](const std::string& p) { return read_mosfet_shard(p); },
            [](const std::string& p, const Shard<MosfetRow>& s) { write_shard(p, s); });
    return *mosfet_;
}
const Shard<DiodeRow>& Engine::diode_shard() {
    if (!diode_)
        diode_ = load_or_build<DiodeRow>(
            ndjson_path(Family::Diode), shard_path(Family::Diode), !cache_dir_.empty(), quiet_,
            "diode",
            [](const std::string& p, const Shard<DiodeRow>* pv) { return build_diode_shard(p, pv); },
            [](const std::string& p) { return read_diode_shard(p); },
            [](const std::string& p, const Shard<DiodeRow>& s) { write_shard(p, s); });
    return *diode_;
}
const Shard<CapacitorRow>& Engine::capacitor_shard() {
    if (!capacitor_)
        capacitor_ = load_or_build<CapacitorRow>(
            ndjson_path(Family::Capacitor), shard_path(Family::Capacitor), !cache_dir_.empty(),
            quiet_, "capacitor",
            [](const std::string& p, const Shard<CapacitorRow>* pv) {
                return build_capacitor_shard(p, pv);
            },
            [](const std::string& p) { return read_capacitor_shard(p); },
            [](const std::string& p, const Shard<CapacitorRow>& s) { write_shard(p, s); });
    return *capacitor_;
}
const Shard<ResistorRow>& Engine::resistor_shard() {
    if (!resistor_)
        resistor_ = load_or_build<ResistorRow>(
            ndjson_path(Family::Resistor), shard_path(Family::Resistor), !cache_dir_.empty(),
            quiet_, "resistor",
            [](const std::string& p, const Shard<ResistorRow>* pv) {
                return build_resistor_shard(p, pv);
            },
            [](const std::string& p) { return read_resistor_shard(p); },
            [](const std::string& p, const Shard<ResistorRow>& s) { write_shard(p, s); });
    return *resistor_;
}
const Shard<ControllerRow>& Engine::controller_shard() {
    if (!controller_)
        controller_ = load_or_build<ControllerRow>(
            ndjson_path(Family::Controller), shard_path(Family::Controller), !cache_dir_.empty(),
            quiet_, "controller",
            [](const std::string& p, const Shard<ControllerRow>* pv) {
                return build_controller_shard(p, pv);
            },
            [](const std::string& p) { return read_controller_shard(p); },
            [](const std::string& p, const Shard<ControllerRow>& s) { write_shard(p, s); });
    return *controller_;
}

const Shard<IgbtRow>& Engine::igbt_shard() {
    if (!igbt_)
        igbt_ = load_or_build<IgbtRow>(
            ndjson_path(Family::Igbt), shard_path(Family::Igbt), !cache_dir_.empty(), quiet_, "igbt",
            [](const std::string& p, const Shard<IgbtRow>* pv) { return build_igbt_shard(p, pv); },
            [](const std::string& p) { return read_igbt_shard(p); },
            [](const std::string& p, const Shard<IgbtRow>& s) { write_shard(p, s); });
    return *igbt_;
}
const Shard<BjtRow>& Engine::bjt_shard() {
    if (!bjt_)
        bjt_ = load_or_build<BjtRow>(
            ndjson_path(Family::Bjt), shard_path(Family::Bjt), !cache_dir_.empty(), quiet_, "bjt",
            [](const std::string& p, const Shard<BjtRow>* pv) { return build_bjt_shard(p, pv); },
            [](const std::string& p) { return read_bjt_shard(p); },
            [](const std::string& p, const Shard<BjtRow>& s) { write_shard(p, s); });
    return *bjt_;
}
const Shard<VaristorRow>& Engine::varistor_shard() {
    if (!varistor_)
        varistor_ = load_or_build<VaristorRow>(
            ndjson_path(Family::Varistor), shard_path(Family::Varistor), !cache_dir_.empty(), quiet_,
            "varistor",
            [](const std::string& p, const Shard<VaristorRow>* pv) {
                return build_varistor_shard(p, pv);
            },
            [](const std::string& p) { return read_varistor_shard(p); },
            [](const std::string& p, const Shard<VaristorRow>& s) { write_shard(p, s); });
    return *varistor_;
}
const Shard<MagneticRow>& Engine::magnetic_shard() {
    if (!magnetic_)
        magnetic_ = load_or_build<MagneticRow>(
            ndjson_path(Family::Magnetic), shard_path(Family::Magnetic), !cache_dir_.empty(), quiet_,
            "magnetic",
            [](const std::string& p, const Shard<MagneticRow>* pv) {
                return build_magnetic_shard(p, pv);
            },
            [](const std::string& p) { return read_magnetic_shard(p); },
            [](const std::string& p, const Shard<MagneticRow>& s) { write_shard(p, s); });
    return *magnetic_;
}

const Shard<AnalogRow>& Engine::analog_shard() {
    if (!analog_)
        analog_ = load_or_build<AnalogRow>(
            ndjson_path(Family::Analog), shard_path(Family::Analog), !cache_dir_.empty(), quiet_,
            "analog",
            [](const std::string& p, const Shard<AnalogRow>* pv) { return build_analog_shard(p, pv); },
            [](const std::string& p) { return read_analog_shard(p); },
            [](const std::string& p, const Shard<AnalogRow>& s) { write_shard(p, s); });
    return *analog_;
}
const Shard<TimingRow>& Engine::timing_shard() {
    if (!timing_)
        timing_ = load_or_build<TimingRow>(
            ndjson_path(Family::Timing), shard_path(Family::Timing), !cache_dir_.empty(), quiet_,
            "timing",
            [](const std::string& p, const Shard<TimingRow>* pv) { return build_timing_shard(p, pv); },
            [](const std::string& p) { return read_timing_shard(p); },
            [](const std::string& p, const Shard<TimingRow>& s) { write_shard(p, s); });
    return *timing_;
}

const Shard<ConnectorRow>& Engine::connector_shard() {
    if (!connector_)
        connector_ = load_or_build<ConnectorRow>(
            ndjson_path(Family::Connector), shard_path(Family::Connector), !cache_dir_.empty(),
            quiet_, "connector",
            [](const std::string& p, const Shard<ConnectorRow>* pv) {
                return build_connector_shard(p, pv);
            },
            [](const std::string& p) { return read_connector_shard(p); },
            [](const std::string& p, const Shard<ConnectorRow>& s) { write_shard(p, s); });
    return *connector_;
}

ShardMeta Engine::load_shard_bytes(const std::string& family, const std::string& bytes) {
    Family f = family_from_string(family);
    switch (f) {
        case Family::Mosfet: mosfet_ = deserialize_mosfet_shard(bytes); return mosfet_->meta;
        case Family::Diode: diode_ = deserialize_diode_shard(bytes); return diode_->meta;
        case Family::Capacitor:
            capacitor_ = deserialize_capacitor_shard(bytes); return capacitor_->meta;
        case Family::Resistor: resistor_ = deserialize_resistor_shard(bytes); return resistor_->meta;
        case Family::Controller:
            controller_ = deserialize_controller_shard(bytes); return controller_->meta;
        case Family::Igbt: igbt_ = deserialize_igbt_shard(bytes); return igbt_->meta;
        case Family::Bjt: bjt_ = deserialize_bjt_shard(bytes); return bjt_->meta;
        case Family::Varistor: varistor_ = deserialize_varistor_shard(bytes); return varistor_->meta;
        case Family::Magnetic: magnetic_ = deserialize_magnetic_shard(bytes); return magnetic_->meta;
        case Family::Analog: analog_ = deserialize_analog_shard(bytes); return analog_->meta;
        case Family::Timing: timing_ = deserialize_timing_shard(bytes); return timing_->meta;
        case Family::Connector: connector_ = deserialize_connector_shard(bytes); return connector_->meta;
    }
    throw InvalidOptions("unknown family");
}

ShardMeta Engine::build_index(const std::string& family) {
    Family f = family_from_string(family);
    switch (f) {
        case Family::Mosfet: mosfet_.reset(); return mosfet_shard().meta;
        case Family::Diode: diode_.reset(); return diode_shard().meta;
        case Family::Capacitor: capacitor_.reset(); return capacitor_shard().meta;
        case Family::Resistor: resistor_.reset(); return resistor_shard().meta;
        case Family::Controller: controller_.reset(); return controller_shard().meta;
        case Family::Igbt: igbt_.reset(); return igbt_shard().meta;
        case Family::Bjt: bjt_.reset(); return bjt_shard().meta;
        case Family::Varistor: varistor_.reset(); return varistor_shard().meta;
        case Family::Magnetic: magnetic_.reset(); return magnetic_shard().meta;
        case Family::Analog: analog_.reset(); return analog_shard().meta;
        case Family::Timing: timing_.reset(); return timing_shard().meta;
        case Family::Connector: connector_.reset(); return connector_shard().meta;
    }
    throw InvalidOptions("unknown family");
}

json Engine::select(const std::string& category, const json& req, const json& options) {
    Family f = family_from_string(category);
    // Browse-only families: no requirements emitter → no selector. Refuse loudly rather than
    // invent selection semantics (they get one when a review-gated selector lands).
    if (f == Family::Analog || f == Family::Timing)
        throw InvalidOptions("no selector for '" + category + "' — browse-only family (use browse)");
    size_t max_cand = opt_size(options, "maxCandidates", kDefaultMaxCandidates);
    // Optional manufacturer controls (settings today, GUI-wired later; default
    // off = parity-locked to the Python selector). maxManufacturerFraction caps
    // any one vendor's share of the result; manufacturerAllowlist restricts to
    // named vendors (substring, case-insensitive).
    MfrPolicy mfr;
    if (auto frac = opt_num(options, "maxManufacturerFraction")) mfr.max_frac = *frac;
    if (options.contains("manufacturerAllowlist") && options.at("manufacturerAllowlist").is_array())
        for (const auto& m : options.at("manufacturerAllowlist"))
            if (m.is_string()) mfr.allowlist.push_back(norm_mfr(m.get<std::string>()));
    // Envelopes require the source NDJSON (the record fetcher). With no data dir (the browser /
    // preloaded-shard path) candidates carry only shard data + the record's byte span; the caller
    // fetches the chosen record itself (HTTP Range).
    bool include_env = opt_bool(options, "includeEnvelope", true) && !data_dir_.empty();

    if (f == Family::Connector) {
        ConnectorConstraints c = connector_constraints(req);
        c.exclude_discontinued = opt_bool(options, "excludeDiscontinued", true);
        ConnectorTiebreaker tb = ConnectorTiebreaker::HighestCurrentMargin;
        if (auto s = opt_str(options, "tiebreaker")) tb = connector_tiebreaker_from_string(*s);
        const Shard<ConnectorRow>& sh = connector_shard();
        std::optional<FileRecordFetcher> fetch;
        if (include_env) fetch.emplace(ndjson_path(Family::Connector));
        return select_connector(sh, c, tb, max_cand, include_env ? &*fetch : nullptr, mfr);
    }
    if (f == Family::Mosfet) {
        auto op_fsw = op_fsw_of(options);
        MosfetConstraints c = mosfet_constraints(req, op_fsw);
        if (options.contains("technologyAllowed") && options.at("technologyAllowed").is_array()) {
            c.technology_allowed.clear();
            for (const auto& t : options.at("technologyAllowed"))
                if (t.is_string()) c.technology_allowed.insert(t.get<std::string>());
        }
        c.exclude_discontinued = opt_bool(options, "excludeDiscontinued", true);
        // Optional real operating-point override (KH-mode loss ranking on measured stresses).
        if (options.contains("operatingPoint")) {
            const json& op = options.at("operatingPoint");
            auto i = opt_num(op, "iRms"), v = opt_num(op, "vds"), d = opt_num(op, "duty"),
                 fq = opt_num(op, "fsw");
            if (i && v && d && fq) { c.op_i_rms = i; c.op_vds = v; c.op_duty = d; c.op_fsw = fq; }
        }
        MosfetTiebreaker tb;
        if (auto s = opt_str(options, "tiebreaker")) tb = mosfet_tiebreaker_from_string(*s);
        else tb = c.op_fsw.has_value() ? MosfetTiebreaker::LowestTotalLoss
                                       : MosfetTiebreaker::LowestRdsOn;
        const Shard<MosfetRow>& sh = mosfet_shard();
        std::optional<FileRecordFetcher> fetch;
        if (include_env) fetch.emplace(ndjson_path(Family::Mosfet));
        return select_mosfet(sh, c, tb, max_cand, include_env ? &*fetch : nullptr, mfr);
    }
    if (f == Family::Diode) {
        DiodeConstraints c = diode_constraints(req);
        c.exclude_discontinued = opt_bool(options, "excludeDiscontinued", true);
        if (auto q = opt_num(options, "qrrMax")) c.qrr_max = q;
        DiodeTiebreaker tb = DiodeTiebreaker::LowestVf;
        if (auto s = opt_str(options, "tiebreaker")) tb = diode_tiebreaker_from_string(*s);
        const Shard<DiodeRow>& sh = diode_shard();
        std::optional<FileRecordFetcher> fetch;
        if (include_env) fetch.emplace(ndjson_path(Family::Diode));
        return select_diode(sh, c, tb, max_cand, include_env ? &*fetch : nullptr, mfr);
    }
    if (f == Family::Capacitor) {
        CapacitorConstraints c = capacitor_constraints(req);
        c.exclude_discontinued = opt_bool(options, "excludeDiscontinued", true);
        if (options.contains("technologyAllowed") && options.at("technologyAllowed").is_array()) {
            for (const auto& t : options.at("technologyAllowed"))
                if (t.is_string()) c.technology_allowed.insert(t.get<std::string>());
        }
        CapacitorTiebreaker tb = CapacitorTiebreaker::LowestEsr;
        if (auto s = opt_str(options, "tiebreaker")) tb = capacitor_tiebreaker_from_string(*s);
        const Shard<CapacitorRow>& sh = capacitor_shard();
        std::optional<FileRecordFetcher> fetch;
        if (include_env) fetch.emplace(ndjson_path(Family::Capacitor));
        return select_capacitor(sh, c, tb, max_cand, include_env ? &*fetch : nullptr, mfr);
    }
    if (f == Family::Resistor) {
        ResistorConstraints c = resistor_constraints(req);
        const Shard<ResistorRow>& sh = resistor_shard();
        std::optional<FileRecordFetcher> fetch;
        if (include_env) fetch.emplace(ndjson_path(Family::Resistor));
        return select_resistor(sh, c, max_cand, include_env ? &*fetch : nullptr, mfr);
    }
    if (f == Family::Igbt) {
        IgbtConstraints c = igbt_constraints(req);
        c.exclude_discontinued = opt_bool(options, "excludeDiscontinued", true);
        IgbtTiebreaker tb = IgbtTiebreaker::LowestVceSat;
        if (auto s = opt_str(options, "tiebreaker")) tb = igbt_tiebreaker_from_string(*s);
        const Shard<IgbtRow>& sh = igbt_shard();
        std::optional<FileRecordFetcher> fetch;
        if (include_env) fetch.emplace(ndjson_path(Family::Igbt));
        return select_igbt(sh, c, tb, max_cand, include_env ? &*fetch : nullptr, mfr);
    }
    if (f == Family::Bjt) {
        BjtConstraints c = bjt_constraints(req);
        c.exclude_discontinued = opt_bool(options, "excludeDiscontinued", true);
        BjtTiebreaker tb = BjtTiebreaker::HighestHfe;
        if (auto s = opt_str(options, "tiebreaker")) tb = bjt_tiebreaker_from_string(*s);
        const Shard<BjtRow>& sh = bjt_shard();
        std::optional<FileRecordFetcher> fetch;
        if (include_env) fetch.emplace(ndjson_path(Family::Bjt));
        return select_bjt(sh, c, tb, max_cand, include_env ? &*fetch : nullptr, mfr);
    }
    if (f == Family::Varistor) {
        VaristorConstraints c = varistor_constraints(req);
        c.exclude_discontinued = opt_bool(options, "excludeDiscontinued", true);
        VaristorTiebreaker tb = VaristorTiebreaker::LowestClampingVoltage;
        if (auto s = opt_str(options, "tiebreaker")) tb = varistor_tiebreaker_from_string(*s);
        const Shard<VaristorRow>& sh = varistor_shard();
        std::optional<FileRecordFetcher> fetch;
        if (include_env) fetch.emplace(ndjson_path(Family::Varistor));
        return select_varistor(sh, c, tb, max_cand, include_env ? &*fetch : nullptr, mfr);
    }
    if (f == Family::Magnetic) {
        MagneticConstraints c = magnetic_constraints(req);
        // Operating-point currents may be supplied out-of-band (they are not in the magnetic
        // designRequirements block): options.operatingPoint {peakCurrent, rmsCurrent}. Absent ->
        // those ranking terms stay neutral (the part is still ranked by inductance closeness).
        if (options.contains("operatingPoint")) {
            const json& op = options.at("operatingPoint");
            if (auto p = opt_num(op, "peakCurrent")) c.peak_current = p;
            if (auto r = opt_num(op, "rmsCurrent")) c.rms_current = r;
        }
        if (auto p = opt_num(options, "peakCurrent")) c.peak_current = p;
        if (auto r = opt_num(options, "rmsCurrent")) c.rms_current = r;
        const Shard<MagneticRow>& sh = magnetic_shard();
        std::optional<FileRecordFetcher> fetch;
        if (include_env) fetch.emplace(ndjson_path(Family::Magnetic));
        return select_magnetic(sh, c, max_cand, include_env ? &*fetch : nullptr, mfr);
    }
    // controller
    auto topo = opt_str(options, "topology");
    auto vin = opt_num(options, "inputVoltage");
    auto fsw = op_fsw_of(options);
    if (!topo || !vin || !fsw)
        throw InvalidOptions(
            "controller select needs options.topology + inputVoltage + switchingFrequency");
    ControllerConstraints c = controller_constraints(req, *topo, *vin, *fsw);
    const Shard<ControllerRow>& sh = controller_shard();
    std::optional<FileRecordFetcher> fetch;
    if (include_env) fetch.emplace(ndjson_path(Family::Controller));
    return select_controller(sh, c, max_cand, include_env ? &*fetch : nullptr, mfr);
}

json Engine::browse(const std::string& category, const json& query) {
    Family f = family_from_string(category);
    // Browser path guard: with no data dir a shard can only come from load_shard_bytes, so an
    // unloaded family is a caller error, not a build trigger (the *_shard() getters would try
    // to read "/<family>.ndjson" and produce a misleading DataError).
    if (data_dir_.empty()) {
        bool loaded = (f == Family::Mosfet && mosfet_) || (f == Family::Diode && diode_) ||
                      (f == Family::Capacitor && capacitor_) ||
                      (f == Family::Resistor && resistor_) ||
                      (f == Family::Controller && controller_) || (f == Family::Igbt && igbt_) ||
                      (f == Family::Bjt && bjt_) || (f == Family::Varistor && varistor_) ||
                      (f == Family::Magnetic && magnetic_) || (f == Family::Analog && analog_) ||
                      (f == Family::Timing && timing_) || (f == Family::Connector && connector_);
        if (!loaded)
            throw InvalidOptions("browse: shard not loaded for family '" + category + "'");
    }
    switch (f) {
        case Family::Mosfet: return browse::browse_rows(mosfet_shard(), query);
        case Family::Diode: return browse::browse_rows(diode_shard(), query);
        case Family::Capacitor: return browse::browse_rows(capacitor_shard(), query);
        case Family::Resistor: return browse::browse_rows(resistor_shard(), query);
        case Family::Controller: return browse::browse_rows(controller_shard(), query);
        case Family::Igbt: return browse::browse_rows(igbt_shard(), query);
        case Family::Bjt: return browse::browse_rows(bjt_shard(), query);
        case Family::Varistor: return browse::browse_rows(varistor_shard(), query);
        case Family::Magnetic: return browse::browse_rows(magnetic_shard(), query);
        case Family::Analog: return browse::browse_rows(analog_shard(), query);
        case Family::Timing: return browse::browse_rows(timing_shard(), query);
        case Family::Connector: return browse::browse_rows(connector_shard(), query);
    }
    throw InvalidOptions("unknown family");
}

std::string select_string(const std::string& data_dir, const std::string& cache_dir,
                          const std::string& category, const std::string& req_json,
                          const std::string& options_json) {
    try {
        Engine eng(data_dir, cache_dir, /*quiet=*/true);
        json req = req_json.empty() ? json::object() : json::parse(req_json);
        json options = options_json.empty() ? json::object() : json::parse(options_json);
        return eng.select(category, req, options).dump();
    } catch (const NoCandidates& e) {
        json err;
        err["error"] = "NoCandidates";
        err["category"] = e.category;
        err["rejections"] = e.rejections;
        err["totalRowsConsidered"] = e.total_rows_considered;
        err["message"] = e.what();
        return err.dump();
    } catch (const std::exception& e) {
        return std::string("Exception: ") + e.what();
    }
}

namespace {
// dimensionWithTolerance-or-scalar -> scalar (nominal|minimum|maximum), matching kirchhoff_fill._scalar.
std::optional<double> dr_scalar(const json& o, const char* key) {
    if (!o.is_object() || !o.contains(key)) return std::nullopt;
    const json& v = o.at(key);
    if (v.is_number()) return v.get<double>();
    if (v.is_object()) {
        for (const char* k : {"nominal", "minimum", "maximum"})
            if (v.contains(k) && v.at(k).is_number()) return v.at(k).get<double>();
    }
    return std::nullopt;
}

bool is_numerical_aid(const std::string& name, const json& req) {
    if (name.rfind("Csn", 0) == 0 || name.rfind("Rsn", 0) == 0 || name.rfind("Csw", 0) == 0)
        return true;
    if (req.is_object() && req.value("name", std::string()) == "__kh_numerical_aid__") return true;
    return false;
}

bool has_num(const json& req, std::initializer_list<const char*> keys) {
    if (!req.is_object()) return false;
    for (const char* k : keys)
        if (!req.contains(k) || !req.at(k).is_number()) return false;
    return true;
}
}  // namespace

json select_components(Engine& engine, const json& tas, const json& options) {
    json out;
    out["components"] = json::array();
    const json empty = json::object();
    const json& inputs = tas.contains("inputs") ? tas.at("inputs") : empty;
    const json& dr = inputs.contains("designRequirements") ? inputs.at("designRequirements") : empty;
    auto vin = dr_scalar(dr, "inputVoltage");
    auto fsw = dr_scalar(dr, "switchingFrequency");
    std::optional<std::string> topology;
    if (options.is_object() && options.contains("topology") && options.at("topology").is_string())
        topology = options.at("topology").get<std::string>();

    if (!tas.contains("topology") || !tas.at("topology").contains("stages"))
        throw InvalidOptions("select_components: TAS has no topology.stages[]");

    static const char* kFamilies[] = {"semiconductor", "magnetic", "capacitor", "resistor",
                                      "analog", "controller"};
    for (const auto& stage : tas.at("topology").at("stages")) {
        if (!stage.contains("circuit") || !stage.at("circuit").contains("components")) continue;
        for (const auto& comp : stage.at("circuit").at("components")) {
            if (!comp.contains("data") || !comp.at("data").is_object()) continue;
            const json& data = comp.at("data");
            std::string family;
            for (const char* f : kFamilies)
                if (data.contains(f)) { family = f; break; }
            if (family.empty()) continue;
            const json& req = data.contains("inputs") && data.at("inputs").contains("designRequirements")
                                  ? data.at("inputs").at("designRequirements")
                                  : empty;
            std::string name = comp.value("name", std::string());
            json rec;
            rec["ref"] = name;
            rec["family"] = family;
            if (stage.contains("name")) rec["stage"] = stage.at("name");

            // Already bound (real part present) -> leave untouched unless asked to reselect.
            const json& slot = data.at(family);
            bool already_bound = slot.is_object() && slot.contains("manufacturerInfo");
            if (family == "semiconductor") {
                // slot = {mosfet|diode: {...}}; bound if that inner object carries manufacturerInfo
                already_bound = false;
                if (slot.is_object())
                    for (auto it = slot.begin(); it != slot.end(); ++it)
                        if (it.value().is_object() && it.value().contains("manufacturerInfo"))
                            already_bound = true;
            }
            if (already_bound && !options.value("reselect", false)) {
                rec["filled"] = false;
                rec["deferred"] = "already bound";
                out["components"].push_back(rec);
                continue;
            }
            if (is_numerical_aid(name, req)) {
                rec["filled"] = false;
                rec["deferred"] = "numerical convergence aid (sim-only)";
                out["components"].push_back(rec);
                continue;
            }

            std::string category;
            json sel_options = json::object();
            if (family == "magnetic") {
                // Rank-not-gate: the magnetic selector returns the closest catalogue matches even
                // when none fully meet the spec, so we no longer defer to MKF — we select here.
                category = "magnetic";
                // Best-effort operating-point currents (peak for saturation, rms for rating) when
                // the seed carries them; absent -> ranked by inductance closeness alone.
                if (auto p = dr_scalar(req, "peakCurrent")) sel_options["peakCurrent"] = *p;
                else if (auto mc = dr_scalar(req, "maximumCurrent")) sel_options["peakCurrent"] = *mc;
                if (auto r = dr_scalar(req, "rmsCurrent")) sel_options["rmsCurrent"] = *r;
            } else if (family == "semiconductor") {
                std::string kind = slot.is_object() && !slot.empty() ? slot.begin().key() : "";
                rec["kind"] = kind;
                if (kind == "mosfet") {
                    if (!has_num(req, {"ratedDrainSourceVoltage", "ratedContinuousDrainCurrent",
                                       "maximumOnResistance"})) {
                        rec["filled"] = false;
                        rec["deferred"] = "mosfet seed carries no rating requirement";
                        out["components"].push_back(rec);
                        continue;
                    }
                    category = "mosfet";
                    if (fsw) sel_options["opFsw"] = *fsw;
                } else if (kind == "diode") {
                    if (req.value("role", std::string()) == "bodyDiode" ||
                        !has_num(req, {"ratedReverseVoltage", "ratedForwardCurrent"})) {
                        rec["filled"] = false;
                        rec["deferred"] = "body diode / unswept rectifier seed";
                        out["components"].push_back(rec);
                        continue;
                    }
                    category = "diode";
                } else {
                    rec["filled"] = false;
                    rec["deferred"] = "unknown semiconductor kind";
                    out["components"].push_back(rec);
                    continue;
                }
            } else if (family == "capacitor") {
                category = "capacitor";
            } else if (family == "resistor") {
                category = "resistor";
            } else if (family == "controller") {
                if (!topology || !vin || !fsw) {
                    rec["filled"] = false;
                    rec["deferred"] = "controller: need topology + Vin + fsw";
                    out["components"].push_back(rec);
                    continue;
                }
                category = "controller";
                sel_options["topology"] = *topology;
                sel_options["inputVoltage"] = *vin;
                sel_options["switchingFrequency"] = *fsw;
            } else {
                rec["filled"] = false;
                rec["deferred"] = "no filler for " + family;
                out["components"].push_back(rec);
                continue;
            }

            try {
                json sel = engine.select(category, req, sel_options);
                rec["category"] = category;
                rec["filled"] = true;
                rec["selection"] = sel;
                if (!sel.at("candidates").empty())
                    rec["mpn"] = sel.at("candidates")[0].at("mpn");
                out["components"].push_back(rec);
            } catch (const NoCandidates& e) {
                rec["filled"] = false;
                rec["category"] = category;
                rec["error"] = "NoCandidates";
                rec["rejections"] = e.rejections;
                rec["totalRowsConsidered"] = e.total_rows_considered;
                out["components"].push_back(rec);
            }
        }
    }
    return out;
}

json bind_part(const json& tas, const std::string& ref, const json& envelope) {
    if (!envelope.is_object() || envelope.empty())
        throw InvalidOptions("bind_part: envelope is not a non-empty object");
    std::string fam = envelope.begin().key();  // family slot key (semiconductor|capacitor|...)
    json t = tas;
    if (!t.contains("topology") || !t.at("topology").contains("stages"))
        throw InvalidOptions("bind_part: TAS has no topology.stages[]");
    for (auto& stage : t.at("topology").at("stages")) {
        if (!stage.contains("circuit") || !stage.at("circuit").contains("components")) continue;
        for (auto& comp : stage.at("circuit").at("components")) {
            if (comp.value("name", std::string()) != ref) continue;
            if (!comp.contains("data") || !comp.at("data").is_object())
                comp["data"] = json::object();
            comp["data"][fam] = envelope.at(fam);  // verbatim, already schema-valid
            return t;
        }
    }
    throw InvalidOptions("bind_part: no component named '" + ref + "'");
}

ShardMeta build_and_write_index(const std::string& data_dir, const std::string& out_dir,
                                const std::string& family) {
    Engine eng(data_dir, out_dir, /*quiet=*/false);
    return eng.build_index(family);
}

}  // namespace api
}  // namespace kelvin
