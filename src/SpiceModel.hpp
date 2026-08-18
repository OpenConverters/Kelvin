#pragma once
// A switching device, as a SPICE model card — with the tier it earned.
//
// ABT #806 (epic #804). A layout gives "Q1, value BSC0902NS". A transient that
// means anything needs Coss(V), Qg, Rds(on), the body diode and its recovery.
// Kelvin already holds those numbers; this turns them into something ngspice
// can run, and — the part that matters — refuses when it cannot.
//
// WHY THE TIER IS THE PRODUCT. A fabricated FET model produces a plausible
// spectrum, and nothing downstream can tell from the output that it was
// invented. This project has had that lesson four times over (#247, #316,
// #391, #557: parts that looked real and were not), so the model card carries
// its tier, its provenance and its assumptions in comments that survive into
// whatever deck includes it:
//
//   1 VENDOR      the manufacturer's own .model/.subckt for that exact part.
//                 Not produced here — Kelvin does not hold vendor model files —
//                 and reported as unavailable rather than approximated.
//   2 DATASHEET   a first-order VDMOS fitted to the catalogue's datasheet
//                 electrical block. What this file makes.
//   3 CLASS       a stand-in from the package/voltage/current class. NOT
//                 produced. A class default is indistinguishable from a real
//                 model once it is in a netlist, and a spectrum computed from
//                 one would be a guess wearing a simulation's authority.
//   4 NONE        refuse, and say which field was missing.
//
// WHAT A TIER-2 MODEL IS AND IS NOT. The capacitances, the threshold, the
// on-resistance and the body diode come from the datasheet. The SHAPE — a
// first-order square-law channel, a fixed Cgd ratio between its two ends, a
// single-time-constant recovery — is a modelling convention, not a
// measurement. It reproduces edge rates and ringing to the accuracy those
// conventions allow, which is why ABT #810 exists to measure how much that is.

#include <nlohmann/json.hpp>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace Kelvin::spice {

enum class Tier { Vendor = 1, Datasheet = 2, ClassDefault = 3, None = 4 };

inline const char* tier_name(Tier t) {
    switch (t) {
        case Tier::Vendor: return "vendor";
        case Tier::Datasheet: return "datasheet-parameterised";
        case Tier::ClassDefault: return "class-default";
        default: return "none";
    }
}

struct ModelResult {
    Tier tier = Tier::None;
    std::string name;          // the .model name a deck instantiates
    std::string card;          // the .model line(s), with provenance comments
    std::string part;          // manufacturer part number
    std::string manufacturer;
    std::string why;           // on refusal: exactly what was missing
    std::vector<std::string> assumptions;   // every modelling convention used
    nlohmann::json used;       // the datasheet fields this stands on
};

namespace detail {
inline std::optional<double> num(const nlohmann::json& j, const char* key) {
    if (!j.contains(key)) return std::nullopt;
    const auto& v = j.at(key);
    if (v.is_number()) return v.get<double>();
    // dimensionWithTolerance-shaped: prefer nominal, else the midpoint
    if (v.is_object()) {
        if (v.contains("nominal") && v.at("nominal").is_number())
            return v.at("nominal").get<double>();
        if (v.contains("minimum") && v.contains("maximum") &&
            v.at("minimum").is_number() && v.at("maximum").is_number())
            return 0.5 * (v.at("minimum").get<double>() +
                          v.at("maximum").get<double>());
        if (v.contains("maximum") && v.at("maximum").is_number())
            return v.at("maximum").get<double>();
        if (v.contains("minimum") && v.at("minimum").is_number())
            return v.at("minimum").get<double>();
    }
    return std::nullopt;
}
inline std::string g(double v, int sig = 4) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*g", sig, v);
    return b;
}
}  // namespace detail

