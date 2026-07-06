#include "Index.hpp"

#include <cstring>
#include <fstream>
#include <functional>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "Hash.hpp"
#include "Views.hpp"

namespace kelvin {
namespace {

using json = nlohmann::json;

static_assert(sizeof(double) == 8, "double must be 8 bytes");

constexpr char kMagic[8] = {'K', 'E', 'L', 'V', 'I', 'D', 'X', '\1'};
constexpr uint32_t kFormatVersion = 1;

// ---- string pool -----------------------------------------------------------
class StringPool {
   public:
    StringPool() { intern(std::string()); }  // "" always at ref 0
    uint32_t intern(const std::string& s) {
        auto it = map_.find(s);
        if (it != map_.end()) return it->second;
        uint32_t ref = static_cast<uint32_t>(blob_.size());
        uint32_t len = static_cast<uint32_t>(s.size());
        blob_.append(reinterpret_cast<const char*>(&len), 4);
        blob_.append(s);
        map_.emplace(s, ref);
        return ref;
    }
    const std::string& blob() const { return blob_; }

   private:
    std::string blob_;
    std::unordered_map<std::string, uint32_t> map_;
};

std::string pool_read(const char* base, size_t size, uint32_t ref) {
    if (ref + 4 > size) throw std::runtime_error("kelvin: corrupt shard (string ref OOB)");
    uint32_t len;
    std::memcpy(&len, base + ref, 4);
    if (ref + 4 + len > size) throw std::runtime_error("kelvin: corrupt shard (string len OOB)");
    return std::string(base + ref + 4, len);
}

// ---- archives --------------------------------------------------------------
struct WriteAr {
    std::string* buf;
    StringPool* pool;
    static constexpr bool writing = true;
    void u32(uint32_t& v) { buf->append(reinterpret_cast<const char*>(&v), 4); }
    void u64(uint64_t& v) { buf->append(reinterpret_cast<const char*>(&v), 8); }
    void dbl(double& v) { buf->append(reinterpret_cast<const char*>(&v), 8); }
    void str(std::string& s) {
        uint32_t ref = pool->intern(s);
        u32(ref);
    }
    void boolean(bool& b) {
        uint32_t v = b ? 1u : 0u;
        u32(v);
    }
    void strvec(std::vector<std::string>& v) {
        uint32_t n = static_cast<uint32_t>(v.size());
        u32(n);
        for (auto& s : v) str(s);
    }
};

struct ReadAr {
    const char* rows_base;
    size_t rows_size;
    size_t pos = 0;
    const char* pool_base;
    size_t pool_size;
    static constexpr bool writing = false;
    void need(size_t n) {
        if (pos + n > rows_size) throw std::runtime_error("kelvin: corrupt shard (row data OOB)");
    }
    void u32(uint32_t& v) {
        need(4);
        std::memcpy(&v, rows_base + pos, 4);
        pos += 4;
    }
    void u64(uint64_t& v) {
        need(8);
        std::memcpy(&v, rows_base + pos, 8);
        pos += 8;
    }
    void dbl(double& v) {
        need(8);
        std::memcpy(&v, rows_base + pos, 8);
        pos += 8;
    }
    void str(std::string& s) {
        uint32_t ref;
        u32(ref);
        s = pool_read(pool_base, pool_size, ref);
    }
    void boolean(bool& b) {
        uint32_t v;
        u32(v);
        b = v != 0;
    }
    void strvec(std::vector<std::string>& v) {
        uint32_t n;
        u32(n);
        v.resize(n);
        for (auto& s : v) str(s);
    }
};

template <class Ar>
void io_base(Ar& ar, RowBase& r) {
    ar.u32(r.lineno);
    ar.u64(r.src_offset);
    ar.u32(r.src_length);
    ar.str(r.mpn);
    ar.str(r.manufacturer);
}

template <class Ar>
void row_io(Ar& ar, MosfetRow& r) {
    io_base(ar, r);
    ar.dbl(r.vds_rated);
    ar.dbl(r.id_continuous);
    ar.dbl(r.rds_on);
    ar.dbl(r.qg_total);
    ar.dbl(r.coss);
    ar.dbl(r.vgs_threshold_max);
    ar.dbl(r.rth_ja);
    ar.dbl(r.rth_jc);
    ar.dbl(r.tj_max);
    ar.str(r.technology);
    ar.boolean(r.is_production);
    ar.boolean(r.datasheet_usable);
}

template <class Ar>
void row_io(Ar& ar, DiodeRow& r) {
    io_base(ar, r);
    ar.dbl(r.vrrm_rated);
    ar.dbl(r.if_avg_rated);
    ar.dbl(r.vf_typ);
    ar.dbl(r.qrr);
    ar.dbl(r.trr);
    ar.dbl(r.rth_ja);
    ar.dbl(r.rth_jc);
    ar.dbl(r.tj_max);
    ar.str(r.technology);
    ar.boolean(r.is_production);
}

template <class Ar>
void row_io(Ar& ar, CapacitorRow& r) {
    io_base(ar, r);
    ar.dbl(r.capacitance);
    ar.dbl(r.v_rated);
    ar.dbl(r.ripple_current_rms);
    ar.dbl(r.esr);
    ar.dbl(r.rth);
    ar.str(r.technology);
    ar.boolean(r.is_production);
}

template <class Ar>
void row_io(Ar& ar, ResistorRow& r) {
    io_base(ar, r);
    ar.dbl(r.resistance);
    ar.dbl(r.tolerance);
    ar.dbl(r.power_rating);
    ar.boolean(r.is_production);
}

template <class Ar>
void row_io(Ar& ar, IgbtRow& r) {
    io_base(ar, r);
    ar.dbl(r.vces_rated);
    ar.dbl(r.ic_continuous);
    ar.dbl(r.vce_sat);
    ar.dbl(r.rth_jc);
    ar.dbl(r.tj_max);
    ar.str(r.technology);
    ar.boolean(r.is_production);
}

template <class Ar>
void row_io(Ar& ar, BjtRow& r) {
    io_base(ar, r);
    ar.dbl(r.vceo_rated);
    ar.dbl(r.ic_continuous);
    ar.dbl(r.hfe_min);
    ar.dbl(r.power_dissipation);
    ar.dbl(r.tj_max);
    ar.str(r.technology);
    ar.boolean(r.is_production);
}

template <class Ar>
void row_io(Ar& ar, VaristorRow& r) {
    io_base(ar, r);
    ar.dbl(r.varistor_voltage);
    ar.dbl(r.clamping_voltage);
    ar.dbl(r.peak_surge_current);
    ar.dbl(r.max_continuous_dc_voltage);
    ar.dbl(r.capacitance);
    ar.str(r.technology);
    ar.boolean(r.is_production);
}

template <class Ar>
void row_io(Ar& ar, MagneticRow& r) {
    io_base(ar, r);
    ar.dbl(r.inductance);
    ar.dbl(r.saturation_current);
    ar.dbl(r.rated_current);
    ar.dbl(r.dcr);
    ar.dbl(r.srf);
    ar.dbl(r.turns_ratio);
    ar.str(r.device_type);
    ar.str(r.family);
    ar.boolean(r.is_production);
}

template <class Ar>
void row_io(Ar& ar, ControllerRow& r) {
    io_base(ar, r);
    ar.dbl(r.vref);
    ar.str(r.category);
    ar.strvec(r.topologies);
    ar.boolean(r.integrated_fet);
    ar.boolean(r.integrated_driver);
}

// ---- header (fixed 80 bytes, all little-endian) ----------------------------
#pragma pack(push, 1)
struct Header {
    char magic[8];
    uint32_t format_version;
    uint32_t family_id;
    uint64_t source_size;
    uint64_t content_hash;
    uint64_t row_count;
    uint64_t unreadable_row_count;
    uint64_t source_line_count;
    uint64_t physical_line_count;
    uint64_t build_id;
    uint64_t pool_size;
    uint64_t rows_size;
};
#pragma pack(pop)

// ---- generic serialize -----------------------------------------------------
template <class Row>
void serialize_body(const Shard<Row>& shard, std::string& pool_blob, std::string& rows_buf) {
    StringPool pool;
    WriteAr war{&rows_buf, &pool};
    for (const auto& row : shard.rows) {
        Row copy = row;  // io takes non-const refs; a copy keeps this pure
        row_io(war, copy);
    }
    pool_blob = pool.blob();
}

template <class Row>
uint64_t compute_build_id(const Shard<Row>& shard) {
    std::string pool_blob, rows_buf;
    serialize_body(shard, pool_blob, rows_buf);
    return fnv1a64(rows_buf.data(), rows_buf.size(), fnv1a64(pool_blob.data(), pool_blob.size()));
}

template <class Row>
std::string serialize_impl(const Shard<Row>& shard) {
    std::string pool_blob, rows_buf;
    serialize_body(shard, pool_blob, rows_buf);

    uint64_t build_id = fnv1a64(rows_buf.data(), rows_buf.size(),
                                fnv1a64(pool_blob.data(), pool_blob.size()));

    Header h{};
    std::memcpy(h.magic, kMagic, 8);
    h.format_version = kFormatVersion;
    h.family_id = static_cast<uint32_t>(shard.meta.family);
    h.source_size = shard.meta.source_size;
    h.content_hash = shard.meta.content_hash;
    h.row_count = shard.meta.row_count;
    h.unreadable_row_count = shard.meta.unreadable_row_count;
    h.source_line_count = shard.meta.source_line_count;
    h.physical_line_count = shard.meta.physical_line_count;
    h.build_id = build_id;
    h.pool_size = pool_blob.size();
    h.rows_size = rows_buf.size();

    std::string out;
    out.reserve(sizeof(Header) + pool_blob.size() + rows_buf.size());
    out.append(reinterpret_cast<const char*>(&h), sizeof(Header));
    out.append(pool_blob);
    out.append(rows_buf);
    return out;
}

// ---- generic deserialize ---------------------------------------------------
template <class Row>
Shard<Row> deserialize_impl(const std::string& bytes, Family expected) {
    if (bytes.size() < sizeof(Header))
        throw std::runtime_error("kelvin: shard too small for header");
    Header h;
    std::memcpy(&h, bytes.data(), sizeof(Header));
    if (std::memcmp(h.magic, kMagic, 8) != 0)
        throw std::runtime_error("kelvin: bad shard magic");
    if (h.format_version != kFormatVersion)
        throw std::runtime_error("kelvin: unsupported shard format version " +
                                 std::to_string(h.format_version));
    if (h.family_id != static_cast<uint32_t>(expected))
        throw std::runtime_error("kelvin: shard family mismatch");
    if (sizeof(Header) + h.pool_size + h.rows_size > bytes.size())
        throw std::runtime_error("kelvin: shard truncated");

    const char* pool_base = bytes.data() + sizeof(Header);
    const char* rows_base = pool_base + h.pool_size;

    // Verify build_id (content integrity + determinism guard).
    uint64_t expect_build = fnv1a64(rows_base, h.rows_size, fnv1a64(pool_base, h.pool_size));
    if (expect_build != h.build_id)
        throw std::runtime_error("kelvin: shard build_id mismatch (corrupt or tampered)");

    Shard<Row> shard;
    shard.meta.family = expected;
    shard.meta.source_size = h.source_size;
    shard.meta.content_hash = h.content_hash;
    shard.meta.row_count = h.row_count;
    shard.meta.unreadable_row_count = h.unreadable_row_count;
    shard.meta.source_line_count = h.source_line_count;
    shard.meta.physical_line_count = h.physical_line_count;
    shard.meta.build_id = h.build_id;

    ReadAr rar{rows_base, static_cast<size_t>(h.rows_size), 0, pool_base,
               static_cast<size_t>(h.pool_size)};
    shard.rows.resize(h.row_count);
    for (uint64_t i = 0; i < h.row_count; ++i) row_io(rar, shard.rows[i]);
    return shard;
}

// ---- file helpers ----------------------------------------------------------
std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw DataError(path, 0, "TAS catalogue file does not exist");
    f.seekg(0, std::ios::end);
    std::streamoff sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::string buf;
    buf.resize(static_cast<size_t>(sz));
    if (sz > 0) f.read(&buf[0], sz);
    return buf;
}

// Trim ASCII whitespace (matches Python str.strip for the whitespace TAS lines carry).
void trim(const char*& s, size_t& n) {
    while (n > 0 && (s[0] == ' ' || s[0] == '\t' || s[0] == '\r' || s[0] == '\n' || s[0] == '\v' ||
                     s[0] == '\f')) {
        ++s;
        --n;
    }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n' ||
                     s[n - 1] == '\v' || s[n - 1] == '\f')) {
        --n;
    }
}

