import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { http, startServer, type TestServer } from './helpers.ts'

let server: TestServer

beforeEach(async () => {
  server = await startServer()
})

afterEach(async () => {
  await server.close()
})

describe('admin API', () => {
  it('requires the X-Admin-Secret header', async () => {
    const url = `${server.adminUrl}/licenses`
    expect(await http(url)).toEqual({ status: 401, body: { error: 'unauthorized' } })
    expect(await http(url, { headers: { 'X-Admin-Secret': 'wrong' } })).toEqual({
      status: 401,
      body: { error: 'unauthorized' },
    })
    expect(await http(url, { body: { name: 'faokun' }, headers: { 'X-Admin-Secret': 'wrong' } })).toEqual({
      status: 401,
      body: { error: 'unauthorized' },
    })
    expect((await server.admin('/licenses')).status).toBe(200)
  })

  it('is not served by the public server', async () => {
    expect((await http(`${server.baseUrl}/licenses`)).status).toBe(404)
  })

  it('creates a user with the first key and adds keys to it afterwards, ignoring case', async () => {
    const first = await server.admin('/licenses', { name: 'FaoKun' })
    expect(first.body).toMatchObject({ user: { id: 1, name: 'FaoKun' }, createdUser: true })
    const second = await server.admin('/licenses', { name: 'faokun' })
    expect(second.body).toMatchObject({ user: { id: 1, name: 'FaoKun' }, createdUser: false })
    expect(second.body.key).not.toBe(first.body.key)

    for (const key of [first.body.key, second.body.key]) {
      expect((await server.login(key)).body.user).toEqual({ id: 1, name: 'FaoKun', rank: 'user' })
    }
  })

  it('rejects usernames outside 3-16 letters, digits and underscores', async () => {
    for (const name of ['ab', 'a'.repeat(17), 'bad name', 'ふぁお', 'semi;colon', '']) {
      expect(await server.admin('/licenses', { name })).toEqual({ status: 400, body: { error: 'bad_request' } })
    }
    expect((await server.admin('/licenses', { name: 'ok_name_123' })).status).toBe(200)
  })

  it('answers user_not_found for unknown users', async () => {
    for (const path of ['/licenses/revoke', '/users/ban', '/users/unban']) {
      expect(await server.admin(path, { name: 'nobody' })).toEqual({ status: 404, body: { error: 'user_not_found' } })
    }
  })

  it('lists licenses with their state and no hashes', async () => {
    const key = await server.createLicense('faokun')
    await server.createLicense('faokun')
    await server.login(key)
    await server.admin('/licenses/revoke', { name: 'faokun' })
    await server.createLicense('alice')

    const result = await server.admin('/licenses')
    expect(result.status).toBe(200)
    expect(result.body.items).toMatchObject([
      { name: 'alice', banned: false, status: 'active', lastUsedAt: null },
      { name: 'faokun', banned: false, status: 'revoked' },
      { name: 'faokun', banned: false, status: 'revoked', lastUsedAt: null },
    ])
    expect(result.body.items[1].lastUsedAt).toEqual(expect.any(Number))
    expect(JSON.stringify(result.body)).not.toMatch(/[0-9a-f]{64}/)
  })
})
