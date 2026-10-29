import { afterEach, expect, it } from 'vitest'
import { connect, startServer, type TestServer } from './helpers.ts'

let server: TestServer
afterEach(async () => { await server?.close() })

it('shares the chosen model only with watching clients and restores vanilla on OFF/disconnect', async () => {
  server = await startServer()
  const alice = await connect(server.wsUrl, await server.signUp('alice'))
  const bob = await connect(server.wsUrl, await server.signUp('bob'))
  await alice.next(); await bob.next()
  const player = { actorId: '-9223372036854775807', name: 'MC Alice' }
  bob.send({ t: 'model.sync', self: null, watching: [player] })
  expect(await bob.next()).toEqual({ t: 'model.peers', peers: [] })
  alice.send({ t: 'model.sync', self: { ...player, model: 'chocolat' }, watching: [] })
  expect(await bob.next()).toEqual({ t: 'model.peers', peers: [{ ...player, model: 'chocolat', userId: 1 }] })
  expect(await alice.next()).toEqual({ t: 'model.peers', peers: [] })
  // All syncs use the same authenticated connection; updates cannot supply userId.
  await new Promise((resolve) => setTimeout(resolve, 510))
  alice.send({ t: 'model.sync', self: { ...player, model: 'neon', userId: 99 }, watching: [] })
  expect(await bob.next()).toEqual({ t: 'model.peers', peers: [{ ...player, model: 'neon', userId: 1 }] })
  await new Promise((resolve) => setTimeout(resolve, 510))
  alice.send({ t: 'model.sync', self: null, watching: [] })
  expect(await bob.next()).toEqual({ t: 'model.peers', peers: [] })
  await new Promise((resolve) => setTimeout(resolve, 510))
  alice.send({ t: 'model.sync', self: { ...player, model: 'neon' }, watching: [] })
  await bob.next()
  alice.ws.close()
  expect(await bob.next()).toEqual({ t: 'model.peers', peers: [] })
})

it('matches both server actor id and name, rejects unknown models and hides duplicate claims', async () => {
  server = await startServer()
  const a = await connect(server.wsUrl, await server.signUp('alice'))
  const b = await connect(server.wsUrl, await server.signUp('bobby'))
  const c = await connect(server.wsUrl, await server.signUp('carol'))
  await a.next(); await b.next(); await c.next()
  const player = { actorId: '42', name: 'Alex' }
  b.send({ t: 'model.sync', self: null, watching: [player, { actorId: '43', name: 'Alex' }] })
  await b.next()
  a.send({ t: 'model.sync', id: 'bad', self: { ...player, model: '../evil.fbx' }, watching: [] })
  expect(await a.next()).toMatchObject({ t: 'ack', id: 'bad', ok: false })
  a.send({ t: 'model.sync', self: { ...player, model: 'neon' }, watching: [] })
  expect(await b.next()).toEqual({ t: 'model.peers', peers: [{ ...player, model: 'neon', userId: 1 }] })
  await a.next()
  c.send({ t: 'model.sync', self: { ...player, model: 'chocolat' }, watching: [] })
  expect(await b.next()).toEqual({ t: 'model.peers', peers: [] })
  expect(await c.next()).toEqual({ t: 'model.peers', peers: [] })
  c.ws.close()
  expect(await b.next()).toEqual({ t: 'model.peers', peers: [{ ...player, model: 'neon', userId: 1 }] })
})
