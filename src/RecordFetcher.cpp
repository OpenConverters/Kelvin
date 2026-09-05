#include "RecordFetcher.hpp"

#include <algorithm>
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

    // A span is a WHOLE LINE of the NDJSON, so it must begin an object. When it does
    // not, the index and the file have gone out of step — the offset is pointing into
    // the middle of some other record — and reading on is not an option in either
    // direction: nlohmann throws a bare "parse error at line 1, column 1 ... last read
    // 'c'" that names neither the file nor the offset (a whole afternoon, that one), and
    // in the worse case the bytes happen to parse and a WRONG part is returned as the
    // right one. Say which file and which span, and say what it means.
    const char* p = buf.data();
    size_t n = buf.size();
    while (n && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) { ++p; --n; }
    if (n == 0 || *p != '{')
        throw std::runtime_error(
            "kelvin: the index does not match its source. " + path_ + " byte " +
            std::to_string(offset) + " (+" + std::to_string(length) +
            ") should begin a record and begins " +
            (n == 0 ? std::string("whitespace")
                    : "'" + std::string(p, std::min<size_t>(n, 24)) + "'") +
            ". Rebuild the shard against this file: a shard cached from a DIFFERENT "
            "version of the same catalogue has byte offsets that no longer point at "
            "record boundaries.");

    try {
        return json::parse(buf);
    } catch (const json::parse_error& e) {
        throw std::runtime_error("kelvin: record at " + path_ + " byte " +
                                 std::to_string(offset) + " (+" + std::to_string(length) +
                                 ") is not valid JSON: " + e.what());
    }
}

}  // namespace kelvin
