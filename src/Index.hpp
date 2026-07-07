// Kelvin index: a compact binary shard per family, built once offline from the NDJSON and
// consumed (native mmap-or-load / WASM ArrayBuffer) at query time. Separates the query index
// from the record store: the shard holds only the fields selection touches + the byte span of
// the full record in the source NDJSON, so a capacitor query scans ~30 MB instead of parsing
// 292 MB, then fetches full envelopes for the top-N by offset.
//
// Growth: the TAS nightly APPENDS to the main catalogs (librarian/promote.py), so
// build_*_shard() supports an incremental tail build — if the new file's prefix hashes to the
// old shard's content hash, only the appended lines are parsed. All widths are u64 so the
// format has no practical row cap.
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "Rows.hpp"

namespace kelvin {

// Thrown for structurally invalid NDJSON at index-build time (mirrors HS CatalogueReadError).
// A view-unreadable row is NOT this — it is counted as unreadable_row and skipped.
struct DataError : std::runtime_error {
    std::string file;
    uint32_t lineno;
    DataError(std::string f, uint32_t ln, const std::string& detail)
        : std::runtime_error(f + ":" + std::to_string(ln) + ": " + detail),
          file(std::move(f)),
          lineno(ln) {}
};

struct ShardMeta {
    Family family{};
    uint64_t source_size = 0;         // bytes of the source NDJSON at build time
    uint64_t content_hash = 0;        // fnv1a64 of the whole source file
    uint64_t row_count = 0;           // readable (indexed) rows
    uint64_t unreadable_row_count = 0;
    uint64_t source_line_count = 0;   // non-blank lines considered (== Python `total`)
    uint64_t physical_line_count = 0; // total physical lines incl. blanks (last lineno); for incremental
    uint64_t build_id = 0;            // fnv1a64 of the serialized body — content-addressed, deterministic
};

template <class Row>
struct Shard {
    ShardMeta meta;
    std::vector<Row> rows;
};

// ---- build (full or incremental) -------------------------------------------
// `prev` (optional): when non-null and the new file is an append-only extension of the shard's
// source (size grew, prefix hash matches), only the appended tail is parsed; otherwise a full
// rebuild. The result is byte-identical to a full rebuild either way ([incremental] test).
Shard<MosfetRow> build_mosfet_shard(const std::string& ndjson_path,
                                    const Shard<MosfetRow>* prev = nullptr);
Shard<DiodeRow> build_diode_shard(const std::string& ndjson_path,
                                  const Shard<DiodeRow>* prev = nullptr);
Shard<CapacitorRow> build_capacitor_shard(const std::string& ndjson_path,
                                          const Shard<CapacitorRow>* prev = nullptr);
Shard<ResistorRow> build_resistor_shard(const std::string& ndjson_path,
                                        const Shard<ResistorRow>* prev = nullptr);
Shard<ControllerRow> build_controller_shard(const std::string& ndjson_path,
                                            const Shard<ControllerRow>* prev = nullptr);
Shard<IgbtRow> build_igbt_shard(const std::string& ndjson_path, const Shard<IgbtRow>* prev = nullptr);
Shard<BjtRow> build_bjt_shard(const std::string& ndjson_path, const Shard<BjtRow>* prev = nullptr);
Shard<VaristorRow> build_varistor_shard(const std::string& ndjson_path,
                                        const Shard<VaristorRow>* prev = nullptr);
Shard<MagneticRow> build_magnetic_shard(const std::string& ndjson_path,
                                        const Shard<MagneticRow>* prev = nullptr);
Shard<AnalogRow> build_analog_shard(const std::string& ndjson_path,
                                    const Shard<AnalogRow>* prev = nullptr);
Shard<TimingRow> build_timing_shard(const std::string& ndjson_path,
                                    const Shard<TimingRow>* prev = nullptr);

// ---- serialize / deserialize ----------------------------------------------
void write_shard(const std::string& out_path, const Shard<MosfetRow>&);
void write_shard(const std::string& out_path, const Shard<DiodeRow>&);
void write_shard(const std::string& out_path, const Shard<CapacitorRow>&);
void write_shard(const std::string& out_path, const Shard<ResistorRow>&);
void write_shard(const std::string& out_path, const Shard<ControllerRow>&);
void write_shard(const std::string& out_path, const Shard<IgbtRow>&);
void write_shard(const std::string& out_path, const Shard<BjtRow>&);
void write_shard(const std::string& out_path, const Shard<VaristorRow>&);
void write_shard(const std::string& out_path, const Shard<MagneticRow>&);
void write_shard(const std::string& out_path, const Shard<AnalogRow>&);
void write_shard(const std::string& out_path, const Shard<TimingRow>&);

// In-memory serialization (used by write_shard and by [determinism] bit-identity tests).
std::string serialize_shard(const Shard<MosfetRow>&);
std::string serialize_shard(const Shard<DiodeRow>&);
std::string serialize_shard(const Shard<CapacitorRow>&);
std::string serialize_shard(const Shard<ResistorRow>&);
std::string serialize_shard(const Shard<ControllerRow>&);
std::string serialize_shard(const Shard<IgbtRow>&);
std::string serialize_shard(const Shard<BjtRow>&);
std::string serialize_shard(const Shard<VaristorRow>&);
std::string serialize_shard(const Shard<MagneticRow>&);
std::string serialize_shard(const Shard<AnalogRow>&);
std::string serialize_shard(const Shard<TimingRow>&);

Shard<MosfetRow> read_mosfet_shard(const std::string& path);
Shard<DiodeRow> read_diode_shard(const std::string& path);
Shard<CapacitorRow> read_capacitor_shard(const std::string& path);
Shard<ResistorRow> read_resistor_shard(const std::string& path);
Shard<ControllerRow> read_controller_shard(const std::string& path);
Shard<IgbtRow> read_igbt_shard(const std::string& path);
Shard<BjtRow> read_bjt_shard(const std::string& path);
Shard<VaristorRow> read_varistor_shard(const std::string& path);
Shard<MagneticRow> read_magnetic_shard(const std::string& path);
Shard<AnalogRow> read_analog_shard(const std::string& path);
Shard<TimingRow> read_timing_shard(const std::string& path);

Shard<MosfetRow> deserialize_mosfet_shard(const std::string& bytes);
Shard<DiodeRow> deserialize_diode_shard(const std::string& bytes);
Shard<CapacitorRow> deserialize_capacitor_shard(const std::string& bytes);
Shard<ResistorRow> deserialize_resistor_shard(const std::string& bytes);
Shard<ControllerRow> deserialize_controller_shard(const std::string& bytes);
Shard<IgbtRow> deserialize_igbt_shard(const std::string& bytes);
Shard<BjtRow> deserialize_bjt_shard(const std::string& bytes);
Shard<VaristorRow> deserialize_varistor_shard(const std::string& bytes);
Shard<MagneticRow> deserialize_magnetic_shard(const std::string& bytes);
Shard<AnalogRow> deserialize_analog_shard(const std::string& bytes);
Shard<TimingRow> deserialize_timing_shard(const std::string& bytes);

// Staleness: size-first (the nightly append always changes size), hash as confirmation.
bool shard_is_stale(const ShardMeta& meta, const std::string& ndjson_path);

}  // namespace kelvin
