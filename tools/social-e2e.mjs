// End-to-end check of Social phase 3 against a throwaway social-server.
//
//   social-server\.tools\node-v24.22.1-win-x64\node.exe tools\social-e2e.mjs [--preview <MediconPreview.exe>] [--out <dir>]
//
// Use a separate Preview build configured with
// -DMEDICON_SOCIAL_SERVER_URL=http://127.0.0.1:18787; social.json never sets the URL.
//
// Run A (scenario social_live_login): sign in through the Social screen while the
// server is down, with an unknown key, then with the real one; read Home (25 news
// items over two pages, Change Log), turn Sound off, sign out and back in.
// Run B (scenario social_live): sign in again from license.bin alone, receive a
// notification, survive a server restart, and get revoked.
//
// The license key is passed to the Preview in MEDICON_SOCIAL_TEST_KEY and is
// never printed. Nothing here touches %APPDATA%\Medicon.
import { execFileSync, spawn } from 'node:child_process'
import { randomBytes } from 'node:crypto'
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const serverRoot = join(repo, 'social-server')
const args = process.argv.slice(2)
const option = (name, fallback) => {
  const index = args.indexOf(name)
  return index >= 0 && index + 1 < args.length ? args[index + 1] : fallback
}
const preview = resolve(option('--preview', join(repo, 'out', 'build', 'local', 'bin', 'MediconPreview.exe')))
const work = join(tmpdir(), `medicon-social-e2e-${process.pid}`)
const out = resolve(option('--out', join(work, 'shots')))
const socialDir = join(work, 'social')
rmSync(work, { recursive: true, force: true })
mkdirSync(socialDir, { recursive: true })
mkdirSync(out, { recursive: true })
writeFileSync(join(socialDir, 'social.json'), '{}\n')

const node = process.execPath
const env = {
  ...process.env,
  PORT: '18787',
  ADMIN_PORT: '18788',
  ADMIN_SECRET: randomBytes(18).toString('hex'),
  DB_PATH: join(work, 'social.db'),
  BACKUP_DIR: join(work, 'backups'),
}
// A body that must never appear in a log.
const secretBody = `e2e-body-${randomBytes(6).toString('hex')}`
let failures = 0
let key = ''
const logs = []

