import { createApp } from 'vue'
import App from './App.vue'
import './style.css'
import { initTelemetry, trackEvent } from './telemetry.js'

// Interaction telemetry (production-only; no-ops on localhost / the dev server).
// Umami website-id for kelvin.openconverters.com, registered in the shared OM
// Umami instance (Settings → Websites). Leave null until registered — Umami then
// no-ops while the same-origin /telemetry pipeline still records every event.
const UMAMI_WEBSITE_ID = null // TODO(deploy): set once the Kelvin Umami website exists
initTelemetry({ site: 'kelvin', umamiWebsiteId: UMAMI_WEBSITE_ID })
trackEvent('app_open')

createApp(App).mount('#app')
