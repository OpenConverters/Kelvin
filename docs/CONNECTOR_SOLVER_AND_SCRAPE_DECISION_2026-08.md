# Connector Field-Solver Tool — Architecture Decision

**Status:** DECISION. Supersedes Deliverable A + B as written.
**Author:** lead architect. **Date:** 2026-08-01.
**Basis:** every number below marked ✅ was re-measured by me on this machine today against `/home/alf/PSMA/TAS/data/connectors.ndjson` (480.3 MB, 392,346 records), the raw pulls on disk, and live HTTP. Where I disagree with a reviewer I say so and show the measurement.

---

## Executive verdict in five lines

1. **The browser can run the solve. There is nothing worth solving live.** 392,346 parts collapse to ✅ **4,181** distinct geometry answers and ✅ **384** shape classes. Precompute is not an optimisation, it is the correct architecture.
2. **The 3-D/BEM tier is not justified.** Re-measured today: the 1,600-unknown panel BEM moves the coupling ratio *k* by **+3.8%** against a 50-unknown closed form, while the *missing* `rowPitch` moves it by **±25%** and the *missing* post cross-section moves C_diag by **−10.7…+22.7%**.
3. **"35.1% already solvable" is false. The measured figure is ✅ 17.85% (70,016 parts).** Of 137,686 parts with pitch+positions+rows, ✅ 69,960 are single-row and ✅ 67,726 are multi-row — of which exactly ✅ **56** carry `rowPitch`.
4. **P0's "zero-HTTP mapper fix" is dead.** The Amphenol importer already maps `row_to_row_spacing` (`amphenol_cs_import.py:277-279`) and the 87,503-record raw pull contains that key ✅ **0 times**. `/tmp/molex` is ✅ gone. The TE dump has ✅ 14 fields and no `rows`. It recovers `stack_height` (✅ 3,833 Amphenol records) and nothing else.
5. **The corpus is already published.** ✅ `curl -sI https://kelvin.openconverters.com/kelvin/connector.ndjson` → `HTTP/1.1 200`, `Content-Length: 486233766`, no auth. ✅ 377,107 records carry a verbatim vendor description; ✅ 392,346 carry a named source endpoint and date. This is a remediation task for this week, not a future risk.

**Decision: build the closed-form tier, ship intervals not scalars, restrict v1 to single-row parts, promote thermal + insulation coordination to the headline, and gate every coverage number behind a measured probe.**

---

# Part 1 — Can we build a 3D field-solver connector tool that runs in the browser?

## 1.1 Verdict, with numbers

**Yes technically. No as specified. The specification is aimed at the wrong 90%.**

| Question | Answer | Evidence |
|---|---|---|
| Can WASM factor the matrices? | Yes, comfortably | Measured in real Chrome 151, cross-origin isolated: N=4,000 blocked fp64 LU = **1.65 s** at 12 threads, N=5,000 = **3.04 s**. Not in doubt. |
| Does the catalog need a live solve? | **No** | ✅ 4,181 distinct `(pitch, rows, positions, orientation, mounting)` tuples; ✅ 384 distinct shape classes. Top archetype `(2.54 mm, 2 rows, 10 pos, vertical, tht)` = ✅ **742 MPNs**, all of which get one identical answer. |
| Does the 3-D tier buy accuracy? | **No, by 6.6×** | Re-run today: BEM(1,600) vs closed form(50) on *k* = **+3.8%**. Missing `rowPitch` = **±25%**. Missing post section = **−10.7…+22.7%** on C_diag. Housing ε_r bracket = **+300%**. |
| How much of the catalog is actually solvable? | **17.85%** | ✅ 70,016 = 69,960 single-row + 56 multi-row-with-rowPitch. |
| Offline batch cost for the whole catalog? | **~1 weekend** | Measured BEM inverse N=1,600 = 3.36 s (numpy/OpenBLAS, AVX-512). 4,181 archetypes × 5 axial sections × 2 solves × 3.36 s ≈ **39 core-hours ≈ 3.3 h on 12 cores.** |

The last row is the one that kills the live solver. The entire A4 envelope table, the +250-line blocked SIMD128 complex LU, the COEP/threads analysis and the Liftoff→TurboFan warm-up exist to solve, interactively, a problem with 4,181 possible inputs that batches in an afternoon.

**I accept the `value-and-legal` reviewer's Objection 1 in full. I disagree with one clause of it.** He writes that the catalog "contributes nothing to the number." It contributes two things: the MPN→archetype *mapping*, and — more valuable — the ability to say *"your part number is under-determined; we do not know its row spacing."* A five-field form cannot do that. It is a weaker moat than the plan claims. It is not zero.

## 1.2 The geometry-input decision — the crux, argued

The plan frames this as a three-way choice. It is not. Two of the three are already settled by measurement, and the real decision is a different one.

**STEP — rejected as a gate, retained as optional enrichment.** Measured on real files: `MANIFOLD_SOLID_BREP = 1`, `NEXT_ASSEMBLY_USAGE_OCCURRENCE = 0`, 424 faces ↔ 424 styled_items, **18.0 gold faces per pin exactly**. The contact beam is not defeatured; it was never in the file. Samtec's own words: *"acceptable for form and fit."* Acquisition is a stateful CADENAS server-side generation job — one driven browser session per part, non-deterministic file URL. It yields pin positions, post section, tail length, mated height. Everything it yields, the footprint yields more cheaply and with a contractual dimensional guarantee.

**Footprint vector extraction — the only at-scale route to the one field that matters, and it is unproven.** The worked example is real: Würth `61300211121`, 208 path components → 10 reassembled circles, dX σ ≈ 0.005 pt, recovered Ø 1.103 mm against the printed `Ø 1,10 ±0,15` = **0.3% error, self-checking**. But the only multi-view test *failed*: Samtec `tsw_th.pdf` page 1, 772 path objects across ~6 overlapping views, 52 false circles that are pin ends in an isometric. n=3 documents, one failure, and Samtec is not even in the PDF corpus. This is a **probe-gated hypothesis, not a plan input.**

**Parametric — chosen as primary, and it is under-determined.** Zero acquisition cost, and it is the only route with 70,016 parts available today. But:

| Required generator input | Coverage today | Effect of getting it wrong |
|---|---|---|
| `pitch` | ✅ 246,344 (62.8%) | sets everything |
| `positions` | ✅ 365,951 (93.3%) | matrix order |
| `rows` | ✅ 142,393 (36.3%), and **0** on Molex/Samtec/TE/JAE/Amphenol RF/Adam Tech | array topology |
| `rowPitch` | ✅ **57** (0.015%), **all 57 from CUI Devices** | **±25% on k** |
| post cross-section | **0**, and no CONAS field exists to hold it | **−10.7…+22.7% on C_diag** |
| board thickness | **0**, no field exists | PEEC barrel length |
| housing fill fraction | **0** | **+300% on C_diag** |

**The actual crux is therefore not "which source" — it is: a determinate answer on 17.85%, or an interval on 35.09%.** A1's own no-fallbacks clause forbids assuming post section and board thickness, so even for the 70,016 "solvable" parts the honest output is not a number.

**Decision: the interval is the product.**

```
C_report = [ C(post=0.20·p, ε=1) , C(post=0.35·p, ε=ε_housing) ]      + assumption flags
L_report = [ L(post=0.20·p)      , L(post=0.35·p)      ]              + image-sign corrected
```

Once you accept the interval, the marginal value of a better *solver* collapses — which is exactly why the BEM tier is unjustified. A factor-4 bracket honestly labelled, with an explicit "post cross-section assumed over 0.20–0.35 × pitch" flag, beats a four-digit number whose fourth digit came from a BEM and whose first digit came from a guessed post size. That is the whole argument, and it is why the two reviewers who attacked the solver from opposite directions reach the same place.

## 1.3 Solver stack — what we build

| Tier | Method | Where | Cost | Status |
|---|---|---|---|---|
| **T1 — closed form (v1, the product)** | Line-charge C, one unknown per conductor, `r_eq = 0.59017·s` (exact transfinite diameter of a square). Partial L from Grover/Ruehli + **explicit image sign** | Live, WASM | µs | **BUILD. ~300 lines.** |
| **T1b — image sign** | `current_normal_to_plane ? +M : −M` | Live | free | **BUILD — this is the single most valuable correction in the plan.** Worth 1.45–2.22×, always in the unsafe direction. Invalidates `Rlgc.hpp:160-162` (`C0 = to_signal(maxwell_vacuum); Linv = invert_matrix(C0,n)`) for board-normal conductors. |
| **T2 — R(f), L_int(f)** | Exact Bessel below a/δ=2, 3-term asymptote above (measured error +0.03% at 3 MHz, 0.000% ≥10 MHz) | Live | ~1 µs/pt | **BUILD.** |
| **T3 — proximity** | 2-D graded cross-section, 1.5:1 grading, 12–16 cells (<0.35% across 1 MHz–1 GHz on one mesh) | **Precomputed table** vs (post/pitch, a/δ) | 1.73 s/freq offline | **BUILD OFFLINE.** Skip above ~2 mm pitch with a stated bar: magnitude is (a/p)² = 6.3% at 2.54 mm. |
| **T4 — panel BEM** | `Bem2d.hpp` with the image plane made optional and a pin-field `discretise()` | **Offline batch only** | 3.36 s per solve; 384 shape classes = 21 min | **BUILD ONCE, NEVER SHIP.** Its job is to *establish the closed form's error bar*, not to produce product numbers. |
| **T5 — 3-D PEEC, full cross-section** | — | — | 28,800 unknowns, 12.7 GB, 3 h | **REFUSE.** |
| **T6 — 3-D surface BEM for C** | — | — | 31,600–123,000 panels, 8.0 GB, exceeds wasm32 | **REFUSE.** |
| **Circuit** | `Mna.hpp` trapezoidal MNA, multi-aggressor; `Rlgc.hpp` Z₀/k_b with L injected, never `C₀⁻¹` | Live | ms | **REUSE.** |
| **Ground-pattern study** | Submatrix deletion on the stored Maxwell matrix | Live | µs | **REUSE.** Needs the matrix, not a solver. |

