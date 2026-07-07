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
// marshalling is UTF-8 and would corrupt bytes >= 0x80).
std::string load_shard(std::string family, val bytes) {
    return guarded([&] {
        std::vector<unsigned char> v = vecFromJSArray<unsigned char>(bytes);
        kelvin::ShardMeta m = web_engine().load_shard_bytes(family, std::string(v.begin(), v.end()));
        return json{{"family", kelvin::family_name(m.family)},
                    {"rowCount", m.row_count},
                    {"buildId", m.build_id}}
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
    function("select", &select);
    function("browse", &browse);
}
