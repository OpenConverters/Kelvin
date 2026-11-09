// Which comparison model web/src/crossref.js puts in force for a magnetic original
// (famFor -> variantFor). Run: node tools/qarlos/test_crossref_variants.mjs
//
// Qarlos 2026-11-09, seed 20889483: the signal common-mode choke ACM-21-161M-T (MAS subtype
// 'commonModeChoke', no inductance, |Z| = 160 ohm at 100 MHz) was refused with "no L value in the
// catalogue": the inductor model asked it for an inductance its record has no reason to state.
// Row values below are the browse row of the real TAS record.
import assert from 'node:assert/strict'
import { dirname, join } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const here = dirname(fileURLToPath(import.meta.url))
const { famFor, runCrossRef } = await import(
  pathToFileURL(join(here, '..', '..', 'web', 'src', 'crossref.js')).href)

const acm = { mpn: 'ACM-21-161M-T', manufacturer: 'Abracon', device_type: 'commonModeChoke',
  inductance: null, impedance_100mhz: 160, impedance_peak: 160, impedance_peak_freq: 1e8,
  dcr: 0.35, rated_current: 0.33, widthM: 0.0012, heightM: 0.0014 }

// 1. A signal CMC (no L, a 100 MHz |Z|) is ranked on its impedance, within its own device type.
const f = famFor('magnetic', acm)
assert.equal(f.category, 'chipBead', 'signal CMC must take the impedance model')
assert.equal(f.primary, null)
assert.deepEqual(f.sameFacet, { f: 'device_type', label: 'device type' })
assert.equal(f.spec(acm).impedance_100mhz, 160)

// 2. It runs: the pool is restricted to CMCs and the ranker receives the impedance spec.
let seen = null
const engine = {
  async browse(family, q) {
    seen = q
    return { total: 1, rows: [{ ...acm, mpn: 'OTHER-CMC', manufacturer: 'TDK', impedance_100mhz: 150 }] }
  },
  async crossReference(category, orig, cands) {
    assert.equal(category, 'chipBead')
    assert.equal(orig.impedance_100mhz, 160)
    assert.equal(cands[0].impedance_100mhz, 150)
    return { candidates: [] }
  },
}
await runCrossRef({ family: 'magnetic', original: acm, manufacturers: ['TDK'], engine })
assert.deepEqual(seen.filters.device_type, ['commonModeChoke'])

// 3. A power CMC states its common-mode inductance and stays on the inductor model.
const power = { ...acm, mpn: 'POWER-CMC', inductance: 1e-3 }
assert.equal(famFor('magnetic', power).category, 'magnetic')
// 4. A CMC stating neither stays on the inductor model and is refused for the missing L.
const bare = { ...acm, mpn: 'BARE-CMC', impedance_100mhz: null }
assert.equal(famFor('magnetic', bare).category, 'magnetic')
await assert.rejects(runCrossRef({ family: 'magnetic', original: bare, manufacturers: ['TDK'], engine }),
  /no L value/)
// 5. Beads and inductors are unchanged.
assert.equal(famFor('magnetic', { device_type: 'chipBead' }).category, 'chipBead')
assert.equal(famFor('magnetic', { device_type: 'inductor', inductance: 1e-5 }).category, 'magnetic')

console.log('crossref variants: all checks passed')
