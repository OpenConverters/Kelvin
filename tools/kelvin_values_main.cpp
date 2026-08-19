// kelvin-values: refdes + manufacturer part number in, component values out.
//
// A CAD export does not have to carry values. Altium's ODB++ writes the
// manufacturer part number in the component record and nothing else, so a
// board full of Würth capacitors arrives at a layout tool as a board full of
// nameless two-terminal parts — and the PDN model, which needs capacitances,
// correctly refuses to invent them. Kelvin holds those part numbers.
//
//   faraday_cli board --parts-out parts.csv        # refdes,partNumber
//   kelvin-values --catalog capacitors.ndjson --in parts.csv --out values.csv
//   faraday_cli board --values values.csv          # now the PDN can run
//
// Unresolved parts are REPORTED, never filled with a plausible value: a wrong
// capacitance is a wrong impedance curve that looks exactly like a right one.

#include "SpiceModel.hpp"   // for the shared json include and value helpers

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string trim(std::string s) {
    const char* ws = " \t\r\n\"";
    const size_t a = s.find_first_not_of(ws);
    const size_t b = s.find_last_not_of(ws);
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// SI, with a unit, so the consumer never has to guess: "100pF", "4.7uF".
std::string format_si(double v, const char* unit) {
    struct Step { double mult; const char* prefix; };
    static const Step steps[] = {{1e-12, "p"}, {1e-9, "n"}, {1e-6, "u"},
                                 {1e-3, "m"}, {1.0, ""}};
    for (const auto& s : steps) {
        const double scaled = v / s.mult;
        if (scaled < 1000.0 || s.mult == 1.0) {
            char b[64];
            std::snprintf(b, sizeof b, "%g%s%s", scaled, s.prefix, unit);
            return b;
        }
    }
    return "";
}

struct Resolved {
    std::string value;
    std::string series, package, manufacturer;
};

// The value of a part, from whichever family the catalogue filed it under.
std::optional<Resolved> value_of(const nlohmann::json& rec) {
    static const std::pair<const char*, const char*> families[] = {
        {"capacitor", "F"}, {"inductor", "H"}, {"resistor", "Ohm"},
    };
    for (const auto& [family, unit] : families) {
        if (!rec.contains(family)) continue;
        const auto& mi = rec[family].value("manufacturerInfo", nlohmann::json::object());
        const auto& ds = mi.value("datasheetInfo", nlohmann::json::object());
        const auto& el = ds.value("electrical", nlohmann::json::object());
        static const char* keys[] = {"capacitance", "inductance", "resistance"};
        for (const char* k : keys) {
            if (!el.contains(k)) continue;
            const auto& v = el[k];
            double x = 0;
            if (v.is_number()) x = v.get<double>();
            else if (v.is_object() && v.contains("nominal") && v["nominal"].is_number())
                x = v["nominal"].get<double>();
            else continue;
            if (!(x > 0)) continue;
            Resolved r;
            r.value = format_si(x, unit);
            r.manufacturer = mi.value("name", "");
            const auto& part = ds.value("part", nlohmann::json::object());
            r.series = part.value("series", "");
            r.package = part.value("case", "");
            return r;
        }
    }
    return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> catalogs;
    std::string in_path, out_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--catalog" && i + 1 < argc) catalogs.push_back(argv[++i]);
        else if (a == "--in" && i + 1 < argc) in_path = argv[++i];
        else if (a == "--out" && i + 1 < argc) out_path = argv[++i];
    }
    if (catalogs.empty() || in_path.empty()) {
        std::cerr << "usage: kelvin-values --catalog <parts.ndjson> [--catalog ...] "
                     "--in <refdes,part.csv> [--out values.csv]\n";
        return 2;
    }

    // what the board asked about
    std::vector<std::pair<std::string, std::string>> wanted;   // refdes, part
    {
        std::ifstream in(in_path);
        if (!in) { std::cerr << "cannot open " << in_path << "\n"; return 2; }
        std::string line;
        while (std::getline(in, line)) {
            const size_t comma = line.find(',');
            if (comma == std::string::npos) continue;
            const std::string ref = trim(line.substr(0, comma));
            const std::string part = trim(line.substr(comma + 1));
            if (ref.empty() || part.empty() || ref == "refdes") continue;
            wanted.emplace_back(ref, part);
        }
    }
    if (wanted.empty()) {
        std::cerr << "no refdes,part rows in " << in_path << "\n";
        return 2;
    }

    std::map<std::string, std::string> want_parts;   // part -> "" (to fill)
    for (const auto& [ref, part] : wanted) want_parts[part];

    // one pass per catalogue, only touching the parts this board asks for
    std::map<std::string, Resolved> found;
    for (const auto& cat : catalogs) {
        std::ifstream in(cat);
        if (!in) { std::cerr << "cannot open catalogue " << cat << "\n"; return 2; }
        std::string line;
        while (std::getline(in, line)) {
            if (line.size() < 2) continue;
            bool interesting = false;
            for (const auto& [part, _] : want_parts)
                if (line.find(part) != std::string::npos) { interesting = true; break; }
            if (!interesting) continue;
            auto j = nlohmann::json::parse(line, nullptr, false);
            if (j.is_discarded()) continue;
            for (const auto& family : {"capacitor", "inductor", "resistor"}) {
                if (!j.contains(family)) continue;
                const auto& mi = j[family].value("manufacturerInfo", nlohmann::json::object());
                const std::string ref = mi.value("reference", "");
                if (!want_parts.count(ref) || found.count(ref)) continue;
                if (auto v = value_of(j)) found[ref] = *v;
            }
        }
    }

    std::ostringstream out;
    out << "refdes,value\n";
    int resolved = 0;
    std::vector<std::string> missing;
    for (const auto& [ref, part] : wanted) {
        auto it = found.find(part);
        if (it == found.end()) {
            missing.push_back(ref + " (" + part + ")");
            continue;
        }
        out << ref << "," << it->second.value << "\n";
        ++resolved;
    }

    if (out_path.empty()) std::cout << out.str();
    else {
        std::ofstream fo(out_path);
        fo << out.str();
        std::cerr << "wrote " << out_path << "\n";
    }
    std::cerr << "resolved " << resolved << " of " << wanted.size()
              << " part number(s)\n";
    if (!missing.empty()) {
        std::cerr << "NOT in the catalogue (left out rather than guessed): ";
        for (size_t i = 0; i < missing.size() && i < 8; ++i)
            std::cerr << (i ? ", " : "") << missing[i];
        if (missing.size() > 8) std::cerr << ", +" << (missing.size() - 8) << " more";
        std::cerr << "\n";
    }
    return resolved > 0 ? 0 : 1;
}
