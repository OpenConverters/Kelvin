// PyKelvin — pybind11 module Heaviside consumes (like PyOpenMagnetics). Exposes the Engine and
// the index builder. designRequirements/options pass as dicts (pybind11_json); SelectionResult
// returns as a dict. Exceptions propagate to Python (NoCandidates carries the rejection
// histogram on its .args-serialized message and as a KelvinNoCandidates with attributes).
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <nlohmann/json.hpp>
#include <pybind11_json/pybind11_json.hpp>

#include "Constraints.hpp"
#include "CrossRef.hpp"
#include "Index.hpp"
#include "KelvinApi.hpp"
#include "Select.hpp"

namespace py = pybind11;
using nlohmann::json;

PYBIND11_MODULE(PyKelvin, m) {
    m.doc() = "Kelvin — deterministic TAS component selection (parity-locked to HS selector.py)";

    static py::exception<kelvin::NoCandidates> no_cand(m, "NoCandidates");
    static py::exception<kelvin::InvalidOptions> invalid_opt(m, "InvalidOptions");
    static py::exception<kelvin::DataError> data_err(m, "DataError");

    py::register_exception_translator([](std::exception_ptr p) {
        try {
            if (p) std::rethrow_exception(p);
        } catch (const kelvin::NoCandidates& e) {
            // Attach the rejection histogram + total as a dict on the exception message tail so
            // the HS adapter can reconstruct a SelectionError.
            json payload = {{"category", e.category},
                            {"rejections", e.rejections},
                            {"totalRowsConsidered", e.total_rows_considered}};
            no_cand(payload.dump().c_str());
        } catch (const kelvin::InvalidOptions& e) {
            invalid_opt(e.what());
        } catch (const kelvin::DataError& e) {
            data_err(e.what());
        }
    });

    py::class_<kelvin::api::Engine>(m, "Engine")
        .def(py::init<std::string, std::string, bool>(), py::arg("data_dir"),
             py::arg("cache_dir") = std::string(), py::arg("quiet") = false)
        .def(
            "select",
            [](kelvin::api::Engine& e, const std::string& category, const json& req,
               const json& options) { return e.select(category, req, options); },
            py::arg("category"), py::arg("design_requirements"), py::arg("options") = json::object())
        .def("build_index", &kelvin::api::Engine::build_index, py::arg("family"))
        .def(
            "load_shard_bytes",
            [](kelvin::api::Engine& e, const std::string& family, const py::bytes& b) {
                std::string bytes = b;  // copy the buffer
                kelvin::ShardMeta m = e.load_shard_bytes(family, bytes);
                return json{{"family", kelvin::family_name(m.family)},
                            {"rowCount", m.row_count},
                            {"buildId", m.build_id}};
            },
            py::arg("family"), py::arg("bytes"))
        .def(
            "select_components",
            [](kelvin::api::Engine& e, const json& tas, const json& options) {
                return kelvin::api::select_components(e, tas, options);
            },
            py::arg("tas"), py::arg("options") = json::object());

    m.def(
        "bind_part",
        [](const json& tas, const std::string& ref, const json& envelope) {
            return kelvin::api::bind_part(tas, ref, envelope);
        },
        py::arg("tas"), py::arg("ref"), py::arg("envelope"));

    // Cross-reference ranker: deterministic scored substitutes for an original.
    // Heaviside runs its LLM chooser over the returned candidates[]; Kirchhoff
    // consumes them directly. `original`/`candidates[]` are category-appropriate
    // spec dicts (SI units); `options.original_verified` tells the ranker whether
    // the original's specs were resolved (the honesty gate).
    m.def(
        "cross_reference",
        [](const std::string& category, const json& original, const json& candidates,
           const json& options) {
            return kelvin::crossref::cross_reference_json(category, original, candidates, options);
        },
        py::arg("category"), py::arg("original"), py::arg("candidates"),
        py::arg("options") = json::object());

    m.def(
        "build_and_write_index",
        [](const std::string& data_dir, const std::string& out_dir, const std::string& family) {
            kelvin::ShardMeta meta = kelvin::api::build_and_write_index(data_dir, out_dir, family);
            return json{{"family", kelvin::family_name(meta.family)},
                        {"rowCount", meta.row_count},
                        {"unreadableRowCount", meta.unreadable_row_count},
                        {"sourceLineCount", meta.source_line_count},
                        {"buildId", meta.build_id}};
        },
        py::arg("data_dir"), py::arg("out_dir"), py::arg("family"));
}
