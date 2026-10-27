import { afterEach, describe, expect, it } from 'vitest'
import { connect, http, startServer, type TestServer, type TestSocket } from './helpers.ts'

let server: TestServer

afterEach(async () => {
  await server.close()
})

interface Member {
  id: number
  token: string
  socket: TestSocket
}

async function join(name: string): Promise<Member> {
  const token = await server.signUp(name)
  const socket = await connect(server.wsUrl, token)
  const hello = await socket.next((frame) => frame.t === 'hello')
  return { id: hello.user.id, token, socket }
}

const get = (token: string, path: string) => http(`${server.baseUrl}${path}`, { headers: { Authorization: `Bearer ${token}` } })
const ack = (id: string) => (frame: { t: string; id?: string }) => frame.t === 'ack' && frame.id === id

async function befriend(a: Member, b: Member, bName: string): Promise<void> {
  a.socket.send({ t: 'friend.request', id: 'r', name: bName })
  await a.socket.next(ack('r'))
  b.socket.send({ t: 'friend.respond', id: 'a', userId: a.id, accept: true })
  await b.socket.next(ack('a'))
  await a.socket.next((frame) => frame.t === 'friend.update' && frame.status === 'accepted')
}

describe('Global chat', () => {
  it('stores a message, acks it and sends global.new to everyone, the sender included', async () => {
    server = await startServer()
    const alice = await join('alice')
    const bob = await join('bob')
    alice.socket.send({ t: 'global.send', id: 'g1', body: '  こんにちは  ' })
    const done = await alice.socket.next(ack('g1'))
    expect(done).toMatchObject({ ok: true, message: { author: { id: alice.id, name: 'alice' }, body: 'こんにちは' } })
    expect(await alice.socket.next((frame) => frame.t === 'global.new')).toMatchObject({ message: { id: done.message.id } })
    expect(await bob.socket.next((frame) => frame.t === 'global.new')).toMatchObject({ message: { body: 'こんにちは' } })
  })

  it('rejects empty and over-long bodies, counting code points', async () => {
    server = await startServer()
    const alice = await join('alice')
    alice.socket.send({ t: 'global.send', id: 'e', body: '   ' })
    expect(await alice.socket.next(ack('e'))).toMatchObject({ ok: false, error: 'empty' })
    alice.socket.send({ t: 'global.send', id: 'l', body: '😀'.repeat(301) })
    expect(await alice.socket.next(ack('l'))).toMatchObject({ ok: false, error: 'too_long' })
    alice.socket.send({ t: 'global.send', id: 'ok', body: '😀'.repeat(300) })
    expect(await alice.socket.next(ack('ok'))).toMatchObject({ ok: true })
  })

  it('allows one message per user every 0.3 seconds', async () => {
    server = await startServer()
    const alice = await join('alice')
    alice.socket.send({ t: 'global.send', id: '1', body: 'a' })
    expect(await alice.socket.next(ack('1'))).toMatchObject({ ok: true })
    alice.socket.send({ t: 'global.send', id: '2', body: 'b' })
    expect(await alice.socket.next(ack('2'))).toMatchObject({ ok: false, error: 'rate_limited' })
    server.clock.offset += 301
    alice.socket.send({ t: 'global.send', id: '3', body: 'c' })
    expect(await alice.socket.next(ack('3'))).toMatchObject({ ok: true })
  })

  it('returns history newest first with a before cursor', async () => {
    server = await startServer()
    const alice = await join('alice')
    for (let i = 1; i <= 5; ++i) {
      alice.socket.send({ t: 'global.send', id: `m${i}`, body: `message ${i}` })
      await alice.socket.next(ack(`m${i}`))
      server.clock.offset += 301
    }
    const page = await get(alice.token, '/global/messages?limit=3')
    expect(page.body.items.map((m: { body: string }) => m.body)).toEqual(['message 5', 'message 4', 'message 3'])
    const older = await get(alice.token, `/global/messages?limit=3&before=${page.body.items[2].id}`)
    expect(older.body.items.map((m: { body: string }) => m.body)).toEqual(['message 2', 'message 1'])
    expect((await http(`${server.baseUrl}/global/messages`)).status).toBe(401)
  })
})