// Parse the NDJSON region [begin,end) of `content`, calling extract() per non-blank line.
// start_physical_line: the 1-based physical line number of the first byte at `begin`.
template <class Row, class Extract>
void parse_region(const std::string& path, const std::string& content, size_t begin, size_t end,
                  uint32_t start_physical_line, Extract&& extract, Shard<Row>& out) {
    size_t i = begin;
    uint32_t phys = start_physical_line;
    while (i < end) {
        size_t nl = content.find('\n', i);
        size_t line_end = (nl == std::string::npos || nl > end) ? end : nl;
        const char* raw = content.data() + i;
        size_t raw_len = line_end - i;
        const char* s = raw;
        size_t n = raw_len;
        trim(s, n);
        if (n != 0) {
            out.meta.source_line_count += 1;
            json env;
            try {
                env = json::parse(s, s + n);
            } catch (const json::parse_error& e) {
                throw DataError(path, phys, std::string("JSON decode error: ") + e.what());
            }
            if (!env.is_object())
                throw DataError(path, phys, "top-level value is not an object");
            auto row = extract(env);
            if (row) {
                row->lineno = phys;
                row->src_offset = static_cast<uint64_t>(i);
                row->src_length = static_cast<uint32_t>(raw_len);
                out.rows.push_back(std::move(*row));
                out.meta.row_count += 1;
            } else {
                out.meta.unreadable_row_count += 1;
            }
        }
        if (nl == std::string::npos || nl >= end) break;
        i = nl + 1;
        phys += 1;
    }
}

