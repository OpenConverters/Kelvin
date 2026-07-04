#include "RecordFetcher.hpp"

#include <stdexcept>

namespace kelvin {

FileRecordFetcher::FileRecordFetcher(std::string path)
    : path_(std::move(path)), stream_(path_, std::ios::binary) {
    if (!stream_) throw std::runtime_error("kelvin: record source not found: " + path_);
}

json FileRecordFetcher::fetch(uint64_t offset, uint32_t length) {
    // Seek + read only the winner's span (top-N per select => a handful of small reads); no
    // whole-file load, so this is cheap even against capacitors.ndjson (292 MB).
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::string buf(length, '\0');
    stream_.read(&buf[0], static_cast<std::streamsize>(length));
    if (stream_.gcount() != static_cast<std::streamsize>(length))
        throw std::runtime_error("kelvin: short record read (stale index vs source?)");
    return json::parse(buf);
}

}  // namespace kelvin
