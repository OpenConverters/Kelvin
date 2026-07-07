// Kelvin site e2e: engine boots, catalogue browses a real shard, the drawer
// Range-fetches a record, the recommender ranks deterministically, compare pins.
import { test, expect } from '@playwright/test'

test.describe('kelvin site', () => {
  test('boots the engine and browses the magnetic catalogue', async ({ page }) => {
    const errors = []
    page.on('pageerror', (e) => errors.push(String(e)))
    page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()) })

    await page.goto('/#/catalog/magnetic')
    await expect(page.locator('.led.ok')).toBeVisible({ timeout: 20_000 })
    await expect(page.locator('tbody tr').first()).toBeVisible({ timeout: 20_000 })
    // family cards show manifest counts
    const magCount = await page.locator('.fam-card.active .fam-count').textContent()
    expect(Number(magCount.replace(/,/g, ''))).toBeGreaterThan(1000)
    expect(errors).toEqual([])
  })

  test('numeric filter narrows the result set', async ({ page }) => {
    await page.goto('/#/catalog/magnetic')
    await expect(page.locator('tbody tr').first()).toBeVisible({ timeout: 20_000 })
    const before = await page.locator('.results-head .mono').first().textContent()
    const minL = page.locator('.num-filter').first().locator('input').first()
    await minL.fill('100u')
    await minL.press('Enter')
    await expect(async () => {
      const after = await page.locator('.results-head .mono').first().textContent()
      expect(after).not.toBe(before)
    }).toPass({ timeout: 10_000 })
    await expect(page.locator('.chips .chip.on')).toHaveCount(1)
  })

  test('part drawer Range-fetches the full record', async ({ page }) => {
    await page.goto('/#/catalog/mosfet')
    await expect(page.locator('tbody tr .mpn').first()).toBeVisible({ timeout: 20_000 })
    await page.locator('tbody tr').first().click()
    const drawer = page.getByTestId('part-drawer')
    await expect(drawer).toBeVisible()
    await expect(drawer.locator('.spec-table').first()).toBeVisible({ timeout: 15_000 })
    await expect(drawer.locator('.err')).toHaveCount(0)
    await page.keyboard.press('Escape')
    await expect(drawer).not.toBeVisible()
  })

  test('recommender ranks mosfets deterministically', async ({ page }) => {
    await page.goto('/#/recommend/mosfet')
    const inputs = page.locator('.req input:not([type=checkbox])')
    await inputs.nth(0).fill('60')
    await inputs.nth(1).fill('5')
    await inputs.nth(2).fill('100m')
    await page.getByRole('button', { name: 'Find parts' }).click()
    await expect(page.locator('.cand').first()).toBeVisible({ timeout: 30_000 })
    // deterministic engine: same question, same answer
    await expect(page.locator('.cand').first().locator('.mpn-link')).toHaveText('CSD18536KCS')
    await expect(page.locator('.cand').first().locator('.meter.pass').first()).toBeVisible()
  })

  test('impossible spec shows the rejection histogram', async ({ page }) => {
    await page.goto('/#/recommend/mosfet')
    const inputs = page.locator('.req input:not([type=checkbox])')
    await inputs.nth(0).fill('10k')
    await inputs.nth(1).fill('500')
    await inputs.nth(2).fill('1m')
    await page.getByRole('button', { name: 'Find parts' }).click()
    await expect(page.locator('.rejections')).toBeVisible({ timeout: 30_000 })
    await expect(page.locator('.rej-row').first()).toBeVisible()
  })

  test('browse-only families: timing catalogue browses, recommender hides them', async ({ page }) => {
    await page.goto('/#/catalog/timing')
    await expect(page.locator('tbody tr .mpn').first()).toBeVisible({ timeout: 20_000 })
    // technology facet from the live shard
    await expect(page.locator('.facet label', { hasText: 'quartzCrystal' })).toBeVisible()
    // the recommender strip offers no browse-only family
    await page.goto('/#/recommend/mosfet')
    await page.reload()
    await expect(page.getByRole('button', { name: 'Find parts' })).toBeVisible({ timeout: 20_000 })
    await expect(page.locator('.fam-tab', { hasText: 'Timing' })).toHaveCount(0)
    await expect(page.locator('.fam-tab', { hasText: 'Analog' })).toHaveCount(0)
  })

  test('pins overlay curves in compare', async ({ page }) => {
    await page.goto('/#/catalog/magnetic')
    await expect(page.locator('tbody tr .mpn').first()).toBeVisible({ timeout: 20_000 })
    const firstBefore = await page.locator('tbody tr .mpn').first().textContent()
    // chip beads carry impedance curves
    await page.locator('.facet label', { hasText: 'chipBead' }).locator('input').check()
    await expect(page.locator('.chips .chip.on')).toHaveCount(1)
    // wait for the FILTERED rows (pinning the stale pre-filter rows would compare inductors)
    await expect(page.locator('tbody tr .mpn').first()).not.toHaveText(firstBefore, { timeout: 10_000 })
    await page.locator('tbody tr .pin-btn').nth(0).click()
    await page.locator('tbody tr .pin-btn').nth(1).click()
    await page.getByRole('button', { name: /overlay curves/ }).click()
    await expect(page.locator('.overlay-grid .curve').first()).toBeVisible({ timeout: 20_000 })
    await expect(page.locator('.legend-item')).toHaveCount(2)
  })
})