uint32_t count_physical_lines(const std::string& content, size_t upto) {
    // Physical line count of content[0,upto): number of '\n' up to upto, +1 if the last byte
    // isn't a trailing newline (mirrors Python enumerate over the file's physical lines).
    uint32_t lines = 0;
    size_t i = 0;
    while (i < upto) {
        size_t nl = content.find('\n', i);
        if (nl == std::string::npos || nl >= upto) {
            lines += 1;  // final line without newline
            break;
        }
        lines += 1;
        i = nl + 1;
    }
    return lines;
}

template <class Row, class Extract>
Shard<Row> build_generic(Family fam, const std::string& path, Extract&& extract,
                         const Shard<Row>* prev) {
    std::string content = read_file(path);
    Shard<Row> shard;
    shard.meta.family = fam;
    shard.meta.source_size = content.size();
    shard.meta.content_hash = fnv1a64(content.data(), content.size());

    // Incremental tail build: new file is an append-only extension of prev's source.
    if (prev != nullptr && content.size() > prev->meta.source_size &&
        fnv1a64(content.data(), prev->meta.source_size) == prev->meta.content_hash) {
        shard.rows = prev->rows;  // reuse prefix rows verbatim
        shard.meta.row_count = prev->meta.row_count;
        shard.meta.unreadable_row_count = prev->meta.unreadable_row_count;
        shard.meta.source_line_count = prev->meta.source_line_count;
        // prev ended exactly at a line boundary (append-only) -> continue numbering after it.
        uint32_t start_line = static_cast<uint32_t>(prev->meta.physical_line_count) + 1;
        parse_region(path, content, prev->meta.source_size, content.size(), start_line,
                     std::forward<Extract>(extract), shard);
        shard.meta.physical_line_count = count_physical_lines(content, content.size());
        shard.meta.build_id = compute_build_id(shard);
        return shard;
    }

    parse_region(path, content, 0, content.size(), 1, std::forward<Extract>(extract), shard);
    shard.meta.physical_line_count = count_physical_lines(content, content.size());
    shard.meta.build_id = compute_build_id(shard);
    return shard;
}

}  // namespace

