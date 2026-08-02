# Connector EMI/EMC tooling — research record

**Date:** 2026-08-01 · **Scope:** what exists in the market, what EMI/EMC for connectors
physically means, and whether the TAS/Kelvin connector catalog (392,346 CONAS records) can
found a new OpenConverters tool.

**Method.** 37 agents across two rounds: 5 market/landscape web sweeps, 3 physics deep-dives
with equations, 2 data-fit probes against the live NDJSON, 8 synthesized tool concepts, and
24 adversarial critiques (physics / market / data, one per concept). ~24 targeted web
searches, 1042 tool calls. Every field-coverage number below was measured directly against
`TAS/data/connectors.ndjson`, twice, by independent agents.

**Outcome in one line.** The market gap is real and precisely located — nothing takes a
connector part number and returns EMC guidance — but six of the eight proposed ways to close
it were falsified by our own data. See §4 for what survived and §6 for what did not.

---

## Correction, applied 2026-08-01 after implementation

§5 of the report below claims the Holm plating-voltage screen fires on **28.3%** of parts
(tin softening) and **11.0%** (melting). **That number is wrong.** Implementing the check and
measuring it directly against all 392,346 records gives:

| | measured |
|---|---|
| parts carrying both `ratedCurrentPerContact` and `contactResistance` | 46,927 |
| fire at any Holm tier | **468 (0.99%)** |
| above the melting voltage of their own stated plating | 285 (0.61%) |
| above the softening voltage | 172 (0.37%) |
| no plating stated, above the 1.10 V ceiling of every contact metal | 4 |
| **largest ratio to melting voltage anywhere in the catalog** | **3.08x** |

The correction matters for severity. A maximum ratio of 3.08x is comfortably explained by the
two specs being written to different conventions — a bulk terminal-to-terminal LLCR that
includes the clamp and lead, versus the mated-pair constriction of IEC 60512-2-1. So the
plating-based tiers ship as SUSPICIOUS, never IMPOSSIBLE; **no real part is invalidated by
them.** The check is far more precise than the report claimed, and correspondingly less
dramatic. It is now `CONN_CONTACT_VOLTAGE` in the TAS physics validator (Blade Runner) —
see the addendum.

---

# Connector EMC Tooling — Final Recommendation

**To:** Founder, OpenConverters
**From:** Lead architect
**Date:** 2026-08-01
**Basis:** full research corpus (5 landscape sweeps, 3 physics deep-dives, 2 data-fit probes), 8 synthesized concepts, 24 adversarial critiques (physics / market / data per concept), and a direct re-measurement of all 392,346 records in `/home/alf/PSMA/TAS/data/connectors.ndjson`.

**Headline:** every one of the eight concepts was falsified in at least one dimension, several by our own file. One survives a hard re-scope, and only because its deliverable is a *difference* rather than a number. The primary recommendation is therefore sequenced so that the data work ships standalone value first, and the physics engine has an explicit kill gate.

---

## 1. What exists today

### 1.1 The market is two halves that never meet

