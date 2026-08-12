import { defineConfig } from "vite";
import { viteSingleFile } from "vite-plugin-singlefile";

/**
 * The MCP Apps contract serves a widget as ONE self-contained HTML resource:
 * no external scripts, no CDN, nothing the host has to fetch. Everything is
 * inlined so the sandboxed iframe works under a strict CSP.
 */
export default defineConfig({
  plugins: [viteSingleFile()],
  build: {
    outDir: "dist",
    rollupOptions: { input: "picker.html" },
    assetsInlineLimit: 100_000_000,
    cssCodeSplit: false,
  },
});
