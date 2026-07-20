import { createApp } from 'vue'
import App from './App.vue'
// Self-hosted fonts (was Google Fonts CDN — see security assessment N1). Only the
// weights the UI actually uses are bundled; Vite fingerprints + serves them same-origin.
import '@fontsource/michroma/400.css'
import '@fontsource/ibm-plex-sans/400.css'
import '@fontsource/ibm-plex-sans/500.css'
import '@fontsource/ibm-plex-sans/600.css'
import '@fontsource/ibm-plex-mono/400.css'
import '@fontsource/ibm-plex-mono/500.css'
import './style.css'
import { initTelemetry, trackEvent } from './telemetry.js'

// Interaction telemetry (production-only; no-ops on localhost / the dev server).
// Umami website-id for kelvin.openconverters.com, registered in the shared OM
// Umami instance (Settings → Websites). Leave null until registered — Umami then
// no-ops while the same-origin /telemetry pipeline still records every event.
const UMAMI_WEBSITE_ID = 'bb483621-8413-47b6-a212-7d2319dff8f9' // kelvin.openconverters.com (OM Umami)
initTelemetry({ site: 'kelvin', umamiWebsiteId: UMAMI_WEBSITE_ID })
trackEvent('app_open')

createApp(App).mount('#app')
