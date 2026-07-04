// Fetches the full TAS envelope for a selected candidate by its byte span in the source NDJSON.
// Injected so the query path is storage-agnostic: native reads the file; the browser plugs an
// HTTP Range fetcher (embind callback). A null fetcher = candidates carry no envelope (fast path
// for parity tests that only compare MPNs/margins/histograms).
#pragma once
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
class FileRecordFetcher : public RecordFetcher {
   public:
    explicit FileRecordFetcher(std::string path);
    json fetch(uint64_t offset, uint32_t length) override;

   private:
    std::string path_;
    std::ifstream stream_;
};

}  // namespace kelvin