describe('Friends', () => {
  it('request, notify, accept, and presence both ways', async () => {
    server = await startServer()
    const alice = await join('alice')
    const bob = await join('bob')
    alice.socket.send({ t: 'friend.request', id: 'r', name: 'BOB' })
    expect(await alice.socket.next(ack('r'))).toMatchObject({ ok: true })
    expect(await bob.socket.next((f) => f.t === 'friend.request')).toEqual({ t: 'friend.request', from: { id: alice.id, name: 'alice' } })
    expect((await get(alice.token, '/friends')).body).toEqual({ friends: [], incoming: [], outgoing: [{ id: bob.id, name: 'bob' }] })
    expect((await get(bob.token, '/friends')).body.incoming).toEqual([{ id: alice.id, name: 'alice' }])

    bob.socket.send({ t: 'friend.respond', id: 'a', userId: alice.id, accept: true })
    expect(await bob.socket.next(ack('a'))).toMatchObject({ ok: true })
    expect(await alice.socket.next((f) => f.t === 'friend.update')).toEqual({
      t: 'friend.update',
      user: { id: bob.id, name: 'bob' },
      status: 'accepted',
    })
    expect(await alice.socket.next((f) => f.t === 'presence')).toEqual({ t: 'presence', userId: bob.id, online: true })
    expect(await bob.socket.next((f) => f.t === 'presence')).toEqual({ t: 'presence', userId: alice.id, online: true })
    const list = (await get(alice.token, '/friends')).body
    expect(list.friends).toEqual([{ id: bob.id, name: 'bob', online: true, unread: 0, theirReadUpTo: 0 }])

    bob.socket.ws.close()
    expect(await alice.socket.next((f) => f.t === 'presence')).toEqual({ t: 'presence', userId: bob.id, online: false })
  })

  it('makes mutual requests friends at once', async () => {
    server = await startServer()
    const alice = await join('alice')
    const bob = await join('bob')
    alice.socket.send({ t: 'friend.request', id: 'r1', name: 'bob' })
    await alice.socket.next(ack('r1'))
    bob.socket.send({ t: 'friend.request', id: 'r2', name: 'alice' })
    expect(await bob.socket.next(ack('r2'))).toMatchObject({ ok: true })
    expect(await bob.socket.next((f) => f.t === 'friend.update')).toMatchObject({ status: 'accepted', user: { id: alice.id } })
    expect(await alice.socket.next((f) => f.t === 'friend.update')).toMatchObject({ status: 'accepted', user: { id: bob.id } })
  })

  it('declines, cancels and removes', async () => {
    server = await startServer()
    const alice = await join('alice')
    const bob = await join('bob')
    alice.socket.send({ t: 'friend.request', id: 'r', name: 'bob' })
    await alice.socket.next(ack('r'))
    bob.socket.send({ t: 'friend.respond', id: 'd', userId: alice.id, accept: false })
    await bob.socket.next(ack('d'))
    expect(await alice.socket.next((f) => f.t === 'friend.update')).toMatchObject({ status: 'declined' })

    alice.socket.send({ t: 'friend.request', id: 'r2', name: 'bob' })
    await alice.socket.next(ack('r2'))
    alice.socket.send({ t: 'friend.remove', id: 'c', userId: bob.id })
    expect(await alice.socket.next(ack('c'))).toMatchObject({ ok: true })
    expect(await bob.socket.next((f) => f.t === 'friend.update')).toMatchObject({ status: 'cancelled' })

    await befriend(alice, bob, 'bob')
    alice.socket.send({ t: 'friend.remove', id: 'x', userId: bob.id })
    await alice.socket.next(ack('x'))
    expect(await bob.socket.next((f) => f.t === 'friend.update' && f.status === 'removed')).toMatchObject({ user: { id: alice.id } })
    expect((await get(alice.token, '/friends')).body.friends).toEqual([])
  })

  it('reports the request errors', async () => {
    server = await startServer()
    const alice = await join('alice')
    const bob = await join('bob')
    const tryRequest = async (name: string) => {
      alice.socket.send({ t: 'friend.request', id: name, name })
      return (await alice.socket.next(ack(name))).error
    }
    expect(await tryRequest('alice')).toBe('self')
    expect(await tryRequest('nobody')).toBe('user_not_found')
    expect(await tryRequest('bob')).toBeUndefined()
    alice.socket.send({ t: 'friend.request', id: 'again', name: 'bob' })
    expect((await alice.socket.next(ack('again'))).error).toBe('request_exists')
    bob.socket.send({ t: 'friend.respond', id: 'a', userId: alice.id, accept: true })
    await bob.socket.next(ack('a'))
    alice.socket.send({ t: 'friend.request', id: 'third', name: 'bob' })
    expect((await alice.socket.next(ack('third'))).error).toBe('already_friends')
  })

  it('searches by prefix with the relation, never listing me', async () => {
    server = await startServer()
    const alice = await join('alice')
    await join('alfred')
    await join('bob')
    alice.socket.send({ t: 'friend.request', id: 'r', name: 'alfred' })
    await alice.socket.next(ack('r'))
    const found = (await get(alice.token, '/users/search?q=AL')).body.items
    expect(found).toEqual([{ id: expect.any(Number), name: 'alfred', relation: 'outgoing' }])
    expect((await get(alice.token, '/users/search?q=_')).body.items).toEqual([])
  })
})

