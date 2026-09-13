#!/usr/bin/env node
// Build-time catalogue summary for the Kelvin home page.
//
// The home page must never download a shard (12–60 MB) or an NDJSON catalogue
// (hundreds of MB) just to say how many parts exist. This script runs the SAME
// WASM engine the SPA ships (public/kelvin.js) over the SAME shards the SPA
// serves (public/kelvin/*.kidx + manifest.json) and writes a few-KB JSON the
// home view imports into the bundle. Every figure is a browse() query result —
// the identical queries StatsView runs live — so the two can never disagree
// about the data they describe.
//
//   node scripts/build-home-summary.mjs            # regenerate if the shards changed
//   node scripts/build-home-summary.mjs --force    # regenerate unconditionally
//
// Fails loudly (exit 1) when the manifest, a shard or the engine is missing: a
// home page with invented or stale-by-accident numbers is worse than no build.

import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { dirname, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { FAMILIES } from '../src/families.js'

const WEB = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const DATA = resolve(WEB, 'public/kelvin')
const ENGINE = resolve(WEB, 'public/kelvin.js')
const OUT = resolve(WEB, 'src/generated/catalog-summary.json')
const TOP_MANUFACTURERS = 5
const TOP_MIX = 4
const FORMAT = 1

function die(msg) {
  console.error(`build-home-summary: ${msg}`)
  process.exit(1)
}

const manifestPath = resolve(DATA, 'manifest.json')
if (!existsSync(manifestPath)) {
  die(`${manifestPath} not found — run scripts/build-kelvin-shards.sh first`)
}
if (!existsSync(ENGINE)) die(`${ENGINE} not found — run \`npm run sync-wasm\` first`)
const manifest = JSON.parse(readFileSync(manifestPath, 'utf8'))
if (!manifest?.families) die('manifest.json has no "families" object')

const buildIds = Object.fromEntries(
  Object.entries(manifest.families).map(([k, e]) => [k, String(e.buildId)]))

// Up-to-date check: same shard builds (content-hash buildIds) and same summary
// format ⇒ the existing file already describes exactly these shards.
if (!process.argv.includes('--force') && existsSync(OUT)) {
  try {
    const prev = JSON.parse(readFileSync(OUT, 'utf8'))
    if (prev.format === FORMAT && JSON.stringify(prev.buildIds) === JSON.stringify(buildIds)) {
      console.log('build-home-summary: shards unchanged — summary is current')
      process.exit(0)
    }
  } catch {
    // unreadable previous output: regenerate below
  }
}

const M = await (await import(pathToFileURL(ENGINE).href)).default()

function call(fn, ...args) {
  const out = M[fn](...args)
  if (typeof out === 'string' && out.startsWith('Exception: ')) {
    throw new Error(`${fn}(${args[0]}): ${out.slice('Exception: '.length)}`)
  }
  return JSON.parse(out)
}

const families = []
const allManufacturers = new Set()

for (const fam of FAMILIES) {
  const entry = manifest.families[fam.key]
  if (!entry) die(`manifest.json has no entry for family '${fam.key}' (families.js lists it)`)
  const shardPath = resolve(DATA, entry.shard)
  if (!existsSync(shardPath)) die(`shard ${shardPath} missing`)

  const t0 = Date.now()
  const meta = call('load_shard', fam.key, new Uint8Array(readFileSync(shardPath)))
  if (String(meta.buildId) !== String(entry.buildId)) {
    die(`${fam.key}: shard is build ${meta.buildId} but manifest says ${entry.buildId}`)
  }

  const base = call('browse', fam.key, JSON.stringify({ withFacets: true, facetTop: 1000000, limit: 0 }))
  const mfr = base.facets?.manufacturer
  if (!mfr) die(`${fam.key}: browse returned no manufacturer facet`)
  if (mfr.omitted) die(`${fam.key}: manufacturer facet truncated (${mfr.omitted} omitted) — raise facetTop`)
  for (const [name] of mfr.values) if (name) allManufacturers.add(name)

  // production share: only families whose shard carries the flag (Browse throws
  // InvalidOptions for an unknown filter field — that is the "no flag" signal).
  let production = null
  try {
    production = call('browse', fam.key, JSON.stringify({ filters: { is_production: true }, limit: 0 })).total
  } catch (e) {
    if (!/is_production/.test(e.message)) throw e
  }

  // one categorical mix per family: its first catalogue facet that is not a
  // thousands-of-values series list
  const mixFacet = fam.facets.find((f) => f.f !== 'series' && f.label !== 'Series')
  let mix = null
  if (mixFacet) {
    const f = base.facets?.[mixFacet.f]
    if (f && f.values.length) {
      mix = {
        field: mixFacet.f,
        label: mixFacet.label,
        distinct: f.values.length + (f.omitted || 0),
        top: f.values.slice(0, TOP_MIX).map(([value, count]) => ({ value, count })),
      }
    }
  }

  families.push({
    key: fam.key,
    parts: base.total,
    sourceLines: entry.sourceLines,
    unreadableRows: entry.unreadableRows,
    manufacturers: mfr.values.length,
    topManufacturers: mfr.values.slice(0, TOP_MANUFACTURERS).map(([name, count]) => ({ name, count })),
    production,
    mix,
  })
  console.log(`build-home-summary: ${fam.key.padEnd(10)} ${String(base.total).padStart(7)} parts, `
    + `${mfr.values.length} manufacturers (${Date.now() - t0} ms)`)
}

const summary = {
  format: FORMAT,
  // provenance of every figure below: these exact shard builds
  buildIds,
  totals: {
    parts: families.reduce((a, f) => a + f.parts, 0),
    families: families.length,
    manufacturerNames: allManufacturers.size,
  },
  families,
}

mkdirSync(dirname(OUT), { recursive: true })
writeFileSync(OUT, JSON.stringify(summary, null, 1) + '\n')
console.log(`build-home-summary: wrote ${OUT}`)
