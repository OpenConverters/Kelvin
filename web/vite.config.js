import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// base './' so the built SPA can be hosted anywhere (nginx subdomain root, file://, previews).
export default defineConfig({
  plugins: [vue()],
  base: './',
  build: { chunkSizeWarningLimit: 1200 },
})