// ---- concrete wrappers -----------------------------------------------------
Shard<MosfetRow> build_mosfet_shard(const std::string& p, const Shard<MosfetRow>* prev) {
    return build_generic<MosfetRow>(Family::Mosfet, p, extract_mosfet, prev);
}
Shard<DiodeRow> build_diode_shard(const std::string& p, const Shard<DiodeRow>* prev) {
    return build_generic<DiodeRow>(Family::Diode, p, extract_diode, prev);
}
Shard<CapacitorRow> build_capacitor_shard(const std::string& p, const Shard<CapacitorRow>* prev) {
    return build_generic<CapacitorRow>(Family::Capacitor, p, extract_capacitor, prev);
}
Shard<ResistorRow> build_resistor_shard(const std::string& p, const Shard<ResistorRow>* prev) {
    return build_generic<ResistorRow>(Family::Resistor, p, extract_resistor, prev);
}
Shard<ControllerRow> build_controller_shard(const std::string& p, const Shard<ControllerRow>* prev) {
    return build_generic<ControllerRow>(Family::Controller, p, extract_controller, prev);
}
Shard<IgbtRow> build_igbt_shard(const std::string& p, const Shard<IgbtRow>* prev) {
    return build_generic<IgbtRow>(Family::Igbt, p, extract_igbt, prev);
}
Shard<BjtRow> build_bjt_shard(const std::string& p, const Shard<BjtRow>* prev) {
    return build_generic<BjtRow>(Family::Bjt, p, extract_bjt, prev);
}
Shard<VaristorRow> build_varistor_shard(const std::string& p, const Shard<VaristorRow>* prev) {
    return build_generic<VaristorRow>(Family::Varistor, p, extract_varistor, prev);
}
Shard<MagneticRow> build_magnetic_shard(const std::string& p, const Shard<MagneticRow>* prev) {
    return build_generic<MagneticRow>(Family::Magnetic, p, extract_magnetic, prev);
}