**Part-number-indexed tools** — DigiKey/Mouser parametric search, every vendor selector and configurator, vendor model portals, and our own Kelvin — know exactly *which* connector and nothing about EMC. DigiKey exposes a `shield` yes/no attribute so unreliable it runs a standing forum thread titled ["Is this product EMI shielded?"](https://forum.digikey.com/t/is-this-product-emi-shielded/66829).

**EMC-capable tools** understand the physics and are indexed by geometry, meshes, Touchstone files and Z_t curves. None has any concept of a part number.

| Tier | Representative | Price (sourced) |
|---|---|---|
| Full-wave 3D | [Ansys HFSS](https://www.ansys.com/products/electronics/ansys-hfss), [Cadence Clarity](https://www.cadence.com/en_US/home/tools/system-analysis/em-solver/clarity.html), [CST Studio](https://www.3ds.com/products/simulia/cst-studio-suite), Keysight EMPro, [Altair Feko](https://help.altair.com/feko/) | CST: **$14.5k/yr LF, $22k/yr HF lease; $45k/$62.5k purchase** ([Fidelis FEA, Jan 2026](https://www.fidelisfea.com/post/how-much-does-cst-studio-suite-cost-and-whats-included)). HFSS unpublished, ~$10–50k/seat per reseller. |
| Harness/EMC | Ansys [EMC Plus / EMA3D Cable](https://www.ansys.com/products/electronics/ansys-emc-plus), CST Cable Studio, [EMCoS](https://emcos.com/) | Enterprise. Keyed on **user-supplied Z_t**, not part numbers. |
| Channel SI | [Siemens HyperLynx](https://www.siemens.com/en-us/products/pcb/hyperlynx/signal-integrity/), Cadence Sigrity, Zuken CR-8000 | HyperLynx **$3.5–5k/seat** (2019, [SemiWiki](https://semiwiki.com/eda/5959-mainstream-pcb-design-requires-a-complete-tool-platform-too/)). Connectors enter as imported Touchstone. |
| Cheap parametric | [Polar Si9000e](https://www.polarinstruments.eu/en/polar--si9000e.html), Simbeor (~$7.5k/yr historic), Sonnet LitePlus ($495), [AtaiTec](https://us.ataitec.com/shop/) ($1.5k–36k) | The architectural precedent: BEM solver driven from a handful of catalog-style numbers, no CAD. **No connector object.** |
| Free | [Saturn PCB Toolkit](https://saturnpcb.com/saturn-pcb-toolkit/), [LearnEMC MREMC](https://www.learnemc.com/ext/calculators/mremc/cmode.php), [rftools.io](https://rftools.io/tools/emi-radiated/), [Clemson CVEL](https://cecas.clemson.edu/cvel/emc/calculators/), IEC 60664-1 calculators | Saturn's crosstalk model is a two-conductor microstrip formula. [Standard Clarity](https://standardclarity.com/calculators/creepage-clearance/) states on its own page that it "does not address connectors specifically." |
| Open source | [openEMS](https://github.com/thliebig/openEMS), [Palace](https://github.com/awslabs/palace) (Apache-2.0, does L/C matrix extraction), [OpenParEM2D](https://openparem.org), [FastHenry2/FasterCap](https://www.fastfieldsolvers.com), [scikit-rf](https://scikit-rf.org/) | $0. All start from a mesh. **None has a connector database of any kind.** |

### 1.2 What vendors publish

Free connector models exist for roughly the **top few hundred SKUs** across four or five vendors selling 25 Gb/s+ interconnect — about **0.1% of the connector universe**, concentrated exactly where we already hold `characteristicImpedance` (11,266 RF parts).

- **Samtec** — the best free SI publisher in the industry. [3D models no-login](https://www.samtec.com/support/3dmodels/), Touchstone by [request form](https://www.samtec.com/lp/s-parameter-models/) with ~24 h turnaround, encrypted HFSS 3D components pre-installed inside HFSS, and — critically for us — **free High Speed Characterization reports on `suddendocs.samtec.com` with per-pin NEXT/FEXT aggressor-victim matrices**, no login, no referer check.
- **TE** — Touchstone + SPICE MLM/SLM behind secure access; product-spec PDFs reachable with a full Chrome header set.
- **Hirose** — per-part `.s2p` + encrypted HFSS behind *free member registration*. Structurally the most machine-friendly.
- **Molex** — deterministic model-doc PDF URLs for mass-production parts, NDA for newer.
- **Würth REDEXPERT** — verified: module 52 is `"eiCan derating"` / menu `"Derating Connectors"`, and `GET /redexpert/product/list/52` returns `{"Data": [], "Total": 0}`. The one vendor tool aimed at connector derating is **empty**.

**Nobody publishes transfer impedance or shielding effectiveness for board connectors.** SAE/USCAR-37 §5.5.3 names transfer impedance as a required characteristic and then states, verbatim: *"The responsible person will determine the requirement and procedure for this test."* That is why no public parametric shielding data exists and why none will appear.

### 1.3 What the standards define

IEC 62153-4-x (`-4-3` triaxial Z_t, `-4-4` screening attenuation, `-4-7` tube-in-tube for **mated connectors**, `-4-15` triaxial cell) are **test methods with zero pass/fail limits**. Limits live in product standards. The two places with real numbers: **MIL-DTL-38999** publishes a per-series/per-shell-class leakage-attenuation table (65–105 dB at 100 MHz) derivable from the part number, and **LV 215-1 Table 4** gives hard shield thresholds (<3/<4/<2 mΩ per junction, <10 mΩ/m Z_t @2 MHz, <50 mΩ/m @30 MHz).

The single most important structural fact: **IEC 62153-4-15 §3.7 defines the DUT as *"connector with mating connector and attached connecting cables."*** A per-part shielding boolean asserts something the governing standard does not define.

### 1.4 Where the gap actually is

The abstraction was standardised and abandoned. IBIS **ICM v1.1** (ANSI/GEIA-STD-0001, 2005) is a behavioural connector format keyed on pin lists and RLGC matrices — exactly the level a catalog can populate. I verified against the [IBIS 7.0/8.0 spec PDF](https://ibis.org/ver7.0/ver7_0.pdf): the string "ICM" appears **zero times**, and connector interconnect is now handled by §11 Interconnect Modeling (BIRD189.7), whose `.ims` files reference **Touchstone**. The format outlived its data layer, then got replaced by the one format we must never synthesize.

**The unoccupied position:** a vendor-neutral, catalog-parametric, screening-grade connector estimator with explicit confidence tiers — competing over the 99.9% that has no model, never against HFSS over the 0.1% that does.

**The counter-finding I will not hide:** the gap is unoccupied partly because the bridging data does not exist for us either. Three of our eight bridge attempts were falsified by our own file. The gap is real; our ability to fill it is *narrower than the gap*.

---

## 2. What EMI/EMC for connectors really means

Ranked by frequency × pain for a power-converter engineer specifically — not for a SerDes engineer, whose ranking is nearly inverted.

**1. Current derating at real ambient.** Asked on every design. `I_allowed = I_rated·√((T_max − T_amb)/ΔT_rated)`, plus the mandated 80% derate that both IEC 61984 and LV 215-1 invoke off the EIA-364-70D / IEC 60512-5-2 30 K-rise base curve. Not EMC, but it is the first question and the one the catalog is densest on.

**2. Connector placement relative to the switch node and the filter.** TI SLUP389 Figure 17 is the canonical failure: *"switching nodes were placed right next to the EMI filter components… The noise bypasses the filtering in this example."* The filter must be topologically *and physically* between the connector and the switch node.

**3. Ground-pin inductance raising the local reference → CM current onto the harness.** This is the most connector-specific mechanism and the one with hard numbers. [LearnEMC grounding](https://learnemc.com/grounding): 0.3 mA at 100 MHz through a 1 cm pin (≈10 nH) gives ≈2 mV of cable-driving voltage, against a ~0.4 mV FCC Class B threshold — **~14 dB over from one ground pin**. TI SLUP389: *"an induced common-mode current harmonic in the FM band of only 1 µA will fail Class 5."* Armstrong: a 10 mm × 1 mm pin at 40 mA of 16 MHz bus current drops ~40 mV.

**4. Shield termination: pigtail vs 360°.** Armstrong: a 25 mm pigtail *"ruins the cable's screening effectiveness at >30 MHz"* and ruins enclosure SE *"above say 10 MHz."* Paul 1980 (IEEE TEMC EMC-22(3):161–172) measured **up to 30 dB**. The drives industry's published budget is ≤20 mm. Note this is decided by *installation*, not by the part number.

**5. Insulation coordination at the DC link.** IEC 60664-1: clearance from rated impulse (overvoltage category) + pollution degree + altitude; creepage from working voltage + PD + CTI material group. 800 V DC / PD2 / group IIIa → **8.0 mm creepage**. Our modal pitch is 2.54 mm on 99,530 parts.

**6. Y-capacitor return path through the connector.** Armstrong: a 100 mm filter earth wire (≈100 nH) *"can ruin filter performance at >5 MHz."* LearnEMC: *"no Y-capacitors on the LISN side of the choke."* This is a pin-assignment decision.

**7. Crosstalk onto high-impedance sense/feedback nets.** Real, but the correct first-order answer is usually "don't route a 20 kΩ feedback node through a connector" — which every buck-controller datasheet says in its layout section.

**8. ESD/surge flashover from outside pins to inside pins.** Armstrong: such flashovers *"can bypass protective devices."* Requires mating sequence and pin recess — parametric fields that exist nowhere in the industry.

**9. Aperture leakage of the connector cut-out.** `SE = 20·log(λ/2d)`, `ΔSE = −20·log(n)` for n apertures. Armstrong: 20 dB at 1 GHz needs d ≤ ~16 mm. A D-sub cut-out at ~40 mm diagonal gives ~11.5 dB at 1 GHz *before any pin passes through it* — and *"a wire poked through an aperture will completely destroy any SE pretensions."*

---

## 3. What our data can and cannot support

All counts re-measured against the file; record shape is `{connector:{manufacturerInfo:{datasheetInfo:{…}}}}`.

| Quantity | Computable? | Tier | Parts covered |
|---|---|---|---|
| `ratedCurrentPerContact` (as stated) | Yes, vendor-stated | **A** | 381,118 (97.14%) |
| `operatingTemperature.maximum` | Yes | **A** | 366,933 (93.52%) |
| `positions` | Yes | **A** | 365,951 (93.27%) |
| `pitch` | Yes | **A** | 246,344 (62.79%) |
| `ratedVoltage` (unqualified scalar) | Yes, but neither working nor impulse voltage | **C** | 228,846 (58.33%) |
| `mountingStyle` | Yes | **A** | 199,978 (50.97%) |
| Pin lattice (pitch ∧ positions ∧ rows) | Yes | **A** (lattice) | 137,686 (35.09%) |
| **2-D lattice with a conductor length** (rows ≥ 2) | Yes | **B** | **40,551 (10.34%)** |
| Conductor length (tailLength ∨ matedHeight) | Yes, but two disjoint conventions; ∩ = **0 parts** | **B** | 138,093 |
| `tailLength` | Yes | **A** | 87,965 (22.42%) — 100% `pinHeaderSocket` |
| `matedHeight` | Value yes; per-part no | **A**/**C** | 53,674 parts = **1,244 distinct facts, 276 series pairs, 126 of them conflicting** |
| Plating triple (material+thickness+base) | Yes | **A** | 54,336 (13.85%) |
| `interfaceStandard` | Yes; 34.6% genuinely standardised | **A** | 48,607 → **~9,000 joinable to a pinout table, ~16,800 ceiling** |
| `contactResistance` | Yes, but per-series class bounds from 5 vendors, 84% `maximum`-only | **C** | 47,018 (11.98%) |
| `characteristicImpedance` | Yes, nameplate | **A** | 11,266 (2.87%) — 99.8% on three values |
| **Partial L_p, mutual M, ratio L_m/L** | Yes, ε-independent | **B** | 40,551 measured / 137,686 with assumed rows |
| **L_g,eff** (ground-bundle parallel partial L) | Yes, given a ground set | **B** | same |
| C_m/C, k_b | Yes as a band over ε_eff | **C** | same |
| Z₀, T_d, stub resonance | Yes as a ~2× band | **C** | same |
| Loop L with a *named* return | Yes | **B** | ~9,000 from standards; else user input |
| Pin cross-section | Imputed from pitch | **C** | 156,745 standards-backed (2.54/2.00 mm) |
| `rowPitch` | **NO** | — | **57 records vs 70,829 multi-row parts** |
| Per-pin signal/ground map (pinout) | No from catalog; yes from ~30 public standards | **B** | ~9,000–16,800 |
| `shielded` boolean | **NO** — rule cascade scores **7.0% recall (65/926)** on the only labels | — | 1,807 stated (926 `true`) |
| Shielding effectiveness in dB | **NO** | — | **0** |
| Transfer impedance Z_t | **NO** — 0 records, 0 vendors, structurally unpublishable | — | **0** |
| S-parameters / IL / RL / NEXT curves | **NO** | — | **0** |
| `creepage ∧ pollutionDegree` | **NO** | — | **exactly 0** |
| CTI / housing material group | **NO** — not a CONAS field | — | **0** |
| ΔT_rated (derating basis) | **NO** — all 2,767 `ratedCurrentReferenceTemperature` are literally `25.0` (reference *ambient*), 100% Harwin | — | **0** |
| N_energised at rating | **NO** — not a field, not standardised | — | **0** |
| `dataRate` | **NO** — field does not exist | — | **0** |
| CONAS Tier-2 `geometry{}` / `contactSystem{}` | **NO** — key never appears | — | **0 of 392,346** |
| Absolute dBµV/m | **NO** — Z_cm unknown, ±20 dB published model uncertainty | — | — |

**Three structural facts that decide everything below:**

1. **Rows are missing on 49.8% of the catalog, including Molex 0/94,255, Samtec 0/55,450, TE 0/35,192.** A pin-field model is undefined without a lattice.
2. **The information content is far below the row count.** 340,224 derating-eligible parts collapse to **889 distinct (I_rated, T_max) pairs**; 794 of 1,110 series carry a single `I_rated`. 87,712 `matedHeight` edges collapse to **1,244 facts**.
3. **Every field dense enough to found a tool on is a *scalar*, and every field an EMC claim needs is a *matrix or a curve* — and those are at 0%.**

---

## 4. Recommended tool

# Ampère — the pin-field delta comparator

> **Naming (2026-08-02):** this tool was named *Neumann* in the first draft (after Neumann's mutual-inductance formula, still the physics kernel — see "Grover/Neumann kernel"); renamed **Ampère** for a more famous name that still points at the core physics (current-to-current magnetic coupling between adjacent pins = the crosstalk).

**One line:** given a real catalog part with a *measured* pin lattice, Ampère reports how much the coupling and ground-return inductance change between two ground/signal patterns on that same part — and refuses to report anything absolute.

The entire design rests on one observation that survived all 24 critiques: **on a fixed lattice, a ratio cancels ε_eff, pin cross-section, conductor length, dV/dt, dI/dt, victim impedance, noise margin, cable length and Z_cm.** Every one of those is a thing we do not have. A difference is computable from data we own; a number is not.

### 4.1 User story

> *"I have a 2×13 2.54 mm header carrying gate drive, a current-sense return and an NTC. How many of those 26 pins should be grounds, and does interleaving buy me anything I can measure? Show it as a difference from what I have now — and then show me which parts in your catalog give me that same pattern at 3.5 mm pitch with 20% current headroom at 85 °C."*

Note what the story does **not** ask for: a millivolt, a dBµV/m, a pass/fail, a Touchstone file, or a shielding number. That is deliberate.

### 4.2 The exact computations

**Kernels.** Grover partial mutual inductance over two parallel filaments of length ℓ, separation d:

```
M(ℓ,d) = (μ₀ℓ/2π)·[ asinh(ℓ/d) − √(1+(d/ℓ)²) + d/ℓ ]
```

Partial self-inductance via geometric mean distance, `g_self = 0.44705·w` (DC) or `0.59017·w` (HF) for a square post of side w:

```
L_p = M(ℓ, g_self)     →     (μ₀ℓ/2π)·[ ln(2ℓ/r) − 3/4 ]  in the long-thin limit
```

**Return-current solve (minimum energy).** For signal pin *s* carrying 1 A with ground set G, `A = L^p[G,G]`, `b = L^p[G,s]`:

```
g = −A⁻¹(b + λ·1),   λ = (1 − 1ᵀA⁻¹b) / (1ᵀA⁻¹1)
M_loop = CᵀL^p C
L_g,eff = 1 / (1ᵀA⁻¹1)          ← falls out of the same factorization, free
```

`L_g,eff` is the parallel partial inductance of the whole ground bundle — the quantity that sets the CM drive voltage across the mated interface, and the quantitative version of Faraday's `connector-ground-spread` intuition.

**Capacitance with grounds actually grounded** (Schur complement of the Maxwell potential-coefficient matrix):

```
P̃ = P_SS − P_SG·P_GG⁻¹·P_GS ,   C_M = P̃⁻¹ ,   C_m,kl = −C_M,kl
```

**The deliverable — deltas only:**

```
Δ_xtalk(dB) = 20·log₁₀[ k_b(pattern A) / k_b(pattern B) ] ,   k_b = ¼(L_m/L + C_m/C)
Δ_ground(dB) = 20·log₁₀[ L_g,eff(A) / L_g,eff(B) ]
```

Note **20·log₁₀, not 10·log₁₀** — `L_g,eff` appears in `V = L·dI/dt`, a voltage ratio. The Poynting concept used both conventions in one sentence and was off by 2× on its own headline. Worked correctly: 3.340 nH → 0.944 nH for 1 → 8 grounds is **11.0 dB, against a naive 1/N promise of 18.1 dB.** That gap *is* the product — it kills the "just add grounds" rule of thumb with a number.

**Validity gate (adopted from the critiques, corrected in both directions):**

```
T_d = (ℓ_mated + 2·ℓ_tail)·√ε_eff / c        ← total conductor, both tails
refuse when  t_r < 6·T_d                      ← Johnson & Graham / Bogatin, not 3·T_d
```

Evaluate the gate at ε_eff = 1.0 (the most permissive bound) so the *refusal* is conservative even though T_d itself is Tier C.

**Optional, only when the user supplies terminations:** stamp `[L_p, C_maxwell]` into `faraday::mna::simulate` with real `R_near ∥ C_near` / `R_far ∥ C_far` and a trapezoidal edge. This is mandatory if we ever print a millivolt — the closed form `v = Z_v·C_m·dV/dt` has no C_v and returns 5.2 V from a 40 V aggressor through 0.128 pF at Z_v = 20 kΩ, which is larger than the aggressor swing and physically impossible.

### 4.3 The exact data fields

| Purpose | Field | Coverage |
|---|---|---|
| Lattice | `mechanical.pitch`, `.positions`, `.rows` | 137,686 |
| Row spacing | `mechanical.rowPitch` | **57 → Phase 0 backfill target** |
| Length | `familyDetails.tailLength` ∨ `mating.matesWith[].matedHeight` | 138,093 (disjoint) |
| Pin cross-section | imputed from pitch (0.64 mm sq @2.54, 0.50 mm sq @2.00) | 156,745 standards-backed |
| Ground pattern | `interfaceStandard` → public standard, else **user input** | ~9,000 derived |
| Regime | `mechanical.orientation`, `.mountingStyle` | 44.5% / 51.0% |
| Kelvin facets | `ratedCurrentPerContact`, `operatingTemperature.maximum` | 97.1% / 93.5% |
| Dimensional resolve | `PEAS::resolve_dimensional_values` everywhere | mandatory |

### 4.4 Confidence tiers, surfaced in the UI

| Tier | Meaning | Applies to |
|---|---|---|
| **A** | Vendor-stated, carried unprocessed, with field name + datasheet URL on hover | pitch, positions, rows, tailLength, I_rated, T_max, plating |
| **B** | Geometric ratio, genuinely ε-independent | `L_m/L`, `M/L_p`, `Δ_ground(dB)` |
| **C** | Involves ε_eff — emitted as a **band over ε_eff ∈ [1.0, 3.5]**, never a point | `C_m/C`, `k_b`, `Z₀`, `T_d`, any millivolt |
| **REFUSE** | Named reason, no number | everything in §4.5 |

Plus a per-part badge that is never silently defaulted: **MEASURED LATTICE** (rows present, 40,551 parts) vs **ASSUMED LATTICE** (rows imputed) vs **NO LATTICE — REFUSED**.

### 4.5 What Ampère deliberately does NOT claim

This list is the credibility spine and belongs on the tool's front page.

- **No dBµV/m, no CISPR verdict.** Z_cm is unknown and swings ±15 dB across resonances; Hertz publishes `RADIATED_MODEL_UNCERTAINTY_DB = 20.0` for exactly this reason; and the connector is one of several parallel CM sources (board return drop, heatsink capacitance, transformer interwinding C). Halving L_g,eff buys 11 dB only if the connector is ~100% of V_cm.
- **No absolute millivolts** without user-supplied R and C terminations — and then labelled Tier C.
- **No shielding effectiveness, no transfer impedance, no per-part `shielded` boolean.** The classification cascade scores 7.0% recall on the only 926 labels that exist, and IEC 62153-4-15 §3.7 makes a per-part boolean undefined.
- **No Touchstone, no EBD, no ICM, no `.subckt` file.** IBIS 8.0 §8 EBD admits only `Len/L/R/C/Fork/Endfork/Node/Pin` — **there is no mutual term in EBD anywhere**; the matrices are in §7 `.pkg`. ICM was last revised 2005 and is absent from IBIS 7.0/8.0. A synthetic `.s4p` is indistinguishable from a measurement, and the SI community would be right to reject it on sight.
- **No IEC 60664-1 compliance verdict.** `creepage ∧ pollutionDegree` = exactly 0 records; CTI is not a field; our two secondary sources for Table F.4 disagree by 1.6× at 250 V PD2/IIIa.
- **No absolute temperature.** ΔT_rated coverage is 0%, N_energised is 0%.
- **No "the optimal pinout."** Eight independent SA runs produced eight ground sets spanning 0.5 dB. The deliverable is the ground-ratio sweep (1:1 / 2:1 / 3:1 / all-signal), the top five distinct patterns, and the **invariants that hold across all of them**.
- **Nothing above the validity gate.**

### 4.6 Architecture

New repo, sibling to Kelvin/Faraday/Hertz/Kirchhoff, linking `faraday` and `hertz` as INTERFACE header libraries and `kelvin` as a static lib.

| Need | Reuse |
|---|---|
| Catalog, filter, rank, in-browser | `kelvin::api::Engine::browse/select/load_shard_bytes`; embind + HTTP-Range record fetch |
| Maxwell → RLGC, `c_mutual`, `kb` | `faraday::rlgc_from_maxwell`, `faraday::Rlgc` |
| N-conductor BEM (**only where a plane genuinely exists** — SMT mezzanine) | `faraday::bem::solve`, honouring `max_panels_per_face = 56` and its "refuse rather than lie" rule |
| Terminated-victim transient | `faraday::mna::simulate`, `sections_for` |
| Peak NEXT/FEXT in dB | `faraday::crosstalk_from_waveforms` |
| Correctness cross-check | `faraday::spice_ladder_deck` + `Kirchhoff::run_ngspice_in_process` |
| Dimensional resolve | `PEAS::resolve_dimensional_values` |
| Build/WASM/Vue/Catch2 pattern | copy `Faraday/cpp/CMakeLists.txt`, `scripts/build_wasm.sh` (**keep `-sDYNAMIC_EXECUTION=0`**), `cpp/bindings/wasm.cpp`, `web/`; Kelvin's `worker.js`/`engine.js`/`families.js` for the catalog side |

**Two corrections to the concept's stated reuse plan:**

1. **Do not replace `find_connector_ground_spread` (Screener.hpp:1933).** Reading the source: it gates on `conns.size() >= 2`, takes the max pairwise distance between *two different* edge-mounted connectors' ground centroids, requires `worst >= 0.4*diag`, and does print millimetres. Its mechanism is Franz's series-ground structure across board copper — tens of nH — not intra-connector inductance of 1–15 nH. **Add a sibling rule; deleting that one removes a defect it was written to catch.**
2. **Do not add a third radiated-field implementation.** `faraday::emc::cm_e_field` and `Hertz::radiated_efield_dbuvm` are already the same Ott monopole expression differing by whether the ×2 ground reflection is conditional. If Ampère ever touches that path, pick one and add a Catch2 test asserting agreement.

Catch2 binaries run directly, never `ctest`. Playwright always headless.

---

## 5. Runner-up concepts

**1. Holm-lite — the plating-voltage contradiction screen.** The physics critique's rescue is the single most defensible finding in the entire corpus: emit `ΔT_spot = √(θ₀² + (I_rated·R_c)²/4L) − θ₀` with `L = 2.44e-8 V²K⁻²` and flag every part whose `I_rated·R_c` exceeds the softening/melting voltage of its own stated plating (Sn 0.07/0.13 V, Ag 0.09/0.37, Au 0.08/0.43, Ni 0.22/0.65). **Zero free parameters. Two fields already co-present on 46,927 parts.** Measured: 13,261 (28.3%) exceed Sn softening, 5,170 (11.0%) exceed melting, worst case Amphenol CS 91570-114LF at 40 A × 55 mΩ = 88 W per contact. *Lost because* it is not EMC and it is not a model — it is a data-provenance detector proving that on 11–28% of parts `ratedCurrentPerContact` and `contactResistance` describe different physical objects. **It should still ship, as a Blade-Runner-class flag inside Kelvin.** So should the derated ranking column: on 3.5 mm-pitch parts at 85 °C (n=4,243), Spearman ρ vs a raw `I_rated` sort is 0.919 but **top-100 overlap is only 5/100** — the reorder is real.

**2. Volta-lite — the standards spine.** The ~30 public interface-standard net-map tables, the shield-evidence enum, and `matedHeight` republished as **276 series-pair records with [min,max] ranges and a `skuFanout` count** instead of 53,674 rows implying 53,674 measurements. *Lost because* it is the dependency, not the product — and because DigiKey [cat 315](https://www.digikey.com/en/products/filter/rectangular-connectors-headers-receptacles-female-sockets/315) / [cat 314](https://www.digikey.com/en/products/filter/rectangular-connectors-headers-male-pins/314) already expose `Mated Stacking Heights`, `Contact Shape` and `Contact Length` as free facets over 506k parts. **This is Phase 0 of the recommendation.**

**3. Poynting-lite — L_g,eff as a Faraday finding in nanohenries.** Today a 26-way ribbon with one ground pin and one with thirteen interleaved grounds are literally indistinguishable to Faraday. *Lost because* it is a sub-feature of Ampère (the same `1ᵀA⁻¹1` falls out of the same factorization), and because attaching dBµV/m to it is the credibility hazard every lens flagged.

**4. Zobel-inverse — a max-tolerable-pin-capacitance gate inside Hertz.** ~200 lines, no transcription, no maintenance tail: max C from rise-time and source impedance, max total C from **touch/leakage current** (IEC 60601/62368 — 25 pins × 1000 pF = 25 nF to chassis, which the original concept never mentioned), then the 20 dB/dec (C) vs 40–60 dB/dec (Pi) envelope to say feasible / infeasible / needs a bulkhead feedthrough. *Lost because* the market is mil-aero/medical, not power converters, and the catalog contributes 46 records.

**5. Grover-as-calculator.** Same physics as Ampère, exposed interactively inside Faraday where the layout supplies both pin positions and the ground assignment, writing nothing to disk. *Lost because* it is Ampère's degenerate case: no catalog, no cross-part comparison, no answer without a board.

---

## 6. Rejected concepts

**Paschen — Insulation-Coordination Screener.** *Killed by:* the governing inequality has the wrong sign. `pitch − w_pin` is the geodesic across a *flat* surface, i.e. a **floor**; every rib, slot, pocket and barrier makes it longer. Empirically confirmed on the only ground truth available: **1,321 of 1,561 parts (84.6%) declare a creepage larger than their own pitch**, median ratio 1.26, p90 1.67, max 5.76 — `Samtec PESC-01-40-01-01-L-VT`, pitch 0.635 mm, creepage 3.66 mm, and *clearance* 3.31 mm (5.2× pitch, which kills the clearance-only fallback too). The 78% flag rate was the model failing, not the catalog lying. Independently: the requirement side needs a CTI material group present on **zero records**, `creepage ∧ pollutionDegree` = **exactly 0**, and the tool would publish `GEOMETRICALLY-IMPOSSIBLE` against ~12,850 UL-listed parts on an imputed pin width.

**Grover — Parasitic Model Foundry.** *Killed by:* the output formats do not exist as specified. Verified against [IBIS 7.0/8.0](https://ibis.org/ver7.0/ver7_0.pdf) §8 — EBD's `[Path Description]` supports only `Len/L/R/C/Fork/Endfork/Node/Pin`; **there is no mutual term in EBD**, and the matrices live in §7 `.pkg`. ICM v1.1 dates to 2005, appears zero times in the current spec, and was superseded by §11/BIRD189.7 pointing at Touchstone — the one format the concept correctly forbids itself. Compounding: eligible set ∩ `characteristicImpedance` = **0 of 138,093**; `tailLength ∩ matedHeight` = **0 parts**, so the two length conventions can never be cross-checked; 91.3% of the deliverable is 2.54/2.0 mm and 83% through-hole, i.e. verbatim the segment where nobody runs a channel simulation.

**Schelkunoff — Shield Screener.** *Killed by:* the classification cascade scores **7.0% recall (65/926)** against the only vendor-stated labels, and **not one stated-shielded part carries a metal-shell string** — labels and evidence are disjoint vendor populations (labels: CUI + Amphenol CS; prose evidence: 91.4% Molex). **335 Micro-D/Nano-D parts state "Plastic Shell"** in the vendor's own description, from the concept's own Tier-A class. And Schelkunoff `A+R+B` on a 0.4 mm shell returns 131–1367 dB against a measured 20–40 dB, because the wall was never the limiting mechanism.

**Poynting — CISPR Bridge.** *Killed by:* a dB-convention error in the headline (3.340→0.944 nH is **11.0 dB**, not the 5.5 dB quoted, in the same sentence as `20·log₁₀(8) = 18 dB` for the baseline); `A = pitch × pin_length` computes the connector's *internal* loop, which is ~1–3% of the radiating loop once the cable is included (a 300 mm cable adds +29 dB); `L_p` requires a pin radius that is 0% populated; `k̄` is a fitted constant with no per-part data; and "screens 246,344 parts from pitch alone" reduces to `ORDER BY pitch` over 215 values of which two cover 63.6%.

**Zobel — Filtered-Connector Selector.** *Killed by:* the catalog contributes **46 records**, from 2 vendors, with 6 capacitance values, all in prose. Ten of the fourteen named vendors have **zero parts** in the catalog; Würth's own WE-D-SUB filtered line is absent despite Würth being an ingested manufacturer. Physically: an ideal 1000 pF shunt in the MIL-STD-220 50/50 Ω jig gives **0.002 dB at 150 kHz and 13.7 dB at 30 MHz** — approximately nothing across the entire conducted band Hertz owns, and the vendors' lowest tabulated point is 20 MHz. Meanwhile [EESeal Builder](https://www.eeseal.com/) is free, configures a filter for your connector, and mails a custom sample in under 48 h.

**Ampère as originally scoped** (absolute millivolts + an optimizer-as-oracle). *Killed by:* `v = Z_v·C_m·dV/dt` has no victim capacitance, so at Z_v = 20 kΩ it returns 5.2 V from a 40 V aggressor through 0.128 pF — larger than the aggressor swing. The correct charge-divider limit with a realistically compensated FB node (100 pF) is 52 mV: **100× over**. The "capacitive coupling is 9× inductive" differentiator was an artifact of omitting the one component guaranteed to be present on a feedback node.

**Volta as a published data product.** *Killed by:* **941,041 of 1,024,515 `matesWith` edges (91.9%) point at a series that does not exist under that manufacturer**; 87,712 `matedHeight` edges are 1,244 distinct facts across 276 series pairs, 126 of which carry conflicting heights; and the headline inductance claim (14.8 nH) is a single pin's *partial self*-inductance, not a loop, and ignores that a mezzanine has dozens of parallel grounds.

**Holm as a derating tool.** *Killed by:* all 2,767 `ratedCurrentReferenceTemperature` values are literally `25.0`, 100% Harwin — that is a reference *ambient*, not a rated *rise*, so **ΔT_rated coverage is 0%, not 0.71%**. The 340,224-part population yields **889 distinct answers**. CONAS already models `derating.currentVsAmbient` and `derating.currentVsEnergizedContacts`; both are 0% populated, exactly like Tier-2 geometry.

---

## 7. What would have to be acquired

Ranked by leverage per week. Note the counter-intuitive result at the bottom.

**A. Row count and row pitch, per series — 3–4 weeks. HARD DEPENDENCY.**
102,971 parts have pitch+positions but no rows; `rowPitch` exists on 57 records against 70,829 multi-row parts. Molex (94,255), Samtec (55,450) and TE (35,192) publish rows on **zero** records. This is a **series-level** target — 1,110 series, not 392k parts — and both fields are on every 2-row datasheet drawing.
*Feasibility:* Samtec HTML product pages return 200 to curl; TE's `DocumentDelivery` PDFs work with the full `Sec-Fetch-*` + `sec-ch-ua` header set (4/4 sampled returned real PDFs, avg 36 kB of text); Molex and Amphenol CS are Cloudflare/Akamai-gated and need the Playwright-MCP in-page-`fetch` trick. **Without this, Ampère's addressable population stays at 40,551.**

**B. Interface-standard pinout join — ~1 week, clerical, fully citable.**
~30 public tables: USB-C (A1/A12/B1/B12 GND), TIA-568 T568A/B with the (3,6) split pair, HDMI grounds at 2/5/8/11/17, DisplayPort at 2/5/8/11/16, SATA 1/4/7, PCIe CEM, M12 codings A/B/C/D/X/K/L/T/Y, IEC 60320, DE-9 TIA-574, DB-25 pin 7. **Realistic yield ~9,000 parts, not the 22,000 originally claimed** — 4,157 of 9,510 D-Subs lack a position count so DE-9 vs DB-25 cannot be chosen, and "USB" is a generic 561-part bucket. Backfilling positions raises the ceiling to ~16,800. This is the only route to a *real* ground pattern instead of a user guess.

**C. Electrical parametrics from datasheet PDFs — 4–6 weeks.**
312,456 URLs → **167,613 unique documents** (1.86 parts/doc), so ~168k fetches not 312k. **118,119 (37.8%) return a real PDF to plain curl** (Sullins 82,518, TE 21,909, Harwin 5,173, JST 4,039, CUI 2,589, Würth 1,891); 86,541 are genuine PDFs behind a WAF (Molex, Amphenol CS → Playwright); 103,913 are HTML pages needing one extra hop. Text-layer yield 22/24 sampled; **Harwin drawings are frequently vector-only and need OCR**. Projected: DWV 4.4% → ~45%, insulation resistance 11.4% → ~55%, contact resistance 12.0% → ~62%, recommended footprint 0% → 50–70%.

**D. Per-series derating curves — 4–6 weeks, high value but not for Ampère.**
The only path that converts the derating column from Tier C to Tier A. Target the ~46,068 power-family parts ≥5 A. Current rating appears in 10 of 24 sampled PDFs; the *curve* in maybe 25%.

**E. Pin cross-section from drawings — DEPRIORITISE.**
Measured sensitivity: a **2× error in assumed pin width moves L_p by ±13% and loop L by ±18% — about 1.5 dB**, because inductance depends on radius only logarithmically. The 2.54 mm → 0.64 mm sq and 2.00 mm → 0.50 mm sq conventions are already standards-backed on 156,745 parts. This is a lot of extraction work for ~1.5 dB.

**F. S-parameters — DO NOT AGGREGATE.**
Samtec (email form, 24 h), TE (secure access), Hirose (free member registration, per-part `.s2p`), Molex (NDA for newer parts). Realistically 10–20k parts have an actual Touchstone file, all behind single-vendor click-through licences. **Ingest a user-supplied `.s4p`; never host one.**

**G. Shielding rules — DO NOT BUILD.**
7.0% recall measured. The only citable enrichment is MIL-DTL-38999's per-shell-class leakage-attenuation table derivable from a part number, and `38999` appears in the corpus essentially only through one AirBorn block.

**H. IEC 60664-1 Ed.3.1 — buy it or quote nothing.**
Our two secondary sources disagree by 1.6× at 250 V PD2/IIIa. That is not a paywall problem; it means the requirement side is *unknown to us*.

---

## 8. Phased build plan

### Phase 0 — the data spine (6–8 weeks). Ships standalone value; no physics.

**Deliverables:** rows + rowPitch backfilled per series for the top ~300 series (items A); the ~30 interface-standard net-map tables as a small hand-verified citable reference joined at query time (B); `pitch`, `rows`, `positions`, `temp_max_c` added to Kelvin's `ConnectorRow` and `Browse.hpp` facets (a Kelvin code change — `KELVIN_EXTRACTOR_HASH` in `Kelvin/CMakeLists.txt` already handles cache invalidation); the derated sort key `I_rated·√((T_max−T_amb)/ΔT_basis)` with user-selectable `ΔT_basis ∈ {30,45} K`; the Holm plating-voltage contradiction flag; a `shieldEvidence` enum (`vendor-stated` / `by-interface-spec` / `none`) with **no boolean and no dB**; `matedHeight` republished as 276 series-pair ranges with `skuFanout`.

**Validation gate:**
- Every backfilled field carries a per-field provenance URL that resolves, and a hand-audited sample of 100 rows values round-trips against the source drawing. Anything below 98% agreement halts the pass.
- The `shieldEvidence` enum is scored against the 926 `true` / 881 `false` vendor labels and **published with its recall**, whatever it is.
- The Holm flag is spot-checked against 20 vendor datasheets; every fire must be explicable as either a real spec contradiction or a known measurement-convention mismatch (bulk-terminal LLCR vs mated-pair constriction), and the split must be reported.

**Kill criterion:** if the rows backfill lands below ~60% of the pitch+positions population, Phase 1's addressable set stays at 40,551 and Phase 1 should not start.

### Phase 1 — the delta engine (8–10 weeks). Hard gate.

**Deliverables:** the Grover Ampère kernel, minimum-energy return solve, `L_g,eff`, Schur-complemented C_m, and the ground-ratio sweep (1:1 / 2:1 / 3:1 / all-signal) reported **only as Δ dB with a Tier badge and a MEASURED/ASSUMED lattice flag**. The `t_r ≥ 6·T_d` refusal. Optional MNA-terminated victim waveforms when the user supplies R and C. WASM + Vue shell on the Faraday pattern.

**Validation gate — all four must pass:**
1. **Independent solver cross-check.** Partial-L and capacitance matrices reproduced within 5% against **FastHenry2 + FasterCap** on 5–10 canonical pin fields. Two algebraic routes through the same magnetostatic assumption agreeing to 1.7% is a unit test, not validation — that claim must be retired.
2. **Circuit cross-check.** `faraday::spice_ladder_deck` → `Kirchhoff::run_ngspice_in_process` agrees with `faraday::mna::simulate` to the tolerance Faraday's own `faraday_xcheck` already achieves (0.01 dB near-end, 0.05 dB far-end).
3. **The measured-delta gate — this is the one that decides the phase.** Reproduce Samtec's published SEAM/SEAF 1:1 vs 2:1 result — worst-case single-ended NEXT 2.97% vs 11.70% at a 30 ps edge, **+11.9 dB** — to within **±3 dB**. Free, public, a named part, identical connector, identical stack height, only the S:G pattern changed. This is the only apples-to-apples measured delta in existence for a connector pin field. **If we miss by more than 3 dB, Phase 1 does not ship.**
4. **Stability gate.** Re-run the top-5 pattern ranking over the box `Z_v ∈ [0.2, 20] kΩ × C_v ∈ [1, 1000] pF × ε_eff ∈ [1.0, 3.5]`. If the top five are not stable across that box, the honest deliverable collapses to the qualitative invariants Altium and Cadence already publish for free, and **Phase 1 ships as the ground-ratio sweep only, with no per-net recommendation.** Declare this criterion before running it.

Secondary ordering check against Sercu (IEEE EPEP 1998) published optimal S:G patterns.

### Phase 2 — Faraday integration (3–4 weeks).

**Deliverables:** a **new sibling rule** in Faraday's screener (not a replacement for `find_connector_ground_spread`, which is a different mechanism) that, when an MPN is bound to a refdes, reports intra-connector `L_g,eff` in nanohenries and a rank against alternative patterns on the same part. The layout supplies pin positions and the ground assignment, which removes the two largest imputations. Never emits dBµV/m.

**Validation gate:** on a board where only the ground-pin count changes, the rule's ordering must be monotone and match the Phase 1 engine to within 0.5 dB; and it must not co-fire with the existing spread rule on a board where only one connector exists.

**Deploy gate (all phases):** after `rsync`/atomic swap, `curl` the live file and `sha256sum` it against a clean-`HEAD` rebuild, **per artifact** — the WASM engine and the JS bundle separately — then boot the app headless and confirm a real result renders with zero console errors.

---

## 9. Honest risks

**Resolved-by-design (stated for completeness):** the absolute-millivolt error (deltas only, or MNA with a real C_v); the optimizer-as-oracle (sweep + top-5 + invariants); the dB convention (20·log₁₀, verified); the validity gate (6·T_d on total conductor length).

**Unresolved. These survived the critiques and I have not fixed them.**

**R1 — The image-plane orientation is wrong for through-hole headers, and it is unsafe.**
For a vertical THT header — **148,731 parts, 74% of everything with a mounting style** — the pins are *normal* to the reference plane, where the image of a current element is co-directed and mutual inductance is **reinforced**, not reduced. Applying the parallel-conductor image kernel there under-predicts M by roughly 2× in the direction that tells an engineer their connector is fine when it isn't. And `orientation` is 44.5% populated, so we cannot always tell which regime we are in. **This must be fixed before Phase 1 ships, and the fix is not a coefficient — it is a different kernel per orientation, with a refusal when orientation is unknown.**

**R2 — "Ratios cancel ε_eff" is only half true.**
`L_m/L` is genuinely geometric. `C_m/C` is ε-independent **only in a homogeneous dielectric**, and a connector is air + LCP/PBT/PA + FR4 with the mutual path through plastic and the self path mostly through air. So `k_b`, which mixes both, carries the ε band. The tool must report inductive and capacitive ratios *separately*, with the capacitive one banded. This is a permanent limitation of the data, not a bug.

**R3 — Return-path assignment dominates and is unknown for ~95% of parts.**
Measured: same geometry, ℓ = 24 mm, 2.54 mm pitch — `L_loop` = 21 nH with an adjacent ground vs 38.6 nH with the return at the far end of a 2×20. That is 1.8×, **5.3 dB**, versus the ±18% (1.5 dB) that a 2× pin-width error costs. The standards join fixes ~9,000–16,800 parts. For everything else the ground pattern is a user input, and the answer belongs to the user's assumption, not to us.

**R4 — Rows are missing on 49.8% of the catalog including all three largest vendors.**
Phase 0 is a hard dependency, not a nice-to-have, and it targets vendors who are WAF-gated. If it under-delivers, the product's addressable set is 40,551 parts (10.34%) and the "392k catalog" framing is not available to us.

**R5 — `contactResistance` is spec-writing conservatism, not physics.**
Five vendors (Amphenol CS 31,504 of 47,018), per-series class bounds not per-part (WAGO: 2 distinct values across 1,463 parts; Würth: 14 across 3,827), 84% `maximum`-only, and vendor medians spanning 1 mΩ to 30 mΩ — a 30× spread that mostly encodes *what was measured* (cage-clamp bulk termination vs mated-pair-plus-lead per IEC 60512-2-1). Any output ranked on R_c ranks datasheet policy across vendors.

**R6 — The stability gate may fail, and I do not know which way.**
The physics critique predicts the top-5 ranking is bistable in a guessed `Z_v`, because capacitive coupling is screened by an *interposed* ground while inductive coupling is screened by a ground *adjacent to the aggressor*, and those are frequently different positions. If R6 fires, what remains is the ground-ratio sweep and the qualitative invariants — which is a feature, not a product. **I have written this as a declared kill criterion rather than hoping it passes.**

**R7 — Usage frequency is low and the market lens said IMAGINARY.**
The ground-pattern decision is once per project, at schematic phase. The counter is that Phase 0's output is a *browse* action inside Kelvin ("rank 3.5 mm-pitch parts by current headroom at 85 °C") rather than a tool session. But this is the weakest dimension of the recommendation and I am not going to dress it up.

**R8 — Samtec is both our validation source and 14.1% of the catalog, and gives the answer away for its own parts.**
We are differentiated only over the other 85.9% — which is precisely where no measurement exists to validate against, ever. Our credibility on those parts rests entirely on the fact that the *ratio* transfers, and on the honesty of the refusals.

**R9 — Redistribution exposure, unaddressed.**
Phase 0 publishes a normalised dataset derived from 16 manufacturers' click-through-licensed parametric catalogs. Raised by the Volta market critique, and it applies to the Kelvin facets as much as to any "data product" framing. This needs a legal read before Phase 0 ships publicly, and it is outside my competence.

**R10 — The safety mechanism carries the uncertainty it protects against.**
`T_d ∝ √ε_eff`, the least-known quantity. Mitigated by gating at ε_eff = 1.0 so refusals are conservative, but the gate is structurally Tier C and should be labelled as such rather than presented as a hard boundary.

---

## Closing position

The market gap is real and precisely located: nothing on earth takes a connector part number and returns EMC-relevant guidance. But our own file falsifies six of the eight ways we proposed to close it, and the seventh (Holm) is a thermal tool wearing an EMC badge.

What is left is smaller than the gap and honest about it: **a comparator that tells you how much better one ground pattern is than another on a part you actually hold, sitting on a Kelvin data spine that is worth building regardless.** It refuses more than it emits. That is the correct ratio given what we can measure, and the refusals are the part a skeptical engineer will check first.

If the Samtec +11.9 dB gate fails at Phase 1, we will have spent 6–8 weeks producing genuinely useful catalog data and will have proved, with a measurement, that the physics layer should not be built. That is an acceptable outcome and it should be stated as one up front.


---

# Appendix A — Market-gap summary (synthesis stage, verbatim)

The market splits into two halves that never meet, and I could not find a single product bridging them across 24 targeted searches spanning the commercial EDA, vendor-portal, open-source and free-calculator landscapes.

PART-NUMBER-INDEXED tools — distributor parametric search, vendor selectors and configurators, vendor model portals, and our own Kelvin — know exactly *which* connector and nothing whatsoever about EMC. DigiKey and Mouser expose a `shield` yes/no attribute so unreliable that DigiKey runs a standing forum thread titled "Is this product EMI shielded?".

EMC-CAPABLE tools — HFSS, Cadence Clarity, CST (~$14.5k–$62.5k/yr), Ansys EMA3D/EMC Plus, Altair Feko, HyperLynx ($3.5k–5k/seat), Sigrity, Zuken CR-8000, Polar Si9000e, plus every free calculator (Saturn, Mantaro, EEWeb, everythingRF, Pasternack, Clemson CVEL, LearnEMC) — understand the physics but are indexed by geometry, meshes, Touchstone files and Zt curves. None has any concept of a part number, and not one free calculator anywhere accepts a connector as an object: nothing takes pitch + rows + positions + a signal/ground pattern.

The structural reason is economic and permanent. Every commercial tool needs either 3D CAD or a pre-solved Touchstone. Free models exist only for the few hundred 25 Gb/s+ SKUs whose vendors run an HFSS shop — roughly 0.1% of the connector universe, concentrated precisely in the RF/high-speed families where we already hold `characteristicImpedance` (11,266 parts). For the other 99.9% — 99,530 parts at 2.54 mm pitch, 57,215 at 2.00 mm, every terminal block and wire-to-board — no model exists anywhere, free or paid, and none ever will, because nobody will fund a full-wave characterisation of a $0.12 pin header. Solver vendors would commoditise themselves by closing the gap; connector vendors would be helping you buy a competitor's part.

The abstraction was even standardised and then abandoned: IBIS ICM (ANSI/GEIA-STD-0001, ratified 2006) is a behavioural connector format keyed on pin lists, segment families and frequency-dependent RLGC matrices — exactly the level a catalog can populate — and it has near-zero tool support today because the only parties able to populate it never did. The format outlived its data layer by twenty years.

Three near-misses define the boundary precisely. Samtec's online Sig-Sim is parametric over *catalog configuration* (family, S:G ratio, stack height, length) but is interpolation over a pre-solved HFSS corpus Samtec paid for, single-vendor, and a sales funnel rather than a product. Feko's measured cable-property database is the only place a parametric property database substitutes for geometry — for cables, inside an expensive solver. IEC 60664-1 creepage calculators are genuinely parametric, genuinely free, genuinely useful, and completely disconnected from any connector database.

The unoccupied position is therefore precise: a vendor-neutral, catalog-parametric, screening-grade connector electrical/EMC estimator with explicit confidence tiers — the connector equivalent of what Polar's Si9000e is for traces — sitting one tier below Touchstone and never pretending to be a solver. It should compete over the 99.9% that has no model, not against HFSS over the 0.1% that does.


---

# Appendix B — Data verdict (independent re-measurement of all 392,346 records)

I re-measured all 392,346 records directly rather than trusting the corpus (the record shape is `{connector:{manufacturerInfo:{datasheetInfo:{...}}}}`, not the flat form I first assumed). Every headline number reproduced exactly. The verdict is split three ways and it should decide the roadmap.

DENSE ENOUGH TO FOUND A TOOL ON: ratedCurrentPerContact 381,118 (97.14%); operatingTemperature.maximum 366,933 (93.52%); positions 365,951 (93.27%) — the triple 340,224 (86.72%), of which 181,832 have T_max ≤ 105 °C so the derating actually bites. pitch 246,344 (62.79%) with a sharp mode structure (2.54 mm ×99,530, 2.00 ×57,215, 5.00 ×8,821, 1.00 ×8,096, 5.08 ×7,217, 1.27 ×6,786). ratedVoltage 228,846 (58.33%), 92,709 of them ≥400 V. mountingStyle 199,978 (50.97%). mating 194,965 (49.69%). These support thermal/current derating, insulation screening and loop-area radiation *today*, with no new data.

THIN BUT REAL, ENOUGH FOR A SECOND TIER: pitch∧positions∧rows 137,686 (35.09%); pitch∧positions∧length 138,093; tailLength 87,965 (22.42%); matedHeight on 87,712 edges / 53,674 parts / 50,166 with pitch — the only Tier-A geometry in the file and completely unexploited; plating triple 54,336 (13.85%); interfaceStandard 48,607 (12.39%); contactResistance 47,018 (11.98%); characteristicImpedance 11,266 (2.87%).

FATAL FOR ANYTHING CLAIMING TO BE EMC-GRADE: pinout 0%. CONAS Tier-2 `geometry{}` and `contactSystem{}` 0% on every one of the 392,346 records. Transfer impedance 0. Shielding effectiveness in dB 0. S-parameters 0. No `dataRate` field exists. `rowPitch` 57 parts against 70,829 multi-row parts. `creepage ∧ pollutionDegree` = exactly 0 records — the IEC 60664-1 chain cannot be closed from parametrics on a single part. `ratedCurrentReferenceTemperature` 2,767 (0.71%), so the derating basis is unknown on 99.3%. The `shielded` boolean 1,807 (0.46%) with only 926 `true`, and absent from every family a power engineer touches. Free text mentions "filter" 51 times and "ferrite" 7 times in the whole file. Material references resolve 100% cleanly (0 dangling ids in 272,204 refs) but come from 3 of 16 vendors and are ~99.9% Sullins headers.

CONCLUSION: the catalog can honestly found a SCREENING tool over 86.7% of parts on thermal/current, 62.8% on geometry-driven radiation and insulation, and 35.1% on pin-field coupling — provided every assumed input (pin width, row pitch, ε_eff, ΔT_rated, Z_cm, ground pattern) is surfaced as an assumption on the face of the output, and every missing input returns UNKNOWN rather than a default. It CANNOT found a compliance tool, a Touchstone library, or any shielding claim in dB, and saying so plainly is a feature, not a weakness — it is the difference between the honest position and the one that gets falsified in a chamber.

One cautionary result from my own run: applying the flat-surface creepage bound (pitch − imputed pin width) against coarse IEC 60664-1 PD2/IIIa anchors flags 91,397 of 116,609 screenable parts (78%), and 19,808 of the 26,148 screenable parts rated ≥400 V. That is a model failure, not a catalog failure — vendors meet creepage with ribs and barriers the parametrics cannot see — and it is a live demonstration that a physically correct bound can still produce a useless product. Any concept here must be validated against its own false-positive rate before it ships.

HIGHEST-LEVERAGE DATA INVESTMENTS, in order: (1) exploit `matedHeight` — it is free, already in the file, and immediately yields the strongest quantitative claim available (mezzanine stack height, not pitch, dominates connector ground inductance: 14.8 nH at the median 17.27 mm stack versus 3.3 nH for a 6 mm header); (2) back-fill `rowPitch`, which appears on every two-row datasheet and is usually simply equal to pitch, unblocking 70,829 multi-row parts; (3) join `interfaceStandard` to ~30 public standards for exact per-pin ground maps on ~22,000 parts, converting a 0%-populated Tier-2 field into a populated one with no measurement; (4) extract pin cross-section, creepage and DWV from the 118,119 curl-reachable datasheet PDFs (167,613 unique documents, only 1.86 parts each, so ~168k fetches not 312k).

---

# Addendum — what was implemented in Blade Runner (2026-08-01)

The physics learned here was encoded into the TAS C++ physics validator
(`TAS/validator/src/connectors.cpp`, `include/tas_validator/thresholds.hpp`). **Every check
was calibrated against all 392,346 live records before being written**, and its measured fire
rate is quoted at the check in the source.

## Shipped

| Code | What it catches | Severity | Fires |
|---|---|---|---|
| `CONN_CONTACT_VOLTAGE` | Holm voltage-temperature relation: `I_rated x R_contact` against the softening/melting voltage of the part's own stated plating | SUSPICIOUS (IMPOSSIBLE past 5x tungsten's 1.10 V) | 468 (0.12%) |
| `CONN_CURRENT_DENSITY` | rated current through the contact cross-section the pitch allows (`side = pitch/4`, both standard conventions) above 100 A/mm2 | SUSPICIOUS | 1,829 (0.47%) |
| `CONN_TEMPERATURE_UNIT` | unconverted Fahrenheit: a maximum above 200 that back-converts to a round Celsius figure | SUSPICIOUS | **67** |
| `CONN_TEMPERATURE_RANGE` | inverted range, below absolute zero, or above any polymer housing | SUSPICIOUS / IMPOSSIBLE | 65 |
| `CONN_ROWS_POSITIONS` | more rows than positions | SUSPICIOUS | 35 |
| `CONN_DURABILITY` | mating cycles against the plating that must survive them (tin, gold flash < 0.1 um) | SUSPICIOUS / IMPOSSIBLE | 18 |
| `CONN_UNIT_SCALE` | mm-for-m and um-for-m unit slips on pitch, plating thickness, tail length, stack height, mated height | IMPOSSIBLE | 0 (forward guard) |
| `CONN_CLEARANCE_BREAKDOWN` | **upgraded** from a linear 3 kV/mm rule to the ideal uniform-field **Paschen curve** | IMPOSSIBLE | 0 |
| `CONN_DWV_VS_CLEARANCE` | proof voltage above the Paschen breakdown of the part's own clearance | IMPOSSIBLE | 0 |
| `CONN_RF_BAND` | Z0 outside any transmission line, inverted frequency range, VSWR below unity | SUSPICIOUS / IMPOSSIBLE | 0 |
| `CONN_MATED_HEIGHT` | one part quoting mated heights an order of magnitude apart | SUSPICIOUS | 0 |

Total connector-specific findings: **2,482 of 392,346 (0.63%)**. **Zero parts are
invalidated** — every IMPOSSIBLE tier is a forward guard for future sourcing passes, not a
retroactive rejection of real catalog data.

The `CONN_TEMPERATURE_UNIT` result is the cleanest catch: 392 degC (58 parts) is 200 degC,
302 degC (7 parts) is 150 degC, and 221 degC (2 parts) is 105 degC — all three among the
catalog's most common genuine Celsius maxima, all read off Fahrenheit datasheets. The genuine
high-temperature parts (205, 250 and 260 degC, 3,571 parts) back-convert to 96.1, 121.1 and
126.7 and are untouched.

The Paschen upgrade is a correctness fix, not a yield change: both the old linear rule and the
new curve fire on 0 of the 1,210 parts carrying clearance and voltage. But the linear rule
demands ~5 kV/mm of margin below 10 mm that the physics does not require, so it would have
started calling possible parts impossible as soon as HV connector data was added.

## Designed, measured, and REJECTED

Recorded in the source header so they are not re-proposed. All three fail for the same reason
the Paschen creepage screener failed in §6: **CONAS does not say which pair of conductors a
clearance or creepage figure refers to, and it does not label which axis a body dimension
runs along.**

| Rejected check | Fire rate | Why it is wrong |
|---|---|---|
| body envelope must contain the pin field: `max(length,width,height) >= (ceil(positions/rows)-1) x pitch` | **78,044 / 104,260 = 74.9%** | `mechanical` length/width/height carry no axis convention, so the largest *stated* dimension is routinely not the axis the pin field runs along |
| `creepage <= pitch` (flat-surface geodesic bound) | **1,321 / 1,561 = 84.6%**, median ratio 1.26, max 5.76 | vendors meet creepage with ribs and barriers no parametric field can see; the bound is a floor, not a ceiling |
| `clearance <= pitch` | **623 / 1,399 = 44.5%** | the figure frequently refers to contact-to-shell or group-to-group, not adjacent contacts |

Two further per-record checks were rejected on inspection rather than rate:

- **self-mating series** (`matesWith[].series == own series`) — 52 fires, all legitimate:
  Samtec LSHM/LSEM board-to-board variants genuinely mate within one series name.
- **`rows > 20`** — 35 fires on real Amphenol backplane connectors with 24-40 rows.

## Schema gaps this surfaced

Not acted on — CONAS changes are governance-controlled and need explicit approval. Recorded
as findings:

1. `electrical.clearance` and `electrical.creepage` have **no reference-pair qualifier**. The
   same field is used for adjacent-contact, contact-to-shell and group-to-group figures, which
   makes every geometric consistency check on them undecidable.
2. `mechanical.length` / `width` / `height` have **no axis convention** relative to the
   contact array, which makes envelope-vs-pin-field checks undecidable.
3. `mechanical.positions` on multi-level terminal blocks counts **poles per level**, not total
   contacts, so `rows > positions` is reachable on legitimate parts (35 records).
4. `electrical.ratedCurrentReferenceTemperature` is present on 2,767 records and is **literally
   `25.0` on every one of them** (100% Harwin) — that is a reference *ambient*, not a rated
   *rise*, so the derating basis (dT_rated) has **0% coverage**, not 0.71%.
