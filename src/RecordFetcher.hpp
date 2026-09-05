// Fetches the full TAS envelope for a selected candidate by its byte span in the source NDJSON.
// Injected so the query path is storage-agnostic: native reads the file; the browser plugs an
// HTTP Range fetcher (embind callback). A null fetcher = candidates carry no envelope (fast path
// for parity tests that only compare MPNs/margins/histograms).
#pragma once
#include <sys/stat.h>

#include <cstdint>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

namespace kelvin {

using json = nlohmann::json;

struct RecordFetcher {
    virtual ~RecordFetcher() = default;
    virtual json fetch(uint64_t offset, uint32_t length) = 0;
};

// Reads spans from a local NDJSON file (opened once, pread per fetch).
//
// A span is only meaningful against the exact bytes the shard was built from, and on this
// corpus that is not a safe assumption: TAS catalogues are rewritten in place by other
// processes while things read them (capacitors.ndjson is 632 MB and moved twice in one
// evening). So the identity of the source is checked, not assumed — see the .cpp for what
// each check catches and, more importantly, what it deliberately does not.
class FileRecordFetcher : public RecordFetcher {
   public:
    // expected_source_size: the shard's ShardMeta::source_size. Pass it wherever it is
    // known — 0 means "unknown, skip that check", which is only for callers that have no
    // shard to hand.
    explicit FileRecordFetcher(std::string path, uint64_t expected_source_size = 0);
    json fetch(uint64_t offset, uint32_t length) override;

   private:
    void refuse_if_source_moved();

    std::string path_;
    std::ifstream stream_;
    // the file's identity when we opened it
    uint64_t dev_ = 0, ino_ = 0, size_at_open_ = 0;
    int64_t mtime_at_open_ = 0;
};

}  // namespace kelvin
