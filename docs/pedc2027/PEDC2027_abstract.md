# Who Picked This MOSFET? Open-Source Data Standards and Component Selection for Power Converters

**Author:** Alfonso Martinez
**Affiliation:** Würth Elektronik
**E-mail:** alfonso.martinez@we-online.com
**Preferred topic:** Design Software and Tools (EDA software, AI in design, digital twins)
**Keywords:** open standards, component database, power converters, design automation, reproducibility, second sourcing, AI-assisted design

## Abstract

Component selection sits at the heart of every power-converter design, yet it still relies on vendor portals, spreadsheets and ad-hoc scripts: irreproducible, unauditable and hard to automate. As design flows increasingly delegate choices to software — including AI assistants — reproducibility and auditability must become properties of the design data itself. We present a three-layer, fully open-source answer.

The first layer is a family of vendor-neutral, machine-readable data formats developed under the Power Sources Manufacturers Association (PSMA). A common root (PEAS, the Power Electronics Agnostic Structure) fixes the shared physics in strict SI units; sibling formats cover every component class, from semiconductors and capacitors to magnetics and connectors, while the Topology Agnostic Structure (TAS) captures a complete design — requirements, components, connections and results — in one document.

The second layer is an open database of more than 670,000 real, validated parts, normalised from manufacturer datasheets and checked for physical plausibility; records that fail verification are quarantined with a stated reason, never silently served.

The third layer, Kelvin, is an open-source selection engine: a meticulous librarian, not a black box. From the design requirements it returns, in a fraction of a second, a ranked shortlist of real parts — every rejection counted with its reason, every candidate carrying its design margins — ready to be reviewed and signed off like any engineering document. The same requirements always yield the same shortlist, and equivalent parts from other manufacturers turn second-sourcing studies into a single query.

## Speaker bio (draft — edit as needed)

Alfonso Martinez is a power-electronics engineer at Würth Elektronik. In his free time he created the OpenMagnetics open-source magnetics-design toolchain and is a lead author of the PSMA-hosted "Agnostic Structure" schema family and the open TAS component database, with a focus on reproducible, physics-checked engineering data and deterministic design tools.
