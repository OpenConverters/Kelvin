#include "RecordFetcher.hpp"

#include <algorithm>
#include <stdexcept>

namespace kelvin {

FileRecordFetcher::FileRecordFetcher(std::string path, uint64_t expected_source_size)
    : path_(std::move(path)), stream_(path_, std::ios::binary) {
    if (!stream_) throw std::runtime_error("kelvin: record source not found: " + path_);
    struct stat st {};
    if (::stat(path_.c_str(), &st) == 0) {
        dev_ = static_cast<uint64_t>(st.st_dev);
        ino_ = static_cast<uint64_t>(st.st_ino);
        size_at_open_ = static_cast<uint64_t>(st.st_size);
        mtime_at_open_ = static_cast<int64_t>(st.st_mtime);
    }
    // The shard records how big its source was. A different size is not a suspicion, it is
    // proof the index cannot address this file — and it is far better caught here, once,
    // than as a mangled record later.
    if (expected_source_size != 0 && size_at_open_ != 0 &&
        size_at_open_ != expected_source_size)
        throw std::runtime_error(
            "kelvin: " + path_ + " is " + std::to_string(size_at_open_) +
            " bytes but its index was built against " + std::to_string(expected_source_size) +
            ". The catalogue changed after the shard was built; rebuild the shard.");
}

void FileRecordFetcher::refuse_if_source_moved() {
    if (ino_ == 0) return;                      // could not stat at open; nothing to compare
    struct stat st {};
    if (::stat(path_.c_str(), &st) != 0) return;  // vanished mid-run: the read below will say so
    if (static_cast<uint64_t>(st.st_ino) != ino_ ||
        static_cast<uint64_t>(st.st_dev) != dev_) {
        // REPLACED, not rewritten: the file was renamed over and our descriptor still holds
        // the old inode. What we read is exactly what the shard indexed, so this is SAFE and
        // deliberately not an error — refusing here would fail a correct read.
        return;
    }
    if (static_cast<uint64_t>(st.st_size) != size_at_open_ ||
        static_cast<int64_t>(st.st_mtime) != mtime_at_open_)
        throw std::runtime_error(
            "kelvin: " + path_ + " was rewritten IN PLACE while it was being read (was " +
            std::to_string(size_at_open_) + " bytes, now " + std::to_string(st.st_size) +
            "). Every byte offset from the index is now meaningless against it. This is the "
            "quiet one: a span whose bytes happen to parse would otherwise have returned a "
            "DIFFERENT part as the right one.");
}

json FileRecordFetcher::fetch(uint64_t offset, uint32_t length) {
    refuse_if_source_moved();
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
