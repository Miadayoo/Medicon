import { mkdtempSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import Database from 'better-sqlite3'
import { afterEach, describe, expect, it } from 'vitest'
import { openDb } from '../src/db/db.ts'
import { connect, http, startServer, type TestServer } from './helpers.ts'

let server: TestServer | null = null

afterEach(async () => {
  await server?.close()
  server = null
})

const ack = (id: string) => (frame: { t: string; id?: string }) => frame.t === 'ack' && frame.id === id

describe('Ranks', () => {
  it('reports the rank in the login, the hello and message authors', async () => {
    server = await startServer()
    const key = await server.createLicense('boss')
    expect((await server.admin('/users/rank', { name: 'boss', rank: 'owner' })).status).toBe(200)
    const login = await server.login(key)
    expect(login.body.user).toEqual({ id: 1, name: 'boss', rank: 'owner' })
    const socket = await connect(server.wsUrl, login.body.token)
    expect(await socket.next((f) => f.t === 'hello')).toMatchObject({ user: { name: 'boss', rank: 'owner' } })
    socket.send({ t: 'global.send', id: 'g', body: 'hi' })
    expect(await socket.next(ack('g'))).toMatchObject({ message: { author: { name: 'boss', rank: 'owner' } } })
    const history = await http(`${server.baseUrl}/global/messages`, { headers: { Authorization: `Bearer ${login.body.token}` } })
    expect(history.body.items[0].author).toEqual({ id: 1, name: 'boss', rank: 'owner' })
  })

  it('makes a connected user sign in again when the rank changes', async () => {
    server = await startServer()
    const socket = await connect(server.wsUrl, await server.signUp('boss'))
    await socket.next((f) => f.t === 'hello')
    const result = await server.admin('/users/rank', { name: 'boss', rank: 'owner' })
    expect(result.body).toMatchObject({ rank: 'owner', reconnected: true })
    expect((await socket.closed).code).toBe(4001)
    expect((await server.admin('/users/rank', { name: 'nobody', rank: 'owner' })).status).toBe(404)
    expect((await server.admin('/users/rank', { name: 'boss', rank: 'king' })).status).toBe(400)
  })
})

describe('migration', () => {
  it('adds the rank column to a database made before it existed', () => {
    const directory = mkdtempSync(join(tmpdir(), 'medicon-rank-'))
    const path = join(directory, 'old.db')
    const old = new Database(path)
    old.exec(`CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE COLLATE NOCASE,
      role TEXT NOT NULL DEFAULT 'user', banned INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL)`)
    old.prepare('INSERT INTO users (name, created_at) VALUES (?, ?)').run('veteran', 1)
    old.close()
    const db = openDb(path)
    expect(db.prepare('SELECT rank FROM users WHERE name = ?').get('veteran')).toEqual({ rank: 'user' })
    db.close()
    rmSync(directory, { recursive: true, force: true })
  })
})

describe('news.post', () => {
  it('lets owners post News and Change Log to everyone', async () => {
    server = await startServer()
    await server.createLicense('boss')
    await server.admin('/users/rank', { name: 'boss', rank: 'owner' })
    const boss = await connect(server.wsUrl, await server.signUp('boss'))
    const user = await connect(server.wsUrl, await server.signUp('fan'))
    await boss.next((f) => f.t === 'hello')
    await user.next((f) => f.t === 'hello')
    boss.send({ t: 'news.post', id: 'p', kind: 'changelog', title: ' v1.2 ', body: '+ Added Killaura\r\n- Removed antistaff\r\n* Fixed auth', version: '1.2.0' })
    expect(await boss.next(ack('p'))).toEqual({ t: 'ack', id: 'p', ok: true })
    const posted = await user.next((f) => f.t === 'news.new')
    expect(posted.item).toMatchObject({ kind: 'changelog', title: 'v1.2', version: '1.2.0', body: '+ Added Killaura\n- Removed antistaff\n* Fixed auth' })
    expect(await boss.next((f) => f.t === 'news.new')).toMatchObject({ item: { id: posted.item.id } })
    boss.send({ t: 'news.post', id: 'n', kind: 'news', title: 'Hello', body: 'Body', version: '' })
    expect(await boss.next(ack('n'))).toMatchObject({ ok: true })
    expect((await user.next((f) => f.t === 'news.new')).item.version).toBeNull()
  })

  it('refuses users and empty posts', async () => {
    server = await startServer()
    const user = await connect(server.wsUrl, await server.signUp('fan'))
    await user.next((f) => f.t === 'hello')
    user.send({ t: 'news.post', id: 'p', kind: 'news', title: 'x', body: 'y' })
    expect(await user.next(ack('p'))).toEqual({ t: 'ack', id: 'p', ok: false, error: 'forbidden' })
    await server.admin('/users/rank', { name: 'fan', rank: 'owner' })
    const owner = await connect(server.wsUrl, await server.signUp('fan'))
    await owner.next((f) => f.t === 'hello')
    owner.send({ t: 'news.post', id: 'e', kind: 'news', title: '  ', body: 'y' })
    expect(await owner.next(ack('e'))).toMatchObject({ ok: false, error: 'bad_request' })
  })
})
