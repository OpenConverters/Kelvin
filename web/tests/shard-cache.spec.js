import { test, expect } from '@playwright/test'

test.setTimeout(180000)

test('a poisoned shard cache repairs itself', async ({ page }) => {
  // Exactly the state a user was left in: the CURRENT url holding the WRONG
  // bytes, stored while the server was briefly serving a different file.
  await page.goto('/')
  const poisoned = await page.evaluate(async () => {
    const m = await (await fetch('/kelvin/manifest.json', { cache: 'no-store' })).json()
    const url = `/kelvin/bjt.kidx?b=${m.families.bjt.buildId}&v=12`
    // stand a DIFFERENT family's shard in as the bjt one
    const wrong = await fetch(`/kelvin/varistor.kidx?b=${m.families.varistor.buildId}&v=12`)
    const c = await caches.open('kelvin-shards')
    await c.put(url, new Response(await wrong.arrayBuffer()))
    return url
  })
  await page.goto('/#/catalog/bjt')
  await page.waitForTimeout(12000)
  const rows = await page.locator('tbody tr').count()
  console.log('POISON url:', poisoned)
  console.log('POISON rows after repair:', rows)
  expect(rows).toBeGreaterThan(10) // recovered, rather than staying broken
})
