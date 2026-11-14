// The UNVERIFIED key of every family's operating-point ranking (owner 2026-11-14), written once.
//
// Under the operating-point loss ranking a candidate with an UNVERIFIED item ranks after every
// fully verified one; among the incomplete ones, fewer items first. The items are the family's
// UNVERIFIED loss terms (its own breakdown's unverified()) plus ONE more when the candidate's
// size cannot be determined: its board area, or its volume where the family reports one
// (capacitors, magnetics). A part whose size nobody can state is not one the ranking may put
// ahead of parts it can size.
//
// Each family resolves its size into the candidate's JSON — boardArea {mm2, basis}, and
// volume {mm3, basis} where it reports one — and passes those objects here, so the ranking
// reads exactly what the candidate shows. A selector calls:
//
//   const auto size = op_ranking::size_of(area_json, &volume_json);    // or (area_json) alone
//   key = std::make_tuple(op_ranking::unverified_items(loss.unverified().size(), size), ...);
//   cd["sortKey"] += op_ranking::sort_key_fields(loss.unverified().size(), size);
//   if (size.undetermined) loss_json["notes"].push_back(size.note);
#pragma once
#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

namespace kelvin::op_ranking {

struct SizeEvidence {
    bool undetermined = false;
    std::string note;  // why, when undetermined (empty otherwise)
};

// `area`: the candidate's boardArea object; `volume`: its volume object, or nullptr when the
// family reports none. A null mm2 / mm3 is "cannot be determined"; the note carries the basis
// the size resolver wrote ("not stated: ...").
inline SizeEvidence size_of(const nlohmann::json& area, const nlohmann::json* volume = nullptr) {
    SizeEvidence s;
    auto missing = [](const nlohmann::json& j, const char* key) {
        return !j.is_object() || !j.contains(key) || j.at(key).is_null();
    };
    auto basis = [](const nlohmann::json& j) {
        return j.is_object() && j.contains("basis") && j.at("basis").is_string()
                   ? j.at("basis").get<std::string>()
                   : std::string("not stated");
    };
    std::string why;
    if (missing(area, "mm2")) why = "board area " + basis(area);
    if (volume && missing(*volume, "mm3"))
        why += (why.empty() ? "" : "; ") + std::string("volume ") + basis(*volume);
    if (!why.empty()) {
        s.undetermined = true;
        s.note = "size UNVERIFIED (" + why +
                 "): counts one more UNVERIFIED item in the operating-point ranking";
    }
    return s;
}

// The first key of the operating-point ranking: UNVERIFIED loss terms + 1 for an undetermined size.
inline std::size_t unverified_items(std::size_t unverified_terms, const SizeEvidence& size) {
    return unverified_terms + (size.undetermined ? 1 : 0);
}

// The sortKey fields every family reports under the operating-point ranking.
inline nlohmann::json sort_key_fields(std::size_t unverified_terms, const SizeEvidence& size) {
    return {{"unverifiedTerms", unverified_terms},
            {"sizeUnverified", size.undetermined},
            {"unverifiedItems", unverified_items(unverified_terms, size)}};
}

}  // namespace kelvin::op_ranking
