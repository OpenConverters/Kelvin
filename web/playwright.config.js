import { defineConfig } from '@playwright/test'

// e2e runs against the PRODUCTION build (vite preview serves dist/ with Range
// support). Prereqs: `npm run build` and `scripts/build-kelvin-shards.sh`.
// Always headless (house rule) — debug via screenshots, never --headed.
export default defineConfig({
  testDir: 'tests',
  timeout: 60_000,
  use: {
    baseURL: 'http://localhost:4174',
    headless: true,
    viewport: { width: 1600, height: 950 },
  },
  webServer: {
    command: 'npx vite preview --port 4174 --strictPort',
    url: 'http://localhost:4174',
    reuseExistingServer: true,
    timeout: 30_000,
  },
})
