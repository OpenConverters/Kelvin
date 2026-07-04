// libKelvin — Emscripten/embind surface for the KH web worker. Two shapes:
//  * select_string(dataDir, cacheDir, category, reqJson, optionsJson) — native-style (used in
//    node tests and any environment with a filesystem, e.g. Emscripten NODEFS).
//  * a shard-bytes + JS-range-fetcher path is added in Phase 3 web integration; kept minimal here
//    so the module builds and the node cross-target test can exercise select_string.
#include <emscripten/bind.h>

#include "KelvinApi.hpp"

using namespace emscripten;

EMSCRIPTEN_BINDINGS(kelvin) {
    function("select_string", &kelvin::api::select_string);
}
