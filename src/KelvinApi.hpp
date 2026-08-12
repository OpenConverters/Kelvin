// Kelvin's public engine: given a TAS data dir (+ optional index cache dir), select real parts
// for a schema designRequirements block. Shards are loaded from the cache (or built + written,
// rebuilding when stale — an index is a cache, so a rebuild is legitimate, not a silent
// fallback; it is logged). Exceptions (NoCandidates / InvalidOptions / DataError) propagate.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Index.hpp"

namespace kelvin {
namespace api {

using json = nlohmann::json;

class Engine {
   public:
    // data_dir: directory holding <family>.ndjson. cache_dir: where <family>.kidx shards live
    // (empty => build shards in memory each run, no persistence). quiet: suppress the
    // "(re)building index" log line.
    explicit Engine(std::string data_dir, std::string cache_dir = "", bool quiet = false);

    // category in {mosfet, diode, capacitor, resistor, controller}. options is Kelvin's own
    // (non-schema) options object. Returns a SelectionResult; throws NoCandidates on none.
    json select(const std::string& category, const json& design_requirements, const json& options);

    // Catalogue browsing (the Kelvin web frontend): deterministic filter/sort/facet/paginate
    // over one family's shard rows — see Browse.hpp for the query/result contract. With an
    // empty data dir (the browser path) the family's shard must have been fed via
    // load_shard_bytes first; browsing an unloaded family throws InvalidOptions.
    json browse(const std::string& category, const json& query);

    // The full TAS record behind a browse/select row, by its byte span in the family's source
    // NDJSON (row.srcOffset / row.srcLength) — the native equivalent of the browser's HTTP Range
    // fetch. Needs a data dir; throws DataError without one (there is no file to read).
    json fetch_record(const std::string& category, uint64_t offset, uint32_t length);

    // Build (or incrementally refresh) and persist the shard for one family. Returns its meta.
    ShardMeta build_index(const std::string& family);

    // Load a prebuilt shard from raw bytes (the browser path: no filesystem). After loading, the
    // family is queryable via select() with an empty data dir (candidates carry no envelope —
    // the caller fetches the chosen record's byte span itself, e.g. an HTTP Range request). The
    // candidate's srcOffset/srcLength locate that record in the source NDJSON.
    ShardMeta load_shard_bytes(const std::string& family, const std::string& bytes);

   private:
    std::string data_dir_;
    std::string cache_dir_;
    bool quiet_;
    std::optional<Shard<MosfetRow>> mosfet_;
    std::optional<Shard<DiodeRow>> diode_;
    std::optional<Shard<CapacitorRow>> capacitor_;
    std::optional<Shard<ResistorRow>> resistor_;
    std::optional<Shard<ControllerRow>> controller_;
    std::optional<Shard<IgbtRow>> igbt_;
    std::optional<Shard<BjtRow>> bjt_;
    std::optional<Shard<VaristorRow>> varistor_;
    std::optional<Shard<MagneticRow>> magnetic_;
    std::optional<Shard<AnalogRow>> analog_;
    std::optional<Shard<TimingRow>> timing_;
    std::optional<Shard<ConnectorRow>> connector_;

    std::string ndjson_path(Family f) const;
    std::string shard_path(Family f) const;
    const Shard<MosfetRow>& mosfet_shard();
    const Shard<DiodeRow>& diode_shard();
    const Shard<CapacitorRow>& capacitor_shard();
    const Shard<ResistorRow>& resistor_shard();
    const Shard<ControllerRow>& controller_shard();
    const Shard<IgbtRow>& igbt_shard();
    const Shard<BjtRow>& bjt_shard();
    const Shard<VaristorRow>& varistor_shard();
    const Shard<MagneticRow>& magnetic_shard();
    const Shard<AnalogRow>& analog_shard();
    const Shard<TimingRow>& timing_shard();
    const Shard<ConnectorRow>& connector_shard();
};

// String facade (guarded) for the embind/WASM and any C-string consumer: returns the
// SelectionResult JSON string, or an "Exception: ..." string on error.
std::string select_string(const std::string& data_dir, const std::string& cache_dir,
                          const std::string& category, const std::string& design_requirements_json,
                          const std::string& options_json);

// Walk a Kirchhoff TAS document and select a real part for every fillable component seed. This
// is the single selection authority KH (api::select_components) and HS (kirchhoff_fill) forward
// to. Skips body diodes, numerical-convergence aids, magnetics (MKF), and already-bound parts.
// A per-component NoCandidates is captured as {ref,error,rejections} rather than thrown, so one
// unsatisfiable part does not sink the whole BOM result (the caller decides fail-loud vs defer).
// `options.topology` (+ tas.inputs.designRequirements.inputVoltage/switchingFrequency) drive the
// controller and HV-mosfet paths. Returns {components:[...]}.
json select_components(Engine& engine, const json& tas, const json& options);

// Bind a chosen candidate's envelope into the named component's data slot (verbatim — it is
// already schema-valid). Returns the new TAS. Everything downstream in KH then treats the
// component as DATASHEET fidelity. Throws if no component is named `ref`.
json bind_part(const json& tas, const std::string& ref, const json& envelope);

// Free helper for the kelvin-index CLI: build (incremental if a prior shard exists) + write one
// family's shard into out_dir. Returns its meta. family in the 5-family set.
ShardMeta build_and_write_index(const std::string& data_dir, const std::string& out_dir,
                                const std::string& family);

Family family_from_string(const std::string& s);

// Every family Kelvin indexes, in a stable order — the catalogue's table of contents.
std::vector<std::string> family_names();

// The browse query language for one family: which field names take numeric ranges, which take
// value lists (facetable), which take booleans. Generated from Browse.hpp's field table, so a
// caller learns the real vocabulary instead of guessing at it. No shard needed.
json family_fields(const std::string& category);

}  // namespace api
}  // namespace kelvin
