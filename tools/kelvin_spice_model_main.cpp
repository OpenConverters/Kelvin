// kelvin-spice-model: a part number in, a SPICE model card out — or a refusal.
//
// ABT #806. The pipeline in epic #804 needs a switching device it can simulate;
// Kelvin is where the parts live. Exit 0 with a card on stdout (or --out), or
// exit 1 with the reason on stderr. Never both, and never a card that came
// from anywhere but the catalogue.
//
//   kelvin-spice-model --catalog mosfets.ndjson --part EPC2019 [--name Q_HS]

#include "SpiceModel.hpp"

#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string catalog, part, name = "SW", out;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--catalog" && i + 1 < argc) catalog = argv[++i];
        else if (a == "--part" && i + 1 < argc) part = argv[++i];
        else if (a == "--name" && i + 1 < argc) name = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
    }
    if (catalog.empty() || part.empty()) {
        std::cerr << "usage: kelvin-spice-model --catalog <parts.ndjson> "
                     "--part <PART> [--name SW] [--out file.lib]\n";
        return 2;
    }
    std::ifstream in(catalog);
    if (!in) {
        std::cerr << "cannot open catalogue " << catalog << "\n";
        return 2;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.contains("semiconductor")) continue;
        const auto& sc = j.at("semiconductor");
        if (!sc.contains("mosfet")) continue;
        const auto& m = sc.at("mosfet");
        if (m.value("manufacturerInfo", nlohmann::json::object())
                .value("reference", "") != part)
            continue;

        const auto r = Kelvin::spice::mosfet_model(m, name);
        if (r.tier != Kelvin::spice::Tier::Datasheet) {
            std::cerr << "refused (" << Kelvin::spice::tier_name(r.tier)
                      << "): " << r.why << "\n";
            return 1;
        }
        if (out.empty()) {
            std::cout << r.card;
        } else {
            std::ofstream fo(out);
            fo << r.card;
            std::cerr << "wrote " << out << " — tier 2, "
                      << Kelvin::spice::tier_name(r.tier) << ", from "
                      << r.manufacturer << " " << r.part << "\n";
        }
        return 0;
    }
    std::cerr << "refused (none): no MOSFET named '" << part
              << "' in " << catalog
              << ". A class default would be indistinguishable from a real "
                 "model once it is in a netlist, so none is offered.\n";
    return 1;
}
