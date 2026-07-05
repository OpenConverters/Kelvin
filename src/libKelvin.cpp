// libKelvin — Emscripten/embind surface for the KH web worker. Two shapes:
//  * select_string(dataDir, cacheDir, category, reqJson, optionsJson) — native-style (used in
//    node tests and any environment with a filesystem, e.g. Emscripten NODEFS).
//  * a shard-bytes + JS-range-fetcher path is added in Phase 3 web integration; kept minimal here
//    so the module builds and the node cross-target test can exercise select_string.
#include <emscripten/bind.h>

#include <nlohmann/json.hpp>

#include "CrossRef.hpp"
#include "KelvinApi.hpp"

using namespace emscripten;

// String-in/string-out cross-reference for the KH web worker (no LLM, no FS):
// the PartDrawer passes the original spec + candidate list as JSON and renders
// the scored substitutes.
static std::string cross_reference_string(const std::string& category,
                                          const std::string& original_json,
                                          const std::string& candidates_json,
                                          const std::string& options_json) {
    using nlohmann::json;
    json original = json::parse(original_json, nullptr, false);
    json candidates = json::parse(candidates_json, nullptr, false);
    json options = options_json.empty() ? json::object() : json::parse(options_json, nullptr, false);
    if (original.is_discarded() || candidates.is_discarded()) return R"({"error":"bad json"})";
    if (options.is_discarded()) options = json::object();
    return kelvin::crossref::cross_reference_json(category, original, candidates, options).dump();
}

EMSCRIPTEN_BINDINGS(kelvin) {
    function("select_string", &kelvin::api::select_string);
    function("cross_reference_string", &cross_reference_string);
}