function check(label, ok, detail = '') {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${label}${detail ? `  ${detail}` : ''}`)
  if (!ok) failures++
}

function start(command, commandArgs, options) {
  const child = spawn(command, commandArgs, { ...options, stdio: ['ignore', 'pipe', 'pipe'] })
  const proc = { child, text: '', exited: null, seen: 0 }
  logs.push(proc)
  child.stdout.on('data', (d) => (proc.text += d))
  child.stderr.on('data', (d) => (proc.text += d))
  child.on('exit', (code) => (proc.exited = code))
  return proc
}

function waitFor(proc, pattern, ms = 60000) {
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

function waitExit(proc, ms = 120000) {
  return new Promise((resolveExit, reject) => {
    if (proc.exited !== null) return resolveExit(proc.exited)
    const timer = setTimeout(() => reject(new Error(`did not exit\n${masked(proc.text)}`)), ms)
    proc.child.on('exit', (code) => {
      clearTimeout(timer)
      resolveExit(code)
    })
  })
}

const masked = (text) => (key ? text.split(key).join('MDC-****') : text)
const admin = (...adminArgs) => execFileSync(node, ['admin/cli.ts', ...adminArgs], { cwd: serverRoot, env, encoding: 'utf8' })

let server = null
async function startServer() {
  server = start(node, ['src/index.ts'], { cwd: serverRoot, env })
  await waitFor(server, /listening on http:\/\/127\.0\.0\.1:18787/, 15000)
}
async function stopServer() {
  if (!server || server.exited !== null) return
  server.child.kill()
  await waitExit(server, 10000)
}

function runPreview(scenario, shots) {
  return start(preview, ['--social-dir', socialDir, '--scenario', scenario, '--size', '1280x720', '--screenshot', join(out, shots)], {
    cwd: repo,
    env: { ...process.env, MEDICON_SOCIAL_TEST_KEY: key },
  })
}

try {
  if (!existsSync(preview)) throw new Error(`MediconPreview not found: ${preview}`)
  await startServer()
  key = admin('license:create', '--name', 'e2e').match(/MDC(-[0-9A-Z]{4}){4}/)?.[0] ?? ''
  check('license:create issued a key', key !== '')
  const body = join(work, 'body.txt')
  writeFileSync(body, `${secretBody}\nSecond line.\n`)
  for (let i = 1; i <= 25; ++i) admin('news:post', '--kind', 'news', '--title', `News ${String(i).padStart(2, '0')}`, '--body-file', body)
  admin('news:post', '--kind', 'changelog', '--title', 'Social screen', '--body-file', body, '--version', '0.3.0')
  admin('news:post', '--kind', 'changelog', '--title', 'Notifications', '--body-file', body, '--version', '0.3.1')
  await stopServer()

  // --- Run A: sign in through the screen --------------------------------------
  const a = runPreview('social_live_login', 'login.png')
  await waitFor(a, /milestone login_unreachable/)
  check('server down: "Cannot reach the server"', true)
  await startServer()
  await waitFor(a, /milestone login_invalid_key/)
  check('unknown key: "That license key is not valid"', true)
  await waitFor(a, /milestone online\b/)
  const sealed = readFileSync(join(socialDir, 'license.bin'))
  check('license.bin written after signing in', sealed.length > 0)
  check('license.bin does not hold the key in plain text', !sealed.includes(Buffer.from(key)) && !sealed.includes(Buffer.from(key, 'utf16le')))
  await waitFor(a, /milestone home_news/)
  await waitFor(a, /milestone news_all/)
  const loaded = (await waitFor(a, /news loaded: (\d+)/))[1]
  check('scrolling loads past the first 20 news items', loaded === '25', `loaded=${loaded}`)
  await waitFor(a, /milestone home_changelog/)
  check('Home shows News and Change Log, newest first', true)
  await waitFor(a, /milestone sound_off/)
  await waitFor(a, /milestone logged_out/)
  check('Sign out deletes license.bin', !existsSync(join(socialDir, 'license.bin')))
  await waitFor(a, /milestone online_again/)
  const codeA = await waitExit(a)
  check('run A finished', codeA === 0, `exit=${codeA}`)
  const settings = JSON.parse(readFileSync(join(socialDir, 'social.json'), 'utf8'))
  check('social.json does not store the server URL', !Object.hasOwn(settings, 'serverUrl'))
  check('the Sound toggle was saved to social.json', settings.notify?.sound === false, JSON.stringify(settings.notify))
  check('license.bin is back after signing in again', existsSync(join(socialDir, 'license.bin')))

  // --- Run B: sign in from license.bin, notification, restart, revoke ----------
  const b = runPreview('social_live', 'live.png')
  await waitFor(b, /milestone online\b/)
  check('a restart signs in from license.bin alone', true)
  admin('news:post', '--kind', 'news', '--title', 'Live notification', '--body-file', body)
  await waitFor(b, /milestone toast\b/)
  check('a news:post shows a notification', true)
  await waitFor(b, /milestone toast_gone/)
  await stopServer()
  await waitFor(b, /milestone offline/)
  await startServer()
  await waitFor(b, /milestone reconnected/)
  check('reconnects after a server restart', true)
  admin('license:revoke', '--name', 'e2e')
  await waitFor(b, /milestone revoked_toast/)
  check('revoke: "This license key has been revoked"', true)
  const codeB = await waitExit(b)
  check('run B finished', codeB === 0, `exit=${codeB}`)
} catch (err) {
  failures++
  console.log(`FAIL  ${masked(err.message)}`)
} finally {
  await stopServer().catch(() => {})
  for (const proc of logs) if (proc.exited === null) proc.child.kill()
}

const everything = logs.map((proc) => proc.text).join('\n')
check('no log line holds the key', key !== '' && !everything.includes(key))
check('no log line holds a news body', !everything.includes(secretBody))
console.log(`screenshots: ${out}`)
console.log(failures === 0 ? '\nALL PASSED' : `\n${failures} FAILED`)
process.exit(failures === 0 ? 0 : 1)
