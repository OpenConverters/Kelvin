// Main-thread facade over the Kelvin WASM worker + the hosted data triple
// (/kelvin/manifest.json + <family>.kidx + <family>.ndjson). Same contract as
// Kirchhoff's kh.js Kelvin section: shards load into WASM by bytes, selection and
// browsing run fully in-browser, and a part's FULL record only ever crosses the
// wire as an HTTP Range slice of the source NDJSON (a 206 of a few KB).

const KELVIN_BASE = '/kelvin'

let _worker = null
let _seq = 0
const _pending = new Map()

function worker() {
  if (!_worker) {
    _worker = new Worker(new URL('./worker.js', import.meta.url), { type: 'module' })
    _worker.onmessage = (ev) => {
      const { id, ok, result, error } = ev.data
      const p = _pending.get(id)
      if (!p) return
      _pending.delete(id)
      if (ok) p.resolve(result)
      else p.reject(new Error(error))
    }
  }
  return _worker
}

function call(fn, ...args) {
  return new Promise((resolve, reject) => {
    const id = ++_seq
    _pending.set(id, { resolve, reject })
    worker().postMessage({ id, fn, args })
  })
}

async function callJson(fn, ...args) {
  const out = await call(fn, ...args)
  if (typeof out === 'string' && out.startsWith('Exception: ')) {
    throw new Error(out.slice('Exception: '.length))
  }
  return JSON.parse(out)
}

export function loadEngine() {
  return call('__init__')
}

// ── manifest: the deploy pairing record (per family: buildId + sourceSize) ────
let _manifestPromise = null

export function manifest() {
  if (!_manifestPromise) {
    _manifestPromise = (async () => {
      const res = await fetch(`${KELVIN_BASE}/manifest.json`, { cache: 'no-store' })
      if (!res.ok) throw new Error(`Kelvin manifest not hosted (HTTP ${res.status}) — deploy manifest.json with the shards`)
      const m = await res.json()
      if (!m?.families) throw new Error('Kelvin manifest malformed (no families)')
      return m
    })()
  }
  return _manifestPromise
}

async function manifestEntry(family) {
  const m = await manifest()
  const e = m.families[family]
  if (!e) throw new Error(`Kelvin manifest has no entry for '${family}'`)
  return e
}

// ── shard loading, cache-busted by buildId in Cache Storage ──────────────────
const _shards = new Map() // family -> Promise<meta>

// load progress events for the header's temperature rail: {family, phase: loading|loaded|error}
export const shardEvents = new EventTarget()
function emitShard(family, phase) {
  shardEvents.dispatchEvent(new CustomEvent('shard', { detail: { family, phase } }))
}

async function fetchShardBytes(family, buildId) {
  const url = `${KELVIN_BASE}/${family}.kidx?b=${buildId}`
  if (typeof caches === 'undefined') {
    const res = await fetch(url)
    if (!res.ok) throw new Error(`Kelvin shard '${family}' not hosted (HTTP ${res.status})`)
    return new Uint8Array(await res.arrayBuffer())
  }
  const cache = await caches.open('kelvin-shards')
  const hit = await cache.match(url)
  if (hit) return new Uint8Array(await hit.arrayBuffer())
  for (const req of await cache.keys()) {            // drop older builds of this family
    if (req.url.includes(`/${family}.kidx?`) && !req.url.endsWith(`b=${buildId}`)) await cache.delete(req)
  }
  const res = await fetch(url)
  if (!res.ok) throw new Error(`Kelvin shard '${family}' not hosted (HTTP ${res.status})`)
  await cache.put(url, res.clone())
  return new Uint8Array(await res.arrayBuffer())
}

export function ensureShard(family) {
  if (!_shards.has(family)) {
    emitShard(family, 'loading')
    _shards.set(family, (async () => {
      const entry = await manifestEntry(family)
      const bytes = await fetchShardBytes(family, entry.buildId)
      const meta = await callJson('load_shard', family, bytes)
      emitShard(family, 'loaded')
      return meta
    })().catch((e) => {
      _shards.delete(family) // a failed load is retryable, not cached
      emitShard(family, 'error')
      throw e
    }))
  }
  return _shards.get(family)
}

export function shardLoaded(family) {
  return _shards.has(family)
}

// ── browse: filter/sort/facet/paginate over the loaded shard (see Browse.hpp) ─
export async function browse(family, query = {}) {
  await ensureShard(family)
  return callJson('browse', family, JSON.stringify(query))
}

// ── select: the deterministic recommender. Returns a SelectionResult or
//    {error:'NoCandidates', rejections, ...} — the caller renders both honestly. ─
export async function select(family, requirements, options = {}) {
  await ensureShard(family)
  return callJson('select', family, JSON.stringify(requirements), JSON.stringify(options))
}

// ── cross-reference: the deterministic substitute ranker (CrossRef.hpp). Pure
//    math over spec blocks the caller built from browse rows — no shard access,
//    so no ensureShard here. ──────────────────────────────────────────────────
export async function crossReference(category, original, candidates, options = {}) {
  const out = await callJson('cross_reference_string', category,
    JSON.stringify(original), JSON.stringify(candidates), JSON.stringify(options))
  if (out?.error) throw new Error(`cross-reference failed: ${out.error}`)
  return out
}

// ── record fetch: ONE part's full datasheet record by byte span (Range, 206) ──
export async function fetchRecord(family, srcOffset, srcLength) {
  if (typeof srcOffset !== 'number' || typeof srcLength !== 'number') {
    throw new Error('record fetch needs a byte span (srcOffset/srcLength)')
  }
  const entry = await manifestEntry(family)
  const res = await fetch(`${KELVIN_BASE}/${family}.ndjson`,
    { headers: { Range: `bytes=${srcOffset}-${srcOffset + srcLength - 1}` } })
  // A 206 is a genuine ranged read. A 200 means the host IGNORED Range and would
  // stream the whole multi-hundred-MB catalog — refuse without reading the body.
  if (res.status !== 206) {
    throw new Error(res.ok
      ? `${family}.ndjson host ignored Range (HTTP ${res.status}) — enable byte-range serving`
      : `record fetch failed (HTTP ${res.status}) — is ${family}.ndjson hosted next to the shard?`)
  }
  // Version guard: hosted NDJSON size must equal the size the shard was indexed
  // against, or these byte offsets could point at the wrong record.
  const total = Number(res.headers.get('Content-Range')?.split('/')[1])
  if (Number.isFinite(total) && total !== entry.sourceSize) {
    throw new Error(`${family}: shard/catalog version mismatch (NDJSON ${total} B vs shard-indexed ${entry.sourceSize} B) — redeploy shards + NDJSON + manifest together`)
  }
  return JSON.parse(await res.text())
}
