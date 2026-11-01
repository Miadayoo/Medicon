import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { startServer, type TestServer } from './helpers.ts'

let server: TestServer

beforeEach(async () => {
  server = await startServer()
})

afterEach(async () => {
  await server.close()
})

describe('GET /docs', () => {
  it('serves the page without sign-in, with or without a trailing slash', async () => {
    for (const path of ['/docs', '/docs/']) {
      const response = await fetch(`${server.baseUrl}${path}`)
      expect(response.status).toBe(200)
      expect(response.headers.get('content-type')).toBe('text/html; charset=utf-8')
      expect(response.headers.get('content-security-policy')).toContain("default-src 'none'")
      expect(await response.text()).toContain('<title>Medicon Script SDK</title>')
    }
  })

  it('links every table-of-contents entry to a section on the page', async () => {
    const html = await (await fetch(`${server.baseUrl}/docs`)).text()
    const targets = [...html.matchAll(/href="#([^"]+)"/g)].map((match) => match[1])
    expect(targets.length).toBeGreaterThan(0)
    for (const target of targets) expect(html).toContain(`id="${target}"`)
  })
})
