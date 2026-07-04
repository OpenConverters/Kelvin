// Kelvin's public engine: given a TAS data dir (+ optional index cache dir), select real parts
// for a schema designRequirements block. Shards are loaded from the cache (or built + written,
// rebuilding when stale — an index is a cache, so a rebuild is legitimate, not a silent
// fallback; it is logged). Exceptions (NoCandidates / InvalidOptions / DataError) propagate.
#pragma once
#include <optional>
#include <string>

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

}  // namespace api
}  // namespace kelvin
