# Kelvin contract (v1)

Frozen interface between Kelvin and its consumers (Kirchhoff, Heaviside). Input is the governed
`designRequirements` schema block (SAS/CAS/RAS/CTAS) — **no new schema, no schema changes**.
Output (`SelectionResult`) is Kelvin's own versioned library type, not a governed schema.

## select

```
Engine(data_dir, cache_dir="", quiet=false)
Engine.select(category, design_requirements, options) -> SelectionResult   # throws NoCandidates
```

`category` ∈ {`mosfet`, `diode`, `capacitor`, `resistor`, `controller`}.

### options (Kelvin's own object; all optional)

| key | meaning | default |
|---|---|---|
| `tiebreaker` | force a tiebreaker (family enum string) | per-family default¹ |
| `opFsw` / `switchingFrequency` | converter fsw (Hz) — HV mosfet total-loss + controller fsw | — |
| `operatingPoint` `{iRms,vds,duty,fsw}` | real op point (KH-mode loss ranking); overrides the rating proxies | — |
| `inputVoltage` | controller Vin | — |
| `topology` | controller topology (normalized HS short name) | — |
| `technologyAllowed` | array; overrides schema `allowedTechnologies` | family default² |
| `excludeDiscontinued` | drop non-`production` rows | `true` |
| `qrrMax` | diode Qrr hard limit | none |
| `maxCandidates` | cap the ranked list | 25 |
| `includeEnvelope` | attach the full TAS envelope per candidate | `true` |

¹ mosfet: `lowest_total_loss` when an op point is present (HV path), else `lowest_rds_on`; diode
`lowest_vf`; capacitor `lowest_esr`; controller/resistor have fixed internal orderings.
² mosfet `{Si,SiC,GaN}`; capacitor any.

### SelectionResult

```json
{
  "category": "mosfet",
  "tiebreaker": "lowest_rds_on",
  "totalRowsConsidered": 9943,
  "alternativesConsidered": 427,
  "rejections": { "unreadable_row": 886, "vds_rated_low": 8610, ... },
  "candidates": [
    { "mpn": "...", "manufacturer": "...", "line": 42, "srcOffset": 1234, "srcLength": 1490,
      "margins": { "vds_margin": 2.0, "rds_on_headroom": 15.3, "qg_headroom": null },
      "sortKey": { "metric": "lowest_rds_on", "value": 0.005 },
      "evidence": { "datasheetUsable": true, "thermalPresent": true, "qgPresent": true },
      "envelope": { "semiconductor": { "mosfet": { ... } } } }
  ]
}
```

- `candidates[0]` is the deterministic default (== the HS `select_*` winner under identical
  input — the parity invariant).
- `rejections` carries only non-zero keys (matches the Python `Counter`). Present on success too.
- `margins` are **ratios** (rated/required, headroom = max/actual); `inf` → JSON `null`.
- Ordering is stable with source line number as the final key (reproduces Python `min()`/`max()`
  first-in-file-order-wins).

## Errors

| exception | when | payload |
|---|---|---|
| `NoCandidates` | no row satisfies the constraints | `category`, `rejections`, `totalRowsConsidered` |
| `InvalidOptions` | bad options/constraints (maps Python `ValueError`) | message |
| `DataError` | structurally invalid NDJSON at index-build time | `file`, `lineno` |

A view-unreadable row is **not** an error — it is counted in `rejections.unreadable_row` and
skipped (the one sanctioned "skip", accounted not silent).

## select_components / bind_part (TAS-document level)

```
select_components(engine, tas, options) -> { "components": [ {ref, family, kind?, filled,
                                              mpn?|deferred?|error?, selection?, rejections?} ] }
bind_part(tas, ref, envelope) -> tas'      # stamps envelope into the component's data slot
```

Skips body diodes, numerical-convergence aids (`Csn*/Rsn*/Csw*` or `__kh_numerical_aid__`),
magnetics (→ MKF), and already-bound parts. A per-component `NoCandidates` is captured in the
record (not thrown) so one unsatisfiable part doesn't sink the whole BOM result.

## Determinism guarantees

- Same fixture + same constraints ⇒ same `candidates[0]`, same histogram, same margins as HS.
- Same source file ⇒ **byte-identical** shard (`build_id` is a content hash; no timestamps).
- Native and WASM produce identical `SelectionResult` JSON (one span-based reader).
- The nightly's append-only growth is handled by incremental tail indexing (prefix-hash gate).
