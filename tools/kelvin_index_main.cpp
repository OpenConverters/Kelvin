// kelvin-index: build (incremental when the source is an append-only extension) and persist the
// per-family binary shards. Hooked into the TAS nightly after the promote step.
//
//   kelvin-index --data <TAS/data> --out <cachedir> [--family mosfet] [--check]
//
// --check: exit non-zero if any shard is stale vs its source (no build); for a cheap nightly gate.
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "Index.hpp"
#include "KelvinApi.hpp"

using namespace kelvin;

namespace {
const char* kFamilies[] = {"mosfet", "diode", "capacitor",  "resistor",
                           "controller", "igbt", "bjt", "varistor"};

int usage() {
    std::cerr << "usage: kelvin-index --data <dir> --out <dir> [--family <f>] [--check]\n";
    return 2;
}
}  // namespace

int main(int argc, char** argv) {
    std::string data_dir, out_dir, only_family;
    bool check = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << "missing value for " << a << "\n"; exit(usage()); }
            return argv[++i];
        };
        if (a == "--data") data_dir = next();
        else if (a == "--out") out_dir = next();
        else if (a == "--family") only_family = next();
        else if (a == "--check") check = true;
        else return usage();
    }
    if (data_dir.empty() || out_dir.empty()) return usage();

    std::vector<std::string> families;
    if (!only_family.empty()) families.push_back(only_family);
    else for (const char* f : kFamilies) families.emplace_back(f);

    try {
        if (check) {
            bool stale = false;
            for (const auto& fam : families) {
                Family f = api::family_from_string(fam);
                std::string shard = out_dir + "/" + std::string(family_name(f)) + ".kidx";
                std::string ndjson = data_dir + "/" + family_file(f);
                ShardMeta meta;
                bool have = false;
                try {
                    // read just the meta by loading the shard header (cheap read).
                    switch (f) {
                        case Family::Mosfet: meta = read_mosfet_shard(shard).meta; break;
                        case Family::Diode: meta = read_diode_shard(shard).meta; break;
                        case Family::Capacitor: meta = read_capacitor_shard(shard).meta; break;
                        case Family::Resistor: meta = read_resistor_shard(shard).meta; break;
                        case Family::Controller: meta = read_controller_shard(shard).meta; break;
                        case Family::Igbt: meta = read_igbt_shard(shard).meta; break;
                        case Family::Bjt: meta = read_bjt_shard(shard).meta; break;
                        case Family::Varistor: meta = read_varistor_shard(shard).meta; break;
                    }
                    have = true;
                } catch (const std::exception&) { have = false; }
                bool s = !have || shard_is_stale(meta, ndjson);
                std::cout << fam << ": " << (s ? "STALE" : "fresh") << "\n";
                stale = stale || s;
            }
            return stale ? 1 : 0;
        }

        for (const auto& fam : families) {
            ShardMeta m = api::build_and_write_index(data_dir, out_dir, fam);
            std::cout << fam << ": rows=" << m.row_count << " unreadable=" << m.unreadable_row_count
                      << " lines=" << m.source_line_count << " buildId=" << m.build_id << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "kelvin-index error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
