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

## Families (v1)

MOSFET, diode, capacitor, resistor, controller — the five with both an HS reference selector and
a KH requirements emitter. (IGBT/BJT/analog/varistor are review-gated follow-ups; magnetics stay
with MKF; connectors have no emitter yet.)

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

v1 core complete and parity-green. Heaviside switchover and the Kirchhoff web candidate table are
staged (see the implementation plan, phases 3–4).
