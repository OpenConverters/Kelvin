# Kelvin

Deterministic parts librarian over the TAS component database — the shared, single-source
component selector for **Kirchhoff** (no-AI candidate list) and **Heaviside** (LLM chooser over
the same list). Given a schema `designRequirements` block, Kelvin returns a ranked, auditable
list of real candidate parts.

Sibling of [Kirchhoff](../Kirchhoff) and [Heaviside](../Heaviside). Design docs live in
`Kirchhoff/docs/KELVIN_PROPOSAL.md` (v2) and `Kirchhoff/docs/KELVIN_IMPLEMENTATION_PLAN.md`.

## What it is

- **One selection authority.** The deterministic filter/rank logic (ported parity-exact from
  Heaviside's `catalogue/selector.py`) + the `designRequirements → constraints` policy mapping
  (ported from `kirchhoff_fill.py`) live here once. KH consumes the candidate list directly; HS
  runs its LLM chooser over it. No two implementations to drift.
- **A compact binary index.** One shard per family holds only the fields selection touches + the
  byte span of the full record in the source NDJSON. A capacitor query scans ~30 MB instead of
  parsing 292 MB — ~50× faster than streaming, exact same pick.
- **Growth-aware.** The TAS nightly appends to the catalogs; the builder does an incremental
  tail index (prefix-hash gate) so re-indexing skips the unchanged prefix.

## Families

- **v1 (parity-locked to the HS Python selector):** MOSFET, diode, capacitor, resistor, controller.
- **Phase 5 (new selectors, no HS reference — physics-sensible bounds, review-gated):** IGBT, BJT,
  varistor.

(Analog ICs + the cross-ref ranker are further follow-ups; magnetics stay with MKF; connectors
have no requirements emitter yet.)

## Build

```bash
cmake -S . -B build -G Ninja
ninja -C build -j4            # kelvin lib, kelvin-index CLI, test_kelvin, PyKelvin
./build/test_kelvin          # Catch2 suite (parity/index/determinism/select/components)
```

Emscripten/embind (`libKelvin`, for the KH web worker) builds under `emcmake cmake`.

## Index a catalog

```bash
./build/kelvin-index --data /path/to/TAS/data --out ~/.kelvin/index          # all families
./build/kelvin-index --data /path/to/TAS/data --out ~/.kelvin/index --check  # nightly staleness gate
```

## Use from Python (Heaviside)

```python
import PyKelvin
eng = PyKelvin.Engine("/path/to/TAS/data", "~/.kelvin/index")
res = eng.select("mosfet", {"ratedDrainSourceVoltage": 60, "ratedContinuousDrainCurrent": 5,
                            "maximumOnResistance": 0.1}, {})
res["candidates"][0]["mpn"]   # the deterministic default pick
```

## Validation

- **`[parity]`** replays a golden generated from the Python selector over committed fixtures:
  `candidates[0].mpn`, rejection histograms, and margins must match exactly. This is the gate.
- **`[index]/[determinism]/[components]`** cover round-trip, byte-identical rebuilds, incremental
  tail builds, lineno tie-breaking, and the TAS-document walk.
- A **differential fuzzer** (`heaviside/tools/diff_kelvin.py`) runs Kelvin vs the Python selector
  over the live full DB — confirmed 0 mismatches across all five families.

See `docs/CONTRACT.md` for the frozen interface.

## Status

- **Core (v1 + Phase 5 + cross-ref ranker):** complete and parity-green — `test_kelvin` at 1257
  assertions / 43 cases; differential fuzzer 0 mismatches over the full DB.
- **Kirchhoff (Phase 3):** C++ (`select_components`/`bind_part`) done; web PartDrawer shows real
  WASM-Kelvin candidates (Playwright e2e green). *Remaining:* in-drawer bind+re-sim over hosted
  NDJSON Range requests, and the prod shard/NDJSON deploy (ABT #124 tail).
- **Heaviside cross-ref seam:** landed (HS `bbb2ede`) — `kelvin_adapter.cross_reference()` wraps
  the C++ ranker; parity test (10 cases) proves verdicts match HS `scoring.py`. *Validated but
  not yet activated:* `crossref_pipeline.py` still scores in Python; no live caller routes through
  the adapter.
- **Heaviside selector switchover (Phase 4, ABT #125):** not started — `kirchhoff_fill`/`assemble`
  still run the all-Python selector; `kelvin_adapter.select` has no live caller.

See the implementation plan (`Kirchhoff/docs/KELVIN_IMPLEMENTATION_PLAN.md`) phases 3–4 and ABT
#123/#124/#125.
