// End-to-end check of Social phases 4-6 (Global, friends, DM) against a
// throwaway social-server, with the Preview as one user and this script as the
// others:
//
//   social-server\.tools\node-v24.22.1-win-x64\node.exe tools\social-chat-e2e.mjs [--preview <MediconPreview.exe>] [--out <dir>]
//
// The Preview must be a build configured with
// -DMEDICON_SOCIAL_SERVER_URL=http://127.0.0.1:18787 (default: out/build/e2e).
//
// Phase 4: the DLL user reads 60 older Global messages by scrolling, sends
//   Japanese that "bob" receives, receives bob's, and sees the 0.3 s limit.
// Phase 5: bob's request notifies and is accepted on the Friends screen; a
//   request crossing one from "cat" makes friends at once.
// Phase 6: bob's DMs show an unread badge and a notification; opening the DM
//   reads them (bob gets dm.read); bob reading the reply shows "Read"; bob's
//   next DM while it is open notifies nothing; "eve" (not a friend) is refused.
// Keys and message bodies are never printed.
import { execFileSync, spawn } from 'node:child_process'
import { randomBytes } from 'node:crypto'
import { existsSync, mkdirSync, rmSync, writeFileSync } from 'node:fs'
import { createRequire } from 'node:module'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const serverRoot = join(repo, 'social-server')
const require = createRequire(join(serverRoot, 'package.json'))
const { WebSocket } = require('ws')
const Database = require('better-sqlite3')

const args = process.argv.slice(2)
const option = (name, fallback) => {
  const index = args.indexOf(name)
  return index >= 0 && index + 1 < args.length ? args[index + 1] : fallback
}
const preview = resolve(option('--preview', join(repo, 'out', 'build', 'e2e', 'bin', 'MediconPreview.exe')))
const work = join(tmpdir(), `medicon-social-chat-e2e-${process.pid}`)
const out = resolve(option('--out', join(work, 'shots')))
const socialDir = join(work, 'social')
rmSync(work, { recursive: true, force: true })
mkdirSync(socialDir, { recursive: true })
mkdirSync(out, { recursive: true })

const node = process.execPath
const base = 'http://127.0.0.1:18787'
const env = {
  ...process.env,
  PORT: '18787',
  ADMIN_PORT: '18788',
  ADMIN_SECRET: randomBytes(18).toString('hex'),
  DB_PATH: join(work, 'social.db'),
  BACKUP_DIR: join(work, 'backups'),
}
const secret = `body-${randomBytes(6).toString('hex')}` // must never appear in a log
let failures = 0
const keys = {}
const logs = []