**I disagree with `solver-realism` on one point.** He concludes the BEM tier should be deleted. I keep it — *offline*. Without it you cannot *claim* the closed form is within 4%; you can only assert it. One week of BEM work buys a published, FasterCap-cross-checked residual on 384 shape classes. That is the difference between an engineering claim and a marketing claim, and this founder will ask for it. Cost: **1 week, not 5–6.**

## 1.4 Problem-size envelope, with the benchmark rigging removed

The plan's A4 table benchmarks WASM against Faraday's own unoptimised `Dense.hpp` (verified: naive rank-1 LU, **0** SIMD intrinsics), which makes WASM look like 0.74× native. Measured against LAPACK on this box (AVX-512, OpenBLAS 0.3.30, 1 thread):

| | plan's "native" | actual native, 1 thread | honest WASM ratio |
|---|---|---|---|
| DGEMM N=2048 | 19.2 GF/s | **47.9 GF/s** | **0.30×**, not 0.74× |
| Real fp64 LU | 6.84 GF/s | DGETRF N=1600 = **37.4 GF/s / 73.1 ms** | **0.14–0.17×** |
| Complex LU | — | ZGETRF N=1600 = **38.7 GF/s / 282 ms** | — |

The plan's 1.1 s in-tab C solve is **146 ms** in native LAPACK. The browser tax is ~7.5×, not 1.35×. And the 5.0–6.2 GF/s figure the whole envelope rests on is a *projection* from an unwritten 4×2 micro-kernel budgeted at one day; the **measured** complex path is 3.35 GF/s.

**None of this matters any more, because nothing is factored in the tab.** The envelope that governs the product is:

| Live operation | Unknowns | Wall time |
|---|---|---|
| Closed-form C + L, 50 pins | 50 | **< 1 ms** |
| Closed-form C + L, 200 pins | 200 | **< 20 ms** |
| R(f), L_int(f), 10-point decade sweep | — | **~10 µs** |
| Ground-pattern exploration (submatrix deletion) | — | **µs** |
| MNA transient, multi-aggressor | — | **ms** |
| Shard fetch of the archetype record | — | one HTTP Range read |