std::string serialize_shard(const Shard<MosfetRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<DiodeRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<CapacitorRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<ResistorRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<ControllerRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<IgbtRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<BjtRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<VaristorRow>& s) { return serialize_impl(s); }
std::string serialize_shard(const Shard<MagneticRow>& s) { return serialize_impl(s); }

namespace {
void write_bytes(const std::string& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("kelvin: cannot open shard for writing: " + path);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
}  // namespace

void write_shard(const std::string& p, const Shard<MosfetRow>& s) { write_bytes(p, serialize_impl(s)); }
void write_shard(const std::string& p, const Shard<DiodeRow>& s) { write_bytes(p, serialize_impl(s)); }
void write_shard(const std::string& p, const Shard<CapacitorRow>& s) { write_bytes(p, serialize_impl(s)); }
void write_shard(const std::string& p, const Shard<ResistorRow>& s) { write_bytes(p, serialize_impl(s)); }
void write_shard(const std::string& p, const Shard<ControllerRow>& s) { write_bytes(p, serialize_impl(s)); }
void write_shard(const std::string& p, const Shard<IgbtRow>& s) { write_bytes(p, serialize_impl(s)); }
void write_shard(const std::string& p, const Shard<BjtRow>& s) { write_bytes(p, serialize_impl(s)); }
void write_shard(const std::string& p, const Shard<VaristorRow>& s) { write_bytes(p, serialize_impl(s)); }
void write_shard(const std::string& p, const Shard<MagneticRow>& s) { write_bytes(p, serialize_impl(s)); }

Shard<MosfetRow> deserialize_mosfet_shard(const std::string& b) { return deserialize_impl<MosfetRow>(b, Family::Mosfet); }
Shard<DiodeRow> deserialize_diode_shard(const std::string& b) { return deserialize_impl<DiodeRow>(b, Family::Diode); }
Shard<CapacitorRow> deserialize_capacitor_shard(const std::string& b) { return deserialize_impl<CapacitorRow>(b, Family::Capacitor); }
Shard<ResistorRow> deserialize_resistor_shard(const std::string& b) { return deserialize_impl<ResistorRow>(b, Family::Resistor); }
Shard<ControllerRow> deserialize_controller_shard(const std::string& b) { return deserialize_impl<ControllerRow>(b, Family::Controller); }
Shard<IgbtRow> deserialize_igbt_shard(const std::string& b) { return deserialize_impl<IgbtRow>(b, Family::Igbt); }
Shard<BjtRow> deserialize_bjt_shard(const std::string& b) { return deserialize_impl<BjtRow>(b, Family::Bjt); }
Shard<VaristorRow> deserialize_varistor_shard(const std::string& b) { return deserialize_impl<VaristorRow>(b, Family::Varistor); }
Shard<MagneticRow> deserialize_magnetic_shard(const std::string& b) { return deserialize_impl<MagneticRow>(b, Family::Magnetic); }

Shard<MosfetRow> read_mosfet_shard(const std::string& p) { return deserialize_mosfet_shard(read_file(p)); }
Shard<DiodeRow> read_diode_shard(const std::string& p) { return deserialize_diode_shard(read_file(p)); }
Shard<CapacitorRow> read_capacitor_shard(const std::string& p) { return deserialize_capacitor_shard(read_file(p)); }
Shard<ResistorRow> read_resistor_shard(const std::string& p) { return deserialize_resistor_shard(read_file(p)); }
Shard<ControllerRow> read_controller_shard(const std::string& p) { return deserialize_controller_shard(read_file(p)); }
Shard<IgbtRow> read_igbt_shard(const std::string& p) { return deserialize_igbt_shard(read_file(p)); }
Shard<BjtRow> read_bjt_shard(const std::string& p) { return deserialize_bjt_shard(read_file(p)); }
Shard<VaristorRow> read_varistor_shard(const std::string& p) { return deserialize_varistor_shard(read_file(p)); }
Shard<MagneticRow> read_magnetic_shard(const std::string& p) { return deserialize_magnetic_shard(read_file(p)); }

bool shard_is_stale(const ShardMeta& meta, const std::string& ndjson_path) {
    std::ifstream f(ndjson_path, std::ios::binary | std::ios::ate);
    if (!f) return true;  // source gone -> a rebuild (or error) is warranted
    uint64_t size = static_cast<uint64_t>(f.tellg());
    if (size != meta.source_size) return true;  // append/edit always changes size
    // Same size: confirm with a content hash (guards an in-place edit that preserved size).
    std::string buf(size, '\0');
    f.seekg(0);
    if (size > 0) f.read(&buf[0], static_cast<std::streamsize>(size));
    return fnv1a64(buf.data(), buf.size()) != meta.content_hash;
}

}  // namespace kelvin
