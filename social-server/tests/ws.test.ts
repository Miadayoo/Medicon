import { afterEach, describe, expect, it } from 'vitest'
import { SESSION_TTL_MS } from '../src/auth/session.ts'
import { connect, startServer, withTimeout, type TestServer } from './helpers.ts'

let server: TestServer

afterEach(async () => {
  await server.close()
})

describe('WebSocket /ws', () => {
  it('rejects upgrades without a valid token with HTTP 401', async () => {
    server = await startServer()
    await expect(connect(server.wsUrl, null)).rejects.toThrow('401')
    await expect(connect(server.wsUrl, 'not-a-token')).rejects.toThrow('401')
  })

  it('rejects upgrades on other paths', async () => {
    server = await startServer()
    const token = await server.signUp('faokun')
    await expect(connect(`${server.baseUrl.replace('http', 'ws')}/other`, token)).rejects.toThrow('404')
  })

  it('sends hello first and answers ping with pong', async () => {
    server = await startServer()
    const token = await server.signUp('faokun')
    const before = Date.now()
    const socket = await connect(server.wsUrl, token)

    const hello = await socket.next()
    expect(hello).toMatchObject({ t: 'hello', user: { id: 1, name: 'faokun' } })
    expect(hello.serverTime).toBeGreaterThanOrEqual(before)

    socket.send({ t: 'ping' })
    expect(await socket.next()).toEqual({ t: 'pong' })
  })

  it('acks malformed requests that carry an id and ignores the rest', async () => {
    server = await startServer()
    const socket = await connect(server.wsUrl, await server.signUp('faokun'))
    await socket.next((frame) => frame.t === 'hello')

    socket.send({ t: 'global.send', id: 'n1', body: 42 })
    expect(await socket.next()).toEqual({ t: 'ack', id: 'n1', ok: false, error: 'bad_request' })

    socket.send({ t: 'unknown' })
    socket.send({ t: 'unknown', id: 42 })
    socket.send('not json')
    socket.send({ t: 'ping' })
    // Nothing arrives before the pong.
    expect(await socket.next()).toEqual({ t: 'pong' })
  })

  it('replaces an older connection of the same user with close 4009', async () => {
    server = await startServer()
    const token = await server.signUp('faokun')
    const first = await connect(server.wsUrl, token)
    const second = await connect(server.wsUrl, token)

    expect(await withTimeout(first.closed)).toMatchObject({ code: 4009 })
    expect(server.ctx.hub.size).toBe(1)
    await second.next((frame) => frame.t === 'hello')
    second.send({ t: 'ping' })
    expect(await second.next()).toEqual({ t: 'pong' })
  })

  it('kicks a revoked user with kicked + close 4003', async () => {
    server = await startServer()
    const socket = await connect(server.wsUrl, await server.signUp('faokun'))
    await socket.next((frame) => frame.t === 'hello')

    const result = await server.admin('/licenses/revoke', { name: 'faokun' })
    expect(result.body).toMatchObject({ revokedKeys: 1, kicked: true })
    expect(await socket.next()).toEqual({ t: 'kicked', reason: 'revoked' })
    expect(await withTimeout(socket.closed)).toMatchObject({ code: 4003, reason: 'revoked' })
    expect(server.ctx.hub.isOnline(1)).toBe(false)
  })

  it('kicks a banned user with reason banned', async () => {
    server = await startServer()
    const socket = await connect(server.wsUrl, await server.signUp('faokun'))
    await socket.next((frame) => frame.t === 'hello')

    expect((await server.admin('/users/ban', { name: 'faokun' })).body).toMatchObject({ kicked: true })
    expect(await socket.next()).toEqual({ t: 'kicked', reason: 'banned' })
    expect(await withTimeout(socket.closed)).toMatchObject({ code: 4003 })
  })

  it('broadcasts news.new to every connection', async () => {
    server = await startServer()
    const sockets = [
      await connect(server.wsUrl, await server.signUp('alice')),
      await connect(server.wsUrl, await server.signUp('bob')),
    ]
    const posted = await server.admin('/news', { kind: 'news', title: 'メンテナンス', body: '明日の夜です' })
    expect(posted.status).toBe(200)
    for (const socket of sockets) {
      expect(await socket.next((frame) => frame.t === 'news.new')).toEqual({ t: 'news.new', item: posted.body.item })
    }
  })

  it('drops connections that stay silent for the idle timeout', async () => {
    server = await startServer({ idleTimeoutMs: 300 })
    const silent = await connect(server.wsUrl, await server.signUp('alice'))
    const active = await connect(server.wsUrl, await server.signUp('bob'))
    const pinger = setInterval(() => active.send({ t: 'ping' }), 100)
    try {
      expect(await withTimeout(silent.closed)).toMatchObject({ code: 1006 })
      expect(active.ws.readyState).toBe(active.ws.OPEN)
    } finally {
      clearInterval(pinger)
    }
  })

  it('closes with 4001 when the session expires', async () => {
    server = await startServer()
    const token = await server.signUp('faokun')
    server.clock.offset = SESSION_TTL_MS - 200
    const socket = await connect(server.wsUrl, token)
    expect(await withTimeout(socket.closed)).toMatchObject({ code: 4001 })
  })

  it('closes every connection with 1001 on shutdown', async () => {
    server = await startServer()
    const socket = await connect(server.wsUrl, await server.signUp('faokun'))
    await server.ctx.hub.closeAll(1001, 'server_shutdown')
    expect(await withTimeout(socket.closed)).toMatchObject({ code: 1001 })
  })
})