**Consequences we now get for free:** no blocked SIMD128 kernel, no COEP/`Cross-Origin-Resource-Policy` decision (which would have broken Kelvin's cross-origin Range fetches), no threads, no warm-up GEMM, no memory64 question, no 1.5 MB module. The module is small enough to be an afterthought. Four weeks of the plan's critical path disappear.

## 1.5 Precomputed vs live

| Precomputed offline → Kelvin shard | Live in the tab |
|---|---|
| **All 4,181 archetype C/L matrices**, keyed on `(pitch, rows, positions, orientation, mounting)` — 2-D C′ is scale-invariant (verified: pitch 2.54 and 1.27 mm at equal post/pitch give identical 6.5776 pF/m) | Interval arithmetic over the assumption ranges |
| Both interval endpoints per archetype (post 0.20 / 0.35 × pitch) | Ground-pattern / pinout exploration by submatrix deletion |
| Proximity correction table vs (post/pitch, a/δ) | Closed-form R_ac(f), L_int(f) |
| BEM-vs-closed-form residual per shape class (**the published accuracy claim**) | MNA transient, crosstalk waveform |
| 3-D end-correction terms from OMFEM (off-diagonal ΔL ≈ 0.029 × pitch; diagonal ΔL ≈ 0.68 × pitch = 21% of a 6.35 mm pin) | `NearField::h_loop_vec`, `Shielding::aperture_se_db` |
| Material tables (σ, µ_r, ε_r, Dk(f)) | Thermal + insulation arithmetic (§Part 4) |
| MPN → archetype index, **and the un-mappable set** | The refusal state |

## 1.6 Reuse map

| Asset | Verdict | Change |
|---|---|---|
| `Faraday/Bem2d.hpp` (809 lines) | **REUSE — offline batch engine** | Make the image plane optional (currently unconditional at `:557-558,568`; measured impact 6.38 → 7.31 pF/m, +15%). Pin-field `discretise()`, ~200–300 lines. Interface-panel physics is already general. |
| `Faraday/Dense.hpp` | **REUSE as-is** | The +250-line blocked SIMD LU is **cancelled**. Offline batch runs against LAPACK. |
| `Faraday/Rlgc.hpp`, `Mna.hpp`, `CrosstalkPeaks` | **REUSE** | **Delete/guard the `L = µ₀ε₀C₀⁻¹` path for pin fields** and accept an externally supplied L. Everything else drop-in; `Mna` is already cross-checked against Kirchhoff's in-process libngspice to 0.01 dB NEXT. |
| `Faraday/NearField.hpp`, `Shielding.hpp` | **REUSE unchanged** | Exact Biot–Savart over a pin polyline; can seam + aperture SE. |
| `Faraday/FastcapExport.hpp`, `CrossSection.hpp::write_gmsh` | **REUSE** | Oracle path; the working Faraday→OMFEM gmsh-2.2 handshake. |
| Faraday's WASM delivery (1.22 MB, no pthreads, `-sDYNAMIC_EXECUTION=0`, sha256-verified deploy) | **REUSE — proven pattern** | Keep `-fwasm-exceptions`. |
| **OMFEM** | **ORACLE ONLY, native** | Promote `pinfield.cpp` (130 lines, MFEM, self-generated Cartesian mesh, no gmsh/OCCT) to `tools/`. Each calibration geometry independently h-converged — the 24-design 3-D scorecard spans −96.5% to +1088.9% on L, so it is a *per-design-validated* oracle, never a blanket one. |
| **gmsh-wasm** (41.9 MB), **mmg-wasm**, **OCCT wasm** | **DO NOT SHIP** | We never mesh a volume. Also resolves the hard `-fwasm-exceptions` vs `-fexceptions` link incompatibility by never creating it. |
| **Kelvin** | **REUSE** | Shard delivery. COEP conflict is now moot. |
| **FastHenry / FastCap** | **SHIPPABLE — corrected 2026-08-01** | ~~NEVER SHIP~~. The original verdict read only the source headers and was **wrong**. `induct.h`/`mulGlobal.h` (included by 38 of 41 `.c` files) do carry a restrictive 1994 M.I.T. notice (*"Any distribution … is strictly prohibited"*), **but M.I.T. relicensed in 2003** and the source tree was never updated. Per <https://www.fastfieldsolvers.com/license.txt> §"License agreement for the M.I.T. copyrighted material": *"Copyright (C) 2003 by the Board of Trustees of Massachusetts Institute of Technology … License to use, copy, modify, **sell and/or distribute** this software and its documentation **for any purpose** is hereby granted **without royalty**"*, conditioned only on retaining the notice and not using M.I.T.'s name in advertising. The same file states FastFieldSolvers' own porting/modifications (FastCap2, **FastHenry2**) are **LGPL**. Verified: the 2003 grant appears **nowhere** in the GitHub tree — headers are stale, the website is the distributor's authoritative statement, and this relicensing is what made FasterCap's LGPL possible at all. **Consequences:** (a) the G1 FastHenry cross-check moves from developer-local into the repo and CI, so the accuracy claim becomes independently reproducible; (b) the archetype shard can be published as *FastHenry-validated with decks included*; (c) a FastHenry-WASM "verify with an independent solver" tier is now available, though the archetype collapse means it is a nice-to-have, not core. **Obligations:** retain the 2003 notice; LGPL-2.1 §6 relinking applies to the statically-linked FFS-modified parts — the same constraint already accepted for FasterCap. **Residual ambiguity:** FFS's own file says conflicts defer to the per-source-file notes, which are the stale 1994 text — ask FFS to refresh the headers before redistributing at scale. |
| **FasterCap** | **Offline oracle** | Genuinely LGPL-2.1 and clean. `FastcapExport.hpp` already writes its decks. |

## 1.7 Validation gates

**G0 — PRE-REGISTERED FALSIFICATION. Runs before any solver line is written.** Build 10 geometries both ways — closed form (~300 lines) and full BEM/PEEC. **Gate: does the BEM/PEEC answer move a *reported* number by more than the input uncertainty on the same geometry?** My re-measurement predicts it does not (3.8% vs ±25%). If it *does*, the BEM tier is reinstated and this document is wrong. That is the point of pre-registering it.

**G1 — FastHenry2, offline.** `.inp` decks from a new `FastHenryExport.hpp`. Gate: L matrix agrees to the printed 6 digits (achieved on `d_50_1`; only differing entries were exact numerical zeros at 1e-19).

**G2 — FasterCap, offline.** ≥1 geometry per archetype family.

**G3 — OMFEM 3-D, per-design h-converged.** Fixes ΔL(pitch, post ratio) and the diagonal end term.

**G4 — Samtec SEAM/SEAF: DEMOTED from gate to directional sanity check.** The datum (+11.9 dB, 2.97% → 11.70% single-ended NEXT) is at a **30 ps edge** → knee ≈ **11.7 GHz**, against a model the plan itself caps at "honest to 1–2 GHz." It is 5–10× out of band. Our prediction is +6.5…+10.1 dB — a factor 1.2–1.9 miss on the ratio change, larger than every solver refinement combined. And the plan pre-excuses the miss in the same paragraph that calls it a gate. **A gate whose failure has already been excused is a knob.** Use it as: "the ordering 1:1 worse than 2:1 reproduces; magnitude is not claimed."

**G5 — internal invariants, every run (Blade Runner class).** `L_eff(l) = ½L_p(2l)`; Grover/Rosa bar self-term within 0.5%; GMD of a square = 0.44705a; 2-D C′ scale invariance; `C_air ≤ C_true ≤ ~ε_r·C_air`; derating monotonicity; `R_ac ≥ R_dc`.

## 1.8 Honest limits — every one goes in the UI

1. **MQS only.** Phase error ≈ ⅙(2πl/λ)². 12 mm pin at 1 GHz ≈ 1%; at 3 GHz ≈ 9.5%. **Honest to 1–2 GHz. No further.**
2. **No per-SKU claim, ever.** ✅ 742 MPNs share one archetype. The answer is a property of the geometry class, not the part number. The UI must say so on every result.
3. **Absolutes are bracketed at a factor of 4 (12 dB).** Ratios drift ≤10% across ε=1→4 while absolutes move 266%. Ship deltas and ratios; bracket absolutes.
4. **No contact beam, ever.** 16–18 faces per contact in a vendor STEP is a chamfered post. Effect on contact resistance is total, and that we already have from datasheets at 12.0%.
5. **Nickel underplate unmodelled above ~50 MHz.** 1.27–2.54 µm electroless Ni, µ_r ≈ 100–600, δ = 1.33 µm at 100 MHz — inside the plating. `contactPlating.material` 24.8%, `.thickness` 13.9%: enough to flag, not to model. **Refuse above 50 MHz or model it explicitly.**
6. **±13% from the square→round equivalent radius.** DC wants equal area (0.5642a), HF wants equal perimeter (0.6366a); ratio 1.128.
7. **Shielding effectiveness and transfer impedance for board connectors: refused.** IEC 62153-4-15 §3.7 defines the DUT as connector + mating connector + attached cables. A per-part number is **undefined by the governing standard.**
8. **The input geometry is the product problem.** `geometry{}` and `contactSystem{}` are ✅ 0 of 392,346 — the record shape does not even contain them.

---

# Part 2 — The complete connector data model

**Legend.** `✓` field exists in CONAS. `◆` **PROPOSED** — needs explicit schema approval; until approved it lives in an out-of-band sidecar and is never persisted under a schema type it does not validate against. `▣` no CONAS home at all → sidecar + `abt post --to conas`.
**Consumers:** `THERM` (I²R/derating) · `INSUL` (IEC 60664) · `GEN` (pin-field generator) · `CFORM` (closed-form C/L) · `SKIN` · `PROX` · `RLGC/MNA` · `FOOT` · `SI` · `BR` (Blade Runner) · `CAT`.
**Yield discipline:** every figure is **MEASURED** (by me, today), **PROBE** (unknown until Phase 1 measures it), or **EST** (reasoned estimate, explicitly not measured). **The plan's asserted "→ 52%" style targets are removed. They had no measured basis.**

### Group 0 — Identity, lifecycle, commerce

| Field | Unit | Consumer | Source of truth | Now | Yield |
|---|---|---|---|---|---|
| ✓ `manufacturerInfo.{name, reference, orderCode}` | — | join key | vendor API | ~100% MEASURED | — |
| ✓ `manufacturerInfo.status` | — | CAT | vendor API | high | — |
| ✓ `manufacturerInfo.datasheetUrl` | URI | **PDF pipeline input** | vendor | ✅ 312,456 (79.6%) | hard ceiling on all PDF-derived fields |
| ✓ `datasheetInfo.part.{partNumber, series, case, matingPolarity}` | — | archetype key | vendor | high | — |
| ✓ `manufacturerInfo.description` | — | interface inference | vendor | ✅ 377,107 | **strip from public artifact — see Part 5** |
| ▣ `ulFileNumber` (`E######`) | — | cert join | datasheet text (verified live: Würth prints `cULus Approval E315414`) | 0% | PROBE |
| ▣ `countryOfOrigin`, `packaging`, `weight` | — | CAT | Harwin API (all present) | 0% | EST high on Harwin |

### Group 1 — Family & taxonomy

| Field | Unit | Consumer | Source | Now |
|---|---|---|---|---|
| ✓ `familyDetails.family` (14-variant union) | — | **selects archetype**, CAT | derived | high — ✅ pinHeaderSocket 120,994 · boardToBoard 118,164 · dataInterface 48,607 · wireToBoard 41,377 · terminalBlock 21,901 · rf 11,266 · circular 9,190 · fpcFfc 6,473 · power 6,289 · wireToWire 4,851 · cardEdge 3,213 · busbar 21 |
| ✓ `familyPinHeaderSocket.{pinStyle, tailLength, stackable}` | m | **CFORM z-extent**, FOOT | drawing | tailLength 22.4% |
| ✓ `familyBoardToBoard.{stackHeight, stackTolerance, mezzanine}` | m | **CFORM mated z** | Amphenol `stack_height` ✅ 3,833 in raw; Samtec `filters/3` (~400 values) | 0.26% |
| ✓ `familyTerminalBlock.{clampType, levels, wireGaugeRange, wireEntryAngle, pluggable}` | AWG/m² | **THERM, INSUL** | ETIM, vendor | partial |
| ✓ `familyWireToBoard/WireToWire.{termination, wireGaugeRange, secondaryLock}` | — | **THERM** (lead is the heat path) | vendor | partial |
| ✓ `familyFpcFfc`, `familyCardEdge`, `familyCircular`, `familyRf`, `familyDataInterface`, `familyPower`, `familyBusbar`, `familyAcInlet` | mixed | CAT, SI, INSUL | vendor | partial; `familyDataInterface.interfaceStandard` 12.4% of which only ~34.6% genuinely standardised |

### Group 2 — Pin-field geometry ★ THE BINDING CONSTRAINT ★

| Field | Unit | Consumer | Source of truth | Now (MEASURED) | Yield |
|---|---|---|---|---|---|
| ✓ `mechanical.pitch` | m | **GEN, CFORM, PROX gate, PDF scale key** | all vendors | ✅ 246,344 (62.8%) | PROBE |
| ✓ `mechanical.positions` | — | matrix order | all vendors | ✅ 365,951 (93.3%) | — |
| ✓ `mechanical.rows` | — | **GEN topology** | Samtec PN suffix `-S/-P/-D/-T/-Q` (grammar confirmed from the CADENAS configurator); Sullins ✅ 82,518/82,518; Harwin `noOfRows`; vector pad grid | ✅ 142,393 (36.3%) — **0 on Molex (94,255), Samtec (55,450), TE (35,192), JAE, Amphenol RF, Adam Tech** | PROBE; Samtec PN regex is EST-high |
| ✓ `mechanical.rowPitch` | m | **GEN — ±25% on k. THE gate.** | vector pad grid; CUI; DigiKey 314 `Row Spacing - Mating` (legally blocked) | ✅ **57, all CUI** | **PROBE — no measured route exists** |
| ✓ `mechanical.orientation` | — | segment count, corner term | all vendors | ✅ 174,717 (44.5%) | — |
| ✓ `mechanical.mountingStyle` | — | z-extent, FOOT | all vendors | ✅ 199,978 (51.0%) | — |
| ✓ `geometry.parametric.contactArray.{pitchX, pitchY, countX, countY, origin}` | m | **generator's direct input** | derived from the four above | **0% — the block is absent from the record shape** | derived |
| ✓ `geometry.parametric.housingExtrusion.{profile, length}` | m | BEM dielectric region | drawing / STEP | 0% | PROBE |
| ✓ `geometry.matedHeight` | m | **CFORM mated section** | Samtec `stackHeight`; Amphenol `height_mated` | 0% | PROBE |
| ✓ `geometry.coordinateSystem.{originDatum, mateAxis}` | — | **BR: an (x,y) with no datum is not data** | the drawing | 0% | — |
| ✓ `geometry.boundingEnvelope`, `keepOut`, `mountingFeatures[]`, `cadModels[]` | m | MECH, FOOT | drawing | 0% | — |
| ◆ `contacts[].crossSection.{width, thickness, diameter, cornerRadius}` / `.outline[[x,y]]` | m | **CFORM, PROX, SKIN — `area` alone cannot be panelled. −10.7…+22.7% on C_diag.** | contact drawing; STEP section | **0%, and NO SCHEMA FIELD EXISTS** | **START GOVERNANCE DAY 1** |
| ◆ `geometry.pcbFootprint.recommendedBoardThickness` (`dimensionWithTolerance`) | m | **PEEC barrel length; generator throws without it** | datasheet | 0%, no field | **START GOVERNANCE DAY 1** |
| ◆ `contact.tailLength` (per contact) | m | **the stub is the antenna** | drawing | 0% | PROBE |
| ◆ `contact.matedEngagementLength` | m | coupled length | mated cross-section | 0% | PROBE |
| ◆ `contacts[].centerlinePath[]` (polyline ≥2 pts) | m | right-angle bends, length matching | STEP, drawing | 0% | PROBE |

### Group 3 — Contact system

| Field | Unit | Consumer | Source | Now |
|---|---|---|---|---|
| ✓ `contacts[].id` / `.pinName` | — | join key for nets/pads/pinout | pinout table | 0% |
| ✓ `contacts[].signalRole` (signal/power/ground/shield/diffP/diffN/nc) | — | **ground-pattern study, RLGC/MNA** | standards join (Part 3 §3.5) | 0% |
| ✓ `contacts[].position` (`vector3`, all of x,y,z **required**) | m | CFORM | full 3-D model | 0% |
| ◆ `contacts[].positionXY {x,y}` + `.positionDatum` | m | **the catalog-derivable 2-D field without a fabricated z** | footprint drawing; parametric grid | 0% |
| ✓ `contacts[].{crossSection.area, pathLength, currentRating, contactResistance, normalForce, baseMaterialRef, platingMaterialRef}` | m², m, A, Ω, N | **THERM**, constriction R | datasheet | 0% |
| ✓ `contactSystem.nets[].{name, contactIds[]}` | — | current injection, ground pattern | pinout | 0% |
| ✓ `contactSystem.matingInterface.contactType` (pinSocket/blade/tuningFork/cantileverBeam/hyperboloid/springFinger/poke) | — | contact-R model | ODU LAMTAC/SPRINGTAC/TURNTAC; Samtec `contactSystem` | 0% |
| ✓ `contactSystem.shield.{present, materialRef, coverageFraction, groundedContactIds[]}` | — | **presence only, NOT performance** | datasheet | 0.46% |
| ◆ `contactSystem.differentialPairs[].{id, positiveContactId, negativeContactId, intraPairSkewLength}` | m | **differential Z / skew / NEXT have no subject without this** | pinout, standard | 0% |
| ◆ `contactSystem.groundAssignment.{scheme, signalToGroundRatio}` | — | the one public apples-to-apples SI datum | characterisation report | 0% |
| ◆ `contacts[].matingSequence` | — | hot-plug, shield path during insertion | sequencing table | 0% |
| ◆ `contacts[].signalRole` **enum extension**: `keying`, `polarizing`, `mountingLug`, `detect`, `drain`, `reserved`, `mechanicalOnly` | — | **keying posts are conductive metal in the field** | pinout | — |

### Group 4 — PCB land pattern

| Field | Unit | Consumer | Source | Now |
|---|---|---|---|---|
| ✓ `pcbFootprint.pattern`, `pads[].{id, x, y, shape, width, height, drill}` | m | **FOOT, GEN (rowPitch!), CFORM** | **vector extraction, 0.3% measured on one worked example** | 0% |
| ◆ `pads[].antipadDiameter` | m | **the SI-critical one** — plane clearance sets the shunt C of a through-board contact | recommended-layout drawing | 0% |
| ◆ `pads[].{padNumber, contactId, padType, layer, rotation, finishedHoleSize, holeTolerance, annularRing, solderMaskExpansion, pasteAreaRatio}` | m, rad | FOOT, press-fit retention | drawing | 0% |
| ◆ `pcbFootprint.courtyard[[x,y]]` + `courtyardExcess` + `densityLevel` (A/B/C) | m | FOOT — `keepOut` is a box, **not** an IPC-7351 courtyard | drawing + IPC-7351B | 0% |
| ◆ `pcbFootprint.planeKeepouts[].{layer, outline, purpose}` | m | **EMC — a slot in a reference plane under a connector is a classic emissions failure** | drawing | 0% |
| ◆ `pcbFootprint.{assemblyOutline, silkscreenOutline, pin1Marker, recommendedPlating}` | m | FOOT, orientation, press-fit gas-tightness | drawing | 0% |
| ◆ `pcbFootprint.sourceDrawing` (`externalFileReference` incl. **page**) | — | **a footprint is read off a specific page of a specific PDF** | the PDF | 0% |

### Group 5 — Contact metallurgy & plating

| Field | Unit | Consumer | Source | Now |
|---|---|---|---|---|
| ✓ `material.contactBaseMaterialRef` → `conas-materials` | — | **SKIN (σ, TCR), R_dc, THERM** | datasheet; Harwin `contactBaseMaterial` | partial |
| ✓ `contactPlating.{matingAreaMaterialRef, matingAreaThickness}` | m | **SKIN, Ni-underplate flag** | plating table; Harwin `contactFinish` | 24.8% / 13.9% |
| ✓ `contactPlating.{terminationMaterialRef, terminationThickness}` | m | solderability | Harwin `terminationFinish` (**separate field**) | low |
| ✓ `contactPlating.{underplatingMaterialRef, underplatingThickness}` | m | ★ **the >50 MHz limit** | plating stack (µin Au over µin Ni) | very low |
| ✓ registry `electricalConductivity`, `temperatureCoefficientOfResistance` | S/m, 1/K | **SKIN, R(T), THERM** | material registry | registry |
| ▣ contact-metal `relativePermeability` (µ_r for Ni) | — | **SKIN above 50 MHz — no CONAS home** | material datasheets | 0% |
| ▣ `pinDiameter`, `tailDiameter`, `acceptsPinDiameterRange`, `shoulderDimensions`, `springTravel`, `springForce` | m, N | **the only real contact geometry published anywhere** | Mill-Max & Preci-Dip publish all per part | 0% |

### Group 6 — Insulator / housing material (→ registry)

| Field | Unit | Consumer | Source | Now |
|---|---|---|---|---|
| ✓ `material.{housingMaterialRef, shieldMaterialRef, sealingMaterialRef}` | — | **CFORM (ε_r), INSUL** | datasheet ("Insulator Material PA66") | ~0% |
| ✓ registry `relativePermittivity` / `lossTangent` / `characterizationFrequency` | —, Hz | **CFORM, RLGC** | CAMPUS, vendor grade PDFs | 15/14/15 of 17 dielectrics — **but at 1 kHz, 1 MHz *and* 1 GHz across the registry** |
| ✓ registry `dielectricStrength` / `volumeResistivity` | V/m, Ω·m | INSUL | material datasheet | 17/17 — **specimen thickness unknown** |
| ✓ registry `comparativeTrackingIndex` | V | **INSUL — the 60664 material group** | UL Yellow Card, CAMPUS | **3 of 17** |
| ✓ registry `ul94Rating` | — | safety | Yellow Card | **4 of 17** |
| ◆ registry `materialGroup` (I/II/IIIA/IIIB) → **hoist to PEAS** | — | **INSUL — what the 60664 tables are indexed by.** MAS already defines this enum; CONAS may not `$ref` MAS | Yellow Card, CAMPUS | 0% |
| ◆ registry `ul94Ratings[]` `{rating, thickness, color}` | m | **V-0 at 0.75 mm ≠ V-0 at 3.0 mm** | Yellow Card | 0% |
| ◆ registry `trackingIndex {value, kind: CTI\|PTI, solution: A\|B, standard}` | V | Solution A and B differ for the same polymer | Yellow Card | 0% |
| ◆ registry `relativeThermalIndex{Electrical, MechWithImpact, MechWithoutImpact}` | °C | **IEC 62368-1 polymer limit — RTI is NOT `maxOperatingTemperature`** | UL 746B | 0% |
| ◆ registry `dielectricSpectrum[]` `{frequency, Dk, Df, temperature, conditioning, testMethod}` | Hz, °C | **fixes the 1 kHz/1 MHz/1 GHz incoherence; PA66's Dk moves with moisture** | CAMPUS, split-post resonator | 0% |
| ◆ registry `dielectricStrengthThickness` / `dielectricStrengthMedium` | m | **17/17 records carry a V/m whose specimen thickness is unknown** | IEC 60243-1 | 0% |
| ◆ registry `{glowWireIgnitionTemperature, glowWireFlammabilityIndex, arcResistance}` | °C, s | IEC 60335 | material datasheet | 0% |
| ◆ `material.insulatorParts[]` `{role: housing\|insert\|wafer\|potting\|sealingRing, materialRef}` | — | **the creepage path may run over the part that is not the housing** | material declaration | 0% |
| ▣ `ulYellowCardUrl`, material-side `ulFileNumber` | — | fetchable evidence | UL Product iQ (**counsel required**) | 0% |

### Group 7 — Electrical ratings

| Field | Unit | Consumer | Source | Now (MEASURED) |
|---|---|---|---|---|
| ✓ `electrical.ratedCurrentPerContact` | A | **THERM anchor** | all vendors | ✅ 381,118 (97.1%) |
| ✓ `electrical.ratedCurrentReferenceTemperature` | °C | **THERM — without it the current rating is nearly meaningless** | datasheet | ✅ **2,767 (0.71%)** |
| ✓ `electrical.ratedVoltage` | V | INSUL, CAT | all vendors | 58.3% |
| ✓ `electrical.contactResistance` | Ω | **THERM (I²R)** | datasheet; Stäubli MC4 publishes 0.25 mΩ; H+S (centre + outer separately) | 12.0%, 84% max-only, 5 vendors |
| ✓ `electrical.insulationResistance` | Ω | INSUL | datasheet | ~0% |
| ✓ `electrical.dielectricWithstandingVoltage` | V | INSUL | datasheet | 4.4% |
| ✓ `electrical.pairRatings[]` | A, V | **rating is a PAIR property** | datasheet | 1.31% |
| ▣ IEC-vs-UL split: `Current - IEC/UL`, `Voltage - IEC/UL` | A, V | INSUL | Schurter Solr (verified endpoint) | 0% |
| ▣ `workingVoltage` (≠ ratedVoltage) | V | **INSUL — the 60664 chain's first input** | datasheet prints it as a labelled row | 0% |

### Group 8 — Insulation coordination (IEC 60664) ★ NEW HEADLINE ★

| Field | Unit | Consumer | Source | Now (MEASURED) |
|---|---|---|---|---|
| ✓ `electrical.creepage` / `.clearance` | m | **INSUL** | datasheet; **ETIM/BMEcat**; IEC 61984 cert | ✅ 1,561 / 1,399 |
| ✓ `environmental.pollutionDegree` | — | **INSUL** | ETIM; Stäubli (PD 3); binder | ✅ 17,232 |
| — | — | — | — | ✅ **creepage ∧ pollutionDegree co-present = 0. Not one clearance figure in the catalog is currently interpretable.** |
| ✓ `environmental.overvoltageCategory` | — | INSUL | ETIM; Stäubli (OVC III) | 4.20% |
| ◆ `electrical.insulationPaths[].{from, to, clearance, creepage, distanceThroughInsulation, insulatorMaterialRef, materialGroup, pollutionDegree, ratedImpulseVoltage, insulationType}` | m, V | **INSUL — two scalars that don't say *between what* cannot support a design decision. This object makes path + material group + PD co-resident by construction.** | datasheet insulation table + 60664-1 | 0% |
| ▣ rated impulse voltage (U_imp) | V | INSUL | ETIM; IEC 61984 cert | 0% |
| ▣ IEC 61984 / IECEE CB certificate id + applied standard edition | — | evidence | `certificates.iecee.org` | 0% |
| **Computed** `outputs[].insulationStress.{appliedVoltage, creepage, clearance, margin, passes}` | m, V | **the product** | **our computation** | — |

> **Placement rule, non-negotiable:** a *computed* creepage requirement is an **output** with `origin`/`methodUsed`. **Never write it into `datasheetInfo.electrical.creepage`** — that field means "the manufacturer stated this."

### Group 9 — Thermal & derating ★ NEW HEADLINE ★

| Field | Unit | Consumer | Source | Now |
|---|---|---|---|---|
| ✓ `derating.currentVsAmbient` (`deratingCurve{temperature[], amplitude[]}`) | °C, — | **THERM — the number that decides whether a connector survives** | **vector chart digitisation from the datasheet PDF — the ONLY source** | 0% |
| ✓ `derating.currentVsEnergizedContacts` | —, — | **THERM — a 20-way at 3 A on *every* contact is a different component** | Molex `current_rating_percntct`; Amphenol; LEMO; Positronic | 0% |
| ✓ `derating.maxTemperatureRise` | K | THERM | datasheet ("30 K T-rise") | 0% |
| ◆ `derating.temperatureRiseVsCurrent {current[], temperatureRise[], standard, measurementMethod}` | A, K | **the EIA-364-70 base curve — the measured primitive from which the derating curve is a *construction*.** Without it you can never re-derive a rating at a 20 K vs 30 K limit | vendor T-rise chart | 0% |
| ◆ `derating.energizedContactsConvention` (all/adjacent/single/alternating/perStandard) | — | **a bare `ratedCurrentPerContact` today silently mixes single-contact and fully-loaded figures across 16 manufacturers** | datasheet footnote | 0% |
| ◆ `derating.{referenceCurrent, referenceTemperature}` | A, °C | **makes the curve self-contained** — 2.9% of parts have no `ratedCurrentPerContact` at all | chart caption | 0% |
| ◆ `derating.currentVsEnergizedContacts.{energizedContacts[], totalContacts}` | — | vendors publish against an **integer count in a stated housing**; 50% of a 4-way ≠ 50% of a 40-way | chart | 0% |
| ◆ `derating.measurementConditions.{airflow, mountingBoard{copperWeight, layerCount, boardThickness, traceWidth}, attachedWireGauge, matedWithSeries, sampleCount}` | m/s, … | **T-rise is not a part property** | test report | 0% |
| ◆ **remove `deratingCurve.amplitude` `maximum: 1`** | — | vendors normalising to an 85 °C rating produce amplitude >1 at 20 °C — currently **unrepresentable**, forcing a silent rescale (a fabrication) | — | — |
| ◆ **hoist `deratingCurve` to PEAS** | — | defined **structurally identically in RAS and CONAS today** — two modules ⇒ PEAS | — | — |

### Group 10 — Mechanical & durability

| Field | Unit | Consumer | Source | Now |
|---|---|---|---|---|
| ✓ `mechanical.matingCycles` | — | wear, CAT | datasheet; ODU quotes 100,000+ for LAMTAC | 14.4% |
| ✓ `mechanical.insertionForce` / `.withdrawalForce` | N | MECH | Mill-Max & Preci-Dip publish in grams per part | 1.24% |
| ✓ `mechanical.contactNormalForce` | N | **constriction resistance → THERM** | datasheet | very low |
| ✓ `mechanical.{length, width, height, weight, locking, …}` | m, kg | MECH | vendor | partial |
| ▣ `contactRetentionForce`, `panelThickness`, `threadSize`, `matingPinSize`, `polarised`, `endStackable`, `housingColour` | N, m | MECH, CAT | **Harwin API (all present today)** | 0% |

### Group 11 — Environmental, compliance, certification

| Field | Unit | Consumer | Source | Now |
|---|---|---|---|---|
| ✓ `environmental.operatingTemperature.{minimum, maximum}` | °C | **THERM, derating endpoint gate** | all vendors | 93.5% |
| ✓ `environmental.{ipRating, sealed, solderProcess, moistureSensitivityLevel, rohsCompliant, reachCompliant}` | — | CAT, compliance | vendor | partial–high |
| ▣ UL94 **connector-level** rating | — | safety | Harwin `flammabilityRating`; TE | 0% |
| ▣ per-approval-body booleans (UL, CSA, VDE, ENEC, cULus, CCC, TÜV, …) | — | compliance | **Schurter Solr publishes ~45 as facets** | 0% |
| ▣ `ik_schutzklasse`, `schutzklasse` (IEC 61140), `glow_wire_proof`, `lebensdauer` | — | safety | Schurter Solr | 0% |
| ▣ USCAR-2 class, terminal size, cavity count, keying A/B/C | m | automotive | Aptiv, TE Deutsch | 0% |

### Group 12 — High-speed / SI evidence ★ PAIR-SCOPED, DEMOTED TO SECONDARY ★

> **Structural rule (IEC 62153-4-15 §3.7): the DUT is the connector *with its mating connector and attached cables*. A measurement record must NOT hang off a part.** Proposal: a `conas-matedPair` registry (NDJSON, mirroring `conas-materials`), referenced from parts.

| Field | Unit | Source | Now |
|---|---|---|---|
| ◆ `matedPair.members[]` `{manufacturer, partNumber, role}` (`minItems: 2`) | — | **encodes the 62153-4-15 DUT structurally, so a per-part shielding claim is impossible to express** | 0% |
| ◆ `matedPair.sParameters[]` → PEAS `sParameterReference` `{file, format, portCount, referenceImpedance, portMap[], mixedMode, origin, deEmbedding}` | Hz, Ω | `origin` **required** — a vendor "S-parameter" is very often 3-D-simulated. **Samtec `suddendocs` (free, unauthenticated) and partially Kyocera AVX are the only sources on earth.** | ~0.1% of universe |
| ◆ `matedPair.crosstalk[]` `{type, aggressorPairId, victimPairId, riseTime, value, quantityKind, signalToGroundRatio}` | s, dB or % | records the Samtec 2.97%/11.70% @30 ps datum exactly as published (vendors report % of aggressor, not only dB) | 0% |
| ◆ `matedPair.{impedanceProfile, skew}` | Ω, s | vendor characterisation report | 0% |
| ◆ `matedPair.shielding[]` `{testMethod (**required**), dutDescription, measurand, quantityKind (lumped Ω vs Ω/m), …}` | Hz, Ω/dB | H+S publishes RF leakage per IEC 62153-4-x — **coax only** | 0% |
| ◆ `matingInterface.referencePlane` (mateFace/boardSurface/packageEdge/deEmbeddedToContact) | — | **required before ANY S-parameter can be attached** | 0% |

### Group 13 — Interface standards & pinout

| Field | Consumer | Source | Now |
|---|---|---|---|
| ✓ `mating.intermateabilityStandard` / `familyDataInterface.interfaceStandard` | **the standards-pinout join key** | datasheet; binder (100% standardised: IEC 61076-2-101/-109/-111) | ✅ 48,607, of which only ~34.6% genuine (rest are AirBorn proprietary strings — a data-cleaning task) |
| ✓ `contacts[].signalRole` + `nets[]` + `shield.groundedContactIds[]` | **ground-pattern study** | **the ~50-interface hand-transcribed table (Part 3 §3.5)** | 0% |
| ▣ M12/M8 coding letter (A/B/C/D/X/S/T/K/L/M) | interface identity | binder | 0% |
| ▣ MIL-STD-1560 insert arrangement (per-contact x,y within the shell) | **the ONLY public source of real circular contact geometry** | Glenair (Cloudflare) | 0% |
| ▣ mixed-contact population ("2 Low Voltage", "n Coax") | `contactSystem.nets` for circulars | LEMO; Nicomatic | 0% |

### Group 14 — Mating graph & accessories

| Field | Consumer | Source | Now |
|---|---|---|---|
| ✓ `mating.matesWith[].{manufacturer, series, relation, matedHeight}` | **CFORM mated geometry** | Samtec Solutionator (288,502 rows, each with a ground-truth male/female pair); Harwin `related.matings` | 195k parts / 407k edges, **91.9% of edges point at a series that does not exist under that manufacturer** |
| ▣ accessories/tooling graph | CAT | Harwin, TE, Molex | 0% |

### Group 15 — Provenance & evidence ★ ENFORCEMENT ★

| Field | Consumer | Now |
|---|---|---|
| ✓ `datasheetInfo.provenance[]` `{source, sourceName, sourceUrl, retrievedDate, fields[]}` | audit | ✅ present on **392,346 of 392,346** records with a `sourceName` |
| ◆ `provenance` on `geometry`, `contactSystem`, `conasMaterial`, `conasMatedPair` | **the entire Tier-2 layer is currently unattributable** | 0% |
| ◆ `provenance[].fieldPointers[]` — **RFC 6901 JSON Pointers** | makes "every numeric has an attributed source" **machine-checkable by Blade Runner**; `fields[]` is free-string and cannot be | — |
| ◆ `provenance[].documentChecksum` (`sha256:` of the bytes **actually fetched**) | converts "cited" into "verified" | — |
| ◆ `provenance[].locator` (`"p. 4, table 3"`) | human-verifiable in seconds. **No confidence scores** — a confidence number invites fabrication, a page number does not | — |
| ◆ `provenance[].retrievedDate` **required whenever `sourceUrl` present** | closes the "plausible URL, no fetch" hole at schema level | — |
| ◆ `provenance[].source` enum + `derived`, `standard`, `measurement`, `vendorCadModel` | **coordinates regenerated from `pitch × rows` are NOT `manufacturerDatasheet`** | — |
| ◆ `provenance[].derivation {method, inputs[], tool, toolVersion}` | makes every generated pin field reproducible and **impossible to masquerade as measured** | — |
| ◆ PEAS `externalFileReference {uri, mediaType, checksum, byteSize, retrievedDate, license, page}` | needed by CONAS/CAS/RAS/SAS/AAS ⇒ **PEAS, not CONAS** | — |

### Group 16 — Assumption & interval control ★ NEW — REQUIRED BY THE INTERVAL PRODUCT ★

The plan has no way to record that a result was computed over assumed inputs. If the deliverable is an interval, the schema must say *which* inputs were assumed and over what range, or the interval is uninterpretable.

| Field | Unit | Consumer | Why |
|---|---|---|---|
| ◆ `outputs[].assumptions[].{parameter (JSON Pointer), assumedRange {minimum, maximum}, basis: enum(archetypeDefault, standardPractice, vendorFamilyTypical), rationale}` | mixed | **every solver output** | Without it, `[0.09, 0.36] pF` is not a defensible engineering statement. With it, it is. |
| ◆ `outputs[].geometryTier` (parametric / footprintVector / step / refused) | — | UI, BR | The UI must state which tier produced the geometry and carry its error bar. |
| ◆ `outputs[].archetypeId` + `outputs[].archetypeMemberCount` | — | UI | ✅ 742 MPNs share the top archetype. The user must be told the answer is shared. |
| ◆ `outputs[].refusalReason` (enum incl. `rowPitchUnavailable`, `postCrossSectionUnavailable`, `boardThicknessUnavailable`) | — | UI, BR | **Absent beats wrong.** A refusal with a named missing field is a product feature. |

---

# Part 3 — The acquisition plan

**The single structural change: every coverage target is replaced by `(source, measured fill on N=200, date)`.** The plan's `→ 52%` figures had no measured basis; I checked and the routes that were supposed to supply them yield zero.

## 3.1 W0 — Corpus remediation (2 days, blocks nothing, do it first)

Not a scrape. Regenerate the **public** `connector.ndjson` with `description` stripped (✅ 377,107 strings) and `provenance.sourceName` reduced to a bare source class + date; keep full attribution in a private working copy for integrity checks. Byte-verify the deployed file against a clean-HEAD rebuild (`curl` the live file, `sha256sum` vs the rebuild) per the standing deploy rule. **Gate: the live 486 MB file contains 0 verbatim vendor descriptions and 0 named vendor endpoints.**

## 3.2 W1 — THE PROBE (1 week) — replaces P0 entirely

**Scope: 200 known-multi-row parts per candidate route, pulled end-to-end, measured per-field fill reported as a number.**

| Route | Method / endpoint | What it must yield | Status of the endpoint |
|---|---|---|---|
| **Molex Solr re-pull** | `search.molex.com/api/search/products-per-category`, Solr `rows`/`start`, full `fl` list | `rows`, `rowPitch`, plating thickness | ⚠ `/tmp/molex` ✅ deleted — this is a **new gated scrape**, not a re-map |
| **TE wide field set** | `api.te.com` via Playwright (Akamai) with an expanded attribute request | `rows`, `Row Spacing`, `Contact Length - Post` | ⚠ current dump has ✅ 14 fields, none of them rows |
| **Samtec Solutionator filter body** | `POST /api/solutionator/filters/solutionator/search?skip=&top=2000`, body `[]`; facets `/filters/{1..6}` | `rows` per part (facet `filters/6` is a **value list**, not a per-part assignment — the filter-body shape must be reverse-engineered) | ✅ 200, `totalRows: 288502`, plain curl, no auth |
| **Samtec PN grammar** | local regex `-S/-P/-D/-T/-Q` | `rows` on 55,450 parts | grammar confirmed from the CADENAS configurator |
| **PDF vector on real PDFs** | PyMuPDF `get_cdrawings()` → Bézier flatten → endpoint union-find → closed-loop → view segmentation → scale ladder | **`rowPitch`, `rows`, `pads[]`** | 0.3% on 1 doc; **failed on the 1 multi-view doc** |
| **PDF chart digitisation** | frame detect → tick calibration → polyline extract | **`derating.currentVsAmbient`** ← the new headline | unproven at scale |
| **KiCad footprint join** | `.kicad_mod` exact pads; symbol⋈footprint pin map | `rowPitch` oracle, `signalRole` | 7,283 connector footprints; **CC-BY-SA + design exception — oracle only, never redistributed** |
| **ETIM/BMEcat ×6** | file download + parse (WAGO, Weidmüller, Phoenix, Dinkle, Degson, Camdenboss) | **creepage + clearance + PD + OVC + CTI** | file download, not a scrape |

**EXIT GATES (pre-registered, pass/fail):**

| # | Gate | Consequence of failure |
|---|---|---|
| **G-RP** | Does **any** route yield ≥30% `rowPitch` fill on 200 known-multi-row parts? | **Fail ⇒ v1 is single-row-only over ✅ 70,016 parts (17.85%) with an explicit `rowPitchUnavailable` refusal state, and every B1/B3/B6 coverage number is rewritten.** |
| **G-ROWS** | Does Samtec PN regex + Molex/TE re-pull yield ≥70% `rows` on 200 parts from those three vendors? | Fail ⇒ 184,897 parts stay un-mappable to an archetype. |
| **G-DER** | Does chart digitisation yield a schema-valid, gate-passing `deratingCurve` on ≥25% of 200 parts whose datasheet visibly contains one? | Fail ⇒ the thermal headline needs a different source, and P4 is not scheduled. |
| **G-INSUL** | Does ETIM/BMEcat yield creepage **∧** clearance **∧** pollutionDegree co-present on ≥40% of 200 terminal-block parts? | Fail ⇒ the 60664 checker ships as a calculator with user-entered inputs, not a catalog screener. |
| **G-PDF** | What fraction of the ✅ 167,613 unique datasheet URLs is actually a PDF? | Settles the corpus-size discrepancy (below) before any compute is budgeted. |

**The PDF corpus is not 167,613 documents.** ✅ Measured today: 167,613 unique URLs across 14 hosts. Only ✅ **38,441** have a `.pdf` path. The three largest hosts are ✅ Samtec 55,450 (HTML product pages), Molex 34,062 (PDF sales drawings), TE 21,909 (`DocumentDelivery`, family-scope spec docs). The reviewer's stricter host+shape heuristic gives 64,233 PDF-shaped. **Both are far below 167,613; the true figure is 38k–64k and G-PDF settles it.** The plan's `167,613 × 0.3 s = 14 CPU-h` is therefore wrong twice: wrong corpus size, and it counts page 1 only, excluding OCR, Docling over every page, and Marker/Surya `use_llm` on "20–35k docs."

## 3.3 W2 — Governance, started day 1 (parallel, 0 build weeks)

`abt post --to conas` for the two fields the generator **throws** without and which have **no schema home at all**:
- `contacts[].crossSection.{width, thickness, diameter, cornerRadius}` / `.outline[[x,y]]`
- `geometry.pcbFootprint.recommendedBoardThickness`

Plus the two PEAS hoists (`deratingCurve`, `insulationMaterialGroup`) and `externalFileReference`. **These have committee latency. Starting them in week 20 is the plan's largest schedule error.**

## 3.4 W3 — Unblocked, clean, verified-endpoint vendors (E-difficulty, ~3 weeks)

Ordered by what the *new* product needs, all with verified-live endpoints and no bot gate:

| Target | Rows | Endpoint (verified) | What it unlocks |
|---|---|---|---|
| **Schurter Solr** | ~2,509 groups / ~25k articles | `GET /api/catalog/v1/products.json?rows=&facet=true&<anyKey>=<anyValue>` — every unrecognised param becomes `f_<param>:"<value>"`; every doc carries `link_datasheet_pdf_s` | IEC/UL split, ~45 approval-body booleans, `glow_wire_proof`, derating curves in every datasheet, **filtered inlets** |
| **Harwin** | 7,231 (we hold 5,173) | `GET api.harwin.com/v1/products` — plain GET, no auth | `flammabilityRating`, `pcTailLength`, `terminationFinish`, **`Test Reports`** documents |
| **HUBER+SUHNER** | 6,042 | `/en/shop/products/4707?currentPage=N`, 504 pages, plain curl | RF screening effectiveness in dB per IEC 62153-4-x (coax only) — the one public crack |
| **binder** | 5,281 | TYPO3+Solr; discovery hook `/en/product-search/suggest.json` | **derating curves**, pollution degree, 100% standardised `interfaceStandard` |
| **Stäubli MC4 / Amphenol H4 (PV)** | 3–6k | Playwright + PDF table extraction | **contact resistance 0.25 mΩ, current-vs-cross-section table, PD 3, OVC III, full derating curve** — six worst fields, vendor-published and citable |
| **ETIM/BMEcat ×6** | ~45k | file download | **the IEC 60664 triple at scale** |
| **Samtec Solutionator** | 288,502 rows | `POST .../search`, body `[]`, plain curl, no auth | `clearance`, `creepage`, `stackHeight`, `protocolDataRate`, **288k ground-truth mating edges** (fixes the 91.9% dangling-edge defect) |

## 3.5 W4 — Standards pinout join (2 weeks, no HTTP, deterministic)

~50 interfaces hand-transcribed once from the specs, checked in, unit-tested. Covers USB 2.0/3.x/Type-C, RJ45 T568A/B + PoE, HDMI, DisplayPort, DVI, VGA, PCIe CEM ×1/×4/×8/×16, SATA, M12 A/B/C/D/X/S/T/K/L/M, M8, IEC 60320 C5–C20, D-Sub (incl. CiA 303-1 CAN), microSD/SD, ISO/IEC 7816-2, SFP/QSFP, RJ11/12, 3.5 mm TRS/TRRS (CTIA vs OMTP — **the ground contact moves; asserting which is the deliverable**), XLR-3/-5 (ANSI E1.11), DIN 41524, mini-DIN, ATX 24-pin, EPS12V, PCIe aux, 12VHPWR, ARM CoreSight-10/-20, SAE J1962, IEC 60603-13, DIN 41612.

**Gate on assertion (mandatory):** assert only when `family ∈ {dataInterface, circular, rf, acInlet}` **AND** `positions` **equals** the standard's contact count **AND** no `with LED` / `integrated magnetics` token in the description. Positions mismatch ⇒ do not assert. **Expected yield: EST 7–9%** — and note this is EST, not measured; ✅ 48,607 parts carry `interfaceStandard` but only ~34.6% is genuinely standardised.

## 3.6 W5 — 3D/STEP acquisition: **CANCELLED**

Measured: envelope-only, 18.0 gold faces/pin, `NEXT_ASSEMBLY_USAGE_OCCURRENCE = 0`. Acquisition is a stateful CADENAS generation job — one browser session per part, non-deterministic file URL, ToS exposure. **It yields nothing the footprint does not yield more cheaply, and its own vendor tells you to prefer the print.** Keep only the free Samtec availability oracle (`/techspecs/blocks/aremodelsavailable/{pn}` → `true`, plain curl) as a cheap reconnaissance map. Never redistribute vendor STEP/IGES/IBIS/Touchstone.

## 3.7 W6 — Distributor tier: **BLOCKED PENDING COUNSEL**

DigiKey 314+315 (505,801 rows) is genuinely the single largest parametric win in the plan — `Row Spacing - Mating`, `Contact Finish Thickness - Mating`, `Contact Length - Post`, `Overall Contact Length`, `Insulation Height`, `Contact Shape`, `Mated Stacking Heights`, `Number of Rows`: 9 of our 10 worst fields, cross-manufacturer. It is also the item the plan's own §B7 classifies **High risk / counsel required**, and the keyed APIs (DigiKey OAuth, Mouser, Farnell, TME) are **legally worse than scraping**, not interchangeable with it: registering converts a contested browsewrap question into an executed contract with a named counterparty and an audit trail on their side.

**Decision: do not schedule any distributor work — scraped or keyed — before written counsel.** I am not pre-committing to abandoning it; I am refusing to build a plan whose critical path runs through it.

## 3.8 Explicitly dead (evidence-backed, do not revisit)

- **Würth REDEXPERT module 52 "Derating Connectors"** — `{"Data":[],"Total":0}`. The one vendor tool aimed at this is empty.
- **Sumitomo, Yazaki, Kostal** — 0 bytes, OEM-portal only.
- **XKB, Ckmtw, HRO, Jushuo, Cvilux, UCONN, Techly** — 0–645-byte shells; get them via LCSC.
- **TraceParts API** — `robots.txt: Disallow: /*/api/`.
- **Octopart/Nexar free tier** — 100 matched parts. A demo.
- **SnapEDA / SamacSys / GrabCAD** — redrawn from the same outline drawings; provenance fails the "must be true and verified" standard.
- **Transfer impedance / shielding effectiveness for board connectors** — undefined per part by IEC 62153-4-15 §3.7.

---

# Part 4 — What this makes possible that was impossible before

Six tools were rejected in the earlier round. Here is exactly what revives each, and whether it survives. **Two survive cleanly, two survive re-scoped, one survives conditionally, one stays dead.**

### 4.1 Paschen creepage screener — killed by no CTI + creepage/pollutionDegree never co-present

✅ **Re-verified: creepage ∧ pollutionDegree co-present = 0 of 392,346.** Every one of the 1,561 creepage figures is currently uninterpretable.

**Revived by:** `insulationPaths[].{from, to, creepage, clearance, materialGroup, pollutionDegree, ratedImpulseVoltage}` (◆) — which makes the three inputs co-resident **by construction** rather than by luck; `materialGroup` (◆ PEAS hoist, indexed from CTI: I ≥600 V, II 400–599, IIIA 175–399, IIIB 100–174); `workingVoltage` (▣); ETIM/BMEcat as the at-scale source.

**Survives — RE-SCOPED, and it is better than the original.** Two corrections to the original framing:
1. It should not be a **Paschen** calculator. Paschen governs clearance in air; IEC 60664-1's tables already encode it. The defensible tool is an **IEC 60664 insulation-coordination checker**: `workingVoltage → OVC → rated impulse voltage → clearance`; `workingVoltage → pollution degree → material group → creepage`. Arithmetic and table lookup. Error bars ~10%, not 12 dB. **It is a pass/fail on a real design decision.**
2. Coverage is honest and small: **EST 8–12% of the catalog end-to-end**, concentrated in ✅ WAGO 15,841 + Phoenix 12,120 + Weidmüller 11,182 + Würth 5,774 = **44,917 parts**, of which EST 45–60% should close. Going from **0 interpretable records** to ~25,000 with a full auditable chain is the most defensible safety capability in the plan. Gated on **G-INSUL**.

### 4.2 Grover parasitic foundry — killed by no output format and tailLength ∩ matedHeight = 0

**Revived by:** `contact.tailLength` (◆), `matedEngagementLength` (◆), `recommendedBoardThickness` (◆), `crossSection.{width,thickness,diameter}` (◆) — plus an output format that already exists: `Rlgc.hpp::spice_ladder_deck` writes a SPICE ladder, and `Mna.hpp` is cross-checked against Kirchhoff's in-process libngspice to 0.01 dB NEXT / 0.05 dB FEXT.

**Survives — RE-SCOPED to archetype scope with an interval.** What ships is a SPICE subcircuit + Touchstone-equivalent for a **geometry class**, carrying `archetypeMemberCount` and `assumptions[]`, over the ✅ 70,016 single-row parts (17.85%) unless **G-RP** passes. It is not a per-SKU foundry and must never be presented as one. The image-sign correction is what makes it *correct* rather than 1.45–2.22× low in the unsafe direction — that alone justifies building it.

### 4.3 Schelkunoff shield screener — killed by 7% recall

**Revived by:** nothing acquirable. `shield.present` is 0.46%; adding `shield.{materialRef, coverageFraction}` raises *recall* but the **measurand does not exist per part**: IEC 62153-4-15 §3.7 defines the DUT as connector + mating connector + attached cables. H+S publishes screening effectiveness in dB — **for coax only**.

**DOES NOT SURVIVE. It stays dead.** I am holding this line explicitly because it is the place where a data-acquisition plan is most tempted to promise a revival. No field in Part 2 changes the answer. The only honest move is the `conas-matedPair` registry (§Group 12), whose job is to make the *absence* of evidence structurally visible rather than letting a `shielded: true` boolean stand in for it. A registry with 200 well-attributed pairs and 392,146 parts that honestly say nothing is strictly better than what exists now.

### 4.4 Poynting CISPR bridge — killed by no pin radius and internal-loop-only

**Revived by:** `contacts[].crossSection.{width, thickness, diameter}` (◆) supplies the pin radius; `centerlinePath[]` (◆) supplies the external loop; `NearField::h_loop_vec` already computes exact Biot–Savart over a 3-D polyline with both limits asserted in tests.

**Survives — RE-SCOPED to a comparator, not a predictor.** The physics becomes computable, but the CISPR claim does not: radiated emissions depend on the board and the attached cable, and the connector is one term. The honest deliverable is **"connector contribution to loop area and near-field H, connector A vs connector B, same board"** — a relative number with a stated ±. Presenting it as a CISPR limit-line prediction would be exactly the kind of unfounded claim that got it rejected the first time.

### 4.5 Zobel filtered connectors — killed by 46 records

**Revived by:** Schurter Solr (✅ verified endpoint, ~2,509 product groups, publishes `filter` variants, IEC/UL split, and ~45 approval-body facets), plus Bulgin and the filtered-D-sub segment.

**Survives — CONDITIONALLY, and it is marginal.** Filtered connectors are a genuinely small population. Expect **EST low thousands, not tens of thousands**. The Schurter pull is cheap (E-difficulty, no gate) and lands in W3 anyway for other reasons, so the marginal cost is near zero — but it should not be sold as a headline capability. Ship it as a filter tab if the record count clears ~1,500; otherwise leave it dead.

### 4.6 Neumann — limited to 40,551 parts by missing rows

✅ **Re-verified: `rows` present on 142,393 of 392,346 (36.3%), and exactly 0 on Molex (94,255), Samtec (55,450), TE (35,192), JAE (4,106), Amphenol RF (5,776), Adam Tech (561) = 195,340 parts.**

**Revived by:** `rows` from the Samtec PN grammar (`-S/-P/-D/-T/-Q`, confirmed from the vendor's own configurator), Sullins (already ✅ 82,518/82,518), and a Molex/TE re-pull with a wider field set.

**Survives — this is the one clean, unambiguous win.** `rows` genuinely is published by all three, genuinely is decodable for Samtec locally, and its acquisition is bounded. **But it does not get you to a pin field**, because `rowPitch` (✅ 57 records) remains missing. Neumann's part count goes up; the *geometry* coverage does not move until **G-RP** passes. Do not let the `rows` win be reported as a geometry win — that conflation is the single most misleading thing in the original plan.

---

# Part 5 — Risks, costs and legal

## 5.1 Legal — immediate, not prospective

✅ **Verified live today:** `HTTP/1.1 200 OK`, `Content-Length: 486233766`, no auth, no rate limit, on `https://kelvin.openconverters.com/kelvin/connector.ndjson`. ✅ **377,107 records carry a verbatim vendor description.** ✅ **392,346 records carry a named source endpoint and a `retrievedDate`.**

| Exposure | Assessment |
|---|---|
| **Copyright — compilation** | Facts (2.0 A, 250 V, 26 positions) are uncopyrightable under *Feist*. Marketing prose is not a fact. 377,107 strings copied wholesale from 16 named vendor APIs and rehosted as a downloadable file is a textbook compilation fact pattern. The remedy sought would be takedown of the file, **which is the product**. |
| **Our own provenance is the exhibit** | `sourceName: "Molex Solr API (search.molex.com)"` + `retrievedDate` × 392,346, publicly indexed by field name. §B1 Group 15 proposes to **strengthen** this (`documentChecksum`, `locator`, `derivation`) — every one of those is scientifically right and evidentially worse. **The correct conclusion is not to weaken provenance. It is that what we do must be clean enough that our own records prove it.** |
| **Zero-backend forecloses the best defence** | "Derived data is ours" is the strongest argument and architecturally unusable: there is no backend, so the browser receives the source values as an unfiltered Range read of the raw NDJSON (`Kelvin/web/src/engine.js:161`). Copy-traps (vendor typos, unit-conversion artifacts) survive a diff. |
| **EU/UK database right** | WAGO, Weidmüller, Phoenix, Würth (DE), Harwin (UK) are already in the corpus. Directive 96/9/EC **Art 7(1)** covers extraction of a substantial part; **Art 7(5)** separately covers repeated and systematic extraction of insubstantial parts — the literal description of a paginated scrape. Our own *Ryanair* (C-30/14) cite closes the other door: unprotected DB ⇒ contract restricts without limit. **There is no configuration in which we win by default.** |
| **Keyed APIs are worse, not safer** | DigiKey OAuth, Mouser `apiKey`, Farnell `callerId`, TME token+HMAC each require clicking accept — converting browsewrap into an executed contract with an audit trail on their side, under terms that prohibit building competing aggregated databases. §B2 offers OAuth as a convenience alternative to scraping. It is the opposite. |
| **The circumvention is written down** | Samtec's `robots.txt` disallows `ClaudeBot`/`GPTBot`/`CCBot`, sets `Content-Signal: search=yes,ai-train=no,use=reference`, and states the restrictions are **express reservations under Art. 4 of Directive 2019/790** — which makes the commercial TDM exception unavailable. §B7 quotes the block and then says bulk pulls *"must not present as ClaudeBot."* That sentence converts a cease-and-desist into a willfulness argument and, in Germany, a fast *einstweilige Verfügung*. **Delete it from the plan and from practice.** |
| **Two internal contradictions to resolve** | §B7 classifies distributor bulk extraction High-risk while §B3 ranks DigiKey #3 at 3 weeks. §A8 forbids shipping a KiCad-derived pin-field DB while §B5 makes KiCad joins a *source* for `signalRole`. |
| **ETIM/BMEcat "published for redistribution"** | Asserted with no evidence. ETIM classes and features are ETIM International's licensed IP; vendor BMEcat feeds come under trading-partner agreements. **Add to the counsel list; do not treat as free.** |

**Counsel questions (six, unchanged in substance, reordered by urgency):**
1. The already-published 486 MB file — remediation adequacy, and whether the pre-remediation version creates residual exposure.
2. Keyed manufacturer/distributor APIs: contract formation and the "no competing database" clauses.
3. Distributor bulk extraction: EU sui generis + *CV-Online* detriment test + DigiKey's ToS under *Ryanair*.
4. Art. 4(3) DSM machine-readable reservations (Samtec/CADENAS) and what "use=reference" permits.
5. UL Product iQ / UL Prospector bulk access. **Nothing has been pulled and nothing will be without an explicit yes.**
6. ETIM/BMEcat redistribution terms; and the licence we republish the derived layer under (sourced and derived layers may need different terms).

**Realistic worst case is not damages. It is a takedown naming `kelvin.openconverters.com`, permanent Akamai/Cloudflare fingerprint bans that kill the pipeline, and reputational damage to a brand whose entire premise is being the trustworthy open reference.**

## 5.2 Maintenance — 0 of 38 person-weeks budgeted, and the endpoints demonstrably rot

Evidence already on disk: ✅ `/tmp/molex` deleted; TE surviving only as 27 opaque `.xls` blobs; LCSC's `wmsc.lcsc.com` now `{"code":404}`; every catalog record stamped `retrievedDate: 2026-06-24` with no refresh mechanism; Weidmüller and Phoenix hard-403 to curl today (23,302 documents). The plan proposes ~50 more endpoints of exactly the class that died (Samtec Solutionator, Schurter Solr, LEMO `/views/ajax`, Mill-Max `mmax_product_search`, Preci-Dip behind Turnstile, Rosenberger Shopware 5).

**Cost: EST 8–15 pw/yr to stand still — 25–40% of the entire build budget, recurring, currently uncosted.** Budget it explicitly or the coverage numbers decay silently.

## 5.3 Surviving critiques — things I could not refute

1. **The archetype collapse is real and it weakens the moat.** ✅ 4,181 answers. A five-field form gets the same number without our database. Our residual advantage — the refusal state, the interval, the assumption record, and the thermal/insulation layer — is real but thinner than the plan claims.
2. **The audience mismatch is real.** ✅ pinHeaderSocket (120,994) + boardToBoard (118,164) = 61% of the catalog, dominated by 2.54 mm THT. Nobody has ever needed the NEXT waveform of a 2×25 0.1″ header. The parts where crosstalk decides a design are the ones where the vendor already publishes free Touchstone and the contact beam was never in the file. **The SI tool is accurate where it is not needed and needed where it is not accurate.** That is why it is demoted, not deleted.
3. **The one external validation datum is out of band by 5–10×** and its miss was pre-excused. There is currently **no experiment in the plan that can falsify the solver** — which is why G0 is pre-registered.
4. **The PDF pipeline is the least-proven workstream and it now carries the headline.** n=3 documents; the only multi-view test failed; and the corpus is ✅ 38k–64k documents, not 167,613. This is the largest single risk in the revised plan and it is why P4 is probe-gated.
5. **`rowPitch` has no measured route.** Not one. Everything downstream of "geometry 0% → 61%" is speculation until G-RP returns a number.

## 5.4 Cost summary

| | Plan as written | This decision |
|---|---|---|
| Solver | 5–6 weeks | **~2 weeks** (closed form + image sign + skin model, and 1 week offline BEM oracle) |
| Scrape | ~38 pw | **~20 pw to a defensible v1**, distributor tier excluded pending counsel |
| Maintenance | **0** | **8–15 pw/yr, explicitly budgeted** |
| Governance latency | starts week 20 | **starts day 1** |
| Legal remediation | not scheduled | **2 days, this week, blocking** |

---

# Part 6 — Recommended sequence

Each phase has a **gate that can fail**, and a written consequence of failure. No phase is scheduled on an assumption another phase is supposed to prove.

### Phase 0 — Corpus remediation · 2 days · blocks nothing
Strip `description` and reduce `provenance.sourceName` on the public artifact; keep full attribution privately. Rebuild and byte-verify against clean HEAD.
**GATE:** live 486 MB file contains 0 verbatim descriptions and 0 named endpoints, verified by `curl` + `sha256sum` against a clean-HEAD rebuild. **Fail ⇒ nothing else ships.**

### Phase 1 — The two probes · 1 week · parallel · pre-registered
**1a — G0, solver falsification.** 10 geometries, closed form (~300 lines) vs full BEM/PEEC. **GATE: does the BEM move a reported number by more than the input uncertainty on the same geometry?** Predicted: no (3.8% vs ±25%). **Fail-to-falsify ⇒ reinstate the BEM tier and rewrite Part 1.**
**1b — Field availability probe, N=200 per route.** Gates **G-RP**, **G-ROWS**, **G-DER**, **G-INSUL**, **G-PDF** as specified in §3.2. **Every `→ target %` in the acquisition plan is rewritten as `(source, measured fill, date)` before any further week is committed.**
**1c — Governance:** `abt post --to conas` for `crossSection` dimensions, `recommendedBoardThickness`, and the PEAS hoists. Day 1.

### Phase 2 — Counsel · 2 weeks calendar, ~0 build weeks · parallel with Phase 3
Six questions from §5.1. **GATE: written answer on (a) the remediated corpus, (b) keyed APIs, (c) distributor bulk extraction, before any distributor or keyed-API work is scheduled.** No exceptions, no "we'll ask later."

### Phase 3 — Thermal + insulation + closed-form parasitics · 3 weeks
The three things that are arithmetic and table lookup, error bars ~10%, and answer a real pass/fail:
- I²R + T-rise against the extracted `temperatureRiseVsCurrent` primitive.
- The IEC 60664 chain: `workingVoltage → PD → material group → creepage/clearance`, via `insulationPaths[]`.
- Closed-form C/L with the image-sign correction, intervals, `assumptions[]`, `archetypeMemberCount`, refusal states.

**GATE:** T-rise model reproduces the vendor-stated `maxTemperatureRise` within 15% on ≥20 parts where both exist; 60664 chain reproduces ≥20 vendor-stated creepage figures within one table row. **Fail ⇒ the physics is wrong, not the data.**

### Phase 4 — Clean vendor pulls · 3 weeks · gated by Phase 2
Schurter, Harwin, H+S, binder, Stäubli/Amphenol PV, ETIM×6, Samtec Solutionator. All verified-live endpoints, no bot gate, no distributor exposure.
**GATE:** each vendor's measured per-field fill matches its N=200 probe within ±10 points. **Divergence ⇒ the probe was unrepresentative; re-probe before continuing.**

### Phase 5 — PDF pipeline · 8 weeks · scoped by G-DER and G-PDF, **not scheduled if G-DER fails**
Primary target: **derating charts** (the only source that exists — REDEXPERT module 52 is empty). Secondary: footprint pads and `rowPitch`, only if G-RP showed vector extraction is the winning route. Mandatory scope classifier (FAMILY vs PART fan-out) — get it wrong and you inject 300k wrong records.
**GATE:** the cross-check pass (extracted pitch vs catalog pitch on ✅ 246,344 parts; extracted positions vs ✅ 365,951) rejects <2% of pages for disagreement, and **no page is ever resolved by "preferring" one value.**

### Phase 6 — Offline archetype batch · 1 week
384 shape classes (or 4,181 archetypes if the extra resolution proves to matter) through `Bem2d.hpp` + a small PEEC, both interval endpoints, gated against FasterCap (G2) and FastHenry2 (G1), h-converged against OMFEM (G3). Published as a Kelvin shard with the residual per shape class.
**GATE:** every archetype's BEM-vs-closed-form residual is published, and any class where the residual exceeds the input-uncertainty interval is flagged in the UI.

### Phase 7 — Standards pinout + mating-graph repair · 2 weeks
The ~50-interface table with the strict assertion gate; repair of the ✅ 91.9% dangling `matesWith` edges using Samtec's 288,502 ground-truth pairs; AirBorn `interfaceStandard` misclassification cleanup.

### Phase 8 — Breadth · unscheduled, subject to Phase 2
LCSC, Rosenberger, Radiall, ITT, Aptiv, Glenair, Kyocera/Panasonic/Omron, Mill-Max/Preci-Dip, LEMO/ODU. **Distributor tier and keyed APIs only on a written yes.**

---

## The one-paragraph version

Build the closed-form tier, not the 3-D one — measured, the BEM buys 3.8% on a quantity whose missing input moves it 25%. Precompute all ✅ 4,181 archetypes offline in an afternoon and ship a table; nothing factors in the tab, which deletes four weeks of solver work, the SIMD kernel, and the COEP/threads problem outright. Make the interval the product, with an explicit refusal state, because ✅ only 70,016 parts (17.85%) — not 137,686 — have a determinate pin field, and even those are missing a post cross-section that has no schema field to live in. Point the tool at current derating and IEC 60664 insulation coordination, which are the questions a power engineer actually asks, are arithmetic rather than a 3-D solve, and live in datasheets we are already licensed to read. Strip the ✅ 377,107 verbatim vendor descriptions off the ✅ publicly-served 486 MB file this week. And before committing another person-week, run two pre-registered experiments — one that tries to falsify the solver tier, one that measures whether `rowPitch` can be acquired at all — because the plan as written spends 38 person-weeks on coverage targets derived from an assumption that I checked today and found to be false.