// The five parametric selectors. Each filters the shard rows (exact Python order + rejection
// keys), ranks the survivors by the tiebreaker tuple with source line number as the final key
// (reproduces Python min()/max() first-in-file-order-wins semantics), and returns a
// SelectionResult JSON. Throws NoCandidates (carrying the rejection histogram) when none pass.
#pragma once
#include "MfrPolicy.hpp"
#include <cstddef>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "Constraints.hpp"
#include "Index.hpp"
#include "RecordFetcher.hpp"

namespace kelvin {

using json = nlohmann::json;

// No row satisfied the constraints. Mirrors selector.py SelectionError: carries the rejection
// histogram (only non-zero keys, matching Python's Counter) and the total rows considered.
struct NoCandidates : std::runtime_error {
    std::string category;
    json rejections;
    uint64_t total_rows_considered;
    NoCandidates(std::string cat, json rej, uint64_t total)
        : std::runtime_error("no " + cat + " candidate satisfies the constraints"),
          category(std::move(cat)),
          rejections(std::move(rej)),
          total_rows_considered(total) {}
};

constexpr size_t kDefaultMaxCandidates = 25;

json select_mosfet(const Shard<MosfetRow>& shard, const MosfetConstraints& c, MosfetTiebreaker tb,
                   size_t max_candidates = kDefaultMaxCandidates, RecordFetcher* fetcher = nullptr, const MfrPolicy& mfr = {});
json select_diode(const Shard<DiodeRow>& shard, const DiodeConstraints& c, DiodeTiebreaker tb,
                  size_t max_candidates = kDefaultMaxCandidates, RecordFetcher* fetcher = nullptr, const MfrPolicy& mfr = {});
json select_capacitor(const Shard<CapacitorRow>& shard, const CapacitorConstraints& c,
                      CapacitorTiebreaker tb, size_t max_candidates = kDefaultMaxCandidates,
                      RecordFetcher* fetcher = nullptr, const MfrPolicy& mfr = {});
json select_controller(const Shard<ControllerRow>& shard, const ControllerConstraints& c,
                       size_t max_candidates = kDefaultMaxCandidates,
                       RecordFetcher* fetcher = nullptr, const MfrPolicy& mfr = {});
json select_resistor(const Shard<ResistorRow>& shard, const ResistorConstraints& c,
                     size_t max_candidates = kDefaultMaxCandidates,
                     RecordFetcher* fetcher = nullptr, const MfrPolicy& mfr = {});

// ---- Phase 5 ---------------------------------------------------------------
json select_igbt(const Shard<IgbtRow>& shard, const IgbtConstraints& c, IgbtTiebreaker tb,
                 size_t max_candidates = kDefaultMaxCandidates, RecordFetcher* fetcher = nullptr, const MfrPolicy& mfr = {});
json select_bjt(const Shard<BjtRow>& shard, const BjtConstraints& c, BjtTiebreaker tb,
                size_t max_candidates = kDefaultMaxCandidates, RecordFetcher* fetcher = nullptr, const MfrPolicy& mfr = {});
json select_varistor(const Shard<VaristorRow>& shard, const VaristorConstraints& c,
                     VaristorTiebreaker tb, size_t max_candidates = kDefaultMaxCandidates,
                     RecordFetcher* fetcher = nullptr, const MfrPolicy& mfr = {});

// Connector: gates on the physically disqualifying axes (exact position count, family,
// polarity) and on minimum electrical ratings — a part with an UNDOCUMENTED gated rating is
// rejected (counted as *_undocumented), never assumed adequate. Survivors rank by rating
// headroom.
json select_connector(const Shard<ConnectorRow>& shard, const ConnectorConstraints& c,
                      ConnectorTiebreaker tb, size_t max_candidates = kDefaultMaxCandidates,
                      RecordFetcher* fetcher = nullptr, const MfrPolicy& mfr = {});

// ---- Magnetic (from-spec, rank-not-gate) -----------------------------------
// Unlike every other selector this one NEVER hard-gates: it ranks the whole catalogue toward the
// (all-optional) targets and returns the top-N even when nothing satisfies them, each candidate
// carrying per-dimension margins + an overall pass/marginal/fail verdict. It throws NoCandidates
// only when there is literally nothing to rank (an empty shard, or an allowlist that admits none).
json select_magnetic(const Shard<MagneticRow>& shard, const MagneticConstraints& c,
                     size_t max_candidates = kDefaultMaxCandidates,
                     RecordFetcher* fetcher = nullptr, const MfrPolicy& mfr = {});

}  // namespace kelvin