// `mosfet` is the catalogue's semiconductor.mosfet node (manufacturerInfo +
// datasheetInfo). `name` is what the deck will call the model.
inline ModelResult mosfet_model(const nlohmann::json& mosfet,
                                const std::string& name = "SW") {
    ModelResult r;
    r.name = name;
    const auto& mi = mosfet.contains("manufacturerInfo") ? mosfet.at("manufacturerInfo")
                                                         : nlohmann::json::object();
    r.manufacturer = mi.value("name", "");
    r.part = mi.value("reference", "");
    if (!mi.contains("datasheetInfo") || !mi.at("datasheetInfo").contains("electrical")) {
        r.tier = Tier::None;
        r.why = "the record carries no datasheetInfo.electrical block, so there "
                "is nothing to fit a model to";
        return r;
    }
    const auto& e = mi.at("datasheetInfo").at("electrical");
    const auto& part = mi.at("datasheetInfo").contains("part")
                           ? mi.at("datasheetInfo").at("part")
                           : nlohmann::json::object();

    // The fields a first-order VDMOS cannot be built without. Named
    // individually on refusal: "no model" is not an answer a caller can act on.
    const auto vth = detail::num(e, "gateThresholdVoltage");
    const auto rds = detail::num(e, "onResistance");
    const auto ciss = detail::num(e, "inputCapacitance");
    const auto coss = detail::num(e, "outputCapacitance");
    const auto crss = detail::num(e, "reverseTransferCapacitance");
    std::vector<std::string> missing;
    if (!vth) missing.push_back("gateThresholdVoltage");
    if (!rds || !(*rds > 0)) missing.push_back("onResistance");
    if (!ciss) missing.push_back("inputCapacitance");
    if (!coss) missing.push_back("outputCapacitance");
    if (!crss) missing.push_back("reverseTransferCapacitance");
    if (!missing.empty()) {
        r.tier = Tier::None;
        r.why = "cannot build a datasheet model for " + r.part + ": missing ";
        for (size_t i = 0; i < missing.size(); ++i)
            r.why += (i ? ", " : "") + missing[i];
        r.why +=
            ". A class default would fill these in and be indistinguishable "
            "from a real model in the netlist, so it is not offered.";
        return r;
    }
    if (*crss > *ciss || *crss > *coss) {
        r.tier = Tier::None;
        r.why = "the record is not physical: reverse-transfer capacitance "
                "exceeds input or output capacitance for " + r.part;
        return r;
    }

    const double vgs_spec = detail::num(e, "onResistanceVgs").value_or(10.0);
    const double id_spec = detail::num(e, "onResistanceId").value_or(1.0);
    const double vds_max = detail::num(e, "drainSourceVoltage").value_or(0.0);
    const double vf = detail::num(e, "bodyDiodeForwardVoltage").value_or(0.0);
    const double qrr = detail::num(e, "reverseRecoveryCharge").value_or(0.0);
    const double qg = detail::num(e, "totalGateCharge").value_or(0.0);

    // Fit Kp to the ON-RESISTANCE, not to the test current. A switch spends its
    // conduction time in TRIODE, where Ids = Kp[(Vgs-Vth)Vds - Vds^2/2] and the
    // small-signal channel resistance is 1/(Kp(Vgs-Vth)). Fitting Kp from the
    // datasheet's Id in SATURATION instead (Kp = 2Id/Vov^2) is the obvious
    // mistake and it is badly wrong in the direction that matters: on the
    // EPC2019 it produced a 360 mOhm channel for a 50 mOhm part, because the
    // "Id" beside an Rds(on) figure is a test current, not a saturation
    // current. Conduction loss and edge damping would both have been ~7x off.
    const double vov = std::max(vgs_spec - *vth, 0.1);
    // A tenth of Rds(on) goes to series drain resistance — the drift/metal
    // share that does not scale with gate drive. A convention, and stated.
    const double rd = 0.1 * *rds;
    const double r_chan = *rds - rd;
    const double kp = 1.0 / std::max(r_chan * vov, 1e-12);

    const double cgs = std::max(*ciss - *crss, 1e-15);
    const double cgd_max = *crss;
    const double cgd_min = *crss / 10.0;      // convention, stated below
    const double cjo = std::max(*coss - *crss, 1e-15);

    std::string c;
    c += "* ------------------------------------------------------------------\n";
    c += "* " + r.part + " (" + r.manufacturer + ")";
    if (part.contains("technology")) c += ", " + part.value("technology", "");
    c += "\n* TIER 2 — datasheet-parameterised. NOT a vendor model.\n";
    c += "* Built by Kelvin from the catalogue's datasheet electrical block.\n";
    c += "* From the datasheet: Vth=" + detail::g(*vth) + " V, Rds(on)=" +
         detail::g(*rds) + " ohm at Vgs=" + detail::g(vgs_spec) + " V / Id=" +
         detail::g(id_spec) + " A, Ciss=" + detail::g(*ciss * 1e12) +
         " pF, Coss=" + detail::g(*coss * 1e12) + " pF, Crss=" +
         detail::g(*crss * 1e12) + " pF";
    if (qg > 0) c += ", Qg=" + detail::g(qg * 1e9) + " nC";
    if (vds_max > 0) c += ", Vds(max)=" + detail::g(vds_max) + " V";
    c += ".\n";
    c += "* Model conventions, none of them measured:\n";
    c += "*   - first-order channel fitted in TRIODE so that Rds(on) is\n";
    c += "*     reproduced at the datasheet's own Vgs; 10% of it is carried as\n";
    c += "*     series drain resistance\n";
    c += "*   - Cgd steps between Crss and Crss/10 (a real Cgd collapses with\n";
    c += "*     Vds; this is the standard two-point stand-in)\n";
    c += "*   - capacitances taken at the datasheet's measurement Vds\n";
    if (qrr > 0)
        c += "*   - body-diode recovery from Qrr as a single time constant\n";
    else
        c += "*   - no reverse recovery in the record (0 or absent): the body\n"
             "*     diode is modelled without it\n";
    c += "* ------------------------------------------------------------------\n";
    c += ".model " + name + " VDMOS(nchan";
    c += " Vto=" + detail::g(*vth);
    c += " Kp=" + detail::g(kp);
    c += " Rd=" + detail::g(rd);
    c += " Rs=0";
    c += " Cgs=" + detail::g(cgs);
    c += " Cgdmax=" + detail::g(cgd_max);
    c += " Cgdmin=" + detail::g(cgd_min);
    c += " Cjo=" + detail::g(cjo);
    if (vds_max > 0) c += " Bv=" + detail::g(vds_max);
    if (vf > 0) c += " Vj=" + detail::g(std::min(vf, 1.2));
    if (qrr > 0 && id_spec > 0)
        c += " Tt=" + detail::g(qrr / std::max(id_spec, 1e-9));
    c += ")\n";

    r.tier = Tier::Datasheet;
    r.card = c;
    r.assumptions = {
        "first-order channel fitted in triode to reproduce Rds(on) at the "
        "datasheet Vgs; 10% of Rds(on) carried as series drain resistance",
        "Cgd modelled as a two-point step between Crss and Crss/10",
        "capacitances taken at the datasheet's stated measurement Vds",
        qrr > 0 ? "body-diode recovery as a single time constant from Qrr"
                : "no body-diode reverse recovery (absent from the record)"};
    r.used = {{"gateThresholdVoltage", *vth}, {"onResistance", *rds},
              {"onResistanceVgs", vgs_spec}, {"onResistanceId", id_spec},
              {"inputCapacitance", *ciss}, {"outputCapacitance", *coss},
              {"reverseTransferCapacitance", *crss},
              {"drainSourceVoltage", vds_max},
              {"bodyDiodeForwardVoltage", vf},
              {"reverseRecoveryCharge", qrr}, {"totalGateCharge", qg}};
    return r;
}

// Tier 1 is a lookup Kelvin cannot perform: it holds parametric data, not
// vendor model files. Saying so explicitly keeps the tier honest — an absent
// vendor model must never silently become a datasheet fit that CLAIMS to be
// one.
inline ModelResult vendor_model_unavailable(const std::string& part) {
    ModelResult r;
    r.tier = Tier::Vendor;
    r.part = part;
    r.why = "Kelvin holds parametric datasheet data, not vendor SPICE model "
            "files; tier 1 must come from the manufacturer's own library";
    return r;
}

}  // namespace Kelvin::spice
