import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { deleteExpiredSessions, issueSession, SESSION_TTL_MS } from '../src/auth/session.ts'
import { http, startServer, type Json, type TestServer } from './helpers.ts'

let server: TestServer

beforeEach(async () => {
  server = await startServer()
})

afterEach(async () => {
  await server.close()
})

const newsWith = (token?: string) =>
  http(`${server.baseUrl}/news`, token === undefined ? {} : { headers: { Authorization: `Bearer ${token}` } })

describe('POST /auth/license', () => {
  it('returns a 24 h session for a valid key and stores only hashes', async () => {
    const key = await server.createLicense('faokun')
    const before = Date.now()
    const result = await server.login(key)

    expect(result.status).toBe(200)
    expect(result.body.user).toEqual({ id: 1, name: 'faokun', rank: 'user' })
    expect(result.body.token).toMatch(/^[A-Za-z0-9_-]{43}$/)
    expect(result.body.expiresAt).toBeGreaterThanOrEqual(before + SESSION_TTL_MS)
    expect(result.body.expiresAt).toBeLessThanOrEqual(Date.now() + SESSION_TTL_MS)

    const license: Json = server.ctx.db.prepare('SELECT key_hash, last_used_at FROM licenses').get()
    expect(license.last_used_at).toBeGreaterThanOrEqual(before)
    const stored = JSON.stringify(server.ctx.db.prepare('SELECT * FROM licenses, sessions').all())
    expect(stored).not.toContain(key)
    expect(stored).not.toContain(result.body.token)
  })

  it('accepts a key typed in lower case with spaces', async () => {
    const key = await server.createLicense('faokun')
    const result = await server.login(`  ${key.toLowerCase().replaceAll('-', ' - ')} `)
    expect(result.status).toBe(200)
  })

  it('rejects an unknown key with 401 invalid_key', async () => {
    await server.createLicense('faokun')
    const result = await server.login('MDC-0000-0000-0000-0000')
    expect(result).toEqual({ status: 401, body: { error: 'invalid_key' } })
  })

  it('rejects malformed bodies with 400 bad_request', async () => {
    const url = `${server.baseUrl}/auth/license`
    expect(await http(url, { body: {} })).toEqual({ status: 400, body: { error: 'bad_request' } })
    expect(await http(url, { body: { key: 5 } })).toEqual({ status: 400, body: { error: 'bad_request' } })
    expect(await http(url, { rawBody: '{"key":' })).toEqual({ status: 400, body: { error: 'bad_request' } })
  })

  it('rejects revoked keys with 403 revoked and ends their sessions', async () => {
    const key = await server.createLicense('faokun')
    const { body } = await server.login(key)
    expect((await newsWith(body.token)).status).toBe(200)

    expect((await server.admin('/licenses/revoke', { name: 'faokun' })).status).toBe(200)

    expect(await newsWith(body.token)).toEqual({ status: 401, body: { error: 'unauthorized' } })
    expect(await server.login(key)).toEqual({ status: 403, body: { error: 'revoked' } })
  })

  it('a new key works again after a revoke', async () => {
    await server.createLicense('faokun')
    await server.admin('/licenses/revoke', { name: 'faokun' })
    const key = await server.createLicense('faokun')
    expect((await server.login(key)).status).toBe(200)
  })

  it('rejects banned users with 403 banned until they are unbanned', async () => {
    const key = await server.createLicense('faokun')
    const { body } = await server.login(key)

    expect((await server.admin('/users/ban', { name: 'faokun' })).status).toBe(200)
    expect((await newsWith(body.token)).status).toBe(401)
    expect(await server.login(key)).toEqual({ status: 403, body: { error: 'banned' } })

    expect((await server.admin('/users/unban', { name: 'faokun' })).status).toBe(200)
    expect((await server.login(key)).status).toBe(200)
  })

  it('allows 10 attempts per 10 minutes per client IP (CF-Connecting-IP)', async () => {
    for (let i = 0; i < 10; i++) expect((await server.login('MDC-0000-0000-0000-0000', '203.0.113.7')).status).toBe(401)
    expect(await server.login('MDC-0000-0000-0000-0000', '203.0.113.7')).toEqual({
      status: 429,
      body: { error: 'rate_limited' },
    })
    // Another client behind the same tunnel is not affected.
    expect((await server.login('MDC-0000-0000-0000-0000', '203.0.113.8')).status).toBe(401)

    server.clock.offset = 10 * 60 * 1000
    expect((await server.login('MDC-0000-0000-0000-0000', '203.0.113.7')).status).toBe(401)
  })
})

describe('REST authentication', () => {
  it('requires a valid, unexpired bearer token', async () => {
    const token = await server.signUp('faokun')
    expect(await newsWith()).toEqual({ status: 401, body: { error: 'unauthorized' } })
    expect((await newsWith('not-a-token')).status).toBe(401)
    expect((await newsWith(token)).status).toBe(200)

    server.clock.offset = SESSION_TTL_MS
    expect(await newsWith(token)).toEqual({ status: 401, body: { error: 'unauthorized' } })
  })

  it('answers unknown routes in the error shape', async () => {
    expect(await http(`${server.baseUrl}/nope`)).toEqual({ status: 404, body: { error: 'bad_request' } })
  })
})

describe('deleteExpiredSessions', () => {
  it('removes only expired sessions', async () => {
    await server.createLicense('faokun')
    const { db } = server.ctx
    issueSession(db, 1, 0)
    issueSession(db, 1, 1000)
    expect(deleteExpiredSessions(db, SESSION_TTL_MS + 500)).toBe(1)
    expect(db.prepare('SELECT COUNT(*) AS n FROM sessions').get()).toEqual({ n: 1 })
  })
})