function check(label, ok, detail = '') {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${label}${detail ? `  ${detail}` : ''}`)
  if (!ok) failures++
}

function start(command, commandArgs, options) {
  const child = spawn(command, commandArgs, { ...options, stdio: ['ignore', 'pipe', 'pipe'] })
  const proc = { child, text: '', exited: null }
  logs.push(proc)
  child.stdout.on('data', (d) => (proc.text += d))
  child.stderr.on('data', (d) => (proc.text += d))
  child.on('exit', (code) => (proc.exited = code))
  return proc
}

const masked = (text) => Object.values(keys).reduce((t, key) => t.split(key).join('MDC-****'), text)

function waitFor(proc, pattern, ms = 90000) {
  return new Promise((resolveWait, reject) => {
    const started = Date.now()
    const timer = setInterval(() => {
      const match = proc.text.match(pattern)
      if (match) {
        clearInterval(timer)
        resolveWait(match)
      } else if (proc.exited !== null || Date.now() - started > ms) {
        clearInterval(timer)
        reject(new Error(`waiting for ${pattern}: ${proc.exited !== null ? `exited ${proc.exited}` : 'timed out'}\n${masked(proc.text)}`))
      }
    }, 50)
  })
}

const admin = (...a) => execFileSync(node, ['admin/cli.ts', ...a], { cwd: serverRoot, env, encoding: 'utf8' })

let server = null
async function startServer() {
  server = start(node, ['src/index.ts'], { cwd: serverRoot, env })
  await waitFor(server, /listening on http:\/\/127\.0\.0\.1:18787/, 15000)
}
async function stopServer() {
  if (!server || server.exited !== null) return
  server.child.kill()
  await new Promise((r) => (server.exited !== null ? r() : server.child.on('exit', r)))
}

// A scripted user on its own WebSocket.
async function connectAs(name) {
  const login = await fetch(`${base}/auth/license`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'CF-Connecting-IP': `10.0.1.${Object.keys(keys).indexOf(name) + 1}` },
    body: JSON.stringify({ key: keys[name], clientVersion: 'e2e' }),
  }).then((r) => r.json())
  const ws = new WebSocket(`${base.replace('http', 'ws')}/ws`, { headers: { Authorization: `Bearer ${login.token}` } })
  const inbox = []
  const waiters = []
  ws.on('message', (data) => {
    const frame = JSON.parse(data.toString())
    const i = waiters.findIndex((w) => w.test(frame))
    if (i >= 0) waiters.splice(i, 1)[0].resolve(frame)
    else inbox.push(frame)
  })
  await new Promise((r, j) => {
    ws.once('open', r)
    ws.once('error', j)
  })
  let nonce = 1
  const user = {
    id: login.user.id,
    token: login.token,
    send: (frame) => ws.send(JSON.stringify(frame)),
    request(frame) {
      const id = `${name}${nonce++}`
      ws.send(JSON.stringify({ ...frame, id }))
      return user.next((f) => f.t === 'ack' && f.id === id)
    },
    next(test, ms = 30000) {
      const i = inbox.findIndex(test)
      if (i >= 0) return Promise.resolve(inbox.splice(i, 1)[0])
      return new Promise((r, j) => {
        const waiter = { test, resolve: (f) => (clearTimeout(timer), r(f)) }
        const timer = setTimeout(() => {
          waiters.splice(waiters.indexOf(waiter), 1)
          j(new Error(`${name}: no matching frame`))
        }, ms)
        waiters.push(waiter)
      })
    },
    close: () => ws.close(),
  }
  await user.next((f) => f.t === 'hello')
  // Keep the connection alive like a client.
  const ping = setInterval(() => ws.readyState === WebSocket.OPEN && ws.send('{"t":"ping"}'), 20000)
  ws.on('close', () => clearInterval(ping))
  return user
}

const pauseFrames = (ms) => new Promise((r) => setTimeout(r, ms))

try {
  if (!existsSync(preview)) throw new Error(`MediconPreview not found: ${preview}`)
  await startServer()
  for (const name of ['dll', 'bob', 'cat', 'eve']) {
    keys[name] = admin('license:create', '--name', name).match(/MDC(-[0-9A-Z]{4}){4}/)?.[0] ?? ''
  }
  check('licenses issued', Object.values(keys).every((k) => k !== ''))
  await stopServer()
  // 60 older Global messages, written straight into the database (quicker than
  // posting them one by one under the rate limit).
  {
    const db = new Database(env.DB_PATH)
    const bobId = db.prepare('SELECT id FROM users WHERE name = ?').get('bob').id
    const insert = db.prepare('INSERT INTO global_messages (author_id, body, created_at) VALUES (?, ?, ?)')
    const t0 = Date.now() - 60 * 60000
    for (let i = 1; i <= 60; ++i) insert.run(bobId, i === 1 ? secret : `old message ${i}`, t0 + i * 60000)
    db.close()
  }
  await startServer()
  const bob = await connectAs('bob')
  const cat = await connectAs('cat')
  const eve = await connectAs('eve')

  const p = start(preview, ['--social-dir', socialDir, '--scenario', 'social_live_chat', '--size', '1280x720', '--screenshot', join(out, 'chat.png')], {
    cwd: repo,
    env: { ...process.env, MEDICON_SOCIAL_TEST_KEY: keys.dll },
  })

  // Phase 4.
  await waitFor(p, /milestone online\b/)
  const loaded = (await waitFor(p, /global messages loaded: (\d+)/))[1]
  check('scrolling up loads the older Global history', Number(loaded) === 60, `loaded=${loaded}`)
  const fromDll = await bob.next((f) => f.t === 'global.new' && f.message.author.name === 'dll')
  check('Japanese typed in the DLL reaches the other client', fromDll.message.body === 'こんにちは、DLL から')
  await waitFor(p, /milestone global_sent/)
  const sent = await bob.request({ t: 'global.send', body: 'テストクライアントから送信しました' })
  check('the other client posts Japanese', sent.ok === true)
  await waitFor(p, /milestone global_received/)
  check('and the DLL shows it', true)
  await waitFor(p, /milestone global_rate_limited/)
  check('the 0.3 s limit is shown as an error', true)

  // Phase 5.
  const asked = await bob.request({ t: 'friend.request', name: 'dll' })
  check('bob asks dll to be friends', asked.ok === true)
  await waitFor(p, /milestone friend_request_seen/)
  check('the request notifies and waits on the Friends screen', true)
  await waitFor(p, /milestone friend_accepted/)
  const accepted = await bob.next((f) => f.t === 'friend.update' && f.status === 'accepted')
  check('accepting on the Friends screen reaches bob', accepted.user.name === 'dll')
  await waitFor(p, /milestone cat_requested/)
  const crossing = await cat.request({ t: 'friend.request', name: 'dll' })
  const instant = await cat.next((f) => f.t === 'friend.update' && f.status === 'accepted')
  check('crossing requests make friends at once', crossing.ok === true && instant.user.name === 'dll')
  await waitFor(p, /milestone mutual_friends/)

  // Phase 6.
  const dllId = accepted.user.id
  await bob.request({ t: 'dm.send', to: dllId, body: 'hey' })
  await bob.request({ t: 'dm.send', to: dllId, body: secret })
  await waitFor(p, /milestone dm_unread/)
  check('two DMs show an unread badge, a notification and the menu badge', true)
  await waitFor(p, /milestone dm_opened/)
  const read = await bob.next((f) => f.t === 'dm.read' && f.by === dllId)
  check('opening the DM tells bob it was read', read.upTo > 0)
  const reply = await bob.next((f) => f.t === 'dm.new')
  check('the reply typed in the DLL reaches bob', reply.message.body === '了解！21時ね')
  bob.send({ t: 'dm.read', with: dllId, upTo: reply.message.id })
  await waitFor(p, /milestone dm_read_shown/)
  check('bob reading it shows "Read"', true)
  await bob.request({ t: 'dm.send', to: dllId, body: 'see you' })
  await waitFor(p, /milestone dm_no_toast/)
  check('a DM from the open conversation does not notify', true)
  const refused = await eve.request({ t: 'dm.send', to: dllId, body: 'hi' })
  check('a DM from a non-friend is refused', refused.ok === false && refused.error === 'not_friends')
  const history = await fetch(`${base}/dm/${dllId}/messages`, { headers: { Authorization: `Bearer ${eve.token}` } })
  check('and so is their history', history.status === 403)

  await new Promise((r) => (p.exited !== null ? r() : p.child.on('exit', r)))
  check('the Preview run finished', p.exited === 0, `exit=${p.exited}`)
  for (const user of [bob, cat, eve]) user.close()
  await pauseFrames(200)
} catch (err) {
  failures++
  console.log(`FAIL  ${masked(err.message)}`)
  // The Preview's last lines: milestones and Social state changes only.
  const last = logs.at(-1)
  if (last && last !== server) console.log(masked(last.text.split('\n').slice(-12).join('\n')))
} finally {
  await stopServer().catch(() => {})
  for (const proc of logs) if (proc.exited === null) proc.child.kill()
}

const everything = logs.map((proc) => proc.text).join('\n')
check('no log line holds a key', Object.values(keys).every((key) => key && !everything.includes(key)))
check('no log line holds a message body', !everything.includes(secret) && !everything.includes('テストクライアント'))
console.log(`screenshots: ${out}`)
console.log(failures === 0 ? '\nALL PASSED' : `\n${failures} FAILED`)
process.exit(failures === 0 ? 0 : 1)
