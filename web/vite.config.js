import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { cpSync, existsSync, readdirSync, symlinkSync } from 'node:fs'
import { resolve } from 'node:path'

// public/kelvin/ is the catalogue data set (shards + NDJSON, ~6 GB once its symlinks resolve).
// Vite copies public/ into dist/ verbatim, which on 2026-11-12 filled the disk mid-build; the
// data is never deployed from dist/ anyway (deploy-prod.sh rsyncs with --exclude kelvin/ and
// the box serves /kelvin from /cache/kelvin). So copy everything else, and LINK the data dir
// so `vite preview` (the e2e server) still serves it.
function publicWithoutData(dataDir = 'kelvin') {
  let outDir, publicDir
  return {
    name: 'public-without-data',
    apply: 'build',
    configResolved(c) {
      outDir = resolve(c.root, c.build.outDir)
      publicDir = c.publicDir
    },
    closeBundle() {
      for (const entry of readdirSync(publicDir)) {
        if (entry === dataDir) continue
        cpSync(resolve(publicDir, entry), resolve(outDir, entry), { recursive: true, dereference: true })
      }
      if (existsSync(resolve(publicDir, dataDir)))
        symlinkSync(resolve(publicDir, dataDir), resolve(outDir, dataDir), 'dir')
    },
  }
}

// base './' so the built SPA can be hosted anywhere (nginx subdomain root, file://, previews).
export default defineConfig({
  plugins: [vue(), publicWithoutData()],
  base: './',
  build: { chunkSizeWarningLimit: 1200, copyPublicDir: false },
})
