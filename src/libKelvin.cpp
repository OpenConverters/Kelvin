// libKelvin — Emscripten/embind surface. Three shapes:
//  * select_string(dataDir, cacheDir, category, reqJson, optionsJson) — native-style (used in
//    node tests and any environment with a filesystem, e.g. Emscripten NODEFS).
//  * the browser shard-bytes path (the Kelvin web frontend + any static host): a persistent
//    in-module Engine with an empty data dir, fed prebuilt .kidx bytes via load_shard, then
//    queried with select (recommendations) and browse (catalogue filtering/facets). Candidates
//    carry srcOffset/srcLength; JS fetches the full record itself (HTTP Range).
//  * cross_reference_string — the no-FS substitute ranker.
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <nlohmann/json.hpp>

#include "CrossRef.hpp"
#include "Index.hpp"
#include "KelvinApi.hpp"
#include "Select.hpp"

using namespace emscripten;
using nlohmann::json;

namespace {

// String-in/string-out cross-reference for the web worker (no LLM, no FS):
// the caller passes the original spec + candidate list as JSON and renders
// the scored substitutes.
std::string cross_reference_string(const std::string& category, const std::string& original_json,
                                   const std::string& candidates_json,
                                   const std::string& options_json) {
    json original = json::parse(original_json, nullptr, false);
    json candidates = json::parse(candidates_json, nullptr, false);
    json options = options_json.empty() ? json::object() : json::parse(options_json, nullptr, false);
    if (original.is_discarded() || candidates.is_discarded()) return R"({"error":"bad json"})";
    if (options.is_discarded()) options = json::object();
    return kelvin::crossref::cross_reference_json(category, original, candidates, options).dump();
}

// The browser engine: no filesystem — shards are fed as bytes, records fetched by JS via Range.
kelvin::api::Engine& web_engine() {
    static kelvin::api::Engine engine("", "", /*quiet=*/true);
    return engine;
}

template <class F>
std::string guarded(F&& body) {
    try {
        return body();
    } catch (const std::exception& e) {
        return std::string("Exception: ") + e.what();
    } catch (...) {
        return std::string("Exception: unknown error");
    }
}

// Shard bytes are BINARY, so load_shard takes a Uint8Array (not a std::string, whose embind
// marshalling is UTF-8 and would corrupt bytes >= 0x80). The bytes are copied ONCE, straight
// into the string the deserializer reads (v23: vecFromJSArray went element by element through a
// vector and then copied again — three copies of a 100+ MB shard alive at the peak).
std::string bytes_of(const val& bytes) {
    const size_t n = bytes["length"].as<size_t>();
    std::string out(n, '\0');
    val view(typed_memory_view(n, reinterpret_cast<unsigned char*>(out.data())));
    view.call<void>("set", bytes);
    return out;
}

std::string load_shard(std::string family, val bytes) {
    return guarded([&] {
        kelvin::ShardMeta m = web_engine().load_shard_bytes(family, bytes_of(bytes));
        // buildId is a uint64 and these run past 2^53, so a JSON number would
        // be ROUNDED the moment JS parsed it and could never be compared with
        // the manifest's — which is why the manifest writes it as a string too.
        // The caller needs that comparison: it is the only way to tell that the
        // bytes it was handed are the shard the manifest asked for.
        json out{{"family", kelvin::family_name(m.family)},
                 {"rowCount", m.row_count},
                 {"buildId", std::to_string(m.build_id)}};
        if (m.has_curve_shard) out["curvesBuildId"] = std::to_string(m.curves_build_id);
        return out.dump();
    });
}

// v23: the family's curve shard (<family>.curves.kidx), after its base shard. Throws (as an
// "Exception: " string) when it is not the curve shard of the loaded base build.
std::string load_curve_shard(std::string family, val bytes) {
    return guarded([&] {
        kelvin::ShardMeta m = web_engine().load_curve_shard_bytes(family, bytes_of(bytes));
        return json{{"family", kelvin::family_name(m.family)},
                    {"rowCount", m.row_count},
                    {"buildId", std::to_string(m.build_id)},
                    {"curvesBuildId", std::to_string(m.curves_build_id)}}
            .dump();
    });
}

std::string select(const std::string& category, const std::string& req_json,
                   const std::string& options_json) {
    try {
        json req = req_json.empty() ? json::object() : json::parse(req_json);
        json options = options_json.empty() ? json::object() : json::parse(options_json);
        return web_engine().select(category, req, options).dump();
    } catch (const kelvin::NoCandidates& e) {
        return json{{"error", "NoCandidates"},
                    {"category", e.category},
                    {"rejections", e.rejections},
                    {"totalRowsConsidered", e.total_rows_considered}}
            .dump();
    } catch (const std::exception& e) {
        return std::string("Exception: ") + e.what();
    }
}

std::string browse(const std::string& category, const std::string& query_json) {
    return guarded([&] {
        json query = query_json.empty() ? json::object() : json::parse(query_json);
        return web_engine().browse(category, query).dump();
    });
}

}  // namespace

EMSCRIPTEN_BINDINGS(kelvin) {
    function("select_string", &kelvin::api::select_string);
    function("cross_reference_string", &cross_reference_string);
    // Browser sourcing (no FS): load prebuilt shard bytes, then select/browse the loaded families.
    function("load_shard", &load_shard);
    function("load_curve_shard", &load_curve_shard);
    // The shard layout this build reads. The loader puts it in every shard URL,
    // so a format bump changes the URL and a browser cannot serve back the
    // previous file from an "immutable, 1 year" cache entry.
    function("shard_format_version", &kelvin::shard_format_version);
    function("select", &select);
    function("browse", &browse);
}