describe('DM', () => {
  it('refuses DMs and history between non-friends', async () => {
    server = await startServer()
    const alice = await join('alice')
    const bob = await join('bob')
    alice.socket.send({ t: 'dm.send', id: 'd', to: bob.id, body: 'hi' })
    expect(await alice.socket.next(ack('d'))).toMatchObject({ ok: false, error: 'not_friends' })
    expect((await get(alice.token, `/dm/${bob.id}/messages`)).status).toBe(403)
  })

  it('sends, counts unread, and reports reads', async () => {
    server = await startServer()
    const alice = await join('alice')
    const bob = await join('bob')
    await befriend(alice, bob, 'bob')
    alice.socket.send({ t: 'dm.send', id: 'd1', to: bob.id, body: 'hi bob' })
    const sent = await alice.socket.next(ack('d1'))
    expect(sent).toMatchObject({ ok: true, message: { to: bob.id, body: 'hi bob', author: { id: alice.id } } })
    expect(await bob.socket.next((f) => f.t === 'dm.new')).toMatchObject({ message: { id: sent.message.id } })
    expect((await get(bob.token, '/friends')).body.friends[0]).toMatchObject({ id: alice.id, unread: 1 })

    bob.socket.send({ t: 'dm.read', with: alice.id, upTo: sent.message.id })
    expect(await alice.socket.next((f) => f.t === 'dm.read')).toEqual({ t: 'dm.read', by: bob.id, upTo: sent.message.id })
    expect((await get(bob.token, '/friends')).body.friends[0].unread).toBe(0)
    expect((await get(alice.token, '/friends')).body.friends[0].theirReadUpTo).toBe(sent.message.id)
    // An older mark never moves it back.
    bob.socket.send({ t: 'dm.read', with: alice.id, upTo: 0 })
    expect(await alice.socket.next((f) => f.t === 'dm.read')).toMatchObject({ upTo: sent.message.id })

    bob.socket.send({ t: 'dm.send', id: 'd2', to: alice.id, body: 'hi alice' })
    await bob.socket.next(ack('d2'))
    const history = (await get(alice.token, `/dm/${bob.id}/messages`)).body.items
    expect(history.map((m: { body: string }) => m.body)).toEqual(['hi alice', 'hi bob'])
  })

  it('allows 5 DMs in 5 seconds', async () => {
    server = await startServer()
    const alice = await join('alice')
    const bob = await join('bob')
    await befriend(alice, bob, 'bob')
    for (let i = 0; i < 5; ++i) {
      alice.socket.send({ t: 'dm.send', id: `d${i}`, to: bob.id, body: `${i}` })
      expect(await alice.socket.next(ack(`d${i}`))).toMatchObject({ ok: true })
    }
    alice.socket.send({ t: 'dm.send', id: 'd5', to: bob.id, body: 'x' })
    expect(await alice.socket.next(ack('d5'))).toMatchObject({ ok: false, error: 'rate_limited' })
  })
})
